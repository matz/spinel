/* codegen_call_method.c -- emit_call_body's arms of Method, UnboundMethod and Proc values.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* calling a Method or a Proc (call / () / [] / ===), composing Procs (<< >>), and a Proc's
   own methods (its builtin-op rows, parameters, source_location) */
int emit_call_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* <method>.call(args) / [] -> invoke the bound function. A top-level
     method ref calls its function directly; an object-bound Method casts
     fn through the (void *self, sp_int...) ABI, evaluating recv once. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]") ||
       sp_streq(name, "==="))) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    /* A bare `method(:sym)` in an instance method names no receiver but binds
       the enclosing self: it is receiverless in syntax only, so it must take
       the object-bound arm (which passes _t->self and the fn cast) rather than
       the self-less direct call, which dropped self and did not compile. A
       top-level def and a class method stay self-less. */
    int target_selfless = target_recvless &&
        !(target >= 0 && c->scopes[target].class_id >= 0 && !c->scopes[target].is_cmethod);
    if (target >= 0 && target_selfless) {
      /* A top-level target is called the way its name would be: the ordinary
         argument lowering binds the site's arguments to the target's
         parameters -- an omitted optional takes its default, a rest parameter
         takes the surplus as one array, a keyword lands in its own slot, a
         splat spreads at run time -- and a count or keyword the target cannot
         take is CRuby's ArgumentError ahead of the call. Handed over one C
         argument per site argument, `def f(*a)` took a raw Integer in its
         array pointer, `def f(a, b = 2)` was a C call one argument short, and
         a wrong count was the C compiler's error. */
      Scope *tms = &c->scopes[target];
      emit_method_cname(c, tms, b);
      buf_puts(b, "(");
      emit_args_filled(c, target, nt_ref(nt, id, "arguments"), "", b);
      emit_method_call_block(c, id, tms, tms->nparams > 0, b);
      buf_puts(b, ")");
      return 1;
    }
    if (target >= 0 && !method_call_param_shift(c, mn, target)) {
      emit_bound_method_call(c, id, recv, target, b);
      return 1;
    }
    /* What is left is a Method whose target is unresolved (`self.class.method(:m)`,
       a Method that arrived through a slot), a typed-array adapter
       (`<array>.method(:op)`), or the wrapper of a bound builtin
       (`"ab".method(:center)`), whose first parameter is the receiver the
       Method carries as self. The fn is cast through the wrapper's signature,
       or the ABI the bind site stamped, and called once. */
    int tr = ++g_tmp;
    Scope *tm = target >= 0 ? &c->scopes[target] : NULL;
    /* a bound __bam wrapper carries param[0] in the Method's self slot:
       call arg k is coerced to param[k + shift] */
    int shift = target >= 0 ? method_call_param_shift(c, mn, target) : 0;
    /* A binop wrapper (`__bam_r <op> __bam_a`) has a real operand parameter:
       a call without one is CRuby's ArgumentError, where it read the padding
       zero as the operand (`arr.method(:[]).call()` answered `arr[0]`). */
    if (tm && bam_binop_wrapper(tm) && argc < 1) {
      emit_wrong_count(c, id, "1", 1, 0, b);
      return 1;
    }
    /* When the target is unresolved under promote, fall back to the poly ABI
       (sp_RbVal self/args/return) rather than the legacy sp_int ABI: every
       method is poly-signatured in promote, so a `(void*, sp_int)->sp_int`
       cast would truncate the boxed args and return to garbage. */
    int poly_abi = !tm && g_promote_mode;
    TyKind tret = tm ? (TyKind)tm->ret : (poly_abi ? TY_POLY : TY_INT);
    /* A typed-array adapter (`<array>.method(:op)`) has no target scope; its
       Ruby return is op-dependent. Read it from the same shared helper the
       bind site stamps SP_BM_RET_* from, so the call site casts the raw
       register to the array/String it really is instead of an Integer. The
       adapter's fixed arg count is needed too: a trailing splat expands
       against it below, instead of being passed as one sp_int argument (which
       emitted a `sp_int = sp_PolyArray *` initializer and did not compile). */
    int adapter_argc = -1;
    TyKind adapter_ty = TY_UNKNOWN;   /* the adapter's receiver array kind */
    int adapter_push = 0;             /* `<array>.method(:push)`, variadic */
    int adapter_set = 0;              /* `<array>.method(:[]=)`, index+value slots */
    if (!tm) {
      int arecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
      const char *asym = mn >= 0 ? method_sym_arg(c, mn) : NULL;
      TyKind at = (arecv >= 0 && asym)
                    ? method_obj_adapter_ret(comp_ntype(c, arecv), asym) : TY_UNKNOWN;
      if (at != TY_UNKNOWN) {
        adapter_ty = comp_ntype(c, arecv);
        if (!g_promote_mode) tret = at;
        adapter_argc = sp_streq(asym, "[]=") ? 2 : 1;
        adapter_push = sp_streq(asym, "push");
        adapter_set = sp_streq(asym, "[]=");
      }
    }
    if (!is_scalar_ret(tret)) tret = TY_INT;  /* aggregate ret: raw carrier */
    /* A Method whose target the site cannot name -- a local written to more
       than one method (method_recv_node declines it; inference bound the
       arguments to each target through method_recv_nodes), one read out of
       a slot -- is called through the stamped fixed casts below, one C
       argument per site argument. That is the whole call only when it passes
       plain positionals: a splat went over as one argument (`q.call(*s, 2)`
       into `def m(*r, p1)` did not compile, and bound `[[1]]` under
       promote), keywords rode as a trailing positional the thunk had to
       guess at (`**nil` never refused them), and the block was dropped. Such
       a call lays its arguments out as the site wrote them and hands them,
       with the block, to the Method's trampoline, whose thunk binds them by
       the target's parameters (sp_bm_call_spread). */
    if (!tm && adapter_argc < 0 && bm_call_needs_layout(nt, id, argv, argc)) {
      emit_bm_spread_call(c, id, recv, argv, argc, b);
      return 1;
    }
    /* A trailing splat with a statically-known target expands into the
       remaining declared params from the splatted array (#3248); the
       effective arg count becomes the target's residual arity. A wrapper
       whose call sites disagree takes one rest instead (bam_rest), which the
       arguments fill whatever their shape. */
    int splat_at2 = -1;
    int any_splat_arg = 0;
    for (int k = 0; k < argc; k++) {
      const char *aty3 = nt_type(nt, argv[k]);
      if (aty3 && sp_streq(aty3, "SplatNode")) { splat_at2 = k; any_splat_arg = 1; break; }
    }
    int bam_rest = tm && tm->rest_idx >= 0;
    int eargc = argc, tsplat = 0;
    if (!bam_rest && splat_at2 >= 0 && splat_at2 == argc - 1 && (tm || adapter_argc >= 0)) {
      eargc = tm ? (tm->nparams - shift) : adapter_argc;
      if (eargc < splat_at2) eargc = splat_at2;
    }
    else splat_at2 = -1;   /* mid-list splat / unknown target: old path */
    buf_printf(b, "({ sp_BoundMethod *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    /* The Method is a fresh allocation and the defaults evaluated below in
       this frame allocate too (`b = "x" * 64, c = Array.new(400) { }`): a
       collection between the two freed the Method and the call jumped
       through a garbage fn. Root it for the length of the expression. */
    buf_printf(b, "SP_GC_ROOT(_t%d); ", tr);
    /* An unresolved target (`self.class.method(:m)`, any class value that is
       not a statically-known constant) binds with a NULL fn -- there is no
       callable address. Invoking it jumped through NULL; the poly-slot and
       first-class routes decline with NoMethodError, so decline the same way
       here instead of dereferencing it. */
    buf_printf(b, "if (!_t%d->fn) sp_raise_cls(\"NoMethodError\","
                  " sp_sprintf(\"undefined method '%%s' for an instance of Object\","
                  " _t%d->name ? _t%d->name : \"?\")); ", tr, tr, tr);
    /* A typed-array `push` adapter is variadic in CRuby, but the synthesized
       adapter pushes exactly ONE value. The generic single-cast call below
       would silently drop every value after the first (`m.call(*[3, 4])`
       answered `m.call(3)`), so invoke it once per value, materializing the
       full (fixed + splatted) argument list for a runtime count. */
    if (adapter_push && (any_splat_arg || argc != 1)) {
      if (any_splat_arg) {
        int tflat = emit_bm_flat_args(c, argv, argc, b);
        int ti = ++g_tmp;
        buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) { sp_RbVal _e = _t%d->data[_t%d]; ",
                   ti, ti, tflat, ti, tflat, ti);
        if (poly_abi)
          buf_printf(b, "((sp_RbVal (*)(void *, sp_RbVal))(uintptr_t)_t%d->fn)((void *)_t%d->self, _e); ", tr, tr);
        else {
          char _ak = adapter_arg_kind(adapter_ty, 1, 0, 0);
          buf_printf(b, "((sp_int (*)(void *, sp_int))(uintptr_t)_t%d->fn)((void *)_t%d->self, ", tr, tr);
          emit_adapter_arg_boxed("_e", _ak, b);
          buf_puts(b, "); ");
        }
        buf_puts(b, "} ");
      }
      else {
        for (int k = 0; k < argc; k++) {
          int tv = ++g_tmp;
          if (poly_abi) { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[k], b); }
          else {
            char _ak = adapter_arg_kind(adapter_ty, 1, 0, k);
            buf_printf(b, "sp_int _t%d = ", tv);
            emit_adapter_arg_static(c, argv[k], _ak, b);
          }
          buf_puts(b, "; ");
          if (poly_abi)
            buf_printf(b, "((sp_RbVal (*)(void *, sp_RbVal))(uintptr_t)_t%d->fn)((void *)_t%d->self, _t%d); ", tr, tr, tv);
          else
            buf_printf(b, "((sp_int (*)(void *, sp_int))(uintptr_t)_t%d->fn)((void *)_t%d->self, _t%d); ", tr, tr, tv);
        }
      }
      /* push answers the receiver array itself. An adapter call has no
         target scope, so the analyzer types `.call` poly and the caller may
         dispatch on the result; box it here like the generic Method-call
         ABI's sp_bm_box_ret arm. */
      {
        char expr[32]; snprintf(expr, sizeof expr, "_t%d->self", tr);
        emit_boxed_text(c, adapter_ty, expr, b);
      }
      buf_puts(b, "; })");
      return 1;
    }
    /* the wrapper's rest takes every argument, each splat spread into it */
    int *atmp = NULL, *bxtmp = NULL;
    char (*bxref)[24] = NULL;
    if (bam_rest) {
      atmp = (int *)calloc(1, sizeof(int));
      atmp[0] = emit_bm_flat_args(c, argv, argc, b);
      eargc = 1;
      goto bm_emit_call;
    }
    if (splat_at2 >= 0) {
      tsplat = ++g_tmp;
      buf_printf(b, "sp_PolyArray *_t%d = ", tsplat);
      emit_expr(c, argv[splat_at2], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", tsplat);
    }
    /* A typed-array adapter's synthesized C function has a fixed parameter
       count (one for `[]`, two for `[]=`), but the call's C cast is built
       from the supplied count: too few arguments leave its later parameters
       reading an undefined register, so `ia.method(:[]=).call(0)` and
       `.call(*[0])` mutated the array with a garbage element instead of
       raising. CRuby raises ArgumentError; check the count before the cast.
       `push` is variadic and handled above. */
    if (!tm && adapter_argc >= 0 && !adapter_push) {
      if (splat_at2 < 0) {
        if (argc < adapter_argc) {
          char am[128];
          arity_message(am, sizeof am, argc, adapter_argc, adapter_argc + 1, NULL);
          buf_printf(b, "sp_raise_cls(\"ArgumentError\", \"%s\"); %s; })", am, default_value_from_compiler(c, tret));
          return 1;
        }
      }
      else {
        char given[48]; snprintf(given, sizeof given, "_t%d->len + %d", tsplat, splat_at2);
        buf_printf(b, "if (%s < %d) ", given, adapter_argc);
        emit_arity_raise(b, given, adapter_argc, adapter_argc + 1, NULL);
        buf_puts(b, "; ");
      }
    }
    atmp = eargc ? (int *)calloc((size_t)eargc, sizeof(int)) : NULL;
    /* the same argument, kept in its own C type, so the decline path can box
       it without evaluating the expression twice; -1 where there is none */
    bxtmp = eargc ? (int *)calloc((size_t)eargc, sizeof(int)) : NULL;
    for (int k = 0; k < eargc; k++) bxtmp[k] = -1;
    /* only the unresolved-target lane can fall back to the trampoline */
    int bm_want_boxed = !tm && adapter_argc < 0 && !poly_abi && splat_at2 < 0;
    bxref = eargc ? (char (*)[24])calloc((size_t)eargc, 24) : NULL;
    /* Hoist each argument into a temp so both call arms (self-ful / self-less)
       reference it without re-evaluating (#3252). */
    for (int k = 0; k < eargc; k++) {
      atmp[k] = ++g_tmp;
      if (splat_at2 >= 0 && k >= splat_at2 && !tm) {
        /* A typed-array adapter's every fixed slot rides the sp_int register
           (an StrArray element is laundered as its pointer), and a runtime
           splat has no per-position type here, so launder each element the
           same way sp_poly_callable_spread/sp_proc_call_spread do. The KIND
           is its ABSOLUTE adapter position (`k`), not its offset inside the
           splat: a fixed argument before the splat still occupies slot 0, so
           `sa.method(:[]=).call(0, *vals)` must tag the first splat element
           as the VALUE, not the index. Under promote the adapter is
           poly-signatured and takes the boxed element. */
        char el[64];
        snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, %d)", tsplat, k - splat_at2);
        if (poly_abi) buf_printf(b, "sp_RbVal _t%d = %s; ", atmp[k], el);
        else {
          char _ak = adapter_arg_kind(adapter_ty, adapter_push, adapter_set, k);
          buf_printf(b, "sp_int _t%d = ", atmp[k]);
          emit_adapter_arg_boxed(el, _ak, b);
          buf_puts(b, "; ");
        }
      }
      else if (splat_at2 >= 0 && k >= splat_at2) {
        /* an expanded splat element, coerced to the wrapper's param type */
        LocalVar *pp = (k + shift < tm->nparams && tm->pnames)
                         ? scope_local(tm, tm->pnames[k + shift]) : NULL;
        TyKind pt3 = pp ? pp->type : TY_INT;
        char el[64];
        snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, %d)", tsplat, k - splat_at2);
        emit_ctype(c, pt3, b);
        buf_printf(b, " _t%d = ", atmp[k]);
        if (pt3 == TY_POLY) buf_puts(b, el);
        else emit_unbox_text(c, pt3, el, b);
        buf_puts(b, "; ");
        emit_named_root(c, pt3, "_t", atmp[k], b);
        buf_puts(b, " ");
      }
      else if (tm && k + shift < tm->nparams) {
        LocalVar *pp = scope_local(tm, tm->pnames[k + shift]);
        emit_ctype(c, pp ? pp->type : TY_INT, b);
        buf_printf(b, " _t%d = ", atmp[k]);
        emit_arg_or_default(c, tm, k + shift, argv[k], b);
        buf_puts(b, "; ");
        emit_named_root(c, pp ? pp->type : TY_INT, "_t", atmp[k], b);
        buf_puts(b, " ");
      }
      else if (poly_abi) { buf_printf(b, "sp_RbVal _t%d = ", atmp[k]); emit_boxed(c, argv[k], b); buf_puts(b, "; "); emit_named_root(c, TY_POLY, "_t", atmp[k], b); }
      else if (!tm && adapter_argc >= 0) {
        char _ak = adapter_arg_kind(adapter_ty, adapter_push, adapter_set, k);
        buf_printf(b, "sp_int _t%d = ", atmp[k]);
        emit_adapter_arg_static(c, argv[k], _ak, b);
      }
      else if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) {
        /* Hold the value in its own C type first, then launder. The raw
           sp_int carries no class, and the generic trampoline this site
           falls back to reads the BOXED argument channel -- so the boxed
           form has to come from the same single evaluation, not from a
           second one (an argument with a side effect must run once). */
        TyKind pk = comp_ntype(c, argv[k]);
        bxtmp[k] = ++g_tmp;
        emit_ctype(c, pk, b); buf_printf(b, " _t%d = ", bxtmp[k]); emit_expr(c, argv[k], b); buf_puts(b, "; ");
        emit_named_root(c, pk, "_t", bxtmp[k], b); buf_puts(b, " ");
        buf_printf(b, "sp_int _t%d = (sp_int)(uintptr_t)(_t%d)", atmp[k], bxtmp[k]);
      }
      else {
        TyKind pk = comp_ntype(c, argv[k]);
        if (bm_want_boxed && pk != TY_UNKNOWN && pk != TY_VOID && pk != TY_NIL) {
          bxtmp[k] = ++g_tmp;
          emit_ctype(c, pk, b); buf_printf(b, " _t%d = ", bxtmp[k]); emit_expr(c, argv[k], b); buf_puts(b, "; ");
          emit_named_root(c, pk, "_t", bxtmp[k], b); buf_puts(b, " ");
          buf_printf(b, "sp_int _t%d = ", atmp[k]);
          if (pk == TY_POLY) buf_printf(b, "sp_poly_to_i(_t%d)", bxtmp[k]);
          else buf_printf(b, "(sp_int)(_t%d)", bxtmp[k]);
        }
        else {
          /* `nil` has no C value to hold in a typed temp -- its C type is
             void -- so it keeps the plain sp_int slot. The boxed channel
             still has everything it needs: the slot is the single
             evaluation, and the boxed form of nil is a constant. */
          buf_printf(b, "sp_int _t%d = ", atmp[k]); emit_expr(c, argv[k], b);
          if (bm_want_boxed && pk == TY_NIL) bxtmp[k] = atmp[k];
        }
      }
      buf_puts(b, "; ");
    }
    /* A top-level def has a self-less C ABI (fn(args)); an object-bound method
       is fn(self, args). The bound method carries a NULL self for the former. */
  bm_emit_call:;
    /* An unresolved target (a Method that arrived through a parameter or a
       slot) has no static return type: the bind site stamped the kind of its
       C return, and the call reads it the way the poly-slot arms do -- the
       sp_RbVal cast for a poly-returning target, the sp_int cast boxed by
       sp_bm_box_ret for every other -- so the value is a boxed poly rather
       than the raw register read as an Integer (a String answered its
       pointer, a poly two registers of garbage, #4445). The argument classes
       are checked against the stamp too when the call site can spell them;
       a target that cannot ride the cast raises the same NoMethodError the
       poly-slot call does instead of reading garbage. */
    int bm_dyn = !tm && !poly_abi;
    char bm_sig[8 * 64 + 1]; bm_sig[0] = 0;
    /* An over-arity typed-array adapter call (`arr.method(:[]=).call(0, 9,
       8)`) has no over-count to validate: the adapter's C function ignores
       operands past the slots it models, and its own per-slot emission
       already class-checks the modeled ones. Skipping the stamped-ABI gate
       here keeps that shape from raising; the boxing below still applies. */
    int bm_over_arity_adapter = adapter_argc >= 0 && eargc > adapter_argc;
    /* every argument kept a typed temp, so each can be boxed from the value
       already evaluated -- the precondition for handing them to the
       trampoline without evaluating anything a second time */
    /* A call with no arguments has nothing to box and no temps to hold it
       (they are allocated per argument): it can always take the boxed
       lane, which is how a target with an omitted optional is called
       (`q.call` on `def o(p = 51)`, which the zero-width stamp declines). */
    int bm_boxed_ok = bm_want_boxed && (eargc == 0 || (bxtmp && bxref)) && eargc <= 16;
    for (int k = 0; k < eargc && bm_boxed_ok; k++) {
      if (bxtmp[k] < 0) { bm_boxed_ok = 0; break; }
      snprintf(bxref[k], 24, "_t%d", bxtmp[k]);
    }
    int bm_sig_ok = bm_dyn && splat_at2 < 0 && !bm_over_arity_adapter &&
                    call_arg_sig(c, argv, eargc, bm_sig, sizeof bm_sig);
    /* the promote counterpart of the legacy gate below: a dynamic target
       under promote is only callable through the sp_RbVal casts when its
       bind-time poly-ABI stamp says its C signature IS that (and at the
       exact fixed arity); anything else raises the same NoMethodError the
       legacy gate produces instead of reading garbage registers. An
       over-arity adapter call skips the gate for the same reason bm_sig_ok
       does: the extra operands are ignored by the adapter's C function. */
    int bm_poly_gate = poly_abi && splat_at2 < 0 && !bm_over_arity_adapter;
    if (bm_dyn) {
      if (bm_sig_ok) {
        /* The stamped legacy cast does not fit this Method. That is not the
           same as the Method being uncallable: its own thunk, or the poly
           ABI, may take exactly these arguments -- a Float where the site
           could only classify sp_int, a count the stamp does not carry. Hand
           the boxed arguments to the generic trampoline, which picks a lane
           and raises the NoMethodError itself when none fits, rather than
           refusing here on the strength of one lane's answer (#4542). */
        buf_printf(b, "!sp_bm_legacy_abi_ok(_t%d, %d, \"%s\") ? (", tr, eargc, bm_sig);
        if (bm_boxed_ok) {
          for (int k = 0; k < eargc; k++) {
            buf_printf(b, "_sp_proc_poly_args[%d] = ", k);
            emit_boxed_text(c, comp_ntype(c, argv[k]), bxref[k], b);
            buf_puts(b, ", ");
          }
          buf_printf(b, "sp_bm_call_boxed_kw(_t%d, %d, 1)", tr, eargc);
        }
        else buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_obj(_t%d, SP_BUILTIN_METHOD))), sp_box_nil()", name, tr);
        buf_puts(b, ") : ");
      }
      else if (bm_boxed_ok) {
        /* No legacy signature could be built for this site at all: an
           argument the classifier cannot place in an sp_int slot -- a Float,
           a poly value -- means the legacy cast is not merely unlikely to
           fit, it is certainly wrong. Falling through to it read a Float's
           bits as an integer and `m.call(3.5)` answered false. There is
           nothing to test at run time here: go straight to the boxed lane. */
        for (int k = 0; k < eargc; k++) {
          buf_printf(b, "_sp_proc_poly_args[%d] = ", k);
          emit_boxed_text(c, comp_ntype(c, argv[k]), bxref[k], b);
          buf_puts(b, ", ");
        }
        buf_printf(b, "sp_bm_call_boxed_kw(_t%d, %d, 1); })", tr, eargc);
        free(atmp); free(bxtmp); free(bxref);
        return 1;
      }
      buf_printf(b, "_t%d->legacy_ret == SP_BM_RET_POLY ? (", tr);
    }
    if (bm_poly_gate) {
      /* The poly stamp not fitting is one lane's answer, as the legacy gate
         above says: the boxed arguments go to the generic trampoline, the
         Method's own thunk, which raises itself when nothing fits (a local
         re-written to a Method of another signature reaches this) */
      buf_printf(b, "!sp_bm_poly_abi_ok(_t%d, %d) ? (", tr, eargc);
      if (eargc <= 16 && !(tm == NULL && adapter_argc >= 0)) {
        for (int k = 0; k < eargc; k++) buf_printf(b, "_sp_proc_poly_args[%d] = _t%d, ", k, atmp[k]);
        /* an unresolved target's call is plain positionals here (the rest
           took the layout lane above): a trailing Hash is a positional */
        if (!tm) buf_printf(b, "sp_bm_call_boxed_kw(_t%d, %d, 1)", tr, eargc);
        else buf_printf(b, "sp_bm_call_boxed(_t%d, %d)", tr, eargc);
      }
      else buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_obj(_t%d, SP_BUILTIN_METHOD))), sp_box_nil()", name, tr);
      buf_puts(b, ") : (");
      /* the poly ABI shares the legacy stamps' ret kinds: dispatch the same
         three casts (poly / nil / int-boxed) over sp_RbVal argument slots */
      buf_printf(b, "_t%d->legacy_ret == SP_BM_RET_POLY ? (", tr);
    }
    /* A dynamic Method's target is called through the cast its stamped
       return kind names: the 16-byte poly return as sp_RbVal, a nil-returning
       target (C void) as void, everything else as the sp_int the legacy ABI
       carries. The void arm is not a nicety: wasm checks the callee's
       signature at the call and traps on an sp_int cast of a void function
       (a native target read a leftover register and boxed nil regardless). */
    static const int bm_passes[3] = { 0, 2, 1 };   /* poly, nil, int */
    int bm_kinds = bm_dyn || bm_poly_gate;   /* ret-kind-dispatched cast passes */
    for (int pi = 0; pi < (bm_kinds ? 3 : 1); pi++) {
    int pass = bm_passes[pi];
    if (pass == 2) buf_printf(b, ") : _t%d->legacy_ret == SP_BM_RET_NIL ? ((", tr);
    if (pass == 1) buf_printf(b, "), sp_box_nil()) : sp_bm_box_ret(_t%d, ", tr);
    buf_printf(b, "_t%d->recv_bound ? ", tr);
    for (int arm = 0; arm < 2; arm++) {
      if (arm) buf_puts(b, " : ");
      buf_puts(b, "((");
      if (bm_kinds) buf_puts(b, pass == 0 ? "sp_RbVal" : pass == 2 ? "void" : "sp_int");
      else emit_ctype(c, tret, b);
      buf_puts(b, " (*)(");
      const char *sct = bm_self_ctype(tm, shift);
      if (arm == 0) buf_puts(b, sct);
      for (int k = 0; k < eargc; k++) {
        if (arm == 0 || k) buf_puts(b, ", ");
        if (tm && k + shift < tm->nparams) {
          LocalVar *pp = scope_local(tm, tm->pnames[k + shift]);
          emit_ctype(c, pp ? pp->type : TY_INT, b);
        }
        else if (poly_abi) buf_puts(b, "sp_RbVal");
        else buf_puts(b, "sp_int");
      }
      if (arm != 0 && eargc == 0) buf_puts(b, "void");
      buf_printf(b, "))(uintptr_t)_t%d->fn)(", tr);
      if (arm == 0) buf_printf(b, "(%s)(uintptr_t)_t%d->self", sct, tr);
      for (int k = 0; k < eargc; k++) {
        if (arm == 0 || k) buf_puts(b, ", ");
        buf_printf(b, "_t%d", atmp[k]);
      }
      buf_puts(b, ")");
    }
    if (pass == 1) buf_puts(b, ")");
    }
    if (bm_poly_gate) buf_puts(b, ")");
    buf_puts(b, "; })");
    free(atmp); free(bxtmp); free(bxref);
    return 1;
  }

  /* <proc>.call(args) / .() / [] -> sp_proc_call with the sp_int[] ABI.
     (A `&block`-param `.call` is handled earlier by the inline path, whose
     receiver name matches g_block_param_name; this is the escaped-value case.) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC &&
      (is_call_alias(name) ||
       (sp_streq(name, "===") && argc == 1))) {
    TyKind rty = comp_ntype(c, id);          /* the call's result = proc's body return */
    /* `pr&.call(...)`: a nil proc answers nil and the call does not run. The
       receiver goes into a temp the call below reads (through the argument
       override), so it is evaluated once. Without this the nil receiver
       reached sp_proc_recv, which raises -- as `pr.call` should (#4844). */
    { const char *cop = nt_str(nt, id, "call_operator");
      if (cop && sp_streq(cop, "&.") && g_sn_proc_node != id && g_n_argov < MAX_ARG_OVERRIDE) {
        int tq = ++g_tmp;
        Buf rq; memset(&rq, 0, sizeof rq); emit_expr(c, recv, &rq);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d);\n", tq, rq.p ? rq.p : "NULL", tq);
        free(rq.p);
        int slot = view_bind(recv, "_t%d", tq);
        int sv = g_sn_proc_node; g_sn_proc_node = id;
        Buf cb; memset(&cb, 0, sizeof cb);
        emit_expr(c, id, &cb);
        g_sn_proc_node = sv;
        view_unbind(g_n_argov - 1);
        const char *nilv = rty == TY_POLY || rty == TY_UNKNOWN ? "sp_box_nil()"
                         : rty == TY_INT ? "SP_INT_NIL"
                         : rty == TY_FLOAT ? "sp_float_nil()" : default_value_from_compiler(c, rty);
        buf_printf(b, "(_t%d ? %s : %s)", tq, cb.p ? cb.p : nilv, nilv);
        free(cb.p);
        return 1;
      } }
    /* a nil receiver raises NoMethodError, except for `===`, which nil
       answers itself (false) */
    int proc_nil_raises = !sp_streq(name, "===");
    const char *proc_meth = sp_streq(name, "[]") ? "[]"
                          : nt_str(nt, id, "written_name") ? nt_str(nt, id, "written_name")
                          : "call";
    /* `.call { |x| ... }`: the literal block rides the _sp_proc_blk
       side-channel to the callee's &block param (#2648), and so does a Proc
       passed with `&` (`pr.call(1, &b)`). It is handed to the call itself
       (sp_proc_call_blk), which sets the channel as it enters the body:
       published here, ahead of the arguments, it was taken by a `&b` proc
       an argument called, and left behind for the next `&b` body when this
       one took no block. The temp keeps it rooted for the call. */
    char blk_tmp[24] = "";
    {
      /* a forwarded `&blk` or `&` resolves as a Method call's does
         (forwarded_real_proc) */
      int cblk0 = nt_ref(nt, id, "block"), cblk = resolve_forwarded_block(c, cblk0);
      int cbx = cblk >= 0 && nt_kind(nt, cblk) == NK_BlockArgumentNode ? nt_ref(nt, cblk, "expression") : -1;
      Buf bpv; memset(&bpv, 0, sizeof bpv);
      const char *fwd = forwarded_real_proc(cblk0, cblk);
      if (fwd) buf_puts(&bpv, fwd);
      if (fwd || (cbx >= 0 && emit_block_arg_proc(c, cbx, &bpv))) {
        int tb = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d);%c", tb, bpv.p, tb, 10);
        snprintf(blk_tmp, sizeof blk_tmp, "_t%d", tb);
      }
      free(bpv.p);
      if (cblk >= 0 && nt_type(nt, cblk) && sp_streq(nt_type(nt, cblk), "BlockNode")) {
        int tb = ++g_tmp;
        /* render the proc value first: a capturing block's emission writes its
           own prelude (capture struct fill) to g_pre, which must land as whole
           statements before this line */
        Buf pv; memset(&pv, 0, sizeof pv);
        emit_proc_literal(c, cblk, &pv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d);%c", tb, pv.p ? pv.p : "NULL", tb, 10);
        snprintf(blk_tmp, sizeof blk_tmp, "_t%d", tb);
        free(pv.p);
      }
    }
    /* <proc>.call with any splat among the arguments: materialize the full
       argument list (fixed args pushed, each splat expanded) and spread it at
       call time -- the length is dynamic, unlike the fixed sp_int[16] list.
       #2691, #2729 */
    {
      if (call_args_need_spread(nt, argv, argc)) {
        char kwp[24];
        int ta = emit_spread_args_kw(c, argv, argc, kwp, sizeof kwp);
        buf_puts(b, blk_tmp[0] ? "((void)sp_proc_call_spread_blk(" : "((void)sp_proc_call_spread(");
        if (proc_nil_raises) buf_puts(b, "sp_proc_recv(");
        emit_expr(c, recv, b);
        if (proc_nil_raises) buf_printf(b, ", \"%s\")", proc_meth);
        if (blk_tmp[0]) buf_printf(b, ", %s", blk_tmp);
        buf_printf(b, ", sp_box_poly_array(_t%d), %s), ", ta, kwp);
        emit_proc_ret_unbox(c, rty, b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* Universal boxed return: the proc publishes its result in _sp_proc_poly_ret
       (see emit_proc_literal); evaluate the call for effect, then unbox the slot
       to the call's inferred type. */
    buf_puts(b, blk_tmp[0] ? "((void)sp_proc_call_blk(" : "((void)sp_proc_call(");
    if (proc_nil_raises) buf_puts(b, "sp_proc_recv(");
    /* The receiver and the argument list are two operands of ONE C call, and C
       does not order them. A receiver that is itself a call publishes into --
       and its callee's prologue then clears -- the same _sp_proc_poly_args
       slots this argument list writes. `o.call(f).call(x)` lost x that way:
       the argument was published, the receiver's own call ran afterwards and
       wiped the slot, and the inner lambda read nil (#4328). Evaluating a
       non-trivial receiver into a temp first is what orders it before the
       publish -- the same reason the arguments themselves are hoisted in
       emit_proc_call_args. */
    {
      NodeKind rk = nt_kind(nt, recv);
      if (rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode ||
          rk == NK_ConstantReadNode || rk == NK_SelfNode)
        emit_expr(c, recv, b);
      else {
        TyKind rct = comp_ntype(c, recv);
        Buf rb = expr_buf(c, recv);
        int tr = ++g_tmp;
        emit_indent(g_pre, g_indent);
        if (c_type_name(rct) || ty_is_object(rct)) emit_ctype(c, rct, g_pre);
        else buf_puts(g_pre, "sp_RbVal");
        buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : "");
        free(rb.p);
        emit_indent(g_pre, g_indent);
        if (rct == TY_POLY) buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tr);
        else buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
        buf_printf(b, "_t%d", tr);
      }
    }
    if (proc_nil_raises) buf_printf(b, ", \"%s\")", proc_meth);
    if (blk_tmp[0]) buf_printf(b, ", %s", blk_tmp);
    buf_puts(b, ", ");
    emit_proc_call_args(c, id, argc, argv, b, 1);  /* emits args + the closing `)` */
    buf_puts(b, ", ");
    emit_proc_ret_unbox(c, rty, b);
    buf_puts(b, ")");
    return 1;
  }

  /* Proc introspection: arity / lambda? read the sp_Proc metadata directly. */
  /* proc << proc / proc >> proc -> composed Proc. f<<g = f(g(x)) (outer f,
     inner g); f>>g = g(f(x)) (outer g, inner f). */
  if (recv >= 0 &&
      (comp_ntype(c, recv) == TY_PROC || comp_ntype(c, recv) == TY_CURRY ||
       comp_ntype(c, recv) == TY_POLY) &&
      argc == 1 && (sp_streq(name, "<<") || sp_streq(name, ">>")) &&
      (comp_ntype(c, argv[0]) == TY_PROC || comp_ntype(c, argv[0]) == TY_CURRY ||
       comp_ntype(c, argv[0]) == TY_POLY) &&
      /* `<<` on a boxed receiver is far more often Array/String append, so a
         poly receiver composes only through `>>`, which nothing else spells */
      (comp_ntype(c, recv) != TY_POLY || sp_streq(name, ">>")) &&
      (comp_ntype(c, recv) != TY_POLY || comp_ntype(c, argv[0]) != TY_POLY)) {
    int fwd = sp_streq(name, ">>");
    /* Both operands are usually built right here (`f >> g` on two literal
       procs), and C does not order the two argument expressions -- whichever
       runs first is held by nothing while the second allocates, so a
       collection in between frees it. Evaluate them into rooted temps first. */
    int t1 = ++g_tmp, t2 = ++g_tmp;
    /* a Proc read out of a container arrives boxed: unwrap it (#3655) */
    #define SP_EMIT_PROC_OPERAND(nd_) do { \
      if (comp_ntype(c, (nd_)) == TY_POLY) { \
        buf_puts(b, "sp_poly_to_proc("); emit_boxed(c, (nd_), b); buf_puts(b, ")"); \
      } \
      else if (comp_ntype(c, (nd_)) == TY_CURRY) { \
        /* a curry accumulates arguments; composition threads Procs (#3864) */ \
        buf_puts(b, "sp_curry_to_proc("); emit_expr(c, (nd_), b); buf_puts(b, ")"); \
      } \
      else emit_expr(c, (nd_), b); \
    } while (0)
    buf_printf(b, "({ sp_Proc *_t%d = ", t1);
    if (fwd) SP_EMIT_PROC_OPERAND(argv[0]); else SP_EMIT_PROC_OPERAND(recv);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_Proc *_t%d = ", t1, t2);
    if (fwd) SP_EMIT_PROC_OPERAND(recv); else SP_EMIT_PROC_OPERAND(argv[0]);
    #undef SP_EMIT_PROC_OPERAND
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_proc_compose(_t%d, _t%d); })", t2, t1, t2);
    return 1;
  }
  /* Composing something that can never answer #call (`f >> 1`): CRuby's
     Proc#>> refuses the operand up front. Both operands still evaluate,
     receiver first, before the raise. A reopened builtin with its own
     #call disarms the rule, mirroring the inference side. */
  if (recv >= 0 &&
      (comp_ntype(c, recv) == TY_PROC || comp_ntype(c, recv) == TY_CURRY ||
       comp_ntype(c, recv) == TY_METHOD) &&
      argc == 1 && (sp_streq(name, "<<") || sp_streq(name, ">>")) &&
      ty_never_callable(comp_ntype(c, argv[0])) &&
      !user_defines_or_reads(c, "call")) {
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
    buf_puts(b, "); (void)("); emit_boxed(c, argv[0], b);
    buf_puts(b, "); sp_raise_cls(\"TypeError\", \"callable object is expected\");"
                " (sp_Proc *)NULL; })");
    return 1;
  }
  /* Proc's receiver-and-argument arms (arity, lambda?, inspect/to_s, the
     identity and state predicates, freeze/dup/clone/itself): builtin-op
     rows (builtin_ops.c) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC &&
      emit_builtin_op(c, id, recv, TY_PROC, name, b)) return 1;
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && sp_streq(name, "parameters")) {
    /* parameters() follows the receiver's own nature (mode -1); an explicit
       `lambda:` keyword forces the view: true -> lambda (kinds as stored),
       false -> proc (req remapped to opt at print), nil -> the receiver's own.
       Kinds are stored canonically, see the meta emitter. #2693 */
    int pmode = -1, pmode_ok = argc == 0;
    if (argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
      int en = 0; const int *elems = nt_arr(nt, argv[0], "elements", &en);
      if (en == 1) {
        int key = nt_ref(nt, elems[0], "key");
        const char *kn = key >= 0 ? nt_str(nt, key, "unescaped") : NULL;
        if (!kn && key >= 0) kn = nt_str(nt, key, "value");
        int val = nt_ref(nt, elems[0], "value");
        const char *vty = val >= 0 ? nt_type(nt, val) : NULL;
        if (kn && sp_streq(kn, "lambda") && vty) {
          if (sp_streq(vty, "TrueNode"))  { pmode = 1;  pmode_ok = 1; }
          if (sp_streq(vty, "FalseNode")) { pmode = 0;  pmode_ok = 1; }
          if (sp_streq(vty, "NilNode"))   { pmode = -1; pmode_ok = 1; }
        }
      }
    }
    if (pmode_ok) {
      buf_printf(b, "sp_proc_parameters_ids("); emit_expr(c, recv, b);
      buf_printf(b, ", %d, (sp_sym)%d, (sp_sym)%d)",
                 pmode, comp_sym_intern(c, "req"), comp_sym_intern(c, "opt"));
      return 1;
    }
  }
  /* Proc#source_location: [file, line] of the proc's literal. The receiver is
     the literal itself, or a local whose every same-scope write is one proc
     literal -- then that literal's definition site answers (#2649, #2720). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "source_location")) {
    int lit = -1;
    if (nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "LambdaNode") || is_proc_create(c, recv)))
      lit = recv;
    else if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "LocalVariableReadNode")) {
      const char *vn = nt_str(nt, recv, "name");
      Scope *sc = vn ? comp_scope_of(c, recv) : NULL;
      for (int w = 0; vn && w < nt->count; w++) {
        if (nt_kind(nt, w) != NK_LocalVariableWriteNode) continue;
        const char *wn = nt_str(nt, w, "name");
        if (!wn || !sp_streq(wn, vn) || comp_scope_of(c, w) != sc) continue;
        int val = nt_ref(nt, w, "value");
        int is_lit = val >= 0 && nt_type(nt, val) &&
                     (sp_streq(nt_type(nt, val), "LambdaNode") || is_proc_create(c, val));
        if (!is_lit || lit >= 0) { lit = -1; break; }   /* non-literal or second write */
        lit = val;
      }
    }
    if (lit >= 0) {
    int nl = (int)nt_int(nt, lit, "node_line", 0);
    int nf = (int)nt_int(nt, lit, "node_file", 0);
    const char *fn = nt_file_path(nt, nf);
    if (!fn) fn = nt->source_file;
    if (!fn) fn = "?";
    int ta = ++g_tmp;
    buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
    buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(", ta); emit_str_literal(b, fn); buf_puts(b, "));");
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(%d));", ta, nl);
    buf_printf(b, " _t%d; })", ta);
    return 1;
    }
  }
  return 0;
}

/* Method and UnboundMethod objects: instance_method, method(:sym), bind / bind_call, owner,
   original_name, arity, parameters, receiver, unbind, super_method, source_location, to_proc */
int emit_call_method_obj_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* "#<Method: Owner#name(a, b=..., *r)>" -- CRuby's Method#inspect
     rendering, built at compile time from the target scope; " file:line" is
     appended when positions were stamped (--line-map). Keyword parameters
     are not rendered (they live outside pnames). */
  #define BUILD_METHOD_DESC(mi_, sym_, unbound_, out_) do { \
    Scope *_m = &c->scopes[(mi_)]; \
    const char *_ow = "Object"; char _sep = '#'; \
    int _owci = _m->origin_module_ci ? _m->origin_module_ci - 1 : _m->class_id; \
    if (_owci >= 0) { \
      const char *_rn = class_ruby_name(c, _owci); \
      _ow = _rn ? _rn : c->classes[_owci].name; \
      if (_m->is_cmethod) _sep = '.'; \
    } \
    size_t _off = 0, _cap = sizeof (out_); \
    _off += (size_t)snprintf((out_), _cap, "#<%s: %s%c%s(", \
                             (unbound_) ? "UnboundMethod" : "Method", _ow, _sep, \
                             (sym_) ? (sym_) : (_m->name ? _m->name : "?")); \
    for (int _i = 0; _i < _m->nparams && _off < _cap; _i++) { \
      if (!_m->pnames[_i]) continue; \
      if (_i) _off += (size_t)snprintf((out_) + _off, _cap - _off, ", "); \
      if (_i == _m->rest_idx) \
        _off += (size_t)snprintf((out_) + _off, _cap - _off, "*%s", _m->pnames[_i]); \
      else if (_i == _m->kwrest_idx) \
        _off += (size_t)snprintf((out_) + _off, _cap - _off, "**%s", _m->pnames[_i]); \
      else if (_m->pdefault[_i] >= 0) \
        _off += (size_t)snprintf((out_) + _off, _cap - _off, "%s=...", _m->pnames[_i]); \
      else \
        _off += (size_t)snprintf((out_) + _off, _cap - _off, "%s", _m->pnames[_i]); \
    } \
    if (_off < _cap) _off += (size_t)snprintf((out_) + _off, _cap - _off, ")"); \
    { int _ln = scope_def_line(c, _m); \
      if (_ln > 0 && _off < _cap) \
        _off += (size_t)snprintf((out_) + _off, _cap - _off, " %s:%d", \
                                 scope_def_file(c, _m), _ln); } \
    if (_off < _cap) snprintf((out_) + _off, _cap - _off, ">"); \
  } while (0)
  /* Klass.instance_method(:sym) -> the unbound object: the same
     sp_BoundMethod with a NULL self, so name/arity/owner ride the Method
     arms; #bind supplies the self (#2676). */
  if (sp_streq(name, "instance_method") && method_sym_arg(c, id) != NULL) {
    int umi = method_obj_target_mi(c, id);
    if (umi >= 0) {
      buf_puts(b, "sp_bm_set_unbound(sp_bm_set_thunk(sp_bm_set_abi(sp_bound_method_new_d(NULL, SP_BM_SELF_NONE, (sp_int)(uintptr_t)&");
      emit_method_cname(c, &c->scopes[umi], b);
      buf_puts(b, ", ");
      emit_str_literal(b, method_sym_arg(c, id));
      { int ar; if (method_scope_arity(c, umi, &ar)) buf_printf(b, ", (sp_int)%d", ar); else buf_puts(b, ", SP_INT_NIL"); }
      { char _db[512]; BUILD_METHOD_DESC(umi, method_sym_arg(c, id), 1, _db);
        buf_puts(b, ", "); emit_str_literal(b, _db); }
      buf_puts(b, ")");
      { char _s[8 * 64 + 1]; int _fx = 0, _rs = 0, _rt = 0, _pf = 0, _prt = 0;
        int _ab = method_legacy_int_abi(c, umi, 0, _s, sizeof _s, &_fx, &_rs, &_rt);
        int _pa = method_poly_abi(c, umi, 0, &_pf, &_prt);
        /* an instance method binds at #bind, which stamps the bound thunk */
        int _tmin = 0, _tmax = 0;
        const char *_th = c->scopes[umi].is_cmethod ? emit_method_thunk(c, umi, 0, &_tmin, &_tmax) : NULL;
        emit_bm_abi_args(b, "0", _ab, _s, _fx, _rs, _ab ? _rt : _prt, _pa, _pf, _th, _tmin, _tmax); }
      buf_puts(b, ")");
      return 1;
    }
  }
  /* Method#box: Spinel has no namespaces, so no method is ever boxed --
     nil, as CRuby answers for an unboxed method. Evaluate the receiver
     for effect (it may construct the Method). */
  /* Method's receiver-only arms (box, inspect/to_s, name, eql?/equal?,
     dup/clone): builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      emit_builtin_op(c, id, recv, TY_METHOD, name, b)) return 1;
  /* Method#unbind: the same target with no self, re-rendered as an
     UnboundMethod (#3249). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "unbind")) {
    int mnu = method_recv_node(c, recv);
    int tmu = mnu >= 0 ? method_obj_target_mi(c, mnu) : -1;
    int tvu = ++g_tmp;
    buf_printf(b, "({ sp_BoundMethod *_t%d = ", tvu);
    emit_expr(c, recv, b);
    buf_printf(b, "; sp_bm_set_unbound(sp_bm_set_abi(sp_bound_method_new_d(NULL, SP_BM_SELF_NONE, _t%d->fn, _t%d->name, _t%d->arity, ", tvu, tvu, tvu);
    if (tmu >= 0) {
      char _db[512]; BUILD_METHOD_DESC(tmu, method_sym_arg(c, mnu), 1, _db);
      emit_str_literal(b, _db);
    }
    else buf_puts(b, "NULL");
    buf_printf(b, "), 0, _t%d->legacy_int_abi, _t%d->legacy_sig, _t%d->legacy_fixed, _t%d->legacy_rest, _t%d->legacy_ret, _t%d->poly_abi, _t%d->poly_fixed)); })", tvu, tvu, tvu, tvu, tvu, tvu, tvu);
    return 1;
  }
  /* Method#super_method: the same-named method one step up the ancestor
     chain, bound to the same receiver; nil when there is none (#3247). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "super_method")) {
    int mnp = method_recv_node(c, recv);
    int imip = mnp >= 0 ? method_obj_target_mi(c, mnp) : -1;
    int pmip = -1;
    if (imip >= 0 && c->scopes[imip].class_id >= 0 &&
        c->classes[c->scopes[imip].class_id].parent >= 0) {
      int parp = c->classes[c->scopes[imip].class_id].parent;
      pmip = c->scopes[imip].is_cmethod
               ? comp_cmethod_in_chain(c, parp, c->scopes[imip].name, NULL)
               : comp_method_in_chain(c, parp, c->scopes[imip].name, NULL);
    }
    if (pmip >= 0) {
      int tvp = ++g_tmp;
      buf_printf(b, "({ sp_BoundMethod *_t%d = ", tvp);
      emit_expr(c, recv, b);
      /* The new constructor allocates: root the source Method so its self
         (loaded as the new self below) cannot be swept mid-call. */
      buf_printf(b, "; SP_GC_ROOT(_t%d); ", tvp);
      int sup_unb = method_expr_is_unbound(c, recv);
      buf_printf(b, "%ssp_bm_set_thunk(sp_bm_set_abi(sp_bound_method_new_d(_t%d->self, _t%d->self_kind, (sp_int)(uintptr_t)&", sup_unb ? "sp_bm_set_unbound(" : "", tvp, tvp);
      emit_method_cname(c, &c->scopes[pmip], b);
      buf_puts(b, ", ");
      emit_str_literal(b, c->scopes[pmip].name ? c->scopes[pmip].name : "?");
      { int ar; if (method_scope_arity(c, pmip, &ar)) buf_printf(b, ", (sp_int)%d", ar); else buf_puts(b, ", SP_INT_NIL"); }
      { char _db[512];
        BUILD_METHOD_DESC(pmip, c->scopes[pmip].name, sup_unb, _db);
        buf_puts(b, ", "); emit_str_literal(b, _db); }
      buf_puts(b, ")");
      { char _s[8 * 64 + 1]; int _fx = 0, _rs = 0, _rt = 0, _pf = 0, _prt = 0;
        int _ab = method_legacy_int_abi(c, pmip, !sup_unb, _s, sizeof _s, &_fx, &_rs, &_rt);
        int _pa = method_poly_abi(c, pmip, !sup_unb, &_pf, &_prt);
        char _rbb[32]; snprintf(_rbb, sizeof _rbb, "_t%d->recv_bound", tvp);
        /* the parent's binding is the source Method's: an instance method
           is receiver-bound, a class method is not */
        int _prb = c->scopes[pmip].class_id >= 0 && !c->scopes[pmip].is_cmethod;
        int _tmin = 0, _tmax = 0;
        const char *_th = sup_unb ? NULL : emit_method_thunk(c, pmip, _prb, &_tmin, &_tmax);
        emit_bm_abi_args(b, _rbb, _ab, _s, _fx, _rs, _ab ? _rt : _prt, _pa, _pf, _th, _tmin, _tmax); }
      if (sup_unb) buf_puts(b, ")");
      buf_puts(b, "; })");
    }
    else {
      buf_puts(b, "((void)(");
      emit_expr(c, recv, b);
      buf_puts(b, "), (sp_BoundMethod *)NULL)");
    }
    return 1;
  }
  /* An UNBOUND method is not callable: CRuby's NoMethodError (#2724). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      is_call_alias(name) &&
      method_expr_is_unbound(c, recv)) {
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
    buf_printf(b, "); sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"undefined method '%s' for an instance of UnboundMethod\")[1])); %s; })",
               name, default_value_from_compiler(c, comp_ntype(c, id)));
    return 1;
  }
  /* UnboundMethod#bind_call(obj, args...) = bind(obj).call(args...): with a
     statically-known instance-method target, call it directly with obj as
     self (#3246), the arguments after obj bound as bind(obj).call binds
     them (emit_bind_call). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc >= 1 &&
      sp_streq(name, "bind_call")) {
    int mn2 = method_recv_node(c, recv);
    int t2 = mn2 >= 0 ? method_obj_target_mi(c, mn2) : -1;
    if (t2 >= 0 && c->scopes[t2].class_id >= 0 && !c->scopes[t2].is_cmethod &&
        ty_is_object(comp_ntype(c, argv[0]))) {
      emit_bind_call(c, id, t2, argv, argc, b);
      return 1;
    }
    /* a boxed obj: its class is checked at run time */
    if (t2 >= 0 && c->scopes[t2].class_id >= 0 && !c->scopes[t2].is_cmethod &&
        comp_ntype(c, argv[0]) == TY_POLY && nt_kind(nt, argv[0]) != NK_SplatNode &&
        class_value_bind_call_target(c, c->scopes[t2].class_id, c->scopes[t2].name, NULL) == t2) {
      emit_bind_call_boxed(c, id, t2, -1, NULL, argv, argc, b);
      return 1;
    }
  }
  /* ...and on a Class value known only at run time, a switch on its class */
  if (recv >= 0 && argc >= 1 && sp_streq(name, "bind_call") && nt_kind(nt, argv[0]) != NK_SplatNode) {
    const char *cvsym = class_value_instance_method_sym(c, recv);
    /* a class with no arm to build is a gap, not the NameError */
    if (cvsym) refuse_from_plan(c, id, CRF_BIND_CALL, "refuse-bind-call");
    if (cvsym) {
      emit_bind_call_boxed(c, id, -1, nt_ref(nt, recv, "receiver"), cvsym, argv, argc, b);
      return 1;
    }
  }
  /* UnboundMethod#bind(obj): the same target with obj as self. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 1 && sp_streq(name, "bind")) {
    int mn2 = method_recv_node(c, recv);
    int t2 = mn2 >= 0 ? method_obj_target_mi(c, mn2) : -1;
    if (t2 >= 0 && ty_is_object(comp_ntype(c, argv[0])) &&
        bind_owner_mismatch(c, t2, comp_ntype(c, argv[0]))) {
      /* obj of another class: CRuby's TypeError, at bind time, whether or
         not the Method is ever called. It was bound and called as one. */
      buf_puts(b, "({ (void)(");
      emit_expr(c, argv[0], b);
      buf_printf(b, "); sp_raise_cls(\"TypeError\", \"bind argument must be an instance of %s\"); (sp_BoundMethod *)NULL; })",
                 c->classes[c->scopes[t2].class_id].name);
      return 1;
    }
    if (t2 >= 0 && ty_is_object(comp_ntype(c, argv[0]))) {
      /* The constructor allocates: hold the fresh `obj` (`...bind(C.new)`)
         in a rooted C temporary across it, or the only reference can be
         swept. Same shape as the `.method` and `.to_proc` captures. */
      int bt = ++g_tmp;
      buf_printf(b, "({ void *_t%d = (void *)(", bt);
      emit_expr(c, argv[0], b);
      buf_printf(b, "); SP_GC_ROOT(_t%d); sp_bm_set_thunk(sp_bm_set_abi(sp_bound_method_new_d(_t%d, SP_BM_SELF_OBJ, (sp_int)(uintptr_t)&", bt, bt);
      emit_method_cname(c, &c->scopes[t2], b);
      buf_puts(b, ", ");
      emit_str_literal(b, method_sym_arg(c, mn2));
      { int ar; if (method_scope_arity(c, t2, &ar)) buf_printf(b, ", (sp_int)%d", ar); else buf_puts(b, ", SP_INT_NIL"); }
      { char _db[512]; BUILD_METHOD_DESC(t2, method_sym_arg(c, mn2), 0, _db);
        buf_puts(b, ", "); emit_str_literal(b, _db); }
      buf_puts(b, ")");
      { char _s[8 * 64 + 1]; int _fx = 0, _rs = 0, _rt = 0, _pf = 0, _prt = 0;
        int _ab = method_legacy_int_abi(c, t2, 1, _s, sizeof _s, &_fx, &_rs, &_rt);
        int _pa = method_poly_abi(c, t2, 1, &_pf, &_prt);
        int _tmin = 0, _tmax = 0;
        const char *_th = emit_method_thunk(c, t2, 1, &_tmin, &_tmax);
        emit_bm_abi_args(b, "1", _ab, _s, _fx, _rs, _ab ? _rt : _prt, _pa, _pf, _th, _tmin, _tmax); }
      buf_puts(b, "; })");
      return 1;
    }
  }
  /* method(:sym) / <recv>.method(:sym) -> a bound Method object. */
  if (sp_streq(name, "method") && method_sym_arg(c, id) != NULL) {
    const char *sym = method_sym_arg(c, id);
    int mi = method_obj_target_mi(c, id);
    if (mi < 0 && emit_method_obj_on_constant(c, id, recv, sym, b)) return 1;
    /* A poly receiver has no statically-known class, so method_obj_target_mi
       resolves nothing (mi < 0), there is no callable address to bind, and
       the boxed sp_RbVal is not a C pointer: the `(void *)(<expr>)` self slot
       below did not even compile (`cannot convert to a pointer type`). This
       became reachable when a poly `.call` result is typed poly (#4395):
       `o = arr[0].call; o.method(:foo)` used to key off the user `call`'s
       return type and stayed concrete. Raise CRuby's NoMethodError instead of
       failing the build. A poly `<recv>.method` never bound a real target
       (the emitted fn was 0), so this only moves the failure from the C
       compiler to a clean runtime error. */
    int bam_poly = mi >= 0 && c->scopes[mi].def_node >= 0 &&
                   nt_int(nt, c->scopes[mi].def_node, "bam_poly", 0);
    if (recv >= 0 && comp_ntype(c, recv) == TY_POLY && !bam_poly) {
      buf_puts(b, "({ sp_RbVal _rpm = ");
      emit_expr(c, recv, b);
      buf_puts(b, "; SP_GC_ROOT_RBVAL(_rpm); sp_raise_cls(\"NoMethodError\", sp_nomethod_msg(");
      emit_str_literal(b, sym);
      buf_puts(b, ", _rpm)); (sp_BoundMethod *)0; })");
      return 1;
    }
    /* bare method(:sym) on an instance method binds the current self */
    int self_bound = (recv < 0 && mi >= 0 && c->scopes[mi].class_id >= 0 &&
                      !c->scopes[mi].is_cmethod);
    /* Whether this Method's target can ride the legacy sp_int poly-call ABI.
       A typed-array adapter (mi < 0, bop below) is legacy in non-promote
       mode; an unresolved builtin (no callable address) must not claim it.
       `recv_bound` tells the classifier whether a __bam_ wrapper's self slot
       carries its first parameter: a receiver-bound builtin/accessor wrapper
       is wrapped with self, a receiverless Kernel wrapper with NULL self. */
    /* The target's C ABI takes the bound receiver in the leading slot: true
       for every non-class receiver (a value receiver of 0 has self == NULL but
       is still bound) and for a bare method(:sym) bound to the enclosing
       instance; false for a top-level method, a class method, or a
       receiverless Kernel wrapper. Persisted on the Method (#4395) because the
       raw `self != NULL` test cannot recover it later. */
    int recv_bound = recv >= 0 ? (comp_ntype(c, recv) != TY_CLASS) : (self_bound ? 1 : 0);
    int mi_legacy = 0;
    char mi_sig[8 * 64 + 1]; mi_sig[0] = 0;
    int mi_fixed = 0, mi_rest = 0, mi_ret = 0, mi_poly = 0, mi_pfixed = 0, mi_pret = 0;
    if (mi >= 0) {
      mi_legacy = method_legacy_int_abi(c, mi, recv_bound,
                                        mi_sig, sizeof mi_sig, &mi_fixed, &mi_rest, &mi_ret);
      mi_poly = method_poly_abi(c, mi, recv_bound, &mi_pfixed, &mi_pret);
    }
    int bop_argc = -1;  /* typed-array adapter fixed-slot count, set below */
    int bop_rest = 0;   /* typed-array adapters have no rest slot */
    /* Keep in step with SP_BM_RET_* in lib/sp_proc.h: how the adapter's sp_int
       C return boxes back into a Ruby value. IntArray get/set return the int
       element; the other adapters launder the array or a String through the
       register and must not be boxed as Integers (#4395). */
    int bop_ret = 0;    /* SP_BM_RET_INT */
    int bop_is_adapter = 0;  /* target is a synthesized typed-array adapter */
    char bop_sig[8 * 64 + 1]; bop_sig[0] = 0;  /* typed-array adapter ABI signature */
    /* A Method bound to a class/module (Klass.method(:cmeth)) has no instance
       self -- the class value is not a heap pointer, so pass NULL. */
    const char *self_kind = "SP_BM_SELF_NONE";
    int self_is_str = 0, self_rooted = 0;
    int self_receiver = (recv >= 0 && comp_ntype(c, recv) != TY_CLASS);
    int self_tmp = 0;
    int tbr = 0;
    if (self_receiver && bam_poly) {
      /* the receiver rides the self slot inside a one-element PolyArray,
         which the synthesized wrapper reads back as __bam_r[0] */
      tbr = ++g_tmp;
      self_tmp = ++g_tmp;
      self_kind = "SP_BM_SELF_OBJ";
      self_rooted = 1;
      buf_printf(b, "({ sp_RbVal _t%d = ", tbr);
      emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); void *_t%d = (void *)sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push((sp_PolyArray *)_t%d, _t%d); ",
                 tbr, self_tmp, self_tmp, self_tmp, tbr);
    }
    else if (self_receiver) {
      TyKind rt2 = comp_ntype(c, recv);
      /* A Method binds to whatever the receiver is, and a number is not a
         reference the collector can follow. */
      if (rt2 == TY_STRING || rt2 == TY_STRBUF) { self_kind = "SP_BM_SELF_STR"; self_is_str = 1; self_rooted = 1; }
      else if (needs_root(rt2) && rt2 != TY_POLY && !comp_ty_value_obj(c, rt2)) { self_kind = "SP_BM_SELF_OBJ"; self_rooted = 1; }
      /* The constructor allocates and may collect: a fresh receiver
         (`M.new.method(:v)`) is otherwise unreachable and would be swept, so
         hold it in a rooted C temporary across the call -- the same shape as
         the `.to_proc` capture below. */
      if (self_rooted) {
        self_tmp = ++g_tmp;
        buf_printf(b, "({ void *_t%d = (void *)(", self_tmp);
        emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT%s(_t%d); ", self_is_str ? "_STR" : "", self_tmp);
      }
    }
    buf_puts(b, mi >= 0 ? "sp_bm_set_thunk(sp_bm_set_abi(sp_bound_method_new_d(" : "sp_bm_set_thunk(sp_bm_set_abi(sp_bound_method_new(");
    if (self_rooted) buf_printf(b, "_t%d", self_tmp);
    else if (recv >= 0 && comp_ntype(c, recv) == TY_CLASS) buf_puts(b, "NULL");
    else if (recv >= 0) { buf_puts(b, "(void *)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (self_bound) { buf_printf(b, "(void *)%s", g_self); self_kind = "SP_BM_SELF_OBJ"; }
    else buf_puts(b, "NULL");
    buf_printf(b, ", %s, ", self_kind);
    if (mi >= 0) { buf_puts(b, "(sp_int)(uintptr_t)&"); emit_method_cname(c, &c->scopes[mi], b); }
    else {
      /* `<typed_array>.method(:op)`: lower through a per-(type, op) adapter
         matching the Method dispatch ABI (optcarrot's
         `add_mappings(.., @ram, @ram.method(:[]=))` shape). */
      TyKind brt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
      const char *bk = ty_is_array(brt) ? array_kind(brt) : NULL;
      const char *bop = NULL;
      if (bk && (brt == TY_INT_ARRAY || brt == TY_STR_ARRAY)) {
        if (sp_streq(sym, "[]")) bop = "get";
        else if (sp_streq(sym, "[]=")) bop = "set";
        else if (sp_streq(sym, "push")) bop = "push";
      }
      if (bop) {
        /* Under promote the adapter is emitted poly-signatured (sp_RbVal
           self/arg/return), so it claims the POLY stamp there -- its C
           signature IS the poly ABI -- and must not claim the legacy sp_int
           one; every poly arm (the call gates, the spread path's generic
           trampoline) then rides or declines it by the same stamp as any
           other target. */
        mi_legacy = g_promote_mode ? 0 : 1;
        bop_is_adapter = 1;
        bop_argc = (bop[0] == 's') ? 2 : 1;  /* set=2, get/push=1 */
        if (g_promote_mode) {
          mi_poly = 1;
          mi_pfixed = bop_argc;
          bop_ret = 5;   /* SP_BM_RET_POLY: the promote adapter returns sp_RbVal */
        }
        /* memoized per (kind, op): emit the adapter once */
        static char bam_done[2][3];
        int ki = (brt == TY_INT_ARRAY) ? 0 : 1;
        int oi = bop[0] == 'g' ? 0 : bop[0] == 's' ? 1 : 2;
        /* The non-promote adapter's Ruby return comes from the shared helper
           (which also drives the analyzer's inferred type): an IntArray
           adapter answers the array for push and the int element otherwise;
           a StrArray adapter answers the array for push and the laundered
           String element for get/set. */
        TyKind art = method_obj_adapter_ret(brt, sym);
        bop_ret = art == TY_INT_ARRAY ? 2 /* SP_BM_RET_INT_ARRAY */
                : art == TY_STR_ARRAY ? 3 /* SP_BM_RET_STR_ARRAY */
                : art == TY_STRING    ? 1 /* SP_BM_RET_STR */
                : 0 /* SP_BM_RET_INT */;
        /* A StrArray adapter launders its String element through the sp_int
           slot: the value is arg 1 of []= and arg 0 of push. The int array
           and the StrArray index are scalars. The stamped signature mirrors
           the call site's argument types, not the adapter's C parameter
           types (the element is laundered through an sp_int). */
        for (int _q = 0; _q < bop_argc; _q++) abi_sig_token(TY_INT, bop_sig + 8 * _q);
        if (ki == 1 && oi == 1) abi_sig_token(TY_STRING, bop_sig + 8);
        else if (ki == 1 && oi == 2) abi_sig_token(TY_STRING, bop_sig);
        bop_sig[8 * bop_argc] = 0;
        if (!bam_done[ki][oi]) {
          bam_done[ki][oi] = 1;
          const char *cast = (ki == 0) ? "" : "(sp_int)(uintptr_t)";
          const char *uncast = (ki == 0) ? "" : "(const char *)(uintptr_t)";
          if (g_promote_mode) {
            /* promote: bound methods are invoked through the poly ABI, so the
               adapter takes/returns sp_RbVal (boxing the int/string element). */
            const char *boxret = (ki == 0) ? "sp_box_int_or_nil" : "sp_box_str";
            /* the element the typed array is asked to hold: its own kind
               or the refusal, never a coercion (#4481) */
            const char *unbox  = (ki == 0) ? "sp_poly_elem_i" : "sp_poly_elem_s";
            const char *boxarr = (ki == 0) ? "sp_box_int_array" : "sp_box_str_array";
            if (oi == 0) {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_get(void *a, sp_RbVal i);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_get(void *a, sp_RbVal i) {\n"
                                   "  return %s(sp_%sArray_get((sp_%sArray *)a, sp_poly_arg_i(i)));\n}\n", bk, boxret, bk, bk);
            }
            else if (oi == 1) {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_set(void *a, sp_RbVal i, sp_RbVal v);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_set(void *a, sp_RbVal i, sp_RbVal v) {\n"
                                   "  sp_%sArray_set((sp_%sArray *)a, sp_poly_arg_i(i), %s(v));\n  return v;\n}\n", bk, bk, bk, unbox);
            }
            else {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_push(void *a, sp_RbVal v);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_push(void *a, sp_RbVal v) {\n"
                                   "  sp_%sArray_push((sp_%sArray *)a, %s(v));\n  return %s(a);\n}\n", bk, bk, bk, unbox, boxarr);
            }
          }
          else if (oi == 0) {
            buf_printf(&g_proc_protos, "static sp_int _bam_%sArray_get(void *a, sp_int i);\n", bk);
            buf_printf(&g_procs, "static sp_int _bam_%sArray_get(void *a, sp_int i) {\n"
                                 "  return %ssp_%sArray_get((sp_%sArray *)a, i);\n}\n", bk, cast, bk, bk);
          }
          else if (oi == 1) {
            buf_printf(&g_proc_protos, "static sp_int _bam_%sArray_set(void *a, sp_int i, sp_int v);\n", bk);
            buf_printf(&g_procs, "static sp_int _bam_%sArray_set(void *a, sp_int i, sp_int v) {\n"
                                 "  sp_%sArray_set((sp_%sArray *)a, i, %sv);\n  return v;\n}\n", bk, bk, bk, uncast);
          }
          else {
            buf_printf(&g_proc_protos, "static sp_int _bam_%sArray_push(void *a, sp_int v);\n", bk);
            buf_printf(&g_procs, "static sp_int _bam_%sArray_push(void *a, sp_int v) {\n"
                                 "  sp_%sArray_push((sp_%sArray *)a, %sv);\n  return (sp_int)(uintptr_t)a;\n}\n", bk, bk, bk, uncast);
          }
        }
        buf_printf(b, "(sp_int)(uintptr_t)&_bam_%sArray_%s", bk, bop);
      }
      else {
        /* An undefined name is CRuby's immediate NameError (#2752): with a
           concretely-typed receiver, accept only names the builtin table (or
           the universal Object surface) knows; everything else raises now,
           not at call time. A poly/unknown receiver keeps the lenient path. */
        TyKind nrt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
        const char *ncls = nrt == TY_STRING ? "String" : nrt == TY_INT ? "Integer"
                         : nrt == TY_FLOAT ? "Float" : nrt == TY_SYMBOL ? "Symbol"
                         : ty_is_array(nrt) ? "Array" : ty_is_hash(nrt) ? "Hash"
                         : nrt == TY_RANGE ? "Range" : nrt == TY_TIME ? "Time"
                         : nrt == TY_BOOL || nrt == TY_NIL ? "Object"
                         : ty_is_object(nrt) ? "Object" : NULL;
        int nknown = 0, nba;
        if (ncls && builtin_method_arity(ncls, sym, &nba)) nknown = 1;
        if (builtin_object_method_known(sym)) nknown = 1;
        if (ncls && !nknown) {
          buf_printf(b, "(sp_int)(sp_raise_cls(\"NameError\","
                        " sp_sprintf(\"undefined method '%s' for %%s\","
                        " \"an instance of %s\")), 0)",
                     sym, ncls);
        }
        else buf_puts(b, "(sp_int)0");  /* builtin/Kernel method: no callable address */
      }
    }
    buf_puts(b, ", ");
    /* a synthesized __bam_N forwarder is an implementation detail: Method#name
       must report the original method, recovered from the wrapper's body call */
    const char *disp = sym;
    if (strncmp(sym, "__bam_", 6) == 0 && mi >= 0) disp = bam_builtin_sym(c, mi, sym);
    emit_str_literal(b, disp);
    { int ar;
      if (tbr > 0) {
        /* the builtin's own arity, by the class the receiver has at run
           time; the wrapper's parameters are the call sites' plumbing */
        static const char *const PCLS[] = { "String", "Array", "Hash", "Integer", "Float", "Symbol", "Range", NULL };
        static const char *const PTST[] = {
          "_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)",
          "_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)",
          "_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id)",
          "_t%d.tag == SP_TAG_INT && _t%d.tag == SP_TAG_INT",
          "_t%d.tag == SP_TAG_FLT && _t%d.tag == SP_TAG_FLT",
          "_t%d.tag == SP_TAG_SYM && _t%d.tag == SP_TAG_SYM",
          "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE" };
        int fallback = method_scope_arity(c, mi, &ar) ? ar : -1;
        buf_puts(b, ", (sp_int)(");
        for (int q = 0; PCLS[q]; q++) {
          int ba;
          if (!builtin_method_arity(PCLS[q], disp, &ba)) continue;
          buf_puts(b, "("); buf_printf(b, PTST[q], tbr, tbr); buf_printf(b, ") ? %d : ", ba);
        }
        for (int k = 0; k < c->nclasses; k++) {
          int ka = method_scope_arity(c, comp_method_in_chain(c, k, disp, NULL), &ar) ? ar
                 : comp_reader_in_chain(c, k, disp, NULL) ? 0 : attr_writer_named(c, k, disp) ? 1 : fallback;
          if (ka != fallback)
            buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d) ? %d : ", tbr, tbr, k, ka);
        }
        buf_printf(b, "%d)", fallback);
      }
      else if (mi >= 0 && method_scope_arity(c, mi, &ar)) buf_printf(b, ", (sp_int)%d", ar);
      else if (bop_is_adapter) {
        /* An adapter has no method scope: stamp CRuby's arity for the Array op
           it stands in for (`push`/`[]`/`[]=` are all -1), not SP_INT_NIL. */
        int ba;
        if (builtin_method_arity("Array", sym, &ba)) buf_printf(b, ", (sp_int)%d", ba);
        else buf_puts(b, ", SP_INT_NIL");
      }
      else buf_puts(b, ", SP_INT_NIL"); }
    if (mi >= 0) {
      char _db[512]; BUILD_METHOD_DESC(mi, disp, 0, _db);
      buf_puts(b, ", "); emit_str_literal(b, _db);
    }
    buf_puts(b, ")");
    { char _rbb[16]; snprintf(_rbb, sizeof _rbb, "%d", recv_bound);
      int _tmin = 0, _tmax = 0;
      const char *_th = mi >= 0 ? emit_method_thunk(c, mi, recv_bound, &_tmin, &_tmax) : NULL;
      emit_bm_abi_args(b, _rbb, mi_legacy, mi >= 0 ? mi_sig : bop_sig,
                       mi >= 0 ? mi_fixed : bop_argc, mi >= 0 ? mi_rest : bop_rest,
                       mi >= 0 ? (mi_legacy ? mi_ret : mi_pret) : bop_ret, mi_poly, mi_pfixed, _th, _tmin, _tmax); }
    if (self_rooted) buf_puts(b, "; })");
    return 1;
  }
  /* <method>.to_proc wraps the bound method in a trampoline Proc. When the
     target method is statically known, emit a per-site trampoline that calls
     it with its real C signature (a top-level method has no self parameter;
     an object-bound one is invoked through a typed fn cast) and publishes the
     result boxed in _sp_proc_poly_ret -- the universal first-class-proc
     return ABI a later `.call` reads back. Falls back to the generic runtime
     trampoline when the target is unresolved. */
  /* Proc#to_proc is self (#3687) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "to_proc")) {
    emit_expr(c, recv, b);
    return 1;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "to_proc")) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    if (target >= 0) {
      Scope *tm = &c->scopes[target];
      int np = tm->nparams;
      int tid = ++g_proc_counter;
      /* a bound __bam wrapper carries param[0] in the Method's self slot:
         proc arg k maps to param[k + shift] */
      int shift = method_call_param_shift(c, mn, target);
      char mtpn[40]; snprintf(mtpn, sizeof mtpn, "_mtp_%d", tid);
      emit_method_tramp_fn(c, tm, shift, mtpn, 0, method_node_recv_class(c, recv, tm->class_id), NULL, NULL);
      /* arity: required count (minus the self-carried wrapper param),
         negative when the signature is variadic (the Scope folds
         optionals/rest into nparams > nrequired). */
      int m_arity = (np != tm->nrequired) ? -(tm->nrequired - shift + 1)
                                          : tm->nrequired - shift;
      /* The capture is built before sp_proc_new_meta allocates the proc it
         goes into, and until the proc holds it the C argument slot is the only
         thing that does, so the proc's own allocation could collect it. */
      { int tcap = ++g_tmp;
        buf_printf(b, "({ void *_t%d = (void *)(", tcap);
        emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", tcap);
        buf_printf(b, " sp_proc_new_meta((void *)_mtp_%d, _t%d, sp_bm_cap_scan, %d, TRUE, 0, NULL, NULL); })",
                   tid, tcap, m_arity); }
      return 1;
    }
    /* sp_method_to_proc allocates the proc and stores the Method as its
       capture; a fresh receiver Method (`self.class.method(:m).to_proc`) is
       otherwise unreachable and would be swept mid-allocation, leaving the
       proc's capture dangling. Root it across the constructor, the same shape
       as the per-site path above. */
    { int tp = ++g_tmp;
      buf_printf(b, "({ sp_BoundMethod *_t%d = ", tp);
      emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_method_to_proc(_t%d); })", tp, tp); }
    return 1;
  }
  /* Method#original_name: the target scope's own name -- an alias-created
     method resolves through comp_method_in_chain, so the scope carries the
     original (#3247). Falls back to #name for an unresolved target. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "original_name")) {
    int mn4 = method_recv_node(c, recv);
    int tg4 = mn4 >= 0 ? method_obj_target_mi(c, mn4) : -1;
    if (tg4 >= 0 && c->scopes[tg4].name) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_sym_intern(");
      emit_str_literal(b, c->scopes[tg4].name);
      buf_puts(b, "))");
    }
    else {
      buf_puts(b, "sp_sym_intern((const char *)("); emit_expr(c, recv, b); buf_puts(b, ")->name)");
    }
    return 1;
  }
  /* Method#source_location: [file, line] of the target's def, from the same
     node position the #line machinery uses (#3247). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "source_location")) {
    int mn4 = method_recv_node(c, recv);
    int tg4 = mn4 >= 0 ? method_obj_target_mi(c, mn4) : -1;
    int dn4 = tg4 >= 0 ? c->scopes[tg4].def_node : -1;
    int ln4 = dn4 >= 0 ? (int)nt_int(nt, dn4, "node_line", 0) : 0;
    /* under --no-line-map node_line is unstamped (0): still emit the pair so
       the program compiles; the line is 0 there */
    if (dn4 >= 0) {
      const char *file4 = nt_file_path(nt, (int)nt_int(nt, dn4, "node_file", 0));
      if (!file4 || !*file4) file4 = nt->source_file;
      int tr4 = ++g_tmp;
      buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_str(", tr4, tr4, tr4);
      emit_str_literal(b, file4 ? file4 : "");
      buf_printf(b, ")); sp_PolyArray_push(_t%d, sp_box_int(%dLL)); _t%d; })", tr4, ln4, tr4);
      return 1;
    }
  }
  /* Method/UnboundMethod#parameters: [[:req, :a], [:opt, :b], ...] built
     statically from the target's parameter list (#3247). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "parameters")) {
    int mn4 = method_recv_node(c, recv);
    int tg4 = mn4 >= 0 ? method_obj_target_mi(c, mn4) : -1;
    int dn4 = tg4 >= 0 ? c->scopes[tg4].def_node : -1;
    int pn4 = dn4 >= 0 ? nt_ref(nt, dn4, "parameters") : -1;
    /* A Method no one node names: the list its rendering carries, as a
       poly one reads it (#3692) */
    if (mn4 < 0) {
      buf_puts(b, "sp_bm_parameters(");
      emit_expr(c, recv, b);
      buf_puts(b, ")");
      return 1;
    }
    if (tg4 >= 0) {
      int tr4 = ++g_tmp;
      buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr4, tr4);
      if (pn4 >= 0) {
        /* CRuby order: req, opt, rest, post-req, key(req), keyrest, block */
        static const struct { const char *arr; const char *kind; const char *optkind; } PKINDS[] = {
          { "requireds", "req", NULL }, { "optionals", "opt", NULL },
          { "rest", NULL, NULL },   /* placeholder: rest emitted in this slot */
          { "posts", "req", NULL }, { "keywords", "keyreq", "key" },
        };
        for (size_t pk = 0; pk < sizeof PKINDS / sizeof PKINDS[0]; pk++) {
          if (!PKINDS[pk].kind) {   /* the rest param, in CRuby's slot */
            int rest4 = nt_ref(nt, pn4, "rest");
            if (rest4 >= 0 && nt_str(nt, rest4, "name")) {
              int tp4 = ++g_tmp;
              buf_printf(b, " { sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                            " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4, tp4, tp4);
              emit_str_literal(b, "rest");
              buf_printf(b, ")));"
                            " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4);
              emit_str_literal(b, sp_streq(nt_str(nt, rest4, "name"), "__anon_rest") ? "*" : nt_str(nt, rest4, "name"));
              buf_printf(b, ")));"
                            " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr4, tp4);
            }
            continue;
          }
          int an4 = 0; const int *av4 = nt_arr(nt, pn4, PKINDS[pk].arr, &an4);
          for (int e4 = 0; e4 < an4; e4++) {
            const char *pnm4 = nt_str(nt, av4[e4], "name");
            if (!pnm4) continue;
            const char *kind4 = PKINDS[pk].kind;
            if (PKINDS[pk].optkind) {
              const char *ety4 = nt_type(nt, av4[e4]);
              if (ety4 && sp_streq(ety4, "OptionalKeywordParameterNode")) kind4 = PKINDS[pk].optkind;
            }
            int tp4 = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                          " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4, tp4, tp4);
            emit_str_literal(b, kind4);
            buf_printf(b, ")));"
                          " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4);
            emit_str_literal(b, pnm4);
            buf_printf(b, ")));"
                          " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr4, tp4);
          }
        }
        int kwr4 = nt_ref(nt, pn4, "keyword_rest");
        if (kwr4 >= 0 && nt_str(nt, kwr4, "name")) {
          int tp4 = ++g_tmp;
          buf_printf(b, " { sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                        " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4, tp4, tp4);
          emit_str_literal(b, "keyrest");
          buf_printf(b, ")));"
                        " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4);
          emit_str_literal(b, sp_streq(nt_str(nt, kwr4, "name"), "__anon_kwrest") ? "**" : nt_str(nt, kwr4, "name"));
          buf_printf(b, ")));"
                        " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr4, tp4);
        }
        int blk4 = nt_ref(nt, pn4, "block");
        if (blk4 >= 0 && nt_str(nt, blk4, "name")) {
          int tp4 = ++g_tmp;
          buf_printf(b, " { sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                        " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4, tp4, tp4);
          emit_str_literal(b, "block");
          buf_printf(b, ")));"
                        " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", tp4);
          /* an anonymous `&` carries a synthetic name (desugar_anon_block_param);
             CRuby reports it as :& */
          const char *bnm4 = nt_str(nt, blk4, "name");
          emit_str_literal(b, sp_streq(bnm4, "__anon_block") ? "&" : bnm4);
          buf_printf(b, ")));"
                        " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr4, tp4);
        }
      }
      buf_printf(b, " _t%d; })", tr4);
      return 1;
    }
  }
  /* Method#owner / #receiver (#2701). The creation site knows both: a user
     target's owner is its defining class; a builtin target's owner is the
     receiver's class, and the receiver re-emits when doing so cannot repeat a
     side effect (a literal or a local read). */
  /* `method(:m).receiver.equal?(self)` / `== self` for a bare top-level
     method: the receiver IS the main object, so the identity test folds to
     true. There is no materialized main-object value to emit for the
     receiver alone, but the comparison's answer is static (#3245). */
  if (recv >= 0 && argc == 1 &&
      is_equality_name(name) &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SelfNode") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "receiver")) {
    int mrecv2 = nt_ref(nt, recv, "receiver");
    if (mrecv2 >= 0 && comp_ntype(c, mrecv2) == TY_METHOD) {
      int mn3 = method_recv_node(c, mrecv2);
      int tg3 = mn3 >= 0 ? method_obj_target_mi(c, mn3) : -1;
      int bare3 = mn3 >= 0 && nt_ref(nt, mn3, "receiver") < 0;
      /* at top level (or any main-self context) a bare method's receiver is
         self; an instance-method context binds self too, same answer */
      if (tg3 >= 0 && bare3) {
        buf_puts(b, "((void)(");
        emit_expr(c, mrecv2, b);
        buf_puts(b, "), (sp_bool)1)");
        return 1;
      }
    }
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      (sp_streq(name, "owner") || sp_streq(name, "receiver"))) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int is_bam = target >= 0 && c->scopes[target].name &&
                 strncmp(c->scopes[target].name, "__bam_", 6) == 0;
    int mrecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
    if (sp_streq(name, "owner")) {
      if (target >= 0 && !is_bam) {
        /* a method a module provides is owned by that module, however many
           classes include it (#3662) */
        int ocid = c->scopes[target].origin_module_ci
                     ? c->scopes[target].origin_module_ci - 1
                     : c->scopes[target].class_id;
        const char *ocn = ocid >= 0 ? (class_ruby_name(c, ocid) ? class_ruby_name(c, ocid)
                                                                : c->classes[ocid].name) : "Object";
        buf_printf(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(sp_int)-1, SPL(\"%s\")}))", ocn);
        return 1;
      }
      if (mrecv >= 0) {
        TyKind mrt = comp_ntype(c, mrecv);
        const char *mcls = mrt == TY_STRING ? "String" : mrt == TY_INT ? "Integer"
                         : mrt == TY_FLOAT ? "Float" : mrt == TY_SYMBOL ? "Symbol"
                         : ty_is_array(mrt) ? "Array" : ty_is_hash(mrt) ? "Hash"
                         : mrt == TY_RANGE ? "Range" : mrt == TY_TIME ? "Time" : NULL;
        if (mcls) {
          buf_printf(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), ((sp_Class){(sp_int)-1, SPL(\"%s\")}))", mcls);
          return 1;
        }
      }
      /* A Method no one node names (a local re-written to another Method):
         the owner its rendering carries, as a poly one reads it (#3692) */
      if (mn < 0) {
        buf_puts(b, "({ const char *_o = sp_bm_owner_name(");
        emit_expr(c, recv, b);
        buf_puts(b, "); (sp_Class){(sp_int)-1, _o ? _o : SPL(\"Object\")}; })");
        return 1;
      }
    }
    else if (mrecv >= 0) {   /* receiver */
      const char *mrty = nt_type(nt, mrecv);
      int pure = mrty && (sp_streq(mrty, "LocalVariableReadNode") ||
                          sp_streq(mrty, "IntegerNode") || sp_streq(mrty, "FloatNode") ||
                          sp_streq(mrty, "StringNode") || sp_streq(mrty, "SymbolNode") ||
                          sp_streq(mrty, "ArrayNode") || sp_streq(mrty, "SelfNode") ||
                          sp_streq(mrty, "InstanceVariableReadNode"));
      if (pure) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ");
        emit_expr(c, mrecv, b); buf_puts(b, ")");
        return 1;
      }
      /* An expression receiver (`Calc.new(3).method(:add)`) must not be
         evaluated a second time -- and need not be: the Method object carries
         the self it bound (#3693). */
      TyKind mrt2 = comp_ntype(c, mrecv);
      if (ty_is_object(mrt2) && !comp_ty_value_obj(c, mrt2)) {
        int trv = ++g_tmp;
        buf_printf(b, "({ sp_BoundMethod *_t%d = ", trv);
        emit_expr(c, recv, b);
        buf_printf(b, "; (sp_%s *)_t%d->self; })",
                   c->classes[ty_object_class(mrt2)].c_name, trv);
        return 1;
      }
    }
  }
  /* <method>.arity -> a compile-time constant from the target method's param
     shape, read straight off the DefNode's parameters node (the Scope counts
     fold keyword and post-rest params into nparams/nrequired, so they cannot
     reconstruct the arity). Per Ruby: a method is variadic (arity -(req + 1))
     if it has an optional positional, a rest `*`, a forwarding `...`, or a
     keyword block that is not mandatory; otherwise it reports its required
     count. Required positionals, post-splat requireds, and a *mandatory*
     keyword block (a required keyword, which counts as one fixed argument) all
     contribute to that required count. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "arity")) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    /* a builtin-receiver method object: the arity comes from the CRuby table
       keyed on the receiver's class and the literal method name (#2700). Such
       a method() was retargeted at a synthesized __bam_* wrapper, whose params
       are ABI plumbing, not the builtin's signature -- recover the original
       name from the wrapper body's tail call. */
    int is_bam = target >= 0 && c->scopes[target].name &&
                 strncmp(c->scopes[target].name, "__bam_", 6) == 0;
    /* a boxed receiver's wrapper: the Method carries the arity its receiver's
       run-time class gave it */
    if (is_bam && c->scopes[target].def_node >= 0 &&
        nt_int(nt, c->scopes[target].def_node, "bam_poly", 0)) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_puts(b, ")->arity");
      return 1;
    }
    if ((target < 0 || is_bam) && mn >= 0) {
      const char *msym = method_sym_arg(c, mn);
      if (is_bam) msym = bam_builtin_sym(c, target, NULL);
      int mrecv = nt_ref(nt, mn, "receiver");
      TyKind mrt = mrecv >= 0 ? comp_ntype(c, mrecv) : TY_UNKNOWN;
      const char *mcls = mrt == TY_STRING ? "String" : mrt == TY_INT ? "Integer"
                       : mrt == TY_FLOAT ? "Float" : mrt == TY_SYMBOL ? "Symbol"
                       : ty_is_array(mrt) ? "Array" : ty_is_hash(mrt) ? "Hash"
                       : mrt == TY_RANGE ? "Range" : mrt == TY_TIME ? "Time" : NULL;
      int ba = 0, have_ba = 0;
      if (msym && mcls && builtin_method_arity(mcls, msym, &ba)) have_ba = 1;
      /* A receiverless Kernel wrapper (`method(:String)`) has no receiver
         class; the builtin's real arity is keyed under "Kernel" (#4395). */
      else if (is_bam && mrecv < 0 && msym && builtin_method_arity("Kernel", msym, &ba)) have_ba = 1;
      if (have_ba) {
        /* evaluate for effect: the method()'s own receiver when the chain is
           inline (creating the bound method emits a non-void-castable
           expression), or the local read when the method object is var-held */
        buf_printf(b, "((void)(");
        if (mn == recv) { if (mrecv >= 0) emit_expr(c, mrecv, b); else buf_puts(b, "0"); }
        else emit_expr(c, recv, b);
        buf_printf(b, "), (sp_int)%d)", ba);
        return 1;
      }
    }
    if (target >= 0 && c->scopes[target].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[target].def_node, "parameters");
      int ok = 1;
      int n_req = 0, n_opt = 0, n_post = 0;
      int has_rest = 0, has_forward = 0, kw_block = 0, has_req_kw = 0;
      if (pn >= 0) {
        nt_arr(c->nt, pn, "requireds", &n_req);
        nt_arr(c->nt, pn, "optionals", &n_opt);
        nt_arr(c->nt, pn, "posts", &n_post);
        /* a synthesized __bam_ wrapper's leading __bam_r is the bound receiver,
           not a real argument: the accessor/method it forwards to has one fewer
           required param (reader -> 0, writer -> 1) (#3110 follow-up) */
        if (c->scopes[target].name &&
            strncmp(c->scopes[target].name, "__bam_", 6) == 0 && n_req > 0)
          n_req--;
        int rp = nt_ref(c->nt, pn, "rest");
        if (rp >= 0) {
          const char *rty = nt_type(c->nt, rp);
          if (rty && sp_streq(rty, "RestParameterNode")) has_rest = 1;
          else ok = 0;  /* e.g. ImplicitRestNode: leave unsupported */
        }
        int kn = 0;
        const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
        if (kn > 0) kw_block = 1;
        for (int i = 0; i < kn; i++) {
          const char *kty = nt_type(c->nt, kws[i]);
          if (kty && sp_streq(kty, "RequiredKeywordParameterNode")) has_req_kw = 1;
        }
        int kwrp = nt_ref(c->nt, pn, "keyword_rest");
        if (kwrp >= 0) {
          const char *kty = nt_type(c->nt, kwrp);
          if (kty && sp_streq(kty, "KeywordRestParameterNode")) kw_block = 1;
          else if (kty && sp_streq(kty, "ForwardingParameterNode")) has_forward = 1;
        }
      }
      if (ok) {
        int req = n_req + n_post + (has_req_kw ? 1 : 0);
        int variadic = n_opt > 0 || has_rest || has_forward || (kw_block && !has_req_kw);
        int arity = variadic ? -(req + 1) : req;
        buf_printf(b, "%d", arity);
        return 1;
      }
    }
    /* A Method no one node names (a local re-written to another Method):
       the arity stamped on it at creation, as a poly one reads it (#3231) */
    if (mn < 0) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_puts(b, ")->arity");
      return 1;
    }
    /* A Method built at run time on a receiver of no static class --
       `callable.method(:call).arity`, activesupport's arity_of_callable over
       a proc or an object with #call -- binds no target the signature could
       be read from (the poly `method` above raises when reached). Answer per
       runtime class instead: a Proc's own arity, and each user class's
       method of that name from its signature; anything else is the
       NoMethodError the poly `method` would have raised. */
    if (target < 0 && mn >= 0 && nt_ref(nt, mn, "receiver") >= 0 &&
        comp_ntype(c, nt_ref(nt, mn, "receiver")) == TY_POLY) {
      const char *msym = method_sym_arg(c, mn);
      if (msym) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, nt_ref(nt, mn, "receiver"), b); buf_puts(b, "; ");
        buf_printf(b, "_t%d.cls_id == SP_BUILTIN_PROC ? sp_proc_arity((sp_Proc *)_t%d.v.p)", t, t);
        for (int k = 0; k < c->nclasses; k++) {
          int a = 0;
          int mi = comp_method_in_chain(c, k, msym, NULL);
          if (mi < 0 || !method_scope_arity(c, mi, &a)) continue;
          buf_printf(b, " : (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d) ? (sp_int)%d", t, t, k, a);
        }
        buf_printf(b, " : (sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"undefined method '%s'\")[1])), (sp_int)0); })", msym);
        return 1;
      }
    }
  }
  /* <method>.to_proc -> a first-class Proc trampolining into the compiled
     method. The proc ABI publishes every argument boxed on the side-channel
     (a type-erased proc call always force-boxes), so the trampoline unboxes
     each argument to the target's parameter type and boxes the result. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      sp_streq(name, "to_proc")) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    if (target >= 0 && target_recvless) {
      Scope *ts = &c->scopes[target];
      int pid = ++g_proc_counter;
      buf_printf(&g_proc_protos, "static sp_int _proc_%d(void *_cap, sp_int argc, sp_int *args);\n", pid);
      Buf *pb = &g_procs;
      buf_printf(pb, "static sp_int _proc_%d(void *_cap, sp_int argc, sp_int *args) {\n", pid);
      buf_puts(pb, "  (void)_cap; (void)argc; (void)args;\n");
      Buf callb; memset(&callb, 0, sizeof callb);
      emit_method_cname(c, ts, &callb);
      buf_puts(&callb, "(");
      int ok_params = 1;
      for (int k = 0; k < ts->nparams && k < 16; k++) {
        LocalVar *pv = scope_local(ts, ts->pnames[k]);
        TyKind pt = pv ? pv->type : TY_INT;
        if (k) buf_puts(&callb, ", ");
        char slot[40]; snprintf(slot, sizeof slot, "_sp_proc_poly_args[%d]", k);
        if (pt == TY_POLY) buf_puts(&callb, slot);
        else if (ty_is_object(pt)) {
          buf_puts(&callb, "(");
          emit_ctype(c, pt, &callb);
          buf_printf(&callb, ")%s.v.p", slot);
        }
        else if (c_type_name(pt)) emit_unbox_text(c, pt, slot, &callb);
        else ok_params = 0;
      }
      buf_puts(&callb, ")");
      if (ok_params && ts->nparams <= 16) {
        buf_puts(pb, "  _sp_proc_poly_ret = ");
        if (ts->ret == TY_POLY) buf_printf(pb, "%s", callb.p ? callb.p : "");
        else emit_boxed_text(c, ts->ret, callb.p ? callb.p : "", pb);
        buf_puts(pb, ";\n  return 0;\n}\n");
        g_needs_proc_poly_argslot = 1;
        buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, NULL, NULL, %d, TRUE, 0, NULL, NULL)",
                   pid, ts->nparams);
        free(callb.p);
        return 1;
      }
      free(callb.p);
    }
  }
  return 0;
}

/* a Method or a Proc read out of a container widened to poly: name / owner / receiver, arity, and call / () / yield answered from the boxed value */
int emit_call_poly_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* A Method read out of a container widened to poly: answer from the
     sp_BoundMethod itself, which carries everything the compile-time arms
     read off the AST (#3692). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "owner") ||
       /* #receiver is also an exception's, so take this arm only when the
          program builds Method objects at all and no user class owns the
          name (#3692) */
       (sp_streq(name, "receiver") && an_program_builds_methods(c))) &&
      /* #name also names a Class and an Encoding, #receiver an exception's:
         take this arm only where the result slot is the boxed one those other
         readings do not use */
      comp_ntype(c, id) == TY_POLY &&
      (!sp_streq(name, "name") || !sp_feature_required("ostruct"))) {
    /* A user READER owns the name just as a user method does: `attr_accessor
       :name` on a class this receiver could be makes the builtin arm the wrong
       answer, and the general cls_id dispatch does emit a reader arm. Counting
       methods alone let Method#name / Class#name claim the call and raise for
       an object whose class answers it perfectly well (#4036). */
    int has_user = user_defines_or_reads(c, name);
    if (!has_user) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.cls_id == SP_BUILTIN_METHOD ? ", t);
      if (sp_streq(name, "name"))
        buf_printf(b, "sp_box_sym(sp_sym_intern(((sp_BoundMethod *)_t%d.v.p)->name))"
                      " : _t%d.tag == SP_TAG_CLASS ? sp_box_str(sp_class_val_name(_t%d))"
                      " : _t%d.tag == SP_TAG_ENCODING ? sp_box_str(_t%d.v.s)", t, t, t, t, t);
      else if (sp_streq(name, "owner"))
        buf_printf(b, "({ const char *_o = sp_bm_owner_name((sp_BoundMethod *)_t%d.v.p);"
                      " _o ? sp_box_class_name(_o) : sp_box_nil(); })", t);


      else if (sp_streq(name, "unbind"))
        buf_printf(b, "sp_box_obj(sp_bm_unbind((sp_BoundMethod *)_t%d.v.p), SP_BUILTIN_METHOD)", t);
      else if (sp_streq(name, "receiver"))
        /* the object the Method bound: a reference self carries its own class
           id in its first field, which is how any boxed object names its class */
        buf_printf(b, "({ sp_BoundMethod *_m = (sp_BoundMethod *)_t%d.v.p;"
                      " (_m->self && _m->self_kind == SP_BM_SELF_OBJ)"
                      " ? sp_box_nullable_obj_dyn(_m->self, 0) : sp_box_nil(); })", t);
      else
        buf_printf(b, "sp_box_obj(sp_method_to_proc((sp_BoundMethod *)_t%d.v.p), SP_BUILTIN_PROC)", t);
      buf_printf(b, " : (sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)), sp_box_nil()); })", name, t);
      return 1;
    }
  }
  /* poly_val.arity -- a Method read out of a container widened to poly. The
     arity was stamped onto the sp_BoundMethod at creation, so read it back
     (a non-Method poly here is a genuine NoMethodError, as before) (#3231). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY && argc == 0 && sp_streq(name, "arity")) {
    int has_user_arity = 0;
    for (int _k = 0; _k < c->nclasses && !has_user_arity; _k++)
      if (comp_method_in_class(c, _k, name) >= 0) has_user_arity = 1;
    if (!has_user_arity) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.cls_id == SP_BUILTIN_METHOD ? ((sp_BoundMethod *)_t%d.v.p)->arity"
                    " : _t%d.cls_id == SP_BUILTIN_PROC ? sp_proc_arity((sp_Proc *)_t%d.v.p)"
                    " : (sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"undefined method 'arity'\")[1])), (sp_int)0); })",
                 t, t, t, t);
      return 1;
    }
  }
  /* poly_val.call -- the poly value is a proc; unbox then call. Only applies
     when no user-defined class has a `call` method that could take THIS
     call: when one does, the poly method dispatch carries a callable pre-arm
     that routes a boxed Proc/Curry/Method through this same machinery, and
     the user classes take their switch arms (#4395). A user `call` whose
     required arity exceeds the site's argument count is no candidate there
     -- the dispatch counts candidates by `argc >= nrequired` and declines
     with none -- so stepping aside for it left the site at the unresolved
     raise, with no arm at all: one `def call(severity, time, progname,
     message)` on a log formatter turned every `pred.call(host, port)` on a
     Proc slot into NoMethodError. */
  /* Proc#yield is #call as well; a Method has no `yield`, so one read out
     of the slot raises NoMethodError, as anything else that is no Proc
     does (is_yield below) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "yield"))) {
    int has_user_call = 0;
    for (int _k = 0; _k < c->nclasses && !has_user_call; _k++) {
      int umi = comp_method_in_class(c, _k, name);
      if (umi >= 0 && argc >= c->scopes[umi].nrequired) has_user_call = 1;
    }
    /* The callable ABI packs at most 16 positional args into sp_int[16];
       beyond that the publish loop writes _sp_proc_poly_args[16] out of
       bounds and the compound literal has 17+ initializers (a hard -Werror
       failure). Decline to the general dispatch for a wider call, matching
       the shadowed pre-arm (#4395). */
    if (!has_user_call && argc <= 16) {
      int t = ++g_tmp;
      /* Render first, then write the line. emit_expr may need g_pre lines of
         its own -- a container literal builds itself there -- and writing
         straight into a half-built line spliced them into the MIDDLE of it,
         which is not C at all: `{ plain: ->(v){}, quoted: ->(v){} }[style].call`
         emitted the hash's construction inside the index call's argument list
         (#4065). */
      { Buf rvb; memset(&rvb, 0, sizeof rvb);
        emit_expr(c, recv, &rvb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", t, rvb.p ? rvb.p : "sp_box_nil()");
        free(rvb.p); }
      /* rooted: the arguments are evaluated after it and allocate (a lambda
         literal builds its proc), and a callable a call answered is held by
         nothing else */
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t);
      /* `.yield`: the call below for a Proc or a curried one, and
         NoMethodError for anything else, a Method included */
      int is_yield = sp_streq(name, "yield");
      char yield_else[96] = "";
      if (is_yield) {
        buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.v.p && (_t%d.cls_id == SP_BUILTIN_PROC ||"
                      " _t%d.cls_id == SP_BUILTIN_CURRY) ? ", t, t, t, t);
        snprintf(yield_else, sizeof yield_else,
                 " : (sp_raise_nomethod(sp_nomethod_msg(\"yield\", _t%d)), sp_box_nil()))", t);
      }
      /* the poly callable may be a Proc or a bound Method (different ABIs).
         Under promote the bound method is poly-signatured, so call it through
         the poly ABI and unbox the result back to the sp_int the Proc arm
         yields, keeping the ternary's two branches a single type. */
      int mabi_poly = g_promote_mode;
      const char *aty = mabi_poly ? "sp_RbVal" : "sp_int";
      /* A splat among the arguments makes the arg count dynamic: build the full
         (boxed) argument array and spread it, as the statically-typed Proc path
         does. The poly value may be a Proc, a Curry, or a bound Method, so the
         spread dispatches on the value's class (sp_poly_callable_spread): reading
         it as an sp_Proc made a Method call segfault (#3178, #4395). */
      {
        if (call_args_need_spread(nt, argv, argc)) {
          char kwp[24];
          int ta = emit_spread_args_kw(c, argv, argc, kwp, sizeof kwp);
          buf_printf(b, "sp_poly_callable_spread(_t%d, sp_box_poly_array(_t%d), %s)%s", t, ta, kwp, yield_else);
          return 1;
        }
      }
      /* Hoist every argument to a rooted temp and publish it (boxed) to the
         proc poly-arg side-channel, mirroring the general proc-call ABI. The
         callee may be a poly-signatured Proc whose params read from that
         side-channel (`cb.call(5)` where cb was read out of a container and its
         param unified to poly), so even an Integer arg must be published, not
         just left in the sp_int slot (#2883). A poly arg that is itself a
         side-effecting call is also evaluated exactly once this way (#2874).
         Promote hoists too: its arguments are mostly TY_POLY (the int
         widening), and the un-hoisted emission handed the raw sp_RbVal to the
         Proc arm's sp_int[16] slots -- the generated C did not compile -- while
         the absent publish left a poly-signatured Proc callee reading a stale
         side channel. The Method arm reads the same temps boxed, exactly as
         emit_poly_callable_prearm's PA_MARG does. */
      int *aptmp = argc ? calloc(argc, sizeof(int)) : NULL;
      Buf pubs; memset(&pubs, 0, sizeof pubs);
      if (aptmp) {
        g_needs_proc_poly_argslot = 1;
        for (int k = 0; k < argc; k++) {
          TyKind at = comp_ntype(c, argv[k]);
          int storable = ty_is_object(at) || c_type_name(at) != NULL;
          aptmp[k] = ++g_tmp;
          /* the arg may spill setup into g_pre; emit into a private buffer
             first so it lands ahead of the temp declaration, not mid-line. */
          Buf inner; memset(&inner, 0, sizeof inner);
          Buf valb; memset(&valb, 0, sizeof valb);
          Buf *saved_pre = g_pre; g_pre = &inner;
          emit_expr(c, argv[k], &valb);
          g_pre = saved_pre;
          if (inner.p) buf_puts(g_pre, inner.p);
          emit_indent(g_pre, g_indent);
          if (storable) emit_ctype(c, at, g_pre); else buf_puts(g_pre, "sp_int");
          buf_printf(g_pre, " _t%d = %s;\n", aptmp[k], valb.p ? valb.p : "0");
          if (at == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", aptmp[k]); }
          else if (proc_slot_is_ptr(at) || at == TY_PROC) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", aptmp[k]); }
          /* The publish belongs to THIS call, not to the statement above it:
             the side channel is one global array, and an argument that is
             itself a proc call writes it -- and its callee's prologue then
             clears it -- between a prelude publish and the call that reads it.
             `step.call(acc, fn.call(input))` lost acc that way, arriving nil
             (#4333). Collected here and emitted as a comma sequence
             immediately in front of the call, which is the shape
             emit_proc_call_args already uses for the same reason (#4059).
             The temps stay in the prelude: they are what the roots are on. */
          buf_printf(&pubs, "_sp_proc_poly_args[%d] = ", k);
          { char tn[24]; snprintf(tn, sizeof tn, "_t%d", aptmp[k]);
            if (storable) emit_boxed_text(c, at, tn, &pubs); else buf_puts(&pubs, "sp_box_nil()"); }
          buf_puts(&pubs, ", ");
          free(inner.p); free(valb.p);
        }
      }
      /* sp_poly_to_i for a bound Method's legacy slot, sp_poly_slot_i for
         the Proc's speculative one (see PA_SLOT) */
      const char *pc_conv = "sp_poly_to_i";
      #define EMIT_POLY_CALL_SLOT(k) do { \
        TyKind _at = comp_ntype(c, argv[k]); \
        if (aptmp) { \
          if (_at == TY_POLY) buf_printf(b, "%s(_t%d)", pc_conv, aptmp[k]); \
          else if (proc_slot_is_ptr(_at) || _at == TY_PROC) buf_printf(b, "(sp_int)(uintptr_t)_t%d", aptmp[k]); \
          /* A value carried by the boxed side channel takes a placeholder in \
             the legacy sp_int slot: a Float does not fit the integer \
             register, and a struct-valued kind (Time, a Range, a Complex, a \
             Rational, a Class) does not convert to sp_int at all -- it \
             reached the slot verbatim and the C compiler refused the whole \
             program (#4804). The real value is published beside it. */ \
          else if (_at == TY_FLOAT || proc_slot_via_poly(c, _at)) buf_puts(b, "0"); \
          else buf_printf(b, "_t%d", aptmp[k]); \
        } \
        else emit_expr(c, argv[k], b); \
      } while (0)
      /* the promote Method arm takes the hoisted temp BOXED (its target is
         poly-signatured), the same shape as the prearm's PA_MARG */
      #define EMIT_POLY_CALL_MARG(k) do { \
        if (mabi_poly && aptmp) { \
          TyKind _at = comp_ntype(c, argv[k]); \
          if (_at == TY_POLY) buf_printf(b, "_t%d", aptmp[k]); \
          else { char _tn[24]; snprintf(_tn, sizeof _tn, "_t%d", aptmp[k]); \
                 emit_boxed_text(c, _at, _tn, b); } \
        } \
        else if (mabi_poly) emit_boxed(c, argv[k], b); \
        else EMIT_POLY_CALL_SLOT(k); \
      } while (0)
      /* both arms yield a BOXED result now that the call types poly: the
         Method arm's legacy int ABI result boxes, the Proc arm reads the
         boxed return slot intact */
      /* The bound Method may wrap an instance method (self-ful C ABI:
         fn(self, args)) or a top-level def (self-less: fn(args)); the latter
         is created with a NULL self. Branch at run time so a top-level Method
         read out of a container is not invoked with a spurious leading self
         arg -- which would shift every real argument by one (#3231). */
      if (pubs.p) { buf_puts(b, "("); buf_puts(b, pubs.p); }
      /* A Method whose target cannot read the argument class, or whose
         stamped per-position class differs, cannot be invoked through the
         cast below: its fn signature reads the arg registers as the wrong
         types (garbage or a crash). The bind sites stamp the target's class
         mask and arity, and a Method that does not match falls to the Proc
         arm's sp_poly_callable_call, which raises CRuby's NoMethodError.
         In promote mode every method is poly-signatured, so the flag is not
         consulted. */
      int method_ok = 1;
      char method_sig[8 * 64 + 1];
      method_sig[0] = 0;
      if (!mabi_poly) method_ok = call_arg_sig(c, argv, argc, method_sig, sizeof method_sig);
      /* The legacy box reads the Method's stamped Ruby return kind, so a
         typed-array adapter that answers an array/string is not mis-tagged as
         an Integer (#4395). */
      char boxopen[64];
      snprintf(boxopen, sizeof boxopen, "sp_bm_box_ret((sp_BoundMethod *)_t%d.v.p, ", t);
      if (mabi_poly)
        /* A NULL-fn Method (an unresolved class value) has no callable
           address, and a target whose signature is NOT the poly ABI (a
           Float parameter, a String return, a mismatched arity) would read
           the sp_RbVal registers as garbage: gate on the bind-time stamp so
           both fall to sp_poly_callable_call's NoMethodError instead. */
        buf_printf(b, "(_t%d.cls_id == SP_BUILTIN_METHOD && ((sp_BoundMethod *)_t%d.v.p)->fn"
                      " && sp_bm_poly_abi_ok((sp_BoundMethod *)_t%d.v.p, %d) ? (((sp_BoundMethod *)_t%d.v.p)->recv_bound ? ", t, t, t, argc, t);
      else {
        buf_printf(b, "(_t%d.cls_id == SP_BUILTIN_METHOD && ", t);
        if (method_ok) emit_bm_legacy_ok(b, t, argc, method_sig);
        else buf_puts(b, "0");
        buf_printf(b, " ? (((sp_BoundMethod *)_t%d.v.p)->recv_bound ? ", t);
      }
      /* Two casts per receiver shape, as in emit_poly_callable_prearm --
         three passes in BOTH modes, keyed on the shared runtime ret kind:
         the nil (void) cast, the sp_int cast boxed by the stamped kind, and
         the sp_RbVal cast for a poly-returning target (a 16-byte struct
         return no sp_int cast can read). Only the ARGUMENT slots differ by
         mode (aty): promote's poly ABI still returns bool/Symbol/array/
         object kinds un-widened, and each takes its own cast. */
      /* self-ful arm: fn((void *)self, args...) */
      for (int pi = 0; pi < 3; pi++) {
        int pass = pi == 0 ? 2 : pi == 1 ? 0 : 1;   /* nil (void), int, poly */
        const char *rty = pass == 2 ? "void" : (pass == 0) ? "sp_int" : "sp_RbVal";
        const char *bo = (pass == 0) ? boxopen : "";
        buf_printf(b, pass == 2 ? "(((sp_BoundMethod *)_t%d.v.p)->legacy_ret == SP_BM_RET_NIL ? (" : pass == 0 ? "(((sp_BoundMethod *)_t%d.v.p)->legacy_ret != SP_BM_RET_POLY ? " : "", t);
        buf_printf(b, "%s((%s (*)(void *", bo, rty);
        for (int k = 0; k < argc; k++) buf_printf(b, ", %s", aty);
        buf_printf(b, "))(uintptr_t)((sp_BoundMethod *)_t%d.v.p)->fn)((void *)((sp_BoundMethod *)_t%d.v.p)->self", t, t);
        for (int k = 0; k < argc; k++) {
          buf_puts(b, ", ");
          EMIT_POLY_CALL_MARG(k);
        }
        buf_printf(b, ")%s", pass == 2 ? ", sp_box_nil())" : pass == 0 ? ")" : "");
        buf_puts(b, pass != 1 ? " : " : "))");
      }
      buf_puts(b, " : ");
      /* self-less arm: fn(args...) with no leading self */
      for (int pi = 0; pi < 3; pi++) {
        int pass = pi == 0 ? 2 : pi == 1 ? 0 : 1;   /* nil (void), int, poly */
        const char *rty = pass == 2 ? "void" : (pass == 0) ? "sp_int" : "sp_RbVal";
        const char *bo = (pass == 0) ? boxopen : "";
        buf_printf(b, pass == 2 ? "(((sp_BoundMethod *)_t%d.v.p)->legacy_ret == SP_BM_RET_NIL ? (" : pass == 0 ? "(((sp_BoundMethod *)_t%d.v.p)->legacy_ret != SP_BM_RET_POLY ? " : "", t);
        buf_printf(b, "%s((%s (*)(", bo, rty);
        for (int k = 0; k < argc; k++) buf_printf(b, "%s%s", k ? ", " : "", aty);
        if (argc == 0) buf_puts(b, "void");
        buf_printf(b, "))(uintptr_t)((sp_BoundMethod *)_t%d.v.p)->fn)(", t);
        for (int k = 0; k < argc; k++) {
          if (k) buf_puts(b, ", ");
          EMIT_POLY_CALL_MARG(k);
        }
        buf_printf(b, ")%s", pass == 2 ? ", sp_box_nil())" : pass == 0 ? ")" : "");
        buf_puts(b, pass != 1 ? " : " : "))");
      }
      buf_puts(b, ")");
      /* the Proc publishes its result in _sp_proc_poly_ret (universal return
         ABI); evaluate for effect and read the boxed sp_RbVal intact, matching
         the Method branch's now-boxed result so the ternary's two arms are a
         single type. */
      /* through the callable helper, so a curried Proc in the slot takes its
         arguments instead of being read as an sp_Proc (#3885) */
      buf_printf(b, " : sp_poly_callable_call_kw(_t%d, %d, (sp_int[16]){", t, argc);
      pc_conv = "sp_poly_slot_i";
      for (int k = 0; k < argc; k++) {
        if (k) buf_puts(b, ", ");
        EMIT_POLY_CALL_SLOT(k);
      }
      if (argc == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
      buf_printf(b, "}, %d))", argc == 0 ? 0 : nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? 2 : 1);
      if (pubs.p) buf_puts(b, ")");
      buf_puts(b, yield_else);
      free(pubs.p);
      #undef EMIT_POLY_CALL_MARG
      #undef EMIT_POLY_CALL_SLOT
      free(aptmp);
      return 1;
    }
  }
  return 0;
}
