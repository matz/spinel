/* order_plan.h -- the operand-order record (#11506).

   Ruby evaluates an operand list (a call's receiver and arguments, an
   interpolation's parts, a literal's elements) left to right, and a C
   expression over them promises no order. Which operands have to be
   evaluated ahead, in their Ruby place, is one decision per list, made
   here once from settled facts (order_plan.c). */
#ifndef ORDER_PLAN_H
#define ORDER_PLAN_H

#include "compiler.h"

/* What a walk of "runs no code" asks beside the node's own shape
   (order_pure_walk): `ran`, when set, answers a node that already ran into
   a temp, which reads that temp whatever its shape (emission's bindings,
   view_ran_first); `alloc` counts a field read that builds a copy as code
   (an allocation: emission's question, which reads the String marks). The
   record asks neither. */
typedef struct {
  int (*ran)(int node);
  int alloc;
} OrderWalk;

/* Does evaluating the subtree at `id` run no code and store nothing --
   reads of variables and literals, scalar arithmetic, typed-array index
   reads and plain field reads, all the way down? A whitelist, so anything
   it does not name runs code. */
int order_pure_walk(Compiler *c, int id, const OrderWalk *w);

#endif
