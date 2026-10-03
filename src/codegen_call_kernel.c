/* codegen_call_kernel.c -- emit_call_body's arms of the Kernel functions, control flow and the process environment.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* the Kernel calls without a receiver: __dir__, at_exit, attr_* declarations, block_given?,
   the conversion functions (Integer, Float, String, Array, Hash, Rational, Complex), sleep,
   exit / exit! / abort, puts / print, p / pp, warn */
int emit_call_kernel_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* __dir__ -> the source file's directory (compile-time literal, mirroring
     the legacy generator). */
  if (recv < 0 && sp_streq(name, "__dir__") && argc == 0) {
    /* a required file's own directory, which the parser stamped (#4839) */
    char dir[1024];
    an_node_dir(nt, id, dir, sizeof dir);
    emit_str_literal(b, dir);
    return 1;
  }

  /* at_exit { ... } -> register the block as a Proc; sp_at_exit_run runs
     the hooks in reverse order, from main()'s tail and from every other path
     that ends the program. The registration expression evaluates to the proc;
     sp_at_exit_push locks the table in the threaded build. */
  if (recv < 0 && sp_streq(name, "at_exit") && nt_ref(nt, id, "block") >= 0 && !bare_call_class_owned(c, id)) {
    g_needs_at_exit = 1;
    buf_puts(b, "sp_at_exit_push(");
    emit_proc_literal(c, id, b);
    buf_puts(b, ")");
    return 1;
  }

  /* `x = private def m ... end` in a class body: the visibility was recorded
     at analysis time and the def is emitted with the class; the value is the
     name, the Array of names, or nil for the bare form (the class itself for
     private_class_method). */
  {
    int vk = vis_decl_call(c, id);
    if (vk == VIS_DECL_NIL) { buf_puts(b, "0"); return 1; }
    if (vk == VIS_DECL_SELF) { buf_printf(b, "((sp_Class){%d})", self_class_body(c, id)); return 1; }
    if (vk == VIS_DECL_SYM) {
      const char *vn = nt_kind(nt, argv[0]) == NK_DefNode ? nt_str(nt, argv[0], "name")
                                                          : nt_str(nt, argv[0], "value");
      buf_printf(b, "((sp_sym)%d)", comp_sym_intern(c, vn ? vn : ""));
      return 1;
    }
    if (vk == VIS_DECL_ARRAY) {
      int en = argc; const int *ev = argv;
      if (argc == 1 && nt_kind(nt, argv[0]) == NK_ArrayNode) ev = nt_arr(nt, argv[0], "elements", &en);
      int ta = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new();", ta);
      for (int i = 0; i < en; i++) {
        const char *vn = nt_kind(nt, ev[i]) == NK_DefNode ? nt_str(nt, ev[i], "name")
                                                          : nt_str(nt, ev[i], "value");
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", ta, comp_sym_intern(c, vn ? vn : ""));
      }
      buf_printf(b, " _t%d; })", ta);
      return 1;
    }
  }

  /* `r = attr_accessor :a` in a class body: the definitions were made at
     analysis time; the value is the Array of the method names, readers
     before writers per name ([:a, :a=]). A writer's name is a symbol the
     program did not spell, so it is interned at run time. */
  if (attr_decl_call(c, id)) {
    int acc = sp_streq(name, "attr_accessor"), wr = sp_streq(name, "attr_writer");
    int ta = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new();", ta);
    for (int i = 0; i < argc; i++) {
      const char *an = nt_str(nt, argv[i], "value");
      if (!an) an = "";
      if (!wr)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", ta, comp_sym_intern(c, an));
      if (wr || acc) {
        size_t wl = strlen(an) + 2;
        char *wn = malloc(wl);
        snprintf(wn, wl, "%s=", an);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", ta);
        emit_str_literal(b, wn);
        buf_puts(b, ")));");
        free(wn);
      }
    }
    buf_printf(b, " _t%d; })", ta);
    return 1;
  }

  /* __method__ / __callee__ -> the enclosing method's name as a symbol
     (nil at the top level) */
  if (recv < 0 && argc == 0 &&
      (sp_streq(name, "__method__") || sp_streq(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    /* __callee__ names the ALIAS the call spelled, which only the call site
       knows: an alias shares the definition's one function. The prologue took
       the channel; fall back to the definition's name when nothing wrote it
       (a direct call, or a path with no write). __method__ always answers the
       definition's name (#3729). */
    if (s && s->name && s->name[0] && sp_streq(name, "__callee__"))
      buf_printf(b, "(_sp_cal ? sp_sym_intern(_sp_cal) : (sp_sym)%d)", comp_sym_intern(c, s->name));
    else if (s && s->name && s->name[0]) buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, s->name));
    else buf_puts(b, "sp_box_nil()");
    return 1;
  }

  /* Bareword Object#freeze / frozen? (implicit self) inside an instance
     method: heap-represented instances carry real frozen state in the GC
     header (the analyze observation pass forces any freeze-touched class to
     heap representation), so the defensive-freeze idiom
     (`def initialize; ...; freeze; end`) sets the bit and `frozen?` reads it
     back. A class that defines its own freeze/frozen? keeps its method; a
     value-type self (unreachable for observed classes) keeps the identity
     no-op for freeze and the loud reject for frozen?. */
  if (recv < 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "freeze") || sp_streq(name, "frozen?"))) {
    Scope *s = comp_scope_of(c, id);
    int scid = s ? s->class_id : -1;
    if (scid >= 0 && comp_method_in_chain(c, scid, name, NULL) < 0) {
      if (!s->is_cmethod && !c->classes[scid].is_value_type) {
        if (sp_streq(name, "freeze"))
          buf_printf(b, "((sp_%s *)sp_gc_freeze((void *)%s))",
                     c->classes[scid].c_name, g_self ? g_self : "self");
        else
          buf_printf(b, "sp_gc_is_frozen((void *)%s)", g_self ? g_self : "self");
        return 1;
      }
      if (sp_streq(name, "freeze")) {
        buf_puts(b, g_self ? g_self : "self");
        return 1;
      }
    }
  }

  /* block_given? / self.block_given? -> true inside an inlined yielding
     method (we only inline when a block is present). In a lowered yielding
     method the block is the `__yblk__` proc parameter, which is non-NULL
     exactly when the caller passed a block, so test it directly. */
  if (sp_streq(name, "block_given?") &&
      (recv < 0 || (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")))) {
    /* block_given? asks about the innermost method. An inlined yielding method
       (g_block_id >= 0) statically has a block, so fold to 1 even when the
       enclosing method is lowered; only a genuinely lowered scope inspects its
       runtime __yblk__ parameter. */
    if (g_block_id >= 0 && nt_int(nt, g_block_id, "fwd_yield", 0)) {
      /* the literal a forwarded `&b` became: ask about the enclosing
         method's block (see static_block_given_cond) */
      if (g_yield_block_fallback >= 0 || g_yield_proc_ref_fallback) buf_puts(b, "1");
      else if (g_yield_lowered_fallback) {
        const char *sv = g_lowered_blk_name; g_lowered_blk_name = g_yield_lowered_blk_fallback;
        buf_puts(b, "("); emit_yblk_ref(b); buf_puts(b, " != NULL)");
        g_lowered_blk_name = sv;
      }
      else buf_puts(b, "0");
    }
    else if (g_block_id >= 0) {
      buf_puts(b, "1");
    }
    else if (g_current_scope_is_lowered) {
      buf_puts(b, "("); emit_yblk_ref(b); buf_puts(b, " != NULL)");
    }
    /* A proc form receives its block as a parameter, so the question is a
       runtime one about that parameter -- folding to 0 answered `false` inside
       a method the caller had plainly given a block to (#3399). */
    else if (g_yield_proc_ref) {
      buf_printf(b, "(%s != NULL)", g_yield_proc_ref);
    }
    /* a real function that keeps its named &blk (see blk_param_escapes):
       the block is its parameter, present exactly when it is not NULL */
    else if (comp_scope_of(c, id) && comp_scope_of(c, id)->blk_param &&
             comp_scope_of(c, id)->blk_param[0] && !comp_scope_of(c, id)->yields) {
      /* through emit_local_ref: in a block the parameter may be a captured
         cell, not the method's own lv_ */
      Buf lb; memset(&lb, 0, sizeof lb);
      emit_local_ref(c, id, comp_scope_of(c, id)->blk_param, &lb);
      buf_printf(b, "(%s != NULL)", lb.p ? lb.p : "NULL");
      free(lb.p);
    }
    else {
      buf_puts(b, "0");
    }
    return 1;
  }

  /* Kernel conversions. These are private Kernel methods available on every
     object, so an explicit-receiver form (obj.send(:Float, x), desugared to
     obj.Float(x)) dispatches here too when the receiver is a plain user object
     whose chain does not define the name. Only a side-effect-free receiver
     (a local/ivar read or self) is accepted, since it is discarded. */
  /* ...and the enclosing chain's own method, which `comp_method_index`
     does not see: only a TOP-LEVEL def registers there, so a module's
     `module_function; def format(...)` beside a sibling that calls it
     bare lost to Kernel#format (#4592). Same ownership test every
     other Kernel arm here makes. */
  int kconv = (recv < 0 && comp_method_index(c, name) < 0 &&
               !bare_call_class_owned(c, id));
  if (!kconv && recv >= 0) {
    TyKind krt = comp_ntype(c, recv);
    const char *krty = nt_type(nt, recv);
    int krecv_ok = krty &&
        (sp_streq(krty, "LocalVariableReadNode") ||
         sp_streq(krty, "InstanceVariableReadNode") || sp_streq(krty, "SelfNode"));
    int kname_ok = sp_streq(name, "Integer") || sp_streq(name, "Float") ||
        sp_streq(name, "String") || sp_streq(name, "Array") ||
        sp_streq(name, "Hash") ||
        sp_streq(name, "Rational") || sp_streq(name, "Complex");
    if (krecv_ok && kname_ok) {
      if (ty_is_object(krt) &&
          comp_method_in_chain(c, ty_object_class(krt), name, NULL) < 0)
        kconv = 1;
      /* a nil/poly/untyped receiver (e.g. an unassigned @object in specs)
         still reaches Kernel in CRuby; require that NO user class defines the
         name so a real method can never be shadowed. */
      else if ((krt == TY_NIL || krt == TY_POLY || krt == TY_UNKNOWN) &&
               comp_method_index(c, name) < 0)
        kconv = 1;
    }
  }
  if (kconv) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    /* `exception: false` asks for nil instead of a raise. The keyword hash is
       not a positional argument: passed through as one it reached the base
       slot as a pointer (#3718). */
    int kconv_noraise = 0;
    if (ac > 0 && (sp_streq(name, "Integer") || sp_streq(name, "Float"))) {
      const char *lty = nt_type(nt, av[ac - 1]);
      if (lty && sp_streq(lty, "KeywordHashNode")) {
        int en = 0; const int *el = nt_arr(nt, av[ac - 1], "elements", &en);
        for (int e = 0; e < en; e++) {
          int k = nt_ref(nt, el[e], "key"), v = nt_ref(nt, el[e], "value");
          const char *kt = k >= 0 ? nt_type(nt, k) : NULL;
          const char *kn = (kt && sp_streq(kt, "SymbolNode")) ? nt_str(nt, k, "value") : NULL;
          const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
          if (kn && sp_streq(kn, "exception") && vt && sp_streq(vt, "FalseNode"))
            kconv_noraise = 1;
        }
        ac--;   /* the rest of this arm counts positionals only */
      }
    }
    if (kconv_noraise && sp_streq(name, "Integer") && (ac == 1 || ac == 2)) {
      TyKind at0 = comp_ntype(c, av[0]);
      if (at0 == TY_STRING) {
        int promo = comp_ntype(c, id) == TY_POLY;   /* promote mode: a Bignum past sp_int */
        buf_puts(b, promo ? "sp_str_to_i_promote(" : "sp_str_to_i_lenient_base("); emit_expr(c, av[0], b); buf_puts(b, ", ");
        if (ac == 2) emit_int_expr(c, av[1], b); else buf_puts(b, "0");
        buf_puts(b, promo ? ", 2)" : ")");
        return 1;
      }
      /* with a base only a String converts, so a number is nil here */
      if (at0 == TY_INT && ac == 1) { emit_expr(c, av[0], b); return 1; }
      /* a user object, a boxed value that may hold one, or a Float (NaN and
         Infinity are nil, CRuby's FloatDomainError swallowed) converts
         through the runtime's Kernel#Integer path, nil for every failure */
      if (ty_is_object(at0) || at0 == TY_POLY || (at0 == TY_FLOAT && ac == 1)) {
        emit_kconv_call(c, id, av, ac, 0, b);
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), SP_INT_NIL)");
      return 1;
    }
    if (kconv_noraise && sp_streq(name, "Float") && ac == 1) {
      TyKind at0 = comp_ntype(c, av[0]);
      if (at0 == TY_STRING) { buf_puts(b, "sp_str_to_f_lenient("); emit_expr(c, av[0], b); buf_puts(b, ")"); return 1; }
      if (at0 == TY_INT) { buf_puts(b, "((sp_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); return 1; }
      if (at0 == TY_FLOAT) { emit_expr(c, av[0], b); return 1; }
      if (ty_is_object(at0) || at0 == TY_POLY) {
        buf_puts(b, "sp_poly_Float_ex("); emit_boxed(c, av[0], b); buf_puts(b, ", 0)");
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_float_nil())");
      return 1;
    }
    if (sp_streq(name, "Integer") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      /* An empty `{}` / `[]` literal carries no type of its own, so it reached
         the untyped pass-through and the container pointer was answered as the
         integer (#3888). It is a container either way. */
      { NodeKind ek = nt_kind(nt, av[0]);
        if (at == TY_UNKNOWN && (ek == NK_HashNode || ek == NK_KeywordHashNode || ek == NK_ArrayNode))
          at = TY_POLY_ARRAY; }
      /* A nullable Integer or Float holding its sentinel is nil, and nil does
         not convert: CRuby's TypeError, where the sentinel passed through as
         a number (or, as a Float, raised FloatDomainError on its NaN). */
      /* Under --int-overflow=promote the call answers a box (a Float past
         sp_int is a Bignum), and the guard boxes its answer the same way. */
      TyKind rt9 = comp_ntype(c, id);
      if ((at == TY_INT || at == TY_FLOAT) && (rt9 == TY_INT || rt9 == TY_POLY) &&
          call_returns_nullable_int(c, av[0])) {
        char ref[24];
        buf_puts(b, "({ "); emit_sentinel_bind(c, at, av[0], ref, sizeof ref, b);
        buf_puts(b, "if (!"); emit_slot_truthy(at, ref, b);
        buf_puts(b, ") sp_raise_cls(\"TypeError\", \"can't convert nil into Integer\"); ");
        if (at == TY_INT) buf_printf(b, rt9 == TY_POLY ? "sp_box_int(%s); })" : "%s; })", ref);
        else buf_printf(b, rt9 == TY_POLY ? "sp_poly_flo_domain_ck(%s); sp_box_f_to_int(%s); })"
                                          : "sp_poly_flo_domain_ck(%s); sp_float_fit_i(%s); })", ref, ref);
      }
      else if (at == TY_STRING && comp_ntype(c, id) == TY_POLY) {   /* promote mode: a Bignum past sp_int */
        buf_puts(b, "sp_str_to_i_promote("); emit_expr(c, av[0], b); buf_puts(b, ", 0, 1)");
      }
      else if (at == TY_STRING) { buf_puts(b, "sp_str_to_i_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      /* a Float truncates, and NaN or an infinity is CRuby's FloatDomainError
         rather than the C cast's undefined value */
      else if (at == TY_FLOAT) {
        int tf = ++g_tmp;
        buf_printf(b, "({ sp_float _t%d = ", tf); emit_expr(c, av[0], b);
        buf_printf(b, comp_ntype(c, id) == TY_POLY
                        ? "; sp_poly_flo_domain_ck(_t%d); sp_box_f_to_int(_t%d); })"
                        : "; sp_poly_flo_domain_ck(_t%d); sp_float_fit_i(_t%d); })", tf, tf);
      }
      else if (at == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_raise_cls(\"TypeError\", \"can't convert nil into Integer\"), (sp_int)0)"); }  /* #2514 */
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_Integer("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT || at == TY_UNKNOWN) { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      /* Kernel#Integer converts anything answering #to_int, and a Rational or
         a Complex with no imaginary part are among them (#3888): the Integer
         slot's own conversion, which truncates the Rational exactly where the
         trip through a double lost the low digits of a large one. */
      else if (at == TY_RATIONAL || at == TY_COMPLEX) emit_int_expr(c, av[0], b);
      /* a user object converts through its own #to_int, #to_str and #to_i,
         each answer judged by the runtime as CRuby judges it; a class whose
         #to_int or #to_i answers a Bignum types the call as one */
      else if (ty_is_object(at)) {
        emit_kconv_call(c, id, av, ac, 1, b);
      }
      else {
        /* an Array, Hash, Range or Symbol has no to_int: CRuby's TypeError,
           not the value reinterpreted as an integer (#3717); nil, true and
           false name themselves, as sp_convert_src_name spells them for the
           boxed path */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", sp_sprintf(\"can't convert %s into Integer\", sp_convert_src_name(");
        emit_boxed(c, av[0], b);
        buf_puts(b, "))), (sp_int)0)");
      }
      return 1;
    }
    if (sp_streq(name, "Integer") && ac == 2) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) {
        int promo = comp_ntype(c, id) == TY_POLY;   /* promote mode: a Bignum past sp_int */
        buf_puts(b, promo ? "sp_str_to_i_promote(" : "sp_str_to_i_strict_base("); emit_expr(c, av[0], b);
        /* Integer("5", nil) is CRuby's TypeError, not base 0 */
        buf_puts(b, ", "); emit_int_expr(c, av[1], b); buf_puts(b, promo ? ", 1)" : ")");
      }
      /* only a String converts with a base, and whether a boxed value or a
         user object is one -- a plain String, a shared handle, an object's
         #to_str -- the runtime decides, raising CRuby's ArgumentError for
         anything else (#2515) */
      else emit_kconv_call(c, id, av, ac, 1, b);
      return 1;
    }
    if (sp_streq(name, "Float") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      { NodeKind ek = nt_kind(nt, av[0]);
        if (at == TY_UNKNOWN && (ek == NK_HashNode || ek == NK_KeywordHashNode || ek == NK_ArrayNode))
          at = TY_POLY_ARRAY; }
      /* the Float twin of Integer's nullable arm above */
      if ((at == TY_INT || at == TY_FLOAT) && call_returns_nullable_int(c, av[0])) {
        char ref[24];
        buf_puts(b, "({ "); emit_sentinel_bind(c, at, av[0], ref, sizeof ref, b);
        buf_puts(b, "if (!"); emit_slot_truthy(at, ref, b);
        buf_printf(b, ") sp_raise_cls(\"TypeError\", \"can't convert nil into Float\"); (sp_float)%s; })", ref);
      }
      else if (at == TY_STRING) { buf_puts(b, "sp_str_to_f_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT) { buf_puts(b, "((sp_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_raise_cls(\"TypeError\", \"can't convert nil into Float\"), 0.0)"); }  /* #2514 */
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_Float("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT || at == TY_UNKNOWN) { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      /* Kernel#Float converts anything answering #to_f, and a Rational or a
         Complex with no imaginary part are among them (#3888). */
      else if (at == TY_RATIONAL) {
        buf_puts(b, "sp_rational_to_f("); emit_expr(c, av[0], b); buf_puts(b, ")");
      }
      else if (at == TY_COMPLEX) {
        int tc = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", tc); emit_expr(c, av[0], b);
        buf_printf(b, "; if (_t%d.im != 0) sp_raise_cls(\"RangeError\", "
                      "\"can't convert into Float\"); _t%d.re; })", tc, tc);
      }
      /* a user object converts through its own #to_f, the answer judged by
         the runtime as CRuby judges it */
      else if (ty_is_object(at)) {
        buf_puts(b, "sp_poly_Float_ex("); emit_boxed(c, av[0], b); buf_puts(b, ", 1)");
      }
      else {
        /* a Boolean, Symbol, Array or Hash has no #to_f: CRuby's TypeError,
           not the value reinterpreted as a double (#3888) */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), sp_raise_cls(\"TypeError\", sp_sprintf(\"can't convert %s into Float\", sp_convert_src_name(");
        emit_boxed(c, av[0], b);
        buf_puts(b, "))), 0.0)");
      }
      return 1;
    }
    if (sp_streq(name, "String") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { emit_expr(c, av[0], b); }
      /* a nullable Integer or Float holding its sentinel is nil, whose
         String is "": box it, as nil where it is one */
      else if ((at == TY_INT || at == TY_FLOAT) && call_returns_nullable_int(c, av[0])) {
        buf_puts(b, "sp_poly_to_s("); emit_boxed(c, av[0], b); buf_puts(b, ")");
      }
      else if (at == TY_INT) { buf_puts(b, "sp_int_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "sp_float_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      /* a boxed object answering #to_str converts through it
         (sp_poly_check_str), as a typed one does below, and only then through
         #to_s; the root here holds it across both, where sp_poly_check_str
         holds it only across #to_str */
      else if (at == TY_POLY) {
        int ts = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", ts); emit_expr(c, av[0], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); const char *_s%d = sp_poly_check_str(_t%d);"
                      " _s%d ? _s%d : sp_poly_to_s(_t%d); })", ts, ts, ts, ts, ts, ts);
      }
      /* the frozen "true" / "false" true.to_s answers; a bare C literal here
         had no marker byte at all */
      else if (at == TY_BOOL) { buf_puts(b, "(("); emit_expr(c, av[0], b); buf_puts(b, ") ? sp_str_frozen_true : sp_str_frozen_false)"); }
      else if (at == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s_chilled("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_NIL || at == TY_UNKNOWN) { buf_puts(b, "sp_poly_to_s(sp_box_nil())"); }
      /* Kernel#String asks for #to_str first, and only then #to_s (#3721) */
      else if (ty_is_object(at) &&
               comp_method_in_chain(c, ty_object_class(at), "to_str", NULL) >= 0) {
        int tsc = ty_object_class(at), tsd = tsc;
        (void)comp_method_in_chain(c, tsc, "to_str", &tsd);
        if (expr_is_held_ref(c, av[0])) {
          buf_printf(b, "sp_%s_to_str((sp_%s *)(", c->classes[tsd].c_name, c->classes[tsd].c_name);
          emit_expr(c, av[0], b); buf_puts(b, "))");
        }
        else {
          /* rooted: `String(C.new)` holds the fresh object nowhere else while
             its #to_str allocates */
          int tso = ++g_tmp;
          buf_printf(b, "({ sp_%s *_t%d = (sp_%s *)(", c->classes[tsd].c_name, tso, c->classes[tsd].c_name);
          emit_expr(c, av[0], b);
          buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%s_to_str(_t%d); })", tso, c->classes[tsd].c_name, tso);
        }
      }
      else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }  /* container/range/object: #to_s */
      return 1;
    }
    if (sp_streq(name, "Array") && ac == 1) {
      /* an argument already typed as an array is returned as-is (identity and
         element type preserved); a statically scalar argument wraps into a typed
         one-element array (matching the precise inference); everything else
         routes through the runtime coercion, which yields a poly array. */
      TyKind at = comp_ntype(c, av[0]);
      if (ty_is_array(at)) emit_expr(c, av[0], b);
      else if (at == TY_RANGE) {
        /* Array(range) enumerates it */
        int tr6 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tr6); emit_expr(c, av[0], b);
        buf_printf(b, "; sp_range_to_ia(_t%d); })", tr6);
      }
      else if (ty_is_hash(at) && ty_hash_cname(at)) {
        /* Array(hash) is the pair list */
        buf_puts(b, "sp_enum_items_from(");
        emit_boxed(c, av[0], b);
        buf_puts(b, ")");
      }
      else if (at == TY_INT || at == TY_FLOAT || at == TY_STRING) {
        const char *ak = at == TY_INT ? "Int" : at == TY_FLOAT ? "Float" : "Str";
        int t = ++g_tmp;
        /* a nullable Integer or Float holding its sentinel is nil, and
           Array(nil) is empty: the element goes in only when it is one */
        if (at != TY_STRING && call_returns_nullable_int(c, av[0])) {
          char ref[24];
          buf_puts(b, "({ "); emit_sentinel_bind(c, at, av[0], ref, sizeof ref, b);
          buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); if (", ak, t, ak, t);
          emit_slot_truthy(at, ref, b);
          buf_printf(b, ") sp_%sArray_push(_t%d, %s); _t%d; })", ak, t, ref, t);
          return 1;
        }
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); sp_%sArray_push(_t%d, ", ak, t, ak, t, ak, t);
        if (at == TY_INT) emit_int_expr(c, av[0], b);
        else if (at == TY_FLOAT) emit_float_expr(c, av[0], b);
        else emit_expr(c, av[0], b);
        buf_printf(b, "); _t%d; })", t);
      }
      /* Kernel#Array asks the object for #to_ary, then #to_a (#3721) */
      else if (ty_is_object(at) &&
               (comp_method_in_chain(c, ty_object_class(at), "to_ary", NULL) >= 0 ||
                comp_method_in_chain(c, ty_object_class(at), "to_a", NULL) >= 0)) {
        int acid = ty_object_class(at), adef = acid;
        const char *anm = comp_method_in_chain(c, acid, "to_ary", NULL) >= 0 ? "to_ary" : "to_a";
        (void)comp_method_in_chain(c, acid, anm, &adef);
        if (expr_is_held_ref(c, av[0])) {
          buf_printf(b, "sp_%s_%s((sp_%s *)(", c->classes[adef].c_name, mc(anm), c->classes[adef].c_name);
          emit_expr(c, av[0], b); buf_puts(b, "))");
        }
        else {
          /* rooted, as Kernel#String roots its #to_str receiver */
          int tao = ++g_tmp;
          buf_printf(b, "({ sp_%s *_t%d = (sp_%s *)(", c->classes[adef].c_name, tao, c->classes[adef].c_name);
          emit_expr(c, av[0], b);
          buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%s_%s(_t%d); })", tao, c->classes[adef].c_name, mc(anm), tao);
        }
      }
      else { buf_puts(b, "sp_kernel_array("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }
      return 1;
    }
    if (sp_streq(name, "Hash") && ac == 1) {
      /* Kernel#Hash: nil or [] -> {}, a Hash is returned as-is, anything else is
         a TypeError (unlike Array, no per-element wrapping). */
      TyKind at = comp_ntype(c, av[0]);
      int empty_arr_lit = 0;
      if (nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "ArrayNode")) {
        int _en = 0; nt_arr(nt, av[0], "elements", &_en);
        empty_arr_lit = (_en == 0);
      }
      /* `{}` types as no hash variant at all, so the hash test below missed it
         and an empty Hash literal was rejected as unconvertible (#3746) */
      if (nt_type(nt, av[0]) &&
          (sp_streq(nt_type(nt, av[0]), "HashNode") ||
           sp_streq(nt_type(nt, av[0]), "KeywordHashNode"))) {
        int _hn = 0; nt_arr(nt, av[0], "elements", &_hn);
        if (_hn == 0) empty_arr_lit = 1;
      }
      if (ty_is_hash(at)) { emit_expr(c, av[0], b); }
      /* an object answers through its own #to_hash (#3721) */
      else if (ty_is_object(at) &&
               comp_method_in_chain(c, ty_object_class(at), "to_hash", NULL) >= 0) {
        int hci = ty_object_class(at), hdef = hci;
        (void)comp_method_in_chain(c, hci, "to_hash", &hdef);
        buf_printf(b, "sp_%s_to_hash((sp_%s *)(", c->classes[hdef].c_name, c->classes[hdef].c_name);
        emit_expr(c, av[0], b); buf_puts(b, "))");
      }
      else if (at == TY_NIL || empty_arr_lit) {
        /* result type is TY_POLY_POLY_HASH: emit the raw hash pointer */
        buf_puts(b, "((void)("); emit_expr(c, av[0], b);
        buf_puts(b, "), sp_PolyPolyHash_new())");
      }
      else if (at == TY_POLY) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, av[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"
                      " : (_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id)) ? _t%d"
                      " : (sp_raise_cls(\"TypeError\", \"can't convert to Hash\"), sp_box_nil()); })",
                   t, t, t, t);
      }
      else { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_raise_cls(\"TypeError\", \"can't convert to Hash\"), sp_PolyPolyHash_new())"); }
      return 1;
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && ac == 1 &&
        nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "SplatNode")) {
      /* format(*args): the array's head is the format string */
      int sfx = nt_ref(nt, av[0], "expression");
      if (sfx >= 0) {
        buf_puts(b, "sp_str_format_splat(");
        emit_boxed(c, sfx, b);
        buf_puts(b, ")");
        return 1;
      }
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && ac >= 1) {
      /* format(fmt, *args) -> sp_str_format_polyarr(fmt, poly_arr) */
      int tf = ++g_tmp, ta = ++g_tmp;
      /* Emit the format into a local buffer BEFORE opening the `const char
         *_t =` line: a format that is itself a call rooting its operands
         pushes those statements to g_pre, which must land ahead of this
         declaration, not inside its initializer (the args below do the same,
         #1498 / #1508). */
      Buf fb; memset(&fb, 0, sizeof fb);
      emit_str_expr(c, av[0], &fb);
      emit_indent(g_pre, g_indent);
      /* rooted too: a #to_str's answer lives on the heap, and every arg
         below boxes */
      buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n", tf, fb.p ? fb.p : "", tf);
      free(fb.p);
      emit_pre_format_args(c, av, ac, ta);
      buf_printf(b, "sp_str_format_polyarr(_t%d, _t%d)", tf, ta);
      return 1;
    }
    if (sp_streq(name, "rand")) {
      if (ac == 0) { buf_puts(b, "sp_krand_float()"); return 1; }
      TyKind a0t = comp_ntype(c, av[0]);
      if (a0t == TY_FLOAT_RANGE) {   /* rand(1.0..10.0) -> a Float in [first, last) */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, av[0], b);
        buf_printf(b, "; _t%d.first + sp_Random_rand_float(sp_random_default_get()) * (_t%d.last - _t%d.first); })", tr, tr, tr);
        return 1;
      }
      if (a0t == TY_RANGE) {
        const char *atype = nt_type(nt, av[0]);
        int islit = atype && sp_streq(atype, "RangeNode");
        int lo = islit ? nt_ref(nt, av[0], "left") : -1;
        int hi = islit ? nt_ref(nt, av[0], "right") : -1;
        /* an endless/beginless range has no finite span -> Errno::EDOM (#2544) */
        if (islit && (lo < 0 || hi < 0)) {
          buf_puts(b, "(sp_raise_cls(\"Errno::EDOM\", \"Domain error - rand\"), (sp_int)0)");
          return 1;
        }
        int is_float = lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
        /* a statically empty/reversed int range -> nil (#2519) */
        if (islit && !is_float && lo >= 0 && hi >= 0 &&
            nt_type(nt, lo) && sp_streq(nt_type(nt, lo), "IntegerNode") &&
            nt_type(nt, hi) && sp_streq(nt_type(nt, hi), "IntegerNode")) {
          long long lov = nt_int(nt, lo, "value", 0);
          long long hiv = nt_int(nt, hi, "value", 0);
          int excl = (nt_int(nt, av[0], "flags", 0) & 4) ? 1 : 0;
          if ((excl ? hiv - 1 : hiv) < lov) { buf_puts(b, "sp_box_nil()"); return 1; }
        }
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, av[0], b); buf_puts(b, "; ");
        if (is_float)
          buf_printf(b, "(sp_float)_t%d.first + sp_Random_rand_float(sp_random_default_get()) * (sp_float)(_t%d.last - _t%d.first); })", tr, tr, tr);
        else if (islit)
          buf_printf(b, "_t%d.first + sp_Random_rand_int(sp_random_default_get(), _t%d.last - _t%d.first + 1 - _t%d.excl); })", tr, tr, tr, tr);
        else
          /* a range held in a variable can be empty at runtime -> nil, like CRuby;
             otherwise an Integer. The result is a poly (Integer or nil) (#3221). */
          buf_printf(b, "((_t%d.last - _t%d.excl) < _t%d.first) ? sp_box_nil() : sp_box_int(_t%d.first + sp_Random_rand_int(sp_random_default_get(), _t%d.last - _t%d.first + 1 - _t%d.excl)); })", tr, tr, tr, tr, tr, tr, tr);
        return 1;
      }
      /* rand(int): 0 behaves like rand() (a Float in [0,1)); a nonzero magnitude
         gives an Integer in [0, |n|) (#2518). A literal folds to the exact form.
         Not a literal past the TARGET's Integer, though: on a 32-bit build
         `rand(0x100000000)` is a Bignum bound, which the analyzer types
         TY_BIGINT, and folding it here emitted sp_krand_below of the saturated
         sp_int -- so `rand(0x100000000).to_s(36)` (CRuby's own Dir::Tmpname
         shape) fed an sp_int to sp_bigint_to_s_base and the C build stopped.
         Fall through to the Bignum arm below, which the 64-bit build already
         takes for a literal past int64. */
      if (nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "IntegerNode") &&
          comp_ntype(c, av[0]) != TY_BIGINT) {
        long long v = nt_int(nt, av[0], "value", 0);
        if (v == 0) { buf_puts(b, "sp_krand_float()"); return 1; }
        long long m = v < 0 ? -v : v;
        buf_printf(b, "sp_krand_below(%lldLL)", m);
        return 1;
      }
      /* A Float bound truncates to an integer (rand(3.5) draws over [0,3)), but
         a non-finite bound (Infinity/NaN) raises FloatDomainError rather than
         casting garbage to an integer (#3049); evaluate into a double first so
         the finiteness check precedes the narrowing cast. */
      if (comp_ntype(c, av[0]) == TY_FLOAT) {
        int tf = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tf); emit_float_expr(c, av[0], b);
        buf_printf(b, "; if (!isfinite(_t%d)) sp_raise_cls(\"FloatDomainError\","
                      " isnan(_t%d) ? \"NaN\" : \"Infinity\");", tf, tf);
        buf_printf(b, " sp_int _t%d = (sp_int)_t%d; if (_t%d < 0) _t%d = -_t%d;"
                      " _t%d > 0 ? sp_box_int(sp_krand_below(_t%d))"
                      " : sp_box_float(sp_krand_float()); })",
                   tn, tf, tn, tn, tn, tn, tn);
        return 1;
      }
      /* rand(Bignum bound): a uniform Bigint in [0, bound) off the shared
         default stream (#3058) */
      if (comp_ntype(c, av[0]) == TY_BIGINT) {
        buf_puts(b, "sp_bigint_rand(sp_random_default_get(), ");
        emit_expr(c, av[0], b); buf_puts(b, ")");
        return 1;
      }
      /* a dynamic Integer argument may be 0 at run time (a Float [0,1)) or
         nonzero (an Integer [0,|n|)), so the result is boxed and chosen at
         run time (#2549). */
      int tn = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tn);
      emit_int_expr(c, av[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d = -_t%d; _t%d > 0"
                    " ? sp_box_int(sp_krand_below(_t%d))"
                    " : sp_box_float(sp_krand_float()); })",
                 tn, tn, tn, tn, tn);
      return 1;
    }
    if (sp_streq(name, "srand")) {
      /* srand returns the PREVIOUS seed (#2517). */
      if (ac == 0) { buf_puts(b, "sp_kernel_srand((sp_int)time(NULL))"); return 1; }
      buf_puts(b, "sp_kernel_srand("); emit_int_expr_conv(c, av[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Kernel#gets reads the next line of ARGF, as `ARGF.gets` does; nil at
       end of input. Only the bare form (a separator, limit or `chomp:`
       would be dropped here), and only in the programs comp_bare_gets_is_argf
       admits: inference reads the same answer, so the two never disagree. */
    if (sp_streq(name, "gets") && ac == 0 && comp_bare_gets_is_argf(c)) {
      buf_puts(b, "sp_argf_gets()");
      return 1;
    }
  }

  /* exit / abort as expressions (noreturn, emit as C statement-expression) */
  /* sleep(seconds) / Kernel.sleep(seconds) / ::Kernel.sleep(seconds) */
  if (sp_streq(name, "sleep") && argc <= 1 && !bare_call_class_owned(c, id) &&
      (recv < 0 ||
       (nt_type(nt, recv) &&
        (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode")) &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Kernel")))) {
    /* a bare sleep, and sleep(nil), sleep until Thread#wakeup */
    if (argc == 0) { buf_puts(b, "(sp_sleep_forever(), (sp_int)0)"); return 1; }
    TyKind st = comp_ntype(c, argv[0]);
    if (st == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_sleep_forever(), (sp_int)0)");
      return 1;
    }
    buf_puts(b, "((void)sp_sleep(");
    if (st == TY_INT) { buf_puts(b, "(double)"); emit_expr(c, argv[0], b); }
    else if (st == TY_POLY) {   /* a boxed nil sleeps until #wakeup, as a literal nil does */
      int tn = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tn); emit_expr(c, argv[0], b);
      buf_printf(b, "; _t%d.tag == SP_TAG_NIL ? (sp_sleep_forever(), 0.0) : sp_poly_time_interval(_t%d); })", tn, tn);
    }
    else if (st == TY_BOOL) {
      buf_puts(b, "({ sp_raise_cls(\"TypeError\", (");
      emit_expr(c, argv[0], b);
      buf_puts(b, ") ? \"can't convert TrueClass into time interval\""
                  " : \"can't convert FalseClass into time interval\"); 0.0; })");
    }
    else if (st != TY_FLOAT && st != TY_BIGINT && conv_cls_name_of(c, st)) {
      /* rb_time_interval's wording, which names the class -- not the Float
         slot's ("can't convert String into time interval") */
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_printf(b, "); sp_raise_cls(\"TypeError\", \"can't convert %s into time interval\"); 0.0; })",
                 conv_cls_name_of(c, st));
    }
    else emit_float_expr(c, argv[0], b);
    buf_puts(b, "), (sp_int)0)");
    return 1;
  }
  /* A bare `exit` inside a class that defines its own `exit` reader/method is
     that member, not Kernel#exit (Ruby's implicit-self dispatch prefers the
     defined method) -- fall through to the normal resolution (#3207). */
  if (recv < 0 && (sp_streq(name, "exit") || sp_streq(name, "exit!")) && !bare_call_class_owned(c, id)) {
    Scope *xsc = comp_scope_of(c, id);
    int xcls = xsc ? xsc->class_id : -1;
    if (xcls >= 0 && (comp_reader_in_chain(c, xcls, name, NULL) ||
                      comp_method_in_chain(c, xcls, name, NULL) >= 0)) {
      /* not Kernel#exit: leave it to the reader/method dispatch below */
    }
    else {
    /* exit raises a rescuable SystemExit (#2761); exit! terminates directly.
       A boolean status maps true -> 0, false -> 1, as in CRuby. */
    const char *xfn = sp_streq(name, "exit!") ? "exit" : "sp_exit_raise";
    if (argc == 0) { buf_printf(b, "({ %s(0); (sp_int)0; })", xfn); return 1; }
    /* a poly status (e.g. a widened attr read or poly-hash get) must be
       unboxed -- (int)(sp_RbVal) is a struct cast, a cc error. */
    TyKind xt = comp_ntype(c, argv[0]);
    if (xt == TY_POLY) { buf_printf(b, "({ %s((int)sp_poly_arg_i(", xfn); emit_expr(c, argv[0], b); buf_puts(b, ")); (sp_int)0; })"); }
    else if (xt == TY_BOOL) { buf_printf(b, "({ %s((", xfn); emit_expr(c, argv[0], b); buf_puts(b, ") ? 0 : 1); (sp_int)0; })"); }
    else { buf_printf(b, "({ %s((int)(", xfn); emit_int_expr(c, argv[0], b); buf_puts(b, ")); (sp_int)0; })"); }
    return 1;
    }
  }
  if (recv < 0 && sp_streq(name, "abort") && !bare_call_class_owned(c, id)) {
    /* abort raises a rescuable SystemExit(1) after writing the message to
       stderr (#3077) */
    if (argc >= 1) {
      TyKind at2 = comp_ntype(c, argv[0]);
      buf_puts(b, "({ sp_abort_raise(");
      if (at2 == TY_STRING) emit_expr(c, argv[0], b);
      else emit_str_expr(c, argv[0], b);
      buf_puts(b, "); (sp_int)0; })");
    }
    else buf_puts(b, "({ sp_abort_raise((const char *)0); (sp_int)0; })");
    return 1;
  }

  /* Kernel#puts / #print as an expression: run the statement emitters inside
     a statement-expression and yield nil (their Ruby value). */
  if (recv < 0 && !bare_call_class_owned(c, id) && (sp_streq(name, "puts") || sp_streq(name, "print")) &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "({ ");
    if (argc == 0 && sp_streq(name, "puts")) buf_puts(b, "putchar('\n');\n");
    if (!emit_output_spilled(c, name, argc, argv, b, 0)) {
      for (int k = 0; k < argc; k++) {
        if (sp_streq(name, "puts")) emit_puts_one(c, argv[k], b, 0);
        else emit_print_one(c, argv[k], b, 0);
      }
    }
    buf_puts(b, " sp_box_nil(); })");
    return 1;
  }

  /* Kernel#p as an expression: print the argument's inspect, yield the
     argument as the value (statement position has its own emitter). The
     value is boxed once, printed through the poly inspect (which consults
     the user-object hook), and unboxed back to the static type. */
  /* p(a, b, ...) as a value: prints each argument's inspect, returns the
     argument array. */
  if (recv < 0 && !bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && argc >= 2 && nt_ref(nt, id, "block") < 0) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
    for (int k = 0; k < argc; k++) {
      buf_printf(b, "sp_PolyArray_push(_t%d, ", t);
      emit_boxed(c, argv[k], b);
      buf_puts(b, "); ");
    }
    buf_printf(b, "for (sp_int _i%d = 0; _i%d < _t%d->len; _i%d++) { "
                  "sp_puts_line(sp_poly_inspect(_t%d->data[_i%d])); } _t%d; })",
               t, t, t, t, t, t, t);
    return 1;
  }
  if (recv < 0 && !bare_call_class_owned(c, id) && (sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
    TyKind at = comp_ntype(c, argv[0]);
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_puts_line(sp_poly_inspect(_t%d)); ", t, t);
    char tv[16]; snprintf(tv, sizeof tv, "_t%d", t);
    emit_unbox_text(c, at, tv, b);
    buf_puts(b, "; })");
    return 1;
  }
  /* Kernel#warn / #printf / #putc / p() as an expression: run the statement
     emitter inside a statement-expression and yield the Ruby value (nil for
     warn/printf/p(), the argument for putc). */
  if (recv < 0 && !bare_call_class_owned(c, id) && (sp_streq(name, "warn") || sp_streq(name, "printf") ||
                   (sp_streq(name, "putc") && argc == 1) ||
                   ((sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 0)) &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "({ ");
    emit_output_call(c, id, b, 0);
    if (sp_streq(name, "putc") && argc == 1) emit_expr(c, argv[0], b);  /* putc returns its arg */
    else buf_puts(b, "sp_box_nil()");
    buf_puts(b, "; })");
    return 1;
  }
  return 0;
}

/* control flow and the process: caller, eval, caller_locations, loop, catch / throw, system, trap, Fiber storage reads, and ENV */
int emit_call_kernel_flow_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* loop { break val } as expression: emit pre-statement for-loop, result via break var */
  /* Kernel#caller / caller(start) / caller(start, len) -> the current stack
     (method-granularity, via sp_caller_now). Bare `caller` is `caller(1)`.
     Also accepts caller(0..n) / caller(0...n) -- a Range literal with
     integer endpoints -- and rewrites it to the (start, len) form. */
  if (recv < 0 && sp_streq(name, "caller") && argc <= 2 && !bare_call_class_owned(c, id)) {
    /* Range literal: caller(lo..hi) or caller(lo...hi) -> caller(lo, len). */
    if (argc == 1 && nt_type(c->nt, argv[0]) &&
        sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
      int rid = argv[0];
      int left = nt_ref(c->nt, rid, "left");
      int right = nt_ref(c->nt, rid, "right");
      int excl = (int)(nt_int(c->nt, rid, "flags", 0) & 4) ? 1 : 0;
      TyKind lty = left >= 0 ? comp_ntype(c, left) : TY_UNKNOWN;
      TyKind rty = right >= 0 ? comp_ntype(c, right) : TY_UNKNOWN;
      /* A boxed endpoint (a value out of a poly slot; under
         --int-overflow=promote every Integer local and method answer)
         is read through the checked unbox, which raises CRuby's TypeError
         for a non-Integer, where the gate refused the program (#4766). */
      if (left >= 0 && right >= 0 &&
          (lty == TY_INT || lty == TY_POLY) && (rty == TY_INT || rty == TY_POLY)) {
        /* Evaluate left and right into temps in source order so a
           side-effecting endpoint (e.g. caller(foo()..bar())) runs each
           call exactly once, left before right. */
        int lt = ++g_tmp, rt = ++g_tmp;
        Buf lb; memset(&lb, 0, sizeof lb); emit_int_expr(c, left, &lb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = %s;\n", lt, lb.p ? lb.p : "0");
        free(lb.p);
        Buf rb; memset(&rb, 0, sizeof rb); emit_int_expr(c, right, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = %s;\n", rt, rb.p ? rb.p : "0");
        free(rb.p);
        buf_printf(b, "sp_caller(_t%d, 1, (_t%d - _t%d%s))", lt, rt, lt,
                   excl ? "" : " + 1");
        return 1;
      }
      unsupported_feature(c, id,
        "caller(Range) only supports Range literals with integer endpoints "
        "(e.g. caller(0..5)); pass start and length instead: caller(start, len)");
    }
    /* Non-literal Range argument: compile error instead of a runtime
       crash with "no implicit conversion of Range into Integer". */
    if (argc >= 1 && comp_ntype(c, argv[0]) == TY_RANGE)
      unsupported_feature(c, id,
        "caller(Range) only supports Range literals with integer endpoints "
        "(e.g. caller(0..5)); pass start and length instead: caller(start, len)");
    buf_puts(b, "sp_caller(");
    if (argc >= 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
    if (argc == 2) { buf_puts(b, ", 1, "); emit_int_expr(c, argv[1], b); }
    else buf_puts(b, ", 0, 0");
    buf_puts(b, ")");
    return 1;
  }
  /* eval(string) / Kernel.eval(string): a hard AOT boundary (see helper). */
  if (diagnose_eval_call(c, id)) return 1;
  /* caller_locations: no runtime frame stack in AOT builds (as with `caller`),
     so this is an empty array of locations -- an Array, never nil. The (start,
     length) arguments are still evaluated for their side effects, as CRuby
     evaluates them before the call; the `(void)` casts keep a literal arg from
     tripping -Wunused-value. */
  if (recv < 0 && sp_streq(name, "caller_locations") && argc <= 2 && !bare_call_class_owned(c, id)) {
    buf_puts(b, "(");
    for (int ai = 0; ai < argc; ai++) { buf_puts(b, "(void)("); emit_expr(c, argv[ai], b); buf_puts(b, "), "); }
    buf_puts(b, "sp_PolyArray_new())");
    return 1;
  }
  if (recv < 0 && sp_streq(name, "loop") && argc == 0 && !bare_call_class_owned(c, id)) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = infer_type(c, id);
      /* a value-less `break` (or none at all) makes the loop's value nil:
         ride the poly slot so the nil default is the result */
      if (bt == TY_UNKNOWN || bt == TY_NIL) bt = TY_POLY;
      {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", t,
                   bt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, bt));
        /* rooted like the while form's: a `break <v>` stores it here and then
           runs the ensure bodies it leaves, which may allocate */
        if (bt == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t); }
        else if (needs_root(bt)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t); }
        /* Kernel#loop rescues StopIteration to terminate; wrap in a setjmp. */
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_check_depth();\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "for (;;) {\n");
        const char *sv_lb = g_loop_break_var;
        /* The frame this loop opened is live for its body: a `return` from
           inside must pop it (see the statement-form loop). The break base is
           taken AFTER the bump -- a `break` leaves through the code below the
           loop, which pops that frame itself. */
        int sv_lexc = g_loop_exc_base;
        g_exc_frame_depth++;
        g_loop_exc_base = g_exc_frame_depth;
        /* a C loop like the statement form's (emit_loop_body): a break or
           next crossing an ensure opened in the body runs it, then leaves */
        int sv_lens = g_loop_ensure_base; g_loop_ensure_base = g_ensure_depth;
        g_c_loop_depth++;
        int sv_iep = g_ie_res_poly;
        const char *sv_bj = g_brk_ser_var; g_brk_ser_var = NULL;  /* break here targets this loop */
        g_ie_res_poly = (bt == TY_POLY);   /* box a scalar `break <v>` into the poly slot */
        char lb_buf[32]; snprintf(lb_buf, sizeof lb_buf, "_t%d", t);
        g_loop_break_var = lb_buf;
        int lbody = nt_ref(nt, blk, "body");
        emit_stmts(c, lbody, g_pre, g_indent + 2);
        g_exc_frame_depth--;
        g_c_loop_depth--;
        g_loop_ensure_base = sv_lens;
        g_loop_break_var = sv_lb;
        g_loop_exc_base = sv_lexc;
        g_ie_res_poly = sv_iep;
        g_brk_ser_var = sv_bj;
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_exc_top--;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "else {\n");
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_exc_top--;\n");
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n");
        emit_indent(g_pre, g_indent + 1);
        buf_puts(g_pre, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) { sp_pending_exc_obj = sp_exc_obj[sp_exc_top]; sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]); }\n");
        /* the loop's value is the exhausted iteration's result (#3588) */
        if (bt == TY_POLY) {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (sp_exc_obj[sp_exc_top]) _t%d = ((sp_Exception *)sp_exc_obj[sp_exc_top])->result;\n", t);
        }
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", t);
        return 1;
      }
    }
    /* blockless `loop` is an infinite Enumerator yielding nil (#3236). The
       full `loop.with_index.<terminal>` chain over an infinite generator is not
       wired yet (it is unsupported for endless ranges too), but the bare
       Enumerator and its #next / #first / #take / #each work. */
    buf_puts(b, "sp_loop_enum()");
    return 1;
  }

  /* catch(:tag) { ... [throw :tag, val] ... } as expression: a setjmp scope
     whose value is the block's last expression, or the thrown value. */
  if (recv < 0 && sp_streq(name, "catch") && argc <= 1 && !bare_call_class_owned(c, id)) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = comp_ntype(c, id);
      /* NIL: a body whose tail is a break-less loop; ride the int slot (0). */
      if (bt == TY_UNKNOWN || bt == TY_VOID || bt == TY_NIL) bt = TY_INT;
      int ptr = proc_slot_is_ptr(bt);
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", t, default_value_from_compiler(c, bt));
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_catch_check_depth();\n");
      int tag_kind = 0;
      if (argc == 1) {
        Buf tgb; memset(&tgb, 0, sizeof tgb);
        tag_kind = emit_catch_tag(c, argv[0], &tgb);
        emit_indent(g_pre, g_indent);
        if (tag_kind < 0)   /* a boxed tag: its kind is the value's, decided at run time */
          buf_printf(g_pre, "sp_catch_tag[sp_catch_top] = sp_catch_tag_of(%s, &sp_catch_tag_kind[sp_catch_top]);\n", tgb.p ? tgb.p : "sp_box_nil()");
        else
          buf_printf(g_pre, "sp_catch_tag[sp_catch_top] = %s;\n", tgb.p ? tgb.p : "");
        free(tgb.p);
        /* the block takes the tag it was given (a literal, or a variable
           read again: neither runs anything) */
        const char *tp0 = block_param_name(c, blk, 0);
        NodeKind tk = nt_kind(nt, argv[0]);
        if (tp0 && (tk == NK_SymbolNode || tk == NK_StringNode || tk == NK_IntegerNode ||
                    tk == NK_LocalVariableReadNode)) {
          Scope *cbs = comp_scope_of(c, blk);
          LocalVar *clv = cbs ? scope_local(cbs, tp0) : NULL;
          if (clv && clv->type != TY_UNKNOWN) {
            Buf vb; memset(&vb, 0, sizeof vb);
            if (clv->type == TY_POLY) emit_boxed(c, argv[0], &vb); else emit_expr(c, argv[0], &vb);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "lv_%s = %s;\n", rename_local(tp0), vb.p ? vb.p : "");
            free(vb.p);
          }
        }
      }
      else {
        /* `catch { |tag| ... }`: mint a fresh, content-unique heap tag per
           entry (CRuby mints a new Object; a serial-unique name string gives
           the same only-this-invocation matching). Rooted for the body's
           duration -- the body can allocate. */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "const char *_ctag%d = sp_sprintf(\"#<catch:%%lld>\", (long long)++sp_catch_seq);\n", t);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_STR(_ctag%d);\n", t);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_catch_tag[sp_catch_top] = _ctag%d;\n", t);
        const char *bp0 = block_param_name(c, blk, 0);
        if (bp0) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "lv_%s = _ctag%d;\n", rename_local(bp0), t);
        }
      }
      if (tag_kind >= 0) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_catch_tag_kind[sp_catch_top] = %d;\n", tag_kind);
      }
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "sp_catch_val[sp_catch_top] = sp_box_nil();\n");
      /* record the exception-handler depth at this catch's entry so a `throw`
         can run intervening `ensure` blocks before delivering here. */
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_catch_exc_top[sp_catch_top] = sp_exc_top;\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_catch_rootmark[sp_catch_top] = sp_gc_nroots;\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_catch_top++;\n");
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "if (setjmp(sp_catch_stack[sp_catch_top-1]) == 0) {\n");
      /* a bare break in a catch body keeps today's C-break behavior */
      const char *sv_cser = g_brk_ser_var; g_brk_ser_var = NULL;
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], g_pre, g_indent + 1);
      if (bn > 0) {
        int last = bb[bn - 1];
        const char *lty = nt_type(nt, last);
        const char *lnm = (lty && sp_streq(lty, "CallNode")) ? nt_str(nt, last, "name") : NULL;
        int last_throw = (lnm && sp_streq(lnm, "throw") && nt_ref(nt, last, "receiver") < 0);
        TyKind lt = comp_ntype(c, last);
        /* TY_NIL includes a tail `loop { throw ... }` (a break-less loop
           infers nil): it produces no value to store, only effects. */
        if (last_throw || lt == TY_VOID || lt == TY_UNKNOWN || lt == TY_NIL) {
          emit_stmt(c, last, g_pre, g_indent + 1);
        }
        else {
          /* build the value into its own buffer first: emitting it straight
             into g_pre let a nested construct (another catch) append ITS
             statements in the middle of the assignment (#3706) */
          Buf cvb; memset(&cvb, 0, sizeof cvb);
          int sv_ind = g_indent; g_indent = g_indent + 1;
          if (bt == TY_POLY && lt != TY_POLY) emit_boxed(c, last, &cvb);
          else emit_expr(c, last, &cvb);
          g_indent = sv_ind;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "_t%d = %s;\n", t, cvb.p ? cvb.p : "0");
          free(cvb.p);
        }
      }
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_catch_top--;\n");
      g_brk_ser_var = sv_cser;
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "else {\n");
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_catch_top--;\n");
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_gc_nroots = sp_catch_rootmark[sp_catch_top];\n");
      emit_indent(g_pre, g_indent + 1);
      if (ptr) {
        buf_printf(g_pre, "_t%d = (", t); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, ")sp_catch_val[sp_catch_top].v.p;\n");
      }
      else if (bt == TY_POLY) {
        buf_printf(g_pre, "_t%d = sp_catch_val[sp_catch_top];\n", t);
      }
      else {
        buf_printf(g_pre, "_t%d = ", t);
        emit_unbox_text(c, bt, "sp_catch_val[sp_catch_top]", g_pre);
        buf_puts(g_pre, ";\n");
      }
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", t);
      return 1;
    }
  }

  /* throw :tag[, val] -> non-local jump to the matching catch scope. */
  if (recv < 0 && sp_streq(name, "throw") && !bare_call_class_owned(c, id)) {
    int tag_kind = 0;
    Buf tb; memset(&tb, 0, sizeof tb);
    if (argc >= 1) tag_kind = emit_catch_tag(c, argv[0], &tb);
    else buf_puts(&tb, "(&(\"\\xff\")[1])");
    if (tag_kind < 0) buf_printf(b, "sp_throw_boxed(%s, ", tb.p ? tb.p : "sp_box_nil()");
    else buf_printf(b, "sp_throw(%s, %d, ", tb.p ? tb.p : "", tag_kind);
    free(tb.p);
    if (argc >= 2) emit_boxed(c, argv[1], b);
    else buf_puts(b, "sp_box_nil()");
    buf_puts(b, ")");
    return 1;
  }

  /* system(cmd, ...) expr: run and return bool */
  if (recv < 0 && sp_streq(name, "system") && argc >= 1 && !bare_call_class_owned(c, id)) {
    system_refuse_unsupported(c, id, argv, argc);
    /* each argument converts and is rooted before the next converts: a
       #to_str's answer is a heap String the following conversion may collect */
    int ts = ++g_tmp;
    buf_puts(b, "({ ");
    for (int k = 0; k < argc; k++) {
      buf_printf(b, "const char *_sys_%d_%d = ", ts, k); emit_str_expr(c, argv[k], b);
      buf_printf(b, "; SP_GC_ROOT_STR(_sys_%d_%d); ", ts, k);
    }
    buf_printf(b, "const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); buf_printf(b, "_sys_%d_%d", ts, k); }
    buf_printf(b, ", NULL }; (sp_bool)sp_system_args(%d, _sys_%d); })", argc, ts);
    return 1;
  }
  /* trap(...) / Signal.trap(...): install the handler (a string command or a
     proc/block), validate the designator, and return the previous handler
     (#2736, #2737, #2749). */
  {
    int is_trap = (recv < 0 && sp_streq(name, "trap") && !bare_call_class_owned(c, id));
    if (!is_trap && recv >= 0 && sp_streq(name, "trap") && argc >= 1) {
      const char *rty2 = nt_type(nt, recv);
      if (rty2 && (sp_streq(rty2, "ConstantReadNode") || sp_streq(rty2, "ConstantPathNode"))) {
        const char *rn = nt_str(nt, recv, "name");
        if (rn && sp_streq(rn, "Signal")) is_trap = 1;
      }
    }
    if (is_trap && argc >= 1) {
      g_uses_symbols = 1;   /* :INT designators resolve through the sym table */
      buf_puts(b, "sp_signal_trap(");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ", ");
      if (nt_ref(nt, id, "block") >= 0) {
        buf_puts(b, "sp_box_proc(");
        emit_proc_literal(c, id, b);
        buf_puts(b, ")");
      }
      else if (argc >= 2) emit_boxed(c, argv[1], b);
      else buf_puts(b, "sp_box_str(\"DEFAULT\")");
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Fiber[:k] / Fiber.current[:k] -> sp_Fiber_storage_get */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    if (fiber_storage_recv(nt, recv)) {
      buf_puts(b, "sp_Fiber_storage_get(sp_fiber_current, ");
      emit_fiber_storage_key(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
  }
  /* ENV direct arms: identity, copy refusal, mutators, arity and key-type
     validation (#2743, #2746, #2747, #2765, #2766, #2773). */
  {
    int erecv = nt_ref(nt, id, "receiver");
    const char *erty = erecv >= 0 ? nt_type(nt, erecv) : NULL;
    const char *ern = (erty && sp_streq(erty, "ConstantReadNode")) ? nt_str(nt, erecv, "name") : NULL;
    if (ern && sp_streq(ern, "ENV")) {
      const char *enm = nt_str(nt, id, "name");
      int eac = 0; const int *eav = call_args(nt, id, &eac);
      /* a non-String key never converts: TypeError at the call (#2765/#2766) */
      if (enm && eac >= 1 &&
          (sp_streq(enm, "[]") || sp_streq(enm, "fetch") || sp_streq(enm, "key?") ||
           sp_streq(enm, "has_key?") || sp_streq(enm, "include?") || sp_streq(enm, "member?") ||
           sp_streq(enm, "delete") || sp_streq(enm, "store") || sp_streq(enm, "[]="))) {
        TyKind kt = comp_ntype(c, eav[0]);
        const char *kcn = kt == TY_SYMBOL ? "Symbol" : kt == TY_INT ? "Integer"
                        : kt == TY_FLOAT ? "Float" : kt == TY_BOOL ? "TrueClass"
                        : kt == TY_NIL ? "nil" : NULL;
        if (kcn) {
          buf_printf(b, "(sp_raise_cls(\"TypeError\","
                        " (&(\"\\xff\" \"no implicit conversion of %s into String\")[1])), %s)",
                     kcn, default_value_from_compiler(c, comp_ntype(c, id)));
          return 1;
        }
      }
      /* arity: the key predicates take exactly 1 (#2831) */
      if (enm && eac != 1 &&
          (sp_streq(enm, "key?") || sp_streq(enm, "has_key?") ||
           sp_streq(enm, "include?") || sp_streq(enm, "member?") || sp_streq(enm, "[]"))) {
        buf_puts(b, "(");
        for (int q = 0; q < eac; q++) { buf_puts(b, "(void)("); emit_expr(c, eav[q], b); buf_puts(b, "), "); }
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\","
                      " (&(\"\\xff\" \"wrong number of arguments (given %d, expected 1)\")[1])), %s)",
                   eac, default_value_from_compiler(c, comp_ntype(c, id)));
        buf_puts(b, ")");
        return 1;
      }
      /* arity: [] takes 1; fetch takes 1..2 (#2773) */
      if (enm && sp_streq(enm, "fetch") && (eac == 0 || eac > 2) && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "(");
        for (int q = 0; q < eac; q++) { buf_puts(b, "(void)("); emit_expr(c, eav[q], b); buf_puts(b, "), "); }
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\","
                      " (&(\"\\xff\" \"wrong number of arguments (given %d, expected 1..2)\")[1])), %s)",
                   eac, default_value_from_compiler(c, comp_ntype(c, id)));
        buf_puts(b, ")");
        return 1;
      }
      /* the remaining query/mutation surface (#2832) */
      if (enm && sp_streq(enm, "class") && eac == 0) {
        buf_puts(b, "((sp_Class){(sp_int)-116, SPL(\"Object\")})");   /* ENV is an Object singleton */
        return 1;
      }
      if (enm && sp_streq(enm, "frozen?") && eac == 0) {
        buf_puts(b, "((sp_bool)0)");
        return 1;
      }
      if (enm && sp_streq(enm, "clear") && eac == 0) {
        buf_puts(b, "sp_env_clear()");
        return 1;
      }
      if (enm && sp_streq(enm, "shift") && eac == 0) {
        buf_puts(b, "sp_env_shift()");
        return 1;
      }
      /* update/merge! with a conflict block: the block resolves keys already
         present in the environment (#2998) */
      if (enm && (sp_streq(enm, "update") || sp_streq(enm, "merge!")) && eac == 1 &&
          nt_ref(nt, id, "block") >= 0) {
        TyKind htb = comp_ntype(c, eav[0]);
        const char *htyb = nt_type(nt, eav[0]);
        if (htb == TY_STR_STR_HASH ||
            (htyb && (sp_streq(htyb, "HashNode") || sp_streq(htyb, "KeywordHashNode")))) {
          buf_puts(b, "sp_env_update_h_blk(");
          emit_expr(c, eav[0], b);
          buf_puts(b, ", ");
          emit_proc_literal(c, id, b);
          buf_puts(b, ")");
          return 1;
        }
      }
      if (enm && (sp_streq(enm, "update") || sp_streq(enm, "merge!") ||
                  sp_streq(enm, "replace")) && eac == 1 &&
          nt_ref(nt, id, "block") < 0) {
        TyKind ht2 = comp_ntype(c, eav[0]);
        if (ht2 == TY_STR_STR_HASH) {
          buf_printf(b, "sp_env_update_h(");
          emit_expr(c, eav[0], b);
          buf_printf(b, ", %d)", sp_streq(enm, "replace") ? 1 : 0);
          return 1;
        }
        /* a hash of any other variant (an empty `{}` too), or something a
           hash may be: boxed, so the run time checks each pair -- a nil value
           deletes, and a key or value that is not a String raises CRuby's
           TypeError */
        buf_printf(b, "sp_env_update_v(");
        emit_boxed(c, eav[0], b);
        buf_printf(b, ", %d)", sp_streq(enm, "replace") ? 1 : 0);
        return 1;
      }
      if (enm && eac == 0 && nt_ref(nt, id, "block") >= 0 &&
          is_select_bang(enm)) {
        int keep2 = sp_streq(enm, "keep_if") || sp_streq(enm, "select!") ||
                    sp_streq(enm, "filter!");
        /* the bang trio answers nil when nothing changed (#2844) */
        int optn = sp_streq(enm, "reject!") || sp_streq(enm, "select!") ||
                   sp_streq(enm, "filter!");
        buf_printf(b, "sp_env_filter_bang%s(", optn ? "_opt" : "");
        emit_proc_literal(c, id, b);
        buf_printf(b, ", %d)", keep2);
        return 1;
      }
      if (enm && sp_streq(enm, "to_s") && eac == 0) {
        buf_puts(b, "(&(\"\\xff\" \"ENV\")[1])");
        return 1;
      }
      if (enm && (sp_streq(enm, "dup") || sp_streq(enm, "clone")) && eac == 0) {
        buf_printf(b, "(sp_raise_cls(\"TypeError\","
                      " (&(\"\\xff\" \"Cannot %s ENV, use ENV.to_h to get a copy of ENV as a hash\")[1])), sp_box_nil())",
                   enm);
        return 1;
      }
      if (enm && sp_streq(enm, "freeze") && eac == 0) {
        buf_puts(b, "(sp_raise_cls(\"TypeError\", (&(\"\\xff\" \"cannot freeze ENV\")[1])), sp_box_nil())");
        return 1;
      }
      /* ENV.delete(k): the removed value (or nil); with a block, a missing key
         yields the block's value instead of nil (#2999) */
      if (enm && sp_streq(enm, "delete") && eac == 1) {
        int t1 = ++g_tmp, t2 = ++g_tmp;
        int dblk = nt_ref(nt, id, "block");
        buf_printf(b, "({ const char *_t%d = ", t1); emit_str_expr(c, eav[0], b);
        buf_printf(b, "; const char *_t%d = getenv(_t%d);"
                      " _t%d = _t%d ? sp_str_dup_external(_t%d) : NULL;"
                      " unsetenv(_t%d); ", t2, t1, t2, t2, t2, t1);
        if (dblk >= 0) {
          const char *dp0 = block_param_name(c, dblk, 0);
          int dbody = nt_ref(nt, dblk, "body");
          int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
          int dval = dbn > 0 ? dbb[dbn - 1] : -1;
          buf_printf(b, "if (!_t%d) { ", t2);
          if (dp0) {
            /* shadow-declare the block param in this stmt-expr scope with its
               analyzed type (it may not be a declared local here) */
            LocalVar *dlv = scope_local(comp_scope_of(c, id), dp0);
            TyKind dpt = dlv ? dlv->type : TY_STRING;
            if (dpt == TY_POLY)
              buf_printf(b, "sp_RbVal lv_%s = sp_box_str(_t%d); (void)lv_%s; ",
                         rename_local(dp0), t1, rename_local(dp0));
            else
              buf_printf(b, "const char *lv_%s = _t%d; (void)lv_%s; ",
                         rename_local(dp0), t1, rename_local(dp0));
          }
          for (int k = 0; k < dbn - 1; k++) emit_stmt(c, dbb[k], b, 0);
          if (dval >= 0) {
            TyKind dvt = comp_ntype(c, dval);
            buf_printf(b, "_t%d = ", t2);
            if (dvt == TY_STRING) emit_expr(c, dval, b);
            else if (dvt == TY_POLY) {
              buf_puts(b, "({ sp_RbVal _dv = "); emit_expr(c, dval, b);
              buf_puts(b, "; _dv.tag == SP_TAG_NIL ? NULL : sp_poly_to_s(_dv); })");
            }
            else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, dval, b); buf_puts(b, ")"); }
            buf_puts(b, "; ");
          }
          buf_puts(b, "} ");
        }
        buf_printf(b, "_t%d; })", t2);
        return 1;
      }
      /* ENV.store(k, v) is []= */
      if (enm && sp_streq(enm, "store") && eac == 2) {
        buf_puts(b, "sp_env_aset(");
        emit_str_expr(c, eav[0], b); buf_puts(b, ", ");
        emit_boxed(c, eav[1], b); buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* ENV[key] -> getenv */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        buf_puts(b, "sp_str_dup_external(getenv("); emit_str_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
    }
  }
  /* ENV[key] = value -> setenv (value nil unsets, like CRuby) */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        TyKind evt = comp_ntype(c, argv[1]);
        const char *v1ty = nt_type(nt, argv[1]);
        int lit_nil = v1ty && sp_streq(v1ty, "NilNode");
        if (evt == TY_STRING || lit_nil) {
          int tk = ++g_tmp, tv = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", tk); emit_str_expr(c, argv[0], b);
          /* a nil VALUE unsets the variable; only the key is strict */
          buf_printf(b, "; const char *_t%d = ", tv); emit_str_expr_nilable(c, argv[1], b);
          buf_printf(b, "; if (_t%d) setenv(_t%d, _t%d, 1); else unsetenv(_t%d); _t%d; })",
                     tv, tk, tv, tk, tv);
        }
        else {
          /* runtime-typed RHS: nil deletes, a String sets, anything else
             raises CRuby's TypeError (naming the actual class) */
          buf_puts(b, "sp_env_aset(");
          emit_str_expr(c, argv[0], b);
          buf_puts(b, ", ");
          emit_boxed(c, argv[1], b);
          buf_puts(b, ")");
        }
        return 1;
      }
    }
  }
  /* ENV.size/count/length -> environ entry count */
  if (recv >= 0 && argc == 0 &&
      is_count_alias(name)) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        buf_puts(b, "sp_env_size()");
        return 1;
      }
    }
  }
  /* ENV.key?/has_key?/include?/member?(key) -> getenv presence test */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "key?") || sp_streq(name, "has_key?") ||
       sp_streq(name, "include?") || sp_streq(name, "member?"))) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        buf_puts(b, "(getenv("); emit_str_expr(c, argv[0], b); buf_puts(b, ") != NULL)");
        return 1;
      }
    }
  }
  /* ENV.fetch(key, default) -> getenv with fallback */
  if (recv >= 0 && sp_streq(name, "fetch") && argc >= 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        /* The call's type is String joined with the default's: a String or
           nil default keeps the nullable string, any other default boxes both
           arms. (The block form is the ENV snapshot's Hash#fetch, #2742.) */
        int fpoly = comp_ntype(c, id) == TY_POLY;
        int tk = ++g_tmp, tky = ++g_tmp, tv = ++g_tmp, td = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tky); emit_str_expr(c, argv[0], b);
        /* the default is an argument: it evaluates whether or not the
           variable is set, before the lookup */
        if (argc >= 2) {
          buf_puts(b, "; "); emit_ctype(c, fpoly ? TY_POLY : TY_STRING, b);
          buf_printf(b, " _t%d = ", td);
          if (fpoly) emit_boxed(c, argv[1], b);
          else emit_expr(c, argv[1], b);
        }
        buf_printf(b, "; const char *_t%d = getenv(_t%d); ", tk, tky);
        emit_ctype(c, fpoly ? TY_POLY : TY_STRING, b);
        buf_printf(b, " _t%d = _t%d ? ", tv, tk);
        if (fpoly) buf_printf(b, "sp_box_str(sp_str_dup_external(_t%d)) : ", tk);
        else buf_printf(b, "sp_str_dup_external(_t%d) : ", tk);
        if (argc >= 2) buf_printf(b, "_t%d", td);
        else
          /* no default: CRuby raises KeyError naming the key. Route it through
             sp_raise_key_not_found so the key is staged for #key (#3027). */
          buf_printf(b, "(sp_raise_key_not_found(sp_box_str(_t%d)), (const char *)0)", tky);
        buf_printf(b, "; _t%d; })", tv);
        return 1;
      }
    }
  }
  return 0;
}
