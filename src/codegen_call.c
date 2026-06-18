#include "codegen_internal.h"

int emit_ctor_yield_inline(Compiler *c, int id, int ci, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || strcmp(nt_type(nt, block), "BlockNode")) return 0;
  int mi = comp_method_in_chain(c, ci, "initialize", NULL);
  if (mi < 0 || !c->scopes[mi].yields) return 0;
  Scope *m = &c->scopes[mi];
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_self = g_self;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  g_yield_block_fallback = saved_block;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  int st = ++g_tmp;
  static char selfbuf[64];
  buf_puts(b, "({\n");
  emit_indent(b, g_indent + 1);
  buf_printf(b, "sp_%s *_t%d = sp_%s_new(", c->classes[ci].name, st, c->classes[ci].name);
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  buf_puts(b, ");\n");
  snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
  g_self = selfbuf;
  int din = g_indent + 1;

  /* declare the initialize body's locals under renamed names */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  /* bind params to the call args (call-site scope: renames off) */
  int args = nt_ref(nt, id, "arguments");
  int argc2 = 0;
  const int *argv2 = args >= 0 ? nt_arr(nt, args, "arguments", &argc2) : NULL;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int sv = g_nren; g_nren = 0;
    emit_arg_or_default(c, m, i, i < argc2 ? argv2[i] : -1, b);
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  int save_ind = g_indent; g_indent = din;
  emit_stmts(c, m->body, b, din);
  g_indent = save_ind;
  emit_indent(b, g_indent + 1);
  buf_printf(b, "_t%d;\n", st);
  emit_indent(b, g_indent); buf_puts(b, "})");

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_self = saved_self;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  return 1;
}

/* `s[i]` on a string with a single non-negative-style int index. Records the
   string receiver and index nodes. Used to fold `s[i] == "c"` into a raw byte
   comparison (no per-access 1-char string allocation). */
static int str_index1(Compiler *c, int node, int *out_recv, int *out_idx) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || (strcmp(nm, "[]") && strcmp(nm, "slice"))) return 0;
  if (nt_ref(nt, node, "block") >= 0) return 0;
  int recv = nt_ref(nt, node, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_STRING) return 0;
  int args = nt_ref(nt, node, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1 || comp_ntype(c, av[0]) != TY_INT) return 0;
  *out_recv = recv;
  *out_idx = av[0];
  return 1;
}

/* A bare single-byte string literal, e.g. `"{"`. */
static int single_byte_lit(Compiler *c, int node, unsigned char *out) {
  const char *ty = nt_type(c->nt, node);
  if (!ty || strcmp(ty, "StringNode")) return 0;
  const char *s = nt_str(c->nt, node, "unescaped");
  if (!s) s = nt_str(c->nt, node, "content");
  if (!s || s[0] == '\0' || s[1] != '\0') return 0;
  *out = (unsigned char)s[0];
  return 1;
}

/* Emit `s[i] == "c"` / `!=` as a raw byte compare when one operand is a
   single-char string index and the other a single-byte literal. The index is
   guarded against negatives (Ruby `s[-1]` indexes from the end) by falling
   back to the general path. Returns 1 if it emitted the optimized form. */
static int emit_strchar_cmp(Compiler *c, int recv, int arg, int eq, Buf *b) {
  int sr, si;
  unsigned char ch;
  int ok = (str_index1(c, recv, &sr, &si) && single_byte_lit(c, arg, &ch)) ||
           (str_index1(c, arg, &sr, &si) && single_byte_lit(c, recv, &ch));
  if (!ok) return 0;
  /* A negative literal index would read out of bounds; only fold when the
     index can't be a negative literal. */
  const char *ity = nt_type(c->nt, si);
  if (ity && !strcmp(ity, "IntegerNode") && nt_int(c->nt, si, "value", 0) < 0) return 0;
  buf_puts(b, "((unsigned char)(");
  emit_expr(c, sr, b);
  buf_puts(b, ")[(mrb_int)(");
  emit_expr(c, si, b);
  buf_printf(b, ")] %s %u)", eq ? "==" : "!=", (unsigned)ch);
  return 1;
}

/* Does `node` (an instance_exec body subtree) contain a break/next that binds
   to the splice itself -- i.e. not consumed by a nested loop or block? */
static int ie_body_has_break_next(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (!strcmp(ty, "BreakNode") || !strcmp(ty, "NextNode")) return 1;
  /* constructs that bind their own break/next */
  if (!strcmp(ty, "WhileNode") || !strcmp(ty, "UntilNode") || !strcmp(ty, "ForNode") ||
      !strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode") || !strcmp(ty, "DefNode") ||
      !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode")) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (ie_body_has_break_next(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) if (ie_body_has_break_next(c, ids[k])) return 1; }
  return 0;
}

/* Emit a valid assignment rvalue for an instance_exec block param the caller
   omitted, given its slot type. default_value() is rvalue-safe for scalars,
   pointers, and the compound-literal value types (Range/Time/Complex/Rational),
   but returns "NULL" for an object type -- correct for a heap object, invalid
   for a value-type object whose C type is a struct (`lv = NULL` won't compile).
   Emit a zero compound literal `(sp_Name){0}` in that case. */
static void emit_ie_param_default(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type) {
      buf_printf(b, "(sp_%s){0}", c->classes[cid].name);
      return;
    }
  }
  buf_puts(b, default_value(t));
}

/* Print "spinel: <file>:<line>: warning: " for node `id` when
   SPINEL_WARN_UNRESOLVED is set, so a call/constant that silently degrades to
   nil/0 (where CRuby would raise or do real work) can be audited. Returns 1
   when a warning line was started -- the caller appends its message + newline.
   Zero runtime/codegen effect: opt-in stderr only. */
static int warn_unresolved_pos(Compiler *c, int id) {
  if (!getenv("SPINEL_WARN_UNRESOLVED")) return 0;
  const NodeTable *nt = c->nt;
  int ln = (int)nt_int(nt, id, "node_line", 0);
  const char *file = nt->source_file;
  if (ln > 0) {
    const char *f = nt_file_path(nt, (int)nt_int(nt, id, "node_file", 0));
    if (f && *f) file = f;
  }
  if (!file || !*file) file = "source.rb";
  fprintf(stderr, "spinel: %s:%d: warning: ", file, ln);
  return 1;
}

void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  /* `require` / `require_relative` is a compile-time directive: top-level ones
     are textually spliced away before codegen, and native libs are provided by
     the runtime. One that still reaches codegen -- indented inside an `if`,
     module, or method body -- is a runtime no-op (it would otherwise be an
     unsupported CallNode). Returns nil in value position via emit_boxed. */
  {
    int rcv = nt_ref(nt, id, "receiver");
    const char *cn = nt_str(nt, id, "name");
    if (rcv < 0 && cn && (!strcmp(cn, "require") || !strcmp(cn, "require_relative"))) {
      buf_puts(b, "0");
      return;
    }
  }
  if (emit_partition_expr(c, id, b)) return;
  if (emit_with_index_expr(c, id, b)) return;
  if (emit_collect_expr(c, id, b)) return;
  if (emit_predicate_expr(c, id, b)) return;
  if (emit_grep_expr(c, id, b)) return;
  if (emit_minmax_by_expr(c, id, b)) return;
  if (emit_flat_map_expr(c, id, b)) return;
  if (emit_filter_map_expr(c, id, b)) return;
  if (emit_poly_uniq_block(c, id, b)) return;
  if (emit_sort_cmp_expr(c, id, b)) return;
  if (emit_minmax_cmp_expr(c, id, b)) return;
  if (emit_step_array_expr(c, id, b)) return;
  if (emit_chunk_while_expr(c, id, b)) return;
  if (emit_slice_when_chunk_inspect_expr(c, id, b)) return;
  if (emit_product_inspect_expr(c, id, b)) return;
  if (emit_bsearch_expr(c, id, b)) return;
  if (emit_sum_block_expr(c, id, b)) return;
  if (emit_transform_hash_expr(c, id, b)) return;
  if (emit_gsub_block_expr(c, id, b)) return;
  if (emit_inject_expr(c, id, b)) return;
  if (emit_reduce_block_expr(c, id, b)) return;
  if (emit_sortby_expr(c, id, b)) return;
  if (emit_each_with_object_expr(c, id, b)) return;
  if (emit_tap_then_expr(c, id, b)) return;
  if (emit_group_by_expr(c, id, b)) return;
  if (emit_inline_expr(c, id, b)) return;  /* value-returning yield method */
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) unsupported(c, id, "call (no name)");

  /* `@nested[i]` inferred as an int array (poly array of int arrays): unbox
     the poly element to sp_IntArray* so the surrounding code stays typed. */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1 &&
      comp_ntype(c, recv) == TY_POLY_ARRAY && comp_ntype(c, id) == TY_INT_ARRAY) {
    buf_puts(b, "((sp_IntArray *)((sp_PolyArray_get(");
    emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    buf_puts(b, ")).v.p))");
    return;
  }

  /* ---- Complex / Rational value types ---- */
  /* Kernel#Complex(re[, im]) */
  if (recv < 0 && !strcmp(name, "Complex") && argc >= 1) {
    buf_puts(b, "((sp_Complex){(mrb_float)(");
    emit_expr(c, argv[0], b);
    buf_puts(b, "), (mrb_float)(");
    if (argc >= 2) emit_expr(c, argv[1], b);
    else buf_puts(b, "0");
    buf_puts(b, ")})");
    return;
  }
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    /* Complex.polar(magnitude, angle) */
    if (rrty && !strcmp(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        !strcmp(nt_str(nt, recv, "name"), "Complex") && !strcmp(name, "polar") && argc >= 1) {
      buf_puts(b, "sp_complex_polar(");
      emit_float_expr(c, argv[0], b);
      buf_puts(b, ", ");
      if (argc >= 2) emit_float_expr(c, argv[1], b);
      else buf_puts(b, "0");
      buf_puts(b, ")");
      return;
    }
    TyKind crt = comp_ntype(c, recv);
    if (crt == TY_COMPLEX) {
      if (!strcmp(name, "real"))      { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").re"); return; }
      if (!strcmp(name, "imaginary") || !strcmp(name, "imag")) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").im"); return; }
      if (!strcmp(name, "conjugate") || !strcmp(name, "conj")) { buf_puts(b, "sp_complex_conjugate("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      if ((!strcmp(name, "+") || !strcmp(name, "*")) && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
        buf_printf(b, "sp_complex_%s(", name[0] == '+' ? "add" : "mul");
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) { buf_puts(b, "sp_complex_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    }
    /* Proc#curry and curry application. */
    if (crt == TY_PROC && !strcmp(name, "curry") && argc == 0) {
      buf_puts(b, "sp_curry_new("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (crt == TY_CURRY && (!strcmp(name, "[]") || !strcmp(name, "call") || !strcmp(name, "()")) && argc == 1) {
      /* The application that reaches the proc's arity realizes the curry to its
         (int) result; earlier applications return another curry. */
      int complete = 0; TyKind cret = TY_UNKNOWN;
      int realize = curry_apply_info(c, id, &complete, &cret) && complete && cret == TY_INT;
      if (realize) buf_puts(b, "sp_curry_to_int(");
      buf_puts(b, "sp_curry_apply("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)(");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
      if (realize) buf_puts(b, ")");
      return;
    }
    if (crt == TY_INT && !strcmp(name, "quo") && argc == 1) {
      buf_puts(b, "sp_rational_new((mrb_int)(");
      emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)(");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
      return;
    }
    if (crt == TY_RATIONAL) {
      if (!strcmp(name, "numerator"))   { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").num"); return; }
      if (!strcmp(name, "denominator")) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").den"); return; }
      if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) { buf_puts(b, "sp_rational_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    }
  }

  /* loop { break val } as expression: emit pre-statement for-loop, result via break var */
  /* Kernel#caller / caller(start) / caller(start, len) -> the current stack
     (method-granularity, via sp_caller_now). Bare `caller` is `caller(1)`. */
  if (recv < 0 && !strcmp(name, "caller") && argc <= 2) {
    buf_puts(b, "sp_caller(");
    if (argc >= 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
    if (argc == 2) { buf_puts(b, ", 1, "); emit_int_expr(c, argv[1], b); }
    else buf_puts(b, ", 0, 0");
    buf_puts(b, ")");
    return;
  }
  if (recv < 0 && !strcmp(name, "loop") && argc == 0) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = infer_type(c, id);
      if (bt != TY_UNKNOWN && bt != TY_NIL) {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", t, (bt == TY_INT) ? "0" : "NULL");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "for (;;) {\n");
        const char *sv_lb = g_loop_break_var;
        char lb_buf[32]; snprintf(lb_buf, sizeof lb_buf, "_t%d", t);
        g_loop_break_var = lb_buf;
        int lbody = nt_ref(nt, blk, "body");
        emit_stmts(c, lbody, g_pre, g_indent + 1);
        g_loop_break_var = sv_lb;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", t);
        return;
      }
    }
  }

  /* catch(:tag) { ... [throw :tag, val] ... } as expression: a setjmp scope
     whose value is the block's last expression, or the thrown value. */
  if (recv < 0 && !strcmp(name, "catch") && argc == 1) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = comp_ntype(c, id);
      if (bt == TY_UNKNOWN || bt == TY_VOID) bt = TY_INT;
      int ptr = proc_slot_is_ptr(bt);
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", t, default_value(bt));
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "sp_catch_tag[sp_catch_top] = ");
      emit_catch_tag(c, argv[0], g_pre);
      buf_puts(g_pre, ";\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_catch_top++;\n");
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "if (setjmp(sp_catch_stack[sp_catch_top-1]) == 0) {\n");
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], g_pre, g_indent + 1);
      if (bn > 0) {
        int last = bb[bn - 1];
        const char *lty = nt_type(nt, last);
        const char *lnm = (lty && !strcmp(lty, "CallNode")) ? nt_str(nt, last, "name") : NULL;
        int last_throw = (lnm && !strcmp(lnm, "throw") && nt_ref(nt, last, "receiver") < 0);
        TyKind lt = comp_ntype(c, last);
        if (last_throw || lt == TY_VOID || lt == TY_UNKNOWN) {
          emit_stmt(c, last, g_pre, g_indent + 1);
        }
        else {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "_t%d = ", t);
          if (bt == TY_POLY && lt != TY_POLY) emit_boxed(c, last, g_pre);
          else emit_expr(c, last, g_pre);
          buf_puts(g_pre, ";\n");
        }
      }
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_catch_top--;\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "else {\n");
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_catch_top--;\n");
      emit_indent(g_pre, g_indent + 1);
      if (ptr) {
        buf_printf(g_pre, "_t%d = (", t); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, ")(uintptr_t)sp_catch_val[sp_catch_top];\n");
      }
      else {
        buf_printf(g_pre, "_t%d = sp_catch_val[sp_catch_top];\n", t);
      }
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", t);
      return;
    }
  }

  /* throw :tag[, val] -> non-local jump to the matching catch scope. */
  if (recv < 0 && !strcmp(name, "throw")) {
    buf_puts(b, "sp_throw(");
    if (argc >= 1) emit_catch_tag(c, argv[0], b);
    else buf_puts(b, "(&(\"\\xff\")[1])");
    buf_puts(b, ", ");
    if (argc >= 2) {
      if (proc_slot_is_ptr(comp_ntype(c, argv[1]))) {
        buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[1], b);
    }
    else buf_puts(b, "0");
    buf_puts(b, ")");
    return;
  }

  /* system(cmd, ...) expr: run and return bool */
  if (recv < 0 && !strcmp(name, "system") && argc >= 1) {
    int ts = ++g_tmp;
    buf_printf(b, "({ const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
    buf_printf(b, ", NULL }; (mrb_bool)sp_system_args(%d, _sys_%d); })", argc, ts);
    return;
  }
  /* trap(...) / Signal.trap(...) expr: return "DEFAULT" */
  {
    int is_trap = (recv < 0 && !strcmp(name, "trap"));
    if (!is_trap && recv >= 0 && !strcmp(name, "trap") && argc >= 1) {
      const char *rty2 = nt_type(nt, recv);
      if (rty2 && (!strcmp(rty2, "ConstantReadNode") || !strcmp(rty2, "ConstantPathNode"))) {
        const char *rn = nt_str(nt, recv, "name");
        if (rn && !strcmp(rn, "Signal")) is_trap = 1;
      }
    }
    if (is_trap && argc >= 1) { emit_str_literal(b, "DEFAULT"); return; }
  }

  /* Fiber[:k] / Fiber.current[:k] -> sp_Fiber_storage_get */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1) {
    int is_fiber_recv = 0;
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && !strcmp(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "Fiber")) is_fiber_recv = 1;
    }
    else if (rty2 && !strcmp(rty2, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && !strcmp(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber"))
          is_fiber_recv = 1;
      }
    }
    if (is_fiber_recv) {
      buf_puts(b, "sp_Fiber_storage_get(sp_fiber_current, ");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
  }
  /* ENV[key] -> getenv */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && !strcmp(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "ENV")) {
        buf_puts(b, "sp_str_dup_external(getenv("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return;
      }
    }
  }
  /* ENV.fetch(key, default) -> getenv with fallback */
  if (recv >= 0 && !strcmp(name, "fetch") && argc >= 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && !strcmp(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "ENV")) {
        int tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = getenv(", tk); emit_expr(c, argv[0], b);
        buf_printf(b, "); const char *_t%d = _t%d ? sp_str_dup_external(_t%d) : ", tv, tk, tk);
        if (argc >= 2) emit_expr(c, argv[1], b);
        else buf_puts(b, "NULL");
        buf_printf(b, "; _t%d; })", tv);
        return;
      }
    }
  }

  /* proc {} / lambda {} / Proc.new {} literal -> a first-class Proc value.
     Guard with is_proc_literal so that any method call that returns TY_PROC
     and happens to have a block (e.g. wrap { }) is not mistaken for a literal. */
  if (comp_ntype(c, id) == TY_PROC && nt_ref(nt, id, "block") >= 0) {
    int _pr_recv = nt_ref(nt, id, "receiver");
    const char *_pr_nm = nt_str(nt, id, "name");
    int is_literal = 0;
    if (_pr_recv < 0 && _pr_nm && (!strcmp(_pr_nm, "proc") || !strcmp(_pr_nm, "lambda")))
      is_literal = 1;
    if (!is_literal && _pr_recv >= 0 && _pr_nm && !strcmp(_pr_nm, "new")) {
      const char *_rty = nt_type(nt, _pr_recv);
      const char *_rnm = (_rty && (!strcmp(_rty, "ConstantReadNode") || !strcmp(_rty, "ConstantPathNode")))
                         ? nt_str(nt, _pr_recv, "name") : NULL;
      if (_rnm && !strcmp(_rnm, "Proc")) is_literal = 1;
    }
    if (is_literal) { emit_proc_literal(c, id, b); return; }
  }

  /* Safe navigation &. : nil receiver -> return nil/0; non-nil -> emit conditional */
  {
    const char *safe_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && safe_op && !strcmp(safe_op, "&.")) {
      TyKind rrt = comp_ntype(c, recv);
      if (rrt == TY_NIL) {
        /* nil&.foo always returns nil */
        TyKind ret = comp_ntype(c, id);
        const char *dv = default_value(ret);
        buf_puts(b, dv ? dv : "0");
        return;
      }
      if (rrt == TY_POLY) {
        /* poly &. method: nil check + dispatch on non-nil string/int/other */
        int tsn = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _sn_%d = ", tsn); emit_expr(c, recv, b); buf_puts(b, "; ");
        buf_printf(b, "_sn_%d.tag == SP_TAG_NIL ? sp_box_nil() : ", tsn);
        /* dispatch the method on the non-nil value */
        if (!strcmp(name, "upcase")) {
          buf_printf(b, "sp_box_str(sp_str_upcase(_sn_%d.v.s))", tsn);
        }
        else if (!strcmp(name, "downcase")) {
          buf_printf(b, "sp_box_str(sp_str_downcase(_sn_%d.v.s))", tsn);
        }
        else if (!strcmp(name, "length") || !strcmp(name, "size")) {
          buf_printf(b, "sp_box_int(sp_poly_length(_sn_%d))", tsn);
        }
        else if (!strcmp(name, "inspect")) {
          buf_printf(b, "sp_box_str(sp_poly_inspect(_sn_%d))", tsn);
        }
        else if (!strcmp(name, "to_s")) {
          buf_printf(b, "sp_box_str(sp_poly_to_s(_sn_%d))", tsn);
        }
        else {
          /* fallback: return the poly value unchanged */
          buf_printf(b, "_sn_%d", tsn);
        }
        buf_puts(b, "; })");
        return;
      }
      /* non-nil typed receiver: for concrete types, dispatch as normal (always non-nil) */
    }
  }

  /* n.times/upto/downto/step { ... } in expression position: run the loop
     (lowered to a statement) and evaluate to the receiver (Ruby returns self) */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && comp_ntype(c, recv) == TY_INT &&
      (!strcmp(name, "times") || !strcmp(name, "upto") ||
       !strcmp(name, "downto") || !strcmp(name, "step"))) {
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    return;
  }
  /* n.times / lo.upto(hi) / hi.downto(lo) without block: produce sp_Range for chaining */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 && comp_ntype(c, recv) == TY_INT &&
      comp_ntype(c, id) == TY_RANGE) {
    if (!strcmp(name, "times")) {
      buf_puts(b, "(sp_Range){ .first = 0, .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 1 }");
      return;
    }
    if (!strcmp(name, "upto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, recv, b);
      buf_puts(b, ", .last = "); emit_expr(c, argv[0], b); buf_puts(b, ", .excl = 0 }");
      return;
    }
    if (!strcmp(name, "downto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, argv[0], b);
      buf_puts(b, ", .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 0 }");
      return;
    }
  }

  /* poly_val.call — the poly value is a proc; unbox then call.
     Only applies when no user-defined class has a `call` method (otherwise
     use the existing poly dispatch switch which handles user-defined call). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      (!strcmp(name, "call") || !strcmp(name, "()"))) {
    int has_user_call = 0;
    for (int _k = 0; _k < c->nclasses && !has_user_call; _k++)
      if (comp_method_in_class(c, _k, name) >= 0) has_user_call = 1;
    if (!has_user_call) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = ", t); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
      /* the poly callable may be a Proc or a bound Method (different ABIs) */
      buf_printf(b, "(_t%d.cls_id == SP_BUILTIN_METHOD ? ((mrb_int (*)(void *", t);
      for (int k = 0; k < argc; k++) buf_puts(b, ", mrb_int");
      buf_printf(b, "))(uintptr_t)((sp_BoundMethod *)_t%d.v.p)->fn)((void *)((sp_BoundMethod *)_t%d.v.p)->self", t, t);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[k], b);
      }
      buf_printf(b, ") : sp_proc_call((sp_Proc *)_t%d.v.p, %d, (mrb_int[16]){", t, argc);
      for (int k = 0; k < argc; k++) {
        if (k) buf_puts(b, ", ");
        if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
      buf_puts(b, "}))");
      return;
    }
  }
  /* method(:sym) / <recv>.method(:sym) -> a bound Method object. */
  if (!strcmp(name, "method") && method_sym_arg(c, id) != NULL) {
    const char *sym = method_sym_arg(c, id);
    int mi = method_obj_target_mi(c, id);
    /* bare method(:sym) on an instance method binds the current self */
    int self_bound = (recv < 0 && mi >= 0 && c->scopes[mi].class_id >= 0 &&
                      !c->scopes[mi].is_cmethod);
    buf_puts(b, "sp_bound_method_new(");
    /* A Method bound to a class/module (Klass.method(:cmeth)) has no instance
       self -- the class value is not a heap pointer, so pass NULL. */
    if (recv >= 0 && comp_ntype(c, recv) == TY_CLASS) buf_puts(b, "NULL");
    else if (recv >= 0) { buf_puts(b, "(void *)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (self_bound) buf_printf(b, "(void *)%s", g_self);
    else buf_puts(b, "NULL");
    buf_puts(b, ", ");
    if (mi >= 0) { buf_puts(b, "(mrb_int)(uintptr_t)&"); emit_method_cname(c, &c->scopes[mi], b); }
    else {
      /* `<typed_array>.method(:op)`: lower through a per-(type, op) adapter
         matching the Method dispatch ABI (optcarrot's
         `add_mappings(.., @ram, @ram.method(:[]=))` shape). */
      TyKind brt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
      const char *bk = ty_is_array(brt) ? array_kind(brt) : NULL;
      const char *bop = NULL;
      if (bk && (brt == TY_INT_ARRAY || brt == TY_STR_ARRAY)) {
        if (!strcmp(sym, "[]")) bop = "get";
        else if (!strcmp(sym, "[]=")) bop = "set";
        else if (!strcmp(sym, "push")) bop = "push";
      }
      if (bop) {
        /* memoized per (kind, op): emit the adapter once */
        static char bam_done[2][3];
        int ki = (brt == TY_INT_ARRAY) ? 0 : 1;
        int oi = bop[0] == 'g' ? 0 : bop[0] == 's' ? 1 : 2;
        if (!bam_done[ki][oi]) {
          bam_done[ki][oi] = 1;
          const char *et = (ki == 0) ? "mrb_int" : "const char *";
          const char *cast = (ki == 0) ? "" : "(mrb_int)(uintptr_t)";
          const char *uncast = (ki == 0) ? "" : "(const char *)(uintptr_t)";
          if (oi == 0) {
            buf_printf(&g_proc_protos, "static mrb_int _bam_%sArray_get(void *a, mrb_int i);\n", bk);
            buf_printf(&g_procs, "static mrb_int _bam_%sArray_get(void *a, mrb_int i) {\n"
                                 "  return %ssp_%sArray_get((sp_%sArray *)a, i);\n}\n", bk, cast, bk, bk);
          }
          else if (oi == 1) {
            buf_printf(&g_proc_protos, "static mrb_int _bam_%sArray_set(void *a, mrb_int i, mrb_int v);\n", bk);
            buf_printf(&g_procs, "static mrb_int _bam_%sArray_set(void *a, mrb_int i, mrb_int v) {\n"
                                 "  sp_%sArray_set((sp_%sArray *)a, i, %sv);\n  return v;\n}\n", bk, bk, bk, uncast);
          }
          else {
            buf_printf(&g_proc_protos, "static mrb_int _bam_%sArray_push(void *a, mrb_int v);\n", bk);
            buf_printf(&g_procs, "static mrb_int _bam_%sArray_push(void *a, mrb_int v) {\n"
                                 "  sp_%sArray_push((sp_%sArray *)a, %sv);\n  return (mrb_int)(uintptr_t)a;\n}\n", bk, bk, bk, uncast);
          }
          (void)et;
        }
        buf_printf(b, "(mrb_int)(uintptr_t)&_bam_%sArray_%s", bk, bop);
      }
      else buf_puts(b, "(mrb_int)0");  /* builtin/Kernel method: no callable address */
    }
    buf_puts(b, ", ");
    emit_str_literal(b, sym);
    buf_puts(b, ")");
    return;
  }
  /* <method>.name -> the stored method name. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && !strcmp(name, "name")) {
    buf_puts(b, "(const char *)("); emit_expr(c, recv, b); buf_puts(b, ")->name");
    return;
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
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && !strcmp(name, "arity")) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (target >= 0 && c->scopes[target].def_node >= 0) {
      int pn = nt_ref(c->nt, c->scopes[target].def_node, "parameters");
      int ok = 1;
      int n_req = 0, n_opt = 0, n_post = 0;
      int has_rest = 0, has_forward = 0, kw_block = 0, has_req_kw = 0;
      if (pn >= 0) {
        nt_arr(c->nt, pn, "requireds", &n_req);
        nt_arr(c->nt, pn, "optionals", &n_opt);
        nt_arr(c->nt, pn, "posts", &n_post);
        int rp = nt_ref(c->nt, pn, "rest");
        if (rp >= 0) {
          const char *rty = nt_type(c->nt, rp);
          if (rty && !strcmp(rty, "RestParameterNode")) has_rest = 1;
          else ok = 0;  /* e.g. ImplicitRestNode: leave unsupported */
        }
        int kn = 0;
        const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
        if (kn > 0) kw_block = 1;
        for (int i = 0; i < kn; i++) {
          const char *kty = nt_type(c->nt, kws[i]);
          if (kty && !strcmp(kty, "RequiredKeywordParameterNode")) has_req_kw = 1;
        }
        int kwrp = nt_ref(c->nt, pn, "keyword_rest");
        if (kwrp >= 0) {
          const char *kty = nt_type(c->nt, kwrp);
          if (kty && !strcmp(kty, "KeywordRestParameterNode")) kw_block = 1;
          else if (kty && !strcmp(kty, "ForwardingParameterNode")) has_forward = 1;
        }
      }
      if (ok) {
        int req = n_req + n_post + (has_req_kw ? 1 : 0);
        int variadic = n_opt > 0 || has_rest || has_forward || (kw_block && !has_req_kw);
        int arity = variadic ? -(req + 1) : req;
        buf_printf(b, "%d", arity);
        return;
      }
    }
  }
  /* <method>.call(args) / [] -> invoke the bound function. A top-level
     method ref calls its function directly; an object-bound Method casts
     fn through the (void *self, mrb_int...) ABI, evaluating recv once. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]"))) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    if (target >= 0 && target_recvless) {
      /* top-level / self method: direct call sp_<name>(args). */
      emit_method_cname(c, &c->scopes[target], b);
      buf_puts(b, "(");
      for (int k = 0; k < argc; k++) { if (k) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
      buf_puts(b, ")");
      return;
    }
    /* object-bound: ((mrb_int(*)(void*, mrb_int...))rc->fn)((void*)rc->self, args) */
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_BoundMethod *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    buf_printf(b, "((mrb_int (*)(void *");
    for (int k = 0; k < argc; k++) buf_puts(b, ", mrb_int");
    buf_printf(b, "))(uintptr_t)_t%d->fn)((void *)_t%d->self", tr, tr);
    for (int k = 0; k < argc; k++) {
      buf_puts(b, ", ");
      if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[k], b);
    }
    buf_printf(b, "); })");
    return;
  }

  /* <proc>.call(args) / .() / [] -> sp_proc_call with the mrb_int[] ABI.
     (A `&block`-param `.call` is handled earlier by the inline path, whose
     receiver name matches g_block_param_name; this is the escaped-value case.) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]"))) {
    TyKind rty = comp_ntype(c, id);          /* the call's result = proc's body return */
    int unbox_ptr = proc_slot_is_ptr(rty);
    int unbox_poly = (rty == TY_POLY);
    int unbox_float = (rty == TY_FLOAT);     /* boxed in the poly slot, read back as float */
    /* Ensure _sp_proc_poly_ret is declared even when triggered from a call site
       (e.g. ivar-stored proc whose proc_ret is TY_UNKNOWN → TY_POLY at analysis). */
    if ((unbox_poly || unbox_float) && !g_needs_proc_poly_retslot) {
      g_needs_proc_poly_retslot = 1;
      buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_ret;\n");
    }
    if (unbox_ptr) { buf_puts(b, "("); emit_ctype(c, rty, b); buf_puts(b, ")(uintptr_t)("); }
    /* poly/float return: proc stores the boxed result in _sp_proc_poly_ret;
       read it back after the call (float unboxes via sp_poly_to_f). */
    if (unbox_poly || unbox_float) buf_puts(b, "((void)");
    buf_puts(b, "sp_proc_call(");
    emit_expr(c, recv, b);
    buf_printf(b, ", %d, ", argc);
    /* A TY_POLY (sp_RbVal) argument doesn't fit the mrb_int slot. When any arg
       is poly, evaluate every argument into a temp first (so a nested poly-proc
       call in an arg expression finishes), then publish the poly ones to the
       _sp_proc_poly_args side-channel and pass a 0 placeholder in the slot --
       re-entrancy-safe, mirroring the _sp_proc_poly_ret return path. */
    int any_poly_arg = 0;
    for (int k = 0; k < argc; k++) if (comp_ntype(c, argv[k]) == TY_POLY) { any_poly_arg = 1; break; }
    if (any_poly_arg) {
      if (!g_needs_proc_poly_argslot) {
        g_needs_proc_poly_argslot = 1;
        buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_args[16];\n");
      }
      int atmp[16]; int nargs = argc < 16 ? argc : 16;  /* proc-call ABI caps args at the mrb_int[16] slot */
      for (int k = 0; k < nargs; k++) {
        TyKind at = comp_ntype(c, argv[k]);
        atmp[k] = ++g_tmp;
        Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, argv[k], &vb);
        emit_indent(g_pre, g_indent);
        if (at == TY_POLY) buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", atmp[k], vb.p ? vb.p : "");
        else if (proc_slot_is_ptr(at)) buf_printf(g_pre, "mrb_int _t%d = (mrb_int)(uintptr_t)(%s);\n", atmp[k], vb.p ? vb.p : "");
        else buf_printf(g_pre, "mrb_int _t%d = %s;\n", atmp[k], vb.p ? vb.p : "");
        free(vb.p);
      }
      for (int k = 0; k < nargs; k++) if (comp_ntype(c, argv[k]) == TY_POLY) {
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "_sp_proc_poly_args[%d] = _t%d;\n", k, atmp[k]);
      }
      buf_puts(b, "(mrb_int[16]){");
      for (int k = 0; k < nargs; k++) {
        if (k) buf_puts(b, ", ");
        if (comp_ntype(c, argv[k]) == TY_POLY) buf_puts(b, "0");
        else buf_printf(b, "_t%d", atmp[k]);
      }
      buf_puts(b, "})");
    }
    else {
      buf_puts(b, "(mrb_int[16]){");
      for (int k = 0; k < argc; k++) {
        if (k) buf_puts(b, ", ");
        /* a heap-pointer argument is laundered into the mrb_int slot */
        if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
      buf_puts(b, "})");
    }
    if (unbox_ptr) buf_puts(b, ")");
    if (unbox_poly) buf_puts(b, ", _sp_proc_poly_ret)");
    if (unbox_float) buf_puts(b, ", sp_poly_to_f(_sp_proc_poly_ret))");
    return;
  }

  /* Proc introspection: arity / lambda? read the sp_Proc metadata directly. */
  /* proc << proc / proc >> proc -> composed Proc. f<<g = f(g(x)) (outer f,
     inner g); f>>g = g(f(x)) (outer g, inner f). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 1 &&
      (!strcmp(name, "<<") || !strcmp(name, ">>")) && comp_ntype(c, argv[0]) == TY_PROC) {
    int fwd = !strcmp(name, ">>");
    buf_puts(b, "sp_proc_compose(");
    if (fwd) emit_expr(c, argv[0], b); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    if (fwd) emit_expr(c, recv, b); else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "arity")) {
    buf_puts(b, "sp_proc_arity("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "lambda?")) {
    buf_puts(b, "sp_proc_lambda_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && !strcmp(name, "parameters")) {
    buf_puts(b, "sp_proc_parameters("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }

  /* Fiber instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_FIBER) {
    if (!strcmp(name, "resume")) {
      buf_puts(b, "sp_Fiber_resume("); emit_expr(c, recv, b);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (comp_ntype(c, argv[k]) == TY_POLY) emit_expr(c, argv[k], b);
        else emit_boxed(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, ", sp_box_nil()");
      buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "alive?")) {
      buf_puts(b, "sp_Fiber_alive("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "transfer")) {
      buf_puts(b, "sp_Fiber_transfer("); emit_expr(c, recv, b);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (comp_ntype(c, argv[k]) == TY_POLY) emit_expr(c, argv[k], b);
        else emit_boxed(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, ", sp_box_nil()");
      buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "value")) {
      /* Fiber#value: resume until fiber finishes and return last yielded value. */
      buf_puts(b, "sp_Fiber_resume("); emit_expr(c, recv, b); buf_puts(b, ", sp_box_nil())");
      return;
    }
  }

  /* Random class methods: Random.rand(n) / Random.rand / Random.bytes(n)
     share a lazily-seeded default instance. */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Random")) {
    if (!strcmp(name, "rand")) {
      if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int(sp_random_default_get(), ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else buf_puts(b, "sp_Random_rand_float(sp_random_default_get())");
      return;
    }
    if (!strcmp(name, "bytes") && argc == 1) {
      buf_puts(b, "sp_Random_bytes(sp_random_default_get(), ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Random instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_RANDOM) {
    if (!strcmp(name, "rand")) {
      if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_puts(b, "sp_Random_rand_float("); emit_expr(c, recv, b); buf_puts(b, ")");
      }
      return;
    }
    if (!strcmp(name, "bytes") && argc == 1) {
      buf_puts(b, "sp_Random_bytes("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* ARGF pseudo-IO methods: read the ARGV files (or stdin) in sequence. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ARGF) {
    if (!strcmp(name, "read")) { buf_puts(b, "sp_argf_read()"); return; }
    if (!strcmp(name, "gets") || !strcmp(name, "readline")) { buf_puts(b, "sp_argf_gets()"); return; }
    if (!strcmp(name, "readlines") || !strcmp(name, "to_a")) { buf_puts(b, "sp_argf_readlines()"); return; }
    if (!strcmp(name, "filename") || !strcmp(name, "path")) { buf_puts(b, "sp_argf_filename()"); return; }
    if (!strcmp(name, "eof?") || !strcmp(name, "eof")) { buf_puts(b, "sp_argf_eof()"); return; }
    if (!strcmp(name, "to_s")) { buf_puts(b, "SPL(\"ARGF\")"); return; }
    if ((!strcmp(name, "each_line") || !strcmp(name, "each_string") || !strcmp(name, "each")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, blk, 0);
      const char *bpn = bp ? rename_local(bp) : NULL;
      int bdy = nt_ref(nt, blk, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      int lt = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "const char *_t%d; while ((_t%d = sp_argf_gets()) != NULL) {", lt, lt);
      if (bpn) buf_printf(b, " const char *lv_%s = _t%d;", bpn, lt);
      for (int k = 0; k < bbn; k++) emit_stmt(c, bbb[k], b, 0);
      buf_puts(b, " } (&sp_argf_obj); })");
      return;
    }
  }

  /* TY_IO (File/IO handle) instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_IO) {
    const char *r = NULL;
    Buf rb = {0};
    emit_expr(c, recv, &rb);
    r = rb.p ? rb.p : "NULL";
    if (!strcmp(name, "read")) {
      if (argc == 0) buf_printf(b, "sp_File_read(%s)", r);
      else { buf_puts(b, "sp_File_read_n("); buf_puts(b, r); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      free(rb.p); return;
    }
    if (!strcmp(name, "gets") || !strcmp(name, "readline")) {
      buf_printf(b, "sp_File_gets(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "readlines")) {
      buf_printf(b, "sp_File_readlines(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "write") || !strcmp(name, "syswrite")) {
      if (argc >= 1) { buf_printf(b, "sp_File_write(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else buf_puts(b, "0");
      free(rb.p); return;
    }
    if (!strcmp(name, "print") || !strcmp(name, "puts")) {
      /* emit as a statement-like expression: print each arg, return nil */
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "({ ");
      for (int k = 0; k < argc; k++) {
        buf_printf(g_pre, "sp_File_write(%s, ", r); emit_expr(c, argv[k], g_pre); buf_puts(g_pre, "); ");
      }
      if (!strcmp(name, "puts")) buf_printf(g_pre, "sp_File_write(%s, \"\\n\"); ", r);
      buf_puts(g_pre, "});\n");
      (void)t;
      buf_puts(b, "((mrb_int)0)");
      free(rb.p); return;
    }
    if (!strcmp(name, "close")) {
      buf_printf(b, "sp_File_close(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "closed?")) {
      buf_printf(b, "sp_File_closed_p(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "eof?") || !strcmp(name, "eof")) {
      buf_printf(b, "sp_File_eof_p(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "path") || !strcmp(name, "to_path")) {
      buf_printf(b, "sp_File_path(%s)", r); free(rb.p); return;
    }
    if (!strcmp(name, "flush") || !strcmp(name, "sync=") || !strcmp(name, "sync")) {
      if (!strcmp(name, "sync")) { buf_printf(b, "((mrb_bool)1)"); } /* always synced */
      else { emit_expr(c, recv, b); }
      free(rb.p); return;
    }
    if ((!strcmp(name, "each_line") || !strcmp(name, "each")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, blk, 0);
      const char *bpn = bp ? rename_local(bp) : NULL;
      int bdy = nt_ref(nt, blk, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      int lt = ++g_tmp, rf = ++g_tmp, buft = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = %s; ", rf, r);
      free(rb.p); r = NULL;
      /* Read each line into a reusable stack buffer instead of allocating a
         GC string per line; the line does not escape the loop body. */
      buf_printf(b, "char _t%d[65536]; const char *_t%d; "
                    "while ((_t%d = sp_File_gets_buf(_t%d, _t%d, sizeof(_t%d))) != NULL) {",
                 buft, lt, lt, rf, buft, buft);
      if (bpn) buf_printf(b, " const char *lv_%s = _t%d;", bpn, lt);
      for (int k = 0; k < bbn; k++) emit_stmt(c, bbb[k], b, 0);
      buf_printf(b, " } (sp_File *)_t%d; })", rf);
      return;
    }
    free(rb.p);
  }

  /* `poly_val << x`: runtime dispatch via sp_poly_shl. For an array receiver it
     appends and returns the (same) array; for an integer it returns the shifted
     value. Use sp_poly_shl's RESULT -- returning the receiver would discard the
     shift (e.g. peek16's `hi << 8`). */
  if (recv >= 0 && !strcmp(name, "<<") && argc == 1 &&
      comp_ntype(c, recv) == TY_POLY) {
    int t = ++g_tmp;
    buf_puts(b, "({ sp_RbVal _t"); buf_printf(b, "%d = ", t); emit_expr(c, recv, b); buf_puts(b, "; ");
    buf_printf(b, "sp_poly_shl(_t%d, ", t);
    emit_boxed(c, argv[0], b);
    buf_puts(b, "); })");
    return;
  }
  /* poly_val >> int / poly_val & int / | / ^ : unbox recv to int, apply op */
  if (recv >= 0 && argc == 1 && comp_ntype(c, recv) == TY_POLY &&
      (!strcmp(name, ">>") || !strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^"))) {
    TyKind at = comp_ntype(c, argv[0]);
    buf_puts(b, "(sp_poly_to_i("); emit_expr(c, recv, b); buf_printf(b, ") %s ", name);
    if (at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  /* `arr << x` / push / append in value position: mutate, then yield the array
     (statement position is handled earlier by emit_array_mutate_stmt). */
  if (recv >= 0 && (!strcmp(name, "<<") || !strcmp(name, "push") || !strcmp(name, "append")) &&
      argc >= 1 && ty_is_array(comp_ntype(c, recv))) {
    TyKind art = comp_ntype(c, recv);
    /* Lift: when a typed-array literal is pushed a heterogeneous element,
       rebuild the receiver as a PolyArray rather than emitting a type mismatch. */
    int needs_lift = 0;
    if (art != TY_POLY_ARRAY && array_kind(art)) {
      TyKind elem_t = ty_array_elem(art);
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ArrayNode")) {
        for (int a = 0; a < argc; a++) {
          TyKind at = comp_ntype(c, argv[a]);
          if (at != TY_UNKNOWN && at != elem_t) { needs_lift = 1; break; }
        }
      }
    }
    if (needs_lift) {
      int en = 0;
      const int *els = nt_arr(nt, recv, "elements", &en);
      int t = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); ", t);
      for (int j = 0; j < en; j++) {
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, els[j], &el);
        buf_printf(b, "sp_PolyArray_push(_t%d, %s); ", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      for (int a = 0; a < argc; a++) {
        buf_printf(b, "sp_PolyArray_push(_t%d, ", t);
        emit_boxed(c, argv[a], b);
        buf_puts(b, "); ");
      }
      buf_printf(b, "_t%d; })", t);
      return;
    }
    const char *k = (art == TY_POLY_ARRAY) ? "Poly" : array_kind(art);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, art, b); buf_printf(b, " _t%d = ", t); emit_expr(c, recv, b); buf_puts(b, "; ");
    TyKind elem = ty_array_elem(art);
    for (int a = 0; a < argc; a++) {
      buf_printf(b, "sp_%sArray_push(_t%d, ", k, t);
      if (art == TY_POLY_ARRAY) emit_boxed(c, argv[a], b);
      else if (comp_ntype(c, argv[a]) == TY_POLY && elem == TY_STRING) {
        /* a poly value (holds a string at runtime) into a str_array: coerce */
        buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else if (comp_ntype(c, argv[a]) == TY_POLY && elem == TY_INT) {
        buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else if (comp_ntype(c, argv[a]) == TY_POLY && elem == TY_FLOAT) {
        buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[a], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[a], b);
      buf_puts(b, "); ");
    }
    buf_printf(b, "_t%d; })", t);
    return;
  }

  /* __dir__ -> the source file's directory (compile-time literal, mirroring
     the legacy generator). */
  if (recv < 0 && !strcmp(name, "__dir__") && argc == 0) {
    const char *sf = nt->source_file;
    char dir[1024];
    if (sf && strrchr(sf, '/')) { size_t n = (size_t)(strrchr(sf, '/') - sf); if (n >= sizeof dir) n = sizeof dir - 1; if (n == 0) { dir[0] = '/'; dir[1] = 0; }
else { memcpy(dir, sf, n); dir[n] = 0; } }
    else { dir[0] = '.'; dir[1] = 0; }
    emit_str_literal(b, dir);
    return;
  }

  /* at_exit { ... } -> register the block as a Proc; main()'s tail runs the
     hooks in reverse order. The registration expression evaluates to the proc. */
  if (recv < 0 && !strcmp(name, "at_exit") && nt_ref(nt, id, "block") >= 0) {
    g_needs_at_exit = 1;
    buf_puts(b, "(sp_at_exit_hooks[sp_at_exit_count++] = ");
    emit_proc_literal(c, id, b);
    buf_puts(b, ")");
    return;
  }

  /* __method__ / __callee__ -> the enclosing method's name as a symbol
     (nil at the top level) */
  if (recv < 0 && argc == 0 &&
      (!strcmp(name, "__method__") || !strcmp(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->name && s->name[0]) buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, s->name));
    else buf_puts(b, "sp_box_nil()");
    return;
  }

  /* block_given? / self.block_given? -> true inside an inlined yielding
     method (we only inline when a block is present) */
  if (!strcmp(name, "block_given?") &&
      (recv < 0 || (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")))) {
    buf_puts(b, g_block_id >= 0 ? "1" : "0");
    return;
  }

  /* Kernel conversions */
  if (recv < 0 && comp_method_index(c, name) < 0) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (!strcmp(name, "Integer") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_i_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "((mrb_int)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (!strcmp(name, "Integer") && ac == 2) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) {
        buf_puts(b, "sp_str_to_i_strict_base("); emit_expr(c, av[0], b);
        buf_puts(b, ", "); emit_expr(c, av[1], b); buf_puts(b, ")");
      }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (!strcmp(name, "Float") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_f_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT) { buf_puts(b, "((mrb_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (!strcmp(name, "String") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { emit_expr(c, av[0], b); }
      else if (at == TY_INT) { buf_puts(b, "sp_int_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "sp_float_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_BOOL) { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, " ? \"true\" : \"false\")"); }
      else if (at == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_poly_to_s(sp_box_nil())"); }  /* nil or unknown */
      return;
    }
    if ((!strcmp(name, "format") || !strcmp(name, "sprintf")) && ac >= 1) {
      /* format(fmt, *args) -> sp_str_format_polyarr(fmt, poly_arr) */
      int tf = ++g_tmp, ta = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "const char *_t%d = ", tf);
      Buf fb; memset(&fb, 0, sizeof fb);
      emit_expr(c, av[0], &fb);
      buf_printf(g_pre, "%s;\n", fb.p ? fb.p : "");
      free(fb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", ta);
      for (int ai = 1; ai < ac; ai++) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", ta);
        Buf ab; memset(&ab, 0, sizeof ab);
        emit_boxed(c, av[ai], &ab);
        buf_printf(g_pre, "%s);\n", ab.p ? ab.p : "sp_box_nil()");
        free(ab.p);
      }
      buf_printf(b, "sp_str_format_polyarr(_t%d, _t%d)", tf, ta);
      return;
    }
    if (!strcmp(name, "rand")) {
      if (ac == 0) { buf_puts(b, "(mrb_float)((double)rand() / (RAND_MAX + 1.0))"); return; }
      TyKind a0t = comp_ntype(c, av[0]);
      if (a0t == TY_RANGE) {
        int tr = ++g_tmp;
        /* is the range a float range? */
        int is_float = 0;
        const char *atype = nt_type(nt, av[0]);
        if (atype && !strcmp(atype, "RangeNode")) {
          int lo = nt_ref(nt, av[0], "left");
          if (lo >= 0 && comp_ntype(c, lo) == TY_FLOAT) is_float = 1;
        }
        buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, av[0], b); buf_puts(b, "; ");
        if (is_float)
          buf_printf(b, "(mrb_float)_t%d.first + sp_Random_rand_float(sp_random_default_get()) * (mrb_float)(_t%d.last - _t%d.first); })", tr, tr, tr);
        else
          buf_printf(b, "_t%d.first + sp_Random_rand_int(sp_random_default_get(), _t%d.last - _t%d.first + 1 - _t%d.excl); })", tr, tr, tr, tr);
        return;
      }
      buf_puts(b, "((mrb_int)("); emit_expr(c, av[0], b); buf_printf(b, " > 0 ? rand() %% (int)"); emit_expr(c, av[0], b); buf_puts(b, " : rand()))");
      return;
    }
    if (!strcmp(name, "srand")) {
      if (ac == 0) { buf_puts(b, "(srand((unsigned)time(NULL)), (mrb_int)0)"); return; }
      buf_puts(b, "({ unsigned _sv = (unsigned)("); emit_expr(c, av[0], b); buf_puts(b, "); srand(_sv); (mrb_int)_sv; })");
      return;
    }
  }

  /* exit / abort as expressions (noreturn, emit as C statement-expression) */
  /* sleep(seconds) / Kernel.sleep(seconds) */
  if (!strcmp(name, "sleep") && argc <= 1 &&
      (recv < 0 ||
       (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Kernel")))) {
    if (argc == 0) { buf_puts(b, "((void)sp_sleep(0.0), (mrb_int)0)"); return; }
    TyKind st = comp_ntype(c, argv[0]);
    buf_puts(b, "((void)sp_sleep(");
    if (st == TY_INT) { buf_puts(b, "(double)"); emit_expr(c, argv[0], b); }
    else if (st == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[0], b);
    buf_puts(b, "), (mrb_int)0)");
    return;
  }
  if (recv < 0 && (!strcmp(name, "exit") || !strcmp(name, "exit!"))) {
    if (argc == 0) { buf_puts(b, "({ exit(0); (mrb_int)0; })"); return; }
    buf_puts(b, "({ exit((int)("); emit_expr(c, argv[0], b); buf_puts(b, ")); (mrb_int)0; })");
    return;
  }
  if (recv < 0 && !strcmp(name, "abort")) {
    if (argc >= 1) {
      TyKind at2 = comp_ntype(c, argv[0]);
      buf_puts(b, "({ fputs(");
      if (at2 == TY_STRING) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ", stderr); fputc('\\n', stderr); exit(1); (mrb_int)0; })");
    }
    else buf_puts(b, "({ exit(1); (mrb_int)0; })");
    return;
  }

  /* raise */
  if (recv < 0 && !strcmp(name, "raise")) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac == 0) {
      if (g_rescue_cls) buf_printf(b, "sp_raise_cls(%s, %s)", g_rescue_cls, g_rescue_msg);
      else buf_puts(b, "sp_raise((&(\"\\xff\")[1]))");
    }
    else if (ac == 1 && nt_type(nt, av[0]) &&
             (!strcmp(nt_type(nt, av[0]), "ConstantReadNode") || !strcmp(nt_type(nt, av[0]), "ConstantPathNode"))) {
      buf_printf(b, "sp_raise_cls(\"%s\", (&(\"\\xff\")[1]))", nt_str(nt, av[0], "name"));
    }
    else if (ac >= 2 && nt_type(nt, av[0]) &&
             (!strcmp(nt_type(nt, av[0]), "ConstantReadNode") || !strcmp(nt_type(nt, av[0]), "ConstantPathNode"))) {
      /* `raise Cls, arg` on a user exception subclass with ivars is
         `raise Cls.new(arg)`: construct the object so its ivar is set (and
         the message comes from the class's initialize/super), then carry it.
         A bare-string/builtin exception keeps the (cls, msg) fast path. */
      const char *cn = nt_str(nt, av[0], "name");
      int xc = cn ? comp_class_index(c, cn) : -1;
      int ic = -1;
      if (xc >= 0 && class_is_exc_subclass(c, xc) && c->classes[xc].nivars > 0)
        ic = comp_method_in_chain(c, xc, "initialize", NULL);
      if (xc >= 0 && ic >= 0 && c->scopes[ic].nparams >= 1) {
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].name);
        /* match the constructor's first-param type, which falls back to poly
           when unknown (same rule emit_class_new uses for the signature). */
        LocalVar *p0 = scope_local(&c->scopes[ic], c->scopes[ic].pnames[0]);
        TyKind pt0 = (p0 && p0->type != TY_UNKNOWN) ? p0->type : TY_POLY;
        if (pt0 == TY_POLY) emit_boxed(c, av[1], b);
        else emit_expr(c, av[1], b);
        buf_puts(b, "))");
      }
      else {
        buf_printf(b, "sp_raise_cls(\"%s\", ", cn);
        emit_expr(c, av[1], b); buf_puts(b, ")");
      }
    }
    else {
      TyKind at = ac > 0 ? comp_ntype(c, av[0]) : TY_UNKNOWN;
      if (at == TY_EXCEPTION)
        { buf_puts(b, "sp_raise_exc((sp_Exception *)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else
        { buf_puts(b, "sp_raise("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
    }
    return;
  }

  /* A specialized rescue var (`rescue MyError => e`, MyError carrying ivars)
     is typed as the subclass object so `e.<ivar>` reads work. Its
     exception-shaped queries still route through the base sp_Exception helpers
     (the struct's leading members mirror sp_Exception); the ivar readers fall
     through to normal object dispatch below. */
  if (recv >= 0 && ty_is_object(comp_ntype(c, recv)) &&
      class_is_exc_subclass(c, ty_object_class(comp_ntype(c, recv)))) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") || !strcmp(name, "to_str")) {
      buf_puts(b, "sp_exc_message((sp_Exception *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
  }

  /* exception object methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_EXCEPTION) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") || !strcmp(name, "to_str")) {
      buf_puts(b, "sp_exc_message("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "full_message")) {
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    if (!strcmp(name, "inspect")) {
      /* #<ClassName: message> */
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    if (!strcmp(name, "class")) {  /* used as .class.to_s / .class.name */
      buf_puts(b, "sp_exc_class_name("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "backtrace")) {
      /* the stack captured at the most recent raise (sp_bt_buf); the substrate
         is live in --debug builds and empty in release, same as Kernel#caller. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_backtrace_captured())");
      return;
    }
    if (argc == 1 && (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
      const char *cn = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")
                       ? nt_str(nt, argv[0], "name") : NULL;
      if (cn) {
        buf_puts(b, "sp_exc_is_a("); emit_expr(c, recv, b);
        buf_printf(b, ", \"%s\")", cn);
        return;
      }
    }
  }

  if (recv < 0 && comp_method_index(c, name) >= 0) { emit_method_call(c, id, b); return; }
  /* bare call to a sibling class method (inside def self.foo, calling bar()) */
  if (recv < 0) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      /* bare `new` inside a class method -> construct the *emitting* class.
         For an inherited cls method specialized into a subclass, the emitting
         class is that subclass, so `new` resolves to the subclass constructor. */
      int new_cls = (g_emitting_class_id >= 0) ? g_emitting_class_id : encl->class_id;
      if (!strcmp(name, "new")) {
        buf_printf(b, "sp_%s_new(", c->classes[new_cls].name);
        int initm = comp_method_in_chain(c, new_cls, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
      int smi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      if (smi >= 0) {
        Scope *ms = &c->scopes[smi];
        emit_method_cname(c, ms, b);
        buf_puts(b, "(");
        emit_args_filled(c, smi, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
  }
  /* bare call to a class method of the enclosing module/class body */
  if (recv < 0 && g_class_body_id >= 0) {
    int smi = comp_cmethod_in_chain(c, g_class_body_id, name, NULL);
    if (smi >= 0) {
      Scope *ms = &c->scopes[smi];
      emit_method_cname(c, ms, b);
      buf_puts(b, "(");
      emit_args_filled(c, smi, nt_ref(nt, id, "arguments"), "", b);
      buf_puts(b, ")");
      return;
    }
  }
  /* bare call to a module_function method made available via top-level include */
  if (recv < 0) {
    int imi = comp_included_method_index(c, name);
    if (imi >= 0) {
      Scope *ms = &c->scopes[imi];
      emit_method_cname(c, ms, b);
      buf_puts(b, "(");
      emit_args_filled(c, imi, nt_ref(nt, id, "arguments"), "", b);
      buf_puts(b, ")");
      return;
    }
  }

  /* X.class.name / .to_s -> identity when .class yields a string;
     for user-object receivers .class now yields TY_CLASS, so wrap with sp_class_to_s. */
  if (recv >= 0 && argc == 0 && (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class")) {
    if (comp_ntype(c, recv) == TY_CLASS) {
      /* When emitting a builtin-reopen method, the inner .class emits
         sp_poly_class_name (returns const char *), not sp_Class — no wrapper needed. */
      int _inner = nt_ref(nt, recv, "receiver");
      int _is_poly_ov = (_inner >= 0 && g_emitting_class_id >= 0 &&
        nt_type(nt, _inner) && !strcmp(nt_type(nt, _inner), "SelfNode") &&
        is_builtin_reopen(c->classes[g_emitting_class_id].name));
      if (_is_poly_ov) {
        emit_expr(c, recv, b);
      }
      else {
        int _clt = ++g_tmp;
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_class_to_s(_cl%d); })", _clt);
      }
    }
    else emit_expr(c, recv, b);
    return;
  }
  /* obj.class.cmeth(...) -> dispatch class method on obj's runtime class
     Emits a cls_id switch: each case calls the right class method. */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class")) {
    int robj = nt_ref(nt, recv, "receiver");
    TyKind rrt = robj >= 0 ? comp_ntype(c, robj) : TY_UNKNOWN;
    if (ty_is_object(rrt)) {
      int cid = ty_object_class(rrt);
      int defmi = comp_cmethod_in_chain(c, cid, name, NULL);
      if (defmi >= 0) {
        /* Count distinct class method impls across the hierarchy */
        int nimpl = 0;
        for (int k = 0; k < c->nclasses; k++) {
          if (!is_descendant(c, k, cid)) continue;
          if (comp_cmethod_in_class(c, k, name) >= 0) nimpl++;
        }
        TyKind cret = (TyKind)c->scopes[defmi].ret;
        /* Stash the receiver object in a temp (referenced in every switch case) */
        char objptr[64];
        const char *rty = nt_type(nt, robj);
        if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, robj, &rb);
          snprintf(objptr, sizeof objptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int ot = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, robj, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rrt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", ot, rb.p ? rb.p : ""); free(rb.p);
          snprintf(objptr, sizeof objptr, "_t%d", ot);
        }
        if (nimpl <= 1) {
          /* single implementation: call directly */
          emit_method_cname(c, &c->scopes[defmi], b);
          buf_puts(b, "(");
          emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
        }
        else {
          /* Check if all descendants agree on return type */
          TyKind unified = cret;
          for (int k2 = 0; k2 < c->nclasses; k2++) {
            if (!is_descendant(c, k2, cid)) continue;
            int kmi2 = comp_cmethod_in_chain(c, k2, name, NULL);
            if (kmi2 < 0) continue;
            TyKind kr = (TyKind)c->scopes[kmi2].ret;
            if (kr != unified) { unified = TY_POLY; break; }
          }
          int rtmp = ++g_tmp;
          buf_puts(b, "({ ");
          if (unified == TY_POLY) buf_puts(b, "sp_RbVal");
          else emit_ctype(c, unified, b);
          buf_printf(b, " _t%d; switch ((%s)->cls_id) {", rtmp, objptr);
          for (int k = 0; k < c->nclasses; k++) {
            if (!is_descendant(c, k, cid)) continue;
            int kmi = comp_cmethod_in_chain(c, k, name, NULL);
            if (kmi < 0) continue;
            TyKind kr = (TyKind)c->scopes[kmi].ret;
            buf_printf(b, " case %d: ", k);
            if (unified == TY_POLY && method_is_void(&c->scopes[kmi])) {
              /* void-return (raises): call then fall through with nil */
              emit_method_cname(c, &c->scopes[kmi], b);
              buf_puts(b, "(");
              emit_args_filled(c, kmi, nt_ref(nt, id, "arguments"), "", b);
              buf_printf(b, "); _t%d = sp_box_nil(); break;", rtmp);
            }
            else {
              buf_printf(b, "_t%d = ", rtmp);
              if (unified == TY_POLY) {
                const char *boxfn = (kr == TY_INT) ? "sp_box_int" :
                                    (kr == TY_STRING) ? "sp_box_str" :
                                    (kr == TY_FLOAT) ? "sp_box_float" :
                                    (kr == TY_BOOL) ? "sp_box_bool" : NULL;
                if (boxfn) buf_printf(b, "%s(", boxfn);
              }
              emit_method_cname(c, &c->scopes[kmi], b);
              buf_puts(b, "(");
              emit_args_filled(c, kmi, nt_ref(nt, id, "arguments"), "", b);
              buf_puts(b, ")");
              if (unified == TY_POLY) {
                const char *boxfn = (kr == TY_INT) ? "sp_box_int" :
                                    (kr == TY_STRING) ? "sp_box_str" :
                                    (kr == TY_FLOAT) ? "sp_box_float" :
                                    (kr == TY_BOOL) ? "sp_box_bool" : NULL;
                if (boxfn) buf_puts(b, ")");
              }
              buf_puts(b, "; break;");
            }
          }
          buf_printf(b, " default: ");
          {
            TyKind dr = (TyKind)c->scopes[defmi].ret;
            if (unified == TY_POLY && method_is_void(&c->scopes[defmi])) {
              emit_method_cname(c, &c->scopes[defmi], b);
              buf_puts(b, "(");
              emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), "", b);
              buf_printf(b, "); _t%d = sp_box_nil(); break;", rtmp);
            }
            else {
              buf_printf(b, "_t%d = ", rtmp);
              if (unified == TY_POLY) {
                const char *boxfn = (dr == TY_INT) ? "sp_box_int" :
                                    (dr == TY_STRING) ? "sp_box_str" :
                                    (dr == TY_FLOAT) ? "sp_box_float" :
                                    (dr == TY_BOOL) ? "sp_box_bool" : NULL;
                if (boxfn) buf_printf(b, "%s(", boxfn);
              }
              emit_method_cname(c, &c->scopes[defmi], b);
              buf_puts(b, "(");
              emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), "", b);
              buf_puts(b, ")");
              if (unified == TY_POLY) {
                const char *boxfn = (dr == TY_INT) ? "sp_box_int" :
                                    (dr == TY_STRING) ? "sp_box_str" :
                                    (dr == TY_FLOAT) ? "sp_box_float" :
                                    (dr == TY_BOOL) ? "sp_box_bool" : NULL;
                if (boxfn) buf_puts(b, ")");
              }
              buf_printf(b, "; break;");
            }
          }
          buf_printf(b, " } _t%d; })", rtmp);
        }
        return;
      }
    }
  }
  /* SomeClass.name / .to_s / .inspect -> the class-name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0) {
    buf_printf(b, "SPL(\"%s\")", nt_str(nt, recv, "name"));
    return;
  }
  /* self.name / self.to_s / self.inspect inside a class method -> class name */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      buf_printf(b, "SPL(\"%s\")", c->classes[encl->class_id].name);
      return;
    }
  }
  /* bare `name` inside a class method body -> the class name */
  if (recv < 0 && !strcmp(name, "name") && argc == 0) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      buf_printf(b, "SPL(\"%s\")", c->classes[encl->class_id].name);
      return;
    }
  }
  /* Regexp.last_match(n) -> nth capture group string, or whole match for n=0 */
  if (recv >= 0 && argc == 1 && !strcmp(name, "last_match") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Regexp")) {
    const char *aty = nt_type(nt, argv[0]);
    if (aty && !strcmp(aty, "IntegerNode")) {
      long long idx = nt_int(nt, argv[0], "value", 0);
      if (idx == 0) { buf_puts(b, "sp_re_match_str"); return; }
      if (idx >= 1 && idx <= 9) { buf_printf(b, "sp_re_captures[%d]", (int)idx); return; }
      buf_puts(b, "NULL");
      return;
    }
    int tv = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = ", tv); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
    buf_printf(b, "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL))",
               tv, tv, tv, tv);
    return;
  }
  /* Regexp.escape / Regexp.quote -> escape special regex characters */
  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "escape") || !strcmp(name, "quote")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Regexp")) {
    TyKind _re_at = comp_ntype(c, argv[0]);
    if (_re_at == TY_POLY) { buf_puts(b, "sp_re_escape(sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
    else { buf_puts(b, "sp_re_escape("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    return;
  }
  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && argc >= 1 && !strcmp(name, "compile") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Regexp")) {
    int tp = ++g_tmp, ts = ++g_tmp;
    int flags = (argc >= 2) ? 1 : 0;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = ", ts); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), %d);\n",
               tp, ts, ts, ts, flags);
    buf_printf(b, "_t%d", tp);
    return;
  }

  /* SomeClass.superclass -> the parent class as sp_Class value */
  if (recv >= 0 && argc == 0 && !strcmp(name, "superclass") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name")) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (ci >= 0) {
      int par = c->classes[ci].parent;
      if (par >= 0) { buf_printf(b, "((sp_Class){%d})", par); return; }
      /* Check if the class has a builtin superclass via AST. */
      int sc_nd = nt_ref(nt, c->classes[ci].def_node, "superclass");
      int bpar = -116;  /* Object */
      if (sc_nd >= 0) {
        const char *sc_ty2 = nt_type(nt, sc_nd);
        const char *sc_nm2 = (sc_ty2 && (!strcmp(sc_ty2, "ConstantReadNode") || !strcmp(sc_ty2, "ConstantPathNode"))) ? nt_str(nt, sc_nd, "name") : NULL;
        if (sc_nm2) { int bid2 = builtin_class_id(sc_nm2); if (bid2 != 0) bpar = bid2; }
      }
      buf_printf(b, "((sp_Class){%d})", bpar);
      return;
    }
  }

  /* x.class -> the class-name string (compile-time for known types) */
  if (recv >= 0 && !strcmp(name, "class") && argc == 0) {
    TyKind rt = comp_ntype(c, recv);
    /* When emitting a scope transplanted from a builtin-reopen class (Object/Array/
       Numeric), self is sp_RbVal even if the nscope-based type says otherwise.
       Override the inferred type to TY_POLY so we get sp_poly_class_name(self).
       Exception: TrueClass/FalseClass use int self; keep TY_BOOL for ternary. */
    if (g_emitting_class_id >= 0 &&
        nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode") &&
        is_builtin_reopen(c->classes[g_emitting_class_id].name)) {
      const char *ecn = c->classes[g_emitting_class_id].name;
      if (strcmp(ecn, "TrueClass") && strcmp(ecn, "FalseClass"))
        rt = TY_POLY;
    }
    const char *cn = NULL;
    if (rt == TY_INT) cn = "Integer";
    else if (rt == TY_FLOAT) cn = "Float";
    else if (rt == TY_STRING) cn = "String";
    else if (rt == TY_SYMBOL) cn = "Symbol";
    else if (rt == TY_RANGE) cn = "Range";
    else if (rt == TY_TIME) cn = "Time";
    else if (rt == TY_IO) cn = "IO";
    else if (rt == TY_ARGF) cn = "ARGF.class";  /* ARGF's singleton class name (CRuby) */
    else if (rt == TY_NIL) cn = "NilClass";
    else if (rt == TY_METHOD) cn = "Method";
    else if (rt == TY_PROC) cn = "Proc";
    else if (ty_is_array(rt)) cn = "Array";
    else if (ty_is_hash(rt)) cn = "Hash";
    else if (ty_is_object(rt)) {
      /* user object: .class returns a TY_CLASS value */
      int _cidx = ty_object_class(rt);
      /* a value-type instance has the class's static cls_id (no NULL case) */
      if (comp_ty_value_obj(c, rt)) { buf_printf(b, "((sp_Class){%d})", _cidx); return; }
      int _tobj = ++g_tmp;
      emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", _tobj);
      Buf _rb; memset(&_rb, 0, sizeof _rb); emit_expr(c, recv, &_rb);
      buf_puts(g_pre, _rb.p ? _rb.p : ""); buf_puts(g_pre, ";\n"); free(_rb.p);
      buf_printf(b, "((sp_Class){_t%d ? _t%d->cls_id : %d})", _tobj, _tobj, _cidx);
      return;
    }
    if (cn) { buf_printf(b, "SPL(\"%s\")", cn); return; }
    if (rt == TY_BOOL) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? SPL(\"TrueClass\") : SPL(\"FalseClass\"))");
      return;
    }
    if (rt == TY_POLY) {
      buf_puts(b, "sp_poly_class_name("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* TY_CLASS method dispatch */
  if (recv >= 0 && comp_ntype(c, recv) == TY_CLASS) {
    int _clt = ++g_tmp;
    if (!strcmp(name, "to_s") || !strcmp(name, "name") || !strcmp(name, "inspect")) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_to_s(_cl%d); })", _clt);
      return;
    }
    if (!strcmp(name, "nil?")) { buf_puts(b, "0"); return; }
    if (!strcmp(name, "class")) {
      buf_printf(b, "({ sp_Class _cl%da = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%da)?SPL(\"Module\"):SPL(\"Class\"); })", _clt);
      return;
    }
    if (!strcmp(name, "superclass") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_superclass(_cl%d); })", _clt);
      return;
    }
    if (!strcmp(name, "ancestors") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_ancestors(_cl%d); })", _clt);
      return;
    }
    /* ClassName.instance_methods(false): compile-time sym array of own methods */
    if (!strcmp(name, "instance_methods") && argc == 1) {
      /* check arg is `false` */
      const char *argt = nt_type(nt, argv[0]);
      int is_false_arg = argt && !strcmp(argt, "FalseNode");
      if (is_false_arg) {
        const char *cn2 = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")
                          ? nt_str(nt, recv, "name") : NULL;
        int ci2 = cn2 ? comp_class_index(c, cn2) : -1;
        if (ci2 >= 0) {
          /* Build a real sp_PolyArray of boxed symbols so the declared
             TY_POLY_ARRAY type matches the runtime value -- chained ops like
             `.map(&:to_s).sort` then iterate it correctly (a boxed SYM_ARRAY
             obj is opaque to the poly-array path and iterated as empty). */
          int ta = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", ta, ta);
          /* user-defined instance methods */
          for (int si = 0; si < c->nscopes; si++) {
            Scope *s = &c->scopes[si];
            if (s->class_id != ci2 || s->is_cmethod) continue;
            if (!s->name || !s->name[0]) continue;
            /* skip shadow methods */
            if (strncmp(s->name, "__prep_", 7) == 0) continue;
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", ta, s->name);
          }
          /* attr_readers */
          ClassInfo *ci3 = &c->classes[ci2];
          for (int ri = 0; ri < ci3->nreaders; ri++)
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", ta, ci3->readers[ri]);
          /* attr_writers */
          for (int wi = 0; wi < ci3->nwriters; wi++)
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s=\"))); ", ta, ci3->writers[wi]);
          buf_printf(b, "sp_box_poly_array(_t%d); })", ta);
          return;
        }
      }
    }
    if ((!strcmp(name, "==" ) || !strcmp(name, "eql?")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; _cl%d.cls_id == _cl%da.cls_id; })", _clt, _clt);
        return;
      }
    }
    if (!strcmp(name, "!=" ) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; _cl%d.cls_id != _cl%da.cls_id; })", _clt, _clt);
        return;
      }
    }
    if ((!strcmp(name, "<") || !strcmp(name, "<=") || !strcmp(name, ">") || !strcmp(name, ">=")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        const char *fn = !strcmp(name, "<") ? "sp_class_lt" :
                         !strcmp(name, "<=") ? "sp_class_le" :
                         !strcmp(name, ">") ? "sp_class_gt" : "sp_class_ge";
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; %s(_cl%d, _cl%da); })", fn, _clt, _clt);
        return;
      }
    }
    /* klass.is_a?/kind_of?(Module|Class|Object|BasicObject) */
    if (argc == 1 && (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
      int exact = !strcmp(name, "instance_of?");
      const char *cn2 = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")
                        ? nt_str(nt, argv[0], "name") : NULL;
      if (cn2) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b); buf_puts(b, "; ");
        /* Module: all class/module values are instances of Module */
        if (!strcmp(cn2, "Module")) {
          if (exact) buf_printf(b, "sp_class_is_module_val(_cl%d); })", _clt);
          else buf_printf(b, "1; })");
        }
        /* Class: user classes only (not modules); builtin Class constant is -109 */
        else if (!strcmp(cn2, "Class")) {
          if (exact)
            buf_printf(b, "(_cl%d.cls_id>=0?!sp_class_is_module_val(_cl%d):(_cl%d.cls_id==-109)); })", _clt, _clt, _clt);
          else
            buf_printf(b, "(_cl%d.cls_id>=0?!sp_class_is_module_val(_cl%d):(_cl%d.cls_id==-109||_cl%d.cls_id==-108)); })", _clt, _clt, _clt, _clt);
        }
        else if (!strcmp(cn2, "Object") || !strcmp(cn2, "BasicObject")) {
          buf_printf(b, "1; })");
        }
        else {
          /* Unknown target: emit 0 with side effect */
          buf_printf(b, "((void)_cl%d, 0); })", _clt);
        }
        return;
      }
    }
  }

  /* freeze / frozen? on an array set/read the struct's frozen flag */
  if (recv >= 0 && argc == 0 && comp_ntype(c, recv) != TY_POLY) {
    TyKind crt = comp_ntype(c, recv);
    const char *ck = (crt == TY_POLY_ARRAY) ? "Poly" : array_kind(crt);
    if (ck && !strcmp(name, "freeze")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", ck, t); emit_expr(c, recv, b);
      buf_printf(b, "; if (_t%d) _t%d->frozen = 1; _t%d; })", t, t, t);
      return;
    }
    if (ck && !strcmp(name, "frozen?")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen != 0)");
      return;
    }
  }

  /* freeze / frozen? on hashes: use the GC-header frozen bit */
  if (recv >= 0 && argc == 0 && ty_is_hash(comp_ntype(c, recv))) {
    if (!strcmp(name, "to_h") && nt_ref(nt, id, "block") < 0) {  /* identity */
      emit_expr(c, recv, b);
      return;
    }
    if (!strcmp(name, "freeze")) {
      buf_puts(b, "sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* frozen? on numeric/symbol scalars: always frozen in Ruby semantics.
     TY_STRING uses a runtime check because dup/String.new produce unfrozen strings. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "frozen?")) {
    TyKind frt = comp_ntype(c, recv);
    if (frt == TY_INT || frt == TY_FLOAT || frt == TY_SYMBOL || frt == TY_BOOL || frt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return;
    }
    if (frt == TY_STRING) {
      buf_puts(b, "sp_str_is_frozen_val("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* TY_STRING freeze: update the variable to the frozen copy and return it */
  if (recv >= 0 && argc == 0 && !strcmp(name, "freeze") && comp_ntype(c, recv) == TY_STRING) {
    const char *rtyf = nt_type(nt, recv);
    int assignable_f = rtyf && (!strcmp(rtyf, "LocalVariableReadNode") || !strcmp(rtyf, "InstanceVariableReadNode"));
    if (assignable_f) {
      buf_puts(b, "({ ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_freeze_val("); emit_expr(c, recv, b); buf_puts(b, "); ");
      emit_expr(c, recv, b); buf_puts(b, "; })");
    }
    else {
      buf_puts(b, "sp_str_freeze_val("); emit_expr(c, recv, b); buf_puts(b, ")");
    }
    return;
  }

  /* identity methods -> the receiver itself */
  if (recv >= 0 &&
      (!strcmp(name, "freeze") || !strcmp(name, "itself") ||
       !strcmp(name, "dup") || !strcmp(name, "clone"))) {
    int args = nt_ref(nt, id, "arguments");
    int argc0 = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc0);
    /* hash, string, and array dup/clone require real copies (they are mutable
       reference types) -- skip the identity shortcut for them so the dedicated
       sp_*_dup paths run. freeze/itself on any value stay identity. */
    TyKind recv_t = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    int is_dup_clone = !strcmp(name, "dup") || !strcmp(name, "clone");
    if (argc0 == 0 && !ty_is_hash(recv_t) &&
        !(is_dup_clone && (recv_t == TY_STRING || ty_is_array(recv_t)))) {
      emit_expr(c, recv, b); return;
    }
    if (argc0 == 0 && recv_t == TY_STRING && is_dup_clone) {
      buf_puts(b, "sp_str_dup_external("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
  }

  /* then / yield_self: pass receiver to block, return block result */
  if (recv >= 0 && (!strcmp(name, "then") || !strcmp(name, "yield_self"))) {
    int blk = nt_ref(nt, id, "block");
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
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
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
        Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, then_bb[then_bn - 1], &vb);
        g_indent = save_ind;
        emit_indent(g_pre, bodyIndent); buf_printf(g_pre, "_t%d = %s;\n", tres, vb.p ? vb.p : "0"); free(vb.p);
        if (use_shadow_th) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
        if (use_shadow_th && tlv0) tlv0->type = tsaved0;
        buf_printf(b, "_t%d", tres);
        return;
      }
    }
  }

  int ie_direct = recv >= 0 && (!strcmp(name, "instance_eval") || !strcmp(name, "instance_exec"));
  int ie_tramp = 0;
  /* receiverless instance_eval/exec inside an instance method: self is the
     receiver. Lower it like a direct call with self aliased into the temp. */
  int ie_self_cls = -1;
  if (!ie_direct && recv < 0) {
    ie_self_cls = ie_implicit_self_class(c, id);
    if (ie_self_cls >= 0) ie_direct = 1;
  }
  if (!ie_direct && recv >= 0 && nt_ref(nt, id, "block") >= 0 && ty_is_object(comp_ntype(c, recv)))
    ie_tramp = comp_trampoline_kind(c, ty_object_class(comp_ntype(c, recv)), name, NULL);
  if (ie_direct || ie_tramp) {
    int blk = nt_ref(nt, id, "block");
    /* `instance_exec(args, &b)` forwarding the enclosing (now-inlined) method's
       block param: the real block is the literal active at the inline splice,
       so resolve the BlockArgumentNode to it (as `inner(&block)` does). */
    if (blk >= 0 && nt_type(nt, blk) && !strcmp(nt_type(nt, blk), "BlockArgumentNode"))
      blk = g_block_id;
    TyKind rtype = ie_self_cls >= 0 ? ty_object(ie_self_cls) : comp_ntype(c, recv);
    if (blk >= 0 && ty_is_object(rtype) &&
        (ie_tramp || comp_method_in_chain(c, ty_object_class(rtype), name, NULL) < 0)) {
      int blk_body = nt_ref(nt, blk, "body");
      int ie_bn = 0; const int *ie_bb = blk_body >= 0 ? nt_arr(nt, blk_body, "body", &ie_bn) : NULL;
      int cls_id = ty_object_class(rtype);
      TyKind body_ty = ie_bn > 0 ? comp_ntype(c, ie_bb[ie_bn - 1]) : TY_NIL;
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
      buf_printf(g_pre, "sp_%s %s_t%d = %s;\n", c->classes[cls_id].name,
                 self_is_val ? "" : "*", tr,
                 rb.p ? rb.p : (self_is_val ? "{0}" : "NULL"));
      free(rb.p);
      if (scalar_res) {
        emit_indent(g_pre, g_indent); emit_ctype(c, body_ty, g_pre);
        buf_printf(g_pre, " _t%d;\n", tres);
      }
      char selfbuf[64]; snprintf(selfbuf, sizeof selfbuf, "_t%d", tr);
      const char *saved_self2 = g_self; g_self = selfbuf;
      const char *saved_deref2 = g_self_deref; g_self_deref = self_is_val ? "." : "->";
      int saved_ie = g_ie_class_id; g_ie_class_id = cls_id;
      /* Bind the block params (interned in the enclosing scope, declared
         there): instance_exec assigns the call-site args; instance_eval
         yields the receiver to each param. */
      {
        int is_exec = ie_tramp ? (ie_tramp == 2) : !strcmp(name, "instance_exec");
        int bp_node = nt_ref(nt, blk, "parameters");
        const char *bpty = bp_node >= 0 ? nt_type(nt, bp_node) : NULL;
        int iargs = nt_ref(nt, id, "arguments");
        int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
        /* a trailing `k: v` call-site hash binds keyword params, not positionals */
        int ie_kwhash = ie_call_kwhash(c, id);
        if (ie_kwhash >= 0) iac -= 1;
        if (bpty && !strcmp(bpty, "NumberedParametersNode")) {
          /* `{ _1.method }`: _1.._N bind like positional block params. */
          int maxn = (int)nt_int(nt, bp_node, "maximum", 0);
          for (int p = 0; p < maxn; p++) {
            char pn[16]; snprintf(pn, sizeof pn, "_%d", p + 1);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "lv_%s = ", rename_local(pn));
            if (is_exec) {
              if (p < iac) emit_expr(c, iav[p], g_pre);
              else { LocalVar *plv = scope_local(comp_scope_of(c, id), pn); emit_ie_param_default(c, plv ? plv->type : TY_POLY, g_pre); }
            }
            else buf_printf(g_pre, "_t%d", tr);
            buf_puts(g_pre, ";\n");
          }
        }
        else {
        int inner = bp_node >= 0 ? nt_ref(nt, bp_node, "parameters") : -1;
        int pnode = inner >= 0 ? inner : bp_node;
        int npar = 0; const int *reqs = pnode >= 0 ? nt_arr(nt, pnode, "requireds", &npar) : NULL;
        /* auto-splat: a single array arg spread across N>=2 params. Evaluate
           the array once, then bind each param to its element. */
        int as_arr = 0; const char *as_kind = NULL;
        /* mixed-args trampoline: bind params to the trampoline body's args. */
        int tramp_argc = ie_tramp ? ie_tramp_effective_argc(c, id) : -1;
        /* A sole splat arg (`instance_exec(*arr) { |a, b| }`) spreads its source
           array across the params, exactly like passing the array directly.
           Unwrap the splat to its operand and let the auto-splat path handle it.
           A splat also spreads across a single param (`instance_exec(*arr) { |a| }`
           binds `a` to `arr[0]`), unlike a directly-passed array (whole array to a
           lone param), so allow `npar >= 1` when explicitly splatted. */
        int arg0 = (iac == 1 && iav) ? iav[0] : -1;
        int is_splat = arg0 >= 0 && nt_type(nt, arg0) && !strcmp(nt_type(nt, arg0), "SplatNode");
        if (is_splat) arg0 = nt_ref(nt, arg0, "expression");
        if (tramp_argc < 0 && is_exec && iac == 1 && (npar >= 2 || (npar >= 1 && is_splat)) && arg0 >= 0) {
          TyKind a0 = comp_ntype(c, arg0);
          if (ty_is_array(a0)) {
            as_kind = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
            as_arr = ++g_tmp;
            /* Evaluate the array into a side buffer so its own prelude flushes
               to g_pre before this declaration line (avoid splicing mid-line). */
            Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, arg0, &ab);
            emit_indent(g_pre, g_indent); emit_ctype(c, a0, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", as_arr, ab.p ? ab.p : "NULL"); free(ab.p);
          }
        }
        for (int p = 0; p < npar; p++) {
          const char *pn = nt_str(nt, reqs[p], "name");
          if (!pn) continue;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "lv_%s = ", rename_local(pn));
          if (as_kind) buf_printf(g_pre, "sp_%sArray_get(_t%d, %d)", as_kind, as_arr, p);
          else if (tramp_argc >= 0) {
            int an = ie_tramp_effective_arg(c, id, p);
            Buf eb; memset(&eb, 0, sizeof eb);
            if (an >= 0) emit_expr(c, an, &eb); else buf_puts(&eb, "0");
            buf_puts(g_pre, eb.p ? eb.p : "0"); free(eb.p);
          }
          else if (is_exec) {
            if (p < iac) emit_expr(c, iav[p], g_pre);
            else { LocalVar *plv = scope_local(comp_scope_of(c, id), pn); emit_ie_param_default(c, plv ? plv->type : TY_POLY, g_pre); }
          }
          else buf_printf(g_pre, "_t%d", tr);  /* instance_eval yields self */
          buf_puts(g_pre, ";\n");
        }
        /* keyword block params: each binds to its matched `k: v` value, or to
           the default expr when an optional keyword is omitted. */
        int nkw = 0; const int *kws = pnode >= 0 ? nt_arr(nt, pnode, "keywords", &nkw) : NULL;
        for (int k = 0; k < nkw; k++) {
          const char *kpn = nt_str(nt, kws[k], "name");
          if (!kpn) continue;
          int vn = ie_kwhash_value(c, ie_kwhash, kpn);
          if (vn < 0) vn = nt_ref(nt, kws[k], "value");  /* omitted optional -> default */
          Buf vb; memset(&vb, 0, sizeof vb);
          if (vn >= 0) emit_expr(c, vn, &vb); else buf_puts(&vb, "0");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "lv_%s = %s;\n", rename_local(kpn), vb.p ? vb.p : "0");
          free(vb.p);
        }
        }
      }
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
        char bvbuf[32];
        if (ie_bn_wrap) {
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "do {\n"); g_indent++;
          if (scalar_res) { snprintf(bvbuf, sizeof bvbuf, "_t%d", tres); g_loop_break_var = bvbuf; g_ie_next_var = bvbuf; }
          else { g_loop_break_var = NULL; g_ie_next_var = NULL; }
        }
        for (int j = 0; j < upto; j++) emit_stmt(c, ie_bb[j], g_pre, g_indent);
        if (!last_as_stmt) {
          Buf vb; memset(&vb, 0, sizeof vb);
          int si2 = g_indent; g_indent = si2;
          emit_expr(c, ie_bb[ie_bn - 1], &vb);
          g_indent = si2;
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
          g_loop_break_var = sv_lb; g_ie_next_var = sv_nx;
          g_indent--; emit_indent(g_pre, g_indent); buf_puts(g_pre, "} while (0);\n");
        }
        g_ie_discard_value = saved_discard;
      }
      g_ie_class_id = saved_ie;
      g_self = saved_self2;
      g_self_deref = saved_deref2;
      if (scalar_res) buf_printf(b, "_t%d", tres);
      else buf_printf(b, "_t%d", tr);  /* statement use: value is the receiver */
      return;
    }
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    /* Inside an instance_eval/exec block, g_ie_class_id is the rebound
       receiver class and takes priority -- the splice may sit inside a class
       method whose own class (g_emitting_class_id) is unrelated to the block's
       self. Otherwise, when emitting a scope transplanted by include
       (g_emitting_class_id is set), dispatch through the emitting class so
       overrides are found correctly. */
    int dispatch_cid = (g_ie_class_id >= 0) ? g_ie_class_id
                     : (g_emitting_class_id >= 0) ? g_emitting_class_id : self->class_id;
    if (dispatch_cid >= 0) {
      if (comp_reader_in_chain(c, dispatch_cid, name, NULL)) {
        const char *rn = comp_resolve_alias(c, dispatch_cid, name);
        buf_printf(b, "%s%siv_%s", g_self, g_self_deref, rn);
        return;
      }
      int mi = comp_method_in_chain(c, dispatch_cid, name, NULL);
      /* Template-method pattern: a base-class method calls an abstract method
         that is implemented only in subclasses. Not found up the chain, but if a
         descendant defines it, emit_dispatch can still resolve it virtually on
         self's runtime class. */
      if (mi < 0 && !self->is_cmethod) {
        for (int k = 0; k < c->nclasses; k++) {
          if (k == dispatch_cid || !is_descendant(c, k, dispatch_cid)) continue;
          if (comp_method_in_chain(c, k, name, NULL) >= 0) { mi = k; break; }
        }
      }
      if (mi >= 0) {
        emit_dispatch(c, dispatch_cid, name, g_self, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return;
      }
      /* Built-in class reopening: implicit self → dispatch as self.builtin_method() */
      if (mi < 0 && !self->is_cmethod) {
        const char *bcn = c->classes[dispatch_cid].name;
        TyKind brt = TY_UNKNOWN;
        if (!strcmp(bcn, "String"))        brt = TY_STRING;
        else if (!strcmp(bcn, "Integer"))  brt = TY_INT;
        else if (!strcmp(bcn, "Float"))    brt = TY_FLOAT;
        else if (!strcmp(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          int args2 = nt_ref(nt, id, "arguments");
          int ac2 = 0; const int *av2 = args2 >= 0 ? nt_arr(nt, args2, "arguments", &ac2) : NULL;
          const char *s = g_self;
          if (brt == TY_STRING) {
            if (!strcmp(name, "upcase"))     { buf_printf(b, "sp_str_upcase(%s)", s); return; }
            if (!strcmp(name, "downcase"))   { buf_printf(b, "sp_str_downcase(%s)", s); return; }
            if (!strcmp(name, "capitalize")) { buf_printf(b, "sp_str_capitalize(%s)", s); return; }
            if (!strcmp(name, "reverse"))    { buf_printf(b, "sp_str_reverse(%s)", s); return; }
            if (!strcmp(name, "strip"))      { buf_printf(b, "sp_str_strip(%s)", s); return; }
            if (!strcmp(name, "lstrip"))     { buf_printf(b, "sp_str_lstrip(%s)", s); return; }
            if (!strcmp(name, "rstrip"))     { buf_printf(b, "sp_str_rstrip(%s)", s); return; }
            if (!strcmp(name, "chomp"))      { buf_printf(b, "sp_str_chomp(%s, NULL)", s); return; }
            if (!strcmp(name, "chop"))       { buf_printf(b, "sp_str_chop(%s)", s); return; }
            if (!strcmp(name, "dup") || !strcmp(name, "clone")) { buf_printf(b, "sp_str_dup(%s)", s); return; }
            if (!strcmp(name, "to_s") || !strcmp(name, "itself")) { buf_puts(b, s); return; }
            if (!strcmp(name, "to_sym"))     { buf_printf(b, "sp_sym_intern(%s)", s); return; }
            if (!strcmp(name, "to_i"))       { buf_printf(b, "sp_str_to_i(%s)", s); return; }
            if (!strcmp(name, "to_f"))       { buf_printf(b, "sp_str_to_f(%s)", s); return; }
            if (!strcmp(name, "length") || !strcmp(name, "size")) { buf_printf(b, "sp_str_length(%s)", s); return; }
            if (!strcmp(name, "bytesize"))   { buf_printf(b, "sp_str_bytesize(%s)", s); return; }
            if (!strcmp(name, "empty?"))     { buf_printf(b, "(!%s || !*%s)", s, s); return; }
            if (!strcmp(name, "inspect"))    { buf_printf(b, "sp_str_inspect(%s)", s); return; }
            if (!strcmp(name, "+") && ac2 == 1) {
              buf_printf(b, "sp_str_concat(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "*") && ac2 == 1) {
              buf_printf(b, "sp_str_repeat(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_INT) {
            if (!strcmp(name, "to_s"))   { buf_printf(b, "sp_int_to_s(%s)", s); return; }
            if (!strcmp(name, "to_f"))   { buf_printf(b, "((double)(%s))", s); return; }
            if (!strcmp(name, "abs"))    { buf_printf(b, "sp_int_abs(%s)", s); return; }
            if (!strcmp(name, "odd?"))   { buf_printf(b, "((%s) %% 2 != 0)", s); return; }
            if (!strcmp(name, "even?"))  { buf_printf(b, "((%s) %% 2 == 0)", s); return; }
            if (!strcmp(name, "zero?"))  { buf_printf(b, "((%s) == 0)", s); return; }
            if (!strcmp(name, "succ") || !strcmp(name, "next")) { buf_printf(b, "sp_int_add(%s, 1LL)", s); return; }
            if (!strcmp(name, "+") && ac2 == 1) {
              buf_printf(b, "sp_int_add(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "-") && ac2 == 1) {
              buf_printf(b, "sp_int_sub(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "*") && ac2 == 1) {
              buf_printf(b, "sp_int_mul(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "/") && ac2 == 1) {
              buf_printf(b, "sp_idiv(%s, ", s); emit_int_divisor(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "%") && ac2 == 1) {
              buf_printf(b, "sp_imod(%s, ", s); emit_int_divisor(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_FLOAT) {
            if (!strcmp(name, "to_s"))   { buf_printf(b, "sp_float_to_s(%s)", s); return; }
            if (!strcmp(name, "to_i"))   { buf_printf(b, "((mrb_int)(%s))", s); return; }
            if (!strcmp(name, "abs"))    { buf_printf(b, "fabs(%s)", s); return; }
            if (!strcmp(name, "floor"))  { buf_printf(b, "((double)((mrb_int)floor(%s)))", s); return; }
            if (!strcmp(name, "ceil"))   { buf_printf(b, "((double)((mrb_int)ceil(%s)))", s); return; }
            if (!strcmp(name, "round"))  { buf_printf(b, "((double)((mrb_int)round(%s)))", s); return; }
            if (!strcmp(name, "+") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " + "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "-") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " - "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "*") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " * "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (!strcmp(name, "/") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " / "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_SYMBOL) {
            if (!strcmp(name, "to_s") || !strcmp(name, "id2name")) {
              buf_printf(b, "sp_sym_to_s(%s)", s); return;
            }
            if (!strcmp(name, "inspect")) { buf_printf(b, "sp_sym_inspect(%s)", s); return; }
            if (!strcmp(name, "to_sym") || !strcmp(name, "itself")) { buf_puts(b, s); return; }
          }
        }
      }
    }
  }

  /* TY_CLASS variable .new -> runtime switch over user classes, returns TY_POLY */
  if (recv >= 0 && !strcmp(name, "new") && comp_ntype(c, recv) == TY_CLASS &&
      nt_type(nt, recv) &&
      strcmp(nt_type(nt, recv), "ConstantReadNode") != 0 &&
      strcmp(nt_type(nt, recv), "ConstantPathNode") != 0 &&
      argc == 0) {
    int kt = ++g_tmp, rt2 = ++g_tmp;
    buf_printf(b, "({ sp_Class _t%d = ", kt); emit_expr(c, recv, b); buf_printf(b, "; ");
    buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); ", rt2);
    buf_printf(b, "switch(_t%d.cls_id){", kt);
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (is_builtin_reopen(c->classes[ci].name)) continue;
      buf_printf(b, "case %d: _t%d=sp_box_obj(sp_%s_new(),%d);break;",
                 ci, rt2, c->classes[ci].name, ci);
    }
    buf_printf(b, "} _t%d; })", rt2);
    return;
  }

  /* self.class.new(args) in a leaf-class instance method -> construct the
     enclosing class statically (no subclass can shadow it at runtime). */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    int cid = self ? self->class_id : -1;
    int has_sub = 0;
    for (int j = 0; cid >= 0 && j < c->nclasses; j++) if (c->classes[j].parent == cid) { has_sub = 1; break; }
    if (cid >= 0 && !has_sub) {
      buf_printf(b, "sp_%s_new(", c->classes[cid].name);
      for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
      buf_puts(b, ")");
      return;
    }
  }

  /* namespaced class M::Sub.new -> check for user-defined `def self.new` first,
     then fall back to sp_<Sub>_new(args) */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantPathNode")) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) {
      if (class_is_exc_subclass(c, ci)) {
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) {
          /* user initialize: call the generated sp_ClassName_new(args) constructor */
          buf_printf(b, "sp_%s_new(", c->classes[ci].name);
          emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
        }
        else {
          /* no user initialize: create directly with first arg as message */
          const char *cn2 = class_ruby_name(c, ci); if (!cn2) cn2 = c->classes[ci].name;
          const char *par = exc_builtin_parent(c, ci);
          buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", ", cn2, par);
          if (argc >= 1) emit_expr(c, argv[0], b);
          else buf_puts(b, "(&(\"\\xff\")[1])");
          buf_puts(b, ")");
        }
        return;
      }
      int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
      if (ucnew >= 0) {
        /* user-defined def self.new: call it as a regular class method */
        int defcls2 = -1; comp_cmethod_in_chain(c, ci, "new", &defcls2);
        buf_printf(b, "sp_%s_s_new(", c->classes[defcls2 >= 0 ? defcls2 : ci].name);
        emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
      if (!c->classes[ci].is_struct) {
        buf_printf(b, "sp_%s_new(", c->classes[ci].name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
    if (cn && is_exc_name(cn)) {
      buf_printf(b, "sp_exc_new(\"%s\", ", cn);
      if (argc >= 1) emit_expr(c, argv[0], b);
      else buf_puts(b, "(&(\"\\xff\")[1])");
      buf_puts(b, ")");
      return;
    }
  }

  /* Class.new(args) -> sp_<Class>_new(args) */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0 && c->classes[ci].is_struct) {
        /* Struct.new members: positional args, or keyword args mapping each
           member by name; each coerced to the member ivar type. */
        ClassInfo *cls = &c->classes[ci];
        int kwh = (argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
        buf_printf(b, "sp_%s_new(", cls->name);
        for (int a = 0; a < cls->nivars; a++) {
          if (a) buf_puts(b, ", ");
          int vnode = -1;
          if (kwh >= 0) vnode = struct_kwarg_value(c, kwh, cls->ivars[a] + 1);
          else if (a < argc) vnode = argv[a];
          if (vnode >= 0) {
            if (cls->ivar_types[a] == TY_POLY && comp_ntype(c, vnode) != TY_POLY) emit_boxed(c, vnode, b);
            else emit_expr(c, vnode, b);
          }
          else buf_puts(b, default_value(cls->ivar_types[a]));
        }
        buf_puts(b, ")");
        return;
      }
      if (ci >= 0) {
        /* user exception subclass: use the generated constructor */
        if (class_is_exc_subclass(c, ci)) {
          int initm = comp_method_in_chain(c, ci, "initialize", NULL);
          if (initm >= 0) {
            /* user initialize: sp_ClassName_new(args) calls initialize which calls super(msg) */
            buf_printf(b, "sp_%s_new(", c->classes[ci].name);
            emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
            buf_puts(b, ")");
          }
          else {
            /* no user initialize: create directly with first arg as message */
            const char *cn2 = class_ruby_name(c, ci); if (!cn2) cn2 = c->classes[ci].name;
            const char *par = exc_builtin_parent(c, ci);
            buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", ", cn2, par);
            if (argc >= 1) emit_expr(c, argv[0], b);
            else buf_puts(b, "(&(\"\\xff\")[1])");
            buf_puts(b, ")");
          }
          return;
        }
        /* yielding initialize: inline its body at the call site (the block
           feeds the yields; the emitted constructor only allocates) */
        if (emit_ctor_yield_inline(c, id, ci, b)) return;
        /* user-defined def self.new takes precedence over the constructor */
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) {
          int defcls2 = -1; comp_cmethod_in_chain(c, ci, "new", &defcls2);
          buf_printf(b, "sp_%s_s_new(", c->classes[defcls2 >= 0 ? defcls2 : ci].name);
          emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
          return;
        }
        buf_printf(b, "sp_%s_new(", c->classes[ci].name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
      const char *cn = nt_str(nt, recv, "name");
      if (cn && is_exc_name(cn)) {
        /* builtin exception class .new(msg) */
        buf_printf(b, "sp_exc_new(\"%s\", ", cn);
        if (argc >= 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ")");
        return;
      }
      if (cn && !strcmp(cn, "String")) {
        /* String.new / String.new(s): always create a mutable heap copy */
        if (argc == 1) { buf_puts(b, "sp_str_dup_external("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else buf_puts(b, "sp_str_dup_external((&(\"\\xff\")[1]))");
        return;
      }
      if (cn && !strcmp(cn, "Object") && argc == 0) {
        buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT)");
        return;
      }
      /* Mutex/Monitor.new: no-op sentinel (single-threaded; synchronize runs block inline) */
      if (cn && (!strcmp(cn, "Mutex") || !strcmp(cn, "Monitor"))) {
        buf_puts(b, "sp_box_nil()"); return;
      }
      if (cn && !strcmp(cn, "StringIO")) {
        if (argc == 0) buf_puts(b, "sp_StringIO_new()");
        else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        return;
      }
      if (cn && (!strcmp(cn, "Fiber") || !strcmp(cn, "Thread")) && nt_ref(nt, id, "block") >= 0) {
        /* Single-threaded: a Thread is modeled as a Fiber that runs to
           completion on #value (no preemption, no Fiber.yield in the body). */
        emit_fiber_new(c, id, b);
        return;
      }
      if (cn && !strcmp(cn, "Random")) {
        buf_puts(b, "sp_Random_new(");
        if (argc >= 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "(mrb_int)time(NULL)");
        buf_puts(b, ")");
        return;
      }
      /* Hash.new { |hash, key| default } -> a StrPolyHash with a default-proc
         function computing the missing-key value. */
      if (cn && !strcmp(cn, "Hash") && nt_ref(nt, id, "block") >= 0) {
        int hblk = nt_ref(nt, id, "block");
        int hbody = nt_ref(nt, hblk, "body");
        const char *hp = block_param_name(c, hblk, 0);
        const char *kp = block_param_name(c, hblk, 1);
        int dn = ++g_proc_counter;
        Buf *pb = &g_procs;
        /* If the default block runs inside an instance/class method, thread
           that receiver in as `self` so the block can call instance methods
           or read ivars (the enclosing self is named `self` with `->`
           deref). Value-typed / top-level enclosers carry no usable pointer
           self, so pass NULL there. (#1379) */
        int dp_self = (g_emitting_class_id >= 0 && g_self && !strcmp(g_self, "self") &&
                       g_self_deref && !strcmp(g_self_deref, "->"));
        const char *dp_cls = dp_self ? c->classes[g_emitting_class_id].name : NULL;
        buf_printf(pb, "static sp_RbVal _sp_hash_dproc_%d(sp_StrPolyHash *_self_h, const char *_key, void *_dproc_self) {\n", dn);
        if (dp_self) buf_printf(pb, "  sp_%s *self = (sp_%s *)_dproc_self; (void)self;\n", dp_cls, dp_cls);
        else buf_puts(pb, "  (void)_dproc_self;\n");
        if (hp) buf_printf(pb, "  sp_StrPolyHash *lv_%s = _self_h; (void)lv_%s;\n", rename_local(hp), rename_local(hp));
        if (kp) buf_printf(pb, "  const char *lv_%s = _key; (void)lv_%s;\n", rename_local(kp), rename_local(kp));
        Buf *sv_pre = g_pre; int sv_ind = g_indent; const char *sv_self = g_self;
        g_pre = pb; g_indent = 1;
        int bn = 0; const int *bb = hbody >= 0 ? nt_arr(nt, hbody, "body", &bn) : NULL;
        for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
        if (bn > 0) {
          int last = bb[bn - 1];
          const char *lty = nt_type(nt, last);
          int is_set = lty && !strcmp(lty, "CallNode") && nt_str(nt, last, "name") &&
                       !strcmp(nt_str(nt, last, "name"), "[]=");
          if (is_set) {
            int srecv = nt_ref(nt, last, "receiver");
            int sargs = nt_ref(nt, last, "arguments");
            int san = 0; const int *sav = sargs >= 0 ? nt_arr(nt, sargs, "arguments", &san) : NULL;
            if (san == 2) {
              int vtmp = ++g_tmp;
              /* Emit the value via a side-buffer so any hoisted prelude (e.g.
                 an instance-method call needing arg temps) lands on its own
                 lines before this assignment, not spliced mid-line. */
              Buf vexpr; memset(&vexpr, 0, sizeof vexpr);
              Buf vpre; memset(&vpre, 0, sizeof vpre);
              Buf *svp = g_pre; g_pre = &vpre;
              emit_expr(c, sav[1], &vexpr);
              g_pre = svp;
              if (vpre.p) buf_puts(pb, vpre.p);
              free(vpre.p);
              emit_indent(pb, 1); emit_ctype(c, comp_ntype(c, sav[1]), pb);
              buf_printf(pb, " _t%d = %s;\n", vtmp, vexpr.p ? vexpr.p : "0");
              free(vexpr.p);
              emit_indent(pb, 1); buf_puts(pb, "sp_StrPolyHash_set(");
              emit_expr(c, srecv, pb); buf_puts(pb, ", ");
              emit_expr(c, sav[0], pb); buf_puts(pb, ", ");
              { Buf bx; memset(&bx, 0, sizeof bx); char vb[32]; snprintf(vb, sizeof vb, "_t%d", vtmp);
                emit_boxed_text(c, comp_ntype(c, sav[1]), vb, &bx); buf_puts(pb, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
              buf_puts(pb, ");\n");
              emit_indent(pb, 1); buf_puts(pb, "return ");
              { Buf bx; memset(&bx, 0, sizeof bx); char vb[32]; snprintf(vb, sizeof vb, "_t%d", vtmp);
                emit_boxed_text(c, comp_ntype(c, sav[1]), vb, &bx); buf_puts(pb, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
              buf_puts(pb, ";\n");
            }
          }
          else {
            Buf vexpr; memset(&vexpr, 0, sizeof vexpr);
            Buf vpre; memset(&vpre, 0, sizeof vpre);
            Buf *svp = g_pre; g_pre = &vpre;
            if (comp_ntype(c, last) == TY_POLY) emit_expr(c, last, &vexpr);
            else emit_boxed(c, last, &vexpr);
            g_pre = svp;
            if (vpre.p) buf_puts(pb, vpre.p);
            free(vpre.p);
            emit_indent(pb, 1);
            buf_printf(pb, "return %s;\n", vexpr.p ? vexpr.p : "sp_box_nil()");
            free(vexpr.p);
          }
        }
        else { emit_indent(pb, 1); buf_puts(pb, "return sp_box_nil();\n"); }
        g_pre = sv_pre; g_indent = sv_ind; g_self = sv_self;
        buf_puts(pb, "}\n");
        if (dp_self) buf_printf(b, "sp_StrPolyHash_new_dproc(_sp_hash_dproc_%d, (void *)self)", dn);
        else buf_printf(b, "sp_StrPolyHash_new_dproc(_sp_hash_dproc_%d, NULL)", dn);
        return;
      }
      if (cn && !strcmp(cn, "StringScanner") && argc == 1) {
        buf_puts(b, "sp_StringScanner_new("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (cn && !strcmp(cn, "Regexp") && argc >= 1) {
        int tp = ++g_tmp, ts = ++g_tmp;
        int flags = (argc >= 2) ? 1 : 0; /* Regexp::IGNORECASE=1 if 2nd arg truthy */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "const char *_t%d = ", ts); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), %d);\n",
                   tp, ts, ts, ts, flags);
        buf_printf(b, "_t%d", tp);
        return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_new()"); return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* Array.new(n) -> PolyArray of n nils */
        int tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        Buf nb; memset(&nb, 0, sizeof nb); emit_int_expr(c, argv[0], &nb);  /* poly size -> int (spinel-dev#24) */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : "0"); buf_puts(g_pre, ";\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", tr);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_box_nil());\n",
                   ti, ti, tn, ti, tr);
        free(nb.p);
        buf_printf(b, "_t%d", tr); return;
      }
      if (cn && !strcmp(cn, "Array") && nt_ref(nt, id, "block") >= 0) {
        /* Array.new(n) { |i| body } / Array.new(0) { body } */
        int blk = nt_ref(nt, id, "block");
        TyKind at = comp_ntype(c, id);
        const char *k = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
        if (!k) k = "Poly";
        int tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        const char *ip = block_param_name(c, blk, 0);
        const char *irn = ip ? rename_local(ip) : NULL;
        Buf nb; memset(&nb, 0, sizeof nb);
        if (argc >= 1) emit_int_expr(c, argv[0], &nb);  /* poly size -> int (spinel-dev#24) */
        emit_indent(g_pre, g_indent);
        if (argc >= 1) { buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : "0"); buf_puts(g_pre, ";\n"); }
        else { buf_printf(g_pre, "mrb_int _t%d = 0;\n", tn); }
        free(nb.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tr, k);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
        g_indent++;
        if (irn) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int lv_%s = _t%d;\n", irn, ti); }
        if (bn > 0 && bb) {
          TyKind elem_t = ty_array_elem(at);
          Buf vb; memset(&vb, 0, sizeof vb);
          for (int bi = 0; bi < bn - 1; bi++) {
            Buf sb; memset(&sb, 0, sizeof sb);
            emit_expr(c, bb[bi], &sb);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, sb.p ? sb.p : ""); buf_puts(g_pre, ";\n"); free(sb.p);
          }
          emit_expr(c, bb[bn - 1], &vb);
          emit_indent(g_pre, g_indent);
          if (!strcmp(k, "Poly")) {
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tr);
            TyKind vt = comp_ntype(c, bb[bn - 1]);
            if (vt == TY_UNKNOWN) {
              /* comp_ntype may return UNKNOWN for e.g. empty [] literals.
                 emit_boxed handles those correctly (no extra g_pre side effects
                 for side-effect-free expressions like empty array literals). */
              Buf bx; memset(&bx, 0, sizeof bx);
              emit_boxed(c, bb[bn - 1], &bx);
              buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()");
              free(bx.p);
            }
            else if (vt != TY_POLY) emit_boxed_text(c, vt, vb.p ? vb.p : "sp_box_nil()", g_pre);
            else buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
            buf_puts(g_pre, ");\n");
          }
          else { buf_printf(g_pre, "sp_%sArray_push(_t%d, %s);\n", k, tr, vb.p ? vb.p : ""); }
          free(vb.p);
        }
        g_indent--;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tr);
        return;
      }
      if (cn && !strcmp(cn, "Array") && argc == 2) {
        /* Array.new(n, v) -> n copies of v */
        TyKind at = comp_ntype(c, id);
        const char *k = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
        if (k) {
          int tn = ++g_tmp, tv = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          /* The size goes into an `mrb_int` temp; coerce a poly size expression
             (e.g. `nrows * ncols` where a factor widened to poly -> sp_poly_mul,
             which returns sp_RbVal) through sp_poly_to_i. spinel-dev#24. */
          Buf nb; memset(&nb, 0, sizeof nb); emit_int_expr(c, argv[0], &nb);
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, argv[1], &vb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = ", tn); buf_puts(g_pre, nb.p ? nb.p : ""); buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          if (at == TY_POLY_ARRAY) {
            buf_printf(g_pre, "sp_RbVal _t%d = ", tv);
            TyKind fvt = comp_ntype(c, argv[1]);
            if (fvt != TY_POLY) emit_boxed_text(c, fvt, vb.p ? vb.p : "sp_box_nil()", g_pre);
            else buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
          }
          else {
            emit_ctype(c, ty_array_elem(at), g_pre);
            buf_printf(g_pre, " _t%d = ", tv); buf_puts(g_pre, vb.p ? vb.p : "");
          }
          buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tr, k);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, _t%d);\n",
                     ti, ti, tn, ti, k, tr, tv);
          free(nb.p); free(vb.p);
          buf_printf(b, "_t%d", tr);
          return;
        }
      }
      if (cn && !strcmp(cn, "Time")) {
        if (argc == 0) { buf_puts(b, "sp_time_now()"); return; }
        buf_printf(b, "sp_time_new(");
        for (int i = 0; i < 6; i++) {
          if (i) buf_puts(b, ", ");
          if (i < argc) emit_expr(c, argv[i], b);
          else buf_puts(b, (i == 1 || i == 2) ? "1" : "0");
        }
        buf_puts(b, ")");
        return;
      }
      /* `.new` on a constant Spinel could not resolve -- not a user class, not a
         builtin/stdlib class handled above (Mutex, Thread, etc. return earlier).
         It is either a genuine undefined constant or a real stdlib class Spinel
         doesn't implement (Pathname, OpenStruct, IPAddr, ...). Either way the
         object can't work, so raise NameError rather than silently degrade to an
         inert 0 whose methods then return nil (a program that used it would
         diverge from CRuby with no signal). Mirrors the value-position read of an
         unresolved constant. The raise expression is int-typed, so an ivar slot
         assigned from it still compiles. */
      if (cn) {
        TyKind nret = comp_ntype(c, id);
        buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), %s)",
                   cn, (is_scalar_ret(nret) && nret != TY_UNKNOWN) ? default_value(nret) : "sp_box_nil()");
        return;
      }
    }
  }

  /* StringIO.open(args) { |io| body } -> run the block with a fresh IO,
     return the block's value, then close. */
  if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "StringIO")) {
    int block = nt_ref(nt, id, "block");
    if (block < 0) {
      /* no block: behaves like StringIO.new */
      if (argc == 0) buf_puts(b, "sp_StringIO_new()");
      else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      return;
    }
    const char *fp = block_param_name(c, block, 0); if (fp) fp = rename_local(fp);
    int bbody = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    TyKind res = comp_ntype(c, id);
    int rv = ++g_tmp;
    int scalar = is_scalar_ret(res);
    buf_puts(b, "({ ");
    if (fp) {
      buf_printf(b, "lv_%s = ", fp);
      if (argc == 0) buf_puts(b, "sp_StringIO_new()");
      else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      buf_puts(b, "; ");
    }
    /* leading statements first, then capture the last as the value */
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
    if (scalar && bn > 0) {
      emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
      if (res == TY_POLY && comp_ntype(c, bb[bn - 1]) != TY_POLY) emit_boxed(c, bb[bn - 1], b);
      else emit_expr(c, bb[bn - 1], b);
      buf_puts(b, "; ");
    }
    else if (bn > 0) emit_stmt(c, bb[bn - 1], b, 0);
    if (fp) buf_printf(b, "sp_StringIO_close(lv_%s); ", fp);
    buf_printf(b, "%s; })", scalar && bn > 0 ? ({ static char _tb[16]; snprintf(_tb, sizeof _tb, "_t%d", rv); _tb; }) : "0");
    return;
  }

  /* GC module methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC")) {
    if (!strcmp(name, "start") && argc == 0) { buf_puts(b, "(sp_gc_collect(), (mrb_int)0)"); return; }
    if (!strcmp(name, "compact") && argc == 0) { buf_puts(b, "(sp_gc_collect(), (mrb_int)0)"); return; }
    if (!strcmp(name, "stat") && argc == 0) { buf_puts(b, "sp_gc_stat()"); return; }
  }

  /* Fiber class methods: Fiber.yield(val) and Fiber.current */
  if (recv_is_const(nt, recv, "Fiber")) {
    if (!strcmp(name, "yield")) {
      if (argc == 0) buf_puts(b, "sp_Fiber_yield(sp_box_nil())");
      else { buf_puts(b, "sp_Fiber_yield("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      return;
    }
    if (!strcmp(name, "current") && argc == 0) {
      buf_puts(b, "sp_fiber_current");
      return;
    }
  }

  /* Process module methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Process")) {
    if (!strcmp(name, "pid") && argc == 0) { buf_puts(b, "((mrb_int)getpid())"); return; }
    if (!strcmp(name, "ppid") && argc == 0) { buf_puts(b, "sp_process_ppid()"); return; }
    if (!strcmp(name, "clock_gettime") && argc >= 1) {
      buf_puts(b, "sp_process_clock_gettime()"); return;
    }
  }

  /* Integer.sqrt(n) -> integer square root (exact, Newton's method) */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Integer") &&
      !strcmp(name, "sqrt") && argc == 1) {
    buf_puts(b, "sp_int_sqrt("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* Math module functions -> C math.h equivalents */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Math")) {
    /* 1-arg functions */
    /* Domain-restricted functions route through sp_math_* wrappers that
       raise Math::DomainError on out-of-domain input (CRuby parity); the
       rest call libc directly (all reals are in domain). */
    const char *cfn = NULL;
    if      (!strcmp(name, "sin"))   cfn = "sin";
    else if (!strcmp(name, "cos"))   cfn = "cos";
    else if (!strcmp(name, "tan"))   cfn = "tan";
    else if (!strcmp(name, "asin"))  cfn = "sp_math_asin";
    else if (!strcmp(name, "acos"))  cfn = "sp_math_acos";
    else if (!strcmp(name, "atan"))  cfn = "atan";
    else if (!strcmp(name, "sinh"))  cfn = "sinh";
    else if (!strcmp(name, "cosh"))  cfn = "cosh";
    else if (!strcmp(name, "tanh"))  cfn = "tanh";
    else if (!strcmp(name, "asinh")) cfn = "asinh";
    else if (!strcmp(name, "acosh")) cfn = "sp_math_acosh";
    else if (!strcmp(name, "atanh")) cfn = "sp_math_atanh";
    else if (!strcmp(name, "exp"))   cfn = "exp";
    else if (!strcmp(name, "sqrt"))  cfn = "sp_math_sqrt";
    else if (!strcmp(name, "cbrt"))  cfn = "cbrt";
    else if (!strcmp(name, "erf"))   cfn = "erf";
    else if (!strcmp(name, "erfc"))  cfn = "erfc";
    if (cfn && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_printf(b, "%s(", cfn);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    /* Math.log(x) or Math.log(x, base) */
    if (!strcmp(name, "log") && (argc == 1 || argc == 2)) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (argc == 1) {
        buf_puts(b, "sp_math_log(");
        if (a0t == TY_INT) buf_puts(b, "(double)");
        emit_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      else {
        TyKind a1t = comp_ntype(c, argv[1]);
        int t0 = ++g_tmp, t1 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = ", t0);
        if (a0t == TY_INT) buf_puts(g_pre, "(double)");
        emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = ", t1);
        if (a1t == TY_INT) buf_puts(g_pre, "(double)");
        emit_expr(c, argv[1], g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(sp_math_log(_t%d) / sp_math_log(_t%d))", t0, t1);
      }
      return;
    }
    /* Math.log2(x), Math.log10(x) */
    if (!strcmp(name, "log2") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_math_log2(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "log10") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_math_log10(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Math.atan2(y, x), Math.hypot(x, y), Math.ldexp(x, e) */
    if ((!strcmp(name, "atan2") || !strcmp(name, "hypot")) && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      TyKind a1t = comp_ntype(c, argv[1]);
      buf_printf(b, "%s(", name);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (a1t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "ldexp") && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "ldexp(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", (int)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
  }

  /* JSON.generate(x) / JSON.dump(x) -> serialize a boxed value */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "JSON") &&
      (!strcmp(name, "generate") || !strcmp(name, "dump")) && argc == 1) {
    TyKind at = comp_ntype(c, argv[0]);
    /* a Struct serializes as a JSON object of its members */
    if (ty_is_object(at) && c->classes[ty_object_class(at)].is_struct) {
      ClassInfo *cls = &c->classes[ty_object_class(at)];
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", cls->name, ts); emit_expr(c, argv[0], b); buf_puts(b, "; sp_sprintf(\"{");
      for (int a = 0; a < cls->nivars; a++) {
        if (a) buf_puts(b, ",");
        buf_printf(b, "\\\"%s\\\":%%s", cls->ivars[a] + 1);  /* member name, sans @ */
      }
      buf_puts(b, "}\"");
      for (int a = 0; a < cls->nivars; a++) {
        TyKind mt = cls->ivar_types[a];
        const char *iv = cls->ivars[a] + 1;  /* field name, sans @ */
        buf_puts(b, ", ");
        if (mt == TY_INT) buf_printf(b, "(_t%d->iv_%s == SP_INT_NIL ? SPL(\"null\") : sp_int_to_s(_t%d->iv_%s))", ts, iv, ts, iv);
        else if (mt == TY_STRING) buf_printf(b, "(_t%d->iv_%s ? sp_json_str(_t%d->iv_%s) : SPL(\"null\"))", ts, iv, ts, iv);
        else if (mt == TY_FLOAT) buf_printf(b, "sp_float_to_s(_t%d->iv_%s)", ts, iv);
        else if (mt == TY_BOOL) buf_printf(b, "(_t%d->iv_%s ? SPL(\"true\") : SPL(\"false\"))", ts, iv);
        else if (mt == TY_POLY) buf_printf(b, "sp_json_val(_t%d->iv_%s)", ts, iv);
        else buf_puts(b, "SPL(\"null\")");
      }
      buf_printf(b, "); })");
      return;
    }
    if (!ty_is_object(at)) {  /* other user objects have no JSON serializer yet */
      buf_puts(b, "sp_json_val("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Dir.exist? / Dir.exists? -> directory test */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
      (!strcmp(name, "exist?") || !strcmp(name, "exists?")) && argc == 1) {
    buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* File class methods -> runtime helpers (the runtime has long carried
     these; only the dispatch was missing). */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "File")) {
    if ((!strcmp(name, "basename") || !strcmp(name, "dirname") || !strcmp(name, "extname")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((!strcmp(name, "read") || !strcmp(name, "binread")) && argc == 1) {
      buf_puts(b, "sp_file_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((!strcmp(name, "write") || !strcmp(name, "binwrite")) && argc == 2) {
      /* runtime write is void; Ruby returns the byte count */
      buf_puts(b, "({ const char *_wp = "); emit_expr(c, argv[0], b);
      buf_puts(b, "; const char *_wd = ");
      if (comp_ntype(c, argv[1]) == TY_POLY) {
        buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_file_write(_wp, _wd); (mrb_int)sp_str_byte_len(_wd); })"); return;
    }
    if ((!strcmp(name, "exist?") || !strcmp(name, "exists?") || !strcmp(name, "readable?")) && argc == 1) {
      buf_puts(b, "sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((!strcmp(name, "directory?") || !strcmp(name, "zero?") || !strcmp(name, "empty?")) && argc == 1) {
      buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "file?") && argc == 1) {
      buf_puts(b, "(!sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ") && sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
    }
    if (!strcmp(name, "delete") && argc == 1) {
      buf_puts(b, "({ sp_file_delete("); emit_expr(c, argv[0], b); buf_puts(b, "); (mrb_int)1; })"); return;
    }
    if (!strcmp(name, "mtime") && argc == 1) {
      buf_puts(b, "sp_file_mtime("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "expand_path") && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_expand_path("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_expr(c, argv[1], b); else buf_puts(b, "(const char *)0");
      buf_puts(b, ")"); return;
    }
    if (!strcmp(name, "join")) {
      buf_printf(b, "sp_file_join((const char*[]){");
      for (int k = 0; k < argc; k++) { if (k) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
      if (argc == 0) buf_puts(b, "(const char*)0");
      buf_printf(b, "}, %d)", argc); return;
    }
    if (!strcmp(name, "readlines") && argc >= 1) {
      /* File.readlines(path) or File.readlines(path, chomp: true) */
      int chomp = 0;
      for (int ki = 1; ki < argc; ki++) {
        const char *kty = nt_type(nt, argv[ki]);
        if (kty && !strcmp(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[ki], "chomp");
          if (cv >= 0 && nt_type(nt, cv) && !strcmp(nt_type(nt, cv), "TrueNode"))
            chomp = 1;
        }
      }
      if (chomp) buf_puts(b, "sp_file_readlines_chomp(");
      else buf_puts(b, "sp_file_readlines(");
      emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* File.open(path, mode) / File.new(path, mode) without block -> TY_IO handle */
    if (!strcmp(name, "open") || !strcmp(name, "new")) {
      int block = nt_ref(nt, id, "block");
      if (block < 0) {
        buf_puts(b, "sp_File_open("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
        if (argc >= 2) emit_expr(c, argv[1], b); else buf_puts(b, "\"r\"");
        buf_puts(b, ")");
        return;
      }
      /* File.open(path, mode) { |f| body } -> open, run body, close, return body value */
      const char *fp = block_param_name(c, block, 0);
      const char *frn = fp ? rename_local(fp) : NULL;
      int bbody = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
      TyKind res = comp_ntype(c, id);
      int rv = ++g_tmp, tf = ++g_tmp;
      int scalar = is_scalar_ret(res) && res != TY_VOID && res != TY_NIL && res != TY_UNKNOWN;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = sp_File_open(", tf); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc >= 2) emit_expr(c, argv[1], b); else buf_puts(b, "\"r\"");
      buf_puts(b, "); ");
      /* Root the handle for the block's duration: the body may allocate and
         trigger a GC, and an unrooted sp_File would be swept (its finalizer
         fcloses mid-iteration, silently truncating each_line loops). */
      buf_printf(b, "SP_GC_ROOT(_t%d); ", tf);
      if (frn) {
        /* Declare the file param as a local: look it up in the enclosing scope.
           Since it's the block param, just use the sp_File * type directly. */
        buf_printf(b, "sp_File *lv_%s = _t%d; ", frn, tf);
      }
      for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
      if (bn > 0) {
        TyKind lty = comp_ntype(c, bb[bn-1]);
        /* Emit last stmt as expression when it has a usable non-void value.
           For void/nil/unknown side-effecting calls (e.g. f.print), emit_stmt
           handles g_pre correctly; then synthesize a return value. */
        int can_expr = (lty != TY_VOID && lty != TY_UNKNOWN &&
                        (lty != TY_NIL ||
                         (nt_type(nt, bb[bn-1]) && !strcmp(nt_type(nt, bb[bn-1]), "NilNode"))));
        if (scalar && can_expr) {
          emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
          if (res == TY_POLY && lty != TY_POLY) emit_boxed(c, bb[bn-1], b);
          else emit_expr(c, bb[bn-1], b);
          buf_puts(b, "; ");
        }
        else {
          emit_stmt(c, bb[bn-1], b, 0);
          if (scalar) {
            emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
            if (res == TY_POLY) buf_puts(b, "sp_box_nil()");
            else buf_puts(b, default_value(res));
            buf_puts(b, "; ");
          }
        }
      }
      buf_printf(b, "sp_File_close(_t%d); ", tf);
      buf_printf(b, "%s; })",
        scalar && bn > 0 ? ({ static char _tb[16]; snprintf(_tb, sizeof _tb, "_t%d", rv); _tb; }) : "0");
      return;
    }
  }
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir")) {
    if (!strcmp(name, "pwd") && argc == 0) { buf_puts(b, "sp_dir_pwd()"); return; }
    if (!strcmp(name, "home") && argc == 0) { buf_puts(b, "sp_dir_home()"); return; }
    if (!strcmp(name, "glob") && argc == 1) {
      buf_puts(b, "sp_dir_glob("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((!strcmp(name, "mkdir") || !strcmp(name, "rmdir") || !strcmp(name, "chdir")) && argc >= 1) {
      buf_printf(b, "sp_dir_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
  }

  /* Time class constructors */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Time")) {
    if ((!strcmp(name, "now") || !strcmp(name, "new")) && argc == 0) { buf_puts(b, "sp_time_now()"); return; }
    if (!strcmp(name, "at") && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      buf_printf(b, "sp_time_at_%s(", at == TY_FLOAT ? "float" : "int");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if ((!strcmp(name, "local") || !strcmp(name, "mktime") ||
         !strcmp(name, "utc") || !strcmp(name, "gm") || !strcmp(name, "new")) && argc >= 1) {
      /* y[,mo,d,h,mi,s] -- missing trailing parts default (mo/d=1, rest 0) */
      int is_utc = (!strcmp(name, "utc") || !strcmp(name, "gm"));
      buf_printf(b, "sp_time_new%s(", is_utc ? "_utc" : "");
      for (int i = 0; i < 6; i++) {
        if (i) buf_puts(b, ", ");
        if (i < argc) emit_expr(c, argv[i], b);
        else buf_puts(b, (i == 1 || i == 2) ? "1" : "0");
      }
      buf_puts(b, ")");
      return;
    }
  }

  /* FFI call dispatch: Module.func(...) where Module declared ffi_func */
  if (recv >= 0) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (rty_ffi && !strcmp(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (rty_ffi && !strcmp(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      int fi = -1;
      for (int ffi_i = 0; ffi_i < c->n_ffi_funcs; ffi_i++)
        if (!strcmp(c->ffi_func_mods[ffi_i], rcmod) && !strcmp(c->ffi_func_names[ffi_i], name)) {
          fi = ffi_i; break;
        }
      if (fi >= 0) {
        const char *ret_spec = c->ffi_func_ret[fi];
        int is_void_ret = !strcmp(ret_spec, "void");
        int is_ptr_ret  = !strcmp(ret_spec, "ptr");
        int is_str_ret  = !strcmp(ret_spec, "str");
        int is_binstr_ret = !strcmp(ret_spec, "binstr");
        int call_argc = c->ffi_func_nargs[fi];
        /* Build the raw C call */
        Buf call_buf; memset(&call_buf, 0, sizeof call_buf);
        buf_puts(&call_buf, c->ffi_func_names[fi]);
        buf_puts(&call_buf, "(");
        for (int ai = 0; ai < call_argc && ai < argc; ai++) {
          if (ai) buf_puts(&call_buf, ", ");
          const char *spec = c->ffi_func_args[fi][ai];
          TyKind at = comp_ntype(c, argv[ai]);
          if (!strcmp(spec, "str")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.s");
            }
            else emit_expr(c, argv[ai], &call_buf);
          }
          else if (!strcmp(spec, "ptr")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "((void *)(");
              emit_expr(c, argv[ai], &call_buf);
              buf_puts(&call_buf, ").v.p)");
            }
            else {
              buf_puts(&call_buf, "((void *)(uintptr_t)(");
              emit_expr(c, argv[ai], &call_buf);
              buf_puts(&call_buf, "))");
            }
          }
          else if (!strcmp(spec, "float") || !strcmp(spec, "double")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")(");
              emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.f)");
            }
            else { buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (!strcmp(spec, "int_array")) {
            /* Hand off element data, never the array struct pointer (which
               would pun the header / read boxed sp_RbVal tags as ints). */
            if (at == TY_INT_ARRAY)        { buf_puts(&call_buf, "sp_IntArray_ffi_data(");   emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_int_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_PolyArray_ffi_int_data((sp_PolyArray *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.p)"); }
            else                           { buf_puts(&call_buf, "((const int64_t *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (!strcmp(spec, "float_array")) {
            if (at == TY_FLOAT_ARRAY)      { buf_puts(&call_buf, "sp_FloatArray_ffi_data(");  emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_float_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_PolyArray_ffi_float_data((sp_PolyArray *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.p)"); }
            else                           { buf_puts(&call_buf, "((const double *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else {
            /* integer-like: int, uint32, size_t, long, etc. */
            if (at == TY_POLY) {
              buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")(");
              emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.i)");
            }
            else if (at == TY_BIGINT) {
              /* An overflow-promoted integer (e.g. a backoff computed by
                 repeated *2) arrives as sp_Bigint*. Narrow it to the C
                 integer the FFI arg expects, not the pointer value. */
              buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")sp_bigint_to_int(");
              emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
            else { buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
        }
        buf_puts(&call_buf, ")");
        if (is_void_ret) {
          buf_puts(b, "("); buf_puts(b, call_buf.p); buf_puts(b, ", (mrb_int)0)");
        }
        else if (is_ptr_ret) {
          /* wrap the foreign void* in a poly sp_RbVal that the GC won't trace */
          buf_printf(b, "sp_box_foreign_ptr((void *)(%s))", call_buf.p);
        }
        else if (is_str_ret) {
          buf_printf(b, "sp_str_dup_external(%s)", call_buf.p);
        }
        else if (is_binstr_ret) {
          /* Binary-safe: build the String from the exact byte count the callee
             published in sp_net_bin_len, not strlen (which truncates at an
             embedded NUL). Sequence the call before reading the length -- C
             leaves argument evaluation order unspecified -- via a temp. */
          int tp = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = %s; "
                        "sp_str_from_bytes(_t%d, (size_t)(sp_net_bin_len < 0 ? 0 : sp_net_bin_len)); })",
                     tp, call_buf.p, tp);
        }
        else {
          /* numeric / bool: cast to mrb_int or mrb_float */
          int ffi_ret_is_float = (!strcmp(ret_spec, "float") || !strcmp(ret_spec, "double"));
          if (ffi_ret_is_float) {
            buf_puts(b, "((mrb_float)("); buf_puts(b, call_buf.p); buf_puts(b, "))");
          }
          else {
            buf_puts(b, "((mrb_int)("); buf_puts(b, call_buf.p); buf_puts(b, "))");
          }
        }
        free(call_buf.p);
        return;
      }
      /* ffi_buffer access: Module.buf_name returns static char* as void* poly */
      {
        int bi = -1;
        for (int fbi = 0; fbi < c->n_ffi_bufs; fbi++)
          if (!strcmp(c->ffi_buf_mods[fbi], rcmod) && !strcmp(c->ffi_buf_names[fbi], name)) {
            bi = fbi; break;
          }
        if (bi >= 0) {
          buf_printf(b, "sp_box_foreign_ptr((void *)sp_ffi_buf_%s_%s)",
                     c->ffi_buf_mods[bi], c->ffi_buf_names[bi]);
          return;
        }
      }
      /* ffi_read_* access: Module.reader_name(buf) */
      {
        int ri = -1;
        for (int fri = 0; fri < c->n_ffi_readers; fri++)
          if (!strcmp(c->ffi_reader_mods[fri], rcmod) && !strcmp(c->ffi_reader_names[fri], name)) {
            ri = fri; break;
          }
        if (ri >= 0 && argc >= 1) {
          const char *kind = c->ffi_reader_kinds[ri];
          int off = c->ffi_reader_offsets[ri];
          const char *ctype = "uint32_t";
          if (kind && !strcmp(kind, "i32")) ctype = "int32_t";
          if (argc >= 1) {
            if (kind && !strcmp(kind, "ptr")) {
              int rt3 = ++g_tmp;
              buf_printf(b, "({ void *_t%d = (*(void **)((char *)(", rt3);
              /* unbox if poly */
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else emit_expr(c, argv[0], b);
              buf_printf(b, " + %d)); sp_box_foreign_ptr(_t%d); })", off, rt3);
            }
            else {
              /* `+ off` must apply to the char* (byte offset), not the typed
                 pointer (which would scale it by sizeof(elem)). */
              buf_printf(b, "((mrb_int)(*(%s *)((char *)(", ctype);
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else emit_expr(c, argv[0], b);
              buf_printf(b, " + %d)))", off);
            }
          }
          return;
        }
      }
    }
  }

  /* Module.field = val  /  Module.field  -> singleton accessor sg_Mod_field */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        ClassInfo *_sgcls = &c->classes[ci];
        int nlen = (int)strlen(name);
        if (nlen > 1 && name[nlen - 1] == '=') {
          /* setter */
          char base[256]; int blen = nlen - 1;
          memcpy(base, name, (size_t)blen); base[blen] = '\0';
          if (comp_is_sg_writer(_sgcls, base)) {
            buf_printf(b, "(sg_%s_%s = ", cn, base);
            if (argc >= 1) {
              TyKind _at = comp_ntype(c, argv[0]);
              emit_box_open(c, _at, b); emit_expr(c, argv[0], b); emit_box_close(c, _at, b);
            }
            else buf_puts(b, "sp_box_nil()");
            buf_puts(b, ")");
            return;
          }
        }
        else {
          /* getter */
          if (comp_is_sg_reader(_sgcls, name)) {
            buf_printf(b, "sg_%s_%s", cn, name);
            return;
          }
        }
      }
    }
  }

  /* self.field = val  /  self.field  inside a class method or module body */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *_sgencl = comp_scope_of(c, id);
    int _sg_cid = (_sgencl && _sgencl->is_cmethod && _sgencl->class_id >= 0)
                  ? _sgencl->class_id : g_class_body_id;
    if (_sg_cid >= 0) {
      ClassInfo *_sgcls = &c->classes[_sg_cid];
      const char *_sgcn = _sgcls->name;
      int _nlen = (int)strlen(name);
      if (_nlen > 1 && name[_nlen - 1] == '=') {
        char _base[256]; int _blen = _nlen - 1;
        memcpy(_base, name, (size_t)_blen); _base[_blen] = '\0';
        if (comp_is_sg_writer(_sgcls, _base)) {
          buf_printf(b, "(sg_%s_%s = ", _sgcn, _base);
          if (argc >= 1) {
            TyKind _at = comp_ntype(c, argv[0]);
            emit_box_open(c, _at, b); emit_expr(c, argv[0], b); emit_box_close(c, _at, b);
          }
          else buf_puts(b, "sp_box_nil()");
          buf_puts(b, ")");
          return;
        }
      }
      else if (comp_is_sg_reader(_sgcls, name)) {
        buf_printf(b, "sg_%s_%s", _sgcn, name);
        return;
      }
    }
  }

  /* obj.attr = val as an expression: store into the ivar and yield the value.
     The statement form is handled in emit_stmt; this expression form is hit
     when the assignment is the last statement of an instance_eval block. */
  if (recv >= 0) {
    int _alen = (int)strlen(name);
    TyKind _art = comp_ntype(c, recv);
    if (_alen > 1 && name[_alen - 1] == '=' && ty_is_object(_art)) {
      char _abase[256]; int _ablen = _alen - 1;
      if (_ablen < (int)sizeof _abase) {
        memcpy(_abase, name, (size_t)_ablen); _abase[_ablen] = '\0';
        int _arc = ty_object_class(_art), _adefc = -1;
        if (comp_writer_in_chain(c, _arc, _abase, &_adefc)) {
          char _aivn[258]; snprintf(_aivn, sizeof _aivn, "@%s", _abase);
          int _aiv = comp_ivar_index(&c->classes[_adefc < 0 ? _arc : _adefc], _aivn);
          TyKind _aivt = _aiv >= 0 ? c->classes[_adefc < 0 ? _arc : _adefc].ivar_types[_aiv] : TY_UNKNOWN;
          buf_puts(b, "(("); emit_expr(c, recv, b); buf_printf(b, ")->iv_%s = ", _abase);
          if (argc >= 1) {
            if (_aivt == TY_POLY && comp_ntype(c, argv[0]) != TY_POLY) emit_boxed(c, argv[0], b);
            else emit_expr(c, argv[0], b);
          }
          else buf_puts(b, "0");
          buf_puts(b, ")");
          return;
        }
      }
    }
  }

  /* `Module.accessor.cmethod(args)` folded to a constant (Stage-1): emit the
     resolved constant's class method directly. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int defcls = -1;
      int mi = comp_cmethod_in_chain(c, fold_ci, name, &defcls);
      if (mi >= 0) {
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].name, mc(c->scopes[mi].name));
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
    /* Stage-2: the accessor holds one of several constants (stored as a boxed
       Class). Dispatch the class method via a cls_id cascade over the slot. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      int valid = 0;
      for (int k = 0; k < ncand; k++) if (comp_cmethod_in_chain(c, cand[k], name, NULL) >= 0) valid++;
      if (valid > 0) {
        TyKind res = comp_ntype(c, id);
        int void_res = (res == TY_VOID || res == TY_UNKNOWN);
        int tcid = ++g_tmp;
        buf_printf(b, "({ int _t%d = (", tcid); emit_expr(c, recv, b); buf_puts(b, ").cls_id; ");
        if (void_res) {
          for (int k = 0; k < ncand; k++) {
            int defcls = -1;
            int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
            if (mi < 0) continue;
            buf_printf(b, "if (_t%d == %d) sp_%s_s_%s(", tcid, cand[k], c->classes[defcls].name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
            buf_puts(b, "); ");
          }
          buf_printf(b, "0; })");
          return;
        }
        emit_ctype(c, res, b); buf_printf(b, " _t%d_r = %s; ", tcid, default_value(res));
        for (int k = 0; k < ncand; k++) {
          int defcls = -1;
          int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
          if (mi < 0) continue;
          buf_printf(b, "if (_t%d == %d) _t%d_r = ", tcid, cand[k], tcid);
          if (res == TY_POLY && c->scopes[mi].ret != TY_POLY) {
            Buf cb; memset(&cb, 0, sizeof cb);
            buf_printf(&cb, "sp_%s_s_%s(", c->classes[defcls].name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", &cb);
            buf_puts(&cb, ")");
            emit_boxed_text(c, c->scopes[mi].ret, cb.p ? cb.p : "0", b);
            free(cb.p);
          }
          else {
            buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
            buf_puts(b, ")");
          }
          buf_puts(b, "; ");
        }
        buf_printf(b, "_t%d_r; })", tcid);
        return;
      }
    }
  }

  /* Class.cmethod(args) / M::Sub.cmethod(args) -> sp_<Class>_s_<method>(args) */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      int defcls = -1;
      int mi = ci >= 0 ? comp_cmethod_in_chain(c, ci, name, &defcls) : -1;
      if (mi >= 0) {
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].name, mc(c->scopes[mi].name));
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
  }

  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);

  /* regex literal match predicates (bool-returning, no MatchData/globals):
     /re/.match?(str[, pos])  and  str !~ /re/  and  str.match?(/re/[, pos]) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && (!strcmp(name, "match?") || !strcmp(name, "===")) && argc == 1) {
      /* /re/ === str and /re/.match?(str) both yield a match boolean */
      if (a0 == TY_POLY) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, sp_poly_to_s(", rre); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      return;
    }
    if (rre >= 0 && !strcmp(name, "match?") && argc == 2) {
      buf_printf(b, "sp_re_match_p_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    /* /re/ =~ str -> match offset or nil (poly) */
    if (rre >= 0 && !strcmp(name, "=~") && argc == 1 && a0 == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* /re/.source and /re/.options are compile-time constants of the literal */
    if (rre >= 0 && !strcmp(name, "source") && argc == 0) {
      emit_str_literal(b, nt_str(nt, recv, "unescaped")); return;
    }
    if (rre >= 0 && !strcmp(name, "options") && argc == 0) {
      int pf = (int)nt_int(nt, recv, "flags", 0);
      int opt = ((pf & 4) ? 1 : 0) | ((pf & 8) ? 2 : 0) | ((pf & 16) ? 4 : 0);
      buf_printf(b, "%d", opt); return;
    }
  }
  if (recv >= 0 && argc >= 1 && (!strcmp(name, "match?") || !strcmp(name, "!~") || !strcmp(name, "=~") || !strcmp(name, "match"))) {
    int are = re_lit_index(c, argv[0]);
    if (are >= 0 && !strcmp(name, "=~") && rt == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && !strcmp(name, "!~")) {
      buf_printf(b, "(!sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (are >= 0 && !strcmp(name, "match?")) {
      if (argc == 1) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && !strcmp(name, "match")) {
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      return;
    }
  }
  /* /re/.match(str) and /re/.match(str, pos) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && !strcmp(name, "match") && (argc == 1 || argc == 2)) {
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      return;
    }
  }

  /* General handler for regex-related calls where the pattern is an
     interpolated regex (/foo_#{x}/) or a TY_REGEX local variable.
     Covers match?, =~, !~, match, gsub, sub, scan, split as regex arg. */
  {
    /* Pattern from argument (str.match?(/dyn/), str =~ /dyn/, etc.) */
    if (recv >= 0 && argc >= 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      int is_interp_arg = a0ty && !strcmp(a0ty, "InterpolatedRegularExpressionNode");
      int is_regex_lv_arg = !is_interp_arg && argc >= 1 && comp_ntype(c, argv[0]) == TY_REGEX
                            && nt_type(nt, argv[0])
                            && (!strcmp(nt_type(nt, argv[0]), "LocalVariableReadNode") ||
                                !strcmp(nt_type(nt, argv[0]), "ConstantReadNode"));
      if (is_interp_arg || is_regex_lv_arg) {
        Buf rp; memset(&rp, 0, sizeof rp);
        int rp_ok = emit_regex_pat_to_buf(c, argv[0], &rp) && rp.p;
        /* Fallback: TY_REGEX local/constant/inline Regexp.new -- value IS the mrb_regexp_pattern* */
        if (!rp_ok && is_regex_lv_arg) {
          int tv = ++g_tmp;
          Buf eb; memset(&eb, 0, sizeof eb);
          emit_expr(c, argv[0], &eb);  /* may itself append pre-code to g_pre */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_regexp_pattern *_t%d = %s;\n", tv, eb.p ? eb.p : "NULL");
          free(eb.p);
          char tbuf[32]; snprintf(tbuf, sizeof tbuf, "_t%d", tv);
          memset(&rp, 0, sizeof rp); buf_puts(&rp, tbuf);
          rp_ok = 1;
        }
        if (rp_ok && rp.p) {
          if (!strcmp(name, "match?") && argc == 1) {
            buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (!strcmp(name, "=~") && rt == TY_STRING) {
            buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (!strcmp(name, "!~")) {
            buf_printf(b, "(!sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, "))");
            free(rp.p); return;
          }
          if (!strcmp(name, "match") && argc == 1) {
            buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          free(rp.p);
        }
      }
    }
    /* Pattern from receiver (rx.match?(str), rx =~ str, etc.) */
    {
      const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
      int is_interp_recv = rty && !strcmp(rty, "InterpolatedRegularExpressionNode");
      int is_regex_lv_recv = !is_interp_recv && recv >= 0 && comp_ntype(c, recv) == TY_REGEX;
      if (is_interp_recv || is_regex_lv_recv) {
        Buf rp; memset(&rp, 0, sizeof rp);
        int rp_ok = emit_regex_pat_to_buf(c, recv, &rp) && rp.p;
        /* Fallback: TY_REGEX local/constant/inline Regexp.new -- value IS the mrb_regexp_pattern* */
        if (!rp_ok && is_regex_lv_recv) {
          int tv = ++g_tmp;
          Buf eb; memset(&eb, 0, sizeof eb);
          emit_expr(c, recv, &eb);  /* may itself append pre-code to g_pre */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_regexp_pattern *_t%d = %s;\n", tv, eb.p ? eb.p : "NULL");
          free(eb.p);
          char tbuf[32]; snprintf(tbuf, sizeof tbuf, "_t%d", tv);
          memset(&rp, 0, sizeof rp); buf_puts(&rp, tbuf);
          rp_ok = 1;
        }
        if (rp_ok && rp.p) {
          if ((!strcmp(name, "match?") || !strcmp(name, "===")) && argc == 1) {
            if (a0 == TY_POLY) { buf_printf(b, "sp_re_match_p(%s, sp_poly_to_s(", rp.p); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
            else { buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            free(rp.p); return;
          }
          if (!strcmp(name, "=~") && argc == 1) {
            if (a0 == TY_STRING) {
              buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")");
            }
            else if (a0 == TY_POLY) {
              /* runtime type check: raise TypeError if not a string */
              int tv = ++g_tmp;
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "if (_t%d.tag != SP_TAG_STR) sp_raise_cls(\"TypeError\", \"no implicit conversion into String\");\n", tv);
              buf_printf(b, "sp_re_match_poly(%s, _t%d.v.s)", rp.p, tv);
            }
            else {
              /* statically known non-string: always raises TypeError */
              const char *tn = (a0 == TY_INT) ? "Integer" : (a0 == TY_FLOAT) ? "Float"
                             : (a0 == TY_BOOL) ? "true/false" : (a0 == TY_NIL) ? "NilClass" : "Object";
              buf_printf(b, "((void)(");
              emit_expr(c, argv[0], b);
              buf_printf(b, "), sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into String\"), sp_box_nil())", tn);
            }
            free(rp.p); return;
          }
          if (!strcmp(name, "match") && (argc == 1 || argc == 2)) {
            if (argc == 1) { buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            else { buf_printf(b, "sp_re_matchdata_at(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
            free(rp.p); return;
          }
          free(rp.p);
        }
      }
    }
  }

  /* String#% with an array argument: printf-style formatting. Any typed array
     is boxed to poly so a single format path handles mixed specs. */
  if (recv >= 0 && rt == TY_STRING && !strcmp(name, "%") && argc == 1) {
    TyKind at = a0;
    if (at == TY_POLY_ARRAY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *ak = array_kind(at);
    if (ak) {
      const char *kind = at == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : at == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", sp_typed_to_poly((void *)("); emit_expr(c, argv[0], b);
      buf_printf(b, "), %s))", kind);
      return;
    }
    /* a single non-array argument formats as a one-element array */
    if (at == TY_INT || at == TY_FLOAT || at == TY_STRING || at == TY_SYMBOL || at == TY_POLY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", ({ sp_PolyArray *_fa = sp_PolyArray_new(); sp_PolyArray_push(_fa, ");
      emit_boxed(c, argv[0], b); buf_puts(b, "); _fa; }))");
      return;
    }
  }

  /* an empty array literal as a receiver: its node type is unknown (element
     type is usage-folded, but a bare literal has no usage). Handle the common
     methods directly against an empty (poly) array. */
  if (recv >= 0 && rt == TY_UNKNOWN) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0) { buf_puts(b, "0"); return; }
        if (!strcmp(name, "empty?") && argc == 0) { buf_puts(b, "1"); return; }
        if ((!strcmp(name, "first") || !strcmp(name, "last") ||
             !strcmp(name, "min") || !strcmp(name, "max") ||
             !strcmp(name, "pop") || !strcmp(name, "shift")) && argc == 0) { buf_puts(b, "SP_INT_NIL"); return; }
        if (!strcmp(name, "sample") && argc == 0) { buf_puts(b, "0"); return; }
        if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) { buf_puts(b, "\"[]\""); return; }
        if ((!strcmp(name, "join") || !strcmp(name, "pack")) && argc <= 1) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
        if ((!strcmp(name, "union")) && argc == 0) { buf_puts(b, "sp_IntArray_new()"); return; }
        if ((!strcmp(name, "flatten") || !strcmp(name, "compact") || !strcmp(name, "uniq") ||
             !strcmp(name, "sort") || !strcmp(name, "reverse") || !strcmp(name, "dup") ||
             !strcmp(name, "clone") || !strcmp(name, "to_a")) && argc <= 1) {
          buf_puts(b, "sp_PolyArray_new()"); return;
        }
      }
    }
  }

  /* respond_to?(:m): compile-time approximation. A universal method set is
     always true; otherwise consult the receiver's class / class-method chain.
     Unknown primitive methods answer conservatively false. */
  if (!strcmp(name, "respond_to?") && recv >= 0 && argc >= 1) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "unescaped");
    if (qm) {
      static const char *const uni[] = {
        "to_s", "inspect", "class", "nil?", "dup", "clone", "freeze",
        "frozen?", "hash", "==", "!=", "equal?", "eql?", "object_id",
        "respond_to?", "is_a?", "kind_of?", "instance_of?", "itself",
        "tap", "then", "send", "===", NULL };
      int yes = 0, resolved = 0;
      for (int u = 0; uni[u]; u++) if (!strcmp(qm, uni[u])) { yes = resolved = 1; break; }
      if (!resolved) {
        const char *rty = nt_type(nt, recv);
        if (rty && !strcmp(rty, "ConstantReadNode")) {
          int ci = comp_class_index(c, nt_str(nt, recv, "name"));
          if (ci >= 0) {
            resolved = 1;
            yes = comp_cmethod_in_chain(c, ci, qm, NULL) >= 0;
            /* singleton attr_accessor/reader/writer via class << self */
            if (!yes) {
              size_t ql = strlen(qm);
              int is_wr = ql > 0 && qm[ql - 1] == '=';
              if (is_wr) {
                char base[256];
                if (ql - 1 < sizeof base) { memcpy(base, qm, ql - 1); base[ql - 1] = '\0'; }
                yes = comp_is_sg_writer(&c->classes[ci], base);
              }
              else {
                yes = comp_is_sg_reader(&c->classes[ci], qm);
              }
            }
            /* a module also responds to its def'd (module_function) methods */
            if (!yes) {
              int dn = c->classes[ci].def_node;
              const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
              if (dt && !strcmp(dt, "ModuleNode")) yes = comp_method_in_chain(c, ci, qm, NULL) >= 0;
            }
            /* builtin Class/Module methods every class object inherits */
            if (!yes) {
              static const char *const cls_uni[] = {
                "name", "instance_methods", "public_instance_methods",
                "private_instance_methods", "protected_instance_methods",
                "instance_method", "method_defined?", "superclass", "ancestors",
                "include?", "const_get", "const_set", "const_defined?",
                "define_method", "allocate", "<", "<=", ">", ">=", NULL };
              for (int u = 0; cls_uni[u]; u++) if (!strcmp(qm, cls_uni[u])) { yes = 1; break; }
              /* `new`: a class responds, a module does not */
              if (!yes && !strcmp(qm, "new")) {
                int dn = c->classes[ci].def_node;
                const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
                yes = !(dt && !strcmp(dt, "ModuleNode"));
              }
            }
          }
        }
        else if (ty_is_object(rt)) {
          int cid = ty_object_class(rt);
          resolved = 1;
          yes = comp_method_in_chain(c, cid, qm, NULL) >= 0 ||
                comp_reader_in_chain(c, cid, qm, NULL) ||
                comp_writer_in_chain(c, cid, qm, NULL);
        }
        /* a primitive/poly/unknown receiver with a non-universal method: we
           lack a per-type method table, so leave it to the fall-through
           rather than answer a possibly-wrong false. */
      }
      if (resolved) { buf_printf(b, "%d", yes); return; }
    }
  }

  /* Class.method_defined?(:m[, inherit]): compile-time decided from the
     class's recorded method table (instance methods + attr readers/writers).
     inherit=false restricts the lookup to the receiver's own definitions. */
  if (!strcmp(name, "method_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (qm && ci >= 0) {
      int inherit = 1;
      if (argc >= 2) {
        const char *it = nt_type(nt, argv[1]);
        if (it && !strcmp(it, "FalseNode")) inherit = 0;
      }
      /* a writer query (`m=`) consults the writer table under its base name */
      size_t ln = strlen(qm);
      int is_setter = ln > 0 && qm[ln - 1] == '=';
      char base[256];
      base[0] = '\0';
      if (is_setter && ln - 1 < sizeof base) { memcpy(base, qm, ln - 1); base[ln - 1] = '\0'; }
      int parent = c->classes[ci].parent;
      int mc = -1;
      int mi = comp_method_in_chain(c, ci, qm, &mc);
      int yes;
      if (inherit) {
        yes = mi >= 0 || comp_reader_in_chain(c, ci, qm, NULL) ||
              (is_setter && comp_writer_in_chain(c, ci, base, NULL));
      }
      else {
        /* attr readers/writers are flattened into descendants at analyze
           time, so "own" means present here but not in the parent chain */
        int rd_own = comp_is_reader(&c->classes[ci], qm) &&
                     (parent < 0 || !comp_reader_in_chain(c, parent, qm, NULL));
        int wr_own = is_setter && comp_is_writer(&c->classes[ci], base) &&
                     (parent < 0 || !comp_writer_in_chain(c, parent, base, NULL));
        yes = (mi >= 0 && mc == ci) || rd_own || wr_own;
      }
      buf_printf(b, "%d", yes);
      return;
    }
  }

  /* Class.const_defined?(:K): compile-time presence check. Constants are
     recorded in a flat namespace, so this consults the global const and class
     tables rather than the receiver's own constants. */
  if (!strcmp(name, "const_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && !strcmp(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && !strcmp(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    if (qm) {
      int yes = comp_const(c, qm) != NULL || comp_class_index(c, qm) >= 0;
      buf_printf(b, "%d", yes);
      return;
    }
  }

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0 && !ty_is_object(rt)) {
    if (rt == TY_POLY) {
      if (name[0] == '-') { buf_puts(b, "sp_poly_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { emit_expr(c, recv, b); }  /* +@ is identity on poly */
    }
    else { buf_puts(b, name[0] == '-' ? "(-" : "(+"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  /* poly `<<` in expression position: sp_poly_shl dispatches on the runtime tag
     (Integer#<< shift -> boxed int, Array#push append -> the array) and returns
     a poly either way, matching the statement-level path. */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "<<") && argc == 1) {
    buf_puts(b, "sp_poly_shl("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* unary bitwise complement: ~int -> (~x); ~poly -> coerce to int first */
  if (!strcmp(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY)) {
    if (rt == TY_POLY) { buf_puts(b, "(~sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    else { buf_puts(b, "(~"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  /* poly numeric predicates: coerce the poly value to int and test. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (!strcmp(name, "even?") || !strcmp(name, "odd?") || !strcmp(name, "zero?") ||
       !strcmp(name, "positive?") || !strcmp(name, "negative?"))) {
    int t = ++g_tmp;
    buf_printf(b, "({ mrb_int _t%d = sp_poly_to_i(", t); emit_expr(c, recv, b); buf_puts(b, "); ");
    if (!strcmp(name, "even?")) buf_printf(b, "(_t%d %% 2 == 0); })", t);
    else if (!strcmp(name, "odd?")) buf_printf(b, "(_t%d %% 2 != 0); })", t);
    else if (!strcmp(name, "zero?")) buf_printf(b, "(_t%d == 0); })", t);
    else if (!strcmp(name, "positive?")) buf_printf(b, "(_t%d > 0); })", t);
    else buf_printf(b, "(_t%d < 0); })", t);
    return;
  }
  if (!strcmp(name, "!") && recv >= 0 && argc == 0) {
    /* Ruby truthiness: only nil and false are falsy. `!x` negates the same
       per-type truthiness emit_cond uses -- a poly / nullable scalar / nullable
       pointer can be falsy, so the result is not unconditionally false. */
    if (rt == TY_BOOL) { buf_puts(b, "(!"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (rt == TY_NIL) { buf_puts(b, "1"); }
    else if (rt == TY_POLY) { buf_puts(b, "(!sp_poly_truthy("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    else if (rt == TY_INT) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)"); }
    else if (rt == TY_FLOAT) { buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else if (rt == TY_STRING || ty_is_array(rt) || ty_is_hash(rt) || ty_is_object(rt) ||
             rt == TY_PROC || rt == TY_STRINGIO || rt == TY_STRINGSCANNER ||
             rt == TY_MATCHDATA || rt == TY_EXCEPTION || rt == TY_FIBER || rt == TY_IO) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");  /* NULL pointer is falsy */
    }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }  /* always-truthy -> false */
    return;
  }

  /* poly arithmetic: sp_poly_<op>(boxed, boxed) -> a (poly) result.
     `str + poly` / `str * poly` are string concat/repeat (handled below as
     sp_str_concat/sp_str_repeat with the poly operand coerced), not poly
     arithmetic, so let them fall through. */
  if (recv >= 0 && argc == 1 && (rt == TY_POLY || a0 == TY_POLY) &&
      !(rt == TY_STRING && (!strcmp(name, "+") || !strcmp(name, "*")))) {
    const char *pfn = NULL;
    if (!strcmp(name, "+")) pfn = "sp_poly_add";
    else if (!strcmp(name, "-")) pfn = "sp_poly_sub";
    else if (!strcmp(name, "*")) pfn = "sp_poly_mul";
    else if (!strcmp(name, "/")) pfn = "sp_poly_div";
    else if (!strcmp(name, "%")) pfn = "sp_poly_mod";
    else if (!strcmp(name, "**")) pfn = "sp_poly_pow";
    if (pfn) {
      buf_printf(b, "%s(", pfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *cfn = NULL;
    if (!strcmp(name, "<")) cfn = "sp_poly_lt";
    else if (!strcmp(name, ">")) cfn = "sp_poly_gt";
    else if (!strcmp(name, "<=")) cfn = "sp_poly_le";
    else if (!strcmp(name, ">=")) cfn = "sp_poly_ge";
    if (cfn) {
      buf_printf(b, "%s(", cfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Array#* (join): arr * sep_str  ->  elements joined by separator string. */
  if (recv >= 0 && argc == 1 && !strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      comp_ntype(c, argv[0]) == TY_STRING) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) k = "Str";
    buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* Array#* (repeat): arr * n  ->  new array with elements repeated n times. */
  if (recv >= 0 && argc == 1 && !strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      comp_ntype(c, argv[0]) == TY_INT) {
    int ta = ++g_tmp, tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    if (rt == TY_POLY_ARRAY) {
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                    " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                    " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                 tr, tr,
                 ti, ti, tn, ti,
                 tj, tj, ta, tj,
                 tr, ta, tj, tr);
    }
    else {
      const char *k = array_kind(rt);
      /* Only IntArray has a start offset; Float/StrArray index directly. */
      int has_start = (rt == TY_INT_ARRAY);
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
      if (has_start) {
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d->start + _t%d]); _t%d; })",
                   k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, ta, tj, tr);
      }
      else {
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                   k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, tj, tr);
      }
    }
    return;
  }

  if (recv >= 0 && argc == 1 && int_arith_fn(name) && !ty_is_object(rt) && !ty_is_array(rt)) {
    if (rt == TY_STRING && !strcmp(name, "+")) {
      /* Root both operands when either may allocate: `a + b` evaluates both,
         and a fresh heap string from one can be swept while the other
         allocates or forces a GC (chained `a + b + c` with side-effecting
         operands — concat_chain_operand_gc_root). Recurses naturally: a
         chain's left operand is itself a `+` and gets its own rooted block.
         Pure literal / bare-read operands need no rooting. */
      /* A poly operand (statically typed string here, holds a string at
         runtime) must be coerced to a C string for sp_str_concat. */
      int arg_poly = comp_ntype(c, argv[0]) == TY_POLY;
      if (subtree_may_allocate(nt, recv) || subtree_may_allocate(nt, argv[0])) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); const char *_t%d = ", ta, tb);
        if (arg_poly) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_str_concat(_t%d, _t%d); })", tb, ta, tb);
      }
      else {
        buf_puts(b, "sp_str_concat(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (arg_poly) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      return;
    }
    if (rt == TY_STRING && !strcmp(name, "*")) {
      buf_puts(b, "sp_str_repeat(");
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (res == TY_BIGINT) {
      const char *bfn = bigint_arith_fn(name);
      if (bfn) {
        buf_printf(b, "%s(", bfn);
        if (rt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        else emit_expr(c, recv, b);
        buf_puts(b, ", ");
        TyKind at0 = comp_ntype(c, argv[0]);
        if (at0 != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
    }
    /* Re-derive result type when cache may be stale due to block-param widening */
    TyKind eff_res = res;
    if (eff_res != TY_INT && eff_res != TY_FLOAT && eff_res != TY_BIGINT) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) eff_res = TY_FLOAT;
      else if (rt == TY_INT && (a0 == TY_INT || a0 == TY_UNKNOWN)) eff_res = TY_INT;
    }
    if (eff_res == TY_INT) {
      int isdivmod = !strcmp(name, "/") || !strcmp(name, "%");
      buf_printf(b, "%s(", int_arith_fn(name));
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (isdivmod) emit_int_divisor(c, argv[0], b);
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (eff_res == TY_FLOAT && !strcmp(name, "**") && rt != TY_TIME) {
      TyKind at0 = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      buf_puts(b, "pow(");
      if (rt == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (at0 == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (eff_res == TY_FLOAT && rt != TY_TIME && strcmp(name, "%") && strcmp(name, "**")) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    /* Time + int/float, Time - int/float, Time - Time */
    if (rt == TY_TIME && (!strcmp(name, "+") || !strcmp(name, "-"))) {
      TyKind at = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int tt = ++g_tmp, tu = ++g_tmp;
      if (!strcmp(name, "-") && at == TY_TIME) {
        /* Time - Time -> Float */
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_sub_t(_t%d, _t%d); })", tt, tu);
      }
      else if (at == TY_FLOAT) {
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; double _t%d = ", tu); emit_expr(c, argv[0], b);
        if (!strcmp(name, "+"))
          buf_printf(b, "; sp_time_add_f(_t%d, _t%d); })", tt, tu);
        else
          buf_printf(b, "; sp_time_add_f(_t%d, -_t%d); })", tt, tu);
      }
      else {
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tu); emit_expr(c, argv[0], b);
        if (!strcmp(name, "+"))
          buf_printf(b, "; sp_time_add_i(_t%d, _t%d); })", tt, tu);
        else
          buf_printf(b, "; sp_time_sub_i(_t%d, _t%d); })", tt, tu);
      }
      return;
    }
    unsupported(c, id, "arithmetic");
  }

  /* integer bitwise operators. A poly receiver is coerced to int (the matching
     inference types these TY_INT); `<<` on a poly is handled earlier as the
     ambiguous shift/append via sp_poly_shl, so only &,|,^,>> reach here. */
  if (recv >= 0 && argc == 1 &&
      ((rt == TY_INT && (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
                         !strcmp(name, "<<") || !strcmp(name, ">>"))) ||
       (rt == TY_POLY && (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
                          !strcmp(name, ">>"))))) {
    TyKind at0 = comp_ntype(c, argv[0]);
    buf_puts(b, "(");
    if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else emit_expr(c, recv, b);
    buf_printf(b, " %s ", name);
    if (at0 == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  if (recv >= 0 && argc == 1 && !strcmp(name, "<=>")) {
    /* Re-infer when stale cache has TY_POLY (e.g. block params temporarily pinned to element type). */
    TyKind lrt = (rt == TY_POLY || rt == TY_UNKNOWN) ? infer_type(c, recv) : rt;
    TyKind at = comp_ntype(c, argv[0]);
    TyKind lat = (at == TY_POLY || at == TY_UNKNOWN) ? infer_type(c, argv[0]) : at;
    if (ty_is_numeric(lrt) && ty_is_numeric(lat)) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, lrt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b);
      buf_puts(b, "; "); emit_ctype(c, lat, b); buf_printf(b, " _t%d = ", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; (_t%d > _t%d) - (_t%d < _t%d); })", ta, tb, ta, tb);
      return;
    }
    if (lrt == TY_STRING && lat == TY_STRING) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = strcmp(", tc); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, "); (_t%d > 0) - (_t%d < 0); })", tc, tc);
      return;
    }
    if (lrt == TY_SYMBOL && lat == TY_SYMBOL) {
      int tc = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ sp_sym _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_sym _t%d = ", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; int _t%d = strcmp(sp_sym_to_s(_t%d), sp_sym_to_s(_t%d));"
                    " (_t%d > 0) - (_t%d < 0); })", tc, ta, tb, tc, tc);
      return;
    }
    if (lrt == TY_TIME) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Time _t%d = ", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; (mrb_int)sp_time_cmp(_t%d, _t%d); })", ta, tb);
      return;
    }
  }

  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "<") || !strcmp(name, ">") ||
       !strcmp(name, "<=") || !strcmp(name, ">="))) {
    if (rt == TY_BIGINT || comp_ntype(c, argv[0]) == TY_BIGINT) {
      buf_printf(b, "(sp_bigint_cmp(");
      if (rt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      TyKind cmp_at = comp_ntype(c, argv[0]);
      if (cmp_at != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return;
    }
    if (ty_is_numeric(rt)) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (rt == TY_STRING) {
      buf_puts(b, "(strcmp(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return;
    }
    /* Time comparison via sp_time_cmp */
    if (rt == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
      return;
    }
    /* Comparable: object with a user `<=>` method but no direct `<` etc. */
    if (ty_is_object(rt)) {
      int cid4 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid4, name, NULL) < 0 &&
          comp_method_in_chain(c, cid4, "<=>", NULL) >= 0) {
        char selfptr[64];
        const char *rtyp = nt_type(nt, recv);
        if (rtyp && (!strcmp(rtyp, "LocalVariableReadNode") ||
                     !strcmp(rtyp, "InstanceVariableReadNode") ||
                     !strcmp(rtyp, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t4 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t4, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t4);
        }
        buf_puts(b, "(");
        emit_dispatch(c, cid4, "<=>", selfptr, nt_ref(nt, id, "arguments"), -1, b);
        buf_printf(b, " %s 0)", name);
        return;
      }
    }
    unsupported(c, id, "comparison");
  }

  /* concrete builtin receiver: is_a?/kind_of?/instance_of? is known at compile
     time (evaluate the receiver for side effects, then yield the constant). */
  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?")) &&
      nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")) {
    /* `[]` and a bare `Array.new` are arrays even when their element type (and
       so the inferred type) is still UNKNOWN -- treat them as such for the fold. */
    TyKind eff_rt = rt;
    if (eff_rt == TY_UNKNOWN) {
      const char *rvt = nt_type(nt, recv);
      if (rvt && !strcmp(rvt, "ArrayNode")) eff_rt = TY_POLY_ARRAY;
      else if (rvt && !strcmp(rvt, "CallNode") && nt_str(nt, recv, "name") &&
               !strcmp(nt_str(nt, recv, "name"), "new")) {
        int rr = nt_ref(nt, recv, "receiver");
        if (rr >= 0 && nt_type(nt, rr) && !strcmp(nt_type(nt, rr), "ConstantReadNode") &&
            nt_str(nt, rr, "name") && !strcmp(nt_str(nt, rr, "name"), "Array")) eff_rt = TY_POLY_ARRAY;
      }
    }
    int yes = ty_matches_class(eff_rt, nt_str(nt, argv[0], "name"), !strcmp(name, "instance_of?"));
    if (yes >= 0) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes); return; }
  }

  /* poly.is_a?(class_var) where the argument is a TY_CLASS typed expression.
     Skip if argv[0] is a ConstantReadNode: the fast-path below handles builtins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?")) &&
      comp_ntype(c, argv[0]) == TY_CLASS &&
      !(nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode"))) {
    int t = ++g_tmp, k = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
    buf_printf(b, "sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
    if (!strcmp(name, "instance_of?"))
      buf_printf(b, "sp_poly_get_class(_t%d).cls_id == _t%d.cls_id; })", t, k);
    else
      buf_printf(b, "sp_poly_is_a(_t%d, _t%d); })", t, k);
    return;
  }

  /* poly.is_a?(Class) / kind_of?: runtime tag/cls_id check */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
    const char *cty = nt_type(nt, argv[0]);
    const char *cn = cty && !strcmp(cty, "ConstantReadNode") ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
      char v[32]; snprintf(v, sizeof v, "_t%d", t);
      if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum")) buf_printf(b, "%s.tag == SP_TAG_INT", v);
      else if (!strcmp(cn, "String"))   buf_printf(b, "%s.tag == SP_TAG_STR", v);
      else if (!strcmp(cn, "Float"))    buf_printf(b, "%s.tag == SP_TAG_FLT", v);
      else if (!strcmp(cn, "Symbol"))   buf_printf(b, "%s.tag == SP_TAG_SYM", v);
      else if (!strcmp(cn, "NilClass")) buf_printf(b, "%s.tag == SP_TAG_NIL", v);
      else if (!strcmp(cn, "TrueClass"))  buf_printf(b, "(%s.tag == SP_TAG_BOOL && %s.v.b)", v, v);
      else if (!strcmp(cn, "FalseClass")) buf_printf(b, "(%s.tag == SP_TAG_BOOL && !%s.v.b)", v, v);
      else if (!strcmp(cn, "Numeric"))  buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", v, v);
      else if (!strcmp(cn, "Array"))    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", v, v, v);
      else if (!strcmp(cn, "Hash"))     buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -13 && %s.cls_id >= -20)", v, v, v);
      else if (!strcmp(cn, "Encoding")) buf_printf(b, "%s.tag == SP_TAG_ENCODING", v);
      else {
        int cid = comp_class_index(c, cn);
        int exact = !strcmp(name, "instance_of?");
        if (cid >= 0) {
          buf_printf(b, "(%s.tag == SP_TAG_OBJ && (", v);
          int first = 1;
          for (int k = 0; k < c->nclasses; k++)
            if (k == cid || (!exact && is_descendant(c, k, cid))) {
              buf_printf(b, "%s%s.cls_id == %d", first ? "" : " || ", v, k); first = 0;
            }
          if (first) buf_puts(b, "0");
          buf_puts(b, "))");
        }
        else buf_puts(b, "0");
      }
      buf_puts(b, "; })");
      return;
    }
  }

  /* nil receiver: nil.inspect -> "nil", nil.to_s -> "", nil.nil? -> true.
     Evaluate the receiver for side effects, then yield the constant. */
  if (recv >= 0 && rt == TY_NIL) {
    if (argc == 0 && !strcmp(name, "inspect")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"nil\"))"); return; }
    if (argc == 0 && !strcmp(name, "to_s"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"\"))"); return; }
    if (argc == 0 && !strcmp(name, "nil?"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return; }
    if (argc == 0 && !strcmp(name, "to_i"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)0)"); return; }
    if (argc == 0 && !strcmp(name, "to_f"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0.0)"); return; }
    if (argc == 0 && !strcmp(name, "to_r"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_float)0.0)"); return; }
    if (argc == 0 && !strcmp(name, "to_a"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())"); return; }
    if (argc == 0 && !strcmp(name, "to_h"))    {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_SymPolyHash_new())");
      return;
    }
    if (argc == 1 && (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
      const char *cn = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode") ? nt_str(nt, argv[0], "name") : NULL;
      int yes = cn ? (!strcmp(cn, "NilClass") || !strcmp(name, "instance_of?") ? !strcmp(cn, "NilClass") : (!strcmp(cn, "Object") || !strcmp(cn, "BasicObject"))) : 0;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return;
    }
  }

  /* encoding.name -> the encoding name string */
  if (!strcmp(name, "name") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_POLY) {
    const char *rty2 = nt_type(nt, recv);
    int is_enc = (rty2 && !strcmp(rty2, "SourceEncodingNode")) ||
                 (rty2 && !strcmp(rty2, "CallNode") &&
                  nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding"));
    if (is_enc) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
  }

  /* poly receiver: nil? / conversions / a few type-agnostic queries */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    if (!strcmp(name, "nil?")) { buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    if (!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "empty?")) {
      int has_user_len = 0;
      const char *lcheck = (!strcmp(name, "empty?")) ? "length" : name;
      for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
        if (comp_method_in_chain(c, kk, lcheck, NULL) >= 0) has_user_len = 1;
      if (!strcmp(name, "empty?") && !has_user_len)
        for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
          if (comp_method_in_chain(c, kk, "empty?", NULL) >= 0) has_user_len = 1;
      if (!has_user_len) {
        if (!strcmp(name, "empty?")) {
          buf_puts(b, "(sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        }
        else {
          buf_puts(b, "sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        return;
      }
    }
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) {
      int has_user_method = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (comp_method_in_chain(c, k, name, NULL) >= 0) { has_user_method = 1; break; }
      if (!has_user_method) {
        buf_printf(b, "%s(", !strcmp(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect");
        emit_expr(c, recv, b); buf_puts(b, ")"); return;
      }
    }
    if (!strcmp(name, "to_i")) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    if (!strcmp(name, "to_f")) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
    if (!strcmp(name, "upcase"))     { buf_puts(b, "sp_box_str(sp_str_upcase(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "downcase"))   { buf_puts(b, "sp_box_str(sp_str_downcase(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "capitalize")) { buf_puts(b, "sp_box_str(sp_str_capitalize(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "strip"))      { buf_puts(b, "sp_box_str(sp_str_strip(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "reverse"))    { buf_puts(b, "sp_box_str(sp_str_reverse(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "chomp"))      { buf_puts(b, "sp_box_str(sp_str_chomp(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "chop"))       { buf_puts(b, "sp_box_str(sp_str_chop(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "chr"))        { buf_puts(b, "sp_box_str(sp_str_chr(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return; }
    if (!strcmp(name, "freeze"))     { emit_expr(c, recv, b); return; }
  }
  /* poly receiver: arr[start, len] = src -- 3-arg splice assign
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]=") && argc == 3 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    int tv = ++g_tmp;
    buf_puts(b, "({ sp_RbVal _t"); buf_printf(b, "%d = ", tv); emit_boxed(c, argv[2], b);
    buf_puts(b, "; sp_poly_splice("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b);
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    return;
  }
  /* poly receiver: []= with symbol, string, int, or poly key -> runtime dispatch
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]=") && argc == 2 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    TyKind at = comp_ntype(c, argv[0]);
    TyKind vt = comp_ntype(c, argv[1]);
    int tv = ++g_tmp;
    buf_puts(b, "({ sp_RbVal _t"); buf_printf(b, "%d = ", tv); emit_boxed(c, argv[1], b);
    buf_puts(b, "; ");
    if (at == TY_STRING) {
      buf_printf(b, "sp_poly_set_str("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else if (at == TY_SYMBOL) {
      buf_printf(b, "sp_poly_set_sym("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[0], b);
    }
    else if (at == TY_INT) {
      /* widen_and_set only returns a *different* boxed value when an IntArray is
         promoted to a PolyArray; otherwise it mutates in place. We can only
         write the result back when the receiver is a simple assignable lvalue
         (a local or ivar). For a computed receiver (e.g. @nmt_mem[bank][idx]=v,
         where the receiver is itself an array element), drop the write-back and
         rely on in-place mutation. */
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (!strcmp(rcvty, "LocalVariableReadNode") ||
                                     !strcmp(rcvty, "InstanceVariableReadNode"));
      if (recv_is_lvalue) {
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
      }
      else {
        buf_puts(b, "sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
      }
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    }
    else {
      buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    }
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    (void)vt;
    return;
  }
  /* poly receiver: [] with symbol or string key -> runtime dispatch */
  /* poly receiver: arr[start, len] -> sp_poly_slice (string or typed array) */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]") && argc == 2) {
    /* The runtime dispatches on the receiver's tag: a string/array does a
       two-arg slice, a bound Method (optcarrot's poke handlers) is called with
       both int args. Both operands are raw integers. */
    buf_puts(b, "sp_poly_slice("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    return;
  }
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "[]") && argc == 1) {
    /* `@table[i][j]` dispatch table narrowed to int (poly_double_index_int):
       call the entry (bound method / int array) for an unboxed int result. */
    if (comp_ntype(c, id) == TY_INT) {
      buf_puts(b, "sp_poly_index_int("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    TyKind at = comp_ntype(c, argv[0]);
    /* Only use the fast single-call path when no user class defines [].
       If any user class has its own [] method, fall through to the per-class
       poly dispatch (line ~4640) which generates both user and builtin arms. */
    int has_user_aref = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, "[]", NULL) >= 0) { has_user_aref = 1; break; }
    if (!has_user_aref) {
      if (at == TY_SYMBOL) {
        buf_puts(b, "sp_poly_get_sym("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (at == TY_STRING) {
        buf_puts(b, "sp_poly_get_str("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (at == TY_INT) {
        buf_puts(b, "sp_poly_arr_get_hash("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (at == TY_POLY) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      /* a non-poly key (e.g. a Method): box it, then index polymorphically */
      if (at != TY_UNKNOWN) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
    }
  }
  /* poly receiver: join */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "join")) {
    buf_puts(b, "sp_poly_join("); emit_expr(c, recv, b);
    buf_puts(b, ", "); if (argc >= 1) emit_expr(c, argv[0], b); else buf_puts(b, "\"\"");
    buf_puts(b, ")"); return;
  }
  /* poly receiver: replace(other) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "replace") && argc == 1) {
    buf_puts(b, "sp_poly_replace("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* poly receiver: pack(fmt) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && !strcmp(name, "pack") && argc == 1) {
    buf_puts(b, "sp_poly_pack("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* poly receiver: gsub/sub with a regex literal -- extract the string
     payload (poly values reaching here are strings) and route to the
     engine, just like a TY_STRING receiver. */
  if (recv >= 0 && rt == TY_POLY && (!strcmp(name, "gsub") || !strcmp(name, "sub")) &&
      argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, sp_poly_to_s(", name, suf, re_lit_index(c, argv[0]));
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    return;
  }

  /* between?(lo, hi): lo <= self <= hi */
  if (!strcmp(name, "between?") && argc == 2) {
    if (rt == TY_STRING) {
      int tv = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (strcmp(_t%d, ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, ") >= 0 && strcmp(_t%d, ", tv); emit_expr(c, argv[1], b); buf_puts(b, ") <= 0); })");
      return;
    }
    if (ty_is_numeric(rt)) {
      int tv = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; (_t%d >= ", tv); emit_expr(c, argv[0], b);
      buf_printf(b, " && _t%d <= ", tv); emit_expr(c, argv[1], b); buf_puts(b, "); })");
      return;
    }
    /* Comparable: user type with <=> method */
    if (ty_is_object(rt)) {
      int cid_b = ty_object_class(rt);
      int defcls_b = -1;
      int mi_b = comp_method_in_chain(c, cid_b, "<=>", &defcls_b);
      if (mi_b >= 0) {
        int ts = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        const char *cname = c->classes[defcls_b].name;
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts);
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", tlo);
        Buf lb; memset(&lb, 0, sizeof lb); emit_expr(c, argv[0], &lb);
        buf_puts(g_pre, lb.p ? lb.p : ""); buf_puts(g_pre, ";\n"); free(lb.p);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", thi);
        Buf hb; memset(&hb, 0, sizeof hb); emit_expr(c, argv[1], &hb);
        buf_puts(g_pre, hb.p ? hb.p : ""); buf_puts(g_pre, ";\n"); free(hb.p);
        buf_printf(b, "(sp_%s_%s((sp_%s *)_t%d, _t%d) >= 0 && sp_%s_%s((sp_%s *)_t%d, _t%d) <= 0)",
                   cname, mc("<=>"), cname, ts, tlo,
                   cname, mc("<=>"), cname, ts, thi);
        return;
      }
    }
  }

  /* object_id: a stable integer id. Int uses MRI's 2n+1; pointer-backed
     values use the pointer bit pattern; a symbol uses its interned id. */
  if (!strcmp(name, "object_id") && recv >= 0 && argc == 0) {
    if (rt == TY_INT) { buf_puts(b, "(2*("); emit_expr(c, recv, b); buf_puts(b, ")+1)"); }
    else if (rt == TY_SYMBOL) { buf_puts(b, "((mrb_int)("); emit_expr(c, recv, b); buf_puts(b, ")*2)"); }
    else if (rt == TY_BOOL || rt == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }
    /* a boxed value: its identity is the boxed payload (heap pointer / int) */
    else if (rt == TY_POLY) { buf_puts(b, "((mrb_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, ").v.p)"); }
    else { buf_puts(b, "((mrb_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    return;
  }

  /* nil? on an integer: a nullable int carries the SP_INT_NIL sentinel
     (e.g. an int-valued hash miss). A plain int is never the sentinel, so
     `5.nil?` constant-folds to false; a missing-key value reads true. */
  if (recv >= 0 && rt == TY_INT && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)");
    return;
  }
  /* nil? on a string: a nullable string carries NULL (e.g. a scan miss) */
  if (recv >= 0 && rt == TY_STRING && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
    return;
  }
  /* nil? on a float: a nullable float carries the NaN sentinel (e.g. first/
     last of an empty float array). A real float is never the sentinel. */
  if (recv >= 0 && rt == TY_FLOAT && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* nil? on an array/hash: a nil container is a NULL pointer */
  if (recv >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && !strcmp(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return;
  }
  /* nil? on a pointer-backed concrete type: nil is the NULL pointer. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "nil?") &&
      (rt == TY_FIBER || rt == TY_PROC || rt == TY_CURRY || rt == TY_RANDOM ||
       rt == TY_METHOD || rt == TY_IO || rt == TY_STRINGIO || rt == TY_STRINGSCANNER ||
       rt == TY_MATCHDATA || rt == TY_REGEX || rt == TY_EXCEPTION || rt == TY_BIGINT)) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return;
  }
  /* nil? on a value-typed concrete receiver is always false. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "nil?") &&
      (rt == TY_RANGE || rt == TY_TIME || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_SYMBOL || rt == TY_BOOL || rt == TY_CLASS)) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
    return;
  }
  /* a predicate on an empty array literal folds to a constant: the block (if
     any) never runs, so empty all?/none? are true, any?/one? false */
  if (recv >= 0 && (argc == 0 || argc == 1) &&
      (!strcmp(name, "all?") || !strcmp(name, "any?") ||
       !strcmp(name, "none?") || !strcmp(name, "one?") || !strcmp(name, "count")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    if (!strcmp(name, "count")) { buf_puts(b, "0"); return; }
    buf_puts(b, (!strcmp(name, "all?") || !strcmp(name, "none?")) ? "1" : "0");
    return;
  }

  /* Class.===(obj): equivalent to obj.is_a?(Class). Receiver is a class constant. */
  if (recv >= 0 && argc == 1 && !strcmp(name, "===") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode")) {
    const char *cn = nt_str(nt, recv, "name");
    if (cn) {
      TyKind at2 = comp_ntype(c, argv[0]);
      /* TrueClass/FalseClass/NilClass === <literal/typed value>: decide
         statically from the arg's node kind or scalar type. */
      const char *aty = nt_type(nt, argv[0]);
      if (!strcmp(cn, "NilClass") || !strcmp(cn, "TrueClass") || !strcmp(cn, "FalseClass")) {
        int yn = -1;
        if (!strcmp(cn, "NilClass"))
          yn = (at2 == TY_NIL || (aty && !strcmp(aty, "NilNode"))) ? 1 : (at2 != TY_POLY ? 0 : -1);
        else if (!strcmp(cn, "TrueClass"))
          yn = (aty && !strcmp(aty, "TrueNode")) ? 1 : (aty && !strcmp(aty, "FalseNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
        else
          yn = (aty && !strcmp(aty, "FalseNode")) ? 1 : (aty && !strcmp(aty, "TrueNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
        if (yn >= 0) { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", yn); return; }
      }
      int yes = ty_matches_class(at2, cn, 0);
      if (yes >= 0) {
        buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", yes);
        return;
      }
      /* arg type is poly or unknown: runtime tag check */
      if (at2 == TY_POLY) {
        int tv = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, argv[0], b); buf_printf(b, "; ");
        char v[32]; snprintf(v, sizeof v, "_t%d", tv);
        if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum")) buf_printf(b, "%s.tag == SP_TAG_INT", v);
        else if (!strcmp(cn, "String"))   buf_printf(b, "%s.tag == SP_TAG_STR", v);
        else if (!strcmp(cn, "Float"))    buf_printf(b, "%s.tag == SP_TAG_FLT", v);
        else if (!strcmp(cn, "Symbol"))   buf_printf(b, "%s.tag == SP_TAG_SYM", v);
        else if (!strcmp(cn, "NilClass")) buf_printf(b, "%s.tag == SP_TAG_NIL", v);
        else if (!strcmp(cn, "Numeric"))  buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", v, v);
        else if (!strcmp(cn, "Array"))    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", v, v, v);
        else buf_printf(b, "0");
        buf_puts(b, "; })");
        return;
      }
    }
  }

  /* `===` on a scalar comparable (bool/int/float/string/symbol) is case
     equality == value equality. Range/Class/Regexp `===` have their own
     handlers and fall through here. */
  if (argc == 1 && !strcmp(name, "===")) {
    int fr = eq_family(rt), fa = eq_family(a0);
    if (fr && fr != 5 && fa && fa != 5) {
      if (fr == fa) {
        if (fr == 2) { buf_puts(b, "sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), ("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      return;
    }
  }

  if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "!="))) {
    int eq = !strcmp(name, "==");
    /* `x == nil` / `x != nil` for any receiver */
    int a_nil = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "NilNode");
    int r_nil = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "NilNode");
    if (a_nil || r_nil) {
      int other = a_nil ? recv : argv[0];
      TyKind ot = comp_ntype(c, other);
      /* recv.==(nil): user object may override ==; dispatch to its method.
         nil.==(obj): NilClass#== is identity-only, so false for any object. */
      if (a_nil && ty_is_object(ot)) goto equality_skip_nil;
      if (ot == TY_POLY) {
        buf_puts(b, eq ? "sp_poly_nil_p(" : "(!sp_poly_nil_p(");
        emit_expr(c, other, b); buf_puts(b, eq ? ")" : "))");
      }
      else if (ot == TY_NIL) buf_puts(b, eq ? "1" : "0");
      else if (ot == TY_INT) {
        /* a nullable int compares equal to nil iff it holds the sentinel;
           a plain int constant-folds to false */
        buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, ") %s SP_INT_NIL)", eq ? "==" : "!=");
      }
      else if (ot == TY_FLOAT) {
        /* a nullable float carries the NaN sentinel */
        buf_puts(b, eq ? "sp_float_is_nil(" : "(!sp_float_is_nil(");
        emit_expr(c, other, b); buf_puts(b, eq ? ")" : "))");
      }
      else if (ot == TY_STRING || ot == TY_MATCHDATA || ot == TY_STRINGIO || ot == TY_STRINGSCANNER ||
               ty_is_hash(ot) || ty_is_array(ot) || ot == TY_PROC || ot == TY_IO ||
               ot == TY_FIBER || ot == TY_EXCEPTION || ot == TY_REGEX) {
        /* nullable heap pointer: a NULL pointer encodes nil (a `@h = {}` slot is
           still NULL until assigned, so `@h == nil` must be a NULL test, not the
           always-false fallback below). */
        buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, ") %s 0)", eq ? "==" : "!=");
      }
      else { buf_puts(b, "(("); emit_expr(c, other, b); buf_printf(b, "), %d)", eq ? 0 : 1); }
      return;
    }
    equality_skip_nil:;
    /* arr == [] : an array equals the empty literal iff it has no elements */
    {
      int er = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ea = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if ((er && (array_kind(a0) || a0 == TY_POLY_ARRAY)) ||
          (ea && (array_kind(rt) || rt == TY_POLY_ARRAY))) {
        int arr = er ? argv[0] : recv;
        TyKind at = er ? a0 : rt;
        const char *kk = array_kind(at);
        buf_printf(b, "(%ssp_%sArray_length(", eq ? "" : "!", kk ? kk : "Poly");
        emit_expr(c, arr, b); buf_puts(b, ") == 0)");
        return;
      }
    }
    if (rt == TY_POLY_ARRAY && a0 == TY_POLY_ARRAY) {
      buf_puts(b, eq ? "sp_PolyArray_eq(" : "(!sp_PolyArray_eq(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    /* two typed arrays of the same kind: element-wise compare */
    if (array_kind(rt) && rt == a0) {
      if (!eq) buf_puts(b, "(!");
      buf_printf(b, "sp_%sArray_eq(", array_kind(rt));
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    /* poly array vs a typed array: box the typed side element-wise */
    if ((rt == TY_POLY_ARRAY && array_kind(a0)) || (a0 == TY_POLY_ARRAY && array_kind(rt))) {
      int polyn = rt == TY_POLY_ARRAY ? recv : argv[0];
      int typedn = rt == TY_POLY_ARRAY ? argv[0] : recv;
      TyKind tk = rt == TY_POLY_ARRAY ? a0 : rt;
      const char *kind = tk == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : tk == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      buf_puts(b, eq ? "sp_PolyArray_eq_typed(" : "(!sp_PolyArray_eq_typed(");
      emit_expr(c, polyn, b); buf_puts(b, ", (void *)("); emit_expr(c, typedn, b);
      buf_printf(b, "), %s)%s", kind, eq ? "" : ")");
      return;
    }
    /* hash == hash */
    if (ty_is_hash(rt) || ty_is_hash(a0) || rt == TY_UNKNOWN || a0 == TY_UNKNOWN) {
      /* two empty hash literals are trivially equal */
      int re = nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ae = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if (re && ae) { buf_puts(b, eq ? "1" : "0"); return; }
      if (ty_is_hash(rt) && ty_is_hash(a0)) {
        if (rt == a0) {
          /* same typed hash: use the dedicated equality function */
          const char *hn = ty_hash_cname(rt);
          if (hn) {
            buf_puts(b, eq ? "" : "(!");
            buf_printf(b, "sp_%sHash_eq(", hn);
            emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
            buf_puts(b, eq ? ")" : "))");
            return;
          }
        }
        /* different hash types can never be equal */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
      if (ty_is_hash(rt) || ty_is_hash(a0)) {
        /* hash vs non-hash */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
    }
    /* bigint == / != */
    if (rt == TY_BIGINT || a0 == TY_BIGINT) {
      buf_printf(b, "(sp_bigint_cmp(");
      if (rt != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (a0 != TY_BIGINT) { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", eq ? "==" : "!=");
      return;
    }
    /* a poly operand compares dynamically (covers string-vs-poly etc.) */
    if (rt == TY_POLY || a0 == TY_POLY) {
      buf_puts(b, eq ? "sp_poly_eq(" : "(!sp_poly_eq(");
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return;
    }
    {
      int fr = eq_family(rt), fa = eq_family(a0);
      /* same comparable family: compare by value */
      if (fr && fa && fr == fa) {
        if (fr == 2 && emit_strchar_cmp(c, recv, argv[0], eq, b)) return;
        if (fr == 2) { buf_puts(b, eq ? "sp_str_eq(" : "(!sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else if (fr == 5) { buf_puts(b, eq ? "sp_range_eq(" : "(!sp_range_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", eq ? "==" : "!="); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return;
      }
      /* two different concrete types are never == in Ruby (no coercion);
         still evaluate both operands for their side effects */
      if (fr && fa) {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return;
      }
    }
    /* object == / != : try direct method, then fall back to <=> == 0 */
    if (recv >= 0 && ty_is_object(rt)) {
      int ecid = ty_object_class(rt);
      int emi = comp_method_in_chain(c, ecid, name, NULL);
      if (emi >= 0) {
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (!strcmp(rty2, "LocalVariableReadNode") ||
                     !strcmp(rty2, "InstanceVariableReadNode") ||
                     !strcmp(rty2, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t2 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t2, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t2);
        }
        emit_dispatch(c, ecid, name, selfptr, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return;
      }
      /* no direct == : use <=> == 0 when the class supports Comparable */
      if (comp_method_in_chain(c, ecid, "<=>", NULL) >= 0) {
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (!strcmp(rty2, "LocalVariableReadNode") ||
                     !strcmp(rty2, "InstanceVariableReadNode") ||
                     !strcmp(rty2, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t3 = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t3, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t3);
        }
        buf_puts(b, "(");
        emit_dispatch(c, ecid, "<=>", selfptr, nt_ref(nt, id, "arguments"), -1, b);
        buf_printf(b, " %s 0)", eq ? "==" : "!=");
        return;
      }
      /* obj.!= synthesized from obj.== when != is not explicitly defined */
      if (!eq) {
        int eqm2 = comp_method_in_chain(c, ecid, "==", NULL);
        if (eqm2 >= 0) {
          char selfptr2[64];
          const char *rty3 = nt_type(nt, recv);
          if (rty3 && (!strcmp(rty3, "LocalVariableReadNode") ||
                       !strcmp(rty3, "InstanceVariableReadNode") ||
                       !strcmp(rty3, "SelfNode"))) {
            Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
            snprintf(selfptr2, sizeof selfptr2, "%s", rb.p ? rb.p : "");
            free(rb.p);
          }
          else {
            int t4 = ++g_tmp;
            Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
            emit_indent(g_pre, g_indent);
            emit_ctype(c, rt, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", t4, rb.p ? rb.p : "");
            free(rb.p);
            snprintf(selfptr2, sizeof selfptr2, "_t%d", t4);
          }
          buf_puts(b, "(!");
          emit_dispatch(c, ecid, "==", selfptr2, nt_ref(nt, id, "arguments"), -1, b);
          buf_puts(b, ")");
          return;
        }
      }
    }
    /* Time == / != via sp_time_cmp */
    if (rt == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, eq ? "==" : "!=");
      return;
    }
    /* cross-type: primitive vs user-object */
    if ((eq_family(rt) && ty_is_object(a0)) || (eq_family(a0) && ty_is_object(rt))) {
      TyKind obj_t = ty_is_object(a0) ? a0 : rt;
      int    obj_n = ty_is_object(a0) ? argv[0] : recv;
      TyKind prim_t = ty_is_object(a0) ? rt : a0;
      int    prim_n = ty_is_object(a0) ? recv : argv[0];
      int    obj_cid = ty_object_class(obj_t);
      int    eqm = comp_method_in_chain(c, obj_cid, "==", NULL);
      /* Numeric types delegate == to other.==(self) when types mismatch */
      if (ty_is_numeric(prim_t) && eqm >= 0) {
        Scope *ms = &c->scopes[eqm];
        int to2 = ++g_tmp;
        Buf ob2; memset(&ob2, 0, sizeof ob2); emit_expr(c, obj_n, &ob2);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, obj_t, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", to2, ob2.p ? ob2.p : ""); free(ob2.p);
        if (!eq) buf_puts(b, "(!");
        emit_method_cname(c, ms, b);
        buf_printf(b, "(_t%d, ", to2);
        /* Match the parameter type: if == expects TY_POLY, box the primitive */
        LocalVar *p1 = (ms->nparams > 0) ? scope_local(ms, ms->pnames[0]) : NULL;
        if (p1 && p1->type == TY_POLY) emit_boxed(c, prim_n, b);
        else emit_expr(c, prim_n, b);
        buf_puts(b, ")");
        if (!eq) buf_puts(b, ")");
        return;
      }
      /* other primitive types (string, symbol, bool) are strict: false */
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
      emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
      return;
    }
    /* object vs nil: identity/pointer comparison (Object#== fallback).
       A non-nullable TY_OBJECT pointer is never NULL, so obj==nil=false
       and obj!=nil=true. A nullable object also works correctly via NULL. */
    if ((ty_is_object(rt) && a0 == TY_NIL) || (rt == TY_NIL && ty_is_object(a0))) {
      int obj_n = ty_is_object(rt) ? recv : argv[0];
      buf_puts(b, "(");
      emit_expr(c, obj_n, b);
      buf_printf(b, " %s NULL)", eq ? "==" : "!=");
      return;
    }
    unsupported(c, id, "equality");
  }

  /* obj.is_a?/kind_of?/instance_of?(Class): resolved via sp_class_le for
     correctness with module includes; falls back to constant for builtins. */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) {
    const char *cn = nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "ConstantReadNode")
                     ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int cid = ty_object_class(rt);
      int target = comp_class_index(c, cn);
      if (target >= 0) {
        if (!strcmp(name, "instance_of?")) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), %d)", cid == target);
        }
        else {
          /* use sp_class_le_mod (via macro) so includes chain is checked */
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_class_le(((sp_Class){%d}),((sp_Class){%d})))", cid, target);
        }
        return;
      }
      else {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
        return;
      }
    }
    /* Dynamic klass argument typed as TY_CLASS: runtime sp_class_le check */
    if (comp_ntype(c, argv[0]) == TY_CLASS) {
      int cid = ty_object_class(rt);
      int k = ++g_tmp;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), ");
      buf_printf(b, "({ sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
      if (!strcmp(name, "instance_of?"))
        buf_printf(b, "((sp_Class){%d}).cls_id == _t%d.cls_id; }))", cid, k);
      else
        buf_printf(b, "sp_class_le(((sp_Class){%d}),_t%d); }))", cid, k);
      return;
    }
  }

  /* Struct instance methods (to_h / to_a / values / members / dig). */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    int is_to_a = (!strcmp(name, "to_a") || !strcmp(name, "values") || !strcmp(name, "deconstruct"));
    if (is_to_a && argc == 0) {
      int t = ++g_tmp; int rt2 = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->name, t, rb.p ? rb.p : "", rt2, rt2);
      for (int i = 0; i < sc->nivars; i++) {
        buf_printf(b, " sp_PolyArray_push(_t%d, ", rt2);
        Buf fb; memset(&fb, 0, sizeof fb); buf_printf(&fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
        emit_boxed_text(c, sc->ivar_types[i], fb.p, b); free(fb.p);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", rt2);
      free(rb.p);
      return;
    }
    if (!strcmp(name, "to_h") && argc == 0) {
      int block = nt_ref(nt, id, "block");
      int t = ++g_tmp, rh = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      TyKind res = comp_ntype(c, id);
      const char *hn = ty_hash_cname(res);
      if (!hn) hn = "SymPoly";
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);",
                 sc->name, t, rb.p ? rb.p : "", hn, rh, hn, rh);
      free(rb.p);
      if (block >= 0) {
        /* to_h { |k, v| [nk, nv] }: per member, bind k/v then set hash[nk] = nv */
        const char *kp = block_param_name(c, block, 0); if (kp) kp = rename_local(kp);
        const char *vp = block_param_name(c, block, 1); if (vp) vp = rename_local(vp);
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        int ke = -1, ve = -1;
        if (last >= 0 && nt_type(nt, last) && !strcmp(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) { ke = els[0]; ve = els[1]; }
        }
        TyKind kt = ty_hash_key(res), vt = ty_hash_val(res);
        for (int i = 0; i < sc->nivars; i++) {
          if (kp) buf_printf(b, " lv_%s = (sp_sym)%d;", kp, comp_sym_intern(c, sc->ivars[i] + 1));
          if (vp) {
            char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
            buf_printf(b, " lv_%s = ", vp); emit_boxed_text(c, sc->ivar_types[i], fb, b); buf_puts(b, ";");
          }
          buf_printf(b, " sp_%sHash_set(_t%d, ", hn, rh);
          if (ke >= 0) emit_expr(c, ke, b); else buf_puts(b, "0");
          buf_puts(b, ", ");
          if (ve >= 0) { if (vt == TY_POLY && comp_ntype(c, ve) != TY_POLY) emit_boxed(c, ve, b); else emit_expr(c, ve, b); }
          else buf_puts(b, "0");
          buf_puts(b, ");");
        }
      }
      else {
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, sc->ivars[i] + 1);
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");");
        }
      }
      buf_printf(b, " _t%d; })", rh);
      return;
    }
    if ((!strcmp(name, "members")) && argc == 0) {
      int rm = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
      for (int i = 0; i < sc->nivars; i++)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, sc->ivars[i] + 1));
      buf_printf(b, " _t%d; })", rm);
      return;
    }
    if (!strcmp(name, "dig") && argc >= 1) {
      /* literal key resolves a member at compile time */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && !strcmp(kty, "SymbolNode")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && !strcmp(kty, "IntegerNode")) {
        int v = (int)nt_int(nt, argv[0], "value", -1);
        if (v >= 0 && v < sc->nivars) mi = v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        char fld[300]; snprintf(fld, sizeof fld, "_t%d->iv_%s", t, sc->ivars[mi] + 1);
        TyKind mt = sc->ivar_types[mi];
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->name, t, rb.p ? rb.p : ""); free(rb.p);
        if (argc == 1) buf_puts(b, fld);
        else if (ty_is_hash(mt) && argc == 2) {
          const char *hn = ty_hash_cname(mt);
          buf_printf(b, "sp_%sHash_%s(%s, ", hn, ty_hash_val(mt) == TY_INT ? "get_opt" : "get", fld);
          emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (ty_is_array(mt) && argc == 2) {
          buf_printf(b, "sp_%sArray_get(%s, ", array_kind(mt), fld); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else buf_puts(b, fld);
        buf_puts(b, "; })");
        return;
      }
    }
    if (!strcmp(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int_literal]: return member value boxed to poly */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && !strcmp(kty, "SymbolNode") && nt_str(nt, argv[0], "value")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && !strcmp(kty, "IntegerNode")) {
        long long v = (long long)nt_int(nt, argv[0], "value", 0);
        if (v < 0) v += (long long)sc->nivars;
        if (v >= 0 && v < sc->nivars) mi = (int)v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->name, t, rb.p ? rb.p : ""); free(rb.p);
        buf_printf(b, "_t%d->iv_%s; })", t, sc->ivars[mi] + 1);
        return;
      }
      /* general: generate chain of comparisons */
      if (sc->nivars > 0) {
        int t = ++g_tmp, tk = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->name, t, rb.p ? rb.p : "", tk);
        free(rb.p);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ";");
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%dLL))){",
                     tk, comp_sym_intern(c, sc->ivars[i]+1), tk, (long long)i);
          char fld2[300]; snprintf(fld2, sizeof fld2, "_t%d->iv_%s", t, sc->ivars[i] + 1);
          emit_boxed_text(c, sc->ivar_types[i], fld2, b);
          buf_printf(b, ";} else");
        }
        buf_puts(b, " sp_box_nil(); })");
        return;
      }
    }
  }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* undef'd method: raise NoMethodError */
    if (comp_is_undeffed_in_chain(c, cid, name)) {
      TyKind ret_ty = comp_ntype(c, id);
      buf_printf(b, "(sp_raise_cls(\"NoMethodError\",\"undefined method '%s' for an instance of %s\"),%s)",
                 name, c->classes[cid].name,
                 ret_ty == TY_RANGE ? "(sp_Range){0}" : default_value(ret_ty));
      return;
    }
    /* attr reader -> field access (recv).iv_x */
    if (comp_reader_in_chain(c, cid, name, NULL)) {
      const char *rn2 = comp_resolve_alias(c, cid, name);
      buf_puts(b, "("); emit_expr(c, recv, b);
      buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", rn2);
      return;
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      /* a value-type receiver is passed by value; an ordinary object by
         pointer. For a value recv we hand emit_dispatch the value expression
         (lvalue or hoisted temp); the method takes `self` by value. */
      if (comp_ty_value_obj(c, rt)) {
        char selfv[64];
        const char *rty = nt_type(nt, recv);
        if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          snprintf(selfv, sizeof selfv, "%s", rb.p ? rb.p : ""); free(rb.p);
        }
        else {
          int t = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", t); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
          snprintf(selfv, sizeof selfv, "_t%d", t);
        }
        emit_dispatch(c, cid, name, selfv, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return;
      }
      /* receiver is a pointer; reuse it directly if it's a simple lvalue,
         else stash in a temp (the virtual-dispatch switch references it
         multiple times) */
      char selfptr[64];
      const char *rty = nt_type(nt, recv);
      if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
        free(rb.p);
      }
      else {
        int t = ++g_tmp;
        /* emit the receiver first so any setup it pushes into g_pre is fully
           flushed before we write this temp's declaration line */
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        /* Root the hoisted receiver: a freshly constructed object (e.g.
           Scene.new.render(...)) must survive any GC the call triggers. */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
        snprintf(selfptr, sizeof selfptr, "_t%d", t);
      }
      emit_dispatch(c, cid, name, selfptr, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
      return;
    }
  }

  /* Time instance methods: sp_Time is a value -- splice the receiver once. */
  if (recv >= 0 && rt == TY_TIME) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (!strcmp(name, "utc") || !strcmp(name, "gmtime") || !strcmp(name, "getutc")) buf_printf(b, "sp_time_utc(%s)", r);
    else if (!strcmp(name, "localtime") || !strcmp(name, "getlocal")) buf_printf(b, "sp_time_localtime(%s)", r);
    else if (!strcmp(name, "year"))  buf_printf(b, "sp_time_year(%s)", r);
    else if (!strcmp(name, "mon") || !strcmp(name, "month")) buf_printf(b, "sp_time_mon(%s)", r);
    else if (!strcmp(name, "day") || !strcmp(name, "mday"))  buf_printf(b, "sp_time_mday(%s)", r);
    else if (!strcmp(name, "hour")) buf_printf(b, "sp_time_hour(%s)", r);
    else if (!strcmp(name, "min"))  buf_printf(b, "sp_time_min(%s)", r);
    else if (!strcmp(name, "sec"))  buf_printf(b, "sp_time_sec(%s)", r);
    else if (!strcmp(name, "wday")) buf_printf(b, "sp_time_wday(%s)", r);
    else if (!strcmp(name, "yday")) buf_printf(b, "sp_time_yday(%s)", r);
    else if (!strcmp(name, "to_i") || !strcmp(name, "tv_sec")) buf_printf(b, "(%s).tv_sec", r);
    else if (!strcmp(name, "to_f")) buf_printf(b, "((mrb_float)(%s).tv_sec + (mrb_float)(%s).tv_nsec / 1e9)", r, r);
    else if (!strcmp(name, "subsec")) buf_printf(b, "((mrb_float)(%s).tv_nsec / 1e9)", r);
    else if (!strcmp(name, "tv_usec") || !strcmp(name, "usec")) buf_printf(b, "((mrb_int)(%s).tv_nsec / 1000)", r);
    else if (!strcmp(name, "tv_nsec") || !strcmp(name, "nsec")) buf_printf(b, "((mrb_int)(%s).tv_nsec)", r);
    else if (!strcmp(name, "utc?") || !strcmp(name, "gmt?")) buf_printf(b, "((%s).is_utc != 0)", r);
    else if (!strcmp(name, "dst?") || !strcmp(name, "isdst")) buf_printf(b, "(sp_time_isdst(%s) != 0)", r);
    else if (!strcmp(name, "utc_offset") || !strcmp(name, "gmt_offset") || !strcmp(name, "gmtoff")) buf_printf(b, "sp_time_utc_offset(%s)", r);
    else if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) buf_printf(b, "sp_time_inspect_v(%s)", r);
    else if (!strcmp(name, "iso8601")) buf_printf(b, "sp_time_iso8601(%s)", r);
    else if (!strcmp(name, "zone")) buf_printf(b, "sp_time_zone(%s)", r);
    else if (!strcmp(name, "class")) buf_puts(b, "SPL(\"Time\")");
    else if (!strcmp(name, "strftime") && argc == 1) { buf_printf(b, "sp_time_strftime(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((!strcmp(name, "+") || !strcmp(name, "-")) && argc == 1) {
      buf_printf(b, "sp_time_add(%s, %s(mrb_float)(", r, name[0] == '-' ? "-" : "");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else if ((!strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") ||
              !strcmp(name, ">=") || !strcmp(name, "==") || !strcmp(name, "!=")) && argc == 1) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
    }
    else if (!strcmp(name, "<=>") && argc == 1) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; (mrb_int)sp_time_cmp(_t%d, _t%d); })", tt, tu);
    }
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* StringScanner instance methods. String-returning methods may yield NULL
     (nil) on a miss; the NULL-aware string output operators render that. */
  if (recv >= 0 && rt == TY_STRINGSCANNER) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if ((!strcmp(name, "scan") || !strcmp(name, "check") || !strcmp(name, "scan_until")) &&
        argc == 1 && re_lit_index(c, argv[0]) >= 0) {
      buf_printf(b, "sp_StringScanner_%s(%s, sp_re_pat_%d)", name, r, re_lit_index(c, argv[0]));
    }
    else if (!strcmp(name, "matched")) buf_printf(b, "sp_StringScanner_matched(%s)", r);
    else if (!strcmp(name, "matched?")) buf_printf(b, "sp_StringScanner_matched_p(%s)", r);
    else if (!strcmp(name, "pre_match")) buf_printf(b, "sp_StringScanner_pre_match(%s)", r);
    else if (!strcmp(name, "post_match")) buf_printf(b, "sp_StringScanner_post_match(%s)", r);
    else if (!strcmp(name, "pos") || !strcmp(name, "charpos")) buf_printf(b, "sp_StringScanner_pos(%s)", r);
    else if (!strcmp(name, "pos=") && argc == 1) { buf_printf(b, "sp_StringScanner_pos_set(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "rest")) buf_printf(b, "sp_StringScanner_rest(%s)", r);
    else if (!strcmp(name, "rest?")) buf_printf(b, "sp_StringScanner_rest_p(%s)", r);
    else if (!strcmp(name, "rest_size")) buf_printf(b, "sp_StringScanner_rest_size(%s)", r);
    else if (!strcmp(name, "string")) buf_printf(b, "sp_StringScanner_string(%s)", r);
    else if (!strcmp(name, "eos?")) buf_printf(b, "sp_StringScanner_eos_p(%s)", r);
    else if (!strcmp(name, "getch")) buf_printf(b, "sp_StringScanner_getch(%s)", r);
    else if (!strcmp(name, "peek") && argc == 1) { buf_printf(b, "sp_StringScanner_peek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "[]") && argc == 1) { buf_printf(b, "sp_StringScanner_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "reset")) buf_printf(b, "(sp_StringScanner_reset(%s), %s)", r, r);
    else if (!strcmp(name, "terminate")) buf_printf(b, "(sp_StringScanner_terminate(%s), %s)", r, r);
    else if (!strcmp(name, "unscan")) buf_printf(b, "(sp_StringScanner_unscan(%s), %s)", r, r);
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* MatchData instance methods (sp_MatchData *, nullable on no-match). */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    if (!strcmp(name, "[]") && argc == 1) {
      buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "pre_match"))  buf_printf(b, "sp_MatchData_pre_match(%s)", r);
    else if (!strcmp(name, "post_match")) buf_printf(b, "sp_MatchData_post_match(%s)", r);
    else if (!strcmp(name, "to_s"))       buf_printf(b, "sp_MatchData_to_s(%s)", r);
    else if ((!strcmp(name, "length") || !strcmp(name, "size")) && argc == 0)
      buf_printf(b, "sp_MatchData_length(%s)", r);
    else if (!strcmp(name, "begin") && argc == 1) {
      buf_printf(b, "sp_MatchData_begin(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "end") && argc == 1) {
      buf_printf(b, "sp_MatchData_end(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "offset") && argc == 1) {
      buf_printf(b, "sp_MatchData_offset(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (!strcmp(name, "captures"))  buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (!strcmp(name, "to_a"))      buf_printf(b, "sp_MatchData_to_a(%s)", r);
    else if (!strcmp(name, "nil?"))      buf_printf(b, "(%s == 0)", r);
    else unsupported(c, id, "MatchData method");
    free(rs.p);
    return;
  }

  /* StringIO instance methods (a non-GC heap buffer behind sp_StringIO *). */
  if (recv >= 0 && rt == TY_STRINGIO) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (!strcmp(name, "string")) buf_printf(b, "sp_StringIO_string(%s)", r);
    else if (!strcmp(name, "pos") || !strcmp(name, "tell")) buf_printf(b, "sp_StringIO_pos(%s)", r);
    else if (!strcmp(name, "size") || !strcmp(name, "length")) buf_printf(b, "sp_StringIO_size(%s)", r);
    else if (!strcmp(name, "lineno")) buf_printf(b, "(%s)->lineno", r);
    else if (!strcmp(name, "puts") && argc == 0) buf_printf(b, "sp_StringIO_puts_empty(%s)", r);
    else if (!strcmp(name, "puts") && argc == 1) { buf_printf(b, "sp_StringIO_puts(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "print") && argc == 1) { buf_printf(b, "sp_StringIO_print(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if ((!strcmp(name, "write") || !strcmp(name, "<<")) && argc == 1) { buf_printf(b, "sp_StringIO_write(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "putc") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_STRING) { buf_printf(b, "sp_StringIO_putc(%s, (mrb_int)(unsigned char)(", r); emit_expr(c, argv[0], b); buf_puts(b, ")[0])"); }
      else { buf_printf(b, "sp_StringIO_putc(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    else if (!strcmp(name, "fsync") || !strcmp(name, "fileno") || !strcmp(name, "pid")) buf_printf(b, "((void)(%s), 0)", r);
    else if (!strcmp(name, "read") && argc == 0) buf_printf(b, "sp_StringIO_read(%s)", r);
    else if (!strcmp(name, "read") && argc == 1) { buf_printf(b, "sp_StringIO_read_n(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "gets")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_gets(%s))", r);
    else if (!strcmp(name, "getc")) buf_printf(b, "sp_box_nullable_str(sp_StringIO_getc(%s))", r);
    else if (!strcmp(name, "getbyte")) buf_printf(b, "sp_StringIO_getbyte(%s)", r);
    else if (!strcmp(name, "rewind")) buf_printf(b, "sp_StringIO_rewind(%s)", r);
    else if (!strcmp(name, "seek") && argc >= 1) { buf_printf(b, "sp_StringIO_seek(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "truncate") && argc == 1) { buf_printf(b, "sp_StringIO_truncate(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (!strcmp(name, "eof?") || !strcmp(name, "eof")) buf_printf(b, "sp_StringIO_eof_p(%s)", r);
    else if (!strcmp(name, "close")) buf_printf(b, "sp_StringIO_close(%s)", r);
    else if (!strcmp(name, "closed?")) buf_printf(b, "sp_StringIO_closed_p(%s)", r);
    else if (!strcmp(name, "flush")) buf_printf(b, "sp_StringIO_flush(%s)", r);
    else if (!strcmp(name, "sync")) buf_printf(b, "sp_StringIO_sync(%s)", r);
    else if (!strcmp(name, "isatty") || !strcmp(name, "tty?")) buf_printf(b, "sp_StringIO_isatty(%s)", r);
    else done = 0;
    free(rs.p);
    if (done) return;
  }

  /* Array-reduction methods on a boxed array element of a poly array (e.g.
     `runs.map { |r| r.sum }` over chunk_while runs). The runtime helper switches
     on the element's cls_id. Skipped when a user class defines the same method
     (it falls through to the general poly dispatch below). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0) {
    const char *pm = NULL;
    if (!strcmp(name, "sum")) pm = "sp_poly_sum";
    else if (!strcmp(name, "min")) pm = "sp_poly_min";
    else if (!strcmp(name, "max")) pm = "sp_poly_max";
    else if (!strcmp(name, "first")) pm = "sp_poly_first";
    else if (!strcmp(name, "last")) pm = "sp_poly_last";
    /* a Thread (Fiber-modelled) carried through a poly slot: #value/#resume/#join
       dispatch on the boxed Fiber when no user class defines the name (#1261). */
    else if (!strcmp(name, "value") || !strcmp(name, "resume")) pm = "sp_poly_fiber_value";
    else if (!strcmp(name, "join")) pm = "sp_poly_fiber_join";
    if (pm) {
      int ncand = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (comp_method_in_chain(c, k, name, NULL) >= 0) ncand++;
      if (ncand == 0) {
        buf_printf(b, "%s(", pm); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
    }
  }

  /* poly method dispatch: switch on the boxed object's cls_id and call the
     matching class's method (walking the chain for inherited methods),
     unboxing the pointer. */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    int is_lengthlike = !strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count");
    int is_empty = !strcmp(name, "empty?");
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL)) ncand++;
    if (ncand > 0 || is_lengthlike) {
      TyKind ret = comp_ntype(c, id);
      int tv = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
      emit_ctype(c, is_scalar_ret(ret) ? ret : TY_INT, b);
      buf_printf(b, " _t%d = %s; ", tr, is_scalar_ret(ret) ? default_value(ret) : "0");
      /* When the dispatch result feeds a poly context, tr is sp_RbVal, so the
         length-like int branches must box their integer result. */
      const char *bopen = (ret == TY_POLY) ? "sp_box_int(" : "";
      const char *bclose = (ret == TY_POLY) ? ")" : "";
      /* empty? answers a bool; box it (not an int) when the result feeds poly */
      const char *ebopen = (ret == TY_POLY) ? "sp_box_bool(" : "";
      const char *ebclose = (ret == TY_POLY) ? ")" : "";
      /* string/symbol-tagged poly values answer length/size directly */
      if (is_lengthlike) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %s(mrb_int)strlen(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ", tv, tr, bopen, tv, bclose);
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %s(mrb_int)sp_str_length(_t%d.v.s)%s; else ", tv, tr, bopen, tv, bclose);
      }
      /* a string/symbol-tagged poly value answers empty? directly (#1438) */
      if (is_empty) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %ssp_str_length(_t%d.v.s) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
        buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %sstrlen(sp_sym_to_s((sp_sym)_t%d.v.i)) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
      }
      buf_printf(b, "switch (_t%d.cls_id) {", tv);
      for (int k = 0; k < c->nclasses; k++) {
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        if (mi >= 0 && c->scopes[mi].nrequired == 0) {
          /* Build the call; append default values for any optional params
             not provided by the (zero-arg) call site. */
          Buf cb; memset(&cb, 0, sizeof cb);
          buf_printf(&cb, "sp_%s_%s((sp_%s *)_t%d.v.p",
                     c->classes[defcls].name, mc(c->scopes[mi].name), c->classes[defcls].name, tv);
          if (c->scopes[mi].nparams > 0) {
            const char *saved_self = g_self;
            static char selfpbuf[64];
            snprintf(selfpbuf, sizeof selfpbuf, "(sp_%s *)_t%d.v.p", c->classes[defcls].name, tv);
            g_self = selfpbuf;
            for (int a = 0; a < c->scopes[mi].nparams; a++) {
              buf_puts(&cb, ", "); emit_arg_or_default(c, &c->scopes[mi], a, -1, &cb);
            }
            g_self = saved_self;
          }
          buf_puts(&cb, ")");
          const char *call = cb.p ? cb.p : "";
          buf_printf(b, " case %d: ", k);
          if (method_is_void(&c->scopes[mi])) buf_puts(b, call);  /* void: no usable value */
          else {
            buf_printf(b, "_t%d = ", tr);
            if (ret == TY_POLY && c->scopes[mi].ret != TY_POLY) emit_boxed_text(c, c->scopes[mi].ret, call, b);
            else buf_puts(b, call);
          }
          buf_puts(b, "; break;");
          free(cb.p);
          continue;
        }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          const char *rn3 = comp_resolve_alias(c, k, name);
          char fld[600];
          snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[rdcls].name, tv, rn3);
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn3);
          int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
          buf_printf(b, " case %d: _t%d = ", k, tr);
          if (ret == TY_POLY && ivt != TY_POLY) emit_boxed_text(c, ivt, fld, b);
          else buf_puts(b, fld);
          buf_puts(b, "; break;");
        }
      }
      /* built-in array receivers reaching a length-like poly dispatch */
      if (!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) {
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY: _t%d = %ssp_IntArray_length((sp_IntArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_length((sp_StrArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_length((sp_FloatArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = %ssp_PolyArray_length((sp_PolyArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = %s((sp_PolyPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = %s((sp_SymPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = %s((sp_StrPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
      }
      /* built-in array / hash receivers reaching a poly empty? dispatch (#1438) */
      if (is_empty) {
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY: _t%d = %ssp_IntArray_length((sp_IntArray *)_t%d.v.p) == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_length((sp_StrArray *)_t%d.v.p) == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_length((sp_FloatArray *)_t%d.v.p) == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = %ssp_PolyArray_length((sp_PolyArray *)_t%d.v.p) == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = %s((sp_PolyPolyHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = %s((sp_SymPolyHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = %s((sp_StrPolyHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
      }
      buf_printf(b, " } _t%d; })", tr);
      return;
    }
  }

  /* poly method dispatch with arguments: switch on the boxed object's cls_id
     and call the matching user method (or a builtin array `[]`), passing the
     arguments evaluated once into temps. */
  if (recv >= 0 && rt == TY_POLY && argc > 0) {
    /* the builtin-array `[]` arm only applies to an integer index */
    int is_index = !strcmp(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_INT;
    int is_include = (!strcmp(name, "include?") || !strcmp(name, "member?") ||
                      !strcmp(name, "has_key?") || !strcmp(name, "key?")) && argc == 1;
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      /* Include if call supplies all required params (pad defaults / truncate extras) */
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand > 0 || is_index || is_include) {
      TyKind ret = comp_ntype(c, id);
      int tv = ++g_tmp, tr = ++g_tmp;
      int *atmp = malloc(sizeof(int) * argc);
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
      for (int a = 0; a < argc; a++) {
        atmp[a] = ++g_tmp;
        emit_ctype(c, infer_type(c, argv[a]), b);
        buf_printf(b, " _t%d = ", atmp[a]); emit_expr(c, argv[a], b); buf_puts(b, "; ");
      }
      emit_ctype(c, is_scalar_ret(ret) ? ret : TY_INT, b);
      buf_printf(b, " _t%d = %s; ", tr, is_scalar_ret(ret) ? default_value(ret) : "0");
      /* include? on a TAG_STR receiver: check tag before entering cls_id switch */
      if (is_include && infer_type(c, argv[0]) == TY_STRING)
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_str_include(_t%d.v.s, _t%d); } else ", tv, tr, tv, atmp[0]);
      /* Integer#[N] bit-extraction: poly recv may hold a tagged int */
      if (is_index) {
        if (ret == TY_POLY)
          buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = sp_box_int((_t%d.v.i >> _t%d) & 1); } else ", tv, tr, tv, atmp[0]);
        else
          buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = (_t%d.v.i >> _t%d) & 1; } else ", tv, tr, tv, atmp[0]);
      }
      buf_printf(b, "switch (_t%d.cls_id) {", tv);
      for (int k = 0; k < c->nclasses; k++) {
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        if (mi < 0 || argc < c->scopes[mi].nrequired) continue;
        TyKind mret = c->scopes[mi].ret;
        int mnp = c->scopes[mi].nparams;
        Buf cb; memset(&cb, 0, sizeof cb);
        buf_printf(&cb, "sp_%s_%s((sp_%s *)_t%d.v.p", c->classes[defcls].name,
                   mc(c->scopes[mi].name), c->classes[defcls].name, tv);
        const char *saved_self = g_self;
        static char selfpbuf2[64];
        snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_%s *)_t%d.v.p", c->classes[defcls].name, tv);
        for (int a = 0; a < mnp; a++) {
          /* box the call-site arg if this candidate's parameter is poly;
             emit default for args beyond the call-site count (padding) */
          TyKind pt = TY_UNKNOWN;
          LocalVar *pv = scope_local(&c->scopes[mi], c->scopes[mi].pnames[a]);
          if (pv) pt = pv->type;
          buf_puts(&cb, ", ");
          if (a < argc) {
            TyKind at = infer_type(c, argv[a]);
            char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
            if (pt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, tn, &cb);
            else buf_puts(&cb, tn);
          }
else {
            g_self = selfpbuf2;
            emit_arg_or_default(c, &c->scopes[mi], a, -1, &cb);
            g_self = saved_self;
          }
        }
        g_self = saved_self;
        buf_puts(&cb, ")");
        buf_printf(b, " case %d: ", k);
        if (mret == TY_VOID || mret == TY_NIL || method_is_void(&c->scopes[mi])) buf_puts(b, cb.p);  /* no usable value */
        else {
          buf_printf(b, "_t%d = ", tr);
          if (ret == TY_POLY && mret != TY_POLY) emit_boxed_text(c, mret, cb.p, b);
          else buf_puts(b, cb.p);
        }
        buf_puts(b, "; break;");
        free(cb.p);
      }
      if (is_index) {
        if (ret == TY_POLY) {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_box_int(sp_IntArray_get((sp_IntArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_box_str(sp_StrArray_get((sp_StrArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_box_float(sp_FloatArray_get((sp_FloatArray *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
        else {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_get((sp_IntArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
      }
      if (is_include) {
        TyKind at = infer_type(c, argv[0]);
        if (at == TY_INT) {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = sp_range_include((sp_Range *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
        else if (at == TY_STRING) {
          buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_StrArray_include((sp_StrArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_INT_HASH: _t%d = sp_StrIntHash_has_key((sp_StrIntHash *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_STR_HASH: _t%d = sp_StrStrHash_has_key((sp_StrStrHash *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = sp_StrPolyHash_has_key((sp_StrPolyHash *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
        else if (at == TY_SYMBOL) {
          /* sym array is stored as IntArray (sp_sym == mrb_int) */
          buf_printf(b, " case SP_BUILTIN_SYM_ARRAY: _t%d = sp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = sp_SymPolyHash_has_key((sp_SymPolyHash *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        }
        /* PolyArray: box the arg for runtime comparison */
        {
          int tbox = ++g_tmp;
          buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: { sp_RbVal _t%d = ", tbox);
          char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
          emit_boxed_text(c, at, tn, b);
          buf_printf(b, "; _t%d = sp_PolyArray_include((sp_PolyArray *)_t%d.v.p, _t%d); break; }", tr, tv, tbox);
        }
        /* PolyPolyHash: keys are boxed sp_RbVal */
        {
          int tbox = ++g_tmp;
          buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: { sp_RbVal _t%d = ", tbox);
          char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
          emit_boxed_text(c, at, tn, b);
          buf_printf(b, "; _t%d = sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, _t%d); break; }", tr, tv, tbox);
        }
      }
      /* the poly value may actually be a string-keyed hash: dispatch `[]` /
         `fetch` to the matching hash storage, boxing the value into the poly
         result. */
      int is_aref = !strcmp(name, "[]") && argc == 1;
      int is_fetch = !strcmp(name, "fetch") && (argc == 1 || argc == 2);
      if ((is_aref || is_fetch) && infer_type(c, argv[0]) == TY_STRING) {
        TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;  /* the result temp's type */
        static const struct { const char *cls, *hn; TyKind vt; } HV[] = {
          {"SP_BUILTIN_STR_STR_HASH", "StrStr", TY_STRING},
          {"SP_BUILTIN_STR_INT_HASH", "StrInt", TY_INT},
          {"SP_BUILTIN_STR_POLY_HASH", "StrPoly", TY_POLY},
        };
        for (unsigned hvi = 0; hvi < sizeof HV / sizeof HV[0]; hvi++) {
          /* only a variant whose value fits the result temp can be emitted */
          if (ret != TY_POLY && HV[hvi].vt != trt) continue;
          char getx[200];
          snprintf(getx, sizeof getx, "sp_%sHash_get((sp_%sHash *)_t%d.v.p, _t%d)", HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
          buf_printf(b, " case %s: _t%d = sp_%sHash_has_key((sp_%sHash *)_t%d.v.p, _t%d) ? ",
                     HV[hvi].cls, tr, HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
          if (ret == TY_POLY) emit_boxed_text(c, HV[hvi].vt, getx, b); else buf_puts(b, getx);
          buf_puts(b, " : ");
          if (is_fetch && argc == 2) {
            char dn[32]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
            if (ret == TY_POLY) emit_boxed_text(c, infer_type(c, argv[1]), dn, b); else buf_puts(b, dn);
          }
          else if (is_fetch) { buf_puts(b, "(sp_raise_cls(\"KeyError\", \"key not found\"), "); buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt)); buf_puts(b, ")"); }
          else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
          buf_puts(b, "; break;");
        }
      }
      /* a symbol-keyed hash (`{ name: ... }`) reaches here as SymPolyHash; add
         its `[]` / `fetch` arm so a Hash receiver indexed by a symbol is not
         dropped when a user class also defines an instance `[]` (#1437). */
      if ((is_aref || is_fetch) && infer_type(c, argv[0]) == TY_SYMBOL) {
        TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;
        char getx[200];
        snprintf(getx, sizeof getx, "sp_SymPolyHash_get((sp_SymPolyHash *)_t%d.v.p, _t%d)", tv, atmp[0]);
        buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = sp_SymPolyHash_has_key((sp_SymPolyHash *)_t%d.v.p, _t%d) ? ", tr, tv, atmp[0]);
        if (ret == TY_POLY) buf_puts(b, getx);
        else if (trt == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getx);
        else if (trt == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", getx);
        else buf_printf(b, "sp_poly_to_i(%s)", getx);
        buf_puts(b, " : ");
        if (is_fetch && argc == 2) {
          char dn[32]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
          if (ret == TY_POLY) emit_boxed_text(c, infer_type(c, argv[1]), dn, b); else buf_puts(b, dn);
        }
        else if (is_fetch) { buf_puts(b, "(sp_raise_cls(\"KeyError\", \"key not found\"), "); buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt)); buf_puts(b, ")"); }
        else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
        buf_puts(b, "; break;");
      }
      buf_printf(b, " } _t%d; })", tr);
      free(atmp);
      return;
    }
  }

  /* string-range literal methods: the int-only sp_Range struct can't hold
     string bounds, so inline strcmp / char-iteration for a literal
     `("a".."z")` receiver. */
  if (recv >= 0 && rt == TY_RANGE && nt_type(nt, unwrap_parens(c, recv)) &&
      !strcmp(nt_type(nt, unwrap_parens(c, recv)), "RangeNode")) {
    int rnode = unwrap_parens(c, recv);
    int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
    if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
      int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
      if ((!strcmp(name, "include?") || !strcmp(name, "member?") ||
           !strcmp(name, "cover?") || !strcmp(name, "===")) && argc == 1) {
        if (a0 != TY_STRING) {
          /* a non-string can't be in a string range: false (eval arg) */
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        }
        else {
          int ta = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", ta); emit_expr(c, argv[0], b);
          buf_puts(b, "; (strcmp("); emit_expr(c, lo, b); buf_printf(b, ", _t%d) <= 0 && strcmp(_t%d, ", ta, ta);
          emit_expr(c, hi, b); buf_printf(b, ") %s 0); })", excl ? "<" : "<=");
        }
        return;
      }
      if (!strcmp(name, "to_a") && argc == 0) {
        /* succ-based string range (handles multi-char: "aa".."ac" etc.) */
        buf_puts(b, "sp_StrArray_from_string_range("); emit_expr(c, lo, b);
        buf_puts(b, ", "); emit_expr(c, hi, b); buf_printf(b, ", %d)", excl);
        return;
      }
    }
  }

  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    int block = nt_ref(nt, id, "block");
    if (!strcmp(name, "step") && argc == 1) {
      int t = ++g_tmp, ar = ++g_tmp, ii = ++g_tmp, st = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, argv[0], &sb);
      buf_printf(b, "({ sp_Range _t%d = %s; mrb_int _t%d = %s; sp_IntArray *_t%d = sp_IntArray_new();"
                    " for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl; _t%d += _t%d)"
                    " sp_IntArray_push(_t%d, _t%d); _t%d; })",
                 t, rb.p ? rb.p : "", st, sb.p ? sb.p : "", ar,
                 ii, t, ii, t, t, ii, st, ar, ii, ar);
      free(rb.p); free(sb.p);
      return;
    }
    if (!strcmp(name, "each") && block < 0) {  /* enumerator: materialize to_a */
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      buf_printf(b, "({ sp_Range _t%d = %s; sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl); })",
                 t, rb.p ? rb.p : "", t, t, t);
      free(rb.p);
      return;
    }
    static const char *const rmeths[] = {
      "to_a", "include?", "member?", "cover?", "===", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end",
      "exclude_end?", "eql?", "minmax", "overlap?", NULL };
    int known = 0;
    for (int i = 0; rmeths[i]; i++) if (!strcmp(name, rmeths[i])) known = 1;
    if (known) {
      /* size/count on a string-literal range: no integer size -> nil, skip creating sp_Range */
      if ((!strcmp(name, "size") || !strcmp(name, "count")) && argc == 0) {
        int rn = unwrap_parens(c, recv);
        if (rn >= 0 && nt_type(nt, rn) && !strcmp(nt_type(nt, rn), "RangeNode")) {
          int lo = nt_ref(nt, rn, "left");
          if (lo >= 0 && comp_ntype(c, lo) == TY_STRING) {
            buf_puts(b, "SP_INT_NIL"); return;
          }
        }
      }
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      if (!strcmp(name, "to_a"))
        buf_printf(b, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", t, t, t);
      else if (!strcmp(name, "include?") || !strcmp(name, "member?") ||
               !strcmp(name, "cover?") || !strcmp(name, "===")) {
        /* cover?(range) checks that both endpoints of the arg fit inside self */
        if (!strcmp(name, "cover?") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
          int t2 = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.first >= _t%d.first && (_t%d.last - _t%d.excl) <= (_t%d.last - _t%d.excl); })",
                     t2, t, t2, t2, t, t);
        }
        else {
          /* sp_range_include takes mrb_int; a float arg (`(1..).include?(2.4)`)
             needs an explicit cast, else clang -Werror flags the implicit
             float-literal->int conversion (gcc truncates silently). */
          int arg_is_float = comp_ntype(c, argv[0]) == TY_FLOAT;
          buf_printf(b, "sp_range_include(&_t%d, ", t);
          if (arg_is_float) buf_puts(b, "(mrb_int)(");
          emit_expr(c, argv[0], b);
          if (arg_is_float) buf_puts(b, ")");
          buf_puts(b, ")");
        }
      }
      else if (!strcmp(name, "first") || !strcmp(name, "min") || !strcmp(name, "begin")) {
        if (argc == 1) {
          /* first(n): collect up to n elements starting at first */
          int tf = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); mrb_int _t%d = ", tf, tn);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl && _t%d - _t%d.first < _t%d; _t%d++)"
                        " sp_IntArray_push(_t%d, _t%d); _t%d; })",
                     ti, t, ti, t, t, ti, t, tn, ti, tf, ti, tf);
        }
        else buf_printf(b, "(_t%d.first)", t);
      }
      else if (!strcmp(name, "max"))  /* max element: end minus the exclusive bound */
        buf_printf(b, "(_t%d.last - _t%d.excl)", t, t);
      else if (!strcmp(name, "last") || !strcmp(name, "end")) {
        if (argc == 1 && !strcmp(name, "last")) {
          /* last(n): collect up to n elements ending at last */
          int tf = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = _t%d.last - _t%d.excl;", te, t, t);
          buf_printf(b, " mrb_int _t%d = _t%d - _t%d + 1; if (_t%d < _t%d.first) _t%d = _t%d.first;", ts, te, tn, ts, t, ts, t);
          buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); for (mrb_int _i%d = _t%d; _i%d <= _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _i%d); _t%d; })",
                     tf, tf, ts, tf, te, tf, tf, tf, tf);
        }
        else buf_printf(b, "(_t%d.last)", t);
      }
      else if (!strcmp(name, "size") || !strcmp(name, "count"))
        buf_printf(b, "(_t%d.last - _t%d.excl - _t%d.first + 1)", t, t, t);
      else if (!strcmp(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl), 0)", t, t, t);
      else if (!strcmp(name, "exclude_end?"))
        buf_printf(b, "(_t%d.excl != 0)", t);
      else if (!strcmp(name, "eql?")) {
        buf_printf(b, "sp_range_eq(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "overlap?")) {
        int t2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
        buf_printf(b, "; (_t%d.first <= _t%d.last - _t%d.excl && _t%d.first <= _t%d.last - _t%d.excl); })",
                   t, t2, t2, t2, t, t, t);
      }
      else if (!strcmp(name, "minmax")) {
        int ma = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, _t%d.first);"
                      " sp_IntArray_push(_t%d, _t%d.last - _t%d.excl); _t%d; })", ma, ma, t, ma, t, t, ma);
      }
      return;
    }
  }

  /* hash value methods */
  /* {}.default (empty hash literal with unknown type) always returns nil */
  if (recv >= 0 && !strcmp(name, "default") && argc == 0 && !ty_is_hash(rt)) {
    buf_puts(b, "sp_box_nil()");
    return;
  }
  if (recv >= 0 && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      /* Hash#to_proc: a Proc mapping a key to the hash value, closing over the
         hash. Emit a per-variant lookup fn matching the sp_proc_call ABI. */
      if (!strcmp(name, "to_proc") && argc == 0) {
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int pn = ++g_proc_counter;
        const char *keyexpr = (kt == TY_SYMBOL) ? "(sp_sym)args[0]"
                            : (kt == TY_STRING) ? "(const char *)(uintptr_t)args[0]"
                            : "args[0]";
        buf_printf(&g_proc_protos, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args);\n", pn);
        buf_printf(&g_procs, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args) {\n", pn);
        buf_printf(&g_procs, "  if (argc < 1) return 0;\n");
        buf_printf(&g_procs, "  sp_%sHash *_h = (sp_%sHash *)cap;\n", hn, hn);
        if (vt == TY_POLY) {
          if (!g_needs_proc_poly_retslot) {
            g_needs_proc_poly_retslot = 1;
            buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_ret;\n");
          }
          buf_printf(&g_procs, "  _sp_proc_poly_ret = sp_%sHash_get(_h, %s);\n  return 0;\n}\n", hn, keyexpr);
        }
        else if (vt == TY_STRING) {
          buf_printf(&g_procs, "  return (mrb_int)(uintptr_t)sp_%sHash_get(_h, %s);\n}\n", hn, keyexpr);
        }
        else {
          buf_printf(&g_procs, "  return (mrb_int)sp_%sHash_get(_h, %s);\n}\n", hn, keyexpr);
        }
        buf_printf(b, "sp_proc_new_meta((void *)_hashproc_%d, (void *)(", pn);
        emit_expr(c, recv, b);
        buf_puts(b, "), sp_hashproc_cap_scan, 1, FALSE, 1, NULL, NULL)");
        return;
      }
      if ((!strcmp(name, "dup") || !strcmp(name, "clone")) && argc == 0) {
        buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "[]") && argc == 1) {
        TyKind arg_kt = comp_ntype(c, argv[0]);
        TyKind hash_kt = ty_hash_key(rt);
        /* key type mismatch: sym key on str-keyed hash (or vice versa) -- the key
           can never exist in the hash, so always return the hash's default value.
           Exception: a symbol key on a string-keyed hash is coerced to its name
           (the Hash.new{} StrPolyHash model), so it is NOT a mismatch. */
        if (hash_kt != TY_POLY && hash_kt != TY_UNKNOWN &&
            arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt &&
            !(hash_kt == TY_STRING && arg_kt == TY_SYMBOL)) {
          TyKind vt = ty_hash_val(rt);
          int t = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b); buf_puts(b, "; ");
          if (vt == TY_INT) buf_printf(b, "_t%d ? _t%d->default_v : SP_INT_NIL; })", t, t);
          else if (vt == TY_STRING) buf_printf(b, "_t%d && _t%d->default_v ? _t%d->default_v : (&(\"\\xff\")[1]); })", t, t, t);
          else buf_printf(b, "_t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
          return;
        }
        if (rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "sp_%sHash_get(", hn);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* int-valued hashes have a nullable get_opt; string-valued use get */
          const char *getter = ty_hash_val(rt) == TY_INT ? "get_opt" : "get";
          buf_printf(b, "sp_%sHash_%s(", hn, getter);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], ty_hash_key(rt), b); buf_puts(b, ")");
        }
        return;
      }
      if (!strcmp(name, "dig") && argc >= 1) {
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, etc.) -> nil. */
        TyKind arg0t = comp_ntype(c, argv[0]);
        if ((kt == TY_SYMBOL && arg0t == TY_STRING) ||
            (kt == TY_STRING && arg0t == TY_SYMBOL)) {
          if (vt == TY_INT) buf_puts(b, "SP_INT_NIL");
          else if (vt == TY_STRING) buf_puts(b, "NULL");
          else buf_puts(b, "sp_box_nil()");
          return;
        }
        const char *getter = vt == TY_INT ? "get_opt" : "get";
        if (argc == 1) {
          buf_printf(b, "sp_%sHash_%s(", hn, getter);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], kt, b); buf_puts(b, ")");
        }
        else {
          /* multi-step dig: use a compound statement to guarantee
             left-to-right key-expression evaluation order. */
          int tr = ++g_tmp, th = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th);
          emit_expr(c, recv, b); buf_puts(b, ";");
          /* first key -> box to sp_RbVal so remaining steps are uniform */
          buf_printf(b, " sp_RbVal _t%d = ", tr);
          if (vt == TY_INT) {
            int tk0 = ++g_tmp;
            buf_printf(b, "({ mrb_int _t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_printf(b, "); _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d); });", tk0, tk0);
          }
          else if (vt == TY_STRING) {
            int tk0 = ++g_tmp;
            buf_printf(b, "({ const char *_t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_printf(b, "); _t%d ? sp_box_str(_t%d) : sp_box_nil(); });", tk0, tk0);
          }
          else {
            /* TY_POLY: getter already returns sp_RbVal */
            buf_printf(b, "sp_%sHash_%s(_t%d, ", hn, getter, th);
            emit_hash_key(c, argv[0], kt, b);
            buf_puts(b, ");");
          }
          /* remaining keys via sp_poly_get_sym / sp_poly_get_str / sp_poly_arr_get */
          for (int di = 1; di < argc; di++) {
            int tk = ++g_tmp;
            /* For poly-keyed hashes (e.g. PolyPolyHash), infer sub-key type
               from the argument itself rather than from the parent key type. */
            TyKind dkt = (kt == TY_POLY || kt == TY_UNKNOWN)
                         ? comp_ntype(c, argv[di]) : kt;
            if (dkt == TY_SYMBOL) {
              buf_printf(b, " sp_sym _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_sym(_t%d, _t%d);", tr, tr, tk);
            }
            else if (dkt == TY_STRING) {
              buf_printf(b, " const char *_t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_get_str(_t%d, _t%d);", tr, tr, tk);
            }
            else {
              buf_printf(b, " mrb_int _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tr, tr, tk);
            }
          }
          buf_printf(b, " _t%d; })", tr);
        }
        return;
      }
      if ((!strcmp(name, "values_at") || !strcmp(name, "fetch_values")) && argc >= 1) {
        /* collect looked-up values into a poly array; values_at yields nil for
           a missing key, fetch_values raises KeyError */
        int is_fetch = !strcmp(name, "fetch_values");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        for (int a = 0; a < argc; a++) {
          int tk = ++g_tmp;
          buf_printf(b, " %s _t%d = ", c_type_name(kt), tk); emit_hash_key(c, argv[a], kt, b); buf_puts(b, ";");
          buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) sp_PolyArray_push(_t%d, ", hn, th, tk, tr);
          char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          if (vt == TY_POLY) buf_puts(b, getexpr);
          else emit_boxed_text(c, vt, getexpr, b);
          buf_puts(b, ");");
          if (is_fetch) buf_puts(b, " else sp_raise_cls(\"KeyError\", \"key not found\");");
          else buf_printf(b, " else sp_PolyArray_push(_t%d, sp_box_nil());", tr);
        }
        buf_printf(b, " _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "fetch") && argc == 1) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* fetch(key) { default } -> has_key? ? get : block-default */
          TyKind vt = ty_hash_val(rt);
          int th = ++g_tmp, tk = ++g_tmp;
          buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
          buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          TyKind bvt = bval >= 0 ? comp_ntype(c, bval) : vt;
          /* When the block's return type differs from the hash value type,
             box both arms so the ternary produces a consistent sp_RbVal. */
          int mismatch = vt != TY_POLY && bvt != vt;
          if (mismatch) {
            buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? ", hn, th, tk);
            char getexpr[128]; snprintf(getexpr, sizeof getexpr, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
            emit_boxed_text(c, vt, getexpr, b);
            buf_puts(b, " : ({ ");
          }
else {
            buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : ({ ",
                       hn, th, tk, hn, th, tk);
          }
          const char *fp0 = block_param_name(c, blk, 0);  /* fetch yields the key */
          if (fp0) { buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), tk); }
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);  /* leading stmts */
          if (bval >= 0) {
            if ((vt == TY_POLY || mismatch) && bvt != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, (vt == TY_POLY || mismatch) ? "sp_box_nil()" : default_value(vt));
          buf_printf(b, "; }); })");
          return;
        }
        /* fetch(key) with no default raises KeyError on a miss */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d)"
                      " : (sp_raise_cls(\"KeyError\", \"key not found\"), %s); })",
                   hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        return;
      }
      if (!strcmp(name, "fetch") && argc == 2) {
        /* fetch(key, default) -> has_key? ? value : default */
        TyKind vt = ty_hash_val(rt);
        TyKind dt = comp_ntype(c, argv[1]);
        /* Empty `{}` default infers TY_UNKNOWN but is a hash — incompatible with int/str etc. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(c->nt, argv[1]);
          if (atn && (!strcmp(atn, "HashNode") || !strcmp(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        int needs_box = (vt != TY_POLY && ty_unify(vt, dt) == TY_POLY);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        if (needs_box) {
          buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? ", hn, th, tk);
          Buf _bx; memset(&_bx, 0, sizeof _bx);
          buf_printf(&_bx, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
          emit_boxed_text(c, vt, _bx.p, b);
          free(_bx.p);
          buf_puts(b, " : "); emit_boxed(c, argv[1], b);
        }
        else {
          buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : ", hn, th, tk, hn, th, tk);
          if (vt == TY_POLY && dt != TY_POLY) emit_boxed(c, argv[1], b);
          else emit_expr(c, argv[1], b);
        }
        buf_puts(b, "; })");
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0) {
        buf_printf(b, "sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if (!strcmp(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_clear(_t%d); _t%d; })", hn, t, t);
        return;
      }
      if ((!strcmp(name, "has_key?") || !strcmp(name, "key?") ||
           !strcmp(name, "include?") || !strcmp(name, "member?")) && argc == 1) {
        TyKind arg_kt = comp_ntype(c, argv[0]);
        TyKind hash_kt = ty_hash_key(rt);
        if (hash_kt != TY_POLY && arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt) {
          /* type mismatch (e.g. string arg on sym-keyed hash): always false */
          buf_puts(b, "0"); return;
        }
        buf_printf(b, "sp_%sHash_has_key(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], hash_kt, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "value?") || !strcmp(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH);
        buf_printf(b, "sp_%sHash_has_value(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (poly) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      /* Hash#key(value): the first key mapping to value (sym-keyed hash). */
      if (!strcmp(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) {
        buf_puts(b, "sp_SymPolyHash_key(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && comp_ntype(c, argv[0]) == rt) {
        buf_printf(b, "sp_%sHash_replace(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "default") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "; _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
        }
        else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
          buf_printf(b, "; (_t%d && _t%d->default_v != SP_INT_NIL) ? sp_box_int(_t%d->default_v) : sp_box_nil(); })", t, t, t);
        }
        else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
          buf_printf(b, "; (_t%d && _t%d->default_v) ? sp_box_str(_t%d->default_v) : sp_box_nil(); })", t, t, t);
        }
        else {
          buf_printf(b, "; (void)_t%d; sp_box_nil(); })", t);
        }
        return;
      }
      if (!strcmp(name, "default=") && argc == 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_boxed(c, argv[0], b); buf_puts(b, "; ");
        }
        else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_expr(c, argv[0], b); buf_puts(b, "; ");
        }
        else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
          buf_printf(b, "; if (_t%d) _t%d->default_v = ", t, t); emit_expr(c, argv[0], b); buf_puts(b, "; ");
        }
        emit_expr(c, argv[0], b); buf_puts(b, "; })"); return;
      }
      if (!strcmp(name, "keys") && argc == 0 && rt == TY_SYM_POLY_HASH) {
        /* runtime returns sym ids as an IntArray; box into a poly (sym) array */
        int ki = ++g_tmp, kp = ++g_tmp, ii = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_SymPolyHash_keys(", ki); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ki, kp, kp);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                      " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)sp_IntArray_get(_t%d, _t%d)));",
                   ii, ii, ki, ii, kp, ki, ii);
        buf_printf(b, " _t%d; })", kp);
        return;
      }
      if (!strcmp(name, "keys") && argc == 0) {
        buf_printf(b, "sp_%sHash_keys(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "values") && argc == 0 && rt != TY_INT_INT_HASH) {
        buf_printf(b, "sp_%sHash_values(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sHash_inspect(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
        /* merge(other) { |k, v1, v2| } -- conflict-resolution block. The
           result starts as a copy of the receiver, then each key of `other`
           is inserted; on a collision the block picks the value. */
        int blk = nt_ref(nt, id, "block");
        const char *bp0 = block_param_name(c, blk, 0);
        const char *bp1 = block_param_name(c, blk, 1);
        const char *bp2 = block_param_name(c, blk, 2);
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tc = ++g_tmp, tj = ++g_tmp;
        buf_printf(b, "({ %s _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", c_type_name(rt), tr, hn, tr);
        /* copy the receiver into the fresh result */
        buf_printf(b, " %s _t%d = ", c_type_name(rt), tc); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sHash_set(_t%d, _t%d->order[_t%d], sp_%sHash_get(_t%d, _t%d->order[_t%d]));",
                   tj, tj, tc, tj, hn, tr, tc, tj, hn, tc, tc, tj);
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
        buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
        buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) {", hn, tr, tk);
        if (bp0) buf_printf(b, " lv_%s = _t%d;", rename_local(bp0), tk);
        if (bp1) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp1), hn, tr, tk);
        if (bp2) buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d);", rename_local(bp2), hn, to, tk);
        buf_printf(b, " sp_%sHash_set(_t%d, _t%d, ", hn, tr, tk);
        {
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          buf_puts(b, "({ ");
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], b, 0);
          if (bval >= 0) {
            if (vt == TY_POLY && comp_ntype(c, bval) != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
          buf_puts(b, "; })");
        }
        buf_printf(b, "); } else { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); } }", hn, tr, tk, hn, to, tk);
        buf_printf(b, " _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH)) {
        TyKind at = comp_ntype(c, argv[0]);
        /* cross-variant str merge: promote both sides to str_poly_hash */
        if ((rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH) &&
            ty_is_hash(at) && ty_hash_key(at) == TY_STRING && at != rt) {
          buf_puts(b, "sp_StrPolyHash_merge(");
          const char *rfn = rt == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                                                   : "sp_StrPolyHash_from_str_str_hash(";
          buf_puts(b, rfn); emit_expr(c, recv, b); buf_puts(b, "), ");
          const char *afn = at == TY_STR_INT_HASH ? "sp_StrPolyHash_from_str_int_hash("
                          : at == TY_STR_STR_HASH  ? "sp_StrPolyHash_from_str_str_hash("
                                                   : NULL;
          if (afn) { buf_puts(b, afn); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          else { emit_expr(c, argv[0], b); }
          buf_puts(b, ")");
          return;
        }
        buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a str_poly receiver may be merged with a concrete str-keyed hash;
           coerce the argument to the receiver's variant first */
        if (rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(b, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (at == TY_POLY) {
          /* poly arg: unbox to the receiver's hash type */
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, argv[0], b);
          buf_printf(b, "; (sp_%sHash*)_t%d.v.p; })", hn, t);
        }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "invert") && argc == 0) {
        if (rt == TY_STR_STR_HASH) {
          buf_printf(b, "sp_StrStrHash_invert("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_STR_INT_HASH) {
          buf_printf(b, "sp_StrIntHash_invert_poly("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_INT_STR_HASH) {
          buf_printf(b, "sp_IntStrHash_invert("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else {
          /* generic: build PolyPolyHash by swapping key/value of each entry */
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          /* key and value access depend on the hash variant */
          TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
          /* emit key as sp_RbVal */
          if (kt == TY_SYMBOL)
            buf_printf(b, " sp_RbVal _k%d = sp_box_sym(_t%d->order[_t%d]);", ti, th, ti);
          else if (kt == TY_STRING)
            buf_printf(b, " sp_RbVal _k%d = sp_box_str(_t%d->order[_t%d]);", ti, th, ti);
          else if (kt == TY_INT)
            buf_printf(b, " sp_RbVal _k%d = sp_box_int(_t%d->order[_t%d]);", ti, th, ti);
          else
            buf_printf(b, " sp_RbVal _k%d = _t%d->keys[_t%d->order[_t%d]];", ti, th, th, ti);
          /* emit value as sp_RbVal */
          if (vt == TY_POLY)
            buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
          else if (vt == TY_INT) {
            buf_printf(b, " sp_RbVal _v%d = sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
          }
          else {
            buf_printf(b, " sp_RbVal _v%d = sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
          }
          buf_printf(b, " sp_PolyPolyHash_set(_t%d, _v%d, _k%d); }", tr, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        return;
      }
      if (!strcmp(name, "flatten") && argc <= 1) {
        /* interleave keys and values into a flat PolyArray */
        int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
        if (kt == TY_SYMBOL)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_STRING)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tr, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tr, th, th, ti);
        if (vt == TY_POLY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tr, hn, th, th, ti);
        else if (vt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        buf_printf(b, " } _t%d; })", tr);
        return;
      }
      if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && argc == 1) {
        /* find first pair where key==arg (assoc) or value==arg (rassoc); returns [k,v] or nil */
        int is_rassoc = !strcmp(name, "rassoc");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, ta = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b); buf_puts(b, ";");
        /* store argument */
        if (!is_rassoc) {
          buf_printf(b, " %s _t%d = ", c_type_name(kt), ta); emit_hash_key(c, argv[0], kt, b); buf_puts(b, ";");
        }
        else {
          /* rassoc: arg has value type */
          buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
        if (!is_rassoc) {
          /* assoc: compare key */
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(b, " if (sp_rbval_eql_key(_t%d->keys[_t%d->order[_t%d]], _t%d)) {", th, th, ti, ta);
          else if (kt == TY_STRING)
            buf_printf(b, " if (!strcmp(_t%d->order[_t%d], _t%d)) {", th, ti, ta);
          else
            buf_printf(b, " if (_t%d->order[_t%d] == _t%d) {", th, ti, ta);
        }
        else {
          /* rassoc: compare value (boxed) */
          buf_printf(b, " sp_RbVal _rv%d = ", ti);
          if (vt == TY_POLY) buf_printf(b, "sp_%sHash_get(_t%d, _t%d->order[_t%d]);", hn, th, th, ti);
          else if (vt == TY_INT) buf_printf(b, "sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", hn, th, th, ti);
          else buf_printf(b, "sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", hn, th, th, ti);
          buf_printf(b, " if (sp_poly_eq(_rv%d, _t%d)) {", ti, ta);
        }
        /* build pair */
        buf_printf(b, " _t%d = sp_PolyArray_new();", tr);
        if (kt == TY_SYMBOL)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_STRING)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tr, th, ti);
        else if (kt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tr, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tr, th, th, ti);
        if (vt == TY_POLY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tr, hn, th, th, ti);
        else if (vt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
        buf_printf(b, " break; } } _t%d; })", tr);  /* NULL = nil in poly context */
        return;
      }
      if (!strcmp(name, "compact") && argc == 0) {
        TyKind vt = ty_hash_val(rt);
        if (vt != TY_POLY) {
          /* Non-poly values can't be nil; compact is equivalent to dup */
          buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        else if (rt == TY_POLY_POLY_HASH) {
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          buf_printf(b, " sp_RbVal _v%d = _t%d->vals[_t%d->order[_t%d]];", ti, th, th, ti);
          buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_PolyPolyHash_set(_t%d, _t%d->keys[_t%d->order[_t%d]], _v%d); }", ti, tr, th, th, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        else {
          /* SYM_POLY_HASH or other poly-valued hash */
          int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", hn, tr, hn, tr);
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
          buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
          buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_%sHash_set(_t%d, _t%d->order[_t%d], _v%d); }", ti, hn, tr, th, ti, ti);
          buf_printf(b, " _t%d; })", tr);
        }
        return;
      }
      if (!strcmp(name, "delete") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_POLY_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; %s _t%d = sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : %s;",
                   c_type_name(vt), tv, hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        buf_printf(b, " sp_%sHash_delete(_t%d, _t%d); _t%d; })", hn, th, tk, tv);
        return;
      }
    }
  }

  /* `arr[i] = v` in expression position: do the store, evaluate to the rhs
     (Ruby []= returns the assigned value). The statement form is emitted
     elsewhere; this covers rvalue chains like `b = arr[i] = v`. */
  /* a[i, n] = src  —  slice assignment */
  if (recv >= 0 && ty_is_array(rt) && !strcmp(name, "[]=") && argc == 3) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    TyKind rhs_ty = comp_ntype(c, argv[2]);
    if (k && ty_is_array(rhs_ty)) {
      /* src is an array: copy elements from src into arr starting at i */
      int ta = ++g_tmp, ti = ++g_tmp, ts = ++g_tmp, tj = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
      buf_printf(b, "sp_%sArray *_t%d = ", k, ts); emit_expr(c, argv[2], b); buf_puts(b, "; ");
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", tj, tj, k, ts, tj);
      buf_printf(b, " sp_%sArray_set(_t%d, _t%d + _t%d, sp_%sArray_get(_t%d, _t%d));", k, ta, ti, tj, k, ts, tj);
      buf_printf(b, " _t%d; })", ts);
      return;
    }
    if (k && rhs_ty == TY_POLY && rt != TY_POLY_ARRAY) {
      /* poly RHS wrapping a same-kind array (e.g. PolyArray element is IntArray):
         unbox via v.p cast and copy elements */
      int ta3 = ++g_tmp, ti3 = ++g_tmp, ts3 = ++g_tmp, tj3 = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta3); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti3); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
      buf_printf(b, "sp_RbVal _tv%d = ", ts3); emit_expr(c, argv[2], b); buf_puts(b, "; ");
      buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)_tv%d.v.p; ", k, ts3, k, ts3);
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", tj3, tj3, k, ts3, tj3);
      buf_printf(b, " sp_%sArray_set(_t%d, _t%d + _t%d, sp_%sArray_get(_t%d, _t%d));", k, ta3, ti3, tj3, k, ts3, tj3);
      buf_printf(b, " _tv%d; })", ts3);
      return;
    }
    if (k && rhs_ty == TY_POLY && rt == TY_POLY_ARRAY) {
      /* poly_array recv + poly src (array at runtime): copy src elements to recv[start..] */
      int ta4 = ++g_tmp, tst4 = ++g_tmp, tsrc4 = ++g_tmp, tlen4 = ++g_tmp, tj4 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta4); emit_expr(c, recv, b); buf_puts(b, ";");
      buf_printf(b, " mrb_int _t%d = ", tst4); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
      buf_printf(b, " sp_RbVal _t%d = ", tsrc4); emit_expr(c, argv[2], b); buf_puts(b, ";");
      buf_printf(b, " mrb_int _t%d = sp_poly_arr_len(_t%d);", tlen4, tsrc4);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)", tj4, tj4, tlen4, tj4);
      buf_printf(b, " sp_PolyArray_set(_t%d, _t%d + _t%d, sp_poly_arr_get(_t%d, _t%d));", ta4, tst4, tj4, tsrc4, tj4);
      buf_printf(b, " _t%d; })", tsrc4);
      return;
    }
    if (k && !ty_is_array(rhs_ty)) {
      /* scalar RHS: set element at start index and return the scalar value */
      int ta2 = ++g_tmp, ti2 = ++g_tmp, tv2 = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta2); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti2); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
      if (rt == TY_POLY_ARRAY) {
        buf_printf(b, "sp_RbVal _t%d = ", tv2); emit_boxed(c, argv[2], b);
      }
      else {
        TyKind et2 = ty_array_elem(rt);
        emit_ctype(c, et2, b); buf_printf(b, " _t%d = ", tv2);
        if (rhs_ty == TY_POLY && et2 == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[2], b); buf_puts(b, ")"); }
        else if (rhs_ty == TY_POLY && et2 == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[2], b); buf_puts(b, ")"); }
        else if (rhs_ty == TY_POLY && et2 == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[2], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[2], b);
      }
      buf_printf(b, "; sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })", k, ta2, ti2, tv2, tv2);
      return;
    }
  }
  if (recv >= 0 && ty_is_array(rt) && !strcmp(name, "[]=") && argc == 2) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (k) {
      int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, "; ");
      if (rt == TY_POLY_ARRAY) {
        buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b);
      }
      else {
        TyKind et = ty_array_elem(rt);
        TyKind vt = comp_ntype(c, argv[1]);
        emit_ctype(c, et, b); buf_printf(b, " _t%d = ", tv);
        if (vt == TY_POLY && et == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vt == TY_POLY && et == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else if (vt == TY_POLY && et == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[1], b);
      }
      buf_printf(b, "; sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })", k, t, ti, tv, tv);
      return;
    }
  }

  /* array value methods */
  /* empty array literal [] has TY_UNKNOWN; sum returns init or 0 */
  if (recv >= 0 && rt == TY_UNKNOWN && !strcmp(name, "sum") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) {
      TyKind call_t = comp_ntype(c, id);
      if (argc == 1) {
        if (call_t == TY_POLY) emit_boxed(c, argv[0], b);
        else emit_expr(c, argv[0], b);
      }
      else {
        if (call_t == TY_POLY) buf_puts(b, "sp_box_int(0)");
        else buf_puts(b, "0");
      }
      return;
    }
  }
  /* take_while/drop_while/each_index/set-ops on empty array literal [] (TY_UNKNOWN receiver) */
  if (recv >= 0 && rt == TY_UNKNOWN &&
      (!strcmp(name, "take_while") || !strcmp(name, "drop_while") || !strcmp(name, "each_index") ||
       !strcmp(name, "difference") || !strcmp(name, "-") || !strcmp(name, "&") || !strcmp(name, "|") ||
       !strcmp(name, "intersection") || !strcmp(name, "union") || !strcmp(name, "+") ||
       !strcmp(name, "zip") || !strcmp(name, "flatten") || !strcmp(name, "compact") ||
       !strcmp(name, "uniq") || !strcmp(name, "sort") || !strcmp(name, "reverse") ||
       !strcmp(name, "shuffle")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) {
      if (!strcmp(name, "each_index")) {
        /* each_index on [] is a no-op; evaluate receiver for side-effects */
        emit_expr(c, recv, b);
      }
      else if (!strcmp(name, "take_while") || !strcmp(name, "drop_while")) {
        buf_puts(b, "sp_PolyArray_new()");
      }
      else {
        /* set/transform ops on [] receiver: call the runtime with NULL first arg */
        TyKind akt = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
        const char *ek = ty_is_array(akt) ? ((akt == TY_POLY_ARRAY) ? "Poly" : array_kind(akt)) : NULL;
        if (!ek) ek = "Poly";
        if (argc > 0 && akt != TY_UNKNOWN &&
            (!strcmp(name, "union") || !strcmp(name, "|") ||
             !strcmp(name, "difference") || !strcmp(name, "-") ||
             !strcmp(name, "intersection") || !strcmp(name, "&") ||
             !strcmp(name, "+") || !strcmp(name, "zip"))) {
          /* call the real function with NULL receiver (handles empty-self case) */
          const char *fn = (!strcmp(name, "&") || !strcmp(name, "intersection")) ? "intersect"
                         : (!strcmp(name, "|") || !strcmp(name, "union")) ? "union"
                         : (!strcmp(name, "+")) ? "concat"
                         : "difference";
          buf_printf(b, "sp_%sArray_%s(NULL, ", ek, fn); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          buf_printf(b, "sp_%sArray_new()", ek);
        }
      }
      return;
    }
  }
  if (recv >= 0 && ty_is_array(rt)) {
    if (!strcmp(name, "pack") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_POLY_ARRAY)) {
      buf_printf(b, "sp_%sArray_pack(", rt == TY_POLY_ARRAY ? "Poly" : "Int");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* values_at(i, j, ...) -> fresh same-kind array of the picked elements
       (works for typed and poly arrays alike, and range args) */
    if (!strcmp(name, "values_at") && argc >= 1) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); ", an, to, an);
        for (int a = 0; a < argc; a++) {
          TyKind at = comp_ntype(c, argv[a]);
          if (at == TY_RANGE) {
            int trng = ++g_tmp, ti = ++g_tmp;
            buf_printf(b, "{ sp_Range _t%d = ", trng); emit_expr(c, argv[a], b);
            buf_printf(b, "; for (mrb_int _t%d = _t%d.first; _t%d <= _t%d.last - _t%d.excl; _t%d++)"
                          " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d)); } ",
                       ti, trng, ti, trng, trng, ti, an, to, an, tr, ti);
          }
          else {
            buf_printf(b, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, ", an, to, an, tr);
            emit_expr(c, argv[a], b); buf_puts(b, ")); ");
          }
        }
        buf_printf(b, "_t%d; })", to);
        return;
      }
    }
    const char *k = array_kind(rt);
    /* drop(n) / take(n): subarrays via slice (all kinds incl. poly). */
    if ((!strcmp(name, "drop") || !strcmp(name, "take")) && argc == 1) {
      const char *dk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (dk) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", dk, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
        if (!strcmp(name, "take"))
          buf_printf(b, "; sp_%sArray_slice(_t%d, 0, _t%d); })", dk, t, tn);
        else
          buf_printf(b, "; sp_%sArray_slice(_t%d, _t%d, _t%d->len - _t%d); })", dk, t, tn, t, tn);
        return;
      }
    }
    /* poly-array max/min: boxed elements compared at runtime (numerics,
       strings, int-array tuples lexicographically). */
    if ((!strcmp(name, "max") || !strcmp(name, "min")) && argc == 0 &&
        rt == TY_POLY_ARRAY && nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    /* fill(val[, start[, len]]): fill a range with val, evaluate to self. */
    if (!strcmp(name, "fill") && argc >= 1 && argc <= 3) {
      const char *fk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (fk) {
        int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", fk, t); emit_expr(c, recv, b); buf_puts(b, "; ");
        emit_ctype(c, ty_array_elem(rt), b); buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d);", tn, fk, t);
        if (argc >= 2 && comp_ntype(c, argv[1]) == TY_RANGE) {
          /* fill(val, range): use range as index span */
          int tr = ++g_tmp, te = ++g_tmp;
          buf_printf(b, " sp_Range _t%d = ", tr); emit_expr(c, argv[1], b);
          buf_printf(b, "; mrb_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;",
                     ts, tr, ts, ts, tn, ts, ts);
          buf_printf(b, " mrb_int _t%d = _t%d.last - _t%d.excl;", te, tr, tr);
          buf_printf(b, " for (mrb_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, te, ti, fk, t, ti, tv, t);
        }
        else if (argc >= 2) {
          buf_printf(b, " mrb_int _t%d = ", ts); emit_expr(c, argv[1], b);
          buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
          if (argc == 3) {
            int tl = ++g_tmp;
            buf_printf(b, " mrb_int _t%d = ", tl); emit_expr(c, argv[2], b);
            /* end = start+len; negative len = no-op (empty range) */
            buf_printf(b, "; if (_t%d < 0) _t%d = 0; _t%d = _t%d + _t%d;",
                       tl, tl, tn, ts, tl);
          }
          buf_printf(b, " for (mrb_int _t%d = _t%d; _t%d < _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, tn, ti, fk, t, ti, tv, t);
        }
        else {
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ti, tn, ti, fk, t, ti, tv, t);
        }
        return;
      }
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_box_int(sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      TyKind init_t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_box_int(");
      if (init_t == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { emit_expr(c, argv[0], b); }
      buf_puts(b, " + sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (rt == TY_POLY_ARRAY && (!strcmp(name, "shift") || !strcmp(name, "pop")) && argc == 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_arr_get_hash(");
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")"); }
      }
      return;
    }
    /* each_index { |i| ... } - iterate with index (works for all array kinds) */
    {
      int ei_blk = nt_ref(nt, id, "block");
      if (!strcmp(name, "each_index") && ei_blk >= 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          const char *ip = block_param_name(c, ei_blk, 0); if (ip) ip = rename_local(ip);
          int body = nt_ref(nt, ei_blk, "body");
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, ek, trecv, ti);
          if (ip) {
            Scope *eic = comp_scope_of(c, ei_blk);
            LocalVar *eilv = eic ? scope_local(eic, ip) : NULL;
            TyKind eit = eilv ? eilv->type : TY_INT;
            emit_indent(g_pre, g_indent + 1);
            if (eit == TY_POLY)
              buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
            else
              buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
          }
          emit_stmts(c, body, g_pre, g_indent + 1);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return;
        }
      }
    }
    /* take_while / drop_while (works for typed and poly arrays alike) */
    if ((!strcmp(name, "take_while") || !strcmp(name, "drop_while")) && argc == 0
        && nt_ref(nt, id, "block") >= 0) {
      int is_drop = !strcmp(name, "drop_while");
      int tw_blk = nt_ref(nt, id, "block");
      const char *tw_bp = block_param_name(c, tw_blk, 0); if (tw_bp) tw_bp = rename_local(tw_bp);
      int tw_body = nt_ref(nt, tw_blk, "body");
      int tw_bn = 0; const int *tw_bb = tw_body >= 0 ? nt_arr(nt, tw_body, "body", &tw_bn) : NULL;
      if (tw_bn > 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", ek, tout, ek);
          if (is_drop) {
            emit_indent(g_pre, g_indent);
            buf_puts(g_pre, "{ mrb_bool _dropping = 1;\n");
          }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, ek, trecv, ti);
          if (tw_bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", tw_bp, ek, trecv, ti);
          }
          for (int j = 0; j < tw_bn - 1; j++) emit_stmt(c, tw_bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent = g_indent + 1;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, tw_bb[tw_bn - 1], &cb); g_indent = sv;
          if (is_drop) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (_dropping && !(%s)) _dropping = 0;\n", cb.p ? cb.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (!_dropping) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                       ek, tout, ek, trecv, ti);
          }
          else {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (!(%s)) break;\n", cb.p ? cb.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                       ek, tout, ek, trecv, ti);
          }
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (is_drop) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
          buf_printf(b, "_t%d", tout); return;
        }
      }
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "tally") && argc == 0) {
      buf_puts(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (rt == TY_POLY_ARRAY && !strcmp(name, "delete_at") && argc == 1) {
      buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (k) {
      if ((!strcmp(name, "to_a") || !strcmp(name, "to_ary") || !strcmp(name, "entries") ||
           !strcmp(name, "flatten") || !strcmp(name, "compact")) && argc == 0) {
        /* a scalar-element array can't nest or hold nil: these are identity */
        emit_expr(c, recv, b); return;
      }
      if (!strcmp(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_printf(b, "sp_%sArray_slice_range(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return;
      }
      if (!strcmp(name, "[]") && argc == 2) {
        /* arr[start, len] -> subarray */
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "[]") || !strcmp(name, "at")) && argc == 1) {
        buf_printf(b, "sp_%sArray_get(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (infer_type(c, argv[0]) == TY_POLY) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.v.i; })", t);
        }
else {
          emit_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "fetch") && (argc == 1 || argc == 2)) {
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_%sArray *_t%d = %s;", k, ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_%sArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, k, ta, tnorm);
        if (argc == 2) {
          buf_puts(b, " "); emit_expr(c, argv[1], b); buf_puts(b, "; })");
        }
        else {
          buf_printf(b, " (sp_raise_cls(\"IndexError\", \"index out of bounds\"), %s); })",
                     default_value(ty_array_elem(rt)));
        }
        return;
      }
      if (!strcmp(name, "dig") && argc >= 1) {
        if (argc == 1) {
          /* single-step: same as arr[i] */
          buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* multi-step: box the array as sp_RbVal, then chain sp_poly_arr_get */
          buf_printf(b, "sp_poly_arr_get_hash(");
          /* first step: box the typed array as obj, then get element i */
          int is_int = (rt == TY_INT_ARRAY);
          (void)is_int;
          /* build chain from innermost outward */
          for (int di = argc - 1; di >= 1; di--) {
            buf_printf(b, "sp_poly_arr_get_hash(");
          }
          /* first access: typed get then box */
          buf_printf(b, "sp_box_obj(");
          emit_expr(c, recv, b);
          buf_printf(b, ", SP_BUILTIN_%s_ARRAY)", rt == TY_INT_ARRAY ? "INT" : rt == TY_FLOAT_ARRAY ? "FLT" : "STR");
          buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
          for (int di = 1; di < argc; di++) {
            buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")");
          }
        }
        return;
      }
      if (!strcmp(name, "+") && argc == 1 && a0 == rt) {
        /* array + array of the same kind -> a fresh concatenation */
        buf_printf(b, "sp_%sArray_concat(", k);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "+") && argc == 1 && ty_is_array(a0) && a0 != rt) {
        /* array + different-kind array -> poly_array */
        const char *k2 = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
        if (k2) {
          int tL = ++g_tmp, tR = ++g_tmp, tO = ++g_tmp, ti = ++g_tmp;
          Buf lbuf; memset(&lbuf, 0, sizeof lbuf); emit_expr(c, recv, &lbuf);
          Buf rbuf; memset(&rbuf, 0, sizeof rbuf); emit_expr(c, argv[0], &rbuf);
          const char *box_l = (rt == TY_INT_ARRAY) ? "sp_box_int" :
                              (rt == TY_FLOAT_ARRAY) ? "sp_box_float" :
                              (rt == TY_STR_ARRAY) ? "sp_box_str" : NULL;
          const char *box_r = (a0 == TY_INT_ARRAY) ? "sp_box_int" :
                              (a0 == TY_FLOAT_ARRAY) ? "sp_box_float" :
                              (a0 == TY_STR_ARRAY) ? "sp_box_str" : NULL;
          const char *get_l = (rt == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                              NULL;
          const char *get_r = (a0 == TY_POLY_ARRAY) ? "sp_PolyArray_get" :
                              NULL;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = %s;\n", k, tL, lbuf.p ? lbuf.p : ""); free(lbuf.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sArray *_t%d = %s;\n", k2, tR, rbuf.p ? rbuf.p : ""); free(rbuf.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tO, tO);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k, tL, ti);
          emit_indent(g_pre, g_indent + 1);
          if (rt == TY_POLY_ARRAY)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tL, ti);
          else if (box_l)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_l, k, tL, ti);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k2, tR, ti);
          emit_indent(g_pre, g_indent + 1);
          if (a0 == TY_POLY_ARRAY)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tR, ti);
          else if (box_r)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_r, k2, tR, ti);
          buf_printf(b, "_t%d", tO);
          (void)get_l; (void)get_r;
          return;
        }
      }
      if (!strcmp(name, "clear") && argc == 0) {
        /* empty the array in place, evaluate to it (Ruby returns self) */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return;
      }
      if ((!strcmp(name, "shift") || !strcmp(name, "pop")) && argc == 0) {
        /* remove and return first/last element (nil sentinel when empty) */
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "unshift") && argc >= 1) {
        int t = ++g_tmp;
        if (rt == TY_INT_ARRAY) {
          buf_printf(b, "({ sp_IntArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = argc - 1; a >= 0; a--) {
            buf_printf(b, " sp_IntArray_unshift(_t%d, ", t); emit_expr(c, argv[a], b); buf_puts(b, ");");
          }
        }
        else if (rt == TY_STR_ARRAY) {
          buf_printf(b, "({ sp_StrArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = 0; a < argc; a++) {
            buf_printf(b, " sp_StrArray_insert(_t%d, %d, ", t, a); emit_expr(c, argv[a], b); buf_puts(b, ");");
          }
        }
        else {
          /* PolyArray/FloatArray: use insert-at-0 (PolyArray has no insert; push + rotate) */
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b); buf_puts(b, ";");
          /* fallthrough: just evaluate args for side effects */
          for (int a = 0; a < argc; a++) { buf_puts(b, " (void)("); emit_expr(c, argv[a], b); buf_puts(b, ");"); }
        }
        buf_printf(b, " _t%d; })", t);
        return;
      }
      /* non-mutating copy-then-operate methods */
      if (!strcmp(name, "shuffle") && argc == 0) {
        buf_printf(b, "sp_%sArray_shuffle(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      /* in-place mutators that return self (raise FrozenError when frozen) */
      {
        const char *base = NULL;
        if      (!strcmp(name, "reverse!")) base = "reverse_bang";
        else if (!strcmp(name, "sort!"))    base = "sort_bang";
        else if (!strcmp(name, "shuffle!")) base = "shuffle_bang";
        else if (!strcmp(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sArray_%s(_t%d); _t%d; })", k, base, t, t);
          return;
        }
      }
      if ((!strcmp(name, "dup") || !strcmp(name, "clone")) && argc == 0) {
        /* a real copy: arrays are mutable, so dup/clone must not alias. */
        buf_printf(b, "sp_%sArray_dup(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "reverse") && argc == 0) {
        /* copy + reverse in place; sp_*Array_dup exists for Int/Str/Float/Poly */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_dup(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); sp_%sArray_reverse_bang(_t%d); _t%d; })", k, t, t);
        return;
      }
      if (!strcmp(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        /* recv.zip(b, c...) → [[recv[0],b[0],c[0],...], ...] as PolyArray of PolyArrays */
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        const char *ka = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        buf_printf(b, "({ sp_%sArray *_t%d = ", ka, ta); emit_expr(c, recv, b); buf_puts(b, ";");
        for (int j = 0; j < nargs; j++) {
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {",
                   ti, ti, ka, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
        if (rt == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        for (int j = 0; j < nargs; j++) {
          if (at[j] == TY_INT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_STR_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_FLOAT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb[j], ti);
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "product") && argc == 1) {
        TyKind at = comp_ntype(c, argv[0]);
        const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
        int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
        emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
        buf_printf(b, "({ sp_%sArray *_t%d = %s; sp_%sArray *_t%d = %s;",
                   k, ta, ra.p ? ra.p : "NULL", kb, tb, rb2.p ? rb2.p : "NULL");
        free(ra.p); free(rb2.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
        buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
        if (rt == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else if (rt == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, ta, ti);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        if (at == TY_INT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else if (at == TY_STR_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else if (at == TY_FLOAT_ARRAY)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb, tj);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb, tj);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } } _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "repeated_combination") && argc == 1 && rt == TY_INT_ARRAY) {
        int ta = ++g_tmp, tc = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_IntArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_PtrArray *_t%d = sp_IntArray_repeated_combination(_t%d, ", tc, ta);
        emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tout, tout);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tc, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int_array(_t%d->data[_t%d]));", tout, tc, ti);
        buf_printf(b, " _t%d; })", tout);
        return;
      }
      if (!strcmp(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_rotate_bang(_t%d, ", k, t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && a0 == rt) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_replace(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "insert") && argc == 2 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_insert(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_printf(b, "); _t%d; })", t);
        return;
      }
      if (!strcmp(name, "delete_at") && argc == 1) {
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "delete") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_delete(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "tally") && argc == 0) {
        if (rt == TY_INT_ARRAY) { buf_printf(b, "sp_IntArray_tally_int("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
        if (rt == TY_STR_ARRAY) { buf_printf(b, "sp_StrArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
        if (rt == TY_POLY_ARRAY) { buf_printf(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      }
      if (!strcmp(name, "slice!") && argc == 2) {
        /* slice!(start, len): remove and return the subarray (raises
           FrozenError inside the runtime helper when the array is frozen) */
        buf_printf(b, "sp_%sArray_slice_bang(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      int block = nt_ref(nt, id, "block");
      /* bsearch { |x| cond } on typed arrays - find-minimum mode */
      if (!strcmp(name, "bsearch") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs; memset(&rbs, 0, sizeof rbs); emit_expr(c, recv, &rbs);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1;\n", tlo, thi, k, trecv);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tres, et == TY_INT ? "SP_INT_NIL" : "NULL");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                     cb.p ? cb.p : "0", tres, k, trecv, tmid, thi, tmid);
          free(cb.p);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return;
        }
      }
      /* find_index { |x| cond } on typed arrays - returns index or SP_INT_NIL */
      if (!strcmp(name, "find_index") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rfi; memset(&rfi, 0, sizeof rfi); emit_expr(c, recv, &rfi);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rfi.p ? rfi.p : "NULL"); free(rfi.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tres, tres);
          return;
        }
      }
      /* find / detect { |x| cond } - returns element or nil */
      if ((!strcmp(name, "find") || !strcmp(name, "detect")) && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          if (et == TY_STRING) buf_printf(g_pre, " _t%d = NULL;\n", tres);
          else if (et == TY_INT) buf_printf(g_pre, " _t%d = SP_INT_NIL;\n", tres);
          else buf_printf(g_pre, " _t%d = 0;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
          else buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                          cb.p ? cb.p : "0", tres, k, trecv, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return;
        }
      }
      /* map! / collect! { |x| body } - in-place transform, returns receiver */
      if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && block >= 0) {
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *ms = comp_scope_of(c, block);
          LocalVar *mlv = (ms && bp0) ? scope_local(ms, bp0) : NULL;
          TyKind msaved = mlv ? mlv->type : TY_UNKNOWN;
          if (mlv) { mlv->type = et; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, %s);\n", k, trecv, ti, vb.p ? vb.p : "0");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (mlv) mlv->type = msaved;
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond } - in-place filter */
      if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "keep_if") ||
           !strcmp(name, "reject!") || !strcmp(name, "delete_if")) && block >= 0) {
        int is_rej = !strcmp(name, "reject!") || !strcmp(name, "delete_if");
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *fs = comp_scope_of(c, block);
          LocalVar *flv = (fs && bp0) ? scope_local(fs, bp0) : NULL;
          TyKind fsaved = flv ? flv->type : TY_UNKNOWN;
          if (flv) { flv->type = et; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _telt%d = sp_%sArray_get(_t%d, _t%d);\n", ti, k, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = _telt%d;\n", bp, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (is_rej)
            buf_printf(g_pre, "if (!(%s)) { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                       cb.p ? cb.p : "0", k, trecv, twp, ti, twp);
          else
            buf_printf(g_pre, "if (%s) { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                       cb.p ? cb.p : "0", k, trecv, twp, ti, twp);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          if (flv) flv->type = fsaved;
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
           !strcmp(name, "none?") || !strcmp(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* scalar-element arrays never hold nil/false: predicate is length-based */
        const char *op = !strcmp(name, "all?") ? ">= 0" : !strcmp(name, "any?") ? "> 0"
                       : !strcmp(name, "none?") ? "== 0" : "== 1";
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_printf(b, ") %s)", op);
        return;
      }
      if ((!strcmp(name, "any?") || !strcmp(name, "none?") || !strcmp(name, "one?") || !strcmp(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* array.any?(v) / none?(v) / one?(v) / count(v) -- compare by == */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_%sArray *_t%d = %s;", k, ta, ra.p ? ra.p : "NULL"); free(ra.p);
        emit_indent(g_pre, 0);
        buf_printf(b, " "); emit_ctype(c, ty_array_elem(rt), b);
        buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", ti, ti, k, ta, ti);
        if (rt == TY_STR_ARRAY)
          buf_printf(b, " if (strcmp(sp_%sArray_get(_t%d, _t%d), _t%d) == 0) _t%d++;", k, ta, ti, tv, tc);
        else
          buf_printf(b, " if (sp_%sArray_get(_t%d, _t%d) == _t%d) _t%d++;", k, ta, ti, tv, tc);
        if (!strcmp(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
        else if (!strcmp(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (!strcmp(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } -- loop and count truthy block results */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_expr(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return;
        }
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if (!strcmp(name, "sum") && argc == 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        TyKind init_t = comp_ntype(c, argv[0]);
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_FLOAT_ARRAY && init_t == TY_INT) {
          buf_puts(b, "(mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (rt == TY_FLOAT_ARRAY && init_t == TY_POLY) {
          buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (rt == TY_INT_ARRAY && init_t == TY_POLY) {
          buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          emit_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "join") && argc <= 1) {
        buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sArray_inspect(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "first") && argc == 0) {
        buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "first") && argc == 1) {
        buf_printf(b, "sp_%sArray_slice(", k); emit_expr(c, recv, b); buf_puts(b, ", 0, ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "last") && argc == 1) {
        /* slice's negative start counts from the end -> the last n elements */
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_%sArray_slice(", k); emit_expr(c, recv, b);
        buf_printf(b, ", -_t%d, _t%d); })", tn, tn);
        return;
      }
      if (!strcmp(name, "pop") && argc == 0) {
        buf_printf(b, "sp_%sArray_pop(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "min") || !strcmp(name, "max")) && argc == 0 && rt != TY_STR_ARRAY) {
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "minmax") && argc == 0 && rt != TY_STR_ARRAY && block < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push(_t%d, sp_%sArray_min(_t%d));"
                      " sp_%sArray_push(_t%d, sp_%sArray_max(_t%d)); _t%d; })",
                   k, o, k, k, o, k, t, k, o, k, t, o);
        return;
      }
      if ((!strcmp(name, "index") || !strcmp(name, "find_index") || !strcmp(name, "rindex")) && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* nil-on-miss -> poly */
        const char *fn = !strcmp(name, "rindex") ? "rindex_poly" : "index_poly";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "include?") && argc == 1) {
        /* a typed array can never contain an element of an incompatible
           type (numeric vs string), so the answer is statically false;
           still evaluate both operands for any side effects. */
        int mismatch = 0;
        if (rt == TY_STR_ARRAY && a0 != TY_STRING && a0 != TY_UNKNOWN && a0 != TY_POLY) mismatch = 1;
        if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) &&
            a0 != TY_INT && a0 != TY_FLOAT && a0 != TY_UNKNOWN && a0 != TY_POLY) mismatch = 1;
        if (mismatch) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
          return;
        }
      }
      if ((!strcmp(name, "include?") || !strcmp(name, "member?") || !strcmp(name, "index") || !strcmp(name, "find_index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
        const char *fn = (!strcmp(name, "include?") || !strcmp(name, "member?")) ? "include" : "index";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "sort") && argc == 0 &&
          (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_sort(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "uniq") && argc == 0 && rt == TY_INT_ARRAY) {
        buf_puts(b, "sp_IntArray_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "%s _t%d = ", c_type_name(rt), t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1)", k, t, k, t);
        return;
      }
      if ((!strcmp(name, "&") || !strcmp(name, "intersection") ||
           !strcmp(name, "|") || !strcmp(name, "union") ||
           !strcmp(name, "-") || !strcmp(name, "difference")) && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        const char *fn = (!strcmp(name, "&") || !strcmp(name, "intersection")) ? "intersect" : ((!strcmp(name, "|") || !strcmp(name, "union")) ? "union" : "difference");
        /* empty literal [] arg: use a null pointer (safe for all sp_*Array_* set ops) */
        if (a0 == TY_UNKNOWN) { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", NULL)"); }
        else { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return;
      }
      if (!strcmp(name, "union") && argc == 0) {
        buf_printf(b, "sp_%sArray_union(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return;
      }
      if (!strcmp(name, "sample") && argc == 0) {
        buf_printf(b, "sp_%sArray_sample(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
    }
    /* poly (mixed-element) array methods: elements are boxed sp_RbVal */
    if (rt == TY_POLY_ARRAY) {
      if (!strcmp(name, "[]") && argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d) _t%d->len = 0; _t%d; })", t, t, t);
        return;
      }
      if (!strcmp(name, "+") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_concat("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "&") || !strcmp(name, "intersection") ||
           !strcmp(name, "|") || !strcmp(name, "union") ||
           !strcmp(name, "-") || !strcmp(name, "difference")) && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
        const char *fn = (!strcmp(name, "&") || !strcmp(name, "intersection")) ? "intersect" : (!strcmp(name, "|") || !strcmp(name, "union") ? "union" : "difference");
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")"); return;
      }
      if (!strcmp(name, "union") && argc == 0) {
        buf_puts(b, "sp_PolyArray_union("); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return;
      }
      if (!strcmp(name, "sample") && argc == 0) {
        buf_puts(b, "sp_PolyArray_sample("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
           !strcmp(name, "none?") || !strcmp(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* count truthy elements; a poly element may be nil/false */
        int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = 0; for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " if (sp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) _t%d++;",
                   tn, ti, ti, t, ti, t, ti, tn);
        const char *expr = !strcmp(name, "all?") ? "_t%d == sp_PolyArray_length(_t%d)"
                         : !strcmp(name, "any?") ? "_t%d > 0"
                         : !strcmp(name, "none?") ? "_t%d == 0" : "_t%d == 1";
        buf_puts(b, " (");
        if (!strcmp(name, "all?")) buf_printf(b, expr, tn, t);
        else buf_printf(b, expr, tn);
        buf_puts(b, "); })");
        return;
      }
      if ((!strcmp(name, "any?") || !strcmp(name, "none?") || !strcmp(name, "one?") || !strcmp(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* poly_array.one?(v) / any?(v) / none?(v) / count(v) */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) _t%d++;", ta, ti, tv, tc);
        if (!strcmp(name, "any?"))        buf_printf(b, " _t%d > 0; })", tc);
        else if (!strcmp(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (!strcmp(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return;
      }
      if ((!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "count")) && argc == 0
          && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } on PolyArray */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_expr(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return;
        }
      }
      if (!strcmp(name, "empty?") && argc == 0) {
        buf_puts(b, "(sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return;
      }
      if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc == 1) {
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "first") && argc == 0) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return;
      }
      if (!strcmp(name, "to_a") && argc == 0) { emit_expr(c, recv, b); return; }
      if (!strcmp(name, "fetch") && (argc == 1 || argc == 2)) {
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_PolyArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, ta, tnorm);
        if (argc == 2) {
          buf_puts(b, " "); emit_boxed(c, argv[1], b); buf_puts(b, "; })");
        }
        else {
          buf_printf(b, " (sp_raise_cls(\"IndexError\", \"index out of bounds\"), sp_box_nil()); })");
        }
        return;
      }
      if (!strcmp(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        Buf ra; memset(&ra, 0, sizeof ra); emit_expr(c, recv, &ra);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        for (int j = 0; j < nargs; j++) {
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new();", tpair);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        for (int j = 0; j < nargs; j++) {
          if (at[j] == TY_INT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_STR_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_StrArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else if (at[j] == TY_FLOAT_ARRAY)
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_float(sp_FloatArray_get(_t%d, _t%d)));", tpair, tb[j], ti);
          else
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb[j], ti);
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "last") && argc == 0) {
        int t = ++g_tmp;
        Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = ", t);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        buf_printf(b, "sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1)", t, t);
        return;
      }
      if (!strcmp(name, "include?") && argc == 1) {
        buf_puts(b, "sp_PolyArray_include("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "dup") && argc == 0) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "compact") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "compact!") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact_bang("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "flatten") && argc <= 1) {
        if (argc == 1) { buf_puts(b, "sp_PolyArray_flatten_n("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_PolyArray_flatten("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        return;
      }
      if (!strcmp(name, "transpose") && argc == 0) {
        buf_puts(b, "sp_int_array_transpose("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && argc == 1) {
        buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "join") && argc <= 1) {
        buf_puts(b, "sp_PolyArray_join("); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* the separator must be a const char*; a poly separator (e.g. a reader
           whose ivar widened to poly) is converted with sp_poly_to_s. */
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return;
      }
      if ((!strcmp(name, "inspect") || !strcmp(name, "to_s")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "slice!") && argc == 2) {
        buf_puts(b, "sp_PolyArray_slice_bang("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "shuffle") && argc == 0) {
        buf_puts(b, "sp_PolyArray_shuffle("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      if (!strcmp(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
      {
        const char *base = NULL;
        if      (!strcmp(name, "reverse!")) base = "reverse_bang";
        else if (!strcmp(name, "shuffle!")) base = "shuffle_bang";
        else if (!strcmp(name, "sort!"))    base = "sort_bang";
        else if (!strcmp(name, "uniq!"))    base = "uniq_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyArray_%s(_t%d); _t%d; })", base, t, t);
          return;
        }
      }
      if (!strcmp(name, "product") && argc == 1 && a0 == TY_POLY_ARRAY) {
        int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
        emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray *_t%d = %s;",
                   ta, ra.p ? ra.p : "NULL", tb, rb2.p ? rb2.p : "NULL");
        free(ra.p); free(rb2.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", tj, tj, tb, tj);
        buf_printf(b, " _t%d = sp_PolyArray_new();", tpair);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, ta, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));", tpair, tb, tj);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tpair);
        buf_printf(b, " } } _t%d; })", tr);
        return;
      }
      if (!strcmp(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray_rotate_bang(_t%d, ", t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return;
      }
      if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "keep_if") ||
           !strcmp(name, "reject!") || !strcmp(name, "delete_if")) && nt_ref(nt, id, "block") >= 0) {
        int is_rej = !strcmp(name, "reject!") || !strcmp(name, "delete_if");
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _telt%d = sp_PolyArray_get(_t%d, _t%d);\n", ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _telt%d;\n", bp, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          emit_cond(c, bb[bn - 1], g_pre);
          g_indent = sv;
          buf_printf(g_pre, ") { sp_PolyArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     trecv, twp, ti, twp);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          buf_printf(b, "_t%d", trecv); return;
        }
      }
      if (!strcmp(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        TyKind res = comp_ntype(c, id);
        const char *hn = ty_hash_cname(res);
        if (!hn) hn = "SymPoly";
        TyKind kty = ty_hash_key(res), vty = ty_hash_val(res);
        int tr = ++g_tmp, th = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", hn, th, hn, th);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
        buf_printf(b, " sp_PolyArray *_t%d = (sp_PolyArray *)sp_PolyArray_get(_t%d, _t%d).v.p;", tp, tr, ti);
        /* key extraction */
        buf_printf(b, " sp_%sHash_set(_t%d, ", hn, th);
        char kexpr[128];
        if (kty == TY_SYMBOL)      snprintf(kexpr, sizeof kexpr, "(sp_sym)sp_PolyArray_get(_t%d, 0).v.i", tp);
        else if (kty == TY_STRING) snprintf(kexpr, sizeof kexpr, "sp_PolyArray_get(_t%d, 0).v.s", tp);
        else                       snprintf(kexpr, sizeof kexpr, "sp_PolyArray_get(_t%d, 0).v.i", tp);
        buf_puts(b, kexpr); buf_puts(b, ", ");
        /* value extraction */
        if (vty == TY_POLY)        buf_printf(b, "sp_PolyArray_get(_t%d, 1)", tp);
        else if (vty == TY_INT)    buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.i", tp);
        else if (vty == TY_STRING) buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.s", tp);
        else if (vty == TY_FLOAT)  buf_printf(b, "sp_PolyArray_get(_t%d, 1).v.f", tp);
        else                       buf_printf(b, "sp_PolyArray_get(_t%d, 1)", tp);
        buf_printf(b, "); } _t%d; })", th);
        return;
      }
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "name")) {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (!strcmp(name, "inspect")) {
      buf_puts(b, "sp_str_concat(SPL(\":\"), sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (!strcmp(name, "to_sym") || !strcmp(name, "itself")) { emit_expr(c, recv, b); return; }
    /* case-folding methods return a (re-interned) symbol */
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "swapcase")) {
      buf_printf(b, "sp_sym_intern(sp_str_%s(sp_sym_to_s(", name); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (!strcmp(name, "length") || !strcmp(name, "size")) {
      buf_puts(b, "((mrb_int)strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (!strcmp(name, "empty?")) {
      buf_puts(b, "(strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")) == 0)");
      return;
    }
    if (!strcmp(name, "==") || !strcmp(name, "!=")) {
      buf_puts(b, name[0] == '=' ? "(" : "(!(");
      emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b);
      buf_puts(b, name[0] == '=' ? ")" : "))");
      return;
    }
  }

  /* boolean receiver methods */
  if (recv >= 0 && rt == TY_BOOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? SPL(\"true\") : SPL(\"false\"))");
      return;
    }
    if (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", name); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* str.each_char / each_line / chars / lines / bytes / codepoints { |x| ... } -> iterate, return self. */
  if (recv >= 0 && rt == TY_STRING && nt_ref(nt, id, "block") >= 0 &&
      (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "each_byte") ||
       !strcmp(name, "chars") || !strcmp(name, "lines") || !strcmp(name, "bytes") || !strcmp(name, "codepoints"))) {
    int block = nt_ref(nt, id, "block");
    int body = nt_ref(nt, block, "body");
    const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
    int ts = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    int is_line = !strcmp(name, "each_line") || !strcmp(name, "lines");
    int is_byte = !strcmp(name, "each_byte") || !strcmp(name, "bytes") || !strcmp(name, "codepoints");
    Scope *cs_ech = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *clv_ech = (p0 && cs_ech) ? scope_local(cs_ech, p0) : NULL;
    int p0_box_poly_ech = clv_ech && clv_ech->type == TY_POLY;
    buf_printf(b, "({ const char *_t%d = %s; ", ts, rb.p ? rb.p : ""); free(rb.p);
    /* Save outer variable before loop to restore it afterward */
    int tsv_ech = 0;
    if (p0 && clv_ech) {
      tsv_ech = ++g_tmp;
      Buf sv_ech; memset(&sv_ech, 0, sizeof sv_ech); emit_ctype(c, clv_ech->type, &sv_ech);
      buf_printf(b, "%s _t%d = lv_%s; ", sv_ech.p ? sv_ech.p : "sp_RbVal", tsv_ech, p0); free(sv_ech.p);
    }
    if (is_line) {
      int tl = ++g_tmp;
      /* chomp: true keyword arg uses the _chomp variant */
      int eline_chomp = 0;
      if (argc == 1 && argv && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int cv = struct_kwarg_value(c, argv[0], "chomp");
        eline_chomp = (cv >= 0 && nt_type(nt, cv) && !strcmp(nt_type(nt, cv), "TrueNode"));
      }
      buf_printf(b, "sp_StrArray *_t%d = %s(_t%d); for (mrb_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) { ",
                 tl, eline_chomp ? "sp_str_lines_chomp" : "sp_str_lines", ts, ti, ti, tl, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d)); ", p0, tl, ti);
        else buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d); ", p0, tl, ti);
      }
    }
    else if (is_byte) {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < (mrb_int)sp_str_byte_len(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_int((unsigned char)_t%d[_t%d]); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = (unsigned char)_t%d[_t%d]; ", p0, ts, ti);
      }
    }
    else {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_str_length(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_str_char_at_or_nil(_t%d, _t%d)); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = sp_str_char_at_or_nil(_t%d, _t%d); ", p0, ts, ti);
      }
    }
    int sv = g_nren; g_nren = 0;
    emit_stmts(c, body, b, 0);
    g_nren = sv;
    if (p0 && tsv_ech > 0) buf_printf(b, " lv_%s = _t%d;", p0, tsv_ech);
    buf_printf(b, " } _t%d; })", ts);
    return;
  }

  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT)) {
    Buf rs; memset(&rs, 0, sizeof rs);
    emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    /* A String-typed receiver that resolved to a poly nil -- e.g. an
       unresolvable chain like `Rails.application.class.to_s` in a method that
       is compiled but never called -- emits sp_box_nil(); coerce it to a
       const char* (yields "" at runtime) so the string ops below type-check. */
    if (rt == TY_STRING && !strcmp(r, "sp_box_nil()")) r = "sp_poly_to_s(sp_box_nil())";
    int handled = 1;

    if (rt == TY_STRING) {
      /* blockless "a".upto("c") materializes the succ-sequence as an array */
      if (!strcmp(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 0)");
      }
      /* string methods taking a regex-literal argument route to the engine */
      else if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2 &&
               nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
        Buf rp; memset(&rp, 0, sizeof rp);
        emit_regex_pat_to_buf(c, argv[0], &rp);
        buf_printf(b, "sp_re_%s(%s, %s, ", name, rp.p ? rp.p : "NULL", r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
        free(rp.p);
      }
      else if (!strcmp(name, "split") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               !re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "to_sym") || !strcmp(name, "intern")) buf_printf(b, "sp_sym_intern(%s)", r);
      else if (!strcmp(name, "length") || !strcmp(name, "size")) {
        if (g_hoist_len_var && g_hoist_len_recv && recv >= 0 && nt_type(nt, recv) &&
            !strcmp(nt_type(nt, recv), "LocalVariableReadNode") && nt_str(nt, recv, "name") &&
            !strcmp(nt_str(nt, recv, "name"), g_hoist_len_recv))
          buf_puts(b, g_hoist_len_var);
        else buf_printf(b, "sp_str_length(%s)", r);
      }
      else if (!strcmp(name, "bytesize")) buf_printf(b, "(mrb_int)sp_str_byte_len(%s)", r);
      else if (!strcmp(name, "upcase"))     buf_printf(b, "sp_str_upcase(%s)", r);
      else if (!strcmp(name, "downcase"))   buf_printf(b, "sp_str_downcase(%s)", r);
      else if (!strcmp(name, "capitalize")) buf_printf(b, "sp_str_capitalize(%s)", r);
      else if (!strcmp(name, "reverse"))    buf_printf(b, "sp_str_reverse(%s)", r);
      else if (!strcmp(name, "strip"))      buf_printf(b, "sp_str_strip(%s)", r);
      else if (!strcmp(name, "lstrip"))     buf_printf(b, "sp_str_lstrip(%s)", r);
      else if (!strcmp(name, "rstrip"))     buf_printf(b, "sp_str_rstrip(%s)", r);
      else if (!strcmp(name, "chomp") && argc == 1) {
        const char *a0ty = nt_type(nt, argv[0]);
        if (a0ty && !strcmp(a0ty, "NilNode")) {
          /* chomp(nil) returns the string unchanged */
          buf_puts(b, r);
        }
        else {
          buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
      }
      else if (!strcmp(name, "chomp"))      buf_printf(b, "sp_str_chomp(%s)", r);
      else if (!strcmp(name, "chop"))       buf_printf(b, "sp_str_chop(%s)", r);
      else if (!strcmp(name, "to_s") || !strcmp(name, "to_str")) buf_puts(b, r);
      else if ((!strcmp(name, "dup") || !strcmp(name, "clone")) && argc == 0) buf_printf(b, "sp_str_dup_external(%s)", r);
      else if (!strcmp(name, "inspect"))    { int tv = ++g_tmp; buf_printf(b, "({ const char *_t%d = %s; _t%d ? sp_str_inspect(_t%d) : SPL(\"nil\"); })", tv, r, tv, tv); }
      else if (!strcmp(name, "empty?"))     buf_printf(b, "(sp_str_length(%s) == 0)", r);
      else if (!strcmp(name, "include?") && argc == 1) {
        buf_printf(b, "sp_str_include(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "start_with?") && argc == 1) {
        buf_printf(b, "sp_str_start_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "end_with?") && argc == 1) {
        buf_printf(b, "sp_str_end_with(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "ascii_only?") && argc == 0) buf_printf(b, "sp_str_ascii_only(%s)", r);
      else if (!strcmp(name, "valid_encoding?") && argc == 0) buf_printf(b, "sp_str_valid_encoding(%s)", r);
      else if (!strcmp(name, "index") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_index_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "index") && argc == 1) {
        /* nil-on-miss carried as the SP_INT_NIL sentinel (a nullable int) */
        buf_printf(b, "sp_str_index_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "index") && argc == 2) {
        buf_printf(b, "sp_str_index_from_opt(%s, ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "partition") || !strcmp(name, "rpartition")) && argc == 1 &&
               re_lit_index(c, argv[0]) < 0) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "partition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* [before, match, after] from the first regex match, else [s, "", ""] */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_new();"
                      " if (sp_re_match(sp_re_pat_%d, %s) >= 0) {"
                      " sp_StrArray_push(_t%d, sp_re_match_pre); sp_StrArray_push(_t%d, sp_re_match_str);"
                      " sp_StrArray_push(_t%d, sp_re_match_post); } else {"
                      " sp_StrArray_push(_t%d, %s); sp_StrArray_push(_t%d, SPL(\"\")); sp_StrArray_push(_t%d, SPL(\"\")); }"
                      " _t%d; })",
                   tr, re_lit_index(c, argv[0]), r, tr, tr, tr, tr, r, tr, tr, tr);
      }
      else if (!strcmp(name, "rpartition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rpartition(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "rindex") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rindex_opt(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (!strcmp(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "crypt") && argc == 1) { buf_printf(b, "sp_str_crypt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "scrub") && argc == 0) buf_printf(b, "sp_str_scrub(%s, 0)", r);
      else if (!strcmp(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/] -> the matched substring, or nil (NULL) on no match */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1 && nt_type(c->nt, argv[0]) &&
               !strcmp(nt_type(c->nt, argv[0]), "RangeNode")) {
        /* s[a..b] / s[a...b]; beginless/endless ranges use 0 / length */
        int rn = argv[0];
        int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
        buf_printf(b, "sp_str_sub_range_r(%s, ", r);
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) { emit_expr(c, hi, b); buf_printf(b, ", %d)", excl); }
        else buf_printf(b, "(mrb_int)sp_str_length(%s), 0)", r);  /* endless: to the end */
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 2) {
        /* s[start, len] */
        buf_printf(b, "sp_str_sub_range(%s, ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((!strcmp(name, "[]") || !strcmp(name, "slice")) && argc == 1) {
        buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "split") && argc == 0) buf_printf(b, "sp_str_split_ws(%s)", r);
      else if (!strcmp(name, "split") && argc == 1) {
        /* split(nil) and split(" ") are whitespace-mode; split(sep) drops trailing empties */
        const char *aty = nt_type(c->nt, argv[0]);
        int nil_arg = aty && !strcmp(aty, "NilNode");
        int ws = nil_arg || (aty && !strcmp(aty, "StringNode") && nt_str(c->nt, argv[0], "content") &&
                 !strcmp(nt_str(c->nt, argv[0], "content"), " "));
        if (ws) buf_printf(b, "sp_str_split_ws(%s)", r);
        else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (!strcmp(name, "split") && argc == 2) {
        buf_printf(b, "sp_str_split_limit(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "clamp") && (argc == 2 ||
               (argc == 1 && nt_type(c->nt, argv[0]) && !strcmp(nt_type(c->nt, argv[0]), "RangeNode")))) {
        int lo_n, hi_n;
        if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
        else { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
        int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; const char *_t%d = ", tc, r, tlo); emit_expr(c, lo_n, b);
        buf_printf(b, "; const char *_t%d = ", thi); emit_expr(c, hi_n, b);
        buf_printf(b, "; strcmp(_t%d, _t%d) < 0 ? _t%d : (strcmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                   tc, tlo, tlo, tc, thi, thi, tc);
      }
      else if (!strcmp(name, "oct") && argc == 0) buf_printf(b, "sp_str_oct(%s)", r);
      else if (!strcmp(name, "hex") && argc == 0) buf_printf(b, "sp_str_to_i_base(%s, 16)", r);
      else if (!strcmp(name, "ord") && argc == 0) buf_printf(b, "sp_str_ord(%s)", r);
      else if ((!strcmp(name, "force_encoding") || !strcmp(name, "b") || !strcmp(name, "encode")) && argc <= 1) buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "encoding") && argc == 0) buf_printf(b, "((void)(%s), sp_box_encoding(sp_encoding_utf8()))", r);
      else if (!strcmp(name, "dump") && argc == 0) buf_printf(b, "sp_str_dump(%s)", r);
      else if (!strcmp(name, "undump") && argc == 0) buf_printf(b, "sp_str_undump(%s)", r);
      else if (!strcmp(name, "casecmp") && argc == 1) { buf_printf(b, "sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "casecmp?") && argc == 1) { buf_printf(b, "(sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ") == 0)"); }
      else if (!strcmp(name, "byteslice") && argc == 2) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "byteslice") && argc == 1) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
      else if (!strcmp(name, "setbyte") && argc == 2) { buf_printf(b, "sp_str_setbyte(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "getbyte") && argc == 1) {
        /* inline to a direct byte load (matches the legacy generator): the
           per-byte sp_str_getbyte call recomputes the string length and bounds
           every iteration, which the C compiler can't hoist across an aliasing
           setbyte. An out-of-range index reads adjacent bytes (as in legacy). */
        buf_printf(b, "((mrb_int)(unsigned char)(%s)[", r); emit_int_expr(c, argv[0], b); buf_puts(b, "])");
      }
      else if (!strcmp(name, "squeeze") && argc == 0) buf_printf(b, "sp_str_squeeze(%s)", r);
      else if (!strcmp(name, "squeeze") && argc == 1) { buf_printf(b, "sp_str_squeeze_chars(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "squeeze") && argc >= 2) {
        buf_printf(b, "sp_str_squeeze_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if ((!strcmp(name, "tr") || !strcmp(name, "tr_s")) && argc == 2) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "delete") && argc == 0) { buf_printf(b, "(%s)", r); return; }
      else if (!strcmp(name, "delete") && argc == 1) { buf_printf(b, "sp_str_delete(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "delete") && argc >= 2) {
        buf_printf(b, "sp_str_delete_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (!strcmp(name, "count") && argc == 0) { buf_printf(b, "(sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\"), 0LL)"); return; }
      else if (!strcmp(name, "count") && argc == 1) { buf_printf(b, "sp_str_count(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "count") && argc >= 2) {
        buf_printf(b, "sp_str_count_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (!strcmp(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (!strcmp(name, "lines") && argc == 1 && nt_type(nt, argv[0]) &&
               !strcmp(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
        int is_chomp = (chomp_v >= 0 && nt_type(nt, chomp_v) &&
                        !strcmp(nt_type(nt, chomp_v), "TrueNode"));
        buf_printf(b, "%s(%s)", is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", r);
      }
      else if (!strcmp(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (!strcmp(name, "codepoints") && argc == 0) buf_printf(b, "sp_str_codepoints(%s)", r);
      else if (!strcmp(name, "unpack") && argc == 1)  { buf_printf(b, "sp_str_unpack(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
      else if ((!strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) buf_printf(b, "sp_str_succ(%s)", r);
      else if (!strcmp(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
      else if (!strcmp(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "to_f") && argc == 0)    buf_printf(b, "atof(%s)", r);
      else if (!strcmp(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "sub") && argc == 2 && comp_ntype(c, argv[1]) == TY_STR_STR_HASH) {
        buf_printf(b, "sp_str_sub_str_str_hash(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "sub") && argc == 2) {
        buf_printf(b, "sp_str_sub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "center") && argc == 2) {
        buf_printf(b, "sp_str_center2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "ljust") && argc == 2) {
        buf_printf(b, "sp_str_ljust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "rjust") && argc == 2) {
        buf_printf(b, "sp_str_rjust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else handled = 0;
    }
    else if (rt == TY_INT) {
      /* a nullable int's to_s/inspect tests the value and converts it -- bind
         the receiver to a temp first so a side-effecting `r` (e.g. ARGF.read,
         a method call) is evaluated exactly once, not twice. */
      if (!strcmp(name, "to_s") && argc == 0) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (!strcmp(name, "inspect")) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (!strcmp(name, "to_f"))   buf_printf(b, "((mrb_float)(%s))", r);
      else if ((!strcmp(name, "to_i") || !strcmp(name, "to_int") || !strcmp(name, "floor") ||
                !strcmp(name, "ceil") || !strcmp(name, "round") || !strcmp(name, "truncate")) &&
               argc == 0) buf_printf(b, "(%s)", r);
      else if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
                !strcmp(name, "round") || !strcmp(name, "truncate")) && argc == 1) {
        buf_printf(b, "sp_int_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (!strcmp(name, "abs"))    buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "chr"))    buf_printf(b, "sp_int_chr(%s)", r);
      else if (!strcmp(name, "[]") && argc == 1) { buf_printf(b, "(((%s) >> (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) & 1)"); }
      else if (!strcmp(name, "even?"))  buf_printf(b, "((%s) %% 2 == 0)", r);
      else if (!strcmp(name, "odd?"))   buf_printf(b, "((%s) %% 2 != 0)", r);
      else if (!strcmp(name, "zero?"))  buf_printf(b, "((%s) == 0)", r);
      else if (!strcmp(name, "nonzero?")) buf_printf(b, "((%s) == 0 ? SP_INT_NIL : (%s))", r, r);
      else if (!strcmp(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (!strcmp(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (!strcmp(name, "divmod") && argc == 1) {
        int tb = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
      }
      else if (!strcmp(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "magnitude") && argc == 0) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "remainder") && argc == 1) { buf_printf(b, "((%s) %% (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (!strcmp(name, "size") && argc == 0) buf_puts(b, "((mrb_int)sizeof(mrb_int))");
      else if (!strcmp(name, "gcdlcm") && argc == 1) {
        int ta = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", ta); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_gcd(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_lcm(%s, _t%d)); _t%d; })", o, o, r, ta, o, r, ta, o);
      }
      else if (!strcmp(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "clamp") && argc == 1 && nt_type(c->nt, argv[0]) && !strcmp(nt_type(c->nt, argv[0]), "RangeNode")) {
        int rn = argv[0]; int tcr = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tcr); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_int_clamp(%s, _t%d.first, _t%d.last - _t%d.excl); })", r, tcr, tcr, tcr);
        (void)rn;
      }
      else if (!strcmp(name, "digits") && argc == 0) buf_printf(b, "sp_int_digits(%s, 10)", r);
      else if (!strcmp(name, "allbits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_printf(b, ")) == ("); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (!strcmp(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
      else if (!strcmp(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
      else if (!strcmp(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pow") && argc == 2) { buf_printf(b, "sp_powmod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pow") && argc == 1) { buf_printf(b, "sp_int_pow(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "pred") && argc == 0) buf_printf(b, "((%s) - 1)", r);
      else if ((!strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) buf_printf(b, "((%s) + 1)", r);
      else if (!strcmp(name, "to_s") && argc == 1) { buf_printf(b, "sp_int_to_s_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_FLOAT) {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (mrb_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new();"
                        " sp_IntArray_push(_t%d, _t%d);"
                        " sp_IntArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      else handled = 0;
    }
    else { /* TY_FLOAT */
      /* round/ceil/floor/truncate(n>0) -> Float to n decimals; else Integer */
      int ndig = 0;
      if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
           !strcmp(name, "round") || !strcmp(name, "truncate")) && argc == 1) {
        const char *aty = nt_type(c->nt, argv[0]);
        if (aty && !strcmp(aty, "IntegerNode")) ndig = (int)nt_int(c->nt, argv[0], "value", 0);
      }
      const char *cfn = !strcmp(name, "floor") ? "floor" : !strcmp(name, "ceil") ? "ceil"
                      : !strcmp(name, "truncate") ? "trunc" : "round";
      if ((!strcmp(name, "floor") || !strcmp(name, "ceil") ||
           !strcmp(name, "round") || !strcmp(name, "truncate"))) {
        if (ndig > 0)
          buf_printf(b, "({ double _f = pow(10, %d); %s((%s) * _f) / _f; })", ndig, cfn, r);
        else if (ndig < 0)  /* round to a power of ten left of the decimal -> Integer */
          buf_printf(b, "({ double _f = pow(10, %d); (mrb_int)(%s((%s) / _f) * _f); })", -ndig, cfn, r);
        else
          buf_printf(b, "((mrb_int)%s(%s))", cfn, r);
      }
      else if (!strcmp(name, "to_i"))  buf_printf(b, "((mrb_int)(%s))", r);
      else if (!strcmp(name, "to_f"))  buf_printf(b, "(%s)", r);
      else if (!strcmp(name, "divmod") && argc == 1) {
        /* Float#divmod(n) -> [floor(x/n) (Integer), x - q*n (Float)] */
        int tx = ++g_tmp, tn = ++g_tmp, tq = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_float _t%d = (%s); mrb_float _t%d = ", tx, r, tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (isnan(_t%d) || isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " mrb_int _t%d = (mrb_int)floor(_t%d / _t%d); sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float(_t%d - (mrb_float)_t%d * _t%d)); _t%d; })",
                   tx, tn, tn, tq, tx, tn, o, o, tq, o, tx, tq, tn, o);
      }
      else if (!strcmp(name, "to_s"))    buf_printf(b, "sp_float_opt_to_s(%s)", r);
      else if (!strcmp(name, "inspect")) buf_printf(b, "sp_float_opt_inspect(%s)", r);
      else if (!strcmp(name, "abs"))   buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "zero?")) buf_printf(b, "((%s) == 0.0)", r);
      else if (!strcmp(name, "nan?"))  buf_printf(b, "(isnan(%s) != 0)", r);
      else if (!strcmp(name, "finite?")) buf_printf(b, "(isfinite(%s) != 0)", r);
      else if (!strcmp(name, "infinite?")) buf_printf(b, "(isinf(%s) ? ((%s) > 0 ? 1LL : -1LL) : SP_INT_NIL)", r, r);
      else if (!strcmp(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (!strcmp(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (!strcmp(name, "next_float")) buf_printf(b, "nextafter(%s, INFINITY)", r);
      else if (!strcmp(name, "prev_float")) buf_printf(b, "nextafter(%s, -INFINITY)", r);
      else if (!strcmp(name, "magnitude")) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (!strcmp(name, "modulo") && argc == 1) { buf_printf(b, "fmod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (!strcmp(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        int ta = ++g_tmp, o = ++g_tmp;
        if (a0 == TY_INT) {
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, (mrb_float)_t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else {
          buf_printf(b, "({ mrb_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      else handled = 0;
    }
    free(rs.p);
    if (handled) return;
  }

  /* bigint methods */
  if (recv >= 0 && rt == TY_BIGINT) {
    Buf rs; memset(&rs, 0, sizeof rs); emit_expr(c, recv, &rs);
    const char *r = rs.p ? rs.p : "";
    if ((!strcmp(name, "to_s") || !strcmp(name, "inspect")) && argc == 0) {
      buf_printf(b, "sp_bigint_to_s(%s)", r); free(rs.p); return;
    }
    if (!strcmp(name, "to_i") && argc == 0) {
      buf_printf(b, "sp_bigint_to_int(%s)", r); free(rs.p); return;
    }
    if (!strcmp(name, "to_f") && argc == 0) {
      buf_printf(b, "((mrb_float)sp_bigint_to_int(%s))", r); free(rs.p); return;
    }
    free(rs.p);
  }

  /* Fiber[:k] = v (expression form) */
  if (!strcmp(name, "[]=") && argc == 2 && recv >= 0) {
    int is_fiber2 = 0;
    const char *rty3 = nt_type(nt, recv);
    if (rty3 && !strcmp(rty3, "ConstantReadNode")) {
      const char *rn3 = nt_str(nt, recv, "name");
      if (rn3 && !strcmp(rn3, "Fiber")) is_fiber2 = 1;
    }
    else if (rty3 && !strcmp(rty3, "CallNode")) {
      const char *rn3 = nt_str(nt, recv, "name");
      int rr3 = nt_ref(nt, recv, "receiver");
      if (rn3 && !strcmp(rn3, "current") && rr3 >= 0) {
        const char *rrty3 = nt_type(nt, rr3);
        const char *rrn3 = nt_str(nt, rr3, "name");
        if (rrty3 && !strcmp(rrty3, "ConstantReadNode") && rrn3 && !strcmp(rrn3, "Fiber"))
          is_fiber2 = 1;
      }
    }
    if (is_fiber2) {
      TyKind fvt = comp_ntype(c, argv[1]);
      /* Fiber storage is poly-valued. A nil/void/untyped value has no scalar
         C slot -- carry it boxed (`void _t = nil` is otherwise a type error). */
      int fval_poly = (fvt == TY_POLY || fvt == TY_UNKNOWN || fvt == TY_NIL || fvt == TY_VOID);
      int tf = ++g_tmp;
      buf_puts(b, "({ ");
      emit_ctype(c, fval_poly ? TY_POLY : fvt, b);
      buf_printf(b, " _t%d = ", tf);
      if (fval_poly) emit_boxed(c, argv[1], b);
      else emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_Fiber_storage_set(sp_fiber_current, ");
      emit_expr(c, argv[0], b);
      buf_puts(b, ", ");
      if (!fval_poly) {
        char tfs[32]; snprintf(tfs, sizeof tfs, "_t%d", tf);
        emit_boxed_text(c, fvt, tfs, b);
      }
      else buf_printf(b, "_t%d", tf);
      buf_printf(b, "); _t%d; })", tf);
      return;
    }
  }

  /* `[]=` in expression position: mutate and return the assigned value.
     Ruby's `(h[k] = v)` and `(a[i] = v)` evaluate to v. */
  if (!strcmp(name, "[]=") && argc == 2 && recv >= 0) {
    TyKind vt = comp_ntype(c, argv[1]);
    if (ty_is_hash(rt)) {
      const char *hn = ty_hash_cname(rt);
      if (hn) {
        int tv = ++g_tmp;
        int is_poly_hash = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH);
        TyKind hvt = ty_hash_val(rt);
        /* A poly value into a typed-value hash (e.g. a String? guarded non-nil
           stored into a Hash[String, String]): unbox to the value type. */
        int unbox_poly_val = (!is_poly_hash && vt == TY_POLY &&
                              (hvt == TY_STRING || hvt == TY_INT || hvt == TY_FLOAT));
        buf_puts(b, "({ ");
        /* For poly hashes with scalar values, store the scalar and box it for the hash call. */
        TyKind decl_type = unbox_poly_val ? hvt
                         : (is_poly_hash && vt != TY_UNKNOWN && vt != TY_POLY) ? vt
                         : (vt != TY_UNKNOWN ? vt : TY_POLY);
        emit_ctype(c, decl_type, b);
        buf_printf(b, " _t%d = ", tv);
        /* When the slot is poly but the rhs has no type yet (e.g. `{}`),
           emit a boxed value so the sp_RbVal temp initialises correctly. */
        if (unbox_poly_val) {
          const char *fn = hvt == TY_STRING ? "sp_poly_to_s" : hvt == TY_INT ? "sp_poly_to_i" : "sp_poly_to_f";
          buf_printf(b, "%s(", fn); emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (decl_type == TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr(c, argv[1], b);
        buf_printf(b, "; if (sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")) sp_raise_frozen_hash(); ");
        buf_printf(b, "sp_%sHash_set(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ", ");
        char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
        if (is_poly_hash && vt != TY_POLY) {
          emit_boxed_text(c, decl_type, tvn, b);
        }
        else {
          buf_printf(b, "_t%d", tv);
        }
        /* For poly-hash receivers the expression returns the boxed value
           (sp_RbVal); for typed-hash receivers return the raw typed value. */
        if (is_poly_hash) {
          buf_puts(b, "); ");
          if (decl_type == TY_POLY) buf_printf(b, "_t%d; })", tv);
          else { Buf _bx; memset(&_bx, 0, sizeof _bx); emit_boxed_text(c, decl_type, tvn, &_bx); buf_printf(b, "%s; })", _bx.p ? _bx.p : tvn); free(_bx.p); }
        }
        else if (unbox_poly_val) {
          /* value was poly (the `[]=` expression's value type); box the
             unboxed temp back so the expression result stays poly. */
          buf_puts(b, "); "); emit_boxed_text(c, hvt, tvn, b); buf_puts(b, "; })");
        }
        else {
          buf_printf(b, "); _t%d; })", tv);
        }
        return;
      }
    }
    if (ty_is_array(rt) || rt == TY_POLY_ARRAY) {
      const char *k = rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt);
      if (k) {
        int tv = ++g_tmp;
        buf_puts(b, "({ ");
        emit_ctype(c, vt != TY_UNKNOWN ? vt : TY_POLY, b);
        buf_printf(b, " _t%d = ", tv);
        if (rt == TY_POLY_ARRAY && vt != TY_POLY) emit_boxed(c, argv[1], b);
        else emit_expr(c, argv[1], b);
        buf_printf(b, "; sp_%sArray_set(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
        return;
      }
    }
  }

  /* $stderr.puts / $stderr.print: emit to stderr */
  if (recv >= 0 && argc >= 0 && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gvnm = nt_str(nt, recv, "name");
    if (gvnm && (!strcmp(gvnm, "$stderr") || !strcmp(gvnm, "$stdout"))) {
      int is_err = gvnm[1] == 's' && gvnm[2] == 't' && gvnm[3] == 'd' && gvnm[4] == 'e';
      const char *fd = is_err ? "stderr" : "stdout";
      if (!strcmp(name, "puts") || !strcmp(name, "print")) {
        int want_nl = !strcmp(name, "puts");
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
        return;
      }
      if (!strcmp(name, "flush")) { buf_printf(b, "fflush(%s)", fd); return; }
    }
  }
  /* Last-resort fallbacks for inspect/to_s on unresolved receivers.
     The test array_unresolved_inspect_no_segv expects "[]" when an
     unsupported method chains into inspect. Emit a safe nil-degrade
     rather than aborting the compiler. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "inspect")) {
    buf_puts(b, "\"[]\""); return;
  }
  if (recv >= 0 && argc == 0 && !strcmp(name, "to_s")) {
    buf_puts(b, "\"\""); return;
  }
  /* nil? on an object type: a value-type object is never nil; a heap object
     reference is nil exactly when its pointer is NULL. */
  if (recv >= 0 && argc == 0 && !strcmp(name, "nil?") && ty_is_object(rt)) {
    if (comp_ty_value_obj(c, rt)) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)"); }
    return;
  }

  /* dispatch user-defined methods on reopened built-in types */
  if (recv >= 0) {
    const char *oc_cn = NULL;
    if (rt == TY_STRING)       oc_cn = "String";
    else if (rt == TY_INT)     oc_cn = "Integer";
    else if (rt == TY_FLOAT)   oc_cn = "Float";
    else if (rt == TY_SYMBOL)  oc_cn = "Symbol";
    if (oc_cn) {
      int oc_ci = comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) {
          buf_printf(b, "sp_%s_%s(", oc_cn, mc(name));
          emit_expr(c, recv, b);
          emit_args_filled(c, oc_mi, nt_ref(nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          return;
        }
      }
    }
    /* bool: dispatch based on value to correct TrueClass/FalseClass impl */
    if (rt == TY_BOOL) {
      int tc_ci = comp_class_index(c, "TrueClass");
      int fc_ci = comp_class_index(c, "FalseClass");
      int tc_mi = tc_ci >= 0 ? comp_method_in_chain(c, tc_ci, name, NULL) : -1;
      int fc_mi = fc_ci >= 0 ? comp_method_in_chain(c, fc_ci, name, NULL) : -1;
      if (tc_mi >= 0 && fc_mi >= 0) {
        /* both defined: ternary dispatch */
        int bt = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = ", bt); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(_t%d ? sp_TrueClass_%s(_t%d", bt, mc(name), bt);
        emit_args_filled(c, tc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_printf(b, ") : sp_FalseClass_%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "))");
        return;
      }
      if (tc_mi >= 0) {
        /* only TrueClass defined */
        buf_printf(b, "sp_TrueClass_%s(", mc(name));
        emit_expr(c, recv, b);
        emit_args_filled(c, tc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, ")");
        return;
      }
      if (fc_mi >= 0) {
        /* only FalseClass defined: ternary still needed */
        int bt = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = ", bt); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(_t%d ? (", bt);
        buf_printf(b, "sp_FalseClass_%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "), 0) : sp_FalseClass_");
        buf_printf(b, "%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "))");
        return;
      }
    }
    /* Array reopening: any array-typed receiver -> box to sp_RbVal */
    if (ty_is_array(rt)) {
      int oc_ci2 = comp_class_index(c, "Array");
      if (oc_ci2 >= 0) {
        int oc_mi2 = comp_method_in_chain(c, oc_ci2, name, NULL);
        if (oc_mi2 >= 0) {
          const char *box_fn = (rt == TY_INT_ARRAY) ? "sp_box_int_array" :
                               (rt == TY_STR_ARRAY) ? "sp_box_str_array" :
                               (rt == TY_FLOAT_ARRAY) ? "sp_box_float_array" : "sp_box_poly_array";
          buf_printf(b, "sp_Array_%s(", mc(name));
          buf_printf(b, "%s(", box_fn); emit_expr(c, recv, b); buf_puts(b, ")");
          emit_args_filled(c, oc_mi2, nt_ref(nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          return;
        }
      }
    }
    /* Object reopening: universal fallback -> box receiver to sp_RbVal */
    {
      int oc_ci3 = comp_class_index(c, "Object");
      if (oc_ci3 >= 0) {
        int oc_mi3 = comp_method_in_chain(c, oc_ci3, name, NULL);
        if (oc_mi3 >= 0) {
          buf_printf(b, "sp_Object_%s(", mc(name));
          emit_boxed(c, recv, b);
          emit_args_filled(c, oc_mi3, nt_ref(nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          return;
        }
      }
    }
  }

  /* Mutex/Monitor#synchronize { block }: run block inline (single-threaded) */
  if (!strcmp(name, "synchronize") && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    int bdy = nt_ref(nt, blk, "body");
    int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
    TyKind res = comp_ntype(c, id);
    int scalar = is_scalar_ret(res) && res != TY_VOID && res != TY_NIL && res != TY_UNKNOWN;
    int rv = ++g_tmp;
    buf_puts(b, "({ ");
    for (int k = 0; k < bbn - 1; k++) emit_stmt(c, bbb[k], b, 0);
    if (bbn > 0) {
      TyKind lty = comp_ntype(c, bbb[bbn-1]);
      const char *lnty = nt_type(nt, bbb[bbn-1]);
      int nil_lit = (lty == TY_NIL && lnty && !strcmp(lnty, "NilNode"));
      int can_expr = (lty != TY_VOID && lty != TY_UNKNOWN && (lty != TY_NIL || nil_lit));
      if (scalar && can_expr) {
        emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
        if (res == TY_POLY && lty != TY_POLY) emit_boxed(c, bbb[bbn-1], b);
        else emit_expr(c, bbb[bbn-1], b);
        buf_puts(b, "; ");
      }
      else {
        emit_stmt(c, bbb[bbn-1], b, 0);
        if (scalar) {
          emit_ctype(c, res, b); buf_printf(b, " _t%d = ", rv);
          if (res == TY_POLY) buf_puts(b, "sp_box_nil()");
          else buf_puts(b, default_value(res));
          buf_puts(b, "; ");
        }
      }
    }
    if (scalar) buf_printf(b, "_t%d; })", rv);
    else buf_puts(b, "0; })");
    return;
  }

  /* (range).lazy[.select/reject/filter{blk}].first(n) -- lower to a C for-loop */
  if (!strcmp(name, "first") && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode")) {
    const char *rname0 = nt_str(nt, recv, "name");
    int lazy_nid = -1;
    int filter_block = -1;
    int filter_negate = 0;
    if (rname0 && !strcmp(rname0, "lazy") && nt_ref(nt, recv, "block") < 0) {
      lazy_nid = recv;
    }
    else if (rname0 && (!strcmp(rname0, "select") || !strcmp(rname0, "reject") || !strcmp(rname0, "filter"))) {
      filter_block = nt_ref(nt, recv, "block");
      if (filter_block >= 0) {
        filter_negate = !strcmp(rname0, "reject") ? 1 : 0;
        int inner = nt_ref(nt, recv, "receiver");
        if (inner >= 0 && nt_type(nt, inner) && !strcmp(nt_type(nt, inner), "CallNode")) {
          const char *iname = nt_str(nt, inner, "name");
          if (iname && !strcmp(iname, "lazy") && nt_ref(nt, inner, "block") < 0)
            lazy_nid = inner;
        }
      }
    }
    if (lazy_nid >= 0) {
      int src = unwrap_parens(c, nt_ref(nt, lazy_nid, "receiver"));
      if (src >= 0 && infer_type(c, src) == TY_RANGE) {
        int excl = (int)(nt_int(nt, src, "flags", 0) & 4) ? 1 : 0;
        int right = nt_ref(nt, src, "right");
        int endless = (right < 0);
        if (!endless && nt_type(nt, right) && !strcmp(nt_type(nt, right), "NilNode")) endless = 1;
        if (!endless && nt_type(nt, right) && !strcmp(nt_type(nt, right), "ConstantPathNode")) {
          const char *cpnm = nt_str(nt, right, "name");
          if (cpnm && !strcmp(cpnm, "INFINITY")) {
            int par = nt_ref(nt, right, "parent");
            const char *parnm = (par >= 0 && nt_type(nt, par) &&
                                 !strcmp(nt_type(nt, par), "ConstantReadNode"))
                                ? nt_str(nt, par, "name") : NULL;
            if (parnm && !strcmp(parnm, "Float")) endless = 1;
          }
        }
        int left_n = nt_ref(nt, src, "left");
        const char *bp = "_lx";
        if (filter_block >= 0) {
          const char *bpn = block_param_name(c, filter_block, 0);
          if (bpn && bpn[0]) bp = rename_local(bpn);
        }
        Buf lb; memset(&lb, 0, sizeof lb);
        emit_expr(c, left_n, &lb);
        Buf hb; memset(&hb, 0, sizeof hb);
        if (!endless) emit_expr(c, right, &hb);
        int thi = -1;
        if (!endless) {
          thi = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = %s;\n", thi, hb.p ? hb.p : "0");
          free(hb.p);
        }
        int tloop = ++g_tmp;
        if (argc >= 1) {
          /* first(n): collect matching elements into sp_IntArray */
          Buf nb; memset(&nb, 0, sizeof nb);
          emit_expr(c, argv[0], &nb);
          int tn = ++g_tmp, tres = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = %s;\n", tn, nb.p ? nb.p : "0");
          free(nb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
          emit_indent(g_pre, g_indent);
          if (endless) {
            buf_printf(g_pre, "for (mrb_int _t%d = %s; sp_IntArray_length(_t%d) < _t%d; _t%d++) {\n",
                       tloop, lb.p ? lb.p : "0", tres, tn, tloop);
          }
          else {
            buf_printf(g_pre, "for (mrb_int _t%d = %s; _t%d %s _t%d && sp_IntArray_length(_t%d) < _t%d; _t%d++) {\n",
                       tloop, lb.p ? lb.p : "0", tloop, excl ? "<" : "<=", thi, tres, tn, tloop);
          }
          free(lb.p);
          if (filter_block >= 0) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "lv_%s = _t%d;\n", bp, tloop);
            int fbody = nt_ref(nt, filter_block, "body");
            int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
            for (int k = 0; k < fbn - 1; k++) emit_stmt(c, fbb[k], g_pre, g_indent + 1);
            if (fbn > 0) {
              Buf pb; memset(&pb, 0, sizeof pb);
              int svind = g_indent; g_indent += 1;
              emit_cond(c, fbb[fbn - 1], &pb);
              g_indent = svind;
              emit_indent(g_pre, g_indent + 1);
              if (filter_negate)
                buf_printf(g_pre, "if (!(%s)) sp_IntArray_push(_t%d, _t%d);\n",
                           pb.p ? pb.p : "0", tres, tloop);
              else
                buf_printf(g_pre, "if (%s) sp_IntArray_push(_t%d, _t%d);\n",
                           pb.p ? pb.p : "0", tres, tloop);
              free(pb.p);
            }
          }
          else {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_IntArray_push(_t%d, _t%d);\n", tres, tloop);
          }
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres);
          return;
        }
        else {
          /* first (no arg): return first matching element as mrb_int */
          int tres = ++g_tmp, tfound = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0; mrb_bool _t%d = 0;\n", tres, tfound);
          emit_indent(g_pre, g_indent);
          if (endless) {
            buf_printf(g_pre, "for (mrb_int _t%d = %s; !_t%d; _t%d++) {\n",
                       tloop, lb.p ? lb.p : "0", tfound, tloop);
          }
          else {
            buf_printf(g_pre, "for (mrb_int _t%d = %s; !_t%d && _t%d %s _t%d; _t%d++) {\n",
                       tloop, lb.p ? lb.p : "0", tfound, tloop, excl ? "<" : "<=", thi, tloop);
          }
          free(lb.p);
          if (filter_block >= 0) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "lv_%s = _t%d;\n", bp, tloop);
            int fbody = nt_ref(nt, filter_block, "body");
            int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
            for (int k = 0; k < fbn - 1; k++) emit_stmt(c, fbb[k], g_pre, g_indent + 1);
            if (fbn > 0) {
              Buf pb; memset(&pb, 0, sizeof pb);
              int svind = g_indent; g_indent += 1;
              emit_cond(c, fbb[fbn - 1], &pb);
              g_indent = svind;
              emit_indent(g_pre, g_indent + 1);
              if (filter_negate)
                buf_printf(g_pre, "if (!(%s)) { _t%d = _t%d; _t%d = 1; }\n",
                           pb.p ? pb.p : "0", tres, tloop, tfound);
              else
                buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = 1; }\n",
                           pb.p ? pb.p : "0", tres, tloop, tfound);
              free(pb.p);
            }
          }
          else {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "_t%d = _t%d; _t%d = 1;\n", tres, tloop, tfound);
          }
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres);
          return;
        }
      }
    }
  }

  /* NoMethodError gate: an unresolved call on a dynamically-typed receiver
     (poly/nil/int/unknown -- no user class defines the method and no builtin
     matches) yields a typed nil/0 placeholder instead of aborting. In practice
     such a call is guarded by a runtime-nil receiver (e.g. an optional hook that
     is never installed), so it never executes; emitting the inferred-type
     default keeps codegen going without changing observable behaviour.

     TY_STRING is included for the same reason: a String is a builtin with a
     closed method table, so an unresolved call on it (e.g. `s.each`, which is a
     real NoMethodError in Ruby) is the String analogue of the poly/int case,
     not a user-class typo. The motivating shape is a `String|Hash` parameter
     that this closed-world program only ever calls with a String: the
     `if x.is_a?(String) ... else x.each end` Hash branch is then statically
     dead, and CRuby never reaches its NoMethodError, so a runtime-nil stub
     matches observable behaviour (#1434). A concrete user-object receiver still
     errors -- that is a genuine missing method worth catching at compile time. */
  if (recv >= 0) {
    TyKind grt = comp_ntype(c, recv);
    if (grt == TY_POLY || grt == TY_NIL || grt == TY_INT || grt == TY_UNKNOWN ||
        grt == TY_STRING) {
      TyKind ret = comp_ntype(c, id);
      /* Opt-in visibility: this call silently becomes nil/0 where CRuby would
         raise NoMethodError. The degradation is deliberate (a dead poly-dispatch
         arm or an inference gap), but it can also hide a genuine missing method.
         SPINEL_WARN_UNRESOLVED lists every such site so a port can be audited
         without changing runtime behaviour. */
      if (warn_unresolved_pos(c, id)) {
        const char *nm = nt_str(nt, id, "name");
        fprintf(stderr, "unresolved call '%s' on %s receiver -> nil "
                        "(CRuby would raise NoMethodError)\n",
                nm ? nm : "?", ty_name(grt));
      }
      buf_puts(b, (is_scalar_ret(ret) && ret != TY_UNKNOWN) ? default_value(ret) : "sp_box_nil()");
      return;
    }
  }
  unsupported(c, id, "call");
}

/* Array-mutating calls emitted as statements: a[i]=v, a.push(v), a<<v.
   Returns 1 if handled. */
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
  if ((!strcmp(name, "<<") || !strcmp(name, "concat")) && argc == 1) {
    int chain[64]; int nchain = 0; int cur = id;
    while (nchain < 64) {
      while (nt_type(nt, cur) && !strcmp(nt_type(nt, cur), "ParenthesesNode")) {
        int pb = nt_ref(nt, cur, "body");
        if (pb < 0) break;
        int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
        if (bn != 1) break;
        cur = bb[0];
      }
      const char *cty = nt_type(nt, cur);
      if (!cty || strcmp(cty, "CallNode")) break;
      const char *cnm = nt_str(nt, cur, "name");
      int crecv = nt_ref(nt, cur, "receiver");
      if (!cnm || (strcmp(cnm, "<<") && strcmp(cnm, "concat")) || crecv < 0) break;
      int cargs = nt_ref(nt, cur, "arguments");
      int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac != 1) break;
      chain[nchain++] = cav[0];
      cur = crecv;
    }
    const char *bty = nt_type(nt, cur);
    LocalVar *blv = (bty && !strcmp(bty, "LocalVariableReadNode"))
                    ? scope_local(comp_scope_of(c, cur), nt_str(nt, cur, "name")) : NULL;
    if (nchain > 0 && blv && blv->type == TY_STRBUF) {
      const char *bn2 = rename_local(nt_str(nt, cur, "name"));
      for (int j = nchain - 1; j >= 0; j--) {
        int arg = chain[j];
        TyKind at = comp_ntype(c, arg);
        emit_indent(b, indent);
        buf_printf(b, "sp_String_append(lv_%s, ", bn2);
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
  if (rt == TY_STRING && !strcmp(name, "<<") && argc == 1) {
    /* walk down the receiver chain, collecting each `<<` argument */
    int chain[64]; int nchain = 0;
    int cur = id;
    while (nchain < 64) {
      /* unwrap ParenthesesNode wrappers (e.g. `(s << a) << b`) */
      while (nt_type(nt, cur) && !strcmp(nt_type(nt, cur), "ParenthesesNode")) {
        int pb = nt_ref(nt, cur, "body");
        if (pb < 0) break;
        int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
        if (bn != 1) break;
        cur = bb[0];
      }
      const char *cty = nt_type(nt, cur);
      if (!cty || strcmp(cty, "CallNode")) break;
      const char *cnm = nt_str(nt, cur, "name");
      int crecv = nt_ref(nt, cur, "receiver");
      if (!cnm || strcmp(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
      int cargs = nt_ref(nt, cur, "arguments");
      int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
      if (cac != 1) break;
      chain[nchain++] = cav[0];
      cur = crecv;
    }
    const char *rty = nt_type(nt, cur);
    if (nchain > 0 && rty &&
        (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
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
    if (rty && !strcmp(rty, "StringNode")) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"FrozenError\", \"can't modify frozen String\");\n");
      return 1;
    }
    return 0;
  }

  /* in-place string bang methods on an assignable receiver: reassign the
     receiver to the transformed value (value-semantics mutation, like <<). */
  if (rt == TY_STRING && argc == 0) {
    const char *base = NULL;
    if      (!strcmp(name, "chomp!"))      base = "chomp";
    else if (!strcmp(name, "chop!"))       base = "chop";
    else if (!strcmp(name, "upcase!"))     base = "upcase";
    else if (!strcmp(name, "downcase!"))   base = "downcase";
    else if (!strcmp(name, "capitalize!")) base = "capitalize";
    else if (!strcmp(name, "swapcase!"))   base = "swapcase";
    else if (!strcmp(name, "strip!"))      base = "strip";
    else if (!strcmp(name, "lstrip!"))     base = "lstrip";
    else if (!strcmp(name, "rstrip!"))     base = "rstrip";
    else if (!strcmp(name, "reverse!"))    base = "reverse";
    else if (!strcmp(name, "squeeze!"))    base = "squeeze";
    if (base) {
      const char *rty = nt_type(nt, recv);
      if (rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"))) {
        emit_indent(b, indent);
        emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ");\n");
        return 1;
      }
    }
  }
  /* replace / prepend / clear / delete_prefix!/suffix! via reassignment */
  if (rt == TY_STRING) {
    const char *rty = nt_type(nt, recv);
    int assignable = rty && (!strcmp(rty, "LocalVariableReadNode") || !strcmp(rty, "InstanceVariableReadNode") || !strcmp(rty, "SelfNode"));
    /* an in-place mutator on a frozen string literal raises FrozenError */
    if (rty && !strcmp(rty, "StringNode") &&
        (!strcmp(name, "insert") || !strcmp(name, "prepend") || !strcmp(name, "<<") ||
         !strcmp(name, "concat") || !strcmp(name, "replace") || !strcmp(name, "clear") ||
         !strcmp(name, "delete_prefix!") || !strcmp(name, "delete_suffix!"))) {
      emit_indent(b, indent);
      buf_puts(b, "sp_raise_cls(\"FrozenError\", \"can't modify frozen String\");\n");
      return 1;
    }
    if (assignable && !strcmp(name, "replace") && argc == 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = "); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
      return 1;
    }
    if (assignable && !strcmp(name, "prepend") && argc == 1) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, recv, b); buf_puts(b, ");\n");
      return 1;
    }
    if (assignable && !strcmp(name, "clear") && argc == 0) {
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent); emit_expr(c, recv, b); buf_puts(b, " = (&(\"\\xff\")[1]);\n");
      return 1;
    }
    if (assignable && !strcmp(name, "insert") && argc == 2) {
      /* insert(i, x): s[0,i] + x + s[i..]. A negative i counts from the end
         and inserts after that character (i += len + 1). */
      int ti = ++g_tmp;
      emit_indent(b, indent); buf_puts(b, "sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, ");\n");
      emit_indent(b, indent);
      buf_printf(b, "{ mrb_int _t%d = ", ti); emit_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (mrb_int)sp_str_length(", ti, ti); emit_expr(c, recv, b); buf_printf(b, ") + 1; ");
      emit_expr(c, recv, b); buf_puts(b, " = sp_str_concat(sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b); buf_printf(b, ", 0, _t%d), ", ti); emit_expr(c, argv[1], b);
      buf_puts(b, "), sp_str_sub_range("); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, (mrb_int)sp_str_length(", ti); emit_expr(c, recv, b); buf_printf(b, "))); }\n");
      return 1;
    }
    if (assignable && (!strcmp(name, "delete_prefix!") || !strcmp(name, "delete_suffix!")) && argc == 1) {
      const char *base = !strcmp(name, "delete_prefix!") ? "delete_prefix" : "delete_suffix";
      emit_indent(b, indent); emit_expr(c, recv, b); buf_printf(b, " = sp_str_%s(", base); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ");\n");
      return 1;
    }
  }

  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (hn && !strcmp(name, "[]=") && argc == 2) {
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
    if (!strcmp(name, "[]=") && argc == 2) {
      emit_indent(b, indent);
      buf_puts(b, "sp_PolyArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ");\n");
      return 1;
    }
    if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc >= 1) {
      for (int a = 0; a < argc; a++) {
        emit_indent(b, indent);
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[a], b); buf_puts(b, ");\n");
      }
      return 1;
    }
    if (!strcmp(name, "clear") && argc == 0) {
      emit_indent(b, indent);
      buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ")->len = 0;\n");
      return 1;
    }
    return 0;
  }

  if (rt == TY_POLY && !strcmp(name, "<<") && argc == 1) {
    emit_indent(b, indent);
    buf_puts(b, "sp_poly_shl("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ");\n");
    return 1;
  }

  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;

  if (!strcmp(name, "[]=") && argc == 2) {
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
  if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) && argc >= 1) {
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
  if (!strcmp(name, "concat") && argc >= 1) {
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
      if (!strcmp(k, "Poly") && strcmp(ak, "Poly")) {
        /* box the source scalar into the poly receiver */
        emit_boxed_text(c, ty_array_elem(at), getexpr, b);
      }
      else if (strcmp(k, "Poly") && !strcmp(ak, "Poly")) {
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
        (!strcmp(op, "+") ? "sp_poly_add" : !strcmp(op, "-") ? "sp_poly_sub" :
         !strcmp(op, "*") ? "sp_poly_mul" : !strcmp(op, "/") ? "sp_poly_div" :
         !strcmp(op, "%") ? "sp_poly_mod" : !strcmp(op, "**") ? "sp_poly_pow" : NULL) : NULL;
    if (vt == TY_STRING && !strcmp(op, "+")) {
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
    const char *k = array_kind(rt);
    if (!k) unsupported(c, id, "index operator assignment (array)");
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_puts(b, "; ");
    buf_printf(b, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d) %s ", k, ta, tb, k, ta, tb, op);
    buf_puts(b, "("); emit_expr(c, v, b); buf_puts(b, ")); }\n");
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
      unsupported(c, id, "index operator assignment (poly-recv, non-sym/str key)");
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
    const char *k = array_kind(rt);
    if (!k) { unsupported(c, id, "index and/or write (array kind)"); return; }
    emit_indent(b, indent);
    buf_printf(b, "{ %s _t%d = ", c_type_name(rt), ta); emit_expr(c, recv, b);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_expr(c, argv[0], b);
    buf_puts(b, "; ");
    if (rt == TY_INT_ARRAY) {
      buf_printf(b, "if (%ssp_IntArray_get(_t%d, _t%d) != SP_INT_NIL) sp_IntArray_set(_t%d, _t%d, ",
                 is_or ? "!" : "", ta, tb, ta, tb);
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

int scope_has_return(Compiler *c, int scope_idx) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && !strcmp(ty, "ReturnNode") && c->nscope[id] == scope_idx) return 1;
  }
  return 0;
}

/* Follow a chain of pure `...` forwarders (a method whose whole body is a
   single `target(...)` call, no receiver) from `mi` to the method that
   actually yields or owns the &block; return its index, else -1. A real-
   function forwarder can't pass a literal block down to a yielding target,
   so a block-bearing call is redirected straight to that final target. */
static int pure_forwarding_target(Compiler *c, int mi, int depth) {
  if (mi < 0 || depth > 16) return -1;
  Scope *m = &c->scopes[mi];
  if (m->yields || (m->blk_param && m->blk_param[0])) return mi;
  int body = m->body;
  if (body < 0 || !nt_type(c->nt, body) || strcmp(nt_type(c->nt, body), "StatementsNode")) return -1;
  int n = 0; const int *st = nt_arr(c->nt, body, "body", &n);
  if (n != 1) return -1;
  int call = st[0];
  const char *cty = nt_type(c->nt, call);
  if (!cty || strcmp(cty, "CallNode") || nt_ref(c->nt, call, "receiver") >= 0) return -1;
  int args = nt_ref(c->nt, call, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
  if (ac != 1 || !nt_type(c->nt, av[0]) || strcmp(nt_type(c->nt, av[0]), "ForwardingArgumentsNode")) return -1;
  const char *tn = nt_str(c->nt, call, "name");
  if (!tn) return -1;
  int t = comp_method_index(c, tn);
  if (t < 0 && m->class_id >= 0) t = comp_method_in_chain(c, m->class_id, tn, NULL);
  return pure_forwarding_target(c, t, depth + 1);
}

/* Inline a call to a free-function yielding method `foo(args) { |bp| ... }`:
   declare the method's locals (renamed to avoid clashing with the call
   site), bind params to args, then emit the method body with yield
   expanding to the block. Returns 1 if handled. */
int emit_inline_call_x(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;
  int mi, recv_class = -1;
  int implicit_self = 0;
  if (recv < 0) {
    mi = comp_method_index(c, name);     /* free function */
    if (mi < 0) {                        /* implicit-self instance method */
      Scope *encl = comp_scope_of(c, id);
      if (encl->class_id >= 0) { mi = comp_method_in_chain(c, encl->class_id, name, NULL); implicit_self = 1; }
      else return 0;
    }
  }
  else {
    TyKind rt = comp_ntype(c, recv);
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      /* Cls.method with a yield block: look up as a class method */
      const char *cname = nt_str(nt, recv, "name");
      int ci = cname ? comp_class_index(c, cname) : -1;
      if (ci < 0) return 0;
      mi = comp_cmethod_in_chain(c, ci, name, NULL);
    }
    else if (ty_is_object(rt)) {
      recv_class = ty_object_class(rt);
      mi = comp_method_in_chain(c, recv_class, name, NULL);
    }
    else return 0;
  }
  (void)implicit_self;
  if (mi < 0) return 0;
  /* `fwd(args) { block }` where fwd just forwards `target(...)`: a literal
     block can't reach a real-function forwarder, so retarget to `target`
     with this call's args + block (target then splices the block normally). */
  {
    int blk0 = nt_ref(nt, id, "block");
    if (blk0 >= 0 && nt_type(nt, blk0) && !strcmp(nt_type(nt, blk0), "BlockNode")) {
      int t = pure_forwarding_target(c, mi, 0);
      if (t >= 0) mi = t;
    }
  }
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  int block = nt_ref(nt, id, "block");   /* may be -1: no block passed */
  /* `inner(&)` / `inner(&block)`: a BlockArgumentNode forwards the block
     active at this (already-inlined) site, not a fresh literal. */
  if (block >= 0 && nt_type(nt, block) && !strcmp(nt_type(nt, block), "BlockArgumentNode"))
    block = g_block_id;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  /* Pre-check: every body local must have an emittable type. Bail BEFORE
     writing anything (a mid-emit bail would leave an unbalanced `{`). */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_self = g_self;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  static char selfbuf[64];
  /* Nested `yield` inside the block body should chain to the block that was
     active before this inline, not to the inner block. */
  g_yield_block_fallback = saved_block;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  /* instance method: bind self to the receiver. A heap object is a pointer; a
     value-type receiver is a by-value struct, so copy it and dereference its
     ivars with `.` (value types are immutable, so the copy is transparent). */
  const char *saved_deref = g_self_deref;
  if (recv >= 0 && recv_class >= 0) {
    int self_is_val = c->classes[recv_class].is_value_type;
    int st = ++g_tmp;
    emit_indent(b, indent + 1);
    buf_printf(b, "sp_%s %s_t%d = ", c->classes[recv_class].name, self_is_val ? "" : "*", st);
    emit_expr(c, recv, b);
    buf_puts(b, ";\n");
    snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
    g_self = selfbuf;
    g_self_deref = self_is_val ? "." : "->";
  }
  int din = indent + 1;

  /* declare method locals under renamed names */
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;  /* virtual &block slot */
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  /* bind params to call args (args are in the call-site scope: renames off) */
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  /* `bar(...)` inside a `def foo(...)` forwarder: bind this (inlined) target's
     params from the enclosing forwarder's synth __fwd_* params, not from a
     literal ForwardingArgumentsNode (which has no value of its own). */
  Scope *fwd_encl = NULL;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      !strcmp(nt_type(nt, argv[0]), "ForwardingArgumentsNode"))
    fwd_encl = comp_scope_of(c, argv[0]);
  /* A trailing keyword-hash arg binds by param name, not positionally. */
  int kwh = -1, pos_argc = argc;
  if (argc > 0 && argv && nt_type(nt, argv[argc - 1]) &&
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1]; pos_argc = argc - 1;
  }
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int sv = g_nren; g_nren = 0;
    if (fwd_encl && i < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[i]);
      LocalVar *mp = scope_local(m, m->pnames[i]);
      TyKind et = ep ? ep->type : TY_POLY;
      TyKind mt = mp ? mp->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[i]);
      if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, b);
      else buf_puts(b, txt);
    }
    else if (i < pos_argc) emit_arg_or_default(c, m, i, argv[i], b);
    else {
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      emit_arg_or_default(c, m, i, kv, b);
    }
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  if (as_expr) {
    /* Use a result var so the tail uses assignment, not `return`, in the
       GCC statement-expression ({ ... result_var; }) context. */
    TyKind rt = comp_ntype(c, id);
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value(rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    emit_stmts_tail(c, m->body, b, din);
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else emit_stmts(c, m->body, b, din);
  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_self = saved_self;
  g_self_deref = saved_deref;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  return 1;
}

int emit_inline_call(Compiler *c, int id, Buf *b, int indent) {
  return emit_inline_call_x(c, id, b, indent, 0);
}

/* Is `id` a `<&block-param>.call(...)` invocation of the active block? */
int is_block_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (strcmp(nm, "call") && strcmp(nm, "()") && strcmp(nm, "[]") && strcmp(nm, "yield"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && !strcmp(rn, g_block_param_name);
}

/* A `<&block-param>.call(...)` on the inlined method's block param while NO
   block is supplied at this site (g_block_id<0). This arises when a real-
   function forwarder with no block of its own inlines a target that calls its
   &block: the path is dead for any real caller (a block-requiring method
   invoked without one raises), but must still compile. Caller emits nil. */
int is_blockless_block_param_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (!g_block_param_name || !g_block_param_name[0] || g_block_id >= 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || strcmp(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || (strcmp(nm, "call") && strcmp(nm, "()") && strcmp(nm, "[]"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  return rn && !strcmp(rn, g_block_param_name);
}

/* Expand the active block's body, binding its params to the given call
   args. Shared by YieldNode and `block.call`. `as_expr` wraps in ({...}). */
void emit_block_invoke(Compiler *c, int args_node, Buf *b, int indent, int as_expr) {
  const NodeTable *nt = c->nt;
  int blk = g_block_id;
  int bbody = nt_ref(nt, blk, "body");
  int yc = 0;
  const int *yargs = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &yc) : NULL;
  Scope *bsc = comp_scope_of(c, blk);
  if (as_expr) buf_puts(b, "({ ");
  for (int k = 0; ; k++) {
    const char *bp = block_param_name(c, blk, k);
    if (!bp) break;
    /* When inside an inlined method, block params may be renamed (e.g. x →
       _y3_x); apply the rename table so we write the right C variable. */
    const char *bpr = rename_local(bp);
    if (!as_expr) emit_indent(b, indent);
    buf_printf(b, "lv_%s = ", bpr);
    if (k < yc) {
      LocalVar *bl = bsc ? scope_local(bsc, bp) : NULL;
      TyKind bt = bl ? bl->type : TY_UNKNOWN;
      TyKind at = comp_ntype(c, yargs[k]);
      if (bt == TY_POLY && at != TY_POLY && at != TY_UNKNOWN)
        emit_boxed(c, yargs[k], b);
      else
        emit_expr(c, yargs[k], b);
    }
    else {
      LocalVar *bl = scope_local(bsc, bp);
      TyKind bt = bl ? bl->type : TY_INT;
      buf_puts(b, bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
    }
    buf_puts(b, as_expr ? "; " : ";\n");
  }
  /* Keep the rename table active for the block body: the block's variable
     references are in the same lexical scope as the surrounding inlined
     method, so renames like x → _y3_x must stay visible. Nested inlines
     inside the block body append at the current g_nren and self-restore.
     Set g_block_id to the fallback (the block active before the enclosing
     inline started) so that a nested `yield` inside the block chains to
     the outermost caller's block rather than going dead. */
  int svb = g_block_id; g_block_id = g_yield_block_fallback;
  const char *svbpn = g_block_param_name; g_block_param_name = NULL;
  emit_stmts(c, bbody, b, as_expr ? 0 : indent);
  g_block_id = svb; g_block_param_name = svbpn;
  if (as_expr) {
    /* `{ return e }`: the block exits the enclosing function, so the
       statement-expr's tail is unreachable — but C still needs a value
       expression there (a trailing `return;` makes the ({...}) void). */
    int bn2 = 0; const int *bd2 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn2) : NULL;
    if (bn2 > 0 && nt_type(nt, bd2[bn2 - 1]) &&
        !strcmp(nt_type(nt, bd2[bn2 - 1]), "ReturnNode")) {
      int ra = nt_ref(nt, bd2[bn2 - 1], "arguments");
      int rn = 0; const int *rv = ra >= 0 ? nt_arr(nt, ra, "arguments", &rn) : NULL;
      TyKind rt2 = rn > 0 ? comp_ntype(c, rv[0]) : TY_INT;
      buf_printf(b, " %s;", default_value(is_scalar_ret(rt2) ? rt2 : TY_INT));
    }
    buf_puts(b, "})");
  }
}

/* Inline a yielding method call in expression position: ({ ...; value; }).
   The method must return a usable value (its body's last statement). */
int emit_inline_expr(Compiler *c, int id, Buf *b) {
  /* only when a value is actually produced (scalar return) */
  TyKind rt = comp_ntype(c, id);
  if (!is_scalar_ret(rt)) return 0;
  return emit_inline_call_x(c, id, b, g_indent + 1, 1);
}

/* Block iteration lowered to an inline C for-loop. Handles n.times,
   array.each, range.each, n.upto/downto. Returns 1 if handled. */
/* Emit `lv_<p0> = <expr_src>` boxing if p0 is poly and src is concrete. */
void emit_iter_param_assign(Compiler *c, int block, const char *p0_orig,
                                   const char *p0_ren, TyKind src_type,
                                   const char *src_expr, Buf *b, int indent) {
  Scope *sc = comp_scope_of(c, block);
  LocalVar *lv = sc ? scope_local(sc, p0_orig) : NULL;
  TyKind pt = lv ? lv->type : src_type;
  emit_indent(b, indent);
  if (pt == TY_POLY && src_type != TY_POLY) {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, src_type, src_expr, &bx);
    buf_printf(b, "lv_%s = %s;\n", p0_ren, bx.p ? bx.p : src_expr);
    free(bx.p);
  }
  else {
    buf_printf(b, "lv_%s = %s;\n", p0_ren, src_expr);
  }
}

/* Does the subtree contain a `redo` that belongs to THIS loop, i.e. one not
   nested inside a deeper loop/block/def (which would own it instead)? */
int subtree_has_own_redo(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "RedoNode")) return 1;
  /* nested scope/loop boundaries: a redo inside binds to that inner loop */
  if (!strcmp(ty, "DefNode") || !strcmp(ty, "ClassNode") || !strcmp(ty, "ModuleNode") ||
      !strcmp(ty, "WhileNode") || !strcmp(ty, "UntilNode") || !strcmp(ty, "ForNode") ||
      !strcmp(ty, "LambdaNode"))
    return 0;
  if (!strcmp(ty, "CallNode") && nt_ref(nt, id, "block") >= 0) return 0;  /* nested iteration */
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (subtree_has_own_redo(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (subtree_has_own_redo(nt, ids[k])) return 1;
  }
  return 0;
}

/* Emit a loop body, prefixing a `_redo_N:` label (and pushing it on the redo
   stack) when the body contains a `redo` that targets this loop. The label
   sits at the body top so `redo` re-runs the body without advancing. */
void emit_loop_body(Compiler *c, int body, Buf *b, int indent) {
  int has_redo = subtree_has_own_redo(c->nt, body);
  int lbl = 0;
  if (has_redo) {
    lbl = ++g_tmp;
    if (g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0]))
      g_redo_stack[g_redo_depth++] = lbl;
    else has_redo = 0;
  }
  if (has_redo) { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", lbl); }
  emit_stmts(c, body, b, indent);
  if (has_redo) g_redo_depth--;
}

/* `recv.tap { |x| body }` / `recv.then { |x| body }` (alias yield_self) in
   expression position. tap runs the block for its side effect and yields the
   (unchanged) receiver; then yields the block's value. The loop body emits into
   the statement prelude (g_pre); the result temp is the expression value. */
int emit_tap_then_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_tap = !strcmp(name, "tap");
  int is_then = !strcmp(name, "then") || !strcmp(name, "yield_self");
  if (!is_tap && !is_then) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || strcmp(nt_type(nt, block), "BlockNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind et = comp_ntype(c, recv);
  if (et == TY_UNKNOWN) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (is_then && bn < 1) return 0;  /* then must yield a value */
  const char *p0 = block_param_name(c, block, 0);
  if (p0) p0 = rename_local(p0);

  int tr = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
  if (needs_root(et)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr); }

  /* a then result temp is declared outside the (optional) shadow block so the
     block value escapes it. */
  int tres = 0; TyKind rett = TY_VOID;
  if (is_then) {
    rett = comp_ntype(c, id);
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); emit_ctype(c, rett, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tres, default_value(rett));
    if (needs_root(rett)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres); }
  }

  /* pin the block param to the receiver type if inference widened it */
  Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
  TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
  int use_shadow = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
  int din = g_indent;
  if (use_shadow) {
    tlv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    din = g_indent + 1;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = _t%d;\n", p0, tr);
  }
  else if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tr); }

  int sv = g_indent; g_indent = din;
  int last = is_tap ? bn : bn - 1;
  for (int j = 0; j < last; j++) emit_stmt(c, bb[j], g_pre, din);
  if (is_then) {
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb);
    TyKind vt = comp_ntype(c, bb[bn - 1]);
    emit_indent(g_pre, din); buf_printf(g_pre, "_t%d = ", tres);
    if (rett == TY_POLY && vt != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, vt, vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_puts(g_pre, vb.p ? vb.p : "");
    buf_puts(g_pre, ";\n"); free(vb.p);
  }
  g_indent = sv;
  if (use_shadow) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n"); }
  if (tlv0) tlv0->type = tsaved0;

  buf_printf(b, "_t%d", is_tap ? tr : tres);
  return 1;
}

int emit_iteration_stmt(Compiler *c, int id, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name) return 0;

  /* loop { ... } -- infinite loop, exited by break */
  if (recv < 0 && !strcmp(name, "loop")) {
    int lbody = nt_ref(nt, block, "body");
    emit_indent(b, indent); buf_puts(b, "for (;;) {\n");
    emit_loop_body(c, lbody, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  if (recv < 0) return 0;
  int body = nt_ref(nt, block, "body");
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  TyKind rt = comp_ntype(c, recv);

  /* n.times { |i| ... } */
  if (!strcmp(name, "times") && rt == TY_INT) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "; _t%d++) {\n", t);
    if (p0) { char ts[32]; snprintf(ts, sizeof ts, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* num.step(limit[, step]) { [|i|] ... } -- stepping loop. A float receiver
     or a float limit/step makes it a float walk (yielding floats), computed
     by iteration count to avoid floating-point drift (CRuby semantics). */
  if (!strcmp(name, "step") && (rt == TY_INT || rt == TY_FLOAT)) {
    int args = nt_ref(nt, id, "arguments");
    int sargc = 0;
    const int *sargv = args >= 0 ? nt_arr(nt, args, "arguments", &sargc) : NULL;
    if (sargc < 1) return 0;
    int is_float = (rt == TY_FLOAT) || comp_ntype(c, sargv[0]) == TY_FLOAT ||
                   (sargc >= 2 && comp_ntype(c, sargv[1]) == TY_FLOAT);
    if (!is_float) {
      int t = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp;
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tl); emit_expr(c, sargv[0], b); buf_puts(b, ";\n");
      emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", ts);
      if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1");
      buf_puts(b, ";\n");
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) {\n",
                 ts, t, tl, t, tl, t, ts);
      if (p0) { char ts2[32]; snprintf(ts2, sizeof ts2, "_t%d", t); emit_iter_param_assign(c, block, p0_orig, p0, TY_INT, ts2, b, indent + 1); }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      return 1;
    }
    int tb = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", tb); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", tl); emit_expr(c, sargv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "mrb_float _t%d = ", ts);
    if (sargc >= 2) emit_expr(c, sargv[1], b); else buf_puts(b, "1.0");
    buf_puts(b, ";\n");
    /* n = floor((limit-begin)/step + err); err bounds fp drift (CRuby) */
    emit_indent(b, indent);
    buf_printf(b, "mrb_float _t%d_e = (fabs(_t%d)+fabs(_t%d)+fabs(_t%d-_t%d))/fabs(_t%d)*DBL_EPSILON;\n",
               tn, tb, tl, tl, tb, ts);
    emit_indent(b, indent);
    buf_printf(b, "if (_t%d_e > 0.5) _t%d_e = 0.5;\n", tn, tn);
    emit_indent(b, indent);
    buf_printf(b, "mrb_int _t%d = (mrb_int)floor((_t%d-_t%d)/_t%d + _t%d_e);\n", tn, tl, tb, ts, tn);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d <= _t%d; _t%d++) {\n", ti, ti, tn, ti);
    if (p0) { char fp_expr[64]; snprintf(fp_expr, sizeof fp_expr, "_t%d + _t%d * _t%d", tb, ti, ts); emit_iter_param_assign(c, block, p0_orig, p0, TY_FLOAT, fp_expr, b, indent + 1); }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* hash.each / each_pair { |k, v| ... } */
  if ((!strcmp(name, "each") || !strcmp(name, "each_pair")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "->len; _t%d++) {\n", t);
    if (p0) {
      /* The param may be poly (a name shared across hashes of differing element
         types); box a concrete key into the poly slot. */
      const char *raw0 = block_param_name(c, block, 0);
      LocalVar *pv0 = raw0 ? scope_local(comp_scope_of(c, block), raw0) : NULL;
      TyKind want0 = ty_hash_key(rt);
      int box0 = pv0 && pv0->type == TY_POLY && want0 != TY_POLY;
      char src0[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src0, sizeof src0, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src0, sizeof src0, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box0) emit_boxed_text(c, want0, src0, b); else buf_puts(b, src0);
      buf_puts(b, ";\n");
    }
    if (p1) {
      const char *raw1 = block_param_name(c, block, 1);
      LocalVar *pv1 = raw1 ? scope_local(comp_scope_of(c, block), raw1) : NULL;
      TyKind want1 = ty_hash_val(rt);
      int box1 = pv1 && pv1->type == TY_POLY && want1 != TY_POLY;
      char src1[256];
      if (rt == TY_POLY_POLY_HASH)
        snprintf(src1, sizeof src1, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
      else
        snprintf(src1, sizeof src1, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p1);
      if (box1) emit_boxed_text(c, want1, src1, b); else buf_puts(b, src1);
      buf_puts(b, ";\n");
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.each_value { |v| ... } / each_key { |k| ... } -- single param */
  if ((!strcmp(name, "each_value") || !strcmp(name, "each_key")) && ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int is_val = !strcmp(name, "each_value");
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < ", t, t);
    buf_puts(b, rb.p); buf_printf(b, "->len; _t%d++) {\n", t);
    if (p0) {
      /* The param may be poly (shared name across hashes of differing
         element types); box a concrete element into the poly slot. */
      const char *raw = block_param_name(c, block, 0);
      LocalVar *pv = raw ? scope_local(comp_scope_of(c, block), raw) : NULL;
      TyKind want = is_val ? ty_hash_val(rt) : ty_hash_key(rt);
      int box = pv && pv->type == TY_POLY && want != TY_POLY;
      char src[256];
      if (rt == TY_POLY_POLY_HASH) {
        /* PolyPolyHash: ->order[i] is an index; keys/vals hold sp_RbVal */
        if (is_val)
          snprintf(src, sizeof src, "%s->vals[%s->order[_t%d]]", rb.p, rb.p, t);
        else
          snprintf(src, sizeof src, "%s->keys[%s->order[_t%d]]", rb.p, rb.p, t);
      }
      else if (is_val)
        snprintf(src, sizeof src, "sp_%sHash_get(%s, %s->order[_t%d])", hn, rb.p, rb.p, t);
      else
        snprintf(src, sizeof src, "%s->order[_t%d]", rb.p, t);
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = ", p0);
      if (box) emit_boxed_text(c, want, src, b);
      else buf_puts(b, src);
      buf_puts(b, ";\n");
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(rb.p);
    return 1;
  }

  /* hash.delete_if / reject! / select! / filter! / keep_if { |k, v| cond } */
  if ((!strcmp(name, "delete_if") || !strcmp(name, "reject!") || !strcmp(name, "select!") ||
       !strcmp(name, "filter!") || !strcmp(name, "keep_if")) && ty_is_hash(rt) && block >= 0) {
    const char *hn = ty_hash_cname(rt);
    if (hn && rt != TY_POLY_POLY_HASH) {
      int is_rej = (!strcmp(name, "delete_if") || !strcmp(name, "reject!"));
      const char *p0_raw = block_param_name(c, block, 0);
      const char *p1_raw = block_param_name(c, block, 1);
      const char *kp = p0_raw ? rename_local(p0_raw) : NULL;
      const char *vp = p1_raw ? rename_local(p1_raw) : NULL;
      int body2 = nt_ref(nt, block, "body");
      int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
      if (bn2 >= 1) {
        Scope *hs2 = comp_scope_of(c, block);
        TyKind hkt = ty_hash_key(rt), hvt = ty_hash_val(rt);
        /* Temporarily set block param types to hash key/value types so
           the block body is emitted with the right concrete types.
           Refresh the ntype cache with the new types. */
        LocalVar *klv = (kp && hs2) ? scope_local(hs2, p0_raw) : NULL;
        LocalVar *vlv = (vp && hs2) ? scope_local(hs2, p1_raw) : NULL;
        TyKind ksaved = klv ? klv->type : TY_UNKNOWN;
        TyKind vsaved = vlv ? vlv->type : TY_UNKNOWN;
        if (klv) klv->type = hkt;
        if (vlv) vlv->type = hvt;
        for (int j = 0; j < bn2; j++) infer_type(c, bb2[j]);
        int tr2 = ++g_tmp, ti2 = ++g_tmp;
        Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
        emit_indent(b, indent); emit_ctype(c, rt, b);
        buf_printf(b, "_t%d = %s;\n", tr2, rb2.p ? rb2.p : "NULL"); free(rb2.p);
        emit_indent(b, indent);
        buf_printf(b, "for (mrb_int _t%d = 0; _t%d && _t%d < _t%d->len; ) {\n",
                   ti2, tr2, ti2, tr2);
        /* declare + assign key param with hash key type (shadows outer lv_k) */
        if (kp) {
          emit_indent(b, indent + 1); emit_ctype(c, hkt, b);
          buf_printf(b, " lv_%s = _t%d->order[_t%d];\n", kp, tr2, ti2);
        }
        /* declare + assign value param with hash value type (shadows outer lv_v) */
        if (vp) {
          emit_indent(b, indent + 1); emit_ctype(c, hvt, b);
          buf_printf(b, " lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n",
                     vp, hn, tr2, tr2, ti2);
        }
        /* block body stmts except last */
        for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], b, indent + 1);
        /* condition: save g_pre so pre-effects land inside the loop body */
        Buf *sp2 = g_pre; int si2 = g_indent;
        Buf cpre2; memset(&cpre2, 0, sizeof cpre2); g_pre = &cpre2; g_indent = indent + 1;
        Buf cexpr2; memset(&cexpr2, 0, sizeof cexpr2);
        emit_expr(c, bb2[bn2 - 1], &cexpr2);
        g_pre = sp2; g_indent = si2;
        if (cpre2.p) { buf_puts(b, cpre2.p); free(cpre2.p); }
        emit_indent(b, indent + 1);
        if (is_rej) buf_printf(b, "if (%s) {\n", cexpr2.p ? cexpr2.p : "0");
        else        buf_printf(b, "if (!(%s)) {\n", cexpr2.p ? cexpr2.p : "0");
        free(cexpr2.p);
        emit_indent(b, indent + 2);
        buf_printf(b, "sp_%sHash_delete(_t%d, _t%d->order[_t%d]);\n", hn, tr2, tr2, ti2);
        emit_indent(b, indent + 1); buf_puts(b, "} else {\n");
        emit_indent(b, indent + 2); buf_printf(b, "_t%d++;\n", ti2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        emit_indent(b, indent); buf_puts(b, "}\n");
        /* restore block param types */
        if (klv) klv->type = ksaved;
        if (vlv) vlv->type = vsaved;
        return 1;
      }
    }
  }

  /* array.each_with_index { |x, i| ... } */
  if (!strcmp(name, "each_with_index") && ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    Scope *cs_ewi = comp_scope_of(c, id);
    LocalVar *clv_ewi_p1 = (p1 && cs_ewi) ? scope_local(cs_ewi, p1) : NULL;
    LocalVar *clv_ewi_p0 = (p0 && cs_ewi) ? scope_local(cs_ewi, p0) : NULL;
    TyKind ewi_et = ty_array_elem(rt);
    int p0_box_poly = clv_ewi_p0 && clv_ewi_p0->type == TY_POLY && ewi_et != TY_POLY;
    int p1_box_poly = clv_ewi_p1 && clv_ewi_p1->type == TY_POLY;
    /* Save outer variables before loop */
    int ts_p0 = 0, ts_p1 = 0;
    if (p0 && clv_ewi_p0) {
      ts_p0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p0->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p0, p0); free(ot.p);
    }
    if (p1 && clv_ewi_p1) {
      ts_p1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, clv_ewi_p1->type, &ot);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", ts_p1, p1); free(ot.p);
    }
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
    buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    if (p0) {
      emit_indent(b, indent + 1);
      if (p0_box_poly) {
        char src[512]; snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        buf_printf(b, "lv_%s = ", p0); emit_boxed_text(c, ewi_et, src, b); buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    if (p1) {
      emit_indent(b, indent + 1);
      if (p1_box_poly) buf_printf(b, "lv_%s = sp_box_int(_t%d);\n", p1, t);
      else buf_printf(b, "lv_%s = _t%d;\n", p1, t);
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    /* Restore outer variables */
    if (p0 && ts_p0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_p0); }
    if (p1 && ts_p1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1, ts_p1); }
    free(rb.p);
    return 1;
  }

  /* array.zip(other) { |a, b| ... } — block form, returns nil */
  if (!strcmp(name, "zip") && ty_is_array(rt) && block >= 0) {
    int zargs_n = nt_ref(nt, id, "arguments");
    int zargc = 0; const int *zargv = zargs_n >= 0 ? nt_arr(nt, zargs_n, "arguments", &zargc) : NULL;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (k && zargc == 1 && zargv) {
      TyKind a0t = comp_ntype(c, zargv[0]);
      const char *k2 = ty_is_array(a0t) ? ((a0t == TY_POLY_ARRAY) ? "Poly" : array_kind(a0t)) : NULL;
      if (!k2) k2 = k;
      TyKind et = ty_array_elem(rt);
      TyKind et2 = ty_is_array(a0t) ? ty_array_elem(a0t) : et;
      const char *p1n = block_param_name(c, block, 1); if (p1n) p1n = rename_local(p1n);
      int t = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      Buf ob; memset(&ob, 0, sizeof ob); emit_expr(c, zargv[0], &ob);
      Scope *zs = comp_scope_of(c, id);
      LocalVar *zlv0 = (p0 && zs) ? scope_local(zs, p0) : NULL;
      LocalVar *zlv1 = (p1n && zs) ? scope_local(zs, p1n) : NULL;
      int zs0 = 0, zs1 = 0;
      if (p0 && zlv0) {
        zs0 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv0->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs0, p0); free(ot.p);
      }
      if (p1n && zlv1) {
        zs1 = ++g_tmp; Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, zlv1->type, &ot);
        emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "sp_RbVal", zs1, p1n); free(ot.p);
      }
      emit_indent(b, indent);
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(%s); _t%d++) {\n",
                 t, t, k, rb.p ? rb.p : "NULL", t);
      if (p0 && zlv0) {
        char src[512]; snprintf(src, sizeof src, "sp_%sArray_get(%s, _t%d)", k, rb.p ? rb.p : "NULL", t);
        int box0 = zlv0->type == TY_POLY && et != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p0);
        if (box0) emit_boxed_text(c, et, src, b);
        else buf_puts(b, src);
        buf_puts(b, ";\n");
      }
      if (p1n && zlv1 && ob.p) {
        char src2[512]; snprintf(src2, sizeof src2, "sp_%sArray_get(%s, _t%d)", k2, ob.p, t);
        int box1 = zlv1->type == TY_POLY && et2 != TY_POLY;
        emit_indent(b, indent + 1); buf_printf(b, "lv_%s = ", p1n);
        if (box1) emit_boxed_text(c, et2, src2, b);
        else buf_puts(b, src2);
        buf_puts(b, ";\n");
      }
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      if (p0 && zs0 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, zs0); }
      if (p1n && zs1 > 0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p1n, zs1); }
      free(rb.p); free(ob.p);
      return 1;
    }
  }

  /* poly_val.each { |v| ... }: runtime-dispatch over a boxed array or hash */
  if (!strcmp(name, "each") && rt == TY_POLY && block >= 0 && p0) {
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "sp_RbVal _t%d = %s;\n", ta, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
    /* Root the boxed receiver so a GC fired by the loop body doesn't free a
       freshly-built collection held only by this temp. */
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d);\n", ta);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tn, ta);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    /* multi-param: auto-splat each poly element into params */
    int npp_poly = 0; while (block_param_name(c, block, npp_poly)) npp_poly++;
    if (npp_poly >= 2) {
      int telem = ++g_tmp;
      emit_indent(b, indent + 1);
      buf_printf(b, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);\n", telem, ta, ti);
      for (int pj = 0; pj < npp_poly; pj++) {
        const char *pnj = block_param_name(c, block, pj);
        if (!pnj) break;
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_poly_arr_get_hash(_t%d, (mrb_int)%d);\n", rename_local(pnj), telem, pj);
      }
    }
    else {
      emit_indent(b, indent + 1);
      buf_printf(b, "lv_%s = sp_poly_each_elem(_t%d, _t%d);\n", p0, ta, ti);
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each { |x| ... } */
  if (!strcmp(name, "each") && rt == TY_POLY_ARRAY) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    int ta = ++g_tmp;
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    Scope *cs_pa = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer_pa = (p0 && cs_pa) ? scope_local(cs_pa, p0) : NULL;
    int ts_pa = 0;
    if (outer_pa) {
      ts_pa = ++g_tmp; Buf ot_pa; memset(&ot_pa, 0, sizeof ot_pa); emit_ctype(c, outer_pa->type, &ot_pa);
      emit_indent(b, indent); buf_printf(b, "%s _t%d = lv_%s;\n", ot_pa.p ? ot_pa.p : "sp_RbVal", ts_pa, p0); free(ot_pa.p);
    }
    emit_indent(b, indent);
    buf_printf(b, "sp_PolyArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    /* Root the receiver: a freshly-built array referenced only by this temp
       is otherwise freed if the loop body triggers GC mid-iteration, leaving
       the next element fetch dangling. */
    emit_indent(b, indent);
    buf_printf(b, "SP_GC_ROOT(_t%d);\n", ta);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", t, t, ta, t);
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed */
      const char *orig_p0n = block_param_name(c, block, 0);
      Scope *blk_sp = comp_scope_of(c, block);
      LocalVar *bp0p = orig_p0n ? scope_local(blk_sp, orig_p0n) : NULL;
      TyKind bp0_tp = bp0p ? bp0p->type : TY_UNKNOWN;
      int npp = 0; while (block_param_name(c, block, npp)) npp++;
      int did_destruct = 0;
      if (npp >= 2 && bp0_tp != TY_POLY && bp0_tp != TY_UNKNOWN) {
        const char *inner_kk = array_kind(ty_array_of(bp0_tp));
        if (inner_kk) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(_t%d, _t%d).v.p;\n",
                     inner_kk, tsub, inner_kk, ta, t);
          for (int pj = 0; pj < npp; pj++) {
            const char *pnj = block_param_name(c, block, pj);
            if (!pnj) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pnj), inner_kk, tsub, pj);
          }
          did_destruct = 1;
        }
      }
      if (!did_destruct) {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", p0, ta, t);
      }
    }
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer_pa) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts_pa); }
    return 1;
  }
  if ((!strcmp(name, "each") || !strcmp(name, "each_entry") || !strcmp(name, "reverse_each")) &&
      ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int rev = !strcmp(name, "reverse_each");
    int t = ++g_tmp, tn = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, recv, &rb);
    /* Detect block param shadowing an outer variable; save/restore to preserve outer value */
    TyKind et = p0 ? ty_array_elem(rt) : TY_UNKNOWN;
    Scope *cs = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *outer = (p0 && cs) ? scope_local(cs, p0) : NULL;
    int box_to_poly = outer && outer->type == TY_POLY && et != TY_POLY;
    int ts = 0;
    if (outer) {
      /* Block params shadow outer variables in Ruby; save and restore */
      ts = ++g_tmp;
      Buf ot_ea; memset(&ot_ea, 0, sizeof ot_ea); emit_ctype(c, outer->type, &ot_ea);
      emit_indent(b, indent);
      buf_printf(b, "%s _t%d = lv_%s;\n", ot_ea.p ? ot_ea.p : "sp_RbVal", ts, p0); free(ot_ea.p);
    }
    if (rev) { emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = sp_%sArray_length(%s);\n", tn, k, rb.p); }
    emit_indent(b, indent);
    if (rev) buf_printf(b, "for (mrb_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", t, tn, t, t);
    else {
      buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(", t, t, k);
      buf_puts(b, rb.p); buf_printf(b, "); _t%d++) {\n", t);
    }
    if (p0) {
      /* Destructuring: 2+ params over poly_array where params are scalar-typed
         (e.g. `[[1,2],[3,4]].each { |a,b| }` or numbered `{ _1; _2 }`).
         The poly element is an inner typed array; unbox and destructure. */
      Scope *blk_s = comp_scope_of(c, block);
      /* Use original (unrenameD) name for scope lookup; p0 is already renamed */
      const char *orig_p0_name = block_param_name(c, block, 0);
      LocalVar *bp0 = orig_p0_name ? scope_local(blk_s, orig_p0_name) : NULL;
      TyKind bp0_type = bp0 ? bp0->type : TY_UNKNOWN;
      int np = 0; while (block_param_name(c, block, np)) np++;
      if (np >= 2 && !strcmp(k, "Poly") && bp0_type != TY_POLY && bp0_type != TY_UNKNOWN) {
        /* Get the inner array kind from the first param's element type */
        const char *inner_k = array_kind(ty_array_of(bp0_type));
        if (inner_k) {
          int tsub = ++g_tmp;
          emit_indent(b, indent + 1);
          buf_printf(b, "sp_%sArray *_t%d = (sp_%sArray *)sp_PolyArray_get(", inner_k, tsub, inner_k);
          buf_puts(b, rb.p); buf_printf(b, ", _t%d).v.p;\n", t);
          for (int pj = 0; pj < np; pj++) {
            const char *pname2 = block_param_name(c, block, pj);
            if (!pname2) continue;
            emit_indent(b, indent + 1);
            buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, %d);\n",
                       rename_local(pname2), inner_k, tsub, pj);
          }
          goto each_body;
        }
      }
      emit_indent(b, indent + 1);
      if (box_to_poly) {
        if (et == TY_INT) buf_printf(b, "lv_%s = sp_box_int(sp_%sArray_get(", p0, k);
        else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(sp_%sArray_get(", p0, k);
        else if (et == TY_FLOAT) buf_printf(b, "lv_%s = sp_box_float(sp_%sArray_get(", p0, k);
        else if (et == TY_BOOL) buf_printf(b, "lv_%s = sp_box_bool(sp_%sArray_get(", p0, k);
        else buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d)", t);
        if (et == TY_INT || et == TY_STRING || et == TY_FLOAT || et == TY_BOOL) buf_puts(b, ")");
        buf_puts(b, ";\n");
      }
      else {
        buf_printf(b, "lv_%s = sp_%sArray_get(", p0, k);
        buf_puts(b, rb.p); buf_printf(b, ", _t%d);\n", t);
      }
    }
    each_body:
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    if (outer) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, ts); }
    free(rb.p);
    return 1;
  }

  /* int_array.combination(k) { |c| ... } -- yield each k-combination as a
     fresh int_array */
  if (!strcmp(name, "combination") && rt == TY_INT_ARRAY) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac != 1) return 0;
    int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); buf_printf(b, "{ sp_IntArray *_t%d = ", ta); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n"); free(rb.p);
    emit_indent(b, indent + 1); buf_printf(b, "sp_PtrArray *_t%d = sp_IntArray_combination(_t%d, ", tc, ta); emit_expr(c, av[0], b); buf_puts(b, "); SP_GC_ROOT(_t"); buf_printf(b, "%d);\n", tc);
    emit_indent(b, indent + 1); buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tc, ti);
    if (p0) {
      Scope *cbsc = comp_scope_of(c, block);
      LocalVar *clv = cbsc ? scope_local(cbsc, p0) : NULL;
      TyKind cpt = clv ? clv->type : TY_UNKNOWN;
      emit_indent(b, indent + 2);
      if (cpt == TY_POLY || cpt == TY_UNKNOWN)
        buf_printf(b, "lv_%s = sp_box_obj((sp_IntArray *)_t%d->data[_t%d], SP_BUILTIN_INT_ARRAY);\n", p0, tc, ti);
      else
        buf_printf(b, "lv_%s = (sp_IntArray *)_t%d->data[_t%d];\n", p0, tc, ti);
    }
    emit_loop_body(c, body, b, indent + 2);
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_cons(n) { |a, b, ...| } -- sliding window of n consecutive
     elements; a single param binds the n-element sub-array, multiple params
     destructure the window */
  if (!strcmp(name, "each_cons") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int ec = 0; const int *eav = args >= 0 ? nt_arr(nt, args, "arguments", &ec) : NULL;
    if (ec != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np = 0; while (block_param_name(c, block, np)) np++;
    int ta = ++g_tmp, tnn = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tnn); emit_expr(c, eav[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, tnn, k, ta, ti);
    if (np == 1) {
      const char *pn = block_param_name(c, block, 0);
      const char *rpn = rename_local(pn);
      Scope *csc_ec = comp_scope_of(c, block);
      LocalVar *clv_ec = csc_ec ? scope_local(csc_ec, pn) : NULL;
      TyKind csaved_ec = clv_ec ? clv_ec->type : TY_UNKNOWN;
      int use_shadow_ec = clv_ec && clv_ec->type != rt && rt != TY_UNKNOWN;
      if (use_shadow_ec) {
        int bodyBn = 0; const int *bodyBb = body >= 0 ? nt_arr(nt, body, "body", &bodyBn) : NULL;
        clv_ec->type = rt;
        for (int j = 0; j < bodyBn; j++) infer_type(c, bodyBb[j]);
        emit_indent(b, indent + 1); buf_puts(b, "{\n");
        emit_indent(b, indent + 2); emit_ctype(c, rt, b);
        buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_loop_body(c, body, b, indent + 2);
        emit_indent(b, indent + 1); buf_puts(b, "}\n");
        clv_ec->type = csaved_ec;
      }
      else {
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", rpn, k, ta, ti, tnn);
        emit_loop_body(c, body, b, indent + 1);
      }
    }
    else {
      for (int pj = 0; pj < np; pj++) {
        const char *pn = block_param_name(c, block, pj);
        emit_indent(b, indent + 1);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, indent + 1);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* (a..b).each { |i| ... } -- any range-typed receiver */
  if (!strcmp(name, "each") && rt == TY_RANGE && p0) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_Range _t%d = ", t); buf_puts(b, rb.p ? rb.p : ""); buf_puts(b, ";\n");
    free(rb.p);
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = _t%d.first; lv_%s <= _t%d.last - _t%d.excl; lv_%s++) {\n",
               p0, t, p0, t, t, p0);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* n.upto(m) / n.downto(m) { |i| ... } */
  if ((!strcmp(name, "upto") || !strcmp(name, "downto")) && rt == TY_INT && p0) {
    int up = !strcmp(name, "upto");
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = NULL;
    if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
    if (argc != 1) return 0;
    Buf lo; memset(&lo, 0, sizeof lo); emit_expr(c, recv, &lo);
    Buf hi; memset(&hi, 0, sizeof hi); emit_expr(c, argv[0], &hi);
    emit_indent(b, indent);
    buf_printf(b, "for (lv_%s = ", p0); buf_puts(b, lo.p);
    buf_printf(b, "; lv_%s %s ", p0, up ? "<=" : ">="); buf_puts(b, hi.p);
    buf_printf(b, "; lv_%s%s) {\n", p0, up ? "++" : "--");
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent); buf_puts(b, "}\n");
    free(lo.p); free(hi.p);
    return 1;
  }

  /* "a".upto("e") { |c| ... } -- string succ-sequence loop, mirrors
     sp_StrArray_from_string_range semantics (inclusive, 4096-cap) */
  if (!strcmp(name, "upto") && rt == TY_STRING && p0) {
    int args = nt_ref(nt, id, "arguments");
    int argc = 0;
    const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
    if (argc != 1) return 0;
    int te = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp, tcmp = ++g_tmp;
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", te); emit_expr(c, argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "const char *_t%d = ", tc); emit_expr(c, recv, b); buf_puts(b, ";\n");
    emit_indent(b, indent); buf_printf(b, "for (int _t%d = 0; _t%d < 4096; _t%d++) {\n", ti, ti, ti);
    emit_indent(b, indent + 1); buf_printf(b, "int _t%d = strcmp(_t%d, _t%d);\n", tcmp, tc, te);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d > 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "lv_%s = _t%d;\n", p0, tc);
    emit_loop_body(c, body, b, indent + 1);
    emit_indent(b, indent + 1); buf_printf(b, "if (_t%d == 0) break;\n", tcmp);
    emit_indent(b, indent + 1); buf_printf(b, "_t%d = sp_str_succ(_t%d);\n", tc, tc);
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* recv.tap { |p| body } -- run block for side effects, preserve outer var */
  if (!strcmp(name, "tap") && recv >= 0) {
    TyKind et = infer_type(c, recv);
    Scope *tsc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *tlv0 = (tsc && p0) ? scope_local(tsc, p0) : NULL;
    TyKind tsaved0 = tlv0 ? tlv0->type : TY_UNKNOWN;
    int use_shadow_t = tlv0 && tlv0->type != et && et != TY_UNKNOWN;
    int tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, et, b);
    buf_printf(b, " _t%d = %s;\n", tr, rb.p ? rb.p : ""); free(rb.p);
    if (use_shadow_t) {
      int tbody_bn = 0; const int *tbody_bb = body >= 0 ? nt_arr(nt, body, "body", &tbody_bn) : NULL;
      tlv0->type = et;
      for (int j = 0; j < tbody_bn; j++) infer_type(c, tbody_bb[j]);
      emit_indent(b, indent); buf_puts(b, "{\n");
      emit_indent(b, indent + 1); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = _t%d;\n", p0, tr);
      emit_loop_body(c, body, b, indent + 1);
      emit_indent(b, indent); buf_puts(b, "}\n");
      tlv0->type = tsaved0;
    }
    else {
      if (p0) { emit_indent(b, indent); buf_printf(b, "lv_%s = _t%d;\n", p0, tr); }
      emit_loop_body(c, body, b, indent);
    }
    return 1;
  }

  /* array.cycle(n) { |p| body } -- repeat n times over the array */
  if (!strcmp(name, "cycle") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int cyc_argc = 0; const int *cyc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &cyc_argc) : NULL;
    if (cyc_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    TyKind et = ty_array_elem(rt);
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_cy = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int ta = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", tn);
    emit_expr(c, cyc_argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
    emit_indent(b, indent + 1);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", tj, tj, k, ta, tj);
    int innerIndent = indent + 2;
    if (use_shadow_cy) {
      int cyb_bn = 0; const int *cyb_bb = body >= 0 ? nt_arr(nt, body, "body", &cyb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < cyb_bn; j++) infer_type(c, cyb_bb[j]);
      emit_indent(b, innerIndent); buf_puts(b, "{\n"); innerIndent++;
      emit_indent(b, innerIndent); emit_ctype(c, et, b);
      buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj);
      emit_loop_body(c, body, b, innerIndent);
      innerIndent--;
      emit_indent(b, innerIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, innerIndent); buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, tj); }
      emit_loop_body(c, body, b, innerIndent);
    }
    emit_indent(b, indent + 1); buf_puts(b, "}\n");
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* array.each_slice(n) { |p| body } -- yield subarrays of size n */
  if (!strcmp(name, "each_slice") && ty_is_array(rt)) {
    int args = nt_ref(nt, id, "arguments");
    int es_argc = 0; const int *es_argv = args >= 0 ? nt_arr(nt, args, "arguments", &es_argc) : NULL;
    if (es_argc != 1) return 0;
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) return 0;
    int np_es = 0; while (block_param_name(c, block, np_es)) np_es++;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_es = np_es == 1 && clv0 && clv0->type != rt && rt != TY_UNKNOWN;
    int ta = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "mrb_int _t%d = ", ts);
    emit_expr(c, es_argv[0], b); buf_puts(b, ";\n");
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
               ti, ti, k, ta, ti, ts);
    int bodyIndent = indent + 1;
    if (np_es > 1) {
      /* multi-param: destructure slice elements into individual params */
      for (int pj = 0; pj < np_es; pj++) {
        const char *pn = block_param_name(c, block, pj);
        if (!pn) break;
        emit_indent(b, bodyIndent);
        buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n", rename_local(pn), k, ta, ti, pj);
      }
      emit_loop_body(c, body, b, bodyIndent);
    }
    else if (use_shadow_es) {
      int esb_bn = 0; const int *esb_bb = body >= 0 ? nt_arr(nt, body, "body", &esb_bn) : NULL;
      clv0->type = rt;
      for (int j = 0; j < esb_bn; j++) infer_type(c, esb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); emit_ctype(c, rt, b);
      buf_printf(b, " lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts);
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n", p0, k, ta, ti, ts); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  /* str.scan(/re/) { |m| body } -- iterate over regex matches */
  if (!strcmp(name, "scan") && rt == TY_STRING) {
    int args = nt_ref(nt, id, "arguments");
    int sc_argc = 0; const int *sc_argv = args >= 0 ? nt_arr(nt, args, "arguments", &sc_argc) : NULL;
    if (sc_argc != 1 || re_lit_index(c, sc_argv[0]) < 0) return 0;
    TyKind et = TY_STRING;
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int use_shadow_sc = clv0 && clv0->type != et && et != TY_UNKNOWN;
    int tm = ++g_tmp, ti = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(b, indent);
    buf_printf(b, "sp_StrArray *_t%d = sp_re_scan(sp_re_pat_%d, %s);\n",
               tm, re_lit_index(c, sc_argv[0]), rb.p ? rb.p : ""); free(rb.p);
    emit_indent(b, indent); buf_printf(b, "SP_GC_ROOT(_t%d);\n", tm);
    emit_indent(b, indent);
    buf_printf(b, "for (mrb_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {\n",
               ti, ti, tm, ti);
    int bodyIndent = indent + 1;
    if (use_shadow_sc) {
      int scb_bn = 0; const int *scb_bb = body >= 0 ? nt_arr(nt, body, "body", &scb_bn) : NULL;
      clv0->type = et;
      for (int j = 0; j < scb_bn; j++) infer_type(c, scb_bb[j]);
      emit_indent(b, bodyIndent); buf_puts(b, "{\n"); bodyIndent++;
      emit_indent(b, bodyIndent); buf_printf(b, "const char *lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti);
      emit_loop_body(c, body, b, bodyIndent);
      bodyIndent--;
      emit_indent(b, bodyIndent); buf_puts(b, "}\n");
      clv0->type = csaved0;
    }
    else {
      if (p0) { emit_indent(b, bodyIndent); buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d);\n", p0, tm, ti); }
      emit_loop_body(c, body, b, bodyIndent);
    }
    emit_indent(b, indent); buf_puts(b, "}\n");
    return 1;
  }

  return 0;
}

/* ---- interpolation ---- */

