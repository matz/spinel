/* Render the String object a native slot takes and the byte views readers use. */

#include "codegen_internal.h"
#include "call_plan.h"
#include "codegen_call_arms.h"
#include "repr.h"

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

/* Each helper answers 1 with the handle
   rendered, or 0 with nothing emitted: a value that ran ahead of the
   binding (the handle it took there); a route over a handle; a variable's
   slot. */
static int str_handle_ran_first(Compiler *c, int v, Buf *b) {
  int ran = arg_ran_first(v, 0), late = ran && str_var_read(c, v);
  if (ran_first_handle(v) < 0 || (!late && !(ran))) return 0;
  buf_printf(b, "_t%d", ran_first_handle(v));
  return 1;
}
static int str_handle_route(Compiler *c, int v, Buf *b) {
  if (arg_ran_first(v, 0) || str_var_read(c, v) || repr_of(c, v).as_ty != TY_STRING || !strbuf_arg_carries(c, v)) return 0;
  emit_strbuf_handle_of(c, v, b);
  return 1;
}
static int str_handle_slot(Compiler *c, int v, Buf *b) {
  /* (one-String mode: a variable read that ran first holds the handle, or
     a view of it, in its temp: the String the variable had) */
  if (arg_ran_first(v, 0) && str_var_read(c, v))
    for (int i = g_n_argov - 1; i >= 0; i--)
      if (g_argov_node[i] == v) { buf_printf(b, "SP_STR_HDL(%s)", g_argov_text[i]); return 1; }
  if ((arg_ran_first(v, 0) && str_var_read(c, v)) || !strbuf_slot_text(c, v, b)) return 0;
  return 1;
}

/* A boxed value carries the String object. For a byte producer, take
   the existing payload owner or materialize the new String it answers. */
static void str_handle_value(Compiler *c, int v, unsigned dest, Buf *b) {
  NodeKind k = nt_kind(c->nt, v);
  int own = (dest & SFD_OWN) && k != NK_LocalVariableReadNode && k != NK_InstanceVariableReadNode;
  if (!arg_ran_first(v, 0) && (dest & SFD_OWN) && str_uplus_literal(c, v)) {
    buf_puts(b, "sp_String_new_unfrozen(");
    emit_expr(c, nt_ref(c->nt, v, "receiver"), b);
    buf_puts(b, ")");
    return;
  }
  /* (a nil that is no bare read -- a call answering nil -- runs first) */
  if (comp_ntype(c, v) == TY_NIL) {
    if (subtree_is_pure_read(c, v)) buf_puts(b, "NULL");
    else { buf_puts(b, "({ (void)("); emit_expr_for_effect(c, v, b); buf_puts(b, "); NULL; })"); }
    return;
  }
  if (repr_of(c, v).kind == RK_BOXED) {
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    t = hold_operand(c, v, TY_POLY, 0, t, 1, " ", b);
    buf_printf(b, "sp_poly_nil_p(_t%d) ? NULL : sp_poly_arg_str_handle(_t%d); })", t, t);
    return;
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

/* The box face of a String value by its representation (emit_boxed_impl's
   TY_STRING arm): SP_STR_BOX of the value. A value that may allocate (a
   call's answer, a new String) is boxed into a rooted temp first: the box
   can go to a runtime call that allocates (an Array push, a Hash lookup).
   Reserve the temp before emitting the operand. */
void emit_str_box_rooted(Compiler *c, int v, Buf *b) {
  int temp = operand_may_allocate(c, v) ? ++g_tmp : 0;
  if (temp) buf_printf(b, "({ sp_RbVal _t%d = ", temp);
  buf_puts(b, "SP_STR_BOX(");
  emit_expr(c, v, b);
  buf_puts(b, ")");
  if (temp) buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); _t%d; })", temp, temp);
}

void emit_str_handle_form(Compiler *c, int v, unsigned dest, Buf *b) {
  if (!arg_ran_first(v, 0) && repr_native_call_handle(c, v)) {
    emit_native_string_handle(c, v, b);
    return;
  }
  if (emit_strbuf_local_write_handle(c, v, b) || str_handle_ran_first(c, v, b)) return;
  if (repr_of(c, v).kind != RK_BOXED &&
      (str_handle_route(c, v, b) || str_handle_slot(c, v, b))) return;
  str_handle_value(c, v, dest, b);
}

void emit_str_form_handle(Compiler *c, int v, Buf *b) {
  emit_str_handle_form(c, v, 0, b);
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
