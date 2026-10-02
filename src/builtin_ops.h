/* builtin_ops.h -- builtin methods as data, read by inference and codegen.

   A row says what a builtin call on a receiver of one TyKind answers and
   how codegen emits it. Inference reads the result kind; codegen reads the
   emitter id and runs the emitter codegen_ops.c keeps for it. One row
   replaces the matching rule in infer_call_inner and the matching arm in
   emit_call_body, which today each decide the same call separately (#7100).

   A family's lookup sits where that family's legacy branch sat in each
   chain, so everything an earlier branch claims still goes there; it moves
   up only once nothing earlier can match the same call. */
#ifndef SPINEL_BUILTIN_OPS_H
#define SPINEL_BUILTIN_OPS_H

#include "types.h"

/* What a row asks of the call's block. BF_ANY: the legacy rule ignored the
   block, so the row does too. */
typedef enum { BF_ANY, BF_NONE, BF_REQUIRED } BopBlock;

/* The emitter codegen runs for a row (codegen_ops.c). BOPE_NONE: the row
   only types the call; codegen leaves it to the legacy chain. */
typedef enum {
  BOPE_NONE,
  BOPE_TEMPLATE,          /* arg, its placeholders filled (codegen_ops.c) */
  BOPE_PSTATUS_SUCCESS,   /* Process::Status#success?: true, false or nil */
  BOPE_PSTATUS_EQ,        /* Process::Status#== / #eql? with no operand */
  /* the concurrency handles (codegen_call_concurrency.c) */
  BOPE_THREAD_SET_REPORT, /* Thread#report_on_exception= */
  BOPE_THREAD_RAISE,      /* Thread#raise */
  BOPE_THREAD_TLS,        /* Thread#[] / []= / key? / thread_variable_*, any key */
  BOPE_MUTEX_SLEEP,       /* Mutex#sleep */
  BOPE_CONDVAR_WAIT,      /* ConditionVariable#wait */
  BOPE_QUEUE_PUSH,        /* Queue#push / << / enq, with non_block and timeout: */
  BOPE_QUEUE_POP,         /* Queue#pop / shift / deq, likewise */
  BOPE_FIBER_RESUME,      /* Fiber#resume */
  BOPE_FIBER_TRANSFER,    /* Fiber#transfer */
  BOPE_FIBER_RAISE,       /* Fiber#raise */
  BOPE__COUNT
} BopEmit;

typedef struct BuiltinOp {
  TyKind recv;            /* receiver kind the row applies to */
  const char *name;
  signed char argc_min, argc_max;
  BopBlock block;
  TyKind result;          /* what the call answers; TY_UNKNOWN: inference
                             leaves the call to the rules after it */
  BopEmit emit;
  const char *arg;        /* BOPE_TEMPLATE's C text */
  TyKind arg0;            /* the kind the first argument must have, or
                             TY_UNKNOWN for any */
  unsigned char flags;    /* BOPF_* */
} BuiltinOp;

/* The row answers for a boxed (poly) receiver too, behind a run-time class
   check: emit_poly_builtin_method emits these names unboxed, so inference
   types the call with the row's result (bop_find_boxed). */
#define BOPF_BOXED 1

/* argc_max of a row that takes any number of arguments */
#define BOP_ARGC_ANY 127

/* The row for `name` called with `argc` arguments (and a block when
   has_block) on a receiver of kind rt, or NULL. Pure: it reads no node and
   no Compiler state.

   Rows of one name are tried narrowest arity first, and among rows of the
   same arity in the order they are written. So a codegen row for one arity
   comes before the wider row that only types the call for every arity, and
   a row with an arg0 guard before the unguarded row of its arity. A row
   with an arg0 guard never fits here: bop_find_arg checks the guard. */
const BuiltinOp *bop_find(TyKind rt, const char *name, int argc, int has_block);

/* bop_find, with arg0_of(ud) answering the first argument's kind for the
   rows that guard it. It is called at most once, and only when such a row
   is a candidate, so a lookup that needs no argument's kind computes none. */
typedef TyKind (*BopArgKind)(const void *ud);
const BuiltinOp *bop_find_arg(TyKind rt, const char *name, int argc, int has_block,
                              BopArgKind arg0_of, const void *ud);

/* Whether any row applies to receivers of kind rt: a caller checks this
   before reading the call's arguments, so receivers no row covers cost
   nothing. */
int bop_covers(TyKind rt);

/* The BOPF_BOXED row of kind rt for `name`, or NULL. */
const BuiltinOp *bop_find_boxed(TyKind rt, const char *name, int argc, int has_block);

#endif
