/* codegen_call_poly.c -- emit_call_body's arms on a poly (boxed) receiver that the runtime answers by the value it holds.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"
#include "repr.h"

static void emit_boxed_array_read(Compiler *c, int id, int receiver, const char *name, int count_node,
                                  const char *fn, BopEmit op, int default_count, Buf *b) {
  int array_only = op == BOPE_BOXED_ARRAY_ONLY;
  /* Check and root the receiver before converting a nullable or boxed count.
     The conversion may raise or call the program's to_int. */
  int hold = array_only || (count_node >= 0 &&
                          (node_has_oint_form(c, count_node) || repr_of(c, count_node).kind == RK_BOXED));
  int held = hold ? ++g_tmp : -1;
  int count_binding = -1, count_view = -1;
  Buf call = {0};
  Buf recv = {0};
  emit_expr(c, receiver, &recv);
  const char *receiver_text = recv.p ? recv.p : "sp_box_nil()";
  if (hold) buf_printf(&call, "({ sp_RbVal _t%d = ", held);
  else buf_printf(&call, "%s(", fn);
  if (array_only) {
    buf_printf(&call, "%s; SP_GC_ROOT_RBVAL(_t%d); ", receiver_text, held);
    /* Ruby evaluates the raw count before looking up the method. Hold it
       here so even a lone scalar-producing expression runs before the
       guard. The checked Integer conversion below reads the held value. */
    int splat = count_node >= 0 && nt_kind(c->nt, count_node) == NK_SplatNode;
    int operand = splat ? nt_ref(c->nt, count_node, "expression") : count_node;
    if (operand >= 0) {
      Buf *saved_pre = g_pre;
      g_pre = &call;
      int temp = ++g_tmp;
      if (splat) {
        Buf items = {0};
        emit_splat_operand_array(c, operand, &items);
        buf_printf(&call, "sp_RbVal _t%d = sp_box_poly_array(%s); SP_GC_ROOT_RBVAL(_t%d); ",
                   temp, items.p, temp);
        free(items.p);
      }
      else temp = hold_operand(c, operand, TY_POLY, 1, temp, 1, " ", &call);
      g_pre = saved_pre;
      count_binding = view_bind_temp(operand, temp);
      count_view = view_push(c, operand, TY_POLY);
    }
    buf_printf(&call, "if (!sp_poly_face_array_ok(_t%d)) sp_raise_poly_nomethod(\"%s\", _t%d); %s(_t%d",
               held, name, held, fn, held);
  }
  else {
    buf_puts(&call, "sp_poly_enum_chk(");
    if (op == BOPE_BOXED_ARRAY_OR_RANGE)
      buf_printf(&call, "sp_poly_ary_chk(%s, \"%s\", 1)", receiver_text, name);
    else buf_puts(&call, receiver_text);
    buf_printf(&call, ", \"%s\")", name);
  }
  free(recv.p);
  if (hold && !array_only) buf_printf(&call, "; SP_GC_ROOT_RBVAL(_t%d); %s(_t%d", held, fn, held);
  if (count_node >= 0) {
    Buf count = {0};
    emit_int_expr(c, count_node, &count);
    buf_puts(&call, ", ");
    buf_puts(&call, count.p ? count.p : "0");
    free(count.p);
  }
  else if (default_count) buf_puts(&call, ", 1");
  if (count_binding >= 0) {
    view_pop(c, count_view);
    view_unbind(count_binding);
  }
  buf_puts(&call, hold ? "); })" : ")");
  emit_unbox_text(c, repr_of(c, id).as_ty, call.p, b);
  free(call.p);
}

int emit_op_boxed_array_count(Compiler *c, const BopCtx *x, Buf *b) {
  if (poly_name_user_claimed(c, x->name, x->argc)) return 0;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  emit_boxed_array_read(c, x->id, x->recv, x->name, argc ? argv[0] : -1,
                        x->op->arg, x->op->emit, x->op->argc_max > 0, b);
  return 1;
}

/* builtin methods on a poly receiver the runtime answers by the value it holds: inject / reduce(:op), the Array reductions and slices, values_at, Fiber's resume / transfer / raise, Queue's enq / deq */
int emit_call_poly_builtin_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* Array-reduction methods on a boxed array element of a poly array (e.g.
     `runs.map { |r| r.sum }` over chunk_while runs). The runtime helper switches
     on the element's cls_id. Skipped when a user class defines the same method
     (it falls through to the general poly dispatch below). */
  /* inject/reduce(:op) on a container-read poly iterable (#3234). An operator
     not typed a Symbol is asked at run time (sp_poly_inject_op: a String's
     Symbol, or CRuby's TypeError), the receiver read first */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (is_reduce_alias(name)) && comp_ntype(c, argv[0]) != TY_UNKNOWN) {
    if (!poly_name_user_claimed(c, name, argc)) {
      if (comp_ntype(c, argv[0]) == TY_SYMBOL) {
        buf_puts(b, "sp_poly_inject_sym("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_poly_inject_sym(_t%d, sp_poly_inject_op(", tr, tr);
      emit_boxed(c, argv[0], b); buf_puts(b, ")); })");
      return 1;
    }
  }
  /* sum(seed) on a container-read row: numeric-tower accumulation from the
     boxed seed (#3234) */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "sum")) {
    int ncand9 = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_poly_arm_defines_n(c, k, name, argc)) ncand9++;
    if (ncand9 == 0) {
      emit_poly_sum_seed(c, recv, argv[0], b);
      return 1;
    }
  }
  /* shuffle / sample given `random:` on a boxed Array (emit_array_random_kw):
     the count arm below read the keyword hash as the count */
  if (emit_array_random_kw(c, id, b, nt, name, recv, argc, argv, rt)) return 1;
  /* and Queue's `timeout:` (emit_poly_queue_timeout), read as Array#pop's
     count or as non_block below */
  if (emit_poly_queue_timeout(c, id, b, nt, name, recv, argc, argv, rt)) return 1;
  if (rt == TY_POLY && emit_builtin_op(c, id, recv, BOP_BOXED_FALLBACK, name, b)) return 1;
  /* The keyword arm can decline sample(random: ...). Preserve its legacy
     fallback until keyword dispatch can handle all the accepted shapes. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "sample") && !poly_name_user_claimed(c, name, argc)) {
    emit_boxed_array_read(c, id, recv, name, argv[0], "sp_poly_arr_sample_n", BOPE_BOXED_ARRAY_ONLY, 0, b);
    return 1;
  }
  /* values_at takes any number of indices; collect them into a poly array. */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "values_at")) {
    if (!poly_name_user_claimed(c, name, argc)) {
      int ti9 = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ti9, ti9);
      for (int k = 0; k < argc; k++) {
        /* a splatted index list contributes each of its elements, not itself:
           pushed whole it became one index and the call answered from that
           one alone (#4164) */
        if (nt_kind(nt, argv[k]) == NK_SplatNode) {
          int sx9 = nt_ref(nt, argv[k], "expression");
          int ts9 = ++g_tmp, tj9 = ++g_tmp;
          /* the list first: what it hoists belongs ahead of the line that
             reads it, not inside that line's call */
          { Buf sb9; memset(&sb9, 0, sizeof sb9);
            if (sx9 >= 0) emit_boxed(c, sx9, &sb9);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_to_poly_array(%s); SP_GC_ROOT(_t%d);\n",
                       ts9, sb9.p ? sb9.p : "sp_box_nil()", ts9);
            free(sb9.p); }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                            " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n",
                     tj9, tj9, ts9, tj9, ti9, ts9, tj9);
          continue;
        }
        /* the index first, for the same reason */
        Buf ab9; memset(&ab9, 0, sizeof ab9);
        emit_boxed(c, argv[k], &ab9);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ti9, ab9.p ? ab9.p : "sp_box_nil()");
        free(ab9.p);
      }
      Buf cv9; memset(&cv9, 0, sizeof cv9);
      buf_puts(&cv9, "sp_poly_arr_values_at(");
      { Buf rv9; memset(&rv9, 0, sizeof rv9); emit_expr(c, recv, &rv9);
        buf_puts(&cv9, rv9.p ? rv9.p : "sp_box_nil()"); free(rv9.p); }
      buf_printf(&cv9, ", _t%d)", ti9);
      emit_unbox_text(c, repr_of(c, id).as_ty, cv9.p ? cv9.p : "sp_box_nil()", b);
      free(cv9.p);
      return 1;
    }
  }
  /* resume / transfer / raise on a boxed Fiber (one read out of an Array, or
     a local that was nil first). Anything else in the slot raises
     NoMethodError at run time -- but a boxed Thread answers #raise too (one
     read back out of a Hash's keys): raised in that thread, answering nil
     as Thread#raise does. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "resume") || sp_streq(name, "transfer") ||
       (sp_streq(name, "raise") && argc <= 3))) {
    if (!poly_name_user_claimed(c, name, argc) && sp_streq(name, "raise")) {
      Buf fv; memset(&fv, 0, sizeof fv);
      int tv = ++g_tmp, tf = ++g_tmp, tr = ++g_tmp;
      buf_printf(&fv, "({ sp_RbVal _t%d = ", tv);
      emit_boxed(c, recv, &fv);
      buf_printf(&fv, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d; "
                      "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_THREAD) { (void)(",
                 tv, tr, tv, tv);
      char tt[48]; snprintf(tt, sizeof tt, "((sp_thread *)_t%d.v.p)", tv);
      emit_concurrency_raise(c, tt, argc, argv, "sp_thread", 't', "sp_Thread_raise", &fv);
      buf_printf(&fv, "); _t%d = sp_box_nil(); } else { sp_Fiber *_t%d = sp_poly_as_fiber(_t%d, \"raise\"); "
                      "SP_GC_ROOT(_t%d); _t%d = ", tr, tf, tv, tf, tr);
      char ft[32]; snprintf(ft, sizeof ft, "_t%d", tf);
      emit_concurrency_raise(c, ft, argc, argv, "sp_Fiber", 'f', "sp_Fiber_raise", &fv);
      buf_printf(&fv, "; } _t%d; })", tr);
      emit_unbox_text(c, repr_of(c, id).as_ty, fv.p, b);
      free(fv.p);
      return 1;
    }
    if (!poly_name_user_claimed(c, name, argc)) {
      Buf fv; memset(&fv, 0, sizeof fv);
      int tf = ++g_tmp;
      buf_printf(&fv, "({ sp_Fiber *_t%d = sp_poly_as_fiber(", tf);
      emit_boxed(c, recv, &fv);
      buf_printf(&fv, ", \"%s\"); SP_GC_ROOT(_t%d); ", name, tf);
      char ft[32]; snprintf(ft, sizeof ft, "_t%d", tf);
      char fn[32]; snprintf(fn, sizeof fn, "sp_Fiber_%s_n", name);
      emit_fiber_pass_call(c, fn, ft, argc, argv, &fv);
      buf_puts(&fv, "; })");
      emit_unbox_text(c, repr_of(c, id).as_ty, fv.p, b);
      free(fv.p);
      return 1;
    }
  }
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      !poly_ostruct_member_call(c, id)) {
    const char *pm = NULL;
    if (sp_streq(name, "sum")) pm = "sp_poly_sum";
    else if (sp_streq(name, "min")) pm = "sp_poly_min";
    else if (sp_streq(name, "max")) pm = "sp_poly_max";
    else if (sp_streq(name, "first")) pm = "sp_poly_first";
    else if (sp_streq(name, "last")) pm = "sp_poly_last";
    else if (sp_streq(name, "sample")) pm = "sp_poly_sample";
    /* a Thread (Fiber-modelled) carried through a poly slot: #value/#resume/#join
       dispatch on the boxed Fiber when no user class defines the name (#1261). */
    else if (sp_streq(name, "value")) pm = "sp_poly_fiber_value";
    else if (sp_streq(name, "join")) pm = "sp_poly_fiber_join";
    /* and #alive? / #status, which a pool polls through its worker Array
       (#4463); these answer their own C types, not a boxed value */
    else if (sp_streq(name, "alive?")) pm = "sp_poly_fiber_alive";
    else if (sp_streq(name, "status")) pm = "sp_poly_thread_status";
    /* and #kill, which a shutdown path reaches through the handle it kept in
       an Array or an ivar rather than a traceable local (#4619) */
    else if (sp_streq(name, "kill")) pm = "sp_poly_thread_kill";
    /* and #kill's aliases, which a Fiber does not have */
    else if (sp_streq(name, "exit")) pm = "sp_poly_thread_exit";
    else if (sp_streq(name, "terminate")) pm = "sp_poly_thread_terminate_m";
    else if (sp_streq(name, "blocking?")) pm = "sp_poly_fiber_blocking";
    if (pm) {
      /* Attr readers count as user definitions too: `attr_accessor :value`
         must shadow the builtin helper exactly like `def value` does, or the
         reader call is hijacked (e.g. sp_poly_fiber_value on a Node). The
         general poly dispatch below emits reader arms, so it handles them. */
      if (!poly_name_user_claimed(c, name, argc)) {
        Repr wr = repr_of(c, id);
        int is_bool = sp_streq(name, "alive?") || sp_streq(name, "blocking?");
        if (is_bool && wr.kind == RK_BOXED) buf_puts(b, "sp_box_bool(");
        buf_printf(b, "%s(", pm); emit_expr(c, recv, b); buf_puts(b, ")");
        if (is_bool && wr.kind == RK_BOXED) buf_puts(b, ")");
        return 1;
      }
    }
  }

  /* The Queue names no other builtin answers, on a boxed Queue (one taken
     out of an Array or an ivar): #enq, #deq and #num_waiting. Anything else
     in the slot raises NoMethodError at run time. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      ((argc == 0 && (sp_streq(name, "deq") || sp_streq(name, "num_waiting"))) ||
       (argc == 1 && sp_streq(name, "enq")) ||
       (argc == 1 && sp_streq(name, "deq")) || (argc == 2 && sp_streq(name, "enq")))) {
    if (!poly_name_user_claimed(c, name, argc)) {
      Buf qv; memset(&qv, 0, sizeof qv);
      if (sp_streq(name, "deq") && argc == 1) {   /* deq(non_block) */
        buf_puts(&qv, "sp_poly_queue_pop_flag("); emit_boxed(c, recv, &qv);
        buf_puts(&qv, ", \"deq\", sp_poly_truthy("); emit_boxed(c, argv[0], &qv); buf_puts(&qv, "))");
      }
      else if (argc == 2) {   /* enq(obj, non_block) */
        buf_puts(&qv, "sp_poly_queue_push_flag("); emit_boxed(c, recv, &qv);
        buf_puts(&qv, ", \"enq\", "); emit_boxed(c, argv[0], &qv);
        buf_puts(&qv, ", sp_poly_truthy("); emit_boxed(c, argv[1], &qv); buf_puts(&qv, "))");
      }
      else {
        buf_printf(&qv, "sp_poly_queue_%s(", name);
        emit_boxed(c, recv, &qv);
        if (argc == 1) { buf_puts(&qv, ", "); emit_boxed(c, argv[0], &qv); }
        buf_puts(&qv, ")");
      }
      Repr wr = repr_of(c, id);
      TyKind want = wr.as_ty;
      if (wr.kind == RK_BOXED) buf_puts(b, qv.p);
      else emit_unbox_text(c, want, qv.p, b);
      free(qv.p);
      return 1;
    }
  }
  /* The same for a boxed Mutex (one read back out of a Hash of locks):
     #lock, #unlock, #try_lock, #locked? and Monitor#mon_owned?. #owned? is File::Stat's too
     and goes through the boxed stream arm. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "lock") || sp_streq(name, "unlock") ||
       sp_streq(name, "try_lock") || sp_streq(name, "locked?") || sp_streq(name, "mon_owned?")) &&
      !poly_name_user_claimed(c, name, argc)) {
    int is_bool = sp_streq(name, "try_lock") || sp_streq(name, "locked?") || sp_streq(name, "mon_owned?");
    Buf mv; memset(&mv, 0, sizeof mv);
    buf_printf(&mv, "sp_poly_mutex_%s(", sp_streq(name, "locked?") ? "locked" : sp_streq(name, "mon_owned?") ? "mon_owned" : name);
    emit_boxed(c, recv, &mv);
    buf_puts(&mv, ")");
    Repr wr = repr_of(c, id);
    if (is_bool && wr.kind == RK_BOXED) buf_printf(b, "sp_box_bool(%s)", mv.p);
    else if (is_bool || wr.kind == RK_BOXED) buf_puts(b, mv.p);
    else emit_unbox_text(c, wr.as_ty, mv.p, b);
    free(mv.p);
    return 1;
  }
  return 0;
}
