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
  /* when set, a node's answer once found (1 runs no code, 2 runs code),
     per node below memo_n: only for a walk that asks neither question */
  unsigned char *memo;
  int memo_n;
} OrderWalk;

/* Does evaluating the subtree at `id` run no code and store nothing --
   reads of variables and literals, scalar arithmetic, typed-array index
   reads and plain field reads, all the way down? A whitelist, so anything
   it does not name runs code. */
int order_pure_walk(Compiler *c, int id, const OrderWalk *w);

/* Does call `id` itself run no code of the program's, whatever its
   operands do: a builtin operator over scalars, an index read of a typed
   Array by an Integer (`@ref[addr & 3]`), an index read of a boxed value
   by a value whose methods run no code, in a program with no `[]` of its
   own and no Hash default block (`@bg_pattern_lut[@bg_pattern]`), a plain
   field read (`m.cols`), or a reader every class of a boxed receiver
   answers by an attr_reader (`node.name`, #10252)? A builtin handed one of
   the program's objects can call back into it, so any other call may. */
int order_call_runs_no_code(Compiler *c, int id);

/* ---- the list layer: an owner's operands in Ruby's order, no storage ----
   A call's (`[]=` and `o.x = v` too), super's and yield's: the receiver,
   then each argument, a keyword hash's keys and values one by one, a splat
   in place, the block-pass expression last; an interpolation's embedded
   parts; an Array literal's elements; a Hash literal's keys and values.
   Index op-writes, `||=` / `&&=` and multiple assignment are not listed
   yet. */
typedef enum { OPL_CALL, OPL_SUPER, OPL_YIELD, OPL_INTERP, OPL_ARRAY, OPL_HASH } OpListKind;
typedef enum { OPG_AT_CALL,      /* the guards run after every operand */
               OPG_AFTER_RECV    /* `&.`: the nil test runs after the receiver */
} OpGuard;
typedef struct {
  unsigned char kind, guard;
  int n, cap;
  int *node;
} OpList;
/* The operands of `owner` into l (its buffer reused, oplist_free frees it):
   1, or 0 when the node owns no list. */
int oplist_of(const Compiler *c, int owner, OpList *l);
void oplist_free(OpList *l);

/* ---- the record ----
   One row per list whose answer is not "nothing bound": where each operand
   is evaluated, not the form it is held in (Repr's, from OPF_ACROSS). */
typedef enum { OPA_INLINE,   /* read where the consumer puts it, after every AHEAD operand */
               OPA_AHEAD,    /* runs code: evaluated in its Ruby place, ahead of the consumer */
               OPA_HOLD,     /* a read a later AHEAD operand may change: taken in its place */
               OPA_EFFECT    /* evaluated for its effect, its value not read */
} OpAt;
enum { OPF_CODE   = 1,       /* the operand runs code (order_runs_code) */
       OPF_ACROSS = 2 };     /* an AHEAD operand runs after it, before its consumer */
typedef struct { int node; unsigned char at, flags; int by; } OpSlot;   /* by: the operand index that made it HOLD, or -1 */
typedef struct { int owner; unsigned char kind, guard, steps; int first, n; } OpOrder;   /* its slots: first..first+n */

/* The analysis phase operand_order (analyze.c): the rows of every list in
   the program's tree (the file roots and each scope's def and body), into
   c->order. After the last type change, before the String
   marks; nothing it reads is written after it before the seal. */
void order_facts_build(Compiler *c);
void order_facts_free(Compiler *c);
/* Does the subtree run code or have an effect (the walk, asking neither of
   emission's questions)? From the phase's column. */
int order_runs_code(Compiler *c, int node);
/* The row of `owner`, or NULL: nothing is bound (every operand a pure read,
   or one code operand last beside immutable values), or the phase did not
   see the list (op_order_visited). */
const OpOrder *op_order_of(const Compiler *c, int owner);
/* Did the phase decide owner's list: 1 when it has a row or nothing is
   bound; 0 for a node out of the tree or made after the phase, of which
   the record knows nothing. */
int op_order_visited(const Compiler *c, int owner);
/* The slot of `node` in owner's row, or NULL. */
const OpSlot *op_slot_of(const Compiler *c, int owner, int node);
/* Does an AHEAD operand of its list run after `node` before the consumer? */
int op_held_across(const Compiler *c, int node);

/* --dump-order (1; 2 with =all, the lists that have no row too) */
extern int g_dump_order;
/* --dump-order: one sorted line per row, taken once the analysis is final */
char *order_dump(Compiler *c);

#endif
