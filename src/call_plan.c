/* call_plan.c -- the user method a call binds, resolved from the tables
   (see call_plan.h). */

#include <assert.h>
#include <limits.h>
#include "codegen_internal.h"
#include "codegen_poly.h"
#include "analyze_internal.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "repr.h"
#include "codegen_call_arms.h"
#include "implicit_calls.h"
#include "implied_calls.h"
#include "method_table_check.h"

static int cplan_is_root_class(Compiler *c, int ci);
static int cplan_in_closure(Compiler *c, int id);

/* The plans kept per node where the node is read as itself
   (Compiler.cplan_memo), and the storage the plans handed out point into.
   A table's plan[id], where have[id], was resolved under name[id] (a copy:
   a rename frees the node's string). A node codegen renames for a re-entry
   (Array#slice emitted as #[]) is resolved fresh, and not kept, while its
   name is not the memo's. */
struct CplanTable { CallPlan *plan; unsigned char *have; char **name; int cap; };
struct CplanMemo {
  struct CplanTable user, refuse;   /* cplan_user's, cplan_refuse's */
  PolyPlan *poly;                   /* cplan_poly_level's, where poly_name[id]: */
  char **poly_name;                 /* kept while its ntype and edits are the node's */
  int poly_cap;
  /* the arm arrays and refusal messages the plans handed out point into,
     freed with the Compiler: no ask frees or reuses one an answer holds */
  void **kept;
  int nkept, ckept;
  /* cplan_rtr_lists's entries, one per class and the Object reopening's (slot cls + 1), kept under the key the tables
     had when they were built; loose is the entry of an ask made where the tables may move */
  CplanRtrLists *rtr, loose;
  int rtr_n;
  /* cplan_omit_candidates's names, built once under the same key (omit_ok) */
  CplanOmitCand *omit;
  int nomit;
  unsigned char omit_ok;
  struct { const Compiler *c; int ncls, nscp, nnat; unsigned sgen, shape, tgen, vgen; } rtr_key;
};

static void cplan_oom(void) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
static struct CplanMemo *cplan_memo(Compiler *c) {
  if (!c->cplan_memo && !(c->cplan_memo = calloc(1, sizeof *c->cplan_memo))) cplan_oom();
  return c->cplan_memo;
}
/* p, kept until the Compiler is freed */
static void *cplan_keep(Compiler *c, void *p) {
  struct CplanMemo *m = cplan_memo(c);
  if (!p) cplan_oom();
  if (m->nkept == m->ckept) {
    m->ckept = m->ckept ? m->ckept * 2 : 64;
    if (!(m->kept = realloc(m->kept, (size_t)m->ckept * sizeof *m->kept))) cplan_oom();
  }
  return m->kept[m->nkept++] = p;
}
/* t's entry for node id: plan p, resolved under name nm */
static void cplan_table_put(struct CplanTable *t, int id, CallPlan p, const char *nm) {
  if (id >= t->cap) {
    int ncap = t->cap ? t->cap : 1024;
    while (ncap <= id) ncap *= 2;
    if (!(t->plan = realloc(t->plan, (size_t)ncap * sizeof *t->plan)) || !(t->have = realloc(t->have, (size_t)ncap)) ||
        !(t->name = realloc(t->name, (size_t)ncap * sizeof *t->name))) cplan_oom();
    memset(t->have + t->cap, 0, (size_t)(ncap - t->cap));
    memset(t->name + t->cap, 0, (size_t)(ncap - t->cap) * sizeof *t->name);
    t->cap = ncap;
  }
  t->plan[id] = p;
  free(t->name[id]);
  t->name[id] = nm ? strdup(nm) : NULL;
  if (nm && !t->name[id]) cplan_oom();
  t->have[id] = 1;
}
/* t's plan for node id read under name nm, or NULL */
static const CallPlan *cplan_table_get(const struct CplanTable *t, int id, const char *nm, int *renamed) {
  if (id >= t->cap || !t->have[id]) return NULL;
  const char *mn = t->name[id];
  if (mn ? nm && sp_streq(mn, nm) : !nm) return &t->plan[id];
  *renamed = 1;
  return NULL;
}
static void cplan_table_free(struct CplanTable *t) {
  for (int i = 0; i < t->cap; i++) free(t->name[i]);
  free(t->plan); free(t->have); free(t->name);
}
static void cplan_rtr_free(CplanRtrLists *e) {
  free(e->pub); free(e->priv); free((void *)e->undef_priv); free((void *)e->held_priv);
  *e = (CplanRtrLists){0};
}
static void cplan_omit_free(struct CplanMemo *m) {
  for (int i = 0; i < m->nomit; i++) free(m->omit[i].name);
  free(m->omit);
  m->omit = NULL; m->nomit = 0; m->omit_ok = 0;
}
/* The lists and the omission candidates are kept while the tables they read are as they were built (the key); 1 where
   the tables are fixed (the scope index is frozen), else 0 and nothing is kept */
static int cplan_rtr_sync(Compiler *c, struct CplanMemo *m) {
  if (!(comp_scope_index_is_frozen() && !comp_scope_moving())) return 0;
  if (m->rtr_key.c != c || m->rtr_key.ncls != c->nclasses || m->rtr_key.nscp != c->nscopes ||
      m->rtr_key.nnat != c->n_native_methods || m->rtr_key.sgen != comp_scope_index_gen() ||
      m->rtr_key.shape != comp_class_shape_gen() || m->rtr_key.tgen != comp_table_gen || m->rtr_key.vgen != comp_vis_gen) {
    for (int i = 0; i < m->rtr_n; i++) cplan_rtr_free(&m->rtr[i]);
    free(m->rtr);
    cplan_omit_free(m);
    m->rtr_n = c->nclasses + 1;
    if (!(m->rtr = calloc((size_t)m->rtr_n, sizeof *m->rtr))) cplan_oom();
    m->rtr_key.c = c; m->rtr_key.ncls = c->nclasses; m->rtr_key.nscp = c->nscopes; m->rtr_key.nnat = c->n_native_methods;
    m->rtr_key.sgen = comp_scope_index_gen(); m->rtr_key.shape = comp_class_shape_gen();
    m->rtr_key.tgen = comp_table_gen; m->rtr_key.vgen = comp_vis_gen;
  }
  return 1;
}
/* See call_plan.h. */
CplanRtrLists *cplan_rtr_lists(Compiler *c, int cls) {
  struct CplanMemo *m = cplan_memo(c);
  if (!cplan_rtr_sync(c, m)) {
    cplan_rtr_free(&m->loose);
    return &m->loose;
  }
  return &m->rtr[cls + 1];
}
void cplan_memo_free(Compiler *c) {
  struct CplanMemo *m = c->cplan_memo;
  if (!m) return;
  for (int i = 0; i < m->rtr_n; i++) cplan_rtr_free(&m->rtr[i]);
  free(m->rtr);
  cplan_omit_free(m);
  cplan_rtr_free(&m->loose);
  cplan_table_free(&m->user);
  cplan_table_free(&m->refuse);
  for (int i = 0; i < m->poly_cap; i++) free(m->poly_name[i]);
  free(m->poly); free(m->poly_name);
  for (int i = 0; i < m->nkept; i++) free(m->kept[i]);
  free(m->kept);
  free(m);
  c->cplan_memo = NULL;
}

/* a descendant of cid with its own `name`: the dispatch is a switch */
int cplan_overridden(Compiler *c, int cid, const char *name, int cmeth) {
  return comp_method_overridden(c, cid, name, cmeth);
}

int cplan_dispatch_form(Compiler *c, int call, int cid, const char *name, int has_base) {
  if (cid < 0 || !name) return has_base ? CP_DIRECT : CP_NONE;
  int n = dispatch_impl_count(c, call, cid, name);
  if (!(n > 1 || (!has_base && n >= 1))) return has_base ? CP_DIRECT : CP_NONE;
  return dispatch_arms_disagree(c, call, cid, name) || (!has_base && dispatch_count_misfits(c, call, cid, name))
         ? CP_PER_ARM : CP_SWITCH;
}

/* Is call id inside a block or a lambda of its method? */
static int call_in_block(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const int *par = comp_parent_map(c);
  for (int a = par[id]; a >= 0; a = par[a]) {
    NodeKind k = nt_kind(nt, a);
    if (k == NK_DefNode) return 0;
    if (k == NK_BlockNode || k == NK_LambdaNode) return 1;
  }
  return 0;
}
int cplan_self_root_override(Compiler *c, int id, int scls, int root_mi) {
  const NodeTable *nt = c->nt;
  if (id < 0 || scls < 0 || scls >= c->nclasses || root_mi < 0 || root_mi >= c->nscopes) return 0;
  int recv = nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "receiver") : -1;
  if (nt_kind(nt, id) != NK_CallNode || (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) || nt_ref(nt, id, "block") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || cplan_builtin_call(c, id) || nt_int(nt, id, "kernel_recv", 0)) return 0;
  Scope *self = comp_scope_of(c, id);
  if (!self || self->is_cmethod || self->class_id != scls || ie_class_of(c, id) >= 0) return 0;
  ClassInfo *ci = &c->classes[scls];
  if (comp_class_is_module(c, ci) || ci->is_native_class) return 0;
  /* (a call in a block or a lambda takes the switch too: the block then
     captures self, proc_body_uses_self) */
  Scope *rm = &c->scopes[root_mi];
  int oci = comp_core_class(c, CORE_OBJECT);
  if (rm->yields || rm->is_cmethod || (rm->class_id >= 0 && rm->class_id != oci) || scls == oci) return 0;
  /* the chain of scls itself answers first */
  MethodLookup oml = { .def_class = -1 };
  int own = call_target(c, id, scls, name, 0, &oml);
  if ((own >= 0 && own != root_mi) || oml.kind == ML_ATTR) return 0;
  int nd = 0, found = 0;
  const int *ds = comp_descendants(c, scls, &nd);
  for (int i = 0; i < nd; i++) {
    int k = ds[i];
    if (k == scls) continue;
    if (call_attr_target(c, -1, k, name, CT_ATTR, NULL)) return 0;
    int kmi = call_target(c, id, k, name, 0, NULL);
    if (kmi < 0 || kmi == root_mi) continue;
    if (c->scopes[kmi].yields) return 0;
    /* (an arm whose super reaches a top-level def keeps the old path in a
       block: that super is not resolved yet, and the plain call raises
       NoMethodError for it already) */
    if (rm->class_id < 0 && scope_calls_super(c, kmi) && call_in_block(c, id)) return 0;
    found = 1;
  }
  return found;
}

int cplan_root_switch(Compiler *c, int id, int scls) {
  const CallPlan p = cplan_user(c, id);
  if (p.mi < 0 || p.via != UC_INST || !p.chain || p.owner_ci != scls || scls < 0) return 0;
  int rc = c->scopes[p.mi].class_id, oci = comp_core_class(c, CORE_OBJECT);
  return rc < 0 || (rc == oci && scls != oci);
}

/* See call_plan.h. */
int cplan_builtin_call(const Compiler *c, int id) {
  return nt_int(c->nt, id, "builtin_only", 0) != 0;
}

/* See call_plan.h. */
int cplan_main_text_call(const Compiler *c, int id) {
  return nt_int(c->nt, id, "main_sg", 0) != 0;
}

/* See call_plan.h. */
int cplan_main_singleton_call(Compiler *c, int id) {
  const char *nm = nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  int cap = is_bound_method_maker(nm);
  MethodLookup ml = { .def_class = -1 };
  if (cap) nm = method_sym_arg(c, id);
  return nm && main_singleton_method(nm) && call_target(c, id, -1, nm, CT_TOP | (cap ? CT_CAPTURE : 0), &ml) < 0 &&
         ml.kind == ML_SINGLETON;
}

/* See call_plan.h. */
int comp_call_reaches_own_any(Compiler *c, int id, const char *name) {
  return !cplan_builtin_call(c, id) && cplan_core_method(c, id, CORE_OBJECT, name) >= 0;
}
/* See call_plan.h. */
int cplan_core_method(Compiler *c, int id, int core, const char *name) {
  int k = comp_core_class(c, core);
  return k >= 0 ? call_target(c, id, k, name, 0, NULL) : -1;
}
/* See call_plan.h. */
int comp_call_reaches_own(Compiler *c, int id, int class_id, const char *name) {
  if (cplan_builtin_call(c, id)) return 0;
  return call_target(c, id, class_id, name, 0, NULL) >= 0 || cplan_core_method(c, id, CORE_OBJECT, name) >= 0;
}

static void cplan_set(CallPlan *p, int mi, int owner, int via, int dispatch) {
  p->method_node = -1;
  p->method_call = 0;
  p->send_fallback = -1;
  p->delegate = NULL;
  p->delegate_undef = 0;
  p->delegate_cut = BCUT_NONE;
  p->mi = mi; p->owner_ci = (short)owner;
  p->via = (unsigned char)via; p->dispatch = (unsigned char)dispatch;
  p->by_name = 0;
  p->chain = 0;
  p->absent = 0;
  p->closed = 0; p->closed_cls = -1;
  p->rkind = CR_NONE; p->msg = NULL; p->rfrom = CRF_NONE;
  p->bowner = NULL;
  p->answers_receiver = 0;
  p->vis_cell = 0;
}

/* The core id of the class a builtin receiver kind is reopened as, or -1. */
static int cplan_reopen_class(TyKind rt) {
  switch (rt) {
  case TY_STRING: case TY_STRBUF: case TY_INT:
  case TY_FLOAT: case TY_SYMBOL: case TY_RANGE:
  case TY_TIME: case TY_THREAD: case TY_FIBER:
  case TY_ENUMERATOR: case TY_RANDOM: case TY_CLASS:
    return core_of_type(rt);
  default:        return -1;
  }
}

/* The methods a TrueClass and a FalseClass reopen define of a call's name,
   each -1 where that class has none of its own. A true or false receiver
   reaches its own class's method; the other value keeps the builtin (or
   CRuby's NoMethodError). 1 when either class has one. */
int cplan_bool_sides(Compiler *c, int id, int *tmi, int *fmi) {
  *tmi = *fmi = -1;
  const char *name = nt_str(c->nt, id, "name");
  if (!name || cplan_builtin_call(c, id)) return 0;
  static const int CN[2] = { CORE_TRUECLASS, CORE_FALSECLASS };
  for (int k = 0; k < 2; k++) {
    int ci = comp_core_class(c, CN[k]);
    int mi = ci >= 0 ? call_target(c, id, ci, name, 0, NULL) : -1;
    if (mi >= 0 && c->scopes[mi].class_id == ci) *(k ? fmi : tmi) = mi;
  }
  return *tmi >= 0 || *fmi >= 0;
}

/* The builtin class an instance of program class cid is, by its chain's
   builtin root ("Object" for a plain one); NULL for a BasicObject's, which
   no Object method reaches. */
static const char *cplan_builtin_name_of(Compiler *c, int cid) {
  const BuiltinClass *r = NULL;
  if (cid == comp_core_class(c, CORE_BASIC_OBJECT) || class_is_blank_slate(c, cid)) return NULL;
  int bp = class_builtin_parent(c, cid);
  for (int i = 0; (r = builtin_class_at(i)) && r->id != bp; i++) {}
  return r ? r->name : "Object";
}

/* Does a class value (a call's receiver recv, typed Class) answer `name`
   with a method of CRuby's before Object's instance method: its class's
   singleton chain (File.basename), or Class's and Module's own? A constant
   names the class; a class held in a variable could be any builtin one. A
   constant of a builtin's name is the builtin, a program's reopening of it
   included (`class File; end`, or one in Object's body), unless it names a
   class of the program's own (a nested `Foo::File`), which only Class's and
   Module's methods precede. */
int cplan_class_value_answers(Compiler *c, int id, int recv, const char *name) {
  const NodeTable *nt = c->nt;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  int named = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode;
  const char *bn = rk == NK_ConstantReadNode || (rk == NK_ConstantPathNode && nt_ref(nt, recv, "parent") < 0) ? key_name(c, recv) : NULL;
  int ci = named ? const_node_class_index(c, recv) : -1;
  int own = ci >= 0 && !(c->classes[ci].is_builtin_const && !comp_class_in_namespace(c, ci));
  /* a singleton reader or writer of the named class (`class << self; attr_reader`, singleton_class.attr_*, its
     ancestors' included, an alias of one resolved, one an alias shadows not): no scope the plan finds, and it
     answers ahead of the top level's def */
  char slot[256];
  if (own && cplan_sg_accessor(c, id, ci, name, slot, sizeof slot) != CP_SG_NONE) return 1;
  if (own || !bn || !(is_builtin_class_name(bn) || is_builtin_module_const_name(bn)))
    return (!bn && !own && builtin_cmethod_known_any(name)) || core_class_value_answers("Class#", name) || core_class_value_answers("Module#", name);
  /* (a module of the table with no class id, Process or GC, has its singleton methods in the arity rows) */
  int bid = builtin_class_id(bn);
  return core_class_value_answers(bn, name) || core_class_value_answers(is_builtin_module_const_name(bn) ? "Module#" : "Class#", name) ||
         (bid ? builtin_class_id_answers(bid, name, 1) : builtin_cmethod_known(bn, name));
}

/* obj.class can select a program singleton method when the class object
   carries a program class id. Exceptions instead carry their class by name. */
int cplan_class_reader_overridden(Compiler *c, TyKind type, const char *name) {
  int own = type == TY_POLY || (ty_is_object(type) && !class_is_exc_subclass(c, ty_object_class(type))) ||
            ((type == TY_STRING || type == TY_STRBUF) && program_has_strsub(c));
  int count = 0;
  if (own) comp_cmethod_candidates(c, name, &count);
  return count != 0;
}

/* Does call id enforce visibility: public_send, or a call with an explicit
   receiver that is not self (`send`, a call on self and one that skips
   visibility run the method)? */
static int cplan_call_enforces_vis(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "receiver") : -1;
  if (recv < 0) return 0;
  return nt_str(nt, id, "vis_enforce") ||
         !(nt_ignore_visibility(nt, id) || nt_str(nt, id, "send_blind") || nt_int(nt, id, "dyn_arm", 0) ||
           nt_kind(nt, recv) == NK_SelfNode || nt_int(nt, id, "self_call", 0));
}
static int object_protected_open(Compiler *c, int id);
/* The flags an enforcing call id asks with (CT_PUBLIC, and CT_PROTECTED where
   a protected owner is closed to it too), 0 for a call that enforces
   nothing */
static unsigned ct_enforce_flags(Compiler *c, int id) {
  if (!cplan_call_enforces_vis(c, id)) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  return CT_PUBLIC | (nt_str(c->nt, id, "vis_enforce") || (nm && is_public_send(nm)) || !object_protected_open(c, id) ? CT_PROTECTED : 0);
}
/* See call_plan.h. A call with a block binds no entry (a yielding method is
   called through the proc form of one version), so it asks with none. */
unsigned ct_vis_flags(Compiler *c, int id, const char *name, unsigned flags) {
  /* (what each flag does to the face: CT_PUBLIC and CT_PROTECTED are the
     face itself; CT_NOFACE says the ask has none; CT_OWN, CT_TOP,
     CT_REACH, CT_CAPTURE, CT_IMPLIED and CT_INCLUDED are asks of a name
     that take it as it is, whatever the call enforces; a class method's
     call, CT_CMETHOD, enforces as an instance's does, where a class
     side's face can turn on it: a name whose visibility varies
     (ct_face_for). Readers ask a class side once per class of the
     program for a call, so the others ask with none.) */
  if (flags & CT_NOFACE) return flags;
  if ((flags & ~(CT_WRITER | CT_CMETHOD)) || id < 0 || nt_ref(c->nt, id, "block") >= 0) return flags;
  if ((flags & CT_CMETHOD) && !elab_method_vis_varies(c, name)) return flags;
  return flags | ct_enforce_flags(c, id);
}

static int cplan_root_def_ran(Compiler *c, int call, int k, const char *name);
/* Is scope s a top-level def of the program (not a method of a class, a
   class method, or a version entry)? */
static int cplan_is_top_def_scope(const Compiler *c, int s) {
  return s >= 1 && s < c->nscopes && c->scopes[s].class_id < 0 && !c->scopes[s].is_cmethod && c->scopes[s].entry == ENTRY_NONE;
}
static void cplan_top_edge_add(int **edges, int *n, int *cap, int from, int to) {
  if (*n == *cap) {
    *cap = *cap ? 2 * *cap : 16;
    if (!(*edges = realloc(*edges, sizeof(int) * 2 * (size_t)*cap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  (*edges)[2 * *n] = from;
  (*edges)[2 * *n + 1] = to;
  (*n)++;
}
/* May the body of top-level def s read self: a `self`, an instance variable,
   a super, or a call with no receiver whose lookup could end at a method of
   self's own class (it is asked on self ahead of Object): a name some class
   or module of the program answers (a def, an attribute, a member), a name a
   builtin class below Object has, or a name that is neither a top-level def
   of the program nor one of Kernel's functions that never consult self
   (`inspect`, `send`, `method`, `binding`, an included module's method, a
   method_missing); or a call of a top-level def that may read self, which
   runs on the same self (a recursion included)? A yield, a block argument and
   `...` do not read it: the block's self is the caller's, and a callee the
   block or the arguments go to has its own edge. Where the answer is
   unsure, it reads self. Each node reads the scope it stands in
   (Compiler.nscope), blocks and defaults included. One pass over the nodes
   marks the scopes and lists the calls between defs, an edge to EVERY version
   of the callee's name; the marks are then carried to the callers over the
   reversed edges with one worklist, so the cost is linear in the nodes. They
   are asked again only for another tree. */
static void cplan_top_self_marks(Compiler *c) {
  const NodeTable *nt = c->nt;
  unsigned long stamp = (unsigned long)nt->count * 1000003ul ^ (unsigned long)c->nscopes * 31ul ^ (unsigned long)nt->edits ^ (unsigned long)nt->version * 7ul;
  if (c->top_self_marks && c->top_self_stamp == stamp) return;
  free(c->top_self_marks);
  if (!(c->top_self_marks = calloc((size_t)c->nscopes + 1, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->top_self_stamp = stamp;
  unsigned char *mk = c->top_self_marks;
  int *edges = NULL, ne = 0, cap = 0;
  /* the scopes of one def node (a def and the copies of it an alias makes) share its body: they are one
     scope here, the first of them */
  int *canon = malloc(sizeof(int) * ((size_t)c->nscopes + 1)), *first = malloc(sizeof(int) * ((size_t)nt->count + 1));
  if (!canon || !first) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int d = 0; d <= nt->count; d++) first[d] = 0;
  for (int s = 0; s <= c->nscopes; s++) {
    int d = c->scopes[s < c->nscopes ? s : 0].def_node;
    canon[s] = s;
    if (s >= 1 && s < c->nscopes && cplan_is_top_def_scope(c, s) && d >= 0 && d < nt->count) canon[s] = first[d] ? first[d] : (first[d] = s);
  }
  free(first);
  for (int id = 0; id < nt->count; id++) {
    int sc = id < c->node_cap ? c->nscope[id] : 0;
    if (!cplan_is_top_def_scope(c, sc)) continue;
    sc = canon[sc];
    switch (nt_kind(nt, id)) {
    case NK_SelfNode: case NK_SuperNode: case NK_ForwardingSuperNode:
    case NK_InstanceVariableReadNode: case NK_InstanceVariableWriteNode: case NK_InstanceVariableOperatorWriteNode:
    case NK_InstanceVariableOrWriteNode: case NK_InstanceVariableAndWriteNode: case NK_InstanceVariableTargetNode:
      mk[sc] = 1; break;
    case NK_CallNode: {
      const char *nm = nt_str(nt, id, "name");
      if (!nm) { mk[sc] = 1; break; }
      if (nt_ref(nt, id, "receiver") >= 0) break;
      if (comp_name_answered_any_side(c, nm) || builtin_name_below_object(nm)) { mk[sc] = 1; break; }
      int found = 0;
      for (int t = an_scopes_named_first(c, nm); t >= 0; t = an_scopes_named_next(c, nm, t))
        if (cplan_is_top_def_scope(c, t)) { found = 1; if (canon[t] != sc) cplan_top_edge_add(&edges, &ne, &cap, sc, canon[t]); }
      if (!found) {
        int tm = call_target(c, -1, -1, nm, CT_TOP, NULL);   /* (an alias of a def) */
        if (tm >= 0 && cplan_is_top_def_scope(c, tm)) { found = 1; if (canon[tm] != sc) cplan_top_edge_add(&edges, &ne, &cap, sc, canon[tm]); }
      }
      if (!found && !kernel_function_ignores_self(nm)) mk[sc] = 1;
      break;
    }
    default: break;
    }
  }
  /* the callers of a scope: a compressed list over the reversed edges */
  int *start = calloc((size_t)c->nscopes + 2, sizeof(int)), *by = malloc(sizeof(int) * ((size_t)ne + 1)), *wl = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  if (!start || !by || !wl) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int e = 0; e < ne; e++) start[edges[2 * e + 1] + 1]++;
  for (int s = 0; s <= c->nscopes; s++) start[s + 1] += start[s];
  int *fill = malloc(sizeof(int) * ((size_t)c->nscopes + 1));
  if (!fill) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memcpy(fill, start, sizeof(int) * ((size_t)c->nscopes + 1));
  for (int e = 0; e < ne; e++) by[fill[edges[2 * e + 1]]++] = edges[2 * e];
  int nw = 0;
  for (int s = 1; s < c->nscopes; s++) if (mk[s]) wl[nw++] = s;
  while (nw > 0) {   /* (a caller of a def that may read self may read it) */
    int s = wl[--nw];
    for (int i = start[s]; i < start[s + 1]; i++) if (!mk[by[i]]) { mk[by[i]] = 1; wl[nw++] = by[i]; }
  }
  for (int s = 1; s < c->nscopes; s++) mk[s] = mk[canon[s]];
  free(canon); free(fill); free(wl); free(by); free(start); free(edges);
}
/* May any version of the top-level defs named `name`, or the def an alias of that name stands for, read self? Asked before
   the lookup, so a def that stays private to a receiver costs nothing more. */
static int cplan_top_name_reads_self(Compiler *c, const char *name) {
  int any = 0;
  for (int t = an_scopes_named_first(c, name); t >= 0 && !any; t = an_scopes_named_next(c, name, t))
    any = cplan_is_top_def_scope(c, t);
  int tm = call_target(c, -1, -1, name, CT_TOP, NULL);   /* (an alias names a def under another name) */
  int tm_top = tm >= 0 && cplan_is_top_def_scope(c, tm);
  /* (no def of the name, no marks to read: the marks are made again after each edit of the tree, and the
     supers and guards the pre-fixpoint phases look up ask between edits) */
  if (!any && !tm_top) return 0;
  cplan_top_self_marks(c);
  for (int t = an_scopes_named_first(c, name); t >= 0; t = an_scopes_named_next(c, name, t))
    if (cplan_is_top_def_scope(c, t) && c->top_self_marks[t]) return 1;
  return tm_top && c->top_self_marks[tm];
}

static void ver_vis_read_add(Compiler *c, int call, const char *name, int cell);
/* The visibility the record has for main's only version of `name` (the
   histories of one version, with no reopened Object's twin: the lone
   version's answer, which a face entry does not make): its arm's
   (elab_version_vis), a visibility where the record proves it, -1 where
   only the run tells and every visibility word of the name has a write that
   keeps the cell *cell (lr_vis_cellable); -2 for any other history. */
static int cplan_top_lone_vis(Compiler *c, const char *name, int *cell) {
  ElabVersion vs[2];
  if (elab_main_object_one_entry(c, name) || elab_history_versions(c, -1 /* main */, 0, name, vs, 2) != 1) return -2;
  if (cell) *cell = vs[0].cell;
  return elab_version_vis(c, &vs[0]);
}
/* Has main's def of `name` surely run where call `call` runs, as its one
   version, never to be undefined again? Placed in the load, the record's
   own answer, no condition between; with no place, at every state of the
   statements it may run in (cplan_root_def_ran). */
static int cplan_top_def_surely_live(Compiler *c, int call, const char *name) {
  const LrDef *d = NULL;
  LoadPoint p = elab_point_of(c, call);
  if (p.region < 0) return cplan_root_def_ran(c, call, -1, name);
  return elab_method_at(c, -1 /* main */, name, 0, p, &d) == ELAB_YES && d && !d->may;
}
/* The live version of the top-level def `name` that call `call`, which has
   an explicit receiver, runs where the root lookup finds the def open to it:
   public at the call, as the load record has it there (cplan_root_method).
   A top-level def is Object's method from where it runs, and takes no self.
   -1 for a call on self, which is the def's own call, and for a def closed
   to the call. Where the lookup cannot tell the visibility, and the def is
   main's only version, which has surely run where the call does, with a word
   that only the run orders against the call, the def is bound with the
   visibility cell that guards the call (*cell, the history cell + 1:
   cplan_top_lone_vis); a call whose operands have an effect is not (the
   guard's two arms would each run them: the receiver is held first), nor is
   one with a block. The version is the call's
   (the table's last one is another def where the program defines the name
   again), asked after the lookup, which makes no version demand. */
static int cplan_root_top_def(Compiler *c, int call, const char *name, const char *cls, int *cell) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, call, "receiver");
  if (cell) *cell = 0;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode || call_target(c, -1, -1, name, CT_TOP, NULL) < 0) return -1;
  /* a def that may read self stays private to a receiver (master's answer): main's self is
     not the receiver's */
  if (cplan_top_name_reads_self(c, name)) return -1;
  int v = -1, rm = cplan_root_method(c, call, cls, name, &v);
  if (rm < 0 || c->scopes[rm].class_id >= 0 || v == SP_VIS_PRIVATE || v == SP_VIS_PROTECTED) return -1;
  /* (a public method of a builtin of the name answers the call, as it did
     before: the program's def of a builtin's name, which has no self but
     main, is left to it; asked last, of the few calls the lookup binds) */
  if (core_vis_of_bit(core_method_vis(cls, name, 1, NULL)) == SP_VIS_PUBLIC) return -1;
  if (v != SP_VIS_PUBLIC) {
    int cl = -1;
    if (v != -1 || !cell || cplan_top_lone_vis(c, name, &cl) != -1 || cl < 0 || !cplan_top_def_surely_live(c, call, name) ||
        nt_ref(nt, call, "block") >= 0) return -1;
    int an = 0; const int *av = call_args(nt, call, &an);
    for (int i = 0; i < an; i++) if (subtree_has_side_effect(c, av[i])) return -1;
    *cell = cl + 1;
  }
  return call_target(c, call, -1, name, CT_TOP, NULL);
}

/* A program's own method on builtin class root (CORE_OBJECT, CORE_NUMERIC,
   CORE_NILCLASS) that call `call` runs, on an instance of builtin class cls
   (a program object's: its chain's builtin root; NULL: one the root does not
   reach): for a name no ancestor of cls ahead of root owns, as CRuby's
   lookup reaches the reopening first. */
static void cplan_root_reopen(Compiler *c, int call, const char *name, const char *cls, int root, CallPlan *p) {
  if (!cls || builtin_answers_ahead_of(cls, name, core_class_name(root))) return;
  /* A call in a closure has no place in the load: before the program's def
     it reaches the builtin's method, which only a name the builtin owns
     (Kernel's tap, frozen?) could tell apart, so that call keeps the
     builtin's, as before the def. */
  if (builtin_answers_ahead_of(cls, name, NULL) && cplan_in_closure(c, call)) return;
  int rci = comp_core_class(c, root);
  int mi = rci >= 0 ? call_target(c, call, rci, name, 0, NULL) : -1;
  /* Where the builtin has the name, the program's def answers only if the
     load record has it live at the call: a def under any condition, one that
     runs or not, keeps the builtin's. */
  if (mi >= 0 && c->scopes[mi].class_id >= 0 && builtin_answers_ahead_of(cls, name, NULL) &&
      elab_method_at(c, c->scopes[mi].class_id, name, 0, elab_point_of(c, call), NULL) != ELAB_YES) return;
  /* (Object's lookup also finds its modules' methods and the top level's,
     which a Numeric's or a NilClass's must not stand for) */
  if (mi >= 0 && (root == CORE_OBJECT || c->scopes[mi].class_id == rci)) cplan_set(p, mi, rci, UC_REOPEN, CP_DIRECT);
  /* no method of Object's table: the top level's def, which the lookup finds
     open to the call, ends it */
  else if (mi < 0 && root == CORE_OBJECT) {
    int cell, tm = cplan_root_top_def(c, call, name, cls, &cell);
    if (tm >= 0) {
      cplan_set(p, tm, c->scopes[tm].class_id, UC_REOPEN, CP_DIRECT);
      p->vis_cell = cell;
      if (cell) ver_vis_read_add(c, call, name, cell - 1);
    }
  }
}

static int ct_runs_on_main(Compiler *c, int id);
/* See call_plan.h. */
int cplan_object_method_named(Compiler *c, const char *name) {
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (sc->is_cmethod || !sc->name || !sp_streq(sc->name, name)) continue;
    const char *cn = sc->class_id >= 0 && sc->class_id < c->nclasses ? c->classes[sc->class_id].name : NULL;
    if (!cn || is_object_root(cn) || sc->class_id == c->toplevel_class) return 1;
  }
  return 0;
}
/* ---- does a call's iterator throw its block's values away? ----
   A builtin iterator of the each family (is_block_value_dropped_iter) does,
   on any receiver but an Enumerator, whose each answers what its method
   does with them (`a.map.each { }`). A method of the program's the call
   enters does where it never reads one: no yield of its whose value is
   taken, no super handed its block, and its block parameter, if any, only
   handed on (`&blk`, `&`) to a call whose iterator throws them away. A
   method is asked once while nothing changes (Scope.bvi_*, numbered by
   Compiler.bvi_epoch); a method asked again while it is being asked (a
   cycle) is taken as throwing them away, an answer kept only where it rests
   on no method still being asked further out. */
enum { BVI_BUSY = 1, BVI_DROPS, BVI_READS };
static int cplan_bvi_call(Compiler *c, int call);
/* Is method s's block handed on at call n: `&bp`, or a bare `&` where s
   takes an anonymous one (bp "")? */
static int cplan_bvi_forwards(const NodeTable *nt, const char *bp, int n) {
  int b = bp && nt_kind(nt, n) == NK_CallNode ? nt_ref(nt, n, "block") : -1;
  if (b < 0 || nt_kind(nt, b) != NK_BlockArgumentNode) return 0;
  int e = nt_ref(nt, b, "expression");
  return bp[0] ? e >= 0 && nt_kind(nt, e) == NK_LocalVariableReadNode && sp_streq(nt_str(nt, e, "name"), bp) : e < 0;
}
/* Does node n of method s's body (block parameter bp) throw away every
   value of s's block it can reach? A nested def, class or module is not
   s's body; a block in it is (its nodes are its method's). */
static int cplan_bvi_body(Compiler *c, int s, const char *bp, int n) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, n);
  if (k == NK_DefNode || k == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode) return 1;
  if (k == NK_YieldNode && c->nscope[n] == s && !an_value_dropped(c, comp_parent_map(c), n)) return 0;
  if (super_forwards_caller_block(c, n)) return 0;
  if (bp && bp[0] && k == NK_LocalVariableReadNode && sp_streq(nt_str(nt, n, "name"), bp)) return 0;
  int skip = -1;
  if (cplan_bvi_forwards(nt, bp, n)) {
    if (!cplan_bvi_call(c, n)) return 0;
    skip = nt_ref(nt, n, "block");
  }
  for (int i = 0, nr = nt_num_refs(nt, n); i < nr; i++) {
    int ch = nt_ref_at(nt, n, i);
    if (ch >= 0 && ch != skip && !cplan_bvi_body(c, s, bp, ch)) return 0;
  }
  for (int i = 0, na = nt_num_arrs(nt, n); i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++)
      if (ids[j] >= 0 && !cplan_bvi_body(c, s, bp, ids[j])) return 0;
  }
  return 1;
}
/* Does program method s throw away every value its block answers? A `...`
   parameter hands the block on unseen. */
static int cplan_bvi_method(Compiler *c, int s) {
  const NodeTable *nt = c->nt;
  Scope *sc = &c->scopes[s];
  if (sc->bvi_epoch == c->bvi_epoch) {
    if (sc->bvi_state != BVI_BUSY) return sc->bvi_state == BVI_DROPS;
    if (sc->bvi_depth < c->bvi_low) c->bvi_low = sc->bvi_depth;
    return 1;
  }
  int prm = sc->def_node >= 0 && nt_kind(nt, sc->def_node) == NK_DefNode ? nt_ref(nt, sc->def_node, "parameters") : -1;
  int kwr = prm >= 0 ? nt_ref(nt, prm, "keyword_rest") : -1;
  if (kwr >= 0 && nt_kind(nt, kwr) == NK_ForwardingParameterNode) return 0;
  int low = c->bvi_low;
  sc->bvi_epoch = c->bvi_epoch;
  sc->bvi_state = BVI_BUSY;
  sc->bvi_depth = ++c->bvi_mdepth;
  c->bvi_low = INT_MAX;
  int drops = sc->body < 0 || cplan_bvi_body(c, s, sc->blk_param, sc->body);
  c->bvi_mdepth--;
  /* (an answer that took a method further out as throwing them away waits
     for that method's own) */
  if (drops && c->bvi_low < sc->bvi_depth) sc->bvi_epoch = 0;
  else sc->bvi_state = drops ? BVI_DROPS : BVI_READS;
  if (c->bvi_low >= sc->bvi_depth) c->bvi_low = low;
  else if (low < c->bvi_low) c->bvi_low = low;
  return drops;
}
/* Does every method of the program's the call may enter throw its block's
   values away, where the plan lists none: each method of the names that
   hold them (cplan_entries_bound_names) and a method_missing? */
static int cplan_bvi_named(Compiler *c, int call) {
  const char *names[3];
  int nn = cplan_entries_bound_names(c, call, names);
  names[nn++] = METHOD_MISSING_NAME;
  for (int i = 0; i < nn; i++)
    for (int s = an_scopes_named_first(c, names[i]); s >= 1; s = an_scopes_named_next(c, names[i], s))
      if (!c->scopes[s].is_proc_form && !cplan_bvi_method(c, s)) return 0;
  return 1;
}
static int cplan_bvi_call(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  const char *name = nt_kind(nt, call) == NK_CallNode ? nt_str(nt, call, "name") : NULL;
  if (!name || !is_block_value_dropped_iter(name)) return 0;
  if (cplan_builtin_call(c, call)) return 1;
  int recv = nt_ref(nt, call, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (rt == TY_ENUMERATOR) return 0;
  /* (a receiver of no type yet, before inference, has no plan to keep) */
  int tg[CPT_MAX], n = rt == TY_UNKNOWN ? CPT_UNKNOWN : cplan_entries(c, call, tg, CPT_MAX);
  if (n == CPT_UNKNOWN) return cplan_bvi_named(c, call);
  for (int i = 0; i < n; i++)
    if (tg[i] >= 1 && !cplan_bvi_method(c, tg[i])) return 0;
  return 1;
}
/* See call_plan.h. */
int cplan_block_value_ignored(Compiler *c, int call) {
  /* (the methods' answers stand while no node, type or method has changed
     since they were found, as infer_type's do, and only while the scope
     index is frozen: before, a pass may rename a scope, which moves
     nothing a stamp can read; one ask still asks each method once) */
  if (c->bvi_calls++ == 0) {
    const NodeTable *nt = c->nt;
    unsigned gen = comp_scope_index_gen();
    if (!comp_scope_index_is_frozen() || c->bvi_stamp.ty != comp_ty_edits || c->bvi_stamp.edits != nt->edits ||
        c->bvi_stamp.version != nt->version || c->bvi_stamp.nscopes != c->nscopes ||
        c->bvi_stamp.scope_gen != gen) {
      c->bvi_epoch++;
      c->bvi_stamp.ty = comp_ty_edits;
      c->bvi_stamp.edits = nt->edits;
      c->bvi_stamp.version = nt->version;
      c->bvi_stamp.nscopes = c->nscopes;
      c->bvi_stamp.scope_gen = gen;
    }
    c->bvi_low = INT_MAX;
  }
  int r = cplan_bvi_call(c, call);
  c->bvi_calls--;
  return r;
}
/* Is core slot s one of the roots a program's method on a builtin
   receiver is found on, past the builtin's own classes: NilClass, Numeric,
   Object, Kernel, BasicObject? */
static int cplan_root_slot(int s) {
  return s == CORE_NILCLASS || s == CORE_NUMERIC || s == CORE_OBJECT || s == CORE_KERNEL || s == CORE_BASIC_OBJECT;
}
/* Is class ci the program's reopening of one of those roots? */
static int cplan_is_root_class(Compiler *c, int ci) {
  int s = ci >= 0 && ci < c->nclasses && c->classes[ci].name ? builtin_core_class_slot(c->classes[ci].name) : -1;
  return s >= 0 && comp_core_class(c, s) == ci && cplan_root_slot(s);
}
/* Is plan p a top-level def bound to an explicit receiver (cplan_root_top_def)?
   It has no owner class, and no self but main's. */
static int cplan_root_reopen_top(Compiler *c, const CallPlan *p) {
  return p->mi >= 0 && p->via == UC_REOPEN && c->scopes[p->mi].class_id < 0;
}
/* See call_plan.h. */
int cplan_root_reopen_owner(Compiler *c, const CallPlan *p) {
  return p->via == UC_REOPEN && (cplan_is_root_class(c, p->owner_ci) || cplan_root_reopen_top(c, p));
}
/* See call_plan.h. */
int cplan_root_reopen_call(Compiler *c, int id, const CallPlan *p) {
  const char *name = nt_str(c->nt, id, "name");
  if (!name || cplan_builtin_call(c, id)) return 0;
  if (p->mi < 0) return 0;
  /* (a top-level def bound for an explicit receiver: Object's method from where it runs) */
  int top = cplan_root_reopen_top(c, p);
  if (!cplan_root_reopen_owner(c, p)) return 0;
  /* (p's facts are read before the asks below, which can rewrite it) */
  int mc = c->scopes[p->mi].class_id, at_object = top || p->owner_ci == comp_core_class(c, CORE_OBJECT);
  /* (a class-gated exception accessor the program adds to Object is picked
     by an exception's class at run time) */
  int recv = nt_ref(c->nt, id, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* (`nil&.m` answers nil and calls nothing: the readers' own fold of it) */
  if (rt == TY_NIL && nt_safe_nav(c->nt, id)) return 0;
  /* (an Object method beside a NilClass one of the name: a String, Integer or
     Float is answered boxed, as the analysis reads it before the nullability settles) */
  if (at_object && cplan_nil_scalar_boxed(c, id, name, recv, rt)) return 0;
  int exc = rt == TY_EXCEPTION || (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)));
  if (exc && cplan_exc_object_method(c, id, name) >= 0) return 0;
  /* A private or protected one is refused, or allowed, by the visibility
     arms, which see the receiver's class (an exception's arms refuse it
     themselves), so every reader leaves it to them where a call of the name
     would otherwise reach a builtin's method: Kernel's tap, dup, frozen?.
     One no builtin has is typed by the program's method, as before. */
  if (mc < 0) return top && cplan_root_closed(c, id, NULL) < 0;
  if (exc || builtin_answers_ahead_of("Object", name, NULL)) {
    /* (the visibility at the call, which a later `public` or `private` does not change) */
    if (cplan_method_vis(c, id, mc, name, CPV_ROOT, NULL) != SP_VIS_PUBLIC) return 0;
  }
  /* (one closed to the call binds none: it raises) */
  if (cplan_root_closed(c, id, NULL) >= 0) return 0;
  return 1;
}

/* See call_plan.h. */
int cplan_arm_object_target(Compiler *c, int call, const char *cls) {
  const char *name = nt_str(c->nt, call, "name");
  if (!name) return -1;
  if (!cls) {
    int oc = comp_core_class(c, CORE_OBJECT);
    return oc >= 0 ? call_target(c, call, oc, name, 0, NULL) : -1;
  }
  CallPlan p;
  cplan_set(&p, -1, -1, UC_NONE, CP_NONE);
  cplan_root_reopen(c, call, name, cls, CORE_OBJECT, &p);
  return p.mi;
}

/* See call_plan.h. */
int cplan_reopen_binding(CallPlan p) {
  return p.mi >= 0 && p.via == UC_REOPEN && p.dispatch == CP_DIRECT ? p.mi : -1;
}

/* See call_plan.h. */
int cplan_reopen_target(Compiler *c, CallPlan p) {
  int mi = cplan_reopen_binding(p);
  return mi >= 0 && (cplan_root_reopen_owner(c, &p) || c->scopes[mi].yields) ? mi : -1;
}

/* A method the program adds to Object under the name of a class-gated
   exception accessor (UncaughtThrowError#tag, KeyError#key, ...): every
   exception but those classes answers it, as CRuby's lookup reaches Object.
   Its scope for call `call`, or -1 (none, or one taking arguments or a
   block). */
int cplan_exc_object_method(Compiler *c, int call, const char *name) {
  if (!is_gated_exception_accessor(name)) return -1;
  int oc = comp_core_class(c, CORE_OBJECT);
  int mi = oc >= 0 ? call_target(c, call, oc, name, CT_OWN, NULL) : -1;
  if (mi < 0 || cplan_method_vis(c, call, oc, name, 0, NULL) == SP_VIS_PRIVATE ||
      c->scopes[mi].nparams != 0 || c->scopes[mi].yields) return -1;
  return mi;
}
/* the reopenings of builtin exception classes that define name: the first
   stands for them, picked among at run time by the class name (call `call`) */
static void cplan_exc_reopen(Compiler *c, int call, const char *name, CallPlan *p) {
  int xr[8];
  int xn = exc_reopen_definers(c, name, xr, 8);
  if (xn <= 0) return;
  int mi = call_target(c, call, xr[0], name, 0, NULL);
  if (mi >= 0) cplan_set(p, mi, xr[0], UC_REOPEN, xn > 1 ? CP_SWITCH : CP_DIRECT);
}

/* Does the chain of root class `top` (one with no user superclass) go on
   into a builtin other than Object: `< StandardError`, `< String`, a
   Struct.new? That builtin's methods come before an Object reopening's in
   CRuby's lookup (a Child#message super reaches Exception#message), and
   the plan sees none of them, so a super that found no user method stops
   there rather than at the reopening. */
static int cplan_root_under_builtin(Compiler *c, int top) {
  if (c->classes[top].is_struct) return 1;
  int sc = class_def_super(c->nt, c->classes[top].def_node);
  if (sc < 0) return 0;
  const char *nm = nt_kind(c->nt, sc) == NK_ConstantReadNode ? nt_str(c->nt, sc, "name") : NULL;
  return !(nm && sp_streq(nm, "Object"));
}
/* Does the program define `name` on Kernel? Its top-level history holds a
   version from a Kernel reopening (the def desugar_kernel_reopen made of it,
   marked kernel_def), whichever version is the live one. A super from a
   method of Object whose chain ends there reaches that def, which takes the
   receiver as self: the top-level def has none, so such a super is no
   builtin's answer either. */
int cplan_kernel_def(Compiler *c, const char *name) {
  return elab_main_history_marked(c, name, "kernel_def");
}
/* See call_plan.h. */
int cplan_kernel_raise(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  return nm && is_raise_alias(nm) && cplan_user(c, id).dispatch == CP_NONE;
}
/* Does the program have a top-level def of `name` that a `super` of scope s, which cplan_super_chain_mi found no
   target for, may still reach? A top-level def is Object's private method, ahead of Kernel's and of a builtin that
   does not answer the name first, for a chain that reaches Object: the lookup binds it only where one def is made
   for sure before the method's own def runs (a def made after the class, or twice, is none it proves). Where it
   binds none but a def exists, the builtin's own method is not the answer either: CRuby answers the def wherever it
   has run, Kernel's function where it has not, and which one depends on when the super runs. The super keeps the
   NoMethodError it had, rather than a Kernel function's answer that is wrong wherever the def has run. */
static int cplan_super_top_def_unproven(Compiler *c, const Scope *s, const char *name) {
  if (s->is_cmethod || s->class_id < 0 || comp_class_is_module(c, &c->classes[s->class_id])) return 0;
  int top = s->class_id;
  while (c->classes[top].parent >= 0) top = c->classes[top].parent;
  if (class_is_blank_slate(c, top) || builtin_core_class_slot(c->classes[top].name) == CORE_BASIC_OBJECT) return 0;
  const char *root = comp_super_builtin_root(c, s->class_id, NULL);
  if (!root || builtin_answers_ahead_of(root, name, "Object") || comp_chain_core_answers(c, s->class_id, name)) return 0;
  for (int t = an_scopes_named_first(c, name); t >= 0; t = an_scopes_named_next(c, name, t))
    if (cplan_is_top_def_scope(c, t)) return 1;
  int tm = call_target(c, -1, -1, name, CT_TOP, NULL);   /* (an alias names a def under another name) */
  return tm >= 0 && cplan_is_top_def_scope(c, tm);
}
/* A super's parent-chain target, past inherited copies of the same
   extended definition. Shared by resolution, inference, inline splicing and
   the miss check. Every step is call_target's, with CT_REACH: it skips the
   ask of the calls' names (ct_calls), so call_node_names, which runs
   comp_super_name for this very name, is not entered again, and it asks the
   record, a shadow's name too. ml->from is the par asked, though the
   extend-copy skip and the Object start look further; ml->def_class is the
   class that answered, -1 on a miss. */
static int ct_top_chain(Compiler *c, const char *name, int sg);
int cplan_super_chain_mi(Compiler *c, int id, const Scope *s, int par, const char *name, MethodLookup *ml) {
  MethodLookup own = { .def_class = -1 };
  if (!ml) ml = &own;
  const int start = par;
  unsigned side = CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0);
  int mi = -1;
  *ml = (MethodLookup){ .kind = ML_NONE, .def_class = -1 };
  if (par >= 0) mi = call_target(c, id, par, name, side, ml);
  /* An extended module's method is one entry in the class's ancestry, and
     `super` inside it goes past the module. Copied into each class that
     inherits the extend, the parent's copy is that same definition, not a
     superclass method: skip such copies (Thor's ClassMethods#method_added
     called on a Thor subclass reaches Module#method_added). */
  while (s->is_cmethod && mi >= 0 && c->scopes[mi].def_node == s->def_node && s->def_node >= 0) {
    par = c->classes[c->scopes[mi].class_id].parent;
    mi = par >= 0 ? call_target(c, id, par, name, side, ml) : -1;
    if (par < 0) *ml = (MethodLookup){ .kind = ML_NONE, .def_class = -1 };
  }
  /* (a superclass chain ends in Object, then Kernel and BasicObject: a
     reopening of each is looked in, in that order; a module's own chain is
     its includer's) */
  if (mi < 0 && !s->is_cmethod && s->class_id >= 0 && !comp_class_is_module(c, &c->classes[s->class_id])) {
    int top = par >= 0 ? par : s->class_id;
    while (c->classes[top].parent >= 0) top = c->classes[top].parent;
    /* (not under BasicObject: Object is not in that chain; not under another
       builtin: its methods come first, cplan_root_under_builtin) */
    int plain = !class_is_blank_slate(c, top) && !cplan_root_under_builtin(c, top);
    if (plain) {
      int obj = comp_super_parent(c, top, 0);
      if (obj >= 0 && obj != top) mi = call_target(c, id, obj, name, CT_REACH, ml);
    }
    /* a top-level def is Object's (private) method, ahead of Kernel's, for a
       chain that reaches Object (the program need not reopen Object for
       that): not past a builtin or a core module the chain includes that
       answers the name before Object does (Struct#to_s, Exception#message,
       an included Comparable's clamp), nor under BasicObject (a program's
       reopening of it is the top of a chain that does not reach Object
       either) */
    if (mi < 0) {
      const char *root = class_is_blank_slate(c, top) || builtin_core_class_slot(c->classes[top].name) == CORE_BASIC_OBJECT
                         ? NULL : comp_super_builtin_root(c, s->class_id, NULL);
      /* (one def on the chain the target is read from, made for sure before the method's own def runs: a top-level function the program makes again has versions,
         and one made later is none a call before it reaches (the plain call takes its presence entry, which raises
         there); which one a super reaches depends on when it runs, which no single target says, so the super keeps
         the answer it had. A def that may read self stays private to a receiver here as it does for a call
         (cplan_root_top_def): the def takes no self, so it would run on main) */
      ElabVersion tv[VER_MAX_ARMS];
      int tc = ct_top_chain(c, name, 0);
      if (root && !builtin_answers_ahead_of(root, name, "Object") && !comp_chain_core_answers(c, s->class_id, name) &&
          !cplan_top_name_reads_self(c, name) && elab_chain_versions(c, tc, 0, name, tv, VER_MAX_ARMS) == 1 && s->def_node >= 0 &&
          elab_method_made_at(c, tc, name, elab_point_of(c, s->def_node)) == ELAB_YES)
        mi = call_target(c, id, -1, name, CT_TOP | CT_REACH, ml);
    }
    if (mi < 0 && plain) {
      static const int owners[] = { CORE_KERNEL, CORE_BASIC_OBJECT };
      /* a reopening of Kernel or BasicObject answers only where no builtin
         method of the name comes before it (Object's, then Kernel's, as
         CRuby's tables hold them) */
      int builtin_first = builtin_owns_instance_method("Object", name);
      for (int i = 0; mi < 0 && !builtin_first && i < 2; i++) {
        int k = comp_core_class(c, owners[i]);
        if (k >= 0) mi = call_target(c, id, k, name, CT_REACH, ml);
        builtin_first = builtin_owns_instance_method(core_class_name(owners[i]), name);
      }
    }
  }
  if (mi < 0) ml->def_class = -1;
  ml->from = start;
  return mi;
}

/* The method `super` in method scope s reaches, for call id (-1: a super
   no call stands for, an open history's entry's else arm), and in ml->from
   the class it is found from: the next method of s's prepend chain
   (comp_super_shadow), else the parent chain's (cplan_super_chain_mi); -1
   for none; ml->from is s's class when the shadow is found, -1 when not.
   A super of a call whose next method is one of a mixin's copies of a
   module's versions, a layer below a later one, reaches the entry over
   them once the entries are made (super_layer_entry). ml is call_target's
   answer for the lookup (NULL: the scope only). */
/* See call_plan.h. */
int cplan_super_start(Compiler *c, const Scope *s) {
  int par = comp_super_parent(c, s->class_id, s->is_cmethod);
  if (par < 0 || s->is_cmethod || c->classes[s->class_id].parent >= 0) return par;
  /* a class with no program superclass whose builtin root answers the name ahead of Object: the builtin's
     method comes before a reopening of Object's in CRuby's ancestors, and the chain has no program class to
     start from (a reopening itself starts after its own methods, which the builtin_super_owner answers) */
  const char *uname = comp_super_name(c, par, s);
  int self = 0;
  const char *root = uname ? comp_super_builtin_root(c, s->class_id, &self) : NULL;
  /* (a Struct's or Data's own methods other than its text and its initialize have no arm yet behind a plan with no
     start: the emitter raises where the chain's reopening of Object answered, so those keep the reopening) */
  if (builtin_owner_kind(root) == BOWN_STRUCT && !is_to_s_name(uname) && !is_inspect_name(uname) && !is_initialize_name(uname)) return par;
  return root && !self && builtin_answers_ahead_of(root, uname, "Object") ? -1 : par;
}
static int super_layer_entry(Compiler *c, int id, int t);
static int super_guard_entry(Compiler *c, int id, int t);
int cplan_super_target(Compiler *c, int id, const Scope *s0, MethodLookup *ml) {
  const Scope *s = comp_super_site(c, s0);
  const char *shadow = comp_super_shadow(c, s);
  MethodLookup own = { .def_class = -1 };
  if (!ml) ml = &own;
  if (shadow) {
    int mi = call_target(c, id, s->class_id, shadow, CT_OWN | CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), ml);
    ml->from = mi >= 0 ? s->class_id : -1;
    int e = mi >= 0 && id >= 0 && c->ver_built ? super_layer_entry(c, id, mi) : -1;
    if (e < 0 && mi >= 0 && id >= 0 && c->ver_built) e = super_guard_entry(c, id, mi);
    if (e >= 0) { ml->kind = ML_ENTRY; ml->why = 0; }
    return e >= 0 ? e : mi;
  }
  /* (a Method#super_method, asked with no node, has no Method value of the builtin's to answer with: it keeps
     the program's chain) */
  int par = id >= 0 ? cplan_super_start(c, s) : comp_super_parent(c, s->class_id, s->is_cmethod);
  const char *uname = comp_super_name(c, par, s);
  if (!uname) {
    *ml = (MethodLookup){ .kind = ML_NONE, .def_class = -1, .from = par };
    return -1;
  }
  int mi = cplan_super_chain_mi(c, id, s, par, uname, ml);
  /* no method of the program's answers: the builtin's does, if one, unless the program has a top-level def of the
     name that the lookup could not rule out (below) */
  if (mi < 0) ml->bowner = comp_super_bowner(c, s, uname);
  if (mi < 0 && ml->bowner && cplan_super_top_def_unproven(c, s, uname)) ml->bowner = NULL;
  return mi;
}

/* Does the `super` of the program's send, __send__ or public_send in scope
   s, which finds no method above it, reach Object's, which nothing can call
   (a body that is only a pass-through to it lowers past the method:
   send_def_just_supers; an instance side's does only while no other of the
   name exists, as every send then lowers past it)? */
static int cplan_send_super_refused(Compiler *c, const Scope *s, const char *uname) {
  return !send_def_just_supers(c, (int)(s - c->scopes)) || (!s->is_cmethod && send_family_defined_anywhere(c, uname));
}

static void cplan_resolve_super(Compiler *c, int id, CallPlan *p) {
  Scope *s = comp_scope_of(c, id);
  if (!s || s->class_id < 0 || !s->name) return;
  MethodLookup ml = { .def_class = -1 };
  int mi = cplan_super_target(c, id, s, &ml);
  if (mi >= 0) { cplan_set(p, mi, ml.from, UC_SUPER, CP_DIRECT); return; }
  p->bowner = ml.bowner;
  const Scope *st = comp_super_site(c, s);
  const char *uname = comp_super_shadow(c, st) ? NULL : comp_super_name(c, ml.from, st);
  if (uname && s->is_cmethod &&
           builtin_super_cmethod_known(c->classes[s->class_id].name, uname) &&
           ml.kind == ML_NONE) {
    cplan_set(p, -1, s->class_id, UC_SUPER, CP_REFUSE);
    p->rkind = CR_FEATURE; p->rfrom = CRF_LIMIT;
    p->msg = "super to an inherited builtin singleton method is not supported; see docs/limitations.md";
  } else if (uname && s->is_cmethod && is_definition_hook_name(uname))
    /* Class#inherited and the like: a no-op that reads no class and calls no method */
    p->bowner = builtin_super_owner(comp_class_is_module(c, &c->classes[s->class_id]) ? "Module" : "Class", uname, 0);
  else if (uname && is_send_family(uname) && cplan_send_super_refused(c, s, uname)) {
    /* `super` from the program's own send, __send__ or public_send that
       nothing above defines is Object's: it dispatches on the name at run
       time, which no emitted code does, so it is refused rather than
       answered by a NoMethodError CRuby does not raise */
    char msg[400];
    snprintf(msg, sizeof msg, "unsupported `super` in a program-defined `%s`: "
             "it reaches Object#%s, which a program method cannot call yet "
             "(only a `%s` that passes its parameters straight to `super` is supported, "
             "in a program with no other `%s` of its own)", uname, uname, uname, uname);
    cplan_set(p, -1, s->class_id, UC_SUPER, CP_REFUSE);
    p->rkind = CR_FEATURE; p->rfrom = CRF_LIMIT;
    p->msg = cplan_keep(c, strdup(msg));
  }
}

/* See call_plan.h. */
int cplan_core_hook(Compiler *c, int id) {
  const Scope *s = comp_scope_of(c, id);
  return s && s->is_cmethod && cplan_user(c, id).bowner;
}

/* Is node id inside a block, a lambda or a method body (it may run later)? */
static int cplan_in_closure(Compiler *c, int id) {
  const int *par = comp_parent_map(c);
  for (int a = par[id]; a >= 0; a = par[a]) {
    NodeKind k = nt_kind(c->nt, a);
    if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode) return 1;
  }
  return 0;
}
/* Does call `id`, outside any block, lambda or method body, run ahead of
   the statement that defines method mi, in the order the load phase runs
   (the load record)? There the name is still the builtin's. */
static int cplan_called_before_def(Compiler *c, int id, int mi) {
  return cplan_runs_before_def_node(c, id, c->scopes[mi].def_node);
}
/* See call_plan.h. */
int cplan_runs_before_def_node(Compiler *c, int id, int dn) {
  return dn >= 0 && !cplan_in_closure(c, id) && elab_cmp(elab_point_of(c, id), elab_point_of(c, dn)) == PROG_BEFORE;
}
/* --share-strings: does call `id`, with a literal block, still reach the
   builtin rather than String reopening mi's yielding method of its name,
   for being one the program runs ahead of mi's definition at its top level
   (as in CRuby)? The plan, the inference and the emitter ask it alike. */
int cplan_reopen_runs_builtin(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  if (mi >= 0 && nt_int(nt, c->scopes[mi].def_node, "concat_builtin_version", 0)) return 1;
  /* concat keeps the builtin until Array's definition has run, whether
     or not this call supplies a block. Use the same load-order fact as
     the existing String case, not another method lookup. */
  if (mi >= 0 && c->scopes[mi].class_id == comp_core_class(c, CORE_ARRAY) &&
      sp_streq(nt_str(nt, id, "name"), "concat") && cplan_called_before_def(c, id, mi)) return 1;
  int blk = nt_ref(nt, id, "block");
  if (mi >= 0 && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && c->scopes[mi].yields &&
      c->scopes[mi].class_id == comp_core_class(c, CORE_STRING) && cplan_called_before_def(c, id, mi)) return 1;
  /* any builtin class's reopening, for a call the program runs ahead of
     the method's definition at its top level, where the builtin class has
     the name: CRuby runs the builtin's (#10719: `p a.sum` before `class
     Array; def sum`) */
  int k = mi >= 0 ? c->scopes[mi].class_id : -1;
  const char *kn = k >= 0 && k < c->nclasses && c->classes[k].name && c->classes[k].enclosing_class < 0 &&
                   builtin_class_id(c->classes[k].name) < 0 ? c->classes[k].name : NULL;
  const char *cn = nt_str(nt, id, "name");
  return kn && builtin_answers_ahead_of(kn, cn, NULL) && cplan_called_before_def(c, id, mi);
}
/* The existing boxed dispatch candidates also serve literal-name send arms:
   the method call `call` runs on each candidate's arm. */
static void cplan_resolve_poly(Compiler *c, int call, const char *name, CallPlan *p,
                               const PolyCand *pcs, int npc) {
  for (int i = 0; i < npc; i++) {
    if (c->classes[pcs[i].cls].is_native_class) continue;
    /* a reader candidate written as a method (`def x = @x`) is that method */
    int pmi = pcs[i].mi >= 0 ? pcs[i].mi : call_target(c, call, pcs[i].cls, name, 0, NULL);
    if (pmi >= 0) { cplan_set(p, pmi, pcs[i].cls, UC_POLY, CP_SWITCH); return; }
  }
  /* a boxed class value: the dispatch's class-method arms */
  int ncc = 0;
  const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
  for (int i = 0; i < ncc; i++)
    if (ccs[i].mi >= 0) { cplan_set(p, ccs[i].mi, ccs[i].cls, UC_POLY, CP_SWITCH); return; }
  return;
}

/* A missing base implementation still dispatches to the existing descendants:
   the method call `call` runs on each. */
static void cplan_descendant_call(Compiler *c, int call, int cid, const char *name, int cmeth, CallPlan *p) {
  int nd = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  int reader = !cmeth && call_attr_target(c, -1, cid, name, CT_ATTR, NULL);
  for (int i = 0; i < nd; i++) {
    int mi = call_target(c, call, ds[i], name, reader ? 0 : CT_OWN | (cmeth ? CT_CMETHOD : 0), NULL);
    if (mi < 0) continue;
    cplan_set(p, mi, cid, cmeth ? UC_CMETH : UC_INST,
              cmeth || reader ? CP_SWITCH : cplan_dispatch_form(c, call, cid, name, 0));
    return;
  }
}

/* The finite singleton-accessor union, using the resolver's precedence. */
int cplan_accessor_classes(Compiler *c, int id, int *out, int cap) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0 || cplan_builtin_call(c, id)) return 0;
  int ci = comp_sg_reader_const(c, recv);
  if (ci >= 0 && call_target(c, id, ci, name, CT_CMETHOD, NULL) >= 0) return 0;
  int n = comp_sg_reader_candidates(c, recv, out, cap);
  if (n >= 2)
    for (int i = 0; i < n; i++)
      if (call_target(c, id, out[i], name, CT_CMETHOD, NULL) >= 0) return n;
  return 0;
}

/* The name index bounds the native block query before its class scan. */
int cplan_native_block_possible(Compiler *c, int id, const PolyCand *pc, int n) {
  if (nt_kind(c->nt, nt_ref(c->nt, id, "block")) != NK_BlockNode) return 0;
  for (int i = 0; i < n; i++)
    if (pc[i].native && pc[i].mi >= 0 && c->classes[pc[i].cls].instantiated) return 1;
  return 0;
}

static void cplan_resolve_call(Compiler *c, int id, CallPlan *p) {
  const NodeTable *nt = c->nt;
  if (cplan_builtin_call(c, id)) return;
  const char *name = nt_str(nt, id, "name");
  if (!name) return;
  int recv = nt_ref(nt, id, "receiver");
  /* a singleton accessor folding to one constant: that class's method */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    int fmi = fold_ci >= 0 ? call_target(c, id, fold_ci, name, CT_CMETHOD, NULL) : -1;
    if (fmi >= 0) { cplan_set(p, fmi, fold_ci, UC_CMETH, CP_DIRECT); return; }
    int cand[32];
    int ncand = cplan_accessor_classes(c, id, cand, 32);
    if (ncand >= 2) {
      for (int i = 0; i < ncand; i++) {
        int mi = call_target(c, id, cand[i], name, CT_CMETHOD, NULL);
        if (mi >= 0) {
          cplan_set(p, mi, cand[i], UC_CMETH, CP_SWITCH);
          return;
        }
      }
    }
  }
  /* a retargeted `x.send(:m)` reaching a top-level def the receiver does
     not own itself */
  if (recv >= 0 && nt_str(nt, id, "send_blind") && nt_ref(nt, id, "block") < 0) {
    int smi = call_target(c, id, -1, name, CT_TOP, NULL);
    if (smi >= 0 && !c->scopes[smi].yields) {
      if (!send_blind_recv_owns(c, id, recv, comp_ntype(c, recv), name)) {
        cplan_set(p, smi, -1, UC_SEND_BLIND, CP_DIRECT);
        return;
      }
    }
  }
  if (recv < 0) {
    /* inside an instance_eval/exec block self is the rebound receiver */
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = call_target(c, id, iec, name, 0, NULL);
      if (imi >= 0) { cplan_set(p, imi, iec, UC_IE, CP_DIRECT); return; }
    }
    /* ...or one of several, by the receiver's runtime class */
    int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    for (int i = 0; i < npk; i++) {
      int imi = call_target(c, id, pk[i], name, 0, NULL);
      if (imi >= 0) { cplan_set(p, imi, pk[i], UC_IE, CP_SWITCH); return; }
    }
    int cb = comp_cbody_call_mi(c, id, name);
    if (cb >= 0) { cplan_set(p, cb, c->node_cbody[id], UC_CMETH, CP_DIRECT); return; }
    int mi = comp_self_call_mi(c, id, name);
    if (mi >= 0) {
      Scope *m = &c->scopes[mi];
      Scope *self = comp_scope_of(c, id);
      /* a top-level def a descendant of self's class overrides: a switch
         on self's class, the def its default */
      if (self && (m->class_id < 0 || m->class_id == comp_core_class(c, CORE_OBJECT)) &&
          cplan_self_root_override(c, id, self->class_id, mi)) {
        cplan_set(p, mi, self->class_id, UC_INST, CP_PER_ARM);
        p->chain = 1;
        return;
      }
      if (m->class_id < 0) { cplan_set(p, mi, -1, UC_TOP, CP_DIRECT); return; }
      int scls = self ? self->class_id : m->class_id;
      int form = m->is_cmethod ? (scls >= 0 && cplan_overridden(c, scls, name, 1) ? CP_SWITCH : CP_DIRECT)
               : scls >= 0 ? cplan_dispatch_form(c, id, scls, name, 1) : CP_DIRECT;
      cplan_set(p, mi, scls, m->is_cmethod ? UC_CMETH : UC_INST, form);
      /* an instance method comp_self_call_mi took from the self class's chain */
      p->chain = !m->is_cmethod && self && self->class_id >= 0;
      return;
    }
    int imi = call_target(c, id, -1, name, CT_INCLUDED, NULL);
    if (imi >= 0) { cplan_set(p, imi, c->scopes[imi].class_id, UC_INCLUDED, CP_DIRECT); return; }
    /* outside a class method, self reaches a program's own Object method */
    Scope *self = comp_scope_of(c, id);
    if (!(self && self->is_cmethod)) cplan_root_reopen(c, id, name, self && self->class_id >= 0 ? cplan_builtin_name_of(c, self->class_id) : "Object", CORE_OBJECT, p);
    /* ...unless a descendant of self's class overrides it: a switch on
       self's class, Object's method its default */
    if (p->mi >= 0 && p->via == UC_REOPEN && self && cplan_self_root_override(c, id, self->class_id, p->mi)) {
      int omi = p->mi;
      cplan_set(p, omi, self->class_id, UC_INST, CP_PER_ARM);
      p->chain = 1;
      return;
    }
    if (p->mi < 0 && self && self->class_id >= 0)
      cplan_descendant_call(c, id, self->class_id, name, self->is_cmethod, p);
    return;
  }
  NodeKind rk = nt_kind(nt, recv);
  TyKind rt = comp_ntype(c, recv);
  /* a Class reopen's own method answers a class value first, as the
     inference arm for Range / Time / IO / Class receivers does */
  if (rt == TY_CLASS) {
    int kci = comp_core_class(c, CORE_CLASS);
    int kmi = kci >= 0 ? call_target(c, id, kci, name, 0, NULL) : -1;
    if (kmi >= 0) { cplan_set(p, kmi, kci, UC_REOPEN, CP_DIRECT); return; }
  }
  int sci = self_class_static_ci(c, recv);   /* `self.class` naming one class */
  int bci = rk == NK_SelfNode ? class_body_self_ci(c, recv) : -1;   /* a class body's self */
  if (sci >= 0 || bci >= 0 || (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)) {
    int ci = sci >= 0 ? sci : bci >= 0 ? bci : const_node_class_index(c, recv);
    int mi = ci < 0 ? -1 : cplan_cmethod_target(c, id, ci, name, 0);
    /* a package's native_func, which the constant calls in C directly; its
       def is for the module reached as a value */
    if (sci < 0 && comp_scope_native_twin(c, mi)) return;
    if (mi >= 0) { cplan_set(p, mi, ci, UC_CMETH, CP_DIRECT); return; }
    /* a method the program adds to Class, on a builtin class constant */
    int rmi = class_reopen_cmethod(c, id, recv, name);
    if (rmi >= 0) { cplan_set(p, rmi, c->scopes[rmi].class_id, UC_CMETH, CP_DIRECT); return; }
    /* a constant holding an instance is read by its type below */
  }
  /* a class held in a variable: a switch over every class with a class
     method of the name */
  else if (rt == TY_CLASS) {
    int ncc = 0;
    const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
    /* (self in a class method is that class or one under it: a class
       method of an unrelated class or module, a module function's copy
       among them, is not on its chain) */
    const Scope *ss = rk == NK_SelfNode ? comp_scope_of(c, id) : NULL;
    int under = ss && ss->is_cmethod && ss->class_id >= 0 && !comp_class_is_module(c, &c->classes[ss->class_id]) ? ss->class_id : -1;
    for (int i = 0; i < ncc; i++)
      if (ccs[i].mi >= 0 && (under < 0 || ccs[i].cls == under || is_descendant(c, ccs[i].cls, under) || is_descendant(c, under, ccs[i].cls))) {
        cplan_set(p, ccs[i].mi, ccs[i].cls, UC_CMETH, CP_SWITCH);
        p->by_name = 1;
        return;
      }
  }
  if (ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    int mi = call_target(c, id, cid, name, 0, NULL);
    if (mi >= 0) {
      cplan_set(p, mi, cid, UC_INST, cplan_dispatch_form(c, id, cid, name, 1));
      p->chain = 1;
      return;
    }
    /* the operators an object answers through another of its methods:
       `!=` through `==`, the comparisons through `<=>` when Comparable
       supplies them (class_is_comparable). Not one a subclass defines
       itself: the call dispatches at run time (#4023). */
    mtab_enter(c);   /* (the plan's own candidate read: the strict subclasses that answer the name) */
    int sub_answers = comp_subclass_answers(c, cid, name);
    mtab_leave(c);
    const char *via_op = sub_answers ? NULL : builtin_operator_delegate(name);
    if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
    /* (the delegate is an implied call of the operator: asked as the emitter asks it, which sees an undef) */
    MethodLookup dl = { .def_class = -1 };
    const char *gone = NULL;
    /* (the operator itself undefined where the record places the call: the call raises naming it) */
    if (via_op) {
      cplan_implied_target(c, id, recv, cid, name, 0, &dl);
      if (dl.kind == ML_NONE && dl.why == MLW_RECORD) gone = name;
    }
    int dmi = via_op && !gone ? cplan_implied_target(c, id, recv, cid, via_op, 0, &dl) : -1;
    if (via_op && !gone && dmi < 0 && dl.kind == ML_NONE && dl.why == MLW_RECORD) gone = via_op;
    if (dmi < 0 && via_op && !gone) {   /* (stops at the first undefined delegate: CRuby raises there) */
      via_op = builtin_operator_delegate(via_op);
      if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
      dmi = via_op ? cplan_implied_target(c, id, recv, cid, via_op, 0, &dl) : -1;
      if (via_op && dmi < 0 && dl.kind == ML_NONE && dl.why == MLW_RECORD) gone = via_op;
    }
    if (dmi >= 0) {
      cplan_set(p, dmi, cid, UC_INST, cplan_dispatch_form(c, id, cid, via_op, 1));
      p->delegate = via_op;
      return;
    }
    if (gone) {
      p->delegate = gone;
      p->delegate_undef = 1;
      /* what the builtin answers before it reaches the undefined method (the delegate row's cut) */
      p->delegate_cut = (unsigned char)builtin_operator_cut(name, gone);
      return;
    }
    /* a reader a subclass overrides with a method: the switch reaches the
       override for that subclass */
    int reader = call_attr_target(c, -1, cid, name, 0, NULL);
    if (reader) {
      int nd = 0;
      const int *ds = comp_descendants(c, cid, &nd);
      for (int i = 0; i < nd; i++) {
        int kmi = ds[i] != cid ? call_target(c, -1, ds[i], name, 0, NULL) : -1;
        if (kmi >= 0) { cplan_set(p, kmi, cid, UC_INST, CP_SWITCH); return; }
      }
    }
    if (class_is_exc_subclass(c, cid)) cplan_exc_reopen(c, id, name, p);
    /* (an attribute of the class or a Struct's member answers its name first) */
    char abase[256];
    int attr_answers = reader || (p->mi < 0 && setter_base_name(name, abase, sizeof abase) &&
                                  call_attr_target(c, -1, cid, abase, CT_WRITER, NULL));
    if (p->mi < 0 && !attr_answers) {
      cplan_root_reopen(c, id, name, cplan_builtin_name_of(c, cid), CORE_OBJECT, p);
      /* `self.m` (or a bare m the analysis gave self) reaching Object's
         method that a descendant of self's class overrides: a switch on
         self's class, Object's method its default */
      if (p->mi >= 0 && rk == NK_SelfNode && cplan_self_root_override(c, id, cid, p->mi)) {
        int omi = p->mi;
        cplan_set(p, omi, cid, UC_INST, CP_PER_ARM);
        p->chain = 1;
        return;
      }
    }
    if (p->mi < 0 && !reader && !c->classes[cid].is_native_class)
      cplan_descendant_call(c, id, cid, name, 0, p);
    return;
  }
  if (rt == TY_POLY) {
    /* main is an Object and no other class's: a call on it reaches Object's
       method of the name, its own or a module's, with no builtin ahead */
    if (ct_runs_on_main(c, id)) {
      cplan_root_reopen(c, id, name, "Object", CORE_OBJECT, p);
      if (p->mi >= 0) return;
    }
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    if (cplan_native_block_possible(c, id, pcs, npc)) {
      const PolyPlan bp = cplan_poly_block(c, id);
      for (int i = 0; i < bp.n; i++)
        if (bp.arm[i].mi >= 0) {
          cplan_set(p, bp.arm[i].mi, bp.arm[i].key, UC_POLY, CP_SWITCH);
          return;
        }
    }
    cplan_resolve_poly(c, id, name, p, pcs, npc);
    /* no class of the program answers, and no builtin class below Object
       (whose method a value of it reaches first): every value is Object's,
       and the top level's def is open to the call */
    /* (a class that answers the name without a method scope, an attribute
       or a Struct member, leaves the plan unbound too: its instance in the
       box takes its own, so the call stays the dispatch it was. Such a
       class is one of the name's candidates, which the plan already holds:
       no table is read for it.) */
    if (p->mi < 0 && !builtin_name_below_object(name) && !npc) {
      int cell, tm = cplan_root_top_def(c, id, name, core_class_name(CORE_OBJECT), &cell);
      if (tm >= 0) {
        cplan_set(p, tm, c->scopes[tm].class_id, UC_REOPEN, CP_DIRECT);
        p->vis_cell = cell;
        if (cell) ver_vis_read_add(c, id, name, cell - 1);
      }
    }
    return;
  }
  /* a reopening of the exception classes that defines the name stands for
     the call; otherwise the call goes on to the Object fallback below */
  if (rt == TY_EXCEPTION) { cplan_exc_reopen(c, id, name, p); if (p->mi >= 0) return; }
  /* a call written under a builtin alias (detect, collect_concat) that the
     desugar renamed to the builtin's own name reaches a reopen's method of
     the name it was written with, not of the new name */
  const char *rname = nt_str(nt, id, "orig_name") ? nt_str(nt, id, "orig_name") : name;
  /* an IO handle's reopen: the File, IO or socket class that defines it */
  if (rt == TY_IO) {
    int eci = io_reopen_class(c, id, name);
    int emi = eci >= 0 ? call_target(c, id, eci, name, 0, NULL) : -1;
    if (emi >= 0) {
      cplan_set(p, emi, eci, UC_REOPEN, io_reopen_ret_mixed(c, id, name) ? CP_SWITCH : CP_DIRECT);
      return;
    }
  }
  /* true and false: each value's own class (cplan_bool_sides) */
  if (rt == TY_BOOL) {
    int tmi, fmi;
    if (cplan_bool_sides(c, id, &tmi, &fmi)) {
      int mi = tmi >= 0 ? tmi : fmi;
      cplan_set(p, mi, c->scopes[mi].class_id, UC_REOPEN, CP_SWITCH);
      return;
    }
  }
  int rc = cplan_reopen_class(rt);
  if (rc >= 0) {
    int ci = comp_core_class(c, rc);
    int mi = ci >= 0 ? call_target(c, id, ci, rname, 0, NULL) : -1;
    /* (--share-strings: a String reopening's yielding method of a name,
       which a call with a literal block reaches, is the builtin's still for
       a call the program runs ahead of the method's definition, at its top
       level, as in CRuby) */
    if (mi >= 0 && cplan_reopen_runs_builtin(c, id, mi)) return;
    if (mi >= 0) { cplan_set(p, mi, ci, UC_REOPEN, CP_DIRECT); return; }
  }
  /* a nil receiver's NilClass reopen, an Integer or Float receiver's
     Numeric reopen (the inference rule beside the scalar reopens) */
  if (rt == TY_NIL || rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) {
    cplan_root_reopen(c, id, name, rt == TY_NIL ? "NilClass" : rt == TY_FLOAT ? "Float" : "Integer",
                      rt == TY_NIL ? CORE_NILCLASS : CORE_NUMERIC, p);
    if (p->mi >= 0) return;
  }
  /* an Array or Hash reopen's own method of the name. With a block, the
     reopen's only when the builtin arms do not keep the call: a yielding
     reopen of a name the builtin surface has (`each`, `map`) stays the
     builtin's loop; one of a name it lacks, or one that does not yield,
     runs the reopen's method, through its proc form when it yields
     (an_reopen_owner: the inference and the emitter decide alike) */
  if (ty_is_array(rt) || ty_is_obj_array(rt) || ty_is_hash(rt)) {
    int cn = ty_is_hash(rt) ? CORE_HASH : CORE_ARRAY;
    int ami = an_reopen_owner(c, id, cn);
    if (ami >= 0) { cplan_set(p, ami, comp_core_class(c, cn), UC_REOPEN, CP_DIRECT); return; }
  }
  /* last, a program's own Object method for a name no builtin class the
     receiver's lookup passes first answers */
  int bcls = object_fallback_class_of_type(rt);
  const char *bn = rt == TY_BOOL ? "TrueClass" : bcls >= 0 ? core_class_name(bcls) : NULL;
  /* (not a String, Integer or Float that may be nil, where NilClass has the
     name too: the boxed dispatch, cplan_poly_sub, picks by the value) */
  /* (a singleton accessor of a class answers its name ahead of the top level's def: it is no scope) */
  if (bn && !(rt == TY_CLASS && cplan_class_value_answers(c, id, recv, name)) && !cplan_nil_scalar_reach(c, id))
    cplan_root_reopen(c, id, name, bn, CORE_OBJECT, p);
}

static void cplan_resolve(Compiler *c, int id, CallPlan *p) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) cplan_resolve_super(c, id, p);   /* (its reads stay the super family's to convert) */
  else if (k == NK_CallNode) {
    MtabMark mt_was = mtab_push_in(c, id);   /* (what the resolution asks of the tables is the call's) */
    cplan_resolve_call(c, id, p);
    p->answers_receiver = p->via == UC_CMETH && cmethod_receiver_class(c, p->mi, p->owner_ci) >= 0;
    int recv = nt_ref(c->nt, id, "receiver");
    const char *name = nt_str(c->nt, id, "name");
    if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
        nt_str(c->nt, id, "send_blind") && nt_ref(c->nt, id, "block") < 0 && name &&
        send_blind_recv_owns(c, id, recv, TY_POLY, name)) {
      int mi = call_target(c, id, -1, name, CT_TOP, NULL);
      if (mi >= 0 && !c->scopes[mi].yields) p->send_fallback = mi;
    }
    mtab_pop(c, mt_was);
  }
}

/* A constant call's user singleton target, shared by inference, rewrites
   and builtin emitters. Unregistered constants need no target lookup.
   Ordinary user classes never need this precedence decision; native package
   bindings supply their own builtin names. The caller supplies the receiver
   node; a receiver that is not a constant has no target. */
CallPlan cplan_const_user(Compiler *c, int id, int recv, int fresh) {
  const NodeTable *nt = c->nt;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? const_key_at(c, recv) : NULL;
  CallPlan none;
  cplan_set(&none, -1, -1, UC_NONE, CP_NONE);
  if (!cn || c->nclasses == 0) return none;
  int ci = const_node_class_index(c, recv);
  if (ci < 0) return none;
  if (!c->classes[ci].is_native_class && !c->classes[ci].is_builtin_const &&
      (c->n_native_funcs == 0 || comp_native_find(c, cn, nt_str(nt, id, "name")) < 0) &&
      (c->n_ffi_funcs == 0 || ffi_find_func(c, cn, nt_str(nt, id, "name")) < 0)) return none;
  if (cplan_builtin_call(c, id)) return none;
  const CallPlan p = fresh ? cplan_user_fresh(c, id) : cplan_user(c, id);
  return p.mi >= 0 && p.via == UC_CMETH && p.dispatch == CP_DIRECT ? p : none;
}

/* Ask the native-block dispatch of the call once into blk (see
   PolyBlockMembers); the same tests cplan_resolve_call makes for a poly
   receiver. */
static void cplan_block_members(Compiler *c, int id, const PolyCand *pcs, int npc, PolyBlockMembers *blk) {
  blk->state = 1;
  blk->n = 0;
  if (!cplan_native_block_possible(c, id, pcs, npc)) return;
  const PolyPlan bp = cplan_poly_block(c, id);
  blk->state = 2;
  for (int i = 0; i < bp.n; i++) {
    int arm = bp.arm[i].mi;
    if (arm < 0) continue;
    /* (more than blk holds: the member test scans the plan itself) */
    if (blk->n + 2 > (int)(sizeof blk->mi / sizeof *blk->mi)) { blk->state = 3; blk->n = 0; return; }
    blk->mi[blk->n++] = arm;
    int form = scope_proc_form_of(c, arm);
    if (form >= 0) blk->mi[blk->n++] = form;
  }
}

int cplan_virtual_member(Compiler *c, int id, const CallPlan *p, int mi, PolyBlockMembers *blk) {
  if (mi < 0) return 0;
  if (p->send_fallback == mi) return 1;
  if (p->mi < 0) return 0;
  if (p->mi == mi) return 1;
  if (p->dispatch < CP_SWITCH) return 0;
  const char *name = p->delegate ? p->delegate : nt_str(c->nt, id, "name");
  if (!name) return 0;
  if (p->via == UC_CMETH) {
    int cand[32];
    int ncand = cplan_accessor_classes(c, id, cand, 32);
    if (ncand >= 2) {
      for (int i = 0; i < ncand; i++)
        if (call_target(c, id, cand[i], name, CT_CMETHOD, NULL) == mi) return 1;
      return 0;
    }
  }
  if (p->by_name) return cplan_cand_arm(c, id, name, mi, 0);
  if (p->via == UC_POLY) {
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    PolyBlockMembers once;
    once.state = 0;
    if (!blk) blk = &once;
    if (!blk->state) cplan_block_members(c, id, pcs, npc, blk);
    for (int i = 0; i < blk->n; i++)
      if (blk->mi[i] == mi) return 1;
    if (blk->state == 3) {
      const PolyPlan bp = cplan_poly_block(c, id);
      for (int i = 0; i < bp.n; i++)
        if (bp.arm[i].mi == mi || scope_proc_form_of(c, bp.arm[i].mi) == mi) return 1;
    }
    return cplan_cand_arm(c, id, name, mi, 1);
  }
  if (p->via == UC_REOPEN) {
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv >= 0 && comp_ntype(c, recv) == TY_IO) {
      int ks[16], nk = io_reopen_defs(c, id, name, 0, ks, 16);
      for (int i = 0; i < nk; i++)
        if (call_target(c, id, ks[i], name, 0, NULL) == mi) return 1;
    }
    int xr[8], xn = exc_reopen_definers(c, name, xr, 8);
    for (int i = 0; i < xn; i++)
      if (call_target(c, id, xr[i], name, 0, NULL) == mi) return 1;
    return 0;
  }
  if (p->via == UC_IE) {
    int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    for (int i = 0; i < npk; i++)
      if (call_target(c, id, pk[i], name, 0, NULL) == mi) return 1;
    return 0;
  }
  /* an override in a descendant of the class the chain was searched from */
  int cmeth = c->scopes[p->mi].is_cmethod;
  int nd = 0;
  const int *ds = p->owner_ci >= 0 ? comp_descendants(c, p->owner_ci, &nd) : NULL;
  for (int i = 0; i < nd; i++)
    if (call_target(c, id, ds[i], name, CT_OWN | (cmeth ? CT_CMETHOD : 0), NULL) == mi)
      return 1;
  return 0;
}

static const char *g_cp_site[16];
static int g_cp_site_n[16];
static int g_cp_nsites;

void cplan_served(const char *site) {
  for (int i = 0; i < g_cp_nsites; i++)
    if (g_cp_site[i] == site || sp_streq(g_cp_site[i], site)) { g_cp_site_n[i]++; return; }
  if (g_cp_nsites < 16) { g_cp_site[g_cp_nsites] = site; g_cp_site_n[g_cp_nsites++] = 1; }
}

void cplan_served_report(void) {
  for (int i = 0; i < g_cp_nsites; i++)
    fprintf(stderr, "plan-check: cplan-served: %s %d\n", g_cp_site[i], g_cp_site_n[i]);
}

static int ct_send_shadow(Compiler *c, int id, const char *name) {
  return id >= 0 && (nt_str(c->nt, id, "send_blind") || nt_str(c->nt, id, "vis_enforce")) &&
         comp_name_is_shadow_copy(c, name, SHADOW_PREP);
}

/* A plan-only reader must not lose a user target. This assertion belongs
   with resolution, and never supplies an emission target on a miss. */
void cplan_check_missing(Compiler *c, int id, int mi, int ci, const char *name, int cmeth, const char *site) {
  if (!g_plan_check || mi >= 0 || ci < 0 || !name || cplan_builtin_call(c, id)) return;
  if (ct_send_shadow(c, id, name)) return;
  int own = cmeth ? comp_cmethod_in_chain(c, ci, name, NULL) : comp_method_in_chain(c, ci, name, NULL);
  NodeKind k = nt_kind(c->nt, id);
  if (cmeth && (k == NK_SuperNode || k == NK_ForwardingSuperNode)) {
    Scope *sc = comp_scope_of(c, id);
    if (sc && !comp_super_shadow(c, comp_super_site(c, sc))) own = cplan_super_chain_mi(c, id, sc, ci, name, NULL);
  }
  if (own >= 0)
    fprintf(stderr, "plan-check: cplan-missing: %s node %d %s: chain %d\n", site, id, name, own);
}

/* the node is read as itself: nothing re-types or re-scopes it */
int cplan_plain_ctx(void) {
  return view_depth() == 0 && comp_scope_move_depth() == 0 && g_ie_class_id < 0 &&
         inline_splice_depth() == 0;
}

static int cplan_capture_absent(Compiler *c, int id, int mi);
static int cplan_capture_closed(Compiler *c, int id, int mi, short *cls);
static int cplan_main_capture_absent(Compiler *c, int id);
static int cm_capture_class(Compiler *c, int id, const char **name);
static int cplan_capture_gap(Compiler *c, int id);
/* A bound Method already carries its target. Its arguments are bound by
   the Method adapter, not by the generic call-target argument walk.
   Metadata readers also need the captured target and dispatch kind. */
CallPlan cplan_method_call(Compiler *c, int id) {
  CallPlan p;
  cplan_set(&p, -1, -1, UC_NONE, CP_NONE);
  int recv = nt_ref(c->nt, id, "receiver");
  int capture = is_method_object_maker(nt_str(c->nt, id, "name"));
  if (!capture && (recv < 0 || comp_ntype(c, recv) != TY_METHOD)) return p;
  an_pure_read_begin();
  int mn = capture ? id : method_recv_node(c, recv);
  int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
  an_pure_read_end();
  if (mi >= 0) cplan_set(&p, mi, c->scopes[mi].class_id, c->scopes[mi].is_cmethod ? UC_CMETH : UC_INST, CP_DIRECT);
  if (mi >= 0 && (c->scopes[mi].entry || cplan_bam_string_guard(c, mn, mi) >= 0)) p.dispatch = CP_SWITCH;
  /* (a reader on the capture itself: the capture's own, where it has no target) */
  int mk = capture ? id : mi < 0 && mn >= 0 && mn == recv ? mn : -1;
  p.absent = mk >= 0 ? (unsigned char)cplan_capture_absent(c, mk, mi) : 0;
  if (mk >= 0 && mi < 0 && !p.absent && cplan_main_capture_absent(c, mk)) p.absent = CP_ABSENT_MAIN;
  if (p.absent && mi < 0) p.owner_ci = (short)cm_capture_class(c, mk, NULL);
  /* (and a reader on the capture itself, as for absent: where the capture raises, or tests at the run) */
  int ck = capture ? id : mn >= 0 && mn == recv && is_method_object_maker(nt_str(c->nt, mn, "name")) ? mn : -1;
  p.closed = ck >= 0 && mi >= 0 ? (unsigned char)cplan_capture_closed(c, ck, mi, &p.closed_cls) : 0;
  p.method_node = mn;
  p.method_call = 1;
  return p;
}

/* See call_plan.h. */
int cplan_capture_missing(Compiler *c, int id) {
  return id >= 0 ? (int)nt_int(c->nt, id, "mm_target", 0) - 1 : -1;
}
int cplan_missing_guard(Compiler *c, int t) {
  return t >= 0 && c->scopes[t].def_node >= 0 ? (int)nt_int(c->nt, c->scopes[t].def_node, "mm_test", 0) - 1 : -1;
}
int cplan_capture_arm(Compiler *c, int id, int t) {
  int sup = t >= 0 && c->scopes[t].def_node >= 0 ? (int)nt_int(c->nt, c->scopes[t].def_node, "behind_super", 0) - 1 : -1;
  int mw = sup >= 0 && cplan_user(c, sup).mi < 0 && !cplan_core_hook(c, sup) ? cplan_capture_missing(c, id) : -1;
  return mw >= 0 ? mw : t;
}

/* A literal-name arm emitted on boxed self uses the existing boxed
   candidates. Its record must not admit new arms to the outer send. */
CallPlan cplan_send_arm(Compiler *c, int id) {
  CallPlan p = cplan_user_fresh(c, id);
  int recv = nt_ref(c->nt, id, "receiver");
  if (p.mi < 0 && nt_int(c->nt, id, "dyn_arm", 0) &&
      (recv < 0 || nt_kind(c->nt, recv) == NK_SelfNode)) {
    int npc = 0;
    const char *name = nt_str(c->nt, id, "name");
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    cplan_resolve_poly(c, id, name, &p, pcs, npc);
  }
  return p;
}

CallPlan cplan_conversion(Compiler *c, int id, int op, int ci, const char *name) {
  CallPlan p;
  int mi = cplan_implied_target(c, id, op, ci, name, CIT_TABLE, NULL);
  cplan_set(&p, mi, ci, UC_INST, cplan_dispatch_form(c, -1, ci, name, mi >= 0));
  p.chain = 1;
  p.delegate = name;
  return p;
}

CallPlan cplan_exception_render(Compiler *c, int id, int xcm) {
  const char *name = nt_str(c->nt, id, "name");
  int own = is_message_name(name) ? -1 : call_target(c, id, xcm, name, 0, NULL);
  /* #message is #to_s unless the class overrides #message itself: an
     override answering something other than a String cannot ride the
     const char * helper, so call it directly (#3868) */
  if (own < 0 && is_message_name(name)) {
    int mi8 = cplan_implied_target(c, id, nt_ref(c->nt, id, "receiver"), xcm, "message", CIT_TABLE, NULL);
    /* (Exception#message's to_s, a callback the call makes on its receiver: asked at the call) */
    if (mi8 < 0) mi8 = cplan_implied_target(c, id, nt_ref(c->nt, id, "receiver"), xcm, "to_s", 0, NULL);
    if (mi8 >= 0 && (TyKind)c->scopes[mi8].ret != TY_STRING &&
        (TyKind)c->scopes[mi8].ret != TY_UNKNOWN)
      own = mi8;
  }
  /* a subclass overriding a String #to_s is picked at run time by the
     cls_name-keyed dispatcher below */
  if (own >= 0 && is_to_s_name(name) && (TyKind)c->scopes[own].ret == TY_STRING) {
    int sub_str = 0, sub_other = 0;
    for (int k = 0; k < c->nclasses; k++) {
      if (k == xcm || !is_descendant(c, k, xcm)) continue;
      int ko = cplan_implied_target(c, id, nt_ref(c->nt, id, "receiver"), k, "to_s", CT_OWN, NULL);
      if (ko < 0) continue;
      if ((TyKind)c->scopes[ko].ret == TY_STRING) sub_str = 1;
      else sub_other = 1;
    }
    if (sub_str && !sub_other) own = -1;
  }
  CallPlan p;
  cplan_set(&p, own, own >= 0 ? c->scopes[own].class_id : -1, UC_INST, own >= 0 ? CP_DIRECT : CP_NONE);
  p.chain = 1;
  p.delegate = own >= 0 && is_message_name(name) && is_to_s_name(c->scopes[own].name) ? "to_s" : NULL;
  return p;
}

/* a plan answered is what the asker reads next: the method's value and
   parameters, the owning class's slots (infer_type's memo, imemo_dep_*) */
static CallPlan cplan_dep(CallPlan p) {
  if (p.mi >= 0) IMEMO_DEP_SCOPE(p.mi);
  if (p.send_fallback >= 0) IMEMO_DEP_SCOPE(p.send_fallback);
  if (p.owner_ci >= 0) IMEMO_DEP_CLASS(p.owner_ci);
  return p;
}
/* The name call node id finally calls: its own, or for a send-family call
   the literal Symbol or String its first argument gives, a send of a send
   unwound (each send takes one argument). *used is how many arguments the
   sends took. NULL for a send whose name is computed or missing. */
const char *call_sent_name(const NodeTable *nt, int id, int *used) {
  const char *nm = nt_str(nt, id, "name");
  int ac = 0, a = 0;
  if (used) *used = 0;
  if (!nm || !is_send_family(nm)) return nm;
  const int *av = call_args(nt, id, &ac);
  while (nm && is_send_family(nm)) nm = a < ac ? vis_arg_name(nt, av[a++]) : NULL;
  if (used) *used = a;
  return nm;
}
/* See call_plan.h. */
const char *vis_word_call(const NodeTable *nt, int id, int *skip) {
  const char *nm = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int used = 0, rv = nm ? nt_ref(nt, id, "receiver") : -1;
  const char *w = nm ? call_sent_name(nt, id, &used) : NULL;
  int cm = class_method_vis_of_name(w) != 0, blind = nm && nt_str(nt, id, "send_blind"), pub = nm && nt_str(nt, id, "vis_enforce");
  if (skip) *skip = used;
  /* (a word is private on Module but the class-method ones: public_send of another raises, and so does a direct
     call on a receiver other than self; a send the lowering retargeted keeps which it was, send_blind or
     vis_enforce) */
  if (!w || used > 1 || !(cm || visibility_of_name(w)) ||
      (!cm && (used ? is_public_send(nm) : pub || (rv >= 0 && nt_kind(nt, rv) != NK_SelfNode && !blind))))
    return NULL;
  return w;
}
int call_node_names(Compiler *c, int id, const char *out[2]) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  int n = 0;
  if (k == NK_CallNode || k == NK_CallTargetNode) out[n++] = nt_str(nt, id, "name");
  else if (k == NK_CallOperatorWriteNode || k == NK_CallOrWriteNode || k == NK_CallAndWriteNode) {
    /* (the node table keeps Prism's `read_name` as `name`) */
    out[n++] = nt_str(nt, id, "name");
    out[n++] = nt_str(nt, id, "write_name");
  }
  else if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    Scope *s0 = comp_scope_of(c, id);
    const Scope *s = s0 ? comp_super_site(c, s0) : NULL;
    if (s && s->name && !comp_super_shadow(c, s))
      out[n++] = comp_super_name(c, comp_super_parent(c, s->class_id, s->is_cmethod), s);
  }
  if (k == NK_CallNode && out[0] && is_method_object_maker(out[0])) {
    const char *sym = method_sym_arg(c, id);
    if (sym) out[n++] = sym;
  }
  /* a send of a literal name calls that name too, at the same point */
  if (k == NK_CallNode && out[0] && is_send_family(out[0])) {
    const char *sent = call_sent_name(nt, id, NULL);
    if (sent) out[n++] = sent;
  }
  return n;
}
/* Kernel.String's scalar conversion calls use the version machinery as a
   normal call at the conversion site. The builtin callback table owns the
   names and marks those called on the argument. */
static int kernel_string_implied_calls(Compiler *c, int id, const BopCallback **out) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || comp_core_class(c, CORE_SYMBOL) < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!is_string_class_name(name)) return 0;
  int recv = nt_ref(nt, id, "receiver"), argc;
  if (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && !nt_str(nt, id, "send_blind")) return 0;
  (void)call_args(nt, id, &argc);
  if (argc != 1 || call_plain_argc(c, id) != 1) return 0;
  const BuiltinOp *op = bop_find(BOP_KERNEL, name, argc, nt_ref(nt, id, "block") >= 0);
  if (!op || op->emit != BOPE_KERNEL_STRING) return 0;
  int count = bop_class_callbacks("Kernel", op->name, out);
  return count > 0 ? count : 0;
}

static int kernel_string_implies(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if (!(flags & CT_IMPLIED) || (flags & (CT_CMETHOD | CT_TOP)) || cls != comp_core_class(c, CORE_SYMBOL)) return 0;
  const BopCallback *callbacks;
  int n = kernel_string_implied_calls(c, id, &callbacks);
  for (int i = 0; i < n; i++)
    if ((callbacks[i].roles & BCR_ARG) && sp_streq(callbacks[i].name, name)) return 1;
  return 0;
}

/* See call_plan.h. */
IcCall *call_node_implied_names(Compiler *c, int id, IcCall *buf, int cap, int *count) {
  NtImplied rb[16], *rows = rb;
  int nr = nt_implied_calls(c->nt, id, rb, 16), n = 0;
  if (nr > 16) {
    if (!(rows = malloc(sizeof *rows * (size_t)nr))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    nt_implied_calls(c->nt, id, rows, nr);
  }
  IcCall *out = buf;
  if (nr > cap && !(out = malloc(sizeof *out * (size_t)nr))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nr; i++)
    if (rows[i].planned && rows[i].operand != NTI_NONE)
      out[n++] = (IcCall){ .name = rows[i].name, .operand = rows[i].operand, .target = rows[i].target };
  if (rows != rb) free(rows);
  *count = n;
  return out;
}
/* does node id itself call `name` (not a method the language calls for it)? */
static int ct_calls(Compiler *c, int id, const char *name) {
  /* Capture asks explicitly; an ordinary call need not enumerate the
     construction names used by entry discovery on every target query. */
  if (nt_kind(c->nt, id) == NK_CallNode) {
    const char *nm = nt_str(c->nt, id, "name");
    /* (a send of a literal name calls that name too, call_node_names) */
    const char *sent = nm && is_send_family(nm) ? call_sent_name(c->nt, id, NULL) : NULL;
    return (nm && sp_streq(nm, name)) || (sent && sp_streq(sent, name));
  }
  const char *nm[2];
  int n = call_node_names(c, id, nm);
  for (int i = 0; i < n; i++) if (nm[i] && sp_streq(nm[i], name)) return 1;
  return 0;
}
/* The method scope of version d of `name` for a call on class rcls (class
   side when sg; main when rcls < 0), as the record answered it: a def's
   or a define_method's scope (rcls's copy of it, when a mixin copied it); for a module_function
   copy, the scope of the def it copies; for an alias, the version of its
   target (a def, an alias or a copy) live where the alias was made, in the
   class whose body made it; for a class-side accessor a later def
   replaces, the method made of it (elab_attr_scope). -1 for any other
   version (a define_method's of a Method or a Proc, which has no scope,
   or its copy, any other attribute's) or an
   alias made where the record has no point. A chain of aliases ends: each
   step asks for the version live at an alias's point, one recorded ahead
   of that point, so the points only go back. */
static int ct_versioned_string_reader(Compiler *c, int cls, int sg, const char *name) {
  return !sg && cls >= 0 && cls == comp_core_class(c, CORE_STRING) && is_string_boxed_reader(name);
}

static int ct_def_version(Compiler *c, int rcls, const char *name, int sg, const LrDef *d, int *dc) {
  int cls = rcls;
  while (d) {
    if (d->kind == LR_DEF || (d->kind == LR_DEFINE_METHOD && d->scope > 0)) return comp_method_version(c, rcls, name, sg, d->node, dc);
    if ((d->kind == LR_READER || d->kind == LR_WRITER || d->kind == LR_ALIAS) && d->scope > 0)
      return comp_method_version(c, rcls, name, sg, c->scopes[d->scope].def_node, dc);
    /* a module_function copy: the module's singleton method made of the def
       it copies (mf_copy_entries), as the load record keeps its scope (the
       named form's too); a define_method's copy is not mapped */
    if (d->kind == LR_COPY && d->scope > 0 && nt_kind(c->nt, c->scopes[d->scope].def_node) == NK_DefNode)
      return comp_method_version(c, rcls, name, sg, c->scopes[d->scope].def_node, dc);
    if (d->kind != LR_ALIAS || !d->target) return -1;
    int at = d->node;
    name = d->target;
    LoadPoint p = elab_point_of(c, at);
    if (p.region < 0) return -1;
    cls = c->node_cbody[at];
    if (elab_method_at(c, cls, name, sg, p, &d) != ELAB_YES || !d ||
        (d->kind != LR_DEF && d->kind != LR_ALIAS && d->kind != LR_COPY)) return -1;
    int mi = call_target(c, at, cls, name, CT_CAPTURE | (cls < 0 ? CT_TOP : sg ? CT_CMETHOD : 0), NULL);
    /* The alias holds the def's own body, which a later prepend's copy
       shadows: the table answers the copy there, which a call of the name
       reaches, and an alias of the name does not */
    int bi = d->kind == LR_DEF && (mi < 0 || c->scopes[mi].def_node != d->node) && cls >= 0
               ? comp_shadowed_body(c, cls, sg, d->node, dc) : -1;
    if (bi >= 0) return bi;
    /* A module's captured body runs as the receiving class's copy. */
    return mi >= 0 ? comp_method_version(c, rcls, c->scopes[mi].name, sg, c->scopes[mi].def_node, dc) : -1;
  }
  return -1;
}
/* The version entry of `name` on rcls (singleton when sg; main when -1)
   with ar parameters (ver_call_arity), or with guard a later prepend's
   guard entry (ct_guard): its index in Compiler.ver_entries,
   -1 for none asked (ver_entries_build); ...and its method, -1 for none
   made */
static unsigned ver_key_hash(int rcls, int sg, const char *name, int ar) {
  return sp_strhash(name) * 31u + (unsigned)(rcls + 1) * 131u + (unsigned)sg * 7u + (unsigned)ar;
}
static int ver_entry_ix(const Compiler *c, int guard, int from, int rcls, int sg, const char *name, int ar, int face) {
  if (!c->ver_ix_nb) return -1;
  for (int i = c->ver_ix[ver_key_hash(rcls, sg, name, ar) % c->ver_ix_nb]; i >= 0; i = c->ver_ix_next[i]) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == guard && e->from == from && e->cls == rcls && e->sg == sg && e->ar == ar && e->face == face && sp_streq(e->name, name)) return i;
  }
  return -1;
}
static int ct_entry_of(const Compiler *c, int guard, int rcls, int sg, const char *name, int ar) {
  int i = ver_entry_ix(c, guard, 0, rcls, sg, name, ar, VF_NONE);
  return i >= 0 ? c->ver_entries[i].scope : -1;
}
static void ver_cell_name(int cell, char out[32]) {
  snprintf(out, 32, "$__spinel_ver_%d", cell + 1);
}
/* ...and the cell of the visibility the live version of that cell has,
   where a face's arm reads it at run time (elab_version_vis -1) */
static void ver_vis_cell_name(int cell, char out[32]) {
  snprintf(out, 32, "$__spinel_vis_%d", cell + 1);
}
/* The name of the cell a later prepend's statement sets (Scope.mixin_guarded) */
void ver_prepend_cell_name(int cell, char out[32]) {
  snprintf(out, 32, "$__spinel_ver_p%d", cell);
}
static int ver_plain_arity(Compiler *c, int s) {   /* -1: not only required positionals */
  const NodeTable *nt = c->nt;
  int def = c->scopes[s].def_node, pn = def >= 0 && nt_kind(nt, def) == NK_DefNode ? nt_ref(nt, def, "parameters") : -2, n = 0;
  const char *arrs[] = { "optionals", "posts", "keywords" };
  if (pn == -1) return 0;
  if (pn == -2 && def >= 0 && nt_kind(nt, def) == NK_CallNode && is_define_method_name(nt_str(nt, def, "name"))) {   /* a define_method's block: its scope's parameters (walk_scope) */
    const Scope *sc = &c->scopes[s];
    return sc->rest_idx < 0 && sc->kwrest_idx < 0 && !sc->blk_param && sc->nrequired == sc->nparams ? sc->nparams : -1;
  }
  if (pn < 0 || nt_ref(nt, pn, "rest") >= 0 || nt_ref(nt, pn, "keyword_rest") >= 0 || nt_ref(nt, pn, "block") >= 0) return -1;
  for (int i = 0; i < 3; i++) {
    nt_arr(nt, pn, arrs[i], &n);
    if (n) return -1;
  }
  nt_arr(nt, pn, "requireds", &n);
  return n;
}
/* Does version s take n plain positional arguments, and no keyword it
   must be given? */
static int ver_takes(Compiler *c, int s, int n) {
  const NodeTable *nt = c->nt;
  int def = c->scopes[s].def_node, pn = def >= 0 && nt_kind(nt, def) == NK_DefNode ? nt_ref(nt, def, "parameters") : -2;
  int rn = 0, on = 0, qn = 0, kn = 0;
  if (pn == -1) return n == 0;
  if (pn < 0 || n < 0) return 0;
  nt_arr(nt, pn, "requireds", &rn);
  nt_arr(nt, pn, "optionals", &on);
  nt_arr(nt, pn, "posts", &qn);
  const int *kw = nt_arr(nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) if (nt_kind(nt, kw[i]) == NK_RequiredKeywordParameterNode) return 0;
  return n >= rn + qn && (nt_ref(nt, pn, "rest") >= 0 || n <= rn + qn + on);
}
/* Does a call of `name` that the table answers with method mi choose among
   versions: mi's own (Scope.versioned), or, where the table reached mi
   through an alias of `name`, the alias's own versions? Two aliases of one
   name, one under a run-time condition, leave the table on the last one
   made, which need not have run; the versions are the name's, not its
   target's, whose scope holds one. */
static int ct_versioned(Compiler *c, int mi, const char *name) {
  const Scope *s = &c->scopes[mi];
  return s->versioned || (name && s->name && !sp_streq(s->name, name) &&
          (elab_method_versions(c, name) > 1 || (is_string_boxed_reader(name) && elab_method_late(c, name))));
}
/* The parameters of the entry call id reaches whose arms call the methods
   to[0..nto): their required ones, when they all take that many and
   nothing else; else the call's plain positional arguments, when every
   one takes that many: each arm's call leaves out what the call leaves
   out, so each method fills its own defaults; else -1, `*r, **k`, which
   hands every method what the call gave */
/* The plain positional arguments call id hands the method, -1 for one that
   hands on what it got (`...`) or makes a Method: all ver_arity reads of
   the call */
static int ver_call_n(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NodeKind kind = nt_kind(nt, id);
  int a = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &ac) : NULL;
  int n = kind == NK_CallNode || kind == NK_SuperNode ? call_plain_argc(c, id) : -1;
  if (kind == NK_CallNode && is_send_family(nt_str(nt, id, "name")) && n > 0) n--;
  if (kind == NK_ForwardingSuperNode) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->rest_idx < 0 && s->kwrest_idx < 0) n = s->nparams;
  }
  if (kind == NK_CallNode && is_method_object_maker(nt_str(nt, id, "name"))) n = -1;
  for (int i = 0; i < ac; i++) if (nt_kind(nt, av[i]) == NK_ForwardingArgumentsNode) n = -1;
  return n;
}
/* Does a Kernel row answer `name` behind an instance-side version of class cls where none has run: a Kernel row's name, in a
   class that has Kernel? A class whose chain the record holds ending at BasicObject, with no Kernel mixed in, has none
   (elab_class_kernel): the else arm there is what the chain finds past the versions, NoMethodError at the last; one that
   gains it where a cell is set (KERN_CELL) has the row there, which the entry's else arm tests (VK_CELL) */
static int ver_kernel_row_behind(Compiler *c, int cls, const char *name) {
  return cls >= 0 && bop_kernel_answers(name, -1, 0) && elab_class_kernel(c, cls) == KERN_HAS;
}
/* See call_plan.h. */
void cplan_kernel_cell_name(int cls, char out[32]) {
  snprintf(out, 32, "$__spinel_kern_%d", cls);
}
/* class cls's Kernel cell, registered as the version cells are: an Integer global the call plan made (ver_cell, which
   cplan_cell_dropped asks), 0 until an include that brings Kernel into cls runs and writes it (cplan_vis_cell_after),
   read by the entries' arms and the guards (VK_CELL, ver_kernel_guard) */
static LocalVar *ver_kern_cell(Compiler *c, int cls) {
  char nm[32];
  cplan_kernel_cell_name(cls, nm);
  LocalVar *g = comp_gvar_intern(c, nm + 1);
  lv_set_type(g, g_promote_mode ? TY_POLY : TY_INT);   /* (as the version cells' Integer is, --int-overflow=promote) */
  g->gvar_seeded = 2;
  g->ver_cell = 1;
  return g;
}
/* a growing list of class ids */
typedef struct { int *v, n, cap; } VerIds;
static int ver_ids_add(VerIds *l, int x) {
  for (int i = 0; i < l->n; i++) if (l->v[i] == x) return 0;
  if (l->n == l->cap && !(l->v = realloc(l->v, sizeof *l->v * (size_t)(l->cap = l->cap ? 2 * l->cap : 8)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  l->v[l->n++] = x;
  return 1;
}
static void ver_kern_add(Compiler *c, int node, int cls) {
  int n = c->nver_kern_after;
  if (!(n & (n - 1)) && !(c->ver_kern_after = realloc(c->ver_kern_after, sizeof *c->ver_kern_after * (size_t)(n ? 2 * n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  c->ver_kern_after[c->nver_kern_after++] = (struct VerKernAfter){ node, cls };
}
/* The include calls that bring Kernel into a class where the record cannot tell whether they ran, each with the
   classes whose cell it writes as it runs (c->ver_kern_after, as ver_vis_after lists the words that write a
   visibility cell): an include of Kernel, or of a module of the program that includes it, that may not run (under a
   condition), and one a class method makes, which the load phase does not run: it writes the cell of the class
   it ran on (the owner, and its blank-slate subclasses, told by _sp_cls). A prepend is not an include (Kernel's
   methods then stand in front of the class's own), and a route the record cannot see (a send by a computed name, an
   eval, a Method) writes none: master's answer stands. Rebuilt when the classes, the scopes or the record's mixins change. */
static void ver_kern_sites(Compiler *c) {
  const NodeTable *nt = c->nt;
  long stamp = (long)c->nclasses * 1000003L + c->nscopes + (long)elab_mixin_gen(c) * 7919L;
  if (c->ver_kern_made && c->ver_kern_stamp == stamp) return;
  c->ver_kern_made = 1;
  c->ver_kern_stamp = stamp;
  c->nver_kern_after = 0;
  for (int k = 0; k < c->nclasses; k++) {
    int nm = 0;
    const LrMixin *mx = elab_class_mixins(c, k, &nm);
    for (int j = 0; j < nm; j++) {
      /* (an include call, or a send of :include, which a pass later makes one) */
      const char *cn = nt_kind(nt, mx[j].node) == NK_CallNode ? call_sent_name(nt, mx[j].node, NULL) : NULL;
      if (mx[j].kind == LR_INCLUDE && cn && is_mixin_call(cn) && elab_mixin_may_not_run(&mx[j]) &&
          elab_mixin_certain_kernel(c, mx[j].ref) == 1)
        ver_kern_add(c, mx[j].node, k);
    }
  }
  int *par = NULL;
  for (int id = an_calls_named_first(c, "include"); id >= 0; id = an_calls_named_next(id)) {
    int an = nt_ref(nt, id, "arguments"), ac = 0, recv = nt_ref(nt, id, "receiver"), brings = 0;
    const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
    for (int i = 0; i < ac; i++) brings |= elab_mixin_certain_kernel(c, av[i]) == 1;
    const Scope *s = comp_scope_of(c, id);
    if (!brings || (recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) || elab_point_of(c, id).region >= 0 || !s || !s->is_cmethod || s->class_id < 0) continue;
    int own = s->class_id, nd = 0;
    if (!par) par = du_parent_map(nt);
    for (int a = id; own >= 0 && a >= 0 && par[a] >= 0 && a != s->def_node; a = par[a])
      if (nt_kind(nt, a) == NK_BlockNode || nt_kind(nt, a) == NK_LambdaNode) own = -1;   /* (a block's self is another) */
    if (own < 0 || !(class_is_blank_slate(c, own) || comp_class_is_module(c, &c->classes[own]))) continue;
    ver_kern_add(c, id, own);
    const int *ds = comp_descendants(c, own, &nd);
    for (int i = 0; i < nd; i++) if (class_is_blank_slate(c, ds[i])) ver_kern_add(c, id, ds[i]);
  }
  free(par);
  free(c->ver_kern_has);
  if (!(c->ver_kern_has = calloc((size_t)c->nclasses + 1, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < c->nver_kern_after; i++) c->ver_kern_has[c->ver_kern_after[i].cls] = 1;
}
/* ...the classes on cls's chain (its superclasses, and the modules they and it include, prepend or include in turn)
   that an include of the list writes the cell of: the cells the class's Kernel is read from */
static void ver_kern_walk(Compiler *c, int k, VerIds *seen, VerIds *out, int depth) {
  if (k < 0 || depth > c->nclasses || !ver_ids_add(seen, k)) return;   /* (each class once: no chain is deeper than the classes) */
  if (k < c->nclasses && c->ver_kern_has[k]) ver_ids_add(out, k);
  int nm = 0;
  const LrMixin *mx = elab_class_mixins(c, k, &nm);
  for (int j = 0; j < nm; j++)
    if ((mx[j].kind == LR_INCLUDE || mx[j].kind == LR_PREPEND) && !elab_mixin_may_not_run(&mx[j]))   /* (an include that may not run brings Kernel only where it is a site of its own: ver_kern_sites) */
      ver_kern_walk(c, elab_mixin_module(c, &mx[j]), seen, out, depth + 1);
  int p = c->classes[k].parent;
  ver_kern_walk(c, p >= 0 ? p : elab_class_super(c, k), seen, out, depth + 1);
}
/* The classes whose Kernel cell class cls reads, in *out (the caller frees ids->v); how many */
static int ver_kern_cells(Compiler *c, int cls, VerIds *out) {
  VerIds seen = { NULL, 0, 0 };
  *out = (VerIds){ NULL, 0, 0 };
  ver_kern_sites(c);
  if (c->nver_kern_after) ver_kern_walk(c, cls, &seen, out, 0);
  free(seen.v);
  return out->n;
}
/* See call_plan.h. */
int cplan_class_kernel(Compiler *c, int cls) {
  int r = elab_class_kernel(c, cls);
  if (r != KERN_LACKS) return r;   /* (a chain not rooted at BasicObject pays nothing more) */
  VerIds cells;
  r = ver_kern_cells(c, cls, &cells) ? KERN_CELL : KERN_LACKS;
  free(cells.v);
  return r;
}
/* A call whose answer is Kernel's where a cell of its class's chain is set, and NoMethodError where none is
   (KERN_CELL): a `super` that reaches no method of the program's, and a receiverless call that binds a Kernel row (no
   method of the program's answers it on the class: a version entry has the cells' test in its else arm already). Its
   node is replaced by `if <cells> then <call> else <raise> end`, the call as a class that has Kernel binds it
   (cplan_kernel_reaches, comp_super_parent), the raise the one an entry for the name gives (ver_no_method_raise), which
   does not run the call's arguments. */
static int ver_no_method_raise(Compiler *c, int cls, int sg, const char *name, int ar, int vcall);
static int cplan_mm_method(Compiler *c, int id, int ci, int cmethod);
/* `<cell> == 1 || ...` over the Kernel cells *cells lists */
static int ver_kern_test(Compiler *c, const VerIds *cells) {
  DispArm *arm = malloc(sizeof *arm * (size_t)cells->n);
  char (*sel)[32] = malloc(32 * (size_t)cells->n);
  if (!arm || !sel) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < cells->n; k++) {
    cplan_kernel_cell_name(cells->v[k], sel[k]);
    (void)ver_kern_cell(c, cells->v[k]);
    arm[k] = (DispArm){ sel[k], 1, NULL, 0, 0 };
  }
  int test = disp_cell_any((NodeTable *)c->nt, arm, cells->n);
  free(arm); free(sel);
  return test;
}
/* What a Kernel guard asks of the classes below the holder cls: the call id of name (how: 2 for a super), the ncells
   cells cls's chain reads, cls's method_missing mm, and the call's count of arguments (ver_kern_arity) and its block */
typedef struct { int id, cls, ncells, mm, how, ar, blk; const char *name; } VerKernSelf;
/* For each class D below x->cls (comp_descendants' order), pick's answer in *how (the caller frees it): >= 0 for an
   arm, -1 for none (a singleton class has none); how many have one. The record is asked here, before any node of
   the arms is made: a node made moves the load points its next question reads (an_elab_points) */
static int ver_kern_pick(Compiler *c, const VerKernSelf *x, int (*pick)(Compiler *c, const VerKernSelf *x, int d), int **how) {
  int nd = 0, n = 0;
  const int *ds = comp_descendants(c, x->cls, &nd);
  if (!(*how = malloc(sizeof **how * (size_t)(nd > 0 ? nd : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nd; i++) n += ((*how)[i] = c->classes[ds[i]].is_singleton_of ? -1 : pick(c, x, ds[i])) >= 0;
  return n;
}
/* `case when self.instance_of?(D) then <arm(D, how)> ... else els end`, with an arm for each class D below x->cls
   that how (ver_kern_pick) gives one */
static int ver_kern_by_self(Compiler *c, const VerKernSelf *x, const int *how, int (*arm)(Compiler *c, const VerKernSelf *x, int d, int how), int els) {
  int nd = 0, n = 0;
  const int *ds = comp_descendants(c, x->cls, &nd);
  int *is = malloc(sizeof *is * (size_t)(nd > 0 ? nd : 1)), *then = malloc(sizeof *then * (size_t)(nd > 0 ? nd : 1));
  if (!is || !then) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < nd; i++)
    if (how[i] >= 0 && (is[n] = disp_self_class_is(c, ds[i])) >= 0) then[n++] = arm(c, x, ds[i], how[i]);
  if (n) els = disp_case((NodeTable *)c->nt, is, then, n, els);
  free(is); free(then);
  return els;
}
/* Method scope mi, which D's table finds for name below the holder cls (comp_method_in_chain, cplan_mm_method), where
   the record says a call of name on a D runs it from D's first object on (elab_runs_below): the def it was made of, a
   module's an includer's copy shares, the original an alias names, or for a version entry the versions its arms call
   (ver_entry_targets); -1 where the lookup ends at an undef ahead of it (a module's too), where a call may run ahead
   of what binds the name (a def, an alias, an undef, a mixin), and where the record finds another */
static int ver_kern_found(Compiler *c, int d, int cls, const char *name, int mi) {
  if (mi < 0) return -1;
  int to[VER_MAX_ARMS + 1], nodes[VER_MAX_ARMS + 1], n = 0, entry = c->scopes[mi].entry == ENTRY_DISPATCH || c->scopes[mi].entry == ENTRY_NAMED;
  if (!entry) nodes[n++] = c->scopes[mi].def_node;
  for (int k = 0, nt = entry ? ver_entry_targets(c, mi, to, NULL) : 0; k < nt; k++)
    if (to[k] >= 0 && to[k] != mi && c->scopes[to[k]].class_id == c->scopes[mi].class_id) nodes[n++] = c->scopes[to[k]].def_node;
  return elab_runs_below(c, d, cls, name, nodes, n, entry) == ELAB_YES ? mi : -1;
}
/* Does a call of x->name on a D find a method of its own, below x->cls, which the holder's chain does not hold: CRuby
   calls it, whatever Kernel says (not a super, which looks above the holder's method; not one behind an undef of the
   name, where the lookup ends, nor one a call may run ahead of: ver_kern_found) */
static int ver_kern_own(Compiler *c, const VerKernSelf *x, int d) {
  int own = -1;
  if (x->how != 2) {
    mtab_enter(c);   /* (entry construction: the table's method D finds for the guard's name, which the record's walk below checks) */
    int found = comp_method_in_chain(c, d, x->name, NULL);
    mtab_leave(c);
    own = ver_kern_found(c, d, x->cls, x->name, found);
  }
  int oc = own >= 0 ? c->scopes[own].class_id : -1;
  return oc >= 0 && oc != x->cls && is_descendant(c, oc, x->cls) && is_descendant(c, d, oc);
}
/* How the test of self's Kernel answers where self is a D (ver_kern_by_self; the holder's own is the else): 1 true,
   where D finds a method of its own (ver_kern_own), whose call the guard's then arm makes, or where D surely has Kernel
   (elab_class_kernel: an include in its body, or in a module it includes, that surely runs); 0 the test of the cells
   D's chain reads (ver_kern_cells), cls's among them, where it reads more: a class method's include run on D, an
   include under a condition in D's body or a module's, a module of D's with a cell of its own; -1 none, where D reads
   what cls reads */
static int ver_kern_test_pick(Compiler *c, const VerKernSelf *x, int d) {
  if (ver_kern_own(c, x, d) || elab_class_kernel(c, d) == KERN_HAS) return 1;
  VerIds dc;
  int more = ver_kern_cells(c, d, &dc) > x->ncells;
  free(dc.v);
  return more ? 0 : -1;
}
/* ...its arm for a D: true, or the test of D's cells */
static int ver_kern_test_arm(Compiler *c, const VerKernSelf *x, int d, int how) {
  if (how) return nt_new_node((NodeTable *)c->nt, "TrueNode");
  VerIds dc;
  (void)ver_kern_cells(c, d, &dc);
  int t = ver_kern_test(c, &dc);
  free(dc.v);
  return t;
}
/* `callee(<call id's arguments>)` with the call's block, a copy of each (the call's own run only on the other arm),
   `:sym` ahead of them where sym (a method_missing's) */
static int ver_kern_fwd(Compiler *c, int id, const char *callee, const char *sym) {
  NodeTable *nt = (NodeTable *)c->nt;
  int an = nt_ref(nt, id, "arguments"), nargs = 0, k0 = sym != NULL;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &nargs) : NULL;
  int call = disp_forward_call(nt, callee, 0, 0), margs = k0 + nargs ? nt_new_node(nt, "ArgumentsNode") : -1;
  int *mv = malloc(sizeof *mv * (size_t)(nargs + 1));
  if (!mv) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (sym) {
    comp_sym_intern(c, sym);
    nt_node_set_str(nt, mv[0] = nt_new_node(nt, "SymbolNode"), "value", sym);
  }
  for (int k = 0; k < nargs; k++) mv[k0 + k] = nt_clone_subtree(nt, av[k]);
  if (margs >= 0) {
    nt_node_set_arr(nt, margs, "arguments", mv, k0 + nargs);
    nt_node_set_ref(nt, call, "arguments", margs);
  }
  if (nt_ref(nt, id, "block") >= 0) nt_node_set_ref(nt, call, "block", nt_clone_subtree(nt, nt_ref(nt, id, "block")));   /* (the call's block goes with it) */
  free(mv);
  return call;
}
/* The call's count of arguments, as the guard's else helper takes them (disp_def_build's ar): -1 where one is a splat,
   keywords or `...`, which `*r, **k` take */
static int ver_kern_arity(const NodeTable *nt, const int *av, int n) {
  for (int k = 0; k < n; k++) {
    NodeKind ak = nt_kind(nt, av[k]);
    if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_ForwardingArgumentsNode) return -1;
  }
  return n;
}
static void ver_pin(Compiler *c, int node, int scope, int vis);
/* D's own method_missing, which the guard's else calls where no cell is set and self is a D: another than the
   holder's, as the holder's chain does not hold it; -1 where D finds a method of its own (the test's arm is true
   there), and where the record does not say a call on a D runs that method_missing from D's first object on
   (ver_kern_found: an undef of it ahead, a def or a mixin a call may run ahead of) */
static int ver_kern_mm_pick(Compiler *c, const VerKernSelf *x, int d) {
  int dm = ver_kern_found(c, d, x->cls, METHOD_MISSING_NAME, cplan_mm_method(c, -1, d, 0));
  return dm < 0 || dm == x->mm || ver_kern_own(c, x, d) ? -1 : dm;
}
/* ...its arm in the else helper (ver_kern_by_self; the holder's method_missing or the raise is the else):
   `method_missing(:name, <the helper's parameters>, &blk)`, pinned to D's, dm (ver_pin) */
static int ver_kern_mm_arm(Compiler *c, const VerKernSelf *x, int d, int dm) {
  int call = disp_method_missing_call((NodeTable *)c->nt, x->name, x->ar, x->blk);
  ver_pin(c, call, dm, 0);
  return call;
}
/* A Kernel guard's def on its holder (ver_kern_help): kind 0 the test, 1 the else; scope -1 where no class below has
   an arm */
typedef struct { int kind, cls, how, ar, blk, scope; const char *name; } VerKernHelp;
typedef struct { VerKernHelp *v; int n, cap; } VerKernHelps;
/* The holder's def that answers x's guard for the classes below it, made once per (holder, name, how, count, block)
   and kept in *hs (ver_kernel_guards' own): kind 0 the test, a def of no parameters whose body is the `case` over them
   (ver_kern_test_arm), the test of cls's cells its else; kind 1 the else, which takes the call's operands and block
   as disp_def_build gives them, its arms calling each D's own method_missing (ver_kern_mm_arm), its else cls's
   method_missing or the raise. It is walked as a method of cls (synth_def_scope), reached where the call's method is,
   and the site calls it pinned to it, its operands evaluated as its arguments. A call of it runs the `case` as an
   if-chain, an instance_of? test per class with an arm until one holds. -1 where no class below has an arm: the site
   builds the guard in place. */
static int ver_kern_help(Compiler *c, VerKernHelps *hs, const VerKernSelf *x, int kind, const VerIds *cells) {
  int how = kind ? x->how : x->how == 2, ar = kind ? x->ar : 0, blk = kind ? x->blk : 0, scope = -1;
  const Scope *site = comp_scope_of(c, x->id);
  int run = site && site->reachable;
  for (int i = 0; i < hs->n; i++) {
    const VerKernHelp *h = &hs->v[i];
    if (h->kind != kind || h->cls != x->cls || h->how != how || h->ar != ar || h->blk != blk || !sp_streq(h->name, x->name)) continue;
    if (run && h->scope >= 0 && !c->scopes[h->scope].reachable) { c->scopes[h->scope].reachable = 1; comp_touch_scope(c->scopes[h->scope].idx); }
    return h->scope;
  }
  int *picks = NULL;
  if (ver_kern_pick(c, x, kind ? ver_kern_mm_pick : ver_kern_test_pick, &picks)) {
    NodeTable *nt = (NodeTable *)c->nt;
    size_t hl = strlen(x->name) + 48;
    char *hn = malloc(hl);
    if (!hn) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    /* (the space: a name no def of the program has, name_is_synth_method) */
    if (kind) snprintf(hn, hl, "__kern else %d %d %d %s", how, ar, blk, x->name);
    else snprintf(hn, hl, "__kern %s %s", how ? "super" : "test", x->name);
    int body;
    if (kind) {
      comp_sym_intern(c, x->name);
      body = ver_kern_by_self(c, x, picks, ver_kern_mm_arm, x->mm >= 0 ? disp_method_missing_call(nt, x->name, ar, blk) : ver_no_method_raise(c, x->cls, 0, x->name, 0, x->how));
    }
    else body = ver_kern_by_self(c, x, picks, ver_kern_test_arm, ver_kern_test(c, cells));
    scope = synth_def_scope(c, x->cls, hn, disp_def_build(nt, -1, hn, 0, ar, blk, NULL, 0, body), run);
    free(hn);
    if (kind && blk) {
      /* (it forwards its block: spliced where every method_missing it calls lets a spliced block through, as a
         version entry's arms, ver_entry_emit) */
      int spliced = x->mm < 0 || c->scopes[x->mm].yields || !c->scopes[x->mm].blk_param, nd = 0;
      (void)comp_descendants(c, x->cls, &nd);
      for (int i = 0; i < nd; i++) if (picks[i] >= 0) spliced &= c->scopes[picks[i]].yields || !c->scopes[picks[i]].blk_param;
      c->scopes[scope].yields = spliced;
      comp_touch_scope(c->scopes[scope].idx);
    }
  }
  free(picks);
  if (hs->n == hs->cap && !(hs->v = realloc(hs->v, sizeof *hs->v * (size_t)(hs->cap = hs->cap ? 2 * hs->cap : 8)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  hs->v[hs->n++] = (VerKernHelp){ kind, x->cls, how, ar, blk, scope, x->name };
  return scope;
}
static void ver_kernel_guard(Compiler *c, VerKernHelps *hs, int id, int cls, const char *name, int how, const int *pm) {
  NodeTable *nt = (NodeTable *)c->nt;
  int par = pm ? pm[id] : -1;
  VerIds cells;
  if (!ver_kern_cells(c, cls, &cells) || par < 0) { free(cells.v); return; }
  int an = nt_ref(nt, id, "arguments"), nargs = 0, els, mm = how != 2 ? cplan_mm_method(c, -1, cls, 0) : -1;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &nargs) : NULL;
  VerKernSelf x = { id, cls, cells.n, mm, how, ver_kern_arity(nt, av, nargs), nt_ref(nt, id, "block") >= 0, name };
  /* (the helpers first: their nodes are their own scope's, the guard's the call's) */
  int es = how != 2 ? ver_kern_help(c, hs, &x, 1, &cells) : -1, ts = ver_kern_help(c, hs, &x, 0, &cells), base = nt->count;
  if (es >= 0) ver_pin(c, els = ver_kern_fwd(c, id, c->scopes[es].name, NULL), es, 0);
  else if (mm >= 0) els = ver_kern_fwd(c, id, METHOD_MISSING_NAME, name);   /* (a class with a method_missing of its own answers the call where no cell is set) */
  else {
    /* the call's operands that run code are evaluated, as CRuby evaluates them before it finds no method, ahead of the raise
       (a copy of each: the call's own run only on the other arm) */
    int raise = ver_no_method_raise(c, cls, 0, name, 0, how), nops = 0;
    int *ops = malloc(sizeof *ops * (size_t)(nargs + 1));
    if (!ops) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < nargs; k++) if (!order_operand_inert(c, id, av[k], 0)) ops[nops++] = nt_clone_subtree(nt, av[k]);
    els = nops ? disp_ahead(nt, ops, nops, raise) : raise;
    free(ops);
  }
  int test;
  if (ts >= 0) ver_pin(c, test = disp_forward_call(nt, c->scopes[ts].name, 0, 0), ts, 0);
  else test = ver_kern_test(c, &cells);
  int ifn = disp_choose(nt, test, id, els);
  nt_replace_child(nt, par, id, ifn);
  comp_grow_node_arrays(c);
  for (int j = base; j < nt->count; j++) c->nscope[j] = c->nscope[id];
  nt_node_set_int(nt, ifn, "node_line", (int)nt_int(nt, id, "node_line", 0));
  free(cells.v);
}
/* ...for each such call of the program, the helpers that answer for the classes below a holder made once (VerKernHelps) */
static void ver_kernel_guards(Compiler *c) {
  const NodeTable *nt = c->nt;
  int n0 = nt->count;
  ver_kern_sites(c);
  if (!c->nver_kern_after) return;
  int *pm = du_parent_map(nt);   /* (of the nodes the guards leave where they stand) */
  VerKernHelps hs = { NULL, 0, 0 };
  for (int id = 0; id < n0; id++) {
    NodeKind k = nt_kind(nt, id);
    const Scope *s = k == NK_SuperNode || k == NK_ForwardingSuperNode || k == NK_CallNode ? comp_scope_of(c, id) : NULL;
    if (!s || s->class_id < 0 || s->is_cmethod || !s->name || s->entry != ENTRY_NONE || nt_int(nt, id, "kernel_recv", 0) ||
        cplan_class_kernel(c, s->class_id) != KERN_CELL) continue;   /* (an entry's arms, and the raise they hold, are the plan's own) */
    if (k == NK_CallNode) {
      const char *nm = nt_str(nt, id, "name");
      if (cplan_dispatch_row(c, id) && call_target(c, id, s->class_id, nm, 0, NULL) < 0) ver_kernel_guard(c, &hs, id, s->class_id, nm, nt_vcall(nt, id), pm);
    }
    else if (cplan_super_target(c, id, s, NULL) < 0) ver_kernel_guard(c, &hs, id, s->class_id, comp_scope_user_name(s), 2, pm);
  }
  free(hs.v);
  free(pm);
}
/* See call_plan.h. */
int cplan_kern_site_by_class(Compiler *c, int scope) {
  for (int i = 0; i < c->nver_kern_after; i++) {
    int n = 0;
    if (c->nscope[c->ver_kern_after[i].node] != scope) continue;
    for (int j = 0; j < c->nver_kern_after; j++) n += c->ver_kern_after[j].node == c->ver_kern_after[i].node;
    if (n > 1) return 1;
  }
  return 0;
}
/* Is scope s a version held under a condition (a lone def, cond_lone) of an instance-side name a Kernel row answers behind
   it (ver_kernel_row_behind)? Its entry's else arm is the row, which takes the call's count itself, whether or not the
   version takes it: the entry is at the call's count, and each arm's call raises the ArgumentError its own count meets */
static int ver_held_row(Compiler *c, const Scope *s) {
  return s->def_node >= 0 && !s->is_cmethod && nt_int(c->nt, s->def_node, "cond_lone", 0) && ver_kernel_row_behind(c, s->class_id, s->name);
}
static int ver_arity(Compiler *c, int id, const int *to, int nto) {
  int n = ver_call_n(c, id);
  int a = nt_ref(c->nt, id, "arguments"), ac = 0;
  const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &ac) : NULL;
  /* not the versions' required ones: a boxed receiver's arm cannot gather a splat into __cond0.. (arg_layout_gathers) */
  if (nt_argv_has(c->nt, av, ac, NT_ARG_SPLAT)) return -1;
  int ar = ver_plain_arity(c, to[0]), fits = 1, held = 0;
  for (int i = 0; i < nto; i++) {
    if (ver_plain_arity(c, to[i]) != ar) ar = -1;
    fits &= ver_takes(c, to[i], n);
    held |= ver_held_row(c, &c->scopes[to[i]]);
  }
  return ar >= 0 ? ar : fits || (held && n >= 0) ? n : -1;
}
/* ...for a version entry of `name` on rcls (mi, the table's answer): the
   versions an entry arms (ver_entry_make) */
static int ver_call_arity(Compiler *c, int id, int rcls, int sg, const char *name, int mi) {
  /* Kernel.String's argument belongs to the outer call. Both conversion
     callbacks receive zero arguments, even if their versions take others. */
  if (!sg && kernel_string_implies(c, id, rcls, name, CT_IMPLIED)) return 0;
  if (ct_versioned_string_reader(c, rcls, sg, name)) {
    int ar = ver_call_n(c, id);
    if (ar >= 0) return ar;
  }
  ElabVersion vs[VER_MAX_ARMS];
  int to[VER_MAX_ARMS + 1], nto = 0, dc;
  int nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS);
  to[nto++] = mi;
  for (int i = 0; i < nv; i++) {
    /* (an undef's arm raises for any arguments, a remove has none) */
    int s = vs[i].v->kind == LR_REMOVE || vs[i].v->kind == LR_UNDEF ? -1 : ct_def_version(c, rcls, name, sg, vs[i].v, &dc);
    if (s >= 0) to[nto++] = s;
  }
  return ver_arity(c, id, to, nto);
}
/* guard_chain's answer kept for mi (Compiler.guard_memo), into to: their
   count, 0 for none kept */
static int guard_memo_get(const struct GuardMemo *g, int mi, int *to) {
  if (mi >= g->nat || !g->at[mi]) return 0;
  const int *p = g->to + g->at[mi] - 1;
  memcpy(to, p + 1, sizeof *to * (size_t)p[0]);
  return p[0];
}
static void guard_memo_put(struct GuardMemo *g, int mi, const int *to, int n) {
  if (mi >= g->nat) {
    int nat = g->nat ? g->nat : 64, *at;
    while (nat <= mi) nat *= 2;
    at = realloc(g->at, sizeof *at * (size_t)nat);
    if (!at) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(at + g->nat, 0, sizeof *at * (size_t)(nat - g->nat));
    g->at = at; g->nat = nat;
  }
  if (g->nto + n + 1 > g->cto && !(g->to = realloc(g->to, sizeof *g->to * (size_t)(g->cto = 2 * (g->nto + n + 1))))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  g->at[mi] = g->nto + 1;
  g->to[g->nto++] = n;
  memcpy(g->to + g->nto, to, sizeof *to * (size_t)n);
  g->nto += n;
}
/* The methods a guard entry chooses from for mi, a later prepend's copy
   (Scope.mixin_guarded): mi, each guarded copy it stands in front of, and
   the method behind the last. Kept once the entries are made, when the
   scopes no longer change (as ct_version's answers are) */
static int guard_chain(Compiler *c, int mi, int *to, DispArm *arm, char sel[][32]) {
  int memo = !arm && c->ver_built, asked = mi, n = memo ? guard_memo_get(&c->guard_memo, mi, to) : 0;
  if (n) return n;
  if (c->scopes[mi].entry == ENTRY_TABLE) mi = c->scopes[mi].guard_behind;
  while (n < VER_MAX_ARMS && c->scopes[mi].mixin_guarded && c->scopes[mi].guard_behind >= 0) {
    if (arm) {
      ver_prepend_cell_name(c->scopes[mi].mixin_guarded, sel[n]);
      int value = c->scopes[mi].mixin_guard_value;
      arm[n] = (DispArm){ sel[n], value ? value : 1, c->scopes[mi].name, 0 };
    }
    to[n++] = mi;
    mi = c->scopes[mi].guard_behind;
  }
  /* A capture before the prepend can also precede a redefinition of the
     method behind it. Its replaced definitions use the same version
     cells as an ordinary version entry, after the prepend arms. */
  const Scope *m = &c->scopes[mi];
  const char *name = m->def_node >= 0 ? nt_str(c->nt, m->def_node, "name") : NULL;
  if (name && m->entry != ENTRY_NAMED && ct_version_named(c, name)) {
    ElabVersion vs[VER_MAX_ARMS];
    int nv = elab_chain_versions(c, m->class_id, m->is_cmethod, name, vs, VER_MAX_ARMS), dc;
    for (int i = 0; i < nv && n < VER_MAX_ARMS; i++) {
      if (vs[i].v->kind != LR_DEF) continue;
      int s = ct_def_version(c, m->class_id, name, m->is_cmethod, vs[i].v, &dc);
      if (s < 0 || !c->scopes[s].replaced || c->scopes[s].class_id != m->class_id || c->scopes[s].is_transplanted_source) continue;
      /* Guarded copies belong to the prepend arms above, never to the
         pre-prepend fallback selected by a module definition's cell. */
      if (c->scopes[s].mixin_guarded) continue;
      if (arm) {
        ver_cell_name(vs[i].cell, sel[n]);
        arm[n] = (DispArm){ sel[n], vs[i].n, name, vs[i].cell + 1 };
      }
      to[n++] = s;
    }
  }
  to[n++] = mi;
  if (memo) guard_memo_put(&c->guard_memo, asked, to, n);
  return n;
}
static int mixin_copy_versions(Compiler *c, int mi, int from, int *to, ElabVersion *vs, int one);
static int ver_closed_to(int vis, int face);
/* Has class k prepended module (also through another module's include) for every call: in its first body,
   unconditionally? A conditional prepend or a later reopening's may run after a call */
static int class_prepends_first(Compiler *c, int k, int module) {
  int nm = 0, nb = 0;
  const LrMixin *mix = elab_class_mixins(c, k, &nm);
  const LrOpen *bodies = nm ? elab_class_bodies(c, k, &nb) : NULL;
  for (int i = 0; nb && i < nm; i++)
    if (mix[i].kind == LR_PREPEND && mix[i].open == bodies[0].node && mix[i].applied && !mix[i].cond &&
        !elab_point_may(c, mix[i].node) && comp_mixes_in(c, elab_mixin_module(c, &mix[i]), module, 0)) return 1;
  return 0;
}
/* Does class k or a superclass mix in module (also through another module's include): an include, and a prepend
   that some call may follow (every 0: any of the class's) or one every call follows (every 1: class_prepends_first) */
static int chain_mixes_in(Compiler *c, int k, int module, int every) {
  for (int d = 0; k >= 0 && k < c->nclasses && d <= c->nclasses; k = c->classes[k].parent, d++) {
    const ClassInfo *ki = &c->classes[k];
    for (int i = 0; i < ki->nincluded_mods; i++)
      if (comp_mixes_in(c, ki->included_mods[i], module, 0)) return 1;
    for (int i = 0; !every && i < ki->nprepended_mods; i++)
      if (comp_mixes_in(c, ki->prepended_mods[i], module, 0)) return 1;
    if (every && class_prepends_first(c, k, module)) return 1;
  }
  return 0;
}
int cplan_caller_includes_module(Compiler *c, int caller, int module) {
  return chain_mixes_in(c, caller, module, 1);
}
int cplan_protected_owner_open(Compiler *c, int caller, int singleton, int owner) {
  if (caller < 0 || owner < 0) return 0;
  if (singleton) return comp_class_singleton_has_module(c, caller, owner);
  return is_descendant(c, caller, owner) || cplan_caller_includes_module(c, caller, owner);
}
static Scope *guard_body(Compiler *c, int scope) {
  Scope *s = &c->scopes[scope];
  if (!s->super_behind) return s;
  int inherited = cplan_super_target(c, -1, s, NULL);
  return inherited >= 0 ? &c->scopes[inherited] : s;
}
static int guard_visibility(Compiler *c, int scope) {
  Scope *s = guard_body(c, scope);
  if (s->super_behind && object_private_method_name(comp_scope_user_name(s))) return SP_VIS_PRIVATE;
  int owner = s->origin_module_ci - 1;
  if (owner >= 0) return comp_method_vis(&c->classes[owner], comp_scope_user_name(s));
  return s->class_id >= 0 ? comp_method_vis(&c->classes[s->class_id], s->name) : SP_VIS_PUBLIC;
}
/* ...and that of a module's version a guarded copy runs (guard_body): -1 where only the run tells it (elab_version_vis:
   a word in a body), the cell of the version's history in *cell, whose visibility cell the word writes; else the
   table's (guard_visibility) */
static int guard_version_vis(Compiler *c, int scope, int *cell) {
  const Scope *s = guard_body(c, scope);
  ElabVersion vs[VER_MAX_ARMS];
  int nv = s->origin_module_ci > 0 && s->def_node >= 0
         ? elab_history_versions(c, s->origin_module_ci - 1, s->is_extend_copy ? 0 : s->is_cmethod, comp_scope_user_name(s), vs, VER_MAX_ARMS) : 0;
  for (int i = 0; i < nv; i++)
    if (vs[i].v->kind == LR_DEF && vs[i].v->node == s->def_node) {
      if (elab_version_vis(c, &vs[i]) >= 0) break;
      *cell = vs[i].cell;
      return -1;
    }
  return guard_visibility(c, scope);
}
static int guard_is_conditional(Compiler *c, const int *to, int n) {
  for (int k = 0; k < n; k++) if (c->scopes[to[k]].mixin_conditional) return 1;
  return 0;
}
/* ...whether an arm of to[] takes a visibility only the run tells (guard_version_vis) */
static int guard_vis_runs(Compiler *c, const int *to, int n) {
  for (int k = 0, cell; k < n - 1; k++) if (guard_version_vis(c, to[k], &cell) < 0) return 1;
  return 0;
}
static int guard_arity(Compiler *c, int id, const int *to, int n) {
  return guard_is_conditional(c, to, n) ? ver_call_n(c, id) : ver_arity(c, id, to, n);
}
enum { VER_VIS_PRESENT = 1, VER_VIS_STRING_HISTORY = 2 };
/* A hidden String history whose empty arm is the public builtin. */
static const struct VerEntry *guard_history_of(const Compiler *c, int scope) {
  if (c->scopes[scope].entry != ENTRY_NAMED) return NULL;
  for (int i = 0; i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->scope == scope && e->guard == VER_ENTRY_HISTORY && e->vis == VER_VIS_STRING_HISTORY) return e;
  }
  return NULL;
}
static int guard_history_arms(Compiler *c, const struct VerEntry *e, ElabVersion *out, int *targets) {
  ElabVersion versions[VER_MAX_ARMS];
  int to[VER_MAX_ARMS + 1], tests[VER_MAX_ARMS + 1], n = 0;
  int nv = elab_history_versions(c, e->cls, e->sg, e->name, versions, VER_MAX_ARMS);
  int nt = ver_entry_targets(c, e->scope, to, tests);
  for (int i = 0; i < nt - 1; i++) {
    int argc = 0, cell = (int)nt_int(c->nt, tests[i], "ver_hist", 0);
    const int *args = call_args(c->nt, tests[i], &argc);
    if (!cell || argc != 1) continue;
    int value = (int)nt_int(c->nt, args[0], "value", 0);
    for (int j = 0; j < nv; j++) if (versions[j].cell + 1 == cell && versions[j].n == value) {
      if (to[i] < 0 && versions[j].v->kind != LR_UNDEF) break;
      out[n] = versions[j];
      if (targets) targets[n] = to[i];
      n++;
      break;
    }
  }
  return n;
}
static int guard_needs_face(Compiler *c, const int *to, int n) {
  if (guard_vis_runs(c, to, n)) return 1;
  if (!guard_is_conditional(c, to, n)) return 0;
  const struct VerEntry *history = guard_history_of(c, to[n - 1]);
  for (int k = 0; k < n - (history != NULL); k++) if (guard_visibility(c, to[k]) != SP_VIS_PUBLIC) return 1;
  if (history) {
    ElabVersion vs[VER_MAX_ARMS];
    int nv = guard_history_arms(c, history, vs, NULL);
    for (int k = 0; k < nv; k++)
      if (vs[k].v->kind != LR_UNDEF && elab_physical_version_vis(c, &vs[k]) != SP_VIS_PUBLIC) return 1;
  }
  return 0;
}
/* A guarded chain can hold protected bodies of different owners. Share an
   entry among calls with the same caller class and side. Ordinary faces
   still serve a caller with no protected access and explicit public_send. */
enum { VF_GUARD_CALLER = VF_VCALL + 1 };
static int guard_call_face(Compiler *c, int id, const int *to, int n) {
  const char *name = nt_str(c->nt, id, "name");
  if (id < 0 || nt_str(c->nt, id, "vis_enforce") || (name && is_public_send(name))) return VF_PUBLIC;
  const Scope *caller = comp_scope_of(c, id);
  if (caller && caller->class_id >= 0) {
    for (int k = 0, cell, v; k < n; k++)
      if ((v = guard_version_vis(c, to[k], &cell)) == SP_VIS_PROTECTED || (v < 0 && elab_method_may_be_protected(c, name)) || guard_history_of(c, to[k]))
        return VF_GUARD_CALLER + 2 * caller->class_id + (caller->is_cmethod != 0);
  }
  return VF_PUBLIC;
}
static int guard_arm_face(Compiler *c, int scope, int face) {
  if (face < VF_GUARD_CALLER) return face;
  int caller = (face - VF_GUARD_CALLER) / 2, singleton = (face - VF_GUARD_CALLER) % 2;
  const Scope *method = guard_body(c, scope);
  int module = method->origin_module_ci - 1;
  int owner = module >= 0 ? module : method->class_id;
  /* If a protected prepend on this class is live, self of that class
     or one of its subclasses also has the module (and a class that
     includes or prepends it: cplan_protected_owner_open). */
  return cplan_protected_owner_open(c, caller, singleton, owner) ||
         (!singleton && module >= 0 && is_descendant(c, caller, method->class_id))
         ? VF_PRIVATE : VF_PUBLIC;
}

static int conditional_guard_method(Compiler *c, int cls, const char *name) {
  if (!c->n_prepend_cells || cls < 0 || !name) return -1;
  int mi = call_target(c, -1, cls, name, CT_METHODS, NULL);
  return mi >= 0 && c->scopes[mi].mixin_guarded ? mi : -1;
}
int cplan_bam_string_guard(Compiler *c, int cap, int scope) {
  if (!c->ver_built || scope < 0 || cap < 0) return -1;
  const Scope *wrapper = &c->scopes[scope];
  const char *name = nt_str(c->nt, wrapper->def_node, "bam_sym");
  if (!name || !is_string_boxed_reader(name)) return -1;
  int recv = nt_ref(c->nt, cap, "receiver");
  TyKind type = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (type != TY_STRING && type != TY_STRBUF && type != TY_POLY) return -1;
  int mi = conditional_guard_method(c, comp_core_class(c, CORE_STRING), name);
  if (mi < 0) return -1;
  int targets[VER_MAX_ARMS + 1], n = guard_chain(c, mi, targets, NULL, NULL);
  return guard_is_conditional(c, targets, n) && c->scopes[mi].entry == ENTRY_TABLE ? mi : -1;
}
/* Visibility of an installed conditional prepend belongs to the entry,
   including the case where a known earlier call reaches its fallback; so
   does a face entry's (VerEntry.face, ct_face_for), whose arms test the
   visibility of the version they run. */
static int ver_scope_faced(Compiler *c, int t) {
  if (!c->ver_scope_face || c->ver_scope_face_n != c->nver_entries || c->ver_scope_face_ns != c->nscopes) {
    free(c->ver_scope_face);
    if (!(c->ver_scope_face = calloc((size_t)c->nscopes + 1, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < c->nver_entries; i++)
      if (c->ver_entries[i].face && c->ver_entries[i].scope >= 0 && c->ver_entries[i].scope < c->nscopes) c->ver_scope_face[c->ver_entries[i].scope] = 1;
    c->ver_scope_face_n = c->nver_entries, c->ver_scope_face_ns = c->nscopes;
  }
  return t >= 0 && t < c->nscopes && c->ver_scope_face[t];
}
int cplan_guarded_call_visibility(Compiler *c, int id, int cls, const char *name) {
  if (!c->ver_built || !cplan_call_enforces_vis(c, id)) return 0;
  if (ver_scope_faced(c, call_target(c, id, cls, name, 0, NULL))) return 1;
  int mi = conditional_guard_method(c, cls, name);
  if (mi < 0) return 0;
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, NULL, NULL);
  return guard_needs_face(c, to, n);
}
/* A call that reaches mi, a copy a later prepend puts in front of a method,
   runs it once the prepend's statement has run and the method behind it
   until then: the guard entry of mi's class for the call's arguments
   (guard_entry_make), which the entries' build makes for every call of
   the name; mi when asked before the build */
static int ct_guard(Compiler *c, int id, int mi, unsigned flags, MethodLookup *ml) {
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, NULL, NULL);
  const Scope *m = &c->scopes[mi];
  const char *name = c->ver_built ? m->name : comp_scope_user_name(m);
  if (flags & CT_CAPTURE) {
    const LrDef *d = NULL;
    LoadPoint p = elab_point_of(c, id);
    /* (a mixin's arm, or the copy of its module's version live there:
       pos_entry) */
    if (p.region >= 0 && elab_method_at(c, m->class_id, name, m->is_cmethod, p, &d) == ELAB_YES && d)
      for (int i = 0; i < n; i++) {
        ElabVersion vs[VER_MAX_ARMS];
        int vt[VER_MAX_ARMS], nv = c->scopes[to[i]].mixin_guarded ? mixin_copy_versions(c, to[i], m->class_id, vt, vs, 0) : 0;
        if (!nv) vt[nv++] = to[i];
        for (int k = 0; k < nv; k++)
          if (c->scopes[vt[k]].def_node == d->node) {
            *ml = (MethodLookup){ .kind = ML_METHOD, .n = 1, .def_class = ml->def_class };
            return vt[k];
          }
      }
  }
  int e = -1;
  if (c->ver_built) {
    int ar = guard_arity(c, id, to, n);
    int enforcing = (flags & CT_PUBLIC) || (!flags && id >= 0 && cplan_call_enforces_vis(c, id));
    int face = enforcing && guard_needs_face(c, to, n) ? guard_call_face(c, id, to, n) : VF_NONE;
    int ix = ver_entry_ix(c, VER_ENTRY_GUARD, 0, m->class_id, m->is_cmethod, name, ar, face);
    e = ix >= 0 ? c->ver_entries[ix].scope : -1;
  }
  *ml = (MethodLookup){ .kind = e >= 0 ? ML_ENTRY : ML_TABLE, .why = e >= 0 ? 0 : MLW_MANY, .n = (short)n, .def_class = ml->def_class };
  return e >= 0 ? e : mi;   /* a later synthesized call uses the general table entry */
}
/* The version an entry's arm (or else arm) call is pinned to, -1 for any
   other node, CT_PIN_BUILTIN for an else arm that runs the builtin
   (VK_BUILTIN): the pins are kept in the order of their nodes (ver_pin) */
enum { CT_PIN_BUILTIN = -2 };
static const struct VerPin *ct_pin_find(const Compiler *c, int id) {
  int lo = 0, hi = c->nver_pins - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, at = c->ver_pins[mid].node;
    if (at == id) return &c->ver_pins[mid];
    if (at < id) lo = mid + 1;
    else hi = mid - 1;
  }
  return NULL;
}
static int ct_pin_of(const Compiler *c, int id) {
  const struct VerPin *p = ct_pin_find(c, id);
  return p ? p->scope : -1;
}
/* ...as it answers a lookup of name at the call: a pin answers the call's
   own name, not another a lookup asks at it (the hook's, method_missing).
   The pins are asked first: a call with none reads no node. */
static int ct_pin_for(const Compiler *c, int id, const char *name) {
  int pin = ct_pin_of(c, id);
  const char *own = pin != -1 ? nt_str(c->nt, id, "name") : NULL;
  return own && sp_streq(own, name) ? pin : -1;
}
/* ...a pin of call id, of name, to a method of a class below cls on the side flags ask (CT_CMETHOD), which a lookup
   on cls's chain does not find: a blank slate's Kernel guard calls a subclass's own method_missing where self is of
   that class (ver_kern_mm_arm), the def an alias of it names among them */
static int ct_pin_below(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  int pin = id >= 0 && cls >= 0 ? ct_pin_for(c, id, name) : -1;
  const Scope *s = pin >= 0 ? &c->scopes[pin] : NULL;
  return s && !s->is_cmethod == !(flags & CT_CMETHOD) && s->class_id >= 0 && is_descendant(c, s->class_id, cls) ? pin : -1;
}
/* ...a pin of call id, of name, to a method of a class cls is at or below, on the side flags ask: an entry's arm or
   else arm runs its own method, static, for every receiver of that class or a subclass, whether or not the receiver
   class's own lookup lands on a version, a guard or an entry. A subclass's method of the same name (a def a prepend
   hid under the same hidden name) is no target of it */
static int ct_pin_binds(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  int pin = id >= 0 && cls >= 0 ? ct_pin_for(c, id, name) : -1;
  const Scope *s = pin >= 0 ? &c->scopes[pin] : NULL;
  return s && !s->is_cmethod == !(flags & CT_CMETHOD) && s->class_id >= 0 && is_descendant(c, cls, s->class_id);
}
int cplan_pinned_vis(const Compiler *c, int id) {
  const struct VerPin *p = ct_pin_find(c, id);
  return p ? p->vis : 0;
}
int cplan_pinned_builtin(const Compiler *c, int id) { return ct_pin_of(c, id) == CT_PIN_BUILTIN; }
int cplan_pinned(const Compiler *c, int id) { return ct_pin_of(c, id) != -1; }
/* See call_plan.h. */
int cplan_pin_scope(const Compiler *c, int id) {
  const struct VerPin *p = ct_pin_find(c, id);
  return p && p->scope >= 0 && !p->vis ? p->scope : -1;
}
/* Is version vi a top-level method (Object's) that a call asked on a
   class's chain reaches past the class's own versions? The class's table
   holds no such method: the call's top-level ask (CT_TOP) binds it. */
static int ct_top_version(const Compiler *c, int vi, unsigned flags) {
  return !(flags & CT_TOP) && c->scopes[vi].class_id < 0;
}
/* The class whose chain holds the versions a call on main (CT_TOP) meets
   of `name` on side sg: Object's, where main's top-level history and
   Object's are one entry of its table (elab_main_object_one_entry), whose
   entry the call then takes with main as self; else main's own (-1) */
static int ct_top_chain(Compiler *c, const char *name, int sg) {
  int oc = comp_core_class(c, CORE_OBJECT);
  if (sg || oc < 0) return -1;
  if (elab_main_object_one_entry(c, name)) return oc;
  /* (a module Object includes holds a version behind main's own) */
  ElabVersion vs[VER_MAX_ARMS];
  int n = elab_chain_versions(c, oc, 0, name, vs, VER_MAX_ARMS), mh = elab_history_versions(c, -1, 0, name, vs, VER_MAX_ARMS);
  return n > 0 && mh > 0 && n > mh ? oc : -1;
}
/* (mixed through the multiply and shifts of a finalizer: the keys run over
   small ranges, and the sum or xor of their products fell in long runs) */
static unsigned ver_mix(unsigned h) {
  h ^= h >> 15; h *= 0xC2B2AE35u; h ^= h >> 13;
  return h;
}
/* How many versions rcls's chain holds of name (-1 past VER_MAX_ARMS),
   whether the first is a plain def, whether one has an extent (a method
   body makes it, or a load region holds it: elab_version_extent; a call
   may run before that body does, where the chain may hold none of them),
   and the late bit of the chain's histories
   (elab_versions_late), kept by (class, side, name) for the record as it
   stands (elab_stamp): every call asked of every class of a name asks it,
   and the answer is the chain's. The names are its own copies, as in the
   other version tables (Compiler.ver_entries). */
struct VerChain { struct VerChainSlot { char *name; int rcls, sg, n, def, body, late; } *slot; int cap, used; ElabStamp stamp; };
void ver_chain_free(Compiler *c) {
  if (!c->ver_chain) return;
  for (int i = 0; i < c->ver_chain->cap; i++) free(c->ver_chain->slot[i].name);
  free(c->ver_chain->slot);
  free(c->ver_chain);
  c->ver_chain = NULL;
}
static unsigned ver_chain_hash(const char *name, int rcls, int sg) {
  return ver_mix(sp_strhash(name) + (unsigned)rcls * 0x9E3779B1u + (unsigned)sg * 0x85EBCA6Bu);
}
static int ver_chain_count(Compiler *c, int rcls, int sg, const char *name, int *first_def, int *body, int *late) {
  struct VerChain *m = c->ver_chain;
  if (m && !elab_stamp_same(m->stamp, elab_stamp(c))) { ver_chain_free(c); m = NULL; }
  if (!m) {
    if (!(c->ver_chain = m = calloc(1, sizeof *m))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->stamp = elab_stamp(c);
  }
  if (2 * (m->used + 1) > m->cap) {   /* grown: every kept answer moves */
    struct VerChainSlot *old = m->slot;
    int ocap = m->cap;
    m->cap = ocap ? 2 * ocap : 1024;
    if (!(m->slot = calloc((size_t)m->cap, sizeof *m->slot))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!old[i].name) continue;
      int j = (int)(ver_chain_hash(old[i].name, old[i].rcls, old[i].sg) & (unsigned)(m->cap - 1));
      while (m->slot[j].name) j = (j + 1) & (m->cap - 1);
      m->slot[j] = old[i];
    }
    free(old);
  }
  int j = (int)(ver_chain_hash(name, rcls, sg) & (unsigned)(m->cap - 1));
  for (; m->slot[j].name; j = (j + 1) & (m->cap - 1))
    if (m->slot[j].rcls == rcls && m->slot[j].sg == sg && sp_streq(m->slot[j].name, name)) {
      *first_def = m->slot[j].def;
      *body = m->slot[j].body;
      *late = m->slot[j].late;
      return m->slot[j].n;
    }
  ElabVersion vs[VER_MAX_ARMS];
  int n = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS), def = n >= 1 && vs[0].v->kind == LR_DEF, any = 0;
  for (int k = 0; k < n; k++) any |= (elab_version_extent(c, &vs[k]) != 0 && !elab_body_reflected(c, name, sg)) || elab_version_held_row(vs[k].v, name, sg);
  m->slot[j] = (struct VerChainSlot){ name_dup(name), rcls, sg, n, def, any, elab_versions_late(c, name, vs, n) };
  m->used++;
  *first_def = def;
  *body = any;
  *late = m->slot[j].late;
  return n;
}
/* Does call id run any time after its point: in a block the record
   answers later or an END body (elab_runs_later), or in a block it takes
   to run at once that its call's typed target keeps (an_mark_kept_blocks)? */
static int ct_after_point(Compiler *c, int id) {
  return elab_runs_later(c, id) || elab_in_kept_block(c, id);
}
/* The versions call id may meet as cls's `name` over the statements it may
   run in (elab_call_span), into out[0..cap): how many, where the window
   holds a version at every state of the call and fits cap; else 0 for a
   window with a state that holds none yet (a def or the include that
   brings it comes later) or more than cap, or elab_call_span's negative
   answer for a window it cannot tell */
static int ct_every_span(Compiler *c, int id, int cls, const char *name, int sg, const LrDef **out, int cap) {
  int n = elab_call_span(c, id, cls, name, sg, ct_after_point(c, id), NULL, out, cap);
  for (int k = 0; k < n && k < cap; k++) if (!out[k]) return 0;
  return n > cap ? 0 : n;
}
/* The version of the one definition d of `name` a call meets (the window's
   one): d must be a def, or with aliased an alias or a reader (not a
   mixin's copy, whose window the record does not place yet), that ran
   there for sure (not under a condition, nor after a statement that may
   raise in a begin body with rescue clauses: elab_point_partial);
   ct_def_version's, -1 for any other */
static int ct_window_def(Compiler *c, int cls, const char *name, int sg, int aliased, const LrDef *d) {
  if (!d || (d->kind != LR_DEF && !(aliased && (d->kind == LR_ALIAS || d->kind == LR_READER))) || d->may || d->unplaced ||
      elab_point_partial(c, d->node)) return -1;
  int dc = -1;
  return ct_def_version(c, cls, name, sg, d, &dc);
}
/* The method call id meets as cls's `name` (singleton when sg) wherever
   it runs: the one version of the name on cls's chain over the statements
   id may run in (elab_call_span), a def that ran there for sure, as
   cls's copy of it when a mixin brought it (ct_def_version);
   -1 where the record cannot prove one -- no version yet at some point of
   the window (a def or the include that brings it comes later), more than
   one, or one that may not have run (under a condition, or after a
   statement that may raise in a begin body with rescue clauses:
   elab_point_partial); a def, or with aliased an alias or a reader too
   (ct_window_def) */
static int ct_window_version(Compiler *c, int id, int cls, const char *name, int sg, int aliased) {
  const LrDef *ds[2];
  return ct_every_span(c, id, cls, name, sg, ds, 2) == 1 ? ct_window_def(c, cls, name, sg, aliased, ds[0]) : -1;
}
/* Does call id meet versions after its point (elab_call_meet's 1): it runs
   later (ct_after_point), or the record keeps a block around it
   (elab_in_record_kept_block), which adds ELAB_MEET_RECORD, so that a window
   that cannot tell leaves the answer at its point */
static int ct_meet_after(Compiler *c, int id) {
  if (ct_after_point(c, id)) return 1;
  return elab_in_record_kept_block(c, id) ? 1 | ELAB_MEET_RECORD : 0;
}
/* ...asked by ct_version_at for each class a call may land on, once the
   entries are made (memo): kept by node (Compiler.meet_memo) */
static int ct_meet_after_kept(Compiler *c, int id, int memo) {
  struct MeetMemo *m = &c->meet_memo;
  if (!memo || id < 0) return ct_meet_after(c, id);
  if (id >= m->cap) {
    int cap = m->cap ? m->cap : 1024;
    while (cap <= id) cap *= 2;
    if (!(m->at = realloc(m->at, (size_t)cap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(m->at + m->cap, 0, (size_t)(cap - m->cap));
    m->cap = cap;
  }
  if (!m->at[id]) m->at[id] = (unsigned char)(ct_meet_after(c, id) + 1);
  return m->at[id] - 1;
}
/* How call id asks the record for the versions it may meet
   (elab_call_meet): after its point where it runs later (also in a block
   the record keeps to run at a time the syntax does not tell, which may run
   after any later statement and from an exit handler:
   elab_in_record_kept_block), and as the
   mixin's copy's call in a mixin's copy of its body */
static int ct_meet_how(Compiler *c, int id) {
  const Scope *own = comp_scope_of(c, id);
  return ct_meet_after(c, id) | (own && own->origin_module_ci > 0 ? ELAB_MEET_COPY : 0);
}
/* The one version node id meets as cls's `name` (singleton when sg): the
   one the record answers at id's point, or the one over the statements id
   may run in (elab_call_meet, asked as ct_version_at asks); NULL where it
   meets none or the record cannot tell which, also for no node (id < 0),
   *none 1 where it surely meets none. Its callers ask it in the record's
   frame for the call plan (elab_plan_enter). */
static const LrDef *ct_met_version(Compiler *c, int id, int cls, const char *name, int sg, int *none) {
  const LrDef *ds[2];
  ElabMeet m;
  *none = 0;
  if (id < 0) return NULL;
  elab_call_meet(c, id, cls, name, sg, ct_meet_how(c, id), &m, ds, 2);
  *none = m.at == ELAB_NO || (m.at == ELAB_UNKNOWN && m.win == ELAB_YES && m.n == 0);
  if (m.at == ELAB_YES) return m.d;
  return m.at == ELAB_UNKNOWN && m.win == ELAB_YES && m.n == 1 ? ds[0] : NULL;
}
/* The class an alias that node id meets as cls's `name` (singleton when
   sg) was made in, as Method#owner names it: the class or module whose body
   holds the alias statement, the version the record answers at id's point
   or the one version over the statements id may run in; *own_sg 1 where
   the alias is on that class's singleton. -1 where the name meets no alias
   there, or the record cannot tell. Asked in the record's frame for the
   call plan (elab_plan_enter), as ct_version asks for the target, so the
   owner and the target are read from the same facts */
int cplan_alias_holder(Compiler *c, int id, int cls, const char *name, int sg, int *own_sg) {
  int none;
  if (cls < 0 || !name) return -1;
  int framed = elab_plan_enter(c, id);
  const LrDef *d = ct_met_version(c, id, cls, name, sg, &none);
  int h = d && d->kind == LR_ALIAS ? c->node_cbody[d->node] : -1;
  *own_sg = h >= 0 && elab_version_on_singleton(c, h, name, d->node);
  if (framed) elab_plan_leave(c);
  return h;
}
/* (a copy of the space-separated names s, each ended by a NUL in place
   of its space, and their count in *n; the caller frees it) */
static char *bam_names_split(const char *s, int *n) {
  char *v = strdup(s);
  if (!v) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  *n = *v != 0;
  for (char *p = v; *p; p++)
    if (*p == ' ') { *p = 0; (*n)++; }
  return v;
}
/* (the version of sym the maker, node site, meets on the ancestors of
   builtin class cls up to own, or the whole chain where no class owns the
   builtin's method of the name (own NULL; a name only a reopening of an
   ancestor defines): the method of a def a reopen gives it there, -1 where
   none is live, -2 where the record cannot tell) */
static int bam_reopen_walk(Compiler *c, int site, const char *cls, const char *sym, const char *own) {
  const char *anc = core_class_ancestors(cls);
  if (!anc) return -2;
  int na = 0, nk = 0, r = -1, last = 0;
  char *names = bam_names_split(anc, &na);
  const char *lastn = NULL;   /* the last class walked: the builtin's */
  int *ks = malloc(sizeof(int) * (size_t)(na > 0 ? na : 1));
  if (!ks) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* (the ancestors' classes the program reopens, -1 for the others) */
  for (const char *p = names; nk < na && !last; p += strlen(p) + 1) {
    last = own && sp_streq(p, own);
    lastn = p;
    ks[nk++] = comp_builtin_class_named(c, p);
  }
  for (int i = 0; i < nk && r == -1; i++) {
    int none, dc = -1;
    if (ks[i] < 0) continue;
    const LrDef *d = ct_met_version(c, site, ks[i], sym, 0, &none);
    if (none) continue;
    int k = d && d->kind == LR_DEF ? c->node_cbody[d->node] : -1, in = 0;
    for (int j = i; j < nk && k >= 0; j++) in |= ks[j] == k;
    /* (or a module the program prepends to it, which shadows the class's
       own method as a reopen does) */
    int own_at = !in && k >= 0 ? elab_own_method_at(c, ks[i], sym, elab_point_of(c, site)) : ELAB_UNKNOWN;
    in = in || own_at == ELAB_YES;
    /* (a def the last class surely gives no def of its own nor a
       prepend's is an included module's or an ancestor's, which the
       class's builtin shadows where the class defines the method itself
       (its instance facts list it); where the builtin comes from a module
       behind the include, the include's def wins, and like any other
       version the record gives that is no def of these, or no method the
       program has, it cannot be told) */
    if (!in && own_at == ELAB_NO && i == nk - 1 && builtin_instance_method_known(lastn, sym)) continue;
    int v = in ? comp_method_version(c, k, sym, 0, d->node, &dc) : -1;
    r = v >= 0 ? v : -2;
  }
  free(ks);
  free(names);
  return r;
}
/* The program's def a builtin's wrapper, method mi
   (desugar_builtin_method_obj), stands for in place of the builtin: the
   version of its name (bam_sym) its Method's maker (bam_site) meets
   (ct_met_version), a def a reopen of the program gives the name on the
   ancestors (core_class_ancestors) of the builtin class the receiver is
   of (bam_classes) up to the class or module the builtin is in
   (bam_owner), a module the class includes too (`module Comparable; def
   clamp ...`). A true-or-false receiver may be of either class: the walks
   from TrueClass and from FalseClass must meet the same def. -1 where the
   wrapper stands for the builtin: no reopen gives the name, or its def is
   not yet live at the maker. -2 where the record cannot tell (a
   conditional reopen, a maker that may run before or after it, walks that
   disagree): the Method may stand for either, which only a run-time
   version of the method could answer. Asked in the record's frame for the
   call plan (elab_plan_enter), as cplan_alias_holder asks */
int cplan_bam_reopen_target(Compiler *c, int mi) {
  const NodeTable *nt = c->nt;
  int dn = mi >= 0 ? c->scopes[mi].def_node : -1;
  const char *sym = dn >= 0 ? nt_str(nt, dn, "bam_sym") : NULL, *own = dn >= 0 ? nt_str(nt, dn, "bam_owner") : NULL;
  /* (a Comparable's or Numeric's method has no class owner in the facts,
     but the module or the superclass ends the walk as its owner does) */
  if (!own && dn >= 0) own = nt_int(nt, dn, "bam_cmp", 0) ? "Comparable" : nt_int(nt, dn, "bam_num", 0) ? "Numeric" : NULL;
  const char *cl = dn >= 0 ? nt_str(nt, dn, "bam_classes") : NULL;
  int site = dn >= 0 ? (int)nt_int(nt, dn, "bam_site", 0) - 1 : -1, nc = 0, r = -1;
  if (!sym || !cl || site < 0) return -1;
  char *cls = bam_names_split(cl, &nc);
  int framed = elab_plan_enter(c, site);
  const char *p = cls;
  for (int i = 0; i < nc; i++, p += strlen(p) + 1) {
    /* (a true's or false's own method is its class's: the owner is each
       class's, where the receiver's class is not known) */
    const char *po = own ? own : is_boolean_class_name(p) ? builtin_bool_method_owner(p, sym) : NULL;
    int ri = bam_reopen_walk(c, site, p, sym, po);
    r = i == 0 || ri == r ? ri : -2;
  }
  if (framed) elab_plan_leave(c);
  free(cls);
  return r;
}
/* See call_plan.h. */
int cplan_builtin_reopen_def(Compiler *c, int site, const char *cls, const char *sym, const char *own) {
  int framed = elab_plan_enter(c, site);
  int r = bam_reopen_walk(c, site, cls, sym, own);
  if (framed) elab_plan_leave(c);
  return r;
}
/* See call_plan.h. */
int cplan_reopen_def_public(Compiler *c, int site, int mi, const char *sym) {
  int framed = elab_plan_enter(c, site);
  int vis = mi >= 0 ? elab_method_vis_at(c, c->scopes[mi].class_id, sym, 0, elab_point_of(c, site)) : -1;
  if (framed) elab_plan_leave(c);
  return vis == SP_VIS_PUBLIC;
}
static void ct_accessor_met(Compiler *c, int id, int cls, const char *name, CplanAccessor *a);
/* See call_plan.h. */
CplanClassArm *cplan_capture_class_arms(Compiler *c, int mn, const char *sym, int *n) {
  int cap = 8;
  CplanClassArm *r = malloc(sizeof *r * (size_t)cap);
  char base[256];
  int wr = setter_base_name(sym, base, sizeof base), framed = mn >= 0 ? elab_plan_enter(c, mn) : 0;
  if (!r) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  *n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    /* (the accessor lists only narrow the classes asked, every class as the
       candidates are: no answer is read off them here) */
    mtab_enter(c);
    int reader = comp_reader_in_chain(c, k, sym, NULL), writer = !reader && wr && comp_writer_in_chain(c, k, base, NULL), mi;
    mtab_leave(c);
    CplanClassArmKind kind = reader ? CP_CARM_READER : CP_CARM_WRITER;
    CplanAccessor acc = { -1, -1, -1 };
    /* an accessor the capture meets (a later def of the name does not
       replace it), else the def or entry the capture finds, else the
       class's accessor as the tables have it: a table answer
       (--method-table-check) */
    if ((reader || writer) && mn >= 0) ct_accessor_met(c, mn, k, sym, &acc);
    if (acc.owner >= 0) mi = -1;
    else {
      mi = call_target(c, mn, k, sym, CT_CAPTURE, NULL);
      if (mi >= 0) kind = c->scopes[mi].entry != ENTRY_NONE ? CP_CARM_ENTRY : CP_CARM_METHOD;
      else if (!reader && !writer) continue;
      else if (g_method_table_check) mtab_table(c, mn, k, 0, sym, MLW_ACCESSOR);
    }
    if (*n == cap) {
      cap *= 2;
      r = realloc(r, sizeof *r * (size_t)cap);
      if (!r) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    r[(*n)++] = (CplanClassArm){ k, mi, kind };
  }
  if (framed) elab_plan_leave(c);
  return r;
}
/* (the class whose accessor lists answer cls's `name` on its chain, a
   writer's as `x=`; -1 for none) */
static int ct_accessor_class(Compiler *c, int cls, const char *name) {
  int dc = -1;
  char base[256];
  mtab_enter(c);   /* (entry construction: the accessor lists' own answer) */
  int found = setter_base_name(name, base, sizeof base) ? comp_writer_in_chain(c, cls, base, &dc)
                                                         : comp_reader_in_chain(c, cls, name, &dc);
  mtab_leave(c);
  return found ? dc : -1;
}
/* (the Struct or the Data on cls's superclass chain whose member `name`
   is, a writer's as `x=` (a Struct's alone): by the members its Struct.new
   or Data.define gave it, which no attr_* call versions; asked where the
   record has no version of the name; -1 for none) */
static int ct_struct_member(Compiler *c, int cls, const char *name) {
  char base[256];
  int wr = setter_base_name(name, base, sizeof base);
  for (int k = cls; k >= 0 && k < c->nclasses; k = c->classes[k].parent) {
    const ClassInfo *ci = &c->classes[k];
    if (!ci->is_struct && (wr || !ci->is_data)) continue;
    for (int j = 0; j < ci->nmembers; j++)
      if (ci->ivars[j] && sp_streq(ci->ivars[j] + 1, wr ? base : name)) return k;
  }
  return -1;
}
/* (is v an attr_* call's version that surely ran where it stands?) */
static int ct_attr_version(const LrDef *v) {
  return v && (v->kind == LR_READER || v->kind == LR_WRITER) && !v->may && !v->unplaced;
}
/* What node id, a Method's maker, meets as an instance of cls's accessor
   `name` (a writer's as `x=`): the version it meets (ct_met_version), past
   an alias to the version of its target live where the alias was made, in
   the class whose body made it (as ct_def_version follows one; the points
   only go back). As CRuby answers its Method:
     - the owner is the class or module whose body holds the first alias
       met, else the one whose body declares the accessor;
     - the position is the attr_* call's line, or none (nil) through an
       alias of an accessor a module declares: CRuby makes such an alias
       an entry of its own around the module's method, and
       #source_location reads an attr's position off the attr's own
       entry alone, not through that wrapper (an alias of a class's
       accessor shares its entry, and of a module's def answers the
       def's line). A Struct's or a Data's member, which no call
       declares, has none either. */
static void ct_accessor_met(Compiler *c, int id, int cls, const char *name, CplanAccessor *a) {
  int none, holder = -1;
  const LrDef *d = ct_met_version(c, id, cls, name, 0, &none);
  if (!d) {
    int sm = none ? ct_struct_member(c, cls, name) : -1;
    if (sm >= 0) { a->owner = sm; a->pos = 0; }
    return;
  }
  while (d->kind == LR_ALIAS && d->target && !d->may) {
    LoadPoint p = elab_point_of(c, d->node);
    int h = c->node_cbody[d->node], o, sm;
    const char *t = d->target;
    if (p.region < 0 || h < 0) return;
    if (holder < 0) holder = h;
    if ((o = elab_method_at(c, h, t, 0, p, &d)) == ELAB_NO) {
      if ((sm = ct_struct_member(c, h, t)) >= 0) { a->owner = holder; a->pos = 0; }
      return;
    }
    if (o != ELAB_YES || !d) return;
  }
  if (!ct_attr_version(d)) return;
  int body = c->node_cbody[d->node];
  if (body < 0) return;
  a->node = d->node;
  a->owner = holder >= 0 ? holder : body;
  a->pos = holder >= 0 && comp_class_is_module(c, &c->classes[body]) ? 0 : 1;
}
/* ...asked in the record's frame for the call plan (elab_plan_enter), as
   cplan_alias_holder asks. Where the record cannot tell which accessor id
   meets (or there is no id), the Method stands at the last attr_* call on
   cls's chain sure to run, whatever id's point (elab_accessor_node), and
   is owned by the class whose body makes it, else by the one whose
   accessor lists answer the name: CRuby's answer where no later version
   ran, not the one a version run only under a condition gives. Which of
   them ran is a run-time fact no accessor records. */
void cplan_accessor_met(Compiler *c, int id, int cls, const char *name, CplanAccessor *a) {
  *a = (CplanAccessor){ -1, -1, -1 };
  if (cls < 0 || !name) return;
  if (id >= 0) {
    int framed = elab_plan_enter(c, id);
    ct_accessor_met(c, id, cls, name, a);
    if (framed) elab_plan_leave(c);
  }
  if (a->pos < 0) {
    a->node = elab_accessor_node(c, cls, name);
    a->pos = a->node >= 0;
    a->owner = a->node >= 0 && c->node_cbody[a->node] >= 0 ? c->node_cbody[a->node] : ct_accessor_class(c, cls, name);
  }
}
/* Does node id make the implied call `name` on operand op (a node, or an
   NTI_ word): a planned row of tools/implied_calls.yml says so
   (nt_implied_calls), the rows the load record reads and the survey makes
   the entries of (call_node_implied_names) */
static int ct_implied_row(Compiler *c, int id, const char *name, int op) {
  NtImplied rows[16];
  int n = nt_implied_calls(c->nt, id, rows, 16);
  for (int i = 0; i < n && i < 16; i++)
    if (rows[i].planned && rows[i].operand == op && sp_streq(rows[i].name, name)) return 1;
  return 0;
}
/* The method the implied call `name` node id makes on operand op (a row:
   a multiple assignment's to_ary of its value, ct_implied_row) runs on an
   instance of cls, asked in the record's frame for the call plan
   (elab_plan_enter), as ct_version asks: the passes that ask it append
   nodes between the asks. Only where the call's own window holds a version
   at every state (ct_every_span): a call that may run before any version
   exists takes the object whole, as CRuby's, and asks no entry. Then:
   - one version, a def, an alias or a reader that ran there for sure
     (ct_window_def), which the explicit call of the name the lowering
     makes binds too: *ml is ML_METHOD;
   - several, the entry another call of the name made (CT_IMPLIED: the
     node does not itself call it, and an implied ask makes no entry),
     unless the chain holds a version a method body makes, which the
     entry has no arm for: ML_ENTRY;
   - else the table's method (ML_TABLE), with the why of the declined window
     (MLW_UNPLACED no window, MLW_MOVES the chain moves, MLW_MANY any
     other), as call_target answers it.
   A call no planned row makes (a builtin's callback, a syntax row not
   planned yet) is not asked of the record: the table's method, with
   MLW_UNASKED where the name has versions, else ML_METHOD; so is one with no
   site (id -1), the ask of a program-wide reader. side is 0 for
   an instance of cls, CT_CMETHOD for the class object, CT_OWN for cls's
   own method (with CT_CMETHOD, its own class method), CT_TOP for the top-level
   function of the name (cls is not read), as call_target's flags; the planned
   rows ask instances. -1 with ML_NONE where cls has no
   class, the table holds no method, or an undef of the name on cls's chain
   provably ran before id (cplan_undef_before: *ml is ML_NONE with MLW_RECORD; a
   planned row's reader takes the table's program-wide undef, as it always
   did): the call is lowered as a
   call of `name`, which binds through the table, so that call is not the
   version proved. An undef the record cannot place before id leaves the
   table's method, ML_TABLE, as cplan_hook_target does. ml may be NULL. */
static int ct_implied_at(Compiler *c, int id, int cls, const char *name, MethodLookup *m);
static int ver_chain_undef(Compiler *c, int rcls, int sg, const char *name);
static int ct_table(Compiler *c, int cls, const char *name, unsigned flags, int *def_class, int *attr);
/* Does an undef of `name` on cls's chain take the method away at call site: the
   record places the undef before site's point, unconditionally (elab_method_at
   answers none with the undef as the version in force), and site runs where
   the record places it, not in a method's body or a block it cannot place,
   which may run before the undef? Anywhere else the table's method stands, as
   cplan_hook_target keeps it. */
/* Does cls's instance chain hold an undef of name among the record's
   versions? Read from the version index alone: it places no node, so it
   does not rebuild the record's map after the table changed, which
   placing the call outside a plan frame does once per ask (#12194). 1
   where the index cannot say. */
static int cplan_chain_undef(Compiler *c, int cls, const char *name) {
  ElabVersion vs[VER_MAX_ARMS];
  int nv = elab_chain_versions(c, cls, 0, name, vs, VER_MAX_ARMS);
  if (nv < 0 || nv >= VER_MAX_ARMS) return 1;   /* (the index cannot say, or holds more: ask) */
  for (int i = 0; i < nv; i++)
    if (vs[i].v && vs[i].v->kind == LR_UNDEF) return 1;
  return 0;
}
/* See call_plan.h. */
int cplan_string_may_undef(Compiler *c, const char *name) {
  int ci = comp_core_class(c, CORE_STRING);
  return ci >= 0 && elab_method_versions(c, name) && builtin_instance_method_known("String", name) &&
         cplan_chain_undef(c, ci, name);
}
int cplan_undef_before(Compiler *c, int site, int cls, const char *name) {
  /* (no undef on the chain: none is in force at any point, and the answer
     below is 1 only for an undef) */
  if (site < 0 || !cplan_chain_undef(c, cls, name) || elab_in_unplaced_block(c, site)) return 0;
  int framed = elab_plan_enter(c, site);
  LoadPoint p = elab_point_of(c, site);
  const LrDef *d = NULL;
  int o = p.region >= 0 ? elab_method_at(c, cls, name, 0, p, &d) : ELAB_UNKNOWN;
  if (framed) elab_plan_leave(c);
  return o == ELAB_NO && d && d->kind == LR_UNDEF;
}
int cplan_implied_target(Compiler *c, int site, int op, int cls, const char *name, unsigned side, MethodLookup *ml) {
  MethodLookup own = { .def_class = -1 }, *m = ml ? ml : &own;
  *m = (MethodLookup){ .kind = ML_NONE, .def_class = -1 };
  if (cls < 0 && !(side & CT_TOP)) return -1;
  int table_only = (side & CIT_TABLE) != 0;   /* (a reader the plan has not taken over yet: the table's answer, as the table primitive gave it) */
  side &= ~(unsigned)CIT_TABLE;
  mtab_enter(c);
  /* (an undef the table no longer holds, a later def having replaced it, is in the record's versions) */
  int undef = !table_only && !(side & (CT_CMETHOD | CT_TOP)) && (comp_is_undeffed_in_chain(c, cls, name) || (elab_method_versions(c, name) > 1 && ver_chain_undef(c, cls, 0, name)));
  mtab_leave(c);
  /* (a call no planned row makes, a builtin's callback or a row not planned
     yet, is not asked of the record: the table's method, as call_target
     answers a call whose node does not make it) */
  int vi = -1, planned = !table_only && !side && site >= 0 && ct_implied_row(c, site, name, op);
  /* (a planned row's undef stays the table's, program-wide, as to_ary's readers
     always had it: its lowering, an explicit call of the name, would raise at
     the sites before the undef, as cplan_undef_stops takes the undef
     program-wide) */
  if (undef && planned) return -1;
  if (undef && cplan_undef_before(c, site, cls, name)) {
    m->why = MLW_RECORD;   /* (the record proves the call raises, as call_target's none) */
    return -1;
  }
  if (!planned) *m = (MethodLookup){ .kind = ML_TABLE, .why = MLW_UNASKED, .def_class = -1 };
  else vi = ct_implied_at(c, site, cls, name, m);
  if (vi >= 0 && m->def_class < 0) m->def_class = c->scopes[vi].class_id;
  if (m->kind != ML_TABLE) return vi;
  /* (no proof: the table's method, so the reader reads no table of its own) */
  int dc = -1;
  mtab_enter(c);
  int tm = ct_table(c, cls, name, side | CT_METHODS, &dc, NULL);
  mtab_leave(c);
  m->def_class = dc;
  if (tm < 0) m->kind = ML_NONE;
  else if (!planned && !undef && !ct_versioned(c, tm, name)) m->kind = ML_METHOD;   /* (a window the plan declined, or an undef it cannot place, stays declined) */
  else if (g_method_table_check) mtab_table(c, site, cls, side, name, m->why);
  return tm;
}
/* The answers of cplan_hook_target, kept by (site, class, name) for the
   record, the method table and the version entries as they stand
   (elab_stamp, nscopes, nclasses, nver_entries), and dropped with the
   version memos (ver_memo_drop: a fact the windows read changed, a block
   newly kept): every reader of a site asks it, and each ask read the
   site's window from the record. */
struct NewSites { struct NewSiteSlot { int id, ci, vi; const char *name; MethodLookup m; } *slot; int cap, used, nscopes, ncls, nent; ElabStamp stamp; };
void new_sites_free(Compiler *c) {
  if (!c->new_sites) return;
  free(c->new_sites->slot);
  free(c->new_sites);
  c->new_sites = NULL;
}
static unsigned new_sites_hash(int id, int ci, const char *name) {
  return ver_mix((unsigned)id * 0x9E3779B1u + (unsigned)ci * 0x85EBCA6Bu + sp_strhash(name));
}
/* (the slot of (id, ci, name): the kept answer, or the empty slot it goes in) */
static struct NewSiteSlot *new_sites_slot(Compiler *c, int id, int ci, const char *name) {
  struct NewSites *m = c->new_sites;
  if (m && (!elab_stamp_same(m->stamp, elab_stamp(c)) || m->nscopes != c->nscopes || m->ncls != c->nclasses || m->nent != c->nver_entries)) {
    new_sites_free(c);
    m = NULL;
  }
  if (!m) {
    if (!(c->new_sites = m = calloc(1, sizeof *m))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->stamp = elab_stamp(c);
    m->nscopes = c->nscopes;
    m->ncls = c->nclasses;
    m->nent = c->nver_entries;
  }
  if (2 * (m->used + 1) > m->cap) {   /* grown: every kept answer moves */
    struct NewSiteSlot *old = m->slot;
    int ocap = m->cap;
    m->cap = ocap ? 2 * ocap : 256;
    if (!(m->slot = calloc((size_t)m->cap, sizeof *m->slot))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < m->cap; i++) m->slot[i].id = -1;
    for (int i = 0; i < ocap; i++) {
      if (old[i].id < 0) continue;
      int j = (int)(new_sites_hash(old[i].id, old[i].ci, old[i].name) & (unsigned)(m->cap - 1));
      while (m->slot[j].id >= 0) j = (j + 1) & (m->cap - 1);
      m->slot[j] = old[i];
    }
    free(old);
  }
  int j = (int)(new_sites_hash(id, ci, name) & (unsigned)(m->cap - 1));
  while (m->slot[j].id >= 0 && (m->slot[j].id != id || m->slot[j].ci != ci || !sp_streq(m->slot[j].name, name))) j = (j + 1) & (m->cap - 1);
  return &m->slot[j];
}
/* Does call id, a construction of class ci (or a copy of an instance of
   it, for initialize_copy), run where the record proves no `name` (initialize
   or initialize_copy) of the program's on ci's chain: at its point
   (elab_call_method, which also proves no code the record cannot order
   supplies one), or at each state of its window, where it may run more than
   once (as ct_every_span asks it), with no code the record has not seen; and
   not where ci is not opened yet (elab_class_live_at), as a `new` there
   cannot build it */
static int new_init_absent(Compiler *c, int id, int ci, const char *name) {
  const LrDef *ds[2];
  int framed = elab_plan_enter(c, id), at = elab_call_method(c, id, ci, name, 0, 0), absent = at == ELAB_NO;
  if (at == ELAB_UNKNOWN && !elab_runs_unseen(c))
    absent = elab_call_span(c, id, ci, name, 0, ct_after_point(c, id), NULL, ds, 2) == 1 && !ds[0];
  absent = absent && elab_class_live_at(c, ci, 1, elab_point_of(c, id));
  if (framed) elab_plan_leave(c);
  return absent;
}
/* See call_plan.h. Class#new's initialize, or the initialize_copy a dup or
   clone runs on the copy (name): none where the record proves
   none there (new_init_absent: BasicObject's runs, or the builtin
   exception's or record's own), else the window's version (the steps of
   cplan_implied_target past its row); an entry is not taken, as none is
   made for initialize (ver_named_one). A site in a block whose run time
   the record does not place keeps the table's (elab_in_unplaced_block).
   The question's own reads are the plan's (--method-table-check), whatever
   node is the frame where a reader asks it of site id; an answer that
   stands on the table for want of a proof is reported as call_target's
   are. */
int cplan_hook_target(Compiler *c, int id, int ci, const char *name, MethodLookup *ml) {
  MethodLookup own = { .def_class = -1 }, *m = ml ? ml : &own;
  int dc = -1;
  mtab_enter(c);
  int tm = ci >= 0 ? comp_method_in_chain(c, ci, name, &dc) : -1;
  int undef = tm >= 0 && id >= 0 && comp_is_undeffed_in_chain(c, ci, name);
  mtab_leave(c);
  *m = (MethodLookup){ .kind = tm >= 0 ? ML_TABLE : ML_NONE, .why = MLW_UNASKED, .def_class = dc };
  /* (a siteless ask, which every reader of a class's constructions makes with id -1, answers the table's method:
     for a name with versions that is an unasked table line, as call_target's no-site ask reports one) */
  if (g_method_table_check && id < 0 && tm >= 0 && ct_versioned(c, tm, name)) mtab_table(c, id, ci, 0, name, MLW_UNASKED);
  if (tm < 0 || id < 0 || undef) return tm;
  /* (one version on the chain: the table's, the only one a site meets
     where there is one) */
  int first = 0, body = 0, late = 0, nv = ver_chain_count(c, ci, 0, name, &first, &body, &late);
  struct NewSiteSlot *e = new_sites_slot(c, id, ci, name);
  if (e->id >= 0) { *m = e->m; return e->vi; }
  /* (a site in a block the record does not place, kept, assumed or given
     to a call it cannot read, may run on either side of a reopening: it
     cannot tell, and keeps the table's; so does one where the record holds
     no version of the table's method, nv 0, as for a def a method body
     makes: it may be made before the site runs) */
  int vi = -1, unplaced = elab_in_unplaced_block(c, id), absent = !unplaced && nv != 0 && new_init_absent(c, id, ci, name);
  if (absent) *m = (MethodLookup){ .kind = ML_NONE, .def_class = -1 };
  else if (nv == 1) vi = tm;
  else if (unplaced) *m = (MethodLookup){ .kind = ML_TABLE, .why = MLW_UNPLACED, .def_class = dc };
  else vi = ct_implied_at(c, id, ci, name, m);
  if (!absent && (vi < 0 || m->kind != ML_METHOD)) {
    if (m->kind != ML_TABLE) *m = (MethodLookup){ .kind = ML_TABLE, .why = MLW_MANY, .def_class = dc };
    vi = tm;
    if (g_method_table_check && nv != 1) mtab_table(c, id, ci, 0, name, m->why);
  }
  /* (the class that holds the method the answer is: the table's, or the version's own) */
  if (vi >= 0) m->def_class = vi == tm ? dc : c->scopes[vi].class_id;
  /* (asking may have grown the table or rebuilt the record: the slot again) */
  e = new_sites_slot(c, id, ci, name);
  if (e->id < 0) { *e = (struct NewSiteSlot){ id, ci, vi, name, *m }; c->new_sites->used++; }
  return vi;
}
/* The exception class a Kernel#raise id builds, as its lowering names it
   (emit_call_raise_arms): the constant's class, by name; -1 for none, with
   *val set where its first argument is a value that may hold a class at
   run time (class_recv_may_be_dynamic) */
static int ct_raise_class(Compiler *c, int id, int *val) {
  const NodeTable *nt = c->nt;
  *val = 0;
  if (!cplan_kernel_raise(c, id)) return -1;
  int args = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  NodeKind k = ac > 0 ? nt_kind(nt, av[0]) : NK_NONE;
  if (k != NK_ConstantReadNode && k != NK_ConstantPathNode) {
    *val = ac > 0 && class_recv_may_be_dynamic(c, av[0]);
    return -1;
  }
  int xc = const_node_match_class_or_named(c, av[0]);
  return xc >= 0 && class_is_exc_subclass(c, xc) ? xc : -1;
}
/* See call_plan.h. The class ct_raise_class names. */
int cplan_raise_initialize(Compiler *c, int id) {
  int val, xc = ct_raise_class(c, id, &val);
  return xc >= 0 ? cplan_hook_target(c, id, xc, "initialize", NULL) : -1;
}
/* See call_plan.h. */
int cplan_raise_class_value(Compiler *c, int id) {
  int val;
  ct_raise_class(c, id, &val);
  return val;
}
/* See call_plan.h. A class not opened yet at the last statement of the
   site's window (elab_class_live_at) is opened at none of them. */
int cplan_new_arm_initialize(Compiler *c, int id, int ci) {
  ElabWindow w;
  int framed = elab_plan_enter(c, id), live = 1;
  if (elab_call_window(c, id, &w) == ELAB_YES && !w.never) {
    if (ct_after_point(c, id)) elab_window_open(c, &w);
    live = w.from.region != w.to.region || elab_class_live_at(c, ci, 1, (LoadPoint){ w.to.region, w.to.seq - 1, -1 });
  }
  if (framed) elab_plan_leave(c);
  return cplan_hook_target(c, live ? id : -1, ci, "initialize", NULL);
}
/* See call_plan.h. */
int cplan_new_arm_class(Compiler *c, int recv, int ci) {
  if (ci < 0 || ci >= c->nclasses) return 0;
  const ClassInfo *k = &c->classes[ci];
  return !is_builtin_reopen(k->name) && !k->is_native_class && !cls_fact_excludes_class_value(c, recv, ci);
}
/* The window step of cplan_implied_target, for a call node id makes */
static int ct_implied_at(Compiler *c, int id, int cls, const char *name, MethodLookup *m) {
  const LrDef *ds[VER_MAX_ARMS];
  int framed = elab_plan_enter(c, id), vi = -1, n = ct_every_span(c, id, cls, name, 0, ds, VER_MAX_ARMS);
  if (n == 1) {
    /* (the lowering makes an explicit call of the name, which binds as
       call_target answers it: the two must meet the same version) */
    vi = ct_window_def(c, cls, name, 0, 1, ds[0]);
    int why = vi < 0 ? MLW_UNMAPPED : call_target(c, id, cls, name, CT_IMPLIED, NULL) != vi ? MLW_DISAGREE : 0;
    *m = why ? (MethodLookup){ .kind = ML_TABLE, .why = (unsigned char)why, .def_class = -1 } : (MethodLookup){ .kind = ML_METHOD, .n = 1, .def_class = -1 };
    if (why) vi = -1;
  }
  else if (n >= 2) {
    /* (an entry has no arm for a version a method body makes) */
    int first = 0, body = 0, late = 0;
    ver_chain_count(c, cls, 0, name, &first, &body, &late);
    int mi = body ? -1 : call_target(c, id, cls, name, CT_IMPLIED, m);
    if (m->kind == ML_ENTRY && mi >= 0) vi = mi;
    else if (body || (m->kind != ML_TABLE && m->kind != ML_NONE)) *m = (MethodLookup){ .kind = ML_TABLE, .why = MLW_MANY, .def_class = -1 };
  }
  else *m = (MethodLookup){ .kind = ML_TABLE, .why = n == ELAB_SPAN_NO_WINDOW ? MLW_UNPLACED : n == -1 ? MLW_MOVES : MLW_MANY, .def_class = -1 };
  if (framed) elab_plan_leave(c);
  return vi;
}
/* The versions of the history that holds the table's method (its class's
   own), into out[0..VER_MAX_ARMS): with one, the table's is the one; with
   more, those an instance meets over the load (elab_instance_span), or
   every one where the record cannot tell them apart. How many; *whole as
   cplan_new_initializers says. */
static int new_inits_list(Compiler *c, int ci, int *out, int *whole) {
  int n = 0, dc;
  mtab_enter(c);   /* (the plan's own read: the table's method, which the versions of its history then refine) */
  int tm = comp_method_in_chain(c, ci, "initialize", NULL);
  mtab_leave(c);
  *whole = 1;
  if (tm < 0) return 0;
  out[n++] = tm;
  ElabVersion hv[VER_MAX_ARMS];
  const LrDef *ds[VER_MAX_ARMS];
  int nh = elab_history_versions(c, c->scopes[tm].class_id, 0, "initialize", hv, VER_MAX_ARMS);
  if (nh < 0) *whole = 0;
  if (nh <= 1) return n;
  int ns = elab_instance_span(c, ci, "initialize", ds, VER_MAX_ARMS), all = ns < 0 || ns > VER_MAX_ARMS;
  /* (an interval of the span with no version, after a removal: an instance
     made there runs no initialize of the program's) */
  for (int j = 0; !all && j < ns; j++)
    if (!ds[j]) *whole = 0;
  for (int i = 0; i < nh; i++) {
    const LrDef *d = hv[i].v;
    int met = all;
    for (int j = 0; j < ns && !met; j++) met = ds[j] == d;
    if (!met) continue;
    /* (a removal or an undef: what is behind it, or none of the program's) */
    int vi = d->kind == LR_REMOVE || d->kind == LR_UNDEF ? -1 : ct_def_version(c, ci, "initialize", 0, d, &dc), dup = 0;
    if (vi < 0) { *whole = 0; continue; }
    for (int j = 0; j < n; j++) dup |= out[j] == vi;
    if (dup) continue;
    if (n == VER_MAX_ARMS) { *whole = 0; break; }
    out[n++] = vi;
  }
  return n;
}
/* The lists, kept by class for the record and the method table as they
   stand (elab_stamp, nscopes, nclasses): each ivar fact of each class asks
   them, and a class of many ivars asked the record its history and span
   once an ivar. pool[lo[k]] is how many, the next whether that is whole,
   then the methods; lo[k] < 0 where not asked yet. sites[0..nsites), the
   census of construction sites (new_census), taken at the first ask that
   needs it; nsites < 0 before. */
struct NewSite;
struct NewInits { int *lo, *pool, ncls, nscopes, npool, cpool, nsites; struct NewSite *sites; ElabStamp stamp; };
void new_inits_free(Compiler *c) {
  struct NewInits *m = c->new_inits;
  if (!m) return;
  free(m->lo); free(m->pool); free(m->sites); free(m);
  c->new_inits = NULL;
}
/* A construction site of the census: node id builds an instance of class
   ci, and with own of each class below it; NEW_ANY, of any class its
   receiver may hold at run time; NEW_ANY_EXC, of any exception class. */
struct NewSite { int id, ci, own; };
enum { NEW_ANY = -1, NEW_ANY_EXC = -2 };
static int cplan_new_class(Compiler *c, int id, int *own);
static int ct_raise_class(Compiler *c, int id, int *val);
/* The census of the program's construction sites, beside compute_instantiated's
   walk: each `new` the builtin Class#new may answer (the class
   cplan_new_class names, or NEW_ANY where only the run time gives the
   receiver's class, class_recv_may_be_dynamic), each super that reaches it
   from a class method `new` (comp_super_is_class_new: that class and those
   below) and each Kernel#raise of a class (ct_raise_class, NEW_ANY_EXC for
   a value). By the nodes' shapes alone, reached or not: no type, so a
   reader in the type fixpoint may ask. */
static void new_census(Compiler *c, struct NewInits *m) {
  const NodeTable *nt = c->nt;
  int cap = 0, val = 0;
  m->nsites = 0;
  for (int id = 0; id < nt->count; id++) {
    struct NewSite s = { id, NEW_ANY, 0 };
    int r = nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "receiver") : -1;
    if (comp_super_is_class_new(c, id)) s = (struct NewSite){ id, comp_scope_of(c, id)->class_id, 1 };
    else if (nt_kind(nt, id) != NK_CallNode) continue;
    else if (is_new_name(nt_str(nt, id, "name"))) {
      int ci = cplan_new_class(c, id, &s.own);
      if (ci < 0 && (r < 0 || !class_recv_may_be_dynamic(c, r))) continue;
      if (ci >= 0) s.ci = ci;
    }
    else if (cplan_kernel_raise(c, id)) {
      int xc = ct_raise_class(c, id, &val);
      if (xc < 0 && !val) continue;
      s.ci = xc >= 0 ? xc : NEW_ANY_EXC;
    }
    else continue;
    if (m->nsites == cap && !(m->sites = realloc(m->sites, sizeof *m->sites * (size_t)(cap = cap ? 2 * cap : 16)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->sites[m->nsites++] = s;
  }
}
/* Does a site of the census build an instance of class k where the plan
   answers no initialize of the program's (cplan_hook_target: one that
   comes later)? Such an instance has none of what k's initialize sets. */
static int new_census_none(Compiler *c, struct NewInits *m, int k) {
  if (m->nsites < 0) new_census(c, m);
  for (int i = 0; i < m->nsites; i++) {
    const struct NewSite *s = &m->sites[i];
    int builds = s->ci == NEW_ANY ? 1 : s->ci == NEW_ANY_EXC ? class_is_exc_subclass(c, k) :
                 k == s->ci || (s->own && is_descendant(c, k, s->ci));
    if (builds && cplan_hook_target(c, s->id, k, "initialize", NULL) < 0) return 1;
  }
  return 0;
}
/* See call_plan.h. */
int cplan_new_initializers(Compiler *c, int ci, int *out, int cap, int *whole) {
  struct NewInits *m = c->new_inits;
  *whole = 1;
  if (ci < 0 || ci >= c->nclasses) return 0;
  if (m && (!elab_stamp_same(m->stamp, elab_stamp(c)) || m->nscopes != c->nscopes || m->ncls != c->nclasses)) { new_inits_free(c); m = NULL; }
  if (!m) {
    if (!(c->new_inits = m = calloc(1, sizeof *m)) || !(m->lo = malloc(sizeof(int) * (size_t)c->nclasses))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = 0; k < c->nclasses; k++) m->lo[k] = -1;
    m->nsites = -1;
    m->stamp = elab_stamp(c);
    m->nscopes = c->nscopes;
    m->ncls = c->nclasses;
  }
  if (m->lo[ci] < 0) {
    int ms[VER_MAX_ARMS], w, n = new_inits_list(c, ci, ms, &w);
    if (n > 0 && w && new_census_none(c, m, ci)) w = 0;
    if (m->npool + n + 2 > m->cpool) {
      m->cpool = m->npool + n + 2 > 2 * m->cpool ? m->npool + n + 2 : 2 * m->cpool;
      if (!(m->pool = realloc(m->pool, sizeof(int) * (size_t)m->cpool))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    }
    m->lo[ci] = m->npool;
    m->pool[m->npool++] = n;
    m->pool[m->npool++] = w;
    memcpy(m->pool + m->npool, ms, sizeof(int) * (size_t)n);
    m->npool += n;
  }
  const int *e = m->pool + m->lo[ci];
  int n = e[0] < cap ? e[0] : cap;
  memcpy(out, e + 2, sizeof(int) * (size_t)n);
  *whole = e[1] && e[0] <= cap;
  return n;
}
/* The version entry an implied call `name` on an instance of class cls
   takes where the plan answers an entry (mi: the table's method of the
   name on cls): the scope, -1 for none made. Its key is the one the
   plan's answer finds it by (cplan_implied_target, call_target's
   VER_ENTRY_CHAIN entry of the receiver's class): the receiver class, the
   name and the arity the versions take, for a call no CallNode makes
   (ver_call_arity of node -1: no arguments), so the same entry at every
   site of a syntax row. */
static int cplan_implied_entry(Compiler *c, int cls, const char *name, int mi) {
  if (!c->ver_built || cls < 0 || mi < 0) return -1;
  int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, cls, 0, name, ver_call_arity(c, -1, cls, 0, name, mi), VF_NONE);
  return ix >= 0 ? c->ver_entries[ix].scope : -1;
}
/* See call_plan.h. An Array subclass instance is an Array already
   (comp_arysub_emb): CRuby takes it as it is and never calls its #to_ary. */
int cplan_to_ary_method(Compiler *c, int id, int op, int cid) {
  if (comp_ary_is_array_proper(c, cid)) return -1;
  return cplan_implied_target(c, id, op, cid, "to_ary", 0, NULL);
}
/* See call_plan.h. The classes are cplan_boxed_to_ary_classes' (a box can
   hold them, their to_ary is the table's no-argument one). */
const CplanToAryEntries *cplan_to_ary_entries(Compiler *c) {
  if (c->to_ary_entries) return c->to_ary_entries;
  int np = 0;
  const PolyCand *pc = comp_poly_candidates(c, "to_ary", &np);
  CplanToAryEntries *es = calloc(1, sizeof *es);
  int *cls = malloc(sizeof(int) * (size_t)(np + 1)), *scope = malloc(sizeof(int) * (size_t)(np + 1));
  if (!es || !cls || !scope) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  es->cls = cls;
  es->scope = scope;
  for (int i = 0; i < np; i++) {
    int k = pc[i].cls, mi = comp_implicit_to_ary_mi(c, -1, -1, k);
    if (pc[i].native || mi < 0 || mi != pc[i].mi || c->scopes[mi].nparams != 0 || !c->classes[k].instantiated) continue;
    int e = cplan_implied_entry(c, k, "to_ary", mi);
    if (e < 0 || !c->scopes[e].reachable) continue;
    es->cls[es->n] = k;
    es->scope[es->n++] = e;
  }
  c->to_ary_entries = es;
  return es;
}
/* See call_plan.h. */
void cplan_to_ary_entries_free(Compiler *c) {
  if (!c->to_ary_entries) return;
  free(c->to_ary_entries->cls);
  free(c->to_ary_entries->scope);
  free(c->to_ary_entries);
  c->to_ary_entries = NULL;
}
/* The entry of class k in the program's set (cplan_to_ary_entries), -1
   for a class it has none for */
static int cplan_to_ary_entry_of(Compiler *c, int k) {
  const CplanToAryEntries *es = cplan_to_ary_entries(c);
  int lo = 0, hi = es->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2;
    if (es->cls[m] == k) return es->scope[m];
    if (es->cls[m] < k) lo = m + 1;
    else hi = m - 1;
  }
  return -1;
}
/* The classes whose own to_ary the implied to_ary call node id makes on
   operand op reaches when the operand is boxed (a multiple assignment's
   value, the value a block of several parameters takes apart): each
   class a box can hold (comp_poly_candidates) whose no-argument to_ary is
   the version the plan answers there (cplan_implied_target). An Array
   subclass is an Array already. A nested target's element converts at
   its own level, as its MultiTargetNode's implied to_ary of the element
   it takes (emit_massign_poly_target). An operand node of a program
   class's type (op >= 0) is boxed as that object, and reaches only the
   to_ary of its class or a subclass; one of another type (a Hash, a
   scalar) none. Into out[0..cap), ascending, with the method each class
   runs there into ans[0..cap) when ans is given, and how: the table's, a
   version the window proves, or the entry (the plan's MethodLookup); the
   count. A class the plan answers
   none for (a call that may run before any version exists) keeps the
   object, as CRuby's does before a to_ary is defined. */
int cplan_boxed_to_ary_classes(Compiler *c, int id, int op, int *out, CplanToAry *ans, int cap) {
  int n = 0, np = 0;
  TyKind ot = op >= 0 ? comp_ntype(c, op) : TY_POLY;
  int oc = ty_is_object(ot) ? ty_object_class(ot) : -1;
  if (oc < 0 && ot != TY_POLY) return 0;
  const PolyCand *pc = comp_poly_candidates(c, "to_ary", &np);
  for (int i = 0; i < np && n < cap; i++) {
    int k = pc[i].cls, mi = comp_implicit_to_ary_mi(c, id, op, k);
    if (pc[i].native || mi < 0 || mi != pc[i].mi || c->scopes[mi].nparams != 0 || !c->classes[k].instantiated) continue;
    if (oc >= 0 && !is_descendant(c, k, oc)) continue;
    MethodLookup ml;
    int vi = cplan_implied_target(c, id, op, k, "to_ary", 0, &ml);
    if (vi < 0 || ml.kind == ML_TABLE || c->scopes[vi].nparams != 0) continue;
    if (ans) ans[n] = (CplanToAry){ vi, ml.kind == ML_ENTRY && cplan_to_ary_entry_of(c, k) == vi ? CTA_ENTRY : vi == mi ? CTA_TABLE : CTA_VERSION };
    out[n++] = k;
  }
  return n;
}

/* See call_plan.h. An Array subclass instance is an Array already
   (comp_implicit_to_ary_mi). */
int cplan_typed_to_ary(Compiler *c, TyKind t, int at, int op, int *boxed) {
  if (boxed) *boxed = 0;
  int cls = ty_is_object(t) ? ty_object_class(t) : -1;
  int mi = cls >= 0 ? comp_implicit_to_ary_mi(c, at, op, cls) : -1;
  if (mi < 0 || c->scopes[mi].nparams != 0) return -1;
  MethodLookup ml;
  int vi = cplan_implied_target(c, at, op, cls, "to_ary", 0, &ml);
  if (vi < 0 || ml.kind == ML_TABLE || c->scopes[vi].nparams != 0) return -1;
  if (ty_is_array(c->scopes[mi].ret) && ty_is_array(c->scopes[vi].ret)) return vi;
  if (boxed) *boxed = 1;
  return -1;
}
/* See call_plan.h. */
int cplan_index_writer(Compiler *c, int cls, int *cdef) {
  int dc = -1;
  mtab_enter(c);   /* (the plan's own read: the class's `[]=` an index target stores through, as the plan states it) */
  int wmi = cls >= 0 ? comp_method_in_chain(c, cls, "[]=", &dc) : -1;
  mtab_leave(c);
  if (wmi < 0 || dc < 0 || c->scopes[wmi].nparams != 2) return -1;
  if (cdef) *cdef = dc;
  return wmi;
}
/* Does builtin `[]=` on a receiver of type rt call back, on one of the
   keys keys[0..n) it is handed, a method of the program? Its rows name the
   callbacks (bop_class_callbacks, BCR_ARG); a key of a program class
   reaches its class's method, a boxed one any class's. A receiver class
   the rows have not probed may call anything. The stored value is the
   element the assignment takes, not handed over to be converted here. */
static int cps_key_callbacks_run_code(Compiler *c, int tgt, TyKind rt, const int *keys, int n) {
  const char *rc = ty_is_obj_array(rt) ? "Array" : builtin_class_of_type(rt);
  const BopCallback *cb = NULL;
  int nc = rc ? bop_class_callbacks(rc, "[]=", &cb) : 0;
  if (nc < 0) return 1;
  for (int i = 0; i < nc; i++) {
    if (!(cb[i].roles & BCR_ARG)) continue;
    for (int j = 0; j < n; j++) {
      TyKind kt = comp_ntype(c, keys[j]);
      if (ty_is_object(kt) && call_target(c, tgt, ty_object_class(kt), cb[i].name, 0, NULL) >= 0) return 1;
      if (kt != TY_POLY && kt != TY_UNKNOWN) continue;
      int np = 0;
      const PolyCand *pc = comp_poly_candidates(c, cb[i].name, &np);
      for (int q = 0; q < np; q++) if (!pc[q].native) return 1;
    }
  }
  return 0;
}
/* Does a receiver typed as class `cid` store through a writer a descendant
   answers differently? Typed so, it can hold an instance of the descendant
   (`self` in an inherited method), and the single setter dispatches on the
   instance's class then. A descendant's own `def x=` does; so does its
   attr_writer where the typed class's writer is a method. */
int cplan_target_writer_switch(Compiler *c, int tgt, int cid, const char *nm) {
  if (c->classes[cid].is_native_class || c->classes[cid].c_struct) return 0;
  if (cplan_overridden(c, cid, nm, 0)) return 1;
  char base[256];
  /* (the writer the target makes on its receiver: the implied call of its row) */
  if (!setter_base_name(nm, base, sizeof base) || cplan_implied_target(c, tgt, nt_ref(c->nt, tgt, "receiver"), cid, nm, CIT_TABLE, NULL) < 0) return 0;
  int nd = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  for (int i = 0; i < nd; i++)
    if (ds[i] != cid && comp_is_writer(&c->classes[ds[i]], base)) return 1;
  return 0;
}
/* See call_plan.h. A target is the receiver and name of the call its
   store makes; an index target with no key stores nothing the program
   sees. */
unsigned cplan_target_store_effect(Compiler *c, int tgt) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, tgt);
  int r = k == NK_CallTargetNode || k == NK_IndexTargetNode ? nt_ref(nt, tgt, "receiver") : -1;
  if (r < 0) return 0;
  TyKind rt = comp_ntype(c, r);
  int open = rt == TY_POLY || rt == TY_UNKNOWN, cls = ty_is_object(rt) ? ty_object_class(rt) : -1;
  if (k == NK_CallTargetNode) {
    const char *nm = nt_str(nt, tgt, "name");
    MethodLookup ml = { .def_class = -1 };
    if (open) return CPS_CODE;
    if (!nm || cls < 0) return 0;
    if (cplan_target_writer_switch(c, tgt, cls, nm)) return CPS_CODE;
    return call_target(c, tgt, cls, nm, 0, &ml) >= 0 && ml.def_class >= 0 ? CPS_CODE : 0;
  }
  int args = nt_ref(nt, tgt, "arguments"), n = 0;
  const int *keys = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
  if (n < 1) return 0;
  if (open || cplan_index_writer(c, cls, NULL) >= 0) return CPS_CODE;
  if (cls >= 0) return 0;
  unsigned e = cps_key_callbacks_run_code(c, tgt, rt, keys, n) ? CPS_CODE : 0;
  if ((ty_is_array(rt) || ty_is_obj_array(rt)) && bop_name_mutates("[]=", BOP_MUT_CONTAINER)) e |= CPS_CHANGES_ARRAY;
  return e;
}
/* A chain a typed call may meet with no version made yet (ct_version_at),
   or, span, one a call past the build spans (with the face it binds), kept
   for ver_entries_demand to make its entry (Compiler.ver_late) */
static void ver_absent_note(Compiler *c, int rcls, int sg, const char *name, int mi, int ar, int mm, int span, int face) {
  if (!(c->nver_late & (c->nver_late - 1)) &&
      !(c->ver_late = realloc(c->ver_late, sizeof *c->ver_late * (size_t)(c->nver_late ? 2 * c->nver_late : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  c->ver_late[c->nver_late++] = (struct VerLate){ name_dup(name), rcls, sg, mi, ar, mm, span, face };
}
/* The method_missing of class ci's own chain in the tables (its class
   side when cmethod), Object's left out; -1 for none, and for main (-1) */
static int cplan_mm_chain(Compiler *c, int ci, int cmethod) {
  return ci < 0 ? -1 : cmethod ? comp_cmethod_in_chain(c, ci, METHOD_MISSING_NAME, NULL) : comp_method_in_chain(c, ci, METHOD_MISSING_NAME, NULL);
}
/* Is method_missing in cls's history chain (side sg) where a call runs,
   at its point p when the record places it there (at ELAB_NO), else over
   its window w: 1 where it is there throughout, 2 where it is not, both
   where it changes or the record cannot tell; with want (a method scope),
   there only as that method's def, both where another is found */
static int ver_mm_held(Compiler *c, int cls, int sg, LoadPoint p, int at, const ElabWindow *w, int want) {
  const LrDef *ds[VER_MAX_ARMS];
  int wd = want >= 0 ? c->scopes[want].def_node : -1;
  if (at == ELAB_NO) {
    int r = elab_method_at(c, cls, METHOD_MISSING_NAME, sg, p, ds);
    return r == ELAB_YES ? (wd < 0 || (ds[0] && ds[0]->node == wd) ? 1 : 3) : r == ELAB_NO ? 2 : 3;
  }
  int n = elab_method_span(c, cls, METHOD_MISSING_NAME, sg, w, ds, VER_MAX_ARMS), mm = n >= 1 && n <= VER_MAX_ARMS ? 0 : 3;
  for (int k = 0; k < n && k < VER_MAX_ARMS; k++) mm |= !ds[k] ? 2 : wd < 0 || ds[k]->node == wd ? 1 : 3;
  return mm;
}
/* ...on rcls's chain, which past its classes runs into Object's methods
   (main's history holds a top-level def, Object's an Object or a Kernel
   reopen's), as every lookup but a blank slate's does (cplan_mm_method,
   VerLate.mm): there throughout on one of them, or on none of them at any
   point. The else arm calls method_missing by name, which reaches the
   chain's own in the tables where it has one (cplan_mm_chain): then only
   that one counts, and Object's, at a call before it, keeps the table's
   answer (both) */
static int ver_mm_at(Compiler *c, int rcls, int sg, LoadPoint p, int at, const ElabWindow *w) {
  int oc = comp_core_class(c, CORE_OBJECT), own = cplan_mm_chain(c, rcls, sg), mm = ver_mm_held(c, rcls, sg, p, at, w, own);
  if (mm == 1 || rcls < 0 || (!sg && class_is_blank_slate(c, rcls))) return mm;
  int om = ver_mm_held(c, -1, 0, p, at, w, -1), cm = oc >= 0 && oc != rcls ? ver_mm_held(c, oc, 0, p, at, w, -1) : 2;
  if (own >= 0) return mm == 2 && ((om | cm) & 1) ? 3 : mm;
  return om == 1 || cm == 1 ? 1 : (mm | om | cm) == 2 ? 2 : 3;
}

static int cplan_capture_names(Compiler *c, int id, int cls, int sg, const char *name, int entry);
/* Does call id, asked with flags, make a Method of cls's `name` (side sg)
   that keeps the table's method on a chain a method body makes a version
   on: a capture whose NameError cannot be stated (cplan_capture_names)?
   (A respond_to? that asks as a capture reads the cells all the same.) */
static int ct_capture_keeps_table(Compiler *c, int id, int cls, int sg, const char *name, unsigned flags, int entry) {
  if (!(flags & CT_CAPTURE) || id < 0 || !is_method_obj_call(c, id)) return 0;
  int k = cplan_capture_names(c, id, cls, sg, name, entry);
  return k == CP_ABSENT_NONE || k == CP_ABSENT_UNTOLD;
}
/* The builtin class whose reopenings hold every version of `name` on
   program class rcls's chain (instance side), where rcls is a program
   class under it (`class Scalar < Numeric`): -1 for none. Its methods take
   a boxed self, and rcls holds none of its own, so a call on rcls meets the
   versions as the builtin's entry arms them (the table answered the
   builtin's method there too); an entry made for rcls would hand its own
   typed self to them. */
static int ct_builtin_versions_holder(Compiler *c, int rcls, const char *name) {
  ElabVersion vs[VER_MAX_ARMS];
  if (rcls < 0 || rcls >= c->nclasses || c->classes[rcls].is_builtin_const) return -1;
  int nv = elab_chain_versions(c, rcls, 0, name, vs, VER_MAX_ARMS), h = -1;
  if (nv < 1) return -1;
  for (int i = 0; i < nv; i++) {
    int node = vs[i].v->node, k = node >= 0 && node < c->node_cap ? c->node_cbody[node] : -1;
    if (k < 0 || (h >= 0 && k != h)) return -1;
    h = k;
  }
  return h != rcls && c->classes[h].is_builtin_const && !is_object_base_name(c->classes[h].name) ? h : -1;
}
/* Does a visibility close a version to a call of face? (Not to one that
   enforces none, nor to a variable-like call's, VF_VCALL, which is open
   to every version.) */
static int ver_face_closes(int face) {
  return face == VF_PRIVATE || face == VF_PUBLIC;
}
/* Is a version of visibility vis closed to a call of face? */
static int ver_closed_to(int vis, int face) {
  return ver_face_closes(face) && (vis == SP_VIS_PRIVATE || (face == VF_PUBLIC && vis == SP_VIS_PROTECTED));
}
/* The face a call asked with flags binds (VF_NONE: it enforces nothing) */
static int ct_flags_face(unsigned flags) {
  return !(flags & CT_PUBLIC) ? VF_NONE : flags & CT_PROTECTED ? VF_PUBLIC : VF_PRIVATE;
}
/* Are the versions of name on rcls's instance side, each with the
   visibility its arm takes (elab_version_vis: its def's, or a visibility
   word's the record proves), all closed to a call of face (1, *vis the
   first one's visibility), all open (0), or some of each (-1, also where
   one's is the run's and an entry arms it)? -2 where they cannot be told
   apart: no version, or one that is not Object's or the top level's. (An
   undef's arm raises for every face: ver_entry_make.) */
static int ver_arms_closed(Compiler *c, int rcls, const char *name, int face, int *vis) {
  int closed = 0, open = 0, oc = comp_core_class(c, CORE_OBJECT);
  ElabVersion vs[VER_MAX_ARMS];
  int nv = rcls == oc ? elab_chain_versions(c, rcls, 0, name, vs, VER_MAX_ARMS) : 0;
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE || vs[i].v->kind == LR_UNDEF) continue;
    /* (only Object's and the top level's own versions: a version of
       another root has a self of its own type, which the entry's boxed
       self cannot be passed to) */
    int sc = vs[i].v->scope, ci = sc > 0 && sc < c->nscopes ? c->scopes[sc].class_id : -2;
    if (ci != -1 && ci != oc) return -2;
    /* (one whose visibility only the run tells is both, where an entry
       arms it and tests its cell: over two or more versions; a lone one
       has no entry, and keeps its def's) */
    int v = elab_version_vis(c, &vs[i]);
    if (v < 0 && nv >= 2) closed = open = 1;
    else if (ver_closed_to(v = v < 0 ? vs[i].v->vis : v, face)) {
      if (!closed && vis) *vis = v;
      closed = 1;
    }
    else open = 1;
  }
  return !closed && !open ? -2 : closed && open ? -1 : closed;
}
/* The visibility the arm of the version of name whose def is node def
   on rcls's instance side takes (elab_version_vis; its def's where only
   the run tells: a call with a block binds no entry), -1 for none. */
static int ver_def_vis(Compiler *c, int rcls, const char *name, int def) {
  ElabVersion vs[VER_MAX_ARMS];
  int nv = elab_chain_versions(c, rcls, 0, name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++)
    if (vs[i].v->node == def) {
      int v = elab_version_vis(c, &vs[i]);
      return v >= 0 ? v : vs[i].v->vis;
    }
  return -1;
}
/* Does a version of name on program class rcls's chain (side sg) take a
   visibility only the run tells (elab_version_vis -1: a word under a
   condition, or one a method body runs), which its face entry's arm reads
   from the visibility cell (ver_vis_arm)? Asked of a name whose
   visibility varies (elab_method_vis_varies) only. */
static int ver_vis_runs(Compiler *c, int rcls, int sg, const char *name) {
  ElabVersion vs[VER_MAX_ARMS];
  if (rcls < 0 || rcls >= c->nclasses || c->classes[rcls].is_builtin_const || !elab_method_vis_varies(c, name)) return 0;
  for (int i = 0, nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS); i < nv; i++)
    if (vs[i].v->kind != LR_UNDEF && vs[i].v->kind != LR_REMOVE && elab_version_vis(c, &vs[i]) < 0) return 1;
  return 0;
}
/* ...or is its one version a body's def closed to a call of face (a protected one for any, ct_call_face closes it
   to the callers it is closed to) that a call may run ahead of (elab_method_late)?
   Its face entry's arm refuses it once the body has run, and finds no method before, as CRuby does: a refusal
   made at the call would say the method is private where it is not there yet. */
static int ver_lone_closed_late(Compiler *c, int rcls, int sg, const char *name, int face) {
  ElabVersion vs[VER_MAX_ARMS];
  if (rcls < 0 || rcls >= c->nclasses || c->classes[rcls].is_builtin_const || elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS) != 1) return 0;
  int vis = elab_version_vis(c, &vs[0]);
  return vs[0].v->kind == LR_DEF && vs[0].v->body && (ver_closed_to(vis, face) || vis == SP_VIS_PROTECTED) && elab_method_late(c, name);
}
/* ...or are its versions some closed to a call of face, some open, each
   with the visibility its arm takes (`private def m` and a body's public
   `def m`)? */
static int ver_faces_split(Compiler *c, int rcls, int sg, const char *name, int face) {
  ElabVersion vs[VER_MAX_ARMS];
  int closed = 0, open = 0;
  if (rcls < 0 || rcls >= c->nclasses || c->classes[rcls].is_builtin_const || !elab_method_vis_varies(c, name)) return 0;
  for (int i = 0, nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS); i < nv; i++) {
    if (vs[i].v->kind == LR_UNDEF || vs[i].v->kind == LR_REMOVE) continue;
    int v = elab_version_vis(c, &vs[i]);
    if (ver_closed_to(v < 0 ? vs[i].v->vis : v, face)) closed = 1;
    else open = 1;
  }
  return closed && open;
}
/* The face of the entry a call asked with flags binds on rcls's side sg,
   for a call that enforces visibility (CT_PUBLIC) on a root class's
   chain or on a class side; VF_NONE where the call does not, or where its versions are all
   closed to it or all open (the plan then answers the call as it does
   without versions of its own: it raises, or runs the entry). */
static int ct_face_for(Compiler *c, int rcls, int sg, const char *name, unsigned flags) {
  int face = ct_flags_face(flags), oc = comp_core_class(c, CORE_OBJECT);
  /* (only Object's versions are told apart, ver_arms_closed: another class
     answers none without a lookup, which every class a call may land on
     asked once per call; but where a version's visibility only the run
     tells, ver_vis_runs, or where its versions differ for the face,
     ver_faces_split; a class side's alike) */
  if (!face) return VF_NONE;
  if (rcls != oc || sg)
    return (ver_vis_runs(c, rcls, sg, name) || ver_faces_split(c, rcls, sg, name, face) || ver_lone_closed_late(c, rcls, sg, name, face)) && cplan_mm_method(c, -1, rcls, sg) < 0 ? face : VF_NONE;
  /* (a method_missing every object reaches answers the call instead) */
  if (cplan_mm_method(c, -1, oc, 0) >= 0) return VF_NONE;
  return ver_arms_closed(c, rcls, name, face, NULL) == -1 ? face : VF_NONE;
}
/* Does rcls's chain (its side sg) hold an undef of name? */
static int ver_chain_undef(Compiler *c, int rcls, int sg, const char *name) {
  ElabVersion vs[VER_MAX_ARMS];
  int nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++)
    if (vs[i].v->kind == LR_UNDEF) return 1;
  return 0;
}
static int ct_block_self_call(Compiler *c, int node);
static int block_self_call_in(Compiler *c, const int *par, int n, int node, const CplanTreeFacts *tree);
/* The self the block of call bsc (ct_block_self_call) runs with, as its
   receiver spells it: the class of an instance_eval's or instance_exec's
   `K.new` where no class method `new` of the program's is on K's chain
   (-1, no program class's instance, for a builtin class's,
   `Object.new`), for one with no receiver or on self the self of the
   block around it that may run with another self (the scope's self,
   caller, outside any), and -1 for an eval block on a constant, whose self is the class
   value; a bare `new` in a class method, outside any block that may run
   with another self, is an instance of the method's class where no class
   of the program's subclasses it. CPLAN_SELF_UNKNOWN for any other
   receiver or block (a local, a lambda, a define_method's, one handed to a
   method of the program's). A send of a literal name is the call it sends
   (call_sent_name), as the lowering that rewrites it into that call makes
   it, so an answer asked before that pass is the one asked after it.
   tree: asked of the tree and its parent map par alone, as the load
   record asks it (cplan_block_self_untold), whose facts tell which names
   the program defines and which classes it may subclass: a `new` is a
   class's own where it defines no method `new` at all */
static int ct_block_self_caller(Compiler *c, const int *par, int n, int bsc, int caller, const CplanTreeFacts *tree) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_kind(nt, bsc) == NK_CallNode ? call_sent_name(nt, bsc, NULL) : NULL;
  int ie = nm && is_instance_eval_family(nm), rv = nm ? nt_ref(nt, bsc, "receiver") : -1;
  if (!ie && !(nm && is_class_eval_family(nm))) return CPLAN_SELF_UNKNOWN;
  NodeKind rk = rv >= 0 ? nt_kind(nt, rv) : NK_SelfNode;
  /* (self there is the self of the block around bsc that may run with another, `K.new.instance_exec {
     instance_exec { ... } }`, else the scope's) */
  int out = rk == NK_SelfNode && ie ? block_self_call_in(c, par, n, bsc, tree) : -1;
  if (out >= 0) return ct_block_self_caller(c, par, n, out, caller, tree);
  if (rk == NK_SelfNode) return ie ? caller : -1;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return -1;
  int nw = ie && rk == NK_CallNode && is_new_name(call_sent_name(nt, rv, NULL)), nr = nw ? nt_ref(nt, rv, "receiver") : -1;
  const Scope *cs = nw && (nr < 0 || nt_kind(nt, nr) == NK_SelfNode) ? comp_scope_of(c, bsc) : NULL;
  int own = nw && (!tree || !tree->takes(tree->ctx, "new"));
  if (cs && cs->is_cmethod && cs->class_id >= 0 && block_self_call_in(c, par, n, bsc, tree) < 0)
    return own && (tree ? !tree->subclassed(tree->ctx, bsc) : cplan_user_new(c, -1, cs->class_id) < 0 && !class_has_subclass(c, cs->class_id))
           ? cs->class_id : CPLAN_SELF_UNKNOWN;
  if (nr < 0 || (nt_kind(nt, nr) != NK_ConstantReadNode && nt_kind(nt, nr) != NK_ConstantPathNode)) return CPLAN_SELF_UNKNOWN;
  if (tree) return own ? -1 : CPLAN_SELF_UNKNOWN;
  int ci = const_node_class_index(c, nr);
  /* (a class of the program's own `new` on its chain, wherever the program
     defines it, may answer anything: cplan_user_new) */
  if (ci >= 0 && ci < c->nclasses && cplan_user_new(c, -1, ci) >= 0) return CPLAN_SELF_UNKNOWN;
  return ci >= 0 && ci < c->nclasses ? ci : -1;
}
/* See call_plan.h. */
int cplan_protected_caller(Compiler *c, int id, int caller) {
  int n = 0, framed = id >= 0 ? elab_plan_enter(c, id) : 0;
  const int *par = elab_node_parents(c, &n);
  int bsc = block_self_call_in(c, par, n, id, NULL), r = bsc >= 0 ? ct_block_self_caller(c, par, n, bsc, caller, NULL) : caller;
  if (framed) elab_plan_leave(c);
  return r;
}
/* See call_plan.h. */
int cplan_block_self_untold(Compiler *c, const int *par, int n, int node, const CplanTreeFacts *tf) {
  int bsc = block_self_call_in(c, par, n, node, tf);
  return bsc >= 0 && ct_block_self_caller(c, par, n, bsc, -1, tf) == CPLAN_SELF_UNKNOWN;
}
/* May the self of a method of class caller be kind_of? module owner at some call: is caller, or a class below it (a
   subclass, or the singleton class an object's `extend` makes, whose parent is the object's class), owner's
   descendant, or does it or a superclass include or prepend owner at any time (chain_mixes_in)? A self of any of
   them runs caller's method, and a later or conditional mixin may have run where the call does
   (cplan_caller_includes_module is the test that proves it for every call) */
static int caller_may_be_kind_of(Compiler *c, int caller, int owner) {
  for (int d = 0; caller >= 0 && d < c->nclasses; d++) {
    if (!is_descendant(c, d, caller)) continue;
    if (is_descendant(c, d, owner) || chain_mixes_in(c, d, owner, 0)) return 1;
  }
  return 0;
}
/* Is a protected version of name on program class rcls's instance chain
   closed to call id: is its caller's self (an instance of the class its
   scope is in; a class in a class method; main at the top level; in a
   block that may run with another self, that self, cplan_protected_caller)
   no instance of a class on the chain that holds a version of the name,
   or of one that includes a module whose version the chain holds, the
   owner a protected method is open to (cplan_protected_owner_open)?
   Where nothing tells the caller's class, the scope's self is the caller,
   as where the record holds no body versions (method_vis_for_caller): the
   versions such a call meets are the ones that form has too, since the
   record keeps a method body's protected word or def of a name such a
   call names in the unplaced form (lr_body_forms) */
static int ver_protected_closed(Compiler *c, int id, int rcls, const char *name) {
  if (id < 0 || !elab_method_may_be_protected(c, name)) return 0;
  const Scope *cs = comp_scope_of(c, id);
  int scope = cs && !cs->is_cmethod ? cs->class_id : -1, caller = cplan_protected_caller(c, id, scope);
  if (caller == CPLAN_SELF_UNKNOWN) caller = scope;
  ElabVersion vs[VER_MAX_ARMS];
  for (int a = rcls, k = 0; a >= 0 && a < c->nclasses && k <= c->nclasses; a = c->classes[a].parent, k++)
    if (elab_history_versions(c, a, 0, name, vs, 1) > 0 && !cplan_protected_owner_open(c, caller, 0, a)) return 1;
  /* (and a module's history on the chain: an include's or a prepend's copy is the module's method, whose owner a
     protected one is open to is the module, open to a caller whose self may be kind_of? it: caller_may_be_kind_of) */
  for (int i = 0, nv = elab_chain_versions(c, rcls, 0, name, vs, VER_MAX_ARMS); i < nv; i++) {
    int sc = vs[i].v->scope, own = sc > 0 && sc < c->nscopes ? c->scopes[sc].class_id : -1;
    if (own >= 0 && own < c->nclasses && comp_class_is_module(c, &c->classes[own]) && !caller_may_be_kind_of(c, caller, own)) return 1;
  }
  return 0;
}
/* The face of the entry call id binds on rcls's chain (side sg): a
   variable-like call's (nt_vcall) where the chain holds an undef, whose arm
   raises NameError for it (VF_VCALL), or where it may meet no version
   (none), whose else arm does; else ct_face_for's, which for a program
   class's chain tests the protected visibility too where a protected
   version is closed to the call (ver_protected_closed: CT_PROTECTED tells
   only Object's owner apart, object_protected_open). (Elsewhere its entry
   is the one every other call takes.) */
static int ct_call_face(Compiler *c, int id, int rcls, int sg, const char *name, unsigned flags, int none) {
  if (id >= 0 && nt_kind(c->nt, id) == NK_CallNode && nt_vcall(c->nt, id) && (none || ver_chain_undef(c, rcls, sg, name))) return VF_VCALL;
  /* (a call that enforces visibility, which may meet none of a class's definition hook: the core's private hook
     raises for it, where a send's runs it, ct_core_hook) */
  if (none && sg && id >= 0 && cplan_call_enforces_vis(c, id) && is_definition_hook_name(name)) return VF_PRIVATE;
  int face = ct_face_for(c, rcls, sg, name, flags);
  if (face == VF_PRIVATE && !sg && rcls != comp_core_class(c, CORE_OBJECT) && ver_protected_closed(c, id, rcls, name)) face = VF_PUBLIC;
  return face;
}
/* The face of the entry of a late layer in front of cls's chain (pos_entry) call id binds: ct_call_face's, for a call
   that enforces visibility (ct_vis_flags) where a version on the chain takes a visibility only the run tells, or
   where its versions differ for the face; VF_NONE otherwise, and for a variable-like call's, which a layer's else arm
   does not raise for */
static int ct_layer_face(Compiler *c, int id, int cls, int sg, const char *name, unsigned flags) {
  int face = id >= 0 ? ct_call_face(c, id, cls, sg, name, ct_vis_flags(c, id, name, flags), 0) : VF_NONE;
  return ver_face_closes(face) ? face : VF_NONE;
}
/* The version of `name` call id runs, when the method has more than one
   (#9898), or one the call may run before (elab_versions_late): a call at
   a load point gets the version live there; any other (a load point in a
   body that runs again, a method body, a block kept to run later), the
   one version it can reach over the statements it may run in
   (elab_call_window, elab_method_span), or, where it can reach two or
   more, rcls's entry, which calls the one the run-time cells name; an
   alias that is the last version, the version it captured. A call that
   may run where rcls's chain holds no version yet gets the entry whose
   else arm answers for none (VerEntry.absent): asked once the build is
   over, of the classes the fixpoint finds the receiver can be
   (ver_absent_note), and made by ver_entries_demand. mi, the table's
   answer (the last version), where the record cannot tell or no entry
   was made. Where the record cannot tell over a window rcls's chain moves
   in, or one before the chain holds still (a mixin that may not have run:
   a conditional include, one in a block), and mi is one of the copies a
   class holds of a module's versions (mixin_copies_module), the entry
   over the copies, whose cells tell which version the module holds (the
   mixin taken as run, as with one version). *ml: n, how many versions the
   window reaches (elab_method_span), 0 when no window was asked, -1 when
   the answer is mi for want of a version or an entry, or because the call
   is in a body that never runs (ElabWindow.never); copies set for the
   copies' entry, whose n is 2; kind and why (MethodLookup). */
static int mixin_copies_module(Compiler *c, int mi, int *to, ElabVersion *vs, int *n);
static int pos_front(Compiler *c, int mi);
static int pos_runs(Compiler *c, const Scope *s, int t, int *to, int *layer);
static int pos_runs_front(Compiler *c, int mi, int *to);
/* The entry a call that reaches mi, a late layer the class's table answers (pos_front), runs: over mi's versions,
   its else arm what lies behind it (pos_entry), of the face the call binds where a version's visibility only the run
   tells (ct_call_face: its arms raise for the visibility the run gives); mi, with ML_TABLE, when asked before the
   entries are made */
static int ct_front_layer(Compiler *c, int id, int rcls, int mi, unsigned flags, MethodLookup *ml) {
  int to[VER_MAX_ARMS], n = pos_runs_front(c, mi, to);
  const Scope *m = &c->scopes[mi];
  const char *name = comp_scope_user_name(m);
  int ar = ver_arity(c, id, to, n), face = c->ver_built ? ct_layer_face(c, id, m->class_id, m->is_cmethod, name, flags) : VF_NONE;
  int ix = c->ver_built ? ver_entry_ix(c, VER_ENTRY_LAYER, mi + 1, m->class_id, m->is_cmethod, name, ar, face) : -1;
  if (ix < 0 && face) ix = ver_entry_ix(c, VER_ENTRY_LAYER, mi + 1, m->class_id, m->is_cmethod, name, ar, VF_NONE);
  ml->n = 2;
  if (ix < 0) {
    ml->kind = ML_TABLE;
    ml->why = MLW_MANY;
    return mi;
  }
  if (!(flags & CT_TOP)) ml->def_class = m->class_id;
  ml->kind = ML_ENTRY;
  return c->ver_entries[ix].scope;
}
static int ct_version_at(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi, MethodLookup *ml) {
  /* (a presence reader's own ask, respond_to? or method_defined?, reads the visibility cells of the face entry an
     enforcing call there takes: presence_cells) */
  unsigned ff = (flags & CT_REACH) && id >= 0 && nt_kind(c->nt, id) == NK_CallNode && is_method_presence_name_query(call_sent_name(c->nt, id, NULL)) ?
                flags | CT_PUBLIC : flags;
  int sg = (flags & CT_CMETHOD) != 0, rcls = flags & CT_TOP ? ct_top_chain(c, name, sg) : cls, vi = -1, dc = -1, n = 0, first_def = 0, none = 0, body = 0, cl = 0;
  /* (the table's answer a late layer in front of the chain: its entry, whose arms tell the live version) */
  if (mi >= 0 && !(flags & CT_TOP) && pos_front(c, mi)) return ct_front_layer(c, id, rcls, mi, flags, ml);
  /* (a program class under a reopened builtin that holds every version: the builtin's chain) */
  { int bh = !sg && rcls >= 0 ? ct_builtin_versions_holder(c, rcls, name) : -1; if (bh >= 0) rcls = bh; }
  const LrDef *d = NULL, *ds[VER_MAX_ARMS];
  /* a call may meet the chain with no version made yet: the late bit of
     its histories, or a version a method body makes on the chain (its
     extent opens where that body first runs). A capture too: where no
     version is live, its Method raises NameError (cplan_capture_absent);
     one that cannot state that error (cplan_capture_names) keeps the
     table's method, as a capture reader keeps a name off the late bit */
  int nc = ver_chain_count(c, rcls, sg, name, &first_def, &body, &cl);
  int kernel_callback = kernel_string_implies(c, id, rcls, name, flags);
  int late = (cl == 2 || body || kernel_callback) && !ct_capture_keeps_table(c, id, rcls, sg, name, flags, 1);
  /* one def on rcls's whole chain, which no call runs ahead of: the
     table's (a name with versions on other chains only) */
  if (nc == 1 && first_def && !late) {
    ml->n = 0;
    return mi;
  }
  LoadPoint p = elab_point_of(c, id);
  /* An unplaced capture chooses at construction through the existing
     entry, even when a later wrapper changes the call's live window. Not
     where a reader that answers from the table names the method
     (elab_method_reflected, the fact that keeps the name off the late
     bit): a respond_to? guarding the capture says it is there, so the
     capture binds the table's method too, as the guarded call does. The
     fact is the side's: a class method's respond_to? that reads the
     entry's cells (cplan_cmethod_presence) asks only where it does not
     hold, and an instance-side reader leaves a class-side capture on the
     entry. */
  if ((flags & CT_CAPTURE) && p.region < 0 && elab_method_reflected(c, name, sg)) {
    ml->kind = ML_TABLE;
    ml->why = MLW_UNPLACED;
    ml->n = 0;
    return mi;
  }
  if ((flags & CT_CAPTURE) && p.region < 0 &&
      (vi = ct_entry_of(c, VER_ENTRY_CHAIN, rcls, sg, name, ver_call_arity(c, id, rcls, sg, name, mi))) >= 0) {
    if (!(flags & CT_TOP)) ml->def_class = rcls;
    ml->kind = ML_ENTRY;
    ml->n = 2;
    return vi;
  }
  ElabMeet m;
  int kept = ct_meet_after_kept(c, id, !ml->survey && c->ver_built && !elab_check_on(c));   /* (it runs at its point only if nothing around it runs later, nor the record keeps a block around it to run at another time) */
  const Scope *own = comp_scope_of(c, id);
  elab_call_meet(c, id, rcls, name, sg, kept | (own && own->origin_module_ci > 0 ? ELAB_MEET_COPY : 0), &m, ds, VER_MAX_ARMS);
  /* (a call in a body that never runs meets no version at any time: the
     table's, the cheapest call, not the window's last) */
  if (m.at == ELAB_UNKNOWN && m.win == ELAB_YES && m.w.never) {
    ml->kind = ML_TABLE;
    ml->why = MLW_NEVER;
    ml->n = -1;
    return mi;
  }
  int at = m.at;
  ElabWindow w = m.w;
  d = m.d;
  if (kernel_callback) {
    const LrDef *only = at == ELAB_NO ? d : at == ELAB_UNKNOWN && m.win == ELAB_YES && m.n == 1 ? ds[0] : NULL;
    if (at == ELAB_NO || (only && only->kind == LR_UNDEF)) {
      ml->kind = ML_NONE;
      ml->why = only && only->kind == LR_UNDEF ? MLW_UNDEF : MLW_RECORD;
      return -1;
    }
  }
  /* (whether the call asks rcls's class object, live from its header on, where no instance may be yet: a class
     method's call, or a reach ask's reader, a list or a method_defined?) */
  int on_class = sg || (flags & CT_REACH);
  if (at == ELAB_YES) {
    vi = ct_def_version(c, rcls, name, sg, d, &dc);
    if (vi < 0 && (flags & CT_CAPTURE) && d && d->kind == LR_DEF && rcls >= 0) vi = comp_shadowed_body(c, rcls, sg, d->node, &dc);
  }
  /* (not an undef's, nor on a class no call can run on yet) */
  else if (at == ELAB_NO) none = late && !d && elab_class_live_at(c, rcls, on_class, p);
  else if (m.win == ELAB_YES) {
    n = m.n;
    for (int k = 0; k < n && k < VER_MAX_ARMS; k++) none |= late && !ds[k];
    if (n == ELAB_SPAN_ANY) none |= body;   /* (a method body's version may not be made yet) */
    if (n == 1 && ds[0]) vi = ct_def_version(c, rcls, name, sg, d = ds[0], &dc);
  }
  ElabVersion cvs[VER_MAX_ARMS];
  int cto[VER_MAX_ARMS], cn = 0;
  if (vi < 0 && at == ELAB_UNKNOWN && m.win == ELAB_YES && n != 0 && n != 1 &&
      (n == -1 || !elab_chain_settled(c, rcls, sg, name, w.from)) && mixin_copies_module(c, mi, cto, cvs, &cn) >= 0) {
    int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, c->scopes[mi].origin_module_ci, rcls, sg, name, ver_call_arity(c, id, rcls, sg, name, mi), VF_NONE);
    ml->n = 2;
    ml->copies = 1;
    if (ix < 0) {
      ml->kind = ML_TABLE;
      ml->why = m.n == -1 ? MLW_MOVES : MLW_MANY;
      return mi;
    }
    if (!(flags & CT_TOP)) ml->def_class = rcls;
    ml->kind = ML_ENTRY;
    return c->ver_entries[ix].scope;
  }
  if (vi < 0 && (n >= 2 || n == ELAB_SPAN_ANY || none)) {
    /* the entry, where the chain holds still over the window: asked of
       an entry that was made, or by the entries' build (span); for a
       call that only meets no version, one that answers for none */
    int ar = ver_call_arity(c, id, rcls, sg, name, mi), face = ct_call_face(c, id, rcls, sg, name, ff, none);
    int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, rcls, sg, name, ar, face), own = ix;
    if (ix < 0 && face) ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, rcls, sg, name, ar, VF_NONE);   /* (no face entry made: the entry every arm of which runs) */
    int many = n >= 2 || n == ELAB_SPAN_ANY;
    int settled = at == ELAB_NO || elab_chain_settled(c, rcls, sg, name, w.from);
    const Scope *in = comp_scope_of(c, id);
    /* (an entry only where the call takes it, in a body that may run; an
       Kernel.String's Symbol callback can also look up a late history. A body
       the fixpoint has not reached yet may be reached later: its answer is
       not kept, MethodLookup.unsure) */
    ml->unsure = in && c->ver_built && !comp_scope_may_run(c, (int)(in - c->scopes));
    /* (a variable-like call's entry raises NameError where none is live, an enforcing call's of a class's hook
       NoMethodError (ct_call_face), and one whose face entry tests a version's visibility its arms read (ct_face_for):
       its own face's) */
    int nface = face, nix = nface ? own : ix;
    /* (...where its else arm raises: one that runs a method or the core's
       no-op is the entry of no face, VerEntry.anyform) */
    if (face == VF_VCALL && own < 0 && ix >= 0 && c->ver_entries[ix].anyform) nface = VF_NONE, nix = ix;
    if (none && settled && in && comp_scope_may_run(c, (int)(in - c->scopes)) && c->ver_built && !ml->survey &&
        (nix < 0 || !c->ver_entries[nix].absent) &&
        (!(flags & CT_IMPLIED) || kernel_callback))
      ver_absent_note(c, rcls, sg, name, mi, ar, ver_mm_at(c, rcls, sg, p, at, &w), 0, nface);
    /* a call the build did not ask (one in a method made after it, a
       dynamic send's arm) whose window spans versions asks the entry the
       build makes for such a call, of the face the call binds
       (ct_vis_flags: also where this ask only reads the tables) */
    else if (many && own < 0 && !(flags & CT_IMPLIED) && settled && in && comp_scope_may_run(c, (int)(in - c->scopes)) && c->ver_built && !ml->survey)
      ver_absent_note(c, rcls, sg, name, mi, ar, 0, 1, ct_face_for(c, rcls, sg, name, ct_vis_flags(c, id, name, 0)));
    vi = ix >= 0 && (many || c->ver_entries[ix].absent == 2) ? c->ver_entries[ix].scope : -1;
    n = ((ml->survey && many) || vi >= 0) && settled ? 2 : 0;
    if (!n) vi = -1;
    dc = rcls;
  }
  else if (vi < 0) n = 0;
  if (vi < 0 && !kept && elab_method_at(c, rcls, name, sg, p, &d) == ELAB_YES && d && d->kind == LR_ALIAS)
    vi = ct_def_version(c, rcls, name, sg, d, &dc);
  ml->n = n;
  /* only a version that is a function of its own: a replaced one
     (lr_mark_versions), an entry, or one no later scope of its name
     shadows; not one a scope no def made (a define_method's) passes over.
     (n -1: no version told and no entry asked, the table's answer) */
  if (vi < 0 || (!c->scopes[vi].replaced && !c->scopes[vi].entry && scope_is_shadowed(c, vi))) {
    ml->kind = ML_TABLE;
    ml->why = vi >= 0 || at == ELAB_YES ? MLW_UNMAPPED : at == ELAB_NO ? MLW_DISAGREE : m.win != ELAB_YES ? MLW_UNPLACED
            : m.n == -1 ? MLW_MOVES : m.n == 1 && ds[0] ? MLW_UNMAPPED : MLW_MANY;
    if (!n) ml->n = -1;
    return mi;
  }
  /* (a capture on Object's chain takes it: `Object.instance_method(:m)`
     names the one entry, Object's, whichever history defined it) */
  if (ct_top_version(c, vi, flags) && !(flags & CT_CAPTURE)) {
    ml->kind = ML_NONE;
    return -1;
  }
  /* (and the other way: a method of Object's, or its entry, that a call
     on main meets along Object's chain is bound by the call's ask on
     Object, which passes main as self) */
  if (rcls >= 0 && (flags & CT_TOP) && c->scopes[vi].class_id >= 0) {
    ml->kind = ML_OBJECT;
    return -1;
  }
  /* (a version whose visibility at the call the record cannot place, a
     call that it may close takes the face entry of, whose arm reads the
     visibility cell: ct_face_for, ver_vis_runs; asked by the survey. A
     call that meets versions after its point, kept, has no visibility at
     its point: one in a block the record keeps runs at a time the syntax
     does not tell, which the fixpoint may find after the build, blk_kept:
     ver_entries_demand then makes the entry. Nor does a class method's
     call on a receiver that names no one class where it is emitted (a
     block's parameter, `self.class` in an instance_eval block): its arms
     switch on the value's class, and fold no visibility as one on a
     constant or self does, emit_cmethod_vis_refusal. A reader's ask of the
     name, respond_to?'s, is the reader's: it folds the record's) */
  int face = !(flags & CT_TOP) && !c->scopes[vi].entry ? ct_call_face(c, id, rcls, sg, name, ff, 0) : VF_NONE;
  const char *own_nm = face && sg && id >= 0 && nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  int rv = own_nm && sp_streq(own_nm, name) && elab_method_body_word(c, name, sg) ? nt_ref(c->nt, id, "receiver") : -1;
  if (face && (kept || (rv >= 0 && class_recv_static_ci(c, rv) < 0 && self_class_static_ci(c, rv) < 0) ||
               elab_method_vis_at(c, rcls, name, sg, p) < 0)) {
    int ar = ver_call_arity(c, id, rcls, sg, name, mi), ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, rcls, sg, name, ar, face);
    if (ix < 0 && !(flags & CT_IMPLIED) && c->ver_built && !ml->survey && own && comp_scope_may_run(c, (int)(own - c->scopes)))
      ver_absent_note(c, rcls, sg, name, mi, ar, 0, 1, face);
    /* (an entry the build asked for but made no method of keeps scope -1,
       ver_entry_make: as with none asked, the version stays) */
    int ex = ix >= 0 ? c->ver_entries[ix].scope : -1;
    if (ex >= 0) vi = ex, dc = rcls;
    ml->n = ex >= 0 || ml->survey ? 2 : ml->n;
  }
  if (!(flags & CT_TOP)) ml->def_class = dc;
  ml->kind = c->scopes[vi].entry ? ML_ENTRY : ML_METHOD;
  return vi;
}
/* The version ct_version answered for call id asked on cls, kept once the
   entries are made (Compiler.ver_built): the record's facts for the call
   are decided at the plan's first ask (elab_plan_enter), and the scopes
   and entries no longer change, so a call asked again (each round of the
   fixpoint asks) gets the version it got. Its slot, a free one where none
   is kept. */
static struct VerMemo *ver_memo_slot(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if (2 * (c->ver_memo_n + 1) > c->ver_memo_cap) {   /* grown: every kept answer moves */
    struct VerMemo *old = c->ver_memo;
    int ocap = c->ver_memo_cap;
    c->ver_memo_cap = ocap ? 2 * ocap : 1024;
    if (!(c->ver_memo = calloc((size_t)c->ver_memo_cap, sizeof *c->ver_memo))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->ver_memo_n = 0;
    for (int i = 0; i < ocap; i++)
      if (old[i].name) {
        struct VerMemo *m = ver_memo_slot(c, old[i].id, old[i].cls, old[i].name, old[i].flags);
        *m = old[i];
        c->ver_memo_n++;
      }
    free(old);
  }
  unsigned h = ver_mix(((unsigned)id * 0x9E3779B1u) ^ (((unsigned)cls + 0x7F4A7C15u) * 0x85EBCA6Bu) ^ (flags * 0x27D4EB2Fu));
  for (int i = (int)(h & (unsigned)(c->ver_memo_cap - 1));; i = (i + 1) & (c->ver_memo_cap - 1)) {
    struct VerMemo *m = &c->ver_memo[i];
    if (!m->name || (m->id == id && m->cls == cls && m->flags == flags && sp_streq(m->name, name))) return m;
  }
}
/* ...asked in the record's frame for the call plan (elab_plan_enter): the
   version is decided from the facts the record had for the call when the
   plan first asked, not from a map rebuilt per ask */
static int ct_version(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi, MethodLookup *ml) {
  int memo = !ml->survey && c->ver_built && !elab_check_on(c);
  const struct VerMemo *m = memo ? ver_memo_slot(c, id, cls, name, flags) : NULL;
  if (m && m->name) {
    if (m->dc != -2) ml->def_class = m->dc;
    ml->kind = m->kind; ml->why = m->why; ml->copies = m->copies; ml->n = m->n;
    return m->vi;
  }
  int framed = elab_plan_enter(c, id), dc = ml->def_class;
  MethodLookup at = *ml;
  if (memo) at.def_class = -2;   /* (the class it answers, -2 for the table's) */
  int vi = ct_version_at(c, id, cls, name, flags, mi, &at);
  if (memo && !at.unsure) {
    *ver_memo_slot(c, id, cls, name, flags) = (struct VerMemo){ name_dup(name), id, cls, flags, vi, at.def_class, at.kind, at.why, at.copies, at.n };
    c->ver_memo_n++;
  }
  *ml = at;
  if (ml->def_class == -2) ml->def_class = dc;
  if (framed) elab_plan_leave(c);
  if (framed && elab_check_on(c)) {   /* the map as it is, for check mode */
    MethodLookup now = { .def_class = dc };
    elab_check_version(c, id, vi, ml->def_class, ct_version_at(c, id, cls, name, flags, mi, &now), now.def_class);
  }
  return vi;
}
/* The builtin self kind (BSELF_*) of program class self, main for -1 */
static int cm_self_kind(Compiler *c, int self) {
  if (self < 0) return BSELF_MAIN;
  return comp_class_is_module(c, &c->classes[self]) ? BSELF_MODULE : BSELF_CLASS;
}
/* Does call id run where no method of `name` is live yet, as the record
   proves it at the call (elab_call_method): cls's chain, its class side
   for CT_CMETHOD, main's top-level functions for CT_TOP, for CT_OWN the
   method mi the tables hold as cls's own (elab_call_own_method)? The method the
   tables answer is then one a later statement defines, and the call
   raises (cplan_missing) or reaches the builtin self answers
   (cplan_self_builtin). Asked in the record's frame, once it is made.
   Only for a class's class methods and main's functions: a call on an
   instance keeps its tables' target, which its dispatch arms are built
   on, and so does a call on an explicit receiver of a name a builtin
   answers on any self (`U.format`, private in CRuby). */
/* ...and the top-level reopening of a builtin class or module whose
   values carry its class id (builtin_class_id: the roots, Array, String,
   Comparable, StandardError; not Random, Method or LoadError, which have
   none, nor a class of the program's own, which keeps the tables' target
   its dispatch arms are built on), on its instance side: a call the
   record proves runs before the reopening's method of `name` is defined
   reaches the builtin's method of the name, as in CRuby (#10719: `p
   a.sum` ahead of `class Array; def sum`), where the builtin has one, of
   its own or of a module it includes (Comparable's between?, Enumerable's
   tally); where it has none, the call finds no method, and its
   NoMethodError is the undefined method's (`nil.foo` ahead of `class
   Object; private def foo`, `[1].foo` ahead of `class Array; def foo`) */
static int ct_builtin_reopen_undefined_at(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if ((flags & (CT_CMETHOD | CT_TOP | CT_OWN)) || cls < 0 || cls >= c->nclasses || !c->classes[cls].name ||
      c->classes[cls].enclosing_class >= 0 || !builtin_class_id(c->classes[cls].name) ||
      !elab_method_versions(c, name))
    return 0;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, cls, name, 0, 1) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none;
}
/* Does call id, asked for class method `name` (flags as call_target's),
   reach the core's definition hook where no version of the program's is
   live: one a call that does not enforce visibility (a send) runs, the
   private no-op row (builtin_ops.c), as where the program defines none? A
   call that enforces it raises NoMethodError for it. */
static int ct_core_hook(Compiler *c, int id, const char *name, unsigned flags) {
  return id >= 0 && (flags & CT_CMETHOD) && !cplan_call_enforces_vis(c, id) && is_definition_hook_name(name) && bop_find(TY_CLASS, name, 1, 0);
}
/* ct_undefined_at's ask of the record, once the chain it asks on (rcls) is known: the side of the call's own receiver
   and the builtin self methods apart from it (self_known: builtin_self_method_known of the name, -1 to ask it here). */
static int ct_undefined_in(Compiler *c, int id, int cls, int rcls, const char *name, unsigned flags, int mi, int self_known) {
  int sg = (flags & CT_CMETHOD) != 0;
  /* CT_OWN asks of the method the tables hold as cls's own (mi): for its
     own def, cls's own history at the call, whatever an ancestor or an
     extended module holds; for a module's copy (an extend's), that and
     the side's mixins, each from its point on; for an inherited copy, the
     chain, as a call there */
  const Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  int copy = m && (m->origin_module_ci > 0 || m->is_extend_copy), own = (flags & CT_OWN) && !(m && m->is_cmethod_copy && !copy);
  int framed = elab_plan_enter(c, id);
  int none = (own ? elab_call_own_method(c, id, rcls, name, sg, copy) : elab_call_method(c, id, rcls, name, sg, 1)) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none && !(nt_ref(c->nt, id, "receiver") >= 0 && !nt_int(c->nt, id, "self_call", 0) &&
                   (self_known >= 0 ? self_known : builtin_self_method_known(name, cm_self_kind(c, flags & CT_TOP ? -1 : cls))) &&
                   !((flags & CT_CMETHOD) && elab_record_cmethod(c, cls, name)));
}
static int ct_undefined_at(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi) {
  if (ct_builtin_reopen_undefined_at(c, id, cls, name, flags)) return 1;
  if (!(flags & (CT_CMETHOD | CT_TOP)) || !elab_method_versions(c, name)) return 0;
  /* (a capture that cannot state the NameError keeps the table's method
     on a chain a method body makes a version on, as in ct_version_at) */
  int sg = (flags & CT_CMETHOD) != 0, rcls = flags & CT_TOP ? ct_top_chain(c, name, sg) : cls, first_def = 0, body = 0, cl = 0;
  if (ct_capture_keeps_table(c, id, rcls, sg, name, flags, 0) && (ver_chain_count(c, rcls, sg, name, &first_def, &body, &cl), body))
    return 0;
  return ct_undefined_in(c, id, cls, rcls, name, flags, mi, -1);
}
/* Does call id run before every version of `name` on cls's route (flags
   and mi as ct_undefined_at's), as the record proves it: none is live at
   its point (ct_undefined_at), it does not run any time after it
   (ct_after_point), and every block around it runs at once
   (elab_blocks_run_at_once)? A presence question there finds none at
   every run (call_target's MLW_RECORD for a name with no method scope,
   and cplan_cmethod_later). */
static int ct_before_versions(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi) {
  if (!ct_undefined_at(c, id, cls, name, flags, mi)) return 0;
  int framed = elab_plan_enter(c, id);
  int before = !ct_after_point(c, id) && elab_blocks_run_at_once(c, id);
  if (framed) elab_plan_leave(c);
  return before;
}
/* Does the copy a class-side alias of the name makes (Scope.alias_of) in a
   class of ci's chain below the one declaring the name's `class << self`
   accessor stand in front of the accessor ci inherits? The alias is that
   class's own class method, which a call of the name on ci runs, as CRuby's
   lookup meets it first (bound by name, it was the alias's target the call
   took, past the accessor). A def of the name there still leaves the call
   on the accessor's slot, whose super into the accessor has no method. */
static int sg_accessor_shadowed(Compiler *c, int id, int ci, const char *name) {
  for (int k = ci, g = 0; k >= 0 && g < c->nclasses; k = c->classes[k].parent, g++) {
    if (comp_sg_accessor_own(c, k, name)) return 0;
    int m = call_target(c, id, k, name, CT_CMETHOD | CT_OWN, NULL);
    if (m >= 0 && c->scopes[m].alias_of) return 1;
    if (k == c->classes[k].parent) break;
  }
  return 0;
}
/* See call_plan.h. */
int cplan_sg_accessor(Compiler *c, int id, int ci, const char *name, char *slot, size_t cap) {
  if (sg_accessor_shadowed(c, id, ci, name)) return CP_SG_NONE;
  if (name_is_plain_setter(name)) {
    if (!comp_sg_writer_reached(c, ci, name, slot, cap)) return CP_SG_NONE;
  }
  else {
    const char *rn = comp_resolve_cm_alias_at(c, ci, name, NULL);
    if (!comp_is_sg_reader(&c->classes[ci], rn ? rn : name)) return CP_SG_NONE;
    snprintf(slot, cap, "%s", rn ? rn : name);
  }
  /* the slot where the call runs the accessor's own version, and where the
     record tells no version (master's answer: the table's would be the
     def's, which nothing proves runs there) */
  return cplan_sg_version(c, id, ci, name) == CP_SG_METHOD ? CP_SG_METHOD : CP_SG_SLOT;
}
/* See call_plan.h. */
int cplan_sg_version(Compiler *c, int id, int ci, const char *name) {
  /* (a name whose class-side visibility a method body's word sets: the method made of the accessor, its alias's,
     the def or the entry, whose faces read the word's cells; the slot reads none) */
  int word = id >= 0 && elab_method_body_word(c, name, 1);
  if (id < 0 || (!word && !elab_attr_replaced(c, ci, 1, name))) return CP_SG_NONE;
  mtab_enter(c);
  int m = call_target(c, id, ci, name, CT_CMETHOD, NULL), r;
  MethodLookup told = { .survey = 1, .def_class = -1 };
  if (m < 0 || (c->scopes[m].attr_version && !word)) r = CP_SG_SLOT;
  else if (word && scope_of_accessor(&c->scopes[m])) r = CP_SG_METHOD;
  else {
    if (!c->scopes[m].entry) ct_version(c, id, ci, name, CT_CMETHOD, m, &told);
    r = told.n < 0 ? CP_SG_UNTOLD : CP_SG_METHOD;
  }
  mtab_leave(c);
  return r;
}
/* See call_plan.h. */
int cplan_sg_slot_call(Compiler *c, int id, int ci, const char *name, int mi) {
  char slot[256];
  if (mi < 0 || !c->scopes[mi].attr_version || !c->scopes[mi].is_cmethod) return 0;
  const char *nm = id >= 0 && nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  return nm && name && sp_streq(nm, name) && cplan_sg_accessor(c, id, ci, name, slot, sizeof slot) == CP_SG_SLOT;
}
/* See call_plan.h. */
int cplan_cmethod_later(Compiler *c, int id, int cls, const char *name) {
  int asked = id >= 0 ? (int)nt_int(c->nt, id, "defined_ask", 0) : 0;
  if (asked) id = asked - 1;   /* defined?'s respond_to?: at the call it asks of */
  return id >= 0 && cls >= 0 && ct_before_versions(c, id, cls, name, CT_CMETHOD, -1);
}
/* (the class whose class side class-side reader id, on a constant or self, asks of) */
static int cells_reader_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return class_recv_static_ci(c, recv);
  if (rk != NK_SelfNode) return -1;
  const Scope *s = comp_scope_of(c, id);
  if (s && s->class_id >= 0) return s->is_cmethod ? s->class_id : -1;
  return s && c->node_cbody && id < c->node_cap ? c->node_cbody[id] : -1;
}
/* See call_plan.h. */
int cplan_presence_class(Compiler *c, int id) {
  return respond_to_cells_shape(c->nt, id) ? cells_reader_class(c, id) : -1;
}
static int cplan_presence_asks(Compiler *c, int id, int cls, const char *name);
/* See call_plan.h. */
int cplan_list_class(Compiler *c, int id, int *own) {
  const NodeTable *nt = c->nt;
  if (list_cells_shape(nt, id) != 1) return -1;
  int ac = 0;
  const int *av = call_args(nt, id, &ac);
  *own = ac == 1 && nt_kind(nt, av[0]) == NK_FalseNode;
  int ci = class_recv_static_ci(c, nt_ref(nt, id, "receiver"));
  return ci >= 0 && !c->classes[ci].is_builtin_const ? ci : -1;
}
/* The names class-side list id asks the cells of: each candidate of the list the cells may answer
   (cplan_presence_asks) */
typedef struct { Compiler *c; int id, cls; void (*fn)(Compiler *, int, const char *, int, void *); void *arg; } ListAsk;
static void list_ask_name(void *ud, const char *name, int vis) {
  ListAsk *a = ud;
  (void)vis;
  if (cplan_presence_asks(a->c, a->id, a->cls, name)) a->fn(a->c, a->id, name, a->cls, a->arg);
}
static void list_asks(Compiler *c, int id, void (*fn)(Compiler *, int, const char *, int, void *), void *arg) {
  int own, cls = cplan_list_class(c, id, &own);
  ListAsk a = { c, id, cls, fn, arg };
  if (cls >= 0) cplan_list_candidates(c, cls, CT_CMETHOD | (own ? CT_OWN : 0), list_ask_name, &a);
}
/* An instance-side presence reader's asks (cplan_presence_cells): a
   respond_to? (defined?'s) of a literal name on a receiver, or a
   method_defined? kin on a constant or on `x.class`, asks fn (flags 0) of
   each class whose instances it may ask of, where it runs, as a call of
   the name on such an instance there asks it; a respond_to? also of each
   class whose class object it may ask of (flags CT_CMETHOD), whose arm
   reads the cells there (cplan_class_value_presence, cplan_presence_asks); a
   list of a class's own instance methods on a constant (list_cells_shape
   2), of that class, for each name it may list */
typedef struct { int id, cside; CtClassFn fn; void *arg; } PresAsk;
typedef struct { Compiler *c; int id, cls; CtClassFn fn; void *arg; } PresListAsk;
static void pres_list_one(void *ud, const char *name, int vis) {
  const PresListAsk *a = ud;
  (void)vis;
  if (ct_version_named(a->c, name)) a->fn(a->c, a->id, name, a->cls, 0, a->arg);
}
static void pres_ask_one(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  const PresAsk *a = arg;
  (void)id;
  if (k < 0 || (flags & CT_TOP)) return;
  if (!(flags & CT_CMETHOD)) a->fn(c, a->id, nm, k, 0, a->arg);
  else if (a->cside && cplan_presence_asks(c, a->id, k, nm)) a->fn(c, a->id, nm, k, CT_CMETHOD, a->arg);
}
/* Does list id, of a class's own instance methods on a constant (list_cells_shape 2), read the version cells of
   class cls's names: the class its receiver names, one of the program's or a builtin it reopens, in a program
   whose respond_to? asks only the methods? The keep (presence_asks) and the read (cplan_list_cells) ask this
   alike */
static int list_own_reads_cells(Compiler *c, int id, int cls) {
  return id >= 0 && cls >= 0 && list_cells_shape(c->nt, id) == 2 && cls == class_recv_static_ci(c, nt_ref(c->nt, id, "receiver")) &&
         !respond_to_missing_defined(c);
}
/* How presence reader id (a respond_to? or a method_defined? kin of a literal name, or a list of a class's own
   instance methods) reads the cells, as cplan_presence_list asks them: a respond_to?'s are the chain's versions'
   (CT_REACH), of the public visibility, or every one where its second argument is `true` (*mask 1 or 7); a
   method_defined? kin's are a list's (*list, cplan_list_cells), of the visibilities it counts
   (method_defined_vis_mask; a list's, those it shows), its own class's alone where its inherit argument is `false`
   (CT_OWN), as a list's are */
static int list_core_vis_mask(const char *n) {
  int vm = method_list_vis_mask(n), m = 0;
  for (int v = SP_VIS_PUBLIC; v <= SP_VIS_PROTECTED; v++) if ((vm >> v) & 1) m |= core_bit_of_vis(v);
  return m;
}
/* The visibilities a respond_to? id counts (core bits): the public one, or
   every one where its second argument is `true`; 0 where that argument is
   one only the run knows */
static int respond_to_vis_mask(const NodeTable *nt, int id) {
  int ac = 0;
  const int *av = call_args(nt, id, &ac);
  NodeKind ak = ac >= 2 ? nt_kind(nt, av[1]) : NK_FalseNode;
  return ak == NK_TrueNode ? 7 : ak == NK_FalseNode || ak == NK_NilNode ? 1 : 0;
}
static unsigned presence_reader_side(const NodeTable *nt, int id, int *mask, int *list) {
  int ac = 0, rm = respond_to_vis_mask(nt, id), flag = rm == 7;
  const char *rn = nt_str(nt, id, "name");
  call_args(nt, id, &ac);
  *list = !is_respond_to_name(rn);
  *mask = !*list ? (flag ? 7 : 1) : is_instance_method_list(rn) ? list_core_vis_mask(rn) : method_defined_vis_mask(rn);
  return !*list || (ac == 1 && !is_instance_method_list(rn)) || flag ? CT_REACH : CT_OWN;
}
static void presence_asks(Compiler *c, int id, CtClassFn fn, void *arg) {
  const NodeTable *nt = c->nt;
  const char *rn = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  if (list_cells_shape(nt, id) == 2) {
    PresListAsk la = { c, id, class_recv_static_ci(c, nt_ref(nt, id, "receiver")), fn, arg };
    if (list_own_reads_cells(c, id, la.cls)) cplan_list_candidates(c, la.cls, CT_OWN, pres_list_one, &la);
    return;
  }
  const char *qm = is_method_presence_name_query(rn) ? method_sym_arg(c, id) : NULL;
  if (!qm || !ct_version_named(c, qm) || respond_to_missing_defined(c)) return;
  int r = nt_ref(nt, id, "receiver"), k;
  PresAsk a = { id, is_respond_to_name(rn), fn, arg };
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_SelfNode;
  if (is_respond_to_name(rn)) ct_version_classes(c, id, qm, pres_ask_one, &a);
  else if (!is_method_defined_query(rn) || r < 0) return;
  else if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    if ((k = class_recv_static_ci(c, r)) >= 0) fn(c, id, qm, k, 0, arg);
  }
  else if (rk == NK_CallNode && is_class_query(nt_str(nt, r, "name")) && nt_ref(nt, r, "receiver") >= 0)
    ct_version_classes(c, r, qm, pres_ask_one, &a);
}
/* ...each such reader's of the program (written: in a scope the C writes, ver_scope_written) */
static int ver_scope_written(const Scope *s);
static int presence_reader_name(const char *n) { return is_method_presence_name_query(n) || is_instance_method_list(n); }
static void presence_readers(Compiler *c, int written, CtClassFn fn, void *arg) {
  int *ids = NULL, n = an_calls_named_if(c, presence_reader_name, c->nt->count, &ids);
  for (int i = 0; i < n; i++)
    if (!written || ver_scope_written(comp_scope_of(c, ids[i]))) presence_asks(c, ids[i], fn, arg);
  free(ids);
}
/* Does respond_to? call id ask the version cells of cls's class method
   name: one the record cannot order there, in a program whose
   respond_to? asks only the methods (respond_to_missing_defined), where
   no reader answering from the table reports the class side's methods
   (elab_method_reflected: a capture it guards binds the table's method) */
static int cplan_presence_asks(Compiler *c, int id, int cls, const char *name) {
  return id >= 0 && cls >= 0 && ct_version_named(c, name) && !cplan_cmethod_later(c, id, cls, name) &&
         !respond_to_missing_defined(c) && !elab_method_reflected(c, name, 1);
}
static int ver_entry_vis(const Compiler *c, int scope) {
  for (int i = 0; i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == scope) return c->ver_entries[i].vis;
  return 0;
}
/* The cells over the versions of cls's `name` on side sg (own: cls's own
   history alone), as cplan_cmethod_presence gives them: finds, what a call
   runs where none is live (VER_FINDS_*); vis_kept, whether a version's
   visibility only the run knows has its cell written for the readers */
static int presence_cells_of(Compiler *c, int cls, int sg, const char *name, int mask, int own, int finds, int vis_kept, CplanCells *out) {
  ElabVersion vs[VER_MAX_ARMS];
  int nv = own ? elab_history_versions(c, cls, sg, name, vs, VER_MAX_ARMS) : elab_chain_versions(c, cls, sg, name, vs, VER_MAX_ARMS);
  out->n = 0;
  out->mask = mask;
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE) return 0;
    int gone = vs[i].v->kind == LR_UNDEF;   /* (a live undef answers 0, a live def 1) */
    /* (the visibility its arm takes, a later `private :s` among it; one only the run knows, -1, is read from its
       visibility cell, which the entry writes for a respond_to? that reads it, ver_presence_mark) */
    int vis = gone || mask == 7 ? SP_VIS_PUBLIC : elab_version_vis(c, &vs[i]);
    if (vis < 0 && !vis_kept) return 0;
    ver_cell_name(vs[i].cell, out->cell[out->n]);
    out->vis[out->n][0] = 0;
    if (vis < 0) ver_vis_cell_name(vs[i].cell, out->vis[out->n]);
    out->ans[out->n] = (unsigned char)(!gone && vis >= 0 && (core_bit_of_vis(vis) & mask));
    out->val[out->n++] = vs[i].n;
  }
  out->els = (finds == VER_FINDS_PUBLIC ? core_bit_of_vis(SP_VIS_PUBLIC) : finds == VER_FINDS_PROTECTED ? core_bit_of_vis(SP_VIS_PROTECTED) :
              finds == VER_FINDS_PRIVATE ? core_bit_of_vis(SP_VIS_PRIVATE) : 0) & mask ? 1 : 0;
  return 1;
}
/* ...those of entry e: with none live, what the call runs is the entry's
   else arm, the visibility of the method it finds; a visibility only the
   run knows is read from its cell, which the entry writes for a reader
   (ver_presence_mark); a guard entry's (ct_guard), the prepends' cells its arms test, as cplan_guarded_presence
   gives them, each answer counted against mask */
static int presence_cells(Compiler *c, int e, int cls, int sg, const char *name, int mask, int own, CplanCells *out) {
  for (int i = 0; e >= 0 && !sg && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e && c->ver_entries[i].guard == VER_ENTRY_GUARD) {
      if (!cplan_guarded_presence(c, cls, name, own, out)) return 0;
      for (int k = 0; k < out->n; k++)   /* (a visibility cell counts for a mask short of all three: presence_cells_of) */
        if (out->vis[k][0] && mask == 7) out->vis[k][0] = 0, out->ans[k] = 1;
        else out->ans[k] = (out->ans[k] & mask) != 0;
      out->els = (out->els & mask) != 0, out->mask = mask;
      return 1;
    }
  if (e < 0 || c->scopes[e].entry != ENTRY_DISPATCH) return 0;
  return presence_cells_of(c, cls, sg, name, mask, own, cplan_entry_finds(c, e), ver_entry_vis(c, e), out);
}
static int ver_first_stmt(const NodeTable *nt, int stmts);

/* The selector made by conditional def lowering uses positive arm
   numbers. Before any definition runs the slot has no arm selected. */
const char *cplan_conditional_entry_selector(Compiler *c, int mi) {
  if (mi < 0) return NULL;
  const NodeTable *nt = c->nt;
  int def = c->scopes[mi].def_node;
  if (def < 0 || !nt_int(nt, def, "cond_lone_disp", 0)) return NULL;
  int first = ver_first_stmt(nt, nt_ref(nt, def, "body"));
  int predicate = nt_ref(nt, first, "predicate");
  int selector = nt_ref(nt, predicate, "receiver");
  return nt_kind(nt, selector) == NK_GlobalVariableReadNode ? nt_str(nt, selector, "name") : NULL;
}

/* See call_plan.h. */
int cplan_implied_entry_cells(Compiler *c, int mi, int undefined, CplanCells *out) {
  if (mi < 0) return 0;
  memset(out, 0, sizeof *out);
  const Scope *scope = &c->scopes[mi];
  if (scope->entry != ENTRY_DISPATCH && scope->entry != ENTRY_NAMED) return 0;
  const struct VerEntry *entry = NULL;
  for (int i = 0; i < c->nver_entries && !entry; i++)
    if (c->ver_entries[i].scope == mi) entry = &c->ver_entries[i];
  if (!entry || (entry->guard != VER_ENTRY_CHAIN && entry->guard != VER_ENTRY_HISTORY)) return 0;
  ElabVersion versions[VER_MAX_ARMS];
  int n = entry->guard == VER_ENTRY_HISTORY
        ? elab_history_versions(c, entry->cls, entry->sg, entry->name, versions, VER_MAX_ARMS)
        : elab_chain_versions(c, entry->from ? entry->from - 1 : entry->cls,
                              entry->from ? 0 : entry->sg, entry->name, versions, VER_MAX_ARMS);
  if (n <= 0 || n > VER_MAX_ARMS) return 0;
  int any = 0;
  for (int i = 0; i < n; i++) {
    if (versions[i].v->kind == LR_REMOVE) continue;
    int k = out->n++;
    ver_cell_name(versions[i].cell, out->cell[k]);
    out->val[k] = versions[i].n;
    out->ans[k] = (versions[i].v->kind == LR_UNDEF) == undefined;
    any |= out->ans[k];
  }
  return any;
}

/* See call_plan.h. */
int cplan_cells_fixed(const CplanCells *cs) {
  for (int k = 0; k < cs->n; k++)
    if (cs->vis[k][0] || cs->ans[k] != cs->els) return 0;
  return 1;
}
/* The call guard of a module's own history whose visibility cells a presence reader reads (VerGuard.vis), -1 for none */
static int ver_history_vis_guard(const Compiler *c, int cls, const char *name) {
  for (int k = 0; k < c->nver_guards; k++)
    if (c->ver_guards[k].cls == cls && c->ver_guards[k].vis && sp_streq(c->ver_guards[k].name, name)) return k;
  return -1;
}
/* ...the cells a presence reader id reads of cls's name on side sg, where call_target answered it e (ml): an
   entry's; else, for a method live where the reader runs whose visibility a body's word sets where it runs
   (elab_method_vis_at answers none), the words' versions' cells, which the words write, and the method's own where
   none has run */
static int pres_module_cells(Compiler *c, int id, int e, const MethodLookup *ml, int cls, const char *name);
static int presence_target_cells(Compiler *c, int id, int e, const MethodLookup *ml, int cls, int sg, const char *name, int mask, int own, CplanCells *out) {
  if (ml->kind == ML_ENTRY) return presence_cells(c, e, cls, sg, name, mask, own, out);
  /* (a module's own where the table answers, its versions more than one: their cells all the same, pres_module_cells) */
  if ((ml->kind != ML_METHOD && !(ml->kind == ML_TABLE && !sg && pres_module_cells(c, id, e, ml, cls, name))) || e < 0 ||
      !elab_method_body_word(c, name, sg) || elab_method_vis_at(c, cls, name, sg, elab_point_of(c, id)) >= 0)
    return 0;
  int v = guard_visibility(c, e);
  /* (a module's own instance side: its chain's cells where a guard writes them, VerGuard.vis) */
  int kept = sg || !comp_class_is_module(c, &c->classes[cls]) || ver_history_vis_guard(c, cls, name) >= 0;
  return presence_cells_of(c, cls, sg, name, mask, own,
                           v == SP_VIS_PUBLIC ? VER_FINDS_PUBLIC : v == SP_VIS_PROTECTED ? VER_FINDS_PROTECTED : VER_FINDS_PRIVATE, kept, out);
}
/* Does a presence reader id of module cls's own instance method `name` (call_target answered it e, ml) read the
   cells of the histories on cls's chain? No call runs on a module's instance side, so no entry stands over its
   versions (its method is the copies' source); where a body's word sets a version's visibility only the run tells
   it (elab_version_vis -1), which the version's visibility cell holds: presence_target_cells reads them */
static int pres_module_cells(Compiler *c, int id, int e, const MethodLookup *ml, int cls, const char *name) {
  if ((ml->kind != ML_METHOD && ml->kind != ML_TABLE) || e < 0 || cls < 0 || cls >= c->nclasses || !comp_class_is_module(c, &c->classes[cls]) ||
      !elab_method_body_word(c, name, 0) || respond_to_missing_defined(c) || elab_method_vis_at(c, cls, name, 0, elab_point_of(c, id)) >= 0)
    return 0;
  ElabVersion vs[VER_MAX_ARMS];
  int nv = elab_chain_versions(c, cls, 0, name, vs, VER_MAX_ARMS), runs = 0;
  for (int i = 0; i < nv && !runs; i++) runs = vs[i].v->kind == LR_DEF && elab_version_vis(c, &vs[i]) < 0;
  return runs;
}
/* See call_plan.h. */
int cplan_cmethod_presence(Compiler *c, int id, int cls, const char *name, int mask, int own, CplanCells *out) {
  MethodLookup ml = { .def_class = -1 };
  if (!cplan_presence_asks(c, id, cls, name)) return 0;
  int e = call_target(c, id, cls, name, CT_CMETHOD | CT_CAPTURE, &ml);
  return presence_target_cells(c, id, e, &ml, cls, 1, name, mask, own, out);
}
/* See call_plan.h. */
int cplan_presence_cells(Compiler *c, int id, int cls, unsigned side, const char *name, int mask, CplanCells *out) {
  MethodLookup ml = { .def_class = -1 };
  if (id < 0 || cls < 0 || (side & (CT_CMETHOD | CT_TOP)) || respond_to_missing_defined(c)) return 0;
  int e = call_target(c, id, cls, name, side | CT_REACH, &ml);
  return presence_target_cells(c, id, e, &ml, cls, 0, name, mask, (side & CT_OWN) != 0, out);
}
/* See call_plan.h. */
int cplan_list_cells(Compiler *c, int id, int cls, unsigned side, const char *name, int mask, CplanCells *out) {
  int own = (side & CT_OWN) != 0, cm = (side & CT_CMETHOD) != 0;
  int list = cm || (id >= 0 && is_instance_method_list(nt_str(c->nt, id, "name")));
  /* (an instance-method list reads them in the shape its asks were made for, presence_asks) */
  if (!cm && list && !list_own_reads_cells(c, id, cls)) return 0;
  /* (some version has cells, each answering for the visibilities mask counts: a run-time one from its cell) */
  if (!(cm ? cplan_cmethod_presence(c, id, cls, name, mask, own, out) : cplan_presence_cells(c, id, cls, side, name, mask, out)) ||
      out->n <= 0) return 0;
  /* (where no version has run a list leaves the name out, whatever a call there finds: its else; the keep,
     ver_obj_presence_keep, and the list's read ask the same cells) */
  if (list) out->els = 0;
  return 1;
}
/* What class cls holds of qm on its instance side where call id runs, as the call plan answers it (side: CT_REACH to
   count the generated iterators of a Struct, CT_OWN for the class's own entries): pr is the table's entry, visibility
   and undef (an undef of a reopened builtin's own method counts where it has run), and the names the class carries
   with no entry; PR_ABSENT says the record proves no method of the name live where id runs, though the table holds
   one. A super's id (its method runs wherever the override is called from: no point is its own) and no id ask the
   final tables alone (PR_TABLE). */
PresenceKind presence_at(Compiler *c, int id, int cls, unsigned side, const char *qm, CplanPresence *pr) {
  NodeKind k = id >= 0 ? nt_kind(c->nt, id) : NK_SuperNode;
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    cplan_table_presence(c, cls, side, qm, pr);
    return PR_TABLE;
  }
  return cplan_presence(c, id, cls, side, qm, pr);
}

/* The visibility of method qm that a program's reopening of Object, Kernel
   or BasicObject gives an instance of class cid (-1: a value of no program
   class) where call id runs (-1 for none), in the order CRuby looks them
   up, or -1 when none defines it there. A blank slate (BasicObject's
   subclass) reaches only BasicObject's. The class table keeps these roots
   apart from cid's own chain; each root's entry, visibility and the record's
   answer at the call come from the call plan (presence_at). */
int root_reopen_method_vis(Compiler *c, int id, int cid, const char *qm) {
  static const int roots[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  int blank = cid >= 0 && class_is_blank_slate(c, cid);
  for (int u = 0; roots[u] >= 0; u++) {
    if (blank && roots[u] != CORE_BASIC_OBJECT) continue;
    int k = comp_core_class(c, roots[u]);
    if (k < 0 || k == cid) continue;
    /* an undef here hides a lower root's method of that name; a Kernel reopening's defs land in Object's table */
    CplanPresence pr;
    PresenceKind pk = presence_at(c, id, k, CT_REACH, qm, &pr);
    if (pr.undef) return -1;
    if (pk != PR_ABSENT && pr.table) return pr.vis;
  }
  return -1;
}

/* What an instance of user class cid answers to Object#respond_to?(qm): 2 =
   true, 1 = only with include_all (a private or protected match), 0 = false.
   The method-table reading of respond_to_static_answer's object branch,
   without its deferral to the class's own respond_to? -- this is what that
   override's `super` asks. The table's entry, visibility and undef, and the
   names the class carries with no entry, come from the call plan
   (cplan_table_presence). */
/* Does a program class below a builtin exception inherit one of the class-gated
   accessors (KeyError#key, NameError#name, NoMethodError#args, ...) from the
   builtin it descends from? The instance is the builtin's own object, so the
   runtime gate (sp_exc_has_acc) admits it; the program's method table has no
   entry for it, and a name no ancestor of the builtin defines stays false. */
static int exc_subclass_inherits_gated_acc(Compiler *c, int cid, const char *qm) {
  if (!class_is_exc_subclass(c, cid)) return 0;
  const char *const *owners = gated_exception_accessor_owners(qm);
  const char *par = exc_builtin_parent(c, cid);
  for (int o = 0; owners && owners[o]; o++)
    for (int i = 0; builtin_ancestor_at(par, i); i++)
      if (sp_streq(builtin_ancestor_at(par, i), owners[o])) return 1;
  return 0;
}
int cplan_obj_responds(Compiler *c, int cid, const char *qm, int id) {
  if (name_is_synth_method(c, qm) || comp_method_is_private_helper(c, cid, qm)) return 0;
  if (is_initialize_copy_name(qm)) return 1;
  /* BasicObject#initialize is private too, unless the class made it public:
     a `public :initialize` counts whether or not the chain defines it */
  if (is_initialize_name(qm)) {
    int at = -1, vis = cplan_method_vis(c, -1, cid, qm, 0, &at);
    if (at >= 0) return vis == SP_VIS_PUBLIC ? 2 : 1;
    return call_target(c, -1, cid, qm, CT_METHODS, NULL) >= 0 && vis == SP_VIS_PUBLIC ? 2 : 1;
  }
  for (int u = 0; respond_universal[u]; u++) if (sp_streq(qm, respond_universal[u])) return 2;
  CplanPresence pr;
  /* (where the record proves none of the program's live at the call, the builtin tiers below answer) */
  PresenceKind pk = presence_at(c, id, cid, CT_REACH, qm, &pr);
  if (pr.undef) return 0;
  /* a singleton alias names a class method, not one of the instance's */
  if (pr.sg_only) return 0;
  /* an alias that captured an Object / Kernel method answers as that one */
  { int bi = 0; const char *bn = comp_resolve_alias_ex(c, cid, qm, NULL, &bi);
    if (bi == 3 && !sp_streq(bn, qm)) return 2; }
  int found = pk != PR_ABSENT && pr.table;
  /* an Array (Hash) subclass instance answers Array's (Hash's) names (#7449) */
  if (!found && comp_ty_ary_root(c, ty_object(cid)) >= 0 && comp_arysub_builtin_name(c, -1, cid, qm)) return 2;
  /* Both a native class and its subclasses answer the public bindings.
     A source visibility declaration applies just as it does to a def. */
  int nk = comp_native_binding_class(c, cid, qm);
  if (!found && nk >= 0 && comp_native_method_find(c, nk, qm, 0, 0) >= 0)
    return pr.vis == SP_VIS_PUBLIC ? 2 : 1;
  /* the names the class answers without a method-table entry: an
     Enumerable includer's, a Comparable's, a Struct's or a Data's
     core names (#2663) */
  if (!found && pr.implied) return 2;
  /* a class-gated accessor of the builtin exception the class descends from (K-055) */
  /* (a `private :key` the class declares on it is the class's own visibility word) */
  if (!found && exc_subclass_inherits_gated_acc(c, cid, qm)) return pr.vis == SP_VIS_PUBLIC ? 2 : 1;
  /* a method of a program's Object, Kernel or BasicObject reopening that
     the chain does not reach (the class table keeps them apart): the
     root's, by its visibility there */
  if (!found) {
    int rv = root_reopen_method_vis(c, id, cid, qm);
    /* ...or by the top level's def, Object's method from where the call runs (the chain of a class with no
       explicit superclass ends before it); not for a blank slate, which has no Object */
    if (rv < 0 && id >= 0 && !class_is_blank_slate(c, cid)) {
      rv = cplan_top_def_vis(c, id, qm);
      /* (a visibility word the class's chain holds for the name, `private :tk`, is the class's answer) */
      int at = -1, dv = cplan_method_vis(c, -1, cid, qm, 0, &at);
      if (rv >= 0 && at >= 0 && at != comp_core_class(c, CORE_OBJECT)) rv = dv;
    }
    if (rv >= 0) return rv == SP_VIS_PUBLIC ? 2 : 1;
  }
  /* Object's private methods (exit, puts, raise) answer to include_all;
     one an Object reopening redefines is left as the chain reads it */
  if (!found) return object_private_method_name(qm) && !class_is_blank_slate(c, cid) &&
                     !root_reopen_defines(c, id, qm) ? 1 : 0;
  return pr.vis == SP_VIS_PUBLIC ? 2 : 1;
}

/* Whether a slot typed as class cid may hold an instance of class k: cid's
   own subclasses, and for a singleton class the classes above it its object
   carries until its `def obj.m` runs, or after #dup drops it (the links up
   to the class Ruby sees). */
int cplan_slot_may_hold(Compiler *c, int k, int cid) {
  if (is_descendant(c, k, cid)) return 1;
  for (int p = cid; p >= 0 && c->classes[p].is_singleton_of; )
    if ((p = c->classes[p].parent) == k) return 1;
  return 0;
}

/* Does a reopening of Object, Kernel or BasicObject define or undefine qm,
   where call id runs (-1: anywhere in the program)? Then the name is left
   to the class chain, as it was read before Object's private methods were
   answered. A def the record proves has not run there is none yet. */
int root_reopen_holds(Compiler *c, const char *qm) {
  static const int roots[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  for (int i = 0; roots[i] >= 0; i++) {
    int k = comp_core_class(c, roots[i]);
    CplanPresence pr;
    if (k < 0) continue;
    cplan_table_presence(c, k, CT_REACH, qm, &pr);
    if (pr.table || pr.undef) return 1;
  }
  return 0;
}
int root_reopen_defines(Compiler *c, int id, const char *qm) {
  static const int roots[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  /* (the tables hold none: the record is asked only for a name they hold) */
  if (!root_reopen_holds(c, qm)) return 0;
  for (int i = 0; roots[i] >= 0; i++) {
    int k = comp_core_class(c, roots[i]);
    if (k < 0) continue;
    CplanPresence pr;
    cplan_table_presence(c, k, CT_REACH, qm, &pr);
    if (!pr.table && !pr.undef) continue;
    PresenceKind pk = presence_at(c, id, k, CT_REACH, qm, &pr);
    if ((pk != PR_ABSENT && pr.table) || pr.undef) return 1;
  }
  return 0;
}

/* Do the cells hold values the run-time test compares (an Integer global: an sp_int, or an sp_RbVal under
   --int-overflow=promote, compared as the entry's arms compare it)? */
int cplan_cells_testable(Compiler *c, const CplanCells *cs) {
  for (int i = 0; i < cs->n; i++) {
    LocalVar *g = comp_gvar(c, cs->cell[i] + 1);
    ReprKind k = g ? repr_slot_kind(c, g) : RK_NONE;
    if (k != RK_SCALAR && k != RK_BOXED) return 0;
    LocalVar *v = cs->vis[i][0] ? comp_gvar(c, cs->vis[i] + 1) : NULL;
    if (cs->vis[i][0] && (!v || (repr_slot_kind(c, v) != RK_SCALAR && repr_slot_kind(c, v) != RK_BOXED) || cplan_cell_dropped(c, v))) return 0;
  }
  return 1;
}
/* ...one arm of list l: class k's answer, from cells *cs where some (PRA_CELLS, or the answer they fix) or
   from answer a; 0, adding none, where the cells cannot be read (a boxed receiver's or a class value's class
   then answers as the reader does without cells) */
static int presence_arm(Compiler *c, CplanPresList *l, int k, const CplanCells *cs, int a) {
  if (cs && !cplan_cells_fixed(cs) && !cplan_cells_testable(c, cs)) return 0;
  if (!(l->n & (l->n - 1)) && !(l->arm = realloc(l->arm, sizeof *l->arm * (size_t)(l->n ? 2 * l->n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  CplanPresArm *m = &l->arm[l->n++];
  m->cls = k;
  m->ans = !cs ? a : cplan_cells_fixed(cs) ? cs->els : PRA_CELLS;
  if (cs) m->cells = *cs;
  return 1;
}
/* See call_plan.h. */
void cplan_presence_list_free(CplanPresList *l) {
  free(l->arm);
  l->arm = NULL, l->n = 0;
}
/* See call_plan.h. */
int cplan_presence_list(Compiler *c, int id, TyKind rt, CplanPresList *out) {
  const NodeTable *nt = c->nt;
  *out = (CplanPresList){ PRL_OBJECT, -1, 0, NULL };
  /* (`defined?(m)` of a receiverless call: what respond_to?(:m, true) of self answers) */
  int v = nt_kind(nt, id) == NK_DefinedNode ? nt_ref(nt, id, "value") : -1;
  int dm = v >= 0 && nt_kind(nt, v) == NK_CallNode && nt_ref(nt, v, "receiver") < 0;
  const char *rn = dm ? RESPOND_TO_NAME : nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int ac = 0, recv = dm ? -1 : nt_ref(nt, id, "receiver"), mask = method_defined_vis_mask(rn), cells = 0, ok = 1;
  const int *av = dm ? NULL : call_args(nt, id, &ac);
  NodeKind lk = ac == 2 ? nt_kind(nt, av[1]) : NK_TrueNode;
  /* (a literal name, a literal second argument: include_all, or method_defined?'s inherit) */
  const char *qm = dm ? nt_str(nt, v, "name") : ac == 1 || ac == 2 ? vis_arg_name(nt, av[0]) : NULL;
  if (!rn || !qm || (lk != NK_TrueNode && lk != NK_FalseNode) || (!is_respond_to_name(rn) && !mask) ||
      respond_to_missing_defined(c)) return 0;
  int list = 0, cid = -1;
  unsigned side = dm ? CT_REACH : presence_reader_side(nt, id, &mask, &list);
  if (dm) mask = 7;
  int flag = mask == 7;
  CplanCells cs;
  CplanPresence pr;
  if (list) {
    /* a class value's method_defined?: a constant's class, or any the value may be; its public versions' cells */
    NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
    int one = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? class_recv_static_ci(c, recv) : -1;
    out->form = PRL_CLASS;
    for (int k = one >= 0 ? one : 0; k < (one >= 0 ? one + 1 : c->nclasses); k++)
      if (presence_at(c, id, k, side, qm, &pr) == PR_CELLS && cplan_list_cells(c, id, k, side, qm, mask, &cs)) presence_arm(c, out, k, &cs, -1);
  }
  else if (recv >= 0 && (rt == TY_POLY || rt == TY_UNKNOWN)) {
    /* a boxed receiver: the classes whose cells answer */
    out->form = PRL_BOXED;
    for (int k = 0; k < c->nclasses; k++)
      if (presence_at(c, id, k, CT_REACH, qm, &pr) == PR_CELLS && cplan_presence_cells(c, id, k, CT_REACH, qm, mask, &cs))
        presence_arm(c, out, k, &cs, -1);
  }
  else {
    /* an object, or self in an instance method: each class the slot may hold (a defined?'s, as its call asks) */
    int at = dm ? v : id;
    const Scope *s = recv < 0 ? comp_scope_of(c, at) : NULL;
    if (recv >= 0 && ty_is_object(rt) && !comp_ty_value_obj(c, rt)) cid = ty_object_class(rt), out->nil = nil_answers_name(qm);
    else if (s && s->class_id >= 0 && !s->is_cmethod && !c->classes[s->class_id].is_value_type &&
             !comp_class_is_module(c, &c->classes[s->class_id])) cid = s->class_id;
    for (int k = 0; ok && cid >= 0 && k < c->nclasses; k++) {
      if (!cplan_slot_may_hold(c, k, cid) || (k != cid && !c->classes[k].instantiated)) continue;
      /* (a class's own respond_to? answers for it; not for a defined?) */
      if (!dm && call_target(c, at, k, RESPOND_TO_NAME, CT_METHODS, NULL) >= 0) ok = 0;
      else if (cplan_presence_cells(c, at, k, CT_REACH, qm, mask, &cs)) ok = presence_arm(c, out, k, &cs, -1), cells = 1;
      else {
        int a = cplan_obj_responds(c, k, qm, at);
        ok = presence_arm(c, out, k, NULL, a == 2 || (a == 1 && flag) ? PRA_YES : PRA_NO);
      }
    }
  }
  for (int i = 0; i < out->n; i++) cells |= out->arm[i].ans == PRA_CELLS || out->form != PRL_OBJECT;
  if (!ok || !cells || !out->n) { cplan_presence_list_free(out); return 0; }
  return 1;
}
/* See call_plan.h. */
int cplan_class_builtin_responds(Compiler *c, int ci, const char *qm) {
  return class_object_builtin_responds(qm, comp_class_is_module(c, &c->classes[ci]), class_is_exc_subclass(c, ci), class_struct_kind(c, ci));
}
/* See call_plan.h. */
int cplan_class_responds(Compiler *c, int ci, const char *qm, int id) {
  if (!cplan_cmethod_later(c, id, ci, qm)) {
    CplanPresence pr;
    cplan_table_presence(c, ci, CT_CMETHOD, qm, &pr);
    if (pr.table) return 1;
  }
  return cplan_class_builtin_responds(c, ci, qm);
}
/* Does class k's class method qm, which its table holds (pr), count for a
   reader of the visibilities in mask at call id: by the visibility the
   record has where id runs; else the table's where the name's visibility
   does not vary over the load (elab_method_vis_varies), or the one every
   version of it on k's class-side chain takes (elab_version_vis); where
   none tells (a version's that only the run knows), as the table holds it */
static int class_value_vis_counts(Compiler *c, int id, int k, const char *qm, const CplanPresence *pr, int mask) {
  ElabVersion vs[VER_MAX_ARMS];
  LoadPoint p = elab_point_of(c, id);
  int vis = p.region >= 0 ? elab_method_vis_at(c, k, qm, 1, p) : -1;
  if (vis < 0 && !elab_method_vis_varies(c, qm)) vis = pr->vis;
  for (int i = 0, nv = vis < 0 ? elab_chain_versions(c, k, 1, qm, vs, VER_MAX_ARMS) : 0, v; i < nv; i++) {
    v = vs[i].v->kind == LR_UNDEF || vs[i].v->kind == LR_REMOVE ? -1 : elab_version_vis(c, &vs[i]);
    if (v < 0 || (i && v != vis)) { vis = -1; break; }
    vis = v;
  }
  return vis < 0 || (core_bit_of_vis(vis) & mask);
}
/* See call_plan.h. */
int cplan_class_value_presence(Compiler *c, int id, const char *qm, CplanPresList *out) {
  int uni = class_object_universal_method(qm), any = class_reopen_cmethod(c, -1, -1, qm) >= 0;
  *out = (CplanPresList){ PRL_CLASS_VALUE, -1, 0, NULL, uni || any };
  CplanCells cs;
  CplanPresence pr;
  int mask = respond_to_vis_mask(c->nt, id);
  for (int k = 0; k < c->nclasses; k++) {
    PresenceKind pk;
    /* (its version cells, where they answer; their els the builtin's answer where none is live) */
    if (cplan_cmethod_presence(c, id, k, qm, mask ? mask : 1, 0, &cs) && cplan_cells_testable(c, &cs)) {
      cs.els = cs.els || cplan_class_builtin_responds(c, k, qm);
      presence_arm(c, out, k, &cs, -1);
    }
    /* (else the table's method, of the visibility it has where the reader runs: class_value_vis_counts) */
    else if (cplan_class_responds(c, k, qm, id) &&
             ((pk = cplan_presence(c, id, k, CT_CMETHOD, qm, &pr)) != PR_ABSENT || cplan_class_builtin_responds(c, k, qm)))
      presence_arm(c, out, k, NULL, !mask || pk == PR_ABSENT || !pr.table || class_value_vis_counts(c, id, k, qm, &pr, mask) ? PRA_YES : PRA_NO);
    else continue;
    any |= !uni;   /* (a class's no is an answer too: its method is closed to the reader there) */
  }
  if (!any) cplan_presence_list_free(out);
  return any;
}
/* See call_plan.h. */
int cplan_entry_finds(const Compiler *c, int e) {
  for (int i = 0; e >= 0 && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e) return c->ver_entries[i].finds;
  return -1;
}
/* The class method t of the program's that a call id of the builtin's own name (`new`, `allocate`) on class ci
   finds, where the record proves the call runs before t is made: the builtin runs, -1. That holds for one a
   superclass defines as well (the record's chain answers it where the call runs). Not one no def makes
   (define_singleton_method), which the record does not see. */
static int ct_cmethod_made_at(Compiler *c, int id, int ci, const char *nm, int t) {
  int dn = c->scopes[t].def_node;
  if (nt_kind(c->nt, dn) != NK_DefNode || elab_point_of(c, dn).region < 0) return t;
  int framed = elab_plan_enter(c, id), at = elab_call_method(c, id, ci, nm, 1, 1);
  if (framed) elab_plan_leave(c);
  return at == ELAB_NO ? -1 : t;
}
/* The program's `new` a call reaches on class ci (cplan_user_new): the one
   call_target finds, and on a record class also in *unplaced whether the
   record can tell neither that version nor the builtin constructor there:
   a def call_target finds that is proven made neither at the call nor over
   its window, nor proven absent there. Such a call takes no guess; it is
   refused (CRF_RECORD_NEW). */
static int user_new_at(Compiler *c, int id, int ci, int *unplaced) {
  /* (`S[x]`, which desugar_struct_index_ctor gives new's name, is the
     class's own `[]`: a program's `new` is not what it calls) */
  const char *nm = nt_int(c->nt, id, "index_ctor", 0) ? "[]" : "new";
  /* (index_ctor marks a record's only: any other class's name is `new`) */
  if (ct_pin_of(c, id) != -1) return call_target(c, id, ci, nm, CT_CMETHOD, NULL);
  int t = call_target(c, id, ci, nm, CT_CMETHOD, NULL);
  if (t < 0 || c->scopes[t].entry != ENTRY_NONE) return t;
  /* Any other class: the builtin constructor where the record proves the
     call runs before the `def self.new` it finds is made (ct_cmethod_made_at) */
  if (!c->classes[ci].is_struct) return ct_cmethod_made_at(c, id, ci, nm, t);
  int framed = elab_plan_enter(c, id), at = elab_call_method(c, id, ci, nm, 1, 1), absent = at == ELAB_NO;
  /* (a call in a method or a block: the def in force over its window, or
     none over all of it) */
  int made = at == ELAB_YES || ct_window_version(c, id, ci, nm, 1, 0) == t;
  const LrDef *ds[2];
  if (!made && !absent) absent = elab_call_span(c, id, ci, nm, 1, 0, NULL, ds, 2) == 1 && !ds[0];
  if (framed) elab_plan_leave(c);
  if (unplaced) *unplaced = !made && !absent;
  return made ? t : -1;
}
/* See call_plan.h. */
int cplan_user_new(Compiler *c, int id, int ci) {
  return id < 0 ? call_target(c, -1, ci, "new", CT_CMETHOD, NULL) : user_new_at(c, id, ci, NULL);
}
/* See call_plan.h. */
int cplan_aref_builds(Compiler *c, int id, int ci) {
  const ClassInfo *k = &c->classes[ci];
  return k->is_struct && !is_builtin_reopen(k->name) && !k->is_native_class && call_target(c, id, ci, "[]", CT_CMETHOD, NULL) < 0;
}
/* See call_plan.h. Does call `call` (-1: any) of `allocate` on class or module ci reach a
   class method `allocate` of the program's own: on ci, its singleton
   class, a parent or a module it extends? Not a call the analysis made
   for Class#new's allocation (cnew_alloc: CRuby's new does not call
   allocate), and not one whose method calls a super that reaches neither
   Class#allocate (allocate_super_reaches_class: not one with arguments,
   in an Exception class, ...) nor a parent's own allocate that answers:
   that super has no target here, so such a call keeps Class#allocate as
   before. */
static int own_allocate_answers(Compiler *c, int call, int mi);
int cplan_own_allocate(Compiler *c, int call, int ci) {
  NodeTable *nt = (NodeTable *)c->nt;
  if (ci < 0 || ci >= c->nclasses) return 0;
  if (call >= 0 && nt_int(nt, call, "cnew_alloc", 0)) return 0;
  int mi = call_target(c, call, ci, "allocate", CT_CMETHOD, NULL);
  if (mi >= 0 && call >= 0) mi = ct_cmethod_made_at(c, call, ci, "allocate", mi);
  return mi >= 0 && own_allocate_answers(c, call, mi);
}
/* Does the program's allocate mi answer for call `call` (-1: any)? Not when
   it calls a super that reaches neither Class#allocate nor a parent's own
   allocate that answers. The parent's is the version in force at the
   call (ct_met_version), found without the call's pin: a call pinned to a
   version of the child's allocate answers that scope for every class
   asked, and the question would not end. Where the record cannot tell
   which version the call meets, the table's, kept only where it is made
   at the call (ct_cmethod_made_at). Each level above it is asked the
   same way. */
static int own_allocate_answers(Compiler *c, int call, int mi) {
  NodeTable *nt = (NodeTable *)c->nt;
  int d = c->scopes[mi].def_node;
  if (d < 0) return 1;
  int pc = c->scopes[mi].class_id >= 0 ? comp_super_parent(c, c->scopes[mi].class_id, 1) : -1;
  int pm = pc >= 0 ? call_target(c, -1, pc, "allocate", CT_CMETHOD, NULL) : -1;
  if (pm >= 0 && call >= 0) {
    int none = 0, dc = -1, framed = elab_plan_enter(c, call);
    const LrDef *v = ct_met_version(c, call, pc, "allocate", 1, &none);
    int at = v ? ct_def_version(c, pc, "allocate", 1, v, &dc) : -1;
    if (framed) elab_plan_leave(c);
    pm = none ? -1 : at >= 0 ? at : ct_cmethod_made_at(c, call, pc, "allocate", pm);
  }
  int parent_own = pm >= 0 && own_allocate_answers(c, call, pm);
  int n = 0, sup = 0, *ids = nt_subtree_ids(nt, &d, 1, &n);
  for (int i = 0; i < n && !sup; i++)
    sup = (nt_kind(nt, ids[i]) == NK_SuperNode || nt_kind(nt, ids[i]) == NK_ForwardingSuperNode) &&
          !allocate_super_reaches_class(c, ids[i]) && !parent_own;
  free(ids);
  return !sup;
}
/* `<class value>.allocate` where the class value is no written-out constant
   (`x.class.allocate`, a local holding a class): a call the generated
   sp_cls_allocate answers, as it does a boxed class value, and whose value is
   boxed. A constant has its own static arm; a `self` receiver and a program
   that gives some class a class method `allocate` of its own keep the paths
   they had (that method is the user dispatch's). One rule for the analyzer's
   type of the call, the emitter, and the pass that asks for the sp_cls_*
   answers (g_gen_cls_answers). */
int cplan_class_value_allocate(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !name || !is_allocate_name(name) || nt_ref(nt, id, "arguments") >= 0 ||
      nt_ref(nt, id, "block") >= 0 || comp_ntype(c, recv) != TY_CLASS)
    return 0;
  NodeKind rk = nt_kind(nt, recv);
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode || rk == NK_SelfNode) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (call_target(c, -1, k, "allocate", CT_CMETHOD, NULL) >= 0) return 0;
  return 1;
}
/* See call_plan.h. */
int cplan_cmethod_target(Compiler *c, int id, int ci, const char *name, unsigned flags) {
  if (!is_new_name(name)) return call_target(c, id, ci, name, CT_CMETHOD | flags, NULL);
  return (flags & CT_OWN) && call_target(c, -1, ci, name, CT_CMETHOD | CT_OWN, NULL) < 0 ? -1 : cplan_user_new(c, id, ci);
}
/* Is call id a `new` (or a record class's `[]`) on a record class whose
   target the record cannot place (user_new_at)? The class is the
   receiver's constant, or self's in a class method (the copy's class for
   an inherited one). */
static int cplan_record_new_gap(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver"), ci = -1, unplaced = 0;
  if (!is_new_name(nt_str(nt, id, "name"))) return 0;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) {
    const Scope *s = comp_scope_of(c, id);
    ci = s && s->is_cmethod ? s->class_id : -1;
  }
  else if (nt_kind_is_constant(nt_kind(nt, recv))) ci = class_recv_static_ci(c, recv);
  if (ci >= 0 && c->classes[ci].is_struct) user_new_at(c, id, ci, &unplaced);
  return unplaced;
}
/* Does the literal block of a call of `nm` (on the constant `rn`, or NULL)
   run with the self around the call however it runs: a builtin's (an
   iterator, tap, loop, catch, at_exit, a thread's or a fiber's body) where
   the program has no method of the name (an instance method, a class
   method, main's own), nor a method_missing a call of it could reach. A
   block given to a method of the program's, made a Proc or a lambda, or
   handed on by a call that reaches its target by what it is given (send,
   call, new: is_opaque_reaching_call) may run later on another object
   (`o.instance_exec(&b)`), so its self is not known. */
static int ct_block_keeps_self(Compiler *c, const char *rn, const char *nm, const CplanTreeFacts *tree) {
  if (tree ? tree->takes(tree->ctx, nm) : elab_program_may_take(c, nm)) return 0;
  return is_async_code_entry(rn, nm) || !is_opaque_reaching_call(nm);
}
/* The nearest call around node whose literal block may run with another
   self than the code around it, or -1: one that runs it so
   (is_block_self_call: an instance_eval, a define_method, a class
   builder), or one whose block's self is not known (ct_block_keeps_self);
   a lambda literal is its own such node. A def or class body ends the
   climb. */
static int ct_block_self_call(Compiler *c, int node) {
  int n = 0;
  /* (the parents are read in the record's frame, which a node a pass appended since takes the facts of its origin
     in: outside it, each ask after an append rebuilds the map) */
  int framed = node >= 0 ? elab_plan_enter(c, node) : 0;
  const int *par = elab_node_parents(c, &n);
  int found = block_self_call_in(c, par, n, node, NULL);
  if (framed) elab_plan_leave(c);
  return found;
}
/* ...over the parent map par of n nodes (tree: as ct_block_self_caller) */
static int block_self_call_in(Compiler *c, const int *par, int n, int node, const CplanTreeFacts *tree) {
  const NodeTable *nt = c->nt;
  int found = -1;
  for (int p = node >= 0 && node < n ? par[node] : -1, k = 0; p >= 0 && p < n && k < n; p = par[p], k++) {
    NodeKind pk = nt_kind(nt, p);
    if (pk == NK_DefNode || pk == NK_ClassNode || pk == NK_ModuleNode || pk == NK_SingletonClassNode) break;
    if (pk == NK_LambdaNode) { found = p; break; }
    int call = pk == NK_BlockNode ? par[p] : -1;
    if (call < 0 || call >= n || nt_kind(nt, call) != NK_CallNode || nt_ref(nt, call, "block") != p) continue;
    int rv = nt_ref(nt, call, "receiver");
    const char *rn = rv >= 0 && nt_kind(nt, rv) == NK_ConstantReadNode ? nt_str(nt, rv, "name") : NULL;
    /* (a send of a literal name is the call it sends, as the lowering makes it; of a computed one, opaque) */
    const char *nm = call_sent_name(nt, call, NULL);
    if (!nm || is_block_self_call(rn, nm) || !ct_block_keeps_self(c, rn, nm, tree)) { found = call; break; }
  }
  return found;
}
static int ct_runs_on_main(Compiler *c, int id);
/* Does call id on main, of `nm`, reach the builtin: no method of the
   program's on main's chain answers it? Main's singleton holds none at
   the call's point (elab_main_singleton_at: main's own, an extended
   module's), and the plan binds none on main (call_target: a top-level
   def) nor on Object's chain ahead of the builtin's, which BasicObject's
   own table holds (an Object, Kernel or BasicObject def, a module's mixed
   into Object or Kernel or prepended to BasicObject; not one BasicObject
   includes, which sits behind it). A class's own method of the name, a
   class method or a method_missing elsewhere cannot answer a call on
   main. */
static int ct_main_builtin_call(Compiler *c, int id, const char *nm) {
  static const int roots[] = { CORE_OBJECT, CORE_BASIC_OBJECT };
  const LrDef *d = NULL;
  int s = elab_main_singleton_at(c, nm, elab_point_of(c, id), &d);
  if (s == ELAB_UNKNOWN || s == ELAB_STOP || (s == ELAB_YES && d) || call_target(c, id, -1, nm, CT_TOP, NULL) >= 0) return 0;
  int bo = comp_core_class(c, CORE_BASIC_OBJECT);
  for (int i = 0; i < 2; i++) {
    int k = comp_core_class(c, roots[i]);
    MethodLookup ml = { .def_class = -1 };
    int mi = k >= 0 ? call_target(c, id, k, nm, 0, &ml) : -1;
    if (mi >= 0 && (ml.def_class != bo || !c->scopes[mi].is_include_copy)) return 0;
  }
  return 1;
}
/* Does call id run with main as self: receiverless or on `self`, in a
   file's top-level statements (a block there too, where its self is
   known: ct_block_self_call, and the builtin instance_eval's or
   instance_exec's on main: ct_main_builtin_call) or in a method of main's
   own; not in a top-level `def`, which runs on whatever object called
   it. */
static int ct_runs_on_main(Compiler *c, int id) {
  if (nt_kind(c->nt, id) != NK_CallNode) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv >= 0 && nt_kind(c->nt, recv) != NK_SelfNode) return 0;
  int ie = ct_block_self_call(c, id);
  const char *ien = ie >= 0 && nt_kind(c->nt, ie) == NK_CallNode ? nt_str(c->nt, ie, "name") : NULL;
  if (ie >= 0) return ien && is_instance_eval_family(ien) && ct_runs_on_main(c, ie) && ct_main_builtin_call(c, ie, ien);
  const Scope *s = comp_scope_of(c, id);
  return self_is_main(c, id) && (prog_is_top_scope(c, s) || s->is_cmethod);
}
int cplan_runs_on_main(Compiler *c, int id) { return ct_runs_on_main(c, id); }
/* What main's singleton answers for `name` at call id's point
   (elab_main_singleton_at): 1, main's own method (or the record cannot
   tell, or the point is no load point); 0, none, so Object's chain
   answers (an undef too, which stops the call where ct_main_stopped says
   so); -1, another's (an extended module's, the core's). None only
   where the record places the def the table holds: one in a body the walk
   does not place (a block given in a call's arguments) makes no entry. */
static int ct_main_own(Compiler *c, int id, const char *name) {
  const LrDef *d = NULL;
  /* (the point is read in the record's frame, as ct_version reads it: outside it, every call a pass asks after
     appending a node rebuilds the map) */
  int framed = elab_plan_enter(c, id);
  LoadPoint p = elab_point_of(c, id);
  int s = p.region < 0 ? ELAB_YES : elab_main_singleton_at(c, name, p, &d);
  if (framed) elab_plan_leave(c);
  if (p.region < 0 || s == ELAB_UNKNOWN || (s == ELAB_YES && d && d->scope > 0 && c->scopes[d->scope].class_id < 0)) return 1;
  /* (an undef: where a program method_missing keeps it from stopping the
     call, ct_main_stopped, main's own method is gone all the same) */
  if (s != ELAB_NO && s != ELAB_STOP) return -1;
  /* (no record: it cannot tell) */
  return elab_history_holds_scope(c, -1, 1, name, comp_main_method(c, name)) != 1;
}
/* Does call id run on main (ct_runs_on_main) and look on main first? At a
   load point, only where main's own method of the name answers there
   (ct_main_own): not once it is removed, before it is defined, nor where
   an extended module's or the core's answers. */
static int ct_on_main(Compiler *c, int id, const char *name) {
  return ct_runs_on_main(c, id) && ct_main_own(c, id, name) == 1;
}
/* Does call id, at a load point, run on main (ct_runs_on_main) where an
   undef of `name` on main's singleton stops the lookup
   (elab_main_singleton_at's ELAB_STOP): no method of Object's chain nor a
   builtin's (the core's to_s and inspect) answers it, so it raises, as a
   call ct_undefined_at proves undefined does, where no program
   method_missing may answer it instead. The point is asked only of a
   call that runs on main: outside the plan's frame, it rebuilds the map
   whenever a pass appended nodes since, and every call a pass asks the
   hook for (cplan_mm_method) comes here. The same stop where a module main extends or Object includes or prepends
   holds the undef (the main-chain fact's EMC_STOP), in a top-level def's call too. */
static int ct_main_stopped(Compiler *c, int id, const char *name) {
  const LrDef *d = NULL;
  int stop = 0;
  if (ct_runs_on_main(c, id)) {
    int framed = elab_plan_enter(c, id);
    LoadPoint p = elab_point_of(c, id);
    stop = p.region >= 0 && elab_main_singleton_at(c, name, p, &d) == ELAB_STOP;
    if (framed) elab_plan_leave(c);
  }
  /* (or an undef in a module main extends or Object includes or prepends: the main-chain fact's stop. It holds for a
     top-level def's call too, whose self is its caller's, main or another object, both of which the fact asks) */
  if (!stop) {
    const Scope *s = comp_scope_of(c, id);
    stop = s && s->class_id < 0 && !s->is_cmethod && cplan_main_chain_step(c, id, name) == EMC_STOP;
  }
  if (!stop) return 0;
  /* (a program method_missing main reaches, its singleton's or Object's,
     answers the call instead: not told apart here, so none stops) */
  mtab_enter(c);   /* (the plan's own read: main's lookup tail, which every stop of the lookup has to meet) */
  int mm = comp_main_method(c, METHOD_MISSING_NAME);
  mtab_leave(c);
  return mm < 0 && cplan_mm_method(c, -1, -1, 0) < 0;
}
static int ct_lookup(Compiler *c, int id, int cls, const char *name, unsigned *flags, MethodLookup *ml);
/* Does call id, by its own name on main (ct_runs_on_main), meet an undef
   of `name` on main's chain that may or may not have run: at no load
   point, or at one where the record cannot say (elab_main_singleton_at's
   ELAB_UNKNOWN), of a builtin main answers (builtin_self_method_known)
   that no program method does? Where it ran, the call stops as one
   ct_main_stopped stops (where no program method_missing answers it), so
   the plan puts the call behind the undef's cell (ver_main_undef_guards).
   Not a call an entry's arm pins. */
static int ct_main_builtin_undef(Compiler *c, int id, const char *name) {
  const LrDef *d = NULL;
  unsigned f = CT_TOP;
  mtab_enter(c);   /* (the plan's own reads: main's lookup tail and the table's lookup of the name, as ct_main_stopped's) */
  int none = builtin_self_method_known(name, BSELF_MAIN) && ct_pin_of(c, id) == -1 && ver_chain_undef(c, -1, 1, name) &&
             comp_main_method(c, METHOD_MISSING_NAME) < 0 && cplan_mm_method(c, -1, -1, 0) < 0 && ct_runs_on_main(c, id) &&
             ct_lookup(c, id, -1, name, &f, NULL) < 0;
  mtab_leave(c);
  if (!none) return 0;
  LoadPoint p = elab_point_of(c, id);
  return p.region < 0 || elab_main_singleton_at(c, name, p, &d) == ELAB_UNKNOWN;
}
/* The method call id reaches when it can reach only main's own method of
   `name`: a call on main (ct_on_main) at a load point where main's method
   is defined, which CRuby's lookup finds ahead of Object's. -1 for any
   other call (a method's, a block's kept to run later), which may run
   before main's method is defined. */
int ct_main_target(Compiler *c, int id, const char *name) {
  const LrDef *d = NULL;
  if (comp_main_method(c, name) < 0 || !ct_on_main(c, id, name)) return -1;
  LoadPoint p = elab_point_of(c, id);
  if (p.region < 0 || elab_method_at(c, -1, name, 1, p, &d) != ELAB_YES) return -1;
  return call_target(c, id, -1, name, CT_TOP, NULL);
}
/* An attribute and an explicit method can both own one name. CRuby's
   attr_accessor defines an ordinary method, so a `def` at an equal-or-more-
   derived class replaces it: the more-derived definition wins, and a same-class
   tie goes to the method. Every emission site used to compose this policy from
   the chain queries itself, and the sites that composed only half of it emitted
   a call to the override typed as the attribute, or a switch with no arm for
   the method at all (#3907, #3909, #3910). Where the attribute wins, *attr is
   set, the scope is -1 and *def_class the class that declares it. */
static int ct_member(Compiler *c, int cls, const char *name, unsigned flags, int *def_class, int *attr) {
  char base[256];
  int wr = (flags & CT_WRITER) && setter_base_name(name, base, sizeof base);
  int adc = -1, mdc = -1;
  int has_attr = wr ? comp_writer_in_chain(c, cls, base, &adc) : comp_reader_in_chain(c, cls, name, &adc);
  int mi = comp_method_in_chain(c, cls, name, &mdc);
  int attr_wins = has_attr;
  if (has_attr && mi >= 0) {
    for (int k = cls; k >= 0; k = c->classes[k].parent) {
      if (k == mdc) { attr_wins = 0; break; }
      if (k == adc) { attr_wins = 1; break; }
    }
  }
  if (attr_wins) {
    *attr = 1;
    if (def_class) *def_class = adc;
    return -1;
  }
  if (mi >= 0 && def_class) *def_class = mdc;
  return mi;
}
/* the table's answer for `name` on cls, as call_target asks it; *attr (when
   asked) is set where an attribute answers it (ct_member) */
static int ct_table(Compiler *c, int cls, const char *name, unsigned flags, int *def_class, int *attr) {
  int none = 0;
  if (!attr) attr = &none;
  if (flags & CT_TOP) return comp_method_index(c, name);
  if (flags & CT_OWN) {
    int mi = flags & CT_CMETHOD ? comp_cmethod_in_class(c, cls, name) : comp_method_in_class(c, cls, name);
    if (mi >= 0 && def_class) *def_class = cls;
    return mi;
  }
  /* a Method's capture (method, instance_method, an alias) takes a private
     instance method too, as a receiverless call does: a module function */
  if (flags & CT_CMETHOD) return comp_cmethod_in_chain(c, cls, name, def_class);
  if (flags & CT_CAPTURE) return comp_self_method_in_chain(c, cls, name, def_class);
  if (flags & CT_ATTR) {
    char base[256];
    int adc = -1, wr = (flags & CT_WRITER) && setter_base_name(name, base, sizeof base);
    if (wr ? comp_writer_in_chain(c, cls, base, &adc) : comp_reader_in_chain(c, cls, name, &adc)) {
      *attr = 1;
      if (def_class) *def_class = adc;
    }
    return -1;
  }
  if (flags & CT_METHODS) return comp_method_in_chain(c, cls, name, def_class);
  return ct_member(c, cls, name, flags, def_class, attr);
}
/* The method call id runs on candidate pc of its boxed dispatch: the
   table's, or for a name with versions the one call_target binds on pc's
   class (a version, an entry), which the dispatch's arm calls */
int cplan_cand_target(Compiler *c, int id, const char *name, const PolyCand *pc, unsigned flags) {
  int mi = pc->mi >= 0 && (ct_versioned(c, pc->mi, name) || c->scopes[pc->mi].mixin_guarded) ? call_target(c, id, pc->cls, name, flags, NULL) : -1;
  return mi >= 0 ? mi : pc->mi;
}
/* See call_plan.h. */
int cplan_cand_arm(Compiler *c, int id, const char *name, int mi, int inst) {
  int n = 0;
  const PolyCand *pcs = inst ? comp_poly_candidates(c, name, &n) : NULL;
  for (int i = 0; i < n; i++)
    if (pcs[i].mi >= 0 ? pcs[i].mi == mi || cplan_cand_target(c, id, name, &pcs[i], 0) == mi
                       : call_target(c, id, pcs[i].cls, name, 0, NULL) == mi) return 1;
  pcs = comp_cmethod_candidates(c, name, &n);
  for (int i = 0; i < n; i++)
    if (pcs[i].mi >= 0 && (pcs[i].mi == mi || cplan_cand_target(c, id, name, &pcs[i], CT_CMETHOD) == mi)) return 1;
  return 0;
}
/* The method call id finds by name, call_target's start: the table's
   (ct_table), but main's own method ahead of Object's for a call on main
   (ct_on_main), as CRuby's lookup there; its versions are then main's
   chain's, which ends in Object's (*flags gains CT_CMETHOD). Where main's
   singleton answers by the core's `to_s` or `inspect`, none
   (ML_SINGLETON); where an undef there stops the lookup
   (ct_main_stopped), none (ML_NONE). */
/* The main-chain fact: what a self-call of `name` at call id meets along main's chain (elab_main_chain_at),
   at the call's point; for a call with none (a method body, a block kept to run later), over the window it may run
   in (elab_call_window): its answer at the window's start, where the answer at the load's end is the same one
   (MCH_MOVES where they differ: the answer depends on when the body runs). A call in a class's instance method, or in
   a top-level def not run on main, asks Object's chain alone. Asked inside the plan's frame (elab_plan_enter), so a
   pass that appends nodes between its asks does not rebuild the point map. ELAB_YES / NO / UNKNOWN, MCH_MOVES. */
enum { MCH_MOVES = 3 };
/* (the answer by (name, region, seq, node, side, window): a call at a point, whose node tells it from another call of the
   statement (a def or an include in an argument runs between two of them), or over one window (node -1), kept while the
   record stands, whatever nodes a pass appends) */
struct MainChainMemo { struct MainChainSlot { const char *name; int region, seq, node, key, r, step; const LrDef *d; } *slot; int cap, used; ElabStamp stamp; };
void main_chain_free(Compiler *c) {
  if (!c->main_chain) return;
  free(c->main_chain->slot);
  free(c->main_chain);
  c->main_chain = NULL;
}
static struct MainChainSlot *ct_main_chain_slot(Compiler *c, const char *name, int region, int seq, int node, int key, int *hit) {
  struct MainChainMemo *m = c->main_chain;
  ElabStamp st = elab_stamp(c);
  st.nt = 0;
  if (m && !elab_stamp_same(m->stamp, st)) { main_chain_free(c); m = NULL; }
  if (!m) {
    if (!(c->main_chain = m = calloc(1, sizeof *m))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->stamp = st;
  }
  if (2 * (m->used + 1) > m->cap) {
    struct MainChainSlot *old = m->slot;
    int ocap = m->cap;
    m->cap = ocap ? 2 * ocap : 256;
    if (!(m->slot = calloc((size_t)m->cap, sizeof *m->slot))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!old[i].name) continue;
      unsigned h = ver_mix(sp_strhash(old[i].name) + (unsigned)old[i].seq * 0x9E3779B1u + (unsigned)old[i].region * 31u + (unsigned)old[i].node * 0x85EBCA6Bu + (unsigned)old[i].key);
      int j = (int)(h & (unsigned)(m->cap - 1));
      while (m->slot[j].name) j = (j + 1) & (m->cap - 1);
      m->slot[j] = old[i];
    }
    free(old);
  }
  unsigned h = ver_mix(sp_strhash(name) + (unsigned)seq * 0x9E3779B1u + (unsigned)region * 31u + (unsigned)node * 0x85EBCA6Bu + (unsigned)key);
  int j = (int)(h & (unsigned)(m->cap - 1));
  for (; m->slot[j].name; j = (j + 1) & (m->cap - 1))
    if (m->slot[j].region == region && m->slot[j].seq == seq && m->slot[j].node == node && m->slot[j].key == key && sp_streq(m->slot[j].name, name)) { *hit = 1; return &m->slot[j]; }
  *hit = 0;
  m->slot[j].name = name;   /* (a name the node table holds: it outlives the memo) */
  m->slot[j].region = region, m->slot[j].seq = seq, m->slot[j].node = node, m->slot[j].key = key;
  m->used++;
  return &m->slot[j];
}
/* The main-chain fact over a method body's window w, from one side (obj: an Object instance's, else main's): its
   answer where the body may first run, or MCH_MOVES where the end of the program answers otherwise */
static int ct_main_chain_win(Compiler *c, const char *name, ElabWindow w, int obj, int *step, const LrDef **d) {
  int hit = 0, r;
  struct MainChainSlot *sl = ct_main_chain_slot(c, name, w.from.region, w.from.seq, -1, obj + 2, &hit);
  if (hit) { *step = sl->step, *d = sl->d; return sl->r; }
  const LrDef *d2 = NULL;
  int st2 = EMC_NONE;
  r = elab_main_chain_at(c, name, w.from, obj, d, step);
  int r2 = elab_main_chain_at(c, name, (LoadPoint){ -1, -1, -1 }, obj, &d2, &st2);
  if (r == ELAB_UNKNOWN || r2 == ELAB_UNKNOWN) r = ELAB_UNKNOWN;
  else if (r != r2 || *step != st2 || *d != d2) r = MCH_MOVES;
  sl->r = r, sl->step = *step, sl->d = *d;
  return r;
}
static int ct_main_chain(Compiler *c, int id, const char *name, int defs, int *step, const LrDef **d) {
  *step = EMC_NONE;
  *d = NULL;
  if (id < 0 || !name || nt_kind(c->nt, id) != NK_CallNode) return ELAB_UNKNOWN;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv >= 0 && nt_kind(c->nt, recv) != NK_SelfNode) return ELAB_UNKNOWN;
  /* (no mixin on main or Object: the defs' versions answer, as they did; nothing to walk. Asked before anything that
     can refresh the parent map: a program with no mixin pays this count per ask and no more) */
  int nm0 = 0, nm1 = 0, oc = comp_core_class(c, CORE_OBJECT);
  elab_class_mixins(c, -1, &nm0);
  if (oc >= 0) elab_class_mixins(c, oc, &nm1);
  if (!nm0 && !nm1 && !defs) return ELAB_UNKNOWN;
  const Scope *s = comp_scope_of(c, id);
  if (!s || s->is_cmethod || ct_block_self_call(c, id) >= 0) return ELAB_UNKNOWN;
  if (s->class_id >= 0 && class_is_blank_slate(c, s->class_id)) return ELAB_UNKNOWN;
  /* (a `class << self` body's self is main's singleton class, whose chain is Class's: not asked here) */
  int pb = id < c->node_pbase_n ? c->node_pbase[id] : -1;
  if (pb >= 0 && nt_kind(c->nt, pb) == NK_SingletonClassNode) return ELAB_UNKNOWN;
  int framed = elab_plan_enter(c, id), r;
  LoadPoint p = elab_point_of(c, id);
  /* (a top-level def's self is its caller's, main or another object: both sides ask, and where they differ the
     answer depends on self, which the fact leaves to the table) */
  int both = s->class_id < 0 && !s->is_cmethod && !prog_is_top_scope(c, s);
  int obj = !(s->class_id < 0 && self_is_main(c, id)), hit = 0;
  if (p.region >= 0) {
    struct MainChainSlot *sl = ct_main_chain_slot(c, name, p.region, p.seq, p.node, obj, &hit);
    if (hit) r = sl->r, *step = sl->step, *d = sl->d;
    else r = elab_main_chain_at(c, name, p, obj, d, step), sl->r = r, sl->step = *step, sl->d = *d;
  }
  else {
    ElabWindow w;
    int wr = elab_call_window(c, id, &w);
    if (wr != ELAB_YES || w.never) r = ELAB_UNKNOWN;
    else if (!both) r = ct_main_chain_win(c, name, w, obj, step, d);
    else {
      const LrDef *d2 = NULL;
      int st2 = EMC_NONE;
      r = ct_main_chain_win(c, name, w, 0, step, d);
      int r2 = ct_main_chain_win(c, name, w, 1, &st2, &d2);
      if (r != r2 || *step != st2 || *d != d2) r = ELAB_UNKNOWN;
    }
  }
  if (framed) elab_plan_leave(c);
  if (p.region < 0 && r == ELAB_UNKNOWN) *step = EMC_NONE;   /* (a body's site: its unsure answer is the table's here) */
  return r;
}
/* The method of a module main extends, Object prepends or Object includes that the main-chain fact answers for call
   id, as the table's entry for that module holds it (toplevel_mod_method's scope, which shares the definition); -1
   where another step answers or none does; -2 where the fact does not answer (the table's) */
static int ct_main_mixin(Compiler *c, int id, const char *name, int *ext) {
  int step;
  const LrDef *d;
  int o = ct_main_chain(c, id, name, 0, &step, &d);
  if (o == ELAB_UNKNOWN) return -2;
  if (o == MCH_MOVES) return -3;
  if (o != ELAB_YES || (step != EMC_EXTEND && step != EMC_PREPEND && step != EMC_INCLUDE)) return -1;
  *ext = step == EMC_EXTEND;
  if (*ext) return d && d->scope > 0 && d->scope < c->nscopes ? d->scope : -2;
  int im = comp_included_method_index(c, name, id);
  if (im >= 0 && d && c->scopes[im].def_node == d->node) return im;
  return d && d->scope > 0 && d->scope < c->nscopes ? d->scope : -2;
}
/* See call_plan.h. */
int cplan_main_chain_step(Compiler *c, int id, const char *name) {
  int step;
  const LrDef *d;
  /* (a capture asks it of the defs too: method(:sym)'s maker is no call the defs' versions bind) */
  int o = ct_main_chain(c, id, name, is_bound_method_maker(nt_str(c->nt, id, "name")), &step, &d);
  return o == ELAB_YES || o == ELAB_NO ? step : -1;   /* (EMC_NONE, or EMC_STOP for an undef that stops the lookup) */
}
/* Does a bare call of `name` at call id, in top-level code, meet a mixin or def of the name that runs under a condition,
   or from a body that may not have run (an `include L if cond`, an extend inside a method called later)? CRuby finds
   its method only once it has run, which the record cannot tell: the call is refused (CRF_MAIN_CHAIN). A call whose
   target is fixed (a compiler-made builtin call, `Kernel.name`) takes no lookup of main's chain: not refused. */
static int cplan_main_chain_unsure(Compiler *c, int id) {
  const char *name = nt_str(c->nt, id, "name");
  int step;
  const LrDef *d;
  return nt_kind(c->nt, id) == NK_CallNode && name && nt_ref(c->nt, id, "receiver") < 0 &&
         !(cplan_builtin_call(c, id) || nt_int(c->nt, id, "kernel_recv", 0)) &&
         ct_main_chain(c, id, name, 0, &step, &d) == ELAB_UNKNOWN && step == EMC_UNSURE;
}
/* Does the main-chain fact put a mixin's method ahead of main's and Object's own at call id (an extend, an Object
   prepend), so that the top-level def of the name does not answer where the ask goes to the mixin (CT_TOP | CT_INCLUDED;
   CT_TOP alone keeps the top-level def)? Not an extend over a top-level def of the name: the emitters ask CT_TOP alone
   and bind that def, so inference answers it too, where both would otherwise type the call differently (a bare call
   of an extend's method over a top-level def of the name is the follow-up's, with the emitters' ask). */
static int ct_main_mixin_ahead(Compiler *c, int id, const char *name) {
  int step;
  const LrDef *d;
  if (ct_main_chain(c, id, name, 0, &step, &d) != ELAB_YES || (step != EMC_EXTEND && step != EMC_PREPEND)) return 0;
  return step == EMC_PREPEND || call_target(c, id, -1, name, CT_TOP, NULL) < 0;
}
static int ct_lookup(Compiler *c, int id, int cls, const char *name, unsigned *flags, MethodLookup *ml) {
  /* (CT_TOP | CT_INCLUDED asks main's whole chain at the call: a module main extends or Object prepends answers ahead of
     the top-level def where the fact puts it there, else the top-level def, else a module Object includes. CT_TOP alone
     is the top-level functions' ask and keeps that meaning) */
  if ((*flags & (CT_TOP | CT_INCLUDED)) == (CT_TOP | CT_INCLUDED)) {
    unsigned f = *flags & ~(unsigned)CT_TOP;
    int mi = -1;
    if (id < 0 || !ct_main_mixin_ahead(c, id, name)) {
      f = *flags & ~(unsigned)CT_INCLUDED;
      mi = ct_lookup(c, id, cls, name, &f, ml);
      if (mi >= 0 || (ml && (ml->kind == ML_NONE || ml->kind == ML_SINGLETON))) { *flags = f; return mi; }   /* (an undef's stop or main's own core method answers none: nothing behind it) */
      f = *flags & ~(unsigned)CT_TOP;
    }
    mi = ct_lookup(c, id, cls, name, &f, ml);
    *flags = f | CT_TOP;
    return mi;
  }
  /* (a module the top level includes brings its method into Object's chain: where the name has versions there, a
     top-level def after the include among them, the call's own point chooses, as a call on Object's does) */
  if (*flags & CT_INCLUDED) {
    int ext = 0, im = ct_main_mixin(c, id, name, &ext);
    int ob = comp_core_class(c, CORE_OBJECT);
    /* (where the fact does not answer, or the answer moves with when the body runs: the table's, as before) */
    if (im < -1) im = comp_included_method_index(c, name, id);
    /* (an extend's method is main's singleton's, no version of Object's) */
    if (im >= 0 && id >= 0 && !ext && ob >= 0 && ct_versioned(c, im, name) && ct_block_self_call(c, id) < 0) {
      MethodLookup at = *ml;
      int vi = ct_version(c, id, ob, name, *flags, im, &at);
      /* (the version a top-level def or its entry makes; a module's own copy, which a prepend or an alias of the
         module's singleton chain also reads, stays the module's method) */
      if (vi >= 0 && vi != im && (c->scopes[vi].entry || c->scopes[vi].shadow || c->scopes[vi].class_id < 0)) { *ml = at; return vi; }
    }
    return im;
  }
  const LrDef *d = NULL;
  if ((*flags & CT_TOP) && id >= 0 && ct_main_stopped(c, id, name)) {
    if (ml) ml->kind = ML_NONE;
    return -1;
  }
  int sg = 0;
  if ((*flags & CT_TOP) && id >= 0 && main_singleton_method(name) && ct_runs_on_main(c, id)) {
    int framed = elab_plan_enter(c, id);
    sg = elab_main_singleton_at(c, name, elab_point_of(c, id), &d) == ELAB_YES && !d;
    if (framed) elab_plan_leave(c);
  }
  if (sg) {
    if (ml) ml->kind = ML_SINGLETON;
    return -1;
  }
  int si = (*flags & CT_TOP) && id >= 0 ? comp_main_method(c, name) : -1;
  int own = si >= 0 && ct_runs_on_main(c, id) ? ct_main_own(c, id, name) : -1;
  int attr = 0;
  int mi = own == 1 ? si : ct_table(c, cls, name, *flags, ml ? &ml->def_class : NULL, &attr);
  if (attr && ml) ml->kind = ML_ATTR;
  /* (a call on main where main's singleton holds no method, main's own
     removed or not yet defined, does not take it: Object's chain answers,
     as with none) */
  if (!own && mi >= 0 && c->scopes[mi].is_cmethod && c->scopes[mi].class_id < 0) return -1;
  if ((*flags & CT_TOP) && mi >= 0 && c->scopes[mi].is_cmethod) *flags |= CT_CMETHOD;
  return mi;
}
/* Whether class-side scope si is a copy of another class's `def` (an
   extended module's method, or an inherited one specialized for a subclass):
   it shares the source scope's def node, and the source comes first. A
   class's own class methods leave it out. */
static int pr_cmethod_copy(Compiler *c, int si) {
  Scope *s = &c->scopes[si];
  if (s->origin_module_ci > 0) return 1;
  if (s->alias_of) return 0;
  if (s->def_node < 0) return 0;
  for (int j = an_scopes_named_first(c, s->name); j >= 0 && j < si; j = an_scopes_named_next(c, s->name, j))
    if (c->scopes[j].def_node == s->def_node && c->scopes[j].class_id != s->class_id) return 1;
  return 0;
}
/* Whether cls's attribute reader (writer: `wr`, name its base) is its own:
   attr readers and writers are flattened into descendants at analyze time,
   so "own" means present here but not in the parent chain; a `class T <
   Struct.new(:a)` member belongs to the anonymous superclass, so it counts
   as the class's with `copies` only. */
static int pr_accessor_own(Compiler *c, int cls, const char *name, int wr, int copies) {
  int parent = c->classes[cls].parent;
  if (parent >= 0 && (wr ? comp_writer_in_chain(c, parent, name, NULL) : comp_reader_in_chain(c, parent, name, NULL))) return 0;
  return copies || !refl_member_of_struct_superclass(c, cls, name);
}
/* The scope of cls's own body of name that a prepended module's method of
   the name displaced (`__prep <n> name`, SHADOW_PREP): the entry the class
   defines under the name, which the module's copy answers; -1 where none. */
static int pr_prep_body(Compiler *c, int cls, const char *name) {
  int mi = comp_method_in_class(c, cls, name);
  if (mi < 0 || c->scopes[mi].origin_module_ci <= 0) return -1;
  ClassInfo *k = &c->classes[cls];
  for (int i = 0; i < k->nprep_chain; i++) {
    if (!k->prep_to[i]) continue;
    for (int si = an_scopes_named_first(c, k->prep_to[i]); si >= 0; si = an_scopes_named_next(c, k->prep_to[i], si)) {
      Scope *s = &c->scopes[si];
      if (s->class_id == cls && s->shadow == SHADOW_PREP && s->origin_module_ci <= 0 && s->shadow_user && sp_streq(s->shadow_user, name)) return si;
    }
  }
  return -1;
}
/* Behind a guard entry of name (a mixin in a reopening, whose copy answers
   once its statement has run): does cls hold a body of its own under the
   name, hidden as `__prep <n> name`? The stand-in a mixin makes where the
   class had none (mixin_method_behind's `= super`, Scope.super_behind) is
   no method of the class's: Module#instance_methods(false) lists the
   name for the first and not for the second, as CRuby does (#12224). */
static int pr_guarded_own(Compiler *c, int cls, const char *name) {
  for (int si = 0; si < c->nscopes; si++) {
    const Scope *s = &c->scopes[si];
    if (s->class_id == cls && !s->is_cmethod && s->shadow == SHADOW_PREP && s->origin_module_ci <= 0 &&
        !s->super_behind && s->shadow_user && sp_streq(s->shadow_user, name)) return 1;
  }
  return 0;
}
/* Whether cls's alias table holds name as an alias of its side (the
   alias entry has its own visibility; its original is the name's method):
   an instance alias, or on the class side a singleton alias or one of a
   class method; the class's own statements under CT_OWN (an alias an
   included module made is the module's), else the chain's. */
static int pr_alias(Compiler *c, int cls, unsigned side, const char *name) {
  for (int k = cls, g = 0; k >= 0 && g <= c->nclasses; k = c->classes[k].parent, g++) {
    ClassInfo *kc = &c->classes[k];
    for (int i = 0; i < kc->naliases; i++) {
      if (!kc->alias_new[i] || !sp_streq(kc->alias_new[i], name)) continue;
      int sg = kc->alias_sg && kc->alias_sg[i];
      if (side & CT_CMETHOD) {
        const char *od = comp_resolve_cm_alias_at(c, k, name, NULL);
        if (sg || (comp_cmethod_in_chain(c, k, od, NULL) >= 0 && comp_method_in_chain(c, k, od, NULL) < 0)) return 1;
      }
      else if (!sg && (kc->alias_node[i] >= 0 || !(side & CT_OWN))) return 1;
    }
    if (side & CT_OWN) break;
  }
  return 0;
}
/* The visibility a call of name on cls's route checks: the final table's,
   which a lookup of the name finds declared (public where none is). Under
   CT_OWN the class's own entry's: a prepend's renamed body of the name is
   its entry (its shadow name carries the visibility), and an alias that
   states none takes the visibility its original had at the alias. */
static int ct_vis(Compiler *c, int cls, const char *name, unsigned flags) {
  if (cls < 0 || (flags & CT_TOP)) return SP_VIS_PUBLIC;
  if (flags & CT_CMETHOD) return comp_cmethod_vis_declared(c, cls, name, NULL);
  if (!(flags & CT_OWN)) return comp_method_vis_declared(c, cls, name, NULL);
  ClassInfo *k = &c->classes[cls];
  int si = pr_prep_body(c, cls, name);
  const char *own = si >= 0 ? c->scopes[si].name : name;
  for (int i = 0; i < k->nvis; i++)
    if (sp_streq(k->vis_names[i], own)) return k->vis_kinds[i];
  return pr_alias(c, cls, CT_OWN, name) ? comp_method_vis_declared(c, cls, name, NULL) : SP_VIS_PUBLIC;
}
/* ct_lookup, and for a call asked on Object that its tables give no
   method: the top-level def of the name where it is one of the versions
   Object's chain holds beside a module's (ct_top_chain), whose entry the
   record then chooses (the def hides the module's copy in the tables) */
static int ct_lookup_object_top(Compiler *c, int id, int cls, const char *name, unsigned *flags, MethodLookup *ml) {
  int mi = ct_lookup(c, id, cls, name, flags, ml);
  if (mi >= 0 || id < 0 || cls < 0 || cls != comp_core_class(c, CORE_OBJECT) || (*flags & ~(unsigned)(CT_PUBLIC | CT_PROTECTED | CT_NOFACE)) ||
      ml->kind == ML_SINGLETON || ml->kind == ML_ATTR || elab_main_object_one_entry(c, name) || ct_top_chain(c, name, 0) != cls) return mi;
  unsigned tf = CT_TOP;
  MethodLookup tl = { .def_class = -1 };
  int ti = ct_lookup(c, id, -1, name, &tf, &tl);
  return ti >= 0 && c->scopes[ti].class_id < 0 ? ti : mi;
}
/* The accessor an ask of CT_ATTR finds (ML_ATTR) as it stands at call id: the record holds the attr_* calls of a name
   as versions (ct_attr_version), so where none has run by id's point and the class has no Struct or Data member of the
   name, the ask answers none (MLW_RECORD). Where the name has versions otherwise, the answer is the table's, reported
   as an `accessor` table answer (id -1 asks it of every class: a candidate scan). */
typedef struct AttrFact { int cls, first, versioned; char *name; } AttrFact;
struct AttrFacts { AttrFact *slot; int cap, used; ElabStamp stamp; unsigned long hgen; };
void attr_facts_free(Compiler *c) {
  if (!c->attr_facts) return;
  for (int i = 0; i < c->attr_facts->cap; i++) free(c->attr_facts->slot[i].name);
  free(c->attr_facts->slot);
  free(c->attr_facts);
  c->attr_facts = NULL;
}
/* Has `name` versions, and is the attr_* call that is the only version of it on cls's chain the first thing of the
   program's that matters to a call of it (elab_accessor_first)? Kept by (class, name) while the record's shape stands: a node a pass appends,
   or the rebuild of the map it causes, moves no attr_* call's place, and a pass that appends between asks would
   otherwise drop the facts at each. */
static const AttrFact *attr_fact(Compiler *c, int cls, const char *name) {
  struct AttrFacts *m = c->attr_facts;
  ElabStamp cur = elab_stamp(c);
  if (m) cur.nt = m->stamp.nt, cur.rec = m->stamp.rec;
  if (m && (!elab_stamp_same(m->stamp, cur) || m->hgen != elab_hgen(c))) { attr_facts_free(c); m = NULL; }
  if (!m) {
    if (!(c->attr_facts = m = calloc(1, sizeof *m))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->stamp = elab_stamp(c);
    m->hgen = elab_hgen(c);
  }
  if (2 * (m->used + 1) > m->cap) {
    AttrFact *old = m->slot;
    int ocap = m->cap;
    m->cap = ocap ? 2 * ocap : 256;
    if (!(m->slot = calloc((size_t)m->cap, sizeof *m->slot))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!old[i].name) continue;
      int j = (int)(ver_mix(sp_strhash(old[i].name) + (unsigned)old[i].cls * 0x9E3779B1u) & (unsigned)(m->cap - 1));
      while (m->slot[j].name) j = (j + 1) & (m->cap - 1);
      m->slot[j] = old[i];
    }
    free(old);
  }
  int j = (int)(ver_mix(sp_strhash(name) + (unsigned)cls * 0x9E3779B1u) & (unsigned)(m->cap - 1));
  for (; m->slot[j].name; j = (j + 1) & (m->cap - 1))
    if (m->slot[j].cls == cls && sp_streq(m->slot[j].name, name)) return &m->slot[j];
  ElabVersion vs[2];
  int versioned = ct_version_named(c, name), first = 0;
  if (versioned) {
    int n = elab_chain_versions(c, cls, 0, name, vs, 2);
    first = n == 1 && ct_attr_version(vs[0].v) && vs[0].v->node >= 0 && elab_accessor_first(c, vs[0].v->node, name, cls);
  }
  m->slot[j] = (AttrFact){ cls, first, versioned, name_dup(name) };
  m->used++;
  return &m->slot[j];
}
static void ct_attr_point(Compiler *c, int id, int cls, unsigned flags, const char *name, MethodLookup *ml) {
  const AttrFact *af = attr_fact(c, cls, name);
  if (!af->versioned) return;
  if (id >= 0) {
    /* (made before any code of the program's can run: live at every call, none asks a window) */
    if (af->first) return;
    int none, framed = elab_plan_enter(c, id);
    const LrDef *d = ct_met_version(c, id, cls, name, 0, &none);
    if (framed) elab_plan_leave(c);
    if (none && ct_struct_member(c, cls, name) < 0) {
      *ml = (MethodLookup){ .kind = ML_NONE, .why = MLW_RECORD, .def_class = ml->def_class };
      return;
    }
    if (d && ct_attr_version(d)) return;
  }
  if (g_method_table_check) mtab_table(c, id, cls, flags, name, MLW_ACCESSOR);
}
/* Is call id inside a native twin's def (desugar_native_func_defs), the call
   into C it makes? Its builtin_only record means "never back into this def",
   not the builtin's own method. */
static int ct_native_twin_call(Compiler *c, int id) {
  const Scope *s = comp_scope_of(c, id);
  return comp_scope_native_twin(c, (int)(s - c->scopes));
}
/* Does call id, asked for name, bind the builtin's own method (ML_BUILTIN)?
   Only the name the call itself calls: a send's sent name, a reach (super's
   lookup), a capture and an implied call are other asks. Not main's own text
   (cplan_main_text_call): that call is flagged for the same reason, and main's
   singleton answers it (ML_SINGLETON), as the plan's question about it asks.
   Nor the forward a program's Array#concat keeps as its builtin version
   (desugar_concat_builtin_version): the version tables bind it to that
   version, whose parameter its argument types. */
static int ct_builtin_own(Compiler *c, int id, const char *name, unsigned flags) {
  /* (no call marked, no read: call_target is asked at every site in every round) */
  if (!nt_any_builtin_call(c->nt) || id < 0 || !name || (flags & (CT_REACH | CT_CAPTURE | CT_IMPLIED)) || !cplan_builtin_call(c, id) ||
      cplan_main_text_call(c, id) || nt_int(c->nt, id, "concat_builtin_version", 0)) return 0;
  const char *own = nt_str(c->nt, id, "name"), *orig = nt_str(c->nt, id, "orig_name");
  return ((own && sp_streq(own, name)) || (orig && sp_streq(orig, name))) && !ct_native_twin_call(c, id);
}
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, MethodLookup *out) {
  MethodLookup own = { .def_class = -1 }, *ml = out ? out : &own;
  int dc0 = ml->def_class, mi = -1, pin, r;
  char setter[300];
  /* (a writer's name is its attribute's base: the call is the setter) */
  if (flags & CT_WRITER) {
    /* (spelled by hand: snprintf here was a measurable share of the inference passes, asked at every attribute read) */
    size_t nl = strlen(name);
    if (nl + 2 > sizeof setter) nl = sizeof setter - 2;
    memcpy(setter, name, nl);
    setter[nl] = '=';
    setter[nl + 1] = 0;
    name = setter;
  }
  /* (a no-site ask of a reader the call sites share, made while a node's frame is open: its read stays that node's) */
  int open = !((flags & CT_SHARED) && id < 0 && mtab_framed(c));
  if (open) mtab_enter(c);
  flags = ct_vis_flags(c, id, name, flags);
  *ml = (MethodLookup){ .def_class = dc0 };
  if (ct_send_shadow(c, id, name)) {
    ml->kind = ML_NONE;
    r = -1;
  }
  else if (ct_builtin_own(c, id, name, flags)) {
    ml->kind = ML_BUILTIN;
    r = -1;
  }
  else if ((mi = ct_lookup_object_top(c, id, cls, name, &flags, ml)) < 0 || id < 0 || (flags & CT_INCLUDED) ||
           (!(flags & (CT_REACH | CT_CAPTURE | CT_IMPLIED)) && !ct_calls(c, id, name))) {
    if (mi < 0) {
      if (ml->kind != ML_SINGLETON && ml->kind != ML_ATTR) ml->kind = ML_NONE;
      /* (a reach ask, whose reader tells a none the record proves from one
         the tables give: an accessor has no method scope, so the record's
         proof is asked here, where the call runs before every version) */
      if (ml->kind == ML_NONE && (flags & CT_REACH) && id >= 0 && ct_before_versions(c, id, cls, name, flags, -1)) ml->why = MLW_RECORD;
      if ((pin = ct_pin_below(c, id, cls, name, flags)) >= 0) {
        *ml = (MethodLookup){ .kind = ML_METHOD, .def_class = c->scopes[pin].class_id };
        mi = pin;
      }
    }
    else if (ct_versioned(c, mi, name) || c->scopes[mi].mixin_guarded) {   /* (the record was not asked) */
      ml->kind = ML_TABLE;
      ml->why = MLW_UNASKED;
    }
    r = mi;
  }
  else if ((ct_versioned(c, mi, name) || c->scopes[mi].mixin_guarded || c->scopes[mi].entry || ct_pin_binds(c, id, cls, name, flags)) &&
           (pin = ct_pin_for(c, id, name)) != -1 &&
           /* (a pin answers the side its entry is on: an instance entry's
              arm, asked for a class method of a Class value its receiver
              may hold, finds that class's own method. A call on main takes
              main's singleton versions and Object's alike.) */
           (pin < 0 || (flags & CT_TOP) || !c->scopes[pin].is_cmethod == !(flags & CT_CMETHOD))) {   /* (an entry's arm: its method's own class) */
    if (pin == CT_PIN_BUILTIN || ct_top_version(c, pin, flags)) {
      ml->kind = ML_NONE;
      r = -1;
    }
    /* (an arm of main's own entry, which holds no self, pinned to Object's
       version: the call's ask on Object binds it with main as self, as
       ct_version_at's) */
    else if ((flags & CT_TOP) && c->scopes[pin].class_id >= 0 && comp_scope_of(c, id) && comp_scope_of(c, id)->class_id < 0) {
      ml->kind = ML_OBJECT;
      r = -1;
    }
    else {
      ml->def_class = c->scopes[pin].class_id;
      r = pin;
    }
  }
  else if (ct_undefined_at(c, id, cls, name, flags, mi)) {
    /* a call the record proves runs before any version of a class method or
       a top-level function is defined raises, whatever versions follow; the
       entry's else arm answers only a call that may run before them. One at
       a point that may run any time after it (ct_after_point) takes the
       entry, whose cells tell at run time; with no entry, it raises too:
       nothing proves it runs after a def (MLW_RECORD) */
    int vi = ct_versioned(c, mi, name) && ct_after_point(c, id) ? ct_version(c, id, cls, name, flags, mi, ml) : -1;
    if (vi >= 0 && c->scopes[vi].entry) r = vi;
    else {
      /* (but the core's hook, which a send reaches: ct_core_hook) */
      *ml = (MethodLookup){ .kind = ML_NONE, .why = ct_core_hook(c, id, name, flags) ? 0 : MLW_RECORD, .def_class = dc0 };
      r = -1;
    }
  }
  else {
    int asked = ct_versioned(c, mi, name) || (flags & CT_CAPTURE) || kernel_string_implies(c, id, cls, name, flags) || pos_front(c, mi);
    if (asked) mi = ct_version(c, id, cls, name, flags, mi, ml);
    /* An alias's record already selected its captured definition. */
    if (mi >= 0 && c->scopes[mi].mixin_guarded && sp_streq(name, c->scopes[mi].name)) r = ct_guard(c, id, mi, flags, ml);
    else {
      if (mi >= 0 && c->scopes[mi].mixin_guarded && !asked) {   /* (neither the record nor the guard was asked) */
        ml->kind = ML_TABLE;
        ml->why = MLW_UNASKED;
      }
      r = mi;
    }
  }
  /* (a variable-like call of a conditional def's dispatcher: its twin, ver_vcall_twins) */
  const Scope *rs = r >= 0 ? &c->scopes[r] : NULL;
  int tw = rs && c->nvcall_twins && id >= 0 && rs->def_node >= 0 && nt_kind(c->nt, id) == NK_CallNode && nt_vcall(c->nt, id) &&
           nt_int(c->nt, rs->def_node, "cond_raise_els", 0)
           ? ver_entry_ix(c, VER_ENTRY_CHAIN, 0, rs->class_id, rs->is_cmethod, name, 0, VF_VCALL) : -1;
  if (tw >= 0) r = c->ver_entries[tw].scope;
  if (r >= 0 && c->scopes[r].entry && ml->kind != ML_ENTRY) {   /* (an entry answered by name too) */
    ml->kind = ML_ENTRY;
    ml->why = 0;
  }
  if ((flags & CT_ATTR) && ml->kind == ML_ATTR) ct_attr_point(c, id, cls, flags, name, ml);
  /* (the method answered is one the asker reads: a version, pin or entry the record chose past the table's, which
     the table lookup did not note; infer_type's memo notes it) */
  if (r >= 0) IMEMO_DEP_SCOPE(r);
  if (open) mtab_leave(c);
  if (g_method_table_check && ml->kind == ML_TABLE) mtab_table(c, id, cls, flags, name, ml->why);
  return r;
}
/* See call_plan.h. */
int call_attr_target(Compiler *c, int id, int cls, const char *name, unsigned flags, MethodLookup *out) {
  MethodLookup own = { .def_class = -1 }, *ml = out ? out : &own;
  return call_target(c, id, cls, name, flags, ml) < 0 && ml->kind == ML_ATTR;
}
/* See call_plan.h. */
int call_answers(Compiler *c, int id, int cls, const char *name, unsigned flags, MethodLookup *out) {
  MethodLookup own = { .def_class = -1 }, *ml = out ? out : &own;
  return call_target(c, id, cls, name, flags, ml) >= 0 || ml->kind == ML_ATTR;
}


/* Whether the final tables hold name on cls's side as a method of it: the
   method mi the table lookup found, an accessor or an alias; mi is the
   generated iterator of a Struct or a Data class (unless CT_REACH: what a
   call of the name runs, not what a reflection list shows), or a module's
   copy under CT_OWN, no method of the class's. Under CT_OWN the
   class's own entries: its defs, a prepend's renamed body, its aliases and
   the accessors it introduces. */
static int pr_in_table(Compiler *c, int cls, unsigned side, const char *name, int mi) {
  ClassInfo *k = &c->classes[cls];
  char base[256];
  if (side & CT_CMETHOD) {
    if (mi >= 0 && !((side & CT_OWN) && pr_cmethod_copy(c, mi))) return 1;
    if (pr_alias(c, cls, side, name)) return 1;
    if (name_is_plain_setter(name)) return setter_base_name(name, base, sizeof base) && comp_is_sg_writer(k, base) && (!(side & CT_OWN) || comp_sg_accessor_own(c, cls, name));
    return comp_is_sg_reader(k, name) && (!(side & CT_OWN) || comp_sg_accessor_own(c, cls, name));
  }
  int wr = setter_base_name(name, base, sizeof base);
  if (!(side & CT_OWN))
    return (mi >= 0 && !method_hidden_from_reflection(c, cls, name) && ((side & CT_REACH) || !scope_is_struct_synth(c, mi))) || comp_reader_in_chain(c, cls, name, NULL) ||
           (wr && comp_writer_in_chain(c, cls, base, NULL)) || pr_alias(c, cls, side, name);
  /* (a guard entry the table answers by the name, ENTRY_TABLE, is the
     compiler's: the class's own is the body it hides, pr_guarded_own) */
  if (mi >= 0 && c->scopes[mi].entry == ENTRY_TABLE && c->scopes[mi].mixin_guarded)
    return pr_guarded_own(c, cls, name) || pr_alias(c, cls, side, name);
  return (mi >= 0 && !scope_is_struct_synth(c, mi) && c->scopes[mi].origin_module_ci == 0) ||
         pr_prep_body(c, cls, name) >= 0 ||
         (comp_is_reader(k, name) && pr_accessor_own(c, cls, name, 0, 0)) ||
         (wr && comp_is_writer(k, base) && pr_accessor_own(c, cls, base, 1, 0)) || pr_alias(c, cls, side, name);
}
/* Does instance class cls answer name with no entry of its own, by what a mixin or a core class brings: the names of
   Enumerable once it includes the module and has the `each` it serves (the synthesized __enum_to_a marks any yielding
   `each`, include or not; a Struct is Enumerable without the line), Comparable's once it includes that module and
   defines `<=>`, and the names a Struct and a Data carry from their core class. The names are asked first: every
   candidate of a list asks this. */
static int pr_implied(Compiler *c, int cls, unsigned side, const char *name) {
  if ((side & CT_CMETHOD) || cls < 0) return 0;
  int en = name_is_enumerable_module_method(name), cm = is_comparable_module_method(name);
  int sk = class_struct_kind(c, cls);
  if (!en && !cm && !is_struct_core_name(name, sk)) return 0;
  if (en && comp_method_in_chain(c, cls, "__enum_to_a", NULL) >= 0 &&
      (sk == 1 || class_mixes_in(c, cls, elab_module_named(c, "Enumerable"), 0))) return 1;
  if (cm && comp_method_in_chain(c, cls, "<=>", NULL) >= 0 && class_is_comparable(c, cls)) return 1;
  return is_struct_core_name(name, sk);
}
/* Does an undef_method of name stand on cls's chain, on the instance side (whatever builtin answers the name)? */
static int pr_undef(Compiler *c, int cls, unsigned side, const char *name) {
  return !(side & CT_CMETHOD) && cls >= 0 && comp_is_undeffed_in_chain(c, cls, name);
}
/* The final tables' answer for name on cls's side: what they hold (pr_in_table), the visibility (ct_vis) and, on the
   instance side, whether an undef_method on the chain takes the name away, whatever builtin answers it (until an
   undef answers at the point, the table's). Returns the table's method, as ct_table finds it. */
static int pr_table(Compiler *c, int cls, unsigned side, const char *name, CplanPresence *out) {
  mtab_enter(c);
  int mi = ct_table(c, cls, name, side, NULL, NULL);
  *out = (CplanPresence){ .table = pr_in_table(c, cls, side, name, mi), .vis = ct_vis(c, cls, name, side),
                          .undef = pr_undef(c, cls, side, name), .implied = pr_implied(c, cls, side, name),
                          .sg_only = !(side & CT_CMETHOD) && cls >= 0 && comp_name_is_sg_alias_only(c, cls, name) };
  mtab_leave(c);
  return mi;
}
/* Is presence site id in a block its call may run after it answers, as
   the types tell: the block of a call on a receiver typed no one kind
   (poly or unknown; a lazy enumerator has no type of its own: `l =
   [1].lazy; l.map { }`), or of a stage of a lazy chain a desugar runs
   eagerly over a finite source (its first stage is marked lazy_stage),
   where CRuby runs it once the chain is forced; up to the def or lambda
   whose body holds it? The record places such a block at its call, an
   iterator's name read as run at once, so its none there is not the
   site's. */
static int pr_in_untyped_block(Compiler *c, int id) {
  int n;
  const int *par = elab_node_parents(c, &n);
  for (int a = id; a >= 0 && a < n && par[a] >= 0; a = par[a]) {
    NodeKind k = nt_kind(c->nt, a);
    int call = par[a], rv;
    if (k == NK_DefNode || k == NK_LambdaNode) return 0;
    if (k != NK_BlockNode || nt_kind(c->nt, call) != NK_CallNode || nt_ref(c->nt, call, "block") != a ||
        (rv = nt_ref(c->nt, call, "receiver")) < 0) continue;
    TyKind t = comp_ntype(c, rv);
    if (t == TY_POLY || t == TY_UNKNOWN) return 1;
    for (int s = call; s >= 0 && nt_kind(c->nt, s) == NK_CallNode; s = nt_ref(c->nt, s, "receiver"))
      if (nt_int(c->nt, s, "lazy_stage", 0)) return 1;
  }
  return 0;
}
/* Does node run at once, where it stands (ct_before_versions' proof for a call: no method body, and no block or lambda
   that may run later, ct_after_point and elab_blocks_run_at_once)? */
int cplan_runs_at_once(Compiler *c, int node) {
  if (elab_in_method_body(c, node)) return 0;
  int framed = elab_plan_enter(c, node);
  int once = !ct_after_point(c, node) && elab_blocks_run_at_once(c, node);
  if (framed) elab_plan_leave(c);
  return once;
}
/* See call_plan.h. */
int cplan_runs_once(Compiler *c, int node) {
  return cplan_runs_at_once(c, node) && !elab_runs_again(c, node);
}
/* See call_plan.h. */
int cplan_undef_at(Compiler *c, int id, int cls, const char *name) {
  LoadPoint p = elab_point_of(c, id);
  if (p.region < 0 || !cplan_runs_once(c, id)) return -1;
  const LrDef *d = NULL;
  int o = elab_method_at(c, cls, name, 0, p, &d);
  if (o == ELAB_UNKNOWN) return -1;
  return o == ELAB_NO && d && d->kind == LR_UNDEF;
}
/* See call_plan.h. */
PresenceKind cplan_presence(Compiler *c, int id, int cls, unsigned side, const char *name, CplanPresence *out) {
  MethodLookup ml = { .def_class = -1 };
  pr_table(c, cls, side, name, out);
  /* an undef_method of a reopened builtin's own method answers where it runs (as ct_builtin_reopen_undefined_at asks a
     reopened builtin's defs); a program class's keeps the table's, whose chain the record does not place */
  if (out->undef && id >= 0 && !(side & (CT_CMETHOD | CT_TOP | CT_OWN)) && cls >= 0 && cls < c->nclasses && c->classes[cls].name &&
      c->classes[cls].enclosing_class < 0 && builtin_class_id(c->classes[cls].name) &&
      name_list_has(c->classes[cls].undefs, c->classes[cls].nundefs, name))
    out->undef = cplan_undef_at(c, id, cls, name) != 0;   /* (where the record cannot tell, the table's) */
  if (pr_in_untyped_block(c, id)) return PR_TABLE;
  int e = call_target(c, id, cls, name, side | CT_REACH, &ml);
  /* (an instance's entry the call takes there: its cells tell, cplan_presence_cells) */
  if (ml.kind == ML_ENTRY && !(side & (CT_CMETHOD | CT_TOP)) && !respond_to_missing_defined(c)) return PR_CELLS;
  /* (and a module's own, whose chain's cells a guard writes: pres_module_cells) */
  if (!(side & (CT_CMETHOD | CT_TOP)) && pres_module_cells(c, id, e, &ml, cls, name) && ver_history_vis_guard(c, cls, name) >= 0) return PR_CELLS;
  if (ml.kind == ML_TABLE || ml.kind == ML_ENTRY) return PR_TABLE;
  if (ml.kind == ML_NONE && ml.why == MLW_RECORD) return PR_ABSENT;   /* (the record's none) */
  /* (a visibility word a method body runs gives the table no visibility: the one the record has where id runs,
     elab_method_body_word) */
  LoadPoint p = out->table && id >= 0 && !(side & CT_TOP) && elab_method_body_word(c, name, (side & CT_CMETHOD) != 0) ? elab_point_of(c, id) : (LoadPoint){ -1, -1, -1 };
  int vis = p.region >= 0 ? elab_method_vis_at(c, cls, name, (side & CT_CMETHOD) != 0, p) : -1;
  if (vis >= 0) out->vis = vis;
  return out->table ? PR_PRESENT : PR_ABSENT;
}
/* See call_plan.h. */
void cplan_table_presence(Compiler *c, int cls, unsigned side, const char *name, CplanPresence *out) {
  pr_table(c, cls, side, name, out);
}
/* See call_plan.h. */
int cplan_undefined(Compiler *c, int cls, unsigned side, const char *name) {
  mtab_enter(c);
  int u = pr_undef(c, cls, side, name);
  mtab_leave(c);
  return u;
}
/* See call_plan.h. */
int cplan_list_omits(Compiler *c, int id, int cls, unsigned side, const char *name) {
  ElabVersion vs[1];
  CplanPresence pr;
  if (id < 0 || cls < 0 || !name || elab_chain_versions(c, cls, (side & CT_CMETHOD) != 0, name, vs, 1) == 0) return 0;
  return cplan_presence(c, id, cls, side, name, &pr) == PR_ABSENT && pr.table;
}

/* See call_plan.h. A chain lookup from class cls answers a scope owned by cls or one of its ancestors (chain_lookup:
   an alias resumes at an ancestor, and the walk is up the parents), so only those scopes are asked, in the order every
   scope was. The names are noted after the walk, outside the question's bracket: the reader's own reads stay its own. */
void cplan_chain_names(Compiler *c, int cls, CplanNameFn fn, void *ud) {
  unsigned char *anc = calloc((size_t)c->nclasses + 1, 1);
  int *own = calloc((size_t)c->nscopes + 1, sizeof *own), nown = 0;
  if (!anc || !own) cplan_oom();
  mtab_enter(c);
  for (int p = cls; p >= 0; p = c->classes[p].parent) anc[p] = 1;
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].name && !c->scopes[s].is_cmethod && !c->scopes[s].is_proc_form &&
        (c->scopes[s].class_id < 0 || (c->scopes[s].class_id < c->nclasses && anc[c->scopes[s].class_id])) &&
        call_target(c, -1, cls, c->scopes[s].name, CT_METHODS, NULL) == s)
      own[nown++] = s;
  free(anc);
  mtab_leave(c);
  for (int q = 0; q < nown; q++) fn(ud, c->scopes[own[q]].name);
  free(own);
  for (int p = cls; p >= 0; p = c->classes[p].parent)
    for (int m = 0; m < c->n_native_methods; m++)
      if (c->native_methods[m].class_id == p && !c->native_methods[m].kind) fn(ud, c->native_methods[m].name);
  for (int p = cls; p >= 0; p = c->classes[p].parent) {
    ClassInfo *cl = &c->classes[p];
    for (int r = 0; r < cl->nreaders; r++) fn(ud, cl->readers[r]);
    for (int w = 0; w < cl->nwriters; w++) {
      char wn[256];
      snprintf(wn, sizeof wn, "%s=", cl->writers[w]);
      fn(ud, wn);
    }
    for (int a = 0; a < cl->naliases; a++) {
      mtab_enter(c);
      int sg = comp_name_is_sg_alias_only(c, cls, cl->alias_new[a]);
      mtab_leave(c);
      if (!sg) fn(ud, cl->alias_new[a]);
    }
  }
}
/* See call_plan.h. */
void cplan_own_names(Compiler *c, int cls, CplanNameFn fn, void *ud) {
  int *own = calloc((size_t)c->nscopes + 1, sizeof *own), nown = 0;
  if (!own) cplan_oom();
  mtab_enter(c);
  for (int s = 0; s < c->nscopes; s++) {
    const Scope *os = &c->scopes[s];
    if (!os->name || os->is_cmethod || os->is_proc_form || os->class_id != cls) continue;
    if (call_target(c, -1, cls, os->name, CT_OWN, NULL) == s) own[nown++] = s;
  }
  mtab_leave(c);
  for (int q = 0; q < nown; q++) fn(ud, c->scopes[own[q]].name);
  free(own);
  const ClassInfo *rc = &c->classes[cls];
  for (int i = 0; i < rc->nreaders; i++) {
    mtab_enter(c);
    int shadowed = call_target(c, -1, cls, rc->readers[i], CT_OWN, NULL) >= 0;
    mtab_leave(c);
    if (!shadowed) fn(ud, rc->readers[i]);
  }
  for (int i = 0; i < rc->nwriters; i++) {
    char nm[260];
    if (snprintf(nm, sizeof nm, "%s=", rc->writers[i]) >= (int)sizeof nm) continue;
    mtab_enter(c);
    int shadowed = call_target(c, -1, cls, nm, CT_OWN, NULL) >= 0;
    mtab_leave(c);
    if (!shadowed) fn(ud, nm);
  }
}

typedef struct { Compiler *c; struct CplanMemo *m; int root, cap; } OmitBuild;
/* Does class k answer `name` on a side (CT_CMETHOD: the class's own) by a def, an alias or an accessor of a class or
   module of the program, not one of the roots'? The roots' defs and the top-level ones are what a respond_to? leaves
   out, and every class reaches them (a `Child < Object`'s instance side, a class value's Object ancestry): only a
   def another class holds can keep the name answering where they have not run. */
static int omit_class_answers(Compiler *c, int k, const char *name, unsigned side) {
  MethodLookup ml = { .def_class = -1 };
  int mi = call_target(c, -1, k, name, side, &ml);
  if (mi < 0) return ml.kind == ML_ATTR;
  int dc = c->scopes[mi].class_id;
  return dc >= 0 && !(c->classes[dc].name && is_object_root(c->classes[dc].name));
}
/* a name a respond_to? may leave out of the shared lists, once its siteless facts hold: it has versions, and no class
   of the program defines it besides the roots, no builtin answers it, and it is no private Kernel function */
static void omit_note(void *ud, const char *name) {
  OmitBuild *o = ud;
  Compiler *c = o->c;
  if (!elab_method_versions(c, name) || builtin_object_method_known(name) || builtin_instance_name_known(name) ||
      object_private_method_name(name)) return;
  for (int k = 0; k < c->nclasses; k++)
    if (!(c->classes[k].name && is_object_root(c->classes[k].name)) &&
        (omit_class_answers(c, k, name, 0) || omit_class_answers(c, k, name, CT_CMETHOD))) return;
  /* (the tables must hold the name as the top-level def, or as the root's own: where they do not, no call leaves it out) */
  int mi = o->root < 0 ? call_target(c, -1, -1, name, CT_TOP, NULL) : call_target(c, -1, o->root, name, CT_OWN, NULL);
  if (mi < 0) return;
  if (o->m->nomit == o->cap && !(o->m->omit = realloc(o->m->omit, sizeof *o->m->omit * (size_t)(o->cap = o->cap ? o->cap * 2 : 16)))) cplan_oom();
  if (!(o->m->omit[o->m->nomit].name = strdup(name))) cplan_oom();
  CplanOmitCand *k = &o->m->omit[o->m->nomit++];
  /* (the facts of the name no call changes: the chain a top-level def's record ask runs on, the scope the tables hold
     it as, and whether main answers it as a builtin self method) */
  *k = (CplanOmitCand){ .name = k->name, .root = o->root, .chain = o->root < 0 ? ct_top_chain(c, name, 0) : -1, .mi = mi,
                        .self_main = builtin_self_method_known(name, BSELF_MAIN) };
}
/* See call_plan.h. */
const CplanOmitCand *cplan_omit_candidates(Compiler *c, int *n) {
  struct CplanMemo *m = cplan_memo(c);
  int keyed = cplan_rtr_sync(c, m);
  if (!keyed || !m->omit_ok) {
    cplan_omit_free(m);
    OmitBuild o = { c, m, -1, 0 };
    for (int s = 0; s < c->nscopes; s++) {
      const Scope *ts = &c->scopes[s];
      if (ts->name && ts->def_node >= 0 && ts->class_id < 0 && !ts->is_cmethod && !ts->is_proc_form) omit_note(&o, ts->name);
    }
    static const int roots[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
    for (int u = 0; roots[u] >= 0; u++) {
      o.root = comp_core_class(c, roots[u]);
      if (o.root >= 0) cplan_own_names(c, o.root, omit_note, &o);
    }
    m->omit_ok = (unsigned char)keyed;
  }
  *n = m->nomit;
  return m->omit;
}

/* A candidate of a list: nm asked of cls on the list's side, with no site
   (the final tables' answer, cplan_table_presence), and handed on where the question says cls
   holds it, with the visibility it gives. */
static void list_ask(Compiler *c, int cls, unsigned side, const char *nm, CplanCandFn fn, void *ud) {
  CplanPresence pr;
  cplan_table_presence(c, cls, side, nm, &pr);
  if (pr.table) fn(ud, nm, pr.vis);
}
/* The instance methods class ci holds: its defs, aliases and attribute
   accessors, those copied in from an included module and an alias an
   include made besides unless CT_OWN. */
static void list_instance_candidates(Compiler *c, int ci, unsigned side, CplanCandFn fn, void *ud) {
  ClassInfo *k = &c->classes[ci];
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id != ci || s->is_cmethod || !s->name || !s->name[0]) continue;
    /* a prepended module's method displaced the class's own body, which
       carries a shadow name: the name it is listed under is the user's,
       and a renamed module copy (a second prepend over the first) is none */
    if (s->shadow == SHADOW_PREP && s->origin_module_ci > 0) continue;
    list_ask(c, ci, side, s->shadow == SHADOW_PREP ? s->shadow_user : s->name, fn, ud);
  }
  for (int i = 0; i < k->naliases; i++)
    if (k->alias_new[i]) list_ask(c, ci, side, k->alias_new[i], fn, ud);
  /* accessors are flattened into descendants, so a class lists the ones it
     introduces */
  for (int i = 0; i < k->nreaders; i++)
    if (pr_accessor_own(c, ci, k->readers[i], 0, 1)) list_ask(c, ci, side, k->readers[i], fn, ud);
  for (int i = 0; i < k->nwriters; i++) {
    char wn[256]; snprintf(wn, sizeof wn, "%s=", k->writers[i]);
    if (pr_accessor_own(c, ci, k->writers[i], 1, 1)) list_ask(c, ci, side, wn, fn, ud);
  }
}
/* The class methods cls defines (defs, `class << self` aliases and
   accessors), then, unless CT_OWN, those of its superclasses and extended
   modules. */
static void list_cmethod_candidates(Compiler *c, int cls, unsigned side, CplanCandFn fn, void *ud) {
  for (int k = cls, g = 0; k >= 0 && g <= c->nclasses; k = c->classes[k].parent, g++) {
    ClassInfo *kc = &c->classes[k];
    for (int si = 0; si < c->nscopes; si++) {
      Scope *s = &c->scopes[si];
      if (s->class_id == k && s->is_cmethod && s->name && s->name[0]) list_ask(c, cls, side, s->name, fn, ud);
    }
    for (int i = 0; i < kc->naliases; i++)
      if (kc->alias_new[i]) list_ask(c, cls, side, kc->alias_new[i], fn, ud);
    for (int i = 0; i < kc->nsg_readers; i++) list_ask(c, cls, side, kc->sg_readers[i], fn, ud);
    for (int i = 0; i < kc->nsg_writers; i++) {
      char wn[256]; snprintf(wn, sizeof wn, "%s=", kc->sg_writers[i]);
      list_ask(c, cls, side, wn, fn, ud);
    }
    if (side & CT_OWN) break;
  }
}
/* See call_plan.h. */
void cplan_list_candidates(Compiler *c, int cls, unsigned side, CplanCandFn fn, void *ud) {
  mtab_enter(c);
  if (side & CT_CMETHOD) list_cmethod_candidates(c, cls, side, fn, ud);
  else list_instance_candidates(c, cls, side, fn, ud);
  mtab_leave(c);
}
/* ---- a module's instance-method list ----
   A module's list is its own definitions and those of the modules it includes, each name as the nearest of them that
   answers for it has it. The call plan owns the model (it reads the module's undef and visibility tables once, here):
   cplan_module_listable says whether the lookup order is plain enough to list ahead of time, cplan_module_methods
   enumerates the names with the answer's visibility. */
/* The visibility `ci` gives its own method `m`: its own declaration, else
   the including module's for a copied-in method, else public. */
static int module_own_vis(Compiler *c, int ci, int si, const char *m) {
  ClassInfo *k = &c->classes[ci];
  for (int i = 0; i < k->nvis; i++)
    if (sp_streq(k->vis_names[i], m)) return k->vis_kinds[i];
  int om = si >= 0 ? c->scopes[si].origin_module_ci - 1 : -1;
  if (om >= 0 && om < c->nclasses) return comp_method_vis(&c->classes[om], m);
  return SP_VIS_PUBLIC;
}

/* Does module m itself define nm as an instance method: a def of its own (a
   module_function's private instance side too), or an accessor or alias no
   module it includes carries as well (an include copies those into the
   includer's lists, as it copies defs into its table)? *vis is the
   visibility it gives, -1 for a def (the caller reads the def's). */
static int module_carries(Compiler *c, int m, const char *nm) {
  ClassInfo *kc = &c->classes[m];
  char base[256];
  if (setter_base_name(nm, base, sizeof base)) {
    if (comp_is_writer(kc, base)) return 1;
  }
  else if (comp_is_reader(kc, nm)) return 1;
  for (int i = 0; i < kc->naliases; i++) if (kc->alias_new[i] && sp_streq(kc->alias_new[i], nm)) return 1;
  return 0;
}
static int module_defines(Compiler *c, int m, const char *nm, int *vis) {
  int si = comp_method_in_class(c, m, nm);
  if (si >= 0 && c->scopes[si].origin_module_ci == 0) {
    *vis = c->scopes[si].is_module_function || c->scopes[si].is_cmethod ? SP_VIS_PRIVATE : -1;
    return 1;
  }
  for (int k = 0; k < c->nscopes; k++) {
    Scope *s = &c->scopes[k];
    if (s->class_id == m && s->is_module_function && s->name && sp_streq(s->name, nm) &&
        s->origin_module_ci == 0) { *vis = SP_VIS_PRIVATE; return 1; }
  }
  if (!module_carries(c, m, nm)) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (k != m && comp_mixes_in(c, m, k, 0) && module_carries(c, k, nm)) return 0;
  *vis = comp_method_vis(&c->classes[m], nm);
  return 1;
}

/* Mod.instance_methods / public_instance_methods with the inherited ones:
   listable ahead of time when the lookup order is plain. The module and the
   modules it includes (each reached through one include only) are defined
   by `module` bodies (not Module.new) and mix in nothing but each other,
   through an include that is a statement of the body itself -- not one in a
   conditional, a begin or a method, not a builtin (Comparable), not a
   prepend -- and no two of them that are not one another's ancestor
   define, undefine or give a visibility to the same name, nor does one
   both define and undefine it. Then each name's answer is the nearest
   module's whatever order sibling includes take. The module has no
   class-level method of `name` of its own or from a module it extends. */
static int module_tree(Compiler *c, int m, int *mods, int *n, int cap) {
  const NodeTable *nt = c->nt;
  if (m < 0 || m >= c->nclasses || *n >= cap) return 0;
  for (int j = 0; j < *n; j++) if (mods[j] == m) return 0;   /* reached twice */
  ClassInfo *mc = &c->classes[m];
  if (!comp_class_is_module(c, mc) || mc->is_native_class || mc->nincluded_mod_names > 0) return 0;
  mods[(*n)++] = m;
  int nb = 0;
  const LrOpen *ob = elab_class_bodies(c, m, &nb);
  for (int b = 0; b < nb; b++) {
    int sn = 0;
    const int *stmts = ob[b].body >= 0 ? nt_arr(nt, ob[b].body, "body", &sn) : NULL;
    for (int k = 0; k < sn; k++) {
      int prep = 0; const int *aa;
      int an = mixin_stmt_args(nt, stmts[k], &prep, &aa);
      if (an < 0) continue;
      if (prep || an == 0) return 0;
      for (int j = 0; j < an; j++) {
        NodeKind ak = nt_kind(nt, aa[j]);
        int im = ak == NK_ConstantReadNode || ak == NK_ConstantPathNode ? class_recv_static_ci(c, aa[j]) : -1;
        if (im < 0 || !module_tree(c, im, mods, n, cap)) return 0;
      }
    }
  }
  return 1;
}
static int module_answers(Compiler *c, int m, const char *nm, int *undef) {
  ClassInfo *kc = &c->classes[m];
  int v;
  int def = module_defines(c, m, nm, &v);
  *undef = 0;
  for (int u = 0; u < kc->nundefs; u++) if (sp_streq(kc->undefs[u], nm)) *undef = 1;
  if (def || *undef) return 1;
  for (int i = 0; i < kc->nvis; i++) if (sp_streq(kc->vis_names[i], nm)) return 1;
  return 0;
}
/* Is id a statement of one of class ci's bodies' own statement lists? */
static int module_body_stmt(Compiler *c, int ci, int id) {
  int nb = 0;
  const LrOpen *ob = elab_class_bodies(c, ci, &nb);
  for (int b = 0; b < nb; b++) {
    int sn = 0;
    const int *stmts = ob[b].body >= 0 ? nt_arr(c->nt, ob[b].body, "body", &sn) : NULL;
    for (int k = 0; k < sn; k++) if (stmts[k] == id) return 1;
  }
  return 0;
}

static int module_listable(Compiler *c, int m, const char *name) {
  const NodeTable *nt = c->nt;
  if (m < 0 || m >= c->nclasses) return 0;
  ClassInfo *mc = &c->classes[m];
  if (comp_cmethod_in_chain(c, m, name, NULL) >= 0) return 0;
  for (int e = 0; e < mc->nextended_mods; e++)
    if (comp_method_in_class(c, mc->extended_mods[e], name) >= 0) return 0;
  int mods[64], n = 0;
  int ok = module_tree(c, m, mods, &n, 64);
  /* an include or prepend anywhere but a body's own statement list */
  for (int id = 0; ok && id < nt->count; id++) {
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !(is_include_name(nm) || is_prepend_name(nm))) continue;
    int rcv = nt_ref(nt, id, "receiver");
    if (rcv >= 0 && nt_kind(nt, rcv) != NK_SelfNode) continue;   /* refused on its own */
    int owner = c->node_cbody && id < c->node_cap ? c->node_cbody[id] : -1;
    if (owner < 0) { Scope *sc = comp_scope_of(c, id); owner = sc ? sc->class_id : -1; }
    int in_mods = 0;
    for (int j = 0; j < n; j++) if (mods[j] == owner) in_mods = 1;
    if (!in_mods || (rcv < 0 && module_body_stmt(c, owner, id))) continue;
    ok = 0;
  }
  if (!ok) return 0;
  /* no two unrelated modules answer for one name */
  for (int j = 0; j < n; j++) {
    ClassInfo *kc = &c->classes[mods[j]];
    int nnames = 0;
    const char *names[1024];
    for (int si = 0; si < c->nscopes && nnames < 1024; si++)
      if (c->scopes[si].class_id == mods[j] && c->scopes[si].name) names[nnames++] = c->scopes[si].name;
    for (int i = 0; i < kc->nreaders && nnames < 1024; i++) names[nnames++] = kc->readers[i];
    for (int i = 0; i < kc->naliases && nnames < 1024; i++) if (kc->alias_new[i]) names[nnames++] = kc->alias_new[i];
    for (int i = 0; i < kc->nvis && nnames < 1024; i++) names[nnames++] = kc->vis_names[i];
    for (int i = 0; i < kc->nundefs && nnames < 1024; i++) names[nnames++] = kc->undefs[i];
    if (nnames >= 1024) return 0;
    for (int q = 0; q < nnames; q++) {
      int ua, ub;
      if (!module_answers(c, mods[j], names[q], &ua)) continue;
      int va;
      if (ua && module_defines(c, mods[j], names[q], &va)) return 0;
      for (int j2 = j + 1; j2 < n; j2++)
        if (module_answers(c, mods[j2], names[q], &ub) &&
            !comp_mixes_in(c, mods[j], mods[j2], 0) && !comp_mixes_in(c, mods[j2], mods[j], 0))
          return 0;
    }
    /* a writer's name is its reader's plus `=` */
    for (int i = 0; i < kc->nwriters; i++) {
      char wn[256]; snprintf(wn, sizeof wn, "%s=", kc->writers[i]);
      int ua, ub;
      if (!module_answers(c, mods[j], wn, &ua)) continue;
      for (int j2 = j + 1; j2 < n; j2++)
        if (module_answers(c, mods[j2], wn, &ub) &&
            !comp_mixes_in(c, mods[j], mods[j2], 0) && !comp_mixes_in(c, mods[j2], mods[j], 0))
          return 0;
    }
  }
  return 1;
}

/* Mod.instance_methods / public_instance_methods with the inherited ones
   (no argument, or `true`): what the module and the modules it includes
   define, each name as the nearest of them that answers for it has it --
   defined (module_defines), given a visibility, or undefined. Only for a
   module cplan_module_listable passes, whose includes form a tree in
   which no two unrelated modules answer for one name, so an includer comes
   before what it includes and sibling order does not matter. */
static void module_order(Compiler *c, int m, int *order, int *n, int cap) {
  for (int j = 0; j < *n; j++) if (order[j] == m) return;
  if (*n >= cap || m < 0 || m >= c->nclasses) return;
  order[(*n)++] = m;
  ClassInfo *kc = &c->classes[m];
  for (int i = kc->nincluded_mods - 1; i >= 0; i--) module_order(c, kc->included_mods[i], order, n, cap);
}
/* 1 when module m itself answers for nm (its own definition, or a
   visibility line for one a module it includes defines), -1 when it
   undefines it, 0 when it leaves it to the modules it includes. *vis is the
   visibility it gives. The visibility table holds class-level methods'
   lines too, so a line counts only for an instance method below it. */
static int module_own(Compiler *c, int m, const int *order, int norder, int at,
                           const char *nm, int *vis) {
  ClassInfo *kc = &c->classes[m];
  for (int u = 0; u < kc->nundefs; u++) if (sp_streq(kc->undefs[u], nm)) return -1;
  if (module_defines(c, m, nm, vis)) {
    if (*vis < 0) *vis = module_own_vis(c, m, comp_method_in_class(c, m, nm), nm);
    return 1;
  }
  for (int i = 0; i < kc->nvis; i++) {
    if (!sp_streq(kc->vis_names[i], nm)) continue;
    for (int j = at + 1; j < norder; j++) {
      int v2 = 0;
      if (comp_mixes_in(c, m, order[j], 0) && module_defines(c, order[j], nm, &v2))
        { *vis = kc->vis_kinds[i]; return 1; }
    }
    break;
  }
  return 0;
}
/* A name the module's lists may hold, once (its first sighting keeps the order) */
typedef struct { Compiler *c; char **v; int n, cap; } ModNames;
static void module_name_add(ModNames *a, const char *nm) {
  if (name_is_synth_method(a->c, nm)) return;   /* (the compiler's synthesized helpers are no method of the module's: candidates and visibility lines alike) */
  for (int i = 0; i < a->n; i++) if (sp_streq(a->v[i], nm)) return;
  if (a->n >= a->cap) {
    a->cap = a->cap ? a->cap * 2 : 32;
    if (!(a->v = realloc(a->v, sizeof(char *) * (size_t)a->cap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  a->v[a->n++] = strdup(nm);
}
/* See call_plan.h. */
int cplan_module_listable(Compiler *c, int m, const char *name) {
  mtab_enter(c);   /* (the question's own reads are the plan's) */
  int r = module_listable(c, m, name);
  mtab_leave(c);
  return r;
}
static void module_candidate(void *ud, const char *nm, int vis) {
  (void)vis;
  module_name_add(ud, nm);
}
/* See call_plan.h. */
void cplan_module_methods(Compiler *c, int m, int own, CplanCandFn fn, void *ud) {
  mtab_enter(c);
  int order[256], n = 0;
  ModNames all = { c, NULL, 0, 0 };
  module_order(c, m, order, &n, 256);
  /* every name any of them mentions, then each one's answer */
  for (int j = 0; j < n; j++) {
    ClassInfo *kc = &c->classes[order[j]];
    cplan_list_candidates(c, order[j], 0, module_candidate, &all);
    for (int i = 0; i < kc->nvis; i++) module_name_add(&all, kc->vis_names[i]);
  }
  for (int i = 0; i < all.n; i++) {
    const char *nm = all.v[i];
    int vis = SP_VIS_PUBLIC;
    if (own) {
      if (module_own(c, m, order, n, 0, nm, &vis) > 0) fn(ud, nm, vis);
      continue;
    }
    for (int j = 0; j < n; j++) {
      vis = SP_VIS_PUBLIC;
      int a = module_own(c, order[j], order, n, j, nm, &vis);
      if (a == 0) continue;
      fn(ud, nm, a > 0 ? vis : -1);   /* (-1: undefined, which hides the name) */
      break;
    }
  }
  for (int i = 0; i < all.n; i++) free(all.v[i]);
  free(all.v);
  mtab_leave(c);
}


int cplan_class_row(Compiler *c, int id, int recv, const char *name) {
  const NodeTable *nt = c->nt;
  int ci = class_recv_static_ci(c, recv);
  if (ci >= 0 && call_target(c, id, ci, name, CT_CMETHOD, NULL) >= 0) return 0;
  /* a Class value known only at run time may be one that defines the method
     (`def self.include?`): the class-value dispatch answers it, its own
     method where it has one and the row elsewhere. Not a receiver an
     enclosing dispatch bound: its arms took the classes that define one. */
  if (ci < 0 && recv >= 0) {
    int bound = 0;
    for (int i = 0; i < g_n_argov && !bound; i++) bound = g_argov_node[i] == recv;
    for (int k = 0; !bound && k < c->nclasses; k++)
      if (call_target(c, id, k, name, CT_CMETHOD, NULL) >= 0) return 0;
  }
  int fl = bop_flags(TY_CLASS, name);
  if ((fl & BOPF_CLASS_ONLY) && !cplan_recv_class(c, id, recv)) return 0;
  return !((fl & BOPF_PRIVATE) && recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && !nt_ignore_visibility(nt, id) && !nt_str(nt, id, "send_blind"));
}

CvCtor cplan_cv_ctor_shape(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if ((!c->has_arysub && !c->has_record) || nt_kind(nt, id) != NK_CallNode) return CVC_NONE;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), argc = 0;
  const int *argv = call_args(nt, id, &argc);
  if (!nm || recv < 0 || !is_record_class_cmethod_with_args(nm) ||
      nt_argv_has(nt, argv, argc, NT_ARG_SPLAT | NT_ARG_BLOCK | NT_ARG_KEYWORDS)) return CVC_NONE;
  int aref = !is_new_name(nm);
  if (aref ? nt_ref(nt, id, "block") >= 0 : argc == 0) return CVC_NONE;
  if (aref) {
    int ncc = 0;
    comp_cmethod_candidates(c, nm, &ncc);
    if (ncc) return CVC_NONE;
  }
  if (!class_recv_may_be_dynamic(c, recv) || class_var_static_ci(c, recv) >= 0) return CVC_NONE;
  return aref ? CVC_AREF : CVC_NEW;
}

CvCtor cplan_cv_ctor(Compiler *c, int id) {
  if ((!c->has_arysub && !c->has_record) || nt_kind(c->nt, id) != NK_CallNode) return CVC_NONE;
  int recv = nt_ref(c->nt, id, "receiver");
  /* a `[]` on any other type than a Class is an index: the cheap answer for
     most `[]`, ahead of the shape's receiver walks */
  if (recv < 0 || (is_aref_name(nt_str(c->nt, id, "name")) && infer_type(c, recv) != TY_CLASS)) return CVC_NONE;
  CvCtor f = cplan_cv_ctor_shape(c, id);
  if (f == CVC_NONE) return f;
  /* a constant holds a class value where its slot is typed one
     (class_recv_is_dynamic) */
  if (!class_recv_is_dynamic(c, recv)) return CVC_NONE;
  return f;
}

int cplan_cv_ctor_class(Compiler *c, int id, CvCtor f, int k) {
  if (f == CVC_NONE || comp_ary_root(c, k) < 0 || !comp_class_builds_by_aref(c, k))
    return 0;
  return f != CVC_NEW || (cplan_hook_target(c, id, k, "initialize", NULL) < 0 &&
                          call_target(c, id, k, "new", CT_CMETHOD, NULL) < 0);
}

int cplan_cv_ctor_builds(Compiler *c, int id, int k) {
  return cplan_cv_ctor_class(c, id, cplan_cv_ctor(c, id), k) && poly_cls_value_reaches(c, id, k);
}

int cplan_as_new(Compiler *c, int id, int k) {
  if (nt_kind(c->nt, id) != NK_CallNode) return 0;
  const char *nm = nt_str(c->nt, id, "name");
  if (is_new_name(nm)) return 1;
  if (!is_aref_name(nm) || (k < 0 ? !c->has_record : k >= c->nclasses || !c->classes[k].is_struct)) return 0;
  return cplan_cv_ctor(c, id) == CVC_AREF;
}

int cplan_own_class_reflection(Compiler *c, int id, int recv, const char *name) {
  static const char *const R[] = { "superclass", "ancestors", "subclasses", "included_modules",
    "instance_methods", "public_instance_methods", "private_instance_methods",
    "protected_instance_methods", NULL };
  int r = 0;
  for (int i = 0; name && R[i]; i++) if (sp_streq(name, R[i])) { r = 1; break; }
  int ci = r && recv >= 0 ? class_recv_static_ci(c, recv) : -1;
  MethodLookup ml = { .def_class = -1 };
  int mi = ci >= 0 ? call_target(c, id, ci, name, CT_CMETHOD, &ml) : -1, dc = ml.def_class;
  if (mi < 0) return 0;
  if (!scope_calls_super(c, mi)) return 1;
  /* (emit_super answers a bare super of these three) */
  if (sp_streq(name, "superclass") || sp_streq(name, "ancestors") || sp_streq(name, "subclasses")) return 1;
  /* a super that reaches a class method of the program above it, whose
     own super (if any) does so too: the last one in the chain calls no
     super. A class on the chain that holds more than one version of the
     method (a reopen replaced it) takes part only when none of them calls
     super; otherwise the old path stays */
  ElabVersion vs[8];
  for (int depth = 0; depth < 64 && dc >= 0; depth++) {
    if (elab_history_versions(c, dc, 1, name, vs, 8) != 1) return 0;
    int p = comp_super_parent(c, dc, 1), pdc = -1;
    mtab_enter(c);   /* (the plan's own walk: the class method each super of the chain reaches, above the one the ask at id found) */
    int pm = p >= 0 ? ct_table(c, p, name, CT_CMETHOD, &pdc, NULL) : -1;
    mtab_leave(c);
    if (pm < 0 || pdc < 0) return 0;
    int nv = elab_history_versions(c, pdc, 1, name, vs, 8);
    if (nv < 1 || nv > 8) return 0;
    if (nv > 1) {
      for (int i = 0; i < nv; i++)
        if (vs[i].v->scope <= 0 || scope_calls_super(c, vs[i].v->scope)) return 0;
      return 1;
    }
    if (!scope_calls_super(c, pm)) return 1;
    dc = pdc;
  }
  return 0;
}

int cplan_recv_class(Compiler *c, int id, int recv) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, recv);
  const char *nm = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? nt_str(nt, recv, "name") : NULL;
  int ci = k == NK_SelfNode ? self_class_static_ci(c, recv) : nm ? class_recv_static_ci(c, recv) : -1;
  if (nt_int(nt, id, "class_recv", 0)) return 1;
  if (nm && is_builtin_module_const_name(nm)) return 0;   /* (a core module has an entry that is no module's) */
  if (ci >= 0) return !comp_class_is_module(c, &c->classes[ci]);
  if (nm) return is_builtin_class_name(nm);
  return k == NK_CallNode && nt_ref(nt, recv, "receiver") >= 0 && nt_kind(nt, nt_ref(nt, recv, "receiver")) == NK_ConstantReadNode &&
         is_class_new(nt_str(nt, nt_ref(nt, recv, "receiver"), "name"), nt_str(nt, recv, "name"));
}

/* Is a call of nm one whose version call_target chooses: does the record
   hold more than one version of the name, or an alias, which names the
   version it captured? */
int ct_version_named(Compiler *c, const char *nm) {
  return nm && (elab_method_replaced(c, nm) || elab_method_versions(c, nm) > 1 || elab_method_late(c, nm));
}
/* ...and does a call of nm bind a method only its call's marks keep
   (cr_collect_versions): a replaced version, which the table does not
   answer, or an entry made for nm? Other versions are reached by name. */
int ct_version_marked(Compiler *c, const char *nm) {
  if (!nm) return 0;
  if (elab_method_replaced(c, nm)) return 1;
  if (!c->ver_ix_nb) return 0;
  for (int i = c->ver_names[sp_strhash(nm) % c->ver_ix_nb]; i >= 0; i = c->ver_names_next[i])
    if (sp_streq(c->ver_entries[i].name, nm)) return 1;
  return 0;
}
/* The classes call node id of versioned name nm may run on, each handed to
   fn with the call_target flags it is asked with: main and self's class (a
   method's, or the class body's) and its descendants (either side) for a
   receiverless or `self.` call, and in an instance_eval or instance_exec
   block the class or classes self is rebound to and their descendants
   (the plan's UC_IE target); for a constant receiver, the side of the
   program class the record says it names (elab_class_ref_at; Object's, or
   one it does not know, as any other receiver); an object's class and its
   descendants; for any other receiver, main and
   each class the load record holds a history of the name for
   (elab_method_holder), on that side (a class that holds none answers as
   its nearest ancestor that does), and the class of each later prepend's
   copy of the name, on its side, whose guard entry the call binds.
   Each of the others also on Object's instance side: every lookup but
   BasicObject's ends in Object's methods (Kernel's among them), which the
   tables leave out of a program class's chain and of its class side, so
   the plan binds them by asking Object (cplan_root_reopen). Reachability
   marks the versions the call binds on each (cr_collect_versions);
   ver_entries_build makes the version and guard entries they need. */
static void ct_class_and_descendants(Compiler *c, int id, const char *nm, int k, unsigned flags, CtClassFn fn, void *arg) {
  int nd = 0;
  const int *ds = k >= 0 ? comp_descendants(c, k, &nd) : NULL;
  for (int i = -1; i < nd; i++) fn(c, id, nm, i < 0 ? k : ds[i], flags, arg);
}
/* The classes (and sides) that hold a history of nm and whose chain of it
   has two or more versions: of the others no entry is made and no version
   is marked (ver_need, cr_mark_version), so a call whose receiver is not
   known is asked of these alone. Kept by name for the record as it stands
   (the node table's version): a program of thousands of classes that each
   define a method of one name has a call of it for every class, and each
   asked every class. */
struct VerHold { ANameHash names; char **key; int *lo, *n, *pool; int npool, cpool; ElabStamp stamp; };
void ver_hold_free(Compiler *c) {
  struct VerHold *m = c->ver_hold;
  if (!m) return;
  for (int i = 0; i < m->names.n; i++) free(m->key[i]);
  anh_free(&m->names);
  free(m->key); free(m->lo); free(m->n); free(m->pool); free(m);
  c->ver_hold = NULL;
}
static void ver_hold_add(struct VerHold *m, int k, int sg) {
  if (m->npool + 2 > m->cpool && !(m->pool = realloc(m->pool, sizeof(int) * (size_t)(m->cpool = m->cpool ? 2 * m->cpool : 64)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  m->pool[m->npool++] = k;
  m->pool[m->npool++] = sg;
}
/* ...and of a module's: the classes that include or prepend it, or extend
   it (on their class side), which hold copies of its versions
   (analyze_scope.c, mixin_prev_version) and none of the name's histories */
static void ver_hold_mixers(Compiler *c, struct VerHold *m, int mod, const char *nm) {
  ElabVersion vs[VER_MAX_ARMS];
  for (int j = 0; j < c->nclasses; j++) {
    const ClassInfo *k = &c->classes[j];
    int in = 0, ext = 0;
    for (int i = 0; i < k->nincluded_mods && !in; i++) in = k->included_mods[i] == mod;
    for (int i = 0; i < k->nprepended_mods && !in; i++) in = k->prepended_mods[i] == mod;
    for (int i = 0; i < k->nextended_mods && !ext; i++) ext = k->extended_mods[i] == mod;
    if (comp_class_is_module(c, k)) continue;
    if (in && elab_chain_versions(c, j, 0, nm, vs, VER_MAX_ARMS) >= 2) ver_hold_add(m, j, 0);
    if (ext && elab_chain_versions(c, j, 1, nm, vs, VER_MAX_ARMS) >= 2) ver_hold_add(m, j, 1);
  }
}
/* ...and of a `class Class` reopening's (the module Class__reopen): every
   class's class side, whose chain ends in it (lr_sg_tail_of), as an
   extend's would. Two reopenings that define a name alike (a file
   guarded by `unless $loaded` beside its first load) made an entry for
   a class's side that no call marked, and the boxed dispatch dropped
   that class's arm: `k.descendants` raised NoMethodError. */
static void ver_hold_class_reopen(Compiler *c, struct VerHold *m, const char *nm) {
  ElabVersion vs[VER_MAX_ARMS];
  for (int j = 0; j < c->nclasses; j++)
    if (!comp_class_is_module(c, &c->classes[j]) && elab_chain_versions(c, j, 1, nm, vs, VER_MAX_ARMS) >= 2)
      ver_hold_add(m, j, 1);
}
static const int *ver_holders(Compiler *c, const char *nm, int *n) {
  ElabVersion vs[VER_MAX_ARMS];
  struct VerHold *m = c->ver_hold;
  if (m && !elab_stamp_same(m->stamp, elab_stamp(c))) { ver_hold_free(c); m = NULL; }
  if (!m) {
    if (!(c->ver_hold = m = calloc(1, sizeof *m))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    m->stamp = elab_stamp(c);
  }
  int ix = anh_find(&m->names, nm);
  if (ix < 0) {
    int lo = m->npool, k, sg;
    for (int h = -1; (h = elab_method_holder(c, nm, h, &k, &sg)) >= 0;) {
      int nv = k >= 0 ? elab_chain_versions(c, k, sg, nm, vs, VER_MAX_ARMS) : 0;
      /* (or one whose visibility only the run tells, for its face entry: ver_vis_runs) */
      if (k < 0 || (nv < 2 && !(nv == 1 && (ver_vis_runs(c, k, sg, nm) || ver_lone_closed_late(c, k, sg, nm, VF_PUBLIC))))) continue;
      ver_hold_add(m, k, sg);
      if (!sg && comp_class_is_module(c, &c->classes[k])) ver_hold_mixers(c, m, k, nm);
      if (!sg && k == comp_core_class(c, CORE_CLASS_REOPEN)) ver_hold_class_reopen(c, m, nm);
    }
    ix = m->names.n;
    if (!(m->key = realloc(m->key, sizeof *m->key * (size_t)(ix + 1))) || !(m->lo = realloc(m->lo, sizeof(int) * (size_t)(ix + 1))) ||
        !(m->n = realloc(m->n, sizeof(int) * (size_t)(ix + 1))) || !(m->key[ix] = strdup(nm))) {
      fprintf(stderr, "spinel: out of memory\n"); exit(1);
    }
    anh_add(&m->names, m->key[ix]);
    m->lo[ix] = lo;
    m->n[ix] = (m->npool - lo) / 2;
  }
  *n = m->n[ix];
  return m->pool + m->lo[ix];
}
/* What receives a call node id makes: the value of a node (an explicit
   call's receiver, the operand a syntax row of implied_calls.yml names),
   self, or a value no node holds (one the node yields or matches), whose
   class is not known. The one resolver of where a call lands
   (ct_landing_classes): a builtin's callback rows join it when they say
   where theirs land (an argument, an element of one, a block's answer). */
typedef enum { CTL_VALUE, CTL_SELF, CTL_ANY } CtLandKind;
/* except: the CORE_ slot of a conversion's own class (IcCall.target), whose
   values (an instance of a subclass too) it leaves as they are; -1 for none */
typedef struct { CtLandKind kind; int node, except; } CtLanding;
/* The classes (and sides) a call of nm that node id makes may run on, landing
   on at (self too where at's node is a SelfNode). Each handed to fn with the
   flags call_target is asked with (ct_version_classes, ct_implied_classes). */
static void ct_landing_classes(Compiler *c, int id, CtLanding at, const char *nm, CtClassFn fn, void *arg) {
  const NodeTable *nt = c->nt;
  int r = at.kind == CTL_VALUE ? at.node : -1;
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  int cref = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode, rc = cref ? elab_class_ref_at(c, r, NULL) : -1;
  /* a constant the record cannot settle to one class (one a run-time
     condition defines, or binds to either of two) names one of the classes
     its writes may bind there, when the record can list them */
  int *rcs = NULL, nrc = rk == NK_ConstantReadNode && rc < 0 ? elab_class_candidates(c, r, &rcs) : -1;
  TyKind rt = r >= 0 && rk != NK_SelfNode && !cref ? comp_ntype(c, r) : TY_UNKNOWN;
  /* a value of the conversion's own class lands nowhere: CRuby makes no call
     (rb_check_array_type answers an Array as it is) */
  int ek = at.except >= 0 ? comp_core_class(c, at.except) : -1;
  if (at.except >= 0 && (core_of_type(rt) == at.except || (ty_is_object(rt) && ek >= 0 && is_descendant(c, ty_object_class(rt), ek)))) {
    free(rcs);
    return;
  }
  int oc = comp_core_class(c, CORE_OBJECT);
  if (at.kind == CTL_SELF || rk == NK_SelfNode) {
    Scope *sc = comp_scope_of(c, id);
    int self_cls = sc && sc->class_id >= 0 ? sc->class_id : c->node_cbody[id];
    ct_class_and_descendants(c, id, nm, -1, CT_TOP, fn, arg);
    /* (and what a module the top level includes brings, where the name has versions in Object's chain: the version
       the call binds, which the included-method ask answers) */
    if (rk != NK_SelfNode && c->ver_built) fn(c, id, nm, oc, CT_INCLUDED, arg);
    if (self_cls >= 0) {
      ct_class_and_descendants(c, id, nm, self_cls, 0, fn, arg);
      ct_class_and_descendants(c, id, nm, self_cls, CT_CMETHOD, fn, arg);
    }
    int iec = ie_class_of(c, id), pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    if (iec >= 0) ct_class_and_descendants(c, id, nm, iec, 0, fn, arg);
    for (int j = 0; j < npk; j++) ct_class_and_descendants(c, id, nm, pk[j], 0, fn, arg);
  }
  else if (rc >= 0)
    ct_class_and_descendants(c, id, nm, rc, is_instance_method_name(nt_str(nt, id, "name")) ? CT_CAPTURE : CT_CMETHOD, fn, arg);
  else if (nrc > 0) {
    for (int i = 0; i < nrc; i++) ct_class_and_descendants(c, id, nm, rcs[i], CT_CMETHOD, fn, arg);
  }
  else if (ty_is_object(rt))
    ct_class_and_descendants(c, id, nm, ty_object_class(rt), 0, fn, arg);
  else {
    int nh = 0;
    const int *hold = ver_holders(c, nm, &nh);
    fn(c, id, nm, -1, CT_TOP, arg);
    for (int i = 0; i < nh; i++) fn(c, id, nm, hold[2 * i], hold[2 * i + 1] ? CT_CMETHOD : 0, arg);
    /* a name a call may run ahead of (elab_method_late) has its entries made
       per class the call meets (ver_absent_note), a holder's descendant or
       includer among them: each class its boxed dispatch takes an arm for,
       on either side, as the arms ask (cplan_cand_target). So has a name
       whose window spans two versions on a class that holds none of them
       (a subclass that inherits a method its parent redefines later): the
       span's entry for that class. Asked once the entries' build is over,
       when those are noted (and the candidates' lists are kept, the scope
       index frozen); the build's own asks need the holders alone */
    if (c->ver_built && ct_version_named(c, nm)) {
      int npc = 0;
      const PolyCand *pcs = comp_poly_candidates_at(c, id, nm, &npc);
      for (int i = 0; i < npc; i++) fn(c, id, nm, pcs[i].cls, 0, arg);
      pcs = comp_cmethod_candidates(c, nm, &npc);
      for (int i = 0; i < npc; i++) fn(c, id, nm, pcs[i].cls, CT_CMETHOD, arg);
    }
    int obj = oc;
    oc = -1;   /* (Object among them, where its chain holds two) */
    for (int i = c->ver_ix_nb ? c->ver_names[sp_strhash(nm) % c->ver_ix_nb] : -1;
         i >= 0; i = c->ver_names_next[i]) {   /* and the class of each later prepend's copy */
      const struct VerEntry *e = &c->ver_entries[i];
      if (e->guard != VER_ENTRY_CHAIN && sp_streq(e->name, nm)) fn(c, id, nm, e->cls, e->sg ? CT_CMETHOD : 0, arg);
      /* (a value no class of the program holds meets Object's chain entry: a module included into Object, or a top-level def, beside versions) */
      else if (e->guard == VER_ENTRY_CHAIN && !e->sg && e->cls == obj && sp_streq(e->name, nm)) fn(c, id, nm, obj, 0, arg);
    }
  }
  if (oc >= 0) fn(c, id, nm, oc, 0, arg);
  free(rcs);
}
void ct_version_classes(Compiler *c, int id, const char *nm, CtClassFn fn, void *arg) {
  NodeKind kind = nt_kind(c->nt, id);
  if (kind == NK_SuperNode || kind == NK_ForwardingSuperNode) {
    Scope *s0 = comp_scope_of(c, id);
    const Scope *s = s0 ? comp_super_site(c, s0) : NULL;
    if (s && !comp_super_shadow(c, s)) {
      int par = comp_super_parent(c, s->class_id, s->is_cmethod);
      if (par >= 0) fn(c, id, nm, par, CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), arg);
    }
    return;
  }
  int r = nt_ref(c->nt, id, "receiver");
  ct_landing_classes(c, id, (CtLanding){ r >= 0 ? CTL_VALUE : CTL_SELF, r, -1 }, nm, fn, arg);
}
/* (an implied call is asked of an instance's class, the side
   cplan_implied_target answers, with CT_IMPLIED: the node does not name
   the method it calls) */
typedef struct { CtClassFn fn; void *arg; } CtImpliedAsk;
static void ct_implied_ask(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  const CtImpliedAsk *a = arg;
  if (k >= 0 && !(flags & (CT_CMETHOD | CT_TOP))) a->fn(c, id, nm, k, flags | CT_IMPLIED, a->arg);
}
void ct_implied_classes(Compiler *c, int id, int (*versioned)(Compiler *, const char *), CtClassFn fn, void *arg) {
  IcCall buf[16];
  int n = 0;
  IcCall *calls = call_node_implied_names(c, id, buf, 16, &n);
  CtImpliedAsk a = { fn, arg };
  for (int i = 0; i < n; i++)
    if (versioned(c, calls[i].name))
      ct_landing_classes(c, id, (CtLanding){ calls[i].operand >= 0 ? CTL_VALUE : calls[i].operand == NTI_SELF ? CTL_SELF : CTL_ANY,
                                             calls[i].operand, calls[i].target ? builtin_core_class_slot(calls[i].target) : -1 },
                         calls[i].name, ct_implied_ask, &a);
  if (calls != buf) free(calls);
  /* The receiver of the native Kernel.String callback arm is a Symbol,
     even at a call site whose operand can hold other values. The ordinary
     holder survey only knows histories with two or more definitions. */
  const BopCallback *callbacks;
  int nc = kernel_string_implied_calls(c, id, &callbacks);
  int symbol = comp_core_class(c, CORE_SYMBOL);
  for (int i = 0; i < nc; i++)
    if ((callbacks[i].roles & BCR_ARG) && versioned(c, callbacks[i].name))
      fn(c, id, callbacks[i].name, symbol, CT_IMPLIED, arg);
}

/* "class U" / "module M" for program class self, "main" for -1, into buf;
   NULL for an anonymous class, which names itself at run time */
static const char *cm_self_name(Compiler *c, int self, char *buf, size_t cap) {
  if (self < 0) { snprintf(buf, cap, "main"); return buf; }
  if (comp_class_anonymous(c, self)) return NULL;
  const char *cn = class_ruby_name(c, self);
  snprintf(buf, cap, "%s %s", comp_class_is_module(c, &c->classes[self]) ? "module" : "class", cn ? cn : c->classes[self].name);
  return buf;
}
/* See call_plan.h. */
int cplan_const_recv_class(Compiler *c, int id, int recv, int *core) {
  int framed = elab_plan_enter(c, id);
  int ci = elab_class_ref_at(c, recv, core);
  if (framed) elab_plan_leave(c);
  return ci;
}
/* See call_plan.h. */
int cplan_self_class(Compiler *c, int id) {
  int framed = elab_plan_enter(c, id);
  int ci = elab_self_at(c, id);
  if (framed) elab_plan_leave(c);
  return ci;
}
/* Does the keyword hash or Hash literal h spread a key: 1; 0 where its
   `**` operands are nil or literals that spread nothing; -1 where one is
   the run time's (an anonymous `**` too). */
static int spread_elements(const NodeTable *nt, int h) {
  int en = 0, sp = 0;
  const int *els = nt_arr(nt, h, "elements", &en);
  for (int e = 0; e < en && sp <= 0; e++) {
    if (nt_kind(nt, els[e]) != NK_AssocSplatNode) return 1;
    int v = nt_ref(nt, els[e], "value");
    NodeKind vk = v >= 0 ? nt_kind(nt, v) : NK_NONE;
    int one = vk == NK_NilNode ? 0 : vk == NK_HashNode ? spread_elements(nt, v) : -1;
    if (one != 0) sp = one;
  }
  return sp;
}
/* See call_plan.h. */
int cplan_hash_positionals(Compiler *c, int id, int kwpos) {
  int argc = 0;
  const int *argv = call_args(c->nt, id, &argc);
  if (argc == 0 || nt_kind(c->nt, argv[argc - 1]) != NK_KeywordHashNode) return 0;
  int h = argv[argc - 1];
  int sp = nt_int(c->nt, h, "kwsplit_full", 0) ? 1 : spread_elements(c->nt, h);
  if (sp == 0 || kwpos == 0) return 0;
  return kwpos == 1 ? sp : -1;
}
/* Is the key node k of a hash pair the Symbol `key`: 1; 0 where it is a
   literal of another value; -1 where its value is the run time's (a
   variable, a call, an interpolated Symbol). */
static int hash_key_is(const NodeTable *nt, int k, const char *key) {
  switch (nt_kind(nt, k)) {
    case NK_SymbolNode: {
      const char *kn = nt_str(nt, k, "value");
      return kn ? sp_streq(kn, key) : -1;
    }
    case NK_StringNode: case NK_IntegerNode: case NK_FloatNode: case NK_RationalNode:
    case NK_ImaginaryNode: case NK_NilNode: case NK_TrueNode: case NK_FalseNode:
      return 0;
    default:
      return -1;
  }
}
/* See call_plan.h. */
int cplan_hash_holds_key_past(Compiler *c, int h, const char *key) {
  const NodeTable *nt = c->nt;
  int en = 0, sp = 0;
  const int *els = nt_arr(nt, h, "elements", &en);
  for (int e = 0; e < en; e++) {
    int k = nt_kind(nt, els[e]) == NK_AssocNode ? nt_ref(nt, els[e], "key") : -1;
    int one;
    if (k >= 0) {
      int is = hash_key_is(nt, k, key);
      one = is < 0 ? -1 : !is;
    } else {
      int v = nt_kind(nt, els[e]) == NK_AssocSplatNode ? nt_ref(nt, els[e], "value") : -1;
      NodeKind vk = v >= 0 ? nt_kind(nt, v) : NK_NONE;
      one = vk == NK_NilNode ? 0 : vk == NK_HashNode ? cplan_hash_holds_key_past(c, v, key) : -1;
    }
    if (one == 1) return 1;
    if (one < 0) sp = -1;
  }
  return sp;
}
/* cplan_hash_positionals for a callee that takes the one keyword `key`
   and passes a hash with any other key on as one more positional
   (builtin_kw_taken): 1 where the hash holds another key
   (cplan_hash_holds_key_past) or is the full branch of a split (split_kwsplat_calls
   tests for another key), 0 where it holds none or is the split's other
   branch, -1 where a `**` operand is the run time's. */
static int hash_positionals_past(Compiler *c, int id, const char *key) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  const int *argv = call_args(nt, id, &argc);
  if (argc == 0 || nt_kind(nt, argv[argc - 1]) != NK_KeywordHashNode) return 0;
  int h = argv[argc - 1];
  if (nt_int(nt, h, "kwsplit_taken", 0)) return 0;
  /* the split's branches: one holds no other key, the other one does */
  return nt_int(nt, h, "kwsplit_full", 0) ? 1 : cplan_hash_holds_key_past(c, h, key);
}
/* See call_plan.h. */
int cplan_builtin_hash_positionals(Compiler *c, int id, const char *cls, const char *name, int cside) {
  int kwpos = builtin_kw_positional(cls, name, cside);
  return kwpos == 2 ? hash_positionals_past(c, id, builtin_kw_taken(cls, name, cside)) : cplan_hash_positionals(c, id, kwpos);
}
typedef struct { const Compiler *c; const int *argv; } CallPlanArgKinds;

static TyKind cplan_bop_arg(const void *ptr, int i) {
  const CallPlanArgKinds *args = ptr;
  return comp_ntype(args->c, args->argv[i]);
}

/* See call_plan.h. */
const BuiltinOp *cplan_builtin_op_row(Compiler *c, int id, TyKind rt, const char *name, int stage, int *argcp) {
  TyKind fam = bop_family_of(rt);
  if (!bop_covers(fam)) return NULL;
  int argc;
  const int *argv = call_args(c->nt, id, &argc);
  CallPlanArgKinds args = { c, argv };
  if (argcp) *argcp = argc;
  const BuiltinOp *op = bop_find_stage(fam, name, argc, nt_ref(c->nt, id, "block") >= 0,
                                       stage >= 0 ? cplan_bop_arg : NULL, &args, stage);
  /* A class-preserving handle result needs the original String. A subclass
     call, including its re-entry on the embedded payload, has its own result. */
  if (op && op->result == TY_STRBUF && (op->flags & BOPF_SELF_CLASS)) {
    int recv = nt_ref(c->nt, id, "receiver");
    TyKind receiver_type = comp_ntype(c, recv);
    if ((receiver_type != TY_STRING && receiver_type != TY_STRBUF) || face_of(recv) != TY_UNKNOWN) return NULL;
  }
  return op;
}

const BuiltinOp *cplan_boxed_fallback_row(Compiler *c, int id, const char *name, TyKind slot, int *argc) {
  if (slot != TY_POLY && slot != TY_STRING && slot != TY_STRBUF) return NULL;
  const BuiltinOp *op = cplan_builtin_op_row(c, id, BOP_BOXED_FALLBACK, name, 1, argc);
  if (op) return op->emit == BOPE_TEMPLATE && (op->result == TY_POLY || op->result == TY_STRBUF) ? op : NULL;
  op = cplan_builtin_op_row(c, id, TY_POLY, name, 1, argc);
  return op && op->emit == BOPE_TEMPLATE && op->result == TY_STRBUF ? op : NULL;
}

/* See call_plan.h. */
int cplan_builtin_display_operand(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || nt_safe_nav(nt, id) ||
      nt_ref(nt, id, "block") >= 0 || cplan_pinned_vis(c, id) || call_plain_argc(c, id) != 0)
    return -1;
  const char *name = view_call_name(c, id);
  int recv = nt_ref(nt, id, "receiver");
  if (!is_to_s_name(name) || recv < 0 || repr_of(c, recv).kind != RK_BOXED ||
      cplan_user(c, id).dispatch != CP_NONE) return -1;
  /* A method-table history includes aliases, undef and visibility changes,
     even when a program method has no C body to emit. */
  if (!elab_stamp(c).recorded || elab_runs_unseen(c) || elab_method_versions(c, name) ||
      elab_method_vis_varies(c, name) || comp_name_answered_any_side(c, name)) return -1;
  return cplan_boxed_fallback_row(c, id, name, TY_POLY, NULL) ? recv : -1;
}

/* See call_plan.h. */
const BuiltinClassOp *cplan_builtin_class_row(Compiler *c, int id, const char *cls) {
  const char *name = nt_str(c->nt, id, "name");
  int argc = 0;
  call_args(c->nt, id, &argc);
  const BuiltinClassOp *cop = bop_class_find(cls, name, argc, nt_ref(c->nt, id, "block") >= 0);
  const char *a;
  for (int i = 0; cop && (a = builtin_ancestor_at(cls, i)); i++) {
    int ci = comp_class_index(c, a);
    if (ci >= 0 && cplan_cmethod_target(c, id, ci, name, 0) >= 0) return NULL;
    if (sp_streq(a, cop->cls)) break;
  }
  return cop;
}

/* Does a builtin answer `name` on program class ci (main for -1): one on
   any self, a core class's own, a record class's own
   (elab_record_cmethod)? */
static int cm_builtin_answers(Compiler *c, int ci, const char *name) {
  const char *cn = ci >= 0 ? c->classes[ci].name : NULL;
  return builtin_self_method_known(name, cm_self_kind(c, ci)) || (ci >= 0 && elab_record_cmethod(c, ci, name)) ||
         (cn && builtin_class_id(cn) && (builtin_cmethod_known(cn, name) || builtin_super_cmethod_known(cn, name)));
}
int cplan_missing(Compiler *c, int id, char *self_text, size_t cap, int *anon) {
  const NodeTable *nt = c->nt;
  const char *name = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int recv = name ? nt_ref(nt, id, "receiver") : -1;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  int cr = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode;
  if (!name || (recv >= 0 && !cr && rk != NK_SelfNode)) return CM_NONE;
  /* (a define_method or attr_* on self where self is an object, which Module's methods are not on:
     an_object_definer_missing) */
  if (!cr && an_object_definer_missing(c, id, comp_scope_of(c, id))) {
    if (anon) *anon = -1;
    if (cap) self_text[0] = '\0';
    return CM_NOMETHOD_SELF;
  }
  /* (a call on main that an undef of main's singleton stops: neither a
     method of the program's nor a builtin's answers it) */
  if (!cr && ct_main_stopped(c, id, name)) {
    if (anon) *anon = -1;
    cm_self_name(c, -1, self_text, cap);
    return nt_vcall(nt, id) ? CM_NAMEERROR : CM_NOMETHOD;
  }
  if (an_scopes_named_first(c, name) < 0) return CM_NONE;
  int ci = cr ? cplan_const_recv_class(c, id, recv, NULL) : cplan_self_class(c, id);
  if (ci < -1 || ci >= c->nclasses || (cr && ci < 0)) return CM_NONE;
  /* a method the tables answer for self, which the record proves is not
     defined where the call runs: a class's class methods, main's (and a
     class body's receiverless call's) top-level functions */
  int cm = ci >= 0 && call_target(c, -1, ci, name, CT_CMETHOD, NULL) >= 0;
  int top = (ci < 0 || recv < 0) && call_target(c, -1, -1, name, CT_TOP, NULL) >= 0;
  int oc = top && ci < 0 ? comp_core_class(c, CORE_OBJECT) : -1;
  MethodLookup cl = { .def_class = -1 }, tl = { .def_class = -1 };   /* (ML_OBJECT: Object's method answers the call; ML_BUILTIN: the builtin's) */
  if ((!cm && !top) || (cm && (call_target(c, id, ci, name, CT_CMETHOD, &cl) >= 0 || cl.kind == ML_BUILTIN)) ||
      (top && (call_target(c, id, -1, name, CT_TOP, &tl) >= 0 || tl.kind == ML_OBJECT || tl.kind == ML_BUILTIN)))
    return CM_NONE;
  /* (a call on main that main's own method does not answer where it runs,
     removed or not yet defined, reaches Object's chain: ct_lookup) */
  if (oc >= 0 && call_target(c, id, oc, name, 0, NULL) >= 0) return CM_NONE;
  if (cm_builtin_answers(c, ci, name) || (cm && ct_core_hook(c, id, name, CT_CMETHOD))) return CM_NONE;
  if (anon) *anon = cm_self_name(c, ci, self_text, cap) ? -1 : ci;
  return nt_vcall(nt, id) ? CM_NAMEERROR : CM_NOMETHOD;
}
/* See call_plan.h. */
int cplan_undef_stops(Compiler *c, int id, int cls, const char *name) {
  MethodLookup ml = { .def_class = -1 };
  mtab_enter(c);   /* (the plan's own read: whether an undef on the chain takes the name away, before its answer is asked) */
  int undef = comp_is_undeffed_in_chain(c, cls, name);
  mtab_leave(c);
  if (!undef) return 0;
  int e = call_target(c, id, cls, name, 0, &ml);
  return e < 0 || ml.kind != ML_ENTRY || c->scopes[e].entry != ENTRY_DISPATCH;
}
/* See call_plan.h. */
int cplan_level_answers(Compiler *c, int id, int k, const char *name) {
  int own = ELAB_UNKNOWN;
  if (id >= 0) {
    int framed = elab_plan_enter(c, id);
    own = elab_call_own_method(c, id, k, name, 0, 0);
    if (framed) elab_plan_leave(c);
  }
  if (own == ELAB_YES) return 1;
  ClassInfo *ci = &c->classes[k];
  mtab_enter(c);   /* (the tables' entries of the level: their answer where the record cannot tell, and their origin) */
  int mi = comp_method_in_class(c, k, name);
  int def = mi >= 0 && (!c->scopes[mi].is_include_copy || c->scopes[mi].is_prepend_copy);
  int rd = comp_is_own_reader(ci, name), rd_copy = 0;
  for (int m = 0; rd && m < ci->nprepended_mods; m++) {
    int p = ci->prepended_mods[m];
    rd_copy = rd_copy || (p >= 0 && p < c->nclasses && comp_is_reader(&c->classes[p], name));
  }
  mtab_leave(c);
  int al = 0, al_copy = 0;
  for (int a = 0; a < ci->naliases; a++)
    if ((!ci->alias_sg || !ci->alias_sg[a]) && sp_streq(ci->alias_new[a], name)) {
      al = 1;
      al_copy = al_copy || ci->alias_node[a] < 0;
    }
  /* The record's none answers where the record places every entry the
     tables hold for the name at this level: a def, reader or alias of the
     level's own body. An entry a mixin copied into the level (a prepend's
     def or reader, an alias an include copied, which has no statement of
     its own) is no source the record models there, and the tables answer. */
  int ans = own == ELAB_NO ? (def && c->scopes[mi].is_prepend_copy) || rd_copy || al_copy : def || rd || al;
  /* (the tables' answer is noted, as call_target's are: unasked with no
     site, where the record cannot tell, or a copy against the record's none) */
  if (g_method_table_check && (own == ELAB_UNKNOWN || ans))
    mtab_table(c, id, k, 0, name, id < 0 ? MLW_UNASKED : own == ELAB_NO ? MLW_DISAGREE :
                                  elab_in_unplaced_block(c, id) ? MLW_UNPLACED : MLW_MANY);
  return ans;
}
/* The builtin class a program class's class side inherits from (Object
   unless it names another), for the singleton methods it answers; NULL where
   the chain ends at a native class, a Struct or a superclass that is no
   plain `class K` (refl_builtin_vis reads those apart). */
static const char *cu_class_root(Compiler *c, int ci) {
  int depth = 0;
  for (int k = ci; k >= 0 && depth++ <= c->nclasses; k = c->classes[k].parent)
    if (c->classes[k].is_native_class) return NULL;
  const ClassInfo *ri = &c->classes[ci];
  if (ri->is_builtin_const && ri->enclosing_class < 0) return ri->name;
  /* the builtin class the whole chain stands on (class_builtin_parent), by its row */
  int bid = class_builtin_parent(c, ci), i = 0;
  const BuiltinClass *r = NULL;
  while ((r = builtin_class_at(i++)) && r->id != bid) {}
  return r ? r->name : NULL;
}
/* Does CRuby's arity table hold a row for `name` of the class that owns it
   (Kernel's and BasicObject's public methods are Object's rows; Kernel's
   module functions have their own, builtin_kernel_fn_span), and admit argc
   positional arguments? No row: not admitted, the table cannot tell. */
static int cu_arity_admits(const char *own, const char *name, int argc) {
  const char *rows = is_object_root(own) ? "Object" : own;
  int lo, hi;
  if (builtin_instance_method_known(rows, name)) return builtin_arity_admits(rows, name, argc);
  return is_kernel_module_name(own) && builtin_kernel_fn_span(name, 0, &lo, &hi) && argc >= lo && (hi < 0 || argc <= hi);
}
/* Does the record prove that no method `name` is defined where call id runs,
   for the instance side of the class named cls (program class ci, or -1 for
   a builtin)? A def proves nothing about a class it does not reach, so the
   proof is the record's answer for each class a def could be on: the
   program's top-level defs (Object's private methods), program class ci's
   chain, or each builtin ancestor of cls the program reopens (a reopening
   carries its includes and prepends, and the record lists them: a mixin it
   cannot place makes the answer unknown, and a computed include is refused).
   elab_call_method also fails the proof for a run the record does not see.
   CRuby's instance_method looks at neither method_missing nor
   respond_to_missing?, and `method` at respond_to_missing? only (bound), so
   only that hook fails the proof, and only for `method`. It cannot
   place a call in a block a user method yields to (then only a name the
   program defines nowhere is proved). */
static int cu_name_absent(Compiler *c, int id, const char *cls, int ci, const char *name, int bound) {
  /* a method CRuby's tables give the subject or an ancestor (with the
     libraries the program loads: the json or bigdecimal methods a require
     adds) is there whatever the program defines */
  const char *tcls = ci >= 0 ? cu_class_root(c, ci) : cls;
  if (tcls && builtin_method_answers(tcls, name, 0, 1)) return 0;
  /* a define_method, alias_method or attr_* of a name the program computes
     adds names the record cannot list */
  if (an_prog_meta_by_value()) return 0;
  /* a name no def, alias or define_method of the program has anywhere needs no
     placement: nothing can add it but the sources the next tests rule out */
  int hooks = bound && elab_method_versions(c, RESPOND_TO_MISSING_NAME);
  if (!elab_method_versions(c, name) && !hooks && !elab_runs_unseen(c)) return 1;
  if (hooks) return 0;
  const char *anc = ci >= 0 ? NULL : core_class_ancestors(cls);
  if (ci < 0 && !anc) return 0;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, -1, name, 0, 0) == ELAB_NO && (ci < 0 || elab_call_method(c, id, ci, name, 0, 0) == ELAB_NO);
  size_t l;
  for (const char *p = anc && *anc ? anc : NULL, *nx; none && p; p = nx) {
    char an[96];
    nx = core_ancestor_step(p, &l);
    snprintf(an, sizeof an, "%.*s", (int)l, p);
    int slot = builtin_core_class_slot(an), ca = slot >= 0 ? comp_core_class(c, slot) : -1;
    if (ca >= 0) none = elab_call_method(c, id, ca, name, 0, 0) == ELAB_NO;
  }
  if (framed) elab_plan_leave(c);
  return none;
}
/* The calls that look a method up by the name they are given and raise
   NameError for one that is not there: Module#instance_method and
   public_instance_method on a class or module value (subj 1), method and
   public_method on a value (subj 2). CRuby's NameError (and its words) when
   a literal name is provably absent, the gap when it is there or the record
   cannot prove it is not (a method_missing or respond_to_missing?, a run it
   does not see, a def of the name it cannot place); a computed name, a
   private name asked of the public forms, singleton_method and a self or a
   class value as the subject of method are CU_NONE: the gate's NoMethodError, which is a
   NameError, stays. msg gets the NameError's message. */
static CplanUnrouted cu_lookup_by_name(Compiler *c, int id, const char *name, int subj, const char *cls,
                                       int ci, int is_mod, char *msg, size_t cap) {
  const char *sym = method_sym_arg(c, id);
  if (!sym || !cls || ci < -1 || (ci >= 0 && comp_class_anonymous(c, ci)) || !subj) return CU_NONE;
  int pub = is_public_method_lookup(name);
  /* a program class's chain is the record's; a builtin's, CRuby's tables (any
     visibility for the plain forms) */
  int v = ci >= 0 && !c->classes[ci].is_builtin_const ? refl_builtin_vis(c, ci, sym, 1, 1, NULL) : core_method_vis(cls, sym, 1, NULL);
  if (v < 0) return CU_NONE;
  if (v > 0) return (pub && v != 1) ? CU_NONE : CU_GAP;
  /* not in the tables: absent only where the record proves it; else a method
     a source it cannot list may add is the gap, not a NameError */
  if (!cu_name_absent(c, id, cls, ci >= 0 && !c->classes[ci].is_builtin_const ? ci : -1, sym,
                      subj == 2)) return CU_GAP;
  snprintf(msg, cap, "undefined method '%s' for %s '%s'", sym, is_mod ? "module" : "class", cls);
  return CU_RAISES_NAME;
}
/* What a class or module value answers for `name`, by CRuby's lookup on its
   class side: the singleton methods of root (the builtin class it is or
   stands on; NULL for a module) and of its superclasses, then Class's or
   Module's instance methods, an undefine on the way ending it
   (Rational.allocate, Integer.new). The visibility and *own as
   core_method_vis gives them; a method of root's own class side is public
   there, with *sep '.'. */
static int cu_class_value_vis(int mod, const char *root, const char *name, int any_vis, const char **own, int *sep) {
  int v = core_method_vis(mod ? "Module" : "Class", name, 1, own);
  if (!root) return v;
  if ((v <= 0 || (v != 1 && !any_vis)) && builtin_method_answers(root, name, 1, 1)) { *own = root; *sep = '.'; return 1; }
  /* what Class or Module has, the lookup may not reach */
  return v > 0 && !builtin_method_answers(root, name, 1, 0) ? 0 : v;
}
/* See call_plan.h. */
int cplan_class_builtin_has(Compiler *c, int ci, const char *name) {
  int mod = comp_class_is_module(c, &c->classes[ci]), sep = '#';
  const char *own = NULL;
  return cu_class_value_vis(mod, mod ? NULL : cu_class_root(c, ci), name, 1, &own, &sep) > 0 || cplan_class_builtin_responds(c, ci, name);
}
/* See call_plan.h. */
CplanUnrouted cplan_unrouted(Compiler *c, int id, char *owner, size_t cap, int *nil_answers) {
  const NodeTable *nt = c->nt;
  const char *name = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int recv = name ? nt_ref(nt, id, "receiver") : -1, argc = 0;
  const int *argv = name ? call_args(nt, id, &argc) : NULL;
  if (nil_answers) *nil_answers = name && core_method_vis("NilClass", name, 1, NULL) > 0;
  if (!name || nt_argv_has(nt, argv, argc, NT_ARG_SPLAT | NT_ARG_KEYWORDS | NT_ARG_FORWARDING)) return CU_NONE;
  /* a method whose CRuby answer turns on the name it is given (a NameError
     for one not allowed or not there, `Object.class_variable_get("x")`) is
     no gap the plan can tell: a `rescue NameError` around the call catches
     CRuby's raise, and the gate's NoMethodError, which is a NameError, but
     no NotImplementedError */
  if (argc > 0 && is_name_checked_call(name)) return CU_NONE;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  int selfish = recv < 0 || rk == NK_SelfNode;
  /* the visibility the call's shape reaches: send and a receiverless call
     reach any, public_send and an explicit receiver only public methods */
  int any_vis = (selfish || nt_str(nt, id, "send_blind")) && !nt_str(nt, id, "vis_enforce");
  /* what the tables say: vis (1 public, 2 protected, 4 private, 0 none, -1
     the class is not in the tables), the owner, and the call's separator
     ('.' for a module function on its module) */
  int v = 0, ocls = comp_core_class(c, CORE_OBJECT), sep = '#', undef_ci = -1;
  /* (cside: a class or module value's builtin class, whose class side
     holds the method the warning names) */
  const char *own = NULL, *cside = NULL;
  /* what a lookup by name asks of (cu_lookup_by_name): the subject's kind (1 a
     class or module value, 2 a value), its class's name and program class */
  int subj = 0, subj_ci = -1, subj_mod = 0;
  const char *subj_cls = NULL;
  TyKind rkinds[2] = { TY_UNKNOWN, TY_UNKNOWN };
  if (selfish) {
    int ie = ie_class_of(c, id), si = cplan_self_class(c, id);
    const Scope *sc = comp_scope_of(c, id);
    /* an instance_eval block (or define_method's) has the class of the
       object it runs on, or none the plan knows; otherwise the record
       settles main and a class body, and a def's own class says the rest */
    int inst = -2, cval = -2, main_self = 0;
    if (ie != -1) { if (ie < 0) return CU_NONE; inst = ie; }
    else if (si >= 0) cval = si;
    else if (si == -1) main_self = 1;
    else if (sc && sc->class_id >= 0) { if (sc->is_cmethod) cval = sc->class_id; else inst = sc->class_id; }
    else if (sc) inst = -1;
    else return CU_NONE;
    rkinds[0] = BOP_KERNEL;
    if (inst >= 0 && (comp_class_is_module(c, &c->classes[inst]) || cu_class_root(c, inst) == NULL)) return CU_NONE;
    if (main_self || inst == -1) {
      v = core_method_vis("Object", name, 1, &own);
      if (v <= 0 && main_self && main_self_method(name)) { v = 4; own = "main"; }
    }
    else if (inst >= 0) { v = refl_builtin_vis(c, inst, name, 1, 1, &own); undef_ci = inst; }
    else {
      int mod = comp_class_is_module(c, &c->classes[cval]);
      cside = mod ? NULL : cu_class_root(c, cval);
      v = cu_class_value_vis(mod, cside, name, any_vis, &own, &sep);
      rkinds[1] = TY_CLASS;
      subj = 1; subj_mod = mod; subj_ci = cval;
      subj_cls = c->classes[cval].is_builtin_const ? c->classes[cval].name : class_ruby_name(c, cval);
    }
  }
  else if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    int core = -1, ci = cplan_const_recv_class(c, id, recv, &core);
    const char *nm = core >= 0 ? core_class_name(core) : NULL;   /* a builtin class or module (also a reopened one) */
    if (!nm && ci < 0) return CU_NONE;
    int mod = nm ? is_builtin_module_name(nm) : comp_class_is_module(c, &c->classes[ci]);
    cside = nm ? nm : mod ? NULL : cu_class_root(c, ci);
    v = cu_class_value_vis(mod, cside, name, any_vis, &own, &sep);
    rkinds[0] = TY_CLASS;
    subj = 1; subj_mod = mod;
    subj_ci = ci >= 0 && core < 0 ? ci : -1;
    subj_cls = nm ? nm : ci >= 0 ? class_ruby_name(c, ci) : NULL;
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    if (repr_of(c, recv).kind == RK_BOXED || rt == TY_CLASS) return CU_NONE;
    if (ty_is_object(rt)) {
      undef_ci = ty_object_class(rt);
      if (!cu_class_root(c, undef_ci)) return CU_NONE;
      v = refl_builtin_vis(c, undef_ci, name, 1, 1, &own);
    }
    else {
      int k = core_of_type(rt);
      const char *cn = k >= 0 ? core_class_name(k) : NULL;
      if (!cn) return CU_NONE;
      v = core_method_vis(cn, name, 1, &own);
      subj_cls = cn;
    }
    if (ty_is_object(rt)) subj_cls = class_ruby_name(c, undef_ci);
    subj = 2; subj_ci = undef_ci;
    rkinds[0] = rt;
    if (ty_builtin_ivar_less(rt)) rkinds[1] = BOP_IVAR_LESS;
  }
  if (v < 0 || (v > 0 && !own)) return CU_NONE;
  /* the program may undefine the name on the receiver's chain (or Object's) */
  if (v > 0 && ((undef_ci >= 0 && cplan_undef_stops(c, id, undef_ci, name)) ||
                (ocls >= 0 && cplan_undef_stops(c, id, ocls, name)))) return CU_RAISES;
  /* a method of the name that the program gives Object stands in front of
     Kernel's for every receiver (a version of it may be the live one where
     this call runs): not the builtin's gap */
  if (v > 0 && ((ocls >= 0 && call_target(c, -1, ocls, name, 0, NULL) >= 0) ||
                call_target(c, -1, -1, name, CT_TOP, NULL) >= 0)) return CU_NONE;
  if (v == 0 || (v != 1 && !any_vis)) return CU_RAISES;
  if (!cu_arity_admits(own, name, argc)) return CU_NONE;
  /* a builtin-op row that takes the call is a route; the call's decline is
     that route's rejection of an operand, which CRuby raises over too
     (`[1].find(42) { false }` raises over `42.call`) */
  for (int i = 0; argc > 0 && i < 2; i++)
    if (rkinds[i] != TY_UNKNOWN && bop_row_takes(rkinds[i], name, argc, nt_ref(nt, id, "block") >= 0))
      return CU_NONE;
  if (is_method_object_maker(name)) {
    CplanUnrouted r = (is_instance_method_name(name) && subj == 1) || (is_bound_method_maker(name) && subj == 2)
                      ? cu_lookup_by_name(c, id, name, subj, subj_cls, subj_ci, subj_mod, owner, cap) : CU_NONE;
    if (r != CU_GAP) return r;
  }
  /* the method CRuby calls on a class value: its class side's, ahead of
     Class's or Module's instance method of the name (Class.constants is
     Module.constants), under the class or module that has it */
  const char *so = cside ? builtin_singleton_owner(cside, name) : NULL;
  snprintf(owner, cap, "%s%c%s", so ? so : own, so ? '.' : sep, name);
  return CU_GAP;
}
/* The program class a bound capture (`S.method(:z)`, or on self or bare)
   asks of, by the record where it runs, the name it asks into *name; -1
   for any other node */
static int cm_capture_class(Compiler *c, int id, const char **name) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  int cr = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode;
  const char *sym = method_sym_arg(c, id);
  if (!is_method_obj_call(c, id) || is_instance_method_name(nt_str(nt, id, "name")) ||
      (recv >= 0 && !cr && rk != NK_SelfNode) || !sym)
    return -1;
  if (name) *name = sym;
  int ci = cr ? cplan_const_recv_class(c, id, recv, NULL) : cplan_self_class(c, id);
  return ci >= 0 && ci < c->nclasses ? ci : -1;
}
/* Can capture id (`K.method(:k)`, `public_method`) of cls's `name`, on
   its class side for sg, state CRuby's NameError when it finds no method,
   and how (CP_ABSENT_*)? The message names the receiver's class, which
   must be the one asked: a constant's (a Class.new class's by the constant
   that names it), self's in a class body, or for a version entry's
   (entry) instance side a class nothing inherits from. Not main's, nor a
   module's instance side, which is an includer's. A module's class side
   is named 'Module' until CRuby makes its singleton class, `#<Class:M>`
   from then on (elab_singleton_made, asked where the NameError is raised,
   where no version of the name is live): CP_ABSENT_UNTOLD where the record
   cannot tell which (cplan_refuse_resolve refuses it). method() also
   finds private methods, which the call does not reach: not for a name a
   top-level function has (Object's), nor for the class side one a builtin
   class's reopening (Object's, Module's) or a module, a builtin's
   reopening too, that may be on a singleton class's chain
   (elab_module_on_singleton_chain: Kernel, an extend's) has as an
   instance method. CP_ABSENT_NONE where it cannot state it. */
static int cplan_capture_names(Compiler *c, int id, int cls, int sg, const char *name, int entry) {
  int nd = 0;
  if (cls < 0 || cls >= c->nclasses || !name) return 0;
  int mod = comp_class_is_module(c, &c->classes[cls]);
  if (mod && !sg) return 0;
  if (entry) {
    int recv = nt_ref(c->nt, id, "receiver");
    NodeKind rk = recv >= 0 ? nt_kind(c->nt, recv) : NK_SelfNode;
    comp_descendants(c, cls, &nd);
    if (nd && !(sg && (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode))) return 0;
  }
  for (int s = an_scopes_named_first(c, name); s >= 0; s = an_scopes_named_next(c, name, s)) {
    int sc = c->scopes[s].class_id;
    if (sc < 0) return 0;
    int smod = comp_class_is_module(c, &c->classes[sc]);
    if (sg && !c->scopes[s].is_cmethod && (smod ? elab_module_on_singleton_chain(c, sc) : c->classes[sc].is_builtin_const))
      return 0;
  }
  if (!mod) return sg ? CP_ABSENT_SG : CP_ABSENT_INST;
  int made = elab_singleton_made(c, cls, id, name);
  return made == ELAB_YES ? CP_ABSENT_SG : made == ELAB_NO ? CP_ABSENT_MODULE : CP_ABSENT_UNTOLD;
}
/* Is a class method `name` of cls one that no def, alias or define_method
   of the program makes (no version of the name), where the record proves
   none is defined at capture id: no method_missing or respond_to_missing?,
   nothing it cannot follow? (ct_undefined_at asks only a name with
   versions.) */
static int cm_capture_never_made(Compiler *c, int id, int cls, const char *name) {
  if (!name || elab_method_versions(c, name)) return 0;
  /* (a name any core class or module has, of any visibility, or a core
     class's singleton method: the builtin lists cm_builtin_answers asks do
     not hold them all) */
  if (core_method_name_any(name) || bop_core_name(name)) return 0;
  /* (a Symbol argument: CRuby names another side for a String's) */
  int ac = 0;
  const int *av = call_args(c->nt, id, &ac);
  if (ac < 1 || nt_kind(c->nt, av[0]) != NK_SymbolNode) return 0;
  /* (a name the program can make nowhere is made nowhere, whenever the capture runs: a block's, which the record
     cannot place, too. Not a module's, whose NameError names its singleton class only once the record tells it is
     made: cplan_capture_names) */
  if (!comp_class_is_module(c, &c->classes[cls]) && an_name_never_made(c, name)) return 1;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, cls, name, 1, 1) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none;
}
/* The visibility, private or protected (SP_VIS_*), that closes public_method capture id of method mi to it: the
   visibility mi's name has, on mi's side, in the class the receiver names (a constant's or self's, cm_capture_class,
   or the static class of an object that is never nil, whose nil's NameError the capture raises) where the capture runs (cplan_method_vis); 0 where it is public, where no one class
   is named, and where the record cannot tell the visibility at the capture (a body's word may change it, or one it
   does not place, elab_method_vis_unplaced): the capture keeps the method. But for a class method whose visibility
   a body's word sets, which the record cannot tell there: CP_CLOSED_RUN where the cells of the call there tell it
   (cplan_cmethod_presence), each version's visibility, every version of the chain (nothing else answers where none
   is live, whose NameError names the singleton class, cplan_capture_names); CP_CLOSED_UNTOLD where they cannot. *cls,
   the class */
static int cplan_capture_closed(Compiler *c, int id, int mi, short *cls) {
  const char *sym = nt_int(c->nt, id, "public_capture", 0) ? method_sym_arg(c, id) : NULL;
  int recv = nt_ref(c->nt, id, "receiver"), sg = c->scopes[mi].is_cmethod, ci = sym ? cm_capture_class(c, id, NULL) : -1;
  /* (a class side's on self or bare in a class method too, as its readers ask it: cells_reader_class) */
  int rc = sym && sg && capture_cells_shape(c->nt, id) ? cells_reader_class(c, id) : -2;
  if (ci < 0 && rc >= 0) ci = rc;
  if (rc > -2 && elab_method_body_word(c, sym, 1) && (rc < 0 || elab_method_vis_at(c, rc, sym, 1, elab_point_of(c, id)) < 0)) {
    CplanCells cs;
    *cls = (short)rc;
    return rc >= 0 && cplan_cmethod_presence(c, id, rc, sym, 7, 0, &cs) && cplan_cells_testable(c, &cs) &&
           !cplan_class_builtin_has(c, rc, sym) && cplan_capture_names(c, id, rc, 1, sym, 0) == CP_ABSENT_SG ? CP_CLOSED_RUN : CP_CLOSED_UNTOLD;
  }
  if (sym && ci < 0 && !sg && recv >= 0 && ty_is_object(comp_ntype(c, recv)) && !repr_of(c, recv).may_nil)
    ci = ty_object_class(comp_ntype(c, recv));
  int v = ci >= 0 ? cplan_method_vis(c, id, ci, sym, sg ? CPV_CMETHOD : 0, NULL) : SP_VIS_PUBLIC;
  if (v == SP_VIS_PUBLIC || elab_method_vis_unplaced(c, sym) ||
      (elab_method_vis_varies(c, sym) && elab_method_vis_at(c, ci, sym, sg, elab_point_of(c, id)) != v))
    return 0;
  *cls = (short)ci;
  return v;
}
/* Where does capture id (`K.method(:k)`, `public_method`; mi its target,
   -1 for none) raise CRuby's NameError: CP_ABSENT_SG (or
   CP_ABSENT_MODULE, a module whose singleton class is not made yet),
   naming K's class side, where the record proves no class method of the
   name is defined where it runs, as a call there raises (ct_undefined_at,
   cplan_missing), with no builtin answering it; for mi a version entry,
   that or CP_ABSENT_INST (its instance side) while none of its versions
   is live, where the else arm the call runs then finds no method
   (cplan_entry_finds), so neither does method(); CP_ABSENT_UNTOLD where
   the record cannot tell which a module's is (cplan_capture_names). 0
   otherwise, and where the message cannot be stated. */
static int cplan_capture_absent(Compiler *c, int id, int mi) {
  const char *name = NULL;
  const Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  int cls = m ? m->class_id : cm_capture_class(c, id, &name), sg = m ? m->is_cmethod : 1;
  if (m) {
    name = method_sym_arg(c, id);
    if (m->entry != ENTRY_DISPATCH && m->entry != ENTRY_TABLE) return 0;
    if (is_instance_method_name(nt_str(c->nt, id, "name"))) {
      CplanCells cs;
      if (!cplan_guarded_presence(c, cls, name, 0, &cs) || cs.els) return 0;
    } else if (cplan_entry_finds(c, mi) != VER_FINDS_NONE) return 0;
  }
  else if (cls < 0 || !(ct_undefined_at(c, id, cls, name, CT_CMETHOD, -1) || cm_capture_never_made(c, id, cls, name)) ||
           cm_builtin_answers(c, cls, name)) return 0;
  return cplan_capture_names(c, id, cls, sg, name, m != NULL);
}
/* Does a receiverless capture of `sym` that runs on main (not in a class's method, a module's, or a top-level def another
   object may run) find no method where it runs (main's chain meets none: the main-chain
   fact), though the program defines the name, only later (a def, an include or an extend that has not run)? Then its
   NameError is main's singleton class's, as CRuby's. Not a name the core's Kernel answers (the capture binds that), unless
   an undef stops the lookup, nor where a method_missing or a respond_to_missing? of the program may answer it. */
static int cplan_main_capture_absent(Compiler *c, int id) {
  const char *name = method_sym_arg(c, id);
  int recv = nt_ref(c->nt, id, "receiver");
  if (!name || !name[0] || (recv >= 0 && nt_kind(c->nt, recv) != NK_SelfNode) || !ct_runs_on_main(c, id)) return 0;
  /* (nothing of the name, or an undef that stops the lookup, which no core method answers behind either) */
  int st = cplan_main_chain_step(c, id, name);
  if (st != EMC_NONE && st != EMC_STOP) return 0;
  /* (a name the program never defines is main's NameError where nothing can make it, an_name_never_made) */
  if (st != EMC_STOP && (core_method_name_any(name) || bop_core_name(name) ||
                         (!elab_method_versions(c, name) && !an_name_never_made(c, name)))) return 0;
  return an_name_binder_free(c, METHOD_MISSING_NAME) && an_name_binder_free(c, RESPOND_TO_MISSING_NAME);
}
/* Is ci's class method `name` defined wherever node at runs? */
static int capture_live_at(Compiler *c, int at, int ci, const char *name) {
  const LrDef *ds[VER_MAX_ARMS];
  int framed = elab_plan_enter(c, at), n;
  int live = elab_call_method(c, at, ci, name, 1, 1) == ELAB_YES;
  if (!live && (n = elab_call_span(c, at, ci, name, 1, 0, NULL, ds, VER_MAX_ARMS)) >= 1) {
    live = 1;
    for (int k = 0; k < n && k < VER_MAX_ARMS; k++) live &= ds[k] != NULL;
  }
  if (framed) elab_plan_leave(c);
  return live;
}
/* A capture of a record class's (Struct.new's, Data.define's) class method
   the program defines, which the record neither proves defined where it
   runs nor absent (cplan_capture_absent): it may run before the def (a
   loop's body around both, a method a call ahead of the def runs). In a
   closure, the calls that run it are asked (elab_closure_runs), not the
   place it is made. No entry tests the def at run time (a name a
   reflection reader names has none, lr_late_make), and the def bound
   there makes a Method where CRuby raises NameError. A closure that
   escapes keeps the def bound, as before, since only a run-time test of
   the def could place it; so do a plain class's capture and one of a name
   a builtin answers. */
static int cplan_capture_gap(Compiler *c, int id) {
  const char *name = NULL;
  int ci = nt_kind(c->nt, id) == NK_CallNode ? cm_capture_class(c, id, &name) : -1;
  if (ci < 0 || elab_record_class_of(c, ci) < 0 || call_target(c, -1, ci, name, CT_CMETHOD, NULL) < 0 ||
      cm_builtin_answers(c, ci, name) || cplan_capture_absent(c, id, -1))
    return 0;
  if (capture_live_at(c, id, ci, name)) return 0;
  int sites[16], n = elab_closure_runs(c, id, sites, 16);
  for (int k = 0; k < n; k++)
    if (!capture_live_at(c, sites[k], ci, name)) return 1;
  return n < 0;
}
/* (does a builtin answer name for an instance of program class cid, or
   may it: one of Object's instance methods, public or private, as the
   oracle lists them (Kernel's, BasicObject's: object_instance_method_name),
   one of a builtin module's cid includes (Comparable, Enumerable:
   core_method_vis), or any where cid's chain holds a native class?) */
static int capture_builtin_answers(Compiler *c, int cid, const char *name) {
  if (object_instance_method_name(name) ||
      (class_is_comparable(c, cid) && core_method_vis("Comparable", name, 0, NULL) > 0) ||
      (class_mixes_in(c, cid, elab_module_named(c, "Enumerable"), 0) && core_method_vis("Enumerable", name, 0, NULL) > 0))
    return 1;
  for (int k = cid, d = 0; k >= 0 && d <= c->nclasses; k = c->classes[k].parent, d++)
    if (c->classes[k].is_native_class) return 1;
  return 0;
}
/* (is d a def that surely ran where it stands: under no condition, past
   no statement that may raise first, elab_point_partial?) */
static int ct_def_sure(Compiler *c, const LrDef *d) {
  return d && d->kind == LR_DEF && !d->may && !d->unplaced && !elab_point_partial(c, d->node);
}
static int ver_builtin_chain(Compiler *c, int cls, int sg, const char *name, int ar, int blk);
/* Do the tables leave a capture of sym on class k to the two hooks: nothing of the name answers on k's chain or a
   class under it, and k's chain holds both hooks? */
static int ct_capture_hooked(Compiler *c, int k, const char *sym, const char *const *hooks) {
  if (k < 0 || !sym || capture_builtin_answers(c, k, sym) || ver_builtin_chain(c, k, 0, sym, 0, 0) ||
      ct_accessor_class(c, k, sym) >= 0)
    return 0;
  for (int i = 0; i < 2; i++)
    if (call_target(c, -1, k, hooks[i], CT_METHODS | CT_SHARED, NULL) < 0) return 0;
  /* (a method of the name, but for the stand-in a later include or
     prepend puts in front of none, cplan_capture_arm) */
  mtab_enter(c);   /* (the plan's own read: the captured name, on k's chain and under it) */
  int mi = comp_method_in_chain(c, k, sym, NULL), nd = 0;
  mtab_leave(c);
  if (mi >= 0 && !c->scopes[mi].mixin_guarded && c->scopes[mi].entry == ENTRY_NONE) return 0;
  /* a class under k that has one: the receiver may be of it */
  const int *ds = comp_descendants(c, k, &nd);
  for (int i = 0; i < nd; i++) {
    mtab_enter(c);
    int d = ds[i], dm = comp_method_in_chain(c, d, sym, NULL);
    mtab_leave(c);
    if (!c->classes[d].unreferenced && ((dm >= 0 && dm != mi) || ct_accessor_class(c, d, sym) >= 0)) return 0;
  }
  return 1;
}
/* See call_plan.h. */
int cplan_capture_misses(Compiler *c, int id, int k, const char *sym) {
  static const char *const hooks[] = { METHOD_MISSING_NAME, RESPOND_TO_MISSING_NAME };
  if (ct_versioned_string_reader(c, k, 0, sym) && conditional_guard_method(c, k, sym) >= 0) {
    ElabVersion versions[VER_MAX_ARMS];
    int n = elab_history_versions(c, k, 0, sym, versions, VER_MAX_ARMS), undef = 0;
    for (int i = 0; i < n; i++) undef |= versions[i].v->kind == LR_UNDEF;
    if (!undef) return 0;
  } else if (!ct_capture_hooked(c, k, sym, hooks)) return 0;
  /* both hooks a def surely live where the capture runs */
  int framed = elab_plan_enter(c, id), live = 1, none;
  for (int i = 0; i < 2 && live; i++) live = ct_def_sure(c, ct_met_version(c, id, k, hooks[i], 0, &none));
  if (framed) elab_plan_leave(c);
  return live;
}
/* The visibility (SP_VIS_*) of program method m of Object's reopening ci
   where reflection call node runs, when main's history of m and Object's
   are one entry (elab_main_object_one_entry): the later version of either
   answers, as the load record orders them. -1 for any other class or name,
   and where the record cannot answer (the class's table does then). */
static int cplan_object_entry_vis(Compiler *c, int node, int ci, const char *m) {
  if (ci < 0 || ci != comp_core_class(c, CORE_OBJECT) || !elab_main_object_one_entry(c, m)) return -1;
  return elab_method_vis_at(c, ci, m, 0, elab_point_of(c, node));
}
/* See call_plan.h. */
int cplan_builtin_method_vis(Compiler *c, int node, int ci, const char *cls, const char *m, int inherit, int own) {
  const ClassInfo *ki = ci >= 0 ? &c->classes[ci] : NULL;
  /* (an alias in Object of a builtin's name, with main's entries of it) */
  int ov = cplan_object_entry_vis(c, node, ci, m);
  if (ov >= 0) return core_bit_of_vis(ov);
  int bv = ki ? refl_builtin_vis(c, ci, m, inherit, own, NULL) : core_method_vis(cls, m, inherit, NULL);
  /* main's defs are Object's own methods, ahead of its and Kernel's rows */
  const char *on = ki ? (ki->is_builtin_const && ki->enclosing_class < 0 ? ki->name : NULL) : cls;
  int mv = bv >= 0 && on && is_object_class_name(on) ? elab_main_method_vis_at(c, node, m) : -1;
  return mv > 0 ? mv : bv;
}
/* (a visibility word a method body runs gives the table no visibility: the one the record has for m on ci where
   reflection call node runs, as presence_at asks it, elab_method_body_word; -1 where no body word names m or the
   record cannot tell there) */
static int cplan_body_word_vis(Compiler *c, int node, int ci, const char *m) {
  return node >= 0 && ci >= 0 && elab_method_body_word(c, m, 0) ? elab_method_vis_at(c, ci, m, 0, elab_point_of(c, node)) : -1;
}
/* See call_plan.h. */
int cplan_program_method_vis(Compiler *c, int node, int ci, const char *m, int inherit) {
  int v = cplan_object_entry_vis(c, node, ci, m);
  if (v >= 0) return v;
  if (inherit && (v = cplan_body_word_vis(c, node, ci, m)) >= 0) return v;
  mtab_enter(c);
  v = inherit ? comp_method_vis_in_chain(c, ci, m) : comp_method_vis(&c->classes[ci], m);
  mtab_leave(c);
  return v;
}
/* Does call id's form reach a Kernel function (they are private)? Receiverless,
   `self.`, or a send (the retargeted call) whose receiver's class does not own
   the name; any other explicit receiver names a private method. A dispatch's
   default arm (dflt) takes the receivers its arms did not, so none of them
   owns the name there. */
static int cplan_kernel_form(Compiler *c, int id, const char *name, int dflt) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  return recv < 0 || nt_kind(nt, recv) == NK_SelfNode ||
         (nt_str(nt, id, "send_blind") && (dflt || !send_blind_recv_owns(c, id, recv, comp_ntype(c, recv), name)));
}
/* Does Kernel reach the receiver of call id (its self, or the class of a send's
   receiver; any class a dispatch's default arm takes when the receiver is of
   no one class)? A class under BasicObject with no Kernel among its
   ancestors (cplan_class_kernel) has no Kernel above it, and its
   instances raise NoMethodError. */
static int cplan_kernel_reaches(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int ci = recv < 0 || nt_kind(nt, recv) == NK_SelfNode ? cplan_self_class(c, id)
         : ty_is_object(comp_ntype(c, recv)) ? ty_object_class(comp_ntype(c, recv)) : -1;
  if (ci < -1 && (recv < 0 || nt_kind(nt, recv) == NK_SelfNode)) {   /* (a self the record cannot place: its scope's class) */
    const Scope *in = comp_scope_of(c, id);
    ci = in ? in->class_id : -1;
  }
  if (ci >= 0 && ci < c->nclasses) return cplan_class_kernel(c, ci) != KERN_LACKS;
  if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) return 1;
  for (int k = 0; k < c->nclasses; k++)
    if (cplan_class_kernel(c, k) == KERN_LACKS) return 0;
  return 1;
}
/* See call_plan.h. */
int cplan_dispatch_row(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = id >= 0 && nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int argc = 0;
  if (name) call_args(nt, id, &argc);
  const BuiltinOp *op = name ? bop_find(BOP_KERNEL, name, argc, nt_ref(nt, id, "block") >= 0) : NULL;
  int oci = comp_core_class(c, CORE_OBJECT);   /* (Object's reopening answers every self) */
  return op && !(op->flags & BOPF_INTERNAL) && cplan_kernel_form(c, id, name, 1) && cplan_kernel_reaches(c, id) &&
         call_target(c, id, -1, name, CT_TOP, NULL) < 0 &&
         !(oci >= 0 && call_target(c, id, oci, name, 0, NULL) >= 0);
}
/* See call_plan.h. */
CplanMissing cplan_dispatch_miss(Compiler *c, int id, int cid) {
  const NodeTable *nt = c->nt;
  const char *name = id >= 0 && nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  if (!name || cid < 0 || cid >= c->nclasses || call_target(c, id, cid, name, 0, NULL) >= 0 || cplan_dispatch_row(c, id) ||
      builtin_object_method_known(name) || is_kernel_module_function(name) ||
      (is_comparable_module_method(name) && class_is_comparable(c, cid)) ||
      (name_is_enumerable_module_method(name) && class_mixes_in(c, cid, elab_module_named(c, "Enumerable"), 0)) ||
      call_target(c, id, cid, METHOD_MISSING_NAME, CT_METHODS, NULL) >= 0 || an_user_defines_method(c, METHOD_MISSING_NAME))
    return CM_NONE;
  for (int k = cid, d = 0; k >= 0 && d <= c->nclasses; k = c->classes[k].parent, d++)
    if (c->classes[k].is_native_class) return CM_NONE;
  return nt_vcall(nt, id) ? CM_NAMEERROR : CM_NOMETHOD;
}
/* See call_plan.h. */
int cplan_subtree_missing(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;
  char self[160];
  if (nt_kind(nt, id) == NK_CallNode && cplan_missing(c, id, self, sizeof self, NULL) != CM_NONE) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (cplan_subtree_missing(c, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (cplan_subtree_missing(c, ids[k])) return 1;
  }
  return 0;
}
/* See call_plan.h. */
int cplan_program_missing(Compiler *c) {
  if (!c->missing_calls) {
    const NodeTable *nt = c->nt;
    char self[160];
    c->missing_calls = 1;
    for (int id = 0; id < nt->count && c->missing_calls == 1; id++)
      if (nt_kind(nt, id) == NK_CallNode && cplan_missing(c, id, self, sizeof self, NULL) != CM_NONE) c->missing_calls = 2;
  }
  return c->missing_calls == 2;
}
static int cplan_missing_in_order_1(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, v) != NK_CallNode) return cplan_subtree_missing(c, v) ? v : -1;
  char self[160];
  if (cplan_missing(c, v, self, sizeof self, NULL) != CM_NONE) return v;
  int argc = 0;
  const int *argv = call_args(nt, v, &argc);
  if (nt_safe_nav(nt, v) || nt_ref(nt, v, "block") >= 0 ||
      nt_argv_has(nt, argv, argc, NT_ARG_SPLAT | NT_ARG_KEYWORDS | NT_ARG_BLOCK | NT_ARG_FORWARDING))
    return cplan_subtree_missing(c, v) ? v : -1;
  for (int i = -1; i < argc; i++) {
    int m = cplan_missing_in_order(c, i < 0 ? nt_ref(nt, v, "receiver") : argv[i]);
    if (m >= 0) return m;
  }
  return -1;
}
/* See call_plan.h. */
int cplan_missing_in_order(Compiler *c, int v) {
  if (v < 0) return -1;
  if (v >= c->missing_order_n) {
    int n = c->nt->count > v ? c->nt->count : v + 1;
    int *g = realloc(c->missing_order, sizeof *g * (size_t)n);
    if (!g) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(g + c->missing_order_n, 0, sizeof *g * (size_t)(n - c->missing_order_n));
    c->missing_order = g;
    c->missing_order_n = n;
  }
  if (!c->missing_order[v]) c->missing_order[v] = cplan_missing_in_order_1(c, v) + 2;
  return c->missing_order[v] - 2;
}
int cplan_self_builtin(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *name = nt_str(nt, id, "name");
  if (!name || recv < 0 || !nt_int(nt, id, "self_call", 0)) return 0;
  NodeKind rk = nt_kind(nt, recv);
  int ci = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? cplan_const_recv_class(c, id, recv, NULL) : -1;
  return ci >= 0 && call_target(c, -1, ci, name, CT_CMETHOD, NULL) >= 0 &&
         call_target(c, id, ci, name, CT_CMETHOD, NULL) < 0 && is_kernel_module_function(name);
}
int cplan_kernel_row(Compiler *c, int id, const char *name) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  call_args(nt, id, &argc);
  int recv = nt_ref(nt, id, "receiver");
  const BuiltinOp *op = name ? bop_find(BOP_KERNEL, name, argc, nt_ref(nt, id, "block") >= 0) : NULL;
  if (!op) return 0;
  /* a call the compiler made (builtin_only: it never reaches a method of the
     program's) takes the row, whatever the program defines; an internal row
     answers no other call, and the program's own call of a name never takes
     it */
  if (recv < 0 && cplan_builtin_call(c, id)) return 1;
  if (op->flags & BOPF_INTERNAL) return 0;
  if (!cplan_kernel_form(c, id, name, 0)) return 0;
  /* an arm pinned to a version runs it, and its else arm (CT_PIN_BUILTIN)
     the row */
  int pin = ct_pin_of(c, id);
  if (pin != -1) return pin == CT_PIN_BUILTIN;
  int self = cplan_self_class(c, id);
  /* (a call whose self the record cannot place, in a def the analysis made, has its scope's class) */
  if (self < -1) {
    const Scope *in = comp_scope_of(c, id);
    self = in ? in->class_id : -1;
  }
  int own = recv < 0 || nt_kind(nt, recv) == NK_SelfNode;
  if (!cplan_dispatch_row(c, id) || (self >= 0 && own && call_target(c, id, self, name, 0, NULL) >= 0)) return 0;
  /* ...and no method of the program's answers the call by the plan (a descendant's override) */
  const CallPlan p = cplan_user_fresh(c, id);
  return p.mi < 0 && p.send_fallback < 0;
}

/* The native result view requires a row that is guaranteed to emit.
   Only emitters that take this row's call unconditionally qualify. */
static int cplan_handle_emitter(const BuiltinOp *op) {
  return op && op->result == TY_STRBUF && (op->emit == BOPE_TEMPLATE || op->emit == BOPE_KERNEL_STRING);
}

const BuiltinOp *cplan_builtin_handle_row(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0) return NULL;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    const Scope *s = comp_scope_of(c, id);
    if (!s || s->class_id < 0 || c->classes[s->class_id].is_value_type) return NULL;
    const Scope *site = comp_super_site(c, s);
    if (comp_super_shadow(c, site)) return NULL;
    const char *name = comp_super_name(c, cplan_super_start(c, site), site);
    if (comp_super_object_builtin(c, s, name, id) != SUPER_OBJ_ROW) return NULL;
    const BuiltinOp *op = comp_super_kernel_row(c, s, name, id);
    return cplan_handle_emitter(op) ? op : NULL;
  }
  if (k != NK_CallNode || cplan_pinned_vis(c, id) ||
      call_is_setter_assign(nt, id) || call_plain_argc(c, id) < 0) return NULL;
  const char *name = view_call_name(c, id);
  int recv = nt_ref(nt, id, "receiver");
  int kernel = recv < 0 || nt_kind(nt, recv) == NK_SelfNode || nt_str(nt, id, "send_blind");
  if (kernel) {
    const BuiltinOp *op = cplan_builtin_op_row(c, id, BOP_KERNEL, name, 0, NULL);
    if (cplan_handle_emitter(op) && cplan_kernel_row(c, id, name)) return op;
  }
  if (recv < 0) return NULL;
  TyKind rt = comp_ntype(c, recv), base;
  if (ty_is_object(rt) && comp_ary_is_str(c, ty_object_class(rt)) && comp_arysub_call(c, id, rt, &base)) rt = base;
  const BuiltinOp *op = rt == TY_POLY ? cplan_boxed_fallback_row(c, id, name, TY_STRBUF, NULL)
                                      : cplan_builtin_op_row(c, id, rt, name, 0, NULL);
  if (!cplan_handle_emitter(op)) return NULL;
  if (cplan_pinned_builtin(c, id) || cplan_builtin_call(c, id) || (g_poly_builtin_arm && g_pd_skip == id)) return op;
  if (rt == TY_POLY && (an_user_defines_or_reads(c, name) || ostruct_member_read(c, id, rt))) return NULL;
  return cplan_user(c, id).dispatch == CP_NONE ? op : NULL;
}

const char *cplan_self_text(Compiler *c, int id, int ci, char *buf, size_t cap) {
  int self = id >= 0 ? cplan_self_class(c, id) : -2;
  if (self >= 0 && self < c->nclasses) return cm_self_name(c, self, buf, cap);
  if (self == -1 || ci < 0) return cm_self_name(c, -1, buf, cap);
  if (comp_class_anonymous(c, ci)) return NULL;
  const char *cn = class_ruby_name(c, ci);
  snprintf(buf, cap, "an instance of %s", cn ? cn : "Object");
  return buf;
}

CallPlan cplan_user_in(Compiler *c, int id, int self_ci, int flags) {
  if (self_ci < 0) return cplan_user(c, id);
  CallPlan ctx;
  cplan_set(&ctx, -1, -1, UC_NONE, CP_NONE);
  if (id < 0 || id >= c->node_cap || self_ci >= c->nclasses || nt_kind(c->nt, id) != NK_CallNode)
    return ctx;
  if ((flags & CPX_TARGET) && cplan_builtin_call(c, id)) return ctx;
  const char *name = flags & CPX_TARGET ? view_call_name(c, id) : nt_str(c->nt, id, "name");
  int mi = name ? call_target(c, id, self_ci, name, 0, NULL) : -1;
  if (mi >= 0) {
    int form = flags & CPX_TARGET ? CP_DIRECT : cplan_dispatch_form(c, id, self_ci, name, 1);
    cplan_set(&ctx, mi, self_ci, UC_INST, form);
    ctx.chain = 1;
    return cplan_dep(ctx);
  }
  if (flags & CPX_TARGET) return ctx;
  /* a miss is no method, but a dispatch on the class still has a form: a
     method only descendants define takes the switch */
  if (!(flags & CPX_IE) && name) {
    cplan_set(&ctx, -1, self_ci, UC_INST, cplan_dispatch_form(c, id, self_ci, name, 0));
    ctx.chain = 1;
    return ctx;
  }
  if (flags & CPX_IE) {
#ifndef NDEBUG
    int tmp0 = g_tmp;
#endif
    cplan_resolve(c, id, &ctx);
#ifndef NDEBUG
    assert(g_tmp == tmp0);
#endif
  }
  return cplan_dep(ctx);
}

CallPlan cplan_user(Compiler *c, int id) {
  CallPlan res;
  if (id < 0 || id >= c->node_cap) { cplan_set(&res, -1, -1, UC_NONE, CP_NONE); return res; }
  int plain = cplan_plain_ctx(), renamed = 0;
  const char *nm = nt_str(c->nt, id, "name");
  const CallPlan *k = plain && c->cplan_memo ? cplan_table_get(&c->cplan_memo->user, id, nm, &renamed) : NULL;
  if (k) return cplan_dep(*k);
  if (renamed) plain = 0;
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  cplan_resolve(c, id, &res);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  /* (the memo asked after resolving: a resolution asks other calls'
     plans, which make and grow it) */
  if (plain) cplan_table_put(&cplan_memo(c)->user, id, res, nm);
  return cplan_dep(res);
}

CallPlan cplan_user_fresh(Compiler *c, int id) {
  CallPlan res;
  cplan_set(&res, -1, -1, UC_NONE, CP_NONE);
  if (id < 0 || id >= c->node_cap) return res;
  cplan_resolve(c, id, &res);
  return cplan_dep(res);
}

/* The class a constructor call builds: a constant receiver's, a local's
   that holds one class or a class body's self (class_recv_static_ci), or
   the class its method runs for, as inference types it (bare or
   `self.new` in a class method, `self.class.new` in an instance method).
   *own is set for the last kind, which a subclass receiving the method
   builds its own of. */
static int cplan_new_class(Compiler *c, int id, int *own) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, id, "receiver");
  NodeKind k = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  *own = 0;
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode || k == NK_LocalVariableReadNode) return class_recv_static_ci(c, r);
  Scope *s = comp_scope_of(c, id);
  if (k == NK_SelfNode && !(s && s->is_cmethod)) {
    int b = class_recv_static_ci(c, r);
    return b >= 0 && !comp_class_is_module(c, &c->classes[b]) ? b : -1;
  }
  int ci = -1;
  if (s && s->is_cmethod && (r < 0 || k == NK_SelfNode)) ci = s->class_id;
  else if (s && !s->is_cmethod && k == NK_CallNode && nt_ref(nt, r, "receiver") >= 0 &&
           nt_kind(nt, nt_ref(nt, r, "receiver")) == NK_SelfNode && nt_str(nt, r, "name") &&
           sp_streq(nt_str(nt, r, "name"), "class")) ci = s->class_id;
  if (ci < 0 || ci >= c->nclasses || comp_class_is_module(c, &c->classes[ci])) return -1;
  *own = 1;
  return ci;
}

/* Does method mi, which the chain of builtin class k holds for `initialize`,
   sit behind the builtin's own (C) initialize, so that CRuby's `new` never
   reaches it? Only a prepend on the builtin and the reopening's own def run
   ahead of it: an included module's, and Object's, Kernel's or BasicObject's,
   come after it. Object and its roots have no initialize of their own to hide
   behind, and a program class's chain holds only program methods. */
static int ct_behind_builtin_init(Compiler *c, int k, int mi) {
  const Scope *sc = &c->scopes[mi];
  if (!is_builtin_reopen(c->classes[k].name) || is_object_root(c->classes[k].name)) return 0;
  return sc->class_id != k || (sc->is_include_copy && !sc->is_prepend_copy);
}
/* The initialize methods a builtin `new` runs that builds class ci, or with
   own an instance of ci or of any subclass (into out, at most CPT_MAX;
   CPT_UNKNOWN past it): those the plan answers where the construction id
   runs (cplan_hook_target), the table's for id -1. */
static int ct_class_initializers(Compiler *c, int id, int ci, int own, int *out) {
  int n = 0;
  for (int k = ci; ci >= 0 && k < c->nclasses; k++) {
    if (k != ci && (!own || !is_descendant(c, k, ci))) continue;
    int mi = cplan_hook_target(c, id, k, "initialize", NULL), dup = 0;
    if (mi >= 0 && ct_behind_builtin_init(c, k, mi)) continue;
    for (int j = 0; j < n; j++) dup |= out[j] == mi;
    if (mi < 0 || dup) continue;
    if (n == CPT_MAX) return CPT_UNKNOWN;
    out[n++] = mi;
  }
  return n;
}
/* A builtin `new` (no program `new` answers it): the initialize methods it
   may run. A constructor's result can be discarded, so this resolves from
   its receiver, not from its result type.
   The class it builds is cplan_new_class's, with each subclass's for a
   `new` on the class its method runs for, which a subclass receiving the
   method builds its own of; a constant naming a builtin (or one the
   builtin class table lacks, is_untabled_native_class_name), one the
   program assigns no constant of its name to, runs none of the program's;
   any other receiver that may be a class (a variable's value of more than
   one class, a call's answer, a constant the program assigns, an untyped
   one) is no closed set. -2 for any other call. The methods a builtin
   `new` enters, never what it answers (cplan_entries). */
static int ct_new_initializers(Compiler *c, int id, int *out) {
  const NodeTable *nt = c->nt;
  if (!is_new_name(nt_str(nt, id, "name"))) return -2;
  int own, ci = cplan_new_class(c, id, &own), n = ci >= 0 ? ct_class_initializers(c, id, ci, own, out) : 0;
  int r = nt_ref(nt, id, "receiver");
  if (ci >= 0 || r < 0) return ci >= 0 ? n : -2;
  NodeKind k = nt_kind(nt, r);
  /* (an untabled builtin runs no program initialize only while reopening,
     subclassing, prepend, class_eval and define_method on it are refused:
     revisit when they are lifted) */
  if ((k == NK_ConstantReadNode || k == NK_ConstantPathNode) && !comp_const(c, const_key_at(c, r)) &&
      (const_seg_builtin(c, r) || (k == NK_ConstantReadNode && is_untabled_native_class_name(nt_str(nt, r, "name")))))
    return 0;
  TyKind rt = comp_ntype(c, r);
  return rt == TY_POLY || rt == TY_UNKNOWN || rt == TY_CLASS ? CPT_UNKNOWN : -2;
}

/* ---- cplan_targets: the user methods a call may reach ---- */

/* Held answers, per node id: g_ct_n[id] is CT_NONE (not asked), CPT_UNKNOWN
   or the count, with the methods in g_ct_pool at g_ct_off[id]. The index
   lists the scopes ordered by name, so a switch finds its same-named
   candidates without walking every scope; both are dropped together. */
enum { CT_NONE = -2 };
static int *g_ct_n, *g_ct_off, g_ct_cap;
static int *g_ct_pool, g_ct_npool, g_ct_poolcap;
static int *g_ct_idx, g_ct_nidx;
static Compiler *g_ct_c;
static unsigned g_ct_drops;   /* (cplan_targets_drops) */

static const Compiler *g_ct_sort_c;
static int ct_idx_cmp(const void *a, const void *b) {
  const char *x = g_ct_sort_c->scopes[*(const int *)a].name;
  const char *y = g_ct_sort_c->scopes[*(const int *)b].name;
  int d = strcmp(x, y);
  return d ? d : *(const int *)a - *(const int *)b;
}

void cplan_targets_drop(void) {
  g_ct_drops++;
  for (int i = 0; i < g_ct_cap; i++) g_ct_n[i] = CT_NONE;
  g_ct_npool = 0;
  g_ct_nidx = -1;
}
unsigned cplan_targets_drops(void) { return g_ct_drops; }

static void ct_index_build(Compiler *c) {
  free(g_ct_idx);
  g_ct_idx = (int *)malloc((size_t)(c->nscopes ? c->nscopes : 1) * sizeof *g_ct_idx);
  g_ct_nidx = 0;
  for (int k = 0; k < c->nscopes; k++)
    if (c->scopes[k].name) g_ct_idx[g_ct_nidx++] = k;
  g_ct_sort_c = c;
  qsort(g_ct_idx, (size_t)g_ct_nidx, sizeof *g_ct_idx, ct_idx_cmp);
}

/* Exception#message invokes #to_s when the class has no message override.
   Use the existing class candidates for both names so the share walk binds
   that implicit edge just as it binds an explicit call. */
static int ct_exception_text(Compiler *c, int id, int *out) {
  const char *name = nt_str(c->nt, id, "name");
  if (!name || cplan_builtin_call(c, id) ||
      !is_exception_message(name) || call_plain_argc(c, id) != 0 ||
      nt_ref(c->nt, id, "block") >= 0) return -2;
  int recv = nt_ref(c->nt, id, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  int cid = ty_is_object(rt) ? ty_object_class(rt) : -1;
  if (rt != TY_EXCEPTION && (cid < 0 || !class_is_exc_subclass(c, cid))) return -2;
  if (cid >= 0) {
    int nd = 0;
    comp_descendants(c, cid, &nd);
    if (nd == 1) {
      int mi = is_to_s_name(name) ? -1 : cplan_implied_target(c, id, recv, cid, "message", CIT_TABLE, NULL);
      if (mi < 0) mi = cplan_implied_target(c, id, recv, cid, "to_s", CIT_TABLE, NULL);
      if (mi >= 0) out[0] = mi;
      return mi >= 0;
    }
  }
  int n = 0;
  for (int pass = is_to_s_name(name) ? 1 : 0; pass < 2; pass++) {
    int nc = 0;
    const PolyCand *cs = comp_poly_candidates(c, pass ? "to_s" : "message", &nc);
    for (int i = 0; i < nc; i++) {
      int k = cs[i].cls;
      if (!class_is_exc_subclass(c, k) && !class_is_exc_reopen(c, k)) continue;
      if (cid >= 0 && k != cid && !is_descendant(c, k, cid)) continue;
      int mi = is_to_s_name(name) ? -1 : cplan_implied_target(c, id, recv, k, "message", CIT_TABLE, NULL);
      if (mi < 0) mi = cplan_implied_target(c, id, recv, k, "to_s", CIT_TABLE, NULL);
      if (mi < 0) continue;
      int j = 0;
      while (j < n && out[j] != mi) j++;
      if (j < n) continue;
      if (n == CPT_MAX) return CPT_UNKNOWN;
      out[n++] = mi;
    }
  }
  return n;
}

/* A receiverless call in an instance method of a class whose chain has no
   method of the name, but a descendant's does (a template method: the base
   calls `header`, each subclass defines it): codegen dispatches it on
   self's class over those descendants (emit_call_implicit_self_arms), so
   the call reaches each of their methods. -2 for any other call. */
static int ct_template_self(Compiler *c, int id, int *out) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || nt_ref(nt, id, "receiver") >= 0 || cplan_builtin_call(c, id)) return -2;
  Scope *sc = comp_scope_of(c, id);
  int cid = sc ? sc->class_id : -1;
  MethodLookup aml = { .def_class = -1 };
  if (!sc || sc->is_cmethod || cid < 0 || cid >= c->nclasses || call_target(c, id, cid, name, 0, &aml) >= 0)
    return -2;
  /* (an attribute of the chain is the call's own member: emit_implicit_self_member) */
  size_t len = strlen(name);
  if (aml.kind == ML_ATTR) return -2;
  if (len > 1 && name[len - 1] == '=') {
    char base[256];
    if (len - 1 >= sizeof base) return -2;
    memcpy(base, name, len - 1);
    base[len - 1] = '\0';
    if (call_attr_target(c, id, cid, base, CT_WRITER, &aml)) return -2;
  }
  int n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid || !is_descendant(c, k, cid)) continue;
    int mi = call_target(c, id, k, name, 0, NULL), seen = 0;
    if (mi < 0) continue;
    for (int j = 0; j < n && !seen; j++) seen = out[j] == mi;
    if (seen) continue;
    if (n == CPT_MAX) return CPT_UNKNOWN;
    out[n++] = mi;
  }
  return n > 0 ? n : -2;
}

static int ct_resolve(Compiler *c, int id, int *out) {
  int text = ct_exception_text(c, id, out);
  if (text != -2) return text;
  const CallPlan plan = cplan_user_fresh(c, id);
  if (plan.mi < 0 || plan.dispatch == CP_REFUSE) {
    int tmpl = plan.dispatch == CP_REFUSE ? -2 : ct_template_self(c, id, out);
    if (tmpl != -2) return tmpl;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv >= 0 && comp_ntype(c, recv) == TY_UNKNOWN) return CPT_UNKNOWN;
    return 0;
  }
  int n = 0;
  out[n++] = plan.mi;
  if (plan.dispatch < CP_SWITCH) return n;
  const char *name = plan.delegate ? plan.delegate : c->scopes[plan.mi].name;
  if (!name) return n;
  if (g_ct_nidx < 0) ct_index_build(c);
  PolyBlockMembers blk;
  blk.state = 0;
  int lo = 0, hi = g_ct_nidx;
  g_ct_sort_c = c;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(c->scopes[g_ct_idx[mid]].name, name) < 0) lo = mid + 1;
    else hi = mid;
  }
  for (int i = lo; i < g_ct_nidx && sp_streq(c->scopes[g_ct_idx[i]].name, name); i++) {
    int k = g_ct_idx[i];
    if (k == plan.mi || !cplan_virtual_member(c, id, &plan, k, &blk)) continue;
    if (n == CPT_MAX) return CPT_UNKNOWN;
    out[n++] = k;
  }
  /* --share-strings: the methods named by the call, each class's own
     (comp_poly_candidates follows an alias, `alias get text`, whose method
     has another name than the call's and the first target's) */
  const char *cn = plan.delegate ? plan.delegate : nt_str(c->nt, id, "name");
  /* (not for a text conversion, #to_s or #inspect: the boxed to_s route
     asks each class the receiver can hold for its own method, and adding
     every class's here widened the facts of the bundled library's calls,
     a Set's #inspect over its elements, for nothing) */
  if (cn && !is_text_conversion(cn)) {
    int np = 0;
    const PolyCand *pc = comp_poly_candidates(c, cn, &np);
    for (int i = 0; i < np; i++) {
      int k = pc[i].mi, seen = 0;
      /* (only a method of another name than the first target's: the loop
         above has the methods of that name) */
      if (k < 0 || !c->scopes[k].name || sp_streq(c->scopes[k].name, name)) continue;
      for (int j = 0; j < n && !seen; j++) seen = out[j] == k;
      if (seen || !cplan_virtual_member(c, id, &plan, k, &blk)) continue;
      if (n == CPT_MAX) return CPT_UNKNOWN;
      out[n++] = k;
    }
  }
  return n;
}

/* A poly protocol keeps each class's own operator arm ahead of the
   builtin delegation. Those arms may answer values other than Boolean. */
TyKind cplan_poly_operator_ret(Compiler *c, int id) {
  const char *name = nt_str(c->nt, id, "name");
  if (!name || is_equality_name(name) || !(is_eq_op(name) || is_cmp_op(name))) return TY_UNKNOWN;
  int recv = nt_ref(c->nt, id, "receiver"), argc = 0;
  call_args(c->nt, id, &argc);
  if (argc != 1 || recv < 0 || comp_ntype(c, recv) != TY_POLY) return TY_UNKNOWN;
  const CallPlan p = cplan_user_fresh(c, id);
  if (p.via != UC_POLY || p.mi < 0) return TY_UNKNOWN;
  int n = 0, found = 0;
  const PolyCand *pcs = comp_poly_candidates_at(c, id, name, &n);
  for (int i = 0; i < n; i++) {
    int mi = pcs[i].mi < 0 ? -1 : cplan_cand_target(c, id, name, &pcs[i], 0);
    if (mi < 0 || c->classes[pcs[i].cls].is_native_class) continue;
    found = 1;
    if (c->scopes[mi].ret != TY_BOOL) return TY_POLY;
  }
  /* the builtin #=== beside them is a Proc's or a Method's call for those
     receivers, which answers any value (`->(x) { x * 128 } === 2` is 256) */
  if (found && sp_streq(name, "===")) return TY_POLY;
  return found ? TY_BOOL : TY_UNKNOWN;
}

/* Is class method scope mi of class p a copy of an ancestor's method (the
   one an inherited method reading class-level ivars gets per subclass, see
   specialize_inherited_cls_new) rather than a method p's own body defines?
   The copy keeps the original's def node, which an ancestor of p holds: the
   original itself shares the node with the copies its subclasses get. */
static int cmethod_is_inherited_copy(Compiler *c, int mi, int p) {
  for (int o = 1; o < c->nscopes; o++) {
    int oc = c->scopes[o].class_id;
    if (o != mi && c->scopes[o].is_cmethod && c->scopes[o].def_node == c->scopes[mi].def_node &&
        oc >= 0 && oc != p && is_descendant(c, p, oc)) return 1;
  }
  return 0;
}
/* Does class k answer call `call`'s `name` by a `class << self` accessor?
   The lookup walks k's singleton class and then its superclasses', and the
   first class that declares the accessor or defines a class method of the
   name answers: the accessor wins over a `def self.label` in a superclass,
   and a class method defined below the accessor's class is ahead of it. A
   class's lists hold the accessors its ancestors declare as well, so what a
   class declares is read from its declarations (comp_sg_accessor_own), the
   reader `x` and the writer `x=` apart: a class can declare one half and
   inherit the other. A per-subclass copy of an ancestor's class method is
   the ancestor's. */
int cplan_cls_own_sg_answers(Compiler *c, int call, int k, const char *name, int argc) {
  /* (a class that declares no singleton accessor answers by none: asked of every class at every
     site, so the cheap test comes first) */
  if (!c->classes[k].nsg_readers && !c->classes[k].nsg_writers) return 0;
  char base[256];
  int setter = setter_base_name(name, base, sizeof base);
  if (setter ? argc != 1 : argc != 0) return 0;
  ClassInfo *cls = &c->classes[k];
  if (is_builtin_reopen(cls->name)) return 0;
  const char *rn = setter ? base : comp_resolve_cm_alias_at(c, k, name, NULL);
  if (!rn) rn = name;
  if (setter ? !comp_is_sg_writer(cls, base) : !comp_is_sg_reader(cls, rn)) return 0;
  char wn[260];
  /* (base is written for a setter only) */
  if (setter) snprintf(wn, sizeof wn, "%s=", base);
  for (int p = k, guard = 0; p >= 0 && guard < 256; p = c->classes[p].parent, guard++) {
    ClassInfo *pc = &c->classes[p];
    int pmi = call_target(c, call, p, name, CT_OWN | CT_CMETHOD, NULL);
    if (pmi >= 0 && !cmethod_is_inherited_copy(c, pmi, p)) return 0;
    if ((setter ? comp_is_sg_writer(pc, base) : comp_is_sg_reader(pc, rn)) &&
        comp_sg_accessor_own(c, p, setter ? wn : rn)) return 1;
    if (p == pc->parent) break;
  }
  return call_target(c, call, k, name, CT_CMETHOD, NULL) < 0;
}
/* Whether a class can be a run-time class value: it is named by reached
   code, or it is an ancestor of one that is (`superclass` reaches it). */
int cplan_class_value_possible(Compiler *c, int k) {
  if (k < 0 || !c->classes[k].unreferenced) return 1;
  for (int j = 0; j < c->nclasses; j++) {
    if (c->classes[j].unreferenced) continue;
    for (int x = c->classes[j].parent; x >= 0; x = c->classes[x].parent)
      if (x == k) return 1;
  }
  return 0;
}
/* See call_plan.h. The arms of the class-valued dispatch emit_call_class_arms writes for a class
   method called on a Class value held in a variable, a parameter or a constant that names no one
   class: each class a value may hold that answers the name by a class method (not by a
   `class << self` accessor, whose own arms the emitter writes), and that class method. *lower says
   how the call is lowered: as the switch of those arms (no block at the call, and no candidate
   that yields, takes a block or has a rest the arm cannot pack), or, where a candidate takes the
   block as a proc, by the proc-form dispatch (CLS_LOWER_PROC; the emitter takes it on a boxed
   receiver). */
int cplan_cls_value_arms(Compiler *c, int id, const char *name, int argc, int **cls, int **mi, unsigned *lower) {
  int recv = nt_ref(c->nt, id, "receiver");
  *cls = *mi = NULL;
  if (lower) *lower = 0;
  /* (only a Class-typed receiver has class arms, and a constant that names one class is a direct
     call: no arm) */
  NodeKind rk = recv >= 0 ? nt_kind(c->nt, recv) : NK_NONE;
  if (recv < 0 || comp_ntype(c, recv) != TY_CLASS || !class_recv_is_dynamic(c, recv)) return 0;
  if ((rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) && const_node_class_index(c, recv) >= 0) return 0;
  int n = 0, veto = nt_ref(c->nt, id, "block") >= 0;
  unsigned low = 0;
  if (!(*cls = malloc(sizeof(int) * (size_t)(c->nclasses + 1))) || !(*mi = malloc(sizeof(int) * (size_t)(c->nclasses + 1)))) {
    fprintf(stderr, "spinel: out of memory\n");
    exit(1);
  }
  for (int k = 0; k < c->nclasses; k++) {
    if (cplan_cls_own_sg_answers(c, id, k, name, argc)) continue;
    int kmi = call_target(c, id, k, name, CT_CMETHOD, NULL);
    if (kmi < 0) continue;
    const Scope *ks = &c->scopes[kmi];
    if (scope_proc_form_of(c, kmi) >= 0 || (ks->blk_param && ks->blk_param[0] && !ks->yields)) low |= CLS_LOWER_PROC;
    if (!comp_cv_arm_class(c, k)) continue;
    /* (a candidate that yields, takes a block, or has a rest the arm cannot pack has no arm the
       switch can build: it vetoes the switch, as one the call gets wrong in arity does not) */
    if (ks->yields || (ks->blk_param && ks->blk_param[0]) || (ks->rest_idx >= 0 && !rest_packable_arm(c, (Scope *)ks))) veto = 1;
    /* (a class no reached code names, and no named class descends from, is no class value: its
       singleton copy is not compiled, so an arm calling it would dangle; a method copied into
       each class a value can hold is not emitted itself) */
    if (!cplan_class_value_possible(c, k) || comp_scope_copied_away(c, kmi)) continue;
    (*cls)[n] = k;
    (*mi)[n++] = kmi;
  }
  if (!veto) low |= CLS_LOWER_SWITCH;
  if (lower) *lower = low;
  return n;
}
/* Does method mi take pos_argc positional arguments and the keyword hash
   kwh (-1 for none): the judgement of its dispatch arm and of a call's
   emission (poly_arm_count) */
static int cplan_def_takes(Compiler *c, int mi, int kwh, int pos_argc) {
  char exp[256];
  return poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, -1, exp, sizeof exp) >= 0;
}

int cplan_def_takes_argc(Compiler *c, int mi, int argc) {
  const Scope *sc = &c->scopes[mi];
  return sc->def_node < 0 || nt_kind(c->nt, sc->def_node) != NK_DefNode || cplan_def_takes(c, mi, -1, argc);
}

int cplan_cbody_direct(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || id >= c->node_cap || !c->node_cbody || c->node_cbody[id] < 0 || nt_kind(nt, id) != NK_CallNode) return 0;
  const Scope *s = comp_scope_of(c, id);
  int r = nt_ref(nt, id, "receiver");
  if ((s && s->name) || (r >= 0 && nt_kind(nt, r) != NK_SelfNode)) return 0;
  const CallPlan p = cplan_user(c, id);
  return p.mi >= 0 && p.via == UC_CMETH && p.dispatch == CP_DIRECT && p.owner_ci == c->node_cbody[id];
}

int cplan_target_binds(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  if (id < 0 || mi < 0 || mi >= c->nscopes || nt_kind(nt, id) != NK_CallNode) return 1;
  int args = nt_ref(nt, id, "arguments"), argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  /* a trailing keyword hash is judged as the arm judges it: taken as
     keywords, refused, or one more positional Hash (kw_plan) */
  int kwh = nt_argv_kwh(nt, argv, argc);
  int pos_argc = kwh >= 0 ? argc - 1 : argc;
  for (int a = 0; a < pos_argc; a++) {
    NodeKind k = nt_kind(nt, argv[a]);
    if (k == NK_SplatNode || k == NK_KeywordHashNode || k == NK_ForwardingArgumentsNode ||
        k == NK_BlockArgumentNode)
      return 1;
  }
  return cplan_def_takes(c, mi, kwh, pos_argc);
}

int cplan_cls_arm_takes(Compiler *c, int id, int mi, int argc, int splat) {
  const Scope *s = &c->scopes[mi];
  if (splat) return s->rest_idx >= 0 || argc - splat <= s->nparams;
  return cplan_target_binds(c, id, mi);
}

/* See call_plan.h. ct_resolve answers CPT_UNKNOWN only for a member past
   CPT_MAX or for a receiver of type unknown that no plan resolves. */
int cplan_targets_unknown_why(Compiler *c, int id, int cap) {
  int tmp[CPT_MAX];
  if (cplan_targets(c, id, tmp, cap > CPT_MAX ? CPT_MAX : cap) != CPT_UNKNOWN) return CPT_WHY_NONE;
  if (g_ct_n[id] >= 0) return CPT_WHY_CAP;   /* resolved, past the caller's cap */
  const CallPlan p = cplan_user_fresh(c, id);
  int recv = nt_ref(c->nt, id, "receiver");
  if ((p.mi < 0 || p.dispatch == CP_REFUSE) && recv >= 0 && comp_ntype(c, recv) == TY_UNKNOWN)
    return CPT_WHY_RECV_UNTYPED;
  return CPT_WHY_CAP;
}

int cplan_targets(Compiler *c, int id, int *out, int cap) {
  if (id < 0 || id >= c->node_cap) return 0;
  if (g_ct_c != c || g_ct_cap < c->node_cap) {
    int ncap = c->node_cap;
    int fresh = g_ct_c != c;
    g_ct_n = (int *)realloc(g_ct_n, (size_t)ncap * sizeof *g_ct_n);
    g_ct_off = (int *)realloc(g_ct_off, (size_t)ncap * sizeof *g_ct_off);
    for (int i = fresh ? 0 : g_ct_cap; i < ncap; i++) g_ct_n[i] = CT_NONE;
    g_ct_cap = ncap;
    if (fresh) { g_ct_npool = 0; g_ct_nidx = -1; g_ct_drops++; }
    g_ct_c = c;
  }
  if (g_ct_n[id] == CT_NONE) {
    int tmp[CPT_MAX];
    int n = ct_resolve(c, id, tmp);
    if (n > 0) {
      if (g_ct_npool + n > g_ct_poolcap) {
        g_ct_poolcap = (g_ct_npool + n) * 2;
        g_ct_pool = (int *)realloc(g_ct_pool, (size_t)g_ct_poolcap * sizeof *g_ct_pool);
      }
      memcpy(g_ct_pool + g_ct_npool, tmp, (size_t)n * sizeof *tmp);
      g_ct_off[id] = g_ct_npool;
      g_ct_npool += n;
    }
    g_ct_n[id] = n;
  }
  int n = g_ct_n[id];
  if (n > cap) return CPT_UNKNOWN;
  if (n > 0) memcpy(out, g_ct_pool + g_ct_off[id], (size_t)n * sizeof *out);
  /* (the targets are what the asker reads next: their values, their parameters) */
  for (int i = 0; i < n; i++) IMEMO_DEP_SCOPE(out[i]);
  return n;
}

/* A super, bare or not, in a program `new` that reaches the builtin
   Class#new (no program `new` above the method): the initialize methods it
   runs, those of the method's class and its subclasses, which self is one
   of. Open (CPT_UNKNOWN) for a module's `new`, which a class's singleton
   takes by extend or prepend, and for a method under a prepend chain,
   whose super the chain decides. -2 for any other super. */
static int ct_super_new_initializers(Compiler *c, int id, int *out) {
  Scope *s = comp_scope_of(c, id);
  if (!s || s->class_id < 0 || !is_new_name(comp_scope_def_name(c, s))) return -2;
  if (cplan_super_target(c, id, s, NULL) >= 0) return -2;
  int mod = comp_class_is_module(c, &c->classes[s->class_id]);
  if (mod ? !s->is_cmethod : comp_super_shadow(c, comp_super_site(c, s)) != NULL) return CPT_UNKNOWN;
  return !mod && s->is_cmethod ? ct_class_initializers(c, -1, s->class_id, 1, out) : -2;
}
/* A `new` call id that a program `new` answers for some class its
   receiver may be: those methods (its cplan_targets), and the initialize
   methods of each class it may be that takes the builtin `new` instead.
   The classes are cplan_new_class's (with its subclasses for the class a
   method runs for) or, for a class value, those its class facts list
   (cls_fact_node); CPT_UNKNOWN where they cannot be listed. */
static int ct_mixed_new_entries(Compiler *c, int id, int *out) {
  int n = cplan_targets(c, id, out, CPT_MAX), own, ci = cplan_new_class(c, id, &own);
  if (n < 0) return n;
  int r = nt_ref(c->nt, id, "receiver"), cls[CPT_MAX], ncls = 0;
  if (ci >= 0) {
    for (int k = ci; k < c->nclasses; k++)
      if (k == ci || (own && is_descendant(c, k, ci))) {
        if (ncls == CPT_MAX) return CPT_UNKNOWN;
        cls[ncls++] = k;
      }
  } else if (r >= 0) {
    const ClsSet *cs = cls_fact_node(c, r);
    if (!cs || cs->all || (cs->bv && cs->bc)) return CPT_UNKNOWN;   /* (a builtin part that is no Class object makes nothing, #11154) */
    /* (a member past nclasses is a class value: its class methods answer) */
    for (int m = cls_set_next(cs, -1); m >= 0; m = cls_set_next(cs, m)) {
      if (m < c->nclasses) continue;
      if (ncls == CPT_MAX) return CPT_UNKNOWN;
      cls[ncls++] = m - c->nclasses;
    }
  }
  for (int i = 0; i < ncls; i++) {
    int k = cls[i], dup = 0;
    if (comp_class_is_module(c, &c->classes[k]) || call_target(c, id, k, "new", CT_CMETHOD, NULL) >= 0) continue;
    /* (the initialize the class's arm of the site runs: the entries are
       what the arms call, so reachability keeps the versions they name) */
    int mi = cplan_new_arm_initialize(c, id, k);
    for (int q = 0; q < n && !dup; q++) dup = out[q] == mi;
    if (mi < 0 || dup) continue;
    if (n == CPT_MAX) return CPT_UNKNOWN;
    out[n++] = mi;
  }
  return n;
}
/* A `new` call id's entries: a builtin one's (ct_new_initializers), one a
   program `new` answers for some class (ct_mixed_new_entries), into out;
   or a super's that reaches the builtin one (ct_super_new_initializers);
   -2 for any other call, whose entries are its targets. */
static int ct_new_entries(Compiler *c, int id, int *out) {
  if (id < 0 || id >= c->node_cap) return -2;
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) return ct_super_new_initializers(c, id, out);
  if (k != NK_CallNode || !is_new_name(nt_str(c->nt, id, "name"))) return -2;
  return cplan_user_fresh(c, id).dispatch != CP_NONE ? ct_mixed_new_entries(c, id, out) : ct_new_initializers(c, id, out);
}

int cplan_entries(Compiler *c, int id, int *out, int cap) {
  int tmp[CPT_MAX], n = ct_new_entries(c, id, tmp);
  if (n == -2) return cplan_targets(c, id, out, cap);
  if (n > cap) return CPT_UNKNOWN;
  if (n > 0) memcpy(out, tmp, (size_t)n * sizeof *tmp);
  /* (the entries are what the asker reads next: their parameters) */
  for (int i = 0; i < n; i++) IMEMO_DEP_SCOPE(out[i]);
  return n;
}

int cplan_entries_bound_names(Compiler *c, int id, const char *names[2]) {
  if (id < 0 || id >= c->node_cap) return 0;
  NodeKind k = nt_kind(c->nt, id);
  const Scope *s = k == NK_SuperNode || k == NK_ForwardingSuperNode ? comp_scope_of(c, id) : NULL;
  /* (a super's name is its method's) */
  const char *nm = k == NK_CallNode ? nt_str(c->nt, id, "name") : s ? comp_scope_def_name(c, s) : NULL;
  return nm ? cplan_name_bound_names(nm, names) : 0;
}
int cplan_name_bound_names(const char *name, const char *names[2]) {
  int n = 0;
  names[n++] = name;
  /* (the builtin `new` runs the initialize of the class it builds. The
     bound takes `new` by its name: an alias of it, or a class-side
     method_missing that builds, is refused today, else its bound would
     miss initialize) */
  if (is_new_name(name)) names[n++] = "initialize";
  return n;
}

/* See call_plan.h. A boxed receiver's cplan_targets lists every class's
   method of the name; its dispatch has an arm only for the classes its
   receiver can hold (comp_poly_candidates_at) and that fit the call, one
   raising ArgumentError for a count the method refuses (PA_ARITY). A send
   on a boxed receiver also names its send_fallback, the top-level method
   it runs for a receiver whose class does not define the name
   (emit_send_blind), which neither list holds. */
int cplan_emitted_targets(Compiler *c, int id, int *out, int cap) {
  const CallPlan p = cplan_user_fresh(c, id);
  int fb = p.send_fallback, n = 0;
  if (fb >= 0) {
    if (cap < 1) return CPT_UNKNOWN;
    out[n++] = fb;
  }
  if (p.via != UC_POLY || p.mi < 0 || p.dispatch == CP_REFUSE) {
    int m = cplan_entries(c, id, out + n, cap - n);
    if (m == CPT_UNKNOWN) return m;
    for (int i = n; i < n + m; i++)
      if (out[i] == fb) { out[i] = out[n + m - 1]; m--; break; }
    return n + m;
  }
  const PolyPlan pp = cplan_poly_fresh(c, id);
  for (int i = 0; i < pp.n; i++) {
    int mi = pp.arm[i].mi, seen = 0;
    for (int j = 0; j < n && !seen; j++) seen = out[j] == mi;
    if (mi < 0 || seen) continue;
    if (n == cap) return CPT_UNKNOWN;
    out[n++] = mi;
  }
  return n;
}

/* A boxed receiver whose own targets include a class method. The receiver
   walk bounds instances only: a successful bound cannot contain a Class.
   A call with a literal block is not one: its builtin iteration arm types
   the result, and joining a class method's return made it poly, which that
   arm does not build, so an Array's `map { }` raised NoMethodError. */
int cplan_boxed_cmethod(Compiler *c, int id, const char *name) {
  int blk = nt_ref(c->nt, id, "block");
  if (blk >= 0 && nt_kind(c->nt, blk) == NK_BlockNode) return 0;
  int ncc = 0;
  comp_cmethod_candidates(c, name, &ncc);
  if (!ncc) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_POLY) return 0;
  int n = 0;
  if (poly_recv_classes(c, id, &n)) return 0;
  int targets[CPT_MAX];
  n = cplan_targets(c, id, targets, CPT_MAX);
  if (n == CPT_UNKNOWN) return 1;
  for (int i = 0; i < n; i++)
    if (c->scopes[targets[i]].is_cmethod) return 1;
  return 0;
}

/* ---- CP_REFUSE ---- */

/* The positional-argument count of a CallNode (0 when it has none). */
static int cplan_argc(const NodeTable *nt, int id) {
  int a = nt_ref(nt, id, "arguments");
  int n = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &n);
  return n;
}

/* Every argument of call `id` is a constant naming one of the program's
   modules: a boxed receiver's `prepend` is then Module#prepend, not String's. */
static int cplan_module_args(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int a = nt_ref(nt, id, "arguments"), n = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &n) : NULL;
  if (n < 1 || !av) return 0;
  for (int i = 0; i < n; i++) {
    NodeKind k = nt_kind(nt, av[i]);
    int ci = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? const_node_class_index(c, av[i]) : -1;
    if (ci < 0 || !comp_class_is_module(c, &c->classes[ci])) return 0;
  }
  return 1;
}

/* A call to something spinel deliberately does not support (docs/limitations.md),
   by the call node alone: the documented limit's message, or NULL. Without
   it the call falls through to the generic diagnostic, which dumps node ids
   and argument types rather than naming the feature -- or, for a name the
   runtime happens to reach, to a NoMethodError that reads like an
   implementation gap instead of a documented limit. A call that reaches a
   method the program defines is left alone: it is that method, not the
   builtin, and each limit names how that is told (limit_exempt). *stop is
   0 when the node names no limit at all, so a limit further down its
   receiver chain is the one to report (diagnose_unsupported_call), and 1
   when the node itself settles it. The message may be a static buffer the
   next call reuses. #2652 / #2667 / #2668 */
/* `s.method(:<<)`, `s.method(:upcase!)`: a Method bound to a String's in-place
   mutator. The synthesized wrapper (`def __bam_N(__bam_r, ...) =
   __bam_r << ...`) appends to its receiver parameter, which is the shared
   handle, while the Method is bound to the String's plain value: a call
   read the value as the handle and crashed, and with the handle it would
   change a copy. The mutators are an_str_mutator_name's: the builtin rows'
   (bop_name_mutates) and the bang methods. */
static int cplan_str_method_mutator(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !is_method_obj_call(c, id)) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (rt != TY_STRING && rt != TY_STRBUF) return 0;
  /* the `__bam_` wrapper a `.method(:sym)` was retargeted at names its builtin */
  int mi = method_obj_target_mi(c, id);
  int dn = mi >= 0 ? c->scopes[mi].def_node : -1;
  const char *sym = dn >= 0 ? nt_str(nt, dn, "bam_sym") : NULL;
  return sym && an_str_mutator_name(sym);
}

static int cplan_reachable(Compiler *c, int id);

/* A computed instance_variable_get or _set on a boxed receiver reads or
   writes by name over the classes the receiver may be (emit_poly_ivar_call,
   #8219); a class that defines its own method of that name takes the
   program's, which that dispatch cannot call, so a program with such a
   class keeps the refusal for the boxed shape. A typed receiver of such a
   class binds to the method as any call does. */
static int cplan_computed_ivar_get(Compiler *c, int id, const char *name) {
  int set = sp_streq(name, "instance_variable_set");
  if (!set && !sp_streq(name, "instance_variable_get")) return 0;
  if (!cplan_reachable(c, id)) return 0;
  const NodeTable *nt = c->nt;
  int args = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac != (set ? 2 : 1) || nt_kind(nt, av[0]) == NK_SymbolNode || nt_kind(nt, av[0]) == NK_StringNode) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (rt != TY_POLY && rt != TY_EXCEPTION) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (c->classes[k].instantiated && !is_builtin_reopen(c->classes[k].name) &&
        call_target(c, id, k, name, 0, NULL) >= 0) return 1;
  return 0;
}

/* How a documented limit tells a call that reaches a method the program
   defines (which answers it, not the builtin) from one that reaches the
   builtin it limits. Each limit names one, and limit_exempt applies it. */
enum {
  LX_NONE,          /* the call is the builtin's, whatever the program defines */
  LX_NAME,          /* a method of the name anywhere in the program */
  LX_RECV_CMETHOD,  /* a class method of the name in the constant receiver's chain */
  LX_BINDING,       /* the call plan binds the call to a user method */
};

static int limit_exempt(Compiler *c, int id, int lx) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  switch (lx) {
  case LX_NAME:
    /* a `refine` with no receiver that runs on main is no Module#refine (main has none: NoMethodError), as
       scan_prologue_features reads it: the limit is for a refine a module can run */
    return diag_user_defines(c, id, name) ||
           (sp_streq(name, "refine") && nt_ref(nt, id, "receiver") < 0 && cplan_runs_on_main(c, id));
  case LX_RECV_CMETHOD: {
    int recv = nt_ref(nt, id, "receiver");
    NodeKind rk = nt_kind(nt, recv);
    int ci = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode
               ? const_node_class_index(c, recv) : -1;
    return ci >= 0 && call_target(c, id, ci, name, CT_CMETHOD, NULL) >= 0;
  }
  case LX_BINDING: {
    /* a switch over every class with a class method of the name (a class
       value, bare or boxed) leaves the classes without one on the builtin */
    const CallPlan p = cplan_user_fresh(c, id);
    return p.dispatch != CP_NONE && !p.by_name &&
           !(p.via == UC_POLY && p.mi >= 0 && c->scopes[p.mi].is_cmethod);
  }
  }
  return 0;
}

/* A `class << x` in a method's body on a class a Struct.new or Data.define
   makes at its write: x a constant the record resolves to that class, or
   self in a class method of it. CRuby defines the body's methods each time
   that method runs; the walk files them on the class's singleton from the
   start, and the record holds no version of them to order the calls
   against, so a call or respond_to? before the method runs (or in a
   program that never runs it) would find them. Refused, reached or not,
   where the program may ask the class for a method the body makes. */
int cplan_record_sclass_early(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_SingletonClassNode || elab_body_def(c, id) < 0) return 0;
  int ex = nt_ref(nt, id, "expression"), ci = -1;
  if (nt_kind(nt, ex) == NK_SelfNode) {
    const Scope *s = comp_scope_of(c, id);
    if (s && s->is_cmethod) ci = s->class_id;
  }
  else ci = elab_class_ref_at(c, ex, NULL);
  if (ci < 0 || ci >= c->nclasses || class_def_record(nt, c->classes[ci].def_node) < 0) return 0;
  /* asked: a call of its name (a Symbol's block among them) or a
     reflection reader naming it (elab_method_asked); a body that does
     more than def is taken as asked */
  int body = nt_ref(nt, id, "body"), n = 0;
  const int *st = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  if (body >= 0 && !st) return 1;
  for (int i = 0; i < n; i++) {
    const char *nm = nt_kind(nt, st[i]) == NK_DefNode ? nt_str(nt, st[i], "name") : NULL;
    if (!nm || an_calls_named_first(c, nm) >= 0 || elab_method_asked(c, nm)) return 1;
  }
  return 0;
}

/* Does call id's receiver, an object of a class of the program's, answer
   `name` with a member or method of its class's own (a Struct member named
   `methods`), so that the call is no list of methods? */
static int cplan_recv_own_member(Compiler *c, int id, const char *name) {
  int r = nt_ref(c->nt, id, "receiver");
  TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
  return ty_is_object(rt) && (call_answers(c, -1, ty_object_class(rt), name, 0, NULL));
}

/* Does presence query id (is_method_presence_query, kind 1) ask about a
   named entry whose name answers before a version ran, of a history
   holding a version marked cond_lone (a lone def under an `if` the
   lowering leaves to the record, a define_method a Struct or Data block
   may not run): a name not literal or of such an entry (cond_def_refl)? */
/* Does the name hold a history by count, whose versions carry the mark (ver_named_make) in place of an entry? */
static int ver_bycount_named(Compiler *c, const char *name) {
  for (int s = name && c->nver_bycount ? an_scopes_named_first(c, name) : -1; s >= 0; s = an_scopes_named_next(c, name, s))
    if (nt_int(c->nt, c->scopes[s].def_node, "cond_def_bycount", 0)) return 1;
  return 0;
}
static int ver_refl_named(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments"), an = 0;
  if (!c->nver_refl && !c->nver_refl_priv) return 0;
  if (args >= 0) nt_arr(c->nt, args, "arguments", &an);
  const char *q = an ? method_sym_arg(c, id) : NULL;
  for (int i = 0; an && q && i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == VER_ENTRY_HISTORY && e->scope >= 0 && nt_int(c->nt, c->scopes[e->scope].def_node, "cond_def_refl", 0) && sp_streq(e->name, q))
      return 1;
  }
  return (an && !q) || ver_bycount_named(c, q);
}
/* Is scope s a method that answers before its branch has run: a named
   entry whose name reflection does not answer (cond_def_refl), or the
   dispatcher a lone def under a run-time condition is lowered to
   (cond_lone_disp)? */
static int ver_cond_scope(Compiler *c, const Scope *s) {
  return s->def_node >= 0 && (nt_int(c->nt, s->def_node, "cond_def_refl", 0) ||
                              (nt_int(c->nt, s->def_node, "cond_lone_disp", 0) && !nt_int(c->nt, s->def_node, "cond_all_arms", 0)));
}
/* Does a class's chain (its class side when sg) find, for a method of a
   conditional scope, such a scope, so that a list of its methods names a
   method that may not exist yet? */
static int ver_list_chain(Compiler *c, int k, int sg) {
  for (int si = 1; si < c->nscopes; si++) {
    const Scope *s = &c->scopes[si];
    if (!s->is_cmethod != !sg || !ver_cond_scope(c, s)) continue;
    int m = call_target(c, -1, k, comp_scope_user_name(s), sg ? CT_CMETHOD : 0, NULL);
    if (m >= 0 && m < c->nscopes && ver_cond_scope(c, &c->scopes[m])) return 1;
  }
  return 0;
}
/* Does list-of-methods call id (is_method_presence_query, kind 3) list a
   method that may not exist yet: its receiver's chain finds a method of a
   conditional scope? A constant names its class: the instance side of a
   class or module for instance_methods and its kin, its class side for the
   others. A class value of another form is asked of the classes it can be
   (poly_cls_value_reaches); an object's list is its class's and the
   classes below it (an object's singleton_methods, its own, lists none of
   a class's). A receiver of no class known here is taken to. */
static int ver_list_held(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int r = nt_ref(nt, id, "receiver"), sg = !is_instance_method_list(name);
  if (!c->nver_refl && !c->nver_refl_priv && !c->ncond_disp) return 0;
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    int k = cplan_const_recv_class(c, id, r, NULL);
    /* a read a pass rewrote from a local has no place in the record: its
       name, as the list builtin resolves it (an_singleton_methods_foldable) */
    if (k < -1 && rk == NK_ConstantReadNode) k = comp_class_index(c, nt_str(nt, r, "name"));
    return k < 0 || ver_list_chain(c, k, sg);
  }
  /* self in a class or module body is that class, as a constant naming
     it is (a list a body makes of the methods it has defined) */
  const Scope *cs = r < 0 ? comp_scope_of(c, id) : NULL;
  if (r < 0 && !(cs && cs->name) && c->node_cbody && id < c->node_cap && c->node_cbody[id] >= 0)
    return ver_list_chain(c, c->node_cbody[id], sg);
  TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
  if (rt == TY_CLASS) {
    for (int si = 1; si < c->nscopes; si++) {
      const Scope *s = &c->scopes[si];
      if (!s->is_cmethod == !sg && ver_cond_scope(c, s) && poly_cls_value_reaches(c, id, s->class_id)) return 1;
    }
    return 0;
  }
  if (!ty_is_object(rt)) return 1;
  if (is_singleton_method_list(name)) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    int a = k;
    while (a >= 0 && a != ty_object_class(rt)) a = c->classes[a].parent;
    if (a >= 0 && ver_list_chain(c, k, 0)) return 1;
  }
  return 0;
}
/* what a call is refused with where it reaches a def under a run-time condition that no cell tells ran (cond_def_override,
   cplan_bycount_gap); front: the class the call binds mixes Kernel in below the def's class (BYCOUNT_FRONT) */
enum { BYCOUNT_GAP = 1, BYCOUNT_FRONT };
static const char *cond_def_override_why(int front) {
  if (front)
    return "a def under a condition only the run time decides, in a class without Kernel, called on a subclass that "
           "includes Kernel, is not supported: Kernel's method answers in front of the def whether the branch ran or not, "
           "and the def's entry stands for the def; see docs/limitations.md";
  return "a def under a condition only the run time decides, of a name another method answers "
         "(a builtin's, another def's or an accessor's), is not supported: which one answers when the branch did "
         "not run is not known at compile time; see docs/limitations.md";
}
/* the limit the call's shape names, and in *lx how a user method exempts it */
static const char *feature_limit(Compiler *c, int id, int *stop, int *lx, char *buf, size_t cap) {
  *stop = 1;
  *lx = LX_NONE;
  const NodeTable *nt = c->nt;
  NodeKind nk = nt_kind(nt, id);
  if (nk != NK_CallNode) return NULL;
  const char *name = nt_str(nt, id, "name");
  if (!name) return NULL;
  if (is_raise_alias(name) && nt_int(nt, id, "class_new_capture", 0) && cplan_reachable(c, id))
    return "Class.new or Module.new with a block that captures outer locals is not supported; see docs/limitations.md";
  if (is_raise_alias(name) && nt_int(nt, id, "cond_def_override", 0) && cplan_reachable(c, id))
    return cond_def_override_why(0);
  int pq = is_method_presence_query(name);
  if ((nt_int(nt, id, "cond_def_reflect", 0) || (pq == 1 && ver_refl_named(c, id)) ||
       ((pq == 2 ? c->nver_refl || c->ncond_lone_disp || (c->ncond_disp && ver_list_held(c, id)) : pq == 3 && ver_list_held(c, id)) && !cplan_recv_own_member(c, id, name))) &&
      cplan_reachable(c, id))
    return "a method defined under a condition only the run time decides is not answered for by "
           "respond_to?, method_defined?, instance_methods and their kin (it would answer as if "
           "the branch had run); see docs/limitations.md";
  if (is_raise_alias(name) && nt_int(nt, id, "sclass_self", 0) && cplan_reachable(c, id))
    return "the ancestry and the method table of a singleton class are not modeled: its name, "
           "identity, instance_methods(false) and method_defined? are; see docs/limitations.md";
  if (elab_builder_kind(c, id) == EB_DYNAMIC_SUPER && cplan_reachable(c, id))
    return "Class.new with a non-constant superclass is not supported; "
           "use a constant superclass (see docs/limitations.md)";
  if (nt_int(nt, id, "attr_splat", 0) && cplan_reachable(c, id)) {
    snprintf(buf, cap, "%s with a splat of anything but a constant Array literal of Symbols or Strings "
             "(written once, ahead of the call, and never changed) is not supported; "
             "name the attributes (see docs/limitations.md)", name);
    return buf;
  }
  if (nt_int(nt, id, "define_method_name", 0) == 2 && cplan_reachable(c, id))
    return "Module#define_method with `&` of a block or Proc that is not a literal (`&block`, a parameter, "
           "a constant, a call's result), or with a Proc or Method passed as its body, is not supported: "
           "the defined method's body is known only at run time. Pass a block literal to define_method instead";
  if (nt_int(nt, id, "define_method_name", 0) && cplan_reachable(c, id))
    return "Module#define_method with a non-literal name is not supported; "
           "use a literal Symbol or String (see docs/limitations.md)";
  if (cplan_computed_ivar_get(c, id, name))
    return sp_streq(name, "instance_variable_set")
      ? "Object#instance_variable_set with a non-literal name on a receiver that may be an instance "
        "of a class defining its own instance_variable_set is not supported (see docs/limitations.md)"
      : "Object#instance_variable_get with a non-literal name on a receiver that may be an instance "
        "of a class defining its own instance_variable_get is not supported (see docs/limitations.md)";
  /* marked by mark_anon_superclass_reflection: no class object stands for
     the anonymous class that CRuby's answer names. A call the plan binds to
     a user method is that method's, not Class#superclass (RDoc's
     ClassModule#superclass beside `class Rule < Struct.new(:weight)`). */
  int anon_reflect = (int)nt_int(nt, id, "anon_reflect", 0);
  if (anon_reflect && cplan_reachable(c, id)) {
    *lx = LX_BINDING;
    snprintf(buf, cap, "unsupported `%s` that can reach a class whose superclass is an anonymous "
             "class (Class.new, Struct.new or Data.define as the superclass): spinel has no class "
             "object for the anonymous class", anon_reflect == 1 ? "superclass" : "ancestors");
    return buf;
  }
  /* a const_set on a receiver known only at run time, of a name constants
     of more than one module share (const_set_slot): constants are C
     globals, one per owner, and which owner's this sets is the receiver's */
  int cs_an = nt_ref(nt, id, "arguments"), cs_ac = 0;
  const int *cs_av = is_const_set_name(name) && cs_an >= 0 ? nt_arr(nt, cs_an, "arguments", &cs_ac) : NULL;
  const char *cs_n = cs_ac == 2 ? vis_arg_name(nt, cs_av[0]) : NULL, *cs_key;
  if (cs_n && const_set_slot(c, id, cs_n, &cs_key) == CS_AMBIG && cplan_reachable(c, id)) {
    *lx = LX_BINDING;
    snprintf(buf, cap, "const_set(:%s, ...) on a receiver whose class or module is known only at run "
             "time is not supported when more than one module holds a constant %s: constants are "
             "compile-time globals, and which module's %s this sets is not known at compile time. "
             "Write `Mod.const_set(:%s, v)` or `Mod::%s = v` (see docs/limitations.md)",
             cs_n, cs_n, cs_n, cs_n, cs_n);
    return buf;
  }

  int recv = nt_ref(nt, id, "receiver");
  if (is_then_alias(name)) {
    int blk = nt_ref(nt, id, "block"), recv = nt_ref(nt, id, "receiver");
    TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    /* A callable block has no inline body or returned-String identity fact. */
    if (blk >= 0 && nt_kind(nt, blk) != NK_BlockNode && (rt == TY_STRING || rt == TY_STRBUF)) {
      *lx = LX_BINDING;
      return "String#then / #yield_self with a callable block is not supported; "
             "use a literal block to preserve its result identity (see docs/limitations.md)";
    }
  }
  static const struct { const char *m; const char *why; } tbl[] = {
    { "define_singleton_method",
      "Object#define_singleton_method is not supported by AOT compilation: a per-object "
      "method table would have to be consulted on every call, but each call site is a "
      "direct C call to a compiled body. Define the method in the class body instead "
      "(see docs/limitations.md)" },
    { "extend",
      "Object#extend is not supported by AOT compilation: mixing a module into a live "
      "object needs a per-object method table. Use `include`/`prepend` in the class body "
      "instead (see docs/limitations.md)" },
    { "ruby2_keywords",
      "Proc#ruby2_keywords is not supported: it is a shim for the Ruby 2.x-to-3.0 "
      "keyword-argument transition, and spinel targets modern keyword semantics, so it "
      "has nothing to toggle (see docs/limitations.md)" },
    { "ruby2_keywords_hash",
      "Hash.ruby2_keywords_hash is not supported: it marks a hash for the Ruby "
      "2.x-to-3.0 keyword-argument transition shim, and spinel targets modern keyword "
      "semantics, so the flag has nothing to toggle (see docs/limitations.md)" },
    { "ruby2_keywords_hash?",
      "Hash.ruby2_keywords_hash? is not supported: it reads the Ruby 2.x-to-3.0 "
      "keyword-transition flag, which spinel's hashes do not carry (see "
      "docs/limitations.md)" },
    { "singleton_class",
      "Object#singleton_class is not supported by AOT compilation: it is the gateway "
      "to a per-object method table, which direct C calls have no room for -- the same "
      "limit as define_singleton_method. Define methods in the class body instead "
      "(see docs/limitations.md)" },
    { "remove_method",
      "Module#remove_method is not supported by AOT compilation: methods are resolved "
      "statically and compiled to direct C calls, so there is no runtime method table to "
      "remove an entry from (see docs/limitations.md)" },
    { "undef_method",
      "Module#undef_method is not supported by AOT compilation: methods are resolved "
      "statically and compiled to direct C calls, so there is no runtime method table to "
      "undefine an entry in (see docs/limitations.md)" },
    { "remove_class_variable",
      "Module#remove_class_variable is not supported by AOT compilation: class variables "
      "are compiled to static storage, so a variable cannot be removed at run time "
      "(see docs/limitations.md)" },
    { "set_trace_func",
      "set_trace_func is not supported by AOT compilation: it requires an interpreter "
      "loop to hook, and compiled code has no such loop (see docs/limitations.md)" },
    { "callcc",
      "Kernel#callcc is not supported by AOT compilation: multi-shot full-stack capture "
      "has no flat-C analogue. Fiber covers the single-shot cases (see docs/limitations.md)" },
    { "refine",
      "Refinements are not supported by AOT compilation: scope-keyed dispatch is "
      "incompatible with direct C calls. Reopen the class instead (see docs/limitations.md)" },
    { "using",
      "Refinements are not supported by AOT compilation: scope-keyed dispatch is "
      "incompatible with direct C calls. Reopen the class instead (see docs/limitations.md)" },
    { NULL, NULL }
  };
  int hit = -1;
  for (int k = 0; tbl[k].m; k++) if (sp_streq(name, tbl[k].m)) { hit = k; break; }
  /* a class's singleton class is a value of its own (sp_sclass_id); an
     object's is not modeled */
  if (hit >= 0 && sp_streq(name, "singleton_class") && sclass_value_call(c, id)) hit = -1;

  /* The receiver's constant name, for the limits that are keyed on it. */
  NodeKind rty_k = nt_kind(nt, recv);
  const char *rcn = (rty_k == NK_ConstantReadNode || rty_k == NK_ConstantPathNode)
                    ? key_name(c, recv) : NULL;
  const char *why = hit >= 0 ? tbl[hit].why : NULL;
  if (why) *lx = LX_NAME;
  /* resolved to String#method's own wrapper, so a user `method` elsewhere
     (`def method` in some Foo) is not what the call reaches */
  int str_mutator = !why && cplan_str_method_mutator(c, id);
  if (str_mutator)
    why = "String#method is not supported for a method that changes the String in place "
          "(`<<`, `concat`, `upcase!`, ...): the Method object is bound to the String's "
          "value, not to the String, so calling it could not change the String it came "
          "from. Call the method on the String directly, or wrap it in a block "
          "(`->(x) { s << x }`) (see docs/limitations.md)";

  if (!why && rcn && const_node_class_index(c, recv) < 0) {
    /* Namespaces that exist only in an interpreter. Keyed on the receiver, so
       every method on them reports the same way rather than one-by-one. */
    if (sp_streq(rcn, "ObjectSpace"))
      why = "ObjectSpace is not supported by AOT compilation: there is no class-keyed "
            "allocation registry to walk -- the GC tracks bytes, not a live-object index "
            "(see docs/limitations.md)";
    else if (sp_streq(rcn, "TracePoint"))
      why = "TracePoint is not supported by AOT compilation: it requires an interpreter "
            "loop to hook, and compiled code has no such loop (see docs/limitations.md)";
    else if (sp_streq(rcn, "Continuation"))
      why = "Continuation is not supported by AOT compilation: multi-shot full-stack "
            "capture has no flat-C analogue. Fiber covers the single-shot cases "
            "(see docs/limitations.md)";
    /* `Class.new(parent) { ... }`: the class graph is baked at compile time. A
       bare `Class.new` with no argument and no block is just an Object factory
       and is left to the normal path. The message names the form written. */
    else if (is_class_new(rcn, name) && cplan_argc(nt, id) > 0)
      why = "Class.new(parent) is not supported by AOT compilation: the class "
            "graph, ancestor chain, and method/ivar layout are baked at compile time. "
            "Declare the class with `class ... end` instead (see docs/limitations.md)";
    else if (is_class_new(rcn, name) && nt_ref(nt, id, "block") >= 0)
      why = "Class.new with a block is not supported here by AOT compilation: the class "
            "graph, ancestor chain, and method/ivar layout are baked at compile time. "
            "Declare the class with `class ... end` instead (see docs/limitations.md)";
    if (why) *lx = LX_NAME;
  }

  /* Structural mutation of a class through an explicit receiver: the same
     declarations INSIDE a `class` body (where they are receiverless) work.
     Object counts though a program that never reopens it has no class entry
     for it: `Object.include M` (or through `O = Object`) otherwise compiled
     to a NoMethodError at run time. */
  int mixin = sp_streq(name, "include") || sp_streq(name, "prepend");
  /* (one the load record takes as a mixin of the class's: mixin_by_record) */
  if (mixin && nt_int(nt, id, "mixin_by_record", 0)) mixin = -1;
  if (!why && rcn && mixin >= 0 && (const_node_class_index(c, recv) >= 0 || sp_streq(rcn, "Object") ||
                      (mixin && is_builtin_reopen_name(rcn))) &&
      (mixin ||
       sp_streq(name, "attr_accessor") || sp_streq(name, "attr_reader") ||
       sp_streq(name, "attr_writer") || sp_streq(name, "define_method"))) {
    /* only the receiver's own class method of the name answers instead:
       a method of that name in an unrelated class (`def include` in some
       Foo) does not */
    *lx = LX_RECV_CMETHOD;
    static char buf[512];
    snprintf(buf, sizeof buf,
             "%s.%s(...) is not supported by AOT compilation: the class graph, ancestor "
             "chain, and method/ivar layout are baked at compile time, so a class cannot be "
             "restructured through an explicit receiver. Move the `%s` inside `class %s ... "
             "end` (see docs/limitations.md)", rcn, name, name, rcn);
    why = buf;
  }
  /* the same through a Class held in a variable (`[A, B].each { |k| k.include(M) }`)
     or a singleton class the desugaring left: a NoMethodError at run time */
  if (!why && mixin) {
    int rk = recv >= 0 ? nt_kind(nt, recv) : -1;
    if (rk == NK_CallNode && nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "singleton_class"))
      why = "singleton_class.include(...) / singleton_class.prepend(...) is supported only as a statement "
            "of a class or module body, or at the top level on a class or module the program declares, "
            "or on Process, Kernel, Math, GC, Signal or Warning: "
            "move it into `class X ... end` (see docs/limitations.md)";
    else if (!rcn && recv >= 0 && (comp_ntype(c, recv) == TY_CLASS ||
                                   (comp_ntype(c, recv) == TY_POLY && cplan_module_args(c, id))))
      why = "include / prepend on a Class held in a variable is not supported by AOT compilation: the ancestor "
            "chain is baked at compile time, so the class must be named by a constant. Write "
            "`K.include(M)` (or `include M` inside `class K ... end`) for each class "
            "(see docs/limitations.md)";
    if (why) *lx = LX_NAME;
  }

  if (!why) { *stop = 0; return NULL; }
  return why;
}

const char *cplan_feature_why(Compiler *c, int id, int *stop, char *buf, size_t cap) {
  int lx;
  const char *why = feature_limit(c, id, stop, &lx, buf, cap);
  return why && limit_exempt(c, id, lx) ? NULL : why;
}


/* The refusals the prepasses raise before any emission (reject_runtime_send,
   reject_runtime_const_get, reject_binding in codegen.c) and Kernel#eval
   (diagnose_eval_call): the `what` unsupported words them with, or NULL.
   The message is unsup_message's, with no class being emitted: the
   prepasses run before any is. */

/* a user method of the name: the call is that method, not the builtin */
static int cplan_user_names(Compiler *c, const char *name) {
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].name && sp_streq(c->scopes[s].name, name)) return 1;
  return 0;
}
/* a call in a method codegen emits: a dead method is pruned before emission,
   so refusing a call in it would fail a program that merely contains it.
   walk_scope assigns every node -- including those inside blocks -- the
   enclosing method's scope, and the emit loop emits a scope's body only when
   it may run as itself (comp_scope_may_run) */
static int cplan_reachable(Compiler *c, int id) {
  if (nt_int(c->nt, id, "refusal_dead", 0)) return 0;
  int proc = nt_int(c->nt, id, "refusal_proc", -1);
  if (proc >= 0 && proc_literal_uncalled(c, proc)) return 0;
  int sc = c->nscope[id];
  return sc >= 0 && sc < c->nscopes && comp_scope_may_run(c, sc);
}

static const char *cplan_runtime_send_what(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !is_send_family(nm))
    return NULL;
  /* a socket's send(data, flags) is the datagram write, not Object#send
     (#7193): the IO arm emits it, whatever holds the payload */
  { int rr = nt_ref(nt, id, "receiver");
    if (rr >= 0 && sp_streq(nm, "send") && infer_type(c, rr) == TY_IO && sp_feature_required("socket")) return NULL; }
  if (nt_int(nt, id, "rt_probe", 0)) return NULL;  /* analysis-only respond_to? probe */
  int args = nt_ref(nt, id, "arguments");
  if (args < 0) return NULL;
  int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
  if (ac < 1 || !av) return NULL;
  NodeKind a0_k = nt_kind(nt, av[0]);
  /* a literal name should have been rewritten already; only a runtime name
     (a variable, a method result, an interpolated string, ...) reaches here */
  if (a0_k == NK_SymbolNode || a0_k == NK_StringNode) return NULL;
  /* lowered to a static name-dispatch (desugar_dynamic_send) over the
     program's literals AND the methods it defines: a computed name (an
     interpolated `"#{name}="`, a concatenation) resolves there too, and a
     name outside that set raises NoMethodError at the dispatch -- loud at
     run time rather than here. On a receiver known to be a builtin the
     set is that class's own methods; a builtin method reached through a
     computed name on a receiver of no known class is the one shape it
     does not cover. */
  { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn);
    if (dn > 0 && (!an_send_name_is_computed(c, av[0]) || nt_int(nt, id, "dyn_send_complete", 0) > 0)) return NULL; }
  if (!cplan_reachable(c, id)) return NULL;
  /* user-defined: leave to normal dispatch */
  if (cplan_user_names(c, "send") || cplan_user_names(c, "__send__") || cplan_user_names(c, "public_send"))
    return NULL;
  return "send with a runtime method name (AOT needs a compile-time-known name)";
}

static const char *cplan_runtime_const_get_what(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "const_get")) return NULL;
  int args = nt_ref(nt, id, "arguments");
  int an = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an < 1 || !av) return NULL;
  NodeKind k = nt_kind(nt, av[0]);
  if (k == NK_SymbolNode || k == NK_StringNode) return NULL;
  { int dn = 0; nt_arr(nt, id, "dyn_cget_arms", &dn); if (dn > 0) return NULL; }
  if (!cplan_reachable(c, id)) return NULL;
  if (cplan_user_names(c, "const_get")) return NULL;   /* user-defined: normal dispatch */
  return "const_get with a name known only at run time and no class or module "
         "receiver (an instance has no const_get; constants are resolved at compile time)";
}

static const char *cplan_binding_what(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "binding")) return NULL;
  if (nt_ref(nt, id, "receiver") >= 0) return NULL;             /* Kernel#binding is receiverless */
  int args = nt_ref(nt, id, "arguments");
  if (args >= 0) { int ac = 0; nt_arr(nt, args, "arguments", &ac); if (ac > 0) return NULL; }
  if (!cplan_reachable(c, id)) return NULL;
  if (cplan_user_names(c, "binding")) return NULL;  /* user-defined */
  return "binding is unsupported (no reified local environment in an "
         "AOT binary; only binding.local_variable_get(:name) for an "
         "in-scope name is)";
}

/* `instance_eval(str)`: dropped on any receiver, as a String class_eval was
   skipped as a class-body declaration (a class-body String class_eval or
   module_eval is now emitted as a statement, refused there as any
   unsupported call is). Not where the call reaches a method of the
   program's own. */
/* Does call id reach a method `name` the program defines: on self's class
   (or the top level), on a program object's class, or as a class method of
   a class named by a constant? A receiver of any other kind does not. */
static int cplan_eval_reaches_own(Compiler *c, int id, const char *name) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, id, "receiver");
  if (r < 0 || nt_kind(nt, r) == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    int k = s ? s->class_id : -1;
    if (k < 0 && c->node_cbody && id < c->node_cap) k = c->node_cbody[id];
    if (call_target(c, id, -1, name, CT_TOP, NULL) >= 0) return 1;
    if (k < 0) return 0;
    /* in a class body or a class method self is the class */
    int on_class = (s && s->is_cmethod) || (c->node_cbody && id < c->node_cap && c->node_cbody[id] >= 0 && s && !s->name);
    return call_target(c, id, k, name, on_class ? CT_CMETHOD : 0, NULL) >= 0;
  }
  TyKind rt = comp_ntype(c, r);
  if (ty_is_object(rt)) return call_target(c, id, ty_object_class(rt), name, 0, NULL) >= 0;
  if (nt_kind(nt, r) == NK_ConstantReadNode) {
    int k = const_node_class_index(c, r);
    return k >= 0 && call_target(c, id, k, name, CT_CMETHOD, NULL) >= 0;
  }
  return 0;
}
static const char *cplan_eval_text_what(Compiler *c, int id, const char *name) {
  const NodeTable *nt = c->nt;
  if (!sp_streq(name, "instance_eval")) return NULL;
  int args = nt_ref(nt, id, "arguments"), argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc < 1 || nt_ref(nt, id, "block") >= 0) return NULL;
  /* the program's own instance_eval only where this call reaches it: an
     unrelated class's method of the name left every other call unrefused */
  if (cplan_eval_reaches_own(c, id, name)) return NULL;
  return "instance_eval of a String is not supported by AOT compilation (pass a block instead)";
}
static const char *cplan_eval_what(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return NULL;
  const char *name = nt_str(nt, id, "name");
  if (!name) return NULL;
  if (!sp_streq(name, "eval")) return cplan_eval_text_what(c, id, name);
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc < 1) return NULL;
  int recv = nt_ref(nt, id, "receiver");
  if (recv >= 0) {
    NodeKind rk = nt_kind(nt, recv);
    if (rk != NK_ConstantReadNode && rk != NK_ConstantPathNode) return NULL;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "Kernel")) return NULL;
  }
  else {
    /* A receiverless `eval(x)` is Kernel#eval only if nothing nearer owns the
       name. A tree-walking interpreter calls its own `eval` from inside the
       class that defines it, and refusing that is refusing the program the
       method belongs to. */
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0 &&
        call_target(c, -1, self->class_id, "eval", CT_METHODS | CT_SHARED, NULL) >= 0) return NULL;
    /* (the refusal keeps the table's answer, as the codegen's own emission of the call does: a def of eval later in the program leaves the call as it is) */
    if (call_target(c, -1, -1, "eval", CT_TOP | CT_SHARED, NULL) >= 0) return NULL;
  }
  return "eval of a runtime string is not supported by AOT compilation (define the code statically)";
}

/* Two gaps a call's tables decide, refused as `unsupported(c, id, "call")`:
   a call on an IO whose reopening of the name yields or takes a block (a
   block-taking one has no standalone function to call: emit_io_reopen_call),
   and a bind_call on a Class value known only at run time where some class
   has no arm to build (class_value_bind_call_gap). */
static int cplan_io_reopen_yields(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0 || comp_ntype(c, recv) != TY_IO) return 0;
  int ci = io_reopen_class(c, id, name);
  if (ci < 0 || call_target(c, id, ci, name, 0, NULL) < 0) return 0;
  int ks[16], n = io_reopen_defs(c, id, name, 0, ks, 16);
  for (int i = 0; i < n; i++) {
    Scope *km = &c->scopes[call_target(c, id, ks[i], name, 0, NULL)];
    if (km->yields || (km->blk_param && km->blk_param[0])) return 1;
  }
  return 0;
}

static int cplan_bind_call_gap(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "bind_call")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (recv < 0 || argc < 1 || nt_kind(nt, argv[0]) == NK_SplatNode) return 0;
  return class_value_instance_method_sym(c, recv) && class_value_bind_call_gap(c, recv);
}

/* A receiverless call in a top-level `def`, which has no self of its own
   here, to a name main has a singleton method for: CRuby's call finds main's
   method when the def runs on main and Object's when it runs on another
   object, and the def is one function whichever calls it. A version entry
   is not a def of the program: its calls are the dispatch to the versions. */
static int cplan_top_self_gap(Compiler *c, int id) {
  const char *name = nt_str(c->nt, id, "name");
  const Scope *s = comp_scope_of(c, id);
  /* (a call written `Kernel.name` is Kernel's whatever self is: desugar_kernel_recv marks it) */
  if (!(nt_kind(c->nt, id) == NK_CallNode && nt_ref(c->nt, id, "receiver") < 0 && name && s->class_id < 0 &&
        !(cplan_builtin_call(c, id) || nt_int(c->nt, id, "kernel_recv", 0)) && !s->is_cmethod && !s->entry && s->def_node >= 0 &&
        !prog_is_top_scope(c, s))) return 0;
  mtab_enter(c);   /* (the plan's own root read: whether main has a singleton method of the name) */
  int gap = comp_main_method(c, name) >= 0;
  mtab_leave(c);
  return gap;
}

/* Does call id bind, on a class it may reach, what no cell tells whether it ran (a call the plan cannot bind at its count):
   the version of a history held by count itself, where no entry of its count was made for it, or a copy of it (an alias),
   or the entry of a held def at a count it does not take (the entry's parameters are the def's, ver_named_make), or one whose
   else arm is no Kernel row on a class whose chain has Kernel in front of it? */
static void cplan_bycount_ask(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  MethodLookup ml = { .def_class = -1 };
  int r = call_target(c, id, k, nm, flags, &ml);
  /* (a class no object of can exist yet where the call runs is no class it binds) */
  int none = ml.why == MLW_DISAGREE && k >= 0 && !elab_class_live_at(c, k, (flags & CT_CMETHOD) != 0, elab_point_of(c, id));
  if (r < 0 || none) return;
  const Scope *t = &c->scopes[r];
  /* (read through an alias's name: CRuby's copy is Kernel's method where the branch did not run. The `alias` keyword of a held
     def keeps the answer it had, the def taken as run: its copy cannot be told taken or untaken without a cell of its own) */
  const LrDef *ad = NULL;
  int via = !sp_streq(nm, comp_scope_user_name(t)) && elab_method_at(c, k, nm, (flags & CT_CMETHOD) != 0, elab_point_of(c, id), &ad) == ELAB_YES &&
            ad && ad->kind == LR_ALIAS;
  int kw = via && nt_kind(c->nt, ad->node) == NK_AliasMethodNode;
  int by = nt_int(c->nt, t->def_node, "cond_def_bycount", 0) && !kw && (ml.kind == ML_TABLE || !t->replaced || via),
      n = ver_call_n(c, id), refl = t->entry == ENTRY_NAMED && bop_kernel_answers(nm, -1, 0) && nt_int(c->nt, t->def_node, "cond_def_refl", 0),
      row = refl && ver_kernel_row_behind(c, t->class_id, nm), held = row && n >= 0 && n != ver_plain_arity(c, r);
  /* (a held def's entry in a class without Kernel, on a class k whose chain mixes Kernel in below it: CRuby's lookup meets
     Kernel's method in front of the history, which the entry does not stand for) */
  int front = refl && !row && ver_kernel_row_behind(c, k, nm);
  if (by || held) *(int *)arg = BYCOUNT_GAP;
  else if (front && !*(int *)arg) *(int *)arg = BYCOUNT_FRONT;
}
/* ...and the super of a prepended module's copy, which reaches the class's own version (ct_version_classes asks it of no class) */
static int cplan_bycount_shadow_super(Compiler *c, const Scope *in, const char *nm) {
  for (int s = 1; s < c->nscopes; s++)   /* (the class's own body is a shadow copy under a spelling of its own) */
    if (c->scopes[s].class_id == in->class_id && c->scopes[s].def_node >= 0 && nt_int(c->nt, c->scopes[s].def_node, "cond_def_bycount", 0) &&
        sp_streq(comp_scope_user_name(&c->scopes[s]), nm)) return 1;
  return 0;
}
static int cplan_bycount_gap(Compiler *c, int id) {
  NodeKind k = nt_kind(c->nt, id);
  const Scope *in = k == NK_SuperNode || k == NK_ForwardingSuperNode ? comp_scope_of(c, id) : NULL;   /* (a super names its method's) */
  const char *nm = in ? comp_scope_user_name(in) : nt_str(c->nt, id, "name");
  int gap = 0;
  if ((!c->nver_bycount && !c->nver_refl && !c->nver_refl_priv) || (k != NK_CallNode && !in) || !nm ||
      !(ver_bycount_named(c, nm) || bop_kernel_answers(nm, -1, 0) || ct_version_named(c, nm)) || !cplan_reachable(c, id)) return 0;
  if (in && in->shadow == SHADOW_PREP && cplan_bycount_shadow_super(c, in, nm)) return BYCOUNT_GAP;
  ct_version_classes(c, id, nm, cplan_bycount_ask, &gap);
  return gap;
}

static void cplan_refuse_set(CallPlan *p, int from, int rkind, const char *msg, char *buf, size_t cap) {
  p->dispatch = CP_REFUSE; p->rfrom = (unsigned char)from; p->rkind = (unsigned char)rkind;
  snprintf(buf, cap, "%s", msg);
  p->msg = buf;
}

/* Is method mi's body empty (nothing, or a bare nil), so that running it
   builds the empty value the builtin's own initialize would not? */
static int cplan_body_empty(Compiler *c, int mi) {
  int b = c->scopes[mi].body;
  if (b < 0) return 1;
  int n = 0;
  const int *bb = nt_kind(c->nt, b) == NK_StatementsNode ? nt_arr(c->nt, b, "body", &n) : NULL;
  if (!bb) return nt_kind(c->nt, b) == NK_NilNode;
  return n == 0 || (n == 1 && nt_kind(c->nt, bb[0]) == NK_NilNode);
}
/* Are the parameters of method sc positional, and without defaults unless
   defaults_ok (the call fills every one: a default that never runs binds
   nothing), a rest and a block allowed, and a keyword rest if kwrest_ok (a
   `super` that passes it on), and optional keywords too where defaults_ok and
   kwopt_ok (a bare `super` passes them on, ct_fill_keywords)? Nothing about
   them runs code or binds anything the call did not pass. */
static int ct_params_plain(Compiler *c, const Scope *sc, int kwrest_ok, int defaults_ok, int kwopt_ok) {
  int pn = sc->def_node >= 0 ? nt_ref(c->nt, sc->def_node, "parameters") : -1, nk = 0;
  const int *kws = pn >= 0 ? nt_arr(c->nt, pn, "keywords", &nk) : NULL;
  if (pn >= 0 && nt_ref(c->nt, pn, "keyword_rest") >= 0 && !kwrest_ok) return 0;
  for (int i = 0; i < nk; i++)
    if (!defaults_ok || !kwopt_ok || nt_kind(c->nt, kws[i]) != NK_OptionalKeywordParameterNode) return 0;
  for (int i = 0; !defaults_ok && i < sc->nparams; i++)
    if (i != sc->rest_idx && sc->pdefault && sc->pdefault[i] >= 0) return 0;
  return 1;
}
/* Can the default of parameter i of method sc run at the call instead, once
   the call's arguments have (desugar_builtin_new_init_args)? It reads nothing
   the new object or the method's frame supplies: no self, instance or class
   variable, call on self, block, yield or super, no local but an earlier
   parameter (held where the call passes it), and it writes no local. Nor a
   constant, which the def's lexical scope reads and the call's need not,
   nor a call a protected method may answer, which CRuby lets self's class
   make (the object being built's there, main's at the call); a private
   one's, with a receiver that is no self, raises at both. root is the
   default (a keyword's too, i then the count of positionals). */
static int ct_default_placeable(Compiler *c, const Scope *sc, int root, int i) {
  int n = 0, ok = 1;
  int *ids = nt_subtree_ids((NodeTable *)c->nt, &root, 1, &n);
  for (int k = 0; ok && k < n; k++) {
    const char *nm = nt_str(c->nt, ids[k], "name");
    switch (nt_kind(c->nt, ids[k])) {
    case NK_IntegerNode: case NK_FloatNode: case NK_RationalNode: case NK_ImaginaryNode: case NK_StringNode:
    case NK_InterpolatedStringNode: case NK_EmbeddedStatementsNode: case NK_SymbolNode: case NK_InterpolatedSymbolNode:
    case NK_NilNode: case NK_TrueNode: case NK_FalseNode: case NK_ArrayNode: case NK_HashNode: case NK_AssocNode:
    case NK_KeywordHashNode: case NK_RangeNode: case NK_RegularExpressionNode: case NK_ParenthesesNode:
    case NK_StatementsNode: case NK_ArgumentsNode: case NK_GlobalVariableReadNode: case NK_GlobalVariableWriteNode: case NK_GlobalVariableOperatorWriteNode:
    case NK_GlobalVariableOrWriteNode: case NK_GlobalVariableAndWriteNode: case NK_IfNode: case NK_ElseNode:
    case NK_UnlessNode: case NK_AndNode: case NK_OrNode:
      break;
    case NK_CallNode:
      ok = nt_ref(c->nt, ids[k], "receiver") >= 0 && nt_ref(c->nt, ids[k], "block") < 0 && !elab_method_may_be_protected(c, nm);
      break;
    case NK_SplatNode: ok = nt_ref(c->nt, ids[k], "expression") >= 0; break;
    case NK_LocalVariableReadNode:
      ok = 0;
      for (int j = 0; j < i && !ok; j++) ok = j != sc->rest_idx && j != sc->kwrest_idx && nm && sc->pnames[j] && sp_streq(nm, sc->pnames[j]);
      break;
    default: ok = 0;
    }
  }
  free(ids);
  return ok;
}
/* Are the parameters of method sc that a call of argc positional arguments
   leaves unfilled all defaults, the trailing optionals ahead of any rest?
   1 where every one runs at the call (ct_default_placeable), each into f where
   f is given (parameter of scope mi); FILL_UNPLACED where one cannot; 0 where
   one has no default. *effects counts, where effects is given, those that
   have an effect (subtree_has_side_effect). */
enum { FILL_UNPLACED = -1, FILL_RAISE = -2 };
/* The index past method sc's positional parameters, its rest among them: its keywords follow (scope_add_param's order). */
static int ct_positionals(Compiler *c, const Scope *sc) {
  int pn = sc->def_node >= 0 ? nt_ref(c->nt, sc->def_node, "parameters") : -1, nkw = 0;
  if (pn >= 0) nt_arr(c->nt, pn, "keywords", &nkw);
  return sc->nparams - nkw - (sc->kwrest_idx >= 0);
}
static int ct_fill_defaults(Compiler *c, const Scope *sc, int mi, int argc, NewFills *f, int *effects) {
  int placed = 1, npos = ct_positionals(c, sc);
  for (int i = 0, k = 0; i < npos; i++) {
    if (i == sc->rest_idx || i == sc->kwrest_idx || k++ < argc) continue;
    if (!sc->pdefault || sc->pdefault[i] < 0 || (sc->rest_idx >= 0 && i > sc->rest_idx) || (f && f->n >= NEW_FILLS_MAX)) return 0;
    if (effects && subtree_has_side_effect(c, sc->pdefault[i])) ++*effects;
    if (!ct_default_placeable(c, sc, sc->pdefault[i], i)) placed = 0;
    else if (f) { f->mi[f->n] = mi; f->pi[f->n++] = i; }
  }
  return placed ? 1 : FILL_UNPLACED;
}
/* Does the `super` sup of method sc pass on exactly what sc received: its
   parameters in order, a rest parameter splatted, no keywords, and its
   caller's block (super_forwards_caller_block)? */
static int ct_super_passes_params(Compiler *c, const Scope *sc, int sup, int argc, int open, int kw_ok) {
  const NodeTable *nt = c->nt;
  /* (the call fills every positional parameter, or the defaults it leaves run at the call) */
  int pos = ct_positionals(c, sc) - (sc->rest_idx >= 0);
  if (!ct_params_plain(c, sc, 1, !open && (argc >= pos || ct_fill_defaults(c, sc, -1, argc, NULL, NULL) != 0),
                       nt_kind(nt, sup) == NK_ForwardingSuperNode)) return 0;
  int an = 0, args = nt_ref(nt, sup, "arguments"), np = 0, blk = nt_ref(nt, sup, "block");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (!block_pass_is_callers(c, sc, blk)) return 0;
  if (nt_kind(nt, sup) == NK_ForwardingSuperNode) return 1;
  for (int i = 0; i < an; i++) {
    if (nt_kind(nt, av[i]) == NK_BlockArgumentNode) continue;   /* (judged above) */
    if (nt_kind(nt, av[i]) == NK_KeywordHashNode) {   /* (`**kw`, the keyword rest passed on) */
      int en = 0, np0 = np;
      const int *el = nt_arr(nt, av[i], "elements", &en);
      int kv = en == 1 && nt_kind(nt, el[0]) == NK_AssocSplatNode ? nt_ref(nt, el[0], "value") : -1;
      if (kv < 0 || nt_kind(nt, kv) != NK_LocalVariableReadNode || sc->kwrest_idx < 0 || np0 != sc->kwrest_idx ||
          !nt_str(nt, kv, "name") || !sp_streq(nt_str(nt, kv, "name"), sc->pnames[sc->kwrest_idx])) return 0;
      np++;
      continue;
    }
    int ex = nt_kind(nt, av[i]) == NK_SplatNode ? nt_ref(nt, av[i], "expression") : av[i];
    if (np >= sc->nparams || ex < 0 || nt_kind(nt, ex) != NK_LocalVariableReadNode || !nt_str(nt, ex, "name") ||
        !sp_streq(nt_str(nt, ex, "name"), sc->pnames[np]) || (nt_kind(nt, av[i]) == NK_SplatNode) != (np == sc->rest_idx)) return 0;
    np++;
  }
  /* (a keyword rest the `super` leaves out drops the keywords a call passes: a forward where the call passes none, or the builtin's
     `new` applies them itself or they make no difference, as String's and Hash's do (BC_NEW_KEYWORDS); not for Time's `in:` or
     Fiber's `blocking:`, which take effect in initialize) */
  return np == sc->nparams || (np + 1 == sc->nparams && sc->kwrest_idx == np && kw_ok);
}
/* Is method mi, which the chain of builtin class ci holds for initialize, a
   forward: a prepended module's copy whose body only calls `super` with what
   it received? Then the builtin's own initialize runs as if it were not there.
   A reopening's own def is never one: its `super` skips the builtin (the
   reopening replaced it) and reaches BasicObject's. The body read is the
   def's, as written, which every copy of the method shares: the copy's own
   is the one the compiler rewrites, and a `super` into the builtin there is
   lowered to the builtin's call (lower_builtin_supers), which no longer says
   what the `super` passes (a bare one's block). */
static int ct_init_forwards(Compiler *c, int mi, int ci, int argc, int open, int kw_ok) {
  const Scope *sc = &c->scopes[mi];
  int b = sc->def_node >= 0 ? nt_ref(c->nt, sc->def_node, "body") : -1, n = 0;
  const int *bb = b >= 0 && nt_kind(c->nt, b) == NK_StatementsNode ? nt_arr(c->nt, b, "body", &n) : NULL;
  if (!sc->is_prepend_copy || sc->class_id != ci || !bb || n != 1) return 0;
  NodeKind k = nt_kind(c->nt, bb[0]);
  return (k == NK_SuperNode || k == NK_ForwardingSuperNode) && ct_super_passes_params(c, sc, bb[0], argc, open, kw_ok);
}
/* The keywords a builtin's `new` passes on (ct_call_keywords): none, some
   for certain (a literal key), or maybe none (only `**h` or `...`, which may
   be empty, and an empty one passes nothing). */
enum { NEWKW_NONE, NEWKW_MAYBE, NEWKW_SURE };
static int ct_call_keywords(const NodeTable *nt, const int *av, int nargs) {
  int kw = nt_argv_has(nt, av, nargs, NT_ARG_FORWARDING) ? NEWKW_MAYBE : NEWKW_NONE;
  for (int i = 0; i < nargs; i++) {
    int en = 0;
    const int *el = nt_kind(nt, av[i]) == NK_KeywordHashNode ? nt_arr(nt, av[i], "elements", &en) : NULL;
    for (int j = 0; j < en; j++) {
      if (nt_kind(nt, el[j]) != NK_AssocSplatNode) return NEWKW_SURE;
      kw = NEWKW_MAYBE;
    }
  }
  return kw;
}
/* ...and the keyword parameters kws of method sc (scope mi), which a bare `super` passes on as keywords: each the call
   does not pass (kw: ct_call_keywords; a `**h` may pass any (maybe), which leaves only a default without an effect to
   fill, ahead of it) into f, as ct_fill_defaults answers. FILL_RAISE where a literal key the call passes names none and
   no keyword rest takes it (CRuby's ArgumentError, raised as the def binds its parameters, for every key it does not
   name, a `**h`'s too). */
static int ct_fill_keywords(Compiler *c, const Scope *sc, int mi, const int *kws, int nkw, int id, int kw, int maybe, NewFills *f, int *effects) {
  int nargs = 0, args = nt_ref(c->nt, id, "arguments"), en = 0, placed = 1, raise = 0;
  const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &nargs) : NULL;
  int kh = kw == NEWKW_SURE ? nt_argv_kwh(c->nt, av, nargs) : -1;
  const int *el = kh >= 0 ? nt_arr(c->nt, kh, "elements", &en) : NULL;
  for (int j = 0, named = 0; j < en; j++, named = 0) {
    if (nt_kind(c->nt, el[j]) == NK_AssocSplatNode) continue;
    int key = nt_kind(c->nt, el[j]) == NK_AssocNode ? nt_ref(c->nt, el[j], "key") : -1;
    if (key < 0 || nt_kind(c->nt, key) != NK_SymbolNode) return 0;
    for (int i = 0; i < nkw && !named; i++) named = sp_streq(nt_str(c->nt, kws[i], "name"), nt_str(c->nt, key, "value"));
    raise |= !named && sc->kwrest_idx < 0;
  }
  if (raise) return FILL_RAISE;
  for (int i = 0, passed = 0; i < nkw; i++, passed = 0) {
    for (int j = 0; j < en && !passed; j++)
      passed = nt_kind(c->nt, el[j]) == NK_AssocNode && sp_streq(nt_str(c->nt, kws[i], "name"), nt_str(c->nt, nt_ref(c->nt, el[j], "key"), "value"));
    if (passed) continue;
    int d = nt_ref(c->nt, kws[i], "value"), eff = d >= 0 && subtree_has_side_effect(c, d);
    if (d < 0 || (maybe && eff) || f->nk >= NEW_FILLS_MAX) return 0;
    *effects += eff;
    if (!ct_default_placeable(c, sc, d, ct_positionals(c, sc))) placed = 0;
    else { f->kmi[f->nk] = mi; f->kp[f->nk++] = kws[i]; }
  }
  return placed ? 1 : FILL_UNPLACED;
}
/* The check of a `**h`'s keys at the first forward that may raise for one (f->rmi), where the chain ends in the builtin's own
   initialize (BNF_FILL, BNF_OWN), which the rewrite makes once the operands are held and ahead of every default, given the effects the walk filled (effects), ek of them ahead of that forward: kept
   where a default with an effect is filled at or after that forward, which runs only once its binding has passed; dropped
   where none is, as then the builtin's own binding raises alike after the defaults; 0 (the call is refused) where one is
   filled ahead of it and another at or after it, as the first runs before the raise. A later forward is given every
   keyword the first names, the call's or the first's defaults, and the walk refuses one that does not name them all
   (FILL_RAISE beside the check, or a keyword fill riding past one with keywords of its own). */
static int ct_key_check(NewFills *f, int effects, int ek) {
  if (f->rmi < 0) return 1;
  if (effects == ek) { f->rmi = -1; return 1; }
  return !ek;
}
/* Is the prepend that made copy sc on class ci in force at call id: ELAB_YES where the load record proves one of its
   statements made before the call can first run (elab_def_before_call), ELAB_NO where every run of the call ends before
   each of them (a closed window: elab_call_window), ELAB_UNKNOWN where the record cannot tell (the call may run on both
   sides, a prepend it does not place or does not name). A guarded copy's entry decides at run time (Scope.mixin_guarded). */
static int ct_prepend_in_force(Compiler *c, int id, int ci, const Scope *sc) {
  int nm = 0, after = 0, n = 0;
  const LrMixin *mx = elab_class_mixins(c, ci, &nm);
  ElabWindow w;
  int placed = elab_call_window(c, id, &w) == ELAB_YES && w.closed;
  for (int j = 0; j < nm; j++) {
    if (mx[j].kind != LR_PREPEND || elab_mixin_module(c, &mx[j]) != sc->origin_module_ci - 1) continue;
    n++;
    if (elab_def_before_call(c, id, mx[j].node)) return ELAB_YES;
    int o = placed ? elab_cmp(elab_point_of(c, mx[j].node), w.to) : PROG_UNKNOWN;
    after += o == PROG_SAME || o == PROG_AFTER;
  }
  return n && after == n ? ELAB_NO : ELAB_UNKNOWN;
}
/* What the chain of program initializes of builtin class ci reduces to from
   start (cplan_builtin_new_form's rule): each takes what the call passes
   (argc positional arguments and its keywords kw, or an open list), and
   forwards or ends the walk. */
static int ct_new_chain_form(Compiler *c, int id, int ci, int start, int argc, int open, int kw, NewFills *f) {
  /* (a keyword-dropping forward is one only where dropping them cannot be observed) */
  int kw_ok = !kw || builtin_new_keywords(c->classes[ci].name), unplaced = 0, effects = 0, kwpass = 0;
  /* (the effects filled ahead of the first forward that may raise for a `**h` key (rmi): ct_key_check) */
  int ek = 0;
  /* (a `**h` alone, the call's only argument) */
  int lone = kw == NEWKW_MAYBE && !open && !argc, nargs = 0, args = nt_ref(c->nt, id, "arguments");
  const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &nargs) : NULL;
  /* (literal keys beside a `**h`, which may add any) */
  int ksplat = nt_argv_has_kwsplat(c->nt, av, nargs);
  for (int mi = start; mi >= 0 && !ct_behind_builtin_init(c, ci, mi); mi = cplan_super_target(c, -1, &c->scopes[mi], NULL)) {
    const Scope *sc = &c->scopes[mi];
    /* (a def of the class's own counts where the load record proves it made before the call can first run; where the call runs
       ahead of it, or may, it does not: the chain is the one without it, as the builtin's own is where nothing else stands) */
    if (!sc->is_prepend_copy && !sc->is_include_copy && !elab_def_before_call(c, id, sc->def_node)) continue;
    /* (a prepend's copy, the same: proved in force, or proved made after every run of the call, which the chain without it
       answers; where the record cannot tell, the call keeps master's answer, which runs no initialize: BNF_NONE) */
    if (sc->is_prepend_copy && !sc->mixin_guarded) {
      int in = ct_prepend_in_force(c, id, ci, sc);
      if (in == ELAB_NO) continue;
      if (in != ELAB_YES) return BNF_NONE;
    }
    int pn = sc->def_node >= 0 ? nt_ref(c->nt, sc->def_node, "parameters") : -1, nkw = 0;
    const int *kws = pn >= 0 ? nt_arr(c->nt, pn, "keywords", &nkw) : NULL;
    /* (keywords go to a keyword rest (`...` holds one), and the positional parameters take the positional arguments; a def
       without one takes them as one more positional Hash, which `**h` alone may not pass, as an open list's splat may not) */
    int kwrest = sc->kwrest_idx >= 0 || nkw || def_takes_forwarding(c->nt, sc->def_node);
    int dopen = open || (kw == NEWKW_MAYBE && !kwrest), dargc = argc + (kw == NEWKW_SURE && !kwrest);
    /* (each takes what the call passes, or CRuby raises ArgumentError there) */
    if (!(dopen ? sc->rest_idx >= 0 && sc->nrequired == 0 : cplan_def_takes_argc(c, mi, dargc))) return BNF_REFUSE;
    /* (and its `super` passes that Hash on as a positional argument: the builtin's initialize sees the call it would have seen
       only where it takes no keywords and makes the Hash a positional argument too (BC_INIT_NO_KEYWORDS), as Array's does; a
       `**h` passes that Hash only when it holds keys, which the plan does not tell apart: as the call's only argument it is
       refused, and beside positional arguments it is taken as passing none, right when empty) */
    if (lone && !kwrest && !builtin_init_no_keywords(c->classes[ci].name)) return BNF_REFUSE;
    if ((kw != NEWKW_SURE || kwrest || builtin_init_no_keywords(c->classes[ci].name)) && ct_init_forwards(c, mi, ci, dargc, dopen, kw_ok)) {
      /* (the defaults a forward fills are the next positional arguments of the one it passes them to; past one without a
         keyword rest, the literal keywords are a positional Hash ahead of its own defaults and every later one's) */
      int pos = ct_positionals(c, sc) - (sc->rest_idx >= 0);
      /* (the keywords a bare `super` passes, which the next takes: the fills ride through a forward that passes them on
         unchanged, one with a keyword rest and no keywords of its own, and that fills no positional default, which the
         rewrite would run ahead of them) */
      if (f->nk && (!kwrest || nkw || (!dopen && dargc < pos))) return BNF_REFUSE;
      if (kw == NEWKW_SURE && !kwrest) f->kwat = f->n;
      int n0 = f->n, e0 = effects, maybe = kw == NEWKW_MAYBE || (kw == NEWKW_SURE && ksplat), fill = !dopen && dargc < pos ? ct_fill_defaults(c, sc, mi, dargc, f, &effects) : 1, kfill = 1;
      if (fill && nkw) kfill = ct_fill_keywords(c, sc, mi, kws, nkw, id, kw, maybe, f, &effects);
      /* (a key this one does not name raises as it binds, before its own defaults run: what the forwards ahead fill has run;
         past the check of a `**h`'s keys at one ahead (rmi), which raises first where `h` holds a key that one does not name,
         the call would need both, which the rewrite does not make) */
      if (kfill == FILL_RAISE) {
        if (f->rmi >= 0) return BNF_REFUSE;
        f->n = n0, effects = e0, f->rmi = mi;
        if (unplaced || !effects) f->n = f->nk = 0, f->kwat = -1;
        return unplaced && effects ? BNF_REFUSE : BNF_RAISE;
      }
      if (!fill || !kfill) return BNF_REFUSE;
      /* (a `**h` may hold a key this one does not name, which raises as it binds, before its defaults and every later one's
         run) */
      if (maybe && nkw && sc->kwrest_idx < 0 && f->rmi < 0) f->rmi = mi, ek = e0;
      /* (a default that reads the object being built (self, an ivar, a call on self) or the def's lexical scope runs only once
         CRuby has made the object, which the builtin's construction does not model yet: the walk goes on, and the call keeps
         master's answer, which runs no default) */
      unplaced |= fill == FILL_UNPLACED || kfill == FILL_UNPLACED;
      argc = !dopen && dargc < pos ? pos : dargc;
      /* (the next takes what this one passes: past one without a keyword rest, the Hash it made, and no keywords; past one
         with keywords, those) */
      if (!kwrest) open = dopen, kw = NEWKW_NONE;
      if (nkw) kw = NEWKW_SURE, kwpass = 1;
      continue;
    }
    /* (the first that does not forward: an empty one builds the builtin's empty value, if it has one, from what the call
       passes; an open list's splat, keywords and block pass are evaluated by the rewrite, which refuses what it cannot, and
       the defaults the forwards ahead leave unfilled run after them where one has an effect, as their values go unread) */
    if (!cplan_body_empty(c, mi) || !builtin_empty_new(c->classes[ci].name) || sc->yields || !ct_params_plain(c, sc, 0, 0, 0))
      return BNF_REFUSE;
    if (unplaced || !effects) f->n = f->nk = 0, f->kwat = -1;
    /* (nothing past an empty one raises for a key: the check stays, ahead of the defaults it runs before, or after them where
       each with an effect is filled ahead of that forward; one ahead of it and another at or after it, the call is refused) */
    if (f->rmi >= 0 && !unplaced && ek) {
      if (effects != ek) return BNF_REFUSE;
      f->rlate = 1;
    }
    return BNF_EMPTY;
  }
  /* (the builtin's own takes the keywords a forward's bare `super` passes, or CRuby raises ArgumentError there) */
  if (kwpass && !builtin_new_keywords(c->classes[ci].name)) return BNF_REFUSE;
  if (unplaced) f->n = f->nk = 0, f->kwat = -1;
  if (!ct_key_check(f, unplaced ? 0 : effects, ek)) return BNF_REFUSE;
  return f->n || f->nk ? BNF_FILL : BNF_OWN;
}
/* cplan_builtin_new_form's answer, and the defaults of a BNF_FILL, BNF_EMPTY or BNF_RAISE into f */
static int ct_builtin_new_form(Compiler *c, int id, NewFills *f) {
  int ci, own, tmp[CPT_MAX];
  f->n = f->nk = f->rlate = 0, f->kwat = f->rmi = -1;
  /* (the empty value desugar_builtin_new_init_args builds once it has run the arguments is the builtin's own `new`) */
  if (id < 0 || id >= c->node_cap || nt_kind(c->nt, id) != NK_CallNode || !is_new_name(nt_str(c->nt, id, "name")) ||
      cplan_builtin_call(c, id)) return BNF_NONE;
  ci = cplan_new_class(c, id, &own);
  /* (a builtin exception's `new` runs the program's initialize itself: its emitter calls it; a program's `self.new` the call runs
     answers it, and calls initialize only where it says so) */
  if (ci < 0 || !is_builtin_reopen(c->classes[ci].name) || class_is_exc_subclass(c, ci) || is_builtin_exception_name(c->classes[ci].name) ||
      cplan_user_new(c, id, ci) >= 0 || ct_class_initializers(c, id, ci, 0, tmp) <= 0) return BNF_NONE;
  /* what the call passes: the count of positional arguments (a block pass and the keywords are none), or an open list (a
     splat, `...`) */
  int nargs = 0, argc = 0, args = nt_ref(c->nt, id, "arguments");
  const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &nargs) : NULL;
  int open = nt_argv_has(c->nt, av, nargs, NT_ARG_SPLAT | NT_ARG_FORWARDING), kw = ct_call_keywords(c->nt, av, nargs);
  for (int i = 0; i < nargs; i++) argc += nt_kind(c->nt, av[i]) != NK_BlockArgumentNode && nt_kind(c->nt, av[i]) != NK_KeywordHashNode;
  /* String's own `new` given keywords builds the String from them itself and runs no initialize (BC_KEYWORDS_NEW_OWN): a literal
     key gives it some; a `**h` alone gives none when empty, and the chain then runs without them, so only one that forwards
     answers both alike. A class method's `new` may build a subclass's, which runs it. */
  int kw_own = kw && !own && builtin_keywords_new_own(c->classes[ci].name);
  if (kw_own && kw == NEWKW_SURE) return BNF_NONE;
  if (kw_own) kw = NEWKW_NONE;
  /* the chain's head, as the plan answers it where the call runs (a prepended module's copy answers the name ahead of the class's own
     def); a def the call runs ahead of is not in force there (ct_new_chain_form) */
  int start = tmp[0];
  /* a prepend a later statement makes is chosen at run time by its guard (guard_chain): every chain it may run must reduce alike */
  if (c->scopes[start].mixin_guarded || c->scopes[start].entry == ENTRY_TABLE) {
    int to[VER_MAX_ARMS + 1], n = guard_chain(c, start, to, NULL, NULL), g = BNF_NONE, in = 0;
    NewFills nf[1];
    for (int k = 0; k < n; k++) {
      /* (the chain from to[k] runs where its prepend's copy is in force and none ahead of it is: one the load record proves
         either way at the call (ct_prepend_in_force) leaves the chains that cannot run out) */
      int y = c->scopes[to[k]].is_prepend_copy ? ct_prepend_in_force(c, id, ci, &c->scopes[to[k]]) : ELAB_UNKNOWN;
      if (y == ELAB_NO) continue;
      /* (a chain whose defaults run at the call is one only where no guard chooses it) */
      nf->n = nf->nk = nf->rlate = 0, nf->kwat = nf->rmi = -1;
      int fk = ct_new_chain_form(c, id, ci, to[k], argc, open, kw, nf);
      if (fk == BNF_NONE) return BNF_NONE;
      /* (and the forward that checks a `**h`'s keys (rmi), BNF_RAISE's or BNF_EMPTY's, is the same in each: the rewrite makes
         one check, which the chain that runs must make; BNF_OWN's builtin binding raises alike, rmi none) */
      if ((g != BNF_NONE && fk != g) || fk == BNF_FILL || nf->n || nf->nk || (in && nf->rmi != f->rmi)) return BNF_REFUSE;
      g = fk, f->rmi = nf->rmi, in = 1;
      if (y == ELAB_YES) break;
    }
    return kw_own && g != BNF_OWN ? BNF_REFUSE : g;
  }
  int g = ct_new_chain_form(c, id, ci, start, argc, open, kw, f);
  return kw_own && g != BNF_OWN && g != BNF_NONE ? BNF_REFUSE : g;
}
/* See call_plan.h. */
int cplan_builtin_new_form(Compiler *c, int id) {
  NewFills f;
  return ct_builtin_new_form(c, id, &f);
}
/* See call_plan.h. */
void cplan_builtin_new_fills(Compiler *c, int id, NewFills *f) {
  int g = ct_builtin_new_form(c, id, f);
  if (g != BNF_FILL && g != BNF_EMPTY && g != BNF_RAISE) f->n = f->nk = 0, f->kwat = -1;
}
/* Is call id a builtin class's `new` the plan does not answer: it runs a program
   initialize the builtin construction does not (the effect and the block_given? a
   yielding one sees are lost), builds its empty value from arguments still on
   the call, raises a forward's unknown keyword with its arguments still on it, or leaves the defaults a forward fills off it
   (desugar_builtin_new_init_args moves the arguments out and places the
   defaults first)? */
static int cplan_builtin_initialize_gap(Compiler *c, int id) {
  NewFills fl;
  int f = ct_builtin_new_form(c, id, &fl);
  return f == BNF_REFUSE || f == BNF_FILL || f == BNF_RAISE ||
         (f == BNF_EMPTY && (nt_ref(c->nt, id, "arguments") >= 0 || nt_ref(c->nt, id, "block") >= 0 || fl.n || fl.nk));
}

/* Is node n's value read: neither dropped where it stands (an_value_dropped) nor thrown away by the class bodies and
   the top level it is the value of (an_def_value_used, as for a def's)? */
static int cplan_value_read(Compiler *c, const int *par, int n) {
  return !an_value_dropped(c, par, n) && an_def_value_used(c->nt, par, n);
}
/* Is the value of definer call id read (its name's Symbol, or an attr's Array of them): where it stands, or, where
   it is the answer of the method its body is, through the class bodies it ends (a def's, or a define_method's or
   define_singleton_method's block), by
   a call of that method that reads it, or by any reader where a Symbol or a String names the method (elab_name_spelled,
   past the name the definer of a block gives it: a send of a name only the run tells is refused, reject_runtime_send)? */
static int cplan_definer_value_used(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const int *par = comp_parent_map(c);
  if (!cplan_value_read(c, par, id)) return 0;
  VpUp up;
  NodeKind k;
  for (int at = id; vp_climb(nt, par, at, VPS_ALL, &up) == VPU_ENDED && nt_ref(nt, up.parent, "body") == up.node &&
                    ((k = nt_kind(nt, up.parent)) == NK_ClassNode || k == NK_ModuleNode || k == NK_SingletonClassNode);)
    at = up.parent;   /* (a class body's value is its class's) */
  int d = vp_up_def(nt, &up), an = 0, ac = 0;
  int dc = d < 0 && up.kind == VPU_ENDED && nt_kind(nt, up.parent) == NK_BlockNode ? par[up.parent] : -1;
  const int *av = dc >= 0 && (is_define_method_name(nt_str(nt, dc, "name")) || is_define_singleton_method_name(nt_str(nt, dc, "name"))) &&
                  (an = nt_ref(nt, dc, "arguments")) >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  const char *hn = d >= 0 ? nt_str(nt, d, "name") : ac > 0 ? vis_arg_name(nt, av[0]) : NULL;
  if (!hn || elab_name_spelled(c, hn, ac > 0 ? av[0] : -1, 0)) return 1;
  for (int k = an_calls_named_first(c, hn); k >= 0; k = an_calls_named_next(k))
    if (cplan_value_read(c, par, k)) return 1;
  return 0;
}
static int ver_dm_keywords(const NodeTable *nt, int dm);
/* Why the readers of name nm cannot read the version definer call id makes (host: the body whose runs make it, on
   cls's side sg), or NULL: a reader that answers from the method tables, which hold the method from the start
   (walk_scope registers its scope there), or one that does not read a definer call's versions yet. They are the
   program's lists and readers of a name only the run tells (elab_body_reflected, which keeps a body def's version off
   the late bit too, and takes in every presence reader of the name where a respond_to_missing? is defined), a
   method_missing on the chain, which the entry's else arm does not call, a Method's maker of the name, the calls
   Ruby makes by itself (to_s, <=>, coerce), a hook of the program that would observe the definition
   (elab_definer_hook_live), a name every object has (is_object_method_name: the version would
   replace the builtin's only once the body runs), keyword parameters, which the method's scope does not bind
   (ver_dm_keywords), the call's own value, which it does not answer (cplan_definer_value_used), a class method's
   call from the class's other methods where a define_method's or a define_singleton_method's block makes it, and a
   super into it from a subclass's method. (An object's singleton method under cls bars the call in the record,
   lr_body_call_barred.) */
static const char *cplan_call_version_unread(Compiler *c, int id, int host, int cls, int sg, const char *nm) {
  const NodeTable *nt = c->nt;
  if (elab_body_reflected(c, nm, sg)) return "a method list, or a reflection reader of a name only the run tells, reads it";
  if (cplan_has_method_missing(c, id, cls, sg)) return "the class's chain has a method_missing";
  if (elab_method_captured(c, nm)) return "a Method or UnboundMethod of it is made";
  if (elab_definer_hook_live(c, cls, sg, nt_str(nt, id, "name"))) return "a method_added or singleton_method_added hook of the program may observe it";
  if (method_name_implicitly_invoked(nm)) return "Ruby calls it by itself (to_s, <=>, coerce and their kin)";
  if (is_object_method_name(nm)) return "every object has a method of the name (tap, then, dup and their kin)";
  if (ver_dm_keywords(nt, id)) return "its block takes keyword parameters";
  if (cplan_definer_value_used(c, id)) return "the call's value is read";
  const char *hc = nt_kind(nt, host) == NK_BlockNode ? nt_str(nt, comp_parent_map(c)[host], "name") : NULL;
  if (sg && (is_define_method_name(hc) || is_define_singleton_method_name(hc))) return "a define_method's or a define_singleton_method's block makes it";
  for (int s = 0; s < c->nscopes; s++) {
    const Scope *m = &c->scopes[s];
    if (!m->name || m->class_id < 0 || !m->is_cmethod != !sg || !sp_streq(m->name, nm) || !scope_calls_super(c, s)) continue;
    for (int a = c->classes[m->class_id].parent; a >= 0; a = c->classes[a].parent)
      if (a == cls) return "a subclass's method of the name calls super";
  }
  return NULL;
}

/* Could the program's own define_method (an_user_method_kinds) answer the receiverless define_method in
   host's body? One a module, a reopened Object or a top level defines, or an alias, could reach any self. In a class
   method self is the class object: a `def self.define_method` up its chain or a reopened Module's or Class's answers;
   in an instance method self is an instance, answered by its chain. One defined elsewhere -- an options object's
   accessor named define_method, where a config's method_missing names are written out -- answers neither: the call
   reaches Module#define_method, and the method registered is the one it defines. */
static int cplan_own_define_method_answers(Compiler *c, int host) {
  const Scope *hs = &c->scopes[host];
  int k = hs->class_id, kinds = an_user_method_kinds(c, DEFINE_METHOD_NAME);
  if (k < 0 || k >= c->nclasses || k == c->toplevel_class || (kinds & UDM_ANY_SELF)) return 1;
  if (!(kinds & (hs->is_cmethod ? UDM_CLASS_SELF : UDM_INSTANCE_SELF))) return 0;
  if (call_target(c, -1, k, DEFINE_METHOD_NAME, hs->is_cmethod ? CT_CMETHOD : CT_METHODS, NULL) >= 0) return 1;
  for (int j = 0; hs->is_cmethod && j < c->nclasses; j++) {
    const char *n = c->classes[j].name;
    if (n && (is_module_const_name(n) || is_class_const_name(n)) && is_builtin_reopen(n) &&
        call_target(c, -1, j, DEFINE_METHOD_NAME, CT_OWN, NULL) >= 0) return 1;
  }
  return 0;
}
/* Would the method scope dm that walk_scope registers from the start for a define_method in host's body (the record
   holds the call as no version; own: its name's node) answer a call CRuby answers otherwise? The early registration
   is right only for a method that is new: one the class does not answer before then. Anything else is: its own def
   or one up its chain, a module's, a subclass's, an alias, attr or Struct member (a Symbol or a String besides its
   own spells it), a method every object has, and, where the program's own define_method may answer the call
   (cplan_own_define_method_answers), any call of the name, which would find a method CRuby never defines. 0 where
   nothing is registered (a method nothing names, a branch that never runs). Its asks are the plan's own survey of
   the classes, made with no site and no CT_SHARED: the call's frame is open, and none of them is the call's question. */
static int cplan_define_method_early(Compiler *c, int host, int dm, int own) {
  if (dm <= 0 || dm >= c->nscopes) return 0;
  const Scope *ds = &c->scopes[dm];
  if (!ds->name || ds->class_id < 0 || is_object_method_name(ds->name)) return 1;
  int mi = call_target(c, -1, ds->class_id, ds->name, CT_METHODS, NULL);
  if (mi >= 0 && mi != dm) return 1;
  for (int t = an_scopes_named_first(c, ds->name); t >= 0; t = an_scopes_named_next(c, ds->name, t)) {
    const Scope *ts = &c->scopes[t];
    if (t == dm || ts->is_cmethod || ts->class_id < 0) continue;
    for (int k = ds->class_id, h = 0; k >= 0 && k < c->nclasses && h < 64; k = c->classes[k].parent, h++) {
      if (ts->class_id == k) return 1;
      for (int m = 0; m < c->classes[k].nincluded_mods; m++)
        if (c->classes[k].included_mods[m] == ts->class_id) return 1;
    }
    /* (a subclass's: a class method run on the subclass defines it there) */
    for (int k = ts->class_id, h = 0; k >= 0 && k < c->nclasses && h < 64; k = c->classes[k].parent, h++)
      if (k == ds->class_id) return 1;
  }
  return elab_name_spelled(c, ds->name, own, 0) ||
         (an_user_method_kinds(c, DEFINE_METHOD_NAME) && cplan_own_define_method_answers(c, host) &&
          an_calls_named_first(c, ds->name) >= 0);
}

/* May a written call of `name` (an_written_call) reach an instance of class k (an_call_may_reach_instance)? A
   singleton method of an object of class k that a define_method's block makes is reached only by a call on that
   object. */
static int cplan_instance_call_named(Compiler *c, const char *name, int k) {
  for (int id = an_calls_named_first(c, name); id >= 0; id = an_calls_named_next(id))
    if (an_written_call(id) && an_call_may_reach_instance(c, id, k)) return 1;
  return 0;
}
/* Can every local that a definer's scope closes over ride a closure cell (repr_cell_capturable)? */
static int cplan_env_cells_ok(Compiler *c, const Scope *dm) {
  for (int i = 0; dm->env_host && i < dm->nenv_names; i++) {
    LocalVar *lv = scope_local(&c->scopes[dm->env_host - 1], dm->env_names[i]);
    if (lv && !repr_cell_capturable(c, lv)) return 0;
  }
  return 1;
}
/* A definer call on self in a method's body that the plan cannot answer as a version (CRF_DEFINER), its words in buf,
   or NULL: a define_method or define_singleton_method in a def's body, or one that is a version
   (elab_body_call_version: in a define_method's or define_singleton_method's block too), whose block yields to that
   method's block (the method it defines is laid out as a method of its own, which has none; the locals it reads or
   writes it closes over, Scope.env_host), that reads or writes a local from outside it where the definer is inside another definer's block (the method's or the outer block's), or
   that closes over the method's locals where a run may define it on another class or object than the one it is
   registered on, or where the program aliases its name (an alias keeps the environment of one run), where the program reaches the name it defines
   (an_name_reached: a call, or a Symbol or String that may run it; one nothing reaches is never run in place
   of the block); a version a reader of its name cannot read
   (cplan_call_version_unread); a define_method version that replaces an attr_* version (elab_attr_replaced) in a
   class with a subclass, whose calls keep the accessor; a define_singleton_method on the object a define_method's block runs on
   (elab_body_self_object) that runs whenever the block does, where a call of its name may reach that object
   (cplan_instance_call_named) and the program defines no define_singleton_method of its own; and a
   define_method in a def's body the
   record holds as no version, whose body is a Proc or a Method value, which no method is made of, or where the
   method walk_scope registers from the start (elab_def_scope_of) answers a call CRuby answers otherwise
   (cplan_define_method_early), whether the def is reached or not (one a class-body macro's expansion leaves behind
   in its module's method, which an extend copies, answers from the start too). A method nothing reaches
   (Scope.reachable) never runs its versions, so no reader can find one early. A file no live require reaches
   (prog_node_in_dropped_file) defines nothing, and a define_method or attr_* whose self is an object
   (an_object_definer_missing) raises CRuby's NoMethodError (cplan_missing). */
static const char *cplan_body_definer_gap(Compiler *c, int id, char *buf, size_t cap) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  const Scope *hs = comp_scope_of(c, id);
  int dm = is_define_method_name(nm), dsm = is_define_singleton_method_name(nm), rv = nt_ref(nt, id, "receiver");
  int blk = nt_ref(nt, id, "block"), an = nt_ref(nt, id, "arguments"), ac = 0, cls = -1, sg = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  const char *mn = ac > 0 ? vis_arg_name(nt, av[0]) : NULL;
  if ((!dm && !dsm && !is_attr_name(nm)) || (rv >= 0 && nt_kind(nt, rv) != NK_SelfNode) || !mn || !hs ||
      prog_node_in_dropped_file(c, id) || an_object_definer_missing(c, id, hs))
    return NULL;
  int lit = blk >= 0 && nt_kind(nt, blk) == NK_BlockNode, def = elab_body_def(c, id) >= 0, run = hs->reachable;
  /* (no `.new` traces that object, so its singleton method is not made) */
  int sdm = dsm && run && !an_user_method_reaches(c, nm, 0) ? elab_body_self_object(c, id) : -1;
  if (sdm >= 0 && elab_runs_with_body(c, id, nt_ref(nt, sdm, "block")) && cplan_instance_call_named(c, mn, hs->class_id)) {
    snprintf(buf, cap, "Module#define_singleton_method in a define_method's block is not supported where a call of the "
             "name it defines may reach that object: its self is an object, whose singleton methods are not made (see "
             "docs/limitations.md)");
    return buf;
  }
  int host = elab_body_call_version(c, id, &cls, &sg);
  const char *envgap = !lit ? NULL : an_block_env_gap(nt, blk) ? an_block_env_gap(nt, blk) :
    /* (a definer in a define_method's block, a def around that block or not: the locals it closes over are that block's own, which have no cells yet, or reach the def's through that block's environment) */
    (!def || (hs->def_node >= 0 && nt_kind(nt, hs->def_node) != NK_DefNode)) && an_block_captures(nt, blk, 0) ? "reads or writes a local from outside it, where the definer is inside another definer's block" :
    /* (the environment is the definer's one, so every run must define on the class and side the method is registered on) */
    an_block_captures(nt, blk, 0) && !elab_body_call_owner(c, id) ? "reads or writes that method's locals where a run may define it on another class or object than the one it is registered on" :
    /* (an alias keeps the method it found: the environment of one run of the definer) */
    an_block_captures(nt, blk, 0) && an_aliases_name(c, mn, NULL) ? "reads or writes that method's locals where the program aliases the name it defines (the alias would keep the environment of one run)" :
    /* (a local of a type no closure cell holds, as a proc's capture is refused) */
    elab_def_scope_of(c, id) > 0 && !cplan_env_cells_ok(c, &c->scopes[elab_def_scope_of(c, id)]) ? "reads or writes a local of a type that a closure cell cannot hold" : NULL;
  if (run && (def || host >= 0) && envgap && (dm || dsm) && an_name_reached(c, mn, av[0])) {
    snprintf(buf, cap, "Module#%s in a method body with a block that %s is not supported (see docs/limitations.md)", nm, envgap);
    return buf;
  }
  char b[ELAB_FORM_NAME_CAP];
  const char *vn, *why = NULL;
  for (int k = 0; run && host >= 0 && !why && (vn = elab_call_form_name(nt, id, k, b, sizeof b, NULL)); k++)
    why = cplan_call_version_unread(c, id, host, cls, sg, vn);
  if (why) {
    snprintf(buf, cap, "Module#%s in a method body is not supported where %s: the method it defines would be "
             "found before that body runs (see docs/limitations.md)", nm, why);
    return buf;
  }
  int nd = 0;
  if (run && host >= 0 && dm && !sg && elab_attr_replaced(c, cls, 0, mn) && (comp_descendants(c, cls, &nd), nd > 0)) {
    snprintf(buf, cap, "Module#define_method in a method body that replaces an attr_* method is not supported where "
             "the class has a subclass: a call on the subclass would keep the accessor after that body runs (see "
             "docs/limitations.md)");
    return buf;
  }
  if (!dm || host >= 0 || hs->def_node < 0 || nt_kind(nt, hs->def_node) != NK_DefNode) return NULL;
  /* (a body the method is handed, `&block` or a parameter, has its own refusal, which --defer-refusals defers:
     define_method_param_body_refusal) */
  if (!lit && run && !define_method_param_body_refusal(c, id))
    snprintf(buf, cap, "Module#define_method in a method body with a Proc or a Method for its body is not supported (see "
             "docs/limitations.md)");
  else if (lit && cplan_define_method_early(c, (int)(hs - c->scopes), elab_def_scope_of(c, id), av[0]))
    snprintf(buf, cap, "Module#define_method in a method body is supported only for a method the class does not answer "
             "yet, where the load record does not order it against the calls: it is defined from the start, not when "
             "that method runs (see docs/limitations.md)");
  else
    return NULL;
  return buf;
}

/* in the order codegen meets them: the prepasses, then the emitters */
static void cplan_refuse_resolve(Compiler *c, int id, CallPlan *p, char *buf, size_t cap) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    int gap = cplan_bycount_gap(c, id);
    if (gap) {
      cplan_refuse_set(p, CRF_LIMIT, CR_FEATURE, cond_def_override_why(gap == BYCOUNT_FRONT), buf, cap);
      return;
    }
    const CallPlan sp = cplan_user(c, id);
    if (sp.dispatch == CP_REFUSE) {
      Scope *s = comp_scope_of(c, id);
      char msg[2400];
      snprintf(msg, sizeof msg, "%s.%s: %s", c->classes[s->class_id].name,
               comp_scope_user_name(s), sp.msg);
      cplan_refuse_set(p, sp.rfrom, sp.rkind, msg, buf, cap);
    }
    return;
  }
  if (k != NK_CallNode) return;
  const char *what = NULL;
  int from = CRF_NONE;
  if ((what = cplan_runtime_send_what(c, id))) from = CRF_SEND;
  else if ((what = cplan_runtime_const_get_what(c, id))) from = CRF_CONST_GET;
  else if ((what = cplan_binding_what(c, id))) from = CRF_BINDING;
  if (!what && cplan_top_self_gap(c, id)) {
    cplan_refuse_set(p, CRF_TOP_SELF, CR_FEATURE,
                     "a top-level method's self is not modelled: this call finds main's singleton method when the "
                     "method runs on main and Object's when it runs on another object", buf, cap);
    return;
  }
  if (!what && cplan_main_chain_unsure(c, id)) {
    cplan_refuse_set(p, CRF_MAIN_CHAIN, CR_FEATURE,
                     "a call of a name that a mixin of main or Object defines, where that mixin runs under a condition or "
                     "from a method that may not have run: CRuby finds its method only once the mixin has run", buf, cap);
    return;
  }
  char dwb[640];
  const char *dw = what ? NULL : cplan_body_definer_gap(c, id, dwb, sizeof dwb);
  if (dw) {
    cplan_refuse_set(p, CRF_DEFINER, CR_FEATURE, dw, buf, cap);
    return;
  }
  if (!what && cplan_record_new_gap(c, id)) {
    cplan_refuse_set(p, CRF_RECORD_NEW, CR_FEATURE,
                     "a Struct or Data class's `new` or `[]` the program defines, called where the record "
                     "cannot tell whether that def has run: CRuby runs it once it has, the builtin before", buf, cap);
    return;
  }
  if (!what && cplan_builtin_initialize_gap(c, id)) {
    cplan_refuse_set(p, CRF_LIMIT, CR_FEATURE,
                     "a builtin class's `new` does not run the initialize the program defines on it (a reopening's or a "
                     "prepended module's): CRuby calls it, the builtin construction does not", buf, cap);
    return;
  }
  /* the plan's capture of a module's class method it does not have, where
     the record cannot tell the class its NameError names, and a
     public_method's whose visibility nothing tells at the run */
  CallPlan mp = { .absent = CP_ABSENT_NONE };
  if (!what && is_method_object_maker(nt_str(c->nt, id, "name"))) mp = cplan_method_call(c, id);
  if (mp.absent == CP_ABSENT_UNTOLD) {
    const char *cn = class_ruby_name(c, mp.owner_ci);
    char msg[640];
    snprintf(msg, sizeof msg, "a Method of a module's class method it does not have, made where the program does not tell "
             "whether the module's singleton class is made yet (a form that makes it may run before): "
             "CRuby's NameError names the class `#<Class:%s>` once it is, `Module` before", cn ? cn : c->classes[mp.owner_ci].name);
    cplan_refuse_set(p, CRF_CAPTURE, CR_FEATURE, msg, buf, cap);
    return;
  }
  if (mp.closed == CP_CLOSED_UNTOLD) {
    cplan_refuse_set(p, CRF_CAPTURE, CR_FEATURE,
                     "a public_method of a class method whose visibility a method body's word sets, made where the "
                     "program does not tell it at the run: CRuby raises NameError where the method is not public", buf, cap);
    return;
  }
  int gap = what ? 0 : cplan_bycount_gap(c, id);
  if (gap) {
    cplan_refuse_set(p, CRF_LIMIT, CR_FEATURE, cond_def_override_why(gap == BYCOUNT_FRONT), buf, cap);
    return;
  }
  if (!what && cplan_capture_gap(c, id)) {
    cplan_refuse_set(p, CRF_CAPTURE, CR_FEATURE,
                     "a Method of a Struct or Data class's class method, made where the method's def may not have "
                     "run yet: CRuby raises NameError before the def runs", buf, cap);
    return;
  }
  if (!what) {
    int stop;
    char why_buf[512];
    const char *why = cplan_feature_why(c, id, &stop, why_buf, sizeof why_buf);
    /* an object's singleton support (extend_module_is_a, singleton_dsm_local)
       answers these first; the limit is the refusal only once it declines,
       which codegen alone knows */
    const char *nm = why ? nt_str(c->nt, id, "name") : NULL;
    if (nm && (sp_streq(nm, "extend") || sp_streq(nm, "define_singleton_method"))) why = NULL;
    if (why) { cplan_refuse_set(p, CRF_LIMIT, CR_FEATURE, why, buf, cap); return; }
  }
  if (!what && (what = cplan_eval_what(c, id))) from = CRF_EVAL;
  if (!what && cplan_io_reopen_yields(c, id)) { what = "call"; from = CRF_IO_REOPEN; }
  if (!what && cplan_bind_call_gap(c, id)) { what = "call"; from = CRF_BIND_CALL; }
  if (!what) return;
  /* the prepasses run before any class is emitted; the emitters' refusals
     here are no bare name's NameError, the one wording that reads it */
  char msg[2400];
  int rk = unsup_message(c, id, what, -1, msg, sizeof msg);
  cplan_refuse_set(p, from, rk, msg, buf, cap);
}

CallPlan cplan_refuse(Compiler *c, int id) {
  CallPlan res;
  if (id < 0 || id >= c->node_cap) { cplan_set(&res, -1, -1, UC_NONE, CP_NONE); return res; }
  int plain = cplan_plain_ctx(), renamed = 0;
  const char *nm = nt_str(c->nt, id, "name");
  const CallPlan *k = plain && c->cplan_memo ? cplan_table_get(&c->cplan_memo->refuse, id, nm, &renamed) : NULL;
  if (k) return *k;
  if (renamed) plain = 0;
  char msg[2400];
  cplan_refuse_resolve(c, id, &res, msg, sizeof msg);
  if (res.msg) res.msg = cplan_keep(c, strdup(res.msg));
  if (plain) cplan_table_put(&cplan_memo(c)->refuse, id, res, nm);
  return res;
}

/* ---- CP_POLY ---- */

static void cpoly_family(PolyPlan *p, int *cap, int fam);
static void cpoly_trial(PolyPlan *p, int *cap, int t);
static int cpoly_str_trial(Compiler *c, int id, const char *name, int argc, const int *argv,
                           const TyKind *atmp_ty, TyKind ret, const PolyPlan *p);
static int cpoly_cls_value_arms(Compiler *c, int id, const char *name, int argc, const PolyArgs *A,
                                TyKind ret, int has_tr, PolyPlan *p, int *cap);

static PolyConv cpoly_conv(TyKind ret, TyKind vty) {
  if (ret == TY_POLY && vty != TY_POLY) return PC_BOX;
  if (ret != TY_POLY && vty == TY_POLY) return PC_UNBOX;
  return PC_SAME;
}

static void cpoly_add(PolyPlan *p, int *cap, int kind, int key, int mi, TyKind vty, int conv) {
  if (p->n == *cap) {
    *cap = *cap ? *cap * 2 : 8;
    p->arm = realloc(p->arm, (size_t)*cap * sizeof *p->arm);
  }
  PolyArm *a = &p->arm[p->n++];
  a->kind = (unsigned char)kind; a->key = (short)key; a->mi = mi;
  a->vty = (unsigned char)vty; a->conv = (unsigned char)conv; a->def = -1; a->vis = 0; a->unsettled = 0;
}

/* The classes a dispatch of name can take an arm for, ascending: the ones
   whose chain has the method or a reader, and the native ones (the
   memoized candidates); every class while an exception reopening can lend
   its method to a class outside them. Into ks (sized nclasses). */
/* ...less the classes the receiver never holds (cls_fact_excludes, #8530):
   their arms could not run, and an arm's parameters, typed by the calls
   that can reach it, need not take this call's arguments */
static int cpoly_arm_classes(Compiler *c, int id, const char *name, int *ks) {
  int n = 0, recv = nt_ref(c->nt, id, "receiver");
  if (any_exc_reopen(c) || (is_element_access(name) && is_store_alias(name))) {
    for (int k = 0; k < c->nclasses; k++)
      if (!cls_fact_excludes(c, recv, k)) ks[n++] = k;
    return n;
  }
  int npc = 0;
  const PolyCand *pcs = comp_poly_candidates_at(c, id, name, &npc);
  for (int i = 0; i < npc; i++) ks[n++] = pcs[i].cls;
  return n;
}

/* Does a class whose Class object may be a value (comp_class_may_be_value:
   held by a variable, a box or a container, answered by `x.class` and its
   kin) take an arm in call `id`'s poly dispatch of name? A boxed Class value
   carries its class's index as cls_id, so the key keeps it off the arms of
   instances (PPF_KEY_CLSV). */
static int cpoly_key_clsv(Compiler *c, int id, const char *name) {
  int *ks = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
  if (!ks) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int n = cpoly_arm_classes(c, id, name, ks), hit = 0;
  for (int i = 0; i < n && !hit; i++) hit = comp_class_may_be_value(c, ks[i]);
  free(ks);
  return hit;
}

/* Struct's builtin writer has no method scope, but still owns an arm. */
int cplan_struct_aset(Compiler *c, int cid, const char *name, int argc) {
  ClassInfo *k = &c->classes[cid];
  return argc == 2 && (is_element_access(name) && is_store_alias(name)) && k->instantiated &&
         k->is_struct && !k->is_data && !k->is_native_class &&
         !call_answers(c, -1, cid, name, 0, NULL);
}

/* The user-class arms of a zero-argument poly dispatch, in emission order.
   The plan supplies each class's method, reader, native binding or refusal. */
static void cpoly_user_arms0(Compiler *c, int id, const char *name, TyKind ret, PolyPlan *p) {
  int cap = 0, slot_oint = -1;
  int *ks = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
  int nks = cpoly_arm_classes(c, id, name, ks);
  for (int ki = 0; ki < nks; ki++) {
    int k = ks[ki];
    /* A never-instantiated class can't be this poly value's runtime class,
       so drop its arm (method or reader); the referenced symbol then DCEs
       as an unreferenced static (#1608). A primitive reopen's instances
       exist without any constructor, so it keeps its arm (#4219). */
    if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) continue;
    /* native (C-backed) class arm: dispatch a declared no-arg method to its
       C symbol on the cast receiver, coercing the result into the slot. */
    if (comp_native_binding_class(c, k, name) >= 0) {
      int nmi = comp_native_method_find(c, k, name, 0, 0);
      /* only a binding that takes no argument: the lookup falls back to
         any arity, and StringScanner#peek(len) beside two user `peek`s
         got an arm calling it with none, which C refused (#5359). A
         receiver of that class then lands in the default arm. */
      if (nmi >= 0 && native_takes(&c->native_methods[nmi], 0)) {
        TyKind mret = repr_native_arm_handle(c, id, k) ? TY_STRBUF : comp_native_ret_ty(c, nmi, k);
        cpoly_add(p, &cap, PA_NATIVE, k, -1, mret, mret == TY_NIL ? PC_VOID : cpoly_conv(ret, mret));
      }
      /* one that needs arguments is CRuby's ArgumentError for this call */
      else if (nmi >= 0 && c->native_methods[nmi].nargs > 0 && c->classes[k].instantiated)
        cpoly_add(p, &cap, PA_ARITY, k, -1, TY_UNKNOWN, PC_VOID);
      continue;
    }
    MethodLookup ml = { .def_class = -1 };
    int mi = call_target(c, id, k, name, 0, &ml), defcls = ml.def_class;
    /* an exception class with no method of its own for the name takes
       the builtin reopening's (`class Exception; def brief`) */
    if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
      int xd = exc_arm_definer(c, k, name);
      if (xd >= 0) { defcls = xd; mi = call_target(c, id, xd, name, 0, NULL); }
    }
    /* A Struct's synthesized each or each_pair, called with no block on a
       boxed receiver: the typed receiver answers an Enumerator over the
       members (its blockless each is redirected through __enum_to_a, its
       each_pair through to_h), and the synthesized method, which yields,
       raised LocalJumpError when this arm called it with no block. The
       each Enumerator is built over the member array the generated
       per-class dispatch answers, the each_pair one over the member hash
       the to_h hook answers, so it yields [name, value] pairs. Not for a
       Data class, which has neither name in CRuby. */
    if (mi >= 0 && c->classes[k].is_struct && !c->classes[k].is_data && g_gen_obj_struct_values &&
        ret == TY_POLY && nt_ref(c->nt, id, "block") < 0 &&
        nt_str(c->nt, c->scopes[mi].def_node, "synth") &&
        (is_each_or_pair(name))) {
      cpoly_add(p, &cap, PA_SYNTH_ENUM, k, mi, TY_ENUMERATOR, PC_SAME);
      continue;
    }
    /* Skip a method with no standalone definition to call: DCE-pruned (its
       params stayed TY_UNKNOWN so it was marked unreachable) or inlined at
       call sites because it yields. Emitting a `case` arm that calls the
       absent `sp_Class_method` symbol dangles at link (issue #1583). The
       class can never be the receiver of this poly value anyway. */
    /* A yielding candidate has a proc form emitted for exactly this
       dispatch (#3399); it is as callable as any other symbol here. */
    { char zexp[600];
      if (poly_arm_refuses_none(c, mi, zexp, sizeof zexp) && !call_attr_target(c, id, k, name, CT_ATTR, NULL)) {
        cpoly_add(p, &cap, PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID);
        continue;
      } }
    /* call_target ranks a def and a reader by which class defines
       them nearer; the reader below takes the arm when it wins (#7248). */
    if (mi >= 0 && c->scopes[mi].nrequired == 0 &&
        (!call_attr_target(c, id, k, name, 0, NULL)) &&
        (scope_has_callable_symbol(c, mi) || scope_needs_proc_form(c, mi)) &&
        !(c->classes[defcls].name && sp_streq(c->classes[defcls].name, "Class"))) {
      /* A proc form is a separately inferred clone, so its own return type
         is the one to read -- not the original's, which an inlined-only
         method never needed (#3399). */
      int pfi = scope_proc_form_of(c, mi);
      Scope *ms = &c->scopes[pfi >= 0 ? pfi : mi];
      TyKind cret = repr_method_ret_storage(ms);
      int conv;
      if (method_is_void(ms)) conv = PC_VOID;
      else {
        TyKind slotty = is_scalar_ret(ret) ? ret : TY_INT;
        conv = cpoly_conv(ret, cret);
        if (conv == PC_SAME && cret == TY_BIGINT && (slotty == TY_INT || slotty == TY_FLOAT)) conv = PC_NUM;
        /* a String's handle into a plain String slot is read, as a reader's is */
        else if (conv == PC_SAME && cret == TY_STRBUF && ret == TY_STRING) conv = PC_COPY;
      }
      cpoly_add(p, &cap, pfi >= 0 ? PA_PROC_FORM : PA_USER, k, mi, cret, conv);
      p->arm[p->n - 1].def = (short)defcls;
      continue;
    }
    MethodLookup rml = { .def_class = -1 };
    if (call_attr_target(c, id, k, name, CT_ATTR, &rml)) {
      int rdcls = rml.def_class;
      const char *rn = comp_resolve_alias(c, k, name);
      char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn);
      int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
      TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
      /* an Integer or Float ivar with a nil bit goes with its nil into a
         boxed result or a result slot that holds one (an sp_oint), as the
         arm emitter writes it; one without a nil bit holds no nil, and
         a boxed result takes its plain box */
      int nilbit = ivx >= 0 && oint_kind(ivt) && ivar_has_nilbit(c, rdcls, ivx);
      /* (the call's result slot, one for every arm: asked once) */
      if (slot_oint < 0) slot_oint = is_scalar_ret(ret) && oint_kind(ret) && node_is_oint(c, id);
      int conv = nilbit && (ret == TY_POLY || slot_oint) ? PC_BOX_OR_NIL
               : ivt == TY_STRBUF && ret == TY_STRING && cpoly_conv(ret, ivt) == PC_SAME ? PC_COPY
               : cpoly_conv(ret, ivt);
      cpoly_add(p, &cap, PA_READER, k, -1, ivt, conv);
      p->arm[p->n - 1].def = (short)rdcls;
    }
  }
  free(ks);
}

static int cpoly_native_conv(TyKind mret, TyKind ret, int is_setter_val) {
  if (mret == TY_NIL || is_setter_val) return PC_VOID;
  return cpoly_conv(ret, mret);
}

/* Can user arm `ks` take the call's argument types? Two different concrete
   kinds, either of them a builtin pointer kind (a Hash, an Exception, a Proc,
   a Regexp, an Array variant), cannot be passed raw: the other is a different
   C struct, a scalar, or an `sp_PtrArray *` holding other elements, so the
   arm is dropped, in either direction. Two object arrays stay (one
   `sp_PtrArray *`). The same test is
   arm_key_incompat in poly_user_arm_n_replay (codegen_poly_plan.c); the two
   must stay identical. */
int cplan_arm_args_fit(Compiler *c, Scope *ks, const ArgLayout *L, int pos_argc,
                              const TyKind *atmp_ty, int kwall_any) {
  for (int a = 0; a < ks->nparams; a++) {
    if (L->from[a] != ARG_NODE || L->arg[a] >= pos_argc) continue;
    LocalVar *pv0 = (ks->pnames && ks->pnames[a]) ? scope_local(ks, ks->pnames[a]) : NULL;
    TyKind pt0 = pv0 ? pv0->type : TY_UNKNOWN;
    TyKind at0 = atmp_ty[L->arg[a]];
    if (pt0 == TY_STRBUF && pv0->str_shared && (at0 == TY_STRING || at0 == TY_STRBUF)) continue;
    /* --share-strings: a handle into a String parameter passes its bytes
       (emit_poly_temp_as reads the held handle) */
    if (pt0 == TY_STRING && at0 == TY_STRBUF) continue;
    int pc = pt0 != TY_POLY && pt0 != TY_UNKNOWN && pt0 != TY_NIL && pt0 != TY_VOID;
    int ac = at0 != TY_POLY && at0 != TY_UNKNOWN && at0 != TY_NIL && at0 != TY_VOID;
    if (pc && ac && pt0 != at0 &&
        (pt0 == TY_STRING || at0 == TY_STRING || needs_root(pt0) != needs_root(at0) ||
         ty_is_object(pt0) != ty_is_object(at0) ||
         (ty_is_object(pt0) && ty_is_object(at0) &&
          obj_class_unrelated(c, ty_object_class(pt0), ty_object_class(at0))) ||
         ty_is_struct_valued(pt0) || ty_is_struct_valued(at0) ||
         (pt0 == TY_BOOL) != (at0 == TY_BOOL) ||
         (ty_is_builtin_ptr(pt0) || ty_is_builtin_ptr(at0))))
      return 0;
  }
  int kslot = L->kwh_slot;
  if (kslot >= 0 && L->from[kslot] != ARG_BY_NAME && ks->pnames && ks->pnames[kslot]) {
    LocalVar *kpv = scope_local(ks, ks->pnames[kslot]);
    TyKind kpt = kpv ? kpv->type : TY_UNKNOWN;
    if (!kwh_param_fits(kpt, kwall_any)) return 0;
  }
  return 1;
}

/* The builtin cases emit_poly_cases_n writes after the class arms of a
   dispatch with arguments. */
static void cpoly_cases_n(Compiler *c, int id, const char *name, int argc, const int *argv, TyKind ret,
                          const TyKind *atmp_ty, const PolySpecialsN *ps, int splat_a, PolyPlan *p, int *cap) {
  int kwh = ps->kwh, plain = kwh < 0 && splat_a < 0;
  if (ps->index) cpoly_family(p, cap, PB_INDEX_CASES);
  if (sp_streq(name, "timeout=") && argc == 1 && plain) cpoly_family(p, cap, PB_IO_TIMEOUT);
  if (sp_streq(name, "read_nonblock") && ps->pos_argc == 1 && splat_a < 0) cpoly_family(p, cap, PB_IO_READ_NB);
  if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc == 1 && plain)
    cpoly_family(p, cap, PB_IO_READPARTIAL);
  if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc == 2 && plain)
    cpoly_family(p, cap, PB_IO_READPARTIAL_BUF);
  if (sp_streq(name, "setsockopt") && argc == 3 && plain) cpoly_family(p, cap, PB_IO_SETSOCKOPT);
  if (sp_streq(name, "write") && argc == 1 && plain) cpoly_family(p, cap, PB_IO_WRITE);
  if (is_io_syswrite(name) && argc == 1 &&
      (plain || (splat_a == 0 && kwh < 0 && atmp_ty[0] == TY_POLY_ARRAY)))
    cpoly_family(p, cap, PB_IO_SYSWRITE);
  if ((is_text_print(name)) && plain) cpoly_family(p, cap, PB_IO_PRINT);
  if (sp_streq(name, "putc") && argc == 1 && plain) cpoly_family(p, cap, PB_IO_PUTC);
  if (((sp_streq(name, "seek") && (argc == 1 || argc == 2)) ||
       (sp_streq(name, "read") && argc == 1 && (ret == TY_POLY || ret == TY_STRING))) && plain) {
    int int_args = 1;
    for (int a = 0; a < argc; a++)
      if (atmp_ty[a] != TY_INT && atmp_ty[a] != TY_POLY) int_args = 0;
    if (int_args) cpoly_family(p, cap, PB_IO_SEEK_READ);
  }
  /* read(*args): the same File arm, reading the gathered list (emit_poly_cases_n) */
  if (sp_streq(name, "read") && argc == 1 && splat_a == 0 && kwh < 0 && (ret == TY_POLY || ret == TY_STRING))
    cpoly_family(p, cap, PB_IO_SEEK_READ);
  if (ps->unshift) cpoly_family(p, cap, PB_UNSHIFT);
  if (ps->push) cpoly_family(p, cap, PB_PUSH);
  if (ps->ppack && !sp_streq(name, "unpack1")) cpoly_family(p, cap, PB_PACK);
  if (ps->pjoin) cpoly_family(p, cap, PB_JOIN_N);
  if (ps->include) cpoly_family(p, cap, PB_INCLUDE_CASES);
  if (ps->arr_index && argc == 1) cpoly_family(p, cap, PB_ARR_INDEX);
  if (ps->intersect) cpoly_family(p, cap, PB_INTERSECT);
  if (ps->strftime) cpoly_family(p, cap, PB_STRFTIME);
  /* a Hash's `[]` or fetch, by the key's kind */
  int aref = (sp_streq(name, "[]") && argc == 1 && splat_a < 0) ||
             (sp_streq(name, "fetch") && (argc == 1 || argc == 2) && splat_a < 0 &&
              nt_ref(c->nt, id, "block") < 0);
  TyKind kt = aref ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  if (aref && kt == TY_STRING) cpoly_family(p, cap, PB_AREF_STR);
  if (aref && kt == TY_SYMBOL) cpoly_family(p, cap, PB_AREF_SYM);
  if (aref && (kt == TY_POLY || kt == TY_UNKNOWN)) cpoly_family(p, cap, PB_AREF_POLY);
  if (ps->pred) cpoly_family(p, cap, PB_PRED_N);
  /* the `default:` arm (emit_poly_defaults_n) */
  int is_aref = sp_streq(name, "[]") && argc == 1 && splat_a < 0;
  int is_aref2 = sp_streq(name, "[]") && argc == 2 && splat_a < 0;
  int is_fetch = sp_streq(name, "fetch") && (argc == 1 || argc == 2) && splat_a < 0 &&
                 nt_ref(c->nt, id, "block") < 0;
  if (!ps->pred && !(ps->strftime && ps->ncand == 0) && !is_aref && !is_aref2 && !is_fetch && !ps->include && !ps->push &&
      !ps->cover && !ps->gcdlcm && !ps->strdel && !ps->strsplit && !ps->pdelete && !ps->pdig &&
      !ps->pvalues_at && !ps->pfirstn && !ps->pmerge) {
    cpoly_family(p, cap, PB_ND_GENERIC);
    if (sp_streq(name, "replace") && argc == 1 && splat_a < 0 && ret == TY_POLY) cpoly_family(p, cap, PB_ND_REPLACE);
    /* a blockless upto / downto's Enumerator, as emit_poly_defaults_n takes it */
    else if ((sp_streq(name, "upto") || sp_streq(name, "downto")) && argc == 1 && ps->pos_argc == 1 &&
             splat_a < 0 && nt_ref(c->nt, id, "block") < 0 && ret == TY_POLY)
      cpoly_family(p, cap, PB_ND_UPTO);
    else if (is_round_family(name) && argc == 1 && splat_a < 0)
      cpoly_family(p, cap, PB_ND_ROUND);
    /* by the positional count, as emit_poly_defaults_n takes the arm */
    else if (splat_a < 0 && poly_num_arm(name, ps->pos_argc) >= 0) cpoly_family(p, cap, PB_ND_NUM);
    else if (ps->arr_index && !sp_streq(name, "find_index")) cpoly_family(p, cap, PB_ND_STR_INDEX);
    else if (ps->arr_index && argc == 1) cpoly_family(p, cap, PB_ND_FIND_INDEX);
  }
  else if (ps->pfirstn) { if (ret == TY_POLY) cpoly_family(p, cap, PB_ND_FIRSTN); }
  else if (ps->pdelete || ps->pdig || ps->pvalues_at) { if (ret == TY_POLY) cpoly_family(p, cap, PB_ND_KEYS); }
  else if (ps->pmerge) cpoly_family(p, cap, PB_ND_MERGE);
  else if (is_aref2) cpoly_family(p, cap, PB_ND_AREF2);
  else if (is_aref || is_fetch) cpoly_family(p, cap, PB_ND_AREF);
}

/* The pre-arms of a dispatch with arguments (emit_poly_prearms_n, then
   emit_poly_prearms_n_blk): the tag pre-arm families, a class value's
   class-side arms, a callable value's call. */
/* The generic `default:` tail of a poly dispatch with arguments when the
   Object (or Kernel) reopening's method of the name refuses the call's
   count and no builtin class has the name, for any receiver no class arm
   took, in place of the builtin surface's trial: the arity raise (as an
   arm, one of the targets reachability keeps), or, where its visibility
   refuses the call first, CRuby's NoMethodError (PA_VIS). 1 when added. */
static int cpoly_obj_default_n(Compiler *c, int id, const char *name, int kwh, int pos_argc, int splat_a,
                               PolyPlan *p, int *cap) {
  if (builtin_object_method_known(name) || builtin_instance_name_known(name)) return 0;
  MethodLookup ml = { .def_class = -1 };
  int obj_cls = comp_core_class(c, CORE_OBJECT);
  int obj_mi = obj_cls >= 0 ? call_target(c, id, obj_cls, name, 0, &ml) : -1;
  char exp[600];
  if (obj_mi < 0 || ml.def_class != obj_cls ||
      poly_arm_count(c, &c->scopes[obj_mi], kwh, pos_argc, splat_a, exp, sizeof exp) >= 0)
    return 0;
  /* send reaches a private one; an explicit receiver does not, and every
     caller's self is an Object, so a protected one refuses only
     public_send */
  const NodeTable *nt = c->nt;
  int enforce = nt_str(nt, id, "vis_enforce") != NULL;
  int blind = !enforce && (nt_str(nt, id, "send_blind") || nt_ignore_visibility(nt, id));
  int vis = blind ? SP_VIS_PUBLIC : cplan_method_vis(c, id, obj_cls, name, 0, NULL);
  if (vis == SP_VIS_PROTECTED && !enforce) vis = SP_VIS_PUBLIC;
  /* ... as the method has it from the first point the call can run (its
     statement's, or that of the def holding it) to the end of the load
     phase: a visibility change at or after that point leaves a call that
     runs before it reaching the method, which the visibility at the end of
     the load phase does not tell apart, so that call keeps the count's
     raise */
  LoadPoint from = elab_point_from(c, id);
  if (vis != SP_VIS_PUBLIC &&
      (from.region < 0 || elab_method_vis_held_from(c, c->scopes[obj_mi].class_id, name, 0, from) != vis))
    vis = SP_VIS_PUBLIC;
  if (vis == SP_VIS_PUBLIC) cpoly_add(p, cap, PA_ARITY, PA_KEY_DEFAULT, obj_mi, TY_UNKNOWN, PC_VOID);
  else {
    cpoly_add(p, cap, PA_VIS, PA_KEY_DEFAULT, -1, TY_UNKNOWN, PC_VOID);
    p->arm[p->n - 1].vis = (unsigned char)vis;
  }
  return 1;
}

static void cpoly_prearms_n(Compiler *c, int id, const char *name, int argc, const int *argv, TyKind ret,
                            const TyKind *atmp_ty, int kwall_any, int is_setter_val, PolyPlan *p, int *cap) {
  const NodeTable *nt = c->nt;
  PolySpecialsN ps;
  poly_specials_n(c, id, name, argc, argv, &ps);
  int splat_a = -1, splat_last = 0;
  poly_specials_n_splat(c, argv, &ps, &splat_a, &splat_last);
  int pos_argc = ps.pos_argc, kwh = ps.kwh;
  if (ps.cover) cpoly_family(p, cap, PB_COVER);
  if (ps.ctryconv) cpoly_family(p, cap, PB_TRY_CONVERT);
  if (ps.gcdlcm) cpoly_family(p, cap, PB_GCDLCM);
  if (ps.unpack1 && ps.str_arm == CP_STR_BUILTIN) cpoly_family(p, cap, PB_UNPACK1);
  if (ps.include) cpoly_family(p, cap, PB_INCLUDE);
  if (ps.str_arm == CP_STR_BUILTIN) {
    if (ps.strdel && (ret == TY_POLY || ret == TY_STRING)) cpoly_family(p, cap, PB_STR_DELETE);
    if (ps.strpart && (ret == TY_POLY || ret == TY_STR_ARRAY)) cpoly_family(p, cap, PB_STR_PARTITION);
    if (ps.strencode && pos_argc <= 2 && (ret == TY_POLY || ret == TY_STRING)) cpoly_family(p, cap, PB_STR_ENCODE);
    if (ps.strsplit && (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY))
      cpoly_family(p, cap, PB_STR_SPLIT_N);
  }
  if (ps.strsetop_n && ps.str_arm != CP_STR_NONE &&
      (sp_streq(name, "count") ? ret == TY_POLY || ret == TY_INT : ret == TY_POLY || ret == TY_STRING))
    cpoly_family(p, cap, PB_STR_SETOP);
  if (ps.pstore && ret == TY_POLY) cpoly_family(p, cap, PB_STORE);
  if (ps.index) cpoly_family(p, cap, PB_INT_BITREF);
  /* the class-side arms read the keyword values' types when the keywords
     split off one by one */
  int kwn = 0;
  const int *kwels = NULL;
  TyKind *kwty = NULL;
  if (kwh >= 0 && !ps.kw_ds) {
    kwels = nt_arr(nt, kwh, "elements", &kwn);
    kwty = malloc(sizeof(TyKind) * (size_t)(kwn > 0 ? kwn : 1));
    for (int e = 0; e < kwn; e++) {
      int val = nt_ref(nt, kwels[e], "value");
      TyKind at = val >= 0 ? repr_of(c, val).as_ty : TY_NIL;
      kwty[e] = at == TY_NIL || at == TY_VOID || at == TY_UNKNOWN ? TY_POLY : at;
    }
  }
  PolyKw kw = { kwh, kwn, ps.kw_ds ? 0 : -1, kwels, NULL, kwty, kwall_any };
  PolyArgs A = { argv, pos_argc, NULL, atmp_ty, &kw, NULL };
  cpoly_cls_value_arms(c, id, name, pos_argc, &A, ret, !is_setter_val, p, cap);
  free(kwty);
  int callable = is_call_or_yield(name);
  /* (of any argument count: one past the channel's slots rides the call's
     wide store) */
  if (callable && ((!ps.has_splat_arg && (ps.kw_pos || kwh >= 0)) || splat_last))
    cpoly_family(p, cap, PB_CALLABLE);
  if (ps.kw_pos && ps.straset && ps.str_arm == CP_STR_BUILTIN) cpoly_family(p, cap, PB_STR_ASET);
  if (ps.kw_pos && !ps.has_splat_arg && ps.str_arm == CP_STR_BUILTIN &&
      cpoly_str_trial(c, id, name, argc, argv, atmp_ty, ret, p))
    cpoly_trial(p, cap, PT_STR);
  cpoly_cases_n(c, id, name, argc, argv, ret, atmp_ty, &ps, splat_a, p, cap);
  if (cplan_poly_has_family(p, PB_ND_GENERIC) && !cplan_poly_has_family(p, PB_ND_REPLACE) &&
      !cplan_poly_has_family(p, PB_ND_UPTO) &&
      !cplan_poly_has_family(p, PB_ND_ROUND) && !cplan_poly_has_family(p, PB_ND_NUM) &&
      !cpoly_obj_default_n(c, id, name, kwh, pos_argc, splat_a, p, cap))
    cpoly_trial(p, cap, PT_GENERIC_TAIL);
  /* the families that write the switch's `default:` themselves */
  static const int dflt[] = { PB_ND_GENERIC, PB_ND_FIRSTN, PB_ND_KEYS, PB_ND_MERGE, PB_ND_AREF2, PB_ND_AREF,
                              PB_PUSH, PB_INCLUDE_CASES, PB_PRED_N };
  int has_default = ps.cover || (ps.strftime && ps.ncand == 0);
  for (size_t i = 0; i < sizeof dflt / sizeof dflt[0]; i++) has_default |= cplan_poly_has_family(p, dflt[i]);
  if (!is_setter_val && !has_default) cpoly_trial(p, cap, PT_DEFAULT_N);
}

/* A String-typed slot may already hold the shared handle. The argument
   temp keeps that representation so boxed arms keep its identity too. So
   does a value whose read yields the handle (Repr.handle: a call a handle
   parameter of one of the arms asked it of): held as the String, the temp
   was declared the bytes and initialized with the handle. */
TyKind cplan_poly_arg_storage(Compiler *c, int id) {
  Repr r = repr_of(c, id);
  if (r.strbuf_src == RS_SLOT_POLY) return TY_POLY;
  if (r.as_ty == TY_STRING && (r.strbuf_src == RS_HANDLE || r.handle)) return TY_STRBUF;
  return r.as_ty;
}

/* The arms the user-class loop of a poly dispatch with arguments writes. */
static void cpoly_user_arms_n(Compiler *c, int id, const char *name, int argc, const int *argv,
                              TyKind ret, PolyPlan *p, int full) {
  const NodeTable *nt = c->nt;
  int cap = 0;
  int has_splat_arg = nt_argv_has_splat(nt, argv, argc);
  int kwh = -1, pos_argc = argc, kw_ds = 0, kw_strkey = 0;
  if (nt_argv_kwh(nt, argv, argc) >= 0) {
    int en = 0; const int *els = nt_arr(nt, argv[argc - 1], "elements", &en);
    int plain = en > 0, nds = 0;
    for (int e = 0; e < en; e++) {
      if (nt_kind(nt, els[e]) == NK_AssocSplatNode) {
        if (!poly_kw_splat_ok(c, els[e])) { plain = 0; break; }
        nds++;
        continue;
      }
      int key = nt_ref(nt, els[e], "key");
      if (key < 0) { plain = 0; break; }
      if (nt_kind(nt, key) != NK_SymbolNode) kw_strkey = 1;
    }
    if (plain) { kwh = argv[argc - 1]; pos_argc = argc - 1; kw_ds = nds > 0 || kw_strkey; }
  }
  int kw_pos = kwh < 0 || kw_ds;
  int splat_a = -1;
  if (has_splat_arg)
    for (int a = 0; a < pos_argc && splat_a < 0; a++)
      if (nt_kind(nt, argv[a]) == NK_SplatNode) splat_a = a;
  int is_setter_val = argc == 1 && !has_splat_arg && call_is_setter_assign(nt, id) &&
                      nt_ref(nt, id, "block") < 0;
  TyKind *atmp_ty = malloc(sizeof(TyKind) * (size_t)argc);
  for (int a = 0; a < argc; a++) atmp_ty[a] = TY_UNKNOWN;
  for (int a = 0; a < pos_argc; a++) {
    if (nt_kind(nt, argv[a]) == NK_SplatNode) { atmp_ty[a] = TY_POLY_ARRAY; continue; }
    TyKind at = cplan_poly_arg_storage(c, argv[a]);
    atmp_ty[a] = at == TY_NIL || at == TY_VOID || at == TY_UNKNOWN ? TY_POLY : at;
  }
  int kwall_any = kw_ds && (kw_strkey || poly_kw_any_key(c, kwh));
  if (kw_ds) atmp_ty[pos_argc] = kwall_any ? TY_POLY_POLY_HASH : TY_SYM_POLY_HASH;
  else if (kwh >= 0 && sp_streq(name, "fetch") && argc == 2) atmp_ty[pos_argc] = TY_SYM_POLY_HASH;
  p->flags = PPF_ROOT | (poly_key_cls0(c, id, name, argc, kwh, pos_argc, splat_a) ? PPF_KEY_CLS0 : 0) |
             (poly_key_prim(c, id, name, argc, kwh, pos_argc, splat_a) ? PPF_KEY_PRIM : 0) |
             (cpoly_key_clsv(c, id, name) ? PPF_KEY_CLSV : 0) |
             (poly_exc_cand(c, name) ? PPF_KEY_EXC : 0);
  PolyKw kw = { kwh, 0, kw_ds ? 0 : -1, NULL, NULL, NULL, kwall_any };
  PolyArgs pargs = { argv, pos_argc, NULL, atmp_ty, &kw, NULL };
  if (full) cpoly_prearms_n(c, id, name, argc, argv, ret, atmp_ty, kwall_any, is_setter_val, p, &cap);
  int *ks = malloc(sizeof(int) * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
  int nks = cpoly_arm_classes(c, id, name, ks);
  /* a class whose writer is an attr writer takes the field-write arm the
     dispatch writes after the user arms: no user arm for it */
  char wbase[256];
  /* (as the dispatch reads the result: a call typed as no value answers boxed) */
  TyKind wret = repr_poly_result_slot(is_scalar_ret(ret) ? ret : TY_POLY);
  /* (the value's kind as the emitter's temp holds it: the gate answers alike on both sides) */
  int attr_wr = poly_writer_attr_arms(c, id, name, argc, pos_argc, kwh, splat_a, wret,
                                      pos_argc == 1 ? poly_arg_temp_ty(c, name, pos_argc, argv[0], NULL) : TY_POLY,
                                      is_setter_val, wbase, sizeof wbase);
  for (int ki = 0; ki < nks; ki++) {
    int k = ks[ki];
    if (attr_wr && boxed_writer_attr_wins(c, k, wbase)) continue;
    if (!has_splat_arg && kwh < 0 && cplan_struct_aset(c, k, name, argc)) {
      cpoly_add(p, &cap, PA_STRUCT_SET, k, -1, ret, PC_SAME);
      continue;
    }
    int nk = comp_native_binding_class(c, k, name);
    if (nk >= 0) {
      if (kw_pos && has_splat_arg && argc == 1 && splat_a == 0 && kwh < 0 && c->classes[k].instantiated) {
        const NativeMethod *rm = NULL;
        for (int i = 0; i < c->n_native_methods && !rm; i++) {
          const NativeMethod *m = &c->native_methods[i];
          if (m->class_id == nk && m->kind == 0 && m->rest && m->nargs == 0 && sp_streq(m->name, name))
            rm = m;
        }
        if (!rm) continue;
        TyKind mret = sp_streq(rm->ret, "self") ? ty_object(k) : native_spec_to_ty(rm->ret);
        cpoly_add(p, &cap, PA_NATIVE, k, -1, mret, cpoly_native_conv(mret, ret, is_setter_val));
        continue;
      }
      if (!kw_pos || has_splat_arg || !c->classes[k].instantiated) continue;
      TyKind mret = TY_UNKNOWN;
      if (!kw_ds) {
        if (poly_native_arm_fits(c, k, name, argc, argv, atmp_ty, &mret) < 0) continue;
        cpoly_add(p, &cap, PA_NATIVE, k, -1, mret, cpoly_native_conv(mret, ret, is_setter_val));
        continue;
      }
      int fit = 0;
      for (int n = pos_argc; n <= argc && !fit; n++)
        if (poly_native_arm_fits(c, k, name, n, argv, atmp_ty, &mret) >= 0 &&
            (mret == TY_NIL || is_setter_val || ret == TY_POLY || mret == TY_POLY || mret == ret))
          fit = 1;
      if (fit) cpoly_add(p, &cap, PA_NATIVE, k, -1, TY_UNKNOWN, PC_SAME);
      continue;
    }
    MethodLookup ml = { .def_class = -1 };
    int mi = call_target(c, id, k, name, 0, &ml), defcls = ml.def_class;
    if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
      int xd = exc_arm_definer(c, k, name);
      if (xd >= 0) { defcls = xd; mi = call_target(c, id, xd, name, 0, NULL); }
    }
    if (mi < 0) continue;
    char arm_exp[600];
    int arm_fit = poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, splat_a, arm_exp, sizeof arm_exp);
    if (arm_fit == 0) continue;
    if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) continue;
    if (arm_fit < 0) { cpoly_add(p, &cap, PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID); continue; }
    if (!scope_has_callable_symbol(c, mi) && !scope_needs_proc_form(c, mi)) continue;
    int pfi = scope_proc_form_of(c, mi);
    Scope *ks = &c->scopes[pfi >= 0 ? pfi : mi];
    ArgLayout L;
    poly_arm_layout(c, ks, &pargs, &L);
    int fits = cplan_arm_args_fit(c, ks, &L, pos_argc, atmp_ty, kwall_any);
    arg_layout_free(&L);
    if (!fits) continue;
    TyKind mret = repr_method_ret_storage(ks);
    int conv = is_setter_val || mret == TY_VOID || mret == TY_NIL || method_is_void(ks) ? PC_VOID
             : mret == TY_STRBUF && ret == TY_STRING ? PC_COPY
             : cpoly_conv(ret, mret);
    cpoly_add(p, &cap, pfi >= 0 ? PA_PROC_FORM : PA_USER, k, mi, mret, conv);
    p->arm[p->n - 1].def = (short)defcls;
  }
  free(atmp_ty);
  free(ks);
}

/* The `default:` arm emit_poly_obj_default0 writes for a zero-argument
   dispatch: the Object reopening's method of the name. */
static void cpoly_obj_default0(Compiler *c, int id, const char *name, TyKind ret, PolyPlan *p, int *cap) {
  int obj_cls = comp_core_class(c, CORE_OBJECT);
  if (obj_cls < 0) return;
  MethodLookup ml = { .def_class = -1 };
  int obj_mi = call_target(c, id, obj_cls, name, 0, &ml);
  int blk = nt_ref(c->nt, id, "block");
  int obj_pf = obj_mi >= 0 && ml.def_class == obj_cls && c->scopes[obj_mi].yields && blk >= 0
               ? scope_proc_form_of(c, obj_mi) : -1;
  if (obj_pf >= 0 && (c->scopes[obj_pf].nparams != 0 || c->scopes[obj_pf].rest_idx >= 0)) obj_pf = -1;
  if (obj_pf >= 0 && resolve_forwarded_block(c, blk) < 0) obj_pf = -1;
  char oexp[600];
  if (obj_pf >= 0) {
    Scope *ps = &c->scopes[obj_pf];
    cpoly_add(p, cap, PA_PROC_FORM, PA_KEY_DEFAULT, obj_mi, repr_method_ret_storage(ps),
              method_is_void(ps) ? PC_VOID : cpoly_conv(ret, repr_method_ret_storage(ps)));
  }
  else if (obj_mi >= 0 && ml.def_class == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
           scope_has_callable_symbol(c, obj_mi)) {
    Scope *ms = &c->scopes[obj_mi];
    cpoly_add(p, cap, PA_USER, PA_KEY_DEFAULT, obj_mi, repr_method_ret_storage(ms),
              method_is_void(ms) ? PC_VOID : cpoly_conv(ret, repr_method_ret_storage(ms)));
  }
  else if (ml.def_class == obj_cls && poly_arm_refuses_none(c, obj_mi, oexp, sizeof oexp))
    cpoly_add(p, cap, PA_ARITY, PA_KEY_DEFAULT, obj_mi, TY_UNKNOWN, PC_VOID);
}

static int cpoly_name_in(const char *name, const char *const *list) {
  for (int i = 0; list[i]; i++) if (sp_streq(name, list[i])) return 1;
  return 0;
}

static void cpoly_family(PolyPlan *p, int *cap, int fam) {
  cpoly_add(p, cap, PA_BUILTIN, PA_KEY_BUILTIN + fam, -1, TY_UNKNOWN, PC_SAME);
}

/* a trial arm the dispatch offers: what it answers is the emission's */
static void cpoly_trial(PolyPlan *p, int *cap, int t) {
  cpoly_add(p, cap, PA_TRIAL, PA_KEY_TRIAL + t, -1, TY_UNKNOWN, PC_SAME);
}

int cplan_poly_has_family(const PolyPlan *p, int fam) {
  for (int i = 0; i < p->n; i++)
    if (p->arm[i].key == PA_KEY_BUILTIN + fam) return 1;
  return 0;
}

/* Does the dispatch offer a String the builtin answer (emit_poly_str_prearm)?
   Not when an earlier pre-arm already took the String tag. */
static int cpoly_str_trial(Compiler *c, int id, const char *name, int argc, const int *argv,
                           const TyKind *atmp_ty, TyKind ret, const PolyPlan *p) {
  if (!(poly_string_read_p(name) || (argc == 1 && sp_streq(name, "to_i")))) return 0;
  if (nt_ref(c->nt, id, "block") >= 0) return 0;
  for (int a = 0; a < argc; a++)
    if (subtree_has_side_effect(c, argv[a]) && comp_ntype(c, argv[a]) != atmp_ty[a]) return 0;
  TyKind bt = c->poly_builtin_ty && id < c->node_cap ? c->poly_builtin_ty[id] : TY_UNKNOWN;
  if (bt == TY_UNKNOWN || (ret != TY_POLY && bt != ret)) return 0;
  static const int str_tag[] = { PB_LEN, PB_EMPTY, PB_STRT, PB_SPLIT, PB_UNPACK1, PB_STR_DELETE,
                                 PB_STR_PARTITION, PB_STR_SETOP, PB_STR_ENCODE, PB_STR_SPLIT_N };
  for (size_t i = 0; i < sizeof str_tag / sizeof str_tag[0]; i++)
    if (cplan_poly_has_family(p, str_tag[i])) return 0;
  /* an Integer's bit reference reads a String's character too */
  if (cplan_poly_has_family(p, PB_INT_BITREF) && (ret == TY_POLY || ret == TY_STRING)) return 0;
  return 1;
}

/* The tag pre-arms emit_poly_prearms0 writes: a builtin value's answer to
   a name a user class also owns. */
static void cpoly_prearms0(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                           PolyPlan *p, int *cap) {
  static const char *const ioz[] = { "close", "closed?", "eof?", "eof", "tty?", "isatty", "flush",
    "fileno", "tell", "pos", "lineno", "sync", "timeout", "gets", "getc", "readchar", "readline", "readbyte",
    "getbyte", "readlines", NULL };
  static const char *const reduce[] = { "sum", "min", "max", "first", "last", NULL };
  /* the String transforms, by what they answer: a String, an Integer
     array, a String array */
  static const char *const strt_s[] = { "upcase", "downcase", "capitalize", "swapcase", "strip",
    "reverse", "chomp", "chop", "succ", "next", "chr", NULL };
  int blockless = nt_ref(c->nt, id, "block") < 0;
  if (ps->lengthlike) cpoly_family(p, cap, PB_LEN);
  if (ps->empty) cpoly_family(p, cap, PB_EMPTY);
  if (ps->class_named) cpoly_family(p, cap, PB_CLASS_NAMED);
  if (ps->class_reflect) cpoly_family(p, cap, PB_CLASS_REFLECT);
  if (ps->ostruct) cpoly_family(p, cap, PB_OSTRUCT);
  if (ps->poly_to_a) cpoly_family(p, cap, PB_TO_A);
  if (ps->io_rewind) cpoly_family(p, cap, PB_IO_REWIND);
  if (blockless && sp_streq(name, "puts")) cpoly_family(p, cap, PB_IO_PUTS);
  if (blockless && cpoly_name_in(name, ioz)) cpoly_family(p, cap, PB_IOZ);
  if (blockless && cpoly_name_in(name, reduce)) cpoly_family(p, cap, PB_REDUCE);
  int sci = comp_core_class(c, CORE_STRING);
  if (sci >= 0 && call_target(c, id, sci, name, 0, NULL) >= 0) return;
  int fits = cpoly_name_in(name, strt_s) ? ret == TY_POLY || ret == TY_STRING
           : sp_streq(name, "bytes") ? ret == TY_POLY || ret == TY_INT_ARRAY
           : sp_streq(name, "chars") ? ret == TY_POLY || ret == TY_STR_ARRAY : 0;
  if (fits && sp_streq(name, "chr")) cpoly_family(p, cap, PB_INT_CHR);
  if (fits) cpoly_family(p, cap, PB_STRT);
  if (sp_streq(name, "split") && (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY))
    cpoly_family(p, cap, PB_SPLIT);
}

/* The class-side arms a class-valued receiver takes ahead of the instance
   switch (emit_poly_cls_value_prearm). Their count. */
static int cpoly_cls_value_arms(Compiler *c, int id, const char *name, int argc, const PolyArgs *A,
                                TyKind ret, int has_tr, PolyPlan *p, int *cap) {
  int ncc = 0;
  (void)comp_cmethod_candidates(c, name, &ncc);
  int *ccls = malloc(sizeof(int) * (size_t)(ncc > 0 ? ncc : 1));
  int *cmi = malloc(sizeof(int) * (size_t)(ncc > 0 ? ncc : 1));
  char (*cexp)[600] = malloc(sizeof *cexp * (size_t)(ncc > 0 ? ncc : 1));
  int wants_blk = 0;
  int n = poly_cls_value_cands(c, id, name, argc, A, 0, ccls, cmi, cexp, &wants_blk);
  for (int i = 0; i < n; i++) {
    Scope *ks = &c->scopes[cmi[i]];
    if (cexp[i][0]) cpoly_add(p, cap, PA_ARITY, PA_KEY_CLASS_VALUE + ccls[i], cmi[i], TY_UNKNOWN, PC_VOID);
    else {
      TyKind result = repr_method_ret_storage(ks);
      cpoly_add(p, cap, PA_USER, PA_KEY_CLASS_VALUE + ccls[i], cmi[i], result,
                !has_tr || method_is_void(ks) ? PC_VOID : cpoly_conv(ret, result));
    }
  }
  free(ccls); free(cmi); free(cexp);
  return n;
}

/* The pre-arms emit_poly_prearms0_blk writes: a builtin container or a
   Mutex driving the call's block as a proc, a callable value, a class
   value's class-side arms, or else a Struct class's members. */
static void cpoly_prearms0_blk(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                               PolyPlan *p, int *cap) {
  int blk = nt_ref(c->nt, id, "block");
  int live_blk = blk >= 0 && (resolve_forwarded_block(c, blk) >= 0 || g_yield_proc_ref);
  if (live_blk && poly_enum_op_for(name)) cpoly_family(p, cap, PB_ENUM_PROC);
  if (live_blk && sp_streq(name, "each_index")) cpoly_family(p, cap, PB_ENUM_PROC);
  if (live_blk && sp_streq(name, "times")) cpoly_family(p, cap, PB_ENUM_PROC);
  if (live_blk && (sp_streq(name, "each_line") || sp_streq(name, "each_char") ||
                   sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")))
    cpoly_family(p, cap, PB_ENUM_PROC);
  if (live_blk && sp_streq(name, "synchronize")) cpoly_family(p, cap, PB_SYNC);
  if (is_call_or_yield(name)) cpoly_family(p, cap, PB_CALLABLE);
  PolyArgs none = { NULL, 0, NULL, NULL, NULL, NULL };
  if (!cpoly_cls_value_arms(c, id, name, 0, &none, ret, 1, p, cap) && ps->cls_members)
    cpoly_family(p, cap, PB_CLS_MEMBERS);
}

/* The builtin cases emit_poly_cases0 writes after the class arms (the
   container read re-entered as a builtin is a trial, not listed here). */
static void cpoly_cases0(const char *name, const PolySpecials0 *ps, PolyPlan *p, int *cap) {
  if (is_count_alias(name))
    cpoly_family(p, cap, PB_LEN_CASES);
  if (sp_streq(name, "clear")) cpoly_family(p, cap, PB_CLEAR);
  if (ps->empty) cpoly_family(p, cap, PB_EMPTY_CASES);
  if (sp_streq(name, "compare_by_identity?")) cpoly_family(p, cap, PB_CMP_BY_ID);
}

/* The builtin `default:` arm emit_poly_defaults0 writes when the Object
   reopening has none (the first that applies), and its named cases. */
static void cpoly_defaults0(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                            int obj_done, PolyPlan *p, int *cap) {
  static const char *const case_conv[] = { "upcase", "downcase", "capitalize", "swapcase", NULL };
  static const char *const num[] = { "abs", "round", "succ", "next", "pred", "ceil", "floor", "truncate",
    "abs2", "infinite?", "numerator", "denominator", "nonzero?", "bit_length", NULL };
  static const char *const arr_t[] = { "flatten", "compact", "uniq", NULL };
  static const char *const queue[] = { "size", "length", "empty?", "pop", "shift", "deq", "num_waiting",
    "closed?", "max", "close", NULL };
  int blockless = nt_ref(c->nt, id, "block") < 0;
  int fam = -1;
  if (obj_done) fam = -1;
  else if (exc_um_reentered(id) && exc_um_only_acc(name)) fam = PB_D_EXC_ACC;
  else if (sp_streq(name, "__to_enum_each") && (ret == TY_POLY || ret == TY_ENUMERATOR)) fam = PB_D_ENUM_EACH;
  else if (cplan_boxed_fallback_row(c, id, name, ret, NULL)) fam = PB_D_TO_S;
  else if (cpoly_name_in(name, case_conv)) fam = PB_D_CASE_CONV;
  else if (cpoly_name_in(name, num)) fam = PB_D_NUM;
  else if (sp_streq(name, "digits")) fam = PB_D_DIGITS;
  else if (cpoly_name_in(name, arr_t) && (ret == TY_POLY || ret == TY_POLY_ARRAY)) fam = PB_D_ARRAY_TRANSFORM;
  else if (ps->pred) fam = PB_D_PRED;
  else if (is_numeric_conversion(name)) fam = PB_D_TO_IF;
  else if (blockless && (sp_streq(name, "any?") || sp_streq(name, "none?"))) fam = PB_D_ANY_NONE;
  else if (sp_streq(name, "to_h") && blockless && ret == TY_POLY) fam = PB_D_TO_H;
  else if (sp_streq(name, "display") && blockless) fam = PB_D_DISPLAY;
  else if (sp_streq(name, "hash") && blockless && (ret == TY_INT || ret == TY_POLY)) fam = PB_D_HASH;
  if (fam >= 0) cpoly_family(p, cap, fam);
  if (blockless && is_indexed_each(name)) cpoly_family(p, cap, PB_N_EACH_INDEX);
  if (sp_streq(name, "join")) cpoly_family(p, cap, PB_N_JOIN);
  if (sp_streq(name, "alive?")) cpoly_family(p, cap, PB_N_ALIVE);
  if (sp_streq(name, "kill")) cpoly_family(p, cap, PB_N_KILL);
  if (sp_streq(name, "status")) cpoly_family(p, cap, PB_N_STATUS);
  if (cpoly_name_in(name, queue)) cpoly_family(p, cap, PB_N_QUEUE);
  if (sp_streq(name, "read") && (ret == TY_POLY || ret == TY_STRING)) cpoly_family(p, cap, PB_N_IO_READ);
  if (sp_streq(name, "flush")) cpoly_family(p, cap, PB_N_IO_FLUSH);
  if (sp_streq(name, "close")) cpoly_family(p, cap, PB_N_IO_CLOSE);
  if (sp_streq(name, "__enum_to_a")) cpoly_family(p, cap, PB_N_ENUM_TO_A);
  if (sp_streq(name, "signal") || sp_streq(name, "broadcast")) cpoly_family(p, cap, PB_N_CONDVAR);
}

/* How a zero-argument dispatch holds and keys its receiver, and the type
   its arms answer into. An OpenStruct member read makes an untyped call
   poly; emission reads these flags and the adjusted result type. */
static void cpoly_flags0(Compiler *c, int id, const char *name, PolyPlan *p, PolySpecials0 *out) {
  MtabMark mt_was = mtab_push_in(c, id);   /* (a part of an interpolation is asked under its parent's frame) */
  PolySpecials0 ps;
  poly_specials0(c, id, name, &ps);
  *out = ps;
  if (ps.ostruct && (p->ret == TY_UNKNOWN || p->ret == TY_VOID || p->ret == TY_NIL)) p->ret = TY_POLY;
  int root = ps.ncall_arm > 0 || ps.lengthlike || ps.empty || ps.pred || ps.class_named ||
             ps.class_reflect || ps.ostruct || ps.io_rewind || ps.poly_to_a || ps.poly_to_h ||
             (nt_ref(c->nt, id, "block") >= 0 && (poly_blk_prearm_name(name) || is_call_or_yield(name)));
  p->flags = (root ? PPF_ROOT : 0) |
             (poly_key_cls0(c, id, name, 0, -1, 0, -1) ? PPF_KEY_CLS0 : 0) |
             (poly_key_prim(c, id, name, 0, -1, 0, -1) ? PPF_KEY_PRIM : 0) |
             (cpoly_key_clsv(c, id, name) ? PPF_KEY_CLSV : 0) |
             (poly_exc_cand(c, name) ? PPF_KEY_EXC : 0) |
             (is_poly_object_surface_name(name) ? PPF_SURFACE : 0);
  mtab_pop(c, mt_was);
}

static void cpoly_resolve(Compiler *c, int id, PolyPlan *p, int full) {
  free(p->arm);
  p->arm = NULL; p->n = 0; p->flags = 0; p->full = full;
  p->ntype = comp_ntype(c, id);
  p->ret = repr_poly_result_slot(p->ntype);
  p->edits = comp_ty_edits;
  const char *name = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  int argc = 0;
  int args = nt_ref(c->nt, id, "arguments");
  if (args >= 0) (void)nt_arr(c->nt, args, "arguments", &argc);
  if (!name || recv < 0) return;
  if (argc == 0) {
    PolySpecials0 ps;
    cpoly_flags0(c, id, name, p, &ps);
    cpoly_user_arms0(c, id, name, p->ret, p);
    if (!full) return;
    int cap = p->n;
    cpoly_prearms0(c, id, name, &ps, p->ret, p, &cap);
    cpoly_prearms0_blk(c, id, name, &ps, p->ret, p, &cap);
    cpoly_cases0(name, &ps, p, &cap);
    if (cpoly_str_trial(c, id, name, 0, NULL, NULL, p->ret, p)) cpoly_trial(p, &cap, PT_STR);
    if (p->ret == TY_POLY && nt_ref(c->nt, id, "block") < 0 && poly_container_read_p(name))
      cpoly_trial(p, &cap, PT_CONTAINER);
    int n0 = p->n;
    cpoly_obj_default0(c, id, name, p->ret, p, &cap);
    int n1 = p->n;
    cpoly_defaults0(c, id, name, &ps, p->ret, p->n > n0, p, &cap);
    /* a default written by now (the Object reopening's, a builtin one), or
       the builtin surface's, and the raise when it declines */
    int done = n1 > n0;
    for (int f = PB_D_EXC_ACC; f <= PB_D_HASH; f++) done |= cplan_poly_has_family(p, f);
    if (cplan_poly_has_family(p, PB_D_ARRAY_TRANSFORM)) cpoly_trial(p, &cap, PT_ARRAY_FALLBACK);
    if (!done) cpoly_trial(p, &cap, PT_DEFAULT0);
  }
  else cpoly_user_arms_n(c, id, name, argc, nt_arr(c->nt, args, "arguments", &argc), p->ret, p, full);
}

static PolyPlan cplan_poly_level(Compiler *c, int id, int full);

PolyPlan cplan_poly(Compiler *c, int id) { return cplan_poly_level(c, id, 1); }
PolyPlan cplan_poly_arms(Compiler *c, int id) { return cplan_poly_level(c, id, 0); }

/* p as an answer: its arms kept for the compile (cplan_keep) */
static PolyPlan cpoly_keep(Compiler *c, PolyPlan p) {
  if (p.arm) cplan_keep(c, p.arm);
  return p;
}

/* The builtin classes the tables give as answering `name` ahead of Object
   (builtin_next_class_answering), with their core slots, asked of the tables
   once per name: the walk over every table is what a send's split costs, and
   the analysis asks it at each round */
struct SendOwners { char *name; int n; const char **cls; int *slot; };
static int cplan_send_owners(Compiler *c, const char *name) {
  for (int i = 0; i < c->nsend_owners; i++) if (!strcmp(c->send_owners[i].name, name)) return i;
  if (c->nsend_owners == c->csend_owners) {
    c->csend_owners = c->csend_owners ? c->csend_owners * 2 : 16;
    c->send_owners = realloc(c->send_owners, sizeof *c->send_owners * (size_t)c->csend_owners);
    if (!c->send_owners) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  struct SendOwners *e = &c->send_owners[c->nsend_owners];
  memset(e, 0, sizeof *e);
  e->name = strdup(name);
  int ri = 0, cap = 0;
  for (const char *bn; (bn = builtin_next_class_answering(&ri, name)); ) {
    if (e->n == cap) {
      cap = cap ? cap * 2 : 16;
      e->cls = realloc(e->cls, sizeof *e->cls * (size_t)cap);
      e->slot = realloc(e->slot, sizeof *e->slot * (size_t)cap);
    }
    if (!e->name || !e->cls || !e->slot) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    e->cls[e->n] = bn; e->slot[e->n++] = builtin_core_class_slot(bn);
  }
  if (!e->name) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return c->nsend_owners++;
}
void cplan_send_owners_free(Compiler *c) {
  for (int i = 0; i < c->nsend_owners; i++) { free(c->send_owners[i].name); free(c->send_owners[i].cls); free(c->send_owners[i].slot); }
  free(c->send_owners); c->send_owners = NULL; c->nsend_owners = c->csend_owners = 0;
}

/* The classes a boxed `recv.send(:name)` split tests the receiver's class
   name against, ahead of the top-level def Object holds: each builtin class
   the tables give as answering the name ahead of Object that the receiver
   may be an instance of (cls_fact_may_hold_builtin), then each class of the
   program under such a builtin class that its fact does not rule out
   (cls_fact_excludes). out takes the first max names (NULL, 0 to count);
   the count of all is returned. None means no class that answers the name
   can reach the receiver: the send calls the def, as for a name no builtin
   answers. */
int cplan_send_split_owners(Compiler *c, int recv, const char *name, const char **out, int max) {
  int n = 0, oi = cplan_send_owners(c, name);
  for (int i = 0; i < c->send_owners[oi].n; i++)
    if (cls_fact_may_hold_builtin(c, recv, c->send_owners[oi].slot[i])) {
      if (n < max) out[n] = c->send_owners[oi].cls[i];
      n++;
    }
  for (int k = 0; k < c->nclasses; k++) {
    if (comp_class_is_module(c, &c->classes[k])) continue;
    int bp = class_builtin_parent(c, k), ex = cls_fact_excludes(c, recv, k);
    if (!builtin_id_below_object_answers(bp, name)) continue;
    /* An instance of a builtin class includes the instances of the program's subclasses of it, which a builtin call
       the facts do not model hands back (`MyA.new.rotate!`): the class part excludes such a class only where the
       receiver cannot be the builtin class it descends from either */
    if (ex) {
      const BuiltinClass *r = NULL;
      for (int i = 0; (r = builtin_class_at(i)) && r->id != bp; i++) {}
      if (r && !cls_fact_may_hold_builtin(c, recv, builtin_core_class_slot(r->name))) continue;
    }
    const char *rn = class_ruby_name(c, k);
    if (!rn) continue;
    if (n < max) out[n] = rn;
    n++;
  }
  return n;
}

static PolyPlan cplan_poly_level(Compiler *c, int id, int full) {
  if (id < 0 || id >= c->node_cap) return (PolyPlan){0};
  const char *nm = nt_str(c->nt, id, "name");
  int plain = cplan_plain_ctx();
  const struct CplanMemo *k = c->cplan_memo;
  if (plain && k && id < k->poly_cap && k->poly_name[id] && nm && sp_streq(k->poly_name[id], nm) &&
      k->poly[id].ntype == comp_ntype(c, id) && k->poly[id].edits == comp_ty_edits && (k->poly[id].full || !full))
    return k->poly[id];
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  PolyPlan r = {0};
  cpoly_resolve(c, id, &r, full);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  r = cpoly_keep(c, r);
  if (!plain) return r;
  /* (the memo asked after resolving: a resolution asks other calls' arms,
     an argument's representation reading its reader arms
     (repr_boxed_reader_handle), which make and grow it) */
  struct CplanMemo *m = cplan_memo(c);
  if (id >= m->poly_cap) {
    int ncap = m->poly_cap ? m->poly_cap : 1024;
    while (ncap <= id) ncap *= 2;
    if (!(m->poly = realloc(m->poly, (size_t)ncap * sizeof *m->poly)) ||
        !(m->poly_name = realloc(m->poly_name, (size_t)ncap * sizeof *m->poly_name))) cplan_oom();
    memset(m->poly + m->poly_cap, 0, (size_t)(ncap - m->poly_cap) * sizeof *m->poly);
    memset(m->poly_name + m->poly_cap, 0, (size_t)(ncap - m->poly_cap) * sizeof *m->poly_name);
    m->poly_cap = ncap;
  }
  m->poly[id] = r;
  free(m->poly_name[id]);
  m->poly_name[id] = nm ? strdup(nm) : NULL;
  if (nm && !m->poly_name[id]) cplan_oom();
  return r;
}

PolyPlan cplan_poly_fresh(Compiler *c, int id) {
  PolyPlan r = {0};
  if (id < 0 || id >= c->node_cap) return r;
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  cpoly_resolve(c, id, &r, 1);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  return cpoly_keep(c, r);
}

/* (call_plan.h) */
int cplan_reopen_own_block(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) return ROB_NONE;
  const Scope *m = &c->scopes[mi];
  if (m->yields) return ROB_DECLINE;   /* (reached through its proc form) */
  if (m->blk_param && m->blk_param[0]) return ROB_PASS;
  int bx = nt_kind(nt, blk) == NK_BlockArgumentNode ? nt_ref(nt, blk, "expression") : -1;
  return bx >= 0 && nt_kind(nt, bx) != NK_LocalVariableReadNode ? ROB_DECLINE : ROB_DROP;
}
/* (call_plan.h) The driver walks a value as a builtin container; an object
   of a class with an `each` of its own is read as an empty one, where
   CRuby's Enumerable runs that each. And it runs the callable on every
   element: a nil one is no block in CRuby, where the call answers as
   written without one (an Enumerator for most names). */
int cplan_driver_walks_recv(Compiler *c, int id) {
  return !poly_recv_defines(c, id, "each") && !an_fwd_block_may_be_nil(c, id);
}

/* (call_plan.h) Only `each`: its inline walk is what sp_poly_enum_proc does
   for the kinds it serves, reading the length at every step. The inline
   forms of reverse_each, each_pair and the rest differ from it on a
   receiver the block changes or a kind the name rejects. The call's value
   is boxed, as the dispatch's is, so the value form serves it from either
   arm. */
PolyPlan cplan_each_split(Compiler *c, int id) {
  PolyPlan sp = {0};
  sp.ntype = sp.ret = comp_ntype(c, id);
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), blk = nt_ref(nt, id, "block");
  if (!name || !is_each_name(name) || recv < 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return cpoly_keep(c, sp);
  if (nt_ref(nt, id, "arguments") >= 0) return cpoly_keep(c, sp);
  if (repr_of(c, recv).kind != RK_BOXED || repr_of(c, id).kind != RK_BOXED) return cpoly_keep(c, sp);
  if (!poly_block_call_needs_dispatch(c, id)) return cpoly_keep(c, sp);
  int cap = 0;
  cpoly_family(&sp, &cap, PB_ENUM_INLINE);
  return cpoly_keep(c, sp);
}

PolyPlan cplan_poly_block(Compiler *c, int id) {
  PolyPlan bp = {0};
  bp.ntype = bp.ret = comp_ntype(c, id);
  int cap = 0;
  int cand[CPB_MAX_CANDS];
  int nc = poly_block_dispatch_cands(c, id, cand, CPB_MAX_CANDS);
  const char *name = nt_str(c->nt, id, "name");
  for (int i = 0; i < nc; i++) {
    cpoly_add(&bp, &cap, PA_USER, cand[i], call_target(c, id, cand[i], name, 0, NULL), TY_UNKNOWN, PC_VOID);
    if (c->classes[cand[i]].is_native_class) bp.native_block = 1;
  }
  if (nc == 0) return cpoly_keep(c, bp);
  /* map!/collect! rewrites a builtin array in place, given a block body */
  int block = nt_ref(c->nt, id, "block");
  int map_bang = 0;
  if (is_map_bang_alias(name)) {
    int body = nt_ref(c->nt, block, "body");
    int bn = 0;
    if (body >= 0) (void)nt_arr(c->nt, body, "body", &bn);
    map_bang = bn >= 1 && block_param_name(c, block, 0);
  }
  if (map_bang) cpoly_family(&bp, &cap, PB_BD_MAP_BANG);
  else cpoly_trial(&bp, &cap, PT_BD_DEFAULT);
  return cpoly_keep(c, bp);
}

/* A layout class need not be self's actual class. Use the existing flow
   bound, or a class with no descendants; otherwise retain legacy calls.
   Without a supplied class, the evidence-call emitter needs its existing
   layout class proved exact, rather than a different actual class. */
int cplan_visibility_class(Compiler *c, int id, int ci) {
  int recv = nt_ref(c->nt, id, "receiver");
  int layout_only = ci < 0;
  if (layout_only && recv >= 0) {
    TyKind rt = comp_ntype(c, recv);
    if (ty_is_object(rt)) ci = ty_object_class(rt);
  }
  const ClsSet *s = cls_fact_node(c, recv);
  int one = s && !s->bv && s->n == 1 ? cls_set_next(s, -1) : -1;
  if (one >= 0 && cls_fact_bounded(c, one))
    return layout_only && one != ci ? -1 : one;
  int n = 0;
  if (ci >= 0 && cls_fact_bounded(c, ci)) comp_descendants(c, ci, &n);
  return ci >= 0 && cls_fact_bounded(c, ci) && n == 0 ? ci : -1;
}

/* The method_missing a call id on class ci (its class side when cmethod;
   main for -1) falls to: its chain's, else, past a class that is no blank
   slate, Object's (an Object or a Kernel reopen's, a top-level def: main
   has no user class entry), which the tables leave out of a program
   class's chain; -1 for none. Each is call_target's answer for the
   hook's name at the call (CT_REACH: every call may reach its lookup's
   tail, whatever it names), as the record has it there. Id -1 asks with
   no site, which keeps the table's answer (noted unasked where the hook has
   versions): for a reader with no single call (an entry's face, a guard's or
   a version's tail), for ct_main_stopped, which runs inside the lookup of the
   call's own name on main (an ask at its id would reach it again for the
   name method_missing), and for ct_main_builtin_undef, which is the same test
   as ct_main_stopped's. cplan_mm_chain stays on the tables: its one caller is
   the entry path (ver_mm_at), which reads the record. */
static int cplan_mm_method(Compiler *c, int id, int ci, int cmethod) {
  if (ci >= 0 && !cmethod && cplan_undef_before(c, id, ci, METHOD_MISSING_NAME)) return -1;
  int t = ci < 0 ? -1 : call_target(c, id, ci, METHOD_MISSING_NAME, CT_REACH | (cmethod ? CT_CMETHOD : 0), NULL);
  if (t >= 0 || (!cmethod && ci >= 0 && class_is_blank_slate(c, ci))) return t;
  int obj = comp_core_class(c, CORE_OBJECT);
  if (obj >= 0 && (t = call_target(c, id, obj, METHOD_MISSING_NAME, CT_REACH, NULL)) >= 0) return t;
  return call_target(c, id, -1, METHOD_MISSING_NAME, CT_REACH | CT_TOP, NULL);
}
/* Missing hooks are not dispatched yet. New visibility checks retain the
   old self-call behaviour when the existing method table defines a hook. */
int cplan_has_method_missing(Compiler *c, int id, int ci, int cmethod) {
  if (ci < 0 && !cmethod) {
    Scope *s = comp_scope_of(c, id);
    if (s && !s->is_cmethod) ci = s->class_id;
    if (ci < 0 && !self_is_main(c, id)) return 0;
  }
  return cplan_mm_method(c, id, ci, cmethod) >= 0;
}

int cplan_string_missing(Compiler *c, int id, const char *name, int *hook, int *undef) {
  int ci = comp_core_class(c, CORE_STRING);
  if (ci < 0 || !elab_method_versions(c, name) || cplan_builtin_call(c, id) || !builtin_instance_method_known("String", name)) return 0;
  /* (no undef of the name on String's chain: no call misses it, and
     cplan_undef_at would place the call outside a plan frame to say so) */
  if (!cplan_chain_undef(c, ci, name)) return 0;
  *undef = cplan_undef_before(c, id, ci, name) ? 1 : cplan_undef_at(c, id, ci, name);
  if (!*undef) return 0;
  *hook = cplan_mm_method(c, id, ci, 0);
  return 1;
}

int cplan_string_builtin_fallback(Compiler *c, int id, const char *name) {
  if (cplan_builtin_call(c, id) || nt_int(c->nt, id, "string_live_builtin", 0)) return 1;
  int cls = comp_core_class(c, CORE_STRING);
  if (cls < 0 || !elab_method_versions(c, name)) return 0;
  /* (where the call runs is asked inside the plan frame: outside it, a
     table change since the last ask, a codegen rename, rebuilt the
     record's map per call, #12194) */
  int framed = elab_plan_enter(c, id), ans = 0;
  if (cplan_runs_once(c, id)) {
    const LrDef *d = NULL;
    int state = elab_method_at(c, cls, name, 0, elab_point_of(c, id), &d);
    ans = state == ELAB_NO && !d;
  }
  if (framed) elab_plan_leave(c);
  return ans;
}
static PolyStringArm cplan_poly_string_arm_framed(Compiler *c, int id, const char *name) {
  int cls = comp_core_class(c, CORE_STRING);
  if (cls < 0 || cplan_string_builtin_fallback(c, id, name)) return CP_STR_BUILTIN;
  int undef = cplan_undef_at(c, id, cls, name);
  if (undef < 0) undef = cplan_undef_before(c, id, cls, name) || cplan_undef_stops(c, id, cls, name);
  if (undef) return cplan_has_method_missing(c, id, cls, 0) ? CP_STR_NONE : CP_STR_UNDEF;
  return call_answers(c, id, cls, name, 0, NULL) ? CP_STR_NONE : CP_STR_BUILTIN;
}
/* (the questions below place the call, cplan_undef_at among them: in one
   plan frame, as cplan_string_builtin_fallback's own) */
PolyStringArm cplan_poly_string_arm(Compiler *c, int id, const char *name) {
  int framed = id >= 0 ? elab_plan_enter(c, id) : 0;
  PolyStringArm a = cplan_poly_string_arm_framed(c, id, name);
  if (framed) elab_plan_leave(c);
  return a;
}
/* The super a method's body is, unconditionally (`super`, `super(n, *a)`):
   its only statement; -1 for any other body */
static int cplan_body_super(Compiler *c, int mi) {
  int b = c->scopes[mi].body, n = 0;
  if (b >= 0 && nt_kind(c->nt, b) == NK_StatementsNode) {
    const int *st = nt_arr(c->nt, b, "body", &n);
    b = st && n == 1 ? st[0] : -1;
  }
  NodeKind k = b >= 0 ? nt_kind(c->nt, b) : NK_NONE;
  return k == NK_SuperNode || k == NK_ForwardingSuperNode ? b : -1;
}
/* Is scope mi's body only the raise desugar_method_missing_super wrote for
   a super that reached BasicObject#method_missing? */
static int cplan_body_mm_raise(Compiler *c, int mi) {
  int b = c->scopes[mi].body, n = 0;
  if (b >= 0 && nt_kind(c->nt, b) == NK_StatementsNode) {
    const int *st = nt_arr(c->nt, b, "body", &n);
    b = st && n == 1 ? st[0] : -1;
  }
  return b >= 0 && nt_int(c->nt, b, "mm_super_raise", 0);
}

enum { SSF_DEAD_ASKED = 1, SSF_DEAD_SUPER = 2, SSF_MM_ASKED = 4, SSF_MM_ANSWERS = 8 };
/* Method scope mi's super facts (Compiler.scope_super_facts), each filled
   when first asked: SSF_DEAD_SUPER (cplan_value_dead_super), SSF_MM_ANSWERS
   (cplan_mm_answers). */
static unsigned char *cplan_scope_facts(Compiler *c, int mi) {
  if (c->nscope_super_facts != c->nscopes) {
    free(c->scope_super_facts);
    c->scope_super_facts = calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
    if (!c->scope_super_facts) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->nscope_super_facts = c->nscopes;
  }
  return &c->scope_super_facts[mi];
}

/* A value position of a method's body: one that answers a value of its own
   (not a super whose plan reaches no method, nor a bare raise,
   an_call_diverges) stops the walk; a dead super is noted in dead */
typedef struct { Compiler *c; int dead; } DeadSuperWalk;
static int cplan_own_value_leaf(const VpLeaf *leaf, void *ud) {
  DeadSuperWalk *w = ud;
  if (leaf->kind != VPK_EXPR && leaf->kind != VPK_GAVE_UP) return 1;
  NodeKind k = leaf->kind == VPK_EXPR ? nt_kind(w->c->nt, leaf->node) : NK_NONE;
  /* (a walk that gave up is taken as dead: the refusal is the safe answer) */
  if (leaf->kind == VPK_GAVE_UP ||
      ((k == NK_SuperNode || k == NK_ForwardingSuperNode) && cplan_user(w->c, leaf->node).mi < 0)) {
    w->dead = 1;
    return 0;
  }
  return !an_call_diverges(w->c, leaf->node);
}
/* Is method scope mi's value a super whose plan reaches no method, where
   CRuby answers what its method_missing does: some value position of its
   body (for_each_value_position) is one, and none answers a value of its
   own? One that can (an untaken arm's nil, a statement after the super)
   answers it as CRuby does there, and a body that only raises, throws or
   exits answers nothing. */
static int cplan_value_dead_super(Compiler *c, int mi) {
  unsigned char *f = cplan_scope_facts(c, mi);
  if (!(*f & SSF_DEAD_ASKED)) {
    *f |= SSF_DEAD_ASKED;
    DeadSuperWalk w = { c, 0 };
    int b = c->scopes[mi].body;
    if (b >= 0 && !for_each_value_position(c->nt, b, VP_ALL_FLAGS & ~VP_CASE_IN_RAISE_NIL, cplan_own_value_leaf, &w) &&
        w.dead)
      *f |= SSF_DEAD_SUPER;
  }
  return (*f & SSF_DEAD_SUPER) != 0;
}

/* Does method_missing scope mm answer for self? One whose value is its own
   super, unconditionally, hands the name on: CRuby then asks the
   method_missing that super reaches, and past the last one raises
   NoMethodError, as Spinel does. Any other answers, or may (one that
   answers some names and supers on for the rest). */
static int cplan_mm_answers(Compiler *c, int mm) {
  unsigned char *f = cplan_scope_facts(c, mm);
  if (!(*f & SSF_MM_ASKED)) {
    *f |= SSF_MM_ASKED;   /* (a cycle of hand-ons answers nothing) */
    int sup = cplan_body_super(c, mm), up = sup >= 0 ? cplan_user(c, sup).mi : -1;
    if ((sup < 0 && !cplan_body_mm_raise(c, mm)) || (up >= 0 && cplan_mm_answers(c, up))) *f |= SSF_MM_ANSWERS;
  }
  return (*f & SSF_MM_ANSWERS) != 0;
}
/* Does super `id` reach a method whose value is its own super that reaches
   no method (cplan_value_dead_super), where CRuby calls a method_missing
   Spinel does not dispatch: one that answers for self (cplan_mm_answers), on
   the chain of the class or of a descendant, as self may be one? */
int cplan_super_needs_method_missing(Compiler *c, int id) {
  const CallPlan p = cplan_user(c, id);
  Scope *s = comp_scope_of(c, id);
  if (!s || p.via != UC_SUPER || p.mi < 0 || !cplan_value_dead_super(c, p.mi)) return 0;
  for (int k = -1; k < c->nclasses; k++) {
    if (k >= 0 && (k == s->class_id || !is_descendant(c, k, s->class_id))) continue;
    int mm = cplan_mm_method(c, id, k < 0 ? s->class_id : k, s->is_cmethod);
    if (mm >= 0 && cplan_mm_answers(c, mm)) return 1;
  }
  return 0;
}

/* Is call id, at its point p, bound to the version of class ci's `name`
   the record has live there: it runs at p (not later, nor in a block a
   target may keep), and the method the plan binds it to is the record's
   there, not a builtin's ahead of it? */
static int cplan_bound_at_point(Compiler *c, int id, int ci, const char *name, int sg, LoadPoint p) {
  if (p.region < 0 || elab_runs_later(c, id) || elab_in_kept_block(c, id)) return 0;
  /* (a reader that names the method, a respond_to?, binds the version live at its point, as a capture does; the
     version is the bound method where the plan maps it there, ct_def_version: an accessor's or an alias's made
     method has a def of its own) */
  int mi = call_target(c, id, ci, name, (sg ? CT_CMETHOD : 0) | (ct_calls(c, id, name) ? 0 : CT_CAPTURE), NULL);
  const LrDef *d = NULL;
  return mi >= 0 && elab_method_at(c, ci, name, sg, p, &d) == ELAB_YES && d &&
         (d->node == c->scopes[mi].def_node || ct_def_version(c, ci, name, sg, d, NULL) == mi);
}
/* See call_plan.h. */
int cplan_module_call_on_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int rv = nm ? nt_ref(nt, id, "receiver") : -1;
  const Scope *s = nm ? comp_scope_of(c, id) : NULL;
  return s && s->is_cmethod && (rv < 0 || nt_kind(nt, rv) == NK_SelfNode) && nt_ref(nt, id, "block") < 0 &&
         builtin_module_method_known(nm) && !an_user_defines_method(c, nm) && !vis_decl_call(c, id);
}
/* See call_plan.h. */
int cplan_method_vis(Compiler *c, int id, int ci, const char *name, int flags, int *at) {
  int sg = (flags & CPV_CMETHOD) != 0, root = (flags & CPV_ROOT) != 0, here = -1, asked = 0;
  mtab_enter(c);   /* (the question's own reads: the tables', the record's and the binding's) */
  int vis = sg ? comp_cmethod_vis_declared(c, ci, name, at) : comp_method_vis_declared(c, ci, name, at);
  if (ci >= 0 && name && (root || (ct_version_named(c, name) && elab_method_vis_varies(c, name)))) {
    LoadPoint p = elab_point_of(c, id);
    asked = 1;
    here = elab_method_vis_at(c, ci, name, sg, p);
    if (here == vis) asked = 0;   /* (the record answered, and agrees) */
    /* (the binding is asked only where the answers differ) */
    else if (here >= 0 && !root && !cplan_bound_at_point(c, id, ci, name, sg, p)) here = -1;
  }
  mtab_leave(c);
  if (here >= 0) return here;
  /* (a name whose visibility the record may tell apart, which it did not: the declared one is the table's last word) */
  if (asked && g_method_table_check) mtab_table(c, id, ci, sg ? CT_CMETHOD : 0, name, MLW_VIS);
  return vis;
}

/* See call_plan.h. */
int cplan_own_list_vis(Compiler *c, int id, int cls, const char *name, int vis) {
  int v = cplan_body_word_vis(c, id, cls, name);
  return v >= 0 ? v : vis;
}
/* See call_plan.h. */
int cplan_cmethod_list_vis(Compiler *c, int id, int cls, const char *name, int vis) {
  mtab_enter(c);
  int own = id >= 0 && cls >= 0 && vis == comp_cmethod_vis_declared(c, cls, name, NULL);
  mtab_leave(c);
  return own ? cplan_method_vis(c, id, cls, name, CPV_CMETHOD, NULL) : vis;
}

/* Whether the boxed class arm calls the method rather than raising for
   its visibility. Explicit self may call private methods too.
   The class-body self stamp identifies the caller of a protected call. */
int cplan_cmethod_visible(Compiler *c, int id, int ci, const char *name) {
  if (nt_ignore_visibility(c->nt, id)) return 1;
  int owner = -1;
  int vis = cplan_method_vis(c, id, ci, name, CPV_CMETHOD, &owner);
  if (vis == SP_VIS_PROTECTED && !nt_str(c->nt, id, "vis_enforce")) {
    if (nt_kind(c->nt, nt_ref(c->nt, id, "receiver")) == NK_SelfNode ||
        nt_int(c->nt, id, "self_call", 0)) return 1;
    Scope *s = comp_scope_of(c, id);
    int caller = s && s->is_cmethod ? s->class_id : -1;
    if (caller >= 0 && owner >= 0 && is_descendant(c, caller, owner)) return 1;
  }
  if (vis != SP_VIS_PUBLIC && !nt_str(c->nt, id, "vis_enforce") &&
      (nt_kind(c->nt, nt_ref(c->nt, id, "receiver")) == NK_SelfNode || nt_int(c->nt, id, "self_call", 0)) &&
      cplan_has_method_missing(c, id, ci, 1)) return 1;
  return vis == SP_VIS_PUBLIC;
}

/* (the face of the entry whose scope is e: VF_NONE for none) */
static int ver_entry_face(const Compiler *c, int e) {
  for (int i = 0; e >= 0 && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e) return c->ver_entries[i].face;
  return VF_NONE;
}
/* See call_plan.h. */
int cplan_cmethod_arm(Compiler *c, int call, int k, const char *nm, MethodLookup *ml, int *pfk, int *vis) {
  MethodLookup own = { .def_class = -1 };
  if (!ml) ml = &own;
  int mi = call_target(c, call, k, nm, CT_CMETHOD, ml);
  if (mi < 0) return -1;
  int pf = scope_proc_form_of(c, mi);
  if (pf >= 0 && !proc_form_live(c, pf)) pf = -1;
  if (!scope_has_callable_symbol(c, mi) && pf < 0) return -1;
  if (pfk) *pfk = pf;
  /* (an entry whose face closes tells the version's visibility as it runs: its arms raise) */
  if (vis) *vis = (ml->kind == ML_ENTRY && ver_face_closes(ver_entry_face(c, mi))) || cplan_cmethod_visible(c, call, k, nm)
                      ? SP_VIS_PUBLIC : cplan_method_vis(c, call, k, nm, CPV_CMETHOD, NULL);
  return mi;
}

/* The class-value trial arms of call id's class-tag switch into p: a Class
   value the receiver can hold (as far as the walk bounds them) whose class
   has no method of the name, for a builtin Module / Class method that
   answers from its receiver's class (is_class_value_reflection). Its answer
   for that class only the emission can tell (a trial: kept where the Class
   emitter answers from the class, and only from what the record proves
   settled before the call: the emitters answer as of the load's end). */
static void cpoly_cls_value_trials(Compiler *c, int id, const char *name, int argc, PolyPlan *p, int *cap) {
  const char *cvals = nt_ref(c->nt, id, "receiver") >= 0 && is_class_value_reflection(name) ? poly_recv_class_values(c, id) : NULL;
  const unsigned all = ELAB_GAIN_METHODS | ELAB_GAIN_INHERITED | ELAB_GAIN_CONSTS | ELAB_GAIN_CVARS;
  for (int k = 0; cvals && k < c->nclasses; k++) {
    if (!cvals[k] || cls_sg_accessor_first(c, id, k, name, argc) || cplan_cmethod_arm(c, id, k, name, NULL, NULL, NULL) >= 0)
      continue;
    int framed = elab_plan_enter(c, id);
    unsigned unsettled = elab_class_unsettled(c, k, all, id, cplan_plain_ctx());
    if (framed) elab_plan_leave(c);
    if (unsettled == all) continue;
    cpoly_add(p, cap, PA_TRIAL, PA_KEY_CLASS_VALUE + k, -1, TY_UNKNOWN, 0);
    p->arm[p->n - 1].unsettled = (unsigned char)unsettled;
  }
}

PolyPlan cplan_poly_cmeth(Compiler *c, int id) {
  PolyPlan cp = {0};
  cp.ntype = cp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  if (!name) return cpoly_keep(c, cp);
  int argc = 0;
  call_args(c->nt, id, &argc);
  for (int k = 0; k < c->nclasses; k++) {
    if (cls_sg_accessor_first(c, id, k, name, argc)) continue;
    int pf = -1, vis = SP_VIS_PUBLIC;
    int mi = cplan_cmethod_arm(c, id, k, name, NULL, &pf, &vis);
    if (mi < 0 || vis != SP_VIS_PUBLIC) continue;
    TyKind mret = c->scopes[pf >= 0 ? pf : mi].ret;
    cpoly_add(&cp, &cap, pf >= 0 ? PA_PROC_FORM : PA_USER, k, mi, mret, mret == TY_POLY ? PC_SAME : PC_BOX);
  }
  cpoly_cls_value_trials(c, id, name, argc, &cp, &cap);
  return cpoly_keep(c, cp);
}

PolyPlan cplan_poly_cls_values(Compiler *c, int id) {
  PolyPlan vp = {0};
  vp.ntype = vp.ret = comp_ntype(c, id);
  int cap = 0, argc = 0;
  const char *name = nt_str(c->nt, id, "name");
  call_args(c->nt, id, &argc);
  if (name) cpoly_cls_value_trials(c, id, name, argc, &vp, &cap);
  return cpoly_keep(c, vp);
}

/* Does call id's receiver, a String, Integer or Float that may be nil,
   reach a method only nil has: the program's NilClass defines the name
   (publicly), and neither the receiver's class nor Kernel, BasicObject or
   the top level has a method of it? Boxed, the call's dispatch then runs
   NilClass's method for nil, and for the rest Object's public method of
   the name where the program adds one, else raises NoMethodError. */
/* Can the boxed dispatch's default arm run call id, one with a splat
   argument or a block, as Object's method of the name? Not for a name a
   builtin has (its builtin answer comes first, and its arm is no
   spreading one), more than POLY_OBJ_SPREAD_MAX splats, or another
   argument with a side effect: the arm reads such an argument again, or
   declines (emit_poly_builtin_default_at). A call with neither a splat
   nor a block: 1. */
int cplan_nil_scalar_form_ok(Compiler *c, int id, const char *name) {
  const NodeTable *nt = c->nt;
  int argc = 0, nsplat = 0;
  const int *argv = call_args(nt, id, &argc);
  for (int i = 0; i < argc; i++) nsplat += nt_kind(nt, argv[i]) == NK_SplatNode;
  if (!nsplat && nt_ref(nt, id, "block") < 0) return 1;
  if (builtin_object_method_known(name) || builtin_instance_name_known(name)) return 0;
  if (nsplat > POLY_OBJ_SPREAD_MAX) return 0;
  for (int i = 0; i < argc; i++)
    if (nt_kind(nt, argv[i]) != NK_SplatNode && subtree_has_side_effect(c, argv[i])) return 0;
  return 1;
}
int cplan_nil_scalar_reach(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_CallNode || nt_safe_nav(nt, id)) return 0;
  const char *name = cplan_call_name(nt, id);
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0 || !cplan_nil_scalar_form_ok(c, id, name)) return 0;
  TyKind rt = comp_ntype(c, recv);
  int core = rt == TY_STRING || rt == TY_STRBUF ? CORE_STRING
           : rt == TY_INT ? CORE_INTEGER : rt == TY_FLOAT ? CORE_FLOAT : -1;
  if (core < 0) return 0;
  if (core == CORE_STRING ? !repr_of(c, recv).may_nil : !nullable_int_value(c, recv)) return 0;
  int nilcls = 0, nmi = cplan_nil_program_method(c, id, name, &nilcls);
  if (nmi < 0 || !nilcls) return 0;
  /* a method of the name on Kernel, BasicObject or at the top level, or
     a private one on Object, keeps the old path */
  if (call_target(c, id, -1, name, CT_TOP, NULL) >= 0) return 0;
  /* a public method the program adds to Object stays the answer for the
     rest: the boxed dispatch's default arm runs it */
  static const int others[] = { CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  for (int i = 0; others[i] >= 0; i++) {
    int ok = comp_core_class(c, others[i]);
    int omi = ok >= 0 ? call_target(c, id, ok, name, 0, NULL) : -1;
    if (omi >= 0 &&
        !(others[i] == CORE_OBJECT && cplan_method_vis(c, id, ok, name, 0, NULL) == SP_VIS_PUBLIC)) return 0;
    /* (a block into one that yields keeps the old path: the default arm
       does not hand Object's method the block) */
    if (omi >= 0 && nt_ref(nt, id, "block") >= 0 && c->scopes[omi].yields) return 0;
  }
  /* (a private or protected one keeps the old path: the boxed dispatch
     does not check visibility) */
  int nk = comp_core_class(c, CORE_NILCLASS);
  if (cplan_method_vis(c, id, nk, name, 0, NULL) != SP_VIS_PUBLIC) return 0;
  int k = comp_core_class(c, core);
  return k < 0 || call_target(c, id, k, name, 0, NULL) < 0;
}

/* See call_plan.h. */
int cplan_nil_scalar_boxed(Compiler *c, int id, const char *name, int recv, TyKind rt) {
  const NodeTable *nt = c->nt;
  if (recv < 0 || !cplan_nil_scalar_form_ok(c, id, name)) return 0;
  /* (a block into Object's method of the name that yields keeps the old
     path, as cplan_nil_scalar_reach) */
  int oc = comp_core_class(c, CORE_OBJECT), nk = comp_core_class(c, CORE_NILCLASS);
  int omi = cplan_core_method(c, id, CORE_OBJECT, name);
  if (nt_ref(nt, id, "block") >= 0 && omi >= 0 && c->scopes[omi].yields) return 0;
  if (cplan_core_method(c, id, CORE_NILCLASS, name) < 0 || cplan_method_vis(c, id, nk, name, 0, NULL) != SP_VIS_PUBLIC) return 0;
  /* beside a public method the program adds to Object, an Integer or a
     Float answers boxed before the analysis settles whether it may be nil:
     an Array the answer is pushed into is built boxed from the start, not
     typed by Object's method alone */
  int beside_obj = (rt == TY_INT || rt == TY_FLOAT) && omi >= 0 && cplan_method_vis(c, id, oc, name, 0, NULL) == SP_VIS_PUBLIC;
  return rt == TY_STRING || rt == TY_STRBUF || beside_obj || ((rt == TY_INT || rt == TY_FLOAT) && nullable_int_value(c, recv));
}

PolyPlan cplan_poly_sub(Compiler *c, int id) {
  PolyPlan sp = {0};
  sp.ntype = sp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  if (!name || recv < 0) return cpoly_keep(c, sp);
  if (cplan_nil_scalar_reach(c, id)) { cpoly_family(&sp, &cap, PB_SUBDISPATCH); return cpoly_keep(c, sp); }
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_object(rt) || comp_ty_value_obj(c, rt)) return cpoly_keep(c, sp);
  int bcid = ty_object_class(rt);
  for (int k = 0; k < c->nclasses; k++) {
    if (k == bcid) continue;
    int anc = 0;
    for (int p2 = c->classes[k].parent; p2 >= 0; p2 = c->classes[p2].parent)
      if (p2 == bcid) { anc = 1; break; }
    if (anc && (call_answers(c, id, k, name, 0, NULL))) {
      cpoly_family(&sp, &cap, PB_SUBDISPATCH);
      break;
    }
  }
  return cpoly_keep(c, sp);
}

PolyPlan cplan_poly_redispatch(Compiler *c, int id) {
  PolyPlan rp = {0};
  rp.ntype = rp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  int argc = 0;
  (void)call_args(c->nt, id, &argc);
  if (!name || recv < 0 || comp_recv_type(c, recv) != TY_POLY) return cpoly_keep(c, rp);
  int kind = poly_redispatch_kind(c, id, name, argc);
  if (kind) cpoly_family(&rp, &cap, kind == 2 ? PB_REDISPATCH_RECV : PB_REDISPATCH);
  return cpoly_keep(c, rp);
}

PolyPlan cplan_poly_face(Compiler *c, int id) {
  PolyPlan fp = {0};
  fp.ntype = fp.ret = comp_ntype(c, id);
  int cap = 0;
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int argc = 0;
  call_args(nt, id, &argc);
  int has_blk = nt_ref(nt, id, "block") >= 0, plain = nt_call_args_plain(nt, id);
  if (!name) return cpoly_keep(c, fp);
  unsigned own = an_zero_arg_builtin_shadowed(c, name, argc) ? 0
                 : ty_poly_face_owners(name, argc, has_blk, plain, 0);
  unsigned kinds = own & PF_OWNERS;
  if (own & PF_STR_BANG) { cpoly_family(&fp, &cap, PB_FACE_STR_BANG); return cpoly_keep(c, fp); }
  for (unsigned kind = 1; kind & PF_OWNERS; kind <<= 1) {
    if (!(kinds & kind)) continue;
    /* an argument of another kind is the owner's TypeError */
    int misfit = face_args_misfit(c, id, kind);
    cpoly_add(&fp, &cap, misfit ? PA_BUILTIN : PA_TRIAL, PA_KEY_FACE + face_kind_index(kind), -1, TY_UNKNOWN,
              PC_SAME);
  }
  return cpoly_keep(c, fp);
}

/* ---- CN_*: a call's nil target (#7444) ---- */

/* the receivers whose nil target the plan decides: a typed pointer whose
   NULL is nil, a builtin's or a user object's (repr_of says which object
   is held as a pointer; a by-value one has no nil), whose arguments then
   run ahead of the nil test (emit_nil_target_head) */
static int cplan_nil_family(TyKind t) {
  return t == TY_STRING || ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) ||
         t == TY_IO || ty_is_object(t);
}

/* Is self at node id an Object, so that Object's protected method is open
   to it? Main and a class method's class are; an instance method's self
   is, unless its class is a blank slate (BasicObject's subclass). */
static int object_protected_open(Compiler *c, int id) {
  Scope *cs = comp_scope_of(c, id);
  if (!cs || cs->is_cmethod || cs->class_id < 0) return 1;
  return !class_is_blank_slate(c, cs->class_id);
}
/* Has a def of `name` on root k's chain (main's own, a top-level def,
   for k < 0) run wherever call `call` runs, which has no place in the
   load (a method body, a closure), as the record proves it: at every
   state of the statements it may run in (ct_every_span)? One that may run
   before every def finds none there: its NoMethodError is the undefined
   method's, not a private one's. */
static int cplan_root_def_ran(Compiler *c, int call, int k, const char *name) {
  const LrDef *ds[8];
  int framed = elab_plan_enter(c, call);
  int n = ct_every_span(c, call, k >= 0 ? k : -1 /* main */, name, 0, ds, 8);
  if (framed) elab_plan_leave(c);
  return n > 0;
}
/* Root k's visibility vis of `name` as call `call` meets it: a protected
   one of Object's, Kernel's or main's is open wherever self is an Object,
   one of BasicObject's wherever self is anything, but never to
   public_send */
static int cplan_root_vis_open(Compiler *c, int call, int k, const char *name, int vis) {
  int oc = comp_core_class(c, CORE_OBJECT);
  if (vis == SP_VIS_PROTECTED && !nt_str(c->nt, call, "vis_enforce") &&
      (k == comp_core_class(c, CORE_BASIC_OBJECT) ||
       ((k < 0 || k == oc || k == comp_core_class(c, CORE_KERNEL)) && object_protected_open(c, call)) ||
       (k >= 0 && method_vis_for_caller(c, call, k, name) == SP_VIS_PUBLIC))) vis = SP_VIS_PUBLIC;
  return vis;
}
/* The visibility at call `call` of root k's method `name` (k -1: main's,
   a top-level def, which is Object's private method until `public :name`),
   as the record has it there, else as declared, as call meets it
   (cplan_root_vis_open); -1 where the record cannot tell for main's (a
   def or a visibility word under a condition), and for a call with no
   place in the load that may run before every def (cplan_root_def_ran). */
static int cplan_root_vis(Compiler *c, int call, int k, const char *name) {
  int oc = comp_core_class(c, CORE_OBJECT), vis;
  int placed = elab_point_of(c, call).region >= 0;
  if (k < 0) {
    int mv = elab_main_method_vis_at(c, call, name), at = -1;
    vis = mv == 1 ? SP_VIS_PUBLIC : mv == 2 ? SP_VIS_PROTECTED : mv == 4 ? SP_VIS_PRIVATE : -1;
    /* (a call with no place in the load takes the declared one, where
       the def ran before it and may not run again) */
    if (vis < 0 && (placed || !cplan_root_def_ran(c, call, k, name))) {
      /* (the version's own arm visibility, where the record proves it for
         the one version) */
      int lv = cplan_top_def_surely_live(c, call, name) ? cplan_top_lone_vis(c, name, NULL) : -2;
      return lv >= 0 ? cplan_root_vis_open(c, call, k, name, lv) : -1;
    }
    ElabVersion vs[8];
    int n = vis < 0 ? elab_history_versions(c, -1 /* main */, 0, name, vs, 8) : 0;
    for (int i = 0; i < n; i++) if (vs[i].v->may) return -1;
    if (n < 0) return -1;
    /* (a call with no place takes the visibility the def's arm has: the declared
       one gives way to a word that only the run orders against the call, which
       the version's cell tells) */
    int lv = vis < 0 ? cplan_top_lone_vis(c, name, NULL) : -2;
    if (lv == -1) return -1;
    if (lv >= 0) vis = lv;
    if (vis < 0) vis = oc >= 0 ? cplan_method_vis(c, call, oc, name, 0, &at) : SP_VIS_PRIVATE;
    if (mv < 1 && at < 0 && lv < 0) vis = SP_VIS_PRIVATE;
  }
  else {
    /* (as the record has it at the call, the roots' lookup having bound it) */
    if (!placed && elab_method_vis_at(c, k, name, 0, elab_point_of(c, call)) < 0 && !cplan_root_def_ran(c, call, k, name)) return -1;
    vis = cplan_method_vis(c, call, k, name, CPV_ROOT, NULL);
  }
  return cplan_root_vis_open(c, call, k, name, vis);
}
/* Has owner's def of `name` (its own part of the lookup) run wherever
   call `call` runs, where that is not its point: in a lazy chain's block,
   at each call that forces the chain (elab_lazy_runs), each one run at
   its own point, with one visibility there (*vis, as the record has it
   at each)? 0 where the record does not know every such place, one may
   run before the def, or the visibility is not known at one or differs
   between them. */
static int cplan_root_def_runs_first(Compiler *c, int call, int owner, const char *name, int *vis) {
  int sites[16], n = elab_lazy_runs(c, call, sites, 16);
  *vis = -1;
  for (int k = 0; k < n; k++) {
    LoadPoint sp = elab_point_of(c, sites[k]);
    if (sp.region < 0 || !elab_blocks_run_at_once(c, sites[k]) || elab_reached_method_at(c, owner, name, sp) != ELAB_YES) return 0;
    int v = elab_method_vis_at(c, owner, name, 0, sp);
    if (v < 0 || (k && v != *vis)) return 0;
    *vis = v;
  }
  return n > 0;
}
/* See call_plan.h. */
int cplan_root_method(Compiler *c, int call, const char *cls, const char *name, int *vis) {
  if (vis) *vis = -1;
  if (!cls || !name) return -1;
  for (int i = 0;; i++) {
    const char *a = builtin_ancestor_at(cls, i);
    if (!a) return -1;
    int s = builtin_core_class_slot(a);
    int root = cplan_root_slot(s);
    int k = comp_builtin_class_named(c, a), owner = k;
    int mi = k >= 0 ? call_target(c, call, k, name, CT_OWN, NULL) : -1;
    /* a top-level def is Object's from where it runs: of the two, the
       later def answers */
    if (s == CORE_OBJECT) {
      /* (the def is asked of the tables, not bound for the call: the walk
         binds nothing, so it asks no version of the def and makes no
         version demand; where the record proves the call runs before the
         def, there is none) */
      int tm = ct_send_shadow(c, call, name) ? -1 : call_target(c, -1, -1, name, CT_TOP, NULL);
      if (tm >= 0 && (elab_main_method_vis_at(c, call, name) == 0 || ct_undefined_at(c, call, -1, name, CT_TOP, tm))) tm = -1;
      int live = tm >= 0 && mi >= 0 ? elab_main_def_live_at(c, call, name) : 1;
      if (live < 0) {
        /* (the record cannot tell which is live: the call is closed where
           every version is closed to it, and otherwise per version, by the
           face of the entry it binds: ct_face_for) */
        int face = ct_flags_face(ct_enforce_flags(c, call)), v = -1, st = face ? ver_arms_closed(c, k, name, face, &v) : -2;
        /* (a call with a block binds no entry: a yielding method's call
           is made through the proc form of one version, so it stays
           closed on the top-level def's version, as before) */
        if (st == -1 && tm >= 0 && nt_ref(c->nt, call, "block") >= 0 && ver_closed_to(v = ver_def_vis(c, k, name, c->scopes[tm].def_node), face)) st = 1;
        if (st != 1) return -1;
        /* (and, for a name a builtin answers, only where a version has run:
           a call that may run before every one reaches the builtin's, as
           cplan_root_def_ran has it) */
        int framed = elab_plan_enter(c, call), ran = elab_call_method(c, call, k, name, 0, 1);
        if (framed) elab_plan_leave(c);
        if (ran != ELAB_YES && builtin_object_method_known(name)) return -1;
        if (vis) *vis = v;
        return tm >= 0 ? tm : mi;
      }
      if (tm >= 0 && live) mi = tm, owner = -1;
    }
    /* (a reopened builtin's own method below the roots answers by its
       class's arms) */
    if (mi >= 0 && !root) return -1;
    /* a root's def answers from where it runs: a call the record proves
       runs before every def, include and visibility change of the name in
       the root's own part of the lookup finds what the lookup finds past it
       (Kernel's tap, ahead of a later `module Kernel; private; def tap`).
       A call in a lazy chain's block runs where the chain is forced: the
       def answers it where every such place is after the def, with the
       visibility the record has there, one at all of them
       (cplan_root_def_runs_first); where the record does not know every
       place, or they straddle the def, the call answers from its point
       (a block a method keeps: telling the two apart takes a run-time
       check of the root's version).
       (A top-level def's place is asked above; a call with no place in the
       load is cplan_root_vis's, by cplan_root_def_ran. Where the record
       cannot order them, a def under a condition, the def still answers,
       except for Object's against the top level's, which the face entries
       tell apart at run time (ct_face_for): another root's versions have
       a self of their own type, which an entry's boxed self does not fit.) */
    int fv = -1;
    if (mi >= 0 && owner >= 0) {
      LoadPoint cp = elab_point_of(c, call);
      if (cp.region >= 0 && elab_reached_method_at(c, owner, name, cp) == ELAB_NO && !cplan_root_def_runs_first(c, call, owner, name, &fv)) mi = -1;
    }
    if (mi >= 0) {
      if (vis) *vis = fv >= 0 ? cplan_root_vis_open(c, call, owner, name, fv) : cplan_root_vis(c, call, owner, name);
      return mi;
    }
    /* a builtin's own method answers first, and so does a `public :name`
       the root declares itself */
    int at = -1;
    if (root && k >= 0 && cplan_method_vis(c, call, k, name, 0, &at) == SP_VIS_PUBLIC && at == k) return -1;
    if (builtin_owns_instance_method(a, name)) {
      int bv = core_method_vis(a, name, 0, NULL);
      if (vis) *vis = bv == 4 ? SP_VIS_PRIVATE : bv == 2 ? SP_VIS_PROTECTED : bv == 1 ? SP_VIS_PUBLIC : -1;
      return -1;
    }
  }
}
/* See call_plan.h. */
int cplan_nil_closed_method(Compiler *c, int call, const char *name, int *vis) {
  if (!name || call < 0) return -1;
  /* (a call that skips visibility, `send`'s, runs the method) */
  const NodeTable *nt = c->nt;
  if (nt_ignore_visibility(nt, call) || nt_str(nt, call, "send_blind") ||
      (nt_int(nt, call, "dyn_arm", 0) && !nt_str(nt, call, "vis_enforce"))) return -1;
  /* (a method_missing nil reaches answers the call instead) */
  if (elab_method_versions(c, "method_missing")) return -1;
  int v = SP_VIS_PUBLIC, mi = cplan_root_method(c, call, core_class_name(CORE_NILCLASS), name, &v);
  if (mi < 0 || (v != SP_VIS_PRIVATE && v != SP_VIS_PROTECTED)) return -1;
  if (vis) *vis = v;
  return mi;
}
/* See call_plan.h. */
int cplan_class_answers_ahead_of_object(Compiler *c, int call, int k, const char *name) {
  int oc = comp_core_class(c, CORE_OBJECT);
  if (k < 0 || k >= c->nclasses || k == oc || !name) return 0;
  int mi = call_target(c, call, k, name, 0, NULL);
  if (mi >= 0 && mi < c->nscopes && c->scopes[mi].class_id != oc) return 1;
  int at = -1;
  cplan_method_vis(c, call, k, name, 0, &at);
  if (at >= 0 && at != oc) return 1;
  char base[256];
  return call_attr_target(c, -1, k, name, CT_ATTR, NULL) ||
         (setter_base_name(name, base, sizeof base) && call_attr_target(c, -1, k, base, CT_ATTR | CT_WRITER, NULL)) ||
         cplan_has_method_missing(c, call, k, 0);
}
/* See call_plan.h. */
int cplan_guard_closed(Compiler *c, int id, int *vis) {
  const CallPlan p = cplan_user(c, id);
  if (p.mi < 0 || !p.vis_cell) return -1;
  if (vis) *vis = SP_VIS_PRIVATE;
  return p.mi;
}
/* See call_plan.h. */
int cplan_root_closed(Compiler *c, int id, int *vis) {
  const NodeTable *nt = c->nt;
  const char *name = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  int recv = name ? nt_ref(nt, id, "receiver") : -1;
  if (recv < 0) return -1;
  /* (a call that skips visibility, `send`'s, and one on self run the
     method; public_send refuses on any receiver) */
  if (!cplan_call_enforces_vis(c, id)) return -1;
  TyKind rt = comp_ntype(c, recv);
  /* (`nil&.m` calls nothing) */
  if (rt == TY_NIL && nt_safe_nav(nt, id)) return -1;
  if (rt == TY_NIL) return cplan_nil_closed_method(c, id, name, vis);
  /* (a method_missing every object reaches answers the call instead) */
  if (cplan_mm_method(c, id, comp_core_class(c, CORE_OBJECT), 0) >= 0) return -1;
  /* the lookup from the receiver's builtin class; a boxed value's from
     Object, which the arms ask at run time of a value whose own class
     answers the name ahead of Object (cplan_send_split_owners) */
  int bcls = object_fallback_class_of_type(rt);
  const char *cls = ty_is_object(rt) ? cplan_builtin_name_of(c, ty_object_class(rt))
                  : rt == TY_CLASS ? core_class_name(CORE_MODULE)
                  : rt == TY_BOOL ? core_class_name(CORE_TRUECLASS)
                  : bcls >= 0 ? core_class_name(bcls)
                  : rt == TY_POLY ? core_class_name(CORE_OBJECT) : NULL;
  /* the receiver's own class answers first: a program object's, or a
     subclass's the value may be an instance of; a builtin value's
     reopened class (a boxed value's is asked at run time) */
  int own = ty_is_object(rt) ? ty_object_class(rt) : rt != TY_POLY && cls ? comp_builtin_class_named(c, cls) : -1;
  if (cplan_class_answers_ahead_of_object(c, id, own, name)) return -1;
  int nd = 0;
  const int *ds = ty_is_object(rt) && own >= 0 ? comp_descendants(c, own, &nd) : NULL;
  for (int i = 0; i < nd; i++) if (cplan_class_answers_ahead_of_object(c, id, ds[i], name)) return -1;
  /* (an exception value may be of any exception class: one whose own
     accessor answers first, UncaughtThrowError#value, or a class of the
     program's or a reopening that answers the name; the exception arms ask
     its class at run time) */
  if (rt == TY_EXCEPTION) {
    int xr[1];
    if (is_gated_exception_accessor(name) || exc_reopen_definers(c, name, xr, 1) > 0) return -1;
    for (int k = 0; k < c->nclasses; k++)
      if (class_is_exc_subclass(c, k) && cplan_class_answers_ahead_of_object(c, id, k, name)) return -1;
  }
  /* (one that may be nil is closed only where nil's lookup closes it too) */
  if (rt != TY_POLY && repr_of(c, recv).may_nil && cplan_nil_closed_method(c, id, name, NULL) < 0) return -1;
  int v = -1, mi = cplan_root_method(c, id, cls, name, &v);
  /* a builtin's own private method found first (Array#initialize_copy)
     closes the call to the program's root method behind it too */
  if (mi < 0 && (v == SP_VIS_PRIVATE || v == SP_VIS_PROTECTED))
    mi = cplan_root_method(c, id, core_class_name(CORE_OBJECT), name, NULL);
  if (mi < 0 || (v != SP_VIS_PRIVATE && v != SP_VIS_PROTECTED)) return -1;
  if (vis) *vis = v;
  return mi;
}

/* See call_plan.h. */
int cplan_top_def_vis(Compiler *c, int call, const char *name) {
  if (!name || call_target(c, -1, -1, name, CT_TOP, NULL) < 0) return -1;
  int v = -1, mi = cplan_root_method(c, call, core_class_name(CORE_OBJECT), name, &v);
  return mi >= 0 && c->scopes[mi].class_id < 0 ? v : -1;
}

/* See call_plan.h. */
int cplan_top_def_absent(Compiler *c, int call, const char *name) {
  int tm = call >= 0 && name ? call_target(c, -1, -1, name, CT_TOP, NULL) : -1;
  return tm >= 0 && ct_undefined_at(c, call, -1, name, CT_TOP, tm);
}

/* The record's ask of a root's method, once the tables are known to hold it as the root's own */
static int root_def_absent_at(Compiler *c, int call, int root, const char *name) {
  MethodLookup ml = { .def_class = -1 };
  call_target(c, call, root, name, CT_REACH, &ml);
  return ml.kind == ML_NONE && ml.why == MLW_RECORD;
}
/* See call_plan.h. */
int cplan_root_def_absent(Compiler *c, int call, int root, const char *name) {
  if (call < 0 || root < 0 || !name || call_target(c, -1, root, name, CT_OWN, NULL) < 0) return 0;
  return root_def_absent_at(c, call, root, name);
}
/* See call_plan.h. */
int cplan_omit_absent(Compiler *c, int call, const CplanOmitCand *k) {
  if (call < 0) return 0;
  return k->root >= 0 ? root_def_absent_at(c, call, k->root, k->name)
                      : ct_undefined_in(c, call, -1, k->chain, k->name, CT_TOP, k->mi, k->self_main);
}

/* See call_plan.h. */
const LocalVar *cplan_top_def_cell(Compiler *c, int call, const char *name) {
  int v = -1, cell = -1;
  if (!name || call_target(c, -1, -1, name, CT_TOP, NULL) < 0 || builtin_name_below_object(name)) return NULL;
  int mi = cplan_root_method(c, call, core_class_name(CORE_OBJECT), name, &v);
  if (mi < 0 || c->scopes[mi].class_id >= 0 || v != -1 || cplan_top_lone_vis(c, name, &cell) != -1 || cell < 0 ||
      !cplan_top_def_surely_live(c, call, name)) return NULL;
  CallPlan g;
  cplan_set(&g, -1, -1, UC_NONE, CP_NONE);
  g.vis_cell = cell + 1;
  return cplan_vis_cell_var(c, &g);
}

int cplan_nil_program_method(Compiler *c, int call, const char *name, int *nilcls) {
  int v, mi = cplan_root_method(c, call, core_class_name(CORE_NILCLASS), name, &v);
  int k = mi >= 0 ? c->scopes[mi].class_id : -1;
  *nilcls = k >= 0 && k == comp_core_class(c, CORE_NILCLASS);
  return k >= 0 && (*nilcls || k == comp_core_class(c, CORE_OBJECT)) ? mi : -1;
}

/* nil's answer to respond_to?(name) by a method of the program's: the
   first of NilClass, Object (a top-level def is a private Object method
   until a word at the call makes it public),
   Kernel and BasicObject that defines it answers, true when that method is
   public or include_all is set (a literal `true` 2nd argument); -1 when
   none of them defines it. */
int cplan_nil_program_responds(Compiler *c, int call, const char *name, int include_all) {
  static const int owners[] = { CORE_NILCLASS, CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  /* an undef on NilClass or Object (above the first definer) hides the
     lower owners' method */
  int nk = comp_core_class(c, CORE_NILCLASS);
  CplanPresence npr = { 0 };
  if (nk >= 0) cplan_presence(c, call, nk, 0, name, &npr);
  if (npr.undef) return 0;
  int ia = include_all < 0 ? 2 : include_all;
  /* NilClass's own public methods (CRuby's NilClass.public_instance_methods
     false): one sits below every program method of Object, Kernel and
     BasicObject, so a top-level `def to_a` leaves nil's answer true */
  for (int i = 0; owners[i] >= 0; i++) {
    if (owners[i] == CORE_OBJECT && builtin_answers_ahead_of("NilClass", name, "Object")) return 1;
    int k = comp_core_class(c, owners[i]);
    if (k < 0 || call_target(c, call, k, name, CT_REACH, NULL) < 0) {
      if (owners[i] == CORE_OBJECT && call_target(c, -1, -1, name, CT_TOP, NULL) >= 0)
        return cplan_top_def_vis(c, call, name) == SP_VIS_PUBLIC ? 1 : ia;
      continue;
    }
    return cplan_method_vis(c, call, k, name, 0, NULL) == SP_VIS_PUBLIC ? 1 : ia;
  }
  return -1;
}

/* Does the receiver of call `call` (of type rt) run another method than
   nil does, or none: a reopened String's, Array's or Hash's, or the
   object's class's own, where nil runs NilClass's or Object's? */
static int cplan_nil_runs_other(Compiler *c, int call, TyKind rt, const char *name) {
  /* (a method closed to the call raises for nil, where the receiver's
     class may run its own) */
  int nilcls = 0, pmi = cplan_nil_program_method(c, call, name, &nilcls);
  int nmi = cplan_nil_closed_method(c, call, name, NULL);
  if (nmi < 0) nmi = pmi;
  if (nmi < 0) return 0;
  int k = rt == TY_STRING ? comp_core_class(c, CORE_STRING)
        : ty_is_array(rt) ? comp_core_class(c, CORE_ARRAY)
        : ty_is_hash(rt) ? comp_core_class(c, CORE_HASH)
        : ty_is_object(rt) ? ty_object_class(rt) : -1;
  if (k < 0) return 0;
  int own = call_target(c, call, k, name, 0, NULL);
  /* (a receiver whose class has none runs Object's through its fallback,
     with the value boxed, which answers for nil as nil's own) */
  return own != nmi && (own >= 0 || nilcls);
}

int cplan_nil_written(int why) {
  return why == NFW_NIL || why == NFW_NO_ELSE || why == NFW_SAFE_NAV || why == NFW_UNSET || why == NFW_ELEM_NIL || why == NFW_ELEM ||
         why == NFW_TAKEN;
}

/* The name call id was written as: an alias of a reader is renamed to the
   reader (infer_call_inner), but nil answers the name the program called. */
const char *cplan_call_name(const NodeTable *nt, int id) {
  const char *wn = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "written_name") : NULL;
  return wn ? wn : nt_str(nt, id, "name");
}

/* Does an argument of call id (a keyword's value too) hold an Integer or a
   Float that can be nil, or a boxed value? A String method converts it as
   it reads it, and nil, or a value of another class, raises TypeError
   there. */
static int cplan_nil_converted_arg(Compiler *c, int v) {
  TyKind t = comp_ntype(c, v);
  return t == TY_POLY || (oint_kind(t) && node_is_oint(c, v));
}
static int cplan_nil_arg_converts(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int a = nt_ref(nt, id, "arguments"), an = 0;
  const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
  for (int i = 0; i < an; i++) {
    int v = av[i];
    if (nt_kind(nt, v) == NK_KeywordHashNode) {
      int en = 0;
      const int *ev = nt_arr(nt, v, "elements", &en);
      for (int j = 0; j < en; j++) {
        int kv = nt_kind(nt, ev[j]) == NK_AssocNode ? nt_ref(nt, ev[j], "value") : -1;
        if (kv >= 0 && cplan_nil_converted_arg(c, kv)) return 1;
      }
    }
    else if (cplan_nil_converted_arg(c, v)) return 1;
  }
  return 0;
}

/* An ivar read the plan tests, where the release build's policy leaves
   the others to their arms: an Array in one that no class's initialize
   sets first (NFW_IVAR), or a String there that call `nm` changes. The
   Array arms read a NULL receiver as an empty Array or read through it
   (`@stops[1]` answered nil, `@stops.first(2)` crashed), and a String
   mutator's frozen check read it as a frozen String (FrozenError, "can't
   modify frozen String: nil"), where CRuby raises NoMethodError; it is
   tested as a local's is. */
static int cplan_nil_ivar_tested(Compiler *c, int r, TyKind rt, const char *nm) {
  return (ty_is_array(rt) || ty_is_obj_array(rt) || (rt == TY_STRING && an_str_mutator_name(nm))) &&
         nil_fact_why(c, r) == NFW_IVAR;
}

/* Do only the builtins convert a `&` value: no to_proc of the program's
   own (`class Integer; def to_proc`), and no respond_to? or
   respond_to_missing? that CRuby asks before it calls method_missing? */
static int cplan_block_conv_builtin(Compiler *c) {
  if (!c->block_arg_conv)
    c->block_arg_conv = an_name_binder_free(c, "to_proc") && an_name_binder_free(c, "respond_to?") &&
                        an_name_binder_free(c, "respond_to_missing?") ? 1 : 2;
  return c->block_arg_conv == 1;
}
/* The class CRuby's TypeError names ("no implicit conversion of X into
   Proc") for a `&fe` argument whose type cannot become a block: X, "" for a
   Boolean, whose value names TrueClass or FalseClass; NULL where it
   converts or its type does not say (a Proc, Method, curried Proc, Symbol,
   Hash, nil, an object with to_proc, a boxed or unknown value). */
static const char *cplan_block_no_proc_class(Compiler *c, int blk, int fe) {
  Repr fr = repr_of(c, fe);
  TyKind ft = fr.as_ty;
  if (fr.kind == RK_BOXED || ft == TY_PROC || ft == TY_METHOD || ft == TY_SYMBOL || ft == TY_NIL ||
      ft == TY_POLY || ft == TY_UNKNOWN || ft == TY_VOID || ft == TY_CURRY || ty_is_hash(ft) ||
      (ty_is_object(ft) && cplan_implied_target(c, blk, fe, ty_object_class(ft), "to_proc", CIT_TABLE, NULL) >= 0)) return NULL;
  return ft == TY_BOOL ? "" : conv_cls_name_of(c, ft);
}
int cplan_block_arg(Compiler *c, int id, const char **cn) {
  const NodeTable *nt = c->nt;
  *cn = NULL;
  int blk = id >= 0 && id < nt->count && nt_kind(nt, id) == NK_CallNode ? nt_ref(nt, id, "block") : -1;
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockArgumentNode || nt_safe_nav(nt, id) ||
      !cplan_block_conv_builtin(c)) return CB_BINDS;
  int fe = nt_ref(nt, blk, "expression");
  if (fe < 0) return CB_BINDS;
  *cn = cplan_block_no_proc_class(c, blk, fe);
  if (!*cn) {
    Repr fr = repr_of(c, fe);
    return fr.kind == RK_BOXED || fr.as_ty == TY_POLY ? CB_CHECK : CB_BINDS;
  }
  /* what the slot can hold is the value's representation's fact: a nil
     passes no block, and only a value that cannot be nil raises */
  return repr_of(c, fe).may_nil ? CB_CHECK : CB_RAISE;
}

int cplan_nil(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || id >= nt->count || nt_kind(nt, id) != NK_CallNode) return CN_NONE;
  int r = nt_ref(nt, id, "receiver");
  const char *nm = cplan_call_name(nt, id);
  if (r < 0 || !nm || nt_safe_nav(nt, id)) return CN_NONE;
  /* the receiver's stored type, which an open type view on it writes (so
     under one this is the view's), tested below against its type here: a
     receiver the two differ on (a yield typed by its site, a String and
     its handle where the refinement reads one as the other) has no nil
     arm */
  TyKind rt = comp_ntype_settled(c, r);
  /* under --share-strings, a String the rule made the shared handle is
     nil as its NULL handle (#6765) */
  /* Exception text dispatch has the same nil target as an ordinary call,
     including a subclass whose #message invokes #to_s implicitly. */
  int exc_text = is_exception_message(nm) &&
    (rt == TY_EXCEPTION || (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt))));
  if (!(cplan_nil_family(rt) || exc_text || (rt == TY_STRBUF)) || comp_ntype(c, r) != rt) return CN_NONE;
  Repr rr = repr_of(c, r);
  if ((rr.kind != RK_PTR && rr.kind != RK_STRBUF) || !rr.may_nil || rr.nil_tested) return CN_NONE;
  /* an ivar keeps the release build's policy (ivar_nil_recv_guard, #5960):
     a hot path reads one that a reset or a setup fills, and a test in front
     of every call through it is not paid for; a class variable's arms write
     back into it */
  /* ...but not on a String call with an argument it converts
     (cplan_nil_arg_converts): the arm or the runtime tests the receiver
     after it has converted the argument, and the argument's TypeError came
     first where CRuby raises NoMethodError, having run the arguments. (A
     mutator writes back into the slot the head would hold.) */
  /* (one-String mode: every String slot is the handle, TY_STRBUF) */
  int conv = (rt == TY_STRING || (rt == TY_STRBUF)) && !an_str_mutator_name(nm) &&
             cplan_nil_arg_converts(c, id);
  /* (and see cplan_nil_ivar_tested) */
  NodeKind rk = nt_kind(nt, r);
  int ivar_tested = rk == NK_InstanceVariableReadNode && cplan_nil_ivar_tested(c, r, rt, nm);
  if ((rk == NK_InstanceVariableReadNode && !conv && !ivar_tested) || rk == NK_ClassVariableReadNode) return CN_NONE;
  /* a local or a global is tested in its slot, where a mutator writes
     back: a slot that holds the pointer, or a shared String's handle; one
     held another way (a box a read unboxes) is left as it is */
  if (rk == NK_LocalVariableReadNode || rk == NK_GlobalVariableReadNode) {
    const char *sn = nt_str(nt, r, "name");
    LocalVar *lv = NULL;
    if (sn && rk == NK_LocalVariableReadNode) {
      Scope *sc = comp_scope_of(c, r);
      lv = sc ? scope_local(sc, sn) : NULL;
    }
    else if (sn && sn[0] == '$') lv = comp_gvar(c, comp_resolve_gvar(c, sn + 1));
    Repr sr = repr_of_slot(c, lv);
    if (!lv || !(sr.kind == RK_STRBUF || (sr.kind == RK_PTR && sr.as_ty == rt))) return CN_NONE;
  }
  /* a shared String's handle a call renders is not bound like a value
     (one-String mode: an ivar's is its slot's, held by the head as a
     local's is; every ivar String is the handle there, where the
     default mode tests one the plan leaves untested in its arms) */
  else if (rr.kind == RK_STRBUF && !((ivar_tested || conv))) return CN_NONE;
  /* a nil the program writes (an element of an Array it stores one into
     or leaves a gap in, too); not one the fact cannot bound (an element
     read, a global, an ivar, a caller not seen, a builtin's answer), which
     a hot loop over a receiver that is never nil would pay for, unless the
     call converts an argument or reads an ivar the plan tests (conv and
     ivar_tested, as above) */
  int why = nil_fact_why(c, r);
  if (!cplan_nil_written(why) && !conv && !ivar_tested) return CN_NONE;
  /* an element read the facts keep within its array reads an element: it is
     nil only where the array holds one (NFW_ELEM_NIL), not past the end */
  if (why == NFW_ELEM && !conv && !ivar_tested && an_elem_read_in_bounds(c, r)) return CN_NONE;
  /* the definite-assignment walk over a temp the compiler wrote itself (a
     desugared splat's receiver) is not the program's nil */
  if (why == NFW_UNSET && nt_int(nt, r, "node_line", 0) <= 0) return CN_NONE;
  /* in a Ruby-defined builtin (builtins/enumerable.rb), only its receiver
     is the program's value: its own locals are never nil */
  if (enum_builtin_node(c, id)) {
    const char *rn = nt_kind(nt, r) == NK_LocalVariableReadNode ? nt_str(nt, r, "name") : NULL;
    if (!rn || !sp_streq(rn, "__self")) return CN_NONE;
  }
  /* (a builtin method NilClass has of its own answers for nil ahead of a
     top-level def or a Kernel or BasicObject method of the name: nil's
     answer, where neither NilClass nor Object has one of the program's) */
  int pnil = 0;
  int nil_own = builtin_answers_ahead_of("NilClass", nm, "Object") && cplan_nil_program_method(c, id, nm, &pnil) < 0;
  if (!nil_own && cplan_root_method(c, id, core_class_name(CORE_NILCLASS), nm, NULL) >= 0)
    return (rr.kind == RK_PTR || rr.kind == RK_STRBUF) && cplan_nil_runs_other(c, id, rt, nm) &&
           cplan_nil_program_served(c, id)
           ? CN_PROGRAM : CN_NONE;
  /* an arm of a lowered `send` on a user object, for a name its class does
     not define: the arm reaches a Kernel method, which nil answers through
     send as any object does (`nil.send(:Integer, "12")`) */
  if (nt_int(nt, id, "dyn_arm", 0) && ty_is_object(rt)) {
    int k = ty_object_class(rt);
    if (!call_answers(c, id, k, nm, 0, NULL)) return CN_NONE;
  }
  /* the typed String bang emitter tests a receiver it does not write back
     into a variable itself (sp_nil_recv), and marks the plain form it emits
     for it: a second test here would only repeat that one */
  if (view_call_nil_tested(c, id)) return CN_NONE;
  if (rt == TY_STRING && ty_str_typed_bang_flags(nm) && comp_ntype(c, id) != TY_ENUMERATOR &&
      !str_mut_var_recv(c, r) && !sb_shadowed_reader(c, r))
    return CN_NONE;
  return is_nil_method(nm) ? CN_ANSWER : CN_RAISE;
}

/* The version entries (#9898): a call whose version is known only at run
   time (ct_version_at's span of two or more versions) on class K reaches
   K's entry method of the name with the call's parameters (Scope.entry: a
   method of the name that no lookup by name answers, spelled apart in C by
   mc_method),

     def name(<the parameters>)
       if $__spinel_ver_<h> == 1 then <h's version 1>(<args>)
       elsif ...                 # each history on K's chain, nearest first
       else <mi>(<args>)         # no version made yet: the table's answer
       end
     end

   (disp_def_build), each arm's call pinned to its version (ct_pin_of), and
   each version's statement is followed by the write of its history's cell
   (ver_cells_write). The parameters (ver_call_arity): the versions'
   required ones when they all take the same that many and nothing else;
   else the call's positional arguments, where every version takes them,
   so that a call leaving out a defaulted argument runs the version's own
   default as CRuby does, and each version keeps the types its callers
   give it; else `*r, **k`. A block when one takes or yields one. None
   when every version is a copy of the method's own def
   (ver_same_source): the table's answer is each of them. None when a version on the chain is one no arm can call (an
   undef, a version that is no function of its own, a module's version
   an include copies, which runs as the includer's copy of the last one):
   the calls keep the table's answer. With the open histories' facts (q),
   for a call that may meet no version of the chain yet: every version an
   arm, the else arm what the lookup finds with none of them made, where
   the versions are one history's of one visibility (ver_absent_else; the
   last version, as for the other calls, where that keeps the table's
   answer); *absent tells which, and *finds what that else arm finds
   (ver_else_finds). */
/* Are defs a and b one def of the source, a file inlined twice (two
   requires of it the inline load model both keeps)? Its copies are one
   method: any of them answers as the others do. */
static int ver_same_source(const NodeTable *nt, int a, int b) {
  static const char *const at[] = { "node_file", "node_line", "node_col" };
  if (a < 0 || b < 0) return 0;
  for (int i = 0; i < 3; i++)
    if (nt_int(nt, a, at[i], -1) != nt_int(nt, b, at[i], -2)) return 0;
  return 1;
}
static int ver_first_stmt(const NodeTable *nt, int stmts) {
  int n = 0;
  const int *b = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
  return n ? b[0] : -1;
}
/* (in the order of the nodes, for ct_pin_of: an entry's nodes come after
   every earlier entry's, so a pin moves past its own entry's at most) */
static void ver_pin(Compiler *c, int node, int scope, int vis) {
  if (!(c->nver_pins & (c->nver_pins - 1)) &&
      !(c->ver_pins = realloc(c->ver_pins, sizeof *c->ver_pins * (size_t)(c->nver_pins ? 2 * c->nver_pins : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  int i = c->nver_pins++;
  for (; i > 0 && c->ver_pins[i - 1].node > node; i--) c->ver_pins[i] = c->ver_pins[i - 1];
  c->ver_pins[i] = (struct VerPin){ node, scope, vis };
}
/* statement g put in statement list `list` at its statement `at` (after it
   when after) */
static void ver_list_put(NodeTable *nt, int list, int at, int after, int g) {
  int n = 0, j = 0;
  const int *st = nt_arr(nt, list, "body", &n);
  while (j < n && st[j] != at) j++;
  if (j == n) { fprintf(stderr, "spinel: internal: a statement put beside another is not in its statement list\n"); exit(1); }
  int *ns = malloc(sizeof(int) * (size_t)(n + 1));
  if (!ns) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  j += after;
  memcpy(ns, st, sizeof(int) * (size_t)j);
  ns[j] = g;
  memcpy(ns + j + 1, st + j, sizeof(int) * (size_t)(n - j));
  nt_node_set_arr(nt, list, "body", ns, n + 1);
  free(ns);
}
/* How a face entry's arm meets its version's visibility (ver_entry_make):
   raise, the visibility it raises for (VerPin.vis; 0 runs the version);
   or, where only the run tells the version's visibility, cell_face, the
   face for which it tests the version's visibility cell first
   (ver_vis_arm) */
typedef struct VerArmVis { int raise, cell_face, missing; } VerArmVis;
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, const VerArmVis *avis, int na, int els_to, int els, int else_vis);
/* The raise of a call of `name` that finds no method on cls's side sg
   (main for -1), in an entry of ar parameters: NoMethodError, or with vcall
   (a variable-like call's entry, VF_VCALL) NameError, whose message names
   the receiver's class by its Ruby name, read at run time when cls has
   subclasses, is a module or is a Class.new class (which may have no name,
   or take one only at run time) */
static int ver_no_method_raise(Compiler *c, int cls, int sg, const char *name, int ar, int vcall) {
  char msg[512];
  int super = vcall == 2;   /* (a super that reaches no method) */
  const char *what = super ? "super: no superclass method" : vcall ? "undefined local variable or method" : "undefined method";
  vcall = vcall == 1;
  int mod = cls >= 0 && comp_class_is_module(c, &c->classes[cls]), nd = 0;
  comp_sym_intern(c, name);   /* (the error's name, a Symbol made after the literals were interned) */
  if (cls >= 0) comp_descendants(c, cls, &nd);
  if (cls < 0) snprintf(msg, sizeof msg, "%s '%s' for main", what, name);
  else if (nd || (mod && !sg) || comp_class_anonymous(c, cls)) {
    snprintf(msg, sizeof msg, "%s '%s' for %s", what, name, sg ? "class " : "an instance of ");
    return disp_raise_no_method_self(c, name, msg, !sg, ar, vcall);
  }
  else if (sg) snprintf(msg, sizeof msg, "%s '%s' for %s %s", what, name, mod ? "module" : "class", class_ruby_name(c, cls));
  else snprintf(msg, sizeof msg, "%s '%s' for an instance of %s", what, name, class_ruby_name(c, cls));
  return disp_raise_no_method(c, name, msg, ar, vcall);
}
/* What an open history's entry runs when no version has run yet, decided
   for every history before any entry is made (ver_named_kind):
   VK_SUPER the method super reaches; VK_IVAR an inherited reader's
   instance variable; VK_MM method_missing; VK_TEXT Kernel's to_s or
   inspect through super(); VK_BUILTIN the builtin a class answers on itself
   (bop_class_answers, or a record class's own: elab_record_cmethod),
   called on self; VK_NOOP a definition hook's own in the core
   (method_added, inherited and the like: a private no-op, nil after its
   arguments); VK_NOMETHOD NoMethodError; VK_KEEP
   no entry, where none of these is CRuby's answer: the history keeps the
   handling it had (its last version taken as run); a version entry's
   else arm the same, VK_KEEP its last version (ver_entry_make) */
enum { VK_SUPER, VK_IVAR, VK_MM, VK_TEXT, VK_BUILTIN, VK_NOMETHOD, VK_KEEP, VK_CELL, VK_NOOP };
struct VerNamedAsks;
static int ver_named_kind(const struct VerNamedAsks *q, int cls, int sg, const char *name, const int *to, int nv, int hist, int vis, int ar, int blk, int mm, int shared);
static int ver_absent_else(Compiler *c, int els, int rcls, int sg, const char *name, const int *to, int na, int ar, int vcall, int *blk, int *spliced);
static int ver_else_finds(Compiler *c, int els, int cls, int sg, const char *name, int vis, int last);
/* Does cls's chain (its class side when sg) hold a builtin class other
   than Object (a reopened one, a builtin superclass)? Its methods are not
   all in the builtin tables (IO's on File, a socket's), so what a call
   meeting no version of the program's runs there is not known: its entry
   keeps the table's answer. Except a class-side name a public row of the
   builtin-op table gives every class (bop_class_answers; elab asks only the
   readers name, to_s and inspect): the lookup finds Module's text whatever
   the chain holds. A Struct or Data class has an inspect of its own, but it
   adds only the keyword_init suffix to that text, which the class tables
   carry (comp_class_kw_init), and no builtin class above one defines name
   or to_s. Nor the class side of a class a record builder made, or of one
   inheriting from it (elab_record_class_of): its builtin's class methods
   are the ones elab_record_cmethod names, and Class's. */
int builtin_method_arity(const char *cls, const char *m, int *out);
/* Does the builtin class a reopened class stands for (its chain's builtin
   root, cu_class_root) have an instance method `name` taking ar
   arguments, by CRuby's arity table? */
static int ver_builtin_instance_answers(Compiler *c, int cls, const char *name, int ar) {
  const char *root = cls >= 0 ? cu_class_root(c, cls) : NULL;
  int a = 0;
  if (!root || is_object_base_name(root) || !builtin_method_arity(root, name, &a)) return 0;
  return a == ar || (a < 0 && ar >= -a - 1);
}
static int ver_builtin_chain(Compiler *c, int cls, int sg, const char *name, int ar, int blk) {
  if (sg && (bop_class_answers(name, ar, blk) || elab_record_class_of(c, cls) >= 0)) return 0;
  if (cls >= 0 && class_builtin_parent(c, cls) != builtin_class_at(CORE_OBJECT)->id) return 1;
  for (int k = cls, d = 0; k >= 0 && d < 64; k = c->classes[k].parent, d++)
    if (c->classes[k].is_builtin_const && !is_object_base_name(c->classes[k].name)) return 1;
  return 0;
}
/* These implied calls can see precisely whether a Symbol conversion
   callback is installed. The argument callbacks come from the builtin
   row's declaration, including the optional conversion. */
static int ver_symbol_conversion_callback(Compiler *c, int cls, int sg, const char *name, int ar) {
  if (sg || ar || cls < 0 || cls != comp_core_class(c, CORE_SYMBOL)) return 0;
  const BopCallback *callbacks;
  int n = bop_class_callbacks("Kernel", "String", &callbacks);
  for (int i = 0; i < n; i++)
    if ((callbacks[i].roles & BCR_ARG) && sp_streq(callbacks[i].name, name)) return 1;
  return 0;
}
/* The version entry of name on rcls's chain (side sg) for ar arguments:
   an arm per version, nearest history first, each testing its version's
   cell; asked for a call that may meet no version (q), its else arm what
   the lookup finds past every version of the chain (ver_absent_else),
   when they share a visibility. Its arms call class into's scope of each
   version, and it is into's method: rcls's own, or the copies a class
   holds of module rcls's versions, which are the module's instance
   methods also where an extend copies them to into's class side. With a
   face, the arm of a version closed to it raises instead of running it
   (VerPin.vis), and that of a version whose visibility only the run tells
   asks its visibility cell first (ver_vis_arm). An undef's arm raises what
   a call finding no method does (ver_no_method_raise: NameError for a
   variable-like call's face). -1 for none. */
/* Is version i of a chain's versions (elab_chain_versions: nearest history
   first) always behind another: a def of a nearer history, at a point that
   runs for sure and before version i's, in a history nothing removes or
   undefines a method from? */
static int ver_always_shadowed(Compiler *c, const ElabVersion *vs, int nv, int i) {
  LoadPoint pi = elab_point_of(c, vs[i].v->node);
  if (pi.region < 0) return 0;
  for (int j = 0; j < i; j++) {
    if (vs[j].hist == vs[i].hist || (vs[j].v->kind != LR_DEF && vs[j].v->kind != LR_COPY) || elab_point_may(c, vs[j].v->node)) continue;
    LoadPoint pj = elab_point_of(c, vs[j].v->node);
    if (pj.region < 0 || elab_cmp(pj, pi) != PROG_BEFORE) continue;
    int removed = 0;
    for (int k = 0; k < nv && !removed; k++)
      removed = vs[k].hist == vs[j].hist && (vs[k].v->kind == LR_REMOVE || vs[k].v->kind == LR_UNDEF);
    if (!removed) return 1;
  }
  return 0;
}
/* ...or a module's version that into holds no copy of because no call on
   into's chain meets it (the include copies a module's method behind the
   class's own def only where a call or a super reaches it there,
   mixin_reached_behind_own: elab_version_met is the record's answer)? */
static int ver_copy_unmet(Compiler *c, const ElabVersion *v, int into, int sg, const char *name) {
  int sc = v->v->scope, mod = sc > 0 && sc < c->nscopes ? c->scopes[sc].class_id : -1;
  return into >= 0 && mod >= 0 && mod != into && comp_class_is_module(c, &c->classes[mod]) &&
         !elab_version_met(c, into, sg, name, v->v->node);
}
static int ver_entry_make(Compiler *c, const struct VerNamedAsks *q, int rcls, int into, int sg, const char *name, int mi, int ar, int mm,
                          int face, int *absent, int *finds, int *anyform) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  VerArmVis avis[VER_MAX_ARMS];
  int to[VER_MAX_ARMS], dto[VER_MAX_ARMS], na = 0, nd = 0, blk = c->scopes[mi].blk_param || c->scopes[mi].yields, dc, same = 1, vis = 1, els = -1, fallback = mi, kind = VK_KEEP;
  int spliced = c->scopes[mi].yields;
  char sel[VER_MAX_ARMS][32];
  int nv = elab_chain_versions(c, rcls, rcls != into ? 0 : sg, name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE) continue;   /* its write passes the search on */
    ver_cell_name(vs[i].cell, sel[na]);
    /* (an undef's arm: its raise, made once the entry is known to be made) */
    if (vs[i].v->kind == LR_UNDEF) {
      arm[na] = (DispArm){ sel[na], vs[i].n, name, vs[i].cell + 1, -1 };
      avis[na] = (VerArmVis){ 0, 0 };
      to[na++] = -1;
      same = 0;
      continue;
    }
    int s = ct_def_version(c, into, name, sg, vs[i].v, &dc);
    /* (main's chain ends in Object's: its versions are Object's methods,
       which an arm's call on main reaches with main as self, ct_lookup) */
    int obj = s < 0 && into < 0 && sg;
    if (obj) s = ct_def_version(c, comp_core_class(c, CORE_OBJECT), name, 0, vs[i].v, &dc);
    /* (a version no call on this chain meets is no arm: one of a history
       further along, made after a def of a nearer one that always runs and
       that nothing removes, which answers ahead of it from then on. A
       top-level def is such a version of every class's singleton chain,
       which ends in Object's methods. So is a module's version the class's
       own def stands in front of where nothing reaches it behind that def.) */
    if (s < 0 || c->scopes[s].is_transplanted_source || (!c->scopes[s].replaced && scope_is_shadowed(c, s))) {
      if (ver_always_shadowed(c, vs, nv, i) || (s < 0 && ver_copy_unmet(c, &vs[i], into, sg, name))) continue;
      return -1;
    }
    vs[nd] = vs[i];   /* (the defs, for the else arm's facts) */
    dto[nd++] = s;
    blk |= c->scopes[s].blk_param || c->scopes[s].yields;
    spliced &= c->scopes[s].yields;
    vis &= vs[i].v->vis == vs[0].v->vis;
    arm[na] = (DispArm){ sel[na], vs[i].n, name, vs[i].cell + 1, -obj };   /* (-1: an Object version's `self.name` call, made below) */
    /* (a face no visibility closes takes no visibility arm: ver_face_closes) */
    int fv = ver_face_closes(face) ? elab_version_vis(c, &vs[i]) : SP_VIS_PUBLIC;
    avis[na] = (VerArmVis){ fv >= 0 && ver_closed_to(fv, face) ? fv : 0, fv < 0 ? face : 0 };
    to[na++] = s;
    if (nt_int(c->nt, c->scopes[s].def_node, "concat_builtin_version", 0)) fallback = s;
    same &= ver_same_source(c->nt, c->scopes[s].def_node, c->scopes[mi].def_node);
  }
  /* (an undef on the chain has run no more than the defs where no version has: the else arm is what the lookup finds
     past all of them, as without one) */
  if (q && nd && vis && !ver_builtin_chain(c, rcls, sg, name, ar, blk)) kind = ver_named_kind(q, rcls, sg, name, dto, nd, vs[nd - 1].hist, vs[0].v->vis, ar, blk, mm, 0);
  /* (on a reopened builtin's instance side, a method the builtin itself
     has: with no version run, the builtin's own, #11819) */
  else if (q && nd && ar >= 0 && ct_versioned_string_reader(c, rcls, sg, name)) kind = VK_BUILTIN;
  else if (q && nd && vis && !sg && ver_builtin_instance_answers(c, rcls, name, ar)) kind = VK_BUILTIN;
  /* An optional Symbol conversion is absent until its first override.
     Model the absence even if the call sites only name Kernel.String. */
  else if (q && nd && vis && !(mm & 1) && ver_symbol_conversion_callback(c, rcls, sg, name, ar))
    kind = VK_NOMETHOD;
  if (kind == VK_NOOP && ver_closed_to(SP_VIS_PRIVATE, face)) kind = VK_NOMETHOD;   /* (the core's hook is private) */
  /* (an else arm that runs a method or the core's no-op raises nothing of
     its own, whatever the call's form or its method_missing: asked with a
     variable-like call's face, the entry of no face, -2, ver_late_make) */
  int forms = kind == VK_SUPER || kind == VK_IVAR || kind == VK_TEXT || kind == VK_BUILTIN || kind == VK_NOOP;
  if (anyform) *anyform = forms;
  if (q && face == VF_VCALL && forms) return -2;
  if (kind != VK_KEEP) els = ver_absent_else(c, kind, rcls, sg, name, dto, nd, ar, face == VF_VCALL, &blk, &spliced);
  if (absent) *absent = els >= 0;
  *finds = ver_else_finds(c, kind, rcls, sg, name, nd > 0 ? vs[0].v->vis : SP_VIS_PUBLIC, nd ? dto[nd - 1] : -1);
  int cellv = 0;   /* (a face arm that reads a visibility cell: an entry for one version too) */
  for (int k = 0; k < na; k++) cellv |= face && avis[k].cell_face;
  if (els < 0 && (q || (nv < 2 && !cellv) || !nd || (same && !face))) return -1;   /* (asked for its else arm only: ver_late_make stores no scope for an entry without one) */
  for (int k = 0; k < na; k++)
    if (to[k] < 0) {
      arm[k].stmt = 1 + ver_no_method_raise(c, into, sg, name, ar, face == VF_VCALL);
      nt_node_set_int((NodeTable *)c->nt, arm[k].stmt - 1, "undef_arm", 1);   /* (cplan_method_arms) */
    }
    else if (arm[k].stmt < 0) {
      int call = disp_self_call((NodeTable *)c->nt, name, ar, blk);
      nt_set_ignore_visibility((NodeTable *)c->nt, call, 1);
      arm[k].stmt = 1 + call;
    }
  return ver_entry_emit(c, into, sg, name, ar, blk, spliced, arm, to, face ? avis : NULL, na, fallback, els, 0);
}
/* A face entry's arm call `call` of a version whose visibility only the
   run tells (VerArmVis.cell_face): `if $__spinel_vis_<cell> == private then <call>
   [elsif ... == protected then <call>, for a face closed to both] else
   call end`, its calls in calls[] with the visibility each one's pin
   raises for (VerPin.vis; 0, call itself, runs the version). How many. */
static int ver_vis_arm(NodeTable *nt, int cell, int face, int call, const char *callee, int ar, int blk, int missing, int *node, int calls[3], int vis[3]) {
  char sel[32];
  ver_vis_cell_name(cell, sel);
  int n = 0, run = call;
  int closed = missing > 0 ? nt_clone_subtree(nt, missing - 1) : disp_forward_call(nt, callee, ar, blk);
  if (!missing) calls[n] = closed, vis[n++] = SP_VIS_PRIVATE;
  if (face == VF_PUBLIC) {
    int protected = nt_clone_subtree(nt, closed);
    if (!missing) calls[n] = protected, vis[n++] = SP_VIS_PROTECTED;
    run = disp_cell_test(nt, sel, SP_VIS_PROTECTED, protected, call);
  }
  calls[n] = call, vis[n++] = 0;
  *node = disp_cell_test(nt, sel, SP_VIS_PRIVATE, closed, run);
  return n;
}
/* The entry's def, its arms' calls pinned to to[] and its else arm's to
   els_to, beside els_to's def; or, given els, that else arm, pinned to
   nothing */
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, const VerArmVis *avis, int na, int els_to, int els, int else_vis) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pin = els < 0, like = c->scopes[els_to].def_node, ecell = -1;
  VerArmVis ev = { else_vis, 0 };
  int vc[VER_MAX_ARMS + 1][3], vv[VER_MAX_ARMS + 1][3], vn[VER_MAX_ARMS + 1] = { 0 };
  /* (the else arm raises as the last arm of its version does: avis, given
     for a face entry, says which arms raise, and with what) */
  for (int k = 0; avis && k < na; k++) if (to[k] == els_to) ev = avis[k], ecell = arm[k].hist - 1;
  /* The shared arms end in one fallback, not tests that repeat it. */
  while (pin && na > 0 && to[na - 1] == els_to && (!avis || (avis[na - 1].raise == ev.raise && avis[na - 1].cell_face == ev.cell_face))) na--;
  if (pin) els = disp_forward_call(nt, c->scopes[els_to].name, ar, blk);
  if (pin && ev.cell_face) vn[na] = ver_vis_arm(nt, ecell, ev.cell_face, els, c->scopes[els_to].name, ar, blk, ev.missing, &els, vc[na], vv[na]);
  int def = disp_def_build(nt, like, name, sg, ar, blk, arm, na, els);
  int body = nt_ref(nt, def, "body"), e = c->nscopes;
  for (int k = 0, at = ver_first_stmt(nt, body); avis && k < na; k++, at = nt_ref(nt, at, "subsequent")) {
    int st = nt_ref(nt, at, "statements"), node = -1;
    if (!avis[k].cell_face) continue;
    vn[k] = ver_vis_arm(nt, arm[k].hist - 1, avis[k].cell_face, ver_first_stmt(nt, st), arm[k].callee, ar, blk, avis[k].missing, &node, vc[k], vv[k]);
    nt_node_set_arr(nt, st, "body", &node, 1);
  }
  /* walked as the def it is, beside that def: its scope, its receiver and
     parameters in it */
  comp_grow_node_arrays(c);
  walk_scope(c, def, c->nscope[like], rcls < 0 ? -1 : rcls);
  c->scopes[e].entry = ENTRY_DISPATCH;
  /* the entry only forwards its block: a call that gives it one splices
     it, its arms spliced in turn, when every method an arm calls is
     spliced (an_phase_block_inline's rule for a forwarder) */
  c->scopes[e].yields = spliced;
  comp_touch_scope(c->scopes[e].idx);
  for (int k = 0, at = ver_first_stmt(nt, body); k < na; k++, at = nt_ref(nt, at, "subsequent")) {
    if (to[k] < 0) continue;   /* (an undef's arm raises: no version to pin) */
    if (avis && avis[k].cell_face)
      for (int j = 0; j < vn[k]; j++) ver_pin(c, vc[k][j], to[k], vv[k][j]);
    else ver_pin(c, ver_first_stmt(nt, nt_ref(nt, at, "statements")), to[k], avis ? avis[k].raise : 0);
  }
  if (pin && ev.cell_face)
    for (int j = 0; j < vn[na]; j++) ver_pin(c, vc[na][j], els_to, vv[na][j]);
  else if (pin) ver_pin(c, els, els_to, ev.raise);
  return e;
}
/* The copies that are the versions of one module method a mixin copies
   (analyze_scope.c, mixin_prev_version): those of mi's class and side
   under mi's name, with its cell and the method it stands in front of
   where a later include or prepend guards them, each with the record's
   version it copies on the chain of class `from` (vs[k], whose history's
   cell tells it): mi's class's own, or the module's, whose instance
   methods an extend's class-side copies copy. In the chain's order.
   How many, 0 where one is not on the chain, and where mi stands alone
   unless one (a position that may meet none of them live: pos_versions). */
/* The scopes of one (class, side, name), for mixin_copy_versions's walk: a
   hashed index over the scopes, in chains, held while the scope index is
   frozen (every scope's class, side and name then hold, and scopes are only
   appended: comp_scope_index_gen) and rebuilt when the count or that
   changes. The walk over every scope it replaces, once per ask, was a
   quarter of an application's compile. Unfrozen, the walk scans as before. */
static int *mcv_head, *mcv_next, mcv_nb, mcv_n = -1;
static unsigned mcv_gen;
static unsigned mcv_hash(int class_id, const char *name, int is_cm) {
  unsigned h = 2166136261u;
  for (const char *p = name; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= (unsigned)class_id * 2654435761u;
  return h ^ (is_cm ? 0x9e3779b9u : 0);
}
/* the first scope of the chain that holds (class_id, is_cm, name)'s scopes,
   which the caller filters (a chain is shared by whatever hashes alike), or
   -2 for no index: the scan */
static int mcv_first(Compiler *c, int class_id, const char *name, int is_cm) {
  if (!comp_scope_index_is_frozen() || !name) return -2;
  if (mcv_n != c->nscopes || mcv_gen != comp_scope_index_gen() || !mcv_nb) {
    free(mcv_head); free(mcv_next);
    mcv_nb = c->nscopes > 0 ? c->nscopes : 1;
    mcv_head = malloc(sizeof(int) * (size_t)mcv_nb);
    mcv_next = malloc(sizeof(int) * (size_t)mcv_nb);
    if (!mcv_head || !mcv_next) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int b = 0; b < mcv_nb; b++) mcv_head[b] = -1;
    for (int s = 0; s < c->nscopes; s++) {
      const Scope *k = &c->scopes[s];
      if (!k->name) { mcv_next[s] = -1; continue; }
      unsigned b = mcv_hash(k->class_id, k->name, k->is_cmethod) % (unsigned)mcv_nb;
      mcv_next[s] = mcv_head[b]; mcv_head[b] = s;
    }
    mcv_n = c->nscopes; mcv_gen = comp_scope_index_gen();
  }
  return mcv_head[mcv_hash(class_id, name, is_cm) % (unsigned)mcv_nb];
}
static int mixin_copy_versions(Compiler *c, int mi, int from, int *to, ElabVersion *vs, int one) {
  const Scope *m = &c->scopes[mi];
  ElabVersion cv[VER_MAX_ARMS];
  int side = m->is_extend_copy && from == m->origin_module_ci - 1 ? 0 : m->is_cmethod;
  int n = 0, ns = 0, nv = elab_chain_versions(c, from, side, comp_scope_user_name(m), cv, VER_MAX_ARMS), at[VER_MAX_ARMS];
  for (int i = 0; i < nv; i++) at[i] = -1;
  /* (one walk: the copies under mi's name, and each version's first one;
     the chain holds them in descending order, so the first is the lowest) */
  int first = mcv_first(c, m->class_id, m->name, m->is_cmethod);
  for (int s = first == -2 ? 0 : first; s >= 0 && s < c->nscopes; s = first == -2 ? s + 1 : mcv_next[s]) {
    const Scope *k = &c->scopes[s];
    if (k->class_id != m->class_id || k->is_cmethod != m->is_cmethod || k->mixin_guarded != m->mixin_guarded ||
        k->guard_behind != m->guard_behind || k->mixin_guard_value != m->mixin_guard_value ||
        !k->name || !sp_streq(k->name, m->name)) continue;
    ns += !k->entry;
    for (int i = 0; i < nv; i++)
      if ((at[i] < 0 || s < at[i]) && cv[i].v->kind == LR_DEF && k->def_node == cv[i].v->node) at[i] = s;
  }
  for (int i = 0; i < nv; i++) {
    /* (a module included again lists its versions again: one copy each) */
    int dup = at[i] < 0;
    for (int j = 0; j < n && !dup; j++) dup = to[j] == at[i];
    if (dup) continue;
    vs[n] = cv[i];
    to[n++] = at[i];
  }
  return ns > !one && n == ns ? n : 0;
}
/* The module whose versions mi, the table's answer in its class, is one of
   the unguarded copies of (mixin_copy_versions, asked of the module's
   chain), with the copies and versions; -1 for none, and for the plain
   copies of a mixin that may not run or runs at a point no guard sees
   (Scope.mixin_unversioned), where no cell tells whether they are there
   yet and the module's later versions are layers of their own */
static int mixin_copies_module(Compiler *c, int mi, int *to, ElabVersion *vs, int *n) {
  const Scope *m = &c->scopes[mi];
  if (!m->versioned || !(m->is_include_copy || m->is_extend_copy) || m->mixin_guarded || m->mixin_unversioned || m->origin_module_ci <= 0) return -1;
  *n = mixin_copy_versions(c, mi, m->origin_module_ci - 1, to, vs, 0);
  return *n ? m->origin_module_ci - 1 : -1;
}
static int layer_ran_before(Compiler *c, const Scope *s, const Scope *t);
static void guard_memo_drop(struct GuardMemo *g) {
  free(g->at); free(g->to);
  *g = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
}
/* The memos that hold a layer's runs, versions and next position, and the pairs layer_ran_before answered, are those
   of one generation of the scope index (comp_scope_index_gen) and of the scopes made so far: a scope renamed, or a
   copy made, drops them, as it drops the walk's index (mcv_first). */
/* (never filled while a scope is moved for an instance_exec block: its class is not the one the memo is keyed on) */
static int pos_memo_fill(void) { return !comp_scope_moving(); }
static void pos_memos_sync(Compiler *c) {
  /* (what the memos' callees read: the scope index's freeze, the scopes' names, classes and entry kinds, the classes'
     shapes and mixin records, the tables, the class count, and the copies made) */
  struct PosKey k = { comp_scope_index_gen(), c->scope_edit_gen, comp_class_shape_gen(), c->mixin_gen, comp_table_gen, c->nclasses };
  int keep = memcmp(&k, &c->posv_key, sizeof k) == 0 && c->posv_ns <= c->nscopes;
  /* (the entries the build makes are scopes too, and no copy: they leave every answer as it was) */
  for (int s = c->posv_ns; keep && s < c->nscopes; s++) keep = c->scopes[s].entry != ENTRY_NONE;
  if (!keep || comp_scope_moving()) {
    guard_memo_drop(&c->posv_memo); guard_memo_drop(&c->pnext_memo); guard_memo_drop(&c->pos_memo);
    free(c->lrb_memo.key); free(c->lrb_memo.val);
    memset(&c->lrb_memo, 0, sizeof c->lrb_memo);
    c->posv_key = k;
  }
  c->posv_ns = c->nscopes;
}
/* layer_ran_before(s, t), asked of every position at every super and every call that reaches one, and each ask
   searches the class's mixin statements: kept for the pair (an open-addressing table of pairs, Compiler.lrb_memo) */
static int pos_ran_before(Compiler *c, const Scope *s, const Scope *t) {
  struct LrbMemo *g = &c->lrb_memo;
  if (!pos_memo_fill()) return layer_ran_before(c, s, t);
  unsigned long long key = ((unsigned long long)(unsigned)(s - c->scopes) << 32) | (unsigned)(t - c->scopes);
  unsigned i = g->cap ? (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(g->cap - 1) : 0;
  for (; g->cap && g->val[i]; i = (i + 1) & (unsigned)(g->cap - 1))
    if (g->key[i] == key) return g->val[i] > 1;
  int r = layer_ran_before(c, s, t);
  if (2 * (g->n + 1) > g->cap) {   /* (grown: every pair moves) */
    int ocap = g->cap, ncap = ocap ? 2 * ocap : 64;
    unsigned long long *ok = g->key;
    unsigned char *ov = g->val;
    g->key = calloc((size_t)ncap, sizeof *g->key);
    g->val = calloc((size_t)ncap, 1);
    if (!g->key || !g->val) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    g->cap = ncap;
    for (int k = 0; k < ocap; k++) {
      if (!ov[k]) continue;
      unsigned h = (unsigned)((ok[k] * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(ncap - 1);
      while (g->val[h]) h = (h + 1) & (unsigned)(ncap - 1);
      g->key[h] = ok[k]; g->val[h] = ov[k];
    }
    free(ok); free(ov);
    i = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 40) & (unsigned)(ncap - 1);
    while (g->val[i]) i = (i + 1) & (unsigned)(ncap - 1);
  }
  g->key[i] = key;
  g->val[i] = (unsigned char)(r + 1);
  g->n++;
  return r;
}
/* The versions a position t, a mixin's copy in a class's lookup chain, stands
   for, into to and vs (mixin_copy_versions: those of its class's chain where
   a later mixin guards the copy, its module's otherwise), nearest first:
   how many, 0 where t is no layer of its own, one that tests nothing: a
   single version that exists wherever t is reached, or a copy the entries
   keep out of it (a mixin that may not run: Scope.mixin_unversioned).
   Several versions are a layer for a guarded copy and for an include's or
   an extend's hidden copy; one that a call may meet none of live (a method
   body's version: elab_versions_late, *late) is for every copy. For the
   super of s (NULL: none, the arm of a guard entry or the method behind its
   arms), a guarded copy below s's that always ran before s's mixin did
   (layer_ran_before): its cell is set wherever s runs, and only its
   module's version tells which of its copies runs. */
static int layer_ran_before(Compiler *c, const Scope *s, const Scope *t);
static int pos_versions(Compiler *c, const Scope *s, int t, int *to, ElabVersion *vs, int *late) {
  const Scope *m = &c->scopes[t];
  int from, many;
  *late = 0;
  pos_memos_sync(c);
  if (m->mixin_guarded) {
    if (s && (m->entry || !m->shadow || !pos_ran_before(c, s, m))) return 0;
    from = m->class_id;
    many = 1;
  }
  else if ((m->is_include_copy || m->is_extend_copy || m->is_prepend_copy) && !m->entry && !m->mixin_unversioned && m->origin_module_ci > 0) {
    from = m->origin_module_ci - 1;
    many = m->shadow && (m->is_include_copy || m->is_extend_copy);
  }
  else return 0;
  /* (kept for the layer, as the walk over its class's chain costs a version for each layer that asks it: Compiler.posv_memo,
     dropped by pos_memos_sync. vs is read only by the entry's make) */
  int n, key[VER_MAX_ARMS + 1], raw = 0;
  if (!vs) raw = guard_memo_get(&c->posv_memo, t, key);
  if (raw) {
    n = raw - 1;
    *late = key[0];
    memcpy(to, key + 1, sizeof *to * (size_t)n);
  }
  else {
    ElabVersion lv[VER_MAX_ARMS];
    ElabVersion *v = vs ? vs : lv;
    n = mixin_copy_versions(c, t, from, to, v, 1);
    *late = n && elab_versions_late(c, comp_scope_user_name(m), v, n) == 2;
    if (!vs && pos_memo_fill()) {
      key[0] = *late;
      memcpy(key + 1, to, sizeof *to * (size_t)n);
      guard_memo_put(&c->posv_memo, t, key, n + 1);
    }
  }
  return *late || (many && n > 1) ? n : 0;
}
/* Is mi, the method a class's table answers, a late layer (pos_versions) in front of its chain: a prepend's copy of
   a module's version that a call may meet none of live? (An include's or an extend's, which stands in front of no
   method of the class's own, is answered by its module's open history: ver_named_asks.) */
static int pos_front(Compiler *c, int mi) {
  const Scope *m = &c->scopes[mi];
  int to[VER_MAX_ARMS], late;
  if (m->mixin_guarded || m->shadow || m->entry || !m->is_prepend_copy) return 0;
  return pos_versions(c, NULL, mi, to, NULL, &late) && late;
}
/* The position behind t in its class's lookup, as the chain holds it: the
   copy a later prepend's guard stands in front of, else the method a super
   in t reaches; -1 for none */
static int pos_next(Compiler *c, int t) {
  const Scope *m = &c->scopes[t];
  int next[1];
  if (m->mixin_guarded && m->guard_behind >= 0) return m->guard_behind;
  pos_memos_sync(c);
  /* (the super's target reads every scope of the name: kept for the position, as the layers behind it ask it for each) */
  if (guard_memo_get(&c->pnext_memo, t, next) > 0) return next[0] - 1;
  next[0] = cplan_super_target(c, -1, m, NULL) + 1;
  if (pos_memo_fill()) guard_memo_put(&c->pnext_memo, t, next, 1);
  return next[0] - 1;
}
/* The scopes a lookup that reaches position t (from s's super: pos_versions)
   may run: t's versions where it is a layer, and where a call may meet none
   of them live, what is behind it in turn; t alone where it is no layer.
   How many, into to (at most VER_MAX_ARMS); *layer: whether t is a layer. */
static int pos_runs(Compiler *c, const Scope *s, int t, int *to, int *layer) {
  int n = 0, late = 1;
  if (layer) *layer = 0;
  for (; late && t >= 0 && n < VER_MAX_ARMS; s = &c->scopes[t], t = pos_next(c, t)) {
    int vt[VER_MAX_ARMS], nv = pos_versions(c, s, t, vt, NULL, &late);
    if (layer && !n) *layer = nv > 0;
    if (!nv) vt[nv++] = t;
    for (int k = 0; k < nv && n < VER_MAX_ARMS; k++) to[n++] = vt[k];
  }
  return n;
}
/* ...for mi, a late layer the class's table answers (pos_front), kept for the layer (Compiler.pos_memo): each call of its
   name asks it, and each answer reads every scope of the name (cplan_super_target). Dropped when the entries are made,
   whose scopes the answer then names. */
static int pos_runs_front(Compiler *c, int mi, int *to) {
  pos_memos_sync(c);
  int n = guard_memo_get(&c->pos_memo, mi, to);
  if (n) return n;
  n = pos_runs(c, NULL, mi, to, NULL);
  if (n && pos_memo_fill()) guard_memo_put(&c->pos_memo, mi, to, n);
  return n;
}
/* The entry made for position t with ar parameters (kept in ver_entries as
   VER_ENTRY_LAYER from t + 1), its scope; -1 for none. The memo of the
   build, whose index is made after it. */
/* Does the lookup past the entry e (a late layer's) end at the receiver's method_missing or a NoMethodError (pos_entry)? */
static int pos_end_arm(const Compiler *c, int e) {
  for (int i = 0; i < c->nver_entries; i++)
    if (c->ver_entries[i].guard == VER_ENTRY_LAYER && c->ver_entries[i].scope == e) return c->ver_entries[i].end_arm;
  return 0;
}
static int pos_made(const Compiler *c, int t, int ar, int face) {
  for (int i = 0; i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == VER_ENTRY_LAYER && e->from == t + 1 && e->ar == ar && e->face == face) return e->scope;
  }
  return -1;
}
static void ver_entry_add(Compiler *c, const char *name, struct VerEntry e);
static int guard_behind_finds_none(Compiler *c, int behind, int cls, int sg, const char *name);
static int guard_entry_make(Compiler *c, int mi, int ar, int face, int *behind);
static int guard_else_bare(Compiler *c, int behind, int cls, int sg, const char *name, int *hooks);
/* The guard entry of the class behind a late layer (kept in ver_entries as VER_ENTRY_GUARD, as the entries' build
   keeps it), where a later prepend guards that class's method g: a lookup that passes the layer finds the guard's
   arms first, not the copy g stands in front of. The memo of the build. */
static int pos_guard_made(const Compiler *c, int g, int ar, int face) {
  const Scope *m = &c->scopes[g];
  const char *name = comp_scope_user_name(m);
  for (int i = 0; i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == VER_ENTRY_GUARD && !e->from && e->cls == m->class_id && e->sg == m->is_cmethod && e->ar == ar && e->face == face && sp_streq(e->name, name)) return e->scope;
  }
  return -1;
}
static int pos_guard(Compiler *c, int g, int ar, int face) {
  int e = pos_guard_made(c, g, ar, face), behind = -1, hooks = -1, to[VER_MAX_ARMS + 1];
  if (e >= 0) return e;
  e = guard_entry_make(c, g, ar, face, &behind);
  if (e < 0) return g;
  const Scope *m = &c->scopes[g];
  int n = guard_chain(c, g, to, NULL, NULL);
  int finds = n && guard_else_bare(c, to[n - 1], m->class_id, m->is_cmethod, comp_scope_user_name(m), &hooks) ? VER_FINDS_NONE
            : ver_else_finds(c, VK_KEEP, m->class_id, m->is_cmethod, comp_scope_user_name(m), SP_VIS_PUBLIC, -1);
  ver_entry_add(c, c->scopes[e].name, (struct VerEntry){ NULL, VER_ENTRY_GUARD, m->class_id, m->is_cmethod, ar, e, .finds = finds, .behind = behind + 1, .face = face });
  return e;
}
/* The entry over the versions of position t (pos_versions), a layer of a
   lookup: an if-chain over the cells of their histories, nearest first,
   whose arm for each version calls its copy. Where a call may meet none of
   them live (a method body's version), its else arm runs what the lookup
   finds past t, as CRuby's lookup passes a module with no method of the
   name yet: the position behind t (pos_next), its entry where that is a
   layer too. Each position's entry is made once and kept, whatever asks
   it: the guard entry's arm for t, the method behind a guard's arms, a
   super that reaches t, a call that takes t as the class's first. t itself
   where it is no layer. With a face (a call that enforces visibility, where
   a version's visibility only the run tells: ct_layer_face), the arm of a
   version closed to it raises, and that of a version whose visibility the
   run tells asks its visibility cell first (ver_vis_arm), as the chain's
   face entry does (ver_entry_make); its else arm is the face's entry of
   what lies behind. */
static int pos_entry(Compiler *c, const Scope *s, int t, int ar, int face) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  VerArmVis avis[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int to[VER_MAX_ARMS], late, nv = pos_versions(c, s, t, to, NULL, &late), blk = 0, spliced = 1;
  if (!nv) return t;
  int made = pos_made(c, t, ar, face);
  if (made >= 0) return made;
  nv = pos_versions(c, s, t, to, vs, &late);
  for (int k = 0; k < nv; k++) {
    blk |= c->scopes[to[k]].blk_param || c->scopes[to[k]].yields;
    spliced &= c->scopes[to[k]].yields;
    ver_cell_name(vs[k].cell, sel[k]);
    arm[k] = (DispArm){ sel[k], vs[k].n, c->scopes[to[k]].name, vs[k].cell + 1 };
    int fv = ver_face_closes(face) ? elab_version_vis(c, &vs[k]) : SP_VIS_PUBLIC;
    avis[k] = (VerArmVis){ fv >= 0 && ver_closed_to(fv, face) ? fv : 0, fv < 0 ? face : 0 };
  }
  int past = late ? pos_next(c, t) : -1, els_to = to[nv - 1], els = -1, kind = VK_KEEP;
  int cls = c->scopes[t].class_id, sg = c->scopes[t].is_cmethod;
  const char *name = comp_scope_user_name(&c->scopes[t]);
  if (past >= 0) {
    els_to = c->scopes[past].mixin_guarded && c->scopes[past].class_id != cls ? pos_guard(c, past, ar, face) : pos_entry(c, NULL, past, ar, face);
    blk |= c->scopes[els_to].blk_param || c->scopes[els_to].yields;
    spliced &= c->scopes[els_to].yields;
  }
  /* (the end of the lookup: no method behind t, or the hidden method a mixin stands in front of where the class had
     none of its own, which finds none: CRuby calls the receiver's method_missing, or raises where it has none,
     guard_behind_finds_none) */
  int end_here = past >= 0 ? els_to == past && guard_behind_finds_none(c, past, cls, sg, name) : late && !cplan_other_answers(c, cls, sg, name, 1);
  int end_arm = end_here || (past >= 0 && pos_end_arm(c, els_to));   /* (the lookup past t ends so) */
  if (end_here) {
    kind = cplan_mm_method(c, -1, cls, sg) >= 0 ? VK_MM : VK_NOMETHOD;
    els = ver_absent_else(c, kind, cls, sg, name, past >= 0 ? &past : to, past >= 0 ? 1 : nv, ar, 0, &blk, &spliced);
  }
  if (end_arm) {
    /* (where the class has none and a subclass defines its own, a receiver of that class, or of one below it, calls it,
       and any other raises: the arm's receiver is tested by its class, innermost last. A subclass's method_missing that
       a method body defines, which is there only once that body has run, or that the call hands a block to, which a
       method_missing only subclasses define cannot take, is no answer the record can tell: its receivers run the
       version of the position they reach, at every position of the layers, as before the else arm answered them (R's
       form, which a layer's body's super reaches position by position: the version of t, its last, pinned) */
    int nd = 0;
    const int *ds = !sg && cplan_mm_method(c, -1, cls, sg) < 0 ? comp_descendants(c, cls, &nd) : NULL;
    for (int i = nd - 1; i >= 0; i--) {
      int own = c->classes[ds[i]].unreferenced ? -1 : cplan_mm_method(c, -1, ds[i], 0);
      int test = own >= 0 ? disp_self_class_is(c, ds[i]) : -1;
      if (test < 0) continue;
      /* (a method_missing that takes a block parameter has no function of its own where the block is only called: no
         dispatch arm, and nothing to call, so it is no answer either) */
      int rform = blk || elab_body_def(c, c->scopes[own].def_node) >= 0 || (c->scopes[own].blk_param && c->scopes[own].blk_param[0]), call;
      if (!rform && !end_here) continue;
      if (els < 0) {   /* (no else arm yet: the position behind, as the entry pins it) */
        els = disp_forward_call((NodeTable *)c->nt, c->scopes[els_to].name, ar, blk);
        ver_pin(c, els, els_to, 0);
      }
      if (rform) {
        call = disp_forward_call((NodeTable *)c->nt, c->scopes[to[nv - 1]].name, ar, blk);
        ver_pin(c, call, to[nv - 1], 0);
      }
      else {
        comp_sym_intern(c, name);
        call = disp_method_missing_call((NodeTable *)c->nt, name, ar, blk);
      }
      els = disp_choose((NodeTable *)c->nt, test, call, els);
    }
  }
  int e = ver_entry_emit(c, cls, sg, name, ar, blk, spliced, arm, to, ver_face_closes(face) ? avis : NULL, nv, els_to, els, 0);
  ver_entry_add(c, name, (struct VerEntry){ NULL, VER_ENTRY_LAYER, cls, sg, ar, e, .finds = ver_else_finds(c, kind, cls, sg, name, SP_VIS_PUBLIC, -1), .from = t + 1, .end_arm = end_arm, .face = face });
  return e;
}
/* The methods a super in s may run, for the readers that ask before the
   entries are made, or for no call (the yields and block marking, a
   super's lifted block): the target cplan_super_target answers, or, where
   that is a layer (pos_runs), every one the entry over it may run. How many,
   0 for none. */
int cplan_super_runs(Compiler *c, Scope *s, int *to) {
  int layer = 0, t = cplan_super_target(c, -1, s, NULL), n = t >= 0 ? pos_runs(c, s, t, to, &layer) : 0;
  if (n && !layer && c->scopes[t].mixin_conditional) {
    int tail[VER_MAX_ARMS + 1];
    n = guard_chain(c, t, tail, NULL, NULL);
    if (n > VER_MAX_ARMS) n = VER_MAX_ARMS;
    memcpy(to, tail, (size_t)n * sizeof *to);
  }
  return n;
}
/* What a Method's super_method names for the method of scope mi: its super's target, the entry over it where that is a
   layer (the one a super of its program made, any arity: a Method's arms tell the versions apart, not the call's
   arguments), else the target itself; -1 for none */
int cplan_super_method_target(Compiler *c, int mi) {
  ElabVersion vs[VER_MAX_ARMS];
  int to[VER_MAX_ARMS], late, from = mi, t;
  /* (a layer's entry stands for the position it is made of: the method behind that is its super's) */
  for (int i = 0; i < c->nver_entries; i++)
    if (c->ver_entries[i].guard == VER_ENTRY_LAYER && c->ver_entries[i].scope == mi) from = c->ver_entries[i].from - 1;
  t = from != mi ? pos_next(c, from) : cplan_super_target(c, -1, &c->scopes[mi], NULL);
  if (t < 0 || !c->ver_built || !pos_versions(c, comp_super_site(c, &c->scopes[from]), t, to, vs, &late)) return t;
  for (int i = 0; i < c->nver_entries; i++)
    if (c->ver_entries[i].guard == VER_ENTRY_LAYER && c->ver_entries[i].from == t + 1 && c->ver_entries[i].face == VF_NONE) return c->ver_entries[i].scope;
  return t;
}
/* The Methods a super_method of the Method of scope mi holds. Of the Method of an entry (a guard's or a late layer's),
   which the run picked an arm of, the arms of the entry (cplan_method_arms) in the order a lookup takes them, one
   scope for each version, each under the test of its cells: its super_method is the next live arm after the one the
   Method is of (*after_own). Of the Method of a method that is none, those of the entry over the late layer its
   super passes (each under the test of its cells), the first live one: where the entry's own else arm finds no
   method, CPLAN_ARM_NONE, its super_method nil. A target that is no layer is one arm, tested by nothing; no
   target, none. How many, into out (and tests, two for each). */
int cplan_super_method_arms(Compiler *c, int mi, int *out, int *tests, int *after_own) {
  int n = 0, t;
  *after_own = 0;
  if (c->scopes[mi].entry == ENTRY_DISPATCH || c->scopes[mi].entry == ENTRY_TABLE) {
    n = cplan_method_arms(c, mi, -1, out, tests);
    for (int k = 0; k < n; k++)
      if (out[k] < 0 || out[k] == mi) out[k] = CPLAN_ARM_NONE;
    if (n > 0) {
      *after_own = 1;
      return n;
    }
  }
  t = cplan_super_method_target(c, mi);
  if (t < 0) return 0;
  if (c->scopes[t].entry != ENTRY_DISPATCH) {
    out[0] = t;
    tests[0] = tests[1] = -1;
    return 1;
  }
  n = cplan_method_arms(c, t, -1, out, tests);
  for (int k = 0; k < n; k++)
    if (out[k] < 0 || out[k] == t) out[k] = CPLAN_ARM_NONE;
  return n;
}
/* ...the entry over a layer a super of node id reaches, made for its
   arguments at the entries' build (ver_entries_build, by the position and
   the program's name); -1 for none */
static int super_layer_entry(Compiler *c, int id, int t) {
  int to[VER_MAX_ARMS], layer, n = pos_runs(c, comp_scope_of(c, id), t, to, &layer);
  if (!layer) return -1;
  const Scope *m = &c->scopes[t];
  int i = ver_entry_ix(c, VER_ENTRY_LAYER, t + 1, m->class_id, m->is_cmethod, comp_scope_user_name(m), ver_arity(c, id, to, n), VF_NONE);
  return i >= 0 ? c->ver_entries[i].scope : -1;
}
/* Has t, a later mixin's guarded copy below s, the copy of a later mixin
   whose super reaches it, always run where s runs: is t's module mixed
   into their class at every statement that mixes s's in (elab_mixed_at)?
   t's cell is then set wherever s's is, and only its module's version
   tells which of t's copies runs. */
static int layer_ran_before(Compiler *c, const Scope *s, const Scope *t) {
  int cls = s->class_id, sg = s->is_cmethod, mod = s->origin_module_ci - 1, low = t->origin_module_ci - 1, nmx = 0, any = 0;
  if (cls < 0 || mod < 0 || low < 0 || mod == low || !s->mixin_guarded || t->class_id != cls || t->is_cmethod != sg) return 0;
  const LrMixin *mx = elab_class_mixins(c, cls, &nmx);
  for (int j = 0; j < nmx; j++) {
    if (elab_mixin_module(c, &mx[j]) != mod || elab_mix_on_singleton(mx[j].kind) != sg) continue;
    if (elab_mixed_at(c, cls, sg, low, elab_point_of(c, mx[j].node)) != ELAB_YES) return 0;
    any = 1;
  }
  return any;
}
/* A super starts at its own tail of the prepend chain. The same user name
   can have multiple tails, keyed by the first copy in each one. */
static int super_guard_entry(Compiler *c, int id, int t) {
  const Scope *m = &c->scopes[t];
  if (!m->mixin_conditional) return -1;
  int i = ver_entry_ix(c, VER_ENTRY_GUARD, t + 1, m->class_id, m->is_cmethod,
                       comp_scope_user_name(m), ver_call_n(c, id), VF_NONE);
  return i >= 0 ? c->ver_entries[i].scope : -1;
}
/* Does the method a guard entry's else arm runs, behind (the hidden method
   behind a later include's copy, Scope.super_behind), find no method of
   `name` on cls's side sg: its super reaches no method of the program, and
   no builtin, reader or builtin mixin answers (cplan_other_answers)? Then the
   chain's method_missing is what CRuby calls there (guard_entry_make), and
   where none may answer, the arm finds nothing (guard_else_bare). A
   module function of a module cls's chain includes answers on the instance
   side too, as the private instance method CRuby gives every includer:
   the super does not reach it, as it reaches no module function. */
static int guard_behind_finds_none(Compiler *c, int behind, int cls, int sg, const char *name) {
  if (!c->scopes[behind].super_behind || cplan_super_target(c, -1, &c->scopes[behind], NULL) >= 0 ||
      cplan_other_answers(c, cls, sg, name, 1))
    return 0;
  for (int s = 0; !sg && s < c->nscopes; s++) {
    const Scope *m = &c->scopes[s];
    if (!m->is_module_function || m->class_id < 0 || !m->name || !sp_streq(m->name, name)) continue;
    for (int k = cls; k >= 0; k = c->classes[k].parent)
      if (k != m->class_id && comp_mixes_in(c, k, m->class_id, 0)) return 0;
  }
  return 1;
}
/* Reflection uses the call's selectors, but a method_missing arm does
   not supply a physical method. */
int cplan_guarded_presence(Compiler *c, int cls, const char *name, int own, CplanCells *out) {
  int mi = conditional_guard_method(c, cls, name);
  if (mi < 0) return 0;
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, NULL, NULL);
  if (!guard_is_conditional(c, to, n) && !guard_vis_runs(c, to, n)) return 0;
  if (!out) return 1;
  DispArm arms[VER_MAX_ARMS];
  memset(out, 0, sizeof *out);
  char sel[VER_MAX_ARMS][32];
  n = guard_chain(c, mi, to, arms, sel);
  for (int i = 0; i < n - 1; i++) {
    const Scope *s = &c->scopes[to[i]];
    if (own && (s->is_include_copy || s->is_prepend_copy || s->class_id != cls)) continue;
    int k = out->n++;
    memcpy(out->cell[k], sel[i], sizeof out->cell[k]);
    out->val[k] = arms[i].val;
    /* (a visibility only the run tells is read from its visibility cell, which the entry writes: guard_entry_make) */
    int cell = -1, v = guard_version_vis(c, to[i], &cell);
    if (v < 0) ver_vis_cell_name(cell, out->vis[k]);
    else out->ans[k] = (unsigned char)core_bit_of_vis(v);
  }
  int last = to[n - 1];
  const struct VerEntry *history = guard_history_of(c, last);
  if (history) {
    if (own && history->cls != cls) return 1;
    ElabVersion vs[VER_MAX_ARMS];
    int nv = guard_history_arms(c, history, vs, NULL);
    for (int i = 0; i < nv && out->n < VER_MAX_ARMS; i++) {
      int k = out->n++;
      ver_cell_name(vs[i].cell, out->cell[k]);
      out->val[k] = vs[i].n;
      if (vs[i].v->kind == LR_UNDEF) continue;  /* the selector's answer is absent */
      int v = elab_physical_version_vis(c, &vs[i]);
      if (v < 0) ver_vis_cell_name(vs[i].cell, out->vis[k]);
      else out->ans[k] = (unsigned char)core_bit_of_vis(v);
    }
    out->els = core_bit_of_vis(SP_VIS_PUBLIC);
    return 1;
  }
  const Scope *s = &c->scopes[last];
  if (guard_behind_finds_none(c, last, cls, 0, name)) return 1;
  if (!own || (!s->super_behind && s->class_id == cls && !s->is_include_copy && !s->is_prepend_copy))
    out->els = core_bit_of_vis(guard_visibility(c, last));
  return 1;
}
/* The guard entry of mi, a copy a later prepend puts in front of a method
   (Scope.mixin_guarded), with ar parameters: the same if-chain over the
   cells of the prepends in front of the method, nearest first, its else
   arm the method behind them all. The arm of a mixin whose module defines
   the method again runs the entry over its copies of the versions
   (pos_entry): whether the mixin has run and which version its
   module holds are told apart, each by its own cell. *behind: the method
   behind them all where the else arm calls method_missing in its place,
   -1 otherwise. */
static int guard_history_entry(Compiler *c, const struct VerEntry *history, int face, int argc);
static int guard_entry_make(Compiler *c, int mi, int ar, int face, int *behind) {
  DispArm arm[VER_MAX_ARMS];
  VerArmVis vis[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, arm, sel), blk = 0, spliced = 1;
  *behind = -1;
  if (n < 2) return -1;
  int else_vis = 0, af[VER_MAX_ARMS + 1], av[VER_MAX_ARMS + 1];
  /* (an arm whose visibility the run tells reads the visibility cell of its version's history: a face's arm asks it,
     ver_vis_arm, and a presence reader's, cplan_guarded_presence; the arm's history has the cells written) */
  for (int k = 0, cell = -1; k < n; k++) {
    af[k] = guard_arm_face(c, to[k], face);
    av[k] = k < n - 1 ? guard_version_vis(c, to[k], &cell) : 0;
    if (av[k] < 0) arm[k].hist = cell + 1;
  }
  if (face) {
    for (int k = 0; k < n - 1; k++)
      vis[k] = (VerArmVis){ av[k] >= 0 && ver_closed_to(av[k], af[k]) ? av[k] : 0, av[k] < 0 && ver_face_closes(af[k]) ? af[k] : 0 };
    const struct VerEntry *history = guard_history_of(c, to[n - 1]);
    if (history) {
      int e = guard_history_entry(c, history, face, ar);
      if (e >= 0) to[n - 1] = e;
    } else {
      int v = guard_visibility(c, to[n - 1]);
      else_vis = ver_closed_to(v, af[n - 1]) ? v : 0;
    }
  }
  /* Cloned forwarding bodies retain the hidden name even when the clone
     has no pin in the original entry's node index. (A layer's entry, for
     the arm's face, tests its versions' visibility itself: pos_entry.) */
  for (int k = 0; k < n - 1; k++) {
    int e = c->scopes[to[k]].mixin_guarded ? pos_entry(c, NULL, to[k], ar, af[k]) : to[k];
    arm[k].callee = e == to[k] ? c->scopes[to[k]].name : comp_scope_user_name(&c->scopes[mi]);
    if (e != to[k] && face && ver_face_closes(af[k])) vis[k] = (VerArmVis){ 0, 0 };
    to[k] = e;
  }
  /* (the method behind them all a layer too: its entry, pos_entry) */
  int behind_e = pos_entry(c, NULL, to[n - 1], ar, af[n - 1]);
  if (behind_e != to[n - 1] && ver_face_closes(af[n - 1])) else_vis = 0;
  to[n - 1] = behind_e;
  for (int k = 0; k < n; k++) {
    blk |= c->scopes[to[k]].blk_param || c->scopes[to[k]].yields;
    spliced &= c->scopes[to[k]].yields;
  }
  const Scope *m = &c->scopes[mi];   /* (after the layers' entries, which grow the scopes) */
  int cls = m->class_id, sg = m->is_cmethod, els = -1;
  const char *name = comp_scope_user_name(m);
  /* the hidden method behind a later include's copy that finds no method:
     CRuby calls the chain's method_missing there, as an open history's
     entry does for none run (VK_MM) */
  int absent = guard_behind_finds_none(c, to[n - 1], cls, sg, name);
  int mm = (absent || face) && cplan_mm_method(c, -1, cls, sg) >= 0;
  if (mm && (absent || else_vis)) {
    els = ver_absent_else(c, VK_MM, cls, sg, name, to, n, ar, 0, &blk, &spliced);
    if (absent) *behind = to[n - 1];
  } else if (absent && guard_is_conditional(c, to, n)) {
    els = ver_absent_else(c, VK_NOMETHOD, cls, sg, name, to, n, ar, 0, &blk, &spliced);
  }
  /* A closed method still calls the receiver's method_missing. Keep
     its forwarding form and the caller's block in the entry. */
  for (int k = 0; mm && face && k < n - 1; k++) {
    if (!vis[k].raise) continue;
    arm[k].stmt = 1 + ver_absent_else(c, VK_MM, cls, sg, name, to, n, ar, 0, &blk, &spliced);
    to[k] = -1;
  }
  return ver_entry_emit(c, cls, sg, name, ar, blk, spliced, arm, to, face ? vis : NULL, n - 1, to[n - 1], els, else_vis);
}
static void ver_entry_add(Compiler *c, const char *name, struct VerEntry e) {
  if (!(c->nver_entries & (c->nver_entries - 1)) &&
      !(c->ver_entries = realloc(c->ver_entries, sizeof *c->ver_entries * (size_t)(c->nver_entries ? 2 * c->nver_entries : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  e.name = name_dup(name);
  c->ver_entries[c->nver_entries++] = e;
}
/* The entry of a history the load phase leaves open (elab_history_open):
   the method by name (ENTRY_NAMED), the same if-chain over the history's
   own versions, each a def's or a define_method's scope (the block stays
   a closure where it stands), hidden behind it (Scope.replaced); its else
   arm, for no version run yet, what the chain answers past the history
   (ver_named_else). With refuse, the entry is a raise flagged so, refused
   (feature_limit) as "cond_def_override" (ver_named_add). Spelled as its versions
   are: a later prepend's shadow name, which the prepend chain's super
   reaches. Kept in the entries as guard 2 for its cells' writes, under
   hcls, the history's class. Every history is asked first and its entry
   made after the last ask (VerNamed), so that a made entry's nodes
   rebuild no ask's parent map. */
typedef struct {
  int cls, sg, hcls, nv, ar, blk, els, espl, refl, atdef, bycount, last, vcall;   /* (espl: the else arm lets a spliced block through, ver_else_block; refl: ver_refl_named; atdef: ver_named_refuse_defs; bycount: ver_named_add; vcall: the entry of a variable-like call, VF_VCALL, whose raise is NameError) */
  const char *name, *refuse;
  ElabVersion vs[VER_MAX_ARMS];
  int to[VER_MAX_ARMS];
} VerNamed;
/* (sup: the method scopes a super of the program reaches; vc: the names
   bare calls name, sorted; rn: the names a call, a super, a Symbol or
   String a call is handed (not define_method's, nor a presence question's, which
   answers from the final methods), an alias or a `&:sym` names, sorted;
   demand: asked for the deferred histories a call names now) */
typedef struct VerNamedAsks { Compiler *c; VerNamed *v; int n; const unsigned char *sup; int nsup; const char **vc; int nvc; const char **rn; int nrn, demand;
  int *mh, *mn, *mk, *seen, *cps, stamp; unsigned long long *bydef; int far; } VerNamedAsks;   /* (ver_mixers_build: the mixins by module, a mark per class side, the scopes by def) */
static int ver_named_else(Compiler *c, const VerNamed *w);
/* (a name reflection does not answer: a private one is no public list's, feature_limit) */
static void ver_refl_count(Compiler *c, int vis) {
  if (vis == SP_VIS_PRIVATE) c->nver_refl_priv++;
  else c->nver_refl++;
}
/* A captured Method's empty history arm calls the pinned builtin directly. */
static int ver_capture_builtin(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  int call;
  int def = disp_string_reader_def(nt, -1, w->name, string_boxed_reader_arity(w->name), &call);
  ver_pin(c, call, CT_PIN_BUILTIN, 0);
  char hidden[320];
  comp_shadow_name(c, w->cls, w->name, hidden, sizeof hidden);
  int mi = synth_def_scope(c, w->cls, hidden, def, 0);
  comp_scope_set_shadow(c, mi, SHADOW_PREP, w->name);
  return mi;
}
static void ver_named_make(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  const int *to = w->to;
  int spliced = w->espl, nv = w->nv, last = w->last;
  char *spell = NULL;
  for (int i = 0; i < nv; i++) {
    ver_cell_name(w->vs[i].cell, sel[i]);
    arm[i] = (DispArm){ sel[i], w->vs[i].n, w->name, w->vs[i].cell + 1 };
    if (to[i] < 0) {
      VerNamed missing = *w;
      missing.els = cplan_mm_method(c, -1, w->cls, w->sg) >= 0 ? VK_MM : VK_NOMETHOD;
      int undef = ver_named_else(c, &missing);
      nt_node_set_int(nt, undef, "undef_arm", 1);
      arm[i].stmt = 1 + undef;
      continue;
    }
    spliced &= c->scopes[to[i]].yields || !c->scopes[to[i]].blk_param;   /* (one with no &b lets it go no further) */
    if (c->scopes[to[i]].shadow == SHADOW_PREP && !spell) spell = strdup(c->scopes[to[i]].name);
  }
  if (!spell && c->scopes[last].shadow == SHADOW_PREP) spell = strdup(c->scopes[last].name);
  int els, like = c->scopes[last].def_node;
  if (w->bycount) {   /* (no entry of its own: its versions answer reflection as an entry's def does, ver_cond_scope, and only the calls bound to them reach them) */
    for (int i = 0; i < nv; i++) {
      nt_node_set_int(nt, c->scopes[to[i]].def_node, "cond_def_refl", 1);
      nt_node_set_int(nt, c->scopes[to[i]].def_node, "cond_def_bycount", 1);
      c->scopes[to[i]].replaced = 1;
      comp_touch_scope(c->scopes[to[i]].idx);
    }
    c->nver_bycount++;
    ver_refl_count(c, w->vs[0].v->vis);
    return;
  }
  if (w->refuse) nt_node_set_int(nt, els = disp_raise_no_method(c, w->name, w->name, 0, 0), w->refuse, 1);
  else els = ver_named_else(c, w);
  /* (at the last version, where a refusal names it) */
  static const char *const at[] = { "node_line", "node_file", "node_col" };
  for (int i = 0; i < 3; i++) nt_node_set_int(nt, els, at[i], nt_int(nt, like, at[i], 0));
  int e = ver_entry_emit(c, w->cls, w->sg, w->name, w->ar, w->blk, spliced, arm, to, NULL, w->refuse ? 0 : nv, last, els, 0);
  c->scopes[e].entry = ENTRY_NAMED;
  if (w->refl) { nt_node_set_int(nt, c->scopes[e].def_node, "cond_def_refl", 1); ver_refl_count(c, w->vs[0].v->vis); }
  if (spell) {
    char *u = c->scopes[e].name;
    c->scopes[e].name = spell;
    comp_scope_set_shadow(c, e, SHADOW_PREP, u);
    free(u);
  }
  for (int i = 0; i < nv; i++) if (to[i] >= 0) { c->scopes[to[i]].replaced = 1; comp_touch_scope(c->scopes[to[i]].idx); }
  int guarded_history = spell && w->els == VK_BUILTIN && ct_versioned_string_reader(c, w->cls, w->sg, w->name);
  if (guarded_history) {
    /* The guarded send and the module's super enter the same own history. */
    for (int s = 0; s < e; s++) {
      Scope *m = &c->scopes[s];
      if (m->class_id != w->cls || !m->mixin_guarded) continue;
      if (m->guard_behind == last) {
        m->guard_behind = e;
        comp_touch_scope_shape(c, s);
        continue;
      }
      for (int i = 0; i < nv; i++)
        if (to[i] >= 0 && m->guard_behind == to[i]) {
          m->guard_behind = e;
          comp_touch_scope_shape(c, s);
          break;
        }
    }
  }
  int captured_builtin = guarded_history ? ver_capture_builtin(c, w) : -1;
  /* (an includer's: under the module's instance side) */
  ver_entry_add(c, comp_scope_user_name(&c->scopes[e]), (struct VerEntry){ NULL, VER_ENTRY_HISTORY, w->hcls, w->hcls == w->cls ? w->sg : 0, w->ar, e,
                                      .behind = captured_builtin + 1,
                                      .vis = guarded_history ? VER_VIS_STRING_HISTORY : 0,
                                      .finds = ver_else_finds(c, w->refuse ? VK_NOMETHOD : w->els, w->cls, w->sg, w->name, w->vs[0].v->vis, last) });
}
/* ...its else arm, as ver_named_kind decided: NoMethodError's message
   names the receiver's class by its Ruby name, read at run time when cls
   has subclasses, is a module or is a Class.new class (which may have no
   name, or take one only at run time) */
static int ver_named_else(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *name = w->name;
  int cls = w->cls, sg = w->sg;
  switch (w->els) {
  case VK_SUPER: return disp_super_call(nt, w->ar, w->blk);
  case VK_NOOP: return nt_new_node(nt, "NilNode");
  case VK_TEXT: return disp_super_call(nt, 0, 0);
  case VK_MM: comp_sym_intern(c, name); return disp_method_missing_call(nt, name, w->ar, w->blk);   /* (its Symbol, made after the literals were interned) */
  case VK_CELL: {
    /* (a Kernel row's name in a class that has Kernel where a cell says so: the row where a cell of its chain is set,
       else NoMethodError; the row's call is made once for each cell) */
    /* (the class's own method_missing, where it has one, answers where no cell is set, as it does where it has no Kernel) */
    int els;
    if (cplan_mm_method(c, -1, cls, sg) >= 0) {
      comp_sym_intern(c, name);
      els = disp_method_missing_call(nt, name, w->ar, w->blk);
    }
    else els = ver_no_method_raise(c, cls, sg, name, w->ar, w->vcall);
    VerIds cells;
    VerNamed row = *w;
    row.els = VK_BUILTIN;
    ver_kern_cells(c, cls, &cells);
    for (int k = 0; k < cells.n; k++) {
      char sel[32];
      cplan_kernel_cell_name(cells.v[k], sel);
      (void)ver_kern_cell(c, cells.v[k]);
      els = disp_cell_test(nt, sel, 1, ver_named_else(c, &row), els);
    }
    free(cells.v);
    return els;
  }
  case VK_BUILTIN: {
    /* (a record class's builtin that takes none, with `*r, **k` passed on:
       called where both are empty, as CRuby passes it no argument then) */
    int bare = w->ar < 0 && !is_record_class_cmethod_with_args(name);
    /* (an instance side's, a Kernel row's, private: called without a
       receiver; a reopened builtin's own public method, on self) */
    int str_reader = ct_versioned_string_reader(c, w->cls, sg, name);
    int own = !sg && w->cls >= 0 && w->ar >= 0 && !bop_kernel_answers(name, -1, 0) && ver_builtin_instance_answers(c, w->cls, name, w->ar);
    int ar = bare ? (str_reader ? string_boxed_reader_arity(name) : 0) : w->ar;
    int call = sg || own || str_reader ? disp_self_call(nt, name, ar, w->blk)
                                      : disp_forward_call(nt, name, w->ar, w->blk);
    if (str_reader) nt_mark_builtin_call(nt, call);
    ver_pin(c, call, CT_PIN_BUILTIN, 0);
    return bare ? disp_when_fixed_arity(c, call, ar, str_reader && ar > 0) : call;
  }
  case VK_IVAR: {
    char iv[256];
    int rd = nt_new_node(nt, "InstanceVariableReadNode");
    snprintf(iv, sizeof iv, "@%s", name);
    nt_node_set_str(nt, rd, "name", iv);
    return rd;
  }
  default: break;
  }
  return ver_no_method_raise(c, cls, sg, name, w->ar, w->vcall);
}
/* Reuse the named dispatcher's own selectors, arity, and builtin fallback.
   Only this entry's view of the installed method's visibility changes. */
static int guard_history_entry(Compiler *c, const struct VerEntry *history, int face, int argc) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  VerArmVis vis[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int named = history->scope, cls = history->cls, sg = history->sg;
  int ar = history->ar == argc ? argc : -1;
  const char *name = history->name;
  int to[VER_MAX_ARMS], n = guard_history_arms(c, history, vs, to);
  if (!n) return -1;
  int blk = c->scopes[named].blk_param != NULL, spliced = c->scopes[named].yields;
  int mm = cplan_mm_method(c, -1, cls, sg) >= 0, builtin = -1;
  int missing = mm ? ver_absent_else(c, VK_MM, cls, sg, name, &named, 1, ar, 0, &blk, &spliced) : -1;
  builtin = ver_absent_else(c, VK_BUILTIN, cls, sg, name, &named, 1, ar, 0, &blk, &spliced);
  for (int i = 0; i < n; i++) {
    ver_cell_name(vs[i].cell, sel[i]);
    arm[i] = (DispArm){ sel[i], vs[i].n, name, vs[i].cell + 1 };
    if (to[i] < 0) {
      vis[i] = (VerArmVis){ 0, 0 };
      arm[i].stmt = 1 + (missing >= 0 ? nt_clone_subtree((NodeTable *)c->nt, missing)
                                   : ver_no_method_raise(c, cls, sg, name, ar, 0));
      nt_node_set_int((NodeTable *)c->nt, arm[i].stmt - 1, "undef_arm", 1);
      continue;
    }
    int v = elab_physical_version_vis(c, &vs[i]), f = guard_arm_face(c, to[i], face);
    vis[i] = (VerArmVis){ v >= 0 && ver_closed_to(v, f) ? v : 0, v < 0 ? f : 0, missing + 1 };
    if (missing >= 0 && vis[i].raise) {
      arm[i].stmt = 1 + nt_clone_subtree((NodeTable *)c->nt, missing);
      to[i] = -1;
    }
  }
  return ver_entry_emit(c, cls, sg, name, ar, blk, spliced, arm, to, vis, n, named, builtin, 0);
}
/* Does a builtin module that class k mixes in on the side sg, or a module
   of the program it mixes in, and so on, have a method `name` in CRuby's
   tables? */
static int other_mixins_answer(Compiler *c, int k, int sg, const char *name, int certain, int depth) {
  int nmx = 0;
  const LrMixin *mx = elab_class_mixins(c, k, &nmx);
  for (int j = 0; j < nmx; j++) {
    int r = mx[j].ref;
    NodeKind rk = r >= 0 ? nt_kind(c->nt, r) : NK_NilNode;
    if (!(sg ? mx[j].kind == LR_EXTEND : mx[j].kind == LR_INCLUDE || mx[j].kind == LR_PREPEND)) continue;
    if (mx[j].mod >= 0 && depth < 16 && other_mixins_answer(c, mx[j].mod, 0, name, certain, depth + 1)) return 1;
    /* a module of the program by a builtin's name reopens it (a library's
       `module Enumerable`): the builtin's methods are still there */
    const char *mn = mx[j].mod >= 0 ? c->classes[mx[j].mod].name :
      (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) ? nt_str(c->nt, r, "name") : NULL;
    if (mn && builtin_method_answers(mn, name, 0, certain)) return 1;
  }
  return 0;
}
/* The superclass step of a chain the parents are not resolved for yet
   (resolve_parents runs after the aliases are registered): the record's */
static int other_super(Compiler *c, int k) {
  int p = c->classes[k].parent;
  return p >= 0 ? p : elab_class_super(c, k);
}
/* The builtin class the chain of cls ends in, by its row's name in CRuby's
   tables (Object for one with no row); *root is the last class of the
   program on it */
static const char *other_builtin_root(Compiler *c, int cls, int *root) {
  int k = cls;
  for (int d = 0; other_super(c, k) >= 0 && d < c->nclasses; d++) k = other_super(c, k);
  *root = k;
  int id = class_builtin_parent(c, k);
  const BuiltinClass *r;
  for (int i = 0; (r = builtin_class_at(i)); i++) if (r->id == id) return r->name;
  return "Object";
}
/* Does a method the program's versions do not give answer `name` on cls's
   side sg: a builtin's that every object (class, module) answers, cls's
   builtin superclass's (a reopened builtin is its own) or a builtin module's
   that is mixed in (Comparable's and Enumerable's own among them; through a
   module of the program too, by include, prepend or, on the class side,
   extend), a Struct or Data member in its chain (writer too), or on the
   class side a record class's own builtin (elab_record_cmethod)? The
   question has two askers. MAY an answer exist (certain 0: an alias's
   original, where a miss is a NameError the alias would raise, and a missed
   one is master's answer): CRuby's tables of every listed library at every
   version, no reader of cls's own (an accessor made later is no method yet,
   its accessors being the record's versions). MUST one (certain 1: the else
   arm and every reader that drops a method_missing or a NoMethodError
   because something answers): only a method CRuby has with no library or
   with one the program requires, at the version CRuby ships, cls's own
   reader too, and no definition hook (a conditional hook def replaces the
   no-op the load fires without). */
int cplan_other_answers(Compiler *c, int cls, int sg, const char *name, int certain) {
  int root = cls, mod = cls >= 0 && comp_class_is_module(c, &c->classes[cls]);
  if (builtin_every_object_answers(name, sg) || is_object_receiver_handoff(name) || is_method_missing_name(name) ||
      is_implicitly_private_name(name) || (sg && is_class_plain_query(name)) ||
      builtin_self_method_known(name, !sg ? BSELF_MAIN : mod ? BSELF_MODULE : BSELF_CLASS)) return 1;
  /* CRuby's own tables: the chain's root (a module's is Object's, and Module's
     on its class side), then each class on it where it reopens a builtin */
  /* (a definition hook a program defines under a condition replaces the
     builtin no-op, which the load fires the hooks without: the else arm
     does not count it as another method. A compromise that keeps master's
     answer: an explicit `send(:inherited, k)` while no version is live still
     raises NoMethodError, where CRuby runs the builtin no-op and answers nil.
     Its home is an else kind that runs the builtin no-op.) */
  int tab = !(certain && is_definition_hook_name(name));
  if (cls < 0) return tab && builtin_method_answers("Object", name, 0, certain);
  if (mod) {
    if (tab && builtin_method_answers(sg ? "Module" : "Object", name, 0, certain)) return 1;
  }
  else {
    const char *rn = other_builtin_root(c, cls, &root);
    if ((tab && builtin_method_answers(rn, name, sg, certain)) || builtin_class_id_answers(class_builtin_parent(c, root), name, sg)) return 1;
  }
  for (int k = cls; k >= 0; k = other_super(c, k)) {
    /* (a `class Class` reopening is registered as the module Class__reopen) */
    if ((tab && builtin_method_answers(k == comp_core_class(c, CORE_CLASS_REOPEN) ? "Class" : c->classes[k].name, name, sg, certain)) ||
        (tab && other_mixins_answer(c, k, sg, name, certain, 0))) return 1;
  }
  if (sg) return elab_record_cmethod(c, cls, name);
  char wb[256];
  int w = setter_base_name(name, wb, sizeof wb);
  for (int k = cls; k >= 0; k = other_super(c, k)) {
    int has = w ? comp_is_writer(&c->classes[k], wb) : comp_is_reader(&c->classes[k], comp_resolve_alias(c, k, name));
    if (has && ((certain && !w && k == cls) || c->classes[k].is_struct || c->classes[k].is_data)) return 1;
  }
  return 0;
}
/* The else kind of the entry of history hist (cls's side sg, versions
   to[0..nv), taking ar arguments and a block when blk): VK_KEEP when a
   super of the program reaches a version (its NoMethodError would read
   as a call's, not super's), or another method answers the name
   (cplan_other_answers), or the record's chain holds a version behind the
   history that super does not reach (an earlier include's, which the
   include path chains only behind what reaches super); else, in that
   order, what super reaches (cplan_super_target) and an inherited
   reader, each when it has the versions' visibility vis (VK_KEEP when
   not: CRuby's lookup finds it and refuses the call), method_missing
   where mm says it is there wherever the else arm runs (VK_KEEP where it
   is there at some calls only; an open history's else arm takes it as
   there, mm 1), NoMethodError (NameError in a variable-like call's
   entry, VF_VCALL: ver_absent_else), but VK_KEEP for a name a bare call
   names in an entry shared by every call (shared: an open history's),
   which does not see the call's form */
static int ver_name_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void ver_name_add(const char ***v, int *n, const char *name) {
  if (!name) return;
  if (!(*n & (*n - 1)) && !(*v = realloc(*v, sizeof **v * (size_t)(*n ? 2 * *n : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  (*v)[(*n)++] = name;
}
static int ver_named_kind(const VerNamedAsks *q, int cls, int sg, const char *name, const int *to, int nv, int hist, int vis, int ar, int blk, int mm, int shared) {
  Compiler *c = q->c;
  ElabVersion cv[VER_MAX_ARMS];
  int par = comp_super_parent(c, cls, sg), st, last = -1, nc = elab_chain_versions(c, cls, sg, name, cv, VER_MAX_ARMS);
  for (int j = 0; j < nc; j++) if (cv[j].hist == hist) last = j;
  /* a class's or module's hook (included, inherited, method_added,
     singleton_method_added and the like) with no version run, and none of
     the program's above it, is the core's no-op, behind a later class's
     history on the chain, a super of the program that reaches it too (the
     core's hook raises nothing). (Not super: an entry of a subclass, whose
     history is its superclass's, would reach that history's version.) */
  if (sg && cls >= 0 && ar == 1 && !blk && is_definition_hook_name(name)) return last < nc - 1 ? VK_KEEP : VK_NOOP;
  for (int i = 0; i < nv; i++) if (to[i] < q->nsup && q->sup[to[i]]) return VK_KEEP;
  if (cls >= 0 && (st = cplan_super_target(c, -1, &c->scopes[to[nv - 1]], NULL)) >= 0) {
    /* (a module's copy has the visibility the module's own table gives it) */
    int sc = c->scopes[st].class_id, om = c->scopes[st].origin_module_ci - 1;
    mtab_enter(c);   /* (entry construction: the visibility the super's holder's table gives the name, which the entry keeps) */
    int sv = om >= 0 ? comp_method_vis(&c->classes[om], name) : sg ? comp_cmethod_vis_declared(c, sc, name, NULL) : comp_method_vis_in_chain(c, sc, name);
    mtab_leave(c);
    return sc >= 0 && sv == vis ? VK_SUPER : VK_KEEP;
  }
  if (!sg && cls >= 0 && !ar && !blk && (is_inspect_name(name) || is_to_s_name(name)) && builtin_every_object_answers(name, 0)) return VK_TEXT;
  /* (a record class's own builtin takes `*r, **k` passed on too: ar -1) */
  if (sg && cls >= 0 && ((ar >= 0 && bop_class_answers(name, ar, blk)) || elab_record_cmethod(c, cls, name))) return VK_BUILTIN;
  /* (an instance-side name a Kernel row answers behind the versions: the row, called without a receiver at ar arguments,
     which it refuses where it takes no such count. A class without Kernel has no row behind them (ver_kernel_row_behind),
     and cplan_other_answers, which reads such a name as Kernel's or Object's, is not asked there (bare). BasicObject's own
     methods of a Kernel row's name, instance_eval and instance_exec, are not read here: such a class answers NoMethodError
     for them past the versions, a gap) */
  int row = !sg && ver_kernel_row_behind(c, cls, name), bare = !sg && cls >= 0 && !row && bop_kernel_answers(name, -1, 0);
  if (row && ar >= 0) return VK_BUILTIN;
  if (!sg && cls >= 0 && ar >= 0 && bop_kernel_answers(name, -1, 0) && cplan_class_kernel(c, cls) == KERN_CELL) return VK_CELL;
  /* (an instance-side method of the builtin a reopened class stands for:
     with no version run, the builtin's own, #11819) */
  if (!sg && cls >= 0 && ar >= 0 && ver_builtin_instance_answers(c, cls, name, ar)) return VK_BUILTIN;
  if (!bare && cplan_other_answers(c, cls, sg, name, 1)) return VK_KEEP;
  if (par >= 0 && !sg && !ar && !blk) {
    mtab_enter(c);   /* (entry construction: an attribute reader the parent's chain holds, and the visibility its table gives it) */
    int rd = comp_reader_in_chain(c, par, name, NULL), pv = rd ? comp_method_vis_in_chain(c, par, name) : 0;
    mtab_leave(c);
    if (rd) return pv == vis ? VK_IVAR : VK_KEEP;
  }
  if (last < nc - 1) return VK_KEEP;
  /* (method_missing where the calls run: there at some and not at
     others, the history keeps its handling; at none, NoMethodError) */
  if (cplan_mm_method(c, -1, cls, sg) >= 0 && (mm & 1)) return mm == 1 ? VK_MM : VK_KEEP;
  return shared && q->nvc && bsearch(&name, q->vc, (size_t)q->nvc, sizeof *q->vc, ver_name_cmp) ? VK_KEEP : VK_NOMETHOD;
}
/* Does the method the else arm els calls (super's target, method_missing)
   take a block, which the entry then passes on (1), and is it spliced, a
   block it is given flowing in like a yield (2)? */
static int ver_else_block(Compiler *c, int cls, int sg, int last, int els) {
  int t = -1;
  if (els == VK_SUPER) t = cplan_super_target(c, -1, &c->scopes[last], NULL);
  else if (els == VK_MM) t = cplan_mm_method(c, -1, cls, sg);
  return t < 0 || !(c->scopes[t].blk_param || c->scopes[t].yields) ? 0 : c->scopes[t].yields ? 2 : 1;
}
/* The else arm of rcls's version entry for a call that may meet no
   version of the chain yet (ver_entries_demand), the versions to[0..na),
   of one visibility: what the lookup finds with none of them made, past
   the farthest of their histories, as an open history's entry answers
   for none run (ver_named_kind's els, not VK_KEEP, for which the entry
   keeps the last version, the table's answer; ver_named_else), at the
   last version's line. It may take the caller's block on
   (ver_else_block). */
static int ver_absent_else(Compiler *c, int els, int rcls, int sg, const char *name, const int *to, int na, int ar, int vcall, int *blk, int *spliced) {
  static const char *const at[] = { "node_line", "node_file", "node_col" };
  int eb = ver_else_block(c, rcls, sg, to[na - 1], els), like = c->scopes[to[na - 1]].def_node;
  *blk |= eb != 0;
  *spliced &= eb != 1;
  VerNamed w = { .cls = rcls, .sg = sg, .ar = ar, .blk = *blk, .els = els, .vcall = vcall, .name = name };
  int e = ver_named_else(c, &w);
  for (int i = 0; i < 3; i++) nt_node_set_int((NodeTable *)c->nt, e, at[i], nt_int(c->nt, like, at[i], 0));
  return e;
}
/* What an entry's else arm of kind els finds of cls's method `name` on
   side sg while no version of its chain is live, as respond_to? and a
   Method's maker see it (VerEntry.finds), the versions of visibility vis,
   the last last: VK_KEEP the table's method (main's, theirs); super's target, which has
   theirs (ver_named_kind), but a module's hook no method of the program
   answers, the core's private no-op; an inherited reader, which has
   theirs too; a public builtin or Kernel text; no method for
   NoMethodError and method_missing */
static int ver_else_finds(Compiler *c, int els, int cls, int sg, const char *name, int vis, int last) {
  switch (els) {
  case VK_NOMETHOD: case VK_MM: return VER_FINDS_NONE;
  case VK_BUILTIN: case VK_TEXT: return VER_FINDS_PUBLIC;
  case VK_CELL: case VK_NOOP: return VER_FINDS_PRIVATE;   /* (a Kernel row's, the core's hook: private, not found where the cell is clear either) */
  case VK_KEEP:   /* (main's: the versions', a private one unless declared) */
    if (cls >= 0) {
      mtab_enter(c);   /* (entry construction: the visibility the versions' table leaves the name) */
      vis = sg ? comp_cmethod_vis_declared(c, cls, name, NULL) : comp_method_vis_in_chain(c, cls, name);
      mtab_leave(c);
    }
    break;
  case VK_SUPER:
    if (last < 0 || cplan_super_target(c, -1, &c->scopes[last], NULL) < 0) vis = SP_VIS_PRIVATE;
    break;
  default: break;
  }
  return vis == SP_VIS_PUBLIC ? VER_FINDS_PUBLIC : vis == SP_VIS_PROTECTED ? VER_FINDS_PROTECTED : VER_FINDS_PRIVATE;
}
/* Does define_method call `dm` take keyword parameters, which its scope
   does not bind (the call sites bind keywords from a DefNode's)? */
static int ver_dm_keywords(const NodeTable *nt, int dm) {
  int b = nt_kind(nt, dm) == NK_CallNode ? nt_ref(nt, dm, "block") : -1, bp = b >= 0 ? nt_ref(nt, b, "parameters") : -1, n = 0;
  int pn = bp >= 0 && nt_kind(nt, bp) == NK_BlockParametersNode ? nt_ref(nt, bp, "parameters") : -1;
  if (pn >= 0) nt_arr(nt, pn, "keywords", &n);
  return pn >= 0 && (n || nt_ref(nt, pn, "keyword_rest") >= 0);
}
/* Does node n stand in a class, module or singleton class body with a
   rescue clause, whose statements no code runs (outside a method body)? */
static int ver_in_unrun_body(const Compiler *c, const int *par, int n) {
  for (int a = par[n]; a >= 0 && par[a] >= 0; a = par[a]) {
    NodeKind k = nt_kind(c->nt, a), pk = nt_kind(c->nt, par[a]);
    if (k == NK_DefNode) return 0;
    if (k == NK_BeginNode && (pk == NK_ClassNode || pk == NK_ModuleNode || pk == NK_SingletonClassNode)) return 1;
  }
  return 0;
}
/* Does node n stand in a rescue clause of a program whose root records
   dropped_require: the inline model dropped a require CRuby's LoadError
   would have left, so whether any rescue clause runs is not known (and its
   defs keep their static handling)? */
static int ver_in_dropped_rescue(const Compiler *c, const int *par, int n) {
  if (!nt_int(c->nt, c->nt->root_id, "dropped_require", 0)) return 0;
  for (int a = par[n]; a >= 0; n = a, a = par[a])
    if (nt_kind(c->nt, a) == NK_RescueNode && n == nt_ref(c->nt, a, "statements")) return 1;
  return 0;
}
/* ...for one open history, asked (its entry made after every ask), with
   its else kind (ver_named_kind): none for a history holding a version no
   arm calls (an undef, a copy, an alias, an accessor), one whose cell
   write has no statement of its own (a def in a value position of its
   statement) or would not run (in a class body with a rescue clause,
   which no code runs, at any depth), a define_method whose block takes
   keywords, versions of more than one visibility, an initialize (which
   the constructor binds by name), or an else kind VK_KEEP: the table's
   answer stays, the last version taken as run. None for an arm of a
   dispatcher desugar_conditional_defs made, and none for a history no
   held mark keeps whose name nothing can read (ver_name_read) until a
   call names it (ver_entries_demand): no entry, cell
   or rename stands where no call reaches them. A history holding a version
   marked cond_lone (a lone def under an `if` that pass leaves to the
   record, a define_method a Struct or Data block may not run,
   lr_block_cond_dms) refuses where it would have none
   ("cond_def_override"); its entry's name is not answered by reflection
   (ver_refl_named). A module's instance history arms each class (each
   side) whose chain holds it (elab_chain_versions, through modules that
   include the module too), through that class's copies of the versions
   (include_module_methods), when the history is the chain's nearest and
   the class's table answers with the last copy; a held history that some
   such class cannot arm (a prepend's chain, the class's own method in
   front) is refused at its versions instead (ver_named_refuse_defs).
   The entry takes the caller's block for a version that yields or takes
   one, and for super's target or method_missing when it takes one
   (ver_else_block), to pass it on. */
static void ver_named_add(VerNamedAsks *q, int cls, int sg, const char *name, const ElabVersion *vs, const int *to, int nv,
                          const char *refuse, int hcls, int held) {
  Compiler *c = q->c;
  int blk = 0, undef = 0, last = -1, els = VK_NOMETHOD, eb;
  for (int i = 0; i < nv; i++) {
    if (to[i] < 0) { undef = 1; continue; }
    last = to[i];
    blk |= c->scopes[to[i]].blk_param || c->scopes[to[i]].yields;
  }
  /* An undef can be the first and only own version. The guarded chain
     already holds the builtin behind it, which supplies the entry shape. */
  if (last < 0) {
    int guard = conditional_guard_method(c, cls, name);
    if (guard < 0) return;
    int chain[VER_MAX_ARMS + 1], n = guard_chain(c, guard, chain, NULL, NULL);
    last = chain[n - 1];
    if (!nt_int(c->nt, c->scopes[last].def_node, "bam_native", 0)) return;
  }
  int ar = q->far ? q->far - 1 : undef ? -1 : ver_arity(c, -1, to, nv);
  /* a held history of a Kernel row's name that Kernel's row, placed in front of it (a prepend of Kernel), answers first
     on cls's chain: no version of it is found there, so its entry would stand for what no call finds */
  if (held && !refuse && !sg && cls >= 0 && bop_kernel_answers(name, -1, 0)) {
    ElabVersion cv[VER_MAX_ARMS];
    int nc = elab_chain_versions(c, cls, sg, name, cv, VER_MAX_ARMS), found = 0;
    for (int j = 0; j < nc; j++) found |= cv[j].hist == vs[0].hist;
    if (nc >= 0 && nc < VER_MAX_ARMS && !found) refuse = "cond_def_override";
  }
  /* a held history of a Kernel row's name that takes no one count (ar -1: an optional parameter), in a blank slate that
     has Kernel or may gain it (whose chain the record does not settle, so that no entry is made late for a call in a
     method body), is an entry for each count the program's calls of the name pass (the entry's arms call the
     versions with that many arguments, its else arm the row at that count); a call at another count stays refused
     (ver_named_make, bycount) */
  if (held && !refuse && !q->far && ar < 0 && !sg && cls >= 0 && bop_kernel_answers(name, -1, 0) && cplan_class_kernel(c, cls) != KERN_LACKS && class_is_blank_slate(c, cls)) {
    VerIds seen = { NULL, 0, 0 };
    for (int id = an_calls_named_first(c, name); id >= 0; id = an_calls_named_next(id)) {
      /* (a call in a method body, which the record gives no point: the entries a call of the load phase makes late,
         ver_late_make, are not made for it) */
      int n = nt_kind(c->nt, id) == NK_CallNode && elab_point_of(c, id).region < 0 ? ver_call_n(c, id) : -1;
      int ok = n >= 0 && bop_kernel_answers(name, n, 0);   /* (the row takes that count) */
      for (int i = 0; ok && i < nv; i++) ok = ver_takes(c, to[i], n);
      if (ok) ver_ids_add(&seen, n);
    }
    for (int i = 0; i < seen.n; i++) {
      q->far = seen.v[i] + 1;
      ver_named_add(q, cls, sg, name, vs, to, nv, refuse, hcls, held);
    }
    q->far = 0;
    free(seen.v);
  }
  /* a held history of a name a Kernel row answers behind it (ver_kernel_row_behind), taking no one
     count (ar -1: an optional or a rest parameter), has no entry of its own, whose else arm would pass
     `*r, **k` on to a row that takes none: each call's count makes one (ver_late_make), its else arm
     the row at that count */
  int bycount = held && !refuse && ar < 0 && !sg && cls >= 0 && bop_kernel_answers(name, -1, 0) && cplan_class_kernel(c, cls) != KERN_LACKS;
  int str_behind = undef;
  int string_reader = ct_versioned_string_reader(c, cls, sg, name);
  if (string_reader && cls == hcls)
    for (int i = 0; i < nv; i++)
      str_behind |= to[i] >= 0 && c->scopes[to[i]].shadow == SHADOW_PREP && !c->scopes[to[i]].origin_module_ci;
  if (str_behind && ar != (string_reader ? string_boxed_reader_arity(name) : is_spaceship_name(name))) ar = -1;
  if (!refuse && !bycount)
    els = str_behind ? VK_BUILTIN : ver_named_kind(q, cls, sg, name, to, nv, vs[0].hist, vs[0].v->vis, ar, blk, 1, 1);
  if (els == VK_KEEP && !held) return;
  if (els == VK_KEEP) refuse = "cond_def_override";
  eb = refuse ? 0 : ver_else_block(c, cls, sg, last, els);
  if (undef && !refuse) {
    int mb = ver_else_block(c, cls, sg, last, VK_MM);
    if (mb) blk = 1;
  }
  blk |= eb != 0;
  if (!(q->n & (q->n - 1)) && !(q->v = realloc(q->v, sizeof *q->v * (size_t)(q->n ? 2 * q->n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  VerNamed *w = &q->v[q->n++];
  *w = (VerNamed){ cls, sg, hcls, nv, ar, blk, els, blk && eb != 1, !refuse && held, 0, bycount, last, 0, name, refuse, { { 0 } }, { 0 } };
  memcpy(w->vs, vs, sizeof *vs * (size_t)nv);
  memcpy(w->to, to, sizeof *to * (size_t)nv);
}
/* (built once, at the first module's history: every mixin's class, listed
   under its module, and the scopes in def_node order, each its def_node
   above its index) */
static int ver_bydef_cmp(const void *a, const void *b) {
  unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
  return x < y ? -1 : x > y;
}
static int ver_int_cmp(const void *a, const void *b) { return (*(const int *)a > *(const int *)b) - (*(const int *)a < *(const int *)b); }
static void ver_mixers_build(VerNamedAsks *q) {
  Compiler *c = q->c;
  int nm = 0, nk = c->nclasses + 1;
  for (int k = -1; k < c->nclasses; k++) { int n = 0; elab_class_mixins(c, k, &n); nm += n; }
  q->mh = malloc(sizeof(int) * (size_t)nk); q->mn = malloc(sizeof(int) * (size_t)(nm + 1)); q->mk = malloc(sizeof(int) * (size_t)(nm + 1));
  q->seen = calloc((size_t)nk, sizeof(int)); q->cps = calloc((size_t)nk * 2, sizeof(int)); q->bydef = malloc(sizeof *q->bydef * (size_t)(c->nscopes + 1));
  if (!q->mh || !q->mn || !q->mk || !q->seen || !q->cps || !q->bydef) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nk; k++) q->mh[k] = -1;
  nm = 0;
  for (int k = -1; k < c->nclasses; k++) {
    int n = 0, m;
    const LrMixin *mx = elab_class_mixins(c, k, &n);
    for (int j = 0; j < n; j++)
      if ((m = elab_mixin_module(c, &mx[j])) >= 0) { q->mk[nm] = k; q->mn[nm] = q->mh[m]; q->mh[m] = nm++; }
  }
  for (int s = 0; s < c->nscopes; s++) q->bydef[s] = (unsigned long long)(unsigned)c->scopes[s].def_node << 32 | (unsigned)s;
  qsort(q->bydef, (size_t)c->nscopes, sizeof *q->bydef, ver_bydef_cmp);
}
/* Can a call reach method `name` (a singleton's when sg) by its name: one
   names it, or CRuby's syntax or a builtin calls it back? (A send of a name
   only the run time knows reaches it through the arms the fixpoint makes,
   ver_entries_demand.) */
static int ver_name_read(const VerNamedAsks *q, const char *name, int sg) {
  return method_name_implicitly_invoked(name) || ic_core_callback(name, sg) ||
         (q->nrn && bsearch(&name, q->rn, (size_t)q->nrn, sizeof *q->rn, ver_name_cmp));
}
/* the first deferred history of name, the others of the name after it, or NULL */
static struct VerDefer *ver_defer_first(Compiler *c, const char *name) {
  struct VerDefer *d = c->nver_defer && name ? bsearch(&name, c->ver_defer, (size_t)c->nver_defer, sizeof *d, ver_name_cmp) : NULL;
  while (d && d > c->ver_defer && sp_streq(d[-1].name, name)) d--;
  return d;
}
/* the deferred history (cls, sg, name) at taken t, or NULL */
static struct VerDefer *ver_defer_find(Compiler *c, int cls, int sg, const char *name, int t) {
  for (struct VerDefer *d = ver_defer_first(c, name); d && d < c->ver_defer + c->nver_defer && sp_streq(d->name, name); d++)
    if (d->cls == cls && d->sg == sg && d->taken == t) return d;
  return NULL;
}
/* (a held history no entry can stand for: refused at its marked versions, ver_named_refuse_defs) */
static void ver_named_refuse_at(VerNamedAsks *q, int cls, int sg, const char *name) {
  if (!(q->n & (q->n - 1)) && !(q->v = realloc(q->v, sizeof *q->v * (size_t)(q->n ? 2 * q->n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  q->v[q->n++] = (VerNamed){ .cls = cls, .sg = sg, .hcls = cls, .atdef = 1, .name = name, .refuse = "cond_def_override" };
}
static void ver_named_one(void *arg, int cls, int sg, const char *name) {
  VerNamedAsks *q = arg;
  Compiler *c = q->c;
  const int *par = comp_parent_map(c);
  ElabVersion vs[VER_MAX_ARMS], cv[VER_MAX_ARMS];
  const char *refuse = NULL;
  int to[VER_MAX_ARMS], nv = cls < -1 ? -1 : elab_history_versions(c, cls, sg, name, vs, VER_MAX_ARMS), na = 0, bad = 0, held = 0, dc;
  int mod = cls >= 0 && !sg && comp_class_is_module(c, &c->classes[cls]), vis0 = nv > 0 ? vs[0].v->vis : 0;
  int string_guard = ct_versioned_string_reader(c, cls, sg, name) && conditional_guard_method(c, cls, name) >= 0;
  for (int i = 0; i < nv; i++) {
    const LrDef *d = vs[i].v;
    int s = d->kind == LR_DEF || d->kind == LR_DEFINE_METHOD ||
            (d->kind == LR_ALIAS && ct_versioned_string_reader(c, cls, sg, name)) ? d->scope : 0;
    int st = d->node;
    while (st >= 0 && par[st] >= 0 && nt_kind(c->nt, par[st]) != NK_StatementsNode) st = par[st];   /* (its cell write's, ver_cells_write) */
    if (nt_int(c->nt, d->node, "cond_arm", 0) || ver_in_dropped_rescue(c, par, d->node)) return;
    held |= nt_int(c->nt, d->node, "cond_lone", 0) != 0;
    if (st < 0 || par[st] < 0 || (d->may == LR_MAY_IN && st != d->node) || ver_in_unrun_body(c, par, d->node)) { bad = 1; continue; }
    if (string_guard && d->kind == LR_UNDEF) {
      vs[na] = vs[i];
      to[na++] = -1;
      continue;
    }
    if (s <= 0 || c->scopes[s].class_id != cls || c->scopes[s].is_cmethod != sg || (!mod && c->scopes[s].is_transplanted_source) ||
        (d->kind == LR_DEFINE_METHOD && ver_dm_keywords(c->nt, d->node)) ||
        (!string_guard && d->vis != vis0)) { bad = 1; continue; }
    vs[na] = vs[i];
    to[na++] = s;
  }
  int can = na && !bad && !is_initialize_name(name);   /* (the constructor binds the initialize it calls by name) */
  struct VerDefer *df = q->demand && !held && can ? ver_defer_find(c, cls, sg, name, 1) : NULL;
  if (q->demand && !df) return;
  if (df) df->taken = 2;
  else if (!held && can && !ver_name_read(q, name, sg)) {   /* (made once a call names it, ver_entries_demand) */
    if (!(c->nver_defer & (c->nver_defer - 1)) &&
        !(c->ver_defer = realloc(c->ver_defer, sizeof *c->ver_defer * (size_t)(c->nver_defer ? 2 * c->nver_defer : 1)))) {
      fprintf(stderr, "spinel: out of memory\n"); exit(1);
    }
    c->ver_defer[c->nver_defer++] = (struct VerDefer){ name, cls, sg, 0 };
    return;
  }
  else if (!held && !can) return;
  if (bad) refuse = "cond_def_override";
  nv = na;
  if (held && (!na || is_initialize_name(name))) { ver_named_refuse_at(q, cls, sg, name); return; }
  if (!mod) { ver_named_add(q, cls, sg, name, vs, to, nv, refuse, cls, held); return; }
  /* (a module's: asked of every includer first, armed only when no held includer fails) */
  int fail = 0, n0 = q->n, hn = elab_history_versions(c, cls, 0, name, cv, VER_MAX_ARMS), nk = 1, wide = 0, *ks;
  const int core[] = { CORE_OBJECT, CORE_BASIC_OBJECT, CORE_KERNEL, CORE_MODULE, CORE_CLASS };
  if (!q->mh) ver_mixers_build(q);
  if (!(ks = malloc(sizeof(int) * (size_t)(c->nclasses + 2)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  q->seen[cls + 1] = ++q->stamp;
  ks[0] = cls;
  /* (the classes whose chains may hold it: the module's mixers, through the
     modules that mix it in, and their descendants, a class's own taken
     when its parent's are not; every class, when main, Object or a core
     class or module mixes it in) */
  for (int h = 0, nd, y, p; h < nk; h++) {
    p = ks[h] >= 0 ? c->classes[ks[h]].parent : -1;
    const int *d = ks[h] >= 0 && (p < 0 || q->seen[p + 1] != q->stamp) ? comp_descendants(c, ks[h], &nd) : (nd = 0, NULL);
    for (int j = 0; j < (int)(sizeof core / sizeof *core); j++) wide |= ks[h] == comp_core_class(c, core[j]);
    for (int e = ks[h] >= 0 ? q->mh[ks[h]] : -1, m = 0; e >= 0 || m < nd; e >= 0 ? (e = q->mn[e]) : m++)
      if (q->seen[(y = e >= 0 ? q->mk[e] : d[m]) + 1] != q->stamp) { q->seen[y + 1] = q->stamp; wide |= y < 0; ks[nk++] = y; }
  }
  if (!wide) qsort(ks, (size_t)nk, sizeof *ks, ver_int_cmp);
  for (int i = 0; i < nv; i++) {   /* (the sides of the classes that hold a copy of a version, under any spelling) */
    unsigned long long key = (unsigned long long)(unsigned)c->scopes[to[i]].def_node << 32;
    int lo = 0, hi = c->nscopes;
    while (lo < hi) { int mid = (lo + hi) / 2; if (q->bydef[mid] < key) lo = mid + 1; else hi = mid; }
    for (int s = 0; lo < c->nscopes && q->bydef[lo] >> 32 == key >> 32; lo++)
      if ((s = (int)(q->bydef[lo] & 0xffffffff)) != to[i] && c->scopes[s].class_id >= 0)
        q->cps[2 * c->scopes[s].class_id + 2 + (c->scopes[s].is_cmethod != 0)] = q->stamp;
  }
  for (int h = 0, n = wide ? c->nclasses + 1 : nk; h < n; h++)
    for (int ksg = 0, k = wide ? h - 1 : ks[h]; ksg < 2; ksg++) {
      int kto[VER_MAX_ARMS], i = 0, nc;
      if (k >= 0 && !ksg && comp_class_is_module(c, &c->classes[k])) continue;
      nc = elab_chain_versions(c, k, ksg, name, cv, VER_MAX_ARMS);
      for (int j = 0; j < nc; j++) i += cv[j].hist == vs[0].hist;   /* (its versions, each once unless the module is mixed in again) */
      if (nc < 0) {   /* (a chain the record cannot hold: failed when it mixes the module in itself) */
        int nmx = 0;
        const LrMixin *mx = elab_class_mixins(c, k, &nmx);
        for (int j = 0; j < nmx; j++) fail |= (mx[j].kind == LR_EXTEND) == ksg && elab_mixin_module(c, &mx[j]) == cls;
      }
      if (!i) continue;
      int once = i == hn, near = cv[0].hist == vs[0].hist;
      i = 0;
      if (once && near)
        for (; i < nv && (kto[i] = comp_method_version(c, k, name, ksg, vs[i].v->node, &dc)) >= 0 && dc == k; i++) {}
      /* (behind the class's own method, which reaches them only through the copies the include path puts behind it, inc_super_reached) */
      if (once && !near && q->cps[2 * k + 2 + ksg] != q->stamp) continue;
      mtab_enter(c);   /* (entry construction: the method the includer's own table holds, which the versions it was made of must be) */
      int mi = ksg ? comp_cmethod_in_class(c, k, name) : comp_method_in_class(c, k, name);
      mtab_leave(c);
      if (i == nv && mi >= 0 && c->scopes[mi].def_node == c->scopes[kto[nv - 1]].def_node)
        ver_named_add(q, k, ksg, name, vs, kto, nv, refuse, cls, held);
      else fail = 1;
    }
  free(ks);
  if (!fail || !held) return;
  q->n = n0;
  ver_named_refuse_at(q, cls, 0, name);
}
/* ...and a held history no entry can stand for (an initialize, which
   the constructor binds by name; one with no version an arm can call; a
   module's that some class holding it cannot arm, its chain holding the
   history behind another method, a prepend's or the class's own, or apart
   from the class's copies): refused where the lowering refused it, ahead
   of each version marked cond_lone ("cond_def_override", feature_limit) */
static void ver_named_refuse_defs(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  static const char *const at[] = { "node_line", "node_file", "node_col" };
  ElabVersion vs[VER_MAX_ARMS];
  int nv = elab_history_versions(c, w->cls, w->sg, w->name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++) {
    const int *par = comp_parent_map(c);
    int d = vs[i].v->node, st = d, n0 = nt->count, r;
    if (!nt_int(nt, d, "cond_lone", 0)) continue;
    while (st >= 0 && par[st] >= 0 && nt_kind(nt, par[st]) != NK_StatementsNode) st = par[st];
    if (st < 0 || par[st] < 0) continue;
    nt_node_set_int(nt, r = disp_raise_no_method(c, w->name, w->name, 0, 0), w->refuse, 1);
    for (int j = 0; j < 3; j++) nt_node_set_int(nt, r, at[j], nt_int(nt, d, at[j], 0));
    comp_grow_node_arrays(c);
    for (int k = n0; k < nt->count; k++) c->nscope[k] = c->nscope[st];
    ver_list_put(nt, par[st], st, 0, r);
  }
}
int ver_entry_targets(const Compiler *c, int e, int *out, int *tests) {
  const NodeTable *nt = c->nt;
  int n = 0, at = ver_first_stmt(nt, nt_ref(nt, c->scopes[e].def_node, "body"));
  for (; at >= 0 && n <= VER_MAX_ARMS; at = nt_kind(nt, at) == NK_IfNode ? nt_ref(nt, at, "subsequent") : -1) {
    if (tests) tests[n] = nt_kind(nt, at) == NK_IfNode ? nt_ref(nt, at, "predicate") : -1;
    int pin = ct_pin_of(c, ver_first_stmt(nt, nt_ref(nt, at, "statements")));
    out[n++] = pin < 0 ? -1 : pin;
  }
  if (n > 1 && out[n - 1] < 0 && c->scopes[e].entry == ENTRY_NAMED) out[n - 1] = out[n - 2];
  return n;
}
/* Is arm k of entry e an undef's, which raises (ver_entry_make)? */
static int ver_arm_undef(const Compiler *c, int e, int k) {
  const NodeTable *nt = c->nt;
  int at = ver_first_stmt(nt, nt_ref(nt, c->scopes[e].def_node, "body"));
  for (; at >= 0 && k > 0 && nt_kind(nt, at) == NK_IfNode; k--) at = nt_ref(nt, at, "subsequent");
  int st = at >= 0 && !k ? ver_first_stmt(nt, nt_ref(nt, at, "statements")) : -1;
  return st >= 0 && nt_int(nt, st, "undef_arm", 0);
}
int cplan_target_runs(const Compiler *c, int t, int mi) {
  if (t < 0 || t == mi) return t >= 0;
  int to[VER_MAX_ARMS + 1], n = c->scopes[t].entry != ENTRY_NONE ? ver_entry_targets(c, t, to, NULL) : 0;
  for (int k = 0; k < n; k++) if (to[k] == mi) return 1;
  return 0;
}
/* (may an instance of class cls, or of a class under it, have a
   respond_to_missing?, which a capture asks where it finds no method?) */
static int capture_hook_reach(Compiler *c, int cls) {
  return comp_name_responder(c, cls, RESPOND_TO_MISSING_NAME, 0) >= 0 || comp_subclass_answers(c, cls, RESPOND_TO_MISSING_NAME);
}
/* (the method a guard entry e's method_missing else arm stands for: the
   one behind its mixins, guard_entry_make; -1 for none) */
static int cplan_entry_behind(const Compiler *c, int e) {
  for (int i = 0; e >= 0 && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e) return c->ver_entries[i].behind - 1;
  return -1;
}
/* (entry e's else arm, which it answers itself, as a Method capture cap
   makes holds it: cplan_method_arms) */
static int capture_else_arm(Compiler *c, int e, int cap) {
  int mw = cplan_capture_missing(c, cap), bh = cplan_entry_behind(c, e);
  if (mw >= 0) return mw;
  if (cap < 0 || c->scopes[e].is_cmethod || bh < 0) return e;
  return capture_hook_reach(c, c->scopes[e].class_id) ? bh : CPLAN_ARM_NONE;
}
/* Is e the entry of a late layer (pos_entry)? */
static int ver_is_layer_entry(const Compiler *c, int e) {
  for (int i = 0; i < c->nver_entries; i++)
    if (c->ver_entries[i].guard == VER_ENTRY_LAYER && c->ver_entries[i].scope == e) return 1;
  return 0;
}
/* The arms of entry e (ver_entry_targets), where an arm is the entry of the layer behind a late layer (its else arm)
   replaced by that entry's arms in turn, nearest first: the lookup passes one late layer to the next, so a Method of
   the entry holds the versions of each, in order. At most lim of them. */
static int ver_layer_arms(const Compiler *c, int e, int *out, int *tests, int lim) {
  int sub[VER_MAX_ARMS + 1], st[VER_MAX_ARMS + 1], n = 0, ns = ver_entry_targets(c, e, sub, st);
  for (int j = 0; j < ns && n < lim; j++) {
    if (sub[j] >= 0 && lim - n > 1 && ver_is_layer_entry(c, sub[j])) n += ver_layer_arms(c, sub[j], out + n, tests + n, lim - n);
    else out[n] = sub[j], tests[n++] = st[j];
  }
  return n;
}
/* ...as a Method made of entry e holds them, each the method that runs
   at the bind: an arm that is itself the entry over a mixin's versions
   (pos_entry) by its own arms, under its test and theirs, and
   a later include's behind (Scope.super_behind) by what its super
   reaches, and an else arm the entry answers itself (a method_missing
   call: guard_entry_make) by the method_missing Method's target that
   capture cap (the maker the Method came from) binds where it finds no
   method (cplan_capture_missing). An instance method's capture that binds
   none finds no method there where no respond_to_missing? can answer
   (CPLAN_ARM_NONE: CRuby's NameError, as with no entry), and holds the
   method behind the mixins the else arm stands for where one may answer
   (cplan_entry_behind): the Method such a capture makes with no entry,
   whose respond_to_missing? the plan does not ask. Where no capture is
   known (cap -1), or on the class side, the arm is the entry. At most
   2 * (VER_MAX_ARMS + 1) of them. */
int cplan_method_arms(Compiler *c, int e, int cap, int *out, int *tests) {
  int to[VER_MAX_ARMS + 1], t[VER_MAX_ARMS + 1], n = ver_entry_targets(c, e, to, t), m = 0;
  for (int k = 0; k < n; k++) {
    int sub[VER_MAX_ARMS + 1], st[VER_MAX_ARMS + 1], ns = 0;
    const struct VerEntry *history = to[k] >= 0 ? guard_history_of(c, to[k]) : NULL;
    if (history) ns = ver_entry_targets(c, to[k], sub, st);
    else if (to[k] >= 0 && c->scopes[to[k]].entry == ENTRY_DISPATCH) ns = ver_layer_arms(c, to[k], sub, st, VER_MAX_ARMS + 1);
    if (history && ns) sub[ns - 1] = history->behind - 1;
    for (int j = 0; j < ns && m < 2 * (VER_MAX_ARMS + 1); j++, m++) {
      if (tests) tests[2 * m] = t[k], tests[2 * m + 1] = st[j];
      if (history && sub[j] < 0 && ver_arm_undef(c, to[k], j)) {
        int missing = cplan_capture_missing(c, cap);
        out[m] = missing >= 0 ? missing : CPLAN_ARM_NONE;
      } else out[m] = sub[j];
    }
    if (ns || m >= 2 * (VER_MAX_ARMS + 1)) continue;
    int sup = to[k] >= 0 && c->scopes[to[k]].super_behind ? cplan_super_target(c, -1, &c->scopes[to[k]], NULL) : -1;
    if (tests) tests[2 * m] = t[k], tests[2 * m + 1] = -1;
    out[m++] = sup >= 0 ? sup : to[k] >= 0 ? to[k] : ver_arm_undef(c, e, k) && cap >= 0 ? CPLAN_ARM_NONE : capture_else_arm(c, e, cap);
  }
  return m;
}
/* See call_plan.h. */
int cplan_method_held(Compiler *c, int e, int cap, int *out) {
  int guard = cplan_bam_string_guard(c, cap, e);
  int n = cplan_method_arms(c, guard >= 0 ? guard : e, cap, out, NULL), m = 0;
  for (int k = 0; k < n; k++)
    if (out[k] != CPLAN_ARM_NONE) out[m++] = out[k];
  if ((!m || guard >= 0) && m < 2 * (VER_MAX_ARMS + 1)) out[m++] = e;
  return m;
}
typedef struct { int guard, cls, sg, mi, ar; const char *name; int from, face; } VerNeed;   /* (from: VerEntry.from; face: VerEntry.face) */
/* The part of an ask that a class's chain decides, not the call: whether
   the chain holds two versions, the method the table answers, and the
   arity of the entry, for (class, flags, name, the call's plain argument
   count, ver_call_n) of an ask not on main (CT_TOP, which reads the call's
   own place). A call of a name that no type bounds yet asks every class
   holding two versions of it, so each was redone per call and class:
   classes x calls (#10546's an_pre_fixpoint). ok 0: no entry is asked.
   from: the copies' entry's VerEntry.from where mi is a module's versioned
   copy (mixin_copies_module), 0 where it is none, -1 until asked
   (ver_pre_needed). */
typedef struct { int cls; unsigned flags; const char *name; int n; int ok, mi, sg, ar, from; } VerPre;
/* The survey's state for one build (ver_entries_build): the entries the
   calls need, an open addressing table over them to tell the asks that
   repeat one (ix holds an index into v, -1 free), and the asks' memo
   (VerPre, pre: nbpre slots, npre used) */
typedef struct { VerNeed *v; int n; int *ix; unsigned nb; VerPre *pre; int npre; unsigned nbpre; } VerNeeds;
static int ver_need_has(const Compiler *c, const VerNeeds *o, VerNeed w) {
  if (ver_entry_ix(c, w.guard, w.from, w.cls, w.sg, w.name, w.ar, w.face) >= 0) return 1;
  unsigned at = o->nb ? ver_key_hash(w.cls, w.sg, w.name, w.ar) & (o->nb - 1) : 0;
  for (; o->nb && o->ix[at] >= 0; at = (at + 1) & (o->nb - 1)) {
    const VerNeed *v = &o->v[o->ix[at]];
    if (v->guard == w.guard && v->from == w.from && v->cls == w.cls && v->sg == w.sg && v->ar == w.ar && v->face == w.face && sp_streq(v->name, w.name)) return 1;
  }
  return 0;
}
static void ver_need_add(VerNeeds *o, VerNeed w) {
  unsigned at;
  if (!(o->n & (o->n - 1)) && !(o->v = realloc(o->v, sizeof *o->v * (size_t)(o->n ? 2 * o->n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  o->v[o->n++] = w;
  if (o->n * 2 > (int)o->nb) {   /* the table grows with the asks: rebuilt over all of them */
    o->nb = o->nb ? o->nb * 2 : 64;
    free(o->ix);
    if (!(o->ix = malloc(sizeof(int) * o->nb))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (unsigned b = 0; b < o->nb; b++) o->ix[b] = -1;
    for (int i = 0; i < o->n; i++) {
      unsigned b = ver_key_hash(o->v[i].cls, o->v[i].sg, o->v[i].name, o->v[i].ar) & (o->nb - 1);
      while (o->ix[b] >= 0) b = (b + 1) & (o->nb - 1);
      o->ix[b] = i;
    }
  }
  else {
    for (at = ver_key_hash(w.cls, w.sg, w.name, w.ar) & (o->nb - 1); o->ix[at] >= 0; at = (at + 1) & (o->nb - 1)) {}
    o->ix[at] = o->n - 1;
  }
}
static void ver_need_front(Compiler *c, VerNeeds *o, int id, int mi) {
  int to[VER_MAX_ARMS], n = pos_runs_front(c, mi, to);
  const Scope *m = &c->scopes[mi];
  VerNeed w = { VER_ENTRY_LAYER, m->class_id, m->is_cmethod, mi, ver_arity(c, id, to, n), comp_scope_user_name(m), mi + 1 };
  if (!ver_need_has(c, o, w)) ver_need_add(o, w);
  /* (and its face entry, for a call that enforces visibility where a version's visibility the run tells: ct_front_layer) */
  if ((w.face = ct_layer_face(c, id, m->class_id, m->is_cmethod, w.name, 0)) && !ver_need_has(c, o, w)) ver_need_add(o, w);
}
static void ver_need_guard(Compiler *c, VerNeeds *o, int id, int mi) {
  const Scope *m = &c->scopes[mi];
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, NULL, NULL);
  VerNeed w = { VER_ENTRY_GUARD, m->class_id, m->is_cmethod, mi, guard_arity(c, id, to, n), comp_scope_user_name(m) };
  if (!ver_need_has(c, o, w)) ver_need_add(o, w);
  if (guard_needs_face(c, to, n)) {
    w.face = guard_call_face(c, id, to, n);
    if (!ver_need_has(c, o, w)) ver_need_add(o, w);
    w.face = VF_PUBLIC;
    if (!ver_need_has(c, o, w)) ver_need_add(o, w);
  }
}
static unsigned ver_pre_hash(int k, unsigned flags, const char *nm, int n) {
  return ver_mix(sp_strhash(nm) * 31u + (unsigned)(k + 1) * 131u + flags * 7u + (unsigned)(n + 1));
}
static VerPre *ver_pre(Compiler *c, VerNeeds *o, int id, const char *nm, int k, unsigned flags) {
  ElabVersion vs[VER_MAX_ARMS];
  int n = ver_call_n(c, id);
  if (o->npre * 2 >= (int)o->nbpre) {
    unsigned onb = o->nbpre; VerPre *op = o->pre;
    o->nbpre = onb ? onb * 2 : 256;
    if (!(o->pre = calloc(o->nbpre, sizeof *o->pre))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (unsigned b = 0; b < onb; b++) {
      if (!op[b].name) continue;
      unsigned at = ver_pre_hash(op[b].cls, op[b].flags, op[b].name, op[b].n) & (o->nbpre - 1);
      while (o->pre[at].name) at = (at + 1) & (o->nbpre - 1);
      o->pre[at] = op[b];
    }
    free(op);
  }
  unsigned at = ver_pre_hash(k, flags, nm, n) & (o->nbpre - 1);
  for (; o->pre[at].name; at = (at + 1) & (o->nbpre - 1)) {
    VerPre *w = &o->pre[at];
    if (w->cls == k && w->flags == flags && w->n == n && sp_streq(w->name, nm)) return w;
  }
  VerPre p = { k, flags, nm, n, 0, -1, 0, 0, -1 };
  unsigned fl = flags;
  int nv = elab_chain_versions(c, k, (flags & CT_CMETHOD) != 0, nm, vs, VER_MAX_ARMS);
  /* (or one, for a call its face entry may close: ct_face_for) */
  if ((nv >= 2 || (nv == 1 && (flags & CT_PUBLIC) && (ver_vis_runs(c, k, (flags & CT_CMETHOD) != 0, nm) ||
                                                          ver_lone_closed_late(c, k, (flags & CT_CMETHOD) != 0, nm, ct_flags_face(flags))))) &&
      (p.mi = ct_lookup(c, id, k, nm, &fl, NULL)) >= 0 && !c->scopes[p.mi].mixin_guarded && ct_versioned(c, p.mi, nm)) {
    p.ok = 1;
    p.sg = (fl & CT_CMETHOD) != 0;
    p.ar = ver_call_arity(c, id, k, p.sg, nm, p.mi);
  }
  o->pre[at] = p;
  o->npre++;
  return &o->pre[at];
}
/* Does the survey already need every entry an ask of p may add: the
   chain's entry of its (class, side, name, arity) and, where the table's
   method is a module's versioned copy, the copies' entry (VerPre.from)?
   An ask answers only which of the two its call needs (ct_version's n and
   copies), so one whose answers are all needed adds nothing and is not
   asked. */
static int ver_pre_needed(Compiler *c, VerNeeds *o, VerPre *p, int face) {
  VerNeed w = { VER_ENTRY_CHAIN, p->cls, p->sg, p->mi, p->ar, p->name, 0, face };
  if (!ver_need_has(c, o, w)) return 0;
  if (p->from < 0) {
    ElabVersion vs[VER_MAX_ARMS];
    int to[VER_MAX_ARMS], nc = 0;
    p->from = mixin_copies_module(c, p->mi, to, vs, &nc) + 1;
  }
  w.from = p->from;
  return !w.from || ver_need_has(c, o, w);
}
static void ver_need(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  ElabVersion vs[VER_MAX_ARMS];
  int mi, ar, rcls = flags & CT_TOP ? -1 : k, sg, face = VF_NONE;
  MethodLookup ml = { .survey = 1, .def_class = -1 };
  int asked = 1;
  /* (a call that enforces visibility binds the face entry where the
     versions differ in it: call_target asks with the same flags) */
  flags = ct_vis_flags(c, id, nm, flags);
  mtab_enter(c);
  /* (an implied call takes no entry where an undef of the name is on the
     chain: cplan_implied_target; the plan's own read, in its bracket) */
  if ((flags & CT_IMPLIED) && k >= 0 && comp_is_undeffed_in_chain(c, k, nm) &&
      !kernel_string_implies(c, id, k, nm, flags)) {
    mtab_leave(c);
    return;
  }
  /* (an entry arms two or more versions of the chain: with fewer, or a
     chain the record does not hold, no call is asked. A call on main may
     ask main's chain, which ends in Object's: ct_lookup. A later prepend's
     copy has its guard entries: ver_entries_build) */
  if (!(flags & CT_TOP)) {
    VerPre *p = ver_pre(c, arg, id, nm, k, flags);
    face = p->ok ? ct_call_face(c, id, k, p->sg, nm, flags, 0) : VF_NONE;
    asked = p->ok && !ver_pre_needed(c, arg, p, face);
    mi = p->mi; sg = p->sg; ar = p->ar;
  }
  else if (elab_chain_versions(c, rcls, 1, nm, vs, VER_MAX_ARMS) < 2 ||
           (mi = ct_lookup(c, id, k, nm, &flags, NULL)) < 0 || c->scopes[mi].mixin_guarded || !ct_versioned(c, mi, nm)) asked = 0;
  else {
    sg = (flags & CT_CMETHOD) != 0;
    rcls = ct_top_chain(c, nm, sg);
    ar = ver_call_arity(c, id, rcls, sg, nm, mi);
    /* (main's chain has no visibility face entry; a variable-like call's
       where it holds an undef) */
    if (ct_call_face(c, id, rcls, sg, nm, flags, 0) == VF_VCALL) face = VF_VCALL;
  }
  if (asked) {
    ct_version(c, id, k, nm, flags, mi, &ml);
    /* (the copies' entry over the module's versions: mixin_copies_module) */
    int to[VER_MAX_ARMS], nc = 0;
    VerNeed w = { VER_ENTRY_CHAIN, rcls, sg, mi, ar, nm, ml.copies ? mixin_copies_module(c, mi, to, vs, &nc) + 1 : 0, face };
    if (ml.n >= 2 && !ver_need_has(c, arg, w)) ver_need_add(arg, w);
  }
  mtab_leave(c);
}
/* The entries the calls need, made once the passes after the record have
   made their methods (an_phase_pre_fixpoint, before the scope index
   freezes): every call is asked first, with the record as it stands, and
   the entries are made after, as each adds nodes the record would be
   rebuilt for. A later prepend's copy has a guard entry for each call of
   its name, whatever its receiver (ct_guard). */
/* The entries by name, once they are all made: ver_ix chains those of one
   (class, side, name, parameter count), ver_names those that hold a method,
   one to a name. Asked for every call of a versioned name, where a scan of
   the entries was a pass over them for each. Guard entries also keep each
   copy's class in the name chain, for ct_version_classes. */
static void ver_entries_index(Compiler *c) {
  unsigned nb = 16;
  free(c->ver_ix); free(c->ver_ix_next); free(c->ver_names); free(c->ver_names_next);
  while (nb < 2u * (unsigned)c->nver_entries) nb *= 2;
  c->ver_ix_nb = nb;
  c->ver_ix = malloc(sizeof(int) * nb);
  c->ver_names = malloc(sizeof(int) * nb);
  c->ver_ix_next = malloc(sizeof(int) * ((size_t)c->nver_entries + 1));
  c->ver_names_next = malloc(sizeof(int) * ((size_t)c->nver_entries + 1));
  if (!c->ver_ix || !c->ver_names || !c->ver_ix_next || !c->ver_names_next) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (unsigned b = 0; b < nb; b++) c->ver_ix[b] = c->ver_names[b] = -1;
  for (int i = c->nver_entries - 1; i >= 0; i--) {   /* (backwards: a chain answers the first entry first) */
    const struct VerEntry *e = &c->ver_entries[i];
    unsigned b = ver_key_hash(e->cls, e->sg, e->name, e->ar) % nb, m = sp_strhash(e->name) % nb;
    c->ver_ix_next[i] = c->ver_ix[b];
    c->ver_ix[b] = i;
    c->ver_names_next[i] = -1;
    /* (an open history's entry is the table's answer; so is the entry of a late layer in front of a chain: pos_front. The
       entry of a layer behind others, which a guard's arm or a super reaches, none) */
    int front = e->guard == VER_ENTRY_LAYER && !c->scopes[e->from - 1].mixin_guarded && !c->scopes[e->from - 1].shadow;
    if (e->scope < 0 || e->guard == VER_ENTRY_HISTORY || (e->guard == VER_ENTRY_LAYER && (!front || e->face != VF_NONE)) ||
        (e->guard == VER_ENTRY_GUARD && e->from)) continue;
    int dup = 0;
    for (int j = c->ver_names[m]; j >= 0 && !dup; j = c->ver_names_next[j]) {
      const struct VerEntry *v = &c->ver_entries[j];
      dup = sp_streq(v->name, e->name) && (e->guard == VER_ENTRY_CHAIN || (v->guard != VER_ENTRY_CHAIN && v->cls == e->cls && v->sg == e->sg));
    }
    if (!dup) { c->ver_names_next[i] = c->ver_names[m]; c->ver_names[m] = i; }
  }
}
/* The facts the open histories' asks read (VerNamedAsks: sup, vc, rn),
   read from the tree as it stands; ver_named_facts_free frees them */
static void ver_named_facts(Compiler *c, VerNamedAsks *q, int demand) {
  const NodeTable *nt = c->nt;
  unsigned char *sup = calloc((size_t)c->nscopes + 1, 1);
  if (!sup) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int id = 0, n0 = nt->count; id < n0; id++) {   /* (the methods a super reaches, asked once a super) */
    NodeKind k = nt_kind(nt, id);
    Scope *s = (k == NK_SuperNode || k == NK_ForwardingSuperNode) ? comp_scope_of(c, id) : NULL;
    int t = s && s->class_id >= 0 && s->name ? cplan_super_target(c, id, s, NULL) : -1;
    if (t >= 0 && t < c->nscopes) sup[t] = 1;
  }
  const char **vc = NULL;
  int nvc = 0;
  const char **rn = NULL;
  int nrn = 0;
  for (int id = 0, n0 = nt->count; id < n0; id++) {   /* (the names bare calls name, and those VerNamedAsks.rn holds) */
    NodeKind k = nt_kind(nt, id);
    const char *cn[2];
    int nn = call_node_names(c, id, cn), ac = 0, sym = k == NK_AliasMethodNode ? nt_ref(nt, id, "old_name") : k == NK_BlockArgumentNode ? nt_ref(nt, id, "expression") : -1;
    for (int i = 0; i < nn; i++) ver_name_add(&rn, &nrn, cn[i]);
    IcCall ib[16];
    int ni = 0;
    IcCall *ic = call_node_implied_names(c, id, ib, 16, &ni);
    for (int i = 0; i < ni; i++) ver_name_add(&rn, &nrn, ic[i].name);
    if (ic != ib) free(ic);
    if (sym >= 0 && nt_kind(nt, sym) == NK_SymbolNode) ver_name_add(&rn, &nrn, nt_str(nt, sym, "value"));
    if (k != NK_CallNode || !nn || !cn[0]) continue;
    if (nt_vcall(nt, id)) ver_name_add(&vc, &nvc, cn[0]);
    const int *av = call_args(nt, id, &ac);
    int pq = is_method_presence_query(cn[0]) == 1 && !is_method_object_maker(cn[0]);
    for (int i = 0; i < ac && !is_define_method_name(cn[0]) && !pq; i++)
      ver_name_add(&rn, &nrn, nt_kind(nt, av[i]) == NK_SymbolNode ? nt_str(nt, av[i], "value") :
                              nt_kind(nt, av[i]) == NK_StringNode ? nt_str(nt, av[i], "content") : NULL);
  }
  if (nvc) qsort(vc, (size_t)nvc, sizeof *vc, ver_name_cmp);
  if (nrn) qsort(rn, (size_t)nrn, sizeof *rn, ver_name_cmp);
  *q = (VerNamedAsks){ c, NULL, 0, sup, c->nscopes, vc, nvc, rn, nrn, demand, NULL, NULL, NULL, NULL, NULL, 0, NULL, 0 };
}
static void ver_named_facts_free(VerNamedAsks *q) {
  free(q->v); free(q->mh); free(q->mn); free(q->mk); free(q->seen); free(q->cps); free(q->bydef);
  free((void *)q->sup); free(q->vc); free(q->rn);
}
/* Asks every open history (elab_open_histories) and makes the entries of
   those a reader names: at the build, each a call names (the others are
   deferred, Compiler.ver_defer); with demand, the deferred ones taken */
static void ver_named_asks(Compiler *c, int demand) {
  VerNamedAsks q;
  ver_named_facts(c, &q, demand);
  elab_open_histories(c, ver_named_one, &q);
  /* A guaranteed own definition still has an initial builtin while a
     prepended method calls super before the definition runs. */
  int sci = comp_core_class(c, CORE_STRING);
  if (!demand && sci >= 0)
    for (int s = 0; s < c->nscopes; s++) {
      const Scope *m = &c->scopes[s];
      if (m->class_id != sci || m->is_cmethod || !m->mixin_guarded || m->guard_behind < 0) continue;
      const Scope *behind = &c->scopes[m->guard_behind];
      if (behind->shadow != SHADOW_PREP || behind->origin_module_ci) continue;
      const char *name = comp_scope_user_name(m);
      if (!is_string_boxed_reader(name) || elab_history_open(c, sci, 0, name)) continue;
      int seen = 0;
      for (int i = 0; i < q.n && !seen; i++)
        seen = q.v[i].cls == sci && !q.v[i].sg && sp_streq(q.v[i].name, name);
      if (!seen) ver_named_one(&q, sci, 0, name);
    }
  for (int i = 0; i < q.n; i++) if (!q.v[i].atdef) ver_named_make(c, &q.v[i]);
  for (int i = 0; i < q.n; i++) if (q.v[i].atdef) ver_named_refuse_defs(c, &q.v[i]);
  ver_named_facts_free(&q);
}
/* Does nothing answer `name` on cls's side sg where a guard entry's else
   arm runs behind: that method finds none (guard_behind_finds_none), and
   no method_missing or respond_to_missing? may (VerEntry.finds)? *hooks:
   whether the program defines either, asked once per build (-1 not yet:
   each is a walk of the program) */
static int guard_else_bare(Compiler *c, int behind, int cls, int sg, const char *name, int *hooks) {
  if (!guard_behind_finds_none(c, behind, cls, sg, name)) return 0;
  if (*hooks < 0) *hooks = an_user_defines_method(c, METHOD_MISSING_NAME) || respond_to_missing_defined(c);
  return !*hooks;
}
static void list_ver_need(Compiler *c, int id, const char *name, int cls, void *arg) { ver_need(c, id, name, cls, CT_CMETHOD, arg); }
static void ver_need_history_vis(Compiler *c, int cls, const char *name);
static void pres_target(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  MethodLookup ml = { .def_class = -1 };
  (void)arg;
  int e = call_target(c, id, k, nm, flags & CT_CMETHOD ? CT_CMETHOD | CT_CAPTURE : flags | CT_REACH, &ml);   /* (a class arm's: cplan_cmethod_presence) */
  /* (a module's own method whose visibility a body's word sets: its chain's cells, pres_module_cells) */
  if (!(flags & (CT_CMETHOD | CT_TOP)) && pres_module_cells(c, id, e, &ml, k, nm)) ver_need_history_vis(c, k, nm);
}
static void list_ver_target(Compiler *c, int id, const char *name, int cls, void *arg) {
  (void)arg;
  call_target(c, id, cls, name, CT_CMETHOD | CT_CAPTURE, NULL);
}
/* The twin of a conditional def's dispatcher (desugar_conditional_defs)
   whose else arm raises: the same arms, its else arm the NameError a
   variable-like call raises (ver_no_method_raise), the method such a call
   binds (call_target): the dispatcher's class's VF_VCALL entry, as a
   version entry's. Made where a variable-like call names the dispatcher. */
static void ver_vcall_twins(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char **vn = NULL;
  int nvn = 0, any = 0;
  for (int d = 0; d < c->nscopes && !any; d++) any = c->scopes[d].def_node >= 0 && nt_int(nt, c->scopes[d].def_node, "cond_raise_els", 0);
  if (!any) return;
  /* (the names variable-like calls call, sorted: one walk of the calls) */
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (!nt_vcall(nt, id) || !nt_str(nt, id, "name")) continue;
    if (!(nvn & (nvn - 1)) && !(vn = realloc(vn, sizeof *vn * (size_t)(nvn ? 2 * nvn : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    vn[nvn++] = nt_str(nt, id, "name");
  }
  if (nvn) qsort(vn, (size_t)nvn, sizeof *vn, ver_name_cmp);
  for (int d = 0, n0 = c->nscopes; d < n0; d++) {
    int dn = c->scopes[d].def_node;
    const char *name = c->scopes[d].name;
    if (dn < 0 || !name || !nt_int(nt, dn, "cond_raise_els", 0) || !nvn || !bsearch(&name, vn, (size_t)nvn, sizeof *vn, ver_name_cmp)) continue;
    int def = nt_clone_subtree(nt, dn), st = ver_first_stmt(nt, nt_ref(nt, def, "body"));
    while (st >= 0 && nt_kind(nt, st) == NK_IfNode) st = nt_ref(nt, st, "subsequent");
    if (st < 0 || nt_kind(nt, st) != NK_ElseNode) continue;
    int raise = ver_no_method_raise(c, c->scopes[d].class_id, c->scopes[d].is_cmethod, name, 0, 1);
    nt_node_set_arr(nt, nt_ref(nt, st, "statements"), "body", &raise, 1);
    nt_node_set_int(nt, def, "cond_lone_disp", 0);   /* (the dispatcher's own facts stay its own) */
    nt_node_set_int(nt, def, "cond_first_arm", 0);
    nt_node_set_int(nt, def, "cond_raise_els", 0);
    nt_node_set_int(nt, def, "cond_twin", 1);   /* (its arms test the dispatcher's selector: no cell of its own, ver_cells_put) */
    /* (a class method's: one of a `class << self` body, a module_function's
       copy, which the clone's own def does not say) */
    if (c->scopes[d].is_cmethod && nt_ref(nt, def, "receiver") < 0) nt_node_set_ref(nt, def, "receiver", nt_new_node(nt, "SelfNode"));
    int e = c->nscopes;
    comp_grow_node_arrays(c);
    walk_scope(c, def, c->nscope[dn], c->scopes[d].class_id);
    c->scopes[e].entry = ENTRY_DISPATCH;
    comp_touch_scope(c->scopes[e].idx);
    ver_entry_add(c, name, (struct VerEntry){ NULL, VER_ENTRY_CHAIN, c->scopes[d].class_id, c->scopes[d].is_cmethod, 0, e, .face = VF_VCALL });
    c->nvcall_twins++;
  }
  free(vn);
}
/* Put each call ct_main_builtin_undef answers behind the cells of the
   undefs on main's chain (disp_cell_guard): its operands that run code
   are held first, in CRuby's order (sd_hold_call_operands: ahead of the
   lookup, as CRuby evaluates them), then `raise ... if <an undef ran>`,
   then the call over the held operands: ahead of it in its statement
   list, or `(<those>; <the call>)` where it is a value (disp_ahead), and
   in an interpolation's part, whose statements the emitter runs after
   the setup of the part's value. The raise is the one an entry's undef arm raises (ver_no_method_raise:
   NameError for a variable-like call). The call itself keeps the
   builtin's binding and lowering. A call that is the receiver of another
   is guarded with that one, outermost: the receiver runs first, and a
   lowering that reads its receiver's call (`binding.x`) still sees it.
   Not a call under defined?, which answers for the call itself. The calls
   are found with one parent map and guarded deepest first, so a call in
   another's operands is guarded before that one holds it. Each name's
   cells are written as an entry's chain's (Compiler.ver_guards,
   ver_cells_put). */
typedef struct { int id, top, depth; } VerGuardAt;
static int ver_guard_at_cmp(const void *a, const void *b) {
  const VerGuardAt *x = a, *y = b;
  return x->depth != y->depth ? y->depth - x->depth : x->id - y->id;
}
static void ver_need_history_guard(Compiler *c, int cls, const char *name) {
  int k = 0;
  while (k < c->nver_guards && !(c->ver_guards[k].cls == cls && sp_streq(c->ver_guards[k].name, name))) k++;
  if (k < c->nver_guards) return;
  if (!(k & (k - 1)) && !(c->ver_guards = realloc(c->ver_guards, sizeof *c->ver_guards * (size_t)(k ? 2 * k : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  c->ver_guards[c->nver_guards++] = (struct VerGuard){ name, cls, 0, 0 };
}
/* ...and one whose versions' visibility cells a module's own presence reader reads too (VerGuard.vis) */
static void ver_need_history_vis(Compiler *c, int cls, const char *name) {
  ver_need_history_guard(c, cls, name);
  for (int k = 0; k < c->nver_guards; k++)
    if (c->ver_guards[k].cls == cls && sp_streq(c->ver_guards[k].name, name)) c->ver_guards[k].vis = 1;
}
/* See call_plan.h. An undef in this class's own history stops lookup
   only when no prepend answers ahead of it. A non-undef in the same
   history can restore a Ruby method or reveal one behind it. */
int cplan_string_undef_guard(Compiler *c, int id, const char *name) {
  int cls = comp_core_class(c, CORE_STRING);
  if (!builtin_owns_instance_method("String", name)) return -1;
  ElabVersion own[VER_MAX_ARMS], chain[VER_MAX_ARMS];
  int nv = elab_history_versions(c, cls, 0, name, own, VER_MAX_ARMS);
  if (nv <= 0) return -1;
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  for (int i = 0; i < nv; i++) {
    const LrDef *v = own[i].v;
    if (v->may == LR_MAY_IN || v->body || v->kind != LR_UNDEF) return -1;
    ver_cell_name(own[i].cell, sel[i]);
    arm[i] = (DispArm){ sel[i], own[i].n, name, own[i].cell + 1, 0 };
  }
  int nc = elab_chain_versions(c, cls, 0, name, chain, VER_MAX_ARMS);
  if (nc != nv || chain[0].hist != own[0].hist || call_answers(c, id, cls, name, 0, NULL)) return -1;
  ver_need_history_guard(c, cls, name);
  return disp_cell_any((NodeTable *)c->nt, arm, nv);
}

int cplan_string_hook_guard(Compiler *c, int id, int hook) {
  int cls = comp_core_class(c, CORE_STRING);
  if (hook < 0 || c->scopes[hook].class_id != cls) return -1;
  const char *selector = cplan_conditional_entry_selector(c, hook);
  if (selector) {
    const NodeTable *nt = c->nt;
    int def = c->scopes[hook].def_node, n = 0;
    DispArm arms[VER_MAX_ARMS];
    for (int a = nt_int(nt, def, "cond_first_arm", 0); a > 0; a = nt_int(nt, a, "cond_next_arm", 0)) {
      if (n == VER_MAX_ARMS) return -1;
      arms[n] = (DispArm){ selector, n + 1, METHOD_MISSING_NAME, 0, 0 };
      n++;
    }
    return n ? disp_cell_any((NodeTable *)nt, arms, n) : -1;
  }
  int framed = elab_plan_enter(c, id);
  const LrDef *d = NULL;
  int placed = elab_method_at(c, cls, METHOD_MISSING_NAME, 0, elab_point_of(c, id), &d);
  if (framed) elab_plan_leave(c);
  if (placed == ELAB_YES && cplan_runs_once(c, id)) return -1;
  /* A single conditional def has one callable implementation. The chain
     must otherwise be empty so a clear cell means the default hook. */
  ElabVersion v[1];
  if (elab_chain_versions(c, cls, 0, METHOD_MISSING_NAME, v, 1) != 1 ||
      v[0].v->kind != LR_DEF || !v[0].v->may || v[0].v->may == LR_MAY_IN || v[0].v->body) return -1;
  ElabVersion own[1];
  if (elab_history_versions(c, cls, 0, METHOD_MISSING_NAME, own, 1) != 1 || own[0].hist != v[0].hist) return -1;
  char sel[32];
  ver_cell_name(v[0].cell, sel);
  DispArm arm = { sel, v[0].n, METHOD_MISSING_NAME, v[0].cell + 1, 0 };
  ver_need_history_guard(c, cls, METHOD_MISSING_NAME);
  return disp_cell_any((NodeTable *)c->nt, &arm, 1);
}
static void ver_main_undef_guards(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int *par = NULL, nat = 0;
  VerGuardAt *at = NULL;
  for (int id = 0, n0 = nt->count; id < n0; id++) {
    const char *name = nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
    if (!name || !ct_main_builtin_undef(c, id, name)) continue;
    if (!par && !(par = du_parent_map(nt))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    /* (No two such calls share a top: one runs on main, receiverless or on
       self (ct_runs_on_main), so it is the innermost call of its receiver
       chain, and a chain holds one. A guard replaces only its own top in
       that top's parent, the parent's other children staying where they
       are, and a call in another's operands is the deeper, guarded first,
       so the map still holds for every top after it.) */
    int top = id, depth = 0;
    while (par[top] >= 0 && nt_kind(nt, par[top]) == NK_CallNode && nt_ref(nt, par[top], "receiver") == top) top = par[top];
    if (par[top] < 0 || nt_kind(nt, par[top]) == NK_DefinedNode) continue;
    for (int a = id; a >= 0; a = par[a]) depth++;
    if (!(nat & (nat - 1)) && !(at = realloc(at, sizeof *at * (size_t)(nat ? 2 * nat : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    at[nat++] = (VerGuardAt){ id, top, depth };
  }
  if (nat) qsort(at, (size_t)nat, sizeof *at, ver_guard_at_cmp);
  for (int g = 0; g < nat; g++) {
    int id = at[g].id, top = at[g].top, na = 0, *pre = NULL, np = sd_hold_call_operands(c, id, &pre, 0);
    const char *name = nt_str(nt, id, "name");
    if (np < 0) { free(pre); continue; }   /* (left as the table answers it) */
    for (int i = 0, nv = elab_chain_versions(c, -1, 1, name, vs, VER_MAX_ARMS); i < nv; i++) {
      if (vs[i].v->kind != LR_UNDEF) continue;
      ver_cell_name(vs[i].cell, sel[na]);
      arm[na] = (DispArm){ sel[na], vs[i].n, name, vs[i].cell + 1, 0 };
      na++;
    }
    pre[np++] = disp_cell_guard(nt, arm, na, ver_no_method_raise(c, -1, 1, name, 0, nt_vcall(nt, id)));
    int pl = par[top], in_list = nt_kind(nt, pl) == NK_StatementsNode && par[pl] >= 0 && nt_kind(nt, par[pl]) != NK_EmbeddedStatementsNode;
    if (in_list) for (int k = 0; k < np; k++) ver_list_put(nt, pl, top, 0, pre[k]);
    else nt_replace_child(nt, pl, top, disp_ahead(nt, pre, np, top));
    free(pre);
    ver_need_history_guard(c, -1, name);
  }
  free(at);
  free(par);
}
static int ver_cells_reader(Compiler *c, int id, int written, const char **qm, int *mask);
void ver_entries_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  const char *names[2];
  VerNeeds o = { NULL, 0, NULL, 0, NULL, 0, 0 };
  int *gc = NULL, ngc = 0;
  for (int s = 0; s < c->nscopes; s++) {   /* the later prepends' copies */
    if (!c->scopes[s].mixin_guarded) continue;
    const Scope *m = &c->scopes[s];
    /* (a module's own guarded copy, of a late include into a reopened
       module, is emitted only as its includers' clones, which carry the
       guard and get entries of their own: one on the module would call a
       copy that no function is emitted for; a singleton prepend's copy is
       the module's own class method, called on the module itself) */
    if (m->class_id >= 0 && !m->is_cmethod && comp_class_is_module(c, &c->classes[m->class_id])) continue;
    const char *name = comp_scope_user_name(m);
    mtab_enter(c);   /* (entry construction: the guarded copy is the table's last of its name) */
    int last = m->is_cmethod ? comp_cmethod_in_class(c, m->class_id, name) : comp_method_in_class(c, m->class_id, name);
    mtab_leave(c);
    if (last != s) continue;
    if (!(ngc & (ngc - 1)) && !(gc = realloc(gc, sizeof *gc * (size_t)(ngc ? 2 * ngc : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    gc[ngc++] = s;
    ver_need_guard(c, &o, -1, s);
  }
  /* (and the late layers in front of a class's chain, which its table answers: pos_front) */
  int *fl = NULL, nfl = 0;
  for (int s = 0; s < c->nscopes; s++) {
    const Scope *m = &c->scopes[s];
    if (!pos_front(c, s)) continue;
    if (m->class_id >= 0 && !m->is_cmethod && comp_class_is_module(c, &c->classes[m->class_id])) continue;
    const char *name = comp_scope_user_name(m);
    mtab_enter(c);
    int last = m->is_cmethod ? comp_cmethod_in_class(c, m->class_id, name) : comp_method_in_class(c, m->class_id, name);
    mtab_leave(c);
    if (last != s) continue;
    if (!(nfl & (nfl - 1)) && !(fl = realloc(fl, sizeof *fl * (size_t)(nfl ? 2 * nfl : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    fl[nfl++] = s;
  }
  ver_main_undef_guards(c);
  /* the open histories' entries first: the table answers them, and no call
     of their names asks an entry of the chain behind them */
  ver_named_asks(c, 0);
  if (c->nver_defer) qsort(c->ver_defer, (size_t)c->nver_defer, sizeof *c->ver_defer, ver_name_cmp);
  /* (the asks below make and rename no scope: Compiler.ver_asking) */
  comp_ver_asking(c, 1);
  for (int id = 0, n0 = nt->count; id < n0; id++) {
    /* a respond_to? of a class's own class method, or a public_method, reads the cells of the entry a call of the
       name would take there (cplan_cmethod_presence), one that sees public methods alone, whatever its receiver */
    const char *qm;
    int pc = ver_cells_reader(c, id, 0, &qm, NULL);
    if (pc >= 0 && cplan_presence_asks(c, id, pc, qm)) ver_need(c, id, qm, pc, CT_CMETHOD | CT_PUBLIC | CT_PROTECTED, &o);
    list_asks(c, id, list_ver_need, &o);   /* (a class-side list asks the same of each name it may list) */
    ct_implied_classes(c, id, ct_version_named, ver_need, &o);
    for (int i = 0, nn = call_node_names(c, id, names); i < nn; i++) {
      if (!names[i]) continue;
      if (ct_version_named(c, names[i])) ct_version_classes(c, id, names[i], ver_need, &o);
      for (int j = 0; j < ngc; j++) {
        const Scope *m = &c->scopes[gc[j]];
        if (!sp_streq(comp_scope_user_name(m), names[i]) && !is_send_family(names[i])) continue;
        ver_need_guard(c, &o, id, gc[j]);
      }
      for (int j = 0; j < nfl; j++)
        if (sp_streq(comp_scope_user_name(&c->scopes[fl[j]]), names[i]) || is_send_family(names[i])) ver_need_front(c, &o, id, fl[j]);
    }
  }
  presence_readers(c, 0, ver_need, &o);   /* (an instance's presence reader, of the classes it may ask of) */
  /* the supers whose next method is a layer of a mixin's versions: the
     entry over them for each's arguments (super_layer_entry) */
  for (int k = 0; k < 2; k++)
    for (int id = comp_kind_first(c, k ? NK_ForwardingSuperNode : NK_SuperNode); id >= 0; id = comp_kind_next(c, id)) {
      Scope *s = comp_scope_of(c, id);
      int to[VER_MAX_ARMS], layer = 0, t = s && s->class_id >= 0 && s->name ? cplan_super_target(c, id, s, NULL) : -1;
      int n = t >= 0 ? pos_runs(c, s, t, to, &layer) : 0;
      if (!layer) n = 0;
      const Scope *m = n ? &c->scopes[t] : NULL;
      VerNeed w = { VER_ENTRY_LAYER, m ? m->class_id : -1, m ? m->is_cmethod : 0, t, n ? ver_arity(c, id, to, n) : 0, m ? comp_scope_user_name(m) : NULL,
                    t + 1 };
      if (n && !ver_need_has(c, &o, w)) ver_need_add(&o, w);
      if (!n && t >= 0 && c->scopes[t].mixin_conditional) {
        m = &c->scopes[t];
        w = (VerNeed){ VER_ENTRY_GUARD, m->class_id, m->is_cmethod, t, ver_call_n(c, id), comp_scope_user_name(m), t + 1 };
        if (!ver_need_has(c, &o, w)) ver_need_add(&o, w);
      }
    }
  comp_ver_asking(c, 0);
  int hooks = -1;   /* (guard_else_bare) */
  for (int i = 0; i < o.n; i++) {
    /* (a layer's entry registers itself, once: pos_entry) */
    if (o.v[i].guard == VER_ENTRY_LAYER) {
      pos_entry(c, NULL, o.v[i].mi, o.v[i].ar, o.v[i].face);
      continue;
    }
    if (o.v[i].guard == VER_ENTRY_GUARD && !o.v[i].from && pos_guard_made(c, o.v[i].mi, o.v[i].ar, o.v[i].face) >= 0) continue;
    /* (a guard entry's else arm runs the method behind the mixins: for a
       later include's behind, what its super reaches, if anything) */
    int to[VER_MAX_ARMS + 1], n = o.v[i].guard != VER_ENTRY_CHAIN ? guard_chain(c, o.v[i].mi, to, NULL, NULL) : 0;
    int finds = n && guard_else_bare(c, to[n - 1], o.v[i].cls, o.v[i].sg, o.v[i].name, &hooks) ? VER_FINDS_NONE : ver_else_finds(c, VK_KEEP, o.v[i].cls, o.v[i].sg, o.v[i].name, SP_VIS_PUBLIC, -1);
    int behind = -1, e = o.v[i].guard != VER_ENTRY_CHAIN ? guard_entry_make(c, o.v[i].mi, o.v[i].ar, o.v[i].face, &behind)
                         : ver_entry_make(c, NULL, o.v[i].from ? o.v[i].from - 1 : o.v[i].cls, o.v[i].cls, o.v[i].sg, o.v[i].name, o.v[i].mi, o.v[i].ar, 1, o.v[i].face, NULL, &finds, NULL);
    int super = o.v[i].guard == VER_ENTRY_GUARD && o.v[i].from;
    ver_entry_add(c, e >= 0 && !super ? c->scopes[e].name : o.v[i].name, (struct VerEntry){ NULL, o.v[i].guard, o.v[i].cls, o.v[i].sg, o.v[i].ar, e, .finds = finds, .from = o.v[i].from, .behind = behind + 1, .face = o.v[i].face });
  }
  free(o.v); free(o.ix); free(o.pre);
  ver_vcall_twins(c);
  ver_entries_index(c);
  /* The table answers the general entry. Copies keep their own super
     chain under a shadow name; argument-specific entries stay hidden. */
  for (int j = 0; j < ngc; j++) {
    int mi = gc[j], to[VER_MAX_ARMS + 1];
    Scope *m = &c->scopes[mi];
    int ar = guard_arity(c, -1, to, guard_chain(c, mi, to, NULL, NULL));
    int e = ct_entry_of(c, VER_ENTRY_GUARD, m->class_id, m->is_cmethod, comp_scope_user_name(m), ar);
    if (e < 0) continue;
    c->scopes[e].entry = ENTRY_TABLE;
    c->scopes[e].mixin_guarded = m->mixin_guarded;
    c->scopes[e].guard_behind = mi;
    comp_touch_scope_shape(c, e);
  }
  free(gc);
  free(fl);
  free(c->pos_memo.at); free(c->pos_memo.to);
  c->pos_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  free(c->pnext_memo.at); free(c->pnext_memo.to);
  c->pnext_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  c->ver_built = 1;
}
/* `cell = v`, in at's scope and on its line (ver_cell_put places it) */
static int ver_cell_node(Compiler *c, const char *cell, int at, int v) {
  NodeTable *nt = (NodeTable *)c->nt;
  int line = (int)nt_int(nt, at, "node_line", 0), tv = nt_new_int(nt, v), g = nt_new_node(nt, "GlobalVariableWriteNode");
  nt_node_set_str(nt, g, "name", cell);
  nt_node_set_ref(nt, g, "value", tv);
  nt_node_set_int(nt, g, "ver_cell", 1);
  nt_node_set_int(nt, tv, "node_line", line);
  nt_node_set_int(nt, g, "node_line", line);
  /* (the cell holds an Integer from the entry file's first statement on, and nothing can read it before: its seed) */
  LocalVar *gv = comp_gvar_intern(c, cell + 1);
  gv->gvar_seeded = 2;
  gv->ver_cell = 1;
  comp_grow_node_arrays(c);
  c->nscope[g] = c->nscope[tv] = c->nscope[at];
  return g;
}
/* `cell = v` put in statement list `list` at its statement `at` (after it
   when after), in at's scope: the write, for the caller to record as the
   load record records a write (elab_record_write) */
int ver_cell_put(Compiler *c, const char *cell, int list, int at, int after, int v) {
  int g = ver_cell_node(c, cell, at, v);
  ver_list_put((NodeTable *)c->nt, list, at, after, g);
  return g;
}
/* A duplicate prepend selects this position only if none ran earlier. */
int ver_cell_put_first(Compiler *c, const char *cell, int list, int at, int v) {
  NodeTable *nt = (NodeTable *)c->nt;
  int first = nt->count, g = ver_cell_node(c, cell, at, v);
  DispArm unset = { cell, 0, NULL, 0 };
  int conditional = disp_cell_guard(nt, &unset, 1, g);
  comp_grow_node_arrays(c);
  for (int n = first; n < nt->count; n++) c->nscope[n] = c->nscope[at];
  ver_list_put(nt, list, at, 1, conditional);
  return g;
}
/* ...and the cells' writes: after each statement that makes a version an
   entry arms (ahead of it when it is a def, an alias or an undef), the write of
   the version's number to its cell (ElabVersion.cell; 0 for a remove_method's,
   which passes the search on), in the same statement list; and 0 ahead of
   the entry file's first statement (unless that statement makes one of the
   cell's versions), so that the cell holds an Integer wherever an entry
   reads it. Each recorded as the load record records a
   write (elab_record_write). An open history's entry (guard 2) arms its
   own history's versions. The writes are placed list by list, one rebuild
   of each list's statements: ahead of a statement in the order made, after
   it in the reverse order, as one insertion at a time placed them. */
typedef struct { int list, stmt, node, n, cell, after, k, g, body, vis, word; } VerWrite;   /* body: a method body's version (LrDef.body); vis: of the cell's visibility cell; word: a visibility word's write (ver_word_writes) */
static void ver_write_add(VerWrite **w, int *nw, VerWrite x) {
  if (!(*nw & (*nw - 1)) && !(*w = realloc(*w, sizeof **w * (size_t)(*nw ? 2 * *nw : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  (*w)[(*nw)++] = x;
}
/* The statement of a list a cell write of the version def `node` makes
   goes beside: the def's own; for a def in a class_eval-family block,
   whose defs the reopen registers and whose call runs none of its
   statements (class_eval_reopen_class), the call's, after it, once the
   block's defs are made */
static int ver_write_stmt(const NodeTable *nt, const int *par, int node) {
  int st = node;
  for (;;) {
    while (st >= 0 && par[st] >= 0 && nt_kind(nt, par[st]) != NK_StatementsNode) st = par[st];
    int blk = st >= 0 && par[st] >= 0 ? par[par[st]] : -1, call = blk >= 0 ? par[blk] : -1;
    const char *nm = call >= 0 && nt_kind(nt, blk) == NK_BlockNode && nt_kind(nt, call) == NK_CallNode &&
                     nt_ref(nt, call, "block") == blk ? nt_str(nt, call, "name") : NULL;
    /* (a defs-only `K.instance_eval { }` is the same reopening, #10785) */
    if (!nm || !(is_class_eval_family(nm) || (is_instance_eval_family(nm) && nt_ref(nt, call, "arguments") < 0))) return st;
    /* (a block on the body's own class that holds other statements runs
       them where it stands, as the body's: the write goes beside the def
       in the block's own list, between the statements around it, #10785) */
    if (is_class_eval_family(nm)) {
      int cr = nt_ref(nt, call, "receiver"), bb = nt_ref(nt, blk, "body"), bn = 0, mixed = 0;
      const int *bs = bb >= 0 && nt_kind(nt, bb) == NK_StatementsNode ? nt_arr(nt, bb, "body", &bn) : NULL;
      for (int i = 0; i < bn; i++) mixed |= nt_kind(nt, bs[i]) != NK_DefNode;
      if (mixed && (cr < 0 || nt_kind(nt, cr) == NK_SelfNode) && nt_ref(nt, blk, "parameters") < 0) return st;
    }
    st = call;
  }
}
static int ver_write_cmp(const void *a, const void *b) {
  const VerWrite *x = a, *y = b;
  if (x->list != y->list) return x->list < y->list ? -1 : 1;
  if (x->stmt != y->stmt) return x->stmt < y->stmt ? -1 : 1;
  if (x->after != y->after) return x->after - y->after;
  /* (a word's write after the writes of the defs it takes, `private_class_method def self.m`, ahead of a statement
     whose value is read too) */
  if (x->word != y->word) return x->word - y->word;
  return x->after ? y->k - x->k : x->k - y->k;
}
/* The cells each node writes, as they are placed: a cell is written once
   at a node, and one node may make versions in several histories, each with
   a cell of its own (`module_function :a, :b` copies both names at its
   node; a bare module_function def is the instance method's version and the
   singleton's copy; `attr_accessor :x` makes a reader and a writer) */
typedef struct { int *at; struct { int cell, next; } *e; int n; } VerCellsAt;
/* 1 the first time node writes cell (ElabVersion.cell, recording it),
   else 0 */
static int ver_cell_once(VerCellsAt *m, int node, int cell) {
  for (int k = m->at[node]; k; k = m->e[k - 1].next)
    if (m->e[k - 1].cell == cell) return 0;
  if (!(m->n & (m->n - 1)) && !(m->e = realloc(m->e, sizeof *m->e * (size_t)(m->n ? 2 * m->n : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->e[m->n].cell = cell;
  m->e[m->n].next = m->at[node];
  m->at[node] = ++m->n;
  return 1;
}
/* Whether the statement runs no program code of its own: a def, an alias, an
   undef, or a visibility call of a def (`private def m`), whose cell write
   then goes ahead of it, not after (it would be a block's value otherwise) */
static int ver_stmt_runs_no_code(const NodeTable *nt, int st) {
  NodeKind k = nt_kind(nt, st);
  if (k == NK_DefNode || k == NK_AliasMethodNode || k == NK_UndefNode) return 1;
  if (k != NK_CallNode || nt_ref(nt, st, "receiver") >= 0 || nt_ref(nt, st, "block") >= 0) return 0;
  const char *nm = nt_str(nt, st, "name");
  int an = 0, args = nt_ref(nt, st, "arguments");
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  return nm && an == 1 && nt_kind(nt, av[0]) == NK_DefNode && visibility_of_name(nm) > BVIS_MODULE_FUNCTION;
}
/* ...and beside each copy of a method body's version (VerWrite.body): a
   module's method cloned into each class that includes, prepends or
   extends it (nt_clone_subtree) holds a copy of a def its body makes
   (LrDef.body), which writes the same cell where the copy runs. Only those:
   a version at a point writes its cell at its own statement. A copy names
   the node it copies (nt_clone_source), along a chain of up to 64 copies;
   only the writes made now are copied, since a copy made after an earlier
   write carries that write */
static void ver_copy_writes(Compiler *c, const int *par, VerCellsAt *cells, VerWrite **wp, int *nwp) {
  const NodeTable *nt = c->nt;
  int nw0 = *nwp, *of = nw0 ? calloc((size_t)nt->count + 1, sizeof(int)) : NULL, *nx = nw0 ? calloc((size_t)nw0, sizeof(int)) : NULL;
  if (!nw0) return;
  if (!of || !nx) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  /* (a body def may have two writes, its version cell's and its visibility cell's: of[] holds the first, nx[]
     the next of the same node, and a clone takes each, once per cell) */
  for (int k = nw0 - 1; k >= 0; k--)
    if ((*wp)[k].node >= 0 && (*wp)[k].body) { nx[k] = of[(*wp)[k].node]; of[(*wp)[k].node] = k + 1; }
  NT_FOREACH_KIND(nt, NK_DefNode, d) {
    int o = d, k = 0, st;
    for (int depth = 0; depth < 64 && !k; depth++) {
      if ((o = nt_clone_source(nt, o)) < 0 || o >= nt->count) break;
      k = of[o];
    }
    if (!k || (st = ver_write_stmt(nt, par, d)) < 0 || par[st] < 0) continue;
    for (int j = k; j; j = nx[j - 1]) {
      VerWrite x = (*wp)[j - 1];
      if (!ver_cell_once(cells, d, x.vis ? -2 - x.cell : x.cell)) continue;
      x.list = par[st]; x.stmt = st; x.node = d;
      x.after = !ver_stmt_runs_no_code(nt, st);
      ver_write_add(wp, nwp, x);
    }
  }
  free(of);
  free(nx);
}
/* Does the C write scope s out: reached, or a proc-form clone (emission
   writes the live ones, proc_form_live; all of them count here) */
static int ver_scope_written(const Scope *s) {
  return !s || s->reachable || s->is_proc_form;
}
/* (keep the cell `$name` names: ver_cells_settle) */
static void ver_cell_keep(Compiler *c, const char *name) {
  const LocalVar *g = comp_gvar(c, name + 1);
  int i = g ? (int)(g - c->gvars) : -1;
  if (i >= 0 && i < c->nver_cell_dropped) c->ver_cell_dropped[i] = 0;
}
/* (the class whose class method *qm reader id asks the cells of: a respond_to?'s, cplan_presence_class, or a
   public_method capture's of a name a body's word sets the visibility of, cplan_capture_closed; -1 for none, and with
   written for one in a scope the C does not write, ver_scope_written); in *mask the visibilities it counts, a
   respond_to?'s (respond_to_vis_mask), or 0 for a capture, whose NameError names the visibility; the names they go
   by, r 0 to 2 (a public_method's until desugar_public_method names it `method`) */
static const char *ver_cells_reader_name(int r) { return r == 2 ? PUBLIC_METHOD_NAME : r ? METHOD_NAME : RESPOND_TO_NAME; }
static int ver_cells_reader(Compiler *c, int id, int written, const char **qm, int *mask) {
  const char *nm = nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  int cap = nm && (nt_int(c->nt, id, "public_capture", 0) || is_public_method_lookup(nm)) && capture_cells_shape(c->nt, id);
  int pc = cap ? cells_reader_class(c, id) : is_respond_to_name(nm) ? cplan_presence_class(c, id) : -1;
  if (mask) *mask = pc >= 0 && !cap ? respond_to_vis_mask(c->nt, id) : 0;
  *qm = pc >= 0 && (!written || ver_scope_written(comp_scope_of(c, id))) ? method_sym_arg(c, id) : NULL;
  if (*qm && cap && !elab_method_body_word(c, *qm, 1)) *qm = NULL;
  return *qm ? pc : -1;
}
/* (keep the cells call id reads to answer whether cls has class method name: a respond_to?'s, cplan_cmethod_presence,
   for the visibilities mask counts, none where they fix its answer, which it folds; a public_method's, ver_cells_reader,
   or a class-side list's, cplan_list_cells, every visibility's (mask 0); own versions or the chain's as the list asks,
   cplan_list_class) */
static void ver_cmethod_cells_keep(Compiler *c, int id, const char *name, int cls, int mask) {
  CplanCells cs;
  int own = 0;
  if (cplan_list_class(c, id, &own) < 0) own = 0;
  for (int all = 1; all >= 0; all--) {
    if (mask && mask != (all ? 7 : 1)) continue;
    if (!cplan_cmethod_presence(c, id, cls, name, all ? 7 : 1, own, &cs) || (mask && cplan_cells_fixed(&cs))) return;
    /* (and the visibility cells, which it reads where a version's visibility is the run's) */
    for (int i = 0; i < cs.n; i++) {
      if (all || mask) ver_cell_keep(c, cs.cell[i]);
      if (!all && cs.vis[i][0]) ver_cell_keep(c, cs.vis[i]);
    }
  }
}
static void ver_presence_keep(Compiler *c, int id, const char *name, int cls, void *arg) {
  (void)arg;
  ver_cmethod_cells_keep(c, id, name, cls, 0);
}
/* (...and an instance's presence reader's, presence_asks: the cells it reads, in the form it asks them
   (presence_reader_side), where they do not fix its answer) */
static void ver_obj_presence_keep(Compiler *c, int id, const char *name, int cls, unsigned flags, void *arg) {
  CplanCells cs;
  int mask, list;
  unsigned side = presence_reader_side(c->nt, id, &mask, &list);
  if (flags & CT_CMETHOD) {   /* (a respond_to?'s class arm: a class's own respond_to?'s cells) */
    ver_cmethod_cells_keep(c, id, name, cls, list ? 0 : mask);
    return;
  }
  if (!(list ? cplan_list_cells(c, id, cls, side, name, mask, &cs) : cplan_presence_cells(c, id, cls, side, name, mask, &cs)) ||
      cplan_cells_fixed(&cs)) return;
  for (int i = 0; i < cs.n; i++) {
    ver_cell_keep(c, cs.cell[i]);
    if (cs.vis[i][0]) ver_cell_keep(c, cs.vis[i]);
  }
}
/* See call_plan.h. A cell is kept where the C reads it: a read in a
   scope the C writes (an entry's arms, a prepend guard's, a conditional
   def's dispatcher), the cells a written respond_to? or class-side list
   asks (ver_presence_keep), a write of the program's own
   to a global of its name, or a write of the plan's (ver_cell_node marks
   it) whose value is not thrown away (an_node_dropped). Every other cell
   is read by nothing
   the program runs, though entries a call bound before the last
   reachability, or a survey's, made it. Entries stay: a respond_to?
   reads the cells of one no call reaches. */
void ver_cells_settle(Compiler *c) {
  const NodeTable *nt = c->nt;
  free(c->ver_cell_dropped);
  c->nver_cell_dropped = c->ngvars;
  if (!(c->ver_cell_dropped = calloc((size_t)c->ngvars + 1, 1))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int any = 0;   /* (a program with no cell walks nothing) */
  for (int i = 0; i < c->ngvars; i++) any |= c->ver_cell_dropped[i] = c->gvars[i].ver_cell;
  /* (a Method's maker copies the tests of the entry it captures into its
     own arity and parameters, cplan_method_arms: where the program makes
     one, an entry's reads count as the C's though no call reaches it) */
  static const char *const MAKERS[] = { "method", "public_method", "singleton_method", "instance_method",
                                        "public_instance_method", NULL };
  int makes = 0;
  for (int m = 0; any && !makes && MAKERS[m]; m++)
    for (int id = an_calls_named_first(c, MAKERS[m]); id >= 0 && !makes; id = an_calls_named_next(id))
      makes = ver_scope_written(comp_scope_of(c, id));
  for (int id = 0; any && id < nt->count; id++) {
    const char *nm = nt_str(nt, id, "name");
    NodeKind k = nt_kind(nt, id);
    const LocalVar *g = nm && nm[0] == '$' ? comp_gvar(c, nm + 1) : NULL;
    const Scope *in = g && g->ver_cell ? comp_scope_of(c, id) : NULL;
    if (!g || !g->ver_cell || !(ver_scope_written(in) || (makes && in->entry != ENTRY_NONE))) continue;
    if (k != NK_GlobalVariableWriteNode || !nt_int(nt, id, "ver_cell", 0) || !an_node_dropped(c, id)) ver_cell_keep(c, nm);
  }
  /* (a guarded call's cell: kept where the call is in a scope the C writes) */
  for (int i = 0; any && i < c->nver_vis_reads; i++) {
    char cell[32];
    ver_vis_cell_name(c->ver_vis_reads[i].cell, cell);
    if (c->ver_vis_reads[i].call >= 0 && ver_scope_written(comp_scope_of(c, c->ver_vis_reads[i].call))) ver_cell_keep(c, cell);
  }
  for (int i = 0; any && i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->vis != VER_VIS_STRING_HISTORY) continue;
    ElabVersion vs[VER_MAX_ARMS];
    int n = guard_history_arms(c, e, vs, NULL);
    for (int k = 0; k < n; k++) {
      char name[32];
      ver_cell_name(vs[k].cell, name);
      ver_cell_keep(c, name);
      if (vs[k].v->kind != LR_UNDEF && elab_physical_version_vis(c, &vs[k]) < 0) {
        ver_vis_cell_name(vs[k].cell, name);
        ver_cell_keep(c, name);
      }
    }
  }
  for (int r = 0; any && r < 3; r++)
    for (int id = an_calls_named_first(c, ver_cells_reader_name(r)); id >= 0; id = an_calls_named_next(id)) {
      const char *qm;
      int mask, pc = ver_cells_reader(c, id, 1, &qm, &mask);
      if (pc >= 0) ver_cmethod_cells_keep(c, id, qm, pc, mask);
    }
  for (int id = any ? an_calls_named_first(c, SINGLETON_METHODS_NAME) : -1; id >= 0; id = an_calls_named_next(id))
    if (ver_scope_written(comp_scope_of(c, id))) list_asks(c, id, ver_presence_keep, NULL);
  if (any) presence_readers(c, 1, ver_obj_presence_keep, NULL);
}
/* See call_plan.h. */
int cplan_cell_dropped(const Compiler *c, const LocalVar *g) {
  int i = g ? (int)(g - c->gvars) : -1;
  return i >= 0 && i < c->nver_cell_dropped && c->ver_cell_dropped[i];
}
/* See call_plan.h. */
int cplan_vis_cell_after(Compiler *c, int id, int k, const LocalVar **g, int *v, int *only) {
  *only = -1;
  for (int i = 0; i < c->nver_vis_after; i++) {
    const struct VerVisAfter *a = &c->ver_vis_after[i];
    if (a->node != id || k--) continue;
    char cell[32];
    ver_vis_cell_name(a->cell, cell);
    *g = comp_gvar(c, cell + 1);
    if (*g && cplan_cell_dropped(c, *g)) *g = NULL;
    *v = a->v;
    return 1;
  }
  /* ...and the Kernel cells an include writes as it runs (ver_kern_sites): one of several classes' by the receiving class */
  int nk = 0;
  for (int i = 0; i < c->nver_kern_after; i++) nk += c->ver_kern_after[i].node == id;
  for (int i = 0; i < c->nver_kern_after; i++) {
    const struct VerKernAfter *a = &c->ver_kern_after[i];
    if (a->node != id || k--) continue;
    char cell[32];
    cplan_kernel_cell_name(a->cls, cell);
    *g = comp_gvar(c, cell + 1);
    if (*g && cplan_cell_dropped(c, *g)) *g = NULL;
    *v = 1;
    if (nk > 1) *only = a->cls;
    return 1;
  }
  return 0;
}
/* See call_plan.h. */
int cplan_cell_write_dropped(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  return nt_kind(nt, id) == NK_GlobalVariableWriteNode && nt_int(nt, id, "ver_cell", 0) &&
         cplan_cell_dropped(c, comp_gvar(c, nt_str(nt, id, "name") + 1));
}
/* ...the write of version v's cell at its statement, and with rt that of
   its visibility cell (with pass 0 only their places taken: written
   before), into *w; *nh past the cells written. After the statement, but
   ahead of one that runs no code (ver_stmt_runs_no_code) or whose value is
   read, `r = def self.m = 1` ending a body, which the write would answer */
static void ver_write_version(Compiler *c, NodeTable *nt, const int *par, VerCellsAt *cells, const ElabVersion *v, int rt, int pass, VerWrite **wp, int *nwp, int *nh) {
  int node = v->v->node, st = ver_write_stmt(nt, par, node);
  if (st < 0 || par[st] < 0) return;
  /* (each cell once: an entry without a face may have written the
     version's; an undef has no visibility to write) */
  int vw = rt && v->v->kind != LR_REMOVE && v->v->kind != LR_UNDEF && ver_cell_once(cells, node, -2 - v->cell);
  int cw = ver_cell_once(cells, node, v->cell);
  if ((!cw && !vw) || !pass) return;
  /* (ahead of a statement that runs no program code: after one that ends a branch, the write would be the branch's value) */
  VerWrite x = { par[st], st, node, v->v->kind == LR_REMOVE ? 0 : v->n, v->cell, !ver_stmt_runs_no_code(nt, st) && an_value_dropped(c, par, st), 0, -1, v->v->body != 0 };
  if (cw) ver_write_add(wp, nwp, x);
  if (vw) x.n = v->v->vis, x.vis = 1, ver_write_add(wp, nwp, x);
  if (v->cell >= *nh) *nh = v->cell + 1;
  /* Not the lone conditional def's dispatcher alone: it stands after the condition, and the
     method is there as soon as one of its arms (`name#__condN`, cond_disp) runs in its branch */
  if (nt_int(nt, node, "cond_lone_disp", 0)) {
    for (int d = nt_int(nt, node, "cond_first_arm", 0); d > 0; d = nt_int(nt, d, "cond_next_arm", 0)) {
      int ast = ver_write_stmt(nt, par, d);
      if (ast < 0 || par[ast] < 0) continue;
      int acw = ver_cell_once(cells, d, v->cell), avw = vw && ver_cell_once(cells, d, -2 - v->cell);
      VerWrite y = { par[ast], ast, d, v->n, v->cell, nt_kind(nt, ast) != NK_DefNode, 0, -1 };
      if (acw) ver_write_add(wp, nwp, y);
      if (avw) y.n = v->v->vis, y.vis = 1, ver_write_add(wp, nwp, y);
    }
  }
}
static int ver_cells_put(Compiler *c);
void ver_cells_write(Compiler *c) {
  /* (the Kernel cells the include calls write, as globals before the types are derived) */
  ver_kern_sites(c);
  for (int k = 0; k < c->nver_kern_after; k++) (void)ver_kern_cell(c, c->ver_kern_after[k].cls);
  ver_kernel_guards(c);
  ver_cells_put(c);
  ver_memo_drop(c);   /* (the versions kept before the writes are asked again) */
}
/* (a word inside an expression's write of visibility cell `cell`, once per
   word and cell: cplan_vis_cell_after) */
static void ver_vis_after_add(Compiler *c, int node, int cell, int v) {
  int n = c->nver_vis_after;
  for (int i = 0; i < n; i++)
    if (c->ver_vis_after[i].node == node && c->ver_vis_after[i].cell == cell) return;
  if (!(n & (n - 1)) && !(c->ver_vis_after = realloc(c->ver_vis_after, sizeof *c->ver_vis_after * (size_t)(n ? 2 * n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  c->ver_vis_after[c->nver_vis_after++] = (struct VerVisAfter){ node, cell, v };
}
/* (a String history's entry keeps its own mark, VER_VIS_STRING_HISTORY: its visibility cells are written already) */
static void ver_entry_vis_mark(Compiler *c, int e) {
  for (int i = 0; e >= 0 && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e && c->ver_entries[i].vis != VER_VIS_STRING_HISTORY) c->ver_entries[i].vis = VER_VIS_PRESENT;
}
/* (an instance's presence reader's, presence_asks: the entry a call there takes; a class arm's, the class
   method's, as pres_target asks it) */
static void ver_obj_presence_mark(Compiler *c, int id, const char *name, int cls, unsigned flags, void *arg) {
  MethodLookup ml = { .def_class = -1 };
  (void)arg;
  int e = call_target(c, id, cls, name, flags & CT_CMETHOD ? CT_CMETHOD | CT_CAPTURE : CT_REACH, &ml);
  if (ml.kind == ML_ENTRY) ver_entry_vis_mark(c, e);
}
/* (a class-side list's, of each name it may list: list_ver_target) */
static void ver_list_presence_mark(Compiler *c, int id, const char *name, int cls, void *arg) {
  (void)arg;
  ver_entry_vis_mark(c, call_target(c, id, cls, name, CT_CMETHOD | CT_CAPTURE, NULL));
}
/* (mark the entry each written respond_to? or public_method of a class method reads the cells of,
   cplan_cmethod_presence, of each name a class-side list reads (ver_list_presence_mark), and of an instance's method
   (ver_obj_presence_mark): its versions' visibility cells are written, as a face that closes would have them, so
   the test of a version whose visibility only the run knows reads one. The entries all exist: asked here as the
   emission asks) */
static void ver_presence_mark(Compiler *c) {
  for (int r = 0; r < 3; r++)
    for (int id = an_calls_named_first(c, ver_cells_reader_name(r)); id >= 0; id = an_calls_named_next(id)) {
      const char *qm;
      int mask, pc = ver_cells_reader(c, id, 1, &qm, &mask);
      /* (an include_all reader counts every visibility: it reads no visibility cell) */
      ver_entry_vis_mark(c, pc >= 0 && mask != 7 && cplan_presence_asks(c, id, pc, qm) ? call_target(c, id, pc, qm, CT_CMETHOD | CT_CAPTURE, NULL) : -1);
    }
  for (int id = an_calls_named_first(c, SINGLETON_METHODS_NAME); id >= 0; id = an_calls_named_next(id))
    if (ver_scope_written(comp_scope_of(c, id))) list_asks(c, id, ver_list_presence_mark, NULL);
  presence_readers(c, 1, ver_obj_presence_mark, NULL);
}
/* A guard's cell: the visibility cell of history cell `cell` (of the top-level def
   `name`), where a call reads it for its guard (CallPlan.vis_cell: call >= 0, which
   keeps the cell, ver_cells_settle), or which the history's versions write (call -1,
   ver_guard_writes), once per call and cell */
static void ver_vis_read_add(Compiler *c, int call, const char *name, int cell) {
  int n = c->nver_vis_reads;
  if (call >= 0) {   /* (one cell a call: its node holds it) */
    if (call >= c->ver_vis_call_cap) {
      int ncap = c->node_cap > call ? c->node_cap : call + 1;
      if (!(c->ver_vis_call_cell = realloc(c->ver_vis_call_cell, sizeof(int) * (size_t)ncap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      memset(c->ver_vis_call_cell + c->ver_vis_call_cap, 0, sizeof(int) * (size_t)(ncap - c->ver_vis_call_cap));
      c->ver_vis_call_cap = ncap;
    }
    if (c->ver_vis_call_cell[call] == cell + 1) return;
    c->ver_vis_call_cell[call] = cell + 1;
  }
  else {   /* (and a history once: its cell holds it) */
    if (cell >= c->ver_vis_hist_cap) {
      int ncap = 2 * cell + 8;
      if (!(c->ver_vis_hist_seen = realloc(c->ver_vis_hist_seen, (size_t)ncap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      memset(c->ver_vis_hist_seen + c->ver_vis_hist_cap, 0, (size_t)(ncap - c->ver_vis_hist_cap));
      c->ver_vis_hist_cap = ncap;
    }
    if (c->ver_vis_hist_seen[cell]) return;
    c->ver_vis_hist_seen[cell] = 1;
  }
  if (!(n & (n - 1)) && !(c->ver_vis_reads = realloc(c->ver_vis_reads, sizeof *c->ver_vis_reads * (size_t)(n ? 2 * n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  char *nm = strdup(name);
  if (!nm) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->ver_vis_reads[c->nver_vis_reads++] = (struct VerVisRead){ nm, cell, call, 0 };
  if (call < 0) c->ver_vis_read_new = 1;
}
/* The histories a call on an explicit receiver may be guarded by (cplan_root_top_def), made
   before the calls' plans are asked: each top-level def of one version, with no twin, whose
   visibility only the run tells, and which some call of the name with a receiver other
   than self names, has its cell written (ver_cells_put); a cell no guard reads is
   dropped (ver_cells_settle) */
static void ver_vis_guard_hists(Compiler *c) {
  const NodeTable *nt = c->nt;
  /* (the same tree asks the same: a round that changed nothing walks nothing) */
  unsigned long stamp = ((unsigned long)nt->count * 1000003ul ^ (unsigned long)c->nscopes) * 1000003ul ^ (unsigned long)nt->edits * 31ul ^ (unsigned long)nt->version;
  if (c->ver_vis_guard_stamp == stamp) return;
  c->ver_vis_guard_stamp = stamp;
  if (c->ver_vis_scope_cap < c->nscopes) {
    int ncap = c->nscopes;
    if (!(c->ver_vis_scope_mark = realloc(c->ver_vis_scope_mark, (size_t)ncap))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(c->ver_vis_scope_mark + c->ver_vis_scope_cap, 0, (size_t)(ncap - c->ver_vis_scope_cap));
    c->ver_vis_scope_cap = ncap;
  }
  for (int s = 1; s < c->nscopes; s++) {
    const Scope *sc = &c->scopes[s];
    if (c->ver_vis_scope_mark[s] || sc->class_id >= 0 || sc->is_cmethod || !sc->name || sc->def_node < 0 || sc->is_proc_form || sc->replaced) continue;
    int cell = -1;
    if (an_calls_named_first(c, sc->name) < 0) continue;
    if (cplan_top_lone_vis(c, sc->name, &cell) != -1 || cell < 0) { c->ver_vis_scope_mark[s] = 2; continue; }
    for (int id = an_calls_named_first(c, sc->name); id >= 0; id = an_calls_named_next(id)) {
      int recv = nt_kind(nt, id) == NK_CallNode && sp_streq(nt_str(nt, id, "name"), sc->name) ? nt_ref(nt, id, "receiver") : -1;
      if (recv < 0 || nt_kind(nt, recv) == NK_SelfNode) continue;
      ver_vis_read_add(c, -1, sc->name, cell);
      c->ver_vis_scope_mark[s] = 1;
      break;
    }
  }
  /* (and a respond_to? of such a name, which reads the cell for its answer, cplan_top_def_cell) */
  for (int id = an_calls_named_first(c, RESPOND_TO_NAME); id >= 0; id = an_calls_named_next(id)) {
    const char *qm = nt_kind(nt, id) == NK_CallNode ? method_sym_arg(c, id) : NULL;
    int cell = -1;
    if (!qm || nt_ref(nt, id, "receiver") < 0 || cplan_top_lone_vis(c, qm, &cell) != -1 || cell < 0 ||
        call_target(c, -1, -1, qm, CT_TOP, NULL) < 0) continue;
    ver_vis_read_add(c, -1, qm, cell);
    ver_vis_read_add(c, id, qm, cell);
  }
}
/* See call_plan.h. */
const LocalVar *cplan_vis_cell_var(Compiler *c, const CallPlan *p) {
  char cell[32];
  if (p->vis_cell <= 0) return NULL;
  ver_vis_cell_name(p->vis_cell - 1, cell);
  const LocalVar *g = comp_gvar(c, cell + 1);
  return g && !cplan_cell_dropped(c, g) ? g : NULL;
}
/* The writes of the visibility cells of versions vs[0..nv) after each visibility word of
   their names, with the word's visibility (elab_version_word): beside the word's statement,
   or, for a word inside an expression, the word's own call (cplan_vis_cell_after), and its
   statement's too where the word takes a def of the name (`x = private_class_method def
   self.m`), whose own writes go beside the statement. Each once (pass 1 places them; pass 0
   only claims the ones already placed) */
static void ver_word_writes(Compiler *c, const int *par, VerCellsAt *cells, VerWrite **w, int *nw, int *nh,
                            const ElabVersion *vs, int nv, int pass) {
  const NodeTable *nt = c->nt;
  for (int j = 0; j < nv; j++)
    for (int k = 0, wn, wv, o; (o = elab_version_word(c, &vs[j], k, &wn, &wv)) >= 0; k++) {
      int st = o ? ver_write_stmt(nt, par, wn) : -1;
      if (st < 0 || par[st] < 0 || !ver_cell_once(cells, wn, -2 - vs[j].cell) || !pass) continue;
      int d = vs[j].v->node, takes = d >= 0 && par[d] >= 0 && nt_kind(nt, par[d]) == NK_ArgumentsNode && par[par[d]] == wn;
      if (nt_kind(nt, par[wn]) != NK_StatementsNode) {
        ver_vis_after_add(c, wn, vs[j].cell, wv);
        if (!takes) continue;
      }
      ver_write_add(w, nw, (VerWrite){ par[st], st, wn, wv, vs[j].cell, an_value_dropped(c, par, st), 0, -1, 0, 1, 1 });
      if (vs[j].cell >= *nh) *nh = vs[j].cell + 1;
    }
}
/* ...and a guarded call's (CallPlan.vis_cell): the history's lone version writes its def's
   visibility ahead of its def, as an entry's face does, and each word of the name after it */
static void ver_guard_writes(Compiler *c, const int *par, VerCellsAt *cells, VerWrite **w, int *nw, int *nh, int pass) {
  const NodeTable *nt = c->nt;
  ElabVersion vs[VER_MAX_ARMS];
  for (int i = 0; i < c->nver_vis_reads; i++) {
    struct VerVisRead *r = &c->ver_vis_reads[i];
    if (r->call >= 0 || r->written == pass) continue;   /* (written ones claim their keys first, then the new ones write) */
    r->written = 1;
    int nv = elab_history_versions(c, -1 /* main */, 0, r->name, vs, VER_MAX_ARMS);
    if (nv != 1 || vs[0].cell != r->cell) continue;
    ver_word_writes(c, par, cells, w, nw, nh, vs, nv, pass);
    int node = vs[0].v->node, st = ver_write_stmt(nt, par, node);
    if (st < 0 || par[st] < 0 || !ver_cell_once(cells, node, -2 - vs[0].cell) || !pass) continue;
    ver_write_add(w, nw, (VerWrite){ par[st], st, node, vs[0].v->vis, vs[0].cell, !ver_stmt_runs_no_code(nt, st), 0, -1, vs[0].v->body != 0, 1 });
    if (vs[0].cell >= *nh) *nh = vs[0].cell + 1;
  }
}
/* ...how many it wrote */
static int ver_cells_put(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  ElabVersion vs[VER_MAX_ARMS];
  VerWrite *w = NULL;
  int nw = 0, top = prog_root_stmts(c, 0), ntop = 0, nh = 0;
  const int *first = top >= 0 ? nt_arr(nt, top, "body", &ntop) : NULL;
  /* (the parents the program's tree reaches: a class_eval block spliced
     into its class body still names its statements, and the statement a
     write goes beside is the class body's) */
  int any = c->nver_entries || c->nver_guards || c->nver_vis_reads, *par = any ? du_parent_map(nt) : NULL;
  VerCellsAt cells = { any ? calloc((size_t)nt->count + 1, sizeof(int)) : NULL, NULL, 0 };
  if (any) ver_presence_mark(c);
  if (any && (!cells.at || !par)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int pass = 0; pass < 2; pass++) {   /* (the entries written before first: a cell is written once) */
  for (int i = 0; i < c->nver_entries; i++) {
    struct VerEntry *e = &c->ver_entries[i];
    /* (not only the entries reached now: a send's arms, made in the fixpoint, reach methods whose calls bind entries this first pass leaves unreached) */
    if (e->scope < 0 || e->cells == pass) continue;
    e->cells = 1;
    if (nt_int(nt, c->scopes[e->scope].def_node, "cond_twin", 0)) continue;
    int arms[VER_MAX_ARMS + 1], tests[2 * (VER_MAX_ARMS + 1)];
    int na = e->guard == VER_ENTRY_GUARD || e->guard == VER_ENTRY_LAYER ? ver_entry_targets(c, e->scope, arms, tests) : 0;
    /* (and an arm's own, of the entry over a mixin's versions:
       pos_entry) */
    for (int k = 0, n0 = na; k < n0; k++)
      if (arms[k] >= 0 && c->scopes[arms[k]].entry == ENTRY_DISPATCH) {
        int sub[VER_MAX_ARMS + 1], st[VER_MAX_ARMS + 1];
        for (int j = 0, ns = ver_entry_targets(c, arms[k], sub, st); j < ns && na < 2 * (VER_MAX_ARMS + 1); j++) tests[na++] = st[j];
      }
    /* (a layer's copies: versions on its class's chain where a later mixin guards them, its module's otherwise, pos_versions) */
    const Scope *lm = e->guard == VER_ENTRY_LAYER ? &c->scopes[e->from - 1] : NULL;
    int mod = e->guard == VER_ENTRY_LAYER ? !lm->mixin_guarded : e->from && e->guard != VER_ENTRY_GUARD;
    int nv = e->guard == VER_ENTRY_HISTORY ? elab_history_versions(c, e->cls, e->sg, e->name, vs, VER_MAX_ARMS)
                           : elab_chain_versions(c, mod ? (lm ? lm->origin_module_ci : e->from) - 1 : e->cls, mod ? 0 : e->sg, e->name, vs, VER_MAX_ARMS);
    /* (a face's arm of a version whose visibility only the run tells reads
       its cell's visibility cell: written beside each version's write,
       with its def's visibility, and after each visibility word of the
       name, with the word's (elab_version_word); ahead of one whose value
       is read, `x = (public :m if c)`, which the write would answer; a
       word inside an expression, `c && public(:m)`, may not run when its
       statement does, so its own call writes it as it runs, which the
       word's node keeps: cplan_vis_cell_after) */
    int rt = 0;
    for (int j = 0; (ver_face_closes(e->face) || e->vis) && j < nv; j++)
      rt |= (e->vis == VER_VIS_STRING_HISTORY ? elab_physical_version_vis(c, &vs[j]) : elab_version_vis(c, &vs[j])) < 0;
    if (rt) ver_word_writes(c, par, &cells, &w, &nw, &nh, vs, nv, pass);
    for (int j = 0; j < nv; j++) {
      if (e->guard == VER_ENTRY_GUARD || e->guard == VER_ENTRY_LAYER) {
        int reads = 0;
        for (int k = 0; k < na; k++)
          reads |= nt_int(nt, tests[k], "ver_hist", 0) == vs[j].cell + 1;
        if (!reads) continue;   /* prepend cells are written at their statements */
      }
      ver_write_version(c, nt, par, &cells, &vs[j], rt, pass, &w, &nw, &nh);
    }
  }
  for (int i = 0; i < c->nver_guards; i++) {
    struct VerGuard *g = &c->ver_guards[i];
    if (g->cells == pass) continue;
    g->cells = 1;
    /* (a module's own presence reader's, VerGuard.vis: the versions on its chain, and their words') */
    int nv = g->cls < 0 ? elab_chain_versions(c, -1, 1, g->name, vs, VER_MAX_ARMS)
           : g->vis ? elab_chain_versions(c, g->cls, 0, g->name, vs, VER_MAX_ARMS)
                    : elab_history_versions(c, g->cls, 0, g->name, vs, VER_MAX_ARMS);
    if (g->vis) ver_word_writes(c, par, &cells, &w, &nw, &nh, vs, nv, pass);
    for (int j = 0; j < nv; j++)
      ver_write_version(c, nt, par, &cells, &vs[j], g->vis, pass, &w, &nw, &nh);
  }
  if (c->nver_vis_reads) ver_guard_writes(c, par, &cells, &w, &nw, &nh, pass);   /* (and a guarded call's, CallPlan.vis_cell) */
  }
  c->ver_vis_read_new = 0;
  ver_copy_writes(c, par, &cells, &w, &nw);
  free(cells.at); free(cells.e); free(par);
  /* the zero writes: one per cell, at its first write, none where the first statement writes it */
  unsigned char *hz = calloc(2 * (size_t)nh + 2, 1);   /* (2 * cell, its visibility cell's one more) */
  int at0 = ntop ? first[0] : -1, nz = 0;
  VerWrite *all = malloc(sizeof *all * (size_t)(2 * nw + 1));
  if (!hz || !all) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nw; k++) if (w[k].stmt == at0) hz[2 * w[k].cell + w[k].vis] = 1;
  for (int k = 0; k < nw; k++) {   /* (made in the order one insertion at a time made them) */
    char cell[32];
    if (w[k].vis) ver_vis_cell_name(w[k].cell, cell);
    else ver_cell_name(w[k].cell, cell);
    if (at0 >= 0 && !hz[2 * w[k].cell + w[k].vis]) {
      hz[2 * w[k].cell + w[k].vis] = 1;
      all[nz] = (VerWrite){ top, at0, -1, 0, w[k].cell, 0, nz, ver_cell_node(c, cell, at0, 0), 0, w[k].vis };
      nz++;
    }
    all[nz] = w[k];
    all[nz].k = nz;
    all[nz].g = ver_cell_node(c, cell, w[k].stmt, w[k].n);
    nz++;
  }
  int *made = malloc(sizeof(int) * (size_t)(nz + 1)), nm = 0;   /* (recorded once placed: the version writes, then the zero writes) */
  if (!made) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nz; k++) if (all[k].node >= 0) made[nm++] = all[k].g;
  for (int k = 0; k < nz; k++) if (all[k].node < 0) made[nm++] = all[k].g;
  qsort(all, (size_t)nz, sizeof *all, ver_write_cmp);
  for (int k = 0, e; k < nz; k = e) {   /* each list once */
    int n = 0, o = 0;
    for (e = k; e < nz && all[e].list == all[k].list; e++) {}
    const int *st = nt_arr(nt, all[k].list, "body", &n);
    int *ns = malloc(sizeof(int) * (size_t)(n + e - k));
    if (!ns) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int j = 0; j < n; j++) {
      int lo = k, hi = e;   /* (the first write of st[j], by node) */
      while (lo < hi) { int mid = (lo + hi) / 2; if (all[mid].stmt < st[j]) lo = mid + 1; else hi = mid; }
      for (; lo < e && all[lo].stmt == st[j] && !all[lo].after; lo++) ns[o++] = all[lo].g;
      ns[o++] = st[j];
      for (; lo < e && all[lo].stmt == st[j]; lo++) ns[o++] = all[lo].g;
    }
    if (o != n + e - k) { fprintf(stderr, "spinel: internal: a cell write's statement is not in its statement list\n"); exit(1); }
    nt_node_set_arr(nt, all[k].list, "body", ns, o);
    free(ns);
  }
  for (int k = 0; k < nm; k++) elab_record_write(c, made[k]);
  free(made); free(all); free(hz); free(w);
  return nm;
}
void ver_memo_drop(Compiler *c) {
  /* (and the user calls' plans and poly arms, which bind those versions
     and entries; the arms handed out stay kept with the Compiler) */
  struct CplanMemo *m = c->cplan_memo;
  if (m) {
    cplan_table_free(&m->user);
    m->user = (struct CplanTable){0};
    for (int i = 0; i < m->poly_cap; i++) { free(m->poly_name[i]); m->poly_name[i] = NULL; }
  }
  for (int i = 0; i < c->ver_memo_cap; i++) free(c->ver_memo[i].name);
  free(c->ver_memo);
  c->ver_memo = NULL;
  c->ver_memo_cap = c->ver_memo_n = 0;
  free(c->guard_memo.at); free(c->guard_memo.to);
  free(c->lrb_memo.key); free(c->lrb_memo.val);
  memset(&c->lrb_memo, 0, sizeof c->lrb_memo);
  free(c->pnext_memo.at); free(c->pnext_memo.to);
  c->pnext_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  free(c->posv_memo.at); free(c->posv_memo.to);
  c->posv_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  free(c->pos_memo.at); free(c->pos_memo.to);
  c->pos_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  c->guard_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
  free(c->meet_memo.at);
  c->meet_memo = (struct MeetMemo){ NULL, 0 };
  new_sites_free(c);
  attr_facts_free(c);
}
static int ver_late_key_cmp(const struct VerLate *x, const struct VerLate *y) {
  int d = strcmp(x->name, y->name);
  return d ? d : x->cls != y->cls ? x->cls - y->cls : x->sg != y->sg ? x->sg - y->sg : x->ar != y->ar ? x->ar - y->ar : x->face - y->face;
}
static int ver_late_cmp(const void *a, const void *b) {   /* (a key's spanning calls last) */
  const struct VerLate *x = a, *y = b;
  int d = ver_late_key_cmp(x, y);
  return d ? d : x->span - y->span;
}
/* The entries of the chains a typed call may meet with no version made
   yet (ver_absent_note), each asked once: one whose else arm answers for
   none, in place of the entry made for the key's other calls, which no
   call binds then (absent 2); or, where the lookup would find the last
   version anyway (VK_KEEP), the table's answer kept (absent 1). Changed
   when it made one. */
static int ver_late_make(Compiler *c) {
  VerNamedAsks q;
  int made = 0;
  ver_named_facts(c, &q, 1);
  qsort(c->ver_late, (size_t)c->nver_late, sizeof *c->ver_late, ver_late_cmp);
  for (int i = 0; i < c->nver_late; i++) {
    const struct VerLate *l = &c->ver_late[i];
    int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, l->cls, l->sg, l->name, l->ar, l->face), absent = 0, e, mm = 0, finds = VER_FINDS_NONE;
    if ((i && !ver_late_cmp(l - 1, l)) || (ix >= 0 && c->ver_entries[ix].absent)) continue;
    if (l->span) {   /* (the build's entry for a call that spans versions, of its face, unless the key's others made one) */
      if (ix < 0 && !(i && !ver_late_key_cmp(l - 1, l)) && (e = ver_entry_make(c, NULL, l->cls, l->cls, l->sg, l->name, l->mi, l->ar, 1, l->face, NULL, &finds, NULL)) >= 0) {
        ver_entry_add(c, c->scopes[e].name, (struct VerEntry){ NULL, VER_ENTRY_CHAIN, l->cls, l->sg, l->ar, e, .finds = finds, .face = l->face });
        made = 1;
      }
      continue;
    }
    for (int j = i; j < c->nver_late && !ver_late_cmp(l, &c->ver_late[j]); j++) mm |= c->ver_late[j].mm;   /* (over every call of the key) */
    int face = l->face, forms = 0;
    e = ver_entry_make(c, &q, l->cls, l->cls, l->sg, l->name, l->mi, l->ar, mm, face, &absent, &finds, &forms);
    if (e == -2) {   /* (a variable-like call's key whose else arm raises nothing of its own: the entry of no face, made once) */
      face = VF_NONE;
      if ((ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, l->cls, l->sg, l->name, l->ar, face)) >= 0 && c->ver_entries[ix].absent) continue;
      e = ver_entry_make(c, &q, l->cls, l->cls, l->sg, l->name, l->mi, l->ar, mm, face, &absent, &finds, &forms);
    }
    if (ix < 0) ver_entry_add(c, e >= 0 ? c->scopes[e].name : l->name, (struct VerEntry){ NULL, VER_ENTRY_CHAIN, l->cls, l->sg, l->ar, absent ? e : -1, absent ? 2 : 1, .finds = finds, .face = face, .anyform = forms });
    else {
      c->ver_entries[ix].absent = absent ? 2 : 1;
      c->ver_entries[ix].anyform = forms;
      if (absent) {   /* (its cells are written where the other was made) */
        c->ver_entries[ix].scope = e;
        c->ver_entries[ix].finds = finds;
        c->ver_scope_face_n = -1;   /* (ver_scope_faced reads the entries again) */
      }
    }
    made |= absent;
  }
  ver_named_facts_free(&q);
  for (int i = 0; i < c->nver_late; i++) free(c->ver_late[i].name);
  c->nver_late = 0;
  return made;
}
/* What the kept versions (Compiler.ver_memo) were answered from of the
   entries: how many, and each one's scope, absence, cells and finds */
static unsigned long ver_entries_sum(const Compiler *c) {
  unsigned long h = (unsigned long)c->nver_entries;
  for (int i = 0; i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    h = h * 1000003ul ^ (unsigned long)(unsigned)e->scope;
    h = h * 1000003ul ^ (unsigned long)(unsigned)(e->absent * 64 + e->cells * 8 + e->finds);
  }
  return h;
}
/* The deferred histories a call names now (desugar_dynamic_send makes an
   arm per name a send of a run-time name may take, and a pass may retarget
   a call in place): asked through the index of the calls by name, which
   follows both (an_calls_named_first), their entries made and their cells
   written now, as the build made the others' (ver_named_asks,
   ver_cells_write); and the entries the typed calls that may meet no
   version ask, and the respond_to?s whose cells stand for such a call
   (ver_late_make). Changed when it made one.
   The versions kept (Compiler.ver_memo) are asked again only where the
   demand moved something they were answered from: a deferred history taken,
   an entry made or changed, a cell written, a scope or a node made or
   edited. A call whose window spans versions on a chain no entry can be
   made of notes it at every ask (ver_absent_note), so a program with one
   such call came here every round of the fixpoint, made nothing, and
   dropped every kept version: each round asked every call of each class
   again, and noted the same calls for the next. */
int ver_entries_demand(Compiler *c) {
  int any = 0, e0 = c->nver_entries, late;
  ver_vis_guard_hists(c);
  for (int i = 0; i < c->nver_defer; i++)
    if (!c->ver_defer[i].taken && an_calls_named_first(c, c->ver_defer[i].name) >= 0) any = c->ver_defer[i].taken = 1;
  /* a respond_to? the cells answer asks the entry a call of the name
     would take where it runs, as such a call asks it (cplan_presence_class) */
  for (int r = 0; r < 3; r++)
    for (int id = an_calls_named_first(c, ver_cells_reader_name(r)); id >= 0; id = an_calls_named_next(id)) {
      const char *nm;
      int pc = ver_cells_reader(c, id, 0, &nm, NULL);
      if (pc >= 0 && cplan_presence_asks(c, id, pc, nm)) call_target(c, id, pc, nm, CT_CMETHOD | CT_CAPTURE, NULL);
    }
  for (int id = an_calls_named_first(c, SINGLETON_METHODS_NAME); id >= 0; id = an_calls_named_next(id))
    list_asks(c, id, list_ver_target, NULL);   /* (a class-side list asks the cells of each name it may list, as respond_to? does) */
  /* A Kernel.String on a Symbol can also meet the builtin before the
     first override. Such calls ask for the same history and cells even
     when no explicit call has asked for the override's entry. */
  int symbol = comp_core_class(c, CORE_SYMBOL);
  if (symbol >= 0)
    for (int id = an_calls_named_first(c, "String"); id >= 0; id = an_calls_named_next(id)) {
      const BopCallback *callbacks;
      int n = kernel_string_implied_calls(c, id, &callbacks);
      for (int i = 0; i < n; i++)
        if (callbacks[i].roles & BCR_ARG) call_target(c, id, symbol, callbacks[i].name, CT_IMPLIED, NULL);
    }
  presence_readers(c, 0, pres_target, NULL);   /* (an instance's presence reader, of the classes it may ask of) */
  late = c->nver_late;
  int new_guard = c->nver_guards && !c->ver_guards[c->nver_guards - 1].cells;
  if (!any && !late && !c->ver_vis_read_new && !new_guard) return 0;
  const NodeTable *nt = c->nt;
  unsigned long sum0 = ver_entries_sum(c), edits0 = nt->edits;
  unsigned ver0 = nt->version;
  int taken = any, scopes0 = c->nscopes, nodes0 = nt->count;
  if (any) ver_named_asks(c, 1);
  any = c->nver_entries > e0;
  if (late) late = ver_late_make(c);
  ver_entries_index(c);
  /* (the presence readers' marks, ver_presence_mark, ask the entries just made) */
  if (any || late) ver_memo_drop(c);
  if (ver_cells_put(c) || taken || any || late || sum0 != ver_entries_sum(c) || scopes0 != c->nscopes ||
      nodes0 != nt->count || ver0 != nt->version || edits0 != nt->edits)
    ver_memo_drop(c);   /* (the versions kept before the entries and the writes are asked again) */
  return any || late || new_guard;
}
