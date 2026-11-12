/* implied_calls.h -- the methods CRuby calls for a node that the program's
   text does not name (tools/implied_calls.yml, generated into
   src/implied_calls.inc): the to_s of an interpolation, the to_ary of
   `a, b = o`, the each of a `for`, the === of a `when`, the inherited of a
   class header. Pure over the node table, like the value-position walk in
   value_pos.h. */
#ifndef SPINEL_IMPLIED_CALLS_H
#define SPINEL_IMPLIED_CALLS_H

#include <stdio.h>
#include "node_table.h"

/* the side a call lands on: a method of an object's class, or of a class object */
enum { NTS_INST = 1, NTS_CLASS = 2 };

/* the receiver of a call that no child node holds */
enum {
  NTI_NONE = -1,     /* no node holds it: the value a read answers, the class a hook is sent to */
  NTI_SELF = -2,     /* the self of the method or class body the node is in */
  NTI_YIELDED = -3,  /* the value yielded to the block, or to the loop variable */
  NTI_MATCHED = -4   /* the value a pattern is matched against */
};

typedef struct {
  const char *name;        /* the method called */
  int operand;             /* the node whose value receives the call (a wrapper
                              such as a StatementsNode may stand for its last
                              statement), or an NTI_ code */
  unsigned char side;      /* NTS_ bits: both when the operand is an object or a class */
  unsigned char planned;   /* the call's lowering asks the call plan which method it
                              runs (cplan_implied_target): it is one of the calls the
                              plan binds by version (call_node_implied_names). The
                              row's `planned` key, temporary until every row has it */
  const char *target;      /* the builtin class the call converts to, whose values it
                              leaves as they are (the yml's `conversions`); NULL for a
                              call that is no such conversion */
} NtImplied;

/* The calls node `id` makes: written to out[0..max), their number answered
   (more than max when the array was short; 0 for a node that calls nothing,
   a kind with no row, and a bad id). A call whose operand is a literal that
   cannot hold an object of the program's, or a pattern or a target, is not
   one. */
int nt_implied_calls(const NodeTable *nt, int id, NtImplied *out, int max);
int nt_implied_possible(const NodeTable *nt, int id);   /* the kind has rows, or is a super */
unsigned nt_implied_name_sides(const char *name);   /* the NTS_ sides some row names `name` for, 0 when none: is it a method CRuby's syntax calls */
int nt_implied_super(const NodeTable *nt, int id);   /* a super: it calls the builtin of its def's name when no ancestor of the program has it */

/* --dump-implied-calls: for each node of the program's own file that makes
   implied calls, one line per call (the unit-test surface of
   tools/implied_calls.yml). */
void implied_calls_dump(const NodeTable *nt, FILE *out);

#endif
