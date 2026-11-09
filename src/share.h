/* share.h -- the questions the String rule asks of the program (#6765).

   Every slot that can hold a String holds its handle (slot_holders and
   share_default_apply in analyze.c enumerate them as ShareHolders). The
   questions below are what codegen and the analysis still ask: whether a
   value is a new String no other name holds, which literal blocks a method
   can run, whether a node hands on a String's handle, and plain questions
   of names, targets and literals (analyze_share.c). */
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
  int node;             /* a node that names the holder, for a message */
} ShareHolder;

/* (Re)build c->share's tables from the current types and calls. */
void share_facts_build(Compiler *c);
void share_facts_free(Compiler *c);

/* Is multiple write n a plain one (`a, b = x, y`): an Array literal value
   with no splat, and no rest or post target -- each target takes its own
   value? */
int share_masgn_plain(const NodeTable *nt, int n);
/* Does call n store its literal block's values into a container it
   answers or changes (an Array's or a Hash's map, map!, zip with a block,
   a fill with a block; an Array.new or Hash.new block), reaching no method
   of the program's? Not where its value is dropped, but map!'s and a
   fill's, which keep them in the receiver. */
int share_call_stores_block(Compiler *c, int n);

/* Can a slot of type t hold a String (a String, a box, a container that may
   hold one, an object whose native class keeps one)? */
int sh_may_hold(const Compiler *c, TyKind t);


/* Once the analysis is final: mark each String-keyed Hash call's key that
   reads a shared handle, with nothing run between the read and the call,
   as strbuf_read_raw, so it hands over the live buffer instead of a copy. */
void share_mark_borrows(Compiler *c);

/* Does method scope mi answer no String: each expression its value can be
   (a nil among them) is none a String can be, by its type or form or by what
   is written to an instance variable it reads? A method whose values are
   not followed answers 0. */
int share_scope_answers_no_string(Compiler *c, int mi);
/* Can a method of the program's own (a reopening of builtin class cls, or
   a module a prepend can put ahead of it) stand in for cls's method name
   where the call plan does not see it? */
int share_builtin_overridden(Compiler *c, const char *cls, const char *name);


/* The literal blocks method scope mi yields to, as a list in *blocks:
   their count, or -1 when a block the table does not list reaches its
   yields (a block passed as a value, a zsuper's, a dynamic call's). */
int share_method_blocks(const Compiler *c, int mi, const int **blocks);
/* The literal blocks that can reach method scope mi's yields: its own
   and, through each method that hands mi the block it was given (`&blk`),
   that method's, each method once, into out (at most cap): their count, or
   -1 when a zsuper's or a dynamic call's block reaches one of them, or
   there are more than cap. Any other block that reaches them is a proc
   the yield calls. */
int share_method_reaching_blocks(const Compiler *c, int mi, int *out, int cap);
/* The class that `K.new(...)` (through parentheses) builds exactly: a class
   of the program's with no `def self.new` in its chain. -1 for any other
   node. */
int share_exact_new_class(Compiler *c, int n);
/* Can call `call` reach its target `mi`? Not a class method a boxed
   receiver's call lists for the class objects a box may hold, where no
   class reaching it by that name can be a value (comp_class_may_be_value):
   no box ever holds that class object. */
int share_target_reachable(Compiler *c, int call, int mi);
/* Is user method mi, a target cplan_targets lists for boxed call `call`,
   one its dispatch arms: does a class the receiver's class fact keeps reach
   it (comp_poly_candidates_at)? */
int share_target_armed(Compiler *c, int call, int mi);
/* Can block node blk hand control back to the call that yields to it: does
   its body fall off its end, or hold a `next` of its own (not a nested
   block's, lambda's, method's or loop's)? A body whose last statement is a
   `break` and that holds no such `next` leaves only by jumping out of the
   call (break, raise, throw). */
int share_block_returns(const Compiler *c, int blk);
/* The builtin arms of a boxed call, before checking its user targets. */
int share_builtin_fresh(Compiler *c, int call);
/* The methods call can run where its plan lists no members -- more than
   CPT_MAX, or a receiver of no settled type -- and the program shows every
   method of its name (no method_missing, no computed definer, no String
   evaluated, the name no alias's, singleton def's or module_function's):
   their count into out (at most cap), or -1 (#9258). */
int share_name_targets(Compiler *c, int call, int *out, int cap);
/* Does method mi answer a new String or none: every way out raises, or it
   is an inspect or to_s ending in a bare super that reaches Object's own? */
int an_target_answers_new(Compiler *c, int mi);
/* Is node n's String a new one no name holds yet, by where it comes from: a
   String literal, a method answering only its own locals' Strings, or an
   element read of a temporary container of new Strings (an Array literal of
   them, `map(&:to_s)` over Symbols)? depth: 0 from a caller. */
int share_value_fresh(Compiler *c, int n, int depth);
/* Is node n the frozen String literal itself (a literal, or a freeze, -@ or
   dedup of one)? Its handle is the literal's own, never a new one. */
int share_frozen_literal(Compiler *c, int n);
/* ...with nothing run besides it: no statements before it in parentheses */
int share_frozen_literal_alone(Compiler *c, int n);
/* ...or a read of a builtin String constant (RUBY_ENGINE ...), one frozen
   object too (share_frozen_string) */
int share_frozen_string(Compiler *c, int n);
/* A fresh value or a single-use local whose destination is its only live name. */
int share_value_unobserved(Compiler *c, int n);

/* Must the String node v evaluates to be its handle where it is emitted,
   and not its bytes: SHN_YES for every String but nil, a literal, a frozen
   String and one in a method no call reaches. */
enum { SHN_NO, SHN_YES };
int share_value_needs_handle(Compiler *c, int v);
/* ... asked for the target of a multiple write that stores v, an element
   of an Array literal it splits into its targets (also through a splat) */
int share_store_needs_handle(Compiler *c, int v);
/* Does node n hand over the shared handle (or a box holding it), not a
   copy of its bytes? The carry a deferred route is checked against
   (share_route_defer), and the String plan's supply for a value held as
   bytes. */
int share_node_carries_handle(const Compiler *c, int n);
/* ... the kind (SPK_*) of the route it hands it over by, SPK_NONE for
   none, not counting a value represented as the handle (the String plan
   reads the representation itself); *op is the node whose supply an
   SPK_SELF or SPK_OPERAND route hands on, else -1. */
int share_node_carry_kind(const Compiler *c, int n, int *op);
/* ...asked as if no store lifted n into a handle (poly_strbuf_lift), the
   lift being the store's: the route n hands the handle on by itself */
int share_node_carry_kind_unlifted(const Compiler *c, int n, int *op);

/* ---- the String plan (share_plan.c, #8328) ----
   For each String value, once the analysis is final: what its consumer
   needs, what its producer supplies, and the form those give. A consumer
   asks for a String in the plan's form through emit_str_form
   (codegen_internal.h). */
enum { SPN_BYTES, SPN_NEW, SPN_OBJECT };              /* need */
enum { SPS_BYTES, SPS_HANDLE };                       /* supply */
enum { SPF_BYTES, SPF_HANDLE, SPF_WRAP, SPF_COPY };   /* form */
/* the role of a value's consumer: it takes the value (keeps, changes,
   compares or hands it on), reads its bytes, tests it for nil or false,
   stores it into a multiple write's target, or asks its identity (equal?,
   object_id, __id__); or it boxes the value (a poly slot, a container's
   element, a poly argument), the role a box's emitter asks for (SPR_BOX),
   which the memo never holds */
enum { SPR_TAKE, SPR_READ, SPR_TEST, SPR_STORE, SPR_IDENTITY, SPR_BOX };
/* the producer that supplies a value's handle (share_plan_producer), one
   kind per route codegen hands a String on by; each face renders each
   kind (codegen_str.c):
     NONE         its bytes only;
     BOX          a box, which hands over itself;
     SLOT         a variable's or a write's slot that holds the handle, a
                  String Range's kept endpoint (repr_of's RS_HANDLE);
     DEMANDED     a reader or receiver call that renders the handle under a
                  handle mark or demand (RS_DEMANDED);
     ELEM         a boxed container's element, a box of the handle (RS_ELEM);
     SLOT_POLY    a narrowed box's slot (RS_SLOT_POLY);
     FROZEN       a frozen String: a literal (share_frozen_literal) or a
                  builtin String constant, its own handle;
     SELF         a call that answers its String receiver, whose supply is
                  that receiver's (the operand);
     BANG_BOX     a bang on a boxed receiver: the receiver's box or nil;
     PROC_CALL    a proc's (or a catch's) answer, which comes back boxed;
     POLY_ARMS    a boxed receiver's call whose every arm answers a handle
                  it publishes or a new String, boxed arm by arm;
     EXC_MESSAGE  an exception's message, the handle it keeps;
     EXC_SUPER    super in an exception's message or to_s override that
                  hands on the builtin's: the kept message's handle;
     PUBLISHED    a call whose targets publish their handle (the return
                  pickup), a spliced call answering a variable;
     READER       a reader of a slot that holds the handle;
     WRITE        a write in value position whose slot holds the handle;
     ASSIGN       an attribute assignment's value, the handle it stores;
     OPERAND      a route that answers the String it is handed (`+s`,
                  String(s), tap, a conversion): the operand's supply;
     BLOCK_VALUE  a call answering its block's value (then, inject, loop,
                  instance_exec's splice, a spliced yielding call): each
                  value the block answers;
     YIELD        a yield spliced with a literal block: the block's value;
     COND         a conditional or begin taken arm by arm: each arm;
     SEQUENCE     parentheses or statements: the last statement (the
                  operand);
     LIFT         a box codegen lifts into its handle (poly_strbuf_lift) */
enum { SPK_NONE, SPK_BOX, SPK_SLOT, SPK_DEMANDED, SPK_ELEM, SPK_SLOT_POLY, SPK_FROZEN, SPK_SELF, SPK_BANG_BOX,
       SPK_PROC_CALL, SPK_POLY_ARMS, SPK_EXC_MESSAGE, SPK_EXC_SUPER, SPK_PUBLISHED, SPK_READER, SPK_WRITE,
       SPK_ASSIGN, SPK_OPERAND, SPK_BLOCK_VALUE, SPK_YIELD, SPK_COND, SPK_SEQUENCE, SPK_LIFT, SPK__COUNT };
/* ...which, for an element of multiple write mw, the seal asks of the write */
int share_plan_multi_renders(Compiler *c, int mw, int e);
/* ...and emit_expr gives it as the handle already (share_plan.c)? */
int share_plan_emits_handle(Compiler *c, int e);
typedef struct {
  unsigned char need, supply, form;
  unsigned char kind;   /* SPK_*, which share_plan_of fills where the supply
                           was asked (a need above bytes) */
} SharePlan;
/* Fill the memo of each node's need (the end of repr_seal); free it with
   the Compiler. */
void share_plan_build(Compiler *c);
void share_plan_free(Compiler *c);
/* The plan of node v for a consumer in `role` (SPR_*) where codegen is:
   the value's need from the memo where the node is read as itself, else
   asked live (*memo, which may be NULL, says which); the supply always
   asked live, as it reads repr_of. */
SharePlan share_plan_of(Compiler *c, int v, int role, int *memo);
/* The plan for a consumer in `role` of node v, whose need is given. */
SharePlan share_plan_with_need(Compiler *c, int v, int role, int need);
/* The role of the consumer node v sits in, as the program has it (a taker
   for a node made after seal), and the role of its second consumer, or -1:
   a call answering its receiver's String has its own answer tested. */
int share_plan_role(Compiler *c, int v);
/* The producer of node v's handle where codegen is (SPK_*), asked live as
   the supply is, whatever a consumer needs; *operand (may be NULL) is the
   node whose supply an SPK_SELF, SPK_OPERAND or SPK_SEQUENCE value hands
   on, the receiver whose box an SPK_BANG_BOX call answers, else -1. */
int share_plan_producer(Compiler *c, int v, int *operand);
/* ...asked as if no store lifted v into a handle (poly_strbuf_lift): the
   route v hands its String on by itself */
int share_plan_producer_unlifted(Compiler *c, int v, int *operand);
/* Are the targets the plan lists for call u all it can reach: every value
   its receiver can be answers the name by a method of the program's own,
   and it is no exception's builtin message or to_s? */
int share_plan_targets_cover(Compiler *c, int u);
/* Is v a call every target of which answers a String literal on every
   path, whose bytes are the literal's own: a handle made over them, or a
   box of them, is the literal's handle, and
   its identity is its bytes' pointer, as a frozen String's is? */
int share_plan_call_answers_literal(Compiler *c, int v);
/* Is target mi of call v one answering a String literal whose answer the
   pickup after the call renders as the literal itself, among targets that
   publish their handle: a method whose body is one String literal
   (`def tag_name = "Group/SectionName"`), called on self with no
   arguments or block, every value of whose receiver answers the name by
   the program's own methods? */
int share_plan_literal_pickup_clean(Compiler *c, int v, int mi);
/* Is node v, a String call a store lifts into a handle (poly_strbuf_lift),
   boxed by the handle its producer supplies (the plan's SPR_BOX form
   with the object a lift needs), not from its bytes? A call whose route
   boxes cannot render (strbuf_box_unrendered) is not. Asked live. */
int share_plan_box_handle(Compiler *c, int v);



#endif
