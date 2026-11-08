/* value_pos.h -- the nodes an expression's value can come from.

   for_each_value_position(nt, root, flags, fn, ud) calls fn on each value
   position of `root`: the places a conditional, a sequence, a begin or a
   jump takes its value from. It is pure over the node table: no Compiler, no
   types, no inference (the line CallPlan draws), so a walker asks it a syntax
   question and decides for itself what a leaf means.

   The walk is a loop over an explicit stack, so a chain of any length (an
   `elsif` ladder, a run of rescue clauses) costs stack entries, not C frames,
   and there is no depth to cut. A budget of one visit per node ends a walk
   that a shared subtree would otherwise run for ever: the callback then gets
   one VPK_GAVE_UP leaf and chooses the safe answer for its question ("any"
   answers "maybe", "every" answers "no").

   The walk never descends into a block, a lambda or a method body: whether a
   block's value is its call's (`map`) or not (`each`) is a fact about the
   call. A caller names the body as a new root.

   The value edges of each Prism node are data (tools/value_edges.yml); the
   arms they describe are generated into src/value_pos_arms.inc, and the rules
   that are evaluation semantics, not edges, are written by hand in
   value_pos.c. */
#ifndef SPINEL_VALUE_POS_H
#define SPINEL_VALUE_POS_H

#include <stdio.h>
#include "node_table.h"

typedef enum {
  VPK_EXPR,    /* an ordinary node: this is where the value comes from */
  VPK_NIL,     /* nil by absence: a missing else, an empty body, () or a bare
                  jump; node = -1 */
  VPK_JUMP,    /* a return/next/break that is not followed: its flag is off,
                  or it hands out several values or a splat; node = the jump */
  VPK_GAVE_UP  /* the visit budget ran out; node = the node it stopped at */
} VpKind;

typedef struct {
  int node;        /* the leaf */
  VpKind kind;
  int parent;      /* the structural node it hangs from (an if, a case, a
                      begin, a rescue clause, a logical operator, a write or a
                      jump that was followed): wrappers (parentheses,
                      statements, else, in) are not parents. -1 at the
                      root */
  int arm;         /* its place among the parent's value positions: for an
                      if chain 0 is the first `then`, 1 the first `elsif` (or
                      the else), and so on; a case's when/in clauses count 0..
                      and its else follows them; a begin's body is 0, its rescue
                      clauses 1.. and its else follows them; `a || b` and
                      `x rescue y` are 0 and 1 */
  int cond_depth;  /* branches crossed from the root: each if, elsif, unless,
                      case, begin, rescue clause, `&&`, `||` and rescue
                      modifier counts one, a wrapper, a write or a followed
                      jump none */
  unsigned reach;  /* VPR_*: which truth values the leaf is the walk's value
                      for. 0 for a leaf that is the value whatever it holds */
} VpLeaf;

/* A left operand is the value of `a || b` only when truthy and of `a && b`
   (under VP_AND_LEFT) only when falsy; every leaf below it inherits that, so
   a caller judging the type of a leaf can set aside the values it never
   answers with (a nil or false from the left of `||`; anything truthy from
   the left of `&&`). A leaf with both bits is never the value. */
enum { VPR_TRUTHY = 1u << 0, VPR_FALSY = 1u << 1 };

/* A nonzero answer stops the walk. */
typedef int (*VpFn)(const VpLeaf *leaf, void *ud);

enum {
  /* a tail return/next/break yields the positions of its value (one
     non-splat argument); a bare one yields VPK_NIL. Off, or with several
     values or a splat, it is a VPK_JUMP leaf. */
  VP_FOLLOW_RETURN = 1u << 0,
  VP_FOLLOW_NEXT = 1u << 1,
  VP_FOLLOW_BREAK = 1u << 2,
  /* `a && b`: the left side is also a position (the value when falsy). Off:
     the right side only. */
  VP_AND_LEFT = 1u << 3,
  /* a begin with an `else` also yields its body. Off, the else's value
     replaces it. */
  VP_BEGIN_BOTH = 1u << 4,
  /* `x = e` and the other plain writes (and a multiple write) yield the
     positions of `e`. `x ||= e` and `x &&= e` (a local, an instance, global
     or class variable, a constant, a constant path, an attribute, an index)
     answer the target's old value as well when it is truthy (`||=`) or falsy
     (`&&=`): they yield the write node itself as a leaf, standing for that
     old value and classified as a read of the target would be, and then the
     positions of `e`. Off, the write is a leaf. An operator write (`x += e`)
     answers the operator's result and is always a leaf. */
  VP_WRITES = 1u << 5,
  /* a case/in without an else yields VPK_NIL (CRuby raises instead). Off, it
     yields nothing for the missing else. */
  VP_CASE_IN_RAISE_NIL = 1u << 6,
  /* an `a || b` or `a && b` that is walked (VPS_LOGIC) is also yielded as a
     leaf of its own, standing for the whole expression, before its operands:
     for a caller that judges the type the expression has as a whole as well
     as the operands one by one. Not part of VP_ALL_FLAGS. */
  VP_LOGIC_NODES = 1u << 7,
  /* Leave clause wrappers intact; only parentheses and statement lists peel. */
  VP_PARENS_ONLY = 1u << 17,
  /* Parentheses holding other than exactly one statement (`()`, `(a; b)`) are
     a leaf; those holding one are looked through. */
  VP_PAREN_ONE = 1u << 18,

  /* The shapes the walk sees through; a shape that is off is a leaf. No bit
     at all means every shape. A walker passes the subset it handled before it
     moved here, so that moving it changes no generated code. */
  VPS_IF = 1u << 8,           /* if, elsif, else, and the ternary */
  VPS_UNLESS = 1u << 9,
  VPS_CASE = 1u << 10,        /* case/when */
  VPS_CASE_IN = 1u << 11,     /* case/in */
  VPS_BEGIN_PLAIN = 1u << 12, /* a begin with no rescue, else or ensure */
  VPS_BEGIN_CLAUSES = 1u << 13, /* a begin with any of them */
  VPS_RESCUE_MOD = 1u << 14,  /* x rescue y */
  VPS_LOGIC = 1u << 15,       /* `a || b`, `a && b` */
  /* Only with no VPS_ bit besides: no shape at all, so that only the
     wrappers (parentheses, statements, else, an `in` clause) are looked
     through. Without it, no bit means every shape. */
  VPS_NONE = 1u << 16,
  VPS_BEGIN = VPS_BEGIN_PLAIN | VPS_BEGIN_CLAUSES,
  VPS_ALL = VPS_IF | VPS_UNLESS | VPS_CASE | VPS_CASE_IN | VPS_BEGIN | VPS_RESCUE_MOD | VPS_LOGIC
};

/* Everything on: what --dump-value-positions walks with. */
#define VP_ALL_FLAGS (VP_FOLLOW_RETURN | VP_FOLLOW_NEXT | VP_FOLLOW_BREAK | VP_AND_LEFT | \
                      VP_BEGIN_BOTH | VP_WRITES | VP_CASE_IN_RAISE_NIL | VPS_ALL)

/* Calls fn on each value position of root in source order, depth first (the
   `ensure` clause is never one). Answers the first nonzero fn answered, else 0. */
int for_each_value_position(const NodeTable *nt, int root, unsigned flags, VpFn fn, void *ud);

/* The one node the value of root comes from, for a caller that asks what an
   expression is, and not which of several it may be. VP1_NODE: a single
   ordinary expression, *node (root itself when nothing is a wrapper; `(a; b)`
   answers b, and `((x))` x). VP1_NIL: the value is nil by absence (`()`, an
   empty body), *node = -1. VP1_MANY: several positions, a jump that is not
   followed or a gave-up leaf, or no expression at all (root < 0), *node = -1:
   the caller does what it does for a value it cannot name. flags are the
   iterator's; VPS_NONE looks through the wrappers only (parentheses,
   statement lists, else and `in` clauses), so an `if` or a `case` is one node. */
typedef enum { VP1_NODE, VP1_NIL, VP1_MANY } Vp1Kind;
Vp1Kind vp_one(const NodeTable *nt, int root, unsigned flags, int *node);

/* ---- the upward half ----

   The walkers that ask how a node's parent uses its value (is it the method's
   answer, a statement whose value is thrown away, an argument, a receiver)
   climb from the node through the same value edges, read from the same
   annotation the other way: a node is a value position of its parent exactly
   when the iterator yields it from the parent. The flags are the iterator's:
   a shape that is off is no edge, VP_AND_LEFT and VP_BEGIN_BOTH name the
   operands and the begin body that count, VP_FOLLOW_* make a single non-splat
   argument of a jump an edge into the jump, and VP_WRITES a write's value an
   edge into the write. */

/* Is child, a direct child of parent, a value position of it: parent's value
   can be child's? A statement list's value is its last statement's. */
int vp_is_value_edge(const NodeTable *nt, int parent, int child, unsigned flags);

typedef enum {
  VPU_ENDED,     /* the parent takes the node as an input or a scope body, not as its own
                    value: a predicate, a receiver, an argument, a def or block body, a
                    loop body. parent = the ender */
  VPU_DROPPED,   /* a statement before the last of its list: its value is thrown away.
                    parent = the list */
  VPU_JUMP,      /* the node is the one value of a return, next or break whose flag is on.
                    parent = the jump: the value leaves by it */
  VPU_ROOT,      /* no parent is left: the climb reached the top of the table */
  VPU_GAVE_UP    /* the budget ran out (a table with a cycle): the safe answer is the caller's */
} VpUpKind;

typedef struct {
  VpUpKind kind;
  int node;      /* the last node of the climb: the child of the ender. The node itself when
                    it was the ender's operand at once */
  int parent;    /* the ender, as the kinds above say; -1 at a root */
  int branches;  /* conditionals crossed on the way (as VpLeaf.cond_depth counts them): 0 when
                    the node is the value however the climb went */
  unsigned reach; /* VPR_*: the truth values the node answers the top for, as VpLeaf.reach
                    says of a leaf. A bit is set when the climb crossed the left operand of
                    `||` (truthy) or of `&&` under VP_AND_LEFT (falsy); that operand was
                    also tested, so a walker asking whether a value is thrown away or
                    unread counts a node with a bit as read */
} VpUp;

/* Climbs from node while the parent takes it as its own value, and says where
   it ended. A budget of one step per node ends a climb in a table with a
   cycle. Answers up->kind. */
VpUpKind vp_climb(const NodeTable *nt, const int *par, int node, unsigned flags, VpUp *up);

/* Where a climb ended, for the walkers that classify it the same way. Each
   answers the node, or -1 when the climb did not end there. */

/* the def whose body the climb reached: the value is the method's answer */
int vp_up_def(const NodeTable *nt, const VpUp *up);

/* the while or until whose body the climb reached: a loop's body is run for
   its effect */
int vp_up_loop(const NodeTable *nt, const VpUp *up);

/* the call whose block's body the climb reached: whether the value is the
   call's answer is a fact about the call, and the caller's to decide */
int vp_up_block_call(const NodeTable *nt, const int *par, const VpUp *up);

/* Did the climb end at a clause that is run for its effect: the body of a
   begin whose else follows it (the begin answers the else's value), an
   ensure clause, or a BEGIN or END block? Its value is thrown away. */
int vp_up_effect_clause(const NodeTable *nt, const VpUp *up);

/* --dump-value-positions: for the body of each method and of the program, the
   leaves the walk yields with every flag on, one line each (the unit-test
   surface of the iterator and of tools/value_edges.yml). */
void value_pos_dump(const NodeTable *nt, FILE *out);

/* --check-value-pos: walks tables built by hand to see the two limits: a cycle
   ends in one VPK_GAVE_UP leaf, and a wide case and a 5000-arm elsif chain are
   walked in order. 0 when they hold. */
int value_pos_check(void);

#endif
