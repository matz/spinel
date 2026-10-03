/* codegen_call_object.c -- emit_call_body's arms of the Object protocol every value answers (identity, hash, nil?, ===, freeze / dup, instance_eval).
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "repr.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* between?, object_id / __id__, hash, nil? and === on a receiver whose kind decides them */
int emit_call_identity_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* between?(lo, hi): lo <= self <= hi */
  if (sp_streq(name, "between?") && argc == 2) {
    if (rt == TY_STRING &&
        (ty_is_object(comp_ntype(c, argv[0])) || ty_is_object(comp_ntype(c, argv[1])))) {
      /* Comparable#between? is two <=>s and it STOPS at the first: when
         `self >= lo` is false CRuby never asks hi for anything, so
         `"abc".between?(W.new("abd"), Plain.new)` is false rather than the
         second bound's comparison error. The bounds are still evaluated, once
         each, in order -- Ruby evaluates the arguments before between? runs
         -- so each is spilled to a temp up front, and only the CONVERSIONS
         and the refusals are left inside the `&&`, where the compare that
         reads them is. That is also why the conversions are not put in the
         call's conversion hold, which would hoist both in front of the `&&`.

         A String bound spills to a rooted `const char *` and compares as it
         always did -- rooted because the OTHER bound's #to_str allocates
         before this one is read; every other bound spills boxed, which is
         both what the conversion reads and what sp_cmperr_desc names in the
         message. A bound whose class answers no usable #to_str converts to
         NULL, so its half of the `&&` IS the comparison error #4265 gives; a
         BOXED bound asks the runtime's rb_check_string_type instead, so one
         holding a String or a converting object compares here exactly as it
         would as a boxed operand anywhere else. */
      int tv = ++g_tmp, tb[2], tc[2], isstr[2];
      for (int i = 0; i < 2; i++) { tb[i] = ++g_tmp; tc[i] = ++g_tmp; }
      buf_printf(b, "({ const char *_t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_STR(_t%d); ", tv);
      for (int i = 0; i < 2; i++) {
        isstr[i] = comp_ntype(c, argv[i]) == TY_STRING;
        if (isstr[i]) {
          buf_printf(b, "const char *_t%d = ", tb[i]); emit_expr(c, argv[i], b);
          buf_printf(b, "; SP_GC_ROOT_STR(_t%d); ", tb[i]);
        }
        else {
          buf_printf(b, "sp_RbVal _t%d = ", tb[i]); emit_boxed(c, argv[i], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tb[i]);
        }
      }
      buf_puts(b, "(");
      for (int i = 0; i < 2; i++) {
        const char *op = i == 0 ? ">=" : "<=";
        if (i) buf_puts(b, " && ");
        if (isstr[i]) { buf_printf(b, "sp_str_cmp_bytes(_t%d, _t%d) %s 0", tv, tb[i], op); continue; }
        buf_printf(b, "({ const char *_t%d = ", tc[i]);
        emit_str_cmp_conv(c, argv[i], tb[i], b);
        buf_printf(b, "; _t%d ? sp_str_cmp_bytes(_t%d, _t%d) %s 0"
                      " : (sp_raise_cls(\"ArgumentError\", sp_sprintf("
                      "\"comparison of String with %%s failed\","
                      " sp_cmperr_desc(_t%d))), FALSE); })",
                   tc[i], tv, tc[i], op, tb[i]);
      }
      buf_puts(b, "); })");
      return 1;
    }
    if (rt == TY_STRING) {
      int tv = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (sp_str_cmp_bytes(_t%d, ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, ") >= 0 && sp_str_cmp_bytes(_t%d, ", tv); emit_expr(c, argv[1], b); buf_puts(b, ") <= 0); })");
      return 1;
    }
    /* A Bignum receiver: sp_Bigint* can't be compared with >=/<= (that is a
       pointer comparison); route through sp_bigint_cmp, coercing an int bound
       to a Bignum first (#2863). */
    if (rt == TY_BIGINT) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (sp_bigint_cmp(_t%d, ", tv);
      if (comp_ntype(c, argv[0]) == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ") >= 0 && sp_bigint_cmp(_t%d, ", tv);
      if (comp_ntype(c, argv[1]) == TY_BIGINT) emit_expr(c, argv[1], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      buf_puts(b, ") <= 0); })");
      return 1;
    }
    /* An Integer receiver with a Bignum bound (`50.between?(1, 2 ** 100)`):
       comparing sp_int against sp_Bigint* with >=/<= is a pointer comparison.
       Coerce the receiver and each bound to Bignum and route through
       sp_bigint_cmp. (#2893) */
    if (rt == TY_INT &&
        (comp_ntype(c, argv[0]) == TY_BIGINT || comp_ntype(c, argv[1]) == TY_BIGINT)) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = sp_bigint_new_int(", tv); emit_int_expr(c, recv, b);
      buf_printf(b, "); (sp_bigint_cmp(_t%d, ", tv);
      if (comp_ntype(c, argv[0]) == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ") >= 0 && sp_bigint_cmp(_t%d, ", tv);
      if (comp_ntype(c, argv[1]) == TY_BIGINT) emit_expr(c, argv[1], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      buf_puts(b, ") <= 0); })");
      return 1;
    }
    /* An Integer/Float receiver with a Rational bound: `>=`/`<=` against an
       sp_Rational struct will not compile. Box both sides and compare through
       sp_poly_cmp_ck, which orders Rational against Integer/Float exactly (#3232). */
    if ((rt == TY_INT || rt == TY_FLOAT) &&
        (comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
      int ts = hoist_boxed_rooted(c, recv);
      int tlo = hoist_boxed_rooted(c, argv[0]), thi = hoist_boxed_rooted(c, argv[1]);
      buf_printf(b, "(sp_poly_cmp_ck(_t%d, _t%d) >= 0 && sp_poly_cmp_ck(_t%d, _t%d) <= 0)",
                 ts, tlo, ts, thi);
      return 1;
    }
    if (ty_is_numeric(rt)) {
      int tv = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tv); emit_expr(c, recv, b);
      /* a receiver that can hold the nil sentinel has no between? (#4567) */
      if (rt == TY_INT && cmp_operand_may_be_nil(c, recv))
        buf_printf(b, "; if (SP_UNLIKELY(_t%d == SP_INT_NIL)) sp_nil_recv(\"between?\")", tv);
      else if (rt == TY_FLOAT && cmp_operand_may_be_nil(c, recv))
        buf_printf(b, "; if (SP_UNLIKELY(sp_float_is_nil(_t%d))) sp_nil_recv(\"between?\")", tv);
      buf_printf(b, "; (_t%d >= ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, " && _t%d <= ", tv); emit_expr(c, argv[1], b); buf_puts(b, "); })");
      return 1;
    }
    /* Comparable#between? on a poly receiver (a user object read from a
       container / block param): compare through the boxed <=> hook, which
       raises the incomparable ArgumentError like CRuby. clamp already takes
       this path; between? was missing it (#3170). */
    if (rt == TY_POLY) {
      int ts = hoist_boxed_rooted(c, recv);
      int tlo = hoist_boxed_rooted(c, argv[0]), thi = hoist_boxed_rooted(c, argv[1]);
      /* nil has no #between?, and the checked comparison below would call it
         an incomparable pair instead -- the Comparable ArgumentError, which
         belongs to the BOUND being incomparable, not to a missing method.
         Only this site knows the method's name. */
      buf_printf(b, "(sp_poly_recv_ck(_t%d, \"%s\"), "
                    "sp_poly_cmp_ck(_t%d, _t%d) >= 0 && sp_poly_cmp_ck(_t%d, _t%d) <= 0)",
                 ts, name, ts, tlo, ts, thi);
      return 1;
    }
    /* Comparable: user type with <=> method */
    if (ty_is_object(rt)) {
      int cid_b = ty_object_class(rt);
      int defcls_b = -1;
      int mi_b = comp_method_in_chain(c, cid_b, "<=>", &defcls_b);
      if (mi_b >= 0 && user_cmp_invalid_ret(c, cid_b) != TY_UNKNOWN)
        unsupported_feature(c, id, "Comparable operator on an object whose #<=> returns a non-Integer (protocol requires Integer or nil)");
      if (mi_b >= 0 && user_cmp_needs_check(c, cid_b)) {
        /* nil-capable `<=>`: checked comparisons (incomparable raises) */
        int ts = hoist_boxed_rooted(c, recv);
        int tlo = hoist_boxed_rooted(c, argv[0]), thi = hoist_boxed_rooted(c, argv[1]);
        buf_printf(b, "(sp_poly_cmp_ck(_t%d, _t%d) >= 0 && sp_poly_cmp_ck(_t%d, _t%d) <= 0)",
                   ts, tlo, ts, thi);
        return 1;
      }
      if (mi_b >= 0) {
        int ts = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        const char *cname = c->classes[defcls_b].name;
        /* the `<=>` param may have widened to poly (sp_obj_clamp and the
           checked-comparison helpers call it through the boxed ABI): match
           the signature by boxing the bound temps when it did */
        Scope *cmp_sc = &c->scopes[mi_b];
        LocalVar *cp0 = cmp_sc->nparams > 0 && cmp_sc->pnames[0]
                        ? scope_local(cmp_sc, cmp_sc->pnames[0]) : NULL;
        int arg_poly = cp0 && cp0->type == TY_POLY;
        /* Compute each RHS into a local buffer first: emit_expr may itself
           hoist temps into g_pre (e.g. an arg `Temp.new(5)` roots its boxed
           int there). Doing that before writing our own `T _tN = ` prefix
           keeps the nested hoist from splitting our declaration line.
           Each temp is rooted as it is bound: the bounds are evaluated after
           the receiver and the second after the first, and any of the three
           may be a fresh object whose only reference is its temp. */
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        emit_pre_root(c, rt, ts);
        Buf lb; memset(&lb, 0, sizeof lb);
        if (arg_poly) emit_boxed(c, argv[0], &lb); else { Buf t = expr_buf(c, argv[0]); lb = t; }
        emit_indent(g_pre, g_indent);
        if (arg_poly) buf_puts(g_pre, "sp_RbVal"); else emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", tlo);
        buf_puts(g_pre, lb.p ? lb.p : ""); buf_puts(g_pre, ";\n"); free(lb.p);
        emit_pre_root(c, arg_poly ? TY_POLY : rt, tlo);
        Buf hb; memset(&hb, 0, sizeof hb);
        if (arg_poly) emit_boxed(c, argv[1], &hb); else { Buf t = expr_buf(c, argv[1]); hb = t; }
        emit_indent(g_pre, g_indent);
        if (arg_poly) buf_puts(g_pre, "sp_RbVal"); else emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", thi);
        buf_puts(g_pre, hb.p ? hb.p : ""); buf_puts(g_pre, ";\n"); free(hb.p);
        emit_pre_root(c, arg_poly ? TY_POLY : rt, thi);
        buf_printf(b, "(sp_%s_%s((sp_%s *)_t%d, _t%d) >= 0 && sp_%s_%s((sp_%s *)_t%d, _t%d) <= 0)",
                   cname, mc("<=>"), cname, ts, tlo,
                   cname, mc("<=>"), cname, ts, thi);
        return 1;
      }
    }
  }

  /* object_id: a stable integer id. Int uses MRI's 2n+1; pointer-backed
     values use the pointer bit pattern; a symbol uses its interned id.
     The immediates have fixed ids: nil is 4, false is 0, true is 20. */
  if ((sp_streq(name, "object_id") || sp_streq(name, "__id__")) && recv >= 0 && argc == 0 &&
      /* a generated READER of this name owns it on a concrete object, as in
         CRuby (which warns and defines it): fall through to the member read,
         which the inference already typed (#4190) */
      !(ty_is_object(rt) &&
        comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) == SP_MEMBER_ATTR)) {
    /* a nullable Integer or Float holding its sentinel is nil, whose id
       is nil's */
    if ((rt == TY_INT || rt == TY_FLOAT) && call_returns_nullable_int(c, recv)) {
      char ref[24];
      buf_puts(b, "({ "); emit_sentinel_bind(c, rt, recv, ref, sizeof ref, b);
      emit_slot_truthy(rt, ref, b);
      if (rt == TY_INT) buf_printf(b, " ? 2*%s+1 : 4; })", ref);
      else buf_printf(b, " ? sp_rbval_hash_key(sp_box_float(%s)) : 4; })", ref);
    }
    else if (rt == TY_INT) { buf_puts(b, "(2*("); emit_expr(c, recv, b); buf_puts(b, ")+1)"); }
    else if (rt == TY_SYMBOL) { buf_puts(b, "((sp_int)("); emit_expr(c, recv, b); buf_puts(b, ")*2)"); }
    else if (rt == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 4)"); }
    else if (rt == TY_BOOL) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? 20 : 0)"); }
    /* a boxed value: its identity is the boxed payload (heap pointer / int) */
    else if (rt == TY_POLY) { buf_puts(b, "((sp_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, ").v.p)"); }
    /* a mutable String held as its shared sp_String: that handle is the
       identity, and the one a box of it carries; its text is a fresh copy
       on every read */
    else if (rt == TY_STRING && strbuf_object_ref(c, recv, b)) { }
    /* unboxed value structs have no identity: derive a stable Integer from
       the value hash (see the identity note in docs/limitations.md) */
    else if (rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_RANGE ||
             rt == TY_TIME || rt == TY_FLOAT) {
      buf_puts(b, "sp_rbval_hash_key("); emit_boxed(c, recv, b); buf_puts(b, ")");
    }
    /* a value-type user object is a by-value struct with no pointer identity;
       hash its bytes for a stable id (`o.object_id == o.object_id`) (#2283) */
    else if (ty_is_object(rt) && comp_ty_value_obj(c, rt)) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%s _t%d = ", c->classes[ty_object_class(rt)].c_name, t);
      emit_expr(c, recv, b);
      buf_printf(b, "; sp_bytes_hash(&_t%d, sizeof _t%d); })", t, t);
    }
    else { buf_puts(b, "((sp_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    return 1;
  }

  /* #hash: box the receiver and run it through sp_rbval_hash_key -- the same
     hashing the Hash container uses to bucket keys, so `h[k]` and `k.hash`
     agree. A boxed user object routes through sp_obj_hash_hook, which dispatches
     to a user-defined #hash (or pointer identity as Object#hash's default). */
  /* #hash on a value-type user object with no user #hash: a by-value struct has
     no pointer identity, so hash its bytes for a stable, content-based id that
     agrees across calls (#2284). */
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 && ty_is_object(rt) &&
      comp_ty_value_obj(c, rt) && comp_method_in_chain(c, ty_object_class(rt), "hash", NULL) < 0 &&
      !c->classes[ty_object_class(rt)].is_struct) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_%s _t%d = ", c->classes[ty_object_class(rt)].c_name, t);
    emit_expr(c, recv, b);
    buf_printf(b, "; sp_bytes_hash(&_t%d, sizeof _t%d); })", t, t);
    return 1;
  }
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 &&
      rt != TY_MATCHDATA &&   /* MatchData has a dedicated content hash (#3014) */
      (!ty_is_object(rt) ||
       (comp_method_in_chain(c, ty_object_class(rt), "hash", NULL) < 0 &&
        /* structs keep their dedicated value-based hash arm below */
        !c->classes[ty_object_class(rt)].is_struct))) {
    buf_puts(b, "sp_rbval_hash_key("); emit_boxed(c, recv, b); buf_puts(b, ")");
    return 1;
  }

  /* nil? on an integer: a nullable int carries the SP_INT_NIL sentinel
     (e.g. an int-valued hash miss). A plain int is never the sentinel, so
     `5.nil?` constant-folds to false; a missing-key value reads true. */
  if (recv >= 0 && rt == TY_INT && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)");
    return 1;
  }
  /* nil? on a string: a nullable string carries NULL (e.g. a scan miss);
     a stage-2 builtin-op row (builtin_ops.c) */
  if (recv >= 0 && rt == TY_STRING && emit_builtin_op_stage(c, id, recv, rt, name, 2, b)) return 1;
  /* nil? on a float: a nullable float carries the NaN sentinel (e.g. first/
     last of an empty float array). A real float is never the sentinel. */
  if (recv >= 0 && rt == TY_FLOAT && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* nil? on an array/hash: a nil container is a NULL pointer */
  if (recv >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return 1;
  }
  /* nil? on a pointer-backed concrete type: nil is the NULL pointer. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      (rt == TY_FIBER || rt == TY_PROC || rt == TY_CURRY || rt == TY_RANDOM ||
       rt == TY_METHOD || rt == TY_IO ||
       rt == TY_MATCHDATA || rt == TY_REGEX || rt == TY_EXCEPTION || rt == TY_BIGINT)) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return 1;
  }
  /* nil? on a symbol: a nilable symbol slot -- an ivar no write has reached,
     a symbol-valued miss -- carries the (sp_sym)-1 sentinel, which a real
     symbol never is. It folded to false with the value types below, so an
     unset `@sym` read as set (a class_attribute's instance reader). */
  if (recv >= 0 && rt == TY_SYMBOL && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (sp_sym)-1)");
    return 1;
  }
  /* nil? on a value-typed concrete receiver is always false. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      (rt == TY_RANGE || rt == TY_TIME || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_BOOL || rt == TY_CLASS)) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
    return 1;
  }
  /* a predicate on an empty array literal folds to a constant: the block (if
     any) never runs, so empty all?/none? are true, any?/one? false */
  if (recv >= 0 && (argc == 0 || argc == 1) &&
      is_quantifier_or_count(name) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    if (sp_streq(name, "count")) { buf_puts(b, "0"); return 1; }
    buf_puts(b, (sp_streq(name, "all?") || sp_streq(name, "none?")) ? "1" : "0");
    return 1;
  }
  /* nil?/empty? on an empty array literal fold to constants (an Array is never
     nil; an empty one is empty). The `[]` literal has no side effects. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "empty?")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    buf_puts(b, sp_streq(name, "empty?") ? "1" : "0");
    return 1;
  }
  /* nil?/frozen?/empty?/is_a?(Hash) on an empty hash literal fold to constants
     (a Hash is never nil; a bare {} is unfrozen and empty). */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")) &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    if (argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "frozen?") || sp_streq(name, "any?"))) { buf_puts(b, "0"); return 1; }
    if (argc == 0 && sp_streq(name, "empty?")) { buf_puts(b, "1"); return 1; }
    if (argc == 0 && sp_streq(name, "to_h") && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_StrPolyHash_new()"); return 1;   /* {}.to_h == {} (#2410) */
    }
    if (argc <= 1 && sp_streq(name, "sum") && nt_ref(nt, id, "block") < 0) {
      /* {}.sum == the init (or 0) (#2416). A nil or Boolean init has no scalar
         slot to be answered in and the call is typed poly for it, so the init
         has to be boxed there -- emitted raw, `{}.sum(nil)` answered 0. */
      TyKind hst = comp_ntype(c, id);
      if (argc == 0) buf_puts(b, hst == TY_POLY ? "sp_box_int(0)" : "0");
      else if (hst == TY_POLY) emit_boxed(c, argv[0], b);
      else emit_expr(c, argv[0], b);
      return 1;
    }
    if (argc == 0 && (sp_streq(name, "min") || sp_streq(name, "max"))) {
      buf_puts(b, "sp_box_nil()"); return 1;   /* empty: nil (#2406) */
    }
    if (argc == 0 && sp_streq(name, "minmax")) {
      int t2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_nil()); sp_PolyArray_push(_t%d, sp_box_nil()); _t%d; })",
                 t2, t2, t2, t2, t2);
      return 1;
    }
    if (argc == 1 && is_kind_query(name)) {
      const char *cn = isa_const_name(nt, argv[0]);
      buf_puts(b, cn && sp_streq(cn, "Hash") ? "1" : "0");
      return 1;
    }
  }

  /* Class.===(obj): equivalent to obj.is_a?(Class). Receiver is a class
     constant, either a bare `Integer` or a top-level `::Integer` (a
     ConstantPathNode with no parent), both naming the class in "name". (#2889) */
  if (recv >= 0 && argc == 1 && sp_streq(name, "===") && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
       (sp_streq(nt_type(nt, recv), "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0) ||
       /* a parented path is this same static dispatch only when its FULL
          qualified name is a known builtin (exception) class -- Errno::ENOENT
          === e. Typing alone is not enough: the leaf fallback types
          Math::String as a class by its leaf, and claiming it here would
          answer String === instead of the NameError the qualified constant
          deserves. */
       (sp_streq(nt_type(nt, recv), "ConstantPathNode") && ({
          char _prq[192];
          const char *_prn = isa_const_qualname(nt, recv, _prq, sizeof _prq);
          _prn && (builtin_class_id(_prn) != 0 || is_builtin_exception_name(_prn)); })))) {
    char rq[192];
    const char *cn = isa_match_name(nt, recv, rq, sizeof rq);
    if (cn) {
      TyKind at2 = comp_ntype(c, argv[0]);
      /* An Integer or Float argument is nil where it holds its sentinel,
         which the scalar type cannot say: `Integer === x` answered true and
         `NilClass === x` false for a nil x (emit_scalar_class_test). */
      if (emit_scalar_class_test(c, argv[0], at2, cn, 0, b)) return 1;
      /* TrueClass/FalseClass/NilClass === <literal/typed value>: decide
         statically from the arg's node kind or scalar type. */
      const char *aty = nt_type(nt, argv[0]);
      if (sp_streq(cn, "NilClass") || sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass")) {
        int yn = -1;
        /* an object slot holds nil as NULL: asked at run time below */
        if (sp_streq(cn, "NilClass"))
          yn = (at2 == TY_NIL || (aty && sp_streq(aty, "NilNode"))) ? 1 :
               (at2 != TY_POLY && !ty_is_object(at2) ? 0 : -1);
        else if (sp_streq(cn, "TrueClass"))
          yn = (aty && sp_streq(aty, "TrueNode")) ? 1 : (aty && sp_streq(aty, "FalseNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
        else
          yn = (aty && sp_streq(aty, "FalseNode")) ? 1 : (aty && sp_streq(aty, "TrueNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
        if (yn >= 0) { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", yn); return 1; }
      }
      /* an exception instance matches by name up its class chain (#2759) */
      if (at2 == TY_EXCEPTION && (is_exc_name(cn) || is_builtin_class_name(cn))) {
        buf_puts(b, "sp_exc_is_a("); emit_expr(c, argv[0], b);
        buf_printf(b, ", \"%s\")", cn);
        return 1;
      }
      int yes = ty_matches_class(at2, cn, 0);
      if (yes >= 0) {
        buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", yes);
        return 1;
      }
      /* a user object: its class may descend from this builtin (a Struct or
         Data class, a subclass of Array), which only the class chain says */
      if (ty_is_object(at2)) {
        buf_puts(b, "sp_poly_is_a("); emit_boxed(c, argv[0], b); buf_puts(b, ", ");
        emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* arg type is poly or unknown: runtime tag check */
      if (at2 == TY_POLY) {
        int tv = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, argv[0], b); buf_printf(b, "; ");
        char v[32]; snprintf(v, sizeof v, "_t%d", tv);
        emit_poly_isa_test(c, cn, v, 0, b);
        buf_puts(b, "; })");
        return 1;
      }
    }
  }
  return 0;
}

/* instance_eval / instance_exec with a block, run with the receiver as self (direct, or through a trampoline) */
int emit_call_instance_eval_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc) {
  int ie_direct = recv >= 0 && (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec"));
  /* a nested block's ivars are its own receiver's */
  if (ie_direct && g_ie_nil_ivars && g_ie_nil_ivars != id + 1) {
    int sv_nil = g_ie_nil_ivars; g_ie_nil_ivars = 0;
    emit_call(c, id, b);
    g_ie_nil_ivars = sv_nil;
    return 1;
  }
  if (ie_direct && g_ie_poly_node != id && comp_ntype(c, recv) == TY_POLY && emit_ie_poly(c, id, b))
    return 1;
  /* instance_eval/exec on a non-object receiver (nil, a scalar): the block runs
     with self = the receiver. The object splice below needs a cls_id/ivar
     layout, so handle the non-object case directly here -- bind the params
     (exec: the call args; eval: the receiver) and emit the body, self boxed
     (a self-using body dispatches through the poly runtime). (#2956) */
  if (ie_direct && recv >= 0) {
    int nblk = nt_ref(nt, id, "block");
    TyKind nrt = comp_ntype(c, recv);
    if (nrt == TY_POLY) nblk = resolve_forwarded_block(c, nblk);
    if (!ty_is_object(nrt) && nblk >= 0 && nt_type(nt, nblk) &&
        sp_streq(nt_type(nt, nblk), "BlockNode")) {
      int nexec = sp_streq(name, "instance_exec");
      int nbp = nt_ref(nt, nblk, "parameters");
      int ninner = nbp >= 0 ? nt_ref(nt, nbp, "parameters") : -1;
      int npnode = ninner >= 0 ? ninner : nbp;
      int nnp = 0; const int *nreqs = npnode >= 0 ? nt_arr(nt, npnode, "requireds", &nnp) : NULL;
      int niargs = nt_ref(nt, id, "arguments");
      int niac = 0; const int *niav = niargs >= 0 ? nt_arr(nt, niargs, "arguments", &niac) : NULL;
      int nbody = nt_ref(nt, nblk, "body");
      int nbn = 0; const int *nbb = nbody >= 0 ? nt_arr(nt, nbody, "body", &nbn) : NULL;
      /* The block's ivars are the receiver's, and this one has none spinel
         lays out: they read nil, as on a fresh object (emit_ie_poly's
         non-object arm), where the caller's own were read and written. A
         write has nowhere to go, and is refused. */
      { int w = ie_body_ivar_write(nt, nbody);
        /* an omitted optional parameter's default runs under the receiver too */
        if (w < 0) w = ie_body_ivar_write(nt, nt_ref(nt, nblk, "parameters"));
        if (w >= 0)
          unsupported_feature(c, w, "an instance variable written in a block that instance_exec or instance_eval runs "
                                    "on a value with no instance variable layout (nil, a builtin value, Object.new): "
                                    "the value has no slot to hold it at run time"); }
      int tself = ++g_tmp;
      {
        Buf rb; memset(&rb, 0, sizeof rb);
        emit_boxed(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tself, rb.p ? rb.p : "sp_box_nil()");
        free(rb.p);
      }
      /* instance_exec binds its arguments as a yield binds them
         (emit_block_binds); instance_eval hands each parameter the receiver */
      int sv_nargov = g_n_argov;
      /* the arguments are the caller's: they run first, reading its ivars,
         into temps the binds read (as the object splice does); the block's
         defaults and body run under the receiver */
      int sv_nil_ie = g_ie_nil_ivars; g_ie_nil_ivars = 0;
      if (nexec) emit_args_off_self(c, niav, niac, g_pre);
      g_ie_nil_ivars = id + 1;
      /* a String variable the block appends to is aliased, as a yield's is
         (#6179); the alias's brace closes after the body, which then leaves
         its value in a temp */
      BlockAliases nal = { .n = 0 };
      int nal_tr = -1;
      TyKind nal_vt = TY_UNKNOWN;
      if (nexec) {
        /* into a side buffer: a value's own prelude drains into g_pre first */
        Buf bb; memset(&bb, 0, sizeof bb);
        emit_block_binds(c, nblk, niav, niac, &bb, g_indent, 0, NULL, &nal);
        if (nal.open && nbn > 0) {
          TyKind lt = comp_ntype(c, nbb[nbn - 1]);
          nal_vt = comp_ntype(c, id) == TY_POLY && lt != TY_POLY ? TY_POLY : lt;
          if (nal_vt != TY_VOID && nal_vt != TY_NIL && nal_vt != TY_UNKNOWN &&
              (ty_is_object(nal_vt) || c_type_name(nal_vt))) {
            nal_tr = ++g_tmp;
            emit_indent(g_pre, g_indent); emit_ctype(c, nal_vt, g_pre);
            buf_printf(g_pre, " _t%d = %s; ", nal_tr, comp_ty_value_obj(c, nal_vt) ? "{0}" : default_value_from_compiler(c, nal_vt));
            emit_gc_root_tmp(c, nal_vt, nal_tr, g_pre);
            buf_puts(g_pre, "\n");
          }
        }
        if (bb.p) buf_puts(g_pre, bb.p);
        free(bb.p);
      }
      else for (int p = 0; p < nnp; p++) {
        const char *pn = nreqs ? nt_str(nt, nreqs[p], "name") : NULL;
        if (!pn) continue;
        LocalVar *plv = scope_local(comp_scope_of(c, nreqs[p]), pn);
        if (!plv || plv->type == TY_UNKNOWN) continue;   /* unused param */
        emit_indent(g_pre, g_indent);
        if (plv->type == TY_POLY) buf_printf(g_pre, "lv_%s = _t%d;\n", rename_local(pn), tself);
        else { buf_printf(g_pre, "lv_%s = ", rename_local(pn)); emit_ie_param_default(c, plv->type, g_pre); buf_puts(g_pre, ";\n"); }
      }
      if (!nexec && (block_keyword_name(c, nblk, 0) || block_kwrest_name(c, nblk)))
        emit_block_kw_binds(c, nblk, -1, comp_scope_of(c, id), g_pre, g_indent, 0, NULL, NULL);
      view_unbind(sv_nargov);
      TyKind nbt = nbn > 0 ? comp_ntype(c, nbb[nbn - 1]) : TY_NIL;
      const char *sv_self = g_self, *sv_deref = g_self_deref;
      char selfb[32]; snprintf(selfb, sizeof selfb, "_t%d", tself);
      g_self = selfb; g_self_deref = ".";
      int *nsnap = ie_body_retype(c, nbody, -2 - id);
      for (int j = 0; j < nbn - 1; j++) emit_stmt(c, nbb[j], g_pre, g_indent);
      if (nbn == 0) {
        ie_body_restore(c, nsnap); g_self = sv_self; g_self_deref = sv_deref;
        if (nal.open) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
        g_ie_nil_ivars = sv_nil_ie;
        buf_puts(b, "sp_box_nil()"); return 1;
      }
      int nscalar = is_scalar_ret(nbt) && nbt != TY_VOID && nbt != TY_NIL && nbt != TY_UNKNOWN;
      int nbox = comp_ntype(c, id) == TY_POLY && nbt != TY_POLY;
      if (nal.open) {
        Buf vb; memset(&vb, 0, sizeof vb);
        if (nal_vt == TY_POLY && nbt != TY_POLY) emit_boxed(c, nbb[nbn - 1], &vb); else emit_expr(c, nbb[nbn - 1], &vb);
        emit_indent(g_pre, g_indent);
        if (nal_tr >= 0) buf_printf(g_pre, "_t%d = %s;\n", nal_tr, vb.p ? vb.p : "0");
        else buf_printf(g_pre, "%s;\n", vb.p ? vb.p : "0");
        free(vb.p);
        ie_body_restore(c, nsnap);
        g_self = sv_self; g_self_deref = sv_deref;
        for (int a = 0; a < nal.n; a++)
          if (--nal.lv[a]->inline_alias == 0) nal.lv[a]->is_cell = 0;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        if (nal_tr >= 0) buf_printf(b, "_t%d", nal_tr);
        else buf_puts(b, nbox ? "sp_box_nil()" : default_value_from_compiler(c, nbt));
        g_ie_nil_ivars = sv_nil_ie;
        return 1;
      }
      if (nscalar) {
        int tr = ++g_tmp;
        Buf vb = expr_buf(c, nbb[nbn - 1]);
        emit_indent(g_pre, g_indent); emit_ctype(c, nbt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", tr, vb.p ? vb.p : "0"); free(vb.p);
        ie_body_restore(c, nsnap);
        g_self = sv_self; g_self_deref = sv_deref;
        char trb[24]; snprintf(trb, sizeof trb, "_t%d", tr);
        if (nbox) emit_boxed_text(c, nbt, trb, b);
        else buf_puts(b, trb);
      }
      else {
        Buf vb = expr_buf(c, nbb[nbn - 1]);
        ie_body_restore(c, nsnap);
        g_self = sv_self; g_self_deref = sv_deref;
        if (nbox && vb.p) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "%s;\n", vb.p); }
        buf_printf(b, "%s", vb.p && !nbox ? vb.p : "sp_box_nil()"); free(vb.p);
      }
      g_ie_nil_ivars = sv_nil_ie;
      return 1;
    }
  }
  int ie_tramp = 0;
  /* receiverless instance_eval/exec inside an instance method: self is the
     receiver. Lower it like a direct call with self aliased into the temp. */
  int ie_self_cls = -1;
  if (!ie_direct && recv < 0) {
    ie_self_cls = ie_implicit_self_class(c, id);
    if (ie_self_cls >= 0) ie_direct = 1;
    if (ie_self_cls >= 0 && g_emitting_class_id >= 0 && g_emitting_class_id != ie_self_cls &&
        is_descendant(c, g_emitting_class_id, ie_self_cls))
      ie_self_cls = g_emitting_class_id;
  }
  if (!ie_direct && recv >= 0 && nt_ref(nt, id, "block") >= 0 && ty_is_object(comp_ntype(c, recv)))
    ie_tramp = comp_trampoline_kind(c, ty_object_class(comp_ntype(c, recv)), name, NULL);
  /* a RECEIVERLESS call to a trampoline method inside an instance method:
     self is the receiver (`go { ... }` where `def go(&b) = instance_exec(&b)`).
     Without this the call lowers to the real method, whose body is the
     unreachable stub, and the literal block is silently dropped. */
  if (!ie_direct && !ie_tramp && recv < 0 && nt_ref(nt, id, "block") >= 0) {
    Scope *trs = comp_scope_of(c, id);
    if (trs && trs->class_id >= 0 && !trs->is_cmethod) {
      ie_tramp = comp_trampoline_kind(c, trs->class_id, name, NULL);
      if (ie_tramp) ie_self_cls = trs->class_id;
    }
  }
  if (ie_direct || ie_tramp) {
    int blk = nt_ref(nt, id, "block");
    /* `instance_exec(args, &b)` forwarding the enclosing (now-inlined) method's
       block param: the real block is the literal active at the inline splice,
       so resolve the BlockArgumentNode to it (as `inner(&block)` does). */
    if (blk >= 0 && nt_type(nt, blk) && sp_streq(nt_type(nt, blk), "BlockArgumentNode")) {
      if (emit_ie_proc(c, id, recv, ie_self_cls, blk, ie_direct ? 0 : ie_tramp, b)) return 1;
      int is_exec = sp_streq(name, "instance_exec");
      if (g_block_id < 0 && ie_direct && recv < 0 && (is_exec || argc == 0) &&
          resolve_forwarded_block(c, blk) < 0 && !g_yield_proc_ref && !g_current_scope_is_lowered) {
        buf_printf(b, "(sp_raise_cls(\"%s\", \"%s\"), ", is_exec ? "LocalJumpError" : "ArgumentError",
                   is_exec ? "no block given" : "wrong number of arguments (given 0, expected 1..3)");
        emit_ie_param_default(c, comp_ntype(c, id), b);
        buf_puts(b, ")");
        return 1;
      }
      blk = g_block_id;
    }
    TyKind rtype = ie_self_cls >= 0 ? ty_object(ie_self_cls) : comp_ntype(c, recv);
    if (blk >= 0 && ty_is_object(rtype) &&
        (ie_tramp || comp_method_in_chain(c, ty_object_class(rtype), name, NULL) < 0)) {
      int blk_body = nt_ref(nt, blk, "body");
      int ie_bn = 0; const int *ie_bb = blk_body >= 0 ? nt_arr(nt, blk_body, "body", &ie_bn) : NULL;
      int cls_id = ty_object_class(rtype);
      TyKind body_ty = ie_bn > 0 ? comp_ntype(c, ie_bb[ie_bn - 1]) : TY_NIL;
      /* A value-carrying `next`/`break` bound to the splice can widen the
         result past the last expression's type (e.g. `next val + 1` is poly
         while the trailing `999` is int); size the temp to their union. */
      TyKind bn_ty = ie_splice_value_ty(c, blk_body);
      if (bn_ty != TY_UNKNOWN)
        body_ty = (body_ty == TY_NIL || body_ty == TY_UNKNOWN) ? bn_ty : ty_unify(body_ty, bn_ty);
      int scalar_res = is_scalar_ret(body_ty) && body_ty != TY_VOID && body_ty != TY_NIL && body_ty != TY_UNKNOWN;
      int tr = ++g_tmp, tres = ++g_tmp;
      int self_is_val = c->classes[cls_id].is_value_type;
      Buf rb; memset(&rb, 0, sizeof rb);
      if (ie_self_cls >= 0) buf_puts(&rb, g_self);  /* implicit self */
      else emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      /* A value-type receiver is a stack struct, not a pointer: bind the
         rebound self by value and dereference its ivars with `.` in the
         splice. Value types are immutable, so the copy is transparent. */
      buf_printf(g_pre, "sp_%s %s_t%d = %s;\n", c->classes[cls_id].c_name,
                 self_is_val ? "" : "*", tr,
                 rb.p ? rb.p : (self_is_val ? "{0}" : "NULL"));
      free(rb.p);
      if (scalar_res) {
        emit_indent(g_pre, g_indent); emit_ctype(c, body_ty, g_pre);
        buf_printf(g_pre, " _t%d;\n", tres);
      }
      /* The arguments are the caller's, so a value that reads self runs
         here, before self switches, into a temp the binds below read:
         `O.new.instance_exec(@x) { |a| }` read O's @x, and the C did not
         build where O had none. The block's defaults, and a trampoline's
         own arguments, still run under the receiver, as its body does. */
      int ie_sv_argov = g_n_argov;
      { int iargs0 = nt_ref(nt, id, "arguments");
        int iac0 = 0; const int *iav0 = iargs0 >= 0 ? nt_arr(nt, iargs0, "arguments", &iac0) : NULL;
        emit_args_off_self(c, iav0, iac0, g_pre); }
      char selfbuf[320];   /* a class name plus a cast: 64 truncated the longest of them */ snprintf(selfbuf, sizeof selfbuf, "_t%d", tr);
      const char *saved_self2 = g_self; g_self = selfbuf;
      const char *saved_deref2 = g_self_deref; g_self_deref = self_is_val ? "." : "->";
      int saved_ie = g_ie_class_id; g_ie_class_id = cls_id;
      /* In a class method the block's self is still the receiver object, so
         its `@x` is that object's ivar, not the class-level one the method's
         own `@x` names: Phlex's `object.instance_exec { @_content_block =
         block }` in `self.new` wrote the class and the object never saw it.
         The splice runs as an instance method of the receiver's class. */
      Scope *ie_sc = comp_scope_of(c, id);
      int ie_flip = ie_sc && ie_sc->is_cmethod;
      int ie_sv_cm = ie_flip ? ie_sc->is_cmethod : 0, ie_sv_cls = ie_flip ? ie_sc->class_id : -1;
      if (ie_flip) { comp_scope_move_begin(c, (int)(ie_sc - c->scopes)); ie_sc->is_cmethod = 0; ie_sc->class_id = cls_id; }
      /* Bind the block params (interned in the enclosing scope, declared
         there): instance_exec assigns the call-site args; instance_eval
         yields the receiver to each param. */
      BlockAliases ie_al = { .n = 0 };
      {
        int is_exec = ie_tramp ? (ie_tramp == 2) : sp_streq(name, "instance_exec");
        int bp_node = nt_ref(nt, blk, "parameters");
        const char *bpty = bp_node >= 0 ? nt_type(nt, bp_node) : NULL;
        int iargs = nt_ref(nt, id, "arguments");
        int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
        /* a trailing `k: v` call-site hash binds keyword params, not positionals */
        int ie_kwhash = ie_call_kwhash(c, id);
        if (ie_kwhash >= 0) iac -= 1;
        if (is_exec && !(bpty && sp_streq(bpty, "NumberedParametersNode")) && (ie_tramp ? ie_tramp_effective_argc(c, id) : -1) < 0) {
          /* instance_exec binds its arguments as a yield binds them
             (emit_block_binds): the proc distribution, a splat gathered, a
             lone Array auto-splatted, the keywords last. Its own copy bound
             requireds and optionals by index and never took a post, so
             `instance_exec(1) { |*r, a| }` bound r = [1]. */
          int sv_nargov = g_n_argov;
          Buf bb; memset(&bb, 0, sizeof bb);   /* see the non-object receiver's */
          /* a String variable the block appends to is aliased, as a yield's
             is: the block writes through the caller's slot (#6179) */
          emit_block_binds(c, blk, iav, iac + (ie_kwhash >= 0), &bb, g_indent, 0, NULL, &ie_al);
          if (bb.p) buf_puts(g_pre, bb.p);
          free(bb.p);
          view_unbind(sv_nargov);
        }
        else if (bpty && sp_streq(bpty, "NumberedParametersNode")) {
          /* `{ _1.method }`: _1.._N bind like positional block params. */
          int maxn = (int)nt_int(nt, bp_node, "maximum", 0);
          for (int p = 0; p < maxn; p++) {
            const char *pn = numbered_param_name(c, bp_node, p);
            if (!pn) continue;
            LocalVar *plv = scope_local(comp_scope_of(c, id), pn);
            int ppoly = plv && plv->type == TY_POLY;
            int pdecl = plv && plv->type != TY_UNKNOWN;   /* see the requireds loop (#2734) */
            emit_indent(g_pre, g_indent);
            if (!pdecl) buf_puts(g_pre, "(void)(");
            else buf_printf(g_pre, "lv_%s = ", rename_local(pn));
            if (is_exec) {
              if (p < iac) { if (ppoly) emit_boxed(c, iav[p], g_pre); else emit_expr(c, iav[p], g_pre); }
              else emit_ie_param_default(c, plv ? plv->type : TY_POLY, g_pre);
            }
            else buf_printf(g_pre, "_t%d", tr);
            buf_puts(g_pre, pdecl ? ";\n" : ");\n");
          }
        }
        else {
        int inner = bp_node >= 0 ? nt_ref(nt, bp_node, "parameters") : -1;
        int pnode = inner >= 0 ? inner : bp_node;
        int npar = 0; const int *reqs = pnode >= 0 ? nt_arr(nt, pnode, "requireds", &npar) : NULL;
        /* mixed-args trampoline: bind params to the trampoline body's args. */
        int tramp_argc = ie_tramp ? ie_tramp_effective_argc(c, id) : -1;
        for (int p = 0; p < npar; p++) {
          const char *pn = nt_str(nt, reqs[p], "name");
          if (!pn) continue;
          /* Resolve the param against its own block's scope (where block params
             are interned), not the call site's: for a forwarded block (`&b`
             resolved to the literal at a different site) the call scope holds a
             different `a`, mis-reading its slot type. */
          LocalVar *plv = scope_local(comp_scope_of(c, reqs[p]), pn);
          int ppoly = plv && plv->type == TY_POLY;  /* widened slot needs a boxed rvalue */
          /* a scalar slot (e.g. an int block param, which is NOT widened) fed a
             poly arg needs the reverse: unbox the poly down to the slot type. */
          int pscalar = plv && plv->type != TY_POLY && plv->type != TY_UNKNOWN;
          /* an unused param stays TY_UNKNOWN and gets no C declaration:
             evaluate its rvalue for effect only (#2734) */
          int pdecl = plv && plv->type != TY_UNKNOWN;
          emit_indent(g_pre, g_indent);
          if (!pdecl) buf_puts(g_pre, "(void)(");
          else buf_printf(g_pre, "lv_%s = ", rename_local(pn));
          if (tramp_argc >= 0) {
            int an = ie_tramp_effective_arg(c, id, p);
            Buf eb; memset(&eb, 0, sizeof eb);
            if (an >= 0) { if (ppoly) emit_boxed(c, an, &eb); else emit_expr(c, an, &eb); }
            /* a parameter the trampoline passes nothing for is nil in its
               slot's own form: the `0` assigned an int to a boxed one */
            else if (pdecl) emit_ie_param_default(c, plv->type, &eb);
            else buf_puts(&eb, "0");
            if (an >= 0 && pscalar && comp_ntype(c, an) == TY_POLY)
              emit_unbox_text(c, plv->type, eb.p ? eb.p : "", g_pre);
            else buf_puts(g_pre, eb.p ? eb.p : "0");
            free(eb.p);
          }
          else {
            /* instance_eval yields self. A poly slot takes it boxed: the
               parameter's slot exists whether or not the body reads it, and
               the receiver is an object (a pointer, or a struct for a value
               type), which is not an sp_RbVal. */
            if (ppoly) {
              char selfsrc[32];
              snprintf(selfsrc, sizeof selfsrc, "_t%d", tr);
              emit_boxed_text(c, ty_object(cls_id), selfsrc, g_pre);
            }
            else buf_printf(g_pre, "_t%d", tr);
          }
          buf_puts(g_pre, pdecl ? ";\n" : ");\n");
        }
        if (block_keyword_name(c, blk, 0) || block_kwrest_name(c, blk))
          emit_block_kw_binds(c, blk, ie_kwhash, comp_scope_of(c, id), g_pre, g_indent, 0, NULL, NULL);
        }
      }
      view_unbind(ie_sv_argov);   /* the binds were the arguments' last readers */
      if (ie_bn > 0) {
        /* In statement position the value is discarded, so emit the whole body
           as statements -- the last node may not be expressible (e.g. puts). */
        int last_as_stmt = g_ie_discard_value && !scalar_res;
        int upto = last_as_stmt ? ie_bn : ie_bn - 1;
        int saved_discard = g_ie_discard_value; g_ie_discard_value = 0;
        /* A break/next that binds to the splice (not a nested loop) needs a C
           loop to target: wrap the body in do{}while(0). `break <v>` captures
           into the result temp via g_loop_break_var; `next <v>` via
           g_ie_next_var. A `return` still returns from the enclosing function. */
        int ie_bn_wrap = ie_body_has_break_next(c, blk_body);
        const char *sv_lb = g_loop_break_var, *sv_nx = g_ie_next_var;
        /* the splice body's break binds to the do/while(0) below, never to an
           enclosing valued-break scope */
        const char *sv_bser = g_brk_ser_var; g_brk_ser_var = NULL;
        int sv_iep = g_ie_res_poly;
        g_ie_res_poly = (scalar_res && body_ty == TY_POLY);
        char bvbuf[32];
        int sv_lexc2 = g_loop_exc_base, sv_lens2 = g_loop_ensure_base;
        if (ie_bn_wrap) {
          g_loop_exc_base = g_exc_frame_depth;   /* break/next exit the do{}while(0) */
          g_loop_ensure_base = g_ensure_depth;   /* ... and run only ensures opened inside it */
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "do {\n"); g_indent++;
          if (scalar_res) { snprintf(bvbuf, sizeof bvbuf, "_t%d", tres); g_loop_break_var = bvbuf; g_ie_next_var = bvbuf; }
          else { g_loop_break_var = NULL; g_ie_next_var = NULL; }
          g_c_loop_depth++;   /* the do{} wrapper makes `continue` valid */
        }
        for (int j = 0; j < upto; j++) emit_stmt(c, ie_bb[j], g_pre, g_indent);
        if (!last_as_stmt) {
          Buf vb; memset(&vb, 0, sizeof vb);
          /* The last expression feeds the (possibly poly-widened) result slot;
             box it when the slot is poly but this expression is scalar. */
          if (scalar_res && body_ty == TY_POLY) emit_boxed(c, ie_bb[ie_bn - 1], &vb);
          else emit_expr(c, ie_bb[ie_bn - 1], &vb);
          emit_indent(g_pre, g_indent);
          if (!scalar_res) {
            if (vb.p) buf_printf(g_pre, "%s;\n", vb.p);
          }
          else {
            buf_printf(g_pre, "_t%d = %s;\n", tres, vb.p ? vb.p : "0");
          }
          free(vb.p);
        }
        if (ie_bn_wrap) {
          g_c_loop_depth--;
          g_loop_break_var = sv_lb; g_ie_next_var = sv_nx;
          g_indent--; emit_indent(g_pre, g_indent); buf_puts(g_pre, "} while (0);\n");
        }
        g_loop_exc_base = sv_lexc2; g_loop_ensure_base = sv_lens2;
        g_ie_res_poly = sv_iep;
        g_brk_ser_var = sv_bser;
        g_ie_discard_value = saved_discard;
      }
      /* the aliases end with the body, which the result temps outlive */
      for (int a = 0; a < ie_al.n; a++)
        if (--ie_al.lv[a]->inline_alias == 0) ie_al.lv[a]->is_cell = 0;
      if (ie_al.open) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
      g_ie_class_id = saved_ie;
      if (ie_flip) { ie_sc->is_cmethod = ie_sv_cm; ie_sc->class_id = ie_sv_cls; comp_scope_move_end(); }
      g_self = saved_self2;
      g_self_deref = saved_deref2;
      if (scalar_res) buf_printf(b, "_t%d", tres);
      else buf_printf(b, "_t%d", tr);  /* statement use: value is the receiver */
      return 1;
    }
  }
  return 0;
}

/* freeze / frozen?, dup / clone, the identity methods that answer the receiver, and then / yield_self */
int emit_call_freeze_dup_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc) {
  /* freeze / frozen? on an array set/read the struct's frozen flag */
  if (recv >= 0 && argc == 0 && comp_ntype(c, recv) != TY_POLY) {
    TyKind crt = comp_ntype(c, recv);
    const char *ck = (crt == TY_POLY_ARRAY) ? "Poly" : array_kind(crt);
    /* An empty array literal infers TY_UNKNOWN and emits as sp_IntArray_new();
       without this `[].freeze` dropped the call and `[].freeze.frozen?`
       answered false (#3828). */
    if (!ck) {
      const char *rvt = nt_type(nt, recv);
      int een = 0;
      if (rvt && sp_streq(rvt, "ArrayNode") && (nt_arr(nt, recv, "elements", &een), een == 0))
        ck = "Int";
    }
    /* An empty hash literal is typed by context too, and its freeze fell
       through every arm the same way (#3828). Its frozen bit lives in the GC
       header, so the emitted pointer keeps its own type. */
    if (!ck && sp_streq(name, "freeze")) {
      const char *hvt = nt_type(nt, recv);
      int hen = 0;
      if (hvt && (sp_streq(hvt, "HashNode") || sp_streq(hvt, "KeywordHashNode")) &&
          (nt_arr(nt, recv, "elements", &hen), hen == 0)) {
        int ht = ++g_tmp;
        buf_printf(b, "({ __typeof__(");
        emit_expr(c, recv, b);
        buf_printf(b, ") _t%d = ", ht);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_gc_freeze((void *)_t%d); _t%d; })", ht, ht);
        return 1;
      }
    }
    if (ck && sp_streq(name, "freeze")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", ck, t); emit_expr(c, recv, b);
      buf_printf(b, "; if (_t%d) _t%d->frozen = 1; _t%d; })", t, t, t);
      return 1;
    }
    if (ck && sp_streq(name, "frozen?")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen != 0)");
      return 1;
    }
  }

  /* freeze / frozen? on hashes: use the GC-header frozen bit */
  if (recv >= 0 && argc == 0 && ty_is_hash(comp_ntype(c, recv))) {
    if (sp_streq(name, "to_h") && nt_ref(nt, id, "block") < 0) {  /* identity */
      emit_expr(c, recv, b);
      return 1;
    }
    if (sp_streq(name, "freeze")) {
      buf_puts(b, "sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
  }

  /* frozen? on numeric/symbol scalars: always frozen in Ruby semantics.
     TY_STRING uses a runtime check because dup/String.new produce unfrozen strings. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?")) {
    TyKind frt = comp_ntype(c, recv);
    if (frt == TY_INT || frt == TY_FLOAT || frt == TY_SYMBOL || frt == TY_BOOL || frt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return 1;
    }
    /* a shared String handle keeps its own frozen flag: read through it, not
       through the String value the handle is read as (a fresh copy) */
    char fsref[1024];
    if ((frt == TY_STRING || frt == TY_STRBUF) && strbuf_slot_ref(c, recv, fsref, sizeof fsref)) {
      buf_printf(b, "sp_String_is_frozen(%s)", fsref);
      return 1;
    }
    if (frt == TY_STRING) {
      buf_puts(b, "sp_str_is_frozen_val("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (frt == TY_POLY) {
      buf_puts(b, "sp_poly_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
  }

  /* TY_STRING freeze: update the variable to the frozen copy and return it */
  if (recv >= 0 && argc == 0 && sp_streq(name, "freeze") && comp_ntype(c, recv) == TY_STRING) {
    const char *rtyf = nt_type(nt, recv);
    int assignable_f = rtyf && (sp_streq(rtyf, "LocalVariableReadNode") || sp_streq(rtyf, "InstanceVariableReadNode"));
    /* a shared String handle reads back as an expression (a copy), which
       cannot be assigned to: freeze the handle, which every later mutation
       through it checks, and answer its contents frozen */
    char fzref[1024];
    if (assignable_f && strbuf_slot_ref(c, recv, fzref, sizeof fzref)) {
      buf_printf(b, "({ sp_String_freeze(%s); sp_str_freeze_val(", fzref);
      emit_expr(c, recv, b); buf_puts(b, "); })");
      return 1;
    }
    if (assignable_f) {
      buf_puts(b, "({ ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_freeze_val("); emit_expr(c, recv, b); buf_puts(b, "); ");
      emit_expr(c, recv, b); buf_puts(b, "; })");
    }
    else {
      buf_puts(b, "sp_str_freeze_val("); emit_expr(c, recv, b); buf_puts(b, ")");
    }
    return 1;
  }

  /* dup/clone of a user (pointer) object: allocate a fresh instance, shallow-copy
     the struct (cls_id + all ivars), then -- if the class defines initialize_copy
     -- run it with the original so deep-copy hooks fire. Without this, dup/clone
     fell through to the identity shortcut below and aliased the original.
     Value-type objects copy by value already; exception subclasses use distinct
     allocation, so both stay on the identity path. */
  if (recv >= 0 && (sp_streq(name, "dup") || sp_streq(name, "clone")) &&
      /* a generated READER of the name owns it, as in CRuby (#4190), and so
         does a method the class defines itself: Nokogiri's Node#dup is a deep
         copy, and the built-in shallow copy took its place (#5450) */
      !(ty_is_object(comp_ntype(c, recv)) &&
        comp_resolve_member(c, ty_object_class(comp_ntype(c, recv)), name, 0, NULL, NULL) != SP_MEMBER_NONE)) {
    int dargs = nt_ref(nt, id, "arguments");
    int dargc = 0; const int *dargv = dargs >= 0 ? nt_arr(nt, dargs, "arguments", &dargc) : NULL;
    TyKind drt = comp_ntype(c, recv);
    /* clone(freeze: true/false): -1 = not given (copy the receiver's state),
       0 = false, 1 = true. Only a single `freeze:` keyword arg is accepted. */
    int freeze_mode = -1, dkw_ok = (dargc == 0);
    if (dargc == 1 && dargv && sp_streq(name, "clone") &&
        nt_type(nt, dargv[0]) && sp_streq(nt_type(nt, dargv[0]), "KeywordHashNode")) {
      int kn = 0; const int *kel = nt_arr(nt, dargv[0], "elements", &kn);
      if (kn == 1 && nt_type(nt, kel[0]) && sp_streq(nt_type(nt, kel[0]), "AssocNode")) {
        int kk = nt_ref(nt, kel[0], "key"), kv = nt_ref(nt, kel[0], "value");
        if (kk >= 0 && nt_type(nt, kk) && sp_streq(nt_type(nt, kk), "SymbolNode") &&
            nt_str(nt, kk, "value") && sp_streq(nt_str(nt, kk, "value"), "freeze") && kv >= 0) {
          const char *kvt = nt_type(nt, kv);
          if (kvt && sp_streq(kvt, "TrueNode"))  { freeze_mode = 1; dkw_ok = 1; }
          else if (kvt && sp_streq(kvt, "FalseNode")) { freeze_mode = 0; dkw_ok = 1; }
        }
      }
    }
    if (dkw_ok && ty_is_object(drt) && !comp_ty_value_obj(c, drt)) {
      int cid = ty_object_class(drt);
      /* native-bound classes have no generated pool/struct copy; their dup
         dispatches to a declared native_method instead */
      if (!class_is_exc_subclass(c, cid) && !c->classes[cid].is_native_class) {
        /* #dup drops the singleton methods, so the copy is an instance of the
           class Ruby sees; the singleton struct is a layout superset of it,
           so the parent's prefix is what carries over (#3739) */
        int dup_desingleton = sp_streq(name, "dup") && c->classes[cid].is_singleton_of;
        if (dup_desingleton) cid = singleton_visible_ci(c, cid);
        ClassInfo *dci = &c->classes[cid];
        const char *cn = dci->c_name;
        int defcls = -1;
        int ic = comp_method_in_chain(c, cid, "initialize_copy", &defcls);
        LocalVar *icp = (ic >= 0 && c->scopes[ic].nparams >= 1)
          ? scope_local(&c->scopes[ic], c->scopes[ic].pnames[0]) : NULL;
        TyKind ictp = icp ? icp->type : TY_UNKNOWN;
        int to = ++g_tmp, td = ++g_tmp;
        buf_printf(b, "({ sp_%s *_t%d = ", cn, to);
        if (dup_desingleton) buf_printf(b, "(sp_%s *)", cn);
        emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%s *_t%d = SP_POOL_NEW(%s, %s%s%s);"
                      " *_t%d = *_t%d; SP_GC_ROOT(_t%d); ",
                   to, cn, td, cn,
                   class_needs_scan(dci) ? "sp_" : "", class_needs_scan(dci) ? cn : "NULL",
                   class_needs_scan(dci) ? "__gc_scan" : "", td, to, td);
        /* The struct copy carries the ORIGINAL's cls_id, which for a singleton
           receiver is the synthesized subclass: the copy then answered that
           class's methods at run time, while `respond_to?` -- reading the
           static type -- said it did not (#4043). #dup drops them, so the copy
           is an instance of the class Ruby sees. */
        if (dup_desingleton) buf_printf(b, "_t%d->cls_id = %d; ", td, cid);
        /* Invoke the hook when its param was typed by the seeding pass to any
           object class -- it unifies to a common ancestor when both a parent and
           a subclass are dup'd, so accept ty_is_object, casting the original to
           the param's class. TY_POLY -> box it. */
        if (ic >= 0 && (ty_is_object(ictp) || ictp == TY_POLY)) {
          const char *nb = c->scopes[ic].blk_param && c->scopes[ic].blk_param[0] && !c->scopes[ic].yields ? ", NULL" : "";
          emit_method_cname(c, &c->scopes[ic], b); buf_puts(b, "(");
          if (defcls != cid) buf_printf(b, "(sp_%s *)", c->classes[defcls].c_name);
          if (ictp == TY_POLY) { buf_printf(b, "_t%d, sp_box_obj(_t%d, %d)%s); ", td, to, cid, nb); }
          else {
            int icid = ty_object_class(ictp);
            buf_printf(b, "_t%d, ", td);
            if (icid != cid) buf_printf(b, "(sp_%s *)", c->classes[icid].c_name);
            buf_printf(b, "_t%d%s); ", to, nb);
          }
        }
        /* clone copies the receiver's frozen state (dup never does); an
           explicit `freeze:` overrides (#2625, #2626). A Data instance is
           frozen by construction and stays so through EVERY copy -- CRuby's
           Data#dup and clone(freeze: false) both return a frozen value
           (#2716). */
        if (dci->is_data) buf_printf(b, "sp_gc_freeze(_t%d); ", td);
        else if (sp_streq(name, "clone")) {
          if (freeze_mode == 1) buf_printf(b, "sp_gc_freeze(_t%d); ", td);
          else if (freeze_mode < 0) buf_printf(b, "if (sp_gc_is_frozen(_t%d)) sp_gc_freeze(_t%d); ", to, td);
        }
        buf_printf(b, "_t%d; })", td);
        return 1;
      }
    }
  }

  /* nil? on a pointer-backed Enumerator: NULL encodes nil, as elsewhere */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      comp_ntype(c, recv) == TY_ENUMERATOR) {
    buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == NULL)");
    return 1;
  }

  /* frozen? on an immutable value type is constantly true (CRuby freezes
     Integer/Float/Symbol/booleans/nil/Range/Complex/Rational values) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?")) {
    TyKind fvt = comp_ntype(c, recv);
    if (fvt == TY_NIL) { buf_puts(b, "1"); return 1; }
    if (fvt == TY_INT || fvt == TY_FLOAT || fvt == TY_SYMBOL || fvt == TY_BOOL ||
        fvt == TY_RANGE || fvt == TY_COMPLEX || fvt == TY_RATIONAL ||
        fvt == TY_BIGINT) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return 1;
    }
  }

  /* identity methods -> the receiver itself */
  if (recv >= 0 &&
      is_self_copy(name) &&
      /* a generated READER of the name owns it, as in CRuby (#4190), and so
         does a method the class defines itself: Nokogiri's Node#dup is a deep
         copy, and the built-in shallow copy took its place (#5450) */
      !(ty_is_object(comp_ntype(c, recv)) &&
        comp_resolve_member(c, ty_object_class(comp_ntype(c, recv)), name, 0, NULL, NULL) != SP_MEMBER_NONE) &&
      !(argc == 0 && comp_ntype(c, recv) == TY_POLY && !sp_streq(name, "itself") && user_defines_or_reads(c, name))) {
    int args = nt_ref(nt, id, "arguments");
    int argc0 = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc0);
    /* hash, string, array, and native-bound object dup/clone require real
       copies (mutable reference types; a native class declares its own dup) --
       skip the identity shortcut for them so the dedicated copy paths run.
       freeze/itself on any value stay identity. */
    TyKind recv_t = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    int is_dup_clone = sp_streq(name, "dup") || sp_streq(name, "clone");
    int recv_native = ty_is_object(recv_t) &&
                      c->classes[ty_object_class(recv_t)].is_native_class;
    /* freeze on a user instance or a boxed value has real state (the GC
       header bit / string marker) -- served by the receiver arms, not the
       identity shortcut. A concurrency handle is a heap instance too, so its
       freeze is equally stateful; taking the shortcut here made `freeze` a
       no-op and left `frozen?` answering false right after it (#3483). The
       same holds for every other GC-allocated handle -- Random, Dir, Addrinfo,
       IO, Enumerator, Method, OpenStruct, a curried Proc -- whose frozen?
       reads the header bit (emit_native_object_protocol). */
    int freeze_stateful = sp_streq(name, "freeze") &&
                          (ty_is_object(recv_t) || recv_t == TY_POLY ||
                           recv_t == TY_MUTEX || recv_t == TY_QUEUE ||
                           recv_t == TY_CONDVAR || recv_t == TY_FIBER ||
                           recv_t == TY_THREAD || recv_t == TY_RANDOM ||
                           recv_t == TY_DIR || recv_t == TY_ADDRINFO ||
                           recv_t == TY_IO || recv_t == TY_ENUMERATOR ||
                           recv_t == TY_METHOD || recv_t == TY_OPENSTRUCT ||
                           recv_t == TY_CURRY || recv_t == TY_MATCHDATA ||
                           recv_t == TY_EXCEPTION);
    /* itself is pure identity for every receiver, hashes included; the
       hash exclusion below is for freeze/dup/clone, which need real
       handling on the reference types */
    if (argc0 == 0 && sp_streq(name, "itself")) { emit_expr(c, recv, b); return 1; }
    /* clone(freeze: _) on an immutable value is the value itself; the
       keyword can't unfreeze what has no mutable state (#2379) */
    if (argc0 == 1 && sp_streq(name, "clone") &&
        (recv_t == TY_INT || recv_t == TY_FLOAT || recv_t == TY_BOOL ||
         recv_t == TY_NIL || recv_t == TY_SYMBOL || recv_t == TY_RANGE ||
         recv_t == TY_RATIONAL || recv_t == TY_COMPLEX || recv_t == TY_BIGINT)) {
      int dargs2 = nt_ref(nt, id, "arguments");
      int dn2 = 0; const int *dv2 = dargs2 >= 0 ? nt_arr(nt, dargs2, "arguments", &dn2) : NULL;
      if (dn2 == 1 && nt_type(nt, dv2[0]) && sp_streq(nt_type(nt, dv2[0]), "KeywordHashNode")) {
        /* clone(freeze: false) on an always-frozen immediate can't produce an
           unfrozen copy: CRuby raises ArgumentError. freeze: true / nil (and a
           non-immediate like Range) return the value. (#2597) */
        int fval = kwh_lookup(nt, dv2[0], "freeze");
        const char *fvt = fval >= 0 ? nt_type(nt, fval) : NULL;
        const char *icn = recv_t == TY_FLOAT ? "Float" : recv_t == TY_SYMBOL ? "Symbol"
                        : recv_t == TY_NIL ? "NilClass"
                        : (recv_t == TY_INT || recv_t == TY_BIGINT) ? "Integer" : NULL;
        if (fvt && sp_streq(fvt, "FalseNode") && icn) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), (sp_raise_cls(\"ArgumentError\", \"can't unfreeze %s\"), %s))",
                     icn, default_value_from_compiler(c, recv_t));
          return 1;
        }
        emit_expr(c, recv, b); return 1;
      }
    }
    /* dup/clone of a boxed plain object copies (Object.new included); the
       identity shortcut would alias the original and o.dup == o would
       misreport true */
    if (argc0 == 0 && is_dup_clone && recv_t == TY_POLY) {
      buf_printf(b, "sp_poly_dup(");
      emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "clone") ? 1 : 0);
      return 1;
    }
    /* clone(freeze: <literal>) on a poly receiver: the immutability of the
       runtime value decides whether `freeze: false` raises, so dispatch on the
       tag at runtime (#3033) */
    if (argc0 == 1 && is_dup_clone && sp_streq(name, "clone") && recv_t == TY_POLY) {
      int cargs = nt_ref(nt, id, "arguments");
      int cn = 0; const int *cv = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cn) : NULL;
      if (cn == 1 && cv && nt_type(nt, cv[0]) && sp_streq(nt_type(nt, cv[0]), "KeywordHashNode")) {
        int fval = kwh_lookup(nt, cv[0], "freeze");
        const char *fvt = fval >= 0 ? nt_type(nt, fval) : NULL;
        if (fvt && (sp_streq(fvt, "FalseNode") || sp_streq(fvt, "TrueNode") || sp_streq(fvt, "NilNode"))) {
          int fz = sp_streq(fvt, "FalseNode") ? 0 : sp_streq(fvt, "TrueNode") ? 1 : -1;
          buf_puts(b, "sp_poly_clone_freeze("); emit_expr(c, recv, b);
          buf_printf(b, ", %d)", fz);
          return 1;
        }
      }
    }
    /* Proc#dup/#clone: a distinct shallow copy, not the identity shortcut
       below (d.equal?(pr) must be false) (#3048) */
    if (argc0 == 0 && is_dup_clone && recv_t == TY_PROC) {
      buf_printf(b, "sp_proc_dup(");
      emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "clone") ? 1 : 0);
      return 1;
    }
    if (argc0 == 0 && !freeze_stateful && !ty_is_hash(recv_t) &&
        !(is_dup_clone && (recv_t == TY_STRING || ty_is_array(recv_t) || recv_native))) {
      emit_expr(c, recv, b); return 1;
    }
    if (argc0 == 0 && recv_t == TY_STRING && is_dup_clone) {
      /* clone preserves the frozen state; dup always returns an unfrozen copy. */
      /* sp_str_dup, not dup_external: byte_len-aware, carries embedded NULs. */
      /* A shared String handle keeps its frozen flag on the handle, and reads
         back as a fresh copy that never carries it: clone asks the handle,
         as frozen? does, or the clone of a frozen String came back unfrozen. */
      char clref[1024];
      if (sp_streq(name, "clone") && strbuf_slot_ref(c, recv, clref, sizeof clref)) {
        int th = ++g_tmp, tcl = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s; const char *_t%d = _t%d ? sp_str_dup(sp_String_cstr(_t%d)) : NULL;"
                      " _t%d && sp_String_is_frozen(_t%d) ? sp_str_freeze_val(_t%d) : _t%d; })",
                   th, clref, tcl, th, th, tcl, th, tcl, tcl);
        return 1;
      }
      buf_printf(b, "%s(", sp_streq(name, "clone") ? "sp_str_clone_val" : "sp_str_dup");
      emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
  }

  /* then / yield_self: pass receiver to block, return block result */
  if (recv >= 0 && (sp_streq(name, "then") || sp_streq(name, "yield_self"))) {
    int blk = nt_ref(nt, id, "block");
    /* with NO block, an enumerator of one element -- the receiver (#4028),
       named `then` for either name, as CRuby's yield_self is then's alias */
    if (blk < 0 && nt_ref(nt, id, "arguments") < 0) {
      buf_puts(b, "sp_enum_of_one(");
      emit_boxed(c, recv, b);
      buf_puts(b, ", SPL(\"then\"))");
      return 1;
    }
    if (blk >= 0) {
      TyKind rtype = infer_type(c, recv);
      const char *bp0 = block_param_name(c, blk, 0); if (bp0) bp0 = rename_local(bp0);
      int blk_body = nt_ref(nt, blk, "body");
      int then_bn = 0; const int *then_bb = blk_body >= 0 ? nt_arr(nt, blk_body, "body", &then_bn) : NULL;
      if (then_bn >= 1) {
        Scope *tsc = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *tlv0 = (tsc && bp0) ? scope_local(tsc, bp0) : NULL;
        TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
        int use_shadow_th = tlv0 && tlv0->type != rtype && rtype != TY_UNKNOWN;
        /* Pin block param type early so body_ty is computed with correct cache */
        if (use_shadow_th && tlv0) {
          tlv0->type = rtype;
          for (int j = 0; j < then_bn; j++) infer_type(c, then_bb[j]);
        }
        TyKind body_ty = infer_type(c, then_bb[then_bn - 1]);
        int tr = ++g_tmp, tres = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rtype, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
        /* Declare tres at outer scope so it is visible after any shadow block */
        emit_indent(g_pre, g_indent); emit_ctype(c, body_ty, g_pre);
        buf_printf(g_pre, " _t%d;\n", tres);
        int bodyIndent = g_indent;
        if (use_shadow_th) {
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
          bodyIndent = g_indent + 1;
          emit_indent(g_pre, bodyIndent); emit_ctype(c, rtype, g_pre);
          buf_printf(g_pre, " lv_%s = _t%d;\n", bp0, tr);
        }
        else if (bp0) {
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", bp0, tr);
        }
        for (int j = 0; j < then_bn - 1; j++) emit_stmt(c, then_bb[j], g_pre, bodyIndent);
        int save_ind = g_indent; g_indent = bodyIndent;
        Buf vb = expr_buf(c, then_bb[then_bn - 1]);
        g_indent = save_ind;
        emit_indent(g_pre, bodyIndent); buf_printf(g_pre, "_t%d = %s;\n", tres, vb.p ? vb.p : "0"); free(vb.p);
        if (use_shadow_th) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
        if (use_shadow_th && tlv0) tlv0->type = tsaved0;
        buf_printf(b, "_t%d", tres);
        return 1;
      }
    }
  }
  return 0;
}

/* safe navigation (&.): a nil receiver answers nil, any other the call, guarded by a nil test */
int emit_call_safe_nav_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv) {
  /* Safe navigation &. : nil receiver -> return nil/0; non-nil -> emit conditional */
  {
    const char *safe_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && safe_op && sp_streq(safe_op, "&.")) {
      TyKind rrt = comp_ntype(c, recv);
      if (rrt == TY_NIL) {
        /* nil&.foo always returns nil */
        TyKind ret = comp_ntype(c, id);
        const char *dv = default_value_from_compiler(c, ret);
        buf_puts(b, dv ? dv : "0");
        return 1;
      }
      /* The poly builtins that answer an UNBOXED scalar: the safe-nav result is
         poly (it may be nil), so the value arm has to be boxed to agree with
         the nil arm -- otherwise the two ternary arms have different C types
         and the program does not build (#3899). */
      if (rrt == TY_POLY && comp_ntype(c, id) == TY_POLY &&
          g_sn_skip != id && nt_ref(nt, id, "block") < 0) {
        /* The call's natural type: infer_call states what the poly runtime
           answers as a raw C scalar, so this arm needs no table of its own --
           it boxes whatever the type names. A hand-kept list stood here and
           went stale within a day of being written. */
        TyKind nat = infer_uncached(c, id);
        int boxit = (nat != TY_POLY && nat != TY_UNKNOWN && nat != TY_VOID &&
                     !ty_is_array(nat) && nat != TY_STRING);
        if (boxit && !user_defines_or_reads(c, name)) {
          int tsn = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _sn_%d = ", tsn); emit_expr(c, recv, b);
          buf_printf(b, "; _sn_%d.tag == SP_TAG_NIL ? sp_box_nil() : ", tsn);
          if (g_n_argov < MAX_ARG_OVERRIDE) {
            int slot3 = view_bind(recv, "_sn_%d", tsn);
            int sv_skip3 = g_sn_skip; g_sn_skip = id;
            /* Re-enter with the call's NATURAL type in place of the widened
               poly one: a dispatch that reads it (`poly.include?` declares its
               accumulator from it) then renders the unboxed answer the box
               below expects, instead of an sp_RbVal the arms assign bools to. */
            int vw = view_push(c, id, nat);
            Buf vb; memset(&vb, 0, sizeof vb);
            emit_expr(c, id, &vb);
            view_pop(c, vw);
            g_sn_skip = sv_skip3;
            view_unbind(g_n_argov - 1);
            emit_boxed_text(c, nat, vb.p ? vb.p : "", b);
            free(vb.p);
          }
          else emit_expr(c, id, b);
          buf_puts(b, "; })");
          return 1;
        }
      }
      if (rrt == TY_POLY && is_len_alias(name) &&
          comp_ntype(c, id) == TY_POLY) {
        /* poly&.length/size: the poly builtin emits an unboxed sp_int, but
           the safe-nav result is inferred poly -- box it so both ternary arms
           are sp_RbVal (#3269). */
        int tsn = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _sn_%d = ", tsn); emit_expr(c, recv, b);
        buf_printf(b, "; _sn_%d.tag == SP_TAG_NIL ? sp_box_nil() : sp_box_int(sp_poly_length(_sn_%d)); })", tsn, tsn);
        return 1;
      }
      if (rrt == TY_POLY && g_sn_skip != id) {
        /* poly &. method: nil-guard, then re-enter the normal call emission
           on the guarded temp so the method dispatches through the regular
           poly runtime-class switch (a hardcoded whitelist used to drop every
           other method, returning the receiver unchanged -- #3269). The temp
           lives in g_pre so the re-entered dispatch's own hoists can still
           see it, exactly like the object/string arm below.
           The non-nil arm's C type must match the nil arm: when the safe-nav
           result is inferred poly, force the call boxed; for a concretely-typed
           result, emit the natural form and default the nil arm to match. */
        int tsn = ++g_tmp;
        TyKind ret2 = comp_ntype(c, id);
        Buf rsn = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _sn%d = %s; SP_GC_ROOT_RBVAL(_sn%d);\n",
                   tsn, rsn.p ? rsn.p : "sp_box_nil()", tsn);
        free(rsn.p);
        /* The value arm is emitted into its own buffer with g_pre redirected:
           a lowering that hoists STATEMENTS (the comprehension family --
           select, reject, reduce, chunk_while -- builds a loop) would
           otherwise put them in front of the guard, where they run on the very
           nil the guard exists to stop. When that happens the guard becomes a
           statement `if` instead of a ternary. */
        Buf nb; memset(&nb, 0, sizeof nb);
        Buf vb2; memset(&vb2, 0, sizeof vb2);
        Buf preb; memset(&preb, 0, sizeof preb);
        Buf *sv_pre = g_pre;
        Buf *b_sv = b;
        b = &nb;
        /* an array-typed result lowers to a C pointer, whose NULL is nil */
        /* Whether the answer can hold a nil in its own C slot, in which case
           both arms share that slot unboxed. Integer and Float have their
           sentinels, String and the arrays have NULL -- and so does every
           other GC pointer (an Enumerator, a Hash, a user object), which the
           list had stopped short of, so `v&.chunk_while { }` declared an
           sp_RbVal slot while its consumers read an sp_Enumerator *. */
        int sn_gcptr = needs_root(ret2) && ret2 != TY_POLY && ret2 != TY_STRING &&
                       !ty_is_array(ret2) && !ty_is_struct_valued(ret2);
        int sn_ptr = (ret2 == TY_INT || ret2 == TY_FLOAT || ret2 == TY_STRING ||
                      ty_is_array(ret2) || sn_gcptr);
        if (ret2 == TY_INT) buf_puts(b, "SP_INT_NIL");
        else if (ret2 == TY_FLOAT) buf_puts(b, "sp_float_nil()");
        else if (ret2 == TY_STRING) buf_puts(b, "((const char *)NULL)");
        else if (ty_is_array(ret2)) {
          const char *ak = (ret2 == TY_POLY_ARRAY) ? "Poly" : array_kind(ret2);
          buf_printf(b, "((sp_%sArray *)NULL)", ak ? ak : "Poly");
        }
        else if (sn_gcptr) { buf_puts(b, "(("); emit_ctype(c, ret2, b); buf_puts(b, ")NULL)"); }
        else buf_puts(b, "sp_box_nil()");
        b = &vb2;
        g_pre = &preb;
        if (g_n_argov < MAX_ARG_OVERRIDE) {
          int slot2 = view_bind(recv, "_sn%d", tsn);
          int sv_skip = g_sn_skip; g_sn_skip = id;
          /* The call is typed poly but its natural answer may render as a C
             pointer (a poly-hash face answering an array): box the text under
             that type, or the guard's two arms disagree (#4070 follow-up). */
          TyKind natg = ret2 == TY_POLY ? infer_uncached(c, id) : ret2;
          /* A Hash/Enumerable face name answers what it would under the face
             the arm below installs -- ask with the same pin, or the answer
             comes back poly while the value arm renders that face's C type. */
          if (ret2 == TY_POLY && natg == TY_POLY &&
              ty_poly_hash_face_name(nt_str(nt, id, "name"))) {
            int fv = view_push_face(recv, TY_POLY_POLY_HASH);
            TyKind fac = infer_uncached(c, id);
            view_pop(c, fv);
            /* a face that answers another hash boxes
               through a different entry point than emit_boxed_text picks */
            if (ty_is_array(fac) || fac == TY_ENUMERATOR) natg = fac;
          }
          int gbox = (!sn_ptr && ret2 == TY_POLY && natg != TY_POLY &&
                      natg != TY_UNKNOWN && natg != TY_VOID);
          if (sn_ptr) emit_expr(c, id, b);
          else if (gbox) {
            Buf gvb; memset(&gvb, 0, sizeof gvb);
            int vw = view_push(c, id, natg);
            emit_expr(c, id, &gvb);
            view_pop(c, vw);
            emit_boxed_text(c, natg, gvb.p ? gvb.p : "", b);
            free(gvb.p);
          }
          else emit_boxed(c, id, b);
          g_sn_skip = sv_skip;
          view_unbind(g_n_argov - 1);
        }
        else emit_expr(c, recv, b);  /* override table full: degrade to unguarded */
        g_pre = sv_pre;
        b = b_sv;
        if (!preb.p || !preb.p[0]) {
          buf_printf(b, "(_sn%d.tag == SP_TAG_NIL ? %s : (%s))",
                     tsn, nb.p ? nb.p : "sp_box_nil()", vb2.p ? vb2.p : "");
        }
        else {
          int rsv = ++g_tmp;
          emit_indent(g_pre, g_indent);
          /* Both arms are BOXED unless the answer has a C nil of its own, so
             the holding slot is sp_RbVal then -- not ret2, which names what the
             value would be before boxing (a poly-hash answer declared its slot
             sp_PolyPolyHash * and took sp_box_nil()). */
          if (sn_ptr) emit_ctype(c, ret2, g_pre);
          else buf_puts(g_pre, "sp_RbVal");
          buf_printf(g_pre, " _snr%d = %s;\n", rsv, nb.p ? nb.p : "sp_box_nil()");
          emit_indent(g_pre, g_indent);
          if (!sn_ptr) buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_snr%d);\n", rsv);
          else if (ret2 == TY_STRING || ty_is_array(ret2) || sn_gcptr) buf_printf(g_pre, "SP_GC_ROOT(_snr%d);\n", rsv);
          else buf_printf(g_pre, "(void)_snr%d;\n", rsv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (_sn%d.tag != SP_TAG_NIL) {\n", tsn);
          buf_puts(g_pre, preb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "  _snr%d = (%s);\n", rsv, vb2.p ? vb2.p : "");
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
          buf_printf(b, "_snr%d", rsv);
        }
        free(nb.p); free(vb2.p); free(preb.p);
        return 1;
      }
      /* A concretely-typed OBJECT receiver is still a nullable C pointer
         (a nil-able ivar like doom's `@combat&.sprites` after death):
         dropping the `&.` deref'd NULL. The same holds for a concrete
         STRING receiver (NULL is the string nil, e.g. the nil arm of a
         chained `obj&.field&.length`). Emit a guard, then re-enter the
         normal call emission with the receiver substituted by the guarded
         temp (via the arg-override table); g_sn_skip suppresses this block
         on re-entry. */
      int sn_obj = ty_is_object(rrt) && !comp_ty_value_obj(c, rrt);
      /* A specialized container answers a miss with the ELEMENT type's own C
         nil -- a NULL string, SP_INT_NIL -- so `h["zz"]&.empty?` reaches the
         guard with a concrete receiver, not a poly one. Guard those too, or
         the miss takes the result type's zero and `&.` answers false (#4070). */
      int sn_scalar = (rrt == TY_INT || rrt == TY_FLOAT);
      /* a typed array or hash is a pointer too, and a slice past the end
         (`a[4..]&.size`) or a container miss hands it NULL (#4524) */
      int sn_cont = needs_root(rrt) && rrt != TY_POLY && rrt != TY_STRING && !ty_is_object(rrt);
      if ((sn_obj || rrt == TY_STRING || sn_scalar || sn_cont) && g_sn_skip != id) {
        int tsn2 = ++g_tmp;
        TyKind ret2 = comp_ntype(c, id);
        /* The temp lives in g_pre (statement scope), not an inline ({ }):
           the re-entered dispatch hoists its (substituted) receiver into
           g_pre too, which lands before the statement and must still see
           the temp. Rooted: the guarded call's args may allocate. */
        Buf rsn = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        if (sn_obj)
          buf_printf(g_pre, "sp_%s *_sn%d = %s; SP_GC_ROOT(_sn%d);\n",
                     c->classes[ty_object_class(rrt)].c_name, tsn2,
                     rsn.p ? rsn.p : "NULL", tsn2);
        else if (rrt == TY_INT)
          buf_printf(g_pre, "sp_int _sn%d = %s;\n", tsn2, rsn.p ? rsn.p : "SP_INT_NIL");
        else if (rrt == TY_FLOAT)
          buf_printf(g_pre, "sp_float _sn%d = %s;\n", tsn2, rsn.p ? rsn.p : "sp_float_nil()");
        else if (sn_cont) {
          emit_ctype(c, rrt, g_pre);
          buf_printf(g_pre, " _sn%d = %s; SP_GC_ROOT(_sn%d);\n", tsn2, rsn.p ? rsn.p : "NULL", tsn2);
        }
        else
          buf_printf(g_pre, "const char *_sn%d = %s; SP_GC_ROOT_STR(_sn%d);\n",
                     tsn2, rsn.p ? rsn.p : "NULL", tsn2);
        free(rsn.p);
        if (rrt == TY_INT)        buf_printf(b, "(_sn%d == SP_INT_NIL ? ", tsn2);
        else if (rrt == TY_FLOAT) buf_printf(b, "(sp_float_is_nil(_sn%d) ? ", tsn2);
        else                      buf_printf(b, "(_sn%d == NULL ? ", tsn2);
        if (ret2 == TY_POLY) buf_puts(b, "sp_box_nil()");
        else if (ret2 == TY_INT) buf_puts(b, "SP_INT_NIL");
        else if (ret2 == TY_FLOAT) buf_puts(b, "sp_float_nil()");
        else if (ret2 == TY_STRING) buf_puts(b, "((const char *)NULL)");  /* string nil, not "" */
        else buf_puts(b, default_value_from_compiler(c, ret2) ? default_value_from_compiler(c, ret2) : "0");
        buf_puts(b, " : (");
        if (g_n_argov < MAX_ARG_OVERRIDE) {
          int slot2 = view_bind(recv, "_sn%d", tsn2);
          int sv_skip = g_sn_skip; g_sn_skip = id;
          /* infer_type widened a C bool answer to poly so the nil arm has
             somewhere to live; the value arm still renders the bool. Emit it
             under its natural type and box the text. */
          TyKind nat2 = ret2 == TY_POLY ? infer_uncached(c, id) : ret2;
          int sn_box = (ret2 == TY_POLY && nat2 != TY_POLY &&
                        nat2 != TY_UNKNOWN && nat2 != TY_VOID);
          int vw = sn_box ? view_push(c, id, nat2) : -1;
          Buf vb; memset(&vb, 0, sizeof vb);
          emit_expr(c, id, &vb);
          if (vw >= 0) view_pop(c, vw);
          g_sn_skip = sv_skip;
          view_unbind(g_n_argov - 1);
          if (sn_box) emit_boxed_text(c, nat2, vb.p ? vb.p : "", b);
          else buf_puts(b, vb.p ? vb.p : "");
          free(vb.p);
        }
        else emit_expr(c, recv, b);  /* override table full: degrade to unguarded */
        buf_puts(b, "))");
        return 1;
      }
      /* Other concrete receivers -- a by-value struct, a Symbol, an array --
         are never the C nil the guards above test, so the call dispatches as
         normal. It is still typed poly (infer_type widens a `&.` whose answer
         is a C bool, and the receiver's type does not narrow that), and the
         value arm still renders that bool: box it under its natural type or
         the slot it lands in disagrees (#4070). */
      {
        TyKind ret3 = comp_ntype(c, id);
        TyKind nat3 = ret3 == TY_POLY ? infer_uncached(c, id) : ret3;
        if (ret3 == TY_POLY && g_sn_skip != id &&
            nat3 != TY_POLY && nat3 != TY_UNKNOWN && nat3 != TY_VOID) {
          int sv_skip3 = g_sn_skip; g_sn_skip = id;
          int vw = view_push(c, id, nat3);
          Buf vb3; memset(&vb3, 0, sizeof vb3);
          emit_expr(c, id, &vb3);
          view_pop(c, vw);
          g_sn_skip = sv_skip3;
          emit_boxed_text(c, nat3, vb3.p ? vb3.p : "", b);
          free(vb3.p);
          return 1;
        }
      }
    }
  }
  return 0;
}

/* the Object protocol ahead of the builtin arms: Proc#===, initialize_copy, a class's own ! and !=, Float#equal?, each_slice / each_cons over a user Enumerable, Kernel#===, define_singleton_method on a local, the documented limits, a BasicObject instance, itself, and the immediate receivers' CRuby-exact arms */
int emit_call_object_override_arms(Compiler *c, int id, Buf *b, const NodeTable *nt) {
  /* Proc#=== calls the proc; a Proc read out of a container arrives boxed,
     where a value comparison would just answer false (#3683). */
  {
    int pr = nt_ref(nt, id, "receiver");
    const char *pn = nt_str(nt, id, "name");
    int pa = nt_ref(nt, id, "arguments");
    int pc = 0; const int *pv = pa >= 0 ? nt_arr(nt, pa, "arguments", &pc) : NULL;
    if (pr >= 0 && pn && sp_streq(pn, "===") && pc == 1 &&
        (comp_ntype(c, pr) == TY_POLY || comp_ntype(c, pr) == TY_PROC) &&
        !user_defines_or_reads(c, "===")) {
      /* The ANSWER is the proc's return value, not whether it was truthy:
         `->(x){ x * 2 } === 5` is 10 in Ruby, and a case/when only cares
         about the truthiness of that value (#3818). */
      int tp3 = ++g_tmp;
      Buf pb3; memset(&pb3, 0, sizeof pb3);
      buf_printf(&pb3, "({ sp_RbVal _t%d = ", tp3);
      { Buf rb3; memset(&rb3, 0, sizeof rb3); emit_boxed(c, pr, &rb3);
        buf_puts(&pb3, rb3.p ? rb3.p : "sp_box_nil()"); free(rb3.p); }
      buf_printf(&pb3, "; _t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_PROC"
                       " ? sp_penum_call1((sp_Proc *)_t%d.v.p, ", tp3, tp3, tp3);
      { Buf ab3; memset(&ab3, 0, sizeof ab3); emit_boxed(c, pv[0], &ab3);
        buf_puts(&pb3, ab3.p ? ab3.p : "sp_box_nil()");
        /* Method#=== calls the method too, as its `[]` does: compared, a
           Method read out of a container answered false (#6179) */
        if (comp_ntype(c, pr) == TY_POLY) {
          buf_printf(&pb3, ") : _t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_METHOD && _t%d.v.p"
                           " ? sp_poly_call_aref(_t%d, ", tp3, tp3, tp3, tp3);
          buf_puts(&pb3, ab3.p ? ab3.p : "sp_box_nil()");
        }
        /* Everything else dispatches case-equality on the receiver's runtime
           class the way CRuby does -- a Regexp matches, a Range covers, a
           Class tests membership. Plain equality answered false for all of
           them (#3963). */
        buf_printf(&pb3, ") : sp_box_bool(sp_poly_case_eq(_t%d, ", tp3);
        buf_puts(&pb3, ab3.p ? ab3.p : "sp_box_nil()"); free(ab3.p); }
      buf_puts(&pb3, ")); })");
      emit_unbox_text(c, comp_ntype(c, id), pb3.p ? pb3.p : "sp_box_nil()", b);
      free(pb3.p);
      return 1;
    }
  }
  /* Object#initialize_copy: every object inherits it, and the default just
     answers self. A class that defines its own keeps its dispatch (#3753). */
  {
    int ic_r = nt_ref(nt, id, "receiver");
    const char *ic_n = nt_str(nt, id, "name");
    int ic_a = nt_ref(nt, id, "arguments");
    int ic_c = 0; const int *ic_v = ic_a >= 0 ? nt_arr(nt, ic_a, "arguments", &ic_c) : NULL;
    if (ic_r >= 0 && ic_n && sp_streq(ic_n, "initialize_copy") && ic_c == 1 &&
        ty_is_object(comp_ntype(c, ic_r)) &&
        comp_method_in_chain(c, ty_object_class(comp_ntype(c, ic_r)), "initialize_copy", NULL) < 0) {
      buf_puts(b, "((void)("); emit_expr(c, ic_v[0], b); buf_puts(b, "), ");
      emit_expr(c, ic_r, b); buf_puts(b, ")");
      return 1;
    }
  }
  /* `!` and `!=` are ordinary methods a class may override, so dispatch to the
     definition before the default negation takes over (#3740). */
  {
    int nr2 = nt_ref(nt, id, "receiver");
    const char *nn2 = nt_str(nt, id, "name");
    int na2 = nt_ref(nt, id, "arguments");
    int nc2 = 0; const int *nv2 = na2 >= 0 ? nt_arr(nt, na2, "arguments", &nc2) : NULL;
    if (nr2 >= 0 && nn2 && (sp_streq(nn2, "!") || sp_streq(nn2, "!=")) &&
        nc2 == (sp_streq(nn2, "!") ? 0 : 1) && ty_is_object(comp_ntype(c, nr2))) {
      int nd = ty_object_class(comp_ntype(c, nr2)), ndef = nd;
      int nmi2 = comp_method_in_chain(c, nd, nn2, &ndef);
      if (nmi2 >= 0) {
        /* A value-type object is passed BY VALUE (sp_X, not sp_X *): casting
           it to a pointer is not a conversion the C compiler accepts, so a
           class with ivars that defines #! did not build (#3819). */
        int nval = comp_ty_value_obj(c, comp_ntype(c, nr2));
        buf_printf(b, "sp_%s_%s(", c->classes[ndef].c_name, mc(nn2));
        if (!nval) buf_printf(b, "(sp_%s *)", c->classes[ndef].c_name);
        buf_puts(b, "(");
        emit_expr(c, nr2, b);
        buf_puts(b, ")");
        if (nc2 == 1) {
          buf_puts(b, ", ");
          /* The callee's parameter decides the argument's form, as the
             general call path decides it: a parameter widened to poly (a
             `!=` also called with a String, every int slot under
             --int-overflow=promote) takes the boxed value, where this site
             handed it the raw int (#4733). */
          if (c->scopes[nmi2].nparams >= 1) emit_arg_or_default(c, &c->scopes[nmi2], 0, nv2[0], b);
          else emit_expr(c, nv2[0], b);
        }
        buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* Float#equal? is identity, and a NaN is not == to itself, so compare the
     bit patterns rather than the values (#3650). */
  {
    int eqr = nt_ref(nt, id, "receiver");
    const char *eqn = nt_str(nt, id, "name");
    int eqa = nt_ref(nt, id, "arguments");
    int eqc = 0; const int *eqv = eqa >= 0 ? nt_arr(nt, eqa, "arguments", &eqc) : NULL;
    if (eqr >= 0 && eqn && sp_streq(eqn, "equal?") && eqc == 1 && eqv &&
        comp_ntype(c, eqr) == TY_FLOAT && comp_ntype(c, eqv[0]) == TY_FLOAT) {
      int t1 = ++g_tmp, t2 = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = ", t1); emit_expr(c, eqr, b);
      buf_printf(b, "; sp_float _t%d = ", t2); emit_expr(c, eqv[0], b);
      buf_printf(b, "; (sp_bool)(memcmp(&_t%d, &_t%d, sizeof _t%d) == 0); })", t1, t2, t1);
      return 1;
    }
  }
  /* each_slice/each_cons over a user Enumerable: the redirect made this read
     the flat element array, so run it for its side effects and yield the
     original receiver, which is what Ruby returns (#2981). */
  {
    static int self_res_active = -1;
    int sr = nt_int(nt, id, "enum_self_result", -1);
    if (sr >= 0 && self_res_active != id) {
      int prev = self_res_active;
      self_res_active = id;
      /* a receiver that acts is read twice here, under the call and as the
         answer: bind it once (emit_iter_value_expr does the same) */
      int bound = iter_recv_bind_once(c, sr);
      buf_puts(b, "((void)(");
      emit_call(c, id, b);
      buf_puts(b, "), ");
      emit_expr(c, sr, b);
      buf_puts(b, ")");
      if (bound) view_unbind(g_n_argov - 1);
      self_res_active = prev;
      return 1;
    }
  }
  /* An object that does not define #=== inherits Kernel#===, which delegates
     to #==. Both infer as Bool, so the node's cached type stays right when we
     re-dispatch under the #== name (#3018). */
  {
    const char *enm = nt_str(nt, id, "name");
    int erecv = nt_ref(nt, id, "receiver");
    if (enm && sp_streq(enm, "===") && erecv >= 0) {
      int eac = 0; call_args(nt, id, &eac);
      TyKind ert = comp_ntype(c, erecv);
      if (eac == 1 && ty_is_object(ert) &&
          comp_method_in_chain(c, ty_object_class(ert), "===", NULL) < 0) {
        nt_node_set_str((NodeTable *)nt, id, "name", "==");
        emit_call(c, id, b);
        nt_node_set_str((NodeTable *)nt, id, "name", "===");
        return 1;
      }
    }
  }
  /* `obj.extend(Mod)` / `obj.define_singleton_method(:m) { }` on a
     statically-traceable object (its type is a synthesized singleton subclass)
     is done at compile time: the module's methods were transplanted into the
     subclass, and the block was compiled as a method on it. The runtime call
     is a no-op; evaluate the receiver for effect.

     define_singleton_method reached here only through a CONSTANT receiver,
     where the call happens not to be emitted at all. A LOCAL one -- the same
     object, the same synthesized subclass, and `def obj.m` on it works --
     arrived at the unsupported-feature diagnostic and stopped the build. */
  {
    const char *cnm = nt_str(nt, id, "name");
    int crecv = nt_ref(nt, id, "receiver");
    if (cnm && (sp_streq(cnm, "extend") ||
                (sp_streq(cnm, "define_singleton_method") && nt_ref(nt, id, "block") >= 0)) &&
        crecv >= 0) {
      TyKind crt = comp_ntype(c, crecv);
      if ((ty_is_object(crt) && c->classes[ty_object_class(crt)].is_singleton_of) ||
          nt_str(nt, id, "sg_resolved") != NULL) {
        /* The transplant happened at compile time, but WHEN it takes effect is
           a runtime fact: the object carries its parent's cls_id from
           construction and this statement is where Ruby says the override
           starts (#4084). Flip it here; the subclass's methods and is_a? read
           it. A value-type receiver has no identity to flip, so it keeps the
           old no-op. */
        /* the node analyze stamped, not the receiver's type: a local that
           also carries a `def obj.m` widens to poly (the reason sg_resolved
           exists), and then the type says nothing about which subclass */
        int sgci = sg_activates_ci(c, id);
        if (sgci < 0 && ty_is_object(crt) && c->classes[ty_object_class(crt)].is_singleton_of &&
            !c->classes[ty_object_class(crt)].is_value_type)
          sgci = ty_object_class(crt);
        if (sgci >= 0) {
          buf_puts(b, "((void)(("); emit_expr(c, crecv, b);
          buf_printf(b, ")->cls_id = %d))", sgci);
          return 1;
        }
        buf_puts(b, "((void)("); emit_expr(c, crecv, b); buf_puts(b, "))");
        return 1;
      }
    }
  }
  /* A documented limit is reported before anything else looks at the call:
     otherwise an earlier arm reports it as a generic gap (or, for a bare
     `binding`, as a NameError) and the specific message never runs. */
  if (diagnose_unsupported_call(c, id)) return 1;
  /* A blank-slate instance (class X < BasicObject) answers only BasicObject's
     own methods and the user's: everything else is CRuby's NoMethodError, and
     must not fall through to the Object/Kernel default arms (#2703). */
  {
    int bsrecv = nt_ref(nt, id, "receiver");
    TyKind bsrt = bsrecv >= 0 ? comp_ntype(c, bsrecv) : TY_UNKNOWN;
    const char *bsnm = nt_str(nt, id, "name");
    if (bsnm && ty_is_object(bsrt)) {
      int bsci = ty_object_class(bsrt);
      if (bsci >= 0 && class_is_blank_slate(c, bsci) &&
          !basicobject_own_method(bsnm) &&
          comp_method_in_chain(c, bsci, bsnm, NULL) < 0 &&
          !comp_reader_in_chain(c, bsci, bsnm, NULL) &&
          !comp_writer_in_chain(c, bsci, bsnm, NULL)) {
        const char *bscn = class_ruby_name(c, bsci) ? class_ruby_name(c, bsci) : c->classes[bsci].name;
        buf_printf(b, "({ (void)("); emit_expr(c, bsrecv, b);
        buf_printf(b, "); sp_raise_cls(\"NoMethodError\", "
                      "(&(\"\\xff\" \"undefined method '%s' for an instance of %s\")[1])); %s; })",
                   bsnm, bscn, default_value_from_compiler(c, comp_ntype(c, id)));
        return 1;
      }
    }
  }
  /* Object#itself is the receiver for every type (mirrors the general infer
     arm); a user-defined #itself still dispatches normally. */
  {
    const char *nm0 = nt_str(nt, id, "name");
    if (nm0 && sp_streq(nm0, "itself") && nt_ref(nt, id, "receiver") >= 0 &&
        nt_ref(nt, id, "block") < 0) {
      int ac0 = 0; call_args(nt, id, &ac0);
      TyKind irt0 = comp_ntype(c, nt_ref(nt, id, "receiver"));
      if (ac0 == 0 && !diag_user_defines(c, "itself") &&
          /* a generated READER of the name owns it, as in CRuby (#4190) */
          !(ty_is_object(irt0) &&
            comp_resolve_member(c, ty_object_class(irt0), "itself", 0, NULL, NULL) == SP_MEMBER_ATTR)) {
        emit_expr(c, nt_ref(nt, id, "receiver"), b);
        return 1;
      }
    }
  }
  /* Small CRuby-exact arms for immediate receivers (#2732, #2733, #2751,
     #2764), placed ahead of the generic paths that mis-emit them. */
  {
    const char *nm0 = nt_str(nt, id, "name");
    int rv0 = nt_ref(nt, id, "receiver");
    TyKind rt0 = rv0 >= 0 ? comp_ntype(c, rv0) : TY_UNKNOWN;
    int ac0 = 0; const int *av0 = call_args(nt, id, &ac0);
    /* true <=> true is 0; nil <=> nil is 0; any other bool/nil pairing is nil */
    if (nm0 && sp_streq(nm0, "<=>") && ac0 == 1 &&
        (rt0 == TY_BOOL || rt0 == TY_NIL)) {
      TyKind at0 = comp_ntype(c, av0[0]);
      if (rt0 == TY_BOOL && at0 == TY_BOOL) {
        int t1 = ++g_tmp, t2 = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", t1); emit_expr(c, rv0, b);
        buf_printf(b, "; sp_int _t%d = ", t2); emit_expr(c, av0[0], b);
        buf_printf(b, "; (_t%d != 0) == (_t%d != 0) ? sp_box_int(0) : sp_box_nil(); })", t1, t2);
        return 1;
      }
      if (rt0 == TY_NIL && at0 == TY_NIL) {
        buf_puts(b, "((void)("); emit_expr(c, rv0, b); buf_puts(b, "), (void)(");
        emit_expr(c, av0[0], b); buf_puts(b, "), sp_box_int(0))");
        return 1;
      }
      /* a boxed operand can hold the same singleton: `false <=> key`, the
         key a boxed false, is 0 */
      if (at0 == TY_POLY || at0 == TY_UNKNOWN) {
        buf_puts(b, "sp_box_int_or_nil(");
        emit_poly_cmp_ordered(c, "sp_poly_spaceship", rv0, av0[0], b);
        buf_puts(b, ")");
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, rv0, b); buf_puts(b, "), (void)(");
      emit_expr(c, av0[0], b); buf_puts(b, "), sp_box_nil())");
      return 1;
    }
    /* a boolean has no #=~ (so #!~ fails the same way): CRuby's NoMethodError.
       A program that REOPENS TrueClass/FalseClass with its own #=~ keeps it. */
    if (nm0 && (sp_streq(nm0, "=~") || sp_streq(nm0, "!~")) && ac0 == 1 && rt0 == TY_BOOL &&
        !diag_user_defines(c, "=~")) {
      int t1 = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", t1); emit_expr(c, rv0, b);
      buf_printf(b, "; (void)("); emit_expr(c, av0[0], b);
      buf_printf(b, "); sp_raise_cls(\"NoMethodError\", _t%d ?"
                    " (&(\"\\xff\" \"undefined method '=~' for true\")[1])"
                    " : (&(\"\\xff\" \"undefined method '=~' for false\")[1])); 0; })", t1);
      return 1;
    }
    /* TrueClass.new / FalseClass.new / NilClass.new: no allocator */
    if (nm0 && sp_streq(nm0, "new") && rv0 >= 0 && nt_kind(nt, rv0) == NK_ConstantReadNode) {
      const char *cn0 = nt_str(nt, rv0, "name");
      if (cn0 && (sp_streq(cn0, "TrueClass") || sp_streq(cn0, "FalseClass") ||
                  sp_streq(cn0, "NilClass"))) {
        buf_printf(b, "(sp_raise_cls(\"NoMethodError\","
                      " (&(\"\\xff\" \"undefined method 'new' for class %s\")[1])), 0)", cn0);
        return 1;
      }
    }
    /* clone(freeze: false) on an immediate: CRuby's ArgumentError */
    if (nm0 && sp_streq(nm0, "clone") && ac0 == 1 &&
        (rt0 == TY_BOOL || rt0 == TY_NIL || rt0 == TY_INT || rt0 == TY_SYMBOL || rt0 == TY_FLOAT) &&
        nt_type(nt, av0[0]) && sp_streq(nt_type(nt, av0[0]), "KeywordHashNode")) {
      int fv = struct_kwarg_value(c, av0[0], "freeze");
      if (fv >= 0 && nt_type(nt, fv) && sp_streq(nt_type(nt, fv), "FalseNode")) {
        const char *icn = rt0 == TY_NIL ? "NilClass" : rt0 == TY_INT ? "Integer"
                        : rt0 == TY_SYMBOL ? "Symbol" : rt0 == TY_FLOAT ? "Float" : NULL;
        if (icn) {
          buf_printf(b, "({ (void)("); emit_expr(c, rv0, b);
          buf_printf(b, "); sp_raise_cls(\"ArgumentError\","
                        " (&(\"\\xff\" \"can't unfreeze %s\")[1])); %s; })",
                     icn, default_value_from_compiler(c, comp_ntype(c, id)));
          return 1;
        }
        int t1 = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", t1); emit_expr(c, rv0, b);
        buf_printf(b, "; sp_raise_cls(\"ArgumentError\", _t%d ?"
                      " (&(\"\\xff\" \"can't unfreeze TrueClass\")[1])"
                      " : (&(\"\\xff\" \"can't unfreeze FalseClass\")[1])); %s; })",
                   t1, default_value_from_compiler(c, comp_ntype(c, id)));
        return 1;
      }
    }
  }
  return 0;
}

/* printing a value: $stderr.puts / print, and to_s / inspect / nil? on an object no arm above answered */
int emit_call_print_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* $stderr.puts / $stderr.print: emit to stderr */
  if (recv >= 0 && argc >= 0 && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gvnm = nt_str(nt, recv, "name");
    if (gvnm && (sp_streq(gvnm, "$stderr") || sp_streq(gvnm, "$stdout"))) {
      int is_err = gvnm[1] == 's' && gvnm[2] == 't' && gvnm[3] == 'd' && gvnm[4] == 'e';
      const char *fd = is_err ? "stderr" : "stdout";
      if (sp_streq(name, "puts") || sp_streq(name, "print")) {
        int want_nl = sp_streq(name, "puts");
        /* Join with the comma operator so the whole thing stays a single C
           expression -- valid both as a statement and in value position (a
           return/if-else arm). puts adds a newline after each argument. */
        for (int k = 0; k < argc; k++) {
          if (k > 0) buf_puts(b, ", ");
          TyKind at = comp_ntype(c, argv[k]);
          if (at == TY_STRING) { buf_printf(b, "fputs("); emit_expr(c, argv[k], b); buf_printf(b, ", %s)", fd); }
          else if (at == TY_INT) { buf_printf(b, "fprintf(%s, \"%%lld\", (long long)(", fd); emit_expr(c, argv[k], b); buf_puts(b, "))"); }
          else { buf_printf(b, "fputs(sp_poly_to_s("); emit_expr(c, argv[k], b); buf_printf(b, "), %s)", fd); }
          if (want_nl) buf_printf(b, ", fputc('\\n', %s)", fd);
        }
        if (argc == 0 && want_nl) buf_printf(b, "fputc('\\n', %s)", fd);
        return 1;
      }
      if (sp_streq(name, "flush")) { buf_printf(b, "fflush(%s)", fd); return 1; }
      if (sp_streq(name, "write") || sp_streq(name, "syswrite")) {
        /* IO#write: write each arg (stringified), return total bytes written. */
        buf_puts(b, "({ sp_int _w = 0; ");
        for (int k = 0; k < argc; k++) {
          int sk9 = comp_ntype(c, argv[k]) == TY_STRING;
          buf_puts(b, "{ const char *_s = ");
          if (sk9) emit_expr(c, argv[k], b);
          else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[k], b); buf_puts(b, ")"); }
          /* A String's own byte count, so `$stdout.write(bin)` writes what
             `io = $stdout; io.write(bin)` does -- the same method answering
             differently depending on how the receiver is spelled is worse than
             either answer. A stringified value keeps strlen: sp_poly_to_s can
             return an unmarked static class/symbol name. */
          buf_printf(b, "; _w += _s ? (sp_int)fwrite(_s, 1, %s, %s) : 0; } ",
                     sk9 ? "sp_str_byte_len(_s)" : "strlen(_s)", fd);
        }
        buf_puts(b, "_w; })");
        return 1;
      }
    }
  }
  /* Last-resort fallbacks for inspect/to_s on unresolved receivers.
     The test array_unresolved_inspect_no_segv expects "[]" when an
     unsupported method chains into inspect. Emit a safe nil-degrade
     rather than aborting the compiler. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "inspect") && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt)) {
    /* default Object#inspect: the generated per-class ivar walk */
    buf_printf(b, "sp_obj_inspect_sw(%d, (void *)(", ty_object_class(rt));
    emit_expr(c, recv, b); buf_puts(b, "))");
    return 1;
  }
  if (recv >= 0 && argc == 0 && sp_streq(name, "to_s") && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt)) {
    /* default Object#to_s: #<Name:0xADDR> */
    int dcid = ty_object_class(rt);
    const char *drn = class_ruby_name(c, dcid) ? class_ruby_name(c, dcid) : c->classes[dcid].name;
    buf_printf(b, "sp_sprintf(\"#<%s:0x%%016llx>\", (unsigned long long)(uintptr_t)(", drn);
    emit_expr(c, recv, b); buf_puts(b, "))");
    return 1;
  }
  if (recv >= 0 && argc == 0 && (sp_streq(name, "inspect") || sp_streq(name, "to_s")) &&
      (rt == TY_MUTEX || rt == TY_QUEUE || rt == TY_CONDVAR)) {
    emit_handle_inspect(c, recv, rt, b);
    return 1;
  }
  if (recv >= 0 && argc == 0 && (sp_streq(name, "inspect") || sp_streq(name, "to_s")) &&
      (ty_is_ptr_array(rt) || ty_is_obj_array(rt))) {
    buf_puts(b, "sp_poly_inspect("); emit_boxed(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* The nil-degrade placeholders must still emit the receiver: a chain like
     `cell.id.inspect` whose receiver is itself an unresolved call then reaches
     that call's own diagnostic (a compile-time NoMethodError) instead of
     silently printing "[]" with the whole receiver dropped. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "inspect")) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), \"[]\")"); return 1;
  }
  if (recv >= 0 && argc == 0 && sp_streq(name, "to_s")) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), \"\")"); return 1;
  }
  /* nil? on an object type: a value-type object is never nil; a heap object
     reference is nil exactly when its pointer is NULL. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") && ty_is_object(rt)) {
    if (comp_ty_value_obj(c, rt)) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)"); }
    return 1;
  }
  return 0;
}

/* Kernel#display (to_s with no newline, answering nil), and instance_variable_defined? on a statically typed object, answered from its layout */
int emit_call_display_ivar_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* Kernel#display prints to_s with no newline, returns nil */
  if (recv >= 0 && sp_streq(name, "display") && argc == 0 &&
      /* a generated READER of the name owns it, as in CRuby (#4190) */
      !(ty_is_object(comp_ntype(c, recv)) &&
        comp_resolve_member(c, ty_object_class(comp_ntype(c, recv)), name, 0, NULL, NULL) == SP_MEMBER_ATTR)) {
    /* Struct#to_s IS inspect in CRuby ("#<struct Point x=1, y=2>"); the
       boxed sp_poly_to_s default would print the bare-object form */
    TyKind drt2 = comp_ntype(c, recv);
    if (ty_is_object(drt2) && c->classes[ty_object_class(drt2)].is_struct &&
        comp_method_in_chain(c, ty_object_class(drt2), "to_s", NULL) < 0 &&
        comp_method_in_chain(c, ty_object_class(drt2), "inspect", NULL) < 0) {
      buf_printf(b, "((void)fputs(sp_%s_inspect(",
                 c->classes[ty_object_class(drt2)].c_name);
      emit_expr(c, recv, b);
      buf_puts(b, "), stdout))");
      return 1;
    }
    buf_puts(b, "((void)fputs(sp_poly_to_s(");
    emit_boxed(c, recv, b);
    buf_puts(b, "), stdout))");
    return 1;
  }
  /* instance_variable_defined?(:@x / '@x') on a statically-typed object:
     the layout answers at compile time */
  if (recv >= 0 && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      ty_is_object(rt) && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *ivn = sp_streq(nt_type(nt, argv[0]), "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    int dcid = ty_object_class(rt);
    int have = ivn && ivn[0] == '@' && comp_ivar_index(&c->classes[dcid], ivn) >= 0;
    /* one nothing has set yet is not defined (ivar_set_kind) */
    if (have && ivar_set_kind(c, dcid, ivn) == 1) {
      int tro = ++g_tmp;
      char ex[160], tb[256];
      snprintf(ex, sizeof ex, "_t%d->iv_%s", tro, iv_c(ivn + 1));
      buf_printf(b, "({ sp_%s *_t%d = ", c->classes[dcid].c_name, tro); emit_expr(c, recv, b);
      buf_printf(b, "; (sp_bool)%s; })", ivar_set_test(c, dcid, ivn, ex, tb, sizeof tb));
      return 1;
    }
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", have);
    return 1;
  }
  return 0;
}

static int emit_data_ivar_set(Compiler *c, int id, int recv, int value, int cid, Buf *b) {
  const char *dn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
  int td = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", td);
  emit_boxed(c, recv, b);
  buf_puts(b, "; (void)(");
  emit_boxed(c, value, b);
  buf_printf(b, "); sp_raise_frozen_obj(_t%d, (&(\"\\xff\" \"can't modify frozen %s\")[1])); ", td, dn);
  Repr rp = repr_of(c, id);
  TyKind rt9 = rp.as_ty;
  buf_printf(b, "%s; })", rp.kind == RK_BOXED || rp.kind == RK_NONE ? "sp_box_nil()" : default_value(rt9));
  return 1;
}

/* Literal ivar access depends on the class layout and member boundary,
   not just the receiver kind and argument kinds of a builtin row. */
int emit_object_ivar_call(Compiler *c, int id, const char *name, int recv, TyKind rt,
                          int cid, int argc, const int *argv, Buf *b) {
  const NodeTable *nt = c->nt;
  if (is_ivar_access(name) &&
      argc >= 1 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *a0ty = nt_type(nt, argv[0]);
    const char *sym = sp_streq(a0ty, "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    int is_set = sp_streq(name, "instance_variable_set");
    /* Arity is statically known: get takes just the name, set the name and a
       value. A wrong count is a clear diagnostic rather than falling through to
       the misleading by-value-receiver message below. */
    if (is_set && argc != 2) { unsupported(c, id, "instance_variable_set takes exactly 2 arguments"); return 1; }
    if (!is_set && argc != 1) { unsupported(c, id, "instance_variable_get takes exactly 1 argument"); return 1; }
    int is_val = repr_of(c, recv).kind == RK_VOBJ;
    const char *rty = nt_type(nt, recv);
    int recv_lvalue = rty && (sp_streq(rty, "LocalVariableReadNode") ||
                              sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"));
    /* A name without a leading `@` is never a valid ivar name: raise NameError
       at runtime (evaluating the receiver first for its side effects). */
    if (!sym || sym[0] != '@') {
      if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), "); }
      else buf_puts(b, "(");
      buf_printf(b, "sp_raise_cls(\"NameError\", \"'%s' is not allowed as an instance variable name\"), sp_box_nil())",
                 sym ? sym : "");
      return 1;
    }
    if (is_set && c->classes[cid].is_data) return emit_data_ivar_set(c, id, recv, argv[1], cid, b);
    int mi = -1;
    /* Data/Struct members live in the layout but are NOT @-instance
       variables in CRuby: a get answers nil, not the member (#2849) */
    for (int i = c->classes[cid].is_struct ? c->classes[cid].nmembers : 0; i < c->classes[cid].nivars; i++)
      if (sp_streq(c->classes[cid].ivars[i], sym)) { mi = i; break; }
    if (mi >= 0) {
      /* A value object is passed by value, so a field write only sticks when
         the receiver is an lvalue (a local / ivar / self); a pointer object
         can be mutated through any reference. */
      if (is_set && is_val && !recv_lvalue) {
        unsupported(c, id, "instance_variable_set on a by-value object requires an lvalue receiver");
        return 1;
      }
      TyKind mt = c->classes[cid].ivar_types[mi];
      const char *acc = is_val ? "." : "->";
      if (is_set) {
        /* the write is a mutation like any other: a frozen receiver raises
           FrozenError rather than taking it (#3872) */
        if (!is_val && c->classes[cid].freeze_observed) {
          int tf9 = ++g_tmp;
          Buf rbf; memset(&rbf, 0, sizeof rbf); emit_expr(c, recv, &rbf);
          buf_printf(b, "({ sp_%s *_t%d = %s; ", c->classes[cid].c_name, tf9,
                     rbf.p ? rbf.p : "NULL");
          free(rbf.p);
          char selft[32]; snprintf(selft, sizeof selft, "_t%d", tf9);
          emit_frozen_obj_guard(c, cid, selft, b);
          buf_printf(b, "_t%d->iv_%s = ", tf9, iv_c(sym + 1));
          if (mt == TY_POLY) emit_boxed(c, argv[1], b);
          else if (nt_kind(nt, argv[1]) == NK_NilNode && nil_value(mt)) buf_puts(b, nil_value(mt));
          else if (emit_array_into_poly_slot(c, mt, argv[1], b)) { }
          else emit_coerce(c, argv[1], mt, CO_HOLD, "an instance variable write", b);
          buf_puts(b, "; })");
          return 1;
        }
        buf_puts(b, "(("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s = ", acc, iv_c(sym + 1));
        if (mt == TY_POLY) emit_boxed(c, argv[1], b);
        else if (mt == TY_STRBUF) {
          char srefIS[1024];
          if (strbuf_slot_ref(c, argv[1], srefIS, sizeof srefIS)) buf_puts(b, srefIS);
          else {
            buf_puts(b, "sp_String_new_shared(");
            emit_str_expr(c, argv[1], b);
            buf_puts(b, ")");
          }
        }
        /* nil into a scalar slot is its in-band nil (SP_INT_NIL, NaN), not
           the zero value the literal emits as */
        else if (nt_kind(nt, argv[1]) == NK_NilNode && nil_value(mt)) buf_puts(b, nil_value(mt));
        /* a typed array into the general Array slot, rebuilt as an
           `@x = v` write does */
        else if (emit_array_into_poly_slot(c, mt, argv[1], b)) { }
        else emit_coerce(c, argv[1], mt, CO_HOLD, "an instance variable write", b);
        buf_puts(b, ")");
      }
      else if (mt == TY_STRBUF && c->strbuf_handle_demand[id]) {
        /* the caller asked for the HANDLE, not a reading of it. The
           out-of-line reader answers the same way for the same demand;
           inlined, it copied regardless, so `obj.reader.equal?(x)` compared
           two fresh copies and answered false for one object (#4363). */
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", acc, iv_c(sym + 1));
      }
      else if (mt == TY_STRBUF) {
        /* a shared-mutable slot reads out as a GC copy; the raw handle
           must not leak into a plain string context (#3227) */
        int tvG = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = (", tvG);
        emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s; _t%d ? sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1])) : NULL; })",
                   acc, iv_c(sym + 1), tvG, tvG);
      }
      else {
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", acc, iv_c(sym + 1));
      }
      return 1;
    }
    /* A valid `@`-name not in the layout: get reads as nil (CRuby returns nil
       for an unset ivar); set has no field to write under the fixed layout. */
    if (is_set) {
      unsupported(c, id, "instance_variable_set to an ivar absent from the fixed object layout");
      return 1;
    }
    if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())"); }
    else buf_puts(b, "sp_box_nil()");
    return 1;
  }
  return 0;
}
