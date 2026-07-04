#include "codegen_internal.h"

const int *call_args(const NodeTable *nt, int id, int *argc) {
  *argc = 0;
  int args = nt_ref(nt, id, "arguments");
  return args >= 0 ? nt_arr(nt, args, "arguments", argc) : NULL;
}


/* Rewrite a printf format that uses named references into a positional one.
   `%<name>SPEC` -> `%SPEC`, `%{name}` -> `%s` (Ruby's to_s of the value), `%%`
   stays. The referenced names (in order) are collected into names[]/name_len[]
   (capacity maxn). Returns the ref count, or -1 (caller falls through) when the
   format has no named reference, more than maxn of them, a name too long for
   the caller's buffer, or a mix of named and positional specifiers (which Ruby
   rejects). */
static int parse_named_format(const char *fmt, Buf *rew, const char **names,
                              int *name_len, int maxn) {
  int n = 0, has_named = 0, has_positional = 0;
  for (const char *p = fmt; *p; ) {
    if (*p != '%') { buf_putn(rew, p, 1); p++; continue; }
    if (p[1] == '%') { buf_puts(rew, "%%"); p += 2; continue; }
    if (p[1] == '<' || p[1] == '{') {
      char close = p[1] == '<' ? '>' : '}';
      const char *e = strchr(p + 2, close);
      if (!e) { buf_putn(rew, p, 1); p++; continue; }
      int len = (int)(e - (p + 2));
      /* the caller copies the name into a fixed char[128]; reject a longer name
         rather than silently truncating it to the wrong symbol. */
      if (n >= maxn || len >= 128) return -1;
      names[n] = p + 2; name_len[n] = len; n++;
      has_named = 1;
      p = e + 1;
      if (close == '}') { buf_puts(rew, "%s"); continue; }  /* %{name} -> to_s */
      buf_puts(rew, "%");
      while (*p && !strchr("diouxXeEfgGscbB", *p)) { buf_putn(rew, p, 1); p++; }  /* flags/width/prec */
      if (*p) { buf_putn(rew, p, 1); p++; }  /* conversion char */
      continue;
    }
    has_positional = 1;
    buf_putn(rew, p, 1); p++;
  }
  /* Ruby raises ArgumentError on a mix of named and positional specifiers; bail
     so the caller falls back rather than emitting misaligned arguments. */
  if (has_named && has_positional) return -1;
  return has_named ? n : -1;
}

/* A regexp's encoding is US-ASCII when its source is 7-bit clean (the common
   case), else UTF-8. Spinel is ASCII/UTF-8 only, so this covers the supported
   domain; fixed_encoding? is true exactly when the encoding is not US-ASCII. */
static int re_src_all_ascii(const char *s) {
  if (!s) return 1;
  for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
  return 1;
}

/* A backreference (\1..\9, \k<name>, \g<name>) defeats the linear-time matcher,
   so Regexp.linear_time? is false for such a pattern and true otherwise. */
static int re_src_has_backref(const char *s) {
  if (!s) return 0;
  /* Inside a [...] character class a `\1` is an octal escape and `\k`/`\g` are
     literal, none of them backreferences. Track class membership (classes do
     not nest; the first `]` closes) so those don't false-positive. */
  int in_class = 0;
  for (; *s; s++) {
    if (*s == '\\' && s[1]) {
      char n = s[1];
      if (!in_class && ((n >= '1' && n <= '9') || n == 'k' || n == 'g')) return 1;
      s++;  /* skip the escaped char (in or out of a class) */
      continue;
    }
    if (in_class) { if (*s == ']') in_class = 0; }
    else if (*s == '[') in_class = 1;
  }
  return 0;
}

int emit_ctor_yield_inline(Compiler *c, int id, int ci, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  int mi = comp_method_in_chain(c, ci, "initialize", NULL);
  if (mi < 0 || !c->scopes[mi].yields) return 0;
  Scope *m = &c->scopes[mi];
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
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
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (needs_root(lv->type)) { emit_indent(b, din); buf_printf(b, lv->type == TY_POLY ? "SP_GC_ROOT_RBVAL(lv_%s);\n" : "SP_GC_ROOT(lv_%s);\n", rn); }
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

/* Emit `node` as a `sp_Bigint *` for a mixed bigint operand (arithmetic or
   comparison where the other side is bigint): a bigint stays itself, a poly is
   narrowed with sp_poly_as_bigint, and anything else (a plain int) is promoted
   with sp_bigint_new_int. Not for the int64 exponent/shift argument of pow or
   the shift operators, which stays an int. */
static void emit_bigint_operand(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_BIGINT) { emit_expr(c, node, b); return; }
  if (t == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, node, b); buf_puts(b, ")"); return; }
  buf_puts(b, "sp_bigint_new_int("); emit_expr(c, node, b); buf_puts(b, ")");
}

/* `s[i]` on a string with a single non-negative-style int index. Records the
   string receiver and index nodes. Used to fold `s[i] == "c"` into a raw byte
   comparison (no per-access 1-char string allocation). */
static int str_index1(Compiler *c, int node, int *out_recv, int *out_idx) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, node);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm || (!sp_streq(nm, "[]") && !sp_streq(nm, "slice"))) return 0;
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
  if (!ty || !sp_streq(ty, "StringNode")) return 0;
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
  if (ity && sp_streq(ity, "IntegerNode") && nt_int(c->nt, si, "value", 0) < 0) return 0;
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
  if (sp_streq(ty, "BreakNode") || sp_streq(ty, "NextNode")) return 1;
  /* constructs that bind their own break/next */
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (ie_body_has_break_next(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(nt, node, i, &n); for (int k = 0; k < n; k++) if (ie_body_has_break_next(c, ids[k])) return 1; }
  return 0;
}

/* Iterators whose normal return value is the receiver (Ruby returns self) and
   that have no value-producing emitter -- the valued-break wrapper emits these
   as a statement and uses the receiver as the no-break result. */
static int brk_iter_returns_self(const char *name) {
  if (!name) return 0;
  return sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "each_pair") || sp_streq(name, "each_value") ||
         sp_streq(name, "each_key") || sp_streq(name, "each_entry") ||
         sp_streq(name, "reverse_each");
}

/* A receiver safe to evaluate twice (once by the iterator's loop, once as the
   break wrapper's no-break result): a read with no side effects. A CallNode or
   anything else falls through, so a side-effecting receiver is not duplicated. */
static int brk_recv_is_pure(Compiler *c, int recv) {
  if (recv < 0) return 0;
  const char *t = nt_type(c->nt, recv);
  if (!t) return 0;
  return sp_streq(t, "LocalVariableReadNode") || sp_streq(t, "InstanceVariableReadNode") ||
         sp_streq(t, "ClassVariableReadNode") || sp_streq(t, "GlobalVariableReadNode") ||
         sp_streq(t, "ConstantReadNode") || sp_streq(t, "ConstantPathNode") ||
         sp_streq(t, "SelfNode") || sp_streq(t, "ArrayNode") || sp_streq(t, "HashNode") ||
         sp_streq(t, "RangeNode") || sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
         sp_streq(t, "StringNode") || sp_streq(t, "SymbolNode");
}

/* Unify the value type of every splice-bound break/next in `node` (same
   binding rules as ie_body_has_break_next). TY_UNKNOWN if none carry a value.
   Sizes the splice result temp so a `next <poly>` (e.g. an int ivar widened in
   promote mode) is not dropped into a narrower mrb_int slot. */
static TyKind ie_splice_value_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "BreakNode") || sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    return an > 0 ? comp_ntype(c, av[0]) : TY_UNKNOWN;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind s = ie_splice_value_ty(c, nt_ref_at(nt, node, i));
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = ie_splice_value_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
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

/* Emit the switch key for a poly method dispatch. An SP_TAG_OBJ value uses its
   real cls_id; a boxed scalar maps to its reopened primitive class index (so a
   reopened Integer/Float/String/Symbol/nil method still dispatches), else to a
   sentinel matching no case -- this keeps a plain scalar (cls_id 0) from
   aliasing a regular user class that happens to occupy index 0. */
static void emit_poly_dispatch_key(Compiler *c, int tv, int cls0_cand, Buf *b) {
  /* Every boxed scalar (int/float/str/sym/nil/bool) carries cls_id 0, which
     aliases the first user class (index 0). When class 0 is a candidate of this
     dispatch -- it defines/inherits the method, so it emits a `case 0:` arm --
     a scalar receiver would wrongly enter that arm and deref its v.p (issue
     #1576: `value.to_s` on a poly int, with a user class 0 defining `to_s`,
     segfaulted). Guard the key so a non-object value maps to a sentinel that
     matches no case and falls through to the default/poly arm. When class 0 is
     not a candidate (no `case 0:` arm), a scalar's cls_id 0 already matches
     nothing, so the plain key is correct -- and cheaper on the hot per-dispatch
     path (optcarrot's per-frame tick), so keep it there. */
  if (!g_promote_mode) {
    if (cls0_cand) buf_printf(b, "(_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff)", tv, tv);
    else buf_printf(b, "_t%d.cls_id", tv);
    return;
  }
  static const struct { const char *tag, *cls; } P[] = {
    {"SP_TAG_INT", "Integer"}, {"SP_TAG_FLT", "Float"},
    {"SP_TAG_STR", "String"},  {"SP_TAG_SYM", "Symbol"},
    {"SP_TAG_NIL", "NilClass"},
  };
  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id", tv, tv);
  for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++) {
    int idx = comp_class_index(c, P[i].cls);
    if (idx >= 0) buf_printf(b, " : _t%d.tag == %s ? %d", tv, P[i].tag, idx);
  }
  buf_puts(b, " : 0x7fffffff)");
}


/* eval(string) / Kernel.eval(string) compiling an arbitrary runtime string is a
   hard AOT boundary, not a missing feature. If node `id` is such a call, emit
   the intentional diagnostic and return 1; otherwise return 0. Shared by
   emit_call and the output builtins (puts/print/p) so `puts eval(s)` gets the
   same specific message as `x = eval(s)` rather than a generic argument dump.
   The instance_eval/class_eval/module_eval block forms carry a literal block,
   not a string, and are handled separately -- they never reach here. */
int diagnose_eval_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, id);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "eval")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc < 1) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (!rty || (!sp_streq(rty, "ConstantReadNode") && !sp_streq(rty, "ConstantPathNode"))) return 0;
    const char *rnm = nt_str(nt, recv, "name");
    if (!rnm || !sp_streq(rnm, "Kernel")) return 0;
  }
  unsupported(c, id, "eval of a runtime string is not supported by AOT compilation (define the code statically)");
  return 1;
}

/* Emit the `<argc>, (mrb_int[16]){...}` argument tail of an sp_proc_call.
   A TY_POLY argument does not fit the mrb_int slot, so it is published to the
   _sp_proc_poly_args side-channel and a heap-pointer argument is laundered
   through (mrb_int)(uintptr_t). Shared by the <proc>.call path and the
   lowered-yield emission.

   force_poly is set for a first-class proc value's `.call`: such a proc is
   type-erased at the call site, so its parameter types are unknown here. A
   poly parameter in the callee reads its argument back from the poly
   side-channel, so every argument -- including a concrete-typed one -- must be
   boxed and published, not just the statically-poly ones. (A `yield` knows its
   block's parameter types, so it passes force_poly=0 and keeps the lean ABI.) */
void emit_proc_call_args(Compiler *c, int argc, const int *argv, Buf *b, int force_poly) {
  int nargs = argc < 16 ? argc : 16;  /* proc-call ABI caps args at mrb_int[16] */
  int any_poly = force_poly;
  for (int k = 0; k < nargs && !any_poly; k++) if (comp_ntype(c, argv[k]) == TY_POLY) any_poly = 1;
  buf_printf(b, "%d, ", argc);
  if (any_poly) {
    if (!g_needs_proc_poly_argslot) {
      g_needs_proc_poly_argslot = 1;
      buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_args[16];\n");
    }
    /* Each argument is evaluated once into a natural-typed temp so it can be
       published both unboxed (the mrb_int[] slot, for a concrete parameter)
       and boxed (the side-channel, for a poly parameter). A nil/unknown arg
       has no storable C type; it rides an mrb_int temp and boxes to nil. */
    int atmp[16];
    for (int k = 0; k < nargs; k++) {
      TyKind at = comp_ntype(c, argv[k]);
      int storable = ty_is_object(at) || c_type_name(at) != NULL;
      atmp[k] = ++g_tmp;
      /* render the value into a side buffer first: emit_expr drains the arg's
         own prelude (e.g. a nested proc call) into g_pre, which must land
         before -- not inside -- this temp's declaration line. */
      Buf vb = expr_buf(c, argv[k]);
      emit_indent(g_pre, g_indent);
      if (storable) emit_ctype(c, at, g_pre); else buf_puts(g_pre, "mrb_int");
      buf_printf(g_pre, " _t%d = %s;\n", atmp[k], vb.p ? vb.p : "");
      /* Root a GC-managed temp: a later argument's evaluation, or sp_proc_call
         itself, can allocate and collect before the callee reads the value back
         from the (un-scanned) side-channel. Use the type-correct macro. */
      if (at == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      else if (proc_slot_is_ptr(at)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]); }
      free(vb.p);
    }
    for (int k = 0; k < nargs; k++) {
      TyKind at = comp_ntype(c, argv[k]);
      int storable = ty_is_object(at) || c_type_name(at) != NULL;
      char tn[24]; snprintf(tn, sizeof tn, "_t%d", atmp[k]);
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "_sp_proc_poly_args[%d] = ", k);
      if (storable) emit_boxed_text(c, at, tn, g_pre); else buf_puts(g_pre, "sp_box_nil()");
      buf_puts(g_pre, ";\n");
    }
    buf_puts(b, "(mrb_int[16]){");
    for (int k = 0; k < nargs; k++) {
      TyKind at = comp_ntype(c, argv[k]);
      if (k) buf_puts(b, ", ");
      if (at == TY_POLY) buf_printf(b, "sp_poly_to_i(_t%d)", atmp[k]);
      else if (proc_slot_is_ptr(at)) buf_printf(b, "(mrb_int)(uintptr_t)_t%d", atmp[k]);
      else buf_printf(b, "_t%d", atmp[k]);
    }
    if (nargs == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
    buf_puts(b, "})");
  }
  else {
    buf_puts(b, "(mrb_int[16]){");
    for (int k = 0; k < nargs; k++) {
      if (k) buf_puts(b, ", ");
      if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[k], b);
    }
    if (nargs == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
    buf_puts(b, "})");
  }
}

/* Emit a node as an sp_Rational value: a Rational stays as-is, an Integer is
   lifted to n/1. Used to coerce the other operand of a Rational arithmetic /
   comparison op. */
static void emit_rat_coerce(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_RATIONAL) { emit_expr(c, node, b); return; }
  buf_puts(b, "sp_rational_new((mrb_int)("); emit_expr(c, node, b); buf_puts(b, "), 1)");
}
/* Emit a node as an sp_Complex: a Complex stays as-is, an Integer/Float
   becomes re+0i. Used to coerce the other operand of a Complex op. */
static void emit_complex_coerce(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_COMPLEX) { emit_expr(c, node, b); return; }
  buf_puts(b, "((sp_Complex){(mrb_float)("); emit_expr(c, node, b); buf_puts(b, "), 0})");
}

/* Returns 1 if `id` is a `Float::INFINITY` / `nil` / absent range endpoint. */
static int lazy_endpoint_is_infinite(Compiler *c, int right) {
  const NodeTable *nt = c->nt;
  if (right < 0) return 1;
  const char *rty = nt_type(nt, right);
  if (rty && sp_streq(rty, "NilNode")) return 1;
  if (rty && sp_streq(rty, "ConstantPathNode")) {
    const char *cpnm = nt_str(nt, right, "name");
    if (cpnm && sp_streq(cpnm, "INFINITY")) {
      int par = nt_ref(nt, right, "parent");
      const char *parnm = (par >= 0 && nt_type(nt, par) &&
                           sp_streq(nt_type(nt, par), "ConstantReadNode"))
                          ? nt_str(nt, par, "name") : NULL;
      if (parnm && sp_streq(parnm, "Float")) return 1;
    }
  }
  return 0;
}

/* (int-range | int-array).lazy.<map/select/reject/filter/take_while...>
   .{first(n) | take(n) | to_a | force}: fuse the whole lazy chain into one int
   loop collecting into an sp_IntArray, short-circuiting at the terminal count.
   Int-typed throughout (source and every stage stay mrb_int). Returns 1 if it
   handled the call. */
int emit_lazy_pipeline_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *tname = nt_str(nt, id, "name");
  if (!tname) return 0;
  /* `first(n)` / `to_a` / `force` force the lazy chain; `take(n)` stays lazy in
     CRuby (returns another Lazy) so it is not a forcing terminal here. */
  int is_first = sp_streq(tname, "first");
  int is_toa = sp_streq(tname, "to_a") || sp_streq(tname, "force");
  if (!is_first && !is_toa) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;

  int has_count = 0, count_node = -1;
  {
    int ar = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
    if (is_first) {
      if (ac == 1) { has_count = 1; count_node = av[0]; }
      else return 0;   /* first with no count: a different handler */
    }
    else if (ac != 0) return 0;
  }

  enum { OP_MAP, OP_FILTER, OP_TAKEWHILE };
  struct { int kind; int block; int negate; } ops[16];
  int nops = 0, cur = recv, lazy_src = -1;
  while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
    const char *nm = nt_str(nt, cur, "name");
    if (!nm) return 0;
    if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) {
      lazy_src = unwrap_parens(c, nt_ref(nt, cur, "receiver"));
      break;
    }
    int blk = nt_ref(nt, cur, "block");
    if (blk < 0 || nops >= 16) return 0;
    if (sp_streq(nm, "map") || sp_streq(nm, "collect")) ops[nops].kind = OP_MAP, ops[nops].negate = 0;
    else if (sp_streq(nm, "select") || sp_streq(nm, "filter")) ops[nops].kind = OP_FILTER, ops[nops].negate = 0;
    else if (sp_streq(nm, "reject")) ops[nops].kind = OP_FILTER, ops[nops].negate = 1;
    else if (sp_streq(nm, "take_while")) ops[nops].kind = OP_TAKEWHILE, ops[nops].negate = 0;
    else return 0;
    ops[nops].block = blk;
    nops++;
    cur = nt_ref(nt, cur, "receiver");
  }
  if (lazy_src < 0) return 0;

  TyKind st = infer_type(c, lazy_src);
  int src_is_range = (st == TY_RANGE), src_is_intarr = (st == TY_INT_ARRAY);
  if (!src_is_range && !src_is_intarr) return 0;

  int excl = 0, endless = 0, right = -1, left_n = -1;
  if (src_is_range) {
    excl = (int)(nt_int(nt, lazy_src, "flags", 0) & 4) ? 1 : 0;
    right = nt_ref(nt, lazy_src, "right");
    endless = lazy_endpoint_is_infinite(c, right);
    left_n = nt_ref(nt, lazy_src, "left");
  }
  if (is_toa && (src_is_range && endless)) return 0;   /* would not terminate */

  /* prelude temps. The pipeline is poly-typed: lazy block params infer poly, so
     each stage carries a boxed value and collects into a PolyArray. */
  int tres = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  int tn = -1;
  if (has_count) {
    Buf nb = expr_buf(c, count_node);
    tn = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = %s;\n", tn, nb.p ? nb.p : "0"); free(nb.p);
  }
  int thi = -1, tsrc = -1;
  if (src_is_range && !endless) {
    Buf hb = expr_buf(c, right);
    thi = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = %s;\n", thi, hb.p ? hb.p : "0"); free(hb.p);
  }
  if (src_is_intarr) {
    Buf sb = expr_buf(c, lazy_src);
    tsrc = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", tsrc, sb.p ? sb.p : "0", tsrc); free(sb.p);
  }

  int tloop = ++g_tmp, tv = ++g_tmp;
  Buf lo_b; memset(&lo_b, 0, sizeof lo_b);
  if (src_is_range) emit_expr(c, left_n, &lo_b);
  emit_indent(g_pre, g_indent);
  const char *climit = has_count ? "" : NULL;
  char cbuf[64]; cbuf[0] = 0;
  if (has_count) snprintf(cbuf, sizeof cbuf, " && sp_PolyArray_length(_t%d) < _t%d", tres, tn);
  (void)climit;
  if (src_is_range) {
    if (endless)
      buf_printf(g_pre, "for (mrb_int _t%d = %s; 1%s; _t%d++) {\n", tloop, lo_b.p ? lo_b.p : "0", cbuf, tloop);
    else
      buf_printf(g_pre, "for (mrb_int _t%d = %s; _t%d %s _t%d%s; _t%d++) {\n",
                 tloop, lo_b.p ? lo_b.p : "0", tloop, excl ? "<" : "<=", thi, cbuf, tloop);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_int(_t%d);\n", tv, tloop);
  }
  else {
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d)%s; _t%d++) {\n",
               tloop, tloop, tsrc, cbuf, tloop);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_int(sp_IntArray_get(_t%d, _t%d)); SP_GC_ROOT_RBVAL(_t%d);\n", tv, tsrc, tloop, tv);
  }
  free(lo_b.p);

  char vbuf[24]; snprintf(vbuf, sizeof vbuf, "_t%d", tv);
  /* ops are collected terminal-first; apply them source-first. */
  for (int oi = nops - 1; oi >= 0; oi--) {
    int blk = ops[oi].block;
    const char *bp0 = block_param_name(c, blk, 0);
    const char *bp = (bp0 && bp0[0]) ? rename_local(bp0) : "_lx";
    /* The running value is boxed (poly); the block param may infer a narrower
       type, so unbox to match its C type. */
    Scope *bs = comp_scope_of(c, blk);
    LocalVar *plv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
    TyKind pt = (plv && plv->type != TY_UNKNOWN) ? plv->type : TY_POLY;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = ", bp);
    if (pt == TY_POLY) buf_puts(g_pre, vbuf);
    else { Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, pt, vbuf, &ub); buf_puts(g_pre, ub.p ? ub.p : vbuf); free(ub.p); }
    buf_puts(g_pre, ";\n");
    int bbody = nt_ref(nt, blk, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], g_pre, g_indent + 1);
    if (bn < 1) continue;
    if (ops[oi].kind == OP_MAP) {
      Buf eb; memset(&eb, 0, sizeof eb);
      int svind = g_indent; g_indent += 1; emit_boxed(c, bb[bn - 1], &eb); g_indent = svind;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "%s = %s;\n", vbuf, eb.p ? eb.p : "sp_box_nil()"); free(eb.p);
    }
    else {
      Buf cb; memset(&cb, 0, sizeof cb);
      int svind = g_indent; g_indent += 1; emit_cond(c, bb[bn - 1], &cb); g_indent = svind;
      emit_indent(g_pre, g_indent + 1);
      if (ops[oi].kind == OP_TAKEWHILE)
        buf_printf(g_pre, "if (!(%s)) break;\n", cb.p ? cb.p : "0");
      else if (ops[oi].negate)
        buf_printf(g_pre, "if (%s) continue;\n", cb.p ? cb.p : "0");
      else
        buf_printf(g_pre, "if (!(%s)) continue;\n", cb.p ? cb.p : "0");
      free(cb.p);
    }
  }
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tres, vbuf);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Dynamic `recv.send(name, args)` over a runtime name: desugar_dynamic_send
   stashed one synthesized `recv.m(args)` arm per candidate method name. Emit a
   chain `name == :m1 ? recv.m1(args) : ... : raise NoMethodError`, boxing each
   arm (the result is poly). Arms whose call did not resolve on the receiver
   (UNKNOWN type -- wrong name or arity for this receiver) are dropped. */
static int emit_dynamic_send(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int narm = 0; const int *arms = nt_arr(nt, id, "dyn_send_arms", &narm);
  if (narm <= 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1 || !argv) return 0;
  int sym = argv[0];
  TyKind st = comp_ntype(c, sym);
  int t = ++g_tmp;
  buf_printf(b, "({ sp_sym _t%d = ", t);
  if (st == TY_SYMBOL) emit_expr(c, sym, b);
  else if (st == TY_STRING) { buf_puts(b, "sp_sym_intern("); emit_expr(c, sym, b); buf_puts(b, ")"); }
  else { buf_puts(b, "sp_sym_intern(sp_poly_to_s("); emit_boxed(c, sym, b); buf_puts(b, "))"); }
  buf_printf(b, "; sp_RbVal _r%d; ", t);
  Buf *sv_pre = g_pre;
  for (int k = 0; k < narm; k++) {
    int arm = arms[k];
    TyKind at = comp_ntype(c, arm);
    if (at == TY_UNKNOWN || at == TY_VOID) continue;     /* did not resolve on this receiver/arity */
    const char *nm = nt_str(nt, arm, "name");
    if (!nm) continue;
    /* Emit the arm into private buffers under a silent probe: a method that
       resolves by type but not by codegen (e.g. wrong arity for a builtin)
       longjmps out of emit and the arm is simply dropped. Its preludes are
       captured to `pre` and replayed inside the arm's own branch so they run
       only when this arm is taken. */
    Buf pre = {0, 0, 0}, body = {0, 0, 0};
    g_pre = &pre;
    int sv_probe = g_unsup_probe; g_unsup_probe = 1;
    volatile int ok;
    if (setjmp(g_unsup_recover) == 0) { emit_expr(c, arm, &body); ok = 1; }
    else ok = 0;
    g_unsup_probe = sv_probe;
    g_pre = sv_pre;
    if (ok) {
      buf_printf(b, "if (_t%d == sp_sym_intern(\"%s\")) { ", t, nm);
      if (pre.p && pre.len) buf_puts(b, pre.p);
      buf_printf(b, "_r%d = ", t);
      emit_boxed_text(c, at, body.p ? body.p : "0", b);
      buf_puts(b, "; } else ");
    }
    free(pre.p); free(body.p);
  }
  buf_printf(b, "{ sp_raise_cls(\"NoMethodError\", sp_sprintf(\"undefined method '%%s'\", sp_sym_to_s(_t%d))); _r%d = sp_box_nil(); } _r%d; })", t, t, t);
  return 1;
}

static int emit_concurrency_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  /* Thread instance methods (a green thread on the scheduler) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_THREAD) {
    if (sp_streq(name, "value") && argc == 0) {
      buf_puts(b, "sp_Thread_value("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "join") && argc == 0) {
      buf_puts(b, "sp_Thread_join("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "alive?") && argc == 0) {
      buf_puts(b, "sp_Thread_alive("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "report_on_exception") && argc == 0) {
      buf_puts(b, "sp_Thread_get_report("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "report_on_exception=") && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_thread *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Thread_set_report(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, "); })");
      return 1;
    }
    if (sp_streq(name, "status") && argc == 0) {
      buf_puts(b, "sp_Thread_status("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "name") && argc == 0) {
      buf_puts(b, "sp_Thread_get_name("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "name=") && argc == 1) {
      buf_puts(b, "sp_Thread_set_name("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "kill") || sp_streq(name, "exit") || sp_streq(name, "terminate")) && argc == 0) {
      buf_puts(b, "sp_Thread_kill("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "equal?") && argc == 1 && comp_ntype(c, argv[0]) == TY_THREAD) {
      buf_puts(b, "((void *)("); emit_expr(c, recv, b);
      buf_puts(b, ") == (void *)("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1;
    }
    if (sp_streq(name, "raise")) {
      /* #raise: deliver an exception to the thread (it fires when the thread next
         runs). Argument forms mirror Kernel#raise; an exception object is unpacked
         into (cls, msg, obj) since sp_exc_* are TU-static (cf Fiber#raise). */
      TyKind a0t = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int arg0_const = argc >= 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "ConstantReadNode") ||
         sp_streq(nt_type(nt, argv[0]), "ConstantPathNode"));
      int arg0_exc = a0t == TY_EXCEPTION ||
        (ty_is_object(a0t) && class_is_exc_subclass(c, ty_object_class(a0t)));
      if (argc >= 1 && arg0_exc) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_thread *_tr%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Exception *_te%d = (sp_Exception *)(", t); emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_Thread_raise(_tr%d, sp_exc_class_name(_te%d), sp_exc_message(_te%d), _te%d); })",
                   t, t, t, t);
        return 1;
      }
      buf_puts(b, "sp_Thread_raise("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (arg0_const) {
        buf_printf(b, "\"%s\", ", nt_str(nt, argv[0], "name"));
        if (argc >= 2) emit_expr(c, argv[1], b); else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ", NULL");
      }
      else if (argc >= 1) { buf_puts(b, "\"RuntimeError\", "); emit_expr(c, argv[0], b); buf_puts(b, ", NULL"); }
      else buf_puts(b, "\"RuntimeError\", (&(\"\\xff\")[1]), NULL");
      buf_puts(b, ")");
      return 1;
    }
    /* thread-local storage: t[:key] / t[:key]=v / t.key?(:key) (symbol keys) */
    if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_SYMBOL) {
      buf_puts(b, "sp_Thread_tls_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2 && comp_ntype(c, argv[0]) == TY_SYMBOL) {
      buf_puts(b, "sp_Thread_tls_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "key?") && argc == 1 && comp_ntype(c, argv[0]) == TY_SYMBOL) {
      buf_puts(b, "sp_Thread_tls_key("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
    }
  }

  /* Mutex instance methods. synchronize is handled by the generic block handler
     below (it wraps the block in lock/unlock for a TY_MUTEX receiver). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_MUTEX) {
    if ((sp_streq(name, "lock") || sp_streq(name, "unlock")) && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_mutex *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Mutex_%s(_t%d); _t%d; })", sp_streq(name, "lock") ? "lock" : "unlock", t, t);
      return 1;
    }
    if (sp_streq(name, "try_lock") && argc == 0) {
      buf_puts(b, "sp_Mutex_try_lock("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "locked?") && argc == 0) {
      buf_puts(b, "sp_Mutex_locked("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "owned?") && argc == 0) {
      buf_puts(b, "sp_Mutex_owned("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
  }

  /* ConditionVariable instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_CONDVAR) {
    if (sp_streq(name, "wait") && argc >= 1) {
      /* wait(mutex): release the mutex, park, re-acquire. A timeout arg (argc==2)
         is accepted but ignored at N=1 (no real clock blocking). */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_condvar *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_CondVar_wait(_t%d, ", t); emit_expr(c, argv[0], b);
      buf_printf(b, "); _t%d; })", t);
      return 1;
    }
    if ((sp_streq(name, "signal") || sp_streq(name, "broadcast")) && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_condvar *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_CondVar_%s(_t%d); _t%d; })", sp_streq(name, "signal") ? "signal" : "broadcast", t, t);
      return 1;
    }
  }

  /* Queue instance methods (a thread-safe FIFO on the scheduler) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_QUEUE) {
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "enq")) && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_queue *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Queue_push(_t%d, ", t); emit_boxed(c, argv[0], b);
      buf_printf(b, "); _t%d; })", t);
      return 1;
    }
    if ((sp_streq(name, "pop") || sp_streq(name, "shift") || sp_streq(name, "deq")) && argc == 0) {
      buf_puts(b, "sp_Queue_pop("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "size") || sp_streq(name, "length")) && argc == 0) {
      buf_puts(b, "sp_Queue_size("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "max") && argc == 0) {
      buf_puts(b, "sp_Queue_max("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "empty?") && argc == 0) {
      buf_puts(b, "sp_Queue_empty("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "closed?") && argc == 0) {
      buf_puts(b, "sp_Queue_closed("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if ((sp_streq(name, "close") || sp_streq(name, "clear")) && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_queue *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Queue_%s(_t%d); _t%d; })", sp_streq(name, "close") ? "close" : "clear", t, t);
      return 1;
    }
  }

  /* Fiber instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_FIBER) {
    if (sp_streq(name, "resume")) {
      buf_puts(b, "sp_Fiber_resume("); emit_expr(c, recv, b);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (comp_ntype(c, argv[k]) == TY_POLY) emit_expr(c, argv[k], b);
        else emit_boxed(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, ", sp_box_nil()");
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "alive?")) {
      buf_puts(b, "sp_Fiber_alive("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "kill") && argc == 0) {
      buf_puts(b, "sp_Fiber_kill("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (sp_streq(name, "transfer")) {
      buf_puts(b, "sp_Fiber_transfer("); emit_expr(c, recv, b);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (comp_ntype(c, argv[k]) == TY_POLY) emit_expr(c, argv[k], b);
        else emit_boxed(c, argv[k], b);
      }
      if (argc == 0) buf_puts(b, ", sp_box_nil()");
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "value")) {
      /* Fiber#value: resume until fiber finishes and return last yielded value. */
      buf_puts(b, "sp_Fiber_resume("); emit_expr(c, recv, b); buf_puts(b, ", sp_box_nil())");
      return 1;
    }
    if (sp_streq(name, "raise")) {
      /* Fiber#raise: inject an exception at the fiber's suspension point. The
         argument forms mirror Kernel#raise: (), ("msg"), (Class), (Class, "msg"),
         or (exc_object). The object form is unpacked into class-name/message here
         because sp_exc_class_name/_message are TU-static (unreachable from the
         fiber runtime), so the runtime takes (cls, msg, obj). */
      TyKind a0t = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int arg0_const = argc >= 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "ConstantReadNode") ||
         sp_streq(nt_type(nt, argv[0]), "ConstantPathNode"));
      int arg0_exc = a0t == TY_EXCEPTION ||
        (ty_is_object(a0t) && class_is_exc_subclass(c, ty_object_class(a0t)));
      if (argc >= 1 && arg0_exc) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Fiber *_fr%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Exception *_fe%d = (sp_Exception *)(", t); emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_Fiber_raise(_fr%d, sp_exc_class_name(_fe%d), sp_exc_message(_fe%d), _fe%d); })",
                   t, t, t, t);
        return 1;
      }
      buf_puts(b, "sp_Fiber_raise("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (arg0_const) {
        buf_printf(b, "\"%s\", ", nt_str(nt, argv[0], "name"));
        if (argc >= 2) emit_expr(c, argv[1], b);
        else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ", NULL");
      }
      else if (argc >= 1) {
        buf_puts(b, "\"RuntimeError\", "); emit_expr(c, argv[0], b); buf_puts(b, ", NULL");
      }
      else buf_puts(b, "\"RuntimeError\", (&(\"\\xff\")[1]), NULL");
      buf_puts(b, ")");
      return 1;
    }
  }
  return 0;
}

static int emit_complex_rational_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  /* ---- Complex / Rational value types ---- */
  /* Kernel#Complex(re[, im]) */
  if (recv < 0 && sp_streq(name, "Complex") && argc >= 1) {
    buf_puts(b, "((sp_Complex){(mrb_float)(");
    emit_expr(c, argv[0], b);
    buf_puts(b, "), (mrb_float)(");
    if (argc >= 2) emit_expr(c, argv[1], b);
    else buf_puts(b, "0");
    buf_puts(b, ")})");
    return 1;
  }
  if (recv < 0 && sp_streq(name, "Rational") && (argc == 1 || argc == 2)) {
    buf_puts(b, "sp_rational_new((mrb_int)(");
    emit_expr(c, argv[0], b);
    buf_puts(b, "), (mrb_int)(");
    if (argc == 2) emit_expr(c, argv[1], b);
    else buf_puts(b, "1");
    buf_puts(b, "))");
    return 1;
  }
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    /* Complex.polar(magnitude, angle) */
    if (rrty && sp_streq(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Complex") && sp_streq(name, "polar") && argc >= 1) {
      buf_puts(b, "sp_complex_polar(");
      emit_float_expr(c, argv[0], b);
      buf_puts(b, ", ");
      if (argc >= 2) emit_float_expr(c, argv[1], b);
      else buf_puts(b, "0");
      buf_puts(b, ")");
      return 1;
    }
    TyKind crt = comp_ntype(c, recv);
    if (crt == TY_COMPLEX) {
      if (sp_streq(name, "real"))      { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").re"); return 1; }
      if (sp_streq(name, "imaginary") || sp_streq(name, "imag")) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").im"); return 1; }
      if (sp_streq(name, "conjugate") || sp_streq(name, "conj")) { buf_puts(b, "sp_complex_conjugate("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "abs") || sp_streq(name, "magnitude")) && argc == 0) { buf_puts(b, "sp_complex_abs("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "abs2") && argc == 0) { buf_puts(b, "sp_complex_abs2("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "-@") && argc == 0) { buf_puts(b, "sp_complex_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "+@") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if ((sp_streq(name, "to_c")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "to_s")) { buf_puts(b, "sp_complex_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "inspect")) { buf_puts(b, "sp_complex_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      TyKind cxa = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int cx_ok = cxa == TY_COMPLEX || cxa == TY_INT || cxa == TY_FLOAT;
      if (cx_ok && argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") ||
                                 sp_streq(name, "*") || sp_streq(name, "/"))) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_complex_%s(", fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_complex_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (argc == 1 && sp_streq(name, "**") && cxa == TY_INT) {
        buf_puts(b, "sp_complex_pow("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      if (cx_ok && argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!="))) {
        buf_printf(b, "(%ssp_complex_eq(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b); buf_puts(b, ", "); emit_complex_coerce(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
    }
    /* Integer/Float <op> Complex: lift the scalar to re+0i. */
    if ((crt == TY_INT || crt == TY_FLOAT) && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
      if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_complex_%s(((sp_Complex){(mrb_float)(", fn); emit_expr(c, recv, b);
        buf_puts(b, "), 0}), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "==") || sp_streq(name, "!=")) {
        buf_printf(b, "(%ssp_complex_eq(((sp_Complex){(mrb_float)(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b);
        buf_puts(b, "), 0}), "); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
    }
    /* Proc#curry and curry application. */
    if (crt == TY_PROC && sp_streq(name, "curry") && argc == 0) {
      buf_puts(b, "sp_curry_new("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (crt == TY_CURRY && (sp_streq(name, "[]") || sp_streq(name, "call") || sp_streq(name, "()")) && argc == 1) {
      /* The application that reaches the proc's arity realizes the curry to its
         (int) result; earlier applications return another curry. */
      int complete = 0; TyKind cret = TY_UNKNOWN;
      int realize = curry_apply_info(c, id, &complete, &cret) && complete && cret == TY_INT;
      if (realize) buf_puts(b, "sp_curry_to_int(");
      buf_puts(b, "sp_curry_apply("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)(");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
      if (realize) buf_puts(b, ")");
      return 1;
    }
    if (crt == TY_INT && sp_streq(name, "quo") && argc == 1) {
      buf_puts(b, "sp_rational_new((mrb_int)(");
      emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)(");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if (crt == TY_RATIONAL) {
      if (sp_streq(name, "numerator"))   { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").num"); return 1; }
      if (sp_streq(name, "denominator")) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").den"); return 1; }
      if (sp_streq(name, "to_s")) { buf_puts(b, "sp_rational_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "inspect")) { buf_puts(b, "sp_rational_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "to_f")) && argc == 0) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "truncate")) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num / ("); emit_expr(c, recv, b); buf_puts(b, ").den)"); return 1; }
      if (sp_streq(name, "-@") && argc == 0) { buf_puts(b, "sp_rational_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "+@") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "abs") && argc == 0) { buf_puts(b, "sp_rational_abs("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      TyKind rat = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      /* Only Integer/Rational/Float operands are modeled (a poly operand --
         e.g. a Rational read out of a poly array, which has no box form yet --
         falls through to the generic path rather than miscompiling). */
      int rat_ok = rat == TY_RATIONAL || rat == TY_INT || rat == TY_FLOAT;
      /* arithmetic against another Rational or an Integer yields a Rational;
         against a Float, coerce self to float (CRuby semantics). */
      if (rat_ok && argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") ||
                        sp_streq(name, "*") || sp_streq(name, "/"))) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        if (rat == TY_FLOAT) {
          const char *op = name;
          buf_puts(b, "(sp_rational_to_f("); emit_expr(c, recv, b); buf_printf(b, ") %s ", op); emit_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        buf_printf(b, "sp_rational_%s(", fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (rat_ok && argc == 1 && sp_streq(name, "**")) {
        if (rat == TY_INT) { buf_puts(b, "sp_rational_pow("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1; }
        buf_puts(b, "pow(sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, "), "); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (rat_ok && argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") ||
                        sp_streq(name, "<=") || sp_streq(name, ">="))) {
        buf_puts(b, "(sp_rational_cmp("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_printf(b, ") %s 0)", name);
        return 1;
      }
      if (rat_ok && argc == 1 && sp_streq(name, "<=>")) {
        buf_puts(b, "sp_rational_cmp("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!="))) {
        if (rat == TY_RATIONAL || rat == TY_INT) {
          buf_printf(b, "(%ssp_rational_eq(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, "))");
          return 1;
        }
      }
    }
    /* Integer <op> Rational: lift the Integer to n/1 (covers `2/3r`, `1 + r`). */
    if (crt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
      if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_rational_%s(sp_rational_new((mrb_int)(", fn); emit_expr(c, recv, b);
        buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") || sp_streq(name, ">=")) {
        buf_puts(b, "(sp_rational_cmp(sp_rational_new((mrb_int)("); emit_expr(c, recv, b);
        buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_printf(b, ") %s 0)", name);
        return 1;
      }
      if (sp_streq(name, "<=>")) {
        buf_puts(b, "sp_rational_cmp(sp_rational_new((mrb_int)("); emit_expr(c, recv, b);
        buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "==") || sp_streq(name, "!=")) {
        buf_printf(b, "(%ssp_rational_eq(sp_rational_new((mrb_int)(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b);
        buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
    }
  }
  return 0;
}

static int emit_poly_method_dispatch(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  /* poly method dispatch: switch on the boxed object's cls_id and call the
     matching class's method (walking the chain for inherited methods),
     unboxing the pointer. */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    int is_lengthlike = sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count");
    int is_empty = sp_streq(name, "empty?");
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
      /* class 0 emits a `case 0:` arm here when it defines/inherits the method
         (nrequired 0) or exposes it as a reader; the dispatch key is then guarded
         so a boxed scalar (cls_id 0) does not alias it (issue #1576). */
      int cls0_d = -1, cls0_rd = -1;
      int cls0_mi = c->nclasses > 0 ? comp_method_in_chain(c, 0, name, &cls0_d) : -1;
      int cls0_cand = ((cls0_mi >= 0 && c->scopes[cls0_mi].nrequired == 0) ||
                       (c->nclasses > 0 && comp_reader_in_chain(c, 0, name, &cls0_rd))) &&
                      c->nclasses > 0 && c->classes[0].instantiated;
      buf_puts(b, "switch (");
      emit_poly_dispatch_key(c, tv, cls0_cand, b);
      buf_puts(b, ") {");
      for (int k = 0; k < c->nclasses; k++) {
        /* A never-instantiated class can't be this poly value's runtime class,
           so drop its arm (method or reader); the referenced symbol then DCEs
           as an unreferenced static (#1608). */
        if (!c->classes[k].instantiated) continue;
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        /* Skip a method with no standalone definition to call: DCE-pruned (its
           params stayed TY_UNKNOWN so it was marked unreachable) or inlined at
           call sites because it yields. Emitting a `case` arm that calls the
           absent `sp_Class_method` symbol dangles at link (issue #1583). The
           class can never be the receiver of this poly value anyway. */
        if (mi >= 0 && c->scopes[mi].nrequired == 0 && scope_has_callable_symbol(c, mi)) {
          /* Build the call; append default values for any optional params
             not provided by the (zero-arg) call site. */
          Buf cb; memset(&cb, 0, sizeof cb);
          /* A reopened primitive (Integer/Float/String/Symbol) method takes the
             unboxed value, not a struct pointer -- read the matching union field
             instead of casting .v.p to a non-existent sp_<Prim> struct. */
          const char *_dcn = c->classes[defcls].name;
          char _dself[64];
          if (sp_streq(_dcn, "Integer") || sp_streq(_dcn, "Numeric")) snprintf(_dself, sizeof _dself, "_t%d.v.i", tv);
          else if (sp_streq(_dcn, "Float")) snprintf(_dself, sizeof _dself, "_t%d.v.f", tv);
          else if (sp_streq(_dcn, "String")) snprintf(_dself, sizeof _dself, "_t%d.v.s", tv);
          else if (sp_streq(_dcn, "Symbol")) snprintf(_dself, sizeof _dself, "(sp_sym)_t%d.v.i", tv);
          else snprintf(_dself, sizeof _dself, "(sp_%s *)_t%d.v.p", _dcn, tv);
          buf_printf(&cb, "sp_%s_%s(%s", _dcn, mc(c->scopes[mi].name), _dself);
          if (c->scopes[mi].nparams > 0) {
            const char *saved_self = g_self;
            static char selfpbuf[64];
            snprintf(selfpbuf, sizeof selfpbuf, "%s", _dself);
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
            TyKind slotty = is_scalar_ret(ret) ? ret : TY_INT;
            buf_printf(b, "_t%d = ", tr);
            if (ret == TY_POLY && c->scopes[mi].ret != TY_POLY) emit_boxed_text(c, c->scopes[mi].ret, call, b);
            /* The slot is scalar (e.g. a length dispatch fixed to mrb_int) but
               this class's method widened its return to poly: coerce down. */
            else if (ret != TY_POLY && c->scopes[mi].ret == TY_POLY) emit_unbox_text(c, slotty, call, b);
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
      if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) {
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY: _t%d = %ssp_IntArray_length((sp_IntArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_length((sp_StrArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_length((sp_FloatArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = %ssp_PolyArray_length((sp_PolyArray *)_t%d.v.p)%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = %s((sp_PolyPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = %s((sp_SymPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = %s((sp_StrPolyHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        /* scalar-valued str/int-keyed hashes (a `params = {}` filled with a
           computed String key is a StrStrHash) reach a poly `.length` dispatch
           once any user `#length` exists -- without these arms the switch missed
           the cls_id and returned the seed 0 (#1614). */
        buf_printf(b, " case SP_BUILTIN_STR_STR_HASH: _t%d = %s((sp_StrStrHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_STR_INT_HASH: _t%d = %s((sp_StrIntHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
        buf_printf(b, " case SP_BUILTIN_INT_STR_HASH: _t%d = %s((sp_IntStrHash *)_t%d.v.p)->len%s; break;", tr, bopen, tv, bclose);
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
        buf_printf(b, " case SP_BUILTIN_STR_STR_HASH: _t%d = %s((sp_StrStrHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_STR_INT_HASH: _t%d = %s((sp_StrIntHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
        buf_printf(b, " case SP_BUILTIN_INT_STR_HASH: _t%d = %s((sp_IntStrHash *)_t%d.v.p)->len == 0%s; break;", tr, ebopen, tv, ebclose);
      }
      /* compare_by_identity? on a poly-carried hash: every spinel hash is
         value-keyed (the mutating variant is a compile error), so any hash
         tag answers false; a non-hash receiver falls through to the gate. */
      if (sp_streq(name, "compare_by_identity?")) {
        buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH:"
                      " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_STR_STR_HASH:"
                      " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_INT_STR_HASH:"
                      " _t%d = %s0%s; break;", tr, ebopen, ebclose);
      }
      /* to_s / inspect are universal: a poly value that is a builtin scalar
         (int, float, string, ...) rather than one of the enumerated user
         classes still answers them. Without a default arm the result stayed
         the empty-string default, so `@x.to_s` on a poly-widened int printed
         blank. Route the fallthrough through the runtime poly converter. */
      if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) {
        const char *pfn = sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect";
        buf_printf(b, " default: _t%d = ", tr);
        if (ret == TY_POLY) buf_printf(b, "sp_box_str(%s(_t%d))", pfn, tv);
        else buf_printf(b, "%s(_t%d)", pfn, tv);
        buf_puts(b, "; break;");
      }
      buf_printf(b, " } _t%d; })", tr);
      return 1;
    }
  }

  /* poly method dispatch with arguments: switch on the boxed object's cls_id
     and call the matching user method (or a builtin array `[]`), passing the
     arguments evaluated once into temps. */
  if (recv >= 0 && rt == TY_POLY && argc > 0) {
    /* the builtin-array `[]` / Integer#[] bit-ref arm applies to an integer
       index; in promote mode that index variable may have widened to poly, so
       accept poly too (the index is unboxed where it is used below). */
    int is_index = sp_streq(name, "[]") && argc == 1 &&
                   (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY);
    /* `fetch(key[, default])` on a poly value that is actually a str/sym-keyed
       hash: without a user `fetch` candidate the dispatch was skipped and the
       call collapsed to default_value (an empty string), dropping the lookup.
       The str/sym-keyed hash arms below handle it, so admit it here. */
    int is_fetch = sp_streq(name, "fetch") && (argc == 1 || argc == 2) &&
                   (infer_type(c, argv[0]) == TY_STRING || infer_type(c, argv[0]) == TY_SYMBOL);
    int is_include = (sp_streq(name, "include?") || sp_streq(name, "member?") ||
                      sp_streq(name, "has_key?") || sp_streq(name, "key?")) && argc == 1;
    /* push/<</append on a poly value that is actually a builtin array: the
       array-mutate statement path skips it when a user class also defines the
       name (the value could be that object), so the switch needs a builtin-array
       arm or the append is silently dropped. sp_poly_shl handles every array
       kind; the user arms above cover the object case. */
    int is_push = (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1;
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      /* Include if call supplies all required params (pad defaults / truncate extras) */
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand > 0 || is_index || is_include || is_fetch || is_push) {
      TyKind ret = comp_ntype(c, id);
      int tv = ++g_tmp, tr = ++g_tmp;
      int *atmp = malloc(sizeof(int) * argc);
      TyKind *atmp_ty = malloc(sizeof(TyKind) * argc);
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
      for (int a = 0; a < argc; a++) {
        atmp[a] = ++g_tmp;
        TyKind at = infer_type(c, argv[a]);
        /* A nil/void/unresolved arg has no concrete C storage (emit_ctype would
           print `void`); hold it as a boxed poly so it can flow into a poly
           param slot. */
        if (at == TY_NIL || at == TY_VOID || at == TY_UNKNOWN) {
          atmp_ty[a] = TY_POLY;
          buf_printf(b, "sp_RbVal _t%d = ", atmp[a]); emit_boxed(c, argv[a], b); buf_puts(b, "; ");
        }
        else {
          atmp_ty[a] = at;
          emit_ctype(c, at, b);
          buf_printf(b, " _t%d = ", atmp[a]); emit_expr(c, argv[a], b); buf_puts(b, "; ");
        }
      }
      emit_ctype(c, is_scalar_ret(ret) ? ret : TY_INT, b);
      /* Seed the result temp. For `fetch(key, default)` the seed IS the
         supplied default, so a receiver whose runtime variant matches no switch
         arm (e.g. an empty `{}` that boxed as PolyPolyHash) still yields the
         default rather than a bare default_value() (an empty string). */
      buf_printf(b, " _t%d = ", tr);
      if (is_fetch && argc == 2) {
        char dn[40]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
        if (ret == TY_POLY) emit_boxed_text(c, infer_type(c, argv[1]), dn, b);
        else buf_puts(b, dn);
      }
      else buf_puts(b, is_scalar_ret(ret) ? default_value(ret) : "0");
      buf_puts(b, "; ");
      /* include? on a TAG_STR receiver: check tag before entering cls_id switch */
      if (is_include && infer_type(c, argv[0]) == TY_STRING)
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_str_include(_t%d.v.s, _t%d); } else ", tv, tr, tv, atmp[0]);
      /* The builtin index/bit-ref arms use the index as a raw mrb_int; unbox it
         when the index temp widened to poly (promote mode). */
      char idxref[64];
      if (is_index && atmp_ty[0] == TY_POLY) snprintf(idxref, sizeof idxref, "sp_poly_to_i(_t%d)", atmp[0]);
      else snprintf(idxref, sizeof idxref, "_t%d", atmp[0]);
      /* Integer#[N] bit-extraction: poly recv may hold a tagged int */
      if (is_index) {
        if (ret == TY_POLY)
          buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = sp_box_int((_t%d.v.i >> %s) & 1); } else ", tv, tr, tv, idxref);
        else
          buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = (_t%d.v.i >> %s) & 1; } else ", tv, tr, tv, idxref);
      }
      /* class 0 emits a `case 0:` arm here when it defines/inherits the method
         with its arity satisfied; guard the key so a boxed scalar (cls_id 0)
         cannot alias it (issue #1576). */
      int cls0_mi2 = c->nclasses > 0 ? comp_method_in_chain(c, 0, name, NULL) : -1;
      int cls0_cand2 = cls0_mi2 >= 0 && argc >= c->scopes[cls0_mi2].nrequired &&
                       c->classes[0].instantiated;
      buf_puts(b, "switch (");
      emit_poly_dispatch_key(c, tv, cls0_cand2, b);
      buf_puts(b, ") {");
      for (int k = 0; k < c->nclasses; k++) {
        int defcls = -1;
        int mi = comp_method_in_chain(c, k, name, &defcls);
        if (mi < 0 || argc < c->scopes[mi].nrequired) continue;
        /* A class no value can ever be (never `.new`/`.allocate`/`raise`d, no
           Struct, no Marshal escape) cannot be this poly value's receiver, so
           its arm is dead. Dropping it makes sp_<Class>_<name> an unreferenced
           static the C compiler then DCEs -- spinel supplies the accurate
           reference graph, the C compiler removes the code (#1608). */
        if (!c->classes[k].instantiated) continue;
        /* Skip a method with no standalone definition (DCE-pruned, or inlined
           at call sites because it yields): a `case` arm calling its absent
           `sp_Class_method` symbol would dangle at link. The class can't be
           this poly value's receiver anyway -- that is why it was pruned, and a
           yielding method's value-position dispatch is moot here (issue #1583). */
        if (!scope_has_callable_symbol(c, mi)) continue;
        /* A candidate whose concrete key parameter type is incompatible with the
           concrete call-site key cannot be this poly value's receiver for that
           key -- e.g. a Symbol-keyed user `[]` reached by a String key, where the
           value's real class is a string-keyed Hash. Passing the key raw would be
           a C pointer/integer type error (const char * into an sp_sym slot), so
           skip the arm. Mirrors the key-type-mismatch handling for typed hashes. */
        int arm_key_incompat = 0;
        for (int a = 0; a < c->scopes[mi].nparams && a < argc; a++) {
          LocalVar *pv0 = (c->scopes[mi].pnames && c->scopes[mi].pnames[a])
                            ? scope_local(&c->scopes[mi], c->scopes[mi].pnames[a]) : NULL;
          TyKind pt0 = pv0 ? pv0->type : TY_UNKNOWN;
          TyKind at0 = atmp_ty[a];
          int pc = pt0 != TY_POLY && pt0 != TY_UNKNOWN && pt0 != TY_NIL && pt0 != TY_VOID;
          int ac = at0 != TY_POLY && at0 != TY_UNKNOWN && at0 != TY_NIL && at0 != TY_VOID;
          if (pc && ac && pt0 != at0 && (pt0 == TY_STRING || at0 == TY_STRING)) {
            arm_key_incompat = 1; break;
          }
        }
        if (arm_key_incompat) continue;
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
            TyKind at = atmp_ty[a];   /* the temp's actual type (poly for a nil/void arg) */
            char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
            if (pt == TY_POLY && at != TY_POLY) emit_boxed_text(c, at, tn, &cb);
            else if (at == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) emit_unbox_text(c, pt, tn, &cb);
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
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_box_int(sp_IntArray_get((sp_IntArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
          buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_box_str(sp_StrArray_get((sp_StrArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
          buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_box_float(sp_FloatArray_get((sp_FloatArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
          buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
        }
        else {
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_get((sp_IntArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
        }
      }
      if (is_push) {
        /* The value is a builtin array: append each (boxed) arg via sp_poly_shl,
           which dispatches on the array kind. `push`/`<<`/`append` return the
           receiver, so yield it when the result is used (chained). */
        buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY:");
        for (int a = 0; a < argc; a++) {
          char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
          Buf ab; memset(&ab, 0, sizeof ab);
          if (atmp_ty[a] == TY_POLY) buf_puts(&ab, tn);
          else emit_boxed_text(c, atmp_ty[a], tn, &ab);
          buf_printf(b, " sp_poly_shl(_t%d, %s);", tv, ab.p ? ab.p : "sp_box_nil()");
          free(ab.p);
        }
        if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
        buf_puts(b, " break;");
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
        else if (at == TY_POLY) {
          /* promote: the include? arg widened to poly. A Range receiver
             (`case x when Range; x.include?(n)`) tests numeric membership, so
             unbox the arg; the PolyArray/PolyPolyHash arms below cover the
             container cases. */
          buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = sp_range_include((sp_Range *)_t%d.v.p, sp_poly_to_i(_t%d)); break;", tr, tv, atmp[0]);
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
      int is_aref = sp_streq(name, "[]") && argc == 1;
      int is_fetch = sp_streq(name, "fetch") && (argc == 1 || argc == 2);
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
      free(atmp_ty);
      return 1;
    }
  }
  return 0;
}

static int emit_class_new_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  /* Class.new(args) -> sp_<Class>_new(args) */
  if (recv >= 0 && sp_streq(name, "new")) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0 && c->classes[ci].is_struct) {
        /* Struct.new members: positional args, or keyword args mapping each
           member by name; each coerced to the member ivar type. */
        ClassInfo *cls = &c->classes[ci];
        int kwh = (argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
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
        return 1;
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
          return 1;
        }
        /* yielding initialize: inline its body at the call site (the block
           feeds the yields; the emitted constructor only allocates) */
        if (emit_ctor_yield_inline(c, id, ci, b)) return 1;
        /* user-defined def self.new takes precedence over the constructor */
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) {
          int defcls2 = -1; comp_cmethod_in_chain(c, ci, "new", &defcls2);
          buf_printf(b, "sp_%s_s_new(", c->classes[defcls2 >= 0 ? defcls2 : ci].name);
          emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
          return 1;
        }
        buf_printf(b, "sp_%s_new(", c->classes[ci].name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return 1;
      }
      const char *cn = nt_str(nt, recv, "name");
      if (cn && is_exc_name(cn)) {
        /* builtin exception class .new(msg) */
        buf_printf(b, "sp_exc_new(\"%s\", ", cn);
        if (argc >= 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ")");
        return 1;
      }
      if (cn && sp_streq(cn, "String")) {
        /* String.new / String.new(s): always create a mutable heap copy */
        if (argc == 1) { buf_puts(b, "sp_str_dup_external("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else buf_puts(b, "sp_str_dup_external((&(\"\\xff\")[1]))");
        return 1;
      }
      if (cn && sp_streq(cn, "Object") && argc == 0) {
        buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT)");
        return 1;
      }
      if (cn && (sp_streq(cn, "Mutex") || sp_streq(cn, "Monitor"))) {
        buf_puts(b, "sp_Mutex_new()"); return 1;
      }
      if (cn && sp_streq(cn, "ConditionVariable")) {
        buf_puts(b, "sp_CondVar_new()"); return 1;
      }
      if (cn && sp_streq(cn, "StringIO") && sp_feature_enabled("stringio")) {
        if (argc == 0) buf_puts(b, "sp_StringIO_new()");
        else if (argc == 1) { buf_puts(b, "sp_StringIO_new_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_StringIO_new_sm("); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
        return 1;
      }
      if (cn && sp_streq(cn, "Enumerator") && nt_ref(nt, id, "block") >= 0) {
        /* Enumerator.new { |y| ... }: a fiber-backed generator where `y << v`
           lowers to a Fiber.yield. */
        emit_fiber_new(c, id, b, 1);
        return 1;
      }
      if (cn && sp_streq(cn, "Thread") && nt_ref(nt, id, "block") >= 0) {
        /* Thread.new(arg): an eager green thread wrapping a fiber built exactly
           like a Fiber.new block (the block result lands in fiber->yielded_value,
           read back by #value). Thread.new's first argument becomes the block's
           first param on entry; it is handed to the scheduler as the thread arg. */
        buf_puts(b, "sp_Thread_spawn_fiber(");
        emit_fiber_new(c, id, b, 0);
        buf_puts(b, ", ");
        if (argc >= 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ")");
        return 1;
      }
      if (cn && sp_streq(cn, "Fiber") && nt_ref(nt, id, "block") >= 0) {
        emit_fiber_new(c, id, b, 0);
        return 1;
      }
      if (cn && sp_streq(cn, "Queue")) { buf_puts(b, "sp_Queue_new()"); return 1; }
      if (cn && sp_streq(cn, "SizedQueue") && argc == 1) {
        buf_puts(b, "sp_SizedQueue_new("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
      }
      if (cn && sp_streq(cn, "Random")) {
        buf_puts(b, "sp_Random_new(");
        if (argc >= 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "(mrb_int)time(NULL)");
        buf_puts(b, ")");
        return 1;
      }
      /* Hash.new { |hash, key| default } -> a StrPolyHash with a default-proc
         function computing the missing-key value. */
      if (cn && sp_streq(cn, "Hash") && nt_ref(nt, id, "block") >= 0) {
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
        int dp_self = (g_emitting_class_id >= 0 && g_self && sp_streq(g_self, "self") &&
                       g_self_deref && sp_streq(g_self_deref, "->"));
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
          int is_set = lty && sp_streq(lty, "CallNode") && nt_str(nt, last, "name") &&
                       sp_streq(nt_str(nt, last, "name"), "[]=");
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
        return 1;
      }
      if (cn && sp_streq(cn, "StringScanner") && argc == 1 && sp_feature_enabled("strscan")) {
        buf_puts(b, "sp_StringScanner_new("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (cn && sp_streq(cn, "Regexp") && argc >= 1) {
        int tp = ++g_tmp, ts = ++g_tmp;
        int flags = (argc >= 2) ? 1 : 0; /* Regexp::IGNORECASE=1 if 2nd arg truthy */
        /* Emit the pattern value into a local buffer first: an interpolated arg
           whose embedded call roots its own args pushes those decls to g_pre,
           which must land as whole statements BEFORE this temp's decl line, not
           inside its initializer. */
        Buf pv; memset(&pv, 0, sizeof pv);
        emit_expr(c, argv[0], &pv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
        free(pv.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), %d);\n",
                   tp, ts, ts, ts, flags);
        buf_printf(b, "_t%d", tp);
        return 1;
      }
      if (cn && sp_streq(cn, "Array") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_new()"); return 1;
      }
      if (cn && sp_streq(cn, "Array") && argc == 1 && nt_ref(nt, id, "block") < 0) {
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
        buf_printf(b, "_t%d", tr); return 1;
      }
      if (cn && sp_streq(cn, "Array") && nt_ref(nt, id, "block") >= 0) {
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
          if (sp_streq(k, "Poly")) {
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
        return 1;
      }
      if (cn && sp_streq(cn, "Array") && argc == 2) {
        /* Array.new(n, v) -> n copies of v */
        TyKind at = comp_ntype(c, id);
        const char *k = (at == TY_POLY_ARRAY) ? "Poly" : array_kind(at);
        if (k) {
          int tn = ++g_tmp, tv = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
          /* The size goes into an `mrb_int` temp; coerce a poly size expression
             (e.g. `nrows * ncols` where a factor widened to poly -> sp_poly_mul,
             which returns sp_RbVal) through sp_poly_to_i. spinel-dev#24. */
          Buf nb; memset(&nb, 0, sizeof nb); emit_int_expr(c, argv[0], &nb);
          Buf vb = expr_buf(c, argv[1]);
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
          return 1;
        }
      }
      if (cn && sp_streq(cn, "Time")) {
        if (argc == 0) { buf_puts(b, "sp_time_now()"); return 1; }
        buf_printf(b, "sp_time_new(");
        for (int i = 0; i < 6; i++) {
          if (i) buf_puts(b, ", ");
          if (i < argc) emit_expr(c, argv[i], b);
          else buf_puts(b, (i == 1 || i == 2) ? "1" : "0");
        }
        buf_puts(b, ")");
        return 1;
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
        return 1;
      }
    }
  }
  return 0;
}

/* True when the user `<=>` reachable from cid (or a subclass override) can
   return nil at runtime -- its unified return type is TY_POLY. The object
   comparison emitters (<, <=, >, >=, ==, between?) then route through the
   checked boxed comparators (sp_poly_cmp_ck / sp_poly_cmp_eq, dispatching
   the user `<=>` via sp_obj_cmp_hook) so an incomparable pair raises the
   Comparable ArgumentError like CRuby; a TY_INT `<=>` keeps the zero-cost
   inline `<op> 0` path. Value-type classes stay inline (never boxed). */
static int user_cmp_ret_poly(Compiler *c, int cid) {
  if (c->classes[cid].is_value_type) return 0;
  int def = -1;
  int mi = comp_method_in_chain(c, cid, "<=>", &def);
  TyKind ret = mi >= 0 ? (TyKind)c->scopes[mi].ret : TY_UNKNOWN;
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, "<=>", &kd);
    if (kmi >= 0 && (TyKind)c->scopes[kmi].ret != TY_UNKNOWN)
      ret = ty_unify(ret, (TyKind)c->scopes[kmi].ret);
  }
  return ret == TY_POLY;
}

/* Bind `node`'s boxed value to a fresh rooted sp_RbVal temp in g_pre and
   return the temp id. The comparison operand may be a fresh allocation whose
   only reference is this value, and the user `<=>` (or the other operand's
   evaluation) can allocate -- rooting keeps it live across those. */
static int hoist_boxed_rooted(Compiler *c, int node) {
  int t = ++g_tmp;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_boxed(c, node, &vb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
             t, vb.p ? vb.p : "sp_box_nil()", t);
  free(vb.p);
  return t;
}

static int emit_case_eq_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  /* `===` on a scalar comparable (bool/int/float/string/symbol) is case
     equality == value equality. Range/Class/Regexp `===` have their own
     handlers and fall through here. */
  if (argc == 1 && sp_streq(name, "===")) {
    int fr = eq_family(rt), fa = eq_family(a0);
    if (fr && fr != 5 && fa && fa != 5) {
      if (fr == fa) {
        if (fr == 2) { buf_puts(b, "sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), ("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      return 1;
    }
  }

  if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!="))) {
    int eq = sp_streq(name, "==");
    /* `x == nil` / `x != nil` for any receiver */
    int a_nil = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "NilNode");
    int r_nil = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "NilNode");
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
      return 1;
    }
    equality_skip_nil:;
    /* arr == [] : an array equals the empty literal iff it has no elements */
    {
      int er = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ea = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if ((er && (array_kind(a0) || a0 == TY_POLY_ARRAY)) ||
          (ea && (array_kind(rt) || rt == TY_POLY_ARRAY))) {
        int arr = er ? argv[0] : recv;
        TyKind at = er ? a0 : rt;
        const char *kk = array_kind(at);
        buf_printf(b, "(%ssp_%sArray_length(", eq ? "" : "!", kk ? kk : "Poly");
        emit_expr(c, arr, b); buf_puts(b, ") == 0)");
        return 1;
      }
    }
    if (rt == TY_POLY_ARRAY && a0 == TY_POLY_ARRAY) {
      buf_puts(b, eq ? "sp_PolyArray_eq(" : "(!sp_PolyArray_eq(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return 1;
    }
    /* two typed arrays of the same kind: element-wise compare */
    if (array_kind(rt) && rt == a0) {
      if (!eq) buf_puts(b, "(!");
      buf_printf(b, "sp_%sArray_eq(", array_kind(rt));
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return 1;
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
      return 1;
    }
    /* hash == hash */
    if (ty_is_hash(rt) || ty_is_hash(a0) || rt == TY_UNKNOWN || a0 == TY_UNKNOWN) {
      /* two empty hash literals are trivially equal */
      int re = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
      int ae = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "HashNode") &&
               ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
      if (re && ae) { buf_puts(b, eq ? "1" : "0"); return 1; }
      if (ty_is_hash(rt) && ty_is_hash(a0)) {
        if (rt == a0) {
          /* same typed hash: use the dedicated equality function */
          const char *hn = ty_hash_cname(rt);
          if (hn) {
            buf_puts(b, eq ? "" : "(!");
            buf_printf(b, "sp_%sHash_eq(", hn);
            emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
            buf_puts(b, eq ? ")" : "))");
            return 1;
          }
        }
        /* different hash types can never be equal */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return 1;
      }
      if (ty_is_hash(rt) || ty_is_hash(a0)) {
        /* hash vs non-hash */
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return 1;
      }
    }
    /* bigint == / != */
    if (rt == TY_BIGINT || a0 == TY_BIGINT) {
      buf_printf(b, "(sp_bigint_cmp(");
      emit_bigint_operand(c, recv, b);
      buf_puts(b, ", ");
      emit_bigint_operand(c, argv[0], b);
      buf_printf(b, ") %s 0)", eq ? "==" : "!=");
      return 1;
    }
    /* a poly operand compares dynamically (covers string-vs-poly etc.) */
    if (rt == TY_POLY || a0 == TY_POLY) {
      buf_puts(b, eq ? "sp_poly_eq(" : "(!sp_poly_eq(");
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, eq ? ")" : "))");
      return 1;
    }
    {
      int fr = eq_family(rt), fa = eq_family(a0);
      /* same comparable family: compare by value */
      if (fr && fa && fr == fa) {
        if (fr == 2 && emit_strchar_cmp(c, recv, argv[0], eq, b)) return 1;
        if (fr == 2) { buf_puts(b, eq ? "sp_str_eq(" : "(!sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else if (fr == 5) { buf_puts(b, eq ? "sp_range_eq(" : "(!sp_range_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", eq ? "==" : "!="); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return 1;
      }
      /* two different concrete types are never == in Ruby (no coercion);
         still evaluate both operands for their side effects */
      if (fr && fa) {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
        emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
        return 1;
      }
    }
    /* object == / != : try direct method, then fall back to <=> == 0 */
    if (recv >= 0 && ty_is_object(rt)) {
      int ecid = ty_object_class(rt);
      int emi = comp_method_in_chain(c, ecid, name, NULL);
      if (emi >= 0) {
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (sp_streq(rty2, "LocalVariableReadNode") ||
                     sp_streq(rty2, "InstanceVariableReadNode") ||
                     sp_streq(rty2, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t2 = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t2, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t2);
        }
        emit_dispatch(c, ecid, name, selfptr, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return 1;
      }
      /* no direct == : use <=> == 0 when the class supports Comparable */
      if (comp_method_in_chain(c, ecid, "<=>", NULL) >= 0) {
        /* a `<=>` that can return nil: Comparable#== semantics -- identity is
           equal, an incomparable pair is false (never an error) */
        if (user_cmp_ret_poly(c, ecid)) {
          int ta = hoist_boxed_rooted(c, recv), tb2 = hoist_boxed_rooted(c, argv[0]);
          buf_printf(b, "(%ssp_poly_cmp_eq(_t%d, _t%d))", eq ? "" : "!", ta, tb2);
          return 1;
        }
        char selfptr[64];
        const char *rty2 = nt_type(nt, recv);
        if (rty2 && (sp_streq(rty2, "LocalVariableReadNode") ||
                     sp_streq(rty2, "InstanceVariableReadNode") ||
                     sp_streq(rty2, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t3 = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t3, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfptr, sizeof selfptr, "_t%d", t3);
        }
        buf_puts(b, "(");
        emit_dispatch(c, ecid, "<=>", selfptr, nt_ref(nt, id, "arguments"), -1, b);
        buf_printf(b, " %s 0)", eq ? "==" : "!=");
        return 1;
      }
      /* obj.!= synthesized from obj.== when != is not explicitly defined */
      if (!eq) {
        int eqm2 = comp_method_in_chain(c, ecid, "==", NULL);
        if (eqm2 >= 0) {
          char selfptr2[64];
          const char *rty3 = nt_type(nt, recv);
          if (rty3 && (sp_streq(rty3, "LocalVariableReadNode") ||
                       sp_streq(rty3, "InstanceVariableReadNode") ||
                       sp_streq(rty3, "SelfNode"))) {
            Buf rb = expr_buf(c, recv);
            snprintf(selfptr2, sizeof selfptr2, "%s", rb.p ? rb.p : "");
            free(rb.p);
          }
          else {
            int t4 = ++g_tmp;
            Buf rb = expr_buf(c, recv);
            emit_indent(g_pre, g_indent);
            emit_ctype(c, rt, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", t4, rb.p ? rb.p : "");
            free(rb.p);
            snprintf(selfptr2, sizeof selfptr2, "_t%d", t4);
          }
          buf_puts(b, "(!");
          emit_dispatch(c, ecid, "==", selfptr2, nt_ref(nt, id, "arguments"), -1, b);
          buf_puts(b, ")");
          return 1;
        }
      }
    }
    /* Time == / != via sp_time_cmp */
    if (rt == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, eq ? "==" : "!=");
      return 1;
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
        Buf ob2 = expr_buf(c, obj_n);
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
        return 1;
      }
      /* other primitive types (string, symbol, bool) are strict: false */
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), (");
      emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
      return 1;
    }
    /* object vs nil: identity/pointer comparison (Object#== fallback).
       A non-nullable TY_OBJECT pointer is never NULL, so obj==nil=false
       and obj!=nil=true. A nullable object also works correctly via NULL. */
    if ((ty_is_object(rt) && a0 == TY_NIL) || (rt == TY_NIL && ty_is_object(a0))) {
      int obj_n = ty_is_object(rt) ? recv : argv[0];
      buf_puts(b, "(");
      emit_expr(c, obj_n, b);
      buf_printf(b, " %s NULL)", eq ? "==" : "!=");
      return 1;
    }
    /* object == object with no user-defined == or <=>: Object#== identity.
       Pointer-backed objects compare by address -- faithful to CRuby, where two
       distinct instances are never == and an instance is == only to itself. A
       value-type object has no stable identity (it is copied by value), so
       identity is unrepresentable; rather than silently diverge (structural
       equality would say true where CRuby says false) we refuse and ask for an
       explicit ==. */
    if (recv >= 0 && ty_is_object(rt) && ty_is_object(a0)) {
      if (comp_ty_value_obj(c, rt) || comp_ty_value_obj(c, a0))
        unsupported(c, id, "equality on a value-type object without a user-defined == (define == for comparison)");
      buf_puts(b, "((void *)(");
      emit_expr(c, recv, b);
      buf_printf(b, ") %s (void *)(", eq ? "==" : "!=");
      emit_expr(c, argv[0], b);
      buf_puts(b, "))");
      return 1;
    }
    /* identity types (Thread/Queue/Mutex/ConditionVariable) compare by address,
       like Object#== -- two are equal only if they are the same instance. */
    if (recv >= 0 && rt == a0 &&
        (rt == TY_THREAD || rt == TY_QUEUE || rt == TY_MUTEX || rt == TY_CONDVAR)) {
      buf_puts(b, "((void *)("); emit_expr(c, recv, b);
      buf_printf(b, ") %s (void *)(", eq ? "==" : "!=");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    unsupported(c, id, "equality");
  }
  return 0;
}

/* Emit `mrb_int _s<ta> = ...; mrb_int _l<ta> = ...;` for a splice, from either
   the (start,len) pair or a range computed against the receiver's length. The
   receiver temp `_t<ta>` must already be bound; `tg` names the range temp. */
static void emit_splice_bounds(Compiler *c, int ta, int tg,
                               int start_node, int len_node, int range_node, Buf *b) {
  if (range_node >= 0) {
    buf_printf(b, "sp_Range _t%d = ", tg); emit_expr(c, range_node, b);
    buf_printf(b, "; mrb_int _al%d = _t%d->len;", tg, ta);
    /* frozen precedes any range validation (CRuby's modify-check order),
       and a range beginning before -len is a RangeError, not IndexError */
    buf_printf(b, " if(_t%d->frozen)sp_raise_frozen_array();", ta);
    buf_printf(b, " if(_t%d.first!=INTPTR_MIN&&_t%d.first<-_al%d)"
                  "sp_raise_cls(\"RangeError\",sp_sprintf(\"%%s out of range\",sp_range_str(_t%d)));",
               tg, tg, tg, tg);
    /* INTPTR_MIN/MAX are the beginless/endless sentinels: start 0 / to-end */
    buf_printf(b, " mrb_int _s%d = _t%d.first==INTPTR_MIN?0:(_t%d.first<0?_t%d.first+_al%d:_t%d.first);",
               ta, tg, tg, tg, tg, tg);
    buf_printf(b, " mrb_int _l%d;"
                  " if(_t%d.last==INTPTR_MAX){_l%d=_al%d-_s%d;if(_l%d<0)_l%d=0;}"
                  " else{mrb_int _e%d=_t%d.last<0?_t%d.last+_al%d:_t%d.last;"
                  " _l%d=_e%d-_s%d+(_t%d.excl?0:1);if(_l%d<0)_l%d=0;} ",
               ta,
               tg, ta, tg, ta, ta, ta,
               tg, tg, tg, tg, tg,
               ta, tg, ta, tg, ta, ta);
  } else {
    buf_printf(b, "mrb_int _s%d = ", ta); emit_int_expr(c, start_node, b);
    buf_printf(b, "; mrb_int _l%d = ", ta); emit_int_expr(c, len_node, b);
    buf_puts(b, "; ");
  }
}

/* Scope index of an array-returning `to_ary` in rhs_ty's class chain, else -1.
   CRuby coerces a non-array splice source through to_ary (rb_ary_to_ary); the
   object itself remains the expression value. Value-type classes are excluded
   (their instances are not boxed). */
int splice_to_ary_mi(Compiler *c, TyKind rhs_ty) {
  if (!ty_is_object(rhs_ty)) return -1;
  int cid = ty_object_class(rhs_ty);
  if (c->classes[cid].is_value_type) return -1;
  int mi = comp_method_in_chain(c, cid, "to_ary", NULL);
  if (mi < 0) return -1;
  return ty_is_array((TyKind)c->scopes[mi].ret) ? mi : -1;
}

/* Bind the splice RHS object to `_tq<ta>` (rooted -- the to_ary dispatch and
   the splice pushes can allocate) and write its to_ary call text into out. */
TyKind emit_splice_to_ary_src(Compiler *c, int rhs_node, TyKind rhs_ty,
                              int mi, int ta, Buf *b, Buf *out) {
  int cid = ty_object_class(rhs_ty);
  char selfptr[24];
  snprintf(selfptr, sizeof selfptr, "_tq%d", ta);
  buf_printf(b, "sp_%s *_tq%d = ", c->classes[cid].name, ta);
  emit_expr(c, rhs_node, b);
  buf_printf(b, "; SP_GC_ROOT(_tq%d); ", ta);
  emit_dispatch(c, cid, "to_ary", selfptr, -1, -1, out);
  return (TyKind)c->scopes[mi].ret;
}

/* Emit a splice assignment: `arr[start,len] = rhs` (start_node,len_node given,
   range_node = -1) or `arr[range] = rhs` (range_node given, the others -1). The
   expression value is the RHS, matching Ruby. A typed receiver requires a
   matching element type (mismatch -> clean reject); a poly receiver accepts any
   RHS and fills nil gaps. An object RHS with to_ary splices the coerced array
   while the object stays the expression value (CRuby rb_ary_to_ary). */
void emit_array_splice(Compiler *c, int id, int recv, TyKind rt,
                       int start_node, int len_node, int range_node,
                       int rhs_node, Buf *b) {
  TyKind rhs_ty = comp_ntype(c, rhs_node);
  int rhs_is_arr = ty_is_array(rhs_ty);
  /* An empty array literal `[]` infers TY_UNKNOWN; treat it as a zero-element
     source (the splice degenerates to a pure delete of the [start,len) span). */
  int rhs_empty = 0;
  {
    const char *rnt = nt_type(c->nt, rhs_node);
    if (rnt && sp_streq(rnt, "ArrayNode")) {
      int en = 0; nt_arr(c->nt, rhs_node, "elements", &en);
      rhs_empty = (en == 0);
    }
  }
  int ta = ++g_tmp, ts = ++g_tmp, tg = ++g_tmp;  /* recv, src, range temps */
  int tam = splice_to_ary_mi(c, rhs_ty);

  if (rt == TY_POLY_ARRAY) {
    buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b); buf_puts(b, "; ");
    if (tam >= 0) {
      /* object RHS with to_ary: splice the coercion, yield the object */
      Buf call; memset(&call, 0, sizeof call);
      TyKind cty = emit_splice_to_ary_src(c, rhs_node, rhs_ty, tam, ta, b, &call);
      buf_printf(b, "sp_RbVal _t%d = ", ts);
      emit_boxed_text(c, cty, call.p ? call.p : "", b);
      buf_puts(b, "; ");
      free(call.p);
      emit_splice_bounds(c, ta, tg, start_node, len_node, range_node, b);
      buf_printf(b, "sp_PolyArray_splice(_t%d, _s%d, _l%d, _t%d); sp_box_obj(_tq%d, %d); })",
                 ta, ta, ta, ts, ta, ty_object_class(rhs_ty));
      return;
    }
    buf_printf(b, "sp_RbVal _t%d = ", ts);
    if (rhs_empty) buf_puts(b, "sp_box_poly_array(sp_PolyArray_new())");
    else emit_boxed(c, rhs_node, b);
    buf_puts(b, "; ");
    emit_splice_bounds(c, ta, tg, start_node, len_node, range_node, b);
    buf_printf(b, "sp_PolyArray_splice(_t%d, _s%d, _l%d, _t%d); _t%d; })", ta, ta, ta, ts, ts);
    return;
  }

  const char *k = array_kind(rt);   /* "Int" / "Str" / "Float" */
  TyKind elem = ty_array_elem(rt);
  if (!k) { unsupported(c, id, "array splice on this receiver type"); return; }
  /* Runtime `src` pointer type per element kind (Str stores `const char *`). */
  const char *srcty = elem == TY_INT   ? "const mrb_int *"
                    : elem == TY_FLOAT ? "const mrb_float *"
                    :                    "const char *const *";

  /* Typed receiver: bind recv, derive a (src, srcn) pair from the RHS, then
     issue one splice call. `valtmp` (normally the RHS temp `_t<ts>`) is the
     yielded value (Ruby `[]=` returns the assigned value as written). */
  char valtmp[24];
  snprintf(valtmp, sizeof valtmp, "_t%d", ts);
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b); buf_puts(b, "; ");
  buf_printf(b, "%s_src%d; mrb_int _srcn%d; ", srcty, ta, ta);

  if (rhs_empty) {
    /* pure delete: the source is a fresh empty array (also the yielded value) */
    buf_printf(b, "sp_%sArray *_t%d = sp_%sArray_new(); _src%d = NULL; _srcn%d = 0; ",
               k, ts, k, ta, ta);
  } else if (rhs_is_arr) {
    /* source array must share the receiver's element type; a poly/mismatched
       source would mix types into a flat typed array (reject loudly). */
    TyKind rhs_elem = ty_array_elem(rhs_ty);
    if (rhs_ty == TY_POLY_ARRAY || rhs_elem != elem) {
      unsupported(c, id, "array splice with a mismatched-element-type source");
      return;
    }
    /* rooted: the splice's pushes can allocate, and a Str source's elements
       are reachable only through this array until they are pushed */
    buf_printf(b, "sp_%sArray *_t%d = ", k, ts); emit_expr(c, rhs_node, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); ", ts);
    /* IntArray carries a `start` offset; Str/Float data begins at index 0 */
    buf_printf(b, "_src%d = ", ta);
    if (elem == TY_INT) buf_printf(b, "_t%d->data + _t%d->start", ts, ts);
    else buf_printf(b, "_t%d->data", ts);
    buf_printf(b, "; _srcn%d = _t%d->len; ", ta, ts);
  } else if (tam >= 0 && (TyKind)c->scopes[tam].ret != TY_POLY_ARRAY &&
             ty_array_elem((TyKind)c->scopes[tam].ret) == elem) {
    /* object RHS whose to_ary returns this element kind: splice the coerced
       array; the OBJECT is the yielded value (CRuby rb_ary_to_ary) */
    Buf call; memset(&call, 0, sizeof call);
    emit_splice_to_ary_src(c, rhs_node, rhs_ty, tam, ta, b, &call);
    buf_printf(b, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); ",
               k, ts, call.p ? call.p : "", ts);
    free(call.p);
    buf_printf(b, "_src%d = ", ta);
    if (elem == TY_INT) buf_printf(b, "_t%d->data + _t%d->start", ts, ts);
    else buf_printf(b, "_t%d->data", ts);
    buf_printf(b, "; _srcn%d = _t%d->len; ", ta, ts);
    snprintf(valtmp, sizeof valtmp, "_tq%d", ta);
  } else if (rhs_ty == TY_POLY) {
    /* A poly RHS can be a same-kind array boxed as poly (e.g. `poly_arr.first`,
       statically TY_POLY but an IntArray at runtime) or a genuine scalar. Decide
       at runtime: use the array's elements when the class id matches the
       receiver's element kind, else the unboxed scalar. */
    const char *bcon = elem == TY_INT   ? "SP_BUILTIN_INT_ARRAY"
                     : elem == TY_STRING ? "SP_BUILTIN_STR_ARRAY"
                     :                     "SP_BUILTIN_FLT_ARRAY";
    const char *conv = elem == TY_INT   ? "sp_poly_to_i"
                     : elem == TY_STRING ? "sp_poly_to_s"
                     :                     "sp_poly_to_f";
    buf_printf(b, "sp_RbVal _t%d = ", ts); emit_boxed(c, rhs_node, b);
    /* root the boxed RHS: when it holds a same-kind array, _src aliases into
       _sa->data, which the splice's pushes can collect out from under us */
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ts);
    emit_ctype(c, elem, b); buf_printf(b, " _v%d; ", ta);
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %s) { sp_%sArray *_sa%d = (sp_%sArray *)_t%d.v.p; _src%d = ",
               ts, ts, bcon, k, ta, k, ts, ta);
    if (elem == TY_INT) buf_printf(b, "_sa%d->data + _sa%d->start", ta, ta);
    else buf_printf(b, "_sa%d->data", ta);
    buf_printf(b, "; _srcn%d = _sa%d->len; } else { _v%d = %s(_t%d); _src%d = &_v%d; _srcn%d = 1; } ",
               ta, ta, ta, conv, ts, ta, ta, ta);
  } else {
    /* scalar RHS: replace the slice with a single element */
    int scalar_ok = (elem == TY_INT    && rhs_ty == TY_INT) ||
                    (elem == TY_STRING && rhs_ty == TY_STRING) ||
                    (elem == TY_FLOAT  && rhs_ty == TY_FLOAT);
    if (!scalar_ok) { unsupported(c, id, "array splice with a mismatched scalar value"); return; }
    emit_ctype(c, elem, b); buf_printf(b, " _t%d = ", ts); emit_expr(c, rhs_node, b); buf_puts(b, "; ");
    buf_printf(b, "_src%d = &_t%d; _srcn%d = 1; ", ta, ts, ta);
  }

  emit_splice_bounds(c, ta, tg, start_node, len_node, range_node, b);
  buf_printf(b, "sp_%sArray_splice(_t%d, _s%d, _l%d, _src%d, _srcn%d); %s; })",
             k, ta, ta, ta, ta, ta, valtmp);
}

static int emit_array_arith_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);
  /* Array#* (repeat): arr * n  ->  new array with elements repeated n times.
     The count is emitted via emit_int_expr, which unboxes a promote-widened
     poly count, so accept TY_POLY as well as TY_INT -- otherwise `arr * n`
     with a poly `n` falls through to sp_poly_mul (arithmetic) and yields 0. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY)) {
    int ta = ++g_tmp, tn = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp;
    if (rt == TY_POLY_ARRAY) {
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
      buf_printf(b, "; mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
    return 1;
  }

  if (recv >= 0 && argc == 1 && !ty_is_object(rt) && !ty_is_array(rt) &&
      (int_arith_fn(name) ||
       /* bigint shifts aren't "int arith" ops but lower through the same
          TY_BIGINT branch below (sp_bigint_shl / sp_bigint_shr). */
       (res == TY_BIGINT && (sp_streq(name, "<<") || sp_streq(name, ">>"))))) {
    if (rt == TY_STRING && sp_streq(name, "+")) {
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
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_str_plus(_t%d, _t%d); })", tb, ta, tb);
      }
      else {
        buf_puts(b, "sp_str_plus(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (arg_poly) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      return 1;
    }
    if (rt == TY_STRING && sp_streq(name, "*")) {
      buf_puts(b, "sp_str_repeat(");
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (res == TY_BIGINT) {
      /* **, <<, >> take an int64 second operand (exponent / shift), not a bigint;
         bigint_arith_fn doesn't map them, so emit them directly. */
      if (sp_streq(name, "**") || sp_streq(name, "<<") || sp_streq(name, ">>")) {
        const char *sfn = sp_streq(name, "**") ? "sp_bigint_pow"
                        : sp_streq(name, "<<") ? "sp_bigint_shl"
                        : "sp_bigint_shr";
        buf_printf(b, "%s(", sfn);
        emit_bigint_operand(c, recv, b);
        buf_puts(b, ", ");
        if (comp_ntype(c, argv[0]) == TY_BIGINT) { buf_puts(b, "sp_bigint_to_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "(int64_t)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
        return 1;
      }
      const char *bfn = bigint_arith_fn(name);
      if (bfn) {
        buf_printf(b, "%s(", bfn);
        emit_bigint_operand(c, recv, b);
        buf_puts(b, ", ");
        emit_bigint_operand(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* Re-derive result type when cache may be stale due to block-param widening */
    TyKind eff_res = res;
    if (eff_res != TY_INT && eff_res != TY_FLOAT && eff_res != TY_BIGINT) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) eff_res = TY_FLOAT;
      else if (rt == TY_INT && (a0 == TY_INT || a0 == TY_UNKNOWN)) eff_res = TY_INT;
    }
    if (eff_res == TY_INT) {
      int isdivmod = sp_streq(name, "/") || sp_streq(name, "%");
      buf_printf(b, "%s(", int_arith_fn(name));
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (isdivmod) emit_int_divisor(c, argv[0], b);
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (eff_res == TY_FLOAT && sp_streq(name, "**") && rt != TY_TIME) {
      TyKind at0 = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      buf_puts(b, "pow(");
      if (rt == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (at0 == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (eff_res == TY_FLOAT && sp_streq(name, "%") && rt != TY_TIME && argc == 1) {
      TyKind at0 = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_fmod(");
      if (rt == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (at0 == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (eff_res == TY_FLOAT && rt != TY_TIME && !sp_streq(name, "%") && !sp_streq(name, "**")) {
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    /* Time + int/float, Time - int/float, Time - Time */
    if (rt == TY_TIME && (sp_streq(name, "+") || sp_streq(name, "-"))) {
      TyKind at = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int tt = ++g_tmp, tu = ++g_tmp;
      if (sp_streq(name, "-") && at == TY_TIME) {
        /* Time - Time -> Float */
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_sub_t(_t%d, _t%d); })", tt, tu);
      }
      else if (at == TY_FLOAT) {
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; double _t%d = ", tu); emit_expr(c, argv[0], b);
        if (sp_streq(name, "+"))
          buf_printf(b, "; sp_time_add_f(_t%d, _t%d); })", tt, tu);
        else
          buf_printf(b, "; sp_time_add_f(_t%d, -_t%d); })", tt, tu);
      }
      else {
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tu); emit_int_expr(c, argv[0], b);
        if (sp_streq(name, "+"))
          buf_printf(b, "; sp_time_add_i(_t%d, _t%d); })", tt, tu);
        else
          buf_printf(b, "; sp_time_sub_i(_t%d, _t%d); })", tt, tu);
      }
      return 1;
    }
    unsupported(c, id, "arithmetic");
  }
  return 0;
}

/* Wrap a break-carrying block call in a serial-addressed setjmp scope: the
   scope's serial (from sp_brk_push) is what a top-level `break` in the
   inlined block -- or a non-lambda proc created under it -- addresses via
   sp_brk_throw; the call expression then yields the break value. The result
   type widened to poly at inference; the call's NORMAL (no-break) type is
   recovered for the inner emission and boxed. b == NULL emits the call in
   statement position (the value temp is simply left unread). */
void emit_brk_wrapped_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int wrecv = nt_ref(nt, id, "receiver");
  const char *wname = nt_str(nt, id, "name");
  /* A builtin self-returning iterator (each / each_with_index / ...) has no
     value-producing emitter: run it as a statement and use the receiver as
     the no-break result. A user method resolving through the inliner takes
     the normal value path (its return value is the result, NOT the receiver
     -- name-matching alone would be wrong for a user `each`). */
  int self_ret = wrecv >= 0 && call_user_yield_mi(c, id) < 0 &&
                 brk_iter_returns_self(wname);
  int sv_ig = g_infer_ignore_brk; g_infer_ignore_brk = 1;
  TyKind normal_ty = self_ret ? comp_ntype(c, wrecv) : infer_uncached(c, id);
  g_infer_ignore_brk = sv_ig;
  if (normal_ty == TY_STRBUF) normal_ty = TY_STRING;
  int tR = ++g_tmp, tG = ++g_tmp, tS = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tR, tR);
  /* frame snapshots: the goto delivery (and the longjmp landing) restore the
     exception/catch/break depths to the wrapper's entry state */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "int _brkexc%d = sp_exc_top, _brkcat%d = sp_catch_top, _brkslot%d = 0;"
                    " (void)_brkexc%d; (void)_brkcat%d; (void)_brkslot%d;\n",
             tS, tS, tS, tS, tS, tS);
  /* An impure self-returning receiver is evaluated ONCE into a rooted temp,
     substituted for the receiver node below (g_argov), and reused as the
     no-break result -- so `make_arr.each { break }` builds the array once. */
  int spill = -1, spilled_argov = 0;
  if (self_ret && !brk_recv_is_pure(c, wrecv)) {
    /* The impure receiver feeds both the call and the no-break result, so it
       MUST be spilled to a temp -- evaluating it twice would double its side
       effects. If the override table is full we cannot substitute it; reject
       loudly rather than emit a double-evaluating call. */
    if (g_n_argov >= MAX_ARG_OVERRIDE)
      unsupported(c, id, "break-wrapped iterator with impure receiver (override table full)");
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_expr(c, wrecv, &rb);
    spill = ++g_tmp;
    emit_indent(g_pre, g_indent);
    emit_ctype(c, normal_ty, g_pre);
    buf_printf(g_pre, " _t%d = %s;", spill, rb.p ? rb.p : "0");
    free(rb.p);
    if (needs_root(normal_ty)) buf_printf(g_pre, " SP_GC_ROOT(_t%d);", spill);
    buf_puts(g_pre, "\n");
    g_argov_node[g_n_argov] = wrecv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", spill);
    g_n_argov++;
    spilled_argov = 1;
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "int _t%d = sp_gc_nroots; (void)_t%d;\n", tG, tG);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _brkser%d = sp_brk_push(); (void)_brkser%d;\n", tS, tS);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "_brkslot%d = sp_brk_top;\n", tS);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "if (setjmp(sp_brk_stack[sp_brk_top - 1]) == 0) {\n");
  g_indent++;
  TyKind sv_cache = c->ntype[id]; c->ntype[id] = normal_ty;
  char servar[24]; snprintf(servar, sizeof servar, "_brkser%d", tS);
  const char *sv_ser = g_brk_ser_var; g_brk_ser_var = servar;
  int sv_ebase = g_brk_ensure_base; g_brk_ensure_base = g_ensure_depth;
  int sv_bexc = g_brk_exc_base; g_brk_exc_base = g_exc_frame_depth;
  int sv_skip = g_brk_skip_id; g_brk_skip_id = id;
  Buf inner; memset(&inner, 0, sizeof inner);
  /* A no-value normal type (a yield method ending in puts/nil) runs as a
     statement with nil as the no-break result. */
  int stmt_form = self_ret || !is_scalar_ret(normal_ty);
  if (stmt_form) {
    emit_stmt(c, id, g_pre, g_indent);
    if (self_ret) {
      if (spill >= 0) buf_printf(&inner, "_t%d", spill);
      else emit_expr(c, wrecv, &inner);
    }
  } else {
    emit_call(c, id, &inner);   /* emits the loop into g_pre, result expr into inner */
  }
  g_brk_ser_var = sv_ser; g_brk_ensure_base = sv_ebase; g_brk_exc_base = sv_bexc; g_brk_skip_id = sv_skip;
  c->ntype[id] = sv_cache;
  if (spilled_argov) g_n_argov--;
  Buf boxed; memset(&boxed, 0, sizeof boxed);
  if (inner.p && inner.p[0]) emit_boxed_text(c, normal_ty, inner.p, &boxed);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "_t%d = %s;\n", tR, boxed.p && boxed.p[0] ? boxed.p : "sp_box_nil()");
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_brk_top--;\n");
  free(inner.p); free(boxed.p);
  g_indent--;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "} else {\n");
  /* the goto delivery lands here too; restore every depth to the entry
     snapshot (correct for both paths -- after an ensure-running longjmp the
     handlers have already popped down to these) */
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "_brklbl%d: __attribute__((unused));\n", tS);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_gc_nroots = _t%d;\n", tG);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_exc_top = _brkexc%d; sp_catch_top = _brkcat%d; sp_brk_top = _brkslot%d;\n",
             tS, tS, tS);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "_t%d = sp_brk_val[sp_brk_top - 1];\n", tR);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_brk_top--;\n");
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (b) buf_printf(b, "_t%d", tR);
  else { emit_indent(g_pre, g_indent); buf_printf(g_pre, "(void)_t%d;\n", tR); }
}

void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (emit_dynamic_send(c, id, b)) return;   /* recv.send(runtime_name, args) static dispatch */
  /* `require` / `require_relative` is a compile-time directive: top-level ones
     are textually spliced away before codegen, and native libs are provided by
     the runtime. One that still reaches codegen -- indented inside an `if`,
     module, or method body -- is a runtime no-op (it would otherwise be an
     unsupported CallNode). Returns nil in value position via emit_boxed. */
  {
    int rcv = nt_ref(nt, id, "receiver");
    const char *cn = nt_str(nt, id, "name");
    if (rcv < 0 && cn && (sp_streq(cn, "require") || sp_streq(cn, "require_relative"))) {
      buf_puts(b, "0");
      return;
    }
  }
  /* Valued `break` from a block: wrap the call in a serial-addressed setjmp
     scope so a top-level `break <v>` in the block sp_brk_throws back here and
     the call yields <v> (see emit_brk_wrapped_call). */
  if (id != g_brk_skip_id && call_breaks(c, id)) {
    emit_brk_wrapped_call(c, id, b);
    return;
  }
  /* Inside an Enumerator.new { |y| ... } generator, `y << v` and `y.yield(v)` on
     the yielder lower to a Fiber.yield (the generator runs on a fiber). */
  if (g_yielder_name) {
    int rcv = nt_ref(nt, id, "receiver");
    const char *cn = nt_str(nt, id, "name");
    if (rcv >= 0 && cn && (sp_streq(cn, "<<") || sp_streq(cn, "yield")) &&
        nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "LocalVariableReadNode") &&
        nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), g_yielder_name)) {
      int ar = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
      buf_puts(b, "sp_Fiber_yield(");
      if (ac == 1) emit_boxed(c, av[0], b);
      else if (ac > 1) {
        /* y.yield(a, b, ...) yields an array of the values */
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", t, t);
        for (int k = 0; k < ac; k++) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t); emit_boxed(c, av[k], g_pre); buf_puts(g_pre, ");\n");
        }
        buf_printf(b, "sp_box_poly_array(_t%d)", t);
      }
      else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ")");
      return;
    }
  }
  if (emit_lazy_pipeline_expr(c, id, b)) return;
  if (emit_partition_expr(c, id, b)) return;
  if (emit_with_index_expr(c, id, b)) return;
  if (emit_each_with_index_chain(c, id, b)) return;
  if (emit_each_with_index_terminal(c, id, b)) return;
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
  int argc;
  const int *argv = call_args(nt, id, &argc);
  if (!name) unsupported(c, id, "call (no name)");

  /* `@nested[i]` inferred as an int array (poly array of int arrays): unbox
     the poly element to sp_IntArray* so the surrounding code stays typed. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 &&
      comp_ntype(c, recv) == TY_POLY_ARRAY && comp_ntype(c, id) == TY_INT_ARRAY) {
    buf_puts(b, "((sp_IntArray *)((sp_PolyArray_get(");
    emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    buf_puts(b, ")).v.p))");
    return;
  }

  if (emit_complex_rational_call(c, id, b)) return;

  /* loop { break val } as expression: emit pre-statement for-loop, result via break var */
  /* Kernel#caller / caller(start) / caller(start, len) -> the current stack
     (method-granularity, via sp_caller_now). Bare `caller` is `caller(1)`. */
  if (recv < 0 && sp_streq(name, "caller") && argc <= 2) {
    buf_puts(b, "sp_caller(");
    if (argc >= 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
    if (argc == 2) { buf_puts(b, ", 1, "); emit_int_expr(c, argv[1], b); }
    else buf_puts(b, ", 0, 0");
    buf_puts(b, ")");
    return;
  }
  /* eval(string) / Kernel.eval(string): a hard AOT boundary (see helper). */
  if (diagnose_eval_call(c, id)) return;
  /* caller_locations: no runtime frame stack in AOT builds (as with `caller`),
     so this is an empty array of locations -- an Array, never nil. The (start,
     length) arguments are still evaluated for their side effects, as CRuby
     evaluates them before the call; the `(void)` casts keep a literal arg from
     tripping -Wunused-value. */
  if (recv < 0 && sp_streq(name, "caller_locations") && argc <= 2) {
    buf_puts(b, "(");
    for (int ai = 0; ai < argc; ai++) { buf_puts(b, "(void)("); emit_expr(c, argv[ai], b); buf_puts(b, "), "); }
    buf_puts(b, "sp_PolyArray_new())");
    return;
  }
  if (recv < 0 && sp_streq(name, "loop") && argc == 0) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = infer_type(c, id);
      if (bt != TY_UNKNOWN && bt != TY_NIL) {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", t,
                   bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
        /* Kernel#loop rescues StopIteration to terminate; wrap in a setjmp. */
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_top++;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
        emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "for (;;) {\n");
        const char *sv_lb = g_loop_break_var;
        int sv_lexc = g_loop_exc_base; g_loop_exc_base = g_exc_frame_depth;
        int sv_iep = g_ie_res_poly;
        const char *sv_bj = g_brk_ser_var; g_brk_ser_var = NULL;  /* break here targets this loop */
        g_ie_res_poly = (bt == TY_POLY);   /* box a scalar `break <v>` into the poly slot */
        char lb_buf[32]; snprintf(lb_buf, sizeof lb_buf, "_t%d", t);
        g_loop_break_var = lb_buf;
        int lbody = nt_ref(nt, blk, "body");
        emit_stmts(c, lbody, g_pre, g_indent + 2);
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
        buf_puts(g_pre, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", t);
        return;
      }
    }
  }

  /* catch(:tag) { ... [throw :tag, val] ... } as expression: a setjmp scope
     whose value is the block's last expression, or the thrown value. */
  if (recv < 0 && sp_streq(name, "catch") && argc == 1) {
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
      g_brk_ser_var = sv_cser;
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "else {\n");
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_catch_top--;\n");
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_gc_nroots = sp_catch_rootmark[sp_catch_top];\n");
      emit_indent(g_pre, g_indent + 1);
      if (ptr) {
        buf_printf(g_pre, "_t%d = (", t); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, ")(uintptr_t)sp_catch_val[sp_catch_top];\n");
      }
      else if (bt == TY_POLY) {
        /* A poly-typed catch (e.g. its block's value is poly): the mrb_int
           value channel can't carry a tagged value, so box the thrown int.
           Common shape is a poly block return with no matching throw, where
           this arm is dead; a thrown non-int to a poly catch is a separate
           limitation of the int-only throw channel. */
        buf_printf(g_pre, "_t%d = sp_box_int(sp_catch_val[sp_catch_top]);\n", t);
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
  if (recv < 0 && sp_streq(name, "throw")) {
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
  if (recv < 0 && sp_streq(name, "system") && argc >= 1) {
    int ts = ++g_tmp;
    buf_printf(b, "({ const char *_sys_%d[] = { ", ts);
    for (int k = 0; k < argc; k++) { if (k > 0) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
    buf_printf(b, ", NULL }; (mrb_bool)sp_system_args(%d, _sys_%d); })", argc, ts);
    return;
  }
  /* trap(...) / Signal.trap(...) expr: return "DEFAULT" */
  {
    int is_trap = (recv < 0 && sp_streq(name, "trap"));
    if (!is_trap && recv >= 0 && sp_streq(name, "trap") && argc >= 1) {
      const char *rty2 = nt_type(nt, recv);
      if (rty2 && (sp_streq(rty2, "ConstantReadNode") || sp_streq(rty2, "ConstantPathNode"))) {
        const char *rn = nt_str(nt, recv, "name");
        if (rn && sp_streq(rn, "Signal")) is_trap = 1;
      }
    }
    if (is_trap && argc >= 1) { emit_str_literal(b, "DEFAULT"); return; }
  }

  /* Fiber[:k] / Fiber.current[:k] -> sp_Fiber_storage_get */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    int is_fiber_recv = 0;
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "Fiber")) is_fiber_recv = 1;
    }
    else if (rty2 && sp_streq(rty2, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && sp_streq(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber"))
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
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        buf_puts(b, "sp_str_dup_external(getenv("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return;
      }
    }
  }
  /* ENV[key] = value -> setenv (value nil unsets, like CRuby) */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        int tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tk); emit_expr(c, argv[0], b);
        buf_printf(b, "; const char *_t%d = ", tv); emit_str_expr(c, argv[1], b);
        buf_printf(b, "; if (_t%d) setenv(_t%d, _t%d, 1); else unsetenv(_t%d); _t%d; })",
                   tv, tk, tv, tk, tv);
        return;
      }
    }
  }
  /* ENV.fetch(key, default) -> getenv with fallback */
  if (recv >= 0 && sp_streq(name, "fetch") && argc >= 1) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
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
    if (_pr_recv < 0 && _pr_nm && (sp_streq(_pr_nm, "proc") || sp_streq(_pr_nm, "lambda")))
      is_literal = 1;
    if (!is_literal && _pr_recv >= 0 && _pr_nm && sp_streq(_pr_nm, "new")) {
      const char *_rty = nt_type(nt, _pr_recv);
      const char *_rnm = (_rty && (sp_streq(_rty, "ConstantReadNode") || sp_streq(_rty, "ConstantPathNode")))
                         ? nt_str(nt, _pr_recv, "name") : NULL;
      if (_rnm && sp_streq(_rnm, "Proc")) is_literal = 1;
    }
    if (is_literal) { emit_proc_literal(c, id, b); return; }
  }

  /* Safe navigation &. : nil receiver -> return nil/0; non-nil -> emit conditional */
  {
    const char *safe_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && safe_op && sp_streq(safe_op, "&.")) {
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
        if (sp_streq(name, "upcase")) {
          buf_printf(b, "sp_box_str(sp_str_upcase(_sn_%d.v.s))", tsn);
        }
        else if (sp_streq(name, "downcase")) {
          buf_printf(b, "sp_box_str(sp_str_downcase(_sn_%d.v.s))", tsn);
        }
        else if (sp_streq(name, "length") || sp_streq(name, "size")) {
          buf_printf(b, "sp_box_int(sp_poly_length(_sn_%d))", tsn);
        }
        else if (sp_streq(name, "inspect")) {
          buf_printf(b, "sp_box_str(sp_poly_inspect(_sn_%d))", tsn);
        }
        else if (sp_streq(name, "to_s")) {
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
      (sp_streq(name, "times") || sp_streq(name, "upto") ||
       sp_streq(name, "downto") || sp_streq(name, "step"))) {
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    return;
  }
  /* n.times / lo.upto(hi) / hi.downto(lo) without block: produce sp_Range for chaining */
  if (recv >= 0 && nt_ref(nt, id, "block") < 0 && comp_ntype(c, recv) == TY_INT &&
      comp_ntype(c, id) == TY_RANGE) {
    if (sp_streq(name, "times")) {
      buf_puts(b, "(sp_Range){ .first = 0, .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 1 }");
      return;
    }
    if (sp_streq(name, "upto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, recv, b);
      buf_puts(b, ", .last = "); emit_expr(c, argv[0], b); buf_puts(b, ", .excl = 0 }");
      return;
    }
    if (sp_streq(name, "downto") && argc == 1) {
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, argv[0], b);
      buf_puts(b, ", .last = "); emit_expr(c, recv, b); buf_puts(b, ", .excl = 0 }");
      return;
    }
  }

  /* poly_val.call — the poly value is a proc; unbox then call.
     Only applies when no user-defined class has a `call` method (otherwise
     use the existing poly dispatch switch which handles user-defined call). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      (sp_streq(name, "call") || sp_streq(name, "()"))) {
    int has_user_call = 0;
    for (int _k = 0; _k < c->nclasses && !has_user_call; _k++)
      if (comp_method_in_class(c, _k, name) >= 0) has_user_call = 1;
    if (!has_user_call) {
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = ", t); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
      /* the poly callable may be a Proc or a bound Method (different ABIs).
         Under promote the bound method is poly-signatured, so call it through
         the poly ABI and unbox the result back to the mrb_int the Proc arm
         yields, keeping the ternary's two branches a single type. */
      int mabi_poly = g_promote_mode;
      const char *aty = mabi_poly ? "sp_RbVal" : "mrb_int";
      buf_printf(b, "(_t%d.cls_id == SP_BUILTIN_METHOD ? %s((%s (*)(void *", t, mabi_poly ? "sp_poly_to_i(" : "", aty);
      for (int k = 0; k < argc; k++) buf_printf(b, ", %s", aty);
      buf_printf(b, "))(uintptr_t)((sp_BoundMethod *)_t%d.v.p)->fn)((void *)((sp_BoundMethod *)_t%d.v.p)->self", t, t);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (mabi_poly) emit_boxed(c, argv[k], b);
        else if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[k], b);
      }
      buf_printf(b, ")%s : sp_proc_call((sp_Proc *)_t%d.v.p, %d, (mrb_int[16]){", mabi_poly ? ")" : "", t, argc);
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
  if (sp_streq(name, "method") && method_sym_arg(c, id) != NULL) {
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
        if (sp_streq(sym, "[]")) bop = "get";
        else if (sp_streq(sym, "[]=")) bop = "set";
        else if (sp_streq(sym, "push")) bop = "push";
      }
      if (bop) {
        /* memoized per (kind, op): emit the adapter once */
        static char bam_done[2][3];
        int ki = (brt == TY_INT_ARRAY) ? 0 : 1;
        int oi = bop[0] == 'g' ? 0 : bop[0] == 's' ? 1 : 2;
        if (!bam_done[ki][oi]) {
          bam_done[ki][oi] = 1;
          const char *cast = (ki == 0) ? "" : "(mrb_int)(uintptr_t)";
          const char *uncast = (ki == 0) ? "" : "(const char *)(uintptr_t)";
          if (g_promote_mode) {
            /* promote: bound methods are invoked through the poly ABI, so the
               adapter takes/returns sp_RbVal (boxing the int/string element). */
            const char *boxret = (ki == 0) ? "sp_box_int_or_nil" : "sp_box_str";
            const char *unbox  = (ki == 0) ? "sp_poly_to_i" : "sp_poly_to_s";
            const char *boxarr = (ki == 0) ? "sp_box_int_array" : "sp_box_str_array";
            if (oi == 0) {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_get(void *a, sp_RbVal i);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_get(void *a, sp_RbVal i) {\n"
                                   "  return %s(sp_%sArray_get((sp_%sArray *)a, sp_poly_to_i(i)));\n}\n", bk, boxret, bk, bk);
            }
            else if (oi == 1) {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_set(void *a, sp_RbVal i, sp_RbVal v);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_set(void *a, sp_RbVal i, sp_RbVal v) {\n"
                                   "  sp_%sArray_set((sp_%sArray *)a, sp_poly_to_i(i), %s(v));\n  return v;\n}\n", bk, bk, bk, unbox);
            }
            else {
              buf_printf(&g_proc_protos, "static sp_RbVal _bam_%sArray_push(void *a, sp_RbVal v);\n", bk);
              buf_printf(&g_procs, "static sp_RbVal _bam_%sArray_push(void *a, sp_RbVal v) {\n"
                                   "  sp_%sArray_push((sp_%sArray *)a, %s(v));\n  return %s(a);\n}\n", bk, bk, bk, unbox, boxarr);
            }
          }
          else if (oi == 0) {
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
  /* <method>.name -> the stored method name, interned to a Symbol (CRuby
     Method#name returns a Symbol, not a String). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "name")) {
    buf_puts(b, "sp_sym_intern((const char *)("); emit_expr(c, recv, b); buf_puts(b, ")->name)");
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
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "arity")) {
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
        return;
      }
    }
  }
  /* <method>.call(args) / [] -> invoke the bound function. A top-level
     method ref calls its function directly; an object-bound Method casts
     fn through the (void *self, mrb_int...) ABI, evaluating recv once. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]"))) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    if (target >= 0 && target_recvless) {
      /* top-level / self method: direct call sp_<name>(args). Coerce each arg to
         the target's parameter type (emit_arg_or_default boxes an int arg into a
         poly param widened under promote, etc.). */
      emit_method_cname(c, &c->scopes[target], b);
      buf_puts(b, "(");
      for (int k = 0; k < argc; k++) {
        if (k) buf_puts(b, ", ");
        if (k < c->scopes[target].nparams) emit_arg_or_default(c, &c->scopes[target], k, argv[k], b);
        else emit_expr(c, argv[k], b);
      }
      buf_puts(b, ")");
      return;
    }
    /* object-bound: cast fn through its real signature and call it once.
       When the target method is statically known (`recv.method(:m)`), use its
       actual return and parameter C types so a promote-widened poly method --
       \`sp_RbVal (*)(void*, sp_RbVal)\` -- is not invoked through the legacy
       mrb_int ABI (which truncates the boxed args and return to garbage).
       Falls back to the raw mrb_int ABI when the target is unresolved. */
    int tr = ++g_tmp;
    Scope *tm = target >= 0 ? &c->scopes[target] : NULL;
    /* When the target is unresolved under promote, fall back to the poly ABI
       (sp_RbVal self/args/return) rather than the legacy mrb_int ABI: every
       method is poly-signatured in promote, so a `(void*, mrb_int)->mrb_int`
       cast would truncate the boxed args and return to garbage. */
    int poly_abi = !tm && g_promote_mode;
    TyKind tret = tm ? (TyKind)tm->ret : (poly_abi ? TY_POLY : TY_INT);
    if (!is_scalar_ret(tret)) tret = TY_INT;  /* aggregate ret: raw carrier */
    buf_printf(b, "({ sp_BoundMethod *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
    buf_puts(b, "(("); emit_ctype(c, tret, b); buf_puts(b, " (*)(void *");
    for (int k = 0; k < argc; k++) {
      buf_puts(b, ", ");
      if (tm && k < tm->nparams) {
        LocalVar *pp = scope_local(tm, tm->pnames[k]);
        emit_ctype(c, pp ? pp->type : TY_INT, b);
      }
      else if (poly_abi) buf_puts(b, "sp_RbVal");
      else buf_puts(b, "mrb_int");
    }
    buf_printf(b, "))(uintptr_t)_t%d->fn)((void *)_t%d->self", tr, tr);
    for (int k = 0; k < argc; k++) {
      buf_puts(b, ", ");
      if (tm && k < tm->nparams) emit_arg_or_default(c, tm, k, argv[k], b);
      else if (poly_abi) emit_boxed(c, argv[k], b);
      else if (proc_slot_is_ptr(comp_ntype(c, argv[k]))) { buf_puts(b, "(mrb_int)(uintptr_t)("); emit_expr(c, argv[k], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[k], b);
    }
    buf_printf(b, "); })");
    return;
  }

  /* <proc>.call(args) / .() / [] -> sp_proc_call with the mrb_int[] ABI.
     (A `&block`-param `.call` is handled earlier by the inline path, whose
     receiver name matches g_block_param_name; this is the escaped-value case.) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]"))) {
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
    buf_puts(b, ", ");
    emit_proc_call_args(c, argc, argv, b, 1);
    if (unbox_ptr) buf_puts(b, ")");
    if (unbox_poly) buf_puts(b, ", _sp_proc_poly_ret)");
    if (unbox_float) buf_puts(b, ", sp_poly_to_f(_sp_proc_poly_ret))");
    return;
  }

  /* Proc introspection: arity / lambda? read the sp_Proc metadata directly. */
  /* proc << proc / proc >> proc -> composed Proc. f<<g = f(g(x)) (outer f,
     inner g); f>>g = g(f(x)) (outer g, inner f). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 1 &&
      (sp_streq(name, "<<") || sp_streq(name, ">>")) && comp_ntype(c, argv[0]) == TY_PROC) {
    int fwd = sp_streq(name, ">>");
    buf_puts(b, "sp_proc_compose(");
    if (fwd) emit_expr(c, argv[0], b); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    if (fwd) emit_expr(c, recv, b); else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "arity")) {
    buf_puts(b, "sp_proc_arity("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "lambda?")) {
    buf_puts(b, "sp_proc_lambda_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "parameters")) {
    buf_puts(b, "sp_proc_parameters("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }

  if (emit_concurrency_call(c, id, b)) return;

  /* arr.each / arr.reverse_each with no block -> an external Enumerator over a
     snapshot of the array's (boxed) elements. Block-form and chained
     (each.with_index, each.map) uses are matched earlier and never reach here. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (ty_is_array(comp_ntype(c, recv)) ||
       /* a bare [] literal types UNKNOWN until pushes promote it */
       (comp_ntype(c, recv) == TY_UNKNOWN && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ArrayNode"))) &&
      (sp_streq(name, "each") || sp_streq(name, "reverse_each"))) {
    buf_printf(b, "sp_Enumerator_new_from%s(", sp_streq(name, "reverse_each") ? "_rev" : "");
    emit_boxed(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* str.each_char with no block -> an Enumerator over the string's characters. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_STRING && argc == 0 &&
      nt_ref(nt, id, "block") < 0 && sp_streq(name, "each_char")) {
    buf_puts(b, "sp_Enumerator_new_from_items(sp_str_chars_poly(");
    emit_expr(c, recv, b); buf_puts(b, "))");
    return;
  }

  /* Enumerator instance methods: #next / #peek (raise StopIteration past the
     end), #rewind (reset, returns self), #size. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ENUMERATOR) {
    if (sp_streq(name, "next") && argc == 0) {
      buf_puts(b, "sp_Enumerator_next("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "peek") && argc == 0) {
      buf_puts(b, "sp_Enumerator_peek("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "rewind") && argc == 0) {
      buf_puts(b, "sp_Enumerator_rewind("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "size") && argc == 0) {
      buf_puts(b, "sp_Enumerator_size("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) {
      buf_puts(b, "sp_Enumerator_take("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) {
      buf_puts(b, "sp_Enumerator_to_a("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
  }

  /* Random class methods: Random.rand(n) / Random.rand / Random.bytes(n)
     share a lazily-seeded default instance. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Random")) {
    if (sp_streq(name, "rand")) {
      if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int(sp_random_default_get(), ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else buf_puts(b, "sp_Random_rand_float(sp_random_default_get())");
      return;
    }
    if (sp_streq(name, "bytes") && argc == 1) {
      buf_puts(b, "sp_Random_bytes(sp_random_default_get(), ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Random instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_RANDOM) {
    if (sp_streq(name, "rand")) {
      if (argc >= 1) {
        buf_puts(b, "sp_Random_rand_int("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_puts(b, "sp_Random_rand_float("); emit_expr(c, recv, b); buf_puts(b, ")");
      }
      return;
    }
    if (sp_streq(name, "bytes") && argc == 1) {
      buf_puts(b, "sp_Random_bytes("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* ARGF pseudo-IO methods: read the ARGV files (or stdin) in sequence. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ARGF) {
    if (sp_streq(name, "read")) { buf_puts(b, "sp_argf_read()"); return; }
    if (sp_streq(name, "gets") || sp_streq(name, "readline")) { buf_puts(b, "sp_argf_gets()"); return; }
    if (sp_streq(name, "readlines") || sp_streq(name, "to_a")) { buf_puts(b, "sp_argf_readlines()"); return; }
    if (sp_streq(name, "filename") || sp_streq(name, "path")) { buf_puts(b, "sp_argf_filename()"); return; }
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) { buf_puts(b, "sp_argf_eof()"); return; }
    if (sp_streq(name, "to_s")) { buf_puts(b, "SPL(\"ARGF\")"); return; }
    if ((sp_streq(name, "each_line") || sp_streq(name, "each_string") || sp_streq(name, "each")) &&
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
    if (sp_streq(name, "read")) {
      if (argc == 0) buf_printf(b, "sp_File_read(%s)", r);
      else { buf_puts(b, "sp_File_read_n("); buf_puts(b, r); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      free(rb.p); return;
    }
    if (sp_streq(name, "gets") || sp_streq(name, "readline")) {
      buf_printf(b, "sp_File_gets(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "readlines")) {
      buf_printf(b, "sp_File_readlines(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "write") || sp_streq(name, "syswrite")) {
      if (argc >= 1) {
        buf_printf(b, "sp_File_write(%s, ", r);
        if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
      }
      else buf_puts(b, "0");
      free(rb.p); return;
    }
    if (sp_streq(name, "<<") && argc == 1) {
      /* IO#<< writes the (stringified) operand and returns self, so it chains
         (`io << a << b`). Hold the handle in a temp, write, yield the handle. */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = %s; sp_File_write(_t%d, ", t, r, t);
      if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "); _t%d; })", t);
      free(rb.p); return;
    }
    if (sp_streq(name, "tty?") || sp_streq(name, "isatty")) {
      buf_printf(b, "sp_File_tty_p(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "fileno")) {
      buf_printf(b, "sp_File_fileno(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) {
      buf_printf(b, "sp_File_winsize(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "print") || sp_streq(name, "puts")) {
      /* emit as a statement-like expression: print each arg, return nil.
         Non-string args are stringified via sp_poly_to_s (sp_File_write wants
         a char *), matching Kernel#puts/#print coercion. */
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "({ ");
      for (int k = 0; k < argc; k++) {
        buf_printf(g_pre, "sp_File_write(%s, ", r);
        if (comp_ntype(c, argv[k]) == TY_STRING) emit_expr(c, argv[k], g_pre);
        else { buf_puts(g_pre, "sp_poly_to_s("); emit_boxed(c, argv[k], g_pre); buf_puts(g_pre, ")"); }
        buf_puts(g_pre, "); ");
      }
      if (sp_streq(name, "puts")) buf_printf(g_pre, "sp_File_write(%s, \"\\n\"); ", r);
      buf_puts(g_pre, "});\n");
      (void)t;
      buf_puts(b, "((mrb_int)0)");
      free(rb.p); return;
    }
    if (sp_streq(name, "close")) {
      buf_printf(b, "sp_File_close(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "closed?")) {
      buf_printf(b, "sp_File_closed_p(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) {
      buf_printf(b, "sp_File_eof_p(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "path") || sp_streq(name, "to_path")) {
      buf_printf(b, "sp_File_path(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "flush") || sp_streq(name, "sync=") || sp_streq(name, "sync")) {
      if (sp_streq(name, "sync")) { buf_printf(b, "((mrb_bool)1)"); } /* always synced */
      else { emit_expr(c, recv, b); }
      free(rb.p); return;
    }
    if ((sp_streq(name, "each_line") || sp_streq(name, "each")) &&
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
      /* Read each line into ONE reusable heap line string (allocated once
         per loop, length reset per line) instead of a GC string per line;
         the line does not escape the loop body. A heap string carries a
         real marker header, so runtime helpers can root it -- a raw stack
         buffer must not cross the runtime API (its [-1] byte is arbitrary
         stack memory for the GC mark). */
      buf_printf(b, "char *_t%d = sp_str_alloc(65535); SP_GC_ROOT(_t%d); const char *_t%d; "
                    "while ((_t%d = sp_File_gets_into(_t%d, _t%d, 65536)) != NULL) {",
                 buft, buft, lt, lt, rf, buft);
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
  if (recv >= 0 && sp_streq(name, "<<") && argc == 1 &&
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
      (sp_streq(name, ">>") || sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^"))) {
    TyKind at = comp_ntype(c, argv[0]);
    buf_puts(b, "(sp_poly_to_i("); emit_expr(c, recv, b); buf_printf(b, ") %s ", name);
    if (at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  /* `arr << x` / push / append in value position: mutate, then yield the array
     (statement position is handled earlier by emit_array_mutate_stmt). */
  if (recv >= 0 && (sp_streq(name, "<<") || sp_streq(name, "push") || sp_streq(name, "append")) &&
      argc >= 1 && ty_is_array(comp_ntype(c, recv))) {
    TyKind art = comp_ntype(c, recv);
    /* Lift: when a typed-array literal is pushed a heterogeneous element,
       rebuild the receiver as a PolyArray rather than emitting a type mismatch. */
    int needs_lift = 0;
    if (art != TY_POLY_ARRAY && array_kind(art)) {
      TyKind elem_t = ty_array_elem(art);
      const char *rty = nt_type(nt, recv);
      if (rty && sp_streq(rty, "ArrayNode")) {
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
  if (recv < 0 && sp_streq(name, "__dir__") && argc == 0) {
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
  if (recv < 0 && sp_streq(name, "at_exit") && nt_ref(nt, id, "block") >= 0) {
    g_needs_at_exit = 1;
    buf_puts(b, "(sp_at_exit_hooks[sp_at_exit_count++] = ");
    emit_proc_literal(c, id, b);
    buf_puts(b, ")");
    return;
  }

  /* __method__ / __callee__ -> the enclosing method's name as a symbol
     (nil at the top level) */
  if (recv < 0 && argc == 0 &&
      (sp_streq(name, "__method__") || sp_streq(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->name && s->name[0]) buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, s->name));
    else buf_puts(b, "sp_box_nil()");
    return;
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
    if (g_block_id >= 0) {
      buf_puts(b, "1");
    }
    else if (g_current_scope_is_lowered) {
      buf_puts(b, "("); emit_yblk_ref(b); buf_puts(b, " != NULL)");
    }
    else {
      buf_puts(b, "0");
    }
    return;
  }

  /* Kernel conversions */
  if (recv < 0 && comp_method_index(c, name) < 0) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (sp_streq(name, "Integer") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_i_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "((mrb_int)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (sp_streq(name, "Integer") && ac == 2) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) {
        buf_puts(b, "sp_str_to_i_strict_base("); emit_expr(c, av[0], b);
        buf_puts(b, ", "); emit_expr(c, av[1], b); buf_puts(b, ")");
      }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (sp_streq(name, "Float") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_f_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT) { buf_puts(b, "((mrb_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (sp_streq(name, "String") && ac == 1) {
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
    if (sp_streq(name, "Array") && ac == 1) {
      /* an argument already typed as an array is returned as-is (identity and
         element type preserved); a statically scalar argument wraps into a typed
         one-element array (matching the precise inference); everything else
         routes through the runtime coercion, which yields a poly array. */
      TyKind at = comp_ntype(c, av[0]);
      if (ty_is_array(at)) emit_expr(c, av[0], b);
      else if (at == TY_INT || at == TY_FLOAT || at == TY_STRING) {
        const char *ak = at == TY_INT ? "Int" : at == TY_FLOAT ? "Float" : "Str";
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); sp_%sArray_push(_t%d, ", ak, t, ak, t, ak, t);
        if (at == TY_INT) emit_int_expr(c, av[0], b);
        else if (at == TY_FLOAT) emit_float_expr(c, av[0], b);
        else emit_expr(c, av[0], b);
        buf_printf(b, "); _t%d; })", t);
      }
      else { buf_puts(b, "sp_kernel_array("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && ac >= 1) {
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
        /* Emit the boxed arg into a local buffer first: an arg that is itself a
           call rooting its operands pushes those decls to g_pre, which must land
           as whole statements before this push line, not inside its arg list
           (#1498 / #1508). */
        Buf ab; memset(&ab, 0, sizeof ab);
        emit_boxed(c, av[ai], &ab);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ta, ab.p ? ab.p : "sp_box_nil()");
        free(ab.p);
      }
      buf_printf(b, "sp_str_format_polyarr(_t%d, _t%d)", tf, ta);
      return;
    }
    if (sp_streq(name, "rand")) {
      if (ac == 0) { buf_puts(b, "(mrb_float)((double)rand() / (RAND_MAX + 1.0))"); return; }
      TyKind a0t = comp_ntype(c, av[0]);
      if (a0t == TY_RANGE) {
        int tr = ++g_tmp;
        /* is the range a float range? */
        int is_float = 0;
        const char *atype = nt_type(nt, av[0]);
        if (atype && sp_streq(atype, "RangeNode")) {
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
    if (sp_streq(name, "srand")) {
      if (ac == 0) { buf_puts(b, "(srand((unsigned)time(NULL)), (mrb_int)0)"); return; }
      buf_puts(b, "({ unsigned _sv = (unsigned)("); emit_expr(c, av[0], b); buf_puts(b, "); srand(_sv); (mrb_int)_sv; })");
      return;
    }
  }

  /* exit / abort as expressions (noreturn, emit as C statement-expression) */
  /* sleep(seconds) / Kernel.sleep(seconds) / ::Kernel.sleep(seconds) */
  if (sp_streq(name, "sleep") && argc <= 1 &&
      (recv < 0 ||
       (nt_type(nt, recv) &&
        (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode")) &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Kernel")))) {
    if (argc == 0) { buf_puts(b, "((void)sp_sleep(0.0), (mrb_int)0)"); return; }
    TyKind st = comp_ntype(c, argv[0]);
    buf_puts(b, "((void)sp_sleep(");
    if (st == TY_INT) { buf_puts(b, "(double)"); emit_expr(c, argv[0], b); }
    else if (st == TY_POLY) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_expr(c, argv[0], b);
    buf_puts(b, "), (mrb_int)0)");
    return;
  }
  if (recv < 0 && (sp_streq(name, "exit") || sp_streq(name, "exit!"))) {
    if (argc == 0) { buf_puts(b, "({ exit(0); (mrb_int)0; })"); return; }
    buf_puts(b, "({ exit((int)("); emit_expr(c, argv[0], b); buf_puts(b, ")); (mrb_int)0; })");
    return;
  }
  if (recv < 0 && sp_streq(name, "abort")) {
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
  /* `fail` is an exact alias of `Kernel#raise`. */
  if (recv < 0 && (sp_streq(name, "raise") || sp_streq(name, "fail"))) {
    int args = nt_ref(nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
    if (ac == 0) {
      if (g_rescue_cls) buf_printf(b, "sp_raise_cls(%s, %s)", g_rescue_cls, g_rescue_msg);
      else buf_puts(b, "sp_raise((&(\"\\xff\")[1]))");
    }
    else if (ac == 1 && nt_type(nt, av[0]) &&
             (sp_streq(nt_type(nt, av[0]), "ConstantReadNode") || sp_streq(nt_type(nt, av[0]), "ConstantPathNode"))) {
      /* `raise E` with a user-defined E#initialize is `raise E.new`: construct
         the object (filling initialize's defaults) so its custom initialize and
         any `super`/message run. Without a custom initialize the message
         defaults to the class name, so the (cls, "") fast path is correct. */
      const char *cn = nt_str(nt, av[0], "name");
      int xc = cn ? comp_class_index(c, cn) : -1;
      int ic = (xc >= 0 && class_is_exc_subclass(c, xc))
                 ? comp_method_in_chain(c, xc, "initialize", NULL) : -1;
      if (xc >= 0 && ic >= 0 && c->scopes[ic].reachable) {
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].name);
        emit_args_filled(c, ic, -1, "", b);
        buf_puts(b, "))");
      }
      else buf_printf(b, "sp_raise_cls(\"%s\", (&(\"\\xff\")[1]))", cn ? cn : "");
    }
    else if (ac >= 2 && nt_type(nt, av[0]) &&
             (sp_streq(nt_type(nt, av[0]), "ConstantReadNode") || sp_streq(nt_type(nt, av[0]), "ConstantPathNode"))) {
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
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      buf_puts(b, "sp_exc_message((sp_Exception *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
  }

  /* exception object methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_EXCEPTION) {
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      buf_puts(b, "sp_exc_message("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "full_message")) {
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"%%s: %%s\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    /* detailed_message -> "message (ClassName)" (kwargs like highlight: ignored) */
    if (sp_streq(name, "detailed_message")) {
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"%%s (%%s)\", sp_exc_message(_t%d), sp_exc_class_name(_t%d))", t, t);
      return;
    }
    if (sp_streq(name, "inspect")) {
      /* #<ClassName: message> */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))", t, t);
      return;
    }
    if (sp_streq(name, "class")) {  /* used as .class.to_s / .class.name */
      buf_puts(b, "sp_exc_class_name("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "backtrace")) {
      /* the stack captured at the most recent raise (sp_bt_buf); the substrate
         is live in --debug builds and empty in release, same as Kernel#caller. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_backtrace_captured())");
      return;
    }
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      const char *cn = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
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
      if (sp_streq(name, "new")) {
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
  if (recv >= 0 && argc == 0 && (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
    if (comp_ntype(c, recv) == TY_CLASS) {
      /* When emitting a builtin-reopen method, the inner .class emits
         sp_poly_class_name (returns const char *), not sp_Class — no wrapper needed. */
      int _inner = nt_ref(nt, recv, "receiver");
      int _is_poly_ov = (_inner >= 0 && g_emitting_class_id >= 0 &&
        nt_type(nt, _inner) && sp_streq(nt_type(nt, _inner), "SelfNode") &&
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
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
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
        if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
          Buf rb = expr_buf(c, robj);
          snprintf(objptr, sizeof objptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int ot = ++g_tmp;
          Buf rb = expr_buf(c, robj);
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
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0) {
    buf_printf(b, "SPL(\"%s\")", nt_str(nt, recv, "name"));
    return;
  }
  /* self.name / self.to_s / self.inspect inside a class method -> class name */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      buf_printf(b, "SPL(\"%s\")", c->classes[encl->class_id].name);
      return;
    }
  }
  /* bare `name` inside a class method body -> the class name */
  if (recv < 0 && sp_streq(name, "name") && argc == 0) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      buf_printf(b, "SPL(\"%s\")", c->classes[encl->class_id].name);
      return;
    }
  }
  /* Regexp.last_match(n) -> nth capture group string, or whole match for n=0 */
  if (recv >= 0 && argc == 1 && sp_streq(name, "last_match") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    const char *aty = nt_type(nt, argv[0]);
    if (aty && sp_streq(aty, "IntegerNode")) {
      long long idx = nt_int(nt, argv[0], "value", 0);
      if (idx == 0) { buf_puts(b, "sp_re_match_str"); return; }
      if (idx >= 1 && idx <= 9) { buf_printf(b, "sp_re_captures[%d]", (int)idx); return; }
      buf_puts(b, "NULL");
      return;
    }
    int tv = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = ", tv); emit_int_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
    buf_printf(b, "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL))",
               tv, tv, tv, tv);
    return;
  }
  /* Regexp.escape / Regexp.quote -> escape special regex characters */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "escape") || sp_streq(name, "quote")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    TyKind _re_at = comp_ntype(c, argv[0]);
    if (_re_at == TY_POLY) { buf_puts(b, "sp_re_escape(sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
    else { buf_puts(b, "sp_re_escape("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    return;
  }
  /* Regexp.union(pat, ...) -> a pattern matching the alternation of its operands.
     A String operand is regexp-escaped; a Regexp operand (literal or a constant
     bound to one) contributes its source wrapped in CRuby's `(?on-off:src)` option
     group so its flags survive; a single Array argument is expanded into its
     elements. A runtime Regexp value has no recoverable source (patterns compile
     to bytecode), so that lone form still loud-rejects. */
  if (recv >= 0 && argc >= 0 && sp_streq(name, "union") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    /* `Regexp.union([a, b])`: a lone Array argument supplies the operands. */
    const int *ops = argv; int nops = argc;
    if (argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode"))
      ops = nt_arr(nt, argv[0], "elements", &nops);
    /* A single Regexp operand is returned unchanged (CRuby keeps its source and
       flags verbatim, no option-group wrapper). */
    if (nops == 1 && re_lit_src(c, ops[0]) && emit_regex_pat_to_buf(c, ops[0], b))
      return;
    int ts = ++g_tmp, tp = ++g_tmp;
    for (int i = 0; i < nops; i++) {
      Buf ab; memset(&ab, 0, sizeof ab);
      const char *resrc = re_lit_src(c, ops[i]);
      if (resrc) {
        /* Regexp operand: splice its bare source. The engine has no inline
           option group (?on-off:...), so an operand carrying an i/x/m option
           (re_engine_flags strips the encoding bits, leaving only the
           matching-relevant options; 0 for a flagless operand) can't have its
           flags preserved in the joined pattern -- reject rather than drop them. */
        if (re_engine_flags(re_lit_flags(c, ops[i])) != 0)
          unsupported(c, id, "Regexp.union with a flagged Regexp operand (engine lacks inline option groups)");
        emit_str_literal(&ab, resrc);
      } else {
        TyKind at = comp_ntype(c, ops[i]);
        if (at != TY_STRING && at != TY_POLY)
          unsupported(c, id, "Regexp.union operand without a compile-time source (runtime Regexp or non-String value)");
        if (at == TY_POLY) { buf_puts(&ab, "sp_re_escape(sp_poly_to_s("); emit_expr(c, ops[i], &ab); buf_puts(&ab, "))"); }
        else { buf_puts(&ab, "sp_re_escape("); emit_expr(c, ops[i], &ab); buf_puts(&ab, ")"); }
      }
      emit_indent(g_pre, g_indent);
      if (i == 0) buf_printf(g_pre, "const char *_t%d = %s;\n", ts, ab.p ? ab.p : "\"\"");
      else buf_printf(g_pre, "_t%d = sp_sprintf(\"%%s|%%s\", _t%d, %s);\n", ts, ts, ab.p ? ab.p : "\"\"");
      free(ab.p);
    }
    /* an empty union (`Regexp.union()` or `Regexp.union([])`) never matches */
    if (nops == 0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = \"(?!)\";\n", ts); }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), 0);\n",
               tp, ts, ts, ts);
    buf_printf(b, "_t%d", tp);
    return;
  }
  /* Regexp.linear_time?(re) -> whether re matches in linear time. A literal arg
     is inspected for a backreference (the construct that defeats it); a
     non-literal regexp value defaults to true (the answer for backref-free
     patterns, which is the supported domain). */
  if (recv >= 0 && argc == 1 && sp_streq(name, "linear_time?") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RegularExpressionNode"))
      buf_puts(b, re_src_has_backref(nt_str(nt, argv[0], "unescaped")) ? "FALSE" : "TRUE");
    else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), TRUE)"); }
    return;
  }
  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && argc >= 1 && sp_streq(name, "compile") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    int tp = ++g_tmp, ts = ++g_tmp;
    int flags = (argc >= 2) ? 1 : 0;
    /* See the Regexp.new path: emit the pattern into a local buffer so an
       interpolated arg's embedded-call arg roots land in g_pre as whole
       statements before this temp's decl, not inside its initializer. */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_expr(c, argv[0], &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), %d);\n",
               tp, ts, ts, ts, flags);
    buf_printf(b, "_t%d", tp);
    return;
  }

  /* SomeClass.superclass -> the parent class as sp_Class value */
  if (recv >= 0 && argc == 0 && sp_streq(name, "superclass") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
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
        const char *sc_nm2 = (sc_ty2 && (sp_streq(sc_ty2, "ConstantReadNode") || sp_streq(sc_ty2, "ConstantPathNode"))) ? nt_str(nt, sc_nd, "name") : NULL;
        if (sc_nm2) { int bid2 = builtin_class_id(sc_nm2); if (bid2 != 0) bpar = bid2; }
      }
      buf_printf(b, "((sp_Class){%d})", bpar);
      return;
    }
  }

  /* x.class -> the class-name string (compile-time for known types) */
  if (recv >= 0 && sp_streq(name, "class") && argc == 0) {
    TyKind rt = comp_ntype(c, recv);
    /* When emitting a scope transplanted from a builtin-reopen class (Object/Array/
       Numeric), self is sp_RbVal even if the nscope-based type says otherwise.
       Override the inferred type to TY_POLY so we get sp_poly_class_name(self).
       Exception: TrueClass/FalseClass use int self; keep TY_BOOL for ternary. */
    if (g_emitting_class_id >= 0 &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode") &&
        is_builtin_reopen(c->classes[g_emitting_class_id].name)) {
      const char *ecn = c->classes[g_emitting_class_id].name;
      if (!sp_streq(ecn, "TrueClass") && !sp_streq(ecn, "FalseClass"))
        rt = TY_POLY;
    }
    const char *cn = NULL;
    if (rt == TY_INT) cn = "Integer";
    else if (rt == TY_FLOAT) cn = "Float";
    else if (rt == TY_STRING) cn = "String";
    else if (rt == TY_SYMBOL) cn = "Symbol";
    else if (rt == TY_RANGE) cn = "Range";
    else if (rt == TY_TIME) cn = "Time";
    else if (rt == TY_FIBER) cn = "Fiber";
    else if (rt == TY_ENUMERATOR) cn = "Enumerator";
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
      Buf _rb = expr_buf(c, recv);
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
    if (sp_streq(name, "to_s") || sp_streq(name, "name") || sp_streq(name, "inspect")) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_to_s(_cl%d); })", _clt);
      return;
    }
    if (sp_streq(name, "nil?")) { buf_puts(b, "0"); return; }
    if (sp_streq(name, "class")) {
      buf_printf(b, "({ sp_Class _cl%da = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%da)?SPL(\"Module\"):SPL(\"Class\"); })", _clt);
      return;
    }
    if (sp_streq(name, "superclass") && argc == 0) {
      /* sp_class_superclass only knows the user chain; a builtin class needs
         sp_builtin_superclass (Integer -> Numeric), as sp_class_is_ancestor
         already dispatches. */
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; _cl%d.cls_id>=0?sp_class_superclass(_cl%d):sp_builtin_superclass(_cl%d); })", _clt, _clt, _clt);
      return;
    }
    if (sp_streq(name, "ancestors") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_ancestors(_cl%d); })", _clt);
      return;
    }
    /* ClassName.{,public_,private_,protected_}instance_methods(false):
       compile-time sym array of own methods, filtered by visibility. CRuby's
       `instance_methods` is public+protected; the prefixed forms narrow to a
       single visibility. Only the own-methods (`false`) form is foldable -- the
       inherited form needs built-in ancestor method sets (left to reject). */
    int im_pub = 0, im_prot = 0, im_priv = 0, im_ok = 1;
    if (sp_streq(name, "instance_methods"))             { im_pub = 1; im_prot = 1; }
    else if (sp_streq(name, "public_instance_methods")) { im_pub = 1; }
    else if (sp_streq(name, "protected_instance_methods")) { im_prot = 1; }
    else if (sp_streq(name, "private_instance_methods")) { im_priv = 1; }
    else im_ok = 0;
    if (im_ok && argc == 1) {
      const char *argt = nt_type(nt, argv[0]);
      int is_false_arg = argt && sp_streq(argt, "FalseNode");
      if (is_false_arg) {
        const char *cn2 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")
                          ? nt_str(nt, recv, "name") : NULL;
        int ci2 = cn2 ? comp_class_index(c, cn2) : -1;
        if (ci2 >= 0) {
          ClassInfo *ci3 = &c->classes[ci2];
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
            int v = comp_method_vis(ci3, s->name);
            if (!((v == SP_VIS_PUBLIC && im_pub) || (v == SP_VIS_PROTECTED && im_prot) ||
                  (v == SP_VIS_PRIVATE && im_priv))) continue;
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", ta, s->name);
          }
          /* attr_readers */
          for (int ri = 0; ri < ci3->nreaders; ri++) {
            int v = comp_method_vis(ci3, ci3->readers[ri]);
            if (!((v == SP_VIS_PUBLIC && im_pub) || (v == SP_VIS_PROTECTED && im_prot) ||
                  (v == SP_VIS_PRIVATE && im_priv))) continue;
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", ta, ci3->readers[ri]);
          }
          /* attr_writers (looked up + emitted as "name=") */
          for (int wi = 0; wi < ci3->nwriters; wi++) {
            char wn[256]; snprintf(wn, sizeof wn, "%s=", ci3->writers[wi]);
            int v = comp_method_vis(ci3, wn);
            if (!((v == SP_VIS_PUBLIC && im_pub) || (v == SP_VIS_PROTECTED && im_prot) ||
                  (v == SP_VIS_PRIVATE && im_priv))) continue;
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", ta, wn);
          }
          buf_printf(b, "sp_box_poly_array(_t%d); })", ta);
          return;
        }
      }
    }
    if ((sp_streq(name, "==" ) || sp_streq(name, "eql?")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; _cl%d.cls_id == _cl%da.cls_id; })", _clt, _clt);
        return;
      }
    }
    if (sp_streq(name, "!=" ) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; _cl%d.cls_id != _cl%da.cls_id; })", _clt, _clt);
        return;
      }
    }
    if ((sp_streq(name, "<") || sp_streq(name, "<=") || sp_streq(name, ">") || sp_streq(name, ">=")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        const char *fn = sp_streq(name, "<") ? "sp_class_lt" :
                         sp_streq(name, "<=") ? "sp_class_le" :
                         sp_streq(name, ">") ? "sp_class_gt" : "sp_class_ge";
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; %s(_cl%d, _cl%da); })", fn, _clt, _clt);
        return;
      }
    }
    /* klass.is_a?/kind_of?(Module|Class|Object|BasicObject) */
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      int exact = sp_streq(name, "instance_of?");
      const char *cn2 = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
                        ? nt_str(nt, argv[0], "name") : NULL;
      if (cn2) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b); buf_puts(b, "; ");
        /* Module: all class/module values are instances of Module */
        if (sp_streq(cn2, "Module")) {
          if (exact) buf_printf(b, "sp_class_is_module_val(_cl%d); })", _clt);
          else buf_printf(b, "1; })");
        }
        /* Class: user classes only (not modules); builtin Class constant is -109 */
        else if (sp_streq(cn2, "Class")) {
          if (exact)
            buf_printf(b, "(_cl%d.cls_id>=0?!sp_class_is_module_val(_cl%d):(_cl%d.cls_id==-109)); })", _clt, _clt, _clt);
          else
            buf_printf(b, "(_cl%d.cls_id>=0?!sp_class_is_module_val(_cl%d):(_cl%d.cls_id==-109||_cl%d.cls_id==-108)); })", _clt, _clt, _clt, _clt);
        }
        else if (sp_streq(cn2, "Object") || sp_streq(cn2, "BasicObject")) {
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
    if (ck && sp_streq(name, "freeze")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_%sArray *_t%d = ", ck, t); emit_expr(c, recv, b);
      buf_printf(b, "; if (_t%d) _t%d->frozen = 1; _t%d; })", t, t, t);
      return;
    }
    if (ck && sp_streq(name, "frozen?")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen != 0)");
      return;
    }
  }

  /* freeze / frozen? on hashes: use the GC-header frozen bit */
  if (recv >= 0 && argc == 0 && ty_is_hash(comp_ntype(c, recv))) {
    if (sp_streq(name, "to_h") && nt_ref(nt, id, "block") < 0) {  /* identity */
      emit_expr(c, recv, b);
      return;
    }
    if (sp_streq(name, "freeze")) {
      buf_puts(b, "sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* frozen? on numeric/symbol scalars: always frozen in Ruby semantics.
     TY_STRING uses a runtime check because dup/String.new produce unfrozen strings. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?")) {
    TyKind frt = comp_ntype(c, recv);
    if (frt == TY_INT || frt == TY_FLOAT || frt == TY_SYMBOL || frt == TY_BOOL || frt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return;
    }
    if (frt == TY_STRING) {
      buf_puts(b, "sp_str_is_frozen_val("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (frt == TY_POLY) {
      buf_puts(b, "sp_poly_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
  }

  /* TY_STRING freeze: update the variable to the frozen copy and return it */
  if (recv >= 0 && argc == 0 && sp_streq(name, "freeze") && comp_ntype(c, recv) == TY_STRING) {
    const char *rtyf = nt_type(nt, recv);
    int assignable_f = rtyf && (sp_streq(rtyf, "LocalVariableReadNode") || sp_streq(rtyf, "InstanceVariableReadNode"));
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

  /* dup/clone of a user (pointer) object: allocate a fresh instance, shallow-copy
     the struct (cls_id + all ivars), then -- if the class defines initialize_copy
     -- run it with the original so deep-copy hooks fire. Without this, dup/clone
     fell through to the identity shortcut below and aliased the original.
     Value-type objects copy by value already; exception subclasses use distinct
     allocation, so both stay on the identity path. */
  if (recv >= 0 && (sp_streq(name, "dup") || sp_streq(name, "clone"))) {
    int dargs = nt_ref(nt, id, "arguments");
    int dargc = 0; if (dargs >= 0) nt_arr(nt, dargs, "arguments", &dargc);
    TyKind drt = comp_ntype(c, recv);
    if (dargc == 0 && ty_is_object(drt) && !comp_ty_value_obj(c, drt)) {
      int cid = ty_object_class(drt);
      if (!class_is_exc_subclass(c, cid)) {
        ClassInfo *dci = &c->classes[cid];
        const char *cn = dci->name;
        int defcls = -1;
        int ic = comp_method_in_chain(c, cid, "initialize_copy", &defcls);
        LocalVar *icp = (ic >= 0 && c->scopes[ic].nparams >= 1)
          ? scope_local(&c->scopes[ic], c->scopes[ic].pnames[0]) : NULL;
        TyKind ictp = icp ? icp->type : TY_UNKNOWN;
        int to = ++g_tmp, td = ++g_tmp;
        buf_printf(b, "({ sp_%s *_t%d = ", cn, to); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%s *_t%d = SP_POOL_NEW(%s, %s%s%s);"
                      " *_t%d = *_t%d; SP_GC_ROOT(_t%d); ",
                   to, cn, td, cn,
                   class_needs_scan(dci) ? "sp_" : "", class_needs_scan(dci) ? cn : "NULL",
                   class_needs_scan(dci) ? "_scan" : "", td, to, td);
        /* Invoke the hook when its param was typed by the seeding pass to any
           object class -- it unifies to a common ancestor when both a parent and
           a subclass are dup'd, so accept ty_is_object, casting the original to
           the param's class. TY_POLY -> box it. */
        if (ic >= 0 && (ty_is_object(ictp) || ictp == TY_POLY)) {
          buf_printf(b, "sp_%s_initialize_copy(", c->classes[defcls].name);
          if (defcls != cid) buf_printf(b, "(sp_%s *)", c->classes[defcls].name);
          if (ictp == TY_POLY) { buf_printf(b, "_t%d, sp_box_obj(_t%d, %d)); ", td, to, cid); }
          else {
            int icid = ty_object_class(ictp);
            buf_printf(b, "_t%d, ", td);
            if (icid != cid) buf_printf(b, "(sp_%s *)", c->classes[icid].name);
            buf_printf(b, "_t%d); ", to);
          }
        }
        buf_printf(b, "_t%d; })", td);
        return;
      }
    }
  }

  /* identity methods -> the receiver itself */
  if (recv >= 0 &&
      (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
       sp_streq(name, "dup") || sp_streq(name, "clone"))) {
    int args = nt_ref(nt, id, "arguments");
    int argc0 = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc0);
    /* hash, string, and array dup/clone require real copies (they are mutable
       reference types) -- skip the identity shortcut for them so the dedicated
       sp_*_dup paths run. freeze/itself on any value stay identity. */
    TyKind recv_t = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    int is_dup_clone = sp_streq(name, "dup") || sp_streq(name, "clone");
    if (argc0 == 0 && !ty_is_hash(recv_t) &&
        !(is_dup_clone && (recv_t == TY_STRING || ty_is_array(recv_t)))) {
      emit_expr(c, recv, b); return;
    }
    if (argc0 == 0 && recv_t == TY_STRING && is_dup_clone) {
      /* clone preserves the frozen state; dup always returns an unfrozen copy. */
      /* sp_str_dup, not dup_external: byte_len-aware, carries embedded NULs. */
      buf_printf(b, "%s(", sp_streq(name, "clone") ? "sp_str_clone_val" : "sp_str_dup");
      emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
  }

  /* then / yield_self: pass receiver to block, return block result */
  if (recv >= 0 && (sp_streq(name, "then") || sp_streq(name, "yield_self"))) {
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
        return;
      }
    }
  }

  int ie_direct = recv >= 0 && (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec"));
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
    if (blk >= 0 && nt_type(nt, blk) && sp_streq(nt_type(nt, blk), "BlockArgumentNode"))
      blk = g_block_id;
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
        int is_exec = ie_tramp ? (ie_tramp == 2) : sp_streq(name, "instance_exec");
        int bp_node = nt_ref(nt, blk, "parameters");
        const char *bpty = bp_node >= 0 ? nt_type(nt, bp_node) : NULL;
        int iargs = nt_ref(nt, id, "arguments");
        int iac = 0; const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iac) : NULL;
        /* a trailing `k: v` call-site hash binds keyword params, not positionals */
        int ie_kwhash = ie_call_kwhash(c, id);
        if (ie_kwhash >= 0) iac -= 1;
        if (bpty && sp_streq(bpty, "NumberedParametersNode")) {
          /* `{ _1.method }`: _1.._N bind like positional block params. */
          int maxn = (int)nt_int(nt, bp_node, "maximum", 0);
          for (int p = 0; p < maxn; p++) {
            char pn[16]; snprintf(pn, sizeof pn, "_%d", p + 1);
            LocalVar *plv = scope_local(comp_scope_of(c, id), pn);
            int ppoly = plv && plv->type == TY_POLY;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "lv_%s = ", rename_local(pn));
            if (is_exec) {
              if (p < iac) { if (ppoly) emit_boxed(c, iav[p], g_pre); else emit_expr(c, iav[p], g_pre); }
              else emit_ie_param_default(c, plv ? plv->type : TY_POLY, g_pre);
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
        int is_splat = arg0 >= 0 && nt_type(nt, arg0) && sp_streq(nt_type(nt, arg0), "SplatNode");
        if (is_splat) arg0 = nt_ref(nt, arg0, "expression");
        if (tramp_argc < 0 && is_exec && iac == 1 && (npar >= 2 || (npar >= 1 && is_splat)) && arg0 >= 0) {
          TyKind a0 = comp_ntype(c, arg0);
          if (ty_is_array(a0)) {
            as_kind = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
            as_arr = ++g_tmp;
            /* Evaluate the array into a side buffer so its own prelude flushes
               to g_pre before this declaration line (avoid splicing mid-line). */
            Buf ab = expr_buf(c, arg0);
            emit_indent(g_pre, g_indent); emit_ctype(c, a0, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", as_arr, ab.p ? ab.p : "NULL"); free(ab.p);
          }
        }
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
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "lv_%s = ", rename_local(pn));
          if (as_kind) {
            /* element of the auto-splat array; box the scalar kinds into the
               poly slot (PolyArray_get already yields an sp_RbVal). */
            const char *bx = !ppoly || sp_streq(as_kind, "Poly") ? NULL
                           : sp_streq(as_kind, "Int") ? "sp_box_int"
                           : sp_streq(as_kind, "Float") ? "sp_box_float"
                           : sp_streq(as_kind, "Str") ? "sp_box_str" : NULL;
            if (bx) buf_printf(g_pre, "%s(", bx);
            buf_printf(g_pre, "sp_%sArray_get(_t%d, %d)", as_kind, as_arr, p);
            if (bx) buf_puts(g_pre, ")");
          }
          else if (tramp_argc >= 0) {
            int an = ie_tramp_effective_arg(c, id, p);
            Buf eb; memset(&eb, 0, sizeof eb);
            if (an >= 0) { if (ppoly) emit_boxed(c, an, &eb); else emit_expr(c, an, &eb); }
            else buf_puts(&eb, "0");
            if (an >= 0 && pscalar && comp_ntype(c, an) == TY_POLY)
              emit_unbox_text(c, plv->type, eb.p ? eb.p : "", g_pre);
            else buf_puts(g_pre, eb.p ? eb.p : "0");
            free(eb.p);
          }
          else if (is_exec) {
            if (p < iac) {
              if (ppoly) emit_boxed(c, iav[p], g_pre);
              else if (pscalar && comp_ntype(c, iav[p]) == TY_POLY) {
                Buf eb = expr_buf(c, iav[p]);
                emit_unbox_text(c, plv->type, eb.p ? eb.p : "", g_pre); free(eb.p);
              }
              else emit_expr(c, iav[p], g_pre);
            }
            else emit_ie_param_default(c, plv ? plv->type : TY_POLY, g_pre);
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
          LocalVar *kplv = scope_local(comp_scope_of(c, id), kpn);
          int kppoly = kplv && kplv->type == TY_POLY;
          Buf vb; memset(&vb, 0, sizeof vb);
          if (vn >= 0) { if (kppoly) emit_boxed(c, vn, &vb); else emit_expr(c, vn, &vb); }
          else buf_puts(&vb, "0");
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
        /* the splice body's break binds to the do/while(0) below, never to an
           enclosing valued-break scope */
        const char *sv_bser = g_brk_ser_var; g_brk_ser_var = NULL;
        int sv_iep = g_ie_res_poly;
        g_ie_res_poly = (scalar_res && body_ty == TY_POLY);
        char bvbuf[32];
        int sv_lexc2 = g_loop_exc_base;
        if (ie_bn_wrap) {
          g_loop_exc_base = g_exc_frame_depth;   /* break/next exit the do{}while(0) */
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "do {\n"); g_indent++;
          if (scalar_res) { snprintf(bvbuf, sizeof bvbuf, "_t%d", tres); g_loop_break_var = bvbuf; g_ie_next_var = bvbuf; }
          else { g_loop_break_var = NULL; g_ie_next_var = NULL; }
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
          g_loop_break_var = sv_lb; g_ie_next_var = sv_nx;
          g_indent--; emit_indent(g_pre, g_indent); buf_puts(g_pre, "} while (0);\n");
        }
        g_loop_exc_base = sv_lexc2;
        g_ie_res_poly = sv_iep;
        g_brk_ser_var = sv_bser;
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
        if (sp_streq(bcn, "String"))        brt = TY_STRING;
        else if (sp_streq(bcn, "Integer"))  brt = TY_INT;
        else if (sp_streq(bcn, "Float"))    brt = TY_FLOAT;
        else if (sp_streq(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          int args2 = nt_ref(nt, id, "arguments");
          int ac2 = 0; const int *av2 = args2 >= 0 ? nt_arr(nt, args2, "arguments", &ac2) : NULL;
          const char *s = g_self;
          if (brt == TY_STRING) {
            if (sp_streq(name, "upcase"))     { buf_printf(b, "sp_str_upcase(%s)", s); return; }
            if (sp_streq(name, "downcase"))   { buf_printf(b, "sp_str_downcase(%s)", s); return; }
            if (sp_streq(name, "capitalize")) { buf_printf(b, "sp_str_capitalize(%s)", s); return; }
            if (sp_streq(name, "reverse"))    { buf_printf(b, "sp_str_reverse(%s)", s); return; }
            if (sp_streq(name, "strip"))      { buf_printf(b, "sp_str_strip(%s)", s); return; }
            if (sp_streq(name, "lstrip"))     { buf_printf(b, "sp_str_lstrip(%s)", s); return; }
            if (sp_streq(name, "rstrip"))     { buf_printf(b, "sp_str_rstrip(%s)", s); return; }
            if (sp_streq(name, "chomp"))      { buf_printf(b, "sp_str_chomp(%s, NULL)", s); return; }
            if (sp_streq(name, "chop"))       { buf_printf(b, "sp_str_chop(%s)", s); return; }
            if (sp_streq(name, "dup") || sp_streq(name, "clone")) { buf_printf(b, "sp_str_dup(%s)", s); return; }
            if (sp_streq(name, "to_s") || sp_streq(name, "itself")) { buf_puts(b, s); return; }
            if (sp_streq(name, "to_sym"))     { buf_printf(b, "sp_sym_intern(%s)", s); return; }
            if (sp_streq(name, "to_i"))       { buf_printf(b, "sp_str_to_i(%s)", s); return; }
            if (sp_streq(name, "to_f"))       { buf_printf(b, "sp_str_to_f(%s)", s); return; }
            if (sp_streq(name, "length") || sp_streq(name, "size")) { buf_printf(b, "sp_str_length(%s)", s); return; }
            if (sp_streq(name, "bytesize"))   { buf_printf(b, "sp_str_bytesize(%s)", s); return; }
            if (sp_streq(name, "empty?"))     { buf_printf(b, "(!%s || !*%s)", s, s); return; }
            if (sp_streq(name, "inspect"))    { buf_printf(b, "sp_str_inspect(%s)", s); return; }
            if (sp_streq(name, "+") && ac2 == 1) {
              buf_printf(b, "sp_str_concat(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "*") && ac2 == 1) {
              buf_printf(b, "sp_str_repeat(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_INT) {
            if (sp_streq(name, "to_s"))   { buf_printf(b, "sp_int_to_s(%s)", s); return; }
            if (sp_streq(name, "to_f"))   { buf_printf(b, "((double)(%s))", s); return; }
            if (sp_streq(name, "abs"))    { buf_printf(b, "sp_int_abs(%s)", s); return; }
            if (sp_streq(name, "odd?"))   { buf_printf(b, "((%s) %% 2 != 0)", s); return; }
            if (sp_streq(name, "even?"))  { buf_printf(b, "((%s) %% 2 == 0)", s); return; }
            if (sp_streq(name, "zero?"))  { buf_printf(b, "((%s) == 0)", s); return; }
            if (sp_streq(name, "succ") || sp_streq(name, "next")) { buf_printf(b, "sp_int_add(%s, 1LL)", s); return; }
            if (sp_streq(name, "+") && ac2 == 1) {
              buf_printf(b, "sp_int_add(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "-") && ac2 == 1) {
              buf_printf(b, "sp_int_sub(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "*") && ac2 == 1) {
              buf_printf(b, "sp_int_mul(%s, ", s); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "/") && ac2 == 1) {
              buf_printf(b, "sp_idiv(%s, ", s); emit_int_divisor(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "%") && ac2 == 1) {
              buf_printf(b, "sp_imod(%s, ", s); emit_int_divisor(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_FLOAT) {
            if (sp_streq(name, "to_s"))   { buf_printf(b, "sp_float_to_s(%s)", s); return; }
            if (sp_streq(name, "to_i"))   { buf_printf(b, "((mrb_int)(%s))", s); return; }
            if (sp_streq(name, "abs"))    { buf_printf(b, "fabs(%s)", s); return; }
            if (sp_streq(name, "floor"))  { buf_printf(b, "((double)((mrb_int)floor(%s)))", s); return; }
            if (sp_streq(name, "ceil"))   { buf_printf(b, "((double)((mrb_int)ceil(%s)))", s); return; }
            if (sp_streq(name, "round"))  { buf_printf(b, "((double)((mrb_int)round(%s)))", s); return; }
            if (sp_streq(name, "+") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " + "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "-") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " - "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "*") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " * "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
            if (sp_streq(name, "/") && ac2 == 1) {
              buf_puts(b, "("); buf_puts(b, s); buf_puts(b, " / "); emit_expr(c, av2[0], b); buf_puts(b, ")"); return;
            }
          }
          else if (brt == TY_SYMBOL) {
            if (sp_streq(name, "to_s") || sp_streq(name, "id2name")) {
              buf_printf(b, "sp_sym_to_s(%s)", s); return;
            }
            if (sp_streq(name, "inspect")) { buf_printf(b, "sp_sym_inspect(%s)", s); return; }
            if (sp_streq(name, "to_sym") || sp_streq(name, "itself")) { buf_puts(b, s); return; }
          }
        }
      }
    }
  }

  /* TY_CLASS variable .new -> runtime switch over user classes, returns TY_POLY */
  if (recv >= 0 && sp_streq(name, "new") && comp_ntype(c, recv) == TY_CLASS &&
      nt_type(nt, recv) &&
      !sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      !sp_streq(nt_type(nt, recv), "ConstantPathNode") &&
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
  /* Class#allocate: a bare instance with default/nil ivars and no initialize.
     Exception subclasses carry raise/message state set up by their dedicated
     constructor, so they are excluded (fall through to the generic reject). */
  if (recv >= 0 && sp_streq(name, "allocate") && argc == 0 && comp_ntype(c, recv) == TY_CLASS &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    int acid = comp_class_index(c, nt_str(nt, recv, "name"));
    if (acid >= 0 && !class_is_exc_subclass(c, acid)) {
      emit_obj_alloc_expr(c, acid, b);
      return;
    }
  }

  if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      sp_streq(nt_str(nt, recv, "name"), "class")) {
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
  if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
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

  /* Thread class methods: Thread.current / Thread.pass (recv is the Thread
     constant). Handled before the Class.new dispatch since they are not `new`. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *tcn = nt_str(nt, recv, "name");
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "current") && argc == 0) {
      buf_puts(b, "sp_Thread_current()"); return;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "main") && argc == 0) {
      buf_puts(b, "sp_Thread_main()"); return;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "list") && argc == 0) {
      /* build a poly array of the live threads (the TU owns sp_PolyArray) */
      int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
      buf_printf(b, " mrb_int _t%d = sp_Thread_list_count();", tn);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                    " sp_PolyArray_push(_t%d, sp_box_obj((void *)sp_Thread_list_at(_t%d), SP_BUILTIN_THREAD));",
                 ti, ti, tn, ti, ta, ti);
      buf_printf(b, " _t%d; })", ta);
      return;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "pass") && argc == 0) {
      /* Thread.pass yields the scheduler and evaluates to nil. */
      buf_puts(b, "(sp_Thread_pass(), sp_box_nil())"); return;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "report_on_exception=") && argc == 1) {
      buf_puts(b, "sp_Thread_set_report_default("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (tcn && sp_streq(tcn, "Thread") && sp_streq(name, "report_on_exception") && argc == 0) {
      buf_puts(b, "sp_Thread_get_report_default()"); return;
    }
  }

  if (emit_class_new_call(c, id, b)) return;

  /* StringIO.open(args) { |io| body } -> run the block with a fresh IO,
     return the block's value, then close. */
  if (recv >= 0 && sp_streq(name, "open") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      sp_streq(nt_str(nt, recv, "name"), "StringIO") && sp_feature_enabled("stringio")) {
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
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC")) {
    if (sp_streq(name, "start") && argc == 0) { buf_puts(b, "(sp_gc_collect(), (mrb_int)0)"); return; }
    if (sp_streq(name, "compact") && argc == 0) { buf_puts(b, "(sp_gc_collect(), (mrb_int)0)"); return; }
    if (sp_streq(name, "stat") && argc == 0) { buf_puts(b, "sp_gc_stat()"); return; }
  }

  /* Fiber class methods: Fiber.yield(val) and Fiber.current */
  if (recv_is_const(nt, recv, "Fiber")) {
    if (sp_streq(name, "yield")) {
      if (argc == 0) buf_puts(b, "sp_Fiber_yield(sp_box_nil())");
      else { buf_puts(b, "sp_Fiber_yield("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      return;
    }
    if (sp_streq(name, "current") && argc == 0) {
      buf_puts(b, "sp_fiber_current");
      return;
    }
  }

  /* Process module methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Process")) {
    if (sp_streq(name, "pid") && argc == 0) { buf_puts(b, "((mrb_int)getpid())"); return; }
    if (sp_streq(name, "ppid") && argc == 0) { buf_puts(b, "sp_process_ppid()"); return; }
    if (sp_streq(name, "clock_gettime") && argc >= 1) {
      buf_puts(b, "sp_process_clock_gettime()"); return;
    }
  }

  /* Integer.sqrt(n) -> integer square root (exact, Newton's method) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      sp_streq(name, "sqrt") && argc == 1) {
    buf_puts(b, "sp_int_sqrt("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* Marshal (Phase 1): dump a value to a binary String, load one back as poly */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Marshal")) {
    if (sp_streq(name, "dump") && argc == 1) {
      buf_puts(b, "sp_marshal_dump("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "load") && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", t); emit_str_expr(c, argv[0], b);
      buf_printf(b, "; sp_marshal_load(_t%d, (mrb_int)sp_str_byte_len(_t%d)); })", t, t);
      return;
    }
  }

  /* Math module functions -> C math.h equivalents */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math")) {
    /* 1-arg functions */
    /* Domain-restricted functions route through sp_math_* wrappers that
       raise Math::DomainError on out-of-domain input (CRuby parity); the
       rest call libc directly (all reals are in domain). */
    const char *cfn = NULL;
    if      (sp_streq(name, "sin"))   cfn = "sin";
    else if (sp_streq(name, "cos"))   cfn = "cos";
    else if (sp_streq(name, "tan"))   cfn = "tan";
    else if (sp_streq(name, "asin"))  cfn = "sp_math_asin";
    else if (sp_streq(name, "acos"))  cfn = "sp_math_acos";
    else if (sp_streq(name, "atan"))  cfn = "atan";
    else if (sp_streq(name, "sinh"))  cfn = "sinh";
    else if (sp_streq(name, "cosh"))  cfn = "cosh";
    else if (sp_streq(name, "tanh"))  cfn = "tanh";
    else if (sp_streq(name, "asinh")) cfn = "asinh";
    else if (sp_streq(name, "acosh")) cfn = "sp_math_acosh";
    else if (sp_streq(name, "atanh")) cfn = "sp_math_atanh";
    else if (sp_streq(name, "exp"))   cfn = "exp";
    else if (sp_streq(name, "sqrt"))  cfn = "sp_math_sqrt";
    else if (sp_streq(name, "cbrt"))  cfn = "cbrt";
    else if (sp_streq(name, "erf"))   cfn = "erf";
    else if (sp_streq(name, "erfc"))  cfn = "erfc";
    else if (sp_streq(name, "gamma")) cfn = "sp_math_gamma";
    if (cfn && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_printf(b, "%s(", cfn);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "lgamma") && argc == 1) {
      /* Math.lgamma(x) -> [log(|gamma(x)|), sign] as a poly array */
      buf_puts(b, "sp_math_lgamma((double)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return;
    }
    /* Math.log(x) or Math.log(x, base) */
    if (sp_streq(name, "log") && (argc == 1 || argc == 2)) {
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
    if (sp_streq(name, "log2") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_math_log2(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "log10") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "sp_math_log10(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Math.atan2(y, x), Math.hypot(x, y), Math.ldexp(x, e) */
    if ((sp_streq(name, "atan2") || sp_streq(name, "hypot")) && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      TyKind a1t = comp_ntype(c, argv[1]);
      buf_printf(b, "%s(", name);
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (a1t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "ldexp") && argc == 2) {
      TyKind a0t = comp_ntype(c, argv[0]);
      buf_puts(b, "ldexp(");
      if (a0t == TY_INT) buf_puts(b, "(double)");
      emit_expr(c, argv[0], b); buf_puts(b, ", (int)");
      emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
  }

  /* JSON.generate(x) / JSON.dump(x) -> serialize a boxed value */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "JSON") &&
      (sp_streq(name, "generate") || sp_streq(name, "dump")) && argc == 1 && sp_feature_enabled("json")) {
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
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (sp_streq(name, "exist?") || sp_streq(name, "exists?")) && argc == 1) {
    buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* File class methods -> runtime helpers (the runtime has long carried
     these; only the dispatch was missing). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "File")) {
    if ((sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "read") || sp_streq(name, "binread")) && argc == 1) {
      buf_puts(b, "sp_file_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "write") || sp_streq(name, "binwrite")) && argc == 2) {
      /* runtime write is void; Ruby returns the byte count */
      buf_puts(b, "({ const char *_wp = "); emit_expr(c, argv[0], b);
      buf_puts(b, "; const char *_wd = ");
      if (comp_ntype(c, argv[1]) == TY_POLY) {
        buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_file_write(_wp, _wd); (mrb_int)sp_str_byte_len(_wd); })"); return;
    }
    if ((sp_streq(name, "exist?") || sp_streq(name, "exists?") || sp_streq(name, "readable?")) && argc == 1) {
      buf_puts(b, "sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "directory?") || sp_streq(name, "zero?") || sp_streq(name, "empty?")) && argc == 1) {
      buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "file?") && argc == 1) {
      buf_puts(b, "(!sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ") && sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
    }
    if (sp_streq(name, "delete") && argc == 1) {
      buf_puts(b, "({ sp_file_delete("); emit_expr(c, argv[0], b); buf_puts(b, "); (mrb_int)1; })"); return;
    }
    if (sp_streq(name, "mtime") && argc == 1) {
      buf_puts(b, "sp_file_mtime("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "size") && argc == 1) {
      buf_puts(b, "sp_file_size("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "expand_path") && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_expand_path("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_expr(c, argv[1], b); else buf_puts(b, "(const char *)0");
      buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "join")) {
      buf_printf(b, "sp_file_join((const char*[]){");
      for (int k = 0; k < argc; k++) { if (k) buf_puts(b, ", "); emit_expr(c, argv[k], b); }
      if (argc == 0) buf_puts(b, "(const char*)0");
      buf_printf(b, "}, %d)", argc); return;
    }
    if (sp_streq(name, "readlines") && argc >= 1) {
      /* File.readlines(path) or File.readlines(path, chomp: true) */
      int chomp = 0;
      for (int ki = 1; ki < argc; ki++) {
        const char *kty = nt_type(nt, argv[ki]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[ki], "chomp");
          if (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "TrueNode"))
            chomp = 1;
        }
      }
      if (chomp) buf_puts(b, "sp_file_readlines_chomp(");
      else buf_puts(b, "sp_file_readlines(");
      emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* File.open(path, mode) / File.new(path, mode) without block -> TY_IO handle */
    if (sp_streq(name, "open") || sp_streq(name, "new")) {
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
                         (nt_type(nt, bb[bn-1]) && sp_streq(nt_type(nt, bb[bn-1]), "NilNode"))));
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
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
    if (sp_streq(name, "pwd") && argc == 0) { buf_puts(b, "sp_dir_pwd()"); return; }
    if (sp_streq(name, "home") && argc == 0) { buf_puts(b, "sp_dir_home()"); return; }
    if (sp_streq(name, "glob") && argc == 1) {
      buf_puts(b, "sp_dir_glob("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "entries") || sp_streq(name, "children")) && argc == 1) {
      buf_printf(b, "sp_dir_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "mkdir") || sp_streq(name, "rmdir") || sp_streq(name, "chdir")) && argc >= 1) {
      buf_printf(b, "sp_dir_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
  }

  /* Time class constructors */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Time")) {
    if ((sp_streq(name, "now") || sp_streq(name, "new")) && argc == 0) { buf_puts(b, "sp_time_now()"); return; }
    if (sp_streq(name, "at") && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      buf_printf(b, "sp_time_at_%s(", at == TY_FLOAT ? "float" : "int");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if ((sp_streq(name, "local") || sp_streq(name, "mktime") ||
         sp_streq(name, "utc") || sp_streq(name, "gm") || sp_streq(name, "new")) && argc >= 1) {
      /* y[,mo,d,h,mi,s] -- missing trailing parts default (mo/d=1, rest 0) */
      int is_utc = (sp_streq(name, "utc") || sp_streq(name, "gm"));
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
    if (rty_ffi && sp_streq(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (rty_ffi && sp_streq(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      int fi = -1;
      for (int ffi_i = 0; ffi_i < c->n_ffi_funcs; ffi_i++)
        if (sp_streq(c->ffi_funcs[ffi_i].mod, rcmod) && sp_streq(c->ffi_funcs[ffi_i].name, name)) {
          fi = ffi_i; break;
        }
      if (fi >= 0) {
        const char *ret_spec = c->ffi_funcs[fi].ret;
        int is_void_ret = sp_streq(ret_spec, "void");
        int is_ptr_ret  = sp_streq(ret_spec, "ptr");
        int is_str_ret  = sp_streq(ret_spec, "str");
        int is_binstr_ret = sp_streq(ret_spec, "binstr");
        int call_argc = c->ffi_funcs[fi].nargs;
        /* Build the raw C call */
        Buf call_buf; memset(&call_buf, 0, sizeof call_buf);
        buf_puts(&call_buf, c->ffi_funcs[fi].name);
        buf_puts(&call_buf, "(");
        for (int ai = 0; ai < call_argc && ai < argc; ai++) {
          if (ai) buf_puts(&call_buf, ", ");
          const char *spec = c->ffi_funcs[fi].args[ai];
          TyKind at = comp_ntype(c, argv[ai]);
          if (sp_streq(spec, "str")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.s");
            }
            else emit_expr(c, argv[ai], &call_buf);
          }
          else if (sp_streq(spec, "ptr")) {
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
          else if (sp_streq(spec, "float") || sp_streq(spec, "double")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")(");
              emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.f)");
            }
            else { buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (sp_streq(spec, "int_array")) {
            /* Hand off element data, never the array struct pointer (which
               would pun the header / read boxed sp_RbVal tags as ints). */
            if (at == TY_INT_ARRAY)        { buf_puts(&call_buf, "sp_IntArray_ffi_data(");   emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_int_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_PolyArray_ffi_int_data((sp_PolyArray *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ").v.p)"); }
            else                           { buf_puts(&call_buf, "((const int64_t *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (sp_streq(spec, "float_array")) {
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
          int ffi_ret_is_float = (sp_streq(ret_spec, "float") || sp_streq(ret_spec, "double"));
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
          if (sp_streq(c->ffi_bufs[fbi].mod, rcmod) && sp_streq(c->ffi_bufs[fbi].name, name)) {
            bi = fbi; break;
          }
        if (bi >= 0) {
          buf_printf(b, "sp_box_foreign_ptr((void *)sp_ffi_buf_%s_%s)",
                     c->ffi_bufs[bi].mod, c->ffi_bufs[bi].name);
          return;
        }
      }
      /* ffi_read_* access: Module.reader_name(buf) */
      {
        int ri = -1;
        for (int fri = 0; fri < c->n_ffi_readers; fri++)
          if (sp_streq(c->ffi_readers[fri].mod, rcmod) && sp_streq(c->ffi_readers[fri].name, name)) {
            ri = fri; break;
          }
        if (ri >= 0 && argc >= 1) {
          const char *kind = c->ffi_readers[ri].kind;
          int off = c->ffi_readers[ri].offset;
          const char *ctype = "uint32_t";
          if (kind && sp_streq(kind, "i32")) ctype = "int32_t";
          if (argc >= 1) {
            if (kind && sp_streq(kind, "ptr")) {
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
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
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
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
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
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
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
    if (rre >= 0 && (sp_streq(name, "match?") || sp_streq(name, "===")) && argc == 1) {
      /* /re/ === str and /re/.match?(str) both yield a match boolean */
      if (a0 == TY_POLY) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, sp_poly_to_s(", rre); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      return;
    }
    if (rre >= 0 && sp_streq(name, "match?") && argc == 2) {
      buf_printf(b, "sp_re_match_p_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    /* /re/ =~ str -> match offset or nil (poly) */
    if (rre >= 0 && sp_streq(name, "=~") && argc == 1 && a0 == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* /re/.source and /re/.options are compile-time constants of the literal */
    if (rre >= 0 && sp_streq(name, "source") && argc == 0) {
      emit_str_literal(b, nt_str(nt, recv, "unescaped")); return;
    }
    if (rre >= 0 && sp_streq(name, "options") && argc == 0) {
      int pf = (int)nt_int(nt, recv, "flags", 0);
      int opt = ((pf & 4) ? 1 : 0) | ((pf & 8) ? 2 : 0) | ((pf & 16) ? 4 : 0);
      buf_printf(b, "%d", opt); return;
    }
    if (rre >= 0 && sp_streq(name, "encoding") && argc == 0) {
      int ascii = re_src_all_ascii(nt_str(nt, recv, "unescaped"));
      buf_printf(b, "sp_box_encoding(%s)", ascii ? "sp_encoding_us_ascii()" : "sp_encoding_utf8()");
      return;
    }
    if (rre >= 0 && sp_streq(name, "fixed_encoding?") && argc == 0) {
      buf_puts(b, re_src_all_ascii(nt_str(nt, recv, "unescaped")) ? "FALSE" : "TRUE");
      return;
    }
  }
  /* encoding/fixed_encoding? on a non-literal regexp value: the source is not
     visible at compile time, so default to US-ASCII (the answer for any 7-bit
     pattern, which is the supported domain). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 0) {
    if (sp_streq(name, "encoding")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_box_encoding(sp_encoding_us_ascii()))"); return;
    }
    if (sp_streq(name, "fixed_encoding?")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), FALSE)"); return;
    }
  }
  if (recv >= 0 && argc >= 1 && rt != TY_SYMBOL &&
      (sp_streq(name, "match?") || sp_streq(name, "!~") || sp_streq(name, "=~") || sp_streq(name, "match"))) {
    int are = re_lit_index(c, argv[0]);
    if (are >= 0 && sp_streq(name, "=~") && rt == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && sp_streq(name, "!~")) {
      buf_printf(b, "(!sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (are >= 0 && sp_streq(name, "match?")) {
      if (argc == 1) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")"); return; }
      buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (are >= 0 && sp_streq(name, "match")) {
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
    if (rre >= 0 && sp_streq(name, "match") && (argc == 1 || argc == 2)) {
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
      int is_interp_arg = a0ty && sp_streq(a0ty, "InterpolatedRegularExpressionNode");
      int is_regex_lv_arg = !is_interp_arg && argc >= 1 && comp_ntype(c, argv[0]) == TY_REGEX
                            && nt_type(nt, argv[0])
                            && (sp_streq(nt_type(nt, argv[0]), "LocalVariableReadNode") ||
                                sp_streq(nt_type(nt, argv[0]), "ConstantReadNode"));
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
          if (sp_streq(name, "match?") && argc == 1) {
            /* A symbol receiver matches over its name, so feed the runtime
               pattern the symbol's string rather than the raw sp_sym. */
            if (rt == TY_SYMBOL) { buf_printf(b, "sp_re_match_p(%s, sp_sym_to_s(", rp.p); emit_expr(c, recv, b); buf_puts(b, "))"); }
            else { buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")"); }
            free(rp.p); return;
          }
          if (sp_streq(name, "=~") && rt == TY_STRING) {
            buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return;
          }
          if (sp_streq(name, "!~")) {
            buf_printf(b, "(!sp_re_match_p(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, "))");
            free(rp.p); return;
          }
          if (sp_streq(name, "match") && argc == 1) {
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
      int is_interp_recv = rty && sp_streq(rty, "InterpolatedRegularExpressionNode");
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
          if ((sp_streq(name, "match?") || sp_streq(name, "===")) && argc == 1) {
            if (a0 == TY_POLY) { buf_printf(b, "sp_re_match_p(%s, sp_poly_to_s(", rp.p); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
            else { buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            free(rp.p); return;
          }
          if (sp_streq(name, "=~") && argc == 1) {
            if (a0 == TY_STRING) {
              buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")");
            }
            else if (a0 == TY_POLY) {
              /* runtime type check: raise TypeError if not a string */
              int tv = ++g_tmp;
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "if (_t%d.tag != SP_TAG_STR) sp_raise_no_str_conversion(_t%d);\n", tv, tv);
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
          if (sp_streq(name, "match") && (argc == 1 || argc == 2)) {
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
  if (recv >= 0 && rt == TY_STRING && sp_streq(name, "%") && argc == 1) {
    TyKind at = a0;
    /* A nil (NULL) receiver is CRuby's NoMethodError. The check sits at the
       call site rather than inside sp_str_format_polyarr, whose body is
       optcarrot-layout-sensitive (a guard there cost ~9% fps); a literal
       format can't be nil and is emitted bare. */
    const char *frty = nt_type(nt, recv);
    int fck = (frty && (sp_streq(frty, "StringNode") || sp_streq(frty, "InterpolatedStringNode")))
              ? -1 : ++g_tmp;
    if (at == TY_POLY_ARRAY) {
      if (fck >= 0) {
        buf_printf(b, "sp_str_format_polyarr(({ const char *_t%d = ", fck);
        emit_expr(c, recv, b);
        buf_printf(b, "; if (!_t%d) sp_nil_recv(\"%%\"); _t%d; }), ", fck, fck);
      }
      else { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); buf_puts(b, ", "); }
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *ak = array_kind(at);
    if (ak) {
      const char *kind = at == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : at == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      if (fck >= 0) {
        buf_printf(b, "sp_str_format_polyarr(({ const char *_t%d = ", fck);
        emit_expr(c, recv, b);
        buf_printf(b, "; if (!_t%d) sp_nil_recv(\"%%\"); _t%d; })", fck, fck);
      }
      else { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); }
      buf_puts(b, ", sp_typed_to_poly((void *)("); emit_expr(c, argv[0], b);
      buf_printf(b, "), %s))", kind);
      return;
    }
    /* named references ("%<name>spec" / "%{name}") reading from a symbol-keyed
       hash. Handled when the format is a string literal, so each name resolves
       to a compile-time symbol id; the looked-up values are pushed in order and
       the rewritten positional format reuses sp_str_format_polyarr. */
    const char *recv_ntype = nt_type(nt, recv);
    if (ty_is_hash(at) && recv_ntype && sp_streq(recv_ntype, "StringNode")) {
      const char *fmt = nt_str(nt, recv, "content");
      const char *names[64]; int name_len[64];
      Buf rew; memset(&rew, 0, sizeof rew);
      int nref = fmt ? parse_named_format(fmt, &rew, names, name_len, 64) : -1;
      if (nref >= 0) {
        int th = ++g_tmp, ta = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", th); emit_boxed(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " SP_GC_ROOT(_t%d); ", th, ta, ta);
        for (int k = 0; k < nref; k++) {
          char nm[128];   /* parse_named_format guarantees name_len[k] < 128 */
          memcpy(nm, names[k], (size_t)name_len[k]); nm[name_len[k]] = 0;
          buf_printf(b, "sp_PolyArray_push(_t%d, sp_poly_get_sym(_t%d, (sp_sym)%d)); ",
                     ta, th, comp_sym_intern(c, nm));
        }
        buf_puts(b, "sp_str_format_polyarr(");
        emit_str_literal(b, rew.p ? rew.p : "");
        buf_printf(b, ", _t%d); })", ta);
        free(rew.p);
        return;
      }
      free(rew.p);
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
    if (rty && sp_streq(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        if ((sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0) { buf_puts(b, "0"); return; }
        if (sp_streq(name, "empty?") && argc == 0) { buf_puts(b, "1"); return; }
        if ((sp_streq(name, "first") || sp_streq(name, "last") ||
             sp_streq(name, "min") || sp_streq(name, "max") ||
             sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) { buf_puts(b, "SP_INT_NIL"); return; }
        if (sp_streq(name, "sample") && argc == 0) { buf_puts(b, "0"); return; }
        if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) { buf_puts(b, "\"[]\""); return; }
        if ((sp_streq(name, "join") || sp_streq(name, "pack")) && argc <= 1) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
        if ((sp_streq(name, "union")) && argc == 0) { buf_puts(b, "sp_IntArray_new()"); return; }
        if ((sp_streq(name, "flatten") || sp_streq(name, "compact") || sp_streq(name, "uniq") ||
             sp_streq(name, "sort") || sp_streq(name, "reverse") || sp_streq(name, "dup") ||
             sp_streq(name, "clone") || sp_streq(name, "to_a")) && argc <= 1) {
          buf_puts(b, "sp_PolyArray_new()"); return;
        }
      }
    }
  }

  /* respond_to?(:m): compile-time approximation. A universal method set is
     always true; otherwise consult the receiver's class / class-method chain.
     Unknown primitive methods answer conservatively false. */
  if (sp_streq(name, "respond_to?") && recv >= 0 && argc >= 1) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) qm = nt_str(nt, argv[0], "unescaped");
    if (qm) {
      /* respond_to?(sym, include_all=false): by default only public methods
         answer true; a literal `true` 2nd arg includes private+protected. A
         non-literal 2nd arg can't be folded, so a private/protected match is
         left unresolved rather than guessed (a public match is true either way). */
      int include_all = 0, foldable = 1;
      if (argc >= 2) {
        const char *a1 = nt_type(nt, argv[1]);
        if (a1 && sp_streq(a1, "TrueNode")) include_all = 1;
        else if (a1 && sp_streq(a1, "FalseNode")) include_all = 0;
        else foldable = 0;
      }
      static const char *const uni[] = {
        "to_s", "inspect", "class", "nil?", "dup", "clone", "freeze",
        "frozen?", "hash", "==", "!=", "equal?", "eql?", "object_id",
        "respond_to?", "is_a?", "kind_of?", "instance_of?", "itself",
        "tap", "then", "send", "===", NULL };
      int yes = 0, resolved = 0;
      for (int u = 0; uni[u]; u++) if (sp_streq(qm, uni[u])) { yes = resolved = 1; break; }
      if (!resolved) {
        const char *rty = nt_type(nt, recv);
        if (rty && sp_streq(rty, "ConstantReadNode")) {
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
              if (dt && sp_streq(dt, "ModuleNode")) yes = comp_method_in_chain(c, ci, qm, NULL) >= 0;
            }
            /* builtin Class/Module methods every class object inherits */
            if (!yes) {
              static const char *const cls_uni[] = {
                "name", "instance_methods", "public_instance_methods",
                "private_instance_methods", "protected_instance_methods",
                "instance_method", "method_defined?", "superclass", "ancestors",
                "include?", "const_get", "const_set", "const_defined?",
                "define_method", "allocate", "<", "<=", ">", ">=", NULL };
              for (int u = 0; cls_uni[u]; u++) if (sp_streq(qm, cls_uni[u])) { yes = 1; break; }
              /* `new`: a class responds, a module does not */
              if (!yes && sp_streq(qm, "new")) {
                int dn = c->classes[ci].def_node;
                const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
                yes = !(dt && sp_streq(dt, "ModuleNode"));
              }
            }
          }
        }
        else if (ty_is_object(rt)) {
          int cid = ty_object_class(rt);
          /* a writer query (`m=`) consults the writer table under its base name */
          size_t ql = strlen(qm);
          int is_wr = ql > 0 && qm[ql - 1] == '=';
          char wbase[256]; wbase[0] = '\0';
          if (is_wr && ql - 1 < sizeof wbase) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
          int found = comp_method_in_chain(c, cid, qm, NULL) >= 0 ||
                      comp_reader_in_chain(c, cid, qm, NULL) ||
                      (is_wr && comp_writer_in_chain(c, cid, wbase, NULL));
          if (!found) { resolved = 1; yes = 0; }
          else {
            int v = comp_method_vis_in_chain(c, cid, qm);
            if (v == SP_VIS_PUBLIC) { resolved = 1; yes = 1; }       /* public: always */
            else if (foldable) { resolved = 1; yes = include_all; }  /* private/protected */
            /* else: private/protected + runtime include_all -> unresolved */
          }
        }
        /* a primitive/poly/unknown receiver with a non-universal method: we
           lack a per-type method table, so leave it to the fall-through
           rather than answer a possibly-wrong false. */
      }
      if (resolved) { buf_printf(b, "%d", yes); return; }
    }
  }

  /* Class.{,public_,private_,protected_}method_defined?(:m[, inherit]):
     compile-time decided from the class's recorded method table (instance
     methods + attr readers/writers) filtered by visibility. `method_defined?`
     matches public+protected (not private); the prefixed forms match exactly
     one visibility. inherit=false restricts the lookup to own definitions. */
  int md_pub = 0, md_prot = 0, md_priv = 0, md_family = 1;
  if (sp_streq(name, "method_defined?"))              { md_pub = 1; md_prot = 1; }
  else if (sp_streq(name, "public_method_defined?"))    { md_pub = 1; }
  else if (sp_streq(name, "protected_method_defined?")) { md_prot = 1; }
  else if (sp_streq(name, "private_method_defined?"))   { md_priv = 1; }
  else md_family = 0;
  if (md_family && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (qm && ci >= 0) {
      int inherit = 1;
      if (argc >= 2) {
        const char *it = nt_type(nt, argv[1]);
        if (it && sp_streq(it, "FalseNode")) inherit = 0;
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
      int found;
      if (inherit) {
        found = mi >= 0 || comp_reader_in_chain(c, ci, qm, NULL) ||
                (is_setter && comp_writer_in_chain(c, ci, base, NULL));
      }
      else {
        /* attr readers/writers are flattened into descendants at analyze
           time, so "own" means present here but not in the parent chain */
        int rd_own = comp_is_reader(&c->classes[ci], qm) &&
                     (parent < 0 || !comp_reader_in_chain(c, parent, qm, NULL));
        int wr_own = is_setter && comp_is_writer(&c->classes[ci], base) &&
                     (parent < 0 || !comp_writer_in_chain(c, parent, base, NULL));
        found = (mi >= 0 && mc == ci) || rd_own || wr_own;
      }
      int yes = 0;
      if (found) {
        int v = inherit ? comp_method_vis_in_chain(c, ci, qm)
                        : comp_method_vis(&c->classes[ci], qm);
        yes = (v == SP_VIS_PUBLIC && md_pub) || (v == SP_VIS_PROTECTED && md_prot) ||
              (v == SP_VIS_PRIVATE && md_priv);
      }
      buf_printf(b, "%d", yes);
      return;
    }
  }

  /* The fully dynamic form (class held in a variable, or a non-literal method
     name) cannot be answered ahead of time: there is no runtime reflection
     table, and builtin classes have no enumerable method set. Emit a specific
     diagnostic rather than a generic unsupported-call node dump. Covers both an
     explicit receiver and an implicit-self call (recv < 0). */
  if (md_family) {
    unsupported(c, id, "method_defined? (needs a compile-time-known class and literal method name)");
    return;
  }

  /* Class.const_get(:K) with a literal name: constants live in a flat namespace
     (cst_<name>), so resolve it like a ConstantRead. A literal name that does not
     resolve raises NameError at runtime, matching CRuby: "uninitialized constant
     <Name>" for a valid constant name, "wrong constant name <name>" for one that
     is not (no leading uppercase). A dynamic name can't be resolved ahead of time
     and is diagnosed. */
  if (sp_streq(name, "const_get") && recv >= 0 && argc >= 1) {
    const char *cg_aty = nt_type(nt, argv[0]);
    const char *cg_qm = NULL;
    if (cg_aty && sp_streq(cg_aty, "SymbolNode")) cg_qm = nt_str(nt, argv[0], "value");
    else if (cg_aty && sp_streq(cg_aty, "StringNode")) cg_qm = nt_str(nt, argv[0], "content");
    if (cg_qm) {
      LocalVar *cv = comp_const(c, cg_qm);
      if (cv && cv->type != TY_UNKNOWN) { buf_printf(b, "cst_%s", cg_qm); return; }
      /* literal but unresolved: evaluate the receiver for side effects, then raise.
         CRuby qualifies "uninitialized constant" by a named module receiver
         (M::Missing) but not by Object/top-level; "wrong constant name" is never
         qualified. Qualify when the receiver is a constant other than Object. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_raise_cls(\"NameError\", ");
      if (cg_qm[0] >= 'A' && cg_qm[0] <= 'Z') {
        /* Qualify by the receiver's full Ruby name when it resolves to a known
           class/module (M, or nested M::N); a builtin like Object resolves to no
           user-class index and stays unqualified, matching CRuby. */
        const char *rcv_ty = nt_type(nt, recv);
        const char *rcv_nm = (rcv_ty && (sp_streq(rcv_ty, "ConstantReadNode") ||
                                         sp_streq(rcv_ty, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
        int rcid = rcv_nm ? comp_class_index(c, rcv_nm) : -1;
        if (rcid >= 0) {
          const char *qn = class_ruby_name(c, rcid); if (!qn) qn = c->classes[rcid].name;
          buf_printf(b, "\"uninitialized constant %s::%s\"", qn, cg_qm);
        }
        else {
          buf_printf(b, "\"uninitialized constant %s\"", cg_qm);
        }
      }
      else {
        buf_printf(b, "\"wrong constant name %s\"", cg_qm);
      }
      buf_puts(b, "), sp_box_nil())");
      return;
    }
    unsupported(c, id, "const_get (needs a compile-time-known constant name)");
    return;
  }

  /* Class.const_defined?(:K): compile-time presence check. Constants are
     recorded in a flat namespace, so this consults the global const and class
     tables rather than the receiver's own constants. */
  if (sp_streq(name, "const_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    if (qm) {
      int yes = comp_const(c, qm) != NULL || comp_class_index(c, qm) >= 0;
      buf_printf(b, "%d", yes);
      return;
    }
  }

  if ((sp_streq(name, "-@") || sp_streq(name, "+@")) && recv >= 0 && argc == 0 && !ty_is_object(rt)) {
    if (rt == TY_POLY) {
      if (name[0] == '-') { buf_puts(b, "sp_poly_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { emit_expr(c, recv, b); }  /* +@ is identity on poly */
    }
    else if (rt == TY_STRING) {
      /* +str returns a mutable copy (so subsequent <</concat/upcase! mutate a
         fresh string); -str returns the (already-immutable) string itself. */
      if (name[0] == '+') { buf_puts(b, "sp_str_dup_external("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
    }
    else { buf_puts(b, name[0] == '-' ? "(-" : "(+"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  /* poly `<<` in expression position: sp_poly_shl dispatches on the runtime tag
     (Integer#<< shift -> boxed int, Array#push append -> the array) and returns
     a poly either way, matching the statement-level path. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "<<") && argc == 1) {
    buf_puts(b, "sp_poly_shl("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* unary bitwise complement: ~int -> (~x); ~poly -> coerce to int first */
  if (sp_streq(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY)) {
    if (rt == TY_POLY) { buf_puts(b, "(~sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    else { buf_puts(b, "(~"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  /* poly numeric predicates: coerce the poly value to int and test. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "even?") || sp_streq(name, "odd?") || sp_streq(name, "zero?") ||
       sp_streq(name, "positive?") || sp_streq(name, "negative?"))) {
    int t = ++g_tmp;
    buf_printf(b, "({ mrb_int _t%d = sp_poly_to_i(", t); emit_expr(c, recv, b); buf_puts(b, "); ");
    if (sp_streq(name, "even?")) buf_printf(b, "(_t%d %% 2 == 0); })", t);
    else if (sp_streq(name, "odd?")) buf_printf(b, "(_t%d %% 2 != 0); })", t);
    else if (sp_streq(name, "zero?")) buf_printf(b, "(_t%d == 0); })", t);
    else if (sp_streq(name, "positive?")) buf_printf(b, "(_t%d > 0); })", t);
    else buf_printf(b, "(_t%d < 0); })", t);
    return;
  }
  if (sp_streq(name, "!") && recv >= 0 && argc == 0) {
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
      !(rt == TY_STRING && (sp_streq(name, "+") || sp_streq(name, "*"))) &&
      !((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "*"))) {
    const char *pfn = NULL;
    if (sp_streq(name, "+")) pfn = "sp_poly_add";
    else if (sp_streq(name, "-")) pfn = "sp_poly_sub";
    else if (sp_streq(name, "*")) pfn = "sp_poly_mul";
    else if (sp_streq(name, "/")) pfn = "sp_poly_div";
    else if (sp_streq(name, "%")) pfn = "sp_poly_mod";
    else if (sp_streq(name, "**")) pfn = "sp_poly_pow";
    if (pfn) {
      buf_printf(b, "%s(", pfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *cfn = NULL;
    if (sp_streq(name, "<")) cfn = "sp_poly_lt";
    else if (sp_streq(name, ">")) cfn = "sp_poly_gt";
    else if (sp_streq(name, "<=")) cfn = "sp_poly_le";
    else if (sp_streq(name, ">=")) cfn = "sp_poly_ge";
    if (cfn) {
      buf_printf(b, "%s(", cfn); emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Array#* (join): arr * sep_str  ->  elements joined by separator string. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) &&
      comp_ntype(c, argv[0]) == TY_STRING) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    if (!k) k = "Str";
    buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  if (emit_array_arith_call(c, id, b)) return;

  /* a literal `<<` whose result overflowed int64 (`1 << 64`): the node is typed
     bigint, but the int receiver would otherwise emit a UB C `1LL << 64LL`.
     Promote to a bigint shift. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "<<") && rt == TY_INT &&
      comp_ntype(c, id) == TY_BIGINT) {
    buf_puts(b, "sp_bigint_shl(sp_bigint_new_int(");
    emit_expr(c, recv, b);
    buf_puts(b, "), ");
    emit_int_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  /* bitwise ops on a bignum receiver: arbitrary precision via sp_bigint_*.
     &/|/^ take a bigint second operand (an int/poly mask is promoted);
     <</>> take an int64 shift amount. The result stays a bignum -- a masked
     value can still exceed int64 (`bignum & MASK64`). */
  if (recv >= 0 && argc == 1 && rt == TY_BIGINT &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
       sp_streq(name, "<<") || sp_streq(name, ">>"))) {
    TyKind at0 = comp_ntype(c, argv[0]);
    if (sp_streq(name, "<<") || sp_streq(name, ">>")) {
      buf_printf(b, "sp_bigint_%s(", sp_streq(name, "<<") ? "shl" : "shr");
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (at0 == TY_BIGINT) { buf_puts(b, "sp_bigint_to_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
    else {
      const char *fn = sp_streq(name, "&") ? "and" : sp_streq(name, "|") ? "or" : "xor";
      buf_printf(b, "sp_bigint_%s(", fn);
      emit_expr(c, recv, b); buf_puts(b, ", ");
      if (at0 == TY_BIGINT) emit_expr(c, argv[0], b);
      else if (at0 == TY_POLY) { buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")");
    }
    return;
  }

  /* integer bitwise operators. A poly receiver is coerced to int (the matching
     inference types these TY_INT); `<<` on a poly is handled earlier as the
     ambiguous shift/append via sp_poly_shl, so only &,|,^,>> reach here. */
  if (recv >= 0 && argc == 1 &&
      ((rt == TY_INT && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
                         sp_streq(name, "<<") || sp_streq(name, ">>"))) ||
       (rt == TY_POLY && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
                          sp_streq(name, ">>"))))) {
    TyKind at0 = comp_ntype(c, argv[0]);
    buf_puts(b, "(");
    if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else emit_expr(c, recv, b);
    buf_printf(b, " %s ", name);
    if (at0 == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    /* A literal wider than int64 (a 64-bit mask like 0xFFFFFFFFFFFFFFFF) is
       typed as a bigint; the result slot is int, so take its low-64 bit pattern
       (sp_bigint_to_int truncates) -- this is the xorshift/64-bit-mask idiom. */
    else if (at0 == TY_BIGINT) {
      buf_puts(b, "sp_bigint_to_int("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return;
  }

  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) {
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
    /* Array <=> Array: lexicographic element-wise compare, or nil when an
       element pair is incomparable. Covers every builtin array kind via the
       boxed accessor. */
    if (ty_is_array(lrt) && ty_is_array(lat)) {
      int ta = ++g_tmp, tb = ++g_tmp, tk = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
      buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b);
      buf_printf(b, "; mrb_bool _t%d; mrb_int _t%d = sp_poly_arr_cmp(_t%d, _t%d, &_t%d);"
                    " _t%d ? _t%d : SP_INT_NIL; })", tk, tr, ta, tb, tk, tk, tr);
      return;
    }
    /* Poly operands (e.g. `@n <=> other.n` with int ivars widened to poly in
       promote mode): tag-dispatch via sp_poly_cmp rather than falling through
       to the object-receiver path, which would misread a boxed int's payload
       as a user-class pointer and recurse into this same `<=>`. */
    if (lrt == TY_POLY || lat == TY_POLY) {
      int ta = ++g_tmp, tb = ++g_tmp, tk = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
      buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b);
      buf_printf(b, "; mrb_bool _t%d; sp_poly_cmp(_t%d, _t%d, &_t%d); })", tk, ta, tb, tk);
      return;
    }
  }

  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "<") || sp_streq(name, ">") ||
       sp_streq(name, "<=") || sp_streq(name, ">="))) {
    if (rt == TY_BIGINT || comp_ntype(c, argv[0]) == TY_BIGINT) {
      buf_printf(b, "(sp_bigint_cmp(");
      emit_bigint_operand(c, recv, b);
      buf_puts(b, ", ");
      emit_bigint_operand(c, argv[0], b);
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
        /* a `<=>` that can return nil: check it -- incomparable raises the
           Comparable ArgumentError instead of comparing a garbage value */
        if (user_cmp_ret_poly(c, cid4)) {
          int ta = hoist_boxed_rooted(c, recv), tb2 = hoist_boxed_rooted(c, argv[0]);
          buf_printf(b, "(sp_poly_cmp_ck(_t%d, _t%d) %s 0)", ta, tb2, name);
          return;
        }
        char selfptr[64];
        const char *rtyp = nt_type(nt, recv);
        if (rtyp && (sp_streq(rtyp, "LocalVariableReadNode") ||
                     sp_streq(rtyp, "InstanceVariableReadNode") ||
                     sp_streq(rtyp, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t4 = ++g_tmp;
          Buf rb = expr_buf(c, recv);
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
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?")) &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")) {
    /* `[]` and a bare `Array.new` are arrays even when their element type (and
       so the inferred type) is still UNKNOWN -- treat them as such for the fold. */
    TyKind eff_rt = rt;
    if (eff_rt == TY_UNKNOWN) {
      const char *rvt = nt_type(nt, recv);
      if (rvt && sp_streq(rvt, "ArrayNode")) eff_rt = TY_POLY_ARRAY;
      else if (rvt && sp_streq(rvt, "CallNode") && nt_str(nt, recv, "name") &&
               sp_streq(nt_str(nt, recv, "name"), "new")) {
        int rr = nt_ref(nt, recv, "receiver");
        if (rr >= 0 && nt_type(nt, rr) && sp_streq(nt_type(nt, rr), "ConstantReadNode") &&
            nt_str(nt, rr, "name") && sp_streq(nt_str(nt, rr, "name"), "Array")) eff_rt = TY_POLY_ARRAY;
      }
    }
    int yes = ty_matches_class(eff_rt, nt_str(nt, argv[0], "name"), sp_streq(name, "instance_of?"));
    if (yes >= 0) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes); return; }
  }

  /* poly.is_a?(class_var) where the argument is a TY_CLASS typed expression.
     Skip if argv[0] is a ConstantReadNode: the fast-path below handles builtins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?")) &&
      comp_ntype(c, argv[0]) == TY_CLASS &&
      !(nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode"))) {
    int t = ++g_tmp, k = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
    buf_printf(b, "sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
    if (sp_streq(name, "instance_of?"))
      buf_printf(b, "sp_poly_get_class(_t%d).cls_id == _t%d.cls_id; })", t, k);
    else
      buf_printf(b, "sp_poly_is_a(_t%d, _t%d); })", t, k);
    return;
  }

  /* poly.is_a?(Class) / kind_of?: runtime tag/cls_id check */
  if (recv >= 0 && rt == TY_POLY && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
    const char *cty = nt_type(nt, argv[0]);
    const char *cn = cty && sp_streq(cty, "ConstantReadNode") ? nt_str(nt, argv[0], "name") : NULL;
    if (cn) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; ");
      char v[32]; snprintf(v, sizeof v, "_t%d", t);
      if (sp_streq(cn, "Integer") || sp_streq(cn, "Fixnum")) buf_printf(b, "%s.tag == SP_TAG_INT", v);
      else if (sp_streq(cn, "String"))   buf_printf(b, "%s.tag == SP_TAG_STR", v);
      else if (sp_streq(cn, "Float"))    buf_printf(b, "%s.tag == SP_TAG_FLT", v);
      else if (sp_streq(cn, "Symbol"))   buf_printf(b, "%s.tag == SP_TAG_SYM", v);
      else if (sp_streq(cn, "NilClass")) buf_printf(b, "%s.tag == SP_TAG_NIL", v);
      else if (sp_streq(cn, "TrueClass"))  buf_printf(b, "(%s.tag == SP_TAG_BOOL && %s.v.b)", v, v);
      else if (sp_streq(cn, "FalseClass")) buf_printf(b, "(%s.tag == SP_TAG_BOOL && !%s.v.b)", v, v);
      else if (sp_streq(cn, "Numeric"))  buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", v, v);
      else if (sp_streq(cn, "Array"))    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", v, v, v);
      else if (sp_streq(cn, "Hash"))     buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -13 && %s.cls_id >= -20)", v, v, v);
      else if (sp_streq(cn, "Encoding")) buf_printf(b, "%s.tag == SP_TAG_ENCODING", v);
      else {
        int cid = comp_class_index(c, cn);
        int exact = sp_streq(name, "instance_of?");
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
    if (argc == 0 && sp_streq(name, "inspect")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"nil\"))"); return; }
    if (argc == 0 && sp_streq(name, "to_s"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"\"))"); return; }
    if (argc == 0 && sp_streq(name, "nil?"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return; }
    if (argc == 0 && sp_streq(name, "to_i"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)0)"); return; }
    if (argc == 0 && sp_streq(name, "to_f"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0.0)"); return; }
    if (argc == 0 && sp_streq(name, "to_r"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_float)0.0)"); return; }
    if (argc == 0 && sp_streq(name, "to_a"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())"); return; }
    if (argc == 0 && sp_streq(name, "to_h"))    {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_SymPolyHash_new())");
      return;
    }
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      const char *cn = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode") ? nt_str(nt, argv[0], "name") : NULL;
      int yes = cn ? (sp_streq(cn, "NilClass") || sp_streq(name, "instance_of?") ? sp_streq(cn, "NilClass") : (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) : 0;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return;
    }
  }

  if (emit_poly_call(c, id, b)) return;

  /* between?(lo, hi): lo <= self <= hi */
  if (sp_streq(name, "between?") && argc == 2) {
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
      if (mi_b >= 0 && user_cmp_ret_poly(c, cid_b)) {
        /* nil-capable `<=>`: checked comparisons (incomparable raises) */
        int ts = hoist_boxed_rooted(c, recv);
        int tlo = hoist_boxed_rooted(c, argv[0]), thi = hoist_boxed_rooted(c, argv[1]);
        buf_printf(b, "(sp_poly_cmp_ck(_t%d, _t%d) >= 0 && sp_poly_cmp_ck(_t%d, _t%d) <= 0)",
                   ts, tlo, ts, thi);
        return;
      }
      if (mi_b >= 0) {
        int ts = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        const char *cname = c->classes[defcls_b].name;
        /* Compute each RHS into a local buffer first: emit_expr may itself
           hoist temps into g_pre (e.g. an arg `Temp.new(5)` roots its boxed
           int there). Doing that before writing our own `T _tN = ` prefix
           keeps the nested hoist from splitting our declaration line. */
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        Buf lb = expr_buf(c, argv[0]);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", tlo);
        buf_puts(g_pre, lb.p ? lb.p : ""); buf_puts(g_pre, ";\n"); free(lb.p);
        Buf hb = expr_buf(c, argv[1]);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", thi);
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
  if (sp_streq(name, "object_id") && recv >= 0 && argc == 0) {
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
  if (recv >= 0 && rt == TY_INT && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == SP_INT_NIL)");
    return;
  }
  /* nil? on a string: a nullable string carries NULL (e.g. a scan miss) */
  if (recv >= 0 && rt == TY_STRING && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
    return;
  }
  /* nil? on a float: a nullable float carries the NaN sentinel (e.g. first/
     last of an empty float array). A real float is never the sentinel. */
  if (recv >= 0 && rt == TY_FLOAT && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "sp_float_is_nil("); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* nil? on an array/hash: a nil container is a NULL pointer */
  if (recv >= 0 && (ty_is_array(rt) || ty_is_hash(rt)) && sp_streq(name, "nil?") && argc == 0) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return;
  }
  /* nil? on a pointer-backed concrete type: nil is the NULL pointer. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      (rt == TY_FIBER || rt == TY_PROC || rt == TY_CURRY || rt == TY_RANDOM ||
       rt == TY_METHOD || rt == TY_IO || rt == TY_STRINGIO || rt == TY_STRINGSCANNER ||
       rt == TY_MATCHDATA || rt == TY_REGEX || rt == TY_EXCEPTION || rt == TY_BIGINT)) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
    return;
  }
  /* nil? on a value-typed concrete receiver is always false. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      (rt == TY_RANGE || rt == TY_TIME || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_SYMBOL || rt == TY_BOOL || rt == TY_CLASS)) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
    return;
  }
  /* a predicate on an empty array literal folds to a constant: the block (if
     any) never runs, so empty all?/none? are true, any?/one? false */
  if (recv >= 0 && (argc == 0 || argc == 1) &&
      (sp_streq(name, "all?") || sp_streq(name, "any?") ||
       sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "count")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    if (sp_streq(name, "count")) { buf_puts(b, "0"); return; }
    buf_puts(b, (sp_streq(name, "all?") || sp_streq(name, "none?")) ? "1" : "0");
    return;
  }

  /* Class.===(obj): equivalent to obj.is_a?(Class). Receiver is a class constant. */
  if (recv >= 0 && argc == 1 && sp_streq(name, "===") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *cn = nt_str(nt, recv, "name");
    if (cn) {
      TyKind at2 = comp_ntype(c, argv[0]);
      /* TrueClass/FalseClass/NilClass === <literal/typed value>: decide
         statically from the arg's node kind or scalar type. */
      const char *aty = nt_type(nt, argv[0]);
      if (sp_streq(cn, "NilClass") || sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass")) {
        int yn = -1;
        if (sp_streq(cn, "NilClass"))
          yn = (at2 == TY_NIL || (aty && sp_streq(aty, "NilNode"))) ? 1 : (at2 != TY_POLY ? 0 : -1);
        else if (sp_streq(cn, "TrueClass"))
          yn = (aty && sp_streq(aty, "TrueNode")) ? 1 : (aty && sp_streq(aty, "FalseNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
        else
          yn = (aty && sp_streq(aty, "FalseNode")) ? 1 : (aty && sp_streq(aty, "TrueNode")) ? 0 : (at2 != TY_BOOL && at2 != TY_POLY ? 0 : -1);
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
        if (sp_streq(cn, "Integer") || sp_streq(cn, "Fixnum")) buf_printf(b, "%s.tag == SP_TAG_INT", v);
        else if (sp_streq(cn, "String"))   buf_printf(b, "%s.tag == SP_TAG_STR", v);
        else if (sp_streq(cn, "Float"))    buf_printf(b, "%s.tag == SP_TAG_FLT", v);
        else if (sp_streq(cn, "Symbol"))   buf_printf(b, "%s.tag == SP_TAG_SYM", v);
        else if (sp_streq(cn, "NilClass")) buf_printf(b, "%s.tag == SP_TAG_NIL", v);
        else if (sp_streq(cn, "Numeric"))  buf_printf(b, "(%s.tag == SP_TAG_INT || %s.tag == SP_TAG_FLT)", v, v);
        else if (sp_streq(cn, "Array"))    buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id <= -1 && %s.cls_id >= -12)", v, v, v);
        else buf_printf(b, "0");
        buf_puts(b, "; })");
        return;
      }
    }
  }

  if (emit_case_eq_call(c, id, b)) return;

  if (emit_object_call(c, id, b)) return;

  if (emit_value_recv_call(c, id, b)) return;

  /* Array-reduction methods on a boxed array element of a poly array (e.g.
     `runs.map { |r| r.sum }` over chunk_while runs). The runtime helper switches
     on the element's cls_id. Skipped when a user class defines the same method
     (it falls through to the general poly dispatch below). */
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
    else if (sp_streq(name, "value") || sp_streq(name, "resume")) pm = "sp_poly_fiber_value";
    else if (sp_streq(name, "join")) pm = "sp_poly_fiber_join";
    if (pm) {
      /* Attr readers count as user definitions too: `attr_accessor :value`
         must shadow the builtin helper exactly like `def value` does, or the
         reader call is hijacked (e.g. sp_poly_fiber_value on a Node). The
         general poly dispatch below emits reader arms, so it handles them. */
      int ncand = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (comp_method_in_chain(c, k, name, NULL) >= 0 ||
            comp_reader_in_chain(c, k, name, NULL)) ncand++;
      if (ncand == 0) {
        buf_printf(b, "%s(", pm); emit_expr(c, recv, b); buf_puts(b, ")");
        return;
      }
    }
  }

  if (emit_poly_method_dispatch(c, id, b)) return;

  /* string-range literal methods: the int-only sp_Range struct can't hold
     string bounds, so inline strcmp / char-iteration for a literal
     `("a".."z")` receiver. */
  if (recv >= 0 && rt == TY_RANGE && nt_type(nt, unwrap_parens(c, recv)) &&
      sp_streq(nt_type(nt, unwrap_parens(c, recv)), "RangeNode")) {
    int rnode = unwrap_parens(c, recv);
    int lo = nt_ref(nt, rnode, "left"), hi = nt_ref(nt, rnode, "right");
    if (lo >= 0 && hi >= 0 && comp_ntype(c, lo) == TY_STRING && comp_ntype(c, hi) == TY_STRING) {
      int excl = (int)(nt_int(nt, rnode, "flags", 0) & 4) ? 1 : 0;
      if ((sp_streq(name, "include?") || sp_streq(name, "member?") ||
           sp_streq(name, "cover?") || sp_streq(name, "===")) && argc == 1) {
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
      if (sp_streq(name, "to_a") && argc == 0) {
        /* succ-based string range (handles multi-char: "aa".."ac" etc.) */
        buf_puts(b, "sp_StrArray_from_string_range("); emit_expr(c, lo, b);
        buf_puts(b, ", "); emit_expr(c, hi, b); buf_printf(b, ", %d)", excl);
        return;
      }
    }
  }

  if (emit_range_call(c, id, b)) return;

  /* hash value methods */
  /* {}.default (empty hash literal with unknown type) always returns nil */
  if (recv >= 0 && sp_streq(name, "default") && argc == 0 && !ty_is_hash(rt)) {
    buf_puts(b, "sp_box_nil()");
    return;
  }
  if (emit_hash_call(c, id, b)) return;

  /* `arr[i] = v` in expression position: do the store, evaluate to the rhs
     (Ruby []= returns the assigned value). The statement form is emitted
     elsewhere; this covers rvalue chains like `b = arr[i] = v`. */
  /* a[i, n] = src  —  slice assignment */
  /* arr[start, len] = rhs : a splice (remove `len` at `start`, insert rhs). */
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 3) {
    emit_array_splice(c, id, recv, rt, argv[0], argv[1], -1, argv[2], b);
    return;
  }
  if (recv >= 0 && ty_is_array(rt) && sp_streq(name, "[]=") && argc == 2) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    /* arr[range] = rhs : a splice over the range's (start, length). */
    if (k && comp_ntype(c, argv[0]) == TY_RANGE) {
      emit_array_splice(c, id, recv, rt, -1, -1, argv[0], argv[1], b);
      return;
    }
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
  if (recv >= 0 && rt == TY_UNKNOWN && sp_streq(name, "sum") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
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
      (sp_streq(name, "take_while") || sp_streq(name, "drop_while") || sp_streq(name, "each_index") ||
       sp_streq(name, "difference") || sp_streq(name, "-") || sp_streq(name, "&") || sp_streq(name, "|") ||
       sp_streq(name, "intersection") || sp_streq(name, "union") || sp_streq(name, "+") ||
       sp_streq(name, "zip") || sp_streq(name, "flatten") || sp_streq(name, "compact") ||
       sp_streq(name, "uniq") || sp_streq(name, "sort") || sp_streq(name, "reverse") ||
       sp_streq(name, "shuffle")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) {
      if (sp_streq(name, "each_index")) {
        /* each_index on [] is a no-op; evaluate receiver for side-effects */
        emit_expr(c, recv, b);
      }
      else if (sp_streq(name, "take_while") || sp_streq(name, "drop_while")) {
        buf_puts(b, "sp_PolyArray_new()");
      }
      else {
        /* set/transform ops on [] receiver: call the runtime with NULL first arg */
        TyKind akt = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
        const char *ek = ty_is_array(akt) ? ((akt == TY_POLY_ARRAY) ? "Poly" : array_kind(akt)) : NULL;
        if (!ek) ek = "Poly";
        if (argc > 0 && akt != TY_UNKNOWN &&
            (sp_streq(name, "union") || sp_streq(name, "|") ||
             sp_streq(name, "difference") || sp_streq(name, "-") ||
             sp_streq(name, "intersection") || sp_streq(name, "&") ||
             sp_streq(name, "+") || sp_streq(name, "zip"))) {
          /* call the real function with NULL receiver (handles empty-self case) */
          const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect"
                         : (sp_streq(name, "|") || sp_streq(name, "union")) ? "union"
                         : (sp_streq(name, "+")) ? "concat"
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
  if (emit_array_call(c, id, b)) return;

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (sp_streq(name, "to_s") || sp_streq(name, "id2name") || sp_streq(name, "name")) {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "inspect")) {
      buf_puts(b, "sp_sym_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "to_sym") || sp_streq(name, "itself")) { emit_expr(c, recv, b); return; }
    /* case-folding methods return a (re-interned) symbol */
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase")) {
      buf_printf(b, "sp_sym_intern(sp_str_%s(sp_sym_to_s(", name); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (sp_streq(name, "length") || sp_streq(name, "size")) {
      buf_puts(b, "((mrb_int)strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (sp_streq(name, "empty?")) {
      buf_puts(b, "(strlen(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")) == 0)");
      return;
    }
    if (sp_streq(name, "==") || sp_streq(name, "!=")) {
      buf_puts(b, name[0] == '=' ? "(" : "(!(");
      emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b);
      buf_puts(b, name[0] == '=' ? ")" : "))");
      return;
    }
    /* case-insensitive compare over the symbols' names; a symbol argument only
       (a non-symbol arg yields nil in Ruby and is left to the unsupported path) */
    if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
        comp_ntype(c, argv[0]) == TY_SYMBOL) {
      int q = sp_streq(name, "casecmp?");
      if (q) buf_puts(b, "(");
      buf_puts(b, "sp_str_casecmp(sp_sym_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      if (q) buf_puts(b, " == 0)");
      return;
    }
    /* string-surface methods over the symbol's name; succ re-interns a symbol,
       index/slice yield a substring (or nil), the predicates yield a bool. */
    if (sp_streq(name, "succ") || sp_streq(name, "next")) {
      buf_puts(b, "sp_sym_intern(sp_str_succ(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 &&
        nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
      /* :s[a..b] / :s[a...b] over the name; a beginless/endless bound is 0 /
         the name length. The name is materialized once to avoid re-evaluating
         the receiver for an endless range. */
      int rn = argv[0];
      int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
      int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
      int t = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = sp_sym_to_s(", t); emit_expr(c, recv, b);
      buf_printf(b, "); sp_str_sub_range_r(_t%d, ", t);
      if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
      buf_puts(b, ", ");
      if (hi >= 0) { emit_int_expr(c, hi, b); buf_printf(b, ", %d); })", excl); }
      else buf_printf(b, "(mrb_int)sp_str_length(_t%d), 0); })", t);
      return;
    }
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY)) {
      buf_puts(b, "sp_str_char_at_or_nil(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "), ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2) {
      buf_puts(b, "sp_str_sub_range(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "), ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if ((sp_streq(name, "start_with?") || sp_streq(name, "end_with?")) && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_POLY)) {
      buf_printf(b, "sp_str_%s(sp_sym_to_s(", sp_streq(name, "start_with?") ? "start_with" : "end_with");
      emit_expr(c, recv, b); buf_puts(b, "), "); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "match?") && argc == 1) {
      int rre = re_lit_index(c, argv[0]);
      if (rre >= 0) {
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, sp_sym_to_s(", rre); emit_expr(c, recv, b);
        buf_puts(b, ")) >= 0)");
        return;
      }
    }
  }

  /* boolean receiver methods */
  if (recv >= 0 && rt == TY_BOOL) {
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? SPL(\"true\") : SPL(\"false\"))");
      return;
    }
    if (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", name); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* str.each_char / each_line / chars / lines / bytes / codepoints { |x| ... } -> iterate, return self. */
  if (recv >= 0 && rt == TY_STRING && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "each_byte") ||
       sp_streq(name, "chars") || sp_streq(name, "lines") || sp_streq(name, "bytes") || sp_streq(name, "codepoints"))) {
    int block = nt_ref(nt, id, "block");
    int body = nt_ref(nt, block, "body");
    const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
    int ts = ++g_tmp, ti = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    int is_line = sp_streq(name, "each_line") || sp_streq(name, "lines");
    int is_byte = sp_streq(name, "each_byte") || sp_streq(name, "bytes") || sp_streq(name, "codepoints");
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
      if (argc == 1 && argv && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int cv = struct_kwarg_value(c, argv[0], "chomp");
        eline_chomp = (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "TrueNode"));
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

  if (emit_scalar_call(c, id, b)) return;

  /* bigint methods */
  if (recv >= 0 && rt == TY_BIGINT) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    if ((sp_streq(name, "to_s") || sp_streq(name, "inspect")) && argc == 0) {
      buf_printf(b, "sp_bigint_to_s(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "to_i") && argc == 0) {
      buf_printf(b, "sp_bigint_to_int(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "to_f") && argc == 0) {
      buf_printf(b, "((mrb_float)sp_bigint_to_int(%s))", r); free(rs.p); return;
    }
    free(rs.p);
  }

  /* Fiber[:k] = v (expression form) */
  if (sp_streq(name, "[]=") && argc == 2 && recv >= 0) {
    int is_fiber2 = 0;
    const char *rty3 = nt_type(nt, recv);
    if (rty3 && sp_streq(rty3, "ConstantReadNode")) {
      const char *rn3 = nt_str(nt, recv, "name");
      if (rn3 && sp_streq(rn3, "Fiber")) is_fiber2 = 1;
    }
    else if (rty3 && sp_streq(rty3, "CallNode")) {
      const char *rn3 = nt_str(nt, recv, "name");
      int rr3 = nt_ref(nt, recv, "receiver");
      if (rn3 && sp_streq(rn3, "current") && rr3 >= 0) {
        const char *rrty3 = nt_type(nt, rr3);
        const char *rrn3 = nt_str(nt, rr3, "name");
        if (rrty3 && sp_streq(rrty3, "ConstantReadNode") && rrn3 && sp_streq(rrn3, "Fiber"))
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
  if (sp_streq(name, "[]=") && argc == 2 && recv >= 0) {
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
        /* For poly hashes with scalar values, store the scalar and box it for the hash call.
           A nil/void rhs (`return @cache[k] = nil`) has no C storage type --
           emit_ctype would print `void` -- so hold it boxed. */
        TyKind vt_eff = (vt == TY_NIL || vt == TY_VOID) ? TY_POLY : vt;
        TyKind decl_type = unbox_poly_val ? hvt
                         : (is_poly_hash && vt_eff != TY_UNKNOWN && vt_eff != TY_POLY) ? vt_eff
                         : (vt_eff != TY_UNKNOWN ? vt_eff : TY_POLY);
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
        if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b);
        else emit_hash_key(c, argv[0], ty_hash_key(rt), b);  /* unbox a poly key to the hash's key type */
        buf_puts(b, ", ");
        char tvn[32]; snprintf(tvn, sizeof tvn, "_t%d", tv);
        if (is_poly_hash && decl_type != TY_POLY) {
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
        return;
      }
      if (sp_streq(name, "flush")) { buf_printf(b, "fflush(%s)", fd); return; }
      if (sp_streq(name, "write") || sp_streq(name, "syswrite")) {
        /* IO#write: write each arg (stringified), return total bytes written. */
        buf_puts(b, "({ mrb_int _w = 0; ");
        for (int k = 0; k < argc; k++) {
          buf_puts(b, "{ const char *_s = ");
          if (comp_ntype(c, argv[k]) == TY_STRING) emit_expr(c, argv[k], b);
          else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[k], b); buf_puts(b, ")"); }
          buf_printf(b, "; _w += _s ? (mrb_int)fwrite(_s, 1, strlen(_s), %s) : 0; } ", fd);
        }
        buf_puts(b, "_w; })");
        return;
      }
    }
  }
  /* Last-resort fallbacks for inspect/to_s on unresolved receivers.
     The test array_unresolved_inspect_no_segv expects "[]" when an
     unsupported method chains into inspect. Emit a safe nil-degrade
     rather than aborting the compiler. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "inspect")) {
    buf_puts(b, "\"[]\""); return;
  }
  if (recv >= 0 && argc == 0 && sp_streq(name, "to_s")) {
    buf_puts(b, "\"\""); return;
  }
  /* nil? on an object type: a value-type object is never nil; a heap object
     reference is nil exactly when its pointer is NULL. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") && ty_is_object(rt)) {
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
  if (sp_streq(name, "synchronize") && nt_ref(nt, id, "block") >= 0) {
    int blk = nt_ref(nt, id, "block");
    int bdy = nt_ref(nt, blk, "body");
    int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
    TyKind res = comp_ntype(c, id);
    int scalar = is_scalar_ret(res) && res != TY_VOID && res != TY_NIL && res != TY_UNKNOWN;
    int rv = ++g_tmp;
    /* A real Mutex#synchronize takes the lock around the block and releases it
       with ensure semantics: the unlock runs on normal completion, on an
       exception in the block (then re-raised), and on a non-local unwind passing
       through it (proc-return / throw, then resumed). A Monitor/other receiver
       keeps the inline no-op behaviour. (A bare `return` -- a C return out of the
       inlined body -- is not yet covered; it would need deferred-return plumbing
       like begin..ensure.) */
    /* Full ensure semantics for a Mutex receiver: the unlock runs on normal
       completion, on a `return` out of the block (deferred via the begin..ensure
       g_ensure_stack mechanism), on an exception (then re-raised), and on a
       non-local unwind passing through (proc-return / throw, then resumed). The
       eid names the deferred-return/exception slots that emit_return targets. */
    int is_mx = recv >= 0 && comp_ntype(c, recv) == TY_MUTEX && g_ensure_depth < MAX_ENSURE_DEPTH;
    int mtmp = 0, eid = 0, has_retval = 0;
    buf_puts(b, "({ ");
    /* result temp is declared before the setjmp so it survives the block scope;
       the body assigns into it. */
    if (scalar) { emit_ctype(c, res, b); buf_printf(b, " _t%d = %s; ", rv, default_value(res)); }
    if (is_mx) {
      mtmp = ++g_tmp; eid = ++g_tmp;
      has_retval = (g_ret_type != TY_VOID && g_ret_type != TY_UNKNOWN);
      buf_printf(b, "sp_mutex *_t%d = ", mtmp); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Mutex_lock(_t%d); ", mtmp);
      buf_printf(b, "int _retf%d = 0; int _excf%d = 0; const char *_excmsg%d = NULL, *_exccls%d = NULL; ",
                 eid, eid, eid, eid);
      if (has_retval) { emit_ctype(c, g_ret_type, b); buf_printf(b, " _retv%d = %s; ", eid, default_value(g_ret_type)); }
      g_ensure_stack[g_ensure_depth++] = (EnsureCtx){ eid, has_retval, g_exc_frame_depth };
      buf_puts(b, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; ");
      buf_puts(b, "sp_exc_top++; if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) { ");
      g_exc_frame_depth++;
    }
    for (int k = 0; k < bbn - 1; k++) emit_stmt(c, bbb[k], b, 0);
    if (bbn > 0) {
      TyKind lty = comp_ntype(c, bbb[bbn-1]);
      const char *lnty = nt_type(nt, bbb[bbn-1]);
      int nil_lit = (lty == TY_NIL && lnty && sp_streq(lnty, "NilNode"));
      int can_expr = (lty != TY_VOID && lty != TY_UNKNOWN && (lty != TY_NIL || nil_lit));
      if (scalar && can_expr) {
        buf_printf(b, "_t%d = ", rv);
        if (res == TY_POLY && lty != TY_POLY) emit_boxed(c, bbb[bbn-1], b);
        else emit_expr(c, bbb[bbn-1], b);
        buf_puts(b, "; ");
      }
      else {
        emit_stmt(c, bbb[bbn-1], b, 0);  /* scalar default already set at rv decl */
      }
    }
    if (is_mx) {
      g_ensure_depth--;
      g_exc_frame_depth--;
      buf_printf(b, "sp_exc_top--; } else { sp_exc_top--; sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; if (sp_unwind_kind == SP_UNWIND_NONE) { sp_proc_homes_unwind(); _excf%d = 1; _excmsg%d = sp_exc_msg[sp_exc_top]; _exccls%d = sp_exc_cls[sp_exc_top]; } } ",
                 eid, eid, eid);
      buf_printf(b, "_ensure%d: ; sp_Mutex_unlock(_t%d); ", eid, mtmp);
      buf_puts(b, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume(); ");
      if (g_ensure_depth > 0) {
        /* nested inside another begin..ensure / synchronize: hand the deferred
           return and unhandled exception to the enclosing ensure. */
        EnsureCtx *outer = &g_ensure_stack[g_ensure_depth - 1];
        if (has_retval && outer->has_retval)
          buf_printf(b, "if (_retf%d) { _retv%d = _retv%d; _retf%d = 1; sp_exc_top--; goto _ensure%d; } ",
                     eid, outer->lid, eid, outer->lid, outer->lid);
        else
          buf_printf(b, "if (_retf%d) { _retf%d = 1; sp_exc_top--; goto _ensure%d; } ", eid, outer->lid, outer->lid);
        buf_printf(b, "if (_excf%d) { _excf%d = 1; _excmsg%d = _excmsg%d; _exccls%d = _exccls%d; sp_exc_top--; goto _ensure%d; } ",
                   eid, outer->lid, outer->lid, eid, outer->lid, eid, outer->lid);
      }
      else {
        /* the deferred return leaves through every enclosing live begin
           frame: pop them (see the begin..ensure epilogue in codegen_stmt.c) */
        if (g_exc_frame_depth > 0)
          buf_printf(b, "if (_retf%d) sp_exc_top -= %d; ", eid, g_exc_frame_depth);
        if (has_retval) buf_printf(b, "if (_retf%d) return _retv%d; ", eid, eid);
        else if (g_ret_type == TY_POLY) buf_printf(b, "if (_retf%d) return sp_box_nil(); ", eid);
        else if (g_ret_type == TY_UNKNOWN) buf_printf(b, "if (_retf%d) return 0; ", eid);
        else buf_printf(b, "if (_retf%d) return; ", eid);
        buf_printf(b, "if (_excf%d) sp_raise_cls(_exccls%d, _excmsg%d); ", eid, eid, eid);
      }
    }
    if (scalar) buf_printf(b, "_t%d; })", rv);
    else buf_puts(b, "0; })");
    return;
  }

  /* (range).lazy[.select/reject/filter{blk}].first(n) -- lower to a C for-loop */
  if (sp_streq(name, "first") && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    const char *rname0 = nt_str(nt, recv, "name");
    int lazy_nid = -1;
    int filter_block = -1;
    int filter_negate = 0;
    if (rname0 && sp_streq(rname0, "lazy") && nt_ref(nt, recv, "block") < 0) {
      lazy_nid = recv;
    }
    else if (rname0 && (sp_streq(rname0, "select") || sp_streq(rname0, "reject") || sp_streq(rname0, "filter"))) {
      filter_block = nt_ref(nt, recv, "block");
      if (filter_block >= 0) {
        filter_negate = sp_streq(rname0, "reject") ? 1 : 0;
        int inner = nt_ref(nt, recv, "receiver");
        if (inner >= 0 && nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "CallNode")) {
          const char *iname = nt_str(nt, inner, "name");
          if (iname && sp_streq(iname, "lazy") && nt_ref(nt, inner, "block") < 0)
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
        if (!endless && nt_type(nt, right) && sp_streq(nt_type(nt, right), "NilNode")) endless = 1;
        if (!endless && nt_type(nt, right) && sp_streq(nt_type(nt, right), "ConstantPathNode")) {
          const char *cpnm = nt_str(nt, right, "name");
          if (cpnm && sp_streq(cpnm, "INFINITY")) {
            int par = nt_ref(nt, right, "parent");
            const char *parnm = (par >= 0 && nt_type(nt, par) &&
                                 sp_streq(nt_type(nt, par), "ConstantReadNode"))
                                ? nt_str(nt, par, "name") : NULL;
            if (parnm && sp_streq(parnm, "Float")) endless = 1;
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
    /* compare_by_identity? on a poly-carried value resolves here, not at the
       gate: every spinel hash is value-keyed (the mutating variant is a
       compile error), so a hash answers false and anything else raises
       CRuby's NoMethodError -- accurate in both gate modes. */
    if ((grt == TY_POLY || grt == TY_UNKNOWN) &&
        nt_str(nt, id, "name") && sp_streq(nt_str(nt, id, "name"), "compare_by_identity?")) {
      buf_puts(b, "sp_poly_cbi_p(");
      emit_boxed(c, recv, b);
      buf_puts(b, ")");
      return;
    }
    if (grt == TY_POLY || grt == TY_NIL || grt == TY_INT || grt == TY_UNKNOWN ||
        grt == TY_STRING) {
      TyKind ret = comp_ntype(c, id);
      /* An unresolved call raises NoMethodError by default, matching CRuby
         (a dead poly-dispatch arm still emits nothing; a live one raising here
         is exactly what CRuby would do). SPINEL_WARN_UNRESOLVED lists every
         such site at compile time for auditing a port; SPINEL_GATE_RAISE=0 is
         the transition escape hatch back to the old silent typed default. The
         coercion paths the raise value flows through (return slots, string/int
         receiver+arg slots, ...) recognize the sp_raise_nomethod token and
         keep the side-effect -- see the staged groundwork notes below. */
      const char *nm = nt_str(nt, id, "name");
      if (warn_unresolved_pos(c, id)) {
        fprintf(stderr, "unresolved call '%s' on %s receiver -> %s\n",
                nm ? nm : "?", ty_name(grt),
                g_gate_raise ? "NoMethodError (matching CRuby)" : "nil (CRuby would raise NoMethodError)");
      }
      const char *dflt = (is_scalar_ret(ret) && ret != TY_UNKNOWN) ? default_value(ret) : "sp_box_nil()";
      if (g_gate_raise) {
        /* Scalar slot: the comma-expr yields a typed default the surrounding C
           accepts directly. Poly slot: emit the recognizable sp_raise_nomethod
           token (returns sp_RbVal) so coercion sites keep the raise side-effect
           rather than text-discarding a bare sp_box_nil(). Both diverge before
           the value is used. */
        if (sp_streq(dflt, "sp_box_nil()"))
          buf_printf(b, "sp_raise_nomethod(\"undefined method '%s' for %s\")", nm ? nm : "?", ty_name(grt));
        else
          buf_printf(b, "(sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for %s\"), %s)",
                     nm ? nm : "?", ty_name(grt), dflt);
        return;
      }
      buf_puts(b, dflt);
      return;
    }
  }
  unsupported(c, id, "call");
}
