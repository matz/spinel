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
  if (x->rtext) return strdup(x->rtext);
  Buf r; memset(&r, 0, sizeof r);
  emit_expr(c, x->recv, &r);
  if (!r.p) return strdup("");
  return r.p;
}

/* A row's C text with its placeholders filled in, each where it stands,
   so the C is emitted in the order the arm the row replaces emitted it:
     $r   the receiver, emitted at its first occurrence (or the text the
          caller rendered, x->rtext); a later $r repeats the same text
     $t   a fresh temp number (++g_tmp) taken at its first occurrence; a
          later $t repeats it
     $eN  argument N by emit_expr
     $bN  argument N boxed (emit_boxed)
     $fN  argument N as a double (emit_float_expr) */
static int emit_op_template(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = NULL;
  int t = 0;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  for (const char *p = x->op->arg; *p; p++) {
    if (p[0] == '$' && p[1] == 'r') {
      if (!r && x->rtext) { r = strdup(x->rtext); buf_puts(b, r); }
      else if (!r) {
        size_t mark = b->len;
        emit_expr(c, x->recv, b);
        r = strndup(b->p ? b->p + mark : "", b->len - mark);
      }
      else buf_puts(b, r);
      p++;
    }
    else if (p[0] == '$' && p[1] == 't') {
      if (!t) t = ++g_tmp;
      buf_printf(b, "%d", t);
      p++;
    }
    else if (p[0] == '$' && (p[1] == 'e' || p[1] == 'b' || p[1] == 'f') &&
             p[2] >= '0' && p[2] <= '9' && p[2] - '0' < argc) {
      int a = argv[p[2] - '0'];
      if (p[1] == 'e') emit_expr(c, a, b);
      else if (p[1] == 'b') emit_boxed(c, a, b);
      else emit_float_expr(c, a, b);
      p += 2;
    }
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
  [BOPE_THREAD_SET_REPORT] = emit_op_thread_set_report,
  [BOPE_THREAD_RAISE] = emit_op_thread_raise,
  [BOPE_THREAD_TLS] = emit_op_thread_tls,
  [BOPE_MUTEX_SLEEP] = emit_op_mutex_sleep,
  [BOPE_CONDVAR_WAIT] = emit_op_condvar_wait,
  [BOPE_QUEUE_PUSH] = emit_op_queue_push,
  [BOPE_QUEUE_POP] = emit_op_queue_pop,
  [BOPE_FIBER_RESUME] = emit_op_fiber_resume,
  [BOPE_FIBER_TRANSFER] = emit_op_fiber_transfer,
  [BOPE_FIBER_RAISE] = emit_op_fiber_raise,
};

/* the first argument's kind, for a row's arg0 guard */
typedef struct { const Compiler *c; int arg; } BopArg0;
static TyKind bop_arg0_ntype(const void *ud) {
  const BopArg0 *a = ud;
  return comp_ntype(a->c, a->arg);
}

int emit_builtin_op_text(Compiler *c, int id, int recv, TyKind rt, const char *name,
                         const char *rtext, Buf *b) {
  if (recv < 0 || !bop_covers(rt)) return 0;
  int argc;
  const int *argv = call_args(c->nt, id, &argc);
  BopArg0 a0 = { c, argc >= 1 ? argv[0] : -1 };
  const BuiltinOp *op = bop_find_arg(rt, name, argc, nt_ref(c->nt, id, "block") >= 0,
                                     bop_arg0_ntype, &a0);
  if (!op || op->emit == BOPE_NONE || !bop_emitters[op->emit]) return 0;
  BopCtx x = { id, recv, argc, rt, name, op, rtext };
  return bop_emitters[op->emit](c, &x, b);
}

int emit_builtin_op(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  return emit_builtin_op_text(c, id, recv, rt, name, NULL, b);
}
