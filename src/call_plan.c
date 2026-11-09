/* call_plan.c -- the user method a call binds, resolved from the tables
   (see call_plan.h). */

#include <assert.h>
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

static CallPlan *g_cp_memo = NULL;
static unsigned char *g_cp_have = NULL;
static int g_cp_cap = 0;
/* the name each memo entry was resolved under (a copy: a rename frees the
   node's string). A node codegen renames for a re-entry (Array#slice emitted
   as #[]) is resolved fresh, and not kept, while its name is not the memo's */
static char **g_cp_name = NULL;

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
  if (!name || nt_int(nt, id, "builtin_only", 0) || nt_int(nt, id, "kernel_recv", 0)) return 0;
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
  int own = call_target(c, id, scls, name, 0, NULL);
  if ((own >= 0 && own != root_mi) || comp_reader_in_chain(c, scls, name, NULL)) return 0;
  int nd = 0, found = 0;
  const int *ds = comp_descendants(c, scls, &nd);
  for (int i = 0; i < nd; i++) {
    int k = ds[i];
    if (k == scls) continue;
    if (comp_reader_in_chain(c, k, name, NULL)) return 0;
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
  const CallPlan *p = cplan_user(c, id);
  if (p->mi < 0 || p->via != UC_INST || !p->chain || p->owner_ci != scls || scls < 0) return 0;
  int rc = c->scopes[p->mi].class_id, oci = comp_core_class(c, CORE_OBJECT);
  return rc < 0 || (rc == oci && scls != oci);
}

static void cplan_set(CallPlan *p, int mi, int owner, int via, int dispatch) {
  p->method_node = -1;
  p->method_call = 0;
  p->send_fallback = -1;
  p->delegate = NULL;
  p->mi = mi; p->owner_ci = (short)owner;
  p->via = (unsigned char)via; p->dispatch = (unsigned char)dispatch;
  p->by_name = 0;
  p->chain = 0;
  p->absent = 0;
  p->rkind = CR_NONE; p->msg = NULL; p->rfrom = CRF_NONE;
  p->core_hook = 0;
  p->answers_receiver = 0;
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
  if (!name || nt_int(c->nt, id, "builtin_only", 0)) return 0;
  static const int CN[2] = { CORE_TRUECLASS, CORE_FALSECLASS };
  for (int k = 0; k < 2; k++) {
    int ci = comp_core_class(c, CN[k]);
    int mi = ci >= 0 ? call_target(c, id, ci, name, 0, NULL) : -1;
    if (mi >= 0 && c->scopes[mi].class_id == ci) *(k ? fmi : tmi) = mi;
  }
  return *tmi >= 0 || *fmi >= 0;
}

/* a program's own Object method, for a name Object's builtin surface does
   not answer itself, that call `call` runs */
static void cplan_object_reopen(Compiler *c, int call, const char *name, CallPlan *p) {
  if (builtin_object_method_known(name)) return;
  int oci = comp_core_class(c, CORE_OBJECT);
  int omi = oci >= 0 ? call_target(c, call, oci, name, 0, NULL) : -1;
  if (omi >= 0) cplan_set(p, omi, oci, UC_REOPEN, CP_DIRECT);
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
  if (mi < 0 || comp_method_vis_declared(c, oc, name, NULL) == SP_VIS_PRIVATE ||
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
/* A super's parent-chain target, past inherited copies of the same
   extended definition. Shared by resolution, inference, inline splicing and
   the miss check. Every step is call_target's, with CT_REACH: it skips the
   ask of the calls' names (ct_calls), so call_node_names, which runs
   comp_super_name for this very name, is not entered again, and it asks the
   record, a shadow's name too. ml->from is the par asked, though the
   extend-copy skip and the Object start look further; ml->def_class is the
   class that answered, -1 on a miss. */
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
  /* (a superclass chain ends in Object: a reopening of it is the last
     class the super looks in) */
  if (mi < 0 && par >= 0 && !s->is_cmethod) {
    int top = par;
    while (c->classes[top].parent >= 0) top = c->classes[top].parent;
    int obj = comp_super_parent(c, top, 0);
    /* (not under BasicObject: Object is not in that chain; not under another
       builtin: its methods come first, cplan_root_under_builtin) */
    if (obj >= 0 && obj != top && !class_is_blank_slate(c, top) && !cplan_root_under_builtin(c, top))
      mi = call_target(c, id, obj, name, CT_REACH, ml);
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
static int super_layer_entry(Compiler *c, int id, int t);
int cplan_super_target(Compiler *c, int id, Scope *s0, MethodLookup *ml) {
  const Scope *s = comp_super_site(c, s0);
  const char *shadow = comp_super_shadow(c, s);
  MethodLookup own = { .def_class = -1 };
  if (!ml) ml = &own;
  if (shadow) {
    int mi = call_target(c, id, s->class_id, shadow, CT_OWN | CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), ml);
    ml->from = mi >= 0 ? s->class_id : -1;
    int e = mi >= 0 && id >= 0 && c->ver_built ? super_layer_entry(c, id, mi) : -1;
    if (e >= 0) { ml->kind = ML_ENTRY; ml->why = 0; }
    return e >= 0 ? e : mi;
  }
  int par = comp_super_parent(c, s->class_id, s->is_cmethod);
  const char *uname = comp_super_name(c, par, s);
  if (!uname) {
    *ml = (MethodLookup){ .kind = ML_NONE, .def_class = -1, .from = par };
    return -1;
  }
  return cplan_super_chain_mi(c, id, s, par, uname, ml);
}

static void cplan_resolve_super(Compiler *c, int id, CallPlan *p) {
  Scope *s = comp_scope_of(c, id);
  if (!s || s->class_id < 0 || !s->name) return;
  MethodLookup ml = { .def_class = -1 };
  int mi = cplan_super_target(c, id, s, &ml);
  if (mi >= 0) { cplan_set(p, mi, ml.from, UC_SUPER, CP_DIRECT); return; }
  const Scope *st = comp_super_site(c, s);
  const char *uname = comp_super_shadow(c, st) ? NULL : comp_super_name(c, ml.from, st);
  if (uname && s->is_cmethod &&
           builtin_super_cmethod_known(c->classes[s->class_id].name, uname) &&
           ml.kind == ML_NONE) {
    cplan_set(p, -1, s->class_id, UC_SUPER, CP_REFUSE);
    p->rkind = CR_FEATURE; p->rfrom = CRF_LIMIT;
    p->msg = "super to an inherited builtin singleton method is not supported; see docs/limitations.md";
  } else if (uname && s->is_cmethod && is_definition_hook_name(uname))
    p->core_hook = 1;   /* Class#inherited and the like: a no-op that reads no class and calls no method */
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
  int reader = !cmeth && comp_reader_in_chain(c, cid, name, NULL);
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
  if (!name || recv < 0 || nt_int(nt, id, "builtin_only", 0)) return 0;
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
  if (nt_int(nt, id, "builtin_only", 0)) return;
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
    int imi = comp_included_method_index(c, name, id);
    if (imi >= 0) { cplan_set(p, imi, c->scopes[imi].class_id, UC_INCLUDED, CP_DIRECT); return; }
    /* outside a class method, self reaches a program's own Object method */
    Scope *self = comp_scope_of(c, id);
    if (!(self && self->is_cmethod)) cplan_object_reopen(c, id, name, p);
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
  if (rt == TY_CLASS && !nt_int(nt, id, "builtin_only", 0)) {
    int kci = comp_core_class(c, CORE_CLASS);
    int kmi = kci >= 0 ? call_target(c, id, kci, name, 0, NULL) : -1;
    if (kmi >= 0) { cplan_set(p, kmi, kci, UC_REOPEN, CP_DIRECT); return; }
  }
  int sci = self_class_static_ci(c, recv);   /* `self.class` naming one class */
  int bci = rk == NK_SelfNode ? class_body_self_ci(c, recv) : -1;   /* a class body's self */
  if (sci >= 0 || bci >= 0 || (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)) {
    int ci = sci >= 0 ? sci : bci >= 0 ? bci : const_node_class_index(c, recv);
    /* (a `new`: the one the call reaches, as inference and the static
       emitter take it: cplan_user_new) */
    int mi = ci < 0 ? -1 : is_new_name(name) ? cplan_user_new(c, id, ci) : call_target(c, id, ci, name, CT_CMETHOD, NULL);
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
    for (int i = 0; i < ncc; i++)
      if (ccs[i].mi >= 0) {
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
    const char *via_op = comp_subclass_answers(c, cid, name) ? NULL : builtin_operator_delegate(name);
    if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
    int dmi = via_op ? call_target(c, id, cid, via_op, 0, NULL) : -1;
    if (dmi < 0 && via_op) {
      via_op = builtin_operator_delegate(via_op);
      if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
      dmi = via_op ? call_target(c, id, cid, via_op, 0, NULL) : -1;
    }
    if (dmi >= 0) {
      cplan_set(p, dmi, cid, UC_INST, cplan_dispatch_form(c, id, cid, via_op, 1));
      p->delegate = via_op;
      return;
    }
    /* a reader a subclass overrides with a method: the switch reaches the
       override for that subclass */
    if (comp_reader_in_chain(c, cid, name, NULL)) {
      int nd = 0;
      const int *ds = comp_descendants(c, cid, &nd);
      for (int i = 0; i < nd; i++) {
        int kmi = -1;
        if (ds[i] != cid) comp_resolve_member(c, ds[i], name, 0, NULL, &kmi);
        if (kmi >= 0) { cplan_set(p, kmi, cid, UC_INST, CP_SWITCH); return; }
      }
    }
    if (class_is_exc_subclass(c, cid)) cplan_exc_reopen(c, id, name, p);
    if (p->mi < 0) {
      cplan_object_reopen(c, id, name, p);
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
    if (p->mi < 0 && !comp_reader_in_chain(c, cid, name, NULL) && !c->classes[cid].is_native_class)
      cplan_descendant_call(c, id, cid, name, 0, p);
    return;
  }
  if (rt == TY_POLY) {
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    if (cplan_native_block_possible(c, id, pcs, npc)) {
      const PolyPlan *bp = cplan_poly_block(c, id);
      for (int i = 0; i < bp->n; i++)
        if (bp->arm[i].mi >= 0) {
          cplan_set(p, bp->arm[i].mi, bp->arm[i].key, UC_POLY, CP_SWITCH);
          return;
        }
    }
    cplan_resolve_poly(c, id, name, p, pcs, npc);
    return;
  }
  /* a reopening of the exception classes that defines the name stands for
     the call; otherwise the call goes on to the Object fallback below */
  if (rt == TY_EXCEPTION) { cplan_exc_reopen(c, id, name, p); if (p->mi >= 0) return; }
  if (nt_int(nt, id, "builtin_only", 0)) return;
  /* a call written under a builtin alias (detect, collect_concat) that the
     desugar renamed to the builtin's own name reaches a reopen's method of
     the name it was written with, not of the new name */
  const char *rname = nt_str(nt, id, "orig_name") ? nt_str(nt, id, "orig_name") : name;
  /* an IO handle's reopen: the File, IO or socket class that defines it */
  if (rt == TY_IO) {
    int eci = io_reopen_class(c, name);
    int emi = eci >= 0 ? call_target(c, id, eci, name, 0, NULL) : -1;
    if (emi >= 0) {
      cplan_set(p, emi, eci, UC_REOPEN, io_reopen_ret_mixed(c, name) ? CP_SWITCH : CP_DIRECT);
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
    const char *own = rt == TY_NIL ? "NilClass" : rt == TY_FLOAT ? "Float" : "Integer";
    const char *anc = rt == TY_NIL ? "NilClass" : "Numeric";
    if (!builtin_method_known(own, name) && !builtin_object_method_known(name)) {
      int aci = comp_builtin_class_named(c, anc);
      int ami = aci >= 0 ? call_target(c, id, aci, name, 0, NULL) : -1;
      if (ami >= 0 && c->scopes[ami].class_id == aci) { cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT); return; }
    }
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
  /* last, a program's own Object method for a name the receiver's builtin
     class and Object's own surface do not have (object_reopen_answers) */
  int bcls = object_fallback_class_of_type(rt);
  const char *bn = rt == TY_BOOL ? "TrueClass" : bcls >= 0 ? core_class_name(bcls) : NULL;
  if (bn && !builtin_answers_ahead_of(bn, name, "Object")) cplan_object_reopen(c, id, name, p);
}

static void cplan_resolve(Compiler *c, int id, CallPlan *p) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) cplan_resolve_super(c, id, p);
  else if (k == NK_CallNode) {
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
  }
}

/* A constant call's user singleton target, shared by inference, rewrites
   and builtin emitters. Unregistered constants need no target lookup.
   Ordinary user classes never need this precedence decision; native package
   bindings supply their own builtin names. The caller supplies the receiver
   node; a receiver that is not a constant has no target. */
const CallPlan *cplan_const_user(Compiler *c, int id, int recv, int fresh) {
  const NodeTable *nt = c->nt;
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_NONE;
  const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? const_key_at(c, recv) : NULL;
  if (!cn || c->nclasses == 0) return NULL;
  int ci = comp_class_index(c, cn);
  if (ci < 0) return NULL;
  if (!c->classes[ci].is_native_class && !c->classes[ci].is_builtin_const &&
      (c->n_native_funcs == 0 || comp_native_find(c, cn, nt_str(nt, id, "name")) < 0) &&
      (c->n_ffi_funcs == 0 || ffi_find_func(c, cn, nt_str(nt, id, "name")) < 0)) return NULL;
  if (nt_int(nt, id, "builtin_only", 0)) return NULL;
  const CallPlan *p = fresh ? cplan_user_fresh(c, id) : cplan_user(c, id);
  return p->mi >= 0 && p->via == UC_CMETH && p->dispatch == CP_DIRECT ? p : NULL;
}

/* Ask the native-block dispatch of the call once into blk (see
   PolyBlockMembers); the same tests cplan_resolve_call makes for a poly
   receiver. */
static void cplan_block_members(Compiler *c, int id, const PolyCand *pcs, int npc, PolyBlockMembers *blk) {
  blk->state = 1;
  blk->n = 0;
  if (!cplan_native_block_possible(c, id, pcs, npc)) return;
  const PolyPlan *bp = cplan_poly_block(c, id);
  blk->state = 2;
  for (int i = 0; i < bp->n; i++) {
    int arm = bp->arm[i].mi;
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
      const PolyPlan *bp = cplan_poly_block(c, id);
      for (int i = 0; i < bp->n; i++)
        if (bp->arm[i].mi == mi || scope_proc_form_of(c, bp->arm[i].mi) == mi) return 1;
    }
    return cplan_cand_arm(c, id, name, mi, 1);
  }
  if (p->via == UC_REOPEN) {
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv >= 0 && comp_ntype(c, recv) == TY_IO) {
      int ks[16], nk = io_reopen_defs(c, name, 0, ks, 16);
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
  if (!g_plan_check || mi >= 0 || ci < 0 || !name || nt_int(c->nt, id, "builtin_only", 0)) return;
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
  if (mi >= 0 && c->scopes[mi].entry) p.dispatch = CP_SWITCH;
  p.absent = capture ? (unsigned char)cplan_capture_absent(c, id, mi) : 0;
  if (p.absent && mi < 0) p.owner_ci = (short)cm_capture_class(c, id, NULL);
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
  int mw = sup >= 0 && cplan_user(c, sup)->mi < 0 && !cplan_user(c, sup)->core_hook ? cplan_capture_missing(c, id) : -1;
  return mw >= 0 ? mw : t;
}

/* A literal-name arm emitted on boxed self uses the existing boxed
   candidates. Its record must not admit new arms to the outer send. */
CallPlan cplan_send_arm(Compiler *c, int id) {
  CallPlan p = *cplan_user_fresh(c, id);
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

CallPlan cplan_conversion(Compiler *c, int ci, const char *name) {
  CallPlan p;
  int mi = comp_method_in_chain(c, ci, name, NULL);
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
    int mi8 = comp_method_in_chain(c, xcm, "message", NULL);
    if (mi8 < 0) mi8 = comp_method_in_chain(c, xcm, "to_s", NULL);
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
      int ko = comp_method_in_class(c, k, "to_s");
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
static const CallPlan *cplan_dep(const CallPlan *p) {
  if (p->mi >= 0) IMEMO_DEP_SCOPE(p->mi);
  if (p->send_fallback >= 0) IMEMO_DEP_SCOPE(p->send_fallback);
  if (p->owner_ci >= 0) IMEMO_DEP_CLASS(p->owner_ci);
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
int call_node_names(Compiler *c, int id, const char *out[2]) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, id);
  int n = 0;
  if (k == NK_CallNode || k == NK_CallTargetNode) out[n++] = nt_str(nt, id, "name");
  else if (k == NK_CallOperatorWriteNode || k == NK_CallOrWriteNode || k == NK_CallAndWriteNode) {
    out[n++] = nt_str(nt, id, "read_name");
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
/* does node id itself call `name` (not a method the language calls for it)? */
static int ct_calls(Compiler *c, int id, const char *name) {
  /* Capture asks explicitly; an ordinary call need not enumerate the
     construction names used by entry discovery on every target query. */
  if (nt_kind(c->nt, id) == NK_CallNode) {
    const char *nm = nt_str(c->nt, id, "name");
    return nm && sp_streq(nm, name);
  }
  const char *nm[2];
  int n = call_node_names(c, id, nm);
  for (int i = 0; i < n; i++) if (nm[i] && sp_streq(nm[i], name)) return 1;
  return 0;
}
/* The method scope of version d of `name` for a call on class rcls (class
   side when sg; main when rcls < 0), as the record answered it: a def's
   scope (rcls's copy of it, when a mixin copied it); for a module_function
   copy, the scope of the def it copies; for an alias, the version of its
   target (a def, an alias or a copy) live where the alias was made, in the
   class whose body made it; for a class-side accessor a later def
   replaces, the method made of it (elab_attr_scope). -1 for any other
   version (a define_method's or its copy, any other attribute's) or an
   alias made where the record has no point. A chain of aliases ends: each
   step asks for the version live at an alias's point, one recorded ahead
   of that point, so the points only go back. */
static int ct_def_version(Compiler *c, int rcls, const char *name, int sg, const LrDef *d, int *dc) {
  int cls = rcls;
  while (d) {
    if (d->kind == LR_DEF) return comp_method_version(c, rcls, name, sg, d->node, dc);
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
static int ver_entry_ix(const Compiler *c, int guard, int from, int rcls, int sg, const char *name, int ar) {
  if (!c->ver_ix_nb) return -1;
  for (int i = c->ver_ix[ver_key_hash(rcls, sg, name, ar) % c->ver_ix_nb]; i >= 0; i = c->ver_ix_next[i]) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == guard && e->from == from && e->cls == rcls && e->sg == sg && e->ar == ar && sp_streq(e->name, name)) return i;
  }
  return -1;
}
static int ct_entry_of(const Compiler *c, int guard, int rcls, int sg, const char *name, int ar) {
  int i = ver_entry_ix(c, guard, 0, rcls, sg, name, ar);
  return i >= 0 ? c->ver_entries[i].scope : -1;
}
static void ver_cell_name(int hist, char out[32]) {
  snprintf(out, 32, "$__spinel_ver_%d", hist + 1);
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
  return s->versioned || (name && s->name && !sp_streq(s->name, name) && elab_method_versions(c, name) > 1);
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
static int ver_arity(Compiler *c, int id, const int *to, int nto) {
  int n = ver_call_n(c, id);
  int a = nt_ref(c->nt, id, "arguments"), ac = 0;
  const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &ac) : NULL;
  /* not the versions' required ones: a boxed receiver's arm cannot gather a splat into __cond0.. (arg_layout_gathers) */
  if (nt_argv_has(c->nt, av, ac, NT_ARG_SPLAT)) return -1;
  int ar = ver_plain_arity(c, to[0]), fits = 1;
  for (int i = 0; i < nto; i++) {
    if (ver_plain_arity(c, to[i]) != ar) ar = -1;
    fits &= ver_takes(c, to[i], n);
  }
  return ar >= 0 ? ar : fits ? n : -1;
}
/* ...for a version entry of `name` on rcls (mi, the table's answer): the
   versions an entry arms (ver_entry_make) */
static int ver_call_arity(Compiler *c, int id, int rcls, int sg, const char *name, int mi) {
  ElabVersion vs[VER_MAX_ARMS];
  int to[VER_MAX_ARMS + 1], nto = 0, dc;
  int nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS);
  to[nto++] = mi;
  for (int i = 0; i < nv; i++) {
    int s = vs[i].v->kind == LR_REMOVE || vs[i].v->kind == LR_UNDEF ? -1 : ct_def_version(c, rcls, name, sg, vs[i].v, &dc);
    if (s >= 0) to[nto++] = s;   /* (no entry arms another: ver_entry_make) */
  }
  return ver_arity(c, id, to, nto);
}
/* guard_chain's answer kept for mi (Compiler.guard_memo), into to: their
   count, 0 for none kept */
static int guard_memo_get(Compiler *c, int mi, int *to) {
  const struct GuardMemo *g = &c->guard_memo;
  if (mi >= g->nat || !g->at[mi]) return 0;
  const int *p = g->to + g->at[mi] - 1;
  memcpy(to, p + 1, sizeof *to * (size_t)p[0]);
  return p[0];
}
static void guard_memo_put(Compiler *c, int mi, const int *to, int n) {
  struct GuardMemo *g = &c->guard_memo;
  if (mi >= g->nat) {
    int nat = c->nscopes > mi ? c->nscopes : mi + 1, *at = realloc(g->at, sizeof *at * (size_t)nat);
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
  int memo = !arm && c->ver_built, asked = mi, n = memo ? guard_memo_get(c, mi, to) : 0;
  if (n) return n;
  if (c->scopes[mi].entry == ENTRY_TABLE) mi = c->scopes[mi].guard_behind;
  while (n < VER_MAX_ARMS && c->scopes[mi].mixin_guarded && c->scopes[mi].guard_behind >= 0) {
    if (arm) {
      ver_prepend_cell_name(c->scopes[mi].mixin_guarded, sel[n]);
      arm[n] = (DispArm){ sel[n], 1, c->scopes[mi].name, 0 };
    }
    to[n++] = mi;
    mi = c->scopes[mi].guard_behind;
  }
  /* A capture before the prepend can also precede a redefinition of the
     method behind it. Its replaced definitions use the same version
     cells as an ordinary version entry, after the prepend arms. */
  const Scope *m = &c->scopes[mi];
  const char *name = m->def_node >= 0 ? nt_str(c->nt, m->def_node, "name") : NULL;
  if (name && ct_version_named(c, name)) {
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
        ver_cell_name(vs[i].hist, sel[n]);
        arm[n] = (DispArm){ sel[n], vs[i].n, name, vs[i].hist + 1 };
      }
      to[n++] = s;
    }
  }
  to[n++] = mi;
  if (memo) guard_memo_put(c, asked, to, n);
  return n;
}
static int mixin_copy_versions(Compiler *c, int mi, int from, int *to, ElabVersion *vs);
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
       guard_layer_versions) */
    if (p.region >= 0 && elab_method_at(c, m->class_id, name, m->is_cmethod, p, &d) == ELAB_YES && d)
      for (int i = 0; i < n; i++) {
        ElabVersion vs[VER_MAX_ARMS];
        int vt[VER_MAX_ARMS], nv = c->scopes[to[i]].mixin_guarded ? mixin_copy_versions(c, to[i], m->class_id, vt, vs) : 0;
        if (!nv) vt[nv++] = to[i];
        for (int k = 0; k < nv; k++)
          if (c->scopes[vt[k]].def_node == d->node) {
            *ml = (MethodLookup){ .kind = ML_METHOD, .n = 1, .def_class = ml->def_class };
            return vt[k];
          }
      }
  }
  int e = c->ver_built ? ct_entry_of(c, VER_ENTRY_GUARD, m->class_id, m->is_cmethod, name, ver_arity(c, id, to, n)) : -1;
  *ml = (MethodLookup){ .kind = e >= 0 ? ML_ENTRY : ML_TABLE, .why = e >= 0 ? 0 : MLW_MANY, .n = (short)n, .def_class = ml->def_class };
  return e >= 0 ? e : mi;   /* a later synthesized call uses the general table entry */
}
/* The version an entry's arm (or else arm) call is pinned to, -1 for any
   other node, CT_PIN_BUILTIN for an else arm that runs the builtin
   (VK_BUILTIN): the pins are kept in the order of their nodes (ver_pin) */
enum { CT_PIN_BUILTIN = -2 };
static int ct_pin_of(const Compiler *c, int id) {
  int lo = 0, hi = c->nver_pins - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2, at = c->ver_pins[mid].node;
    if (at == id) return c->ver_pins[mid].scope;
    if (at < id) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
}
int cplan_pinned_builtin(const Compiler *c, int id) { return ct_pin_of(c, id) == CT_PIN_BUILTIN; }
int cplan_pinned(const Compiler *c, int id) { return ct_pin_of(c, id) != -1; }
/* Is version vi a top-level method (Object's) that a call asked on a
   class's chain reaches past the class's own versions? The class's table
   holds no such method: the call's top-level ask (CT_TOP) binds it. */
static int ct_top_version(const Compiler *c, int vi, unsigned flags) {
  return !(flags & CT_TOP) && c->scopes[vi].class_id < 0;
}
/* (mixed through the multiply and shifts of a finalizer: the keys run over
   small ranges, and the sum or xor of their products fell in long runs) */
static unsigned ver_mix(unsigned h) {
  h ^= h >> 15; h *= 0xC2B2AE35u; h ^= h >> 13;
  return h;
}
/* How many versions rcls's chain holds of name (-1 past VER_MAX_ARMS),
   whether the first is a plain def, whether a method body makes one
   (LrDef.body: a call may run before that body does, where the chain may
   hold none of them), and the late bit of the chain's histories
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
  for (int k = 0; k < n; k++) any |= vs[k].v->body != 0;
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
/* The method call id meets as cls's `name` (singleton when sg) wherever
   it runs: the one version of the name on cls's chain over the statements
   id may run in (elab_call_span), a def that ran there for sure, as
   cls's copy of it when a mixin brought it (ct_def_version);
   -1 where the record cannot prove one -- no version yet at some point of
   the window (a def or the include that brings it comes later), more than
   one, or one that may not have run (under a condition, or after a
   statement that may raise in a begin body with rescue clauses:
   elab_point_partial) */
static int ct_window_version(Compiler *c, int id, int cls, const char *name, int sg) {
  const LrDef *ds[2];
  int n = elab_call_span(c, id, cls, name, sg, ct_after_point(c, id), NULL, ds, 2);
  if (n != 1 || !ds[0] || ds[0]->kind != LR_DEF || ds[0]->may || ds[0]->unplaced ||
      elab_point_partial(c, ds[0]->node)) return -1;
  int dc = -1;
  return ct_def_version(c, cls, name, sg, ds[0], &dc);
}
/* How call id asks the record for the versions it may meet
   (elab_call_meet): after its point where it runs later, and as the
   mixin's copy's call in a mixin's copy of its body */
static int ct_meet_how(Compiler *c, int id) {
  const Scope *own = comp_scope_of(c, id);
  return ct_after_point(c, id) | (own && own->origin_module_ci > 0 ? ELAB_MEET_COPY : 0);
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
   builtin class cls up to own: the method of a def a reopen gives it
   there, -1 where none is live, -2 where the record cannot tell) */
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
    last = !own || sp_streq(p, own);
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
  const char *cl = dn >= 0 ? nt_str(nt, dn, "bam_classes") : NULL;
  int site = dn >= 0 ? (int)nt_int(nt, dn, "bam_site", 0) - 1 : -1, nc = 0, r = -1;
  if (!sym || !cl || site < 0) return -1;
  char *cls = bam_names_split(cl, &nc);
  int framed = elab_plan_enter(c, site);
  const char *p = cls;
  for (int i = 0; i < nc; i++, p += strlen(p) + 1) {
    int ri = bam_reopen_walk(c, site, p, sym, own);
    r = i == 0 || ri == r ? ri : -2;
  }
  if (framed) elab_plan_leave(c);
  free(cls);
  return r;
}
/* (the class whose accessor lists answer cls's `name` on its chain, a
   writer's as `x=`; -1 for none) */
static int ct_accessor_class(Compiler *c, int cls, const char *name) {
  int dc = -1;
  char base[256];
  int found = setter_base_name(name, base, sizeof base) ? comp_writer_in_chain(c, cls, base, &dc)
                                                         : comp_reader_in_chain(c, cls, name, &dc);
  return found ? dc : -1;
}
/* (the Struct or the Data whose member cls's `name` is, the class
   answering it on cls's chain; -1 for none) */
static int ct_struct_member(Compiler *c, int cls, const char *name) {
  int dc = ct_accessor_class(c, cls, name);
  return dc >= 0 && (c->classes[dc].is_struct || c->classes[dc].is_data) ? dc : -1;
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
   NTI_ word): a row of tools/implied_calls.yml says so (nt_implied_calls),
   the rows the load record reads */
static int ct_implied_row(Compiler *c, int id, const char *name, int op) {
  NtImplied rows[16];
  int n = nt_implied_calls(c->nt, id, rows, 16);
  for (int i = 0; i < n && i < 16; i++)
    if (rows[i].operand == op && sp_streq(rows[i].name, name)) return 1;
  return 0;
}
/* The method the implied call `name` node id makes on operand op (a row:
   a multiple assignment's to_ary of its value, ct_implied_row) runs on an
   instance of cls: the version the call meets there (ct_window_version);
   -1 where no row makes the call, or the record cannot prove one. The
   call is lowered as a call of `name`, which binds through the method
   table: where an undef of the name on cls's chain leaves the table
   without it (comp_is_undeffed_in_chain), that call is not the version
   proved, and there is no answer either. */
static int ct_implied_version_at(Compiler *c, int id, const char *name, int op, int cls) {
  if (cls < 0 || !ct_implied_row(c, id, name, op) || comp_is_undeffed_in_chain(c, cls, name)) return -1;
  return ct_window_version(c, id, cls, name, 0);
}
/* ...asked in the record's frame for the call plan (elab_plan_enter), as
   ct_version asks: the passes that ask it append nodes between the asks */
int cplan_implied_version(Compiler *c, int id, const char *name, int op, int cls) {
  int framed = elab_plan_enter(c, id), vi = ct_implied_version_at(c, id, name, op, cls);
  if (framed) elab_plan_leave(c);
  return vi;
}
/* The classes whose own to_ary the implied to_ary call node id makes on
   operand op reaches when the operand is boxed (a multiple assignment's
   value, the value a block of several parameters takes apart): each
   class a box can hold (comp_poly_candidates) whose no-argument to_ary is
   the version the record proves there (cplan_implied_version). An Array
   subclass is an Array already. A nested target's element converts at
   its own level, as its MultiTargetNode's implied to_ary of the element
   it takes (emit_massign_poly_target). An operand node of a program
   class's type (op >= 0) is boxed as that object, and reaches only the
   to_ary of its class or a subclass; one of another type (a Hash, a
   scalar) none. Into out[0..cap), ascending; the count. */
int cplan_boxed_to_ary_classes(Compiler *c, int id, int op, int *out, int cap) {
  int n = 0, np = 0;
  TyKind ot = op >= 0 ? comp_ntype(c, op) : TY_POLY;
  int oc = ty_is_object(ot) ? ty_object_class(ot) : -1;
  if (oc < 0 && ot != TY_POLY) return 0;
  const PolyCand *pc = comp_poly_candidates(c, "to_ary", &np);
  for (int i = 0; i < np && n < cap; i++) {
    int k = pc[i].cls, mi = comp_implicit_to_ary_mi(c, k);
    if (pc[i].native || mi < 0 || mi != pc[i].mi || c->scopes[mi].nparams != 0 || !c->classes[k].instantiated) continue;
    if (oc >= 0 && !is_descendant(c, k, oc)) continue;
    if (cplan_implied_version(c, id, "to_ary", op, k) == mi) out[n++] = k;
  }
  return n;
}

/* See call_plan.h. */
int cplan_index_writer(Compiler *c, int cls, int *cdef) {
  int dc = -1;
  int wmi = cls >= 0 ? comp_method_in_chain(c, cls, "[]=", &dc) : -1;
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
static int cps_key_callbacks_run_code(Compiler *c, TyKind rt, const int *keys, int n) {
  const char *rc = ty_is_obj_array(rt) ? "Array" : builtin_class_of_type(rt);
  const BopCallback *cb = NULL;
  int nc = rc ? bop_class_callbacks(rc, "[]=", &cb) : 0;
  if (nc < 0) return 1;
  for (int i = 0; i < nc; i++) {
    if (!(cb[i].roles & BCR_ARG)) continue;
    for (int j = 0; j < n; j++) {
      TyKind kt = comp_ntype(c, keys[j]);
      if (ty_is_object(kt) && comp_method_in_chain(c, ty_object_class(kt), cb[i].name, NULL) >= 0) return 1;
      if (kt != TY_POLY && kt != TY_UNKNOWN) continue;
      int np = 0;
      const PolyCand *pc = comp_poly_candidates(c, cb[i].name, &np);
      for (int q = 0; q < np; q++) if (!pc[q].native) return 1;
    }
  }
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
    return open || (nm && cls >= 0 && call_target(c, tgt, cls, nm, 0, &ml) >= 0 && ml.def_class >= 0) ? CPS_CODE : 0;
  }
  int args = nt_ref(nt, tgt, "arguments"), n = 0;
  const int *keys = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
  if (n < 1) return 0;
  if (open || cplan_index_writer(c, cls, NULL) >= 0) return CPS_CODE;
  if (cls >= 0) return 0;
  unsigned e = cps_key_callbacks_run_code(c, rt, keys, n) ? CPS_CODE : 0;
  if ((ty_is_array(rt) || ty_is_obj_array(rt)) && bop_name_mutates("[]=", BOP_MUT_CONTAINER)) e |= CPS_CHANGES_ARRAY;
  return e;
}
/* A chain a typed call may meet with no version made yet (ct_version_at),
   kept for ver_entries_demand to make its entry (Compiler.ver_late) */
static void ver_absent_note(Compiler *c, int rcls, int sg, const char *name, int mi, int ar, int mm, int span) {
  if (!(c->nver_late & (c->nver_late - 1)) &&
      !(c->ver_late = realloc(c->ver_late, sizeof *c->ver_late * (size_t)(c->nver_late ? 2 * c->nver_late : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  c->ver_late[c->nver_late++] = (struct VerLate){ name_dup(name), rcls, sg, mi, ar, mm, span };
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
static int ct_version_at(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi, MethodLookup *ml) {
  int sg = (flags & CT_CMETHOD) != 0, rcls = flags & CT_TOP ? -1 : cls, vi = -1, dc = -1, n = 0, first_def = 0, none = 0, body = 0, cl = 0;
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
  int late = (cl == 2 || body) && !ct_capture_keeps_table(c, id, rcls, sg, name, flags, 1);
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
  int kept = ct_after_point(c, id);   /* (it runs at its point only if nothing around it runs later) */
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
  if (at == ELAB_YES) vi = ct_def_version(c, rcls, name, sg, d, &dc);
  else if (at == ELAB_NO) none = late && !d && elab_class_live_at(c, rcls, sg, p);   /* (not an undef's, nor on a class no call can run on yet) */
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
    int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, c->scopes[mi].origin_module_ci, rcls, sg, name, ver_call_arity(c, id, rcls, sg, name, mi));
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
    int ar = ver_call_arity(c, id, rcls, sg, name, mi), ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, rcls, sg, name, ar);
    int many = n >= 2 || n == ELAB_SPAN_ANY;
    int settled = at == ELAB_NO || elab_chain_settled(c, rcls, sg, name, w.from);
    const Scope *in = comp_scope_of(c, id);
    /* (an entry only where the call takes it, in a body that may run) */
    if (none && settled && in && comp_scope_may_run(c, (int)(in - c->scopes)) && c->ver_built && !ml->survey && (ix < 0 || !c->ver_entries[ix].absent))
      ver_absent_note(c, rcls, sg, name, mi, ar, ver_mm_at(c, rcls, sg, p, at, &w), 0);
    /* a call the build did not ask (one in a method made after it) whose
       window spans versions asks the entry the build makes for such a call */
    else if (many && ix < 0 && settled && in && comp_scope_may_run(c, (int)(in - c->scopes)) && c->ver_built && !ml->survey)
      ver_absent_note(c, rcls, sg, name, mi, ar, 0, 1);
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
  if (ct_top_version(c, vi, flags)) {
    ml->kind = ML_NONE;
    return -1;
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
  if (memo) {
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
   for CT_CMETHOD, main's top-level functions for CT_TOP? The method the
   tables answer is then one a later statement defines, and the call
   raises (cplan_missing) or reaches the builtin self answers
   (cplan_self_builtin). Asked in the record's frame, once it is made.
   Only for a class's class methods and main's functions: a call on an
   instance keeps its tables' target, which its dispatch arms are built
   on, and so does a call on an explicit receiver of a name a builtin
   answers on any self (`U.format`, private in CRuby). */
/* ...and a builtin class's reopening, on its instance side: a call the
   record proves runs before the reopening's method of `name` is defined
   reaches the builtin's method of the name, as in CRuby (#10719: `p
   a.sum` ahead of `class Array; def sum`), where the builtin has one, of
   its own or of a module it includes (Comparable's between?, Enumerable's
   tally) */
static int ct_builtin_reopen_undefined_at(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if ((flags & (CT_CMETHOD | CT_TOP | CT_OWN)) || cls < 0 || cls >= c->nclasses || !c->classes[cls].name ||
      c->classes[cls].enclosing_class >= 0 || builtin_class_id(c->classes[cls].name) >= 0 ||
      !builtin_answers_ahead_of(c->classes[cls].name, name, NULL) ||
      !elab_method_versions(c, name))
    return 0;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, cls, name, 0, 1) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none;
}
static int ct_undefined_at(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if (ct_builtin_reopen_undefined_at(c, id, cls, name, flags)) return 1;
  if (!(flags & (CT_CMETHOD | CT_TOP)) || !elab_method_versions(c, name)) return 0;
  /* (a capture that cannot state the NameError keeps the table's method
     on a chain a method body makes a version on, as in ct_version_at) */
  int rcls = flags & CT_TOP ? -1 : cls, sg = (flags & CT_CMETHOD) != 0, first_def = 0, body = 0, cl = 0;
  if (ct_capture_keeps_table(c, id, rcls, sg, name, flags, 0) && (ver_chain_count(c, rcls, sg, name, &first_def, &body, &cl), body))
    return 0;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, flags & CT_TOP ? -1 : cls, name, (flags & CT_CMETHOD) != 0, 1) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none && !(nt_ref(c->nt, id, "receiver") >= 0 && !nt_int(c->nt, id, "self_call", 0) &&
                   builtin_self_method_known(name, cm_self_kind(c, flags & CT_TOP ? -1 : cls)) &&
                   !((flags & CT_CMETHOD) && elab_record_cmethod(c, cls, name)));
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
  if (id < 0 || !elab_attr_replaced(c, ci, name)) return CP_SG_NONE;
  mtab_enter(c);
  int m = call_target(c, id, ci, name, CT_CMETHOD, NULL), r;
  MethodLookup told = { .survey = 1, .def_class = -1 };
  if (m < 0 || c->scopes[m].attr_version) r = CP_SG_SLOT;
  else {
    if (!c->scopes[m].entry) ct_version(c, id, ci, name, CT_CMETHOD, m, &told);
    r = told.n < 0 ? CP_SG_UNTOLD : CP_SG_METHOD;
  }
  mtab_leave(c);
  return r;
}
/* See call_plan.h. */
int cplan_sg_slot_call(Compiler *c, int id, int ci, const char *name) {
  char slot[256];
  const char *nm = id >= 0 && nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  return nm && name && sp_streq(nm, name) && cplan_sg_accessor(c, id, ci, name, slot, sizeof slot) == CP_SG_SLOT;
}
/* See call_plan.h. */
int cplan_cmethod_later(Compiler *c, int id, int cls, const char *name) {
  int asked = id >= 0 ? (int)nt_int(c->nt, id, "defined_ask", 0) : 0;
  if (asked) id = asked - 1;   /* defined?'s respond_to?: at the call it asks of */
  if (id < 0 || cls < 0 || !ct_undefined_at(c, id, cls, name, CT_CMETHOD)) return 0;
  int framed = elab_plan_enter(c, id);
  int now = elab_blocks_run_now(c, id);
  if (framed) elab_plan_leave(c);
  return now;
}
/* See call_plan.h. */
int cplan_presence_class(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!respond_to_cells_shape(nt, id)) return -1;
  int recv = nt_ref(nt, id, "receiver");
  NodeKind rk = recv >= 0 ? nt_kind(nt, recv) : NK_SelfNode;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) return class_recv_static_ci(c, recv);
  if (rk != NK_SelfNode) return -1;
  const Scope *s = comp_scope_of(c, id);
  if (s && s->class_id >= 0) return s->is_cmethod ? s->class_id : -1;
  return s && c->node_cbody && id < c->node_cap ? c->node_cbody[id] : -1;
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
/* See call_plan.h. */
int cplan_cmethod_presence(Compiler *c, int id, int cls, const char *name, int include_all, CplanCells *out) {
  ElabVersion vs[VER_MAX_ARMS];
  if (!cplan_presence_asks(c, id, cls, name)) return 0;
  int e = call_target(c, id, cls, name, CT_CMETHOD | CT_CAPTURE, NULL);
  if (e < 0 || c->scopes[e].entry != ENTRY_DISPATCH) return 0;
  int nv = elab_chain_versions(c, cls, 1, name, vs, VER_MAX_ARMS);
  out->n = 0;
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE || vs[i].v->kind == LR_UNDEF) return 0;
    if (!include_all && vs[i].v->vis != SP_VIS_PUBLIC) continue;
    ver_cell_name(vs[i].hist, out->cell[out->n]);
    out->val[out->n++] = vs[i].n;
  }
  /* (with none live, what the call runs: the entry's else arm) */
  out->els = cplan_entry_finds(c, e) >= (include_all ? VER_FINDS_HIDDEN : VER_FINDS_PUBLIC);
  return 1;
}
/* See call_plan.h. */
int cplan_entry_finds(const Compiler *c, int e) {
  for (int i = 0; e >= 0 && i < c->nver_entries; i++)
    if (c->ver_entries[i].scope == e) return c->ver_entries[i].finds;
  return -1;
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
     call runs before the class's own `def self.new` is made. Not one a
     superclass defines (the class then has no constructor of its own to
     call), nor one no def makes (define_singleton_method), which the
     record does not see. */
  if (!c->classes[ci].is_struct) {
    int dn = c->scopes[t].def_node;
    if (c->scopes[t].class_id != ci || nt_kind(c->nt, dn) != NK_DefNode || elab_point_of(c, dn).region < 0) return t;
    int framed = elab_plan_enter(c, id), at = elab_call_method(c, id, ci, nm, 1, 1);
    if (framed) elab_plan_leave(c);
    return at == ELAB_NO ? -1 : t;
  }
  int framed = elab_plan_enter(c, id), at = elab_call_method(c, id, ci, nm, 1, 1), absent = at == ELAB_NO;
  /* (a call in a method or a block: the def in force over its window, or
     none over all of it) */
  int made = at == ELAB_YES || ct_window_version(c, id, ci, nm, 1) == t;
  const LrDef *ds[2];
  if (!made && !absent) absent = elab_call_span(c, id, ci, nm, 1, 0, NULL, ds, 2) == 1 && !ds[0];
  if (framed) elab_plan_leave(c);
  if (unplaced) *unplaced = !made && !absent;
  return made ? t : -1;
}
/* See call_plan.h. */
int cplan_user_new(Compiler *c, int id, int ci) { return user_new_at(c, id, ci, NULL); }
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
/* Does call id run with main as self and look on main first: receiverless
   or on `self`, in a file's top-level statements (a block there too) or
   in a method of main's own; not in a top-level `def`, which runs on whatever
   object called it. At a load point, only once main's method of the name
   is defined there. */
static int ct_on_main(Compiler *c, int id, const char *name) {
  const LrDef *d = NULL;
  if (nt_kind(c->nt, id) != NK_CallNode) return 0;
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv >= 0 && nt_kind(c->nt, recv) != NK_SelfNode) return 0;
  const Scope *s = comp_scope_of(c, id);
  if (!self_is_main(c, id) || (!prog_is_top_scope(c, s) && !s->is_cmethod)) return 0;
  LoadPoint p = elab_point_of(c, id);
  return p.region < 0 || elab_method_at(c, -1, name, 1, p, &d) != ELAB_NO;
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
/* the table's answer for `name` on cls, as call_target asks it */
static int ct_table(Compiler *c, int cls, const char *name, unsigned flags, int *def_class) {
  if (flags & CT_TOP) return comp_method_index(c, name);
  if (flags & CT_OWN) {
    int mi = flags & CT_CMETHOD ? comp_cmethod_in_class(c, cls, name) : comp_method_in_class(c, cls, name);
    if (mi >= 0 && def_class) *def_class = cls;
    return mi;
  }
  /* a Method's capture (method, instance_method, an alias) takes a private
     instance method too, as a receiverless call does: a module function */
  return flags & CT_CMETHOD ? comp_cmethod_in_chain(c, cls, name, def_class)
       : flags & CT_CAPTURE ? comp_self_method_in_chain(c, cls, name, def_class) : comp_method_in_chain(c, cls, name, def_class);
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
   chain's, which ends in Object's (*flags gains CT_CMETHOD) */
static int ct_lookup(Compiler *c, int id, int cls, const char *name, unsigned *flags, MethodLookup *ml) {
  int si = (*flags & CT_TOP) && id >= 0 ? comp_main_method(c, name) : -1;
  int mi = si >= 0 && ct_on_main(c, id, name) ? si : ct_table(c, cls, name, *flags, ml ? &ml->def_class : NULL);
  if ((*flags & CT_TOP) && mi >= 0 && c->scopes[mi].is_cmethod) *flags |= CT_CMETHOD;
  return mi;
}
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, MethodLookup *out) {
  MethodLookup own = { .def_class = -1 }, *ml = out ? out : &own;
  int dc0 = ml->def_class, mi = -1, pin, r;
  mtab_enter(c);
  *ml = (MethodLookup){ .def_class = dc0 };
  if (ct_send_shadow(c, id, name)) {
    ml->kind = ML_NONE;
    r = -1;
  }
  else if ((mi = ct_lookup(c, id, cls, name, &flags, ml)) < 0 || id < 0 || (!(flags & (CT_REACH | CT_CAPTURE)) && !ct_calls(c, id, name))) {
    if (mi < 0) ml->kind = ML_NONE;
    else if (ct_versioned(c, mi, name) || c->scopes[mi].mixin_guarded) {   /* (the record was not asked) */
      ml->kind = ML_TABLE;
      ml->why = MLW_UNASKED;
    }
    r = mi;
  }
  else if ((ct_versioned(c, mi, name) || c->scopes[mi].mixin_guarded || c->scopes[mi].entry) && (pin = ct_pin_of(c, id)) != -1 &&
           /* (a pin answers the side its entry is on: an instance entry's
              arm, asked for a class method of a Class value its receiver
              may hold, finds that class's own method. A call on main takes
              main's singleton versions and Object's alike.) */
           (pin < 0 || (flags & CT_TOP) || !c->scopes[pin].is_cmethod == !(flags & CT_CMETHOD))) {   /* (an entry's arm: its method's own class) */
    if (pin == CT_PIN_BUILTIN || ct_top_version(c, pin, flags)) {
      ml->kind = ML_NONE;
      r = -1;
    }
    else {
      ml->def_class = c->scopes[pin].class_id;
      r = pin;
    }
  }
  else if (ct_undefined_at(c, id, cls, name, flags)) {
    /* a call the record proves runs before any version of a class method or
       a top-level function is defined raises, whatever versions follow; the
       entry's else arm answers only a call that may run before them. One at
       a point that may run any time after it (ct_after_point) takes the
       entry, whose cells tell at run time; with no entry, it raises too:
       nothing proves it runs after a def */
    int vi = ct_versioned(c, mi, name) && ct_after_point(c, id) ? ct_version(c, id, cls, name, flags, mi, ml) : -1;
    if (vi >= 0 && c->scopes[vi].entry) r = vi;
    else {
      *ml = (MethodLookup){ .kind = ML_NONE, .def_class = dc0 };
      r = -1;
    }
  }
  else {
    int asked = ct_versioned(c, mi, name) || (flags & CT_CAPTURE);
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
  if (r >= 0 && c->scopes[r].entry && ml->kind != ML_ENTRY) {   /* (an entry answered by name too) */
    ml->kind = ML_ENTRY;
    ml->why = 0;
  }
  mtab_leave(c);
  if (g_method_table_check && ml->kind == ML_TABLE) mtab_table(c, id, cls, flags, name, ml->why);
  return r;
}

int cplan_class_row(Compiler *c, int id, int recv, const char *name) {
  const NodeTable *nt = c->nt;
  int ci = class_recv_static_ci(c, recv);
  if (ci >= 0 && call_target(c, id, ci, name, CT_CMETHOD, NULL) >= 0) return 0;
  int fl = bop_flags(TY_CLASS, name);
  if ((fl & BOPF_CLASS_ONLY) && !cplan_recv_class(c, id, recv)) return 0;
  return !((fl & BOPF_PRIVATE) && recv >= 0 && nt_kind(nt, recv) != NK_SelfNode && !nt_ignore_visibility(nt, id) && !nt_str(nt, id, "send_blind"));
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
    int pm = p >= 0 ? comp_cmethod_in_chain(c, p, name, &pdc) : -1;
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
   the plan binds them by asking Object (cplan_object_reopen). Reachability
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
      if (k < 0 || elab_chain_versions(c, k, sg, nm, vs, VER_MAX_ARMS) < 2) continue;
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
void ct_version_classes(Compiler *c, int id, const char *nm, CtClassFn fn, void *arg) {
  const NodeTable *nt = c->nt;
  NodeKind kind = nt_kind(nt, id);
  if (kind == NK_SuperNode || kind == NK_ForwardingSuperNode) {
    Scope *s0 = comp_scope_of(c, id);
    const Scope *s = s0 ? comp_super_site(c, s0) : NULL;
    if (s && !comp_super_shadow(c, s)) {
      int par = comp_super_parent(c, s->class_id, s->is_cmethod);
      if (par >= 0) fn(c, id, nm, par, CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), arg);
    }
    return;
  }
  int r = nt_ref(nt, id, "receiver");
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  int cref = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode, rc = cref ? elab_class_ref_at(c, r, NULL) : -1;
  /* a constant the record cannot settle to one class (one a run-time
     condition defines, or binds to either of two) names one of the classes
     its writes may bind there, when the record can list them */
  int *rcs = NULL, nrc = rk == NK_ConstantReadNode && rc < 0 ? elab_class_candidates(c, r, &rcs) : -1;
  TyKind rt = r >= 0 && rk != NK_SelfNode && !cref ? comp_ntype(c, r) : TY_UNKNOWN;
  int oc = comp_core_class(c, CORE_OBJECT);
  if (r < 0 || rk == NK_SelfNode) {
    Scope *sc = comp_scope_of(c, id);
    int self_cls = sc && sc->class_id >= 0 ? sc->class_id : c->node_cbody[id];
    ct_class_and_descendants(c, id, nm, -1, CT_TOP, fn, arg);
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
       on either side, as the arms ask (cplan_cand_target). Asked once the
       entries' build is over, when those are noted (and the candidates'
       lists are kept, the scope index frozen); the build's own asks need
       the holders alone */
    if (c->ver_built && elab_method_late(c, nm)) {
      int npc = 0;
      const PolyCand *pcs = comp_poly_candidates_at(c, id, nm, &npc);
      for (int i = 0; i < npc; i++) fn(c, id, nm, pcs[i].cls, 0, arg);
      pcs = comp_cmethod_candidates(c, nm, &npc);
      for (int i = 0; i < npc; i++) fn(c, id, nm, pcs[i].cls, CT_CMETHOD, arg);
    }
    oc = -1;   /* (Object among them, where its chain holds two) */
    for (int i = c->ver_ix_nb ? c->ver_names[sp_strhash(nm) % c->ver_ix_nb] : -1;
         i >= 0; i = c->ver_names_next[i]) {   /* and the class of each later prepend's copy */
      const struct VerEntry *e = &c->ver_entries[i];
      if (e->guard != VER_ENTRY_CHAIN && sp_streq(e->name, nm)) fn(c, id, nm, e->cls, e->sg ? CT_CMETHOD : 0, arg);
    }
  }
  if (oc >= 0) fn(c, id, nm, oc, 0, arg);
  free(rcs);
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
int cplan_const_recv_class(Compiler *c, int id, int recv) {
  int framed = elab_plan_enter(c, id);
  int ci = elab_class_ref_at(c, recv, NULL);
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
  if (!name || (recv >= 0 && !cr && rk != NK_SelfNode) || an_scopes_named_first(c, name) < 0) return CM_NONE;
  int ci = cr ? cplan_const_recv_class(c, id, recv) : cplan_self_class(c, id);
  if (ci < -1 || ci >= c->nclasses || (cr && ci < 0)) return CM_NONE;
  /* a method the tables answer for self, which the record proves is not
     defined where the call runs: a class's class methods, main's (and a
     class body's receiverless call's) top-level functions */
  int cm = ci >= 0 && call_target(c, -1, ci, name, CT_CMETHOD, NULL) >= 0;
  int top = (ci < 0 || recv < 0) && call_target(c, -1, -1, name, CT_TOP, NULL) >= 0;
  if ((!cm && !top) || (cm && call_target(c, id, ci, name, CT_CMETHOD, NULL) >= 0) ||
      (top && call_target(c, id, -1, name, CT_TOP, NULL) >= 0))
    return CM_NONE;
  if (cm_builtin_answers(c, ci, name)) return CM_NONE;
  if (anon) *anon = cm_self_name(c, ci, self_text, cap) ? -1 : ci;
  return nt_vcall(nt, id) ? CM_NAMEERROR : CM_NOMETHOD;
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
  int ci = cr ? cplan_const_recv_class(c, id, recv) : cplan_self_class(c, id);
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
   from then on (elab_singleton_made): CP_ABSENT_UNTOLD where the record
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
  int made = elab_singleton_made(c, cls, elab_point_of(c, id));
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
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, cls, name, 1, 1) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none;
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
    if ((m->entry != ENTRY_DISPATCH && m->entry != ENTRY_TABLE) || cplan_entry_finds(c, mi) != VER_FINDS_NONE || is_instance_method_name(nt_str(c->nt, id, "name"))) return 0;
  }
  else if (cls < 0 || !(ct_undefined_at(c, id, cls, name, CT_CMETHOD) || cm_capture_never_made(c, id, cls, name)) ||
           cm_builtin_answers(c, cls, name)) return 0;
  return cplan_capture_names(c, id, cls, sg, name, m != NULL);
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
      (class_is_comparable(c, cid) && core_method_vis("Comparable", name, 0) > 0) ||
      (class_mixes_in(c, cid, elab_module_named(c, "Enumerable"), 0) && core_method_vis("Enumerable", name, 0) > 0))
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
/* See call_plan.h. */
int cplan_capture_misses(Compiler *c, int id, int k, const char *sym) {
  static const char *const hooks[] = { METHOD_MISSING_NAME, RESPOND_TO_MISSING_NAME };
  if (k < 0 || !sym || capture_builtin_answers(c, k, sym) || ver_builtin_chain(c, k, 0, sym, 0, 0) ||
      ct_accessor_class(c, k, sym) >= 0)
    return 0;
  for (int i = 0; i < 2; i++)
    if (comp_method_in_chain(c, k, hooks[i], NULL) < 0) return 0;
  /* (a method of the name, but for the stand-in a later include or
     prepend puts in front of none, cplan_capture_arm) */
  int mi = comp_method_in_chain(c, k, sym, NULL), nd = 0;
  if (mi >= 0 && !c->scopes[mi].mixin_guarded && c->scopes[mi].entry == ENTRY_NONE) return 0;
  /* a class under k that has one: the receiver may be of it */
  const int *ds = comp_descendants(c, k, &nd);
  for (int i = 0; i < nd; i++) {
    int d = ds[i], dm = comp_method_in_chain(c, d, sym, NULL);
    if (!c->classes[d].unreferenced && ((dm >= 0 && dm != mi) || ct_accessor_class(c, d, sym) >= 0)) return 0;
  }
  /* both hooks a def surely live where the capture runs */
  int framed = elab_plan_enter(c, id), live = 1, none;
  for (int i = 0; i < 2 && live; i++) live = ct_def_sure(c, ct_met_version(c, id, k, hooks[i], 0, &none));
  if (framed) elab_plan_leave(c);
  return live;
}
/* See call_plan.h. */
int cplan_builtin_method_vis(Compiler *c, int node, int ci, const char *cls, const char *m, int inherit) {
  const ClassInfo *ki = ci >= 0 ? &c->classes[ci] : NULL;
  int bv = ki ? refl_builtin_vis(c, ci, m, inherit) : core_method_vis(cls, m, inherit);
  /* main's defs are Object's own methods, ahead of its and Kernel's rows */
  const char *on = ki ? (ki->is_builtin_const && ki->enclosing_class < 0 ? ki->name : NULL) : cls;
  int mv = bv >= 0 && on && is_object_class_name(on) ? elab_main_method_vis_at(c, node, m) : -1;
  return mv > 0 ? mv : bv;
}
/* See call_plan.h. */
CplanMissing cplan_dispatch_miss(Compiler *c, int id, int cid) {
  const NodeTable *nt = c->nt;
  const char *name = id >= 0 && nt_kind(nt, id) == NK_CallNode ? nt_str(nt, id, "name") : NULL;
  if (!name || cid < 0 || cid >= c->nclasses || call_target(c, id, cid, name, 0, NULL) >= 0 ||
      builtin_object_method_known(name) || is_kernel_module_function(name) ||
      (is_comparable_module_method(name) && class_is_comparable(c, cid)) ||
      (name_is_enumerable_module_method(name) && class_mixes_in(c, cid, elab_module_named(c, "Enumerable"), 0)) ||
      comp_method_in_chain(c, cid, METHOD_MISSING_NAME, NULL) >= 0 || an_user_defines_method(c, METHOD_MISSING_NAME))
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
  int ci = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? cplan_const_recv_class(c, id, recv) : -1;
  return ci >= 0 && call_target(c, -1, ci, name, CT_CMETHOD, NULL) >= 0 &&
         call_target(c, id, ci, name, CT_CMETHOD, NULL) < 0 && is_kernel_module_function(name);
}
int cplan_kernel_row(Compiler *c, int id, const char *name) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  call_args(nt, id, &argc);
  const BuiltinOp *op = name && nt_ref(nt, id, "receiver") < 0 ? bop_find(BOP_KERNEL, name, argc, nt_ref(nt, id, "block") >= 0) : NULL;
  if (!op) return 0;
  /* an internal row answers the calls the compiler made (builtin_only: it
     never reaches a method of the program's), whatever the program defines;
     the program's own call of its name never takes it */
  if (op->flags & BOPF_INTERNAL) return nt_int(nt, id, "builtin_only", 0);
  int self = cplan_self_class(c, id);
  return call_target(c, id, -1, name, CT_TOP, NULL) < 0 && (self < 0 || call_target(c, id, self, name, 0, NULL) < 0);
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

const CallPlan *cplan_user_in(Compiler *c, int id, int self_ci, int flags) {
  static CallPlan ctx;
  if (self_ci < 0) return cplan_user(c, id);
  cplan_set(&ctx, -1, -1, UC_NONE, CP_NONE);
  if (id < 0 || id >= c->node_cap || self_ci >= c->nclasses || nt_kind(c->nt, id) != NK_CallNode)
    return &ctx;
  if ((flags & CPX_TARGET) && nt_int(c->nt, id, "builtin_only", 0)) return &ctx;
  const char *name = flags & CPX_TARGET ? view_call_name(c, id) : nt_str(c->nt, id, "name");
  int mi = name ? call_target(c, id, self_ci, name, 0, NULL) : -1;
  if (mi >= 0) {
    int form = flags & CPX_TARGET ? CP_DIRECT : cplan_dispatch_form(c, id, self_ci, name, 1);
    cplan_set(&ctx, mi, self_ci, UC_INST, form);
    ctx.chain = 1;
    return cplan_dep(&ctx);
  }
  if (flags & CPX_TARGET) return &ctx;
  /* a miss is no method, but a dispatch on the class still has a form: a
     method only descendants define takes the switch */
  if (!(flags & CPX_IE) && name) {
    cplan_set(&ctx, -1, self_ci, UC_INST, cplan_dispatch_form(c, id, self_ci, name, 0));
    ctx.chain = 1;
    return &ctx;
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
  return cplan_dep(&ctx);
}

const CallPlan *cplan_user(Compiler *c, int id) {
  static CallPlan fresh;
  if (id < 0 || id >= c->node_cap) { cplan_set(&fresh, -1, -1, UC_NONE, CP_NONE); return &fresh; }
  int plain = cplan_plain_ctx();
  const char *nm = nt_str(c->nt, id, "name");
  if (plain && id < g_cp_cap && g_cp_have[id]) {
    const char *mn = g_cp_name[id];
    if (mn ? nm && sp_streq(mn, nm) : !nm) return cplan_dep(&g_cp_memo[id]);
    plain = 0;
  }
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  cplan_resolve(c, id, &fresh);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  if (!plain) return cplan_dep(&fresh);
  if (id >= g_cp_cap) {
    int ncap = g_cp_cap ? g_cp_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_cp_memo = realloc(g_cp_memo, (size_t)ncap * sizeof *g_cp_memo);
    g_cp_have = realloc(g_cp_have, (size_t)ncap);
    g_cp_name = realloc(g_cp_name, (size_t)ncap * sizeof *g_cp_name);
    memset(g_cp_have + g_cp_cap, 0, (size_t)(ncap - g_cp_cap));
    memset(g_cp_name + g_cp_cap, 0, (size_t)(ncap - g_cp_cap) * sizeof *g_cp_name);
    g_cp_cap = ncap;
  }
  g_cp_memo[id] = fresh;
  free(g_cp_name[id]);
  g_cp_name[id] = nm ? strdup(nm) : NULL;
  g_cp_have[id] = 1;
  return cplan_dep(&g_cp_memo[id]);
}

const CallPlan *cplan_user_fresh(Compiler *c, int id) {
  static CallPlan fresh;
  cplan_set(&fresh, -1, -1, UC_NONE, CP_NONE);
  if (id < 0 || id >= c->node_cap) return &fresh;
  cplan_resolve(c, id, &fresh);
  return cplan_dep(&fresh);
}

/* The class a constructor call builds: a constant receiver's or a local's
   that holds one class (class_recv_static_ci), or the class its method
   runs for, as inference types it (bare or `self.new` in a class method,
   `self.class.new` in an instance method). *own is set for the last kind,
   which a subclass receiving the method builds its own of. */
static int cplan_new_class(Compiler *c, int id, int *own) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, id, "receiver");
  NodeKind k = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  *own = 0;
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode || k == NK_LocalVariableReadNode) return class_recv_static_ci(c, r);
  Scope *s = comp_scope_of(c, id);
  int ci = -1;
  if (s && s->is_cmethod && (r < 0 || k == NK_SelfNode)) ci = s->class_id;
  else if (s && !s->is_cmethod && k == NK_CallNode && nt_ref(nt, r, "receiver") >= 0 &&
           nt_kind(nt, nt_ref(nt, r, "receiver")) == NK_SelfNode && nt_str(nt, r, "name") &&
           sp_streq(nt_str(nt, r, "name"), "class")) ci = s->class_id;
  if (ci < 0 || ci >= c->nclasses || comp_class_is_module(c, &c->classes[ci])) return -1;
  *own = 1;
  return ci;
}

/* The initialize methods a builtin `new` runs that builds class ci, or with
   own an instance of ci or of any subclass (into out, at most CPT_MAX;
   CPT_UNKNOWN past it). */
static int ct_class_initializers(Compiler *c, int ci, int own, int *out) {
  int n = 0;
  for (int k = ci; ci >= 0 && k < c->nclasses; k++) {
    if (k != ci && (!own || !is_descendant(c, k, ci))) continue;
    int mi = comp_method_in_chain(c, k, "initialize", NULL), dup = 0;
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
  int own, ci = cplan_new_class(c, id, &own), n = ci >= 0 ? ct_class_initializers(c, ci, own, out) : 0;
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
  if (!name || nt_int(c->nt, id, "builtin_only", 0) ||
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
      int mi = is_to_s_name(name) ? -1 : comp_method_in_chain(c, cid, "message", NULL);
      if (mi < 0) mi = comp_method_in_chain(c, cid, "to_s", NULL);
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
      int mi = is_to_s_name(name) ? -1 : comp_method_in_chain(c, k, "message", NULL);
      if (mi < 0) mi = comp_method_in_chain(c, k, "to_s", NULL);
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
  if (!name || nt_ref(nt, id, "receiver") >= 0 || nt_int(nt, id, "builtin_only", 0)) return -2;
  Scope *sc = comp_scope_of(c, id);
  int cid = sc ? sc->class_id : -1;
  if (!sc || sc->is_cmethod || cid < 0 || cid >= c->nclasses || call_target(c, id, cid, name, 0, NULL) >= 0)
    return -2;
  /* (an attribute of the chain is the call's own member: emit_implicit_self_member) */
  size_t len = strlen(name);
  if (comp_reader_in_chain(c, cid, name, NULL)) return -2;
  if (len > 1 && name[len - 1] == '=') {
    char base[256];
    if (len - 1 >= sizeof base) return -2;
    memcpy(base, name, len - 1);
    base[len - 1] = '\0';
    if (comp_writer_in_chain(c, cid, base, NULL)) return -2;
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
  const CallPlan *p = cplan_user_fresh(c, id);
  if (p->mi < 0 || p->dispatch == CP_REFUSE) {
    int tmpl = p->dispatch == CP_REFUSE ? -2 : ct_template_self(c, id, out);
    if (tmpl != -2) return tmpl;
    int recv = nt_ref(c->nt, id, "receiver");
    if (recv >= 0 && comp_ntype(c, recv) == TY_UNKNOWN) return CPT_UNKNOWN;
    return 0;
  }
  CallPlan plan = *p;
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
  const CallPlan *p = cplan_user_fresh(c, id);
  if (p->via != UC_POLY || p->mi < 0) return TY_UNKNOWN;
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
  const CallPlan *p = cplan_user(c, id);
  return p->mi >= 0 && p->via == UC_CMETH && p->dispatch == CP_DIRECT && p->owner_ci == c->node_cbody[id];
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
  const CallPlan *p = cplan_user_fresh(c, id);
  int recv = nt_ref(c->nt, id, "receiver");
  if ((p->mi < 0 || p->dispatch == CP_REFUSE) && recv >= 0 && comp_ntype(c, recv) == TY_UNKNOWN)
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
  return !mod && s->is_cmethod ? ct_class_initializers(c, s->class_id, 1, out) : -2;
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
    int k = cls[i], init[CPT_MAX];
    if (comp_class_is_module(c, &c->classes[k]) || comp_cmethod_in_chain(c, k, "new", NULL) >= 0) continue;
    int ni = ct_class_initializers(c, k, 0, init);
    if (ni < 0) return CPT_UNKNOWN;
    for (int j = 0; j < ni; j++) {
      int dup = 0;
      for (int q = 0; q < n && !dup; q++) dup = out[q] == init[j];
      if (dup) continue;
      if (n == CPT_MAX) return CPT_UNKNOWN;
      out[n++] = init[j];
    }
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
  return cplan_user_fresh(c, id)->dispatch != CP_NONE ? ct_mixed_new_entries(c, id, out) : ct_new_initializers(c, id, out);
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
  const CallPlan *p = cplan_user_fresh(c, id);
  int fb = p->send_fallback, n = 0;
  if (fb >= 0) {
    if (cap < 1) return CPT_UNKNOWN;
    out[n++] = fb;
  }
  if (p->via != UC_POLY || p->mi < 0 || p->dispatch == CP_REFUSE) {
    int m = cplan_entries(c, id, out + n, cap - n);
    if (m == CPT_UNKNOWN) return m;
    for (int i = n; i < n + m; i++)
      if (out[i] == fb) { out[i] = out[n + m - 1]; m--; break; }
    return n + m;
  }
  const PolyPlan *pp = cplan_poly_fresh(c, id);
  for (int i = 0; i < pp->n; i++) {
    int mi = pp->arm[i].mi, seen = 0;
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
  case LX_NAME: return diag_user_defines(c, name);
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
    const CallPlan *p = cplan_user_fresh(c, id);
    return p->dispatch != CP_NONE && !p->by_name &&
           !(p->via == UC_POLY && p->mi >= 0 && c->scopes[p->mi].is_cmethod);
  }
  }
  return 0;
}

/* Object's public and private instance methods, Kernel's private ones
   aside (kernel_private_method): with those, every method an object answers
   before a define_method replaces it */
static int object_method_name(const char *nm) {
  static const char *const names[] = {
    "!", "!=", "!~", "<=>", "==", "===", "__callee__", "__dir__", "__id__", "__method__",
    "__send__", "`", "class", "clone", "define_singleton_method", "display", "dup", "enum_for",
    "eql?", "equal?", "extend", "freeze", "frozen?", "hash", "initialize", "initialize_clone",
    "initialize_copy", "initialize_dup", "inspect", "instance_eval", "instance_exec",
    "instance_of?", "instance_variable_defined?", "instance_variable_get",
    "instance_variable_set", "instance_variables", "is_a?", "itself", "kind_of?", "method",
    "method_missing", "methods", "nil?", "object_id", "private_methods", "protected_methods",
    "public_method", "public_methods", "public_send", "remove_instance_variable", "respond_to?",
    "respond_to_missing?", "send", "singleton_class", "singleton_method",
    "singleton_method_added", "singleton_method_removed", "singleton_method_undefined",
    "singleton_methods", "tap", "then", "to_enum", "to_s", "yield_self", NULL };
  for (int i = 0; nm && names[i]; i++) if (sp_streq(nm, names[i])) return 1;
  return kernel_private_method(nm);
}

/* A define_method in a method's body defines its method when that method
   runs, but analysis registers it from the start (walk_scope marks the call
   dm_host, and dm_scope for the method it registers). A method nothing names
   is not registered at all (method_never_named). Otherwise the early
   registration is right only for a method that is new: one the class does
   not answer before then. Anything else is refused: its own def or one up
   its chain, a module's, a subclass's, an alias, attr or Struct member
   (spelled as a Symbol), or a method every object has. A call in a branch
   that never runs registers nothing (walk_scope). */
int cplan_define_method_early(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int host = (int)nt_int(nt, id, "dm_host", 0) - 1, dm = (int)nt_int(nt, id, "dm_scope", 0) - 1;
  if (host < 0 || host >= c->nscopes) return 0;
  int an = nt_ref(nt, id, "arguments"), ac = 0;
  const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
  int own = ac > 0 ? av[0] : -1;
  if (own < 0 || (nt_kind(nt, own) != NK_SymbolNode && nt_kind(nt, own) != NK_StringNode)) return 0;
  /* a method object (`define_method(:m, instance_method(:x))`) registers nothing */
  if (dm < 0 || dm >= c->nscopes) return 1;
  const Scope *ds = &c->scopes[dm];
  if (!ds->name || ds->class_id < 0 || object_method_name(ds->name)) return 1;
  int dc = -1, mi = comp_method_in_chain(c, ds->class_id, ds->name, &dc);
  if (mi >= 0 && mi != dm) return 1;
  for (int t = 0; t < c->nscopes; t++) {
    const Scope *ts = &c->scopes[t];
    if (t == dm || ts->is_cmethod || ts->class_id < 0 || !ts->name || !sp_streq(ts->name, ds->name)) continue;
    for (int k = ds->class_id, h = 0; k >= 0 && k < c->nclasses && h < 64; k = c->classes[k].parent, h++) {
      if (ts->class_id == k) return 1;
      for (int m = 0; m < c->classes[k].nincluded_mods; m++)
        if (c->classes[k].included_mods[m] == ts->class_id) return 1;
    }
    /* a subclass's: a class method run on the subclass defines it there */
    for (int k = ts->class_id, h = 0; k >= 0 && k < c->nclasses && h < 64; k = c->classes[k].parent, h++)
      if (k == ds->class_id) return 1;
  }
  for (int k = comp_kind_first(c, NK_SymbolNode); k >= 0; k = comp_kind_next(c, k)) {
    const char *v = nt_kind(nt, k) == NK_SymbolNode && k != own ? nt_str(nt, k, "value") : NULL;
    if (v && sp_streq(v, ds->name)) return 1;
  }
  for (int k = comp_kind_first(c, NK_StringNode); k >= 0; k = comp_kind_next(c, k)) {
    const char *v = nt_kind(nt, k) == NK_StringNode && k != own ? nt_str(nt, k, "content") : NULL;
    if (v && sp_streq(v, ds->name)) return 1;
  }
  /* the block is registered whichever define_method the call reaches, and a
     program's own may answer it without defining anything: then a call of
     the name would find a method CRuby never defines */
  if (an_user_define_method(c))
    for (int k = comp_kind_first(c, NK_CallNode); k >= 0; k = comp_kind_next(c, k)) {
      const char *v = nt_kind(nt, k) == NK_CallNode ? nt_str(nt, k, "name") : NULL;
      if (v && sp_streq(v, ds->name)) return 1;
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
  return ty_is_object(rt) && comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) != SP_MEMBER_NONE;
}

/* Does presence query id (is_method_presence_query, kind 1) ask about a
   named entry whose name answers before a version ran, of a history
   holding a version marked cond_lone (a lone def under an `if` the
   lowering leaves to the record, a define_method a Struct or Data block
   may not run): a name not literal or of such an entry (cond_def_refl)? */
static int ver_refl_named(Compiler *c, int id) {
  int args = nt_ref(c->nt, id, "arguments"), an = 0;
  if (!c->nver_refl) return 0;
  if (args >= 0) nt_arr(c->nt, args, "arguments", &an);
  const char *q = an ? method_sym_arg(c, id) : NULL;
  for (int i = 0; an && q && i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == VER_ENTRY_HISTORY && e->scope >= 0 && nt_int(c->nt, c->scopes[e->scope].def_node, "cond_def_refl", 0) && sp_streq(e->name, q))
      return 1;
  }
  return an && !q;
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
  if (!c->nver_refl && !c->ncond_disp) return 0;
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  if (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode) {
    int k = cplan_const_recv_class(c, id, r);
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
    return "a def under a condition only the run time decides, of a name another method answers "
           "(a builtin's, another def's or an accessor's), is not supported: which one answers when the branch did "
           "not run is not known at compile time; see docs/limitations.md";
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
                    ? nt_str(nt, recv, "name") : NULL;
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

  if (!why && rcn && comp_class_index(c, rcn) < 0) {
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
  if (!why && rcn && mixin >= 0 && (comp_class_index(c, rcn) >= 0 || sp_streq(rcn, "Object") ||
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
        comp_method_in_chain(c, self->class_id, "eval", NULL) >= 0) return NULL;
    if (comp_method_index(c, "eval") >= 0) return NULL;
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
  int ci = io_reopen_class(c, name);
  if (ci < 0 || call_target(c, id, ci, name, 0, NULL) < 0) return 0;
  int ks[16], n = io_reopen_defs(c, name, 0, ks, 16);
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
  return nt_kind(c->nt, id) == NK_CallNode && nt_ref(c->nt, id, "receiver") < 0 && name && s->class_id < 0 &&
         !s->is_cmethod && !s->entry && s->def_node >= 0 && !prog_is_top_scope(c, s) && comp_main_method(c, name) >= 0;
}

static CallPlan *g_rf_memo = NULL;
static unsigned char *g_rf_have = NULL;
static char **g_rf_name = NULL;   /* the name each entry was resolved under (cplan_user) */
static int g_rf_cap = 0;

static void cplan_refuse_set(CallPlan *p, int from, int rkind, const char *msg, char *buf, size_t cap) {
  p->dispatch = CP_REFUSE; p->rfrom = (unsigned char)from; p->rkind = (unsigned char)rkind;
  snprintf(buf, cap, "%s", msg);
  p->msg = buf;
}

/* in the order codegen meets them: the prepasses, then the emitters */
static void cplan_refuse_resolve(Compiler *c, int id, CallPlan *p, char *buf, size_t cap) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    const CallPlan *sp = cplan_user(c, id);
    if (sp->dispatch == CP_REFUSE) {
      Scope *s = comp_scope_of(c, id);
      char msg[2400];
      snprintf(msg, sizeof msg, "%s.%s: %s", c->classes[s->class_id].name,
               comp_scope_user_name(s), sp->msg);
      cplan_refuse_set(p, sp->rfrom, sp->rkind, msg, buf, cap);
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
  if (!what && cplan_record_new_gap(c, id)) {
    cplan_refuse_set(p, CRF_RECORD_NEW, CR_FEATURE,
                     "a Struct or Data class's `new` or `[]` the program defines, called where the record "
                     "cannot tell whether that def has run: CRuby runs it once it has, the builtin before", buf, cap);
    return;
  }
  /* the plan's capture of a module's class method it does not have, where
     the record cannot tell the class its NameError names */
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

const CallPlan *cplan_refuse(Compiler *c, int id) {
  static CallPlan fresh;
  static char fresh_msg[2400];
  if (id < 0 || id >= c->node_cap) { cplan_set(&fresh, -1, -1, UC_NONE, CP_NONE); return &fresh; }
  int plain = cplan_plain_ctx();
  const char *nm = nt_str(c->nt, id, "name");
  if (plain && id < g_rf_cap && g_rf_have[id]) {
    const char *mn = g_rf_name[id];
    if (mn ? nm && sp_streq(mn, nm) : !nm) return &g_rf_memo[id];
    plain = 0;
  }
  cplan_refuse_resolve(c, id, &fresh, fresh_msg, sizeof fresh_msg);
  if (!plain) return &fresh;
  if (id >= g_rf_cap) {
    int ncap = g_rf_cap ? g_rf_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_rf_memo = realloc(g_rf_memo, (size_t)ncap * sizeof *g_rf_memo);
    g_rf_have = realloc(g_rf_have, (size_t)ncap);
    g_rf_name = realloc(g_rf_name, (size_t)ncap * sizeof *g_rf_name);
    memset(g_rf_have + g_rf_cap, 0, (size_t)(ncap - g_rf_cap));
    memset(g_rf_name + g_rf_cap, 0, (size_t)(ncap - g_rf_cap) * sizeof *g_rf_name);
    g_rf_cap = ncap;
  }
  g_rf_memo[id] = fresh;
  if (fresh.msg) g_rf_memo[id].msg = strdup(fresh.msg);
  free(g_rf_name[id]);
  g_rf_name[id] = nm ? strdup(nm) : NULL;
  g_rf_have[id] = 1;
  return &g_rf_memo[id];
}

/* ---- CP_POLY ---- */

static void cpoly_family(PolyPlan *p, int *cap, int fam);
static void cpoly_trial(PolyPlan *p, int *cap, int t);
static int cpoly_has_family(const PolyPlan *p, int fam);
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
         comp_resolve_member(c, cid, name, 0, NULL, NULL) == SP_MEMBER_NONE;
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
        TyKind mret = comp_native_ret_ty(c, nmi, k);
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
    { char zexp[600]; int rdc0 = -1;
      if (poly_arm_refuses_none(c, mi, zexp, sizeof zexp) && !comp_reader_in_chain(c, k, name, &rdc0)) {
        cpoly_add(p, &cap, PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID);
        continue;
      } }
    /* comp_resolve_member ranks a def and a reader by which class defines
       them nearer; the reader below takes the arm when it wins (#7248). */
    if (mi >= 0 && c->scopes[mi].nrequired == 0 &&
        comp_resolve_member(c, k, name, 0, NULL, NULL) != SP_MEMBER_ATTR &&
        (scope_has_callable_symbol(c, mi) || scope_needs_proc_form(c, mi)) &&
        !(c->classes[defcls].name && sp_streq(c->classes[defcls].name, "Class"))) {
      /* A proc form is a separately inferred clone, so its own return type
         is the one to read -- not the original's, which an inlined-only
         method never needed (#3399). */
      int pfi = scope_proc_form_of(c, mi);
      Scope *ms = &c->scopes[pfi >= 0 ? pfi : mi];
      TyKind cret = ms->ret;
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
    int rdcls = -1;
    if (comp_reader_in_chain(c, k, name, &rdcls)) {
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
  if (sp_streq(name, "syswrite") && argc == 1 && plain) cpoly_family(p, cap, PB_IO_SYSWRITE);
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
  int vis = blind ? SP_VIS_PUBLIC : comp_method_vis_declared(c, obj_cls, name, NULL);
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
  if (ps.ppack && sp_streq(name, "unpack1")) cpoly_family(p, cap, PB_UNPACK1);
  if (ps.include) cpoly_family(p, cap, PB_INCLUDE);
  if (ps.strdel && (ret == TY_POLY || ret == TY_STRING)) cpoly_family(p, cap, PB_STR_DELETE);
  if (ps.strpart && (ret == TY_POLY || ret == TY_STR_ARRAY)) cpoly_family(p, cap, PB_STR_PARTITION);
  if (ps.strsetop_n &&
      (sp_streq(name, "count") ? ret == TY_POLY || ret == TY_INT : ret == TY_POLY || ret == TY_STRING))
    cpoly_family(p, cap, PB_STR_SETOP);
  if (ps.pstore && ret == TY_POLY) cpoly_family(p, cap, PB_STORE);
  if (ps.strencode && pos_argc <= 2 && (ret == TY_POLY || ret == TY_STRING)) cpoly_family(p, cap, PB_STR_ENCODE);
  if (ps.strsplit && (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY))
    cpoly_family(p, cap, PB_STR_SPLIT_N);
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
  int callable = is_call_alias(name);
  /* (of any argument count: one past the channel's slots rides the call's
     wide store) */
  if (callable && ((!ps.has_splat_arg && (ps.kw_pos || kwh >= 0)) || splat_last))
    cpoly_family(p, cap, PB_CALLABLE);
  if (ps.kw_pos && ps.straset) cpoly_family(p, cap, PB_STR_ASET);
  if (ps.kw_pos && !ps.has_splat_arg && cpoly_str_trial(c, id, name, argc, argv, atmp_ty, ret, p))
    cpoly_trial(p, cap, PT_STR);
  cpoly_cases_n(c, id, name, argc, argv, ret, atmp_ty, &ps, splat_a, p, cap);
  if (cpoly_has_family(p, PB_ND_GENERIC) && !cpoly_has_family(p, PB_ND_REPLACE) &&
      !cpoly_has_family(p, PB_ND_UPTO) &&
      !cpoly_has_family(p, PB_ND_ROUND) && !cpoly_has_family(p, PB_ND_NUM) &&
      !cpoly_obj_default_n(c, id, name, kwh, pos_argc, splat_a, p, cap))
    cpoly_trial(p, cap, PT_GENERIC_TAIL);
  /* the families that write the switch's `default:` themselves */
  static const int dflt[] = { PB_ND_GENERIC, PB_ND_FIRSTN, PB_ND_KEYS, PB_ND_MERGE, PB_ND_AREF2, PB_ND_AREF,
                              PB_PUSH, PB_INCLUDE_CASES, PB_PRED_N };
  int has_default = ps.cover || (ps.strftime && ps.ncand == 0);
  for (size_t i = 0; i < sizeof dflt / sizeof dflt[0]; i++) has_default |= cpoly_has_family(p, dflt[i]);
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
  for (int ki = 0; ki < nks; ki++) {
    int k = ks[ki];
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
    TyKind mret = ks->ret;
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
    cpoly_add(p, cap, PA_PROC_FORM, PA_KEY_DEFAULT, obj_mi, ps->ret,
              method_is_void(ps) ? PC_VOID : cpoly_conv(ret, ps->ret));
  }
  else if (obj_mi >= 0 && ml.def_class == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
           scope_has_callable_symbol(c, obj_mi)) {
    Scope *ms = &c->scopes[obj_mi];
    cpoly_add(p, cap, PA_USER, PA_KEY_DEFAULT, obj_mi, ms->ret,
              method_is_void(ms) ? PC_VOID : cpoly_conv(ret, ms->ret));
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

static int cpoly_has_family(const PolyPlan *p, int fam) {
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
    if (cpoly_has_family(p, str_tag[i])) return 0;
  /* an Integer's bit reference reads a String's character too */
  if (cpoly_has_family(p, PB_INT_BITREF) && (ret == TY_POLY || ret == TY_STRING)) return 0;
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
  if (sci >= 0 && call_target(c, id, sci, name, 0, NULL) >= 0) return;   /* the reopen answers */
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
    else
      cpoly_add(p, cap, PA_USER, PA_KEY_CLASS_VALUE + ccls[i], cmi[i], ks->ret,
                !has_tr || method_is_void(ks) ? PC_VOID : cpoly_conv(ret, ks->ret));
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
  if (is_call_alias(name)) cpoly_family(p, cap, PB_CALLABLE);
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
  else if (sp_streq(name, "__to_enum_each") && (ret == TY_POLY || ret == TY_ENUMERATOR)) fam = PB_D_ENUM_EACH;
  else if (is_text_conversion(name)) fam = PB_D_TO_S;
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
}

/* How a zero-argument dispatch holds and keys its receiver, and the type
   its arms answer into. An OpenStruct member read makes an untyped call
   poly; emission reads these flags and the adjusted result type. */
static void cpoly_flags0(Compiler *c, int id, const char *name, PolyPlan *p, PolySpecials0 *out) {
  PolySpecials0 ps;
  poly_specials0(c, id, name, &ps);
  *out = ps;
  if (ps.ostruct && (p->ret == TY_UNKNOWN || p->ret == TY_VOID || p->ret == TY_NIL)) p->ret = TY_POLY;
  int root = ps.ncall_arm > 0 || ps.lengthlike || ps.empty || ps.pred || ps.class_named ||
             ps.class_reflect || ps.ostruct || ps.io_rewind || ps.poly_to_a || ps.poly_to_h;
  /* A String mutated in place and then stored is boxed as its
     shared-mutable handle, which no String arm below reads: the chain
     built for a name a user class shares answered it with the switch's
     NoMethodError (#5048). A reading method sees the handle's live
     string, so it is read through the deref; a mutator, and a question
     about the object itself rather than its value, keep the handle. */
  /* Only a name String answers (the face table's String rows, and the
     length-like and empty? arms above them) builds a String arm, so only
     those dispatches pay the deref: the hottest one, an attr reader
     shared by an AST's node classes, never sees a String. */
  unsigned sface = ty_poly_face_owners(name, 0, nt_ref(c->nt, id, "block") >= 0, 1, 1);
  int deref = (ps.lengthlike || ps.empty || ((sface & PF_STRING) && !(sface & PF_MUT))) &&
              !sp_str_mutator(name, SP_MUT_LOCAL);
  p->flags = (root ? PPF_ROOT : 0) | (deref ? PPF_DEREF : 0) |
             (poly_key_cls0(c, id, name, 0, -1, 0, -1) ? PPF_KEY_CLS0 : 0) |
             (poly_key_prim(c, id, name, 0, -1, 0, -1) ? PPF_KEY_PRIM : 0) |
             (cpoly_key_clsv(c, id, name) ? PPF_KEY_CLSV : 0) |
             (poly_exc_cand(c, name) ? PPF_KEY_EXC : 0);
}

static void cpoly_resolve(Compiler *c, int id, PolyPlan *p, int full) {
  free(p->arm);
  p->arm = NULL; p->n = 0; p->flags = 0; p->full = full;
  p->ntype = p->ret = comp_ntype(c, id);
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
    /* (one-String mode: a String answer the arms hand on as its bytes, as
       the emitter's slot takes it: a handle ivar's reader reads it out) */
    cpoly_user_arms0(c, id, name, p->ret == TY_STRBUF ? TY_STRING : p->ret, p);
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
    for (int f = PB_D_ENUM_EACH; f <= PB_D_HASH; f++) done |= cpoly_has_family(p, f);
    if (cpoly_has_family(p, PB_D_ARRAY_TRANSFORM)) cpoly_trial(p, &cap, PT_ARRAY_FALLBACK);
    if (!done) cpoly_trial(p, &cap, PT_DEFAULT0);
  }
  else cpoly_user_arms_n(c, id, name, argc, nt_arr(c->nt, args, "arguments", &argc), p->ret, p, full);
}

static PolyPlan *g_pp_memo;
static char **g_pp_name;
static int g_pp_cap;

static const PolyPlan *cplan_poly_level(Compiler *c, int id, int full);

const PolyPlan *cplan_poly(Compiler *c, int id) { return cplan_poly_level(c, id, 1); }
const PolyPlan *cplan_poly_arms(Compiler *c, int id) { return cplan_poly_level(c, id, 0); }

static const PolyPlan *cplan_poly_level(Compiler *c, int id, int full) {
  static PolyPlan fresh;
  if (id < 0 || id >= c->node_cap) { free(fresh.arm); fresh.arm = NULL; fresh.n = 0; return &fresh; }
  const char *nm = nt_str(c->nt, id, "name");
  int plain = cplan_plain_ctx();
  if (plain && id < g_pp_cap && g_pp_name[id] && nm && sp_streq(g_pp_name[id], nm) &&
      g_pp_memo[id].ntype == comp_ntype(c, id) && g_pp_memo[id].edits == comp_ty_edits &&
      (g_pp_memo[id].full || !full))
    return &g_pp_memo[id];
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  /* Resolve into a plan of this call's own, then store it. Resolving can
     ask for another call's arms (an argument's representation reads its
     reader arms: repr_boxed_reader_handle), which comes back here and
     refills `fresh` or moves the memo under the arms this call is still
     adding. */
  PolyPlan r = {0};
  cpoly_resolve(c, id, &r, full);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  PolyPlan *p = &fresh;
  if (plain) {
    if (id >= g_pp_cap) {
      int ncap = g_pp_cap ? g_pp_cap : 1024;
      while (ncap <= id) ncap *= 2;
      g_pp_memo = realloc(g_pp_memo, (size_t)ncap * sizeof *g_pp_memo);
      g_pp_name = realloc(g_pp_name, (size_t)ncap * sizeof *g_pp_name);
      memset(g_pp_memo + g_pp_cap, 0, (size_t)(ncap - g_pp_cap) * sizeof *g_pp_memo);
      memset(g_pp_name + g_pp_cap, 0, (size_t)(ncap - g_pp_cap) * sizeof *g_pp_name);
      g_pp_cap = ncap;
    }
    p = &g_pp_memo[id];
    free(g_pp_name[id]);
    g_pp_name[id] = nm ? strdup(nm) : NULL;
  }
  free(p->arm);
  *p = r;
  return p;
}

const PolyPlan *cplan_poly_fresh(Compiler *c, int id) {
  static PolyPlan fresh;
  if (id < 0 || id >= c->node_cap) { free(fresh.arm); fresh.arm = NULL; fresh.n = 0; return &fresh; }
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  cpoly_resolve(c, id, &fresh, 1);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  return &fresh;
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
const PolyPlan *cplan_each_split(Compiler *c, int id) {
  static PolyPlan sp;
  free(sp.arm);
  sp.arm = NULL; sp.n = 0; sp.flags = 0; sp.native_block = 0;
  sp.ntype = sp.ret = comp_ntype(c, id);
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), blk = nt_ref(nt, id, "block");
  if (!name || !is_each_name(name) || recv < 0 || blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return &sp;
  if (nt_ref(nt, id, "arguments") >= 0) return &sp;
  if (repr_of(c, recv).kind != RK_BOXED || repr_of(c, id).kind != RK_BOXED) return &sp;
  if (!poly_block_call_needs_dispatch(c, id)) return &sp;
  int cap = 0;
  cpoly_family(&sp, &cap, PB_ENUM_INLINE);
  return &sp;
}

const PolyPlan *cplan_poly_block(Compiler *c, int id) {
  static PolyPlan bp;
  free(bp.arm);
  bp.arm = NULL; bp.n = 0; bp.flags = 0; bp.native_block = 0;
  bp.ntype = bp.ret = comp_ntype(c, id);
  int cap = 0;
  int cand[CPB_MAX_CANDS];
  int nc = poly_block_dispatch_cands(c, id, cand, CPB_MAX_CANDS);
  const char *name = nt_str(c->nt, id, "name");
  for (int i = 0; i < nc; i++) {
    cpoly_add(&bp, &cap, PA_USER, cand[i], call_target(c, id, cand[i], name, 0, NULL), TY_UNKNOWN, PC_VOID);
    if (c->classes[cand[i]].is_native_class) bp.native_block = 1;
  }
  if (nc == 0) return &bp;
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
  return &bp;
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

/* The method_missing a call on class ci (its class side when cmethod;
   main for -1) falls to: its chain's, else, past a class that is no blank
   slate, Object's (an Object or a Kernel reopen's, a top-level def: main
   has no user class entry), which the tables leave out of a program
   class's chain; -1 for none */
static int cplan_mm_method(Compiler *c, int ci, int cmethod) {
  int t = cplan_mm_chain(c, ci, cmethod);
  if (t >= 0 || (!cmethod && ci >= 0 && class_is_blank_slate(c, ci))) return t;
  int obj = comp_core_class(c, CORE_OBJECT);
  if (obj >= 0 && (t = comp_method_in_chain(c, obj, METHOD_MISSING_NAME, NULL)) >= 0) return t;
  return comp_method_index(c, METHOD_MISSING_NAME);
}
/* Missing hooks are not dispatched yet. New visibility checks retain the
   old self-call behaviour when the existing method table defines a hook. */
int cplan_has_method_missing(Compiler *c, int id, int ci, int cmethod) {
  if (ci < 0 && !cmethod) {
    Scope *s = comp_scope_of(c, id);
    if (s && !s->is_cmethod) ci = s->class_id;
    if (ci < 0 && !self_is_main(c, id)) return 0;
  }
  return cplan_mm_method(c, ci, cmethod) >= 0;
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
      ((k == NK_SuperNode || k == NK_ForwardingSuperNode) && cplan_user(w->c, leaf->node)->mi < 0)) {
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
    int sup = cplan_body_super(c, mm), up = sup >= 0 ? cplan_user(c, sup)->mi : -1;
    if (sup < 0 || (up >= 0 && cplan_mm_answers(c, up))) *f |= SSF_MM_ANSWERS;
  }
  return (*f & SSF_MM_ANSWERS) != 0;
}
/* Does super `id` reach a method whose value is its own super that reaches
   no method (cplan_value_dead_super), where CRuby calls a method_missing
   Spinel does not dispatch: one that answers for self (cplan_mm_answers), on
   the chain of the class or of a descendant, as self may be one? */
int cplan_super_needs_method_missing(Compiler *c, int id) {
  const CallPlan *p = cplan_user(c, id);
  Scope *s = comp_scope_of(c, id);
  if (!s || p->via != UC_SUPER || p->mi < 0 || !cplan_value_dead_super(c, p->mi)) return 0;
  for (int k = -1; k < c->nclasses; k++) {
    if (k >= 0 && (k == s->class_id || !is_descendant(c, k, s->class_id))) continue;
    int mm = cplan_mm_method(c, k < 0 ? s->class_id : k, s->is_cmethod);
    if (mm >= 0 && cplan_mm_answers(c, mm)) return 1;
  }
  return 0;
}

/* Whether the boxed class arm calls the method rather than raising for
   its visibility. Explicit self may call private methods too.
   The class-body self stamp identifies the caller of a protected call. */
int cplan_cmethod_visible(Compiler *c, int id, int ci, const char *name) {
  if (nt_ignore_visibility(c->nt, id)) return 1;
  int owner = -1;
  int vis = comp_cmethod_vis_declared(c, ci, name, &owner);
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

/* The class method a value of class k answers `nm` with in a boxed
   class-method dispatch of call `call` (cplan_poly_cmeth, and
   emit_unresolved_call's), or -1 when it has no arm there. A yielding class
   method has no symbol of its own; its proc-form clone is the arm, taking
   the block as a proc (#3399), through *pfk. */
int cplan_cmethod_arm(Compiler *c, int call, int k, const char *nm, MethodLookup *ml, int *pfk) {
  int mi = call_target(c, call, k, nm, CT_CMETHOD, ml);
  if (mi < 0) return -1;
  int pf = scope_proc_form_of(c, mi);
  if (pf >= 0 && !proc_form_live(c, pf)) pf = -1;
  if (!scope_has_callable_symbol(c, mi) && pf < 0) return -1;
  if (pfk) *pfk = pf;
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
    if (!cvals[k] || cls_sg_accessor_first(c, id, k, name, argc) || cplan_cmethod_arm(c, id, k, name, NULL, NULL) >= 0)
      continue;
    int framed = elab_plan_enter(c, id);
    unsigned unsettled = elab_class_unsettled(c, k, all, id, cplan_plain_ctx());
    if (framed) elab_plan_leave(c);
    if (unsettled == all) continue;
    cpoly_add(p, cap, PA_TRIAL, PA_KEY_CLASS_VALUE + k, -1, TY_UNKNOWN, 0);
    p->arm[p->n - 1].unsettled = (unsigned char)unsettled;
  }
}

const PolyPlan *cplan_poly_cmeth(Compiler *c, int id) {
  static PolyPlan cp;
  free(cp.arm);
  cp.arm = NULL; cp.n = 0; cp.flags = 0;
  cp.ntype = cp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  if (!name) return &cp;
  int argc = 0;
  call_args(c->nt, id, &argc);
  for (int k = 0; k < c->nclasses; k++) {
    if (cls_sg_accessor_first(c, id, k, name, argc) || !cplan_cmethod_visible(c, id, k, name)) continue;
    int pf = -1;
    int mi = cplan_cmethod_arm(c, id, k, name, NULL, &pf);
    if (mi < 0) continue;
    TyKind mret = c->scopes[pf >= 0 ? pf : mi].ret;
    cpoly_add(&cp, &cap, pf >= 0 ? PA_PROC_FORM : PA_USER, k, mi, mret, mret == TY_POLY ? PC_SAME : PC_BOX);
  }
  cpoly_cls_value_trials(c, id, name, argc, &cp, &cap);
  return &cp;
}

PolyPlan *cplan_poly_cls_values(Compiler *c, int id) {
  PolyPlan *vp = calloc(1, sizeof *vp);
  if (!vp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  vp->ntype = vp->ret = comp_ntype(c, id);
  int cap = 0, argc = 0;
  const char *name = nt_str(c->nt, id, "name");
  call_args(c->nt, id, &argc);
  if (name) cpoly_cls_value_trials(c, id, name, argc, vp, &cap);
  return vp;
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
        !(others[i] == CORE_OBJECT && comp_method_vis_declared(c, ok, name, NULL) == SP_VIS_PUBLIC)) return 0;
    /* (a block into one that yields keeps the old path: the default arm
       does not hand Object's method the block) */
    if (omi >= 0 && nt_ref(nt, id, "block") >= 0 && c->scopes[omi].yields) return 0;
  }
  /* (a private or protected one keeps the old path: the boxed dispatch
     does not check visibility) */
  int nk = comp_core_class(c, CORE_NILCLASS);
  if (comp_method_vis_declared(c, nk, name, NULL) != SP_VIS_PUBLIC) return 0;
  int k = comp_core_class(c, core);
  return k < 0 || call_target(c, id, k, name, 0, NULL) < 0;
}

const PolyPlan *cplan_poly_sub(Compiler *c, int id) {
  static PolyPlan sp;
  free(sp.arm);
  sp.arm = NULL; sp.n = 0; sp.flags = 0;
  sp.ntype = sp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  if (!name || recv < 0) return &sp;
  if (cplan_nil_scalar_reach(c, id)) { cpoly_family(&sp, &cap, PB_SUBDISPATCH); return &sp; }
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_object(rt) || comp_ty_value_obj(c, rt)) return &sp;
  int bcid = ty_object_class(rt);
  for (int k = 0; k < c->nclasses; k++) {
    if (k == bcid) continue;
    int anc = 0;
    for (int p2 = c->classes[k].parent; p2 >= 0; p2 = c->classes[p2].parent)
      if (p2 == bcid) { anc = 1; break; }
    if (anc && (call_target(c, id, k, name, 0, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL))) {
      cpoly_family(&sp, &cap, PB_SUBDISPATCH);
      break;
    }
  }
  return &sp;
}

const PolyPlan *cplan_poly_redispatch(Compiler *c, int id) {
  static PolyPlan rp;
  free(rp.arm);
  rp.arm = NULL; rp.n = 0; rp.flags = 0;
  rp.ntype = rp.ret = comp_ntype(c, id);
  int cap = 0;
  const char *name = nt_str(c->nt, id, "name");
  int recv = nt_ref(c->nt, id, "receiver");
  int argc = 0;
  (void)call_args(c->nt, id, &argc);
  if (!name || recv < 0 || comp_recv_type(c, recv) != TY_POLY) return &rp;
  int kind = poly_redispatch_kind(c, id, name, argc);
  if (kind) cpoly_family(&rp, &cap, kind == 2 ? PB_REDISPATCH_RECV : PB_REDISPATCH);
  return &rp;
}

const PolyPlan *cplan_poly_face(Compiler *c, int id) {
  static PolyPlan fp;
  free(fp.arm);
  fp.arm = NULL; fp.n = 0; fp.flags = 0;
  fp.ntype = fp.ret = comp_ntype(c, id);
  int cap = 0;
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int argc = 0;
  call_args(nt, id, &argc);
  int has_blk = nt_ref(nt, id, "block") >= 0, plain = nt_call_args_plain(nt, id);
  if (!name) return &fp;
  unsigned own = an_zero_arg_builtin_shadowed(c, name, argc) ? 0
                 : ty_poly_face_owners(name, argc, has_blk, plain, 0);
  unsigned kinds = own & PF_OWNERS;
  if (own & PF_STR_BANG) { cpoly_family(&fp, &cap, PB_FACE_STR_BANG); return &fp; }
  for (unsigned kind = 1; kind & PF_OWNERS; kind <<= 1) {
    if (!(kinds & kind)) continue;
    /* an argument of another kind is the owner's TypeError */
    int misfit = face_args_misfit(c, id, kind);
    cpoly_add(&fp, &cap, misfit ? PA_BUILTIN : PA_TRIAL, PA_KEY_FACE + face_kind_index(kind), -1, TY_UNKNOWN,
              PC_SAME);
  }
  return &fp;
}

PolyPlan *cplan_poly_copy(const PolyPlan *p) {
  PolyPlan *q = malloc(sizeof *q);
  *q = *p;
  q->arm = malloc(sizeof *q->arm * (size_t)(p->n > 0 ? p->n : 1));
  if (p->n) memcpy(q->arm, p->arm, sizeof *q->arm * (size_t)p->n);
  return q;
}

void cplan_poly_free(PolyPlan *p) {
  if (!p) return;
  free(p->arm);
  free(p);
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

/* Does the program give nil a method `name` of its own: on NilClass,
   Object, Kernel or BasicObject, or at the top level (a private Object
   method, which CRuby names as such), for call `call`? */
static int cplan_nil_user_method(Compiler *c, int call, const char *name) {
  static const int owners[] = { CORE_NILCLASS, CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  if (call_target(c, call, -1, name, CT_TOP, NULL) >= 0) return 1;
  for (int i = 0; owners[i] >= 0; i++) {
    int k = comp_core_class(c, owners[i]);
    if (k >= 0 && call_target(c, call, k, name, 0, NULL) >= 0) return 1;
  }
  return 0;
}

int cplan_nil_program_method(Compiler *c, int call, const char *name, int *nilcls) {
  int nk = comp_core_class(c, CORE_NILCLASS), ok = comp_core_class(c, CORE_OBJECT);
  int mi = nk >= 0 ? call_target(c, call, nk, name, 0, NULL) : -1;
  *nilcls = mi >= 0;
  if (mi < 0 && ok >= 0) mi = call_target(c, call, ok, name, 0, NULL);
  return mi;
}

/* nil's answer to respond_to?(name) by a method of the program's: the
   first of NilClass, Object (a top-level def is a private Object method),
   Kernel and BasicObject that defines it answers, true when that method is
   public or include_all is set (a literal `true` 2nd argument); -1 when
   none of them defines it. */
int cplan_nil_program_responds(Compiler *c, const char *name, int include_all) {
  static const int owners[] = { CORE_NILCLASS, CORE_OBJECT, CORE_KERNEL, CORE_BASIC_OBJECT, -1 };
  /* an undef on NilClass or Object (above the first definer) hides the
     lower owners' method */
  int nk = comp_core_class(c, CORE_NILCLASS);
  if (nk >= 0 && comp_is_undeffed_in_chain(c, nk, name)) return 0;
  int ia = include_all < 0 ? 2 : include_all;
  /* NilClass's own public methods (CRuby's NilClass.public_instance_methods
     false): one sits below every program method of Object, Kernel and
     BasicObject, so a top-level `def to_a` leaves nil's answer true */
  for (int i = 0; owners[i] >= 0; i++) {
    if (owners[i] == CORE_OBJECT && builtin_answers_ahead_of("NilClass", name, "Object")) return 1;
    int k = comp_core_class(c, owners[i]);
    if (k < 0 || call_target(c, -1, k, name, 0, NULL) < 0) {
      if (owners[i] == CORE_OBJECT && call_target(c, -1, -1, name, CT_TOP, NULL) >= 0)
        return ia;
      continue;
    }
    return comp_method_vis_declared(c, k, name, NULL) == SP_VIS_PUBLIC ? 1 : ia;
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

int cplan_nil(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || id >= nt->count || nt_kind(nt, id) != NK_CallNode) return CN_NONE;
  int r = nt_ref(nt, id, "receiver");
  const char *nm = cplan_call_name(nt, id);
  if (r < 0 || !nm || nt_safe_nav(nt, id)) return CN_NONE;
  /* the receiver as settled, not as a view retypes it: a poly arm's
     unboxed String is never its box's nil */
  TyKind rt = c->ntype[r];
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
  if (!nil_own && cplan_nil_user_method(c, id, nm))
    return (rr.kind == RK_PTR || rr.kind == RK_STRBUF) && cplan_nil_runs_other(c, id, rt, nm) &&
           cplan_nil_program_served(c, id)
           ? CN_PROGRAM : CN_NONE;
  /* an arm of a lowered `send` on a user object, for a name its class does
     not define: the arm reaches a Kernel method, which nil answers through
     send as any object does (`nil.send(:Integer, "12")`) */
  if (nt_int(nt, id, "dyn_arm", 0) && ty_is_object(rt)) {
    int k = ty_object_class(rt);
    if (call_target(c, id, k, nm, 0, NULL) < 0 && !comp_reader_in_chain(c, k, nm, NULL)) return CN_NONE;
  }
  /* the typed String bang emitter tests a receiver it does not write back
     into a variable itself (sp_nil_recv), and marks the plain form it emits
     for it: a second test here would only repeat that one */
  if (nt_int(nt, id, "recv_nil_tested", 0)) return CN_NONE;
  if (rt == TY_STRING && ty_str_typed_bang_flags(nm) && comp_ntype(c, id) != TY_ENUMERATOR &&
      !str_mut_var_recv(c, r) && !sb_shadowed_reader(r))
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
static void ver_pin(Compiler *c, int node, int scope) {
  if (!(c->nver_pins & (c->nver_pins - 1)) &&
      !(c->ver_pins = realloc(c->ver_pins, sizeof *c->ver_pins * (size_t)(c->nver_pins ? 2 * c->nver_pins : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  int i = c->nver_pins++;
  for (; i > 0 && c->ver_pins[i - 1].node > node; i--) c->ver_pins[i] = c->ver_pins[i - 1];
  c->ver_pins[i] = (struct VerPin){ node, scope };
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
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, int na, int els_to, int els);
/* What an open history's entry runs when no version has run yet, decided
   for every history before any entry is made (ver_named_kind):
   VK_SUPER the method super reaches; VK_IVAR an inherited reader's
   instance variable; VK_MM method_missing; VK_TEXT Kernel's to_s or
   inspect through super(); VK_BUILTIN the builtin a class answers on itself
   (bop_class_answers, or a record class's own: elab_record_cmethod),
   called on self; VK_NOMETHOD NoMethodError; VK_KEEP
   no entry, where none of these is CRuby's answer: the history keeps the
   handling it had (its last version taken as run); a version entry's
   else arm the same, VK_KEEP its last version (ver_entry_make) */
enum { VK_SUPER, VK_IVAR, VK_MM, VK_TEXT, VK_BUILTIN, VK_NOMETHOD, VK_KEEP };
struct VerNamedAsks;
static int ver_named_kind(const struct VerNamedAsks *q, int cls, int sg, const char *name, const int *to, int nv, int hist, int vis, int ar, int blk, int mm);
static int ver_absent_else(Compiler *c, int els, int rcls, int sg, const char *name, const int *to, int na, int ar, int *blk, int *spliced);
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
static int ver_builtin_chain(Compiler *c, int cls, int sg, const char *name, int ar, int blk) {
  if (sg && (bop_class_answers(name, ar, blk) || elab_record_class_of(c, cls) >= 0)) return 0;
  if (cls >= 0 && class_builtin_parent(c, cls) != builtin_class_at(CORE_OBJECT)->id) return 1;
  for (int k = cls, d = 0; k >= 0 && d < 64; k = c->classes[k].parent, d++)
    if (c->classes[k].is_builtin_const && !is_object_base_name(c->classes[k].name)) return 1;
  return 0;
}
/* The version entry of name on rcls's chain (side sg) for ar arguments:
   an arm per version, nearest history first, each testing its history's
   cell; asked for a call that may meet no version (q), its else arm what
   the lookup finds past every version of the chain (ver_absent_else),
   when they share a visibility. Its arms call class into's scope of each
   version, and it is into's method: rcls's own, or the copies a class
   holds of module rcls's versions, which are the module's instance
   methods also where an extend copies them to into's class side. -1 for
   none. */
static int ver_entry_make(Compiler *c, const struct VerNamedAsks *q, int rcls, int into, int sg, const char *name, int mi, int ar, int mm,
                          int *absent, int *finds) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  int to[VER_MAX_ARMS], na = 0, blk = c->scopes[mi].blk_param || c->scopes[mi].yields, dc, same = 1, vis = 1, els = -1, fallback = mi, kind = VK_KEEP;
  int spliced = c->scopes[mi].yields;
  char sel[VER_MAX_ARMS][32];
  int nv = elab_chain_versions(c, rcls, rcls != into ? 0 : sg, name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE) continue;   /* its write passes the search on */
    int s = vs[i].v->kind == LR_UNDEF ? -1 : ct_def_version(c, into, name, sg, vs[i].v, &dc);
    if (s < 0 || c->scopes[s].is_transplanted_source || (!c->scopes[s].replaced && scope_is_shadowed(c, s)))
      return -1;
    blk |= c->scopes[s].blk_param || c->scopes[s].yields;
    spliced &= c->scopes[s].yields;
    vis &= vs[i].v->vis == vs[0].v->vis;
    ver_cell_name(vs[i].hist, sel[na]);
    arm[na] = (DispArm){ sel[na], vs[i].n, name, vs[i].hist + 1 };
    to[na++] = s;
    if (nt_int(c->nt, c->scopes[s].def_node, "concat_builtin_version", 0)) fallback = s;
    same &= ver_same_source(c->nt, c->scopes[s].def_node, c->scopes[mi].def_node);
  }
  if (q && na && vis && !ver_builtin_chain(c, rcls, sg, name, ar, blk)) kind = ver_named_kind(q, rcls, sg, name, to, na, vs[nv - 1].hist, vs[0].v->vis, ar, blk, mm);
  if (kind != VK_KEEP) els = ver_absent_else(c, kind, rcls, sg, name, to, na, ar, &blk, &spliced);
  if (absent) *absent = els >= 0;
  *finds = ver_else_finds(c, kind, rcls, sg, name, nv > 0 ? vs[0].v->vis : SP_VIS_PUBLIC, na ? to[na - 1] : -1);
  if (els < 0 && (q || nv < 2 || !na || same)) return -1;   /* (asked for its else arm only: ver_late_make stores no scope for an entry without one) */
  return ver_entry_emit(c, into, sg, name, ar, blk, spliced, arm, to, na, fallback, els);
}
/* The entry's def, its arms' calls pinned to to[] and its else arm's to
   els_to, beside els_to's def; or, given els, that else arm, pinned to
   nothing */
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, int na, int els_to, int els) {
  NodeTable *nt = (NodeTable *)c->nt;
  int pin = els < 0, like = c->scopes[els_to].def_node;
  /* The shared arms end in one fallback, not tests that repeat it. */
  while (pin && na > 0 && to[na - 1] == els_to) na--;
  if (pin) els = disp_forward_call(nt, c->scopes[els_to].name, ar, blk);
  int def = disp_def_build(nt, like, name, sg, ar, blk, arm, na, els);
  int body = nt_ref(nt, def, "body"), e = c->nscopes;
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
  for (int k = 0, at = ver_first_stmt(nt, body); k < na; k++, at = nt_ref(nt, at, "subsequent"))
    ver_pin(c, ver_first_stmt(nt, nt_ref(nt, at, "statements")), to[k]);
  if (pin) ver_pin(c, els, els_to);
  return e;
}
/* The copies that are the versions of one module method a mixin copies
   (analyze_scope.c, mixin_prev_version): those of mi's class and side
   under mi's name, with its cell and the method it stands in front of
   where a later include or prepend guards them, each with the record's
   version it copies on the chain of class `from` (vs[k], whose history's
   cell tells it): mi's class's own, or the module's, whose instance
   methods an extend's class-side copies copy. In the chain's order.
   How many, 0 where one is not on the chain (mi stands alone). */
static int mixin_copy_versions(Compiler *c, int mi, int from, int *to, ElabVersion *vs) {
  const Scope *m = &c->scopes[mi];
  ElabVersion cv[VER_MAX_ARMS];
  int side = m->is_extend_copy && from == m->origin_module_ci - 1 ? 0 : m->is_cmethod;
  int n = 0, ns = 0, nv = elab_chain_versions(c, from, side, comp_scope_user_name(m), cv, VER_MAX_ARMS), at[VER_MAX_ARMS];
  for (int i = 0; i < nv; i++) at[i] = -1;
  /* (one walk: the copies under mi's name, and each version's first one) */
  for (int s = 0; s < c->nscopes; s++) {
    const Scope *k = &c->scopes[s];
    if (k->class_id != m->class_id || k->is_cmethod != m->is_cmethod || k->mixin_guarded != m->mixin_guarded ||
        k->guard_behind != m->guard_behind || !k->name || !sp_streq(k->name, m->name)) continue;
    ns += !k->entry;
    for (int i = 0; i < nv; i++)
      if (at[i] < 0 && cv[i].v->kind == LR_DEF && k->def_node == cv[i].v->node) at[i] = s;
  }
  for (int i = 0; i < nv; i++) {
    /* (a module included again lists its versions again: one copy each) */
    int dup = at[i] < 0;
    for (int j = 0; j < n && !dup; j++) dup = to[j] == at[i];
    if (dup) continue;
    vs[n] = cv[i];
    to[n++] = at[i];
  }
  return ns > 1 && n == ns ? n : 0;
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
  *n = mixin_copy_versions(c, mi, m->origin_module_ci - 1, to, vs);
  return *n ? m->origin_module_ci - 1 : -1;
}
/* The entry over the versions of guarded copy mi, one layer of a guard
   chain (mixin_copy_versions, asked of class from's chain: mi's class's,
   or for a layer below the mixins the module's, super_layer_versions): an
   if-chain over the cells of their histories, nearest first, whose arm
   for each version calls its copy; mi where it stands alone, -1 where
   none can be made */
static int guard_layer_versions(Compiler *c, int mi, int from, int ar) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int to[VER_MAX_ARMS], nv = mixin_copy_versions(c, mi, from, to, vs), blk = 0, spliced = 1;
  if (!nv) return mi;
  for (int k = 0; k < nv; k++) {
    blk |= c->scopes[to[k]].blk_param || c->scopes[to[k]].yields;
    spliced &= c->scopes[to[k]].yields;
    ver_cell_name(vs[k].hist, sel[k]);
    arm[k] = (DispArm){ sel[k], vs[k].n, c->scopes[to[k]].name, vs[k].hist + 1 };
  }
  const Scope *m = &c->scopes[mi];
  return ver_entry_emit(c, m->class_id, m->is_cmethod, comp_scope_user_name(m), ar, blk, spliced, arm, to, nv, to[nv - 1], -1);
}
/* The copies t is one of, a mixin's unguarded copies of a module's
   versions under one hidden name (a layer the class's chain holds below a
   later mixin's copy, which the prepend chain's super reaches: sb22's
   middle include), into to: their count, 0 for none (mixin_copy_versions,
   asked of the module's chain). For the super of s (NULL: none), also a
   later mixin's guarded copies below s's that always ran before s's mixin
   did (layer_ran_before): their cell is set wherever s runs, and the
   copies are those its guard entry's arm tests, of the versions on their
   class's chain (guard_entry_make). */
static int layer_ran_before(Compiler *c, const Scope *s, const Scope *t);
static int super_layer_versions(Compiler *c, const Scope *s, int t, int *to) {
  ElabVersion vs[VER_MAX_ARMS];
  const Scope *m = &c->scopes[t];
  if (m->mixin_guarded && s && !m->entry && m->shadow && layer_ran_before(c, s, m)) return mixin_copy_versions(c, t, m->class_id, to, vs);
  if (!(m->is_include_copy || m->is_extend_copy) || !m->shadow || m->mixin_guarded || m->entry || m->mixin_unversioned || m->origin_module_ci <= 0) return 0;
  return mixin_copy_versions(c, t, m->origin_module_ci - 1, to, vs);
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
/* The methods a super in s may run, for the readers that ask before the
   entries are made, or for no call (the yields and block marking, a
   super's lifted block): the target cplan_super_target answers, or, where
   that is one of a layer's copies of a module's versions
   (super_layer_versions), every one of them, any of which the entry over
   them may run. How many, 0 for none. */
int cplan_super_runs(Compiler *c, Scope *s, int *to) {
  int t = cplan_super_target(c, -1, s, NULL), n = t >= 0 ? super_layer_versions(c, s, t, to) : 0;
  if (t >= 0 && !n) to[n++] = t;
  return n;
}
/* ...the entry over them a super of node id reaches, made for its
   arguments at the entries' build (ver_entries_build, guard 3: by the
   module, its class and the program's name); -1 for none */
static int super_layer_entry(Compiler *c, int id, int t) {
  int to[VER_MAX_ARMS], n = super_layer_versions(c, comp_scope_of(c, id), t, to);
  if (!n) return -1;
  const Scope *m = &c->scopes[t];
  int i = ver_entry_ix(c, VER_ENTRY_LAYER, m->origin_module_ci, m->class_id, m->is_cmethod, comp_scope_user_name(m), ver_arity(c, id, to, n));
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
      if (k != m->class_id && comp_module_includes(c, k, m->class_id, 0)) return 0;
  }
  return 1;
}
/* The guard entry of mi, a copy a later prepend puts in front of a method
   (Scope.mixin_guarded), with ar parameters: the same if-chain over the
   cells of the prepends in front of the method, nearest first, its else
   arm the method behind them all. The arm of a mixin whose module defines
   the method again runs the entry over its copies of the versions
   (guard_layer_versions): whether the mixin has run and which version its
   module holds are told apart, each by its own cell. *behind: the method
   behind them all where the else arm calls method_missing in its place,
   -1 otherwise. */
static int guard_entry_make(Compiler *c, int mi, int ar, int *behind) {
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, arm, sel), blk = 0, spliced = 1;
  *behind = -1;
  if (n < 2) return -1;
  /* Cloned forwarding bodies retain the hidden name even when the clone
     has no pin in the original entry's node index. */
  for (int k = 0; k < n - 1; k++) {
    int e = c->scopes[to[k]].mixin_guarded ? guard_layer_versions(c, to[k], c->scopes[to[k]].class_id, ar) : to[k];
    if (e < 0) return -1;
    arm[k].callee = e == to[k] ? c->scopes[to[k]].name : comp_scope_user_name(&c->scopes[mi]);
    to[k] = e;
  }
  /* (the method behind them all one of a layer's versions below them: the
     entry over those, super_layer_versions) */
  int vt[VER_MAX_ARMS], e = super_layer_versions(c, NULL, to[n - 1], vt) ? guard_layer_versions(c, to[n - 1], c->scopes[to[n - 1]].origin_module_ci - 1, ar) : -1;
  if (e >= 0) to[n - 1] = e;
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
  if (guard_behind_finds_none(c, to[n - 1], cls, sg, name) && cplan_mm_method(c, cls, sg) >= 0) {
    els = ver_absent_else(c, VK_MM, cls, sg, name, to, n, ar, &blk, &spliced);
    *behind = to[n - 1];
  }
  return ver_entry_emit(c, cls, sg, name, ar, blk, spliced, arm, to, n - 1, to[n - 1], els);
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
  int cls, sg, hcls, nv, ar, blk, els, espl, refl, atdef;   /* (espl: the else arm lets a spliced block through, ver_else_block; refl: ver_refl_named; atdef: ver_named_refuse_defs) */
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
  int *mh, *mn, *mk, *seen, *cps, stamp; unsigned long long *bydef; } VerNamedAsks;   /* (ver_mixers_build: the mixins by module, a mark per class side, the scopes by def) */
static int ver_named_else(Compiler *c, const VerNamed *w);
static void ver_named_make(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  const int *to = w->to;
  int spliced = w->espl, nv = w->nv;
  char *spell = NULL;
  for (int i = 0; i < nv; i++) {
    spliced &= c->scopes[to[i]].yields || !c->scopes[to[i]].blk_param;   /* (one with no &b lets it go no further) */
    ver_cell_name(w->vs[i].hist, sel[i]);
    arm[i] = (DispArm){ sel[i], w->vs[i].n, w->name, w->vs[i].hist + 1 };
    if (c->scopes[to[i]].shadow == SHADOW_PREP && !spell) spell = strdup(c->scopes[to[i]].name);
  }
  int els, like = c->scopes[to[nv - 1]].def_node;
  if (w->refuse) nt_node_set_int(nt, els = disp_raise_no_method(c, w->name, w->name, 0), w->refuse, 1);
  else els = ver_named_else(c, w);
  /* (at the last version, where a refusal names it) */
  static const char *const at[] = { "node_line", "node_file", "node_col" };
  for (int i = 0; i < 3; i++) nt_node_set_int(nt, els, at[i], nt_int(nt, like, at[i], 0));
  int e = ver_entry_emit(c, w->cls, w->sg, w->name, w->ar, w->blk, spliced, arm, to, w->refuse ? 0 : nv, to[nv - 1], els);
  c->scopes[e].entry = ENTRY_NAMED;
  if (w->refl) { nt_node_set_int(nt, c->scopes[e].def_node, "cond_def_refl", 1); c->nver_refl++; }
  if (spell) {
    char *u = c->scopes[e].name;
    c->scopes[e].name = spell;
    comp_scope_set_shadow(c, e, SHADOW_PREP, u);
    free(u);
  }
  for (int i = 0; i < nv; i++) { c->scopes[to[i]].replaced = 1; comp_touch_scope(c->scopes[to[i]].idx); }
  /* (an includer's: under the module's instance side) */
  ver_entry_add(c, comp_scope_user_name(&c->scopes[e]), (struct VerEntry){ NULL, VER_ENTRY_HISTORY, w->hcls, w->hcls == w->cls ? w->sg : 0, w->ar, e,
                                      .finds = ver_else_finds(c, w->refuse ? VK_NOMETHOD : w->els, w->cls, w->sg, w->name, w->vs[0].v->vis, to[nv - 1]) });
}
/* ...its else arm, as ver_named_kind decided: NoMethodError's message
   names the receiver's class by its Ruby name, read at run time when cls
   has subclasses, is a module or is a Class.new class (which may have no
   name, or take one only at run time) */
static int ver_named_else(Compiler *c, const VerNamed *w) {
  NodeTable *nt = (NodeTable *)c->nt;
  const char *name = w->name;
  int cls = w->cls, sg = w->sg, nd = 0;
  char msg[512];
  switch (w->els) {
  case VK_SUPER: return disp_super_call(nt, w->ar, w->blk);
  case VK_TEXT: return disp_super_call(nt, 0, 0);
  case VK_MM: comp_sym_intern(c, name); return disp_method_missing_call(nt, name, w->ar, w->blk);   /* (its Symbol, made after the literals were interned) */
  case VK_BUILTIN: {
    /* (a record class's builtin that takes none, with `*r, **k` passed on:
       called where both are empty, as CRuby passes it no argument then) */
    int bare = w->ar < 0 && !is_record_class_cmethod_with_args(name);
    int call = disp_self_call(nt, name, bare ? 0 : w->ar, w->blk);
    ver_pin(c, call, CT_PIN_BUILTIN);
    return bare ? disp_when_unspread(c, call) : call;
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
  int mod = cls >= 0 && comp_class_is_module(c, &c->classes[cls]);
  comp_sym_intern(c, name);   /* (the error's name, a Symbol made after the literals were interned) */
  if (cls >= 0) comp_descendants(c, cls, &nd);
  if (cls < 0) snprintf(msg, sizeof msg, "undefined method '%s' for main", name);
  else if (nd || (mod && !sg) || comp_class_anonymous(c, cls)) {
    snprintf(msg, sizeof msg, "undefined method '%s' for %s", name, sg ? "class " : "an instance of ");
    return disp_raise_no_method_self(c, name, msg, !sg, w->ar);
  }
  else if (sg) snprintf(msg, sizeof msg, "undefined method '%s' for %s %s", name, mod ? "module" : "class", class_ruby_name(c, cls));
  else snprintf(msg, sizeof msg, "undefined method '%s' for an instance of %s", name, class_ruby_name(c, cls));
  return disp_raise_no_method(c, name, msg, w->ar);
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
   there, mm 1), NoMethodError, but VK_KEEP for a name a bare call
   names, which CRuby answers with NameError there (the entry, shared by
   every call, does not see the call's form) */
static int ver_name_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void ver_name_add(const char ***v, int *n, const char *name) {
  if (!name) return;
  if (!(*n & (*n - 1)) && !(*v = realloc(*v, sizeof **v * (size_t)(*n ? 2 * *n : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  (*v)[(*n)++] = name;
}
static int ver_named_kind(const VerNamedAsks *q, int cls, int sg, const char *name, const int *to, int nv, int hist, int vis, int ar, int blk, int mm) {
  Compiler *c = q->c;
  ElabVersion cv[VER_MAX_ARMS];
  int par = comp_super_parent(c, cls, sg), st, last = -1, nc;
  for (int i = 0; i < nv; i++) if (to[i] < q->nsup && q->sup[to[i]]) return VK_KEEP;
  if (cls >= 0 && (st = cplan_super_target(c, -1, &c->scopes[to[nv - 1]], NULL)) >= 0) {
    /* (a module's copy has the visibility the module's own table gives it) */
    int sc = c->scopes[st].class_id, om = c->scopes[st].origin_module_ci - 1;
    int sv = om >= 0 ? comp_method_vis(&c->classes[om], name) : sg ? comp_cmethod_vis_declared(c, sc, name, NULL) : comp_method_vis_in_chain(c, sc, name);
    return sc >= 0 && sv == vis ? VK_SUPER : VK_KEEP;
  }
  if (!sg && cls >= 0 && !ar && !blk && (is_inspect_name(name) || is_to_s_name(name)) && builtin_every_object_answers(name, 0)) return VK_TEXT;
  /* (a record class's own builtin takes `*r, **k` passed on too: ar -1) */
  if (sg && cls >= 0 && ((ar >= 0 && bop_class_answers(name, ar, blk)) || elab_record_cmethod(c, cls, name))) return VK_BUILTIN;
  if (cplan_other_answers(c, cls, sg, name, 1)) return VK_KEEP;
  if (par >= 0 && !sg && !ar && !blk && comp_reader_in_chain(c, par, name, NULL)) return comp_method_vis_in_chain(c, par, name) == vis ? VK_IVAR : VK_KEEP;
  nc = elab_chain_versions(c, cls, sg, name, cv, VER_MAX_ARMS);
  for (int j = 0; j < nc; j++) if (cv[j].hist == hist) last = j;
  if (last < nc - 1) return VK_KEEP;
  /* a class's or module's hook (included, inherited, method_added and the
     like) with no version run is the core's no-op, which super reaches */
  if (sg && cls >= 0 && ar == 1 && !blk && is_module_hook_name(name)) return VK_SUPER;
  /* (method_missing where the calls run: there at some and not at
     others, the history keeps its handling; at none, NoMethodError) */
  if (cplan_mm_method(c, cls, sg) >= 0 && (mm & 1)) return mm == 1 ? VK_MM : VK_KEEP;
  return q->nvc && bsearch(&name, q->vc, (size_t)q->nvc, sizeof *q->vc, ver_name_cmp) ? VK_KEEP : VK_NOMETHOD;
}
/* Does the method the else arm els calls (super's target, method_missing)
   take a block, which the entry then passes on (1), and is it spliced, a
   block it is given flowing in like a yield (2)? */
static int ver_else_block(Compiler *c, int cls, int sg, int last, int els) {
  int t = -1;
  if (els == VK_SUPER) t = cplan_super_target(c, -1, &c->scopes[last], NULL);
  else if (els == VK_MM) t = cplan_mm_method(c, cls, sg);
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
static int ver_absent_else(Compiler *c, int els, int rcls, int sg, const char *name, const int *to, int na, int ar, int *blk, int *spliced) {
  static const char *const at[] = { "node_line", "node_file", "node_col" };
  int eb = ver_else_block(c, rcls, sg, to[na - 1], els), like = c->scopes[to[na - 1]].def_node;
  *blk |= eb != 0;
  *spliced &= eb != 1;
  VerNamed w = { .cls = rcls, .sg = sg, .ar = ar, .blk = *blk, .els = els, .name = name };
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
  case VK_KEEP:   /* (main's: the versions', a private one unless declared) */
    if (cls >= 0) vis = sg ? comp_cmethod_vis_declared(c, cls, name, NULL) : comp_method_vis_in_chain(c, cls, name);
    break;
  case VK_SUPER:
    if (last < 0 || cplan_super_target(c, -1, &c->scopes[last], NULL) < 0) vis = SP_VIS_PRIVATE;
    break;
  default: break;
  }
  return vis == SP_VIS_PUBLIC ? VER_FINDS_PUBLIC : VER_FINDS_HIDDEN;
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
  int blk = 0, ar = ver_arity(c, -1, to, nv), els = VK_NOMETHOD, eb;
  for (int i = 0; i < nv; i++) blk |= c->scopes[to[i]].blk_param || c->scopes[to[i]].yields;
  if (!refuse) els = ver_named_kind(q, cls, sg, name, to, nv, vs[0].hist, vs[0].v->vis, ar, blk, 1);
  if (els == VK_KEEP && !held) return;
  if (els == VK_KEEP) refuse = "cond_def_override";
  eb = refuse ? 0 : ver_else_block(c, cls, sg, to[nv - 1], els);
  blk |= eb != 0;
  if (!(q->n & (q->n - 1)) && !(q->v = realloc(q->v, sizeof *q->v * (size_t)(q->n ? 2 * q->n : 1)))) {
    fprintf(stderr, "spinel: out of memory\n"); exit(1);
  }
  VerNamed *w = &q->v[q->n++];
  *w = (VerNamed){ cls, sg, hcls, nv, ar, blk, els, blk && eb != 1, !refuse && held, 0, name, refuse, { { 0 } }, { 0 } };
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
  for (int i = 0; i < nv; i++) {
    const LrDef *d = vs[i].v;
    int s = d->kind == LR_DEF || d->kind == LR_DEFINE_METHOD ? d->scope : 0, st = d->node;
    while (st >= 0 && par[st] >= 0 && nt_kind(c->nt, par[st]) != NK_StatementsNode) st = par[st];   /* (its cell write's, ver_cells_write) */
    if (nt_int(c->nt, d->node, "cond_arm", 0) || ver_in_dropped_rescue(c, par, d->node)) return;
    held |= nt_int(c->nt, d->node, "cond_lone", 0) != 0;
    if (s <= 0 || c->scopes[s].class_id != cls || c->scopes[s].is_cmethod != sg || (!mod && c->scopes[s].is_transplanted_source) ||
        st < 0 || par[st] < 0 || (d->may == LR_MAY_IN && st != d->node) || ver_in_unrun_body(c, par, d->node) ||
        (d->kind == LR_DEFINE_METHOD && ver_dm_keywords(c->nt, d->node)) || d->vis != vis0) { bad = 1; continue; }
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
      int mi = ksg ? comp_cmethod_in_class(c, k, name) : comp_method_in_class(c, k, name);
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
    nt_node_set_int(nt, r = disp_raise_no_method(c, w->name, w->name, 0), w->refuse, 1);
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
int cplan_target_runs(const Compiler *c, int t, int mi) {
  if (t < 0 || t == mi) return t >= 0;
  int to[VER_MAX_ARMS + 1], n = c->scopes[t].entry != ENTRY_NONE ? ver_entry_targets(c, t, to, NULL) : 0;
  for (int k = 0; k < n; k++) if (to[k] == mi) return 1;
  return 0;
}
/* (may an instance of class cls, or of a class under it, have a
   respond_to_missing?, which a capture asks where it finds no method?) */
static int capture_hook_reach(Compiler *c, int cls) {
  int nd = 0;
  if (comp_method_in_chain(c, cls, RESPOND_TO_MISSING_NAME, NULL) >= 0) return 1;
  const int *ds = comp_descendants(c, cls, &nd);
  for (int i = 0; i < nd; i++)
    if (comp_method_in_chain(c, ds[i], RESPOND_TO_MISSING_NAME, NULL) >= 0) return 1;
  return 0;
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
/* ...as a Method made of entry e holds them, each the method that runs
   at the bind: an arm that is itself the entry over a mixin's versions
   (guard_layer_versions) by its own arms, under its test and theirs, and
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
    if (to[k] >= 0 && c->scopes[to[k]].entry == ENTRY_DISPATCH) ns = ver_entry_targets(c, to[k], sub, st);
    for (int j = 0; j < ns && m < 2 * (VER_MAX_ARMS + 1); j++, m++) {
      if (tests) tests[2 * m] = t[k], tests[2 * m + 1] = st[j];
      out[m] = sub[j];
    }
    if (ns || m >= 2 * (VER_MAX_ARMS + 1)) continue;
    int sup = to[k] >= 0 && c->scopes[to[k]].super_behind ? cplan_super_target(c, -1, &c->scopes[to[k]], NULL) : -1;
    if (tests) tests[2 * m] = t[k], tests[2 * m + 1] = -1;
    out[m++] = sup >= 0 ? sup : to[k] >= 0 ? to[k] : capture_else_arm(c, e, cap);
  }
  return m;
}
/* See call_plan.h. */
int cplan_method_held(Compiler *c, int e, int cap, int *out) {
  int n = cplan_method_arms(c, e, cap, out, NULL), m = 0;
  for (int k = 0; k < n; k++)
    if (out[k] != CPLAN_ARM_NONE) out[m++] = out[k];
  if (!m) out[m++] = e;
  return m;
}
typedef struct { int guard, cls, sg, mi, ar; const char *name; int from; } VerNeed;   /* (from: VerEntry.from) */
/* ...and, to tell the asks that repeat one, an open addressing table over them (ix holds an index into v, -1 free) */
typedef struct { VerNeed *v; int n; int *ix; unsigned nb; } VerNeeds;
static int ver_need_has(const Compiler *c, const VerNeeds *o, VerNeed w) {
  if (ver_entry_ix(c, w.guard, w.from, w.cls, w.sg, w.name, w.ar) >= 0) return 1;
  unsigned at = o->nb ? ver_key_hash(w.cls, w.sg, w.name, w.ar) & (o->nb - 1) : 0;
  for (; o->nb && o->ix[at] >= 0; at = (at + 1) & (o->nb - 1)) {
    const VerNeed *v = &o->v[o->ix[at]];
    if (v->guard == w.guard && v->from == w.from && v->cls == w.cls && v->sg == w.sg && v->ar == w.ar && sp_streq(v->name, w.name)) return 1;
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
/* The part of an ask that a class's chain decides, not the call: whether
   the chain holds two versions, the method the table answers, and the
   arity of the entry, for (class, flags, name, the call's plain argument
   count, ver_call_n) of an ask not on main (CT_TOP, which reads the call's
   own place). A call of a name that no type bounds yet asks every class
   holding two versions of it, so each was redone per call and class:
   classes x calls (#10546's an_pre_fixpoint). ok 0: no entry is asked. */
typedef struct { int cls; unsigned flags; const char *name; int n; int ok, mi, sg, ar; } VerPre;
static VerPre *ver_pre_tab; static int ver_pre_n; static unsigned ver_pre_nb;
static unsigned ver_pre_hash(int k, unsigned flags, const char *nm, int n) {
  return ver_mix(sp_strhash(nm) * 31u + (unsigned)(k + 1) * 131u + flags * 7u + (unsigned)(n + 1));
}
static const VerPre *ver_pre(Compiler *c, int id, const char *nm, int k, unsigned flags) {
  ElabVersion vs[VER_MAX_ARMS];
  int n = ver_call_n(c, id);
  if (ver_pre_n * 2 >= (int)ver_pre_nb) {
    unsigned onb = ver_pre_nb; VerPre *op = ver_pre_tab;
    ver_pre_nb = onb ? onb * 2 : 256;
    if (!(ver_pre_tab = calloc(ver_pre_nb, sizeof *ver_pre_tab))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (unsigned b = 0; b < onb; b++) {
      if (!op[b].name) continue;
      unsigned at = ver_pre_hash(op[b].cls, op[b].flags, op[b].name, op[b].n) & (ver_pre_nb - 1);
      while (ver_pre_tab[at].name) at = (at + 1) & (ver_pre_nb - 1);
      ver_pre_tab[at] = op[b];
    }
    free(op);
  }
  unsigned at = ver_pre_hash(k, flags, nm, n) & (ver_pre_nb - 1);
  for (; ver_pre_tab[at].name; at = (at + 1) & (ver_pre_nb - 1)) {
    const VerPre *w = &ver_pre_tab[at];
    if (w->cls == k && w->flags == flags && w->n == n && sp_streq(w->name, nm)) return w;
  }
  VerPre p = { k, flags, nm, n, 0, -1, 0, 0 };
  unsigned fl = flags;
  if (elab_chain_versions(c, k, (flags & CT_CMETHOD) != 0, nm, vs, VER_MAX_ARMS) >= 2 &&
      (p.mi = ct_lookup(c, id, k, nm, &fl, NULL)) >= 0 && !c->scopes[p.mi].mixin_guarded && ct_versioned(c, p.mi, nm)) {
    p.ok = 1;
    p.sg = (fl & CT_CMETHOD) != 0;
    p.ar = ver_call_arity(c, id, k, p.sg, nm, p.mi);
  }
  ver_pre_tab[at] = p;
  ver_pre_n++;
  return &ver_pre_tab[at];
}
static void ver_need(Compiler *c, int id, const char *nm, int k, unsigned flags, void *arg) {
  ElabVersion vs[VER_MAX_ARMS];
  int mi, ar, rcls = flags & CT_TOP ? -1 : k, sg;
  MethodLookup ml = { .survey = 1, .def_class = -1 };
  int asked = 1;
  mtab_enter(c);
  /* (an entry arms two or more versions of the chain: with fewer, or a
     chain the record does not hold, no call is asked. A call on main may
     ask main's chain, which ends in Object's: ct_lookup. A later prepend's
     copy has its guard entries: ver_entries_build) */
  if (!(flags & CT_TOP)) {
    const VerPre *p = ver_pre(c, id, nm, k, flags);
    asked = p->ok;
    mi = p->mi; sg = p->sg; ar = p->ar;
  }
  else if (elab_chain_versions(c, rcls, 1, nm, vs, VER_MAX_ARMS) < 2 ||
           (mi = ct_lookup(c, id, k, nm, &flags, NULL)) < 0 || c->scopes[mi].mixin_guarded || !ct_versioned(c, mi, nm)) asked = 0;
  else {
    sg = (flags & CT_CMETHOD) != 0;
    ar = ver_call_arity(c, id, rcls, sg, nm, mi);
  }
  if (asked) {
    ct_version(c, id, k, nm, flags, mi, &ml);
    /* (the copies' entry over the module's versions: mixin_copies_module) */
    int to[VER_MAX_ARMS], nc = 0;
    VerNeed w = { VER_ENTRY_CHAIN, rcls, sg, mi, ar, nm, ml.copies ? mixin_copies_module(c, mi, to, vs, &nc) + 1 : 0 };
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
    if (e->scope < 0 || e->guard == VER_ENTRY_HISTORY || e->guard == VER_ENTRY_LAYER) continue;   /* (an open history's entry is the table's answer; a super's layer entry none) */
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
  *q = (VerNamedAsks){ c, NULL, 0, sup, c->nscopes, vc, nvc, rn, nrn, demand, NULL, NULL, NULL, NULL, NULL, 0, NULL };
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
void ver_entries_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  const char *names[2];
  VerNeeds o = { NULL, 0, NULL, 0 };
  int *gc = NULL, ngc = 0;
  for (int s = 0; s < c->nscopes; s++) {   /* the later prepends' copies */
    if (!c->scopes[s].mixin_guarded) continue;
    const Scope *m = &c->scopes[s];
    /* (a module's own guarded copy, of a late include into a reopened
       module, is emitted only as its includers' clones, which carry the
       guard and get entries of their own: one on the module would call a
       copy that no function is emitted for) */
    if (m->class_id >= 0 && comp_class_is_module(c, &c->classes[m->class_id])) continue;
    const char *name = comp_scope_user_name(m);
    if ((m->is_cmethod ? comp_cmethod_in_class(c, m->class_id, name) : comp_method_in_class(c, m->class_id, name)) != s) continue;
    if (!(ngc & (ngc - 1)) && !(gc = realloc(gc, sizeof *gc * (size_t)(ngc ? 2 * ngc : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    gc[ngc++] = s;
    int to[VER_MAX_ARMS + 1];
    VerNeed w = { VER_ENTRY_GUARD, c->scopes[s].class_id, c->scopes[s].is_cmethod, s,
                  ver_arity(c, -1, to, guard_chain(c, s, to, NULL, NULL)), name };
    if (!ver_need_has(c, &o, w)) ver_need_add(&o, w);
  }
  /* the open histories' entries first: the table answers them, and no call
     of their names asks an entry of the chain behind them */
  ver_named_asks(c, 0);
  if (c->nver_defer) qsort(c->ver_defer, (size_t)c->nver_defer, sizeof *c->ver_defer, ver_name_cmp);
  /* (the asks below make and rename no scope: Compiler.ver_asking) */
  c->ver_asking = 1;
  for (int id = 0, n0 = nt->count; id < n0; id++) {
    /* a respond_to? of a class's own class method reads the cells of the
       entry a call of the name would take there (cplan_cmethod_presence) */
    int pc = cplan_presence_class(c, id);
    if (pc >= 0 && cplan_presence_asks(c, id, pc, method_sym_arg(c, id))) ver_need(c, id, method_sym_arg(c, id), pc, CT_CMETHOD, &o);
    for (int i = 0, nn = call_node_names(c, id, names); i < nn; i++) {
      if (!names[i]) continue;
      if (ct_version_named(c, names[i])) ct_version_classes(c, id, names[i], ver_need, &o);
      for (int j = 0; j < ngc; j++) {
        const Scope *m = &c->scopes[gc[j]];
        int to[VER_MAX_ARMS + 1];
        if (!sp_streq(comp_scope_user_name(m), names[i]) && !is_send_family(names[i])) continue;
        VerNeed w = { VER_ENTRY_GUARD, m->class_id, m->is_cmethod, gc[j], ver_arity(c, id, to, guard_chain(c, gc[j], to, NULL, NULL)), comp_scope_user_name(m) };
        if (!ver_need_has(c, &o, w)) ver_need_add(&o, w);
      }
    }
  }
  /* the supers whose next method is a layer of a mixin's versions: the
     entry over them for each's arguments (super_layer_entry) */
  for (int k = 0; k < 2; k++)
    for (int id = comp_kind_first(c, k ? NK_ForwardingSuperNode : NK_SuperNode); id >= 0; id = comp_kind_next(c, id)) {
      Scope *s = comp_scope_of(c, id);
      int to[VER_MAX_ARMS], t = s && s->class_id >= 0 && s->name ? cplan_super_target(c, id, s, NULL) : -1;
      int n = t >= 0 ? super_layer_versions(c, s, t, to) : 0;
      const Scope *m = n ? &c->scopes[t] : NULL;
      VerNeed w = { VER_ENTRY_LAYER, m ? m->class_id : -1, m ? m->is_cmethod : 0, t, n ? ver_arity(c, id, to, n) : 0, m ? comp_scope_user_name(m) : NULL,
                    m ? m->origin_module_ci : 0 };
      if (n && !ver_need_has(c, &o, w)) ver_need_add(&o, w);
    }
  c->ver_asking = 0;
  int hooks = -1;   /* (guard_else_bare) */
  for (int i = 0; i < o.n; i++) {
    /* (a guard entry's else arm runs the method behind the mixins: for a
       later include's behind, what its super reaches, if anything) */
    int to[VER_MAX_ARMS + 1], n = o.v[i].guard != VER_ENTRY_CHAIN ? guard_chain(c, o.v[i].mi, to, NULL, NULL) : 0;
    int finds = n && guard_else_bare(c, to[n - 1], o.v[i].cls, o.v[i].sg, o.v[i].name, &hooks) ? VER_FINDS_NONE : ver_else_finds(c, VK_KEEP, o.v[i].cls, o.v[i].sg, o.v[i].name, SP_VIS_PUBLIC, -1);
    const Scope *lm = &c->scopes[o.v[i].mi];   /* (a guarded layer's versions are its class's: super_layer_versions) */
    int behind = -1, e = o.v[i].guard == VER_ENTRY_LAYER ? guard_layer_versions(c, o.v[i].mi, lm->mixin_guarded ? lm->class_id : o.v[i].from - 1, o.v[i].ar)
          : o.v[i].guard != VER_ENTRY_CHAIN ? guard_entry_make(c, o.v[i].mi, o.v[i].ar, &behind)
                         : ver_entry_make(c, NULL, o.v[i].from ? o.v[i].from - 1 : o.v[i].cls, o.v[i].cls, o.v[i].sg, o.v[i].name, o.v[i].mi, o.v[i].ar, 1, NULL, &finds);
    ver_entry_add(c, e >= 0 && o.v[i].guard != VER_ENTRY_LAYER ? c->scopes[e].name : o.v[i].name, (struct VerEntry){ NULL, o.v[i].guard, o.v[i].cls, o.v[i].sg, o.v[i].ar, e, .finds = finds, .from = o.v[i].from, .behind = behind + 1 });
  }
  free(o.v); free(o.ix);
  free(ver_pre_tab); ver_pre_tab = NULL; ver_pre_n = 0; ver_pre_nb = 0;   /* (the asks' memo lives for one build) */
  ver_entries_index(c);
  /* The table answers the general entry. Copies keep their own super
     chain under a shadow name; argument-specific entries stay hidden. */
  for (int j = 0; j < ngc; j++) {
    int mi = gc[j], to[VER_MAX_ARMS + 1];
    Scope *m = &c->scopes[mi];
    int ar = ver_arity(c, -1, to, guard_chain(c, mi, to, NULL, NULL));
    int e = ct_entry_of(c, VER_ENTRY_GUARD, m->class_id, m->is_cmethod, comp_scope_user_name(m), ar);
    if (e < 0) continue;
    c->scopes[e].entry = ENTRY_TABLE;
    c->scopes[e].mixin_guarded = m->mixin_guarded;
    c->scopes[e].guard_behind = mi;
  }
  free(gc);
  c->ver_built = 1;
}
/* `cell = v`, in at's scope and on its line (ver_cell_put places it) */
static int ver_cell_node(Compiler *c, const char *cell, int at, int v) {
  NodeTable *nt = (NodeTable *)c->nt;
  int line = (int)nt_int(nt, at, "node_line", 0), tv = nt_new_int(nt, v), g = nt_new_node(nt, "GlobalVariableWriteNode");
  nt_node_set_str(nt, g, "name", cell);
  nt_node_set_ref(nt, g, "value", tv);
  nt_node_set_int(nt, tv, "node_line", line);
  nt_node_set_int(nt, g, "node_line", line);
  /* (the cell holds an Integer from the entry file's first statement on, and nothing can read it before: its seed) */
  comp_gvar_intern(c, cell + 1)->gvar_seeded = 2;
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
/* ...and the cells' writes: after each statement that makes a version an
   entry arms (ahead of it when it is a def, an alias or an undef), the write of
   the version's number to its history's cell (0 for a remove_method's,
   which passes the search on), in the same statement list; and 0 ahead of
   the entry file's first statement (unless that statement makes one of the
   cell's versions), so that the cell holds an Integer wherever an entry
   reads it. Each recorded as the load record records a
   write (elab_record_write). An open history's entry (guard 2) arms its
   own history's versions. The writes are placed list by list, one rebuild
   of each list's statements: ahead of a statement in the order made, after
   it in the reverse order, as one insertion at a time placed them. */
typedef struct { int list, stmt, node, n, hist, after, k, g, body; } VerWrite;   /* body: a method body's version (LrDef.body) */
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
    st = call;
  }
}
static int ver_write_cmp(const void *a, const void *b) {
  const VerWrite *x = a, *y = b;
  if (x->list != y->list) return x->list < y->list ? -1 : 1;
  if (x->stmt != y->stmt) return x->stmt < y->stmt ? -1 : 1;
  if (x->after != y->after) return x->after - y->after;
  return x->after ? y->k - x->k : x->k - y->k;
}
/* The cells each node writes, as they are placed: a cell is written once
   at a node, and one node may make versions in several histories, each with
   a cell of its own (`module_function :a, :b` copies both names at its
   node; a bare module_function def is the instance method's version and the
   singleton's copy; `attr_accessor :x` makes a reader and a writer) */
typedef struct { int *at; struct { int hist, next; } *e; int n; } VerCellsAt;
/* 1 the first time node writes the cell of history hist (recording it),
   else 0 */
static int ver_cell_once(VerCellsAt *m, int node, int hist) {
  for (int k = m->at[node]; k; k = m->e[k - 1].next)
    if (m->e[k - 1].hist == hist) return 0;
  if (!(m->n & (m->n - 1)) && !(m->e = realloc(m->e, sizeof *m->e * (size_t)(m->n ? 2 * m->n : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->e[m->n].hist = hist;
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
  int nw0 = *nwp, *of = nw0 ? calloc((size_t)nt->count + 1, sizeof(int)) : NULL;
  if (!nw0) return;
  if (!of) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nw0; k++) if ((*wp)[k].node >= 0 && (*wp)[k].body) of[(*wp)[k].node] = k + 1;
  NT_FOREACH_KIND(nt, NK_DefNode, d) {
    int o = d, k = 0, st;
    for (int depth = 0; depth < 64 && !k; depth++) {
      if ((o = nt_clone_source(nt, o)) < 0 || o >= nt->count) break;
      k = of[o];
    }
    if (!k || (st = ver_write_stmt(nt, par, d)) < 0 || par[st] < 0 || !ver_cell_once(cells, d, (*wp)[k - 1].hist)) continue;
    if (!(*nwp & (*nwp - 1)) && !(*wp = realloc(*wp, sizeof **wp * (size_t)(*nwp ? 2 * *nwp : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    VerWrite x = (*wp)[k - 1];
    x.list = par[st]; x.stmt = st; x.node = d;
    x.after = !ver_stmt_runs_no_code(nt, st);
    (*wp)[(*nwp)++] = x;
  }
  free(of);
}
void ver_cells_write(Compiler *c) {
  NodeTable *nt = (NodeTable *)c->nt;
  ElabVersion vs[VER_MAX_ARMS];
  VerWrite *w = NULL;
  int nw = 0, top = prog_root_stmts(c, 0), ntop = 0, nh = 0;
  const int *first = top >= 0 ? nt_arr(nt, top, "body", &ntop) : NULL;
  /* (the parents the program's tree reaches: a class_eval block spliced
     into its class body still names its statements, and the statement a
     write goes beside is the class body's) */
  int *par = c->nver_entries ? du_parent_map(nt) : NULL;
  VerCellsAt cells = { c->nver_entries ? calloc((size_t)nt->count + 1, sizeof(int)) : NULL, NULL, 0 };
  if (c->nver_entries && (!cells.at || !par)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int pass = 0; pass < 2; pass++)   /* (the entries written before first: a cell is written once) */
  for (int i = 0; i < c->nver_entries; i++) {
    struct VerEntry *e = &c->ver_entries[i];
    /* (not only the entries reached now: a send's arms, made in the fixpoint, reach methods whose calls bind entries this first pass leaves unreached) */
    if (e->scope < 0 || e->cells == pass) continue;
    e->cells = 1;
    int arms[VER_MAX_ARMS + 1], tests[2 * (VER_MAX_ARMS + 1)];
    int na = e->guard == VER_ENTRY_GUARD || e->guard == VER_ENTRY_LAYER ? ver_entry_targets(c, e->scope, arms, tests) : 0;
    /* (and an arm's own, of the entry over a mixin's versions:
       guard_layer_versions) */
    for (int k = 0, n0 = na; k < n0; k++)
      if (arms[k] >= 0 && c->scopes[arms[k]].entry == ENTRY_DISPATCH) {
        int sub[VER_MAX_ARMS + 1], st[VER_MAX_ARMS + 1];
        for (int j = 0, ns = ver_entry_targets(c, arms[k], sub, st); j < ns && na < 2 * (VER_MAX_ARMS + 1); j++) tests[na++] = st[j];
      }
    /* (a guarded layer's copies: versions on cls's chain, super_layer_versions) */
    int lg = e->guard == VER_ENTRY_LAYER && na && arms[0] >= 0 && c->scopes[arms[0]].mixin_guarded, mod = e->from && !lg;
    int nv = e->guard == VER_ENTRY_HISTORY ? elab_history_versions(c, e->cls, e->sg, e->name, vs, VER_MAX_ARMS)
                           : elab_chain_versions(c, mod ? e->from - 1 : e->cls, mod ? 0 : e->sg, e->name, vs, VER_MAX_ARMS);
    for (int j = 0; j < nv; j++) {
      if (e->guard == VER_ENTRY_GUARD || e->guard == VER_ENTRY_LAYER) {
        int reads = 0;
        for (int k = 0; k < na; k++)
          reads |= nt_int(nt, tests[k], "ver_hist", 0) == vs[j].hist + 1;
        if (!reads) continue;   /* prepend cells are written at their statements */
      }
      int node = vs[j].v->node, st = ver_write_stmt(nt, par, node);
      if (st < 0 || par[st] < 0 || !ver_cell_once(&cells, node, vs[j].hist)) continue;
      if (!pass) continue;
      if (!(nw & (nw - 1)) && !(w = realloc(w, sizeof *w * (size_t)(nw ? 2 * nw : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      /* (ahead of a statement that runs no program code: after one that ends a branch, the write would be the branch's value) */
      w[nw++] = (VerWrite){ par[st], st, node, vs[j].v->kind == LR_REMOVE ? 0 : vs[j].n, vs[j].hist,
                            !ver_stmt_runs_no_code(nt, st), 0, -1, vs[j].v->body != 0 };
      if (vs[j].hist >= nh) nh = vs[j].hist + 1;
      /* Not the lone conditional def's dispatcher alone: it stands after the condition, and the
         method is there as soon as one of its arms (`name#__condN`, cond_disp) runs in its branch */
      if (nt_int(nt, node, "cond_lone_disp", 0)) {
        NT_FOREACH_KIND(nt, NK_DefNode, d) {
          if (!nt_int(nt, d, "cond_arm", 0) || nt_int(nt, d, "cond_disp", -1) != node) continue;
          int ast = ver_write_stmt(nt, par, d);
          if (ast < 0 || par[ast] < 0 || !ver_cell_once(&cells, d, vs[j].hist)) continue;
          if (!(nw & (nw - 1)) && !(w = realloc(w, sizeof *w * (size_t)(nw ? 2 * nw : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
          w[nw++] = (VerWrite){ par[ast], ast, d, vs[j].n, vs[j].hist, nt_kind(nt, ast) != NK_DefNode, 0, -1 };
        }
      }
    }
  }
  ver_copy_writes(c, par, &cells, &w, &nw);
  free(cells.at); free(cells.e); free(par);
  /* the zero writes: one per cell, at its first write, none where the first statement writes it */
  unsigned char *hz = calloc((size_t)nh + 1, 1);
  int at0 = ntop ? first[0] : -1, nz = 0;
  VerWrite *all = malloc(sizeof *all * (size_t)(2 * nw + 1));
  if (!hz || !all) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < nw; k++) if (w[k].stmt == at0) hz[w[k].hist] = 1;
  for (int k = 0; k < nw; k++) {   /* (made in the order one insertion at a time made them) */
    char cell[32];
    ver_cell_name(w[k].hist, cell);
    if (at0 >= 0 && !hz[w[k].hist]) {
      hz[w[k].hist] = 1;
      all[nz] = (VerWrite){ top, at0, -1, 0, w[k].hist, 0, nz, ver_cell_node(c, cell, at0, 0) };
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
  ver_memo_drop(c);   /* (the versions kept before the writes are asked again) */
}
void ver_memo_drop(Compiler *c) {
  for (int i = 0; i < c->ver_memo_cap; i++) free(c->ver_memo[i].name);
  free(c->ver_memo);
  c->ver_memo = NULL;
  c->ver_memo_cap = c->ver_memo_n = 0;
  free(c->guard_memo.at); free(c->guard_memo.to);
  c->guard_memo = (struct GuardMemo){ NULL, 0, NULL, 0, 0 };
}
static int ver_late_key_cmp(const struct VerLate *x, const struct VerLate *y) {
  int d = strcmp(x->name, y->name);
  return d ? d : x->cls != y->cls ? x->cls - y->cls : x->sg != y->sg ? x->sg - y->sg : x->ar - y->ar;
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
    int ix = ver_entry_ix(c, VER_ENTRY_CHAIN, 0, l->cls, l->sg, l->name, l->ar), absent = 0, e, mm = 0, finds = VER_FINDS_NONE;
    if ((i && !ver_late_cmp(l - 1, l)) || (ix >= 0 && c->ver_entries[ix].absent)) continue;
    if (l->span) {   /* (the build's entry for a call that spans versions, unless the key's others made one) */
      if (ix < 0 && !(i && !ver_late_key_cmp(l - 1, l)) && (e = ver_entry_make(c, NULL, l->cls, l->cls, l->sg, l->name, l->mi, l->ar, 1, NULL, &finds)) >= 0) {
        ver_entry_add(c, c->scopes[e].name, (struct VerEntry){ NULL, VER_ENTRY_CHAIN, l->cls, l->sg, l->ar, e, .finds = finds });
        made = 1;
      }
      continue;
    }
    for (int j = i; j < c->nver_late && !ver_late_cmp(l, &c->ver_late[j]); j++) mm |= c->ver_late[j].mm;   /* (over every call of the key) */
    e = ver_entry_make(c, &q, l->cls, l->cls, l->sg, l->name, l->mi, l->ar, mm, &absent, &finds);
    if (ix < 0) ver_entry_add(c, e >= 0 ? c->scopes[e].name : l->name, (struct VerEntry){ NULL, VER_ENTRY_CHAIN, l->cls, l->sg, l->ar, absent ? e : -1, absent ? 2 : 1, .finds = finds });
    else {
      c->ver_entries[ix].absent = absent ? 2 : 1;
      if (absent) {   /* (its cells are written where the other was made) */
        c->ver_entries[ix].scope = e;
        c->ver_entries[ix].finds = finds;
      }
    }
    made |= absent;
  }
  ver_named_facts_free(&q);
  for (int i = 0; i < c->nver_late; i++) free(c->ver_late[i].name);
  c->nver_late = 0;
  return made;
}
/* The deferred histories a call names now (desugar_dynamic_send makes an
   arm per name a send of a run-time name may take, and a pass may retarget
   a call in place): asked through the index of the calls by name, which
   follows both (an_calls_named_first), their entries made and their cells
   written now, as the build made the others' (ver_named_asks,
   ver_cells_write); and the entries the typed calls that may meet no
   version ask, and the respond_to?s whose cells stand for such a call
   (ver_late_make). Changed when it made one. */
int ver_entries_demand(Compiler *c) {
  int any = 0, e0 = c->nver_entries, late;
  for (int i = 0; i < c->nver_defer; i++)
    if (!c->ver_defer[i].taken && an_calls_named_first(c, c->ver_defer[i].name) >= 0) any = c->ver_defer[i].taken = 1;
  /* a respond_to? the cells answer asks the entry a call of the name
     would take where it runs, as such a call asks it (cplan_presence_class) */
  for (int id = an_calls_named_first(c, "respond_to?"); id >= 0; id = an_calls_named_next(id)) {
    int pc = cplan_presence_class(c, id);
    const char *nm = pc >= 0 ? method_sym_arg(c, id) : NULL;
    if (nm && cplan_presence_asks(c, id, pc, nm)) call_target(c, id, pc, nm, CT_CMETHOD | CT_CAPTURE, NULL);
  }
  late = c->nver_late;
  if (!any && !late) return 0;
  if (any) ver_named_asks(c, 1);
  any = c->nver_entries > e0;
  if (late) late = ver_late_make(c);
  ver_entries_index(c);
  ver_cells_write(c);
  return any || late;
}

