/* call_plan.h -- the user method a call binds, resolved from the tables.

   cplan_user(c, id) answers which user method the call node `id` reaches
   -- the method scope, the class whose chain was searched, the arm kind
   (UC_*, compiler.h) and whether the class tables already decide the
   dispatch (one method, or a switch over overrides) -- from the scope and
   class tables and the settled node types alone. It never runs inference
   and never emits, so asking it changes nothing (#7100, Phase B).

   A plan is memoized per node, but only where the node is read as itself:
   no view, no instance_exec scope move or class, no inline splice. Inside
   one of those it is computed afresh and not kept. Nothing reads it to
   emit yet; --plan-check compares it with inference's record and with the
   binding codegen made. */
#ifndef SPINEL_CALL_PLAN_H
#define SPINEL_CALL_PLAN_H

#include "compiler.h"

typedef enum {
  CP_NONE,      /* no user method */
  CP_DIRECT,    /* one method, whatever the receiver's runtime class */
  CP_VIRTUAL    /* a switch: a descendant overrides, or a boxed receiver */
} CplanDispatch;

typedef struct {
  int mi;                  /* the method scope, or -1 */
  short owner_ci;          /* the class whose chain was searched, or -1 */
  unsigned char via;       /* UC_* */
  unsigned char dispatch;  /* CplanDispatch */
} CallPlan;

const CallPlan *cplan_user(Compiler *c, int id);
/* whether mi is the plan's method or, for a switch, one of its arms */
int cplan_virtual_member(Compiler *c, int id, const CallPlan *p, int mi);

#endif
