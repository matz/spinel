/* share.h -- the share classes --share-strings decides by (#6765).

   A union-find over the places a String object can be held (holders: a
   local or parameter, an ivar, a global, a class variable, a constant, the
   elements of a container) and the values flowing between them, built by
   one walk over the node table (analyze_share.c). Two holders in one class
   may hold the same String object. A class records whether an in-place
   String mutation reaches it (SHF_MUT), whether it meets anything the walk
   does not follow (SHF_UNKNOWN), and whether a mutation reaches it through a
   receiver that is no holder of its own, so no slot can take the new
   pointer back (SHF_INDIRECT).

   The facts only describe; repr_str_shares (repr.c) is the rule that reads
   them. They are built only under --share-strings. */
#ifndef SPINEL_SHARE_H
#define SPINEL_SHARE_H

#include "compiler.h"

typedef enum {
  SHK_VALUE,    /* an expression's value: a container literal, a join */
  SHK_LOCAL,    /* a local or a parameter: scope, local */
  SHK_IVAR,     /* cid, name */
  SHK_GVAR,     /* name */
  SHK_CVAR,     /* name */
  SHK_CONST,    /* name */
  SHK_ELEM,     /* the elements of the containers of one class */
  SHK_RET,      /* a method's value: scope */
  SHK_YIELD,    /* what a method yields: scope */
  SHK_BLKRET,   /* what the blocks a method yields to answer: scope */
  SHK_SELF,     /* a String instance method's receiver: scope */
  SHK_UNKNOWN   /* anything the walk does not follow */
} ShareKind;

enum {
  SHF_MUT      = 1,   /* an in-place String mutation reaches the class */
  SHF_UNKNOWN  = 2,   /* the class meets UNKNOWN */
  SHF_INDIRECT = 4,   /* mutated through a receiver that is no holder */
  SHF_MULTI    = 16,  /* an ivar of the class is written a String it did not
                         make (a call's answer, a member read): the holder is
                         one per class but a slot per object, so it can be
                         several names at once (sh_ivar_store) */
  SHF_IDENTITY = 32   /* a reopening's receiver is compared by identity or
                         handed to a consumer that keeps or mutates it */
};
/* The facts are kept in one byte per class (ShareFacts.flags): with SHF_OUT
   (8), SHF_CHG (64) and SHF_FRZ (128) of analyze_share.c, bits 1 to 128 are
   all taken. The next flag has to widen the field first. */

typedef struct {
  unsigned char kind;   /* ShareKind */
  int scope, local;     /* SHK_LOCAL: the scope and its local's index;
                           SHK_RET/YIELD/BLKRET: the method scope */
  int cid;              /* SHK_IVAR: the owning class */
  const char *name;     /* SHK_IVAR/GVAR/CVAR/CONST */
  int node;             /* a node that names the holder, for a message */
} ShareHolder;

/* The flows the walk follows from a value into the place that takes it:
   the node that takes it (the site) and the node whose value it is. Where
   the rule shares the value's class, codegen has to hand the shared handle
   along the flow; a copy there is a second String the other names never
   see. Only flows into a slot (a variable, a parameter, an element) or an
   in-place change are kept: a method's, a block's or a jump's value is a
   value until it reaches one, and that flow is the call's, the yield's or
   the loop's. The seal checks each against the routes codegen carries the
   handle along (strbuf_flow_carries); a holder's read hands on its slot,
   which the seal checks as a holder, wherever codegen reads it as one. */
typedef enum {
  SHFL_WRITE,    /* a variable's write, an optional parameter's default:
                    site the write (the parameter) */
  SHFL_MEMBER,   /* an attribute writer's, a Struct member's or
                    instance_variable_set's store: site the call */
  SHFL_ARG,      /* an argument bound to a user method's parameter: site
                    the call */
  SHFL_ELEM,     /* a container's element: site the container (a literal,
                    the receiver a call stores into, Array.new's value) */
  SHFL_BLOCK,    /* a block's value a call stores (map, map!, Array.new and
                    Hash.new blocks): site the call, or the `next` that
                    answers it */
  SHFL_YIELD,    /* what a yield hands its block's parameters: site the
                    yield */
  SHFL_PARAM,    /* an iterator's receiver bound to its block's parameter
                    (then, tap): site the call */
  SHFL_LEND,     /* an argument whose slot a parameter is lent (sh_lend):
                    site the parameter's holder (share_holder), not a node */
  SHFL_MULTI,    /* a multiple write's value taken by one target: site the
                    write */
  SHFL_MUTATE    /* the receiver an in-place String change goes through:
                    site the call */
} ShareFlowKind;

/* (Re)build c->share from the current types and tables. */
void share_facts_build(Compiler *c);
void share_facts_free(Compiler *c);

/* The holders, by index 0..share_holder_count-1. */
int share_holder_count(const Compiler *c);
const ShareHolder *share_holder(const Compiler *c, int h);
/* The holder of a local / an ivar, or -1 when the walk made none. */
int share_local_holder(const Compiler *c, int scope, int local);
int share_ivar_holder(const Compiler *c, int cid, const char *name);
int share_self_holder(const Compiler *c, int scope);
int share_self_used(const Compiler *c, int holder);
/* The element of holder h's containers' elements, or -1 (not a holder:
   read it with share_elem_flags / share_elem_holders). */
int share_elem_holder(const Compiler *c, int h);

/* The facts of holder h's class. */
unsigned share_class_flags(const Compiler *c, int h);
/* the number of holders in it that store a String (not a method's value) */
int share_class_holders(const Compiler *c, int h);
/* the facts of an element (share_elem_holder's answer) */
unsigned share_elem_flags(const Compiler *c, int e);
int share_elem_holders(const Compiler *c, int e);

/* Under --share-strings, once the analysis is final: mark each String-keyed
   Hash call's key that reads a shared handle, with nothing run between the
   read and the call, as strbuf_read_raw, so it hands over the live buffer
   instead of a copy. Answers how many it marked. */
int share_mark_borrows(Compiler *c);

/* A master route refusal: a String handed along a route that would copy
   it. Under --share-strings the route is the rule's, as share_route_defer
   says. */
typedef struct ShareRoute {
  int site;            /* the node the refusal names */
  int value, elems;    /* the String handed along (elems: the Strings the
                          container `value` names holds) */
  int to, to_elems;    /* the holder it reaches: node `to`'s value (to_elems:
                          its elements), or with to_name, that local in the
                          scope of node `to`; -1 none */
  const char *to_name;
  int carry;           /* the node that hands the String along, or -1
                          (SHARE_CARRY_NONE); SHARE_CARRY_COPY when the
                          route hands over a copy whatever it reads */
  int sole;            /* a local's read node whose String must have no other
                          name, or -1: a plain local (no parameter, no
                          capture) whose class holds one name at most
                          (share_node_one_name), asked of the final facts */
  int fresh_elems;     /* (with elems) the container's elements reach only the
                          route's holder: an element iterator answering its
                          receiver whose value is dropped, so a container
                          whose elements no name holds hands on Strings no
                          other name holds */
  int rt_site, rt_grow; /* (scan routes) the call whose block changes no
                          String in place but by growing or clearing it
                          (scan_block_changes), or -1: a failed sole check
                          leaves its subject to a run-time length check
                          (rt_grow: one is needed, on a handle subject)
                          instead of refusing */
  char *msg;           /* (a kept route's message) */
} ShareRoute;
/* a route's sole when the subject is no local the facts can be asked of */
enum { SHARE_SOLE_UNVOUCHED = -2 };
enum { SHARE_CARRY_NONE = -1, SHARE_CARRY_COPY = -2 };
ShareRoute share_route(int site, int value, int elems);
/* Is node n's value a String the walk reached and found no identity in (a
   fresh one no other name holds)? */
int share_node_fresh(const Compiler *c, int n);
/* Does the rule share the elements of node n's value (a container)? */
int share_node_elems_share(const Compiler *c, int n);
/* ...or is their class proven to keep its Strings to itself: it exists,
   the rule does not share it, and it meets nothing the walk does not
   follow? */
int share_node_elems_local(const Compiler *c, int n);
/* Is node v, a new String, boxed into a container literal a holder keeps
   whose elements share_node_elems_local proves its own, so that a copy of
   the box no other name can tell from the String may hold its bytes? */
int share_plan_box_plain(Compiler *c, int v);
/* Is node n a blockless builtin's new Array of new Strings whose elements
   the rule shares? Its one evaluation is the only source of those Strings,
   so where it is consumed each can be wrapped as a handle of its own. */
int share_node_fresh_elems(const Compiler *c, int n);
/* Can a method of the program's own (a reopening of builtin class cls, or
   a module a prepend can put ahead of it) stand in for cls's method name
   where the call plan does not see it? */
int share_builtin_overridden(Compiler *c, const char *cls, const char *name);
/* Is call node `call` a retaining iterator (select, reject, find_all, the
   in-place filters, partition) with a literal block over a fresh Array of
   new Strings (as share_node_fresh_elems has it, before the rule is asked
   whether they share)? Its answer keeps elements the block's parameter
   names. */
int share_iter_fresh_elems(const Compiler *c, int call);
/* ... and the rule shares those elements: the answer holds the handles its
   block saw, so the receiver is consumed as the PolyArray of handles a
   local's would be and the answer is one too. */
int share_iter_answers_handles(const Compiler *c, int call);
/* Can node n's value (a container) be reached again once its expression
   is done: a holder keeps it, it leaves a call to be read after, or it
   meets what the walk does not follow? */
int share_node_anchored(const Compiler *c, int n);

/* A master route-refusal site asks this first. With the flag off it answers
   0 and the site refuses as before. With it on, the route is the rule's,
   and it passes when the facts see it (the String and the holder it
   reaches in one class) and either the rule does not share that class (no
   other name can see the copy) or it does and `carry` hands over the
   handle (a SHARE_CARRY_COPY route never does); every other holder of a
   shared class holds the handle or is refused by name at seal. Asked while the analysis runs, the facts are not
   final yet: the route is kept, and repr_seal refuses it with `msg` unless
   the final facts pass (share_routes_check). Asked from codegen, it answers
   from the final facts. */
int share_route_defer(Compiler *c, const ShareRoute *q, const char *msg);
void share_routes_check(Compiler *c);
void share_routes_free(Compiler *c);

/* The flows the walk recorded (ShareFlowKind), by index
   0..share_flow_count-1: the kind, the site and the value node. */
int share_flow_count(const Compiler *c);
int share_flow_at(const Compiler *c, int i, int *site, int *value);
/* Does the rule share the class of flow i's destination: the holder of the
   variable, parameter, ivar or block parameter, or the elements of the
   container, that takes the value (-1 when the walk made none)? A new
   String joins no class, so its own class never says whether the holder it
   is stored into shares. */
int share_flow_dest_shares(const Compiler *c, int i);
/* The literal blocks method scope mi yields to, as a list in *blocks:
   their count, or -1 when a block the walk does not list reaches its
   yields (a block passed as a value, a zsuper's, a dynamic call's). */
int share_method_blocks(const Compiler *c, int mi, const int **blocks);
/* Is call node `call`'s String one no other name holds: each user method
   it reaches answers only Strings its returns did not join to its value
   (sh_settle_rets: built in its own locals, which die with the call)?
   The settled return-tail fact also admits fresh returns and a returned
   parameter bound to a fresh argument or omitted fresh default. */
int share_call_fresh(Compiler *c, int call);
/* Can block node blk hand control back to the call that yields to it: does
   its body fall off its end, or hold a `next` of its own (not a nested
   block's, lambda's, method's or loop's)? A body whose last statement is a
   `break` and that holds no such `next` leaves only by jumping out of the
   call (break, raise, throw). */
int share_block_returns(const Compiler *c, int blk);
/* The builtin arms of a boxed call, before checking its user targets. */
int share_builtin_fresh(Compiler *c, int call);
/* Is node n's String a new one no name holds yet, by where it comes from: a
   String literal, a method answering only its own locals' Strings, or an
   element read of a temporary container of new Strings (an Array literal of
   them, `map(&:to_s)` over Symbols)? depth: 0 from a caller. */
int share_value_fresh(Compiler *c, int n, int depth);
/* Is node n the frozen String literal itself (a literal, or a freeze, -@ or
   dedup of one)? Its handle is the literal's own, never a new one. */
int share_frozen_literal(Compiler *c, int n);
int share_return_owned(const Compiler *c, int n, int mi);
/* Is node n a container literal a builtin only reads and keeps none of
   (`puts [a, b]`)? */
int share_node_peeked(const Compiler *c, int n);
/* Is node n's result dropped or only read by a builtin that keeps none? */
int share_node_transient(const Compiler *c, int n);
/* Is call n an attribute assignment of one plain value, under --share-strings? */
int share_assign_call(const Compiler *c, int n);
/* Is node n's value dropped (a statement, or a tail no caller reads)? */
int share_node_unread(const Compiler *c, int n);
/* The facts (SHF_*) of the class of node n's value. */
unsigned share_node_flags(const Compiler *c, int n);
/* Does the class of node n's value hold one name at most: no more than one
   holder stores its String, and nothing the walk does not follow meets it
   (its other values are transients a call or a mutator made)? */
int share_node_one_name(const Compiler *c, int n);
/* A fresh value or a single-use local whose destination is its only live name. */
int share_value_unobserved(Compiler *c, int n);
/* Does the rule share the class of node n's value? */
int share_node_shares(const Compiler *c, int n);
/* --share-check (share_check.c): the flag, and the compiler's state freed
   with it. */
extern int g_share_check;
void share_check_free(Compiler *c);
/* Is node v the receiver of an in-place String change the walk recorded, or
   a link of the chain of self-answering calls down to it? */
int share_node_mutated(Compiler *c, int v);

/* Must the String node v evaluates to be the shared handle where it is
   emitted, and not a copy of its bytes: SHN_YES, or SHN_FRESH when no other
   name sees it, so a copy is fine unless a box keeps it (the box has to be
   the handle's, or no change shows through the box's other names). A pure
   query, answered from the final facts. */
enum { SHN_NO, SHN_YES, SHN_FRESH };
int share_value_needs_handle(Compiler *c, int v);
/* ... asked for the target of a multiple write that stores v, an element
   of an Array literal it splits into its targets (also through a splat):
   the literal is no container that keeps v (its own need says bytes where
   no name sees the literal), the target is one. */
int share_store_needs_handle(Compiler *c, int v);
/* Does node n hand over the shared handle (or a box holding it), not a
   copy of its bytes? The carry a deferred route is checked against
   (share_route_defer), and the String plan's supply for a value held as
   bytes. */
int share_node_carries_handle(const Compiler *c, int n);

/* ---- the String plan (share_plan.c, #8328) ----
   For each String value under --share-strings, once the analysis is final:
   what its consumer needs, what its producer supplies, and the form those
   give. A consumer asks for a String in the plan's form through
   emit_str_form (codegen_internal.h); --share-check compares the plan with
   the conversion each emitted site has. */
enum { SPN_BYTES, SPN_NEW, SPN_OBJECT };              /* need */
enum { SPS_BYTES, SPS_HANDLE };                       /* supply */
enum { SPF_BYTES, SPF_HANDLE, SPF_WRAP, SPF_REFUSE }; /* form */
/* the role of a value's consumer: it takes the value (keeps, changes,
   compares or hands it on), reads its bytes, tests it for nil or false,
   stores it into a multiple write's target, or asks its identity (equal?,
   object_id, __id__) */
enum { SPR_TAKE, SPR_READ, SPR_TEST, SPR_STORE, SPR_IDENTITY };
typedef struct {
  unsigned char need, supply, form;
} SharePlan;
/* the form a need and a supply give */
int share_plan_form(int need, int supply);
/* Fill the memo of each node's need (the end of repr_seal); free it with
   the Compiler. */
void share_plan_build(Compiler *c);
void share_plan_free(Compiler *c);
/* The plan of node v for a consumer in `role` (SPR_*) where codegen is:
   the value's need from the memo where the node is read as itself, else
   asked live (*memo, which may be NULL, says which); the supply always
   asked live, as it reads repr_of. */
SharePlan share_plan_of(Compiler *c, int v, int role, int *memo);
/* The memo's need of node v where codegen is, or -1 where it cannot answer. */
int share_plan_memo_need(Compiler *c, int v);
/* The value's need (what a taker needs), asked live (--share-check
   compares the memo with it), and the plan for a consumer in `role` with
   that need given. */
int share_plan_need(Compiler *c, int v);
SharePlan share_plan_with_need(Compiler *c, int v, int role, int need);
/* The role of the consumer node v sits in, as the program has it (a taker
   for a node made after seal), and the role of its second consumer, or -1:
   a call answering its receiver's String has its own answer tested. */
int share_plan_role(Compiler *c, int v);
int share_plan_second_role(Compiler *c, int v);
const char *share_plan_form_name(int form);
const char *share_plan_role_name(int role);
/* Is node n an explicit writer's attribute assignment whose value the facts
   keep apart from its right-hand side's String (nothing observes them as one
   object)? */
int share_assign_unjoined(const Compiler *c, int n);
/* Does the rule share the String attribute assignment n hands on? */
int share_assign_shares(const Compiler *c, int n);

/* SPINEL_SHARE_STATS=3: name the mutations that reach UNKNOWN's class */
void share_dump_unknown_mutations(Compiler *c);

/* The stats' second build, with every union with UNKNOWN dropped: would
   the holder with h's key share without what the walk does not follow? */
struct ShareFacts *share_facts_build_closed(Compiler *c);
void share_facts_drop(struct ShareFacts *F);
int share_closed_shares(const struct ShareFacts *F, const ShareHolder *h);

#endif
