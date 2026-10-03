/* codegen_call_poly.c -- emit_call_body's arms on a poly (boxed) receiver that the runtime answers by the value it holds.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* builtin methods on a poly receiver the runtime answers by the value it holds: inject / reduce(:op), the Array reductions and slices, values_at, Fiber's resume / transfer / raise, Queue's enq / deq */
int emit_call_poly_builtin_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* Array-reduction methods on a boxed array element of a poly array (e.g.
     `runs.map { |r| r.sum }` over chunk_while runs). The runtime helper switches
     on the element's cls_id. Skipped when a user class defines the same method
     (it falls through to the general poly dispatch below). */
  /* inject/reduce(:op) on a container-read poly iterable (#3234) */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "inject") || sp_streq(name, "reduce")) &&
      comp_ntype(c, argv[0]) == TY_SYMBOL) {
    if (!poly_name_user_claimed(c, name, argc, 0)) {
      buf_puts(b, "sp_poly_inject_sym("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
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
  /* The count-taking Array reads on a poly receiver. An array read out of a
     nested Array or Hash answers Array to #class but had no arm for these, so
     they raised NoMethodError (#3464). rotate's count is optional. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      (argc == 1 || (argc == 0 && (sp_streq(name, "rotate") || sp_streq(name, "shuffle"))))) {
    const char *pn9 = NULL;
    if (sp_streq(name, "first") || sp_streq(name, "take")) pn9 = "sp_poly_arr_take";
    else if (sp_streq(name, "last")) pn9 = "sp_poly_arr_last_n";
    else if (sp_streq(name, "drop")) pn9 = "sp_poly_arr_drop";
    else if (sp_streq(name, "rotate")) pn9 = "sp_poly_arr_rotate";
    else if (sp_streq(name, "sample")) pn9 = "sp_poly_arr_sample_n";
    else if (sp_streq(name, "min")) pn9 = "sp_poly_arr_min_n";
    else if (sp_streq(name, "max")) pn9 = "sp_poly_arr_max_n";
    else if (sp_streq(name, "shuffle") && argc == 0) pn9 = "sp_poly_arr_shuffle";
    if (pn9) {
      if (!poly_name_user_claimed(c, name, argc, 1)) {
        Buf cb9; memset(&cb9, 0, sizeof cb9);
        buf_printf(&cb9, "%s(", pn9);
        { Buf rb9; memset(&rb9, 0, sizeof rb9); emit_expr(c, recv, &rb9);
          buf_puts(&cb9, rb9.p ? rb9.p : "sp_box_nil()"); free(rb9.p); }
        if (argc == 1) { Buf nb9; memset(&nb9, 0, sizeof nb9); emit_int_expr(c, argv[0], &nb9);
                         buf_puts(&cb9, ", "); buf_puts(&cb9, nb9.p ? nb9.p : "0"); free(nb9.p); }
        else if (!sp_streq(name, "shuffle")) buf_puts(&cb9, ", 1");   /* rotate's default count */
        buf_puts(&cb9, ")");
        /* the helpers answer a boxed poly array; a slot typed as the array
           itself takes the pointer out of the box */
        emit_unbox_text(c, comp_ntype(c, id), cb9.p ? cb9.p : "sp_box_nil()", b);
        free(cb9.p);
        return 1;
      }
    }
  }
  /* values_at takes any number of indices; collect them into a poly array. */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 && nt_ref(nt, id, "block") < 0 &&
      sp_streq(name, "values_at")) {
    if (!poly_name_user_claimed(c, name, argc, 0)) {
      int ti9 = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ti9, ti9);
      for (int k = 0; k < argc; k++) {
        /* a splatted index list contributes each of its elements, not itself:
           pushed whole it became one index and the call answered from that
           one alone (#4164) */
        if (nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "SplatNode")) {
          int sx9 = nt_ref(nt, argv[k], "expression");
          int ts9 = ++g_tmp, tj9 = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_to_poly_array(", ts9);
          { Buf sb9; memset(&sb9, 0, sizeof sb9);
            if (sx9 >= 0) emit_boxed(c, sx9, &sb9);
            buf_puts(g_pre, sb9.p ? sb9.p : "sp_box_nil()"); free(sb9.p); }
          buf_printf(g_pre, "); SP_GC_ROOT(_t%d);\n", ts9);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                            " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n",
                     tj9, tj9, ts9, tj9, ti9, ts9, tj9);
          continue;
        }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", ti9);
        Buf ab9; memset(&ab9, 0, sizeof ab9);
        emit_boxed(c, argv[k], &ab9);
        buf_puts(g_pre, ab9.p ? ab9.p : "sp_box_nil()");
        free(ab9.p);
        buf_puts(g_pre, ");\n");
      }
      Buf cv9; memset(&cv9, 0, sizeof cv9);
      buf_puts(&cv9, "sp_poly_arr_values_at(");
      { Buf rv9; memset(&rv9, 0, sizeof rv9); emit_expr(c, recv, &rv9);
        buf_puts(&cv9, rv9.p ? rv9.p : "sp_box_nil()"); free(rv9.p); }
      buf_printf(&cv9, ", _t%d)", ti9);
      emit_unbox_text(c, comp_ntype(c, id), cv9.p ? cv9.p : "sp_box_nil()", b);
      free(cv9.p);
      return 1;
    }
  }
  /* resume / transfer / raise on a boxed Fiber (one read out of an Array, or
     a local that was nil first). Anything else in the slot raises
     NoMethodError at run time. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "resume") || sp_streq(name, "transfer") ||
       (sp_streq(name, "raise") && argc <= 3))) {
    if (!poly_name_user_claimed(c, name, argc, 1)) {
      Buf fv; memset(&fv, 0, sizeof fv);
      int tf = ++g_tmp;
      buf_printf(&fv, "({ sp_Fiber *_t%d = sp_poly_as_fiber(", tf);
      emit_boxed(c, recv, &fv);
      buf_printf(&fv, ", \"%s\"); SP_GC_ROOT(_t%d); ", name, tf);
      char ft[32]; snprintf(ft, sizeof ft, "_t%d", tf);
      if (sp_streq(name, "raise"))
        emit_concurrency_raise(c, ft, argc, argv, "sp_Fiber", 'f', "sp_Fiber_raise", &fv);
      else {
        char fn[32]; snprintf(fn, sizeof fn, "sp_Fiber_%s_n", name);
        emit_fiber_pass_call(c, fn, ft, argc, argv, &fv);
      }
      buf_puts(&fv, "; })");
      emit_unbox_text(c, comp_ntype(c, id), fv.p, b);
      free(fv.p);
      return 1;
    }
  }
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0) {
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
    else if (sp_streq(name, "blocking?")) pm = "sp_poly_fiber_blocking";
    if (pm) {
      /* Attr readers count as user definitions too: `attr_accessor :value`
         must shadow the builtin helper exactly like `def value` does, or the
         reader call is hijacked (e.g. sp_poly_fiber_value on a Node). The
         general poly dispatch below emits reader arms, so it handles them. */
      if (!poly_name_user_claimed(c, name, argc, 1)) {
        TyKind want = comp_ntype(c, id);
        int is_bool = sp_streq(name, "alive?") || sp_streq(name, "blocking?");
        if (is_bool && want == TY_POLY) buf_puts(b, "sp_box_bool(");
        buf_printf(b, "%s(", pm); emit_expr(c, recv, b); buf_puts(b, ")");
        if (is_bool && want == TY_POLY) buf_puts(b, ")");
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
    if (!poly_name_user_claimed(c, name, argc, 1)) {
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
      TyKind want = comp_ntype(c, id);
      if (want == TY_POLY) buf_puts(b, qv.p);
      else emit_unbox_text(c, want, qv.p, b);
      free(qv.p);
      return 1;
    }
  }
  return 0;
}
