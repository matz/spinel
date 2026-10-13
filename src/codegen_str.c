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

/* Node v's object id, in the plan's form: the handle's where the plan
   gives the handle -- strbuf_object_ref names it for each producer that
   has one: a variable's slot, a box, a Range's kept endpoint, a route over
   a handle, the box an exception keeps its message in -- and else the
   bytes' address; a String that can be nil answers nil's id when it is
   nil. An identity is never wrapped or refused. Answers the form it
   rendered: the bytes' where strbuf_object_ref names no handle, as it did
   before the plan was read. */
static int str_render_id(Compiler *c, int v, int form, StrForm *sf) {
  Buf *b = &sf->expr;
  if (form == SPF_HANDLE && strbuf_object_ref(c, v, b)) {
    /* (a String Range's min or max can be a String of its own, which
       sp_srange_extreme_h makes: strbuf_object_ref's arm for it, after the
       exception message's and the endpoints') */
    if (!exc_message_identity_operand(c, v) && !strbuf_route_srange_end(c, v) && strbuf_route_srange_extreme(c, v))
      sf->needs |= SF_ALLOCATES;
    return SPF_HANDLE;
  }
  if (comp_recv_type(c, v) == TY_STRING && repr_of(c, v).may_nil) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t);
    emit_boxed(c, v, b);
    buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? SP_NIL_OBJECT_ID : (sp_int)(uintptr_t)_t%d.v.p; })", t, t);
  }
  else {
    buf_puts(b, "((sp_int)(uintptr_t)(");
    emit_expr(c, v, b);
    buf_puts(b, "))");
  }
  return SPF_BYTES;
}

/* Node v as a box whose identity is the String's, in the plan's form, for
   an identity the consumer compares through sp_poly_equal: where the plan
   gives the handle, the producer's -- the box an exception keeps its
   message in, the box a value already is (a narrowed box's own, a bang on
   a boxed receiver's), the handle a String Range keeps for an endpoint
   (else the String the Range holds), the box a route hands its String on
   in (a proc's answer, a spliced call's tails: emit_strbuf_route_box), a
   call answering its receiver that receiver's (emit_bang_self_handle), and
   any other producer's handle as emit_strbuf_handle_of names it; else the
   bytes, which sp_box_str boxes as the handle that owns them, if one
   does. A route that names no handle where it runs hands on bytes, which
   are then the String's identity: a new handle around them would be
   another object. */
static int str_render_idbox(Compiler *c, int v, int form, StrForm *sf) {
  Buf *b = &sf->expr;
  Repr r = repr_of(c, v);
  if (form != SPF_HANDLE) {
    /* (a value held other than as bytes is boxed as it is held) */
    if (r.as_ty != TY_STRING) emit_boxed(c, v, b);
    else {
      buf_puts(b, "sp_box_str(");
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    return SPF_BYTES;
  }
  if (exc_message_identity_operand(c, v)) emit_exc_message_idbox(c, v, b);
  else if (r.kind == RK_BOXED || r.strbuf_src == RS_SLOT_POLY || face_str_bang_call(c, v) ||
           strbuf_srange_tail(c, v) >= 0) {
    /* (a min or max can be a String of its own, which
       sp_srange_extreme_h makes) */
    int u = strbuf_srange_tail(c, v);
    if (u >= 0 && !strbuf_route_srange_end(c, u)) sf->needs |= SF_ALLOCATES;
    emit_boxed(c, v, b);
  }
  else if (emit_strbuf_route_box(c, v, b)) { }
  else {
    /* a call answering its receiver: the receiver's handle, where the
       bang has a form of its own; a route: the handle it hands on, where
       one reaches it (emit_strbuf_route), and else the bytes it hands on,
       unless the value is the handle itself (a reader or a call under the
       handle demand); a route over a box answers the String the box holds
       (strbuf_route_box_operand), whose read is a plain String's bytes or a
       handle's live buffer, which sp_box_str boxes as that handle: no
       copy, where emit_strbuf_route would make a plain String's box a new
       handle */
    Buf hb;
    memset(&hb, 0, sizeof hb);
    int named = bang_self_handle_has(c, v) ? emit_bang_self_handle(c, v, &hb) : 0;
    int route = !named && share_plan_producer(c, v, NULL) == SPK_ROUTE;
    int bx = route ? strbuf_route_box_operand(c, v) : -1;
    if (bx >= 0) {
      free(hb.p);
      size_t at = b->len;
      buf_puts(b, "sp_box_str(");
      emit_expr(c, v, b);
      buf_puts(b, ")");
      if (g_share_check) {
        share_check_declare_reads(b, at, v);
        share_check_declare_reads(b, at, bx);
      }
      return SPF_HANDLE;
    }
    if (route) named = emit_strbuf_route(c, v, &hb);
    if (route && !named && r.as_ty != TY_STRBUF && !r.demand && !strbuf_slot_has(c, v)) {
      free(hb.p);
      buf_puts(b, "sp_box_str(");
      emit_expr(c, v, b);
      buf_puts(b, ")");
      return SPF_BYTES;
    }
    if (!named) emit_strbuf_handle_of(c, v, &hb);
    buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", hb.p ? hb.p : "NULL");
    free(hb.p);
  }
  return SPF_HANDLE;
}

int emit_str_form(Compiler *c, int v, int role, int face, StrForm *sf) {
  memset(sf, 0, sizeof *sf);
  /* (the plan exists where share_plan_build made it: under the flag, once
     the share facts are built) */
  int form = c->share_plan ? share_plan_of(c, v, role, NULL).form : SPF_HANDLE;
  switch (face) {
  case SPX_ID:
    sf->cty = TY_INT;
    return str_render_id(c, v, form, sf);
  case SPX_BOX:
    sf->cty = TY_POLY;
    return str_render_idbox(c, v, form, sf);
  }
  return form;
}

/* Is v an operand whose identity is a String's: typed a String, or an
   exception's message asked its identity? */
static int str_identity_operand(Compiler *c, int v) {
  TyKind t = comp_recv_type(c, v);
  return t == TY_STRING || t == TY_STRBUF || exc_message_identity_operand(c, v);
}

/* Operand v of an identity comparison, boxed: a String through the plan,
   any other value as the box it is held in. */
static void str_identity_box(Compiler *c, int v, StrForm *sf) {
  if (str_identity_operand(c, v)) {
    emit_str_form(c, v, SPR_IDENTITY, SPX_BOX, sf);
    return;
  }
  Buf bx;
  memset(&bx, 0, sizeof bx);
  emit_boxed(c, v, &bx);
  str_form_of_text(sf, bx.p ? bx.p : "sp_box_nil()", TY_POLY, 0);
  free(bx.p);
}

/* --share-strings: `x.equal?(y)` where a side is a String: both operands
   in the plan's form for an identity (SPX_BOX), compared by sp_poly_equal,
   which reads a frozen literal's handle as its bytes. It answers for a
   String receiver whose equal? is the builtin, and for any other receiver
   where no class of the program defines equal? (the plan's identity role).
   The receiver runs first, bound once, and stays rooted while the argument
   runs where that may allocate: the argument's own evaluation
   (operand_may_allocate) or its form (SF_ALLOCATES). A `&.` call is
   asked again past its nil guard. 0, with nothing emitted, for any other
   call. */
int emit_str_equal(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver"), argc = 0, tg[CPT_MAX];
  if (!c->share_plan || !nm || !is_equal_name(nm) || recv < 0 || nt_ref(nt, id, "block") >= 0 ||
      sn_guard_pending(c, id))
    return 0;
  const int *argv = call_args(nt, id, &argc);
  if (argc != 1 || call_has_splat_arg(nt, argv, argc)) return 0;
  TyKind rt = comp_recv_type(c, recv);
  int builtin = (rt == TY_STRING || rt == TY_STRBUF) && cplan_targets(c, id, tg, CPT_MAX) == 0;
  if (!builtin && !(share_plan_role(c, recv) == SPR_IDENTITY &&
                    (str_identity_operand(c, recv) || str_identity_operand(c, argv[0]))))
    return 0;
  StrForm x, y;
  str_identity_box(c, recv, &x);
  str_identity_box(c, argv[0], &y);
  x.needs |= SF_EVAL_ONCE;
  if (operand_may_allocate(c, argv[0]) || (y.needs & SF_ALLOCATES)) x.needs |= SF_ROOT_UNTIL_USE;
  str_form_settle(c, &x, b);
  buf_printf(b, "sp_poly_equal(_t%d, ", x.tmp);
  str_form_settle(c, &y, b);
  buf_puts(b, ")");
  str_form_close(&x, b);
  str_form_free(&x);
  str_form_free(&y);
  /* (the operands run in order ahead of one runtime call, which cannot
     raise: a boxed receiver's call needs no head, nil_target_runs_in_call) */
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
                        consumer writes and str_form_close ends.
   A form that needs both binds the rooted assignment in its temp -- a box
   (TY_POLY), which has no NULL to be declared with ahead, is rooted where
   its temp is bound, as a consumer that held an operand ahead of the next
   one wrote it -- and one that needs neither is its expression.
   SF_ALLOCATES asks nothing of the form itself: it is for the values live
   across it. */
void str_form_settle(Compiler *c, StrForm *sf, Buf *b) {
  const char *e = sf->expr.p ? sf->expr.p : "";
  Buf rb;
  memset(&rb, 0, sizeof rb);
  int box_root = (sf->needs & SF_ROOT_UNTIL_USE) && (sf->needs & SF_EVAL_ONCE) && sf->cty == TY_POLY;
  if ((sf->needs & SF_ROOT_UNTIL_USE) && !box_root) {
    emit_rooted_conversion(c, sf->cty, e, &rb);
    e = rb.p ? rb.p : "";
  }
  if (sf->needs & SF_EVAL_ONCE) {
    Buf ct;
    memset(&ct, 0, sizeof ct);
    emit_ctype(c, sf->cty, &ct);
    const char *t = ct.p ? ct.p : "void";
    size_t n = strlen(t);
    sf->tmp = ++g_tmp;
    /* (a pointer type's star stays with the name: `sp_String *_tN`) */
    buf_printf(b, "({ %s%s_t%d = %s; ", t, n && t[n - 1] == '*' ? "" : " ", sf->tmp, e);
    if (box_root) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", sf->tmp);
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
