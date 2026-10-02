/* codegen_ops.c -- the emitters behind builtin_ops rows.

   emit_builtin_op looks the call up in the builtin-op table and runs the
   row's emitter. It sits where the receiver families it covers sat in
   emit_call_body, so the chain above it still claims what it claimed. */

#include "codegen_internal.h"
#include "builtin_ops.h"

/* The receiver's C text, emitted once. A row names it as $r, possibly
   more than once; every $r prints the same text, as the arms the rows
   replace did. */
static char *op_recv_text(Compiler *c, const BopCtx *x) {
  Buf r; memset(&r, 0, sizeof r);
  emit_expr(c, x->recv, &r);
  if (!r.p) return strdup("");
  return r.p;
}

static int emit_op_template(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  for (const char *p = x->op->arg; *p; p++) {
    if (p[0] == '$' && p[1] == 'r') { buf_puts(b, r); p++; }
    else { char ch[2] = { *p, 0 }; buf_puts(b, ch); }
  }
  free(r);
  return 1;
}

/* Process::Status#success?: the runtime answers -1 for CRuby's nil, when
   the process did not exit normally */
static int emit_op_pstatus_success(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  int t = ++g_tmp;
  buf_printf(b, "({ int _t%d = sp_process_status_success_p((%s)->status);"
                " _t%d < 0 ? sp_box_nil() : sp_box_bool((sp_bool)_t%d); })", t, r, t, t);
  free(r);
  return 1;
}

/* Process::Status#== / #eql? with no operand: false, the receiver still
   evaluated */
static int emit_op_pstatus_eq(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  free(r);
  buf_puts(b, "((void)("); emit_boxed(c, x->recv, b); buf_puts(b, "), (sp_bool)0)");
  return 1;
}

static int (*const bop_emitters[BOPE__COUNT])(Compiler *, const BopCtx *, Buf *) = {
  [BOPE_NONE] = NULL,
  [BOPE_TEMPLATE] = emit_op_template,
  [BOPE_PSTATUS_SUCCESS] = emit_op_pstatus_success,
  [BOPE_PSTATUS_EQ] = emit_op_pstatus_eq,
};

int emit_builtin_op(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  if (recv < 0 || !bop_covers(rt)) return 0;
  int argc;
  call_args(c->nt, id, &argc);
  const BuiltinOp *op = bop_find(rt, name, argc, nt_ref(c->nt, id, "block") >= 0);
  if (!op || op->emit == BOPE_NONE || !bop_emitters[op->emit]) return 0;
  BopCtx x = { id, recv, argc, rt, name, op };
  return bop_emitters[op->emit](c, &x, b);
}
