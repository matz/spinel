/* Remaining String return-origin queries (#11391). */

#include "codegen_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "repr.h"
#include "share.h"
#include "value_pos.h"

static int share_plan_method_reached(Compiler *c, int mi, const char *name) {
  for (int k = 0; k < c->nclasses; k++)
    if ((c->classes[k].instantiated || c->classes[k].is_builtin_const) &&
        comp_name_responder(c, k, name, 0) == mi) return 1;
  return 0;
}
/* Is call u an exception's message or to_s, whose targets the plan lists
   as the overrides of the exception classes it may reach
   (ct_exception_text)? */
static int share_plan_exc_text_call(Compiler *c, int u) {
  const char *nm = nt_str(c->nt, u, "name");
  int recv = nt_ref(c->nt, u, "receiver");
  if (!nm || !is_exception_message(nm) || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int cid = ty_is_object(rt) ? ty_object_class(rt) : -1;
  return rt == TY_EXCEPTION || (cid >= 0 && class_is_exc_subclass(c, cid));
}
/* Does each class or value of class cid or below it that the program has
   (made by `new`, or a builtin's) answer `name` by a method of the
   program's own? One whose chain has none answers by a builtin method or
   raises, which no target the plan lists covers. */
static int share_plan_below_has(Compiler *c, int cid, const char *name) {
  for (int k = 0; k < c->nclasses; k++) {
    if (k != cid && !is_descendant(c, k, cid)) continue;
    if (!c->classes[k].instantiated && !c->classes[k].is_builtin_const) continue;
    if (comp_name_responder(c, k, name, 0) < 0) return 0;
  }
  return 1;
}
/* Does every value the receiver of call u can be (self for a receiverless
   call) answer `name` by a method of the program's own, so the call's
   targets (cplan_targets, which lists those methods only) are all it can
   reach? A boxed receiver that may hold a builtin value, or whose classes
   the analysis cannot bound, may not. Another receiver (a builtin's, a
   class method's self) is taken as its targets say. */
static int share_plan_recv_answers_by_targets(Compiler *c, int u, const char *name) {
  int recv = nt_ref(c->nt, u, "receiver");
  if (recv < 0) {
    Scope *s = comp_scope_of(c, u);
    return !s || s->is_cmethod || s->class_id < 0 || share_plan_below_has(c, s->class_id, name);
  }
  TyKind rt = comp_ntype(c, recv);
  if (rt == TY_POLY) {
    if (poly_recv_builtin(c, u)) return 0;
    int nc = 0;
    const int *cls = poly_recv_classes(c, u, &nc);
    if (!cls) return 0;
    for (int i = 0; i < nc; i++)
      if (comp_name_responder(c, cls[i], name, 0) < 0) return 0;
    return 1;
  }
  return !ty_is_object(rt) || share_plan_below_has(c, ty_object_class(rt), name);
}
/* See share.h. */
int share_plan_targets_cover(Compiler *c, int u) {
  const char *nm = u >= 0 && nt_kind(c->nt, u) == NK_CallNode ? nt_str(c->nt, u, "name") : NULL;
  return nm && share_plan_recv_answers_by_targets(c, u, nm) && !share_plan_exc_text_call(c, u);
}
/* Is v a call every target of which answers a String literal on every
   path (scope_ret_str_literal)? A frozen literal keeps one identity: its
   bytes are the literal's own, and a handle made over them is the
   literal's (sp_String_of_literal), so
   what the call hands on is the object. See share.h. */
int share_plan_call_answers_literal(Compiler *c, int v) {
  int u = nt_paren_single(c->nt, v);
  if (u < 0 || nt_kind(c->nt, u) != NK_CallNode || nt_ref(c->nt, u, "block") >= 0) return 0;
  int tg[CPT_MAX], n = cplan_targets(c, u, tg, CPT_MAX);
  const char *nm = nt_str(c->nt, u, "name");
  if (n > 0) {
    /* the targets are the program's methods only: a receiver that may be
       a value whose class has none answers by a builtin method instead,
       and an exception's message or to_s by the builtin text of an
       exception class that overrides neither (ct_exception_text lists
       only the overrides) */
    if (!nm || !share_plan_recv_answers_by_targets(c, u, nm) || share_plan_exc_text_call(c, u)) return 0;
    /* (a target no value can reach -- a base class's method that every
       class made overrides -- answers nothing) */
    int any = 0;
    for (int i = 0; i < n; i++) {
      /* (a method of a class the receiver never holds, its class fact,
         answers nothing here: share_target_armed) */
      if (!share_target_armed(c, u, tg[i])) continue;
      if (scope_ret_str_literal(c, tg[i])) { any = 1; continue; }
      if (!nm || c->scopes[tg[i]].is_cmethod || share_plan_method_reached(c, tg[i], nm)) return 0;
    }
    return any;
  }
  /* a receiverless call in an instance method the plan cannot list
     (`cop_name` in a base class that only its subclasses define): self is
     the class or one below it, and each of those made answers by the
     method its chain has; one whose chain has none answers by a builtin
     (Exception#message) or raises, which the plan does not tell apart,
     so it supplies nothing. A builtin class among them (a reopened
     `class Object`) holds values the program does not make by `new`, so
     it counts as made */
  Scope *s = comp_scope_of(c, u);
  if (!nm || nt_ref(c->nt, u, "receiver") >= 0 || nt_ref(c->nt, u, "arguments") >= 0 ||
      !s || s->is_cmethod || s->class_id < 0) return 0;
  int any = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k != s->class_id && !is_descendant(c, k, s->class_id)) continue;
    if (!c->classes[k].instantiated && !c->classes[k].is_builtin_const) continue;
    int mi = call_target(c, u, k, nm, 0, NULL);
    if (mi < 0 || !scope_ret_str_literal(c, mi)) return 0;
    any = 1;
  }
  return any;
}
