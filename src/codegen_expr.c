#include "codegen_internal.h"

/* Adjacent string literals joined by `\`-continuation (or `+`-folded at parse
   time) produce an InterpolatedStringNode whose own parts are themselves
   InterpolatedStringNodes. Flatten the part tree into one list of leaf parts
   (StringNode / EmbeddedStatementsNode) so the format/arg assembly below sees a
   single flat sequence and emits one sp_sprintf call. */
static void interp_flatten(const NodeTable *nt, int id, int **out, int *n, int *cap) {
  int pn = 0;
  const int *parts = nt_arr(nt, id, "parts", &pn);
  for (int k = 0; k < pn; k++) {
    int pid = parts[k];
    const char *pty = nt_type(nt, pid);
    if (pty && sp_streq(pty, "InterpolatedStringNode")) {
      interp_flatten(nt, pid, out, n, cap);
      continue;
    }
    if (*n >= *cap) {
      int ncap = *cap ? *cap * 2 : 8;
      int *grown = realloc(*out, (size_t)ncap * sizeof(int));
      if (!grown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      *out = grown; *cap = ncap;
    }
    (*out)[(*n)++] = pid;
  }
}

void emit_interp(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int n = 0;
  int *flat = NULL, fcap = 0;
  interp_flatten(nt, id, &flat, &n, &fcap);
  const int *parts = flat;
  Buf fmt; memset(&fmt, 0, sizeof fmt);
  Buf argbuf; memset(&argbuf, 0, sizeof argbuf);
  Buf decls; memset(&decls, 0, sizeof decls);  /* rooted %s-arg temp decls */
  int nargs = 0;

  for (int k = 0; k < n; k++) {
    int pid = parts[k];
    const char *pty = nt_type(nt, pid);
    if (pty && sp_streq(pty, "StringNode")) {
      const char *content = nt_str(nt, pid, "content");
      for (const char *p = content ? content : ""; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '%') buf_puts(&fmt, "%%");
        else if (ch == '\\') buf_puts(&fmt, "\\\\");
        else if (ch == '"') buf_puts(&fmt, "\\\"");
        else if (ch == '\n') buf_puts(&fmt, "\\n");
        else if (ch == '\t') buf_puts(&fmt, "\\t");
        else if (ch == '\r') buf_puts(&fmt, "\\r");
        else if (ch >= 0x20 && ch < 0x7f) buf_printf(&fmt, "%c", ch);
        else buf_printf(&fmt, "\\%03o", ch);
      }
    }
    else if (pty && sp_streq(pty, "EmbeddedStatementsNode")) {
      int s = nt_ref(nt, pid, "statements");
      int bn = 0;
      const int *body = s >= 0 ? nt_arr(nt, s, "body", &bn) : NULL;
      int expr = bn > 0 ? body[bn - 1] : -1;
      /* `#{ s1; s2; ...; sN }` evaluates every statement in order and uses sN's
         value. Run the leading statements for side effects; if sN is itself an
         assignment, perform it and read the assigned variable back as the value. */
      char vexpr[48]; vexpr[0] = 0;
      for (int si = 0; si + 1 < bn; si++) emit_stmt(c, body[si], g_pre, g_indent);
      const char *ety = expr >= 0 ? nt_type(nt, expr) : NULL;
      TyKind t = comp_ntype(c, expr);
      /* A bare implicit-self call inside an included-module method is analyzed
         generically (self type unknown -> TY_UNKNOWN), but codegen emits the
         method for a concrete class. Re-resolve the call against the class
         being emitted so the interpolation dispatches on the real return type
         (the same self-class lookup codegen uses for implicit-self calls). */
      if (t == TY_UNKNOWN && ety && sp_streq(ety, "CallNode") &&
          nt_ref(nt, expr, "receiver") < 0) {
        const char *cn = nt_str(nt, expr, "name");
        Scope *cs = comp_scope_of(c, expr);
        int dcid = g_ie_class_id >= 0 ? g_ie_class_id
                 : g_emitting_class_id >= 0 ? g_emitting_class_id
                 : cs->class_id;
        if (cn && dcid >= 0) {
          int mi = comp_method_in_chain(c, dcid, cn, NULL);
          if (mi >= 0) t = c->scopes[mi].ret;
        }
      }
      if (ety && (sp_streq(ety, "LocalVariableWriteNode") || sp_streq(ety, "LocalVariableOperatorWriteNode") ||
                  sp_streq(ety, "LocalVariableOrWriteNode") || sp_streq(ety, "LocalVariableAndWriteNode"))) {
        emit_stmt(c, expr, g_pre, g_indent);
        const char *vn = nt_str(nt, expr, "name");
        LocalVar *lvp = vn ? scope_local(comp_scope_of(c, expr), vn) : NULL;
        if (lvp) t = lvp->type;
        int tv = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, t, g_pre);
        buf_printf(g_pre, " _t%d = ", tv); emit_local_ref(c, expr, vn, g_pre); buf_puts(g_pre, ";\n");
        snprintf(vexpr, sizeof vexpr, "_t%d", tv);
      }
      /* Build this part's conversion into its own buffer; a %s arg that
         allocates a fresh string (sp_poly_to_s / sp_*_to_s / *_inspect …)
         is then hoisted into a rooted temp so a sibling arg's allocation
         (or sp_sprintf's own) cannot free it mid-call. */
      Buf conv; memset(&conv, 0, sizeof conv);
      int is_str_arg = 1;
      #define EMIT_IV() do { if (vexpr[0]) buf_puts(&conv, vexpr); else emit_expr(c, expr, &conv); } while (0)
      if (t == TY_INT) {
        /* nil-aware: an int slot holding the nil sentinel interpolates as ""
           (the helper returns a rooted decimal string otherwise). */
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_int_interp(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_STRING) {
        /* a nullable string (NULL) interpolates as the empty string */
        buf_puts(&fmt, "%s"); buf_puts(&conv, "("); EMIT_IV(); buf_puts(&conv, " ?: \"\")");
      }
      else if (t == TY_FLOAT) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_float_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_BOOL) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "(");
        EMIT_IV(); buf_puts(&conv, " ? \"true\" : \"false\")");
      }
      else if (t == TY_SYMBOL) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_sym_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_POLY) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_EXCEPTION) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_exc_message(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_NIL) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "((void)(");
        EMIT_IV(); buf_puts(&conv, "), \"\")");
      }
      else if (t == TY_RATIONAL) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_rational_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_COMPLEX) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_complex_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_POLY_ARRAY) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_PolyArray_inspect(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_is_array(t) && array_kind(t)) {
        buf_puts(&fmt, "%s"); buf_printf(&conv, "sp_%sArray_inspect(", array_kind(t));
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
        const char *cn = obj_str_cname(c, ty_object_class(t), 0);
        buf_puts(&fmt, "%s"); buf_printf(&conv, "sp_%s_to_s((sp_%s *)", cn, cn);
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_is_hash(t) && ty_hash_cname(t)) {
        buf_puts(&fmt, "%s"); buf_printf(&conv, "sp_%sHash_inspect(", ty_hash_cname(t));
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_CLASS) {
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_class_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_is_object(t)) {
        /* user object without to_s: fall back to poly_to_s of boxed value */
        int cid2 = ty_object_class(t);
        buf_puts(&fmt, "%s");
        buf_printf(&conv, "sp_poly_to_s(sp_box_obj(");
        EMIT_IV(); buf_printf(&conv, ", %d))", cid2);
      }
      else if (t == TY_UNKNOWN && ety && sp_streq(ety, "ArrayNode") &&
               (nt_arr(nt, expr, "elements", (int[]){0}), 1)) {
        /* a bare empty array literal interpolates as "[]" */
        int en = 0; nt_arr(nt, expr, "elements", &en);
        if (en == 0) { buf_puts(&fmt, "%s"); buf_puts(&conv, "\"[]\""); }
        else { free(fmt.p); free(argbuf.p); free(decls.p); free(conv.p); free(flat); unsupported(c, pid, "interpolation value"); }
      }
      else if (t == TY_UNKNOWN) {
        /* An untyped value (e.g. a method resolved only through an included
           module) is already emitted as a boxed sp_RbVal, so stringify it the
           same way as a poly value rather than rejecting the interpolation. */
        buf_puts(&fmt, "%s"); buf_puts(&conv, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else {
        free(fmt.p); free(argbuf.p); free(decls.p); free(conv.p); free(flat);
        unsupported(c, pid, "interpolation value");
      }
      #undef EMIT_IV
      /* Hoist a %s arg's (possibly freshly-allocated) string into a rooted
         temp. Collected into `decls` and emitted as a leading sequence of a
         statement-expression below — NOT into g_pre, since emit_interp may
         run while g_pre holds a half-written enclosing statement. */
      if (is_str_arg) {
        int tv = ++g_tmp;
        buf_printf(&decls, "const char *_t%d = %s; SP_GC_ROOT(_t%d); ",
                   tv, conv.p ? conv.p : "\"\"", tv);
        buf_printf(&argbuf, ", _t%d", tv);
      }
else {
        buf_printf(&argbuf, ", %s", conv.p ? conv.p : "0");
      }
      free(conv.p);
      nargs++;
    }
    else {
      free(fmt.p); free(argbuf.p); free(flat);
      unsupported(c, pid, "interpolation part");
    }
  }

  if (nargs == 0) {
    /* adjacent literals ("a" "b") fold to one literal: frozen per the
       InterpolatedStringNode's own file pragma flag */
    buf_printf(b, "(&(\"%s\" \"", nt_int(c->nt, id, "fzl", 0) ? "\\xf1" : "\\xff");
    for (const char *p = fmt.p ? fmt.p : ""; *p; p++) {
      if (p[0] == '%' && p[1] == '%') { buf_puts(b, "%"); p++; }
      else buf_printf(b, "%c", *p);
    }
    buf_puts(b, "\")[1])");
  }
  else if (!decls.p) {
    /* all args are plain int values — no rooting needed, emit directly */
    buf_printf(b, "sp_sprintf(\"%s\"%s)", fmt.p ? fmt.p : "", argbuf.p ? argbuf.p : "");
  }
  else {
    /* at least one %s arg was hoisted into a rooted temp: wrap the whole
       thing in a statement-expression so the temps (and their roots) are
       self-contained and valid in any expression position. */
    buf_printf(b, "({ %ssp_sprintf(\"%s\"%s); })",
               decls.p, fmt.p ? fmt.p : "", argbuf.p ? argbuf.p : "");
  }
  free(fmt.p); free(argbuf.p); free(decls.p); free(flat);
}

/* ---- expression ---- */

static int fold_int_const_name(Compiler *c, const char *name, long long *out, int depth);

/* Fold a compile-time integer-constant expression to its value: integer
   literals, references to other int-literal constants, and binary integer
   arithmetic over folded operands (no `/`/`%` -- Ruby floor semantics differ
   from C for negatives). Returns 1 + *out on success. */
static int fold_int_node(Compiler *c, int id, long long *out, int depth) {
  if (id < 0 || depth > 16) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "IntegerNode")) {
    if (nt_str(nt, id, "bigval")) return 0;  /* out-of-int64 literal: not a foldable int */
    *out = nt_int(nt, id, "value", 0); return 1;
  }
  if (sp_streq(ty, "ConstantReadNode")) return fold_int_const_name(c, nt_str(nt, id, "name"), out, depth + 1);
  if (sp_streq(ty, "ParenthesesNode")) {
    int bd = nt_ref(nt, id, "body"); int n = 0; const int *bb = bd >= 0 ? nt_arr(nt, bd, "body", &n) : NULL;
    return (n == 1) ? fold_int_node(c, bb[0], out, depth + 1) : 0;
  }
  if (sp_streq(ty, "CallNode")) {
    int recv = nt_ref(nt, id, "receiver");
    const char *nm = nt_str(nt, id, "name");
    int args = nt_ref(nt, id, "arguments"); int an = 0;
    const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
    if (recv < 0 || an != 1 || !nm) return 0;
    long long a, bb2;
    if (!fold_int_node(c, recv, &a, depth + 1) || !fold_int_node(c, av[0], &bb2, depth + 1)) return 0;
    if (sp_streq(nm, "+")) { *out = a + bb2; return 1; }
    if (sp_streq(nm, "-")) { *out = a - bb2; return 1; }
    if (sp_streq(nm, "*")) { *out = a * bb2; return 1; }
    if (sp_streq(nm, "<<")) { *out = a << bb2; return 1; }
    if (sp_streq(nm, ">>")) { *out = a >> bb2; return 1; }
    if (sp_streq(nm, "&")) { *out = a & bb2; return 1; }
    if (sp_streq(nm, "|")) { *out = a | bb2; return 1; }
    if (sp_streq(nm, "^")) { *out = a ^ bb2; return 1; }
    return 0;
  }
  return 0;
}

static int fold_int_const_name(Compiler *c, const char *name, long long *out, int depth) {
  if (!name || depth > 16) return 0;
  int wnode = -1;
  for (int i = 0; i < c->nt->count; i++) {
    const char *t = nt_type(c->nt, i);
    if (!t) continue;
    /* a compound/or/and write mutates the constant slot at runtime -> not a
       stable compile-time value, don't fold. */
    if (sp_streq(t, "ConstantOperatorWriteNode") || sp_streq(t, "ConstantOrWriteNode") ||
        sp_streq(t, "ConstantAndWriteNode")) {
      const char *n = nt_str(c->nt, i, "name");
      if (n && sp_streq(n, name)) return 0;
      continue;
    }
    if (!sp_streq(t, "ConstantWriteNode")) continue;
    const char *n = nt_str(c->nt, i, "name");
    if (!n || !sp_streq(n, name)) continue;
    if (wnode >= 0) return 0;   /* reassigned -> not a stable constant */
    wnode = i;
  }
  if (wnode < 0) return 0;
  return fold_int_node(c, nt_ref(c->nt, wnode, "value"), out, depth);
}

/* Emit an integer divisor operand, substituting a compile-time-constant
   value with its literal so the C compiler can strength-reduce `/`/`%` by
   a constant. Falls back to the normal expression emit otherwise. Folding
   is restricted to divisor positions: literalizing arbitrary constants
   (e.g. loop bounds in optcarrot) regresses layout-sensitive hot loops. */
void emit_int_divisor(Compiler *c, int node, Buf *b) {
  long long v;
  if (fold_int_node(c, node, &v, 0)) { buf_printf(b, "%lldLL", v); return; }
  emit_expr(c, node, b);
}

/* True if `root` contains a `break <value>` that binds to the enclosing loop
   (not one inside a nested loop, block, lambda, or def/class/module scope, which
   capture their own break — but a break in a block-bearing call's receiver or
   arguments does bind to the loop, so those are still traversed). A valued break
   makes the loop's value the break value rather than nil; detect it so a loop in
   value position rejects instead of silently yielding nil. */
static int loop_has_valued_break(Compiler *c, int root) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (sp_streq(ty, "BreakNode")) {
      int a = nt_ref(nt, root, "arguments");
      int an = 0;
      if (a >= 0) nt_arr(nt, a, "arguments", &an);
      return an > 0;
    }
    if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
        sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
        sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") || sp_streq(ty, "SingletonClassNode"))
      return 0;
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (loop_has_valued_break(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (loop_has_valued_break(c, el[j])) return 1;
  }
  return 0;
}

/* One arm of a value-position if/unless: box a concrete arm into a poly
   result, and give empty []/{} literals the result's container type. */
static void emit_ternary_arm(Compiler *c, int nd, TyKind res, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *bty = nt_type(nt, nd);
  if (res == TY_POLY && comp_ntype(c, nd) != TY_POLY) { emit_boxed(c, nd, b); return; }
  if (ty_is_array(res) && bty && sp_streq(bty, "ArrayNode")) {
    int bn = 0; nt_arr(nt, nd, "elements", &bn);
    if (bn == 0) {
      const char *rk = (res == TY_POLY_ARRAY) ? "Poly" : array_kind(res);
      buf_printf(b, "sp_%sArray_new()", rk ? rk : "Int");
      return;
    }
    emit_expr(c, nd, b);
    return;
  }
  if (ty_is_hash(res) && bty && (sp_streq(bty, "HashNode") || sp_streq(bty, "KeywordHashNode"))) {
    int bn = 0; nt_arr(nt, nd, "elements", &bn);
    const char *hc = ty_hash_cname(res);
    if (bn == 0 && hc) { buf_printf(b, "sp_%sHash_new()", hc); return; }
    emit_expr(c, nd, b);
    return;
  }
  emit_expr(c, nd, b);
}

void emit_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "expression (no type)");

  /* Hoisted call argument: substitute the rooted temp (see emit_args_filled). */
  for (int i = g_n_argov - 1; i >= 0; i--)
    if (g_argov_node[i] == id) { buf_puts(b, g_argov_text[i]); return; }

  if (sp_streq(ty, "IntegerNode")) {
    const char *bigval = nt_str(nt, id, "bigval");
    if (bigval) { buf_printf(b, "sp_bigint_new_str(\"%s\", 10)", bigval); return; }
    buf_printf(b, "%lldLL", nt_int(nt, id, "value", 0)); return;
  }
  if (sp_streq(ty, "FloatNode")) { const char *v = nt_content(nt, id); buf_puts(b, v ? v : "0.0"); return; }
  if (sp_streq(ty, "ImaginaryNode")) {
    int num = nt_ref(nt, id, "numeric");
    buf_puts(b, "((sp_Complex){0.0, (mrb_float)(");
    if (num >= 0) emit_expr(c, num, b);
    else buf_puts(b, "0");
    buf_puts(b, ")})");
    return;
  }
  if (sp_streq(ty, "RationalNode")) {
    const char *rn = nt_str(nt, id, "rat_num");
    const char *rd = nt_str(nt, id, "rat_den");
    buf_printf(b, "((sp_Rational){%sLL, %sLL})", rn ? rn : "0", rd ? rd : "1");
    return;
  }
  if (sp_streq(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_n(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0,
                       (int)nt_int(nt, id, "fzl", 0));
    return;
  }
  if (sp_streq(ty, "SourceFileNode")) {
    /* __FILE__ is a literal of its file: frozen under that file's pragma. */
    const char *sc = nt_str(nt, id, "content");
    emit_str_literal_n(b, sc ? sc : "", sc ? strlen(sc) : 0,
                       (int)nt_int(nt, id, "fzl", 0));
    return;
  }
  if (sp_streq(ty, "SourceLineNode")) {
    buf_printf(b, "%lld", (long long)nt_int(nt, id, "start_line", 0));
    return;
  }
  if (sp_streq(ty, "SourceEncodingNode")) {
    buf_puts(b, "sp_box_encoding(sp_encoding_utf8())"); return;
  }
  if (sp_streq(ty, "RegularExpressionNode")) {
    int ri = re_lit_index(c, id);
    if (ri >= 0) buf_printf(b, "sp_re_pat_%d", ri);
    else buf_puts(b, "NULL");
    return;
  }
  if (sp_streq(ty, "InterpolatedStringNode")) { emit_interp(c, id, b); return; }
  if (sp_streq(ty, "XStringNode")) {
    /* backtick content is a command argument, never a user-visible string */
    const char *sc = nt_str(nt, id, "content");
    buf_puts(b, "sp_backtick(");
    emit_str_literal_n(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0, 0);
    buf_puts(b, ")");
    return;
  }
  if (sp_streq(ty, "InterpolatedXStringNode")) {
    buf_puts(b, "sp_backtick(");
    emit_interp(c, id, b);
    buf_puts(b, ")");
    return;
  }
  if (sp_streq(ty, "InterpolatedSymbolNode")) {
    buf_puts(b, "sp_sym_intern("); emit_interp(c, id, b); buf_puts(b, ")");
    return;
  }
  if (sp_streq(ty, "TrueNode"))  { buf_puts(b, "1"); return; }
  if (sp_streq(ty, "FalseNode")) { buf_puts(b, "0"); return; }
  if (sp_streq(ty, "NilNode"))   { buf_puts(b, "0"); return; }  /* default in numeric/bool context */
  if (sp_streq(ty, "SymbolNode")) {
    int sid = comp_sym_intern(c, nt_str(nt, id, "value"));
    buf_printf(b, "((sp_sym)%d)", sid);
    return;
  }
  if (sp_streq(ty, "LambdaNode")) { emit_proc_literal(c, id, b); return; }
  if (sp_streq(ty, "CaseNode")) { emit_case_expr(c, id, b); return; }
  if (sp_streq(ty, "CaseMatchNode")) {
    /* case/in as a value: declare a result temp, emit the match into g_pre
       with each arm assigning its body value to it, then yield the temp. */
    TyKind rt = comp_ntype(c, id);
    if (rt == TY_UNKNOWN || rt == TY_VOID || rt == TY_NIL) rt = TY_POLY;
    int cr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", cr, rt == TY_RANGE ? "(sp_Range){0}" : default_value(rt));
    if (needs_root(rt)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", cr); }
    emit_case_match(c, id, g_pre, g_indent, 0, cr);
    buf_printf(b, "_t%d", cr);
    return;
  }

  if (sp_streq(ty, "MatchWriteNode")) {
    /* `/(?<n>..)/ =~ str`: run the match (setting the match registers), bind
       each named group to its local (NULL = nil when it did not participate),
       and yield the `=~` result (match index, or nil). */
    int call = nt_ref(nt, id, "call");
    int recv = call >= 0 ? nt_ref(nt, call, "receiver") : -1;
    int reidx = re_lit_index(c, recv);
    int args = call >= 0 ? nt_ref(nt, call, "arguments") : -1;
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (reidx < 0 || ac < 1) { unsupported(c, id, "named-capture =~ (non-literal regexp)"); return; }
    int tcount = 0; const int *tv = nt_arr(nt, id, "targets", &tcount);
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = sp_re_match_poly(sp_re_pat_%d, ", t, reidx);
    emit_str_expr(c, av[0], b);
    buf_puts(b, "); ");
    for (int ti = 0; ti < tcount; ti++) {
      const char *tnm = nt_str(nt, tv[ti], "name");
      if (!tnm) continue;
      buf_printf(b, "lv_%s = sp_re_named_capture(sp_re_pat_%d, ", rename_local(tnm), reidx);
      emit_str_literal(b, tnm);
      buf_puts(b, "); ");
    }
    buf_printf(b, "_t%d; })", t);
    return;
  }
  if (sp_streq(ty, "RangeNode")) {
    int left = nt_ref(nt, id, "left");
    int right = nt_ref(nt, id, "right");
    int excl = (int)(nt_int(nt, id, "flags", 0) & 4) ? 1 : 0;
    buf_puts(b, "sp_range_new(");
    if (left >= 0) emit_int_expr(c, left, b); else buf_puts(b, "INTPTR_MIN");  /* beginless */
    buf_puts(b, ", ");
    if (right >= 0) emit_int_expr(c, right, b); else buf_puts(b, "INTPTR_MAX");  /* endless */
    buf_printf(b, ", %d)", excl);
    return;
  }
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *lrn = nt_str(nt, id, "name");
    /* compile-time define_method substitution: the loop var IS the literal */
    if (g_dm_subst_name && lrn && sp_streq(lrn, g_dm_subst_name) && g_dm_subst_node >= 0) {
      emit_expr(c, g_dm_subst_node, b); return;
    }
    /* a `return .. if p.nil?`-guarded param read: unbox the poly slot to the
       non-nil type the narrowing pass proved (#1661) */
    if (c->nilnarrow[id] != TY_UNKNOWN) {
      Buf rb2; memset(&rb2, 0, sizeof rb2);
      emit_local_ref(c, id, lrn, &rb2);
      emit_unbox_text(c, c->nilnarrow[id], rb2.p ? rb2.p : "", b);
      free(rb2.p);
      return;
    }
    LocalVar *slv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
    if (slv && slv->type == TY_STRBUF) {
      /* A mutable-string local read yields an independent GC string copy: its
         sp_String buffer is not itself a GC object, so a bare cstr pointer
         would dangle once the wrapper is unreachable (e.g. after `return`).
         Transient append/length use the raw sp_String via dedicated paths. */
      buf_printf(b, "sp_str_concat(sp_String_cstr(lv_%s), (&(\"\\xff\")[1]))", rename_local(lrn));
      return;
    }
    emit_local_ref(c, id, lrn, b); return;
  }
  if (sp_streq(ty, "LocalVariableWriteNode")) {
    /* assignment used as expression: ({ lv = rhs; lv; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    /* `x = y = nil` as expression: inner writes become statements inside the
       stmt-expr; this target takes its own typed nil (not the inner slot). */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) {
        buf_puts(b, "({ ");
        emit_stmt_inner(c, v, b, 0);
        emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
        if (lv && lv->type == TY_RANGE) buf_puts(b, "(sp_Range){0}");
        else if (lv) buf_puts(b, default_value(lv->type));
        else buf_puts(b, "sp_box_nil()");
        buf_puts(b, "; "); emit_local_ref(c, id, nm, b); buf_puts(b, "; })");
        return;
      }
    }
    buf_puts(b, "({ ");
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    if (lv && lv->type == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    buf_puts(b, "; "); emit_local_ref(c, id, nm, b); buf_puts(b, "; })");
    return;
  }
  if (sp_streq(ty, "LocalVariableOperatorWriteNode")) {
    /* c += 3 used as expression: ({ <op-assign>; <c>; }). The value-yielding
       read goes through emit_local_ref so a celled (captured) local derefs its
       shared cell (*_cell_c) instead of a bare lv_c -- the latter is undeclared
       inside a block that captured c by cell (e.g. `mutex.synchronize { c += 1 }`
       in a thread). */
    const char *nm = nt_str(nt, id, "name");
    buf_puts(b, "({ ");
    emit_op_assign(c, id, b, 0);
    emit_local_ref(c, id, nm, b);
    buf_puts(b, "; })");
    return;
  }
  if (sp_streq(ty, "InstanceVariableWriteNode")) {
    /* @ivar = rhs used as expression: ({ self->iv_x = rhs; self->iv_x; }) */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws = comp_scope_of(c, id);
    int cid2 = cws ? cws->class_id : -1;
    if (cid2 < 0 && g_class_body_id >= 0) cid2 = g_class_body_id;
    if (!nm || v < 0) { buf_puts(b, "0"); return; }
    /* inside an instance_eval/exec splice the block scope has no class_id, so
       the ivar belongs to the rebound receiver class (g_ie_class_id). */
    int ivcls2 = cid2 >= 0 ? cid2 : g_ie_class_id;
    TyKind ivt2 = TY_UNKNOWN;
    if (ivcls2 >= 0) {
      int iv2 = comp_ivar_index(&c->classes[ivcls2], nm);
      if (iv2 >= 0) ivt2 = c->classes[ivcls2].ivar_types[iv2];
    }
    const char *vty2 = nt_type(nt, v);
    int ven2 = 0;
    int v_empty_array2 = vty2 && sp_streq(vty2, "ArrayNode") && (nt_arr(nt, v, "elements", &ven2), ven2 == 0);
    int v_empty_hash2 = 0;
    if (!v_empty_array2 && vty2) {
      int hen2 = 0;
      if (sp_streq(vty2, "HashNode") || sp_streq(vty2, "KeywordHashNode"))
        v_empty_hash2 = (nt_arr(nt, v, "elements", &hen2), hen2 == 0);
    }
    char ref2e[300];
    if (cws && cws->is_cmethod && cid2 >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_%s_%s", c->classes[cid2].name, nm + 1);
    else if (cid2 < 0 && g_ie_class_id >= 0)
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    else if (cid2 < 0 && comp_class_index(c, "Toplevel") >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_Toplevel_%s", nm + 1);
    else
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    /* `@a = @b = nil` as expression: inner writes become statements inside the
       stmt-expr; this target takes its own typed nil (not the inner slot). */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) {
        buf_puts(b, "({ ");
        emit_stmt_inner(c, v, b, 0);
        buf_printf(b, "%s = ", ref2e);
        if (ivt2 == TY_RANGE) buf_puts(b, "(sp_Range){0}");
        else if (ivt2 == TY_POLY) buf_puts(b, "sp_box_nil()");
        else if (ivt2 == TY_INT) buf_puts(b, "SP_INT_NIL");
        else if (ivt2 == TY_FLOAT) buf_puts(b, "sp_float_nil()");
        else if (ivt2 == TY_STRING) buf_puts(b, "NULL");
        else buf_puts(b, default_value(ivt2));
        buf_printf(b, "; %s; })", ref2e);
        return;
      }
    }
    buf_puts(b, "({ ");
    buf_printf(b, "%s = ", ref2e);
    if (v_empty_array2 && ivt2 == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array2 && array_kind(ivt2)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt2));
    else if (v_empty_hash2 && ty_is_hash(ivt2)) {
      const char *hcn = ty_hash_cname(ivt2);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt2 == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    else if (ivt2 != TY_POLY && ivt2 != TY_UNKNOWN && comp_ntype(c, v) == TY_POLY) {
      /* poly rhs assigned to a typed ivar: unbox to the concrete type */
      Buf _rb; memset(&_rb, 0, sizeof _rb);
      emit_expr(c, v, &_rb);
      emit_unbox_text(c, ivt2, _rb.p ? _rb.p : "sp_box_nil()", b);
      free(_rb.p);
    }
    else emit_expr(c, v, b);
    buf_printf(b, "; %s; })", ref2e);
    return;
  }
  if (sp_streq(ty, "InstanceVariableOrWriteNode") || sp_streq(ty, "InstanceVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "InstanceVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *cws3 = comp_scope_of(c, id);
    int cid3 = cws3 ? cws3->class_id : -1;
    if (cid3 < 0 && g_class_body_id >= 0) cid3 = g_class_body_id;
    TyKind ivt3 = TY_UNKNOWN;
    if (cid3 >= 0) { int iv3 = comp_ivar_index(&c->classes[cid3], nm); if (iv3 >= 0) ivt3 = c->classes[cid3].ivar_types[iv3]; }
    char ref3[300];
    if (cws3 && cws3->is_cmethod && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_%s_%s", c->classes[cid3].name, nm + 1);
    else
      snprintf(ref3, sizeof ref3, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    if (ivt3 == TY_POLY) {
      buf_printf(b, "({ if (%ssp_poly_truthy(%s)) %s = ", is_or ? "!" : "", ref3, ref3);
      emit_boxed(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (ivt3 == TY_BOOL) {
      buf_printf(b, "({ if (%s%s) %s = ", is_or ? "!" : "", ref3, ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (ivt3 == TY_INT) {
      if (is_or) {
        buf_printf(b, "({ if (%s == SP_INT_NIL) %s = ", ref3, ref3);
        emit_expr(c, v, b);
        buf_printf(b, "; %s; })", ref3);
      }
      else {
        buf_printf(b, "({ if (%s != SP_INT_NIL) %s = ", ref3, ref3);
        emit_expr(c, v, b);
        buf_printf(b, "; %s; })", ref3);
      }
    }
    else if (ivt3 == TY_SYMBOL) {
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      buf_printf(b, "({ if (%s %s= (sp_sym)-1) %s = ", ref3, is_or ? "=" : "!", ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (ivt3 == TY_STRING) {
      if (is_or) {
        buf_printf(b, "({ if (!%s) %s = ", ref3, ref3);
        emit_expr(c, v, b);
        buf_printf(b, "; %s; })", ref3);
      }
      else {
        buf_printf(b, "({ if (%s) %s = ", ref3, ref3);
        emit_expr(c, v, b);
        buf_printf(b, "; %s; })", ref3);
      }
    }
    /* a pointer-backed ivar (object/array/hash/fiber/proc/...) reads falsy
       when NULL, so `@x ||= v` is `if (!@x) @x = v` and `@x &&= v` is
       `if (@x) @x = v`. Falling through to a bare read dropped the init when
       this or-write was the RHS of a poly-receiver setter switch (#1447). */
    else if (ty_is_object(ivt3) || ty_is_array(ivt3) || ty_is_hash(ivt3) ||
             ivt3 == TY_FIBER || ivt3 == TY_THREAD || ivt3 == TY_QUEUE || ivt3 == TY_MUTEX || ivt3 == TY_CONDVAR || ivt3 == TY_PROC || ivt3 == TY_IO ||
             ivt3 == TY_STRINGIO || ivt3 == TY_STRINGSCANNER ||
             ivt3 == TY_MATCHDATA || ivt3 == TY_EXCEPTION || ivt3 == TY_REGEX) {
      buf_printf(b, "({ if (%s%s) %s = ", is_or ? "!" : "", ref3, ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else if (!is_or) {
      buf_printf(b, "({ %s = ", ref3);
      emit_expr(c, v, b);
      buf_printf(b, "; %s; })", ref3);
    }
    else buf_puts(b, ref3);
    return;
  }
  if (sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    const char *en = rename_local(nm);
    if (t == TY_POLY) {
      buf_printf(b, "({ if (%ssp_poly_truthy(lv_%s)) lv_%s = ", is_or ? "!" : "", en, en);
      emit_boxed(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else if (t == TY_BOOL) {
      buf_printf(b, "({ if (%slv_%s) lv_%s = ", is_or ? "!" : "", en, en);
      emit_expr(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else if (t == TY_SYMBOL) {
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      buf_printf(b, "({ if (lv_%s %s= (sp_sym)-1) lv_%s = ", en, is_or ? "=" : "!", en);
      emit_expr(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else if (!is_or) {
      buf_printf(b, "({ lv_%s = ", en);
      emit_expr(c, v, b);
      buf_printf(b, "; lv_%s; })", en);
    }
    else {
      buf_printf(b, "lv_%s", en);
    }
    return;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode")) {
    /* A loop in value position runs for its side effects and evaluates to nil.
       A valued `break` would instead make the loop yield that value; that is a
       separate gap, so reject it here rather than silently producing nil. */
    if (loop_has_valued_break(c, nt_ref(nt, id, "statements")))
      unsupported(c, id, "valued break in loop used as a value");
    /* Run the loop as a statement inside a GCC statement-expression, then
       yield the boxed nil. */
    buf_puts(b, "({ ");
    emit_while(c, id, b, 0, sp_streq(ty, "UntilNode"));
    buf_puts(b, "sp_box_nil(); })");
    return;
  }
  if (sp_streq(ty, "IndexOrWriteNode") || sp_streq(ty, "IndexAndWriteNode")) {
    int is_or2 = sp_streq(ty, "IndexOrWriteNode");
    int ir = nt_ref(nt, id, "receiver");
    int ia = nt_ref(nt, id, "arguments");
    int iv = nt_ref(nt, id, "value");
    int iac = 0; const int *iav = ia >= 0 ? nt_arr(nt, ia, "arguments", &iac) : NULL;
    if (iac != 1 || ir < 0 || iv < 0) { unsupported(c, id, "index-or/and-write (expr)"); return; }
    TyKind irt = comp_ntype(c, ir);
    int ta2 = ++g_tmp, tb2 = ++g_tmp, tc2 = ++g_tmp;
    if (irt == TY_POLY_ARRAY) {
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; mrb_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
      emit_boxed(c, iv, b);
      buf_printf(b, "; sp_PolyArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_INT_ARRAY) {
      buf_printf(b, "({ sp_IntArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; mrb_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; mrb_int _t%d = sp_IntArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%s(_t%d == SP_INT_NIL)) { _t%d = ", is_or2 ? "" : "!", tc2, tc2);
      emit_expr(c, iv, b);
      buf_printf(b, "; sp_IntArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_FLOAT_ARRAY) {
      buf_printf(b, "({ sp_FloatArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; mrb_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; mrb_float _t%d = sp_FloatArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%ssp_float_is_nil(_t%d)) { _t%d = ", is_or2 ? "" : "!", tc2, tc2);
      emit_expr(c, iv, b);
      buf_printf(b, "; sp_FloatArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_STR_ARRAY) {
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; mrb_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; const char *_t%d = sp_StrArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%s_t%d) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
      emit_expr(c, iv, b);
      buf_printf(b, "; sp_StrArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (ty_is_hash(irt)) {
      const char *hn = ty_hash_cname(irt);
      TyKind kt = ty_hash_key(irt);
      TyKind vt = ty_hash_val(irt);
      if (!hn) { unsupported(c, id, "index-or/and-write (expr, unknown hash)"); return; }
      buf_printf(b, "({ %s _t%d = ", c_type_name(irt), ta2); emit_expr(c, ir, b);
      buf_printf(b, "; %s _t%d = ", c_type_name(kt), tb2); emit_hash_key(c, iav[0], kt, b);
      if (vt == TY_POLY) {
        buf_printf(b, "; sp_RbVal _t%d = sp_%sHash_get(_t%d, _t%d);", tc2, hn, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
        emit_boxed(c, iv, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, _t%d, _t%d); } _t%d; })", hn, ta2, tb2, tc2, tc2);
      }
      else {
        buf_printf(b, "; %s _t%d = sp_%sHash_get(_t%d, _t%d);", c_type_name(vt), tc2, hn, ta2, tb2);
        buf_printf(b, " if (%s_t%d) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
        emit_expr(c, iv, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, _t%d, _t%d); } _t%d; })", hn, ta2, tb2, tc2, tc2);
      }
    }
    else if (irt == TY_POLY) {
      /* TY_POLY receiver: dispatch via sp_poly_get/set based on key type */
      TyKind kt2 = comp_ntype(c, iav[0]);
      buf_printf(b, "({ sp_RbVal _t%d = ", ta2); emit_expr(c, ir, b);
      buf_puts(b, "; ");
      if (kt2 == TY_INT) {
        buf_printf(b, "mrb_int _t%d = ", tb2); emit_int_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
        emit_boxed(c, iv, b);
        buf_printf(b, "; sp_poly_arr_set_hash(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else if (kt2 == TY_STRING) {
        buf_printf(b, "const char *_t%d = ", tb2); emit_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_get_str(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
        emit_boxed(c, iv, b);
        buf_printf(b, "; sp_poly_set_str(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else {
        /* poly key fallback: box the key, look up polymorphically, set via poly */
        buf_printf(b, "sp_RbVal _t%d = ", tb2); emit_boxed(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_index_poly(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { _t%d = ", is_or2 ? "!" : "", tc2, tc2);
        emit_boxed(c, iv, b);
        buf_printf(b, "; sp_poly_set_poly(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
    }
    else {
      unsupported(c, id, "index-or/and-write (expr, unsupported recv type)");
    }
    return;
  }
  if (sp_streq(ty, "YieldNode")) {
    /* Lowered self-recursive yield method: `yield` calls the synthetic __yblk__ proc. */
    if (g_current_scope_is_lowered) {
      int yargs = nt_ref(nt, id, "arguments");
      int yargc = 0; const int *yargv = yargs >= 0 ? nt_arr(nt, yargs, "arguments", &yargc) : NULL;
      /* Lowered yield method returns mrb_int (raw carrier for any type).
         sp_proc_call already returns mrb_int; emit it directly with no cast. */
      buf_puts(b, "sp_proc_call(");
      emit_yblk_ref(b);
      buf_puts(b, ", ");
      emit_proc_call_args(c, yargc, yargv, b, 0);
      return;
    }
    if (g_block_id < 0) {  /* no block: yield is nil */
      /* box the sentinel when the yield value feeds a poly slot (its method
         return widened to poly in promote mode) */
      buf_puts(b, comp_ntype(c, id) == TY_POLY ? "sp_box_nil()" : "SP_INT_NIL");
      return;
    }
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1);
    return;
  }
  if (is_block_call(c, id)) {           /* block.call used for its value */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1);
    return;
  }
  if (is_blockless_block_param_call(c, id)) {  /* dead path: no block supplied */
    buf_puts(b, default_value(comp_ntype(c, id)));
    return;
  }
  if (sp_streq(ty, "SelfNode")) { buf_puts(b, g_self); return; }  /* self is the object reference (pointer) */
  if (sp_streq(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@x" */
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      buf_printf(b, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);  /* module/class-level ivar */
    else if (cs && cs->class_id < 0 && g_ie_class_id >= 0)
      /* inside instance_eval block: access ivar via receiver pointer */
      buf_printf(b, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    else if (cs && cs->class_id < 0) {
      /* top-level method: ivar stored as file-scope global in Toplevel pseudo-class */
      int tl = comp_class_index(c, "Toplevel");
      if (tl >= 0) buf_printf(b, "civ_Toplevel_%s", nm + 1);
      else buf_printf(b, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    }
    else
      buf_printf(b, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    return;
  }
  if (sp_streq(ty, "ClassVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@@x" */
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid >= 0) { buf_printf(b, "cvar_%s_%s", c->classes[cid].name, nm + 2); return; }
    unsupported(c, id, "class variable read (no class scope)");
  }
  if (sp_streq(ty, "ClassVariableWriteNode")) {  /* in value position: yields the assigned value */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[cid], nm);
    if (idx >= 0) ct = c->classes[cid].cvar_types[idx];
    buf_printf(b, "(cvar_%s_%s = ", c->classes[cid].name, nm + 2);
    if (ct == TY_POLY) emit_boxed(c, v, b); else emit_expr(c, v, b);
    buf_puts(b, ")");
    return;
  }
  if (sp_streq(ty, "ClassVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable op-write (no class scope)"); return; }
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[cid], nm);
    if (idx >= 0) ct = c->classes[cid].cvar_types[idx];
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    if (ct == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "(%s = sp_str_plus(%s, ", ref, ref);
      emit_expr(c, v, b); buf_puts(b, "))");
    }
    else if (ct == TY_POLY) {
      const char *pfn = sp_streq(op ? op : "+", "+") ? "sp_poly_add"
                      : sp_streq(op, "-") ? "sp_poly_sub"
                      : sp_streq(op, "*") ? "sp_poly_mul"
                      : sp_streq(op, "/") ? "sp_poly_div" : NULL;
      int bitop = op && (sp_streq(op, "<<") || sp_streq(op, ">>") || sp_streq(op, "&") ||
                         sp_streq(op, "|") || sp_streq(op, "^") || sp_streq(op, "%"));
      if (pfn) { buf_printf(b, "(%s = %s(%s, ", ref, pfn, ref); emit_boxed(c, v, b); buf_puts(b, "))"); }
      else if (bitop) { buf_printf(b, "(%s = sp_box_int(sp_poly_to_i(%s) %s ", ref, ref, op); emit_int_expr(c, v, b); buf_puts(b, "))"); }
      else { buf_printf(b, "(%s %s= ", ref, op ? op : "+"); emit_expr(c, v, b); buf_puts(b, ")"); }
    }
    else {
      buf_printf(b, "(%s %s= ", ref, op ? op : "+");
      emit_expr(c, v, b); buf_puts(b, ")");
    }
    return;
  }
  if (sp_streq(ty, "ClassVariableOrWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable or-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    int oidx = comp_cvar_index(&c->classes[cid], nm);
    if (oidx >= 0 && c->classes[cid].cvar_types[oidx] == TY_POLY) {
      buf_printf(b, "(sp_poly_truthy(%s) ? %s : (%s = ", ref, ref, ref);
      emit_boxed(c, v, b); buf_puts(b, "))");
    }
    else { buf_printf(b, "(%s ? %s : (%s = ", ref, ref, ref); emit_expr(c, v, b); buf_puts(b, "))"); }
    return;
  }
  if (sp_streq(ty, "ClassVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable and-write (no class scope)"); return; }
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    int aidx = comp_cvar_index(&c->classes[cid], nm);
    if (aidx >= 0 && c->classes[cid].cvar_types[aidx] == TY_POLY) {
      buf_printf(b, "(sp_poly_truthy(%s) ? (%s = ", ref, ref);
      emit_boxed(c, v, b); buf_puts(b, ") : sp_box_nil())");
    }
    else { buf_printf(b, "(%s ? (%s = ", ref, ref); emit_expr(c, v, b); buf_puts(b, ") : 0)"); }
    return;
  }
  if (sp_streq(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ is the record separator "\n"; $! / $; /
       $, read nil (spinel doesn't honor the split/print-sep defaults) */
    if (nm && sp_streq(nm, "$/")) { emit_str_literal(b, "\n"); return; }
    if (nm && sp_streq(nm, "$?")) { buf_puts(b, "sp_last_status"); return; }
    if (nm && (sp_streq(nm, "$PROGRAM_NAME") || sp_streq(nm, "$0"))) { buf_puts(b, "sp_program_name"); return; }
    if (nm && (sp_streq(nm, "$!") || sp_streq(nm, "$;") || sp_streq(nm, "$,"))) { buf_puts(b, "0"); return; }
    /* regex match globals that Prism may emit as GlobalVariableReadNode */
    if (nm && (sp_streq(nm, "$~") || sp_streq(nm, "$&")))  { buf_puts(b, "sp_re_match_str");  return; }
    if (nm && sp_streq(nm, "$`"))                          { buf_puts(b, "sp_re_match_pre");  return; }
    if (nm && sp_streq(nm, "$'"))                          { buf_puts(b, "sp_re_match_post"); return; }
    if (nm && sp_streq(nm, "$+")) {
      buf_puts(b, "({ int _bri = 9; while (_bri > 0 && !sp_re_captures[_bri-1]) _bri--; _bri > 0 ? sp_re_captures[_bri-1] : NULL; })");
      return;
    }
    if (nm && nm[0] == '$') {
      const char *rn = comp_resolve_gvar(c, nm + 1);
      if (comp_gvar(c, rn)) { buf_printf(b, "gv_%s", rn); return; }
    }
    unsupported(c, id, "global variable read");
  }
  if (sp_streq(ty, "NumberedReferenceReadNode")) {
    /* $1..$9 -> the n-th capture of the last match (NULL when absent) */
    long long n = nt_int(nt, id, "number", 0);
    if (n >= 1 && n <= 9) buf_printf(b, "sp_re_captures[%lld]", n);
    else buf_puts(b, "NULL");
    return;
  }
  if (sp_streq(ty, "BackReferenceReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    if (!nm) { buf_puts(b, "NULL"); return; }
    if (sp_streq(nm, "$&") || sp_streq(nm, "$~")) buf_puts(b, "sp_re_match_str");
    else if (sp_streq(nm, "$`"))                 buf_puts(b, "sp_re_match_pre");
    else if (sp_streq(nm, "$'"))                 buf_puts(b, "sp_re_match_post");
    else if (sp_streq(nm, "$+")) {
      /* last group that participated: scan captures[] backwards */
      buf_puts(b, "({ int _bri = 9; while (_bri > 0 && !sp_re_captures[_bri-1]) _bri--; _bri > 0 ? sp_re_captures[_bri-1] : NULL; })");
    }
    else buf_puts(b, "NULL");
    return;
  }
  if (sp_streq(ty, "ConstantReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cv = nm ? comp_const(c, nm) : NULL;
    if (cv && cv->type != TY_UNKNOWN) {
      if (cv->init_guarded) {
        /* a read during the const's own Class.new init raises NameError */
        buf_printf(b, "(sp_init_in_progress_%s ? (sp_raise_cls(\"NameError\","
                      " \"uninitialized constant %s\"), cst_%s) : cst_%s)", nm, nm, nm, nm);
      }
      else buf_printf(b, "cst_%s", nm);
      return;
    }
    if (nm && sp_streq(nm, "RUBY_DESCRIPTION")) { buf_puts(b, "SPL(\"spinel\")"); return; }
    if (nm && sp_streq(nm, "RUBY_VERSION"))     { buf_puts(b, "SPL(\"3.2.0\")"); return; }
    if (nm && sp_streq(nm, "RUBY_ENGINE"))      { buf_puts(b, "SPL(\"ruby\")"); return; }
    if (nm && sp_streq(nm, "RUBY_ENGINE_VERSION")) { buf_puts(b, "SPL(\"3.2.0\")"); return; }
    if (nm && sp_streq(nm, "RUBY_PLATFORM"))    { buf_puts(b, "sp_ruby_platform_str()"); return; }
    if (nm && sp_streq(nm, "RUBY_RELEASE_DATE")) { buf_puts(b, "SPL(\"2023-03-30\")"); return; }
    if (nm && sp_streq(nm, "RUBY_REVISION"))    { buf_puts(b, "SPL(\"0\")"); return; }
    if (nm && sp_streq(nm, "RUBY_COPYRIGHT"))   { buf_puts(b, "SPL(\"ruby - Copyright (C) 1993-2023 Yukihiro Matsumoto\")"); return; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return; }
    if (nm && sp_streq(nm, "STDOUT")) { buf_puts(b, "sp_io_stdout()"); return; }
    if (nm && sp_streq(nm, "STDERR")) { buf_puts(b, "sp_io_stderr()"); return; }
    if (nm) {
      int _cidx = comp_class_index(c, nm);
      if (_cidx >= 0) {
        buf_printf(b, "((sp_Class){%d})", _cidx);  /* user class as value: TY_CLASS unboxed */
      }
      else {
        int _bcid = builtin_class_id(nm);
        if (_bcid != 0)
          buf_printf(b, "((sp_Class){%d})", _bcid);  /* builtin class as value */
        else
          buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), ((sp_Class){-1}))", nm);
      }
    }
    else unsupported(c, id, "constant read");
    return;
  }
  if (sp_streq(ty, "ConstantPathNode")) {
    /* M::CONST -> the flat constant named by the final path component */
    const char *nm = nt_str(nt, id, "name");
    LocalVar *cpcv = nm ? comp_const(c, nm) : NULL;
    if (cpcv && cpcv->type != TY_UNKNOWN) { buf_printf(b, "cst_%s", nm); return; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return; }
    /* well-known module constants */
    int par_idc = nt_ref(nt, id, "parent");
    const char *par_tyc = par_idc >= 0 ? nt_type(nt, par_idc) : NULL;
    const char *par_nmc = (par_tyc && sp_streq(par_tyc, "ConstantReadNode")) ? nt_str(nt, par_idc, "name") : NULL;
    if (par_nmc && sp_streq(par_nmc, "Float") && nm) {
      if (sp_streq(nm, "MAX"))      { buf_puts(b, "DBL_MAX"); return; }
      if (sp_streq(nm, "MIN"))      { buf_puts(b, "DBL_MIN"); return; }
      if (sp_streq(nm, "EPSILON"))  { buf_puts(b, "DBL_EPSILON"); return; }
      if (sp_streq(nm, "INFINITY")) { buf_puts(b, "(1.0/0.0)"); return; }
      if (sp_streq(nm, "NAN"))      { buf_puts(b, "(0.0/0.0)"); return; }
      if (sp_streq(nm, "DIG"))      { buf_printf(b, "(double)DBL_DIG"); return; }
      if (sp_streq(nm, "MANT_DIG")) { buf_printf(b, "(double)DBL_MANT_DIG"); return; }
      if (sp_streq(nm, "RADIX"))    { buf_printf(b, "(double)FLT_RADIX"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Math") && nm) {
      if (sp_streq(nm, "PI")) { buf_puts(b, "M_PI"); return; }
      if (sp_streq(nm, "E"))  { buf_puts(b, "M_E"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "File") && nm) {
      /* Emit marker-framed literals (\xff prefix at [-1]) like every other
         spinel string, so sp_str_byte_len/sp_str_concat can read the length
         marker without an out-of-bounds [-1] over-read on a bare literal. */
      if (sp_streq(nm, "SEPARATOR"))      { buf_puts(b, "(&(\"\\xff\" \"/\")[1])"); return; }
      if (sp_streq(nm, "PATH_SEPARATOR")) { buf_puts(b, "(&(\"\\xff\" \":\")[1])"); return; }
      if (sp_streq(nm, "ALT_SEPARATOR"))  { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Process") && nm) {
      if (sp_streq(nm, "CLOCK_MONOTONIC")) { buf_puts(b, "((mrb_int)1)"); return; }
      if (sp_streq(nm, "CLOCK_REALTIME"))  { buf_puts(b, "((mrb_int)0)"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Integer") && nm &&
        (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) {
      /* Integer::MAX/MIN do not exist in Ruby — raise NameError at runtime */
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant Integer::%s\"), 0)", nm);
      return;
    }
    /* FFI const: Module::NAME -> integer literal */
    if (par_nmc && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (sp_streq(c->ffi_consts[fci].mod, par_nmc) &&
            sp_streq(c->ffi_consts[fci].name, nm)) {
          buf_printf(b, "((mrb_int)%d)", c->ffi_consts[fci].val);
          return;
        }
      }
    }
    /* class/module constant as value */
    if (nm) {
      int _cpidx = comp_class_index(c, nm);
      if (_cpidx >= 0) { buf_printf(b, "((sp_Class){%d})", _cpidx); return; }
      int _bcpid = builtin_class_id(nm);
      if (_bcpid != 0) { buf_printf(b, "((sp_Class){%d})", _bcpid); return; }
    }
    /* unresolved qualified constant: raise NameError at runtime */
    {
      char fullname[512];
      if (par_nmc && nm) snprintf(fullname, sizeof fullname, "%s::%s", par_nmc, nm);
      else if (nm) snprintf(fullname, sizeof fullname, "%s", nm);
      else snprintf(fullname, sizeof fullname, "?");
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), ((sp_Class){-1}))", fullname);
    }
    return;
  }
  if (sp_streq(ty, "DefinedNode")) {
    /* compile-time defined? -> a label string, or nil (NULL) when undefined */
    int v = nt_ref(nt, id, "value");
    const char *vt = v >= 0 ? nt_type(nt, v) : NULL;
    const char *res = NULL;
    if (vt) {
      if (sp_streq(vt, "LocalVariableReadNode")) res = "local-variable";
      else if (sp_streq(vt, "InstanceVariableReadNode")) {
        /* Return "instance-variable" only when the ivar is known to be assigned. */
        const char *inm = nt_str(nt, v, "name");
        for (int kk = 0; kk < nt->count && !res; kk++) {
          const char *kt = nt_type(nt, kk);
          if (kt && sp_streq(kt, "InstanceVariableWriteNode") &&
              inm && nt_str(nt, kk, "name") && sp_streq(nt_str(nt, kk, "name"), inm))
            res = "instance-variable";
        }
      }
      else if (sp_streq(vt, "ClassVariableReadNode")) res = "class variable";
      else if (sp_streq(vt, "SelfNode")) res = "self";
      else if (sp_streq(vt, "NilNode")) res = "nil";
      else if (sp_streq(vt, "TrueNode")) res = "true";
      else if (sp_streq(vt, "FalseNode")) res = "false";
      else if (sp_streq(vt, "IntegerNode") || sp_streq(vt, "FloatNode") ||
               sp_streq(vt, "StringNode") || sp_streq(vt, "SymbolNode") || sp_streq(vt, "ArrayNode")) res = "expression";
      else if (sp_streq(vt, "GlobalVariableReadNode")) {
        const char *gn = nt_str(nt, v, "name");
        for (int kk = 0; kk < nt->count && !res; kk++) {
          const char *kt = nt_type(nt, kk);
          if (kt && (sp_streq(kt, "GlobalVariableWriteNode") || sp_streq(kt, "GlobalVariableOperatorWriteNode")) &&
              gn && nt_str(nt, kk, "name") && sp_streq(nt_str(nt, kk, "name"), gn))
            res = "global-variable";
        }
      }
      else if (sp_streq(vt, "ConstantReadNode")) {
        const char *cn = nt_str(nt, v, "name");
        static const char *const builtins[] = {
          "Object", "BasicObject", "Kernel", "Module", "Class", "Array", "Hash",
          "String", "Integer", "Float", "Symbol", "Regexp", "Range", "NilClass",
          "TrueClass", "FalseClass", "Numeric", "Comparable", "Enumerable",
          "IO", "File", "Dir", "Math", "GC", "Process", "ENV", "ARGV",
          "STDOUT", "STDERR", "STDIN", NULL
        };
        if (cn) {
          if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) res = "constant";
          if (!res) {
            for (int bi = 0; builtins[bi]; bi++)
              if (sp_streq(cn, builtins[bi])) { res = "constant"; break; }
          }
        }
      }
      else if (sp_streq(vt, "CallNode") && nt_ref(nt, v, "receiver") < 0) {
        const char *cn = nt_str(nt, v, "name");
        if (cn && comp_method_index(c, cn) >= 0) res = "method";
      }
    }
    if (res) buf_printf(b, "SPL(\"%s\")", res);
    else buf_puts(b, "NULL");
    return;
  }
  if (sp_streq(ty, "ParenthesesNode")) {
    int body = nt_ref(nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
    if (n == 0) { buf_puts(b, "sp_box_nil()"); return; }
    if (n == 1) {
      buf_puts(b, "("); emit_expr(c, bd[0], b); buf_puts(b, ")");
      return;
    }
    /* Multi-stmt parens: `(s1; s2; expr)` — run leading stmts in prelude,
       return value of last expression via GNU statement expression. */
    buf_puts(b, "({ ");
    for (int j = 0; j < n - 1; j++) {
      emit_stmt(c, bd[j], b, 0);
    }
    emit_expr(c, bd[n - 1], b);
    buf_puts(b, "; })");
    return;
  }
  if (sp_streq(ty, "ArrayNode")) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    TyKind at = comp_ntype(c, id);
    /* an empty `[]` literal carries no element type of its own; it is
       emitted via the target's type in emit_assign. If we reach here for
       an empty literal, use g_ret_type context (e.g. tail position in a
       poly_array-returning method) before falling back to int array. */
    if (n == 0 && at == TY_UNKNOWN && ty_is_array(g_ret_type)) at = g_ret_type;
    const char *k = array_kind(at);
    if (n == 0 && !k && at != TY_POLY_ARRAY) { buf_puts(b, "sp_IntArray_new()"); return; }
    /* poly (mixed-element) array: build an sp_PolyArray of boxed elements */
    if (at == TY_POLY_ARRAY) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", t);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
      for (int j = 0; j < n; j++) {
        const char *ety = nt_type(nt, els[j]);
        if (ety && sp_streq(ety, "SplatNode")) {
          /* [*arr] or [*range] — expand into poly */
          int inner = nt_ref(nt, els[j], "expression");
          TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
          Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
          const char *ep = el.p ? el.p : "NULL";
          emit_indent(g_pre, g_indent);
          if (it == TY_RANGE) {
            /* check if it's a string range (bounds are TY_STRING) */
            int rn = nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "RangeNode") ? inner : -1;
            int rlo = rn >= 0 ? nt_ref(nt, rn, "left") : -1;
            int rhi = rn >= 0 ? nt_ref(nt, rn, "right") : -1;
            int rexcl = rn >= 0 ? (int)(nt_int(nt, rn, "flags", 0) & 4) : 0;
            if (rlo >= 0 && comp_ntype(c, rlo) == TY_STRING) {
              Buf lo_b; memset(&lo_b, 0, sizeof lo_b); emit_expr(c, rlo, &lo_b);
              Buf hi_b; memset(&hi_b, 0, sizeof hi_b); emit_expr(c, rhi, &hi_b);
              buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }\n",
                         lo_b.p ? lo_b.p : "NULL", hi_b.p ? hi_b.p : "NULL", rexcl, t);
              free(lo_b.p); free(hi_b.p);
            }
            else {
              buf_printf(g_pre, "{ sp_Range _sr = %s; mrb_int _e = _sr.last+(_sr.excl?0:1); for (mrb_int _si = _sr.first; _si < _e; _si++) sp_PolyArray_push(_t%d, sp_box_int(_si)); }\n", ep, t);
            }
          }
          else if (it == TY_INT_ARRAY)
            buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }\n", ep, t);
          else if (it == TY_STR_ARRAY)
            buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }\n", ep, t);
          else if (it == TY_FLOAT_ARRAY)
            buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_sa->data[_si])); }\n", ep, t);
          else if (it == TY_POLY_ARRAY)
            buf_printf(g_pre, "{ sp_PolyArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }\n", ep, t);
          else if (it == TY_POLY)
            /* `*poly`: whether it holds an array is only known at runtime, so
               splice one level if it is an array, drop nil, else push as-is
               (CRuby splat semantics). */
            buf_printf(g_pre, "{ sp_RbVal _sv = %s; if (!sp_poly_nil_p(_sv)) sp_PolyArray_flatten_into_n(_t%d, _sv, 1); }\n", ep, t);
          else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx); buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", t, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
          free(el.p);
        }
else {
          Buf el; memset(&el, 0, sizeof el);
          emit_boxed(c, els[j], &el);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t);
          buf_puts(g_pre, el.p ? el.p : "");
          buf_puts(g_pre, ");\n");
          free(el.p);
        }
      }
      buf_printf(b, "_t%d", t);
      return;
    }
    if (!k) unsupported(c, id, "array literal (element type)");
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, t, k);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    for (int j = 0; j < n; j++) {
      const char *ety = nt_type(nt, els[j]);
      if (ety && sp_streq(ety, "SplatNode")) {
        /* [*range] or [*arr] inside a typed array literal */
        int inner = nt_ref(nt, els[j], "expression");
        TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
        const char *ep = el.p ? el.p : "NULL";
        emit_indent(g_pre, g_indent);
        if (it == TY_RANGE) {
          int rn2 = nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "RangeNode") ? inner : -1;
          int rlo2 = rn2 >= 0 ? nt_ref(nt, rn2, "left") : -1;
          int rexcl2 = rn2 >= 0 ? (int)(nt_int(nt, rn2, "flags", 0) & 4) : 0;
          if (rlo2 >= 0 && comp_ntype(c, rlo2) == TY_STRING) {
            int rhi2 = nt_ref(nt, rn2, "right");
            Buf lo2; memset(&lo2, 0, sizeof lo2); emit_expr(c, rlo2, &lo2);
            Buf hi2; memset(&hi2, 0, sizeof hi2); emit_expr(c, rhi2, &hi2);
            buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_si]); }\n",
                       lo2.p ? lo2.p : "NULL", hi2.p ? hi2.p : "NULL", rexcl2, k, t);
            free(lo2.p); free(hi2.p);
          }
          else {
            buf_printf(g_pre, "{ sp_Range _sr = %s; mrb_int _e = _sr.last+(_sr.excl?0:1); for (mrb_int _si = _sr.first; _si < _e; _si++) sp_%sArray_push(_t%d, _si); }\n", ep, k, t);
          }
        }
        else if (it == TY_INT_ARRAY && sp_streq(k, "Int"))
          buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_sa->start+_si]); }\n", ep, k, t);
        else if (it == TY_STR_ARRAY && sp_streq(k, "Str"))
          buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (mrb_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_si]); }\n", ep, k, t);
        else {
          /* Mismatched or unknown element type: emit_expr fallback */
          buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, t, ep);
        }
        free(el.p);
      }
else {
        Buf el; memset(&el, 0, sizeof el);
        emit_expr(c, els[j], &el);   /* element preludes flow to g_pre first */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sArray_push(_t%d, ", k, t);
        buf_puts(g_pre, el.p ? el.p : "");
        buf_puts(g_pre, ");\n");
        free(el.p);
      }
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode")) {
    TyKind ht = comp_ntype(c, id);
    const char *hn = ty_hash_cname(ht);
    if (!hn) {
      /* Empty `{}` with unknown type: fall back to StrPolyHash */
      int ne2 = 0; nt_arr(nt, id, "elements", &ne2);
      if (ne2 == 0) hn = "StrPoly";
      else unsupported(c, id, "hash literal (key/value type)");
    }
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new();\n", hn, t, hn);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    int sym_poly = (ht == TY_SYM_POLY_HASH || ht == TY_STR_POLY_HASH);
    int poly_poly = (ht == TY_POLY_POLY_HASH);
    for (int j = 0; j < n; j++) {
      int key = nt_ref(nt, els[j], "key");
      int val = nt_ref(nt, els[j], "value");
      Buf kb; memset(&kb, 0, sizeof kb);
      if (poly_poly) emit_boxed(c, key, &kb); else emit_expr(c, key, &kb);
      Buf vb; memset(&vb, 0, sizeof vb);
      if (sym_poly || poly_poly) emit_boxed(c, val, &vb); else emit_expr(c, val, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sHash_set(_t%d, ", hn, t);
      buf_puts(g_pre, kb.p ? kb.p : ""); buf_puts(g_pre, ", ");
      buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ");\n");
      free(kb.p); free(vb.p);
    }
    buf_printf(b, "_t%d", t);
    return;
  }
  if (sp_streq(ty, "IfNode") || sp_streq(ty, "UnlessNode")) {
    /* if/unless as a value: a ternary when both branches are single
       value-expressions. Arm emission (boxing / empty-literal typing) lives
       in emit_ternary_arm below the switch. */
    int pred = nt_ref(nt, id, "predicate");
    int then_b = nt_ref(nt, id, "statements");
    int is_unless = sp_streq(ty, "UnlessNode");
    int sub = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    int tn = 0;
    const int *tb = then_b >= 0 ? nt_arr(nt, then_b, "body", &tn) : NULL;
    int else_stmts = -1;
    if (sub >= 0 && nt_type(nt, sub) && sp_streq(nt_type(nt, sub), "ElseNode"))
      else_stmts = nt_ref(nt, sub, "statements");
    int en = 0;
    const int *eb = else_stmts >= 0 ? nt_arr(nt, else_stmts, "body", &en) : NULL;
    if (tn == 1 && en == 1) {
      TyKind res = comp_ntype(c, id);
      /* A void/nil-typed if (e.g. an arm that is a writer call, doom's
         `self.fullscreen = value if respond_to?(...)`) has no C storage
         type -- emit_ctype would declare `void _tN` -- so hold the result
         boxed; void arms degrade to nil. */
      if (res == TY_VOID || res == TY_NIL) res = TY_POLY;
      /* Emit each arm with a CAPTURED prelude: an arm whose sub-expressions
         hoist statements (a rooted call argument, a constructed receiver, ...)
         cannot ride a flat C ternary -- a shared prelude would evaluate BOTH
         arms eagerly (`File.exist?(f) ? File.read(f) : x` raised on the
         untaken read). Preludeless arms keep the flat form; otherwise the
         arms become real branches with their preludes scoped inside. */
      Buf ta; memset(&ta, 0, sizeof ta);
      Buf te; memset(&te, 0, sizeof te);
      Buf pa; memset(&pa, 0, sizeof pa);
      Buf pe; memset(&pe, 0, sizeof pe);
      Buf *sv_pre = g_pre;
      g_pre = &pa; emit_ternary_arm(c, tb[0], res, &ta);
      g_pre = &pe; emit_ternary_arm(c, eb[0], res, &te);
      g_pre = sv_pre;
      int hoists = (pa.p && pa.p[0]) || (pe.p && pe.p[0]);
      if (!hoists) {
        buf_puts(b, "(");
        if (is_unless) buf_puts(b, "!(");
        emit_cond(c, pred, b);
        if (is_unless) buf_puts(b, ")");
        buf_puts(b, " ? ");
        buf_puts(b, ta.p ? ta.p : "0");
        buf_puts(b, " : ");
        buf_puts(b, te.p ? te.p : "0");
        buf_puts(b, ")");
      }
      else {
        int tr = ++g_tmp;
        buf_puts(b, "({ ");
        emit_ctype(c, res, b);
        /* braced zero: valid for scalars, pointers, AND by-value structs
           (value-class objects, sp_Range); both branches assign over it */
        buf_printf(b, " _t%d = {0}; if (", tr);
        if (is_unless) buf_puts(b, "!(");
        emit_cond(c, pred, b);
        if (is_unless) buf_puts(b, ")");
        buf_printf(b, ") {\n%s _t%d = %s;\n} else {\n%s _t%d = %s;\n} _t%d; })",
                   pa.p ? pa.p : "", tr, ta.p ? ta.p : "0",
                   pe.p ? pe.p : "", tr, te.p ? te.p : "0", tr);
      }
      free(ta.p); free(te.p); free(pa.p); free(pe.p);
      return;
    }
    /* Multi-stmt branches or no-else: emit as if/else block with a result temp.
       Preludes and the if structure go into g_pre; `b` receives only _t<N>. */
    {
      TyKind res = comp_ntype(c, id);
      /* void/nil result: no C storage type (see the ternary form above). */
      if (res == TY_VOID || res == TY_NIL) res = TY_POLY;
      int tr = ++g_tmp;
      /* Declare the temp and default-initialize it. */
      emit_indent(g_pre, g_indent);
      emit_ctype(c, res, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", tr,
                 res == TY_RANGE ? "(sp_Range){0}" : default_value(res));
      /* Emit condition — any prolog from the condition expr also goes to g_pre. */
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "if (");
      if (is_unless) buf_puts(g_pre, "!(");
      emit_cond(c, pred, g_pre);
      if (is_unless) buf_puts(g_pre, ")");
      buf_puts(g_pre, ") {\n");
      /* Then branch: side-effect stmts, then assign last expr to temp. */
      for (int i = 0; i < tn - 1; i++) emit_stmt(c, tb[i], g_pre, g_indent + 2);
      if (tn > 0) {
        int last_then = tb[tn - 1];
        TyKind lt = comp_ntype(c, last_then);
        if (lt == TY_NIL || lt == TY_UNKNOWN || lt == TY_VOID) {
          emit_stmt(c, last_then, g_pre, g_indent + 2);
        }
        else {
          int saved_gi = g_indent; g_indent = g_indent + 2;
          Buf le; memset(&le, 0, sizeof le);
          emit_expr(c, last_then, &le);
          g_indent = saved_gi;
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "_t%d = ", tr);
          if (res == TY_POLY && lt != TY_POLY) {
            Buf bx; memset(&bx, 0, sizeof bx);
            emit_boxed_text(c, lt, le.p ? le.p : default_value(lt), &bx);
            buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
          }
          else buf_puts(g_pre, le.p ? le.p : default_value(res));
          buf_puts(g_pre, ";\n");
          free(le.p);
        }
      }
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "}\n");
      /* Else / elsif branch. */
      if (sub >= 0) {
        const char *sub_ty = nt_type(nt, sub);
        if (sub_ty && sp_streq(sub_ty, "ElseNode")) {
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "else {\n");
          for (int i = 0; i < en - 1; i++) emit_stmt(c, eb[i], g_pre, g_indent + 2);
          if (en > 0) {
            int last_else = eb[en - 1];
            TyKind lt2 = comp_ntype(c, last_else);
            if (lt2 == TY_NIL || lt2 == TY_UNKNOWN || lt2 == TY_VOID) {
              emit_stmt(c, last_else, g_pre, g_indent + 2);
            }
            else {
              int saved_gi2 = g_indent; g_indent = g_indent + 2;
              Buf le2; memset(&le2, 0, sizeof le2);
              emit_expr(c, last_else, &le2);
              g_indent = saved_gi2;
              emit_indent(g_pre, g_indent + 2);
              buf_printf(g_pre, "_t%d = ", tr);
              if (res == TY_POLY && lt2 != TY_POLY) {
                Buf bx2; memset(&bx2, 0, sizeof bx2);
                emit_boxed_text(c, lt2, le2.p ? le2.p : default_value(lt2), &bx2);
                buf_puts(g_pre, bx2.p ? bx2.p : "sp_box_nil()"); free(bx2.p);
              }
              else buf_puts(g_pre, le2.p ? le2.p : default_value(res));
              buf_puts(g_pre, ";\n");
              free(le2.p);
            }
          }
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
        }
        else {
          /* elsif (sub is IfNode) or other subsequent: recurse via emit_expr */
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "else {\n");
          int saved_gi3 = g_indent; g_indent = g_indent + 2;
          Buf sub_e; memset(&sub_e, 0, sizeof sub_e);
          emit_expr(c, sub, &sub_e);
          g_indent = saved_gi3;
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "_t%d = %s;\n", tr, sub_e.p ? sub_e.p : default_value(res));
          free(sub_e.p);
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
        }
      }
      buf_printf(b, "_t%d", tr);
      return;
    }
  }
  if (sp_streq(ty, "CallNode")) { emit_call(c, id, b); return; }
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode")) {
    if (!emit_super_inline(c, id, b, 0, 1)) emit_super(c, id, b);
    return;
  }
  if (sp_streq(ty, "AndNode") || sp_streq(ty, "OrNode")) {
    int is_and = sp_streq(ty, "AndNode");
    int left = nt_ref(nt, id, "left"), right = nt_ref(nt, id, "right");
    TyKind lt = comp_ntype(c, left), res = comp_ntype(c, id);
    if (lt == TY_BOOL && comp_ntype(c, right) == TY_BOOL) {
      buf_puts(b, "(");
      emit_expr(c, left, b);
      buf_puts(b, is_and ? " && " : " || ");
      emit_expr(c, right, b);
      buf_puts(b, ")");
      return;
    }
    /* value form: a || b  ->  truthy(a) ? a : b ;  a && b -> truthy(a) ? b : a.
       Evaluate the left once into a temp; results widen to the unified type. */
    int t = ++g_tmp;
    int lt_falsy_const = (lt == TY_NIL || lt == TY_VOID);  /* a nil/void left has no C-typed value */
    buf_puts(b, "({ ");
    emit_ctype(c, (lt == TY_UNKNOWN || lt_falsy_const) ? res : lt, b);
    buf_printf(b, " _t%d = ", t);
    if (lt_falsy_const) {
      buf_puts(b, "("); emit_expr(c, left, b); buf_puts(b, ", ");
      buf_puts(b, res == TY_POLY ? "sp_box_nil()" : default_value(res == TY_UNKNOWN ? TY_INT : res));
      buf_puts(b, ")");
    }
    /* an unresolved (TY_UNKNOWN) left emits a poly fallback (sp_box_nil); coerce
       it to the unified scalar result so the temp's declared type matches. */
    else if (lt == TY_UNKNOWN && res == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, left, b); buf_puts(b, ")"); }
    else if (lt == TY_UNKNOWN && res == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, left, b); buf_puts(b, ")"); }
    else emit_expr(c, left, b);
    buf_puts(b, "; ");
    if (lt == TY_POLY)      buf_printf(b, "sp_poly_truthy(_t%d)", t);
    else if (lt == TY_BOOL) buf_printf(b, "_t%d", t);
    else if (lt_falsy_const) buf_puts(b, "0");
    else if (lt == TY_INT)  buf_printf(b, "(_t%d != SP_INT_NIL)", t);  /* a nullable int reads falsy at the sentinel; a plain int is always truthy */
    else if (lt == TY_FLOAT) buf_printf(b, "(!sp_float_is_nil(_t%d))", t);
    else if (lt == TY_STRING || ty_is_array(lt) || ty_is_hash(lt) || ty_is_object(lt) ||
             lt == TY_PROC || lt == TY_STRINGIO || lt == TY_STRINGSCANNER || lt == TY_MATCHDATA || lt == TY_EXCEPTION)
      buf_printf(b, "(_t%d != 0)", t);  /* nullable pointer: NULL reads falsy */
    else if (lt == TY_SYMBOL) buf_printf(b, "(_t%d != (sp_sym)-1)", t);  /* nilable symbol sentinel */
    else                    buf_puts(b, "1");  /* concrete value: always truthy */
    buf_puts(b, " ? ");
    /* the "kept-left" arm and the "right" arm, each widened to res */
    #define EMIT_ARM(IS_RIGHT) do { \
      if (IS_RIGHT) { if (res == TY_POLY && comp_ntype(c, right) != TY_POLY) emit_boxed(c, right, b); else emit_expr(c, right, b); } \
      else { if (res == TY_POLY && lt != TY_POLY) { /* box the temp by left type */ \
               if (lt==TY_INT) buf_printf(b, "sp_box_int(_t%d)", t); \
               else if (lt==TY_STRING) buf_printf(b, "sp_box_nullable_str(_t%d)", t); \
               else if (lt==TY_FLOAT) buf_printf(b, "sp_box_float(_t%d)", t); \
               else if (lt==TY_BOOL) buf_printf(b, "sp_box_bool(_t%d)", t); \
               else if (lt==TY_SYMBOL) buf_printf(b, "(_t%d != (sp_sym)-1 ? sp_box_sym(_t%d) : sp_box_nil())", t, t); \
               else if (ty_is_object(lt)) buf_printf(b, "sp_box_nullable_obj((void *)_t%d, %d)", t, ty_object_class(lt)); \
               else buf_printf(b, "_t%d", t); } \
             else buf_printf(b, "_t%d", t); } \
    } while (0)
    if (is_and) { EMIT_ARM(1); buf_puts(b, " : "); EMIT_ARM(0); }
    else        { EMIT_ARM(0); buf_puts(b, " : "); EMIT_ARM(1); }
    #undef EMIT_ARM
    buf_printf(b, "; })");
    return;
  }

  if (sp_streq(ty, "RescueModifierNode")) {
    /* `expr rescue fallback` as an rvalue: evaluate expr under setjmp;
       on exception, evaluate fallback instead. */
    int e  = nt_ref(nt, id, "expression");
    int r  = nt_ref(nt, id, "rescue_expression");
    TyKind rt = comp_ntype(c, id);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s; sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_exc_top++;\n", t, default_value(rt));
    buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    /* expression arm — assign result to temp (skip diverging exprs like raise) */
    TyKind et = e >= 0 ? comp_ntype(c, e) : TY_UNKNOWN;
    int e_diverges = (et == TY_UNKNOWN || et == TY_VOID);
    buf_puts(b, "  ");
    if (e >= 0 && !e_diverges) {
      buf_printf(b, "_t%d = ", t);
      if (rt == TY_POLY && et != TY_POLY) emit_boxed(c, e, b);
      else emit_expr(c, e, b);
      buf_puts(b, ";");
    }
    else if (e >= 0) {
      /* diverging expression like raise: emit as stmt (no assignment) */
      emit_expr(c, e, b); buf_puts(b, ";");
    }
    buf_puts(b, " sp_exc_top--;\n}\nelse {\n  sp_exc_top--;\n  sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n  "
                "if (sp_unwind_kind == SP_UNWIND_NONE) sp_proc_homes_unwind();\n  "
                "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n  ");
    /* rescue arm */
    if (r >= 0) {
      buf_printf(b, "_t%d = ", t);
      if (rt == TY_POLY && comp_ntype(c, r) != TY_POLY) emit_boxed(c, r, b);
      else emit_expr(c, r, b);
      buf_puts(b, ";");
    }
    buf_printf(b, "\n}\n_t%d; })", t);
    return;
  }

  if (sp_streq(ty, "BeginNode")) {
    /* begin/rescue as an rvalue: hoist the block into g_pre so the temp
       is assigned before the surrounding expression reads it. */
    TyKind rt = comp_ntype(c, id);
    /* a void/nil-typed begin (its tail is a writer call or another
       value-less statement) has no C storage type; hold the result boxed
       and let the value-less tail leave it nil. */
    if (rt == TY_VOID || rt == TY_NIL) rt = TY_POLY;
    int t = ++g_tmp;
    char rv[32]; snprintf(rv, sizeof rv, "_t%d", t);
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    if (g_pre) {
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", t, default_value(rt));
      emit_begin(c, id, g_pre, g_indent, rv);
    }
    else {
      /* No prelude available (e.g. inside another expression's prelude):
         fall back to a GCC statement expression. */
      buf_puts(b, "({ ");
      emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;\n", t, default_value(rt));
      emit_begin(c, id, b, 0, rv);
      buf_printf(b, "_t%d; })", t);
      g_result_poly = sp;
      return;
    }
    g_result_poly = sp;
    buf_printf(b, "_t%d", t);
    return;
  }

  /* MultiWriteNode as expression: execute the destructuring (side effect),
     then return the RHS value (Ruby semantics: value of `a, b = arr` is arr). */
  if (sp_streq(ty, "MultiWriteNode")) {
    int value = nt_ref(nt, id, "value");
    /* emit the multi-write as a statement first (for its side effects) */
    emit_stmt(c, id, g_pre, g_indent);
    /* then yield the RHS value as the expression's result */
    emit_expr(c, value, b);
    return;
  }

  /* ivar OP= as expression: emit the mutation then read back the ivar. */
  if (sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int sc = comp_scope_of(c, id)->class_id;
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, nm + 1);
    else
      snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, nm + 1);
    if (g_pre) {
      emit_stmt(c, id, g_pre, g_indent);
      buf_puts(b, ref);
    }
    else {
      TyKind vt = TY_UNKNOWN;
      if (sc >= 0) { int iv = comp_ivar_index(&c->classes[sc], nm); if (iv >= 0) vt = c->classes[sc].ivar_types[iv]; }
      int t = ++g_tmp;
      buf_printf(b, "({ ");
      emit_ctype(c, vt, b);
      buf_printf(b, " _t%d; ", t);
      /* inline the write */
      const char *op = nt_str(nt, id, "binary_operator");
      buf_printf(b, "%s %s= ", ref, op ? op : "+");
      emit_expr(c, nt_ref(nt, id, "value"), b);
      buf_printf(b, "; _t%d = %s; _t%d; })", t, ref, t);
    }
    return;
  }

  unsupported(c, id, "expression");
}

/* ---- output statements (puts/print/p) ---- */

