/* share_plan.c -- the plan for each String value under --share-strings (#8328).

   Whether a String reaches its consumer as the object or as a copy is
   decided today at each emission site, and predicted by the analysis
   through a mirror it keeps of the emitters (strbuf_flow_carries). The
   plan is the one place that decision is meant to come from. For each
   value:

     need    what its consumer needs (share_value_needs_handle): its bytes
             (SPN_BYTES); a handle that may be a new one, because no other
             name can see the String, but a box keeps it (SPN_NEW); or the
             object itself (SPN_OBJECT);
     supply  whether its producer can hand over the handle, or a box that
             holds it (SPS_HANDLE, share_plan_supplies), or only its bytes
             (SPS_BYTES);
     form   what the two give (share_plan_form): the bytes (SPF_BYTES);
             the producer's handle (SPF_HANDLE); the bytes wrapped into a
             new handle, which no other name can tell from the object
             (SPF_WRAP); or the bytes, which the consumer that needs the
             object makes a handle of (SPF_COPY).

   A need is a consumer's. A value has one consumer, the node it sits in,
   whose role says what it does with it (share_plan_role): it takes the
   value -- keeps it, changes it, compares it, hands it on (SPR_TAKE); it
   reads its bytes and keeps nothing, as an interpolation does, or a
   freeze of a variable's String, which reaches the String through the
   variable, or a String iterator's Enumerator walked where it is made
   (SPR_READ); it only asks whether it is nil or false, as a predicate does (SPR_TEST);
   or, for an element of the Array literal a multiple write splits (also
   through a splat, `a, b = *[s, t]`), the target the element is stored
   into takes it (SPR_STORE), where the literal is no container; or it
   asks the value's identity, as equal?, object_id and __id__ do where no
   class of the program defines them (SPR_IDENTITY). A taker needs what
   the value's facts say (share_value_needs_handle); a reader and a test
   need its bytes; a store needs what its target does
   (share_store_needs_handle). An identity asks for the object, and is the
   handle where the producer has one (for an exception's message, the box
   the exception keeps it in) and the bytes' pointer where it has only
   bytes, which are then all the String there is: never a new handle,
   which no other name holds, and never a refusal.
   Some values have a second consumer:
     - a call that answers its receiver or nil (rule 4 below) answers its
       receiver's String, so its own answer is only tested for nil
       (share_plan_second_role: SPR_TEST);
     - a write, an attribute or index assignment, an `||=` or `&&=` and a
       call answering its receiver answer a value they also store or
       change: that second consumer is the answering node's own, asked of
       that node;
     - a multiple write whose own value is read answers the values it
       stores in a new Array: a taker beside the store, which needs no more
       than the store (the literal's elements need what its facts say).

   Nothing is refused by the plan. One consumer reads it: object_id and
   __id__ on a String, through emit_str_form (codegen_str.c), the one
   place a consumer is to ask for a String in the plan's form. Every other
   emitter still decides on its own; --share-check (share_check.c) reads
   the plan at each site it marks and reports how often it agrees with the
   conversion the emitted C has there.

   repr_of is not memoised, and neither is anything that reads it. The
   need reads the share facts, final at seal, the node's type, names and
   scope's reachability; it is memoised at the end of repr_seal, in one
   byte per node beside the node's consumer role. Each guard of the memo
   covers one way codegen makes a node read as something else:
     - a view, a scope move or a splice: cplan_plain_ctx, the call plan's
       own rule; a poly dispatch arm: the arm context at rest;
     - a rename for one emission (the value-form bang transform's), a reset,
       a swap of two nodes, an added node: the node table's version, which
       exactly those bump (nt_node_set_str, nt_set_str, nt_node_reset,
       nt_swap_nodes, nt_new_node);
     - a node retyped outside the view stack: its type as at seal;
     - a method codegen makes reachable after seal (the exception message
       and to_s overrides the dispatcher calls): a node of a scope
       unreachable at seal is not memoised (SPN_ASK).
   A swap of a node's receiver, arguments or block (nt_node_set_ref) and
   an emitter's retype of a local bump nothing; the need does not read a
   local's type, and reads a call's receiver only through freshness.
   The supply reads repr_of, which an emitter's retyped local changes
   outside all of those, so it is always computed where it is asked.
   The role is the kind of the node a value sits in, as the program has
   it at seal, and needs no guard: codegen renames calls and retypes
   values, but no interpolation, predicate, multiple write or identity
   query changes where its parts sit, and no rename makes or unmakes an
   identity query; a receiver or arguments swapped in for one
   emission (nt_node_set_ref) and nodes added after seal sit in a call,
   whose role is a taker's. The second role of a call that answers its
   receiver reads its name, its receiver's type and its targets, as rule 4
   does, so it is asked where it is asked.
   --share-check asks the need live too: the memo is measured, never
   trusted, until an emitter reads it. (The one that does asks for an
   identity, whose need is the object whatever the memo says.) */

#include <stdlib.h>
#include "codegen_internal.h"
#include "builtin_ops.h"
#include "builtin_names.h"
#include "call_plan.h"
#include "repr.h"
#include "share.h"
#include "value_pos.h"

/* the form a need and a supply give */
static int share_plan_form(int need, int supply) {
  if (need == SPN_BYTES) return SPF_BYTES;
  if (supply == SPS_HANDLE) return SPF_HANDLE;
  return need == SPN_NEW ? SPF_WRAP : SPF_COPY;
}


/* The String receiver call v answers (a String method that answers self,
   or self or nil, and no program method of the name), or -1. itself, and
   tap with its block, answer any receiver, so a boxed one (a bang's String
   or nil) is handed on as it is too. */
static int share_plan_self_recv(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  /* (a String's conversion of a boxed local an is_a? guard narrows to a
     String answers the box's String: the slot takes the local's handle,
     strbuf_narrowed_box_conversion) */
  int nb = strbuf_narrowed_box_conversion(c, v);
  if (nb >= 0) return nb;
  const char *nm = nt_str(nt, v, "name");
  int r = nt_paren_single(nt, nt_ref(nt, v, "receiver")), tg[CPT_MAX];
  int any_recv = nm && ((sp_streq(nm, "itself") && call_plain_argc(c, v) == 0 && nt_ref(nt, v, "block") < 0) ||
                        (is_tap_name(nm) && nt_ref(nt, v, "block") >= 0));
  if (!nm || r < 0 || (c->ntype[r] != TY_STRING && c->ntype[r] != TY_STRBUF && !(any_recv && c->ntype[r] == TY_POLY)))
    return -1;
  /* (a String's to_s or to_str answers the String itself where it is no
     subclass's instance, BOPF_SELF_EXACT: a bang's on a boxed receiver,
     the box's String or nil, whose handle emit_strbuf_value takes) */
  int exact = (sp_streq(nm, "to_s") || sp_streq(nm, "to_str")) && face_str_bang_call(c, r);
  if (!any_recv && !exact && !(bop_answers_self(TY_STRING, nm, call_plain_argc(c, v), nt_ref(nt, v, "block") >= 0) &
                     (BOPF_SELF | BOPF_SELF_OR_NIL)))
    return -1;
  return cplan_targets(c, v, tg, CPT_MAX) == 0 ? r : -1;
}

/* Is node v super in an exception class's message or to_s that publishes
   the String it answers (scope_ret_handle) and inherits the builtin's
   (no ancestor of the program's own defines it): the message the
   exception keeps, as its handle? */
static int share_plan_exc_super(Compiler *c, int v) {
  NodeKind k = v >= 0 ? nt_kind(c->nt, v) : NK_NONE;
  if (k != NK_SuperNode && k != NK_ForwardingSuperNode) return 0;
  Scope *sc = comp_scope_of(c, v);
  int mi = sc ? (int)(sc - c->scopes) : -1, cls = sc ? sc->class_id : -1;
  if (mi < 0 || cls < 0 || cls >= c->nclasses || sc->def_node < 0 || !sc->name ||
      (!is_exception_message(sc->name)) || (!class_is_exc_subclass(c, cls) && !class_is_exc_reopen(c, cls)))
    return 0;
  int sup = c->classes[cls].parent;
  return scope_ret_handle(c, mi) && (sup < 0 || comp_method_in_chain(c, sup, sc->name, NULL) < 0);
}

static int share_plan_yield_supply(Compiler *c, int v, int depth);

/* The producer of node v's handle (SPK_*, share.h), the first of these
   that names it:
     1. a box (SPK_BOX);
     2. a value repr_of represents as the handle by where the handle comes
        from: a slot, a demanded call, a boxed element, a narrowed box. Not
        RS_FRESH, repr_of's answer for a handle whose source none of its
        checks names, not a proof that the String is new: such a value is
        asked the rules below;
     3. a frozen String, whose handle is its own (SPK_FROZEN);
     4. a call that answers its String receiver (SPK_SELF; *op is that
        receiver), or a chain whose base is held as its handle where a
        mutator takes it (strbuf_held_chain: SPK_SELF with no operand), a
        bang on a boxed receiver (SPK_BANG_BOX; *op the receiver), a boxed
        receiver's call whose arms each answer a String, read as a box
        whose handle is bound (SPK_POLY_ARMS);
     5. a new String (share_value_fresh), which has no handle to hand over,
        whatever a route says about whether its copy can be seen: none;
     6. super in an exception's text override (SPK_EXC_SUPER);
     7. a call whose answer the C picks up after it, the handle its targets
        publish (SPK_PUBLISHED, strbuf_call_picks_up), whatever route the
        call is besides;
     8. the route the analysis checks a carry against
        (share_node_carry_kind, which names each route by its kind: a
        callee that publishes the handle, a reader, a write, a value route
        over an operand, a block's value, a conditional ...); a `||` or
        `&&` with a bang on a boxed receiver on an arm (SPK_COND); a yield
        a block splices, asked outside the splice (SPK_YIELD);
     9. parentheses or statements: their last statement (SPK_SEQUENCE).
   Anything else has its bytes only (SPK_NONE). */
static int share_plan_producer_core(Compiler *c, int v, int *op, int unlifted) {
  const NodeTable *nt = c->nt;
  Repr r = repr_of(c, v);
  *op = -1;
  if (r.kind == RK_BOXED) return SPK_BOX;
  if (r.kind == RK_STRBUF && r.strbuf_src != RS_FRESH) {
    switch (r.strbuf_src) {
    case RS_DEMANDED: return SPK_DEMANDED;
    case RS_ELEM: return SPK_ELEM;
    case RS_SLOT_POLY: return SPK_SLOT_POLY;
    default: return SPK_SLOT;
    }
  }
  if (share_frozen_string(c, v)) return SPK_FROZEN;
  /* (a chain whose base is held as its handle where a mutator takes it,
     strbuf_held_chain: that held handle, SPK_SELF with no operand) */
  if (strbuf_held_chain(c, nt_paren_single(nt, v))) return SPK_SELF;
  int rv = share_plan_self_recv(c, v);
  if (rv >= 0) { *op = rv; return SPK_SELF; }
  if (face_str_bang_call(c, v)) { *op = nt_ref(nt, v, "receiver"); return SPK_BANG_BOX; }
  /* a boxed receiver's call whose every arm answers a String, the handle it
     publishes or a new one (or a conversion of it a String answers with
     itself): the dispatch is read as a box, and its handle is bound
     (strbuf_poly_user_base) */
  if (strbuf_poly_user_base(c, v) >= 0) return SPK_POLY_ARMS;
  if (share_value_fresh(c, v, 0)) return SPK_NONE;
  if (share_plan_exc_super(c, v)) return SPK_EXC_SUPER;
  /* (a call whose answer the C picks up after it, the handle its targets
     publish: that pickup, whatever route the call is besides) */
  { int u = nt_paren_single(nt, v);
    if (u >= 0 && nt_kind(nt, u) == NK_CallNode && strbuf_call_picks_up(c, u)) return SPK_PUBLISHED; }
  int k = unlifted ? share_node_carry_kind_unlifted(c, v, op) : share_node_carry_kind(c, v, op);
  if (k != SPK_NONE) return k;
  /* (a yield a block splices, asked outside the splice: its method's
     blocks, share_plan_yield_supply) */
  if (v >= 0 && nt_kind(nt, v) == NK_YieldNode && share_plan_yield_supply(c, v, 0) >= 0) return SPK_YIELD;
  /* (a `||` or `&&` with a bang on a boxed receiver on an arm, which the
     faces take arm by arm: strbuf_face_bang_cond) */
  if (strbuf_face_bang_cond(c, v)) return SPK_COND;
  NodeKind nk = v >= 0 ? nt_kind(nt, v) : NK_NONE;
  if (nk == NK_ParenthesesNode || nk == NK_StatementsNode) {
    int b = nk == NK_ParenthesesNode ? nt_ref(nt, v, "body") : v, bn = 0;
    const int *bv = b >= 0 && nt_kind(nt, b) == NK_StatementsNode ? nt_arr(nt, b, "body", &bn) : NULL;
    if (b >= 0 && !bv) bn = 1, bv = &b;
    if (bn > 0) { *op = bv[bn - 1]; return SPK_SEQUENCE; }
  }
  return SPK_NONE;
}

static int share_plan_producer_at(Compiler *c, int v, int *op) { return share_plan_producer_core(c, v, op, 0); }


int share_plan_producer_unlifted(Compiler *c, int v, int *operand) {
  int rv, k = share_plan_producer_core(c, v, &rv, 1);
  if (operand) *operand = rv;
  return k;
}

int share_plan_producer(Compiler *c, int v, int *operand) {
  int rv, k = share_plan_producer_at(c, v, &rv);
  if (operand) *operand = rv;
  return k;
}

static int share_plan_supplies_d(Compiler *c, int v, int depth);
static int share_plan_need(Compiler *c, int v);

/* Does leaf a of a value taken arm by arm (a conditional's arm, a block's
   value) give its consumer what it needs: nil, a raise (no value), a
   String no other name holds (a new handle cannot be told from it), or a
   producer that supplies the handle? A variable's read supplies what its
   slot holds: the handle, or bytes no handle holds, whose identity is
   theirs. With foreign set, a is read under another object's self (an
   instance_exec or instance_eval splice): an instance variable's read
   there is that object's, which the plan cannot read from here, so it
   hands on its slot, which the seal checks as a holder. */
static int share_plan_leaf_ok(Compiler *c, int a, int foreign, int depth) {
  const NodeTable *nt = c->nt;
  if (a < 0) return 1;
  NodeKind k = nt_kind(nt, a);
  if (k == NK_NilNode || (foreign && k == NK_InstanceVariableReadNode)) return 1;
  if (k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || repr_static_read_kind(k))
    return depth < 16 && share_plan_supplies_d(c, a, depth + 1);
  const char *nm = k == NK_CallNode ? nt_str(nt, a, "name") : NULL;
  if (nm && is_raise_alias(nm) && nt_ref(nt, a, "receiver") < 0 && comp_method_index(c, nm) < 0) return 1;
  if (share_plan_need(c, a) != SPN_OBJECT) return 1;
  return depth < 16 && share_plan_supplies_d(c, a, depth + 1);
}

/* ...and each jump of kind k (NK_NextNode, NK_BreakNode) under n that
   leaves what n is the body of, its value the leaf */
static int share_plan_jumps_ok(Compiler *c, int n, NodeKind k, int foreign, int depth) {
  const NodeTable *nt = c->nt;
  if (n < 0) return 1;
  NodeKind nk = nt_kind(nt, n);
  if (nk == NK_BlockNode || nk == NK_LambdaNode || nk == NK_DefNode || nk == NK_WhileNode || nk == NK_UntilNode ||
      nk == NK_ForNode)
    return 1;
  if (nk == k) {
    int argc = 0;
    const int *argv = call_args(nt, n, &argc);
    if (argc > 1 || (argc == 1 && !share_plan_leaf_ok(c, argv[0], foreign, depth))) return 0;
  }
  for (int i = 0; i < nt_num_refs(nt, n); i++)
    if (!share_plan_jumps_ok(c, nt_ref_at(nt, n, i), k, foreign, depth)) return 0;
  for (int i = 0; i < nt_num_arrs(nt, n); i++) {
    int m = 0;
    const int *ids = nt_arr_at(nt, n, i, &m);
    for (int j = 0; j < m; j++) if (!share_plan_jumps_ok(c, ids[j], k, foreign, depth)) return 0;
  }
  return 1;
}

/* ...a body's value: its last statement, and each jump of kind k */
static int share_plan_body_ok(Compiler *c, int body, NodeKind k, int foreign, int depth) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &n) : NULL;
  return (n == 0 || share_plan_leaf_ok(c, bb[n - 1], foreign, depth)) &&
         (k == NK_NONE || share_plan_jumps_ok(c, body, k, foreign, depth));
}

/* Does conditional (or begin, or rescue modifier) v hand on what each of
   its arms supplies: is every arm a leaf that gives the consumer what it
   needs? `l && r` answers r where l is a String (l is truthy), and l only
   where l is nil or false, so l is no String leaf. The arms are read through
   the iterator (a begin is a leaf of it, which share_plan_arm_bad takes
   apart, arm by arm, as a body's last statement); `+g` answers g itself, or
   a new String where g is frozen, so its arms are g's. */
typedef struct { Compiler *c; int depth; } SharePlanArms;
static int share_plan_arms_ok(Compiler *c, int v, int depth);
static int share_plan_arm_bad(const VpLeaf *lf, void *ud) {
  const SharePlanArms *a = ud;
  Compiler *c = a->c;
  const NodeTable *nt = c->nt;
  int v = lf->node, depth = a->depth;
  if (lf->kind == VPK_NIL) return 0;
  if (lf->kind == VPK_GAVE_UP) return 1;
  if (nt_kind(nt, v) == NK_BeginNode) {
    int el = nt_ref(nt, v, "else_clause");
    /* (the body's value is the begin's only when no else follows it) */
    if (el < 0 && !share_plan_body_ok(c, nt_ref(nt, v, "statements"), NK_NONE, 0, depth)) return 1;
    for (int rc = nt_ref(nt, v, "rescue_clause"); rc >= 0; rc = nt_ref(nt, rc, "subsequent"))
      if (!share_plan_body_ok(c, nt_ref(nt, rc, "statements"), NK_NONE, 0, depth)) return 1;
    return el >= 0 && !share_plan_body_ok(c, nt_ref(nt, el, "statements"), NK_NONE, 0, depth);
  }
  if (nt_kind(nt, v) == NK_CallNode) {
    /* `+g` answers g itself, or a new String where g is frozen */
    const char *nm = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver");
    if (nm && is_unary_plus(nm) && r >= 0 && nt_ref(nt, v, "arguments") < 0) return !share_plan_arms_ok(c, r, depth + 1);
  }
  return !share_plan_leaf_ok(c, v, 0, depth);
}
static int share_plan_arms_ok(Compiler *c, int v, int depth) {
  SharePlanArms a = { c, depth };
  return !for_each_value_position(c->nt, v, VPS_IF | VPS_UNLESS | VPS_CASE | VPS_CASE_IN | VPS_RESCUE_MOD | VPS_LOGIC,
                                  share_plan_arm_bad, &a);
}

/* Does the handle face render a spliced block's value a, a tail or a
   `next`'s, as the handle it names, where the splice reads it under the
   handle demand (emit_block_invoke)? Every producer kind but a call
   answering its own block's value that is no route the face takes
   (strbuf_tail_plain). The value's other kinds and a chain over one of
   these can, and a conditional each of whose arms is one. */
static int share_plan_tail_renders(Compiler *c, int a);
static int share_plan_tail_unrendered(const VpLeaf *lf, void *ud) {
  Compiler *c = ud;
  const NodeTable *nt = c->nt;
  int a = lf->node;
  if (lf->kind == VPK_NIL) return 0;
  if (lf->kind == VPK_GAVE_UP) return 1;
  int o, k = share_plan_producer_at(c, a, &o);
  if (k == SPK_LIFT) k = share_plan_producer_core(c, a, &o, 1);
  switch (k) {
  case SPK_BLOCK_VALUE:
    return !strbuf_tail_plain(c, a);
  case SPK_OPERAND: {
    /* (tap answers its receiver through its own block) */
    const char *nm = nt_kind(nt, a) == NK_CallNode ? nt_str(nt, a, "name") : NULL;
    if (nm && is_tap_name(nm) && nt_ref(nt, a, "block") >= 0) return 1;
    return o >= 0 && !share_plan_tail_renders(c, o);
  }
  case SPK_SELF: case SPK_SEQUENCE:
    return o >= 0 && !share_plan_tail_renders(c, o);
  default:
    return 0;
  }
}
static int share_plan_tail_renders(Compiler *c, int a) {
  return !for_each_value_position(c->nt, a, VP_AND_LEFT | VPS_IF | VPS_UNLESS | VPS_CASE | VPS_CASE_IN | VPS_LOGIC,
                                  share_plan_tail_unrendered, c);
}

/* Does call v, which answers its literal block's value (SPK_BLOCK_VALUE),
   hand on what each value it answers supplies: the block's tail and each
   `next` (then, inject, instance_exec's splice, a spliced yielding call
   that answers its yield), each `break` (loop)? A spliced call that
   answers a variable of its method hands on that slot, and a call whose
   value no block gives (sum, an ENV store) is the route's own. */
static int share_plan_block_ok(Compiler *c, int v, int depth) {
  const NodeTable *nt = c->nt;
  int u = nt_paren_single(nt, v), blk = u >= 0 && nt_kind(nt, u) == NK_CallNode ? nt_ref(nt, u, "block") : -1;
  if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) return 1;
  int body = nt_ref(nt, blk, "body");
  /* (the block's tail is read where the block is spliced in, under the
     handle demand: it supplies the handle only where the face renders it,
     share_plan_tail_renders) */
  int bn = 0;
  const int *bb = body >= 0 && nt_kind(nt, body) == NK_StatementsNode ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!strbuf_route_loop(c, u) && bn > 0 && !share_plan_tail_renders(c, bb[bn - 1])) return 0;
  if (strbuf_route_loop(c, u)) return share_plan_jumps_ok(c, body, NK_BreakNode, 0, depth);
  if (strbuf_route_ie(c, u)) return share_plan_body_ok(c, body, NK_NONE, 1, depth);
  if (strbuf_route_inline_call(c, u)) {
    int mi = strbuf_inline_mi(c, u), last = mi > 0 ? scope_body_last(c, mi) : -1;
    int bl = block_given_tail_then_last(c, last);
    if (bl >= 0) last = bl;
    if (last >= 0 && !strbuf_tail_answers_block(c, mi, last, 0)) return 1;
  }
  return share_plan_body_ok(c, body, NK_NextNode, 0, depth);
}

/* Does yield v supply the handle where the seal asks, outside the splice
   that renders it: does each block its method is given answer one in its
   tail and each `next` (share_method_blocks, share_plan_body_ok), as
   strbuf_route_yield takes a spliced block's, each a tail the face renders
   (share_plan_tail_renders)? 1 or 0; -1 for a yield that is no such
   route: a lowered method's, a top-level one, one a zsuper's or a dynamic
   call's block reaches, or the last statement of a method a block passed
   as a value reaches. A yield in
   its own blocks' tails is asked no deeper than the plan's walks go. */
static int share_plan_yield_supply(Compiler *c, int v, int depth) {
  Scope *sc = v >= 0 && nt_kind(c->nt, v) == NK_YieldNode ? comp_scope_of(c, v) : NULL;
  if (!sc || sc->def_node < 0 || sc->is_lowered_yield) return -1;
  int mi = (int)(sc - c->scopes), reach[64];
  const int *blocks = NULL;
  int n = sc->is_proc_form || sc->blk_param ? -1 : share_method_blocks(c, mi, &blocks);
  /* (a block passed as a value reaches the yields: where it is a proc, the
     yield calls it and answers the proc's box, whose handle the route takes
     (strbuf_yield_proc_box); where a method hands on the literal block it
     was given, that block splices in, as a literal one does. Not the
     method's last statement, which an inlined call unboxes to its return
     slot) */
  if (n < 0) {
    int last = scope_body_last(c, mi);
    if (last == v || block_given_tail_then_last(c, last) == v) return -1;
    n = share_method_reaching_blocks(c, mi, reach, 64);
    if (n < 0) return -1;
    blocks = reach;
  }
  if (depth > 8) return 0;
  for (int i = 0; i < n; i++) {
    int body = nt_ref(c->nt, blocks[i], "body"), bn = 0;
    const int *bb = body >= 0 && nt_kind(c->nt, body) == NK_StatementsNode ? nt_arr(c->nt, body, "body", &bn) : NULL;
    if ((bn > 0 && !share_plan_tail_renders(c, bb[bn - 1])) || !share_plan_body_ok(c, body, NK_NextNode, 0, depth + 1))
      return 0;
  }
  return 1;
}

/* The supply node v decides by itself (1 or 0), or -1 when it hands on its
   operand's (a call answering its receiver, a value route over an operand,
   a sequence's last statement): *op is then that node. */
static int share_plan_supply_kind(Compiler *c, int v, int k, int *op, int depth);

/* Is the base of the chain of self-answering calls and sequences that value
   v hands on (v itself included) a boxed receiver's call whose arms answer a
   String (SPK_POLY_ARMS)? That call, or a sequence ending in one, is what
   the faces render as a handle (strbuf_poly_user_base, emit_poly_arms_handle:
   the same function the producer rule asks); a chain of calls over it is not
   rendered, so it supplies no handle. */
static int share_plan_poly_base(Compiler *c, int v) {
  for (int depth = 0; depth < 16; depth++) {
    int u = nt_paren_single(c->nt, v), o;
    if (u < 0) return 0;
    int k = share_plan_producer_at(c, u, &o);
    if (k == SPK_POLY_ARMS) return 1;
    if ((k != SPK_SELF && k != SPK_SEQUENCE) || o < 0) return 0;
    v = o;
  }
  return 0;
}
static int share_plan_supply_at(Compiler *c, int v, int *op, int depth) {
  return share_plan_supply_kind(c, v, share_plan_producer_at(c, v, op), op, depth);
}
/* Does an instance of some class the program makes, or a builtin value
   (whose class a reopening such as `class Object` reaches), reach
   instance method mi as its `name`? */
static int share_plan_method_reached(Compiler *c, int mi, const char *name) {
  for (int k = 0; k < c->nclasses; k++)
    if ((c->classes[k].instantiated || c->classes[k].is_builtin_const) &&
        comp_method_in_chain(c, k, name, NULL) == mi) return 1;
  return 0;
}
/* Is call u an exception's message or to_s, whose targets the plan lists
   as the overrides of the exception classes it may reach
   (ct_exception_text)? */
static int share_plan_exc_text_call(Compiler *c, int u) {
  const char *nm = nt_str(c->nt, u, "name");
  int recv = nt_ref(c->nt, u, "receiver");
  if (!nm || !is_exception_message(nm) || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int cid = ty_is_object(rt) ? ty_object_class(rt) : -1;
  return rt == TY_EXCEPTION || (cid >= 0 && class_is_exc_subclass(c, cid));
}
/* Does each class or value of class cid or below it that the program has
   (made by `new`, or a builtin's) answer `name` by a method of the
   program's own? One whose chain has none answers by a builtin method or
   raises, which no target the plan lists covers. */
static int share_plan_below_has(Compiler *c, int cid, const char *name) {
  for (int k = 0; k < c->nclasses; k++) {
    if (k != cid && !is_descendant(c, k, cid)) continue;
    if (!c->classes[k].instantiated && !c->classes[k].is_builtin_const) continue;
    if (comp_method_in_chain(c, k, name, NULL) < 0) return 0;
  }
  return 1;
}
/* Does every value the receiver of call u can be (self for a receiverless
   call) answer `name` by a method of the program's own, so the call's
   targets (cplan_targets, which lists those methods only) are all it can
   reach? A boxed receiver that may hold a builtin value, or whose classes
   the analysis cannot bound, may not. Another receiver (a builtin's, a
   class method's self) is taken as its targets say. */
static int share_plan_recv_answers_by_targets(Compiler *c, int u, const char *name) {
  int recv = nt_ref(c->nt, u, "receiver");
  if (recv < 0) {
    Scope *s = comp_scope_of(c, u);
    return !s || s->is_cmethod || s->class_id < 0 || share_plan_below_has(c, s->class_id, name);
  }
  TyKind rt = comp_ntype(c, recv);
  if (rt == TY_POLY) {
    if (poly_recv_builtin(c, u)) return 0;
    int nc = 0;
    const int *cls = poly_recv_classes(c, u, &nc);
    if (!cls) return 0;
    for (int i = 0; i < nc; i++)
      if (comp_method_in_chain(c, cls[i], name, NULL) < 0) return 0;
    return 1;
  }
  return !ty_is_object(rt) || share_plan_below_has(c, ty_object_class(rt), name);
}
/* See share.h. The pickup resets the side channel before the call and
   takes what is there after it, else a new handle of the bytes, which for
   a literal is the literal's own. Such a target publishes nothing, and
   neither does anything that runs between the reset and its return: no
   argument, no receiver but self, a body that computes nothing (a
   condition or a call may publish another String's handle, which the
   pickup would then read as this target's answer). */
int share_plan_literal_pickup_clean(Compiler *c, int v, int mi) {
  const NodeTable *nt = c->nt;
  int u = nt_paren_single(nt, v);
  if (u < 0 || nt_kind(nt, u) != NK_CallNode) return 0;
  int recv = nt_ref(nt, u, "receiver");
  const char *nm = nt_str(nt, u, "name");
  if ((recv >= 0 && nt_kind(nt, recv) != NK_SelfNode) || nt_ref(nt, u, "arguments") >= 0 ||
      nt_ref(nt, u, "block") >= 0 || !nm || mi <= 0 || mi >= c->nscopes || !scope_ret_str_literal(c, mi))
    return 0;
  int b = c->scopes[mi].body, bn = 1;
  const int *bv = &b;
  if (b >= 0 && nt_kind(nt, b) == NK_StatementsNode) bv = nt_arr(nt, b, "body", &bn);
  return bv && bn == 1 && bv[0] >= 0 && nt_kind(nt, bv[0]) == NK_StringNode &&
         share_plan_recv_answers_by_targets(c, u, nm);
}
/* See share.h. */
int share_plan_targets_cover(Compiler *c, int u) {
  const char *nm = u >= 0 && nt_kind(c->nt, u) == NK_CallNode ? nt_str(c->nt, u, "name") : NULL;
  return nm && share_plan_recv_answers_by_targets(c, u, nm) && !share_plan_exc_text_call(c, u);
}
/* Is v a call every target of which answers a String literal on every
   path (scope_ret_str_literal)? A frozen literal keeps one identity: its
   bytes are the literal's own, and a handle made over them is the
   literal's (sp_String_literal_slot), so
   what the call hands on is the object. See share.h. */
int share_plan_call_answers_literal(Compiler *c, int v) {
  int u = nt_paren_single(c->nt, v);
  if (u < 0 || nt_kind(c->nt, u) != NK_CallNode || nt_ref(c->nt, u, "block") >= 0) return 0;
  int tg[CPT_MAX], n = cplan_targets(c, u, tg, CPT_MAX);
  const char *nm = nt_str(c->nt, u, "name");
  if (n > 0) {
    /* the targets are the program's methods only: a receiver that may be
       a value whose class has none answers by a builtin method instead,
       and an exception's message or to_s by the builtin text of an
       exception class that overrides neither (ct_exception_text lists
       only the overrides) */
    if (!nm || !share_plan_recv_answers_by_targets(c, u, nm) || share_plan_exc_text_call(c, u)) return 0;
    /* (a target no value can reach -- a base class's method that every
       class made overrides -- answers nothing) */
    int any = 0;
    for (int i = 0; i < n; i++) {
      /* (a method of a class the receiver never holds, its class fact,
         answers nothing here: share_target_armed) */
      if (!share_target_armed(c, u, tg[i])) continue;
      if (scope_ret_str_literal(c, tg[i])) { any = 1; continue; }
      if (!nm || c->scopes[tg[i]].is_cmethod || share_plan_method_reached(c, tg[i], nm)) return 0;
    }
    return any;
  }
  /* a receiverless call in an instance method the plan cannot list
     (`cop_name` in a base class that only its subclasses define): self is
     the class or one below it, and each of those made answers by the
     method its chain has; one whose chain has none answers by a builtin
     (Exception#message) or raises, which the plan does not tell apart,
     so it supplies nothing. A builtin class among them (a reopened
     `class Object`) holds values the program does not make by `new`, so
     it counts as made */
  Scope *s = comp_scope_of(c, u);
  if (!nm || nt_ref(c->nt, u, "receiver") >= 0 || nt_ref(c->nt, u, "arguments") >= 0 ||
      !s || s->is_cmethod || s->class_id < 0) return 0;
  int any = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k != s->class_id && !is_descendant(c, k, s->class_id)) continue;
    if (!c->classes[k].instantiated && !c->classes[k].is_builtin_const) continue;
    int mi = comp_method_in_chain(c, k, nm, NULL);
    if (mi < 0 || !scope_ret_str_literal(c, mi)) return 0;
    any = 1;
  }
  return any;
}
static int share_plan_supply_kind(Compiler *c, int v, int k, int *op, int depth) {
  switch (k) {
  case SPK_NONE: return share_plan_call_answers_literal(c, v);
  /* a call answering its receiver hands on its receiver's supply, but the
     face renders a chain over a boxed receiver's call whose arms answer a
     String (SPK_POLY_ARMS) as that call's bytes: no handle (the change
     reaches the String, the answer is a copy) */
  case SPK_SELF:
    if (*op < 0) return 1;
    return share_plan_poly_base(c, *op) ? 0 : -1;
  case SPK_SEQUENCE: return -1;
  /* a call whose targets publish their handle: where it picks the handle
     up after the call, only if every target publishes it on each path
     (strbuf_pickup_published); else the pickup falls back to a new one */
  case SPK_PUBLISHED: {
    int u = nt_paren_single(c->nt, v);
    return !(u >= 0 && nt_kind(c->nt, u) == NK_CallNode && strbuf_call_picks_up(c, u)) || strbuf_pickup_published(c, u);
  }
  case SPK_OPERAND: {
    if (*op >= 0) return -1;
    /* a String clamp: each operand it can answer */
    int ops[3];
    if (!strbuf_route_clamp(c, v, ops)) return 1;
    for (int i = 0; i < 3; i++)
      if (ops[i] >= 0 && !share_plan_leaf_ok(c, ops[i], 0, depth)) return 0;
    return 1;
  }
  case SPK_COND: return share_plan_arms_ok(c, nt_paren_single(c->nt, v), depth);
  case SPK_BLOCK_VALUE: return share_plan_block_ok(c, v, depth);
  /* a String a store lifts into a handle (poly_strbuf_lift): the lift is
     the store's, which no other consumer applies (an argument's binder
     takes the String as it is); the value supplies what it does as the
     String it is before the lift (share_node_carry_kind_unlifted), or a
     box's to_s, the box's own String (strbuf_poly_to_s) */
  case SPK_LIFT: {
    int k2 = share_plan_producer_core(c, v, op, 1);
    if (k2 == SPK_NONE) return strbuf_poly_to_s(c, v) || share_plan_call_answers_literal(c, v);
    if (k2 == SPK_SEQUENCE && *op >= 0 && share_plan_poly_base(c, *op)) return 0;
    return k2 != SPK_LIFT && share_plan_supply_kind(c, v, k2, op, depth);
  }
  /* a yield: what its method's blocks answer (a spliced yield's route,
     asked where the splice is, takes its own block's) */
  case SPK_YIELD:
    return share_plan_yield_supply(c, v, depth) != 0;
  case SPK_BOX: case SPK_SLOT: case SPK_DEMANDED: case SPK_ELEM: case SPK_SLOT_POLY: case SPK_FROZEN:
  case SPK_BANG_BOX: case SPK_PROC_CALL: case SPK_POLY_ARMS: case SPK_EXC_MESSAGE: case SPK_EXC_SUPER:
  case SPK_READER: case SPK_WRITE: case SPK_ASSIGN:
    return 1;
  }
  return 0;
}

/* One answer of a chain of self-answering calls: the stamp it holds for
   (the view epoch and the node table's version), the node that decided it
   and what that node's representation was. */
struct SharePlanSup { unsigned epoch, ver; int d; unsigned char kind, src, val, set; };

/* Is cache entry e of a chain still the answer: the same stamp, and the
   node that decided it deciding it the same way? The stamp does not move
   with a dispatch arm, a yield site's types, a splice, a scope move or a
   receiver swap, and the links between the asked node and the deciding
   one are not checked again: the entry rests on the deciding node's answer
   alone, which a query of every supply both ways (cached and walked) over
   the corpus found to agree. */
static int share_plan_sup_valid(Compiler *c, const struct SharePlanSup *e) {
  if (!e->set || e->epoch != view_epoch() || e->ver != c->nt->version) return 0;
  Repr r = repr_of(c, e->d);
  int rv;
  return r.kind == e->kind && r.strbuf_src == e->src && share_plan_supply_at(c, e->d, &rv, 0) == e->val;
}

/* Can node v's producer hand over the String's handle? By its kind
   (share_plan_producer_at): a value that hands on its operand's supply
   (a call answering its receiver, a value route over an operand, a
   sequence's last statement) supplies what that operand does; a value
   taken arm by arm (a conditional, a begin, a call answering its block's
   value) supplies the handle where each of its arms gives its consumer
   what it needs (share_plan_leaf_ok); every other kind but SPK_NONE has
   the handle, or a box that holds it.
   A chain of operands is asked at each of its links, outermost first: the
   walk down it keeps each link's answer with the node that decided it
   (Compiler.share_sup), and a link asked again reads it while the view
   epoch and the table's version are the same and the deciding node still
   decides it the same way, so a chain costs its length once, not once per
   link. Nothing outside the chain's own emission can change a link in
   between: an emitter retypes a block parameter's local, which no
   receiver chain outside the block reaches. */
static int share_plan_supplies_d(Compiler *c, int v, int depth) {
  int rv, s = share_plan_supply_at(c, v, &rv, depth);
  if (s >= 0) return s;
  int n = c->nt->count;
  if (!c->share_sup || c->share_sup_n < n) {
    free(c->share_sup);
    c->share_sup = calloc((size_t)n + 1, sizeof *c->share_sup);
    if (!c->share_sup) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->share_sup_n = n;
  }
  struct SharePlanSup *e = &c->share_sup[v];
  if (share_plan_sup_valid(c, e)) return e->val;
  /* walk down to the deciding node, then stamp every link on the way */
  int d = rv;
  while (d >= 0 && (s = share_plan_supply_at(c, d, &rv, depth)) < 0) {
    if (d < n && share_plan_sup_valid(c, &c->share_sup[d])) { s = c->share_sup[d].val; d = c->share_sup[d].d; break; }
    d = rv;
  }
  if (d < 0) return 0;
  Repr r = repr_of(c, d);
  struct SharePlanSup fill = { view_epoch(), c->nt->version, d, r.kind, r.strbuf_src, (unsigned char)s, 1 };
  for (int l = v, o; l >= 0 && l < n && l != d; l = o) {
    c->share_sup[l] = fill;
    if (share_plan_producer_at(c, l, &o) == SPK_NONE) break;
  }
  return s;
}
static int share_plan_supplies(Compiler *c, int v) { return share_plan_supplies_d(c, v, 0); }

/* The memo's byte per node: the need of the value (SPN_*), or SPN_ASK for
   a node whose scope was unreachable at seal (codegen can still make its
   method reachable, so its need is asked live), and above it, in the
   byte's other six bits, the role of the node's consumer (SPR_*). */
enum { SPN_ASK = 3, SPM_NEED = 3, SPM_ROLE_SHIFT = 2 };

static int share_plan_need_of(int need) {
  return need == SHN_NO ? SPN_BYTES : SPN_OBJECT;
}
/* The value's need (what a taker needs), asked live. */
static int share_plan_need(Compiler *c, int v) { return share_plan_need_of(share_value_needs_handle(c, v)); }

static int share_plan_box_kept(Compiler *c, int v);

/* What a consumer in `role` needs of value v, whose own need (what its
   facts say a taker needs) is `need`. */
static int share_plan_role_need(Compiler *c, int v, int role, int need) {
  if (role == SPR_READ || role == SPR_TEST) return SPN_BYTES;
  if (role == SPR_IDENTITY) return SPN_OBJECT;
  /* A box is copied by value, so a box of a String's bytes is a String of
     its own once a copy is changed in place: a box of a String no other
     name holds is a new handle, which every copy names, unless the box
     goes into nothing that keeps it (share_plan_box_kept): a builtin that
     only reads it is handed the bytes. A frozen literal is its own object already. Any
     other value needs what a taker does. */
  if (role == SPR_BOX) {
    if (share_frozen_literal(c, v)) return SPN_BYTES;
    if (share_value_fresh(c, v, 0))
      return !share_plan_box_kept(c, v) ? SPN_BYTES : SPN_NEW;
  }
  /* the store asks past the literal it splits (never less than the value) */
  if (role == SPR_STORE && need == SPN_BYTES) return share_plan_need_of(share_store_needs_handle(c, v));
  return need;
}

/* Is value e of a kind whose text emit_expr gives as the handle (a read, a
   publication, a route over a handle, a block's value, a call answering its
   receiver without changing it), not the bytes it answers (a call changing
   its receiver, a conditional, a boxed receiver's call, a bang on a boxed
   receiver, a lifted String, inject's value)? */
static int share_plan_emits_handle_kind(Compiler *c, int e) {
  for (int depth = 0; depth < 16; depth++) {
    int o, k = share_plan_producer(c, e, &o);
    if (k == SPK_SEQUENCE && o >= 0) { e = o; continue; }
    int u = nt_paren_single(c->nt, e);
    const char *nm = u >= 0 && nt_kind(c->nt, u) == NK_CallNode ? nt_str(c->nt, u, "name") : NULL;
    /* (a call answering its receiver gives it as the handle unless it
       changes it in place; inject answers its block's text) */
    /* (a variable's write in parentheses is that write's value) */
    if (u >= 0 && holder_kind_of(nt_kind(c->nt, u)) != HK_NONE && strstr(nt_type(c->nt, u), "Write")) return 1;
    if (nm && is_reduce_alias(nm)) return 0;
    if (k == SPK_SELF) return !(nm && bop_name_mutates(nm, 0));
    return k != SPK_COND && k != SPK_POLY_ARMS && k != SPK_BANG_BOX && k != SPK_LIFT && k != SPK_SEQUENCE;
  }
  return 0;
}
int share_plan_emits_handle(Compiler *c, int e) {
  Repr r = repr_of(c, e);
  if (r.as_ty != TY_STRING && r.as_ty != TY_STRBUF) return 1;
  return r.as_ty == TY_STRBUF && r.handle && share_plan_emits_handle_kind(c, e);
}

/* Does multiple write mw store the value of its element e (an element of
   its Array literal, parentheses and all) so that the plan can give it as
   the handle? A value emit_expr gives as the handle is stored as it is into
   any target. Any other is rendered through the handle face by
   emit_multi_write into a local, an instance variable, a global or an
   attribute, and into an index when its kind is one emit_expr gives as the
   handle; into a nested target, a class variable or a constant it has no
   store of such a temp, so the value stays its bytes to the plan, and is
   refused where the String is shared. So is one in a write
   that has a nested target, unless it runs nothing or is of a kind
   emit_expr gives as the handle: the write builds the nested literal ahead
   of its other values, and a value that runs nothing cannot tell the
   order. */
int share_plan_multi_renders(Compiler *c, int mw, int e) {
  const NodeTable *nt = c->nt;
  if (mw < 0 || nt_kind(nt, mw) != NK_MultiWriteNode || !share_masgn_plain(nt, mw)) return 1;
  int en = 0, ln = 0;
  const int *el = nt_arr(nt, nt_ref(nt, mw, "value"), "elements", &en), *lefts = nt_arr(nt, mw, "lefts", &ln);
  int ti = -1, nested = 0;
  for (int i = 0; i < ln; i++) {
    nested |= nt_kind(nt, lefts[i]) == NK_MultiTargetNode;
    if (i < en && el[i] == e) ti = i;
  }
  if (ti < 0) return 1;
  NodeKind k = nt_kind(nt, lefts[ti]);
  /* (a variable whose slot is typed as an Array or a Hash, as a top-level
     instance variable a later `<<` types so, holds no String: no handle goes
     into it) */
  HolderRef h;
  if (holder_kind_of(k) != HK_NONE && holder_of_node(c, lefts[ti], &h) && (ty_is_array(h.r.ty) || ty_is_hash(h.r.ty)))
    return 0;
  if (share_plan_emits_handle(c, e)) return 1;
  int kind_ok = share_plan_emits_handle_kind(c, e);
  /* (an index of a container that holds boxes takes a sequence or a
     conditional as its box, which keeps the handle: masgn_boxed_index_keeps) */
  if (k == NK_IndexTargetNode) return kind_ok || masgn_boxed_index_keeps(c, lefts[ti], e);
  int target = k == NK_LocalVariableTargetNode || k == NK_InstanceVariableTargetNode ||
               k == NK_GlobalVariableTargetNode || k == NK_CallTargetNode;
  return target && (!nested || !subtree_has_side_effect(c, e) || kind_ok);
}

/* Is value v, stored by a consumer in role SPR_STORE, one its target takes
   as a handle (share_plan_multi_renders)? The element of a multiple write
   is found through the parents (the map the plan builds once the seal is
   done: the seal asks share_plan_multi_renders of the write itself). */
static int share_plan_store_renders(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  int n = c->share_parent_n, p = c->share_parent && v >= 0 && v < n ? c->share_parent[v] : -1, e = v;
  while (p >= 0 && p < n && (nt_kind(nt, p) == NK_ParenthesesNode || nt_kind(nt, p) == NK_StatementsNode)) {
    e = p;
    p = c->share_parent[p];
  }
  if (p < 0 || p >= n || nt_kind(nt, p) != NK_ArrayNode) return 1;
  int mw = c->share_parent[p];
  return mw < 0 || mw >= n || nt_ref(nt, mw, "value") != p || share_plan_multi_renders(c, mw, e);
}


/* The plan of value v for a consumer in `role`, v's own need known. */
SharePlan share_plan_with_need(Compiler *c, int v, int role, int need) {
  need = share_plan_role_need(c, v, role, need);
  SharePlan p = { (unsigned char)need, SPS_BYTES, SPF_BYTES };
  if (need == SPN_BYTES) return p;
  p.supply = share_plan_supplies(c, v) && (role != SPR_STORE || share_plan_store_renders(c, v)) ? SPS_HANDLE : SPS_BYTES;
  /* an exception's message asked its identity is the box the exception
     keeps it in (exc_message_identity_operand), which the identity reads
     where the value is read as bytes: no handle is attached to the read,
     so no other consumer has it */
  if (role == SPR_IDENTITY && p.supply == SPS_BYTES && exc_message_identity_operand(c, v)) p.supply = SPS_HANDLE;
  /* (an identity is the handle or the bytes' pointer; a frozen String's
     is its bytes' pointer, which every handle of it names too, and so is
     a call's that answers a literal) */
  int bytes_id = role == SPR_IDENTITY &&
                 (p.supply == SPS_BYTES || share_frozen_string(c, v) || share_plan_call_answers_literal(c, v));
  p.form = (unsigned char)(bytes_id ? SPF_BYTES : share_plan_form(p.need, p.supply));
  return p;
}


/* Does an arm of conditional (or begin, `||`, `&&`) v, or v itself where it
   is a call, answer by a route boxes cannot render (strbuf_box_unrendered)?
   The arms are an `if`, `unless`, `case` and begin's, and both operands of
   `||` and `&&`; share_plan_arms_ok also takes a `case`/`in` and a rescue
   modifier, and the right operand of `&&` only. */
static int share_plan_arm_unrendered(const VpLeaf *lf, void *ud) {
  Compiler *c = ud;
  if (lf->kind == VPK_GAVE_UP) return 1;
  return lf->kind != VPK_NIL && nt_kind(c->nt, lf->node) == NK_CallNode && strbuf_box_unrendered(c, lf->node);
}
static int share_plan_arms_unrendered(Compiler *c, int v) {
  return for_each_value_position(c->nt, v, VP_AND_LEFT | VPS_IF | VPS_UNLESS | VPS_CASE | VPS_BEGIN | VPS_LOGIC,
                                 share_plan_arm_unrendered, c);
}

/* (see share.h) The plan names the handle as the form of a box of v's String
   (SPR_BOX, the object a lift needs), but only where an emitter renders v so:
   a call whose route boxes cannot render (strbuf_box_unrendered, also on an
   arm of a conditional) boxes as it did, from its bytes. */
int share_plan_box_handle(Compiler *c, int v) {
  if (c->share && v >= 0 && strbuf_self_chain_base(c, v) >= 0) return 1;
  if (!c->share || v < 0 || share_plan_arms_unrendered(c, v) ||
      share_plan_with_need(c, v, SPR_BOX, SPN_OBJECT).form != SPF_HANDLE)
    return 0;
  /* ...and a route the emitters take: a value that hands on its handle, or
     a bang or an iterator given a block on a slot (`@s.upcase!`, `@s.tap { }`),
     whose handle form boxes that slot (emit_boxed_bang_self) */
  int u = nt_paren_single(c->nt, v);
  int r = u >= 0 && nt_kind(c->nt, u) == NK_CallNode ? nt_paren_single(c->nt, nt_ref(c->nt, u, "receiver")) : -1;
  return strbuf_value_carries(c, u) ||
         (r >= 0 && nt_kind(c->nt, r) != NK_CallNode && strbuf_bang_self_local(c, u) && strbuf_slot_has(c, r));
}


int share_plan_role(Compiler *c, int v) {
  return c->share_plan && v >= 0 && v < c->share_plan_n ? c->share_plan[v] >> SPM_ROLE_SHIFT : SPR_TAKE;
}


/* Give node n's consumer `role`, and through parentheses and a statement
   list's last statement, the node whose value n is. */
static void share_plan_role_mark(Compiler *c, int n, int role) {
  const NodeTable *nt = c->nt;
  while (n >= 0 && n < c->share_plan_n) {
    c->share_plan[n] = (unsigned char)((c->share_plan[n] & SPM_NEED) | (role << SPM_ROLE_SHIFT));
    NodeKind k = nt_kind(nt, n);
    /* a dynamic send's value is the value of the arm the name selects: the
       arms have the send's consumer */
    if (k == NK_CallNode) {
      int na = 0;
      const int *arms = nt_arr(nt, n, "dyn_send_arms", &na);
      for (int i = 0; i < na; i++)
        if (arms[i] >= 0 && arms[i] < c->share_plan_n)
          c->share_plan[arms[i]] = (unsigned char)((c->share_plan[arms[i]] & SPM_NEED) | (role << SPM_ROLE_SHIFT));
    }
    if (k == NK_ParenthesesNode) n = nt_ref(nt, n, "body");
    else if (k == NK_StatementsNode) {
      int bn = 0;
      const int *bv = nt_arr(nt, n, "body", &bn);
      n = bn > 0 ? bv[bn - 1] : -1;
    }
    else break;
  }
}

/* The elements a multiple write's value v -- an Array literal, and the
   literals splatted in it (`a, b = *[s, t]`) -- stores into its targets. */
static void share_plan_masgn_stores(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  v = nt_paren_single(nt, v);
  if (v >= 0 && nt_kind(nt, v) == NK_SplatNode) v = nt_paren_single(nt, nt_ref(nt, v, "expression"));
  if (v < 0 || nt_kind(nt, v) != NK_ArrayNode) return;
  int en = 0;
  const int *el = nt_arr(nt, v, "elements", &en);
  for (int i = 0; i < en; i++) {
    if (nt_kind(nt, el[i]) == NK_SplatNode) share_plan_masgn_stores(c, el[i]);
    else share_plan_role_mark(c, el[i], SPR_STORE);
  }
}

/* The operands of identity query n (equal?'s receiver and argument,
   object_id's and __id__'s receiver), where no class of the program
   defines its name: a method of the program's own takes them. An arm of
   a dynamic send shares the send's receiver, which the send takes. */
static void share_plan_identity_roles(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, n, "name");
  int recv = nt_ref(nt, n, "receiver"), args = nt_ref(nt, n, "arguments"), np = 0, an = 0;
  if (!nm || recv < 0 || nt_ref(nt, n, "block") >= 0 || !is_identity_query(nm) || is_freeze_family(nm) ||
      nt_int(nt, n, "dyn_arm", 0))
    return;
  int pair = is_equality_name(nm);
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != pair || call_plain_argc(c, n) != pair) return;
  const PolyCand *pc = comp_poly_candidates(c, nm, &np);
  for (int i = 0; i < np; i++)
    if (pc[i].mi >= 0 || pc[i].rdcls >= 0) return;
  share_plan_role_mark(c, recv, SPR_IDENTITY);
  if (pair) share_plan_role_mark(c, av[0], SPR_IDENTITY);
}

/* Is call n a freeze of a String variable's read (a local, an instance
   variable, a global or a constant), with no method of the program's own?
   The String it freezes is the one the variable holds: the C freezes the
   handle the variable's slot keeps (sp_String_freeze), whose flag every
   name of the String reads, and the read of the variable gives the bytes
   the call answers frozen. The variable's value is only read. */
static int share_plan_freeze_read(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, n, "name");
  int recv = nt_paren_single(nt, nt_ref(nt, n, "receiver")), tg[CPT_MAX];
  if (!nm || recv < 0 || !is_freeze_name(nm) || nt_ref(nt, n, "arguments") >= 0 || nt_ref(nt, n, "block") >= 0)
    return 0;
  NodeKind k = nt_kind(nt, recv);
  if (k != NK_LocalVariableReadNode && k != NK_InstanceVariableReadNode && !repr_static_read_kind(k)) return 0;
  return (c->ntype[recv] == TY_STRING || c->ntype[recv] == TY_STRBUF) && cplan_targets(c, n, tg, CPT_MAX) == 0;
}

/* The roles of the receivers that a builtin call only reads: a freeze of
   a variable's String (share_plan_freeze_read), and a blockless String
   iterator whose Enumerator is walked where it is made
   (str_each_enum_walked_here): the walk reads the String as it is there,
   and the Enumerator's source box dies with the expression. One that
   outlives it holds the handle, and its receiver stays a taker. */
static void share_plan_reader_roles(Compiler *c, int n) {
  if (share_plan_freeze_read(c, n) || str_each_enum_walked_here(c, n))
    share_plan_role_mark(c, nt_ref(c->nt, n, "receiver"), SPR_READ);
}

/* The roles other than a taker's, from the consumers that have them: an
   interpolation reads, a predicate tests, a multiple write stores each
   element of its value's literal (and of a literal splatted in it) into
   its target, an identity query asks its operands' identity, and a
   builtin that only reads its receiver reads it. One pass over each kind. */
static void share_plan_roles_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_EmbeddedStatementsNode, e) share_plan_role_mark(c, nt_ref(nt, e, "statements"), SPR_READ);
  static const NodeKind preds[] = { NK_IfNode, NK_UnlessNode, NK_WhileNode, NK_UntilNode };
  for (int k = 0; k < 4; k++)
    NT_FOREACH_KIND(nt, preds[k], n) share_plan_role_mark(c, nt_ref(nt, n, "predicate"), SPR_TEST);
  NT_FOREACH_KIND(nt, NK_MultiWriteNode, mw) share_plan_masgn_stores(c, nt_ref(nt, mw, "value"));
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    share_plan_identity_roles(c, n);
    share_plan_reader_roles(c, n);
  }
}

/* Does builtin call `call` keep or hand on the argument it is given: a
   store into its receiver's elements, a constructor, an ivar store, a proc
   call (its parameter keeps it), Array(); or answer it (2: its consumer
   decides)? A builtin that only reads it (0) is handed its bytes. */
static int share_plan_builtin_keeps(Compiler *c, int call) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, call, "name");
  int recv = nt_ref(nt, call, "receiver");
  if (!nm) return 0;
  TyKind rt = recv >= 0 ? c->ntype[recv] : TY_UNKNOWN;
  TyKind fams[6];
  int nf = 0;
  if (rt == TY_STRING || rt == TY_STRBUF) fams[nf++] = TY_STRING;
  else if (ty_is_array(rt) || ty_is_ptr_array(rt)) fams[nf++] = BOP_ANY_ARRAY;
  else if (ty_is_hash(rt)) fams[nf++] = BOP_ANY_HASH;
  else if (rt == TY_PROC || rt == TY_METHOD || rt == TY_CURRY) fams[nf++] = BOP_CALLABLE;
  else if (recv >= 0 && rt != TY_POLY && rt != TY_UNKNOWN) fams[nf++] = rt;
  else { fams[nf++] = BOP_ANY_ARRAY; fams[nf++] = BOP_ANY_HASH; fams[nf++] = TY_CLASS; fams[nf++] = BOP_CALLABLE; }
  fams[nf++] = BOP_ANY_RECV;
  int keep = 0;
  for (int i = 0; i < nf; i++) {
    switch (bop_share_named(fams[i], nm)) {
    case BSH_STORE_LAST: case BSH_STORE_ALL: case BSH_STORE_TAIL: case BSH_MERGE: case BSH_ARRAY_OF:
    case BSH_CALL: case BSH_IVAR_SET: case BSH_EXEC: case BSH_NEW: case BSH_NEW_FILL: case BSH_NEW_DEFAULT:
    case BSH_NEW_FIELDS: case BSH_FILL: case BSH_SUM:
      return 1;
    case BSH_LAST: case BSH_FETCH:
      keep = 2;
      break;
    default: break;
    }
  }
  return keep;
}

static int share_plan_box_kept(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  int n = c->share_parent_n;
  /* the parents form a tree, so a walk of n steps reaches the root; the
     bound only guards a malformed map, and running out of it answers
     "kept": a handle is never wrong, bytes would be a copy */
  for (int depth = 0; depth < n; depth++) {
    int p = c->share_parent && v >= 0 && v < n ? c->share_parent[v] : -1;
    if (p < 0 || p >= n) return 0;
    const char *pty = nt_type(nt, p);
    /* (no node kinds of their own) an argument list's call, a `when` arm */
    if (is_arguments_node_type(pty)) {
      int call = c->share_parent[p];
      if (call < 0 || call >= n) return 0;
      if (nt_kind(nt, call) != NK_CallNode)
        return nt_kind(nt, call) == NK_SuperNode || nt_kind(nt, call) == NK_YieldNode;
      const CallPlan *cp = cplan_user(c, call);
      if (cp->mi >= 0 || cp->dispatch != CP_NONE) return 1;
      int k = share_plan_builtin_keeps(c, call);
      if (k != 2) return k;
      v = call;
      continue;
    }
    if (is_when_node_type(pty)) { v = p; continue; }
    switch (nt_kind(nt, p)) {
    /* a holder: a container literal, a variable, a constant, a value a
       method, block or jump hands on */
    case NK_ArrayNode: case NK_HashNode: case NK_KeywordHashNode:
    case NK_LocalVariableWriteNode: case NK_InstanceVariableWriteNode: case NK_GlobalVariableWriteNode:
    case NK_ClassVariableWriteNode: case NK_ConstantWriteNode: case NK_ConstantPathWriteNode:
    case NK_LocalVariableOrWriteNode: case NK_InstanceVariableOrWriteNode: case NK_GlobalVariableOrWriteNode:
    case NK_ClassVariableOrWriteNode: case NK_LocalVariableAndWriteNode: case NK_InstanceVariableAndWriteNode:
    case NK_GlobalVariableAndWriteNode: case NK_ClassVariableAndWriteNode: case NK_MultiWriteNode:
    case NK_ReturnNode: case NK_NextNode: case NK_BreakNode: case NK_YieldNode: case NK_SplatNode:
    case NK_DefNode: case NK_BlockNode: case NK_LambdaNode: case NK_OptionalParameterNode:
    case NK_OptionalKeywordParameterNode:
      return 1;
    case NK_AssocNode:
      /* a Hash keeps a value; a String key is copied and frozen */
      if (nt_ref(nt, p, "value") != v) return 0;
      v = p;
      continue;
    /* the value of a conditional, a sequence's last statement, a group:
       its own consumer decides */
    case NK_ParenthesesNode: case NK_IfNode: case NK_UnlessNode: case NK_ElseNode:
    case NK_CaseNode: case NK_InNode: case NK_CaseMatchNode: case NK_AndNode: case NK_OrNode: case NK_BeginNode:
    case NK_RescueModifierNode:
      v = p;
      continue;
    case NK_StatementsNode: {
      int bn = 0;
      const int *bv = nt_arr(nt, p, "body", &bn);
      if (bn <= 0 || bv[bn - 1] != v) return 0;
      v = p;
      continue;
    }
    /* an arm of a dynamic send (dyn_send_arms) is the send's value when
       the name selects it: its consumer decides */
    case NK_CallNode:
      if (!nt_int(nt, v, "dyn_arm", 0)) return 0;
      v = p;
      continue;
    default:
      return 0;
    }
  }
  return 1;
}

void share_plan_free(Compiler *c) {
  free(c->share_parent);
  c->share_parent = NULL;
  c->share_parent_n = 0;
  free(c->share_plan);
  free(c->share_plan_ty);
  free(c->share_sup);
  c->share_plan = NULL;
  c->share_plan_ty = NULL;
  c->share_sup = NULL;
  c->share_plan_n = c->share_sup_n = 0;
}

/* Once per compile, over the node table: each node's need, with the type
   and the table version it was asked under, and its consumer's role. */
void share_plan_build(Compiler *c) {
  share_plan_free(c);
  if (!c->share) return;
  int n = c->nt->count;
  c->share_plan = malloc((size_t)n + 1);
  c->share_plan_ty = malloc(sizeof *c->share_plan_ty * ((size_t)n + 1));
  if (!c->share_plan || !c->share_plan_ty) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int v = 0; v < n; v++) {
    Scope *sc = comp_scope_of(c, v);
    c->share_plan[v] = sc && !sc->reachable ? SPN_ASK : (unsigned char)share_plan_need(c, v);
    c->share_plan_ty[v] = c->ntype[v];
  }
  c->share_plan_n = n;
  c->share_plan_ver = c->nt->version;
  share_plan_roles_build(c);
  c->share_parent = an_parent_map(c->nt);
  if (!c->share_parent) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  c->share_parent_n = n;
}

/* Is codegen where the memo may answer: the node read as itself (no view,
   no scope move, no splice) and no poly dispatch arm open? */
static int share_plan_plain_ctx(void) {
  return cplan_plain_ctx() && g_arm.pd_skip < 0 && g_arm.prbd_skip < 0 && !g_arm.builtin_arm &&
         g_arm.send_split < 0;
}

/* The memo's need of node v where codegen is, or -1 where it cannot answer. */
static int share_plan_memo_need(Compiler *c, int v) {
  int m = c->share_plan && v >= 0 && v < c->share_plan_n && (c->share_plan[v] & SPM_NEED) != SPN_ASK &&
          c->nt->version == c->share_plan_ver && c->ntype[v] == c->share_plan_ty[v] && share_plan_plain_ctx();
  return m ? c->share_plan[v] & SPM_NEED : -1;
}

SharePlan share_plan_of(Compiler *c, int v, int role, int *memo) {
  int m = share_plan_memo_need(c, v);
  if (memo) *memo = m >= 0;
  SharePlan p = share_plan_with_need(c, v, role, m >= 0 ? m : share_plan_need(c, v));
  /* (the producer, where the supply was asked) */
  if (p.need != SPN_BYTES) p.kind = (unsigned char)share_plan_producer(c, v, NULL);
  return p;
}
