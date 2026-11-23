/* share.h -- the questions the String rule asks of the program (#6765).

   Every slot that can hold a String holds its handle (slot_holders and
   share_default_apply in analyze.c enumerate them as ShareHolders). The
   questions below are what the analysis still asks (analyze_share.c). */
#ifndef SPINEL_SHARE_H
#define SPINEL_SHARE_H

#include "compiler.h"

typedef enum {
  SHK_LOCAL,    /* a local or a parameter: scope, local */
  SHK_IVAR,     /* cid, name */
  SHK_GVAR,     /* name */
  SHK_CVAR,     /* name */
  SHK_CONST,    /* name */
  SHK_RET       /* a method's value: scope */
} ShareKind;

typedef struct {
  unsigned char kind;   /* ShareKind */
  int scope, local;     /* SHK_LOCAL: the scope and its local's index;
                           SHK_RET: the method scope */
  int cid;              /* SHK_IVAR: the owning class */
  const char *name;     /* SHK_IVAR/GVAR/CVAR/CONST */
} ShareHolder;

/* (Re)build c->share's tables from the current types and calls. */
void share_facts_build(Compiler *c);
void share_facts_free(Compiler *c);

/* Does call n store its literal block's values into a container it
   answers or changes (an Array's or a Hash's map, map!, zip with a block,
   a fill with a block; an Array.new or Hash.new block), reaching no method
   of the program's? Not where its value is dropped, but map!'s and a
   fill's, which keep them in the receiver. */
int share_call_stores_block(Compiler *c, int n);

/* Can a slot of type t hold a String (a String, a box, a container that may
   hold one, an object whose native class keeps one)? */
int sh_may_hold(const Compiler *c, TyKind t);

/* Can block node blk hand control back to the call that yields to it: does
   its body fall off its end, or hold a `next` of its own (not a nested
   block's, lambda's, method's or loop's)? A body whose last statement is a
   `break` and that holds no such `next` leaves only by jumping out of the
   call (break, raise, throw). */
int share_block_returns(const Compiler *c, int blk);

#endif
