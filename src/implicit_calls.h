/* implicit_calls.h -- the methods CRuby calls that the program does not name.

   Two tables say which: src/implied_calls.inc (what a node's syntax calls: the
   to_s of an interpolation, the each of a `for`, the inherited of a class
   header; nt_implied_calls) and src/builtin_callbacks.inc (what a builtin
   calls back on the values it is handed: sort asks its elements for <=>;
   bop_name_callbacks). This module reads both for the passes that ask "may
   this method run?" (reachability, which keeps a method CRuby calls back once
   a node that makes the call can run).

   A call has the name CRuby sends, the operand that receives it and, for a
   builtin's callback, the role the operand is handed over in (BCR_*). Nothing
   here decides what a pass does with a call. */
#ifndef SPINEL_IMPLICIT_CALLS_H
#define SPINEL_IMPLICIT_CALLS_H

#include <stdio.h>
#include "compiler.h"
#include "analyze_internal.h"
#include "implied_calls.h"
#include "builtin_ops.h"

/* ic_node_calls's answer for a call of a core name the builtin table does not
   cover: it may call anything */
#define IC_ANY (-1)

typedef struct IcCall {
  const char *name;
  int operand;         /* a syntax row: the node whose value receives the call,
                          or an NTI_ code. A builtin's callback: the call node,
                          whose receiver, arguments and block `role` picks */
  unsigned short role; /* 0 for a syntax row, else one BCR_ bit */
  unsigned char recv;  /* 1: `operand` is the receiver itself (the callbacks of an
                          implied call), not a call node */
  const char *target;  /* a syntax row's conversion: the builtin class whose values
                          it leaves as they are, receiving no call
                          (implied_calls.yml `conversions`); NULL for none */
} IcCall;

/* What node `id` makes CRuby call without naming it: its kind's rows, and for a
   call the callbacks of the builtin of its name (an implied call of a core
   name takes the callbacks of its operand's elements and its own as well). Out
   to out[0..max), their number answered (more than max when it was short);
   IC_ANY for a call of a core name the table does not cover. Pure over the node
   table and the tables. */
int ic_node_calls(Compiler *c, int id, IcCall *out, int max);

/* Whether CRuby calls a method `name` on a program's object it is handed
   (class_side 0: a method of the object's class) or on a class object it is
   handed (class_side 1: a singleton method), from some builtin or, for an
   object, from some syntax. A name only the table names, whatever node or
   builtin makes the call: for the passes that ask before knowing which. */
int ic_core_callback(const char *name, int class_side);

/* Whether the operand that receives the call may hold an object of the
   program's. Before inference, or when the types do not settle it, yes. */
int ic_operand_may_hold(Compiler *c, const IcCall *k);

/* The node of the value a block answers: the last statement of its body, -1
   for an empty one */
int ic_block_value(const NodeTable *nt, int blk);

/* Does call node `id` run a method of the program's (typed runs only), so that
   no builtin of its name runs? */
int ic_runs_program_method(Compiler *c, int id);

/* the text --dump-implicit gives for a call of node `id` */
void ic_cause(Compiler *c, int node, const IcCall *k, char *buf, size_t n);

/* ---- the run's state (Compiler.ic) ---- */

typedef struct { char *name, *cause; int scope; } IcMark;
typedef struct { char *name, *cause; } IcLive;
struct IcState {
  int typed;           /* the run's types are settled (1 or 2), else 0 */
  int scope;           /* the scope being collected */
  ANameHash class_only;/* the names only plain classes' methods hold (cr_class_only_names) */
  ANameHash defined;   /* the names some scope or alias defines */
  ANameHash cext;      /* the names of the methods a C extension defines: a call of one hands its values to C code */
  int has_cext;        /* the program loads a C extension */
  int obj_range;       /* some value is a Range of objects (TY_OBJ_RANGE), typed run only */
  unsigned char *plain;/* per class: 0 not asked, 1 not plain, 2 plain (comp_plain_class) */
  /* --dump-implicit alone: what each scope's nodes call, and the calls of
     the reachable scopes, one per name */
  IcMark *marks;
  int nmarks, capmarks;
  IcLive *live;
  int nlive, caplive;
  ANameHash live_set;
};

/* start a reachability run: forget the last one's, with the types settled or not */
void ic_begin(Compiler *c, int typed);
/* the scope whose nodes the walk is collecting the calls of */
void ic_scope(Compiler *c, int scope);
/* Is `name` defined by some scope or alias? A call of a name nothing defines
   keeps nothing alive. */
int ic_defined(const Compiler *c, const char *name);
/* The current scope has a node that calls `name`; `cause` is a short text for
   --dump-implicit, which alone keeps it. A name of "*" is a call the builtin
   table does not cover. */
void ic_record(Compiler *c, const char *name, const char *cause);
/* the walk is over: the names of the scopes it reached are live (--dump-implicit) */
void ic_end(Compiler *c);
void ic_free(Compiler *c);

/* --dump-implicit: each live implicit name with a cause, and each reachable def
   of the program's own file */
void ic_dump(Compiler *c, FILE *out);
extern int g_dump_implicit;

#endif
