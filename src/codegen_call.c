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
  /* A block, when present, must be a literal BlockNode (a forwarded &proc is a
     different shape handled elsewhere). block < 0 is the no-block construction:
     still inline the yielding initialize body so its ivar writes run -- the
     emitted constructor skips a yielding initialize, so without this the body
     (including @ivar = ...) would vanish. With g_block_id < 0 a `yield` is dead
     code and `block_given?` folds to false, matching a blockless `new`. */
  if (block >= 0 && (!nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode"))) return 0;
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
  const char *saved_self_deref = g_self_deref;
  int saved_emcls = g_emitting_class_id;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  int is_val = c->classes[ci].is_value_type;
  /* stack-local: this ctor inliner recurses (a constructor's block may itself
     construct), and g_self points into this buffer -- a shared static would be
     clobbered by the nested inline. */
  char selfbuf[64];
  g_yield_block_fallback = saved_block;
  /* the block being captured is caller code: record the caller's self so
     emit_block_invoke can restore it around the spliced block body. Aliasing
     g_self by pointer is safe now that selfbuf is stack-local: it names an
     ancestor frame's selfbuf, which stays live and unmodified for the whole
     nested emission (a frame only ever writes its own selfbuf). */
  const char *saved_self_fb = g_yield_self_fallback;
  const char *saved_deref_fb = g_yield_self_deref_fallback;
  int saved_emcls_fb = g_yield_emitting_class_fallback;
  g_yield_self_fallback = g_self;
  g_yield_self_deref_fallback = g_self_deref;
  g_yield_emitting_class_fallback = g_emitting_class_id;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  int st = ++g_tmp;
  buf_puts(b, "({\n");
  emit_indent(b, g_indent + 1);
  /* A value-type class returns sp_X by value from sp_X_new; a heap class returns
     sp_X *. The inlined body reaches its ivars through g_self + g_self_deref, so
     match the deref to the storage: "." for a value-type local, "->" for a
     pointer. */
  buf_printf(b, "sp_%s %s_t%d = sp_%s_new(", c->classes[ci].c_name, is_val ? "" : "*", st, c->classes[ci].c_name);
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  buf_puts(b, ");\n");
  snprintf(selfbuf, sizeof selfbuf, "_t%d", st);
  g_self = selfbuf;
  g_self_deref = is_val ? "." : "->";
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
  /* A trailing keyword-hash arg (a Data class's `new(x: .., y: ..)` into a
     keyword-param initialize) binds each keyword param by name, not by
     position -- otherwise the whole hash lands in the first param's slot. */
  const char *last_ty = argc2 > 0 ? nt_type(nt, argv2[argc2 - 1]) : NULL;
  int kwh = (last_ty && sp_streq(last_ty, "KeywordHashNode")) ? argv2[argc2 - 1] : -1;
  int pos_argc = kwh >= 0 ? argc2 - 1 : argc2;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int provided = i < pos_argc ? argv2[i] : -1;
    /* Only bind from the keyword hash when the param was not already filled
       positionally -- otherwise a same-named key would clobber the positional. */
    if (kwh >= 0 && i >= pos_argc) {
      int kv = kwh_lookup(nt, kwh, m->pnames[i]);
      if (kv >= 0) provided = kv;
    }
    /* hide THIS inline's renames only: args are call-site expressions,
       and the call site may itself be an outer inlined body whose locals
       are renamed (nested yield-method inlines) -- zeroing the whole
       table emitted the unrenamed lv_<name> (undeclared identifier, or a
       silent capture of a same-named caller local). */
    int sv = g_nren; g_nren = saved_nren;
    /* args are CALL-SITE expressions: an ivar arg (`Set.new(@data)`) must
       read the caller's self, not the freshly-allocated instance g_self was
       repointed at for the inlined body */
    const char *svs = g_self, *svd = g_self_deref;
    g_self = saved_self; g_self_deref = saved_self_deref;
    emit_arg_or_default(c, m, i, provided, b);
    g_self = svs; g_self_deref = svd;
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  /* The inlined `initialize` body runs in the CONSTRUCTED class's context:
     an implicit-self call inside it (`setup` in `def initialize; setup;
     yield self; end`) must resolve against `ci`, not the caller's class.
     Args above were bound in the caller's context (g_self temporarily
     restored per arg; g_emitting_class_id untouched until here), and the
     spliced block body restores the caller's class via the fallback. */
  g_emitting_class_id = ci;
  int save_ind = g_indent; g_indent = din;
  emit_stmts(c, m->body, b, din);
  g_indent = save_ind;
  emit_indent(b, g_indent + 1);
  buf_printf(b, "_t%d;\n", st);
  emit_indent(b, g_indent); buf_puts(b, "})");

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_self = saved_self;
  g_self_deref = saved_self_deref;
  g_emitting_class_id = saved_emcls;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  g_yield_self_fallback = saved_self_fb;
  g_yield_self_deref_fallback = saved_deref_fb;
  g_yield_emitting_class_fallback = saved_emcls_fb;
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
      buf_printf(b, "(sp_%s){0}", c->classes[cid].c_name);
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


/* Compiler-synthesized instance methods that must stay invisible to
   reflection (respond_to?, instance_methods): they are implementation detail
   with no CRuby counterpart. Matched by exact name so a user-defined `__foo`
   stays visible. */
static int name_is_synth_method(const char *m) {
  return m && sp_streq(m, "__enum_to_a");
}

/* Enumerable's public instance methods, for respond_to? on a user class
   that includes the module (served through the __enum_to_a redirect). */
static int name_is_enumerable_module_method(const char *m) {
  static const char *const em[] = {
    "each", "map", "collect", "select", "filter", "reject", "find",
    "detect", "find_all", "find_index", "reduce", "inject", "to_a",
    "entries", "sort", "sort_by", "min", "max", "min_by", "max_by",
    "minmax", "minmax_by", "sum", "count", "include?", "member?",
    "first", "take", "drop", "take_while", "drop_while", "tally",
    "each_with_index", "each_with_object", "each_slice", "each_cons",
    "each_entry", "flat_map", "collect_concat", "group_by", "partition",
    "zip", "any?", "all?", "none?", "one?", "chunk", "chunk_while",
    "slice_when", "slice_before", "slice_after", "filter_map", "uniq",
    "to_h", "lazy", "cycle", "reverse_each", "to_set", NULL };
  for (int i = 0; em[i]; i++) if (sp_streq(m, em[i])) return 1;
  return 0;
}

/* Comparable's instance methods, for respond_to? on a user class that mixes it
   in (spinel keys the mixin off the presence of a user `<=>`). */
static int name_is_comparable_module_method(const char *m) {
  static const char *const cm[] = {
    "<", ">", "<=", ">=", "==", "between?", "clamp", NULL };
  for (int i = 0; cm[i]; i++) if (sp_streq(m, cm[i])) return 1;
  return 0;
}

/* Builtin Method#arity: (class, method) -> CRuby's arity, dumped from ruby
   4.0.4 over each class's OWN public instance methods (#2700). A miss falls
   through to the pre-existing path. */
static const struct { const char *cls; const char *m; int a; } sp_builtin_arity_tbl[] = {
  {"String","%",1},{"String","*",1},{"String","+",1},{"String","+@",0},
  {"String","-@",0},{"String","<<",1},{"String","<=>",1},{"String","==",1},
  {"String","===",1},{"String","=~",1},{"String","[]",-1},{"String","[]=",-1},
  {"String","append_as_bytes",-1},{"String","ascii_only?",0},{"String","b",0},{"String","byteindex",-1},
  {"String","byterindex",-1},{"String","bytes",0},{"String","bytesize",0},{"String","byteslice",-1},
  {"String","bytesplice",-1},{"String","capitalize",-1},{"String","capitalize!",-1},{"String","casecmp",1},
  {"String","casecmp?",1},{"String","center",-1},{"String","chars",0},{"String","chomp",-1},
  {"String","chomp!",-1},{"String","chop",0},{"String","chop!",0},{"String","chr",0},
  {"String","clear",0},{"String","codepoints",0},{"String","concat",-1},{"String","count",-1},
  {"String","crypt",1},{"String","dedup",0},{"String","delete",-1},{"String","delete!",-1},
  {"String","delete_prefix",1},{"String","delete_prefix!",1},{"String","delete_suffix",1},{"String","delete_suffix!",1},
  {"String","downcase",-1},{"String","downcase!",-1},{"String","dump",0},{"String","dup",0},
  {"String","each_byte",0},{"String","each_char",0},{"String","each_codepoint",0},{"String","each_grapheme_cluster",0},
  {"String","each_line",-1},{"String","empty?",0},{"String","encode",-1},{"String","encode!",-1},
  {"String","encoding",0},{"String","end_with?",-1},{"String","eql?",1},{"String","force_encoding",1},
  {"String","freeze",0},{"String","getbyte",1},{"String","grapheme_clusters",0},{"String","gsub",-1},
  {"String","gsub!",-1},{"String","hash",0},{"String","hex",0},{"String","include?",1},
  {"String","index",-1},{"String","insert",2},{"String","inspect",0},{"String","intern",0},
  {"String","length",0},{"String","lines",-1},{"String","ljust",-1},{"String","lstrip",-1},
  {"String","lstrip!",-1},{"String","match",-1},{"String","match?",-1},{"String","next",0},
  {"String","next!",0},{"String","oct",0},{"String","ord",0},{"String","partition",1},
  {"String","prepend",-1},{"String","replace",1},{"String","reverse",0},{"String","reverse!",0},
  {"String","rindex",-1},{"String","rjust",-1},{"String","rpartition",1},{"String","rstrip",-1},
  {"String","rstrip!",-1},{"String","scan",1},{"String","scrub",-1},{"String","scrub!",-1},
  {"String","setbyte",2},{"String","size",0},{"String","slice",-1},{"String","slice!",-1},
  {"String","split",-1},{"String","squeeze",-1},{"String","squeeze!",-1},{"String","start_with?",-1},
  {"String","strip",-1},{"String","strip!",-1},{"String","sub",-1},{"String","sub!",-1},
  {"String","succ",0},{"String","succ!",0},{"String","sum",-1},{"String","swapcase",-1},
  {"String","swapcase!",-1},{"String","to_c",0},{"String","to_f",0},{"String","to_i",-1},
  {"String","to_r",0},{"String","to_s",0},{"String","to_str",0},{"String","to_sym",0},
  {"String","tr",2},{"String","tr!",2},{"String","tr_s",2},{"String","tr_s!",2},
  {"String","undump",0},{"String","unicode_normalize",-1},{"String","unicode_normalize!",-1},{"String","unicode_normalized?",-1},
  {"String","unpack",-2},{"String","unpack1",-2},{"String","upcase",-1},{"String","upcase!",-1},
  {"String","upto",-1},{"String","valid_encoding?",0},{"Integer","%",1},{"Integer","&",1},
  {"Integer","*",1},{"Integer","**",1},{"Integer","+",1},{"Integer","-",1},
  {"Integer","-@",0},{"Integer","/",1},{"Integer","<",1},{"Integer","<<",1},
  {"Integer","<=",1},{"Integer","<=>",1},{"Integer","==",1},{"Integer","===",1},
  {"Integer",">",1},{"Integer",">=",1},{"Integer",">>",1},{"Integer","[]",-1},
  {"Integer","^",1},{"Integer","abs",0},{"Integer","allbits?",1},{"Integer","anybits?",1},
  {"Integer","bit_length",0},{"Integer","ceil",-1},{"Integer","ceildiv",1},{"Integer","chr",-1},
  {"Integer","coerce",1},{"Integer","denominator",0},{"Integer","digits",-1},{"Integer","div",1},
  {"Integer","divmod",1},{"Integer","downto",1},{"Integer","even?",0},{"Integer","fdiv",1},
  {"Integer","floor",-1},{"Integer","gcd",1},{"Integer","gcdlcm",1},{"Integer","inspect",-1},
  {"Integer","integer?",0},{"Integer","lcm",1},{"Integer","magnitude",0},{"Integer","modulo",1},
  {"Integer","next",0},{"Integer","nobits?",1},{"Integer","numerator",0},{"Integer","odd?",0},
  {"Integer","ord",0},{"Integer","pow",-1},{"Integer","pred",0},{"Integer","rationalize",-1},
  {"Integer","remainder",1},{"Integer","round",-1},{"Integer","size",0},{"Integer","succ",0},
  {"Integer","times",0},{"Integer","to_f",0},{"Integer","to_i",0},{"Integer","to_int",0},
  {"Integer","to_r",0},{"Integer","to_s",-1},{"Integer","truncate",-1},{"Integer","upto",1},
  {"Integer","zero?",0},{"Integer","|",1},{"Integer","~",0},{"Float","%",1},
  {"Float","*",1},{"Float","**",1},{"Float","+",1},{"Float","-",1},
  {"Float","-@",0},{"Float","/",1},{"Float","<",1},{"Float","<=",1},
  {"Float","<=>",1},{"Float","==",1},{"Float","===",1},{"Float",">",1},
  {"Float",">=",1},{"Float","abs",0},{"Float","angle",0},{"Float","arg",0},
  {"Float","ceil",-1},{"Float","coerce",1},{"Float","denominator",0},{"Float","divmod",1},
  {"Float","eql?",1},{"Float","fdiv",1},{"Float","finite?",0},{"Float","floor",-1},
  {"Float","hash",0},{"Float","infinite?",0},{"Float","inspect",0},{"Float","magnitude",0},
  {"Float","modulo",1},{"Float","nan?",0},{"Float","negative?",0},{"Float","next_float",0},
  {"Float","numerator",0},{"Float","phase",0},{"Float","positive?",0},{"Float","prev_float",0},
  {"Float","quo",1},{"Float","rationalize",-1},{"Float","round",-1},{"Float","to_f",0},
  {"Float","to_i",0},{"Float","to_int",0},{"Float","to_r",0},{"Float","to_s",0},
  {"Float","truncate",-1},{"Float","zero?",0},{"Array","&",1},{"Array","*",1},
  {"Array","+",1},{"Array","-",1},{"Array","<<",1},{"Array","<=>",1},
  {"Array","==",1},{"Array","[]",-1},{"Array","[]=",-1},{"Array","all?",-1},
  {"Array","any?",-1},{"Array","append",-1},{"Array","assoc",1},{"Array","at",1},
  {"Array","bsearch",0},{"Array","bsearch_index",0},{"Array","clear",0},{"Array","collect",0},
  {"Array","collect!",0},{"Array","combination",1},{"Array","compact",0},{"Array","compact!",0},
  {"Array","concat",-1},{"Array","count",-1},{"Array","cycle",-1},{"Array","deconstruct",0},
  {"Array","delete",1},{"Array","delete_at",1},{"Array","delete_if",0},{"Array","detect",-1},
  {"Array","difference",-1},{"Array","dig",-1},{"Array","drop",1},{"Array","drop_while",0},
  {"Array","each",0},{"Array","each_index",0},{"Array","empty?",0},{"Array","eql?",1},
  {"Array","fetch",-1},{"Array","fetch_values",-1},{"Array","fill",-1},{"Array","filter",0},
  {"Array","filter!",0},{"Array","find",-1},{"Array","find_index",-1},{"Array","first",-1},
  {"Array","flatten",-1},{"Array","flatten!",-1},{"Array","freeze",0},{"Array","hash",0},
  {"Array","include?",1},{"Array","index",-1},{"Array","insert",-1},{"Array","inspect",0},
  {"Array","intersect?",1},{"Array","intersection",-1},{"Array","join",-1},{"Array","keep_if",0},
  {"Array","last",-1},{"Array","length",0},{"Array","map",0},{"Array","map!",0},
  {"Array","max",-1},{"Array","min",-1},{"Array","minmax",0},{"Array","none?",-1},
  {"Array","one?",-1},{"Array","pack",-2},{"Array","permutation",-1},{"Array","pop",-1},
  {"Array","prepend",-1},{"Array","product",-1},{"Array","push",-1},{"Array","rassoc",1},
  {"Array","reject",0},{"Array","reject!",0},{"Array","repeated_combination",1},{"Array","repeated_permutation",1},
  {"Array","replace",1},{"Array","reverse",0},{"Array","reverse!",0},{"Array","reverse_each",0},
  {"Array","rfind",-1},{"Array","rindex",-1},{"Array","rotate",-1},{"Array","rotate!",-1},
  {"Array","sample",-1},{"Array","select",0},{"Array","select!",0},{"Array","shift",-1},
  {"Array","shuffle",-1},{"Array","shuffle!",-1},{"Array","size",0},{"Array","slice",-1},
  {"Array","slice!",-1},{"Array","sort",0},{"Array","sort!",0},{"Array","sort_by!",0},
  {"Array","sum",-1},{"Array","take",1},{"Array","take_while",0},{"Array","to_a",0},
  {"Array","to_ary",0},{"Array","to_h",0},{"Array","to_s",0},{"Array","transpose",0},
  {"Array","union",-1},{"Array","uniq",0},{"Array","uniq!",0},{"Array","unshift",-1},
  {"Array","values_at",-1},{"Array","zip",-1},{"Array","|",1},{"Hash","<",1},
  {"Hash","<=",1},{"Hash","==",1},{"Hash",">",1},{"Hash",">=",1},
  {"Hash","[]",1},{"Hash","[]=",2},{"Hash","any?",-1},{"Hash","assoc",1},
  {"Hash","clear",0},{"Hash","compact",0},{"Hash","compact!",0},{"Hash","compare_by_identity",0},
  {"Hash","compare_by_identity?",0},{"Hash","deconstruct_keys",1},{"Hash","default",-1},{"Hash","default=",1},
  {"Hash","default_proc",0},{"Hash","default_proc=",1},{"Hash","delete",1},{"Hash","delete_if",0},
  {"Hash","dig",-1},{"Hash","each",0},{"Hash","each_key",0},{"Hash","each_pair",0},
  {"Hash","each_value",0},{"Hash","empty?",0},{"Hash","eql?",1},{"Hash","except",-1},
  {"Hash","fetch",-1},{"Hash","fetch_values",-1},{"Hash","filter",0},{"Hash","filter!",0},
  {"Hash","flatten",-1},{"Hash","freeze",0},{"Hash","has_key?",1},{"Hash","has_value?",1},
  {"Hash","hash",0},{"Hash","include?",1},{"Hash","inspect",0},{"Hash","invert",0},
  {"Hash","keep_if",0},{"Hash","key",1},{"Hash","key?",1},{"Hash","keys",0},
  {"Hash","length",0},{"Hash","member?",1},{"Hash","merge",-1},{"Hash","merge!",-1},
  {"Hash","rassoc",1},{"Hash","rehash",0},{"Hash","reject",0},{"Hash","reject!",0},
  {"Hash","replace",1},{"Hash","select",0},{"Hash","select!",0},{"Hash","shift",0},
  {"Hash","size",0},{"Hash","slice",-1},{"Hash","store",2},{"Hash","to_a",0},
  {"Hash","to_h",0},{"Hash","to_hash",0},{"Hash","to_proc",0},{"Hash","to_s",0},
  {"Hash","transform_keys",-1},{"Hash","transform_keys!",-1},{"Hash","transform_values",0},{"Hash","transform_values!",0},
  {"Hash","update",-1},{"Hash","value?",1},{"Hash","values",0},{"Hash","values_at",-1},
  {"Symbol","<=>",1},{"Symbol","==",1},{"Symbol","===",1},{"Symbol","=~",1},
  {"Symbol","[]",-1},{"Symbol","capitalize",-1},{"Symbol","casecmp",1},{"Symbol","casecmp?",1},
  {"Symbol","downcase",-1},{"Symbol","empty?",0},{"Symbol","encoding",0},{"Symbol","end_with?",-1},
  {"Symbol","id2name",0},{"Symbol","inspect",0},{"Symbol","intern",0},{"Symbol","length",0},
  {"Symbol","match",-1},{"Symbol","match?",-1},{"Symbol","name",0},{"Symbol","next",0},
  {"Symbol","size",0},{"Symbol","slice",-1},{"Symbol","start_with?",-1},{"Symbol","succ",0},
  {"Symbol","swapcase",-1},{"Symbol","to_proc",0},{"Symbol","to_s",0},{"Symbol","to_sym",0},
  {"Symbol","upcase",-1},{"Range","%",1},{"Range","==",1},{"Range","===",1},
  {"Range","begin",0},{"Range","bsearch",0},{"Range","count",-1},{"Range","cover?",1},
  {"Range","each",0},{"Range","end",0},{"Range","entries",0},{"Range","eql?",1},
  {"Range","exclude_end?",0},{"Range","first",-1},{"Range","hash",0},{"Range","include?",1},
  {"Range","inspect",0},{"Range","last",-1},{"Range","max",-1},{"Range","member?",1},
  {"Range","min",-1},{"Range","minmax",0},{"Range","overlap?",1},{"Range","reverse_each",0},
  {"Range","size",0},{"Range","step",-1},{"Range","to_a",0},{"Range","to_s",0},
  {"Range","to_set",-1},{"Time","+",1},{"Time","-",1},{"Time","<=>",1},
  {"Time","asctime",0},{"Time","ceil",-1},{"Time","ctime",0},{"Time","day",0},
  {"Time","deconstruct_keys",1},{"Time","dst?",0},{"Time","eql?",1},{"Time","floor",-1},
  {"Time","friday?",0},{"Time","getgm",0},{"Time","getlocal",-1},{"Time","getutc",0},
  {"Time","gmt?",0},{"Time","gmt_offset",0},{"Time","gmtime",0},{"Time","gmtoff",0},
  {"Time","hash",0},{"Time","hour",0},{"Time","inspect",0},{"Time","isdst",0},
  {"Time","iso8601",-1},{"Time","localtime",-1},{"Time","mday",0},{"Time","min",0},
  {"Time","mon",0},{"Time","monday?",0},{"Time","month",0},{"Time","nsec",0},
  {"Time","round",-1},{"Time","saturday?",0},{"Time","sec",0},{"Time","strftime",1},
  {"Time","subsec",0},{"Time","sunday?",0},{"Time","thursday?",0},{"Time","to_a",0},
  {"Time","to_f",0},{"Time","to_i",0},{"Time","to_r",0},{"Time","to_s",0},
  {"Time","tuesday?",0},{"Time","tv_nsec",0},{"Time","tv_sec",0},{"Time","tv_usec",0},
  {"Time","usec",0},{"Time","utc",0},{"Time","utc?",0},{"Time","utc_offset",0},
  {"Time","wday",0},{"Time","wednesday?",0},{"Time","xmlschema",-1},{"Time","yday",0},
  {"Time","year",0},{"Time","zone",0},
  {NULL, NULL, 0}
};
/* Exported probes for the analyze-side method() desugar (#2752): whether the
   builtin table knows (cls, m), and whether m is universal Object surface. */
int builtin_method_known(const char *cls, const char *m);
int builtin_object_method_known(const char *m);
static int builtin_method_arity(const char *cls, const char *m, int *out) {
  for (int i = 0; sp_builtin_arity_tbl[i].cls; i++)
    if (sp_streq(sp_builtin_arity_tbl[i].cls, cls) && sp_streq(sp_builtin_arity_tbl[i].m, m))
      { *out = sp_builtin_arity_tbl[i].a; return 1; }
  return 0;
}
int builtin_method_known(const char *cls, const char *m) {
  int a;
  return builtin_method_arity(cls, m, &a);
}
int builtin_object_method_known(const char *m) {
  static const char *const OBJM2[] = {
    "class", "clone", "dup", "display", "enum_for", "eql?", "equal?",
    "extend", "freeze", "frozen?", "hash", "inspect", "instance_of?",
    "instance_variable_get", "instance_variable_set", "instance_variables",
    "is_a?", "itself", "kind_of?", "method", "methods", "nil?",
    "object_id", "public_send", "respond_to?", "send", "__send__",
    "tap", "then", "to_s", "yield_self", "==", "!=", "!", "===", NULL };
  for (int i = 0; OBJM2[i]; i++) if (sp_streq(m, OBJM2[i])) return 1;
  return 0;
}


/* The constant names a class/module defines in its OWN body, in declaration
   order, appended to `out` (deduplicated). Scans every ClassNode/ModuleNode
   that opens `ci`, so reopenings contribute too. #2674 */
static int class_own_constants(Compiler *c, int ci, const char **out, int max, int n) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count && n < max; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || comp_class_index(c, cn) != ci) continue;
    int body = nt_ref(nt, id, "body");
    int bn = 0; const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn && n < max; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty || !sp_streq(sty, "ConstantWriteNode")) continue;
      const char *nm = nt_str(nt, stmts[k], "name");
      if (!nm) continue;
      int dup = 0;
      for (int q = 0; q < n; q++) if (sp_streq(out[q], nm)) { dup = 1; break; }
      if (!dup) out[n++] = nm;
    }
  }
  return n;
}

/* The user-class indices `ci` names in its body via `include` / `prepend`
   (declaration order). Builtin modules carry no constants, so they are skipped. */
static int class_module_refs(Compiler *c, int ci, const char *kw, int *out, int max) {
  const NodeTable *nt = c->nt;
  int n = 0;
  for (int id = 0; id < nt->count && n < max; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
    int cp = nt_ref(nt, id, "constant_path");
    const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
    if (!cn || comp_class_index(c, cn) != ci) continue;
    int body = nt_ref(nt, id, "body");
    int bn = 0; const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn && n < max; k++) {
      const char *sty = nt_type(nt, stmts[k]);
      if (!sty || !sp_streq(sty, "CallNode")) continue;
      const char *nm = nt_str(nt, stmts[k], "name");
      if (!nm || !sp_streq(nm, kw) || nt_ref(nt, stmts[k], "receiver") >= 0) continue;
      int an = nt_ref(nt, stmts[k], "arguments");
      int ac = 0; const int *av = an >= 0 ? nt_arr(nt, an, "arguments", &ac) : NULL;
      for (int j = 0; j < ac && n < max; j++) {
        const char *aty = nt_type(nt, av[j]);
        const char *mn = (aty && (sp_streq(aty, "ConstantReadNode") || sp_streq(aty, "ConstantPathNode")))
                         ? nt_str(nt, av[j], "name") : NULL;
        int mi = mn ? comp_class_index(c, mn) : -1;
        if (mi >= 0) out[n++] = mi;
      }
    }
  }
  return n;
}

/* Module#constants: the receiver's own constants first, then the remaining
   ancestors' (prepends, includes, superclass chain) -- CRuby reports own before
   inherited even when a prepended module precedes the class in #ancestors.
   `inherit == 0` stops at self. The walk stays inside user classes: Object's
   constants are the top-level ones, which CRuby does not report here.
   The order WITHIN one class is its declaration order; CRuby's comes out of an
   id table and is not specified, so only the grouping is meaningful. #2674 */
static int collect_class_constants(Compiler *c, int ci, int inherit,
                                   const char **out, int max, int n, int depth) {
  if (ci < 0 || ci >= c->nclasses || depth > 32 || n >= max) return n;
  n = class_own_constants(c, ci, out, max, n);
  if (!inherit) return n;
  int mods[32], nm2;
  nm2 = class_module_refs(c, ci, "prepend", mods, 32);
  for (int q = nm2 - 1; q >= 0; q--) n = collect_class_constants(c, mods[q], 1, out, max, n, depth + 1);
  nm2 = class_module_refs(c, ci, "include", mods, 32);
  for (int q = nm2 - 1; q >= 0; q--) n = collect_class_constants(c, mods[q], 1, out, max, n, depth + 1);
  return collect_class_constants(c, c->classes[ci].parent, 1, out, max, n, depth + 1);
}

/* eval(string) / Kernel.eval(string) compiling an arbitrary runtime string is a
   hard AOT boundary, not a missing feature. If node `id` is such a call, emit
   the intentional diagnostic and return 1; otherwise return 0. Shared by
   emit_call and the output builtins (puts/print/p) so `puts eval(s)` gets the
   same specific message as `x = eval(s)` rather than a generic argument dump.
   The instance_eval/class_eval/module_eval block forms carry a literal block,
   not a string, and are handled separately -- they never reach here. */
/* Does the program define a method of this name itself? Such a call is that
   method, not the builtin, so a documented-limit diagnostic must not claim it.
   Singleton (class) methods count too: `def self.prepend(...)` on a module
   makes `Mod.prepend(...)` that method, not Module#prepend (#2712). */
int diag_user_defines(Compiler *c, const char *name) {
  for (int uk = 0; uk < c->nclasses; uk++) {
    if (comp_method_in_chain(c, uk, name, NULL) >= 0) return 1;
    if (comp_cmethod_in_chain(c, uk, name, NULL) >= 0) return 1;
  }
  return comp_method_index(c, name) >= 0;
}

/* BasicObject's own instance methods (the spinel-relevant surface). */
static int basicobject_own_method(const char *n) {
  static const char *const B[] = { "==", "!=", "!", "equal?", "instance_eval",
    "instance_exec", "__send__", "__id__", "initialize", "method_missing", NULL };
  for (int i = 0; B[i]; i++) if (sp_streq(n, B[i])) return 1;
  return 0;
}

/* The positional-argument count of a CallNode (0 when it has none). */
static int argc_of(const NodeTable *nt, int id) {
  int a = nt_ref(nt, id, "arguments");
  int n = 0;
  if (a >= 0) nt_arr(nt, a, "arguments", &n);
  return n;
}

/* A call to something spinel deliberately does not support (docs/limitations.md).
   Without this the call falls through to the generic diagnostic, which dumps
   node ids and argument types rather than naming the feature -- or, for a name
   the runtime happens to reach, to a NoMethodError that reads like an
   implementation gap instead of a documented limit. Names a user class defines
   are left alone: they are that method, not the builtin. Returns 1 if it
   reported (never returns -- `unsupported` is noreturn). #2652 / #2667 / #2668 */
int diagnose_unsupported_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, id);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  static const struct { const char *m; const char *why; } tbl[] = {
    { "define_singleton_method",
      "Object#define_singleton_method is not supported by AOT compilation: a per-object "
      "method table would have to be consulted on every call, but each call site is a "
      "direct C call to a compiled body. Define the method in the class body instead "
      "(see docs/limitations.md)" },
    { "extend",
      "Object#extend is not supported by AOT compilation: mixing a module into a live "
      "object needs a per-object method table. Use `include`/`prepend` in the class body "
      "instead (see docs/limitations.md)" },
    { "ruby2_keywords",
      "Proc#ruby2_keywords is not supported: it is a shim for the Ruby 2.x-to-3.0 "
      "keyword-argument transition, and spinel targets modern keyword semantics, so it "
      "has nothing to toggle (see docs/limitations.md)" },
    { "ruby2_keywords_hash",
      "Hash.ruby2_keywords_hash is not supported: it marks a hash for the Ruby "
      "2.x-to-3.0 keyword-argument transition shim, and spinel targets modern keyword "
      "semantics, so the flag has nothing to toggle (see docs/limitations.md)" },
    { "ruby2_keywords_hash?",
      "Hash.ruby2_keywords_hash? is not supported: it reads the Ruby 2.x-to-3.0 "
      "keyword-transition flag, which spinel's hashes do not carry (see "
      "docs/limitations.md)" },
    { "unicode_normalize",
      "String#unicode_normalize is not supported: Unicode normalization requires "
      "shipping the Unicode decomposition/composition tables, which spinel "
      "deliberately does not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
    { "unicode_normalize!",
      "String#unicode_normalize! is not supported: Unicode normalization requires "
      "shipping the Unicode decomposition/composition tables, which spinel "
      "deliberately does not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
    { "unicode_normalized?",
      "String#unicode_normalized? is not supported: answering it requires the "
      "Unicode decomposition/composition tables, which spinel deliberately does "
      "not carry -- the same limit as String#grapheme_clusters "
      "(see docs/limitations.md)" },
    { "singleton_class",
      "Object#singleton_class is not supported by AOT compilation: it is the gateway "
      "to a per-object method table, which direct C calls have no room for -- the same "
      "limit as define_singleton_method. Define methods in the class body instead "
      "(see docs/limitations.md)" },
    { "set_trace_func",
      "set_trace_func is not supported by AOT compilation: it requires an interpreter "
      "loop to hook, and compiled code has no such loop (see docs/limitations.md)" },
    { "callcc",
      "Kernel#callcc is not supported by AOT compilation: multi-shot full-stack capture "
      "has no flat-C analogue. Fiber covers the single-shot cases (see docs/limitations.md)" },
    { "refine",
      "Refinements are not supported by AOT compilation: scope-keyed dispatch is "
      "incompatible with direct C calls. Reopen the class instead (see docs/limitations.md)" },
    { "using",
      "Refinements are not supported by AOT compilation: scope-keyed dispatch is "
      "incompatible with direct C calls. Reopen the class instead (see docs/limitations.md)" },
    { NULL, NULL }
  };
  int hit = -1;
  for (int k = 0; tbl[k].m; k++) if (sp_streq(name, tbl[k].m)) { hit = k; break; }

  /* The receiver's constant name, for the limits that are keyed on it. */
  int recv = nt_ref(nt, id, "receiver");
  const char *rty = recv >= 0 ? nt_type(nt, recv) : NULL;
  const char *rcn = (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode")))
                    ? nt_str(nt, recv, "name") : NULL;
  const char *why = hit >= 0 ? tbl[hit].why : NULL;

  if (!why && rcn && comp_class_index(c, rcn) < 0) {
    /* Namespaces that exist only in an interpreter. Keyed on the receiver, so
       every method on them reports the same way rather than one-by-one. */
    if (sp_streq(rcn, "ObjectSpace"))
      why = "ObjectSpace is not supported by AOT compilation: there is no class-keyed "
            "allocation registry to walk -- the GC tracks bytes, not a live-object index "
            "(see docs/limitations.md)";
    else if (sp_streq(rcn, "TracePoint"))
      why = "TracePoint is not supported by AOT compilation: it requires an interpreter "
            "loop to hook, and compiled code has no such loop (see docs/limitations.md)";
    else if (sp_streq(rcn, "Continuation"))
      why = "Continuation is not supported by AOT compilation: multi-shot full-stack "
            "capture has no flat-C analogue. Fiber covers the single-shot cases "
            "(see docs/limitations.md)";
    /* `Class.new(parent) { ... }`: the class graph is baked at compile time. A
       bare `Class.new` with no argument and no block is just an Object factory
       and is left to the normal path. */
    else if (sp_streq(rcn, "Class") && sp_streq(name, "new") &&
             (nt_ref(nt, id, "block") >= 0 || argc_of(nt, id) > 0))
      why = "Class.new(parent) { ... } is not supported by AOT compilation: the class "
            "graph, ancestor chain, and method/ivar layout are baked at compile time. "
            "Declare the class with `class ... end` instead (see docs/limitations.md)";
  }

  /* Structural mutation of a class through an explicit receiver: the same
     declarations INSIDE a `class` body (where they are receiverless) work. */
  if (!why && rcn && comp_class_index(c, rcn) >= 0 &&
      (sp_streq(name, "include") || sp_streq(name, "prepend") ||
       sp_streq(name, "attr_accessor") || sp_streq(name, "attr_reader") ||
       sp_streq(name, "attr_writer") || sp_streq(name, "define_method"))) {
    static char buf[512];
    snprintf(buf, sizeof buf,
             "%s.%s(...) is not supported by AOT compilation: the class graph, ancestor "
             "chain, and method/ivar layout are baked at compile time, so a class cannot be "
             "restructured through an explicit receiver. Move the `%s` inside `class %s ... "
             "end` (see docs/limitations.md)", rcn, name, name, rcn);
    why = buf;
  }

  if (!why) {
    /* a documented limit buried under a chain (`obj.singleton_class.class`)
       would otherwise be shadowed by the OUTER call's generic diagnostic:
       walk down the receiver chain */
    if (recv >= 0 && nt_kind(nt, recv) == NK_CallNode)
      return diagnose_unsupported_call(c, recv);
    return 0;
  }
  if (diag_user_defines(c, name)) return 0;
  unsupported_feature(c, id, why);
  return 1;
}

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

/* Unbox the boxed proc result (_sp_proc_poly_ret, an sp_RbVal) to the call's
   statically-inferred return type. Inverse of emit_box_open: every first-class
   proc now publishes its result boxed in the slot (the universal return ABI),
   so a `.call` reads it back through here. sp_poly_to_i/f coerce defensively
   (matching the historical float arm); pointer kinds read the union member the
   matching sp_box_* wrote (strings live in v.s, other heap values in v.p). */
void emit_proc_ret_unbox(Compiler *c, TyKind rty, Buf *b) {
  if (rty == TY_POLY || rty == TY_UNKNOWN) { buf_puts(b, "_sp_proc_poly_ret"); return; }
  if (rty == TY_FLOAT)  { buf_puts(b, "sp_poly_to_f(_sp_proc_poly_ret)"); return; }
  if (rty == TY_SYMBOL) { buf_puts(b, "(sp_sym)sp_poly_to_i(_sp_proc_poly_ret)"); return; }
  if (proc_slot_is_direct(rty)) { buf_puts(b, "sp_poly_to_i(_sp_proc_poly_ret)"); return; }  /* int/bool/nil */
  if (rty == TY_STRING) { buf_puts(b, "_sp_proc_poly_ret.v.s"); return; }
  if (rty == TY_RANGE)  { buf_puts(b, "(*(sp_Range *)_sp_proc_poly_ret.v.p)"); return; }
  if (rty == TY_TIME)   { buf_puts(b, "(*(sp_Time *)_sp_proc_poly_ret.v.p)"); return; }
  buf_puts(b, "("); emit_ctype(c, rty, b); buf_puts(b, ")_sp_proc_poly_ret.v.p");  /* array/hash/object */
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
  /* A float arg also forces the boxed side-channel: an mrb_float placed in the
     mrb_int[] slot is value-truncated (0.7 -> 0), so it must be published boxed
     like a poly and read back with sp_poly_to_f in the callee. */
  for (int k = 0; k < nargs && !any_poly; k++) {
    TyKind at = comp_ntype(c, argv[k]);
    if (at == TY_POLY || at == TY_FLOAT) any_poly = 1;
  }
  buf_printf(b, "%d, ", argc);
  if (any_poly) {
    g_needs_proc_poly_argslot = 1;  /* channel array now lives in sp_runtime.h */
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
      else if (proc_slot_is_ptr(at) || at == TY_PROC) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]); }
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
      else if (proc_slot_is_ptr(at) || at == TY_PROC) buf_printf(b, "(mrb_int)(uintptr_t)_t%d", atmp[k]);
      else if (at == TY_FLOAT) buf_puts(b, "0");  /* float rides the boxed side-channel; the mrb_int slot is dead */
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
void emit_rat_coerce(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_RATIONAL) { emit_expr(c, node, b); return; }
  if (comp_ntype(c, node) == TY_FLOAT) {
    /* the exact rational value of the double, not a truncating int cast */
    buf_puts(b, "sp_float_to_rational("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  buf_puts(b, "sp_rational_new((mrb_int)("); emit_expr(c, node, b); buf_puts(b, "), 1)");
}
/* Emit a node as an sp_Complex: a Complex stays as-is, an Integer/Float
   becomes re+0i (a Float operand marks the real component Float-classed). */
static void emit_complex_coerce(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_COMPLEX) { emit_expr(c, node, b); return; }
  /* a poly operand (e.g. a Complex read out of a poly array) unboxes at
     runtime -- a boxed Complex keeps its components, a real number is re+0i */
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_as_complex("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  /* a Rational operand computes in floats (owner-approved divergence: CRuby
     keeps rational components, spinel's Complex stores machine floats --
     documented in docs/limitations.md) */
  if (comp_ntype(c, node) == TY_RATIONAL) {
    buf_puts(b, "((sp_Complex){sp_rational_to_f("); emit_expr(c, node, b);
    buf_puts(b, "), 0, 1})");
    return;
  }
  buf_puts(b, "((sp_Complex){(mrb_float)("); emit_expr(c, node, b);
  buf_printf(b, "), 0, %d})", comp_ntype(c, node) == TY_FLOAT ? 1 : 0);
}

/* Returns 1 if `id` is a `Float::INFINITY` / `nil` / absent range endpoint. */
int lazy_endpoint_is_infinite(Compiler *c, int right) {
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

/* `<source>.lazy.<ops>.size`: the source size propagated through the chain, no
   iteration. Size-preserving stages (map/collect) keep it; take(n)/drop(n)
   transform it (min / max(0,-)); any element-count-changing stage (select,
   reject, filter, filter_map, flat_map, take_while, drop_while) makes it nil. An
   endless source is Float::INFINITY unless a take bounds it. (#2485) */
int emit_lazy_size_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *tname = nt_str(nt, id, "name");
  if (!tname || !sp_streq(tname, "size") || nt_ref(nt, id, "block") >= 0) return 0;
  { int ar = nt_ref(nt, id, "arguments"); int ac = 0; if (ar >= 0) nt_arr(nt, ar, "arguments", &ac); if (ac != 0) return 0; }
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  enum { LK_MAP, LK_TAKE, LK_DROP, LK_KILL };
  struct { int kind; int arg; } ops[32]; int nops = 0;
  int cur = recv, lazy_src = -1;
  while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
    const char *nm = nt_str(nt, cur, "name");
    if (!nm) return 0;
    if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) { lazy_src = unwrap_parens(c, nt_ref(nt, cur, "receiver")); break; }
    int blk = nt_ref(nt, cur, "block");
    if (nops >= 32) return 0;
    if ((sp_streq(nm, "take") || sp_streq(nm, "drop")) && blk < 0) {
      int ar = nt_ref(nt, cur, "arguments"); int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
      if (ac != 1 || !av) return 0;
      ops[nops].kind = sp_streq(nm, "take") ? LK_TAKE : LK_DROP; ops[nops].arg = av[0]; nops++;
      cur = nt_ref(nt, cur, "receiver"); continue;
    }
    if (blk < 0) return 0;
    if (sp_streq(nm, "map") || sp_streq(nm, "collect")) { ops[nops].kind = LK_MAP; ops[nops].arg = -1; nops++; }
    else if (sp_streq(nm, "select") || sp_streq(nm, "filter") || sp_streq(nm, "find_all") ||
             sp_streq(nm, "reject") || sp_streq(nm, "take_while") || sp_streq(nm, "drop_while") ||
             sp_streq(nm, "filter_map") || sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) {
      ops[nops].kind = LK_KILL; ops[nops].arg = -1; nops++;
    }
    else return 0;
    cur = nt_ref(nt, cur, "receiver");
  }
  if (lazy_src < 0) return 0;
  TyKind st = infer_type(c, lazy_src);
  int src_is_range = (st == TY_RANGE);
  /* an empty `[]` literal source infers UNKNOWN; treat any ArrayNode as an array */
  int src_is_arr = ty_is_array(st) ||
                   (nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ArrayNode"));
  if (!src_is_range && !src_is_arr) return 0;
  /* any element-count-changing stage -> nil */
  for (int i = 0; i < nops; i++) if (ops[i].kind == LK_KILL) { buf_puts(b, "sp_box_nil()"); return 1; }
  int has_take = 0; for (int i = 0; i < nops; i++) if (ops[i].kind == LK_TAKE) has_take = 1;
  int endless = 0, excl = 0, range_lit = 0, lo = -1, hi = -1, trange = -1;
  if (src_is_range) {
    range_lit = nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "RangeNode");
    if (range_lit) {
      excl = (nt_int(nt, lazy_src, "flags", 0) & 4) ? 1 : 0;
      hi = nt_ref(nt, lazy_src, "right"); lo = nt_ref(nt, lazy_src, "left");
      endless = lazy_endpoint_is_infinite(c, hi);
    } else {
      trange = ++g_tmp; Buf sb = expr_buf(c, lazy_src);
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_Range _t%d = %s;\n", trange, sb.p ? sb.p : "(sp_Range){0}"); free(sb.p);
    }
  }
  /* endless source with no bounding take -> Float::INFINITY */
  if (endless && !has_take) { buf_puts(b, "(1.0/0.0)"); return 1; }
  /* compute source length (INTPTR_MAX sentinel for an endless source; a take
     below reduces it to a finite value -- guaranteed here since has_take). */
  /* build the source-length expression into a local buffer first: an array
     literal source boxes with its own g_pre decls, which must precede this line. */
  Buf lenb; memset(&lenb, 0, sizeof lenb);
  if (src_is_range) {
    if (endless) buf_puts(&lenb, "INTPTR_MAX");
    else if (range_lit) { buf_puts(&lenb, "("); emit_int_expr(c, hi, &lenb); buf_puts(&lenb, " - "); emit_int_expr(c, lo, &lenb); buf_printf(&lenb, " + %d)", 1 - excl); }
    else buf_printf(&lenb, "(_t%d.last - _t%d.first + 1 - (mrb_int)_t%d.excl)", trange, trange, trange);
  } else {
    buf_puts(&lenb, "sp_poly_length("); emit_boxed(c, lazy_src, &lenb); buf_puts(&lenb, ")");
  }
  int tsz = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _t%d = %s;\n", tsz, lenb.p ? lenb.p : "0");
  free(lenb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d < 0) _t%d = 0;\n", tsz, tsz);
  /* apply take/drop source->terminal (ops were collected terminal-first) */
  for (int i = nops - 1; i >= 0; i--) {
    if (ops[i].kind == LK_MAP) continue;
    emit_indent(g_pre, g_indent);
    if (ops[i].kind == LK_TAKE) {
      buf_puts(g_pre, "{ mrb_int _n = "); emit_int_expr(c, ops[i].arg, g_pre);
      buf_printf(g_pre, "; if (_n < 0) _n = 0; if (_n < _t%d) _t%d = _n; }\n", tsz, tsz);
    } else {
      buf_puts(g_pre, "{ mrb_int _n = "); emit_int_expr(c, ops[i].arg, g_pre);
      buf_printf(g_pre, "; _t%d = _t%d > _n ? _t%d - _n : 0; }\n", tsz, tsz, tsz);
    }
  }
  buf_printf(b, "_t%d", tsz);
  return 1;
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

  enum { OP_MAP, OP_FILTER, OP_TAKEWHILE, OP_DROPWHILE, OP_FILTERMAP, OP_FLATMAP, OP_TAKE, OP_DROP, OP_EACHSLICE };
  /* arg/cnt/lim are used only by the blockless counter stages (take/drop):
     arg is the count AST node, lim a prelude temp holding its value, cnt a
     prelude counter initialised to 0. */
  struct { int kind; int block; int negate; int arg; int cnt; int lim; } ops[16];
  int nops = 0, cur = recv, lazy_src = -1;
  while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
    const char *nm = nt_str(nt, cur, "name");
    if (!nm) return 0;
    if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) {
      lazy_src = unwrap_parens(c, nt_ref(nt, cur, "receiver"));
      break;
    }
    /* Blockless counter stages: `take(n)` limits the stream to n elements
       (break once reached), `drop(n)` skips the first n. Both stay lazy in
       CRuby, fusing straight into the loop. */
    if ((sp_streq(nm, "take") || sp_streq(nm, "drop") ||
         (sp_streq(nm, "each_slice") && nops == 0)) && nt_ref(nt, cur, "block") < 0) {
      int ar = nt_ref(nt, cur, "arguments");
      int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
      if (ac != 1 || !av || nops >= 16) return 0;
      ops[nops].kind = sp_streq(nm, "take") ? OP_TAKE :
                       sp_streq(nm, "drop") ? OP_DROP : OP_EACHSLICE;
      ops[nops].block = -1; ops[nops].negate = 0; ops[nops].arg = av[0];
      ops[nops].cnt = -1; ops[nops].lim = -1;
      nops++;
      cur = nt_ref(nt, cur, "receiver");
      continue;
    }
    int blk = nt_ref(nt, cur, "block");
    if (blk < 0 || nops >= 16) return 0;
    if (sp_streq(nm, "map") || sp_streq(nm, "collect")) ops[nops].kind = OP_MAP, ops[nops].negate = 0;
    else if (sp_streq(nm, "select") || sp_streq(nm, "filter") ||
             sp_streq(nm, "find_all")) ops[nops].kind = OP_FILTER, ops[nops].negate = 0;
    else if (sp_streq(nm, "reject")) ops[nops].kind = OP_FILTER, ops[nops].negate = 1;
    else if (sp_streq(nm, "take_while")) ops[nops].kind = OP_TAKEWHILE, ops[nops].negate = 0;
    else if (sp_streq(nm, "drop_while")) ops[nops].kind = OP_DROPWHILE, ops[nops].negate = 0;
    else if (sp_streq(nm, "filter_map")) ops[nops].kind = OP_FILTERMAP, ops[nops].negate = 0;
    /* flat_map splices its block result into the stream; supported as the
       stage adjacent to the terminal (nops == 0 here: ops collect
       terminal-first), where the splice happens straight into the result. */
    else if ((sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) && nops == 0)
      ops[nops].kind = OP_FLATMAP, ops[nops].negate = 0;
    else return 0;
    ops[nops].block = blk;
    ops[nops].arg = -1; ops[nops].cnt = -1; ops[nops].lim = -1;
    nops++;
    cur = nt_ref(nt, cur, "receiver");
  }
  if (lazy_src < 0) return 0;

  TyKind st = infer_type(c, lazy_src);
  int src_is_range = (st == TY_RANGE), src_is_intarr = (st == TY_INT_ARRAY);
  int src_is_enum = (st == TY_ENUMERATOR);
  /* other array kinds iterate a boxed-element snapshot */
  int src_is_arr = (st == TY_POLY_ARRAY || st == TY_STR_ARRAY || st == TY_FLOAT_ARRAY);
  if (!src_is_range && !src_is_intarr && !src_is_enum && !src_is_arr) return 0;

  int excl = 0, endless = 0, right = -1, left_n = -1;
  int src_range_literal = 0, trange = -1;
  if (src_is_range) {
    src_range_literal = nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "RangeNode");
    if (src_range_literal) {
      excl = (int)(nt_int(nt, lazy_src, "flags", 0) & 4) ? 1 : 0;
      right = nt_ref(nt, lazy_src, "right");
      endless = lazy_endpoint_is_infinite(c, right);
      left_n = nt_ref(nt, lazy_src, "left");
    } else {
      /* a range held in a variable or returned from a method: materialize it
         once and read the bounds from the runtime sp_Range value. */
      Buf sb = expr_buf(c, lazy_src);
      trange = ++g_tmp; emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = %s;\n", trange, sb.p ? sb.p : "(sp_Range){0}"); free(sb.p);
    }
  }
  if (is_toa && (src_is_range && endless)) {
    /* an endless source terminates only through a bounding stage */
    int bounded = 0;
    for (int oi = 0; oi < nops; oi++)
      if (ops[oi].kind == OP_TAKE || ops[oi].kind == OP_TAKEWHILE) bounded = 1;
    if (!bounded) return 0;
  }

  /* prelude temps. The pipeline is poly-typed: lazy block params infer poly, so
     each stage carries a boxed value and collects into a PolyArray. */
  int tres = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  /* Per-stage state for the blockless counter stages: evaluate the count once
     into a limit temp and start a zeroed counter, both before the loop. */
  for (int oi = 0; oi < nops; oi++) {
    if (ops[oi].kind == OP_DROPWHILE) {
      /* 1 while still dropping; cleared at the first false predicate */
      ops[oi].cnt = ++g_tmp; emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "int _t%d = 1;\n", ops[oi].cnt);
      continue;
    }
    if (ops[oi].kind == OP_EACHSLICE) {
      Buf ab = expr_buf(c, ops[oi].arg);
      ops[oi].lim = ++g_tmp; emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "mrb_int _t%d = %s;\n", ops[oi].lim, ab.p ? ab.p : "0"); free(ab.p);
      ops[oi].cnt = ++g_tmp; emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n",
                 ops[oi].cnt, ops[oi].cnt);
      continue;
    }
    if (ops[oi].kind != OP_TAKE && ops[oi].kind != OP_DROP) continue;
    Buf ab = expr_buf(c, ops[oi].arg);
    ops[oi].lim = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = %s;\n", ops[oi].lim, ab.p ? ab.p : "0"); free(ab.p);
    ops[oi].cnt = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = 0;\n", ops[oi].cnt);
  }
  int tn = -1;
  if (has_count) {
    Buf nb = expr_buf(c, count_node);
    tn = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = %s;\n", tn, nb.p ? nb.p : "0"); free(nb.p);
  }
  int thi = -1, tsrc = -1;
  if (src_is_range && !endless) {
    thi = ++g_tmp; emit_indent(g_pre, g_indent);
    if (src_range_literal) { Buf hb = expr_buf(c, right); buf_printf(g_pre, "mrb_int _t%d = %s;\n", thi, hb.p ? hb.p : "0"); free(hb.p); }
    else buf_printf(g_pre, "mrb_int _t%d = _t%d.last;\n", thi, trange);
  }
  if (src_is_intarr) {
    Buf sb = expr_buf(c, lazy_src);
    tsrc = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", tsrc, sb.p ? sb.p : "0", tsrc); free(sb.p);
  }
  if (src_is_arr) {
    /* snapshot the source's elements boxed (shared element walker) */
    Buf sb = {0};
    emit_boxed(c, lazy_src, &sb);
    tsrc = ++g_tmp; emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_enum_items_from(%s); SP_GC_ROOT(_t%d);\n", tsrc, sb.p ? sb.p : "0", tsrc); free(sb.p);
  }

  int tloop = ++g_tmp, tv = ++g_tmp;
  Buf lo_b; memset(&lo_b, 0, sizeof lo_b);
  if (src_is_range) { if (src_range_literal) emit_expr(c, left_n, &lo_b); else buf_printf(&lo_b, "_t%d.first", trange); }
  char cbuf[64]; cbuf[0] = 0;
  if (has_count) snprintf(cbuf, sizeof cbuf, " && sp_PolyArray_length(_t%d) < _t%d", tres, tn);
  if (src_is_range) {
    emit_indent(g_pre, g_indent);
    if (endless)
      buf_printf(g_pre, "for (mrb_int _t%d = %s; 1%s; _t%d++) {\n", tloop, lo_b.p ? lo_b.p : "0", cbuf, tloop);
    else if (src_range_literal)
      buf_printf(g_pre, "for (mrb_int _t%d = %s; _t%d %s _t%d%s; _t%d++) {\n",
                 tloop, lo_b.p ? lo_b.p : "0", tloop, excl ? "<" : "<=", thi, cbuf, tloop);
    else
      buf_printf(g_pre, "for (mrb_int _t%d = %s; _t%d <= _t%d - _t%d.excl%s; _t%d++) {\n",
                 tloop, lo_b.p ? lo_b.p : "0", tloop, thi, trange, cbuf, tloop);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_int(_t%d);\n", tv, tloop);
  }
  else if (src_is_intarr) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d)%s; _t%d++) {\n",
               tloop, tloop, tsrc, cbuf, tloop);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_int(sp_IntArray_get(_t%d, _t%d)); SP_GC_ROOT_RBVAL(_t%d);\n", tv, tsrc, tloop, tv);
  }
  else if (src_is_arr) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len%s; _t%d++) {\n",
               tloop, tloop, tsrc, cbuf, tloop);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = _t%d->data[_t%d]; SP_GC_ROOT_RBVAL(_t%d);\n", tv, tsrc, tloop, tv);
  }
  else {
    /* Enumerator source: drive a fresh run one value at a time -- a generator
       via its fiber, a materialized enumerator via a local cursor -- so an
       infinite generator streams through first(n) without materializing. */
    int te = ++g_tmp, tf = ++g_tmp, tidx = ++g_tmp;
    Buf sb = expr_buf(c, lazy_src);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Enumerator *_t%d = %s; SP_GC_ROOT(_t%d);\n", te, sb.p ? sb.p : "0", te); free(sb.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Fiber *_t%d = (_t%d && _t%d->gen) ? sp_Fiber_new(_t%d->gen) : NULL; SP_GC_ROOT(_t%d);\n", tf, te, te, te, tf);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (_t%d && _t%d->gen_cap) _t%d->user_data = _t%d->gen_cap;\n", tf, te, tf, te);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = 0;\n", tidx);
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "for (;;) {\n");
    if (has_count) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (sp_PolyArray_length(_t%d) >= _t%d) break;\n", tres, tn);
    }
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d;\n", tv);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d) { if (!sp_Fiber_alive(_t%d)) break; _t%d = sp_Fiber_resume(_t%d, sp_box_nil()); if (!sp_Fiber_alive(_t%d)) break; }\n",
               tf, tf, tv, tf, tf);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "else { if (!_t%d || !_t%d->items || _t%d >= _t%d->items->len) break; _t%d = _t%d->items->data[_t%d++]; }\n",
               te, te, tidx, te, tv, te, tidx);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tv);
  }
  free(lo_b.p);

  char vbuf[24]; snprintf(vbuf, sizeof vbuf, "_t%d", tv);
  /* ops are collected terminal-first; apply them source-first. */
  for (int oi = nops - 1; oi >= 0; oi--) {
    /* Blockless counter stages have no block to bind -- apply and skip the
       block machinery. take breaks the source loop once the limit is reached;
       drop consumes (skips) the first n elements. */
    if (ops[oi].kind == OP_TAKE) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d >= _t%d) break;\n", ops[oi].cnt, ops[oi].lim);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "_t%d++;\n", ops[oi].cnt);
      continue;
    }
    if (ops[oi].kind == OP_DROP) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d < _t%d) { _t%d++; continue; }\n", ops[oi].cnt, ops[oi].lim, ops[oi].cnt);
      continue;
    }
    if (ops[oi].kind == OP_EACHSLICE) {
      /* group the stream into boxed n-element chunks; only ever the
         terminal-adjacent stage (oi == 0), so it owns the result push */
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ops[oi].cnt, vbuf);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (sp_PolyArray_length(_t%d) >= _t%d) {\n", ops[oi].cnt, ops[oi].lim);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tres, ops[oi].cnt);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", ops[oi].cnt);
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent + 1);
      buf_puts(g_pre, "continue;\n");
      continue;
    }
    int blk = ops[oi].block;
    const char *bp0 = block_param_name(c, blk, 0);
    const char *bp = (bp0 && bp0[0]) ? rename_local(bp0) : "_lx";
    if (ops[oi].kind == OP_DROPWHILE) {
      /* while dropping, evaluate the predicate: true skips the element,
         false clears the flag and lets everything through untouched */
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d) {\n", ops[oi].cnt);
      Scope *dws = comp_scope_of(c, blk);
      LocalVar *dwl = (dws && bp0) ? scope_local(dws, bp0) : NULL;
      TyKind dwt = (dwl && dwl->type != TY_UNKNOWN) ? dwl->type : TY_POLY;
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "lv_%s = ", bp);
      if (dwt == TY_POLY) buf_puts(g_pre, vbuf);
      else { Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, dwt, vbuf, &ub); buf_puts(g_pre, ub.p ? ub.p : vbuf); free(ub.p); }
      buf_puts(g_pre, ";\n");
      int dwb = nt_ref(nt, blk, "body");
      int dwn = 0; const int *dwv = dwb >= 0 ? nt_arr(nt, dwb, "body", &dwn) : NULL;
      for (int k = 0; k < dwn - 1; k++) emit_stmt(c, dwv[k], g_pre, g_indent + 2);
      if (dwn >= 1) {
        Buf cb; memset(&cb, 0, sizeof cb);
        int svind = g_indent; g_indent += 2; emit_cond(c, dwv[dwn - 1], &cb); g_indent = svind;
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "if (%s) continue;\n", cb.p ? cb.p : "0");
        free(cb.p);
      }
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "_t%d = 0;\n", ops[oi].cnt);
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
      continue;
    }
    /* The running value is boxed (poly); the block param may infer a narrower
       type, so unbox to match its C type. A |k, v| header destructures a
       pair element (hash.lazy rides the pair array, #2845). */
    if (!emit_iter_autosplat(c, blk, TY_POLY_ARRAY, vbuf, g_indent + 1)) {
      Scope *bs = comp_scope_of(c, blk);
      LocalVar *plv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind pt = (plv && plv->type != TY_UNKNOWN) ? plv->type : TY_POLY;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "lv_%s = ", bp);
      if (pt == TY_POLY) buf_puts(g_pre, vbuf);
      else { Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, pt, vbuf, &ub); buf_puts(g_pre, ub.p ? ub.p : vbuf); free(ub.p); }
      buf_puts(g_pre, ";\n");
    }
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
    else if (ops[oi].kind == OP_FILTERMAP) {
      /* map, then drop a falsy result */
      Buf eb; memset(&eb, 0, sizeof eb);
      int svind = g_indent; g_indent += 1; emit_boxed(c, bb[bn - 1], &eb); g_indent = svind;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "%s = %s;\n", vbuf, eb.p ? eb.p : "sp_box_nil()"); free(eb.p);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (!sp_poly_truthy(%s)) continue;\n", vbuf);
    }
    else if (ops[oi].kind == OP_FLATMAP) {
      /* splice an array result element-wise into the result (a non-array
         result pushes as itself), honoring the terminal count, then skip the
         shared tail push. Only ever the last-applied stage (oi == 0). */
      Buf eb; memset(&eb, 0, sizeof eb);
      int svind = g_indent; g_indent += 1; emit_boxed(c, bb[bn - 1], &eb); g_indent = svind;
      int tfm = ++g_tmp, tfj = ++g_tmp;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tfm, eb.p ? eb.p : "sp_box_nil()", tfm); free(eb.p);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)) {\n", tfm, tfm);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_poly_length(_t%d)%s; _t%d++)\n", tfj, tfj, tfm, cbuf, tfj);
      emit_indent(g_pre, g_indent + 3);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_poly_arr_get(_t%d, _t%d));\n", tres, tfm, tfj);
      emit_indent(g_pre, g_indent + 1);
      buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent + 1);
      buf_puts(g_pre, "else ");
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d);\n", tres, tfm);
      emit_indent(g_pre, g_indent + 1);
      buf_puts(g_pre, "continue;\n");
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
  for (int oi = 0; oi < nops; oi++) {
    if (ops[oi].kind != OP_EACHSLICE) continue;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (sp_PolyArray_length(_t%d) > 0%s) sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n",
               ops[oi].cnt, cbuf, tres, ops[oi].cnt);
  }
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
  /* __enum_chain(arr): the desugared Enumerable#chain / Enumerator#+. The arg is
     already the concatenation of every source's #to_a, so the chain enumerator
     just snapshots it. #2545 / #2548 / #2551 */
  /* the desugared ENV snapshot (#2742) */
  if (recv < 0 && sp_streq(name, "__env_to_h") && argc == 0) {
    buf_puts(b, "sp_env_to_h()");
    return 1;
  }
  if (recv < 0 && sp_streq(name, "__enum_chain") && argc == 1) {
    buf_puts(b, "sp_enum_chain_new("); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* ---- Complex / Rational value types ---- */
  /* Kernel#Complex(re[, im]): a Float argument marks its component
     Float-classed so rendering and abs/abs2 keep CRuby's classes. */
  /* Complex takes 1..2 arguments: 0 or >2 raises ArgumentError (#2576, #2574) */
  if (recv < 0 && sp_streq(name, "Complex") && (argc == 0 || argc > 2)) {
    buf_puts(b, "({ ");
    for (int a = 0; a < argc; a++) { buf_puts(b, "(void)("); emit_expr(c, argv[a], b); buf_puts(b, "); "); }
    buf_printf(b, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given %d, expected 1..2)\");"
                  " (sp_Complex){0, 0, 0}; })", argc);
    return 1;
  }
  if (recv < 0 && sp_streq(name, "Complex") && argc >= 1) {
    /* Complex("2+3i"): parse like String#to_c */
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
      Buf sb = expr_buf(c, argv[0]);
      buf_printf(b, "sp_str_to_c(%s)", sb.p ? sb.p : "\"\"");
      free(sb.p);
      return 1;
    }
    /* a nil component can't be converted to Complex (CRuby raises TypeError);
       the general float construction below would silently read it as 0.0 */
    if (comp_ntype(c, argv[0]) == TY_NIL ||
        (argc >= 2 && comp_ntype(c, argv[1]) == TY_NIL)) {
      buf_puts(b, "({ ");
      for (int a = 0; a < argc; a++) { buf_puts(b, "(void)("); emit_expr(c, argv[a], b); buf_puts(b, "); "); }
      buf_puts(b, "sp_raise_cls(\"TypeError\", \"can't convert nil into Complex\");"
                  " (sp_Complex){0, 0, 0}; })");
      return 1;
    }
    int re_rat = comp_ntype(c, argv[0]) == TY_RATIONAL;
    int im_rat = argc >= 2 && comp_ntype(c, argv[1]) == TY_RATIONAL;
    int fl = (comp_ntype(c, argv[0]) == TY_FLOAT || re_rat ? 1 : 0) |
             (argc >= 2 && (comp_ntype(c, argv[1]) == TY_FLOAT || im_rat) ? 2 : 0);
    buf_puts(b, "((sp_Complex){");
    buf_puts(b, re_rat ? "sp_rational_to_f(" : "(mrb_float)(");
    emit_expr(c, argv[0], b);
    buf_puts(b, "), ");
    buf_puts(b, im_rat ? "sp_rational_to_f(" : "(mrb_float)(");
    if (argc >= 2) emit_expr(c, argv[1], b);
    else buf_puts(b, "0");
    buf_printf(b, "), %d})", fl);
    return 1;
  }
  if (recv < 0 && sp_streq(name, "Rational") && (argc == 1 || argc == 2)) {
    /* Rational(String): parse "n", "n/d", or "n.d" like String#to_r rather than
       reading the string pointer as an integer. */
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
      Buf sb = expr_buf(c, argv[0]);
      buf_printf(b, "sp_str_to_r(%s)", sb.p ? sb.p : "\"\"");
      free(sb.p);
      return 1;
    }
    /* Rational(Float) is the exact value of the double (5/2 for 2.5), not the
       truncating int cast; a Rational passes through unchanged. */
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
      Buf sb = expr_buf(c, argv[0]);
      buf_printf(b, "sp_float_to_rational(%s)", sb.p ? sb.p : "0");
      free(sb.p);
      return 1;
    }
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
      emit_expr(c, argv[0], b);
      return 1;
    }
    /* Rational(a, b) with a Float or Rational operand: the exact quotient of
       the two values (sp_rational_div raises ZeroDivisionError at b == 0). */
    if (argc == 2 && (comp_ntype(c, argv[0]) == TY_FLOAT || comp_ntype(c, argv[1]) == TY_FLOAT ||
                      comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
      buf_puts(b, "sp_rational_div(");
      emit_rat_coerce(c, argv[0], b);
      buf_puts(b, ", ");
      emit_rat_coerce(c, argv[1], b);
      buf_puts(b, ")");
      return 1;
    }
    if (argc == 2) {
      /* a zero denominator raises ZeroDivisionError (CRuby), not (n/0). Both
         arguments are evaluated first, in order. Render each into a local Buf so
         a complex argument's prelude is hoisted whole to g_pre, not spliced into
         this line when b is g_pre. */
      int tn = ++g_tmp, td = ++g_tmp;
      Buf nb = expr_buf(c, argv[0]);
      Buf db = expr_buf(c, argv[1]);
      buf_printf(b, "({ mrb_int _t%d = (mrb_int)(%s); mrb_int _t%d = (mrb_int)(%s);"
                    " if (_t%d == 0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                    " sp_rational_new(_t%d, _t%d); })",
                 tn, nb.p ? nb.p : "0", td, db.p ? db.p : "0", td, tn, td);
      free(nb.p); free(db.p);
      return 1;
    }
    {
      Buf sb = expr_buf(c, argv[0]);
      buf_printf(b, "sp_rational_new((mrb_int)(%s), (mrb_int)(1))", sb.p ? sb.p : "0");
      free(sb.p);
      return 1;
    }
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
      buf_printf(b, ", %d)", comp_ntype(c, argv[0]) == TY_FLOAT ? 1 : 0);
      return 1;
    }
    /* Complex.rect(re[, im]) / .rectangular: the component-pair constructor. */
    if (rrty && sp_streq(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Complex") &&
        (sp_streq(name, "rect") || sp_streq(name, "rectangular")) && argc >= 1 && argc <= 2) {
      int fl = (comp_ntype(c, argv[0]) == TY_FLOAT ? 1 : 0) |
               (argc == 2 && comp_ntype(c, argv[1]) == TY_FLOAT ? 2 : 0);
      buf_puts(b, "((sp_Complex){(mrb_float)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), (mrb_float)(");
      if (argc == 2) emit_expr(c, argv[1], b);
      else buf_puts(b, "0");
      buf_printf(b, "), %d})", fl);
      return 1;
    }
    TyKind crt = comp_ntype(c, recv);
    if (crt == TY_COMPLEX) {
      /* real/imaginary return the component with its CRuby class (Integer for
         an Integer-classed component), so box to poly via comp_v. */
      if (sp_streq(name, "real")) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_complex_comp_v(_t%d.re, _t%d.fl & SP_CPLX_RE_F); })", t, t);
        return 1;
      }
      if (sp_streq(name, "imaginary") || sp_streq(name, "imag")) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_complex_comp_v(_t%d.im, _t%d.fl & SP_CPLX_IM_F); })", t, t);
        return 1;
      }
      if (sp_streq(name, "conjugate") || sp_streq(name, "conj")) { buf_puts(b, "sp_complex_conjugate("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      /* abs/abs2 box to poly: the CRuby class depends on the component classes
         (Integer via the zero-component shortcut / all-Integer abs2). */
      if ((sp_streq(name, "abs") || sp_streq(name, "magnitude")) && argc == 0) { buf_puts(b, "sp_complex_abs_v("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "abs2") && argc == 0) { buf_puts(b, "sp_complex_abs2_v("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; atan2(_t%d.im, _t%d.re); })", t, t);
        return 1;
      }
      /* instance #polar ([abs, arg]) and #rect ([re, im]): poly pairs, since
         each element's class follows its component. */
      if (sp_streq(name, "polar") && argc == 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " sp_PolyArray_push(_t%d, sp_complex_abs_v(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float(atan2(_t%d.im, _t%d.re))); _t%d; })",
                   o, o, t, o, t, t, o);
        return 1;
      }
      if (sp_streq(name, "rectangular") || sp_streq(name, "rect")) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " sp_PolyArray_push(_t%d, sp_complex_comp_v(_t%d.re, _t%d.fl & SP_CPLX_RE_F));"
                      " sp_PolyArray_push(_t%d, sp_complex_comp_v(_t%d.im, _t%d.fl & SP_CPLX_IM_F)); _t%d; })",
                   o, o, t, t, o, t, t, o);
        return 1;
      }
      if (sp_streq(name, "-@") && argc == 0) { buf_puts(b, "sp_complex_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "+@") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if ((sp_streq(name, "to_c")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "to_s")) { buf_puts(b, "sp_complex_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "inspect")) { buf_puts(b, "sp_complex_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      TyKind cxa = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      int cx_ok = cxa == TY_COMPLEX || cxa == TY_INT || cxa == TY_FLOAT ||
                  cxa == TY_RATIONAL || cxa == TY_POLY;
      /* Dividing a Complex by a real scalar divides each component: a Float
         divisor yields Infinity at 0 (IEEE), an Integer divisor raises
         ZeroDivisionError at 0 (integer rules). The conjugate-formula
         sp_complex_div boxes the real to c+0i and produces NaN at c==0. */
      if (argc == 1 && sp_streq(name, "/") && cxa == TY_FLOAT) {
        buf_puts(b, "sp_complex_div_real("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      if (argc == 1 && sp_streq(name, "/") && cxa == TY_INT) {
        buf_puts(b, "sp_complex_div_int("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      if (cx_ok && argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") ||
                                 sp_streq(name, "*") || sp_streq(name, "/") ||
                                 sp_streq(name, "quo"))) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_complex_%s(", fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_complex_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (argc == 1 && sp_streq(name, "**") && cxa == TY_INT) {
        buf_puts(b, "sp_complex_pow("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      if (argc == 1 && sp_streq(name, "**") && (cxa == TY_FLOAT || cxa == TY_COMPLEX)) {
        buf_puts(b, "sp_complex_pow_c("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_complex_coerce(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* Complex has no modulo -> NoMethodError, not a compile abort (#2618) */
      if (argc == 1 && (sp_streq(name, "%") || sp_streq(name, "modulo"))) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_expr(c, argv[0], b);
        buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of Complex\"), (sp_Complex){0,0,0}))", name);
        return 1;
      }
      /* to_i/to_f/to_r require a zero imaginary part (RangeError otherwise);
         numerator/denominator model the Integer-component case (den 1). */
      if ((sp_streq(name, "to_i") || sp_streq(name, "to_int")) && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Integer\"); (mrb_int)_t%d.re; })", t, t);
        return 1;
      }
      if (sp_streq(name, "to_f") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Float\"); _t%d.re; })", t, t);
        return 1;
      }
      if (sp_streq(name, "to_r") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Rational\"); sp_float_to_rational(_t%d.re); })", t, t);
        return 1;
      }
      if (sp_streq(name, "<=>") && argc == 1 &&
          (cxa == TY_COMPLEX || cxa == TY_INT || cxa == TY_FLOAT)) {
        int ta3 = ++g_tmp, tb3 = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", ta3); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Complex _t%d = ", tb3); emit_complex_coerce(c, argv[0], b);
        buf_printf(b, "; (_t%d.im == 0.0 && _t%d.im == 0.0)"
                      " ? (_t%d.re < _t%d.re ? (mrb_int)-1 : _t%d.re > _t%d.re ? (mrb_int)1 : (mrb_int)0)"
                      " : SP_INT_NIL; })",
                   ta3, tb3, ta3, tb3, ta3, tb3);
        return 1;
      }
      if (sp_streq(name, "zero?") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; (_t%d.re == 0.0 && _t%d.im == 0.0); })", t, t);
        return 1;
      }
      if (sp_streq(name, "nonzero?") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; (_t%d.re != 0.0 || _t%d.im != 0.0) ? sp_box_complex(_t%d) : sp_box_nil(); })", t, t, t);
        return 1;
      }
      if ((sp_streq(name, "real?") || sp_streq(name, "integer?")) && argc == 0) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
        return 1;
      }
      if (sp_streq(name, "finite?") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; (isfinite(_t%d.re) && isfinite(_t%d.im)); })", t, t);
        return 1;
      }
      if (sp_streq(name, "infinite?") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; (isinf(_t%d.re) || isinf(_t%d.im)) ? (mrb_int)1 : SP_INT_NIL; })", t, t);
        return 1;
      }
      if (sp_streq(name, "eql?") && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
        int t = ++g_tmp, u = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Complex _t%d = ", u); emit_expr(c, argv[0], b);
        buf_printf(b, "; (_t%d.re == _t%d.re && _t%d.im == _t%d.im && _t%d.fl == _t%d.fl); })",
                   t, u, t, u, t, u);
        return 1;
      }
      /* rationalize takes an optional eps argument (ignored -- a Complex with a
         zero imaginary part rationalizes its real part exactly) (#2556) */
      if (sp_streq(name, "rationalize") && (argc == 0 || argc == 1)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Complex _t%d = ", t); emit_expr(c, recv, b);
        if (argc == 1) { buf_puts(b, "; (void)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_printf(b, "; if (_t%d.im != 0.0) sp_raise_cls(\"RangeError\", \"can't convert into Rational\"); sp_float_to_rational(_t%d.re); })", t, t);
        return 1;
      }
      if (sp_streq(name, "numerator") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "denominator") && argc == 0) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)1)");
        return 1;
      }
      if (sp_streq(name, "fdiv") && argc == 1 && (cxa == TY_INT || cxa == TY_FLOAT)) {
        buf_puts(b, "sp_complex_div_real("); emit_expr(c, recv, b);
        buf_puts(b, ", (mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      /* fdiv by a Complex is ordinary complex division in floats (#2555) */
      if (sp_streq(name, "fdiv") && argc == 1 && cxa == TY_COMPLEX) {
        buf_puts(b, "sp_complex_div("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "coerce") && argc == 1 &&
          (cxa == TY_INT || cxa == TY_FLOAT || cxa == TY_COMPLEX || cxa == TY_RATIONAL)) {
        int tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_complex(", tp, tp, tp);
        emit_complex_coerce(c, argv[0], b);
        buf_printf(b, ")); sp_PolyArray_push(_t%d, sp_box_complex(", tp);
        emit_expr(c, recv, b);
        buf_printf(b, ")); _t%d; })", tp);
        return 1;
      }
      if (cx_ok && argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!="))) {
        buf_printf(b, "(%ssp_complex_eq(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b); buf_puts(b, ", "); emit_complex_coerce(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      /* Complex == a non-numeric value is always false (!= true); the argument
         still evaluates for its side effects (#2557). */
      if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!="))) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_boxed(c, argv[0], b);
        buf_printf(b, "), %d)", name[0] == '!' ? 1 : 0);
        return 1;
      }
      /* eql? / equal? on the unboxed Complex value: component equality when
         the argument is a Complex (the struct has no object identity; a
         self-reference compares equal, matching the common x.equal?(x)
         probe), constant false for any other argument type. */
      if (crt == TY_COMPLEX && argc == 1 &&
          (sp_streq(name, "eql?") || sp_streq(name, "equal?"))) {
        if (comp_ntype(c, argv[0]) == TY_COMPLEX) {
          buf_puts(b, "sp_complex_eq("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
        }
        return 1;
      }
    }
    /* Integer/Float <op> Complex: lift the scalar to re+0i. */
    if ((crt == TY_INT || crt == TY_FLOAT) && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
      if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_complex_%s(((sp_Complex){(mrb_float)(", fn); emit_expr(c, recv, b);
        buf_printf(b, "), 0, %d}), ", crt == TY_FLOAT ? 1 : 0); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "==") || sp_streq(name, "!=")) {
        buf_printf(b, "(%ssp_complex_eq(((sp_Complex){(mrb_float)(", name[0] == '!' ? "!" : ""); emit_expr(c, recv, b);
        buf_puts(b, "), 0}), "); emit_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
    }
    /* Proc#curry and curry application. */
    if (crt == TY_PROC && sp_streq(name, "curry") &&
        (argc == 0 || (argc == 1 && comp_ntype(c, argv[0]) == TY_INT))) {
      /* curry(n) fixes the arity explicitly; when it matches the proc's own
         arity (the common case) it behaves like the no-arg form, so the count
         is accepted and ignored. #2669 */
      buf_puts(b, "sp_curry_new("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* A curried proc reports as a lambda Proc: #arity -1 (variadic), #lambda?
       true, #class Proc. #2651 */
    if (crt == TY_CURRY && argc == 0 && sp_streq(name, "arity")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)-1)");
      return 1;
    }
    if (crt == TY_CURRY && argc == 0 && sp_streq(name, "lambda?")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), TRUE)");
      return 1;
    }
    if (crt == TY_CURRY && (sp_streq(name, "[]") || sp_streq(name, "call") || sp_streq(name, "()")) && argc >= 1) {
      /* The application that reaches the proc's arity realizes the curry to its
         (int) result; earlier applications return another curry. curry[a, b]
         chains one apply per argument. */
      int complete = 0; TyKind cret = TY_UNKNOWN;
      int realize = curry_apply_info(c, id, &complete, &cret) && complete;
      if (realize) buf_puts(b, cret == TY_INT ? "sp_curry_to_int(" : "sp_curry_realize_poly(");
      for (int k = 0; k < argc; k++) buf_puts(b, "sp_curry_apply(");
      emit_expr(c, recv, b);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", (mrb_int)(");
        emit_expr(c, argv[k], b);
        buf_puts(b, "))");
      }
      if (realize) buf_puts(b, ")");
      return 1;
    }
    if (crt == TY_INT && sp_streq(name, "quo") && argc == 1) {
      /* a Float argument makes quo a Float division (#2334); an int arg
         yields the exact Rational */
      if (comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "((mrb_float)(");
        emit_expr(c, recv, b); buf_puts(b, ") / (");
        emit_float_expr(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      int tq9 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = (mrb_int)(", tq9);
      emit_expr(c, argv[0], b);
      buf_printf(b, "); if (_t%d == 0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                    " sp_rational_new((mrb_int)(", tq9);
      emit_expr(c, recv, b);
      buf_printf(b, "), _t%d); })", tq9);
      return 1;
    }
    /* Float <op> Rational (either side): CRuby coerces to Float. Arithmetic
       stays float; comparisons are bool. */
    if (argc == 1 &&
        ((crt == TY_FLOAT && comp_ntype(c, argv[0]) == TY_RATIONAL) ||
         (crt == TY_RATIONAL && comp_ntype(c, argv[0]) == TY_FLOAT)) &&
        (is_arith_op(name) || is_cmp_op(name) || sp_streq(name, "==") ||
         sp_streq(name, "!=") || sp_streq(name, "quo") || sp_streq(name, "fdiv"))) {
      const char *cop = (sp_streq(name, "quo") || sp_streq(name, "fdiv")) ? "/" : name;
      int lft_rat = (crt == TY_RATIONAL);
      if (sp_streq(name, "%")) {
        /* C's % is integer-only: modulo via fmod, floor-adjusted like Ruby */
        buf_puts(b, "sp_fmod(");
        if (lft_rat) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        else emit_float_expr(c, recv, b);
        buf_puts(b, ", ");
        if (lft_rat) emit_float_expr(c, argv[0], b);
        else { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "**")) {
        buf_puts(b, "pow(");
        if (lft_rat) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        else emit_float_expr(c, recv, b);
        buf_puts(b, ", ");
        if (lft_rat) emit_float_expr(c, argv[0], b);
        else { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
        return 1;
      }
      buf_puts(b, "((");
      if (lft_rat) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_float_expr(c, recv, b);
      buf_printf(b, ") %s (", cop);
      if (lft_rat) emit_float_expr(c, argv[0], b);
      else { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, "))");
      return 1;
    }
    if (crt == TY_FLOAT && sp_streq(name, "quo") && argc == 1) {
      /* Float#quo == / (float division) */
      buf_puts(b, "((");
      emit_expr(c, recv, b); buf_puts(b, ") / (mrb_float)(");
      emit_float_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    /* Integer ** <negative literal>: a Rational in CRuby (2 ** -2 == (1/4)).
       The non-literal-exponent form types poly and resolves in sp_poly_pow. */
    if (crt == TY_INT && argc == 1 && sp_streq(name, "**") &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
        !nt_str(nt, argv[0], "bigval") && nt_int(nt, argv[0], "value", 0) < 0) {
      buf_puts(b, "sp_rational_pow(sp_rational_new((mrb_int)(");
      emit_expr(c, recv, b);
      buf_printf(b, "), 1), %lldLL)", (long long)nt_int(nt, argv[0], "value", 0));
      return 1;
    }
    /* Integer ** Complex = the complex power (real base, complex exponent). */
    if (crt == TY_INT && argc == 1 && sp_streq(name, "**") &&
        comp_ntype(c, argv[0]) == TY_COMPLEX) {
      buf_puts(b, "sp_real_pow_complex((mrb_float)("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Integer ** Rational is a Float in Spinel (an integer-valued exponent
       would be an exact Rational in CRuby; kept Float by design, matching
       Rational ** Rational). */
    if (crt == TY_INT && argc == 1 && sp_streq(name, "**") &&
        comp_ntype(c, argv[0]) == TY_RATIONAL) {
      buf_puts(b, "pow((double)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    /* fdiv converts its argument to Float, which a Complex can't be (RangeError);
       Integer#div with a Complex is a NoMethodError (#2619). */
    if (crt == TY_INT && argc == 1 && sp_streq(name, "fdiv") &&
        comp_ntype(c, argv[0]) == TY_COMPLEX) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"can't convert Complex into Float\"), 0.0))");
      return 1;
    }
    if (crt == TY_INT && argc == 1 && sp_streq(name, "div") &&
        comp_ntype(c, argv[0]) == TY_COMPLEX) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method 'div' for an instance of Complex\"), 0))");
      return 1;
    }
    /* Integer#fdiv(Rational) is the float quotient; #div(Rational) is the floor. */
    if (crt == TY_INT && argc == 1 && sp_streq(name, "fdiv") &&
        comp_ntype(c, argv[0]) == TY_RATIONAL) {
      buf_puts(b, "((mrb_float)("); emit_expr(c, recv, b);
      buf_puts(b, ") / sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if (crt == TY_INT && argc == 1 && sp_streq(name, "div") &&
        comp_ntype(c, argv[0]) == TY_RATIONAL) {
      buf_puts(b, "sp_rational_floor_i(sp_rational_div(sp_rational_new((mrb_int)(");
      emit_expr(c, recv, b); buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    /* An Integer viewed as a Rational: numerator is self, denominator is 1. */
    if (crt == TY_INT && sp_streq(name, "numerator") && argc == 0) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    if (crt == TY_INT && sp_streq(name, "denominator") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return 1;
    }
    if (crt == TY_INT && (sp_streq(name, "to_r") ||
        (sp_streq(name, "rationalize") && argc <= 1)) && argc <= 1) {
      buf_puts(b, "sp_rational_new((mrb_int)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return 1;
    }
    /* n.to_c is Complex(n, 0) for an Integer or Float receiver. */
    if ((crt == TY_INT || crt == TY_FLOAT) && sp_streq(name, "to_c") && argc == 0) {
      buf_puts(b, "((sp_Complex){(mrb_float)("); emit_expr(c, recv, b);
      buf_printf(b, "), 0, %d})", crt == TY_FLOAT ? 1 : 0); return 1;
    }
    if (crt == TY_RATIONAL) {
      if (sp_streq(name, "numerator"))   { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").num"); return 1; }
      if (sp_streq(name, "denominator")) { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, ").den"); return 1; }
      if (sp_streq(name, "to_s")) { buf_puts(b, "sp_rational_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "inspect")) { buf_puts(b, "sp_rational_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "to_f")) && argc == 0) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
           (sp_streq(name, "truncate") && argc == 0))) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num / ("); emit_expr(c, recv, b); buf_puts(b, ").den)"); return 1; }
      if (sp_streq(name, "round") && argc == 0) { buf_puts(b, "sp_rational_round_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "floor") && argc == 0) { buf_puts(b, "sp_rational_floor_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "ceil") && argc == 0)  { buf_puts(b, "sp_rational_ceil_i(");  emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "zero?") && argc == 0)     { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num == 0)"); return 1; }
      if (sp_streq(name, "positive?") && argc == 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num > 0)"); return 1; }
      if (sp_streq(name, "negative?") && argc == 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num < 0)"); return 1; }
      /* Numeric predicates: a Rational is a finite, non-Integer real (#2562) */
      if ((sp_streq(name, "finite?") || sp_streq(name, "real?")) && argc == 0)
        { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), TRUE)"); return 1; }
      if (sp_streq(name, "integer?") && argc == 0)
        { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), FALSE)"); return 1; }
      if (sp_streq(name, "infinite?") && argc == 0)
        { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SP_INT_NIL)"); return 1; }
      if (sp_streq(name, "nonzero?") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; _t%d.num != 0 ? sp_box_rational(_t%d) : sp_box_nil(); })", t, t);
        return 1;
      }
      /* Complex/real-projection methods on a real Rational (#2561) */
      if ((sp_streq(name, "real") || sp_streq(name, "conjugate") || sp_streq(name, "conj")) && argc == 0)
        { emit_expr(c, recv, b); return 1; }
      if ((sp_streq(name, "imaginary") || sp_streq(name, "imag")) && argc == 0)
        { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)0)"); return 1; }
      if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0)
        { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ").num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))"); return 1; }
      if ((sp_streq(name, "abs2")) && argc == 0)
        { int t = ++g_tmp; buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b); buf_printf(b, "; sp_rational_mul(_t%d, _t%d); })", t, t); return 1; }
      if (sp_streq(name, "magnitude") && argc == 0)
        { buf_puts(b, "sp_rational_abs("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "to_c") && argc == 0)
        { buf_puts(b, "((sp_Complex){sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, "), 0, 1})"); return 1; }
      /* Rational#i -> Complex(0, self): the imaginary part takes the Rational's
         float projection (spinel's Complex is float-backed), so it renders
         "(0+0.75i)" where CRuby prints "(0+(3/4)*i)". #2706 */
      if (sp_streq(name, "i") && argc == 0)
        { buf_puts(b, "((sp_Complex){0.0, sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, "), 2})"); return 1; }
      if ((sp_streq(name, "rectangular") || sp_streq(name, "rect")) && argc == 0) {
        int t = ++g_tmp, ta = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_rational(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_int(0)); _t%d; })", ta, ta, ta, t, ta, ta);
        return 1;
      }
      if (sp_streq(name, "polar") && argc == 0) {
        int t = ++g_tmp, ta = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_rational(sp_rational_abs(_t%d)));"
                      " sp_PolyArray_push(_t%d, _t%d.num < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0));"
                      " _t%d; })", ta, ta, ta, t, ta, t, ta);
        return 1;
      }
      /* coerce(n): [n as Rational, self] boxed pair */
      if (sp_streq(name, "coerce") && argc == 1 &&
          (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_RATIONAL)) {
        int tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_rational(", tp, tp, tp);
        if (comp_ntype(c, argv[0]) == TY_INT) {
          buf_puts(b, "sp_rational_new(");
          emit_expr(c, argv[0], b);
          buf_puts(b, ", 1)");
        }
        else emit_expr(c, argv[0], b);
        buf_printf(b, ")); sp_PolyArray_push(_t%d, sp_box_rational(", tp);
        emit_expr(c, recv, b);
        buf_printf(b, ")); _t%d; })", tp);
        return 1;
      }
      /* coerce against a Float converts both operands to Float (#2568) */
      if (sp_streq(name, "coerce") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        int tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_float(", tp, tp, tp);
        emit_expr(c, argv[0], b);
        buf_printf(b, ")); sp_PolyArray_push(_t%d, sp_box_float(sp_rational_to_f(", tp);
        emit_expr(c, recv, b);
        buf_printf(b, "))); _t%d; })", tp);
        return 1;
      }
      /* % / modulo / remainder / divmod against a Rational or Integer operand */
      if ((sp_streq(name, "%") || sp_streq(name, "modulo") ||
           sp_streq(name, "remainder") || sp_streq(name, "divmod")) && argc == 1 &&
          (comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[0]) == TY_INT)) {
        int is_int_arg = comp_ntype(c, argv[0]) == TY_INT;
        if (sp_streq(name, "divmod")) {
          int ta2 = ++g_tmp, tb2 = ++g_tmp, tq2 = ++g_tmp, tp2 = ++g_tmp;
          buf_printf(b, "({ sp_Rational _t%d = ", ta2); emit_expr(c, recv, b);
          buf_printf(b, "; sp_Rational _t%d = ", tb2);
          if (is_int_arg) { buf_puts(b, "sp_rational_new("); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
          else emit_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = sp_rational_idiv(_t%d, _t%d);"
                        " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                        " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                        " sp_PolyArray_push(_t%d, sp_box_rational(sp_rational_mod(_t%d, _t%d)));"
                        " _t%d; })",
                     tq2, ta2, tb2, tp2, tp2, tp2, tq2, tp2, ta2, tb2, tp2);
        }
        else {
          const char *rfn = name[0] == 'r' ? "sp_rational_rem" : "sp_rational_mod";
          buf_printf(b, "%s(", rfn); emit_expr(c, recv, b); buf_puts(b, ", ");
          if (is_int_arg) { buf_puts(b, "sp_rational_new("); emit_expr(c, argv[0], b); buf_puts(b, ", 1)"); }
          else emit_expr(c, argv[0], b);
          buf_puts(b, ")");
        }
        return 1;
      }
      /* divmod / % / modulo / remainder against a Float: compute in floats,
         [Integer quotient, Float remainder] for divmod (#2595) */
      if ((sp_streq(name, "%") || sp_streq(name, "modulo") ||
           sp_streq(name, "remainder") || sp_streq(name, "divmod")) && argc == 1 &&
          comp_ntype(c, argv[0]) == TY_FLOAT) {
        if (sp_streq(name, "divmod")) {
          int tx = ++g_tmp, tf = ++g_tmp, tq = ++g_tmp, tp = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = sp_rational_to_f(", tx); emit_expr(c, recv, b);
          buf_printf(b, "); mrb_float _t%d = ", tf); emit_float_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = (mrb_int)floor(_t%d / _t%d);"
                        " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                        " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                        " sp_PolyArray_push(_t%d, sp_box_float(_t%d - (mrb_float)_t%d * _t%d));"
                        " _t%d; })", tq, tx, tf, tp, tp, tp, tq, tp, tx, tq, tf, tp);
          return 1;
        }
        buf_puts(b, name[0] == 'r' ? "fmod(sp_rational_to_f(" : "sp_fmod(sp_rational_to_f(");
        emit_expr(c, recv, b); buf_puts(b, "), ");
        emit_float_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* Rational ** Rational computes as floats (CRuby; a negative base would
         be Complex, out of the value model -- it yields NaN here) */
      if (sp_streq(name, "**") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
        buf_puts(b, "pow(sp_rational_to_f(");
        emit_expr(c, recv, b);
        buf_puts(b, "), sp_rational_to_f(");
        emit_expr(c, argv[0], b);
        buf_puts(b, "))");
        return 1;
      }
      /* round/truncate/floor/ceil with a literal precision: nd > 0 keeps a
         Rational, nd <= 0 realizes the Integer value (.num of the den-1 result). */
      if ((sp_streq(name, "round") || sp_streq(name, "truncate") ||
           sp_streq(name, "floor") || sp_streq(name, "ceil")) && argc == 1 &&
          nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode")) {
        long long nd = nt_int(nt, argv[0], "value", 0);
        const char *fn = name[0] == 'r' ? "round"
                       : name[0] == 't' ? "truncate"
                       : name[0] == 'f' ? "floor" : "ceil";
        buf_printf(b, "%ssp_rational_%s_prec(", nd > 0 ? "" : "(", fn);
        emit_expr(c, recv, b);
        buf_printf(b, ", %lld)%s", nd, nd > 0 ? "" : ".num)");
        return 1;
      }
      /* Non-literal precision: the result class depends on the runtime value
         (Rational for nd > 0, Integer otherwise), so box to poly and choose at
         runtime. Both operands are value types -- nothing to GC-root. */
      if ((sp_streq(name, "round") || sp_streq(name, "truncate") ||
           sp_streq(name, "floor") || sp_streq(name, "ceil")) && argc == 1) {
        const char *fn = name[0] == 'r' ? "round" : name[0] == 't' ? "truncate"
                       : name[0] == 'f' ? "floor" : "ceil";
        int tr = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = (mrb_int)(", tn); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d > 0 ? sp_box_rational(sp_rational_%s_prec(_t%d, _t%d))"
                      " : sp_box_int(sp_rational_%s_prec(_t%d, _t%d).num); })",
                   tn, fn, tr, tn, fn, tr, tn);
        return 1;
      }
      if (sp_streq(name, "-@") && argc == 0) { buf_puts(b, "sp_rational_neg("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      if (sp_streq(name, "+@") && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "abs") && argc == 0) { buf_puts(b, "sp_rational_abs("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      TyKind rat = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      /* Rational <op> Complex computes in floats (same divergence note as
         emit_complex_coerce) */
      if (rat == TY_COMPLEX && argc == 1 &&
          (sp_streq(name, "+") || sp_streq(name, "-") ||
           sp_streq(name, "*") || sp_streq(name, "/"))) {
        const char *cfn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        buf_printf(b, "sp_complex_%s(((sp_Complex){sp_rational_to_f(", cfn);
        emit_expr(c, recv, b);
        buf_puts(b, "), 0, 1}), ");
        emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* Only Integer/Rational/Float operands are modeled (a poly operand --
         e.g. a Rational read out of a poly array, which has no box form yet --
         falls through to the generic path rather than miscompiling). */
      int rat_ok = rat == TY_RATIONAL || rat == TY_INT || rat == TY_FLOAT;
      /* arithmetic against another Rational or an Integer yields a Rational;
         against a Float, coerce self to float (CRuby semantics). */
      if (rat_ok && argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") ||
                        sp_streq(name, "*") || sp_streq(name, "/") || sp_streq(name, "quo"))) {
        const char *fn = name[0] == '+' ? "add" : name[0] == '-' ? "sub" : name[0] == '*' ? "mul" : "div";
        if (rat == TY_FLOAT) {
          const char *op = name[0] == 'q' ? "/" : name;  /* quo against a Float divides */
          buf_puts(b, "(sp_rational_to_f("); emit_expr(c, recv, b); buf_printf(b, ") %s ", op); emit_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        buf_printf(b, "sp_rational_%s(", fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* fdiv: float division regardless of operand kind. */
      if (rat_ok && argc == 1 && sp_streq(name, "fdiv")) {
        buf_puts(b, "(sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, ") / ");
        if (rat == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_float_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* div: floor division to an Integer (CRuby Numeric#div). */
      if (rat_ok && argc == 1 && sp_streq(name, "div")) {
        if (rat == TY_FLOAT) {
          buf_puts(b, "((mrb_int)floor(sp_rational_to_f("); emit_expr(c, recv, b);
          buf_puts(b, ") / ("); emit_expr(c, argv[0], b); buf_puts(b, ")))");
          return 1;
        }
        buf_puts(b, "sp_rational_idiv("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (rat_ok && argc == 1 && sp_streq(name, "**")) {
        if (rat == TY_INT) { buf_puts(b, "sp_rational_pow("); emit_expr(c, recv, b); buf_puts(b, ", (mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1; }
        buf_puts(b, "pow(sp_rational_to_f("); emit_expr(c, recv, b); buf_puts(b, "), "); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (rat_ok && argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") ||
                        sp_streq(name, "<=") || sp_streq(name, ">="))) {
        /* against a Float, compare by float value: coercing the Float to a
           Rational truncates it (1.5 -> 1/1) and compares wrong. */
        if (rat == TY_FLOAT) {
          buf_puts(b, "(sp_rational_to_f("); emit_expr(c, recv, b); buf_printf(b, ") %s ", name); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        buf_puts(b, "(sp_rational_cmp("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_printf(b, ") %s 0)", name);
        return 1;
      }
      if (rat_ok && argc == 1 && sp_streq(name, "<=>")) {
        if (rat == TY_FLOAT) {
          int tl = ++g_tmp, tr = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = sp_rational_to_f(", tl); emit_expr(c, recv, b);
          buf_printf(b, "); mrb_float _t%d = ", tr); emit_float_expr(c, argv[0], b);
          buf_printf(b, "; _t%d < _t%d ? -1 : (_t%d > _t%d ? 1 : 0); })", tl, tr, tl, tr);
          return 1;
        }
        buf_puts(b, "sp_rational_cmp("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* == / != / === (=== is case equality = value equality for a Numeric,
         #2564). A numeric argument compares by value; a non-numeric argument is
         never == a Rational -> false (#2572). */
      if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "==="))) {
        int neg = name[0] == '!';
        if (rat_ok) {
          if (rat == TY_FLOAT) {
            buf_puts(b, "(sp_rational_to_f("); emit_expr(c, recv, b);
            buf_printf(b, ") %s ", neg ? "!=" : "=="); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
            return 1;
          }
          buf_printf(b, "(%ssp_rational_eq(", neg ? "!" : ""); emit_expr(c, recv, b);
          buf_puts(b, ", "); emit_rat_coerce(c, argv[0], b); buf_puts(b, "))");
          return 1;
        }
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_boxed(c, argv[0], b);
        buf_printf(b, "), %d)", neg ? 1 : 0);
        return 1;
      }
      /* Comparable#between?/clamp via <=> (#2563). between? is bool; clamp with
         two Rational bounds returns a Rational (the receiver or a bound). */
      if (argc == 2 && sp_streq(name, "between?") &&
          (comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[0]) == TY_INT) &&
          (comp_ntype(c, argv[1]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_INT)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; (sp_rational_cmp(_t%d, ", t); emit_rat_coerce(c, argv[0], b);
        buf_printf(b, ") >= 0 && sp_rational_cmp(_t%d, ", t); emit_rat_coerce(c, argv[1], b);
        buf_puts(b, ") <= 0); })");
        return 1;
      }
      if (argc == 2 && sp_streq(name, "clamp") &&
          comp_ntype(c, argv[0]) == TY_RATIONAL && comp_ntype(c, argv[1]) == TY_RATIONAL) {
        int t = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Rational _t%d = ", ta); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_Rational _t%d = ", tb); emit_expr(c, argv[1], b);
        buf_printf(b, "; sp_rational_cmp(_t%d, _t%d) < 0 ? _t%d"
                      " : (sp_rational_cmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                   t, ta, ta, t, tb, tb, t);
        return 1;
      }
      /* eql? / equal? on the unboxed Rational value: component equality for
         a Rational argument (no object identity; see the Complex arm),
         constant false otherwise. */
      if (crt == TY_RATIONAL && argc == 1 &&
          (sp_streq(name, "eql?") || sp_streq(name, "equal?"))) {
        if (comp_ntype(c, argv[0]) == TY_RATIONAL) {
          buf_puts(b, "sp_rational_eq("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
        }
        return 1;
      }
    }
    /* Float % Rational: floor modulo in doubles (1.5 % (1/2r) is 0.0) */
    if (crt == TY_FLOAT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL &&
        (sp_streq(name, "%") || sp_streq(name, "modulo"))) {
      int tx = ++g_tmp, ty2 = ++g_tmp;
      buf_printf(b, "({ double _t%d = ", tx); emit_expr(c, recv, b);
      buf_printf(b, "; double _t%d = sp_rational_to_f(", ty2); emit_expr(c, argv[0], b);
      buf_printf(b, "); _t%d - _t%d * floor(_t%d / _t%d); })", tx, ty2, tx, ty2);
      return 1;
    }
    /* Integer <op> Rational: lift the Integer to n/1 (covers `2/3r`, `1 + r`). */
    if (crt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
      if (sp_streq(name, "%") || sp_streq(name, "modulo")) {
        buf_puts(b, "sp_rational_mod(sp_rational_new((mrb_int)("); emit_expr(c, recv, b);
        buf_puts(b, "), 1), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
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

/* Emit a `fetch`-miss KeyError raise carrying MRI's "key not found: <key>"
   text. The key node is boxed so sp_raise_key_not_found can inspect any key
   type; re-evaluating it on the (aborting) miss path is harmless. */
static void emit_key_not_found(Compiler *c, int key_node, Buf *b) {
  buf_puts(b, "sp_raise_key_not_found(");
  emit_boxed(c, key_node, b);
  buf_puts(b, ")");
}

/* Emit the else-branch of a poly-Hash `fetch` dispatched through the poly value
   switch: the caller's default (fetch(k, dflt)) or a KeyError raise (fetch(k)),
   coerced to the dispatch's result-temp representation (poly, or the scalar
   `trt`). `argv1` is the default-argument node (only read when argc == 2);
   `key_node` is the fetched key (argv[0]) for the KeyError message. */
static void emit_poly_fetch_absent(Compiler *c, int argc, const int *atmp, int argv1,
                                   int key_node, TyKind ret, TyKind trt, Buf *b) {
  if (argc == 2) {
    char dn[32]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
    if (ret == TY_POLY) emit_boxed_text(c, infer_type(c, argv1), dn, b);
    else buf_puts(b, dn);
  }
  else {
    buf_puts(b, "(");
    emit_key_not_found(c, key_node, b);
    buf_puts(b, ", ");
    buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
    buf_puts(b, ")");
  }
}

/* Names of the universal Object/Kernel predicates the poly switch supplies a
   builtin default arm for (eql?, equal?, is_a?, kind_of?, instance_of?,
   frozen?, nil?). Every value answers these, but the generic poly dispatch only
   emits a user `case` arm per class that defines the name -- a poly value that
   is a builtin scalar (or an object of a class without the override) otherwise
   fell through to "undefined method '<m>' for poly" at runtime, the dominant
   real gap under the ruby/spec harness (`x.should.eql?(y)` and friends). */
static int poly_pred_kind(const char *name, int argc) {
  if (!name) return 0;
  if (argc == 0) return (sp_streq(name, "frozen?") || sp_streq(name, "nil?")) ? 1 : 0;
  if (argc == 1) return (sp_streq(name, "eql?") || sp_streq(name, "equal?") ||
                         sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                         sp_streq(name, "instance_of?")) ? 1 : 0;
  return 0;
}

/* Emit the boolean VALUE of a universal predicate on a poly receiver already
   held in `tvref` (an sp_RbVal lvalue like "_t42"). `argref` is the BOXED
   argument expression for the binary forms (eql?/equal?); the class forms read
   their class straight from the call's argument node. Returns 1 on success.
   Consumed as the switch's default arm so builtin scalars and un-overridden
   objects answer these alongside the per-class user arms. */
static int emit_poly_pred_value(Compiler *c, int id, const char *tvref,
                                const char *argref, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  if (argc == 0 && sp_streq(name, "frozen?")) { buf_printf(b, "sp_poly_frozen(%s)", tvref); return 1; }
  if (argc == 0 && sp_streq(name, "nil?"))    { buf_printf(b, "sp_poly_nil_p(%s)", tvref); return 1; }
  if (argc == 1 && sp_streq(name, "eql?"))    { buf_printf(b, "sp_poly_eql(%s, %s)", tvref, argref); return 1; }
  if (argc == 1 && sp_streq(name, "equal?"))  { buf_printf(b, "sp_poly_equal(%s, %s)", tvref, argref); return 1; }
  int is_isa = argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?"));
  int is_iof = argc == 1 && sp_streq(name, "instance_of?");
  if (is_isa || is_iof) {
    int arg = argv[0];
    const char *cn = isa_const_name(nt, arg);
    if (cn) {
      if (is_iof) {   /* exact class match by name (builtin or user class) */
        buf_printf(b, "(strcmp(sp_poly_class_name(%s), \"%s\") == 0)", tvref, cn);
        return 1;
      }
      int target = comp_class_index(c, cn);   /* a user class in this program? */
      if (target >= 0) {   /* user-class target: chain check on the object cls_id */
        buf_printf(b, "(%s.tag == SP_TAG_OBJ && %s.cls_id >= 0 && "
                      "sp_class_le((sp_Class){%s.cls_id}, (sp_Class){%d}))",
                   tvref, tvref, tvref, target);
        return 1;
      }
      buf_printf(b, "sp_poly_kind_of_builtin(%s, \"%s\")", tvref, cn);   /* builtin target */
      return 1;
    }
    /* Runtime class value (a method param, a non-literal): resolve by the
       class's name at runtime. `argref` is the boxed class; when absent (the
       standalone caller), box the arg node here. */
    buf_printf(b, "sp_poly_is_a_dyn(%s, ", tvref);
    if (argref) buf_puts(b, argref);
    else emit_boxed(c, arg, b);
    buf_printf(b, ", %d)", is_iof ? 1 : 0);
    return 1;
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
    /* A class-tagged poly value answers these with its class name (#2656); only
       when no user class defines them, or that user method is the real target. */
    int is_class_named = (sp_streq(name, "name") || sp_streq(name, "to_s") ||
                          sp_streq(name, "inspect")) && !diag_user_defines(c, name);
    int is_pred = nt_ref(nt, id, "block") < 0 && poly_pred_kind(name, 0);
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0 || comp_reader_in_chain(c, k, name, NULL) ||
          (c->classes[k].is_native_class && comp_native_method_find(c, k, name, 0, 0) >= 0)) ncand++;
    if (ncand > 0 || is_lengthlike || is_pred || is_class_named) {
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
        buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %ssp_str_length(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ", tv, tr, bopen, tv, bclose);
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %s(mrb_int)sp_str_length(_t%d.v.s)%s; else ", tv, tr, bopen, tv, bclose);
      }
      /* a string/symbol-tagged poly value answers empty? directly (#1438) */
      if (is_empty) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %ssp_str_length(_t%d.v.s) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
        buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %sstrlen(sp_sym_to_s((sp_sym)_t%d.v.i)) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
      }
      /* a class-tagged poly value answers its name: `Base.subclasses` and
         `#ancestors` hand back boxed classes, so `.map { |c| c.name }` reaches
         here (#2656). The tag is checked ahead of the cls_id switch, because a
         boxed class carries the CLASS's id and would otherwise alias that
         user class's arm. Declined when a user class defines the method --
         then a user object is the likelier receiver and it must win. */
      if (is_class_named) {
        const char *sbopen = (ret == TY_POLY) ? "sp_box_str(" : "";
        const char *sbclose = (ret == TY_POLY) ? ")" : "";
        buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %ssp_class_val_name(_t%d)%s; else ",
                   tv, tr, sbopen, tv, sbclose);
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
        /* native (C-backed) class arm: dispatch a declared no-arg method to its
           C symbol on the cast receiver, coercing the result into the slot. */
        if (c->classes[k].is_native_class) {
          int nmi = comp_native_method_find(c, k, name, 0, 0);
          if (nmi >= 0) {
            NativeMethod *nmet = &c->native_methods[nmi];
            char nbuf[300];
            if (sp_streq(nmet->ret, "string?"))
              snprintf(nbuf, sizeof nbuf, "sp_box_nullable_str(%s((%s *)_t%d.v.p))", nmet->csym, c->classes[k].c_struct, tv);
            else
              snprintf(nbuf, sizeof nbuf, "%s((%s *)_t%d.v.p)", nmet->csym, c->classes[k].c_struct, tv);
            TyKind mret = native_spec_to_ty(nmet->ret);
            buf_printf(b, " case %d: ", k);
            if (mret == TY_NIL) buf_puts(b, nbuf);
            else {
              buf_printf(b, "_t%d = ", tr);
              if (ret == TY_POLY && mret != TY_POLY) emit_boxed_text(c, mret, nbuf, b);
              else if (ret != TY_POLY && mret == TY_POLY) emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, nbuf, b);
              else buf_puts(b, nbuf);
            }
            buf_puts(b, "; break;");
          }
          continue;
        }
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
          const char *_dcn = c->classes[defcls].c_name;
          char _dself[64];
          if (sp_streq(_dcn, "Integer") || sp_streq(_dcn, "Numeric")) snprintf(_dself, sizeof _dself, "_t%d.v.i", tv);
          else if (sp_streq(_dcn, "Float")) snprintf(_dself, sizeof _dself, "_t%d.v.f", tv);
          else if (sp_streq(_dcn, "String")) snprintf(_dself, sizeof _dself, "_t%d.v.s", tv);
          else if (sp_streq(_dcn, "Symbol")) snprintf(_dself, sizeof _dself, "(sp_sym)_t%d.v.i", tv);
          /* a by-value (value-type) class method takes self by value:
             dereference the boxed pointer instead of passing it (#2441) */
          else if (c->classes[defcls].is_value_type)
            snprintf(_dself, sizeof _dself, "*(sp_%s *)_t%d.v.p", _dcn, tv);
          else snprintf(_dself, sizeof _dself, "(sp_%s *)_t%d.v.p", _dcn, tv);
          buf_printf(&cb, "sp_%s_%s(%s", _dcn, mc(c->scopes[mi].name), _dself);
          if (c->scopes[mi].nparams > 0) {
            const char *saved_self = g_self;
            char selfpbuf[64];  /* stack-local: nested inlines each need their own receiver buffer */
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
          snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[rdcls].c_name, tv, rn3);
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn3);
          int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
          buf_printf(b, " case %d: _t%d = ", k, tr);
          if (ret == TY_POLY && ivt != TY_POLY) emit_boxed_text(c, ivt, fld, b);
          /* The slot is scalar (e.g. a length dispatch fixed to mrb_int) but
             this class's ivar widened to poly: coerce down. */
          else if (ret != TY_POLY && ivt == TY_POLY)
            emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, fld, b);
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
      /* A method reopened on Object applies to ANY receiver -- boxed scalar
         tags included; its generated self parameter is the boxed sp_RbVal
         itself -- so it forms the switch's DEFAULT arm rather than a
         per-class case. This also keeps the switch from being empty when no
         user class is instantiated: an empty switch left the result NULL and
         the next call on it segfaulted (ruby/spec's mspec `should` proxy on
         a case/when result -- 133 of the harness's ERROR examples). */
      int obj_default_done = 0;
      { int obj_cls = comp_class_index(c, "Object");
        if (obj_cls >= 0) {
          int obj_def = -1;
          int obj_mi = comp_method_in_chain(c, obj_cls, name, &obj_def);
          if (obj_mi >= 0 && obj_def == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
              scope_has_callable_symbol(c, obj_mi)) {
            char ocall[160];
            snprintf(ocall, sizeof ocall, "sp_Object_%s(_t%d)", mc(c->scopes[obj_mi].name), tv);
            buf_puts(b, " default: ");
            if (method_is_void(&c->scopes[obj_mi])) buf_puts(b, ocall);
            else {
              TyKind oslot = is_scalar_ret(ret) ? ret : TY_INT;
              buf_printf(b, "_t%d = ", tr);
              if (ret == TY_POLY && c->scopes[obj_mi].ret != TY_POLY)
                emit_boxed_text(c, c->scopes[obj_mi].ret, ocall, b);
              else if (ret != TY_POLY && c->scopes[obj_mi].ret == TY_POLY)
                emit_unbox_text(c, oslot, ocall, b);
              else buf_puts(b, ocall);
            }
            buf_puts(b, "; break;");
            obj_default_done = 1;
          }
        } }
      /* to_s / inspect are universal: a poly value that is a builtin scalar
         (int, float, string, ...) rather than one of the enumerated user
         classes still answers them. Without a default arm the result stayed
         the empty-string default, so `@x.to_s` on a poly-widened int printed
         blank. Route the fallthrough through the runtime poly converter. */
      if (!obj_default_done && (sp_streq(name, "to_s") || sp_streq(name, "inspect"))) {
        const char *pfn = sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect";
        buf_printf(b, " default: _t%d = ", tr);
        if (ret == TY_POLY) buf_printf(b, "sp_box_str(%s(_t%d))", pfn, tv);
        else buf_printf(b, "%s(_t%d)", pfn, tv);
        buf_puts(b, "; break;");
      }
      /* frozen?/nil? on a builtin-scalar (or un-overridden object) poly value:
         the switch default answers via the runtime predicate. */
      if (!obj_default_done && is_pred) {
        char tvref[24]; snprintf(tvref, sizeof tvref, "_t%d", tv);
        buf_printf(b, " default: _t%d = ", tr);
        if (ret == TY_POLY) { buf_puts(b, "sp_box_bool("); emit_poly_pred_value(c, id, tvref, NULL, b); buf_puts(b, ")"); }
        else emit_poly_pred_value(c, id, tvref, NULL, b);
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
                   (infer_type(c, argv[0]) == TY_STRING || infer_type(c, argv[0]) == TY_SYMBOL ||
                    infer_type(c, argv[0]) == TY_POLY || infer_type(c, argv[0]) == TY_UNKNOWN);
    int is_include = (sp_streq(name, "include?") || sp_streq(name, "member?") ||
                      sp_streq(name, "has_key?") || sp_streq(name, "key?")) && argc == 1;
    /* push/<</append on a poly value that is actually a builtin array: the
       array-mutate statement path skips it when a user class also defines the
       name (the value could be that object), so the switch needs a builtin-array
       arm or the append is silently dropped. sp_poly_shl handles every array
       kind; the user arms above cover the object case. */
    int is_push = (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1;
    /* delete(chars) with a string arg: the poly value may be a string even
       when a user class also defines `delete` (the bundled Set does), so the
       switch needs a TAG_STR pre-arm routing to String#delete (doom's
       `data[offset, 8].delete("\x00").upcase` WAD name fields). */
    int is_strdel = sp_streq(name, "delete") && argc == 1 &&
                    infer_type(c, argv[0]) == TY_STRING;
    int is_pred = nt_ref(nt, id, "block") < 0 && poly_pred_kind(name, argc);
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      /* Include if call supplies all required params (pad defaults / truncate extras) */
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    /* strftime on a poly value that is really a Time: a nilable Time
       (`created_at : Time?`) is held as a poly sp_RbVal, so `t.strftime(fmt)`
       would otherwise lower to the unresolved-call raise even when the value
       is a genuine Time. Give the switch a SP_BUILTIN_TIME arm so a real Time
       formats and nil/anything-else raises NoMethodError, matching CRuby
       (issue #2457, the family2 nilable value-method dispatch gap). Only when
       no user class defines strftime, so the default-raise arm is unambiguous. */
    int is_strftime = ncand == 0 && sp_streq(name, "strftime") && argc == 1 &&
                      infer_type(c, argv[0]) == TY_STRING;
    if (ncand > 0 || is_index || is_include || is_fetch || is_push || is_pred || is_strftime) {
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
      /* delete(chars) on a TAG_STR receiver: String#delete, boxed when the
         dispatch result stays poly. */
      if (is_strdel && (ret == TY_POLY || ret == TY_STRING)) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
        if (ret == TY_POLY) buf_printf(b, "sp_box_str(sp_str_delete(_t%d.v.s, _t%d))", tv, atmp[0]);
        else buf_printf(b, "sp_str_delete(_t%d.v.s, _t%d)", tv, atmp[0]);
        buf_puts(b, "; } else ");
      }
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
        /* String#[int]: a poly value that is really a String (e.g. a method
           with multiple return paths widened to poly) answers `[]` with the
           single character at the index, or nil. The cls_id switch below only
           covers SP_TAG_OBJ variants, so without this tag arm a String receiver
           fell through and returned the seed (nil/0). */
        if (ret == TY_POLY)
          buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_box_nullable_str(sp_str_char_at_or_nil(_t%d.v.s, %s)); } else ", tv, tr, tv, idxref);
        else if (ret == TY_STRING)
          buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_str_char_at_or_nil(_t%d.v.s, %s); } else ", tv, tr, tv, idxref);
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
          if (pc && ac && pt0 != at0 &&
              (pt0 == TY_STRING || at0 == TY_STRING ||
               /* pointer/scalar C-representation mismatch: e.g. an int-typed
                  param (bound by an unrelated poly==int) receiving a typed
                  object arg -- the raw pass would be a C int-conversion
                  error and semantic garbage; the arm cannot be this call's
                  real target shape */
               ty_is_object(pt0) != ty_is_object(at0))) {
            arm_key_incompat = 1; break;
          }
        }
        if (arm_key_incompat) continue;
        TyKind mret = c->scopes[mi].ret;
        int mnp = c->scopes[mi].nparams;
        Buf cb; memset(&cb, 0, sizeof cb);
        buf_printf(&cb, "sp_%s_%s((sp_%s *)_t%d.v.p", c->classes[defcls].c_name,
                   mc(c->scopes[mi].name), c->classes[defcls].c_name, tv);
        const char *saved_self = g_self;
        char selfpbuf2[64];  /* stack-local: nested inlines each need their own receiver buffer */
        snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_%s *)_t%d.v.p", c->classes[defcls].c_name, tv);
        int r_idx = c->scopes[mi].rest_idx;
        int npost = c->scopes[mi].npost_rest;
        int rest_end = argc - npost;   /* where the *rest collection stops */
        for (int a = 0; a < mnp; a++) {
          buf_puts(&cb, ", ");
          /* a *rest parameter collects the trailing call-site temps (evaluated
             once above) into a PolyArray; without this the raw scalar temp was
             passed straight into the sp_PolyArray* slot, a C type error at the
             poly-dispatch arm (issue #2457). */
          if (r_idx >= 0 && a == r_idx) {
            int rt2 = ++g_tmp;
            buf_printf(&cb, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rt2, rt2);
            for (int a2 = a; a2 < rest_end && a2 < argc; a2++) {
              char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a2]);
              Buf eb; memset(&eb, 0, sizeof eb);
              if (atmp_ty[a2] == TY_POLY) buf_puts(&eb, tn);
              else emit_boxed_text(c, atmp_ty[a2], tn, &eb);
              buf_printf(&cb, " sp_PolyArray_push(_t%d, %s);", rt2, eb.p ? eb.p : "sp_box_nil()");
              free(eb.p);
            }
            buf_printf(&cb, " _t%d; })", rt2);
            continue;
          }
          /* box the call-site arg if this candidate's parameter is poly;
             emit default for args beyond the call-site count (padding) */
          TyKind pt = TY_UNKNOWN;
          LocalVar *pv = scope_local(&c->scopes[mi], c->scopes[mi].pnames[a]);
          if (pv) pt = pv->type;
          /* post-*rest required params take from the tail of the call args */
          int src = (r_idx >= 0 && npost > 0 && a > r_idx) ? rest_end + (a - r_idx - 1) : a;
          if (src < argc) {
            TyKind at = atmp_ty[src];   /* the temp's actual type (poly for a nil/void arg) */
            char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[src]);
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
          buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = sp_frange_cover(*(sp_FloatRange *)_t%d.v.p, (mrb_float)_t%d); break;", tr, tv, atmp[0]);
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
             container cases. Typed arrays match only when the boxed arg's tag
             fits the element type (a Set difference against an Array literal
             reaches these; a mismatched tag is simply not a member). */
          buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = sp_range_include((sp_Range *)_t%d.v.p, sp_poly_to_i(_t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = sp_frange_cover(*(sp_FloatRange *)_t%d.v.p, sp_poly_to_f(_t%d)); break;", tr, tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = _t%d.tag == SP_TAG_INT && sp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d.v.i); break;", tr, atmp[0], tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = _t%d.tag == SP_TAG_FLT && sp_FloatArray_include((sp_FloatArray *)_t%d.v.p, _t%d.v.f); break;", tr, atmp[0], tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = _t%d.tag == SP_TAG_STR && sp_StrArray_include((sp_StrArray *)_t%d.v.p, _t%d.v.s); break;", tr, atmp[0], tv, atmp[0]);
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
      /* strftime on a poly value that is really a Time: format it; nil or any
         other runtime class raises NoMethodError as CRuby does. */
      if (is_strftime) {
        if (ret == TY_POLY)
          buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_box_str(sp_time_strftime(*(sp_Time *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
        else
          buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_time_strftime(*(sp_Time *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
        buf_printf(b, " default: sp_raise_cls(\"NoMethodError\", sp_nomethod_msg(\"strftime\", _t%d)); break;", tv);
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
          else if (is_fetch) { buf_puts(b, "("); emit_key_not_found(c, argv[0], b); buf_puts(b, ", "); buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt)); buf_puts(b, ")"); }
          else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
          buf_puts(b, "; break;");
        }
        /* the poly value may be a generic PolyPolyHash keyed by (boxed) strings
           -- a `to_h { |x| [x.name, x] }` result widened to poly, indexed by a
           string (doom's `@flats[anim_flat(name)]`). The STR_*_HASH arms above
           only match native-string-keyed storage, so box the string key and
           look it up in the poly-keyed storage. Only for `[]` (nil on miss);
           `fetch` keeps falling through to its default-seed (a bare get would
           drop the caller's supplied default). */
        /* `[]` returns nil on a miss; `fetch` must return the present value or
           fall back to its supplied default / raise KeyError -- so gate the get
           on a has_key check (a bare get would drop the caller's default and
           mistake a stored nil for absence). */
        if (is_aref || is_fetch) {
          char getx[220], hx[220];
          snprintf(getx, sizeof getx, "sp_PolyPolyHash_get((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(_t%d))", tv, atmp[0]);
          snprintf(hx, sizeof hx, "sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(_t%d))", tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = ", tr);
          if (is_fetch) buf_printf(b, "%s ? ", hx);
          if (ret == TY_POLY) buf_puts(b, getx);
          else emit_unbox_text(c, trt, getx, b);
          if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? argv[1] : -1, argv[0], ret, trt, b); }
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
        else if (is_fetch) { buf_puts(b, "("); emit_key_not_found(c, argv[0], b); buf_puts(b, ", "); buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt)); buf_puts(b, ")"); }
        else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value(trt));
        buf_puts(b, "; break;");
        /* a symbol key against generic poly-keyed storage: an empty `{}`
           literal boxes as PolyPolyHash, so a symbol-keyed [] / fetch must
           reach it too (the arm above only matches SymPolyHash) */
        {
          char getx2[220], hx2[220];
          snprintf(getx2, sizeof getx2, "sp_PolyPolyHash_get((sp_PolyPolyHash *)_t%d.v.p, sp_box_sym(_t%d))", tv, atmp[0]);
          snprintf(hx2, sizeof hx2, "sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, sp_box_sym(_t%d))", tv, atmp[0]);
          buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = ", tr);
          if (is_fetch) buf_printf(b, "%s ? ", hx2);
          if (ret == TY_POLY) buf_puts(b, getx2);
          else emit_unbox_text(c, trt, getx2, b);
          if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? argv[1] : -1, argv[0], ret, trt, b); }
          buf_puts(b, "; break;");
        }
      }
      /* a poly-keyed `[]` on a poly value that is actually a Hash: dispatch to
         the hash storage by the (boxed) poly key. The string/symbol-key arms
         above only fire for a statically-typed key; a key that stayed poly
         (a method param, e.g. `@textures[name]` in doom's TextureManager, where
         @textures = result[:textures] widened the Hash to a poly local) has no
         static key type, so without this arm the receiver switch fell through
         every Hash cls_id and returned nil. */
      /* fetch mirrors `[]` here: same runtime storage kinds, but gated on a
         has_key check so a present key returns its value while an absent key
         falls back to the supplied default / raises KeyError (a bare
         sp_poly_index_poly returns nil on a miss, which fetch must not do). */
      /* A TY_UNKNOWN key is held boxed-as-poly (atmp_ty == TY_POLY above), so
         it flows through sp_poly_index_poly / sp_poly_has_key exactly like an
         explicit poly key -- cover it here so a Hash reached by such a key is
         not dropped to nil (gemini review). */
      if ((is_aref || is_fetch) &&
          (infer_type(c, argv[0]) == TY_POLY || infer_type(c, argv[0]) == TY_UNKNOWN)) {
        TyKind ptrt = is_scalar_ret(ret) ? ret : TY_INT;
        buf_puts(b, " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:"
                    " case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_STR_STR_HASH:"
                    " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_INT_STR_HASH:");
        char gx[64], hx[64]; snprintf(gx, sizeof gx, "sp_poly_index_poly(_t%d, _t%d)", tv, atmp[0]);
        snprintf(hx, sizeof hx, "sp_poly_has_key(_t%d, _t%d)", tv, atmp[0]);
        buf_printf(b, " _t%d = ", tr);
        if (is_fetch) buf_printf(b, "%s ? ", hx);
        if (ret == TY_POLY) buf_puts(b, gx);
        else emit_unbox_text(c, ptrt, gx, b);
        if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? argv[1] : -1, argv[0], ret, ptrt, b); }
        buf_puts(b, "; break;");
      }
      /* eql?/equal?/is_a?/kind_of?/instance_of? on a builtin-scalar (or
         un-overridden object) poly value: the switch default answers via the
         universal predicate, with the (boxed) argument reused from atmp. */
      if (is_pred) {
        char tvref[24]; snprintf(tvref, sizeof tvref, "_t%d", tv);
        Buf ab; memset(&ab, 0, sizeof ab);
        if (atmp_ty[0] == TY_POLY) buf_printf(&ab, "_t%d", atmp[0]);
        else { char at[24]; snprintf(at, sizeof at, "_t%d", atmp[0]); emit_boxed_text(c, atmp_ty[0], at, &ab); }
        const char *argref = ab.p ? ab.p : "sp_box_nil()";
        buf_printf(b, " default: _t%d = ", tr);
        if (ret == TY_POLY) { buf_puts(b, "sp_box_bool("); emit_poly_pred_value(c, id, tvref, argref, b); buf_puts(b, ")"); }
        else emit_poly_pred_value(c, id, tvref, argref, b);
        buf_puts(b, "; break;");
        free(ab.p);
      }
      buf_printf(b, " } _t%d; })", tr);
      free(atmp);
      free(atmp_ty);
      return 1;
    }
  }
  return 0;
}

/* native (C-backed) class constructor: call the declared C symbol with the
   assigned cls_id first (runtime cls_id == class index), then the args in
   their native representation. The returned pointer is GC-allocated by the
   package. Shared by every `.new` shape (ConstantRead, ConstantPath). */
int emit_native_ctor(Compiler *c, int id, int ci, int argc, const int *argv, Buf *b) {
  if (ci < 0 || !c->classes[ci].is_native_class) return 0;
  TyKind natys[8];
  int nta = argc < 8 ? argc : 8;
  for (int a = 0; a < nta; a++) natys[a] = comp_ntype(c, argv[a]);
  int nn = comp_native_method_find_typed(c, ci, "new", argc, 1, nta == argc ? natys : NULL);
  if (nn < 0) return 0;
  NativeMethod *m = &c->native_methods[nn];
  buf_printf(b, "%s(%d", m->csym, ci);
  for (int ai = 0; ai < m->nargs && ai < argc; ai++) {
    buf_puts(b, ", ");
    if (sp_streq(m->args[ai], "any")) emit_boxed(c, argv[ai], b);
    else if (sp_streq(m->args[ai], "string") && comp_ntype(c, argv[ai]) == TY_POLY) {
      buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
    }
    else if (sp_streq(m->args[ai], "int") && comp_ntype(c, argv[ai]) == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
    }
    else if (sp_streq(m->args[ai], "float") && comp_ntype(c, argv[ai]) == TY_POLY) {
      buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
    }
    else if (sp_streq(m->args[ai], "bool") && comp_ntype(c, argv[ai]) == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
    }
    else emit_expr(c, argv[ai], b);
  }
  buf_puts(b, ")");
  (void)id;
  return 1;
}

/* Positional-arity bounds a class's `initialize` accepts, for the `.new` arity
   check. Returns 1 and fills [*pmin,*pmax] only when the arity is a fixed
   positional window we can enforce: no `initialize` (Object#initialize, arity
   0), or an `initialize` with only required/optional positionals. Returns 0 --
   don't enforce -- when `initialize` has a rest (`*a`), any keyword params, a
   `**kwrest`, or `...` forwarding, since those accept variable/keyword arg
   counts that argc alone can't validate. */
static int class_new_pos_arity(Compiler *c, int ci, int *pmin, int *pmax) {
  int initm = comp_method_in_chain(c, ci, "initialize", NULL);
  if (initm < 0) { *pmin = 0; *pmax = 0; return 1; }
  int def = c->scopes[initm].def_node;
  if (def < 0) return 0;
  int pn = nt_ref(c->nt, def, "parameters");
  if (pn < 0) { *pmin = 0; *pmax = 0; return 1; }
  /* Any rest / keyword / kwrest / forwarding param makes the count variable. */
  if (nt_ref(c->nt, pn, "rest") >= 0 || nt_ref(c->nt, pn, "keyword_rest") >= 0)
    return 0;
  int kn = 0; nt_arr(c->nt, pn, "keywords", &kn);
  if (kn > 0) return 0;
  int rn = 0; nt_arr(c->nt, pn, "requireds", &rn);
  int on = 0; nt_arr(c->nt, pn, "optionals", &on);
  int postn = 0; nt_arr(c->nt, pn, "posts", &postn);
  *pmin = rn + postn;
  *pmax = rn + postn + on;
  return 1;
}

/* Emit an unconditional ArgumentError raise into the prelude when `argc`
   positional args cannot satisfy class `ci`'s `initialize` -- MRI raises at the
   `.new` site. Skips the check when the call carries a splat or a keyword hash
   (argc is not the true positional count), or when the arity is variable.
   Returns 1 if a raise was emitted (caller still emits the -- now dead --
   construction so the expression stays well-typed). */
/* Civil-argument Time constructor forms, shared by `Time.new(...)` (via the
   generic constant-new path) and Time.local/mktime/utc/gm. Up to 6 civil
   fields with CRuby's defaults (month/day 1, rest 0); a 7th positional
   argument is the utc_offset for Time.new but the usec-of-second count for
   the named class methods. Returns 1 when it emitted. */
static int emit_time_civil_ctor(Compiler *c, int id, int is_utc, int is_new, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc;
  const int *argv = call_args(nt, id, &argc);
  /* Time.new("YYYY-MM-DD HH:MM:SS[.frac][ zone]") string-parsing form */
  if (is_new && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
    buf_puts(b, "sp_time_parse(");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  /* the 10-argument reversed form (sec, min, hour, day, mon, year, wday,
     yday, isdst, tz) accepted by the named class methods */
  if (!is_new && argc == 10) {
    buf_printf(b, "sp_time_new%s(", is_utc ? "_utc" : "");
    for (int i = 5; i >= 0; i--) {
      emit_expr(c, argv[i], b);
      if (i) buf_puts(b, ", ");
    }
    buf_puts(b, ")");
    return 1;
  }
  if (argc < 1 || argc > 7) return 0;
  /* a fractional seconds field (Rational/Float) for the named class methods:
     split into whole seconds and a nanosecond remainder (#2638). */
  if (!is_new && argc == 6) {
    TyKind sect = comp_ntype(c, argv[5]);
    if (sect == TY_RATIONAL || sect == TY_FLOAT) {
      int ts = ++g_tmp;
      buf_printf(b, "({ double _t%d = ", ts);
      if (sect == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[5], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[5], b);
      buf_printf(b, "; sp_Time _t%da = sp_time_new%s(", ts, is_utc ? "_utc" : "");
      for (int i = 0; i < 5; i++) {
        if (i) buf_puts(b, ", ");
        TyKind fit = comp_ntype(c, argv[i]);
        if (fit == TY_STRING) { buf_puts(b, "(int64_t)strtoll("); emit_expr(c, argv[i], b); buf_puts(b, ", NULL, 10)"); }
        else if (fit == TY_POLY || fit == TY_UNKNOWN) { buf_puts(b, "sp_poly_to_i("); emit_boxed(c, argv[i], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[i], b);
      }
      buf_printf(b, ", (int64_t)_t%d); _t%da.tv_nsec = (int32_t)((_t%d - (double)(int64_t)_t%d) * 1e9 + 0.5); _t%da; })",
                 ts, ts, ts, ts, ts);
      return 1;
    }
  }
  /* Time.new(y[, mo[, d[, h[, mi[, s]]]]], in: <off>): the `in:` keyword is
     the utc_offset, same as the 7th positional (#2718). A "+HH:MM" literal
     resolves at compile time; an Integer expression passes through. */
  if (is_new && argc >= 2 && argc <= 7 &&
      nt_type(nt, argv[argc - 1]) && sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    int inv = struct_kwarg_value(c, argv[argc - 1], "in");
    if (inv >= 0) {
      int npos = argc - 1;
      long koff = 0; int khave = 0;
      const char *ity = nt_type(nt, inv);
      if (ity && sp_streq(ity, "StringNode")) {
        const char *sv = nt_str(nt, inv, "content");
        if (!sv || strlen(sv) != 6 || (sv[0] != '+' && sv[0] != '-') || sv[3] != ':' ||
            sv[1] < '0' || sv[1] > '9' || sv[2] < '0' || sv[2] > '9' ||
            sv[4] < '0' || sv[4] > '9' || sv[5] < '0' || sv[5] > '9')
          return 0;
        koff = ((sv[1] - '0') * 10 + (sv[2] - '0')) * 3600 +
               ((sv[4] - '0') * 10 + (sv[5] - '0')) * 60;
        if (sv[0] == '-') koff = -koff;
        khave = 1;
      }
      else if (comp_ntype(c, inv) != TY_INT) return 0;
      buf_puts(b, "sp_time_new_off(");
      for (int i = 0; i < 6; i++) {
        if (i) buf_puts(b, ", ");
        if (i < npos) {
          TyKind fit = comp_ntype(c, argv[i]);
          if (fit == TY_STRING) { buf_puts(b, "(int64_t)strtoll("); emit_expr(c, argv[i], b); buf_puts(b, ", NULL, 10)"); }
          else if (fit == TY_POLY || fit == TY_UNKNOWN) { buf_puts(b, "sp_poly_to_i("); emit_boxed(c, argv[i], b); buf_puts(b, ")"); }
          else emit_int_expr(c, argv[i], b);
        }
        else buf_puts(b, i == 1 || i == 2 ? "1" : "0");   /* mo/d default 1; h/mi/s 0 */
      }
      buf_puts(b, ", ");
      if (khave) buf_printf(b, "%ld", koff);
      else emit_int_expr(c, inv, b);
      buf_puts(b, ")");
      return 1;
    }
  }
  long lit_off = 0;
  int have_lit_off = 0;
  if (argc == 7 && is_new) {
    /* utc_offset: an Integer-second expression, or a literal "+HH:MM" */
    const char *oty = nt_type(nt, argv[6]);
    if (oty && sp_streq(oty, "StringNode")) {
      const char *sv = nt_str(nt, argv[6], "content");
      if (!sv || strlen(sv) != 6 || (sv[0] != '+' && sv[0] != '-') || sv[3] != ':' ||
          sv[1] < '0' || sv[1] > '9' || sv[2] < '0' || sv[2] > '9' ||
          sv[4] < '0' || sv[4] > '9' || sv[5] < '0' || sv[5] > '9')
        return 0;
      lit_off = ((sv[1] - '0') * 10 + (sv[2] - '0')) * 3600 +
                ((sv[4] - '0') * 10 + (sv[5] - '0')) * 60;
      if (sv[0] == '-') lit_off = -lit_off;
      have_lit_off = 1;
    }
else {
      /* sp_time_new_off's 7th param is int64_t; only TY_INT guarantees an
         int-compatible emission. A TY_UNKNOWN offset can emit a boxed value
         (invalid C), so fall back to the generic "unsupported form" error. */
      TyKind ot = comp_ntype(c, argv[6]);
      if (ot != TY_INT) return 0;
    }
    buf_puts(b, "sp_time_new_off(");
  }
else if (argc == 7) buf_printf(b, "sp_time_with_usec(sp_time_new%s(", is_utc ? "_utc" : "");
else buf_printf(b, "sp_time_new%s(", is_utc ? "_utc" : "");
  for (int i = 0; i < 6; i++) {
    if (i) buf_puts(b, ", ");
    if (i < argc) {
      /* a numeric-string civil field is coerced to its Integer value, as CRuby
         does (Time.utc("2020", "3", "4")); a poly field unboxes. (#2689) */
      TyKind fit = comp_ntype(c, argv[i]);
      if (fit == TY_STRING) { buf_puts(b, "(int64_t)strtoll("); emit_expr(c, argv[i], b); buf_puts(b, ", NULL, 10)"); }
      else if (fit == TY_POLY || fit == TY_UNKNOWN) { buf_puts(b, "sp_poly_to_i("); emit_boxed(c, argv[i], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[i], b);
    }
    else buf_puts(b, (i == 1 || i == 2) ? "1" : "0");
  }
  if (argc == 7) {
    buf_puts(b, is_new ? ", " : "), ");
    if (have_lit_off) buf_printf(b, "%ld", lit_off);
    else emit_expr(c, argv[6], b);
  }
  buf_puts(b, ")");
  return 1;
}

static int emit_new_arity_check(Compiler *c, int ci, int argc,
                                const int *argv, Buf *pre) {
  const NodeTable *nt = c->nt;
  for (int a = 0; a < argc; a++) {
    const char *at = nt_type(nt, argv[a]);
    if (at && (sp_streq(at, "SplatNode") || sp_streq(at, "KeywordHashNode"))) return 0;
  }
  int lo, hi;
  if (!class_new_pos_arity(c, ci, &lo, &hi)) return 0;
  if (argc >= lo && argc <= hi) return 0;
  char exp[48];
  if (lo == hi) snprintf(exp, sizeof exp, "%d", lo);
  else snprintf(exp, sizeof exp, "%d..%d", lo, hi);
  buf_printf(pre, "sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given %d, expected %s)\");\n",
             argc, exp);
  return 1;
}

static int emit_class_new_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  /* Symbol receiver: the case conversions and succ/next stay SYMBOLS
     (round-trip through the name text and re-intern); to_s and friends
     keep their existing string paths. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_SYMBOL && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    const char *symfn = NULL;
    if (sp_streq(name, "upcase")) symfn = "sp_str_upcase";
    else if (sp_streq(name, "downcase")) symfn = "sp_str_downcase";
    else if (sp_streq(name, "capitalize")) symfn = "sp_str_capitalize";
    else if (sp_streq(name, "swapcase")) symfn = "sp_str_swapcase";
    else if (sp_streq(name, "succ") || sp_streq(name, "next")) symfn = "sp_str_succ";
    if (symfn) {
      buf_printf(b, "sp_sym_intern(%s(sp_sym_to_s(", symfn);
      emit_expr(c, recv, b);
      buf_puts(b, ")))");
      return 1;
    }
  }
  /* Hash[k: v] desugared to a bare hash literal: emit the receiver */
  if (recv >= 0 && sp_streq(name, "__hash_brackets_kw")) {
    emit_expr(c, recv, b);
    return 1;
  }
  /* Hash[] with no arguments constructs an empty hash. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash")) {
    buf_puts(b, "sp_StrPolyHash_new()");
    return 1;
  }
  /* <StructClass>.members at the class level: the member symbol list */
  if (recv >= 0 && sp_streq(name, "members") && argc == 0) {
    const char *mrty = nt_type(nt, recv);
    int mci = -1;
    if (mrty && (sp_streq(mrty, "ConstantReadNode") || sp_streq(mrty, "ConstantPathNode")))
      mci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (mrty && (sp_streq(mrty, "LocalVariableReadNode") ||
                      (sp_streq(mrty, "CallNode") && is_struct_call(c, recv))))
      mci = class_var_static_ci(c, recv);
    if (mci >= 0 && c->classes[mci].is_struct &&
        comp_cmethod_in_chain(c, mci, "members", NULL) < 0) {
      ClassInfo *mcl = &c->classes[mci];
      int rm = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
      for (int i = 0; i < mcl->nivars; i++)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, mcl->ivars[i] + 1));
      buf_printf(b, " _t%d; })", rm);
      return 1;
    }
  }
  /* StructClass.keyword_init? -> whether the struct was defined keyword_init: true
     (a Data class is not keyword-init in CRuby's sense: keyword_init? is nil, but
     spinel has no separate nil-class answer here, so report false). */
  if (recv >= 0 && sp_streq(name, "keyword_init?") && argc == 0) {
    const char *krty = nt_type(nt, recv);
    int kci = -1;
    if (krty && (sp_streq(krty, "ConstantReadNode") || sp_streq(krty, "ConstantPathNode")))
      kci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (krty && sp_streq(krty, "LocalVariableReadNode"))
      kci = class_var_static_ci(c, recv);
    if (kci >= 0 && c->classes[kci].is_struct &&
        comp_cmethod_in_chain(c, kci, "keyword_init?", NULL) < 0) {
      int kw = c->classes[kci].kw_init;
      buf_puts(b, kw == 1 ? "sp_box_bool(TRUE)" : kw == -1 ? "sp_box_bool(FALSE)" : "sp_box_nil()");
      return 1;
    }
  }
  /* Class.new(args) -> sp_<Class>_new(args) */
  /* Array.try_convert(x): x if it is array-like (statically an array type),
     else nil. Result is array-or-nil -> poly (#2325). */
  if (recv >= 0 && sp_streq(name, "try_convert") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Array")) {
    TyKind at = comp_ntype(c, argv[0]);
    if (ty_is_array(at)) { emit_boxed(c, argv[0], b); return 1; }
    buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    return 1;
  }
  /* Integer.try_convert(x): x if it is an Integer, else nil (#2585). */
  if (recv >= 0 && sp_streq(name, "try_convert") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer")) {
    TyKind at = comp_ntype(c, argv[0]);
    if (at == TY_INT || at == TY_BIGINT) { emit_boxed(c, argv[0], b); return 1; }
    /* a Float converts via to_int (truncates; Inf/NaN raises FloatDomainError) */
    if (at == TY_FLOAT) { buf_puts(b, "sp_box_int(sp_float_to_i_checked("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1; }
    buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    return 1;
  }
  /* Hash.try_convert(x): the hash itself, nil for a non-hash. (The
     ruby2_keywords_hash pair is a documented compile-time reject, #2846.) */
  if (recv >= 0 && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash") &&
      sp_streq(name, "try_convert")) {
    TyKind at = comp_ntype(c, argv[0]);
    if (ty_is_hash(at)) { emit_boxed(c, argv[0], b); return 1; }
    buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    return 1;
  }
  if (recv >= 0 && (sp_streq(name, "new") || sp_streq(name, "__hash_new_default"))) {
    const char *rty = nt_type(nt, recv);
    /* a local statically holding one class constant dispatches like the
       constant itself (k = Klass; k.new(...)) */
    if (rty && sp_streq(rty, "LocalVariableReadNode") && class_var_static_ci(c, recv) >= 0) {
      int ci = class_var_static_ci(c, recv);
      if (emit_native_ctor(c, id, ci, argc, argv, b)) return 1;
      if (ci >= 0 && !c->classes[ci].is_struct) {
        /* the .new result is typed poly (a class-var receiver dispatches
           dynamically), so box the concrete constructor result to match (#2653) */
        int is_val = comp_ty_value_obj(c, ty_object(ci));
        if (is_val) buf_printf(b, "sp_box_vobj_%s(sp_%s_new(", c->classes[ci].c_name, c->classes[ci].c_name);
        else buf_printf(b, "sp_box_obj(sp_%s_new(", c->classes[ci].c_name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        if (is_val) buf_puts(b, "))");
        else buf_printf(b, "), %d)", ci);
        return 1;
      }
    }
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode") ||
                (sp_streq(rty, "LocalVariableReadNode") && class_var_static_ci(c, recv) >= 0) ||
                (sp_streq(rty, "CallNode") && is_struct_call(c, recv)))) {
      int ci = sp_streq(rty, "LocalVariableReadNode") ? class_var_static_ci(c, recv)
             : sp_streq(rty, "CallNode") ? anon_struct_ci_for_value(c, recv)
                                                      : comp_class_index(c, nt_str(nt, recv, "name"));
      /* native (C-backed) class: the declared constructor (see emit_native_ctor) */
      if (emit_native_ctor(c, id, ci, argc, argv, b)) return 1;
      if (ci >= 0 && c->classes[ci].is_struct) {
        /* Struct.new members: positional args, or keyword args mapping each
           member by name; each coerced to the member ivar type. */
        ClassInfo *cls = &c->classes[ci];
        /* A struct with a custom `initialize` override delegates to it: the
           .new args are that initialize's params (not one-per-member), so call
           the constructor with the args filled to its signature. */
        int scust = comp_method_in_chain(c, ci, "initialize", NULL);
        if (scust >= 0 && c->scopes[scust].reachable && c->scopes[scust].yields) {
          /* a yielding custom initialize (yield / block_given? / a called &blk)
             runs its body inlined at the .new site -- the body drives the block
             and calls super to set the members. Without this the member-wise
             path below runs on the raw args, dropping the block and the body. */
          if (emit_ctor_yield_inline(c, id, ci, b)) return 1;
          /* a declined inline must not silently fall through to member-wise
             construction (which drops the block); reject loudly instead. */
          unsupported(c, id, "yielding Struct/Data initialize with a non-inlinable body");
        }
        if (scust >= 0 && c->scopes[scust].reachable && !c->scopes[scust].yields) {
          buf_printf(b, "sp_%s_new(", cls->c_name);
          emit_args_filled(c, scust, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
          return 1;
        }
        int kwh = (argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) ? argv[0] : -1;
        /* Data.new validates its arguments strictly (unlike Struct, which
           nil-fills): exact positional count, or a keyword for every member and
           no extras; a mix of positional and keyword is an error. A `**splat`
           has non-literal keys, so its check is deferred to run time (#2661). */
        if (cls->is_data) {
          int kw_splat = 0;
          if (kwh >= 0) {
            int nk0; const int *els0 = nt_arr(nt, kwh, "elements", &nk0);
            for (int i = 0; i < nk0; i++)
              if (!(nt_type(nt, els0[i]) && sp_streq(nt_type(nt, els0[i]), "AssocNode"))) kw_splat = 1;
          }
          int mixed = 0;
          for (int a = 0; kwh < 0 && a < argc; a++)
            if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "KeywordHashNode")) mixed = 1;
          int bad = 0;
          if (!kw_splat) {
            if (kwh < 0) bad = mixed || argc != cls->nivars;
            else {
              int present = 0;
              for (int a = 0; a < cls->nivars; a++)
                if (struct_kwarg_value(c, kwh, cls->ivars[a] + 1) >= 0) present++;
              int nk1; nt_arr(nt, kwh, "elements", &nk1);
              bad = present != cls->nivars || nk1 != cls->nivars;
            }
          }
          if (bad) {
            buf_puts(b, "({ ");
            for (int a = 0; a < argc; a++) {
              if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "KeywordHashNode")) {
                int nk2; const int *els2 = nt_arr(nt, argv[a], "elements", &nk2);
                for (int i = 0; i < nk2; i++)
                  if (nt_type(nt, els2[i]) && sp_streq(nt_type(nt, els2[i]), "AssocNode")) {
                    int vv = nt_ref(nt, els2[i], "value");
                    if (vv >= 0) { buf_puts(b, "(void)("); emit_boxed(c, vv, b); buf_puts(b, "); "); }
                  }
              } else { buf_puts(b, "(void)("); emit_boxed(c, argv[a], b); buf_puts(b, "); "); }
            }
            buf_puts(b, "sp_raise_cls(\"ArgumentError\", (&(\"\\xff\" \"wrong number of arguments\")[1])); ");
            buf_printf(b, "sp_%s_new(", cls->c_name);
            for (int a = 0; a < cls->nivars; a++) { if (a) buf_puts(b, ", "); buf_puts(b, default_value(cls->ivar_types[a])); }
            buf_puts(b, "); })");
            return 1;
          }
        }
        /* more positional args than members is a struct-size ArgumentError
           (fewer are allowed for Struct: nil fill); evaluate args first for
           any side effects, then raise before constructing */
        if (kwh < 0 && argc > cls->nivars) {
          buf_puts(b, "({ ");
          for (int a2 = 0; a2 < argc; a2++) { buf_puts(b, "(void)("); emit_boxed(c, argv[a2], b); buf_puts(b, "); "); }
          buf_puts(b, "sp_raise_cls(\"ArgumentError\", (&(\"\\xff\" \"struct size differs\")[1])); ");
          buf_printf(b, "sp_%s_new(", cls->c_name);
          for (int a2 = 0; a2 < cls->nivars; a2++) {
            if (a2) buf_puts(b, ", ");
            buf_puts(b, default_value(cls->ivar_types[a2]));
          }
          buf_puts(b, "); })");
          return 1;
        }
        /* a `**h` keyword splat: pull each member from the splatted hash at run
           time by its symbol name (variant-agnostic getter). (#2704) */
        int splat_h = -1;
        if (kwh >= 0) {
          int nks; const int *elss = nt_arr(nt, kwh, "elements", &nks);
          for (int i = 0; i < nks; i++)
            if (nt_type(nt, elss[i]) && sp_streq(nt_type(nt, elss[i]), "AssocSplatNode"))
              splat_h = nt_ref(nt, elss[i], "value");
        }
        buf_printf(b, "sp_%s_new(", cls->c_name);
        for (int a = 0; a < cls->nivars; a++) {
          if (a) buf_puts(b, ", ");
          int vnode = -1;
          if (kwh >= 0) vnode = struct_kwarg_value(c, kwh, cls->ivars[a] + 1);
          else if (a < argc) vnode = argv[a];
          if (vnode >= 0) {
            if (cls->ivar_types[a] == TY_POLY && comp_ntype(c, vnode) != TY_POLY) emit_boxed(c, vnode, b);
            else emit_expr(c, vnode, b);
          }
          else if (splat_h >= 0) {
            Buf hv; memset(&hv, 0, sizeof hv); emit_boxed(c, splat_h, &hv);
            buf_printf(b, "sp_poly_hash_get_pair_val(%s, sp_box_sym(sp_sym_intern(\"%s\")), &(mrb_bool){0})",
                       hv.p ? hv.p : "sp_box_nil()", cls->ivars[a] + 1);
            free(hv.p);
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
            buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
            emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
            buf_puts(b, ")");
          }
          else {
            /* no user initialize: create directly with first arg as message.
               An ivar-bearing subclass needs its dedicated struct size --
               sp_exc_new_sub would only allocate the base (#2772). */
            const char *cn2 = class_ruby_name(c, ci); if (!cn2) cn2 = c->classes[ci].name;
            const char *par = exc_builtin_parent(c, ci);
            if (c->classes[ci].nivars > 0)
              buf_printf(b, "((sp_%s *)sp_exc_new_sub_sized(sizeof(sp_%s), \"%s\", ",
                         c->classes[ci].c_name, c->classes[ci].c_name, cn2);
            else
              buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", ", cn2, par);
            if (argc >= 1) {
              if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
              else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
            }
            else buf_puts(b, "(&(\"\\xff\")[1])");
            buf_puts(b, c->classes[ci].nivars > 0 ? "))" : ")");
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
          buf_printf(b, "sp_%s_s_new(", c->classes[defcls2 >= 0 ? defcls2 : ci].c_name);
          emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
          return 1;
        }
        /* Extra/missing positional args to a fixed-arity initialize raise
           ArgumentError at the .new site, as in MRI; the check emits the raise
           into the prelude and the (dead) construction below keeps the type. */
        emit_new_arity_check(c, ci, argc, argv, g_pre);
        buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        /* An explicit `&blk` on a non-yielding initialize is threaded as a
           trailing sp_Proc* (the constructor accepts + forwards it). Pass the
           call's block, or NULL when the block is omitted. */
        if (initm >= 0 && c->scopes[initm].blk_param && c->scopes[initm].blk_param[0] &&
            !c->scopes[initm].yields) {
          if (c->scopes[initm].nparams > 0) buf_puts(b, ", ");
          int blk = nt_ref(nt, id, "block");
          const char *bty = blk >= 0 ? nt_type(nt, blk) : NULL;
          int bexpr = (bty && sp_streq(bty, "BlockArgumentNode")) ? nt_ref(nt, blk, "expression") : -1;
          if (bty && sp_streq(bty, "BlockNode"))
            emit_proc_literal(c, blk, b);
          /* A forwarded `&proc` (BlockArgumentNode) threads the proc value
             itself into the stored `&blk`. This is faithful now that every proc
             publishes its result on the boxed return channel, so a later
             `@blk.call` reads it back correctly regardless of the proc's body
             type (the reason this once had to be a loud reject). */
          else if (bexpr >= 0 && comp_ntype(c, bexpr) == TY_PROC)
            emit_expr(c, bexpr, b);
          /* A poly-carried proc (a proc pulled from a poly slot -- a container
             element, an untyped ivar/return) arrives boxed; unbox it to the
             sp_Proc*, mirroring the poly `.call` site's `(sp_Proc *)v.v.p`. */
          else if (bexpr >= 0 && comp_ntype(c, bexpr) == TY_POLY) {
            buf_puts(b, "(sp_Proc *)("); emit_expr(c, bexpr, b); buf_puts(b, ").v.p");
          }
          /* A forwarded block of an unmodeled static type: refuse loudly rather
             than silently thread a NULL block (a later @blk.call would misfire
             -- the silent-wrong outcome). block omitted (bexpr < 0) is the real
             NULL case below. */
          else if (bexpr >= 0)
            unsupported(c, id, "forwarding a block of this type into a stored-block initialize");
          else buf_puts(b, "NULL");
        }
        buf_puts(b, ")");
        return 1;
      }
      const char *cn = nt_str(nt, recv, "name");
      if (cn && is_exc_name(cn)) {
        /* SignalException.new(sig) / Interrupt.new(msg?): the message is the
           SIG-name and #signo is carried (#2762) */
        if (sp_streq(cn, "SignalException")) {
          if (argc == 0) {
            buf_puts(b, "((sp_Exception *)(sp_raise_cls(\"ArgumentError\","
                        " \"wrong number of arguments (given 0, expected 1..2)\"), NULL))");
            return 1;
          }
          buf_puts(b, "sp_signal_exc_new(");
          emit_boxed(c, argv[0], b);
          buf_puts(b, ")");
          return 1;
        }
        if (sp_streq(cn, "Interrupt")) {
          buf_puts(b, "sp_interrupt_new(");
          if (argc >= 1) {
            if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
            else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
          }
          else buf_puts(b, "(&(\"\\xff\")[1])");
          buf_puts(b, ")");
          return 1;
        }
        /* SystemExit.new(status = true, msg = "exit"): a leading Integer or
           boolean is the status; a String is the message (#2761) */
        if (sp_streq(cn, "SystemExit")) {
          int te4 = ++g_tmp;
          TyKind s0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
          int has_status = argc >= 1 && (s0 == TY_INT || s0 == TY_BOOL);
          int msg_i = has_status ? 1 : 0;
          buf_printf(b, "({ sp_Exception *_t%d = sp_exc_new(\"SystemExit\", ", te4);
          if (argc > msg_i) {
            if (comp_ntype(c, argv[msg_i]) == TY_STRING) emit_expr(c, argv[msg_i], b);
            else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[msg_i], b); buf_puts(b, ")"); }
          }
          else buf_puts(b, "(&(\"\\xff\")[1])");   /* default message = class name */
          buf_printf(b, "); SP_GC_ROOT(_t%d); _t%d->result = sp_box_int(", te4, te4);
          if (has_status) {
            if (s0 == TY_BOOL) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ") ? 0 : 1"); }
            else emit_expr(c, argv[0], b);
          }
          else buf_puts(b, "0");
          buf_printf(b, "); _t%d; })", te4);
          return 1;
        }
        /* trailing `key:`/`receiver:` keywords (KeyError.new(msg, key:,
           receiver:), NameError.new(msg, receiver:)): carry them for the
           introspection accessors (#2753, #2754) */
        if (argc >= 1 && nt_type(nt, argv[argc - 1]) &&
            sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
          int kn2 = 0;
          const int *kel2 = nt_arr(nt, argv[argc - 1], "elements", &kn2);
          int key_v = -1, recv_v = -1, other = 0;
          for (int ke = 0; ke < kn2; ke++) {
            const char *kty = nt_type(nt, kel2[ke]);
            int kk2 = (kty && sp_streq(kty, "AssocNode")) ? nt_ref(nt, kel2[ke], "key") : -1;
            const char *knm = (kk2 >= 0 && nt_type(nt, kk2) && sp_streq(nt_type(nt, kk2), "SymbolNode"))
                                ? nt_str(nt, kk2, "value") : NULL;
            if (knm && sp_streq(knm, "key")) key_v = nt_ref(nt, kel2[ke], "value");
            else if (knm && sp_streq(knm, "receiver")) recv_v = nt_ref(nt, kel2[ke], "value");
            else other = 1;
          }
          if (!other && (key_v >= 0 || recv_v >= 0)) {
            int te3 = ++g_tmp;
            buf_printf(b, "({ sp_Exception *_t%d = sp_exc_new(\"%s\", ", te3, cn);
            if (argc >= 2) {
              if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
              else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
            }
            else buf_puts(b, "(&(\"\\xff\")[1])");
            buf_printf(b, "); SP_GC_ROOT(_t%d);", te3);
            if (key_v >= 0) {
              buf_printf(b, " _t%d->xkey = ", te3);
              emit_boxed(c, key_v, b); buf_puts(b, ";");
            }
            if (recv_v >= 0) {
              buf_printf(b, " _t%d->xrecv = ", te3);
              emit_boxed(c, recv_v, b); buf_puts(b, ";");
            }
            buf_printf(b, " _t%d; })", te3);
            return 1;
          }
        }
        /* NameError.new(msg, name) / NoMethodError.new(msg, name): carry the
           missing name for the #name accessor (rooted across the alloc). */
        if (argc >= 2 && (sp_streq(cn, "NameError") || sp_streq(cn, "NoMethodError"))) {
          int tn2 = ++g_tmp, te2 = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tn2);
          emit_boxed(c, argv[1], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_Exception *_t%d = sp_exc_new(\"%s\", ", tn2, te2, cn);
          emit_expr(c, argv[0], b);
          buf_printf(b, "); _t%d->xname = _t%d; _t%d; })", te2, tn2, te2);
          return 1;
        }
        /* builtin exception class .new(msg): any object can be the message,
           a non-String coerces via to_s (#2741) */
        buf_printf(b, "sp_exc_new(\"%s\", ", cn);
        if (argc >= 1) {
          if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
          else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        }
        else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ")");
        return 1;
      }
      if (cn && sp_streq(cn, "String")) {
        /* String.new(str = "", capacity:, encoding:): always a mutable heap copy.
           The capacity/encoding keyword arguments are hints that do not change
           the value, so the content is the leading positional argument (when it
           is not itself the keyword hash). */
        const char *a0ty = argc >= 1 ? nt_type(nt, argv[0]) : NULL;
        int has_content = argc >= 1 && !(a0ty && sp_streq(a0ty, "KeywordHashNode"));
        if (has_content) { buf_puts(b, "sp_str_dup_external("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else buf_puts(b, "sp_str_dup_external((&(\"\\xff\")[1]))");
        return 1;
      }
      if (cn && sp_streq(cn, "Object") && argc == 0) {
        buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT)");
        return 1;
      }
      /* BasicObject.new: the same blank instance, tagged with the root's
         cls_id so #class answers BasicObject (#2658) */
      if (cn && sp_streq(cn, "BasicObject") && argc == 0) {
        buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_BASIC_OBJECT)");
        return 1;
      }
      if (cn && (sp_streq(cn, "Mutex") || sp_streq(cn, "Monitor"))) {
        buf_puts(b, "sp_Mutex_new()"); return 1;
      }
      if (cn && sp_streq(cn, "ConditionVariable")) {
        buf_puts(b, "sp_CondVar_new()"); return 1;
      }
      if (cn && sp_streq(cn, "Enumerator") && nt_ref(nt, id, "block") >= 0) {
        /* Enumerator.new(size) { |y| ... }: a fiber-backed generator where
           `y << v` lowers to a Fiber.yield. The optional leading arg is the
           #size (a value or a callable), stored on the enumerator. */
        emit_fiber_new(c, id, b, 1, argc >= 1 ? argv[0] : -1);
        return 1;
      }
      if (cn && sp_streq(cn, "Thread") && nt_ref(nt, id, "block") >= 0) {
        /* Thread.new(arg): an eager green thread wrapping a fiber built exactly
           like a Fiber.new block (the block result lands in fiber->yielded_value,
           read back by #value). Thread.new's first argument becomes the block's
           first param on entry; it is handed to the scheduler as the thread arg. */
        buf_puts(b, "sp_Thread_spawn_fiber(");
        emit_fiber_new(c, id, b, 0, -1);
        buf_puts(b, ", ");
        if (argc >= 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ")");
        return 1;
      }
      if (cn && sp_streq(cn, "Fiber") && nt_ref(nt, id, "block") >= 0) {
        emit_fiber_new(c, id, b, 0, -1);
        return 1;
      }
      if (cn && sp_streq(cn, "Queue")) { buf_puts(b, "sp_Queue_new()"); return 1; }
      if (cn && sp_streq(cn, "SizedQueue") && argc == 1) {
        buf_puts(b, "sp_SizedQueue_new("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); return 1;
      }
      if (cn && sp_streq(cn, "Random")) {
        if (argc >= 1) {
          TyKind a0t = comp_ntype(c, argv[0]);
          /* A Bignum seed (Random.new(2**70)) folds to its low 64 bits --
             the PRNG state only holds 64 bits of seed anyway. */
          int is_big = a0t == TY_BIGINT;
          /* A Float seed truncates to an integer (CRuby's Float#to_i); a plain
             (mrb_int) cast is UB when out of range (Random.new(1e300)), giving
             a non-reproducible seed. sp_Random_new_float truncates in range and
             folds the float's bits otherwise -- always deterministic. */
          if (a0t == TY_FLOAT) {
            buf_puts(b, "sp_Random_new_float("); emit_expr(c, argv[0], b); buf_puts(b, ")");
          }
          else {
            buf_puts(b, "sp_Random_new(");
            if (is_big) buf_puts(b, "sp_bigint_to_int(");
            emit_expr(c, argv[0], b);
            if (is_big) buf_puts(b, ")");
            buf_puts(b, ")");
          }
        }
        else {
          /* no seed: auto-seed uniquely (time alone repeats within a second) */
          buf_puts(b, "sp_Random_new_auto()");
        }
        return 1;
      }
      /* Hash.new / Hash.new(default) whose variant was pinned on the node
         (argument position pins PolyPoly): construct that variant directly. */
      if (cn && sp_streq(cn, "Hash") && nt_ref(nt, id, "block") < 0 &&
          ty_is_hash(comp_ntype(c, id))) {
        TyKind ht = comp_ntype(c, id);
        const char *hcn = ty_hash_cname(ht);
        if (argc == 0) buf_printf(b, "sp_%sHash_new()", hcn);
        else {
          buf_printf(b, "sp_%sHash_new_with_default(", hcn);
          if (ht == TY_SYM_POLY_HASH || ht == TY_STR_POLY_HASH || ht == TY_POLY_POLY_HASH)
            emit_boxed(c, argv[0], b);
          else emit_expr(c, argv[0], b);
          buf_puts(b, ")");
        }
        return 1;
      }
      /* Hash.new { |hash, key| default } -> a PolyPolyHash with a default-proc
         function computing the missing-key value. PolyPoly boxes each key by
         value, so a symbol key round-trips as a symbol (inspect renders `a:`),
         a string as a string, etc. -- faithful for the dynamically-keyed hash a
         default block implies. */
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
        buf_printf(pb, "static sp_RbVal _sp_hash_dproc_%d(sp_PolyPolyHash *_self_h, sp_RbVal _key, void *_dproc_self) {\n", dn);
        if (dp_self) buf_printf(pb, "  sp_%s *self = (sp_%s *)_dproc_self; (void)self;\n", dp_cls, dp_cls);
        else buf_puts(pb, "  (void)_dproc_self;\n");
        if (hp) buf_printf(pb, "  sp_PolyPolyHash *lv_%s = _self_h; (void)lv_%s;\n", rename_local(hp), rename_local(hp));
        if (kp) buf_printf(pb, "  sp_RbVal lv_%s = _key; (void)lv_%s;\n", rename_local(kp), rename_local(kp));
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
                 lines before this assignment, not spliced mid-line. Box into an
                 sp_RbVal temp rather than a comp_ntype-typed one: a container
                 literal like `[]` infers TY_UNKNOWN, whose emit_ctype is `void`
                 -- `void _t = sp_IntArray_new()` doesn't compile. The boxed
                 value is set into the hash and returned (the block's value). */
              Buf vexpr; memset(&vexpr, 0, sizeof vexpr);
              Buf vpre; memset(&vpre, 0, sizeof vpre);
              Buf *svp = g_pre; g_pre = &vpre;
              emit_boxed(c, sav[1], &vexpr);
              g_pre = svp;
              if (vpre.p) buf_puts(pb, vpre.p);
              free(vpre.p);
              emit_indent(pb, 1);
              buf_printf(pb, "sp_RbVal _t%d = %s;\n", vtmp, vexpr.p ? vexpr.p : "sp_box_nil()");
              free(vexpr.p);
              Buf kexpr; memset(&kexpr, 0, sizeof kexpr);
              Buf kpre; memset(&kpre, 0, sizeof kpre);
              Buf *svk = g_pre; g_pre = &kpre;
              emit_boxed(c, sav[0], &kexpr);
              g_pre = svk;
              if (kpre.p) buf_puts(pb, kpre.p);
              free(kpre.p);
              emit_indent(pb, 1); buf_puts(pb, "sp_PolyPolyHash_set(");
              emit_expr(c, srecv, pb); buf_puts(pb, ", ");
              buf_printf(pb, "%s, _t%d);\n", kexpr.p ? kexpr.p : "sp_box_nil()", vtmp);
              free(kexpr.p);
              emit_indent(pb, 1); buf_printf(pb, "return _t%d;\n", vtmp);
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
        if (dp_self) buf_printf(b, "sp_PolyPolyHash_new_dproc(_sp_hash_dproc_%d, (void *)self)", dn);
        else buf_printf(b, "sp_PolyPolyHash_new_dproc(_sp_hash_dproc_%d, NULL)", dn);
        return 1;
      }
      if (cn && sp_streq(cn, "Regexp") && argc >= 1) {
        int tp = ++g_tmp, ts = ++g_tmp;
        int flags = (argc >= 2) ? 1 : 0; /* Regexp::IGNORECASE=1 if 2nd arg truthy */
        /* Regexp.new(/re/): copying an existing Regexp reuses its source text and
           options (a second arg is ignored by CRuby in that case). Reading the
           source via sp_re_source avoids treating the pattern pointer as a
           C string (which yielded a garbage source, #2528). */
        if (comp_ntype(c, argv[0]) == TY_REGEX) {
          Buf rv; memset(&rv, 0, sizeof rv);
          emit_expr(c, argv[0], &rv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "const char *_t%d = sp_re_source((void *)(%s));\n", ts, rv.p ? rv.p : "0");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), sp_re_raw_flags((void *)(%s)));\n",
                     tp, ts, ts, ts, rv.p ? rv.p : "0");
          free(rv.p);
          buf_printf(b, "_t%d", tp);
          return 1;
        }
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
        buf_printf(g_pre, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");\n", tn);
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
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");\n", tn);
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
          buf_printf(g_pre, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");\n", tn);
          emit_indent(g_pre, g_indent);
          if (at == TY_POLY_ARRAY) {
            buf_printf(g_pre, "sp_RbVal _t%d = ", tv);
            TyKind fvt = comp_ntype(c, argv[1]);
            const char *fvty = nt_type(nt, argv[1]);
            int fv_en = 0;
            if (fvty && (sp_streq(fvty, "ArrayNode") || sp_streq(fvty, "HashNode")))
              nt_arr(nt, argv[1], "elements", &fv_en);
            /* an empty container literal types UNKNOWN: box the container
               itself (ONE object, aliased into every slot like CRuby) */
            if (fvt == TY_UNKNOWN && fvty && sp_streq(fvty, "ArrayNode") && fv_en == 0)
              buf_puts(g_pre, "sp_box_poly_array(sp_PolyArray_new())");
            else if (fvt == TY_UNKNOWN && fvty && sp_streq(fvty, "HashNode") && fv_en == 0)
              buf_puts(g_pre, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)");
            else if (fvt != TY_POLY) emit_boxed_text(c, fvt, vb.p ? vb.p : "sp_box_nil()", g_pre);
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
        if (emit_time_civil_ctor(c, id, 0, 1, b)) return 1;
        unsupported(c, id, "Time.new argument form");
        return 1;
      }
      /* File.new is File.open: handled by the File class-method block in the
         later dispatch (#2779). Dir.new likewise (#2821). */
      if (cn && (sp_streq(cn, "File") || sp_streq(cn, "Dir"))) return 0;
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
   return nil at runtime -- its unified return type is TY_POLY (nil among
   other results) or TY_NIL (always nil). The object comparison emitters
   (<, <=, >, >=, ==, between?) then route through the checked boxed
   comparators (sp_poly_cmp_ck / sp_poly_cmp_eq, dispatching the user `<=>`
   via sp_obj_cmp_hook) so an incomparable pair raises the Comparable
   ArgumentError like CRuby; a TY_INT `<=>` keeps the zero-cost inline
   `<op> 0` path. Value-type classes take the checked path too when their
   `<=>` can return nil: the direct `<op> 0` emit would compare an sp_RbVal
   (invalid C), and sp_box_vobj / the cmp hook's by-value deref handle them. */
static int user_cmp_needs_check(Compiler *c, int cid) {
  int def = -1;
  int mi = comp_method_in_chain(c, cid, "<=>", &def);
  if (mi < 0) return 0;   /* no user <=> reachable: keep the inline path */
  TyKind ret = (TyKind)c->scopes[mi].ret;
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, "<=>", &kd);
    if (kmi >= 0 && (TyKind)c->scopes[kmi].ret != TY_UNKNOWN)
      ret = ty_unify(ret, (TyKind)c->scopes[kmi].ret);
  }
  /* POLY/NIL may be nil at runtime; a statically non-numeric result
     (String/Symbol/Bool) is never a valid `<=>` value -> the checked path
     raises the Comparable ArgumentError instead of the inline `<op> 0` reading
     a non-int as an int (#2559). A pure Integer/Float `<=>` keeps the inline
     path (Float compares as a double correctly). */
  return ret == TY_POLY || ret == TY_NIL ||
         ret == TY_STRING || ret == TY_SYMBOL || ret == TY_BOOL;
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
  /* Object#equal? is pointer identity and is never overridden, so a plain user
     object (e.g. a package's Set) answers it even with no user-defined method
     (#2629). A pointer-backed arg compares by address; anything else (a scalar,
     or a value-type object with no stable identity) is never the same object. */
  if (argc == 1 && sp_streq(name, "equal?") && recv >= 0 && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt)) {
    if (ty_is_object(a0) && !comp_ty_value_obj(c, a0)) {
      buf_puts(b, "((void *)("); emit_expr(c, recv, b); buf_puts(b, ") == (void *)(");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
    } else {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b); buf_puts(b, "), FALSE)");
    }
    return 1;
  }
  /* `===` on a scalar comparable (bool/int/float/string/symbol) is case
     equality == value equality. Range/Class/Regexp `===` have their own
     handlers and fall through here. */
  if (argc == 1 && sp_streq(name, "===")) {
    int fr = eq_family(rt), fa = eq_family(a0);
    /* Integer#=== a Bignum (either side) compares by value, not the pointer
       identity a plain `==` on the sp_Bigint* would give (#2584). */
    if ((rt == TY_INT || rt == TY_BIGINT) && (a0 == TY_INT || a0 == TY_BIGINT) &&
        (rt == TY_BIGINT || a0 == TY_BIGINT)) {
      buf_puts(b, "(sp_bigint_cmp(");
      if (rt == TY_BIGINT) emit_expr(c, recv, b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      buf_puts(b, ", ");
      if (a0 == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ") == 0)");
      return 1;
    }
    /* Range/float-range `===` is membership, not value equality; both fall
       through to their dedicated cover handlers. */
    if (fr && fr != 5 && fr != 6 && fa && fa != 5 && fa != 6) {
      if (fr == fa) {
        if (fr == 2) { buf_puts(b, "sp_str_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), ("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      return 1;
    }
    /* A scalar-comparable receiver against a poly argument (e.g. a boolean or
       integer param `=== y` where y unified to poly across call sites): case
       equality is value equality, so box the receiver and compare by the poly
       runtime rule (int/float cross-compare numerically, other tags by tag). */
    if (fr && fr != 5 && fr != 6 && a0 == TY_POLY) {
      buf_puts(b, "sp_poly_eq("); emit_boxed(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* a comparable-family receiver === nil is always false; === is value
       equality, not a method nil must define (#2584: 3 === nil is false). */
    if (fr && fr != 5 && fr != 6 && a0 == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
      return 1;
    }
  }

  if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "!=") ||
                    (sp_streq(name, "===") && rt == TY_STRING) ||  /* String#=== is == (#2347) */
                    (sp_streq(name, "eql?") &&
                     (ty_is_array(rt) || ty_is_hash(rt) ||
                      (ty_is_object(rt) && ty_object_class(rt) >= 0 &&
                       ty_object_class(rt) < c->nclasses &&
                       c->classes[ty_object_class(rt)].is_struct))))) {
    /* Array#eql? is structural like == but class-strict per element
       (1 is not eql? to 1.0): box both sides through the strict poly
       comparator. Scalar eql? is handled by the per-type emitters. */
    int eq = !sp_streq(name, "!=");
    if (sp_streq(name, "eql?") && (ty_is_array(rt) || ty_is_array(a0) ||
                                   ty_is_hash(rt) || ty_is_hash(a0))) {
      buf_puts(b, "sp_poly_eql(");
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    /* `x == nil` / `x != nil` for any receiver */
    int a_nil = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "NilNode");
    int r_nil = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "NilNode");
    /* Both operands statically nil-TYPED: two calls the analyzer proved return
       nil (`probe(1) == probe(2)`), or one such call against a literal nil
       (`probe(5) == nil`). nil == nil is constant truth, but both expressions
       must still evaluate for their effects. Checked before the literal-nil
       paths below so a non-literal nil-typed operand is not constant-folded
       away unevaluated. Receiver-then-arg order preserved by the C comma. */
    if (rt == TY_NIL && a0 == TY_NIL) {
      buf_puts(b, "((void)(");
      emit_expr(c, recv, b);
      buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b);
      buf_printf(b, "), %d)", eq ? 1 : 0);
      return 1;
    }
    /* `x == nil` / `x != nil` for any receiver */
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
      else if (ot == TY_STRING || ot == TY_MATCHDATA ||
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
    /* Struct instances compare by member value (CRuby Struct#==); a user
       override on the struct class still wins via the dispatch below. eql?
       shares the arm. */
    if (ty_is_object(rt) && rt == a0) {
      int scid = ty_object_class(rt);
      if (scid >= 0 && scid < c->nclasses && c->classes[scid].is_struct &&
          comp_method_in_chain(c, scid, "==", NULL) < 0) {
        ClassInfo *sci = &c->classes[scid];
        int ta = ++g_tmp, tb2 = ++g_tmp;
        buf_puts(b, eq ? "(" : "(!(");
        buf_printf(b, "({ sp_%s *_t%d = ", sci->c_name, ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%s *_t%d = ", sci->c_name, tb2); emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d == _t%d || (_t%d && _t%d", ta, tb2, ta, tb2);
        for (int j = 0; j < sci->nivars; j++) {
          const char *ivn = sci->ivars[j] + 1;   /* skip @ */
          TyKind ivt = sci->ivar_types[j];
          if (ivt == TY_INT || ivt == TY_BOOL || ivt == TY_SYMBOL || ivt == TY_FLOAT)
            buf_printf(b, " && _t%d->iv_%s == _t%d->iv_%s", ta, ivn, tb2, ivn);
          else if (ivt == TY_STRING)
            buf_printf(b, " && sp_str_eq(_t%d->iv_%s, _t%d->iv_%s)", ta, ivn, tb2, ivn);
          else if (ivt == TY_POLY)
            buf_printf(b, " && sp_poly_eq(_t%d->iv_%s, _t%d->iv_%s)", ta, ivn, tb2, ivn);
          else {
            /* boxed comparison covers arrays/hashes/objects uniformly */
            Buf ba; memset(&ba, 0, sizeof ba);
            char lx[128], rx[128];
            snprintf(lx, sizeof lx, "_t%d->iv_%s", ta, ivn);
            snprintf(rx, sizeof rx, "_t%d->iv_%s", tb2, ivn);
            buf_puts(b, " && sp_poly_eq(");
            emit_boxed_text(c, ivt, lx, b);
            buf_puts(b, ", ");
            emit_boxed_text(c, ivt, rx, b);
            buf_puts(b, ")");
            free(ba.p);
          }
        }
        buf_puts(b, "); })");
        buf_puts(b, eq ? ")" : "))");
        return 1;
      }
    }
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
        /* Different hash VARIANTS compare by value, not by storage kind:
           Ruby has one Hash, and a StrPolyHash built at runtime (JSON.parse)
           equals the same pairs written as a StrIntHash literal. Box both
           and let sp_poly_eq's cross-variant arm walk the pairs. */
        buf_puts(b, eq ? "sp_poly_eq(" : "(!sp_poly_eq(");
        emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
        buf_puts(b, eq ? ")" : "))");
        return 1;
      }
      if ((ty_is_hash(rt) || ty_is_hash(a0)) && rt != TY_POLY && a0 != TY_POLY) {
        /* hash vs a concrete non-hash: never equal. A poly operand instead
           falls through to the dynamic sp_poly_eq arm below (it may hold a
           hash at runtime -- the JSON.parse result compared against a hash
           literal used to constant-fold here to false). */
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
    /* a poly operand compares dynamically (covers string-vs-poly etc.) --
       EXCEPT when the typed object receiver's class defines #==: that method
       must dispatch (sp_poly_eq would compare object identity), e.g. the
       `self == o` body of a hash key's #eql?. == falls through to the generic
       user-method call; != (derived from == in Ruby) negates it here. */
    if (rt == TY_POLY || a0 == TY_POLY) {
      if (ty_is_object(rt) && !comp_ty_value_obj(c, rt)) {
        int ueq_def = -1;
        int ueq_mi = comp_method_in_chain(c, ty_object_class(rt), "==", &ueq_def);
        if (ueq_mi >= 0) {
          if (eq) return 0;
          Scope *um = &c->scopes[ueq_mi];
          LocalVar *up = um->nparams >= 1 ? scope_local(um, um->pnames[0]) : NULL;
          TyKind upt = (up && up->type != TY_UNKNOWN) ? up->type : TY_POLY;
          int uretb = (um->ret == TY_BOOL);
          buf_puts(b, "(!");
          if (!uretb) buf_puts(b, "sp_poly_truthy(");
          buf_printf(b, "sp_%s_%s((sp_%s *)(", c->classes[ueq_def].c_name,
                     mc(um->name), c->classes[ueq_def].c_name);
          emit_expr(c, recv, b);
          buf_puts(b, "), ");
          if (upt == TY_POLY) emit_boxed(c, argv[0], b);
          else emit_expr(c, argv[0], b);
          buf_puts(b, ")");
          if (!uretb) buf_puts(b, ")");
          buf_puts(b, ")");
          return 1;
        }
      }
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
        else if (fr == 6) { buf_puts(b, eq ? "sp_frange_eq(" : "(!sp_frange_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, eq ? ")" : "))"); }
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
        /* a `<=>` that statically returns a non-numeric non-nil value
           (String/Symbol/Bool) is never a valid comparison: Comparable#==
           raises ArgumentError (unlike a nil result, which is just false).
           Route through the checked comparator so it raises. */
        int cmi_eq = comp_method_in_chain(c, ecid, "<=>", NULL);
        TyKind cmpret = cmi_eq >= 0 ? (TyKind)c->scopes[cmi_eq].ret : TY_UNKNOWN;
        if (cmpret == TY_STRING || cmpret == TY_SYMBOL || cmpret == TY_BOOL) {
          int ta = hoist_boxed_rooted(c, recv), tb2 = hoist_boxed_rooted(c, argv[0]);
          buf_printf(b, "(%ssp_poly_cmp_ck(_t%d, _t%d) == 0)", eq ? "" : "!", ta, tb2);
          return 1;
        }
        /* a `<=>` that can return nil: Comparable#== semantics -- identity is
           equal, an incomparable pair is false (never an error) */
        if (user_cmp_needs_check(c, ecid)) {
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
    /* Time == / != via sp_time_cmp. A non-Time operand is never equal (CRuby
       compares Time only to Time); a poly operand is checked at runtime. */
    if (rt == TY_TIME) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (a0t == TY_TIME) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, eq ? "==" : "!=");
        return 1;
      }
      if (a0t == TY_POLY || a0t == TY_UNKNOWN) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tu); emit_boxed(c, argv[0], b);
        buf_printf(b, "; %s(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME && "
                      "sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p) == 0); })",
                   eq ? "" : "!", tu, tu, tt, tu);
        return 1;
      }
      /* a concrete non-Time operand: not equal, but evaluate it for effect */
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
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
    /* Exception#== / #!=: same class + same message; any non-exception
       operand is simply unequal (#2748). */
    if (recv >= 0 && rt == TY_EXCEPTION) {
      if (a0 == TY_EXCEPTION) {
        buf_printf(b, "(%ssp_exc_eq((sp_Exception *)(", eq ? "" : "!");
        emit_expr(c, recv, b);
        buf_puts(b, "), (sp_Exception *)(");
        emit_expr(c, argv[0], b); buf_puts(b, ")))");
      } else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_boxed(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
      }
      return 1;
    }
    /* MatchData#== : structural equality with another MatchData, else false (#2529) */
    if (recv >= 0 && rt == TY_MATCHDATA) {
      if (a0 == TY_MATCHDATA) {
        buf_printf(b, "(%ssp_MatchData_eq(", eq ? "" : "!"); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, "))");
      } else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_boxed(c, argv[0], b); buf_printf(b, "), %d)", eq ? 0 : 1);
      }
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
  buf_printf(b, "sp_%s *_tq%d = ", c->classes[cid].c_name, ta);
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
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative argument\");"
                    " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                    " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                    " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                 tn, tr, tr,
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
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative argument\");"
                      " sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d->start + _t%d]); _t%d; })",
                   tn, k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, ta, tj, tr);
      }
      else {
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative argument\");"
                      " sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);"
                      " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                      " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sArray_push(_t%d, _t%d->data[_t%d]); _t%d; })",
                   tn, k, tr, k, tr,
                   ti, ti, tn, ti,
                   tj, tj, ta, tj,
                   k, tr, ta, tj, tr);
      }
    }
    return 1;
  }

  /* Numeric coerce protocol: `recv <op> arg` where recv is a builtin numeric
     (Integer/Float) and arg is a user object defining coerce. CRuby asks the
     object to coerce: `a, b = arg.coerce(recv)` then computes `a.<op>(b)`. The
     standard `coerce` returns a pair of the object's own class, so dispatch the
     op on that class with both pair elements. */
  if (recv >= 0 && argc == 1 && (rt == TY_INT || rt == TY_FLOAT) &&
      ty_is_object(a0) && is_arith_op(name)) {
    int acls = ty_object_class(a0);
    int coerce_def = -1, op_def = -1;
    int coerce_mi = comp_method_in_chain(c, acls, "coerce", &coerce_def);
    int op_mi = comp_method_in_chain(c, acls, name, &op_def);
    if (coerce_mi >= 0 && op_mi >= 0) {
      int tp = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_%s_coerce(", tp, c->classes[coerce_def].c_name);
      emit_expr(c, argv[0], b);          /* the coercing object = coerce's self */
      buf_puts(b, ", ");
      emit_boxed(c, recv, b);            /* the numeric receiver, boxed as `other` */
      buf_printf(b, "); SP_GC_ROOT(_t%d); ", tp);
      /* a = pair[0] (the coercing class); dispatch a.<op>(b). */
      buf_printf(b, "sp_%s_%s((sp_%s *)_t%d->data[0].v.p, ",
                 c->classes[op_def].c_name, mc(name), c->classes[op_def].c_name, tp);
      /* b = pair[1], coerced to the op method's first parameter type. */
      TyKind pt = TY_POLY;
      if (c->scopes[op_mi].nparams > 0) {
        LocalVar *pv = scope_local(&c->scopes[op_mi], c->scopes[op_mi].pnames[0]);
        if (pv) pt = pv->type;
      }
      char pair1[64]; snprintf(pair1, sizeof pair1, "_t%d->data[1]", tp);
      if (ty_is_object(pt))
        buf_printf(b, "(sp_%s *)_t%d->data[1].v.p", c->classes[ty_object_class(pt)].c_name, tp);
      else if (pt == TY_POLY || pt == TY_UNKNOWN)
        buf_puts(b, pair1);
      else
        emit_unbox_text(c, pt, pair1, b);
      buf_puts(b, "); })");
      return 1;
    }
  }

  /* `[] + x` / `x - []`: an empty array literal operand leaves the expression
     UNKNOWN-typed, so `+`/`-` would land in the scalar-arith path though it is
     really an Array concat/difference. Defer to the array-call path. */
  if ((sp_streq(name, "+") || sp_streq(name, "-")) && argc == 1) {
    int re = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
             ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; });
    int ae = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode") &&
             ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; });
    if (re || ae) return 0;
  }

  if (recv >= 0 && argc == 1 && !ty_is_object(rt) && !ty_is_array(rt) &&
      (int_arith_fn(name) ||
       /* bigint shifts aren't "int arith" ops but lower through the same
          TY_BIGINT branch below (sp_bigint_shl / sp_bigint_shr). */
       (res == TY_BIGINT && (sp_streq(name, "<<") || sp_streq(name, ">>"))))) {
    /* An Integer/Bignum arith op coerces its argument via coerce/to_int; a
       String/Symbol/Array/Hash/nil/bool/Range has neither, so CRuby raises
       "X can't be coerced into Integer" rather than aborting compilation
       (#2471). Numeric args (int/float/bigint/rational/complex) fall through
       to the real arithmetic below. */
    if ((rt == TY_INT || rt == TY_BIGINT) && is_arith_op(name)) {
      TyKind at9 = comp_ntype(c, argv[0]);
      const char *cn9 =
        at9 == TY_STRING ? "String" : at9 == TY_SYMBOL ? "Symbol" :
        at9 == TY_NIL ? "nil" : ty_is_array(at9) ? "Array" :
        ty_is_hash(at9) ? "Hash" : at9 == TY_RANGE ? "Range" : NULL;
      if (at9 == TY_BOOL) {
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
        buf_puts(b, "); sp_raise_cls(\"TypeError\", sp_sprintf(\"%s can't be coerced into Integer\", (");
        emit_expr(c, argv[0], b);
        buf_puts(b, ") ? \"true\" : \"false\")); (mrb_int)0; })");
        return 1;
      }
      if (cn9) {
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
        emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_raise_cls(\"TypeError\", \"%s can't be coerced into Integer\"); (mrb_int)0; })", cn9);
        return 1;
      }
    }
    if (rt == TY_STRING && sp_streq(name, "+")) {
      /* `str + <non-string>` is a TypeError in CRuby (no implicit conversion);
         a statically non-string, non-poly argument raises rather than emitting
         an invalid sp_str_plus(str, int) C call (#2306) */
      {
        TyKind at9 = comp_ntype(c, argv[0]);
        const char *acn9 =
          at9 == TY_INT || at9 == TY_BIGINT ? "Integer" :
          at9 == TY_FLOAT ? "Float" : at9 == TY_SYMBOL ? "Symbol" :
          at9 == TY_NIL ? "nil" :   /* CRuby names the value, not NilClass */
          ty_is_array(at9) ? "Array" : ty_is_hash(at9) ? "Hash" :
          at9 == TY_RANGE ? "Range" : NULL;
        if (at9 == TY_BOOL) {
          /* true/false name the value at run time */
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
          buf_puts(b, "); sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %s into String\", (");
          emit_expr(c, argv[0], b);
          buf_puts(b, ") ? \"true\" : \"false\")); (&(\"\\xff\")[1]); })");
          return 1;
        }
        if (acn9) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
          emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into String\"); (&(\"\\xff\")[1]); })", acn9);
          return 1;
        }
      }
      /* Root both operands when either may allocate: `a + b` evaluates both,
         and a fresh heap string from one can be swept while the other
         allocates or forces a GC (chained `a + b + c` with side-effecting
         operands — concat_chain_operand_gc_root). Recurses naturally: a
         chain's left operand is itself a `+` and gets its own rooted block.
         Pure literal / bare-read operands need no rooting. */
      /* A poly operand (statically typed string here, holds a string at
         runtime) must be coerced to a C string for sp_str_concat. */
      /* emit_str_expr coerces both a TY_POLY operand (sp_poly_to_s) and the
         unresolved-call gate's sp_raise_nomethod(...) (which is sp_RbVal, not a
         const char*) to a C string, so a raise-all operand type-checks (#2457). */
      if (subtree_may_allocate(nt, recv) || subtree_may_allocate(nt, argv[0])) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", ta); emit_str_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); const char *_t%d = ", ta, tb);
        emit_str_expr(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_str_plus(_t%d, _t%d); })", tb, ta, tb);
      }
      else {
        buf_puts(b, "sp_str_plus(");
        emit_str_expr(c, recv, b); buf_puts(b, ", ");
        emit_str_expr(c, argv[0], b);
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
      else emit_scalar_operand(c, argv[0], "0", b);
      buf_puts(b, ")");
      return 1;
    }
    if (eff_res == TY_FLOAT && sp_streq(name, "**") && rt != TY_TIME) {
      TyKind at0 = argc > 0 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
      /* sp_float_pow raises loudly where CRuby would promote to a Complex
         (negative base, fractional exponent) instead of returning NaN */
      buf_puts(b, "sp_float_pow(");
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
      /* an integer zero divisor raises (5.0 % 0), a float one is NaN (5.0 % 0.0):
         route the int-divisor form through the checking helper. */
      buf_puts(b, at0 == TY_INT ? "sp_fmod_intdiv(" : "sp_fmod(");
      if (rt == TY_INT) { buf_puts(b, "(double)("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (eff_res == TY_FLOAT && rt != TY_TIME && !sp_streq(name, "%") && !sp_streq(name, "**")) {
      buf_puts(b, "(");
      emit_scalar_operand(c, recv, "0.0", b);
      buf_printf(b, " %s ", name);
      emit_scalar_operand(c, argv[0], "0.0", b);
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
      else if (sp_streq(name, "-") && at == TY_POLY) {
        /* Time - poly: the poly holds a Time at run time (a mixed collection
           whose element method returns Time). Unbox to sp_Time and subtract;
           a non-Time value raises TypeError (#2456). */
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tu); emit_boxed(c, argv[0], b);
        buf_printf(b, "; if (_t%d.tag != SP_TAG_OBJ || _t%d.cls_id != SP_BUILTIN_TIME)"
                      " sp_raise_cls(\"TypeError\", \"can't convert to Time\");"
                      " sp_time_sub_t(_t%d, *(sp_Time *)_t%d.v.p); })",
                   tu, tu, tt, tu);
      }
      else if (at == TY_FLOAT) {
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; double _t%d = ", tu); emit_expr(c, argv[0], b);
        if (sp_streq(name, "+"))
          buf_printf(b, "; sp_time_add_f(_t%d, _t%d); })", tt, tu);
        else
          buf_printf(b, "; sp_time_add_f(_t%d, -_t%d); })", tt, tu);
      }
      else if (at == TY_RATIONAL) {
        /* Time +/- Rational: add the fractional seconds through the float path.
           spinel's Time keeps nanosecond precision, so a Rational whose value
           is representable in ns (e.g. 1/2) round-trips exactly (#2678). */
        buf_printf(b, "({ sp_Time _t%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; double _t%d = sp_rational_to_f(", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_time_add_f(_t%d, %s_t%d); })", tt, sp_streq(name, "-") ? "-" : "", tu);
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

/* Would the class/module *object* with class id `ci` answer
   respond_to?(:qm)? Consults user class (singleton) methods, singleton
   attr readers/writers via `class << self`, a module's def'd
   (module_function) methods, then the builtin Class/Module methods every
   class object inherits (`new` answered by classes, not modules). Shared
   by the explicit `Const.respond_to?(:m)` fold and the receiverless form
   inside a `def self.x` method (implicit self = the class object). */
/* Does any user class's chain define qm (instance method, attr reader, or a
   `m=` attr writer)? Lets a poly respond_to?(:qm) decide whether a runtime
   cls_id check is needed instead of the union-wide static probe answer. */
static int any_class_defines(Compiler *c, const char *qm) {
  size_t ql = strlen(qm);
  char wbase[256]; wbase[0] = '\0';
  int is_wr = ql > 0 && qm[ql - 1] == '=' && ql - 1 < sizeof wbase;
  if (is_wr) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
  for (int k = 0; k < c->nclasses; k++)
    if (comp_method_in_chain(c, k, qm, NULL) >= 0 ||
        comp_reader_in_chain(c, k, qm, NULL) ||
        (is_wr && comp_writer_in_chain(c, k, wbase, NULL)))
      return 1;
  return 0;
}
static int class_responds_to(Compiler *c, int ci, const char *qm) {
  const NodeTable *nt = c->nt;
  if (comp_cmethod_in_chain(c, ci, qm, NULL) >= 0) return 1;
  /* singleton attr_accessor/reader/writer via class << self */
  size_t ql = strlen(qm);
  int is_wr = ql > 0 && qm[ql - 1] == '=';
  if (is_wr) {
    char base[256];
    if (ql - 1 < sizeof base) {
      memcpy(base, qm, ql - 1); base[ql - 1] = '\0';
      if (comp_is_sg_writer(&c->classes[ci], base)) return 1;
    }
  }
  else if (comp_is_sg_reader(&c->classes[ci], qm)) return 1;
  int dn = c->classes[ci].def_node;
  const char *dt = dn >= 0 ? nt_type(nt, dn) : NULL;
  int is_module = dt && sp_streq(dt, "ModuleNode");
  /* a module also responds to its def'd (module_function) methods */
  if (is_module && comp_method_in_chain(c, ci, qm, NULL) >= 0) return 1;
  /* builtin Class/Module methods every class object inherits */
  static const char *const cls_uni[] = {
    "name", "instance_methods", "public_instance_methods",
    "private_instance_methods", "protected_instance_methods",
    "instance_method", "method_defined?", "superclass", "ancestors",
    "include?", "const_get", "const_set", "const_defined?",
    "define_method", "allocate", "<", "<=", ">", ">=", NULL };
  for (int u = 0; cls_uni[u]; u++) if (sp_streq(qm, cls_uni[u])) return 1;
  /* `new`: a class responds, a module does not */
  if (sp_streq(qm, "new")) return !is_module;
  return 0;
}

/* Append the trailing `&block` argument (an sp_Proc *, or NULL) to a direct
   class-method call when the callee keeps a real &blk param and isn't
   yield-inlined -- otherwise the block is silently dropped and the callee's
   lv_blk dangles. When blk_tmp >= 0 the caller already materialized the proc
   temp (Stage-2 cascade: one proc shared by several candidate branches);
   otherwise the call's literal block (if any) is lowered here. */
static void emit_cmethod_block_arg(Compiler *c, int id, Scope *cm, int blk_tmp, Buf *b) {
  if (!cm->blk_param || !cm->blk_param[0] || cm->yields) return;
  int blk_node = resolve_forwarded_block(c, nt_ref(c->nt, id, "block"));
  if (cm->nparams > 0) buf_puts(b, ", ");
  if (blk_node < 0) { buf_puts(b, "NULL"); return; }
  /* `inner(child, &block)` from a REAL function (not a yield-inline splice):
     the caller's &blk is a live sp_Proc* local -- pass it through instead of
     lowering (a BlockArgumentNode is not a proc literal). An anonymous `&`
     forwards the caller's own blk param (#2444). */
  if (nt_type(c->nt, blk_node) && sp_streq(nt_type(c->nt, blk_node), "BlockArgumentNode")) {
    int fe = nt_ref(c->nt, blk_node, "expression");
    if (fe >= 0 && comp_ntype(c, fe) == TY_PROC) { emit_expr(c, fe, b); return; }
    if (fe < 0) {
      Scope *caller9 = comp_scope_of(c, id);
      if (caller9 && caller9->blk_param && caller9->blk_param[0] && !caller9->yields) {
        buf_printf(b, "lv_%s", caller9->blk_param); return;
      }
    }
    buf_puts(b, "NULL"); return;
  }
  if (blk_tmp < 0) {
    blk_tmp = ++g_tmp;
    Buf pb; memset(&pb, 0, sizeof pb);
    emit_proc_literal(c, blk_node, &pb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
    /* Root the proc box: an allocating argument evaluated after this hoist
       (or the callee prologue) may GC before the callee roots lv_blk. */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", blk_tmp);
    free(pb.p);
  }
  buf_printf(b, "_t%d", blk_tmp);
}

/* Emit a compile-time trampoline that lets a compiled Ruby method be passed as
   an ffi_callback C function pointer. It takes the C-ABI args the callback
   declares, converts each to the target method's inferred parameter type
   (boxing a poly param, casting a scalar one), calls the compiled method, and
   converts the result to the callback's return type. The definition is appended
   to g_procs; returns the trampoline's unique id. */
static int emit_ffi_cb_trampoline(Compiler *c, int cbidx, int mi) {
  FfiCallback *k = &c->ffi_callbacks[cbidx];
  Scope *ts = &c->scopes[mi];
  int tid = ++g_proc_counter;
  Buf *pb = &g_procs;
  buf_printf(pb, "static %s __sp_ffi_cb_%d(", ffi_c_type(k->ret_spec), tid);
  for (int i = 0; i < k->nargs; i++) {
    if (i) buf_puts(pb, ", ");
    buf_printf(pb, "%s _a%d", ffi_cb_arg_ctype(k->arg_specs[i]), i);
  }
  if (k->nargs == 0) buf_puts(pb, "void");
  buf_puts(pb, ") {\n  ");
  /* sp_<target>(converted args) */
  Buf call; memset(&call, 0, sizeof call);
  emit_method_cname(c, ts, &call);
  buf_puts(&call, "(");
  for (int i = 0; i < k->nargs; i++) {
    if (i) buf_puts(&call, ", ");
    const char *spec = k->arg_specs[i];
    TyKind p = TY_UNKNOWN;
    if (i < ts->nparams && ts->pnames[i]) {
      LocalVar *lv = scope_local(ts, ts->pnames[i]);
      if (lv) p = lv->type;
    }
    char a[16]; snprintf(a, sizeof a, "_a%d", i);
    if (p == TY_POLY || p == TY_UNKNOWN) {           /* param is sp_RbVal: box it */
      if (sp_streq(spec, "ptr"))                          buf_printf(&call, "sp_box_foreign_ptr((void *)%s)", a);
      else if (sp_streq(spec, "str"))                     buf_printf(&call, "sp_box_str(%s)", a);
      else if (sp_streq(spec, "float") || sp_streq(spec, "double")) buf_printf(&call, "sp_box_float(%s)", a);
      else                                                buf_printf(&call, "sp_box_int((mrb_int)%s)", a);
    }
    else if (p == TY_INT)    buf_printf(&call, sp_streq(spec, "ptr") ? "(mrb_int)(uintptr_t)%s" : "(mrb_int)%s", a);
    else if (p == TY_STRING) buf_printf(&call, "(const char *)%s", a);
    else if (p == TY_FLOAT)  buf_printf(&call, "(mrb_float)%s", a);
    else                     buf_printf(&call, "(mrb_int)%s", a);
  }
  buf_puts(&call, ")");
  /* convert the result to the callback's return type */
  if (sp_streq(k->ret_spec, "void")) {
    buf_printf(pb, "%s;\n}\n", call.p ? call.p : "");
  }
  else {
    const char *rc = ffi_c_type(k->ret_spec);
    TyKind rt = method_is_void(ts) ? TY_NIL : ts->ret;
    if (rt == TY_POLY || rt == TY_UNKNOWN) {
      if (sp_streq(k->ret_spec, "ptr"))                             buf_printf(pb, "return (%s)(%s).v.p;\n}\n", rc, call.p);
      else if (sp_streq(k->ret_spec, "float") || sp_streq(k->ret_spec, "double")) buf_printf(pb, "return (%s)(%s).v.f;\n}\n", rc, call.p);
      else                                                          buf_printf(pb, "return (%s)(%s).v.i;\n}\n", rc, call.p);
    }
    else if (rt == TY_NIL) buf_printf(pb, "%s; return (%s)0;\n}\n", call.p, rc);
    else                   buf_printf(pb, "return (%s)(%s);\n}\n", rc, call.p);
  }
  free(call.p);
  return tid;
}

/* Emit a callback-typed ffi argument. It must be a statically resolvable
   method(:name); anything else is rejected loudly (never a silent NULL). */
static void emit_ffi_callback_arg(Compiler *c, int cbidx, int argnode, Buf *out) {
  if (is_method_obj_call(c, argnode)) {
    int mi = method_obj_target_mi(c, argnode);
    if (mi >= 0) {
      int tid = emit_ffi_cb_trampoline(c, cbidx, mi);
      buf_printf(out, "&__sp_ffi_cb_%d", tid);
      return;
    }
  }
  unsupported(c, argnode, "ffi callback argument (need a statically resolvable method(:name))");
  buf_puts(out, "NULL");
}

/* A single-segment constant name is well-formed iff it begins with an uppercase
   ASCII letter (or any non-ASCII byte, conservatively treated as a valid Unicode
   start) and every remaining ASCII byte is a constant-identifier char. A scoped
   name (containing "::") is left to the flat lookup rather than diagnosed here.
   Returns 1 when the name is malformed -- CRuby raises NameError "wrong constant
   name" for these instead of answering false. */
static int const_name_is_wrong(const char *s) {
  if (strstr(s, "::")) return 0;
  if (!s[0]) return 1;
  unsigned char c0 = (unsigned char)s[0];
  if (c0 < 0x80 && !(c0 >= 'A' && c0 <= 'Z')) return 1;
  for (const char *p = s + 1; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    if (ch < 0x80 && !((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                       (ch >= '0' && ch <= '9') || ch == '_'))
      return 1;
  }
  return 0;
}

/* Emit a Math.<fn> argument as a C double. A statically numeric operand casts
   directly (hot path for trig-heavy code); anything else is boxed and coerced
   through sp_num_to_f, which raises TypeError for nil / String / non-numeric
   rather than the lenient sp_poly_to_f's 0.0 (or a String->double CC error). */
static void emit_math_arg(Compiler *c, int node, Buf *out) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_INT || t == TY_FLOAT) {
    buf_puts(out, "(mrb_float)("); emit_expr(c, node, out); buf_puts(out, ")");
    return;
  }
  if (t == TY_RATIONAL) {
    buf_puts(out, "sp_rational_to_f("); emit_expr(c, node, out); buf_puts(out, ")");
    return;
  }
  /* a Bignum converts to its exact double, not a truncated machine int (#2591) */
  if (t == TY_BIGINT) {
    buf_puts(out, "sp_bigint_to_double("); emit_expr(c, node, out); buf_puts(out, ")");
    return;
  }
  /* a Complex has no real conversion for a Math function: RangeError (#2571) */
  if (t == TY_COMPLEX) {
    buf_puts(out, "((void)("); emit_expr(c, node, out);
    buf_puts(out, "), (sp_raise_cls(\"RangeError\", \"can't convert Complex into Float\"), 0.0))");
    return;
  }
  buf_puts(out, "sp_num_to_f("); emit_boxed(c, node, out); buf_puts(out, ")");
}

/* Does class `cid` (or any ancestor) have a literal `include <mod_name>` in a
   class/module body? Compile-time mirror of the ancestors-table include scan,
   for folding is_a?(Comparable) / is_a?(Enumerable) on a statically-typed
   user instance (#2363). */
static int class_includes_module_named(Compiler *c, int cid, const char *mod_name) {
  const NodeTable *nt = c->nt;
  for (int cur = cid; cur >= 0; cur = c->classes[cur].parent) {
    for (int id = 0; id < nt->count; id++) {
      const char *ty = nt_type(nt, id);
      if (!ty || (!sp_streq(ty, "ClassNode") && !sp_streq(ty, "ModuleNode"))) continue;
      int cp = nt_ref(nt, id, "constant_path");
      const char *cn = cp >= 0 ? nt_str(nt, cp, "name") : NULL;
      if (!cn || comp_class_index(c, cn) != cur) continue;
      int body = nt_ref(nt, id, "body");
      int bn = 0; const int *stmts = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int k = 0; k < bn; k++) {
        const char *sty = nt_type(nt, stmts[k]);
        if (!sty || !sp_streq(sty, "CallNode")) continue;
        const char *nm = nt_str(nt, stmts[k], "name");
        if (!nm || !sp_streq(nm, "include") || nt_ref(nt, stmts[k], "receiver") >= 0) continue;
        int an = 0; const int *aa;
        int anode = nt_ref(nt, stmts[k], "arguments");
        aa = anode >= 0 ? nt_arr(nt, anode, "arguments", &an) : NULL;
        for (int j = 0; j < an; j++) {
          const char *anm = nt_type(nt, aa[j]) && (sp_streq(nt_type(nt, aa[j]), "ConstantReadNode") ||
                                                   sp_streq(nt_type(nt, aa[j]), "ConstantPathNode"))
                            ? nt_str(nt, aa[j], "name") : NULL;
          if (anm && sp_streq(anm, mod_name)) return 1;
        }
      }
    }
    if (c->classes[cur].parent == cur) break;
  }
  return 0;
}

void emit_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  /* `obj.extend(Mod)` on a statically-traceable object (its type is a
     synthesized singleton subclass) is done at compile time: the module's
     methods were transplanted into the subclass. The runtime call is a
     no-op; evaluate the receiver for effect. */
  {
    const char *cnm = nt_str(nt, id, "name");
    int crecv = nt_ref(nt, id, "receiver");
    if (cnm && sp_streq(cnm, "extend") && crecv >= 0) {
      TyKind crt = comp_ntype(c, crecv);
      if (ty_is_object(crt) && c->classes[ty_object_class(crt)].is_singleton_of) {
        buf_puts(b, "((void)("); emit_expr(c, crecv, b); buf_puts(b, "))");
        return;
      }
    }
  }
  /* A documented limit is reported before anything else looks at the call:
     otherwise an earlier arm reports it as a generic gap (or, for a bare
     `binding`, as a NameError) and the specific message never runs. */
  if (diagnose_unsupported_call(c, id)) return;
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
                   bsnm, bscn, default_value(comp_ntype(c, id)));
        return;
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
      if (ac0 == 0 && !diag_user_defines(c, "itself")) {
        emit_expr(c, nt_ref(nt, id, "receiver"), b);
        return;
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
        buf_printf(b, "({ mrb_int _t%d = ", t1); emit_expr(c, rv0, b);
        buf_printf(b, "; mrb_int _t%d = ", t2); emit_expr(c, av0[0], b);
        buf_printf(b, "; (_t%d != 0) == (_t%d != 0) ? sp_box_int(0) : sp_box_nil(); })", t1, t2);
        return;
      }
      if (rt0 == TY_NIL && at0 == TY_NIL) {
        buf_puts(b, "((void)("); emit_expr(c, rv0, b); buf_puts(b, "), (void)(");
        emit_expr(c, av0[0], b); buf_puts(b, "), sp_box_int(0))");
        return;
      }
      buf_puts(b, "((void)("); emit_expr(c, rv0, b); buf_puts(b, "), (void)(");
      emit_expr(c, av0[0], b); buf_puts(b, "), sp_box_nil())");
      return;
    }
    /* a boolean has no #=~ (so #!~ fails the same way): CRuby's NoMethodError.
       A program that REOPENS TrueClass/FalseClass with its own #=~ keeps it. */
    if (nm0 && (sp_streq(nm0, "=~") || sp_streq(nm0, "!~")) && ac0 == 1 && rt0 == TY_BOOL &&
        !diag_user_defines(c, "=~")) {
      int t1 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", t1); emit_expr(c, rv0, b);
      buf_printf(b, "; (void)("); emit_expr(c, av0[0], b);
      buf_printf(b, "); sp_raise_cls(\"NoMethodError\", _t%d ?"
                    " (&(\"\\xff\" \"undefined method '=~' for true\")[1])"
                    " : (&(\"\\xff\" \"undefined method '=~' for false\")[1])); 0; })", t1);
      return;
    }
    /* TrueClass.new / FalseClass.new / NilClass.new: no allocator */
    if (nm0 && sp_streq(nm0, "new") && rv0 >= 0 && nt_kind(nt, rv0) == NK_ConstantReadNode) {
      const char *cn0 = nt_str(nt, rv0, "name");
      if (cn0 && (sp_streq(cn0, "TrueClass") || sp_streq(cn0, "FalseClass") ||
                  sp_streq(cn0, "NilClass"))) {
        buf_printf(b, "(sp_raise_cls(\"NoMethodError\","
                      " (&(\"\\xff\" \"undefined method 'new' for class %s\")[1])), 0)", cn0);
        return;
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
                     icn, default_value(comp_ntype(c, id)));
          return;
        }
        int t1 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", t1); emit_expr(c, rv0, b);
        buf_printf(b, "; sp_raise_cls(\"ArgumentError\", _t%d ?"
                      " (&(\"\\xff\" \"can't unfreeze TrueClass\")[1])"
                      " : (&(\"\\xff\" \"can't unfreeze FalseClass\")[1])); %s; })",
                   t1, default_value(comp_ntype(c, id)));
        return;
      }
    }
  }
  if (emit_dynamic_send(c, id, b)) return;   /* recv.send(runtime_name, args) static dispatch */
  /* a public_send-lowered call (stamped by the desugars): a private or
     protected target raises NoMethodError like CRuby, which enforces
     visibility on public_send regardless of caller context. The receiver
     still evaluates for its effects; the never-taken comma value keeps the
     expression's static type. */
  if (nt_str(nt, id, "vis_enforce")) {
    const char *vnm = nt_str(nt, id, "name");
    int vrecv = nt_ref(nt, id, "receiver");
    int vcid = -1;
    if (vrecv >= 0) {
      TyKind vrt = comp_ntype(c, vrecv);
      if (ty_is_object(vrt)) vcid = ty_object_class(vrt);
    }
    else {
      Scope *vs = comp_scope_of(c, id);
      if (vs && vs->class_id >= 0 && !vs->is_cmethod) vcid = vs->class_id;
    }
    if (vnm && vcid >= 0) {
      int vis = comp_method_vis_in_chain(c, vcid, vnm);
      if (vis != SP_VIS_PUBLIC) {
        const char *vrn = class_ruby_name(c, vcid) ? class_ruby_name(c, vcid) : c->classes[vcid].name;
        buf_puts(b, "(");
        if (vrecv >= 0) { buf_puts(b, "(void)("); emit_expr(c, vrecv, b); buf_puts(b, "), "); }
        buf_printf(b, "sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"%s method '%s' called for an instance of %s\")[1])), %s)",
                   vis == SP_VIS_PRIVATE ? "private" : "protected", vnm, vrn,
                   default_value(comp_ntype(c, id)));
        return;
      }
    }
  }
  /* k = Struct.new(:a, :b): the registered anonymous struct class, as a
     first-class class value */
  {
    int aci = anon_struct_ci_for_value(c, id);
    if (aci >= 0) {
      /* A duplicate member name is an ArgumentError at definition time in
         CRuby; raise at runtime so a surrounding rescue can catch it (#2705). */
      const char *dup = struct_call_dup_member(c, id);
      if (dup) {
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\", \"duplicate member: %s\"), (sp_Class){%d})", dup, aci);
        return;
      }
      buf_printf(b, "((sp_Class){%d})", aci); return;
    }
  }
  /* push/append/<< on an empty array literal in value position: the literal
     has no storage to mutate and returns self, so `[].push(1, 2)` is just the
     array `[1, 2]`. Materialize a fresh poly array from the args (the empty
     literal receiver infers TY_POLY_ARRAY). */
  {
    const char *pnm = nt_str(nt, id, "name");
    int precv = nt_ref(nt, id, "receiver");
    if (pnm && precv >= 0 &&
        (sp_streq(pnm, "push") || sp_streq(pnm, "append") || sp_streq(pnm, "<<") ||
         /* on an EMPTY literal, unshift/prepend build the same array (#2364) */
         sp_streq(pnm, "unshift") || sp_streq(pnm, "prepend")) &&
        nt_type(nt, precv) && sp_streq(nt_type(nt, precv), "ArrayNode") &&
        ({ int _n = 0; nt_arr(nt, precv, "elements", &_n); _n == 0; })) {
      int pargc = 0; const int *pargv = call_args(nt, id, &pargc);
      if (pargc >= 1) {
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tr, tr);
        for (int a = 0; a < pargc; a++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", tr); emit_boxed(c, pargv[a], b); buf_puts(b, "); ");
        }
        buf_printf(b, "_t%d; })", tr);
        return;
      }
    }
  }
  /* unshift/prepend a nil onto a typed numeric array literal: nil cannot live
     in an Int/Float array (it would store 0), so widen the whole thing to a
     poly array. `[1, 2].unshift(nil)` == the poly array `[nil, 1, 2]`. Only the
     direct-literal receiver needs this -- a variable receiver is already
     widened by the analyze pass. */
  {
    const char *unm = nt_str(nt, id, "name");
    int urecv = nt_ref(nt, id, "receiver");
    if (unm && urecv >= 0 && (sp_streq(unm, "unshift") || sp_streq(unm, "prepend")) &&
        nt_type(nt, urecv) && sp_streq(nt_type(nt, urecv), "ArrayNode")) {
      TyKind urt = comp_ntype(c, urecv);
      int uargc = 0; const int *uargv = call_args(nt, id, &uargc);
      int has_nil = 0;
      for (int a = 0; a < uargc; a++) {
        TyKind at = infer_type(c, uargv[a]);
        const char *anty = nt_type(nt, uargv[a]);
        if (at == TY_NIL || (anty && sp_streq(anty, "NilNode"))) { has_nil = 1; break; }
      }
      if (has_nil && (urt == TY_INT_ARRAY || urt == TY_FLOAT_ARRAY) && uargc >= 1) {
        int en = 0; const int *elems = nt_arr(nt, urecv, "elements", &en);
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tr, tr);
        for (int a = 0; a < uargc; a++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", tr); emit_boxed(c, uargv[a], b); buf_puts(b, "); ");
        }
        for (int e = 0; e < en; e++) {
          buf_printf(b, "sp_PolyArray_push(_t%d, ", tr); emit_boxed(c, elems[e], b); buf_puts(b, "); ");
        }
        buf_printf(b, "_t%d; })", tr);
        return;
      }
    }
  }
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
  /* Enumerating a float-begin range raises like CRuby ("can't iterate from
     Float"): the int-backed sp_Range would otherwise silently truncate and
     walk integers. `step` legitimately iterates from a Float, and tap/then
     never enumerate; everything else block-taking on such a range raises. */
  {
    int rcv = nt_ref(nt, id, "receiver");
    const char *cn = nt_str(nt, id, "name");
    /* Range#bsearch over a BOUNDED float range with a truthy (non-find-any)
       block is handled by emit_bsearch_expr's float-bisection branch, so it is
       exempt from the float-iteration reject; a beginless/endless or find-any
       (int-block) bsearch still reaches the reject like other iterations. */
    int bsearch_float_ok = 0;
    if (rcv >= 0 && cn && sp_streq(cn, "step") == 0 && sp_streq(cn, "bsearch")) {
      int brn = unwrap_parens(c, rcv);
      if (brn >= 0 && nt_type(nt, brn) && !sp_streq(nt_type(nt, brn), "RangeNode"))
        brn = local_sole_range_node(c, brn);
      int bl = brn >= 0 ? nt_ref(nt, brn, "left") : -1;
      int br = brn >= 0 ? nt_ref(nt, brn, "right") : -1;
      int bblk = nt_ref(nt, id, "block");
      int bbd = bblk >= 0 ? nt_ref(nt, bblk, "body") : -1;
      int bbn = 0; const int *bbb = bbd >= 0 ? nt_arr(nt, bbd, "body", &bbn) : NULL;
      if (bl >= 0 && br >= 0 && bbn >= 1 && infer_type(c, bbb[bbn - 1]) != TY_INT)
        bsearch_float_ok = 1;
      /* a distinct-typed float range bsearch (variable or literal) bisects too */
      if (rcv >= 0 && comp_ntype(c, rcv) == TY_FLOAT_RANGE) bsearch_float_ok = 1;
    }
    if (rcv >= 0 && cn && nt_ref(nt, id, "block") >= 0 &&
        !sp_streq(cn, "step") && !sp_streq(cn, "tap") &&
        !sp_streq(cn, "then") && !sp_streq(cn, "yield_self") &&
        !bsearch_float_ok &&
        ((comp_ntype(c, rcv) == TY_RANGE && range_float_begin(c, rcv)) ||
         comp_ntype(c, rcv) == TY_FLOAT_RANGE)) {
      const char *dv = default_value(comp_ntype(c, id));
      buf_printf(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from Float\"); %s; })",
                 dv ? dv : "0");
      return;
    }
  }
  if (emit_lazy_size_expr(c, id, b)) return;
  if (emit_lazy_pipeline_expr(c, id, b)) return;
  if (emit_partition_expr(c, id, b)) return;
  if (emit_with_index_expr(c, id, b)) return;
  if (emit_enum_with_index_expr(c, id, b)) return;
  if (emit_each_with_index_chain(c, id, b)) return;
  if (emit_each_with_index_terminal(c, id, b)) return;
  if (emit_collect_expr(c, id, b)) return;
  if (emit_predicate_expr(c, id, b)) return;
  if (emit_grep_expr(c, id, b)) return;
  if (emit_minmax_by_expr(c, id, b)) return;
  if (emit_flat_map_expr(c, id, b)) return;
  if (emit_filter_map_expr(c, id, b)) return;
  if (emit_poly_uniq_block(c, id, b)) return;
  if (emit_iter_value_expr(c, id, b)) return;
  if (emit_sort_cmp_expr(c, id, b)) return;
  if (emit_minmax_cmp_expr(c, id, b)) return;
  if (emit_step_array_expr(c, id, b)) return;
  if (emit_chunk_while_expr(c, id, b)) return;
  if (emit_chunk_family_poly_expr(c, id, b)) return;
  if (emit_chunk_family_enum_expr(c, id, b)) return;
  if (emit_chunk_first_class_expr(c, id, b)) return;
  if (emit_cycle_bounded_expr(c, id, b)) return;
  if (emit_slice_when_chunk_inspect_expr(c, id, b)) return;
  if (emit_product_inspect_expr(c, id, b)) return;
  if (emit_bsearch_expr(c, id, b)) return;
  if (emit_sum_block_poly_expr(c, id, b)) return;
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

  /* x.is_a?(NoSuchConst): the is_a?-family arms below match the target
     ConstantReadNode textually and never emit the constant read, so an
     UNDEFINED constant silently answered false where CRuby raises NameError
     at the read. Catch it here, before any arm: a name that is no user
     class/module, no builtin, and no defined value-constant cannot exist at
     runtime (whole-program), so evaluate the receiver for effect and raise. */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
       sp_streq(name, "instance_of?")) &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")) {
    const char *tcn = nt_str(nt, argv[0], "name");
    /* Names CRuby defines that is_builtin_class_name doesn't list (the
       is_a? arms below handle several of them by TY kind). The list errs
       generous: a CRuby constant missing here just keeps the old false
       answer; only a name in NO list may raise, so a wrong raise on real
       code is impossible for listed names. */
    static const char *const CRUBY_KNOWN[] = {
      "Enumerator", "Struct", "Data", "Random", "Queue", "SizedQueue",
      "ConditionVariable", "Dir", "Set", "Marshal", "Binding", "Warning",
      "Errno", "EOFError", "SystemExit", "Interrupt", "SignalException",
      "SyntaxError", "SystemStackError", "SecurityError", "EncodingError",
      "RegexpError", "FiberError", "ThreadError", "UncaughtThrowError",
      "ClosedQueueError", "NoMatchingPatternError",
      "NoMatchingPatternKeyError", "SystemCallError", "Ractor", "ARGF",
      NULL };
    int cruby_known = 0;
    for (int ki = 0; tcn && CRUBY_KNOWN[ki]; ki++)
      if (sp_streq(tcn, CRUBY_KNOWN[ki])) { cruby_known = 1; break; }
    if (tcn && !cruby_known && comp_class_index(c, tcn) < 0 &&
        !is_builtin_class_name(tcn) && !comp_const(c, tcn)) {
      buf_puts(b, "((void)(");
      emit_expr(c, recv, b);
      buf_printf(b, "), sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), 0)", tcn);
      return;
    }
  }

  /* $~[N]: the Nth regexp group of the last match (0 = the whole match), read
     from the match registers. $~ is a special regexp accessor rather than
     stored MatchData, so index it directly instead of char-indexing a string. */
  {
    const char *rvty0 = recv >= 0 ? nt_type(nt, recv) : NULL;
    int recv_is_tilde = rvty0 &&
        (sp_streq(rvty0, "GlobalVariableReadNode") || sp_streq(rvty0, "BackReferenceReadNode")) &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "$~");
    if (recv_is_tilde && sp_streq(name, "[]") && argc == 1) {
      buf_puts(b, "({ mrb_int _mi = "); emit_int_expr(c, argv[0], b);
      buf_puts(b, "; _mi == 0 ? sp_re_match_str : (_mi >= 1 && _mi <= 9 ? sp_re_captures[_mi] : (const char *)0); })");
      return;
    }
    /* $~'s MatchData face over the match registers: pre/post_match and to_s
       read the same backing the $` / $' / $& back-references use. */
    if (recv_is_tilde && argc == 0) {
      if (sp_streq(name, "pre_match"))  { buf_puts(b, "sp_re_match_pre");  return; }
      if (sp_streq(name, "post_match")) { buf_puts(b, "sp_re_match_post"); return; }
      if (sp_streq(name, "to_s"))       { buf_puts(b, "sp_re_match_str");  return; }
    }
  }

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
      /* a value-less `break` (or none at all) makes the loop's value nil:
         ride the poly slot so the nil default is the result */
      if (bt == TY_UNKNOWN || bt == TY_NIL) bt = TY_POLY;
      {
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", t,
                   bt == TY_RANGE ? "(sp_Range){0}" : default_value(bt));
        /* Kernel#loop rescues StopIteration to terminate; wrap in a setjmp. */
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_rootmark[sp_exc_top] = sp_gc_nroots;\n");
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
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
  if (recv < 0 && sp_streq(name, "catch") && argc <= 1) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      TyKind bt = comp_ntype(c, id);
      /* NIL: a body whose tail is a break-less loop; ride the int slot (0). */
      if (bt == TY_UNKNOWN || bt == TY_VOID || bt == TY_NIL) bt = TY_INT;
      int ptr = proc_slot_is_ptr(bt);
      int t = ++g_tmp;
      emit_indent(g_pre, g_indent); emit_ctype(c, bt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", t, default_value(bt));
      int tag_kind = 0;
      if (argc == 1) {
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "sp_catch_tag[sp_catch_top] = ");
        tag_kind = emit_catch_tag(c, argv[0], g_pre);
        buf_puts(g_pre, ";\n");
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
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_catch_tag_kind[sp_catch_top] = %d;\n", tag_kind);
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
      return;
    }
  }

  /* throw :tag[, val] -> non-local jump to the matching catch scope. */
  if (recv < 0 && sp_streq(name, "throw")) {
    int tag_kind = 0;
    Buf tb; memset(&tb, 0, sizeof tb);
    if (argc >= 1) tag_kind = emit_catch_tag(c, argv[0], &tb);
    else buf_puts(&tb, "(&(\"\\xff\")[1])");
    buf_printf(b, "sp_throw(%s, %d, ", tb.p ? tb.p : "", tag_kind);
    free(tb.p);
    if (argc >= 2) emit_boxed(c, argv[1], b);
    else buf_puts(b, "sp_box_nil()");
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
  /* trap(...) / Signal.trap(...): install the handler (a string command or a
     proc/block), validate the designator, and return the previous handler
     (#2736, #2737, #2749). */
  {
    int is_trap = (recv < 0 && sp_streq(name, "trap"));
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
      return;
    }
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
                     kcn, default_value(comp_ntype(c, id)));
          return;
        }
      }
      /* arity: the key predicates take exactly 1 (#2831) */
      if (enm && eac != 1 &&
          (sp_streq(enm, "key?") || sp_streq(enm, "has_key?") ||
           sp_streq(enm, "include?") || sp_streq(enm, "member?"))) {
        buf_puts(b, "(");
        for (int q = 0; q < eac; q++) { buf_puts(b, "(void)("); emit_expr(c, eav[q], b); buf_puts(b, "), "); }
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\","
                      " (&(\"\\xff\" \"wrong number of arguments (given %d, expected 1)\")[1])), %s)",
                   eac, default_value(comp_ntype(c, id)));
        buf_puts(b, ")");
        return;
      }
      /* arity: [] takes 1; fetch takes 1..2 (#2773) */
      if (enm && sp_streq(enm, "[]") && eac != 1) {
        buf_puts(b, "(");
        for (int q = 0; q < eac; q++) { buf_puts(b, "(void)("); emit_expr(c, eav[q], b); buf_puts(b, "), "); }
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\","
                      " (&(\"\\xff\" \"wrong number of arguments (given %d, expected 1)\")[1])), %s)",
                   eac, default_value(comp_ntype(c, id)));
        buf_puts(b, ")");
        return;
      }
      if (enm && sp_streq(enm, "fetch") && (eac == 0 || eac > 2) && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "(");
        for (int q = 0; q < eac; q++) { buf_puts(b, "(void)("); emit_expr(c, eav[q], b); buf_puts(b, "), "); }
        buf_printf(b, "(sp_raise_cls(\"ArgumentError\","
                      " (&(\"\\xff\" \"wrong number of arguments (given %d, expected 1..2)\")[1])), %s)",
                   eac, default_value(comp_ntype(c, id)));
        buf_puts(b, ")");
        return;
      }
      /* the remaining query/mutation surface (#2832) */
      if (enm && sp_streq(enm, "class") && eac == 0) {
        buf_puts(b, "((sp_Class){(mrb_int)-116, \"Object\"})");   /* ENV is an Object singleton */
        return;
      }
      if (enm && sp_streq(enm, "frozen?") && eac == 0) {
        buf_puts(b, "((mrb_bool)0)");
        return;
      }
      if (enm && sp_streq(enm, "clear") && eac == 0) {
        buf_puts(b, "sp_env_clear()");
        return;
      }
      if (enm && sp_streq(enm, "shift") && eac == 0) {
        buf_puts(b, "sp_env_shift()");
        return;
      }
      if (enm && (sp_streq(enm, "update") || sp_streq(enm, "merge!") ||
                  sp_streq(enm, "replace")) && eac == 1 &&
          nt_ref(nt, id, "block") < 0) {
        TyKind ht2 = comp_ntype(c, eav[0]);
        const char *hty2 = nt_type(nt, eav[0]);
        if (ht2 == TY_STR_STR_HASH ||
            (hty2 && (sp_streq(hty2, "HashNode") || sp_streq(hty2, "KeywordHashNode")))) {
          buf_printf(b, "sp_env_update_h(");
          emit_expr(c, eav[0], b);
          buf_printf(b, ", %d)", sp_streq(enm, "replace") ? 1 : 0);
          return;
        }
      }
      if (enm && eac == 0 && nt_ref(nt, id, "block") >= 0 &&
          (sp_streq(enm, "delete_if") || sp_streq(enm, "reject!") ||
           sp_streq(enm, "keep_if") || sp_streq(enm, "select!") ||
           sp_streq(enm, "filter!"))) {
        int keep2 = sp_streq(enm, "keep_if") || sp_streq(enm, "select!") ||
                    sp_streq(enm, "filter!");
        /* the bang trio answers nil when nothing changed (#2844) */
        int optn = sp_streq(enm, "reject!") || sp_streq(enm, "select!") ||
                   sp_streq(enm, "filter!");
        buf_printf(b, "sp_env_filter_bang%s(", optn ? "_opt" : "");
        emit_proc_literal(c, id, b);
        buf_printf(b, ", %d)", keep2);
        return;
      }
      if (enm && sp_streq(enm, "to_s") && eac == 0) {
        buf_puts(b, "(&(\"\\xff\" \"ENV\")[1])");
        return;
      }
      if (enm && (sp_streq(enm, "dup") || sp_streq(enm, "clone")) && eac == 0) {
        buf_printf(b, "(sp_raise_cls(\"TypeError\","
                      " (&(\"\\xff\" \"Cannot %s ENV, use ENV.to_h to get a copy of ENV as a hash\")[1])), sp_box_nil())",
                   enm);
        return;
      }
      if (enm && sp_streq(enm, "freeze") && eac == 0) {
        buf_puts(b, "(sp_raise_cls(\"TypeError\", (&(\"\\xff\" \"cannot freeze ENV\")[1])), sp_box_nil())");
        return;
      }
      /* ENV.delete(k): the removed value (or nil) */
      if (enm && sp_streq(enm, "delete") && eac == 1) {
        int t1 = ++g_tmp, t2 = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", t1); emit_expr(c, eav[0], b);
        buf_printf(b, "; const char *_t%d = getenv(_t%d);"
                      " _t%d = _t%d ? sp_str_dup_external(_t%d) : NULL;"
                      " unsetenv(_t%d); _t%d; })", t2, t1, t2, t2, t2, t1, t2);
        return;
      }
      /* ENV.store(k, v) is []= */
      if (enm && sp_streq(enm, "store") && eac == 2) {
        buf_puts(b, "sp_env_aset(");
        emit_expr(c, eav[0], b); buf_puts(b, ", ");
        emit_boxed(c, eav[1], b); buf_puts(b, ")");
        return;
      }
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
        TyKind evt = comp_ntype(c, argv[1]);
        const char *v1ty = nt_type(nt, argv[1]);
        int lit_nil = v1ty && sp_streq(v1ty, "NilNode");
        if (evt == TY_STRING || lit_nil) {
          int tk = ++g_tmp, tv = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", tk); emit_str_expr(c, argv[0], b);
          buf_printf(b, "; const char *_t%d = ", tv); emit_str_expr(c, argv[1], b);
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
        return;
      }
    }
  }
  /* ENV.size/count/length -> environ entry count */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "size") || sp_streq(name, "count") || sp_streq(name, "length"))) {
    const char *rty2 = nt_type(nt, recv);
    if (rty2 && sp_streq(rty2, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) {
        buf_puts(b, "sp_env_size()");
        return;
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
        buf_puts(b, "(getenv("); emit_expr(c, argv[0], b); buf_puts(b, ") != NULL)");
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
        int tk = ++g_tmp, tky = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tky); emit_str_expr(c, argv[0], b);
        buf_printf(b, "; const char *_t%d = getenv(_t%d)", tk, tky);
        buf_printf(b, "; const char *_t%d = _t%d ? sp_str_dup_external(_t%d) : ", tv, tk, tk);
        if (argc >= 2) emit_expr(c, argv[1], b);
        else
          /* no default: CRuby raises KeyError naming the key */
          buf_printf(b, "(sp_raise_cls(\"KeyError\", sp_sprintf(\"key not found: \\\"%%s\\\"\", _t%d)), (const char *)0)", tky);
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
    if (is_literal) {
      /* proc(&x) / Proc.new(&x): the block is a forwarded proc, not a literal.
         Ruby returns that proc as-is (preserving its lambda? flag), so emit the
         forwarded expression directly rather than wrapping it in a fresh
         non-lambda proc. */
      int _blk = nt_ref(nt, id, "block");
      const char *_bty = nt_type(nt, _blk);
      if (_bty && sp_streq(_bty, "BlockArgumentNode")) {
        int _fwd = nt_ref(nt, _blk, "expression");
        /* forwarding the enclosing (inlined) method's block param
           (`Proc.new(&b)` inside `def make(&b)`): the real block is the
           literal active at the inline splice, so materialize THAT --
           the param's own name does not exist in the spliced context. */
        const char *_fnm = (_fwd >= 0 && nt_type(nt, _fwd) &&
                            sp_streq(nt_type(nt, _fwd), "LocalVariableReadNode"))
                           ? nt_str(nt, _fwd, "name") : NULL;
        if (_fnm && g_block_id >= 0 && g_block_param_name &&
            sp_streq(_fnm, g_block_param_name)) {
          emit_proc_literal(c, g_block_id, b);
          return;
        }
        if (_fwd >= 0 && comp_ntype(c, _fwd) == TY_PROC) { emit_expr(c, _fwd, b); return; }
        if (g_block_id >= 0) { emit_proc_literal(c, g_block_id, b); return; }
      }
      emit_proc_literal(c, id, b); return;
    }
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
      /* A concretely-typed OBJECT receiver is still a nullable C pointer
         (a nil-able ivar like doom's `@combat&.sprites` after death):
         dropping the `&.` deref'd NULL. The same holds for a concrete
         STRING receiver (NULL is the string nil, e.g. the nil arm of a
         chained `obj&.field&.length`). Emit a guard, then re-enter the
         normal call emission with the receiver substituted by the guarded
         temp (via the arg-override table); g_sn_skip suppresses this block
         on re-entry. */
      int sn_obj = ty_is_object(rrt) && !comp_ty_value_obj(c, rrt);
      if ((sn_obj || rrt == TY_STRING) && g_sn_skip != id) {
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
        else
          buf_printf(g_pre, "const char *_sn%d = %s; SP_GC_ROOT_STR(_sn%d);\n",
                     tsn2, rsn.p ? rsn.p : "NULL", tsn2);
        free(rsn.p);
        buf_printf(b, "(_sn%d == NULL ? ", tsn2);
        if (ret2 == TY_POLY) buf_puts(b, "sp_box_nil()");
        else if (ret2 == TY_INT) buf_puts(b, "SP_INT_NIL");
        else if (ret2 == TY_FLOAT) buf_puts(b, "sp_float_nil()");
        else if (ret2 == TY_STRING) buf_puts(b, "((const char *)NULL)");  /* string nil, not "" */
        else buf_puts(b, default_value(ret2) ? default_value(ret2) : "0");
        buf_puts(b, " : (");
        if (g_n_argov < MAX_ARG_OVERRIDE) {
          int slot2 = g_n_argov++;
          g_argov_node[slot2] = recv;
          snprintf(g_argov_text[slot2], sizeof g_argov_text[0], "_sn%d", tsn2);
          int sv_skip = g_sn_skip; g_sn_skip = id;
          emit_expr(c, id, b);
          g_sn_skip = sv_skip;
          g_n_argov--;
        }
        else emit_expr(c, recv, b);  /* override table full: degrade to unguarded */
        buf_puts(b, "))");
        return;
      }
      /* other concrete receivers (scalars, value types): never nil, dispatch as normal */
    }
  }

  /* range.step(n) { } in expression position: run the loop, evaluate to the
     receiver range (Ruby returns self) (#2415) */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 && comp_ntype(c, recv) == TY_RANGE &&
      sp_streq(name, "step")) {
    buf_puts(b, "({ ");
    emit_iteration_stmt(c, id, b, 0);
    emit_expr(c, recv, b); buf_puts(b, "; })");
    return;
  }
  /* n.times/upto/downto/step { ... } in expression position: run the loop
     (lowered to a statement) and evaluate to the receiver (Ruby returns self).
     A Rational receiver only steps. */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      ((comp_ntype(c, recv) == TY_INT &&
        (sp_streq(name, "times") || sp_streq(name, "upto") ||
         sp_streq(name, "downto") || sp_streq(name, "step"))) ||
       (comp_ntype(c, recv) == TY_RATIONAL && sp_streq(name, "step")))) {
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
      /* a Float limit is not truncated: n.upto(2.5) stops at 2, i.e. floor. */
      int lf = comp_ntype(c, argv[0]) == TY_FLOAT;
      buf_puts(b, "(sp_Range){ .first = "); emit_expr(c, recv, b);
      buf_puts(b, ", .last = ");
      if (lf) buf_puts(b, "(mrb_int)floor(");
      emit_expr(c, argv[0], b);
      if (lf) buf_puts(b, ")");
      buf_puts(b, ", .excl = 0 }");
      return;
    }
    if (sp_streq(name, "downto") && argc == 1) {
      /* descending: first=hi(recv), last=lo(arg), step=-1 -- an ascending range
         cannot carry the direction, which its .to_a would lose. A Float limit
         is not truncated: n.downto(1.5) stops at 2, i.e. ceil. */
      int lf = comp_ntype(c, argv[0]) == TY_FLOAT;
      buf_puts(b, "sp_range_new_step("); emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (lf) buf_puts(b, "(mrb_int)ceil(");
      emit_expr(c, argv[0], b);
      if (lf) buf_puts(b, ")");
      buf_puts(b, ", 0, -1LL)");
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
      /* Hoist every argument to a rooted temp and publish it (boxed) to the
         proc poly-arg side-channel, mirroring the general proc-call ABI. The
         callee may be a poly-signatured Proc whose params read from that
         side-channel (`cb.call(5)` where cb was read out of a container and its
         param unified to poly), so even an Integer arg must be published, not
         just left in the mrb_int slot (#2883). A poly arg that is itself a
         side-effecting call is also evaluated exactly once this way (#2874). */
      int *aptmp = (argc && !mabi_poly) ? calloc(argc, sizeof(int)) : NULL;
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
          if (storable) emit_ctype(c, at, g_pre); else buf_puts(g_pre, "mrb_int");
          buf_printf(g_pre, " _t%d = %s;\n", aptmp[k], valb.p ? valb.p : "0");
          if (at == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", aptmp[k]); }
          else if (proc_slot_is_ptr(at)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", aptmp[k]); }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "_sp_proc_poly_args[%d] = ", k);
          { char tn[24]; snprintf(tn, sizeof tn, "_t%d", aptmp[k]);
            if (storable) emit_boxed_text(c, at, tn, g_pre); else buf_puts(g_pre, "sp_box_nil()"); }
          buf_puts(g_pre, ";\n");
          free(inner.p); free(valb.p);
        }
      }
      #define EMIT_POLY_CALL_SLOT(k) do { \
        TyKind _at = comp_ntype(c, argv[k]); \
        if (aptmp) { \
          if (_at == TY_POLY) buf_printf(b, "sp_poly_to_i(_t%d)", aptmp[k]); \
          else if (proc_slot_is_ptr(_at)) buf_printf(b, "(mrb_int)(uintptr_t)_t%d", aptmp[k]); \
          else if (_at == TY_FLOAT) buf_puts(b, "0"); \
          else buf_printf(b, "_t%d", aptmp[k]); \
        } else emit_expr(c, argv[k], b); \
      } while (0)
      /* both arms yield a BOXED result now that the call types poly: the
         Method arm's legacy int ABI result boxes, the Proc arm reads the
         boxed return slot intact */
      buf_printf(b, "(_t%d.cls_id == SP_BUILTIN_METHOD ? %s((%s (*)(void *", t, mabi_poly ? "" : "sp_box_int(", aty);
      for (int k = 0; k < argc; k++) buf_printf(b, ", %s", aty);
      buf_printf(b, "))(uintptr_t)((sp_BoundMethod *)_t%d.v.p)->fn)((void *)((sp_BoundMethod *)_t%d.v.p)->self", t, t);
      for (int k = 0; k < argc; k++) {
        buf_puts(b, ", ");
        if (mabi_poly) emit_boxed(c, argv[k], b);
        else EMIT_POLY_CALL_SLOT(k);
      }
      /* the Proc publishes its result in _sp_proc_poly_ret (universal return
         ABI); evaluate for effect and read the boxed sp_RbVal intact, matching
         the Method branch's now-boxed result so the ternary's two arms are a
         single type. */
      buf_printf(b, ")%s : ((void)sp_proc_call((sp_Proc *)_t%d.v.p, %d, (mrb_int[16]){", mabi_poly ? "" : ")", t, argc);
      for (int k = 0; k < argc; k++) {
        if (k) buf_puts(b, ", ");
        EMIT_POLY_CALL_SLOT(k);
      }
      if (argc == 0) buf_puts(b, "0");  /* C99: no empty initializer list */
      buf_puts(b, "}), _sp_proc_poly_ret))");
      #undef EMIT_POLY_CALL_SLOT
      free(aptmp);
      return;
    }
  }
  /* Klass.instance_method(:sym) -> the unbound object: the same
     sp_BoundMethod with a NULL self, so name/arity/owner ride the Method
     arms; #bind supplies the self (#2676). */
  if (sp_streq(name, "instance_method") && method_sym_arg(c, id) != NULL) {
    int umi = method_obj_target_mi(c, id);
    if (umi >= 0) {
      buf_puts(b, "sp_bound_method_new(NULL, (mrb_int)(uintptr_t)&");
      emit_method_cname(c, &c->scopes[umi], b);
      buf_puts(b, ", ");
      emit_str_literal(b, method_sym_arg(c, id));
      buf_puts(b, ")");
      return;
    }
  }
  /* An UNBOUND method is not callable: CRuby's NoMethodError (#2724). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]")) &&
      method_expr_is_unbound(c, recv)) {
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
    buf_printf(b, "); sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"undefined method '%s' for an instance of UnboundMethod\")[1])); %s; })",
               name, default_value(comp_ntype(c, id)));
    return;
  }
  /* UnboundMethod#bind(obj): the same target with obj as self. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 1 && sp_streq(name, "bind")) {
    int mn2 = method_recv_node(c, recv);
    int t2 = mn2 >= 0 ? method_obj_target_mi(c, mn2) : -1;
    if (t2 >= 0 && ty_is_object(comp_ntype(c, argv[0]))) {
      buf_puts(b, "sp_bound_method_new((void *)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), (mrb_int)(uintptr_t)&");
      emit_method_cname(c, &c->scopes[t2], b);
      buf_puts(b, ", ");
      emit_str_literal(b, method_sym_arg(c, mn2));
      buf_puts(b, ")");
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
        static const char *const OBJM[] = {
          "class", "clone", "dup", "display", "enum_for", "eql?", "equal?",
          "extend", "freeze", "frozen?", "hash", "inspect", "instance_of?",
          "instance_variable_get", "instance_variable_set", "instance_variables",
          "is_a?", "itself", "kind_of?", "method", "methods", "nil?",
          "object_id", "public_send", "respond_to?", "send", "__send__",
          "tap", "then", "to_s", "yield_self", "==", "!=", "!", "===", NULL };
        int nknown = 0, nba;
        if (ncls && builtin_method_arity(ncls, sym, &nba)) nknown = 1;
        for (int oi2 = 0; !nknown && OBJM[oi2]; oi2++)
          if (sp_streq(sym, OBJM[oi2])) nknown = 1;
        if (ncls && !nknown) {
          buf_printf(b, "(mrb_int)(sp_raise_cls(\"NameError\","
                        " sp_sprintf(\"undefined method '%s' for %%s\","
                        " \"an instance of %s\")), 0)",
                     sym, ncls);
        }
        else buf_puts(b, "(mrb_int)0");  /* builtin/Kernel method: no callable address */
      }
    }
    buf_puts(b, ", ");
    /* a synthesized __bam_N forwarder is an implementation detail: Method#name
       must report the original method, recovered from the wrapper's body call */
    const char *disp = sym;
    if (strncmp(sym, "__bam_", 6) == 0 && mi >= 0 && c->scopes[mi].body >= 0) {
      int wb = c->scopes[mi].body;
      int wn2 = 0; const int *wbb = nt_arr(nt, wb, "body", &wn2);
      if (wn2 == 1 && nt_type(nt, wbb[0]) && sp_streq(nt_type(nt, wbb[0]), "CallNode")) {
        const char *orig = nt_str(nt, wbb[0], "name");
        if (orig) disp = orig;
      }
    }
    emit_str_literal(b, disp);
    buf_puts(b, ")");
    return;
  }
  /* <method>.to_proc wraps the bound method in a trampoline Proc. When the
     target method is statically known, emit a per-site trampoline that calls
     it with its real C signature (a top-level method has no self parameter;
     an object-bound one is invoked through a typed fn cast) and publishes the
     result boxed in _sp_proc_poly_ret -- the universal first-class-proc
     return ABI a later `.call` reads back. Falls back to the generic runtime
     trampoline when the target is unresolved. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "to_proc")) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int target_recvless = (mn >= 0 && nt_ref(nt, mn, "receiver") < 0);
    if (target >= 0) {
      Scope *tm = &c->scopes[target];
      int np = tm->nparams;
      TyKind tret = (TyKind)tm->ret;
      int tid = ++g_proc_counter;
      /* a bound __bam wrapper carries param[0] in the Method's self slot:
         proc arg k maps to param[k + shift] */
      int shift = method_call_param_shift(c, mn, target);
      /* a float/poly parameter reads back from the boxed side-channel the
         force_poly call site publishes */
      int needs_slot = 0;
      for (int k = shift; k < np; k++) {
        LocalVar *pp = scope_local(tm, tm->pnames[k]);
        TyKind pt = pp ? pp->type : TY_INT;
        if (pt == TY_POLY || pt == TY_FLOAT) needs_slot = 1;
      }
      if (needs_slot && !g_needs_proc_poly_argslot) {
        g_needs_proc_poly_argslot = 1;
        buf_puts(&g_proc_protos, "extern SP_TLS sp_RbVal _sp_proc_poly_args[16];\n");
      }
      Buf *pb = &g_procs;
      buf_printf(pb, "static mrb_int _mtp_%d(void *cap, mrb_int argc, mrb_int *args) {\n", tid);
      buf_puts(pb, "  sp_BoundMethod *_m = (sp_BoundMethod *)cap; (void)_m; (void)argc; (void)args;\n");
      /* the call expression: sp_<name>(args...) for a top-level target, else
         the typed fn cast through _m->fn with _m->self first */
      Buf cb = {0};
      if (target_recvless) {
        emit_method_cname(c, tm, &cb);
        buf_puts(&cb, "(");
      }
      else {
        buf_puts(&cb, "((");
        if (is_scalar_ret(tret)) emit_ctype(c, tret, &cb);
        else buf_puts(&cb, "void");
        buf_puts(&cb, " (*)(void *");
        for (int k = shift; k < np; k++) {
          buf_puts(&cb, ", ");
          LocalVar *pp = scope_local(tm, tm->pnames[k]);
          emit_ctype(c, pp ? pp->type : TY_INT, &cb);
        }
        buf_puts(&cb, "))(uintptr_t)_m->fn)((void *)_m->self");
        if (np > shift) buf_puts(&cb, ", ");
      }
      for (int k = shift; k < np; k++) {
        if (k > shift) buf_puts(&cb, ", ");
        LocalVar *pp = scope_local(tm, tm->pnames[k]);
        TyKind pt = pp ? pp->type : TY_INT;
        if (pt == TY_POLY) buf_printf(&cb, "_sp_proc_poly_args[%d]", k - shift);
        else if (pt == TY_FLOAT) buf_printf(&cb, "sp_poly_to_f(_sp_proc_poly_args[%d])", k - shift);
        else if (pt == TY_SYMBOL) buf_printf(&cb, "(sp_sym)args[%d]", k - shift);
        else if (proc_slot_is_ptr(pt)) {
          buf_puts(&cb, "(");
          emit_ctype(c, pt, &cb);
          buf_printf(&cb, ")(uintptr_t)args[%d]", k - shift);
        }
        else buf_printf(&cb, "args[%d]", k - shift);
      }
      buf_puts(&cb, ")");
      if (is_scalar_ret(tret)) {
        buf_puts(pb, "  ");
        emit_ctype(c, tret, pb);
        buf_printf(pb, " _r = %s;\n", cb.p ? cb.p : "");
        buf_puts(pb, "  _sp_proc_poly_ret = ");
        emit_boxed_text(c, tret, "_r", pb);
        buf_puts(pb, ";\n");
      }
      else {
        buf_printf(pb, "  %s;\n", cb.p ? cb.p : "");
        buf_puts(pb, "  _sp_proc_poly_ret = sp_box_nil();\n");
      }
      buf_puts(pb, "  return 0;\n}\n");
      free(cb.p);
      /* arity: required count (minus the self-carried wrapper param),
         negative when the signature is variadic (the Scope folds
         optionals/rest into nparams > nrequired). */
      int m_arity = (np != tm->nrequired) ? -(tm->nrequired - shift + 1)
                                          : tm->nrequired - shift;
      buf_printf(b, "sp_proc_new_meta((void *)_mtp_%d, (void *)(", tid);
      emit_expr(c, recv, b);
      buf_printf(b, "), sp_bm_cap_scan, %d, TRUE, 0, NULL, NULL)", m_arity);
      return;
    }
    buf_puts(b, "sp_method_to_proc(");
    emit_expr(c, recv, b);
    buf_puts(b, ")");
    return;
  }
  /* <method>.name -> the stored method name, interned to a Symbol (CRuby
     Method#name returns a Symbol, not a String). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 && sp_streq(name, "name")) {
    buf_puts(b, "sp_sym_intern((const char *)("); emit_expr(c, recv, b); buf_puts(b, ")->name)");
    return;
  }
  /* Method#owner / #receiver (#2701). The creation site knows both: a user
     target's owner is its defining class; a builtin target's owner is the
     receiver's class, and the receiver re-emits when doing so cannot repeat a
     side effect (a literal or a local read). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_METHOD && argc == 0 &&
      (sp_streq(name, "owner") || sp_streq(name, "receiver"))) {
    int mn = method_recv_node(c, recv);
    int target = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    int is_bam = target >= 0 && c->scopes[target].name &&
                 strncmp(c->scopes[target].name, "__bam_", 6) == 0;
    int mrecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
    if (sp_streq(name, "owner")) {
      if (target >= 0 && !is_bam) {
        int ocid = c->scopes[target].class_id;
        const char *ocn = ocid >= 0 ? (class_ruby_name(c, ocid) ? class_ruby_name(c, ocid)
                                                                : c->classes[ocid].name) : "Object";
        buf_printf(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(mrb_int)-1, \"%s\"}))", ocn);
        return;
      }
      if (mrecv >= 0) {
        TyKind mrt = comp_ntype(c, mrecv);
        const char *mcls = mrt == TY_STRING ? "String" : mrt == TY_INT ? "Integer"
                         : mrt == TY_FLOAT ? "Float" : mrt == TY_SYMBOL ? "Symbol"
                         : ty_is_array(mrt) ? "Array" : ty_is_hash(mrt) ? "Hash"
                         : mrt == TY_RANGE ? "Range" : mrt == TY_TIME ? "Time" : NULL;
        if (mcls) {
          buf_printf(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), ((sp_Class){(mrb_int)-1, \"%s\"}))", mcls);
          return;
        }
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
        return;
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
    if ((target < 0 || is_bam) && mn >= 0) {
      const char *msym = method_sym_arg(c, mn);
      if (is_bam) {
        int wb = c->scopes[target].body;
        int wn2 = 0; const int *wbb = wb >= 0 ? nt_arr(nt, wb, "body", &wn2) : NULL;
        int tail = wn2 > 0 ? wbb[wn2 - 1] : -1;
        msym = (tail >= 0 && nt_type(nt, tail) && sp_streq(nt_type(nt, tail), "CallNode"))
               ? nt_str(nt, tail, "name") : NULL;
      }
      int mrecv = nt_ref(nt, mn, "receiver");
      TyKind mrt = mrecv >= 0 ? comp_ntype(c, mrecv) : TY_UNKNOWN;
      const char *mcls = mrt == TY_STRING ? "String" : mrt == TY_INT ? "Integer"
                       : mrt == TY_FLOAT ? "Float" : mrt == TY_SYMBOL ? "Symbol"
                       : ty_is_array(mrt) ? "Array" : ty_is_hash(mrt) ? "Hash"
                       : mrt == TY_RANGE ? "Range" : mrt == TY_TIME ? "Time" : NULL;
      int ba;
      if (msym && mcls && builtin_method_arity(mcls, msym, &ba)) {
        /* evaluate for effect: the method()'s own receiver when the chain is
           inline (creating the bound method emits a non-void-castable
           expression), or the local read when the method object is var-held */
        buf_printf(b, "((void)(");
        if (mn == recv) emit_expr(c, mrecv, b); else emit_expr(c, recv, b);
        buf_printf(b, "), (mrb_int)%d)", ba);
        return;
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
      buf_printf(&g_proc_protos, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args);\n", pid);
      Buf *pb = &g_procs;
      buf_printf(pb, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args) {\n", pid);
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
        return;
      }
      free(callb.p);
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
    /* a bound __bam wrapper carries param[0] in the Method's self slot:
       call arg k is coerced to param[k + shift] */
    int shift = target >= 0 ? method_call_param_shift(c, mn, target) : 0;
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
      if (tm && k + shift < tm->nparams) {
        LocalVar *pp = scope_local(tm, tm->pnames[k + shift]);
        emit_ctype(c, pp ? pp->type : TY_INT, b);
      }
      else if (poly_abi) buf_puts(b, "sp_RbVal");
      else buf_puts(b, "mrb_int");
    }
    buf_printf(b, "))(uintptr_t)_t%d->fn)((void *)_t%d->self", tr, tr);
    for (int k = 0; k < argc; k++) {
      buf_puts(b, ", ");
      if (tm && k + shift < tm->nparams) emit_arg_or_default(c, tm, k + shift, argv[k], b);
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
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]") ||
       (sp_streq(name, "===") && argc == 1))) {
    TyKind rty = comp_ntype(c, id);          /* the call's result = proc's body return */
    /* `.call { |x| ... }`: the literal block rides the _sp_proc_blk
       side-channel to the callee's &block param (#2648) */
    {
      int cblk = nt_ref(nt, id, "block");
      if (cblk >= 0 && nt_type(nt, cblk) && sp_streq(nt_type(nt, cblk), "BlockNode")) {
        int tb = ++g_tmp;
        /* render the proc value first: a capturing block's emission writes its
           own prelude (capture struct fill) to g_pre, which must land as whole
           statements before this line */
        Buf pv; memset(&pv, 0, sizeof pv);
        emit_proc_literal(c, cblk, &pv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); _sp_proc_blk = _t%d;%c",
                   tb, pv.p ? pv.p : "NULL", tb, tb, 10);
        free(pv.p);
      }
    }
    /* <proc>.call with any splat among the arguments: materialize the full
       argument list (fixed args pushed, each splat expanded) and spread it at
       call time -- the length is dynamic, unlike the fixed mrb_int[16] list.
       #2691, #2729 */
    {
      int any_splat = 0;
      for (int k = 0; k < argc; k++)
        if (nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "SplatNode")) any_splat = 1;
      if (any_splat) {
        g_needs_proc_poly_argslot = 1;
        int ta = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);%c", ta, ta, 10);
        for (int k = 0; k < argc; k++) {
          Buf ab; memset(&ab, 0, sizeof ab);
          const char *aty = nt_type(nt, argv[k]);
          if (aty && sp_streq(aty, "SplatNode")) {
            int sx = nt_ref(nt, argv[k], "expression");
            if (sx >= 0) emit_boxed(c, sx, &ab);
            int ts = ++g_tmp, ti = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "{ sp_PolyArray *_t%d = sp_enum_items_from(%s); SP_GC_ROOT(_t%d);"
                              " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                              " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); }%c",
                       ts, ab.p ? ab.p : "sp_box_nil()", ts, ti, ti, ts, ti, ta, ts, ti, 10);
          }
          else {
            emit_boxed(c, argv[k], &ab);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);%c", ta, ab.p ? ab.p : "sp_box_nil()", 10);
          }
          free(ab.p);
        }
        buf_puts(b, "((void)sp_proc_call_spread(");
        emit_expr(c, recv, b);
        buf_printf(b, ", sp_box_poly_array(_t%d)), ", ta);
        emit_proc_ret_unbox(c, rty, b);
        buf_puts(b, ")");
        return;
      }
    }
    /* Universal boxed return: the proc publishes its result in _sp_proc_poly_ret
       (see emit_proc_literal); evaluate the call for effect, then unbox the slot
       to the call's inferred type. */
    buf_puts(b, "((void)sp_proc_call(");
    emit_expr(c, recv, b);
    buf_puts(b, ", ");
    emit_proc_call_args(c, argc, argv, b, 1);  /* emits args + the closing `)` */
    buf_puts(b, ", ");
    emit_proc_ret_unbox(c, rty, b);
    buf_puts(b, ")");
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
      return;
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
    return;
    }
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 &&
      (sp_streq(name, "inspect") || sp_streq(name, "to_s"))) {
    buf_puts(b, "sp_proc_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
  }
  /* Proc identity/state predicates: equal?/eql?/== compare by pointer, frozen?
     is false, freeze/dup/clone/itself evaluate to the proc itself. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) &&
      comp_ntype(c, argv[0]) == TY_PROC) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
    emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "frozen?")) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen)"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 && sp_streq(name, "freeze")) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_Proc *_t%d = ", t); emit_expr(c, recv, b);
    buf_printf(b, "; _t%d->frozen = TRUE; _t%d; })", t, t); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_PROC && argc == 0 &&
      (sp_streq(name, "dup") || sp_streq(name, "clone") || sp_streq(name, "itself"))) {
    emit_expr(c, recv, b); return;
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
      (sp_streq(name, "each") || sp_streq(name, "reverse_each") ||
       sp_streq(name, "map") || sp_streq(name, "collect") ||
       sp_streq(name, "each_entry"))) {
    /* a blockless map is the same element snapshot; only its #inspect method
       name differs (the deferred mapping is supplied by a later block form,
       which the chain emitters match before this arm) */
    if (sp_streq(name, "map") || sp_streq(name, "collect")) {
      int te = ++g_tmp;
      buf_printf(b, "({ sp_Enumerator *_t%d = sp_Enumerator_new_from(", te);
      emit_boxed(c, recv, b);
      buf_printf(b, "); _t%d->meth = \"map\"; _t%d; })", te, te);
      return;
    }
    buf_printf(b, "sp_Enumerator_new_from%s(", sp_streq(name, "reverse_each") ? "_rev" : "");
    emit_boxed(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* arr.each_with_index / arr.each_index with no block -> an external
     Enumerator: each_with_index yields [element, index] pairs, each_index
     yields the indices. A chained/terminal use (each_with_index.map/.to_a) is
     matched earlier by emit_each_with_index_terminal and never reaches here. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (ty_is_array(comp_ntype(c, recv)) || comp_ntype(c, recv) == TY_ENUMERATOR) &&
      (sp_streq(name, "each_with_index") || sp_streq(name, "each_index"))) {
    /* An Enumerator receiver (`arr.each.each_with_index`) is materialized to its
       element array first (#2487). */
    int is_enum = comp_ntype(c, recv) == TY_ENUMERATOR;
    if (sp_streq(name, "each_with_index")) {
      buf_puts(b, "sp_Enumerator_new_ewi(");
      if (is_enum) { buf_puts(b, "sp_box_poly_array(sp_Enumerator_to_a("); emit_expr(c, recv, b); buf_puts(b, "))"); }
      else emit_boxed(c, recv, b);
      buf_puts(b, ", 0)");
    }
    else {
      buf_puts(b, "sp_Enumerator_new_indices(");
      if (is_enum) { buf_puts(b, "sp_box_poly_array(sp_Enumerator_to_a("); emit_expr(c, recv, b); buf_puts(b, "))"); }
      else emit_boxed(c, recv, b);
      buf_puts(b, ")");
    }
    return;
  }
  /* str.each_char / each_line with no block -> an Enumerator over the string's
     characters / lines. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_STRING && argc == 0 &&
      nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "each_char") || sp_streq(name, "each_line") ||
       sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint"))) {
    /* spill the receiver once: it feeds both the snapshot and the
       inspect-visible source stamp */
    int tsrc = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsrc);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); ", tsrc);
    if (sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) {
      const char *fn = sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
      buf_printf(b, "sp_enum_with_src(sp_Enumerator_new_from(sp_box_int_array(%s(_t%d))), "
                    "sp_box_str(_t%d), \"%s\"); })", fn, tsrc, tsrc, name);
      return;
    }
    const char *itemfn = sp_streq(name, "each_char") ? "sp_str_chars_poly" : "sp_str_lines_poly";
    buf_printf(b, "sp_enum_with_src(sp_Enumerator_new_from_items(%s(_t%d)), "
                  "sp_box_str(_t%d), \"%s\"); })", itemfn, tsrc, tsrc, name);
    return;
  }
  /* str.each_line(sep) with no block -> an Enumerator over the sep-kept
     segments. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_STRING && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && sp_streq(name, "each_line") &&
      comp_ntype(c, argv[0]) == TY_STRING) {
    int tsrc3 = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsrc3);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); "
                  "sp_enum_with_src(sp_Enumerator_new_from(sp_box_str_array(sp_str_lines_sep(_t%d, ",
               tsrc3, tsrc3);
    emit_expr(c, argv[0], b);
    buf_printf(b, "))), sp_box_str(_t%d), \"each_line\"); })", tsrc3);
    return;
  }
  /* str.each_line(chomp: ...) with no block -> an Enumerator over the
     (possibly chomped) lines. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_STRING && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && sp_streq(name, "each_line") &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
    int is_chomp = (chomp_v >= 0 && nt_type(nt, chomp_v) &&
                    sp_streq(nt_type(nt, chomp_v), "TrueNode"));
    int tsrc2 = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsrc2);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); "
                  "sp_enum_with_src(sp_Enumerator_new_from(sp_box_str_array(%s(_t%d))), "
                  "sp_box_str(_t%d), \"each_line(chomp: %s)\"); })",
               tsrc2, is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", tsrc2, tsrc2,
               is_chomp ? "true" : "false");
    return;
  }
  /* arr.each_slice(n) / arr.each_cons(n) with no block -> a materialized
     Enumerator over the slices (non-overlapping, last may be short) or the
     sliding windows of length n. */
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (ty_is_array(comp_ntype(c, recv)) ||
       comp_ntype(c, recv) == TY_RANGE ||
       (comp_ntype(c, recv) == TY_UNKNOWN && nt_type(nt, recv) &&
        sp_streq(nt_type(nt, recv), "ArrayNode"))) &&
      (sp_streq(name, "each_slice") || sp_streq(name, "each_cons"))) {
    buf_printf(b, "sp_Enumerator_new_%s(", sp_streq(name, "each_slice") ? "slices" : "cons");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* arr.cycle(n) with no block -> a materialized Enumerator of the elements
     repeated n times (the unbounded blockless form stays a loud reject). */
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      ty_is_array(comp_ntype(c, recv)) && sp_streq(name, "cycle")) {
    buf_puts(b, "sp_Enumerator_new_cycle(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }
  /* arr.slice_before(pat) / slice_after(pat) with no block -> a materialized
     Enumerator over the groups. */
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      ty_is_array(comp_ntype(c, recv)) &&
      (sp_streq(name, "slice_before") || sp_streq(name, "slice_after"))) {
    /* CRuby's pattern form matches with `pattern === element`: the boxed
       pattern dispatches through sp_poly_case_eq (Range cover / Class is_a /
       Regexp match / value equality, #2847). A Proc pattern would need a
       stored-proc call per element and stays a loud reject. */
    TyKind spat = comp_ntype(c, argv[0]);
    if (spat == TY_PROC)
      unsupported(c, id, "slice_before/slice_after with a Proc pattern; use the block form");
    buf_printf(b, "sp_Enumerator_new_from_items(sp_poly_slice_groups(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b);
    buf_printf(b, ", %d))", sp_streq(name, "slice_after") ? 1 : 0);
    return;
  }
  /* hash.each / hash.each_pair with no block -> an external Enumerator over the
     hash's [key, value] pairs (sp_enum_items_from builds them). The direct-block
     form has block >= 0 and is excluded; a block-driving consumer of this
     enumerator (e.g. `.map { }`) is a separate, later feature. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      ty_is_hash(comp_ntype(c, recv)) &&
      (sp_streq(name, "each") || sp_streq(name, "each_pair") ||
       /* blockless Enumerable methods yield an external Enumerator over the
          [key, value] pairs (the block-consuming chain is a later feature) */
       sp_streq(name, "map") || sp_streq(name, "collect") ||
       sp_streq(name, "select") || sp_streq(name, "filter") ||
       sp_streq(name, "reject") || sp_streq(name, "find") ||
       sp_streq(name, "detect") || sp_streq(name, "find_all") ||
       sp_streq(name, "flat_map") || sp_streq(name, "filter_map") ||
       sp_streq(name, "sort_by") || sp_streq(name, "min_by") ||
       sp_streq(name, "max_by") || sp_streq(name, "group_by") ||
       sp_streq(name, "partition"))) {
    buf_puts(b, "sp_Enumerator_new_from(");
    emit_boxed(c, recv, b); buf_puts(b, ")");
    return;
  }
  /* hash.each_value / hash.each_key with no block -> an external Enumerator
     over the values / keys (so .to_a/.map chains compose). */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      ty_is_hash(comp_ntype(c, recv)) &&
      (sp_streq(name, "each_value") || sp_streq(name, "each_key"))) {
    buf_printf(b, "sp_Enumerator_new_from_items(sp_enum_hash_side(");
    emit_boxed(c, recv, b);
    buf_printf(b, ", %d))", sp_streq(name, "each_key") ? 1 : 0);
    return;
  }
  /* <enumerator>.with_index(off) with no block -> a materialized Enumerator over
     the [element, off + i] pairs. The block and terminal chain forms
     (each.with_index { }, each.with_index.map { }) are matched earlier. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_ENUMERATOR && argc <= 1 &&
      nt_ref(nt, id, "block") < 0 && sp_streq(name, "with_index")) {
    buf_puts(b, "sp_Enumerator_with_index(");
    emit_expr(c, recv, b); buf_puts(b, ", ");
    if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "0");
    buf_puts(b, ")");
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
    /* #next_values / #peek_values return the yielded value(s) as an array (#2482). */
    if (sp_streq(name, "next_values") && argc == 0) {
      buf_puts(b, "sp_Enumerator_next_values("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "peek_values") && argc == 0) {
      buf_puts(b, "sp_Enumerator_peek_values("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    /* Enumerator#+ chains two enumerators (#2481): the concatenation of their
       element sequences, materialized. */
    if (sp_streq(name, "+") && argc == 1 && comp_ntype(c, argv[0]) == TY_ENUMERATOR) {
      buf_puts(b, "sp_Enumerator_new_from(sp_box_poly_array(sp_PolyArray_concat(sp_Enumerator_to_a(");
      emit_expr(c, recv, b); buf_puts(b, "), sp_Enumerator_to_a(");
      emit_expr(c, argv[0], b); buf_puts(b, "))))");
      return;
    }
    if (sp_streq(name, "rewind") && argc == 0) {
      buf_puts(b, "sp_Enumerator_rewind("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "feed") && argc == 1) {
      buf_puts(b, "sp_Enumerator_feed("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "size") && argc == 0) {
      buf_puts(b, "sp_Enumerator_size("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
      buf_puts(b, "sp_enum_inspect("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) {
      buf_puts(b, "sp_Enumerator_take("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) {
      buf_puts(b, "sp_Enumerator_to_a("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
    if (argc == 1 && (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) &&
        comp_ntype(c, argv[0]) == TY_ENUMERATOR) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
      emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
    }
    if (argc == 0 && sp_streq(name, "frozen?")) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ")->frozen)"); return; }
    if (argc == 0 && sp_streq(name, "freeze")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Enumerator *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d->frozen = TRUE; _t%d; })", t, t); return;
    }
    if (argc == 0 && sp_streq(name, "itself")) { emit_expr(c, recv, b); return; }
    if (argc == 0 && (sp_streq(name, "dup") || sp_streq(name, "clone"))) {
      buf_puts(b, "sp_Enumerator_dup("); emit_expr(c, recv, b); buf_puts(b, ")"); return;
    }
  }

  /* Random class methods: Random.rand(n) / Random.rand / Random.bytes(n)
     share a lazily-seeded default instance. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Random")) {
    if (sp_streq(name, "rand")) {
      if (argc >= 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "sp_Random_rand_float_bound(sp_random_default_get(), ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (argc >= 1) {
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
    if (sp_streq(name, "new_seed") && argc == 0) {   /* #2523 */
      buf_puts(b, "sp_Random_new_seed()");
      return;
    }
    if (sp_streq(name, "urandom") && argc == 1) {     /* #2543 */
      buf_puts(b, "sp_Random_urandom("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "srand")) {                    /* #2525 (returns previous seed) */
      if (argc == 0) { buf_puts(b, "sp_kernel_srand((mrb_int)time(NULL))"); return; }
      buf_puts(b, "sp_kernel_srand("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
  }

  /* Enumerator.product(a, b[, c]) -> an Enumerator over the cartesian product. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Enumerator") &&
      sp_streq(name, "product") && (argc == 2 || argc == 3)) {
    buf_printf(b, "sp_Enumerator_product%d(", argc);
    for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); emit_boxed(c, argv[i], b); }
    buf_puts(b, ")");
    return;
  }

  /* Random instance methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_RANDOM) {
    if (sp_streq(name, "rand")) {
      if (argc >= 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "sp_Random_rand_float_bound("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (argc >= 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) {
        /* Random#rand(1.0..2.0) -> a Float in [first, last), exact endpoints. */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, argv[0], b);
        buf_puts(b, "; sp_Random_rand_float_range("); emit_expr(c, recv, b);
        buf_printf(b, ", _t%d.first, _t%d.last); })", tr, tr);
      }
      else if (argc >= 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* a Float-endpoint range yields a Float (#2521); an int range an Int. */
        const char *atype = nt_type(nt, argv[0]);
        int islit = atype && sp_streq(atype, "RangeNode");
        int lo = islit ? nt_ref(nt, argv[0], "left") : -1;
        int hi = islit ? nt_ref(nt, argv[0], "right") : -1;
        /* an endless/beginless range has no finite span -> Errno::EDOM (#2550) */
        if (islit && (lo < 0 || hi < 0)) {
          buf_puts(b, "(sp_raise_cls(\"Errno::EDOM\", \"Domain error - rand\"), (mrb_int)0)");
          return;
        }
        int is_float = lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
        if (is_float) {
          int tr = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_Random_rand_float_range(", tr);
          emit_expr(c, recv, b);
          buf_printf(b, ", (mrb_float)_t%d.first, (mrb_float)_t%d.last); })", tr, tr);
        } else {
          buf_puts(b, "sp_Random_rand_range("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
      }
      else if (argc >= 1) {
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
    if (sp_streq(name, "seed") && argc == 0) {   /* #2522 */
      buf_puts(b, "sp_Random_seed("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
      buf_puts(b, "sp_Random_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    /* a Random instance is an opaque object: #class, and identity #==/#equal? (#2524) */
    if (sp_streq(name, "class") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ((sp_Class){0, SPL(\"Random\")}))");
      return;
    }
    /* equal? is identity; == / eql? compare by internal PRNG state (#2524). */
    if (sp_streq(name, "equal?") && argc == 1) {
      buf_puts(b, "((void *)("); emit_expr(c, recv, b); buf_puts(b, ") == (void *)(");
      if (comp_ntype(c, argv[0]) == TY_RANDOM) emit_expr(c, argv[0], b);
      else buf_puts(b, "0");
      buf_puts(b, "))");
      return;
    }
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_RANDOM) {
        buf_puts(b, "sp_Random_eq("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      } else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), FALSE)");
      }
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
  /* Dir handle instance methods (#2821) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_DIR) {
    Buf drb = {0};
    emit_expr(c, recv, &drb);
    const char *dr = drb.p ? drb.p : "NULL";
    if (sp_streq(name, "read") && argc == 0) { buf_printf(b, "sp_Dir_read(%s)", dr); free(drb.p); return; }
    if ((sp_streq(name, "path") || sp_streq(name, "to_path")) && argc == 0) {
      buf_printf(b, "sp_Dir_path(%s)", dr); free(drb.p); return;
    }
    if (sp_streq(name, "close") && argc == 0) { buf_printf(b, "sp_Dir_close(%s)", dr); free(drb.p); return; }
    if (sp_streq(name, "rewind") && argc == 0) { buf_printf(b, "sp_Dir_rewind(%s)", dr); free(drb.p); return; }
    if ((sp_streq(name, "tell") || sp_streq(name, "pos")) && argc == 0) {
      buf_printf(b, "sp_Dir_tell(%s)", dr); free(drb.p); return;
    }
    if ((sp_streq(name, "children") || sp_streq(name, "entries")) && argc == 0) {
      buf_printf(b, "sp_dir_entries_impl(sp_Dir_path(%s), %d)", dr,
                 sp_streq(name, "children") ? 1 : 0);
      free(drb.p); return;
    }
    if ((sp_streq(name, "each") || sp_streq(name, "each_child")) &&
        nt_ref(nt, id, "block") >= 0) {
      int dblk2 = nt_ref(nt, id, "block");
      const char *dbp = block_param_name(c, dblk2, 0);
      const char *dbpn = dbp ? rename_local(dbp) : NULL;
      int dbdy = nt_ref(nt, dblk2, "body");
      int dbbn = 0; const int *dbbb = dbdy >= 0 ? nt_arr(nt, dbdy, "body", &dbbn) : NULL;
      int tdh = ++g_tmp, tdn = ++g_tmp;
      int skip_dots = sp_streq(name, "each_child");
      buf_puts(b, "({ ");
      buf_printf(b, "sp_Dir *_t%d = %s; const char *_t%d; ", tdh, dr, tdn);
      buf_printf(b, "while ((_t%d = sp_Dir_read(_t%d)) != NULL) {", tdn, tdh);
      if (skip_dots)
        buf_printf(b, " if (sp_str_eq(_t%d, (&(\"\\xff\" \".\")[1])) ||"
                      " sp_str_eq(_t%d, (&(\"\\xff\" \"..\")[1]))) continue;", tdn, tdn);
      if (dbpn) buf_printf(b, " const char *lv_%s = _t%d;", dbpn, tdn);
      for (int k = 0; k < dbbn; k++) emit_stmt(c, dbbb[k], b, 0);
      buf_printf(b, " } (sp_Dir *)_t%d; })", tdh);
      return;
    }
    free(drb.p);
  }

  if (recv >= 0 && comp_ntype(c, recv) == TY_IO) {
    const char *r = NULL;
    Buf rb = {0};
    emit_expr(c, recv, &rb);
    r = rb.p ? rb.p : "NULL";
    /* metadata via the handle's path (#2790) */
    if (argc == 0 && (sp_streq(name, "size") || sp_streq(name, "mtime") ||
                      sp_streq(name, "atime") || sp_streq(name, "ctime") ||
                      sp_streq(name, "ftype"))) {
      buf_printf(b, "sp_file_%s(sp_File_path(%s))", sp_streq(name, "size") ? "size" : name, r);
      free(rb.p); return;
    }
    if (argc == 0 && sp_streq(name, "mode") && strstr(r, "sp_file_stat_handle")) {
      buf_printf(b, "sp_file_stat_mode(sp_File_path(%s))", r);
      free(rb.p); return;
    }
    if (argc == 1 && sp_streq(name, "chmod")) {
      /* the instance form returns 0, not the class form's file count */
      buf_puts(b, "({ sp_file_chmod("); emit_int_expr(c, argv[0], b);
      buf_printf(b, ", sp_File_path(%s)); (mrb_int)0; })", r);
      free(rb.p); return;
    }
    if (argc == 0 && sp_streq(name, "stat")) {
      buf_printf(b, "sp_file_stat_handle(sp_File_path(%s))", r);
      free(rb.p); return;
    }
    if (sp_streq(name, "read")) {
      if (argc == 0) buf_printf(b, "sp_File_read(%s)", r);
      else if (argc >= 2 && nt_type(nt, argv[1]) &&
               sp_streq(nt_type(nt, argv[1]), "LocalVariableReadNode")) {
        /* read(len, buffer): rebind the buffer local to the bytes read (#2811) */
        const char *bnm = nt_str(nt, argv[1], "name");
        int trd = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = sp_File_read_n(%s, ", trd, r);
        emit_int_expr(c, argv[0], b);
        buf_printf(b, "); lv_%s = _t%d; _t%d; })", bnm ? rename_local(bnm) : "?", trd, trd);
      }
      else { buf_puts(b, "sp_File_read_n("); buf_puts(b, r); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      free(rb.p); return;
    }
    if (sp_streq(name, "gets") || sp_streq(name, "readline")) {
      /* separator / limit / chomp: forms (#2809); readline raises EOFError
         at end of file (#2817) */
      int gsep = -1, glim = -1, gchomp = 0;
      for (int k = 0; k < argc; k++) {
        const char *kty = nt_type(nt, argv[k]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[k], "chomp");
          if (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "TrueNode")) gchomp = 1;
        }
        else if (comp_ntype(c, argv[k]) == TY_INT) glim = argv[k];
        else gsep = argv[k];
      }
      int is_rdl = sp_streq(name, "readline");
      if (argc == 0 && !is_rdl) buf_printf(b, "sp_File_gets(%s)", r);
      else {
        buf_printf(b, "sp_File_%s(%s, ", is_rdl ? "readline_sep" : "gets_sep", r);
        if (gsep >= 0) emit_expr(c, gsep, b); else buf_puts(b, "\"\\n\"");
        buf_puts(b, ", ");
        if (glim >= 0) emit_int_expr(c, glim, b); else buf_puts(b, "0");
        buf_printf(b, ", %d)", gchomp);
      }
      free(rb.p); return;
    }
    if (sp_streq(name, "getc") && argc == 0) {
      buf_printf(b, "sp_File_getc(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "readchar") && argc == 0) {
      buf_printf(b, "sp_File_readchar(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "getbyte") && argc == 0) {
      buf_printf(b, "sp_File_getbyte(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "ungetc") && argc == 1) {
      buf_printf(b, "sp_File_ungetc(%s, ", r); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      free(rb.p); return;
    }
    if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc >= 1) {
      buf_printf(b, "sp_File_readpartial(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(rb.p); return;
    }
    if (sp_streq(name, "sysseek") && argc >= 1) {
      buf_printf(b, "sp_File_sysseek(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc >= 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
      buf_puts(b, ")");
      free(rb.p); return;
    }
    if (sp_streq(name, "flock") && argc == 1) {
      buf_printf(b, "sp_File_flock(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(rb.p); return;
    }
    if ((sp_streq(name, "fsync") || sp_streq(name, "fdatasync")) && argc == 0) {
      buf_printf(b, "sp_File_fsync(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "autoclose?") && argc == 0) {
      buf_printf(b, "({ (void)%s; (mrb_bool)1; })", r); free(rb.p); return;
    }
    if (sp_streq(name, "pid") && argc == 0) {
      buf_printf(b, "({ (void)%s; sp_box_nil(); })", r); free(rb.p); return;
    }
    if (sp_streq(name, "to_i") && argc == 0) {
      buf_printf(b, "sp_File_fileno(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "lineno") && argc == 0) {
      buf_printf(b, "((%s)->lineno)", r); free(rb.p); return;
    }
    if (sp_streq(name, "lineno=") && argc == 1) {
      buf_printf(b, "((%s)->lineno = ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(rb.p); return;
    }
    if (sp_streq(name, "pos=") && argc == 1) {
      /* reposition; the assignment expression's value is the offset (#2798) */
      int tp2 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", tp2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_File_seek(%s, _t%d, 0); _t%d; })", r, tp2, tp2);
      free(rb.p); return;
    }
    if (sp_streq(name, "putc") && argc == 1) {
      buf_printf(b, "sp_File_putc(%s, ", r); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      free(rb.p); return;
    }
    if (sp_streq(name, "printf") && argc >= 1) {
      /* format into a string, then write it (#2796) */
      int tfp = ++g_tmp, tfa = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "const char *_t%d = ", tfp);
      Buf ffb; memset(&ffb, 0, sizeof ffb);
      emit_expr(c, argv[0], &ffb);
      buf_printf(g_pre, "%s;\n", ffb.p ? ffb.p : "\"\"");
      free(ffb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", tfa);
      for (int ai = 1; ai < argc; ai++) {
        Buf fab; memset(&fab, 0, sizeof fab);
        emit_boxed(c, argv[ai], &fab);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tfa, fab.p ? fab.p : "sp_box_nil()");
        free(fab.p);
      }
      buf_printf(b, "({ sp_File_write(%s, sp_str_format_polyarr(_t%d, _t%d)); sp_box_nil(); })", r, tfp, tfa);
      free(rb.p); return;
    }
    /* each_char / each_byte (#2794) */
    if ((sp_streq(name, "each_char") || sp_streq(name, "each_byte")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk2 = nt_ref(nt, id, "block");
      const char *bp2 = block_param_name(c, blk2, 0);
      const char *bpn2 = bp2 ? rename_local(bp2) : NULL;
      int bdy2 = nt_ref(nt, blk2, "body");
      int bbn2 = 0; const int *bbb2 = bdy2 >= 0 ? nt_arr(nt, bdy2, "body", &bbn2) : NULL;
      int is_byte = sp_streq(name, "each_byte");
      int rf2 = ++g_tmp, lt2 = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = %s; ", rf2, r);
      if (is_byte)
        buf_printf(b, "mrb_int _t%d; while ((_t%d = sp_File_getbyte(_t%d)) != SP_INT_NIL) {", lt2, lt2, rf2);
      else
        buf_printf(b, "const char *_t%d; while ((_t%d = sp_File_getc(_t%d)) != NULL) {", lt2, lt2, rf2);
      if (bpn2) buf_printf(b, " %s lv_%s = _t%d;", is_byte ? "mrb_int" : "const char *", bpn2, lt2);
      for (int k = 0; k < bbn2; k++) emit_stmt(c, bbb2[k], b, 0);
      buf_printf(b, " } (sp_File *)_t%d; })", rf2);
      free(rb.p); return;
    }
    if (sp_streq(name, "readlines")) {
      int rsep = -1, rchomp = 0;
      for (int k = 0; k < argc; k++) {
        const char *kty = nt_type(nt, argv[k]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[k], "chomp");
          if (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "TrueNode")) rchomp = 1;
        }
        else rsep = argv[k];
      }
      if (rsep < 0 && !rchomp) buf_printf(b, "sp_File_readlines(%s)", r);
      else {
        buf_printf(b, "sp_File_readlines_sep(%s, ", r);
        if (rsep >= 0) emit_expr(c, rsep, b); else buf_puts(b, "\"\\n\"");
        buf_printf(b, ", %d)", rchomp);
      }
      free(rb.p); return;
    }
    if (sp_streq(name, "write") || sp_streq(name, "syswrite")) {
      /* every argument writes in order; the return is the total byte count (#2814) */
      if (argc == 1) {
        buf_printf(b, "sp_File_write(%s, ", r);
        if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
      }
      else if (argc >= 2) {
        int tw = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = 0;", tw);
        for (int k = 0; k < argc; k++) {
          buf_printf(b, " _t%d += sp_File_write(%s, ", tw, r);
          if (comp_ntype(c, argv[k]) == TY_STRING) emit_expr(c, argv[k], b);
          else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[k], b); buf_puts(b, ")"); }
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", tw);
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
         a char *), matching Kernel#puts/#print coercion.

         Render each arg into a local buffer first: a compound arg
         (format/sprintf/join, array/hash literal) pushes its temp decls to
         g_pre, which must land as whole statements before this block, not
         inside the half-built sp_File_write(...) call. Same class of bug as
         the format/sprintf codegen below (#1498 / #1508). */
      Buf *abs = (Buf *)calloc(argc > 0 ? (size_t)argc : 1, sizeof(Buf));
      int *aarr = (int *)calloc(argc > 0 ? (size_t)argc : 1, sizeof(int));
      for (int k = 0; k < argc; k++) {
        TyKind akt = comp_ntype(c, argv[k]);
        if (sp_streq(name, "puts") && (ty_is_array(akt) || akt == TY_POLY_ARRAY || akt == TY_POLY)) {
          /* an Array argument prints one element per line (#2813) */
          aarr[k] = 1;
          emit_boxed(c, argv[k], &abs[k]);
        }
        else if (akt == TY_STRING) emit_expr(c, argv[k], &abs[k]);
        else { buf_puts(&abs[k], "sp_poly_to_s("); emit_boxed(c, argv[k], &abs[k]); buf_puts(&abs[k], ")"); }
      }
      /* puts uses sp_File_puts, which appends a newline per argument (and only
         when the arg isn't already newline-terminated), matching CRuby's
         `puts a, b` -> "a\nb\n" and `puts "x\n"` -> "x\n". A nullable string
         arg can be NULL (nil); sp_File_puts is a no-op on NULL, so coalesce it
         to "" first so `puts nil` still prints the blank line. print writes the
         raw arg (a NULL write is correctly a no-op). Array flattening
         (puts [1,2] -> one line each) is not yet modelled here -- a non-string
         arg is stringified via sp_poly_to_s above. */
      int is_puts = sp_streq(name, "puts");
      /* An empty `print` (argc == 0, not puts) would emit an empty `({ })`,
         which is not a valid statement-expression; skip the block entirely. */
      if (argc > 0 || is_puts) {
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, "({ ");
        for (int k = 0; k < argc; k++) {
          const char *at = abs[k].p ? abs[k].p : "\"\"";
          if (is_puts && aarr[k])
            buf_printf(g_pre, "sp_File_puts_val(%s, %s); ", r, at);
          else if (is_puts) {
            int ts = ++g_tmp;
            buf_printf(g_pre, "sp_File_puts(%s, ({ const char *_t%d = %s; _t%d ? _t%d : \"\"; })); ",
                       r, ts, at, ts, ts);
          }
          else buf_printf(g_pre, "sp_File_write(%s, %s); ", r, at);
          free(abs[k].p);
        }
        if (is_puts && argc == 0) buf_printf(g_pre, "sp_File_write(%s, \"\\n\"); ", r);
        buf_puts(g_pre, "});\n");
      }
      else for (int k = 0; k < argc; k++) free(abs[k].p);
      free(abs);
      free(aarr);
      buf_puts(b, "((mrb_int)0)");
      free(rb.p); return;
    }
    if (sp_streq(name, "close")) {
      buf_printf(b, "({ sp_File_close(%s); sp_box_nil(); })", r); free(rb.p); return;
    }
    if (sp_streq(name, "closed?")) {
      buf_printf(b, "sp_File_closed_p(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) {
      buf_printf(b, "sp_File_eof_p(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "seek") && argc >= 1) {
      /* offset plus optional whence (IO::SEEK_SET/CUR/END -> 0/1/2; absolute
         when omitted, matching Ruby's SEEK_SET default) */
      buf_printf(b, "sp_File_seek(%s, ", r);
      emit_int_expr(c, argv[0], b);
      buf_puts(b, ", ");
      if (argc >= 2) emit_int_expr(c, argv[1], b);
      else buf_puts(b, "0");
      buf_puts(b, ")");
      free(rb.p); return;
    }
    if (sp_streq(name, "tell") || sp_streq(name, "pos")) {
      buf_printf(b, "sp_File_tell(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "rewind")) {
      buf_printf(b, "sp_File_rewind(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "path") || sp_streq(name, "to_path")) {
      buf_printf(b, "sp_File_path(%s)", r); free(rb.p); return;
    }
    if (sp_streq(name, "sync")) {
      /* CRuby's default: buffered (false); spinel does not model per-handle
         sync state, so report the default (#2792) */
      buf_printf(b, "({ (void)%s; (mrb_bool)0; })", r);
      free(rb.p); return;
    }
    if (sp_streq(name, "sync=") && argc >= 1) {
      int ts2 = ++g_tmp;
      buf_printf(b, "({ mrb_bool _t%d = (", ts2);
      if (comp_ntype(c, argv[0]) == TY_BOOL) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_poly_truthy("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "); if (_t%d) sp_File_flush(%s); _t%d; })", ts2, r, ts2);
      free(rb.p); return;
    }
    if (sp_streq(name, "flush") || sp_streq(name, "binmode")) {
      /* flush/binmode return self, so they chain (#2799) */
      int tfl = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = %s; ", tfl, r);
      if (sp_streq(name, "flush")) buf_printf(b, "sp_File_flush(_t%d); ", tfl);
      buf_printf(b, "_t%d; })", tfl);
      free(rb.p); return;
    }
    if ((sp_streq(name, "each_line") || sp_streq(name, "each")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, blk, 0);
      const char *bpn = bp ? rename_local(bp) : NULL;
      int bdy = nt_ref(nt, blk, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      /* an optional separator argument (#2810) forces the general reader */
      int esep = (argc >= 1 && comp_ntype(c, argv[0]) == TY_STRING) ? argv[0] : -1;
      int lt = ++g_tmp, rf = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_File *_t%d = %s; ", rf, r);
      free(rb.p); r = NULL;
      /* Each iteration yields a FRESH heap line string, matching CRuby --
         a stored reference must keep its own line, not a mutated shared
         buffer (#2803). */
      buf_printf(b, "const char *_t%d; while ((_t%d = sp_File_gets_sep(_t%d, ", lt, lt, rf);
      if (esep >= 0) emit_expr(c, esep, b); else buf_puts(b, "\"\\n\"");
      buf_puts(b, ", 0, 0)) != NULL) {");
      if (bpn) buf_printf(b, " const char *lv_%s = _t%d;", bpn, lt);
      for (int k = 0; k < bbn; k++) emit_stmt(c, bbb[k], b, 0);
      buf_printf(b, " } (sp_File *)_t%d; })", rf);
      return;
    }
    free(rb.p);
  }

  /* IO instance methods on a poly-carried handle (an IO.pipe element): unbox
     and dispatch, unless a user class defines the name (then the general poly
     dispatch owns it). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_POLY &&
      (sp_streq(name, "write") || sp_streq(name, "read") || sp_streq(name, "gets") ||
       sp_streq(name, "readline") || sp_streq(name, "close") || sp_streq(name, "flush") ||
       sp_streq(name, "fileno"))) {
    int iocand = 0;
    for (int k = 0; k < c->nclasses && !iocand; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0 ||
          comp_reader_in_chain(c, k, name, NULL) ||
          c->classes[k].is_native_class)   /* a native class's methods are not in scopes */
        iocand = 1;
    if (!iocand) {
      int tio2 = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = sp_poly_as_io(", tio2);
      emit_boxed(c, recv, b);
      buf_printf(b, ", \"%s\"); ", name);
      if (sp_streq(name, "write") && argc >= 1) {
        buf_printf(b, "sp_File_write(_t%d, ", tio2);
        if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "read")) buf_printf(b, "sp_File_read(_t%d); })", tio2);
      else if (sp_streq(name, "gets")) buf_printf(b, "sp_File_gets(_t%d); })", tio2);
      else if (sp_streq(name, "readline")) buf_printf(b, "sp_File_readline_sep(_t%d, \"\\n\", 0, 0); })", tio2);
      else if (sp_streq(name, "close")) buf_printf(b, "sp_File_close(_t%d); })", tio2);
      else if (sp_streq(name, "flush")) buf_printf(b, "sp_File_flush(_t%d); })", tio2);
      else buf_printf(b, "sp_File_fileno(_t%d); })", tio2);
      return;
    }
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
  /* poly_val >> int: unbox recv to int, apply op. & | ^ dispatch on the
     runtime tag instead (nil/bool are boolean ops, ints bitwise; #2401). */
  if (recv >= 0 && argc == 1 && comp_ntype(c, recv) == TY_POLY &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^"))) {
    int bop = sp_streq(name, "&") ? 0 : sp_streq(name, "|") ? 1 : 2;
    buf_puts(b, "sp_poly_bitop("); emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_printf(b, ", %d)", bop);
    return;
  }
  if (recv >= 0 && argc == 1 && comp_ntype(c, recv) == TY_POLY && sp_streq(name, ">>")) {
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
        return;
      }
      if (sp_streq(name, "freeze")) {
        buf_puts(b, g_self ? g_self : "self");
        return;
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

  /* Kernel conversions. These are private Kernel methods available on every
     object, so an explicit-receiver form (obj.send(:Float, x), desugared to
     obj.Float(x)) dispatches here too when the receiver is a plain user object
     whose chain does not define the name. Only a side-effect-free receiver
     (a local/ivar read or self) is accepted, since it is discarded. */
  int kconv = (recv < 0 && comp_method_index(c, name) < 0);
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
    if (sp_streq(name, "Integer") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_i_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "((mrb_int)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_raise_cls(\"TypeError\", \"can't convert nil into Integer\"), (mrb_int)0)"); }  /* #2514 */
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_Integer("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      return;
    }
    if (sp_streq(name, "Integer") && ac == 2) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) {
        buf_puts(b, "sp_str_to_i_strict_base("); emit_expr(c, av[0], b);
        buf_puts(b, ", "); emit_expr(c, av[1], b); buf_puts(b, ")");
      }
      /* a base with a non-String value raises ArgumentError, as CRuby (#2515) */
      else { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), (void)("); emit_expr(c, av[1], b); buf_puts(b, "), sp_raise_cls(\"ArgumentError\", \"base specified for non string value\"), (mrb_int)0)"); }
      return;
    }
    if (sp_streq(name, "Float") && ac == 1) {
      TyKind at = comp_ntype(c, av[0]);
      if (at == TY_STRING) { buf_puts(b, "sp_str_to_f_strict("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
      else if (at == TY_INT) { buf_puts(b, "((mrb_float)("); emit_expr(c, av[0], b); buf_puts(b, "))"); }
      else if (at == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, av[0], b); buf_puts(b, "), sp_raise_cls(\"TypeError\", \"can't convert nil into Float\"), 0.0)"); }  /* #2514 */
      else if (at == TY_POLY) { buf_puts(b, "sp_poly_Float("); emit_expr(c, av[0], b); buf_puts(b, ")"); }
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
      else if (at == TY_NIL || at == TY_UNKNOWN) { buf_puts(b, "sp_poly_to_s(sp_box_nil())"); }
      else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }  /* container/range/object: #to_s */
      return;
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
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); sp_%sArray_push(_t%d, ", ak, t, ak, t, ak, t);
        if (at == TY_INT) emit_int_expr(c, av[0], b);
        else if (at == TY_FLOAT) emit_float_expr(c, av[0], b);
        else emit_expr(c, av[0], b);
        buf_printf(b, "); _t%d; })", t);
      }
      else { buf_puts(b, "sp_kernel_array("); emit_boxed(c, av[0], b); buf_puts(b, ")"); }
      return;
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
      if (ty_is_hash(at)) { emit_expr(c, av[0], b); }
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
      return;
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && ac == 1 &&
        nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "SplatNode")) {
      /* format(*args): the array's head is the format string */
      int sfx = nt_ref(nt, av[0], "expression");
      if (sfx >= 0) {
        buf_puts(b, "sp_str_format_splat(");
        emit_boxed(c, sfx, b);
        buf_puts(b, ")");
        return;
      }
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
      if (ac == 0) { buf_puts(b, "sp_krand_float()"); return; }
      TyKind a0t = comp_ntype(c, av[0]);
      if (a0t == TY_FLOAT_RANGE) {   /* rand(1.0..10.0) -> a Float in [first, last) */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, av[0], b);
        buf_printf(b, "; _t%d.first + sp_Random_rand_float(sp_random_default_get()) * (_t%d.last - _t%d.first); })", tr, tr, tr);
        return;
      }
      if (a0t == TY_RANGE) {
        const char *atype = nt_type(nt, av[0]);
        int islit = atype && sp_streq(atype, "RangeNode");
        int lo = islit ? nt_ref(nt, av[0], "left") : -1;
        int hi = islit ? nt_ref(nt, av[0], "right") : -1;
        /* an endless/beginless range has no finite span -> Errno::EDOM (#2544) */
        if (islit && (lo < 0 || hi < 0)) {
          buf_puts(b, "(sp_raise_cls(\"Errno::EDOM\", \"Domain error - rand\"), (mrb_int)0)");
          return;
        }
        int is_float = lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
        /* a statically empty/reversed int range -> nil (#2519) */
        if (islit && !is_float && lo >= 0 && hi >= 0 &&
            nt_type(nt, lo) && sp_streq(nt_type(nt, lo), "IntegerNode") &&
            nt_type(nt, hi) && sp_streq(nt_type(nt, hi), "IntegerNode")) {
          long long lov = nt_int(nt, lo, "value", 0);
          long long hiv = nt_int(nt, hi, "value", 0);
          int excl = (nt_int(nt, av[0], "flags", 0) & 4) ? 1 : 0;
          if ((excl ? hiv - 1 : hiv) < lov) { buf_puts(b, "sp_box_nil()"); return; }
        }
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, av[0], b); buf_puts(b, "; ");
        if (is_float)
          buf_printf(b, "(mrb_float)_t%d.first + sp_Random_rand_float(sp_random_default_get()) * (mrb_float)(_t%d.last - _t%d.first); })", tr, tr, tr);
        else
          buf_printf(b, "_t%d.first + sp_Random_rand_int(sp_random_default_get(), _t%d.last - _t%d.first + 1 - _t%d.excl); })", tr, tr, tr, tr);
        return;
      }
      /* rand(int): 0 behaves like rand() (a Float in [0,1)); a nonzero magnitude
         gives an Integer in [0, |n|) (#2518). A literal folds to the exact form. */
      if (nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "IntegerNode")) {
        long long v = nt_int(nt, av[0], "value", 0);
        if (v == 0) { buf_puts(b, "sp_krand_float()"); return; }
        long long m = v < 0 ? -v : v;
        buf_printf(b, "sp_krand_below(%lldLL)", m);
        return;
      }
      /* a dynamic Integer argument may be 0 at run time (a Float [0,1)) or
         nonzero (an Integer [0,|n|)), so the result is boxed and chosen at
         run time (#2549). A Float bound truncates to an integer (rand(3.5)
         draws over [0,3)); cast explicitly so the narrowing is intentional,
         not a warning (#2868). */
      int tn = ++g_tmp;
      int a0_flt = comp_ntype(c, av[0]) == TY_FLOAT;
      buf_printf(b, "({ mrb_int _t%d = ", tn);
      if (a0_flt) buf_puts(b, "(mrb_int)(");
      emit_int_expr(c, av[0], b);
      if (a0_flt) buf_puts(b, ")");
      buf_printf(b, "; if (_t%d < 0) _t%d = -_t%d; _t%d > 0"
                    " ? sp_box_int(sp_krand_below(_t%d))"
                    " : sp_box_float(sp_krand_float()); })",
                 tn, tn, tn, tn, tn);
      return;
    }
    if (sp_streq(name, "srand")) {
      /* srand returns the PREVIOUS seed (#2517). */
      if (ac == 0) { buf_puts(b, "sp_kernel_srand((mrb_int)time(NULL))"); return; }
      buf_puts(b, "sp_kernel_srand("); emit_int_expr(c, av[0], b); buf_puts(b, ")");
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
    /* exit raises a rescuable SystemExit (#2761); exit! terminates directly.
       A boolean status maps true -> 0, false -> 1, as in CRuby. */
    const char *xfn = sp_streq(name, "exit!") ? "exit" : "sp_exit_raise";
    if (argc == 0) { buf_printf(b, "({ %s(0); (mrb_int)0; })", xfn); return; }
    /* a poly status (e.g. a widened attr read or poly-hash get) must be
       unboxed -- (int)(sp_RbVal) is a struct cast, a cc error. */
    TyKind xt = comp_ntype(c, argv[0]);
    if (xt == TY_POLY) { buf_printf(b, "({ %s((int)sp_poly_to_i(", xfn); emit_expr(c, argv[0], b); buf_puts(b, ")); (mrb_int)0; })"); }
    else if (xt == TY_BOOL) { buf_printf(b, "({ %s((", xfn); emit_expr(c, argv[0], b); buf_puts(b, ") ? 0 : 1); (mrb_int)0; })"); }
    else { buf_printf(b, "({ %s((int)(", xfn); emit_expr(c, argv[0], b); buf_puts(b, ")); (mrb_int)0; })"); }
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

  /* Kernel#puts / #print as an expression: run the statement emitters inside
     a statement-expression and yield nil (their Ruby value). */
  if (recv < 0 && (sp_streq(name, "puts") || sp_streq(name, "print")) &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "({ ");
    if (argc == 0 && sp_streq(name, "puts")) buf_puts(b, "putchar('\n');\n");
    for (int k = 0; k < argc; k++) {
      if (sp_streq(name, "puts")) emit_puts_one(c, argv[k], b, 0);
      else emit_print_one(c, argv[k], b, 0);
    }
    buf_puts(b, " sp_box_nil(); })");
    return;
  }

  /* Kernel#p as an expression: print the argument's inspect, yield the
     argument as the value (statement position has its own emitter). The
     value is boxed once, printed through the poly inspect (which consults
     the user-object hook), and unboxed back to the static type. */
  /* p(a, b, ...) as a value: prints each argument's inspect, returns the
     argument array. */
  if (recv < 0 && (sp_streq(name, "p") || sp_streq(name, "pp")) && argc >= 2 && nt_ref(nt, id, "block") < 0) {
    int t = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", t, t);
    for (int k = 0; k < argc; k++) {
      buf_printf(b, "sp_PolyArray_push(_t%d, ", t);
      emit_boxed(c, argv[k], b);
      buf_puts(b, "); ");
    }
    buf_printf(b, "for (mrb_int _i%d = 0; _i%d < _t%d->len; _i%d++) { "
                  "fputs(sp_poly_inspect(_t%d->data[_i%d]), stdout); putchar('\\n'); } _t%d; })",
               t, t, t, t, t, t, t);
    return;
  }
  if (recv < 0 && (sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
    TyKind at = comp_ntype(c, argv[0]);
    int t = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", t);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); fputs(sp_poly_inspect(_t%d), stdout); putchar('\\n'); ", t, t);
    char tv[16]; snprintf(tv, sizeof tv, "_t%d", t);
    emit_unbox_text(c, at, tv, b);
    buf_puts(b, "; })");
    return;
  }
  /* Kernel#warn / #printf / #putc / p() as an expression: run the statement
     emitter inside a statement-expression and yield the Ruby value (nil for
     warn/printf/p(), the argument for putc). */
  if (recv < 0 && (sp_streq(name, "warn") || sp_streq(name, "printf") ||
                   (sp_streq(name, "putc") && argc == 1) ||
                   ((sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 0)) &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "({ ");
    emit_output_call(c, id, b, 0);
    if (sp_streq(name, "putc") && argc == 1) emit_expr(c, argv[0], b);  /* putc returns its arg */
    else buf_puts(b, "sp_box_nil()");
    buf_puts(b, "; })");
    return;
  }

  /* raise */
  /* `fail` is an exact alias of `Kernel#raise`. */
  if (recv < 0 && (sp_streq(name, "raise") || sp_streq(name, "fail"))) {
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
      buf_puts(b, "(sp_explicit_cause = (void *)(");
      emit_expr(c, cause_node, b);
      buf_puts(b, "), ");
    }
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
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].c_name);
        emit_args_filled(c, ic, -1, "", b);
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
        buf_printf(b, "sp_raise_cls(\"%s\", (&(\"\\xff\")[1]))", rn ? rn : (cn ? RAISE_EXC_NAME(av[0], cn) : ""));
      }
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
        buf_printf(b, "sp_raise_exc((sp_Exception *)sp_%s_new(", c->classes[xc].c_name);
        /* `raise Cls, msg` only ever supplies the message, but the generated
           constructor keeps its full signature (defaulted positionals and
           keywords included) -- fill param 0 with the message and every
           remaining param from its default, or the call is emitted with too
           few arguments. emit_arg_or_default also boxes/coerces the message
           to the first param's type (poly when unknown, same rule
           emit_class_new uses for the signature). */
        Scope *im = &c->scopes[ic];
        emit_arg_or_default(c, im, 0, av[1], b);
        for (int pk = 1; pk < im->nparams; pk++) {
          buf_puts(b, ", ");
          emit_arg_or_default(c, im, pk, -1, b);
        }
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
        buf_printf(b, "sp_raise_cls(\"%s\", ", rn ? rn : RAISE_EXC_NAME(av[0], cn));
        if (comp_ntype(c, av[1]) == TY_STRING) emit_expr(c, av[1], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, av[1], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
      }
    }
    else {
      TyKind at = ac > 0 ? comp_ntype(c, av[0]) : TY_UNKNOWN;
      if (at == TY_EXCEPTION)
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
      else if (at == TY_STRING) {
        /* `raise "msg"` raises RuntimeError with the message */
        buf_puts(b, "sp_raise("); emit_expr(c, av[0], b); buf_puts(b, ")");
      }
      else if (at == TY_POLY) {
        /* the runtime value may be a string, an exception object, or a
           non-exception (TypeError) -- dispatch on the tag */
        buf_puts(b, "sp_raise_poly("); emit_boxed(c, av[0], b); buf_puts(b, ")");
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
    return;
  }

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
      return;
    }
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      const char *fn = exc_has_user_msg_override(c)
        ? (sp_streq(name, "message") ? "sp_user_exc_message" : "sp_user_exc_to_s")
        : "sp_exc_message";
      buf_printf(b, "%s((sp_Exception *)(", fn); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
  }

  /* exception object methods */
  if (recv >= 0 && comp_ntype(c, recv) == TY_EXCEPTION) {
    /* equal? is pointer identity; == is CRuby value equality (same class
       and message). */
    if (sp_streq(name, "equal?") && argc == 1 &&
        comp_ntype(c, argv[0]) == TY_EXCEPTION) {
      buf_puts(b, "(((sp_Exception *)("); emit_expr(c, recv, b);
      buf_puts(b, ")) == ((sp_Exception *)("); emit_expr(c, argv[0], b); buf_puts(b, ")))");
      return;
    }
    if (sp_streq(name, "==") && argc == 1 &&
        comp_ntype(c, argv[0]) == TY_EXCEPTION) {
      buf_puts(b, "sp_exc_eq((sp_Exception *)("); emit_expr(c, recv, b);
      buf_puts(b, "), (sp_Exception *)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "nil?") && argc == 0) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)");
      return;
    }
    /* eql? is Object#eql? -- identity, not Exception#== (#2769) */
    if (sp_streq(name, "eql?") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_EXCEPTION) {
        buf_puts(b, "(((sp_Exception *)("); emit_expr(c, recv, b);
        buf_puts(b, ")) == ((sp_Exception *)("); emit_expr(c, argv[0], b); buf_puts(b, ")))");
      } else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
        emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
      }
      return;
    }
    /* dup/clone copy the whole (subclass-sized) struct so mutating the copy
       leaves the original alone (#2772) */
    if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
      buf_puts(b, "sp_exc_dup((sp_Exception *)(");
      emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    /* Exception#exception: no-arg returns the receiver; with a message it is
       a copy carrying the new message (#2740) */
    if (sp_streq(name, "exception")) {
      if (argc == 0) { emit_expr(c, recv, b); return; }
      if (argc == 1) {
        buf_puts(b, "sp_exc_exception((sp_Exception *)(");
        emit_expr(c, recv, b); buf_puts(b, "), ");
        if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        buf_puts(b, ")");
        return;
      }
    }
    /* NameError/NoMethodError#name: the carried missing name; any other
       exception class raises NoMethodError at runtime, per CRuby */
    if (sp_streq(name, "name") && argc == 0) {
      g_uses_symbols = 1;  /* the accessor interns a runtime-recovered name (#2758) */
      buf_puts(b, "sp_exc_name_acc((sp_Exception *)(");
      emit_expr(c, recv, b);
      buf_puts(b, "))");
      return;
    }
    /* class-gated introspection accessors (#2753-#2756, #2770) */
    if (argc == 0) {
      const char *accfn = NULL;
      if (sp_streq(name, "key")) accfn = "sp_exc_key_acc";
      else if (sp_streq(name, "receiver")) accfn = "sp_exc_receiver_acc";
      else if (sp_streq(name, "args")) accfn = "sp_exc_args_acc";
      else if (sp_streq(name, "reason")) accfn = "sp_exc_reason_acc";
      else if (sp_streq(name, "exit_value")) accfn = "sp_exc_exit_value_acc";
      else if (sp_streq(name, "tag")) accfn = "sp_exc_tag_acc";
      else if (sp_streq(name, "value")) accfn = "sp_exc_throw_value_acc";
      else if (sp_streq(name, "private_call?")) accfn = "sp_exc_private_call_acc";
      else if (sp_streq(name, "status")) accfn = "sp_exc_status_acc";
      else if (sp_streq(name, "success?")) accfn = "sp_exc_success_acc";
      else if (sp_streq(name, "signo")) accfn = "sp_exc_signo_acc";
      else if (sp_streq(name, "signm")) accfn = "sp_exc_signm_acc";
      if (accfn) {
        if (sp_streq(name, "reason") || sp_streq(name, "tag") || sp_streq(name, "key"))
          g_uses_symbols = 1;  /* staged names intern back to symbols */
        buf_printf(b, "%s((sp_Exception *)(", accfn);
        emit_expr(c, recv, b);
        buf_puts(b, "))");
        return;
      }
    }
    if (sp_streq(name, "inspect") && argc == 0) {
      int ei = ++g_tmp;
      buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)(", ei); emit_expr(c, recv, b);
      buf_printf(b, "); _t%d ? sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d))"
                    " : (&(\"\\xff\" \"nil\")[1]); })", ei, ei, ei);
      return;
    }
    if (sp_streq(name, "message") || sp_streq(name, "to_s") || sp_streq(name, "to_str")) {
      /* NULL-guard: a nil $! (outside any rescue) has no message. */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      const char *fn = exc_has_user_msg_override(c)
        ? (sp_streq(name, "message") ? "sp_user_exc_message" : "sp_user_exc_to_s")
        : "sp_exc_message";
      buf_printf(b, "(_t%d ? %s(_t%d) : \"\")", t, fn, t);
      return;
    }
    if (sp_streq(name, "cause")) {
      buf_puts(b, "sp_exc_cause("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "result") && argc == 0) {
      /* StopIteration#result: the finished iteration's return value. */
      buf_puts(b, "sp_exc_result("); emit_expr(c, recv, b); buf_puts(b, ")");
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
      /* #<ClassName: message>, or "nil" for a nil $! (outside any rescue). */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "(_t%d ? sp_sprintf(\"#<%%s: %%s>\", sp_exc_class_name(_t%d), sp_exc_message(_t%d)) : \"nil\")", t, t, t);
      return;
    }
    if (sp_streq(name, "class")) {  /* a Class carried by name (complete for every exception class) */
      /* a nil $! (outside any rescue) is NilClass, matching the sibling nil-guards. */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Exception *_t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      buf_printf(b, "((sp_Class){0, _t%d ? sp_exc_class_name(_t%d) : \"NilClass\"})", t, t);
      return;
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
      } else if (at == TY_NIL) {
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "((void *)(%s) == NULL)", rb.p ? rb.p : "0");
        free(rb.p);
      } else {
        buf_puts(b, "0");
      }
      return;
    }
    if (sp_streq(name, "backtrace")) {
      /* the stack captured at the most recent raise (sp_bt_buf); the substrate
         is live in --debug builds and empty in release, same as Kernel#caller. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_backtrace_captured())");
      return;
    }
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      const char *cn = isa_const_name(nt, argv[0]);
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
        ClassInfo *ncls = &c->classes[new_cls];
        int initm = comp_method_in_chain(c, new_cls, "initialize", NULL);
        /* A Data/Struct class has no user `initialize`; its generated constructor
           takes one arg per member. Fill member-wise -- positionally, or by
           keyword when a trailing kwarg hash names each member -- exactly as the
           receiver `Klass.new(...)` path does. Without this, a bare `new(...)`
           inside a `def self.default`/`self.initial` factory emitted an empty
           `sp_Klass_new()`, dropping every argument. */
        if (ncls->is_struct && initm < 0) {
          int sargc; const int *sargv = call_args(nt, id, &sargc);
          int kwh = (sargc == 1 && nt_type(nt, sargv[0]) &&
                     sp_streq(nt_type(nt, sargv[0]), "KeywordHashNode")) ? sargv[0] : -1;
          buf_printf(b, "sp_%s_new(", ncls->c_name);
          for (int a = 0; a < ncls->nivars; a++) {
            if (a) buf_puts(b, ", ");
            int vnode = -1;
            if (kwh >= 0) vnode = struct_kwarg_value(c, kwh, ncls->ivars[a] + 1);
            else if (a < sargc) vnode = sargv[a];
            if (vnode >= 0) {
              if (ncls->ivar_types[a] == TY_POLY && comp_ntype(c, vnode) != TY_POLY) emit_boxed(c, vnode, b);
              else emit_expr(c, vnode, b);
            }
            else buf_puts(b, default_value(ncls->ivar_types[a]));
          }
          buf_puts(b, ")");
          return;
        }
        /* yielding initialize: inline its body at the call site exactly as
           the Klass.new receiver path does -- the emitted constructor only
           allocates, so without the inline the body's @ivar writes vanish
           (with or without a block at this site) */
        if (initm >= 0 && c->scopes[initm].yields &&
            emit_ctor_yield_inline(c, id, new_cls, b)) return;
        buf_printf(b, "sp_%s_new(", ncls->c_name);
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
        emit_cmethod_block_arg(c, id, ms, -1, b);
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
      emit_cmethod_block_arg(c, id, ms, -1, b);
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
      /* every .class now yields an sp_Class (the poly path included, via
         sp_poly_class_val): stringify uniformly. */
      int _clt = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_to_s(_cl%d); })", _clt);
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
  /* Regexp.last_match -> the last MatchData ($~), or nil */
  if (recv >= 0 && argc == 0 && sp_streq(name, "last_match") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    buf_puts(b, "sp_re_last_matchdata()");
    return;
  }
  /* Regexp.try_convert(x) -> x if it is a Regexp, else nil */
  if (recv >= 0 && argc == 1 && sp_streq(name, "try_convert") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (comp_ntype(c, argv[0]) == TY_REGEX) {
      buf_puts(b, "sp_box_regexp((void *)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
    } else {
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    }
    return;
  }
  /* Regexp.timeout / Regexp.timeout= -> spinel enforces no global match timeout;
     the getter is nil and the setter is a no-op returning its argument. */
  if (recv >= 0 && (sp_streq(name, "timeout") || sp_streq(name, "timeout=")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (argc == 0) { buf_puts(b, "sp_box_nil()"); return; }
    if (argc == 1) { buf_puts(b, "("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return; }
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
    /* A single Array-valued argument whose elements are only known at run time
       (a variable/expression, not a literal) is joined by the runtime helper. */
    else if (argc == 1) {
      TyKind uat = comp_ntype(c, argv[0]);
      if (uat == TY_POLY_ARRAY || uat == TY_STR_ARRAY) {
        buf_puts(b, "sp_re_union_array(");
        if (uat == TY_STR_ARRAY) { buf_puts(b, "sp_StrArray_to_poly_fmt("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return;
      }
    }
    /* A single Regexp operand is returned unchanged (CRuby keeps its source and
       flags verbatim, no option-group wrapper). */
    if (nops == 1 && re_lit_src(c, ops[0]) && emit_regex_pat_to_buf(c, ops[0], b))
      return;
    int ts = ++g_tmp, tp = ++g_tmp;
    for (int i = 0; i < nops; i++) {
      Buf ab; memset(&ab, 0, sizeof ab);
      const char *resrc = re_lit_src(c, ops[i]);
      if (resrc) {
        /* Regexp operand. CRuby splices each operand's #to_s form
           `(?on-off:src)` -- an inline option group carrying its flags -- for
           EVERY Regexp operand in a multi-operand union, flagged or not
           (Regexp.union(/a/, /b/) == /(?-mix:a)|(?-mix:b)/, #2624). */
        int rli = re_lit_index(c, ops[i]);
        if (rli >= 0)
          buf_printf(&ab, "sp_re_to_s_str((void *)sp_re_pat_%d)", rli);
        else
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
      /* a Struct/Data-generated class sits under the Struct/Data builtin */
      int bpar = c->classes[ci].is_struct ? (c->classes[ci].is_data ? -146 : -145)
                                          : -116;  /* Object */
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
    TyKind rt = comp_recv_type(c, recv);  /* empty-literal receivers coerce */
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
    /* MatchData is nullable (nil on no-match): .class checks at run time so a
       non-match reports NilClass, not MatchData (#2311) */
    if (rt == TY_MATCHDATA) {
      int tm = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = ", tm); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d ? ((sp_Class){(mrb_int)-1, \"MatchData\"})"
                    " : ((sp_Class){(mrb_int)-1, \"NilClass\"}); })", tm);
      return;
    }
    const char *cn = NULL;
    if (rt == TY_INT) cn = "Integer";
    else if (rt == TY_FLOAT) cn = "Float";
    else if (rt == TY_STRING) cn = "String";
    else if (rt == TY_SYMBOL) cn = "Symbol";
    else if (rt == TY_RANGE) cn = "Range";
    else if (rt == TY_TIME) cn = "Time";
    else if (rt == TY_FIBER) cn = "Fiber";
    else if (rt == TY_ENUMERATOR) {
      /* a chain built by Enumerable#chain / Enumerator#+ reports as
         Enumerator::Chain; every other enumerator is an Enumerator (#2545) */
      int te = ++g_tmp;
      buf_printf(b, "({ sp_Enumerator *_t%d = ", te); emit_expr(c, recv, b);
      buf_printf(b, "; (_t%d && _t%d->is_chain) ? ((sp_Class){(mrb_int)-1, \"Enumerator::Chain\"})"
                    " : ((sp_Class){(mrb_int)-1, \"Enumerator\"}); })", te, te);
      return;
    }
    else if (rt == TY_DIR) cn = "Dir";
    else if (rt == TY_IO) {
      /* a stat handle is a File::Stat (#2841); a path-backed handle is a
         File; a raw stream (STDOUT, pipe end) is an IO (#2797) */
      int tio = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = ", tio); emit_expr(c, recv, b);
      buf_printf(b, "; (_t%d && _t%d->mode && strcmp(_t%d->mode, \"stat\") == 0)"
                    " ? ((sp_Class){(mrb_int)-1, \"File::Stat\"})"
                    " : (_t%d && sp_File_path(_t%d)[0] && sp_File_path(_t%d)[0] != '<')"
                    " ? ((sp_Class){(mrb_int)-121, \"File\"})"
                    " : ((sp_Class){(mrb_int)-120, \"IO\"}); })",
                 tio, tio, tio, tio, tio, tio);
      return;
    }
    else if (rt == TY_ARGF) cn = "ARGF.class";  /* ARGF's singleton class name (CRuby) */
    else if (rt == TY_NIL) cn = "NilClass";
    else if (rt == TY_METHOD) cn = method_expr_is_unbound(c, recv) ? "UnboundMethod" : "Method";
    else if (rt == TY_MATCHDATA) cn = "MatchData";
    else if (rt == TY_REGEX) cn = "Regexp";
    else if (rt == TY_PROC) cn = "Proc";
    else if (rt == TY_CURRY) cn = "Proc";  /* a curried proc is a Proc (#2651) */
    else if (rt == TY_COMPLEX) cn = "Complex";
    else if (rt == TY_RATIONAL) cn = "Rational";
    else if (ty_is_array(rt)) cn = "Array";
    else if (ty_is_hash(rt)) cn = "Hash";
    else if (ty_is_object(rt)) {
      /* user object: .class returns a TY_CLASS value */
      int _cidx = ty_object_class(rt);
      /* a value-type instance has the class's static cls_id (no NULL case) */
      if (comp_ty_value_obj(c, rt)) { buf_printf(b, "((sp_Class){%d})", _cidx); return; }
      /* A bare Object/BasicObject instance uses the runtime sp_Object struct
         ({uint8_t _pad}) -- it has no cls_id field to read (every generated
         user-class struct does). Its class is statically that base, so emit the
         name-backed value and side-effect-eval the receiver. */
      const char *_ocn = _cidx >= 0 && _cidx < c->nclasses ? c->classes[_cidx].name : NULL;
      if (_ocn && (sp_streq(_ocn, "Object") || sp_streq(_ocn, "BasicObject"))) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(mrb_int)-1, \"%s\"}))", _ocn);
        return;
      }
      /* a native-bound class's struct is opaque in the generated TU (no
         cls_id deref possible); its class is statically known */
      if (_cidx >= 0 && c->classes[_cidx].is_native_class) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(mrb_int)%d, \"%s\"}))", _cidx, c->classes[_cidx].name);
        return;
      }
      /* an exception subclass shares sp_Exception's layout (no cls_id
         member); its runtime class is the carried cls_name */
      if (_cidx >= 0 && class_is_exc_subclass(c, _cidx)) {
        /* evaluate the receiver BEFORE writing the temp's declaration head:
           its emission may hoist declarations of its own into g_pre */
        int _texc = ++g_tmp;
        Buf _eb = expr_buf(c, recv);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", _texc);
        buf_puts(g_pre, _eb.p ? _eb.p : ""); buf_puts(g_pre, ";\n"); free(_eb.p);
        buf_printf(b, "((sp_Class){(mrb_int)-1, _t%d ? ((sp_Exception *)_t%d)->cls_name : \"NilClass\"})", _texc, _texc);
        return;
      }
      int _tobj = ++g_tmp;
      Buf _rb = expr_buf(c, recv);
      emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", _tobj);
      buf_puts(g_pre, _rb.p ? _rb.p : ""); buf_puts(g_pre, ";\n"); free(_rb.p);
      buf_printf(b, "((sp_Class){_t%d ? _t%d->cls_id : %d})", _tobj, _tobj, _cidx);
      return;
    }
    if (cn && rt == TY_INT) {
      /* an int slot can hold the nil sentinel (a nil-returning <=>, a
         missing key): report NilClass then, matching how p prints it */
      int tcv = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", tcv); emit_expr(c, recv, b);
      buf_printf(b, "; ((sp_Class){(mrb_int)-1, _t%d == SP_INT_NIL ? \"NilClass\" : \"Integer\"}); })", tcv);
      return;
    }
    if (cn && rt == TY_FLOAT) {
      /* same for the float nil sentinel (NaN-boxed nil) */
      int tcv = ++g_tmp;
      buf_printf(b, "({ mrb_float _t%d = ", tcv); emit_expr(c, recv, b);
      buf_printf(b, "; ((sp_Class){(mrb_int)-1, sp_float_is_nil(_t%d) ? \"NilClass\" : \"Float\"}); })", tcv);
      return;
    }
    if (cn) {
      /* a first-class name-backed Class value; the receiver is side-effect-
         evaluated when it is not a plain read */
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_printf(b, "), ((sp_Class){(mrb_int)-1, \"%s\"}))", cn);
      return;
    }
    if (rt == TY_BOOL) {
      buf_puts(b, "(("); emit_expr(c, recv, b);
      buf_puts(b, ") ? ((sp_Class){(mrb_int)-1, \"TrueClass\"}) : ((sp_Class){(mrb_int)-1, \"FalseClass\"}))");
      return;
    }
    if (rt == TY_POLY) {
      buf_puts(b, "sp_poly_class_val("); emit_expr(c, recv, b); buf_puts(b, ")");
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
    if (sp_streq(name, "nil?")) {
      /* a class value is nil only when it is the nil-class sentinel
         (BasicObject#superclass); every real class is non-nil (#2654) */
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_nil_p(_cl%d); })", _clt);
      return;
    }
    /* const_defined?(:NAME) with a literal name answers at compile time from
       the (flat) constant and class tables -- constants carry no class
       qualifier in the registry, so this is the same namespace a read
       resolves against. */
    if (sp_streq(name, "const_defined?") && argc == 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *cn0 = NULL;
      if (a0ty && sp_streq(a0ty, "SymbolNode")) cn0 = nt_str(nt, argv[0], "value");
      else if (a0ty && sp_streq(a0ty, "StringNode")) cn0 = nt_str(nt, argv[0], "unescaped");
      if (cn0) {
        if (const_name_is_wrong(cn0)) {
          buf_printf(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_raise_cls(\"NameError\", sp_sprintf(\"wrong constant name %%s\", ");
          emit_str_literal(b, cn0);
          buf_printf(b, ")), 0)");
          return;
        }
        int yes = (comp_const(c, cn0) != NULL) || comp_class_index(c, cn0) >= 0;
        buf_printf(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
        return;
      }
    }
    if (sp_streq(name, "class")) {
      buf_printf(b, "({ sp_Class _cl%da = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%da)"
                    "?((sp_Class){(mrb_int)-1, \"Module\"})"
                    ":((sp_Class){(mrb_int)-1, \"Class\"}); })", _clt);
      return;
    }
    if (sp_streq(name, "superclass") && argc == 0) {
      /* sp_class_superclass only knows the user chain; a builtin class needs
         sp_builtin_superclass (Integer -> Numeric), as sp_class_is_ancestor
         already dispatches. BasicObject (the root) yields the nil-class
         sentinel there (#2654). A Module has no #superclass -> NoMethodError. */
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%d) ? "
                    "(sp_raise_cls(\"NoMethodError\", sp_sprintf(\"undefined method 'superclass' for module %%s\", sp_class_to_s(_cl%d))), (sp_Class){0}) : "
                    "(_cl%d.cls_id>=0?sp_class_superclass(_cl%d):sp_builtin_superclass(_cl%d)); })",
                 _clt, _clt, _clt, _clt, _clt);
      return;
    }
    if (sp_streq(name, "ancestors") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_ancestors(_cl%d); })", _clt);
      return;
    }
    /* Module#included_modules: the module ancestors (#2674) */
    if (sp_streq(name, "included_modules") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_included_modules(_cl%d); })", _clt);
      return;
    }
    /* Module#constants: the constant registry is a flat namespace with no class
       qualifier, so recover the ownership from the AST -- a class/module's own
       constants are the ConstantWriteNodes in its body (across reopenings).
       CRuby lists them per ancestor in ancestors order (own, then prepended and
       included modules, then the superclass chain), stopping before Object,
       whose constants are the top-level ones. `constants(false)` is own-only.
       #2674 */
    if (sp_streq(name, "constants") && argc <= 1) {
      const char *rcn = nt_type(nt, recv) &&
                        (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
                         sp_streq(nt_type(nt, recv), "ConstantPathNode"))
                        ? nt_str(nt, recv, "name") : NULL;
      int rci = rcn ? comp_class_index(c, rcn) : -1;
      int inherit = 1;
      if (argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        if (aty && sp_streq(aty, "FalseNode")) inherit = 0;
        else if (!aty || !sp_streq(aty, "TrueNode")) rci = -1;  /* non-literal: leave it */
      }
      if (rci >= 0) {
        const char *names[128];
        int n = collect_class_constants(c, rci, inherit, names, 128, 0, 0);
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        /* intern at runtime: a constant name is not otherwise a symbol in the
           program, so comp_sym_intern here would come too late for the
           generated name table and the symbol would render empty */
        for (int k = 0; k < n; k++) {
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", ta);
          emit_str_literal(b, names[k]);
          buf_puts(b, ")));");
        }
        buf_printf(b, " _t%d; })", ta);
        return;
      }
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
            /* skip shadow methods and compiler-synthesized helpers */
            if (strncmp(s->name, "__prep_", 7) == 0) continue;
            if (name_is_synth_method(s->name)) continue;
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
        buf_printf(b, "; sp_class_eq(_cl%d, _cl%da); })", _clt, _clt);
        return;
      }
    }
    if (sp_streq(name, "!=" ) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; !sp_class_eq(_cl%d, _cl%da); })", _clt, _clt);
        return;
      }
    }
    if ((sp_streq(name, "<") || sp_streq(name, "<=") || sp_streq(name, ">") ||
         sp_streq(name, ">=") || sp_streq(name, "<=>")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        /* CRuby returns nil (not false) when the classes are unrelated, so the
           tri-state helpers yield a boxed true/false/nil (or -1/0/1/nil for
           <=>). */
        const char *fn = sp_streq(name, "<") ? "sp_class_lt3" :
                         sp_streq(name, "<=") ? "sp_class_le3" :
                         sp_streq(name, ">") ? "sp_class_gt3" :
                         sp_streq(name, ">=") ? "sp_class_ge3" : "sp_class_cmp3";
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; %s(_cl%d, _cl%da); })", fn, _clt, _clt);
        return;
      }
    }
    /* Class#subclasses: the direct, instantiated subclasses, known from the
       compile-time class graph. Constant class receiver only. (#2656) */
    if (sp_streq(name, "subclasses") && argc == 0) {
      const char *scty = nt_type(nt, recv);
      int scid = (scty && sp_streq(scty, "ConstantReadNode")) ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
      if (scid >= 0) {
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        for (int k = 0; k < c->nclasses; k++) {
          if (c->classes[k].parent != scid) continue;   /* defined subclasses, even if never .new'd */
          if (is_builtin_reopen(c->classes[k].name)) continue;
          const char *kn = class_ruby_name(c, k); if (!kn) kn = c->classes[k].name;
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_class(((sp_Class){%d, \"%s\"})));", ta, k, kn);
        }
        buf_printf(b, " _t%d; })", ta);
        return;
      }
    }
    /* a named class/module value is never a singleton class (spinel has no
       singleton-class objects), so #singleton_class? is always false. */
    if (sp_streq(name, "singleton_class?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
      return;
    }
    /* Module#include?(mod): mod must be a Module (a Class arg is a TypeError);
       true when mod is a proper ancestor of the receiver (#2674). */
    if (sp_streq(name, "include?") && argc == 1 && comp_ntype(c, argv[0]) == TY_CLASS) {
      int m = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Class _cl%d = ", m); emit_expr(c, argv[0], b);
      buf_printf(b, "; if (!sp_class_is_module_val(_cl%d)) sp_raise_cls(\"TypeError\", "
                    "\"wrong argument type Class (expected Module)\");", m);
      buf_printf(b, " _cl%d.cls_id != _cl%d.cls_id && sp_class_le(_cl%d, _cl%d); })", _clt, m, _clt, m);
      return;
    }
    /* Module#class_variables: the registered cvars, own + ancestors (#2719) */
    if (sp_streq(name, "class_variables") && argc == 0) {
      const char *rcn9 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")
                         ? nt_str(nt, recv, "name") : NULL;
      int rci9 = rcn9 ? comp_class_index(c, rcn9) : -1;
      if (rci9 >= 0) {
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        for (int k9 = rci9; k9 >= 0; k9 = c->classes[k9].parent)
          for (int q9 = 0; q9 < c->classes[k9].ncvars; q9++) {
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", ta);
            emit_str_literal(b, c->classes[k9].cvars[q9]);
            buf_puts(b, ")));");
          }
        buf_printf(b, " _t%d; })", ta);
        return;
      }
    }
    /* class-variable reflection with a literal @@name on a constant class
       receiver: resolve to the cvar's C global (#2694). */
    if ((sp_streq(name, "class_variable_get") || sp_streq(name, "class_variable_set") ||
         sp_streq(name, "class_variable_defined?")) && argc >= 1) {
      const char *aty2 = nt_type(nt, argv[0]);
      const char *cvn = (aty2 && sp_streq(aty2, "SymbolNode")) ? nt_str(nt, argv[0], "value")
                      : (aty2 && sp_streq(aty2, "StringNode")) ? nt_str(nt, argv[0], "content") : NULL;
      const char *rty2 = nt_type(nt, recv);
      int ccid = (rty2 && sp_streq(rty2, "ConstantReadNode")) ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
      if (cvn && cvn[0] == '@' && cvn[1] == '@' && ccid >= 0) {
        int cvi = comp_cvar_index(&c->classes[ccid], cvn);
        if (sp_streq(name, "class_variable_defined?")) { buf_printf(b, "%d", cvi >= 0 ? 1 : 0); return; }
        char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[ccid].name, cvn + 2);
        if (cvi >= 0) {
          TyKind ct = c->classes[ccid].cvar_types[cvi];
          if (sp_streq(name, "class_variable_get")) { emit_boxed_text(c, ct, ref, b); return; }
          if (sp_streq(name, "class_variable_set") && argc == 2) {
            buf_printf(b, "(%s = ", ref);
            if (ct == TY_POLY) emit_boxed(c, argv[1], b); else emit_expr(c, argv[1], b);
            buf_puts(b, ", "); emit_boxed_text(c, ct, ref, b); buf_puts(b, ")");
            return;
          }
        }
        else if (sp_streq(name, "class_variable_get")) {
          buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized class variable %s in %s\"), sp_box_nil())",
                     cvn, c->classes[ccid].name);
          return;
        }
      }
    }
    /* Klass === obj is obj.is_a?(Klass): does the operand's runtime class have
       the receiver class among its ancestors. Only for a user-class receiver --
       the primitive type names (Integer === 5, Comparable === 5) have their own
       tag-based fold elsewhere, which this must not shadow. */
    if (sp_streq(name, "===") && argc == 1) {
      const char *rvt2 = nt_type(nt, recv);
      const char *rcn2 = (rvt2 && (sp_streq(rvt2, "ConstantReadNode") ||
                                   sp_streq(rvt2, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
      if (rcn2 && comp_class_index(c, rcn2) >= 0) {
        int o = ++g_tmp;
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", o); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_poly_is_a(_t%d, _cl%d); })", o, _clt);
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
    /* a user class method called on a Class-typed value carried in a plain
       variable / parameter (`model.table_name` where model is a Class object):
       switch on the boxed class id and call the matching class's static
       method. A constant / accessor receiver (`Foo.bar`, `Reg.handler.run`)
       keeps its existing direct or Stage-2 dispatch. Only user classes that
       define (or inherit) the name get an arm; the result unifies their return
       types (poly when they disagree). (#2445) */
    {
      const char *rvty9 = nt_type(nt, recv);
      int recv_is_var9 = rvty9 && (sp_streq(rvty9, "LocalVariableReadNode") ||
                                   sp_streq(rvty9, "InstanceVariableReadNode"));
      int ncand9 = 0, defmi9 = -1;
      if (!recv_is_var9) goto skip_cls_cmethod9;
      TyKind uret9 = TY_UNKNOWN; int uret_set9 = 0;
      for (int k = 0; k < c->nclasses; k++) {
        if (is_builtin_reopen(c->classes[k].name)) continue;
        int kmi = comp_cmethod_in_chain(c, k, name, NULL);
        if (kmi < 0) continue;
        ncand9++; defmi9 = kmi;
        TyKind kr = (TyKind)c->scopes[kmi].ret;
        if (!uret_set9) { uret9 = kr; uret_set9 = 1; }
        else if (kr != uret9) uret9 = TY_POLY;
      }
      if (ncand9 > 0) {
        /* Every candidate must accept exactly this call's positional args (no
           block/yield). Different candidates may declare different param
           types, so emit_args_filled runs per arm against that method's
           signature -- the args are hoisted into poly temps first so a
           side-effecting argument is evaluated once, not once per arm. */
        int simple9 = (nt_ref(nt, id, "block") < 0);
        for (int k = 0; simple9 && k < c->nclasses; k++) {
          if (is_builtin_reopen(c->classes[k].name)) continue;
          int kmi = comp_cmethod_in_chain(c, k, name, NULL);
          if (kmi < 0) continue;
          if (c->scopes[kmi].yields ||
              (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0]) ||
              c->scopes[kmi].nrequired != argc || c->scopes[kmi].rest_idx >= 0) simple9 = 0;
        }
        if (simple9) {
          TyKind slot9 = is_scalar_ret(uret9) && uret9 != TY_VOID && uret9 != TY_UNKNOWN
                         ? uret9 : (ty_is_object(uret9) ? uret9 : TY_POLY);
          int tk9 = ++g_tmp, tr9 = ++g_tmp;
          buf_puts(b, "({ ");
          /* hoist each argument into a rooted poly temp (evaluate once) */
          int atmp9[16]; int na9 = argc < 16 ? argc : 16;
          for (int a = 0; a < na9; a++) {
            atmp9[a] = ++g_tmp;
            buf_printf(b, "sp_RbVal _t%d = ", atmp9[a]); emit_boxed(c, argv[a], b);
            buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", atmp9[a]);
          }
          buf_printf(b, "sp_Class _t%d = ", tk9); emit_expr(c, recv, b); buf_puts(b, "; ");
          emit_ctype(c, slot9, b);
          buf_printf(b, " _t%d = %s; switch (_t%d.cls_id) {", tr9,
                     slot9 == TY_POLY ? "sp_box_nil()" : default_value(slot9), tk9);
          for (int k = 0; k < c->nclasses; k++) {
            if (is_builtin_reopen(c->classes[k].name)) continue;
            int defcls9 = -1;
            int kmi = comp_cmethod_in_chain(c, k, name, &defcls9);
            if (kmi < 0) continue;
            TyKind kr = (TyKind)c->scopes[kmi].ret;
            buf_printf(b, " case %d: _t%d = ", k, tr9);
            Buf cb9; memset(&cb9, 0, sizeof cb9);
            emit_method_cname(c, &c->scopes[kmi], &cb9);
            buf_printf(&cb9, "(");
            Scope *ks9 = &c->scopes[kmi];
            for (int a = 0; a < na9; a++) {
              if (a) buf_puts(&cb9, ", ");
              LocalVar *pp = a < ks9->nparams ? scope_local(ks9, ks9->pnames[a]) : NULL;
              TyKind pt = pp ? pp->type : TY_POLY;
              char at[32]; snprintf(at, sizeof at, "_t%d", atmp9[a]);
              if (pt == TY_POLY) buf_puts(&cb9, at);
              else emit_unbox_text(c, pt, at, &cb9);
            }
            buf_puts(&cb9, ")");
            if (slot9 == TY_POLY && kr != TY_POLY) emit_boxed_text(c, kr, cb9.p ? cb9.p : "", b);
            else if (slot9 != TY_POLY && kr == TY_POLY) emit_unbox_text(c, slot9, cb9.p ? cb9.p : "", b);
            else buf_puts(b, cb9.p ? cb9.p : "");
            free(cb9.p);
            buf_puts(b, "; break;");
          }
          buf_printf(b, " } _t%d; })", tr9);
          (void)defmi9;
          return;
        }
      }
      skip_cls_cmethod9:;
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
        ClassInfo *dci = &c->classes[cid];
        const char *cn = dci->c_name;
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
          buf_printf(b, "sp_%s_initialize_copy(", c->classes[defcls].c_name);
          if (defcls != cid) buf_printf(b, "(sp_%s *)", c->classes[defcls].c_name);
          if (ictp == TY_POLY) { buf_printf(b, "_t%d, sp_box_obj(_t%d, %d)); ", td, to, cid); }
          else {
            int icid = ty_object_class(ictp);
            buf_printf(b, "_t%d, ", td);
            if (icid != cid) buf_printf(b, "(sp_%s *)", c->classes[icid].c_name);
            buf_printf(b, "_t%d); ", to);
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
        return;
      }
    }
  }

  /* nil? on a pointer-backed Enumerator: NULL encodes nil, as elsewhere */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") &&
      comp_ntype(c, recv) == TY_ENUMERATOR) {
    buf_puts(b, "("); emit_expr(c, recv, b); buf_puts(b, " == NULL)");
    return;
  }

  /* frozen? on an immutable value type is constantly true (CRuby freezes
     Integer/Float/Symbol/booleans/nil/Range/Complex/Rational values) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?")) {
    TyKind fvt = comp_ntype(c, recv);
    if (fvt == TY_NIL) { buf_puts(b, "1"); return; }
    if (fvt == TY_INT || fvt == TY_FLOAT || fvt == TY_SYMBOL || fvt == TY_BOOL ||
        fvt == TY_RANGE || fvt == TY_COMPLEX || fvt == TY_RATIONAL ||
        fvt == TY_BIGINT) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return;
    }
  }

  /* identity methods -> the receiver itself */
  if (recv >= 0 &&
      (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
       sp_streq(name, "dup") || sp_streq(name, "clone"))) {
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
       identity shortcut */
    int freeze_stateful = sp_streq(name, "freeze") &&
                          (ty_is_object(recv_t) || recv_t == TY_POLY);
    /* itself is pure identity for every receiver, hashes included; the
       hash exclusion below is for freeze/dup/clone, which need real
       handling on the reference types */
    if (argc0 == 0 && sp_streq(name, "itself")) { emit_expr(c, recv, b); return; }
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
                     icn, default_value(recv_t));
          return;
        }
        emit_expr(c, recv, b); return;
      }
    }
    /* dup/clone of a boxed plain object copies (Object.new included); the
       identity shortcut would alias the original and o.dup == o would
       misreport true */
    if (argc0 == 0 && is_dup_clone && recv_t == TY_POLY) {
      buf_printf(b, "sp_poly_dup(");
      emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "clone") ? 1 : 0);
      return;
    }
    if (argc0 == 0 && !freeze_stateful && !ty_is_hash(recv_t) &&
        !(is_dup_clone && (recv_t == TY_STRING || ty_is_array(recv_t) || recv_native))) {
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
      buf_printf(g_pre, "sp_%s %s_t%d = %s;\n", c->classes[cls_id].c_name,
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
          /* an unused param stays TY_UNKNOWN and gets no C declaration:
             evaluate its rvalue for effect only (#2734) */
          int pdecl = plv && plv->type != TY_UNKNOWN;
          emit_indent(g_pre, g_indent);
          if (!pdecl) buf_puts(g_pre, "(void)(");
          else buf_printf(g_pre, "lv_%s = ", rename_local(pn));
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
          buf_puts(g_pre, pdecl ? ";\n" : ");\n");
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
      /* a zero-arg .new can only construct a class whose initialize takes no
         required args; an arg-requiring ctor would be an ArgumentError in MRI,
         and its C function has parameters -- omit its arm (a runtime cls_id for
         it lands in the sp_box_nil default), matching MRI's raise. (#2450) */
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      if (initm >= 0 && c->scopes[initm].nrequired != 0) continue;
      if (c->classes[ci].is_struct || c->classes[ci].is_native_class) continue;
      /* fill any optional constructor params with their defaults (an
         optional-arg initialize is zero-arg-compatible but its C function
         still declares the slots, #2452); the call site supplies no args, so
         emit_args_filled emits each param's default. */
      Buf ab9; memset(&ab9, 0, sizeof ab9);
      if (initm >= 0 && c->scopes[initm].nparams > 0)
        emit_args_filled(c, initm, -1, "", &ab9);
      const char *args9 = ab9.p ? ab9.p : "";
      /* a value-type object returns by value: box via its vobj boxer, not
         sp_box_obj which expects a heap pointer (#2450) */
      if (c->classes[ci].is_value_type)
        buf_printf(b, "case %d: _t%d=sp_box_vobj_%s(sp_%s_new(%s));break;",
                   ci, rt2, c->classes[ci].c_name, c->classes[ci].c_name, args9);
      else
        buf_printf(b, "case %d: _t%d=sp_box_obj(sp_%s_new(%s),%d);break;",
                   ci, rt2, c->classes[ci].c_name, args9, ci);
      free(ab9.p);
    }
    buf_printf(b, "} _t%d; })", rt2);
    return;
  }

  /* poly.new(args): the receiver is a Class value read out of a container
     (`REG["c"].new(2)`). Switch on its runtime cls_id and construct, coercing
     each argument to the target constructor's parameter type. Only a class whose
     constructor takes exactly `argc` positional params (all required) gets an
     arm; any other runtime class lands in the NoMethodError default, matching
     CRuby's ArgumentError/NoMethodError. (#2888) */
  if (recv >= 0 && sp_streq(name, "new") && comp_ntype(c, recv) == TY_POLY &&
      nt_ref(nt, id, "block") < 0) {
    int kt = ++g_tmp, rt2 = ++g_tmp;
    int *atmp = argc ? calloc(argc, sizeof(int)) : NULL;
    buf_printf(b, "({ sp_RbVal _t%d = ", kt); emit_expr(c, recv, b); buf_puts(b, "; ");
    for (int a = 0; a < argc; a++) {
      atmp[a] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", atmp[a]); emit_boxed(c, argv[a], b); buf_puts(b, "; ");
    }
    buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); switch(_t%d.cls_id){", rt2, kt);
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (is_builtin_reopen(c->classes[ci].name) || c->classes[ci].is_struct ||
          c->classes[ci].is_native_class || !c->classes[ci].instantiated) continue;
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      int np = initm >= 0 ? c->scopes[initm].nparams : 0;
      int nreq = initm >= 0 ? c->scopes[initm].nrequired : 0;
      if (argc != np || nreq != np) continue;  /* only an exact all-required match */
      buf_printf(b, "case %d: _t%d=", ci, rt2);
      if (c->classes[ci].is_value_type)
        buf_printf(b, "sp_box_vobj_%s(sp_%s_new(", c->classes[ci].c_name, c->classes[ci].c_name);
      else
        buf_printf(b, "sp_box_obj(sp_%s_new(", c->classes[ci].c_name);
      Scope *is = initm >= 0 ? &c->scopes[initm] : NULL;
      for (int j = 0; j < np; j++) {
        if (j) buf_puts(b, ", ");
        LocalVar *pp = is ? scope_local(is, is->pnames[j]) : NULL;
        TyKind pt = (pp && pp->type != TY_UNKNOWN) ? pp->type : TY_POLY;
        char tn[24]; snprintf(tn, sizeof tn, "_t%d", atmp[j]);
        Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, pt, tn, &ub);
        buf_puts(b, ub.p ? ub.p : tn); free(ub.p);
      }
      if (c->classes[ci].is_value_type) buf_puts(b, ")); break;");
      else buf_printf(b, "),%d); break;", ci);
    }
    buf_printf(b, "default: sp_raise_nomethod(sp_nomethod_msg(\"new\", _t%d)); } _t%d; })", kt, rt2);
    free(atmp);
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
    /* builtin allocables: the empty value of the class (#2655). CRuby's
       un-allocatable builtins (Integer, Symbol, ...) keep their TypeError
       path by falling through. */
    if (acid < 0) {
      const char *bcn = nt_str(nt, recv, "name");
      if (bcn && sp_streq(bcn, "String")) { buf_puts(b, "sp_str_dup_external((&(\"\\xff\")[1]))"); return; }
      if (bcn && sp_streq(bcn, "Array"))  { buf_puts(b, "sp_PolyArray_new()"); return; }
      if (bcn && sp_streq(bcn, "Hash"))   { buf_puts(b, "sp_PolyPolyHash_new()"); return; }
      if (bcn && sp_streq(bcn, "Object")) { buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT)"); return; }
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
      buf_printf(b, "sp_%s_new(", c->classes[cid].c_name);
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
    /* native (C-backed) class reached as ::Name (root-qualified) */
    if (emit_native_ctor(c, id, ci, argc, argv, b)) return;
    if (ci >= 0) {
      if (class_is_exc_subclass(c, ci)) {
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) {
          /* user initialize: call the generated sp_ClassName_new(args) constructor */
          buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
          emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
          buf_puts(b, ")");
        }
        else {
          /* no user initialize: create directly with first arg as message.
             An ivar-bearing subclass needs its dedicated struct size (#2772). */
          const char *cn2 = class_ruby_name(c, ci); if (!cn2) cn2 = c->classes[ci].name;
          const char *par = exc_builtin_parent(c, ci);
          if (c->classes[ci].nivars > 0)
            buf_printf(b, "((sp_%s *)sp_exc_new_sub_sized(sizeof(sp_%s), \"%s\", ",
                       c->classes[ci].c_name, c->classes[ci].c_name, cn2);
          else
            buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", ", cn2, par);
          if (argc >= 1) {
            if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
            else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
          }
          else buf_puts(b, "(&(\"\\xff\")[1])");
          buf_puts(b, c->classes[ci].nivars > 0 ? "))" : ")");
        }
        return;
      }
      int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
      if (ucnew >= 0) {
        /* user-defined def self.new: call it as a regular class method */
        int defcls2 = -1; comp_cmethod_in_chain(c, ci, "new", &defcls2);
        buf_printf(b, "sp_%s_s_new(", c->classes[defcls2 >= 0 ? defcls2 : ci].c_name);
        emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
      if (!c->classes[ci].is_struct) {
        buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        buf_puts(b, ")");
        return;
      }
    }
    if (cn && is_exc_name(cn)) {
      buf_printf(b, "sp_exc_new(\"%s\", ", cn);
      if (argc >= 1) {
        if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
        else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      }
      else buf_puts(b, "(&(\"\\xff\")[1])");
      buf_puts(b, ")");
      return;
    }
  }

  /* Thread class methods: Thread.current / Thread.pass (recv is the Thread
     constant). Handled before the Class.new dispatch since they are not `new`. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *tcn = nt_str(nt, recv, "name");
    /* Exception class-level: Cls.exception(msg) is Cls.new (#2740);
       Exception.to_tty? reports whether stderr is a terminal (#2757). */
    if (tcn && is_exc_name(tcn)) {
      if (sp_streq(name, "exception")) {
        buf_printf(b, "sp_exc_new(\"%s\", ", tcn);
        if (argc >= 1) {
          if (comp_ntype(c, argv[0]) == TY_STRING) emit_expr(c, argv[0], b);
          else { buf_puts(b, "sp_poly_to_s("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        }
        else buf_puts(b, "(&(\"\\xff\")[1])");
        buf_puts(b, ")");
        return;
      }
      if (sp_streq(name, "to_tty?") && argc == 0) {
        buf_puts(b, "(isatty(2) != 0)"); return;
      }
    }
    /* IO.pipe / IO.copy_stream / IO.sysopen (#2815) */
    if (tcn && sp_streq(tcn, "IO")) {
      if (sp_streq(name, "pipe") && argc == 0) { buf_puts(b, "sp_io_pipe()"); return; }
      if (sp_streq(name, "copy_stream") && argc == 2) {
        buf_puts(b, "sp_io_copy_stream("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_expr(c, argv[1], b); buf_puts(b, ")"); return;
      }
      if (sp_streq(name, "sysopen") && argc >= 1) {
        buf_puts(b, "sp_io_sysopen("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
      }
    }
    /* Signal module queries (#2735) */
    if (tcn && sp_streq(tcn, "Signal") && sp_streq(name, "list") && argc == 0) {
      buf_puts(b, "sp_signal_list()"); return;
    }
    if (tcn && sp_streq(tcn, "Signal") && sp_streq(name, "signame") && argc == 1) {
      buf_puts(b, "sp_signal_signame(");
      emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    /* Process.kill(sig, *pids): per-pid sends, counting successes (#2750) */
    if (tcn && sp_streq(tcn, "Process") && sp_streq(name, "kill") && argc >= 2) {
      g_uses_symbols = 1;   /* :USR1 designators resolve through the sym table */
      int tk5 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = 0;", tk5);
      for (int k = 1; k < argc; k++) {
        buf_printf(b, " _t%d += sp_process_kill1(", tk5);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ", ");
        emit_int_expr(c, argv[k], b);
        buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tk5);
      return;
    }
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

  /* StringIO is a native-bound package class; .open is Ruby in the package. */

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
      /* honor the clock id, and the unit (default :float_second). An integer
         unit yields an Integer; the float units and the default yield a Float. */
      const char *unit = NULL;
      if (argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode"))
        unit = nt_str(nt, argv[1], "value");
      /* an unknown literal unit is CRuby's ArgumentError, not a silent
         float_second (#2727) */
      if (unit && !sp_streq(unit, "nanosecond") && !sp_streq(unit, "microsecond") &&
          !sp_streq(unit, "millisecond") && !sp_streq(unit, "second") &&
          !sp_streq(unit, "float_microsecond") && !sp_streq(unit, "float_millisecond") &&
          !sp_streq(unit, "float_second")) {
        buf_printf(b, "({ sp_raise_cls(\"ArgumentError\", (&(\"\\xff\" \"unexpected unit: %s\")[1])); 0.0; })", unit);
        return;
      }
      buf_puts(b, "(sp_process_clock_ns("); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      if (unit && sp_streq(unit, "nanosecond")) buf_puts(b, ")");
      else if (unit && sp_streq(unit, "microsecond")) buf_puts(b, " / 1000)");
      else if (unit && sp_streq(unit, "millisecond")) buf_puts(b, " / 1000000)");
      else if (unit && sp_streq(unit, "second")) buf_puts(b, " / 1000000000)");
      else if (unit && sp_streq(unit, "float_microsecond")) buf_puts(b, " / 1e3)");
      else if (unit && sp_streq(unit, "float_millisecond")) buf_puts(b, " / 1e6)");
      else buf_puts(b, " / 1e9)");  /* float_second (default) */
      return;
    }
  }

  /* Integer.sqrt(n) -> integer square root (exact, Newton's method) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      sp_streq(name, "sqrt") && argc == 1) {
    if (comp_ntype(c, argv[0]) == TY_BIGINT) {
      buf_puts(b, "sp_bigint_isqrt("); emit_expr(c, argv[0], b); buf_puts(b, ")");  /* (#2420) */
      return;
    }
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
      /* emit_math_arg casts a plain int and coerces a poly value alike -- a
         bare `if (a0t==TY_INT) "(double)"` cast left a poly-typed arg (e.g.
         `Math.sqrt(dx*dx + dy*dy)` over locals that unify to Integer|Float)
         passed straight through as an unconvertible sp_RbVal -- and raises
         TypeError on a nil / String / non-numeric operand. */
      buf_printf(b, "%s(", cfn);
      emit_math_arg(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "lgamma") && argc == 1) {
      /* Math.lgamma(x) -> [log(|gamma(x)|), sign] as a poly array */
      buf_puts(b, "sp_math_lgamma("); emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Math.frexp(x) -> [fraction, exponent] as a poly array */
    if (sp_streq(name, "frexp") && argc == 1) {
      int te = ++g_tmp, tf = ++g_tmp, o = ++g_tmp;
      buf_printf(b, "({ int _t%d; mrb_float _t%d = frexp(", te, tf);
      emit_math_arg(c, argv[0], b);
      buf_printf(b, ", &_t%d); sp_PolyArray *_t%d = sp_PolyArray_new();"
                    " sp_PolyArray_push(_t%d, sp_box_float(_t%d));"
                    " sp_PolyArray_push(_t%d, sp_box_int(_t%d)); _t%d; })",
                 te, o, o, tf, o, te, o);
      return;
    }
    /* Math.log(x) or Math.log(x, base) */
    if (sp_streq(name, "log") && (argc == 1 || argc == 2)) {
      if (argc == 1) {
        buf_puts(b, "sp_math_log(");
        emit_math_arg(c, argv[0], b);
        buf_puts(b, ")");
      }
      else {
        int t0 = ++g_tmp, t1 = ++g_tmp;
        /* the argument value is captured into a scratch buffer so a
           sub-expression that hoists (an array-literal builder for `[..].max`)
           writes to g_pre AHEAD of this declaration rather than inline into
           the middle of it (#2453). */
        Buf a0; memset(&a0, 0, sizeof a0); emit_math_arg(c, argv[0], &a0);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = %s;\n", t0, a0.p ? a0.p : "0"); free(a0.p);
        Buf a1; memset(&a1, 0, sizeof a1); emit_math_arg(c, argv[1], &a1);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "double _t%d = %s;\n", t1, a1.p ? a1.p : "0"); free(a1.p);
        /* log base 1 is NaN in CRuby (the division log(x)/log(1) is x/0 = Inf) */
        buf_printf(b, "(_t%d == 1.0 ? (0.0/0.0) : sp_math_log(_t%d) / sp_math_log(_t%d))", t1, t0, t1);
      }
      return;
    }
    /* Math.log2(x), Math.log10(x) */
    if (sp_streq(name, "log2") && argc == 1) {
      buf_puts(b, "sp_math_log2(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "log10") && argc == 1) {
      buf_puts(b, "sp_math_log10(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Math.atan2(y, x), Math.hypot(x, y), Math.ldexp(x, e) */
    if ((sp_streq(name, "atan2") || sp_streq(name, "hypot")) && argc == 2) {
      buf_printf(b, "%s(", name);
      emit_math_arg(c, argv[0], b); buf_puts(b, ", ");
      emit_math_arg(c, argv[1], b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "ldexp") && argc == 2) {
      /* a Bignum exponent overflows a C long -> RangeError (CRuby), not a
         pointer-to-int cast that silently truncates to Infinity (#2616) */
      if (comp_ntype(c, argv[1]) == TY_BIGINT) {
        buf_puts(b, "((void)("); emit_math_arg(c, argv[0], b); buf_puts(b, "), (void)(");
        emit_expr(c, argv[1], b);
        buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"bignum too big to convert into `long'\"), 0.0))");
        return;
      }
      buf_puts(b, "ldexp(");
      emit_math_arg(c, argv[0], b); buf_puts(b, ", (int)");
      /* the exponent may be a poly array element (`Math.ldexp(f[0], f[1])`):
         emit_int_expr coerces it to mrb_int instead of casting a struct (#2592) */
      emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      return;
    }
  }

  /* JSON.generate(x) / JSON.dump(x) -> serialize a boxed value */
  /* JSON.generate/dump have NO special-case here: they flow through the native
     binding (packages/json -> sp_json_val). A Struct arg serializes via the
     generic sp_obj_to_hash reflection hook (codegen.c), reached from
     sp_json_val, which then serializes the resulting hash. */

  /* Dir.exist? -> directory test; Dir.exists? was removed in Ruby 4.0 (#2780) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (sp_streq(name, "exist?") || sp_streq(name, "exists?")) && argc == 1) {
    if (sp_streq(name, "exists?")) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"NoMethodError\", \"undefined method 'exists?' for class Dir\"); (mrb_bool)0; })");
      return;
    }
    buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return;
  }

  /* File class methods -> runtime helpers (the runtime has long carried
     these; only the dispatch was missing). FileTest shares File's predicate
     surface (#2819). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "File") ||
       sp_streq(nt_str(nt, recv, "name"), "FileTest"))) {
    if ((sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "basename") && argc == 2) {
      buf_puts(b, "sp_file_basename2("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "read") || sp_streq(name, "binread")) && argc == 1) {
      buf_puts(b, "sp_file_read("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* File.read(path, length) -> the first length bytes (#2776) */
    if ((sp_streq(name, "read") || sp_streq(name, "binread")) && argc == 2) {
      buf_puts(b, "sp_file_read_len("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_int_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    /* the stat/predicate family (#2775) */
    if (sp_streq(name, "ftype") && argc == 1) {
      buf_puts(b, "sp_file_ftype("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "writable?") && argc == 1) {
      buf_puts(b, "sp_file_writable("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "executable?") && argc == 1) {
      buf_puts(b, "sp_file_executable("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "size?") && argc == 1) {
      buf_puts(b, "sp_file_size_q("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "pipe?") && argc == 1) {
      buf_puts(b, "sp_file_pipe("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "identical?") && argc == 2) {
      buf_puts(b, "sp_file_identical("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "atime") || sp_streq(name, "ctime")) && argc == 1) {
      buf_printf(b, "sp_file_%s(", name); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "realpath") && argc == 1) {
      buf_puts(b, "sp_file_realpath("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "stat") && argc == 1) {
      buf_puts(b, "sp_file_stat_handle("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* the path-manipulation family (#2774, #2787) */
    if (sp_streq(name, "split") && argc == 1) {
      buf_puts(b, "sp_file_split("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "path") && argc == 1) {
      emit_expr(c, argv[0], b); return;
    }
    if (sp_streq(name, "absolute_path") && (argc == 1 || argc == 2)) {
      buf_puts(b, "sp_file_expand_path("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      if (argc == 2) emit_expr(c, argv[1], b); else buf_puts(b, "(const char *)0");
      buf_puts(b, ")"); return;
    }
    if ((sp_streq(name, "fnmatch") || sp_streq(name, "fnmatch?")) && argc >= 2) {
      buf_puts(b, "sp_file_fnmatch("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "dirname") && argc == 2) {
      /* File.dirname(path, level): apply dirname `level` times (#2787) */
      int td = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", td); emit_expr(c, argv[0], b);
      buf_printf(b, "; mrb_int _tl%d = ", td); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; for (mrb_int _t%d = 0; _t%d < _tl%d; _t%d++) _t%d = sp_file_dirname(_t%d); _t%d; })",
                 ti2, ti2, td, ti2, td, td, td);
      return;
    }
    /* chmod / truncate (#2778) */
    if (sp_streq(name, "chmod") && argc == 2) {
      buf_puts(b, "sp_file_chmod("); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "truncate") && argc == 2) {
      buf_puts(b, "sp_file_truncate("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_int_expr(c, argv[1], b); buf_puts(b, ")"); return;
    }
    /* File.write(path, str, offset) / File.write(path, str, mode: "a") (#2782) */
    if ((sp_streq(name, "write") || sp_streq(name, "binwrite")) && argc == 3) {
      const char *k3 = nt_type(nt, argv[2]);
      if (k3 && sp_streq(k3, "KeywordHashNode")) {
        int mv = struct_kwarg_value(c, argv[2], "mode");
        if (mv >= 0) {
          buf_puts(b, "sp_file_write_mode("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
          emit_str_expr(c, argv[1], b); buf_puts(b, ", ");
          emit_expr(c, mv, b); buf_puts(b, ")");
          return;
        }
      }
      else {
        buf_puts(b, "sp_file_write_at("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_str_expr(c, argv[1], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[2], b); buf_puts(b, ")");
        return;
      }
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
    /* File.exists? was removed in Ruby 4.0: NoMethodError at the call (#2780) */
    if (sp_streq(name, "exists?") && argc == 1) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"NoMethodError\", \"undefined method 'exists?' for class File\"); (mrb_bool)0; })");
      return;
    }
    if ((sp_streq(name, "exist?") || sp_streq(name, "readable?")) && argc == 1) {
      buf_puts(b, "sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "directory?") && argc == 1) {
      buf_puts(b, "sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* zero-length non-directory, not the directory test it aliased to (#2783) */
    if ((sp_streq(name, "zero?") || sp_streq(name, "empty?")) && argc == 1) {
      buf_puts(b, "sp_file_zero("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "symlink?") && argc == 1) {
      buf_puts(b, "sp_file_symlink("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "file?") && argc == 1) {
      buf_puts(b, "(!sp_file_directory("); emit_expr(c, argv[0], b); buf_puts(b, ") && sp_file_exist("); emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
    }
    if ((sp_streq(name, "delete") || sp_streq(name, "unlink")) && argc >= 1) {
      buf_puts(b, "({ ");
      for (int k = 0; k < argc; k++) {
        buf_puts(b, "sp_file_delete("); emit_expr(c, argv[k], b); buf_puts(b, "); ");
      }
      buf_printf(b, "(mrb_int)%d; })", argc); return;
    }
    if (sp_streq(name, "rename") && argc == 2) {
      buf_puts(b, "({ sp_file_rename("); emit_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_expr(c, argv[1], b); buf_puts(b, "); (mrb_int)0; })"); return;
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
      int has_dyn = 0;
      for (int k = 0; k < argc; k++) {
        TyKind jt = comp_ntype(c, argv[k]);
        if (ty_is_array(jt) || jt == TY_POLY_ARRAY || jt == TY_POLY) has_dyn = 1;
      }
      if (has_dyn) {
        /* an Array component flattens into the path (#2786); box everything
           and let the runtime walk it */
        buf_printf(b, "sp_file_join_vals((sp_RbVal[]){");
        for (int k = 0; k < argc; k++) { if (k) buf_puts(b, ", "); emit_boxed(c, argv[k], b); }
        if (argc == 0) buf_puts(b, "sp_box_nil()");
        buf_printf(b, "}, %d)", argc); return;
      }
      /* each component initializes a `const char *` slot, so a poly arg (e.g.
         doom's `File.join(Dir.tmpdir, ...)` where the first component stays
         poly) must be unboxed via sp_poly_to_s, not land its sp_RbVal raw. */
      buf_printf(b, "sp_file_join((const char*[]){");
      for (int k = 0; k < argc; k++) { if (k) buf_puts(b, ", "); emit_str_expr(c, argv[k], b); }
      if (argc == 0) buf_puts(b, "(const char*)0");
      buf_printf(b, "}, %d)", argc); return;
    }
    if (sp_streq(name, "readlines") && argc >= 1) {
      /* File.readlines(path[, sep][, chomp: true]) (#2820) */
      int chomp = 0, csep = -1;
      for (int ki = 1; ki < argc; ki++) {
        const char *kty = nt_type(nt, argv[ki]);
        if (kty && sp_streq(kty, "KeywordHashNode")) {
          int cv = struct_kwarg_value(c, argv[ki], "chomp");
          if (cv >= 0 && nt_type(nt, cv) && sp_streq(nt_type(nt, cv), "TrueNode"))
            chomp = 1;
        }
        else csep = argv[ki];
      }
      if (csep >= 0) {
        buf_puts(b, "sp_file_readlines_sep(");
        emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_expr(c, csep, b);
        buf_printf(b, ", %d)", chomp);
        return;
      }
      if (chomp) buf_puts(b, "sp_file_readlines_chomp(");
      else buf_puts(b, "sp_file_readlines(");
      emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    /* File.open(path, mode) / File.new(path, mode) without block -> TY_IO
       handle. The mode may be a string, an integer flag word (#2788), or a
       trailing `mode:` keyword (#2789). */
    if (sp_streq(name, "open") || sp_streq(name, "new")) {
      int block = nt_ref(nt, id, "block");
      int kw_mode = -1;
      if (argc >= 2 && nt_type(nt, argv[argc - 1]) &&
          sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode"))
        kw_mode = struct_kwarg_value(c, argv[argc - 1], "mode");
      int int_mode = argc >= 2 && kw_mode < 0 && comp_ntype(c, argv[1]) == TY_INT;
      #define EMIT_FILE_OPEN() do { \
        if (int_mode) { \
          buf_puts(b, "sp_File_open_flags("); emit_expr(c, argv[0], b); buf_puts(b, ", "); \
          emit_int_expr(c, argv[1], b); buf_puts(b, ")"); \
        } \
        else { \
          buf_puts(b, "sp_File_open("); emit_expr(c, argv[0], b); buf_puts(b, ", "); \
          if (kw_mode >= 0) emit_expr(c, kw_mode, b); \
          else if (argc >= 2 && !(nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode"))) emit_expr(c, argv[1], b); \
          else buf_puts(b, "\"r\""); \
          buf_puts(b, ")"); \
        } \
      } while (0)
      if (block < 0) {
        EMIT_FILE_OPEN();
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
      buf_printf(b, "sp_File *_t%d = ", tf); EMIT_FILE_OPEN(); buf_puts(b, "; ");
      #undef EMIT_FILE_OPEN
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
    /* Dir.new / Dir.open -> a directory handle; the block form closes on
       exit and returns the block's value (#2821) */
    if ((sp_streq(name, "new") || sp_streq(name, "open")) && argc >= 1) {
      int dblk = nt_ref(nt, id, "block");
      if (dblk < 0) {
        buf_puts(b, "sp_Dir_new("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return;
      }
      const char *dp0 = block_param_name(c, dblk, 0);
      const char *dpn = dp0 ? rename_local(dp0) : NULL;
      int dbody = nt_ref(nt, dblk, "body");
      int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
      TyKind dres = comp_ntype(c, id);
      int dscalar = is_scalar_ret(dres) && dres != TY_VOID && dres != TY_NIL && dres != TY_UNKNOWN;
      int td = ++g_tmp, tdv = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_Dir *_t%d = sp_Dir_new(", td); emit_expr(c, argv[0], b); buf_puts(b, "); ");
      buf_printf(b, "SP_GC_ROOT(_t%d); ", td);
      if (dpn) buf_printf(b, "sp_Dir *lv_%s = _t%d; ", dpn, td);
      for (int k = 0; k < dbn - 1; k++) emit_stmt(c, dbb[k], b, 0);
      if (dbn > 0 && dscalar) {
        emit_ctype(c, dres, b); buf_printf(b, " _t%d = ", tdv);
        if (dres == TY_POLY && comp_ntype(c, dbb[dbn - 1]) != TY_POLY) emit_boxed(c, dbb[dbn - 1], b);
        else emit_expr(c, dbb[dbn - 1], b);
        buf_puts(b, "; ");
      }
      else if (dbn > 0) emit_stmt(c, dbb[dbn - 1], b, 0);
      buf_printf(b, "sp_Dir_close(_t%d); ", td);
      if (dscalar && dbn > 0) buf_printf(b, "_t%d; })", tdv);
      else buf_puts(b, "0; })");
      return;
    }
    if (sp_streq(name, "pwd") && argc == 0) { buf_puts(b, "sp_dir_pwd()"); return; }
    if (sp_streq(name, "home") && argc == 0) { buf_puts(b, "sp_dir_home()"); return; }
    if (sp_streq(name, "empty?") && argc == 1) {
      buf_puts(b, "sp_dir_empty("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "home") && argc == 1) {
      buf_puts(b, "sp_dir_home_user("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "glob") && argc == 2 && nt_type(nt, argv[1]) &&
        sp_streq(nt_type(nt, argv[1]), "ConstantPathNode") &&
        nt_str(nt, argv[1], "name") && sp_streq(nt_str(nt, argv[1], "name"), "FNM_DOTMATCH")) {
      buf_puts(b, "sp_dir_glob_dot("); emit_expr(c, argv[0], b); buf_puts(b, ")"); return;
    }
    if (sp_streq(name, "glob") && argc == 1 && ty_is_array(comp_ntype(c, argv[0]))) {
      buf_puts(b, "sp_dir_glob_multi("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return;
    }
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
      if (at == TY_TIME) { emit_expr(c, argv[0], b); return; }  /* value copy */
      if (at == TY_RATIONAL) {
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = ", tr);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_at_div(_t%d.num, _t%d.den); })", tr, tr);
        return;
      }
      /* a non-numeric argument raises CRuby's TypeError; the expression
         still needs the arm's sp_Time type for downstream emitters */
      const char *atc = at == TY_STRING ? "String" : at == TY_SYMBOL ? "Symbol" :
                        at == TY_NIL ? "NilClass" : NULL;
      if (atc) {
        buf_printf(b, "({ sp_raise_cls(\"TypeError\", "
                      "\"can't convert %s into an exact number\"); (sp_Time){0, 0, 0}; })", atc);
        return;
      }
      /* a poly argument (`Time.at(x)` where x is a heterogeneous/nullable
         numeric) reaches sp_time_at_int's mrb_int slot: coerce it. A boxed
         float is understood by sp_poly_to_f, so route poly through the float
         ctor (which also accepts an integral value). */
      if (at == TY_POLY) {
        buf_puts(b, "sp_time_at_float(sp_poly_to_f("); emit_expr(c, argv[0], b);
        buf_puts(b, "))");
        return;
      }
      buf_printf(b, "sp_time_at_%s(", at == TY_FLOAT ? "float" : "int");
      emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Time.at(sec, in: "+HH:MM"): the epoch instant carried with a fixed UTC
       offset (is_utc == 2), so #utc_offset returns it. (#2681) */
    if (sp_streq(name, "at") && argc == 2 &&
        nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode")) {
      int inv = struct_kwarg_value(c, argv[1], "in");
      const char *off = (inv >= 0 && nt_type(nt, inv) && sp_streq(nt_type(nt, inv), "StringNode"))
                        ? nt_str(nt, inv, "content") : NULL;
      if (off && strlen(off) == 6 && (off[0] == '+' || off[0] == '-') && off[3] == ':' &&
          off[1] >= '0' && off[1] <= '9' && off[2] >= '0' && off[2] <= '9' &&
          off[4] >= '0' && off[4] <= '9' && off[5] >= '0' && off[5] <= '9') {
        long osec = ((off[1] - '0') * 10 + (off[2] - '0')) * 3600 + ((off[4] - '0') * 10 + (off[5] - '0')) * 60;
        if (off[0] == '-') osec = -osec;
        int ts = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = sp_time_at_int(", ts); emit_int_expr(c, argv[0], b);
        buf_printf(b, "); _t%d.is_utc = 2; _t%d.utc_off = %ld; _t%d; })", ts, ts, osec, ts);
        return;
      }
    }
    /* Time.at(sec, num, :unit): the third argument names the second one's
       unit -- :millisecond / :usec / :microsecond / :nanosecond (and their
       plurals/aliases), scaled to tv_nsec. Only a literal symbol resolves the
       scale at compile time; anything else falls through. (#2714) */
    if (sp_streq(name, "at") && argc == 3 &&
        nt_type(nt, argv[2]) && sp_streq(nt_type(nt, argv[2]), "SymbolNode")) {
      const char *un = nt_str(nt, argv[2], "value");
      long mult = 0;
      /* CRuby's exact unit set -- no plurals; an unknown symbol is its
         runtime ArgumentError */
      if (un && sp_streq(un, "millisecond")) mult = 1000000;
      else if (un && (sp_streq(un, "usec") || sp_streq(un, "microsecond"))) mult = 1000;
      else if (un && (sp_streq(un, "nsec") || sp_streq(un, "nanosecond"))) mult = 1;
      else if (un) {
        buf_printf(b, "({ sp_raise_cls(\"ArgumentError\", \"unexpected unit: %s\"); (sp_Time){0}; })", un);
        return;
      }
      if (mult > 0) {
        TyKind st = comp_ntype(c, argv[0]);
        int ts = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = ", ts);
        if (st == TY_FLOAT) { buf_puts(b, "sp_time_at_float("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
        buf_printf(b, "; _t%d.tv_nsec += (int32_t)(", ts);
        TyKind ut = comp_ntype(c, argv[1]);
        if (ut == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_printf(b, ") * %ld.0", mult); }
        else if (ut == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[1], b); buf_printf(b, ") * %ld.0", mult); }
        else { buf_puts(b, "("); emit_int_expr(c, argv[1], b); buf_printf(b, ") * %ld", mult); }
        buf_printf(b, "); _t%d; })", ts);
        return;
      }
    }
    /* Time.at(sec, usec): the second positional argument is microseconds
       (no unit keyword). tv_nsec = usec * 1000. (#2646) */
    if (sp_streq(name, "at") && argc == 2 &&
        !(nt_type(nt, argv[1]) && (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
                                   sp_streq(nt_type(nt, argv[1]), "HashNode")))) {
      TyKind st = comp_ntype(c, argv[0]);
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = ", ts);
      if (st == TY_FLOAT) { buf_puts(b, "sp_time_at_float("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (st == TY_POLY || st == TY_UNKNOWN) { buf_puts(b, "sp_time_at_float(sp_poly_to_f("); emit_boxed(c, argv[0], b); buf_puts(b, "))"); }
      else { buf_puts(b, "sp_time_at_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "; _t%d.tv_nsec += (int32_t)(", ts);
      TyKind ut = comp_ntype(c, argv[1]);
      if (ut == TY_FLOAT) { emit_expr(c, argv[1], b); buf_puts(b, " * 1000.0"); }
      else if (ut == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[1], b); buf_puts(b, ") * 1000.0"); }
      else { buf_puts(b, "("); emit_int_expr(c, argv[1], b); buf_puts(b, ") * 1000"); }
      buf_printf(b, "); _t%d; })", ts);
      return;
    }
    if ((sp_streq(name, "local") || sp_streq(name, "mktime") ||
         sp_streq(name, "utc") || sp_streq(name, "gm") || sp_streq(name, "new")) && argc >= 1) {
      int is_utc = (sp_streq(name, "utc") || sp_streq(name, "gm"));
      if (emit_time_civil_ctor(c, id, is_utc, sp_streq(name, "new"), b)) return;
      unsupported(c, id, "Time constructor argument form");
      return;
    }
  }

  /* native binding dispatch (Path B): Module.func(...) where Module declared
     native_func. Emit a direct C call to the declared symbol, passing each arg
     in its runtime representation (any -> boxed sp_RbVal, string -> sp_Str*,
     int/float/bool -> the scalar). No FFI boxing. */
  if (recv >= 0) {
    const char *rty_nv = nt_type(nt, recv);
    const char *nvmod = NULL;
    if (rty_nv && (sp_streq(rty_nv, "ConstantReadNode") || sp_streq(rty_nv, "ConstantPathNode")))
      nvmod = nt_str(nt, recv, "name");
    int nvi = nvmod ? comp_native_find(c, nvmod, name) : -1;
    if (nvi >= 0) {
      const char *feat = c->native_funcs[nvi].feat;
      if (!feat || !feat[0] || sp_feature_enabled(feat)) {
        NativeFunc *nf = &c->native_funcs[nvi];
        /* a `cstring` return is a borrowed C string (typically the callee's
           static buffer, e.g. sp_crypto's per-function buffers): the next call
           to the same symbol clobbers it, so dup onto the GC string heap
           before the value escapes into Ruby. */
        int nv_cstr_ret = sp_streq(nf->ret, "cstring");
        /* JSON.parse's symbolize_names: the option rides a keyword hash the
           1-arg native signature would silently drop. Route the parse
           through the deep key-symbolizer: a literal true wraps directly, a
           dynamic value decides at runtime over a single parse. Other
           options keep the prior ignored-with-string-keys behavior. */
        if (sp_streq(nvmod, "JSON") && sp_streq(name, "parse") && argc == 2 &&
            nt_type(nt, argv[1]) &&
            (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
             sp_streq(nt_type(nt, argv[1]), "HashNode"))) {
          int symv = struct_kwarg_value(c, argv[1], "symbolize_names");
          if (symv >= 0) {
            const char *svty = nt_type(nt, symv);
            int lit_true  = svty && sp_streq(svty, "TrueNode");
            int lit_false = svty && (sp_streq(svty, "FalseNode") || sp_streq(svty, "NilNode"));
            if (lit_true) { buf_printf(b, "sp_json_symbolize(%s(", nf->csym); emit_expr(c, argv[0], b); buf_puts(b, "))"); return; }
            if (!lit_false) {
              int tj = ++g_tmp;
              buf_printf(b, "({ sp_RbVal _t%d = %s(", tj, nf->csym); emit_expr(c, argv[0], b);
              buf_puts(b, "); (");
              emit_cond(c, symv, b);
              buf_printf(b, ") ? sp_json_symbolize(_t%d) : _t%d; })", tj, tj);
              return;
            }
            /* literal false/nil: fall through to the plain 1-arg call */
          }
        }
        if (nv_cstr_ret) buf_puts(b, "sp_str_dup_external(");
        buf_puts(b, nf->csym); buf_puts(b, "(");
        for (int ai = 0; ai < nf->nargs && ai < argc; ai++) {
          if (ai) buf_puts(b, ", ");
          const char *spec = nf->args[ai];
          TyKind at = comp_ntype(c, argv[ai]);
          if (sp_streq(spec, "any")) emit_boxed(c, argv[ai], b);
          else if (sp_streq(spec, "string")) {
            if (at == TY_POLY) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
            else emit_expr(c, argv[ai], b);
          }
          else if (sp_streq(spec, "int") && at == TY_POLY) {
            buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
          }
          else if (sp_streq(spec, "float") && at == TY_POLY) {
            buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
          }
          else if (sp_streq(spec, "bool") && at == TY_POLY) {
            buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[ai], b); buf_puts(b, ")");
          }
          else emit_expr(c, argv[ai], b);
        }
        buf_puts(b, ")");
        if (nv_cstr_ret) buf_puts(b, ")");
        return;
      }
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
        /* A function taking an ffi_callback has its extern skipped (codegen.c):
           the symbol is declared by a system header whose per-argument const
           qualification we can't reproduce, so we call the header prototype
           directly. That means pointer-data args carry our own (const) element
           types; cast them to void* at the call site so the implicit conversion
           to the header's real parameter type is warning-free under -Werror on
           both clang and gcc. An explicit (void*) cast also legally drops the
           const our array/string types carry. */
        int hdr_call = 0;
        for (int hi = 0; hi < call_argc; hi++)
          if (ffi_find_callback(c, rcmod, c->ffi_funcs[fi].args[hi]) >= 0) { hdr_call = 1; break; }
        /* A trailing :varargs spec: the declared specs cover only the fixed
           leading args; every extra actual arg is passed through with C's
           default argument promotions. No extern is emitted for a variadic
           function (see codegen.c); the call casts the header-declared symbol
           to a variadic function pointer -- `((ret (*)(fixed..., ...))name)` --
           which cannot conflict with a fortified libc declaration and carries
           no `format` attribute, so gcc does not format-check the call. */
        int is_vararg = call_argc > 0 && sp_streq(c->ffi_funcs[fi].args[call_argc - 1], "varargs");
        int fixed_argc = is_vararg ? call_argc - 1 : call_argc;
        /* Build the raw C call */
        Buf call_buf; memset(&call_buf, 0, sizeof call_buf);
        if (is_vararg) {
          buf_printf(&call_buf, "((%s (*)(", ffi_c_type(ret_spec));
          for (int ai = 0; ai < fixed_argc; ai++) {
            if (ai) buf_puts(&call_buf, ", ");
            buf_puts(&call_buf, ffi_c_type(c->ffi_funcs[fi].args[ai]));
          }
          if (fixed_argc) buf_puts(&call_buf, ", ");
          buf_printf(&call_buf, "...))%s)", c->ffi_funcs[fi].csym ? c->ffi_funcs[fi].csym : c->ffi_funcs[fi].name);
        }
        else buf_puts(&call_buf, c->ffi_funcs[fi].csym ? c->ffi_funcs[fi].csym : c->ffi_funcs[fi].name);
        buf_puts(&call_buf, "(");
        for (int ai = 0; ai < fixed_argc && ai < argc; ai++) {
          if (ai) buf_puts(&call_buf, ", ");
          const char *spec = c->ffi_funcs[fi].args[ai];
          TyKind at = comp_ntype(c, argv[ai]);
          int cbidx = ffi_find_callback(c, rcmod, spec);
          if (cbidx >= 0) { emit_ffi_callback_arg(c, cbidx, argv[ai], &call_buf); continue; }
          /* :ptr already emits a void*; str/int_array/float_array carry a const
             element pointer that must be genericized for the header call. */
          int voidp = hdr_call && (sp_streq(spec, "str") ||
                                   sp_streq(spec, "int_array") ||
                                   sp_streq(spec, "float_array"));
          if (voidp) buf_puts(&call_buf, "(void *)(");
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
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_ffi_int_array_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else                           { buf_puts(&call_buf, "((const int64_t *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (sp_streq(spec, "float_array")) {
            if (at == TY_FLOAT_ARRAY)      { buf_puts(&call_buf, "sp_FloatArray_ffi_data(");  emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_float_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_ffi_float_array_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
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
          if (voidp) buf_puts(&call_buf, ")");
        }
        /* Extra variadic args: promote by inferred type (int->long long,
           float->double, str->const char*, ptr->void*). A poly-typed vararg
           has no compile-time C type to promote to, so reject it loudly. */
        if (is_vararg) {
          for (int ai = fixed_argc; ai < argc; ai++) {
            if (ai) buf_puts(&call_buf, ", ");
            TyKind at = comp_ntype(c, argv[ai]);
            if (at == TY_INT || at == TY_BOOL) {
              buf_puts(&call_buf, "((long long)("); emit_int_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
            else if (at == TY_FLOAT) {
              buf_puts(&call_buf, "((double)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
            else if (at == TY_STRING) {
              emit_expr(c, argv[ai], &call_buf);
            }
            else {
              /* A poly (or otherwise non-scalar) vararg has no compile-time C
                 type -- int vs string is indistinguishable in a C varargs call,
                 so promoting it would be a silent-wrong. Reject loudly. */
              free(call_buf.p);
              unsupported(c, argv[ai], "ffi variadic argument (needs a concrete int/float/str type)");
              return;
            }
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
              /* unbox a poly buffer to its void*; a non-poly arg (e.g. a
                 pointer passed as mrb_int through a callback param) is cast
                 directly -- close its wrapping paren the same way .v.p does. */
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
              buf_printf(b, " + %d)); sp_box_foreign_ptr(_t%d); })", off, rt3);
            }
            else {
              /* `+ off` must apply to the char* (byte offset), not the typed
                 pointer (which would scale it by sizeof(elem)). */
              buf_printf(b, "((mrb_int)(*(%s *)((char *)(", ctype);
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
              buf_printf(b, " + %d)))", off);
            }
          }
          return;
        }
      }
      /* ffi_struct accessors: Module.Name_new (alloc, boxed ptr),
         Module.Name_get_<f>(ptr) (read field, boxed by type),
         Module.Name_set_<f>(ptr, val) (write field, returns nil). */
      {
        int fsi, ffi;
        int fsm = ffi_struct_method(c, rcmod, name, &fsi, &ffi);
        if (fsm == FFI_SM_NEW) {
          buf_printf(b, "sp_box_foreign_ptr(calloc(1, sizeof(sp_ffi_struct_%s_%s)))",
                     c->ffi_structs[fsi].mod, c->ffi_structs[fsi].name);
          return;
        }
        if (fsm == FFI_SM_GET && argc >= 1) {
          const char *sm2 = c->ffi_structs[fsi].mod, *sn2 = c->ffi_structs[fsi].name;
          const char *spec = c->ffi_structs[fsi].fields[ffi].spec;
          const char *fname = c->ffi_structs[fsi].fields[ffi].name;
          TyKind rt2 = ffi_spec_to_ty(spec);
          buf_puts(b, rt2 == TY_POLY ? "sp_box_foreign_ptr((void *)("
                    : rt2 == TY_STRING ? "((const char *)("
                    : rt2 == TY_FLOAT ? "((mrb_float)(" : "((mrb_int)(");
          buf_printf(b, "((sp_ffi_struct_%s_%s *)", sm2, sn2);
          if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
          else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          buf_printf(b, ")->%s))", fname);
          return;
        }
        if (fsm == FFI_SM_SET && argc >= 2) {
          const char *sm2 = c->ffi_structs[fsi].mod, *sn2 = c->ffi_structs[fsi].name;
          const char *spec = c->ffi_structs[fsi].fields[ffi].spec;
          const char *fname = c->ffi_structs[fsi].fields[ffi].name;
          TyKind rt2 = ffi_spec_to_ty(spec);
          buf_printf(b, "(((sp_ffi_struct_%s_%s *)", sm2, sn2);
          if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
          else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          buf_printf(b, ")->%s = (%s)", fname, ffi_c_type(spec));
          if (rt2 == TY_POLY) {
            if (comp_ntype(c, argv[1]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_puts(b, ").v.p"); }
            else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
          }
          else if (rt2 == TY_STRING) { buf_puts(b, "("); emit_str_expr(c, argv[1], b); buf_puts(b, ")"); }
          else if (rt2 == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
          else { buf_puts(b, "("); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
          buf_puts(b, ", sp_box_nil())");
          return;
        }
      }
      /* ffi_write_* access: Module.writer_name(buf, val) stores val at
         `offset` bytes into buf and returns the written value. Symmetric to
         the reader branch above; the `+ off` applies to the char* byte view. */
      {
        int wi = -1;
        for (int fwi = 0; fwi < c->n_ffi_writers; fwi++)
          if (sp_streq(c->ffi_writers[fwi].mod, rcmod) && sp_streq(c->ffi_writers[fwi].name, name)) {
            wi = fwi; break;
          }
        if (wi >= 0 && argc >= 2) {
          const char *kind = c->ffi_writers[wi].kind;
          int off = c->ffi_writers[wi].offset;
          int tv = ++g_tmp;
          if (kind && sp_streq(kind, "ptr")) {
            TyKind vt = comp_ntype(c, argv[1]);
            buf_printf(b, "({ void *_t%d = ", tv);
            if (vt == TY_POLY) { buf_puts(b, "(void *)("); emit_expr(c, argv[1], b); buf_puts(b, ").v.p"); }
            else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
            buf_puts(b, "; *(void **)((char *)(");
            TyKind bt = comp_ntype(c, argv[0]);
            if (bt == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
            else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            buf_printf(b, " + %d) = _t%d; sp_box_foreign_ptr(_t%d); })", off, tv, tv);
          }
          else {
            const char *ctype = (kind && sp_streq(kind, "i32")) ? "int32_t" : "uint32_t";
            buf_printf(b, "({ %s _t%d = (%s)(", ctype, tv, ctype);
            emit_int_expr(c, argv[1], b);
            buf_printf(b, "); *(%s *)((char *)(", ctype);
            TyKind bt = comp_ntype(c, argv[0]);
            if (bt == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
            else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            buf_printf(b, " + %d) = _t%d; (mrb_int)_t%d; })", off, tv, tv);
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
        int _arc = ty_object_class(_art), _adefc = -1, _awmdc = -1;
        /* attr writer -> field write, UNLESS an explicit `def x=` overrides it
           at an equal-or-more-derived class; then fall through to dispatch.
           CRuby: attr_accessor defines an ordinary writer method. */
        int _awins = comp_writer_in_chain(c, _arc, _abase, &_adefc);
        if (_awins && comp_method_in_chain(c, _arc, name, &_awmdc) >= 0) {
          for (int k = _arc; k >= 0; k = c->classes[k].parent) {
            if (k == _awmdc) { _awins = 0; break; }
            if (k == _adefc) { _awins = 1; break; }
          }
        }
        if (_awins) {
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
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
        emit_cmethod_block_arg(c, id, &c->scopes[mi], -1, b);
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
        /* A literal block at the call site is lowered to one sp_Proc * temp
           shared by every candidate branch (lowering it per-branch would
           emit the proc function once per candidate). */
        int blk_tmp = -1;
        int casc_blk = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
        if (casc_blk >= 0) {
          for (int k = 0; k < ncand && blk_tmp < 0; k++) {
            int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
            if (mi < 0) continue;
            Scope *cm = &c->scopes[mi];
            if (cm->blk_param && cm->blk_param[0] && !cm->yields) {
              blk_tmp = ++g_tmp;
              Buf pb; memset(&pb, 0, sizeof pb);
              emit_proc_literal(c, casc_blk, &pb);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", blk_tmp);
              free(pb.p);
            }
          }
        }
        int tcid = ++g_tmp;
        buf_printf(b, "({ int _t%d = (", tcid); emit_expr(c, recv, b); buf_puts(b, ").cls_id; ");
        if (void_res) {
          for (int k = 0; k < ncand; k++) {
            int defcls = -1;
            int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
            if (mi < 0) continue;
            buf_printf(b, "if (_t%d == %d) sp_%s_s_%s(", tcid, cand[k], c->classes[defcls].c_name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, b);
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
            buf_printf(&cb, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", &cb);
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, &cb);
            buf_puts(&cb, ")");
            emit_boxed_text(c, c->scopes[mi].ret, cb.p ? cb.p : "0", b);
            free(cb.p);
          }
          else {
            buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
            emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, b);
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
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
        /* Pass &block as sp_Proc * when the class method keeps a real &blk
           param and isn't yield-inlined -- the instance-method and bare-call
           paths already do this; a module/class-method call must too. */
        emit_cmethod_block_arg(c, id, &c->scopes[mi], -1, b);
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
    /* ~ /re/ -> `/re/ =~ $_`: the match offset in the last-read line, or nil. */
    if (rre >= 0 && sp_streq(name, "~") && argc == 0) {
      const char *urn = comp_resolve_gvar(c, "_");
      LocalVar *ugv = urn ? comp_gvar(c, urn) : NULL;
      if (ugv) {
        if (ugv->type == TY_STRING) buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, gv_%s)", rre, urn);
        else buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, sp_poly_to_s(gv_%s))", rre, urn);
      } else {
        buf_puts(b, "sp_box_nil()");  /* $_ unset: no line to match against */
      }
      return;
    }
    /* /re/.source and /re/.options are compile-time constants of the literal.
       The source/flags come from the RESOLVED literal's registration slot
       (g_re_src) -- re_lit_index also resolves variables and constants, whose
       own nodes carry no "unescaped" (a variable read fell to an empty
       string, #2018). */
    if (rre >= 0 && sp_streq(name, "source") && argc == 0) {
      emit_str_literal(b, g_re_src[rre]); return;
    }
    if (rre >= 0 && sp_streq(name, "options") && argc == 0) {
      int pf = g_re_flg[rre];
      int opt = ((pf & 1) ? 1 : 0) | ((pf & 8) ? 2 : 0) | ((pf & 4) ? 4 : 0);
      buf_printf(b, "%d", opt); return;
    }
    if (rre >= 0 && sp_streq(name, "encoding") && argc == 0) {
      int ascii = re_src_all_ascii(g_re_src[rre]);
      buf_printf(b, "sp_box_encoding(%s)", ascii ? "sp_encoding_us_ascii()" : "sp_encoding_utf8()");
      return;
    }
    if (rre >= 0 && sp_streq(name, "fixed_encoding?") && argc == 0) {
      buf_puts(b, re_src_all_ascii(g_re_src[rre]) ? "FALSE" : "TRUE");
      return;
    }
  }
  /* Regexp VALUE receiver (a variable, or a literal in value position):
     rendering reads the pattern's retained source text at runtime. */
  /* Regexp#== / #eql? compare by pattern source (dup == original) (#2361) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 1 &&
      (sp_streq(name, "==") || sp_streq(name, "!=")) &&
      comp_ntype(c, argv[0]) == TY_REGEX) {
    buf_puts(b, sp_streq(name, "!=") ? "(!(" : "((");
    buf_puts(b, "strcmp(sp_re_source((void *)(");
    emit_expr(c, recv, b);
    buf_puts(b, ")), sp_re_source((void *)(");
    emit_expr(c, argv[0], b);
    buf_puts(b, sp_streq(name, "!=") ? "))) == 0))" : "))) == 0))");
    return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
      comp_ntype(c, argv[0]) == TY_REGEX) {
    buf_puts(b, "((void *)("); emit_expr(c, recv, b); buf_puts(b, ") == (void *)(");
    emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
  }
  /* IO handles compare by pointer identity (f.flush.equal?(f), #2799) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_IO && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) &&
      comp_ntype(c, argv[0]) == TY_IO) {
    buf_puts(b, "((void *)("); emit_expr(c, recv, b); buf_puts(b, ") == (void *)(");
    emit_expr(c, argv[0], b); buf_puts(b, "))"); return;
  }
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 0) {
    /* a Regexp is frozen; freeze/itself/dup evaluate to the pattern itself. */
    if (sp_streq(name, "frozen?")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return; }
    if (sp_streq(name, "freeze") || sp_streq(name, "itself") || sp_streq(name, "dup") || sp_streq(name, "clone")) {
      emit_expr(c, recv, b); return;
    }
    if (sp_streq(name, "source")) {
      buf_puts(b, "sp_re_source((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "inspect")) {
      buf_puts(b, "sp_re_inspect_str((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "to_s")) {
      buf_puts(b, "sp_re_to_s_str((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "names")) {
      buf_puts(b, "sp_Regexp_names((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "options")) {
      buf_puts(b, "sp_re_options((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "casefold?")) {
      buf_puts(b, "sp_re_casefold_p((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    /* spinel does not enforce a match timeout; a Regexp's per-instance timeout
       is unset (nil), matching the default a pattern is compiled with. */
    if (sp_streq(name, "timeout")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())");
      return;
    }
    if (sp_streq(name, "named_captures")) {
      /* {name => [group indices]}, built inline: sp_StrPolyHash is per-TU
         static, so the hash must be constructed by the generated TU itself */
      int tp = ++g_tmp, th = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ const void *_t%d = (const void *)(", tp); emit_expr(c, recv, b);
      buf_printf(b, "); sp_StrPolyHash *_t%d = sp_StrPolyHash_new(); SP_GC_ROOT(_t%d);"
                    " int _n%d = re_num_named((const mrb_regexp_pattern *)_t%d);"
                    " for (int _t%d = 0; _t%d < _n%d; _t%d++) {"
                    " int _g%d = 0; const char *_nm%d = re_named_name((const mrb_regexp_pattern *)_t%d, _t%d, &_g%d);"
                    " if (_nm%d) {"
                    " sp_RbVal _cur%d = sp_StrPolyHash_get(_t%d, _nm%d); sp_IntArray *_ia%d;"
                    " if (_cur%d.tag == SP_TAG_NIL) { _ia%d = sp_IntArray_new();"
                    " sp_StrPolyHash_set(_t%d, sp_str_dup(_nm%d), sp_box_int_array(_ia%d)); }"
                    " else _ia%d = (sp_IntArray *)_cur%d.v.p;"
                    " sp_IntArray_push(_ia%d, _g%d); } } _t%d; })",
                 th, th,
                 ti, tp,
                 ti, ti, ti, ti,
                 ti, ti, tp, ti, ti,
                 ti,
                 ti, th, ti, ti,
                 ti, ti,
                 th, ti, ti,
                 ti, ti,
                 ti, ti, th);
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
  /* str.gsub(/re/) with no block/replacement -> an Enumerator over the
     matches (the same items scan yields). */
  if (recv >= 0 && rt == TY_STRING && argc == 1 && sp_streq(name, "gsub") &&
      nt_ref(nt, id, "block") < 0 && re_lit_index(c, argv[0]) >= 0) {
    int gre = re_lit_index(c, argv[0]);
    int tsg = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsg);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); "
                  "sp_enum_with_src(sp_Enumerator_new_from(sp_box_str_array(sp_re_scan(sp_re_pat_%d, _t%d))), "
                  "sp_box_str(_t%d), \"gsub\"); })", tsg, gre, tsg, tsg);
    return;
  }
  /* Object receivers (incl. native-bound classes like StringScanner) dispatch
     their own match?/match methods; only string-ish receivers belong here. */
  if (recv >= 0 && argc >= 1 && rt != TY_SYMBOL && rt != TY_NIL && !ty_is_object(rt) &&
      (sp_streq(name, "match?") || sp_streq(name, "!~") || sp_streq(name, "=~") || sp_streq(name, "match"))) {
    int are = re_lit_index(c, argv[0]);
    /* a numeric receiver has no =~/!~/match?/match (Object#=~ was removed):
       raise NoMethodError rather than matching the number as a string. */
    if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) &&
        (sp_streq(name, "=~") || sp_streq(name, "!~") ||
         sp_streq(name, "match?") || sp_streq(name, "match"))) {
      const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
      const char *dv9 = default_value(comp_ntype(c, id));
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of %s\"), %s))",
                 name, tn9, dv9 ? dv9 : "sp_box_nil()");
      return;
    }
    if (are >= 0 && sp_streq(name, "=~") && rt == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    /* poly receiver `poly =~ /re/`: String#=~ when it holds a string at runtime
       (e.g. an element read out of an array that widened to poly), nil when it
       holds nil (NilClass#=~ is always nil); any other tag has no =~ (Object#=~
       was removed) -> NoMethodError, matching CRuby. */
    if (are >= 0 && sp_streq(name, "=~") && rt == TY_POLY) {
      int tv = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
      buf_printf(b, "(_t%d.tag == SP_TAG_STR ? sp_re_match_poly(sp_re_pat_%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? sp_box_nil()"
                    " : sp_raise_nomethod(\"undefined method '=~' for poly\"))",
                 tv, are, tv, tv);
      return;
    }
    /* poly receiver `poly !~ /re/`: nil !~ is always true, a string tests the
       negated match; any other tag has no =~ so !~ raises NoMethodError. A
       non-poly (string) receiver keeps the direct negated-match emit. */
    if (are >= 0 && sp_streq(name, "!~") && rt == TY_POLY) {
      int tv = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
      buf_printf(b, "(_t%d.tag == SP_TAG_STR ? !sp_re_match_p(sp_re_pat_%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? 1"
                    " : (sp_raise_nomethod(\"undefined method '=~' for poly\"), 0))",
                 tv, are, tv, tv);
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
    if (are >= 0 && sp_streq(name, "match") && nt_ref(nt, id, "block") >= 0) {
      /* match(re) { |m| body }: yield the MatchData on a hit, evaluate to the
         block's value; nil (block not run) on a miss */
      int mblk = nt_ref(nt, id, "block");
      const char *mp0 = block_param_name(c, mblk, 0);
      const char *mp0r = mp0 ? rename_local(mp0) : NULL;
      int mbody = nt_ref(nt, mblk, "body");
      int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
      int tm = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata(sp_re_pat_%d, ", tm, are);
      emit_str_expr(c, recv, b);
      buf_printf(b, "); sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); if (_t%d) { ",
                 tr2, tr2, tm);
      if (mp0r) buf_printf(b, "lv_%s = _t%d; ", mp0r, tm);
      for (int j = 0; j + 1 < mbn; j++) { emit_stmt(c, mbb[j], b, 0); }
      if (mbn > 0) {
        buf_printf(b, "_t%d = ", tr2);
        emit_boxed(c, mbb[mbn - 1], b);
        buf_puts(b, "; ");
      }
      buf_printf(b, "} _t%d; })", tr2);
      return;
    }
    if (are >= 0 && sp_streq(name, "match")) {
      if (argc == 1) {
        /* recv is the subject string; emit_str_expr coerces a poly/nilable
           receiver (`@message.subject.match(/re/)`) to the const char* slot */
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", are); emit_str_expr(c, recv, b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", are); emit_str_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      return;
    }
    /* String#match?/#match with a String pattern argument: CRuby treats the
       string as a regexp source (Regexp.new(str)). Compile it at run time.
       (=~ / !~ with a String argument raise TypeError, so are excluded.) */
    if (are < 0 && (rt == TY_STRING || rt == TY_STRBUF) && argc == 1 &&
        comp_ntype(c, argv[0]) == TY_STRING &&
        (sp_streq(name, "match?") || sp_streq(name, "match"))) {
      int ts = ++g_tmp;
      const char *fn = sp_streq(name, "match?") ? "sp_re_match_p" : "sp_re_matchdata";
      buf_printf(b, "({ const char *_t%d = ", ts); emit_expr(c, argv[0], b);
      buf_printf(b, "; mrb_regexp_pattern *_t%dp = re_compile(_t%d, (int64_t)strlen(_t%d ? _t%d : \"\"), 0); ",
                 ts, ts, ts, ts);
      buf_printf(b, "%s(_t%dp, ", fn, ts); emit_expr(c, recv, b); buf_puts(b, "); })");
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
          /* `5 =~ /re/`, `3.5 !~ /re/`, `5.match?(/re/)`: Object#=~ was removed,
             so a numeric receiver has no =~/!~/match?/match -- raise
             NoMethodError instead of matching the number as a string (which
             would pass a numeric into sp_re_match_p's const char* slot). */
          if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) &&
              (sp_streq(name, "=~") || sp_streq(name, "!~") ||
               sp_streq(name, "match?") || sp_streq(name, "match"))) {
            const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
            const char *dv9 = default_value(comp_ntype(c, id));
            buf_puts(b, "((void)("); emit_expr(c, recv, b);
            buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of %s\"), %s))",
                       name, tn9, dv9 ? dv9 : "sp_box_nil()");
            free(rp.p); return;
          }
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
          /* poly receiver `poly =~ /re/`: String#=~ when it holds a string at
             runtime (e.g. an element read out of an array that widened to poly);
             any other tag has no =~ (Object#=~ was removed) -> NoMethodError,
             matching CRuby. */
          if (sp_streq(name, "=~") && rt == TY_POLY) {
            int tv = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = ", tv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
            buf_printf(b, "(_t%d.tag == SP_TAG_STR ? sp_re_match_poly(%s, _t%d.v.s)"
                          " : sp_raise_nomethod(\"undefined method '=~' for poly\"))",
                       tv, rp.p, tv);
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
    /* a poly RHS may hold an Array (spread across the directives) or a scalar
       (a one-element list) -- the distinction is only known at runtime. */
    if (at == TY_POLY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", sp_format_args("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
      return;
    }
    /* a single non-array scalar argument formats as a one-element array
       (nil renders empty for %s; Rational/Complex coerce inside the
       formatter's numeric directives) */
    if (at == TY_INT || at == TY_FLOAT || at == TY_STRING || at == TY_SYMBOL ||
        at == TY_NIL || at == TY_BOOL || at == TY_RATIONAL || at == TY_COMPLEX) {
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
        /* first/last type poly (boxed nil, printable as nil); the rest keep
           the historical int-nil sentinel pending their own nil arms */
        if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) { buf_puts(b, "sp_box_nil()"); return; }
        if ((sp_streq(name, "min") || sp_streq(name, "max") ||
             sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) { buf_puts(b, "SP_INT_NIL"); return; }
        if (sp_streq(name, "sample") && argc == 0) { buf_puts(b, "sp_box_nil()"); return; }  /* #2322 */
        if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) { buf_puts(b, "\"[]\""); return; }
        if ((sp_streq(name, "join") || sp_streq(name, "pack")) && argc <= 1) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
        if ((sp_streq(name, "union")) && argc == 0) { buf_puts(b, "sp_IntArray_new()"); return; }
        if ((sp_streq(name, "flatten") || sp_streq(name, "compact") || sp_streq(name, "uniq") ||
             sp_streq(name, "sort") || sp_streq(name, "reverse") || sp_streq(name, "dup") ||
             sp_streq(name, "clone") || sp_streq(name, "to_a") || sp_streq(name, "to_ary") ||
             sp_streq(name, "deconstruct") || sp_streq(name, "entries")) && argc <= 1) {
          buf_puts(b, "sp_PolyArray_new()"); return;
        }
      }
    }
  }

  /* respond_to?(:m): compile-time approximation. A universal method set is
     always true; otherwise consult the receiver's class / class-method chain.
     Unknown primitive methods answer conservatively false. Also fires for
     the receiverless (implicit-self) form, resolved against the enclosing
     class -- `self.fullscreen = v if respond_to?(:fullscreen=)` (doom's
     gosu_window.rb). */
  if (sp_streq(name, "respond_to?") && argc >= 1) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) {
      qm = nt_str(nt, argv[0], "content");
      if (!qm) qm = nt_str(nt, argv[0], "unescaped");
    }
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
        "tap", "then", "send", "===",
        /* Kernel/Object methods every CRuby object answers true for */
        "display", "yield_self", "public_send", "__send__", "method",
        "methods", "to_enum", "enum_for", "instance_variables",
        "instance_variable_get", "instance_variable_set",
        "instance_variable_defined?", "singleton_class", "extend", NULL };
      int yes = 0, resolved = 0;
      /* a compiler-synthesized helper (__enum_to_a) is not a real method: CRuby
         answers false, so never let the class-chain lookup below report it */
      if (name_is_synth_method(qm)) { resolved = 1; yes = 0; }
      for (int u = 0; !resolved && uni[u]; u++) if (sp_streq(qm, uni[u])) { yes = resolved = 1; break; }
      /* value-type receivers: their builtin surface is not in any class
         table; answer the well-known names directly (the probe below only
         reports methods spinel can dispatch, a subset of CRuby's answer) */
      if (!resolved && recv >= 0 && rt == TY_SYMBOL) {
        static const char *const symm[] = {
          "to_proc", "to_sym", "id2name", "name", "length", "size",
          "succ", "next", "upcase", "downcase", "capitalize", "swapcase",
          "empty?", "start_with?", "end_with?", "<=>", "[]", NULL };
        for (int u = 0; symm[u]; u++) if (sp_streq(qm, symm[u])) { yes = resolved = 1; break; }
      }
      if (!resolved && recv >= 0 && rt == TY_PROC) {
        static const char *const procm[] = {
          "call", "()", "[]", "yield", "arity", "lambda?", "curry",
          "to_proc", "parameters", "<<", ">>", NULL };
        for (int u = 0; procm[u]; u++) if (sp_streq(qm, procm[u])) { yes = resolved = 1; break; }
      }
      /* Time: a fixed builtin surface. Unknown names answer false (CRuby),
         which is what the report needs -- previously an unresolved TY_TIME
         receiver fell through to a true-ish default. */
      if (!resolved && recv >= 0 && rt == TY_TIME) {
        static const char *const timem[] = {
          "strftime", "year", "month", "mon", "day", "mday", "hour", "min",
          "sec", "wday", "yday", "to_i", "to_f", "to_r", "usec", "nsec",
          "tv_sec", "tv_usec", "tv_nsec", "subsec", "utc", "gmtime", "getutc",
          "localtime", "getlocal", "utc?", "gmt?", "dst?", "isdst", "zone",
          "asctime", "ctime", "iso8601", "to_a", "to_time",
          "sunday?", "monday?", "tuesday?", "wednesday?", "thursday?",
          "friday?", "saturday?", "+", "-", "<=>", "<", ">", "<=", ">=",
          "between?", "clamp", NULL };
        for (int u = 0; timem[u]; u++) if (sp_streq(qm, timem[u])) { yes = 1; break; }
        resolved = 1;
      }
      if (!resolved) {
        const char *rty = nt_type(nt, recv);
        if (rty && sp_streq(rty, "ConstantReadNode")) {
          int ci = comp_class_index(c, nt_str(nt, recv, "name"));
          if (ci >= 0) { resolved = 1; yes = class_responds_to(c, ci, qm); }
        }
        else if (recv >= 0 && ty_is_object(rt)) {
          int cid = ty_object_class(rt);
          /* a writer query (`m=`) consults the writer table under its base name */
          size_t ql = strlen(qm);
          int is_wr = ql > 0 && qm[ql - 1] == '=';
          char wbase[256]; wbase[0] = '\0';
          if (is_wr && ql - 1 < sizeof wbase) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
          int found = comp_method_in_chain(c, cid, qm, NULL) >= 0 ||
                      comp_reader_in_chain(c, cid, qm, NULL) ||
                      (is_wr && comp_writer_in_chain(c, cid, wbase, NULL));
          /* an Enumerable includer (marked by its synthesized __enum_to_a)
             answers true for the module's methods even though they have no
             entry in the class's own method table (the redirect serves them) */
          if (!found && comp_method_in_chain(c, cid, "__enum_to_a", NULL) >= 0 &&
              name_is_enumerable_module_method(qm)) { resolved = 1; yes = 1; }
          else if (!found && comp_method_in_chain(c, cid, "<=>", NULL) >= 0 &&
                   name_is_comparable_module_method(qm)) { resolved = 1; yes = 1; }
          /* a Struct/Data instance answers the implicit accessors it always
             carries even though they have no user method-table entry (#2663). */
          else if (!found && (c->classes[cid].is_data || c->classes[cid].is_struct) &&
                   (sp_streq(qm, "members") || sp_streq(qm, "to_h") ||
                    sp_streq(qm, "deconstruct") || sp_streq(qm, "deconstruct_keys") ||
                    (c->classes[cid].is_data && sp_streq(qm, "with")) ||
                    (c->classes[cid].is_struct &&
                     (sp_streq(qm, "to_a") || sp_streq(qm, "values") || sp_streq(qm, "each") ||
                      sp_streq(qm, "size") || sp_streq(qm, "length") || sp_streq(qm, "[]") ||
                      sp_streq(qm, "[]="))))) { resolved = 1; yes = 1; }
          else if (!found) { resolved = 1; yes = 0; }
          else {
            int v = comp_method_vis_in_chain(c, cid, qm);
            if (v == SP_VIS_PUBLIC) { resolved = 1; yes = 1; }       /* public: always */
            else if (foldable) { resolved = 1; yes = include_all; }  /* private/protected */
            /* else: private/protected + runtime include_all -> unresolved */
          }
        }
        else if (recv < 0) {
          /* implicit self: resolve against the enclosing scope's class. An
             instance method consults the instance chain (methods + attr
             readers/writers, a `m=` query matching the writer table under
             its base name); a class (`def self.x`) method consults the
             class-method chain and singleton attrs. Toplevel (class_id < 0)
             stays unresolved and takes the normal fall-through. */
          Scope *ss = comp_scope_of(c, id);
          if (ss && ss->class_id >= 0) {
            int cid = ss->class_id;
            size_t ql = strlen(qm);
            int is_wr = ql > 0 && qm[ql - 1] == '=';
            char wbase[256]; wbase[0] = '\0';
            if (is_wr && ql - 1 < sizeof wbase) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
            if (ss->is_cmethod) {
              /* implicit self is the class object itself: same answer as
                 the explicit `Const.respond_to?` fold, including the
                 builtin Class/Module capabilities (:new, :name, ...). */
              resolved = 1;
              yes = class_responds_to(c, cid, qm);
            }
            else {
              int found = comp_method_in_chain(c, cid, qm, NULL) >= 0 ||
                          comp_reader_in_chain(c, cid, qm, NULL) ||
                          (is_wr && comp_writer_in_chain(c, cid, wbase, NULL));
              if (!found) { resolved = 1; yes = 0; }
              else {
                /* receiverless respond_to? still answers false for a private
                   or protected match unless include_all folded true. */
                int v = comp_method_vis_in_chain(c, cid, qm);
                if (v == SP_VIS_PUBLIC) { resolved = 1; yes = 1; }
                else if (foldable) { resolved = 1; yes = include_all; }
                /* else: private/protected + runtime include_all -> unresolved */
              }
            }
          }
        }
        else if ((rt == TY_POLY || rt == TY_UNKNOWN) && any_class_defines(c, qm)) {
          /* poly receiver + a user protocol method (some user class defines qm):
             the analyze probe answers "SOME union member responds", which
             mis-decides per value -- a String member of String|UserClass would
             report true for the user's method and take the wrong branch. Emit a
             runtime check against the exact user classes that define qm; a
             builtin value (which cannot define a user protocol method) answers
             false. This mirrors the poly is_a? runtime cls_id check. */
          int tv = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
          buf_printf(b, "_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0 && (", tv, tv);
          size_t ql = strlen(qm);
          char wbase[256]; wbase[0] = '\0';
          int is_wr = ql > 0 && qm[ql - 1] == '=' && ql - 1 < sizeof wbase;
          if (is_wr) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
          int first = 1;
          for (int k = 0; k < c->nclasses; k++) {
            int has = comp_method_in_chain(c, k, qm, NULL) >= 0 ||
                      comp_reader_in_chain(c, k, qm, NULL) ||
                      (is_wr && comp_writer_in_chain(c, k, wbase, NULL));
            if (has) { buf_printf(b, "%s_t%d.cls_id == %d", first ? "" : " || ", tv, k); first = 0; }
          }
          if (first) buf_puts(b, "0");
          buf_printf(b, "); })");
          return;
        }
        else {
          /* primitive/builtin receiver (String/Integer/Array/...): consult the
             analyze-time probe -- a synthesized `recv.<qm>` call whose inferred
             type says whether spinel can actually dispatch the method. This
             derives the answer from the same resolver that types a real call,
             so it never drifts from what a real `recv.qm` would compile to. A
             poly/unknown receiver with no user protocol method falls through
             here (the builtin probe answer) rather than a possibly-wrong false. */
          int pn = 0; const int *probes = nt_arr(nt, id, "rt_probes", &pn);
          if (probes && pn > 0) {
            resolved = 1; yes = 0;
            /* Each probe is a synthesized `recv.m(...)` call the analyze fixpoint
               already typed with the real resolver; a recognized method infers a
               concrete type, an unrecognized one stays UNKNOWN. Reading the
               cached inferred type is side-effect free (unlike emitting, which
               mutates g_pre/g_tmp), so it is safe inside the live fold. */
            for (int p = 0; p < pn; p++)
              if (comp_ntype(c, probes[p]) != TY_UNKNOWN) { yes = 1; break; }
          }
        }
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
      /* Methods inherited from Object/Kernel are defined on every class. With
         `inherit` (the default) method_defined? must report the public ones as
         true even though they have no entry in the user class chain (#2673). */
      if (!found && inherit && md_pub) {
        static const char *const objm[] = {
          "==", "!=", "===", "<=>", "class", "clone", "dup", "display",
          "enum_for", "eql?", "equal?", "extend", "freeze", "frozen?", "hash",
          "inspect", "instance_of?", "instance_variable_defined?",
          "instance_variable_get", "instance_variable_set", "instance_variables",
          "is_a?", "itself", "kind_of?", "method", "methods", "nil?",
          "object_id", "private_methods", "protected_methods", "public_method",
          "public_methods", "public_send", "respond_to?", "send", "__send__",
          "singleton_class", "singleton_methods", "tap", "then", "to_enum",
          "yield_self", "to_s", NULL };
        for (int u = 0; objm[u]; u++) if (sp_streq(qm, objm[u])) { buf_puts(b, "1"); return; }
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

  /* Class.const_set(:K, v) with a literal name: a constant is a C global
     (cst_<name>) assigned at its definition site, so re-assigning an EXISTING
     one is just that store. The constant must already be defined -- a name the
     program never writes has no global to store into, and its type is what the
     definition inferred, so a value of another type has nowhere to go; both
     fall through to the diagnostic. CRuby returns the value (and warns about
     the reinitialization; spinel does not). #2675 */
  if (sp_streq(name, "const_set") && recv >= 0 && argc == 2) {
    const char *cs_aty = nt_type(nt, argv[0]);
    const char *cs_qm = NULL;
    if (cs_aty && sp_streq(cs_aty, "SymbolNode")) cs_qm = nt_str(nt, argv[0], "value");
    else if (cs_aty && sp_streq(cs_aty, "StringNode")) cs_qm = nt_str(nt, argv[0], "content");
    if (cs_qm) {
      LocalVar *cv = comp_const(c, cs_qm);
      if (cv && cv->type != TY_UNKNOWN && comp_ntype(c, argv[1]) == cv->type) {
        buf_printf(b, "(cst_%s = ", cs_qm);
        emit_expr(c, argv[1], b);
        buf_printf(b, ", cst_%s)", cs_qm);
        return;
      }
      static char csbuf[512];
      if (!cv || cv->type == TY_UNKNOWN)
        snprintf(csbuf, sizeof csbuf,
                 "const_set(:%s, ...) can only re-assign a constant the program already "
                 "defines: a name never written has no storage to set, because constants "
                 "are compile-time globals. Declare `%s = ...` first "
                 "(see docs/limitations.md)", cs_qm, cs_qm);
      else
        snprintf(csbuf, sizeof csbuf,
                 "const_set(:%s, ...) cannot change the constant's type: %s was inferred as "
                 "%s at its definition and is a C global of that type, so a %s value has "
                 "nowhere to go (see docs/limitations.md)", cs_qm, cs_qm,
                 ty_name(cv->type), ty_name(comp_ntype(c, argv[1])));
      unsupported_feature(c, id, csbuf);
    }
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
      /* Builtin module constants: Klass.const_get(:C) resolves to the same value
         as Klass::C. const_get's result is poly, so box it (#2685). */
      {
        const char *rvt = nt_type(nt, recv);
        const char *rnm = (rvt && (sp_streq(rvt, "ConstantReadNode") ||
                                   sp_streq(rvt, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
        if (rnm && sp_streq(rnm, "Float")) {
          if (sp_streq(cg_qm, "INFINITY")) { buf_puts(b, "sp_box_float(1.0/0.0)"); return; }
          if (sp_streq(cg_qm, "NAN"))      { buf_puts(b, "sp_box_float(0.0/0.0)"); return; }
          if (sp_streq(cg_qm, "MAX"))      { buf_puts(b, "sp_box_float(DBL_MAX)"); return; }
          if (sp_streq(cg_qm, "MIN"))      { buf_puts(b, "sp_box_float(DBL_MIN)"); return; }
          if (sp_streq(cg_qm, "EPSILON"))  { buf_puts(b, "sp_box_float(DBL_EPSILON)"); return; }
          if (sp_streq(cg_qm, "DIG"))      { buf_puts(b, "sp_box_int(DBL_DIG)"); return; }
          if (sp_streq(cg_qm, "MANT_DIG")) { buf_puts(b, "sp_box_int(DBL_MANT_DIG)"); return; }
          if (sp_streq(cg_qm, "RADIX"))    { buf_puts(b, "sp_box_int(FLT_RADIX)"); return; }
        }
        if (rnm && sp_streq(rnm, "Math")) {
          if (sp_streq(cg_qm, "PI")) { buf_puts(b, "sp_box_float(M_PI)"); return; }
          if (sp_streq(cg_qm, "E"))  { buf_puts(b, "sp_box_float(M_E)"); return; }
        }
      }
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
      if (const_name_is_wrong(qm)) {
        buf_printf(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), sp_raise_cls(\"NameError\", sp_sprintf(\"wrong constant name %%s\", ");
        emit_str_literal(b, qm);
        buf_printf(b, ")), 0)");
        return;
      }
      int yes = comp_const(c, qm) != NULL || comp_class_index(c, qm) >= 0;
      buf_printf(b, "%d", yes);
      return;
    }
  }

  /* String#concat with no arguments returns the receiver unchanged (#2309) */
  if (recv >= 0 && rt == TY_STRING && sp_streq(name, "concat") && argc == 0) {
    emit_expr(c, recv, b); return;
  }
  /* String#clear consumed as a value: empty the assignable receiver in place
     and yield the now-empty string (#2332) */
  if (recv >= 0 && rt == TY_STRING && sp_streq(name, "clear") && argc == 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "LocalVariableReadNode") ||
                sp_streq(rty, "InstanceVariableReadNode"))) {
      buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b);
      buf_puts(b, "); "); emit_expr(c, recv, b); buf_puts(b, " = (&(\"\\xff\")[1]); ");
      emit_expr(c, recv, b); buf_puts(b, "; })");
      return;
    }
    /* a direct literal receiver has no binding to empty; the value is "" (#2370) */
    if (rty && sp_streq(rty, "StringNode")) {
      buf_puts(b, "(&(\"\\xff\")[1])");
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
         fresh string); -str returns a FROZEN string (#2331). */
      if (name[0] == '+') { buf_puts(b, "sp_str_dup_external("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else { buf_puts(b, "sp_str_uminus_val("); emit_expr(c, recv, b); buf_puts(b, ")"); }  /* frozen recv: identity (#2369) */
    }
    else if (rt == TY_BIGINT) {
      /* -@ negates via 0 - b (no unary neg on a bigint pointer); +@ is self (#2304) */
      if (name[0] == '-') { buf_puts(b, "sp_bigint_sub(sp_bigint_new_int(0), "); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
    }
    else { buf_puts(b, name[0] == '-' ? "(-" : "(+"); emit_expr(c, recv, b); buf_puts(b, ")"); }
    return;
  }
  /* h.default_proc = ->(hh, k) { ... }: lower the lambda literal to the same
     dedicated dproc C function Hash.new{} uses and install it on the receiver
     (dproc + dproc_self slots exist on every poly-valued variant) (#2371). */
  if (recv >= 0 && sp_streq(name, "default_proc=") && argc == 1 &&
      (comp_ntype(c, recv) == TY_STR_POLY_HASH || comp_ntype(c, recv) == TY_SYM_POLY_HASH ||
       comp_ntype(c, recv) == TY_POLY_POLY_HASH) &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "LambdaNode")) {
    TyKind hrt = comp_ntype(c, recv);
    const char *hn2 = ty_hash_cname(hrt);
    int lam = argv[0];
    int lbody = nt_ref(nt, lam, "body");
    /* LambdaNode carries its ParametersNode directly (the BlockParameters
       wrapper is unwrapped at parse time), so read requireds by hand */
    const char *hp = NULL, *kp = NULL;
    { int pn = nt_ref(nt, lam, "parameters");
      if (pn >= 0) { int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
        if (rn > 0) hp = nt_str(nt, reqs[0], "name");
        if (rn > 1) kp = nt_str(nt, reqs[1], "name"); } }
    int dn = ++g_proc_counter;
    Buf *pb = &g_procs;
    const char *keyct = hrt == TY_SYM_POLY_HASH ? "sp_sym"
                      : hrt == TY_STR_POLY_HASH ? "const char *" : "sp_RbVal";
    buf_printf(pb, "static sp_RbVal _sp_hash_dproc_%d(sp_%sHash *_self_h, %s _key, void *_dproc_self) {\n",
               dn, hn2, keyct);
    buf_puts(pb, "  (void)_dproc_self;\n");
    if (hp) buf_printf(pb, "  sp_%sHash *lv_%s = _self_h; (void)lv_%s;\n", hn2, rename_local(hp), rename_local(hp));
    if (kp) {
      const char *box = hrt == TY_SYM_POLY_HASH ? "sp_box_sym(_key)"
                      : hrt == TY_STR_POLY_HASH ? "sp_box_str(_key)" : "_key";
      buf_printf(pb, "  sp_RbVal lv_%s = %s; (void)lv_%s;\n", rename_local(kp), box, rename_local(kp));
    }
    { Buf *sv_pre = g_pre; int sv_ind = g_indent;
      g_pre = pb; g_indent = 1;
      int bn = 0; const int *bb = lbody >= 0 ? nt_arr(nt, lbody, "body", &bn) : NULL;
      for (int k = 0; k + 1 < bn; k++) emit_stmt(c, bb[k], pb, 1);
      buf_puts(pb, "  return ");
      if (bn > 0) emit_boxed(c, bb[bn - 1], pb); else buf_puts(pb, "sp_box_nil()");
      buf_puts(pb, ";\n}\n");
      g_pre = sv_pre; g_indent = sv_ind; }
    int th = ++g_tmp;
    buf_printf(b, "({ sp_%sHash *_t%d = ", hn2, th); emit_expr(c, recv, b);
    buf_printf(b, "; _t%d->dproc = _sp_hash_dproc_%d; _t%d->dproc_self = NULL; _t%d; })",
               th, dn, th, th);
    return;
  }
  /* value-position String#[]= (s[i] = v / s[i, n] = v / s[range] = v / s["sub"]
     = v on an assignable receiver): run the mutate statement, the expression's
     value is the assigned string (#2370). */
  if (recv >= 0 && sp_streq(name, "[]=") && (argc == 2 || argc == 3) &&
      comp_ntype(c, recv) == TY_STRING &&
      nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") ||
                            sp_streq(nt_type(nt, recv), "InstanceVariableReadNode"))) {
    Buf mb; memset(&mb, 0, sizeof mb);
    if (emit_array_mutate_stmt(c, id, &mb, 0)) {
      buf_puts(b, "({ ");
      buf_puts(b, mb.p ? mb.p : "");
      emit_expr(c, argv[argc - 1], b);
      buf_puts(b, "; })");
      free(mb.p);
      return;
    }
    free(mb.p);
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
    /* a by-value object has no pointer to null-check and is never falsy (#2633) */
    else if (ty_is_object(rt) && comp_ty_value_obj(c, rt)) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
    }
    else if (rt == TY_STRING || ty_is_array(rt) || ty_is_hash(rt) || ty_is_object(rt) ||
             rt == TY_PROC ||
             rt == TY_MATCHDATA || rt == TY_EXCEPTION || rt == TY_FIBER || rt == TY_IO) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");  /* NULL pointer is falsy */
    }
    else { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), 0)"); }  /* always-truthy -> false */
    return;
  }

  /* default Object#<=>: 0 when the operands are the same object, nil otherwise
     (identity). Only for a reference user object with no user `<=>` (#2686). */
  if (sp_streq(name, "<=>") && argc == 1 && recv >= 0 && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt) && comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) < 0) {
    const char *cn = c->classes[ty_object_class(rt)].c_name;
    if (comp_ntype(c, argv[0]) == rt) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", cn, ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_%s *_t%d = ", cn, tb); emit_expr(c, argv[0], b);
      buf_printf(b, "; _t%d == _t%d ? sp_box_int(0) : sp_box_nil(); })", ta, tb);
    } else {
      /* a different-class operand is never the same object: nil */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
      emit_boxed(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    }
    return;
  }

  /* poly arithmetic: sp_poly_<op>(boxed, boxed) -> a (poly) result.
     `str + poly` / `str * poly` are string concat/repeat (handled below as
     sp_str_concat/sp_str_repeat with the poly operand coerced), not poly
     arithmetic, so let them fall through. */
  if (recv >= 0 && argc == 1 && (rt == TY_POLY || a0 == TY_POLY) &&
      rt != TY_TIME &&  /* Time +/- a poly is Time arithmetic (emit_array_arith_call, #2456) */
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
       (rt == TY_POLY && sp_streq(name, ">>")))) {
    TyKind at0 = comp_ntype(c, argv[0]);
    /* A `<<`/`>>` by a NEGATIVE (or >= word width) count is UB as a bare C shift
       -- Ruby shifts the other way for a negative count. Only a constant literal
       in that range takes the sp_int_shl/shr path; a non-constant count stays a
       direct C shift (the hot idiom, e.g. optcarrot's `hi << sweep_shift`, whose
       counts are always small and non-negative -- routing it through a branchy
       helper cost ~4% fps). */
    int is_shift = sp_streq(name, "<<") || sp_streq(name, ">>");
    /* a non-integer operand raises TypeError, as CRuby (#2421) -- except that
       the SHIFT operators accept a Float count and truncate it via to_int
       (`10 << 2.9` is 40); the bitwise &/|/^ still reject a Float. */
    if ((at0 == TY_FLOAT && !is_shift) || at0 == TY_STRING || at0 == TY_NIL || at0 == TY_SYMBOL ||
        at0 == TY_BOOL || ty_is_array(at0) || ty_is_hash(at0)) {
      const char *tn9 = at0 == TY_FLOAT ? "Float" : at0 == TY_STRING ? "String"
                      : at0 == TY_NIL ? "nil" : at0 == TY_SYMBOL ? "Symbol"
                      : at0 == TY_BOOL ? "boolean" : ty_is_array(at0) ? "Array" : "Hash";
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Integer\"); (mrb_int)0; })", tn9);
      return;
    }
    /* |/^ with a Bignum operand promote (the & mask idiom keeps its low-64
       truncation: the result fits an int either way) (#2422) */
    if ((sp_streq(name, "|") || sp_streq(name, "^")) && at0 == TY_BIGINT) {
      buf_printf(b, "sp_bigint_%s(sp_bigint_new_int(", sp_streq(name, "|") ? "or" : "xor");
      if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    const char *aty0 = nt_type(nt, argv[0]);
    int lit_shift = aty0 && sp_streq(aty0, "IntegerNode");
    long long litc = lit_shift ? nt_int(nt, argv[0], "value", 0) : 0;
    /* `<< 63` joins the helper path: it overflows (or lands on the nil
       sentinel INTPTR_MIN) for every nonzero receiver, so the overflow check
       in sp_int_shl must see it. */
    if (is_shift && lit_shift &&
        (litc < 0 || litc >= 64 || (sp_streq(name, "<<") && litc >= 63))) {
      buf_printf(b, "sp_int_%s(", sp_streq(name, "<<") ? "shl" : "shr");
      if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_printf(b, ", %lldLL)", litc);
      return;
    }
    if (is_shift && !lit_shift) {
      /* a runtime shift count: range-checked (negative shifts the other way,
         past-the-word raises) via a single-compare fast path (#2423) */
      buf_printf(b, "sp_int_%s_ck(", sp_streq(name, "<<") ? "shl" : "shr");
      if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      else emit_expr(c, recv, b);
      buf_puts(b, ", ");
      if (at0 == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (at0 == TY_FLOAT) { buf_puts(b, "(mrb_int)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    /* `<<` on a signed value is undefined behavior when the receiver is
       negative (`-8 << 1`); do the shift on the unsigned bit pattern and
       reinterpret -- same two's-complement result, well-defined, no runtime
       cost (a bare reinterpret). `>>` stays a signed (arithmetic) shift, which
       matches Ruby and is only implementation-defined, not UB. */
    int shl_neg_safe = sp_streq(name, "<<");
    buf_puts(b, "(");
    if (shl_neg_safe) buf_puts(b, "(mrb_int)((uint64_t)(");
    if (rt == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else emit_expr(c, recv, b);
    if (shl_neg_safe) buf_puts(b, ")");
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
    if (shl_neg_safe) buf_puts(b, ")");
    buf_puts(b, ")");
    return;
  }

  /* any?/all?/none?/one?(Class) over an array: Class === element membership
     (the value-argument arms compare ==). Walks the boxed elements so every
     array kind is covered. */
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      ty_is_array(rt) && comp_ntype(c, argv[0]) == TY_CLASS &&
      (sp_streq(name, "any?") || sp_streq(name, "all?") ||
       sp_streq(name, "none?") || sp_streq(name, "one?"))) {
    int ta = ++g_tmp, tc2 = ++g_tmp, tn = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
    buf_printf(b, "; sp_Class _t%d = ", tc2); emit_expr(c, argv[0], b);
    buf_puts(b, "; "); emit_poly_iter_obj_normalize(c, ta, b);
    buf_printf(b, "mrb_int _t%d = sp_poly_arr_len_ex(_t%d); mrb_int _t%d = 0;"
                  " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                  " if (sp_poly_is_a(sp_poly_each_elem(_t%d, _t%d), _t%d)) _t%d++; ",
               tn, ta, tcnt, ti, ti, tn, ti, ta, ti, tc2, tcnt);
    if (sp_streq(name, "any?"))       buf_printf(b, "_t%d > 0; })", tcnt);
    else if (sp_streq(name, "all?"))  buf_printf(b, "_t%d == _t%d; })", tcnt, tn);
    else if (sp_streq(name, "none?")) buf_printf(b, "_t%d == 0; })", tcnt);
    else                              buf_printf(b, "_t%d == 1; })", tcnt);
    return;
  }

  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) {
    /* Re-infer when stale cache has TY_POLY (e.g. block params temporarily pinned to element type). */
    TyKind lrt = (rt == TY_POLY || rt == TY_UNKNOWN) ? infer_type(c, recv) : rt;
    TyKind at = comp_ntype(c, argv[0]);
    TyKind lat = (at == TY_POLY || at == TY_UNKNOWN) ? infer_type(c, argv[0]) : at;
    /* nil <=> nil is 0; nil <=> anything-else is nil (#2383) */
    if (lrt == TY_NIL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_boxed(c, argv[0], b);
      buf_printf(b, "), %s)", lat == TY_NIL ? "(mrb_int)0" : "SP_INT_NIL");
      return;
    }
    /* Float <=> Rational: compare by float value, agreeing with the operators
       (<, <=, ...) that already coerce (#2596). The reverse direction works. */
    if (lrt == TY_FLOAT && lat == TY_RATIONAL) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_printf(b, "({ double _t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; double _t%d = sp_rational_to_f(", tb); emit_expr(c, argv[0], b);
      buf_printf(b, "); isnan(_t%d) ? SP_INT_NIL : (mrb_int)((_t%d > _t%d) - (_t%d < _t%d)); })",
                 ta, ta, tb, ta, tb);
      return;
    }
    /* Bignum <=> (either side): compare by value, not the pointer identity a
       raw `>`/`<` on the sp_Bigint* would give (always -1) (#2581) */
    if ((lrt == TY_BIGINT || lat == TY_BIGINT) &&
        (lrt == TY_INT || lrt == TY_BIGINT) && (lat == TY_INT || lat == TY_BIGINT)) {
      int tc = ++g_tmp;
      buf_printf(b, "({ int _t%d = sp_bigint_cmp(", tc);
      if (lrt == TY_BIGINT) emit_expr(c, recv, b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, recv, b); buf_puts(b, ")"); }
      buf_puts(b, ", ");
      if (lat == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, "); (mrb_int)((_t%d > 0) - (_t%d < 0)); })", tc, tc);
      return;
    }
    if (ty_is_numeric(lrt) && ty_is_numeric(lat)) {
      int ta = ++g_tmp, tb = ++g_tmp;
      buf_puts(b, "({ "); emit_ctype(c, lrt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b);
      buf_puts(b, "; "); emit_ctype(c, lat, b); buf_printf(b, " _t%d = ", tb); emit_expr(c, argv[0], b);
      /* a NaN operand makes <=> nil, not 0 (#2315); only floats can be NaN */
      if (lrt == TY_FLOAT || lat == TY_FLOAT)
        buf_printf(b, "; (isnan((double)_t%d) || isnan((double)_t%d)) ? SP_INT_NIL"
                      " : (mrb_int)((_t%d > _t%d) - (_t%d < _t%d)); })", ta, tb, ta, tb, ta, tb);
      else
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
      TyKind a0t = comp_ntype(c, argv[0]);
      if (a0t == TY_TIME) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; (mrb_int)sp_time_cmp(_t%d, _t%d); })", ta, tb);
        return;
      }
      /* Time <=> non-Time is nil (poly result). A poly operand is checked at
         run time; a concrete non-Time operand is unconditionally nil. */
      if (a0t == TY_POLY || a0t == TY_UNKNOWN) {
        int ta = ++g_tmp, tb = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tb); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME) ? "
                      "sp_box_int(sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p)) : sp_box_nil(); })",
                   tb, tb, ta, tb);
        return;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
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
      /* sp_poly_spaceship answers nil for incomparable runtime operands (the
         int-nil sentinel) but 0 for identical singletons -- `nil <=> nil` is 0
         even though the two are not "comparable" in the Comparable sense. */
      buf_puts(b, "sp_poly_spaceship("); emit_boxed(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return;
    }
    /* Statically incomparable concrete operands (1 <=> "a"): Ruby answers
       nil. User objects fall through to their own #<=> dispatch. */
    if (lrt != TY_UNKNOWN && lat != TY_UNKNOWN &&
        !ty_is_object(lrt) && !ty_is_object(lat)) {
      buf_puts(b, "((void)(");
      emit_expr(c, recv, b);
      buf_puts(b, "), (void)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "), SP_INT_NIL)");
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
      /* a statically non-numeric operand (nil, a string, ...) raises the
         Comparable ArgumentError instead of comparing against a coerced 0 */
      TyKind cat = comp_ntype(c, argv[0]);
      if (cat == TY_NIL || cat == TY_STRING || cat == TY_BOOL || cat == TY_SYMBOL) {
        const char *cn9 = rt == TY_FLOAT ? "Float" : rt == TY_RATIONAL ? "Rational" : "Integer";
        const char *an9 = cat == TY_NIL ? "nil" : cat == TY_STRING ? "String"
                        : cat == TY_SYMBOL ? "Symbol" : "boolean";
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "), sp_raise_cls(\"ArgumentError\", \"comparison of %s with %s failed\"), 0)",
                   cn9, an9);
        return;
      }
      buf_puts(b, "(");
      emit_expr(c, recv, b);
      buf_printf(b, " %s ", name);
      /* a poly or unresolved-call (raise-all token) right operand against a
         numeric left: coerce it to the comparison's numeric type so the C `>=`
         does not compare an mrb_int with an sp_RbVal (`len >= x.megabytes`, an
         unresolved Rails method). */
      TyKind rht9 = (rt == TY_FLOAT || rt == TY_RATIONAL) ? TY_FLOAT : TY_INT;
      if (cat == TY_POLY) {
        buf_printf(b, "%s(", rht9 == TY_FLOAT ? "sp_poly_to_f" : "sp_poly_to_i");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (cat == TY_UNKNOWN || cat == TY_VOID)
        emit_unresolved_coerced(c, argv[0], rht9, b);
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return;
    }
    if (rt == TY_STRING) {
      buf_puts(b, "(strcmp(");
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
      buf_printf(b, ") %s 0)", name);
      return;
    }
    /* Time comparison via sp_time_cmp; a relational against a non-Time operand
       raises ArgumentError (CRuby's Comparable, its <=> having returned nil). */
    if (rt == TY_TIME) {
      if (comp_ntype(c, argv[0]) == TY_TIME) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = ", tt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Time _t%d = ", tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
        return;
      }
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"comparison of Time with an incompatible value failed\"); 0; })");
      return;
    }
    /* An object whose class defines the comparison operator itself (e.g.
       Set#< aliased to proper_subset?): dispatch to the user method. */
    if (ty_is_object(rt)) {
      int cid5 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid5, name, NULL) >= 0) {
        char selfp5[64];
        const char *rty5 = nt_type(nt, recv);
        if (rty5 && (sp_streq(rty5, "LocalVariableReadNode") ||
                     sp_streq(rty5, "InstanceVariableReadNode") ||
                     sp_streq(rty5, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfp5, sizeof selfp5, "%s", rb.p ? rb.p : "");
          free(rb.p);
        }
        else {
          int t5 = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", t5, rb.p ? rb.p : "");
          free(rb.p);
          snprintf(selfp5, sizeof selfp5, "_t%d", t5);
        }
        emit_dispatch(c, cid5, name, selfp5, nt_ref(nt, id, "arguments"), -1, b);
        return;
      }
    }
    /* Comparable: object with a user `<=>` method but no direct `<` etc. */
    if (ty_is_object(rt)) {
      int cid4 = ty_object_class(rt);
      if (comp_method_in_chain(c, cid4, name, NULL) < 0 &&
          comp_method_in_chain(c, cid4, "<=>", NULL) >= 0) {
        /* a `<=>` that can return nil: check it -- incomparable raises the
           Comparable ArgumentError instead of comparing a garbage value */
        if (user_cmp_needs_check(c, cid4)) {
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
    /* Hash subset/superset: < <= > >= over any hash-variant pairing. An empty
       `{}` literal operand (TY_UNKNOWN) is an empty hash all the same (#2399). */
    {
      TyKind h_rt = rt, h_a0 = comp_ntype(c, argv[0]);
      if (h_rt == TY_UNKNOWN && nt_type(nt, recv) &&
          (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")) &&
          ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) h_rt = TY_STR_POLY_HASH;
      if (h_a0 == TY_UNKNOWN && nt_type(nt, argv[0]) &&
          (sp_streq(nt_type(nt, argv[0]), "HashNode") || sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) &&
          ({ int _n = 0; nt_arr(nt, argv[0], "elements", &_n); _n == 0; })) h_a0 = TY_STR_POLY_HASH;
      if (ty_is_hash(h_rt) && argc == 1 && ty_is_hash(h_a0)) {
        int strict = sp_streq(name, "<") || sp_streq(name, ">");
        int flip = sp_streq(name, ">") || sp_streq(name, ">=");
        buf_puts(b, "sp_poly_hash_subset(");
        if (flip) { emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, recv, b); }
        else { emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); }
        buf_printf(b, ", %d)", strict);
        return;
      }
    }
    if (0 && ty_is_hash(rt) && argc == 1 && ty_is_hash(comp_ntype(c, argv[0]))) {
      int strict = sp_streq(name, "<") || sp_streq(name, ">");
      int flip = sp_streq(name, ">") || sp_streq(name, ">=");
      buf_puts(b, "sp_poly_hash_subset(");
      if (flip) { emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, recv, b); }
      else { emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); }
      buf_printf(b, ", %d)", strict);
      return;
    }
    unsupported(c, id, "comparison");
  }

  /* concrete builtin receiver: is_a?/kind_of?/instance_of? is known at compile
     time (evaluate the receiver for side effects, then yield the constant). */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?")) &&
      isa_const_name(nt, argv[0])) {
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
    /* a bool receiver's class depends on its VALUE: true is a TrueClass,
       false a FalseClass (ty_matches_class carries only the type) */
    {
      const char *bcn = nt_str(nt, argv[0], "name");
      if (rt == TY_BOOL && bcn &&
          (sp_streq(bcn, "TrueClass") || sp_streq(bcn, "FalseClass"))) {
        buf_puts(b, "(("); emit_expr(c, recv, b);
        buf_printf(b, ") %s 0)", sp_streq(bcn, "TrueClass") ? "!=" : "==");
        return;
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
    const char *cn = isa_const_name(nt, argv[0]);
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
      else if (sp_streq(cn, "Hash"))     buf_printf(b, "(%s.tag == SP_TAG_OBJ && ((%s.cls_id <= -13 && %s.cls_id >= -20) || %s.cls_id == -34))", v, v, v, v);
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
        /* a builtin ancestor not covered above (Object/BasicObject/Kernel are
           universal; Numeric/Comparable/Enumerable are module mixins): defer to
           the runtime ancestry helper instead of a blanket false. */
        else if (!exact) buf_printf(b, "sp_poly_kind_of_builtin(%s, \"%s\")", v, cn);
        else buf_printf(b, "(strcmp(sp_poly_class_name(%s), \"%s\") == 0)", v, cn);
      }
      buf_puts(b, "; })");
      return;
    }
  }

  /* nil receiver: nil.inspect -> "nil", nil.to_s -> "", nil.nil? -> true.
     Evaluate the receiver for side effects, then yield the constant. */
  if (recv >= 0 && rt == TY_NIL) {
    /* nil & / | / ^ are BOOLEAN ops (nil is false): & is always false, | and ^
       are the argument's truthiness -- not integer bitwise (#2401) */
    if (argc == 1 && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^"))) {
      if (sp_streq(name, "&")) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), (void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
      } else {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_puts(b, "), sp_poly_truthy("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
      }
      return;
    }
    if (argc == 0 && sp_streq(name, "inspect")) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"nil\"))"); return; }
    if (argc == 0 && sp_streq(name, "to_s"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SPL(\"\"))"); return; }
    if (argc == 0 && sp_streq(name, "nil?"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)"); return; }
    if (argc == 0 && sp_streq(name, "to_i"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_int)0)"); return; }
    if (argc == 0 && sp_streq(name, "to_f"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0.0)"); return; }
    if (argc == 0 && (sp_streq(name, "to_r") || sp_streq(name, "rationalize"))) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_rational_new(0, 1))"); return; }
    /* nil =~ anything is nil; nil !~ anything is true (#2385) */
    if (argc == 1 && sp_streq(name, "=~")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      return;
    }
    if (argc == 1 && sp_streq(name, "!~")) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)");
      return;
    }
    if (argc == 0 && sp_streq(name, "to_c"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_Complex){0, 0})"); return; }
    if (argc == 0 && sp_streq(name, "to_a"))    { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())"); return; }
    if (argc == 0 && sp_streq(name, "to_h"))    {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_SymPolyHash_new())");
      return;
    }
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      const char *cn = isa_const_name(nt, argv[0]);
      int yes = cn ? (sp_streq(cn, "NilClass") || sp_streq(name, "instance_of?") ? sp_streq(cn, "NilClass") : (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) : 0;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return;
    }
    /* NilClass boolean operators: & is always false, | and ^ test the
       operand's truthiness */
    if (argc == 1 && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^"))) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ");
      if (sp_streq(name, "&")) { buf_puts(b, "(void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)"); }
      else { buf_puts(b, "sp_poly_truthy("); emit_boxed(c, argv[0], b); buf_puts(b, "))"); }
      return;
    }
    if (argc == 1 && (sp_streq(name, "===") || sp_streq(name, "equal?") || sp_streq(name, "eql?"))) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ").tag == SP_TAG_NIL)");
      return;
    }
  }
  /* boolean receiver & | ^ with a non-boolean operand: logical ops on the
     operand's truthiness (an Integer operand included -- 0 is truthy) */
  if (recv >= 0 && rt == TY_BOOL && argc == 1 &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^"))) {
    TyKind bat = comp_ntype(c, argv[0]);
    if (bat != TY_BOOL) {
      int tb3 = ++g_tmp;
      buf_printf(b, "({ mrb_bool _t%d = ", tb3); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_bool _a%d = sp_poly_truthy(", tb3);
      emit_boxed(c, argv[0], b);
      buf_puts(b, "); ");
      if (sp_streq(name, "&")) buf_printf(b, "(_t%d && _a%d); })", tb3, tb3);
      else if (sp_streq(name, "|")) buf_printf(b, "(_t%d || _a%d); })", tb3, tb3);
      else buf_printf(b, "(_t%d != _a%d); })", tb3, tb3);
      return;
    }
  }
  /* true/false receiver: equal?/eql?/=== are value identity */
  if (recv >= 0 && rt == TY_BOOL && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==="))) {
    int tb2 = ++g_tmp;
    buf_printf(b, "({ mrb_bool _t%d = ", tb2); emit_expr(c, recv, b);
    buf_printf(b, "; sp_RbVal _tb%d = ", tb2); emit_boxed(c, argv[0], b);
    buf_printf(b, "; (_tb%d.tag == SP_TAG_BOOL && _tb%d.v.b == (_t%d != 0)); })", tb2, tb2, tb2);
    return;
  }
  /* Symbol receiver: equal?/eql? compare the interned id */
  /* A statically-typed user object is_a? the universal ancestors */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?"))) {
    const char *acn = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
                        ? nt_str(nt, argv[0], "name") : NULL;
    if (acn && (sp_streq(acn, "Object") || sp_streq(acn, "BasicObject") ||
                sp_streq(acn, "Kernel"))) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 1)");
      return;
    }
    /* a builtin mixin: fold from the class's own `include` declarations (#2363) */
    if (acn && (sp_streq(acn, "Comparable") || sp_streq(acn, "Enumerable") ||
                sp_streq(acn, "Math")) && comp_class_index(c, acn) < 0) {
      int yes = class_includes_module_named(c, ty_object_class(rt), acn);
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
      return;
    }
  }
  /* Symbol#encoding: US-ASCII when the name is pure ASCII, UTF-8 otherwise */
  if (recv >= 0 && rt == TY_SYMBOL && argc == 0 && sp_streq(name, "encoding")) {
    int te = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_sym_to_s(", te);
    emit_expr(c, recv, b);
    buf_printf(b, "); int _a%d = 1; for (const char *_p%d = _t%d; *_p%d; _p%d++)"
                  " if ((unsigned char)*_p%d >= 0x80) { _a%d = 0; break; }"
                  " sp_box_encoding(_a%d ? sp_encoding_us_ascii() : sp_encoding_utf8()); })",
               te, te, te, te, te, te, te, te);
    return;
  }
  if (recv >= 0 && rt == TY_SYMBOL && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
      comp_ntype(c, argv[0]) == TY_SYMBOL) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
    emit_expr(c, argv[0], b); buf_puts(b, "))");
    return;
  }
  /* Kernel#display prints to_s with no newline, returns nil */
  if (recv >= 0 && sp_streq(name, "display") && argc == 0) {
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
      return;
    }
    buf_puts(b, "((void)fputs(sp_poly_to_s(");
    emit_boxed(c, recv, b);
    buf_puts(b, "), stdout))");
    return;
  }
  /* instance_variable_defined?(:@x / '@x') on a statically-typed object:
     the layout answers at compile time */
  if (recv >= 0 && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      ty_is_object(rt) && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *ivn = sp_streq(nt_type(nt, argv[0]), "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    int have = ivn && ivn[0] == '@' && comp_ivar_index(&c->classes[ty_object_class(rt)], ivn) >= 0;
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", have);
    return;
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
      return;
    }
    /* An Integer receiver with a Bignum bound (`50.between?(1, 2 ** 100)`):
       comparing mrb_int against sp_Bigint* with >=/<= is a pointer comparison.
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
      if (mi_b >= 0 && user_cmp_needs_check(c, cid_b)) {
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
           keeps the nested hoist from splitting our declaration line. */
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts);
        buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
        Buf lb; memset(&lb, 0, sizeof lb);
        if (arg_poly) emit_boxed(c, argv[0], &lb); else { Buf t = expr_buf(c, argv[0]); lb = t; }
        emit_indent(g_pre, g_indent);
        if (arg_poly) buf_puts(g_pre, "sp_RbVal"); else emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", tlo);
        buf_puts(g_pre, lb.p ? lb.p : ""); buf_puts(g_pre, ";\n"); free(lb.p);
        Buf hb; memset(&hb, 0, sizeof hb);
        if (arg_poly) emit_boxed(c, argv[1], &hb); else { Buf t = expr_buf(c, argv[1]); hb = t; }
        emit_indent(g_pre, g_indent);
        if (arg_poly) buf_puts(g_pre, "sp_RbVal"); else emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = ", thi);
        buf_puts(g_pre, hb.p ? hb.p : ""); buf_puts(g_pre, ";\n"); free(hb.p);
        buf_printf(b, "(sp_%s_%s((sp_%s *)_t%d, _t%d) >= 0 && sp_%s_%s((sp_%s *)_t%d, _t%d) <= 0)",
                   cname, mc("<=>"), cname, ts, tlo,
                   cname, mc("<=>"), cname, ts, thi);
        return;
      }
    }
  }

  /* object_id: a stable integer id. Int uses MRI's 2n+1; pointer-backed
     values use the pointer bit pattern; a symbol uses its interned id.
     The immediates have fixed ids: nil is 4, false is 0, true is 20. */
  if ((sp_streq(name, "object_id") || sp_streq(name, "__id__")) && recv >= 0 && argc == 0) {
    if (rt == TY_INT) { buf_puts(b, "(2*("); emit_expr(c, recv, b); buf_puts(b, ")+1)"); }
    else if (rt == TY_SYMBOL) { buf_puts(b, "((mrb_int)("); emit_expr(c, recv, b); buf_puts(b, ")*2)"); }
    else if (rt == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 4)"); }
    else if (rt == TY_BOOL) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? 20 : 0)"); }
    /* a boxed value: its identity is the boxed payload (heap pointer / int) */
    else if (rt == TY_POLY) { buf_puts(b, "((mrb_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, ").v.p)"); }
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
    else { buf_puts(b, "((mrb_int)(uintptr_t)("); emit_expr(c, recv, b); buf_puts(b, "))"); }
    return;
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
    return;
  }
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 &&
      (!ty_is_object(rt) ||
       (comp_method_in_chain(c, ty_object_class(rt), "hash", NULL) < 0 &&
        /* structs keep their dedicated value-based hash arm below */
        !c->classes[ty_object_class(rt)].is_struct))) {
    buf_puts(b, "sp_rbval_hash_key("); emit_boxed(c, recv, b); buf_puts(b, ")");
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
       rt == TY_METHOD || rt == TY_IO ||
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
  /* nil?/empty? on an empty array literal fold to constants (an Array is never
     nil; an empty one is empty). The `[]` literal has no side effects. */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "empty?")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    buf_puts(b, sp_streq(name, "empty?") ? "1" : "0");
    return;
  }
  /* nil?/frozen?/empty?/is_a?(Hash) on an empty hash literal fold to constants
     (a Hash is never nil; a bare {} is unfrozen and empty). */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")) &&
      ({ int _n = 0; nt_arr(nt, recv, "elements", &_n); _n == 0; })) {
    if (argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "frozen?") || sp_streq(name, "any?"))) { buf_puts(b, "0"); return; }
    if (argc == 0 && sp_streq(name, "empty?")) { buf_puts(b, "1"); return; }
    if (argc == 0 && sp_streq(name, "to_h") && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_StrPolyHash_new()"); return;   /* {}.to_h == {} (#2410) */
    }
    if (argc <= 1 && sp_streq(name, "sum") && nt_ref(nt, id, "block") < 0) {
      /* {}.sum == the init (or 0) (#2416) */
      if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "0");
      return;
    }
    if (argc == 0 && (sp_streq(name, "min") || sp_streq(name, "max"))) {
      buf_puts(b, "sp_box_nil()"); return;   /* empty: nil (#2406) */
    }
    if (argc == 0 && sp_streq(name, "minmax")) {
      int t2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_nil()); sp_PolyArray_push(_t%d, sp_box_nil()); _t%d; })",
                 t2, t2, t2, t2, t2);
      return;
    }
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) {
      const char *cn = isa_const_name(nt, argv[0]);
      buf_puts(b, cn && sp_streq(cn, "Hash") ? "1" : "0");
      return;
    }
  }

  /* Class.===(obj): equivalent to obj.is_a?(Class). Receiver is a class
     constant, either a bare `Integer` or a top-level `::Integer` (a
     ConstantPathNode with no parent), both naming the class in "name". (#2889) */
  if (recv >= 0 && argc == 1 && sp_streq(name, "===") && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
       (sp_streq(nt_type(nt, recv), "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0))) {
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
      /* an exception instance matches by name up its class chain (#2759) */
      if (at2 == TY_EXCEPTION && (is_exc_name(cn) || is_builtin_class_name(cn))) {
        buf_puts(b, "sp_exc_is_a("); emit_expr(c, argv[0], b);
        buf_printf(b, ", \"%s\")", cn);
        return;
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
  /* A literal Hash.new(d) receiver that never narrowed: .default and []
     both fold to the default value (no write can have reached the hash);
     the key/receiver still evaluate for their side effects. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      ((sp_streq(name, "default") && argc <= 1) ||   /* default(key) too (#2409) */
       (sp_streq(name, "values_at") && argc >= 1) || /* all keys miss -> defaults (#2408) */
       (sp_streq(name, "[]") && argc == 1))) {
    int dn = hash_new_default_arg(c, recv);
    if (dn >= 0) {
      TyKind dt = comp_ntype(c, dn);
      int t = ++g_tmp;
      buf_puts(b, "({ ");
      emit_ctype(c, dt, b);
      buf_printf(b, " _t%d = ", t);
      emit_expr(c, dn, b);
      buf_puts(b, "; ");
      if (sp_streq(name, "values_at")) {
        int ta = ++g_tmp;
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", ta, ta);
        for (int a = 0; a < argc; a++) {
          buf_puts(b, "(void)("); emit_boxed(c, argv[a], b); buf_puts(b, "); ");
          char dv[24]; snprintf(dv, sizeof dv, "_t%d", t);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", ta);
          if (dt == TY_POLY) buf_puts(b, dv); else emit_boxed_text(c, dt, dv, b);
          buf_puts(b, "); ");
        }
        buf_printf(b, "_t%d; })", ta);
        return;
      }
      if (argc == 1) { buf_puts(b, "(void)("); emit_boxed(c, argv[0], b); buf_puts(b, "); "); }
      buf_printf(b, "_t%d; })", t);
      return;
    }
  }
  /* h.default = v in value position: store when the receiver is a typed
     hash lvalue; a literal {} receiver just yields the value. */
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && !ty_is_hash(rt) &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
    TyKind vt = comp_ntype(c, argv[0]);
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, vt, b);
    buf_printf(b, " _t%d = ", t);
    emit_expr(c, argv[0], b);
    buf_printf(b, "; _t%d; })", t);
    return;
  }
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
  /* empty array literal [] has TY_UNKNOWN; sum returns init or 0. A local bound
     to [] that no push narrowed also stays TY_UNKNOWN, so `a = []; a.sum(0.0)`
     reaches here with a non-literal receiver -- still an empty array. */
  if (recv >= 0 && rt == TY_UNKNOWN && sp_streq(name, "sum")) {
    int is_lit = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode");
    int en = 0; if (is_lit) nt_arr(nt, recv, "elements", &en);
    if (!is_lit || en == 0) {
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
    if (sp_streq(name, "to_s") || sp_streq(name, "id2name")) {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    /* #name answers the frozen name string (to_s answers a mutable copy) */
    if (sp_streq(name, "name")) {
      buf_puts(b, "sp_str_uminus_val(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "))");
      return;
    }
    if (sp_streq(name, "inspect")) {
      buf_puts(b, "sp_sym_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
      return;
    }
    if (sp_streq(name, "to_sym") || sp_streq(name, "intern") || sp_streq(name, "itself")) { emit_expr(c, recv, b); return; }
    /* case-folding methods return a (re-interned) symbol */
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase")) {
      buf_printf(b, "sp_sym_intern(sp_str_%s(sp_sym_to_s(", name); emit_expr(c, recv, b); buf_puts(b, ")))");
      return;
    }
    if (sp_streq(name, "length") || sp_streq(name, "size")) {
      /* character count, not bytes (multibyte symbol names) */
      buf_puts(b, "sp_str_length(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "))");
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
    /* case-insensitive compare over the symbols' names; a non-symbol
       argument answers nil (evaluate both operands for side effects) */
    if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
        comp_ntype(c, argv[0]) != TY_SYMBOL) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      return;
    }
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
    /* to_i / to_int on a Bignum is self -- returning the full value, not the
       64-bit-truncated sp_bigint_to_int (#2319) */
    if ((sp_streq(name, "to_i") || sp_streq(name, "to_int")) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return;
    }
    if ((sp_streq(name, "magnitude") || sp_streq(name, "abs")) && argc == 0) {
      buf_printf(b, "sp_bigint_abs_v(%s)", r); free(rs.p); return;   /* (#2418) */
    }
    if (sp_streq(name, "abs2") && argc == 0) {
      buf_printf(b, "sp_bigint_mul(%s, %s)", r, r); free(rs.p); return;   /* (#2424) */
    }
    /* Bignum#downto(hi)/#upto(hi) with no block: materialize the Bignum sequence
       as a poly array (a Bignum range has no lazy Enumerator type) (#2305). */
    if ((sp_streq(name, "downto") || sp_streq(name, "upto")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) {
      int up = sp_streq(name, "upto");
      buf_printf(b, "sp_bigint_range_array(%s, ", r);
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_BIGINT) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_bigint_new_int("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ", %d)", up);
      free(rs.p); return;
    }
    /* Integer query / reflection on a Bignum receiver (#2318) */
    if (sp_streq(name, "zero?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) == 0)", r); free(rs.p); return;
    }
    if (sp_streq(name, "positive?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) > 0)", r); free(rs.p); return;
    }
    if (sp_streq(name, "negative?") && argc == 0) {
      buf_printf(b, "(sp_bigint_sign(%s) < 0)", r); free(rs.p); return;
    }
    if (sp_streq(name, "integer?") && argc == 0) {
      buf_printf(b, "((void)(%s), TRUE)", r); free(rs.p); return;
    }
    if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) {
      buf_printf(b, "sp_bigint_add(%s, sp_bigint_new_int(1))", r); free(rs.p); return;
    }
    if (sp_streq(name, "pred") && argc == 0) {
      buf_printf(b, "sp_bigint_sub(%s, sp_bigint_new_int(1))", r); free(rs.p); return;
    }
    if (sp_streq(name, "class") && argc == 0) {
      buf_printf(b, "((void)(%s), ((sp_Class){-100}))", r); free(rs.p); return;  /* Integer */
    }
    if (sp_streq(name, "bit_length") && argc == 0) {
      buf_printf(b, "sp_bigint_bit_length(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "even?") && argc == 0) {
      buf_printf(b, "sp_bigint_even_p(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "odd?") && argc == 0) {
      buf_printf(b, "(!sp_bigint_even_p(%s))", r); free(rs.p); return;
    }
    if (sp_streq(name, "abs") && argc == 0) {
      buf_printf(b, "sp_bigint_abs_v(%s)", r); free(rs.p); return;
    }
    /* round/ceil/floor: no precision -> self; a precision arg rounds to
       10^(-ndigits) (a positive precision is a no-op on an integer) (#2303) */
    if ((sp_streq(name, "round") || sp_streq(name, "ceil") || sp_streq(name, "floor")) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return;
    }
    if ((sp_streq(name, "round") || sp_streq(name, "ceil") || sp_streq(name, "floor")) && argc == 1) {
      int mode = sp_streq(name, "floor") ? 1 : sp_streq(name, "ceil") ? 2 : 0;
      buf_printf(b, "sp_bigint_round_prec(%s, ", r); emit_int_expr(c, argv[0], b);
      buf_printf(b, ", %d)", mode); free(rs.p); return;
    }
    if (sp_streq(name, "to_s") && argc == 1) {
      buf_printf(b, "sp_str_dup_external(sp_bigint_to_s_base(%s, ", r);
      emit_int_expr(c, argv[0], b); buf_puts(b, "))"); free(rs.p); return;
    }
    if (sp_streq(name, "digits") && argc <= 1) {
      /* least-significant first via repeated divmod -- any radix >= 2
         (the to_s(base) text path stops at 36) */
      int td = ++g_tmp, tb2 = ++g_tmp, tn2 = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", tb2);
      if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "10");
      buf_printf(b, "; if (_t%d < 2) sp_raise_cls(\"ArgumentError\", \"invalid radix\");", tb2);
      buf_printf(b, " mrb_int *_t%d = NULL; mrb_int _t%d = sp_bigint_digits_buf(%s, _t%d, &_t%d);", ti2, tn2, r, tb2, ti2);
      buf_printf(b, " if (_t%d < 0) sp_raise_cls(\"Math::DomainError\", \"out of domain\");", tn2);
      buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", td, td);
      buf_printf(b, " for (mrb_int _i = 0; _i < _t%d; _i++) sp_IntArray_push(_t%d, _t%d[_i]);", tn2, td, ti2);
      buf_printf(b, " free(_t%d); _t%d; })", ti2, td);
      return;
    }
    if (sp_streq(name, "to_f") && argc == 0) {
      buf_printf(b, "sp_bigint_to_double(%s)", r); free(rs.p); return;
    }
    /* Bignum-receiver methods that return an Integer/Float/bool without a
       Rational (those need a bigint-backed Rational and stay unsupported)
       (#2469). */
    if (sp_streq(name, "~") && argc == 0) { buf_printf(b, "sp_bigint_not(%s)", r); free(rs.p); return; }
    /* numerator/ord of an Integer is the value itself; denominator is 1 */
    if ((sp_streq(name, "numerator") || sp_streq(name, "ord")) && argc == 0) {
      buf_printf(b, "(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "denominator") && argc == 0) {
      buf_printf(b, "((void)(%s), (mrb_int)1)", r); free(rs.p); return;
    }
    if (sp_streq(name, "size") && argc == 0) {
      /* Integer#size is ceil(bit_length / 8); sp_bigint_byte_len rounds up to
         whole limbs, which overcounts (2**100 -> 16 not 13). */
      buf_printf(b, "((sp_bigint_bit_length(%s) + 7) / 8)", r); free(rs.p); return;
    }
    if (sp_streq(name, "nonzero?") && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; sp_bigint_sign(_t%d) != 0 ? sp_box_bigint(_t%d) : sp_box_nil(); })", t, r, t, t);
      free(rs.p); return;
    }
    if (sp_streq(name, "fdiv") && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      buf_printf(b, "(sp_bigint_to_double(%s) / ", r);
      if (at == TY_BIGINT) { buf_puts(b, "sp_bigint_to_double("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (at == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_puts(b, "(double)("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")"); free(rs.p); return;
    }
    if (sp_streq(name, "pow") && argc == 1) {
      buf_printf(b, "sp_bigint_pow(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return;
    }
    /* Bignum modulo/%/remainder/divmod/#[]/modular-pow (#2594) */
    if ((sp_streq(name, "modulo") || sp_streq(name, "%")) && argc == 1) {
      buf_printf(b, "sp_bigint_mod(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return;
    }
    if (sp_streq(name, "remainder") && argc == 1) {
      buf_printf(b, "sp_bigint_remainder(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return;
    }
    if (sp_streq(name, "divmod") && argc == 1) {
      int td = ++g_tmp, tb2 = ++g_tmp, to2 = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; sp_Bigint *_t%d = ", td, r, tb2);
      emit_bigint_operand(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_div(_t%d, _t%d)));"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_mod(_t%d, _t%d))); _t%d; })",
                 to2, to2, to2, td, tb2, to2, td, tb2, to2);
      free(rs.p); return;
    }
    if (sp_streq(name, "[]") && argc == 1) {
      int tn = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; _t%d < 0 ? (mrb_int)0"
                    " : sp_bigint_to_int(sp_bigint_and(sp_bigint_shr(%s, _t%d), sp_bigint_new_int(1))); })",
                 tn, r, tn);
      free(rs.p); return;
    }
    if (sp_streq(name, "pow") && argc == 2) {
      buf_printf(b, "sp_bigint_powmod(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", ");
      emit_bigint_operand(c, argv[1], b); buf_puts(b, ")");
      free(rs.p); return;
    }
    if ((sp_streq(name, "div") || sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1) {
      const char *fn = sp_streq(name, "div") ? "div" : name;
      buf_printf(b, "sp_bigint_%s(%s, ", fn, r);
      emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return;
    }
    if (sp_streq(name, "ceildiv") && argc == 1) {
      /* ceil division = -((-a) div b); div is floor division */
      buf_printf(b, "sp_bigint_sub(sp_bigint_new_int(0), sp_bigint_div(sp_bigint_sub(sp_bigint_new_int(0), %s), ", r);
      emit_bigint_operand(c, argv[0], b); buf_puts(b, "))");
      free(rs.p); return;
    }
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) && argc == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = ", t); emit_bigint_operand(c, argv[0], b); buf_puts(b, "; ");
      if (sp_streq(name, "allbits?"))
        buf_printf(b, "(sp_bigint_cmp(sp_bigint_and(%s, _t%d), _t%d) == 0); })", r, t, t);
      else if (sp_streq(name, "anybits?"))
        buf_printf(b, "(sp_bigint_sign(sp_bigint_and(%s, _t%d)) != 0); })", r, t);
      else
        buf_printf(b, "(sp_bigint_sign(sp_bigint_and(%s, _t%d)) == 0); })", r, t);
      free(rs.p); return;
    }
    if (sp_streq(name, "gcdlcm") && argc == 1) {
      int t = ++g_tmp, ta = ++g_tmp, tr = ++g_tmp;
      buf_printf(b, "({ sp_Bigint *_t%d = %s; sp_Bigint *_t%d = ", tr, r, t);
      emit_bigint_operand(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_gcd(_t%d, _t%d)));"
                    " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_lcm(_t%d, _t%d))); _t%d; })",
                 ta, ta, ta, tr, t, ta, tr, t, ta);
      free(rs.p); return;
    }
    /* to_r / rationalize on a Bignum -> Rational(self, 1); quo -> Rational(self,
       arg). The numerator exceeds mrb_int, so these produce a boxed big
       Rational (poly) rather than the by-value int Rational (#2469). */
    if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) {
      buf_printf(b, "sp_brat_from_bigint(%s)", r); free(rs.p); return;
    }
    if (sp_streq(name, "quo") && argc == 1) {
      buf_printf(b, "sp_box_brat(%s, ", r); emit_bigint_operand(c, argv[0], b); buf_puts(b, ")");
      free(rs.p); return;
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
  if (recv >= 0 && argc == 0 && sp_streq(name, "inspect") && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt)) {
    /* default Object#inspect: the generated per-class ivar walk */
    buf_printf(b, "sp_obj_inspect_sw(%d, (void *)(", ty_object_class(rt));
    emit_expr(c, recv, b); buf_puts(b, "))");
    return;
  }
  if (recv >= 0 && argc == 0 && sp_streq(name, "to_s") && ty_is_object(rt) &&
      !comp_ty_value_obj(c, rt)) {
    /* default Object#to_s: #<Name:0xADDR> */
    int dcid = ty_object_class(rt);
    const char *drn = class_ruby_name(c, dcid) ? class_ruby_name(c, dcid) : c->classes[dcid].name;
    buf_printf(b, "sp_sprintf(\"#<%s:0x%%016llx>\", (unsigned long long)(uintptr_t)(", drn);
    emit_expr(c, recv, b); buf_puts(b, "))");
    return;
  }
  /* The nil-degrade placeholders must still emit the receiver: a chain like
     `cell.id.inspect` whose receiver is itself an unresolved call then reaches
     that call's own diagnostic (a compile-time NoMethodError) instead of
     silently printing "[]" with the whole receiver dropped. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "inspect")) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), \"[]\")"); return;
  }
  if (recv >= 0 && argc == 0 && sp_streq(name, "to_s")) {
    buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), \"\")"); return;
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
      buf_puts(b, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++; if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) { ");
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
           frame: pop them (see the begin..ensure epilogue in codegen_stmt.c),
           and pop the sp_rescue_sp handler for each rescue body it leaves */
        { char g[24]; snprintf(g, sizeof g, "_retf%d", eid);
          if (emit_frame_unwind(b, 0, g)) buf_puts(b, " "); }
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
        grt == TY_STRING || grt == TY_FLOAT || grt == TY_BOOL ||
        grt == TY_COMPLEX || grt == TY_RATIONAL) {
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
        /* CRuby-shaped receiver text: a poly receiver names its runtime class
           through sp_nomethod_msg (evaluating the receiver once, as CRuby
           does before raising); statically-typed receivers get the static
           equivalent. TY_UNKNOWN keeps the old lattice name -- its receiver
           expression may not be independently emittable. */
        char rdesc[128];
        if (grt == TY_NIL) snprintf(rdesc, sizeof rdesc, "nil");
        else if (grt == TY_INT) snprintf(rdesc, sizeof rdesc, "an instance of Integer");
        else if (grt == TY_STRING) snprintf(rdesc, sizeof rdesc, "an instance of String");
        else if (grt == TY_FLOAT) snprintf(rdesc, sizeof rdesc, "an instance of Float");
        else if (grt == TY_COMPLEX) snprintf(rdesc, sizeof rdesc, "an instance of Complex");
        else if (grt == TY_RATIONAL) snprintf(rdesc, sizeof rdesc, "an instance of Rational");
        else snprintf(rdesc, sizeof rdesc, "%s", ty_name(grt));
        if (grt == TY_POLY || grt == TY_BOOL) {
          /* The RESULT slot is sized by the call's own type (ret), not the
             receiver's: a poly receiver whose unresolved call is typed to a
             concrete scalar (`poly.strftime` -> String) must yield that scalar,
             not the sp_RbVal token, or the token lands unconverted in a typed
             slot (#2451). Only a genuinely poly/unknown result keeps the bare
             token so the recognized-token coercion sites can still see it. */
          int ret_scalar = is_scalar_ret(ret) && ret != TY_UNKNOWN &&
                           ret != TY_POLY && ret != TY_VOID && ret != TY_NIL;
          /* stage the call's positional args for NoMethodError#args (#2837);
             a splat/block/kwarg shape keeps the plain message */
          int gac = 0; const int *gav = call_args(nt, id, &gac);
          int gstage = 1;
          for (int gk = 0; gk < gac; gk++) {
            const char *gty = nt_type(nt, gav[gk]);
            if (gty && (sp_streq(gty, "SplatNode") || sp_streq(gty, "BlockArgumentNode") ||
                        sp_streq(gty, "KeywordHashNode"))) gstage = 0;
          }
          #define EMIT_GATE_ARGS() do { \
            buf_printf(b, ", %d, (sp_RbVal[]){", gac); \
            for (int gk = 0; gk < gac; gk++) { if (gk) buf_puts(b, ", "); emit_boxed(c, gav[gk], b); } \
            if (gac == 0) buf_puts(b, "sp_box_nil()"); \
            buf_puts(b, "}"); \
          } while (0)
          if (sp_streq(dflt, "sp_box_nil()") && !ret_scalar) {
            buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg%s(\"%s\", ",
                       gstage ? "_args" : "", nm ? nm : "?");
            emit_boxed(c, recv, b);
            if (gstage) EMIT_GATE_ARGS();
            buf_puts(b, "))");
          }
          else {
            buf_printf(b, "(sp_raise_cls(\"NoMethodError\", sp_nomethod_msg%s(\"%s\", ",
                       gstage ? "_args" : "", nm ? nm : "?");
            emit_boxed(c, recv, b);
            if (gstage) EMIT_GATE_ARGS();
            buf_printf(b, ")), %s)", ret_scalar ? default_value(ret) : dflt);
          }
          return;
        }
        {
          int gac = 0; const int *gav = call_args(nt, id, &gac);
          int gstage = 1;
          for (int gk = 0; gk < gac; gk++) {
            const char *gty = nt_type(nt, gav[gk]);
            if (gty && (sp_streq(gty, "SplatNode") || sp_streq(gty, "BlockArgumentNode") ||
                        sp_streq(gty, "KeywordHashNode"))) gstage = 0;
          }
          #define EMIT_GATE_MSG() do { \
            if (gstage) { \
              buf_printf(b, "sp_stage_args_msg(\"undefined method '%s' for %s\", %d, (sp_RbVal[]){", \
                         nm ? nm : "?", rdesc, gac); \
              for (int gk = 0; gk < gac; gk++) { if (gk) buf_puts(b, ", "); emit_boxed(c, gav[gk], b); } \
              if (gac == 0) buf_puts(b, "sp_box_nil()"); \
              buf_puts(b, "})"); \
            } \
            else buf_printf(b, "\"undefined method '%s' for %s\"", nm ? nm : "?", rdesc); \
          } while (0)
          if (sp_streq(dflt, "sp_box_nil()")) {
            buf_puts(b, "sp_raise_nomethod(");
            EMIT_GATE_MSG();
            buf_puts(b, ")");
          }
          else {
            buf_puts(b, "(sp_raise_cls(\"NoMethodError\", ");
            EMIT_GATE_MSG();
            buf_printf(b, "), %s)", dflt);
          }
          #undef EMIT_GATE_MSG
          #undef EMIT_GATE_ARGS
        }
        return;
      }
      buf_puts(b, dflt);
      return;
    }
  }
  unsupported(c, id, "call");
}
