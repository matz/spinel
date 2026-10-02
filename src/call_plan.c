/* call_plan.c -- the user method a call binds, resolved from the tables
   (see call_plan.h). */

#include <assert.h>
#include "codegen_internal.h"
#include "analyze_internal.h"
#include "call_plan.h"

static CallPlan *g_cp_memo = NULL;
static unsigned char *g_cp_have = NULL;
static int g_cp_cap = 0;

/* a descendant of cid with its own `name`: the dispatch is a switch */
static int cplan_overridden(Compiler *c, int cid, const char *name, int cmeth) {
  int nd = 0;
  const int *ds = comp_descendants(c, cid, &nd);
  for (int i = 0; i < nd; i++) {
    int k = ds[i];
    if (k == cid) continue;
    if ((cmeth ? comp_cmethod_in_class(c, k, name) : comp_method_in_class(c, k, name)) >= 0)
      return 1;
  }
  return 0;
}

static void cplan_set(CallPlan *p, int mi, int owner, int via, int dispatch) {
  p->mi = mi; p->owner_ci = (short)owner;
  p->via = (unsigned char)via; p->dispatch = (unsigned char)dispatch;
}

/* the class a builtin receiver kind is reopened as, or NULL */
static const char *cplan_reopen_class(TyKind rt) {
  switch (rt) {
  case TY_STRING: return "String";
  case TY_INT:    return "Integer";
  case TY_FLOAT:  return "Float";
  case TY_SYMBOL: return "Symbol";
  case TY_RANGE:  return "Range";
  case TY_TIME:   return "Time";
  case TY_THREAD: return "Thread";
  case TY_FIBER:  return "Fiber";
  case TY_CLASS:  return "Class";
  default:        return NULL;
  }
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
  if (par < 0) return;
  const char *uname = comp_super_name(c, par, s->name, s->is_cmethod);
  if (!uname) return;
  int mi = s->is_cmethod ? comp_cmethod_in_chain(c, par, uname, NULL)
                         : comp_method_in_chain(c, par, uname, NULL);
  if (mi >= 0) cplan_set(p, mi, par, UC_SUPER, CP_DIRECT);
}

static void cplan_resolve_call(Compiler *c, int id, CallPlan *p) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return;
  int recv = nt_ref(nt, id, "receiver");
  /* a singleton accessor folding to one constant: that class's method */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    int fmi = fold_ci >= 0 ? comp_cmethod_in_chain(c, fold_ci, name, NULL) : -1;
    if (fmi >= 0) { cplan_set(p, fmi, fold_ci, UC_CMETH, CP_DIRECT); return; }
  }
  /* a retargeted `x.send(:m)` reaching a top-level def the receiver does
     not own itself */
  if (recv >= 0 && nt_str(nt, id, "send_blind") && nt_ref(nt, id, "block") < 0) {
    int smi = comp_method_index(c, name);
    if (smi >= 0 && !c->scopes[smi].yields) {
      TyKind srt = comp_ntype(c, recv);
      int owns = ty_is_object(srt) &&
                 (comp_method_in_chain(c, ty_object_class(srt), name, NULL) >= 0 ||
                  comp_reader_in_chain(c, ty_object_class(srt), name, NULL));
      if (!owns) { cplan_set(p, smi, -1, UC_SEND_BLIND, CP_DIRECT); return; }
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
      if (imi >= 0) { cplan_set(p, imi, pk[i], UC_IE, CP_VIRTUAL); return; }
    }
    int cb = comp_cbody_call_mi(c, id, name);
    if (cb >= 0) { cplan_set(p, cb, c->node_cbody[id], UC_CMETH, CP_DIRECT); return; }
    int mi = comp_self_call_mi(c, id, name);
    if (mi >= 0) {
      Scope *m = &c->scopes[mi];
      if (m->class_id < 0) { cplan_set(p, mi, -1, UC_TOP, CP_DIRECT); return; }
      Scope *self = comp_scope_of(c, id);
      int scls = self ? self->class_id : m->class_id;
      int virt = scls >= 0 && cplan_overridden(c, scls, name, m->is_cmethod);
      cplan_set(p, mi, scls, m->is_cmethod ? UC_CMETH : UC_INST, virt ? CP_VIRTUAL : CP_DIRECT);
      return;
    }
    int imi = comp_included_method_index(c, name, id);
    if (imi >= 0) cplan_set(p, imi, c->scopes[imi].class_id, UC_INCLUDED, CP_DIRECT);
    return;
  }
  const char *rty = nt_type(nt, recv);
  if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    int mi = ci >= 0 ? comp_cmethod_in_chain(c, ci, name, NULL) : -1;
    if (mi >= 0) cplan_set(p, mi, ci, UC_CMETH, CP_DIRECT);
    return;
  }
  TyKind rt = comp_ntype(c, recv);
  if (ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0)
      cplan_set(p, mi, cid, UC_INST, cplan_overridden(c, cid, name, 0) ? CP_VIRTUAL : CP_DIRECT);
    return;
  }
  if (rt == TY_POLY) {
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    for (int i = 0; i < npc; i++) {
      if (c->classes[pcs[i].cls].is_native_class) continue;
      int pmi = pcs[i].mi >= 0 ? pcs[i].mi : comp_method_in_chain(c, pcs[i].cls, name, NULL);
      if (pmi >= 0) { cplan_set(p, pmi, pcs[i].cls, UC_POLY, CP_VIRTUAL); return; }
    }
    return;
  }
  if (nt_int(nt, id, "builtin_only", 0)) return;
  const char *rc = cplan_reopen_class(rt);
  if (rc) {
    int ci = comp_class_index(c, rc);
    int mi = ci >= 0 ? comp_method_in_chain(c, ci, name, NULL) : -1;
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
    int adc = -1, ami = aci >= 0 ? comp_method_in_chain(c, aci, name, &adc) : -1;
    if (ami >= 0 && adc == aci && c->scopes[ami].name && sp_streq(c->scopes[ami].name, name)) {
      cplan_set(p, ami, aci, UC_REOPEN, CP_DIRECT);
      return;
    }
  }
  /* last, a program's own Object method for a name the receiver's builtin
     class and Object's own surface do not have (object_reopen_answers) */
  const char *bcls = builtin_class_of_type(rt);
  if (bcls && !builtin_method_known(bcls, name) && !builtin_object_method_known(name)) {
    int oci = comp_class_index(c, "Object");
    int omi = oci >= 0 ? comp_method_in_chain(c, oci, name, NULL) : -1;
    if (omi >= 0) cplan_set(p, omi, oci, UC_REOPEN, CP_DIRECT);
  }
}

static void cplan_resolve(Compiler *c, int id, CallPlan *p) {
  cplan_set(p, -1, -1, UC_NONE, CP_NONE);
  NodeKind k = nt_kind(c->nt, id);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) cplan_resolve_super(c, id, p);
  else if (k == NK_CallNode) cplan_resolve_call(c, id, p);
}

int cplan_virtual_member(Compiler *c, int id, const CallPlan *p, int mi) {
  if (p->mi < 0 || mi < 0) return 0;
  if (p->mi == mi) return 1;
  if (p->dispatch != CP_VIRTUAL) return 0;
  const char *name = nt_str(c->nt, id, "name");
  if (!name) return 0;
  if (p->via == UC_POLY) {
    int npc = 0;
    const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
    for (int i = 0; i < npc; i++)
      if (pcs[i].mi == mi || (pcs[i].mi < 0 && comp_method_in_chain(c, pcs[i].cls, name, NULL) == mi))
        return 1;
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

/* the node is read as itself: nothing re-types or re-scopes it */
static int cplan_plain_ctx(void) {
  return view_depth() == 0 && comp_scope_move_depth() == 0 && g_ie_class_id < 0 &&
         inline_splice_depth() == 0;
}

const CallPlan *cplan_user(Compiler *c, int id) {
  static CallPlan fresh;
  if (id < 0 || id >= c->node_cap) { cplan_set(&fresh, -1, -1, UC_NONE, CP_NONE); return &fresh; }
  int plain = cplan_plain_ctx();
  if (plain && id < g_cp_cap && g_cp_have[id]) return &g_cp_memo[id];
#ifndef NDEBUG
  int tmp0 = g_tmp;
#endif
  cplan_resolve(c, id, &fresh);
#ifndef NDEBUG
  assert(g_tmp == tmp0);   /* resolving emits nothing */
#endif
  if (!plain) return &fresh;
  if (id >= g_cp_cap) {
    int ncap = g_cp_cap ? g_cp_cap : 1024;
    while (ncap <= id) ncap *= 2;
    g_cp_memo = realloc(g_cp_memo, (size_t)ncap * sizeof *g_cp_memo);
    g_cp_have = realloc(g_cp_have, (size_t)ncap);
    memset(g_cp_have + g_cp_cap, 0, (size_t)(ncap - g_cp_cap));
    g_cp_cap = ncap;
  }
  g_cp_memo[id] = fresh;
  g_cp_have[id] = 1;
  return &g_cp_memo[id];
}
