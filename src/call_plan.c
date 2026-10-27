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

int cplan_dispatch_form(Compiler *c, int cid, const char *name, int has_base) {
  if (cid < 0 || !name) return has_base ? CP_DIRECT : CP_NONE;
  int n = dispatch_impl_count(c, cid, name);
  if (!(n > 1 || (!has_base && n >= 1))) return has_base ? CP_DIRECT : CP_NONE;
  return dispatch_arms_disagree(c, cid, name) ? CP_PER_ARM : CP_SWITCH;
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

/* the class a builtin receiver kind is reopened as, or NULL */
static const char *cplan_reopen_class(TyKind rt) {
  switch (rt) {
  case TY_STRING: case TY_STRBUF: return "String";
  case TY_INT:    return "Integer";
  case TY_FLOAT:  return "Float";
  case TY_SYMBOL: return "Symbol";
  case TY_RANGE:  return "Range";
  case TY_TIME:   return "Time";
  case TY_THREAD: return "Thread";
  case TY_FIBER:  return "Fiber";
  case TY_ENUMERATOR: return "Enumerator";
  case TY_RANDOM: return "Random";
  case TY_CLASS:  return "Class";
  default:        return NULL;
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
  static const char *const CN[2] = { "TrueClass", "FalseClass" };
  for (int k = 0; k < 2; k++) {
    int ci = comp_class_index(c, CN[k]);
    int mi = ci >= 0 ? comp_method_in_chain(c, ci, name, NULL) : -1;
    if (mi >= 0 && c->scopes[mi].class_id == ci) *(k ? fmi : tmi) = mi;
  }
  return *tmi >= 0 || *fmi >= 0;
}

/* a program's own Object method, for a name Object's builtin surface does
   not answer itself */
static void cplan_object_reopen(Compiler *c, const char *name, CallPlan *p) {
  if (builtin_object_method_known(name)) return;
  int oci = comp_class_index(c, "Object");
  int omi = oci >= 0 ? comp_method_in_chain(c, oci, name, NULL) : -1;
  if (omi >= 0) cplan_set(p, omi, oci, UC_REOPEN, CP_DIRECT);
}

/* A method the program adds to Object under the name of a class-gated
   exception accessor (UncaughtThrowError#tag, KeyError#key, ...): every
   exception but those classes answers it, as CRuby's lookup reaches Object.
   Its scope, or -1 (none, or one taking arguments or a block). */
int cplan_exc_object_method(Compiler *c, const char *name) {
  if (!is_gated_exception_accessor(name)) return -1;
  int oc = comp_class_index(c, "Object");
  int mi = oc >= 0 ? comp_method_in_class(c, oc, name) : -1;
  if (mi < 0 || comp_method_vis_declared(c, oc, name, NULL) == SP_VIS_PRIVATE ||
      c->scopes[mi].nparams != 0 || c->scopes[mi].yields) return -1;
  return mi;
}
/* the reopenings of builtin exception classes that define name: the first
   stands for them, picked among at run time by the class name */
static void cplan_exc_reopen(Compiler *c, const char *name, CallPlan *p) {
  int xr[8];
  int xn = exc_reopen_definers(c, name, xr, 8);
  if (xn <= 0) return;
  int mi = comp_method_in_chain(c, xr[0], name, NULL);
  if (mi >= 0) cplan_set(p, mi, xr[0], UC_REOPEN, xn > 1 ? CP_SWITCH : CP_DIRECT);
}

/* A super's parent-chain target, past inherited copies of the same
   extended definition. Shared by resolution, inference, inline splicing and the miss check. */
int cplan_super_chain_mi(Compiler *c, Scope *s, int par, const char *name) {
  int mi = par < 0 ? -1 : s->is_cmethod ? comp_cmethod_in_chain(c, par, name, NULL)
                                       : comp_method_in_chain(c, par, name, NULL);
  /* An extended module's method is one entry in the class's ancestry, and
     `super` inside it goes past the module. Copied into each class that
     inherits the extend, the parent's copy is that same definition, not a
     superclass method: skip such copies (Thor's ClassMethods#method_added
     called on a Thor subclass reaches Module#method_added). */
  while (s->is_cmethod && mi >= 0 && c->scopes[mi].def_node == s->def_node && s->def_node >= 0) {
    int up = c->classes[c->scopes[mi].class_id].parent;
    mi = up >= 0 ? comp_cmethod_in_chain(c, up, name, NULL) : -1;
  }
  return mi;
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
  int mi = cplan_super_chain_mi(c, s, par, uname);
  if (mi >= 0) cplan_set(p, mi, par, UC_SUPER, CP_DIRECT);
  else if (s->is_cmethod &&
           builtin_super_cmethod_known(c->classes[s->class_id].name, uname) &&
           (par < 0 || comp_cmethod_in_chain(c, par, uname, NULL) < 0)) {
    cplan_set(p, -1, s->class_id, UC_SUPER, CP_REFUSE);
    p->rkind = CR_FEATURE; p->rfrom = CRF_LIMIT;
    p->msg = "super to an inherited builtin singleton method is not supported; see docs/limitations.md";
  }
}

/* Where node `target` sits under n: 0 not there, 1 outside any block,
   lambda or method body, 2 inside one (it may run later). */
static int cplan_node_place(const NodeTable *nt, int n, int target, int inner, int depth) {
  if (n < 0 || depth > 400) return 0;
  if (n == target) return inner ? 2 : 1;
  NodeKind k = nt_kind(nt, n);
  int in = inner || k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode;
  for (int i = 0; i < nt_num_refs(nt, n); i++) {
    int r = cplan_node_place(nt, nt_ref_at(nt, n, i), target, in, depth + 1);
    if (r) return r;
  }
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0; const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) {
      int r = cplan_node_place(nt, ids[j], target, in, depth + 1);
      if (r) return r;
    }
  }
  return 0;
}
/* Does call `id` run, at the program's top level, ahead of the statement
   that defines method mi? There the name is still the builtin's. The
   top-level statements are read in load order, each placed file's after the
   load that first runs it (prog_top_seq; the entry's alone under the inline
   model). */
static int cplan_called_before_def(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  int dn = c->scopes[mi].def_node, n = 0, r = 0;
  int *sv = dn >= 0 ? prog_top_seq(c, &n, NULL) : NULL;
  for (int i = 0; i < n; i++) {
    if (cplan_node_place(nt, sv[i], dn, 0, 0)) break;
    if (cplan_node_place(nt, sv[i], id, 0, 0) == 1) { r = 1; break; }
  }
  free(sv);
  return r;
}
/* --share-strings: does call `id`, with a literal block, still reach the
   builtin rather than String reopening mi's yielding method of its name,
   for being one the program runs ahead of mi's definition at its top level
   (as in CRuby)? The plan, the inference and the emitter ask it alike. */
int cplan_reopen_runs_builtin(Compiler *c, int id, int mi) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  return mi >= 0 && blk >= 0 && nt_kind(nt, blk) == NK_BlockNode && c->scopes[mi].yields &&
         c->scopes[mi].class_id == comp_class_index(c, "String") && cplan_called_before_def(c, id, mi);
}
/* The existing boxed dispatch candidates also serve literal-name send arms. */
static void cplan_resolve_poly(Compiler *c, const char *name, CallPlan *p,
                               const PolyCand *pcs, int npc) {
  for (int i = 0; i < npc; i++) {
    if (c->classes[pcs[i].cls].is_native_class) continue;
    /* a reader candidate written as a method (`def x = @x`) is that method */
    int pmi = pcs[i].mi >= 0 ? pcs[i].mi : comp_method_in_chain(c, pcs[i].cls, name, NULL);
    if (pmi >= 0) { cplan_set(p, pmi, pcs[i].cls, UC_POLY, CP_SWITCH); return; }
  }
  /* a boxed class value: the dispatch's class-method arms */
  int ncc = 0;
  const PolyCand *ccs = comp_cmethod_candidates(c, name, &ncc);
  for (int i = 0; i < ncc; i++)
    if (ccs[i].mi >= 0) { cplan_set(p, ccs[i].mi, ccs[i].cls, UC_POLY, CP_SWITCH); return; }
  return;
}

/* A missing base implementation still dispatches to the existing descendants. */
static void cplan_descendant_call(Compiler *c, int cid, const char *name, int cmeth, CallPlan *p) {
  int nd = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  int reader = !cmeth && comp_reader_in_chain(c, cid, name, NULL);
  for (int i = 0; i < nd; i++) {
    int mi = reader ? comp_method_in_chain(c, ds[i], name, NULL) : cmeth ? comp_cmethod_in_class(c, ds[i], name)
                   : comp_method_in_class(c, ds[i], name);
    if (mi < 0) continue;
    cplan_set(p, mi, cid, cmeth ? UC_CMETH : UC_INST,
              cmeth || reader ? CP_SWITCH : cplan_dispatch_form(c, cid, name, 0));
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
  if (ci >= 0 && comp_cmethod_in_chain(c, ci, name, NULL) >= 0) return 0;
  int n = comp_sg_reader_candidates(c, recv, out, cap);
  if (n >= 2)
    for (int i = 0; i < n; i++)
      if (comp_cmethod_in_chain(c, out[i], name, NULL) >= 0) return n;
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
    int fmi = fold_ci >= 0 ? comp_cmethod_in_chain(c, fold_ci, name, NULL) : -1;
    if (fmi >= 0) { cplan_set(p, fmi, fold_ci, UC_CMETH, CP_DIRECT); return; }
    int cand[32];
    int ncand = cplan_accessor_classes(c, id, cand, 32);
    if (ncand >= 2) {
      for (int i = 0; i < ncand; i++) {
        int mi = comp_cmethod_in_chain(c, cand[i], name, NULL);
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
    int smi = comp_method_index(c, name);
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
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) { cplan_set(p, imi, iec, UC_IE, CP_DIRECT); return; }
    }
    /* ...or one of several, by the receiver's runtime class */
    int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    for (int i = 0; i < npk; i++) {
      int imi = comp_method_in_chain(c, pk[i], name, NULL);
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
               : scls >= 0 ? cplan_dispatch_form(c, scls, name, 1) : CP_DIRECT;
      cplan_set(p, mi, scls, m->is_cmethod ? UC_CMETH : UC_INST, form);
      /* an instance method comp_self_call_mi took from the self class's chain */
      p->chain = !m->is_cmethod && self && self->class_id >= 0;
      return;
    }
    int imi = comp_included_method_index(c, name, id);
    if (imi >= 0) { cplan_set(p, imi, c->scopes[imi].class_id, UC_INCLUDED, CP_DIRECT); return; }
    /* outside a class method, self reaches a program's own Object method */
    Scope *self = comp_scope_of(c, id);
    if (!(self && self->is_cmethod)) cplan_object_reopen(c, name, p);
    if (p->mi < 0 && self && self->class_id >= 0)
      cplan_descendant_call(c, self->class_id, name, self->is_cmethod, p);
    return;
  }
  NodeKind rk = nt_kind(nt, recv);
  TyKind rt = comp_ntype(c, recv);
  /* a Class reopen's own method answers a class value first, as the
     inference arm for Range / Time / IO / Class receivers does */
  if (rt == TY_CLASS && !nt_int(nt, id, "builtin_only", 0)) {
    int kci = comp_class_index(c, "Class");
    int kmi = kci >= 0 ? comp_method_in_chain(c, kci, name, NULL) : -1;
    if (kmi >= 0) { cplan_set(p, kmi, kci, UC_REOPEN, CP_DIRECT); return; }
  }
  int sci = self_class_static_ci(c, recv);   /* `self.class` naming one class */
  if (sci >= 0 || (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode)) {
    int ci = sci >= 0 ? sci : comp_class_index(c, nt_str(nt, recv, "name"));
    int mi = ci >= 0 ? comp_cmethod_in_chain(c, ci, name, NULL) : -1;
    /* a package's native_func, which the constant calls in C directly; its
       def is for the module reached as a value */
    if (sci < 0 && comp_scope_native_twin(c, mi)) return;
    if (mi >= 0) { cplan_set(p, mi, ci, UC_CMETH, CP_DIRECT); return; }
    /* a method the program adds to Class, on a builtin class constant */
    int rmi = class_reopen_cmethod(c, recv, name);
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
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      cplan_set(p, mi, cid, UC_INST, cplan_dispatch_form(c, cid, name, 1));
      p->chain = 1;
      return;
    }
    /* the operators an object answers through another of its methods:
       `!=` through `==`, the comparisons through `<=>` when Comparable
       supplies them (class_is_comparable). Not one a subclass defines
       itself: the call dispatches at run time (#4023). */
    const char *via_op = comp_subclass_answers(c, cid, name) ? NULL : builtin_operator_delegate(name);
    if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
    int dmi = via_op ? comp_method_in_chain(c, cid, via_op, NULL) : -1;
    if (dmi < 0 && via_op) {
      via_op = builtin_operator_delegate(via_op);
      if (via_op && sp_streq(via_op, "<=>") && !class_is_comparable(c, cid)) via_op = NULL;
      dmi = via_op ? comp_method_in_chain(c, cid, via_op, NULL) : -1;
    }
    if (dmi >= 0) {
      cplan_set(p, dmi, cid, UC_INST, cplan_dispatch_form(c, cid, via_op, 1));
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
    if (class_is_exc_subclass(c, cid)) cplan_exc_reopen(c, name, p);
    if (p->mi < 0) cplan_object_reopen(c, name, p);
    if (p->mi < 0 && !comp_reader_in_chain(c, cid, name, NULL) && !c->classes[cid].is_native_class)
      cplan_descendant_call(c, cid, name, 0, p);
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
    cplan_resolve_poly(c, name, p, pcs, npc);
    return;
  }
  /* a reopening of the exception classes that defines the name stands for
     the call; otherwise the call goes on to the Object fallback below */
  if (rt == TY_EXCEPTION) { cplan_exc_reopen(c, name, p); if (p->mi >= 0) return; }
  if (nt_int(nt, id, "builtin_only", 0)) return;
  /* a call written under a builtin alias (detect, collect_concat) that the
     desugar renamed to the builtin's own name reaches a reopen's method of
     the name it was written with, not of the new name */
  const char *rname = nt_str(nt, id, "orig_name") ? nt_str(nt, id, "orig_name") : name;
  /* an IO handle's reopen: the File, IO or socket class that defines it */
  if (rt == TY_IO) {
    int eci = io_reopen_class(c, name);
    int emi = eci >= 0 ? comp_method_in_chain(c, eci, name, NULL) : -1;
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
  const char *rc = cplan_reopen_class(rt);
  if (rc) {
    int ci = comp_class_index(c, rc);
    int mi = ci >= 0 ? comp_method_in_chain(c, ci, rname, NULL) : -1;
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
      int ami = aci >= 0 ? comp_method_in_chain(c, aci, name, NULL) : -1;
      if (ami >= 0 && c->scopes[ami].class_id == aci) { cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT); return; }
    }
  }
  /* an Array or Hash reopen's own method of the name */
  if ((ty_is_array(rt) || ty_is_obj_array(rt) || ty_is_hash(rt)) && nt_ref(nt, id, "block") < 0) {
    int aci = comp_class_index(c, ty_is_hash(rt) ? "Hash" : "Array");
    int adc = -1, ami = aci >= 0 ? comp_method_in_chain(c, aci, rname, &adc) : -1;
    if (ami >= 0 && adc == aci && c->scopes[ami].name && sp_streq(c->scopes[ami].name, rname)) {
      cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT);
      return;
    }
  }
  /* last, a program's own Object method for a name the receiver's builtin
     class and Object's own surface do not have (object_reopen_answers) */
  const char *bcls = object_fallback_class_of_type(rt);
  if (rt == TY_BOOL || (bcls && !builtin_method_known(bcls, name))) cplan_object_reopen(c, name, p);
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
      int mi = comp_method_index(c, name);
      if (mi >= 0 && !c->scopes[mi].yields) p->send_fallback = mi;
    }
  }
}

/* A constant call's user singleton target, shared by inference, rewrites
   and builtin emitters. Unregistered constants need no target lookup.
   Ordinary user classes never need this precedence decision; native package
   bindings supply their own builtin names. The caller supplies the constant
   receiver name, or NULL for any other receiver. */
const CallPlan *cplan_const_user(Compiler *c, int id, const char *cn, int fresh) {
  if (!cn || c->nclasses == 0) return NULL;
  const NodeTable *nt = c->nt;
  int ci = comp_class_index(c, cn);
  if (ci < 0) return NULL;
  if (!c->classes[ci].is_native_class && !c->classes[ci].is_builtin_const &&
      (c->n_native_funcs == 0 || comp_native_find(c, cn, nt_str(nt, id, "name")) < 0) &&
      (c->n_ffi_funcs == 0 || ffi_find_func(c, cn, nt_str(nt, id, "name")) < 0)) return NULL;
  if (nt_int(nt, id, "builtin_only", 0)) return NULL;
  const CallPlan *p = fresh ? cplan_user_fresh(c, id) : cplan_user(c, id);
  return p->mi >= 0 && p->via == UC_CMETH && p->dispatch == CP_DIRECT ? p : NULL;
}

int cplan_virtual_member(Compiler *c, int id, const CallPlan *p, int mi) {
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
        if (comp_cmethod_in_chain(c, cand[i], name, NULL) == mi) return 1;
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
    if (cplan_native_block_possible(c, id, pcs, npc)) {
      const PolyPlan *bp = cplan_poly_block(c, id);
      for (int i = 0; i < bp->n; i++)
        if (bp->arm[i].mi == mi || scope_proc_form_of(c, bp->arm[i].mi) == mi) return 1;
    }
    for (int i = 0; i < npc; i++)
      if (pcs[i].mi == mi || (pcs[i].mi < 0 && comp_method_in_chain(c, pcs[i].cls, name, NULL) == mi))
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
        if (comp_method_in_chain(c, ks[i], name, NULL) == mi) return 1;
    }
    int xr[8], xn = exc_reopen_definers(c, name, xr, 8);
    for (int i = 0; i < xn; i++)
      if (comp_method_in_chain(c, xr[i], name, NULL) == mi) return 1;
    return 0;
  }
  if (p->via == UC_IE) {
    int pk[64], npk = ie_poly_classes_at(c, id, pk, 64);
    for (int i = 0; i < npk; i++)
      if (comp_method_in_chain(c, pk[i], name, NULL) == mi) return 1;
    return 0;
  }
  /* an override in a descendant of the class the chain was searched from */
  int cmeth = c->scopes[p->mi].is_cmethod;
  int nd = 0;
  const int *ds = p->owner_ci >= 0 ? comp_descendants(c, p->owner_ci, &nd) : NULL;
  for (int i = 0; i < nd; i++)
    if ((cmeth ? comp_cmethod_in_class(c, ds[i], name) : comp_method_in_class(c, ds[i], name)) == mi)
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

/* A plan-only reader must not lose a user target. This assertion belongs
   with resolution, and never supplies an emission target on a miss. */
void cplan_check_missing(Compiler *c, int id, int mi, int ci, const char *name, int cmeth, const char *site) {
  if (!g_plan_check || mi >= 0 || ci < 0 || !name || nt_int(c->nt, id, "builtin_only", 0)) return;
  int own = cmeth ? comp_cmethod_in_chain(c, ci, name, NULL) : comp_method_in_chain(c, ci, name, NULL);
  NodeKind k = nt_kind(c->nt, id);
  if (cmeth && (k == NK_SuperNode || k == NK_ForwardingSuperNode)) {
    Scope *sc = comp_scope_of(c, id);
    if (sc && !comp_super_shadow(c, sc)) own = cplan_super_chain_mi(c, sc, ci, name);
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
   the Method adapter, not by the generic call-target argument walk. */
CallPlan cplan_method_call(Compiler *c, int id) {
  CallPlan p;
  cplan_set(&p, -1, -1, UC_NONE, CP_NONE);
  int recv = nt_ref(c->nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_METHOD || !(is_method_invoke(nt_str(c->nt, id, "name")) || is_bind_call(nt_str(c->nt, id, "name")))) return p;
  an_pure_read_begin();
  int mn = method_recv_node(c, recv);
  int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
  an_pure_read_end();
  if (mi >= 0) cplan_set(&p, mi, c->scopes[mi].class_id, c->scopes[mi].is_cmethod ? UC_CMETH : UC_INST, CP_DIRECT);
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
    cplan_resolve_poly(c, name, &p, pcs, npc);
  }
  return p;
}

CallPlan cplan_conversion(Compiler *c, int ci, const char *name) {
  CallPlan p;
  int mi = comp_method_in_chain(c, ci, name, NULL);
  cplan_set(&p, mi, ci, UC_INST, cplan_dispatch_form(c, ci, name, mi >= 0));
  p.chain = 1;
  p.delegate = name;
  return p;
}

CallPlan cplan_exception_render(Compiler *c, int id, int xcm) {
  const char *name = nt_str(c->nt, id, "name");
  int own = is_message_name(name) ? -1 : comp_method_in_chain(c, xcm, name, NULL);
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
int call_target(Compiler *c, int id, int cls, const char *name, unsigned flags, int *def_class) {
  (void)id;
  if (flags & CT_TOP) return comp_method_index(c, name);
  if (flags & CT_OWN) {
    int mi = flags & CT_CMETHOD ? comp_cmethod_in_class(c, cls, name) : comp_method_in_class(c, cls, name);
    if (mi >= 0 && def_class) *def_class = cls;
    return mi;
  }
  return flags & CT_CMETHOD ? comp_cmethod_in_chain(c, cls, name, def_class) : comp_method_in_chain(c, cls, name, def_class);
}

const CallPlan *cplan_user_in(Compiler *c, int id, int self_ci, int flags) {
  static CallPlan ctx;
  if (self_ci < 0) return cplan_user(c, id);
  cplan_set(&ctx, -1, -1, UC_NONE, CP_NONE);
  if (id < 0 || id >= c->node_cap || self_ci >= c->nclasses || nt_kind(c->nt, id) != NK_CallNode)
    return &ctx;
  if ((flags & CPX_TARGET) && nt_int(c->nt, id, "builtin_only", 0)) return &ctx;
  const char *name = flags & CPX_TARGET ? view_call_name(c, id) : nt_str(c->nt, id, "name");
  int mi = name ? comp_method_in_chain(c, self_ci, name, NULL) : -1;
  if (mi >= 0) {
    int form = flags & CPX_TARGET ? CP_DIRECT : cplan_dispatch_form(c, self_ci, name, 1);
    cplan_set(&ctx, mi, self_ci, UC_INST, form);
    ctx.chain = 1;
    return cplan_dep(&ctx);
  }
  if (flags & CPX_TARGET) return &ctx;
  /* a miss is no method, but a dispatch on the class still has a form: a
     method only descendants define takes the switch */
  if (!(flags & CPX_IE) && name) {
    cplan_set(&ctx, -1, self_ci, UC_INST, cplan_dispatch_form(c, self_ci, name, 0));
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
  if (k == NK_ConstantReadNode || k == NK_ConstantPathNode) return comp_class_index(c, nt_str(nt, r, "name"));
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

static const Compiler *g_ct_sort_c;
static int ct_idx_cmp(const void *a, const void *b) {
  const char *x = g_ct_sort_c->scopes[*(const int *)a].name;
  const char *y = g_ct_sort_c->scopes[*(const int *)b].name;
  int d = strcmp(x, y);
  return d ? d : *(const int *)a - *(const int *)b;
}

void cplan_targets_drop(void) {
  for (int i = 0; i < g_ct_cap; i++) g_ct_n[i] = CT_NONE;
  g_ct_npool = 0;
  g_ct_nidx = -1;
}

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
  if (!sc || sc->is_cmethod || cid < 0 || cid >= c->nclasses || comp_method_in_chain(c, cid, name, NULL) >= 0)
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
    int mi = comp_method_in_chain(c, k, name, NULL), seen = 0;
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
  int lo = 0, hi = g_ct_nidx;
  g_ct_sort_c = c;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(c->scopes[g_ct_idx[mid]].name, name) < 0) lo = mid + 1;
    else hi = mid;
  }
  for (int i = lo; i < g_ct_nidx && sp_streq(c->scopes[g_ct_idx[i]].name, name); i++) {
    int k = g_ct_idx[i];
    if (k == plan.mi || !cplan_virtual_member(c, id, &plan, k)) continue;
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
      if (seen || !cplan_virtual_member(c, id, &plan, k)) continue;
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
    if (fresh) { g_ct_npool = 0; g_ct_nidx = -1; }
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
    int ci = k == NK_ConstantReadNode || k == NK_ConstantPathNode ? comp_class_index(c, nt_str(nt, av[i], "name")) : -1;
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
        comp_method_in_chain(c, k, name, NULL) >= 0) return 1;
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
               ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
    return ci >= 0 && comp_cmethod_in_chain(c, ci, name, NULL) >= 0;
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
           "a constant, a call's result) is not supported: the defined method's body is known only at run "
           "time. Pass a block literal to define_method instead";
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
    if (comp_method_index(c, name) >= 0) return 1;
    if (k < 0) return 0;
    /* in a class body or a class method self is the class */
    int on_class = (s && s->is_cmethod) || (c->node_cbody && id < c->node_cap && c->node_cbody[id] >= 0 && s && !s->name);
    return on_class ? comp_cmethod_in_chain(c, k, name, NULL) >= 0 : comp_method_in_chain(c, k, name, NULL) >= 0;
  }
  TyKind rt = comp_ntype(c, r);
  if (ty_is_object(rt)) return comp_method_in_chain(c, ty_object_class(rt), name, NULL) >= 0;
  if (nt_kind(nt, r) == NK_ConstantReadNode) {
    int k = comp_class_index(c, nt_str(nt, r, "name"));
    return k >= 0 && comp_cmethod_in_chain(c, k, name, NULL) >= 0;
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
  if (ci < 0 || comp_method_in_chain(c, ci, name, NULL) < 0) return 0;
  int ks[16], n = io_reopen_defs(c, name, 0, ks, 16);
  for (int i = 0; i < n; i++) {
    Scope *km = &c->scopes[comp_method_in_chain(c, ks[i], name, NULL)];
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
               comp_prep_user_name(s->name), sp->msg);
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
  int cap = 0;
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
        TyKind mret = native_spec_to_ty(c->native_methods[nmi].ret);
        cpoly_add(p, &cap, PA_NATIVE, k, -1, mret, mret == TY_NIL ? PC_VOID : cpoly_conv(ret, mret));
      }
      /* one that needs arguments is CRuby's ArgumentError for this call */
      else if (nmi >= 0 && c->native_methods[nmi].nargs > 0 && c->classes[k].instantiated)
        cpoly_add(p, &cap, PA_ARITY, k, -1, TY_UNKNOWN, PC_VOID);
      continue;
    }
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, name, &defcls);
    /* an exception class with no method of its own for the name takes
       the builtin reopening's (`class Exception; def brief`) */
    if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
      int xd = exc_arm_definer(c, k, name);
      if (xd >= 0) { defcls = xd; mi = comp_method_in_chain(c, xd, name, NULL); }
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
      int slot_oint = is_scalar_ret(ret) && oint_kind(ret) && node_is_oint(c, id);
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
    else if (splat_a < 0 && poly_num_arm(name, argc) >= 0) cpoly_family(p, cap, PB_ND_NUM);
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
  p->flags = PPF_ROOT | (poly_key_cls0(c, name, argc, kwh, pos_argc, splat_a) ? PPF_KEY_CLS0 : 0) |
             (poly_key_prim(c, name, argc, kwh, pos_argc, splat_a) ? PPF_KEY_PRIM : 0) |
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
    int mi = comp_method_in_chain(c, k, name, &defcls);
    if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
      int xd = exc_arm_definer(c, k, name);
      if (xd >= 0) { defcls = xd; mi = comp_method_in_chain(c, xd, name, NULL); }
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
  int obj_cls = comp_class_index(c, "Object");
  if (obj_cls < 0) return;
  int obj_def = -1;
  int obj_mi = comp_method_in_chain(c, obj_cls, name, &obj_def);
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
  int sci = comp_class_index(c, "String");
  if (sci >= 0 && comp_method_in_chain(c, sci, name, NULL) >= 0) return;   /* the reopen answers */
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
             (poly_key_cls0(c, name, 0, -1, 0, -1) ? PPF_KEY_CLS0 : 0) |
             (poly_key_prim(c, name, 0, -1, 0, -1) ? PPF_KEY_PRIM : 0) |
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

const PolyPlan *cplan_poly_block(Compiler *c, int id) {
  static PolyPlan bp;
  free(bp.arm);
  bp.arm = NULL; bp.n = 0; bp.flags = 0; bp.native_block = 0;
  bp.ntype = bp.ret = comp_ntype(c, id);
  int cap = 0;
  int cand[64];
  int nc = poly_block_dispatch_cands(c, id, cand, 64);
  const char *name = nt_str(c->nt, id, "name");
  for (int i = 0; i < nc; i++) {
    cpoly_add(&bp, &cap, PA_USER, cand[i], comp_method_in_chain(c, cand[i], name, NULL), TY_UNKNOWN, PC_VOID);
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
  int obj = comp_class_index(c, "Object");
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
   class-method dispatch (cplan_poly_cmeth, and emit_unresolved_call's), or -1
   when it has no arm there. A yielding class
   method has no symbol of its own; its proc-form clone is the arm, taking
   the block as a proc (#3399), through *pfk. */
int cplan_cmethod_arm(Compiler *c, int k, const char *nm, int *dc, int *pfk) {
  int mi = comp_cmethod_in_chain(c, k, nm, dc);
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
    if (cls_sg_accessor_first(c, k, name, argc) || !cplan_cmethod_visible(c, id, k, name)) continue;
    int pf = -1;
    int mi = cplan_cmethod_arm(c, k, name, NULL, &pf);
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
    if (anc && (comp_method_in_chain(c, k, name, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL))) {
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
   method, which CRuby names as such)? */
static int cplan_nil_user_method(Compiler *c, const char *name) {
  static const char *const owners[] = { "NilClass", "Object", "Kernel", "BasicObject", NULL };
  if (comp_method_index(c, name) >= 0) return 1;
  for (int i = 0; owners[i]; i++) {
    int k = comp_class_index(c, owners[i]);
    if (k >= 0 && comp_method_in_chain(c, k, name, NULL) >= 0) return 1;
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
  if (cplan_nil_user_method(c, nm)) return CN_NONE;
  /* an arm of a lowered `send` on a user object, for a name its class does
     not define: the arm reaches a Kernel method, which nil answers through
     send as any object does (`nil.send(:Integer, "12")`) */
  if (nt_int(nt, id, "dyn_arm", 0) && ty_is_object(rt)) {
    int k = ty_object_class(rt);
    if (comp_method_in_chain(c, k, nm, NULL) < 0 && !comp_reader_in_chain(c, k, nm, NULL)) return CN_NONE;
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
