/* codegen_call_exception.c -- emit_call_body's arms of raise and of exception objects.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* the methods of an exception object: message, full_message, backtrace, set_backtrace, cause, ==, and the rest of TY_EXCEPTION */
int emit_call_exception_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* A specialized rescue var (`rescue MyError => e`, MyError carrying ivars)
     is typed as the subclass object so `e.<ivar>` reads work. Its
     exception-shaped queries still route through the base sp_Exception helpers
     (the struct's leading members mirror sp_Exception); the ivar readers fall
     through to normal object dispatch below. */
  if (recv >= 0 && ty_is_object(comp_ntype(c, recv)) &&
      class_is_exc_subclass(c, ty_object_class(comp_ntype(c, recv)))) {
    /* dup/clone copy the whole subclass struct via the GC header size, so
       mutating the copy's ivars leaves the original alone (#2772) */
    if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0 &&
        comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), name, NULL) < 0) {
      int xc2 = ty_object_class(comp_ntype(c, recv));
      buf_printf(b, "((sp_%s *)sp_exc_dup((sp_Exception *)(", c->classes[xc2].c_name);
      emit_expr(c, recv, b);
      buf_puts(b, ")))");
      return 1;
    }
    /* #inspect on an exception subclass is "#<Class: message>"; the generic
       object renderer had no idea and printed "#<Object>" (#3713) */
    if (sp_streq(name, "inspect") && argc == 0 &&
        comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), "inspect", NULL) < 0) {
      buf_puts(b, "sp_exc_inspect((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    /* the readers CRuby defines on one class of the hierarchy (#errno on
       SystemCallError, #path on LoadError, #key on KeyError, ...): an
       instance of a program's subclass is an sp_Exception, and the runtime
       reader judges its class as it does a base-typed one's -- the call fell
       to the object dispatch, which knew none of them */
    if (argc == 0 && exc_gated_acc_fn(name) && nt_ref(nt, id, "block") < 0 &&
        comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), name, NULL) < 0 &&
        !comp_reader_in_chain(c, ty_object_class(comp_ntype(c, recv)), name, NULL)) {
      if (sp_streq(name, "reason") || sp_streq(name, "tag") || sp_streq(name, "key") ||
          sp_streq(name, "name"))
        g_uses_symbols = 1;  /* staged names intern back to symbols */
      buf_printf(b, "%s((sp_Exception *)(", exc_gated_acc_fn(name));
      emit_expr(c, recv, b);
      buf_puts(b, "))");
      return 1;
    }
    /* the accessors every exception carries: an instance of a user subclass is
       still an sp_Exception, so read them off it (#3732) */
    /* An instance of a user exception subclass is an sp_Exception, so the
       message-taking #exception and the value #== read off it too; only the
       no-argument accessors had an arm and both were refused (#3870). */
    if (argc == 1 && comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), name, NULL) < 0 &&
        (sp_streq(name, "exception") ||
         ((sp_streq(name, "==") || sp_streq(name, "eql?")) &&
          (comp_ntype(c, argv[0]) == TY_EXCEPTION ||
           (ty_is_object(comp_ntype(c, argv[0])) &&
            class_is_exc_subclass(c, ty_object_class(comp_ntype(c, argv[0])))))))) {
      if (sp_streq(name, "exception")) { emit_exc_exception(c, recv, argv[0], b); return 1; }
      buf_puts(b, "sp_exc_eq((sp_Exception *)("); emit_expr(c, recv, b);
      buf_puts(b, "), (sp_Exception *)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if (argc == 0 && (sp_streq(name, "cause") || sp_streq(name, "backtrace") ||
                      sp_streq(name, "full_message") || sp_streq(name, "detailed_message") ||
                      sp_streq(name, "exception")) &&
        comp_method_in_chain(c, ty_object_class(comp_ntype(c, recv)), name, NULL) < 0) {
      if (sp_streq(name, "exception")) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "cause")) {
        buf_puts(b, "((sp_Exception *)("); emit_expr(c, recv, b); buf_puts(b, "))->cause");
        return 1;
      }
      if (sp_streq(name, "backtrace")) {
        int tbt = ++g_tmp;
        buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)(", tbt);
        emit_expr(c, recv, b);
        /* The base sp_Exception's `backtrace` field is mirrored by
         * every user exception subclass struct (codegen emits it in
         * the struct definition for ivar-bearing classes; nivars==0
         * subclasses are typedef'd to sp_Exception). So `_t->backtrace`
         * is valid for both shapes. CRuby returns nil for a fresh
         * exception that never had set_backtrace called; we do the
         * same. sp_StrArray_inspect handles NULL and prints "nil". */
        buf_printf(b, "); _t%d->backtrace; })", tbt);
        return 1;
      }
      { int tfm = ++g_tmp;
        Buf rbm = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Exception *_t%d = (sp_Exception *)(%s);\n", tfm, rbm.p ? rbm.p : "");
        free(rbm.p);
        const char *mfn = exc_has_user_msg_override(c) ? "sp_user_exc_message" : "sp_exc_message";
        if (sp_streq(name, "full_message"))
          buf_printf(b, "sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_t%d), %s(_t%d))", tfm, mfn, tfm);
        else
          buf_printf(b, "sp_sprintf(\"%%s (%%s)\", %s(_t%d), sp_exc_class_name(_t%d))", mfn, tfm, tfm);
      }
      return 1;
    }
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      /* the class's OWN to_s answers whatever it answers -- a Symbol, say --
         so it cannot go through the message helpers, whose result is a string
         (#3714) */
      int xcm = ty_object_class(comp_ntype(c, recv));
      /* only for the name the class itself defines: #message keeps the helper,
         whose result type the call site is typed for */
      int own = sp_streq(name, "message") ? -1 : comp_method_in_chain(c, xcm, name, NULL);
      /* #message is #to_s: an override answering something other than a String
         cannot ride the const char * helper, so call it directly (#3868) */
      if (own < 0 && sp_streq(name, "message")) {
        int mi8 = comp_method_in_chain(c, xcm, "to_s", NULL);
        if (mi8 >= 0 && (TyKind)c->scopes[mi8].ret != TY_STRING &&
            (TyKind)c->scopes[mi8].ret != TY_UNKNOWN)
          own = mi8;
      }
      /* a subclass overriding a String #to_s is picked at run time by the
         cls_name-keyed dispatcher below */
      if (own >= 0 && sp_streq(name, "to_s") && (TyKind)c->scopes[own].ret == TY_STRING) {
        int sub_str = 0, sub_other = 0;
        for (int k = 0; k < c->nclasses; k++) {
          if (k == xcm || !is_descendant(c, k, xcm)) continue;
          int ko = comp_method_in_class(c, k, "to_s");
          if (ko < 0) continue;
          if ((TyKind)c->scopes[ko].ret == TY_STRING) sub_str = 1;
          else sub_other = 1;
        }
        if (sub_str && !sub_other) own = -1;
      }
      if (own >= 0) {
        int defc = xcm;
        (void)comp_method_in_chain(c, xcm, c->scopes[own].name, &defc);
        if (g_plan_check) ucall_observe(c, id, own, defc, 0);
        buf_printf(b, "sp_%s_%s((sp_%s *)(", c->classes[defc].c_name,
                   mc(c->scopes[own].name), c->classes[defc].c_name);
        emit_expr(c, recv, b); buf_puts(b, "))");
        return 1;
      }
      const char *fn = exc_has_user_msg_override(c)
        ? (sp_streq(name, "message") ? "sp_user_exc_message" : "sp_user_exc_to_s")
        : "sp_exc_message";
      buf_printf(b, "%s((sp_Exception *)(", fn); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
  }

  /* exception object methods */
  /* Exception#backtrace: return the stored backtrace (attached by
   * #set_backtrace), or fall back to the stack captured at the most recent
   * raise. The receiver is `sp_Exception *` for the rescued variable; the
   * stored `backtrace` field is on the base sp_Exception struct, not on a
   * user subclass (whose fields are at different offsets), so read it only
   * when this is a base exception instance -- a user subclass's #backtrace
   * method dispatches before this rule can fire (the chain check at the
   * gate would have returned >= 0 for a user-defined #backtrace). For a
   * rescued user subclass, fall through to the empty sp_StrArray return
   * the legacy path already provided. */
  if (sp_streq(name, "backtrace") && argc == 0 && recv >= 0) {
    TyKind rrt = comp_ntype(c, recv);
    int exc_ok = (rrt == TY_EXCEPTION) ||
                 (ty_is_object(rrt) && class_is_exc_subclass(c, ty_object_class(rrt)));
    if (exc_ok && comp_method_in_chain(c, ty_object_class(rrt), name, NULL) < 0) {
      /* The base sp_Exception struct's backtrace field is the storage for
       * a builtin exception. A user exception subclass has a different
       * layout (the first 8 base fields, then its own ivars -- the base
       * struct's `backtrace` field is NOT in the user struct, so reading
       * it via a base cast is undefined). The user class's #backtrace
       * method (which dispatches before this rule) reads its own ivar.
       * The remaining case is a rescue variable whose concrete class the
       * analyze pass lost: read the user struct's first ivar, which is
       * at a known offset (after the 8 base fields shared by every user
       * exception subclass). The user class's #set_backtrace stored there. */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)(", t);
      emit_expr(c, recv, b);
      /* The base sp_Exception's `backtrace` field is mirrored by
       * every user exception subclass struct (codegen emits it in
       * the struct definition for ivar-bearing classes; nivars==0
       * subclasses are typedef'd to sp_Exception). So `_t->backtrace`
       * is valid for both shapes; no offset arithmetic, no risk
       * of reading an unrelated ivar as a backtrace. The chain
       * check above already stood down for any class that defines
       * its own #backtrace. */
      buf_printf(b, "); _t%d->backtrace; })", t);
      return 1;
    }
  }
  /* Exception#set_backtrace(bt): store the array on the exception so a
   * later #backtrace returns it. The gate fires for both a bare exception
   * receiver (TY_EXCEPTION) and a user exception subclass instance
   * (TY_OBJECT of a class that inherits Exception), and stands down for
   * a user class that defines its own set_backtrace. The arg arrives as
   * a sp_RbVal (the call site boxes); the runtime takes sp_StrArray * out
   * of the box and stores it on the sp_Exception. The return is
   * sp_box_obj(bt) so a chain like `e = e.set_backtrace(bt)` keeps the
   * array. */
  if (argc == 1 && sp_streq(name, "set_backtrace") && recv >= 0) {
    TyKind rrt = comp_ntype(c, recv);
    int exc_ok = (rrt == TY_EXCEPTION) ||
                 (ty_is_object(rrt) && class_is_exc_subclass(c, ty_object_class(rrt)));
    if (exc_ok && comp_method_in_chain(c, ty_object_class(rrt), name, NULL) < 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)((void*)(", t);
      emit_expr(c, recv, b);
      buf_printf(b, ")); sp_Exception_set_backtrace(_t%d, ", t);
      emit_expr(c, argv[0], b);
      buf_puts(b, "); })");
      return 1;
    }
  }
  /* exception accessors on a POLY receiver (an exception rescued into a
     union-typed local): runtime unbox-and-delegate, but only when no user
     class defines the name (which would need the poly method dispatch)
     (#3120, #3122). That dispatch's builtin default arm, for a receiver
     none of those classes own, lands here too. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY && argc == 0 &&
      nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "message") || sp_streq(name, "result") ||
       sp_streq(name, "errno") ||
       sp_streq(name, "key") || sp_streq(name, "receiver") ||
       sp_streq(name, "backtrace") || sp_streq(name, "cause") ||
       sp_streq(name, "full_message") || sp_streq(name, "detailed_message"))) {
    int pu = 0;
    for (int k = 0; k < c->nclasses && !pu; k++)
      if (comp_method_in_class(c, k, name) >= 0 ||
          comp_reader_in_chain(c, k, name, NULL)) pu = 1;
    if (!pu || g_poly_builtin_arm) {
      if (sp_streq(name, "name")) g_uses_symbols = 1;  /* may intern a recovered name */
      /* a user exception's #to_s is what #message answers, so a boxed
         exception goes through the override dispatcher */
      if (sp_streq(name, "message") &&
          (exc_has_user_msg_override(c) || exc_has_nonstring_msg_override(c))) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", t);
        emit_expr(c, recv, b);
        buf_printf(b, "; (_t%d.tag == SP_TAG_OBJ && _t%d.v.p && (_t%d.cls_id == SP_BUILTIN_EXCEPTION"
                   " || sp_is_exc_subclass_cls(_t%d.cls_id))) ? ", t, t, t, t);
        int boxed = comp_ntype(c, id) == TY_POLY;
        const char *arm = exc_has_nonstring_msg_override(c)
          ? (boxed ? "sp_user_exc_message_v(%s)" : "sp_poly_to_s(sp_user_exc_message_v(%s))")
          : (boxed ? "sp_box_str(sp_user_exc_message(%s))" : "sp_user_exc_message(%s)");
        char ep[64];
        snprintf(ep, sizeof ep, "(sp_Exception *)_t%d.v.p", t);
        buf_printf(b, arm, ep);
        if (boxed) buf_printf(b, " : sp_poly_exc_acc(_t%d, \"message\"); })", t);
        else buf_printf(b, " : sp_poly_to_s(sp_poly_exc_acc(_t%d, \"message\")); })", t);
        return 1;
      }
      /* the renderings read the overridden #message, as the typed receiver's do */
      if ((sp_streq(name, "full_message") || sp_streq(name, "detailed_message")) &&
          exc_has_user_msg_override(c)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", t);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_Exception *_e%d = (_t%d.tag == SP_TAG_OBJ && _t%d.v.p && "
                      "(_t%d.cls_id == SP_BUILTIN_EXCEPTION || sp_is_exc_subclass_cls(_t%d.cls_id)))"
                      " ? (sp_Exception *)_t%d.v.p : NULL; ", t, t, t, t, t, t);
        if (sp_streq(name, "full_message"))
          buf_printf(b, "_e%d ? sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_e%d), sp_user_exc_message(_e%d))",
                     t, t, t);
        else
          buf_printf(b, "_e%d ? sp_sprintf(\"%%s (%%s)\", sp_user_exc_message(_e%d), sp_exc_class_name(_e%d))",
                     t, t, t);
        buf_printf(b, " : sp_poly_to_s(sp_poly_exc_acc(_t%d, \"%s\")); })", t, name);
        return 1;
      }
      /* message and the two renderings infer TY_STRING: unwrap the boxed
         accessor result */
      int unwrap = comp_ntype(c, id) == TY_STRING;
      if (unwrap) buf_puts(b, "sp_poly_to_s(");
      buf_printf(b, "sp_poly_exc_acc(");
      emit_expr(c, recv, b);
      buf_printf(b, ", \"%s\")", name);
      if (unwrap) buf_puts(b, ")");
      return 1;
    }
  }
  /* a method a reopening of a builtin exception class defined (`class
     LoadError; def is_missing?`): the receiver is the runtime's sp_Exception --
     base-typed, or a user subclass instance whose own chain lacks the method
     or reaches it through the reopened parent. Several reopenings defining
     the name (Exception#brief and KeyError#brief) are told apart by the
     runtime class: the one nearest it up its ancestry answers, a base
     Exception reopening for every class. */
  /* #message / #to_s take the runtime's message dispatchers, which pick a
     reopening's override by the runtime class and fall back to the stored
     message (sp_user_exc_message / sp_user_exc_to_s) */
  if (recv >= 0 && !sp_streq(name, "message") && !sp_streq(name, "to_s")) {
    TyKind xrt = comp_ntype(c, recv);
    int xdef = -1, xob = ty_is_object(xrt) ? ty_object_class(xrt) : -1;
    int xhit = xob >= 0 && class_is_exc_subclass(c, xob)
                 ? comp_method_in_chain(c, xob, name, &xdef) : -1;
    if (xrt == TY_EXCEPTION ||
        (xob >= 0 && class_is_exc_subclass(c, xob) &&
         (xhit < 0 || (xdef >= 0 && is_builtin_reopen(c->classes[xdef].name))))) {
      int xr[8];
      int xn = exc_reopen_definers(c, name, xr, 8);
      if (xn > 0) {
        int xt = ++g_tmp;
        buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)(", xt);
        emit_expr(c, recv, b);
        buf_puts(b, "); ");
        char xcls[48]; snprintf(xcls, sizeof xcls, "_t%d->cls_name", xt);
        int pk = emit_exc_reopen_pick_head(c, xr, xn, xcls, b);
        /* a runtime class none of them is above has no such method */
        buf_printf(b, "if (_xi%d < 0) sp_raise_nomethod(sp_nomethod_msg(\"%s\", ", pk, name);
        char xv[32]; snprintf(xv, sizeof xv, "_t%d", xt);
        emit_boxed_text(c, TY_EXCEPTION, xv, b);
        buf_puts(b, ")); ");
        /* definers that answer different types meet as a boxed value */
        int xpoly = comp_ntype(c, id) == TY_POLY;
        for (int q = 0; q < xn; q++) {
          int mi = comp_method_in_chain(c, xr[q], name, NULL);
          if (g_plan_check) ucall_observe(c, id, mi, xr[q], 1);   /* one definer's arm */
          if (q != xn - 1) buf_printf(b, "_xi%d == %d ? ", pk, q);
          Buf cb; memset(&cb, 0, sizeof cb);
          buf_printf(&cb, "sp_%s_%s(_t%d", mc_reopen_cls(c, xr[q], name), mc(name), xt);
          emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), ", ", &cb);
          buf_puts(&cb, ")");
          TyKind mrt = (TyKind)c->scopes[mi].ret;
          if (xpoly && mrt != TY_POLY) emit_boxed_text(c, mrt, cb.p, b);
          else buf_puts(b, cb.p);
          free(cb.p);
          if (q != xn - 1) buf_puts(b, " : ");
        }
        buf_puts(b, "; })");
        return 1;
      }
    }
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_EXCEPTION) {
    /* equal? and eql? are pointer identity; == and === are CRuby's value
       equality (same class and message): Object's protocol arm, which also
       unwraps a poly operand and stands down for a user subclass's own
       definition */
    if (argc == 1 &&
        (sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "===") ||
         sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
        emit_native_object_protocol(c, id, b)) return 1;
    if (sp_streq(name, "nil?") && argc == 0) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
      return 1;
    }
    /* a raised/constructed exception is not frozen in CRuby (#3004), but one
       the program froze itself reads back frozen (#3709) */
    if (sp_streq(name, "frozen?") && argc == 0) {
      buf_puts(b, "sp_gc_is_frozen((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (sp_streq(name, "freeze") && argc == 0) {
      buf_puts(b, "((sp_Exception *)sp_gc_freeze((void *)("); emit_expr(c, recv, b); buf_puts(b, ")))");
      return 1;
    }

    /* dup/clone copy the whole (subclass-sized) struct so mutating the copy
       leaves the original alone (#2772) */
    if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
      buf_puts(b, "sp_exc_dup((sp_Exception *)(");
      emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    /* Exception#exception: no-arg returns the receiver; with a message it is
       a copy carrying the new message (#2740) */
    if (sp_streq(name, "exception")) {
      if (argc == 0) { emit_expr(c, recv, b); return 1; }
      if (argc == 1) { emit_exc_exception(c, recv, argv[0], b); return 1; }
    }
    /* NameError/NoMethodError#name: the carried missing name; any other
       exception class raises NoMethodError at runtime, per CRuby */
    if (sp_streq(name, "name") && argc == 0) {
      g_uses_symbols = 1;  /* the accessor interns a runtime-recovered name (#2758) */
      buf_puts(b, "sp_exc_name_acc((sp_Exception *)(");
      emit_expr(c, recv, b);
      buf_puts(b, "))");
      return 1;
    }
    /* class-gated introspection accessors (#2753-#2756, #2770) */
    if (argc == 0) {
      const char *accfn = exc_gated_acc_fn(name);
      if (accfn) {
        if (sp_streq(name, "reason") || sp_streq(name, "tag") || sp_streq(name, "key") ||
            sp_streq(name, "name"))
          g_uses_symbols = 1;  /* staged names intern back to symbols */
        buf_printf(b, "%s((sp_Exception *)(", accfn);
        emit_expr(c, recv, b);
        buf_puts(b, "))");
        return 1;
      }
    }
    if (sp_streq(name, "inspect") && argc == 0) {
      /* an empty message renders as the bare class name (#3713) */
      buf_puts(b, "sp_exc_inspect((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      /* NULL-guard: a nil $! (outside any rescue) has no message. */
      int t = hoist_exc_recv(c, recv);
      /* An override answering something other than a String cannot ride the
         const char * dispatcher; the boxed pair carries it, and the call types
         poly to match (#3868). */
      if (exc_has_nonstring_msg_override(c) && comp_ntype(c, id) == TY_POLY) {
        buf_printf(b, "(_t%d ? %s(_t%d) : sp_box_str(sp_str_empty))", t,
                   sp_streq(name, "message") ? "sp_user_exc_message_v" : "sp_user_exc_to_s_v", t);
        return 1;
      }
      const char *fn = exc_has_user_msg_override(c)
        ? (sp_streq(name, "message") ? "sp_user_exc_message" : "sp_user_exc_to_s")
        : "sp_exc_message";
      buf_printf(b, "(_t%d ? %s(_t%d) : sp_str_empty)", t, fn, t);
      return 1;
    }
    if (sp_streq(name, "cause")) {
      buf_puts(b, "sp_exc_cause("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "result") && argc == 0) {
      /* StopIteration#result: the finished iteration's return value. */
      buf_puts(b, "sp_exc_result("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "errno") && argc == 0) {
      /* SystemCallError#errno: the number of the Errno:: class, by name (#4560) */
      buf_puts(b, "sp_exc_errno_acc("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "full_message")) {
      int t = hoist_exc_recv(c, recv);
      buf_printf(b, "sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return 1;
    }
    /* detailed_message -> "message (ClassName)" (kwargs like highlight: ignored) */
    if (sp_streq(name, "detailed_message")) {
      int t = hoist_exc_recv(c, recv);
      buf_printf(b, "sp_sprintf(\"%%s (%%s)\", sp_exc_message(_t%d), sp_exc_class_name(_t%d))", t, t);
      return 1;
    }
    if (sp_streq(name, "inspect")) {
      /* #<ClassName: message>, or "nil" for a nil $! (outside any rescue). */
      int t = hoist_exc_recv(c, recv);
      /* an empty message renders as the bare class name (#3713) */
      buf_printf(b, "sp_exc_inspect((void *)_t%d)", t);
      return 1;
    }
    if (sp_streq(name, "class")) {  /* a Class carried by name (complete for every exception class) */
      /* a nil $! (outside any rescue) is NilClass, matching the sibling nil-guards. */
      int t = hoist_exc_recv(c, recv);
      buf_printf(b, "((sp_Class){0, _t%d ? sp_exc_class_name(_t%d) : SPL(\"NilClass\")})", t, t);
      return 1;
    }
    /* object identity: the same raised object compares equal to $! / a `=> e`
       binding, since both now point at the one materialized exception. */
    if (argc == 1 && sp_streq(name, "equal?")) {
      /* Only an exception arg can share identity with the receiver; nil compares
         against a NULL pointer. Any other type is a struct or scalar that can't
         be cast to void* (a -Werror break) and can never be the same object. */
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_EXCEPTION) {
        Buf rb = expr_buf(c, recv), ab = expr_buf(c, argv[0]);
        buf_printf(b, "((void *)(%s) == (void *)(%s))", rb.p ? rb.p : "0", ab.p ? ab.p : "0");
        free(rb.p); free(ab.p);
      }
      else if (at == TY_NIL) {
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "((void *)(%s) == NULL)", rb.p ? rb.p : "0");
        free(rb.p);
      }
      else {
        buf_puts(b, "0");
      }
      return 1;
    }
    if (sp_streq(name, "backtrace")) {
      /* the stack captured at the most recent raise (sp_bt_buf); the substrate
         is live in --debug builds and empty in release, same as Kernel#caller. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_backtrace_captured())");
      return 1;
    }
    if (argc == 1 && is_kind_query(name)) {
      /* exception class names are registered fully qualified ("PG::Error"),
         so a nested-path argument must compare with the whole path -- the
         flat leaf name never matched (#3260) */
      char qbuf[192];
      const char *cn = isa_const_qualname(nt, argv[0], qbuf, sizeof qbuf);
      if (cn) {
        /* instance_of? is an exact-class test, not an ancestor walk: an
           ArgumentError is not instance_of?(StandardError) (#3013) */
        if (sp_streq(name, "instance_of?")) {
          buf_puts(b, "(strcmp(sp_exc_class_name("); emit_expr(c, recv, b);
          buf_printf(b, "), \"%s\") == 0)", cn);
        }
        else {
          buf_puts(b, "sp_exc_is_a("); emit_expr(c, recv, b);
          buf_printf(b, ", \"%s\")", cn);
        }
        return 1;
      }
    }
  }
  return 0;
}

/* raise / fail as an expression */
int emit_call_raise_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv) {
  /* raise */
  /* `fail` is an exact alias of `Kernel#raise`. */
  if (recv < 0 && !bare_call_class_owned(c, id) && (sp_streq(name, "raise") || sp_streq(name, "fail"))) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    /* Resolve a raise target's runtime class name: a ConstantPathNode naming a
       known builtin namespaced exception (Math::DomainError) raises under its
       qualified name, so the same-named rescue arm catches it (#2570). */
    char qexc_buf[160];
    #define RAISE_EXC_NAME(nd, leaf) ({ \
        const char *_ln = (leaf); \
        if (nt_type(nt, nd) && sp_streq(nt_type(nt, nd), "ConstantPathNode")) { \
          int _par = nt_ref(nt, nd, "parent"); \
          const char *_pnm = (_par >= 0 && nt_type(nt, _par) && \
                              sp_streq(nt_type(nt, _par), "ConstantReadNode")) \
                             ? nt_str(nt, _par, "name") : NULL; \
          if (_pnm && _ln) { \
            snprintf(qexc_buf, sizeof qexc_buf, "%s::%s", _pnm, _ln); \
            if (is_exc_name(qexc_buf)) _ln = qexc_buf; \
          } \
        } \
        _ln; })
    /* trailing `cause: exc` kwarg: strip it from the arg list and stage the
       explicit cause the raise machinery consumes for this one raise */
    int cause_node = -1;
    if (ac >= 2 && nt_type(nt, av[ac - 1]) &&
        sp_streq(nt_type(nt, av[ac - 1]), "KeywordHashNode")) {
      int kn = 0;
      const int *kel = nt_arr(nt, av[ac - 1], "elements", &kn);
      if (kn == 1 && nt_type(nt, kel[0]) && sp_streq(nt_type(nt, kel[0]), "AssocNode")) {
        int kk = nt_ref(nt, kel[0], "key");
        if (kk >= 0 && nt_type(nt, kk) && sp_streq(nt_type(nt, kk), "SymbolNode") &&
            nt_str(nt, kk, "value") && sp_streq(nt_str(nt, kk, "value"), "cause")) {
          cause_node = nt_ref(nt, kel[0], "value");
          ac--;
        }
      }
    }
    if (cause_node >= 0) {
      /* record that cause: was given (so cause: nil suppresses the implicit
         cause), then evaluate it -- a nil literal / value carries as NULL (#2990) */
      int cause_is_nil = nt_type(nt, cause_node) && sp_streq(nt_type(nt, cause_node), "NilNode");
      /* The cause must be an exception or nil: one the program can be known
         to hold as anything else is CRuby's TypeError, raised once the first
         argument is known to be raisable (a wrong first argument is the
         first error) and before the exception. Its value is not staged. A
         cause that is a literal, or of a type that has no nil, raises after
         the message operands ran, in place of the exception; one that may
         hold nil at run time (a String or an object read from a container)
         raises only when it is not nil, and before the operands. */
      TyKind ckt = comp_ntype(c, cause_node);
      const char *cty0 = nt_type(nt, cause_node);
      int cause_lit = cty0 && (sp_streq(cty0, "StringNode") || sp_streq(cty0, "InterpolatedStringNode") ||
                               sp_streq(cty0, "IntegerNode") || sp_streq(cty0, "FloatNode") ||
                               sp_streq(cty0, "SymbolNode") || sp_streq(cty0, "TrueNode") ||
                               sp_streq(cty0, "FalseNode") || sp_streq(cty0, "ArrayNode") ||
                               sp_streq(cty0, "HashNode") || sp_streq(cty0, "RangeNode") ||
                               sp_streq(cty0, "RationalNode") || sp_streq(cty0, "ImaginaryNode"));
      /* a Float that may be nil (a parameter some caller leaves out) is not
         known to be never nil: it is judged at run time with the rest */
      int cause_never_nil = cause_lit ||
                            (ckt == TY_FLOAT && !call_returns_nullable_int(c, cause_node)) ||
                            ckt == TY_BOOL || ckt == TY_SYMBOL ||
                            ckt == TY_CLASS || ckt == TY_RANGE;
      int cause_exc = cause_is_nil || (!cause_lit && (ckt == TY_NIL || ckt == TY_EXCEPTION || ckt == TY_POLY || ckt == TY_UNKNOWN ||
                      (ty_is_object(ckt) && class_is_exc_subclass(c, ty_object_class(ckt)))));
      int first_const = ac > 0 && nt_type(nt, av[0]) &&
                        (sp_streq(nt_type(nt, av[0]), "ConstantReadNode") || sp_streq(nt_type(nt, av[0]), "ConstantPathNode"));
      const char *first_cn = first_const ? nt_str(nt, av[0], "name") : NULL;
      int first_xc = first_cn ? comp_class_index(c, first_cn) : -1;
      TyKind first_ty = ac > 0 && !first_const ? comp_ntype(c, av[0]) : TY_UNKNOWN;
      /* a class of the program with its own initialize is built before the
         cause is judged, which this path does not do: it keeps the old one.
         A first argument typed as an exception may hold nil at run time
         (`raise $!`), and nil is the first error: it is tested here, after
         the operands ran, and only for a cause that cannot hold nil. */
      int first_exc_val = !first_const && ac > 0 &&
                          (first_ty == TY_EXCEPTION ||
                           (ty_is_object(first_ty) && class_is_exc_subclass(c, ty_object_class(first_ty))));
      int first_ok = first_const ? (first_xc >= 0 ? (class_is_exc_subclass(c, first_xc) &&
                                                       comp_method_in_chain(c, first_xc, "initialize", NULL) < 0)
                                                  : (first_cn && is_exc_name(first_cn)))
                   : (ac == 1 && first_ty == TY_STRING) || (first_exc_val && cause_never_nil);
      int cause_bad = !cause_exc && first_ok;
      if (cause_bad && cause_never_nil) {
        /* the operands run once, here, and the TypeError is the whole answer */
        if (first_exc_val) {
          int tf = ++g_tmp;
          buf_printf(b, "({ void *_t%d = (void *)(", tf); emit_expr(c, av[0], b); buf_puts(b, ");");
          if (ac > 1) { buf_puts(b, " (void)("); emit_boxed(c, av[1], b); buf_puts(b, ");"); }
          buf_puts(b, " (void)("); emit_boxed(c, cause_node, b); buf_puts(b, ");");
          buf_printf(b, " if (!_t%d) sp_raise_cls(\"TypeError\", \"exception class/object expected\");", tf);
          buf_puts(b, " sp_raise_cls(\"TypeError\", \"exception object expected\"); })");
          return 1;
        }
        buf_puts(b, "(");
        if (ac > 0 && !first_const) { buf_puts(b, "(void)("); emit_expr(c, av[0], b); buf_puts(b, "), "); }
        if (ac > 1) { buf_puts(b, "(void)("); emit_boxed(c, av[1], b); buf_puts(b, "), "); }
        buf_puts(b, "(void)("); emit_boxed(c, cause_node, b); buf_puts(b, "), ");
        buf_puts(b, "sp_raise_cls(\"TypeError\", \"exception object expected\"))");
        return 1;
      }
      buf_puts(b, "(");
      if (cause_bad) {
        int tc = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tc); emit_boxed(c, cause_node, b);
        buf_printf(b, "; if (_t%d.tag != SP_TAG_NIL) sp_raise_cls(\"TypeError\", \"exception object expected\"); }), ", tc);
      }
      else if (!cause_exc) {
        buf_puts(b, "(void)("); emit_boxed(c, cause_node, b); buf_puts(b, "), ");
      }
      buf_puts(b, "sp_explicit_cause_set = 1, sp_explicit_cause = (void *)(");
      if (cause_is_nil || !cause_exc) buf_puts(b, "0");
      else emit_expr(c, cause_node, b);
      buf_puts(b, "), ");
    }
    if (ac == 0) {
      /* a bare re-raise keeps the handled exception's own cause (#3745) */
      if (g_rescue_cls) buf_printf(b, "(sp_reraise_current = 1, sp_raise_cls(%s, %s))", g_rescue_cls, g_rescue_msg);
      else buf_puts(b, "sp_raise(sp_exc_no_msg)");   /* a bare raise's message is "" (#3711) */
    }
    else if (ac == 1 && nt_type(nt, av[0]) &&
             (sp_streq(nt_type(nt, av[0]), "ConstantReadNode") || sp_streq(nt_type(nt, av[0]), "ConstantPathNode")) &&
             /* a constant holding an exception INSTANCE (`ERR = RuntimeError.new(..)`;
                `raise ERR`) is a value, not a class name: it takes the object
                path below, or it raised a class called "ERR" */
             !(comp_class_index(c, nt_str(nt, av[0], "name")) < 0 &&
               (comp_ntype(c, av[0]) == TY_EXCEPTION ||
                (ty_is_object(comp_ntype(c, av[0])) &&
                 class_is_exc_subclass(c, ty_object_class(comp_ntype(c, av[0]))))))) {
      /* `raise E` with a user-defined E#initialize is `raise E.new`: construct
         the object (filling initialize's defaults) so its custom initialize and
         any `super`/message run. Without a custom initialize the message
         defaults to the class name, so the (cls, "") fast path is correct. */
      const char *cn = nt_str(nt, av[0], "name");
      int xc = cn ? comp_class_index(c, cn) : -1;
      /* a reopened builtin exception is still the builtin: raised by name,
         through the reopening's own initialize when it has one */
      if (xc >= 0 && cn && is_exc_name(cn) && is_builtin_reopen(cn)) {
        int rim = exc_reopen_initialize(c, xc);
        if (rim >= 0) {
          buf_puts(b, "sp_raise_exc(");
          emit_exc_reopen_construct(c, xc, rim, -1, -1, b);
          buf_puts(b, ")");
          if (cause_node >= 0) buf_puts(b, ")");
          return 1;
        }
        xc = -1;
      }
      int ic = (xc >= 0 && class_is_exc_subclass(c, xc))
                 ? comp_method_in_chain(c, xc, "initialize", NULL) : -1;
      if (xc >= 0 && ic >= 0 && c->scopes[ic].reachable && ctor_needs_self_defaults(c, ic, 0)) {
        /* a default reading the instance runs on the allocated object, as
           the same `.new` does (emit_ctor_alloc_init) */
        buf_puts(b, "sp_raise_exc((sp_Exception *)");
        emit_ctor_alloc_init(c, xc, ic, -1, -1, b);
        buf_puts(b, ")");
      }
      else if (xc >= 0 && ic >= 0 && c->scopes[ic].reachable) {
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].c_name);
        emit_args_filled(c, ic, -1, "", b);
        if (ctor_init_takes_block(c, ic)) buf_puts(b, c->scopes[ic].nparams > 0 ? ", NULL" : "NULL");
        buf_puts(b, "))");
      }
      else if ((xc >= 0 && !class_is_exc_subclass(c, xc)) ||
               (xc < 0 && cn && is_builtin_class_name(cn) && !is_exc_name(cn) &&
                !is_exc_name(RAISE_EXC_NAME(av[0], cn)))) {
        /* raising a non-Exception class is CRuby's TypeError (#2771) */
        buf_puts(b, "sp_raise_cls(\"TypeError\", \"exception class/object expected\")");
      }
      else {
        /* a known user class raises under its qualified Ruby name (matching
           the constructor emission and the rescue-arm canonicalization) */
        const char *rn = (xc >= 0) ? class_ruby_name(c, xc) : NULL;
        const char *effn = rn ? rn : (cn ? RAISE_EXC_NAME(av[0], cn) : "");
        /* `raise E` is `raise E.new`: in the SystemCallError family its
           initialize builds the errno text (SystemCallError.new itself
           wants a message and raises ArgumentError) */
        if ((xc >= 0 && class_is_syserr(c, xc)) || (xc < 0 && is_syserr_family_name(effn))) {
          if (sp_streq(effn, "SystemCallError"))
            buf_puts(b, "sp_raise_exc(sp_syserr_new_v(0, NULL))");
          else
            buf_printf(b, "sp_raise_cls(\"%s\", sp_syserr_msg_a(\"%s\", 0, NULL))", effn, effn);
        }
        else buf_printf(b, "sp_raise_cls(\"%s\", (&(\"\\xff\")[1]))", effn);
      }
    }
    else if (ac >= 2 && nt_type(nt, av[0]) &&
             (sp_streq(nt_type(nt, av[0]), "ConstantReadNode") || sp_streq(nt_type(nt, av[0]), "ConstantPathNode"))) {
      /* `raise Cls, arg` on a user exception subclass with an initialize is
         `raise Cls.new(arg)`: construct the object so its ivars are set and
         the message comes from the class's initialize/super, then carry it.
         A bare-string/builtin exception keeps the (cls, msg) fast path. */
      const char *cn = nt_str(nt, av[0], "name");
      int xc = cn ? comp_class_index(c, cn) : -1;
      /* a reopened builtin exception is still the builtin: raised by name,
         through the reopening's own initialize when it has one */
      if (xc >= 0 && cn && is_exc_name(cn) && is_builtin_reopen(cn)) {
        int rim = exc_reopen_initialize(c, xc);
        if (rim >= 0 && c->scopes[rim].nparams >= 1) {
          buf_puts(b, "sp_raise_exc(");
          emit_exc_reopen_construct(c, xc, rim, -1, av[1], b);
          buf_puts(b, ")");
          if (cause_node >= 0) buf_puts(b, ")");
          return 1;
        }
        xc = -1;
      }
      int ic = -1;
      if (xc >= 0 && class_is_exc_subclass(c, xc))
        ic = comp_method_in_chain(c, xc, "initialize", NULL);
      if (xc >= 0 && ic >= 0 && c->scopes[ic].reachable && ctor_needs_self_defaults(c, ic, 1)) {
        /* a default the message leaves reading the instance runs on the
           allocated object, as for `Cls.new(msg)` */
        buf_puts(b, "sp_raise_exc((sp_Exception *)");
        emit_ctor_alloc_init_argv(c, xc, ic, &av[1], 1, args, b);
        buf_puts(b, ")");
      }
      else if (xc >= 0 && ic >= 0 && c->scopes[ic].reachable) {
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].c_name);
        /* `raise Cls, msg` is `raise Cls.new(msg)`: the message binds as the
           one argument of that call does (a rest takes it as its element, a
           **kwrest is empty, a default reads an earlier parameter), and an
           initialize taking none refuses it with the wrong count. Each other
           parameter took its default and the message went to the parameter
           the layout named, so `initialize()` answered the message, `(*r)`
           the bare element and a **kwrest nil. */
        emit_args_filled_argv(c, ic, &av[1], 1, args, "", b);
        if (ctor_init_takes_block(c, ic)) buf_puts(b, c->scopes[ic].nparams > 0 ? ", NULL" : "NULL");
        buf_puts(b, "))");
      }
      else if ((xc >= 0 && !class_is_exc_subclass(c, xc)) ||
               (xc < 0 && cn && is_builtin_class_name(cn) && !is_exc_name(cn) &&
                !is_exc_name(RAISE_EXC_NAME(av[0], cn)))) {
        /* raising a non-Exception class is CRuby's TypeError (#2771) */
        buf_puts(b, "((void)(");
        emit_boxed(c, av[1], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", \"exception class/object expected\"))");
      }
      else {
        /* a known user class raises under its qualified Ruby name (matching
           the constructor emission and the rescue-arm canonicalization);
           any object can be the message -- non-String coerces via to_s (#2741) */
        const char *rn = (xc >= 0) ? class_ruby_name(c, xc) : NULL;
        const char *effn = rn ? rn : RAISE_EXC_NAME(av[0], cn);
        /* `raise SignalException, "INT"` resolves the signal name so #signo and
           the "SIG<name>" message are carried, matching SignalException.new (#3074) */
        if (effn && sp_streq(effn, "SignalException")) {
          buf_puts(b, "sp_raise_exc(sp_signal_exc_new(");
          emit_boxed(c, av[1], b);
          buf_puts(b, "))");
        }
        /* `raise Errno::ENOENT, "x"` is Errno::ENOENT.new("x"): the errno
           text, " - x" (SystemCallError.new("x") an unknown error) */
        else if ((xc >= 0 && class_is_syserr(c, xc)) || (xc < 0 && is_syserr_family_name(effn))) {
          if (sp_streq(effn, "SystemCallError")) {
            buf_puts(b, "sp_raise_exc(");
            emit_syserr_call(c, id, "sp_syserr_new_v", "", 1, &av[1], b);
            buf_puts(b, ")");
          }
          else {
            char lead[192]; snprintf(lead, sizeof lead, "\"%s\", ", effn);
            buf_printf(b, "sp_raise_cls(\"%s\", ", effn);
            emit_syserr_call(c, id, "sp_syserr_msg_a", lead, 1, &av[1], b);
            buf_puts(b, ")");
          }
        }
        else {
          /* an explicitly given message is kept even when empty, unlike the
             class-only form, which falls back to the class name (#3711) */
          if (comp_ntype(c, av[1]) == TY_STRING) {
            buf_printf(b, "sp_raise_cls(\"%s\", sp_exc_msg_given(", effn);
            emit_expr(c, av[1], b);
            buf_puts(b, "))");
          }
          else {
            /* nil is not a message, so the class name answers (#3812) */
            int rt2 = ++g_tmp;
            buf_printf(b, "({ sp_RbVal _t%d = ", rt2); emit_boxed(c, av[1], b);
            buf_printf(b, "; sp_raise_cls(\"%s\", _t%d.tag == SP_TAG_NIL ? (&(\"\\xff\")[1])"
                          " : sp_exc_msg_given(sp_poly_to_s(_t%d))); })", effn, rt2, rt2);
          }
        }
      }
    }
    else {
      TyKind at = ac > 0 ? comp_ntype(c, av[0]) : TY_UNKNOWN;
      if (ac >= 2 && (at == TY_POLY || at == TY_CLASS))
        emit_raise_class_value(c, av[0], av[1], b);
      else if (ac >= 2 && (at == TY_EXCEPTION ||
                           (ty_is_object(at) && class_is_exc_subclass(c, ty_object_class(at))))) {
        /* `raise e, msg` with an exception object in a variable: the message
           was dropped and the class name answered */
        int rk = ++g_tmp, rm = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", rk); emit_boxed(c, av[0], b);
        buf_printf(b, "; sp_RbVal _t%d = ", rm); emit_boxed(c, av[1], b);
        buf_printf(b, "; sp_raise_poly_msg(_t%d, _t%d); })", rk, rm);
      }
      else if (at == TY_EXCEPTION)
        { buf_puts(b, "sp_raise_exc((sp_Exception *)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (ty_is_object(at) && class_is_exc_subclass(c, ty_object_class(at)))
        /* an exception-subclass INSTANCE in a variable raises as itself */
        { buf_puts(b, "sp_raise_exc((sp_Exception *)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (ty_is_object(at)) {
        /* CRuby: raising a non-exception object is TypeError, not a
           pointer smuggled into the message slot (which emitted a C
           warning and raised garbage). Evaluate the operand for effect. */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", \"exception class/object expected\"))");
      }
      else if (at == TY_STRING && ac >= 2 && raise_plain_arg(nt, av[1])) {
        /* a String and then anything is not an exception class and message:
           both operands run, then CRuby's TypeError. A splat or keywords may
           be empty, so they keep the path below */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), (void)("); emit_boxed(c, av[1], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", \"exception class/object expected\"))");
      }
      else if (at == TY_STRING) {
        /* `raise "msg"` raises RuntimeError with the message */
        buf_puts(b, "sp_raise(sp_exc_msg_given("); emit_expr(c, av[0], b); buf_puts(b, "))");
      }
      else if (at == TY_POLY || at == TY_CLASS) {
        /* the runtime value may be a string, an exception object, an exception
           CLASS reached through a variable, or a non-exception (TypeError) --
           dispatch on the tag. Only the base Exception (SP_BUILTIN_EXCEPTION)
           is handled by the runtime; user exception subclasses are re-raised
           by the codegen when their static type is known (see the
           ty_is_object branch above). Reading parent_cls_name on an
           arbitrary poly-tagged object is a wrong-offset read (segfault). */
        emit_raise_class_value(c, av[0], -1, b);
      }
      else {
        /* Integer/nil/Array/Symbol/Float/...: valid Ruby, TypeError at
           runtime. The old path smuggled the value into the const char*
           message slot -- a C type error for scalars, garbage for the rest. */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", \"exception class/object expected\"))");
      }
    }
    if (cause_node >= 0) buf_puts(b, ")");
    #undef RAISE_EXC_NAME
    return 1;
  }
  return 0;
}
