/* codegen_str.c -- a String value rendered in the form its plan gives (#8328).

   The String plan (share_plan.c) says, for a value and the role of the
   consumer that takes it, whether that consumer gets the String's handle,
   its bytes, or its bytes wrapped into a new handle. emit_str_form is the
   one place a consumer that reads the plan asks for the value: it names its
   role (SPR_*) and the face it holds the value in (SPX_*, codegen_internal.h),
   and gets the C expression of the plan's form in that face. Consumers
   move to it one family at a time; the others still decide on their own.

   A form says what its expression needs from where it is used (StrForm's
   needs: to be evaluated once, to stay rooted until its use) rather than
   holding or rooting it itself, and str_form_settle meets those needs.
   Where the value is evaluated in the consumer's order, and what holds it
   while other operands run, stays with the consumer.

   Without --share-strings there is no plan, and the producer decides as it
   did: a String the default build keeps as a shared handle (#3227) is that
   handle where its renderer names one. */

#include "codegen_internal.h"
#include "call_plan.h"
#include "codegen_call_arms.h"
#include "repr.h"
#include "share.h"

/* Is node v a read of a variable (a local, an ivar, a global, a class
   variable or a constant), which hands over the handle its slot holds? */
int str_var_read(Compiler *c, int v) {
  NodeKind k = nt_kind(c->nt, v);
  return k == NK_LocalVariableReadNode || k == NK_InstanceVariableReadNode || repr_static_read_kind(k);
}

/* Is node v `+"lit"`, a String literal's mutable copy? */
static int str_uplus_literal(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, v) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, v, "name");
  int r = nt_ref(nt, v, "receiver");
  return nm && is_unary_plus(nm) && nt_ref(nt, v, "arguments") < 0 && r >= 0 && nt_kind(nt, r) == NK_StringNode;
}

/* The handle arms of str_render_handle, each answering 1 with the handle
   rendered, or 0 with nothing emitted: a value that ran ahead of the
   binding (the handle it took there); a route over a handle; a variable's
   slot. */
static int str_handle_ran_first(Compiler *c, int v, StrForm *sf) {
  int ran = arg_ran_first(v, 0), late = ran && str_var_read(c, v);
  if (ran_first_handle(v) < 0 || (!late && !(ran))) return 0;
  buf_printf(&sf->expr, "_t%d", ran_first_handle(v));
  return 1;
}
static int str_handle_route(Compiler *c, int v, StrForm *sf) {
  if (arg_ran_first(v, 0) || str_var_read(c, v) || repr_of(c, v).as_ty != TY_STRING || !strbuf_arg_carries(c, v)) return 0;
  emit_strbuf_handle_of(c, v, &sf->expr);
  return 1;
}
static int str_handle_slot(Compiler *c, int v, StrForm *sf) {
  /* (one-String mode: a variable read that ran first holds the handle, or
     a view of it, in its temp: the String the variable had) */
  if (arg_ran_first(v, 0) && str_var_read(c, v))
    for (int i = g_n_argov - 1; i >= 0; i--)
      if (g_argov_node[i] == v) { buf_printf(&sf->expr, "SP_STR_HDL(%s)", g_argov_text[i]); return 1; }
  if ((arg_ran_first(v, 0) && str_var_read(c, v)) || !strbuf_slot_text(c, v, &sf->expr)) return 0;
  return 1;
}

/* ...and the new handle made of a value's bytes, which no other name sees
   (a box's: the handle it holds where the form is the handle) */
static int str_handle_new(Compiler *c, int v, int form, unsigned dest, StrForm *sf) {
  Buf *b = &sf->expr;
  NodeKind k = nt_kind(c->nt, v);
  int own = (dest & SFD_OWN) && k != NK_LocalVariableReadNode && k != NK_InstanceVariableReadNode;
  if (!arg_ran_first(v, 0) && (dest & SFD_OWN) && str_uplus_literal(c, v)) {
    buf_puts(b, "sp_String_new_unfrozen(");
    emit_expr(c, nt_ref(c->nt, v, "receiver"), b);
    buf_puts(b, ")");
    sf->needs |= SF_ALLOCATES;
    return SPF_HANDLE;
  }
  /* (a nil that is no bare read -- a call answering nil -- runs first) */
  if (comp_ntype(c, v) == TY_NIL) {
    if (subtree_is_pure_read(c, v)) buf_puts(b, "NULL");
    else { buf_puts(b, "({ (void)("); emit_expr_for_effect(c, v, b); buf_puts(b, "); NULL; })"); }
    return SPF_BYTES;
  }
  if (repr_of(c, v).kind == RK_BOXED) {
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    t = hold_operand(c, v, TY_POLY, 0, t, 1, " ", b);
    /* (a box that holds a String's handle is that handle; a String's own
       box, or anything else, is converted as the slot takes it) */
    if (form == SPF_HANDLE)
      buf_printf(b, "sp_poly_nil_p(_t%d) ? NULL : sp_poly_is_strbuf(_t%d) ? sp_poly_as_strbuf(_t%d) : ", t, t, t);
    else buf_printf(b, "sp_poly_nil_p(_t%d) ? NULL : ", t);
    buf_printf(b, "%s(sp_poly_arg_str_chk(_t%d)); })", own ? "sp_String_new_fresh" : "sp_String_new_shared", t);
    sf->needs |= SF_ALLOCATES;
    return SPF_HANDLE;
  }
  if (own) {
    buf_puts(b, "sp_String_new_fresh(");
    emit_str_expr(c, v, b);
    buf_puts(b, ")");
  }
  else {
    Buf e; memset(&e, 0, sizeof e);
    emit_str_expr(c, v, &e);
    strbuf_new_of_text(c, v, e.p, b);
    free(e.p);
  }
  sf->needs |= SF_ALLOCATES;
  return SPF_HANDLE;
}

/* Node v as the handle a handle slot takes (a method's parameter that is
   the shared handle), in the plan's form. Where the plan gives the handle,
   or gives no form (without --share-strings, or where the value's own
   need is no more than its bytes or a new handle), the producer's handle
   is handed over where it has one, in this order:
     - a local buffer's write: that local's buffer;
     - a value that ran ahead of the binding (arg_ran_first): the handle it
       took there (ran_first_handle), a variable's at its read, so a later
       argument that writes the variable does not change what is bound;
     - a route over a handle (strbuf_arg_carries): emit_strbuf_handle_of;
     - a variable that holds the handle: its slot;
   and else a new handle is made of its bytes, which no other name sees
   (str_handle_new):
     - `+"lit"` into a slot that holds its own copy (SFD_OWN): straight
       from the literal;
     - nil: the NULL handle a nullable slot reads as nil
       (`def initialize(o: nil)`);
     - a box: the NULL handle where it holds nil (converted as a String
       first, it raised TypeError where CRuby binds nil), else a handle of
       its String; where the plan gives the handle, a box that holds a
       String's handle hands over that handle (a box of a String that can
       change holds its handle), which a copy of its bytes would fork;
     - any other value: a handle made of its bytes (SF_ALLOCATES), with the
       bytes inside the object for a slot that holds its own copy.
   Under --share-strings the producer's kind (share_plan_producer) picks
   the arms instead, one render per kind (str_handle_by_kind). A form the
   plan gives the handle that no arm hands over (a renderer gap the seal
   did not see, under a view or an arm) is the new handle, as before the
   plan was read; --share-check reports it. Answers the form rendered. */
static int str_handle_by_kind(Compiler *c, int v, int form, unsigned dest, StrForm *sf);
static int str_render_handle(Compiler *c, int v, int form, unsigned dest, StrForm *sf) {
  sf->cty = TY_STRBUF;
  if (!arg_ran_first(v, 0) && repr_native_call_handle(c, v)) {
    emit_native_string_handle(c, v, &sf->expr);
    return SPF_HANDLE;
  }
  return str_handle_by_kind(c, v, form, dest, sf);
}

/* --share-strings: the handle face's render of each producer kind
   (share.h's SPK_*), asked where the value is bound:
     NONE, FROZEN   a new handle of the bytes, which for a frozen literal
                    is the literal's own (sp_String_new_shared), after a
                    value that ran first, which holds what it took;
     BOX            the box's handle or a handle of its String (a box
                    that ran first is the handle it took);
     any other      the handle the producer hands on: a local buffer's
                    write, a value that ran first, a route over a handle,
                    a variable's slot; a kind none of these renders (a
                    renderer gap) is the new handle. */
static int str_handle_by_kind(Compiler *c, int v, int form, unsigned dest, StrForm *sf) {
  int kind = share_plan_producer(c, v, NULL);
  _Static_assert(SPK__COUNT == 23, "a handle render for each producer kind");
  switch (kind) {
  case SPK_NONE: case SPK_FROZEN: case SPK_BOX:
    if (str_handle_ran_first(c, v, sf)) return SPF_HANDLE;
    return str_handle_new(c, v, form, dest, sf);
  case SPK_SLOT: case SPK_DEMANDED: case SPK_ELEM: case SPK_SLOT_POLY: case SPK_SELF: case SPK_BANG_BOX:
  case SPK_PROC_CALL: case SPK_POLY_ARMS: case SPK_EXC_MESSAGE: case SPK_EXC_SUPER: case SPK_NATIVE_CALL:
  case SPK_READER: case SPK_WRITE: case SPK_ASSIGN: case SPK_OPERAND: case SPK_BLOCK_VALUE: case SPK_YIELD:
  case SPK_COND: case SPK_SEQUENCE: case SPK_LIFT:
    if (emit_strbuf_local_write_handle(c, v, &sf->expr)) return SPF_HANDLE;
    if (str_handle_ran_first(c, v, sf) || str_handle_route(c, v, sf) || str_handle_slot(c, v, sf)) return SPF_HANDLE;
    return str_handle_new(c, v, form, dest, sf);
  }
  return str_handle_new(c, v, form, dest, sf);
}

/* The box of a String handle (SP_BUILTIN_STRBUF): the one place its text is
   written. A box of a handle that can be NULL is nullable (a NULL boxes as
   nil); the other wraps what it is given as it is (an object that is never
   nil). The handle's own text is the caller's: str_box_handle boxes text
   already written, str_box_handle_tmp a temp, and str_box_open /
   str_box_close wrap text the caller emits between them. */
/* SP_STR_BOX accepts a handle or its byte view and boxes the handle. */
void str_box_open(Buf *b, int nullable) {
  (void)nullable;
  buf_puts(b, "SP_STR_BOX(");
}
void str_box_close(Buf *b) {
  buf_puts(b, ")");
}
void str_box_handle(Buf *b, const char *h) {
  str_box_open(b, 1);
  buf_puts(b, h ? h : "NULL");
  str_box_close(b);
}
void str_box_handle_tmp(Buf *b, int tmp) {
  str_box_open(b, 1);
  buf_printf(b, "_t%d", tmp);
  str_box_close(b);
}

/* A read held across a block borrows a snapshot. */
static const char *str_read_face(void) {
  return g_one_held ? "sp_strbuf_read_own" : "sp_strbuf_read";
}
void str_read_handle(Buf *b, const char *h) {
  buf_printf(b, "%s(%s)", str_read_face(), h ? h : "NULL");
}
void str_read_handle_tmp(Buf *b, int tmp) {
  buf_printf(b, "%s(_t%d)", str_read_face(), tmp);
}
/* ...and around the handle's text the caller emits between the open and the
   close, as the box of a handle and a new handle are written around theirs
   (a return tail reads the handle that an emitter renders in place). */
void str_read_handle_open(Buf *b) {
  buf_printf(b, "%s(", str_read_face());
}
void str_read_handle_close(Buf *b) {
  buf_puts(b, ")");
}

/* A new handle around bytes no handle holds: the one place the text of its
   making is written, around the bytes the caller emits between the open
   and the close. It is the shared handle of a String that nothing else
   names (sp_String_new_shared), as a route hands on where the String it
   converts has no handle. */
void str_new_handle_open(Buf *b) {
  /* (one representation: the operand may be of either face, SP_STR_HDL) */
  buf_puts(b, "SP_STR_HDL(");
}
void str_new_handle_close(Buf *b) {
  buf_puts(b, ")");
}

/* The box of a String Range's endpoint held in temp `t` (an sp_StrRange):
   the handle the Range keeps for it (field `h`), else the String it holds
   (field `e`) as bytes, else nil for an open end. */
void str_box_kept(Buf *b, int t, const char *h, const char *e) {
  char kept[48];
  snprintf(kept, sizeof kept, "_t%d.%s", t, h);
  buf_printf(b, "%s ? ", kept);
  str_box_handle(b, kept);
  buf_printf(b, " : _t%d.%s ? sp_box_str_new(_t%d.%s) : sp_box_nil()", t, e, t, e);
}

/* The box face of a String value by its representation (emit_boxed_impl's
   TY_STRING arm): SP_STR_BOX of the value. A value that may allocate (a
   call's answer, a new String) is boxed into a rooted temp first: the box
   can go to a runtime call that allocates (an Array push, a Hash lookup)
   while it is held by no root. The temp is named ahead of the operand's
   own temps (StrForm.tmp), so the operand's keep their numbers. */
void emit_str_box_rooted(Compiler *c, int v, Buf *b) {
  if (!operand_may_allocate(c, v)) {
    buf_puts(b, "SP_STR_BOX(");
    emit_expr(c, v, b);
    buf_puts(b, ")");
    return;
  }
  StrForm sf;
  memset(&sf, 0, sizeof sf);
  sf.rf = -1;
  sf.cty = TY_POLY;
  sf.tmp = ++g_tmp;
  sf.needs = SF_EVAL_ONCE | SF_ROOT_UNTIL_USE | SF_ALLOCATES;
  buf_puts(&sf.expr, "SP_STR_BOX(");
  emit_expr(c, v, &sf.expr);
  buf_puts(&sf.expr, ")");
  str_form_settle(c, &sf, b);
  buf_printf(b, "_t%d", sf.tmp);
  str_form_close(&sf, b);
  str_form_free(&sf);
}

/* Record the box form (RF_*, repr.h) the box face wrote and why it may
   differ from the one repr_of predicts for the node (RW_*). */
static void str_box_note(StrForm *sf, int rf, int rw) {
  sf->rf = rf;
  sf->rw = rw;
}

/* --share-strings: the box face of a String value (or one the rule
   holds as the handle), by its producer's kind (share.h's SPK_*), asked
   where it is boxed; 0 with nothing emitted for any other value:
     NONE, FROZEN  the value's box by its representation, or the handle
                   already held by an earlier store operand;
     BOX           the box itself, by its representation (or as above);
     PROC_CALL     a proc's answer: the box the proc hands back, the call
                   emitted as a box (a catch's or a reopened method's
                   answer: the route arms);
     any other     the box of the handle the producer hands on, by the
                   route arms, then the representation's (a variable's
                   slot, a demanded read, a boxed element, a narrowed box,
                   a bang off a handle, a box's to_s, a chain). */
static int str_box_by_kind(Compiler *c, int node, StrForm *sf) {
  Buf *b = &sf->expr;
  if (!arg_ran_first(node, 0) && repr_native_call_handle(c, node)) {
    str_box_open(b, 1);
    emit_native_string_handle(c, node, b);
    str_box_close(b);
    str_box_note(sf, RF_STRBUF_HANDLE, RW_HANDLE_ROUTE);
    return 1;
  }
  TyKind t = comp_ntype(c, node);
  if (t != TY_STRING && t != TY_STRBUF) return 0;
  int kind = share_plan_producer(c, node, NULL);
  _Static_assert(SPK__COUNT == 23, "a box render for each producer kind");
  switch (kind) {
  case SPK_NONE: case SPK_FROZEN: case SPK_BOX:
    if (!emit_boxed_lift_ran_first(c, node, b)) emit_boxed_by_repr(c, node, b);
    return 1;
  case SPK_PROC_CALL:
    if (strbuf_proc_call_box(c, node) && !arg_ran_first(node, 0) && !emit_boxed_lift(c, node)) {
      /* (a block literal the call splices answers its tail boxed, as
         emit_strbuf_route's proc arm asks it) */
      int v = nt_paren_single(c->nt, node), sv = view_push(c, v, TY_POLY);
      if (is_block_call(c, v)) emit_block_invoke(c, v, b, 0, 1, TY_POLY);
      else emit_expr(c, v, b);
      view_pop(c, sv);
      str_box_note(sf, RF_PASS, RW_HANDLE_ROUTE);
      return 1;
    }
    if (!arg_ran_first(node, 0) && !emit_boxed_lift(c, node) && emit_strbuf_route_box(c, node, b)) {
      str_box_note(sf, RF_PASS, RW_HANDLE_ROUTE);
      return 1;
    }
    return 0;
  /* a call with a literal block that the handle route splices (a yielding
     call, `then`) and whose block's value the handle face renders
     (a tail that is a write of a handle, a `next` of one): boxed as that
     handle, as emit_strbuf_route reads the call, where the call's bytes
     boxed whole were a const char * in a handle's place */
  case SPK_BLOCK_VALUE:
    if (nt_kind(c->nt, nt_paren_single(c->nt, node)) == NK_CallNode && nt_ref(c->nt, nt_paren_single(c->nt, node), "block") >= 0 &&
        !arg_ran_first(node, 0) && !emit_boxed_lift(c, node) &&
        share_plan_of(c, node, SPR_BOX).form == SPF_HANDLE) {
      Buf hb; memset(&hb, 0, sizeof hb);
      if (emit_strbuf_route(c, node, &hb)) {
        str_box_handle(b, hb.p);
        free(hb.p);
        str_box_note(sf, RF_STRBUF_HANDLE, RW_HANDLE_ROUTE);
        return 1;
      }
      free(hb.p);
    }
    return 0;
  /* a write whose slot holds the rule's handle (an ivar's, a global's, a
     class variable's, an `||=`): the write runs, and its box is the handle
     it stored */
  case SPK_WRITE:
    if (!arg_ran_first(node, 0) && !emit_boxed_lift(c, node) && strbuf_value_carries(c, node)) {
      str_box_open(b, 1);
      emit_strbuf_handle_of(c, node, b);
      str_box_close(b);
      str_box_note(sf, RF_STRBUF_HANDLE, RW_HANDLE_ROUTE);
      return 1;
    }
    return 0;
  /* A call with a known handle result boxes that object. */
  case SPK_NATIVE_CALL:
    if (!arg_ran_first(node, 0) && !emit_boxed_lift(c, node)) {
      emit_string_call_handle(c, nt_paren_single(c->nt, node), 1, b);
      str_box_note(sf, RF_STRBUF_HANDLE, RW_HANDLE_ROUTE);
      return 1;
    }
    return 0;
  case SPK_SLOT: case SPK_DEMANDED: case SPK_ELEM: case SPK_SLOT_POLY: case SPK_SELF: case SPK_BANG_BOX:
  case SPK_POLY_ARMS: case SPK_EXC_MESSAGE: case SPK_EXC_SUPER: case SPK_READER:
  /* a yield that calls the block's proc: the box the proc answered */
  case SPK_YIELD:
    if (!strbuf_yield_proc_box(c, node)) return 0;
    {
      int v = nt_paren_single(c->nt, node), sv = view_push(c, v, TY_POLY);
      emit_expr(c, v, b);
      view_pop(c, sv);
      str_box_note(sf, RF_PASS, RW_HANDLE_ROUTE);
    }
    return 1;
  case SPK_OPERAND:
  case SPK_ASSIGN: case SPK_COND: case SPK_SEQUENCE:
  case SPK_LIFT:
    return 0;
  }
  return 0;
}

int emit_str_form(Compiler *c, int v, int role, int face, unsigned dest, StrForm *sf) {
  memset(sf, 0, sizeof *sf);
  sf->rf = -1;
  /* (the box face asks the plan arm by arm, by the producer's kind) */
  if (face == SPX_BOX) {
    sf->cty = TY_POLY;
    return str_box_by_kind(c, v, sf) ? 0 : -1;
  }
  int form = share_plan_of(c, v, role).form;
  switch (face) {
  case SPX_HANDLE:
    return str_render_handle(c, v, form, dest, sf);
  }
  return form;
}

/* Node v into a slot that holds the shared handle, rendered whole: the
   handle face with the taker's role, its needs met. */
void emit_str_form_handle(Compiler *c, int v, Buf *b) {
  StrForm sf;
  emit_str_form(c, v, SPR_TAKE, SPX_HANDLE, 0, &sf);
  str_form_settle(c, &sf, b);
  str_form_free(&sf);
}

/* Is v an operand whose identity is a String's: typed a String, or an
   exception's message asked its identity? */
static int str_identity_operand(Compiler *c, int v) {
  TyKind t = comp_recv_type(c, v);
  return t == TY_STRING || t == TY_STRBUF || exc_message_identity_operand(c, v);
}

/* The call plan has selected the builtin identity comparison. */
int emit_str_equal(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), argc = 0, tg[CPT_MAX];
  if (!nm || !is_equal_name(nm) || recv < 0 || nt_ref(nt, id, "block") >= 0 ||
      sn_guard_pending(c, id))
    return 0;
  const int *argv = call_args(nt, id, &argc);
  if (argc != 1 || nt_argv_has_splat(nt, argv, argc)) return 0;
  TyKind rt = comp_recv_type(c, recv);
  int builtin = (rt == TY_STRING || rt == TY_STRBUF) && cplan_targets(c, id, tg, CPT_MAX) == 0;
  if (!builtin) {
    if (nt_int(nt, id, "dyn_arm", 0) || call_plain_argc(c, id) != 1 ||
        (!str_identity_operand(c, recv) && !str_identity_operand(c, argv[0]))) return 0;
    int count = 0;
    const PolyCand *candidates = comp_poly_candidates(c, nm, &count);
    for (int i = 0; i < count; i++)
      if (candidates[i].mi >= 0 || candidates[i].rdcls >= 0) return 0;
  }
  emit_value_identity(c, recv, argv[0], b);
  c->args_in_call = recv;
  return 1;
}

/* The seam between the String forms and the schedule: the one place a
   form's needs are met. Evaluation order, holds and roots are the schedule
   layer's (#8328), which is to replace this function; until it does, the
   needs are met with the helpers the sites used before they stated them,
   so the C is the one those sites wrote:
     SF_ROOT_UNTIL_USE  emit_rooted_conversion: a temp declared NULL and
                        rooted ahead of the statement (g_pre), assigned
                        where the value stands, so its evaluation does not
                        move;
     SF_EVAL_ONCE       a temp of the value's type bound in a statement
                        expression, `({ T _tN = <expr>; `, whose uses the
                        consumer writes and str_form_close ends; N is the
                        number the renderer reserved in sf->tmp, else the
                        next one at this point.
   A form that needs both is rooted where its temp is bound, as the
   consumers that held an operand ahead of the next one wrote it: a box has
   no NULL to be declared with ahead of the statement, and an expression
   need not have a statement ahead of it (g_pre). One that needs neither
   is its expression. SF_ALLOCATES asks nothing of the form itself: it is
   for the values live across it. */
void str_form_settle(Compiler *c, StrForm *sf, Buf *b) {
  const char *e = sf->expr.p ? sf->expr.p : "";
  Buf rb;
  memset(&rb, 0, sizeof rb);
  int bound_root = (sf->needs & SF_ROOT_UNTIL_USE) && (sf->needs & SF_EVAL_ONCE);
  if ((sf->needs & SF_ROOT_UNTIL_USE) && !bound_root) {
    emit_rooted_conversion(c, sf->cty, e, &rb);
    e = rb.p ? rb.p : "";
  }
  if (sf->needs & SF_EVAL_ONCE) {
    Buf ct;
    memset(&ct, 0, sizeof ct);
    emit_ctype(c, sf->cty, &ct);
    const char *t = ct.p ? ct.p : "void";
    size_t n = strlen(t);
    if (!sf->tmp) sf->tmp = ++g_tmp;
    /* (a pointer type's star stays with the name: `sp_String *_tN`) */
    buf_printf(b, "({ %s%s_t%d = %s; ", t, n && t[n - 1] == '*' ? "" : " ", sf->tmp, e);
    if (bound_root) buf_printf(b, sf->cty == TY_POLY ? "SP_GC_ROOT_RBVAL(_t%d); " : "SP_GC_ROOT(_t%d); ", sf->tmp);
    free(ct.p);
  }
  else buf_puts(b, e);
  free(rb.p);
}

void str_form_of_text(StrForm *sf, const char *expr, TyKind cty, unsigned needs) {
  memset(sf, 0, sizeof *sf);
  buf_puts(&sf->expr, expr);
  sf->cty = cty;
  sf->needs = needs;
}

void str_form_close(const StrForm *sf, Buf *b) {
  if (sf->needs & SF_EVAL_ONCE) buf_puts(b, "; })");
}

void str_form_free(StrForm *sf) {
  free(sf->expr.p);
  memset(sf, 0, sizeof *sf);
}
