#include "codegen_internal.h"

void emit_puts_one(Compiler *c, int arg, Buf *b, int indent) {
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
    buf_printf(b, "{ mrb_int _t%d = ", tv); emit_int_expr(c, arg, b);
    buf_printf(b, "; if (_t%d == SP_INT_NIL) putchar('\\n'); else printf(\"%%lld\\n\", (long long)_t%d); }\n", tv, tv);
  }
  else if (t == TY_BIGINT) {
    buf_puts(b, "{ const char *_bs = sp_bigint_to_s("); emit_expr(c, arg, b);
    buf_puts(b, "); if (_bs) fputs(_bs, stdout); putchar('\\n'); }\n");
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
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) ", ti, ti, k, a, ti);
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
  else if (t == TY_TIME) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Time _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; const char *_ts = sp_time_inspect_v(_t%d); fputs(_ts, stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_RANGE) {
    /* puts of a Range renders its to_s ("first..last"), then a newline. */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); putchar('\\n'); }\n", tv);
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
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) sp_poly_puts(sp_PolyArray_get(_t%d, _t%d)); }\n",
               ti, ti, ta, ti, ta, ti);
    free(ab.p);
  }
  else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
    /* an object with #to_s (user-defined or a generated struct/data one) */
    const char *cn = obj_str_cname(c, ty_object_class(t), 0);
    buf_puts(b, "{ const char *_ps = (const char *)(");
    buf_printf(b, "sp_%s_to_s((sp_%s *)", cn, cn);
    const char *rty = nt_type(c->nt, arg);
    if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
      emit_expr(c, arg, b);
    }
    else {
      int tt = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
      buf_printf(g_pre, " _t%d = ", tt);
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arg, &rb);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "_t%d", tt);
    }
    buf_puts(b, ")); if (_ps) fputs(_ps, stdout); if (!_ps || !*_ps || _ps[strlen(_ps)-1] != '\\n') putchar('\\n'); }\n");
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
    if (!diagnose_eval_call(c, arg))
      unsupported(c, arg, "puts argument");
  }
}
void emit_print_one(Compiler *c, int arg, Buf *b, int indent) {
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
  else if (t == TY_POLY || ty_is_object(t)) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_RbVal _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; const char *_ps%d = sp_poly_to_s(_t%d); if (_ps%d) fputs(_ps%d, stdout); }\n", tv, tv, tv, tv);
  }
  else if (t == TY_RANGE) {
    /* print of a Range renders its to_s ("first..last"), no newline. */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); }\n", tv);
  }
  else {
    if (!diagnose_eval_call(c, arg))
      unsupported(c, arg, "print argument");
  }
}
void emit_p_one(Compiler *c, int arg, Buf *b, int indent) {
  TyKind t = comp_ntype(c, arg);
  /* `p x.class` prints the class name bare (it is a Class, not a String). */
  if (t == TY_STRING && nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "CallNode") &&
      nt_str(c->nt, arg, "name") && sp_streq(nt_str(c->nt, arg, "name"), "class") &&
      nt_ref(c->nt, arg, "receiver") >= 0) {
    emit_indent(b, indent);
    buf_puts(b, "fputs("); emit_expr(c, arg, b); buf_puts(b, ", stdout); putchar('\\n');\n");
    return;
  }
  emit_indent(b, indent);
  if (t == TY_INT) {
    /* p of a nullable int at the sentinel prints "nil" */
    int tv = ++g_tmp;
    buf_printf(b, "{ mrb_int _t%d = ", tv); emit_int_expr(c, arg, b);
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
  else if (t == TY_COMPLEX) {
    buf_puts(b, "fputs(sp_complex_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_RATIONAL) {
    buf_puts(b, "fputs(sp_rational_inspect("); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else if (t == TY_TIME) {
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Time _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_time_inspect_v(_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_RANGE) {   /* a Range inspects as "first..last" / "first...last" */
    int tv = ++g_tmp;
    buf_printf(b, "{ sp_Range _t%d = ", tv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_Range_inspect(&_t%d), stdout); putchar('\\n'); }\n", tv);
  }
  else if (t == TY_CLASS) {   /* a Class/Module inspects as its name */
    int cv = ++g_tmp;
    buf_printf(b, "{ sp_Class _t%d = ", cv); emit_expr(c, arg, b);
    buf_printf(b, "; fputs(sp_class_to_s(_t%d), stdout); putchar('\\n'); }\n", cv);
  }
  else if (t == TY_NIL || t == TY_VOID) {
    buf_puts(b, "(void)("); emit_expr(c, arg, b); buf_puts(b, "); fputs(\"nil\\n\", stdout);\n");
  }
  else if (nt_type(c->nt, arg) && sp_streq(nt_type(c->nt, arg), "ArrayNode") &&
           ({ int _n = 0; nt_arr(c->nt, arg, "elements", &_n); _n == 0; })) {
    buf_puts(b, "fputs(\"[]\\n\", stdout);\n");  /* p [] */
  }
  else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 1)) {
    /* an object with #inspect (user-defined or a generated struct/data one) */
    const char *cn = obj_str_cname(c, ty_object_class(t), 1);
    buf_printf(b, "fputs(sp_%s_inspect((sp_%s *)", cn, cn); emit_expr(c, arg, b);
    buf_puts(b, "), stdout); putchar('\\n');\n");
  }
  else {
    if (!diagnose_eval_call(c, arg))
      unsupported(c, arg, "p argument");
  }
}

int emit_output_call(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv >= 0) return 0;
  if (comp_method_index(c, name) >= 0) return 0; /* user method shadows builtin */
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
  if (sp_streq(name, "p"))     { for (int k = 0; k < argc; k++) emit_p_one(c, argv[k], b, indent); return 1; }
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
  /* trap(...) stmt: no-op (Spinel has no signal-handler runtime) */
  if (sp_streq(name, "trap") && argc >= 1) return 1;
  if (sp_streq(name, "exit") || sp_streq(name, "exit!")) {
    emit_indent(b, indent);
    if (argc == 0) buf_puts(b, "exit(0);\n");
    else { buf_puts(b, "exit((int)("); emit_expr(c, argv[0], b); buf_puts(b, "));\n"); }
    return 1;
  }
  if (sp_streq(name, "abort")) {
    emit_indent(b, indent);
    if (argc >= 1) {
      buf_puts(b, "fputs(");
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_STRING) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ", stderr); fputc('\\n', stderr);\n");
      emit_indent(b, indent);
    }
    buf_puts(b, "exit(1);\n");
    return 1;
  }
  if (sp_streq(name, "srand")) {
    emit_indent(b, indent);
    if (argc == 0) buf_puts(b, "srand((unsigned)time(NULL));\n");
    else { buf_puts(b, "srand((unsigned)("); emit_expr(c, argv[0], b); buf_puts(b, "));\n"); }
    return 1;
  }
  if (sp_streq(name, "rand") && argc >= 1) {
    /* stmt-level rand: evaluate for side effects; result unused */
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
    for (int k = 0; k < argc; k++) {
      if (k == kw_idx) continue;
      if (suppress) { emit_indent(b, indent); buf_puts(b, "(void)("); emit_expr(c, argv[k], b); buf_puts(b, ");\n"); continue; }
      TyKind at = comp_ntype(c, argv[k]);
      emit_indent(b, indent); buf_puts(b, "fputs(");
      if (at == TY_STRING) emit_expr(c, argv[k], b);
      else if (at == TY_INT) { buf_puts(b, "sp_int_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "sp_float_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else if (at == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[k], b); buf_puts(b, "), \"\")"); }
      buf_puts(b, ", stderr); fputc('\\n', stderr);\n");
    }
    return 1;
  }
  return 0;
}

/* ---- assignment ---- */

void emit_assign(Compiler *c, int id, Buf *b, int indent) {
  const char *nm = nt_str(c->nt, id, "name");
  int v = nt_ref(c->nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  /* `x = y = nil`: emit the inner writes as their own statements (each target
     renders nil for its own slot type), then write nil here too. */
  {
    int ncb = comp_nil_chain_bottom(c->nt, v);
    if (ncb >= 0) { emit_stmt_inner(c, v, b, indent); v = ncb; }
  }
  emit_indent(b, indent);
  /* A TY_PROC value lives in an int cell as (mrb_int)(uintptr_t)sp_Proc*. The
     write target must be the raw cell deref (an lvalue) with the pointer
     re-encoded as int; emit_local_ref's read form casts to sp_Proc* and is not
     assignable (self-recursive `f = proc { f.call(...) }`). A heap-object cell
     is a typed pointer whose deref is already assignable, so it takes the
     ordinary `emit_local_ref = value` path below. */
  int laundered_cell = lv && lv->type == TY_PROC;
  if (laundered_cell &&
      (lv->is_cell || (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm)))) {
    if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm))
      buf_printf(b, "*((%s *)_cap)->%s", g_cap_struct, nm);
    else
      buf_printf(b, "*_cell_%s", nm);
    buf_puts(b, " = (mrb_int)(uintptr_t)(");
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
    /* a mutable-string local: wrap the (const char*) RHS in a fresh sp_String
       so later `<<` appends are amortized O(1). */
    buf_puts(b, "sp_String_new("); emit_expr(c, v, b); buf_puts(b, ")");
  }
  else if (is_empty_array && lv && array_kind(lv->type)) {
    /* `a = []` -> a new array of the variable's resolved element type */
    buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
  }
  else if (is_empty_array && lv && lv->type == TY_POLY_ARRAY) {
    buf_puts(b, "sp_PolyArray_new()");
  }
  else if (is_empty_array && lv && ty_is_obj_array(lv->type)) {
    /* `a = []` for a narrowed object array: an sp_PtrArray whose elements are
       GC-marked as ordinary heap objects. */
    buf_puts(b, "sp_PtrArray_new()");
  }
  else if (lv && ty_is_obj_array(lv->type) && vty && sp_streq(vty, "ArrayNode")) {
    /* `a = [X.new, ...]` for a narrowed object array: build the sp_PtrArray
       with the unboxed object pointers (rooted while constructing). */
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);", t, t);
    int en = 0; const int *el = nt_arr(c->nt, v, "elements", &en);
    for (int e = 0; e < en; e++) {
      buf_printf(b, " sp_PtrArray_push(_t%d, ", t); emit_expr(c, el[e], b); buf_puts(b, ");");
    }
    buf_printf(b, " _t%d; })", t);
  }
  else if (is_hash_new && nt_ref(c->nt, v, "block") >= 0) {
    /* Hash.new { |hash, key| ... }: emit through emit_call so the dproc
       function + sp_StrPolyHash_new_dproc path runs. */
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
  else if (lv && (lv->type == TY_INT || lv->type == TY_BOOL) && comp_ntype(c, v) == TY_POLY) {
    /* scalar slot, poly RHS (e.g. `x = (a + b) * 2` with poly a/b): coerce. */
    buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")");
  }
  else if (lv && lv->type == TY_FLOAT && comp_ntype(c, v) == TY_POLY) {
    buf_puts(b, "sp_poly_to_f("); emit_expr(c, v, b); buf_puts(b, ")");
  }
  else if (lv && lv->type == TY_STRING && comp_ntype(c, v) == TY_POLY) {
    /* string slot, poly RHS (holds a string at runtime): coerce */
    buf_puts(b, "sp_poly_to_s("); emit_expr(c, v, b); buf_puts(b, ")");
  }
  else {
    emit_expr(c, v, b);
  }
  buf_puts(b, ";\n");
}

void emit_op_assign(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  const char *op = nt_str(nt, id, "binary_operator");
  int v = nt_ref(nt, id, "value");
  LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
  TyKind t = lv ? lv->type : TY_UNKNOWN;
  const char *en = rename_local(nm);
  emit_indent(b, indent);

  /* A captured/cell var: x op= v is x = x op v through the cell deref. Int and
     float cells exist (a float capture rides a native mrb_float cell, so its deref
     is a real mrb_float lvalue); pointer/proc cells take the int_arith path below. */
  int celled = (lv && lv->is_cell) || (g_cap_struct && g_cap_names && nameset_has(g_cap_names, nm));
  if (celled) {
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    /* A bigint cell (an accumulator widened to bigint in promote mode and then
       captured): route through the bigint helpers, not the int ones, which
       would truncate the pointer-sized value. The cell stores the sp_Bigint*
       as its int-sized slot, so read/write are still through emit_local_ref. */
    if (t == TY_BIGINT) {
      const char *bfn = bigint_arith_fn(op);
      if (bfn) {
        TyKind vt = comp_ntype(c, v);
        buf_printf(b, "%s(", bfn); emit_local_ref(c, id, nm, b); buf_puts(b, ", ");
        if (vt == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, v, b); buf_puts(b, ")"); }
        else if (vt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
        else emit_expr(c, v, b);
        buf_puts(b, ");\n");
        return;
      }
    }
    if (t == TY_INT && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*"))) {
      emit_local_ref(c, id, nm, b); buf_printf(b, " %s ", op); emit_expr(c, v, b); buf_puts(b, ";\n");
      return;
    }
    if (t == TY_FLOAT && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") || sp_streq(op, "/"))) {
      TyKind vt = comp_ntype(c, v);
      emit_local_ref(c, id, nm, b); buf_printf(b, " %s ", op);
      if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, v, b); buf_puts(b, ")"); }
      else emit_expr(c, v, b);
      buf_puts(b, ";\n");
      return;
    }
    /* A poly cell (an int local widened to poly in promote mode, then captured):
       route the arithmetic through the tag-dispatching sp_poly_<op>, boxing the
       rhs. Bitwise/shift coerce to int and re-box, mirroring the non-celled
       poly op-assign path. */
    if (t == TY_POLY) {
      const char *pfn = sp_streq(op, "+") ? "sp_poly_add"
                      : sp_streq(op, "-") ? "sp_poly_sub"
                      : sp_streq(op, "*") ? "sp_poly_mul"
                      : sp_streq(op, "/") ? "sp_poly_div"
                      : sp_streq(op, "%") ? "sp_poly_mod" : NULL;
      if (pfn) {
        buf_printf(b, "%s(", pfn); emit_local_ref(c, id, nm, b); buf_puts(b, ", ");
        emit_boxed(c, v, b); buf_puts(b, ");\n");
        return;
      }
      if (sp_streq(op, "<<") || sp_streq(op, ">>") ||
          sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^")) {
        TyKind vt = comp_ntype(c, v);
        buf_puts(b, "sp_box_int((sp_poly_to_i("); emit_local_ref(c, id, nm, b);
        buf_printf(b, ") %s (", op);
        if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")"); }
        else emit_expr(c, v, b);
        buf_puts(b, ")));\n");
        return;
      }
    }
    const char *fn = int_arith_fn(op);
    if (fn) {
      int isdivmod = sp_streq(op, "/") || sp_streq(op, "%");
      buf_printf(b, "%s(", fn); emit_local_ref(c, id, nm, b); buf_puts(b, ", ");
      if (isdivmod) emit_int_divisor(c, v, b);
      else emit_expr(c, v, b);
      buf_puts(b, ");\n"); return;
    }
    emit_local_ref(c, id, nm, b); buf_printf(b, " %s ", op); emit_expr(c, v, b); buf_puts(b, ";\n");
    return;
  }

  if (t == TY_STRING && sp_streq(op, "+")) {
    buf_printf(b, "lv_%s = sp_str_concat(lv_%s, ", en, en);
    emit_expr(c, v, b); buf_puts(b, ");\n");
    return;
  }
  if (t == TY_INT && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*"))) {
    TyKind vt = comp_ntype(c, v);
    if (vt == TY_POLY) {
      buf_printf(b, "lv_%s %s= sp_poly_to_i(", en, op); emit_expr(c, v, b); buf_puts(b, ");\n");
    }
else {
      buf_printf(b, "lv_%s %s= ", en, op); emit_expr(c, v, b); buf_puts(b, ";\n");
    }
    return;
  }
  if (t == TY_INT) {
    const char *fn = int_arith_fn(op);
    if (fn) {
      int isdivmod = sp_streq(op, "/") || sp_streq(op, "%");
      buf_printf(b, "lv_%s = %s(lv_%s, ", en, fn, en);
      if (isdivmod) emit_int_divisor(c, v, b);
      else emit_expr(c, v, b);
      buf_puts(b, ");\n"); return;
    }
  }
  /* Bitwise op-assign on an int: shift/and/or/xor map straight to the C
     operator (fixed-width wrap, same as the binary `x << y` path). */
  if (t == TY_INT && (sp_streq(op, "<<") || sp_streq(op, ">>") ||
                      sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^"))) {
    TyKind vt = comp_ntype(c, v);
    buf_printf(b, "lv_%s = (lv_%s %s (", en, en, op);
    if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, "));\n");
    return;
  }
  if (t == TY_BIGINT) {
    const char *bfn = bigint_arith_fn(op);
    if (bfn) {
      TyKind vt = comp_ntype(c, v);
      buf_printf(b, "lv_%s = %s(lv_%s, ", en, bfn, en);
      if (vt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
      else emit_expr(c, v, b);
      buf_puts(b, ");\n"); return;
    }
  }
  if (t == TY_FLOAT && (sp_streq(op, "+") || sp_streq(op, "-") || sp_streq(op, "*") || sp_streq(op, "/"))) {
    TyKind vt = comp_ntype(c, v);
    buf_printf(b, "lv_%s %s= ", en, op);
    if (vt == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, v, b); buf_puts(b, ")"); }
    else emit_expr(c, v, b);
    buf_puts(b, ";\n");
    return;
  }
  if (t == TY_COMPLEX && (sp_streq(op, "+") || sp_streq(op, "*"))) {
    buf_printf(b, "lv_%s = sp_complex_%s(lv_%s, ", en, sp_streq(op, "+") ? "add" : "mul", en);
    emit_expr(c, v, b); buf_puts(b, ");\n");
    return;
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
      buf_printf(b, "lv_%s = sp_%s_%s((sp_%s *)lv_%s, _t%d);\n",
                 en, c->classes[defcls2].name, mc(ms2->name),
                 c->classes[defcls2].name, en, atmp2);
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
      buf_printf(b, "lv_%s = %s(lv_%s, ", en, pfn, en);
      emit_boxed(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  /* Poly local bitwise op-assign (`x &= v`, `x >>= v`, ...): the result is an
     int, re-boxed into the poly slot. The poly value is coerced via to_i, like
     the binary poly-bitwise path. */
  if (t == TY_POLY && (sp_streq(op, "<<") || sp_streq(op, ">>") ||
                       sp_streq(op, "|") || sp_streq(op, "&") || sp_streq(op, "^"))) {
    TyKind vt = comp_ntype(c, v);
    buf_printf(b, "lv_%s = sp_box_int((sp_poly_to_i(lv_%s) %s (", en, en, op);
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
      buf_printf(b, "lv_%s = sp_%sArray_%s(lv_%s, ", en, k, fn, en);
      if (vt == TY_UNKNOWN) buf_puts(b, "NULL");
      else emit_expr(c, v, b);
      buf_puts(b, ");\n");
      return;
    }
  }
  unsupported(c, id, "operator assignment");
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
      t == TY_PROC || t == TY_STRINGIO || t == TY_STRINGSCANNER || t == TY_MATCHDATA || t == TY_EXCEPTION ||
      t == TY_BIGINT || t == TY_REGEX || t == TY_CURRY || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_RANDOM ||
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
  if (!ty_is_object(rt)) return -1;
  int args = nt_ref(nt, pred, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !av || !nt_type(nt, av[0]) || !sp_streq(nt_type(nt, av[0]), "ConstantReadNode")) return -1;
  int target = comp_class_index(c, nt_str(nt, av[0], "name"));
  if (target < 0) return -1;
  int rcls = ty_object_class(rt);
  if (rcls == target) return 1;
  if (!sp_streq(nm, "instance_of?") && is_descendant(c, rcls, target)) return 1;
  return 0;
}

/* Scan every program-wide write to ivar `nm` ("@foo"): returns 0 when at least
   one write exists and all of them assign nil (statically falsy), -1 otherwise
   (no writes seen, a non-nil write, or an opaque write form). */
static int ivar_all_writes_nil(Compiler *c, const char *nm) {
  const NodeTable *nt = c->nt;
  if (!nm) return -1;
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
    buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", tmp, tmp);
  else if (sp_streq(cn, "Range"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id == SP_BUILTIN_RANGE)", tmp, tmp);
  else if (sp_streq(cn, "Array"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", tmp, tmp, tmp);
  else if (sp_streq(cn, "Hash"))
    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -13 && %s.cls_id >= -20)", tmp, tmp, tmp);
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

/* Recursive match condition for a (possibly nested) array pattern over the
   boxed value `arr` (an sp_RbVal C-expression): `arr` is an array of the right
   length, and each nested-array element is itself a correctly-shaped array.
   Element value/type checks beyond shape are left to the flat path. */
static void emit_pm_array_cond(Compiler *c, int pat, const char *arr, Buf *b) {
  const NodeTable *nt = c->nt;
  int apn = 0;
  const int *reqs = nt_arr(nt, pat, "requireds", &apn);
  int rest_nid = nt_ref(nt, pat, "rest");
  int has_rest = (rest_nid >= 0 && nt_type(nt, rest_nid) &&
                  sp_streq(nt_type(nt, rest_nid), "SplatNode"));
  buf_printf(b, "((%s).tag == SP_TAG_OBJ && sp_poly_length(%s) %s %dLL",
             arr, arr, has_rest ? ">=" : "==", apn);
  for (int i = 0; i < apn; i++) {
    int sub = -1;
    const char *rty = nt_type(nt, reqs[i]);
    if (rty && sp_streq(rty, "ArrayPatternNode")) sub = reqs[i];
    else if (rty && sp_streq(rty, "CapturePatternNode")) {
      int val = nt_ref(nt, reqs[i], "value");
      if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ArrayPatternNode")) sub = val;
    }
    if (sub >= 0) {
      /* the element accessor nests one level per recursion (arr grows), so build
         it in a Buf rather than a fixed buffer that would truncate. */
      Buf e; memset(&e, 0, sizeof e);
      buf_printf(&e, "sp_poly_index_poly(%s, sp_box_int(%dLL))", arr, i);
      buf_puts(b, " && "); emit_pm_array_cond(c, sub, e.p, b);
      free(e.p);
    }
  }
  buf_puts(b, ")");
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
      if (lo >= 0) { buf_printf(b, " && _t%d.v.i >= ", t); emit_int_expr(c, lo, b); }
      if (hi >= 0) { buf_printf(b, " && _t%d.v.i %s ", t, cmp); emit_int_expr(c, hi, b); }
      buf_puts(b, ")");
      return 1;
    }
    return 0;
  }
  if (sp_streq(pty, "ArrayPatternNode")) {
    int apn = 0;
    const int *reqs = nt_arr(nt, pat, "requireds", &apn);
    /* a nested array element needs a recursive shape check; route a poly array
       through the boxed recursive condition so `[1,5,4]` does not match
       `[a,[b,c],d]` on outer length alone. */
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
    if (has_nested) {
      if (pt == TY_POLY_ARRAY) {
        char box[48]; snprintf(box, sizeof box, "sp_box_poly_array(_t%d)", t);
        emit_pm_array_cond(c, pat, box, b);
        return 1;
      }
      /* a typed (int/float/str) array can't hold a sub-array element, so a
         pattern with a nested array can never match it. */
      buf_puts(b, "0");
      return 1;
    }
    /* Length check */
    int rest_nid = nt_ref(nt, pat, "rest");
    int has_rest = (rest_nid >= 0 && nt_type(nt, rest_nid) &&
                    sp_streq(nt_type(nt, rest_nid), "SplatNode"));
    if (has_rest)
      buf_printf(b, "(_t%d && _t%d->len >= %dLL)", t, t, (long long)apn);
    else
      buf_printf(b, "(_t%d && _t%d->len == %dLL)", t, t, (long long)apn);
    return 1;
  }
  if (sp_streq(pty, "CapturePatternNode")) {
    /* Check inner pattern's condition if any */
    int val = nt_ref(nt, pat, "value");
    if (val >= 0) return emit_pm_cond(c, val, t, pt, b);
    return 0;
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
    else if (sp_streq(rty, "ArrayPatternNode")) {
      int sub = ++g_tmp;
      emit_indent(b, indent);
      buf_printf(b, "sp_RbVal _t%d = %s;\n", sub, src.p);
      char se[24]; snprintf(se, sizeof se, "_t%d", sub);
      emit_pm_bind_poly(c, reqs[i], se, indent, b, sc);
    }
    else if (sp_streq(rty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, reqs[i], "target");
      if (tgt >= 0 && nt_type(nt, tgt) && sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) emit_pm_typed_assign(sc, lnm, src.p, b, indent);
      }
      int val = nt_ref(nt, reqs[i], "value");
      if (val >= 0 && nt_type(nt, val) && sp_streq(nt_type(nt, val), "ArrayPatternNode")) {
        int sub = ++g_tmp;
        emit_indent(b, indent);
        buf_printf(b, "sp_RbVal _t%d = %s;\n", sub, src.p);
        char se[24]; snprintf(se, sizeof se, "_t%d", sub);
        emit_pm_bind_poly(c, val, se, indent, b, sc);
      }
    }
    free(src.p);
  }
  int rest_nid = nt_ref(nt, pat, "rest");
  if (rest_nid >= 0 && nt_type(nt, rest_nid) && sp_streq(nt_type(nt, rest_nid), "SplatNode")) {
    int inner = nt_ref(nt, rest_nid, "expression");
    if (inner >= 0 && nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "LocalVariableTargetNode")) {
      const char *rnm = nt_str(nt, inner, "name");
      if (rnm) {
        Buf rsrc; memset(&rsrc, 0, sizeof rsrc);
        buf_printf(&rsrc, "sp_poly_slice(%s, %dLL, sp_poly_length(%s) - %dLL)", arr, apn, arr, apn);
        emit_pm_typed_assign(sc, rnm, rsrc.p, b, indent);
        free(rsrc.p);
      }
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
  emit_indent(b, indent); emit_ctype(c, pt, b);
  buf_printf(b, " _t%d = ", t);
  if (pred >= 0) emit_expr(c, pred, b);
  else buf_puts(b, default_value(pt));
  buf_puts(b, ";\n");
  if (needs_root(pt)) { emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", t); }

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
    if (pt == TY_MATCHDATA && sp_streq(pty, "HashPatternNode")) {
      char md[24]; snprintf(md, sizeof md, "_t%d", t);
      arm_t = emit_md_deconstruct_keys(b, indent + 1, md);
      arm_pt = TY_SYM_POLY_HASH;
    }

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
      buf_printf(b, "mrb_int _t%d = -1;\n", find_pos);
      emit_indent(b, indent + 1);
      buf_printf(b, "for (mrb_int _fi = 0; _t%d && _fi + %dLL <= _t%d->len; _fi++) {\n",
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
          emit_indent(b, indent + 1);
          buf_printf(b, "_t%d = _t%d && sp_%sHash_has_key(_t%d, ", hcond, hcond, hn, arm_t);
          emit_expr(c, key, b); buf_puts(b, ");\n");
          /* value class check: `k: Class` or `k: Class => v` */
          int classpat = -1;
          if (vpat >= 0 && nt_type(nt, vpat)) {
            if (sp_streq(nt_type(nt, vpat), "CapturePatternNode")) classpat = nt_ref(nt, vpat, "value");
            else if (sp_streq(nt_type(nt, vpat), "ConstantReadNode")) classpat = vpat;
          }
          if (classpat >= 0 && nt_type(nt, classpat) && sp_streq(nt_type(nt, classpat), "ConstantReadNode")) {
            if (hvt == TY_POLY) {
              int vtmp = ++g_tmp;
              emit_indent(b, indent + 1);
              buf_printf(b, "{ sp_RbVal _t%d = sp_%sHash_get(_t%d, ", vtmp, hn, arm_t);
              emit_expr(c, key, b); buf_puts(b, "); ");
              char vn[24]; snprintf(vn, sizeof vn, "_t%d", vtmp);
              Buf cw; memset(&cw, 0, sizeof cw);
              if (emit_poly_class_when(c, classpat, vn, &cw))
                buf_printf(b, "_t%d = _t%d && (%s);", hcond, hcond, cw.p ? cw.p : "1");
              free(cw.p);
              buf_puts(b, " }\n");
            }
            else {
              const char *cn = nt_str(nt, classpat, "name");
              if (cn && ty_matches_class(hvt, cn, 0) <= 0) {
                emit_indent(b, indent + 1); buf_printf(b, "_t%d = 0;\n", hcond);
              }
            }
          }
        }
      }
      buf_printf(&cond_buf, "_t%d", hcond);
      has_cond = 1;
    }
    else {
      has_cond = emit_pm_cond(c, pat, t, pt, &cond_buf);
    }
    /* For IfNode the pattern is always a binding (LV), guard is separate */
    if (sp_streq(pty, "IfNode")) has_cond = 0;

    int body_indent = indent + 1;
    if (has_cond) {
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
      if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); } else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
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
            if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); } else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
          }
        }
      }
    }
    else if (sp_streq(pty, "CapturePatternNode")) {
      int tgt = nt_ref(nt, pat, "target");
      if (tgt >= 0 && nt_type(nt, tgt) &&
          sp_streq(nt_type(nt, tgt), "LocalVariableTargetNode")) {
        const char *lnm = nt_str(nt, tgt, "name");
        if (lnm) { emit_indent(b, body_indent); buf_printf(b, "lv_%s = ", lnm); LocalVar *plv = scope_local(comp_scope_of(c, id), lnm); if (plv && plv->type == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) { char ex[24]; snprintf(ex, sizeof ex, "_t%d", t); Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, pt, ex, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); } else buf_printf(b, "_t%d", t); buf_puts(b, ";\n"); }
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
          }
          if (!lnm) continue;
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
      }
    }
    /* IntegerNode/StringNode/SymbolNode/ConstantReadNode: value-only, no binding */

    /* --- ArrayPatternNode destructuring --- */
    if (array_pat >= 0) {
      TyKind arr_t = ty_is_array(pt) ? pt : TY_INT_ARRAY;
      const char *k = (arr_t == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_t);
      if (!k) k = "Int";
      if (sp_streq(k, "Poly")) {
        /* poly array: bind (possibly nested) targets recursively, indexing
           through the boxed receiver so nested typed sub-arrays read correctly */
        char te[48]; snprintf(te, sizeof te, "sp_box_poly_array(_t%d)", t);
        emit_pm_bind_poly(c, array_pat, te, body_indent, b, comp_scope_of(c, id));
      }
      else {
      int apn = 0;
      const int *reqs = nt_arr(nt, array_pat, "requireds", &apn);
      int rest_nid = nt_ref(nt, array_pat, "rest");
      for (int i = 0; i < apn; i++) {
        const char *lty2 = nt_type(nt, reqs[i]);
        if (!lty2 || !sp_streq(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, reqs[i], "name");
        if (!lnm) continue;
        emit_indent(b, body_indent);
        buf_printf(b, "lv_%s = ", lnm);
        LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
        char gx[64]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, %dLL)", k, t, i);
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
            emit_indent(b, body_indent);
            buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, %dLL, _t%d->len - %dLL);\n",
                       rnm, k, t, (long long)apn, t, (long long)apn);
          }
        }
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
      /* required LV targets = the matched window elements */
      for (int j = 0; j < rn; j++) {
        const char *lty2 = nt_type(nt, reqs[j]);
        if (!lty2 || !sp_streq(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, reqs[j], "name");
        if (!lnm) continue;
        emit_indent(b, body_indent);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %dLL);\n",
                   lnm, find_k, find_arr, find_pos, j);
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
      else if (tail) emit_stmts_tail(c, stmts, b, body_indent + 1);
      else { emit_stmts(c, stmts, b, body_indent + 1); emit_indent(b, body_indent + 1); buf_printf(b, "goto _pm_%d;\n", lbl); }
      emit_indent(b, body_indent); buf_puts(b, "}\n");
    }
    else {
      if (value_cr >= 0) { emit_pm_body_value(c, stmts, rt, value_cr, b, body_indent); emit_indent(b, body_indent); buf_printf(b, "goto _pm_%d;\n", lbl); }
      else if (tail) emit_stmts_tail(c, stmts, b, body_indent);
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

  if (!tail || value_cr >= 0) { emit_indent(b, indent); buf_printf(b, "_pm_%d:;\n", lbl); }
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
    emit_ctype(c, pt, b);
    buf_printf(b, " _t%d = ", t);
    emit_expr(c, pred, b);
    buf_puts(b, ";\n");
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
          /* RationalNode: `when 0r` — matches integer iff denominator==1 */
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
          else if (cn2) {
            int yes = ty_matches_class(pt, cn2, 0);
            buf_printf(b, "%d", yes > 0 ? 1 : 0);
          }
          else {
          int reidx = re_lit_index(c, conds[j]);
          if (reidx >= 0 && pt == TY_STRING) {
            buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _t%d)", reidx, t);
          }
          else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
            /* `when lo..hi` is range membership, not equality */
            int tr = ++g_tmp;
            buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
            /* sp_range_include takes mrb_int; coerce a poly scrutinee. */
            if (pt == TY_POLY) buf_printf(b, "; sp_range_include(&_t%d, sp_poly_to_i(_t%d)); })", tr, t);
            else buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
          }
          else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
            /* a when value of a different comparable family never matches */
            buf_puts(b, "0");
          }
          else if (pt == TY_STRING) {
            buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
          }
          else if (pt == TY_POLY) {
            buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")");
          }
          else {
            buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")");
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

/* Emit `_crN = <branch's last value>` (boxed to the case's result type when
   that is poly), after the branch's leading statements. */
void emit_case_branch_value(Compiler *c, int stmts, TyKind rt, int cr, Buf *b) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *bb = stmts >= 0 ? nt_arr(nt, stmts, "body", &n) : NULL;
  for (int k = 0; k < n - 1; k++) emit_stmt(c, bb[k], b, 0);
  /* a value-less tail (nil/void/unknown -- e.g. an arm that is a writer
     call): run it as a statement and leave the arm value at the slot
     default, mirroring the if-as-value emitter. Assigning it would emit
     `_crN = <void call>` and fail to compile. */
  TyKind lt2 = n > 0 ? comp_ntype(c, bb[n - 1]) : TY_NIL;
  if (n > 0 && (lt2 == TY_NIL || lt2 == TY_VOID || lt2 == TY_UNKNOWN)) {
    emit_stmt(c, bb[n - 1], b, 0);
    buf_printf(b, "_cr%d = %s; ", cr, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
    return;
  }
  buf_printf(b, "_cr%d = ", cr);
  if (n > 0) { if (rt == TY_POLY) emit_boxed(c, bb[n - 1], b); else emit_expr(c, bb[n - 1], b); }
  else buf_puts(b, rt == TY_POLY ? "sp_box_nil()" : default_value(rt));
  buf_puts(b, "; ");
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
    emit_ctype(c, pt, b); buf_printf(b, " _t%d = ", t); emit_expr(c, pred, b); buf_puts(b, "; ");
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
        if (cn2 && pt == TY_POLY) {
          char tmp[32]; snprintf(tmp, sizeof tmp, "_t%d", t);
          if (!emit_poly_class_when(c, conds[j], tmp, b)) buf_puts(b, "0");
        }
        else if (cn2 && ty_is_object(pt)) {
          int cid = ty_object_class(pt); int tcid = comp_class_index(c, cn2);
          int yes = (tcid >= 0) && (cid == tcid || is_descendant(c, cid, tcid));
          buf_printf(b, "%d", yes ? 1 : 0);
        }
        else if (cn2) { int yes = ty_matches_class(pt, cn2, 0); buf_printf(b, "%d", yes > 0 ? 1 : 0); }
        else {
        int reidx = re_lit_index(c, conds[j]);
        if (reidx >= 0 && pt == TY_STRING) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _t%d)", reidx, t); }
        else if (comp_ntype(c, conds[j]) == TY_RANGE && pt != TY_STRING) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, conds[j], b);
          if (pt == TY_POLY) buf_printf(b, "; sp_range_include(&_t%d, sp_poly_to_i(_t%d)); })", tr, t);
          else buf_printf(b, "; sp_range_include(&_t%d, _t%d); })", tr, t);
        }
        else if (eq_family(pt) && eq_family(comp_ntype(c, conds[j])) && eq_family(pt) != eq_family(comp_ntype(c, conds[j]))) {
          buf_puts(b, "0");
        }
        else if (pt == TY_STRING) { buf_printf(b, "sp_str_eq(_t%d, ", t); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
        else if (pt == TY_POLY) { buf_printf(b, "sp_poly_eq(_t%d, ", t); emit_boxed(c, conds[j], b); buf_puts(b, ")"); }
        else { buf_printf(b, "(_t%d == ", t); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
        } /* close non-ConstantReadNode else */
      }
      else { buf_puts(b, "("); emit_expr(c, conds[j], b); buf_puts(b, ")"); }
    }
    buf_puts(b, ") { ");
    emit_case_branch_value(c, nt_ref(nt, wn, "statements"), rt, cr, b);
    buf_puts(b, "} ");
  }
  if (else_c >= 0) {
    buf_puts(b, "else { ");
    emit_case_branch_value(c, nt_ref(nt, else_c, "statements"), rt, cr, b);
    buf_puts(b, "} ");
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
          "strip!","chomp!","slice!","squeeze!","force_encoding", NULL};
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
      buf_printf(b, "mrb_int _t%d = sp_str_length_m(", ht); emit_expr(c, hr, b); buf_puts(b, ");\n");
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
  TyKind ct = comp_ntype(c, coll);

  if (ct == TY_RANGE && nt_type(nt, coll) && sp_streq(nt_type(nt, coll), "RangeNode")) {
    /* for v in lo..hi -- a plain counted loop. Under --int-overflow=promote the
       counter and/or endpoints may be widened to poly, so coerce each endpoint
       with sp_poly_to_i when its static type is poly/bigint, and when the
       counter slot is poly drive the loop with a fresh mrb_int temp and re-box
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
    emit_indent(b, indent); buf_puts(b, "{ mrb_int ");
    buf_printf(b, "_t%d = ", thi);
    if (rpoly) buf_puts(b, "sp_poly_to_i(");
    emit_expr(c, rref, b);
    if (rpoly) buf_puts(b, ")");
    buf_puts(b, ";\n");
    if (cpoly) {
      int tc = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "for (mrb_int _t%d = ", tc);
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
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
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
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k ? k : "Poly", ta, ti);
    emit_indent(b, indent + 2);
    buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", vn, k ? k : "Poly", ta, ti);
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  emit_indent(b, indent); buf_puts(b, "/* unsupported for-loop collection */\n");
}

/* Emit `node` (statically poly) coerced to the non-poly return/slot type `t`.
   int/bool/float go through the converting sp_poly_to_i/f (matching the legacy
   scalar-return coercion; mrb_bool is int-backed). Strings, objects, and every
   other pointer-backed reference (arrays, hashes, procs, fibers, ...) unbox via
   emit_unbox_text: a string reads `.v.s` (a nil box has a zeroed union, so this
   is NULL and `String?` round-trips); a pointer reads `(T *)(...).v.p`. The few
   by-value types (Range/Time/Complex/...) have no `.v.p` form, so they fall back
   to a plain emit (no coercion was applied for them before either, and no such
   poly-bodied return arises). Used where a method's RBS return type is narrower
   than its poly body value (#1417). */
static void emit_unbox_node(Compiler *c, TyKind t, int node, Buf *b) {
  if (t == TY_INT || t == TY_BOOL) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, node, b); buf_puts(b, ")"); return; }
  if (t == TY_FLOAT)               { buf_puts(b, "sp_poly_to_f("); emit_expr(c, node, b); buf_puts(b, ")"); return; }
  const char *cn = c_type_name(t);
  if (t == TY_STRING || ty_is_object(t) || (cn && cn[0] && cn[strlen(cn) - 1] == '*')) {
    Buf tmp; memset(&tmp, 0, sizeof tmp);
    emit_expr(c, node, &tmp);
    emit_unbox_text(c, t, tmp.p ? tmp.p : "", b);
    free(tmp.p);
    return;
  }
  emit_expr(c, node, b);
}

/* A genuine poly body (TY_POLY) is unboxed into any narrower (non-poly) return
   slot (#1417). */
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

void emit_return(Compiler *c, int id, Buf *b, int indent) {
  int args = nt_ref(c->nt, id, "arguments");
  int n = 0;
  const int *a = args >= 0 ? nt_arr(c->nt, args, "arguments", &n) : NULL;

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
        TyKind r0 = comp_ntype(c, a[0]);
        if (g_ret_type == TY_POLY && r0 != TY_POLY) emit_boxed(c, a[0], b);
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
    if (g_exc_frame_depth > g_method_pr_exc_depth)
      buf_printf(b, "sp_exc_top -= %d; ", g_exc_frame_depth - g_method_pr_exc_depth);
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
      buf_printf(b, "_retf%d = 1; sp_exc_top -= %d; goto _ensure%d; }\n",
                 ctx->lid, pops, ctx->lid);
    }
    return;
  }

  emit_indent(b, indent);
  /* leaving through live begin/rescue frames: pop them, or their jmp_bufs
     dangle into this soon-dead C frame and the next raise longjmps into
     garbage (doom's SoundManager#[] early cache returns). */
  if (g_exc_frame_depth > 0) buf_printf(b, "sp_exc_top -= %d; ", g_exc_frame_depth);
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
    buf_puts(b, "return ");
    TyKind r0 = comp_ntype(c, a[0]);
    if (g_ret_type == TY_POLY && r0 != TY_POLY) emit_boxed(c, a[0], b);
    /* a poly return value feeding a narrower (non-poly) return slot -- e.g. a
       method(:sym) target pinned to mrb_int that returns a poly @ivar, or an
       RBS-typed String/object method whose body yields poly -- needs coercing. */
    else if (tail_needs_unbox(r0, g_ret_type)) emit_unbox_node(c, g_ret_type, a[0], b);
    else emit_tail_value(c, a[0], b);
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

void emit_stmt_inner(Compiler *c, int id, Buf *b, int indent);
void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent);

/* A rescue type name that conventionally catches "anything" in tests. */
int rescue_is_catchall_name(const char *n) {
  return n && (sp_streq(n, "StandardError") || sp_streq(n, "Exception") ||
               sp_streq(n, "RuntimeError"));
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

  /* type-match condition: catch-all when no types or a StandardError-ish
     type; otherwise exact class-name match */
  int catchall = (nexc == 0);
  for (int i = 0; i < nexc; i++) {
    const char *en = nt_type(nt, exc[i]);
    if (en && sp_streq(en, "ConstantReadNode") && rescue_is_catchall_name(nt_str(nt, exc[i], "name")))
      catchall = 1;
  }

  const char *save_cls = g_rescue_cls, *save_msg = g_rescue_msg;
  static char clsbuf[32], msgbuf[32];
  snprintf(clsbuf, sizeof clsbuf, "_rcls_%d", rc);
  snprintf(msgbuf, sizeof msgbuf, "_rmsg_%d", rc);

  if (!catchall) {
    emit_indent(b, indent);
    buf_puts(b, "if (");
    int first = 1;
    for (int i = 0; i < nexc; i++) {
      const char *en = nt_type(nt, exc[i]);
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
          snprintf(enbuf, sizeof enbuf, "%s_%s", pnm, ename);
          if (is_exc_name(enbuf)) ename = enbuf;
        }
      }
      /* use hierarchy-aware check for exception classes */
      int is_exc_cls = (ename && is_exc_name(ename)) ||
                       (ename && comp_class_index(c, ename) >= 0 &&
                        class_is_exc_subclass(c, comp_class_index(c, ename)));
      if (is_exc_cls)
        buf_printf(b, "sp_exc_cls_matches(_rcls_%d, \"%s\")", rc, ename);
      else
        buf_printf(b, "sp_str_eq(_rcls_%d, \"%s\")", rc, ename);
    }
    if (first) buf_puts(b, "1");  /* no usable type -> always */
    buf_puts(b, ") {\n");
    indent++;
  }

  g_rescue_cls = clsbuf; g_rescue_msg = msgbuf;
  if (ref >= 0 && nt_type(nt, ref) && sp_streq(nt_type(nt, ref), "LocalVariableTargetNode")) {
    /* If the analyzer specialized this binding to a user exception subclass
       object (one arm, one class, no name collision -- see the rescue-var
       typing in analyze_program), bind the original carried object so its
       ivars survive the raise (#1415). The carried slot is at sp_exc_top (the
       just-popped frame, same index _rmsg reads). A degenerate no-object raise
       of that class falls back to a freshly built subclass struct so ivar
       reads stay in-bounds rather than NULL-deref. */
    int spec_cid = -1;
    {
      LocalVar *vlv = scope_local(comp_scope_of(c, ref), nt_str(nt, ref, "name"));
      if (vlv && ty_is_object(vlv->type)) {
        int xc = ty_object_class(vlv->type);
        if (xc >= 0 && class_is_exc_subclass(c, xc)) spec_cid = xc;
      }
    }
    emit_indent(b, indent);
    if (spec_cid >= 0) {
      const char *xn = c->classes[spec_cid].name;
      buf_printf(b, "lv_%s = sp_exc_obj[sp_exc_top] ? (sp_%s *)sp_exc_obj[sp_exc_top]"
                    " : (sp_%s *)sp_exc_new_sub_sized(sizeof(sp_%s), _rcls_%d, _rmsg_%d);\n",
                 nt_str(nt, ref, "name"), xn, xn, xn, rc, rc);
    }
    else
      buf_printf(b, "lv_%s = sp_exc_new_for_catch(_rcls_%d, _rmsg_%d);\n", nt_str(nt, ref, "name"), rc, rc);
  }
  if (resultvar) {
    const char *sv = g_result_var; g_result_var = resultvar;
    emit_stmts_tail(c, stmts, b, indent);
    g_result_var = sv;
  }
  else {
    emit_stmts(c, stmts, b, indent);
  }
  g_rescue_cls = save_cls; g_rescue_msg = save_msg;

  if (!catchall) {
    indent--;
    emit_indent(b, indent);
    buf_puts(b, "}\n");
    emit_indent(b, indent);
    buf_puts(b, "else {\n");
    if (sub >= 0) emit_rescue(c, sub, b, indent + 1, fr, resultvar);
    else {
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
    /* _excf/_excmsg/_exccls track an unhandled exception (no rescue) so
       that ensure can re-raise it after running.  Saved immediately after
       sp_exc_top-- while the index is still valid. */
    emit_indent(b, indent); buf_printf(b, "int _excf%d = 0;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_excmsg%d = NULL;\n", eid);
    emit_indent(b, indent); buf_printf(b, "const char *_exccls%d = NULL;\n", eid);
    if (has_retval) {
      emit_indent(b, indent); emit_ctype(c, g_ret_type, b);
      buf_printf(b, " _retv%d = %s;\n", eid, default_value(g_ret_type));
    }
    g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth };

    emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
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
    emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n");
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
      buf_printf(b, "if (sp_str_eq((const char *)sp_last_exc_cls, \"FiberKillSignal\")) { _excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; }\n",
                 eid, eid, eid);
      emit_indent(b, indent + 2); buf_puts(b, "else {\n");
      emit_rescue(c, rescue, b, indent + 3, fr, resultvar);
      emit_indent(b, indent + 2); buf_puts(b, "}\n");
    }
    else {
      /* No rescue: save exception info for re-raise after ensure runs.
         sp_exc_top has just been decremented so sp_exc_top is the right index. */
      emit_indent(b, indent + 2);
      buf_printf(b, "_excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top];\n",
                 eid, eid, eid);
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
    emit_stmts(c, ensure_stmts, b, indent);
    emit_indent(b, indent);
    buf_printf(b, "sp_unwind_kind = _uk%d; sp_unwind_target = _ut%d; sp_unwind_exc_top = _ue%d; sp_unwind_home = _uh%d;\n",
               eid, eid, eid, eid);

    /* A non-local unwind passing through has now run this ensure: continue to the
       next handler or deliver to its target (never falls through to the
       deferred-return / re-raise propagation below). */
    emit_indent(b, indent);
    buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");

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
      buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; sp_exc_top--; goto _ensure%d; }\n",
                 eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid);
    }
    else {
      /* the deferred return leaves through every enclosing live begin frame:
         pop them or their jmp_bufs dangle into this soon-dead C frame */
      if (g_exc_frame_depth > 0) {
        buf_printf(b, "if (_retf%d) sp_exc_top -= %d;\n", eid, g_exc_frame_depth);
        emit_indent(b, indent);
      }
      /* inside a poly-slot proc body (g_result_var, e.g. a break-capable
         lambda) the deferred value returns through the slot ABI, not a raw
         C return of an sp_RbVal from an mrb_int function */
      if (has_retval && g_proc_body_kind != 0 && g_result_var && g_ret_type == TY_POLY)
        buf_printf(b, "if (_retf%d) { %s = _retv%d; return 0; }\n", eid, g_result_var, eid);
      else if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d;\n", eid, eid);
      else if (g_proc_body_kind != 0 && g_result_var && g_ret_type == TY_POLY)
        buf_printf(b, "if (_retf%d) { %s = sp_box_nil(); return 0; }\n", eid, g_result_var);
      else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil();\n", eid);
      else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0;\n", eid); /* main() */
      else buf_printf(b, "if (_retf%d) return;\n", eid);
      /* Unhandled exception: re-raise using the saved class/message. */
      emit_indent(b, indent);
      buf_printf(b, "if (_excf%d) sp_raise_cls(_exccls%d, _excmsg%d);\n", eid, eid, eid);
    }
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

  emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
  emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
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
  emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n");
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
    buf_puts(b, "if (sp_str_eq((const char *)sp_last_exc_cls, \"FiberKillSignal\")) sp_raise_cls(\"FiberKillSignal\", sp_exc_msg[sp_exc_top]);\n");
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
    if (yrcv >= 0 && ynm && (sp_streq(ynm, "<<") || sp_streq(ynm, "yield")) &&
        nt_type(nt, yrcv) && sp_streq(nt_type(nt, yrcv), "LocalVariableReadNode") &&
        nt_str(nt, yrcv, "name") && sp_streq(nt_str(nt, yrcv, "name"), g_yielder_name)) {
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
                      sp_streq(dsm_rty, "ConstantPathNode"))) return;
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
      emit_proc_call_args(c, yargc, yargv, b, 0);
      buf_puts(b, ";\n");
      return;
    }
    if (g_block_id < 0) return;  /* inlined without block: yield is dead code */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0);
    return;
  }

  if (sp_streq(ty, "CallNode")) {
    /* declarative-only calls emitted as no-ops */
    {
      const char *nm = nt_str(nt, id, "name");
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 && nm && (sp_streq(nm, "include") || sp_streq(nm, "extend") ||
                             sp_streq(nm, "prepend") || sp_streq(nm, "module_function") ||
                             sp_streq(nm, "private") || sp_streq(nm, "protected") ||
                             sp_streq(nm, "public") || sp_streq(nm, "attr_reader") ||
                             sp_streq(nm, "attr_writer") || sp_streq(nm, "attr_accessor"))) {
        /* These are class-body declarations handled at analysis time; skip. */
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
    if (is_block_call(c, id)) { emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, indent, 0); return; }
    if (is_blockless_block_param_call(c, id)) return;  /* dead path: no block supplied */
    if (emit_output_call(c, id, b, indent)) return;
    /* Signal.trap / ::Signal.trap stmt: no-op */
    {
      const char *snm = nt_str(nt, id, "name");
      int srecv = nt_ref(nt, id, "receiver");
      int sargs = nt_ref(nt, id, "arguments");
      int sargc = 0; if (sargs >= 0) nt_arr(nt, sargs, "arguments", &sargc);
      if (srecv >= 0 && snm && sp_streq(snm, "trap") && sargc >= 1) {
        const char *rty2 = nt_type(nt, srecv);
        if (rty2 && (sp_streq(rty2, "ConstantReadNode") || sp_streq(rty2, "ConstantPathNode"))) {
          const char *rn = nt_str(nt, srecv, "name");
          if (rn && sp_streq(rn, "Signal")) return;  /* no-op */
        }
      }
    }
    if (emit_inline_call(c, id, b, indent)) return;
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
            if (comp_writer_in_chain(c, ty_object_class(rt), base, NULL)) {
              if (an >= 1) {
                int rc = ty_object_class(rt);
                char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                int defc = -1; comp_writer_in_chain(c, rc, base, &defc);
                int iv = comp_ivar_index(&c->classes[defc < 0 ? rc : defc], ivn);
                TyKind ivt = iv >= 0 ? c->classes[defc < 0 ? rc : defc].ivar_types[iv] : TY_UNKNOWN;
                emit_indent(b, indent);
                buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ")->iv_%s = ", base);
                if (ivt == TY_POLY && comp_ntype(c, argv[0]) != TY_POLY) emit_boxed(c, argv[0], b);
                else emit_expr(c, argv[0], b);
                buf_puts(b, ";\n");
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
                buf_printf(b, "{ sp_%s *_t%d = ", c->classes[ty_object_class(rt)].name, tp);
                emit_expr(c, recv, b); buf_puts(b, "; ");
                emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval);
                emit_expr(c, argv[0], b); buf_puts(b, ";");
                buf_printf(b, " switch (_t%d->cls_id) {", tp);
                char src[32]; snprintf(src, sizeof src, "_t%d", tval);
                for (int k = 0; k < c->nclasses; k++) {
                  if (!comp_is_writer(&c->classes[k], base)) continue;
                  char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                  int iv = comp_ivar_index(&c->classes[k], ivn);
                  TyKind ivt = iv >= 0 ? c->classes[k].ivar_types[iv] : at;
                  if (at != ivt && at != TY_POLY && ivt != TY_POLY) continue;
                  buf_printf(b, " case %d: ((sp_%s *)_t%d)->iv_%s = ", k, c->classes[k].name, tp, base);
                  if (ivt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, src, b);
                  else if (at == TY_POLY && ivt != TY_POLY) emit_unbox_text(c, ivt, src, b);
                  else buf_puts(b, src);
                  buf_puts(b, "; break;");
                }
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
              buf_printf(b, " switch (_t%d.cls_id) {", tv);
              char src[32]; snprintf(src, sizeof src, "_t%d", tval);
              for (int k = 0; k < c->nclasses; k++) {
                if (!comp_is_writer(&c->classes[k], base)) continue;
                char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", base);
                int iv = comp_ivar_index(&c->classes[k], ivn);
                TyKind ivt = iv >= 0 ? c->classes[k].ivar_types[iv] : at_eff;
                /* skip a class whose slot can't hold this concrete rhs (the
                   runtime object isn't that class anyway): a raw assignment
                   between mismatched C types would not compile */
                if (at_eff != ivt && at_eff != TY_POLY && ivt != TY_POLY) continue;
                buf_printf(b, " case %d: ((sp_%s *)_t%d.v.p)->iv_%s = ", k, c->classes[k].name, tv, base);
                if (ivt == TY_POLY && at_eff != TY_POLY) emit_boxed_text(c, at_eff, src, b);
                else if (at_eff == TY_POLY && ivt != TY_POLY) emit_unbox_text(c, ivt, src, b);
                else buf_puts(b, src);
                buf_puts(b, "; break;");
              }
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
            emit_indent(b, indent);
            emit_expr(c, frcv, b); buf_puts(b, " = sp_str_freeze_val("); emit_expr(c, frcv, b); buf_puts(b, ");\n");
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
    /* a ||= v on an always-truthy var: no-op */
    return;
  }
  if (sp_streq(ty, "InstanceVariableOrWriteNode") || sp_streq(ty, "InstanceVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws2 = comp_scope_of(c, id);
    int sc2 = cws2 ? cws2->class_id : -1;
    TyKind ivt2 = TY_UNKNOWN;
    if (sc2 >= 0) { int iv2 = comp_ivar_index(&c->classes[sc2], nm); if (iv2 >= 0) ivt2 = c->classes[sc2].ivar_types[iv2]; }
    char ref2[300];
    if (cws2 && cws2->is_cmethod && sc2 >= 0)
      snprintf(ref2, sizeof ref2, "civ_%s_%s", c->classes[sc2].name, nm + 1);
    else
      snprintf(ref2, sizeof ref2, "%s%siv_%s", g_self, g_self_deref, nm + 1);
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
             ivt2 == TY_STRINGIO || ivt2 == TY_STRINGSCANNER ||
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
                 is_or ? "!" : "", c->classes[class_id].name, tr, attr,
                 c->classes[class_id].name, tr, attr);
      emit_boxed(c, v, b); buf_puts(b, "; }\n");
    }
    else if (ivt == TY_BOOL) {
      buf_printf(b, "if (%s_t%d->iv_%s) _t%d->iv_%s = ", is_or ? "!" : "", tr, attr, tr, attr);
      emit_expr(c, v, b); buf_puts(b, "; }\n");
    }
    else if (!is_or) {  /* &&= on always-truthy type: always assign */
      buf_printf(b, "_t%d->iv_%s = ", tr, attr); emit_expr(c, v, b); buf_puts(b, "; }\n");
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
      buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, nm + 1);
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
      else
        buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, nm + 1);
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
    if (ivt == TY_INT && emit_nullable_int_ternary(c, v, b)) {
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
    else if (ivt == TY_POLY && comp_ntype(c, v) != TY_POLY) {
      /* a poly ivar slot needs a boxed RHS */
      emit_boxed(c, v, b);
    }
    else if (ivt != TY_POLY && ivt != TY_UNKNOWN && comp_ntype(c, v) == TY_POLY) {
      /* poly rhs assigned to a typed ivar: unbox to the concrete type */
      Buf _rb; memset(&_rb, 0, sizeof _rb);
      emit_expr(c, v, &_rb);
      emit_unbox_text(c, ivt, _rb.p ? _rb.p : "sp_box_nil()", b);
      free(_rb.p);
    }
    else {
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
    if (ct == TY_POLY) emit_boxed(c, v, b); else emit_expr(c, v, b);
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
    TyKind vt = TY_UNKNOWN;
    if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);
    else
      snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    emit_indent(b, indent);
    if (vt == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "%s = sp_str_concat(%s, ", ref, ref);
      emit_expr(c, nt_ref(nt, id, "value"), b); buf_puts(b, ");\n");
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
                   ref, c->classes[idefcls].name, mc(ims->name),
                   c->classes[idefcls].name, ref, iatmp);
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
        emit_expr(c, ival, g_pre);
        buf_puts(g_pre, ";\n");
        buf_printf(b, "%s = sp_box_obj(sp_%s_%s((sp_%s *)(%s).v.p, _t%d), %d);\n",
                   ref, c->classes[poly_defcls].name, mc(pms->name),
                   c->classes[poly_defcls].name, ref, iatmp, poly_defcls);
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
  if (sp_streq(ty, "GlobalVariableWriteNode") || sp_streq(ty, "ConstantWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int isg = ty[0] == 'G';
    const char *pfx = isg ? "gv" : "cst";
    const char *raw_key = isg ? nm + 1 : nm;
    const char *key = isg ? comp_resolve_gvar(c, raw_key) : raw_key;
    LocalVar *lv = isg ? comp_gvar(c, key) : comp_const(c, key);
    if (!lv || (!isg && lv->type == TY_UNKNOWN)) { /* untyped -> skip */ return; }
    int v = nt_ref(nt, id, "value");
    if (!isg && lv->init_guarded) {
      /* flag the const as in-progress while its Class.new runs, so a
         self-referential read inside initialize raises NameError */
      emit_indent(b, indent); buf_printf(b, "sp_init_in_progress_%s = 1;\n", key);
    }
    emit_indent(b, indent);
    buf_printf(b, "%s_%s = ", pfx, key);
    const char *vty = nt_type(nt, v);
    int v_empty_arr = 0, v_empty_hash = 0;
    if (vty && sp_streq(vty, "ArrayNode")) {
      int ac = 0; nt_arr(nt, v, "elements", &ac); v_empty_arr = (ac == 0);
    }
    if (vty && (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode"))) {
      int hec = 0; nt_arr(nt, v, "elements", &hec); v_empty_hash = (hec == 0);
    }
    if (vty && sp_streq(vty, "NilNode"))
      buf_puts(b, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    else if (v_empty_arr && lv->type == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_arr && array_kind(lv->type)) buf_printf(b, "sp_%sArray_new()", array_kind(lv->type));
    else if (v_empty_hash && ty_is_hash(lv->type)) {
      const char *hcn = ty_hash_cname(lv->type);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
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
    const char *vty = nt_type(nt, v);
    int v_empty_arr = 0, v_empty_hash = 0;
    if (vty && sp_streq(vty, "ArrayNode")) {
      int ac = 0; nt_arr(nt, v, "elements", &ac); v_empty_arr = (ac == 0);
    }
    if (vty && (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode"))) {
      int hec = 0; nt_arr(nt, v, "elements", &hec); v_empty_hash = (hec == 0);
    }
    emit_indent(b, indent);
    buf_printf(b, "cst_%s = ", nm);
    if (vty && sp_streq(vty, "NilNode"))
      buf_puts(b, cv->type == TY_RANGE ? "(sp_Range){0}" : default_value(cv->type));
    else if (v_empty_arr && cv->type == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_arr && array_kind(cv->type)) buf_printf(b, "sp_%sArray_new()", array_kind(cv->type));
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
      TyKind vt = comp_ntype(c, value);
      const char *k = ty_is_array(vt) ? ((vt == TY_POLY_ARRAY) ? "Poly" : array_kind(vt)) : NULL;
      if (!k) k = "Int";
      int tarr = ++g_tmp;
      emit_indent(b, indent);
      emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_INT_ARRAY, b);
      buf_printf(b, " _t%d = ", tarr); emit_expr(c, value, b); buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "SP_GC_ROOT(_t%d);\n", tarr);
      /* Length check: raise NoMatchingPatternError if sizes differ. */
      emit_indent(b, indent);
      buf_printf(b, "if (!_t%d || _t%d->len != %dLL) sp_raise_cls(\"NoMatchingPatternError\", \"[array pattern mismatch]\");\n", tarr, tarr, (long long)rn);
      for (int i = 0; i < rn; i++) {
        const char *lty2 = nt_type(nt, reqs[i]);
        if (!lty2 || !sp_streq(lty2, "LocalVariableTargetNode")) continue;
        const char *lnm = nt_str(nt, reqs[i], "name");
        if (!lnm) continue;
        emit_indent(b, indent);
        buf_printf(b, "lv_%s = ", lnm);
        LocalVar *plv = scope_local(comp_scope_of(c, id), lnm);
        char gx[64]; snprintf(gx, sizeof gx, "sp_%sArray_get(_t%d, %dLL)", k, tarr, i);
        if (plv && plv->type == TY_POLY && !sp_streq(k, "Poly")) {
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, ty_array_elem(vt != TY_UNKNOWN ? vt : TY_INT_ARRAY), gx, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_puts(b, gx);
        buf_puts(b, ";\n");
      }
    }
    else if (sp_streq(pty, "HashPatternNode")) {
      int pn = 0;
      const int *pelms = nt_arr(nt, pattern, "elements", &pn);
      /* Evaluate value hash into a temp. */
      TyKind vt = comp_ntype(c, value);
      const char *hn = ty_is_hash(vt) ? ty_hash_cname(vt) : NULL;
      int thash = ++g_tmp;
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
      int multi_src = vty && (sp_streq(vty, "CallNode") || sp_streq(vty, "SuperNode") ||
                              sp_streq(vty, "ForwardingSuperNode") || sp_streq(vty, "YieldNode"));
      /* a TY_POLY value can hold an array at runtime (doom's
         `lump_name, mirrored = @sprite_index[key]` read through a local),
         so it must take the runtime-destructure path below, not the
         scalar fill -- which handed the whole array to the first target. */
      if (vty && !multi_src && !ty_is_array(st) && !ty_is_hash(st) && st != TY_UNKNOWN && st != TY_POLY) {
        for (int i = 0; i < ln; i++) {
          const char *lty = nt_type(nt, lefts[i]);
          if (!lty || !sp_streq(lty, "LocalVariableTargetNode")) continue;
          emit_indent(b, indent);
          const char *lvn = nt_str(nt, lefts[i], "name");
          buf_printf(b, "lv_%s = ", lvn);
          LocalVar *llv = lvn ? scope_local(comp_scope_of(c, id), lvn) : NULL;
          int lpoly = llv && llv->type == TY_POLY;
          if (i == 0) { if (lpoly && st != TY_POLY) emit_boxed(c, value, b); else emit_expr(c, value, b); }
          else if (lpoly) {
            /* rest target's typed-zero default, boxed into the widened slot.
               comp_ntype on the target node still reports its pre-widen scalar
               type, so default_value lands the same 0 default mode emits. */
            TyKind tt = comp_ntype(c, lefts[i]);
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, tt, default_value(tt), &bx);
            buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(b, default_value(comp_ntype(c, lefts[i])));
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
          if (sp_streq(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            const char *lvn = nt_str(nt, lefts[i], "name");
            buf_printf(b, "lv_%s = ", lvn);
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
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, ivnm + 1);
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
              buf_printf(b, "sp_%s_%s((sp_%s *)", c->classes[cdef].name, mc(c->scopes[wmi].name), c->classes[cdef].name);
              emit_expr(c, crecv, b); buf_puts(b, ", ");
              if (pt == TY_POLY && elem != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem, get_expr, &bx); buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
              else buf_puts(b, get_expr);
              buf_puts(b, ");\n");
            }
            else {
              /* attr_writer convention: name matches the backing ivar */
              char base[256]; memcpy(base, setnm, snl - 1); base[snl - 1] = '\0';
              buf_puts(b, "("); emit_expr(c, crecv, b); buf_printf(b, ")->iv_%s = %s;\n", base, get_expr);
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
          buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_slice(_t%d, %dLL, _t%d->len - %dLL - %dLL);\n",
                     rk, tr, rk, tarr, ln, tarr, ln, rn);
          emit_indent(b, indent);
          buf_printf(b, "SP_GC_ROOT(_t%d);\n", tr);
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = _t%d;\n", rest_var, tr);
        }
        for (int j = 0; j < rn; j++) {
          const char *lty = nt_type(nt, rights[j]);
          if (!lty) continue;
          if (sp_streq(lty, "LocalVariableTargetNode")) {
            emit_indent(b, indent);
            const char *rlvn = nt_str(nt, rights[j], "name");
            buf_printf(b, "lv_%s = ", rlvn);
            LocalVar *rllv = rlvn ? scope_local(rt_scope, rlvn) : NULL;
            char rgx[80]; snprintf(rgx, sizeof rgx, "sp_%sArray_get(_t%d, _t%d->len - %dLL + %dLL)", k, tarr, tarr, rn, j);
            if (rllv && rllv->type == TY_POLY && !sp_streq(k, "Poly")) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed_text(c, elem, rgx, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else buf_puts(b, rgx);
            buf_puts(b, ";\n");
          }
          else if (sp_streq(lty, "InstanceVariableTargetNode") &&
                   rt_scope && rt_scope->class_id >= 0) {
            const char *ivnm2 = nt_str(nt, rights[j], "name");
            if (!ivnm2) continue;
            emit_indent(b, indent);
            char get_expr2[80];
            snprintf(get_expr2, sizeof get_expr2, "sp_%sArray_get(_t%d, _t%d->len - %dLL + %dLL)", k, tarr, tarr, rn, j);
            TyKind ivt2 = TY_UNKNOWN;
            int iv_rt2 = comp_ivar_index(&c->classes[rt_scope->class_id], ivnm2);
            if (iv_rt2 >= 0) ivt2 = c->classes[rt_scope->class_id].ivar_types[iv_rt2];
            if (rt_scope->is_cmethod)
              buf_printf(b, "civ_%s_%s = ", c->classes[rt_scope->class_id].name, ivnm2 + 1);
            else
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, ivnm2 + 1);
            if (ivt2 == TY_POLY && elem != TY_POLY) {
              Buf bx2; memset(&bx2, 0, sizeof bx2);
              emit_boxed_text(c, elem, get_expr2, &bx2);
              buf_puts(b, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
            }
            else buf_puts(b, get_expr2);
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
          if (sp_streq(lty, "LocalVariableTargetNode")) {
            const char *lnm = nt_str(nt, lefts[i], "name");
            emit_indent(b, indent);
            buf_printf(b, "lv_%s = sp_poly_massign_get(_t%d, %dLL);\n", lnm, tarr, i);
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
              buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, ivnm + 1);
            if (ivt != TY_POLY) {
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_unbox_text(c, ivt, get_expr, &bx);
              buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
            }
            else buf_puts(b, get_expr);
            buf_puts(b, ";\n");
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
    for (int i = 0; i < en; i++) {
      tmps[i] = ++g_tmp;
      emit_indent(b, indent);
      /* A nil (or void) element has no scalar C type; hold it as a boxed poly
         temp so the slot is valid and a poly target can read it directly. */
      TyKind elt = comp_ntype(c, els[i]);
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
      else {
        Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, els[i], &vb);
        buf_puts(b, vb.p ? vb.p : ""); free(vb.p);
      }
      buf_puts(b, ";\n");
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
          buf_printf(b, "lv_%s = %s;\n", lvn, default_value(ltt));
        }
        continue;
      }
      if (lty && sp_streq(lty, "LocalVariableTargetNode")) {
        emit_indent(b, indent);
        const char *lvn = nt_str(nt, lefts[i], "name");
        buf_printf(b, "lv_%s = ", lvn);
        LocalVar *llv = lvn ? scope_local(comp_scope_of(c, id), lvn) : NULL;
        TyKind ltt = llv ? llv->type : comp_ntype(c, lefts[i]);
        TyKind valt = comp_ntype(c, els[i]);
        if (ltt == TY_POLY && valt != TY_POLY) {
          char expr[32]; snprintf(expr, sizeof expr, "_t%d", tmps[i]);
          Buf bx; memset(&bx, 0, sizeof bx);
          emit_boxed_text(c, valt, expr, &bx);
          buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
        }
        else buf_printf(b, "_t%d", tmps[i]);
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
          buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, ivnm + 1);
        TyKind valt = comp_ntype(c, els[i]);
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
        buf_puts(b, "("); emit_expr(c, recv_id2, b); buf_printf(b, ")->iv_%s = ", base2);
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
          buf_printf(b, "lv_%s = ", inm);
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
            TyKind valt = comp_ntype(c, els[i]);
            char tmp_expr[32]; snprintf(tmp_expr, sizeof tmp_expr, "_t%d", tmps[i]);
            Buf bxi; memset(&bxi, 0, sizeof bxi);
            emit_boxed_text(c, valt, tmp_expr, &bxi);
            buf_puts(b, bxi.p ? bxi.p : "sp_box_nil()"); free(bxi.p);
          }
          else buf_printf(b, "_t%d", tmps[i]);
          buf_puts(b, ");\n");
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
            TyKind valt = comp_ntype(c, els[i]);
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
        TyKind valt = comp_ntype(c, els[i]);
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
      buf_printf(b, "lv_%s = _t%d;\n", rest_var, tr);
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
          buf_printf(b, "lv_%s = ", rnm_j);
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
          buf_printf(b, "lv_%s = ", rnm_j);
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
          buf_printf(b, "%s%siv_%s = ", g_self, g_self_deref, rnm_j + 1);
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
        int is_output = cn && (sp_streq(cn, "puts") || sp_streq(cn, "print") || sp_streq(cn, "p"));
        int is_user = cn && comp_method_index(c, cn) >= 0;
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
      if (brk_goto) buf_printf(b, "; goto _brklbl%s;\n", sfx);
      else buf_puts(b, ");\n");
      return;
    }
    /* break inside a lambda body: a return from the lambda (CRuby). A break-
       capable lambda's ret is widened to poly, so the value returns through
       the proc ABI's _sp_proc_poly_ret slot (g_result_var) with `return 0`;
       ensure deferral still applies through emit_return otherwise. */
    if (g_proc_body_kind == 1) {
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
       scope's serial; a dead/foreign scope raises LocalJumpError. */
    if (g_proc_body_kind == 2 && g_proc_brk_home) {
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
    emit_indent(b, indent);
    /* leaving through live begin/rescue frames opened inside the loop body:
       pop them, or their jmp_bufs dangle (same accounting as emit_return) */
    if (g_exc_frame_depth > g_loop_exc_base)
      buf_printf(b, "sp_exc_top -= %d; ", g_exc_frame_depth - g_loop_exc_base);
    buf_puts(b, "break;\n"); return;
  }
  if (sp_streq(ty, "NextNode")) {
    if (g_ie_next_var) {
      int nargs = nt_ref(nt, id, "arguments");
      int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
      if (nvc > 0) {
        emit_indent(b, indent); buf_printf(b, "%s = ", g_ie_next_var);
        if (g_ie_res_poly) emit_boxed(c, nv[0], b); else emit_expr(c, nv[0], b);
        buf_puts(b, ";\n");
      }
    }
    emit_indent(b, indent);
    if (g_exc_frame_depth > g_loop_exc_base)
      buf_printf(b, "sp_exc_top -= %d; ", g_exc_frame_depth - g_loop_exc_base);
    buf_puts(b, "continue;\n"); return;
  }
  if (sp_streq(ty, "RedoNode"))   {
    emit_indent(b, indent);
    if (g_redo_depth > 0) buf_printf(b, "goto _redo_%d;\n", g_redo_stack[g_redo_depth - 1]);
    else buf_puts(b, "continue;\n");  /* redo outside a labeled loop: best-effort */
    return;
  }
  if (sp_streq(ty, "RetryNode")) {
    if (g_retry_label) { emit_indent(b, indent); buf_printf(b, "goto %s;\n", g_retry_label); }
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
    emit_indent(b, indent); buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
    emit_indent(b, indent); buf_puts(b, "sp_exc_top++;\n");
    emit_indent(b, indent); buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    if (e >= 0) emit_stmt(c, e, b, indent + 1);
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "else {\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_exc_top--;\n");
    emit_indent(b, indent + 1); buf_puts(b, "sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n");
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind == SP_UNWIND_NONE) sp_proc_homes_unwind();\n");
    /* A non-local unwind only passes through (no ensure here): continue it. */
    emit_indent(b, indent + 1); buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
    if (r >= 0) emit_stmt(c, r, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return;
  }
  if (sp_streq(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  if (sp_streq(ty, "DefNode"))    { return; } /* emitted separately */
  if (sp_streq(ty, "UndefNode"))  { return; } /* resolved at scan time */
  if (sp_streq(ty, "AliasGlobalVariableNode")) { return; } /* resolved at scan time */
  if (sp_streq(ty, "PreExecutionNode") || sp_streq(ty, "PostExecutionNode")) { return; } /* hoisted separately */

  /* any remaining value expression as a bare statement (its value is used
     only when this is the last statement of an inlined expr method) */
  emit_indent(b, indent);
  emit_expr(c, id, b);
  buf_puts(b, ";\n");
}

/* Tail position: the value of this statement is the method's return value. */
void emit_stmt_tail_inner(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "tail statement (no type)");

  if (sp_streq(ty, "IfNode"))     { emit_if(c, id, b, indent, 0, 1); return; }
  if (sp_streq(ty, "UnlessNode")) { emit_if(c, id, b, indent, 1, 1); return; }
  if (sp_streq(ty, "CaseMatchNode")) { emit_case_match(c, id, b, indent, 1, -1); return; }
  if (sp_streq(ty, "ReturnNode")) { emit_return(c, id, b, indent); return; }
  /* a tail `break` in a lambda/proc body diverges (return / brk-throw): emit
     it as a statement, like `raise` below; the fall-through value is dead. */
  if (sp_streq(ty, "BreakNode") && g_proc_body_kind != 0) {
    emit_stmt_inner(c, id, b, indent);
    return;
  }
  /* `raise` diverges -- no value to return; emit as a plain statement. */
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "receiver") < 0 &&
      nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), "raise")) {
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
  if (sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") ||
      sp_streq(ty, "LocalVariableAndWriteNode") ||
      sp_streq(ty, "InstanceVariableWriteNode") ||
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
    if (celled) { emit_stmt(c, id, b, indent); return; }
  }
  /* iteration calls with a block are side-effect statements at tail position;
     emit them without wrapping in a return (the method returns nil implicitly).
     A break-carrying iterator is the exception: its value (the break value, or
     the normal result if no break is taken) IS the method's return, so it must
     take the value path below (the break wrapper in emit_call), not the
     statement form here. */
  if (sp_streq(ty, "CallNode") && nt_ref(nt, id, "block") >= 0 &&
      !call_breaks(c, id) &&
      emit_iteration_stmt(c, id, b, indent))
    return;

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
        emit_stmt(c, id, b, indent);
        return;
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
      emit_indent(b, indent); emit_tail_lead(b);
      int _wp = g_result_var ? g_result_poly : (g_ret_type == TY_POLY);
      if (_wp) emit_boxed(c, _srecv, b);
      else emit_expr(c, _srecv, b);
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
  /* a poly tail value feeding a narrower (non-poly) return slot -- a scalar
     method(:sym) target, or an RBS-typed String/object method whose body yields
     poly -- needs coercing. (Only for a real return slot, not a begin/rescue
     result var, which stays poly.) */
  else if (!g_result_var && tail_needs_unbox(vty, g_ret_type)) emit_unbox_node(c, g_ret_type, id, b);
  else emit_tail_value(c, id, b);
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
int needs_root(TyKind t) { return t == TY_STRING || t == TY_STRBUF || t == TY_BIGINT || ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || ty_is_object(t) || t == TY_EXCEPTION || t == TY_POLY || t == TY_PROC || t == TY_CURRY || t == TY_METHOD || t == TY_IO || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_ENUMERATOR || t == TY_RANDOM || t == TY_MATCHDATA; }

/* Emit `node` boxed into an sp_RbVal. Idempotent: an already-poly value is
   passed through unboxed (double-boxing is a classic silent-corruption bug). */
/* Box a C-text expression `expr` of static type `t` into an sp_RbVal. */
const char *hash_box_cls(TyKind t) {
  switch (t) {
    case TY_STR_INT_HASH:   return "SP_BUILTIN_STR_INT_HASH";
    case TY_STR_STR_HASH:   return "SP_BUILTIN_STR_STR_HASH";
    case TY_INT_STR_HASH:   return "SP_BUILTIN_INT_STR_HASH";
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
    const char *bty = nt_type(nt, cur);
    LocalVar *blv = (bty && sp_streq(bty, "LocalVariableReadNode"))
                    ? scope_local(comp_scope_of(c, cur), nt_str(nt, cur, "name")) : NULL;
    if (nchain > 0 && blv && blv->type == TY_STRBUF) {
      const char *bn2 = rename_local(nt_str(nt, cur, "name"));
      for (int j = nchain - 1; j >= 0; j--) {
        int arg = chain[j];
        TyKind at = comp_ntype(c, arg);
        emit_indent(b, indent);
        buf_printf(b, "sp_String_append_bin(lv_%s, ", bn2);
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else emit_expr(c, arg, b);
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
        emit_expr(c, cur, b); buf_puts(b, " = sp_str_concat(");
        emit_expr(c, cur, b); buf_puts(b, ", ");
        if (at == TY_INT) { buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, arg, b); buf_puts(b, ")"); }
        else emit_expr(c, arg, b);
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
  if (rt == TY_STRING && argc == 0) {
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
      if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ");\n");
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
    if (assignable && sp_streq(name, "replace") && argc == 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = "); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
      return 1;
    }
    if (assignable && sp_streq(name, "prepend") && argc == 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, recv, b); buf_puts(b, ");\n");
      return 1;
    }
    if (assignable && sp_streq(name, "clear") && argc == 0) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = (&(\"\\xff\")[1]);\n");
      return 1;
    }
    if (assignable && sp_streq(name, "insert") && argc == 2) {
      /* insert(i, x): s[0,i] + x + s[i..]. A negative i counts from the end
         and inserts after that character (i += len + 1). */
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (mrb_int)sp_str_length(", ti, ti); emit_expr(c, recv, b); buf_printf(b, ") + 1; ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _t%d), ", ti); emit_expr(c, argv[1], b);
      buf_puts(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, (mrb_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_printf(b, "))); }\n");
      return 1;
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
    if (assignable && sp_streq(name, "[]=") && argc == 2 && comp_ntype(c, argv[0]) == TY_INT) {
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; mrb_int _len%d = (mrb_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_puts(b, ");");
      buf_printf(b, " mrb_int _a%d = _t%d < 0 ? _t%d + _len%d : _t%d;", ti, ti, ti, ti, ti);
      buf_printf(b, " if (_a%d < 0 || _a%d > _len%d) sp_raise_cls(\"IndexError\", sp_sprintf(\"index %%lld out of string\", (long long)_t%d));",
                 ti, ti, ti, ti);
      buf_puts(b, " "); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _a%d), ", ti); emit_str_expr(c, argv[1], b);
      buf_printf(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _a%d + 1 < _len%d ? _a%d + 1 : _len%d, _len%d)); }\n", ti, ti, ti, ti, ti);
      return 1;
    }
  }

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn && sp_streq(name, "[]=") && argc == 2) {
      emit_indent(b, indent);
      buf_puts(b, "if (sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")) sp_raise_frozen_hash();\n");
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
       Skip when a user class defines the name -- the poly value may be that
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

  if (sp_streq(name, "[]=") && argc == 2) {
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
    for (int a = 0; a < argc; a++) {
      emit_indent(b, indent);
      buf_printf(b, "sp_%sArray_push(", k);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      /* coerce a poly value (holds the element type at runtime) to the typed
         array's element representation */
      TyKind vt = comp_ntype(c, argv[a]);
      if (vt == TY_POLY && et == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      else if (vt == TY_POLY && et == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      else if (vt == TY_POLY && et == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[a], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[a], b);
      buf_puts(b, ");\n");
    }
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
      const char *ak = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
      if (!ak) ak = k;
      emit_indent(b, indent + 1);
      buf_printf(b, "{ mrb_int _t%d = sp_%sArray_length(", tn, ak); emit_expr(c, argv[a], b);
      buf_printf(b, "); for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, ",
                 ti, ti, tn, ti, k, tr);
      char getexpr[256];
      /* element accessor on the source */
      Buf eb; memset(&eb, 0, sizeof eb);
      buf_printf(&eb, "sp_%sArray_get(", ak); { Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, argv[a], &ab); buf_puts(&eb, ab.p ? ab.p : ""); free(ab.p); }
      buf_printf(&eb, ", _t%d)", ti);
      snprintf(getexpr, sizeof getexpr, "%s", eb.p ? eb.p : ""); free(eb.p);
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
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tb); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_%sHash_set(_t%d, _t%d, ", hn, ta, tb);
    const char *pf = vt == TY_POLY ?
        (sp_streq(op, "+") ? "sp_poly_add" : sp_streq(op, "-") ? "sp_poly_sub" :
         sp_streq(op, "*") ? "sp_poly_mul" : sp_streq(op, "/") ? "sp_poly_div" :
         sp_streq(op, "%") ? "sp_poly_mod" : sp_streq(op, "**") ? "sp_poly_pow" : NULL) : NULL;
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
    buf_puts(b, "); }\n");
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
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    if (subtree_may_allocate(nt, recv)) buf_printf(b, "; SP_GC_ROOT(_t%d)", ta);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
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
      buf_printf(b, "{ sp_RbVal _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_sym _t%d = ", tb); emit_expr(c, argv[0], b); buf_puts(b, "; ");
      buf_printf(b, "sp_RbVal _t%d = sp_poly_get_sym(_t%d, _t%d);", tc, ta, tb);
      buf_printf(b, " sp_poly_set_sym(_t%d, _t%d, sp_box_int(_t%d.v.i %s (", ta, tb, tc, op);
      emit_expr(c, v, b); buf_puts(b, "))); }\n");
    }
    else if (kt == TY_STRING) {
      int tc = ++g_tmp;
      buf_printf(b, "{ sp_RbVal _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; const char *_t%d = ", tb); emit_expr(c, argv[0], b); buf_puts(b, "; ");
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
        buf_printf(b, "{ sp_RbVal _t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tc, ta, tb);
        buf_printf(b, " sp_poly_arr_set_hash(_t%d, _t%d, %s(_t%d, ", ta, tb, pf, tc);
        emit_boxed(c, v, b); buf_puts(b, ")); }\n");
      }
      else {
        buf_printf(b, "{ sp_RbVal _t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b); buf_puts(b, "; ");
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
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
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

