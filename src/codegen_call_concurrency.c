/* codegen_call_concurrency.c -- the builtin-op emitters of the concurrency
   handles (Fiber, Thread, Queue, Mutex, ConditionVariable) that are more
   than one C expression around the receiver; the rest are templates in
   builtin_ops.c. Each answers 1 when it emitted the call, 0 (having
   emitted nothing) to leave it to the chain after the lookup. */

#include "codegen_internal.h"
#include "builtin_ops.h"
#include "codegen_call_arms.h"

int emit_op_thread_set_report(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int t = ++g_tmp;
  buf_printf(b, "({ sp_thread *_t%d = ", t); emit_expr(c, recv, b);
  buf_printf(b, "; sp_Thread_set_report(_t%d, ", t);
  emit_coerce(c, argv[0], TY_BOOL, CO_CONVERT, "Thread#report_on_exception=", b);
  buf_puts(b, "); })");
  return 1;
}

int emit_op_thread_raise(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_concurrency_raise(c, rb.p ? rb.p : "NULL", argc, argv, "sp_thread", 't', "sp_Thread_raise", b);
  free(rb.p);
  return 1;
}

/* Thread#[] / []= / key? by a key that is not a Symbol, and
   thread_variable_get / _set / ?: the key goes through
   sp_thread_local_key */
int emit_op_thread_tls(Compiler *c, const BopCtx *x, Buf *b) {
  const char *name = x->name;
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int tv_get = sp_streq(name, "thread_variable_get"), tv_set = sp_streq(name, "thread_variable_set"),
      tv_key = sp_streq(name, "thread_variable?");
  if (((sp_streq(name, "[]") || sp_streq(name, "key?") || tv_get || tv_key) && argc == 1) ||
      ((sp_streq(name, "[]=") || tv_set) && argc == 2)) {
    int tt = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
    buf_printf(b, "({ sp_thread *_t%d = ", tt); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", tt, tk); emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tk);
    if (argc == 2) {
      buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tv);
    }
    buf_printf(b, " sp_Thread_tls_%s(_t%d, sp_thread_local_key(_t%d)",
               argc == 2 ? "set" : (sp_streq(name, "[]") || tv_get) ? "get" : "key", tt, tk);
    if (argc == 2) buf_printf(b, ", _t%d", tv);
    buf_puts(b, "); })"); return 1;
  }
  return 0;
}

/* #sleep / #sleep(timeout): nil (or no argument) sleeps until #wakeup */
int emit_op_mutex_sleep(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind st = argc == 1 ? comp_ntype(c, argv[0]) : TY_NIL;
  int tm = ++g_tmp;
  buf_printf(b, "({ sp_mutex *_t%d = ", tm); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d); ", tm);   /* it may be the only reference while we park */
  if (argc == 0) buf_printf(b, "sp_Mutex_sleep(_t%d, 0, 0.0); })", tm);
  else if (st == TY_INT || st == TY_FLOAT) {
    buf_printf(b, "sp_Mutex_sleep(_t%d, 1, (double)(", tm); emit_expr(c, argv[0], b); buf_puts(b, ")); })");
  }
  else if (st == TY_NIL) {
    buf_puts(b, "(void)("); emit_expr(c, argv[0], b);
    buf_printf(b, "); sp_Mutex_sleep(_t%d, 0, 0.0); })", tm);
  }
  else {   /* a boxed timeout: nil means none */
    int ta = ++g_tmp;
    buf_printf(b, "sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b);
    buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? sp_Mutex_sleep(_t%d, 0, 0.0)"
                  " : sp_Mutex_sleep(_t%d, 1, sp_poly_time_interval(_t%d)); })", ta, tm, tm, ta);
  }
  return 1;
}

/* wait(mutex): release the mutex, park, re-acquire. The 2-arg
   `wait(mutex, timeout)` form uses the scheduler's deadline queue.
   A nil timeout means no deadline, and non-positive timeouts take the
   existing release-and-reacquire path without parking. The value is
   the runtime's answer: nil on a timeout, else the seconds slept. */
int emit_op_condvar_wait(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int t = ++g_tmp;
  buf_printf(b, "({ sp_condvar *_t%d = ", t); emit_expr(c, recv, b);
  if (argc == 1) {
    buf_printf(b, "; sp_CondVar_wait(_t%d, ", t); emit_expr(c, argv[0], b);
    buf_puts(b, "); })");
  }
  else {
    /* argv[0] is the mutex, argv[1] is the timeout */
    int to_arg = argv[1];
    const char *aty = nt_type(c->nt, to_arg);
    if (aty && sp_streq(aty, "IntegerNode") &&
        (int)nt_int(c->nt, to_arg, "value", 0) == 0) {
      buf_printf(b, "; sp_CondVar_wait_nb(_t%d, ", t); emit_expr(c, argv[0], b);
      buf_puts(b, "); })");
    }
    else if (nt_kind(c->nt, to_arg) == NK_NilNode) {
      buf_printf(b, "; sp_CondVar_wait(_t%d, ", t); emit_expr(c, argv[0], b);
      buf_puts(b, "); })");
    }
    else if (comp_ntype(c, to_arg) == TY_NIL) {
      /* A non-literal nil expression (for example, a method returning
         nil) still has to run for its side effects, then means no timeout. */
      int m = ++g_tmp;
      buf_printf(b, "; sp_mutex *_m%d = ", m); emit_expr(c, argv[0], b);
      buf_puts(b, "; (void)("); emit_expr(c, to_arg, b);
      buf_printf(b, "); sp_CondVar_wait(_t%d, _m%d); })", t, m);
    }
    else {
      int m = ++g_tmp;
      buf_printf(b, "; sp_mutex *_m%d = ", m); emit_expr(c, argv[0], b);
      if (comp_ntype(c, to_arg) == TY_POLY || comp_ntype(c, to_arg) == TY_UNKNOWN) {
        int timeout = ++g_tmp;
        buf_printf(b, "; sp_RbVal _timeout%d = ", timeout); emit_boxed(c, to_arg, b);
        buf_printf(b, "; _timeout%d.tag == SP_TAG_NIL ? sp_CondVar_wait(_t%d, _m%d) "
                      ": sp_CondVar_wait_timeout(_t%d, _m%d, sp_poly_to_f_with_rational(_timeout%d)); })",
                   timeout, t, m, t, m, timeout);
      }
      else {
        int timeout = ++g_tmp;
        buf_printf(b, "; double _timeout%d = ", timeout); emit_float_expr(c, to_arg, b);
        buf_printf(b, "; sp_CondVar_wait_timeout(_t%d, _m%d, _timeout%d); })",
                   t, m, timeout);
      }
    }
  }
  return 1;
}

/* Queue#push / << / enq (value, non_block = false, timeout: nil). The
   row's arity counts the keyword hash; one or two positional arguments are
   taken. */
int emit_op_queue_push(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = x->recv;
  int argc;
  const int *argv = call_args(nt, x->id, &argc);
  int kwh = argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  int pos_argc = kwh >= 0 ? argc - 1 : argc;
  int timeout_arg = kwh >= 0 ? struct_kwarg_value(c, kwh, "timeout") : -1;
  if (pos_argc >= 1 && pos_argc <= 2) {
    int timed = timeout_arg >= 0;
    int t = ++g_tmp;
    buf_printf(b, "({ sp_queue *_t%d = ", t); emit_expr(c, recv, b);
    int v = ++g_tmp;
    buf_printf(b, "; sp_RbVal _v%d = ", v); emit_boxed(c, argv[0], b);
    if (!timed && pos_argc == 1) {
      buf_printf(b, "; sp_Queue_push(_t%d, _v%d); _t%d; })", t, v, t);
      return 1;
    }
    /* The non_block and timeout expressions below may allocate; the pushed
       value sits only in this C local until the runtime roots it. */
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_v%d)", v);
    int nb = -1;
    if (pos_argc == 2) {
      nb = ++g_tmp;
      buf_printf(b, "; sp_RbVal _nb%d = ", nb); emit_boxed(c, argv[1], b);
    }
    if (!timed) {
      if (nb < 0) buf_printf(b, "; sp_Queue_push(_t%d, _v%d); _t%d; })", t, v, t);
      else buf_printf(b, "; sp_Queue_push_options_check(_t%d, 1, 0); sp_poly_truthy(_nb%d) ? (sp_Queue_push_nb(_t%d, _v%d), _t%d) : (sp_Queue_push(_t%d, _v%d), _t%d); })",
                      t, nb, t, v, t, t, v, t);
      return 1;
    }
    int to = ++g_tmp;
    buf_printf(b, "; sp_RbVal _timeout%d = ", to); emit_boxed(c, timeout_arg, b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_Queue_push_options_check(_t%d, %d, 1); ", t, nb >= 0);
    if (nb >= 0) buf_printf(b, "if (sp_poly_truthy(_nb%d) && sp_poly_truthy(_timeout%d)) sp_raise_cls(\"ArgumentError\", \"can't set a timeout if non_block is enabled\"); ", nb, to);
    buf_printf(b, "_timeout%d.tag == SP_TAG_NIL ? ", to);
    if (nb >= 0) buf_printf(b, "(sp_poly_truthy(_nb%d) ? (sp_Queue_push_nb(_t%d, _v%d), _t%d) : (sp_Queue_push(_t%d, _v%d), _t%d)) : ", nb, t, v, t, t, v, t);
    else buf_printf(b, "(sp_Queue_push(_t%d, _v%d), _t%d) : ", t, v, t);
    buf_printf(b, "(sp_Queue_push_timeout(_t%d, _v%d, sp_poly_to_f_with_rational(_timeout%d)) ? _t%d : NULL); })",
               t, v, to, t);
    return 1;
  }
  return 0;
}

/* Queue#pop / shift / deq (non_block = false, timeout: nil) */
int emit_op_queue_pop(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = x->recv;
  int argc;
  const int *argv = call_args(nt, x->id, &argc);
  int kwh = argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  int pos_argc = kwh >= 0 ? argc - 1 : argc;
  int timeout_arg = kwh >= 0 ? struct_kwarg_value(c, kwh, "timeout") : -1;
  if (pos_argc <= 1) {
    int timed = timeout_arg >= 0;
    if (!timed && pos_argc == 0) {
      buf_puts(b, "sp_Queue_pop("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    /* Keep the literal non_block cases on their direct runtime arms. A
       false/nil literal means blocking; true means no_wait. */
    if (!timed && pos_argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && (sp_streq(aty, "FalseNode") || sp_streq(aty, "NilNode"))) {
        int q = ++g_tmp;
        buf_printf(b, "({ sp_queue *_q%d = ", q); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Queue_pop(_q%d); })", q); return 1;
      }
      if (aty && sp_streq(aty, "TrueNode")) {
        int q = ++g_tmp;
        buf_printf(b, "({ sp_queue *_q%d = ", q); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Queue_pop_nb(_q%d); })", q); return 1;
      }
    }
    int q = ++g_tmp;
    buf_printf(b, "({ sp_queue *_q%d = ", q); emit_expr(c, recv, b);
    int nb = -1;
    if (pos_argc == 1) {
      nb = ++g_tmp;
      buf_printf(b, "; sp_RbVal _nb%d = ", nb); emit_boxed(c, argv[0], b);
    }
    if (!timed) {
      if (nb < 0) buf_printf(b, "; sp_Queue_pop(_q%d); })", q);
      else buf_printf(b, "; sp_poly_truthy(_nb%d) ? sp_Queue_pop_nb(_q%d) : sp_Queue_pop(_q%d); })", nb, q, q);
      return 1;
    }
    int to = ++g_tmp;
    buf_printf(b, "; sp_RbVal _timeout%d = ", to); emit_boxed(c, timeout_arg, b);
    buf_puts(b, "; ");
    if (nb >= 0) buf_printf(b, "if (sp_poly_truthy(_nb%d) && sp_poly_truthy(_timeout%d)) sp_raise_cls(\"ArgumentError\", \"can't set a timeout if non_block is enabled\"); ", nb, to);
    buf_printf(b, "_timeout%d.tag == SP_TAG_NIL ? ", to);
    if (nb >= 0) buf_printf(b, "(sp_poly_truthy(_nb%d) ? sp_Queue_pop_nb(_q%d) : sp_Queue_pop(_q%d)) : ", nb, q, q);
    else buf_printf(b, "sp_Queue_pop(_q%d) : ", q);
    buf_printf(b, "sp_Queue_pop_timeout(_q%d, sp_poly_to_f_with_rational(_timeout%d)); })", q, to);
    return 1;
  }
  return 0;
}

int emit_op_fiber_resume(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_fiber_pass_call(c, "sp_Fiber_resume_n", rb.p ? rb.p : "NULL", argc, argv, b);
  free(rb.p);
  return 1;
}

int emit_op_fiber_transfer(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_fiber_pass_call(c, "sp_Fiber_transfer_n", rb.p ? rb.p : "NULL", argc, argv, b);
  free(rb.p);
  return 1;
}

int emit_op_fiber_raise(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_concurrency_raise(c, rb.p ? rb.p : "NULL", argc, argv, "sp_Fiber", 'f', "sp_Fiber_raise", b);
  free(rb.p);
  return 1;
}

/* Mutex/Monitor#synchronize { block }: the block run inline */
int emit_call_synchronize_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv) {
  /* Mutex/Monitor#synchronize { block }: run block inline (single-threaded) */
  if (sp_streq(name, "synchronize") && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    int bdy = nt_ref(nt, blk, "body");
    int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
    TyKind res = comp_ntype(c, id);
    int scalar = is_scalar_ret(res) && res != TY_VOID && res != TY_NIL && res != TY_UNKNOWN;
    int rv = ++g_tmp;
    /* A real Mutex#synchronize takes the lock around the block and releases it
       with ensure semantics: the unlock runs on normal completion, on an
       exception in the block (then re-raised), and on a non-local unwind passing
       through it (proc-return / throw, then resumed). A receiver of any other
       static type keeps the inline no-op behaviour. (A bare `return` -- a C return out of the
       inlined body -- is not yet covered; it would need deferred-return plumbing
       like begin..ensure.) */
    /* Full ensure semantics for a Mutex receiver: the unlock runs on normal
       completion, on a `return` out of the block (deferred via the begin..ensure
       g_ensure_stack mechanism), on an exception (then re-raised), and on a
       non-local unwind passing through (proc-return / throw, then resumed). The
       eid names the deferred-return/exception slots that emit_return targets. */
    /* A poly receiver (`LOCKS[i].synchronize { }`, a Mutex read out of a
       container) takes the same lock/ensure shape through a runtime check of
       the boxed value: the inline no-op left the critical section unlocked
       whenever the static type could not see the Mutex (campfire's fragment
       cache shards, a hash corrupted under concurrent writes). The check
       raises CRuby's NoMethodError for a value that is not a Mutex. */
    TyKind rty = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    int is_mx = recv >= 0 && (rty == TY_MUTEX || rty == TY_POLY) && g_ensure_depth < MAX_ENSURE_DEPTH;
    int mtmp = 0, eid = 0, has_retval = 0;
    buf_puts(b, "({ ");
    /* result temp is declared before the setjmp so it survives the block scope;
       the body assigns into it. */
    if (scalar) { emit_ctype(c, res, b); buf_printf(b, " _t%d = %s; ", rv, default_value_from_compiler(c, res)); }
    if (is_mx) {
      mtmp = ++g_tmp; eid = ++g_tmp;
      has_retval = (g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN);
      buf_printf(b, "sp_mutex *_t%d = ", mtmp);
      if (rty == TY_POLY) { buf_puts(b, "sp_poly_mutex_recv("); emit_boxed(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_printf(b, "; sp_Mutex_lock(_t%d); ", mtmp);
      buf_printf(b, "int _retf%d = 0; int _excf%d = 0; const char *_excmsg%d = NULL, *_exccls%d = NULL; ",
                 eid, eid, eid, eid);
      if (has_retval) { emit_ctype(c, g_ret_type, b); buf_printf(b, " _retv%d = %s; ", eid, default_value_from_compiler(c, g_ret_type)); }
      g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth, g_ret_type };
      buf_puts(b, "sp_exc_check_depth(); sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; ");
      buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++; if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) { ");
      g_exc_frame_depth++;
    }
    for (int k = 0; k < bbn - 1; k++) emit_stmt(c, bbb[k], b, 0);
    if (bbn > 0) {
      TyKind lty = comp_ntype(c, bbb[bbn-1]);
      const char *lnty = nt_type(nt, bbb[bbn-1]);
      int nil_lit = (lty == TY_NIL && lnty && sp_streq(lnty, "NilNode"));
      int can_expr = (lty != TY_VOID && lty != TY_UNKNOWN && (lty != TY_NIL || nil_lit));
      if (scalar && can_expr) {
        /* The tail is emitted as an EXPRESSION, and an expression's
           statement-shaped setup goes to g_pre -- which at this point is the
           buffer for the whole `lock.synchronize { ... }` line, i.e. OUTSIDE
           the lock. `h[k] += 1` puts its read-modify-write there and leaves
           only the read inside, so concurrent increments lost updates while
           the same thing written as two statements did not (#3387). Catch the
           prelude in a local buffer and flush it inside the critical section,
           where it belongs. */
        Buf tpre; memset(&tpre, 0, sizeof tpre);
        Buf tval; memset(&tval, 0, sizeof tval);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &tpre; g_indent = 0;
        if (res == TY_POLY && lty != TY_POLY) emit_boxed(c, bbb[bbn-1], &tval);
        else emit_expr(c, bbb[bbn-1], &tval);
        g_pre = sv_pre; g_indent = sv_ind;
        if (tpre.p) buf_puts(b, tpre.p);
        buf_printf(b, "_t%d = ", rv);
        buf_puts(b, tval.p ? tval.p : "0");
        buf_puts(b, "; ");
        free(tpre.p); free(tval.p);
      }
      else {
        emit_stmt(c, bbb[bbn-1], b, 0);  /* scalar default already set at rv decl */
      }
    }
    if (is_mx) {
      g_ensure_depth--;
      g_exc_frame_depth--;
      buf_printf(b, "sp_exc_top--; }\nelse { sp_exc_top--; sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; if (sp_unwind_kind == SP_UNWIND_NONE) { _excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; } } ",
                 eid, eid, eid);
      buf_printf(b, "_ensure%d: ; sp_Mutex_unlock(_t%d); ", eid, mtmp);
      buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume(); ");
      if (g_ensure_depth > 0) {
        /* nested inside another begin..ensure / synchronize: hand the deferred
           return and unhandled exception to the enclosing ensure. */
        EnsureCtx *outer = &g_ensure_stack[g_ensure_depth - 1];
        if (has_retval && outer->has_retval)
          buf_printf(b, "if (_retf%d) { _retv%d = _retv%d; _retf%d = 1; sp_exc_top--; goto _ensure%d; } ",
                     eid, outer->lid, eid, outer->lid, outer->lid);
        else
          buf_printf(b, "if (_retf%d) { _retf%d = 1; sp_exc_top--; goto _ensure%d; } ", eid, outer->lid, outer->lid);
        buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; sp_exc_top--; goto _ensure%d; } ",
                   eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid);
      }
      else {
        /* the deferred return leaves through every enclosing live begin
           frame: pop them (see the begin..ensure epilogue in codegen_stmt.c),
           and pop the sp_rescue_sp handler for each rescue body it leaves */
        { char g[24]; snprintf(g, sizeof g, "_retf%d", eid);
          if (emit_frame_unwind(b, 0, g)) buf_puts(b, " "); }
        /* Inside a first-class proc body whose returns route through the boxed
           slot, the deferred value leaves through the slot -- a raw C return
           of an sp_RbVal from an sp_int function does not compile. The
           statement-side copy of this funnel (codegen_stmt.c) has had the
           branch; this one did not, so `Mutex#synchronize` inside a proc body
           emitted it (#3383). */
        if (has_retval && g_ret_type == TY_POLY && proc_ret_slot())
          buf_printf(b, "if (_retf%d) { %s = _retv%d; return 0; } ", eid, proc_ret_slot(), eid);
        /* a fiber body is `static void`: see g_c_ret_void */
        else if (has_retval && g_c_ret_void) buf_printf(b, "if (_retf%d) return; ", eid);
        else if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d; ", eid, eid);
        else if (g_in_proc_body && g_result_var && g_result_poly)
          buf_printf(b, "if (_retf%d) { %s = sp_box_nil(); return 0; } ", eid, g_result_var);
        else if (g_c_ret_void) buf_printf(b, "if (_retf%d) return; ", eid);
        else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil(); ", eid);
        else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0; ", eid);
        /* a proc body returns sp_int: see the sibling in codegen_iter.c */
        else if (g_in_proc_body) buf_printf(b, "if (_retf%d) return 0; ", eid);
        else buf_printf(b, "if (_retf%d) return; ", eid);
        buf_printf(b, "if (_excf%d) sp_raise_cls(_exccls%d, _excmsg%d); ", eid, eid, eid);
      }
    }
    if (scalar) buf_printf(b, "_t%d; })", rv);
    else buf_puts(b, "0; })");
    return 1;
  }
  return 0;
}
