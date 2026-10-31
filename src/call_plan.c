/* call_plan.c -- the user method a call binds, resolved from the tables
   (see call_plan.h). */

#include <assert.h>
#include "codegen_internal.h"
#include "codegen_poly.h"
#include "analyze_internal.h"
#include "call_plan.h"
#include "repr.h"
#include "codegen_call_arms.h"

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
  return dispatch_arms_disagree(c, call, cid, name) ? CP_PER_ARM : CP_SWITCH;
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
  p->rkind = CR_NONE; p->msg = NULL; p->rfrom = CRF_NONE;
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

/* A super's parent-chain target, past inherited copies of the same
   extended definition. Shared by resolution, inference, inline splicing and the miss check. */
int cplan_super_chain_mi(Compiler *c, int id, Scope *s, int par, const char *name) {
  int mi = par < 0 ? -1 : s->is_cmethod ? comp_cmethod_in_chain(c, par, name, NULL)
                                       : comp_method_in_chain(c, par, name, NULL);
  /* An extended module's method is one entry in the class's ancestry, and
     `super` inside it goes past the module. Copied into each class that
     inherits the extend, the parent's copy is that same definition, not a
     superclass method: skip such copies (Thor's ClassMethods#method_added
     called on a Thor subclass reaches Module#method_added). */
  while (s->is_cmethod && mi >= 0 && c->scopes[mi].def_node == s->def_node && s->def_node >= 0) {
    par = c->classes[c->scopes[mi].class_id].parent;
    mi = par >= 0 ? comp_cmethod_in_chain(c, par, name, NULL) : -1;
  }
  return mi >= 0 ? call_target(c, id, par, name,
                               CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), NULL) : -1;
}

static void cplan_resolve_super(Compiler *c, int id, CallPlan *p) {
  Scope *s = comp_scope_of(c, id);
  if (!s || s->class_id < 0 || !s->name) return;
  const char *shadow = comp_super_shadow(c, s);
  if (shadow) {
    int mi = s->is_cmethod ? comp_cmethod_in_class(c, s->class_id, shadow)
                           : comp_method_in_class(c, s->class_id, shadow);
    if (mi >= 0) cplan_set(p, mi, s->class_id, UC_SUPER, CP_DIRECT);
    return;
  }
  int par = comp_super_parent(c, s->class_id, s->is_cmethod);
  const char *uname = comp_super_name(c, par, comp_scope_def_name(c, s), s->is_cmethod);
  if (!uname) return;
  int mi = cplan_super_chain_mi(c, id, s, par, uname);
  if (mi >= 0) cplan_set(p, mi, par, UC_SUPER, CP_DIRECT);
  else if (s->is_cmethod &&
           builtin_super_cmethod_known(c->classes[s->class_id].name, uname) &&
           (par < 0 || comp_cmethod_in_chain(c, par, uname, NULL) < 0)) {
    cplan_set(p, -1, s->class_id, UC_SUPER, CP_REFUSE);
    p->rkind = CR_FEATURE; p->rfrom = CRF_LIMIT;
    p->msg = "super to an inherited builtin singleton method is not supported; see docs/limitations.md";
  }
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
  int dn = c->scopes[mi].def_node;
  return dn >= 0 && !cplan_in_closure(c, id) && elab_cmp(elab_point_of(c, id), elab_point_of(c, dn)) == PROG_BEFORE;
}
/* --share-strings: does call `id`, with a literal block, still reach the
   builtin rather than String reopening mi's yielding method of its name,
   for being one the program runs ahead of mi's definition at its top level
   (as in CRuby)? The plan, the inference and the emitter ask it alike. */
int cplan_reopen_runs_builtin(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  /* concat keeps the builtin until Array's definition has run, whether
     or not this call supplies a block. Use the same load-order fact as
     the existing String case, not another method lookup. */
  if (mi >= 0 && c->scopes[mi].class_id == comp_core_class(c, CORE_ARRAY) &&
      sp_streq(nt_str(nt, id, "name"), "concat") && cplan_called_before_def(c, id, mi)) return 1;
  int blk = nt_ref(nt, id, "block");
  return mi >= 0 && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && c->scopes[mi].yields &&
         c->scopes[mi].class_id == comp_core_class(c, CORE_STRING) && cplan_called_before_def(c, id, mi);
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
      if (m->class_id < 0) { cplan_set(p, mi, -1, UC_TOP, CP_DIRECT); return; }
      Scope *self = comp_scope_of(c, id);
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
  if (sci >= 0 || (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)) {
    int ci = sci >= 0 ? sci : const_node_class_index(c, recv);
    int mi = ci >= 0 ? call_target(c, id, ci, name, CT_CMETHOD, NULL) : -1;
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
    if (p->mi < 0) cplan_object_reopen(c, id, name, p);
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
      int aci = comp_class_index(c, anc);
      int ami = aci >= 0 ? call_target(c, id, aci, name, 0, NULL) : -1;
      if (ami >= 0 && c->scopes[ami].class_id == aci) { cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT); return; }
    }
  }
  /* an Array or Hash reopen's own method of the name */
  if ((ty_is_array(rt) || ty_is_obj_array(rt) || ty_is_hash(rt)) &&
      (nt_ref(nt, id, "block") < 0 ||
       ((ty_is_array(rt) || ty_is_obj_array(rt)) && sp_streq(rname, "concat")))) {
    int aci = comp_core_class(c, ty_is_hash(rt) ? CORE_HASH : CORE_ARRAY);
    int adc = -1, ami = aci >= 0 ? call_target(c, id, aci, rname, 0, &adc) : -1;
    if (ami >= 0 && adc == aci && c->scopes[ami].name && sp_streq(c->scopes[ami].name, rname)) {
      if (cplan_reopen_runs_builtin(c, id, ami)) return;
      cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT);
      return;
    }
  }
  /* last, a program's own Object method for a name the receiver's builtin
     class and Object's own surface do not have (object_reopen_answers) */
  int bcls = object_fallback_class_of_type(rt);
  if (rt == TY_BOOL || (bcls >= 0 && !builtin_method_known(core_class_name(bcls), name))) cplan_object_reopen(c, id, name, p);
}

static void cplan_resolve(Compiler *c, int id, CallPlan *p) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) cplan_resolve_super(c, id, p);
  else if (k == NK_CallNode) {
    cplan_resolve_call(c, id, p);
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
  const char *cn = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ? nt_str(nt, recv, "name") : NULL;
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
  if (p->by_name) {
    int ncc = 0;
    const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
    for (int i = 0; i < ncc; i++)
      if (ccs[i].mi == mi) return 1;
    return 0;
  }
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
    for (int i = 0; i < npc; i++)
      if (pcs[i].mi == mi || (pcs[i].mi < 0 && call_target(c, id, pcs[i].cls, name, 0, NULL) == mi))
        return 1;
    int ncc = 0;
    const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
    for (int i = 0; i < ncc; i++)
      if (ccs[i].mi == mi) return 1;
    return 0;
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
    if (sc && !comp_super_shadow(c, sc)) own = cplan_super_chain_mi(c, id, sc, ci, name);
  }
  if (own >= 0)
    fprintf(stderr, "plan-check: cplan-missing: %s node %d %s: chain %d\n", site, id, name, own);
}

/* the node is read as itself: nothing re-types or re-scopes it */
int cplan_plain_ctx(void) {
  return view_depth() == 0 && comp_scope_move_depth() == 0 && g_ie_class_id < 0 &&
         inline_splice_depth() == 0;
}

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
  p.method_node = mn;
  p.method_call = 1;
  return p;
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
    Scope *s = comp_scope_of(c, id);
    if (s && s->name && !comp_super_shadow(c, s))
      out[n++] = comp_super_name(c, comp_super_parent(c, s->class_id, s->is_cmethod), comp_scope_def_name(c, s), s->is_cmethod);
  }
  if (k == NK_CallNode && out[0] && is_method_object_maker(out[0])) {
    const char *sym = method_sym_arg(c, id);
    if (sym) out[n++] = sym;
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
   scope (rcls's copy of it, when a mixin copied it); for an alias, the
   version of its target live where the alias was made, in the class whose
   body made it. -1 for any other version (a define_method's, an
   attribute's) or an alias made where the record has no point. A chain of
   aliases ends: each step asks for the version live at an alias's point,
   one recorded ahead of that point, so the points only go back. */
static int ct_def_version(Compiler *c, int rcls, const char *name, int sg, const LrDef *d, int *dc) {
  int cls = rcls;
  while (d) {
    if (d->kind == LR_DEF) return comp_method_version(c, rcls, name, sg, d->node, dc);
    if (d->kind != LR_ALIAS || !d->target) return -1;
    int at = d->node;
    name = d->target;
    LoadPoint p = elab_point_of(c, at);
    if (p.region < 0) return -1;
    cls = c->node_cbody[at];
    if (elab_method_at(c, cls, name, sg, p, &d) != ELAB_YES || !d ||
        (d->kind != LR_DEF && d->kind != LR_ALIAS)) return -1;
    int mi = call_target(c, at, cls, name, CT_CAPTURE | (cls < 0 ? CT_TOP : sg ? CT_CMETHOD : 0), NULL);
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
static int ver_entry_ix(const Compiler *c, int guard, int rcls, int sg, const char *name, int ar) {
  if (!c->ver_ix_nb) return -1;
  for (int i = c->ver_ix[ver_key_hash(rcls, sg, name, ar) % c->ver_ix_nb]; i >= 0; i = c->ver_ix_next[i]) {
    const struct VerEntry *e = &c->ver_entries[i];
    if (e->guard == guard && e->cls == rcls && e->sg == sg && e->ar == ar && sp_streq(e->name, name)) return i;
  }
  return -1;
}
static int ct_entry_of(const Compiler *c, int guard, int rcls, int sg, const char *name, int ar) {
  int i = ver_entry_ix(c, guard, rcls, sg, name, ar);
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
/* The methods a guard entry chooses from for mi, a later prepend's copy
   (Scope.mixin_guarded): mi, each guarded copy it stands in front of, and
   the method behind the last */
static int guard_chain(Compiler *c, int mi, int *to, DispArm *arm, char sel[][32]) {
  int n = 0;
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
  return n;
}
/* A call that reaches mi, a copy a later prepend puts in front of a method,
   runs it once the prepend's statement has run and the method behind it
   until then: the guard entry of mi's class for the call's arguments
   (guard_entry_make), which the entries' build makes for every call of
   the name; mi when asked before the build */
static int ct_guard(Compiler *c, int id, int mi, unsigned flags) {
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, NULL, NULL);
  const Scope *m = &c->scopes[mi];
  const char *name = c->ver_built ? m->name : comp_scope_user_name(m);
  if (flags & CT_CAPTURE) {
    const LrDef *d = NULL;
    LoadPoint p = elab_point_of(c, id);
    if (p.region >= 0 && elab_method_at(c, m->class_id, name, m->is_cmethod, p, &d) == ELAB_YES && d)
      for (int i = 0; i < n; i++) if (c->scopes[to[i]].def_node == d->node) return to[i];
  }
  if (!c->ver_built) return mi;
  int e = ct_entry_of(c, 1, m->class_id, m->is_cmethod, name, ver_arity(c, id, to, n));
  return e >= 0 ? e : mi;   /* a later synthesized call uses the general table entry */
}
/* The version an entry's arm (or else arm) call is pinned to, -1 for any
   other node: the pins are kept in the order of their nodes (ver_pin) */
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
/* How many versions rcls's chain holds of name (two at most counted: 0, 1 or
   2 for more), and whether the first is a plain def, kept by (class, side,
   name) for the record as it stands (elab_stamp): every call
   asked of every class of a name asks it, and the answer is the chain's. */
struct VerChain { struct VerChainSlot { const char *name; int rcls, sg, n, def; } *slot; int cap, used; ElabStamp stamp; };
void ver_chain_free(Compiler *c) {
  if (!c->ver_chain) return;
  free(c->ver_chain->slot);
  free(c->ver_chain);
  c->ver_chain = NULL;
}
static unsigned ver_chain_hash(const char *name, int rcls, int sg) {
  return ver_mix(sp_strhash(name) + (unsigned)rcls * 0x9E3779B1u + (unsigned)sg * 0x85EBCA6Bu);
}
static int ver_chain_count(Compiler *c, int rcls, int sg, const char *name, int *first_def) {
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
      return m->slot[j].n;
    }
  ElabVersion one[2];
  int n = elab_chain_versions(c, rcls, sg, name, one, 2), def = n >= 1 && one[0].v->kind == LR_DEF;
  m->slot[j] = (struct VerChainSlot){ name, rcls, sg, n, def };
  m->used++;
  *first_def = def;
  return n;
}
/* The version of `name` call id runs, when the method has more than one
   (#9898): a call at a load point gets the version live there; any other
   (a load point in a body that runs again, a method body, a block kept to
   run later), the one version it can reach over the statements it may run
   in (elab_call_window, elab_method_span), or, where it can reach two or
   more, rcls's entry, which calls the one the run-time cells name; an
   alias that is the last version, the version it captured. mi, the
   table's answer (the last version), where the record cannot tell or no
   entry was made. *span (when asked): how many versions the window
   reaches (elab_method_span), 0 when no window was asked. */
static int ct_version_at(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi, int *def_class, int *span) {
  int sg = (flags & CT_CMETHOD) != 0, rcls = flags & CT_TOP ? -1 : cls, vi = -1, dc = -1, n = 0, first_def = 0;
  const LrDef *d = NULL;
  /* one def on rcls's whole chain: the table's (a name with versions on
     other chains only) */
  if (ver_chain_count(c, rcls, sg, name, &first_def) == 1 && first_def) {
    if (span) *span = 0;
    return mi;
  }
  LoadPoint p = elab_point_of(c, id);
  /* An unplaced capture chooses at construction through the existing
     entry, even when a later wrapper changes the call's live window. */
  if ((flags & CT_CAPTURE) && p.region < 0 &&
      (vi = ct_entry_of(c, 0, rcls, sg, name, ver_call_arity(c, id, rcls, sg, name, mi))) >= 0) {
    if (def_class && !(flags & CT_TOP)) *def_class = rcls;
    if (span) *span = 2;
    return vi;
  }
  ElabWindow w;
  int at = p.region >= 0 ? elab_method_at(c, rcls, name, sg, p, &d) : ELAB_UNKNOWN;
  if (at == ELAB_YES) vi = ct_def_version(c, rcls, name, sg, d, &dc);
  else if (at == ELAB_UNKNOWN && elab_call_window(c, id, &w) == ELAB_YES &&
           (n = elab_method_span(c, rcls, name, sg, &w, &d, 1)) == 1 && d)
    vi = ct_def_version(c, rcls, name, sg, d, &dc);
  else if (n >= 2 || n == ELAB_SPAN_ANY) {
    /* the entry, where the chain holds still over the window: asked of
       an entry that was made, or by the entries' build (span) */
    vi = ct_entry_of(c, 0, rcls, sg, name, ver_call_arity(c, id, rcls, sg, name, mi));
    n = (span || vi >= 0) && elab_chain_settled(c, rcls, sg, name, w.from) ? 2 : 0;
    if (!n) vi = -1;
    dc = rcls;
  }
  else n = 0;
  if (vi < 0 && elab_method_at(c, rcls, name, sg, p, &d) == ELAB_YES && d && d->kind == LR_ALIAS)
    vi = ct_def_version(c, rcls, name, sg, d, &dc);
  if (span) *span = n;
  /* only a version that is a function of its own: a replaced one
     (lr_mark_versions), an entry, or one no later scope of its name
     shadows; not one a scope no def made (a define_method's) passes over */
  if (vi < 0 || (!c->scopes[vi].replaced && !c->scopes[vi].entry && scope_is_shadowed(c, vi))) return mi;
  if (ct_top_version(c, vi, flags)) return -1;
  if (def_class && !(flags & CT_TOP)) *def_class = dc;
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
    if (!m->name || (m->id == id && m->cls == cls && m->flags == flags && (m->name == name || sp_streq(m->name, name)))) return m;
  }
}
/* ...asked in the record's frame for the call plan (elab_plan_enter): the
   version is decided from the facts the record had for the call when the
   plan first asked, not from a map rebuilt per ask */
static int ct_version(Compiler *c, int id, int cls, const char *name, unsigned flags, int mi, int *def_class, int *span) {
  int memo = !span && c->ver_built && !elab_check_on(c);
  const struct VerMemo *m = memo ? ver_memo_slot(c, id, cls, name, flags) : NULL;
  if (m && m->name) {
    if (def_class && m->dc != -2) *def_class = m->dc;
    return m->vi;
  }
  int framed = elab_plan_enter(c, id), dc = def_class ? *def_class : -1, now = dc, kept = -2;
  int vi = ct_version_at(c, id, cls, name, flags, mi, memo ? &kept : def_class ? &dc : NULL, span);
  if (memo) {   /* (the class it answers, -2 for the table's) */
    *ver_memo_slot(c, id, cls, name, flags) = (struct VerMemo){ name, id, cls, flags, vi, kept };
    c->ver_memo_n++;
    if (kept != -2) dc = kept;
  }
  if (framed) elab_plan_leave(c);
  if (framed && elab_check_on(c))   /* the map as it is, for check mode */
    elab_check_version(c, id, vi, dc, ct_version_at(c, id, cls, name, flags, mi, def_class ? &now : NULL, NULL), now);
  if (def_class) *def_class = dc;
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
static int ct_undefined_at(Compiler *c, int id, int cls, const char *name, unsigned flags) {
  if (!(flags & (CT_CMETHOD | CT_TOP)) || !elab_method_versions(c, name)) return 0;
  int framed = elab_plan_enter(c, id);
  int none = elab_call_method(c, id, flags & CT_TOP ? -1 : cls, name, (flags & CT_CMETHOD) != 0) == ELAB_NO;
  if (framed) elab_plan_leave(c);
  return none && !(nt_ref(c->nt, id, "receiver") >= 0 && !nt_int(c->nt, id, "self_call", 0) &&
                   builtin_self_method_known(name, cm_self_kind(c, flags & CT_TOP ? -1 : cls)));
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
  return flags & CT_CMETHOD ? comp_cmethod_in_chain(c, cls, name, def_class) : comp_method_in_chain(c, cls, name, def_class);
}
/* The method call id runs on candidate pc of its boxed dispatch: the
   table's, or for a name with versions the one call_target binds on pc's
   class (a version, an entry), which the dispatch's arm calls */
int cplan_cand_target(Compiler *c, int id, const char *name, const PolyCand *pc, unsigned flags) {
  int mi = pc->mi >= 0 && (c->scopes[pc->mi].versioned || c->scopes[pc->mi].mixin_guarded) ? call_target(c, id, pc->cls, name, flags, NULL) : -1;
  return mi >= 0 ? mi : pc->mi;
}
/* The method call id finds by name, call_target's start: the table's
   (ct_table), but main's own method ahead of Object's for a call on main
   (ct_on_main), as CRuby's lookup there; its versions are then main's
   chain's, which ends in Object's (*flags gains CT_CMETHOD) */
static int ct_lookup(Compiler *c, int id, int cls, const char *name, unsigned *flags, int *def_class) {
  int si = (*flags & CT_TOP) && id >= 0 ? comp_main_method(c, name) : -1;
  int mi = si >= 0 && ct_on_main(c, id, name) ? si : ct_table(c, cls, name, *flags, def_class);
  if ((*flags & CT_TOP) && mi >= 0 && c->scopes[mi].is_cmethod) *flags |= CT_CMETHOD;
  return mi;
}
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, int *def_class) {
  if (ct_send_shadow(c, id, name)) return -1;
  int dc0 = def_class ? *def_class : -1, mi = ct_lookup(c, id, cls, name, &flags, def_class), pin;
  if (mi < 0 || id < 0 || (!(flags & (CT_REACH | CT_CAPTURE)) && !ct_calls(c, id, name))) return mi;
  if ((c->scopes[mi].versioned || c->scopes[mi].mixin_guarded || c->scopes[mi].entry) && (pin = ct_pin_of(c, id)) >= 0) {   /* (an entry's arm: its method's own class) */
    if (ct_top_version(c, pin, flags)) return -1;
    if (def_class) *def_class = c->scopes[pin].class_id;
    return pin;
  }
  /* a call the record proves runs before any version of a class method or
     a top-level function is defined raises, whatever versions follow; the
     entry's else arm answers only a call that may run before them */
  if (ct_undefined_at(c, id, cls, name, flags)) {
    if (def_class) *def_class = dc0;
    return -1;
  }
  if (c->scopes[mi].versioned || (flags & CT_CAPTURE)) mi = ct_version(c, id, cls, name, flags, mi, def_class, NULL);
  /* An alias's record already selected its captured definition. */
  return mi >= 0 && c->scopes[mi].mixin_guarded && sp_streq(name, c->scopes[mi].name) ? ct_guard(c, id, mi, flags) : mi;
}

/* Is a call of nm one whose version call_target chooses: does the record
   hold more than one version of the name, or an alias, which names the
   version it captured? */
int ct_version_named(Compiler *c, const char *nm) {
  return nm && (elab_method_replaced(c, nm) || elab_method_versions(c, nm) > 1);
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
      if (m->npool + 2 > m->cpool && !(m->pool = realloc(m->pool, sizeof(int) * (size_t)(m->cpool = m->cpool ? 2 * m->cpool : 64)))) {
        fprintf(stderr, "spinel: out of memory\n"); exit(1);
      }
      m->pool[m->npool++] = k;
      m->pool[m->npool++] = sg;
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
    Scope *s = comp_scope_of(c, id);
    if (s && !comp_super_shadow(c, s)) {
      int par = comp_super_parent(c, s->class_id, s->is_cmethod);
      if (par >= 0) fn(c, id, nm, par, CT_REACH | (s->is_cmethod ? CT_CMETHOD : 0), arg);
    }
    return;
  }
  int r = nt_ref(nt, id, "receiver");
  NodeKind rk = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  int cref = rk == NK_ConstantReadNode || rk == NK_ConstantPathNode, rc = cref ? elab_class_ref_at(c, r) : -1;
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
    oc = -1;   /* (Object among them, where its chain holds two) */
    for (int i = c->ver_ix_nb ? c->ver_names[sp_strhash(nm) % c->ver_ix_nb] : -1;
         i >= 0; i = c->ver_names_next[i]) {   /* and the class of each later prepend's copy */
      const struct VerEntry *e = &c->ver_entries[i];
      if (e->guard && sp_streq(e->name, nm)) fn(c, id, nm, e->cls, e->sg ? CT_CMETHOD : 0, arg);
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
  int ci = elab_class_ref_at(c, recv);
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
  /* ...and no builtin answers: one on any self, a core class's own */
  const char *cn = ci >= 0 ? c->classes[ci].name : NULL;
  if (builtin_self_method_known(name, cm_self_kind(c, ci)) ||
      (cn && builtin_class_id(cn) && (builtin_cmethod_known(cn, name) || builtin_super_cmethod_known(cn, name))))
    return CM_NONE;
  if (anon) *anon = cm_self_name(c, ci, self_text, cap) ? -1 : ci;
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

/* The class a constructor call builds: a constant receiver's, or the class
   its method runs for, as inference types it (bare or `self.new` in a class
   method, `self.class.new` in an instance method). *own is set for the
   second kind, which a subclass receiving the method builds its own of. */
static int cplan_new_class(Compiler *c, int id, int *own) {
  const NodeTable *nt = c->nt;
  int r = nt_ref(nt, id, "receiver");
  NodeKind k = r >= 0 ? nt_kind(nt, r) : NK_NONE;
  *own = 0;
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) return const_node_class_index(c, r);
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

/* A constructor's result can be discarded, so resolve from its receiver
   after the user new plan has declined, rather than from its result type. */
int cplan_initialize(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_CallNode || !is_new_name(nt_str(nt, id, "name"))) return -1;
  int own, ci = cplan_new_class(c, id, &own);
  if (ci < 0 || cplan_user_fresh(c, id)->dispatch != CP_NONE) return -1;
  return comp_method_in_chain(c, ci, "initialize", NULL);
}

/* Every initialize a constructor call may run: cplan_initialize's, and for
   a `new` on the class its method runs for the initialize of each subclass
   too, which may be the one that receives the method. Answers the count. */
int cplan_initializers(Compiler *c, int id, int *out, int cap) {
  const NodeTable *nt = c->nt;
  if (id < 0 || nt_kind(nt, id) != NK_CallNode || !is_new_name(nt_str(nt, id, "name"))) return 0;
  int own, ci = cplan_new_class(c, id, &own), n = 0;
  if (ci < 0 || cplan_user_fresh(c, id)->dispatch != CP_NONE) return 0;
  for (int k = ci; k < c->nclasses && n < cap; k++) {
    if (k != ci && (!own || !is_descendant(c, k, ci))) continue;
    int mi = comp_method_in_chain(c, k, "initialize", NULL), dup = 0;
    for (int j = 0; j < n; j++) dup |= out[j] == mi;
    if (mi >= 0 && !dup) out[n++] = mi;
  }
  return n;
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
    int mi = pcs[i].mi;
    if (mi < 0 || c->classes[pcs[i].cls].is_native_class) continue;
    found = 1;
    if (c->scopes[mi].ret != TY_BOOL) return TY_POLY;
  }
  /* the builtin #=== beside them is a Proc's or a Method's call for those
     receivers, which answers any value (`->(x) { x * 128 } === 2` is 256) */
  if (found && sp_streq(name, "===")) return TY_POLY;
  return found ? TY_BOOL : TY_UNKNOWN;
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
  char exp[256];
  return poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, -1, exp, sizeof exp) >= 0;
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

/* Does call id's receiver, an object of a class of the program's, answer
   `name` with a member or method of its class's own (a Struct member named
   `methods`), so that the call is no list of methods? */
static int cplan_recv_own_member(Compiler *c, int id, const char *name) {
  int r = nt_ref(c->nt, id, "receiver");
  TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
  return ty_is_object(rt) && comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) != SP_MEMBER_NONE;
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
  if (is_raise_alias(name) && nt_int(nt, id, "cond_def_class_new", 0) && cplan_reachable(c, id))
    return "a def under a condition only the run time decides, in a Class.new, Module.new, "
           "Struct.new or Data.define block, is not supported: write the class with `class`; "
           "see docs/limitations.md";
  if (is_raise_alias(name) && nt_int(nt, id, "cond_def_override", 0) && cplan_reachable(c, id))
    return "a def under a condition only the run time decides, of a name another method answers "
           "(a builtin's, another def's or an accessor's), is not supported: which one answers when the branch did "
           "not run is not known at compile time; see docs/limitations.md";
  if ((nt_int(nt, id, "cond_def_reflect", 0) ||
       (nt_int(nt, id, "cond_def_reflect_list", 0) && !cplan_recv_own_member(c, id, name))) &&
      cplan_reachable(c, id))
    return "a method defined under a condition only the run time decides is not answered for by "
           "respond_to?, method_defined?, instance_methods and their kin (it would answer as if "
           "the branch had run); see docs/limitations.md";
  if (is_raise_alias(name) && nt_int(nt, id, "sclass_self", 0) && cplan_reachable(c, id))
    return "the ancestry and the method table of a singleton class are not modeled: its name, "
           "identity, instance_methods(false) and method_defined? are; see docs/limitations.md";
  if (is_raise_alias(name) && nt_int(nt, id, "class_new_superclass", 0) && cplan_reachable(c, id))
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
  if (is_raise_alias(name) && nt_int(nt, id, "struct_cond_dm", 0) && cplan_reachable(c, id))
    return "a define_method under a condition only the run time decides, in a Struct.new or "
           "Data.define block, is not supported: write the class with `class`; see docs/limitations.md";
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
    { "unicode_normalize",
      "String#unicode_normalize is not supported: Unicode normalization requires "
      "shipping the Unicode decomposition/composition tables, which spinel "
      "deliberately does not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
    { "unicode_normalize!",
      "String#unicode_normalize! is not supported: Unicode normalization requires "
      "shipping the Unicode decomposition/composition tables, which spinel "
      "deliberately does not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
    { "unicode_normalized?",
      "String#unicode_normalized? is not supported: answering it requires the "
      "Unicode decomposition/composition tables, which spinel deliberately does "
      "not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
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
       and is left to the normal path. */
    else if (sp_streq(rcn, "Class") && sp_streq(name, "new") &&
             (nt_ref(nt, id, "block") >= 0 || cplan_argc(nt, id) > 0))
      why = "Class.new(parent) is not supported by AOT compilation: the class "
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
  if (!why && rcn && (comp_class_index(c, rcn) >= 0 || sp_streq(rcn, "Object") ||
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
   it is reachable */
static int cplan_reachable(Compiler *c, int id) {
  if (nt_int(c->nt, id, "refusal_dead", 0)) return 0;
  int proc = nt_int(c->nt, id, "refusal_proc", -1);
  if (proc >= 0 && proc_literal_uncalled(c, proc)) return 0;
  int sc = c->nscope[id];
  return sc >= 0 && sc < c->nscopes && c->scopes[sc].reachable;
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
  a->vty = (unsigned char)vty; a->conv = (unsigned char)conv; a->def = -1;
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
    int defcls = -1;
    int mi = call_target(c, id, k, name, 0, &defcls);
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
      !cpoly_has_family(p, PB_ND_ROUND) && !cpoly_has_family(p, PB_ND_NUM))
    cpoly_trial(p, cap, PT_GENERIC_TAIL);
  /* the families that write the switch's `default:` themselves */
  static const int dflt[] = { PB_ND_GENERIC, PB_ND_FIRSTN, PB_ND_KEYS, PB_ND_MERGE, PB_ND_AREF2, PB_ND_AREF,
                              PB_PUSH, PB_INCLUDE_CASES, PB_PRED_N };
  int has_default = ps.cover || (ps.strftime && ps.ncand == 0);
  for (size_t i = 0; i < sizeof dflt / sizeof dflt[0]; i++) has_default |= cpoly_has_family(p, dflt[i]);
  if (!is_setter_val && !has_default) cpoly_trial(p, cap, PT_DEFAULT_N);
}

/* A String-typed slot may already hold the shared handle. The argument
   temp keeps that representation so boxed arms keep its identity too. */
TyKind cplan_poly_arg_storage(Compiler *c, int id) {
  Repr r = repr_of(c, id);
  if (r.strbuf_src == RS_SLOT_POLY) return TY_POLY;
  if (r.as_ty == TY_STRING && r.strbuf_src == RS_HANDLE) return TY_STRBUF;
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
    int defcls = -1;
    int mi = call_target(c, id, k, name, 0, &defcls);
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
  int obj_def = -1;
  int obj_mi = call_target(c, id, obj_cls, name, 0, &obj_def);
  int blk = nt_ref(c->nt, id, "block");
  int obj_pf = obj_mi >= 0 && obj_def == obj_cls && c->scopes[obj_mi].yields && blk >= 0
               ? scope_proc_form_of(c, obj_mi) : -1;
  if (obj_pf >= 0 && (c->scopes[obj_pf].nparams != 0 || c->scopes[obj_pf].rest_idx >= 0)) obj_pf = -1;
  if (obj_pf >= 0 && resolve_forwarded_block(c, blk) < 0) obj_pf = -1;
  char oexp[600];
  if (obj_pf >= 0) {
    Scope *ps = &c->scopes[obj_pf];
    cpoly_add(p, cap, PA_PROC_FORM, PA_KEY_DEFAULT, obj_mi, ps->ret,
              method_is_void(ps) ? PC_VOID : cpoly_conv(ret, ps->ret));
  }
  else if (obj_mi >= 0 && obj_def == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
           scope_has_callable_symbol(c, obj_mi)) {
    Scope *ms = &c->scopes[obj_mi];
    cpoly_add(p, cap, PA_USER, PA_KEY_DEFAULT, obj_mi, ms->ret,
              method_is_void(ms) ? PC_VOID : cpoly_conv(ret, ms->ret));
  }
  else if (obj_def == obj_cls && poly_arm_refuses_none(c, obj_mi, oexp, sizeof oexp))
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

/* Missing hooks are not dispatched yet. New visibility checks retain the
   old self-call behaviour when the existing method table defines a hook. */
int cplan_has_method_missing(Compiler *c, int id, int ci, int cmethod) {
  if (ci < 0 && !cmethod) {
    Scope *s = comp_scope_of(c, id);
    if (s && !s->is_cmethod) ci = s->class_id;
    if (ci < 0 && !self_is_main(c, id)) return 0;
  }
  if (ci >= 0 && (cmethod ? comp_cmethod_in_chain(c, ci, "method_missing", NULL)
                          : comp_method_in_chain(c, ci, "method_missing", NULL)) >= 0) return 1;
  if (!cmethod && ci >= 0 && class_is_blank_slate(c, ci)) return 0;
  /* Object's methods include top-level defs; main has no user class entry. */
  int obj = comp_core_class(c, CORE_OBJECT);
  return (obj >= 0 && comp_method_in_chain(c, obj, "method_missing", NULL) >= 0) ||
         comp_method_index(c, "method_missing") >= 0;
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
int cplan_cmethod_arm(Compiler *c, int call, int k, const char *nm, int *dc, int *pfk) {
  int mi = call_target(c, call, k, nm, CT_CMETHOD, dc);
  if (mi < 0) return -1;
  int pf = scope_proc_form_of(c, mi);
  if (pf >= 0 && !proc_form_live(c, pf)) pf = -1;
  if (!scope_has_callable_symbol(c, mi) && pf < 0) return -1;
  if (pfk) *pfk = pf;
  return mi;
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
  return &cp;
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
  if (cplan_nil_user_method(c, id, nm)) return CN_NONE;
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
   the calls keep the table's answer. */
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
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, int na, int els_to);
static int ver_entry_make(Compiler *c, int rcls, int sg, const char *name, int mi, int ar) {
  ElabVersion vs[VER_MAX_ARMS];
  DispArm arm[VER_MAX_ARMS];
  int to[VER_MAX_ARMS], na = 0, blk = c->scopes[mi].blk_param || c->scopes[mi].yields, dc, same = 1;
  int spliced = c->scopes[mi].yields;
  char sel[VER_MAX_ARMS][32];
  int nv = elab_chain_versions(c, rcls, sg, name, vs, VER_MAX_ARMS);
  for (int i = 0; i < nv; i++) {
    if (vs[i].v->kind == LR_REMOVE) continue;   /* its write passes the search on */
    int s = vs[i].v->kind == LR_UNDEF ? -1 : ct_def_version(c, rcls, name, sg, vs[i].v, &dc);
    if (s < 0 || c->scopes[s].is_transplanted_source || (!c->scopes[s].replaced && scope_is_shadowed(c, s)))
      return -1;
    blk |= c->scopes[s].blk_param || c->scopes[s].yields;
    spliced &= c->scopes[s].yields;
    ver_cell_name(vs[i].hist, sel[na]);
    arm[na] = (DispArm){ sel[na], vs[i].n, name, vs[i].hist + 1 };
    to[na++] = s;
    same &= ver_same_source(c->nt, c->scopes[s].def_node, c->scopes[mi].def_node);
  }
  if (nv < 2 || !na || same) return -1;
  return ver_entry_emit(c, rcls, sg, name, ar, blk, spliced, arm, to, na, mi);
}
/* The entry's def, its arms' calls pinned to to[] and its else arm's to
   els_to, beside els_to's def */
static int ver_entry_emit(Compiler *c, int rcls, int sg, const char *name, int ar, int blk, int spliced,
                          const DispArm *arm, const int *to, int na, int els_to) {
  NodeTable *nt = (NodeTable *)c->nt;
  /* The shared arms end in one fallback, not tests that repeat it. */
  while (na > 0 && to[na - 1] == els_to) na--;
  int els = disp_forward_call(nt, c->scopes[els_to].name, ar, blk), like = c->scopes[els_to].def_node;
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
  ver_pin(c, els, els_to);
  return e;
}
/* The guard entry of mi, a copy a later prepend puts in front of a method
   (Scope.mixin_guarded), with ar parameters: the same if-chain over the
   cells of the prepends in front of the method, nearest first, its else
   arm the method behind them all */
static int guard_entry_make(Compiler *c, int mi, int ar) {
  DispArm arm[VER_MAX_ARMS];
  char sel[VER_MAX_ARMS][32];
  int to[VER_MAX_ARMS + 1], n = guard_chain(c, mi, to, arm, sel), blk = 0, spliced = 1;
  const Scope *m = &c->scopes[mi];
  if (n < 2) return -1;
  for (int k = 0; k < n; k++) {
    blk |= c->scopes[to[k]].blk_param || c->scopes[to[k]].yields;
    spliced &= c->scopes[to[k]].yields;
  }
  /* Cloned forwarding bodies retain the hidden name even when the clone
     has no pin in the original entry's node index. */
  for (int k = 0; k < n - 1; k++) arm[k].callee = c->scopes[to[k]].name;
  return ver_entry_emit(c, m->class_id, m->is_cmethod, comp_scope_user_name(m), ar, blk, spliced, arm, to, n - 1, to[n - 1]);
}
int ver_entry_targets(const Compiler *c, int e, int *out, int *tests) {
  const NodeTable *nt = c->nt;
  int n = 0, at = ver_first_stmt(nt, nt_ref(nt, c->scopes[e].def_node, "body"));
  for (; at >= 0 && n <= VER_MAX_ARMS; at = nt_kind(nt, at) == NK_IfNode ? nt_ref(nt, at, "subsequent") : -1) {
    if (tests) tests[n] = nt_kind(nt, at) == NK_IfNode ? nt_ref(nt, at, "predicate") : -1;
    out[n++] = ct_pin_of(c, ver_first_stmt(nt, nt_ref(nt, at, "statements")));
  }
  return n;
}
typedef struct { int guard, cls, sg, mi, ar; const char *name; } VerNeed;
/* ...and, to tell the asks that repeat one, an open addressing table over them (ix holds an index into v, -1 free) */
typedef struct { VerNeed *v; int n; int *ix; unsigned nb; } VerNeeds;
static int ver_need_has(const Compiler *c, const VerNeeds *o, VerNeed w) {
  if (ver_entry_ix(c, w.guard, w.cls, w.sg, w.name, w.ar) >= 0) return 1;
  unsigned at = o->nb ? ver_key_hash(w.cls, w.sg, w.name, w.ar) & (o->nb - 1) : 0;
  for (; o->nb && o->ix[at] >= 0; at = (at + 1) & (o->nb - 1)) {
    const VerNeed *v = &o->v[o->ix[at]];
    if (v->guard == w.guard && v->cls == w.cls && v->sg == w.sg && v->ar == w.ar && sp_streq(v->name, w.name)) return 1;
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
      (p.mi = ct_lookup(c, id, k, nm, &fl, NULL)) >= 0 && !c->scopes[p.mi].mixin_guarded && c->scopes[p.mi].versioned) {
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
  int mi, n = 0, ar, rcls = flags & CT_TOP ? -1 : k, sg;
  /* (an entry arms two or more versions of the chain: with fewer, or a
     chain the record does not hold, no call is asked. A call on main may
     ask main's chain, which ends in Object's: ct_lookup. A later prepend's
     copy has its guard entries: ver_entries_build) */
  if (!(flags & CT_TOP)) {
    const VerPre *p = ver_pre(c, id, nm, k, flags);
    if (!p->ok) return;
    mi = p->mi; sg = p->sg; ar = p->ar;
  }
  else {
    if (elab_chain_versions(c, rcls, 1, nm, vs, VER_MAX_ARMS) < 2) return;
    if ((mi = ct_lookup(c, id, k, nm, &flags, NULL)) < 0 || c->scopes[mi].mixin_guarded || !c->scopes[mi].versioned) return;
    sg = (flags & CT_CMETHOD) != 0;
    ar = ver_call_arity(c, id, rcls, sg, nm, mi);
  }
  VerNeed w = { 0, rcls, sg, mi, ar, nm };
  if (ver_need_has(c, arg, w)) return;
  ct_version(c, id, k, nm, flags, mi, NULL, &n);
  if (n >= 2) ver_need_add(arg, w);
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
    if (e->scope < 0) continue;
    int dup = 0;
    for (int j = c->ver_names[m]; j >= 0 && !dup; j = c->ver_names_next[j]) {
      const struct VerEntry *v = &c->ver_entries[j];
      dup = sp_streq(v->name, e->name) && (!e->guard || (v->guard && v->cls == e->cls && v->sg == e->sg));
    }
    if (!dup) { c->ver_names_next[i] = c->ver_names[m]; c->ver_names[m] = i; }
  }
}
void ver_entries_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  const char *names[2];
  VerNeeds o = { NULL, 0, NULL, 0 };
  int *gc = NULL, ngc = 0;
  for (int s = 0; s < c->nscopes; s++) {   /* the later prepends' copies */
    if (!c->scopes[s].mixin_guarded) continue;
    const Scope *m = &c->scopes[s];
    const char *name = comp_scope_user_name(m);
    if ((m->is_cmethod ? comp_cmethod_in_class(c, m->class_id, name) : comp_method_in_class(c, m->class_id, name)) != s) continue;
    if (!(ngc & (ngc - 1)) && !(gc = realloc(gc, sizeof *gc * (size_t)(ngc ? 2 * ngc : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    gc[ngc++] = s;
    int to[VER_MAX_ARMS + 1];
    VerNeed w = { 1, c->scopes[s].class_id, c->scopes[s].is_cmethod, s,
                  ver_arity(c, -1, to, guard_chain(c, s, to, NULL, NULL)), name };
    if (!ver_need_has(c, &o, w)) ver_need_add(&o, w);
  }
  for (int id = 0, n0 = nt->count; id < n0; id++)
    for (int i = 0, nn = call_node_names(c, id, names); i < nn; i++) {
      if (!names[i]) continue;
      if (ct_version_named(c, names[i])) ct_version_classes(c, id, names[i], ver_need, &o);
      for (int j = 0; j < ngc; j++) {
        const Scope *m = &c->scopes[gc[j]];
        int to[VER_MAX_ARMS + 1];
        if (!sp_streq(comp_scope_user_name(m), names[i]) && !is_send_family(names[i])) continue;
        VerNeed w = { 1, m->class_id, m->is_cmethod, gc[j], ver_arity(c, id, to, guard_chain(c, gc[j], to, NULL, NULL)), comp_scope_user_name(m) };
        if (!ver_need_has(c, &o, w)) ver_need_add(&o, w);
      }
    }
  for (int i = 0; i < o.n; i++) {
    int e = o.v[i].guard ? guard_entry_make(c, o.v[i].mi, o.v[i].ar)
                         : ver_entry_make(c, o.v[i].cls, o.v[i].sg, o.v[i].name, o.v[i].mi, o.v[i].ar);
    if (!(c->nver_entries & (c->nver_entries - 1)) &&
        !(c->ver_entries = realloc(c->ver_entries, sizeof *c->ver_entries * (size_t)(c->nver_entries ? 2 * c->nver_entries : 1)))) {
      fprintf(stderr, "spinel: out of memory\n"); exit(1);
    }
    c->ver_entries[c->nver_entries++] = (struct VerEntry){ e >= 0 ? c->scopes[e].name : o.v[i].name, o.v[i].guard, o.v[i].cls, o.v[i].sg, o.v[i].ar, e };
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
    int e = ct_entry_of(c, 1, m->class_id, m->is_cmethod, comp_scope_user_name(m), ar);
    if (e < 0) continue;
    c->scopes[e].entry = ENTRY_TABLE;
    c->scopes[e].mixin_guarded = m->mixin_guarded;
    c->scopes[e].guard_behind = mi;
  }
  free(gc);
  c->ver_built = 1;
}
/* `cell = v` put in statement list `list` at its statement `at` (after it
   when after), in at's scope: the write, for the caller to record as the
   load record records a write (elab_record_write) */
int ver_cell_put(Compiler *c, const char *cell, int list, int at, int after, int v) {
  NodeTable *nt = (NodeTable *)c->nt;
  int n = 0, j = 0, line = (int)nt_int(nt, at, "node_line", 0);
  int tv = nt_new_int(nt, v), g = nt_new_node(nt, "GlobalVariableWriteNode");
  nt_node_set_str(nt, g, "name", cell);
  nt_node_set_ref(nt, g, "value", tv);
  nt_node_set_int(nt, tv, "node_line", line);
  nt_node_set_int(nt, g, "node_line", line);
  const int *st = nt_arr(nt, list, "body", &n);
  while (j < n && st[j] != at) j++;
  if (j == n) { fprintf(stderr, "spinel: internal: a cell write's statement is not in its statement list\n"); exit(1); }
  int *ns = malloc(sizeof(int) * (size_t)(n + 1));
  if (!ns) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  j += after;
  memcpy(ns, st, sizeof(int) * (size_t)j);
  ns[j] = g;
  memcpy(ns + j + 1, st + j, sizeof(int) * (size_t)(n - j));
  nt_node_set_arr(nt, list, "body", ns, n + 1);
  free(ns);
  /* (the cell holds an Integer from the entry file's first statement on, and nothing can read it before: its seed) */
  comp_gvar_intern(c, cell + 1)->gvar_seeded = 2;
  comp_grow_node_arrays(c);
  c->nscope[g] = c->nscope[tv] = c->nscope[at];
  return g;
}
/* ...and the cells' writes: after each statement that makes a version an
   entry arms (ahead of it when it is a def, an alias or an undef), the write of
   the version's number to its history's cell (0 for a remove_method's,
   which passes the search on), in the same statement list; and 0 ahead of
   the entry file's first statement (unless that statement makes one of the
   cell's versions), so that the cell holds an Integer wherever an entry
   reads it. Each recorded as the load record records a
   write (elab_record_write). */
typedef struct { int stmt, node, n, hist; } VerWrite;
void ver_cells_write(Compiler *c) {
  const NodeTable *nt = c->nt;
  ElabVersion vs[VER_MAX_ARMS];
  VerWrite *w = NULL;
  int nw = 0, nz = 0, top = prog_root_stmts(c, 0), ntop = 0;
  const int *par = c->nver_entries ? comp_parent_map(c) : NULL, *first = top >= 0 ? nt_arr(nt, top, "body", &ntop) : NULL;
  for (int i = 0; i < c->nver_entries; i++) {
    const struct VerEntry *e = &c->ver_entries[i];
    /* (not only the entries reached now: a send's arms, made in the fixpoint, reach methods whose calls bind entries this first pass leaves unreached) */
    if (e->scope < 0) continue;
    int arms[VER_MAX_ARMS + 1], tests[VER_MAX_ARMS + 1];
    int na = e->guard ? ver_entry_targets(c, e->scope, arms, tests) : 0;
    int nv = elab_chain_versions(c, e->cls, e->sg, e->name, vs, VER_MAX_ARMS);
    for (int j = 0; j < nv; j++) {
      if (e->guard) {
        int reads = 0;
        for (int k = 0; k < na; k++)
          reads |= nt_int(nt, tests[k], "ver_hist", 0) == vs[j].hist + 1;
        if (!reads) continue;   /* prepend cells are written at their statements */
      }
      int node = vs[j].v->node, st = node, dup = 0;
      while (st >= 0 && par[st] >= 0 && nt_kind(nt, par[st]) != NK_StatementsNode) st = par[st];
      for (int k = 0; k < nw && !dup; k++) dup = w[k].node == node;
      if (dup || st < 0 || par[st] < 0) continue;
      if (!(nw & (nw - 1)) && !(w = realloc(w, sizeof *w * (size_t)(nw ? 2 * nw : 1)))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      w[nw++] = (VerWrite){ st, node, vs[j].v->kind == LR_REMOVE ? 0 : vs[j].n, vs[j].hist };
    }
  }
  int *made = malloc(sizeof(int) * (size_t)(2 * nw + 1)), at0 = ntop ? first[0] : -1;
  for (int k = 0; k < nw; k++) {
    char cell[32];
    int z = at0 >= 0;
    ver_cell_name(w[k].hist, cell);
    for (int j = 0; j < nw && z; j++)   /* (one per cell; none where the first statement writes it) */
      z = w[j].hist != w[k].hist || (j >= k && w[j].stmt != at0);
    if (z) made[nw + nz++] = ver_cell_put(c, cell, top, at0, 0, 0);
    NodeKind sk = nt_kind(nt, w[k].stmt);
    /* (ahead of a def, an alias or an undef, which runs no program code: after one that ends a branch, the write would be the branch's value) */
    int after = sk != NK_DefNode && sk != NK_AliasMethodNode && sk != NK_UndefNode;
    made[k] = ver_cell_put(c, cell, par[w[k].stmt], w[k].stmt, after, w[k].n);
  }
  for (int k = 0; k < nw + nz; k++) elab_record_write(c, made[k]);
  free(made); free(w);
  /* (the versions kept before the writes are asked again) */
  free(c->ver_memo);
  c->ver_memo = NULL;
  c->ver_memo_cap = c->ver_memo_n = 0;
}

