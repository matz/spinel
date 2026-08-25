#include "codegen_internal.h"

void emit_puts_one(Compiler *c, int arg, Buf *b, int indent) {
  /* a splat argument expands each element as its own argument */
  if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "SplatNode")) {
    int sx = nt_ref(c->nt, arg, "expression");
    if (sx >= 0) {
      emit_indent(b, indent);
      buf_puts(b, "sp_splat_puts(");
      emit_boxed(c, sx, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  arg = unwrap_parens(c, arg);
  /* bare class/module constant: always print the name regardless of value type */
  const char *arg_ty = nt_type(c->nt, arg);
  if (arg_ty && sp_streq(arg_ty, "ConstantReadNode")) {
    const char *arg_nm = nt_str(c->nt, arg, "name");
    if (arg_nm && comp_class_index(c, arg_nm) >= 0 && !comp_const(c, arg_nm)) {
      emit_indent(b, indent);
      buf_printf(b, "puts(\"%s\");\n", arg_nm);
      return;
    }
  }
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  if (t == TY_INT) {
    /* a nullable int at the sentinel prints as nil (an empty line) */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_int _t%d = ", tv); emit_int_expr(c, arg, b);
    buf_printf(b, "; if (_t%d == SP_INT_NIL) putchar('\\n'); else printf(\"%%lld\\n\", (long long)_t%d); }\n", tv, tv);
  }
  else if (t == TY_BIGINT) {
    buf_puts(b, "{ const char *_bs = sp_bigint_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_bs) fputs(_bs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_MATCHDATA) {
    /* puts uses to_s: the full matched substring; nil (NULL) prints blank */
    int tmd = ++g_tmp;
    buf_printf(b, "{ sp_MatchData *_t%d = ", tmd); emit_expr(c, arg, b);
    buf_printf(b, "; puts(_t%d ? sp_MatchData_to_s(_t%d) : \"\"); }\n", tmd, tmd);
  }
  else if (t == TY_RATIONAL) {
    buf_puts(b, "fputs(sp_rational_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_COMPLEX) {
    buf_puts(b, "fputs(sp_complex_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_CURRY) {
    /* a fully-applied curry realizes to its (int) result */
    buf_puts(b, "printf(\"%lld\\n\", (long long)sp_curry_to_int("); emit_expr(c, arg, b);
    buf_puts(b, "));\n");
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_opt_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_ps = (const char *)("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  }
  else if (t == TY_SYMBOL) {
    buf_puts(b, "puts(sp_sym_to_s("); emit_expr(c, arg, b); buf_puts(b, "));\n");
  }
  else if (ty_is_array(t) && array_kind(t)) {
    /* puts [a,b,c] prints each element on its own line (empty array: blank) */
    const char *k = array_kind(t);
    Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, arg, &ab);
    const char *a = ab.p ? ab.p : "";
    int ti = ++g_tmp;
    buf_printf(b, "if (sp_%sArray_length(%s) == 0) putchar('\\n');\n", k, a);
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) ", ti, ti, k, a, ti);
    if (t == TY_INT_ARRAY)
      buf_printf(b, "printf(\"%%lld\\n\", (long long)sp_IntArray_get(%s, _t%d));\n", a, ti);
    else if (t == TY_FLOAT_ARRAY)
      buf_printf(b, "{ const char *_fs = sp_float_to_s(sp_FloatArray_get(%s, _t%d)); fputs(_fs, stdout); putchar('\\n'); }\n", a, ti);
    else /* str */
      buf_printf(b, "{ const char *_ps = sp_StrArray_get(%s, _t%d); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n", a, ti);
    free(ab.p);
  }
  else if (t == TY_EXCEPTION) {
    buf_puts(b, "{ const char *_ps = sp_exc_message("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (t == TY_REGEX) {
    buf_puts(b, "puts(sp_re_to_s_str((void *)("); emit_expr(c, arg, b); buf_puts(b, ")));\n");
    return;
  }
  else if (t == TY_TIME) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Time _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; const char *_ts = sp_time_to_s_v(_t%d); fputs(_ts, stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_RANGE) {
    /* puts of a Range renders its to_s ("first..last"), then a newline. */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_FLOAT_RANGE) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_FloatRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_frange_inspect(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_STR_RANGE) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_StrRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_srange_inspect(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_CLASS) {
    int _tc = ++g_tmp;
    buf_printf(b, "{ sp_Class _cl%d = ", _tc); emit_expr(c, arg, b);
    buf_printf(b, "; puts(sp_class_to_s(_cl%d)); }\n", _tc);
  }
  else if (t == TY_POLY) {
    buf_puts(b, "sp_poly_puts("); emit_expr(c, arg, b); buf_puts(b, ");\n");
  }
  else if (t == TY_POLY_ARRAY) {
    int ta = ++g_tmp, ti = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, arg, &ab);
    buf_printf(b, "{ sp_PolyArray *_t%d = %s;\n", ta, ab.p ? ab.p : "");
    emit_indent(b, indent);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) sp_poly_puts(sp_PolyArray_get(_t%d, _t%d)); }\n",
               ti, ti, ta, ti, ta, ti);
    free(ab.p);
  }
  else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
    /* an object with #to_s (user-defined or a generated struct/data one) */
    const char *cn = obj_str_cname(c, ty_object_class(t), 0);
    int ret_poly = obj_str_ret_poly(c, ty_object_class(t), 0);
    /* A to_s that unions a String with another branch type (e.g. nil)
       returns a boxed sp_RbVal; route it through sp_poly_to_s rather than a
       pointer cast (#3266). */
    buf_puts(b, ret_poly ? "{ const char *_ps = sp_poly_to_s(" : "{ const char *_ps = (const char *)(");
    buf_printf(b, "sp_%s_to_s((sp_%s *)", cn, cn);
    const char *rty = nt_type(c->nt, arg);
    if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode") || sp_streq(rty, "ConstantReadNode"))) {
      emit_expr(c, arg, b);
    }
    else {
      /* evaluate the argument first: its emission may hoist declarations of
         its own into g_pre, which must land as complete statements before
         this temp's declaration head is written */
      int tt = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arg, &rb);
      emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
      buf_printf(g_pre, " _t%d = ", tt);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "_t%d", tt);
    }
    buf_puts(b, ")); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
  }
  else if (ty_is_object(t)) {
    /* default Object#to_s: #<Name:0xADDR>, like CRuby (no ivars) */
    int cid = ty_object_class(t);
    const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
    buf_printf(b, "{ void *_po = (void *)("); emit_expr(c, arg, b);
    buf_printf(b, "); fputs(_po ? sp_sprintf(\"#<%s:0x%%016llx>\", (unsigned long long)(uintptr_t)_po) : \"\", stdout); putchar('\\n'); }\n", rn);
  }
  else if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "ArrayNode") &&
           ({ int _n = 0; nt_arr(c->nt, arg, "elements", &_n); _n == 0; })) {
    buf_puts(b, "(void)0;  /* puts [] prints nothing */\n");
  }
  else if (t == TY_NIL || t == TY_VOID) {
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); putchar('\\n');  /* puts nil */\n");
  }
  else if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "ConstantReadNode") &&
           nt_str(c->nt, arg, "name") && comp_class_index(c, nt_str(c->nt, arg, "name")) >= 0) {
    /* `puts SomeClass` -- a bare class constant renders its name */
    buf_printf(b, "puts(\"%s\");\n", nt_str(c->nt, arg, "name"));
  }
  else if (t == TY_UNKNOWN &&
           nt_type(c->nt, arg) &&
           (sp_streq(nt_type(c->nt, arg), "ConstantReadNode") ||
            sp_streq(nt_type(c->nt, arg), "ConstantPathNode"))) {
    /* unresolved constant: emit the expression which will raise NameError */
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); putchar('\\n');\n");
  }
  else {
    if (!diagnose_eval_call(c, arg) && !diagnose_unsupported_call(c, arg))
      unsupported(c, arg, "puts argument");
  }
}
void emit_print_one(Compiler *c, int arg, Buf *b, int indent) {
  /* a splat argument expands each element as its own argument */
  if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "SplatNode")) {
    int sx = nt_ref(c->nt, arg, "expression");
    if (sx >= 0) {
      emit_indent(b, indent);
      buf_puts(b, "sp_splat_print(");
      emit_boxed(c, sx, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  TyKind t = comp_ntype(c, arg);
  emit_indent(b, indent);
  /* `print n.chr`: write the byte directly. Going through the C-string
     path would drop a NUL byte (the P4/P6 image benchmarks emit those). */
  {
    const NodeTable *nt = c->nt;
    const char *aty = nt_type(nt, arg);
    if (aty && sp_streq(aty, "CallNode") &&
        nt_str(nt, arg, "name") && sp_streq(nt_str(nt, arg, "name"), "chr")) {
      int crecv = nt_ref(nt, arg, "receiver");
      int cargs = nt_ref(nt, arg, "arguments");
      int can = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &can);
      if (crecv >= 0 && can == 0 && comp_ntype(c, crecv) == TY_INT) {
        buf_puts(b, "putchar((int)((");
        emit_expr(c, crecv, b);
        buf_puts(b, ") & 0xff));\n");
        return;
      }
    }
  }
  if (t == TY_INT) {
    buf_puts(b, "printf(\"%lld\", (long long)"); emit_expr(c, arg, b); buf_puts(b, ");\n");
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "fputs(sp_float_to_s("); emit_expr(c, arg, b); buf_puts(b, "), stdout);\n");
  }
  else if (t == TY_STRING) {
    buf_puts(b, "{ const char *_s = ("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_s) fputs(_s, stdout); }\n");
  }
  else if (t == TY_BIGINT) {
    buf_puts(b, "{ const char *_bs = sp_bigint_to_s((sp_Bigint *)("); emit_expr(c, arg, b);
    buf_puts(b, ")); if (_bs) fputs(_bs, stdout); }\n");
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "fputs(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\", stdout);\n");
  }
  else if (t == TY_SYMBOL) {
    buf_puts(b, "fputs(sp_sym_to_s("); emit_expr(c, arg, b); buf_puts(b, "), stdout);\n");
  }
  else if (t == TY_NIL) {
    (void)arg;
  }
  else if (t == TY_VOID) {
    /* a void value (e.g. an always-raising method) printed: evaluate it for
       its effect (it diverges or returns nil); print renders nil as nothing */
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, ");\n");
  }
  else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
    /* an object with #to_s: call it directly (like the puts arm); evaluate
       the argument first so its hoisted declarations land whole in g_pre */
    const char *cn = obj_str_cname(c, ty_object_class(t), 0);
    int ret_poly = obj_str_ret_poly(c, ty_object_class(t), 0);
    /* poly-returning to_s (String on one branch, e.g. nil on another) is a
       boxed sp_RbVal: render via sp_poly_to_s, not a pointer cast (#3266). */
    buf_puts(b, ret_poly ? "{ const char *_ps = sp_poly_to_s(" : "{ const char *_ps = (const char *)(");
    buf_printf(b, "sp_%s_to_s((sp_%s *)", cn, cn);
    const char *rty = nt_type(c->nt, arg);
    if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode") || sp_streq(rty, "ConstantReadNode"))) {
      emit_expr(c, arg, b);
    }
    else {
      int tt = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arg, &rb);
      emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
      buf_printf(g_pre, " _t%d = ", tt);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "_t%d", tt);
    }
    buf_puts(b, ")); if (_ps) fputs(_ps, stdout); }\n");
  }
  else if (t == TY_POLY || ty_is_object(t)) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_RbVal _t%d = ", tv);
    if (t == TY_POLY) emit_expr(c, arg, b); else emit_boxed(c, arg, b);
    buf_printf(b, "; const char *_ps%d = sp_poly_to_s(_t%d); if (_ps%d) fputs(_ps%d, stdout); }\n", tv, tv, tv, tv);
  }
  else if (t == TY_RANGE) {
    /* print of a Range renders its to_s ("first..last"), no newline. */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); }\n", tv);
  }
  else if (t == TY_FLOAT_RANGE) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_FloatRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_frange_inspect(_t%d), stdout); }\n", tv);
  }
  else if (t == TY_STR_RANGE) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_StrRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_srange_to_s(_t%d), stdout); }\n", tv);
  }
  else if (ty_is_array(t) || ty_is_hash(t) || t == TY_EXCEPTION || t == TY_TIME) {
    /* print of a collection renders its to_s (== inspect for Array/Hash);
       box once and go through the poly renderer. */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_RbVal _t%d = ", tv); emit_boxed(c, arg, b);
    buf_printf(b, "; const char *_ps%d = sp_poly_to_s(_t%d); if (_ps%d) fputs(_ps%d, stdout); }\n", tv, tv, tv, tv);
  }
  else {
    /* an empty Array or Hash literal settles at no container variant, so none
       of the arms above claim it; both render as their two-character to_s */
    const char *aty = nt_type(c->nt, arg);
    int en = 0;
    if (aty && (sp_streq(aty, "ArrayNode") || sp_streq(aty, "HashNode")) &&
        (nt_arr(c->nt, arg, "elements", &en), en == 0)) {
      buf_printf(b, "fputs(\"%s\", stdout);\n", sp_streq(aty, "ArrayNode") ? "[]" : "{}");
      return;
    }
    if (!diagnose_eval_call(c, arg) && !diagnose_unsupported_call(c, arg))
      unsupported(c, arg, "print argument");
  }
}
void emit_p_one(Compiler *c, int arg, Buf *b, int indent) {
  /* a splat argument expands each element as its own argument */
  if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "SplatNode")) {
    int sx = nt_ref(c->nt, arg, "expression");
    if (sx >= 0) {
      emit_indent(b, indent);
      buf_puts(b, "sp_splat_p(");
      emit_boxed(c, sx, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  TyKind t = comp_ntype(c, arg);
  /* an element-less hash construct ({} / Hash.new / Hash.new(default)) that no
     key usage narrowed stays TY_UNKNOWN; box it (emit_boxed carries the
     default) and inspect through the poly path. */
  if (t == TY_UNKNOWN && nt_type(c->nt, arg) &&
      (sp_streq(nt_type(c->nt, arg), "HashNode") ||
       (sp_streq(nt_type(c->nt, arg), "CallNode") &&
        nt_str(c->nt, arg, "name") && sp_streq(nt_str(c->nt, arg, "name"), "new")))) {
    Buf hb; memset(&hb, 0, sizeof hb);
    emit_boxed(c, arg, &hb);
    if (hb.p && strstr(hb.p, "sp_PolyPolyHash_new")) {
      emit_indent(b, indent);
      buf_printf(b, "fputs(sp_poly_inspect(%s), stdout); putchar('\\n');\n", hb.p);
      free(hb.p);
      return;
    }
    free(hb.p);
  }
  /* `p Array.new` (no args, no block): a bare Array.new infers TY_UNKNOWN like
     an empty `[]` literal (deferred for push-narrowing), so as a direct p
     argument it never narrows -- print the empty array explicitly. */
  if (t == TY_UNKNOWN && nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "CallNode") &&
      nt_str(c->nt, arg, "name") && sp_streq(nt_str(c->nt, arg, "name"), "new") &&
      nt_ref(c->nt, arg, "block") < 0 &&
      ({ int _r = nt_ref(c->nt, arg, "receiver");
         _r >= 0 && nt_type(c->nt, _r) && sp_streq(nt_type(c->nt, _r), "ConstantReadNode") &&
         nt_str(c->nt, _r, "name") && sp_streq(nt_str(c->nt, _r, "name"), "Array"); }) &&
      ({ int _a = nt_ref(c->nt, arg, "arguments"); int _n = 0;
         if (_a >= 0) nt_arr(c->nt, _a, "arguments", &_n); _n == 0; })) {
    emit_indent(b, indent);
    buf_puts(b, "fputs(\"[]\\n\", stdout);\n");
    return;
  }
  /* `p x.class` prints the class name bare (it is a Class, not a String) --
     unless a member/reader literally named `class` shadows Object#class, in
     which case `x.class` is a genuine String and `p` quotes it (#2975). */
  if (t == TY_STRING && nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "CallNode") &&
      nt_str(c->nt, arg, "name") && sp_streq(nt_str(c->nt, arg, "name"), "class") &&
      nt_ref(c->nt, arg, "receiver") >= 0 &&
      !({ int _prc = nt_ref(c->nt, arg, "receiver"); TyKind _prt = comp_ntype(c, _prc);
          ty_is_object(_prt) && ty_object_class(_prt) >= 0 &&
          comp_reader_in_chain(c, ty_object_class(_prt), "class", NULL); })) {
    emit_indent(b, indent);
    buf_puts(b, "fputs("); emit_expr(c, arg, b); buf_puts(b, ", stdout); putchar('\\n');\n");
    return;
  }
  emit_indent(b, indent);
  if (t == TY_INT) {
    /* p of a nullable int at the sentinel prints "nil" */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_int _t%d = ", tv); emit_int_expr(c, arg, b);
    buf_printf(b, "; if (_t%d == SP_INT_NIL) fputs(\"nil\\n\", stdout); else printf(\"%%lld\\n\", (long long)_t%d); }\n", tv, tv);
  }
  else if (t == TY_FLOAT) {
    buf_puts(b, "{ const char *_fs = sp_float_opt_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_fs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_STRING) {
    /* a nullable string (NULL) prints "nil" */
    int tv = ++g_tmp;
    buf_printf(b, "{ const char *_t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(_t%d ? sp_str_inspect(_t%d) : \"nil\", stdout); putchar('\\n'); }\n", tv, tv);
  }
  else if (t == TY_BOOL) {
    buf_puts(b, "puts(("); emit_expr(c, arg, b); buf_puts(b, ") ? \"true\" : \"false\");\n");
  }
  else if (t == TY_SYMBOL) {
    buf_puts(b, "fputs(sp_sym_inspect(");
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_EXCEPTION) {
    /* p of an exception inspects as #<ClassName: message>; a NULL receiver
       (nil $! outside a rescue) prints "nil". */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Exception *_t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(_t%d ? sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d)) : \"nil\", stdout); putchar('\\n'); }\n", tv, tv, tv);
  }
  else if (ty_is_array(t) && array_kind(t)) {
    buf_printf(b, "fputs(sp_%sArray_inspect(", array_kind(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (ty_is_hash(t) && ty_hash_cname(t)) {
    buf_printf(b, "fputs(sp_%sHash_inspect(", ty_hash_cname(t));
    emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_POLY_ARRAY) {
    buf_puts(b, "fputs(sp_PolyArray_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_POLY) {
    buf_puts(b, "fputs(sp_poly_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_UNKNOWN) {
    /* An unresolved call (`OpenStruct.new(..)` without `require "ostruct"`, or
       any `undefined.method`) has no static type -- but it emits a diverging
       NoMethodError/NameError raise (the NoMethodError gate), so it must still
       compile in argument position, not just as a statement. Box it (emit_boxed
       evaluates the raise, then yields nil) and inspect; the print never runs
       because the raise unwinds first (#3135 without require). */
    buf_puts(b, "fputs(sp_poly_inspect("); emit_boxed(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_COMPLEX) {
    buf_puts(b, "fputs(sp_complex_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_RATIONAL) {
    buf_puts(b, "fputs(sp_rational_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_REGEX) {
    buf_puts(b, "puts(sp_re_inspect_str((void *)("); emit_expr(c, arg, b); buf_puts(b, ")));\n");
    return;
  }
  else if (t == TY_MATCHDATA) {
    /* p and inspect coincide for MatchData; nil (NULL) prints nil */
    int tm2 = ++g_tmp;
    buf_printf(b, "{ sp_MatchData *_t%d = ", tm2); emit_expr(c, arg, b);
    buf_printf(b, "; puts(_t%d ? sp_MatchData_inspect(_t%d) : \"nil\"); }\n", tm2, tm2);
    return;
  }
  else if (t == TY_TIME) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Time _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_time_inspect_v(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_RANGE) {   /* a Range inspects as "first..last" / "first...last" */
    /* A string-endpoint range has no int sp_Range value; inspect its literal
       endpoints directly (each String quoted), e.g. `"a".."c"`. */
    int rn = unwrap_parens(c, arg);
    if (rn >= 0 && nt_type(c->nt, rn) && sp_streq(nt_type(c->nt, rn), "RangeNode")) {
      int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
      if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
        int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
        buf_puts(b, "fputs(sp_sprintf(\"%s");
        buf_puts(b, excl ? "..." : "..");
        buf_puts(b, "%s\", sp_str_inspect("); emit_expr(c, lo, b);
        buf_puts(b, "), sp_str_inspect("); emit_expr(c, hi, b);
        buf_puts(b, ")), stdout); putchar('\\n');\n");
        return;
      }
    }
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_FLOAT_RANGE) {   /* a Float range inspects as "1.0..3.0" */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_FloatRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_frange_inspect(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_STR_RANGE) {   /* a String range inspects as "\"a\"..\"e\"" */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_StrRange _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_srange_inspect(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_CLASS) {   /* a Class/Module inspects as its name, except a
                                 keyword-init Struct class, which carries the
                                 `(keyword_init: true)` suffix (#3947) */
    int cv = ++g_tmp;
    buf_printf(b, "{ sp_Class _t%d = ", cv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_class_inspect_name(_t%d), stdout); putchar('\\n'); }\n", cv);
  }
  else if (t == TY_BIGINT) {
    /* Integer#inspect == #to_s, so a bignum prints the same as puts/print. */
    buf_puts(b, "{ const char *_bs = sp_bigint_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_bs) fputs(_bs, stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_NIL || t == TY_VOID) {
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); fputs(\"nil\\n\", stdout);\n");
  }
  else if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "ArrayNode") &&
           ({ int _n = 0; nt_arr(c->nt, arg, "elements", &_n); _n == 0; })) {
    buf_puts(b, "fputs(\"[]\\n\", stdout);\n");  /* p [] */
  }
  else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 1)) {
    /* an object with #inspect (user-defined or a generated struct/data one);
       a NULL pointer is nil (nullable-object convention, e.g. Set#add?) */
    const char *cn = obj_str_cname(c, ty_object_class(t), 1);
    int pv = ++g_tmp;
    buf_printf(b, "{ sp_%s *_t%d = (sp_%s *)(", cn, pv, cn); emit_expr(c, arg, b);
    buf_printf(b, "); fputs(_t%d ? sp_%s_inspect(_t%d) : \"nil\", stdout); putchar('\\n'); }\n", pv, cn, pv);
  }
  else if (t == TY_PROC) {
    buf_puts(b, "{ sp_Proc *_pp = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(sp_proc_inspect(_pp), stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_CURRY) {
    /* a curried proc reports as a Proc, lambda-ness from its source */
    buf_puts(b, "{ sp_Curry *_pc = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_pc ? sp_sprintf(_pc->target && _pc->target->lambda_p ?"
              " \"#<Proc:0x%016llx (lambda)>\" : \"#<Proc:0x%016llx>\","
              " (unsigned long long)(uintptr_t)_pc) : \"nil\", stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_METHOD) {
    /* the stamped #<Method: ...> rendering; a NULL (nil super_method) prints nil */
    buf_puts(b, "{ sp_BoundMethod *_pm = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_pm ? sp_method_desc_cstr(_pm) : \"nil\", stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_ENUMERATOR) {
    buf_puts(b, "{ sp_Enumerator *_pe = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(sp_enum_inspect(_pe), stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_IO) {
    /* a nullable handle: the readiness family answers nil on timeout */
    int iv = ++g_tmp;
    buf_printf(b, "{ sp_File *_t%d = (", iv); emit_expr(c, arg, b);
    buf_printf(b, "); fputs(_t%d ? sp_File_inspect(_t%d) : \"nil\", stdout); putchar('\\n'); }\n", iv, iv);
  }
  else if (t == TY_RANDOM) {
    buf_puts(b, "{ sp_Random *_pr = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(sp_Random_inspect(_pr), stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_DIR) {
    /* #<Dir:PATH>, the same rendering Dir#inspect emits (#3395: reachable now
       that each/each_entry answer the receiver rather than nil) */
    int dv = ++g_tmp;
    buf_printf(b, "{ sp_Dir *_t%d = (", dv); emit_expr(c, arg, b);
    buf_printf(b, "); if (_t%d) { const char *_dp = sp_Dir_path(_t%d);"
                  " printf(\"#<Dir:%%s>\\n\", _dp ? _dp : \"\"); }"
                  " else fputs(\"nil\\n\", stdout); }\n", dv, dv);
  }
  else if (t == TY_OPENSTRUCT) {
    buf_puts(b, "{ sp_OpenStruct *_po = ("); emit_expr(c, arg, b);
    buf_puts(b, "); fputs(_po ? sp_OpenStruct_inspect(_po) : \"nil\", stdout); putchar('\\n'); }\n");
  }
  else if (t == TY_EXCEPTION) {
    /* boxed-path inspect: NULL prints nil, else #<Class: message> */
    int ev = ++g_tmp;
    buf_printf(b, "{ sp_Exception *_t%d = (sp_Exception *)(", ev); emit_expr(c, arg, b);
    buf_printf(b, "); fputs(_t%d ? sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d)) : \"nil\", stdout); putchar('\\n'); }\n", ev, ev, ev);
  }
  else if (ty_is_object(t)) {
    /* p obj: a user #inspect wins; otherwise the generated per-class ivar
       walk renders CRuby's default #<Name:0xADDR @a=..., ...> */
    int cid = ty_object_class(t);
    const char *icn = obj_str_cname(c, cid, 1);
    if (icn) {
      buf_printf(b, "{ const char *_pi = sp_%s_inspect((sp_%s *)(", icn, icn);
      emit_expr(c, arg, b);
      buf_puts(b, ")); fputs(_pi ? _pi : \"nil\", stdout); putchar('\\n'); }\n");
    }
    else {
      buf_printf(b, "{ void *_po = (void *)("); emit_expr(c, arg, b);
      buf_printf(b, "); fputs(_po ? sp_obj_inspect_sw(%d, _po) : \"nil\", stdout); putchar('\\n'); }\n", cid);
    }
  }
  else {
    if (!diagnose_eval_call(c, arg) && !diagnose_unsupported_call(c, arg))
      unsupported(c, arg, "p argument");
  }
}

int emit_output_call(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv >= 0) return 0;
  /* a user method shadows the builtin: the enclosing class's own chain first
     (it sits above Kernel), then the top-level table (Object) */
  if (bare_call_class_owned(c, id)) return 0;
  if (comp_method_index(c, name) >= 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  if (sp_streq(name, "puts")) {
    if (argc == 0) { emit_indent(b, indent); buf_puts(b, "putchar('\\n');\n"); return 1; }
    for (int k = 0; k < argc; k++) emit_puts_one(c, argv[k], b, indent);
    return 1;
  }
  if (sp_streq(name, "print")) { for (int k = 0; k < argc; k++) emit_print_one(c, argv[k], b, indent); return 1; }
  if (sp_streq(name, "p") || sp_streq(name, "pp")) { for (int k = 0; k < argc; k++) emit_p_one(c, argv[k], b, indent); return 1; }
  if (sp_streq(name, "putc") && argc == 1) {
    /* Kernel#putc: an int writes (byte & 0xff); a string writes its first char. */
    TyKind at = comp_ntype(c, argv[0]);
    emit_indent(b, indent);
    if (at == TY_STRING) {
      int ts = ++g_tmp;
      buf_printf(b, "{ const char *_t%d = ", ts); emit_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d && *_t%d) putchar((unsigned char)_t%d[0]); }\n", ts, ts, ts);
    }
else if (at == TY_POLY) {
      buf_puts(b, "putchar((int)(sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ") & 0xff));\n");
    }
else {
      buf_puts(b, "putchar((int)(("); emit_expr(c, argv[0], b); buf_puts(b, ") & 0xff));\n");
    }
    return 1;
  }
  if (sp_streq(name, "system") && argc >= 1) {
    int ts = ++g_tmp;
    emit_indent(b, indent);
    buf_printf(b, "{ const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
    buf_printf(b, ", NULL }; sp_system_args(%d, _sys_%d); }\n", argc, ts);
    return 1;
  }
  if (sp_streq(name, "printf") && argc >= 1) {
    /* A conversion C's printf does not have (%b/%B binary: glibc 2.35+ only,
       a literal 'b' elsewhere) routes through the Ruby formatter (the same
       engine String#% / format use), which renders it portably. */
    {
      /* Every conversion goes through the Ruby formatter -- the same engine
         String#% and format use. Handing the arguments to C's printf meant a
         Symbol or nil reached %s as a raw slot and printed `(null)` or an
         address, and %p / %<name> / %{name} have no C meaning at all. */
      {
        int tpa = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "{ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tpa, tpa);
        for (int k = 1; k < argc; k++) {
          Buf ab; memset(&ab, 0, sizeof ab);
          emit_boxed(c, argv[k], &ab);
          emit_indent(b, indent);
          /* a splat contributes its ELEMENTS, not one array argument (#3957) */
          if (nt_kind(nt, argv[k]) == NK_SplatNode)
            buf_printf(b, "  sp_PolyArray_append_all(_t%d, sp_poly_to_poly_array(%s));\n",
                       tpa, ab.p ? ab.p : "sp_box_nil()");
          else
            buf_printf(b, "  sp_PolyArray_push(_t%d, %s);\n", tpa, ab.p ? ab.p : "sp_box_nil()");
          free(ab.p);
        }
        emit_indent(b, indent);
        buf_puts(b, "  fputs(sp_str_format_polyarr(");
        emit_expr(c, argv[0], b);
        buf_printf(b, ", _t%d), stdout); }\n", tpa);
        return 1;
      }
    }
    /* Kernel#printf: printf(fmt, args...) with %d/%i/%x/%o/%u rewritten to ll forms */
    emit_indent(b, indent);
    buf_puts(b, "printf(");
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "StringNode")) {
      const char *lit = nt_str(nt, argv[0], "unescaped");
      if (!lit) lit = nt_str(nt, argv[0], "content");
      if (!lit) lit = "";
      /* Build a rewritten format string with ll-qualified integer specifiers */
      Buf fmtb; memset(&fmtb, 0, sizeof fmtb);
      for (const char *p = lit; *p; ) {
        if (*p == '%') {
          buf_puts(&fmtb, "%"); p++;
          while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' ||
                 (*p >= '1' && *p <= '9') || *p == '.') { buf_printf(&fmtb, "%c", (unsigned char)*p); p++; }
          if (*p == 'd' || *p == 'i') { buf_puts(&fmtb, "lld"); p++; }
          else if (*p == 'x') { buf_puts(&fmtb, "llx"); p++; }
          else if (*p == 'X') { buf_puts(&fmtb, "llX"); p++; }
          else if (*p == 'o') { buf_puts(&fmtb, "llo"); p++; }
          else if (*p == 'u') { buf_puts(&fmtb, "llu"); p++; }
          else if (*p) { buf_printf(&fmtb, "%c", (unsigned char)*p); p++; }
        }
        else { buf_printf(&fmtb, "%c", (unsigned char)*p); p++; }
      }
      buf_puts(b, "\"");
      emit_c_escaped(b, fmtb.p ? fmtb.p : "");
      buf_puts(b, "\"");
      free(fmtb.p);
    }
    else { emit_expr(c, argv[0], b); }
    for (int k = 1; k < argc; k++) {
      buf_puts(b, ", ");
      TyKind at = comp_ntype(c, argv[k]);
      if (at == TY_INT) { buf_puts(b, "(long long)"); emit_expr(c, argv[k], b); }
      else emit_expr(c, argv[k], b);
    }
    buf_puts(b, ");\n");
    return 1;
  }

  if (sp_streq(name, "exit") || sp_streq(name, "exit!")) {
    /* exit raises a rescuable SystemExit (#2761); exit! terminates directly.
       A boolean status maps true -> 0, false -> 1, as in CRuby. */
    const char *xfn = sp_streq(name, "exit!") ? "exit" : "sp_exit_raise";
    emit_indent(b, indent);
    if (argc == 0) buf_printf(b, "%s(0);\n", xfn);
    else {
      /* a poly status (e.g. a widened attr read or poly-hash get) must be
         unboxed -- (int)(sp_RbVal) is a struct cast, a cc error. */
      TyKind xt = comp_ntype(c, argv[0]);
      if (xt == TY_POLY) { buf_printf(b, "%s((int)sp_poly_to_i(", xfn); emit_expr(c, argv[0], b); buf_puts(b, "));\n"); }
      else if (xt == TY_BOOL) { buf_printf(b, "%s((", xfn); emit_expr(c, argv[0], b); buf_puts(b, ") ? 0 : 1);\n"); }
      else { buf_printf(b, "%s((int)(", xfn); emit_expr(c, argv[0], b); buf_puts(b, "));\n"); }
    }
    return 1;
  }
  if (sp_streq(name, "abort")) {
    /* abort raises a rescuable SystemExit(1) after writing to stderr (#3077) */
    emit_indent(b, indent);
    if (argc >= 1) {
      buf_puts(b, "sp_abort_raise(");
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_STRING) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ");\n");
    }
    else buf_puts(b, "sp_abort_raise((const char *)0);\n");
    return 1;
  }
  if (sp_streq(name, "srand")) {
    emit_indent(b, indent);
    if (argc == 0) buf_puts(b, "(void)sp_kernel_srand((sp_int)time(NULL));\n");
    else { buf_puts(b, "(void)sp_kernel_srand("); emit_int_expr_conv(c, argv[0], b); buf_puts(b, ");\n"); }
    return 1;
  }
  if (sp_streq(name, "rand") && argc >= 1) {
    /* stmt-level rand: evaluate for side effects; result unused. An endless/
       beginless range still raises Errno::EDOM (#2544), so keep that raise. */
    const char *raty = nt_type(nt, argv[0]);
    if (raty && sp_streq(raty, "RangeNode") &&
        (nt_ref(nt, argv[0], "left") < 0 || nt_ref(nt, argv[0], "right") < 0)) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"Errno::EDOM\", \"Domain error - rand\");\n");
      return 1;
    }
    /* a non-finite Float bound still raises FloatDomainError even when the
       result is unused (#3049) */
    if (comp_ntype(c, argv[0]) == TY_FLOAT) {
      int tf = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ double _t%d = ", tf); emit_float_expr(c, argv[0], b);
      buf_printf(b, "; if (!isfinite(_t%d)) sp_raise_cls(\"FloatDomainError\","
                    " isnan(_t%d) ? \"NaN\" : \"Infinity\"); }\n", tf, tf);
      return 1;
    }
    emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, ");\n");
    return 1;
  }
  if (sp_streq(name, "warn")) {
    /* Kernel#warn(*msgs, uplevel:, category:): positional messages go to stderr
       with a trailing newline. The trailing keyword hash carries options:
         - a forwarded `**opts` (AssocSplat) is an options bundle: evaluate the
           splat values for side effects and otherwise ignore them;
         - `category:` selects a warning category. CRuby suppresses the
           `:deprecated` category by default (Warning[:deprecated] == false with
           no -W:deprecated), so a literal `category: :deprecated` prints nothing
           (messages are still evaluated for side effects); other categories print
           normally;
         - `uplevel:` prefixes each line with the caller's source location, which
           needs a runtime line-granularity call stack spinel does not have.
           Emitting any prefix would be a wrong location, so it loud-rejects. */
    int kw_idx = -1, suppress = 0;
    if (argc > 0) {
      int last = argv[argc - 1];
      const char *lt = nt_type(c->nt, last);
      if (lt && sp_streq(lt, "KeywordHashNode")) {
        kw_idx = argc - 1;
        int en = 0; const int *elems = nt_arr(c->nt, last, "elements", &en);
        for (int e = 0; e < en; e++) {
          const char *ety = nt_type(c->nt, elems[e]);
          if (ety && sp_streq(ety, "AssocSplatNode")) {
            int val = nt_ref(c->nt, elems[e], "value");
            if (val >= 0) { emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, val, b); buf_puts(b, ");\n"); }
            continue;
          }
          int key = nt_ref(c->nt, elems[e], "key");
          int val = nt_ref(c->nt, elems[e], "value");
          const char *kty = key >= 0 ? nt_type(c->nt, key) : NULL;
          const char *kname = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(c->nt, key, "value") : NULL;
          if (kname && sp_streq(kname, "uplevel")) {
            unsupported(c, elems[e], "warn(uplevel:) caller-location prefix (no runtime source-line stack)");
          }
          else if (kname && sp_streq(kname, "category")) {
            const char *vty = val >= 0 ? nt_type(c->nt, val) : NULL;
            const char *vname = (vty && sp_streq(vty, "SymbolNode")) ? nt_str(c->nt, val, "value") : NULL;
            if (vname && sp_streq(vname, "deprecated"))
              suppress = 1;
            else if (val >= 0) { emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, val, b); buf_puts(b, ");\n"); }
          }
          else if (val >= 0) { emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, val, b); buf_puts(b, ");\n"); }
        }
      }
    }
    /* When the program reassigns `$stderr` to a StringIO, redirect warn to it
       at runtime (gv_stderr is NULL until reassigned, so the default path still
       writes to the real stderr) (#3113). */
    LocalVar *serr = comp_gvar(c, "stderr");
    int redirect = serr && ty_is_object(serr->type) && ty_object_class(serr->type) >= 0 &&
                   c->classes[ty_object_class(serr->type)].c_name &&
                   sp_streq(c->classes[ty_object_class(serr->type)].c_name, "StringIO");
    for (int k = 0; k < argc; k++) {
      if (k == kw_idx) continue;
      if (suppress) { emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, argv[k], b); buf_puts(b, ");\n"); continue; }
      TyKind at = comp_ntype(c, argv[k]);
      if (redirect) {
        /* stringify into a temp, then branch on the live $stderr redirect */
        int wt = ++g_tmp;
        emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", wt);
        if (at == TY_STRING) emit_expr(c, argv[k], b);
        else if (at == TY_INT) { buf_puts(b, "sp_int_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else if (at == TY_FLOAT) { buf_puts(b, "sp_float_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else if (at == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[k], b); buf_puts(b, ")"); }
        buf_puts(b, ";\n");
        emit_indent(b, indent);
        buf_printf(b, "if (gv_stderr) { sp_StringIO_write(gv_stderr, _t%d); sp_StringIO_write(gv_stderr, \"\\n\"); }"
                      "\nelse { fputs(_t%d, stderr); fputc('\\n', stderr); }\n", wt, wt);
        continue;
      }
      /* every message renders like puts: an Array writes one line per element,
         anything else its to_s, and a message already ending in a newline does
         not get a second one (#3728) */
      (void)at;
      emit_indent(b, indent); buf_puts(b, "sp_poly_warn_line(");
      emit_boxed(c, argv[k], b);
      buf_puts(b, ", stderr);\n");
    }
    return 1;
  }
  return 0;
}

/* ---- assignment ---- */

/* A celled/captured TY_PROC local stores its pointer int-laundered in the cell
   as (sp_int)(uintptr_t)sp_Proc*. emit_local_ref renders the READ form
   ((sp_Proc *)(uintptr_t)(*_cell_x)), which is not an assignable lvalue. When
   writing such a local we need the raw cell deref instead; this emits it and
   opens the `= (sp_int)(uintptr_t)(` re-encoding, leaving the caller to emit
   the value expression and a closing `)`. Returns 1 if it handled the lvalue
   (proc cell), 0 if the local is not a laundered proc cell and the caller
   should fall back to `emit_local_ref = value`. */
/* The C expression for a slot of type `t` holding Ruby nil: the type's own nil
   sentinel, or its default value otherwise. */
static const char *nil_sentinel(TyKind t) {
  switch (t) {
    case TY_INT:    return "SP_INT_NIL";
    case TY_FLOAT:  return "sp_float_nil()";
    case TY_POLY:   return "sp_box_nil()";
    case TY_STRING: return "NULL";
    default:        return default_value(t);
  }
}
static int emit_proc_cell_lvalue(Compiler *c, int scope_node, const char *nm, Buf *b) {
  LocalVar *lv = nm ? scope_local(comp_scope_of(c, scope_node), nm) : NULL;
  if (!lv || lv->type != TY_PROC) return 0;
  int captured = g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm);
  if (!lv->is_cell && !captured) return 0;
  if (captured) buf_printf(b, "*((%s *)_cap)->c_%s", g_cap_struct, nm);
  else buf_printf(b, "*_cell_%s", nm);
  buf_puts(b, " = (sp_int)(uintptr_t)(");
  return 1;
}

/* `Array.new(n) { ... }`: a generator that builds its container in place, so
   the container's C form is free to follow the destination rather than the
   node's own type. That matters wherever a pointer-array narrowing is a
   decision about a SLOT (a local, a method's return) which never reached the
   node the value is emitted from. */
static int is_array_new_block(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
  if (!vty || !sp_streq(vty, "CallNode")) return 0;
  if (nt_ref(nt, v, "block") < 0) return 0;
  const char *cn = nt_str(nt, v, "name");
  if (!cn || !sp_streq(cn, "new")) return 0;
  int r = nt_ref(nt, v, "receiver");
  if (r < 0 || !nt_type(nt, r) || !sp_streq(nt_type(nt, r), "ConstantReadNode")) return 0;
  const char *rn = nt_str(nt, r, "name");
  return rn && sp_streq(rn, "Array");
}

/* Emit a value that BUILDS its container into a narrowed pointer array (an
   object array, a table of int arrays). Every shape here constructs in place,
   so the container's C form follows the destination -- which matters because
   the narrowing is a decision about a SLOT (a local, an ivar, a method's
   return) and never reached the node this value is emitted from. Answers 0
   when the value is not one of those shapes and the caller should emit it its
   usual way. */
static int emit_ptr_array_build(Compiler *c, int v, TyKind want, Buf *b) {
  const char *vty = v >= 0 ? nt_type(c->nt, v) : NULL;
  if (!vty) return 0;
  if (sp_streq(vty, "ArrayNode")) {
    /* `[X.new, ...]` / `[[..], [..]]`: an sp_PtrArray of the unboxed element
       pointers (each an object pointer or an sp_IntArray*), rooted while
       constructing. An empty one is just the empty array. */
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);", t, t);
    int en = 0; const int *el = nt_arr(c->nt, v, "elements", &en);
    for (int e = 0; e < en; e++) {
      buf_printf(b, " sp_PtrArray_push(_t%d, ", t); emit_expr(c, el[e], b); buf_puts(b, ");");
    }
    buf_printf(b, " _t%d; })", t);
    return 1;
  }
  if (is_array_new_block(c, v)) {
    TyKind sv = c->ntype[v];
    c->ntype[v] = want;
    emit_expr(c, v, b);
    c->ntype[v] = sv;
    return 1;
  }
  return 0;
}

static int str_append_chain_base(Compiler *c, int id);
void emit_assign(Compiler *c, int id, Buf *b, int indent) {
  const char *nm = nt_str(c->nt, id, "name");
  int v = nt_ref(c->nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  /* A local holding a lazy chain (`p = src.lazy.select{}`) has no runtime
     value; when every use forces it (first(n)/to_a/force) each force site
     fuses the chain via lazy_alias_chain, so skip the broken assignment here
     (the variable keeps its nil default, unread). (#2932) */
  if (lazy_alias_write_suppressible(c, id)) return;
  /* `x = y = nil`: emit the inner writes as their own statements (each target
     renders nil for its own slot type), then write nil here too. */
  {
    int ncb = comp_nil_chain_bottom(c->nt, v);
    if (ncb >= 0) { emit_stmt_inner(c, v, b, indent); v = ncb; }
  }
  emit_indent(b, indent);
  /* A TY_PROC value lives in an int cell as (sp_int)(uintptr_t)sp_Proc*. The
     write target must be the raw cell deref (an lvalue) with the pointer
     re-encoded as int; emit_local_ref's read form casts to sp_Proc* and is not
     assignable (self-recursive `f = proc { f.call(...) }`). A heap-object cell
     is a typed pointer whose deref is already assignable, so it takes the
     ordinary `emit_local_ref = value` path below. */
  int laundered_cell = lv && lv->type == TY_PROC;
  if (laundered_cell &&
      (lv->is_cell || (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm)))) {
    if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm))
      buf_printf(b, "*((%s *)_cap)->c_%s", g_cap_struct, nm);
    else
      buf_printf(b, "*_cell_%s", nm);
    buf_puts(b, " = (sp_int)(uintptr_t)(");
    const char *pvty = nt_type(c->nt, v);
    if (pvty && sp_streq(pvty, "NilNode")) buf_puts(b, "NULL");
    else emit_expr(c, v, b);
    buf_puts(b, ");\n");
    return;
  }
  emit_local_ref(c, id, nm, b);
  buf_puts(b, " = ");
  /* `x = nil` -> the variable's type-appropriate default */
  const char *vty = nt_type(c->nt, v);
  int vn = 0;
  int is_empty_array = vty && sp_streq(vty, "ArrayNode") && (nt_arr(c->nt, v, "elements", &vn), vn == 0);
  /* `[].dup` / `[].clone`: a copy of nothing is a new empty array, and it must
     be built at the target's type. Emitted as a call it took the literal's own
     default kind and wrote an sp_IntArray into a poly-array slot (#3608). */
  int empty_array_frozen = 0;
  if (!is_empty_array && vty && sp_streq(vty, "CallNode") &&
      nt_str(c->nt, v, "name") &&
      (sp_streq(nt_str(c->nt, v, "name"), "dup") || sp_streq(nt_str(c->nt, v, "name"), "clone") ||
       sp_streq(nt_str(c->nt, v, "name"), "freeze")) &&
      nt_ref(c->nt, v, "block") < 0) {
    int dr = nt_ref(c->nt, v, "receiver");
    const char *drt = dr >= 0 ? nt_type(c->nt, dr) : NULL;
    int den = 0;
    if (drt && sp_streq(drt, "ArrayNode") && (nt_arr(c->nt, dr, "elements", &den), den == 0)) {
      is_empty_array = 1;
      /* `[].freeze` is the frozen-empty-array idiom: build it at the target's
         type like dup/clone, and keep the flag the call is there for (#3828) */
      empty_array_frozen = sp_streq(nt_str(c->nt, v, "name"), "freeze");
    }
  }
  /* a bare `Array.new` (no size/block) is an empty array of the target's type */
  if (!is_empty_array && vty && sp_streq(vty, "CallNode") &&
      sp_streq(nt_str(c->nt, v, "name") ? nt_str(c->nt, v, "name") : "", "new") &&
      nt_ref(c->nt, v, "block") < 0) {
    int ar = nt_ref(c->nt, v, "receiver");
    const char *art = ar >= 0 ? nt_type(c->nt, ar) : NULL;
    int aargs = nt_ref(c->nt, v, "arguments"); int aac = 0;
    if (aargs >= 0) nt_arr(c->nt, aargs, "arguments", &aac);
    if (art && sp_streq(art, "ConstantReadNode") &&
        sp_streq(nt_str(c->nt, ar, "name") ? nt_str(c->nt, ar, "name") : "", "Array") && aac == 0)
      is_empty_array = 1;
  }
  int hn = 0;
  int is_empty_hash = vty && sp_streq(vty, "HashNode") && (nt_arr(c->nt, v, "elements", &hn), hn == 0);
  /* h = Hash.new / Hash.new(default) */
  int is_hash_new = 0, hash_new_default = -1;
  if (vty && sp_streq(vty, "CallNode") && sp_streq(nt_str(c->nt, v, "name") ? nt_str(c->nt, v, "name") : "", "new")) {
    int hr = nt_ref(c->nt, v, "receiver");
    const char *hrt = hr >= 0 ? nt_type(c->nt, hr) : NULL;
    if (hrt && (sp_streq(hrt, "ConstantReadNode") || sp_streq(hrt, "ConstantPathNode")) &&
        sp_streq(nt_str(c->nt, hr, "name") ? nt_str(c->nt, hr, "name") : "", "Hash")) {
      is_hash_new = 1;
      int ha = nt_ref(c->nt, v, "arguments");
      int hac = 0;
      const int *hav = ha >= 0 ? nt_arr(c->nt, ha, "arguments", &hac) : NULL;
      if (hac >= 1) hash_new_default = hav[0];
    }
  }

  if (vty && sp_streq(vty, "NilNode") && lv) {
    if (lv->type == TY_RANGE) buf_puts(b, "(sp_Range){0}");
    else buf_puts(b, default_value(lv->type));
  }
  else if (lv && lv->type == TY_STRBUF) {
    /* A shared-mutable alias (`s2 = s1`, both str_shared) copies the sp_String
       HANDLE, not the buffer, so the two names denote one object: a later
       `s1 << x` shows through s2 and `s1.equal?(s2)` is true (#3227). */
    char srefV[1024];
    if (lv->str_shared && strbuf_slot_ref(c, v, srefV, sizeof srefV))
      buf_puts(b, srefV);
    else if (comp_ntype(c, v) == TY_STRBUF) {
      /* a demand-marked read (a reader call, a container element) already
         yields the handle: alias it directly (#3227 P5) */
      emit_expr(c, v, b);
    }
    else if (comp_ntype(c, v) == TY_POLY || strbuf_boxed_elem_read(c, v)) {
      /* a container element read hands out the element's BOXED handle: take the
         handle out of the box, so the local and the element are one object and
         a mutation through either shows in the other (#3941) */
      buf_puts(b, "sp_poly_as_strbuf("); emit_expr(c, v, b); buf_puts(b, ")");
    }
    else {
      /* a value-position append chain over a shared base: run the chain (it
         appends in place), then alias the BASE handle -- wrapping the cstr
         would fork a second buffer and break the alias (#3307 family) */
      int cb9 = str_append_chain_base(c, v);
      char srefC9[1024];
      if (cb9 != v && lv->str_shared &&
          strbuf_slot_ref(c, cb9, srefC9, sizeof srefC9)) {
        buf_puts(b, "({ (void)(");
        emit_expr(c, v, b);
        buf_printf(b, "); %s; })", srefC9);
      }
      else {
        /* otherwise a mutable-string local wraps the (const char*) RHS in a
           fresh sp_String so later `<<` appends are amortized O(1). */
        buf_puts(b, "sp_String_new_shared("); emit_expr(c, v, b); buf_puts(b, ")");
      }
    }
  }
  else if (is_empty_array && lv && array_kind(lv->type)) {
    /* `a = []` -> a new array of the variable's resolved element type */
    if (empty_array_frozen) {
      int ft = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); _t%d->frozen = 1; _t%d; })",
                 array_kind(lv->type), ft, array_kind(lv->type), ft, ft);
    }
    else buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
  }
  else if (is_empty_array && lv && lv->type == TY_POLY_ARRAY) {
    if (empty_array_frozen) {
      int ft = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); _t%d->frozen = 1; _t%d; })", ft, ft, ft);
    }
    else buf_puts(b, "sp_PolyArray_new()");
  }
  else if (is_empty_array && lv && ty_is_ptr_array(lv->type)) {
    /* `a = []` for a narrowed object / int-array array: an sp_PtrArray whose
       elements are GC-marked as ordinary heap objects. */
    buf_puts(b, "sp_PtrArray_new()");
  }
  else if (lv && ty_is_ptr_array(lv->type) && emit_ptr_array_build(c, v, lv->type, b)) {
    /* built in place at the slot's own container type */
  }
  else if (is_hash_new && nt_ref(c->nt, v, "block") >= 0) {
    /* Hash.new { |hash, key| ... }: emit through emit_call so the dproc
       function + sp_PolyPolyHash_new_dproc path runs. */
    emit_expr(c, v, b);
  }
  else if ((is_empty_hash || is_hash_new) && lv && ty_hash_cname(lv->type)) {
    const char *hcn = ty_hash_cname(lv->type);
    int poly_val = (lv->type == TY_SYM_POLY_HASH || lv->type == TY_STR_POLY_HASH ||
                    lv->type == TY_POLY_POLY_HASH);
    if (is_hash_new && hash_new_default >= 0) {
      buf_printf(b, "sp_%sHash_new_with_default(", hcn);
      if (poly_val) emit_boxed(c, hash_new_default, b); else emit_expr(c, hash_new_default, b);
      buf_puts(b, ")");
    }
    else {
      buf_printf(b, "sp_%sHash_new()", hcn);
    }
  }
  else if (lv && lv->type == TY_POLY_ARRAY && ty_is_array(comp_ntype(c, v)) && comp_ntype(c, v) != TY_POLY_ARRAY) {
    /* widen typed array literal to PolyArray for this slot */
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_INT_ARRAY) { buf_puts(b, "sp_PolyArray_from_int_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else if (vt == TY_STR_ARRAY) { buf_puts(b, "sp_PolyArray_from_str_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else if (vt == TY_FLOAT_ARRAY) { buf_puts(b, "sp_PolyArray_from_float_array("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
  }
  else if (lv && lv->type == TY_BIGINT) {
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_BIGINT) emit_expr(c, v, b);
    else if (vt == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
  }
  else if (lv && lv->type == TY_POLY) {
    emit_boxed(c, v, b);   /* poly slot: box the (non-poly) RHS */
  }
  else if (lv && lv->type == TY_STR_POLY_HASH &&
           (comp_ntype(c, v) == TY_STR_STR_HASH || comp_ntype(c, v) == TY_STR_INT_HASH)) {
    /* widen a concrete str-keyed hash into the poly-valued slot */
    buf_printf(b, "sp_StrPolyHash_from_%s(", comp_ntype(c, v) == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
    emit_expr(c, v, b); buf_puts(b, ")");
  }
  /* A hash slot the analysis widened past its initializer's variant: the
     variants are separate C structs, so the pointer went in uncoerced and the
     build stopped. `sub = Tep.str_hash` where a widened block param later
     writes a poly key into `sub` is the shape (#4089); the same conversion the
     argument path uses (#3998) applies here, through the boxed form because
     that is what the converting entries take. */
  else if (lv && ty_is_hash(lv->type) && ty_is_hash(comp_ntype(c, v)) &&
           lv->type != comp_ntype(c, v) &&
           (lv->type == TY_POLY_POLY_HASH || lv->type == TY_SYM_POLY_HASH ||
            lv->type == TY_STR_POLY_HASH)) {
    const char *conv = lv->type == TY_POLY_POLY_HASH ? "sp_poly_as_poly_poly_hash"
                     : lv->type == TY_SYM_POLY_HASH  ? "sp_poly_as_sym_poly_hash"
                     : "sp_poly_as_str_poly_hash";
    buf_printf(b, "%s(", conv); emit_boxed(c, v, b); buf_puts(b, ")");
  }
  /* scalar/string slot with a poly RHS (`x = (a + b) * 2` over poly a/b, a
     string local read back from a poly call): unbox into the slot. */
  else if (lv && emit_poly_rhs_coerced(c, lv->type, v, b)) { }
  else if (lv && lv->type != TY_POLY && lv->type != TY_UNKNOWN &&
           comp_ntype(c, v) == TY_UNKNOWN) {
    /* a typed local assigned an unresolved call (the gate's raise-all token):
       coerce the sp_RbVal token to the slot type (`k = yield rec` where the
       yield is unresolvable, into an sp_int k). A non-token RHS emits raw. */
    emit_unresolved_coerced(c, v, lv->type, b);
  }
  else {
    emit_expr(c, v, b);
  }
  buf_puts(b, ";\n");
}

/* `x op= v` on a local. `lval` is the slot's C lvalue -- `lv_x` for a plain
   local, the cell/capture deref for a captured one -- so the typed arms below
   are the same either way. */
static void emit_op_assign_lv(Compiler *c, int id, Buf *b, int indent,
                              const char *lval) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  const char *op = nt_str(nt, id, "binary_operator");
  int v = nt_ref(nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  TyKind t = lv ? lv->type : TY_UNKNOWN;
  emit_indent(b, indent);

  /* Whether the slot lives in a cell (captured by a proc) only decides how it
     is SPELLED -- `lval` above already carries the deref. The arms below are
     shared: the celled path used to carry a short list of its own (int, float,
     bigint, poly) and drop every other type into the int helpers, so `s += x`
     on a captured String, Array, Rational or Complex emitted sp_int_add over
     pointers. */
  int celled = (lv && lv->is_cell) ||
               (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm));

  if (t == TY_STRING && sp_streq(op, "+")) {
    buf_printf(b, "%s = sp_str_concat(%s, ", lval, lval);
    /* a poly RHS (a destructured `[Int, String]` element bound poly) is an
       sp_RbVal; coerce it to const char* for sp_str_concat (#2875). CRuby's
       String#+ raises TypeError on a non-string, so this only reaches a value
       that is a String at run time. */
    if (comp_ntype(c, v) == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, ");\n");
    return;
  }
  /* Int op-assign routes through the same overflow-checked helpers as the
     binary form: a raw C `lv_x *= y` silently wrapped where `x * y` raised. */
  if (t == TY_INT) {
    const char *fn = int_arith_fn(op);
    if (fn) {
      TyKind vt = comp_ntype(c, v);
      int isdivmod = sp_streq(op, "/") || sp_streq(op, "%");
      buf_printf(b, "%s = %s(%s, ", lval, fn, lval);
      if (isdivmod) emit_int_divisor(c, v, b);
      else if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")"); }
      else emit_expr(c, v, b);
      buf_puts(b, ");\n"); return;
    }
  }
  /* Bitwise op-assign on an int: shift/and/or/xor map straight to the C
     operator (fixed-width wrap, same as the binary `x << y` path). A `<<=`
     by a literal count that overflows every nonzero receiver (>= 63) or a
     negative count routes through sp_int_shl, mirroring the binary gate. */
  if (t == TY_INT && (sp_streq(op, "<<") || sp_streq(op, ">>") ||
                      sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^"))) {
    TyKind vt = comp_ntype(c, v);
    const char *vty = nt_type(nt, v);
    long long vlit = (vty && sp_streq(vty, "IntegerNode")) ? nt_int(nt, v, "value", 0) : 0;
    if (sp_streq(op, "<<") && vty && sp_streq(vty, "IntegerNode") && (vlit < 0 || vlit >= 63)) {
      buf_printf(b, "%s = sp_int_shl(%s, %lldLL);\n", lval, lval, vlit);
      return;
    }
    buf_printf(b, "%s = (%s %s (", lval, lval, op);
    if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, "));\n");
    return;
  }
  if (t == TY_BIGINT) {
    const char *bfn = bigint_arith_fn(op);
    if (bfn) {
      TyKind vt = comp_ntype(c, v);
      buf_printf(b, "%s = %s(%s, ", lval, bfn, lval);
      if (vt == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, v, b); buf_puts(b, ")"); }
      else if (vt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
      else emit_expr(c, v, b);
      buf_puts(b, ");\n"); return;
    }
  }
  if (t == TY_FLOAT && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") || sp_streq(op, "/"))) {
    TyKind vt = comp_ntype(c, v);
    buf_printf(b, "%s %s= ", lval, op);
    if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (t == TY_COMPLEX && (sp_streq(op, "+") || sp_streq(op, "*"))) {
    /* coerce the rhs like the binary path does: an Integer, a Float or a boxed
       value all have to reach sp_complex_* as an sp_Complex */
    buf_printf(b, "%s = sp_complex_%s(%s, ", lval, sp_streq(op, "+") ? "add" : "mul", lval);
    emit_complex_coerce(c, v, b); buf_puts(b, ");\n");
    return;
  }
  /* `r += x` on a Rational local: `r = r <op> x` through the same sp_rational_*
     helpers the binary path uses. Only an Integer/Rational rhs keeps the result
     a Rational; a Float rhs would change the local's type (CRuby returns a
     Float) and falls through to the loud reject. */
  if (t == TY_RATIONAL && (sp_streq(op, "+") || sp_streq(op, "-") ||
                           sp_streq(op, "*") || sp_streq(op, "/"))) {
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_RATIONAL || vt == TY_INT) {
      const char *fn = op[0] == '+' ? "add" : op[0] == '-' ? "sub" : op[0] == '*' ? "mul" : "div";
      buf_printf(b, "%s = sp_rational_%s(%s, ", lval, fn, lval);
      emit_rat_coerce(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  /* `arr += other` is `arr = arr + other` — a fresh concatenation, the same
     helper the binary `+` path uses (codegen_call_recv.c). Covers Int/Float/
     Str/Poly arrays; the RHS must be the same kind, or an empty `[]` literal
     (passed as NULL, which sp_*Array_concat treats as empty — emitting the
     literal would build a wrong-kind IntArray). A genuinely mixed-kind concat
     (which the binary path promotes to a poly array via element boxing) is
     left to fall through — it isn't in the corpus. */
  if (ty_is_array(t) && sp_streq(op, "+")) {
    const char *k = (t == TY_POLY_ARRAY) ? "Poly" : array_kind(t);
    TyKind vt = comp_ntype(c, v);
    const char *vty = nt_type(nt, v);
    int rhs_empty_lit = 0;
    if (vty && sp_streq(vty, "ArrayNode")) {
      int nel = 0; nt_arr(nt, v, "elements", &nel);
      rhs_empty_lit = (nel == 0);
    }
    if (k && (vt == t || rhs_empty_lit)) {
      buf_printf(b, "%s = sp_%sArray_concat(%s, ", lval, k, lval);
      if (rhs_empty_lit) buf_puts(b, "NULL");
      else emit_expr(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  if (ty_is_object(t)) {
    int defcls2 = -1;
    int cid2 = ty_object_class(t);
    int mi2 = comp_method_in_chain(c, cid2, op, &defcls2);
    if (mi2 >= 0) {
      Scope *ms2 = &c->scopes[mi2];
      LocalVar *p2 = ms2->nparams >= 1 ? scope_local(ms2, ms2->pnames[0]) : NULL;
      int atmp2 = ++g_tmp;
      TyKind p2t = p2 ? p2->type : comp_ntype(c, v);
      emit_indent(g_pre, g_indent);
      emit_ctype(c, p2t, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp2);
      /* box the rhs when the operator's param widened to poly (promote mode) */
      if (p2t == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, g_pre);
      else emit_expr(c, v, g_pre);
      buf_puts(g_pre, ";\n");
      buf_printf(b, "%s = sp_%s_%s((sp_%s *)%s, _t%d);\n",
                 lval, c->classes[defcls2].c_name, mc(ms2->name),
                 c->classes[defcls2].c_name, lval, atmp2);
      return;
    }
  }
  /* Poly local (e.g. an int seeded then widened by a float op): defer the
     arithmetic to the runtime's tag-dispatching sp_poly_<op>. */
  if (t == TY_POLY) {
    const char *pfn = NULL;
    if (sp_streq(op, "+")) pfn = "sp_poly_add";
    else if (sp_streq(op, "-")) pfn = "sp_poly_sub";
    else if (sp_streq(op, "*")) pfn = "sp_poly_mul";
    else if (sp_streq(op, "/")) pfn = "sp_poly_div";
    else if (sp_streq(op, "%")) pfn = "sp_poly_mod";
    if (pfn) {
      buf_printf(b, "%s = %s(%s, ", lval, pfn, lval);
      emit_boxed(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  /* A Rational / Complex local op-assigned a BOXED value (an element read out
     of a poly array): the binary form folds through sp_poly_<op>, whose result
     is boxed, so unbox it back into the slot. `acc = acc + b[0]` already
     worked; only the `+=` spelling was refused (#3362). */
  if (t == TY_RATIONAL && comp_ntype(c, v) == TY_POLY) {
    const char *pfn = NULL;
    if (sp_streq(op, "+")) pfn = "sp_poly_add";
    else if (sp_streq(op, "-")) pfn = "sp_poly_sub";
    else if (sp_streq(op, "*")) pfn = "sp_poly_mul";
    else if (sp_streq(op, "/")) pfn = "sp_poly_div";
    if (pfn) {
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, t, lval, &bx);
      buf_printf(b, "%s = sp_poly_as_rational(%s(%s, ", lval, pfn, bx.p ? bx.p : lval);
      free(bx.p);
      emit_boxed(c, v, b);
      buf_puts(b, "));\n");
      return;
    }
  }
  /* Poly local bitwise op-assign (`x &= v`, `x >>= v`, ...): the result is an
     int, re-boxed into the poly slot. The poly value is coerced via to_i, like
     the binary poly-bitwise path. */
  if (t == TY_POLY && (sp_streq(op, "<<") || sp_streq(op, ">>") ||
                       sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^"))) {
    TyKind vt = comp_ntype(c, v);
    buf_printf(b, "%s = sp_box_int((sp_poly_to_i(%s) %s (", lval, lval, op);
    if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, ")));\n");
    return;
  }
  /* Array set-operation op-assign (`a |= b`, `a &= b`, `a -= b`): desugar to
     `a = a OP b` through the same typed set-op helper the binary `a | b` path
     uses. The rhs must be the same array kind (or an empty `[]` literal); a
     mismatched element kind falls through to the loud reject below. */
  if (ty_is_array(t) && (sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "-"))) {
    TyKind vt = comp_ntype(c, v);
    if (vt == t || vt == TY_UNKNOWN) {
      const char *k = (t == TY_POLY_ARRAY) ? "Poly" : array_kind(t);
      const char *fn = sp_streq(op, "&") ? "intersect" : (sp_streq(op, "|") ? "union" : "difference");
      buf_printf(b, "%s = sp_%sArray_%s(%s, ", lval, k, fn, lval);
      if (vt == TY_UNKNOWN) buf_puts(b, "NULL");
      else emit_expr(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  /* A captured local whose type never resolved: the celled path used to send
     every unrecognized type through the int helpers, which is right only when
     the slot really is int-sized. Keep it for the untyped case alone -- a
     typed slot takes its own arm above, or is rejected loudly. */
  if (celled && t == TY_UNKNOWN) {
    const char *fn = int_arith_fn(op);
    if (fn) {
      buf_printf(b, "%s = %s(%s, ", lval, fn, lval);
      if (sp_streq(op, "/") || sp_streq(op, "%")) emit_int_divisor(c, v, b);
      else emit_expr(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  unsupported(c, id, "operator assignment");
}

void emit_op_assign(Compiler *c, int id, Buf *b, int indent) {
  Buf lvb; memset(&lvb, 0, sizeof lvb);
  emit_local_ref(c, id, nt_str(c->nt, id, "name"), &lvb);
  emit_op_assign_lv(c, id, b, indent, lvb.p ? lvb.p : "");
  free(lvb.p);
}

/* ---- control flow ---- */

void emit_cond(Compiler *c, int id, Buf *b) {
  /* &block parameter used as condition in a yielding (inlined) method must
     be checked before the type-based dispatch below: now that blk_param is
     a registered TY_PROC local, it would otherwise hit the "!= 0" pointer
     path and emit lv_<blk> which is never declared at an inline site.
     For non-inlined methods (yields=0) lv_<blk> is a real sp_Proc* and
     the normal != 0 path handles it correctly. */
  {
    const char *nty = nt_type(c->nt, id);
    if (nty && sp_streq(nty, "LocalVariableReadNode")) {
      const char *nm = nt_str(c->nt, id, "name");
      Scope *s = nm ? comp_scope_of(c, id) : NULL;
      if (s && s->blk_param && nm && sp_streq(s->blk_param, nm) && s->yields) {
        buf_puts(b, g_block_id >= 0 ? "1" : "0");
        return;
      }
    }
  }
  TyKind t = comp_ntype(c, id);
  if (t == TY_POLY) { buf_puts(b, "sp_poly_truthy("); emit_expr(c, id, b); buf_puts(b, ")"); return; }
  if (t == TY_NIL)  { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, "), 0)"); return; }
  /* Ruby truthiness: only nil and false are falsy. A nullable scalar reads
     falsy at its sentinel (NULL string / SP_INT_NIL / NaN float); a pointer
     value is falsy when NULL. Every other concrete value is truthy. */
  /* a value-type object is never a NULL pointer -- it is always truthy */
  if (comp_ty_value_obj(c, t)) { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, "), 1)"); return; }
  if (t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t) ||
      t == TY_PROC || t == TY_MATCHDATA || t == TY_EXCEPTION ||
      t == TY_BIGINT || t == TY_REGEX || t == TY_CURRY || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_RANDOM || t == TY_DIR || t == TY_ADDRINFO || t == TY_SOCKOPT ||
      t == TY_METHOD || t == TY_IO || t == TY_ARGF) {
    buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, ") != 0)"); return;
  }
  if (t == TY_INT)   { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, ") != SP_INT_NIL)"); return; }
  if (t == TY_FLOAT) { buf_puts(b, "(!sp_float_is_nil("); emit_expr(c, id, b); buf_puts(b, "))"); return; }
  /* a nilable symbol slot holds (sp_sym)-1 for nil (default_value), so
     truthiness must test the sentinel -- `if @exit_triggered` with
     `@exit_triggered = nil` read always-true and ended doom's level on
     the first tic. */
  if (t == TY_SYMBOL) { buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, ") != (sp_sym)-1)"); return; }
  /* Always-truthy concrete value types: a Range / Class / Complex / Rational /
     Time value is never nil or false, so it is truthy in condition position.
     Evaluate it for side effects and yield 1. */
  if (t == TY_RANGE || t == TY_CLASS || t == TY_COMPLEX || t == TY_RATIONAL || t == TY_TIME) {
    buf_puts(b, "(("); emit_expr(c, id, b); buf_puts(b, "), 1)"); return;
  }
  if (t != TY_BOOL) unsupported(c, id, "condition (non-bool)");
  emit_expr(c, id, b);
}

/* `obj.is_a?(Class)` (or kind_of?/instance_of?) with a concrete object receiver
   is a compile-time constant: 1 (always) / 0 (never) / -1 (unknown). Lets a
   specialized clone whose param has a known class drop statically-dead branches
   (e.g. `instance.title=` in an arm guarded by `row.is_a?(ArticleRow)` when row
   is a CommentRow), which would otherwise emit an unresolvable call. */
int static_isa_cond(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0 || !nt_type(nt, pred) || !sp_streq(nt_type(nt, pred), "CallNode")) return -1;
  const char *nm = nt_str(nt, pred, "name");
  if (!nm || (!sp_streq(nm, "is_a?") && !sp_streq(nm, "kind_of?") && !sp_streq(nm, "instance_of?"))) return -1;
  int recv = nt_ref(nt, pred, "receiver");
  if (recv < 0) return -1;
  TyKind rt = comp_ntype(c, recv);
  int args = nt_ref(nt, pred, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "ConstantReadNode")) return -1;
  const char *tname = nt_str(nt, av[0], "name");
  int target = comp_class_index(c, tname);
  if (target < 0) return -1;
  /* A number/symbol/bool is never an instance of a user class, so the arm that
     reads it as one is dead -- and it is the only place a call like
     `value.value` on an Integer comes from (`Int64.new(0)` reaching
     `if value.is_a?(Int64) then value.value`). A builtin's own name is
     excluded (the class may be a reopen of it), as is a module some reopened
     builtin includes. */
  if (!ty_is_object(rt) && !ty_is_array(rt) && !ty_is_hash(rt) &&
      (rt == TY_INT || rt == TY_BIGINT || rt == TY_FLOAT || rt == TY_BOOL ||
       rt == TY_SYMBOL || rt == TY_STRING) &&
      tname && !is_builtin_class_name(tname)) {
    for (int k = 0; k < c->nclasses; k++) {
      if (!c->classes[k].name || !is_builtin_class_name(c->classes[k].name)) continue;
      for (int m = 0; m < c->classes[k].nincluded_mods; m++)
        if (c->classes[k].included_mods[m] == target) return -1;
    }
    return 0;
  }
  if (!ty_is_object(rt)) return -1;
  int rcls = ty_object_class(rt);
  if (rcls == target) return 1;
  if (!sp_streq(nm, "instance_of?") && is_descendant(c, rcls, target)) return 1;
  return 0;
}

/* Does any class expose a GENERATED writer for this ivar? attr_writer /
   attr_accessor synthesize the setter, so `obj.w = v` is a CallNode against a
   body that has no AST: the scan below cannot see the value it stores, and an
   ivar written only as `@w = nil` in the constructor read as "every write is
   nil" even though the program assigns it an Array from outside. A
   hand-written `def w=(v); @w = v; end` is unaffected -- its body holds a real
   InstanceVariableWriteNode that the scan already counts. */
static int ivar_has_generated_writer(Compiler *c, const char *nm) {
  const char *base = nm + 1;                  /* skip the '@' */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int w = 0; w < ci->nwriters; w++)
      if (ci->writers[w] && sp_streq(ci->writers[w], base)) return 1;
    for (int w = 0; w < ci->nsg_writers; w++)
      if (ci->sg_writers[w] && sp_streq(ci->sg_writers[w], base)) return 1;
  }
  return 0;
}

/* Scan every program-wide write to ivar `nm` ("@foo"): returns 0 when at least
   one write exists and all of them assign nil (statically falsy), -1 otherwise
   (no writes seen, a non-nil write, or an opaque write form). */
static int ivar_all_writes_nil(Compiler *c, const char *nm) {
  const NodeTable *nt = c->nt;
  if (!nm) return -1;
  if (ivar_has_generated_writer(c, nm)) return -1;
  int saw_write = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *wn = nt_str(nt, id, "name");
      if (!wn || !sp_streq(wn, nm)) continue;
      saw_write = 1;
      int v = nt_ref(nt, id, "value");
      const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
      if (!vty || !sp_streq(vty, "NilNode")) return -1;  /* a non-nil write */
    }
    else if (sp_streq(ty, "InstanceVariableOrWriteNode") ||
             sp_streq(ty, "InstanceVariableAndWriteNode") ||
             sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
             sp_streq(ty, "InstanceVariableTargetNode")) {
      const char *wn = nt_str(nt, id, "name");
      if (wn && sp_streq(wn, nm)) return -1;  /* other write forms: unknown */
    }
  }
  return saw_write ? 0 : -1;
}

/* An ivar read whose every program-wide write is nil is statically falsy
   (`@mode = nil` and never reassigned -> `if @mode` never fires). Returns 0
   for always-false, -1 otherwise. */
int static_nil_ivar_cond(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0 || !nt_type(nt, pred) || !sp_streq(nt_type(nt, pred), "InstanceVariableReadNode")) return -1;
  return ivar_all_writes_nil(c, nt_str(nt, pred, "name"));
}

/* `recv.reader` (no args, no block) where `reader` names an attr_reader-backed
   ivar @reader whose every program-wide write is nil is a falsy constant, so
   `if @conf.stackprof_mode` is statically dead. The receiver is restricted to a
   pure, object-typed form so dropping the branch skips no side effects and the
   call really is a getter. This resolves through the backing ivar rather than
   the inferred return type, which can mis-widen a nil-pinned ivar. Returns 0
   for always-false, -1 otherwise. */
int static_nil_reader_cond(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0 || !nt_type(nt, pred) || !sp_streq(nt_type(nt, pred), "CallNode")) return -1;
  const char *cn = nt_str(nt, pred, "name");
  if (!cn) return -1;
  int args = nt_ref(nt, pred, "arguments");
  if (args >= 0) {
    int ac = 0; nt_arr(nt, args, "arguments", &ac);
    if (ac != 0) return -1;
  }
  if (nt_ref(nt, pred, "block") >= 0) return -1;
  int recv = nt_ref(nt, pred, "receiver");
  if (recv < 0) return -1;
  const char *rty = nt_type(nt, recv);
  if (!rty) return -1;
  if (!sp_streq(rty, "InstanceVariableReadNode") &&
      !sp_streq(rty, "LocalVariableReadNode") &&
      !sp_streq(rty, "SelfNode")) return -1;
  if (!ty_is_object(comp_ntype(c, recv))) return -1;  /* a real getter receiver */
  char nm[256];
  snprintf(nm, sizeof nm, "@%s", cn);
  return ivar_all_writes_nil(c, nm);
}

/* `block_given?` is decided statically in most emissions: an inlined yielding
   method always has one, and a plain (non-lowered, non-proc-form) body never
   does. Answering it as a constant left the dead branch in the C, where it
   still had to typecheck -- the `return to_enum(:each) unless block_given?`
   idiom assigned the receiver into the Enumerator-typed result slot of the
   other path and warned on every such method (#3953). 1 = always true,
   0 = always false, -1 = decided at run time. */
static int static_block_given_cond(Compiler *c, int pred) {
  const NodeTable *nt = c->nt;
  if (pred < 0 || nt_kind(nt, pred) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, pred, "name");
  if (!nm || !sp_streq(nm, "block_given?")) return -1;
  int r = nt_ref(nt, pred, "receiver");
  if (r >= 0 && !(nt_type(nt, r) && sp_streq(nt_type(nt, r), "SelfNode"))) return -1;
  if (g_block_id >= 0) return 1;
  if (g_current_scope_is_lowered || g_yield_proc_ref) return -1;
  return 0;
}

void emit_if(Compiler *c, int id, Buf *b, int indent, int is_unless, int tail) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int then_b = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");

  /* Statically-decidable guard: drop the dead branch entirely. */
  {
    int sc = static_isa_cond(c, pred);
    if (sc < 0) sc = static_nil_ivar_cond(c, pred);
    if (sc < 0) sc = static_nil_reader_cond(c, pred);
    if (sc < 0) sc = static_block_given_cond(c, pred);
    /* `if defined?(UnknownConst) [&& ...]`: nil guard, the branch is dead.
       Must be dropped (not just emitted under `if (NULL)`): its body may call
       methods reachability already skipped for the same reason, or lean on
       the missing constant in ways that have no C translation. */
    if (sc < 0 && comp_defined_guard_false(c, pred)) sc = 0;
    int eff = (sc < 0) ? -1 : (is_unless ? !sc : sc);
    if (eff == 1) {
      /* condition always true: emit only the then-branch */
      emit_indent(b, indent); buf_puts(b, "{\n");
      if (tail) emit_stmts_tail(c, then_b, b, indent + 1);
      else      emit_stmts(c, then_b, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
    if (eff == 0) {
      /* condition always false: emit only the subsequent (else / elsif) */
      if (sub >= 0) {
        const char *sty = nt_type(nt, sub);
        if (sty && sp_streq(sty, "ElseNode")) {
          emit_indent(b, indent); buf_puts(b, "{\n");
          int s = nt_ref(nt, sub, "statements");
          if (tail) emit_stmts_tail(c, s, b, indent + 1);
          else      emit_stmts(c, s, b, indent + 1);
          emit_indent(b, indent); buf_puts(b, "}\n");
        }
        else if (sty && sp_streq(sty, "IfNode")) {
          emit_if(c, sub, b, indent, 0, tail);
        }
      }
      return;
    }
  }

  emit_indent(b, indent);
  buf_puts(b, "if (");
  if (is_unless) buf_puts(b, "!(");
  emit_cond(c, pred, b);
  if (is_unless) buf_puts(b, ")");
  buf_puts(b, ") {\n");
  if (tail) emit_stmts_tail(c, then_b, b, indent + 1);
  else      emit_stmts(c, then_b, b, indent + 1);
  emit_indent(b, indent);
  buf_puts(b, "}");

  if (sub >= 0) {
    const char *sty = nt_type(nt, sub);
    if (sty && sp_streq(sty, "ElseNode")) {
      buf_puts(b, "\n");
      emit_indent(b, indent);
      buf_puts(b, "else {\n");
      int s = nt_ref(nt, sub, "statements");
      if (tail) emit_stmts_tail(c, s, b, indent + 1);
      else      emit_stmts(c, s, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
    }
    else if (sty && sp_streq(sty, "IfNode")) {
      buf_puts(b, "\n");
      emit_indent(b, indent);
      buf_puts(b, "else {\n");
      emit_if(c, sub, b, indent + 1, 0, tail);
      emit_indent(b, indent); buf_puts(b, "}\n");
    }
    else {
      buf_puts(b, "\n");
    }
  }
  else {
    buf_puts(b, "\n");
  }
}

/* Emit `when ClassName` test against a poly (sp_RbVal) scrutinee temp.
   Returns 1 if the class is known and the check was emitted, 0 otherwise. */
int emit_poly_class_when(Compiler *c, int cond_id, const char *tmp, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, cond_id);
  if (!cty || (!sp_streq(cty, "ConstantReadNode") && !sp_streq(cty, "ConstantPathNode"))) return 0;
  const char *cn = nt_str(nt, cond_id, "name");
  if (!cn) return 0;
  /* a class-aliasing constant (Alias = SomeClass) tests the aliased class */
  { const char *_ra = resolve_class_alias(c, cn); if (_ra) cn = _ra; }
  if (sp_streq(cn, "Integer") || sp_streq(cn, "Fixnum"))
    buf_printf(b, "%s.tag == SP_TAG_INT", tmp);
  else if (sp_streq(cn, "String"))
    buf_printf(b, "%s.tag == SP_TAG_STR", tmp);
  else if (sp_streq(cn, "Float"))
    buf_printf(b, "%s.tag == SP_TAG_FLT", tmp);
  else if (sp_streq(cn, "Symbol"))
    buf_printf(b, "%s.tag == SP_TAG_SYM", tmp);
  else if (sp_streq(cn, "NilClass"))
    buf_printf(b, "%s.tag == SP_TAG_NIL", tmp);
  else if (sp_streq(cn, "TrueClass"))
    buf_printf(b, "(%s.tag == SP_TAG_BOOL && %s.v.b)", tmp, tmp);
  else if (sp_streq(cn, "FalseClass"))
    buf_printf(b, "(%s.tag == SP_TAG_BOOL && !%s.v.b)", tmp, tmp);
  else if (sp_streq(cn, "Numeric"))
    buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT || %s.tag == SP_TAG_BIGINT || "
                  "(%s.tag == SP_TAG_OBJ && (%s.cls_id == SP_BUILTIN_RATIONAL || "
                  "%s.cls_id == SP_BUILTIN_BIG_RATIONAL || %s.cls_id == SP_BUILTIN_COMPLEX)))",
               tmp, tmp, tmp, tmp, tmp, tmp, tmp);
  /* The value types below have a builtin cls_id but no entry in the class
     table, so the user-class arm at the end found nothing and emitted a
     constant false: `when Rational` simply never matched (#3959). */
  else if (sp_streq(cn, "Rational"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && (%s.cls_id == SP_BUILTIN_RATIONAL || "
                  "%s.cls_id == SP_BUILTIN_BIG_RATIONAL))", tmp, tmp, tmp);
  else if (sp_streq(cn, "Complex"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_COMPLEX)", tmp, tmp);
  else if (sp_streq(cn, "Regexp"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_REGEX)", tmp, tmp);
  else if (sp_streq(cn, "Proc"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_PROC)", tmp, tmp);
  else if (sp_streq(cn, "Time"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_TIME)", tmp, tmp);
  else if (sp_streq(cn, "Range"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_RANGE)", tmp, tmp);
  else if (sp_streq(cn, "Array"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", tmp, tmp, tmp);
  else if (sp_streq(cn, "Hash"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && ((%s.cls_id <= -13 && %s.cls_id >= -20) || %s.cls_id == -34))", tmp, tmp, tmp, tmp);
  else {
    int cid = comp_class_index(c, cn);
    if (cid >= 0) {
      buf_printf(b, "(%s.tag == SP_TAG_OBJ && (", tmp);
      int first = 1;
      for (int k = 0; k < c->nclasses; k++) {
        if (k == cid || is_descendant(c, k, cid)) {
          buf_printf(b, "%s%s.cls_id == %d", first ? "" : " || ", tmp, k);
          first = 0;
        }
      }
      if (first) buf_puts(b, "0");
      buf_puts(b, "))");
    }
    else return 0;
  }
  return 1;
}

/* Emit the match condition for a pattern into buf as a C boolean expression.
   Returns 1 if a condition was emitted (requires a runtime check),
   0 if the pattern always matches (no condition needed). */
/* Emit `_t<scrut> == <value-node>` as a C boolean, dispatching on the
   scrutinee's static type (poly: sp_poly_eq with the value boxed). */
void emit_pm_eq(Compiler *c, int t, TyKind pt, int valnode, Buf *b) {
  if (pt == TY_POLY) {
    buf_printf(b, "sp_poly_eq(_t%d, ", t);
    if (comp_ntype(c, valnode) != TY_POLY) emit_boxed(c, valnode, b);
    else emit_expr(c, valnode, b);
    buf_puts(b, ")");
  }
  else if (pt == TY_STRING) {
    buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, valnode, b); buf_puts(b, ")");
  }
  else {
    buf_printf(b, "(_t%d == ", t);
    if (comp_ntype(c, valnode) == TY_POLY) {
      /* a pinned poly value (e.g. `in ^x` with x widened) against a scalar
         scrutinee: unbox it to the scrutinee's type so the C `==` typechecks. */
      Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, valnode, &vb);
      emit_unbox_text(c, pt, vb.p ? vb.p : "", b); free(vb.p);
    }
    else emit_expr(c, valnode, b);
    buf_puts(b, ")");
  }
}

static int emit_pm_subcond_expr(Compiler *c, int spat, const char *elem, Buf *b);

/* Recursive match condition for a (possibly nested) array pattern over the
   boxed value `arr` (an sp_RbVal C-expression): `arr` is an array of the right
   length and every required/post element matches its sub-pattern (a literal,
   class, alternation, range, or nested container -- anything the general
   sub-pattern matcher checks). */
static void emit_pm_array_cond(Compiler *c, int pat, const char *arr, Buf *b) {
  const NodeTable *nt = c->nt;
  /* the deconstructed temp's name is read by every element accessor below, so
     it lives as long as this call -- and per call, since this recurses */
  char pm_arr_name[24];
  int apn = 0;
  const int *reqs = nt_arr(nt, pat, "requireds", &apn);
  int rest_nid = nt_ref(nt, pat, "rest");
  /* a trailing comma (`in [0, 1, ]`) is an ImplicitRestNode: same length
     semantics as a splat rest (at-least), it just binds nothing. */
  int has_rest = (rest_nid >= 0 && nt_type(nt, rest_nid) &&
                  (sp_streq(nt_type(nt, rest_nid), "SplatNode") ||
                   sp_streq(nt_type(nt, rest_nid), "ImplicitRestNode")));
  int npost = 0;
  const int *posts = nt_arr(nt, pat, "posts", &npost);
  /* With a rest the array need only be long enough to hold the leading
     requireds and the trailing posts; without a rest posts do not occur. */
  /* An array pattern needs an ARRAY (a value responding to #deconstruct), not
     any SP_TAG_OBJ: a Hash is also SP_TAG_OBJ and sp_poly_length returns its
     pair count, so without the array-kind guard `{a: 1, r: 2}` would wrongly
     match `[x, y]` (a Hash has no #deconstruct in CRuby). */
  /* A Struct or Data element answers #deconstruct, so convert it to that array
     before the array-kind guard rejects it (#3580). The conversion is a
     statement-expression so it stays inside this condition. */
  int tdc = ++g_tmp;
  Buf dbuf; memset(&dbuf, 0, sizeof dbuf);
  buf_printf(&dbuf, "({ sp_RbVal _t%d = %s;"
                    " if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0"
                    " && !sp_poly_is_array_kind(_t%d.cls_id)"
                    " && (sp_obj_deconstruct_fn || sp_obj_to_a_fn))"
                    " _t%d = sp_obj_deconstruct_fn ? sp_obj_deconstruct_fn(_t%d)"
                    " : sp_obj_to_a_fn(_t%d); _t%d; })",
             tdc, arr, tdc, tdc, tdc, tdc, tdc, tdc, tdc);
  int tda = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = %s; ", tda, dbuf.p ? dbuf.p : arr);
  free(dbuf.p);
  snprintf(pm_arr_name, sizeof pm_arr_name, "_t%d", tda);
  arr = pm_arr_name;
  buf_printf(b, "((%s).tag == SP_TAG_OBJ && sp_poly_is_array_kind((%s).cls_id) && sp_poly_length(%s) %s %dLL",
             arr, arr, arr, has_rest ? ">=" : "==", apn + npost);
  for (int i = 0; i < apn; i++) {
    /* the element accessor nests one level per recursion (arr grows), so build
       it in a Buf rather than a fixed buffer that would truncate. */
    Buf e; memset(&e, 0, sizeof e);
    buf_printf(&e, "sp_poly_index_poly(%s, sp_box_int(%dLL))", arr, i);
    Buf sub; memset(&sub, 0, sizeof sub);
    if (emit_pm_subcond_expr(c, reqs[i], e.p, &sub)) {
      buf_puts(b, " && "); buf_puts(b, sub.p ? sub.p : "1");
    }
    free(e.p); free(sub.p);
  }
  /* posts are checked from the tail (post j sits at len - (npost - j)). */
  for (int j = 0; j < npost; j++) {
    Buf e; memset(&e, 0, sizeof e);
    buf_printf(&e, "sp_poly_index_poly(%s, sp_box_int(sp_poly_length(%s) - %dLL))",
               arr, arr, (long long)(npost - j));
    Buf sub; memset(&sub, 0, sizeof sub);
    if (emit_pm_subcond_expr(c, posts[j], e.p, &sub)) {
      buf_puts(b, " && "); buf_puts(b, sub.p ? sub.p : "1");
    }
    free(e.p); free(sub.p);
  }
  buf_puts(b, "); })");
}

/* Hash-pattern match condition over a BOXED value (any hash variant),
   expression form: the value is a hash, every listed key is present, and
   each value sub-pattern matches. Used for nested hash patterns (a hash
   value, an array element, a find window) where the scrutinee is sp_RbVal. */
static void emit_pm_hash_cond_poly(Compiler *c, int pat, const char *hexpr, Buf *b) {
  const NodeTable *nt = c->nt;
  int th0 = ++g_tmp, th = ++g_tmp, tok = ++g_tmp, tf = ++g_tmp;
  /* A hash pattern also matches a user object via #deconstruct_keys: a Struct/
     Data value (or any object) read out of a container as a boxed poly. Convert
     a non-hash user object to its keyed hash first, then match. A class-
     qualified pattern (`User[name:]`) additionally guards the runtime class
     (#3161/#3180). */
  int hc_const = nt_ref(nt, pat, "constant");
  char subj0[24]; snprintf(subj0, sizeof subj0, "_t%d", th0);
  buf_printf(b, "({ sp_RbVal _t%d = %s; sp_RbVal _t%d = ", th0, hexpr, th);
  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0 && !sp_poly_is_hash_kind(_t%d.cls_id) && sp_obj_to_h_fn)"
                " ? sp_obj_to_h_fn(_t%d) : _t%d;",
             th0, th0, th0, th0, th0);
  buf_printf(b, " sp_bool _t%d = 0; (void)_t%d; "
                "int _t%d = (_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id));",
             tf, tf, tok, th, th);
  if (hc_const >= 0) {
    Buf cg; memset(&cg, 0, sizeof cg);
    if (emit_poly_class_when(c, hc_const, subj0, &cg))
      buf_printf(b, " _t%d = _t%d && (%s);", tok, tok, cg.p ? cg.p : "1");
    free(cg.p);
  }
  int en = 0, listed = 0;
  const int *elms = nt_arr(nt, pat, "elements", &en);
  for (int i = 0; i < en; i++) {
    if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
    int key = nt_ref(nt, elms[i], "key");
    int vpat = nt_ref(nt, elms[i], "value");
    if (key < 0) continue;
    listed++;
    int tv = ++g_tmp;
    buf_printf(b, " _t%d = 0; sp_RbVal _t%d = _t%d ? sp_poly_hash_get_pair_val(_t%d, ",
               tf, tv, tok, th);
    emit_boxed(c, key, b);
    buf_printf(b, ", &_t%d) : sp_box_nil(); _t%d = _t%d && _t%d;", tf, tok, tok, tf);
    Buf sub; memset(&sub, 0, sizeof sub);
    char ve[24]; snprintf(ve, sizeof ve, "_t%d", tv);
    if (emit_pm_subcond_expr(c, vpat, ve, &sub))
      buf_printf(b, " _t%d = _t%d && (%s);", tok, tok, sub.p ? sub.p : "1");
    free(sub.p);
  }
  /* `**nil`: no keys beyond the listed ones */
  int hp_rest = nt_ref(nt, pat, "rest");
  if (hp_rest >= 0 && nt_type(nt, hp_rest) &&
      sp_streq(nt_type(nt, hp_rest), "NoKeywordsParameterNode"))
    buf_printf(b, " _t%d = _t%d && (sp_poly_length(_t%d) == %dLL);", tok, tok, th, listed);
  buf_printf(b, " _t%d; })", tok);
}

/* Find-pattern match condition over a BOXED value (an array), expression
   form: scan for the first window whose elements all match the requireds.
   Used for nested find patterns; the top-level case-arm form keeps its own
   statement emitter (it must also expose the window position for binding). */
static void emit_pm_find_cond_poly(Compiler *c, int pat, const char *aexpr, Buf *b) {
  const NodeTable *nt = c->nt;
  int rn = 0;
  const int *reqs = nt_arr(nt, pat, "requireds", &rn);
  int ta = ++g_tmp, tp = ++g_tmp, ti = ++g_tmp, tw = ++g_tmp, tl = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = %s; sp_int _t%d = -1; "
                "if (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)) { "
                "sp_int _t%d = sp_poly_length(_t%d); "
                "for (sp_int _t%d = 0; _t%d + %dLL <= _t%d; _t%d++) { int _t%d = 1;",
             ta, aexpr, tp, ta, ta, tl, ta, ti, ti, rn, tl, ti, tw);
  for (int j = 0; j < rn; j++) {
    int te = ++g_tmp;
    buf_printf(b, " sp_RbVal _t%d = sp_poly_arr_get(_t%d, _t%d + %dLL); (void)_t%d;",
               te, ta, ti, j, te);
    Buf sub; memset(&sub, 0, sizeof sub);
    char ee[24]; snprintf(ee, sizeof ee, "_t%d", te);
    if (emit_pm_subcond_expr(c, reqs[j], ee, &sub))
      buf_printf(b, " _t%d = _t%d && (%s);", tw, tw, sub.p ? sub.p : "1");
    free(sub.p);
  }
  buf_printf(b, " if (_t%d) { _t%d = _t%d; break; } } } _t%d >= 0; })", tw, tp, ti, tp);
}

/* General sub-pattern condition over a boxed poly element C-expression.
   Returns 1 when a condition was written to b, 0 when the sub-pattern
   imposes none (a binding or wildcard). A sub-pattern the matcher cannot
   check rejects loudly -- an unchecked always-match silently takes the
   wrong arm. */
static int emit_pm_subcond_expr(Compiler *c, int spat, const char *elem, Buf *b) {
  const NodeTable *nt = c->nt;
  if (spat < 0) return 0;
  const char *pty = nt_type(nt, spat);
  if (!pty) return 0;
  if (sp_streq(pty, "LocalVariableTargetNode") || sp_streq(pty, "ImplicitNode") ||
      sp_streq(pty, "SplatNode") || sp_streq(pty, "ImplicitRestNode"))
    return 0;
  if (sp_streq(pty, "CapturePatternNode"))
    return emit_pm_subcond_expr(c, nt_ref(nt, spat, "value"), elem, b);
  if (sp_streq(pty, "ArrayPatternNode")) { emit_pm_array_cond(c, spat, elem, b); return 1; }
  if (sp_streq(pty, "HashPatternNode")) { emit_pm_hash_cond_poly(c, spat, elem, b); return 1; }
  if (sp_streq(pty, "FindPatternNode")) { emit_pm_find_cond_poly(c, spat, elem, b); return 1; }
  int tn = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = %s; ", tn, elem);
  Buf sub; memset(&sub, 0, sizeof sub);
  int ok = emit_pm_cond(c, spat, tn, TY_POLY, &sub);
  if (!ok) unsupported(c, spat, "pattern sub-form inside a container pattern");
  buf_printf(b, "%s; })", sub.p ? sub.p : "1");
  free(sub.p);
  return 1;
}

/* Classify a hash-pattern value sub-node for the boolean pattern matcher:
   returns the class ConstantReadNode id for `Class` / `Class => v`, -1 for a
   presence-only value (`k:` bare or `k: v` binding, which imposes no value
   constraint), or PM_HASH_VAL_REJECT for a shape we emit no check for (a
   literal, range, pin, or nested pattern) so the caller can reject it. */
#define PM_HASH_VAL_REJECT (-2)
static int pm_hash_value_class(const NodeTable *nt, int vpat) {
  if (vpat < 0 || !nt_type(nt, vpat)) return -1;
  const char *vty = nt_type(nt, vpat);
  if (sp_streq(vty, "ConstantReadNode")) return vpat;
  if (sp_streq(vty, "ImplicitNode") || sp_streq(vty, "LocalVariableTargetNode")) return -1;
  if (sp_streq(vty, "CapturePatternNode")) {
    int cv = nt_ref(nt, vpat, "value");
    if (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "ConstantReadNode")) return cv;
  }
  return PM_HASH_VAL_REJECT;
}

/* When set, a typed-hash pattern's statement-form match helpers (which
   reference the subject temp `_t<t>`) are emitted here instead of g_pre.
   g_pre is prepended to the whole enclosing statement -- ahead of the subject
   declaration -- so an alternation like `{a:} | {b:}` at the top of a case
   would use `_t<t>` before it is declared. Routing to the arm's own block keeps
   the helpers after the subject (#3185). */
static Buf *g_pm_hash_sink = NULL;

/* The name an exception carries at run time is its FULLY QUALIFIED Ruby name:
   sp_exc_new_sub is emitted with class_ruby_name, and sp_exc_is_a compares
   against that. A `when Mod::Klass` pattern node carries only the last
   segment, so comparing it directly never matched and the branch was silently
   skipped (#3373). Resolve through the class table and use the name the
   exception actually has. */
static const char *exc_when_cls_name(Compiler *c, const char *cn) {
  int ci = cn ? comp_class_index(c, cn) : -1;
  const char *q = ci >= 0 ? class_ruby_name(c, ci) : NULL;
  return q ? q : cn;
}

static int g_pm_hash_sink_indent = 0;

/* The `keys` argument a hash pattern hands #deconstruct_keys: an Array of the
   keys it names, or nil when the pattern can take everything (`**rest`). Every
   match passed nil, so an implementation that builds only what was asked for
   could not tell the two cases apart (#4046). */
static void emit_pm_deconstruct_keys_arg(Compiler *c, int pat, Buf *b) {
  const NodeTable *nt = c->nt;
  int en = 0; const int *elms = pat >= 0 ? nt_arr(nt, pat, "elements", &en) : NULL;
  int rest = pat >= 0 ? nt_ref(nt, pat, "rest") : -1;
  if (rest >= 0 || !elms) { buf_puts(b, "sp_box_nil()"); return; }
  int tk = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tk, tk);
  for (int i = 0; i < en; i++) {
    if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
    int key = nt_ref(nt, elms[i], "key");
    const char *kn = key >= 0 ? nt_str(nt, key, "value") : NULL;
    if (!kn) continue;
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", tk, comp_sym_intern(c, kn));
  }
  buf_printf(b, " sp_box_poly_array(_t%d); })", tk);
}

int emit_pm_cond(Compiler *c, int pat, int t, TyKind pt, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *pty = nt_type(nt, pat);
  if (!pty) return 0;
  /* literal value patterns: scrutinee == literal */
  if (sp_streq(pty, "IntegerNode") || sp_streq(pty, "FloatNode") ||
      sp_streq(pty, "StringNode") || sp_streq(pty, "SymbolNode")) {
    emit_pm_eq(c, t, pt, pat, b);
    return 1;
  }
  /* nil / true / false literal patterns */
  if (sp_streq(pty, "NilNode")) {
    if (pt == TY_POLY) buf_printf(b, "(_t%d.tag == SP_TAG_NIL)", t);
    /* a no-match MatchData is a NULL pointer; `in nil` matches it */
    else if (pt == TY_MATCHDATA) buf_printf(b, "(_t%d == NULL)", t);
    else buf_puts(b, (pt == TY_NIL) ? "1" : "0");
    return 1;
  }
  if (sp_streq(pty, "TrueNode")) {
    if (pt == TY_POLY) buf_printf(b, "(_t%d.tag == SP_TAG_BOOL && _t%d.v.b)", t, t);
    else if (pt == TY_BOOL) buf_printf(b, "(_t%d)", t);
    else buf_puts(b, "0");
    return 1;
  }
  if (sp_streq(pty, "FalseNode")) {
    if (pt == TY_POLY) buf_printf(b, "(_t%d.tag == SP_TAG_BOOL && !_t%d.v.b)", t, t);
    else if (pt == TY_BOOL) buf_printf(b, "(!_t%d)", t);
    else buf_puts(b, "0");
    return 1;
  }
  /* class pattern: runtime tag/class test for poly, compile-time fold otherwise */
  if (sp_streq(pty, "ConstantReadNode")) {
    const char *cn2 = nt_str(nt, pat, "name");
    if (!cn2) return 0;
    if (pt == TY_POLY) {
      char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
      if (!emit_poly_class_when(c, pat, tmp, b)) buf_puts(b, "0");
      return 1;
    }
    if (pt == TY_EXCEPTION) {
      /* an exception scrutinee matches by name up its class chain (#2759) */
      buf_printf(b, "sp_exc_is_a((sp_Exception *)_t%d, \"%s\")", t, exc_when_cls_name(c, cn2));
      return 1;
    }
    /* A user object scrutinee: the class-name table below only knows the
       builtins, so `case obj; in SomeClass` folded to a constant false for
       every user class -- including a Struct or a Data value against its own
       class (#3946). Decide it the way the `when` chain does: statically when
       the scrutinee's class is the pattern class or below it, and by the
       runtime tag when the pattern names a descendant of the static type. */
    if (ty_is_object(pt)) {
      int cid = ty_object_class(pt);
      int tcid = comp_class_index(c, cn2);
      if (tcid >= 0 && (cid == tcid || is_descendant(c, cid, tcid))) { buf_puts(b, "1"); return 1; }
      if (tcid >= 0 && is_descendant(c, tcid, cid)) {
        const char *acc = comp_ty_value_obj(c, pt) ? "." : "->";
        int first = 1;
        buf_puts(b, "(");
        for (int k = 0; k < c->nclasses; k++) {
          if (k != tcid && !is_descendant(c, k, tcid)) continue;
          if (!first) buf_puts(b, " || ");
          buf_printf(b, "_t%d%scls_id == %d", t, acc, k);
          first = 0;
        }
        if (first) buf_puts(b, "0");
        buf_puts(b, ")");
        return 1;
      }
      buf_puts(b, "0");
      return 1;
    }
    int yes = ty_matches_class(pt, cn2, 0);
    buf_printf(b, "%d", yes > 0 ? 1 : 0);
    return 1;
  }
  /* alternation `a | b`: either side matches */
  if (sp_streq(pty, "AlternationPatternNode")) {
    int l = nt_ref(nt, pat, "left"), r = nt_ref(nt, pat, "right");
    buf_puts(b, "(");
    if (!emit_pm_cond(c, l, t, pt, b)) buf_puts(b, "1");
    buf_puts(b, " || ");
    if (!emit_pm_cond(c, r, t, pt, b)) buf_puts(b, "1");
    buf_puts(b, ")");
    return 1;
  }
  /* pin `^var` / `^@ivar` / `^(expr)`: scrutinee == the pinned value */
  if (sp_streq(pty, "PinnedVariableNode") || sp_streq(pty, "PinnedExpressionNode")) {
    int ex = nt_ref(nt, pat, "expression");
    if (ex < 0) return 0;
    /* A pinned Regexp matches via === (Regexp#===), not equality: the pinned
       expression is an mrb_regexp_pattern*, so run it against the scrutinee
       string (a poly scrutinee matches only when it holds a string). */
    if (comp_ntype(c, ex) == TY_REGEX && (pt == TY_STRING || pt == TY_POLY)) {
      if (pt == TY_POLY) {
        buf_puts(b, "sp_re_case_eq(");
        emit_expr(c, ex, b);
        buf_printf(b, ", _t%d)", t);
      }
      else {
        buf_puts(b, "(sp_re_match(");
        emit_expr(c, ex, b);
        buf_printf(b, ", _t%d) >= 0)", t);
      }
      return 1;
    }
    emit_pm_eq(c, t, pt, ex, b);
    return 1;
  }
  /* range pattern `lo..hi` / `lo...hi` (and beginless/endless): membership via
     `===`, i.e. lo <= v (&& v <= hi, or v < hi when exclusive). */
  if (sp_streq(pty, "RangeNode")) {
    int lo = nt_ref(nt, pat, "left"), hi = nt_ref(nt, pat, "right");
    int excl = (int)(nt_int(nt, pat, "flags", 0) & 4) ? 1 : 0;
    const char *cmp = excl ? "<" : "<=";
    if (pt == TY_INT || pt == TY_FLOAT) {
      buf_puts(b, "(");
      int wrote = 0;
      if (lo >= 0) { buf_printf(b, "_t%d >= ", t); emit_expr(c, lo, b); wrote = 1; }
      if (hi >= 0) { if (wrote) buf_puts(b, " && "); buf_printf(b, "_t%d %s ", t, cmp); emit_expr(c, hi, b); wrote = 1; }
      if (!wrote) buf_puts(b, "1");
      buf_puts(b, ")");
      return 1;
    }
    if (pt == TY_POLY) {
      /* numeric membership only: the scrutinee must be an int in range. */
      buf_printf(b, "(_t%d.tag == SP_TAG_INT", t);
      if (lo >= 0) { buf_printf(b, " && _t%d.v.i >= ", t); emit_int_expr_nilable(c, lo, b); }
      if (hi >= 0) { buf_printf(b, " && _t%d.v.i %s ", t, cmp); emit_int_expr_nilable(c, hi, b); }
      buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(pty, "ArrayPatternNode")) {
    int apn = 0;
    const int *reqs = nt_arr(nt, pat, "requireds", &apn);
    /* A poly scrutinee -- a bare sp_RbVal (TY_POLY) or a poly-array pointer
       (TY_POLY_ARRAY) -- is matched by the poly-safe recursive condition, which
       reads length/elements through sp_poly_length / sp_poly_index_poly behind a
       SP_TAG_OBJ guard. This never emits `->len` on a non-array value and handles
       nested sub-arrays and required literal element checks uniformly. */
    if (pt == TY_POLY || pt == TY_POLY_ARRAY) {
      char arr[48];
      if (pt == TY_POLY_ARRAY) snprintf(arr, sizeof arr, "sp_box_poly_array(_t%d)", t);
      else                     snprintf(arr, sizeof arr, "_t%d", t);
      emit_pm_array_cond(c, pat, arr, b);
      return 1;
    }
    /* A user object answers an array pattern through its own #deconstruct,
       the way the case-arm emitter asks it: match what that returns. Without
       this the object reached the typed-array walk below and the emitted C
       read `->len` off the object'"'"'s struct (#3956). */
    if (ty_is_object(pt)) {
      int adef = -1;
      int am = comp_method_in_chain(c, ty_object_class(pt), "deconstruct", &adef);
      if (am < 0 || adef < 0 || c->scopes[am].ret == TY_UNKNOWN) return 0;
      TyKind art = c->scopes[am].ret;
      const char *acn = c->classes[adef].c_name;
      int aisv = comp_ty_value_obj(c, pt);
      int at = ++g_tmp;
      Buf *as = g_pm_hash_sink ? g_pm_hash_sink : g_pre;
      int ai = g_pm_hash_sink ? g_pm_hash_sink_indent : g_indent;
      emit_indent(as, ai);
      emit_ctype(c, art, as);
      if (aisv) buf_printf(as, " _t%d = sp_%s_deconstruct(_t%d);\n", at, acn, t);
      else      buf_printf(as, " _t%d = sp_%s_deconstruct((sp_%s *)_t%d);\n", at, acn, acn, t);
      if (needs_root(art)) { emit_indent(as, ai); emit_gc_root_tmp(c, art, at, as); buf_puts(as, "\n"); }
      return emit_pm_cond(c, pat, at, art, b);
    }
    /* From here the scrutinee is a typed (int/float/str) array pointer. A nested
       array element can never match one, since a typed array cannot hold a
       sub-array, so such a pattern is a guaranteed non-match. */
    int has_nested = 0;
    for (int i = 0; i < apn && !has_nested; i++) {
      const char *rty = nt_type(nt, reqs[i]);
      if (!rty) continue;
      if (sp_streq(rty, "ArrayPatternNode")) has_nested = 1;
      else if (sp_streq(rty, "CapturePatternNode")) {
        int val = nt_ref(nt, reqs[i], "value");
        if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ArrayPatternNode")) has_nested = 1;
      }
    }
    if (has_nested) { buf_puts(b, "0"); return 1; }
    /* Length check, then each required literal element must equal its pattern
       (short-circuits after the length guard so element access is in-bounds). */
    int rest_nid = nt_ref(nt, pat, "rest");
    int has_rest = (rest_nid >= 0 && nt_type(nt, rest_nid) &&
                    (sp_streq(nt_type(nt, rest_nid), "SplatNode") ||
                     sp_streq(nt_type(nt, rest_nid), "ImplicitRestNode")));
    int npost = 0;
    const int *posts = nt_arr(nt, pat, "posts", &npost);
    /* likewise a nested-array post can never sit inside a typed array. */
    for (int i = 0; i < npost && !has_nested; i++) {
      const char *rty = nt_type(nt, posts[i]);
      if (!rty) continue;
      if (sp_streq(rty, "ArrayPatternNode")) has_nested = 1;
      else if (sp_streq(rty, "CapturePatternNode")) {
        int val = nt_ref(nt, posts[i], "value");
        if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ArrayPatternNode")) has_nested = 1;
      }
    }
    if (has_nested) { buf_puts(b, "0"); return 1; }
    /* a `Class` / `Class => v` element check is fully static against a typed
       array's element type: a mismatching class can never match. */
    {
      TyKind et2 = ty_array_elem(pt);
      int class_mismatch = 0;
      for (int gi = 0; gi < apn + npost && !class_mismatch; gi++) {
        int pn2 = gi < apn ? reqs[gi] : posts[gi - apn];
        const char *rty = nt_type(nt, pn2);
        int classpat = -1;
        if (rty && sp_streq(rty, "ConstantReadNode")) classpat = pn2;
        else if (rty && sp_streq(rty, "CapturePatternNode")) {
          int val = nt_ref(nt, pn2, "value");
          if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ConstantReadNode"))
            classpat = val;
        }
        if (classpat >= 0) {
          const char *cn2 = nt_str(nt, classpat, "name");
          if (cn2 && ty_matches_class(et2, cn2, 0) <= 0) class_mismatch = 1;
        }
      }
      if (class_mismatch) { buf_puts(b, "0"); return 1; }
    }
    buf_printf(b, "(_t%d && _t%d->len %s %dLL", t, t, has_rest ? ">=" : "==", (long long)(apn + npost));
    const char *ak = array_kind(pt);
    if (ak) {
      const char *lo = sp_streq(ak, "Int") ? "int" : (sp_streq(ak, "Float") ? "float" : "str");
      char boxed[64];
      snprintf(boxed, sizeof boxed, "sp_box_%s_array(_t%d)", lo, t);
      for (int i = 0; i < apn; i++) {
        Buf e; memset(&e, 0, sizeof e);
        buf_printf(&e, "sp_poly_index_poly(%s, sp_box_int(%dLL))", boxed, i);
        Buf sub; memset(&sub, 0, sizeof sub);
        if (emit_pm_subcond_expr(c, reqs[i], e.p, &sub)) {
          buf_puts(b, " && "); buf_puts(b, sub.p ? sub.p : "1");
        }
        free(e.p); free(sub.p);
      }
      for (int j = 0; j < npost; j++) {
        Buf e; memset(&e, 0, sizeof e);
        buf_printf(&e, "sp_poly_index_poly(%s, sp_box_int(sp_poly_length(%s) - %dLL))",
                   boxed, boxed, (long long)(npost - j));
        Buf sub; memset(&sub, 0, sizeof sub);
        if (emit_pm_subcond_expr(c, posts[j], e.p, &sub)) {
          buf_puts(b, " && "); buf_puts(b, sub.p ? sub.p : "1");
        }
        free(e.p); free(sub.p);
      }
    }
    buf_puts(b, ")");
    return 1;
  }
  if (sp_streq(pty, "CapturePatternNode")) {
    /* Check inner pattern's condition if any */
    int val = nt_ref(nt, pat, "value");
    if (val >= 0) return emit_pm_cond(c, val, t, pt, b);
    return 0;
  }
  if (sp_streq(pty, "FindPatternNode")) {
    /* nested find (a hash value, an array element): scan the boxed array.
       The top-level case-arm form keeps its own statement emitter (it must
       also expose the window position for binding). */
    if (pt == TY_POLY) {
      char ae[24]; snprintf(ae, sizeof ae, "_t%d", t);
      emit_pm_find_cond_poly(c, pat, ae, b);
      return 1;
    }
    return 0;
  }
  if (sp_streq(pty, "HashPatternNode")) {
    /* A boxed scrutinee (a nested hash value, an array element) matches via
       the runtime-variant hash walk. */
    if (pt == TY_POLY) {
      char he[24]; snprintf(he, sizeof he, "_t%d", t);
      emit_pm_hash_cond_poly(c, pat, he, b);
      return 1;
    }
    /* matches when the scrutinee is a hash with every key present and each value
       matching its sub-pattern (a class check for `k: Class`). Only a statically
       typed hash scrutinee and the value shapes classified by pm_hash_value_class
       are handled; a non-hash scrutinee, an unsupported value pattern, or an
       unresolvable class returns 0 so the caller reports it unsupported rather
       than emitting a silently-wrong match. Everything is validated before any
       emit, so a reject never leaves half-built helper code in g_pre. */
    /* a Time answers a hash pattern through its #deconstruct_keys (#3702) */
    if (pt == TY_TIME) {
      int tth = ++g_tmp;
      Buf *hs0 = g_pm_hash_sink ? g_pm_hash_sink : g_pre;
      int hi0 = g_pm_hash_sink ? g_pm_hash_sink_indent : g_indent;
      emit_indent(hs0, hi0);
      buf_printf(hs0, "sp_SymPolyHash *_t%d = sp_time_deconstruct_all(_t%d); SP_GC_ROOT(_t%d);\n",
                 tth, t, tth);
      return emit_pm_cond(c, pat, tth, TY_SYM_POLY_HASH, b);
    }
    /* A statically typed object answers a hash pattern through its own
       #deconstruct_keys, exactly as the boxed form above does -- box it and
       take that path rather than refusing. Without this the one-line `obj in
       {...}` was rejected at the front end for every receiver that defines the
       method, while `case`/`in` against the same object compiled (#3956). */
    if (ty_is_object(pt)) {
      /* Ask the object itself, the way the case-arm emitter does: a user
         #deconstruct_keys is the answer, and the generic keyed reflection
         (Struct, Data) is the fallback. Without either the one-line `obj in
         {...}` was refused at the front end for every receiver that defines
         the method, while `case`/`in` against the same object compiled
         (#3956). */
      int ddef = -1;
      int dm = comp_method_in_chain(c, ty_object_class(pt), "deconstruct_keys", &ddef);
      if (dm >= 0 && ddef >= 0 && c->scopes[dm].ret != TY_UNKNOWN) {
        TyKind drt = c->scopes[dm].ret;
        const char *dcn = c->classes[ddef].c_name;
        int isv = comp_ty_value_obj(c, pt);
        int dt = ++g_tmp;
        Buf *ds = g_pm_hash_sink ? g_pm_hash_sink : g_pre;
        int di = g_pm_hash_sink ? g_pm_hash_sink_indent : g_indent;
        emit_indent(ds, di);
        emit_ctype(c, drt, ds);
        Buf kab; memset(&kab, 0, sizeof kab);
        emit_pm_deconstruct_keys_arg(c, pat, &kab);
        if (isv) buf_printf(ds, " _t%d = sp_%s_deconstruct_keys(_t%d, %s);\n", dt, dcn, t, kab.p ? kab.p : "sp_box_nil()");
        else     buf_printf(ds, " _t%d = sp_%s_deconstruct_keys((sp_%s *)_t%d, %s);\n", dt, dcn, dcn, t, kab.p ? kab.p : "sp_box_nil()");
        free(kab.p);
        if (needs_root(drt)) { emit_indent(ds, di); emit_gc_root_tmp(c, drt, dt, ds); buf_puts(ds, "\n"); }
        return emit_pm_cond(c, pat, dt, drt, b);
      }
      Buf hb; memset(&hb, 0, sizeof hb);
      char ho[24]; snprintf(ho, sizeof ho, "_t%d", t);
      emit_boxed_text(c, pt, ho, &hb);
      emit_pm_hash_cond_poly(c, pat, hb.p ? hb.p : "sp_box_nil()", b);
      free(hb.p);
      return 1;
    }
    const char *hn = ty_is_hash(pt) ? ty_hash_cname(pt) : NULL;
    if (!hn) return 0;
    TyKind hvt = ty_hash_val(pt);
    /* Statement-form helpers reference the subject `_t<t>`; route them to the
       arm's own block when a sink is set, else the g_pre prologue (#3185). */
    Buf *hs = g_pm_hash_sink ? g_pm_hash_sink : g_pre;
    int hi = g_pm_hash_sink ? g_pm_hash_sink_indent : g_indent;
    int en = 0;
    const int *elms = nt_arr(nt, pat, "elements", &en);
    for (int i = 0; i < en; i++) {
      if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) return 0;
      if (nt_ref(nt, elms[i], "key") < 0) return 0;
      int classpat = pm_hash_value_class(nt, nt_ref(nt, elms[i], "value"));
      if (classpat == PM_HASH_VAL_REJECT) {
        /* a value sub-pattern (literal, range, alternation, pin, container):
           trial-emit the recursive condition against the fetched value's type
           and reject when the recursion cannot check it. The trial may write
           helper text; roll the sink back so a reject leaves no half-built code. */
        size_t gp_len = hs->len;
        int tt = ++g_tmp;
        Buf trial; memset(&trial, 0, sizeof trial);
        int ok = emit_pm_cond(c, nt_ref(nt, elms[i], "value"), tt, hvt, &trial);
        free(trial.p);
        if (hs->p) { hs->len = gp_len; hs->p[gp_len] = 0; }
        if (!ok) return 0;
        continue;
      }
      if (classpat >= 0 && hvt == TY_POLY) {
        Buf probe; memset(&probe, 0, sizeof probe);
        int ok = emit_poly_class_when(c, classpat, "_v", &probe);
        free(probe.p);
        if (!ok) return 0;
      }
    }
    int hcond = ++g_tmp;
    emit_indent(hs, hi); buf_printf(hs, "int _t%d = 1;\n", hcond);
    for (int i = 0; i < en; i++) {
      int key = nt_ref(nt, elms[i], "key");
      int vchk = nt_ref(nt, elms[i], "value");
      int classpat = pm_hash_value_class(nt, vchk);
      emit_indent(hs, hi);
      buf_printf(hs, "_t%d = _t%d && sp_%sHash_has_key(_t%d, ", hcond, hcond, hn, t);
      emit_expr(c, key, hs); buf_puts(hs, ");\n");
      if (classpat == PM_HASH_VAL_REJECT) {
        /* fetch the value into a typed temp and AND in the recursive check
           (validated to succeed above) */
        int vtmp = ++g_tmp;
        emit_indent(hs, hi);
        buf_puts(hs, "{ ");
        emit_ctype(c, hvt, hs);
        buf_printf(hs, " _t%d = sp_%sHash_get(_t%d, ", vtmp, hn, t);
        emit_expr(c, key, hs); buf_puts(hs, "); ");
        Buf vc; memset(&vc, 0, sizeof vc);
        emit_pm_cond(c, vchk, vtmp, hvt, &vc);
        buf_printf(hs, "_t%d = _t%d && (%s); }\n", hcond, hcond, vc.p ? vc.p : "1");
        free(vc.p);
        continue;
      }
      if (classpat < 0) continue;
      if (hvt == TY_POLY) {
        int vtmp = ++g_tmp;
        emit_indent(hs, hi);
        buf_printf(hs, "{ sp_RbVal _t%d = sp_%sHash_get(_t%d, ", vtmp, hn, t);
        emit_expr(c, key, hs); buf_puts(hs, "); ");
        char vn[24]; snprintf(vn, sizeof vn, "_t%d", vtmp);
        Buf cw; memset(&cw, 0, sizeof cw);
        emit_poly_class_when(c, classpat, vn, &cw);   /* validated to succeed above */
        buf_printf(hs, "_t%d = _t%d && (%s);", hcond, hcond, cw.p ? cw.p : "1");
        free(cw.p);
        buf_puts(hs, " }\n");
      }
      else {
        const char *cn = nt_str(nt, classpat, "name");
        if (cn && ty_matches_class(hvt, cn, 0) <= 0) {
          emit_indent(hs, hi); buf_printf(hs, "_t%d = 0;\n", hcond);
        }
      }
    }
    buf_printf(b, "_t%d", hcond);
    return 1;
  }
  return 0;
}

/* case/in (pattern match) -> bind pattern vars, optional guard check,
   then body; goto end_label to skip subsequent arms. */
/* Emit a pattern-arm body in value mode: side-effect stmts, then assign the
   last expression (boxed to rt) to the result temp _t<cr>. The last expression
   is captured into a local buffer first so any prelude it emits (e.g. an array
   literal's construction) lands in g_pre ahead of the assignment, not spliced
   into it. */
static void emit_pm_body_value(Compiler *c, int stmts, TyKind rt, int cr,
                               Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
  for (int k = 0; k < n - 1; k++) emit_stmt(c, bb[k], b, indent);
  if (n <= 0) {
    emit_indent(b, indent);
    buf_printf(b, "_t%d = %s;\n", cr, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
    return;
  }
  int last = bb[n - 1];
  TyKind lt = comp_ntype(c, last);
  /* An empty `[]` / `{}` caches TY_UNKNOWN because it has no ELEMENT type
     yet, not because it has no value; running it for effect left the arm at
     the default. Build it against the arm's result type, as the `when` form
     and the if-as-value form do. */
  {
    int elen = 0;
    const char *ety = nt_type(nt, last);
    if (lt == TY_UNKNOWN && ety &&
        (sp_streq(ety, "ArrayNode") || sp_streq(ety, "HashNode")) &&
        (nt_arr(nt, last, "elements", &elen), elen == 0)) {
      const char *txt = NULL;
      char buf[128];
      if (sp_streq(ety, "ArrayNode")) {
        const char *rk = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
        if (rk) { snprintf(buf, sizeof buf, "sp_%sArray_new()", rk); txt = buf; }
        else if (rt == TY_POLY) { txt = "sp_box_poly_array(sp_PolyArray_new())"; }
      }
      else {
        const char *hc = ty_hash_cname(rt);
        if (hc) { snprintf(buf, sizeof buf, "sp_%sHash_new()", hc); txt = buf; }
        else if (rt == TY_POLY) {
          snprintf(buf, sizeof buf, "sp_box_obj(sp_StrPolyHash_new(), %s)",
                   hash_box_cls(TY_STR_POLY_HASH));
          txt = buf;
        }
      }
      if (txt) {
        emit_indent(b, indent);
        buf_printf(b, "_t%d = %s;\n", cr, txt);
        return;
      }
    }
  }
  if (lt == TY_NIL || lt == TY_UNKNOWN) {
    /* a valueless last expr (e.g. a bare assignment / void call): run it for
       its side effect, then default the result. */
    emit_stmt(c, last, b, indent);
    emit_indent(b, indent);
    buf_printf(b, "_t%d = %s;\n", cr, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
    return;
  }
  Buf le; memset(&le, 0, sizeof le);
  int saved_gi = g_indent; g_indent = indent;
  if (rt == TY_POLY && lt != TY_POLY) emit_boxed(c, last, &le);
  else emit_expr(c, last, &le);
  g_indent = saved_gi;
  emit_indent(b, indent);
  buf_printf(b, "_t%d = ", cr);
  buf_puts(b, le.p ? le.p : default_value(rt));
  buf_puts(b, ";\n");
  free(le.p);
}

/* Assign the boxed value `boxed` (an sp_RbVal C-expression) into local `lnm`,
   coercing to the local's declared C type: scalars are unboxed, typed-array and
   string locals get the pointer/cstr out of the box, a poly local takes it
   directly. Keeps a pattern binding compatible with however inference typed the
   target (e.g. a `*rest` typed as a concrete array pointer vs. a poly value). */
static void emit_pm_typed_assign(Scope *sc, const char *lnm,
                                 const char *boxed, Buf *b, int indent) {
  LocalVar *lv = sc ? scope_local(sc, lnm) : NULL;
  TyKind ty = lv ? lv->type : TY_POLY;
  emit_indent(b, indent); buf_printf(b, "lv_%s = ", lnm);
  if (ty == TY_INT || ty == TY_BOOL)      buf_printf(b, "sp_poly_to_i(%s)", boxed);
  else if (ty == TY_FLOAT)                buf_printf(b, "sp_poly_to_f(%s)", boxed);
  else if (ty == TY_INT_ARRAY)            buf_printf(b, "(sp_IntArray *)(%s).v.p", boxed);
  else if (ty == TY_FLOAT_ARRAY)          buf_printf(b, "(sp_FloatArray *)(%s).v.p", boxed);
  else if (ty == TY_STR_ARRAY)            buf_printf(b, "(sp_StrArray *)(%s).v.p", boxed);
  else if (ty == TY_POLY_ARRAY)           buf_printf(b, "(sp_PolyArray *)(%s).v.p", boxed);
  else if (ty == TY_STRING)               buf_printf(b, "(%s).v.s", boxed);
  else                                    buf_puts(b, boxed);  /* poly: direct */
  buf_puts(b, ";\n");
}

/* Recursively bind the LocalVariableTargetNode leaves of a (possibly nested)
   array pattern from the boxed array `arr` (an sp_RbVal C-expression). Element
   access goes through the kind-dispatching sp_poly_index_poly / sp_poly_slice
   so a nested element of any array kind (typed IntArray as well as PolyArray)
   is read correctly. A nested array pattern recurses into the element; a
   CapturePatternNode binds the whole element and recurses if its inner pattern
   is an array; a trailing `*rest` slices the tail. `sc` is the case scope, used
   to type each bound local. */
static void emit_pm_bind_poly(Compiler *c, int pat, const char *arr, int indent, Buf *b, Scope *sc);
static void emit_pm_bind_hash_poly(Compiler *c, int pat, const char *hexpr, int indent, Buf *b, Scope *sc);
static void emit_pm_bind_find_poly(Compiler *c, int pat, const char *aexpr, int indent, Buf *b, Scope *sc);

/* Dispatch a nested container sub-pattern to its poly binder over the boxed
   element expression `src`. Returns 1 when `spat` was a container. */
static int emit_pm_bind_container_poly(Compiler *c, int spat, const char *src,
                                       int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  const char *sty = spat >= 0 ? nt_type(nt, spat) : NULL;
  if (!sty) return 0;
  if (!sp_streq(sty, "ArrayPatternNode") && !sp_streq(sty, "HashPatternNode") &&
      !sp_streq(sty, "FindPatternNode")) return 0;
  int sub = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "sp_RbVal _t%d = %s;\n", sub, src);
  char se[24]; snprintf(se, sizeof se, "_t%d", sub);
  if (sp_streq(sty, "HashPatternNode")) emit_pm_bind_hash_poly(c, spat, se, indent, b, sc);
  else if (sp_streq(sty, "FindPatternNode")) emit_pm_bind_find_poly(c, spat, se, indent, b, sc);
  else emit_pm_bind_poly(c, spat, se, indent, b, sc);
  return 1;
}

/* Public entry to the case/in capture binders, for the expression-position
   one-line `expr in pattern`: bind the pattern's locals from a boxed scrutinee
   expression. A non-container pattern (a bare value/pin) binds nothing. */
void emit_pm_bind_pattern(Compiler *c, int pat, const char *src_poly, int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  const char *pty = pat >= 0 ? nt_type(nt, pat) : NULL;
  /* A top-level `<pattern> => name` binds the whole scrutinee to name, then
     recurses into the inner pattern. */
  if (pty && sp_streq(pty, "CapturePatternNode")) {
    int tgt = nt_ref(nt, pat, "target");
    const char *ttype = tgt >= 0 ? nt_type(nt, tgt) : NULL;
    if (ttype && sp_streq(ttype, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, tgt, "name");
      if (lnm) emit_pm_typed_assign(sc, lnm, src_poly, b, indent);
    }
    emit_pm_bind_container_poly(c, nt_ref(nt, pat, "value"), src_poly, indent, b, sc);
    return;
  }
  emit_pm_bind_container_poly(c, pat, src_poly, indent, b, sc);
}

static void emit_pm_bind_poly(Compiler *c, int pat, const char *arr, int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  int apn = 0;
  const int *reqs = nt_arr(nt, pat, "requireds", &apn);
  for (int i = 0; i < apn; i++) {
    const char *rty = nt_type(nt, reqs[i]);
    if (!rty) continue;
    /* the element accessor nests one level per recursion (arr grows), so build
       it in a Buf rather than a fixed-size stack buffer that would truncate. */
    Buf src; memset(&src, 0, sizeof src);
    buf_printf(&src, "sp_poly_index_poly(%s, sp_box_int(%dLL))", arr, i);
    if (sp_streq(rty, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, reqs[i], "name");
      if (lnm) emit_pm_typed_assign(sc, lnm, src.p, b, indent);
    }
    else if (sp_streq(rty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, reqs[i], "target");
      if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) emit_pm_typed_assign(sc, lnm, src.p, b, indent);
      }
      emit_pm_bind_container_poly(c, nt_ref(nt, reqs[i], "value"), src.p, indent, b, sc);
    }
    else emit_pm_bind_container_poly(c, reqs[i], src.p, indent, b, sc);
    free(src.p);
  }
  int npost = 0;
  const int *posts = nt_arr(nt, pat, "posts", &npost);
  int rest_nid = nt_ref(nt, pat, "rest");
  if (rest_nid >= 0 && nt_type(nt, rest_nid) && sp_streq(nt_type(nt, rest_nid), "SplatNode")) {
    int inner = nt_ref(nt, rest_nid, "expression");
    if (inner >= 0 && nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
      const char *rnm = nt_str(nt, inner, "name");
      if (rnm) {
        /* the rest captures the middle: elements after the requireds and before
           the posts, i.e. length - apn - npost of them starting at apn. */
        Buf rsrc; memset(&rsrc, 0, sizeof rsrc);
        buf_printf(&rsrc, "sp_poly_slice(%s, %dLL, sp_poly_length(%s) - %dLL)",
                   arr, apn, arr, (long long)(apn + npost));
        emit_pm_typed_assign(sc, rnm, rsrc.p, b, indent);
        free(rsrc.p);
      }
    }
  }
  /* posts bind from the tail: post j is at index len - (npost - j). */
  for (int j = 0; j < npost; j++) {
    const char *rty = nt_type(nt, posts[j]);
    if (!rty) continue;
    Buf src; memset(&src, 0, sizeof src);
    buf_printf(&src, "sp_poly_index_poly(%s, sp_box_int(sp_poly_length(%s) - %dLL))",
               arr, arr, (long long)(npost - j));
    if (sp_streq(rty, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, posts[j], "name");
      if (lnm) emit_pm_typed_assign(sc, lnm, src.p, b, indent);
    }
    else if (sp_streq(rty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, posts[j], "target");
      if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) emit_pm_typed_assign(sc, lnm, src.p, b, indent);
      }
      /* `[a, b] => cap`: also bind the names inside the captured sub-pattern. */
      emit_pm_bind_container_poly(c, nt_ref(nt, posts[j], "value"), src.p, indent, b, sc);
    }
    else emit_pm_bind_container_poly(c, posts[j], src.p, indent, b, sc);
    free(src.p);
  }
}

/* Bind the names inside a hash pattern whose scrutinee is a BOXED value (any
   hash variant). The match condition already verified key presence, so a
   fetch here always finds its pair. */
static void emit_pm_bind_hash_poly(Compiler *c, int pat, const char *hexpr, int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  int en = 0;
  const int *elms = nt_arr(nt, pat, "elements", &en);
  /* deconstruct a user object (a Struct/Data read as a boxed poly) into its
     keyed hash so the per-key reads below find the members; a real hash is
     left untouched by the runtime guard (#3161/#3180). */
  int thd = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "sp_RbVal _t%d = %s; if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0 && !sp_poly_is_hash_kind(_t%d.cls_id) && sp_obj_to_h_fn) _t%d = sp_obj_to_h_fn(_t%d);\n",
             thd, hexpr, thd, thd, thd, thd, thd);
  char hbuf[24]; snprintf(hbuf, sizeof hbuf, "_t%d", thd);
  hexpr = hbuf;
  for (int i = 0; i < en; i++) {
    if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
    int key = nt_ref(nt, elms[i], "key");
    int vpat = nt_ref(nt, elms[i], "value");
    if (key < 0) continue;
    /* resolve the bound local name: shorthand uses the key symbol */
    const char *lnm = NULL;
    int sub = -1;
    if (vpat < 0 || (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "ImplicitNode"))) {
      if (nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) lnm = nt_str(nt, key, "value");
    }
    else if (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "LocalVariableTargetNode"))
      lnm = nt_str(nt, vpat, "name");
    else if (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "CapturePatternNode")) {
      int tgt = nt_ref(nt, vpat, "target");
      if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
        lnm = nt_str(nt, tgt, "name");
      sub = nt_ref(nt, vpat, "value");
    }
    else sub = vpat;
    if (!lnm && sub < 0) continue;
    int tv = ++g_tmp, tf = ++g_tmp;
    emit_indent(b, indent); buf_puts(b, "{\n");
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_bool _t%d = 0; (void)_t%d;\n", tf, tf);
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_RbVal _t%d = sp_poly_hash_get_pair_val(%s, ", tv, hexpr);
    emit_boxed(c, key, b);
    buf_printf(b, ", &_t%d);\n", tf);
    char ve[24]; snprintf(ve, sizeof ve, "_t%d", tv);
    if (lnm) emit_pm_typed_assign(sc, lnm, ve, b, indent + 1);
    if (sub >= 0) emit_pm_bind_container_poly(c, sub, ve, indent + 1, b, sc);
    emit_indent(b, indent); buf_puts(b, "}\n");
  }
}

/* Bind the names inside a find pattern whose scrutinee is a BOXED array:
   re-run the window scan the condition performed (its position temp lived
   inside an expression scope), then bind the splats and window targets. */
static void emit_pm_bind_find_poly(Compiler *c, int pat, const char *aexpr, int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  int rn = 0;
  const int *reqs = nt_arr(nt, pat, "requireds", &rn);
  int ta = ++g_tmp, tp = ++g_tmp, ti = ++g_tmp, tw = ++g_tmp, tl = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "{ sp_RbVal _t%d = %s; sp_int _t%d = -1;\n", ta, aexpr, tp);
  emit_indent(b, indent + 1);
  buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)) { "
                "sp_int _t%d = sp_poly_length(_t%d); "
                "for (sp_int _t%d = 0; _t%d + %dLL <= _t%d; _t%d++) { int _t%d = 1;",
             ta, ta, tl, ta, ti, ti, rn, tl, ti, tw);
  for (int j = 0; j < rn; j++) {
    int te = ++g_tmp;
    buf_printf(b, " sp_RbVal _t%d = sp_poly_arr_get(_t%d, _t%d + %dLL); (void)_t%d;",
               te, ta, ti, j, te);
    Buf sub; memset(&sub, 0, sizeof sub);
    char ee[24]; snprintf(ee, sizeof ee, "_t%d", te);
    if (emit_pm_subcond_expr(c, reqs[j], ee, &sub))
      buf_printf(b, " _t%d = _t%d && (%s);", tw, tw, sub.p ? sub.p : "1");
    free(sub.p);
  }
  buf_printf(b, " if (_t%d) { _t%d = _t%d; break; } } }\n", tw, tp, ti);
  emit_indent(b, indent + 1);
  buf_printf(b, "if (_t%d >= 0) {\n", tp);
  int sides[2] = { nt_ref(nt, pat, "left"), nt_ref(nt, pat, "right") };
  for (int s = 0; s < 2; s++) {
    if (sides[s] < 0 || !nt_type(nt, sides[s]) ||
        !sp_streq(nt_type(nt, sides[s]), "SplatNode")) continue;
    int inner = nt_ref(nt, sides[s], "expression");
    if (inner < 0 || !nt_type(nt, inner) ||
        !sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) continue;
    const char *snm = nt_str(nt, inner, "name");
    if (!snm) continue;
    Buf ss; memset(&ss, 0, sizeof ss);
    if (s == 0) buf_printf(&ss, "sp_poly_slice(_t%d, 0LL, _t%d)", ta, tp);
    else buf_printf(&ss, "sp_poly_slice(_t%d, _t%d + %dLL, sp_poly_length(_t%d) - (_t%d + %dLL))",
                    ta, tp, rn, ta, tp, rn);
    emit_pm_typed_assign(sc, snm, ss.p, b, indent + 2);
    free(ss.p);
  }
  for (int j = 0; j < rn; j++) {
    const char *rty = nt_type(nt, reqs[j]);
    if (!rty) continue;
    Buf ge; memset(&ge, 0, sizeof ge);
    buf_printf(&ge, "sp_poly_arr_get(_t%d, _t%d + %dLL)", ta, tp, j);
    if (sp_streq(rty, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, reqs[j], "name");
      if (lnm) emit_pm_typed_assign(sc, lnm, ge.p, b, indent + 2);
    }
    else if (sp_streq(rty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, reqs[j], "target");
      if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) emit_pm_typed_assign(sc, lnm, ge.p, b, indent + 2);
      }
      emit_pm_bind_container_poly(c, nt_ref(nt, reqs[j], "value"), ge.p, indent + 2, b, sc);
    }
    else emit_pm_bind_container_poly(c, reqs[j], ge.p, indent + 2, b, sc);
    free(ge.p);
  }
  emit_indent(b, indent + 1); buf_puts(b, "}\n");
  emit_indent(b, indent); buf_puts(b, "}\n");
}

/* Recursively bind a multiple-assignment target from a boxed poly value `val`:
   a local, or a nested (a, (b, c)) / (a, *b, c) MultiTarget. Nested targets only
   arise with a poly-array RHS -- a typed array cannot hold a sub-array element. */
static void emit_massign_poly_target(Compiler *c, int tgt, const char *val,
                                     int indent, Buf *b, Scope *sc) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, tgt);
  if (!ty) return;
  if (sp_streq(ty, "LocalVariableTargetNode")) {
    const char *lnm = nt_str(nt, tgt, "name");
    if (lnm) emit_pm_typed_assign(sc, lnm, val, b, indent);
    return;
  }
  if (sp_streq(ty, "MultiTargetNode")) {
    int ln = 0; const int *lefts = nt_arr(nt, tgt, "lefts", &ln);
    int rn = 0; const int *rights = nt_arr(nt, tgt, "rights", &rn);
    int rest = nt_ref(nt, tgt, "rest");
    int has_rest = (rest >= 0 && nt_type(nt, rest) && sp_streq(nt_type(nt, rest), "SplatNode"));
    for (int i = 0; i < ln; i++) {
      Buf s; memset(&s, 0, sizeof s);
      buf_printf(&s, "sp_poly_index_poly(%s, sp_box_int(%dLL))", val, (long long)i);
      emit_massign_poly_target(c, lefts[i], s.p, indent, b, sc);
      free(s.p);
    }
    if (has_rest) {
      int inner = nt_ref(nt, rest, "expression");
      if (inner >= 0 && nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
        const char *rnm = nt_str(nt, inner, "name");
        if (rnm) {
          Buf s; memset(&s, 0, sizeof s);
          buf_printf(&s, "sp_poly_slice(%s, %dLL, sp_poly_length(%s) - %dLL - %dLL)",
                     val, (long long)ln, val, (long long)ln, (long long)rn);
          emit_pm_typed_assign(sc, rnm, s.p, b, indent);
          free(s.p);
        }
      }
    }
    for (int j = 0; j < rn; j++) {
      Buf s; memset(&s, 0, sizeof s);
      buf_printf(&s, "sp_poly_index_poly(%s, sp_box_int(sp_poly_length(%s) - %dLL + %dLL))",
                 val, val, (long long)rn, (long long)j);
      emit_massign_poly_target(c, rights[j], s.p, indent, b, sc);
      free(s.p);
    }
  }
}

/* case/in pattern match. tail=1: each arm's body is in method-return position
   (emitted via emit_stmts_tail), so arms diverge and no fallthrough label is
   needed. tail=0: statement form, arms fall through to a shared end label.
   value_cr >= 0: value form -- each arm assigns its body value to _t<value_cr>
   (boxed to the case's result type) then jumps to the end label. */
/* Materialize MatchData#deconstruct_keys (the regex's named captures as a
   symbol-keyed hash) into a fresh GC-rooted SymPolyHash temp, so a hash pattern
   can match/bind against a MatchData scrutinee. `md` is a C expression naming
   the sp_MatchData* (it must be side-effect-free; callers pass a plain temp).
   Returns the temp number of the resulting SymPolyHash. */
static int emit_md_deconstruct_keys(Buf *b, int indent, const char *md) {
  int dk = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new();\n", dk);
  emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", dk);
  emit_indent(b, indent);
  buf_printf(b,
    "if (%s) for (int _di = 0, _dn = re_num_named((%s)->pat); _di < _dn; _di++) {"
    " int _dg = -1; const char *_dnm = re_named_name((%s)->pat, _di, &_dg);"
    " if (_dnm) sp_SymPolyHash_set(_t%d, sp_sym_intern(_dnm),"
    " sp_box_nullable_str(sp_MatchData_aref((%s), _dg))); }\n",
    md, md, md, dk, md);
  return dk;
}

void emit_case_match(Compiler *c, int id, Buf *b, int indent, int tail, int value_cr) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int cn = 0;
  const int *conds = nt_arr(nt, id, "conditions", &cn);
  int else_clause = nt_ref(nt, id, "else_clause");
  TyKind rt = value_cr >= 0 ? comp_ntype(c, id) : TY_UNKNOWN;

  int t = ++g_tmp;
  int lbl = ++g_tmp;
  TyKind pt = (pred >= 0) ? comp_ntype(c, pred) : TY_POLY;
  if (pt == TY_UNKNOWN) pt = TY_POLY;
  /* Evaluate the scrutinee first so its own prelude is flushed to g_pre before
     the `_tN =` initializer. In value position b IS g_pre, so emitting the
     scrutinee inline would splice its prelude into the middle of this line. */
  Buf sb; memset(&sb, 0, sizeof sb);
  if (pred >= 0) sb = expr_buf(c, pred);
  emit_indent(b, indent); emit_ctype(c, pt, b);
  buf_printf(b, " _t%d = ", t);
  buf_puts(b, sb.p ? sb.p : default_value(pt));
  free(sb.p);
  buf_puts(b, ";\n");
  if (needs_root(pt)) {
    emit_indent(b, indent);
    emit_gc_root_tmp(c, pt, t, b);
    buf_puts(b, "\n");
  }

  for (int w = 0; w < cn; w++) {
    const char *cty = nt_type(nt, conds[w]);
    if (!cty || !sp_streq(cty, "InNode")) continue;
    int pat = nt_ref(nt, conds[w], "pattern");
    int stmts = nt_ref(nt, conds[w], "statements");
    if (pat < 0) continue;
    const char *pty = nt_type(nt, pat);
    if (!pty) continue;

    /* `in PATTERN if GUARD` (or `unless GUARD`) is parsed as an If/UnlessNode
       wrapping the real pattern. Unwrap it so the pattern flows through the
       normal match/bind logic, and apply the guard after the bindings are in
       scope (a guard can reference them). */
    int arm_guard = -1, arm_guard_negate = 0;
    if (sp_streq(pty, "IfNode") || sp_streq(pty, "UnlessNode")) {
      int istmts = nt_ref(nt, pat, "statements");
      int in = 0; const int *ib = istmts >= 0 ? nt_arr(nt, istmts, "body", &in) : NULL;
      int gp = nt_ref(nt, pat, "predicate");
      if (in >= 1 && gp >= 0) {
        arm_guard = gp;
        arm_guard_negate = sp_streq(pty, "UnlessNode");
        pat = ib[0];
        pty = nt_type(nt, pat);
        if (!pty) continue;
      }
    }

    emit_indent(b, indent); buf_puts(b, "{\n");

    /* A hash pattern against a MatchData scrutinee matches via
       MatchData#deconstruct_keys: materialize the named captures into a
       symbol-keyed hash, then run the ordinary hash-pattern match/bind against
       it. arm_t/arm_pt shadow the scrutinee for just this arm. */
    int arm_t = t;
    TyKind arm_pt = pt;
    int reject_arm = 0;
    int poly_class_guard = -1;   /* `_tN` bool: the poly subject is the pattern's class */
    if (pt == TY_MATCHDATA && sp_streq(pty, "HashPatternNode")) {
      char md[24]; snprintf(md, sizeof md, "_t%d", t);
      arm_t = emit_md_deconstruct_keys(b, indent + 1, md);
      arm_pt = TY_SYM_POLY_HASH;
    }
    /* a hash pattern asks a Time for #deconstruct_keys: its fields (#3702) */
    else if (pt == TY_TIME && sp_streq(pty, "HashPatternNode")) {
      arm_t = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_SymPolyHash *_t%d = sp_time_deconstruct_all(_t%d); SP_GC_ROOT(_t%d);\n",
                 arm_t, t, arm_t);
      arm_pt = TY_SYM_POLY_HASH;
    }
    /* an array pattern asks a MatchData for #deconstruct: its captures (#3675) */
    else if (pt == TY_MATCHDATA && sp_streq(pty, "ArrayPatternNode")) {
      arm_t = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_PolyArray *_t%d = sp_MatchData_captures(_t%d); SP_GC_ROOT(_t%d);\n",
                 arm_t, t, arm_t);
      arm_pt = TY_POLY_ARRAY;
    }
    /* A user object scrutinee is asked to deconstruct itself: #deconstruct for an
       array pattern, #deconstruct_keys for a hash pattern. Materialize the result
       into a temp and match/bind against that (mirrors the MatchData path). A
       value-type object is passed to #deconstruct by value; a heap object by
       pointer. */
    else if (ty_is_object(pt) && sp_streq(pty, "ArrayPatternNode")) {
      int isv = c->classes[ty_object_class(pt)].is_value_type;
      int ddef = -1;
      int dm = comp_method_in_chain(c, ty_object_class(pt), "deconstruct", &ddef);
      if (dm >= 0) {
        TyKind drt = (TyKind)c->scopes[dm].ret;
        if (!ty_is_array(drt)) drt = TY_POLY_ARRAY;
        const char *dcn = c->classes[ddef].name;
        arm_t = ++g_tmp;
        emit_indent(b, indent + 1); emit_ctype(c, drt, b);
        if (isv) buf_printf(b, " _t%d = sp_%s_deconstruct(_t%d);\n", arm_t, dcn, t);
        else     buf_printf(b, " _t%d = sp_%s_deconstruct((sp_%s *)_t%d);\n", arm_t, dcn, dcn, t);
        if (needs_root(drt)) { emit_indent(b, indent + 1); emit_gc_root_tmp(c, drt, arm_t, b); buf_puts(b, "\n"); }
        arm_pt = drt;
      }
      else if (c->classes[ty_object_class(pt)].is_struct) {
        /* Struct/Data classes have no explicit #deconstruct method, but pattern
           matching still requires one. Synthesize it inline: the members boxed
           into a PolyArray, in declaration order (same shape as Struct#to_a). */
        ClassInfo *sc = &c->classes[ty_object_class(pt)];
        arm_t = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", arm_t, arm_t);
        for (int i = 0; i < sc->nivars; i++) {
          char fb[300];
          if (isv) snprintf(fb, sizeof fb, "(_t%d).iv_%s", t, iv_c(sc->ivars[i] + 1));
          else     snprintf(fb, sizeof fb, "((sp_%s *)_t%d)->iv_%s", sc->c_name, t, iv_c(sc->ivars[i] + 1));
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", arm_t);
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");\n");
        }
        arm_pt = TY_POLY_ARRAY;
      }
    }
    else if (ty_is_object(pt) && sp_streq(pty, "HashPatternNode")) {
      int isv = c->classes[ty_object_class(pt)].is_value_type;
      int ddef = -1;
      int dm = comp_method_in_chain(c, ty_object_class(pt), "deconstruct_keys", &ddef);
      if (dm >= 0) {
        TyKind drt = (TyKind)c->scopes[dm].ret;
        if (!ty_is_hash(drt)) drt = TY_SYM_POLY_HASH;
        const char *dcn = c->classes[ddef].name;
        arm_t = ++g_tmp;
        emit_indent(b, indent + 1); emit_ctype(c, drt, b);
        Buf kab2; memset(&kab2, 0, sizeof kab2);
        emit_pm_deconstruct_keys_arg(c, pat, &kab2);
        if (isv) buf_printf(b, " _t%d = sp_%s_deconstruct_keys(_t%d, %s);\n", arm_t, dcn, t, kab2.p ? kab2.p : "sp_box_nil()");
        else     buf_printf(b, " _t%d = sp_%s_deconstruct_keys((sp_%s *)_t%d, %s);\n", arm_t, dcn, dcn, t, kab2.p ? kab2.p : "sp_box_nil()");
        free(kab2.p);
        if (needs_root(drt)) { emit_indent(b, indent + 1); emit_gc_root_tmp(c, drt, arm_t, b); buf_puts(b, "\n"); }
        arm_pt = drt;
      }
      else if (c->classes[ty_object_class(pt)].is_struct) {
        /* Struct/Data #deconstruct_keys synthesized inline: members keyed by
           their symbol names into a SymPolyHash (same shape as Struct#to_h). */
        ClassInfo *sc = &c->classes[ty_object_class(pt)];
        arm_t = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n", arm_t, arm_t);
        for (int i = 0; i < sc->nivars; i++) {
          char fb[300];
          if (isv) snprintf(fb, sizeof fb, "(_t%d).iv_%s", t, iv_c(sc->ivars[i] + 1));
          else     snprintf(fb, sizeof fb, "((sp_%s *)_t%d)->iv_%s", sc->c_name, t, iv_c(sc->ivars[i] + 1));
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", arm_t, comp_sym_intern(c, sc->ivars[i] + 1));
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");\n");
        }
        arm_pt = TY_SYM_POLY_HASH;
      }
    }
    /* A poly subject with a class-qualified pattern (`A[x]` / `User[name:]`,
       common when a case subject unifies several Struct/Data classes): check
       the runtime class, then deconstruct AS that class. Without this the
       constant is dropped and the arm is compiled as a bare array/hash pattern
       -- an is-Array check that a Struct instance never satisfies (#3179/#3180). */
    else if (pt == TY_POLY &&
             (sp_streq(pty, "ArrayPatternNode") || sp_streq(pty, "HashPatternNode"))) {
      int cnode = nt_ref(nt, pat, "constant");
      int ccid = -1;
      if (cnode >= 0 && nt_type(nt, cnode) &&
          (sp_streq(nt_type(nt, cnode), "ConstantReadNode") ||
           sp_streq(nt_type(nt, cnode), "ConstantPathNode"))) {
        const char *ccn = nt_str(nt, cnode, "name");
        if (ccn) { const char *ra = resolve_class_alias(c, ccn); if (ra) ccn = ra; ccid = comp_class_index(c, ccn); }
      }
      if (ccid >= 0 && c->classes[ccid].is_struct) {
        ClassInfo *sc = &c->classes[ccid];
        int isv = sc->is_value_type;
        char subj[24]; snprintf(subj, sizeof subj, "_t%d", t);
        poly_class_guard = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "int _t%d = (", poly_class_guard);
        Buf gb; memset(&gb, 0, sizeof gb);
        if (!emit_poly_class_when(c, cnode, subj, &gb)) buf_puts(&gb, "0");
        buf_puts(b, gb.p ? gb.p : "0"); free(gb.p);
        buf_puts(b, ");\n");
        int is_hash = sp_streq(pty, "HashPatternNode");
        arm_t = ++g_tmp;
        emit_indent(b, indent + 1);
        if (is_hash) buf_printf(b, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n", arm_t, arm_t);
        else         buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", arm_t, arm_t);
        emit_indent(b, indent + 1);
        buf_printf(b, "if (_t%d) {\n", poly_class_guard);
        for (int i = 0; i < sc->nivars; i++) {
          char fb[320];
          if (isv) snprintf(fb, sizeof fb, "((sp_%s *)_t%d.v.p)->iv_%s", sc->c_name, t, iv_c(sc->ivars[i] + 1));
          else     snprintf(fb, sizeof fb, "((sp_%s *)_t%d.v.p)->iv_%s", sc->c_name, t, iv_c(sc->ivars[i] + 1));
          emit_indent(b, indent + 2);
          if (is_hash) buf_printf(b, "sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", arm_t, comp_sym_intern(c, sc->ivars[i] + 1));
          else         buf_printf(b, "sp_PolyArray_push(_t%d, ", arm_t);
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");\n");
        }
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        arm_pt = is_hash ? TY_SYM_POLY_HASH : TY_POLY_ARRAY;
      }
    }
    /* An array pattern needs an array scrutinee. After the deconstruct dispatch
       above, arm_pt is an array type when #deconstruct matched; otherwise a
       non-array, non-poly scrutinee (a value-type object, an object without
       #deconstruct, or a scalar) can never match -- fail the arm closed rather
       than emit `->len` / array accessors on a non-array pointer. (A hash
       pattern already fails closed below via a null hash cname.) */
    if (sp_streq(pty, "ArrayPatternNode") && !ty_is_array(arm_pt) && arm_pt != TY_POLY)
      reject_arm = 1;

    /* --- compute match condition --- */
    Buf cond_buf = {NULL, 0, 0};
    int has_cond;
    /* find pattern `in [*head, m1..mk, *tail]`: scan for the first window of
       k consecutive elements matching the requireds; record its start in a
       position temp (-1 if none). The arm matches when that position >= 0. */
    int find_pat = -1, find_pos = -1, find_arr = t;
    const char *find_k = NULL;
    if (sp_streq(pty, "FindPatternNode") && !ty_is_array(pt) && pt != TY_POLY) {
      /* A statically non-array, non-poly scrutinee (a scalar or object) can never
         match a find pattern; fail closed rather than emit a coercion of a
         non-sp_RbVal value (which would not compile). */
      buf_puts(&cond_buf, "0");
      has_cond = 1;
    }
    else if (sp_streq(pty, "FindPatternNode")) {
      find_pat = pat;
      find_arr = t;
      TyKind elem_t;
      if (ty_is_array(pt)) {
        find_k = (pt == TY_POLY_ARRAY) ? "Poly" : array_kind(pt);
        if (!find_k) find_k = "Int";
        elem_t = ty_array_elem(pt);
      }
      else {
        /* A poly scrutinee: coerce the boxed value to a poly array at runtime and
           scan that. sp_poly_to_poly_array yields an empty array for a non-array
           value, so a scalar or nil never matches. Root it -- the per-element
           conditions and the arm bindings can allocate and trigger GC. */
        find_k = "Poly";
        find_arr = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_PolyArray *_t%d = sp_poly_to_poly_array(_t%d); SP_GC_ROOT(_t%d);\n", find_arr, t, find_arr);
        elem_t = TY_POLY;
      }
      if (elem_t == TY_UNKNOWN) elem_t = TY_POLY;
      int rn = 0;
      const int *reqs = nt_arr(nt, pat, "requireds", &rn);
      find_pos = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_int _t%d = -1;\n", find_pos);
      emit_indent(b, indent + 1);
      buf_printf(b, "for (sp_int _fi = 0; _t%d && _fi + %dLL <= _t%d->len; _fi++) {\n",
                 find_arr, rn, find_arr);
      Buf wb = {NULL, 0, 0};
      for (int j = 0; j < rn; j++) {
        int e = ++g_tmp;
        emit_indent(b, indent + 2);
        emit_ctype(c, elem_t, b);
        buf_printf(b, " _t%d = sp_%sArray_get(_t%d, _fi + %dLL);\n", e, find_k, find_arr, j);
        Buf rcb = {NULL, 0, 0};
        if (emit_pm_cond(c, reqs[j], e, elem_t, &rcb)) {
          buf_puts(&wb, " && "); buf_puts(&wb, rcb.p ? rcb.p : "1");
        }
        free(rcb.p);
      }
      emit_indent(b, indent + 2);
      buf_printf(b, "if (1%s) { _t%d = _fi; break; }\n", wb.p ? wb.p : "", find_pos);
      free(wb.p);
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      buf_printf(&cond_buf, "_t%d >= 0", find_pos);
      has_cond = 1;
    }
    else if (sp_streq(pty, "HashPatternNode")) {
      /* hash pattern `in {k: subpat, ...}`: matches when the scrutinee is a hash
         that has every key and each value matches its sub-pattern. Compute the
         result into a bool temp (like the find pattern), then bind below. */
      int hcond = ++g_tmp;
      const char *hn = ty_is_hash(arm_pt) ? ty_hash_cname(arm_pt) : NULL;
      emit_indent(b, indent + 1);
      buf_printf(b, "int _t%d = %s;\n", hcond, hn ? "1" : "0");
      if (hn) {
        TyKind hvt = ty_hash_val(arm_pt);
        int en = 0;
        const int *elms = nt_arr(nt, pat, "elements", &en);
        for (int i = 0; i < en; i++) {
          if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
          int key = nt_ref(nt, elms[i], "key");
          int vpat = nt_ref(nt, elms[i], "value");
          if (key < 0) continue;
          /* a symbol pattern key can never be present in a string-keyed hash
             (and vice versa): the arm statically fails, and emitting the
             lookup would type-clash in C. */
          {
            const char *kty2 = nt_type(nt, key);
            TyKind hkt2 = ty_hash_key(arm_pt);
            int mism = (kty2 && sp_streq(kty2, "SymbolNode") && (hkt2 == TY_STRING)) ||
                       (kty2 && sp_streq(kty2, "StringNode") && (hkt2 == TY_SYMBOL || hkt2 == TY_INT)) ||
                       (kty2 && sp_streq(kty2, "IntegerNode") && (hkt2 == TY_STRING || hkt2 == TY_SYMBOL));
            if (mism) {
              emit_indent(b, indent + 1);
              buf_printf(b, "_t%d = 0;\n", hcond);
              continue;
            }
          }
          emit_indent(b, indent + 1);
          buf_printf(b, "_t%d = _t%d && sp_%sHash_has_key(_t%d, ", hcond, hcond, hn, arm_t);
          emit_expr(c, key, b); buf_puts(b, ");\n");
          /* value sub-pattern: `k: 0`, `k: Class`, `k: 1 | 2`, `k: PAT => v`,
             a nested container pattern... -- fetch the value and recurse into
             the general matcher. A bare LV target is a pure binding (bound
             below), so only presence gates it. */
          int vchk = vpat;
          if (vchk >= 0 && nt_type(nt, vchk) && sp_streq(nt_type(nt, vchk), "CapturePatternNode"))
            vchk = nt_ref(nt, vchk, "value");
          if (vchk >= 0 && nt_type(nt, vchk) &&
              !sp_streq(nt_type(nt, vchk), "LocalVariableTargetNode") &&
              !sp_streq(nt_type(nt, vchk), "ImplicitNode")) {
            int vtmp = ++g_tmp;
            emit_indent(b, indent + 1);
            emit_ctype(c, hvt, b);
            buf_printf(b, " _t%d = sp_%sHash_get(_t%d, ", vtmp, hn, arm_t);
            emit_expr(c, key, b); buf_puts(b, ");\n");
            Buf vcb = {NULL, 0, 0};
            int hv = emit_pm_cond(c, vchk, vtmp, hvt, &vcb);
            if (!hv) unsupported(c, vchk, "hash-pattern value sub-form");
            emit_indent(b, indent + 1);
            buf_printf(b, "_t%d = _t%d && (%s);\n", hcond, hcond,
                       vcb.p ? vcb.p : "1");
            free(vcb.p);
          }
        }
      }
      else if (arm_pt == TY_POLY) {
        /* poly scrutinee: match at runtime via the boxed-value hash walk (it
           checks the tag is a hash, every key is present, and each value
           sub-pattern matches) instead of failing closed on a null cname. */
        char es[24]; snprintf(es, sizeof es, "_t%d", arm_t);
        Buf pc = {NULL, 0, 0};
        emit_pm_hash_cond_poly(c, pat, es, &pc);
        emit_indent(b, indent + 1);
        buf_printf(b, "_t%d = (%s);\n", hcond, pc.p ? pc.p : "0");
        free(pc.p);
      }
      /* `**nil`: no keys beyond the listed ones */
      {
        int hp_rest = nt_ref(nt, pat, "rest");
        if (hn && hp_rest >= 0 && nt_type(nt, hp_rest) &&
            sp_streq(nt_type(nt, hp_rest), "NoKeywordsParameterNode")) {
          int en2 = 0, listed = 0;
          const int *elms2 = nt_arr(nt, pat, "elements", &en2);
          for (int i = 0; i < en2; i++)
            if (nt_type(nt, elms2[i]) && sp_streq(nt_type(nt, elms2[i]), "AssocNode")) listed++;
          emit_indent(b, indent + 1);
          buf_printf(b, "_t%d = _t%d && (_t%d->len == %dLL);\n", hcond, hcond, arm_t, listed);
        }
      }
      buf_printf(&cond_buf, "_t%d", hcond);
      has_cond = 1;
    }
    else if (reject_arm) {
      buf_puts(&cond_buf, "0");
      has_cond = 1;
    }
    else {
      /* A hash pattern reached here (e.g. inside an alternation `{a:} | {b:}`)
         emits statement-form helpers that reference the subject `_t`; route
         them into this arm's block, after the subject, not the g_pre prologue
         (#3185). */
      Buf *saved_sink = g_pm_hash_sink;
      int saved_sink_indent = g_pm_hash_sink_indent;
      g_pm_hash_sink = b;
      g_pm_hash_sink_indent = indent + 1;
      has_cond = emit_pm_cond(c, pat, arm_t, arm_pt, &cond_buf);
      g_pm_hash_sink = saved_sink;
      g_pm_hash_sink_indent = saved_sink_indent;
    }
    /* For IfNode the pattern is always a binding (LV), guard is separate */
    if (sp_streq(pty, "IfNode")) has_cond = 0;

    int body_indent = indent + 1;
    if (poly_class_guard >= 0) {
      /* the deconstruct above only populated arm_t when the runtime class
         matched: the arm matches only when the guard AND the element/value
         pattern hold (#3179/#3180). */
      emit_indent(b, indent + 1);
      if (has_cond) buf_printf(b, "if (_t%d && (%s)) {\n", poly_class_guard, cond_buf.p ? cond_buf.p : "1");
      else buf_printf(b, "if (_t%d) {\n", poly_class_guard);
      body_indent = indent + 2;
    }
    else if (has_cond) {
      emit_indent(b, indent + 1);
      buf_printf(b, "if (%s) {\n", cond_buf.p ? cond_buf.p : "1");
      body_indent = indent + 2;
    }
    free(cond_buf.p);

    /* --- bindings --- */
    int guard = arm_guard;
    int array_pat = -1;

    if (sp_streq(pty, "LocalVariableTargetNode")) {
      const char *lnm = nt_str(nt, pat, "name");
      if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
      else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
    }
    else if (sp_streq(pty, "IfNode")) {
      guard = nt_ref(nt, pat, "predicate");
      int bs = nt_ref(nt, pat, "statements");
      if (bs >= 0 && nt_type(nt, bs) && sp_streq(nt_type(nt, bs), "StatementsNode")) {
        int bn = 0;
        const int *body = nt_arr(nt, bs, "body", &bn);
        for (int k = 0; k < bn; k++) {
          const char *bty = nt_type(nt, body[k]);
          if (bty && sp_streq(bty, "LocalVariableTargetNode")) {
            const char *lnm = nt_str(nt, body[k], "name");
            if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
            else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
          }
        }
      }
    }
    else if (sp_streq(pty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, pat, "target");
      if (tgt >= 0 && nt_type(nt, tgt) &&
          sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
        else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
      }
      int val = nt_ref(nt, pat, "value");
      if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ArrayPatternNode"))
        array_pat = val;
    }
    else if (sp_streq(pty, "ArrayPatternNode")) {
      array_pat = pat;
    }
    else if (sp_streq(pty, "HashPatternNode")) {
      /* bind each value target from the hash: `{k:}` (shorthand) and `{k: v}`
         bind the value to a local; `{k: Class => v}` binds the capture target.
         The value is assigned through the local's declared C type. */
      const char *hn = ty_is_hash(arm_pt) ? ty_hash_cname(arm_pt) : NULL;
      if (hn) {
        TyKind hvt = ty_hash_val(arm_pt);
        Scope *hsc = comp_scope_of(c, id);
        int en = 0;
        const int *elms = nt_arr(nt, pat, "elements", &en);
        for (int i = 0; i < en; i++) {
          if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
          int key = nt_ref(nt, elms[i], "key");
          int vpat = nt_ref(nt, elms[i], "value");
          if (key < 0) continue;
          /* resolve the bound local name: shorthand uses the key symbol */
          const char *lnm = NULL;
          int vsub = -1;   /* nested container value pattern to descend into */
          if (vpat < 0 || (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "ImplicitNode"))) {
            if (nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) lnm = nt_str(nt, key, "value");
          }
          else if (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "LocalVariableTargetNode")) {
            lnm = nt_str(nt, vpat, "name");
          }
          else if (nt_type(nt, vpat) && sp_streq(nt_type(nt, vpat), "CapturePatternNode")) {
            int tgt = nt_ref(nt, vpat, "target");
            if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
              lnm = nt_str(nt, tgt, "name");
            vsub = nt_ref(nt, vpat, "value");
          }
          else vsub = vpat;
          /* a nested container value ({a: {b:}}, {data: [*, y, *]}) binds its
             inner names from the fetched value through the poly binders */
          if (vsub >= 0 && hvt == TY_POLY && nt_type(nt, vsub) &&
              (sp_streq(nt_type(nt, vsub), "HashPatternNode") ||
               sp_streq(nt_type(nt, vsub), "ArrayPatternNode") ||
               sp_streq(nt_type(nt, vsub), "FindPatternNode"))) {
            Buf vg; memset(&vg, 0, sizeof vg);
            buf_printf(&vg, "sp_%sHash_get(_t%d, ", hn, arm_t);
            emit_expr(c, key, &vg); buf_puts(&vg, ")");
            emit_indent(b, body_indent); buf_puts(b, "{\n");
            emit_pm_bind_container_poly(c, vsub, vg.p, body_indent + 1, b, hsc);
            emit_indent(b, body_indent); buf_puts(b, "}\n");
            free(vg.p);
          }
          if (!lnm) continue;
          {
            const char *kty2 = nt_type(nt, key);
            TyKind hkt2 = ty_hash_key(arm_pt);
            if ((kty2 && sp_streq(kty2, "SymbolNode") && (hkt2 == TY_STRING)) ||
                (kty2 && sp_streq(kty2, "StringNode") && (hkt2 == TY_SYMBOL || hkt2 == TY_INT)) ||
                (kty2 && sp_streq(kty2, "IntegerNode") && (hkt2 == TY_STRING || hkt2 == TY_SYMBOL)))
              continue;  /* arm statically failed; nothing to bind */
          }
          LocalVar *hlv = hsc ? scope_local(hsc, lnm) : NULL;
          TyKind ltype = hlv ? hlv->type : TY_UNKNOWN;
          if (hvt == TY_POLY && ltype != TY_UNKNOWN && ltype != TY_POLY) {
            /* unbox the poly hash value into the concrete local type via the
               shared typed-assign helper, which coerces arrays / string / scalars
               correctly (the old inline chain fell through to assigning an
               sp_RbVal into a pointer-typed local such as an array). */
            int vtmp = ++g_tmp;
            emit_indent(b, body_indent); buf_puts(b, "{\n");
            emit_indent(b, body_indent + 1);
            buf_printf(b, "sp_RbVal _t%d = sp_%sHash_get(_t%d, ", vtmp, hn, arm_t);
            emit_expr(c, key, b); buf_puts(b, ");\n");
            char vn[24]; snprintf(vn, sizeof vn, "_t%d", vtmp);
            emit_pm_typed_assign(hsc, lnm, vn, b, body_indent + 1);
            emit_indent(b, body_indent); buf_puts(b, "}\n");
          }
          else {
            emit_indent(b, body_indent);
            buf_printf(b, "lv_%s = sp_%sHash_get(_t%d, ", lnm, hn, arm_t);
            emit_expr(c, key, b); buf_puts(b, ");\n");
          }
        }
        /* `**rest`: copy every pair whose key is not among the listed ones */
        int hp_rest = nt_ref(nt, pat, "rest");
        if (hp_rest >= 0 && nt_type(nt, hp_rest) &&
            sp_streq(nt_type(nt, hp_rest), "AssocSplatNode")) {
          int rin = nt_ref(nt, hp_rest, "value");
          const char *rnm = (rin >= 0 && nt_type(nt, rin) &&
                             sp_streq(nt_type(nt, rin), "LocalVariableTargetNode"))
                            ? nt_str(nt, rin, "name") : NULL;
          if (rnm) {
            TyKind hkt = ty_hash_key(arm_pt);
            int tr = ++g_tmp, ti2 = ++g_tmp, tk2 = ++g_tmp;
            emit_indent(b, body_indent);
            buf_printf(b, "{ sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hn, tr, hn, tr);
            emit_indent(b, body_indent + 1);
            buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, arm_t, ti2);
            emit_indent(b, body_indent + 2);
            emit_ctype(c, hkt, b);
            if (arm_pt == TY_POLY_POLY_HASH)
              buf_printf(b, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk2, arm_t, arm_t, ti2);
            else
              buf_printf(b, " _t%d = _t%d->order[_t%d];\n", tk2, arm_t, ti2);
            for (int i = 0; i < en; i++) {
              if (!nt_type(nt, elms[i]) || !sp_streq(nt_type(nt, elms[i]), "AssocNode")) continue;
              int key = nt_ref(nt, elms[i], "key");
              if (key < 0) continue;
              {
                const char *kty2 = nt_type(nt, key);
                if ((kty2 && sp_streq(kty2, "SymbolNode") && (hkt == TY_STRING)) ||
                    (kty2 && sp_streq(kty2, "StringNode") && (hkt == TY_SYMBOL || hkt == TY_INT)) ||
                    (kty2 && sp_streq(kty2, "IntegerNode") && (hkt == TY_STRING || hkt == TY_SYMBOL)))
                  continue;  /* can't be present: no exclusion needed */
              }
              emit_indent(b, body_indent + 2);
              if (hkt == TY_POLY) {
                buf_printf(b, "if (sp_poly_eq(_t%d, ", tk2); emit_boxed(c, key, b);
              }
              else if (hkt == TY_STRING) {
                buf_printf(b, "if (_t%d && strcmp(_t%d, ", tk2, tk2); emit_expr(c, key, b); buf_puts(b, ") == 0");
              }
              else {
                buf_printf(b, "if (_t%d == (", tk2); emit_expr(c, key, b);
              }
              buf_puts(b, ")) continue;\n");
            }
            emit_indent(b, body_indent + 2);
            buf_printf(b, "sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d));\n",
                       hn, tr, tk2, hn, arm_t, tk2);
            emit_indent(b, body_indent + 1); buf_puts(b, "}\n");
            emit_indent(b, body_indent + 1);
            {
              LocalVar *rlv2 = hsc ? scope_local(hsc, rnm) : NULL;
              if (rlv2 && rlv2->type == TY_POLY) {
                char rv[24]; snprintf(rv, sizeof rv, "_t%d", tr);
                Buf bx; memset(&bx, 0, sizeof bx);
                emit_boxed_text(c, arm_pt, rv, &bx);
                buf_printf(b, "lv_%s = %s;\n", rnm, bx.p ? bx.p : rv);
                free(bx.p);
              }
              else buf_printf(b, "lv_%s = _t%d;\n", rnm, tr);
            }
            emit_indent(b, body_indent); buf_puts(b, "}\n");
          }
        }
      }
      else if (arm_pt == TY_POLY) {
        /* poly scrutinee: bind the value targets through the boxed-value hash
           binder (the condition above already confirmed a matching hash). */
        Scope *hsc = comp_scope_of(c, id);
        char es[24]; snprintf(es, sizeof es, "_t%d", arm_t);
        emit_pm_bind_hash_poly(c, pat, es, body_indent, b, hsc);
      }
    }
    /* IntegerNode/StringNode/SymbolNode/ConstantReadNode: value-only, no binding */

    /* --- ArrayPatternNode destructuring --- */
    if (!reject_arm && array_pat >= 0) {
      /* A poly scrutinee -- a bare sp_RbVal (TY_POLY) or a poly-array pointer
         (TY_POLY_ARRAY) -- binds (possibly nested) targets through the poly-safe
         recursive path, indexing the boxed receiver so nested typed sub-arrays
         read correctly. A bare value is already an sp_RbVal; a poly array is
         boxed. Typed int/float/str arrays keep the direct accessor path. */
      const char *k = (ty_is_array(arm_pt) && arm_pt != TY_POLY_ARRAY) ? array_kind(arm_pt) : NULL;
      if (!k) {
        char te[48];
        if (arm_pt == TY_POLY_ARRAY) snprintf(te, sizeof te, "sp_box_poly_array(_t%d)", arm_t);
        else                         snprintf(te, sizeof te, "_t%d", arm_t);
        emit_pm_bind_poly(c, array_pat, te, body_indent, b, comp_scope_of(c, id));
      }
      else {
      TyKind arr_t = arm_pt;
      int apn = 0;
      const int *reqs = nt_arr(nt, array_pat, "requireds", &apn);
      int rest_nid = nt_ref(nt, array_pat, "rest");
      int npost = 0;
      const int *posts = nt_arr(nt, array_pat, "posts", &npost);
      for (int i = 0; i < apn; i++) {
        const char *lty2 = nt_type(nt, reqs[i]);
        const char *lnm = NULL;
        if (lty2 && sp_streq(lty2, "LocalVariableTargetNode")) lnm = nt_str(nt, reqs[i], "name");
        else if (lty2 && sp_streq(lty2, "CapturePatternNode")) {
          /* `Class => x`: the class check ran in the arm condition; bind x */
          int tgt = nt_ref(nt, reqs[i], "target");
          if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
            lnm = nt_str(nt, tgt, "name");
        }
        if (!lnm) continue;
        emit_indent(b, body_indent);
        buf_printf(b, "lv_%s = ", lnm);
        LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
        char gx[64]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, %dLL)", k, arm_t, i);
        if (plv && plv->type == TY_POLY && !sp_streq(k, "Poly")) {
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, ty_array_elem(arr_t), gx, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_puts(b, gx);
        buf_puts(b, ";\n");
      }
      if (rest_nid >= 0 && nt_type(nt, rest_nid) &&
          sp_streq(nt_type(nt, rest_nid), "SplatNode")) {
        int inner = nt_ref(nt, rest_nid, "expression");
        if (inner >= 0 && nt_type(nt, inner) &&
            sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
          const char *rnm = nt_str(nt, inner, "name");
          if (rnm) {
            /* the rest is the middle: skip apn leading, drop npost trailing. */
            LocalVar *rlv = scope_local(comp_scope_of(c, id), rnm);
            char sx[96];
            snprintf(sx, sizeof sx, "sp_%sArray_slice(_t%d, %dLL, _t%d->len - %dLL)",
                     k, arm_t, apn, arm_t, apn + npost);
            emit_indent(b, body_indent);
            buf_printf(b, "lv_%s = ", rnm);
            /* a local shared with another arm of a different array kind holds
               the slice boxed, as the required bindings above already do */
            if (rlv && rlv->type == TY_POLY && !sp_streq(k, "Poly")) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, arr_t, sx, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else buf_puts(b, sx);
            buf_puts(b, ";\n");
          }
        }
      }
      /* posts bind from the tail: post j is at index len - (npost - j). */
      for (int j = 0; j < npost; j++) {
        const char *pty2 = nt_type(nt, posts[j]);
        const char *lnm = NULL;
        if (pty2 && sp_streq(pty2, "LocalVariableTargetNode")) lnm = nt_str(nt, posts[j], "name");
        else if (pty2 && sp_streq(pty2, "CapturePatternNode")) {
          int tgt = nt_ref(nt, posts[j], "target");
          if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
            lnm = nt_str(nt, tgt, "name");
        }
        if (!lnm) continue;
        emit_indent(b, body_indent);
        buf_printf(b, "lv_%s = ", lnm);
        LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
        char gx[80]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, _t%d->len - %lldLL)", k, arm_t, arm_t, (long long)(npost - j));
        if (plv && plv->type == TY_POLY && !sp_streq(k, "Poly")) {
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, ty_array_elem(arr_t), gx, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_puts(b, gx);
        buf_puts(b, ";\n");
      }
      }
    }

    /* --- FindPatternNode destructuring (uses the found position temp) --- */
    if (find_pat >= 0 && find_pos >= 0) {
      int rn = 0;
      const int *reqs = nt_arr(nt, find_pat, "requireds", &rn);
      /* leading `*head` = elements before the matched window */
      int left = nt_ref(nt, find_pat, "left");
      if (left >= 0 && nt_type(nt, left) && sp_streq(nt_type(nt, left), "SplatNode")) {
        int inner = nt_ref(nt, left, "expression");
        if (inner >= 0 && nt_type(nt, inner) &&
            sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
          const char *lnm = nt_str(nt, inner, "name");
          if (lnm) {
            emit_indent(b, body_indent);
            buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, 0LL, _t%d);\n",
                       lnm, find_k, find_arr, find_pos);
          }
        }
      }
      /* window targets = the matched elements; a `lit => x` capture binds its
         target (the literal was already checked in the window scan). */
      for (int j = 0; j < rn; j++) {
        const char *lty2 = nt_type(nt, reqs[j]);
        if (!lty2) continue;
        char gx[80]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, _t%d + %dLL)", find_k, find_arr, find_pos, j);
        const char *lnm = NULL;
        if (sp_streq(lty2, "LocalVariableTargetNode")) lnm = nt_str(nt, reqs[j], "name");
        else if (sp_streq(lty2, "CapturePatternNode")) {
          int tgt = nt_ref(nt, reqs[j], "target");
          if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode"))
            lnm = nt_str(nt, tgt, "name");
        }
        if (lnm) {
          emit_indent(b, body_indent);
          buf_printf(b, "lv_%s = ", lnm);
          LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
          if (plv && plv->type == TY_POLY && !sp_streq(find_k, "Poly")) {
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, ty_array_elem(pt), gx, &bx);
            buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(b, gx);
          buf_puts(b, ";\n");
        }
        /* a nested container window element (`[*, [a, b], *]`, `[*, {k:}, *]`)
           or a `PAT => cap` capture binds the names inside it from the same
           element (poly only: a typed array cannot hold a container). */
        int sub = -1;
        if (sp_streq(lty2, "CapturePatternNode")) sub = nt_ref(nt, reqs[j], "value");
        else sub = reqs[j];
        if (sub >= 0 && sp_streq(find_k, "Poly")) {
          /* a PolyArray element is already an sp_RbVal */
          emit_pm_bind_container_poly(c, sub, gx, body_indent, b, comp_scope_of(c, id));
        }
      }
      /* trailing `*tail` = elements after the matched window */
      int right = nt_ref(nt, find_pat, "right");
      if (right >= 0 && nt_type(nt, right) && sp_streq(nt_type(nt, right), "SplatNode")) {
        int inner = nt_ref(nt, right, "expression");
        if (inner >= 0 && nt_type(nt, inner) &&
            sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
          const char *rnm = nt_str(nt, inner, "name");
          if (rnm) {
            emit_indent(b, body_indent);
            buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d + %dLL, _t%d->len - (_t%d + %dLL));\n",
                       rnm, find_k, find_arr, find_pos, rn, find_arr, find_pos, rn);
          }
        }
      }
    }

    /* --- body with optional guard --- */
    if (guard >= 0) {
      emit_indent(b, body_indent); buf_puts(b, arm_guard_negate ? "if (!(" : "if (");
      emit_cond(c, guard, b);  /* Ruby truthiness for every guard type (0/"" are truthy) */
      buf_puts(b, arm_guard_negate ? ")) {\n" : ") {\n");
      if (value_cr >= 0) { emit_pm_body_value(c, stmts, rt, value_cr, b, body_indent + 1); emit_indent(b, body_indent + 1); buf_printf(b, "goto _pm_%d;\n", lbl); }
      else if (tail) { emit_stmts_tail(c, stmts, b, body_indent + 1); emit_indent(b, body_indent + 1); buf_printf(b, "goto _pm_%d;\n", lbl); }
      else { emit_stmts(c, stmts, b, body_indent + 1); emit_indent(b, body_indent + 1); buf_printf(b, "goto _pm_%d;\n", lbl); }
      emit_indent(b, body_indent); buf_puts(b, "}\n");
    }
    else {
      if (value_cr >= 0) { emit_pm_body_value(c, stmts, rt, value_cr, b, body_indent); emit_indent(b, body_indent); buf_printf(b, "goto _pm_%d;\n", lbl); }
      else if (tail) { emit_stmts_tail(c, stmts, b, body_indent); emit_indent(b, body_indent); buf_printf(b, "goto _pm_%d;\n", lbl); }
      else { emit_stmts(c, stmts, b, body_indent); emit_indent(b, body_indent); buf_printf(b, "goto _pm_%d;\n", lbl); }
    }

    if (has_cond) { emit_indent(b, indent + 1); buf_puts(b, "}\n"); }
    emit_indent(b, indent); buf_puts(b, "}\n");
  }

  if (else_clause >= 0) {
    emit_indent(b, indent); buf_puts(b, "{\n");
    if (value_cr >= 0) emit_pm_body_value(c, nt_ref(nt, else_clause, "statements"), rt, value_cr, b, indent + 1);
    else if (tail) emit_stmts_tail(c, nt_ref(nt, else_clause, "statements"), b, indent + 1);
    else emit_stmts(c, nt_ref(nt, else_clause, "statements"), b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
  }
  else {
    /* No matching arm and no else: raise NoMatchingPatternError */
    emit_indent(b, indent);
    buf_printf(b, "sp_raise_cls(\"NoMatchingPatternError\", \"no pattern matched\");\n");
  }

  /* The matched arm jumps here, past the no-match raise. In TAIL mode the arm
     usually ends in a `return`, which made the jump look redundant -- but a
     tail that ASSIGNS (a `begin` block in value position) falls through
     instead, and every such case/in raised NoMatchingPatternError however well
     it had just matched (#4016). The goto after a return is dead, and free. */
  { emit_indent(b, indent); buf_printf(b, "_pm_%d:;\n", lbl); }
}

/* case/when -> an if / else-if chain. Statement form. */
/* True if the subtree contains a `break` that targets the ENCLOSING loop
   (i.e. not captured by a nested loop or block-bearing iterator). Used to
   decide whether a `case` may lower to a C `switch`: a Ruby `break` inside a
   `when` must break the loop, but a C `break` inside a `switch` only exits the
   switch -- so a case whose when-bodies break the loop must use the if-else
   form instead, where C `break` correctly targets the loop. */
static int subtree_has_loop_break(Compiler *c, int root) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (sp_streq(ty, "BreakNode")) return 1;
    /* nested loops / block-bearing iterators capture their own break/next */
    if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode"))
      return 0;
    if (sp_streq(ty, "CallNode") && nt_ref(nt, root, "block") >= 0)
      return 0;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (subtree_has_loop_break(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (subtree_has_loop_break(c, el[j])) return 1;
  }
  return 0;
}

/* `when <string-range>` with a String scrutinee: Range#=== is cover?, i.e.
   lexicographic <=> against both endpoints (byte order, like String#<=>).
   Handles beginless/endless forms. Emits nothing and returns 0 when the
   condition isn't a range with String endpoints. */
static int emit_when_string_range(Compiler *c, int cond, int t, Buf *b) {
  const NodeTable *nt = c->nt;
  /* `when ("a".."e")` wraps the range in parentheses, and the literal check
     below saw only the wrapper -- the arm declined and the whole when folded
     to false (#3694) */
  while (cond >= 0 && nt_type(nt, cond) && sp_streq(nt_type(nt, cond), "ParenthesesNode")) {
    int pb = nt_ref(nt, cond, "body");
    int pn = 0; const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
    if (pn != 1) break;
    cond = pd[0];
  }
  const char *cty = nt_type(nt, cond);
  /* a String range held in a variable has no literal to read: cover it with
     the runtime check */
  if (cty && !sp_streq(cty, "RangeNode") && comp_ntype(c, cond) == TY_STR_RANGE) {
    int trg = ++g_tmp;
    buf_printf(b, "({ sp_StrRange _t%d = ", trg); emit_expr(c, cond, b);
    buf_printf(b, "; sp_srange_cover(_t%d, _t%d); })", trg, t);
    return 1;
  }
  if (!cty || !sp_streq(cty, "RangeNode")) return 0;
  int left = nt_ref(nt, cond, "left");
  int right = nt_ref(nt, cond, "right");
  if (left < 0 && right < 0) return 0;
  if (left >= 0 && comp_ntype(c, left) != TY_STRING) return 0;
  if (right >= 0 && comp_ntype(c, right) != TY_STRING) return 0;
  int excl = (int)(nt_int(nt, cond, "flags", 0) & 4) ? 1 : 0;
  int tl = left >= 0 ? ++g_tmp : -1;
  int tr = right >= 0 ? ++g_tmp : -1;
  buf_puts(b, "({ ");
  if (left >= 0) {
    buf_printf(b, "const char *_t%d = ", tl); emit_expr(c, left, b); buf_puts(b, "; ");
  }
  if (right >= 0) {
    buf_printf(b, "const char *_t%d = ", tr); emit_expr(c, right, b); buf_puts(b, "; ");
  }
  buf_printf(b, "_t%d", t);
  /* A nil endpoint (NULL for a String-typed slot, e.g. an unset ivar) is an
     open bound in CRuby -- `(nil.."m") === "c"` is true -- so a NULL endpoint
     passes its side of the cover check rather than matching or dereferencing. */
  if (left >= 0) buf_printf(b, " && (!_t%d || sp_str_cmp_bytes(_t%d, _t%d) <= 0)", tl, tl, t);
  if (right >= 0) buf_printf(b, " && (!_t%d || sp_str_cmp_bytes(_t%d, _t%d) %s 0)", tr, t, tr, excl ? "<" : "<=");
  buf_puts(b, "; })");
  return 1;
}

static int emit_when_lambda_inline(Compiler *c, int cond, int t, Buf *b);
void emit_case(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int nw = 0;
  const int *whens = nt_arr(nt, id, "conditions", &nw);
  int else_clause = nt_ref(nt, id, "else_clause");

  int t = -1;
  TyKind pt = TY_UNKNOWN;
  if (pred >= 0) {
    pt = comp_ntype(c, pred);
    t = ++g_tmp;
    emit_indent(b, indent);
    if (pt == TY_UNKNOWN || pt == TY_VOID || pt == TY_NIL) {
      emit_ctype(c, TY_POLY, b); buf_printf(b, " _t%d = ", t); emit_boxed(c, pred, b);
      buf_puts(b, ";\n"); pt = TY_POLY;
    }
    else {
      emit_ctype(c, pt, b);
      buf_printf(b, " _t%d = ", t);
      emit_expr(c, pred, b);
      buf_puts(b, ";\n");
    }
  }

  /* Fast path: `case <int/poly> when <integer literals>` lowers to a C switch
     (jump table) instead of an O(n) sp_poly_eq / int-compare if-chain. This is
     the optcarrot CPU opcode dispatch (~256 whens); the poly if-chain made
     sp_poly_eq ~50% of runtime. */
  if (pred >= 0 && (pt == TY_POLY || pt == TY_INT) && nw > 0) {
    int all_int = 1;
    for (int w = 0; w < nw && all_int; w++) {
      int wc = 0; const int *conds = nt_arr(nt, whens[w], "conditions", &wc);
      if (wc == 0) { all_int = 0; break; }
      for (int j = 0; j < wc; j++) {
        const char *cty = nt_type(nt, conds[j]);
        if (!cty || !sp_streq(cty, "IntegerNode")) { all_int = 0; break; }
      }
    }
    /* A C switch can't host a Ruby `break` that targets the enclosing loop
       (C break exits the switch, not the loop). If any arm breaks the loop,
       fall through to the if-else form below where break works correctly. */
    int has_lbreak = (else_clause >= 0) && subtree_has_loop_break(c, nt_ref(nt, else_clause, "statements"));
    for (int w = 0; w < nw && !has_lbreak; w++)
      if (subtree_has_loop_break(c, nt_ref(nt, whens[w], "statements"))) has_lbreak = 1;
    if (all_int && !has_lbreak) {
      emit_indent(b, indent);
      if (pt == TY_POLY) buf_printf(b, "switch (sp_poly_to_i(_t%d)) {\n", t);
      else buf_printf(b, "switch (_t%d) {\n", t);
      for (int w = 0; w < nw; w++) {
        int wc = 0; const int *conds = nt_arr(nt, whens[w], "conditions", &wc);
        for (int j = 0; j < wc; j++) {
          emit_indent(b, indent);
          buf_printf(b, "case %lldLL:\n", (long long)nt_int(nt, conds[j], "value", 0));
        }
        emit_indent(b, indent); buf_puts(b, "{\n");
        emit_stmts(c, nt_ref(nt, whens[w], "statements"), b, indent + 1);
        emit_indent(b, indent + 1); buf_puts(b, "break;\n");
        emit_indent(b, indent); buf_puts(b, "}\n");
      }
      if (else_clause >= 0) {
        emit_indent(b, indent); buf_puts(b, "default: {\n");
        emit_stmts(c, nt_ref(nt, else_clause, "statements"), b, indent + 1);
        emit_indent(b, indent); buf_puts(b, "}\n");
      }
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
  }

  for (int w = 0; w < nw; w++) {
    int wn = whens[w];
    int wc = 0;
    const int *conds = nt_arr(nt, wn, "conditions", &wc);
    emit_indent(b, indent);
    buf_puts(b, w == 0 ? "if (" : "else if (");
    for (int j = 0; j < wc; j++) {
      if (j) buf_puts(b, " || ");
      if (pred >= 0) {
        /* `when *arr` — array membership test */
        if (nt_type(nt, conds[j]) && sp_streq(nt_type(nt, conds[j]), "SplatNode")) {
          int inner = nt_ref(nt, conds[j], "expression");
          TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
          int ta = ++g_tmp;
          if (at == TY_INT_ARRAY) {
            buf_printf(b, "({ sp_IntArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_IntArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_STR_ARRAY) {
            buf_printf(b, "({ sp_StrArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_StrArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_FLOAT_ARRAY) {
            buf_printf(b, "({ sp_FloatArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_FloatArray_include(_t%d, _t%d); })", ta, ta, t);
          }
          else if (at == TY_POLY_ARRAY) {
            buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, inner, b);
            buf_printf(b, "; _t%d && sp_PolyArray_include(_t%d, ", ta, ta);
            emit_boxed(c, pred, b);
            buf_puts(b, "); })");
          }
          else {
            buf_puts(b, "0 /* unsupported splat type */");
          }
        }
        else {
          const char *cnty = nt_type(nt, conds[j]);
          if (emit_when_lambda_inline(c, conds[j], t, b)) { /* literal lambda predicate */ }
          /* `when <proc>` (a variable): Proc#=== calls the proc with the
             subject, via the proc-call ABI (mirrors the case-as-value arm) */
          /* a Proc read out of a container arrives boxed: dispatch on the tag
             so it is CALLED and not compared (#3683) */
          else if (comp_ntype(c, conds[j]) == TY_POLY) {
            char subjp[32]; snprintf(subjp, sizeof subjp, "_t%d", t);
            int tpw = ++g_tmp;
            buf_printf(b, "({ sp_RbVal _t%d = ", tpw); emit_boxed(c, conds[j], b);
            buf_printf(b, "; _t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_PROC"
                          " ? sp_poly_truthy(sp_penum_call1((sp_Proc *)_t%d.v.p, ", tpw, tpw, tpw);
            if (pt == TY_POLY) buf_puts(b, subjp); else emit_boxed_text(c, pt, subjp, b);
            buf_printf(b, ")) : sp_poly_eq(_t%d, ", tpw);
            if (pt == TY_POLY) buf_puts(b, subjp); else emit_boxed_text(c, pt, subjp, b);
            buf_puts(b, "); })");
          }
          else if (comp_ntype(c, conds[j]) == TY_PROC) {
            g_needs_proc_poly_argslot = 1;
            char subj9[32]; snprintf(subj9, sizeof subj9, "_t%d", t);
            buf_puts(b, "({ _sp_proc_poly_args[0] = ");
            if (pt == TY_POLY) buf_puts(b, subj9);
            else emit_boxed_text(c, pt, subj9, b);
            buf_puts(b, "; sp_poly_truthy(((void)sp_proc_call(");
            emit_expr(c, conds[j], b);
            buf_puts(b, ", 1, (sp_int[16]){");
            if (pt == TY_POLY) buf_printf(b, "sp_poly_to_i(%s)", subj9);
            else if (proc_slot_is_ptr(pt)) buf_printf(b, "(sp_int)(uintptr_t)%s", subj9);
            else if (pt == TY_FLOAT) buf_puts(b, "0");
            else buf_puts(b, subj9);
            buf_puts(b, "}), _sp_proc_poly_ret)); })");
          }
          /* `when *arr`: membership -- any element of the splatted array
             matching the scrutinee selects this branch (value equality;
             a Class/Regexp element inside a splat is not #===-dispatched). */
          else if (cnty && sp_streq(cnty, "SplatNode")) {
            int sp_in = nt_ref(nt, conds[j], "expression");
            char stmp[32]; snprintf(stmp, sizeof stmp, "_t%d", t);
            buf_puts(b, "sp_case_splat_match(");
            if (pt == TY_POLY) buf_puts(b, stmp);
            else emit_boxed_text(c, pt, stmp, b);
            buf_puts(b, ", ");
            if (sp_in >= 0) emit_boxed(c, sp_in, b); else buf_puts(b, "sp_box_nil()");
            buf_puts(b, ")");
          }
          /* RationalNode: `when 0r` — matches integer iff denominator==1 */
          else
          if (cnty && sp_streq(cnty, "RationalNode")) {
            const char *rnum = nt_str(nt, conds[j], "rat_num");
            const char *rden = nt_str(nt, conds[j], "rat_den");
            long long den = rden ? atoll(rden) : 1;
            long long num = rnum ? atoll(rnum) : 0;
            if (den == 1) buf_printf(b, "(_t%d == %lldLL)", t, num);
            else buf_puts(b, "0");
          }
          /* ImaginaryNode: `when 0i` — Complex(0,imag); integer matches only if imag==0 */
          else if (cnty && sp_streq(cnty, "ImaginaryNode")) {
            int numnode = nt_ref(nt, conds[j], "numeric");
            long long imval = numnode >= 0 ? (long long)nt_int(nt, numnode, "value", 0) : -1;
            if (imval == 0) buf_printf(b, "(_t%d == 0LL)", t);
            else buf_puts(b, "0");
          }
          else {
          /* when ClassName / when Mod::Klass: Module#=== via is_a? semantics */
          const char *cty2 = nt_type(nt, conds[j]);
          const char *cn2 = cty2 && (sp_streq(cty2, "ConstantReadNode") || sp_streq(cty2, "ConstantPathNode"))
                           ? nt_str(nt, conds[j], "name") : NULL;
          /* a VALUE constant (`STATE_TITLE = :title`; registered in
             comp_const with a real type) in `when` is an equality test,
             not Module#=== -- treating it as a class name folded every
             arm of doom's Menu#render state dispatch to `if (0)`. */
          if (cn2 && ({ LocalVar *_wv = comp_const(c, cn2); _wv && _wv->type != TY_UNKNOWN && _wv->type != TY_CLASS; })) cn2 = NULL;
        /* An `ffi_const` is a value too, and it lives in its own table
           rather than the constant one: read as a class name the arm folded
           to a constant false and the branch was never taken. */
        if (cn2 && comp_ffi_const_at(c, conds[j], NULL)) cn2 = NULL;
          if (cn2 && pt == TY_POLY) {
            char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
            if (!emit_poly_class_when(c, conds[j], tmp, b))
              buf_puts(b, "0");
          }
          else if (cn2 && ty_is_object(pt)) {
            int cid = ty_object_class(pt);
            int tcid = comp_class_index(c, cn2);
            int yes = (tcid >= 0) && (cid == tcid || is_descendant(c, cid, tcid));
            buf_printf(b, "%d", yes ? 1 : 0);
          }
          else if (cn2 && pt == TY_CLASS) {
            /* `when <ClassName>` is ===: a Class VALUE is an instance only
               of Class/Module, never of the named class itself. */
            buf_printf(b, "%d", (sp_streq(cn2, "Class") || sp_streq(cn2, "Module")) ? 1 : 0);
          }
          else if (cn2 && pt == TY_EXCEPTION) {
            /* an exception scrutinee matches by name up its class chain (#2759) */
            buf_printf(b, "sp_exc_is_a((sp_Exception *)_t%d, \"%s\")", t, exc_when_cls_name(c, cn2));
          }
          else if (cn2 && pt == TY_BOOL &&
                   (sp_streq(cn2, "TrueClass") || sp_streq(cn2, "FalseClass"))) {
            /* a boolean's class (TrueClass vs FalseClass) is a runtime value,
               not decidable from the static TY_BOOL type (#2966) */
            buf_printf(b, "(_t%d %s)", t, sp_streq(cn2, "TrueClass") ? "!= 0" : "== 0");
          }
          else if (cn2) {
            int yes = ty_matches_class(pt, cn2, 0);
            buf_printf(b, "%d", yes > 0 ? 1 : 0);
          }
          else {
          int reidx = re_lit_index(c, conds[j]);
          if (reidx >= 0 && pt == TY_STRING) {
            buf_printf(b, "(sp_re_match(sp_re_pat_%d, _t%d) >= 0)", reidx, t);
          }
          else if (reidx >= 0 && pt == TY_POLY) {
            buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, _t%d)", reidx, t);
          }
          else if (reidx >= 0 && pt == TY_SYMBOL) {
            buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, sp_box_sym(_t%d))", reidx, t);
          }
          else if (pt == TY_STRING && emit_when_string_range(c, conds[j], t, b)) {
            /* emitted the lexicographic cover check */
          }
          else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
            /* `when lo..hi` is range membership, not equality */
            int tr = ++g_tmp;
            buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
            /* sp_range_include takes sp_int; coerce a poly scrutinee. */
            if (pt == TY_POLY) buf_printf(b, "; sp_range_include(&_t%d, sp_poly_to_i(_t%d)); })", tr, t);
            else buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
          }
          else if (comp_ntype(c, conds[j]) == TY_FLOAT_RANGE) {
            /* `when 1.0..3.0`: float range membership via sp_frange_cover */
            int tr = ++g_tmp;
            buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, conds[j], b);
            if (pt == TY_POLY) buf_printf(b, "; sp_frange_cover(_t%d, sp_poly_to_f(_t%d)); })", tr, t);
            else buf_printf(b, "; sp_frange_cover(_t%d, (sp_float)_t%d); })", tr, t);
          }
          else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
            /* a when value of a different comparable family never matches */
            buf_puts(b, "0");
          }
          else if (nt_type(nt, conds[j]) && sp_streq(nt_type(nt, conds[j]), "SplatNode")) {
            /* `when *arr`: membership via value equality (see the int-path arm) */
            int sp_in2 = nt_ref(nt, conds[j], "expression");
            char stmp2[32]; snprintf(stmp2, sizeof stmp2, "_t%d", t);
            buf_puts(b, "sp_case_splat_match(");
            if (pt == TY_POLY) buf_puts(b, stmp2);
            else emit_boxed_text(c, pt, stmp2, b);
            buf_puts(b, ", ");
            if (sp_in2 >= 0) emit_boxed(c, sp_in2, b); else buf_puts(b, "sp_box_nil()");
            buf_puts(b, ")");
          }
          else if (pt == TY_BIGINT && comp_ntype(c, conds[j]) == TY_BIGINT) {
            buf_printf(b, "(sp_bigint_cmp(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ") == 0)");
          }
          else if (pt == TY_STRING) {
            /* An arm of another type can never be `===` a String: `when
               ["foo", "foo"]` is Array#=== , which is ==, and a String is not
               an Array. Passing the array pointer to sp_str_eq did not even
               typecheck; evaluate the arm for its effects and answer false. */
            TyKind wat = comp_ntype(c, conds[j]);
            if (wat != TY_STRING && wat != TY_STRBUF && wat != TY_POLY && wat != TY_UNKNOWN) {
              buf_printf(b, "((void)_t%d, (void)(", t); emit_expr(c, conds[j], b); buf_puts(b, "), 0)");
            }
            else {
              buf_printf(b, "sp_str_eq(_t%d, ", t);
              if (wat == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
              else emit_expr(c, conds[j], b);
              buf_puts(b, ")");
            }
          }
          else if (pt == TY_POLY) {
            buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")");
          }
          else {
            /* `when <obj>` is `<obj> === scrutinee`, which for a plain object
               is its own ==; comparing the two pointers answered false for two
               equal instances of a class that defines one (#3741). */
            TyKind wpt = comp_ntype(c, conds[j]);
            int wcid = ty_is_object(wpt) ? ty_object_class(wpt) : -1;
            int wdef = -1;
            int weq = wcid >= 0 ? comp_method_in_chain(c, wcid, "===", &wdef) : -1;
            if (weq < 0 && wcid >= 0) weq = comp_method_in_chain(c, wcid, "==", &wdef);
            if (weq >= 0 && wdef >= 0 && !comp_ty_value_obj(c, wpt)) {
              buf_printf(b, "sp_%s_%s(", c->classes[wdef].c_name, mc(c->scopes[weq].name));
              emit_expr(c, conds[j], b);
              buf_printf(b, ", ");
              /* the user method takes its argument boxed when its parameter is
                 poly, which is the shape these comparison methods settle on */
              { Scope *ws = &c->scopes[weq];
                LocalVar *wp = ws->nparams > 0 ? scope_local(ws, ws->pnames[0]) : NULL;
                TyKind wpt2 = wp ? wp->type : TY_POLY;
                char sref[24]; snprintf(sref, sizeof sref, "_t%d", t);
                if (wpt2 == TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx);
                  emit_boxed_text(c, pt, sref, &bx); buf_puts(b, bx.p ? bx.p : sref); free(bx.p); }
                else buf_puts(b, sref); }
              buf_puts(b, ")");
            }
            else if (ty_is_object(pt) && comp_ntype(c, conds[j]) == pt &&
                 (comp_method_in_chain(c, ty_object_class(pt), "===", NULL) >= 0 ||
                  comp_method_in_chain(c, ty_object_class(pt), "==", NULL) >= 0)) {
          /* `case obj when other`: Ruby asks `other === obj`, which for a class
             defining #== is that method. Comparing the C structs instead did
             not compile for a value-type object and compared addresses for a
             heap one (#3820). */
          int ecid = ty_object_class(pt);
          int emi = comp_method_in_chain(c, ecid, "===", NULL);
          if (emi < 0) emi = comp_method_in_chain(c, ecid, "==", NULL);
          Scope *ems = &c->scopes[emi];
          LocalVar *eplv = ems->nparams > 0 ? scope_local(ems, ems->pnames[0]) : NULL;
          TyKind pty = eplv ? eplv->type : TY_POLY;
          buf_puts(b, "(");
          emit_method_cname(c, ems, b);
          buf_puts(b, "(");
          emit_expr(c, conds[j], b);
          buf_puts(b, ", ");
          { char sref[32]; snprintf(sref, sizeof sref, "_t%d", t);
            if (pty != pt && pt != TY_UNKNOWN) emit_boxed_text(c, pt, sref, b);
            else buf_puts(b, sref); }
          buf_puts(b, "))");
        }
        else {
          char sref2[32]; snprintf(sref2, sizeof sref2, "_t%d", t);
          /* a native handle or value kind answers `cond === subj` the way
             an explicit === does (emit_native_object_protocol) */
          if (!emit_native_case_eq(c, conds[j], pt, sref2, b)) {
            buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
          }
        }
          }
          } /* close non-ConstantReadNode else */
          } /* close else { int reidx... } */
        }
      }
      else {
        buf_puts(b, "("); emit_expr(c, conds[j], b); buf_puts(b, ")");
      }
    }
    buf_puts(b, ") {\n");
    emit_stmts(c, nt_ref(nt, wn, "statements"), b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }

  if (else_clause >= 0) {
    emit_indent(b, indent);
    buf_puts(b, "else {\n");
    emit_stmts(c, nt_ref(nt, else_clause, "statements"), b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }
}

/* `when ->(v) { ... }` with a literal lambda: Proc#=== calls the lambda
   with the scrutinee. Inline the body -- bind the param to the scrutinee
   temp `_tN` and evaluate the last expression as the condition -- so a
   struct-valued scrutinee (sp_Class, sp_Range) never has to ride the
   sp_int proc-call ABI (#2439). Returns 1 when emitted. */
static int emit_when_lambda_inline(Compiler *c, int cond, int t, Buf *b) {
  const NodeTable *nt = c->nt;
  if (!nt_type(nt, cond) || !sp_streq(nt_type(nt, cond), "LambdaNode")) return 0;
  int lbody = nt_ref(nt, cond, "body");
  if (lbody < 0) return 0;
  int lbn = 0; const int *lbb = nt_arr(nt, lbody, "body", &lbn);
  if (lbn < 1) return 0;
  int lbp = nt_ref(nt, cond, "parameters");
  int lbi = lbp >= 0 ? nt_ref(nt, lbp, "parameters") : -1;
  int lpn = lbi >= 0 ? lbi : lbp;
  int lrn = 0; const int *lreqs = lpn >= 0 ? nt_arr(nt, lpn, "requireds", &lrn) : NULL;
  const char *lpnm = lrn > 0 ? nt_str(nt, lreqs[0], "name") : NULL;
  buf_puts(b, "({ ");
  if (lpnm) buf_printf(b, "lv_%s = _t%d; ", rename_local(lpnm), t);
  for (int k9 = 0; k9 < lbn - 1; k9++) emit_stmt(c, lbb[k9], b, 0);
  emit_cond(c, lbb[lbn - 1], b);
  buf_puts(b, "; })");
  return 1;
}

/* Emit `_crN = <branch's last value>` (boxed to the case's result type when
   that is poly), after the branch's leading statements. */
void emit_case_branch_value(Compiler *c, int stmts, TyKind rt, int cr, Buf *b) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
  for (int k = 0; k < n - 1; k++) emit_stmt(c, bb[k], b, 0);
  TyKind lt = n > 0 ? comp_ntype(c, bb[n - 1]) : TY_NIL;
  /* An empty `[]`/`{}` tail caches TY_UNKNOWN on its own -- an empty literal
     carries no element type until something supplies one -- so build it
     against the case's own result type here, the same role g_ret_type plays
     for an empty literal in a method's tail position (mirrors
     emit_ternary_arm's handling of the same literal in an if/unless value
     position). Falling through to the value-less-tail arm below would run it
     for effect and leave _crN at rt's default instead. */
  if (n > 0 && lt == TY_UNKNOWN) {
    const char *lty = nt_type(nt, bb[n - 1]);
    int len = 0;
    if (lty && sp_streq(lty, "ArrayNode")) {
      nt_arr(nt, bb[n - 1], "elements", &len);
      const char *rk = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
      if (len == 0 && rk) { buf_printf(b, "_cr%d = sp_%sArray_new(); ", cr, rk); return; }
      /* A branch merged with one of another kind types the whole case POLY,
         and the empty literal is then a BOXED empty array rather than a typed
         one: `case ... then [] else "x" end` is [] in CRuby, and leaving it to
         the value-less arm below answered nil. */
      if (len == 0 && rt == TY_POLY) {
        buf_printf(b, "_cr%d = sp_box_poly_array(sp_PolyArray_new()); ", cr); return;
      }
    }
    else if (lty && (sp_streq(lty, "HashNode") || sp_streq(lty, "KeywordHashNode"))) {
      nt_arr(nt, bb[n - 1], "elements", &len);
      const char *hc = ty_hash_cname(rt);
      if (len == 0 && hc) { buf_printf(b, "_cr%d = sp_%sHash_new(); ", cr, hc); return; }
      if (len == 0 && rt == TY_POLY) {
        buf_printf(b, "_cr%d = sp_box_obj(sp_StrPolyHash_new(), %s); ",
                   cr, hash_box_cls(TY_STR_POLY_HASH)); return;
      }
    }
  }
  /* a value-less tail (nil/void/unknown -- e.g. an arm ending in `puts` or
     a writer call, doom's debug-toggle arms in GosuWindow#button_down):
     run it as a statement and leave the arm value at the slot default,
     mirroring the if-as-value emitter. Assigning it would route the tail
     through emit_expr, which has no expression form for it. */
  if (n > 0 && (lt == TY_NIL || lt == TY_VOID || lt == TY_UNKNOWN)) {
    emit_stmt(c, bb[n - 1], b, 0);
    buf_printf(b, "_cr%d = %s; ", cr, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
    return;
  }
  /* Emit the tail value with a CAPTURED prelude: a tail whose lowering hoists
     setup statements (an array/hash literal's construction, a rooted call
     argument) must run INSIDE this branch. The ambient g_pre flushes once
     before the whole case statement, which would evaluate every arm's setup
     unconditionally -- a raising call in an untaken arm fired (mirrors the
     ternary emitter's captured-prelude form). */
  Buf pre; memset(&pre, 0, sizeof pre);
  Buf val; memset(&val, 0, sizeof val);
  Buf *sv_pre = g_pre;
  g_pre = &pre;
  if (n > 0) { if (rt == TY_POLY) emit_boxed(c, bb[n - 1], &val); else emit_expr(c, bb[n - 1], &val); }
  else buf_puts(&val, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
  g_pre = sv_pre;
  if (pre.p) buf_puts(b, pre.p);
  buf_printf(b, "_cr%d = ", cr);
  buf_puts(b, val.p ? val.p : "");
  buf_puts(b, "; ");
  free(pre.p); free(val.p);
}

/* `case` in expression position: a GCC statement-expression yielding the
   matched branch's value (or the result type's nil/default on no match). */
void emit_case_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  TyKind rt = comp_ntype(c, id);
  /* a void/nil-typed case (arms are writer calls or nil) has no C storage
     type -- emit_ctype would declare `void _crN` -- so hold it boxed.
     TY_UNKNOWN falls through emit_ctype to `void` too; widen it as well,
     matching the case/in-as-value path in emit_expr. */
  if (rt == TY_VOID || rt == TY_NIL || rt == TY_UNKNOWN) rt = TY_POLY;
  int pred = nt_ref(nt, id, "predicate");
  int nw = 0;
  const int *whens = nt_arr(nt, id, "conditions", &nw);
  int else_c = nt_ref(nt, id, "else_clause");
  int cr = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b);
  buf_printf(b, " _cr%d = %s; ", cr, rt == TY_RANGE ? "(sp_Range){0}" : default_value(rt));
  int t = -1;
  TyKind pt = TY_UNKNOWN;
  if (pred >= 0) {
    pt = comp_ntype(c, pred);
    t = ++g_tmp;
    /* a scrutinee with no usable C storage type (an empty `[]`/`{}` literal
       caches UNKNOWN, and nil/void have no slot) is boxed poly so the temp is
       not declared `void`; the when arms compare it via sp_poly_eq. */
    if (pt == TY_UNKNOWN || pt == TY_VOID || pt == TY_NIL) {
      emit_ctype(c, TY_POLY, b); buf_printf(b, " _t%d = ", t); emit_boxed(c, pred, b); buf_puts(b, "; ");
      pt = TY_POLY;
    }
    else { emit_ctype(c, pt, b); buf_printf(b, " _t%d = ", t); emit_expr(c, pred, b); buf_puts(b, "; "); }
  }

  /* Fast path: `case <int> when <integer literals>` captures the branch value
     into _cr through a C switch (jump table) instead of an O(n) `==` if-chain
     -- the slab-AST `case @nd_type[id] when <int>` interpreter-dispatch shape
     (#282). INT only: the poly path keeps sp_poly_eq (`===`), since
     sp_poly_to_i on a non-numeric subject would mis-match `case 0`. */
  if (pred >= 0 && pt == TY_INT && nw > 0) {
    int all_int = 1, ndup = 0;
    long long vals[512];
    for (int w = 0; w < nw && all_int; w++) {
      int wc = 0; const int *conds = nt_arr(nt, whens[w], "conditions", &wc);
      if (wc == 0) { all_int = 0; break; }
      for (int j = 0; j < wc; j++) {
        const char *cty = nt_type(nt, conds[j]);
        if (!cty || !sp_streq(cty, "IntegerNode")) { all_int = 0; break; }
        long long v = (long long)nt_int(nt, conds[j], "value", 0);
        for (int d = 0; d < ndup; d++) if (vals[d] == v) { all_int = 0; break; }  /* dup label -> bail */
        if (all_int && ndup < (int)(sizeof vals / sizeof vals[0])) vals[ndup++] = v;
      }
    }
    if (all_int) {
      buf_printf(b, "switch (_t%d) { ", t);
      for (int w = 0; w < nw; w++) {
        int wc = 0; const int *conds = nt_arr(nt, whens[w], "conditions", &wc);
        for (int j = 0; j < wc; j++)
          buf_printf(b, "case %lldLL: ", (long long)nt_int(nt, conds[j], "value", 0));
        buf_puts(b, "{ ");
        emit_case_branch_value(c, nt_ref(nt, whens[w], "statements"), rt, cr, b);
        buf_puts(b, "break; } ");
      }
      if (else_c >= 0) {
        buf_puts(b, "default: { ");
        emit_case_branch_value(c, nt_ref(nt, else_c, "statements"), rt, cr, b);
        buf_puts(b, "break; } ");
      }
      buf_printf(b, "} _cr%d; })", cr);
      return;
    }
  }

  for (int w = 0; w < nw; w++) {
    int wn = whens[w];
    int wc = 0;
    const int *conds = nt_arr(nt, wn, "conditions", &wc);
    buf_puts(b, w == 0 ? "if (" : "else if (");
    for (int j = 0; j < wc; j++) {
      if (j) buf_puts(b, " || ");
      if (pred >= 0) {
        /* when ClassName / Mod::Klass: Module#=== via is_a? semantics */
        const char *cty2 = nt_type(nt, conds[j]);
        const char *cn2 = cty2 && (sp_streq(cty2, "ConstantReadNode") || sp_streq(cty2, "ConstantPathNode"))
                         ? nt_str(nt, conds[j], "name") : NULL;
        /* value constant in `when`: equality, not a class test (see above) */
        if (cn2 && ({ LocalVar *_wv = comp_const(c, cn2); _wv && _wv->type != TY_UNKNOWN && _wv->type != TY_CLASS; })) cn2 = NULL;
        /* An `ffi_const` is a value too, and it lives in its own table
           rather than the constant one: read as a class name the arm folded
           to a constant false and the branch was never taken. */
        if (cn2 && comp_ffi_const_at(c, conds[j], NULL)) cn2 = NULL;
        if (cn2 && pt == TY_POLY) {
          char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
          if (!emit_poly_class_when(c, conds[j], tmp, b)) buf_puts(b, "0");
        }
        else if (cn2 && ty_is_object(pt)) {
          int cid = ty_object_class(pt); int tcid = comp_class_index(c, cn2);
          int yes = (tcid >= 0) && (cid == tcid || is_descendant(c, cid, tcid));
          buf_printf(b, "%d", yes ? 1 : 0);
        }
        else if (cn2 && pt == TY_CLASS) {
          /* a Class VALUE is an instance only of Class/Module (see the
             statement chain's arm) */
          buf_printf(b, "%d", (sp_streq(cn2, "Class") || sp_streq(cn2, "Module")) ? 1 : 0);
        }
        else if (cn2 && pt == TY_EXCEPTION) {
          /* an exception scrutinee matches by name up its class chain (#2759) */
          buf_printf(b, "sp_exc_is_a((sp_Exception *)_t%d, \"%s\")", t, exc_when_cls_name(c, cn2));
        }
        else if (cn2 && pt == TY_BOOL &&
                 (sp_streq(cn2, "TrueClass") || sp_streq(cn2, "FalseClass"))) {
          /* a boolean's class is a runtime value, not the static TY_BOOL (#2966) */
          buf_printf(b, "(_t%d %s)", t, sp_streq(cn2, "TrueClass") ? "!= 0" : "== 0");
        }
        else if (cn2) { int yes = ty_matches_class(pt, cn2, 0); buf_printf(b, "%d", yes > 0 ? 1 : 0); }
        else {
        int reidx = re_lit_index(c, conds[j]);
        if (reidx >= 0 && pt == TY_STRING) { buf_printf(b, "(sp_re_match(sp_re_pat_%d, _t%d) >= 0)", reidx, t); }
        else if (reidx >= 0 && pt == TY_POLY) { buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, _t%d)", reidx, t); }
        else if (reidx >= 0 && pt == TY_SYMBOL) { buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, sp_box_sym(_t%d))", reidx, t); }
        else if (pt == TY_STRING && emit_when_string_range(c, conds[j], t, b)) {
          /* emitted the lexicographic cover check */
        }
        else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
          if (pt == TY_POLY) buf_printf(b, "; sp_range_include(&_t%d, sp_poly_to_i(_t%d)); })", tr, t);
          else buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
        }
        else if (comp_ntype(c, conds[j]) == TY_FLOAT_RANGE) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, conds[j], b);
          if (pt == TY_POLY) buf_printf(b, "; sp_frange_cover(_t%d, sp_poly_to_f(_t%d)); })", tr, t);
          else buf_printf(b, "; sp_frange_cover(_t%d, (sp_float)_t%d); })", tr, t);
        }
        else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
          buf_puts(b, "0");
        }
        else if (nt_type(nt, conds[j]) && sp_streq(nt_type(nt, conds[j]), "SplatNode")) {
          int sp_in3 = nt_ref(nt, conds[j], "expression");
          char stmp3[32]; snprintf(stmp3, sizeof stmp3, "_t%d", t);
          buf_puts(b, "sp_case_splat_match(");
          if (pt == TY_POLY) buf_puts(b, stmp3);
          else emit_boxed_text(c, pt, stmp3, b);
          buf_puts(b, ", ");
          if (sp_in3 >= 0) emit_boxed(c, sp_in3, b); else buf_puts(b, "sp_box_nil()");
          buf_puts(b, ")");
        }
        else if (pt == TY_BIGINT && comp_ntype(c, conds[j]) == TY_BIGINT) {
          buf_printf(b, "(sp_bigint_cmp(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ") == 0)");
        }
        else if (comp_ntype(c, conds[j]) == TY_PROC &&
                 emit_when_lambda_inline(c, conds[j], t, b)) { /* literal lambda inlined */ }
        /* a Proc read out of a container arrives boxed: dispatch on the tag so
           it is CALLED and not compared (#3683) */
        else if (comp_ntype(c, conds[j]) == TY_POLY) {
          char subjp[32]; snprintf(subjp, sizeof subjp, "_t%d", t);
          int tpw = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tpw); emit_boxed(c, conds[j], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_PROC"
                        " ? sp_poly_truthy(sp_penum_call1((sp_Proc *)_t%d.v.p, ", tpw, tpw, tpw);
          if (pt == TY_POLY) buf_puts(b, subjp); else emit_boxed_text(c, pt, subjp, b);
          buf_printf(b, ")) : sp_poly_eq(_t%d, ", tpw);
          if (pt == TY_POLY) buf_puts(b, subjp); else emit_boxed_text(c, pt, subjp, b);
          buf_puts(b, "); })");
        }
        else if (comp_ntype(c, conds[j]) == TY_PROC) {
          /* `when <proc>`: Proc#=== calls the proc with the subject. The
             subject is published both in the sp_int slot (typed callee
             param) and boxed on the side-channel (poly callee param), like
             the force_poly proc-call path. */
          g_needs_proc_poly_argslot = 1;
          char subj[32]; snprintf(subj, sizeof subj, "_t%d", t);
          buf_puts(b, "({ _sp_proc_poly_args[0] = ");
          if (pt == TY_POLY) buf_puts(b, subj);
          else emit_boxed_text(c, pt, subj, b);
          buf_puts(b, "; sp_poly_truthy(((void)sp_proc_call(");
          emit_expr(c, conds[j], b);
          buf_puts(b, ", 1, (sp_int[16]){");
          if (pt == TY_POLY) buf_printf(b, "sp_poly_to_i(%s)", subj);
          else if (proc_slot_is_ptr(pt)) buf_printf(b, "(sp_int)(uintptr_t)%s", subj);
          else if (pt == TY_FLOAT) buf_puts(b, "0");
          else buf_puts(b, subj);
          buf_puts(b, "}), _sp_proc_poly_ret)); })");
        }
        else if (pt == TY_STRING) {
          /* an arm of another type can never be `===` a String (see the sibling
             arm above): evaluate it and answer false */
          TyKind wat2 = comp_ntype(c, conds[j]);
          if (wat2 != TY_STRING && wat2 != TY_STRBUF && wat2 != TY_POLY && wat2 != TY_UNKNOWN) {
            buf_printf(b, "((void)_t%d, (void)(", t); emit_expr(c, conds[j], b); buf_puts(b, "), 0)");
          }
          else {
            buf_printf(b, "sp_str_eq(_t%d, ", t);
            if (wat2 == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
            else emit_expr(c, conds[j], b);
            buf_puts(b, ")");
          }
        }
        else if (pt == TY_POLY) { buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")"); }
        else if (ty_is_object(pt) && comp_ntype(c, conds[j]) == pt &&
                 (comp_method_in_chain(c, ty_object_class(pt), "===", NULL) >= 0 ||
                  comp_method_in_chain(c, ty_object_class(pt), "==", NULL) >= 0)) {
          /* `case obj when other`: Ruby asks `other === obj`, which for a class
             defining #== is that method. Comparing the C structs instead did
             not compile for a value-type object and compared addresses for a
             heap one (#3820). */
          int ecid = ty_object_class(pt);
          int emi = comp_method_in_chain(c, ecid, "===", NULL);
          if (emi < 0) emi = comp_method_in_chain(c, ecid, "==", NULL);
          Scope *ems = &c->scopes[emi];
          LocalVar *eplv = ems->nparams > 0 ? scope_local(ems, ems->pnames[0]) : NULL;
          TyKind pty = eplv ? eplv->type : TY_POLY;
          buf_puts(b, "(");
          emit_method_cname(c, ems, b);
          buf_puts(b, "(");
          emit_expr(c, conds[j], b);
          buf_puts(b, ", ");
          { char sref[32]; snprintf(sref, sizeof sref, "_t%d", t);
            if (pty != pt && pt != TY_UNKNOWN) emit_boxed_text(c, pt, sref, b);
            else buf_puts(b, sref); }
          buf_puts(b, "))");
        }
        else {
          char sref2[32]; snprintf(sref2, sizeof sref2, "_t%d", t);
          /* a native handle or value kind answers `cond === subj` the way
             an explicit === does (emit_native_object_protocol) */
          if (!emit_native_case_eq(c, conds[j], pt, sref2, b)) {
            buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
          }
        }
        } /* close non-ConstantReadNode else */
      }
      else { buf_puts(b, "("); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
    }
    buf_puts(b, ") { ");
    emit_case_branch_value(c, nt_ref(nt, wn, "statements"), rt, cr, b);
    buf_puts(b, "}\n");   /* newline: the next arm's `else if` must not join the brace */
  }
  if (else_c >= 0) {
    buf_puts(b, "else { ");
    emit_case_branch_value(c, nt_ref(nt, else_c, "statements"), rt, cr, b);
    buf_puts(b, "}\n");
  }
  buf_printf(b, "_cr%d; })", cr);
}

/* Find a string-typed `s.length`/`s.size` (s a bare local var) anywhere in
   `root`; return the receiver node id, or -1. The receiver's length is then a
   candidate to hoist out of a loop. */
static int find_hoistable_strlen(Compiler *c, int root) {
  if (root < 0) return -1;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty && sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, root, "name");
    int recv = nt_ref(nt, root, "receiver");
    int args = nt_ref(nt, root, "arguments");
    int an = 0; if (args >= 0) nt_arr(nt, args, "arguments", &an);
    if (nm && (sp_streq(nm, "length") || sp_streq(nm, "size")) && an == 0 && recv >= 0 &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "LocalVariableReadNode") &&
        nt_str(nt, recv, "name") && comp_ntype(c, recv) == TY_STRING)
      return recv;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) { int r = find_hoistable_strlen(c, nt_ref_at(nt, root, i)); if (r >= 0) return r; }
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) { int r = find_hoistable_strlen(c, el[j]); if (r >= 0) return r; }
  }
  return -1;
}

/* Whether `root`'s subtree mutates the local `name` (reassignment or an
   in-place mutating method on it). Mirrors legacy body_mutates_var?. */
static int subtree_mutates_local(Compiler *c, int root, const char *name) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (sp_streq(ty, "CallNode")) {
      const char *mn = nt_str(nt, root, "name");
      int recv = nt_ref(nt, root, "receiver");
      if (mn && recv >= 0 && nt_type(nt, recv) &&
          sp_streq(nt_type(nt, recv), "LocalVariableReadNode") &&
          nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), name)) {
        static const char *const mut[] = {"push","pop","shift","unshift","<<","[]=","delete",
          "delete_at","clear","insert","replace","concat","sort!","sort_by!","reverse!","compact!","uniq!",
          "merge!","store","update","fill","prepend","gsub!","sub!","upcase!","downcase!",
          "strip!","chomp!","slice!","squeeze!","force_encoding","bytesplice", NULL};
        for (int i = 0; mut[i]; i++) if (sp_streq(mn, mut[i])) return 1;
      }
    }
    if ((sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
         sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) &&
        nt_str(nt, root, "name") && sp_streq(nt_str(nt, root, "name"), name))
      return 1;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (subtree_mutates_local(c, nt_ref_at(nt, root, i), name)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (subtree_mutates_local(c, el[j], name)) return 1;
  }
  return 0;
}

void emit_while(Compiler *c, int id, Buf *b, int indent, int is_until) {
  const NodeTable *nt = c->nt;
  int pred = nt_ref(nt, id, "predicate");
  int body = nt_ref(nt, id, "statements");
  /* PM_LOOP_FLAGS_BEGIN_MODIFIER (bit 2 == 4): `begin..end while cond` is a
     post-test loop -- the body runs at least once before the guard is tested. */
  int post_test = (int)(nt_int(nt, id, "flags", 0) & 4) ? 1 : 0;
  if (post_test) {
    emit_indent(b, indent);
    buf_puts(b, "do {\n");
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "} while (");
    if (is_until) buf_puts(b, "!(");
    emit_cond(c, pred, b);
    if (is_until) buf_puts(b, ")");
    buf_puts(b, ");\n");
    return;
  }
  /* Hoist a loop-invariant string length out of the loop: if the predicate
     tests `s.length`/`s.size` for a string local `s` the body never mutates,
     compute strlen once before the loop and reuse it (avoids O(n) strlen per
     iteration). Save/restore the outer hoist state for nested loops. */
  const char *sv_hvar = g_hoist_len_var, *sv_hrecv = g_hoist_len_recv;
  char hbuf[24];
  int hr = find_hoistable_strlen(c, pred);
  if (hr >= 0) {
    const char *hn = nt_str(nt, hr, "name");
    if (hn && !subtree_mutates_local(c, body, hn)) {
      int ht = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_int _t%d = sp_str_length_m(", ht); emit_expr(c, hr, b); buf_puts(b, ");\n");
      snprintf(hbuf, sizeof hbuf, "_t%d", ht);
      g_hoist_len_var = hbuf; g_hoist_len_recv = hn;
    }
  }
  /* Capture the predicate and any expression preludes it needs (method-call
     temps etc.) into local buffers. A loop condition like
     `advance while ident_continue_byte?(byte)` evaluates a method call
     (`byte`, which reads mutable state) every iteration -- but emit_cond
     routes that call's setup through g_pre, which is normally flushed ONCE
     before the statement. For a loop that hoists the call out of the loop, so
     the condition never re-evaluates (infinite loop / stuck position). Re-emit
     the preludes INSIDE the loop and break on the (negated) condition so the
     condition is recomputed each iteration. Conditions with no prelude keep
     the plain `while (cond)` form. */
  Buf cpre;  memset(&cpre, 0, sizeof cpre);
  Buf ccond; memset(&ccond, 0, sizeof ccond);
  Buf *sv_pre = g_pre; int sv_ind = g_indent;
  g_pre = &cpre; g_indent = indent + 1;
  emit_cond(c, pred, &ccond);
  g_pre = sv_pre; g_indent = sv_ind;
  if (cpre.p && cpre.p[0]) {
    emit_indent(b, indent); buf_puts(b, "while (1) {\n");
    buf_puts(b, cpre.p);
    emit_indent(b, indent + 1);
    buf_puts(b, "if (");
    if (!is_until) buf_puts(b, "!(");
    buf_puts(b, ccond.p ? ccond.p : "0");
    if (!is_until) buf_puts(b, ")");
    buf_puts(b, ") break;\n");
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
  }
  else {
    emit_indent(b, indent);
    buf_puts(b, "while (");
    if (is_until) buf_puts(b, "!(");
    buf_puts(b, ccond.p ? ccond.p : "");
    if (is_until) buf_puts(b, ")");
    buf_puts(b, ") {\n");
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }
  free(cpre.p); free(ccond.p);
  g_hoist_len_var = sv_hvar; g_hoist_len_recv = sv_hrecv;
}

void emit_for(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int idx = nt_ref(nt, id, "index");
  int coll = nt_ref(nt, id, "collection");
  int body = nt_ref(nt, id, "statements");
  const char *vn = idx >= 0 ? nt_str(nt, idx, "name") : NULL;
  /* Unwrap parenthesized collections so node-kind checks, notably RangeNode,
     see the underlying expression. */
  coll = unwrap_parens(c, coll);
  TyKind ct = comp_ntype(c, coll);

  if (ct == TY_RANGE && nt_type(nt, coll) && sp_streq(nt_type(nt, coll), "RangeNode")) {
    /* for v in lo..hi -- a plain counted loop. Under --int-overflow=promote the
       counter and/or endpoints may be widened to poly, so coerce each endpoint
       with sp_poly_to_i when its static type is poly/bigint, and when the
       counter slot is poly drive the loop with a fresh sp_int temp and re-box
       the counter local each iteration so the body sees a poly value. */
    int excl = (int)(nt_int(nt, coll, "flags", 0) & 4) ? 1 : 0;
    int lref = nt_ref(nt, coll, "left");
    int rref = nt_ref(nt, coll, "right");
    TyKind lty = comp_ntype(c, lref);
    TyKind rty = comp_ntype(c, rref);
    int lpoly = (lty == TY_POLY || lty == TY_BIGINT);
    int rpoly = (rty == TY_POLY || rty == TY_BIGINT);
    LocalVar *clv = scope_local(comp_scope_of(c, idx), vn);
    int cpoly = clv && clv->type == TY_POLY;
    int thi = ++g_tmp;
    emit_indent(b, indent); buf_puts(b, "{ sp_int ");
    buf_printf(b, "_t%d = ", thi);
    if (rpoly) buf_puts(b, "sp_poly_to_i(");
    emit_expr(c, rref, b);
    if (rpoly) buf_puts(b, ")");
    buf_puts(b, ";\n");
    if (cpoly) {
      int tc = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "for (sp_int _t%d = ", tc);
      if (lpoly) buf_puts(b, "sp_poly_to_i(");
      emit_expr(c, lref, b);
      if (lpoly) buf_puts(b, ")");
      buf_printf(b, "; _t%d %s _t%d; _t%d++) {\n", tc, excl ? "<" : "<=", thi, tc);
      emit_indent(b, indent + 2);
      buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", vn, tc);
      emit_loop_body(c, body, b, indent + 2);
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
    emit_indent(b, indent + 1);
    buf_printf(b, "for (lv_%s = ", vn);
    if (lpoly) buf_puts(b, "sp_poly_to_i(");
    emit_expr(c, lref, b);
    if (lpoly) buf_puts(b, ")");
    buf_printf(b, "; lv_%s %s _t%d; lv_%s++) {\n", vn, excl ? "<" : "<=", thi, vn);
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  if (ty_is_array(ct) || ct == TY_POLY_ARRAY) {
    const char *k = array_kind(ct);
    int ta = ++g_tmp, ti = ++g_tmp;
    /* Multi-variable for: `for a, b in coll` -- each element is an inner array. */
    const char *idx_ty = nt_type(nt, idx);
    if (idx_ty && sp_streq(idx_ty, "MultiTargetNode")) {
      int ln = 0;
      const int *lefts = nt_arr(nt, idx, "lefts", &ln);
      int tv = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ sp_%sArray *_t%d = ", k ? k : "Poly", ta); emit_expr(c, coll, b); buf_puts(b, ";\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                 ti, ti, k ? k : "Poly", ta, ti);
      emit_indent(b, indent + 2);
      /* get the outer element as a poly value for inner destructuring */
      if (k) /* typed array: box the element to poly */
        buf_printf(b, "sp_RbVal _t%d = sp_box_%s(sp_%sArray_get(_t%d, _t%d));\n",
                   tv, sp_streq(k,"Int")?"int":sp_streq(k,"Float")?"float":"str", k, ta, ti);
      else
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", tv, ta, ti);
      for (int i = 0; i < ln; i++) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        if (!lnm) continue;
        TyKind vt = scope_local(comp_scope_of(c, idx), lnm) ?
                    scope_local(comp_scope_of(c, idx), lnm)->type : TY_POLY;
        emit_indent(b, indent + 2);
        if (vt == TY_INT || vt == TY_UNKNOWN)
          buf_printf(b, "lv_%s = sp_unbox_int(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
        else if (vt == TY_FLOAT)
          buf_printf(b, "lv_%s = sp_unbox_float(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
        else if (vt == TY_STRING)
          buf_printf(b, "lv_%s = sp_unbox_str(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
        else
          buf_printf(b, "lv_%s = sp_poly_massign_get(_t%d, %d);\n", lnm, tv, i);
      }
      emit_loop_body(c, body, b, indent + 2);
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
    emit_indent(b, indent);
    buf_printf(b, "{ sp_%sArray *_t%d = ", k ? k : "Poly", ta); emit_expr(c, coll, b); buf_puts(b, ";\n");
    emit_indent(b, indent + 1);
    buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k ? k : "Poly", ta, ti);
    emit_indent(b, indent + 2);
    buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", vn, k ? k : "Poly", ta, ti);
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  /* for over a hash: materialize [key, value] pairs in insertion order (the
     shared Hash#to_a walk) and iterate; `for k, v in h` destructures each
     pair, `for pair in h` binds the boxed pair itself. */
  if (ty_is_hash(ct)) {
    const char *hn2 = ty_hash_cname(ct);
    if (hn2) {
      int ta = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ sp_PolyArray *_t%d = ", ta);
      emit_hash_pairs_expr(c, coll, ct, hn2, b);
      buf_puts(b, ";\n");
      emit_indent(b, indent + 1); buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
      emit_indent(b, indent + 1);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ta, ti);
      const char *idx_ty2 = nt_type(nt, idx);
      if (idx_ty2 && sp_streq(idx_ty2, "MultiTargetNode")) {
        int ln = 0;
        const int *lefts = nt_arr(nt, idx, "lefts", &ln);
        emit_indent(b, indent + 2);
        buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", tv, ta, ti);
        for (int i = 0; i < ln; i++) {
          const char *lnm = nt_str(nt, lefts[i], "name");
          if (!lnm) continue;
          TyKind vt2 = scope_local(comp_scope_of(c, idx), lnm) ?
                       scope_local(comp_scope_of(c, idx), lnm)->type : TY_POLY;
          emit_indent(b, indent + 2);
          if (vt2 == TY_INT || vt2 == TY_UNKNOWN)
            buf_printf(b, "lv_%s = sp_unbox_int(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
          else if (vt2 == TY_FLOAT)
            buf_printf(b, "lv_%s = sp_unbox_float(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
          else if (vt2 == TY_STRING)
            buf_printf(b, "lv_%s = sp_unbox_str(sp_poly_massign_get(_t%d, %d));\n", lnm, tv, i);
          else
            buf_printf(b, "lv_%s = sp_poly_massign_get(_t%d, %d);\n", lnm, tv, i);
        }
      }
      else {
        emit_indent(b, indent + 2);
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", vn, ta, ti);
      }
      emit_loop_body(c, body, b, indent + 2);
      emit_indent(b, indent + 1); buf_puts(b, "}\n");
      emit_indent(b, indent); buf_puts(b, "}\n");
      return;
    }
  }
  emit_indent(b, indent); buf_puts(b, "/* unsupported for-loop collection */\n");
}

/* Emit `node` (statically poly) coerced to the non-poly return/slot type `t`.
   int/bool/float go through the converting sp_poly_to_i/f (matching the legacy
   scalar-return coercion; sp_bool is int-backed). Strings, objects, and every
   other pointer-backed reference (arrays, hashes, procs, fibers, ...) unbox via
   emit_unbox_text: a string reads `.v.s` (a nil box has a zeroed union, so this
   is NULL and `String?` round-trips); a pointer reads `(T *)(...).v.p`. The few
   by-value types (Range/Time/Complex/...) have no `.v.p` form, so they fall back
   to a plain emit (no coercion was applied for them before either, and no such
   poly-bodied return arises). Used where a method's RBS return type is narrower
   than its poly body value (#1417). */
static void emit_unbox_node(Compiler *c, TyKind t, int node, Buf *b) {
  /* When the slot being narrowed into is a SEEDED return, the narrowing is the
     moment the seed's truth becomes checkable, so it carries the assertion --
     a no-op macro without -DSP_RBS_CHECK (#3412). g_ret_seeded is set for the
     scope currently being emitted. */
  Buf src; memset(&src, 0, sizeof src);
  emit_expr(c, node, &src);
  Buf val; memset(&val, 0, sizeof val);
  if (g_ret_seeded) emit_rbs_checked_text(c, t, "the return value", src.p ? src.p : "sp_box_nil()", &val);
  else buf_puts(&val, src.p ? src.p : "");
  const char *v = val.p ? val.p : "";
  /* int and float keep nil distinguishable: the plain conversions answer the
     type's zero for a boxed nil, which in these two slots is a real value and
     is what made a nullable return read back as 0 / 0.0. bool has no sentinel
     to land on, so it keeps the plain conversion (#3458). */
  if (t == TY_INT)                 buf_printf(b, "sp_poly_to_i_or_nil(%s)", v);
  else if (t == TY_FLOAT)          buf_printf(b, "sp_poly_to_f_or_nil(%s)", v);
  else if (t == TY_BOOL)           buf_printf(b, "sp_poly_to_i(%s)", v);
  else {
    const char *cn = c_type_name(t);
    if (t == TY_STRING || ty_is_object(t) || (cn && cn[0] && cn[strlen(cn) - 1] == '*'))
      emit_unbox_text(c, t, v, b);
    else buf_puts(b, v);
  }
  free(val.p); free(src.p);
}

/* A genuine poly body (TY_POLY) is unboxed into any narrower (non-poly) return
   slot (#1417). */
/* A concrete typed array feeding a TY_POLY_ARRAY return slot (an
   Array[untyped] rbs seed over a concrete body): convert per element, boxing
   each -- returning the raw pointer reinterprets the layout and silently
   iterates zero elements (#3279). Returns 1 when it emitted the conversion. */
/* A narrower hash storage kind feeding a wider declared one -- an
   Hash[String, untyped] seed over a body that builds {"k" => "v"}. Ruby has one
   Hash and this runtime has seven layouts, so the pointer cannot just be
   returned: it would be reinterpreted and read back as garbage, silently on a
   compiler that only warns (#3420). Convert, as the array side already does. */
static int emit_ret_hash_widen_conv(Compiler *c, TyKind slot, TyKind vty, int node, Buf *b) {
  if (!ty_is_hash(slot) || !ty_is_hash(vty) || slot == vty) return 0;
  const char *fn = slot == TY_STR_POLY_HASH  ? "sp_StrPolyHash_from_poly"
                 : slot == TY_SYM_POLY_HASH  ? "sp_SymPolyHash_from_poly"
                 : slot == TY_POLY_POLY_HASH ? "sp_PolyPolyHash_from_poly" : NULL;
  if (!fn) return 0;   /* narrowing, or a kind with no generic builder: leave it */
  /* Only widen where the key kind agrees; a String-keyed body through a
     Symbol-keyed signature is a contradiction, not a conversion. */
  if (slot == TY_STR_POLY_HASH && ty_hash_key(vty) != TY_STRING) return 0;
  if (slot == TY_SYM_POLY_HASH && ty_hash_key(vty) != TY_SYMBOL) return 0;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, node, &vb);
  buf_printf(b, "%s(", fn);
  emit_boxed_text(c, vty, vb.p ? vb.p : "NULL", b);
  buf_puts(b, ")");
  free(vb.p);
  return 1;
}

static int emit_ret_poly_array_conv(Compiler *c, TyKind slot, TyKind vty, int node, Buf *b) {
  if (slot != TY_POLY_ARRAY) return 0;
  const char *k = vty == TY_INT_ARRAY ? "int" : vty == TY_FLOAT_ARRAY ? "float"
                : vty == TY_STR_ARRAY ? "str" : NULL;
  if (!k) return 0;
  buf_printf(b, "sp_PolyArray_from_%s_array(", k);
  emit_expr(c, node, b);
  buf_puts(b, ")");
  return 1;
}

static int tail_needs_unbox(TyKind r0, TyKind ret) {
  return r0 == TY_POLY && ret != TY_POLY;
}

/* The return slot's nil representation for a non-poly type. */
static void emit_ret_nil(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_INT || t == TY_BOOL) buf_puts(b, "SP_INT_NIL");
  else if (t == TY_FLOAT) buf_puts(b, "sp_float_nil()");
  else if (t == TY_STRING || ty_is_object(t)) buf_puts(b, "NULL");
  else {
    const char *cn = c_type_name(t);
    if (cn && cn[0] && cn[strlen(cn) - 1] == '*') buf_puts(b, "NULL");
    else buf_puts(b, default_value(t));
  }
}

/* Emit a tail/return value expression into a non-poly return slot. A call that
   resolves to nil through a nil/unresolved receiver is typed `-> Integer` (etc.)
   per RBS but emits the poly box `sp_box_nil()`; returning that raw from a
   non-poly C function is uncompilable (#1432). Detect that exact emission and
   substitute the slot's typed nil instead. The text test is precise: a literal
   `sp_box_nil()` carries no side-effect prelude, so discarding it is safe, and
   any other emission (e.g. a poly-dispatch `({...})`) is passed through
   unchanged. */
static void emit_tail_value(Compiler *c, int node, Buf *b) {
  /* A poly tail slot (a poly return, or a poly result var -- e.g. an inlined
     method's result temp) takes the value as-is: do not rewrite a poly
     `sp_box_nil()` into the scalar emit_ret_nil(g_ret_type) form below. */
  if (g_ret_type == TY_POLY || (g_result_var && g_result_poly)) { emit_expr(c, node, b); return; }
  /* An inlined body whose tail answers a different POINTER kind than the slot
     this emission types: the blockless `each` splice types its result from the
     CALL (an Enumerator) while the body's tail is `self`. That value is the
     generator's own return -- CRuby reads it only as StopIteration#result --
     and storing a receiver pointer in an Enumerator slot is a lie the C
     compiler rightly warns about. Evaluate the tail and leave the slot nil
     (#3953). */
  if (g_result_var && !g_result_poly && g_ret_type != TY_POLY && needs_root(g_ret_type)) {
    TyKind vt0 = comp_ntype(c, node);
    if (ty_is_object(vt0) && !ty_is_object(g_ret_type) && !ty_is_array(g_ret_type) &&
        !ty_is_hash(g_ret_type) && g_ret_type != TY_STRING && g_ret_type != TY_STRBUF) {
      buf_puts(b, "((void)("); emit_expr(c, node, b); buf_puts(b, "), NULL)");
      return;
    }
  }
  /* An int value returned through a TY_BIGINT slot must be wrapped: the raw
     integer is otherwise reinterpreted as an sp_Bigint* -- a literal `return 0`
     became a NULL pointer that segfaulted the caller's first sp_bigint_cmp
     (tep's proxy retry loop, whose backoff accumulator promoted the method's
     return to bigint while an early guard returned plain 0). Mirrors the
     assignment-side int->bigint coercion. */
  if (g_ret_type == TY_BIGINT) {
    TyKind bvt = comp_ntype(c, node);
    if (bvt == TY_INT || bvt == TY_BOOL) {
      buf_puts(b, "sp_bigint_new_int(");
      emit_expr(c, node, b);
      buf_puts(b, ")");
      return;
    }
    if (bvt == TY_POLY) {
      buf_puts(b, "sp_poly_as_bigint(");
      emit_expr(c, node, b);
      buf_puts(b, ")");
      return;
    }
  }
  /* A poly-array value returned through a slot pinned (e.g. by RBS) to a concrete
     typed array cannot flow through as-is: sp_PolyArray holds boxed elements while
     sp_StrArray/sp_IntArray/... hold raw unboxed storage, so passing the pointer
     straight through reads boxed words as the wrong C type and crashes (SIGSEGV).
     A typed-array return annotation is a developer promise (CRuby-verifiable), so
     honor it: materialize the declared array by unboxing each element at the
     return boundary (#1827). Array literals are context-coerced to the slot's
     element type above/in emit_expr, so exclude them; a local/call/ivar read has
     no such coercion and is what needs the boundary materialize. Object-typed
     arrays have no generic per-element unbox (the box must already hold that exact
     object type), so those stay a loud compile-time diagnostic. */
  /* The same build the assignment side does, for a method whose value narrowed
     to a pointer array: a literal or a generator constructs its container from
     the NODE's type, which a return-type narrowing never touched, so without
     this the body builds an sp_PolyArray * and hands it back through an
     sp_PtrArray * signature. Any other tail already carries the narrowed type
     from the slot it reads. */
  if (ty_is_ptr_array(g_ret_type) && comp_ntype(c, node) != g_ret_type &&
      emit_ptr_array_build(c, node, g_ret_type, b)) return;
  {
    const char *vnty = nt_type(c->nt, node);
    if (comp_ntype(c, node) == TY_POLY_ARRAY && vnty && !sp_streq(vnty, "ArrayNode")) {
      const char *conv =
        g_ret_type == TY_STR_ARRAY   ? "sp_StrArray_from_poly_array" :
        g_ret_type == TY_INT_ARRAY   ? "sp_IntArray_from_poly_array" :
        g_ret_type == TY_FLOAT_ARRAY ? "sp_FloatArray_from_poly_array" : NULL;
      if (conv) {
        buf_printf(b, "%s(", conv); emit_expr(c, node, b); buf_puts(b, ")");
        return;
      }
      if (ty_is_obj_array(g_ret_type))
        unsupported(c, node, "declared return type is a typed object array but the body infers a poly array");
    }
  }
  /* An empty `{}` literal defaults to StrPolyHash, but in a hash-returning tail
     it must take the return type (e.g. a SymPolyHash-returning method whose
     other branch is `{ a: 1 }`); otherwise the StrPolyHash* return is an
     incompatible pointer type. Same idea as the empty-`[]` array handling. */
  const char *nty = nt_type(c->nt, node);
  if (nty && (sp_streq(nty, "HashNode") || sp_streq(nty, "KeywordHashNode")) && ty_is_hash(g_ret_type)) {
    int hc = 0; nt_arr(c->nt, node, "elements", &hc);
    const char *hcn = ty_hash_cname(g_ret_type);
    if (hc == 0 && hcn) { buf_printf(b, "sp_%sHash_new()", hcn); return; }
  }
  /* `Hash.new` / `Hash.new(default)` in a hash-returning tail takes the return
     type the same way (the element types are witnessed by callers, not this
     empty body -- #1680), constructing the concrete variant rather than falling
     through to the generic call path that can't build an untyped Hash. Mirrors
     the `h = Hash.new(default)` local-write case in emit_assign. */
  if (nty && sp_streq(nty, "CallNode") && ty_is_hash(g_ret_type) && ty_hash_cname(g_ret_type) &&
      sp_streq(nt_str(c->nt, node, "name") ? nt_str(c->nt, node, "name") : "", "new") &&
      nt_ref(c->nt, node, "block") < 0) {
    int hr = nt_ref(c->nt, node, "receiver");
    const char *hrt = hr >= 0 ? nt_type(c->nt, hr) : NULL;
    if (hrt && (sp_streq(hrt, "ConstantReadNode") || sp_streq(hrt, "ConstantPathNode")) &&
        sp_streq(nt_str(c->nt, hr, "name") ? nt_str(c->nt, hr, "name") : "", "Hash")) {
      const char *hcn = ty_hash_cname(g_ret_type);
      int ha = nt_ref(c->nt, node, "arguments"); int hac = 0;
      const int *hav = ha >= 0 ? nt_arr(c->nt, ha, "arguments", &hac) : NULL;
      int poly_val = (g_ret_type == TY_SYM_POLY_HASH || g_ret_type == TY_STR_POLY_HASH);
      if (hac == 0) {
        buf_printf(b, "sp_%sHash_new()", hcn);
        return;
      }
      /* every hash variant now has sp_<H>Hash_new_with_default (PolyPolyHash
         gained it in #1674), and the poly-valued variants box the default */
      buf_printf(b, "sp_%sHash_new_with_default(", hcn);
      if (poly_val || g_ret_type == TY_POLY_POLY_HASH) emit_boxed(c, hav[0], b);
      else emit_expr(c, hav[0], b);
      buf_puts(b, ")");
      return;
    }
  }
  /* A hash variant the body answers where the signature declares another one:
     the variants are separate C structs, so the pointer went back uncoerced
     and the build stopped. An --rbs return type is the usual way the two come
     apart -- the signature is a true description of the method while the ivar
     writes widened its variant -- and the conversion is the one the assignment
     side already makes (#4089), through the boxed form the converting entries
     take. */
  if (ty_is_hash(g_ret_type) && ty_is_hash(comp_ntype(c, node)) &&
      g_ret_type != comp_ntype(c, node) &&
      (g_ret_type == TY_POLY_POLY_HASH || g_ret_type == TY_SYM_POLY_HASH ||
       g_ret_type == TY_STR_POLY_HASH)) {
    const char *hconv = g_ret_type == TY_POLY_POLY_HASH ? "sp_poly_as_poly_poly_hash"
                      : g_ret_type == TY_SYM_POLY_HASH  ? "sp_poly_as_sym_poly_hash"
                      : "sp_poly_as_str_poly_hash";
    buf_printf(b, "%s(", hconv); emit_boxed(c, node, b); buf_puts(b, ")");
    return;
  }
  /* A bare `nil` returned through an int or float slot. emit_expr renders
     NilNode as the numeric default 0, which in those two slots is a real
     value -- the caller reads 0 / 0.0 where the method said nil. Both have a
     sentinel (SP_INT_NIL, the float NaN), and every consumer already tests
     for it: the same method returning nil through a String or bool slot is
     correct today because NULL and the poly box carry nil natively. So spell
     the sentinel here rather than let the numeric default stand (#3458). */
  if ((g_ret_type == TY_INT || g_ret_type == TY_FLOAT) &&
      nt_kind(c->nt, node) == NK_NilNode) { emit_ret_nil(c, g_ret_type, b); return; }
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  const char *txt = tmp.p ? tmp.p : "";
  if (sp_streq(txt, "sp_box_nil()")) emit_ret_nil(c, g_ret_type, b);
  /* The unresolved-call gate's sp_raise_nomethod(...) is a side-effecting poly
     value (it raises): coerce it to the non-poly slot, keeping the call, rather
     than passing the sp_RbVal through raw. A text match on the gate's own token
     is reliable where comp_ntype is not (it can diverge from the emitted C). */
  else if (strncmp(txt, "sp_raise_nomethod(", 18) == 0 &&
           g_ret_type != TY_POLY && g_ret_type != TY_UNKNOWN)
    emit_unbox_text(c, g_ret_type, txt, b);
  /* A NameError-raising constant read is a comma expression whose dummy value
     (e.g. ((sp_Class){-1})) need not match the slot: the raise longjmps first.
     Evaluate it for the raise and yield the slot's default instead of letting
     the mismatched C type flow into the return. */
  else if ((strncmp(txt, "(sp_raise_cls(", 14) == 0 ||
            strncmp(txt, "(sp_exc_stage_key(", 18) == 0) &&
           g_ret_type != TY_POLY && g_ret_type != TY_UNKNOWN)
    buf_printf(b, "({ (void)%s; %s; })", txt, default_value(g_ret_type));
  else buf_puts(b, txt);
  free(tmp.p);
}

/* A literal whose evaluation has no side effects -- used to decide whether a
   discarded `return <expr>` in a void function needs `(void)(<expr>)` to keep
   the side effects or can collapse to a bare `return;`. Callers unwrap parens
   first. */
static int node_is_pure_literal(const NodeTable *nt, int node) {
  const char *ty = nt_type(nt, node);
  return ty && (sp_streq(ty, "NilNode") || sp_streq(ty, "IntegerNode") ||
                sp_streq(ty, "FloatNode") || sp_streq(ty, "StringNode") ||
                sp_streq(ty, "SymbolNode") || sp_streq(ty, "TrueNode") ||
                sp_streq(ty, "FalseNode") || sp_streq(ty, "RationalNode") ||
                sp_streq(ty, "ImaginaryNode"));
}

/* The static type of a single return argument for coercion purposes. A bare
   `*x` return argument (`return *x`) emits an sp_PolyArray* via emit_expr, so it
   is TY_POLY_ARRAY here -- comp_ntype reports a SplatNode as its element type
   (the array-literal element-unification convention), which would drive a
   spurious unbox against a TY_POLY_ARRAY return slot. */
static TyKind ret_arg_ntype(Compiler *c, int node) {
  const char *ty = nt_type(c->nt, node);
  if (ty && sp_streq(ty, "SplatNode")) return TY_POLY_ARRAY;
  return comp_ntype(c, node);
}

void emit_return(Compiler *c, int id, Buf *b, int indent) {
  int args = nt_ref(c->nt, id, "arguments");
  int n = 0;
  const int *a = args >= 0 ? nt_arr(c->nt, args, "arguments", &n) : NULL;

  /* A non-lambda proc written at top level: `return` is a TOP-LEVEL return,
     which ends the script (#3663). */
  if (g_proc_toplevel_return) {
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    for (int k = 0; k < n; k++) { buf_puts(b, "(void)("); emit_boxed(c, a[k], b); buf_puts(b, "); "); }
    buf_puts(b, "exit(0); }\n");
    return;
  }

  /* Inside a non-lambda proc body: `return` is non-local -- longjmp to the
     creating method's frame with the boxed value (CRuby proc-return semantics). */
  if (g_proc_return_home) {
    emit_indent(b, indent);
    if (n > 1) {
      int ta = ++g_tmp;
      buf_printf(b, "{ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
      for (int k = 0; k < n; k++) { buf_printf(b, " sp_PolyArray_push(_t%d, ", ta); emit_boxed(c, a[k], b); buf_puts(b, ");"); }
      buf_printf(b, " sp_proc_return(%s, sp_box_poly_array(_t%d)); }\n", g_proc_return_home, ta);
    }
    else {
      buf_printf(b, "{ sp_proc_return(%s, ", g_proc_return_home);
      if (n == 0) buf_puts(b, "sp_box_nil()");
      else emit_boxed(c, a[0], b);
      buf_puts(b, "); }\n");
    }
    return;
  }

  /* Inside a method that owns a proc-return frame: funnel every `return`
     through the single exit that pops the frame, storing the value first. */
  if (g_method_pr_label && g_ensure_depth == 0) {
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    if (g_method_pr_var) {
      if (n > 1) {
        int ta = ++g_tmp;
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        for (int k = 0; k < n; k++) { buf_printf(b, " sp_PolyArray_push(_t%d, ", ta); emit_boxed(c, a[k], b); buf_puts(b, ");"); }
        buf_printf(b, " %s = _t%d; ", g_method_pr_var, ta);
      }
      else if (n == 1) {
        buf_printf(b, "%s = ", g_method_pr_var);
        TyKind r0 = ret_arg_ntype(c, a[0]);
        if (g_ret_type == TY_POLY && r0 != TY_POLY) emit_boxed(c, a[0], b);
        else if (emit_ret_hash_widen_conv(c, g_ret_type, r0, a[0], b)) { }
        else if (emit_ret_poly_array_conv(c, g_ret_type, r0, a[0], b)) { }
        else if (tail_needs_unbox(r0, g_ret_type)) emit_unbox_node(c, g_ret_type, a[0], b);
        else emit_tail_value(c, a[0], b);
        buf_puts(b, "; ");
      }
      else {
        const char *nilv = g_ret_type == TY_POLY ? "sp_box_nil()"
                         : g_ret_type == TY_INT ? "SP_INT_NIL"
                         : g_ret_type == TY_FLOAT ? "sp_float_nil()"
                         : g_ret_type == TY_STRING ? "NULL" : default_value(g_ret_type);
        buf_printf(b, "%s = %s; ", g_method_pr_var, nilv);
      }
    }
    else {
      /* No result slot (a void-position inline funnel, e.g. a yielding method
         inlined in statement position): the value is discarded, but a
         `return <expr>` must still evaluate its argument for side effects
         before jumping to the exit -- matching the void non-inline path below. */
      for (int k = 0; k < n; k++) {
        int vn = unwrap_parens(c, a[k]);
        if (!node_is_pure_literal(c->nt, vn)) { buf_puts(b, "(void)("); emit_expr(c, vn, b); buf_puts(b, "); "); }
      }
    }
    emit_frame_unwind(b, g_method_pr_exc_depth, NULL);
    buf_printf(b, "goto %s; }\n", g_method_pr_label);
    return;
  }

  if (g_ensure_depth > 0) {
    /* Inside a begin..ensure body: defer the return until ensure runs. */
    EnsureCtx *ctx = &g_ensure_stack[g_ensure_depth - 1];
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    if (ctx->has_retval) {
      if (n > 1) {
        int ta = ++g_tmp;
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", ta, ta);
        for (int k = 0; k < n; k++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", ta);
          emit_boxed(c, a[k], b);
          buf_puts(b, "); ");
        }
        buf_printf(b, "_retv%d = _t%d; ", ctx->lid, ta);
      }
      else if (n > 0) {
        buf_printf(b, "_retv%d = ", ctx->lid);
        if (g_ret_type == TY_POLY && comp_ntype(c, a[0]) != TY_POLY) emit_boxed(c, a[0], b);
        else emit_expr(c, a[0], b);
        buf_puts(b, "; ");
      }
    }
    {
      int pops = g_exc_frame_depth - ctx->exc_base;
      if (pops < 1) pops = 1;   /* at least the ensure frame itself */
      /* pop the handlers for rescue bodies inside this ensure region we are
         leaving; rescues outside it are popped at the ensure re-dispatch. */
      emit_cur_exc_restore(b, ctx->exc_base);
      buf_printf(b, "_retf%d = 1; sp_exc_top -= %d; goto _ensure%d; }\n",
                 ctx->lid, pops, ctx->lid);
    }
    return;
  }

  /* Inside a first-class proc body whose return rides the boxed slot (the
     universal proc return ABI): an explicit `return <v>` writes the boxed value
     to _sp_proc_poly_ret and returns 0 (the raw sp_int carrier is unused),
     mirroring the implicit tail. Compute the value while any live begin/rescue
     frames are still open (a raising value must unwind into them), then pop the
     frames and return 0. Non-local proc `return` (g_proc_return_home) and the
     in-ensure deferral are handled above and return early before here. */
  if (g_in_proc_body && g_result_var && g_result_poly) {
    emit_indent(b, indent);
    buf_printf(b, "{ %s = ", g_result_var);
    if (n == 0) buf_puts(b, "sp_box_nil()");
    else if (n == 1) emit_boxed(c, a[0], b);
    else {
      int ta = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
      for (int k = 0; k < n; k++) { buf_printf(b, " sp_PolyArray_push(_t%d, ", ta); emit_boxed(c, a[k], b); buf_puts(b, ");"); }
      buf_printf(b, " sp_box_poly_array(_t%d); })", ta);
    }
    buf_puts(b, "; ");
    emit_frame_unwind(b, 0, NULL);
    buf_puts(b, "return 0; }\n");
    return;
  }

  emit_indent(b, indent);
  /* leaving through live begin/rescue frames: pop them, or their jmp_bufs
     dangle into this soon-dead C frame and the next raise longjmps into
     garbage (doom's SoundManager#[] early cache returns). Also restores
     the sp_rescue_sp handler for every rescue body this return leaves. */
  /* When live begin/rescue frames must be popped, the return VALUE has to be
     evaluated while they are still live: a `return <expr>` where <expr> raises
     (`return boom` inside begin/rescue) must unwind INTO the enclosing handler,
     not past a frame already popped (issue #1775). So for a value return that
     crosses frames, compute the value into a temp first, then pop, then return
     it. With no frames to pop the order is immaterial -- keep the flat form. */
  int ret_has_frames = (g_exc_frame_depth > 0) || (rescues_crossed(0) > 0);
  if (ret_has_frames && n >= 1 && !(n == 1 && g_ret_type == TY_VOID)) {
    if (n > 1) {
      int ta = ++g_tmp;
      buf_printf(b, "{ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
      for (int k = 0; k < n; k++) { buf_printf(b, " sp_PolyArray_push(_t%d, ", ta); emit_boxed(c, a[k], b); buf_puts(b, ");"); }
      buf_puts(b, " ");
      emit_frame_unwind(b, 0, NULL);
      buf_printf(b, " return _t%d; }\n", ta);
      return;
    }
    int tr = ++g_tmp;
    TyKind r0 = ret_arg_ntype(c, a[0]);
    buf_puts(b, "{ "); emit_ctype(c, g_ret_type == TY_UNKNOWN ? TY_INT : g_ret_type, b);
    buf_printf(b, " _t%d = ", tr);
    if (g_ret_type == TY_POLY && r0 != TY_POLY) emit_boxed(c, a[0], b);
        else if (emit_ret_hash_widen_conv(c, g_ret_type, r0, a[0], b)) { }
        else if (emit_ret_poly_array_conv(c, g_ret_type, r0, a[0], b)) { }
    else if (tail_needs_unbox(r0, g_ret_type)) emit_unbox_node(c, g_ret_type, a[0], b);
    else emit_tail_value(c, a[0], b);
    buf_puts(b, "; ");
    emit_frame_unwind(b, 0, NULL);
    buf_printf(b, "return _t%d; }\n", tr);
    return;
  }
  emit_frame_unwind(b, 0, NULL);
  if (n > 1) {
    int ta = ++g_tmp;
    buf_printf(b, "{ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
    for (int k = 0; k < n; k++) {
      buf_printf(b, " sp_PolyArray_push(_t%d, ", ta);
      emit_boxed(c, a[k], b);
      buf_puts(b, ");");
    }
    buf_printf(b, " return _t%d; }\n", ta);
  }
  else if (n > 0 && g_ret_type == TY_VOID) {
    /* void function: a `return <expr>` (typically `return nil`) discards its
       value. Evaluate a non-literal expr for side effects, then a bare return,
       so the generated C doesn't `return <value>` from a void function. */
    int vn = unwrap_parens(c, a[0]);
    if (!node_is_pure_literal(c->nt, vn)) { buf_puts(b, "(void)("); emit_expr(c, vn, b); buf_puts(b, "); "); }
    buf_puts(b, "return;\n");
  }
  else if (n > 0) {
    TyKind r0n = ret_arg_ntype(c, a[0]);
    /* A guarded `return v if v.is_a?(K)` whose value is statically NOT K:
       the guard folded to constant false, so this return is dead -- but the
       raw value would still be ill-typed C in the K-returning function.
       Evaluate for effect and yield the slot's empty value (#3259). */
    if (ty_is_object(g_ret_type) && !ty_is_object(r0n) &&
        r0n != TY_POLY && r0n != TY_UNKNOWN && r0n != TY_VOID) {
      buf_puts(b, "{ (void)("); emit_expr(c, a[0], b); buf_puts(b, "); return ");
      if (comp_ty_value_obj(c, g_ret_type))
        buf_printf(b, "(sp_%s){0}", c->classes[ty_object_class(g_ret_type)].c_name);
      else buf_puts(b, "NULL");
      buf_puts(b, "; }\n");
      return;
    }
    buf_puts(b, "return ");
    TyKind r0 = ret_arg_ntype(c, a[0]);
    if (g_ret_type == TY_POLY && r0 != TY_POLY) emit_boxed(c, a[0], b);
        else if (emit_ret_hash_widen_conv(c, g_ret_type, r0, a[0], b)) { }
        else if (emit_ret_poly_array_conv(c, g_ret_type, r0, a[0], b)) { }
    /* a poly return value feeding a narrower (non-poly) return slot -- e.g. a
       method(:sym) target pinned to sp_int that returns a poly @ivar, or an
       RBS-typed String/object method whose body yields poly -- needs coercing. */
    else if (tail_needs_unbox(r0, g_ret_type)) emit_unbox_node(c, g_ret_type, a[0], b);
    else {
      /* an explicit `return <subclass>` through an ancestor-typed slot (#3418) */
      emit_obj_upcast_prefix(c, g_ret_type, r0, b);
      emit_tail_value(c, a[0], b);   /* coerces an unresolved TY_UNKNOWN token */
    }
    buf_puts(b, ";\n");
  }
  else if (g_ret_type == TY_POLY) buf_puts(b, "return sp_box_nil();\n");
  else if (g_ret_type == TY_VOID) buf_puts(b, "return;\n");
  /* TY_UNKNOWN is emitted as an int C return type (main(), or a never-inferred
     method), so its bare return is `return 0;`, not the void `return;`. */
  else if (g_ret_type == TY_UNKNOWN) buf_puts(b, "return 0;\n");
  /* bare `return` is Ruby nil: emit the return type's nil representation, not a
     bare `return;` (illegal in a non-void C function -- e.g. `Db.close` returns
     a nullable DbPool*, where nil is NULL). default_value already yields the
     by-value nil form for struct types (e.g. (sp_Range){0}). */
  else if (g_ret_type == TY_INT) buf_puts(b, "return SP_INT_NIL;\n");
  else if (g_ret_type == TY_FLOAT) buf_puts(b, "return sp_float_nil();\n");
  else if (g_ret_type == TY_STRING) buf_puts(b, "return NULL;\n");
  else buf_printf(b, "return %s;\n", default_value(g_ret_type));
}

/* An empty `[]` / `{}` literal, possibly wrapped in a freeze. Such a literal
   carries no element type of its own and falls back to an IntArray (or a
   str-keyed hash), which mismatches a slot an --rbs seed pinned to another
   storage kind -- the emitted C then assigns an sp_IntArray * to an
   sp_PolyArray * slot. `.freeze` is identity for the shape, so see through it
   and build the literal at the slot's kind, exactly as the parameter and ivar
   paths already do (#3418). */
static int empty_literal_node(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  const char *ty = v >= 0 ? nt_type(nt, v) : NULL;
  if (ty && sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, v, "name");
    int r = nt_ref(nt, v, "receiver");
    int ac = 0; int args = nt_ref(nt, v, "arguments");
    if (args >= 0) nt_arr(nt, args, "arguments", &ac);
    if (nm && r >= 0 && ac == 0 && sp_streq(nm, "freeze")) return empty_literal_node(c, r);
  }
  return v;
}

void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);

/* A rescue type that really does catch every exception. Only Exception does:
   `rescue StandardError` must let Interrupt/SystemExit/ScriptError through and
   `rescue RuntimeError` must not swallow its siblings, so both go through the
   hierarchy-aware matcher instead (#3032). */
int rescue_is_catchall_name(const char *n) {
  return n && sp_streq(n, "Exception");
}

/* Return 1 if the subtree at id contains a RetryNode (not crossing DefNode). */
int subtree_has_retry(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "DefNode")) return 0;
  if (sp_streq(ty, "RetryNode")) return 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(nt, id, i); if (subtree_has_retry(nt, ch)) return 1; }
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_retry(nt, ids[k])) return 1;
  }
  return 0;
}

/* Emit one rescue clause (and its `subsequent` chain) inside the handler
   branch. Frame counter `fr` makes the saved cls/msg vars unique. */
void emit_rescue(Compiler *c, int id, Buf *b, int indent, int fr, const char *resultvar) {
  const NodeTable *nt = c->nt;
  int nexc = 0;
  const int *exc = nt_arr(nt, id, "exceptions", &nexc);
  int ref = nt_ref(nt, id, "reference");
  int stmts = nt_ref(nt, id, "statements");
  int sub = nt_ref(nt, id, "subsequent");

  int rc = ++g_tmp;
  emit_indent(b, indent);
  buf_printf(b, "const char *_rcls_%d = (const char *)sp_last_exc_cls; (void)_rcls_%d;\n", rc, rc);
  emit_indent(b, indent);
  buf_printf(b, "const char *_rmsg_%d = sp_exc_msg[sp_exc_top]; (void)_rmsg_%d;\n", rc, rc);

  /* type-match condition: an explicit StandardError-ish type is treated as a
     catch-all; otherwise exact class-name (hierarchy-aware) match. A BARE
     rescue (no types) is NOT unconditional: CRuby matches only StandardError
     and its subclasses, so a raised Exception/ScriptError/NotImplementedError
     falls through to a later `rescue Exception`. */
  int bare = (nexc == 0);
  int catchall = 0;
  for (int i = 0; i < nexc; i++) {
    const char *en = nt_type(nt, exc[i]);
    if (en && sp_streq(en, "ConstantReadNode") && rescue_is_catchall_name(nt_str(nt, exc[i], "name")))
      catchall = 1;
  }

  const char *save_cls = g_rescue_cls, *save_msg = g_rescue_msg;
  /* NOT static: a rescue body containing its own begin/rescue re-enters this
     function, and shared buffers meant the inner clause's numbering overwrote
     the outer one's in place. The save/restore below puts back the POINTER, so
     the outer bare `raise` then emitted the inner frame's _rcls_N / _rmsg_N,
     which is out of scope by then and failed the C build (#4052). One buffer
     per clause, alive for as long as its body is emitted, is the fix. */
  char clsbuf[32], msgbuf[32];
  snprintf(clsbuf, sizeof clsbuf, "_rcls_%d", rc);
  snprintf(msgbuf, sizeof msgbuf, "_rmsg_%d", rc);

  if (!catchall) {
    emit_indent(b, indent);
    buf_puts(b, "if (");
    if (bare) {
    /* bare rescue: match StandardError and its subclasses only */
    buf_printf(b, "sp_exc_is_standard_error(_rcls_%d)", rc);
    }
    else {
    int first = 1;
    for (int i = 0; i < nexc; i++) {
      const char *en = nt_type(nt, exc[i]);
      /* `rescue *list`: decide against the list's members at run time, so an
         empty list matches nothing and a non-class member is a TypeError */
      if (en && sp_streq(en, "SplatNode")) {
        int sx = nt_ref(nt, exc[i], "expression");
        if (sx >= 0) {
          if (!first) buf_puts(b, " || ");
          first = 0;
          buf_printf(b, "sp_exc_matches_splat(_rcls_%d, ", rc);
          emit_boxed(c, sx, b);
          buf_puts(b, ")");
          continue;
        }
      }
      /* an operand that is plainly not a class or module is a TypeError in
         CRuby, not a clause that matches everything (#3712) */
      if (en && !sp_streq(en, "ConstantReadNode") && !sp_streq(en, "ConstantPathNode")) {
        TyKind ot = comp_ntype(c, exc[i]);
        if (ot == TY_INT || ot == TY_FLOAT || ot == TY_STRING || ot == TY_SYMBOL ||
            ot == TY_BOOL || ot == TY_NIL || ty_is_array(ot) || ty_is_hash(ot)) {
          if (!first) buf_puts(b, " || ");
          first = 0;
          buf_puts(b, "((void)("); emit_expr(c, exc[i], b);
          buf_puts(b, "), sp_raise_cls(\"TypeError\", \"class or module required for rescue clause\"), 0)");
          continue;
        }
      }
      if (!en || (!sp_streq(en, "ConstantReadNode") && !sp_streq(en, "ConstantPathNode"))) continue;
      if (!first) buf_puts(b, " || ");
      first = 0;
      const char *ename = nt_str(nt, exc[i], "name");
      /* A builtin namespaced exception (e.g. StringScanner::Error) is raised
         under its flattened runtime name "StringScanner_Error". Map the path
         to that form only when it names a known builtin exception, so user
         classes like M::Err keep matching on their leaf name. */
      char enbuf[128];
      if (sp_streq(en, "ConstantPathNode")) {
        int par = nt_ref(nt, exc[i], "parent");
        const char *pnm = (par >= 0 && nt_type(nt, par) &&
                           sp_streq(nt_type(nt, par), "ConstantReadNode"))
                          ? nt_str(nt, par, "name") : NULL;
        if (pnm && ename) {
          /* qualified form first (Math::DomainError), then the legacy
             flattened form some package raisers still use */
          snprintf(enbuf, sizeof enbuf, "%s::%s", pnm, ename);
          if (is_exc_name(enbuf)) ename = enbuf;
          else {
            snprintf(enbuf, sizeof enbuf, "%s_%s", pnm, ename);
            if (is_exc_name(enbuf)) ename = enbuf;
          }
        }
      }
      /* A user exception class is raised under its QUALIFIED Ruby name
         (self->cls_name = "App::Error" -- the exception-subclass constructor
         emission uses class_ruby_name). Canonicalize the rescue target
         through the (leaf-keyed) class table so both a namespaced arm
         (`rescue App::Error`, whose AST name is the leaf) and a
         leaf-referenced arm inside the module match the raised name. Resolve
         the index BEFORE swapping in the qualified name. */
      int uci = (ename && !is_exc_name(ename)) ? comp_class_index(c, ename) : -1;
      if (uci >= 0) {
        const char *qn = class_ruby_name(c, uci);
        if (qn) ename = qn;
      }
      /* use hierarchy-aware check for exception classes */
      int is_exc_cls = (ename && is_exc_name(ename)) ||
                       (uci >= 0 && class_is_exc_subclass(c, uci));
      /* A namespaced rescue target with NO user class behind it (a package's
         C-raised exception like JSON::ParserError) also matches the raised
         name in its `Parent::Leaf` form: the raiser uses the qualified
         string (so e.class displays like CRuby), while this arm's AST leaf
         alone would only ever match the bare name. */
      char qbuf[160]; qbuf[0] = 0;
      if (sp_streq(en, "ConstantPathNode") && uci < 0 && ename && !is_exc_name(ename)) {
        /* Rebuild the qualified name by walking the parent chain: a plain
           `JSON::ParserError` has a ConstantReadNode parent; a root-anchored
           `::JSON::ParserError` nests a parent-less ConstantPathNode (the
           leading `::`), which must resolve to the same qualified string. */
        char segs[8][64]; int nseg = 0;
        int qpar = nt_ref(nt, exc[i], "parent");
        int ok = 1;
        while (qpar >= 0 && nseg < 8) {
          const char *pty = nt_type(nt, qpar);
          const char *pn = nt_str(nt, qpar, "name");
          if (!pty || !pn) { ok = 0; break; }
          if (sp_streq(pty, "ConstantReadNode")) {
            snprintf(segs[nseg++], sizeof segs[0], "%s", pn);
            break;
          }
          if (sp_streq(pty, "ConstantPathNode")) {
            snprintf(segs[nseg++], sizeof segs[0], "%s", pn);
            qpar = nt_ref(nt, qpar, "parent");   /* -1 = root anchor: done */
            continue;
          }
          ok = 0; break;
        }
        if (ok && nseg > 0) {
          size_t o = 0;
          for (int si = nseg - 1; si >= 0; si--) {
            int w = snprintf(qbuf + o, sizeof qbuf - o, "%s::", segs[si]);
            if (w < 0 || (size_t)w >= sizeof qbuf - o) { qbuf[0] = 0; break; }
            o += (size_t)w;
          }
          if (qbuf[0]) snprintf(qbuf + o, sizeof qbuf - o, "%s", ename);
        }
      }
      /* strcmp rather than sp_str_eq for the name compares: sp_str_eq confirms
         a strcmp hit by comparing byte lengths, and taking the length of a bare
         C literal reads its s[-1] marker -- out of bounds, and whatever byte
         precedes it in rodata. Land on a marker value there and it reads the
         bytes BEFORE the literal as an sp_str_hdr and answers false for two
         equal names, with the rodata layout (so the optimizer) deciding. A
         class or module name carries no NUL, and _rcls_N is never NULL, so
         strcmp is the right comparison here anyway. */
      if (is_exc_cls)
        buf_printf(b, "sp_exc_cls_matches(_rcls_%d, \"%s\")", rc, ename);
      else if (qbuf[0])
        buf_printf(b, "(strcmp(_rcls_%d, \"%s\") == 0 || strcmp(_rcls_%d, \"%s\") == 0)",
                   rc, ename, rc, qbuf);
      else {
        /* `rescue M` where M is an included module: an exception matches when
           its class (or an ancestor) includes M, so expand to the exception
           classes carrying the module. A module nobody includes keeps the
           plain name compare (which, like CRuby, never matches a class). */
        buf_printf(b, "(strcmp(_rcls_%d, \"%s\") == 0", rc, ename);
        if (uci >= 0) {
          for (int k = 0; k < c->nclasses; k++) {
            if (!class_is_exc_subclass(c, k)) continue;
            int inc = 0;
            for (int a = k; a >= 0 && !inc; a = c->classes[a].parent)
              for (int m = 0; m < c->classes[a].nincluded_mods; m++)
                if (c->classes[a].included_mods[m] == uci) { inc = 1; break; }
            if (!inc) continue;
            const char *kq = class_ruby_name(c, k);
            buf_printf(b, " || sp_exc_cls_matches(_rcls_%d, \"%s\")",
                       rc, kq ? kq : c->classes[k].name);
          }
        }
        buf_puts(b, ")");
      }
    }
    if (first) buf_puts(b, "1");  /* no usable type -> always */
    }
    buf_puts(b, ") {\n");
    indent++;
  }

  g_rescue_cls = clsbuf; g_rescue_msg = msgbuf;
  /* If the analyzer specialized a `=> e` binding to a user exception subclass
     object (one arm, one class, no name collision -- see the rescue-var typing
     in analyze_program), the carried object must be kept so its ivars survive
     the raise (#1415). The carried slot is at sp_exc_top (the just-popped frame,
     same index _rmsg reads). A degenerate no-object raise of that class falls
     back to a freshly built subclass struct so ivar reads stay in-bounds. */
  int spec_cid = -1;
  int has_bind = (ref >= 0 && nt_type(nt, ref) && sp_streq(nt_type(nt, ref), "LocalVariableTargetNode"));
  if (has_bind) {
    LocalVar *vlv = scope_local(comp_scope_of(c, ref), nt_str(nt, ref, "name"));
    if (vlv && ty_is_object(vlv->type)) {
      int xc = ty_object_class(vlv->type);
      if (xc >= 0 && class_is_exc_subclass(c, xc)) spec_cid = xc;
    }
  }
  /* Materialize the exception being handled exactly once -- the carried object
     if any, else a freshly built one -- then thread the cause captured at raise
     time onto it. Pushing the same object onto sp_exc_handling (so a re-raise in
     the body threads it as #cause) and binding it to `=> e` keeps the cause chain
     consistent, and covers an explicitly raised object too (it has a carried
     object, so it flows through the same path). */
  emit_indent(b, indent);
  if (spec_cid >= 0) {
    const char *xn = c->classes[spec_cid].name;
    if (bare) {
      /* A bare arm matches any StandardError, so the carried-object cast to
         the specialized class must be guarded by a class match: a foreign
         exception arriving here binds a fresh zero-ivar struct instead. */
      const char *qn = class_ruby_name(c, spec_cid);
      buf_printf(b, "sp_Exception *_ce_%d = (sp_exc_obj[sp_exc_top] && sp_exc_cls_matches(_rcls_%d, \"%s\"))"
                    " ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                    " : (sp_Exception *)sp_exc_new_sub_sized(sizeof(sp_%s), _rcls_%d, _rmsg_%d);\n",
                 rc, rc, qn ? qn : xn, xn, rc, rc);
    }
    else
      buf_printf(b, "sp_Exception *_ce_%d = sp_exc_obj[sp_exc_top] ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                    " : (sp_Exception *)sp_exc_new_sub_sized(sizeof(sp_%s), _rcls_%d, _rmsg_%d);\n",
                 rc, xn, rc, rc);
  }
  else
    buf_printf(b, "sp_Exception *_ce_%d = sp_exc_obj[sp_exc_top] ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                  " : sp_exc_new_for_catch(_rcls_%d, _rmsg_%d);\n", rc, rc, rc);
  emit_indent(b, indent);
  /* an exception never loses a cause it already carries (#3745) */
  buf_printf(b, "if (!_ce_%d->cause) _ce_%d->cause = (sp_Exception *)sp_pending_cause; sp_pending_cause = NULL;\n", rc, rc);
  emit_indent(b, indent);
  buf_printf(b, "sp_rescue_push((void *)_ce_%d);\n", rc);
  g_rescue_save_stack[g_rescue_save_depth++] = (RescueSave){ g_exc_frame_depth };
  if (has_bind) {
    emit_indent(b, indent);
    /* rename_local: inside a yield-inlined method body the binding local was
       declared under its per-inline name (lv__yN_e); the bare name here left
       the assignment referencing an undeclared lv_e. */
    if (spec_cid >= 0)
      buf_printf(b, "lv_%s = (sp_%s *)_ce_%d;\n", rename_local(nt_str(nt, ref, "name")), c->classes[spec_cid].c_name, rc);
    else
      /* bind the materialized object (which already prefers the CARRIED object
         from `raise <exception-object>` / `raise Cls.new`): the rescue variable,
         $!, and the raised object are one identity, since $! reads the same
         sp_exc_handling top this arm just pushed. */
      buf_printf(b, "lv_%s = _ce_%d;\n", rename_local(nt_str(nt, ref, "name")), rc);
  }
  if (resultvar) {
    const char *sv = g_result_var; g_result_var = resultvar;
    emit_stmts_tail(c, stmts, b, indent);
    g_result_var = sv;
  }
  else {
    emit_stmts(c, stmts, b, indent);
  }
  g_rescue_save_depth--;
  emit_indent(b, indent);
  buf_puts(b, "sp_rescue_sp--;\n");
  g_rescue_cls = save_cls; g_rescue_msg = save_msg;

  if (!catchall) {
    indent--;
    emit_indent(b, indent);
    buf_puts(b, "}\n");
    emit_indent(b, indent);
    buf_puts(b, "else {\n");
    if (sub >= 0) emit_rescue(c, sub, b, indent + 1, fr, resultvar);
    else {
      /* re-stage the carried object so a pass-through keeps ivars and the
         SystemExit status (#1415, #2761) */
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_pending_exc_obj = sp_exc_obj[sp_exc_top];\n");
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_raise_cls(_rcls_%d, _rmsg_%d);\n", rc, rc);
    }
    emit_indent(b, indent);
    buf_puts(b, "}\n");
  }
}

/* begin/body/rescue (ensure/else deferred) via the setjmp exception model.
   When resultvar != NULL, the body's and rescue handlers' values are
   assigned to it (begin/rescue as an expression). */
void emit_begin(Compiler *c, int id, Buf *b, int indent, const char *resultvar) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, id, "statements");
  int rescue = nt_ref(nt, id, "rescue_clause");
  int else_c = nt_ref(nt, id, "else_clause");
  int ensure_c = nt_ref(nt, id, "ensure_clause");
  int else_stmts = else_c >= 0 ? nt_ref(nt, else_c, "statements") : -1;
  int ensure_stmts = ensure_c >= 0 ? nt_ref(nt, ensure_c, "statements") : -1;
  /* A `begin ... end` with no rescue, else or ensure protects nothing -- the
     `begin ... end while cond` do-while idiom is the common shape -- so it
     needs no handler frame at all. Emitting one put a setjmp on APU's
     per-sample path, 3.6% of optcarrot's profile. */
  if (rescue < 0 && else_stmts < 0 && ensure_stmts < 0) {
    if (body >= 0) {
      if (resultvar) {
        const char *sv0 = g_result_var; g_result_var = resultvar;
        emit_stmts_tail(c, body, b, indent);
        g_result_var = sv0;
      }
      else emit_stmts(c, body, b, indent);
    }
    return;
  }
  int fr = ++g_tmp;
  /* GC root watermark for the protected region: a raise unwinds via longjmp,
     which skips the __attribute__((cleanup)) pops of any SP_GC_ROOT locals in
     the body (or in runtime helpers it called), leaving stale entries on the
     root stack. Restore to this watermark on the exception landing, before the
     rescue/ensure bodies allocate, so GC never marks a dead stack slot. The
     watermark lives in sp_exc_rootmark beside the handler slot, NOT in a C
     local: an extra local per protected region measurably shifts hot-function
     frames (optcarrot). */

  if (ensure_stmts >= 0 && g_ensure_depth < MAX_ENSURE_DEPTH) {
    /* Ensure clause present: use goto-based deferred-return mechanism so that
       a `return` inside the body still runs the ensure before leaving. */
    int eid = ++g_tmp;
    int has_retval = (g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN);
    emit_indent(b, indent); buf_printf(b, "int _retf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "int _nxtf%d = 0; (void)_nxtf%d;\n", eid, eid);
    /* _excf/_excmsg/_exccls track an unhandled exception (no rescue) so
       that ensure can re-raise it after running.  Saved immediately after
       sp_exc_top-- while the index is still valid. */
    emit_indent(b, indent); buf_printf(b, "int _excf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_excmsg%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_exccls%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "void *_excobj%d = NULL;\n", eid);
    if (has_retval) {
      emit_indent(b, indent); emit_ctype(c, g_ret_type, b);
      buf_printf(b, " _retv%d = %s;\n", eid, default_value(g_ret_type));
    }
    g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth };

    /* retry in the rescue restarts the body; the ensure runs only when the
       begin finally exits (matching CRuby, where an aborted attempt does not
       run it). Same label mechanism as the no-ensure path below. */
    int ens_has_retry = (rescue >= 0) && subtree_has_retry(c->nt, rescue);
    char ens_retry_label[32]; ens_retry_label[0] = 0;
    const char *ens_saved_retry = g_retry_label;
    if (ens_has_retry) {
      snprintf(ens_retry_label, sizeof ens_retry_label, "_retry_%d", eid);
      buf_printf(b, "%s:;\n", ens_retry_label);
      g_retry_label = ens_retry_label;
    }
    emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    g_exc_frame_depth++;
    if (resultvar && else_stmts < 0) {
      const char *sv = g_result_var; g_result_var = resultvar;
      emit_stmts_tail(c, body, b, indent + 1);
      g_result_var = sv;
    }
    else {
      emit_stmts(c, body, b, indent + 1);
    }
    g_exc_frame_depth--;
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    if (else_stmts >= 0) {
      if (resultvar) {
        const char *sv = g_result_var; g_result_var = resultvar;
        emit_stmts_tail(c, else_stmts, b, indent + 1);
        g_result_var = sv;
      }
      else emit_stmts(c, else_stmts, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n");
    /* A non-local unwind (proc return / throw) only passes through here; it is
       not an exception, so skip rescue -- only the ensure (below) runs. */
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) {\n");
    /* A real exception unwound past any proc-return home methods between the
       raise and here; drop their now-dead nodes so a later proc-return misses
       and raises LocalJumpError instead of longjmping into a freed C frame. */
    emit_indent(b, indent + 2); buf_puts(b, "sp_proc_homes_unwind();\n");
    if (rescue >= 0) {
      /* A Fiber#kill signal bypasses every rescue clause -- defer it to the
         ensure + re-raise path (FiberKillSignal must match lib/sp_fiber.c) so
         this begin's ensure still runs, then it propagates toward the fiber. */
      emit_indent(b, indent + 2);
      /* strcmp, not sp_str_eq: the latter checks byte lengths on a hit, and
         reading a bare C literal's length means reading its out-of-bounds
         s[-1] marker. sp_last_exc_cls is never NULL (it starts at
         sp_str_empty) and a class name carries no NUL. */
      buf_printf(b, "if (strcmp((const char *)sp_last_exc_cls, \"FiberKillSignal\") == 0) { _excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; }\n",
                 eid, eid, eid);
      emit_indent(b, indent + 2); buf_puts(b, "else {\n");
      emit_rescue(c, rescue, b, indent + 3, fr, resultvar);
      emit_indent(b, indent + 2); buf_puts(b, "}\n");
    }
    else {
      /* No rescue: save exception info for re-raise after ensure runs.
         sp_exc_top has just been decremented so sp_exc_top is the right index. */
      emit_indent(b, indent + 2);
      buf_printf(b, "_excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; _excobj%d = sp_exc_obj[sp_exc_top];\n",
                 eid, eid, eid, eid);
    }
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");

    g_ensure_depth--;

    /* Ensure label: reached by deferred-return goto AND by normal fall-through. */
    buf_printf(b, "_ensure%d: ;\n", eid);
    /* Save the in-flight unwind state across the ensure body: a nested throw /
       catch / proc-return that completes *inside* this ensure resets the globals
       to SP_UNWIND_NONE, which would otherwise drop an outer unwind passing
       through. An ensure that starts its own escaping unwind longjmps out before
       the restore, so that new unwind correctly supersedes. */
    emit_indent(b, indent);
    buf_printf(b, "int _uk%d = sp_unwind_kind, _ut%d = sp_unwind_target, _ue%d = sp_unwind_exc_top; sp_proc_home *_uh%d = sp_unwind_home;\n",
               eid, eid, eid, eid);
    /* an exception raised from inside this ensure takes the one unwinding
       through it as its cause (#3745) */
    emit_indent(b, indent);
    buf_printf(b, "void *_ic%d = sp_inflight_cause;"
                  " if (_excf%d) sp_inflight_cause = _excobj%d ? _excobj%d"
                  " : (void *)sp_exc_new_for_catch(_exccls%d, _excmsg%d);\n",
               eid, eid, eid, eid, eid, eid);
    emit_stmts(c, ensure_stmts, b, indent);
    emit_indent(b, indent);
    buf_printf(b, "sp_inflight_cause = _ic%d;\n", eid);
    emit_indent(b, indent);
    buf_printf(b, "sp_unwind_kind = _uk%d; sp_unwind_target = _ut%d; sp_unwind_exc_top = _ue%d; sp_unwind_home = _uh%d;\n",
               eid, eid, eid, eid);

    /* A non-local unwind passing through has now run this ensure: continue to the
       next handler or deliver to its target (never falls through to the
       deferred-return / re-raise propagation below). */
    emit_indent(b, indent);
    buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");

    /* a deferred `next`: chain to the enclosing ensure when that region is
       still inside the loop, else run the C continue here */
    if (g_ensure_depth > g_loop_ensure_base) {
      EnsureCtx *outer2 = &g_ensure_stack[g_ensure_depth - 1];
      emit_indent(b, indent);
      buf_printf(b, "if (_nxtf%d) { _nxtf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                 eid, outer2->lid, outer2->lid);
    }
    else if (g_c_loop_depth > 0) {
      emit_indent(b, indent);
      buf_printf(b, "if (_nxtf%d) continue;\n", eid);
    }
    emit_indent(b, indent);
    if (g_ensure_depth > 0) {
      EnsureCtx *outer = &g_ensure_stack[g_ensure_depth - 1];
      if (has_retval && outer->has_retval) {
        buf_printf(b, "if (_retf%d) { _retv%d = _retv%d; _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                   eid, outer->lid, eid, outer->lid, outer->lid);
      }
      else {
        buf_printf(b, "if (_retf%d) { _retf%d = 1; sp_exc_top--; goto _ensure%d; }\n",
                   eid, outer->lid, outer->lid);
      }
      /* Unhandled exception: propagate info to outer ensure context. */
      emit_indent(b, indent);
      buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; _excobj%d = _excobj%d; sp_exc_top--; goto _ensure%d; }\n",
                 eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid, eid, outer->lid);
    }
    else {
      /* the deferred return leaves through every enclosing live begin frame:
         pop them or their jmp_bufs dangle into this soon-dead C frame */
      {
        char g[24]; snprintf(g, sizeof g, "_retf%d", eid);
        if (emit_frame_unwind(b, 0, g)) { buf_puts(b, "\n"); emit_indent(b, indent); }
      }
      /* inside a first-class proc body routing returns through the boxed slot
         (the universal proc return ABI) the deferred value returns through the
         slot, not a raw C return of an sp_RbVal from an sp_int function */
      if (has_retval && g_in_proc_body && g_result_var && g_result_poly)
        buf_printf(b, "if (_retf%d) { %s = _retv%d; return 0; }\n", eid, g_result_var, eid);
      else if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d;\n", eid, eid);
      else if (g_in_proc_body && g_result_var && g_result_poly)
        buf_printf(b, "if (_retf%d) { %s = sp_box_nil(); return 0; }\n", eid, g_result_var);
      else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil();\n", eid);
      else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0;\n", eid); /* main() */
      else buf_printf(b, "if (_retf%d) return;\n", eid);
      /* Unhandled exception: re-raise using the saved class/message. */
      emit_indent(b, indent);
      buf_printf(b, "if (_excf%d) { sp_pending_exc_obj = _excobj%d; sp_raise_cls(_exccls%d, _excmsg%d); }\n", eid, eid, eid, eid);
    }
    g_retry_label = ens_saved_retry;
    return;
  }

  /* No ensure (or ensure depth limit reached): original structure.
     When the rescue handler contains `retry`, wrap with a goto label so retry
     can restart the body. */
  int has_retry = (rescue >= 0) && subtree_has_retry(c->nt, rescue);
  int rl = has_retry ? ++g_tmp : -1;
  char retry_label[32]; retry_label[0] = 0;
  if (has_retry) snprintf(retry_label, sizeof retry_label, "_retry_%d", rl);
  if (has_retry) buf_printf(b, "%s:;\n", retry_label);
  const char *saved_retry = g_retry_label;
  if (has_retry) g_retry_label = retry_label;

  emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;\n");
  emit_indent(b, indent); buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
  emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
  g_exc_frame_depth++;
  /* body value is the begin value only when there is no else clause */
  if (resultvar && else_stmts < 0) {
    const char *sv = g_result_var; g_result_var = resultvar;
    emit_stmts_tail(c, body, b, indent + 1);
    g_result_var = sv;
  }
  else {
    emit_stmts(c, body, b, indent + 1);
  }
  g_exc_frame_depth--;
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  if (else_stmts >= 0) {  /* else runs only on success; its value is the begin value */
    if (resultvar) {
      const char *sv = g_result_var; g_result_var = resultvar;
      emit_stmts_tail(c, else_stmts, b, indent + 1);
      g_result_var = sv;
    }
    else {
      emit_stmts(c, else_stmts, b, indent + 1);
    }
  }
  if (ensure_stmts >= 0) emit_stmts(c, ensure_stmts, b, indent + 1);
  emit_indent(b, indent); buf_puts(b, "}\n");
  emit_indent(b, indent); buf_puts(b, "else {\n");
  emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
  emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n");
  /* Drop home nodes a real exception unwound past (no-op for a throw/proc-return
     pass-through, where sp_unwind_kind is set); see the tail-position begin. */
  emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) sp_proc_homes_unwind();\n");
  /* A non-local unwind (proc return / throw) only passes through; skip rescue. */
  if (rescue >= 0) {
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) {\n");
    /* A Fiber#kill signal bypasses every rescue clause. This begin has no ensure
       to run first, so re-raise it straight away (FiberKillSignal must match
       lib/sp_fiber.c); it propagates toward the fiber's terminating trampoline. */
    emit_indent(b, indent + 2);
    buf_puts(b, "if (strcmp((const char *)sp_last_exc_cls, \"FiberKillSignal\") == 0) sp_raise_cls(\"FiberKillSignal\", sp_exc_msg[sp_exc_top]);\n");
    emit_indent(b, indent + 2); buf_puts(b, "else {\n");
    emit_rescue(c, rescue, b, indent + 3, fr, resultvar);
    emit_indent(b, indent + 2); buf_puts(b, "}\n");
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
  }
  if (ensure_stmts >= 0) {
    /* preserve an outer unwind across a nested one completing inside the ensure */
    int uid = ++g_tmp;
    emit_indent(b, indent + 1);
    buf_printf(b, "int _uk%d = sp_unwind_kind, _ut%d = sp_unwind_target, _ue%d = sp_unwind_exc_top; sp_proc_home *_uh%d = sp_unwind_home;\n",
               uid, uid, uid, uid);
    emit_stmts(c, ensure_stmts, b, indent + 1);
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_unwind_kind = _uk%d; sp_unwind_target = _ut%d; sp_unwind_exc_top = _ue%d; sp_unwind_home = _uh%d;\n",
               uid, uid, uid, uid);
  }
  emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
  emit_indent(b, indent); buf_puts(b, "}\n");
  g_retry_label = saved_retry;
}

/* Wrap a line-emitting statement so any expression preludes are flushed
   before the line itself. */
void emit_with_prelude(Compiler *c, int id, Buf *b, int indent,
                              void (*inner)(Compiler *, int, Buf *, int)) {
  Buf *savePre = g_pre;
  int saveIndent = g_indent;
  Buf pre;  memset(&pre, 0, sizeof pre);
  Buf line; memset(&line, 0, sizeof line);
  g_pre = &pre;
  g_indent = indent;
  inner(c, id, &line, indent);
  g_pre = savePre;
  g_indent = saveIndent;
  if (pre.p)  buf_puts(b, pre.p);
  if (line.p) buf_puts(b, line.p);
  free(pre.p);
  free(line.p);
}

int g_line_map = 0;
int g_gate_raise = 0;
int g_debug = 0;  /* --debug build: emit user methods with external linkage so
                     -rdynamic names backtrace/caller frames (instance/class
                     methods only; toplevel sp_<name> stays static to avoid
                     colliding with runtime helpers). */
/* Last (line, file) pair emitted, to suppress consecutive duplicates. line 0
   is the sentinel for "none yet" since real source lines are 1-based. */
static int g_lm_last_line = 0;
static int g_lm_last_fid = -1;

void emit_line_directive(Compiler *c, int id, Buf *b) {
  if (!g_line_map) return;
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  if (ln <= 0) return;
  int fid = (int)nt_int(c->nt, id, "node_file", 0);
  if (ln == g_lm_last_line && fid == g_lm_last_fid) return;
  g_lm_last_line = ln;
  g_lm_last_fid = fid;
  const char *path = nt_file_path(c->nt, fid);
  if (!path) path = c->nt->source_file;
  if (!path || !*path) path = "source.rb";
  /* A `#line` directive must start a line. When this statement is emitted
     mid-line (e.g. an inlined block/proc body written after `{ `), break the
     line first so the `#` lands in column 0 rather than as a stray token. */
  if (b->len > 0 && b->p[b->len - 1] != '\n') buf_puts(b, "\n");
  buf_printf(b, "#line %d \"%s\"\n", ln, path);
}

void emit_stmt(Compiler *c, int id, Buf *b, int indent) {
  emit_line_directive(c, id, b);
  emit_with_prelude(c, id, b, indent, emit_stmt_inner);
}
void emit_stmt_tail(Compiler *c, int id, Buf *b, int indent) {
  emit_line_directive(c, id, b);
  emit_with_prelude(c, id, b, indent, emit_stmt_tail_inner);
}

/* Emit a `cond ? nil : <int>` ivar-write RHS as a C `?:` in int context: the
   literal-nil arm becomes the SP_INT_NIL sentinel (a bare NilNode would emit
   `0`, colliding with a real 0), the int arm emits raw. Returns 1 if `v` is that
   shape (exactly one literal-nil arm) and was emitted, else 0 (caller falls
   back to the generic paths). Pairs with analyze's ivar_nullable_int_ternary. */
static int emit_nullable_int_ternary(Compiler *c, int v, Buf *b) {
  int tn, en;
  if (!comp_ternary_arms(c->nt, v, &tn, &en)) return 0;
  const char *tt = nt_type(c->nt, tn), *et = nt_type(c->nt, en);
  int t_nil = tt && sp_streq(tt, "NilNode");
  int e_nil = et && sp_streq(et, "NilNode");
  if (t_nil == e_nil) return 0;  /* exactly one arm a literal nil */
  buf_puts(b, "(");
  emit_cond(c, nt_ref(c->nt, v, "predicate"), b);
  buf_puts(b, " ? ");
  if (t_nil) buf_puts(b, "SP_INT_NIL"); else emit_expr(c, tn, b);
  buf_puts(b, " : ");
  if (e_nil) buf_puts(b, "SP_INT_NIL"); else emit_expr(c, en, b);
  buf_puts(b, ")");
  return 1;
}

/* Emit the `switch` arms for `obj.x = v` where obj's class is only known at
   run time. A class answers the write either through an attribute or through
   an explicit `def x=`; both tables are consulted and the more-derived
   definition wins, with a same-class tie going to the method, exactly as the
   statically typed emitter arbitrates. `objp` is a C expression for the
   receiver as a raw pointer, `src` the value temp, `at` its type. */
static void emit_boxed_writer_arms(Compiler *c, const char *base, const char *nm,
                                   const char *objp, const char *src, TyKind at, Buf *b) {
  for (int k = 0; k < c->nclasses; k++) {
    int wmdc = -1, kmi = -1;
    int kind = comp_resolve_member(c, k, base, 1, &wmdc, &kmi);
    if (kind == SP_MEMBER_METHOD && !scope_has_callable_symbol(c, kmi))
      kind = comp_writer_in_chain(c, k, base, &wmdc) ? SP_MEMBER_ATTR : SP_MEMBER_NONE;
    if (kind == SP_MEMBER_NONE) continue;
    int writer_wins = (kind == SP_MEMBER_ATTR);
    if (!writer_wins) {
      Scope *arm = &c->scopes[kmi];
      TyKind pt = TY_POLY;
      if (arm->pnames && arm->nparams >= 1 && arm->pnames[0]) {
        LocalVar *pl = scope_local(arm, arm->pnames[0]);
        pt = (pl && pl->type != TY_UNKNOWN) ? pl->type : TY_POLY;
      }
      buf_printf(b, " case %d: sp_%s_%s((sp_%s *)%s, ", k,
                 c->classes[wmdc].c_name, mc(arm->name), c->classes[wmdc].c_name, objp);
      if (pt == TY_POLY && at != TY_POLY && at != TY_UNKNOWN) emit_boxed_text(c, at, src, b);
      else if (at == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) emit_unbox_text(c, pt, src, b);
      else buf_puts(b, src);
      buf_puts(b, "); break;");
      continue;
    }
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
    int iv = comp_ivar_index(&c->classes[k], ivn);
    TyKind ivt = iv >= 0 ? c->classes[k].ivar_types[iv] : at;
    /* skip a class whose slot can't hold this concrete rhs (the runtime object
       isn't that class anyway): a raw assignment between mismatched C types
       would not compile */
    if (at != ivt && at != TY_POLY && ivt != TY_POLY) continue;
    buf_printf(b, " case %d: ", k);
    { char opn[64]; snprintf(opn, sizeof opn, "((sp_%s *)%s)", c->classes[k].c_name, objp);
      emit_frozen_obj_guard(c, k, opn, b); }
    buf_printf(b, "((sp_%s *)%s)->iv_%s = ", c->classes[k].c_name, objp, iv_c(base));
    if (ivt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, src, b);
    else if (at == TY_POLY && ivt != TY_POLY) emit_unbox_text(c, ivt, src, b);
    else buf_puts(b, src);
    buf_puts(b, "; break;");
  }
}

/* The statement that brings a synthesized singleton subclass into being --
   `def obj.m`, `class << obj`, `obj.extend(M)` -- is where Ruby dates the
   override from. The object is stamped with its PARENT's cls_id at
   construction (ctor_cls_id) and this flips it; the subclass's own methods and
   is_a? read that id. Nothing to flip for a value-type receiver, which has no
   identity of its own. */
/* The subclass index analyze stamped on a singleton-creating node, or -1. */
int sg_activates_ci(Compiler *c, int node) {
  const char *v = node >= 0 ? nt_str(c->nt, node, "sg_activates") : NULL;
  if (!v) return -1;
  int ci = atoi(v);
  if (ci < 0 || ci >= c->nclasses) return -1;
  if (!c->classes[ci].is_singleton_of || c->classes[ci].is_value_type) return -1;
  return ci;
}

void emit_sg_activate(Compiler *c, int node, int recv, Buf *b, int indent) {
  if (recv < 0) return;
  int ci = sg_activates_ci(c, node);
  if (ci < 0) return;
  if (!c->classes[ci].is_singleton_of || c->classes[ci].is_value_type) return;
  emit_indent(b, indent);
  buf_puts(b, "("); emit_expr(c, recv, b);
  buf_printf(b, ")->cls_id = %d;\n", ci);
}

void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "statement (no type)");

  /* `y << v` / `y.yield(v)` inside an Enumerator.new generator lowers to a
     Fiber.yield. Intercept here so the statement-level `<<` mutation fast path
     does not treat the yielder as an array; emit_expr routes through emit_call's
     yielder rewrite. */
  if (g_yielder_name && sp_streq(ty, "CallNode")) {
    int yrcv = nt_ref(nt, id, "receiver");
    const char *ynm = nt_str(nt, id, "name");
    /* `y << a << b` chains: `y << a` answers the yielder, so the second push
       has the first as its receiver. Emit the receiver's push first and then
       this one, rather than reading the yield's value as a container (#3581).
       Parentheses around the inner push are transparent. */
    int ychain = 0;
    if (yrcv >= 0 && ynm && sp_streq(ynm, "<<")) {
      int r2 = yrcv;
      while (r2 >= 0 && nt_type(nt, r2) && sp_streq(nt_type(nt, r2), "ParenthesesNode")) {
        int pb = nt_ref(nt, r2, "body"); int pn2 = 0;
        const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn2) : NULL;
        r2 = (pn2 == 1 && pd) ? pd[0] : -1;
      }
      if (r2 >= 0 && nt_type(nt, r2) && sp_streq(nt_type(nt, r2), "CallNode") &&
          nt_str(nt, r2, "name") && sp_streq(nt_str(nt, r2, "name"), "<<")) {
        int r3 = nt_ref(nt, r2, "receiver");
        while (r3 >= 0 && nt_type(nt, r3) && sp_streq(nt_type(nt, r3), "CallNode") &&
               nt_str(nt, r3, "name") && sp_streq(nt_str(nt, r3, "name"), "<<"))
          r3 = nt_ref(nt, r3, "receiver");
        if (r3 >= 0 && nt_type(nt, r3) && sp_streq(nt_type(nt, r3), "LocalVariableReadNode") &&
            nt_str(nt, r3, "name") && sp_streq(nt_str(nt, r3, "name"), g_yielder_name)) {
          emit_stmt(c, r2, b, indent);   /* the earlier push(es) in the chain */
          ychain = 1;
          yrcv = r3;
        }
      }
    }
    if (ychain ||
        (yrcv >= 0 && ynm && (sp_streq(ynm, "<<") || sp_streq(ynm, "yield")) &&
        nt_type(nt, yrcv) && sp_streq(nt_type(nt, yrcv), "LocalVariableReadNode") &&
        nt_str(nt, yrcv, "name") && sp_streq(nt_str(nt, yrcv, "name"), g_yielder_name))) {
      /* Emit the Fiber.yield directly: re-dispatching `y.yield(v)` through
         emit_expr would let the yield-keyword inlining intercept it. */
      int yar = nt_ref(nt, id, "arguments");
      int yac = 0; const int *yav = yar >= 0 ? nt_arr(nt, yar, "arguments", &yac) : NULL;
      emit_indent(b, indent);
      buf_puts(b, "sp_Fiber_yield(");
      if (yac == 1) emit_boxed(c, yav[0], b);
      else if (yac > 1) {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", t, t);
        for (int k = 0; k < yac; k++) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, yav[k], g_pre); buf_puts(g_pre, ");\n");
        }
        buf_printf(b, "sp_box_poly_array(_t%d)", t);
      }
      else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ");\n");
      return;
    }
  }

  /* `define_method` and the `[lits].each { define_method ... }` unroll are
     resolved at analyze time into real method scopes; emit nothing here. */
  if (sp_streq(ty, "CallNode")) {
    const char *cnm = nt_str(nt, id, "name");
    if (cnm && sp_streq(cnm, "define_method") && nt_ref(nt, id, "receiver") < 0) return;
    /* define_singleton_method on a supported target (no receiver, `self`, or a
       class-constant / namespaced-class receiver) is resolved into a class-method
       scope at analyze time, so the call emits no runtime code. Mirror exactly the
       receivers analyze registers; an arbitrary-instance receiver is NOT no-op'd
       here -- it falls through to the normal unresolved-call reject at this site. */
    if (cnm && sp_streq(cnm, "define_singleton_method")) {
      int dsm_recv = nt_ref(nt, id, "receiver");
      if (dsm_recv < 0) return;
      const char *dsm_rty = nt_type(nt, dsm_recv);
      if (dsm_rty && (sp_streq(dsm_rty, "SelfNode") || sp_streq(dsm_rty, "ConstantReadNode") ||
                      sp_streq(dsm_rty, "ConstantPathNode"))) {
        /* A CLASS constant resolves to a class-method scope and emits nothing.
           An INSTANCE constant (`B = Box.new`) matches the same node type but
           is a singleton binding, and the statement is where its override
           starts (#4084) -- a no-op for anything analyze did not mark. */
        emit_sg_activate(c, id, dsm_recv, b, indent);
        return;
      }
    }
    /* `class_eval/module_eval { defs }` reopen: the block's def/define_method
       were registered as the target's methods at analyze time and are emitted
       separately; the call itself is a no-op at runtime. g_class_body_id resolves
       a `self.` receiver in a class body (a bare receiver is already filtered out
       upstream by the class-body statement loop). */
    if (class_eval_reopen_class(c, id, g_class_body_id) >= 0) return;
    if (cnm && sp_streq(cnm, "each") && nt_ref(nt, id, "block") >= 0) {
      int rcv = nt_ref(nt, id, "receiver");
      if (rcv >= 0 && nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "ArrayNode")) {
        int eblk = nt_ref(nt, id, "block");
        int ebody = nt_ref(nt, eblk, "body");
        int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
        if (ebn == 1 && nt_type(nt, ebb[0]) && sp_streq(nt_type(nt, ebb[0]), "CallNode") &&
            nt_str(nt, ebb[0], "name") && sp_streq(nt_str(nt, ebb[0], "name"), "define_method"))
          return;
      }
    }
  }

  /* `alias new old` is resolved into the class alias table at analyze time
     (register_aliases) and generates no runtime code. A bare alias in a class
     body is skipped by the class-body loop; a statement modifier (`alias a b
     if cond`) wraps it in an IfNode whose then-branch routes the alias here.
     register_aliases only records an alias whose modifier condition is statically
     satisfied, so a constant-false guard creates nothing here and a non-constant
     guard leaves the name unresolved (it rejects loudly at the use site). The
     node itself emits no runtime code. */
  if (sp_streq(ty, "AliasMethodNode")) return;

  if (sp_streq(ty, "YieldNode")) {
    if (g_current_scope_is_lowered) {
      int yargs = nt_ref(nt, id, "arguments");
      int yargc = 0; const int *yargv = yargs >= 0 ? nt_arr(nt, yargs, "arguments", &yargc) : NULL;
      emit_indent(b, indent);
      buf_puts(b, "sp_proc_call(");
      emit_yblk_ref(b);
      buf_puts(b, ", ");
      /* force_poly=1: a rest/post-taking block recovers arguments from the boxed
         side-channel, and this callee's signature is unknown here. The lean
         unboxed ABI only ever served this self-recursive-yield path. */
      emit_proc_call_args(c, yargc, yargv, b, 1);
      buf_puts(b, ";\n");
      return;
    }
    if (g_yield_proc_ref) {
      /* The inlined callee's block is a forwarded real proc (the caller
         nil-checks its &block); call the proc instead of splicing a body. */
      emit_yield_proc_call(c, nt_ref(nt, id, "arguments"), TY_VOID, b, indent, 0);
      return;
    }
    if (g_block_id < 0) {
      /* Reaching a yield emission with no block means the yield is unguarded --
         a `yield if block_given?` folds its guard to a compile-time false and
         is never emitted here. An unguarded yield with no block raises
         LocalJumpError (a guarded one that still reaches codegen sits inside an
         `if (0)` and never executes the raise). */
      emit_indent(b, indent);
      buf_puts(b, "sp_exc_stage_key(sp_box_str((&(\"\\xff\" \"noreason\")[1])));\n");
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"LocalJumpError\", \"no block given (yield)\");\n");
      return;
    }
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0, TY_VOID);
    return;
  }

  if (sp_streq(ty, "CallNode")) {
    /* Reflection-mutation calls (remove_method/undef_method/remove_class_variable)
       in a class body are otherwise dropped silently; report the documented
       limit instead of pretending they took effect (#2954, #2955). */
    {
      const char *rnm = nt_str(nt, id, "name");
      if (rnm && (sp_streq(rnm, "remove_method") || sp_streq(rnm, "undef_method") ||
                  sp_streq(rnm, "remove_class_variable")) &&
          diagnose_unsupported_call(c, id)) return;
    }
    /* declarative-only calls emitted as no-ops */
    {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 && nm && (sp_streq(nm, "include") || sp_streq(nm, "extend") ||
                             sp_streq(nm, "prepend") || sp_streq(nm, "module_function") ||
                             sp_streq(nm, "private") || sp_streq(nm, "protected") ||
                             sp_streq(nm, "public") || sp_streq(nm, "attr_reader") ||
                             sp_streq(nm, "attr_writer") || sp_streq(nm, "attr_accessor") ||
                             sp_streq(nm, "attr"))) {
        /* These are class-body declarations handled at analysis time; skip.
           Exception: a visibility call naming a method the class does not
           define raises NameError when the body executes, per CRuby. */
        if (sp_streq(nm, "private") || sp_streq(nm, "protected") || sp_streq(nm, "public")) {
          int vcid = g_class_body_id >= 0 ? g_class_body_id
                   : (comp_scope_of(c, id) ? comp_scope_of(c, id)->class_id : -1);
          if (vcid >= 0) {
            int vargs = nt_ref(nt, id, "arguments");
            int van = 0;
            const int *vav = vargs >= 0 ? nt_arr(nt, vargs, "arguments", &van) : NULL;
            for (int vi = 0; vi < van; vi++) {
              const char *vaty = nt_type(nt, vav[vi]);
              const char *mn = NULL;
              if (vaty && sp_streq(vaty, "SymbolNode")) mn = nt_str(nt, vav[vi], "value");
              else if (vaty && sp_streq(vaty, "StringNode")) mn = nt_str(nt, vav[vi], "unescaped");
              if (!mn) continue;
              size_t ml = strlen(mn);
              char wb[256]; wb[0] = '\0';
              if (ml > 0 && mn[ml - 1] == '=' && ml - 1 < sizeof wb) {
                memcpy(wb, mn, ml - 1); wb[ml - 1] = '\0';
              }
              int found = comp_method_in_chain(c, vcid, mn, NULL) >= 0 ||
                          comp_reader_in_chain(c, vcid, mn, NULL) ||
                          (wb[0] && comp_writer_in_chain(c, vcid, wb, NULL));
              if (!found) {
                emit_indent(b, indent);
                buf_printf(b, "sp_raise_cls(\"NameError\", \"undefined method '%s' for class '%s'\");\n",
                           mn, class_ruby_name(c, vcid) ? class_ruby_name(c, vcid) : c->classes[vcid].name);
                return;
              }
            }
          }
        }
        return;
      }
    }
    /* A statement-position call whose block top-level-breaks needs its own
       wrapper here: the inline/iteration emitters below would splice the
       block without one, leaving the break to target a WRONG enclosing scope
       (or a bare C `break`). The wrapper emits everything through g_pre. */
    if (id != g_brk_skip_id && call_breaks(c, id)) {
      Buf pre; memset(&pre, 0, sizeof pre);
      Buf *sv_pre = g_pre; int sv_ind = g_indent;
      g_pre = &pre; g_indent = indent;
      emit_brk_wrapped_call(c, id, NULL);
      g_pre = sv_pre; g_indent = sv_ind;
      if (pre.p) buf_puts(b, pre.p);
      free(pre.p);
      return;
    }
    if (is_block_call(c, id)) { emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0, TY_VOID); return; }
    if (is_blockless_block_param_call(c, id)) {
      /* forwarded real proc: <blk>.call(args) for effect; else a dead path. */
      if (g_yield_proc_ref) emit_yield_proc_call(c, nt_ref(nt, id, "arguments"), TY_VOID, b, indent, 0);
      return;
    }
    if (emit_output_call(c, id, b, indent)) return;
    if (emit_inline_call(c, id, b, indent)) return;
    if (emit_poly_recv_block_dispatch(c, id, b, indent)) return;
    /* emit_inline_call is the inliner for a user method that yields; if it
       declined this block-driving call, no plain-call fallback is valid (a
       yielding method emits no standalone C function, so the emitted symbol
       does not exist -> invalid C). Fail loud instead of the silent miscompile
       (the #2895 diagnostic surviving through an unknown-typed root, #2948). */
    {
      const char *ycn = nt_str(nt, id, "name");
      int yrecv = nt_ref(nt, id, "receiver");
      if (ycn && yrecv >= 0 && nt_ref(nt, id, "block") >= 0) {
        TyKind yrt = comp_ntype(c, yrecv);
        if (ty_is_object(yrt)) {
          int ycid = ty_object_class(yrt);
          int ymi = ycid >= 0 ? comp_method_in_chain(c, ycid, ycn, NULL) : -1;
          if (ymi >= 0 && c->scopes[ymi].yields)
            unsupported_feature(c, id,
              "a block-driving call to a method that yields could not be inlined "
              "(a yielding method has no standalone function to call)");
        }
      }
    }
    if (emit_iteration_stmt(c, id, b, indent)) return;
    /* attr writer: obj.x = v */
    {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      size_t ln = nm ? strlen(nm) : 0;
      if (nm && recv >= 0 && ln >= 2 && nm[ln - 1] == '=') {
        TyKind rt = comp_ntype(c, recv);
        if (ty_is_object(rt)) {
          char base[256];
          if (ln - 1 < sizeof base) {
            memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
            int args = nt_ref(nt, id, "arguments");
            int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            /* attr writer -> field write, UNLESS an explicit `def x=` overrides
               it at an equal-or-more-derived class (CRuby: attr_accessor
               defines an ordinary writer method, overridable by a subclass or
               same-class `def x=`). When overridden, fall through to normal
               dispatch. The more-derived definition wins; a same-class tie
               goes to the explicit method. */
            int writer_wins =
                comp_resolve_member(c, ty_object_class(rt), base, 1, NULL, NULL) == SP_MEMBER_ATTR;
            if (writer_wins) {
              if (an >= 1) {
                int rc = ty_object_class(rt);
                char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                int defc = -1; comp_writer_in_chain(c, rc, base, &defc);
                int iv = comp_ivar_index(&c->classes[defc < 0 ? rc : defc], ivn);
                TyKind ivt = iv >= 0 ? c->classes[defc < 0 ? rc : defc].ivar_types[iv] : TY_UNKNOWN;
                emit_indent(b, indent);
                int fo = rc >= 0 && rc < c->nclasses &&
                         c->classes[rc].freeze_observed && !c->classes[rc].is_value_type;
                int tw = fo ? ++g_tmp : -1;
                if (fo) {
                  char twn[32]; snprintf(twn, sizeof twn, "_t%d", tw);
                  buf_printf(b, "{ sp_%s *_t%d = ", c->classes[rc].c_name, tw);
                  emit_expr(c, recv, b); buf_puts(b, "; ");
                  emit_frozen_obj_guard(c, rc, twn, b);
                  buf_printf(b, "_t%d->iv_%s = ", tw, iv_c(base));
                }
                else {
                  buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ")->iv_%s = ", iv_c(base));
                }
                if (ivt == TY_POLY && comp_ntype(c, argv[0]) != TY_POLY) emit_boxed(c, argv[0], b);
                /* A genuinely poly rhs narrowing into a concrete slot takes
                   the same unboxing the local-assignment path uses. The slot
                   is concrete because an --rbs signature said so while the
                   value is poly from observed dataflow, so the two only meet
                   here (#4093); without it the sp_RbVal was assigned raw. */
                else if (ivt != TY_POLY && emit_poly_rhs_coerced(c, ivt, argv[0], b)) { }
                /* A concrete slot takes the coercing emit: an unresolved rhs
                   lowers to the gate's raising sp_RbVal token, which assigned
                   raw into an object-pointer or scalar field is ill-typed C.
                   Anything that is not the token still emits raw. */
                else if (ivt != TY_POLY && ivt != TY_UNKNOWN)
                  emit_unresolved_coerced(c, argv[0], ivt, b);
                else emit_expr(c, argv[0], b);
                buf_puts(b, fo ? "; }\n" : ";\n");
                return;
              }
            }
            else if (comp_method_in_chain(c, ty_object_class(rt), nm, NULL) < 0) {
              /* writer not in chain and no explicit method: try subclass dispatch via cls_id */
              int ncand = 0;
              for (int k = 0; k < c->nclasses; k++)
                if (comp_is_writer(&c->classes[k], base)) ncand++;
              if (an >= 1 && ncand > 0) {
                TyKind at = comp_ntype(c, argv[0]);
                int tp = ++g_tmp, tval = ++g_tmp;
                emit_indent(b, indent);
                buf_printf(b, "{ sp_%s *_t%d = ", c->classes[ty_object_class(rt)].c_name, tp);
                emit_expr(c, recv, b); buf_puts(b, "; ");
                emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval);
                emit_expr(c, argv[0], b); buf_puts(b, ";");
                buf_printf(b, " switch (_t%d->cls_id) {", tp);
                char src[32]; snprintf(src, sizeof src, "_t%d", tval);
                char objp[32]; snprintf(objp, sizeof objp, "_t%d", tp);
                emit_boxed_writer_arms(c, base, nm, objp, src, at, b);
                buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", "
                              "sp_box_obj((void *)_t%d, _t%d->cls_id))); break;", nm, tp, tp);
                buf_puts(b, " } }\n");
                return;
              }
            }
          }
        }
        /* poly receiver: switch on cls_id and store into each candidate
           class's ivar, converting the rhs to that ivar's slot type. */
        else if (rt == TY_POLY) {
          char base[256];
          if (ln - 1 < sizeof base) {
            memcpy(base, nm, ln - 1); base[ln - 1] = '\0';
            int args = nt_ref(nt, id, "arguments");
            int an = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
            int ncand = 0;
            for (int k = 0; k < c->nclasses; k++)
              if (comp_is_writer(&c->classes[k], base)) ncand++;
            if (an >= 1 && ncand > 0) {
              TyKind at = comp_ntype(c, argv[0]);
              /* A nil literal RHS has void type; cache it as a boxed-nil
                 sp_RbVal rather than declaring an (illegal) `void` temp. */
              int nil_rhs = (at == TY_NIL || at == TY_VOID);
              TyKind at_eff = nil_rhs ? TY_POLY : at;
              int tv = ++g_tmp, tval = ++g_tmp;
              emit_indent(b, indent);
              buf_printf(b, "{ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
              if (nil_rhs) {
                buf_printf(b, "sp_RbVal _t%d = sp_box_nil();", tval);
              }
else {
                emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval); emit_expr(c, argv[0], b); buf_puts(b, ";");
              }
              /* A boxed NIL carries cls_id 0, which is a real user class id:
                 switching on the field alone let a nil receiver take class 0's
                 arm and write through its NULL pointer (#4048). Ask the tag,
                 as every other cls_id dispatch does. */
              buf_printf(b, " switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {", tv, tv);
              char src[32]; snprintf(src, sizeof src, "_t%d", tval);
              char objp[32]; snprintf(objp, sizeof objp, "_t%d.v.p", tv);
              emit_boxed_writer_arms(c, base, nm, objp, src, at_eff, b);
              buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", nm, tv);
              buf_puts(b, " } }\n");
              return;
            }
          }
        }
      }
    }
    /* TY_STRING .freeze as a statement: reassign lv to frozen copy */
    {
      const char *fnm = nt_str(nt, id, "name");
      int frcv = nt_ref(nt, id, "receiver");
      if (frcv >= 0 && fnm && sp_streq(fnm, "freeze") && comp_ntype(c, frcv) == TY_STRING) {
        const char *rty2 = nt_type(nt, frcv);
        if (rty2 && (sp_streq(rty2, "LocalVariableReadNode") || sp_streq(rty2, "InstanceVariableReadNode"))) {
          int fargs = nt_ref(nt, id, "arguments");
          int fac = 0; if (fargs >= 0) nt_arr(nt, fargs, "arguments", &fac);
          if (fac == 0) {
            /* the receiver must be an assignable name: a shared-buffer local
               reads back as an expression, which cannot be assigned to (#3749) */
            Buf fb; memset(&fb, 0, sizeof fb);
            emit_expr(c, frcv, &fb);
            int plain = fb.p != NULL;
            for (const char *q = fb.p; plain && *q; q++)
              if (!(*q == '_' || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                    (*q >= '0' && *q <= '9') || *q == '.' || *q == '-' || *q == '>')) plain = 0;
            if (plain) {
              emit_indent(b, indent);
              buf_puts(b, fb.p); buf_puts(b, " = sp_str_freeze_val("); buf_puts(b, fb.p); buf_puts(b, ");\n");
              free(fb.p);
              return;
            }
            free(fb.p);
            /* a shared-buffer local IS the sp_String: freeze the buffer, which
               is what every later mutation checks (#3749) */
            if (sp_streq(rty2, "LocalVariableReadNode")) {
              const char *lnm = nt_str(nt, frcv, "name");
              Scope *lsc = comp_scope_of(c, frcv);
              LocalVar *llv = (lnm && lsc) ? scope_local(lsc, lnm) : NULL;
              if (llv && llv->type == TY_STRBUF) {
                emit_indent(b, indent);
                buf_printf(b, "sp_gc_freeze((void *)lv_%s);\n", rename_local(lnm));
                return;
              }
            }
            emit_indent(b, indent);
            buf_puts(b, "(void)("); emit_expr(c, frcv, b); buf_puts(b, ");\n");
            return;
          }
        }
      }
    }
    if (emit_array_mutate_stmt(c, id, b, indent)) return;
    /* instance_eval/exec or trampoline call in statement position: its value
       is discarded, so let the splice emit the block's last node as a
       statement rather than coercing it to an expression. */
    {
      const char *snm = nt_str(nt, id, "name");
      int srecv = nt_ref(nt, id, "receiver");
      int sblk = nt_ref(nt, id, "block");
      if (snm && srecv >= 0 && sblk >= 0) {
        TyKind srt = comp_ntype(c, srecv);
        int is_ie = sp_streq(snm, "instance_eval") || sp_streq(snm, "instance_exec");
        int discard = (is_ie && ty_is_object(srt)) ||
                      (ty_is_object(srt) && comp_trampoline_kind(c, ty_object_class(srt), snm, NULL));
        if (discard) {
          int sv = g_ie_discard_value; g_ie_discard_value = 1;
          emit_indent(b, indent);
          emit_expr(c, id, b);
          buf_puts(b, ";\n");
          g_ie_discard_value = sv;
          return;
        }
      }
    }
    emit_indent(b, indent);
    emit_expr(c, id, b);
    buf_puts(b, ";\n");
    return;
  }
  if (sp_streq(ty, "LocalVariableWriteNode")) { emit_assign(c, id, b, indent); return; }
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) { emit_op_assign(c, id, b, indent); return; }
  if (sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    const char *en = rename_local(nm);
    if (t == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(lv_%s)) lv_%s = ", is_or ? "!" : "", en, en);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (t == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%slv_%s) lv_%s = ", is_or ? "!" : "", en, en);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (t == TY_SYMBOL) {
      emit_indent(b, indent);
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      buf_printf(b, "if (lv_%s %s= (sp_sym)-1) lv_%s = ", en, is_or ? "=" : "!", en);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {  /* a &&= v on an always-truthy var: always assign */
      emit_indent(b, indent);
      buf_printf(b, "lv_%s = ", en); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else {
      /* `x ||= v` on a slot that can hold nil: test it. Only a slot with no
         nil representation at all is the no-op this used to assume for every
         non-poly type. The assignment goes through emit_assign so it gets the
         same coercions the plain `x = v` form does, with its statement-shaped
         setup caught in a local buffer -- routed to g_pre it would run (and
         its side effects with it) even when the variable is already set. */
      Buf rb; memset(&rb, 0, sizeof rb); emit_local_ref(c, id, nm, &rb);
      Buf nb; memset(&nb, 0, sizeof nb);
      if (rb.p && local_nil_test(c, lv, rb.p, &nb)) {
        Buf apre, abody;
        memset(&apre, 0, sizeof apre); memset(&abody, 0, sizeof abody);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &apre; g_indent = indent + 1;
        emit_assign(c, id, &abody, indent + 1);
        g_pre = sv_pre; g_indent = sv_ind;
        emit_indent(b, indent); buf_printf(b, "if (%s) {\n", nb.p);
        if (apre.p) buf_puts(b, apre.p);
        if (abody.p) buf_puts(b, abody.p);
        emit_indent(b, indent); buf_puts(b, "}\n");
        free(apre.p); free(abody.p);
      }
      free(nb.p); free(rb.p);
    }
    return;
  }
  if (sp_streq(ty, "InstanceVariableOrWriteNode") || sp_streq(ty, "InstanceVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws2 = comp_scope_of(c, id);
    /* Resolve the slot's storage location + type. A top-level method's ivar
       lives in the Toplevel pseudo-class global (`civ_Toplevel_x`) and a
       class/module body's in `civ_<Class>_x` -- mirror the plain-write handler,
       else a top-level `@x ||= v` renders no store and the assignment is lost. */
    int sc2 = cws2 ? cws2->class_id : -1;
    if (sc2 < 0 && g_class_body_id >= 0) sc2 = g_class_body_id;
    if (sc2 < 0) sc2 = comp_class_index(c, "Toplevel");
    TyKind ivt2 = TY_UNKNOWN;
    if (sc2 >= 0) { int iv2 = comp_ivar_index(&c->classes[sc2], nm); if (iv2 >= 0) ivt2 = c->classes[sc2].ivar_types[iv2]; }
    char ref2[300];
    if (cws2 && cws2->is_cmethod && cws2->class_id >= 0)
      snprintf(ref2, sizeof ref2, "civ_%s_%s", c->classes[cws2->class_id].name, nm + 1);
    else if (cws2 && cws2->class_id < 0 && !cws2->is_cmethod && g_ie_class_id >= 0)
      snprintf(ref2, sizeof ref2, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    else if (cws2 && cws2->class_id < 0 && !cws2->is_cmethod && g_class_body_id >= 0)
      snprintf(ref2, sizeof ref2, "civ_%s_%s", c->classes[g_class_body_id].name, nm + 1);
    else if (cws2 && cws2->class_id < 0 && !cws2->is_cmethod && sc2 >= 0)
      snprintf(ref2, sizeof ref2, "civ_Toplevel_%s", nm + 1);
    else
      snprintf(ref2, sizeof ref2, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    if (ivt2 == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(%s)) %s = ", is_or ? "!" : "", ref2, ref2);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (ivt2 == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%s%s) %s = ", is_or ? "!" : "", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (ivt2 == TY_INT) {
      emit_indent(b, indent);
      if (is_or) buf_printf(b, "if (%s == SP_INT_NIL) %s = ", ref2, ref2);
      else       buf_printf(b, "if (%s != SP_INT_NIL) %s = ", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (ivt2 == TY_SYMBOL) {
      emit_indent(b, indent);
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      if (is_or) buf_printf(b, "if (%s == (sp_sym)-1) %s = ", ref2, ref2);
      else       buf_printf(b, "if (%s != (sp_sym)-1) %s = ", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (ivt2 == TY_STRING) {
      emit_indent(b, indent);
      if (is_or) buf_printf(b, "if (!%s) %s = ", ref2, ref2);
      else       buf_printf(b, "if (%s) %s = ", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* a pointer-backed ivar (fiber/proc/object/array/hash/...) reads falsy
       when NULL, so `@x ||= v` is `if (!@x) @x = v` (e.g. PPU's
       `@fiber ||= Fiber.new { ... }`). Without this the init was dropped. */
    else if (ty_is_object(ivt2) || ty_is_array(ivt2) || ty_is_hash(ivt2) ||
             ivt2 == TY_FIBER || ivt2 == TY_THREAD || ivt2 == TY_QUEUE || ivt2 == TY_MUTEX || ivt2 == TY_CONDVAR || ivt2 == TY_PROC || ivt2 == TY_IO ||
             
             ivt2 == TY_MATCHDATA || ivt2 == TY_EXCEPTION || ivt2 == TY_REGEX) {
      emit_indent(b, indent);
      if (is_or) buf_printf(b, "if (!%s) %s = ", ref2, ref2);
      else       buf_printf(b, "if (%s) %s = ", ref2, ref2);
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {
      emit_indent(b, indent);
      buf_printf(b, "%s = ", ref2); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "CallOrWriteNode") || sp_streq(ty, "CallAndWriteNode")) {
    int is_or = sp_streq(ty, "CallOrWriteNode");
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");  /* attr/reader name */
    int v = nt_ref(nt, id, "value");
    if (recv < 0 || !attr) { unsupported(c, id, is_or ? "call-or-write" : "call-and-write"); return; }
    TyKind rt = comp_ntype(c, recv);
    if (!ty_is_object(rt)) { unsupported(c, id, is_or ? "call-or-write (non-object)" : "call-and-write (non-object)"); return; }
    int class_id = ty_object_class(rt);
    char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", attr);
    int iidx = comp_ivar_index(&c->classes[class_id], ivn);
    TyKind ivt = iidx >= 0 ? c->classes[class_id].ivar_types[iidx] : TY_UNKNOWN;
    int tr = ++g_tmp;
    emit_indent(b, indent);
    buf_puts(b, "{ ");
    emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    if (ivt == TY_POLY) {
      buf_printf(b, "if (%ssp_poly_truthy(((sp_%s *)_t%d.v.p)->iv_%s)) ((sp_%s *)_t%d.v.p)->iv_%s = ",
                 is_or ? "!" : "", c->classes[class_id].c_name, tr, attr,
                 c->classes[class_id].c_name, tr, attr);
      emit_boxed(c, v, b); buf_puts(b, "; }\n");
    }
    else if (ivt == TY_BOOL) {
      buf_printf(b, "if (%s_t%d->iv_%s) _t%d->iv_%s = ", is_or ? "!" : "", tr, iv_c(attr), tr, iv_c(attr));
      emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (ivt == TY_INT) {
      /* nullable-int slot: nil is SP_INT_NIL (false does not inhabit an int
         slot) -- ||= assigns exactly when nil, &&= exactly when not. */
      buf_printf(b, "if (_t%d->iv_%s %s SP_INT_NIL) _t%d->iv_%s = ",
                 tr, attr, is_or ? "==" : "!=", tr, attr);
      emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (!is_or) {  /* &&= on always-truthy type: always assign */
      buf_printf(b, "_t%d->iv_%s = ", tr, iv_c(attr)); emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else { buf_puts(b, "}\n"); }  /* ||= on always-truthy type: no-op, but receiver evaluated */
    return;
  }
  if (sp_streq(ty, "InstanceVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    /* `@a = @b = nil`: emit the inner writes as their own statements (each
       target renders nil for its own slot type), then write nil here too. */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) { emit_stmt_inner(c, v, b, indent); v = ncb; }
    }
    Scope *cws = comp_scope_of(c, id);
    /* Ivar write inside instance_eval block: access ivar via receiver pointer. */
    if (cws && cws->class_id < 0 && !cws->is_cmethod && g_ie_class_id >= 0) {
      emit_indent(b, indent);
      buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(nm + 1));
    }
    /* Ivar write in a class/module body (outside any def): write to the
       module-level civ_ variable. */
    else if (cws && cws->class_id < 0 && !cws->is_cmethod && g_class_body_id >= 0) {
      emit_indent(b, indent);
      buf_printf(b, "civ_%s_%s = ", c->classes[g_class_body_id].name, nm + 1);
    }
    /* Top-level method (class_id<0, not cmethod): use Toplevel pseudo-class global. */
    else if (cws && cws->class_id < 0 && !cws->is_cmethod &&
             comp_class_index(c, "Toplevel") >= 0) {
      emit_indent(b, indent);
      buf_printf(b, "civ_Toplevel_%s = ", nm + 1);
    }
    /* True top-level or no-class scope: skip. */
    else if (!cws || (cws->class_id < 0 && !cws->is_cmethod)) { return; }
    else {
      emit_indent(b, indent);
      if (cws && cws->is_cmethod && cws->class_id >= 0)
        buf_printf(b, "civ_%s_%s = ", c->classes[cws->class_id].name, nm + 1);
      else {
        if (cws && cws->class_id >= 0)
          emit_frozen_obj_guard(c, cws->class_id, g_self ? g_self : "self", b);
        buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(nm + 1));
      }
    }
    const char *vty = nt_type(nt, v);
    int sc = cws ? cws->class_id : -1;
    if (sc < 0 && g_class_body_id >= 0) sc = g_class_body_id;
    if (sc < 0) sc = comp_class_index(c, "Toplevel");
    TyKind ivt = TY_INT;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) ivt = c->classes[sc].ivar_types[iv]; }
    int ven = 0;
    int v_empty_array = vty && sp_streq(vty, "ArrayNode") && (nt_arr(nt, v, "elements", &ven), ven == 0);
    int v_empty_hash = 0;
    if (!v_empty_array && vty) {
      int hen = 0;
      if (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode"))
        v_empty_hash = (nt_arr(nt, v, "elements", &hen), hen == 0);
    }
    /* `@t = Array.new(n) { <int array> }` into a narrowed pointer-array ivar:
       the generator emits from the NODE's type, which the narrowing (a slot
       decision) does not change. Lend it the slot's type, exactly as the local
       write does, or the sp_PolyArray * it builds lands in an sp_PtrArray *
       field and every read of the table dereferences the wrong shape. */
    if (ty_is_ptr_array(ivt) && vty && sp_streq(vty, "CallNode") &&
        nt_ref(nt, v, "block") >= 0 && nt_str(nt, v, "name") &&
        sp_streq(nt_str(nt, v, "name"), "new") && nt_ref(nt, v, "receiver") >= 0 &&
        nt_type(nt, nt_ref(nt, v, "receiver")) &&
        sp_streq(nt_type(nt, nt_ref(nt, v, "receiver")), "ConstantReadNode") &&
        nt_str(nt, nt_ref(nt, v, "receiver"), "name") &&
        sp_streq(nt_str(nt, nt_ref(nt, v, "receiver"), "name"), "Array")) {
      TyKind sv = c->ntype[v];
      c->ntype[v] = ivt;
      emit_expr(c, v, b);
      c->ntype[v] = sv;
    }
    else if (ty_is_ptr_array(ivt) && v_empty_array) buf_puts(b, "sp_PtrArray_new()");
    /* `@t = [[..], [..]]` into a narrowed pointer-array ivar: build the
       sp_PtrArray with the unboxed element pointers, as the local write does. */
    else if (ty_is_ptr_array(ivt) && vty && sp_streq(vty, "ArrayNode")) {
      int tpa = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);", tpa, tpa);
      int pen = 0; const int *pel = nt_arr(nt, v, "elements", &pen);
      for (int e = 0; e < pen; e++) {
        buf_printf(b, " sp_PtrArray_push(_t%d, ", tpa); emit_expr(c, pel[e], b); buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tpa);
    }
    else if (ivt == TY_INT && emit_nullable_int_ternary(c, v, b)) {
      /* `@iv = cond ? nil : <int>` emitted in int context (nil -> SP_INT_NIL) */
    }
    else if (vty && sp_streq(vty, "NilNode")) {
      if (ivt == TY_RANGE) buf_puts(b, "(sp_Range){0}");
      else if (ivt == TY_POLY) buf_puts(b, "sp_box_nil()");
      else if (ivt == TY_INT) buf_puts(b, "SP_INT_NIL");
      else if (ivt == TY_FLOAT) buf_puts(b, "sp_float_nil()");
      else if (ivt == TY_STRING) buf_puts(b, "NULL");
      else buf_puts(b, default_value(ivt));
    }
    else if (v_empty_array && ivt == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array && array_kind(ivt)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt));
    else if (v_empty_hash && ty_is_hash(ivt)) {
      const char *hcn = ty_hash_cname(ivt);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt == TY_STRBUF) {
      /* shared handle slot: an alias RHS (a shared local/ivar read) copies
         the handle; anything else wraps a fresh handle, inheriting the
         source's frozen state (#3227 P4) */
      char srefW[1024];
      if (vty && sp_streq(vty, "NilNode")) buf_puts(b, "NULL");
      else if (strbuf_slot_ref(c, v, srefW, sizeof srefW)) buf_puts(b, srefW);
      else {
        buf_puts(b, "sp_String_new_shared(");
        emit_str_expr(c, v, b);
        buf_puts(b, ")");
      }
    }
    else if (ivt == TY_POLY && comp_ntype(c, v) != TY_POLY) {
      /* a poly ivar slot needs a boxed RHS */
      emit_boxed(c, v, b);
    }
    /* An int RHS into a bigint ivar is promoted at the boundary, the same way
       the local assignment and the argument binding do it. Without it `@n = 0`
       on an ivar that elsewhere sees a Bignum wrote the integer 0 into an
       sp_Bigint * slot -- a null pointer constant, so the C compiled clean and
       the first sp_bigint_add on it segfaulted (#3399 sibling; found while
       fixing the argument side). */
    else if (ivt == TY_BIGINT && comp_ntype(c, v) != TY_BIGINT &&
             ty_is_numeric(comp_ntype(c, v))) {
      buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, v, b); buf_puts(b, ")");
    }
    else if (ivt != TY_POLY && ivt != TY_UNKNOWN && comp_ntype(c, v) == TY_POLY) {
      /* poly rhs assigned to a typed ivar: unbox to the concrete type. The
         nil-preserving form -- the RHS is a tagged union whose nil-ness is not
         ruled out here, and an --rbs `Integer?` / `Float?` pin makes this
         exactly the slot a nil is expected to survive in (#3412). */
      Buf _rb; memset(&_rb, 0, sizeof _rb);
      emit_expr(c, v, &_rb);
      /* A seeded slot is the only place a narrowing can be WRONG rather than
         merely lossy, so that is where the assertion goes (#3412). */
      Buf _ck; memset(&_ck, 0, sizeof _ck);
      if (sc >= 0 && class_ivar_pinned(&c->classes[sc], nm))
        emit_rbs_checked_text(c, ivt, nm, _rb.p ? _rb.p : "sp_box_nil()", &_ck);
      else buf_puts(&_ck, _rb.p ? _rb.p : "sp_box_nil()");
      emit_unbox_nilable_text(c, ivt, _ck.p ? _ck.p : "sp_box_nil()", b);
      free(_ck.p);
      free(_rb.p);
    }
    else if (ivt != TY_POLY && ivt != TY_UNKNOWN && comp_ntype(c, v) == TY_UNKNOWN) {
      /* an unresolved call typed TY_UNKNOWN whose value is the gate's
         sp_raise_nomethod(...) poly token, assigned to a typed ivar slot
         (`@settings = TypedStore.write(...)` where write is unresolved):
         coerce the token to the slot type, keeping the raise. */
      emit_unresolved_coerced(c, v, ivt, b);
    }
    else {
      /* a subclass instance stored into an ancestor-typed ivar slot (#3418) */
      emit_obj_upcast_prefix(c, ivt, comp_ntype(c, v), b);
      emit_expr(c, v, b);
    }
    buf_puts(b, ";\n");
    return;
  }
  if (sp_streq(ty, "ClassVariableWriteNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@@x" */
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) sc = comp_class_index(c, "Toplevel");
    if (sc < 0) { unsupported(c, id, "class variable write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[sc], nm);
    if (idx >= 0) ct = c->classes[sc].cvar_types[idx];
    emit_indent(b, indent);
    buf_printf(b, "cvar_%s_%s = ", c->classes[sc].name, nm + 2);
    if (emit_empty_container_for_slot(c, v, ct, b)) { /* emitted at the slot's type */ }
    else if (ct == TY_POLY) emit_boxed(c, v, b);
    /* an int into a bigint slot promotes at the boundary, as everywhere else */
    else if (ct == TY_BIGINT && comp_ntype(c, v) != TY_BIGINT) emit_bigint_operand_ext(c, v, b);
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (sp_streq(ty, "ClassVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) sc = comp_class_index(c, "Toplevel");
    if (sc < 0) { unsupported(c, id, "class variable op-write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[sc], nm);
    if (idx >= 0) ct = c->classes[sc].cvar_types[idx];
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    if (ct == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else if (ct == TY_POLY) {
      /* a widened cvar op-assign routes through the tag-dispatching sp_poly_<op>
         (mirrors the local op-assign poly arm). */
      const char *pfn = sp_streq(op ? op : "+", "+") ? "sp_poly_add"
                      : sp_streq(op, "-") ? "sp_poly_sub"
                      : sp_streq(op, "*") ? "sp_poly_mul"
                      : sp_streq(op, "/") ? "sp_poly_div" : NULL;
      int bitop = op && (sp_streq(op, "<<") || sp_streq(op, ">>") || sp_streq(op, "&") ||
                         sp_streq(op, "|") || sp_streq(op, "^") || sp_streq(op, "%"));
      if (pfn) { buf_printf(b, "%s = %s(%s, ", ref, pfn, ref); emit_boxed(c, v, b); buf_puts(b, ");\n"); }
      else if (bitop) { buf_printf(b, "%s = sp_box_int(sp_poly_to_i(%s) %s ", ref, ref, op); emit_int_expr(c, v, b); buf_puts(b, ");\n"); }
      else { buf_printf(b, "%s %s= ", ref, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ";\n"); }
    }
    else {
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "ClassVariableOrWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) sc = comp_class_index(c, "Toplevel");
    if (sc < 0) { unsupported(c, id, "class variable or-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    buf_printf(b, "if (!(%s)) { %s = ", ref, ref); emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (sp_streq(ty, "ClassVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    int sc = comp_scope_of(c, id)->class_id;
    if (sc < 0) sc = g_class_body_id;
    if (sc < 0) sc = comp_class_index(c, "Toplevel");
    if (sc < 0) { unsupported(c, id, "class variable and-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[sc].name, nm + 2);
    emit_indent(b, indent);
    buf_printf(b, "if (%s) { %s = ", ref, ref); emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int sc = comp_scope_of(c, id)->class_id;
    /* same scope ladder as the plain-write form: class body civ_, then the
       Toplevel pseudo-class global (a bare `self` here is undeclared C) */
    if (sc < 0 && g_class_body_id >= 0) sc = g_class_body_id;
    if (sc < 0 && g_ie_class_id < 0) sc = comp_class_index(c, "Toplevel");
    TyKind vt = TY_UNKNOWN;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);
    else if (cs && cs->class_id < 0 && !cs->is_cmethod && g_ie_class_id < 0 &&
             g_class_body_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[g_class_body_id].name, nm + 1);
    else if (cs && cs->class_id < 0 && !cs->is_cmethod && g_ie_class_id < 0 &&
             comp_class_index(c, "Toplevel") >= 0)
      snprintf(ref, sizeof ref, "civ_Toplevel_%s", nm + 1);
    else
      snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    emit_indent(b, indent);
    /* An op-assign is a write, so a frozen receiver refuses it the way a plain
       `@x = v` does: `c.freeze; c.bump` was changing the ivar (#3736). */
    if (cs && cs->class_id >= 0 && !cs->is_cmethod &&
        strncmp(ref, "civ_", 4) != 0)
      emit_frozen_obj_guard(c, cs->class_id, g_self ? g_self : "self", b);
    /* A bigint ivar has no C operator: `@n += 1` emitted pointer arithmetic on
       an incomplete struct. Route it through the bigint helpers, promoting an
       int operand at the boundary like every other bigint slot. */
    if (vt == TY_BIGINT && op && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*"))) {
      const char *fn = sp_streq(op, "+") ? "sp_bigint_add"
                     : sp_streq(op, "-") ? "sp_bigint_sub" : "sp_bigint_mul";
      buf_printf(b, "%s = %s(%s, ", ref, fn, ref);
      emit_bigint_operand_ext(c, nt_ref(nt, id, "value"), b);
      buf_puts(b, ");\n");
    }
    else if (vt == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ");\n");
    }
    else if (op && sp_streq(op, "+") && ty_is_array(vt)) {
      /* `@arr += other` = `@arr = @arr + other` (#3289), mirroring the
         local-variable arm: same-kind concat, empty-literal rhs is a no-op
         concat with NULL */
      int ival2 = nt_ref(nt, id, "value");
      TyKind rvt2 = comp_ntype(c, ival2);
      const char *k2 = (vt == TY_POLY_ARRAY) ? "Poly" : array_kind(vt);
      int rhs_empty2 = 0;
      { const char *vty2 = nt_type(nt, ival2);
        if (vty2 && sp_streq(vty2, "ArrayNode")) {
          int nel2 = 0; nt_arr(nt, ival2, "elements", &nel2);
          rhs_empty2 = (nel2 == 0);
        } }
      if (k2 && (rvt2 == vt || rhs_empty2)) {
        buf_printf(b, "%s = sp_%sArray_concat(%s, ", ref, k2, ref);
        if (rhs_empty2) buf_puts(b, "NULL");
        else emit_expr(c, ival2, b);
        buf_puts(b, ");\n");
      }
      else if (vt == TY_POLY_ARRAY) {
        buf_printf(b, "%s = sp_poly_to_poly_array(sp_poly_add(sp_box_poly_array(%s), ",
                   ref, ref);
        emit_boxed(c, ival2, b);
        buf_puts(b, "));\n");
      }
      else {
        unsupported(c, id, "ivar += with a mismatched array kind");
      }
    }
    else if (op && ty_is_object(vt)) {
      int idefcls = -1;
      int icid = ty_object_class(vt);
      int imi = comp_method_in_chain(c, icid, op, &idefcls);
      if (imi >= 0) {
        Scope *ims = &c->scopes[imi];
        LocalVar *ip = ims->nparams >= 1 ? scope_local(ims, ims->pnames[0]) : NULL;
        int iatmp = ++g_tmp;
        int ival = nt_ref(nt, id, "value");
        TyKind ipt = ip ? ip->type : comp_ntype(c, ival);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, ipt, g_pre);
        buf_printf(g_pre, " _t%d = ", iatmp);
        /* box the rhs when the operator's param widened to poly (promote mode) */
        if (ipt == TY_POLY && comp_ntype(c, ival) != TY_POLY) emit_boxed(c, ival, g_pre);
        else emit_expr(c, ival, g_pre);
        buf_puts(g_pre, ";\n");
        buf_printf(b, "%s = sp_%s_%s((sp_%s *)%s, _t%d);\n",
                   ref, c->classes[idefcls].c_name, mc(ims->name),
                   c->classes[idefcls].c_name, ref, iatmp);
      }
      else {
        buf_printf(b, "%s %s= ", ref, op);
        emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ";\n");
      }
    }
    else if (op && vt == TY_POLY) {
      /* @ivar OP= rhs where ivar is poly (e.g. nil | user_object). Scan for a
         unique user class defining OP and dispatch through .v.p cast. */
      int poly_defcls = -1, poly_mi = -1;
      for (int _ci = 0; _ci < c->nclasses; _ci++) {
        int _di = -1;
        int _mi2 = comp_method_in_chain(c, _ci, op, &_di);
        if (_mi2 >= 0) { poly_mi = _mi2; poly_defcls = _di; break; }
      }
      if (poly_mi >= 0 && poly_defcls >= 0) {
        int ival = nt_ref(nt, id, "value");
        TyKind rhst = comp_ntype(c, ival);
        Scope *pms = &c->scopes[poly_mi];
        LocalVar *pp = pms->nparams >= 1 ? scope_local(pms, pms->pnames[0]) : NULL;
        TyKind paramt = (pp && pp->type != TY_UNKNOWN) ? pp->type : rhst;
        if (paramt == TY_UNKNOWN) paramt = TY_INT;
        int iatmp = ++g_tmp;
        emit_indent(g_pre, g_indent);
        emit_ctype(c, paramt, g_pre);
        buf_printf(g_pre, " _t%d = ", iatmp);
        /* the temp is declared with the OPERATOR's parameter type, so the
           call-site value has to be converted into it (a raw sp_int landed
           in an sp_RbVal slot, #3733) */
        if (paramt == TY_POLY && rhst != TY_POLY) emit_boxed(c, ival, g_pre);
        else if (paramt != TY_POLY && rhst == TY_POLY) {
          Buf rvb; memset(&rvb, 0, sizeof rvb); emit_expr(c, ival, &rvb);
          emit_unbox_text(c, paramt, rvb.p ? rvb.p : "sp_box_nil()", g_pre);
          free(rvb.p);
        }
        else emit_expr(c, ival, g_pre);
        buf_puts(g_pre, ";\n");
        /* The operator method can `return nil` (a NULL reference); box a
           reference-type result via sp_box_nullable_obj so NULL becomes nil, not
           a truthy wrapper. A value-type class is never NULL, so keep sp_box_obj. */
        const char *pbox = c->classes[poly_defcls].is_value_type
                             ? "sp_box_obj(" : "sp_box_nullable_obj((void *)(";
        const char *pboxc = c->classes[poly_defcls].is_value_type ? ", %d)" : "), %d)";
        /* The slot is POLY: the user operator is only the right answer when the
           value really is an instance of that class. An Integer in the same
           slot takes the numeric path, as it does everywhere else a poly
           receiver dispatches to a user arm (#3733). */
        const char *pnum = sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub"
                         : sp_streq(op, "*") ? "sp_poly_mul" : sp_streq(op, "/") ? "sp_poly_div"
                         : sp_streq(op, "%") ? "sp_poly_mod" : NULL;
        if (pnum) buf_printf(b, "%s = ((%s).tag == SP_TAG_OBJ && (%s).cls_id == %d) ? ",
                             ref, ref, ref, poly_defcls);
        else buf_printf(b, "%s = ", ref);
        buf_printf(b, "%ssp_%s_%s((sp_%s *)(%s).v.p, _t%d)", pbox,
                   c->classes[poly_defcls].c_name, mc(pms->name),
                   c->classes[poly_defcls].c_name, ref, iatmp);
        buf_printf(b, pboxc, poly_defcls);
        if (pnum) {
          buf_printf(b, " : %s(%s, ", pnum, ref);
          emit_boxed(c, ival, b);
          buf_puts(b, ")");
        }
        buf_puts(b, ";\n");
      }
      else if ((sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") || sp_streq(op, "/"))) {
        /* numeric op on a poly slot: runtime poly arithmetic with a boxed rhs */
        const char *pfn = sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub"
                        : sp_streq(op, "*") ? "sp_poly_mul" : "sp_poly_div";
        buf_printf(b, "%s = %s(%s, ", ref, pfn, ref);
        emit_boxed(c, nt_ref(nt, id, "value"), b);
        buf_puts(b, ");\n");
      }
      else if (sp_streq(op, "<<") || sp_streq(op, ">>") ||
               sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^")) {
        /* bitwise op-assign on a poly slot: coerce to int, re-box the result
           (same shape as the local poly-bitwise path). */
        int ival = nt_ref(nt, id, "value");
        TyKind rhst = comp_ntype(c, ival);
        buf_printf(b, "%s = sp_box_int((sp_poly_to_i(%s) %s (", ref, ref, op);
        if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, ival, b); buf_puts(b, ")"); }
        else emit_expr(c, ival, b);
        buf_puts(b, ")));\n");
      }
      else {
        buf_printf(b, "%s %s= ", ref, op);
        emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ";\n");
      }
    }
    else {
      int ival = nt_ref(nt, id, "value");
      TyKind rhst = comp_ntype(c, ival);
      /* An int ivar op-assign takes the overflow-checked helpers like the
         binary form (raw `@x *= y` wrapped where `@x * y` raised). Bitwise
         ops keep the raw C operator below. */
      if (vt == TY_INT && op && int_arith_fn(op)) {
        int ivdm = sp_streq(op, "/") || sp_streq(op, "%");
        buf_printf(b, "%s = %s(%s, ", ref, int_arith_fn(op), ref);
        if (ivdm) emit_int_divisor(c, ival, b);
        else if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, ival, b); buf_puts(b, ")"); }
        else emit_expr(c, ival, b);
        buf_puts(b, ");\n");
        return;
      }
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      /* a poly RHS feeding an int/float ivar op-assign needs coercing to the
         scalar before the C operator (e.g. `@bg_pattern |= chr_mem[i] * 256`). */
      if (rhst == TY_POLY && (vt == TY_INT || vt == TY_BOOL)) {
        buf_puts(b, "sp_poly_to_i("); emit_expr(c, ival, b); buf_puts(b, ")");
      }
      else if (rhst == TY_POLY && vt == TY_FLOAT) {
        buf_puts(b, "sp_poly_to_f("); emit_expr(c, ival, b); buf_puts(b, ")");
      }
      else emit_expr(c, ival, b);
      buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "CallOperatorWriteNode")) {
    /* `recv.attr op= value` (e.g. doom's `sector.ceiling_height -=
       speed`) has neither a statement-level handler here nor an
       expression-level one, so it falls all the way through emit_expr's
       final "unsupported expression" catch-all. Handles the common
       case: an object receiver with a plain attr_reader/Struct-member
       attribute. `recv` is evaluated into a temp exactly once (it may
       be a hash lookup or other expression with real work behind it,
       not just a bare local) and both the read and the write go through
       that same temp. */
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int val = nt_ref(nt, id, "value");
    TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    TyKind rhst = val >= 0 ? comp_ntype(c, val) : TY_UNKNOWN;
    /* the dynamic operator for a boxed (poly) slot, and the bitwise set the
       boxed slot handles via unbox-op-rebox (both mirror the ivar op-assign
       poly arms above). */
    const char *cpf = op && sp_streq(op, "+") ? "sp_poly_add"
                    : op && sp_streq(op, "-") ? "sp_poly_sub"
                    : op && sp_streq(op, "*") ? "sp_poly_mul"
                    : op && sp_streq(op, "/") ? "sp_poly_div"
                    : op && sp_streq(op, "%") ? "sp_poly_mod" : NULL;
    int bitop = op && (sp_streq(op, "<<") || sp_streq(op, ">>") ||
                       sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^"));
    int rdcls = -1;
    /* Ruby desugars `recv.attr op= v` into a reader call AND a writer call;
       an attr_reader-only attribute raises NoMethodError for `attr=`, so a
       reader-only match must not silently lower into an ivar store. */
    if (recv >= 0 && attr && ty_is_object(rt) &&
        comp_reader_in_chain(c, ty_object_class(rt), attr, &rdcls) &&
        comp_writer_in_chain(c, ty_object_class(rt), attr, NULL)) {
      const char *rn = comp_resolve_alias(c, rdcls, attr);
      char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", rn);
      int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
      /* no resolvable backing slot: fail the lowering rather than guess */
      if (ivx < 0) unsupported(c, id, "call operator write (reader without an ivar slot)");
      TyKind ivt = c->classes[rdcls].ivar_types[ivx];
      /* operators the slot type can't take would otherwise fall through to
         raw C on an sp_Str pointer or an sp_RbVal (pointer arithmetic / a
         type error) */
      if (ivt == TY_STRING && !(op && sp_streq(op, "+")))
        unsupported(c, id, "call operator write (operator on a string attribute)");
      if (ivt == TY_POLY && !cpf && !bitop)
        unsupported(c, id, "call operator write (operator on a boxed attribute)");
      int trecv = ++g_tmp;
      emit_indent(b, indent);
      emit_ctype(c, rt, b);
      buf_printf(b, " _t%d = ", trecv); emit_expr(c, recv, b); buf_puts(b, ";\n");
      const char *acc = comp_ty_value_obj(c, rt) ? "." : "->";
      emit_indent(b, indent);
      if (ivt == TY_STRING) {
        buf_printf(b, "_t%d%siv_%s = sp_str_concat(_t%d%siv_%s, ", trecv, acc, rn, trecv, acc, rn);
        if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, val, b); buf_puts(b, ")"); }
        else emit_expr(c, val, b);
        buf_puts(b, ");\n");
      }
      else if (ivt == TY_POLY && cpf) {
        /* boxed slot: dynamic operator on boxed operands (same as the
           poly-receiver dispatch arms below). */
        buf_printf(b, "_t%d%siv_%s = %s(_t%d%siv_%s, ", trecv, acc, rn, cpf, trecv, acc, rn);
        emit_boxed(c, val, b); buf_puts(b, ");\n");
      }
      else if (ivt == TY_POLY) {
        /* bitwise op-assign on a boxed slot: coerce to int, re-box */
        buf_printf(b, "_t%d%siv_%s = sp_box_int((sp_poly_to_i(_t%d%siv_%s) %s (",
                   trecv, acc, rn, trecv, acc, rn, op);
        if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
        else emit_expr(c, val, b);
        buf_puts(b, ")));\n");
      }
      else {
        buf_printf(b, "_t%d%siv_%s = _t%d%siv_%s %s ", trecv, acc, rn, trecv, acc, rn, op ? op : "+");
        if (rhst == TY_POLY && (ivt == TY_INT || ivt == TY_BOOL)) {
          buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")");
        }
        else if (rhst == TY_POLY && ivt == TY_FLOAT) {
          buf_puts(b, "sp_poly_to_f("); emit_expr(c, val, b); buf_puts(b, ")");
        }
        else emit_expr(c, val, b);
        buf_puts(b, ";\n");
      }
      return;
    }
    /* A native (C-backed) class has no ivar behind its attribute: the read and
       the write are the C symbols its own `native_method` declarations name.
       `ctx.options |= FLAG` is the shape this serves, and without it the whole
       op-assign was refused however the setter itself lowered. */
    if (recv >= 0 && attr && op && ty_is_object(rt) &&
        c->classes[ty_object_class(rt)].is_native_class) {
      int ncid = ty_object_class(rt);
      char nsetter[300]; snprintf(nsetter, sizeof nsetter, "%s=", attr);
      int rdm = comp_native_method_find(c, ncid, attr, 0, 0);
      int wrm = comp_native_method_find(c, ncid, nsetter, 1, 0);
      if (rdm >= 0 && wrm >= 0) {
        NativeMethod *nrm = &c->native_methods[rdm];
        NativeMethod *nwm = &c->native_methods[wrm];
        int nint = sp_streq(nrm->ret, "int") && nwm->nargs == 1 && sp_streq(nwm->args[0], "int");
        int nflt = sp_streq(nrm->ret, "float") && nwm->nargs == 1 && sp_streq(nwm->args[0], "float");
        int nstr = sp_streq(nrm->ret, "string") && nwm->nargs == 1 && sp_streq(nwm->args[0], "string");
        int nany = sp_streq(nrm->ret, "any") && nwm->nargs == 1 && sp_streq(nwm->args[0], "any");
        /* the pair has to name a representation the operator exists for: a
           nullable string return, a `self` return and the container specs
           would put the C operator on a pointer that cannot take one */
        if (!nint && !nflt && !nstr && !nany)
          unsupported(c, id, "call operator write (native attribute the operator has no form for)");
        if (nflt && bitop)
          unsupported(c, id, "call operator write (bitwise operator on a float attribute)");
        if (nstr && !(op && sp_streq(op, "+")))
          unsupported(c, id, "call operator write (operator other than + on a string attribute)");
        if (nany && !cpf && !bitop)
          unsupported(c, id, "call operator write (operator on a boxed attribute)");
        int trecv = ++g_tmp;
        emit_indent(b, indent);
        emit_ctype(c, rt, b);
        buf_printf(b, " _t%d = ", trecv); emit_expr(c, recv, b); buf_puts(b, ";\n");
        /* the temp can be the receiver's only live reference and the value
           expression may allocate */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", trecv);
        emit_indent(b, indent);
        if (nstr) {
          /* the reader answers a value, so the concat builds a new string and
             the writer stores it -- the same shape the string ivar slot takes */
          buf_printf(b, "%s(_t%d, sp_str_concat(%s(_t%d), ", nwm->csym, trecv, nrm->csym, trecv);
          if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else emit_expr(c, val, b);
          buf_puts(b, "));\n");
        }
        else if (nany && cpf) {
          buf_printf(b, "%s(_t%d, %s(%s(_t%d), ", nwm->csym, trecv, cpf, nrm->csym, trecv);
          emit_boxed(c, val, b); buf_puts(b, "));\n");
        }
        else if (nany) {
          /* bitwise on a boxed attribute: coerce to int, re-box */
          buf_printf(b, "%s(_t%d, sp_box_int(sp_poly_to_i(%s(_t%d)) %s (",
                     nwm->csym, trecv, nrm->csym, trecv, op);
          if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else emit_expr(c, val, b);
          buf_puts(b, ")));\n");
        }
        else {
          buf_printf(b, "%s(_t%d, %s(_t%d) %s ", nwm->csym, trecv, nrm->csym, trecv, op);
          if (rhst == TY_POLY) {
            buf_puts(b, nint ? "sp_poly_to_i(" : "sp_poly_to_f(");
            emit_expr(c, val, b); buf_puts(b, ")");
          }
          else emit_expr(c, val, b);
          buf_puts(b, ");\n");
        }
        return;
      }
    }
    if (recv >= 0 && attr && rt == TY_POLY) {
      /* poly receiver (e.g. a Hash value that was never narrowed to a
         concrete object type -- doom's `door[:sector]`, where `door`
         is itself one Hash's poly-boxed value pulled from another).
         Dispatch on cls_id: emit a case arm for every instantiated
         class exposing a reader named `attr`, reading/writing that
         class's ivar directly through the cast pointer. */
      int trecv = ++g_tmp;
      emit_indent(b, indent);
      buf_puts(b, "sp_RbVal ");
      buf_printf(b, "_t%d = ", trecv); emit_expr(c, recv, b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      /* The temp can be the receiver's only live reference, and a write arm's
         value expression may allocate: root it for the switch (#3476). */
      buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv);
      emit_indent(b, indent);
      /* Every boxed scalar carries cls_id 0, which aliases the user class at
         index 0 (cf. issue #1576 and emit_poly_dispatch_key): key a non-object
         value to a sentinel matching no case so it lands in the default raise
         rather than dereferencing v.p through a wrong cast. */
      buf_printf(b, "switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {\n", trecv, trecv);
      int any = 0;
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].instantiated) continue;
        int pdcls = -1;
        /* both accessors, same as the concrete arm: a reader-only class must
           raise NoMethodError for `attr=` (via the default arm), not store */
        if (!comp_reader_in_chain(c, k, attr, &pdcls)) continue;
        if (!comp_writer_in_chain(c, k, attr, NULL)) continue;
        const char *rn = comp_resolve_alias(c, pdcls, attr);
        char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", rn);
        int ivx = comp_ivar_index(&c->classes[pdcls], ivn);
        if (ivx < 0) continue;  /* no resolvable backing slot: don't guess */
        TyKind ivt = c->classes[pdcls].ivar_types[ivx];
        /* skip a class whose slot can't take this operator (the raw C
           fallthrough would be pointer arithmetic on an sp_Str* or a type
           error on an sp_RbVal); a runtime object of that class lands in
           the default raise instead -- same skip-on-static-mismatch shape
           as the poly attr-write dispatch above. */
        if (ivt == TY_STRING && !(op && sp_streq(op, "+"))) continue;
        if (ivt == TY_POLY && !cpf && !bitop) continue;
        const char *cn = c->classes[pdcls].name;
        any = 1;
        emit_indent(b, indent + 1);
        buf_printf(b, "case %d: { sp_%s *_o = (sp_%s *)_t%d.v.p; ", k, cn, cn, trecv);
        if (ivt == TY_STRING) {
          buf_puts(b, "_o->iv_"); buf_puts(b, rn); buf_puts(b, " = sp_str_concat(_o->iv_"); buf_puts(b, rn);
          buf_puts(b, ", ");
          if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else emit_expr(c, val, b);
          buf_puts(b, "); break; }\n");
        }
        else if (ivt == TY_POLY && cpf) {
          /* the slot itself is boxed (a whole-program-widened numeric like
             Sector#ceiling_height): fold via the dynamic operator on boxed
             operands rather than raw C arithmetic on an sp_RbVal. */
          buf_puts(b, "_o->iv_"); buf_puts(b, rn);
          buf_printf(b, " = %s(_o->iv_", cpf); buf_puts(b, rn); buf_puts(b, ", ");
          emit_boxed(c, val, b);
          buf_puts(b, "); break; }\n");
        }
        else if (ivt == TY_POLY) {
          /* bitwise op-assign on a boxed slot: coerce to int, re-box */
          buf_puts(b, "_o->iv_"); buf_puts(b, rn);
          buf_printf(b, " = sp_box_int((sp_poly_to_i(_o->iv_%s) %s (", rn, op);
          if (rhst == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
          else emit_expr(c, val, b);
          buf_puts(b, "))); break; }\n");
        }
        else {
          buf_puts(b, "_o->iv_"); buf_puts(b, rn); buf_puts(b, " = _o->iv_"); buf_puts(b, rn);
          buf_printf(b, " %s ", op ? op : "+");
          if (rhst == TY_POLY && (ivt == TY_INT || ivt == TY_BOOL)) {
            buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")");
          }
          else if (rhst == TY_POLY && ivt == TY_FLOAT) {
            buf_puts(b, "sp_poly_to_f("); emit_expr(c, val, b); buf_puts(b, ")");
          }
          else emit_expr(c, val, b);
          buf_puts(b, "; break; }\n");
        }
      }
      /* a receiver whose runtime class has no such accessor pair (or a boxed
         scalar) is CRuby's NoMethodError, not a silent no-op */
      emit_indent(b, indent + 1);
      buf_printf(b, "default: sp_raise_poly_nomethod(\"%s=\", _t%d);\n", attr, trecv);
      emit_indent(b, indent);
      buf_puts(b, "}\n");
      if (any) return;
    }
    unsupported(c, id, "call operator write (unsupported receiver/attr)");
  }
  if (sp_streq(ty, "GlobalVariableWriteNode") || sp_streq(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int isg = ty[0] == 'G';
    const char *pfx = isg ? "gv" : "cst";
    const char *raw_key = isg ? nm + 1 : nm;
    const char *key = isg ? comp_resolve_gvar(c, raw_key) : raw_key;
    LocalVar *lv = isg ? comp_gvar(c, key) : comp_const(c, key);
    int v = nt_ref(nt, id, "value");
    if (!lv || (!isg && lv->type == TY_UNKNOWN)) {
      /* The struct-def constant itself (`Foo = Struct.new(...) do ... end`) has
         no C runtime value and is skipped -- but any top-level constant writes
         INSIDE that block still need init code emitted. register_structs only
         consumes the block for class/ivar/method registration, never as
         executable statements, so without this such constants stay NULL/default
         despite being declared and typed (a method reading them, e.g. a Linedef
         FLAGS[:TWOSIDED] flag helper, silently sees an empty constant). Mirrors
         fix_struct_block_scopes, which does the analogous fixup for DefNodes. */
      if (!isg && v >= 0 && is_struct_call(c, v)) {
        int blk = nt_ref(nt, v, "block");
        int bbody = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
        if (bbody >= 0) {
          int bn = 0;
          const int *stmts = nt_arr(nt, bbody, "body", &bn);
          for (int k = 0; k < bn; k++) {
            const char *sty = nt_type(nt, stmts[k]);
            if (sty && sp_streq(sty, "ConstantWriteNode"))
              emit_stmt(c, stmts[k], b, indent);
          }
        }
      }
      return;
    }
    if (!isg && lv->init_guarded) {
      /* flag the const as in-progress while its Class.new runs, so a
         self-referential read inside initialize raises NameError */
      emit_indent(b, indent); buf_printf(b, "sp_init_in_progress_%s = 1;\n", key);
    }
    emit_indent(b, indent);
    buf_printf(b, "%s_%s = ", pfx, key);
    int vlit = empty_literal_node(c, v);
    /* `X = [].freeze`: the strip above found the literal so the slot's kind
       builds it; the freeze it stripped still has to happen (#3828). */
    int v_lit_frozen = (vlit != v);
    const char *vty = nt_type(nt, vlit);
    int v_empty_arr = 0, v_empty_hash = 0;
    if (vty && sp_streq(vty, "ArrayNode")) {
      int ac = 0; nt_arr(nt, vlit, "elements", &ac); v_empty_arr = (ac == 0);
    }
    if (vty && (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode"))) {
      int hec = 0; nt_arr(nt, vlit, "elements", &hec); v_empty_hash = (hec == 0);
    }
    /* `$g = Hash.new` (no args/block): an empty hash whose typed slot needs a
       fresh `sp_XHash_new()`, not the boxed sp_RbVal emit_expr would produce for
       the untyped Hash.new call (#3205). */
    if (vty && sp_streq(vty, "CallNode") && ty_is_hash(lv->type) &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "new") &&
        nt_ref(nt, v, "block") < 0) {
      int hr = nt_ref(nt, v, "receiver");
      const char *hrn = hr >= 0 && nt_type(nt, hr) && sp_streq(nt_type(nt, hr), "ConstantReadNode")
                        ? nt_str(nt, hr, "name") : NULL;
      int ha = nt_ref(nt, v, "arguments"); int han = 0;
      if (ha >= 0) nt_arr(nt, v, "arguments", &han);
      if (hrn && sp_streq(hrn, "Hash") && han == 0) v_empty_hash = 1;
    }
    if (vty && sp_streq(vty, "NilNode"))
      /* A nil string is NULL, the way an ivar's is: default_value's "" is the
         zero a fresh slot starts at, not nil, and `.nil?` compares against
         NULL -- so assigning it answered false for a global that had just been
         set to nil. */
      buf_puts(b, lv->type == TY_RANGE ? "(sp_Range){0}"
                : lv->type == TY_STRING ? "NULL" : default_value(lv->type));
    else if (v_empty_arr && lv->type == TY_POLY_ARRAY) {
      if (v_lit_frozen) { int _ft = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); _t%d->frozen = 1; _t%d; })", _ft, _ft, _ft); }
      else buf_puts(b, "sp_PolyArray_new()");
    }
    else if (v_empty_arr && array_kind(lv->type)) {
      if (v_lit_frozen) { int _ft = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); _t%d->frozen = 1; _t%d; })",
                   array_kind(lv->type), _ft, array_kind(lv->type), _ft, _ft); }
      else buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
    }
    else if (v_empty_hash && ty_is_hash(lv->type)) {
      const char *hcn = ty_hash_cname(lv->type);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    /* a poly-typed global/const slot boxes a scalar value (`$g = 42` where $g
       elsewhere holds a string/array, so its slot is sp_RbVal) */
    else if (lv->type == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    if (!isg && lv->init_guarded) {
      emit_indent(b, indent); buf_printf(b, "sp_init_in_progress_%s = 0;\n", key);
    }
    return;
  }
  if (sp_streq(ty, "ConstantPathWriteNode")) {
    /* `Mod::X = v`: resolve the constant on the path target and assign. The
       constant must already be typed (registered by analysis), like the
       operator/or/and path-write forms. */
    int tgt = nt_ref(nt, id, "target");
    const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv || cv->type == TY_UNKNOWN) { unsupported(c, id, "constant path write"); return; }
    int v = nt_ref(nt, id, "value");
    int vlit = empty_literal_node(c, v);
    /* `X = [].freeze`: the strip above found the literal so the slot's kind
       builds it; the freeze it stripped still has to happen (#3828). */
    int v_lit_frozen = (vlit != v);
    const char *vty = nt_type(nt, vlit);
    int v_empty_arr = 0, v_empty_hash = 0;
    if (vty && sp_streq(vty, "ArrayNode")) {
      int ac = 0; nt_arr(nt, vlit, "elements", &ac); v_empty_arr = (ac == 0);
    }
    if (vty && (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode"))) {
      int hec = 0; nt_arr(nt, vlit, "elements", &hec); v_empty_hash = (hec == 0);
    }
    emit_indent(b, indent);
    buf_printf(b, "cst_%s = ", nm);
    if (vty && sp_streq(vty, "NilNode"))
      buf_puts(b, cv->type == TY_RANGE ? "(sp_Range){0}" : default_value(cv->type));
    else if (v_empty_arr && cv->type == TY_POLY_ARRAY) {
      if (v_lit_frozen) { int _ft = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); _t%d->frozen = 1; _t%d; })", _ft, _ft, _ft); }
      else buf_puts(b, "sp_PolyArray_new()");
    }
    else if (v_empty_arr && array_kind(cv->type)) {
      if (v_lit_frozen) { int _ft = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); _t%d->frozen = 1; _t%d; })",
                   array_kind(cv->type), _ft, array_kind(cv->type), _ft, _ft); }
      else buf_printf(b, "sp_%sArray_new()", array_kind(cv->type));
    }
    else if (v_empty_hash && ty_is_hash(cv->type)) {
      const char *hcn = ty_hash_cname(cv->type);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (sp_streq(ty, "ConstantPathOperatorWriteNode")) {
    int tgt = nt_ref(nt, id, "target");
    const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) { unsupported(c, id, "constant path operator write"); return; }
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (cv->type == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "cst_%s = sp_str_concat(cst_%s, ", nm, nm); emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "cst_%s %s= ", nm, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "ConstantPathOrWriteNode") || sp_streq(ty, "ConstantPathAndWriteNode")) {
    int is_or = sp_streq(ty, "ConstantPathOrWriteNode");
    int tgt = nt_ref(nt, id, "target");
    const char *nm = tgt >= 0 ? nt_str(nt, tgt, "name") : NULL;
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) { unsupported(c, id, "constant path or/and write"); return; }
    int v = nt_ref(nt, id, "value");
    if (cv->type == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(cst_%s)) cst_%s = ", is_or ? "!" : "", nm, nm);
      emit_boxed(c, v, b); buf_puts(b, ";\n");
    }
    else if (cv->type == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%scst_%s) cst_%s = ", is_or ? "!" : "", nm, nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    else if (!is_or) {  /* &&= on an always-truthy constant: always assign */
      emit_indent(b, indent);
      buf_printf(b, "cst_%s = ", nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* ||= on an always-truthy constant: no-op */
    return;
  }
  if (sp_streq(ty, "ConstantOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) return;
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (cv->type == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "cst_%s = sp_str_concat(cst_%s, ", nm, nm); emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "cst_%s %s= ", nm, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "ConstantOrWriteNode") || sp_streq(ty, "ConstantAndWriteNode")) {
    int is_or = sp_streq(ty, "ConstantOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (!cv) return;
    int v = nt_ref(nt, id, "value");
    if (cv->type == TY_POLY) {
      emit_indent(b, indent);
      buf_printf(b, "if (%ssp_poly_truthy(cst_%s)) { cst_%s = ", is_or ? "!" : "", nm, nm);
      emit_boxed(c, v, b); buf_puts(b, "; }\n");
    }
    else if (cv->type == TY_BOOL) {
      emit_indent(b, indent);
      buf_printf(b, "if (%scst_%s) { cst_%s = ", is_or ? "!" : "", nm, nm); emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (!is_or) {  /* &&= on an always-truthy constant: always assign */
      emit_indent(b, indent);
      buf_printf(b, "cst_%s = ", nm); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    /* ||= on an always-truthy constant: no-op */
    return;
  }
  if (sp_streq(ty, "GlobalVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) return;
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    if (lv->type == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "gv_%s = sp_str_concat(gv_%s, ", rn, rn);
      emit_expr(c, v, b); buf_puts(b, ");\n");
    }
    else {
      buf_printf(b, "gv_%s %s= ", rn, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "GlobalVariableOrWriteNode") || sp_streq(ty, "GlobalVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "GlobalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) return;
    int v = nt_ref(nt, id, "value");
    emit_indent(b, indent);
    buf_printf(b, "if (%sgv_%s) { gv_%s = ", is_or ? "!" : "", rn, rn);
    emit_expr(c, v, b);
    buf_puts(b, "; }\n");
    return;
  }
  if (sp_streq(ty, "MatchRequiredNode")) {
    /* `value => pattern`: destructure pattern into locals. */
    int value = nt_ref(nt, id, "value");
    int pattern = nt_ref(nt, id, "pattern");
    if (value < 0 || pattern < 0) return;
    const char *pty = nt_type(nt, pattern);
    if (!pty) return;
    if (sp_streq(pty, "ArrayPatternNode")) {
      int rn = 0;
      const int *reqs = nt_arr(nt, pattern, "requireds", &rn);
      int pon = 0;
      const int *posts = nt_arr(nt, pattern, "posts", &pon);
      /* `[a, *mid, b]`: the splat absorbs the middle; length becomes a floor. */
      int rest = nt_ref(nt, pattern, "rest");
      int has_rest = 0, rest_tgt = -1;
      /* Both a splat (`[a, *]`/`[a, *mid]`) and a trailing-comma implicit rest
         (`[a,]`, an ImplicitRestNode) make the length a floor, not exact; only
         the named splat binds a target. Mirrors the case/in array path. */
      if (rest >= 0 && nt_type(nt, rest) &&
          (sp_streq(nt_type(nt, rest), "SplatNode") || sp_streq(nt_type(nt, rest), "ImplicitRestNode"))) {
        has_rest = 1;
        if (sp_streq(nt_type(nt, rest), "SplatNode")) {
          int rex = nt_ref(nt, rest, "expression");
          if (rex >= 0 && nt_type(nt, rex) && sp_streq(nt_type(nt, rex), "LocalVariableTargetNode"))
            rest_tgt = rex;
        }
      }
      TyKind vt = comp_ntype(c, value);
      int tarr = ++g_tmp;
      const char *k;
      int obj_dc_def = -1;
      int obj_dc = ty_is_object(vt)
                     ? comp_method_in_chain(c, ty_object_class(vt), "deconstruct", &obj_dc_def)
                     : -1;
      if (obj_dc >= 0) {
        /* `obj => [a, b]` on a class that defines #deconstruct: call it and
           match against the array it answers, as the case/in arm does. The
           untyped branch below read the object itself as an array of ints
           (#3955). */
        int isv = c->classes[ty_object_class(vt)].is_value_type;
        TyKind drt = (TyKind)c->scopes[obj_dc].ret;
        if (!ty_is_array(drt)) drt = TY_POLY_ARRAY;
        const char *dcn = c->classes[obj_dc_def].name;
        int tobj = ++g_tmp;
        emit_indent(b, indent);
        emit_ctype(c, vt, b);
        buf_printf(b, " _t%d = ", tobj); emit_expr(c, value, b); buf_puts(b, ";\n");
        /* Root the receiver: #deconstruct allocates its array. */
        if (!isv) { emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tobj); }
        emit_indent(b, indent); emit_ctype(c, drt, b);
        if (isv) buf_printf(b, " _t%d = sp_%s_deconstruct(_t%d);\n", tarr, dcn, tobj);
        else     buf_printf(b, " _t%d = sp_%s_deconstruct((sp_%s *)_t%d);\n", tarr, dcn, dcn, tobj);
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
        vt = drt;
        k = (drt == TY_POLY_ARRAY) ? "Poly" : array_kind(drt);
      }
      else if (ty_is_object(vt) && c->classes[ty_object_class(vt)].is_struct) {
        /* Struct/Data has no member array; synthesize #deconstruct inline as a
           PolyArray of its (boxed) members, then match against that. */
        ClassInfo *sc = &c->classes[ty_object_class(vt)];
        int tobj = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_%s *_t%d = ", sc->c_name, tobj); emit_expr(c, value, b); buf_puts(b, ";\n");
        /* Root the object: its members are read after sp_PolyArray_new and the
           per-member boxing below, both of which can trigger a GC. */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tobj);
        emit_indent(b, indent);
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tarr, tarr);
        for (int j = 0; j < sc->nivars; j++) {
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", tobj, iv_c(sc->ivars[j] + 1));
          emit_indent(b, indent);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", tarr);
          emit_boxed_text(c, sc->ivar_types[j], fb, b);
          buf_puts(b, ");\n");
        }
        vt = TY_POLY_ARRAY; k = "Poly";
      }
      else {
        k = ty_is_array(vt) ? ((vt == TY_POLY_ARRAY) ? "Poly" : array_kind(vt)) : NULL;
        if (!k) k = "Int";
        emit_indent(b, indent);
        emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_INT_ARRAY, b);
        buf_printf(b, " _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
        emit_indent(b, indent);
        buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
      }
      /* Length check: raise NoMatchingPatternError if sizes differ (a splat
         makes the required+post count a floor instead of an exact size). */
      emit_indent(b, indent);
      if (has_rest)
        buf_printf(b, "if (!_t%d || _t%d->len < %dLL) sp_raise_cls(\"NoMatchingPatternError\", \"[array pattern mismatch]\");\n", tarr, tarr, (long long)(rn + pon));
      else
        buf_printf(b, "if (!_t%d || _t%d->len != %dLL) sp_raise_cls(\"NoMatchingPatternError\", \"[array pattern mismatch]\");\n", tarr, tarr, (long long)rn);
      for (int i = 0; i < rn + pon; i++) {
        int tnode = i < rn ? reqs[i] : posts[i - rn];
        const char *lty2 = nt_type(nt, tnode);
        if (!lty2 || !sp_streq(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, tnode, "name");
        if (!lnm) continue;
        emit_indent(b, indent);
        buf_printf(b, "lv_%s = ", lnm);
        LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
        char gx[80];
        if (i < rn)
          snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
        else
          snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, _t%d->len - %dLL)", k, tarr, tarr, pon - (i - rn));
        if (plv && plv->type == TY_POLY && !sp_streq(k, "Poly")) {
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, ty_array_elem(vt != TY_UNKNOWN ? vt : TY_INT_ARRAY), gx, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_puts(b, gx);
        buf_puts(b, ";\n");
      }
      if (rest_tgt >= 0) {
        const char *rnm = nt_str(nt, rest_tgt, "name");
        if (rnm) {
          LocalVar *rlv = scope_local(comp_scope_of(c, id), rnm);
          char sx[96];
          snprintf(sx, sizeof sx, "sp_%sArray_slice(_t%d, %dLL, _t%d->len - %dLL)",
                   k, tarr, rn, tarr, rn + pon);
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = ", rnm);
          if (rlv && rlv->type == TY_POLY && !sp_streq(k, "Poly")) {
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, vt != TY_UNKNOWN ? vt : TY_INT_ARRAY, sx, &bx);
            buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(b, sx);
          buf_puts(b, ";\n");
        }
      }
    }
    else if (sp_streq(pty, "HashPatternNode")) {
      int pn = 0;
      const int *pelms = nt_arr(nt, pattern, "elements", &pn);
      /* Evaluate value hash into a temp. */
      TyKind vt = comp_ntype(c, value);
      const char *hn = ty_is_hash(vt) ? ty_hash_cname(vt) : NULL;
      int thash = ++g_tmp;
      int obj_dk_def = -1;
      int obj_dk = ty_is_object(vt)
                     ? comp_method_in_chain(c, ty_object_class(vt), "deconstruct_keys", &obj_dk_def)
                     : -1;
      if (vt == TY_MATCHDATA) {
        /* `matchdata => {name:}`: bind through deconstruct_keys, like case/in. */
        int mdt = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_MatchData *_t%d = ", mdt); emit_expr(c, value, b); buf_puts(b, ";\n");
        /* Root the MatchData before deconstruct_keys allocates its hash, so a GC
           during that allocation can't sweep the still-needed match object. */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", mdt);
        char md[24]; snprintf(md, sizeof md, "_t%d", mdt);
        thash = emit_md_deconstruct_keys(b, indent, md);
        hn = "SymPoly";
        vt = TY_SYM_POLY_HASH;
      }
      else if (obj_dk >= 0) {
        /* `obj => {k:}` on a class that defines #deconstruct_keys: call it and
           bind through the hash it answers, as the case/in arm does. Without
           this the object fell to the untyped branch below, whose bindings are
           all skipped for want of a hash type -- every variable stayed nil and
           nothing was raised (#3955). */
        int isv = c->classes[ty_object_class(vt)].is_value_type;
        TyKind drt = (TyKind)c->scopes[obj_dk].ret;
        if (!ty_is_hash(drt)) drt = TY_SYM_POLY_HASH;
        const char *dcn = c->classes[obj_dk_def].name;
        int tobj = ++g_tmp;
        emit_indent(b, indent);
        emit_ctype(c, vt, b);
        buf_printf(b, " _t%d = ", tobj); emit_expr(c, value, b); buf_puts(b, ";\n");
        /* Root the receiver: #deconstruct_keys allocates its hash. */
        if (!isv) { emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tobj); }
        emit_indent(b, indent); emit_ctype(c, drt, b);
        Buf kab3; memset(&kab3, 0, sizeof kab3);
        emit_pm_deconstruct_keys_arg(c, pattern, &kab3);
        if (isv) buf_printf(b, " _t%d = sp_%s_deconstruct_keys(_t%d, %s);\n", thash, dcn, tobj, kab3.p ? kab3.p : "sp_box_nil()");
        else     buf_printf(b, " _t%d = sp_%s_deconstruct_keys((sp_%s *)_t%d, %s);\n", thash, dcn, dcn, tobj, kab3.p ? kab3.p : "sp_box_nil()");
        free(kab3.p);
        if (needs_root(drt)) { emit_indent(b, indent); emit_gc_root_tmp(c, drt, thash, b); buf_puts(b, "\n"); }
        hn = ty_hash_cname(drt);
        vt = drt;
      }
      else if (ty_is_object(vt) && c->classes[ty_object_class(vt)].is_struct) {
        /* `struct => {x:}`: synthesize #deconstruct_keys inline as a SymPolyHash
           of the (boxed) members, then bind through it (like the MatchData path). */
        ClassInfo *sc = &c->classes[ty_object_class(vt)];
        int tobj = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_%s *_t%d = ", sc->c_name, tobj); emit_expr(c, value, b); buf_puts(b, ";\n");
        /* Root the object: its members are read after sp_SymPolyHash_new and the
           per-member boxing below, both of which can trigger a GC. */
        emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tobj);
        emit_indent(b, indent);
        buf_printf(b, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n", thash, thash);
        for (int j = 0; j < sc->nivars; j++) {
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", tobj, iv_c(sc->ivars[j] + 1));
          emit_indent(b, indent);
          buf_printf(b, "sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", thash, comp_sym_intern(c, sc->ivars[j] + 1));
          emit_boxed_text(c, sc->ivar_types[j], fb, b);
          buf_puts(b, ");\n");
        }
        hn = "SymPoly";
        vt = TY_SYM_POLY_HASH;
      }
      else {
        emit_indent(b, indent);
        if (hn) { buf_printf(b, "sp_%sHash *_t%d = ", hn, thash); }
        else { buf_printf(b, "void *_t%d = (void *)", thash); }
        emit_expr(c, value, b); buf_puts(b, ";\n");
      }
      for (int i = 0; i < pn; i++) {
        const char *ety = nt_type(nt, pelms[i]);
        if (!ety || !sp_streq(ety, "AssocNode")) continue;
        int pkey = nt_ref(nt, pelms[i], "key");
        int ptgt = nt_ref(nt, pelms[i], "value");
        if (ptgt < 0) continue;
        const char *tty = nt_type(nt, ptgt);
        if (!tty || !sp_streq(tty, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, ptgt, "name");
        if (!lnm || !hn) continue;
        emit_indent(b, indent);
        /* unbox TY_POLY hash value to the local's concrete type */
        TyKind hvt = ty_hash_val(vt);
        Scope *hpsc = comp_scope_of(c, id);
        LocalVar *hplv = hpsc ? scope_local(hpsc, lnm) : NULL;
        TyKind hpltype = hplv ? hplv->type : TY_UNKNOWN;
        if (hvt == TY_POLY && hpltype != TY_UNKNOWN && hpltype != TY_POLY) {
          int htmp = ++g_tmp;
          buf_printf(b, "{ sp_RbVal _t%d = sp_%sHash_get(_t%d, ", htmp, hn, thash);
          emit_expr(c, pkey, b); buf_puts(b, "); ");
          if (hpltype == TY_STRING) buf_printf(b, "lv_%s = _t%d.v.s; }\n", lnm, htmp);
          else if (hpltype == TY_INT) buf_printf(b, "lv_%s = _t%d.v.i; }\n", lnm, htmp);
          else if (hpltype == TY_FLOAT) buf_printf(b, "lv_%s = _t%d.v.f; }\n", lnm, htmp);
          else if (hpltype == TY_BOOL) buf_printf(b, "lv_%s = _t%d.v.b; }\n", lnm, htmp);
          else if (hpltype == TY_SYMBOL) buf_printf(b, "lv_%s = (sp_sym)_t%d.v.i; }\n", lnm, htmp);
          else { buf_puts(b, "}\n"); emit_indent(b, indent);
                 buf_printf(b, "lv_%s = sp_%sHash_get(_t%d, ", lnm, hn, thash);
                 emit_expr(c, pkey, b); buf_puts(b, ");\n"); }
        }
        else {
          buf_printf(b, "lv_%s = sp_%sHash_get(_t%d, ", lnm, hn, thash);
          emit_expr(c, pkey, b); buf_puts(b, ");\n");
        }
      }
      /* `**rest`: copy every pair whose key is not among the listed ones
         (mirrors the case/in arm binder). */
      int hp_rest = nt_ref(nt, pattern, "rest");
      if (hn && hp_rest >= 0 && nt_type(nt, hp_rest) &&
          sp_streq(nt_type(nt, hp_rest), "AssocSplatNode")) {
        int rin = nt_ref(nt, hp_rest, "value");
        const char *rnm = (rin >= 0 && nt_type(nt, rin) &&
                           sp_streq(nt_type(nt, rin), "LocalVariableTargetNode"))
                          ? nt_str(nt, rin, "name") : NULL;
        if (rnm) {
          TyKind hkt = ty_hash_key(vt);
          int tr = ++g_tmp, ti2 = ++g_tmp, tk2 = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "{ sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hn, tr, hn, tr);
          emit_indent(b, indent + 1);
          buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, thash, ti2);
          emit_indent(b, indent + 2);
          emit_ctype(c, hkt, b);
          if (vt == TY_POLY_POLY_HASH)
            buf_printf(b, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk2, thash, thash, ti2);
          else
            buf_printf(b, " _t%d = _t%d->order[_t%d];\n", tk2, thash, ti2);
          for (int i = 0; i < pn; i++) {
            if (!nt_type(nt, pelms[i]) || !sp_streq(nt_type(nt, pelms[i]), "AssocNode")) continue;
            int key = nt_ref(nt, pelms[i], "key");
            if (key < 0) continue;
            {
              const char *kty2 = nt_type(nt, key);
              if ((kty2 && sp_streq(kty2, "SymbolNode") && (hkt == TY_STRING)) ||
                  (kty2 && sp_streq(kty2, "StringNode") && (hkt == TY_SYMBOL || hkt == TY_INT)) ||
                  (kty2 && sp_streq(kty2, "IntegerNode") && (hkt == TY_STRING || hkt == TY_SYMBOL)))
                continue;  /* can't be present: no exclusion needed */
            }
            emit_indent(b, indent + 2);
            if (hkt == TY_POLY) {
              buf_printf(b, "if (sp_poly_eq(_t%d, ", tk2); emit_boxed(c, key, b);
            }
            else if (hkt == TY_STRING) {
              buf_printf(b, "if (_t%d && strcmp(_t%d, ", tk2, tk2); emit_expr(c, key, b); buf_puts(b, ") == 0");
            }
            else {
              buf_printf(b, "if (_t%d == (", tk2); emit_expr(c, key, b);
            }
            buf_puts(b, ")) continue;\n");
          }
          emit_indent(b, indent + 2);
          buf_printf(b, "sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d));\n",
                     hn, tr, tk2, hn, thash, tk2);
          emit_indent(b, indent + 1); buf_puts(b, "}\n");
          emit_indent(b, indent + 1);
          {
            LocalVar *rlv2 = scope_local(comp_scope_of(c, id), rnm);
            if (rlv2 && rlv2->type == TY_POLY) {
              char rv[24]; snprintf(rv, sizeof rv, "_t%d", tr);
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, vt, rv, &bx);
              buf_printf(b, "lv_%s = %s;\n", rnm, bx.p ? bx.p : rv);
              free(bx.p);
            }
            else buf_printf(b, "lv_%s = _t%d;\n", rnm, tr);
          }
          emit_indent(b, indent); buf_puts(b, "}\n");
        }
      }
    }
    return;
  }
  if (sp_streq(ty, "MultiWriteNode")) {
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    int value = nt_ref(nt, id, "value");
    const char *vty = nt_type(nt, value);
    /* `r, w = IO.pipe` -> make a pipe, bind both ends as IO handles. */
    if (ln == 2 && vty && sp_streq(vty, "CallNode") && nt_str(nt, value, "name") &&
        sp_streq(nt_str(nt, value, "name"), "pipe")) {
      int vrecv = nt_ref(nt, value, "receiver");
      if (vrecv >= 0 && nt_type(nt, vrecv) && sp_streq(nt_type(nt, vrecv), "ConstantReadNode") &&
          nt_str(nt, vrecv, "name") && sp_streq(nt_str(nt, vrecv, "name"), "IO") &&
          nt_type(nt, lefts[0]) && sp_streq(nt_type(nt, lefts[0]), "LocalVariableTargetNode") &&
          nt_type(nt, lefts[1]) && sp_streq(nt_type(nt, lefts[1]), "LocalVariableTargetNode")) {
        const char *rn0 = nt_str(nt, lefts[0], "name");
        const char *wn0 = nt_str(nt, lefts[1], "name");
        if (rn0 && wn0) {
          int tf = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "{ int _t%d[2]; sp_io_make_pipe(_t%d); ", tf, tf);
          buf_printf(b, "lv_%s = sp_io_fdopen(_t%d[0], \"r\"); ", rn0, tf);
          buf_printf(b, "lv_%s = sp_io_fdopen(_t%d[1], \"w\"); }\n", wn0, tf);
          return;
        }
      }
    }
    int en = 0;
    const int *els = (vty && sp_streq(vty, "ArrayNode")) ? nt_arr(nt, value, "elements", &en) : NULL;
    /* A splat among the RHS elements (`*a = *x`, `a, b = 1, *rest`) makes the
       tuple statically unsized: drop the per-element-temp tuple path and let
       the runtime-destructure path evaluate the whole ArrayNode (the literal
       emitter splices splats) and slice it. */
    if (els) {
      for (int i = 0; i < en; i++) {
        const char *ety0 = nt_type(nt, els[i]);
        if (ety0 && sp_streq(ety0, "SplatNode")) { els = NULL; en = 0; break; }
      }
    }
    int rn = 0;
    const int *rights = nt_arr(nt, id, "rights", &rn);
    int rest_nid = nt_ref(nt, id, "rest");
    int rest_inner = -1;
    const char *rest_var = NULL;
    const char *rest_gvar = NULL;  /* global variable name (without $) for *$rest */
    if (rest_nid >= 0) {
      const char *rsty = nt_type(nt, rest_nid);
      if (rsty && sp_streq(rsty, "SplatNode"))
        rest_inner = nt_ref(nt, rest_nid, "expression");
      if (rest_inner >= 0 && nt_type(nt, rest_inner)) {
        if (sp_streq(nt_type(nt, rest_inner), "LocalVariableTargetNode"))
          rest_var = nt_str(nt, rest_inner, "name");
        else if (sp_streq(nt_type(nt, rest_inner), "GlobalVariableTargetNode")) {
          const char *gnm_r = nt_str(nt, rest_inner, "name");
          if (gnm_r) rest_gvar = comp_resolve_gvar(c, gnm_r + 1);
        }
      }
    }
    if (!els) {
      /* scalar RHS (`a, b = 1`): the first target takes the value, the rest
         their slot default (Ruby gives nil; we land the typed zero). A call /
         super / yield can return a multi-value tuple, so those are excluded
         and fall through to the tuple-destructuring path. */
      TyKind st = comp_ntype(c, value);
      /* RHS `*expr` (a, *b = *x) builds an ARRAY (splat-to-array), but
         comp_ntype answers a SplatNode with the ELEMENT type (that arm
         serves array-literal splices) -- override so the destructure path
         below runs; emit_expr's SplatNode arm already yields a normalized
         sp_PolyArray*. */
      if (vty && sp_streq(vty, "SplatNode")) st = TY_POLY_ARRAY;
      int multi_src = vty && (sp_streq(vty, "CallNode") || sp_streq(vty, "SuperNode") ||
                              sp_streq(vty, "ForwardingSuperNode") || sp_streq(vty, "YieldNode"));
      /* a TY_POLY value can hold an array at runtime (doom's
         `lump_name, mirrored = @sprite_index[key]` read through a local),
         so it must take the runtime-destructure path below, not the
         scalar fill -- which handed the whole array to the first target. */
      if (vty && !multi_src && !ty_is_array(st) && !ty_is_hash(st) && st != TY_UNKNOWN && st != TY_POLY) {
        /* rest target under a scalar RHS: `*a = 5` collects [5]; with fixed
           targets present (`a, *r = 5`) the scalar goes to the first target
           and the rest is empty. */
        if (rest_var) {
          Scope *rsc0 = comp_scope_of(c, id);
          LocalVar *rlv0 = scope_local(rsc0, rest_var);
          TyKind rat0 = rlv0 && ty_is_array(rlv0->type) ? rlv0->type : TY_POLY_ARRAY;
          const char *rk0 = (rat0 == TY_POLY_ARRAY) ? "Poly" : array_kind(rat0);
          if (!rk0) rk0 = "Poly";
          int tr0 = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", rk0, tr0, rk0, tr0);
          /* The scalar goes into the rest ONLY when there are no fixed targets on
             either side (`*a = 5` -> [5]); with post-rest targets (`*a, b = 1`)
             the value aligns to the end and the rest stays empty. */
          if (ln == 0 && rn == 0) {
            emit_indent(b, indent);
            buf_printf(b, "sp_%sArray_push(_t%d, ", rk0, tr0);
            if (rat0 == TY_POLY_ARRAY) emit_boxed(c, value, b);
            else emit_expr(c, value, b);
            buf_puts(b, ");\n");
          }
          emit_indent(b, indent);
          emit_local_ref(c, id, rest_var, b); buf_printf(b, " = _t%d;\n", tr0);
          if (ln == 0 && rn == 0) return;
        }
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) continue;
          emit_indent(b, indent);
          const char *lvn = nt_str(nt, lefts[i], "name");
          emit_local_ref(c, lefts[i], lvn, b); buf_puts(b, " = ");
          LocalVar *llv = lvn ? scope_local(comp_scope_of(c, id), lvn) : NULL;
          int lpoly = llv && llv->type == TY_POLY;
          if (i == 0) { if (lpoly && st != TY_POLY) emit_boxed(c, value, b); else emit_expr(c, value, b); }
          else if (lpoly) {
            /* under-filled target under a scalar RHS is Ruby nil, not the
               typed zero (`a, b, c = 1` -> [1, nil, nil]). */
            buf_puts(b, "sp_box_nil()");
          }
          else {
            /* the target NODE's type can read TY_UNKNOWN here; use the local's
               DECLARED type so the nil sentinel matches its C slot. */
            TyKind tt = llv ? llv->type : comp_ntype(c, lefts[i]);
            buf_puts(b, nil_sentinel(tt));
          }
          buf_puts(b, ";\n");
        }
        /* Post-rest targets under a scalar RHS (`*a, b, c = 1`): the empty rest
           leaves the single value to fill the rights left-to-right, so the first
           right takes it when no leading target consumed it (ln == 0) and every
           other right is nil (`*a, b, c = 1` -> a=[], b=1, c=nil). */
        for (int j = 0; j < rn; j++) {
          const char *rty2 = nt_type(nt, rights[j]);
          if (!rty2 || !sp_streq(rty2, "LocalVariableTargetNode")) continue;
          const char *rvn = nt_str(nt, rights[j], "name");
          LocalVar *rlv = rvn ? scope_local(comp_scope_of(c, id), rvn) : NULL;
          int rpoly = rlv && rlv->type == TY_POLY;
          emit_indent(b, indent);
          emit_local_ref(c, rights[j], rvn, b); buf_puts(b, " = ");
          if (j == 0 && ln == 0) {
            if (rpoly && st != TY_POLY) emit_boxed(c, value, b); else emit_expr(c, value, b);
          }
          else if (rpoly) buf_puts(b, "sp_box_nil()");
          else { TyKind tt = rlv ? rlv->type : comp_ntype(c, rights[j]); buf_puts(b, nil_sentinel(tt)); }
          buf_puts(b, ";\n");
        }
        return;
      }
      /* any expression returning a typed array: runtime destructure */
      if (ty_is_array(st) && st != TY_UNKNOWN) {
        const char *k = (st == TY_POLY_ARRAY) ? "Poly" : array_kind(st);
        if (!k) k = "Int";
        TyKind elem = ty_array_elem(st);
        int tarr = ++g_tmp;
        emit_indent(b, indent);
        emit_ctype(c, st, b);
        buf_printf(b, " _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
        emit_indent(b, indent);
        buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
        Scope *rt_scope = comp_scope_of(c, id);
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty) continue;
          if (sp_streq(lty, "MultiTargetNode") && sp_streq(k, "Poly")) {
            /* nested (a, (b, c)) target: recurse over the boxed sub-array */
            char ge[80]; snprintf(ge, sizeof ge, "sp_PolyArray_get(_t%d, %dLL)", tarr, i);
            emit_massign_poly_target(c, lefts[i], ge, indent, b, rt_scope);
          }
          else if (sp_streq(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            const char *lvn = nt_str(nt, lefts[i], "name");
            /* Through emit_local_ref: a target captured by a later block lives
               in a heap cell, and writing `lv_<name>` there names a variable
               that was never declared -- the C compilation aborts on the
               assignment while the cell sits right beside it (#3424). */
            emit_local_ref(c, lefts[i], lvn, b);
            buf_puts(b, " = ");
            LocalVar *llv = lvn ? scope_local(rt_scope, lvn) : NULL;
            TyKind ltt = llv ? llv->type : comp_ntype(c, lefts[i]);
            char gx[64]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
            if (ltt == TY_POLY && !sp_streq(k, "Poly")) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, elem, gx, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else if (sp_streq(k, "Poly") && ltt != TY_POLY && ltt != TY_UNKNOWN) {
              /* typed target from a poly tuple (known multi-value return) */
              emit_unbox_text(c, ltt, gx, b);
            }
            else buf_puts(b, gx);
            buf_puts(b, ";\n");
          }
          else if (sp_streq(lty, "InstanceVariableTargetNode") &&
                   rt_scope && rt_scope->class_id >= 0) {
            const char *ivnm = nt_str(nt, lefts[i], "name");
            if (!ivnm) continue;
            emit_indent(b, indent);
            char get_expr[64]; snprintf(get_expr, sizeof get_expr, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
            TyKind ivt = TY_UNKNOWN;
            int iv_rt = comp_ivar_index(&c->classes[rt_scope->class_id], ivnm);
            if (iv_rt >= 0) ivt = c->classes[rt_scope->class_id].ivar_types[iv_rt];
            if (rt_scope->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope->class_id].name, ivnm + 1);
            else
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(ivnm + 1));
            if (ivt == TY_POLY && elem != TY_POLY) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, elem, get_expr, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else if (sp_streq(k, "Poly") && ivt != TY_POLY && ivt != TY_UNKNOWN) {
              /* typed target from a poly tuple (known multi-value return) */
              emit_unbox_text(c, ivt, get_expr, b);
            }
            else buf_puts(b, get_expr);
            buf_puts(b, ";\n");
          }
          else if ((sp_streq(lty, "ConstantTargetNode") || sp_streq(lty, "ConstantPathTargetNode"))) {
            const char *cnm_rt = nt_str(nt, lefts[i], "name");
            LocalVar *cv_rt = cnm_rt ? comp_const(c, cnm_rt) : NULL;
            if (!cv_rt) continue;
            emit_indent(b, indent);
            char cgx[80]; snprintf(cgx, sizeof cgx, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
            buf_printf(b, "cst_%s = ", cnm_rt);
            if (sp_streq(k, "Poly") && cv_rt->type != TY_POLY && cv_rt->type != TY_UNKNOWN)
              emit_unbox_text(c, cv_rt->type, cgx, b);  /* typed constant from a poly tuple */
            else
              buf_puts(b, cgx);
            buf_puts(b, ";\n");
          }
          /* setter target (`obj.attr = elem`): invoke the writer method so a
             custom writer (e.g. CPU#next_frame_clock= setting @clk_frame) runs.
             Without this the target was silently skipped (optcarrot's
             `@vclk, @hclk_target, @cpu.next_frame_clock = BOOT_FRAME`). */
          else if (sp_streq(lty, "CallTargetNode")) {
            const char *setnm = nt_str(nt, lefts[i], "name");
            int crecv = nt_ref(nt, lefts[i], "receiver");
            size_t snl = setnm ? strlen(setnm) : 0;
            if (!setnm || snl < 2 || setnm[snl - 1] != '=' || crecv < 0) { unsupported(c, id, "multiple assignment call target"); continue; }
            TyKind crt = comp_ntype(c, crecv);
            if (!ty_is_object(crt)) { unsupported(c, id, "multiple assignment call target non-object"); continue; }
            int crc = ty_object_class(crt);
            int cdef = -1;
            int wmi = comp_method_in_chain(c, crc, setnm, &cdef);
            char get_expr[80]; snprintf(get_expr, sizeof get_expr, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
            emit_indent(b, indent);
            if (wmi >= 0 && cdef >= 0) {
              /* real writer method */
              LocalVar *wp = c->scopes[wmi].nparams >= 1 ? scope_local(&c->scopes[wmi], c->scopes[wmi].pnames[0]) : NULL;
              TyKind pt = wp ? wp->type : elem;
              buf_printf(b, "sp_%s_%s((sp_%s *)", c->classes[cdef].c_name, mc(c->scopes[wmi].name), c->classes[cdef].c_name);
              emit_expr(c, crecv, b); buf_puts(b, ", ");
              if (pt == TY_POLY && elem != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem, get_expr, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
              else buf_puts(b, get_expr);
              buf_puts(b, ");\n");
            }
            else {
              /* attr_writer convention: name matches the backing ivar */
              char base[256]; memcpy(base, setnm, snl - 1); base[snl - 1] = '\0';
              buf_puts(b, "("); emit_expr(c, crecv, b); buf_printf(b, ")->iv_%s = %s;\n", iv_c(base), get_expr);
            }
          }
        }
        if (rest_var) {
          Scope *rscope = comp_scope_of(c, id);
          LocalVar *rlv = scope_local(rscope, rest_var);
          TyKind rest_arr_t = rlv ? rlv->type : st;
          if (!ty_is_array(rest_arr_t)) rest_arr_t = st;
          const char *rk = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
          if (!rk) rk = k;
          int tr = ++g_tmp;
          emit_indent(b, indent);
          /* On underflow (fewer elements than the fixed pre/post targets) the
             splat collects nothing rather than wrapping to a negative length;
             clamp the slice length to zero. */
          /* The slice is taken from the SOURCE array, so it must use the
             source's kind; the rest local's own kind decides only what the
             slice is converted to. Slicing a poly array through
             sp_IntArray_slice read one struct as another (#3975 sweep). */
          buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_slice(_t%d, %dLL, _t%d->len > %dLL ? _t%d->len - %dLL : 0LL);\n",
                     k, tr, k, tarr, ln, tarr, ln + rn, tarr, ln + rn);
          emit_indent(b, indent);
          buf_printf(b, "SP_GC_ROOT(_t%d);\n", tr);
          emit_indent(b, indent);
          emit_local_ref(c, id, rest_var, b); buf_puts(b, " = ");
          if (sp_streq(rk, k)) buf_printf(b, "_t%d;\n", tr);
          else if (sp_streq(k, "Poly"))
            buf_printf(b, "sp_%sArray_from_poly_array(_t%d);\n",
                       sp_streq(rk, "Int") ? "Int" : sp_streq(rk, "Str") ? "Str" : "Float", tr);
          else if (sp_streq(rk, "Poly"))
            buf_printf(b, "sp_%sArray_to_poly%s(_t%d);\n", k, sp_streq(k, "Str") ? "_fmt" : "", tr);
          else buf_printf(b, "_t%d;\n", tr);
        }
        for (int j = 0; j < rn; j++) {
          const char *lty = nt_type(nt, rights[j]);
          if (!lty) continue;
          if (sp_streq(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            const char *rlvn = nt_str(nt, rights[j], "name");
            LocalVar *rllv = rlvn ? scope_local(rt_scope, rlvn) : NULL;
            /* CRuby fills the post-splat targets from the back when the source
               has enough elements, but on underflow (fewer than ln+rn) it
               assigns the remaining elements left-to-right starting just past
               the pre targets and nil-fills the rest. That position is the max
               of the back-aligned (len-rn+j) and front-aligned (ln+j) indices;
               a position at or past the end nil-fills. */
            int tix = ++g_tmp;
            buf_printf(b, "sp_int _t%d = (_t%d->len - %dLL + %dLL) > %dLL ? (_t%d->len - %dLL + %dLL) : %dLL;\n",
                       tix, tarr, rn, j, ln + j, tarr, rn, j, ln + j);
            emit_indent(b, indent);
            emit_local_ref(c, rights[j], rlvn, b); buf_puts(b, " = ");
            char rgx[96]; snprintf(rgx, sizeof rgx, "sp_%sArray_get(_t%d, _t%d)", k, tarr, tix);
            if (rllv && rllv->type == TY_POLY && !sp_streq(k, "Poly")) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, elem, rgx, &bx);
              buf_printf(b, "(_t%d >= _t%d->len ? sp_box_nil() : ", tix, tarr);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
              buf_puts(b, ")");
            }
            else {
              const char *nilv = sp_streq(k, "Poly") ? "sp_box_nil()"
                               : sp_streq(k, "Int") ? "SP_INT_NIL"
                               : sp_streq(k, "Float") ? "sp_float_nil()"
                               : "NULL";
              buf_printf(b, "(_t%d >= _t%d->len ? %s : %s)", tix, tarr, nilv, rgx);
            }
            buf_puts(b, ";\n");
          }
          else if (sp_streq(lty, "InstanceVariableTargetNode") &&
                   rt_scope && rt_scope->class_id >= 0) {
            const char *ivnm2 = nt_str(nt, rights[j], "name");
            if (!ivnm2) continue;
            emit_indent(b, indent);
            /* Same underflow clamp as the local-variable branch above: pick the
               post-splat source index as the max of the back-aligned and
               front-aligned positions, nil-filling any position at or past the
               end (a, *b, @c = [1] -> @c = nil). */
            int tix = ++g_tmp;
            buf_printf(b, "sp_int _t%d = (_t%d->len - %dLL + %dLL) > %dLL ? (_t%d->len - %dLL + %dLL) : %dLL;\n",
                       tix, tarr, rn, j, ln + j, tarr, rn, j, ln + j);
            emit_indent(b, indent);
            char get_expr2[96];
            snprintf(get_expr2, sizeof get_expr2, "sp_%sArray_get(_t%d, _t%d)", k, tarr, tix);
            TyKind ivt2 = TY_UNKNOWN;
            int iv_rt2 = comp_ivar_index(&c->classes[rt_scope->class_id], ivnm2);
            if (iv_rt2 >= 0) ivt2 = c->classes[rt_scope->class_id].ivar_types[iv_rt2];
            if (rt_scope->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope->class_id].name, ivnm2 + 1);
            else
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(ivnm2 + 1));
            if (ivt2 == TY_POLY && elem != TY_POLY) {
              Buf bx2; memset(&bx2, 0, sizeof bx2);
              emit_boxed_text(c, elem, get_expr2, &bx2);
              buf_printf(b, "(_t%d >= _t%d->len ? sp_box_nil() : ", tix, tarr);
              buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
              buf_puts(b, ")");
            }
            else {
              const char *nilv = sp_streq(k, "Poly") ? "sp_box_nil()"
                               : sp_streq(k, "Int") ? "SP_INT_NIL"
                               : sp_streq(k, "Float") ? "sp_float_nil()"
                               : "NULL";
              buf_printf(b, "(_t%d >= _t%d->len ? %s : %s)", tix, tarr, nilv, get_expr2);
            }
            buf_puts(b, ";\n");
          }
        }
        return;
      }
      /* poly RHS: destructure with sp_poly_arr_get */
      if (st == TY_POLY) {
        int tarr = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_RbVal _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
        emit_indent(b, indent);
        buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", tarr);
        Scope *rt_scope_p = comp_scope_of(c, id);
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty) continue;
          if (sp_streq(lty, "MultiTargetNode")) {
            /* nested (a, (b, c)) target: recurse over the boxed sub-value */
            char ge[80]; snprintf(ge, sizeof ge, "sp_poly_massign_get(_t%d, %dLL)", tarr, i);
            emit_massign_poly_target(c, lefts[i], ge, indent, b, rt_scope_p);
          }
          else if (sp_streq(lty, "LocalVariableTargetNode")) {
            const char *lnm = nt_str(nt, lefts[i], "name");
            emit_indent(b, indent);
            emit_local_ref(c, lefts[i], lnm, b);
            buf_puts(b, " = ");
            { char mge[64]; snprintf(mge, sizeof mge, "sp_poly_massign_get(_t%d, %dLL)", tarr, i);
              LocalVar *mlv = scope_local(comp_scope_of(c, lefts[i]), lnm);
              /* The target's C slot is whatever the fixpoint settled on it: a
                 scalar one takes the unboxed value, not the sp_RbVal. */
              if (mlv && mlv->type != TY_POLY && mlv->type != TY_UNKNOWN)
                emit_unbox_text(c, mlv->type, mge, b);
              else buf_puts(b, mge); }
            buf_puts(b, ";\n");
          }
          else if (sp_streq(lty, "InstanceVariableTargetNode") &&
                   rt_scope_p && rt_scope_p->class_id >= 0) {
            const char *ivnm = nt_str(nt, lefts[i], "name");
            if (!ivnm) continue;
            int iv_rt = comp_ivar_index(&c->classes[rt_scope_p->class_id], ivnm);
            if (iv_rt < 0) continue;
            TyKind ivt = c->classes[rt_scope_p->class_id].ivar_types[iv_rt];
            emit_indent(b, indent);
            char get_expr[64]; snprintf(get_expr, sizeof get_expr, "sp_poly_massign_get(_t%d, %dLL)", tarr, i);
            if (rt_scope_p->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope_p->class_id].name, ivnm + 1);
            else
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(ivnm + 1));
            if (ivt != TY_POLY) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_unbox_text(c, ivt, get_expr, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else buf_puts(b, get_expr);
            buf_puts(b, ";\n");
          }
        }
        /* rest (`b, *r = <poly>`) + post-splat rights, mirroring the typed-array
           branch but reading the boxed value via sp_poly_arr_get / _len. */
        if (rest_var || rn > 0) {
          int tn = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "sp_int _t%d = sp_poly_arr_len(_t%d);\n", tn, tarr);
          if (rest_var) {
            int tr = ++g_tmp, ti = ++g_tmp;
            emit_indent(b, indent);
            buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tr, tr);
            emit_indent(b, indent);
            buf_printf(b, "for (sp_int _t%d = %dLL; _t%d < _t%d - %dLL; _t%d++) sp_PolyArray_push(_t%d, sp_poly_arr_get(_t%d, _t%d));\n",
                       ti, ln, ti, tn, rn, ti, tr, tarr, ti);
            emit_indent(b, indent);
            emit_local_ref(c, id, rest_var, b); buf_printf(b, " = _t%d;\n", tr);
          }
          for (int j = 0; j < rn; j++) {
            const char *lty = nt_type(nt, rights[j]);
            if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) continue;
            const char *rlvn = nt_str(nt, rights[j], "name");
            if (!rlvn) continue;
            /* CRuby fills post-splat targets from the back; on underflow assign
               left-to-right past the pre targets and nil-fill (max of the back-
               and front-aligned index, a position at/past the end -> nil). */
            int tix = ++g_tmp;
            emit_indent(b, indent);
            buf_printf(b, "sp_int _t%d = (_t%d - %dLL + %dLL) > %dLL ? (_t%d - %dLL + %dLL) : %dLL;\n",
                       tix, tn, rn, j, ln + j, tn, rn, j, ln + j);
            emit_indent(b, indent);
            emit_local_ref(c, rights[j], rlvn, b);
            buf_printf(b, " = (_t%d >= _t%d ? sp_box_nil() : sp_poly_massign_get(_t%d, _t%d));\n",
                       tix, tn, tarr, tix);
          }
        }
        return;
      }
      unsupported(c, id, "multiple assignment");
    }
    if (rest_nid < 0 && en < ln + rn) {
      /* Under-filled literal RHS (`a, b, c = [10, 20]` -> c is nil). The tail
         loop below nil-fills missing local-variable targets; a non-local target
         past the supplied count isn't wired for that, so reject those loudly
         rather than silently skip the assignment. (rn>0 needs trailing elements
         a short RHS can't provide.) */
      if (rn > 0) { unsupported(c, id, "multiple assignment"); return; }
      for (int i = en; i < ln; i++) {
        const char *lty = nt_type(nt, lefts[i]);
        if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) { unsupported(c, id, "multiple assignment"); return; }
      }
    }
    /* evaluate all RHS values into temps first (so `a, b = b, a` swaps).
       Save each temp index separately: emit_expr may consume extra g_tmp
       slots via preludes (e.g. array literals), so base+i is unreliable. */
    int *tmps = en > 0 ? alloca(sizeof(int) * (size_t)en) : NULL;
    /* The RESOLVED C type each temp was declared with (empty-literal adoption
       below can override the element node's inferred type); the assign loop
       must box/unbox from this, not re-derive comp_ntype (#3280). */
    TyKind *tmpts = en > 0 ? alloca(sizeof(TyKind) * (size_t)en) : NULL;
    for (int i = 0; i < en; i++) {
      tmps[i] = ++g_tmp;
      emit_indent(b, indent);
      /* A nil (or void) element has no scalar C type; hold it as a boxed poly
         temp so the slot is valid and a poly target can read it directly. */
      TyKind elt = comp_ntype(c, els[i]);
      /* An empty array/hash literal has no element type of its own, so elt stays
         UNKNOWN and the temp would be declared `void`. Two independent empty
         literals (`numbers, strings = [], []`) each take their matching target's
         concrete container type and materialize a fresh container of it, so each
         slot is typed from its own later use rather than merged/voided (#3213). */
      const char *elty = nt_type(nt, els[i]);
      int el_ec = 0;
      int el_empty_arr = elty && sp_streq(elty, "ArrayNode") &&
                         (nt_arr(nt, els[i], "elements", &el_ec), el_ec == 0);
      int el_empty_hash = elty && (sp_streq(elty, "HashNode") || sp_streq(elty, "KeywordHashNode")) &&
                          (nt_arr(nt, els[i], "elements", &el_ec), el_ec == 0);
      if ((el_empty_arr || el_empty_hash) && (elt == TY_UNKNOWN || elt == TY_VOID) && i < ln &&
          nt_type(nt, lefts[i]) && sp_streq(nt_type(nt, lefts[i]), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, lefts[i], "name");
        LocalVar *tlv = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
        if (tlv && ((el_empty_arr && ty_is_array(tlv->type)) ||
                    (el_empty_hash && ty_is_hash(tlv->type)))) elt = tlv->type;
      }
      /* an IVAR target adopts its declared ivar type the same way; a poly
         ivar takes a fresh boxed container (the temp would otherwise be
         declared `void`, a C error) (#3280) */
      if ((el_empty_arr || el_empty_hash) && (elt == TY_UNKNOWN || elt == TY_VOID) && i < ln &&
          nt_type(nt, lefts[i]) && sp_streq(nt_type(nt, lefts[i]), "InstanceVariableTargetNode")) {
        Scope *isc0 = comp_scope_of(c, id);
        const char *ivn0 = nt_str(nt, lefts[i], "name");
        if (isc0 && isc0->class_id >= 0 && ivn0) {
          int ix0 = comp_ivar_index(&c->classes[isc0->class_id], ivn0);
          TyKind it0 = ix0 >= 0 ? c->classes[isc0->class_id].ivar_types[ix0] : TY_UNKNOWN;
          if ((el_empty_arr && ty_is_array(it0)) ||
              (el_empty_hash && ty_is_hash(it0)) || it0 == TY_POLY) elt = it0;
        }
      }
      int adopt_empty_arr = el_empty_arr && ty_is_array(elt);
      int adopt_empty_hash = el_empty_hash && ty_is_hash(elt);
      int poly_empty_arr = el_empty_arr && elt == TY_POLY;
      int poly_empty_hash = el_empty_hash && elt == TY_POLY;
      int nilish = (elt == TY_NIL || elt == TY_VOID);
      emit_ctype(c, nilish ? TY_POLY : elt, b);
      buf_printf(b, " _t%d = ", tmps[i]);
      if (nilish) {
        /* A void element (e.g. a call that raises) still runs for its side
           effects; only its absent value is replaced with nil. */
        if (!node_is_pure_literal(nt, els[i])) {
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb);
          if (vb.p && vb.p[0]) buf_printf(b, "((void)(%s), sp_box_nil())", vb.p);
          else buf_puts(b, "sp_box_nil()");
          free(vb.p);
        }
        else buf_puts(b, "sp_box_nil()");
      }
      else if (adopt_empty_arr) {
        if (elt == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
        else buf_printf(b, "sp_%sArray_new()", array_kind(elt) ? array_kind(elt) : "Int");
      }
      else if (adopt_empty_hash) {
        const char *hcn = ty_hash_cname(elt);
        if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
        else { Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb); buf_puts(b, vb.p ? vb.p : ""); free(vb.p); }
      }
      else if (poly_empty_arr)
        buf_puts(b, "sp_box_nullable_obj((void *)sp_PolyArray_new(), SP_BUILTIN_POLY_ARRAY)");
      else if (poly_empty_hash)
        buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)");
      else {
        Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb);
        buf_puts(b, vb.p ? vb.p : ""); free(vb.p);
      }
      buf_puts(b, ";\n");
      if (tmpts) tmpts[i] = nilish ? TY_POLY : elt;
    }
    /* assign lefts */
    for (int i = 0; i < ln; i++) {
      const char *lty = nt_type(nt, lefts[i]);
      if (i >= en) {
        if (lty && sp_streq(lty, "LocalVariableTargetNode")) {
          emit_indent(b, indent);
          const char *lvn = nt_str(nt, lefts[i], "name");
          /* Use the local's declared type, not the target node's: an
             under-filled slot lands nil, so the variable is typically widened
             to poly and needs a boxed-nil default rather than a scalar zero. */
          LocalVar *llv = lvn ? scope_local(comp_scope_of(c, id), lvn) : NULL;
          TyKind ltt = llv ? llv->type : comp_ntype(c, lefts[i]);
          /* An under-filled massign target is Ruby `nil`, not the type's zero
             value; emit the slot's nil sentinel (mirroring the typed-array
             under-fill arm above) so `a, b, c = 1` yields [1, nil, nil]. */
          const char *nilv = nil_sentinel(ltt);
          /* a captured TY_PROC cell needs the raw int-laundered lvalue rather
             than emit_local_ref's non-assignable cast form (see emit_assign). */
          if (emit_proc_cell_lvalue(c, id, lvn, b)) {
            buf_printf(b, "%s);\n", nilv);
          }
          else {
            emit_local_ref(c, id, lvn, b);
            buf_printf(b, " = %s;\n", nilv);
          }
        }
        continue;
      }
      if (lty && sp_streq(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        const char *lvn = nt_str(nt, lefts[i], "name");
        /* cell-aware lvalue: a target captured by a later lambda (doom's
           draw_automap `min_x`/`max_y`, closed over by the to_sx/to_sy
           procs) lives in a heap cell, not a plain lv_ slot -- writing
           lv_<name> referenced an undeclared identifier. A captured TY_PROC
           cell needs the raw int-laundered lvalue (emit_local_ref's cast form
           is not assignable), mirroring emit_assign. */
        int proc_cell = emit_proc_cell_lvalue(c, id, lvn, b);
        if (!proc_cell) { emit_local_ref(c, id, lvn, b); buf_puts(b, " = "); }
        LocalVar *llv = lvn ? scope_local(comp_scope_of(c, id), lvn) : NULL;
        TyKind ltt = llv ? llv->type : comp_ntype(c, lefts[i]);
        TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
        if (proc_cell) {
          /* The cell stores (sp_int)(uintptr_t)sp_Proc*, so the value has to
             be a bare pointer. But a nil/void element was pre-evaluated into a
             boxed sp_RbVal temp, and a poly element into an sp_RbVal too --
             casting that struct to (sp_int) is invalid C. Extract the proc
             pointer (NULL for nil, the .v.p slot for a poly) before laundering,
             mirroring emit_assign's proc-cell write. */
          if (valt == TY_NIL || valt == TY_VOID) buf_puts(b, "NULL");
          else if (valt == TY_POLY) buf_printf(b, "_t%d.v.p", tmps[i]);
          else buf_printf(b, "_t%d", tmps[i]);
        }
        else if (ltt == TY_POLY && valt != TY_POLY) {
          char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, valt, expr, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        if (proc_cell) buf_puts(b, ")");
        buf_puts(b, ";\n");
      }
      else if (lty && (sp_streq(lty, "ConstantPathTargetNode") || sp_streq(lty, "ConstantTargetNode")) &&
               nt_str(nt, lefts[i], "name") && comp_const(c, nt_str(nt, lefts[i], "name"))) {
        emit_indent(b, indent);
        buf_printf(b, "cst_%s = _t%d;\n", nt_str(nt, lefts[i], "name"), tmps[i]);
      }
      else if (lty && sp_streq(lty, "InstanceVariableTargetNode")) {
        const char *ivnm = nt_str(nt, lefts[i], "name");
        if (!ivnm) continue;
        Scope *iv_sc = comp_scope_of(c, id);
        int iv_cid = iv_sc ? iv_sc->class_id : -1;
        if (iv_cid < 0 && g_class_body_id >= 0) iv_cid = g_class_body_id;
        TyKind ivt = TY_UNKNOWN;
        if (iv_cid >= 0) {
          int iv_idx = comp_ivar_index(&c->classes[iv_cid], ivnm);
          if (iv_idx >= 0) ivt = c->classes[iv_cid].ivar_types[iv_idx];
        }
        emit_indent(b, indent);
        if (iv_sc && iv_sc->is_cmethod && iv_cid >= 0)
          buf_printf(b, "civ_%s_%s = ", c->classes[iv_cid].name, ivnm + 1);
        else
          buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(ivnm + 1));
        TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
        if (ivt == TY_POLY && valt != TY_POLY) {
          char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, valt, expr, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        buf_puts(b, ";\n");
      }
      else if (lty && sp_streq(lty, "CallTargetNode")) {
        /* setter call: e.g. @c.v = _t<i> */
        const char *setnm = nt_str(nt, lefts[i], "name");
        int recv_id2 = nt_ref(nt, lefts[i], "receiver");
        size_t snlen = setnm ? strlen(setnm) : 0;
        if (!setnm || snlen < 2 || setnm[snlen - 1] != '=' || recv_id2 < 0)
          { unsupported(c, id, "multiple assignment call target"); continue; }
        TyKind rt2 = comp_ntype(c, recv_id2);
        if (!ty_is_object(rt2))
          { unsupported(c, id, "multiple assignment call target non-object"); continue; }
        char base2[256]; memcpy(base2, setnm, snlen - 1); base2[snlen - 1] = '\0';
        int rc2 = ty_object_class(rt2);
        if (!comp_writer_in_chain(c, rc2, base2, NULL))
          { unsupported(c, id, "multiple assignment call target no writer"); continue; }
        char ivn2[260]; snprintf(ivn2, sizeof ivn2, "@%s", base2);
        int defc2 = -1; comp_writer_in_chain(c, rc2, base2, &defc2);
        int iv2 = comp_ivar_index(&c->classes[defc2 < 0 ? rc2 : defc2], ivn2);
        TyKind ivt2 = iv2 >= 0 ? c->classes[defc2 < 0 ? rc2 : defc2].ivar_types[iv2] : TY_UNKNOWN;
        emit_indent(b, indent);
        buf_puts(b, "("); emit_expr(c, recv_id2, b); buf_printf(b, ")->iv_%s = ", iv_c(base2));
        TyKind valt2 = comp_ntype(c, els[i]);
        if (ivt2 == TY_POLY && valt2 != TY_POLY) {
          char expr2[32]; snprintf(expr2, sizeof expr2, "_t%d", tmps[i]);
          Buf bx2; memset(&bx2, 0, sizeof bx2);
          emit_boxed_text(c, valt2, expr2, &bx2);
          buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        buf_puts(b, ";\n");
      }
      else if (lty && sp_streq(lty, "MultiTargetNode")) {
        /* (b, c) = _t<i>  where _t<i> is a typed array */
        TyKind at = comp_ntype(c, els[i]);
        const char *k = array_kind(at);
        if (!k) { unsupported(c, id, "multiple assignment nested target"); continue; }
        int inn2 = 0;
        const int *inner_lefts = nt_arr(nt, lefts[i], "lefts", &inn2);
        TyKind elemty = sp_streq(k, "Int") ? TY_INT : sp_streq(k, "Float") ? TY_FLOAT
                      : sp_streq(k, "Str") ? TY_STRING : TY_POLY;
        for (int j = 0; j < inn2; j++) {
          const char *ilty2 = inner_lefts ? nt_type(nt, inner_lefts[j]) : NULL;
          if (!ilty2 || !sp_streq(ilty2, "LocalVariableTargetNode")) { unsupported(c, id, "multiple assignment nested target"); continue; }
          const char *inm = nt_str(nt, inner_lefts[j], "name");
          LocalVar *ilv = inm ? scope_local(comp_scope_of(c, inner_lefts[j]), inm) : NULL;
          char getexpr[80]; snprintf(getexpr, sizeof getexpr, "sp_%sArray_get(_t%d, %d)", k, tmps[i], j);
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = ", rename_local(inm));
          /* box the scalar element into a widened (poly) target slot */
          if (ilv && ilv->type == TY_POLY && elemty != TY_POLY)
            emit_boxed_text(c, elemty, getexpr, b);
          else buf_puts(b, getexpr);
          buf_puts(b, ";\n");
        }
      }
      else if (lty && sp_streq(lty, "GlobalVariableTargetNode")) {
        const char *gnm = nt_str(nt, lefts[i], "name");
        const char *rn2 = gnm ? comp_resolve_gvar(c, gnm + 1) : NULL;
        if (!rn2 || !comp_gvar(c, rn2)) { unsupported(c, id, "multiple assignment global target"); continue; }
        emit_indent(b, indent);
        buf_printf(b, "gv_%s = _t%d;\n", rn2, tmps[i]);
      }
      else if (lty && sp_streq(lty, "IndexTargetNode")) {
        int recv_id = nt_ref(nt, lefts[i], "receiver");
        int idx_args = nt_ref(nt, lefts[i], "arguments");
        int idx_argc = 0;
        const int *idx_argv = idx_args >= 0 ? nt_arr(nt, idx_args, "arguments", &idx_argc) : NULL;
        if (recv_id < 0 || idx_argc < 1) { unsupported(c, id, "multiple assignment index target"); continue; }
        TyKind recv_t = comp_ntype(c, recv_id);
        emit_indent(b, indent);
        if (ty_is_array(recv_t)) {
          const char *k = (recv_t == TY_POLY_ARRAY) ? "Poly" : array_kind(recv_t);
          if (!k) k = "Int";
          buf_printf(b, "sp_%sArray_set(", k);
          emit_expr(c, recv_id, b); buf_puts(b, ", ");
          emit_expr(c, idx_argv[0], b); buf_puts(b, ", ");
          if (recv_t == TY_POLY_ARRAY) {
            TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
            char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
            Buf bxi; memset(&bxi, 0, sizeof bxi);
            emit_boxed_text(c, valt, tmp_expr, &bxi);
            buf_puts(b, bxi.p ? bxi.p : "sp_box_nil()"); free(bxi.p);
          }
          else buf_printf(b, "_t%d", tmps[i]);
          buf_puts(b, ");\n");
        }
        else if (recv_t == TY_POLY || recv_t == TY_UNKNOWN) {
          /* A container the fixpoint could not type -- an attr read whose name
             several classes own, so the receiver's class is only known at run
             time -- sets through the boxed index path (#3781). */
          int tmw = ++g_tmp;
          buf_printf(b, "{ sp_RbVal _t%d = ", tmw);
          emit_boxed(c, recv_id, b);
          buf_puts(b, "; sp_poly_arr_set(_t");
          buf_printf(b, "%d, ", tmw);
          emit_int_expr(c, idx_argv[0], b); buf_puts(b, ", ");
          { TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
            char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
            Buf bxi; memset(&bxi, 0, sizeof bxi);
            emit_boxed_text(c, valt, tmp_expr, &bxi);
            buf_puts(b, bxi.p ? bxi.p : "sp_box_nil()"); free(bxi.p); }
          buf_puts(b, "); }\n");
        }
        else if (ty_is_hash(recv_t)) {
          const char *hn = ty_hash_cname(recv_t);
          if (!hn) { unsupported(c, id, "multiple assignment hash index target unknown kind"); continue; }
          buf_printf(b, "sp_%sHash_set(", hn);
          emit_expr(c, recv_id, b); buf_puts(b, ", ");
          if (ty_hash_key(recv_t) == TY_INT) emit_int_expr(c, idx_argv[0], b);
          else emit_expr(c, idx_argv[0], b);
          buf_puts(b, ", ");
          if (recv_t == TY_SYM_POLY_HASH || recv_t == TY_STR_POLY_HASH || recv_t == TY_POLY_POLY_HASH) {
            TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
            char tmp_expr2[32]; snprintf(tmp_expr2, sizeof tmp_expr2, "_t%d", tmps[i]);
            Buf bxi2; memset(&bxi2, 0, sizeof bxi2);
            emit_boxed_text(c, valt, tmp_expr2, &bxi2);
            buf_puts(b, bxi2.p ? bxi2.p : "sp_box_nil()"); free(bxi2.p);
          }
          else buf_printf(b, "_t%d", tmps[i]);
          buf_puts(b, ");\n");
        }
        else { unsupported(c, id, "multiple assignment index target non-array/hash"); }
      }
      else if (lty && sp_streq(lty, "ClassVariableTargetNode")) {
        const char *cnm = nt_str(nt, lefts[i], "name");
        if (!cnm || cnm[0] != '@' || cnm[1] != '@') { unsupported(c, id, "multiple assignment class variable target"); continue; }
        Scope *cv_sc = comp_scope_of(c, id);
        int cv_cid = (cv_sc && cv_sc->class_id >= 0) ? cv_sc->class_id : g_class_body_id;
        if (cv_cid < 0) { unsupported(c, id, "multiple assignment class variable target no class"); continue; }
        int cv_idx = comp_cvar_index(&c->classes[cv_cid], cnm);
        if (cv_idx < 0) { unsupported(c, id, "multiple assignment class variable target unregistered"); continue; }
        emit_indent(b, indent);
        buf_printf(b, "cvar_%s_%s = ", c->classes[cv_cid].name, cnm + 2);
        TyKind cvt = c->classes[cv_cid].cvar_types[cv_idx];
        TyKind valt = tmpts ? tmpts[i] : comp_ntype(c, els[i]);
        if (cvt == TY_POLY && valt != TY_POLY) {
          char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, valt, expr, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
        buf_puts(b, ";\n");
      }
      else unsupported(c, id, "multiple assignment target");
    }
    /* build and assign rest (splat) target */
    if (rest_var) {
      int rstart = ln, rend = en - rn;
      if (rend < rstart) rend = rstart;
      Scope *rscope = comp_scope_of(c, id);
      LocalVar *rlv = scope_local(rscope, rest_var);
      TyKind rest_arr_t = rlv ? rlv->type : TY_INT_ARRAY;
      if (!ty_is_array(rest_arr_t)) rest_arr_t = TY_INT_ARRAY;
      const char *k = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
      if (!k) k = "Int";
      int tr = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
      if (rest_arr_t == TY_POLY_ARRAY) {
        for (int i = rstart; i < rend; i++) {
          TyKind et = comp_ntype(c, els[i]);
          char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, et, tmp_expr, &bx);
          emit_indent(b, indent);
          buf_printf(b, "sp_PolyArray_push(_t%d, %s);\n", tr, bx.p ? bx.p : "sp_box_nil()");
          free(bx.p);
        }
      }
      else {
        for (int i = rstart; i < rend; i++) {
          emit_indent(b, indent);
          buf_printf(b, "sp_%sArray_push(_t%d, _t%d);\n", k, tr, tmps[i]);
        }
      }
      emit_indent(b, indent);
      buf_printf(b, "lv_%s = _t%d;\n", rename_local(rest_var), tr);
    }
    if (rest_gvar && comp_gvar(c, rest_gvar)) {
      int rstart = ln, rend = en - rn;
      if (rend < rstart) rend = rstart;
      LocalVar *glv_r = comp_gvar(c, rest_gvar);
      TyKind rest_arr_t = glv_r ? glv_r->type : TY_INT_ARRAY;
      if (!ty_is_array(rest_arr_t)) rest_arr_t = TY_INT_ARRAY;
      const char *k = (rest_arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(rest_arr_t);
      if (!k) k = "Int";
      int tr = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
      for (int i = rstart; i < rend; i++) {
        emit_indent(b, indent);
        buf_printf(b, "sp_%sArray_push(_t%d, _t%d);\n", k, tr, tmps[i]);
      }
      emit_indent(b, indent);
      buf_printf(b, "gv_%s = _t%d;\n", rest_gvar, tr);
    }
    /* assign rights (post-splat fixed targets). They fill left-to-right starting
       just past the splat's actual length (max(0, en-ln-rn)); a target whose
       source index runs off the end lands nil (`a, *b, c, d = [1, 2]` ->
       c=2, d=nil) instead of reusing a leading element. */
    int blen_r = en - ln - rn; if (blen_r < 0) blen_r = 0;
    for (int j = 0; j < rn; j++) {
      int ridx = ln + blen_r + j;
      if (ridx >= en) ridx = -1;
      const char *lty = nt_type(nt, rights[j]);
      if (!lty) continue;
      const char *rnm_j = nt_str(nt, rights[j], "name");
      if (sp_streq(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        LocalVar *rjlv = rnm_j ? scope_local(comp_scope_of(c, id), rnm_j) : NULL;
        int rjpoly = rjlv && rjlv->type == TY_POLY;
        if (ridx >= 0 && ridx < en) {
          buf_printf(b, "lv_%s = ", rename_local(rnm_j));
          TyKind valt = comp_ntype(c, els[ridx]);
          if (rjpoly && valt != TY_POLY) {
            char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[ridx]);
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, valt, expr, &bx);
            buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_printf(b, "_t%d", tmps[ridx]);
          buf_puts(b, ";\n");
        }
        else {
          buf_printf(b, "lv_%s = ", rename_local(rnm_j));
          TyKind tt = comp_ntype(c, rights[j]);
          if (rjpoly) {
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, tt, default_value(tt), &bx);
            buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(b, default_value(tt));
          buf_puts(b, ";\n");
        }
      }
      else if ((sp_streq(lty, "ConstantPathTargetNode") || sp_streq(lty, "ConstantTargetNode")) &&
               rnm_j && comp_const(c, rnm_j)) {
        emit_indent(b, indent);
        if (ridx >= 0 && ridx < en) {
          buf_printf(b, "cst_%s = _t%d;\n", rnm_j, tmps[ridx]);
        }
      }
      else if (sp_streq(lty, "InstanceVariableTargetNode") && rnm_j) {
        Scope *iv_sc2 = comp_scope_of(c, id);
        int iv_cid2 = iv_sc2 ? iv_sc2->class_id : -1;
        TyKind ivt2 = TY_UNKNOWN;
        if (iv_cid2 >= 0) {
          int iv_idx2 = comp_ivar_index(&c->classes[iv_cid2], rnm_j);
          if (iv_idx2 >= 0) ivt2 = c->classes[iv_cid2].ivar_types[iv_idx2];
        }
        emit_indent(b, indent);
        if (iv_sc2 && iv_sc2->is_cmethod && iv_cid2 >= 0)
          buf_printf(b, "civ_%s_%s = ", c->classes[iv_cid2].name, rnm_j + 1);
        else
          buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, iv_c(rnm_j + 1));
        if (ridx >= 0 && ridx < en) {
          TyKind valt2 = (ridx < en) ? comp_ntype(c, els[ridx]) : TY_UNKNOWN;
          if (ivt2 == TY_POLY && valt2 != TY_POLY) {
            char expr2[32]; snprintf(expr2, sizeof expr2, "_t%d", tmps[ridx]);
            Buf bx2; memset(&bx2, 0, sizeof bx2);
            emit_boxed_text(c, valt2, expr2, &bx2);
            buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
          }
          else buf_printf(b, "_t%d", tmps[ridx]);
        }
        else buf_puts(b, default_value(ivt2 != TY_UNKNOWN ? ivt2 : TY_INT));
        buf_puts(b, ";\n");
      }
    }
    return;
  }
  if (sp_streq(ty, "SingletonClassNode")) {
    /* `class << self` / `class << Const`: the inner `def`s were registered as
       class methods during scope analysis and are emitted from the method
       list, so a supported node produces no code here (matching the skip used
       inside a class body). A block on an arbitrary object (`class << obj`) has
       no per-object singleton dispatch and is rejected loudly. */
    int sexpr = nt_ref(nt, id, "expression");
    const char *exty = sexpr >= 0 ? nt_type(nt, sexpr) : NULL;
    if (exty && sp_streq(exty, "SelfNode")) return;
    if (exty && sp_streq(exty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, sexpr, "name");
      if (cn && comp_class_index(c, cn) >= 0) return;
    }
    /* `class << obj` on a statically-traceable instance: the inner defs were
       reattached to a synthesized singleton subclass (register_singleton_defs)
       and are emitted from the method list, so the block is compile-time. */
    if (exty && (sp_streq(exty, "ConstantReadNode") || sp_streq(exty, "LocalVariableReadNode"))) {
      TyKind et = comp_ntype(c, sexpr);
      if (ty_is_object(et) && c->classes[ty_object_class(et)].is_singleton_of) {
        emit_sg_activate(c, id, sexpr, b, indent);
        return;
      }
    }
    unsupported(c, id, "singleton class on arbitrary object");
  }
  if (sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) {
    /* Run the body's side-effecting statements at the definition site
       (top-to-bottom, like CRuby). Method/attr/alias declarations are
       handled elsewhere; everything else (puts, constant writes, nested
       class/module bodies) executes inline here. */
    int cp = nt_ref(nt, id, "constant_path");
    const char *cname = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    int saved_cbi = g_class_body_id;
    if (cname) g_class_body_id = comp_class_index(c, cname);
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    for (int k = 0; k < n; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty) continue;
      if (sp_streq(sty, "DefNode") || sp_streq(sty, "AliasMethodNode") ||
          sp_streq(sty, "SingletonClassNode")) continue;
      /* A receiver-less call in a class body is, by default, a declaration
         macro (attr_*, include, private, an FFI/DSL directive) -- skip it.
         Only run the genuine side-effecting ones: output calls and calls
         that resolve to a user-defined method. */
      if (sp_streq(sty, "CallNode") && nt_ref(nt, stmts[k], "receiver") < 0) {
        const char *cn = nt_str(nt, stmts[k], "name");
        /* reflection-mutation macros can't take effect (methods/class vars are
           static): report the documented limit rather than skip silently
           (#2954, #2955). diagnose no-ops when the class defines its own. */
        if (cn && (sp_streq(cn, "remove_method") || sp_streq(cn, "undef_method") ||
                   sp_streq(cn, "remove_class_variable")) &&
            diagnose_unsupported_call(c, stmts[k])) return;
        int is_output = cn && (sp_streq(cn, "puts") || sp_streq(cn, "print") || sp_streq(cn, "p"));
        int is_user = cn && comp_method_index(c, cn) >= 0;
        /* self in a class body is the class, so a receiver-less call naming one
           of its class methods (its own or an ancestor's) is a real call, not a
           declaration macro -- the shape every declarative DSL uses (`key :a`,
           `validates :name`). Skipping it left the state it establishes unset
           and nothing said so (#4051). */
        if (!is_user && cn && g_class_body_id >= 0)
          is_user = comp_cmethod_in_chain(c, g_class_body_id, cn, NULL) >= 0;
        if (!is_output && !is_user) continue;
      }
      emit_stmt(c, stmts[k], b, indent);
    }
    g_class_body_id = saved_cbi;
    return;
  }
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode")) {
    if (!emit_super_inline(c, id, b, indent, 0)) {
      emit_indent(b, indent); emit_super(c, id, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (sp_streq(ty, "IndexOperatorWriteNode")) { emit_index_op_write(c, id, b, indent); return; }
  if (sp_streq(ty, "IndexAndWriteNode")) { emit_index_and_or_write(c, id, b, indent, 0); return; }
  if (sp_streq(ty, "IndexOrWriteNode"))  { emit_index_and_or_write(c, id, b, indent, 1); return; }
  if (sp_streq(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 0); return; }
  if (sp_streq(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 0); return; }
  /* A `break` directly inside a Ruby while/until/for targets that loop, not an
     enclosing break-wrapped iterator: suppress the longjmp lowering for the
     loop body (a nested iterator inside re-establishes its own scope). */
  if (sp_streq(ty, "WhileNode"))  { const char *sv = g_brk_ser_var; g_brk_ser_var = NULL; emit_while(c, id, b, indent, 0); g_brk_ser_var = sv; return; }
  if (sp_streq(ty, "UntilNode"))  { const char *sv = g_brk_ser_var; g_brk_ser_var = NULL; emit_while(c, id, b, indent, 1); g_brk_ser_var = sv; return; }
  if (sp_streq(ty, "ForNode"))    { const char *sv = g_brk_ser_var; g_brk_ser_var = NULL; emit_for(c, id, b, indent); g_brk_ser_var = sv; return; }
  if (sp_streq(ty, "BreakNode")) {
    if (g_brk_ser_var) {
      /* break from a block: deliver the value to the enclosing wrapper. With
         no intervening ensure frames this is a same-function `goto` -- no
         longjmp, so register-allocated locals mutated in the block keep
         their values. Ensure-crossing breaks longjmp via sp_brk_throw so the
         ensure bodies run (accepting the catch/throw-class register hazard). */
      int bargs = nt_ref(nt, id, "arguments");
      int bvargc = 0; const int *bvargs = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &bvargc) : NULL;
      int brk_goto = (g_ensure_depth == g_brk_ensure_base) &&
                     (g_exc_frame_depth == g_brk_exc_base) &&
                     strncmp(g_brk_ser_var, "_brkser", 7) == 0;
      const char *sfx = brk_goto ? g_brk_ser_var + 7 : NULL;   /* wrapper temp id */
      emit_indent(b, indent);
      /* leaving the block pops the handler for every rescue body opened inside
         it; the throw longjmps, so pop before it (the value persists). */
      if (!brk_goto) emit_cur_exc_restore(b, g_brk_exc_base);
      if (brk_goto) buf_printf(b, "sp_brk_val[_brkslot%s - 1] = ", sfx);
      else buf_printf(b, "sp_brk_throw(%s, ", g_brk_ser_var);
      if (bvargc == 0) buf_puts(b, "sp_box_nil()");
      else if (bvargc == 1) emit_boxed(c, bvargs[0], b);
      else {
        /* `break a, b, ...` returns an array of the values */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
        for (int k = 0; k < bvargc; k++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, bvargs[k], b); buf_puts(b, "); ");
        }
        buf_printf(b, "sp_box_poly_array(_t%d); })", t);
      }
      if (brk_goto) { buf_puts(b, "; "); emit_cur_exc_restore(b, g_brk_exc_base); buf_printf(b, "goto _brklbl%s;\n", sfx); }
      else buf_puts(b, ");\n");
      return;
    }
    /* break inside a lambda body: a return from the lambda (CRuby). A break-
       capable lambda's ret is widened to poly, so the value returns through
       the proc ABI's _sp_proc_poly_ret slot (g_result_var) with `return 0`;
       ensure deferral still applies through emit_return otherwise. Inside a
       C loop within the lambda (`loop { break }`) the break exits THAT loop,
       not the lambda -- fall through to the plain C break below. */
    if (g_proc_body_kind == 1 && g_c_loop_depth == 0) {
      if (g_result_var && g_ensure_depth == 0) {
        int bargs = nt_ref(nt, id, "arguments");
        int bvargc = 0; const int *bvargs = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &bvargc) : NULL;
        emit_indent(b, indent);
        buf_printf(b, "{ %s = ", g_result_var);
        if (bvargc == 0) buf_puts(b, "sp_box_nil()");
        else if (bvargc == 1) emit_boxed(c, bvargs[0], b);
        else {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
          for (int k = 0; k < bvargc; k++) {
            buf_printf(b, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, bvargs[k], b); buf_puts(b, "); ");
          }
          buf_printf(b, "sp_box_poly_array(_t%d); })", t);
        }
        buf_puts(b, "; return 0; }\n");
        return;
      }
      emit_return(c, id, b, indent);
      return;
    }
    /* break inside a non-lambda proc body: throw to the captured creating
       scope's serial; a dead/foreign scope raises LocalJumpError. A C loop
       inside the proc owns its own break, as above. */
    if (g_proc_body_kind == 2 && g_c_loop_depth == 0 && g_proc_brk_home) {
      int bargs = nt_ref(nt, id, "arguments");
      int bvargc = 0; const int *bvargs = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &bvargc) : NULL;
      emit_indent(b, indent); buf_printf(b, "sp_brk_throw(%s, ", g_proc_brk_home);
      if (bvargc == 0) buf_puts(b, "sp_box_nil()");
      else if (bvargc == 1) emit_boxed(c, bvargs[0], b);
      else {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
        for (int k = 0; k < bvargc; k++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, bvargs[k], b); buf_puts(b, "); ");
        }
        buf_printf(b, "sp_box_poly_array(_t%d); })", t);
      }
      buf_puts(b, ");\n");
      return;
    }
    if (g_loop_break_var) {
      int bargs = nt_ref(nt, id, "arguments");
      int bvargc = 0; const int *bvargs = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &bvargc) : NULL;
      if (bvargc > 0) {
        emit_indent(b, indent); buf_printf(b, "%s = ", g_loop_break_var);
        if (g_ie_res_poly) emit_boxed(c, bvargs[0], b); else emit_expr(c, bvargs[0], b);
        buf_puts(b, ";\n");
      }
    }
    else {
      /* the loop's value is unused, but Ruby still EVALUATES the break
         value expression for its side effects (`break kept.concat(list)`,
         #3297): run each argument for effect before leaving. */
      int bargs = nt_ref(nt, id, "arguments");
      int bvargc = 0; const int *bvargs = bargs >= 0 ? nt_arr(nt, bargs, "arguments", &bvargc) : NULL;
      for (int k = 0; k < bvargc; k++) {
        emit_indent(b, indent);
        buf_puts(b, "(void)(");
        emit_boxed(c, bvargs[k], b);
        buf_puts(b, ");\n");
      }
    }
    emit_indent(b, indent);
    /* leaving through live begin/rescue frames opened inside the loop body:
       pop them, or their jmp_bufs dangle (same accounting as emit_return) */
    emit_frame_unwind(b, g_loop_exc_base, NULL);
    buf_puts(b, "break;\n"); return;
  }
  if (sp_streq(ty, "NextNode")) {
    /* `next` at C-loop depth 0 inside a _proc_N function is the proc's own
       return (Ruby block semantics: next leaves the block with its value),
       not a loop continue -- there is no enclosing C loop, and emitting
       `continue` there is invalid C. Route it through the proc's return ABI:
       the poly slot when one is active, else the direct sp_int carrier. */
    if (g_in_proc_body && g_c_loop_depth == 0) {
      int nargs = nt_ref(nt, id, "arguments");
      int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
      if (g_result_var && g_result_poly) {
        emit_indent(b, indent); buf_printf(b, "%s = ", g_result_var);
        if (nvc > 0) emit_boxed(c, nv[0], b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ";\n");
        emit_indent(b, indent); buf_puts(b, "return 0;\n");
      }
      else if (nvc > 0 && (g_ret_type == TY_INT || g_ret_type == TY_BOOL || g_ret_type == TY_SYMBOL)) {
        emit_indent(b, indent); buf_puts(b, "return ");
        emit_expr(c, nv[0], b); buf_puts(b, ";\n");
      }
      else if (nvc > 0 && proc_slot_is_ptr(g_ret_type)) {
        emit_indent(b, indent); buf_puts(b, "return (sp_int)(uintptr_t)(");
        emit_expr(c, nv[0], b); buf_puts(b, ");\n");
      }
      else if (nvc > 0) {
        /* untypable slot: evaluate for effects, return nil */
        emit_indent(b, indent); buf_puts(b, "(void)(");
        emit_expr(c, nv[0], b); buf_puts(b, ");\n");
        emit_indent(b, indent); buf_puts(b, "return 0;\n");
      }
      else { emit_indent(b, indent); buf_puts(b, "return 0;\n"); }
      return;
    }
    if (g_ie_next_var) {
      int nargs = nt_ref(nt, id, "arguments");
      int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
      if (nvc > 0) {
        /* An empty `[]` / `{}` carries no kind of its own, and the destination
           is the block's own value slot: build the literal at THAT kind, or the
           default one lands in it as the wrong struct (#3978). */
        if (g_ie_next_ty != TY_UNKNOWN) {
          int vn9 = nv[0];
          /* `next {}` needs the parens to parse as a hash at all, so the
             literal arrives wrapped. */
          while (nt_kind(nt, vn9) == NK_ParenthesesNode) {
            int pb9 = nt_ref(nt, vn9, "body"); int pn9 = 0;
            const int *pp9 = pb9 >= 0 ? nt_arr(nt, pb9, "body", &pn9) : NULL;
            if (pn9 != 1) break;
            vn9 = pp9[0];
          }
          NodeKind vk9 = nt_kind(nt, vn9);
          int ven9 = 0;
          if (vk9 == NK_ArrayNode || vk9 == NK_HashNode || vk9 == NK_KeywordHashNode)
            nt_arr(nt, vn9, "elements", &ven9);
          if (ven9 == 0 &&
              ((vk9 == NK_ArrayNode && ty_is_array(g_ie_next_ty)) ||
               ((vk9 == NK_HashNode || vk9 == NK_KeywordHashNode) && ty_is_hash(g_ie_next_ty)))) {
            c->ntype[vn9] = g_ie_next_ty;
            if (vn9 != nv[0]) c->ntype[nv[0]] = g_ie_next_ty;
          }
        }
        emit_indent(b, indent); buf_printf(b, "%s = ", g_ie_next_var);
        if (g_ie_res_poly) emit_boxed(c, nv[0], b); else emit_expr(c, nv[0], b);
        buf_puts(b, ";\n");
      }
    }
    /* `next` crossing begin..ensure regions opened INSIDE this loop must run
       their ensure bodies (CRuby). Defer through the innermost ensure label
       with the next flag; the ensure tail chains outward and finally emits
       the continue. Regions opened OUTSIDE the loop are not left by a next. */
    if (g_ensure_depth > g_loop_ensure_base) {
      EnsureCtx *nctx = &g_ensure_stack[g_ensure_depth - 1];
      int npops = g_exc_frame_depth - nctx->exc_base;
      if (npops < 1) npops = 1;
      emit_indent(b, indent);
      buf_puts(b, "{ ");
      emit_cur_exc_restore(b, nctx->exc_base);
      buf_printf(b, "_nxtf%d = 1; sp_exc_top -= %d; goto _ensure%d; }\n",
                 nctx->lid, npops, nctx->lid);
      return;
    }
    emit_indent(b, indent);
    emit_frame_unwind(b, g_loop_exc_base, NULL);
    buf_puts(b, "continue;\n"); return;
  }
  if (sp_streq(ty, "RedoNode"))   {
    emit_indent(b, indent);
    if (g_redo_depth > 0) buf_printf(b, "goto _redo_%d;\n", g_redo_stack[g_redo_depth - 1]);
    else buf_puts(b, "continue;\n");  /* redo outside a labeled loop: best-effort */
    return;
  }
  if (sp_streq(ty, "RetryNode")) {
    if (g_retry_label) {
      emit_indent(b, indent);
      /* leaving this rescue body back to its begin: pop its handler (exactly the
         innermost rescue save). */
      if (g_rescue_save_depth > 0)
        buf_puts(b, "sp_rescue_sp--; ");
      buf_printf(b, "goto %s;\n", g_retry_label);
    }
    else unsupported(c, id, "retry (outside rescue)");
    return;
  }
  if (sp_streq(ty, "CaseNode"))      { emit_case(c, id, b, indent); return; }
  if (sp_streq(ty, "CaseMatchNode")) { emit_case_match(c, id, b, indent, 0, -1); return; }
  if (sp_streq(ty, "BeginNode"))  { emit_begin(c, id, b, indent, NULL); return; }
  if (sp_streq(ty, "RescueModifierNode")) {
    /* `expr rescue fallback` as a statement: run expr under a setjmp guard,
       fall through to the rescue expression on any exception. */
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    if (e >= 0) emit_stmt(c, e, b, indent + 1);
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n");
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) sp_proc_homes_unwind();\n");
    /* A non-local unwind only passes through (no ensure here): continue it. */
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
    /* $! and #cause threading inside the fallback, like a full rescue arm */
    {
      int tce = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_Exception *_t%d = sp_exc_obj[sp_exc_top] ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                    " : sp_exc_new_for_catch(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n", tce);
      emit_indent(b, indent + 1);
      buf_printf(b, "_t%d->cause = (sp_Exception *)sp_pending_cause; sp_pending_cause = NULL;\n", tce);
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_rescue_push((void *)_t%d);\n", tce);
    }
    if (r >= 0) emit_stmt(c, r, b, indent + 1);
    emit_indent(b, indent + 1); buf_puts(b, "sp_rescue_sp--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  if (sp_streq(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  if (sp_streq(ty, "DefNode")) {
    /* `def obj.m` is emitted separately as a method -- but it is also the
       STATEMENT where the singleton starts existing, and Ruby dates the
       override from here rather than from the object's construction (#4084).
       The object carries its parent's cls_id until this line runs. */
    if (getenv("SP_DBG_SG")) { emit_indent(b, indent); buf_printf(b, "/* dbg DefNode recv=%d */\n", nt_ref(c->nt, id, "receiver")); }
    emit_sg_activate(c, id, nt_ref(c->nt, id, "receiver"), b, indent);
    return;
  }
  if (sp_streq(ty, "UndefNode"))  { return; } /* resolved at scan time */
  if (sp_streq(ty, "AliasGlobalVariableNode")) { return; } /* resolved at scan time */
  if (sp_streq(ty, "PreExecutionNode") || sp_streq(ty, "PostExecutionNode")) { return; } /* hoisted separately */

  /* any remaining value expression as a bare statement (its value is used
     only when this is the last statement of an inlined expr method) */
  emit_indent(b, indent);
  emit_expr(c, id, b);
  buf_puts(b, ";\n");
}

/* The base receiver of a string append chain: `s << a << b` bottoms out at
   `s` (unwrapping parens); a plain `s << x` yields its receiver unchanged.
   Mirrors the chain walks in emit_array_mutate_stmt, so after the statement
   form has run a tail `<<` can return the base instead of re-evaluating the
   inner links (which would append their arguments a second time). */
static int str_append_chain_base(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  int cur = id;
  for (;;) {
    while (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "ParenthesesNode")) {
      int pb = nt_ref(nt, cur, "body");
      if (pb < 0) break;
      int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
      if (bn != 1) break;
      cur = bb[0];
    }
    const char *cty = nt_type(nt, cur);
    if (!cty || !sp_streq(cty, "CallNode")) return cur;
    const char *cnm = nt_str(nt, cur, "name");
    int crecv = nt_ref(nt, cur, "receiver");
    if (!cnm || (!sp_streq(cnm, "<<") && !sp_streq(cnm, "concat")) || crecv < 0) return cur;
    int cargs = nt_ref(nt, cur, "arguments");
    int cac = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &cac);
    if (cac != 1) return cur;
    cur = crecv;
  }
}

/* Tail position: the value of this statement is the method's return value. */
/* A block-taking call whose value is its receiver -- `each`, `each_pair`,
   `times`, `upto` and the rest -- emitted as a loop, so wherever that value is
   wanted the receiver has to be put back afterwards. Returns the receiver's
   node when this call is one AND the receiver is a plain read (it is re-read,
   not re-evaluated, so anything with a side effect is out), else -1. */
int tail_iter_receiver(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") < 0) return -1;
  const char *nm = nt_str(nt, id, "name");
  if (!nm) return -1;
  static const char *iter_ret_recv[] = {
    "each", "each_pair", "each_key", "each_value", "each_with_index",
    "each_index", "each_byte", "each_entry", "reverse_each", "each_slice",
    "upto", "downto", "step", "times", NULL
  };
  int hit = 0;
  for (int i = 0; iter_ret_recv[i]; i++)
    if (sp_streq(nm, iter_ret_recv[i])) { hit = 1; break; }
  if (!hit) return -1;
  int r = nt_ref(nt, id, "receiver");
  const char *rt = r >= 0 ? nt_type(nt, r) : NULL;
  if (!rt) return -1;
  if (!sp_streq(rt, "LocalVariableReadNode") && !sp_streq(rt, "InstanceVariableReadNode") &&
      !sp_streq(rt, "SelfNode")) return -1;
  return r;
}

void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "tail statement (no type)");

  if (sp_streq(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 1); return; }
  if (sp_streq(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 1); return; }
  if (sp_streq(ty, "CaseMatchNode")) { emit_case_match(c, id, b, indent, 1, -1); return; }
  if (sp_streq(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  /* a tail `retry` diverges (goto back to its begin); the value is dead */
  if (sp_streq(ty, "RetryNode")) { emit_stmt_inner(c, id, b, indent); return; }
  /* a tail `break` in a lambda/proc body diverges (return / brk-throw): emit
     it as a statement, like `raise` below; the fall-through value is dead. */
  if (sp_streq(ty, "BreakNode") && g_proc_body_kind != 0) {
    emit_stmt_inner(c, id, b, indent);
    return;
  }
  /* `raise` / `throw` diverge -- no value to return; emit as a plain statement
     (throw unwinds to its catch, so it never falls through with a value; #3087). */
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
      nt_str(nt, id, "name") &&
      (sp_streq(nt_str(nt, id, "name"), "raise") || sp_streq(nt_str(nt, id, "name"), "throw"))) {
    emit_indent(b, indent); emit_expr(c, id, b); buf_puts(b, ";\n");
    return;
  }
  if (sp_streq(ty, "BeginNode")) {
    /* begin/rescue value -> a temp, assigned in both branches, then tail */
    TyKind rt = comp_ntype(c, id);
    if (is_scalar_ret(rt)) {
      int t = ++g_tmp;
      char rv[32]; snprintf(rv, sizeof rv, "_t%d", t);
      emit_indent(b, indent); emit_ctype(c, rt, b);
      buf_printf(b, " _t%d = %s;\n", t, rt == TY_RANGE ? "(sp_Range){0}" : default_value(rt));
      int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
      emit_begin(c, id, b, indent, rv);
      g_result_poly = sp;
      emit_indent(b, indent); emit_tail_lead(b);
      /* the begin's scalar result temp feeds a poly tail slot (return type or an
         outer poly result var widened under promote): box it to match. */
      int target_poly = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
      if (target_poly && rt != TY_POLY) {
        char ex[24]; snprintf(ex, sizeof ex, "_t%d", t);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, rt, ex, &bx);
        buf_printf(b, "%s;\n", bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
      }
      else buf_printf(b, "_t%d;\n", t);
      return;
    }
    /* Non-scalar (e.g. TY_VOID when body diverges with raise or return):
       emit as a plain statement; any `return` inside uses deferred mechanism. */
    emit_begin(c, id, b, indent, NULL);
    return;
  }

  /* statements that don't produce a usable tail value: emit normally; the
     trailing default return covers the method's value. Local/instance operator
     assignments (`x += 1`, `@x += 1`) are NOT here -- in Ruby they return the
     updated value, so they fall through to the value path and are returned
     (matz/spinel#1484, matching the class-variable form that already worked).
     Plain `x = v` / `||=` / `&&=` stay statements for now: routing a tail ivar
     write of nil through the value path perturbs nullable-scalar inference. */
  /* A tail ivar write of an OBJECT value (`def m = @x = build`) is the
     method's return value in Ruby: returning the statement default (NULL)
     instead hands callers a null object (#3317). Route it through the
     generic value path below (whose return-slot conversions apply); other
     value shapes keep the statement form -- routing a scalar/nil tail ivar
     write through the value path perturbs nullable-scalar inference, per
     the note below. */
  int _iv_tail_val = 0;
  if (sp_streq(ty, "InstanceVariableWriteNode")) {
    TyKind _ivt9 = comp_ntype(c, nt_ref(nt, id, "value"));
    if (ty_is_object(_ivt9)) _iv_tail_val = 1;
  }
  /* A tail ivar write of a CONTAINER or scalar value is the method's value in
     Ruby just as the object case above is; returning the statement default
     handed back the slot type's nil -- NULL for an Array (segfaulting the
     caller on the first `.length`), 0 for an Integer, "" for a String, {} for
     a Hash. #3317 routed only the object case through the value path, and the
     endless-def form `def m = (@x = v)` is parsed as a value so it was already
     correct; the ordinary `def m; @x = v; end` was not.

     Route it the way the tail LOCAL write below is routed rather than through
     the value path: emitting the write itself as a value is what perturbs the
     nullable-scalar inference the note above guards. Emit the write as a
     statement, then hand back the SLOT -- same value, evaluated once, no
     inference change -- and only when the slot's type reaches the return slot
     without a conversion this text form cannot express. */
  if (sp_streq(ty, "InstanceVariableWriteNode") && !_iv_tail_val) {
    const char *inm9 = nt_str(nt, id, "name");
    Scope *ics9 = comp_scope_of(c, id);
    /* Only the plain instance-slot form (`self->iv_x`) is handled here; the
       class-level, instance_eval and toplevel-global renderings each have
       their own slot text, so leave those on the statement path. */
    int plain9 = ics9 && ics9->class_id >= 0 && !ics9->is_cmethod &&
                 g_ie_class_id < 0 && g_class_body_id < 0;
    if (plain9 && inm9) {
      int iidx9 = comp_ivar_index(&c->classes[ics9->class_id], inm9);
      TyKind it9 = iidx9 >= 0 ? c->classes[ics9->class_id].ivar_types[iidx9] : TY_UNKNOWN;
      int want_poly8 = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
      int slot_ok8 = iidx9 >= 0 && it9 != TY_UNKNOWN && it9 != TY_VOID &&
                     (want_poly8 || it9 == (g_result_var ? g_result_ty : g_ret_type));
      if (slot_ok8) {
        emit_stmt(c, id, b, indent);
        emit_indent(b, indent); emit_tail_lead(b);
        char islot9[512];
        snprintf(islot9, sizeof islot9, "%s%siv_%s", g_self, g_self_deref, iv_c(inm9 + 1));
        if (want_poly8 && it9 != TY_POLY) {
          Buf bx8; memset(&bx8, 0, sizeof bx8);
          emit_boxed_text(c, it9, islot9, &bx8);
          buf_printf(b, "%s;\n", bx8.p ? bx8.p : "sp_box_nil()");
          free(bx8.p);
        }
        else buf_printf(b, "%s;\n", islot9);
        return;
      }
    }
  }
  /* A tail LOCAL write is the method's value in Ruby (`def m; x = v; end`
     answers v), and returning the statement default instead answered "" for a
     String method and 0 for an Integer one (#3388). Routing the write itself
     through the value path is what perturbs the inference the note above
     guards, so emit the write as a statement and then hand back the SLOT:
     same value, evaluated once, no inference change. Only when the slot's
     type reaches the return slot without a conversion this text form cannot
     express -- otherwise keep the old statement-only shape. */
  if (sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") ||
      sp_streq(ty, "LocalVariableAndWriteNode")) {
    const char *lnm = nt_str(nt, id, "name");
    LocalVar *lv9 = lnm ? scope_local(comp_scope_of(c, id), lnm) : NULL;
    TyKind lt9 = lv9 ? lv9->type : TY_UNKNOWN;
    int want_poly9 = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
    int slot_ok = lv9 && lt9 != TY_UNKNOWN && lt9 != TY_VOID &&
                  (want_poly9 || lt9 == (g_result_var ? g_result_ty : g_ret_type));
    emit_stmt(c, id, b, indent);
    if (!slot_ok) return;
    Buf rb9; memset(&rb9, 0, sizeof rb9); emit_local_ref(c, id, lnm, &rb9);
    emit_indent(b, indent); emit_tail_lead(b);
    if (want_poly9 && lt9 != TY_POLY) {
      Buf bx9; memset(&bx9, 0, sizeof bx9);
      emit_boxed_text(c, lt9, rb9.p ? rb9.p : "0", &bx9);
      buf_printf(b, "%s;\n", bx9.p ? bx9.p : "sp_box_nil()");
      free(bx9.p);
    }
    else buf_printf(b, "%s;\n", rb9.p ? rb9.p : "0");
    free(rb9.p);
    return;
  }
  if ((sp_streq(ty, "InstanceVariableWriteNode") && !_iv_tail_val) ||
      sp_streq(ty, "GlobalVariableWriteNode") ||
      sp_streq(ty, "ConstantWriteNode") ||
      sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") ||
      (sp_streq(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
       emit_output_call(c, id, b, indent))) {
    if (!sp_streq(ty, "CallNode")) emit_stmt(c, id, b, indent);
    return;
  }

  /* A local operator-assignment whose target is a captured/cell var (inside a
     proc/block body) has no value form in emit_expr -- cells are int-restricted
     and statement-only -- so keep emitting it as a statement; the enclosing
     callable returns its default. Ordinary locals/ivars fall through below. */
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? scope_local(comp_scope_of(c, id), nm) : NULL;
    int celled = (lv && lv->is_cell) || (g_cap_struct && g_cap_names && nm && nameset_has(g_cap_names, nm));
    if (celled) {
      /* Inside a proc/lambda body the cell op-write must still return the
         POST-assignment value (`->{ n += 1 }.call` is `n+1`, not the stale
         slot). Emit the mutation as a statement, then read the cell back and
         publish it into the proc's result slot. Outside a proc body keep the
         plain-statement form (the method's default return covers the value). */
      if (g_in_proc_body && g_result_var) {
        emit_stmt(c, id, b, indent);
        TyKind ct = lv ? lv->type : TY_INT;
        Buf rb; memset(&rb, 0, sizeof rb); emit_local_ref(c, id, nm, &rb);
        emit_indent(b, indent); buf_printf(b, "%s = ", g_result_var);
        if (g_result_poly && ct != TY_POLY) {
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, ct, rb.p ? rb.p : "0", &bx);
          buf_printf(b, "%s;\n", bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else {
          buf_printf(b, "%s;\n", rb.p ? rb.p : "0");
        }
        free(rb.p);
        return;
      }
      emit_stmt(c, id, b, indent); return;
    }
  }
  /* iteration calls with a block are side-effect statements at tail position;
     emit them without wrapping in a return (the method returns nil implicitly).
     A break-carrying iterator is the exception: its value (the break value, or
     the normal result if no break is taken) IS the method's return, so it must
     take the value path below (the break wrapper in emit_call), not the
     statement form here. */
  /* Kernel#loop {} at tail position: a valued `break` is the method's return,
     so route it through the value handler (emit_call) rather than the statement
     form, which would drop the break value and return nil. */
  int is_tail_loop = sp_streq(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
                     nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), "loop") &&
                     nt_ref(nt, id, "block") >= 0;
  /* tap / then / yield_self at tail position carry a value (the receiver /
     the block's value) that IS the method's return -- the statement form
     would drop it and return nil. Route them to the value path below. */
  const char *tv_name = nt_str(nt, id, "name");
  int is_tail_valued = sp_streq(ty, "CallNode") && tv_name &&
                       nt_ref(nt, id, "block") >= 0 &&
                       (sp_streq(tv_name, "tap") ||
                        sp_streq(tv_name, "then") ||
                        sp_streq(tv_name, "yield_self"));
  /* An iterator whose value is its receiver, called on something that is NOT
     a plain read (`s.keys.each { }`, `s.dup.each { }`): the statement form
     below produces the tail value by RE-READING the receiver, which it cannot
     do for a call without evaluating it twice, so it emitted the loop and let
     the method fall through to nil. The value path hoists the receiver into a
     temp before the loop and yields that temp, so route these there. */
  int is_tail_recv_val = sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0 &&
                         iter_value_answers_recv(c, id) && tail_iter_receiver(c, id) < 0;
  if (!is_tail_loop && !is_tail_valued && !is_tail_recv_val &&
      sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0 &&
      !call_breaks(c, id) &&
      emit_iteration_stmt(c, id, b, indent)) {
    /* CRuby's iterators answer their receiver -- `each`, `each_pair`, `times`,
       `upto` and the rest -- and at tail position that value is the method's
       return. Without this the method returns a zero value of the receiver's
       type: `{}` for a Hash, nil for an Array, `0..0` for a Range (#3517).
       Emitted AFTER the loop rather than by routing the call through the value
       path: the loop keeps the statement form the surrounding code expects,
       and the receiver is re-read rather than re-evaluated, so this is limited
       to receivers that are a plain read. */
    int _rr = tail_iter_receiver(c, id);
    int _named = _rr >= 0;
    if (_named && g_in_proc_body && g_result_var && g_result_poly) {
      /* a proc answers through the boxed slot, not through its carrier */
      emit_indent(b, indent);
      buf_printf(b, "{ %s = ", g_result_var);
      emit_boxed(c, _rr, b);
      buf_puts(b, "; return 0; }\n");
    }
    else if (_named && comp_ntype(c, id) == g_ret_type &&
             g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN) {
      emit_indent(b, indent);
      buf_puts(b, "return ");
      emit_expr(c, _rr, b);
      buf_puts(b, ";\n");
    }
    else if (_named) {
      /* A block's tail is the block's value, and a spliced block is read as
         the value of a statement expression -- so the receiver goes there as
         an expression rather than a return. Without it the expression's value
         is the loop's, which is void, and the slot it is assigned to rejects
         it (or, for an Array receiver, takes the loop counter). */
      emit_indent(b, indent);
      emit_expr(c, _rr, b);
      buf_puts(b, ";\n");
    }
    return;
  }

  /* setter call at tail position (obj.x = v): side-effect only, no return value.
     A setter name ends in a bare '=' that is not part of ==, !=, <=, >=. */
  if (sp_streq(ty, "CallNode")) {
    const char *_setnm = nt_str(nt, id, "name");
    int _setrecv = nt_ref(nt, id, "receiver");
    size_t _setlen = _setnm ? strlen(_setnm) : 0;
    if (_setrecv >= 0 && _setnm && _setlen >= 2 && _setnm[_setlen - 1] == '=' &&
        _setnm[_setlen - 2] != '=' && _setnm[_setlen - 2] != '!' &&
        _setnm[_setlen - 2] != '<' && _setnm[_setlen - 2] != '>') {
      TyKind _setrt = comp_ntype(c, _setrecv);
      if (ty_is_object(_setrt) || _setrt == TY_POLY) {
        /* `[]=` in tail position yields the ASSIGNED VALUE (CRuby): fall
           through to the generic value path below, whose return-slot
           conversions apply (#3316). Other setters stay statements. */
        if (!sp_streq(_setnm, "[]=")) {
          emit_stmt(c, id, b, indent);
          return;
        }
      }
    }
  }

  /* string << at tail position: mutate receiver, then return it */
  if (sp_streq(ty, "CallNode")) {
    int _srecv = nt_ref(nt, id, "receiver");
    const char *_snm = nt_str(nt, id, "name");
    if (_srecv >= 0 && _snm && sp_streq(_snm, "<<") &&
        comp_ntype(c, _srecv) == TY_STRING &&
        emit_array_mutate_stmt(c, id, b, indent)) {
      /* return the chain's BASE receiver: for `buf << a << b` the immediate
         receiver is the inner `<<` call, and re-emitting it would run the
         inner links a second time (doubling the appended text -- and writing
         the doubled string back through a byref param) */
      int _sbase = str_append_chain_base(c, id);
      emit_indent(b, indent); emit_tail_lead(b);
      int _wp = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
      if (_wp) emit_boxed(c, _sbase, b);
      else emit_expr(c, _sbase, b);
      buf_puts(b, ";\n");
      return;
    }
  }

  /* a value expression: return it (or assign to the begin/rescue result) */
  emit_indent(b, indent);
  emit_tail_lead(b);
  /* A define_method subst read emits the captured literal, not the (poly)
     loop-var slot it nominally reads; size the box decision by the literal's
     type and box the literal node so a poly return slot typechecks. */
  int is_subst = g_dm_subst_name && g_dm_subst_node >= 0 &&
                 sp_streq(ty, "LocalVariableReadNode") &&
                 nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), g_dm_subst_name);
  TyKind vty = is_subst ? comp_ntype(c, g_dm_subst_node) : comp_ntype(c, id);
  int want_poly = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
  if (want_poly && vty != TY_POLY) emit_boxed(c, is_subst ? g_dm_subst_node : id, b);
  else if (!g_result_var && emit_ret_hash_widen_conv(c, g_ret_type, vty, is_subst ? g_dm_subst_node : id, b)) { }
  else if (!g_result_var && emit_ret_poly_array_conv(c, g_ret_type, vty, is_subst ? g_dm_subst_node : id, b)) { }
  /* a poly tail value feeding a narrower (non-poly) return slot -- a scalar
     method(:sym) target, or an RBS-typed String/object method whose body yields
     poly -- needs coercing. (Only for a real return slot, not a begin/rescue
     result var, which stays poly.) */
  else if (!g_result_var && tail_needs_unbox(vty, g_ret_type)) emit_unbox_node(c, g_ret_type, id, b);
  /* A void tail value (a rescue arm ending in `puts`, or a void-returning
     method call) feeding a non-poly result slot: the begin/rescue value
     unified to a nullable pointer, so evaluate the tail for effect and yield
     NULL (nil), rather than assigning a void expression to the typed temp. A
     poly slot is already handled by the want_poly boxing above. (#2900) */
  /* Same shape when the tail DIVERGES: a call spinel folds into an
     unconditional raise yields whatever C type that raise helper returns
     (an int for the constant-folded arms, an sp_RbVal for sp_raise_nomethod),
     which need not match the merged result slot. Evaluate it for effect and
     yield the slot's zero; control never reaches the assignment anyway.
     (#3021, #3060) */
  /* an unknown constant read emits a raise too, and its sp_Class result type
     mismatches the slot the same way a raising call's does (#3748). The
     QUALIFIED spelling raises identically and was not in this list, so
     `begin; NoSuchMod::Missing; rescue NameError; "s"; end` assigned an
     sp_Class to a `const char *` and did not build -- a program CRuby runs. */
  else if (g_result_var && !g_result_poly &&
           (sp_streq(ty, "CallNode") || sp_streq(ty, "ConstantReadNode") ||
            sp_streq(ty, "ConstantPathNode")) &&
           (vty == TY_VOID || vty == TY_NIL ||
            (g_result_ty != TY_UNKNOWN && vty != g_result_ty &&
             /* a numeric slot takes an int-ish raise result, but not a struct
                one (an sp_Class from an unknown constant) */
             (!ty_is_numeric(g_result_ty) || !ty_is_numeric(vty))))) {
    /* Cast the nil default to the result slot's type: a bare 0 in the comma
       expression is an int, not a null-pointer constant, so assigning it to a
       pointer slot is -Wint-conversion. typeof gives NULL for a pointer and 0
       for an int uniformly. */
    buf_puts(b, "("); emit_tail_value(c, id, b);
    /* A struct-valued slot (sp_Class, sp_Range, ...) cannot take a scalar
       cast, so name its zero directly when the slot type is known. */
    /* raise_tail_value_c, not default_value: a by-value USER class's zero is
       `((sp_Frame){0})`, where default_value answers the NULL that suits a
       pointer-backed one -- and a struct slot cannot take it. */
    if (g_result_ty != TY_UNKNOWN && comp_ty_value_obj(c, g_result_ty))
      buf_printf(b, ", %s)", raise_tail_value_c(c, g_result_ty));
    else if (g_result_ty != TY_UNKNOWN && !ty_is_numeric(g_result_ty) &&
             g_result_ty != TY_BOOL && g_result_ty != TY_SYMBOL &&
             default_value(g_result_ty) && default_value(g_result_ty)[0] == '(')
      buf_printf(b, ", %s)", default_value(g_result_ty));
    else buf_printf(b, ", (__typeof__(%s))0)", g_result_var);
  }
  else {
    /* An object tail value returned through an ANCESTOR-typed slot (an RBS
       signature declaring the base class, an override widened to the parent):
       the layouts match by construction, but C needs the cast spelled (#3418). */
    if (!g_result_var) emit_obj_upcast_prefix(c, g_ret_type, vty, b);
    else if (g_result_ty != TY_UNKNOWN) emit_obj_upcast_prefix(c, g_result_ty, vty, b);
    emit_tail_value(c, id, b);
  }
  buf_puts(b, ";\n");
}

void emit_stmts(Compiler *c, int id, Buf *b, int indent) {
  /* Ruby block-locals are FRESH on every block invocation. Find the
     BlockNode whose body this is (map built lazily on the compiler, so it
     dies with it -- no static state to go stale across node tables) and
     reset its non-param locals at the top of each iteration -- without
     this a name first assigned inside the block kept the previous
     iteration's value (doom's render_sprites `sprite ||= ...` reused the
     first sprite for every object on screen). */
  {
    if (!c->blk_body_map) {
      c->blk_body_map = malloc(sizeof(int) * (size_t)c->nt->count);
      for (int i2 = 0; i2 < c->nt->count; i2++) c->blk_body_map[i2] = -1;
      for (int i2 = 0; i2 < c->nt->count; i2++) {
        const char *t2 = nt_type(c->nt, i2);
        if (t2 && sp_streq(t2, "BlockNode")) {
          int b2 = nt_ref(c->nt, i2, "body");
          if (b2 >= 0 && b2 < c->nt->count) c->blk_body_map[b2] = i2;
        }
      }
    }
    if (id >= 0 && id < c->nt->count && c->blk_body_map[id] >= 0)
      emit_block_locals_reset(c, c->blk_body_map[id], b, indent);
  }

  if (id < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (ty && sp_streq(ty, "StatementsNode")) {
    int n = 0;
    const int *body = nt_arr(nt, id, "body", &n);
    for (int k = 0; k < n; k++) emit_stmt(c, body[k], b, indent);
  }
  else {
    emit_stmt(c, id, b, indent);
  }
}

void emit_stmts_tail(Compiler *c, int id, Buf *b, int indent) {
  if (id < 0) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (ty && sp_streq(ty, "StatementsNode")) {
    int n = 0;
    const int *body = nt_arr(nt, id, "body", &n);
    for (int k = 0; k < n; k++) {
      if (k == n - 1) emit_stmt_tail(c, body[k], b, indent);
      else emit_stmt(c, body[k], b, indent);
    }
  }
  else {
    emit_stmt_tail(c, id, b, indent);
  }
}

/* ---- declarations ---- */

/* Heap-managed types need a GC root for their local slot. */
int needs_root(TyKind t) { return t == TY_STRING || t == TY_STRBUF || t == TY_BIGINT || ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || ty_is_object(t) || t == TY_EXCEPTION || t == TY_POLY || t == TY_PROC || t == TY_CURRY || t == TY_METHOD || t == TY_IO || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_ENUMERATOR || t == TY_RANDOM || t == TY_DIR || t == TY_ADDRINFO || t == TY_SOCKOPT || t == TY_OPENSTRUCT || t == TY_MATCHDATA; }

/* Emit `node` boxed into an sp_RbVal. Idempotent: an already-poly value is
   passed through unboxed (double-boxing is a classic silent-corruption bug). */
/* Box a C-text expression `expr` of static type `t` into an sp_RbVal. */
const char *hash_box_cls(TyKind t) {
  switch (t) {
    case TY_STR_INT_HASH:   return "SP_BUILTIN_STR_INT_HASH";
    case TY_STR_STR_HASH:   return "SP_BUILTIN_STR_STR_HASH";
    case TY_INT_STR_HASH:   return "SP_BUILTIN_INT_STR_HASH";
    case TY_INT_INT_HASH:   return "SP_BUILTIN_INT_INT_HASH";
    case TY_STR_POLY_HASH:  return "SP_BUILTIN_STR_POLY_HASH";
    case TY_SYM_POLY_HASH:  return "SP_BUILTIN_SYM_POLY_HASH";
    case TY_POLY_POLY_HASH: return "SP_BUILTIN_POLY_POLY_HASH";
    default:                return NULL;
  }
}


/* ---- Index / element-assignment statements (a[i]=x, a[i] op= x, a[i]&&=/||=,
   and receiver-mutating array calls) moved from codegen_call.c. ---- */
int emit_array_mutate_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);

  /* Guard-narrowed POLY receiver (#3227): `x << "!" if x.is_a?(String)`
     narrows the read to TY_STRING, but the SLOT is poly -- the string-value
     emitters would write the box's .v.s field and lose the shared handle
     (or misread a strbuf box). Re-route through the poly mutator emission
     by restoring the node's poly type for this call. */
  if (rt == TY_STRING && nt_kind(nt, recv) == NK_LocalVariableReadNode) {
    if (sp_str_mutator(name, SP_MUT_NARROW)) {
      const char *rnN = nt_str(nt, recv, "name");
      Scope *rsN = rnN ? comp_scope_of(c, recv) : NULL;
      LocalVar *rlN = rsN ? scope_local(rsN, rnN) : NULL;
      if (rlN && rlN->type == TY_POLY) {
        TyKind svN = c->ntype[recv];
        TyKind svNN = c->nilnarrow[recv];
        c->ntype[recv] = TY_POLY;
        c->nilnarrow[recv] = TY_UNKNOWN;
        emit_indent(b, indent);
        buf_puts(b, "(void)(");
        emit_call(c, id, b);
        buf_puts(b, ");\n");
        c->ntype[recv] = svN;
        c->nilnarrow[recv] = svNN;
        return 1;
      }
    }
  }

  /* Shared-mutable shim (#3227): a strbuf-local receiver of a rebinding
     string mutator re-runs the existing value-semantics arm against a plain
     SHADOW copy (a rename entry plus a temporary slot-type flip point every
     read and the final reassignment at it), then swaps the handle's buffer
     contents in place so every alias observes the mutation. */
  if (rt == TY_STRING &&
      (sp_streq(name, "[]=") || sp_streq(name, "insert") ||
       sp_streq(name, "clear") || sp_streq(name, "slice!"))) {
    const char *sbn = strbuf_local_name(c, recv);
    if (sbn && g_nren < MAX_RENAME) {
      Scope *shs = comp_scope_of(c, recv);
      LocalVar *shlv = scope_local(shs, sbn);
      int tH = ++g_tmp;
      Buf armb; memset(&armb, 0, sizeof armb);
      snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", sbn);
      snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_sb%d", tH);
      g_nren++;
      TyKind sv_ty = shlv->type; shlv->type = TY_STRING;
      int handled = emit_array_mutate_stmt(c, id, &armb, indent + 1);
      shlv->type = sv_ty;
      g_nren--;
      if (!handled) { free(armb.p); }
      else {
        emit_indent(b, indent);
        buf_printf(b, "{ sp_String *_t%d = lv_%s;\n", tH, rename_local(sbn));
        emit_indent(b, indent + 1);
        buf_printf(b, "if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);\n", tH, tH);
        emit_indent(b, indent + 1);
        buf_printf(b, "const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));\n", tH, tH);
        emit_indent(b, indent + 1);
        buf_printf(b, "SP_GC_ROOT(lv__sb%d);\n", tH);
        buf_puts(b, armb.p ? armb.p : "");
        free(armb.p);
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_String_set_bin(_t%d, lv__sb%d);\n", tH, tH);
        emit_indent(b, indent);
        buf_puts(b, "}\n");
        return 1;
      }
    }
  }

  /* mutable-string append: a STRBUF-typed local appends in place (amortized
     O(1)) via sp_String_append. Chains (`s << a << b`) all target the same
     buffer. recv is emitted raw (the sp_String*), not via emit_expr (which
     would hand out a copy). */
  if ((sp_streq(name, "<<") || sp_streq(name, "concat")) && argc == 1) {
    int chain[64]; int nchain = 0; int cur = id;
    while (nchain < 64) {
      while (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "ParenthesesNode")) {
        int pb = nt_ref(nt, cur, "body");
        if (pb < 0) break;
        int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
        if (bn != 1) break;
        cur = bb[0];
      }
      const char *cty = nt_type(nt, cur);
      if (!cty || !sp_streq(cty, "CallNode")) break;
      const char *cnm = nt_str(nt, cur, "name");
      int crecv = nt_ref(nt, cur, "receiver");
      if (!cnm || (!sp_streq(cnm, "<<") && !sp_streq(cnm, "concat")) || crecv < 0) break;
      int cargs = nt_ref(nt, cur, "arguments");
      int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac != 1) break;
      chain[nchain++] = cav[0];
      cur = crecv;
    }
    char srefC[1024];
    if (nchain > 0 && strbuf_slot_ref(c, cur, srefC, sizeof srefC)) {
      for (int j = nchain - 1; j >= 0; j--) {
        int arg = chain[j];
        TyKind at = comp_ntype(c, arg);
        emit_indent(b, indent);
        buf_printf(b, "sp_String_append_bin(%s, ", srefC);
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        /* a string-typed arg whose value is really the unresolved-call gate's
           sp_raise_nomethod(...) poly (`s << time_or_nil.strftime(...)`, the
           receiver being nilable): emit_str_expr coerces it to the string slot,
           keeping the raise, instead of passing the sp_RbVal through raw. */
        else emit_str_expr(c, arg, b);
        buf_puts(b, ");\n");
      }
      return 1;
    }
  }

  /* string append: s << x  ->  s = sp_str_concat(s, x) (value semantics).
     recv must be an assignable lvalue (local or ivar). A chained append
     `s << a << b << c` bottoms out at the same lvalue, so unroll it into
     one reassignment per argument in left-to-right order. */
  if (rt == TY_STRING && sp_streq(name, "<<") && argc == 1) {
    /* walk down the receiver chain, collecting each `<<` argument */
    int chain[64]; int nchain = 0;
    int cur = id;
    while (nchain < 64) {
      /* unwrap ParenthesesNode wrappers (e.g. `(s << a) << b`) */
      while (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "ParenthesesNode")) {
        int pb = nt_ref(nt, cur, "body");
        if (pb < 0) break;
        int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
        if (bn != 1) break;
        cur = bb[0];
      }
      const char *cty = nt_type(nt, cur);
      if (!cty || !sp_streq(cty, "CallNode")) break;
      const char *cnm = nt_str(nt, cur, "name");
      int crecv = nt_ref(nt, cur, "receiver");
      if (!cnm || !sp_streq(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
      int cargs = nt_ref(nt, cur, "arguments");
      int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac != 1) break;
      chain[nchain++] = cav[0];
      cur = crecv;
    }
    const char *rty = nt_type(nt, cur);
    if (nchain > 0 && rty &&
        (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
      /* chain was collected outermost-first; emit left-to-right */
      for (int j = nchain - 1; j >= 0; j--) {
        int arg = chain[j];
        TyKind at = comp_ntype(c, arg);
        emit_indent(b, indent);
        buf_puts(b, "sp_str_check_mutable("); emit_expr(c, cur, b); buf_puts(b, ");\n");
        emit_indent(b, indent);
        emit_expr(c, cur, b); buf_puts(b, " = sp_str_append_grow(");
        emit_expr(c, cur, b); buf_puts(b, ", ");
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        /* a string-typed arg whose value is really the unresolved-call gate's
           sp_raise_nomethod(...) poly (`s << time_or_nil.strftime(...)`, the
           receiver being nilable): emit_str_expr coerces it to the string slot,
           keeping the raise, instead of passing the sp_RbVal through raw. */
        else emit_str_expr(c, arg, b);
        buf_puts(b, ");\n");
      }
      return 1;
    }
    /* `<<` onto a frozen string literal raises FrozenError */
    if (rty && sp_streq(rty, "StringNode")) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_frozen_str("); emit_expr(c, cur, b); buf_puts(b, ");\n");
      return 1;
    }
    return 0;
  }

  /* in-place string bang methods on an assignable receiver: reassign the
     receiver to the transformed value (value-semantics mutation, like <<). */
  #define STRBUF_LOCAL_OF(recv_, out_) do { \
    (out_) = NULL; \
    const char *_rty = nt_type(nt, (recv_)); \
    if (_rty && sp_streq(_rty, "LocalVariableReadNode")) { \
      const char *_rn = nt_str(nt, (recv_), "name"); \
      Scope *_rs = _rn ? comp_scope_of(c, (recv_)) : NULL; \
      LocalVar *_rl = _rs ? scope_local(_rs, _rn) : NULL; \
      if (_rl && _rl->type == TY_STRBUF) (out_) = _rn; \
    } \
  } while (0)
  if ((rt == TY_STRING || rt == TY_STRBUF) && argc == 0) {
    const char *base = NULL;
    if      (sp_streq(name, "chomp!"))      base = "chomp";
    else if (sp_streq(name, "chop!"))       base = "chop";
    else if (sp_streq(name, "upcase!"))     base = "upcase";
    else if (sp_streq(name, "downcase!"))   base = "downcase";
    else if (sp_streq(name, "capitalize!")) base = "capitalize";
    else if (sp_streq(name, "swapcase!"))   base = "swapcase";
    else if (sp_streq(name, "strip!"))      base = "strip";
    else if (sp_streq(name, "lstrip!"))     base = "lstrip";
    else if (sp_streq(name, "rstrip!"))     base = "rstrip";
    else if (sp_streq(name, "reverse!"))    base = "reverse";
    else if (sp_streq(name, "squeeze!"))    base = "squeeze";
    if (base) {
      const char *rty = nt_type(nt, recv);
      /* shared-mutable local: transform + replace the buffer in place (#3227) */
      { char srefS[1024];
        if (strbuf_slot_ref(c, recv, srefS, sizeof srefS)) {
          int tbS = ++g_tmp;
          emit_indent(b, indent);
          buf_printf(b, "{ sp_String *_t%d = %s; sp_String_set_bin(_t%d, sp_str_%s(sp_String_cstr(_t%d))); }\n",
                     tbS, srefS, tbS, base, tbS);
          return 1;
        }
      }
      if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
        /* a frozen receiver raises FrozenError before the transform (#2314) */
        emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ");\n");
        return 1;
      }
    }
  }
  /* arg-taking in-place bangs (gsub!/sub!/tr!/delete!/slice!): transform and
     reassign, reusing the non-bang expression emitters by temporarily
     renaming the node. Statement position only, like the other bangs (the
     nil-when-unchanged return is not modeled). slice! removes the matched
     span: the string form deletes the first occurrence, the (i, len) form
     splices the span out. */
  if ((rt == TY_STRING || rt == TY_STRBUF) && argc >= 1) {
    const char *rty2 = nt_type(nt, recv);
    int assignable2 = rty2 && (sp_streq(rty2, "LocalVariableReadNode") ||
                               sp_streq(rty2, "InstanceVariableReadNode") || sp_streq(rty2, "SelfNode"));
    const char *abase = NULL, *abang = NULL;
    if      (sp_streq(name, "gsub!"))   { abase = "gsub";   abang = "gsub!"; }
    else if (sp_streq(name, "sub!"))    { abase = "sub";    abang = "sub!"; }
    else if (sp_streq(name, "tr!"))     { abase = "tr";     abang = "tr!"; }
    else if (sp_streq(name, "delete!")) { abase = "delete"; abang = "delete!"; }
    if (abase) {
      /* shared-mutable local: transform + replace the buffer in place (#3227) */
      char srefA[1024];
      if (strbuf_slot_ref(c, recv, srefA, sizeof srefA)) {
        int tbA = ++g_tmp;
        nt_node_set_str((NodeTable *)nt, id, "name", abase);
        emit_indent(b, indent);
        buf_printf(b, "{ sp_String *_t%d = %s; sp_String_set_bin(_t%d, ",
                   tbA, srefA, tbA);
        emit_expr(c, id, b);
        buf_puts(b, "); }\n");
        nt_node_set_str((NodeTable *)nt, id, "name", abang);
        return 1;
      }
    }
    if (abase && assignable2) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      nt_node_set_str((NodeTable *)nt, id, "name", abase);
      emit_indent(b, indent);
      emit_expr(c, recv, b); buf_puts(b, " = ");
      emit_expr(c, id, b);
      buf_puts(b, ";\n");
      nt_node_set_str((NodeTable *)nt, id, "name", abang);
      return 1;
    }
    if (sp_streq(name, "slice!") && assignable2) {
      if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        /* remove the first occurrence */
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_puts(b, " = sp_str_sub(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ", (&(\"\\xff\")[1]));\n");
        return 1;
      }
      if (argc == 1 && (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_RANGE)) {
        /* slice!(i): one char at i; slice!(range): the range's span. Same
           clamped splice as the (start, len) arm below (OOB is a no-op). */
        int ti2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp;
        emit_indent(b, indent);
        if (comp_ntype(c, argv[0]) == TY_RANGE) {
          int tr2 = ++g_tmp;
          buf_printf(b, "{ sp_Range _t%d = ", tr2); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_int _t%d = (sp_int)sp_str_length(", tn2); emit_expr(c, recv, b);
          buf_printf(b, "); sp_int _t%d = _t%d.first < 0 ? _t%d.first + _t%d : _t%d.first;",
                     ti2, tr2, tr2, tn2, tr2);
          buf_printf(b, " sp_int _t%d = (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                     tl2, tr2, tr2, tn2, tr2, ti2, tr2);
          buf_printf(b, " if (_t%d < 0) _t%d = 0;", tl2, tl2);
        }
        else {
          buf_printf(b, "{ sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_int _t%d = 1; sp_int _t%d = (sp_int)sp_str_length(", tl2, tn2);
          emit_expr(c, recv, b);
          buf_printf(b, "); if (_t%d < 0) _t%d += _t%d;", ti2, ti2, tn2);
        }
        buf_printf(b, " if (_t%d >= 0 && _t%d < _t%d && _t%d > 0) {"
                      " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d; ",
                   ti2, ti2, tn2, tl2,
                   tl2, tn2, ti2, tl2, tn2, ti2);
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_str_concat(sp_str_sub_range(");
        emit_expr(c, recv, b);
        buf_printf(b, ", 0, _t%d), sp_str_sub_range(", ti2);
        emit_expr(c, recv, b);
        buf_printf(b, ", _t%d + _t%d, _t%d - _t%d - _t%d)); } }\n", ti2, tl2, tn2, ti2, tl2);
        return 1;
      }
      if (argc == 2) {
        /* splice out [i, i+len): head + tail, bounds clamped to the string in
           characters (a negative i counts from the end; OOB is a no-op) (#3084) */
        int ti2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "{ sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; sp_int _t%d = (sp_int)sp_str_length(", tn2);
        emit_expr(c, recv, b);
        buf_printf(b, "); if (_t%d < 0) _t%d += _t%d;"
                      " if (_t%d >= 0 && _t%d <= _t%d && _t%d > 0) {"
                      " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d; ",
                   ti2, ti2, tn2,
                   ti2, ti2, tn2, tl2,
                   tl2, tn2, ti2, tl2, tn2, ti2);
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_str_concat(sp_str_sub_range(");
        emit_expr(c, recv, b);
        buf_printf(b, ", 0, _t%d), sp_str_sub_range(", ti2);
        emit_expr(c, recv, b);
        buf_printf(b, ", _t%d + _t%d, _t%d - _t%d - _t%d)); } }\n", ti2, tl2, tn2, ti2, tl2);
        return 1;
      }
    }
  }
  /* replace / prepend / clear / delete_prefix!/suffix! via reassignment */
  if (rt == TY_STRING) {
    const char *rty = nt_type(nt, recv);
    int assignable = rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"));
    /* an in-place mutator on a frozen string literal raises FrozenError */
    if (rty && sp_streq(rty, "StringNode") &&
        (sp_streq(name, "insert") || sp_streq(name, "prepend") || sp_streq(name, "<<") ||
         sp_streq(name, "concat") || sp_streq(name, "replace") || sp_streq(name, "clear") ||
         sp_streq(name, "delete_prefix!") || sp_streq(name, "delete_suffix!"))) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_frozen_str("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      return 1;
    }
    if ((sp_streq(name, "replace") || sp_streq(name, "prepend")) && argc == 1) {
      /* shared-mutable local: swap/prepend the buffer contents in place (#3227) */
      char srefR2[1024];
      if (strbuf_slot_ref(c, recv, srefR2, sizeof srefR2)) {
        int tbR = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "{ sp_String *_t%d = %s; sp_String_set_bin(_t%d, ",
                   tbR, srefR2, tbR);
        if (sp_streq(name, "prepend")) {
          buf_puts(b, "sp_str_concat("); emit_expr(c, argv[0], b);
          buf_printf(b, ", sp_String_cstr(_t%d))", tbR);
        }
        else emit_expr(c, argv[0], b);
        buf_puts(b, "); }\n");
        return 1;
      }
    }
    if (assignable && sp_streq(name, "replace") && argc == 1) {
      /* copy the source bytes: rebinding to the source value itself would
         carry a frozen literal's marker into the receiver, so a later
         mutation of the (CRuby-mutable) receiver would raise */
      int trep = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ const char *_t%d = ", trep); emit_expr(c, argv[0], b);
      buf_printf(b, "; ");
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_from_bytes(_t%d, sp_str_byte_len(_t%d)); }\n", trep, trep);
      return 1;
    }
    if (assignable && sp_streq(name, "prepend") && argc == 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, recv, b); buf_puts(b, ");\n");
      return 1;
    }
    if (!assignable && sp_streq(name, "clear") && argc == 0 &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      /* clear on an unnamed mutable receiver ((+"abc").clear): the temp's
         mutation is unobservable, so evaluate the receiver (a frozen value
         still raises, as CRuby) and yield a fresh unfrozen empty */
      int tcl9 = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ const char *_t%d = ", tcl9); emit_expr(c, recv, b);
      buf_printf(b, "; sp_str_check_mutable(_t%d); (void)_t%d; }\n", tcl9, tcl9);
      return 1;
    }
    if (sp_streq(name, "clear") && argc == 0) {
      /* a shared-mutable receiver owns a buffer: empty it in place, so every
         alias sees the clear and the result is the same object. The
         reassignment form below cannot serve it -- the read of a handle is not
         an lvalue, and the emitted C did not compile. */
      char srefC[1024];
      if (strbuf_slot_ref(c, recv, srefC, sizeof srefC)) {
        emit_indent(b, indent);
        buf_printf(b, "sp_String_set_bin(%s, (&(\"\\xff\")[1]));\n", srefC);
        return 1;
      }
    }
    if (assignable && sp_streq(name, "clear") && argc == 0) {
      /* a fresh unfrozen empty: the shared frozen "" literal would make a
         later mutation of the cleared receiver raise */
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = sp_str_from_bytes(\"\", 0);\n");
      return 1;
    }
    if (assignable && sp_streq(name, "insert") && argc == 2) {
      /* insert(i, x): s[0,i] + x + s[i..]. A negative i counts from the end
         and inserts after that character (i += len + 1). */
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (sp_int)sp_str_length(", ti, ti); emit_expr(c, recv, b); buf_printf(b, ") + 1; ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _t%d), ", ti); emit_expr(c, argv[1], b);
      buf_puts(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, (sp_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_printf(b, "))); }\n");
      return 1;
    }
    if ((sp_streq(name, "delete_prefix!") || sp_streq(name, "delete_suffix!")) && argc == 1) {
      char srefD[1024];
      if (strbuf_slot_ref(c, recv, srefD, sizeof srefD)) {
        const char *base3 = sp_streq(name, "delete_prefix!") ? "delete_prefix" : "delete_suffix";
        int tbD = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "{ sp_String *_t%d = %s; sp_String_set_bin(_t%d, sp_str_%s(sp_String_cstr(_t%d), ",
                   tbD, srefD, tbD, base3, tbD);
        emit_expr(c, argv[0], b);
        buf_puts(b, ")); }\n");
        return 1;
      }
    }
    if (assignable && (sp_streq(name, "delete_prefix!") || sp_streq(name, "delete_suffix!")) && argc == 1) {
      const char *base = sp_streq(name, "delete_prefix!") ? "delete_prefix" : "delete_suffix";
      emit_indent(b, indent); emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ");\n");
      return 1;
    }
    /* concat(a, b, ...): append each argument in order (multi-arg `<<`). An
       Integer argument appends its codepoint, like `<<`. */
    if (assignable && sp_streq(name, "concat") && argc >= 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      for (int a = 0; a < argc; a++) {
        TyKind at = comp_ntype(c, argv[a]);
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[a], b);
        buf_puts(b, ");\n");
      }
      return 1;
    }
    /* s[i] = str: replace the single character at index i (negative from the
       end) with the (string) value -> s[0,i] + val + s[i+1..]. Valid range is
       -len..len (i == len appends, matching CRuby); anything outside raises
       IndexError with the original index. The (start,len) and Range / Regexp
       forms remain unsupported (string splice). */
    /* A boxed index reaches here whenever the value came out of a container --
       a destructured block parameter (`pairs.each { |r, c| s[c] = "*" }`), an
       element read, an untyped argument. It was not in the gate, so the call
       fell through to "undefined method '[]=' for an instance of String" for a
       program CRuby runs (#4060). emit_int_expr converts it through the
       CHECKED form, so a boxed Integer works and a Range or String -- the
       splice forms spinel does not support -- raises TypeError rather than
       being read as a number. */
    if (assignable && sp_streq(name, "[]=") && argc == 2 &&
        (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY)) {
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _len%d = (sp_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_puts(b, ");");
      buf_printf(b, " sp_int _a%d = _t%d < 0 ? _t%d + _len%d : _t%d;", ti, ti, ti, ti, ti);
      buf_printf(b, " if (_a%d < 0 || _a%d > _len%d) sp_raise_cls(\"IndexError\", sp_sprintf(\"index %%lld out of string\", (long long)_t%d));",
                 ti, ti, ti, ti);
      buf_puts(b, " "); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _a%d), ", ti); emit_str_expr(c, argv[1], b);
      buf_printf(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _a%d + 1 < _len%d ? _a%d + 1 : _len%d, _len%d)); }\n", ti, ti, ti, ti, ti);
      return 1;
    }
    /* s[range] = v: splice over the range's char span (negative n inserts) */
    if (assignable && sp_streq(name, "[]=") && argc == 2 && comp_ntype(c, argv[0]) == TY_RANGE) {
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ sp_Range _t%d = ", ti); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _len%d = (sp_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_puts(b, ");");
      /* a beginless bound is 0 and an endless one is the last index, rather
         than the SP_INT_NIL sentinel a negative-index fixup would fold into a
         wild offset (`s[..1] = x` raised RangeError) */
      buf_printf(b, " sp_int _a%d = _t%d.first == SP_INT_NIL ? 0 :"
                    " (_t%d.first < 0 ? _t%d.first + _len%d : _t%d.first);", ti, ti, ti, ti, ti, ti);
      buf_printf(b, " int _oe%d = _t%d.last == SP_INT_NIL;", ti, ti);
      buf_printf(b, " sp_int _e%d = _oe%d ? _len%d - 1 :"
                    " (_t%d.last < 0 ? _t%d.last + _len%d : _t%d.last);", ti, ti, ti, ti, ti, ti, ti);
      buf_printf(b, " sp_int _n%d = _e%d - _a%d + ((_t%d.excl && !_oe%d) ? 0 : 1);", ti, ti, ti, ti, ti);
      buf_puts(b, " "); emit_expr(c, recv, b); buf_puts(b, " = sp_str_splice_at(");
      emit_expr(c, recv, b);
      buf_printf(b, ", _a%d, _n%d < 0 ? 0 : _n%d, ", ti, ti, ti); emit_str_expr(c, argv[1], b);
      buf_puts(b, ", 1); }\n");
      return 1;
    }
    /* s[start, len] = v */
    if (assignable && sp_streq(name, "[]=") && argc == 3 && comp_ntype(c, argv[0]) == TY_INT) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_splice_at("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
      buf_puts(b, ", "); emit_str_expr(c, argv[2], b);
      buf_puts(b, ", 0);\n");
      return 1;
    }
    /* s["sub"] = v: replace the first occurrence; missing raises IndexError */
    if (assignable && sp_streq(name, "[]=") && argc == 2 && comp_ntype(c, argv[0]) == TY_STRING &&
        re_lit_index(c, argv[0]) < 0) {
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ const char *_t%d = ", ti); emit_str_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _a%d = sp_str_index_opt(", ti); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d); if (_a%d == SP_INT_NIL) sp_raise_cls(\"IndexError\", \"string not matched\");", ti, ti);
      buf_puts(b, " "); emit_expr(c, recv, b); buf_puts(b, " = sp_str_splice_at(");
      emit_expr(c, recv, b);
      buf_printf(b, ", _a%d, (sp_int)sp_str_length(_t%d), ", ti, ti); emit_str_expr(c, argv[1], b);
      buf_puts(b, ", 0); }\n");
      return 1;
    }
    /* s[/re/, n] = v: replace the nth capture group's span (#3548) */
    if (assignable && sp_streq(name, "[]=") && argc == 3 && re_lit_index(c, argv[0]) >= 0) {
      int ts = ++g_tmp, tn = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ const char *_t%d = ", ts); emit_expr(c, recv, b);
      buf_printf(b, "; sp_str_check_mutable(_t%d);", ts);
      buf_printf(b, " sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; if (sp_re_match(sp_re_pat_%d, _t%d) < 0)"
                    " sp_raise_cls(\"IndexError\", \"regexp not matched\");",
                 re_lit_index(c, argv[0]), ts);
      buf_printf(b, " if (_t%d < 0 || _t%d > 9)"
                    " sp_raise_cls(\"IndexError\", sp_sprintf(\"index %%lld out of regexp\","
                    " (long long)_t%d));", tn, tn, tn);
      buf_printf(b, " { sp_int _b = sp_re_caps[2 * _t%d], _e = sp_re_caps[2 * _t%d + 1]; ", tn, tn);
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_concat(sp_str_concat(sp_str_byteslice(_t%d, 0, _b), ", ts);
      emit_str_expr(c, argv[2], b);
      buf_printf(b, "), sp_str_byteslice(_t%d, _e, (sp_int)sp_str_byte_len(_t%d) - _e)); } }\n",
                 ts, ts);
      return 1;
    }
    /* s[/re/] = v: replace the first match's span; no match raises IndexError */
    if (assignable && sp_streq(name, "[]=") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_splice_re(sp_re_pat_%d, ", re_lit_index(c, argv[0]));
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b);
      buf_puts(b, ");\n");
      return 1;
    }
  }

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    /* Hash#store is the method form of []= */
    if (hn && (sp_streq(name, "[]=") || sp_streq(name, "store")) && argc == 2) {
      emit_indent(b, indent);
      buf_puts(b, "if (sp_gc_is_frozen("); emit_expr(c, recv, b); { buf_puts(b, ")) sp_raise_frozen_hash_at("); emit_expr(c, recv, b); buf_printf(b, ", %s);\n", hash_box_cls(rt)); }
      emit_indent(b, indent);
      buf_printf(b, "sp_%sHash_set(", hn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b); else emit_hash_key(c, argv[0], ty_hash_key(rt), b);
      buf_puts(b, ", ");
      if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[1], b);
      else {
        /* A poly value (holds the hash's value type at runtime, e.g. a String?
           guarded non-nil) into a typed-value hash: coerce to its element
           representation, as the typed-array `[]=` path does. */
        TyKind hvt = ty_hash_val(rt), vt = comp_ntype(c, argv[1]);
        if (vt == TY_POLY && hvt == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vt == TY_POLY && hvt == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vt == TY_POLY && hvt == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[1], b);
      }
      buf_puts(b, ");\n");
      return 1;
    }
    return 0;
  }

  if (rt == TY_POLY_ARRAY) {
    if (sp_streq(name, "[]=") && argc == 2) {
      /* a splatted index (`recv[*args] = v`) has runtime arity -- it selects
         between element-set and slice-splice per call. Not lowered yet; the
         splat value-position support would otherwise feed the built ARRAY
         pointer in as the element index (a resize loop on a garbage index). */
      if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SplatNode")) {
        unsupported(c, id, "splatted index in []= (later slice)");
        return 1;
      }
      /* arr[range] = rhs : a splice over the range's (start, length). */
      if (comp_ntype(c, argv[0]) == TY_RANGE) {
        emit_indent(b, indent);
        emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
        buf_puts(b, ";\n");
        return 1;
      }
      emit_indent(b, indent);
      buf_puts(b, "sp_PolyArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ");\n");
      return 1;
    }
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1) {
      for (int a = 0; a < argc; a++) {
        emit_indent(b, indent);
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[a], b); buf_puts(b, ");\n");
      }
      return 1;
    }
    if (sp_streq(name, "clear") && argc == 0) {
      emit_indent(b, indent);
      buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ")->len = 0;\n");
      return 1;
    }
    return 0;
  }

  if (rt == TY_POLY &&
      (sp_streq(name, "<<") || sp_streq(name, "push") || sp_streq(name, "append")) && argc >= 1) {
    /* A poly value that holds an array at runtime appends via sp_poly_shl.
       `<<` takes one arg; push/append take any number (each boxed in turn).
       A `<<` whose receiver is an ASSIGNABLE slot REBINDS the result: a
       plain-string runtime value appends by concat (sp_poly_shl returns the
       new box), so discarding the result loses the append (#3325); arrays
       and shared handles return the receiver, making the rebind an identity.
       The rebind routes through the value form so a user-defined `<<` still
       dispatches per class. */
    if (sp_streq(name, "<<") && argc == 1 && nt_type(nt, recv) &&
        (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") ||
         sp_streq(nt_type(nt, recv), "InstanceVariableReadNode"))) {
      emit_indent(b, indent);
      emit_expr(c, recv, b);
      buf_puts(b, " = ");
      emit_call(c, id, b);
      buf_puts(b, ";\n");
      return 1;
    }
    /* Skip when a user class defines the name -- the poly value may be that
       object, so let it reach the per-class poly dispatch instead of forcing
       the builtin-array append. */
    int has_user = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0) { has_user = 1; break; }
    if (!has_user) {
      for (int a = 0; a < argc; a++) {
        emit_indent(b, indent);
        buf_puts(b, "sp_poly_shl("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[a], b); buf_puts(b, ");\n");
      }
      return 1;
    }
  }

  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;

  /* An index that is not one raises a TypeError, which the expression form
     already emits: decline here so the statement falls back to it. Handled
     directly, `a["x"] = 9` put the pointer in the sp_int slot -- the C build
     aborted on a String, and a Symbol or nil was absorbed into a write to
     element 0 (#3925, #3926). */
  if (array_index_bad_class(c, id)) return 0;

  if (sp_streq(name, "[]=") && argc == 2) {
    /* a splatted index (`recv[*args] = v`) has runtime arity; see the guard
       on the other []= arm. */
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SplatNode")) {
      unsupported(c, id, "splatted index in []= (later slice)");
      return 1;
    }
    /* arr[range] = rhs : a splice over the range's (start, length). */
    if (comp_ntype(c, argv[0]) == TY_RANGE) {
      emit_indent(b, indent);
      emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
      buf_puts(b, ";\n");
      return 1;
    }
    emit_indent(b, indent);
    buf_printf(b, "sp_%sArray_set(", k);
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
    /* coerce a poly RHS to the typed array's element representation */
    TyKind et = ty_array_elem(rt);
    TyKind vt = comp_ntype(c, argv[1]);
    if (vt == TY_POLY && et == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else if (vt == TY_POLY && et == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else if (vt == TY_POLY && et == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[1], b);
    buf_puts(b, ");\n");
    return 1;
  }
  if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1) {
    TyKind et = ty_array_elem(rt);
    /* Any splat arg (`arr.push(*other)`) spreads its array's elements at
       runtime; materialize the receiver once so the per-element loop pushes into
       the same array (#3208). A literal splat is pre-expanded upstream, but a
       variable/expression splat arrives as a SplatNode. */
    int has_splat = 0;
    for (int a = 0; a < argc; a++) {
      const char *aty = nt_type(nt, argv[a]);
      if (aty && sp_streq(aty, "SplatNode")) { has_splat = 1; break; }
    }
    int tr = -1;
    if (has_splat) {
      tr = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "{ sp_%sArray *_t%d = ", k, tr); emit_expr(c, recv, b); buf_puts(b, ";\n");
    }
    for (int a = 0; a < argc; a++) {
      const char *aty = nt_type(nt, argv[a]);
      if (aty && sp_streq(aty, "SplatNode")) {
        int inner = nt_ref(nt, argv[a], "expression");
        TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        const char *ak = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : NULL);
        int tsrc = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
        emit_indent(b, indent + 1);
        if (ak && ty_is_array(at)) {
          buf_printf(b, "{ sp_%sArray *_t%d = ", ak, tsrc); emit_expr(c, inner, b); buf_puts(b, "; ");
          buf_printf(b, "sp_int _t%d = sp_%sArray_length(_t%d); ", tn, ak, tsrc);
          buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, ", ti, ti, tn, ti, k, tr);
          char getx[128]; snprintf(getx, sizeof getx, "sp_%sArray_get(_t%d, _t%d)", ak, tsrc, ti);
          TyKind selem = ty_array_elem(at);
          if (et == TY_POLY && !sp_streq(ak, "Poly")) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, selem, getx, &bx); buf_puts(b, bx.p ? bx.p : getx); free(bx.p); }
          else if (sp_streq(ak, "Poly") && et == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getx);
          else if (sp_streq(ak, "Poly") && et == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", getx);
          else if (sp_streq(ak, "Poly") && et == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", getx);
          else buf_puts(b, getx);
          buf_puts(b, "); }\n");
        }
        else {
          /* unknown/poly splat source: normalize to a PolyArray and spread boxed */
          buf_printf(b, "{ sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsrc); emit_boxed(c, inner, b); buf_puts(b, "); ");
          buf_printf(b, "sp_int _t%d = _t%d->len; ", tn, tsrc);
          buf_printf(b, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, ", ti, ti, tn, ti, k, tr);
          char getx[64]; snprintf(getx, sizeof getx, "sp_PolyArray_get(_t%d, _t%d)", tsrc, ti);
          if (et == TY_POLY) buf_puts(b, getx);
          else if (et == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getx);
          else if (et == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", getx);
          else if (et == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", getx);
          else buf_puts(b, getx);
          buf_puts(b, "); }\n");
        }
        continue;
      }
      emit_indent(b, indent + (has_splat ? 1 : 0));
      buf_printf(b, "sp_%sArray_push(", k);
      if (has_splat) buf_printf(b, "_t%d", tr); else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      /* coerce a poly value (holds the element type at runtime) to the typed
         array's element representation */
      TyKind vt = comp_ntype(c, argv[a]);
      /* a poly-array element must be boxed; emit_boxed also fixes a yield whose
         node type widened to poly but whose per-site value is concrete (#2454). */
      if (et == TY_POLY) emit_boxed(c, argv[a], b);
      else if (vt == TY_POLY && et == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      else if (vt == TY_POLY && et == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      else if (vt == TY_POLY && et == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      /* A shared-mutable string (#3227) reads as its sp_String* handle. A
         typed array's element slot is a plain const char*, so the handle has
         to be spent here -- pushed raw it went in as a struct pointer that the
         C compiler only warned about, and the element read back as garbage
         (#3400). The copy is the same one every other plain-String slot takes:
         only a str_shared slot aliases. */
      else if (vt == TY_STRBUF && et == TY_STRING) {
        buf_puts(b, "sp_str_concat(sp_String_cstr(");
        emit_expr(c, argv[a], b);
        buf_puts(b, "), (&(\"\\xff\")[1]))");
      }
      else emit_expr(c, argv[a], b);
      buf_puts(b, ");\n");
    }
    if (has_splat) { emit_indent(b, indent); buf_puts(b, "}\n"); }
    return 1;
  }
  if (sp_streq(name, "concat") && argc >= 1) {
    /* Process each arg sequentially, snapshotting each arg's length before
       its own loop (the test expects sequential/non-snapshotted behavior). */
    int tr = ++g_tmp;
    TyKind et = ty_array_elem(rt);
    emit_indent(b, indent);
    buf_printf(b, "{ sp_%sArray *_t%d = ", k, tr); emit_expr(c, recv, b); buf_puts(b, ";\n");
    for (int a = 0; a < argc; a++) {
      int tn = ++g_tmp, ti = ++g_tmp;
      /* the source array may be a different kind than the receiver (e.g.
         IntArray#concat(PolyArray)); read with the source's kind and coerce
         each element into the receiver's element representation. */
      TyKind at = comp_ntype(c, argv[a]);
      /* Ask the SLOT, not the node: a block parameter's node type can read as
         an array kind while its declaration is boxed, and reading the boxed
         local as a typed pointer does not compile (#3850). */
      if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "LocalVariableReadNode")) {
        Scope *asc = comp_scope_of(c, argv[a]);
        LocalVar *alv = asc ? scope_local(asc, nt_str(nt, argv[a], "name")) : NULL;
        if (alv && alv->type != TY_UNKNOWN) at = alv->type;
      }
      const char *ak = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
      if (at == TY_POLY) {
        /* boxed source: walk it through the boxed surface */
        int tv = ++g_tmp;
        emit_indent(b, indent + 1);
        buf_printf(b, "{ sp_RbVal _t%d = ", tv); emit_boxed(c, argv[a], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);\n", tv);
        emit_indent(b, indent + 1);
        buf_printf(b, "sp_int _t%d = sp_poly_length(_t%d);", tn, tv);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, ",
                   ti, ti, tn, ti, k, tr);
        { char el[64]; snprintf(el, sizeof el, "sp_poly_each_elem(_t%d, _t%d)", tv, ti);
          emit_unbox_text(c, et, el, b); }
        buf_puts(b, "); }\n");
        continue;
      }
      if (!ak) ak = k;
      /* Evaluate the source ONCE. It used to be re-emitted for the length and
         again for every element, so `result.concat(str.bytes)` rebuilt the
         whole byte array per element -- quadratic, and any side effect ran
         n+1 times. Rooted: pushing into the receiver can collect. */
      int ts = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "{ sp_%sArray *_t%d = ", ak, ts); emit_expr(c, argv[a], b);
      buf_printf(b, "; SP_GC_ROOT(_t%d);\n", ts);
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_int _t%d = sp_%sArray_length(_t%d);", tn, ak, ts);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, ",
                 ti, ti, tn, ti, k, tr);
      char getexpr[256];
      snprintf(getexpr, sizeof getexpr, "sp_%sArray_get(_t%d, _t%d)", ak, ts, ti);
      if (sp_streq(k, "Poly") && !sp_streq(ak, "Poly")) {
        /* box the source scalar into the poly receiver */
        emit_boxed_text(c, ty_array_elem(at), getexpr, b);
      }
      else if (!sp_streq(k, "Poly") && sp_streq(ak, "Poly")) {
        /* unbox the source poly element into the receiver's scalar */
        if (et == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", getexpr);
        else if (et == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getexpr);
        else if (et == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", getexpr);
        else buf_puts(b, getexpr);
      }
      else buf_puts(b, getexpr);
      buf_puts(b, "); }\n");
    }
    emit_indent(b, indent);
    buf_puts(b, "}\n");
    return 1;
  }
  return 0;
}

/* h[k] op= v  /  a[i] op= v  (IndexOperatorWriteNode). Receiver and key
   are evaluated once into temps. */
/* The receiver / key of an IndexOperatorWriteNode, already evaluated into C
   temps by the caller. The value form of `h[k] op= v` needs the SAME two for
   the write and the read-back that follows it; re-emitting the nodes would
   evaluate a method-call key twice, which is why that form used to decline
   anything but a variable or a literal (#3417). NULL when nothing hoisted. */
const char *g_iow_recv_ref = NULL;
const char *g_iow_key_ref = NULL;
static char g_iow_recv_buf[32], g_iow_key_buf[32];

enum { IOW_KEY_HASH, IOW_KEY_INT, IOW_KEY_RAW, IOW_KEY_BOXED };

static void iow_emit_recv(Compiler *c, int recv, Buf *b) {
  if (g_iow_recv_ref) { buf_puts(b, g_iow_recv_ref); return; }
  emit_expr(c, recv, b);
}
static void iow_emit_key(Compiler *c, int key, Buf *b, int kind, TyKind kt) {
  if (g_iow_key_ref) { buf_puts(b, g_iow_key_ref); return; }
  switch (kind) {
    case IOW_KEY_HASH:  emit_hash_key(c, key, kt, b); return;
    case IOW_KEY_INT:   emit_int_expr(c, key, b); return;
    case IOW_KEY_BOXED: emit_boxed(c, key, b); return;
    default:            emit_expr(c, key, b); return;
  }
}

/* Evaluate the receiver and key once into prelude temps and point the two
   emitters above at them. Returns 0 for a receiver shape the write paths do
   not handle, leaving nothing hoisted. */
int emit_index_opw_hoist(Compiler *c, int id, Buf *pre, int indent) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (recv < 0 || argc != 1 || !pre) return 0;
  TyKind rt = comp_ntype(c, recv);
  Buf kb; memset(&kb, 0, sizeof kb);
  const char *ktype = NULL;
  int key_is_ptr = 0;
  if (ty_is_hash(rt) && ty_hash_cname(rt)) {
    TyKind kt = ty_hash_key(rt);
    ktype = c_type_name(kt);
    key_is_ptr = (kt == TY_STRING || kt == TY_POLY);
    emit_hash_key(c, argv[0], kt, &kb);
  }
  else if (ty_is_array(rt) && (rt == TY_POLY_ARRAY || array_kind(rt))) {
    ktype = "sp_int"; emit_int_expr(c, argv[0], &kb);
  }
  else if (rt == TY_POLY) {
    TyKind kt = comp_ntype(c, argv[0]);
    if (kt == TY_SYMBOL)      { ktype = "sp_sym";      emit_expr(c, argv[0], &kb); }
    else if (kt == TY_STRING) { ktype = "const char *"; key_is_ptr = 1; emit_expr(c, argv[0], &kb); }
    else if (kt == TY_INT)    { ktype = "sp_int";     emit_int_expr(c, argv[0], &kb); }
    else                      { ktype = "sp_RbVal";    key_is_ptr = 1; emit_boxed(c, argv[0], &kb); }
  }
  else { free(kb.p); return 0; }
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  int ta = ++g_tmp, tb = ++g_tmp;
  emit_indent(pre, indent);
  buf_printf(pre, "%s _t%d = %s;\n", c_type_name(rt), ta, rb.p ? rb.p : "0");
  /* a receiver or key that allocates has no other root while the fold below
     runs, and the fold can collect */
  if (subtree_may_allocate(nt, recv)) {
    emit_indent(pre, indent);
    buf_printf(pre, rt == TY_POLY ? "SP_GC_ROOT_RBVAL(_t%d);\n" : "SP_GC_ROOT(_t%d);\n", ta);
  }
  emit_indent(pre, indent);
  buf_printf(pre, "%s _t%d = %s;\n", ktype, tb, kb.p ? kb.p : "0");
  if (key_is_ptr && subtree_may_allocate(nt, argv[0])) {
    emit_indent(pre, indent);
    buf_printf(pre, sp_streq(ktype, "sp_RbVal") ? "SP_GC_ROOT_RBVAL(_t%d);\n" : "SP_GC_ROOT(_t%d);\n", tb);
  }
  free(rb.p); free(kb.p);
  snprintf(g_iow_recv_buf, sizeof g_iow_recv_buf, "_t%d", ta);
  snprintf(g_iow_key_buf, sizeof g_iow_key_buf, "_t%d", tb);
  g_iow_recv_ref = g_iow_recv_buf;
  g_iow_key_ref = g_iow_key_buf;
  return 1;
}
void emit_index_opw_unhoist(void) { g_iow_recv_ref = NULL; g_iow_key_ref = NULL; }

void emit_index_op_write(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *op = nt_str(nt, id, "binary_operator");
  int args = nt_ref(nt, id, "arguments");
  int v = nt_ref(nt, id, "value");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (argc != 1 || !op) unsupported(c, id, "index operator assignment");
  TyKind rt = comp_ntype(c, recv);

  int ta = ++g_tmp, tb = ++g_tmp;

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    TyKind vt = ty_hash_val(rt);
    if (!hn) unsupported(c, id, "index operator assignment (hash)");
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); iow_emit_recv(c, recv, b);
    buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tb); iow_emit_key(c, argv[0], b, IOW_KEY_HASH, ty_hash_key(rt));
    buf_puts(b, "; ");
    /* Build the new value (which reads the slot and evaluates the RHS) BEFORE
       the frozen check: Ruby desugars `h[k] += v` to `h[k] = h[k] + v`, so the
       read and the RHS run before []= raises on a frozen hash. */
    int tv = ++g_tmp;
    const char *pf = vt == TY_POLY ?
        (sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub" :
         sp_streq(op, "*") ? "sp_poly_mul" : sp_streq(op, "/") ? "sp_poly_div" :
         sp_streq(op, "%") ? "sp_poly_mod" : sp_streq(op, "**") ? "sp_poly_pow" : NULL) : NULL;
    buf_printf(b, "%s _t%d = ", c_type_name(vt), tv);
    if (vt == TY_STRING && sp_streq(op, "+")) {
      buf_printf(b, "sp_str_concat(sp_%sHash_get(_t%d, _t%d), ", hn, ta, tb);
      emit_expr(c, v, b); buf_puts(b, ")");
    }
    else if (pf) {
      /* a poly-valued slot folds via the dynamic operator on boxed operands */
      buf_printf(b, "%s(sp_%sHash_get(_t%d, _t%d), ", pf, hn, ta, tb);
      emit_boxed(c, v, b); buf_puts(b, ")");
    }
    else {
      buf_printf(b, "sp_%sHash_get(_t%d, _t%d) %s ", hn, ta, tb, op);
      buf_puts(b, "("); emit_expr(c, v, b); buf_puts(b, ")");
    }
    buf_puts(b, "; ");
    buf_printf(b, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s); ", ta, ta, hash_box_cls(rt));
    buf_printf(b, "sp_%sHash_set(_t%d, _t%d, _t%d); }\n", hn, ta, tb, tv);
    return;
  }

  if (ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) unsupported(c, id, "index operator assignment (array)");
    TyKind vt = comp_ntype(c, v);
    /* same operator table as the TY_POLY receiver path below */
    const char *pf =
        sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub" :
        sp_streq(op, "*") ? "sp_poly_mul" : sp_streq(op, "/") ? "sp_poly_div" :
        sp_streq(op, "%") ? "sp_poly_mod" : sp_streq(op, "**") ? "sp_poly_pow" :
        sp_streq(op, "<<") ? "sp_poly_shl" : sp_streq(op, ">>") ? "sp_poly_shr" :
        sp_streq(op, "&") ? "sp_poly_band" : sp_streq(op, "|") ? "sp_poly_bor" :
        sp_streq(op, "^") ? "sp_poly_bxor" : NULL;
    emit_indent(b, indent);
    /* The receiver temp is GC-rooted when the receiver expression itself
       can allocate (`make_array()[i] += v`): such a fresh temporary has no
       other root, and the RHS / fold helpers below (sp_poly_<op>,
       sp_str_plus/repeat) can trigger a collection before the closing
       sp_*Array_set. A bare local/ivar read is already rooted at its slot,
       so it skips the push (keeps hot emissions byte-identical). */
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); iow_emit_recv(c, recv, b);
    if (subtree_may_allocate(nt, recv)) buf_printf(b, "; SP_GC_ROOT(_t%d)", ta);
    buf_printf(b, "; sp_int _t%d = ", tb); iow_emit_key(c, argv[0], b, IOW_KEY_INT, TY_INT);
    buf_puts(b, "; ");
    if (rt == TY_POLY_ARRAY) {
      /* poly slot: fold via the tag-dispatching operator on boxed operands,
         like the TY_POLY receiver path below. */
      if (!pf) unsupported(c, id, "index operator assignment (poly array, operator)");
      buf_printf(b, "sp_PolyArray_set(_t%d, _t%d, %s(sp_PolyArray_get(_t%d, _t%d), ",
                 ta, tb, pf, ta, tb);
      emit_boxed(c, v, b); buf_puts(b, ")); }\n");
    }
    else if (rt == TY_STR_ARRAY) {
      /* String slots take String ops only: `+`/`<<` concatenate, `*` repeats
         (String#*). The native fallthrough (`char* << char*`, `char* * int`)
         never compiles, so anything else is rejected here explicitly. */
      if (sp_streq(op, "+") || sp_streq(op, "<<")) {
        int tc = ++g_tmp, td = ++g_tmp;
        buf_printf(b, "sp_StrArray_set(_t%d, _t%d, ({ const char *_t%d = sp_StrArray_get(_t%d, _t%d); "
                   "SP_GC_ROOT(_t%d); const char *_t%d = ", ta, tb, tc, ta, tb, tc, td);
        if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, v, b); buf_puts(b, ")"); }
        else emit_expr(c, v, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_str_plus(_t%d, _t%d); })); }\n", td, tc, td);
      }
      else if (sp_streq(op, "*")) {
        buf_printf(b, "sp_StrArray_set(_t%d, _t%d, sp_str_repeat(sp_StrArray_get(_t%d, _t%d), ",
                   ta, tb, ta, tb);
        emit_int_expr(c, v, b);
        buf_puts(b, ")); }\n");
      }
      else unsupported(c, id, "index operator assignment (string array, operator)");
    }
    else if (vt == TY_POLY && pf) {
      /* typed int/float slot, poly RHS: box the slot, fold via the dynamic
         operator, unbox back to the slot type -- exactly what the plain
         `a[i] = a[i] + rhs` form emits (issue: `double + sp_RbVal`). */
      const char *box = (rt == TY_FLOAT_ARRAY) ? "sp_box_float" : "sp_box_int";
      const char *unbox = (rt == TY_FLOAT_ARRAY) ? "sp_poly_to_f" : "sp_poly_to_i";
      buf_printf(b, "sp_%sArray_set(_t%d, _t%d, %s(%s(%s(sp_%sArray_get(_t%d, _t%d)), ",
                 k, ta, tb, unbox, pf, box, k, ta, tb);
      emit_boxed(c, v, b); buf_puts(b, "))); }\n");
    }
    else if (vt == TY_POLY) {
      /* shift/bitwise on an int slot with a poly RHS: unbox the RHS */
      buf_printf(b, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d) %s sp_poly_to_i(",
                 k, ta, tb, k, ta, tb, op);
      emit_boxed(c, v, b); buf_puts(b, ")); }\n");
    }
    else {
      buf_printf(b, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d) %s ", k, ta, tb, k, ta, tb, op);
      buf_puts(b, "("); emit_expr(c, v, b); buf_puts(b, ")); }\n");
    }
    return;
  }
  if (rt == TY_POLY) {
    /* poly receiver: dispatch get/op/set based on key type */
    TyKind kt = comp_ntype(c, argv[0]);
    TyKind vt = comp_ntype(c, v);
    emit_indent(b, indent);
    if (kt == TY_SYMBOL) {
      int tc = ++g_tmp;
      buf_printf(b, "{ sp_RbVal _t%d = ", ta); iow_emit_recv(c, recv, b);
      buf_printf(b, "; sp_sym _t%d = ", tb); iow_emit_key(c, argv[0], b, IOW_KEY_RAW, TY_UNKNOWN); buf_puts(b, "; ");
      buf_printf(b, "sp_RbVal _t%d = sp_poly_get_sym(_t%d, _t%d);", tc, ta, tb);
      buf_printf(b, " sp_poly_set_sym(_t%d, _t%d, sp_box_int(_t%d.v.i %s (", ta, tb, tc, op);
      emit_expr(c, v, b); buf_puts(b, "))); }\n");
    }
    else if (kt == TY_STRING) {
      int tc = ++g_tmp;
      buf_printf(b, "{ sp_RbVal _t%d = ", ta); iow_emit_recv(c, recv, b);
      buf_printf(b, "; const char *_t%d = ", tb); iow_emit_key(c, argv[0], b, IOW_KEY_RAW, TY_UNKNOWN); buf_puts(b, "; ");
      buf_printf(b, "sp_RbVal _t%d = sp_poly_get_str(_t%d, _t%d);", tc, ta, tb);
      buf_printf(b, " sp_poly_set_str(_t%d, _t%d, sp_box_int(_t%d.v.i %s (", ta, tb, tc, op);
      emit_expr(c, v, b); buf_puts(b, "))); }\n");
    }
    else {
      /* int or fully dynamic key (e.g. `m[a][c] += v` on a nested array):
         read the slot polymorphically, fold via the tag-dispatching
         sp_poly_<op> (handles int/float/bigint/str), and store back through
         the poly setter. Mirrors the IndexOrWrite poly-receiver path. */
      const char *pf =
          sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub" :
          sp_streq(op, "*") ? "sp_poly_mul" : sp_streq(op, "/") ? "sp_poly_div" :
          sp_streq(op, "%") ? "sp_poly_mod" : sp_streq(op, "**") ? "sp_poly_pow" :
          sp_streq(op, "<<") ? "sp_poly_shl" : sp_streq(op, ">>") ? "sp_poly_shr" :
          sp_streq(op, "&") ? "sp_poly_band" : sp_streq(op, "|") ? "sp_poly_bor" :
          sp_streq(op, "^") ? "sp_poly_bxor" : NULL;
      if (!pf) unsupported(c, id, "index operator assignment (poly-recv, operator)");
      int tc = ++g_tmp;
      if (kt == TY_INT) {
        buf_printf(b, "{ sp_RbVal _t%d = ", ta); iow_emit_recv(c, recv, b);
        buf_printf(b, "; sp_int _t%d = ", tb); iow_emit_key(c, argv[0], b, IOW_KEY_INT, TY_INT); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tc, ta, tb);
        buf_printf(b, " sp_poly_arr_set_hash(_t%d, _t%d, %s(_t%d, ", ta, tb, pf, tc);
        emit_boxed(c, v, b); buf_puts(b, ")); }\n");
      }
      else {
        buf_printf(b, "{ sp_RbVal _t%d = ", ta); iow_emit_recv(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tb); iow_emit_key(c, argv[0], b, IOW_KEY_BOXED, TY_POLY); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_index_poly(_t%d, _t%d);", tc, ta, tb);
        buf_printf(b, " sp_poly_set_poly(_t%d, _t%d, %s(_t%d, ", ta, tb, pf, tc);
        emit_boxed(c, v, b); buf_puts(b, ")); }\n");
      }
    }
    return;
  }
  unsupported(c, id, "index operator assignment");
}

/* h[k] &&= v  /  h[k] ||= v  /  a[i] &&= v  /  a[i] ||= v.
   IndexAndWriteNode / IndexOrWriteNode. Receiver and key evaluated once. */
void emit_index_and_or_write(Compiler *c, int id, Buf *b, int indent, int is_or) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int v = nt_ref(nt, id, "value");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (argc != 1) { unsupported(c, id, is_or ? "index-or-write" : "index-and-write"); return; }
  TyKind rt = comp_ntype(c, recv);
  int ta = ++g_tmp, tb = ++g_tmp;

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) { unsupported(c, id, "index and/or write (unknown hash)"); return; }
    TyKind kt = ty_hash_key(rt);
    TyKind vt = ty_hash_val(rt);
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; %s _t%d = ", c_type_name(kt), tb); emit_hash_key(c, argv[0], kt, b);
    buf_puts(b, "; ");
    if (vt == TY_POLY) {
      buf_printf(b, "if (%ssp_poly_truthy(sp_%sHash_get(_t%d, _t%d))) sp_%sHash_set(_t%d, _t%d, ",
                 is_or ? "!" : "", hn, ta, tb, hn, ta, tb);
      emit_boxed(c, v, b);
      buf_puts(b, ")");
    }
    else {
      buf_printf(b, "if (%ssp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_set(_t%d, _t%d, ",
                 is_or ? "!" : "", hn, ta, tb, hn, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    buf_puts(b, "; }\n");
    return;
  }

  if (ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) { unsupported(c, id, "index and/or write (array kind)"); return; }
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; sp_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
    buf_puts(b, "; ");
    if (rt == TY_INT_ARRAY) {
      /* int slots are nil only out of bounds (0 is truthy); ||= writes when
         nil, &&= when present. Compare with == / != to avoid `!x != NIL`. */
      buf_printf(b, "if (sp_IntArray_get(_t%d, _t%d) %s SP_INT_NIL) sp_IntArray_set(_t%d, _t%d, ",
                 ta, tb, is_or ? "==" : "!=", ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (rt == TY_FLOAT_ARRAY) {
      buf_printf(b, "if (%ssp_float_is_nil(sp_FloatArray_get(_t%d, _t%d))) sp_FloatArray_set(_t%d, _t%d, ",
                 is_or ? "" : "!", ta, tb, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (rt == TY_STR_ARRAY) {
      buf_printf(b, "if (%ssp_StrArray_get(_t%d, _t%d)) sp_StrArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (rt == TY_POLY_ARRAY) {
      buf_printf(b, "if (%ssp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) sp_PolyArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
      emit_boxed(c, v, b);
      buf_puts(b, ")");
    }
    else {
      unsupported(c, id, "index and/or write (array type)"); return;
    }
    buf_puts(b, "; }\n");
    return;
  }

  unsupported(c, id, is_or ? "index-or-write" : "index-and-write");
}
