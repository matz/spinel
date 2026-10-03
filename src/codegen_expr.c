#include "codegen_internal.h"

/* defined? support: does this subtree reference a constant the compiler
   cannot resolve? Any such reference makes the whole defined? answer nil
   (CRuby checks every reference without evaluating). */
static int subtree_has_unresolved_const(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "ConstantReadNode")) {
    const char *cn = nt_str(c->nt, id, "name");
    if (!cn) return 0;
    if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) return 0;
    if (comp_is_wellknown_const(cn)) return 0;
    return 1;
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_unresolved_const(c, nt_ref_at(c->nt, id, i))) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (subtree_has_unresolved_const(c, ids[k])) return 1;
  }
  return 0;
}

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

/* Single-buffer construction: every part is either an escaped literal
   (compile-time length), a bounded-width scalar (int <= 21 digits, bool
   <= 5), or a dynamic part pre-evaluated -- in part order, preserving
   side-effect order -- into a rooted `const char *` temp whose byte length
   feeds the capacity sum. One sp_str_alloc_raw then serves the whole
   string; sp_w_* writers append each part. (The previous lowering built
   one heap string per part and ran an sp_sprintf pass over them.)
   The plan is shared with the append form (`s << "..#{x}.."`), which
   writes the same parts into the receiver instead of a fresh string. */
enum { WK_LIT, WK_INT, WK_BOOL, WK_NIL, WK_DYN };
typedef struct { int kind; int tmp; int lit_off; int lit_esc_len; long lit_len; } WPart;
typedef struct {
  WPart *wp; int nwp; int ndyn_or_scalar;
  Buf lits;      /* escaped literal texts, concatenated */
  Buf decls;     /* the pre-evaluated temps, as statements */
  long fixed_cap;
  int *flat;
} InterpPlan;
static void interp_plan_free(InterpPlan *pl) {
  free(pl->lits.p); free(pl->decls.p); free(pl->wp); free(pl->flat);
}
static void interp_plan(Compiler *c, int id, InterpPlan *pl) {
  const NodeTable *nt = c->nt;
  int n = 0;
  int *flat = NULL, fcap = 0;
  interp_flatten(nt, id, &flat, &n, &fcap);
  const int *parts = flat;
  WPart *wp = malloc(sizeof(WPart) * (size_t)(n > 0 ? n : 1));
  if (!wp) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  int nwp = 0, ndyn_or_scalar = 0;
  Buf lits; memset(&lits, 0, sizeof lits);
  Buf decls; memset(&decls, 0, sizeof decls);
  long fixed_cap = 0;

  for (int k = 0; k < n; k++) {
    int pid = parts[k];
    const char *pty = nt_type(nt, pid);
    if (pty && sp_streq(pty, "StringNode")) {
      const char *content = nt_str(nt, pid, "content");
      int off = lits.len;
      long blen = 0;
      /* the node's own byte length, not strlen: an adjacent-literal part
         holding a NUL (`"\0" "1"`) stopped the walk and the byte was
         dropped from the fold (the opportunistic NUL policy) */
      size_t clen = content ? nt_str_len(nt, pid, "content") : 0;
      for (size_t ci = 0; ci < clen; ci++, blen++) {
        unsigned char ch = (unsigned char)content[ci];
        if (ch == '\\') buf_puts(&lits, "\\\\");
        else if (ch == '"') buf_puts(&lits, "\\\"");
        else if (ch == '\n') buf_puts(&lits, "\\n");
        else if (ch == '\t') buf_puts(&lits, "\\t");
        else if (ch == '\r') buf_puts(&lits, "\\r");
        else if (ch >= 0x20 && ch < 0x7f) buf_printf(&lits, "%c", ch);
        else buf_printf(&lits, "\\%03o", ch);
      }
      wp[nwp].kind = WK_LIT; wp[nwp].tmp = -1;
      wp[nwp].lit_off = off; wp[nwp].lit_esc_len = lits.len - off;
      wp[nwp].lit_len = blen;
      nwp++;
      fixed_cap += blen;
    }
    else if (pty && sp_streq(pty, "EmbeddedStatementsNode")) {
      int st = nt_ref(nt, pid, "statements");
      int bn = 0;
      const int *body = st >= 0 ? nt_arr(nt, st, "body", &bn) : NULL;
      int expr = bn > 0 ? body[bn - 1] : -1;
      /* `#{ s1; s2; ...; sN }` evaluates every statement in order and uses sN's
         value. Run the leading statements for side effects; if sN is itself an
         assignment, perform it and read the assigned variable back as the value. */
      char vexpr[48]; vexpr[0] = 0;
      /* ...into this part's place in `decls`, in order with the parts around
         it: hoisted to g_pre they ran before the whole enclosing expression,
         so `"#{x}" + "#{x = "b"}"` read x after the assignment (#5574) */
      for (int si = 0; si + 1 < bn; si++) emit_stmt(c, body[si], &decls, 0);
      const char *ety = expr >= 0 ? nt_type(nt, expr) : NULL;
      TyKind t = comp_ntype(c, expr);
      /* Interpolation is `to_s`, so a program that REOPENED the part's class
         with its own to_s owns the conversion: `class Integer; def to_s(base
         = 10); "INT"; end` makes "x#{5}y" read "xINTy" in CRuby, where the
         table below would write the digits. Hand such a part to the ordinary
         call path -- the reopen dispatch there answers it -- by rendering
         `<part>.to_s` rather than converting in place. */
      {
        /* A String part is NOT one of them: CRuby's interpolation uses a
           String value as it stands (objtostring's own fast path) and never
           calls to_s on it, so a reopened String#to_s does not change
           `"t=#{"ab"}"`. It does change an explicit `"ab".to_s`, which the
           call path above already answers. */
        const char *rcn = t == TY_INT ? "Integer" : t == TY_FLOAT ? "Float"
                        : t == TY_SYMBOL ? "Symbol" : NULL;
        if (rcn && expr >= 0) {
          int rci = comp_class_index(c, rcn);
          if (rci >= 0 && comp_method_in_chain(c, rci, "to_s", NULL) >= 0) {
            int tsc = nt_new_node((NodeTable *)nt, "CallNode");
            if (tsc >= 0) {
              nt_node_set_str((NodeTable *)nt, tsc, "name", "to_s");
              nt_node_set_ref((NodeTable *)nt, tsc, "receiver", expr);
              nt_node_set_ref((NodeTable *)nt, tsc, "arguments", -1);
              nt_node_set_ref((NodeTable *)nt, tsc, "block", -1);
              comp_grow_node_arrays(c);
              c->nscope[tsc] = c->nscope[expr];
              expr = tsc;
              t = TY_STRING;
            }
          }
        }
      }
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
        emit_stmt(c, expr, &decls, 0);
        const char *vn = nt_str(nt, expr, "name");
        LocalVar *lvp = vn ? scope_local(comp_scope_of(c, expr), vn) : NULL;
        if (lvp) t = lvp->type;
        int tv = ++g_tmp;
        emit_ctype(c, t, &decls);
        buf_printf(&decls, " _t%d = ", tv); emit_local_ref(c, expr, vn, &decls); buf_puts(&decls, "; ");
        snprintf(vexpr, sizeof vexpr, "_t%d", tv);
      }
      /* Build this part's conversion expression. Bounded scalars keep their
         native C type (written digit-by-digit later); everything else
         converts to a marker-carrying string whose byte length is summed. */
      Buf conv; memset(&conv, 0, sizeof conv);
      int wkind = WK_DYN;
      char *iv_pre = NULL;   /* the value's text, emitted once by the probe below */
      #define EMIT_IV() do { if (vexpr[0]) buf_puts(&conv, vexpr); \
                             else if (iv_pre) buf_puts(&conv, iv_pre); \
                             else emit_expr(c, expr, &conv); } while (0)
      /* A part that DIVERGES: the raise emitters close their expression with a
         dead placeholder of whatever type the node had, and the arm chosen
         from that node's type may disagree with it -- an unresolvable constant
         read renders `(sp_raise_cls(...), (sp_Class){-1})` while the node types
         poly, so sp_poly_to_s took an sp_Class (#4092). Discard it and answer
         the empty string; the raise means nothing reads the answer. */
      { Buf ivp; memset(&ivp, 0, sizeof ivp);
        int ivp_emitted = 0;
        if (vexpr[0]) buf_puts(&ivp, vexpr);
        else { emit_expr(c, expr, &ivp); ivp_emitted = 1; }
        const char *ivt = ivp.p ? ivp.p : "";
        /* a call to a method that always raises diverges the same way, and
           its void C form has no arm to take (it was refused) */
        if (strncmp(past_open_parens(ivt), "sp_raise_", 9) == 0 ||
            (!vexpr[0] && call_never_returns(c, expr))) {
          buf_printf(&conv, "((void)(%s), sp_str_empty)", ivt);
          free(ivp.p);
          goto iv_done;
        }
        /* The probe EMITTED the value; emitting it again for the real arm
           would run its g_pre hoists and temp numbering twice. Hand the text
           to EMIT_IV instead, the way a pre-evaluated temp already does. */
        if (ivp_emitted && ivp.p) { free(iv_pre); iv_pre = ivp.p; }
        else free(ivp.p); }
      if (t == TY_INT) {
        /* nil-aware: an int slot holding the nil sentinel writes nothing */
        wkind = WK_INT;
        EMIT_IV();
      }
      else if (t == TY_STRING) {
        /* a nullable string (NULL) interpolates as the empty string */
        buf_puts(&conv, "sp_str_or_empty("); EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_FLOAT) {
        /* and a float slot's nil sentinel writes nothing too */
        buf_puts(&conv, "sp_float_opt_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_BOOL) {
        wkind = WK_BOOL;
        EMIT_IV();
      }
      else if (t == TY_SYMBOL) {
        buf_puts(&conv, "sp_sym_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_POLY) {
        buf_puts(&conv, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_EXCEPTION) {
        buf_puts(&conv, "sp_exc_message(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_NIL) {
        wkind = WK_NIL;
        EMIT_IV();
      }
      else if (t == TY_RATIONAL) {
        buf_puts(&conv, "sp_rational_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_COMPLEX) {
        buf_puts(&conv, "sp_complex_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_TIME) {
        buf_puts(&conv, "sp_time_to_s_v(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      /* a Range interpolates as its to_s, the same renderers Range#to_s
         lowers to; with no arm every Range kind was refused (#4824) */
      else if (t == TY_RANGE) {
        buf_puts(&conv, "sp_range_str(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_FLOAT_RANGE) {
        buf_puts(&conv, "sp_frange_inspect(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_STR_RANGE) {
        buf_puts(&conv, "sp_srange_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      /* a Regexp, Array or Hash slot's nil is NULL, which interpolates as
         the empty string the way a nullable String's does */
      else if (t == TY_REGEX || t == TY_POLY_ARRAY || (ty_is_array(t) && array_kind(t)) ||
               (ty_is_hash(t) && ty_hash_cname(t))) {
        int ntv = ++g_tmp;
        buf_puts(&conv, "({ "); emit_ctype(c, t, &conv);
        buf_printf(&conv, " _t%d = ", ntv);
        EMIT_IV();
        buf_printf(&conv, "; _t%d ? ", ntv);
        if (t == TY_REGEX) buf_printf(&conv, "sp_re_to_s_str((void *)_t%d)", ntv);
        else if (t == TY_POLY_ARRAY) buf_printf(&conv, "sp_PolyArray_inspect(_t%d)", ntv);
        else if (ty_is_hash(t)) buf_printf(&conv, "sp_%sHash_inspect(_t%d)", ty_hash_cname(t), ntv);
        else buf_printf(&conv, "sp_%sArray_inspect(_t%d)", array_kind(t), ntv);
        buf_puts(&conv, " : sp_str_empty; })");
      }
      else if (ty_is_object(t) && obj_str_cname(c, ty_object_class(t), 0)) {
        const char *cn = obj_str_cname(c, ty_object_class(t), 0);
        /* a poly-returning to_s (String unioned with e.g. nil) yields a boxed
           sp_RbVal -- render it through sp_poly_to_s so it's a const char *
           in the interpolation (#3266). */
        int ret_poly = obj_str_ret_poly(c, ty_object_class(t), 0);
        if (ret_poly) buf_puts(&conv, "sp_poly_to_s(");
        /* a value-type object (single-ivar) has a by-VALUE to_s signature;
           casting its receiver to a pointer is a C type error (#2357) */
        if (comp_ty_value_obj(c, t) || (!vexpr[0] && !iv_pre && expr_is_held_ref(c, expr))) {
          if (comp_ty_value_obj(c, t)) buf_printf(&conv, "sp_%s_to_s(", cn);
          else buf_printf(&conv, "sp_%s_to_s((sp_%s *)", cn, cn);
          EMIT_IV(); buf_puts(&conv, ")");
        }
        else {
          /* rooted: the part may be a fresh object (`"#{C.new}"`) held
             nowhere else while its #to_s allocates */
          int to = ++g_tmp;
          buf_printf(&conv, "({ sp_%s *_t%d = (sp_%s *)(", cn, to, cn);
          EMIT_IV();
          buf_printf(&conv, "); SP_GC_ROOT(_t%d); sp_%s_to_s(_t%d); })", to, cn, to);
        }
        if (ret_poly) buf_puts(&conv, ")");
      }
      else if (ty_is_ptr_array(t) || ty_is_obj_array(t)) {
        Buf ivb; memset(&ivb, 0, sizeof ivb);
        if (vexpr[0]) buf_puts(&ivb, vexpr);
        else if (iv_pre) buf_puts(&ivb, iv_pre);
        else emit_expr(c, expr, &ivb);
        buf_puts(&conv, "sp_poly_inspect(");
        emit_boxed_text(c, t, ivb.p ? ivb.p : "NULL", &conv);
        buf_puts(&conv, ")");
        free(ivb.p);
      }
      else if (t == TY_CLASS) {
        buf_puts(&conv, "sp_class_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (t == TY_BIGINT) {
        /* NULL is this slot's nil, which interpolates as the empty string
           the way a nullable String's does (#4800). */
        int bgv = ++g_tmp;
        buf_printf(&conv, "({ sp_Bigint *_t%d = ", bgv);
        EMIT_IV();
        buf_printf(&conv, "; _t%d ? sp_bigint_to_s(_t%d) : sp_str_empty; })", bgv, bgv);
      }
      else if (t == TY_IO || t == TY_DIR) {
        /* the IO family has Object's to_s (the protocol arm's render) */
        buf_puts(&conv, t == TY_IO ? "sp_io_to_s(" : "sp_Dir_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else if (ty_nullable_builtin_id(t)) {
        /* a reference-backed builtin handle (Proc, Fiber, Thread, ...):
           renders as its #inspect form rather than refusing the interpolation.
           CRuby's #to_s for these prints the object address, which the
           runtime does not reproduce for them (the IO family above has it);
           the #inspect form is the same shape and actually informative
           (#3539). */
        buf_puts(&conv, "sp_poly_inspect(sp_box_nullable_obj((void *)(");
        EMIT_IV(); buf_printf(&conv, "), %s))", ty_nullable_builtin_id(t));
      }
      else if (ty_is_object(t)) {
        /* user object without to_s: fall back to poly_to_s of boxed value */
        int cid2 = ty_object_class(t);
        buf_printf(&conv, "sp_poly_to_s(sp_box_obj(");
        EMIT_IV(); buf_printf(&conv, ", %d))", cid2);
      }
      else if (t == TY_UNKNOWN) {
        /* An untyped value (e.g. a method resolved only through an included
           module) is already emitted as a boxed sp_RbVal, so stringify it the
           same way as a poly value rather than rejecting the interpolation. */
        buf_puts(&conv, "sp_poly_to_s(");
        EMIT_IV(); buf_puts(&conv, ")");
      }
      else {
        free(conv.p); free(lits.p); free(decls.p); free(wp); free(flat);
        unsupported(c, pid, "interpolation value");
      }
      iv_done:
      free(iv_pre);
      #undef EMIT_IV
      /* Pre-evaluate into an ordered temp. A dynamic part's string is rooted
         (its bytes are copied after later parts may allocate, and the final
         allocation itself can collect); bounded scalars need none.
         Collected into `decls` and emitted as a leading sequence of a
         statement-expression below -- NOT into g_pre, since emit_interp may
         run while g_pre holds a half-written enclosing statement.
         The length is the BYTE length. It used to be strlen, on the reason
         that a string value can be a foreign C literal with no sp_str_hdr
         and no marker byte (lib raise messages, class names, "" fallbacks)
         -- but strlen stops at an embedded NUL, so `"#{bytes}"` silently
         dropped everything past the first one, and interpolation is how a
         program assembles bytes (#4632). sp_str_byte_len answers strlen for
         exactly those foreign strings: it reads the marker byte first and
         only trusts a header behind one of the five it knows. That is the
         same test every other binary-safe path here makes (the write and
         puts operands, String#==, the formatter's %s). */
      int tv2 = ++g_tmp;
      if (wkind == WK_INT) {
        buf_printf(&decls, "sp_int _t%d = %s; ", tv2, conv.p ? conv.p : "0");
        fixed_cap += 21;  /* SP_W_INT_MAX: -9223372036854775808 */
      }
      else if (wkind == WK_BOOL) {
        buf_printf(&decls, "sp_bool _t%d = %s; ", tv2, conv.p ? conv.p : "0");
        fixed_cap += 5;
      }
      else if (wkind == WK_NIL) {
        buf_printf(&decls, "(void)(%s); ", conv.p ? conv.p : "0");
        tv2 = -1;
      }
      else {
        buf_printf(&decls, "const char *_t%d = %s; SP_GC_ROOT(_t%d); size_t _l%d = sp_str_byte_len(_t%d); ",
                   tv2, conv.p ? conv.p : "sp_str_empty", tv2, tv2, tv2);
      }
      free(conv.p);
      wp[nwp].kind = wkind; wp[nwp].tmp = tv2;
      wp[nwp].lit_off = 0; wp[nwp].lit_esc_len = 0; wp[nwp].lit_len = 0;
      nwp++;
      ndyn_or_scalar++;
    }
    else {
      free(lits.p); free(decls.p); free(wp); free(flat);
      unsupported(c, pid, "interpolation part");
    }
  }
  pl->wp = wp; pl->nwp = nwp; pl->ndyn_or_scalar = ndyn_or_scalar;
  pl->lits = lits; pl->decls = decls; pl->fixed_cap = fixed_cap; pl->flat = flat;
}

void emit_interp(Compiler *c, int id, Buf *b) {
  InterpPlan pl; memset(&pl, 0, sizeof pl);
  interp_plan(c, id, &pl);
  WPart *wp = pl.wp; int nwp = pl.nwp, ndyn_or_scalar = pl.ndyn_or_scalar;
  Buf lits = pl.lits, decls = pl.decls; long fixed_cap = pl.fixed_cap; int *flat = pl.flat;

  if (ndyn_or_scalar == 0) {
    /* adjacent literals ("a" "b") fold to one literal: frozen per the
       InterpolatedStringNode's own file pragma flag. A frozen fold carries
       the full static header object -- the bare "\xf1" prefix promises an
       sp_str_hdr that would not exist (#1749 family). */
    if (nt_int(c->nt, id, "fzl", 0)) {
      size_t raw3 = 0;
      for (int k = 0; k < nwp; k++) raw3 += (size_t)wp[k].lit_len;
      int fid3 = emit_frozen_literal_open(b, raw3);
      for (int k = 0; k < nwp; k++)
        buf_printf(b, "%.*s", wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off);
      emit_frozen_literal_close(b, fid3);
    }
    else {
      buf_puts(b, "(&(\"\\xff\" \"");
      for (int k = 0; k < nwp; k++)
        buf_printf(b, "%.*s", wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off);
      buf_puts(b, "\")[1])");
    }
    free(lits.p); free(decls.p); free(wp); free(flat);
    return;
  }

  int rid = ++g_tmp, wpid = ++g_tmp;
  buf_printf(b, "({ %s", decls.p ? decls.p : "");
  buf_printf(b, "size_t _cap%d = %ldUL", rid, fixed_cap);
  for (int k = 0; k < nwp; k++)
    if (wp[k].kind == WK_DYN) buf_printf(b, " + _l%d", wp[k].tmp);
  buf_puts(b, "; ");
  buf_printf(b, "char *_t%d = sp_str_alloc_raw(_cap%d + 1); ", rid, rid);
  buf_printf(b, "char *_t%d = _t%d; ", wpid, rid);
  for (int k = 0; k < nwp; k++) {
    switch (wp[k].kind) {
      case WK_LIT:
        if (wp[k].lit_len > 0)
          buf_printf(b, "memcpy(_t%d, \"%.*s\", %ld); _t%d += %ld; ",
                     wpid, wp[k].lit_esc_len, (lits.p ? lits.p : "") + wp[k].lit_off,
                     wp[k].lit_len, wpid, wp[k].lit_len);
        break;
      case WK_INT:
        buf_printf(b, "_t%d = sp_w_int(_t%d, _t%d); ", wpid, wpid, wp[k].tmp);
        break;
      case WK_BOOL:
        buf_printf(b, "_t%d = sp_w_bool(_t%d, _t%d); ", wpid, wpid, wp[k].tmp);
        break;
      case WK_NIL:
        break;
      default:
        buf_printf(b, "memcpy(_t%d, _t%d, _l%d); _t%d += _l%d; ",
                   wpid, wp[k].tmp, wp[k].tmp, wpid, wp[k].tmp);
        break;
    }
  }
  buf_printf(b, "*_t%d = 0; sp_str_set_len(_t%d, (size_t)(_t%d - _t%d)); (const char *)_t%d; })",
             wpid, rid, wpid, rid, rid);
  free(lits.p); free(decls.p); free(wp); free(flat);
}

/* `s << "lit #{x} lit"`: the interpolation's parts appended to `s` one by
   one, with no intermediate string. The value form above builds the whole
   string in a fresh buffer and the append then copies it again into `s`; a
   template that appends a page-sized interpolation copied the page once per
   nesting level (campfire's room page moved seven times its size through
   memmove per request). The parts are pre-evaluated into rooted temps first,
   in order, exactly as the value form does, so every side effect and every
   raise happens before the first byte lands in `s`; a part that aliases `s`
   itself (`s << "#{s}"`) is appended by the length taken at evaluation, so an
   earlier append growing `s` in place cannot change what it contributes.
   `open`/`close` bracket each part's append statement: open(receiver text)
   ... part ... close; a NULL length-taking variant is spelled by the caller
   through `open_n`/`close_n` when it has one. Answers 0 when the plan has no
   dynamic part (a literal fold, left to the plain path). */
int emit_interp_append(Compiler *c, int id, const char *open, const char *open_n, Buf *b, int indent) {
  InterpPlan pl; memset(&pl, 0, sizeof pl);
  interp_plan(c, id, &pl);
  if (pl.ndyn_or_scalar == 0) { interp_plan_free(&pl); return 0; }
  emit_indent(b, indent);
  buf_printf(b, "{ %s\n", pl.decls.p ? pl.decls.p : "");
  for (int k = 0; k < pl.nwp; k++) {
    WPart *w = &pl.wp[k];
    if (w->kind == WK_NIL) continue;
    if (w->kind == WK_LIT && w->lit_len == 0) continue;
    emit_indent(b, indent + 1);
    switch (w->kind) {
      case WK_LIT:
        buf_printf(b, "%s(&(\"\\xff\" \"%.*s\")[1]), %ldUL);\n", open_n,
                   w->lit_esc_len, (pl.lits.p ? pl.lits.p : "") + w->lit_off, w->lit_len);
        break;
      case WK_INT:
        buf_printf(b, "%s_t%d == SP_INT_NIL ? sp_str_empty : sp_int_to_s(_t%d));\n", open, w->tmp, w->tmp);
        break;
      case WK_BOOL:
        buf_printf(b, "%s_t%d ? SPL(\"true\") : SPL(\"false\"));\n", open, w->tmp);
        break;
      default:
        buf_printf(b, "%s_t%d, _l%d);\n", open_n, w->tmp, w->tmp);
        break;
    }
  }
  emit_indent(b, indent); buf_puts(b, "}\n");
  interp_plan_free(&pl);
  return 1;
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
    /* Ruby shifts by a negative count shift the other way; a C shift by a
       negative (or >= width) count is UB. Fold the direction-flipped and
       saturated cases explicitly; bail on a large `<<` (Ruby promotes to
       Bignum -- not representable as a folded int). */
    if (sp_streq(nm, "<<")) {
      if (bb2 < 0) { long long s = -bb2; *out = s >= 64 ? (a < 0 ? -1 : 0) : (a >> s); return 1; }
      if (bb2 >= 64) return 0;
      *out = a << bb2; return 1;
    }
    if (sp_streq(nm, ">>")) {
      if (bb2 < 0) { long long s = -bb2; if (s >= 64) return 0; *out = a << s; return 1; }
      *out = bb2 >= 64 ? (a < 0 ? -1 : 0) : (a >> bb2); return 1;
    }
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
  /* The callers hand this straight to sp_imod, whose divisor is an sp_int.
     A boxed operand is an sp_RbVal struct, so emitting it raw did not
     produce a wrong number -- it produced C that does not compile, and
     `17.modulo(x)` stopped building the moment x was boxed. The strict
     conversion is the one the sibling arms use for the same position
     (Integer#remainder reaches sp_iremainder through it), so a boxed value
     that is not an integer raises CRuby's TypeError rather than being read
     as one. */
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_arg_int_chk("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  emit_expr(c, node, b);
}

/* True if `root` contains a `break <value>` that binds to the enclosing loop
   (not one inside a nested loop, block, lambda, or def/class/module scope, which
   capture their own break -- but a break in a block-bearing call's receiver or
   arguments does bind to the loop, so those are still traversed). A valued break
   makes the loop's value the break value rather than nil; detect it so a loop in
   value position rejects instead of silently yielding nil. */
int loop_has_valued_break(Compiler *c, int root) {
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

/* A receiverless `raise`/`fail` call: it diverges, so in a value position it
   yields no value (its C form is void). A branch whose value is such a call
   must be emitted as a statement, not assigned to the result temp. */
static int node_is_raise(Compiler *c, int nd) {
  const NodeTable *nt = c->nt;
  if (nd < 0 || !nt_type(nt, nd) || !sp_streq(nt_type(nt, nd), "CallNode")) return 0;
  if (nt_ref(nt, nd, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, nd, "name");
  return nm && (sp_streq(nm, "raise") || sp_streq(nm, "fail"));
}

/* One arm of a value-position if/unless: box a concrete arm into a poly
   result, and give empty []/{} literals the result's container type. */
/* An arm already rendered as TEXT, carried into a BIGINT result temp. The
   poly cases beside each call box; this one wraps, because the temp is an
   sp_Bigint * and a plain Integer arm is otherwise assigned into it as a
   pointer -- the same defect #4794 fixed for the arms that go through
   emit_ternary_arm, in the paths that do not. Answers 1 when it emitted. */
static int emit_arm_text_as_bigint(TyKind res, TyKind at, const char *txt, Buf *out) {
  if (res != TY_BIGINT || at == TY_BIGINT) return 0;
  buf_printf(out, "%s(%s)", at == TY_POLY ? "sp_poly_as_bigint" : "sp_bigint_new_int",
             txt && txt[0] ? txt : "0");
  return 1;
}
static void emit_ternary_arm(Compiler *c, int nd, TyKind res, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *bty = nt_type(nt, nd);
  if (res == TY_POLY && comp_ntype(c, nd) != TY_POLY) { emit_boxed(c, nd, b); return; }
  /* An arm with no C VALUE cannot sit in a C conditional beside a typed
     sibling: a method whose body is `nil` compiles to a void function, and
     `a && a.analyze` put that call straight into one arm of `? :` whose other
     arm is an sp_Att *. Sequence the call and produce the sibling type's nil
     -- NULL for a pointer, which IS nil there -- the same shape as the raise
     coercion below. The poly arm above already handles it by boxing, which is
     why only a TYPED sibling ever saw this (#4163). */
  { TyKind at = comp_ntype(c, nd);
    if ((at == TY_VOID || at == TY_NIL) && res != TY_POLY && res != TY_UNKNOWN &&
        res != TY_VOID && res != TY_NIL) {
      Buf ab; memset(&ab, 0, sizeof ab);
      emit_expr(c, nd, &ab);
      const char *nv = nil_value(res);
      if (!nv) nv = default_value_from_compiler(c, res);
      if (!nv) nv = "0";
      /* a bare `nil` arm emits nothing at all; only a call needs sequencing */
      if (ab.p && ab.p[0]) buf_printf(b, "(%s, %s)", ab.p, nv);
      else buf_puts(b, nv);
      free(ab.p);
      return;
    }
  }
  /* A BIGINT result with an arm that is not one. The arm is a value, not a
     pointer, and C put it straight into the sp_Bigint * slot the sibling arm
     types: `return c ? 9223372036854775808 : 9223372036854775807` returned
     the second literal AS a pointer, and the first read of it segfaulted.
     The C compiler does say so -- `pointer/integer type mismatch in
     conditional expression` -- but the generated TU is built with -Wall off,
     so it is a warning and the build completes.

     The implicit-tail form never showed it: there each branch returns on its
     own and takes the return path's int->bigint wrap. Only an arm sitting in
     a C conditional beside a bigint sibling reaches here. */
  if (res == TY_BIGINT && comp_ntype(c, nd) != TY_BIGINT) {
    emit_bigint_operand_ext(c, nd, b);
    return;
  }
  if ((ty_is_array(res) || ty_is_hash(res)) && bty && sp_streq(bty, "CallNode") &&
      comp_ntype(c, nd) == TY_UNKNOWN && emit_empty_container_for_slot(c, nd, res, b))
    return;
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
  {
    Buf ab; memset(&ab, 0, sizeof ab);
    emit_expr(c, nd, &ab);
    /* An arm that compiles to a NoMethodError raise (e.g. `u.details` where u is
       unresolvable) evaluates to sp_RbVal but never returns; the sibling arm has
       the concrete result type, so coerce the raise to it -- `(raise, default)`
       -- to keep the C ternary's two arms the same type (#2949). */
    if (ab.p && strncmp(ab.p, "sp_raise_nomethod(", 18) == 0 &&
        res != TY_POLY && res != TY_UNKNOWN && res != TY_VOID) {
      buf_printf(b, "(%s, %s)", ab.p, default_value_from_compiler(c, res));
    }
    else buf_puts(b, ab.p ? ab.p : "");
    free(ab.p);
  }
}

/* Effect-free simple read whose re-evaluation is safe (and cheap): gates the
   IndexOperatorWriteNode expression form below, which re-reads receiver and
   key after the mutation. */
static int idx_opw_node_is_cheap(const NodeTable *nt, int node) {
  const char *t = nt_type(nt, node);
  return t && (sp_streq(t, "LocalVariableReadNode") ||
               sp_streq(t, "InstanceVariableReadNode") ||
               sp_streq(t, "IntegerNode") || sp_streq(t, "FloatNode") ||
               sp_streq(t, "StringNode") || sp_streq(t, "SymbolNode"));
}

/* Emit a read of recv[key] for the receiver shapes emit_index_op_write
   handles, yielding the slot's static type (which the new infer rule reports
   as the op-write expression's type, so the two always agree). */
static void emit_index_get(Compiler *c, int recv, int key, Buf *b) {
  TyKind rt = comp_ntype(c, recv);
  if (ty_is_hash(rt) && ty_hash_cname(rt)) {
    buf_printf(b, "sp_%sHash_get(", ty_hash_cname(rt));
    if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    if (g_iow_key_ref) buf_puts(b, g_iow_key_ref); else emit_hash_key(c, key, ty_hash_key(rt), b);
    buf_puts(b, ")");
    return;
  }
  if (ty_is_array(rt)) {
    const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
    buf_printf(b, "sp_%sArray_get(", k ? k : "Poly");
    if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
    buf_puts(b, ", ");
    if (g_iow_key_ref) buf_puts(b, g_iow_key_ref); else emit_int_expr(c, key, b);
    buf_puts(b, ")");
    return;
  }
  /* TY_POLY receiver: dispatch by key type, mirroring emit_index_op_write */
  TyKind kt = comp_ntype(c, key);
  /* A receiver proved to hold only a poly array or nil needs none of the
     dispatch: analyze established that for the root elision, and the read is
     hot enough in optcarrot's per-pixel path to be worth spending it on. */
  const char *fn = kt == TY_SYMBOL ? "sp_poly_get_sym" :
                   kt == TY_STRING ? "sp_poly_get_str" :
                   kt == TY_INT    ? (expr_is_arr_or_nil(c, recv) ? "sp_poly_arr_get_aon"
                                                                  : "sp_poly_arr_get_hash")
                                   : "sp_poly_index_poly";
  buf_printf(b, "%s(", fn);
  if (g_iow_recv_ref) buf_puts(b, g_iow_recv_ref); else emit_expr(c, recv, b);
  buf_puts(b, ", ");
  if (g_iow_key_ref) buf_puts(b, g_iow_key_ref);
  else if (kt == TY_SYMBOL || kt == TY_STRING) emit_expr(c, key, b);
  else if (kt == TY_INT) emit_int_expr(c, key, b);
  else emit_boxed(c, key, b);
  buf_puts(b, ")");
}

/* `h[k] ||= v` stores when the slot is nil or false, `&&=` when it is not, and
   that test has to be spelled per slot type. An sp_int's nil is SP_INT_NIL,
   not 0, so a plain `!x` both skipped the store on an ABSENT key (the sentinel
   is nonzero) and overwrote a legitimately stored 0 (which is truthy in Ruby).
   The typed-array branches spell it correctly; the hash branch did not, and it
   is the expression form that is reached when the result is used (#3421). */
void emit_slot_nil_test(Compiler *c, TyKind t, int tmp, int want_nil, Buf *b) {
  const char *n = want_nil ? "" : "!";
  switch (t) {
    case TY_INT:    buf_printf(b, "%s(_t%d == SP_INT_NIL)", n, tmp); return;
    case TY_FLOAT:  buf_printf(b, "%ssp_float_is_nil(_t%d)", n, tmp); return;
    case TY_SYMBOL: buf_printf(b, "%s(_t%d == (sp_sym)-1)", n, tmp); return;
    case TY_POLY:   buf_printf(b, "%ssp_poly_truthy(_t%d)", want_nil ? "!" : "", tmp); return;
    default: break;
  }
  /* bool, string, and every pointer-backed slot: nil and false are both the
     zero value, which is what Ruby treats as falsy here. */
  buf_printf(b, "%s_t%d", want_nil ? "!" : "", tmp);
  (void)c;
}

/* The zero for a result slot of this type. default_value answers NULL for an
   object, which suits a pointer-backed class and not a by-value one, whose C
   representation is a bare struct -- `sp_Frame _t9 = NULL` is not valid C. */
static const char *slot_zero(Compiler *c, TyKind t) {
  if (comp_ty_value_obj(c, t)) return raise_tail_value_c(c, t);
  return default_value_from_compiler(c, t);
}

/* Is `id` somewhere inside the subtree at `root`? */
static int node_in_subtree(const NodeTable *nt, int root, int id) {
  if (root < 0) return 0;
  if (root == id) return 1;
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++)
    if (node_in_subtree(nt, nt_ref_at(nt, root, i), id)) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++)
      if (node_in_subtree(nt, ids[j], id)) return 1;
  }
  return 0;
}

/* Does this rescue clause (or one further along its chain) catch a NameError?
   A bare rescue does -- it matches StandardError, which NameError is under. */
static int rescue_chain_catches_nameerror(const NodeTable *nt, int rescue_id) {
  for (int r = rescue_id; r >= 0; r = nt_ref(nt, r, "subsequent")) {
    int nexc = 0;
    const int *exc = nt_arr(nt, r, "exceptions", &nexc);
    if (nexc == 0) return 1;
    for (int i = 0; i < nexc; i++) {
      const char *en = nt_str(nt, exc[i], "name");
      /* a computed exception list says nothing statically: assume it may */
      if (!en) return 1;
      if (sp_streq(en, "NameError") || sp_streq(en, "StandardError") ||
          sp_streq(en, "Exception")) return 1;
    }
  }
  return 0;
}

/* Is this constant reference written inside something that catches the
   NameError it raises? Referencing a missing constant on purpose, behind a
   `rescue NameError`, is how a program probes for one -- ruby/spec does exactly
   that -- and CRuby says nothing about it at any stage. Warning there reports a
   program whose behaviour is already right (#4062). */
static int const_ref_is_rescued(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  for (int b = 0; b < nt->count; b++) {
    NodeKind k = nt_kind(nt, b);
    if (k == NK_BeginNode) {
      int rc = nt_ref(nt, b, "rescue_clause");
      if (rc < 0 || !rescue_chain_catches_nameerror(nt, rc)) continue;
      if (node_in_subtree(nt, nt_ref(nt, b, "statements"), id)) return 1;
    }
    /* `X rescue fallback` catches StandardError, so it catches this too */
    else if (k == NK_RescueModifierNode &&
             node_in_subtree(nt, nt_ref(nt, b, "expression"), id)) return 1;
  }
  return 0;
}

/* The constant path AS WRITTEN, for a message someone has to find in their
   source. The `par_nmc` the arms below key on is deliberately the parent's
   LEAF name (the ffi and platform-constant tables are keyed that way), and
   reusing it for the message named `SSL::VERIFY_NONE` where the source said
   `OpenSSL::SSL::VERIFY_NONE`, and `C::D` for `A::B::C::D`. Neither string
   occurs in the program, which is exactly what a reader greps for. Walk the
   parent chain instead.

   A prefix that is not a written constant (`BLOCK::CODE`, where BLOCK holds a
   class) stops the walk: what is left is the written tail, which is still more
   of the path than the leaf pair was. `Object::X` keeps dropping the Object,
   as CRuby does -- Object's constants are the top-level ones (#3976). */
static void const_path_written(const NodeTable *nt, int id, char *out, size_t cap) {
  const char *parts[16];
  int n = 0, cur = id;
  while (cur >= 0 && n < 16) {
    const char *t = nt_type(nt, cur);
    const char *nm = t ? nt_str(nt, cur, "name") : NULL;
    if (!nm || !*nm) break;
    if (sp_streq(t, "ConstantPathNode")) { parts[n++] = nm; cur = nt_ref(nt, cur, "parent"); }
    else if (sp_streq(t, "ConstantReadNode")) { parts[n++] = nm; break; }
    else break;
  }
  if (n > 1 && sp_streq(parts[n - 1], "Object")) n--;
  size_t off = 0;
  out[0] = 0;
  for (int i = n - 1; i >= 0; i--) {
    int w = snprintf(out + off, off < cap ? cap - off : 0, "%s%s",
                     i == n - 1 ? "" : "::", parts[i]);
    if (w < 0) break;
    off += (size_t)w;
    if (off >= cap) { out[cap - 1] = 0; break; }
  }
}

/* One build-time warning per constant that the whole program never defines.
   The reference still emits its runtime NameError (CRuby's behaviour, which
   ruby/spec asserts), but the build no longer says nothing at all about a name
   that can never resolve -- a dropped require reads as a runtime engine bug
   otherwise (#3976). Deduplicated by name so a constant read in a loop body
   warns once. */
static void warn_undefined_constant(Compiler *c, int id, const char *nm) {
  static char seen[64][128];
  static int nseen = 0;
  if (!nm || !*nm) return;
  for (int i = 0; i < nseen; i++) if (sp_streq(seen[i], nm)) return;
  if (nseen < 64) { snprintf(seen[nseen], sizeof seen[0], "%s", nm); nseen++; }
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  int fid = (int)nt_int(c->nt, id, "node_file", 0);
  const char *file = nt_file_path(c->nt, fid);
  if (!file || !*file) file = c->nt->source_file;
  if (!file || !*file) file = "source.rb";
  if (ln > 0)
    fprintf(stderr, "spinel: %s:%d: warning: uninitialized constant %s: "
                    "defined nowhere in the program (raises NameError when reached)\n",
            file, ln, nm);
  else
    fprintf(stderr, "spinel: warning: uninitialized constant %s: "
                    "defined nowhere in the program (raises NameError when reached)\n", nm);
}

/* `recv.attr ||= v` / `&&=` where the reader or the writer is a real `def`.
   The direct-ivar shapes below are a fast path for a generated accessor pair,
   where the ivar IS the attribute; a hand-written reader or writer has to be
   called, and skipping it wrote the ivar and never ran the writer -- silently
   when the ivar happened to share the method's name, and as a C build error
   naming a struct member that does not exist when it did not (#4148).

   CRuby's shape, measured: the receiver is evaluated once, the reader is
   called once, and on the assigning branch the value is the ASSIGNED value --
   not the writer's return and not a re-read (`def v=(x); @v = x * 10; end`
   leaves 70 in the ivar and answers 7). The right-hand side is not evaluated
   at all when the reader's answer decides it.

   Answers 1 when it emitted; 0 leaves the caller's existing shapes alone. */
/* The guarded assignment of an or/and-write, `if (COND) LHS = RHS`, with the
   RHS's setup captured: the statements a composite RHS spills to g_pre (a
   hash literal's fills, a block-taking call's loop) would otherwise run
   ahead of the guard on every evaluation, so `@map ||= { 0 => T.new }` built
   the table each call and threw it away, and a RHS that succeeds once and
   raises afterwards raised where CRuby answered the memo (#4513). The setup
   is spliced inside the conditional. Value form yields the LHS after; the
   statement form ends its line. */
void emit_orw_guard(Compiler *c, int v, TyKind slot, const char *cond, const char *lhs,
                    int value_form, int indent, Buf *b) {
  int boxed = slot == TY_POLY;
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  Buf *saved_pre = g_pre; g_pre = &vpre;
  if (boxed) emit_boxed(c, v, &vval);
  else emit_coerce(c, v, slot, CO_HOLD, "a local variable's `||=` or `&&=`", &vval);
  g_pre = saved_pre;
  if (!value_form) emit_indent(b, indent);
  if (value_form) buf_puts(b, "({ ");
  if (cond) buf_printf(b, "if (%s) { ", cond);
  if (vpre.p) buf_puts(b, vpre.p);
  buf_printf(b, "%s = %s;", lhs, vval.p ? vval.p : (boxed ? "sp_box_nil()" : "0"));
  if (cond) buf_puts(b, " }");
  if (value_form) buf_printf(b, " %s; })", lhs);
  else buf_puts(b, "\n");
  free(vpre.p); free(vval.p);
}

/* The guarded store of `REF ||= v` / `REF &&= v` on a shared-handle String
   slot: the RHS goes in as a handle (an alias by handle, anything else
   freshly wrapped), its setup spliced inside the guard. */
static void emit_strbuf_orw_guard(Compiler *c, const char *ref, int v, int is_or, Buf *b) {
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  Buf *saved_pre = g_pre; g_pre = &vpre;
  char srefO[1024];
  if (strbuf_slot_ref(c, v, srefO, sizeof srefO)) buf_puts(&vval, srefO);
  else { buf_puts(&vval, "sp_String_new_shared("); emit_str_expr(c, v, &vval); buf_puts(&vval, ")"); }
  g_pre = saved_pre;
  buf_printf(b, "if (%s%s) { ", is_or ? "!" : "", ref);
  if (vpre.p) buf_puts(b, vpre.p);
  buf_printf(b, "%s = %s; }", ref, vval.p ? vval.p : "");
  free(vpre.p); free(vval.p);
}

/* A local bound to an ivar write on a shared-handle String slot
   (`b = (@buf ||= +"")`, `b = @buf = +""`): the write, then the slot's
   HANDLE, so the local and the ivar are one object and an append through
   the local shows in the ivar. Answers 0 when `v` is no such write. */
int emit_strbuf_ivar_write_handle(Compiler *c, int v, Buf *b) {
  const NodeTable *nt = c->nt;
  int w = strbuf_ivar_alias_value(nt, v);
  if (w < 0 || nt_kind(nt, w) == NK_InstanceVariableReadNode) return 0;
  const char *nm = nt_str(nt, w, "name");
  int cid = strbuf_ivar_owner(c, w);
  if (!nm || cid < 0) return 0;
  int iv = comp_ivar_index(&c->classes[cid], nm);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_STRBUF) return 0;
  char ref[300];
  Scope *cs = comp_scope_of(c, w);
  if (cs && cs->class_id < 0) snprintf(ref, sizeof ref, "civ_Toplevel_%s", iv_c(nm + 1));
  else snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
  buf_puts(b, "({ ");
  if (nt_kind(nt, w) == NK_InstanceVariableWriteNode) emit_stmt_inner(c, w, b, 0);
  else emit_strbuf_orw_guard(c, ref, nt_ref(nt, w, "value"),
                             nt_kind(nt, w) == NK_InstanceVariableOrWriteNode, b);
  buf_printf(b, " %s; })", ref);
  return 1;
}

/* `REF ||= v` / `REF &&= v` on a typed slot of kind T, as a value: the
   guard tests the slot kind's own nil (NULL for a pointer-backed slot, a
   sentinel for Integer and Symbol) and the RHS converts to the slot's kind.
   Shared by an ivar and by a generated attribute read through a typed
   receiver, which had its own partial copy and lost the write (#5428). */
void emit_slot_orw_value(Compiler *c, TyKind t, const char *ref, int v, int is_or, Buf *b) {
  /* The RHS is rendered with its setup captured: the statements a composite
     RHS spills to g_pre (a hash literal's fills, a block-taking call's loop)
     would otherwise run unconditionally, ahead of the guard, so
     `@map ||= { 0 => T.new }` built the table on every call and discarded
     it, and a RHS that succeeds once and raises afterwards raised on the
     second call where CRuby answered the memo (#4513). The setup is spliced
     inside the conditional, where it runs only when the assignment is
     taken. The poly arm did this already; every slot kind does now. */
  Buf vpre; memset(&vpre, 0, sizeof vpre);
  Buf vval; memset(&vval, 0, sizeof vval);
  const char *cond = NULL;
  char condb[400];
  int unconditional = 0;
  /* A shared-handle string slot takes the RHS as a handle (an alias by
     handle, anything else freshly wrapped), and the expression's value is
     the slot's read face with the handle published, as a plain write's is. */
  if (t == TY_STRBUF) {
    buf_puts(b, "({ ");
    emit_strbuf_orw_guard(c, ref, v, is_or, b);
    buf_printf(b, " (_sp_ret_strbuf = (void *)%s, %s ? sp_str_concat(sp_String_cstr(%s), (&(\"\\xff\")[1])) : NULL); })",
               ref, ref, ref);
    return;
  }
  if (t == TY_POLY) {
    Buf *saved_pre = g_pre; g_pre = &vpre;
    emit_boxed(c, v, &vval);
    g_pre = saved_pre;
    snprintf(condb, sizeof condb, "%ssp_poly_truthy(%s)", is_or ? "!" : "", ref);
    cond = condb;
    if (!vval.p) buf_puts(&vval, "sp_box_nil()");
  }
  else {
    Buf *saved_pre = g_pre; g_pre = &vpre;
    /* an empty `[]`/`{}` takes the slot's variant; its own inferred one
       comes from reads elsewhere, not from this slot (#4111) */
    if (t == TY_BIGINT && comp_ntype(c, v) != TY_BIGINT && ty_is_numeric(comp_ntype(c, v))) {
      /* an Integer into a Bignum slot is promoted, as a plain ivar write does */
      buf_puts(&vval, "sp_bigint_new_int("); emit_int_expr(c, v, &vval); buf_puts(&vval, ")");
    }
    else if (emit_empty_literal_as(c, v, t, &vval)) { }
    /* a typed array into a general Array slot is rebuilt as one, as a plain
       write does */
    else if (emit_array_into_poly_slot(c, t, v, &vval)) { }
    else if (seeded_array_kind_mismatch(t, comp_ntype(c, v)))
      emit_array_store_value(c, t, v, &vval);   /* a seed-pinned kind converts */
    else emit_coerce(c, v, t, CO_HOLD, "a variable's `||=` or `&&=`", &vval);
    g_pre = saved_pre;
    if (t == TY_BOOL || t == TY_STRING)
      snprintf(condb, sizeof condb, "%s%s", is_or ? "!" : "", ref);
    else if (t == TY_INT)
      snprintf(condb, sizeof condb, "%s %s= SP_INT_NIL", ref, is_or ? "=" : "!");
    else if (t == TY_SYMBOL)   /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      snprintf(condb, sizeof condb, "%s %s= (sp_sym)-1", ref, is_or ? "=" : "!");
    else if (t == TY_CLASS)   /* a Class slot's nil is SP_CLASS_NIL (#5357) */
      snprintf(condb, sizeof condb, "%ssp_class_nil_p(%s)", is_or ? "" : "!", ref);
    else if (t == TY_FLOAT)   /* a Float slot's nil is the SP_FLOAT_NIL bit pattern */
      snprintf(condb, sizeof condb, "%ssp_float_is_nil(%s)", is_or ? "" : "!", ref);
    /* a pointer-backed slot (object/array/hash/Bignum/fiber/proc/...) reads falsy
       when NULL, so `@x ||= v` is `if (!@x) @x = v` and `@x &&= v` is
       `if (@x) @x = v`. Falling through to a bare read dropped the init when
       this or-write was the RHS of a poly-receiver setter switch (#1447). */
    else if (ty_is_object(t) || ty_is_array(t) || ty_is_hash(t) || t == TY_BIGINT ||
             t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_PROC || t == TY_IO ||
             t == TY_MATCHDATA || t == TY_EXCEPTION || t == TY_REGEX) {
      snprintf(condb, sizeof condb, "%s%s", is_or ? "!" : "", ref);
      /* an unresolved-call RHS (`@x ||= recv.map{...}` where recv typed
         poly/unknown) is a raise-all returning sp_RbVal; into a pointer-backed
         slot, keep the raise for effect but yield the slot's typed NULL so the
         assignment compiles -- the raise aborts before the NULL is reached
         (#2457). */
      const char *ivtxt = vval.p ? vval.p : "";
      if (strncmp(ivtxt, "sp_raise_nomethod(", 18) == 0 ||
          strncmp(ivtxt, "(sp_raise_cls(", 14) == 0) {
        Buf w; memset(&w, 0, sizeof w);
        buf_printf(&w, "(%s, %s)", ivtxt, default_value_from_compiler(c, t));
        free(vval.p); vval = w;
      }
    }
    else if (!is_or) unconditional = 1;
    else { free(vpre.p); free(vval.p); buf_puts(b, ref); return; }
    if (!unconditional) cond = condb;
  }
  if (unconditional) {
    buf_puts(b, "({ ");
    if (vpre.p) buf_puts(b, vpre.p);
    buf_printf(b, "%s = %s; %s; })", ref, vval.p ? vval.p : "", ref);
  }
  else {
    buf_printf(b, "({ if (%s) { ", cond);
    if (vpre.p) buf_puts(b, vpre.p);
    buf_printf(b, "%s = %s; } %s; })", ref, vval.p ? vval.p : "", ref);
  }
  free(vpre.p); free(vval.p);
}

int emit_call_or_write_via_methods(Compiler *c, int id, int is_or, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  const char *attr = nt_str(nt, id, "name");
  int v = nt_ref(nt, id, "value");
  if (recv < 0 || !attr || v < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_object(rt)) return 0;
  int cid = ty_object_class(rt);
  int rmi = -1, wmi = -1;
  int rk = comp_resolve_member(c, cid, attr, 0, NULL, &rmi);
  int wk = comp_resolve_member(c, cid, attr, 1, NULL, &wmi);
  if (rk != SP_MEMBER_METHOD && wk != SP_MEMBER_METHOD) return 0;
  /* Both halves have to exist for the pair to run; a missing one is
     NoMethodError in CRuby, which the ordinary call path reports. */
  if (rk == SP_MEMBER_NONE || wk == SP_MEMBER_NONE) return 0;
  if ((rk == SP_MEMBER_METHOD && rmi < 0) || (wk == SP_MEMBER_METHOD && wmi < 0)) return 0;

  TyKind want = comp_ntype(c, id);
  if (want == TY_UNKNOWN || want == TY_VOID) want = TY_POLY;
  TyKind rdt = (rk == SP_MEMBER_METHOD) ? (TyKind)c->scopes[rmi].ret : TY_UNKNOWN;
  /* A generated reader is the ivar itself, read straight off the struct, so
     the temp takes the ivar's own type. Taking the expression's type put a
     boxed ivar into an sp_int when the def writer had no other call site to
     type its parameter (#4827); the arms below convert to `want`. */
  if (rk != SP_MEMBER_METHOD) {
    char rivn[300]; snprintf(rivn, sizeof rivn, "@%s", attr);
    int riv = comp_ivar_index(&c->classes[cid], rivn);
    TyKind ivt = riv >= 0 ? c->classes[cid].ivar_types[riv] : TY_UNKNOWN;
    if (ivt != TY_UNKNOWN && ivt != TY_VOID && ivt != TY_NIL) rdt = ivt;
  }
  if (rdt == TY_UNKNOWN || rdt == TY_VOID) rdt = want;
  TyKind vt = comp_ntype(c, v);
  /* TY_NIL joins them: emit_ctype spells it `void`, so a literal nil on the
     right declared a void temp. nil is a value here (`u.g ||= nil` is nil in
     CRuby), and the slot that has to hold it is the poly one. */
  if (vt == TY_UNKNOWN || vt == TY_VOID || vt == TY_NIL) vt = want;

  int tr = ++g_tmp, tv = ++g_tmp, tw = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
  if (!comp_ty_value_obj(c, rt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tr);
  /* the reader, once */
  emit_ctype(c, rdt, b); buf_printf(b, " _t%d = ", tv);
  if (rk == SP_MEMBER_METHOD) {
    Buf rb; memset(&rb, 0, sizeof rb);
    emit_method_cname(c, &c->scopes[rmi], &rb);
    buf_printf(&rb, "((sp_%s *)_t%d)", c->classes[cid].c_name, tr);
    TyKind got = (TyKind)c->scopes[rmi].ret;
    if (got == TY_UNKNOWN || got == TY_VOID) got = rdt;
    if (got == rdt) buf_puts(b, rb.p ? rb.p : "");
    else if (rdt == TY_POLY) emit_boxed_text(c, got, rb.p ? rb.p : "", b);
    else emit_unbox_text(c, rdt, rb.p ? rb.p : "", b);
    free(rb.p);
  }
  else { buf_printf(b, "((sp_%s *)_t%d)->iv_%s", c->classes[cid].c_name, tr, iv_c(attr)); }
  buf_puts(b, "; ");
  if (rdt == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tv);
  else if (!is_scalar_ret(rdt) && !comp_ty_value_obj(c, rdt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tv);
  /* the test, then either the reader's answer or the assignment's value.
     `||=` assigns when the reader is falsy, `&&=` when it is truthy. */
  buf_puts(b, "(");
  emit_slot_nil_test(c, rdt, tv, is_or ? 0 : 1, b);
  buf_puts(b, ") ? ");
  { char sv[32]; snprintf(sv, sizeof sv, "_t%d", tv);
    if (rdt == want) buf_puts(b, sv);
    else if (want == TY_POLY) emit_boxed_text(c, rdt, sv, b);
    /* nil-preserving: the reader can answer nil, and this arm is exactly the
       one taken when it did (`&&=` on a nil attribute). Plain unboxing lands
       on the payload under the tag -- 0 for an int slot, where nil is
       SP_INT_NIL -- so `b.v &&= 7` answered 0 where CRuby answers nil. */
    else emit_unbox_nilable_text(c, want, sv, b);
  }
  buf_puts(b, " : ({ ");
  emit_ctype(c, vt, b); buf_printf(b, " _t%d = ", tw);
  /* An empty container literal has no type of its own: comp_ntype answers
     poly and the emitter picks the concrete container from how the attribute
     is used later, so `_t%d` was declared sp_RbVal and initialized with an
     sp_IntArray * (#4277). Render it into the slot the declaration promises.
     Every other value type answers the same both ways, so this changes only
     the shapes that did not compile. */
  if (vt == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
  else emit_expr(c, v, b);
  buf_puts(b, "; ");
  if (vt == TY_POLY) buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", tw);
  else if (!is_scalar_ret(vt) && !comp_ty_value_obj(c, vt)) buf_printf(b, "SP_GC_ROOT(_t%d); ", tw);
  buf_puts(b, "(void)(");
  if (wk == SP_MEMBER_METHOD) {
    emit_method_cname(c, &c->scopes[wmi], b);
    buf_printf(b, "((sp_%s *)_t%d, ", c->classes[cid].c_name, tr);
    { Scope *ws = &c->scopes[wmi];
      LocalVar *pv = (ws->nparams > 0 && ws->pnames[0]) ? scope_local(ws, ws->pnames[0]) : NULL;
      TyKind pt = pv ? pv->type : vt;
      char sw[32]; snprintf(sw, sizeof sw, "_t%d", tw);
      if (pt == vt || pt == TY_UNKNOWN) buf_puts(b, sw);
      else if (pt == TY_POLY) emit_boxed_text(c, vt, sw, b);
      else emit_unbox_text(c, pt, sw, b);
    }
    buf_puts(b, ")");
  }
  else {
    buf_printf(b, "((sp_%s *)_t%d)->iv_%s = _t%d", c->classes[cid].c_name, tr, iv_c(attr), tw);
  }
  buf_puts(b, "); ");
  { char sw[32]; snprintf(sw, sizeof sw, "_t%d", tw);
    if (vt == want) buf_puts(b, sw);
    else if (want == TY_POLY) emit_boxed_text(c, vt, sw, b);
    else emit_unbox_text(c, want, sw, b);
  }
  buf_puts(b, "; }); })");
  return 1;
}

/* `$g |= v` / `@@a |= v` (and `&=` `-=` `+=` `*=`) on an array slot as a
   value -- a method's last expression, a block's, an assignment's rhs: the
   statement form's array write, then the updated slot. The value arm emitted
   the raw C operator, `|` between two array pointers, which did not compile
   (#4833 fixed the statement form only). Answers 1 when it emitted. */
static int emit_array_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                      const char *op, int v, Buf *b) {
  if (!(ty_is_array(t) || t == TY_POLY_ARRAY)) return 0;
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_array_op_assign(c, ref, t, op, v, &ab);
  if (ok) buf_printf(b, "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

/* The scalar twin: the statement form, then the slot, in a statement
   expression, so a global or class variable takes the overflow-checked
   helpers in value position too. */
static int emit_scalar_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                       const char *op, int v, int lhs_nil, Buf *b) {
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_scalar_op_assign(c, ref, t, op, v, 1, lhs_nil, &ab);
  if (ok) buf_printf(b, "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

/* The boxed twin, for a poly slot. */
static int emit_poly_op_assign_value(Compiler *c, const char *ref, TyKind t,
                                     const char *op, int v, Buf *b) {
  if (t != TY_POLY) return 0;
  Buf ab; memset(&ab, 0, sizeof ab);
  int ok = emit_poly_op_assign(c, ref, op, v, 1, &ab);
  if (ok) buf_printf(b, "({ %s%s; })", ab.p, ref);
  free(ab.p);
  return ok;
}

static void emit_expr_node(Compiler *c, int id, Buf *b);

/* How many expressions enclose the one being emitted: a call nested in an
   argument sits deeper than the call that takes the argument. */
int g_expr_depth = 0;

void emit_expr(Compiler *c, int id, Buf *b) {
  /* an argument of a call re-emitted as its builtin sees the reopenings */
  if (g_io_skip_reopen && id != g_io_skip_node) {
    g_io_skip_reopen = 0;
    emit_expr(c, id, b);
    g_io_skip_reopen = 1;
    return;
  }
  g_expr_depth++;
  emit_expr_node(c, id, b);
  g_expr_depth--;
}

/* `_tN = v` inside the guard of an `a[i] ||= v` / `&&= v` value, with v's
   own prelude emitted in the guard too: hoisted above it, that prelude ran
   even when the guard skipped the write, and before the slot read. */
static void emit_guarded_slot_assign(Compiler *c, int v, int tn, Buf *b) {
  Buf rvb; memset(&rvb, 0, sizeof rvb);
  Buf *svp = g_pre; g_pre = b;
  emit_expr(c, v, &rvb);
  g_pre = svp;
  buf_printf(b, "_t%d = %s", tn, rvb.p ? rvb.p : "0");
  free(rvb.p);
}

static void emit_guarded_poly_slot_assign(Compiler *c, int v, int tn, Buf *b) {
  /* The right-hand side's own prelude belongs INSIDE the guard. Hoisted
     above it, a call there runs even when the key is already present --
     which turned the memoizing `memo[n] ||= f.(n-1) + f.(n-2)` into
     unbounded recursion. */
  Buf rvb; memset(&rvb, 0, sizeof rvb); Buf *svp = g_pre; g_pre = b;
  emit_boxed(c, v, &rvb); g_pre = svp;
  buf_printf(b, "_t%d = %s", tn, rvb.p ? rvb.p : "sp_box_nil()"); free(rvb.p);
}

static void emit_if_arm_value(Compiler *c, int last, TyKind res, int tr) {
  TyKind lt = comp_ntype(c, last);
  int saved_gi = g_indent; g_indent = g_indent + 2;
  Buf le; memset(&le, 0, sizeof le);
  emit_expr(c, last, &le);
  g_indent = saved_gi;
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = ", tr);
  if (res == TY_POLY && lt != TY_POLY) {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, lt, le.p ? le.p : default_value_from_compiler(c, lt), &bx);
    buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
  }
  else if (emit_arm_text_as_bigint(res, lt, le.p ? le.p : default_value_from_compiler(c, lt), g_pre)) { }
  else buf_puts(g_pre, le.p ? le.p : default_value_from_compiler(c, res));
  buf_puts(g_pre, ";\n");
  free(le.p);
}

/* The engine's String constants (RUBY_VERSION, ...) are frozen in CRuby, and
   each is one object however often it is read. Each is a frozen static laid
   out like a frozen literal -- a real sp_str_hdr, then the 0xf1 marker --
   defined once ahead of the bodies at its first read, so `RUBY_VERSION << "x"`
   raises FrozenError and RUBY_VERSION.equal?(RUBY_VERSION) still holds. `lit`
   is the C source of a string literal (RUBY_PLATFORM's is the runtime's
   macros). `var` is named under sp_str_, a prefix the runtime owns, so a
   user `def ruby_version` (sp_ruby_version) cannot take the same C name. */
static void emit_engine_const_str(const char *var, const char *lit, Buf *b) {
  char key[64];
  snprintf(key, sizeof key, " %s = {", var);
  if (!g_proc_protos.p || !strstr(g_proc_protos.p, key))
    buf_printf(&g_proc_protos,
               "static struct { sp_str_hdr h; unsigned char m; char d[sizeof(%s)]; } %s = "
               "{ { NULL, sizeof(%s) | SP_STR_SIZE_ASCII7, sizeof(%s) - 1, 0 }, 0xf1, %s };\n",
               lit, var, lit, lit, lit);
  buf_printf(b, "((const char *)%s.d)", var);
}

static void emit_expr_node(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) unsupported(c, id, "expression (no type)");

  /* Hoisted call argument: substitute the rooted temp (see emit_args_filled). */
  for (int i = g_n_argov - 1; i >= 0; i--)
    if (g_argov_node[i] == id) { buf_puts(b, g_argov_text[i]); return; }

  if (sp_streq(ty, "IntegerNode")) {
    const char *bigval = nt_str(nt, id, "bigval");
    if (bigval) {
      /* An Integer is immutable, so one slot per distinct literal serves
         every use: the string was re-parsed and a Bignum re-allocated at
         each one, every time the line ran (#4637). */
      buf_printf(b, "sp_bigl_get(%d)", bigl_intern(bigval));
      return;
    }
    buf_printf(b, "%lldLL", nt_int(nt, id, "value", 0)); return;
  }
  if (sp_streq(ty, "FloatNode")) { const char *v = nt_content(nt, id); buf_puts(b, v ? v : "0.0"); return; }
  if (sp_streq(ty, "ImaginaryNode")) {
    int num = nt_ref(nt, id, "numeric");
    const char *numty = num >= 0 ? nt_type(nt, num) : NULL;
    buf_puts(b, "((sp_Complex){0.0, (sp_float)(");
    if (num >= 0) emit_expr(c, num, b);
    else buf_puts(b, "0");
    /* 3.5i marks the imaginary component Float-classed; 4i stays Integer */
    buf_printf(b, "), %d})", numty && sp_streq(numty, "FloatNode") ? 2 : 0);
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
    emit_str_literal_src(b, sc ? sc : "", sc ? nt_str_len(nt, id, "content") : 0,
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
    /* the node's own byte length: a symbol literal may hold a NUL (#nul) */
    const char *svl = nt_str(nt, id, "value");
    int sid = comp_sym_intern_n(c, svl ? svl : "", svl ? nt_str_len(nt, id, "value") : 0);
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
    buf_printf(g_pre, " _t%d = %s;\n", cr, rt == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, rt));
    if (needs_root(rt)) { emit_indent(g_pre, g_indent); emit_gc_root_tmp(c, rt, cr, g_pre); buf_puts(g_pre, "\n"); }
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
    emit_str_expr_nilable(c, av[0], b);   /* nil subject: no match, as =~ */
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
    /* (:a..:e): a poly array of boxed symbols, walked by name succession
       (interning through the generated TU's own table) */
    if (left >= 0 && right >= 0 &&
        nt_type(nt, left) && sp_streq(nt_type(nt, left), "SymbolNode") &&
        nt_type(nt, right) && sp_streq(nt_type(nt, right), "SymbolNode")) {
      int ta = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                    " const char *_t%d = sp_sym_to_s(", ta, ta, ts);
      emit_expr(c, left, b);
      buf_printf(b, "); const char *_t%d = sp_sym_to_s(", te);
      emit_expr(c, right, b);
      buf_printf(b, "); for (;;) {"
                    " if (%d && sp_str_eq(_t%d, _t%d)) break;"
                    " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(_t%d)));"
                    " if (!%d && sp_str_eq(_t%d, _t%d)) break;"
                    " _t%d = sp_str_succ(_t%d);"
                    " if (sp_str_length(_t%d) > sp_str_length(_t%d)) break; }"
                    " _t%d; })",
                 excl, ts, te,
                 ta, ts,
                 excl, ts, te,
                 ts, ts,
                 ts, te,
                 ta);
      return;
    }
    /* (1.0..3.0): the distinct float range, endpoints kept as sp_float.
       A missing bound uses -/+HUGE_VAL as the beginless/endless sentinel. */
    if (comp_ntype(c, id) == TY_FLOAT_RANGE) {
      /* A bound written as absent and one written as Float::INFINITY are the
         same value; record which it was so #inspect can tell them apart. */
      int om = (left < 0 ? 1 : 0) | (right < 0 ? 2 : 0);
      /* Each endpoint renders as the user wrote it, so record which one was an
         Integer: a mixed literal (1.5..5) inspects as "1.5..5" (#3896). */
      if (left >= 0 && comp_ntype(c, left) == TY_INT) om |= 4;
      if (right >= 0 && comp_ntype(c, right) == TY_INT) om |= 8;
      buf_printf(b, "sp_frange_new%s(", om ? "_o" : "");
      if (left >= 0) emit_float_expr(c, left, b); else buf_puts(b, "(-HUGE_VAL)");
      buf_puts(b, ", ");
      if (right >= 0) emit_float_expr(c, right, b); else buf_puts(b, "HUGE_VAL");
      buf_printf(b, ", %d", excl);
      if (om) buf_printf(b, ", %d", om);
      buf_puts(b, ")");
      return;
    }
    /* ("a".."e"): the distinct string range, endpoints kept as strings (#3064) */
    if (comp_ntype(c, id) == TY_STR_RANGE) {
      buf_puts(b, "sp_srange_new(");
      if (left >= 0) emit_str_expr_nilable(c, left, b); else buf_puts(b, "NULL");
      buf_puts(b, ", ");
      if (right >= 0) emit_str_expr_nilable(c, right, b); else buf_puts(b, "NULL");
      buf_printf(b, ", %d)", excl);
      return;
    }
    /* sp_Range holds sp_int bounds, so a Range OBJECT over user objects has
       nowhere to live; emit_int_expr below would hand a pointer to an sp_int
       parameter and the C compiler would reject it. Comparable#clamp with such
       a range still works INLINE (`x.clamp(lo..hi)`, and the one-sided forms):
       there the bounds are folded straight into the comparison and no Range is
       ever built. Only materializing one is out. #2558 */
    {
      TyKind lt = left >= 0 ? comp_ntype(c, left) : TY_UNKNOWN;
      TyKind rt2 = right >= 0 ? comp_ntype(c, right) : TY_UNKNOWN;
      /* sp_Range's bounds are sp_int, so a Bignum bound has nowhere to live;
         emit_int_expr below would hand a pointer to an sp_int parameter and
         the C compiler would reject it with a warning-shaped diagnostic far
         from the cause. Name the limitation instead. (#3058) */
      if (lt == TY_BIGINT || rt2 == TY_BIGINT) {
        unsupported_feature(c, id,
          "a Range with a Bignum bound cannot be built: a Range is an unboxed "
          "value with sp_int bounds, so it has nowhere to hold one. Comparing "
          "against the bounds directly (`x >= lo && x < hi`) needs no Range and "
          "does work (see docs/limitations.md)");
        return;
      }
      if (ty_is_object(lt) || ty_is_object(rt2)) {
        const char *ocn = NULL;
        int oci = ty_is_object(lt) ? ty_object_class(lt) : ty_object_class(rt2);
        if (oci >= 0 && oci < c->nclasses) ocn = class_ruby_name(c, oci);
        static char rbuf[400];
        snprintf(rbuf, sizeof rbuf,
                 "a Range of %s objects cannot be built: a Range is an unboxed value "
                 "with sp_int bounds, so it has nowhere to hold them. Passing the "
                 "bounds directly (`x.clamp(lo..hi)`, `x.clamp(lo, hi)`, or a one-sided "
                 "`lo..` / `..hi`) needs no Range and does work (see docs/limitations.md)",
                 ocn ? ocn : "user");
        unsupported_feature(c, id, rbuf);
      }
    }
    /* An explicitly written `nil` bound is the same range as the omitted one
       (`nil..5` == `..5`), and an infinite Float bound cannot be converted to
       sp_int at all -- passing the emitted (1.0/0.0) through the sp_int
       parameter is UB, which is where the arbitrary integer came from. Both
       spellings take the unbounded sentinel (#3670). */
    int left_unbounded = left < 0 ||
        (nt_type(nt, left) && sp_streq(nt_type(nt, left), "NilNode")) ||
        lazy_endpoint_is_infinite(c, left);
    /* `-Float::INFINITY` is the negation call around the constant */
    if (!left_unbounded && left >= 0 && nt_kind(nt, left) == NK_CallNode &&
        nt_str(nt, left, "name") && sp_streq(nt_str(nt, left, "name"), "-@") &&
        lazy_endpoint_is_infinite(c, nt_ref(nt, left, "receiver")))
      left_unbounded = 1;
    int right_unbounded = right < 0 ||
        (nt_type(nt, right) && sp_streq(nt_type(nt, right), "NilNode")) ||
        lazy_endpoint_is_infinite(c, right);
    buf_puts(b, "sp_range_new(");
    if (!left_unbounded) emit_int_expr_nilable(c, left, b); else buf_puts(b, "INTPTR_MIN");  /* beginless; a non-literal nil bound keeps the old looseness */
    buf_puts(b, ", ");
    if (!right_unbounded) emit_int_expr_nilable(c, right, b); else buf_puts(b, "INTPTR_MAX");  /* endless; same */
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
      /* A `rescue K => e` arm's read of a binding two arms share (#4343). The
         slot is a sp_Exception *, and every user exception subclass's struct
         opens with that header, so the arm's class is a plain pointer cast --
         not the poly unbox the guards below want. */
      {
        LocalVar *elv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
        if (elv && elv->type == TY_EXCEPTION && ty_is_object(c->nilnarrow[id])) {
          buf_puts(b, "((");
          emit_ctype(c, c->nilnarrow[id], b);
          buf_printf(b, ")%s)", rb2.p ? rb2.p : "");
          free(rb2.p);
          return;
        }
      }
      /* The narrowing was recorded while the slot was boxed; a slot that
         has since settled on the narrowed type itself (`v = yield x` whose
         every site answers an Array) is read as it is, not unboxed from a
         box it never was. */
      {
        LocalVar *nlv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
        if (nlv && nlv->type == c->nilnarrow[id] && nlv->type != TY_POLY) {
          buf_puts(b, rb2.p ? rb2.p : "");
          free(rb2.p);
          return;
        }
      }
      /* An `is_a?(Array)`-narrowed read: a boxed array can be any element-typed
         representation (Int/Float/Str/Poly array), so normalize to a PolyArray
         at runtime rather than casting the raw .v.p to one kind. */
      if (c->nilnarrow[id] == TY_POLY_ARRAY)
        buf_printf(b, "sp_poly_to_poly_array(%s)", rb2.p ? rb2.p : "");
      else
        emit_unbox_text(c, c->nilnarrow[id], rb2.p ? rb2.p : "", b);
      free(rb2.p);
      return;
    }
    LocalVar *slv = lrn ? scope_local(comp_scope_of(c, id), lrn) : NULL;
    /* The node cache can lag a later narrowing of the local itself (a
       map-block hash-key param settling from poly to string): a POLY-typed
       read of a concretely-declared local boxes the declared value so the
       consumer's poly dispatch stays well-typed (#2730). */
    if (slv && slv->type != TY_POLY && slv->type != TY_UNKNOWN &&
        slv->type != TY_STRBUF && comp_ntype(c, id) == TY_POLY) {
      Buf rb3; memset(&rb3, 0, sizeof rb3);
      emit_local_ref(c, id, lrn, &rb3);
      emit_boxed_text(c, slv->type, rb3.p ? rb3.p : "", b);
      free(rb3.p);
      return;
    }
    if (slv && slv->type == TY_STRBUF) {
      /* A container-store / equal?-arg read of a shared-mutable string yields
         the live HANDLE, not a copy (#3227 phase 3). */
      if (c->strbuf_box[id]) {
        emit_local_ref(c, id, lrn, b);
        return;
      }
      /* A mutable-string local read yields an independent GC string copy: its
         sp_String buffer is not itself a GC object, so a bare cstr pointer
         would dangle once the wrapper is unreachable (e.g. after `return`).
         Transient append/length use the raw sp_String via dedicated paths. */
      /* publish the handle to the deep-return side channel as the copy is
         read out: a marked caller of a shared-returning method picks it up
         right after the call (#3227 P6) */
      buf_puts(b, "(_sp_ret_strbuf = (void *)");
      emit_local_ref(c, id, lrn, b);
      /* A parameter that is the handle can be nil, `def initialize(s, o: nil)`
         or `def run(cmd, text: nil)` whose block appends to it: nil is a NULL
         handle, and reads as nil. So is a local's (`q = nil; q = +"x" if c`
         with `q.tap { |w| w << "!" if w }` making q the handle). */
      if (slv->dyn_handle || slv->is_param || slv->str_shared) {
        buf_puts(b, ", ");
        emit_local_ref(c, id, lrn, b);
        buf_puts(b, " ? sp_str_concat(sp_String_cstr(");
        emit_local_ref(c, id, lrn, b);
        buf_puts(b, "), (&(\"\\xff\")[1])) : NULL)");
        return;
      }
      buf_puts(b, ", sp_str_concat(sp_String_cstr(");
      emit_local_ref(c, id, lrn, b);
      buf_puts(b, "), (&(\"\\xff\")[1])))");
      return;
    }
    /* A POLY variable handed to a parameter the callee appends to in place:
       a plain String it holds becomes the shared handle and is stored back
       first, so the callee appends to the variable's own String (#6179). */
    if (c->poly_strbuf_lift[id] && slv && slv->type == TY_POLY) {
      Buf rl; memset(&rl, 0, sizeof rl);
      emit_local_ref(c, id, lrn, &rl);
      emit_poly_lift_ref(rl.p ? rl.p : "", b);
      free(rl.p);
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
        else if (lv) buf_puts(b, default_value_from_compiler(c, lv->type));
        else buf_puts(b, "sp_box_nil()");
        buf_puts(b, "; "); emit_local_ref(c, id, nm, b); buf_puts(b, "; })");
        return;
      }
    }
    if (lv && lv->type == TY_STRBUF) {
      /* a shared-handle local: the statement form owns every way a value
         becomes the handle (alias, boxed element, fresh wrap); the raw
         const char * went into the sp_String * slot here (a write inside
         Array.new's block, whose non-tail statements are emitted as
         expressions). The value is the slot's ordinary read face, as the
         ivar twin's is. */
      buf_puts(b, "({ ");
      emit_assign(c, id, b, 0);
      /* a nil write leaves the handle NULL: the value is nil, not a read
         through it (CodeRabbit on #4990) */
      buf_puts(b, " (_sp_ret_strbuf = (void *)");
      emit_local_ref(c, id, nm, b);
      buf_puts(b, ", _sp_ret_strbuf ? sp_str_concat(sp_String_cstr(");
      emit_local_ref(c, id, nm, b);
      buf_puts(b, "), (&(\"\\xff\")[1])) : NULL); })");
      return;
    }
    buf_puts(b, "({ ");
    emit_local_ref(c, id, nm, b); buf_puts(b, " = ");
    int ven = 0;
    int v_empty_array = nt_kind(nt, v) == NK_ArrayNode && (nt_arr(nt, v, "elements", &ven), ven == 0);
    if (lv && lv->type == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    /* an empty `[]` is built at the slot's representation, as the statement
       form and the ivar twin below build it: its own default, an IntArray,
       went into an sp_PolyArray *, sp_FloatArray * or sp_PtrArray * slot.
       Array.new's block emits its non-tail statements here, so
       `y = [] if i == 0` there stopped the C build. */
    else if (lv && v_empty_array && ty_is_ptr_array(lv->type)) buf_puts(b, "sp_PtrArray_new()");
    else if (lv && v_empty_array && emit_empty_container_for_slot(c, v, lv->type, b)) { }
    else if (lv && lv->type == TY_POLY_ARRAY && ty_is_array(comp_ntype(c, v)) &&
             comp_ntype(c, v) != TY_POLY_ARRAY) {
      /* a typed array into a poly-array slot: convert, as emit_assign does
         (the statement form; this is its expression twin, #2834) */
      TyKind avt = comp_ntype(c, v);
      buf_printf(b, "sp_PolyArray_from_%s(",
                 avt == TY_INT_ARRAY ? "int_array"
                 : avt == TY_STR_ARRAY ? "str_array" : "float_array");
      emit_expr(c, v, b);
      buf_puts(b, ")");
    }
    else if (lv && lv->type != TY_POLY && lv->type != TY_UNKNOWN &&
             comp_ntype(c, v) == TY_UNKNOWN)
      /* `if (x = obj.unresolved(...)) ...`: the gate's raise-all token into a
         typed slot, coerced (mirrors the statement-form emit_assign). */
      emit_unresolved_coerced(c, v, lv->type, b);
    /* an int or boxed value into a TY_BIGINT slot wraps, exactly as the
       statement form and the return path do. This is the INNER write of a
       chain (`b = a = 0` with `a` promoted to bigint by its loop), which is
       the one write that reaches this expression twin with a bigint target:
       raw, the literal was reinterpreted as an sp_Bigint* and a boxed chain
       value did not compile at all. */
    else if (lv && lv->type == TY_BIGINT && comp_ntype(c, v) != TY_BIGINT) {
      if (comp_ntype(c, v) == TY_POLY) {
        buf_puts(b, "sp_poly_as_bigint("); emit_expr(c, v, b); buf_puts(b, ")");
      }
      else { buf_puts(b, "sp_bigint_new_int("); emit_expr(c, v, b); buf_puts(b, ")"); }
    }
    /* poly RHS into a scalar/string slot: the same unbox the statement form
       applies (emit_poly_rhs_coerced) */
    else if (lv && emit_poly_rhs_coerced(c, lv->type, v, b)) { }
    else if (lv) emit_coerce(c, v, lv->type, CO_HOLD, "a local variable write", b);
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
    /* a top-level ivar's slot is the Toplevel pseudo-class's, the one the
       store below writes (civ_Toplevel_x): without its kind the value went
       in as it was, and `y = (@a = [])` put an Integer array into a slot
       a later write had made a poly array */
    if (ivcls2 < 0) ivcls2 = comp_class_index(c, "Toplevel");
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
    int fz_cid = -1;  /* instance-path writes guard the frozen bit */
    if (cws && cws->is_cmethod && cid2 >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_%s_%s", c->classes[cid2].name, iv_c(nm + 1));
    else if (cid2 < 0 && g_ie_class_id >= 0) {
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
      fz_cid = g_ie_class_id;
    }
    else if (cid2 < 0 && comp_class_index(c, "Toplevel") >= 0)
      snprintf(ref2e, sizeof ref2e, "civ_Toplevel_%s", iv_c(nm + 1));
    else {
      snprintf(ref2e, sizeof ref2e, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
      fz_cid = cid2;
    }
    /* `@a = @b = nil` as expression: inner writes become statements inside the
       stmt-expr; this target takes its own typed nil (not the inner slot). */
    {
      int ncb = comp_nil_chain_bottom(nt, v);
      if (ncb >= 0) {
        buf_puts(b, "({ ");
        if (fz_cid >= 0) emit_frozen_obj_guard(c, fz_cid, g_self ? g_self : "self", b);
        emit_stmt_inner(c, v, b, 0);
        buf_printf(b, "%s = ", ref2e);
        switch (ivt2) {
        case TY_RANGE: buf_puts(b, "(sp_Range){0}"); break;
        case TY_POLY: buf_puts(b, "sp_box_nil()"); break;
        case TY_INT: buf_puts(b, "SP_INT_NIL"); break;
        case TY_FLOAT: buf_puts(b, "sp_float_nil()"); break;
        case TY_STRING: buf_puts(b, "NULL"); break;
        default: buf_puts(b, default_value_from_compiler(c, ivt2)); break;
        }
        buf_printf(b, "; %s; })", ref2e);
        return;
      }
    }
    buf_puts(b, "({ ");
    if (fz_cid >= 0) emit_frozen_obj_guard(c, fz_cid, g_self ? g_self : "self", b);
    buf_printf(b, "%s = ", ref2e);
    if (v_empty_array2 && ty_is_ptr_array(ivt2)) buf_puts(b, "sp_PtrArray_new()");
    else if (v_empty_array2 && ivt2 == TY_POLY_ARRAY) buf_puts(b, "sp_PolyArray_new()");
    else if (v_empty_array2 && array_kind(ivt2)) buf_printf(b, "sp_%sArray_new()", array_kind(ivt2));
    else if (v_empty_hash2 && ty_is_hash(ivt2)) {
      const char *hcn = ty_hash_cname(ivt2);
      if (hcn) buf_printf(b, "sp_%sHash_new()", hcn);
      else emit_expr(c, v, b);
    }
    else if (ivt2 == TY_STRBUF && comp_ntype(c, v) != TY_STRBUF && comp_ntype(c, v) != TY_POLY) {
      /* a shared-handle slot takes an alias RHS by handle and wraps anything
         else in a fresh handle, exactly as the statement form does; the raw
         const char * went into the sp_String * slot here (a value-position
         write, `def w(v) = (@body = v.to_s)`, #3993 / #4567). The value of
         the expression is the slot's ordinary read face below. */
      char srefW2[1024];
      if (strbuf_slot_ref(c, v, srefW2, sizeof srefW2)) buf_puts(b, srefW2);
      else {
        buf_puts(b, "sp_String_new_shared(");
        emit_str_expr(c, v, b);
        buf_puts(b, ")");
      }
      buf_printf(b, "; (_sp_ret_strbuf = (void *)%s, %s ? sp_str_concat(sp_String_cstr(%s), (&(\"\\xff\")[1])) : NULL); })",
                 ref2e, ref2e, ref2e);
      return;
    }
    else if (ivt2 == TY_POLY && comp_ntype(c, v) != TY_POLY) emit_boxed(c, v, b);
    /* a typed array into the general Array slot the ivar widened to (an
       `o.a << nil` through its reader), rebuilt as the statement form does:
       an endless `def initialize(z) = (@a = [z])` assigned the typed array
       itself and the C did not build */
    else if (emit_array_into_poly_slot(c, ivt2, v, b)) { }
    else if (seeded_array_kind_mismatch(ivt2, comp_ntype(c, v))) {
      /* an array of another kind into a seed-pinned array ivar: converted,
         as the statement form does */
      emit_array_store_value(c, ivt2, v, b);
    }
    else if (ivt2 != TY_POLY && ivt2 != TY_UNKNOWN && comp_ntype(c, v) == TY_POLY) {
      /* poly rhs assigned to a typed ivar: unbox to the concrete type */
      Buf _rb; memset(&_rb, 0, sizeof _rb);
      emit_expr(c, v, &_rb);
      Buf _ck; memset(&_ck, 0, sizeof _ck);
      if (ivcls2 >= 0 && class_ivar_pinned(&c->classes[ivcls2], nm))
        emit_rbs_checked_text(c, ivt2, nm, _rb.p ? _rb.p : "sp_box_nil()", &_ck);
      else buf_puts(&_ck, _rb.p ? _rb.p : "sp_box_nil()");
      emit_unbox_text(c, ivt2, _ck.p ? _ck.p : "sp_box_nil()", b);
      free(_ck.p);
      free(_rb.p);
    }
    else {
      /* a subclass instance stored into an ancestor-typed ivar slot (#3418) */
      emit_obj_upcast_prefix(c, ivt2, comp_ntype(c, v), b);
      emit_coerce(c, v, ivt2, CO_HOLD, "an instance variable write", b);
    }
    /* The expression's value is the slot read back, at the node's own type:
       a write retyped for an instance_exec receiver (ie_body_retype) is typed
       by its value, `:sym`, while a poly slot reads back boxed. */
    { TyKind wt = comp_ntype(c, id);
      if (ivt2 == TY_POLY && wt != TY_POLY && wt != TY_UNKNOWN && wt != TY_VOID && wt != TY_NIL &&
          is_scalar_ret(wt)) {
        buf_puts(b, "; ");
        emit_unbox_text(c, wt, ref2e, b);
        buf_puts(b, "; })");
        return;
      } }
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
    /* a toplevel method's ivar is the Toplevel pseudo-class global, as in
       the statement form */
    int tl3 = cid3 < 0 && g_ie_class_id < 0 && cws3 && !cws3->is_cmethod;
    if (tl3) cid3 = comp_class_index(c, "Toplevel");
    TyKind ivt3 = TY_UNKNOWN;
    if (cid3 >= 0) { int iv3 = comp_ivar_index(&c->classes[cid3], nm); if (iv3 >= 0) ivt3 = c->classes[cid3].ivar_types[iv3]; }
    char ref3[300];
    if (cws3 && cws3->is_cmethod && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_%s_%s", c->classes[cid3].name, iv_c(nm + 1));
    else if (tl3 && cid3 >= 0)
      snprintf(ref3, sizeof ref3, "civ_Toplevel_%s", iv_c(nm + 1));
    else
      snprintf(ref3, sizeof ref3, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    emit_slot_orw_value(c, ivt3, ref3, v, is_or, b);
    return;
  }
  if (sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    int is_or = sp_streq(ty, "LocalVariableOrWriteNode");
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    LocalVar *lv = scope_local(comp_scope_of(c, id), nm);
    TyKind t = lv ? lv->type : TY_UNKNOWN;
    /* the local as every other write names it: a captured one lives in its
       cell (`(*_cell_x)`), an inlined one under its renamed C name */
    /* a number local given a boxed value (`u = (v &&= z)`, z boxed) answers
       a boxed value, as its type says: the local's number, or nil for its
       nil sentinel */
    static int orw_boxing_id = -1;
    if ((t == TY_INT || t == TY_FLOAT) && comp_ntype(c, id) == TY_POLY && orw_boxing_id != id) {
      int sv = orw_boxing_id; orw_boxing_id = id;
      buf_puts(b, t == TY_INT ? "sp_box_int_or_nil(" : "sp_box_float_or_nil(");
      emit_expr_node(c, id, b);
      buf_puts(b, ")");
      orw_boxing_id = sv;
      return;
    }
    char lhs[300];
    { Buf lr; memset(&lr, 0, sizeof lr); emit_local_ref(c, id, nm, &lr);
      snprintf(lhs, sizeof lhs, "%s", lr.p ? lr.p : ""); free(lr.p); }
    char cond[400];
    if (t == TY_POLY) {
      snprintf(cond, sizeof cond, "%ssp_poly_truthy(%s)", is_or ? "!" : "", lhs);
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (t == TY_BOOL) {
      snprintf(cond, sizeof cond, "%s%s", is_or ? "!" : "", lhs);
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (t == TY_SYMBOL) {
      /* nilable symbol: (sp_sym)-1 is the nil sentinel */
      snprintf(cond, sizeof cond, "%s %s= (sp_sym)-1", lhs, is_or ? "=" : "!");
      emit_orw_guard(c, v, t, cond, lhs, 1, 0, b);
    }
    else if (!is_or) {
      /* `x &&= v` assigns only when x is not nil, as the statement form
         tests (a slot with no nil representation always assigns) */
      Buf nb; memset(&nb, 0, sizeof nb);
      if (local_nil_test(c, lv, lhs, &nb)) {
        /* through emit_assign, as `||=` below: a nil or boxed value takes
           the coercion the plain `x = v` does (the sentinel, not 0) */
        Buf apre, abody;
        memset(&apre, 0, sizeof apre); memset(&abody, 0, sizeof abody);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &apre; g_indent = 0;
        emit_assign(c, id, &abody, 0);
        g_pre = sv_pre; g_indent = sv_ind;
        buf_printf(b, "({ if (!(%s)) { ", nb.p);
        if (apre.p) buf_puts(b, apre.p);
        if (abody.p) buf_puts(b, abody.p);
        buf_printf(b, " } %s; })", lhs);
        free(apre.p); free(abody.p);
      }
      else emit_orw_guard(c, v, t, NULL, lhs, 1, 0, b);
      free(nb.p);
    }
    else {
      /* `x ||= v` in value position on a slot that can hold nil (see
         local_nil_test): the old bare read assumed every non-poly local was
         already truthy and dropped the assignment entirely (#3388). */
      Buf rb; memset(&rb, 0, sizeof rb); emit_local_ref(c, id, nm, &rb);
      Buf nb; memset(&nb, 0, sizeof nb);
      if (rb.p && local_nil_test(c, lv, rb.p, &nb)) {
        Buf apre, abody;
        memset(&apre, 0, sizeof apre); memset(&abody, 0, sizeof abody);
        Buf *sv_pre = g_pre; int sv_ind = g_indent;
        g_pre = &apre; g_indent = 0;
        emit_assign(c, id, &abody, 0);
        g_pre = sv_pre; g_indent = sv_ind;
        buf_printf(b, "({ if (%s) { ", nb.p);
        if (apre.p) buf_puts(b, apre.p);
        if (abody.p) buf_puts(b, abody.p);
        buf_printf(b, " } %s; })", rb.p);
        free(apre.p); free(abody.p);
      }
      else buf_puts(b, lhs);
      free(nb.p); free(rb.p);
    }
    return;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode")) {
    /* A loop in value position evaluates to nil, unless a valued `break`
       supplies a value. When it can, declare a poly result temp defaulting to
       nil, point the break-value target at it while emitting the loop, and yield
       the temp. */
    if (loop_has_valued_break(c, nt_ref(nt, id, "statements"))) {
      int tr = ++g_tmp;
      const char *saved_bv = g_loop_break_var;
      int saved_rp = g_ie_res_poly;
      char bvbuf[24]; snprintf(bvbuf, sizeof bvbuf, "_t%d", tr);
      g_loop_break_var = bvbuf; g_ie_res_poly = 1;
      if (g_pre) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tr, tr);
        emit_while(c, id, g_pre, g_indent, sp_streq(ty, "UntilNode"));
        buf_printf(b, "_t%d", tr);
      }
      else {
        /* No prelude available (e.g. inside another expression's prelude): fall
           back to a GCC statement expression, mirroring BeginNode. */
        buf_puts(b, "({ ");
        buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tr, tr);
        emit_while(c, id, b, 0, sp_streq(ty, "UntilNode"));
        buf_printf(b, "_t%d; })", tr);
      }
      g_loop_break_var = saved_bv; g_ie_res_poly = saved_rp;
      return;
    }
    /* No valued break: run for side effects inside a statement-expression and
       yield the boxed nil. */
    buf_puts(b, "({ ");
    emit_while(c, id, b, 0, sp_streq(ty, "UntilNode"));
    buf_puts(b, "sp_box_nil(); })");
    return;
  }
  if (sp_streq(ty, "ForNode")) {
    /* A for loop in value position evaluates to its collection, or to a
       `break`'s value (nil for a bare one). The collection -- for a range
       literal, its endpoints -- is evaluated once into temps that the loop
       then reads through the argument overrides. */
    int coll = unwrap_parens(c, nt_ref(nt, id, "collection"));
    int subs[2] = { -1, -1 }, ns = 0;
    if (coll >= 0 && nt_kind(nt, coll) == NK_RangeNode && comp_ntype(c, coll) == TY_RANGE) {
      subs[ns++] = nt_ref(nt, coll, "left");
      subs[ns++] = nt_ref(nt, coll, "right");
    }
    else subs[ns++] = coll;
    if (g_n_argov + ns > MAX_ARG_OVERRIDE) unsupported(c, id, "for loop value nested too deep");
    Buf lb; memset(&lb, 0, sizeof lb);
    int ind = g_pre ? g_indent : 0;
    int sv_argov = g_n_argov;
    for (int i = 0; i < ns; i++) {
      if (subs[i] < 0) continue;
      TyKind st = comp_ntype(c, subs[i]);
      int ts = ++g_tmp;
      Buf eb; memset(&eb, 0, sizeof eb);
      emit_expr(c, subs[i], &eb);
      emit_indent(&lb, ind);
      buf_printf(&lb, "%s _t%d = %s; ", c_type_name(st), ts, eb.p ? eb.p : "0");
      emit_gc_root_tmp(c, st, ts, &lb);
      buf_puts(&lb, "\n");
      free(eb.p);
      view_bind(subs[i], "_t%d", ts);
    }
    int tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    if (coll >= 0) emit_boxed(c, coll, &rb);
    emit_indent(&lb, ind);
    buf_printf(&lb, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tr, rb.p ? rb.p : "sp_box_nil()", tr);
    free(rb.p);
    const char *saved_bv = g_loop_break_var, *saved_bs = g_brk_ser_var;
    int saved_rp = g_ie_res_poly;
    char bvbuf[24]; snprintf(bvbuf, sizeof bvbuf, "_t%d", tr);
    g_loop_break_var = bvbuf; g_ie_res_poly = 1; g_brk_ser_var = NULL;
    emit_for(c, id, &lb, ind);
    g_loop_break_var = saved_bv; g_ie_res_poly = saved_rp; g_brk_ser_var = saved_bs;
    view_unbind(sv_argov);
    if (g_pre) {
      if (lb.p) buf_puts(g_pre, lb.p);
      buf_printf(b, "_t%d", tr);
    }
    else buf_printf(b, "({ %s_t%d; })", lb.p ? lb.p : "", tr);
    free(lb.p);
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
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
      emit_guarded_poly_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_PolyArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_INT_ARRAY) {
      buf_printf(b, "({ sp_IntArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; sp_int _t%d = sp_IntArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%s(_t%d == SP_INT_NIL)) { ", is_or2 ? "" : "!", tc2);
      emit_guarded_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_IntArray_set%s(_t%d, _t%d, _t%d); } _t%d; })", nil_store_sfx(c, "Int", iv), ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_FLOAT_ARRAY) {
      buf_printf(b, "({ sp_FloatArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; sp_float _t%d = sp_FloatArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%ssp_float_is_nil(_t%d)) { ", is_or2 ? "" : "!", tc2);
      emit_guarded_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_FloatArray_set%s(_t%d, _t%d, _t%d); } _t%d; })", nil_store_sfx(c, "Float", iv), ta2, tb2, tc2, tc2);
    }
    else if (irt == TY_STR_ARRAY) {
      buf_printf(b, "({ sp_StrArray *_t%d = ", ta2); emit_expr(c, ir, b);
      buf_printf(b, "; sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b);
      buf_printf(b, "; const char *_t%d = sp_StrArray_get(_t%d, _t%d);", tc2, ta2, tb2);
      buf_printf(b, " if (%s_t%d) { ", is_or2 ? "!" : "", tc2);
      emit_guarded_slot_assign(c, iv, tc2, b);
      buf_printf(b, "; sp_StrArray_set(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
    }
    else if (ty_is_hash(irt)) {
      const char *hn = ty_hash_cname(irt);
      TyKind kt = ty_hash_key(irt);
      TyKind vt = ty_hash_val(irt);
      if (!hn) { unsupported(c, id, "index-or/and-write (expr, unknown hash)"); return; }
      /* The receiver's temp is the hash's only holder once the key or the
         value drops it, and a key that can allocate has no holder at all:
         root the receiver when either can -- a call, a write -- and the key
         when it can allocate, as the statement form does. */
      int drops = subtree_has_side_effect(c, iav[0]) || subtree_has_side_effect(c, iv);
      buf_printf(b, "({ %s _t%d = ", c_type_name(irt), ta2); emit_expr(c, ir, b); buf_puts(b, "; ");
      if (subtree_may_allocate(c->nt, ir) || drops) { emit_gc_root_tmp(c, irt, ta2, b); buf_puts(b, " "); }
      buf_printf(b, "%s _t%d = ", c_type_name(kt), tb2); emit_hash_key(c, iav[0], kt, b); buf_puts(b, "; ");
      if (subtree_may_allocate(c->nt, iav[0]) && needs_root(kt)) { emit_gc_root_tmp(c, kt, tb2, b); buf_puts(b, " "); }
      if (vt == TY_POLY) {
        buf_printf(b, "sp_RbVal _t%d = sp_%sHash_get(_t%d, _t%d);", tc2, hn, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, _t%d, _t%d); } _t%d; })", hn, ta2, tb2, tc2, tc2);
      }
      else {
        buf_printf(b, "%s _t%d = sp_%sHash_get(_t%d, _t%d);", c_type_name(vt), tc2, hn, ta2, tb2);
        buf_puts(b, " if (");
        emit_slot_nil_test(c, vt, tc2, is_or2, b);
        buf_puts(b, ") { ");
        emit_guarded_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_%sHash_set(_t%d, _t%d, _t%d); } _t%d; })", hn, ta2, tb2, tc2, tc2);
      }
    }
    else if (irt == TY_POLY) {
      /* TY_POLY receiver: dispatch via sp_poly_get/set based on key type */
      TyKind kt2 = comp_ntype(c, iav[0]);
      buf_printf(b, "({ sp_RbVal _t%d = ", ta2); emit_expr(c, ir, b);
      buf_puts(b, "; ");
      if (kt2 == TY_INT) {
        buf_printf(b, "sp_int _t%d = ", tb2); emit_int_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_poly_arr_set_hash(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else if (kt2 == TY_STRING) {
        buf_printf(b, "const char *_t%d = ", tb2); emit_expr(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_get_str(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
        buf_printf(b, "; sp_poly_set_str(_t%d, _t%d, _t%d); } _t%d; })", ta2, tb2, tc2, tc2);
      }
      else {
        /* poly key fallback: box the key, look up polymorphically, set via poly */
        buf_printf(b, "sp_RbVal _t%d = ", tb2); emit_boxed(c, iav[0], b); buf_puts(b, "; ");
        buf_printf(b, "sp_RbVal _t%d = sp_poly_index_poly(_t%d, _t%d);", tc2, ta2, tb2);
        buf_printf(b, " if (%ssp_poly_truthy(_t%d)) { ", is_or2 ? "!" : "", tc2);
        emit_guarded_poly_slot_assign(c, iv, tc2, b);
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
      /* The __yblk__ proc publishes its result through the universal boxed
         return channel (_sp_proc_poly_ret); call it for effect, then take the
         raw carrier bits from the slot (v.i aliases the pointer/int value),
         matching this lowered method's sp_int raw-carrier ABI -- the call site
         casts back to the yield's inferred type, exactly as before. */
      buf_puts(b, "((void)");
      { Buf rb; memset(&rb, 0, sizeof rb); emit_yblk_ref(&rb);
        emit_proc_yield(c, rb.p ? rb.p : "NULL", yargc, yargv, b); free(rb.p); }
      /* `.v.i` is the raw carrier the lowered method's own sp_int ABI wants,
         and the call site casts it back. A consumer whose slot is the BOXED
         value -- a Thread body's yielded_value, which is an sp_RbVal -- needs
         the whole struct instead, so take the yield's own inferred type as the
         answer to which (#3383). */
      { TyKind yt = comp_ntype(c, id);
        if (yt == TY_POLY) buf_puts(b, ", _sp_proc_poly_ret)");
        else if (yt == TY_INT) buf_puts(b, ", sp_poly_as_int_or_nil(_sp_proc_poly_ret))");   /* a nil answer is the Integer slot's nil */
        else if (yt == TY_UNKNOWN || yt == TY_VOID || yt == TY_NIL) buf_puts(b, ", _sp_proc_poly_ret.v.i)");
        else {
          /* a typed slot other than an Integer's reads the boxed answer by
             its own kind: an array answer landed its raw carrier bits in an
             sp_PolyArray * (the flat_map of a boxed receiver whose class
             list has a Ruby `each`, so the definition was lowered) */
          buf_puts(b, ", ");
          emit_unbox_text(c, yt, "_sp_proc_poly_ret", b);
          buf_puts(b, ")");
        } }
      return;
    }
    if (g_yield_proc_ref) {
      /* Forwarded real-proc block (caller nil-checks its &block): call the proc.
         Unbox to the inline's return-slot type (g_yield_slot_ty), which the
         analyzer may have typed concretely even though sp_proc_call is poly. */
      /* In a proc form the code AROUND the yield was typed from the inlined
         view -- `yield(1) + yield(2)` compiled its operands as sp_int -- so
         unbox to this node's own type. A tail/boxed position goes through the
         boxing helper instead, which asks for the poly form. */
      { /* the node's OWN type is what this expression position wants -- the
           inline's return slot is only the right answer when the yield IS the
           tail (a yield nested in a literal wants the element type, #3688).
           A yield the analyzer typed boxed that is NOT the tail keeps the
           boxed answer: unboxing it to the return slot read the key of
           `h[yield(x)] ||= []` as the hash the method returns. */
        TyKind _ynt = comp_ntype(c, id);
        int _ytail = 1;
        if (_ynt == TY_POLY && !g_pf_emitting) {
          Scope *_ys = comp_scope_of(c, id);
          int _last = _ys ? scope_body_last(c, (int)(_ys - c->scopes)) : -1;
          if (_last >= 0 && _last != id) {
            /* the tail may be an `if block_given?` whose block arm ends here */
            int _tl = -1;
            if (nt_kind(nt, _last) == NK_IfNode) {
              int _ts = nt_ref(nt, _last, "statements");
              int _tn = 0; const int *_tb = _ts >= 0 ? nt_arr(nt, _ts, "body", &_tn) : NULL;
              if (_tn > 0) _tl = _tb[_tn - 1];
            }
            if (_tl != id) _ytail = 0;
          }
        }
        emit_yield_proc_call(c, nt_ref(nt, id, "arguments"),
                             (g_pf_emitting || (_ynt != TY_UNKNOWN && _ynt != TY_POLY) || (_ynt == TY_POLY && !_ytail))
                               ? _ynt : g_yield_slot_ty,
                             b, 0, 1); }
      return;
    }
    if (g_block_id < 0) {
      /* An unguarded yield with no block raises LocalJumpError. A guarded yield
         (`block_given? ? yield : x`) folds its guard to a compile-time false and
         sits inside an `if (0)`, so the raise never executes there. The trailing
         sentinel keeps the comma expression well-typed for the value position. */
      buf_puts(b, "(sp_exc_stage_key(sp_box_str((&(\"\\xff\" \"noreason\")[1]))), "
                  "sp_raise_cls(\"LocalJumpError\", \"no block given (yield)\"), ");
      /* the sentinel in the slot's own C type: this dead arm still has to
         assign to whatever the yield's value was typed as (a String local
         took an sp_int and the C build stopped) */
      { TyKind yt = comp_ntype(c, id);
        if (yt == TY_POLY || yt == TY_UNKNOWN || yt == TY_NIL || yt == TY_VOID) buf_puts(b, "sp_box_nil())");
        else if (yt == TY_INT) buf_puts(b, "SP_INT_NIL)");
        else { buf_puts(b, default_value_from_compiler(c, yt)); buf_puts(b, ")"); } }
      return;
    }
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1, comp_ntype(c, id));
    return;
  }
  if (is_block_call(c, id)) {           /* block.call used for its value */
    emit_block_invoke(c, nt_ref(nt, id, "arguments"), b, 0, 1, comp_ntype(c, id));
    return;
  }
  /* `blk.nil?` / `!blk` on an inlined method's own block parameter: the
     block is spliced, not a value, so the answer is whether this site has
     one (a forwarded real proc answers by its pointer). The read itself
     would name lv_<blk>, which no inline site declares (#4477). */
  {
    const char *pn = nt_str(nt, id, "name");
    int pr = nt_ref(nt, id, "receiver");
    if (pn && (sp_streq(pn, "nil?") || sp_streq(pn, "!")) && pr >= 0 &&
        nt_kind(nt, pr) == NK_LocalVariableReadNode) {
      int pan = 0; int paa = nt_ref(nt, id, "arguments");
      if (paa >= 0) nt_arr(nt, paa, "arguments", &pan);
      const char *rn = nt_str(nt, pr, "name");
      Scope *ps = rn ? comp_scope_of(c, pr) : NULL;
      if (pan == 0 && ps && ps->yields && ps->blk_param && rn && sp_streq(ps->blk_param, rn)) {
        if (g_block_id >= 0) buf_puts(b, "0");
        else if (g_yield_proc_ref) buf_printf(b, "((%s) == NULL)", g_yield_proc_ref);
        else buf_puts(b, "1");
        return;
      }
    }
  }
  if (is_blockless_block_param_call(c, id)) {
    /* A forwarded real proc (caller nil-checks its &block): <blk>.call(args)
       invokes the proc. Otherwise this site passed no block, so the parameter
       is nil and the call raises NoMethodError, as CRuby's does. It is not a
       dead path: `def m(&b) = b.call(1)` called bare reaches it, and emitting
       only the result's default value made the call silently do nothing. */
    if (g_yield_proc_ref)
      /* In a proc form the code AROUND the yield was typed from the inlined
         view -- `yield(1) + yield(2)` compiled its operands as sp_int -- so
         unbox to this node's own type. A tail/boxed position goes through the
         boxing helper instead, which asks for the poly form. */
      { /* the node's OWN type is what this expression position wants -- the
           inline's return slot is only the right answer when the yield IS the
           tail (a yield nested in a literal wants the element type, #3688) */
        TyKind _ynt = comp_ntype(c, id);
        /* A POLY node type is an answer, not an absence: the inline's return
           slot is only right when the call IS the tail, and a `&blk` forwarded
           into a map wants the element (boxed) form -- unboxing to the
           method's array type did not build (#3886). */
        emit_yield_proc_call(c, nt_ref(nt, id, "arguments"),
                             (g_pf_emitting || _ynt != TY_UNKNOWN)
                               ? _ynt : g_yield_slot_ty,
                             b, 0, 1); }
    else if (nt_str(nt, id, "call_operator") && sp_streq(nt_str(nt, id, "call_operator"), "&.")) {
      /* `blk&.call`: the nil parameter answers nil instead of raising */
      const char *nv = nil_value(comp_ntype(c, id));
      buf_puts(b, nv ? nv : default_value_from_compiler(c, comp_ntype(c, id)));
    }
    else {
      TyKind _bt = comp_ntype(c, id);
      buf_printf(b, "((void)sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_nil())), %s)",
                 blockless_block_param_call_name(c, id),
                 (_bt == TY_POLY || _bt == TY_UNKNOWN || _bt == TY_NIL || _bt == TY_VOID)
                   ? "sp_box_nil()" : default_value_from_compiler(c, _bt));
    }
    return;
  }
  if (sp_streq(ty, "SelfNode")) {
    /* a statement of a class or module body: the class object itself */
    { int cb = self_class_body(c, id);
      if (cb >= 0) { buf_printf(b, "((sp_Class){%d})", cb); return; } }
    /* top-level self is main, which no C scope holds (#4926) */
    if (self_is_main(c, id)) { buf_puts(b, "sp_main_self()"); return; }
    /* self inside a method added to Array (desugar_builtin_reopen_self_calls
       marks it): the method holds self boxed, read here as the poly array */
    if (nt_int(c->nt, id, "ary_self", 0) && g_self && sp_streq(g_self, "self") &&
        comp_ntype(c, id) == TY_POLY_ARRAY) {
      buf_puts(b, "sp_poly_to_poly_array(self)");
      return;
    }
    /* An inherited method inlined at a call site (a yielding one, say) binds
       self to the receiver temp, typed as the receiver's class, while the
       body's own typing reads self as the defining class: the standalone
       function takes `sp_Parent *self`. A value use of self (a temp, a local,
       an argument) is a Parent slot, so spell the upcast C needs (#6769). */
    if (g_emitting_class_id >= 0 && g_self_deref && sp_streq(g_self_deref, "->"))
      emit_obj_upcast_prefix(c, comp_ntype(c, id), ty_object(g_emitting_class_id), b);
    buf_puts(b, g_self); return;   /* self is the object reference (pointer) */
  }
  if (sp_streq(ty, "InstanceVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@x" */
    Scope *cs = comp_scope_of(c, id);
    /* A POLY ivar handed to a parameter the callee appends to in place: a
       plain String it holds becomes the shared handle and is stored back
       first, as a POLY local's marked read is (poly_strbuf_lift); an
       instance's field store takes its write barrier from gc_wb_insert. */
    if (c->poly_strbuf_lift[id] && !g_ie_nil_ivars && comp_ntype(c, id) == TY_POLY) {
      int vl = view_push_repr(c, id, VR_POLY_LIFT, 0);
      Buf rl; memset(&rl, 0, sizeof rl);
      emit_expr_node(c, id, &rl);
      view_pop(c, vl);
      emit_poly_lift_ref(rl.p ? rl.p : "", b);
      free(rl.p);
      return;
    }
    if (g_ie_nil_ivars) {
      TyKind it = comp_ntype(c, id);
      const char *nv = nil_value(it);
      buf_puts(b, nv ? nv : default_value_from_compiler(c, it));
      return;
    }
    /* inside a shared-mutable shim over THIS slot: both the reads and the
       arm's own write-back go to the shadow (see codegen_internal.h) */
    if (g_sb_iv_name && nm && sp_streq(nm, g_sb_iv_name) &&
        strbuf_ivar_owner(c, id) == g_sb_iv_cid) {
      buf_printf(b, "%s", g_sb_iv_repl);
      return;
    }
    /* a shared-mutable string slot: a marked read yields the live HANDLE, an
       ordinary read a GC copy of the current contents (NULL stays nil) (#3227) */
    { char srefI[1024];
      int svm = c->strbuf_box[id];
      int vsm = view_push_repr(c, id, VR_STRBUF_BOX, 1);   /* let slot_ref resolve regardless of mark */
      int is_sb = strbuf_slot_ref(c, id, srefI, sizeof srefI);
      view_pop(c, vsm);
      if (is_sb) {
        if (svm) buf_printf(b, "%s", srefI);
        else buf_printf(b, "(_sp_ret_strbuf = (void *)%s, %s ? sp_str_concat(sp_String_cstr(%s), (&(\"\\xff\")[1])) : NULL)",
                        srefI, srefI, srefI);
        return;
      } }
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      buf_printf(b, "civ_%s_%s", c->classes[cs->class_id].name, iv_c(nm + 1));  /* module/class-level ivar */
    else if (cs && cs->class_id < 0 && g_ie_class_id >= 0) {
      /* inside instance_eval block: access ivar via receiver pointer; one
         the receiver's class never writes is nil, as on that object (the
         struct has no field for it) */
      int has = 0;
      for (int k = g_ie_class_id; k >= 0 && !has; k = c->classes[k].parent)
        has = comp_ivar_index(&c->classes[k], nm) >= 0;
      if (!has) {
        TyKind it = comp_ntype(c, id);
        const char *nv = nil_value(it);
        buf_puts(b, nv ? nv : default_value_from_compiler(c, it));
      }
      else buf_printf(b, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    }
    else if (cs && cs->class_id < 0) {
      /* top-level method: ivar stored as file-scope global in Toplevel pseudo-class */
      int tl = comp_class_index(c, "Toplevel");
      if (tl >= 0) buf_printf(b, "civ_Toplevel_%s", iv_c(nm + 1));
      else buf_printf(b, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    }
    else
      buf_printf(b, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
    return;
  }
  if (sp_streq(ty, "ClassVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");  /* "@@x" */
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid >= 0) {
      cid = comp_cvar_owner(c, cid, nm);
      buf_printf(b, "cvar_%s_%s", c->classes[cid].name, nm + 2);
      return;
    }
    unsupported(c, id, "class variable read (no class scope)");
  }
  if (sp_streq(ty, "ClassVariableWriteNode")) {  /* in value position: yields the assigned value */
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable write (no class scope)"); return; }
    cid = comp_cvar_owner(c, cid, nm);
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[cid], nm);
    if (idx >= 0) ct = c->classes[cid].cvar_types[idx];
    buf_printf(b, "(cvar_%s_%s = ", c->classes[cid].name, nm + 2);
    if (emit_empty_container_for_slot(c, v, ct, b)) { /* emitted at the slot's type */ }
    else if (ct == TY_POLY) emit_boxed(c, v, b);
    else if (emit_array_into_poly_slot(c, ct, v, b)) { }
    /* the slot's nil, as the statement form writes it: an endless `def
       self.b = (@@x = nil)` stored the numeric 0 a bare NilNode renders as */
    else if (nt_kind(nt, v) == NK_NilNode && nil_value(ct)) buf_puts(b, nil_value(ct));
    /* a boxed value into a slot typed by its other writes (a writer's
       parameter reached through a `self.class.x =` dispatch, which boxes
       what it passes) is unboxed into the slot, as an ivar's is */
    else if (comp_ntype(c, v) == TY_POLY && ct != TY_UNKNOWN) {
      Buf vb = expr_buf(c, v); emit_unbox_text(c, ct, vb.p ? vb.p : "sp_box_nil()", b); free(vb.p);
    }
    else emit_coerce(c, v, ct, CO_HOLD, "a class variable write", b);
    emit_cvar_set_flag_after(c, cid, nm, b);
    buf_puts(b, ")");
    return;
  }
  if (sp_streq(ty, "GlobalVariableOperatorWriteNode")) {
    /* `$g += v` as a value: parenthesized assignment yielding the updated
       slot, the same shapes as the statement form (string + concatenates). */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable op-write (unregistered global)"); return; }
    const char *op = nt_str(nt, id, "binary_operator");
    int v = nt_ref(nt, id, "value");
    char gref[256]; snprintf(gref, sizeof gref, "gv_%s", rn);
    if (lv->type == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "(gv_%s = sp_str_concat(gv_%s, ", rn, rn);
      emit_str_expr(c, v, b); buf_puts(b, "))");
    }
    else if (emit_array_op_assign_value(c, gref, lv->type, op, v, b)) { }
    else if (emit_poly_op_assign_value(c, gref, lv->type, op, v, b)) { }
    else if (emit_scalar_op_assign_value(c, gref, lv->type, op, v, lv->nullable_int, b)) { }
    else {
      buf_printf(b, "(gv_%s %s= ", rn, op ? op : "+");
      emit_coerce(c, v, lv->type, CO_HOLD, "the operand of an `op=`", b); buf_puts(b, ")");
    }
    return;
  }
  if (sp_streq(ty, "GlobalVariableWriteNode")) {
    /* `$g = v` as a value: ({ <the write>; gv_g; }) -- the statement form
       builds the slot's value, and the slot is the expression's value */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable write (unregistered global)"); return; }
    buf_puts(b, "({ ");
    emit_stmt_inner(c, id, b, 0);
    buf_printf(b, "gv_%s; })", rn);
    return;
  }
  if (sp_streq(ty, "GlobalVariableOrWriteNode") || sp_streq(ty, "GlobalVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    if (!lv) { unsupported(c, id, "global variable or/and-write (unregistered global)"); return; }
    char gref[256]; snprintf(gref, sizeof gref, "gv_%s", rn);
    emit_slot_orw_value(c, lv->type, gref, nt_ref(nt, id, "value"),
                        sp_streq(ty, "GlobalVariableOrWriteNode"), b);
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
    cid = comp_cvar_owner(c, cid, nm);
    TyKind ct = TY_INT;
    int idx = comp_cvar_index(&c->classes[cid], nm);
    if (idx >= 0) ct = c->classes[cid].cvar_types[idx];
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    if (ct == TY_STRING && op && sp_streq(op, "+")) {
      buf_printf(b, "(%s = sp_str_plus(%s, ", ref, ref);
      emit_str_expr(c, v, b); buf_puts(b, "))");
    }
    else if (emit_array_op_assign_value(c, ref, ct, op, v, b)) { }
    else if (emit_poly_op_assign_value(c, ref, ct, op, v, b)) { }
    else if (emit_scalar_op_assign_value(c, ref, ct, op, v,
                                         idx >= 0 && c->classes[cid].cvar_nullable_int[idx], b)) { }
    else {
      buf_printf(b, "(%s %s= ", ref, op ? op : "+");
      emit_coerce(c, v, ct, CO_HOLD, "the operand of an `op=`", b); buf_puts(b, ")");
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
    cid = comp_cvar_owner(c, cid, nm);
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    int oidx = comp_cvar_index(&c->classes[cid], nm);
    TyKind ot = oidx >= 0 ? c->classes[cid].cvar_types[oidx] : TY_UNKNOWN;
    buf_puts(b, "(");
    emit_slot_truthy(ot, ref, b);
    buf_printf(b, " ? %s : (%s = ", ref, ref);
    if (ot == TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    emit_cvar_set_flag_after(c, cid, nm, b);
    buf_puts(b, "))");
    return;
  }
  if (sp_streq(ty, "ClassVariableAndWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id >= 0 ? s->class_id : g_class_body_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) { unsupported(c, id, "class variable and-write (no class scope)"); return; }
    cid = comp_cvar_owner(c, cid, nm);
    char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[cid].name, nm + 2);
    int aidx = comp_cvar_index(&c->classes[cid], nm);
    TyKind at = aidx >= 0 ? c->classes[cid].cvar_types[aidx] : TY_UNKNOWN;
    buf_puts(b, "(");
    emit_slot_truthy(at, ref, b);
    buf_printf(b, " ? (%s = ", ref);
    if (at == TY_POLY) emit_boxed(c, v, b);
    else emit_expr(c, v, b);
    buf_printf(b, ") : %s)", ref);
    return;
  }
  if (sp_streq(ty, "GlobalVariableReadNode")) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ is the record separator "\n"; $! / $; /
       $, read nil (spinel doesn't honor the split/print-sep defaults) */
    if (nm && sp_streq(nm, "$stdin")) { buf_puts(b, "sp_io_stdin()"); return; }
    /* A program that REASSIGNS $stdout / $stderr gets a real global for it
       (gv_stdout / gv_stderr, NULL until the assignment runs). Reading the
       stream has to consult that global, or `$stderr = $stdout` writes to the
       real stderr anyway and `$stderr == $stdout` answers false (#3406).
       A program that never assigns has no such global and emits as before. */
    if (nm && (sp_streq(nm, "$stdout") || sp_streq(nm, "$stderr"))) {
      const char *base = sp_streq(nm, "$stdout") ? "sp_io_stdout()" : "sp_io_stderr()";
      const char *gv = sp_streq(nm, "$stdout") ? "gv_stdout" : "gv_stderr";
      LocalVar *sv = comp_gvar(c, nm + 1);
      if (sv && sv->type == TY_IO) buf_printf(b, "(%s ? %s : %s)", gv, gv, base);
      else buf_puts(b, base);
      return;
    }
    if (nm && sp_streq(nm, "$/")) { emit_str_literal(b, "\n"); return; }
    if (nm && sp_streq(nm, "$?")) { buf_puts(b, "sp_last_process_status()"); return; }
    if (nm && (sp_streq(nm, "$PROGRAM_NAME") || sp_streq(nm, "$0"))) { buf_puts(b, "sp_program_name"); return; }
    if (nm && sp_streq(nm, "$!")) { buf_puts(b, "((sp_Exception *)sp_cur_handled())"); return; }
    if (nm && (sp_streq(nm, "$;") || sp_streq(nm, "$,"))) { buf_puts(b, "0"); return; }
    /* regex match globals that Prism may emit as GlobalVariableReadNode */
    if (nm && sp_streq(nm, "$~")) { buf_puts(b, "sp_re_last_matchdata()"); return; }
    if (nm && sp_streq(nm, "$&"))  { buf_puts(b, "sp_re_match_str");  return; }
    if (nm && sp_streq(nm, "$`"))                          { buf_puts(b, "sp_re_pre_match()");  return; }
    if (nm && sp_streq(nm, "$'"))                          { buf_puts(b, "sp_re_post_match()"); return; }
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
    if (sp_streq(nm, "$~")) buf_puts(b, "sp_re_last_matchdata()");
    else if (sp_streq(nm, "$&")) buf_puts(b, "sp_re_match_str");
    else if (sp_streq(nm, "$`"))                 buf_puts(b, "sp_re_pre_match()");
    else if (sp_streq(nm, "$'"))                 buf_puts(b, "sp_re_post_match()");
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
    /* `include Math` exposes the module's bare constants (#2600) */
    if (c->has_include_math && nm && !comp_const(c, nm)) {
      if (sp_streq(nm, "PI")) { buf_puts(b, "M_PI"); return; }
      if (sp_streq(nm, "E"))  { buf_puts(b, "M_E"); return; }
    }
    if (nm && sp_streq(nm, "RUBY_DESCRIPTION")) {
      /* The `ruby -v` shape -- engine, version, release and revision,
         platform -- so a harness that records RUBY_DESCRIPTION can tell two spinel builds
         apart. The platform comes from the runtime header at C compile
         time, the same way RUBY_PLATFORM does, so a cross-build names its
         target rather than the host. */
      char lit[256];
      snprintf(lit, sizeof lit, "\"%s [\" SP_RUBY_ARCH \"-\" SP_RUBY_OS \"]\"",
               g_ruby_description ? g_ruby_description : "spinel " SP_RUBY_VERSION);
      emit_engine_const_str("sp_str_ruby_description", lit, b);
      return;
    }
    if (nm && sp_streq(nm, "RUBY_VERSION"))     { emit_engine_const_str("sp_str_ruby_version", "\"" SP_RUBY_VERSION "\"", b); return; }
    if (nm && sp_streq(nm, "RUBY_ENGINE"))      { emit_engine_const_str("sp_str_ruby_engine", "\"spinel\"", b); return; }
    if (nm && sp_streq(nm, "RUBY_ENGINE_VERSION")) { emit_engine_const_str("sp_str_ruby_engine_version", "\"" SP_RUBY_VERSION "\"", b); return; }
    if (nm && sp_streq(nm, "RUBY_PLATFORM"))    { emit_engine_const_str("sp_str_ruby_platform", "SP_RUBY_ARCH \"-\" SP_RUBY_OS", b); return; }
    if (nm && sp_streq(nm, "RUBY_RELEASE_DATE")) { emit_engine_const_str("sp_str_ruby_release_date", "\"2026-09-15\"", b); return; }
    if (nm && sp_streq(nm, "RUBY_REVISION"))    { emit_engine_const_str("sp_str_ruby_revision", "\"229531a6cfbf07e3caef30dbac24a2a3f3fed482\"", b); return; }
    if (nm && sp_streq(nm, "RUBY_PATCHLEVEL"))  { buf_puts(b, "((sp_int)0)"); return; }
    if (nm && sp_streq(nm, "RUBY_COPYRIGHT"))   { emit_engine_const_str("sp_str_ruby_copyright", "\"ruby - Copyright (C) 1993-2026 Yukihiro Matsumoto\"", b); return; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return; }
    if (nm && sp_streq(nm, "STDOUT")) { buf_puts(b, "sp_io_stdout()"); return; }
    if (nm && sp_streq(nm, "STDERR")) { buf_puts(b, "sp_io_stderr()"); return; }
    if (nm && sp_streq(nm, "STDIN"))  { buf_puts(b, "sp_io_stdin()"); return; }
    /* `OpenStruct` as a class value, matching what an OpenStruct's #class
       returns (name-keyed, cls_id -1); require "ostruct" gated (#3155). */
    if (nm && sp_streq(nm, "OpenStruct") && sp_feature_required("ostruct")) {
      buf_puts(b, "((sp_Class){(sp_int)-1, SPL(\"OpenStruct\")})");
      return;
    }
    if (nm) {
      int _cidx = comp_class_index(c, nm);
      if (_cidx >= 0) {
        buf_printf(b, "((sp_Class){%d})", _cidx);  /* user class as value: TY_CLASS unboxed */
      }
      else {
        int _bcid = builtin_class_id(nm);
        if (_bcid != 0)
          buf_printf(b, "((sp_Class){%d})", _bcid);  /* builtin class as value */
        else if (is_builtin_exception_name(nm) || is_builtin_class_name(nm) ||
                 is_builtin_module_name(nm)) {
          /* a builtin class, module or exception class with no cls_id of its
             own (SystemCallError, LoadError, Process, GC, Method, Random): a
             name-backed Class value, like OpenStruct above -- sp_class_eq and
             the boxed form both compare by name, and an instance's #class
             answers the same (`Random.new.class`) */
          buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", nm);
        }
        else {
          /* A constant defined NOWHERE in the program: spinel is closed-world
             and `const_set` only stores into a constant the program already
             defines, so no definition can arrive later. Say so at build time --
             a dropped `require` otherwise reads as an engine bug, since the
             build succeeds and the first request crashes (#3976). It stays a
             warning, not an error: referencing a missing constant to test the
             NameError is legal Ruby, and ruby/spec does exactly that. */
          if (!const_ref_is_rescued(c, id)) warn_undefined_constant(c, id, nm);
          buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), ((sp_Class){-1}))", nm);
        }
      }
    }
    else unsupported(c, id, "constant read");
    return;
  }
  if (sp_streq(ty, "ConstantPathNode")) {
    /* M::CONST -> the flat constant named by the final path component */
    const char *nm = nt_str(nt, id, "name");
    int par_idc = nt_ref(nt, id, "parent");
    const char *par_tyc = par_idc >= 0 ? nt_type(nt, par_idc) : NULL;
    /* the parent's LEAF name also qualifies through a nested path
       (Outer::CSql::TEXT) -- the ffi decl registers under its module's
       unqualified name */
    const char *par_nmc = (par_tyc && (sp_streq(par_tyc, "ConstantReadNode") ||
                                       sp_streq(par_tyc, "ConstantPathNode")))
                          ? nt_str(nt, par_idc, "name") : NULL;
    /* A constant naming a CLASS is a written path (Probe::Block::CODE). A
       constant HOLDING one (BLOCK = Probe::Block; BLOCK::CODE) is a receiver
       whose class only the value knows, so it takes the run-time read below --
       treating it as a path looked the leaf up and answered a top-level
       constant of that name (#4259). */
    if (par_nmc && par_tyc && sp_streq(par_tyc, "ConstantReadNode") &&
        comp_class_index(c, par_nmc) < 0 && !is_builtin_class_name(par_nmc) &&
        comp_const(c, par_nmc))
      par_nmc = NULL;
    /* An ffi_const is parent-qualified; resolve it BEFORE the leaf-keyed
       plain-constant table, or a same-leaf plain constant in another module
       silently claims the reference (and its type). */
    if (par_nmc && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (sp_streq(c->ffi_consts[fci].mod, par_nmc) &&
            sp_streq(c->ffi_consts[fci].name, nm)) {
          buf_printf(b, "((sp_int)%d)", c->ffi_consts[fci].val);
          return;
        }
      }
    }
    /* a ::-scoped BUILTIN class is a first-class Class value (#2840) */
    if (par_nmc && nm) {
      char qbuf[160];
      snprintf(qbuf, sizeof qbuf, "%s::%s", par_nmc, nm);
      int qid = builtin_class_id(qbuf);
      if (qid != 0) {
        buf_printf(b, "((sp_Class){(sp_int)%d, SPL(\"%s\")})", qid, qbuf);
        return;
      }
      /* the Errno:: family (and its id-less siblings) is a name-backed
         Class value; raised exceptions carry this same qualified name */
      if (is_builtin_exception_name(qbuf)) {
        buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", qbuf);
        return;
      }
    }
    /* Errno::ENOENT::Errno: the class's number, from the runtime table
       (the numbers differ by platform) (#4560) */
    if (nm && sp_streq(nm, "Errno") && par_idc >= 0) {
      char pq[160];
      const char *pqn = isa_const_qualname(nt, par_idc, pq, sizeof pq);
      if (pqn && !strncmp(pqn, "Errno::", 7) && is_builtin_exception_name(pqn)) {
        buf_printf(b, "sp_errno_num(\"%s\")", pqn);
        return;
      }
    }
    /* `klass::CODE` where the receiver is a VALUE rather than a written path:
       which constant it names is a run-time question, and answering it with
       the leaf-named one made two different receivers answer the same thing --
       the top-level CODE for both Probe::Block and Probe::Region (#4257). The
       constants themselves are already stored per owner (cst_Probe__Block__CODE
       and cst_Probe__Region__CODE both exist), so switch on the class the
       value carries and read the one that class owns. */
    { TyKind prt = par_idc >= 0 ? comp_ntype(c, par_idc) : TY_UNKNOWN;
    if (nm && par_idc >= 0 && !par_nmc && (prt == TY_CLASS || prt == TY_POLY)) {
      /* A constant is stored under its OWNER's qualified spelling
         (Probe__Block__CODE) while the class's c_name is its leaf (Block), so
         the owner is found by asking each class whether a constant keyed
         "<...>__<c_name>__<nm>" or "<c_name>__<nm>" exists. */
      int ccls[64]; const char *ckey[64]; int nc = 0;
      TyKind ct = TY_UNKNOWN; int uniform = 1;
      for (int k = 0; k < c->nclasses && nc < 64; k++) {
        const char *kn = c->classes[k].c_name;
        if (!kn) continue;
        char tail[512];
        snprintf(tail, sizeof tail, "%s__%s", kn, nm);
        size_t tl = strlen(tail);
        const char *found = NULL; TyKind ft = TY_UNKNOWN;
        for (int ci2 = 0; ci2 < c->nconsts; ci2++) {
          const char *cn2 = c->consts[ci2].name;
          size_t l2 = strlen(cn2);
          if (l2 < tl || strcmp(cn2 + l2 - tl, tail) != 0) continue;
          /* either the whole key, or a component edge before it */
          if (l2 > tl && strncmp(cn2 + l2 - tl - 2, "__", 2) != 0) continue;
          found = cn2; ft = c->consts[ci2].type; break;
        }
        if (!found) continue;
        if (nc == 0) ct = ft;
        else if (ft != ct) uniform = 0;
        ccls[nc] = k; ckey[nc] = found; nc++;
      }
      if (nc > 0 && uniform && ct != TY_UNKNOWN) {
        int tk = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ sp_Class _t%d = ", tk);
        /* a poly slot carries the class boxed; unwrap it to the same key */
        if (prt == TY_POLY) { buf_puts(b, "sp_unbox_class("); emit_boxed(c, par_idc, b); buf_puts(b, ")"); }
        else emit_expr(c, par_idc, b);
        buf_puts(b, "; ");
        emit_ctype(c, ct, b);
        buf_printf(b, " _t%d = %s; switch (_t%d.cls_id) {", tr, default_value_from_compiler(c, ct), tk);
        for (int i = 0; i < nc; i++)
          buf_printf(b, " case %d: _t%d = cst_%s; break;", ccls[i], tr, ckey[i]);
        /* A class with no such constant is CRuby's NameError, not the
           leaf-named constant of some unrelated scope. */
        buf_printf(b, " default: sp_raise_cls(\"NameError\", sp_sprintf("
                      "\"uninitialized constant %%s::%s\", sp_class_to_s(_t%d))); break;",
                   nm, tk);
        buf_printf(b, " } _t%d; })", tr);
        return;
      }
    } }
    LocalVar *cpcv = nm ? comp_const(c, nm) : NULL;
    if (cpcv && cpcv->type != TY_UNKNOWN) { buf_printf(b, "cst_%s", nm); return; }
    if (nm && sp_streq(nm, "ARGV")) { buf_puts(b, "sp_get_ARGV()"); return; }
    if (nm && sp_streq(nm, "ARGF")) { buf_puts(b, "(&sp_argf_obj)"); return; }
    /* well-known module constants */
    if (par_nmc && sp_streq(par_nmc, "Float") && nm) {
      if (sp_streq(nm, "MAX"))      { buf_puts(b, "DBL_MAX"); return; }
      if (sp_streq(nm, "MIN"))      { buf_puts(b, "DBL_MIN"); return; }
      if (sp_streq(nm, "EPSILON"))  { buf_puts(b, "DBL_EPSILON"); return; }
      if (sp_streq(nm, "INFINITY")) { buf_puts(b, "(1.0/0.0)"); return; }
      if (sp_streq(nm, "NAN"))      { buf_puts(b, "(0.0/0.0)"); return; }
      /* DIG/MANT_DIG/RADIX and the exponent limits are Integer constants */
      if (sp_streq(nm, "DIG"))       { buf_printf(b, "((sp_int)DBL_DIG)"); return; }
      if (sp_streq(nm, "MANT_DIG"))  { buf_printf(b, "((sp_int)DBL_MANT_DIG)"); return; }
      if (sp_streq(nm, "RADIX"))     { buf_printf(b, "((sp_int)FLT_RADIX)"); return; }
      if (sp_streq(nm, "MAX_EXP"))    { buf_printf(b, "((sp_int)DBL_MAX_EXP)"); return; }
      if (sp_streq(nm, "MIN_EXP"))    { buf_printf(b, "((sp_int)DBL_MIN_EXP)"); return; }
      if (sp_streq(nm, "MAX_10_EXP")) { buf_printf(b, "((sp_int)DBL_MAX_10_EXP)"); return; }
      if (sp_streq(nm, "MIN_10_EXP")) { buf_printf(b, "((sp_int)DBL_MIN_10_EXP)"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Math") && nm) {
      if (sp_streq(nm, "PI")) { buf_puts(b, "M_PI"); return; }
      if (sp_streq(nm, "E"))  { buf_puts(b, "M_E"); return; }
    }
    /* Regexp option constants: CRuby's public bits (IGNORECASE=1, EXTENDED=2,
       MULTILINE=4) the same integers Regexp.new's option arg accepts, and the
       encoding bits Regexp#options reports (FIXEDENCODING=16, NOENCODING=32). */
    if (par_nmc && sp_streq(par_nmc, "Regexp") && nm) {
      if (sp_streq(nm, "IGNORECASE")) { buf_puts(b, "((sp_int)1)"); return; }
      if (sp_streq(nm, "EXTENDED"))   { buf_puts(b, "((sp_int)2)"); return; }
      if (sp_streq(nm, "MULTILINE"))  { buf_puts(b, "((sp_int)4)"); return; }
      if (sp_streq(nm, "FIXEDENCODING")) { buf_puts(b, "((sp_int)16)"); return; }
      if (sp_streq(nm, "NOENCODING"))    { buf_puts(b, "((sp_int)32)"); return; }
    }
    /* well-known Encoding constants -> the matching boxed encoding value.
       Spinel has one internal representation (UTF-8 / ASCII-8BIT), so the
       many aliases map onto those two; enough for the pervasive
       `str.encoding == Encoding::UTF_8` comparison. */
    if (par_nmc && sp_streq(par_nmc, "Encoding") && nm) {
      if (sp_streq(nm, "UTF_8") || sp_streq(nm, "UTF8")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_utf8())"); return;
      }
      if (sp_streq(nm, "US_ASCII") || sp_streq(nm, "ASCII") || sp_streq(nm, "ANSI_X3_4_1968")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_us_ascii())"); return;
      }
      if (sp_streq(nm, "BINARY") || sp_streq(nm, "ASCII_8BIT")) {
        buf_puts(b, "sp_box_encoding(sp_encoding_binary())"); return;
      }
      /* Every other Encoding constant is a named Encoding value the runtime
         does not transcode to or from (String#encode leaves the bytes alone
         for it). It used to be an undefined constant, which never showed
         while `encode` ignored its argument; the CRuby name is the constant's
         with `_` as `-`, except the handful CRuby spells otherwise. */
      { static const char *const ENC[][2] = {
          {"SHIFT_JIS","Shift_JIS"}, {"SJIS","Shift_JIS"}, {"WINDOWS_31J","Windows-31J"},
          {"CP932","Windows-31J"}, {"EUC_JP","EUC-JP"}, {"EUCJP","EUC-JP"}, {"UTF_16","UTF-16"},
          {"UTF_16BE","UTF-16BE"}, {"UTF_16LE","UTF-16LE"}, {"UTF_32","UTF-32"},
          {"UTF_32BE","UTF-32BE"}, {"UTF_32LE","UTF-32LE"}, {"ISO_8859_1","ISO-8859-1"},
          {"ISO8859_1","ISO-8859-1"}, {"WINDOWS_1252","Windows-1252"}, {"CP1252","Windows-1252"},
          {"UTF_7","UTF-7"}, {"BIG5","Big5"}, {"GBK","GBK"}, {"GB18030","GB18030"},
          {"EUC_KR","EUC-KR"}, {"KOI8_R","KOI8-R"}, {NULL,NULL} };
        const char *ename = NULL;
        for (int k = 0; ENC[k][0]; k++) if (sp_streq(nm, ENC[k][0])) { ename = ENC[k][1]; break; }
        char dashed[96];
        if (!ename) {
          int ok = 1; size_t i = 0;
          for (; nm[i] && i + 1 < sizeof dashed; i++) {
            char ch = nm[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) { ok = 0; break; }
            dashed[i] = ch == '_' ? '-' : ch;
          }
          dashed[i] = 0;
          if (ok && i > 0) ename = dashed;
        }
        if (ename) {
          buf_puts(b, "sp_box_encoding((sp_Encoding){");
          emit_str_literal(b, ename);   /* marker-framed, like every literal */
          buf_puts(b, "})");
          return;
        }
      }
    }
    if (par_nmc && sp_streq(par_nmc, "File") && nm) {
      /* Emit marker-framed literals (\xff prefix at [-1]) like every other
         spinel string, so sp_str_byte_len/sp_str_concat can read the length
         marker without an out-of-bounds [-1] over-read on a bare literal. */
      if (sp_streq(nm, "SEPARATOR"))      { buf_puts(b, "(&(\"\\xff\" \"/\")[1])"); return; }
      if (sp_streq(nm, "PATH_SEPARATOR")) { buf_puts(b, "(&(\"\\xff\" \":\")[1])"); return; }
      if (sp_streq(nm, "ALT_SEPARATOR"))  { buf_puts(b, "((const char *)0)"); return; }  /* nil off Windows (#2781) */
      /* the null device, the name Process.spawn/File.open take to discard a
         stream. Windows is not a target, so it is /dev/null (#4284). */
      if (sp_streq(nm, "NULL"))           { buf_puts(b, "(&(\"\\xff\" \"/dev/null\")[1])"); return; }
      /* the open(2) flag constants, via the C macros (#2788) */
      if (sp_streq(nm, "RDONLY"))   { buf_puts(b, "((sp_int)O_RDONLY)"); return; }
      if (sp_streq(nm, "WRONLY"))   { buf_puts(b, "((sp_int)O_WRONLY)"); return; }
      if (sp_streq(nm, "RDWR"))     { buf_puts(b, "((sp_int)O_RDWR)"); return; }
      if (sp_streq(nm, "CREAT"))    { buf_puts(b, "((sp_int)O_CREAT)"); return; }
      if (sp_streq(nm, "EXCL"))     { buf_puts(b, "((sp_int)O_EXCL)"); return; }
      if (sp_streq(nm, "TRUNC"))    { buf_puts(b, "((sp_int)O_TRUNC)"); return; }
      if (sp_streq(nm, "APPEND"))   { buf_puts(b, "((sp_int)O_APPEND)"); return; }
      if (sp_streq(nm, "NONBLOCK")) { buf_puts(b, "((sp_int)O_NONBLOCK)"); return; }
      if (sp_streq(nm, "BINARY"))   { buf_puts(b, "((sp_int)0)"); return; }
      /* the flock(2) operation constants (#2808) */
      if (sp_streq(nm, "LOCK_SH")) { buf_puts(b, "((sp_int)LOCK_SH)"); return; }
      if (sp_streq(nm, "LOCK_EX")) { buf_puts(b, "((sp_int)LOCK_EX)"); return; }
      if (sp_streq(nm, "LOCK_UN")) { buf_puts(b, "((sp_int)LOCK_UN)"); return; }
      if (sp_streq(nm, "LOCK_NB")) { buf_puts(b, "((sp_int)LOCK_NB)"); return; }
    }
    if (par_nmc && (sp_streq(par_nmc, "IO") || sp_streq(par_nmc, "File")) && nm) {
      /* IO#seek whence constants (File inherits them from IO); the Ruby
         values 0/1/2 are what sp_File_seek expects. */
      if (sp_streq(nm, "SEEK_SET")) { buf_puts(b, "((sp_int)0)"); return; }
      if (sp_streq(nm, "SEEK_CUR")) { buf_puts(b, "((sp_int)1)"); return; }
      if (sp_streq(nm, "SEEK_END")) { buf_puts(b, "((sp_int)2)"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Process") && nm) {
      if (sp_streq(nm, "CLOCK_MONOTONIC")) { buf_puts(b, "((sp_int)CLOCK_MONOTONIC)"); return; }
      if (sp_streq(nm, "CLOCK_REALTIME"))  { buf_puts(b, "((sp_int)CLOCK_REALTIME)"); return; }
      /* the CPU-time clocks are POSIX and present on Linux and macOS; emit the
         C macro so the value is the platform's own clock id, as CRuby's is. */
      if (sp_streq(nm, "CLOCK_PROCESS_CPUTIME_ID")) { buf_puts(b, "((sp_int)CLOCK_PROCESS_CPUTIME_ID)"); return; }
      if (sp_streq(nm, "CLOCK_THREAD_CPUTIME_ID"))  { buf_puts(b, "((sp_int)CLOCK_THREAD_CPUTIME_ID)"); return; }
      /* getpriority/setpriority `which` selectors (#3046) */
      if (sp_streq(nm, "PRIO_PROCESS")) { buf_puts(b, "((sp_int)PRIO_PROCESS)"); return; }
      if (sp_streq(nm, "PRIO_PGRP"))    { buf_puts(b, "((sp_int)PRIO_PGRP)"); return; }
      if (sp_streq(nm, "PRIO_USER"))    { buf_puts(b, "((sp_int)PRIO_USER)"); return; }
    }
    if (par_nmc && sp_streq(par_nmc, "Integer") && nm &&
        (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) {
      /* Integer::MAX/MIN do not exist in Ruby -- raise NameError at runtime */
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant Integer::%s\"), 0)", nm);
      return;
    }
    /* Socket::<CONST>: the value is platform-dependent, so the runtime (where
       the system headers are in scope) resolves it by name. */
    if (par_nmc && nm && sp_streq(par_nmc, "Socket") && sp_feature_required("socket")) {
      buf_printf(b, "({ sp_int _sc = sp_sock_const(\"%s\");"
                    " if (_sc < 0) sp_raise_cls(\"NameError\","
                    " \"uninitialized constant Socket::%s\"); _sc; })", nm, nm);
      return;
    }
    /* class/module constant as value */
    if (nm) {
      int _cpidx = comp_class_index(c, nm);
      if (_cpidx >= 0) { buf_printf(b, "((sp_Class){%d})", _cpidx); return; }
      int _bcpid = builtin_class_id(nm);
      if (_bcpid != 0) { buf_printf(b, "((sp_Class){%d})", _bcpid); return; }
    }
    /* A qualified constant defined nowhere: the same build-time answer as the
       bare form above (#3976). */
    {
      char fullname[512];
      /* CRuby qualifies the name by a NAMED module (`M::Missing`) but not by
         Object, whose constants are the top-level ones (#3976). The written
         path is what goes in the message; see const_path_written. */
      const_path_written(nt, id, fullname, sizeof fullname);
      if (!fullname[0]) {
        if (par_nmc && nm && !sp_streq(par_nmc, "Object"))
          snprintf(fullname, sizeof fullname, "%s::%s", par_nmc, nm);
        else if (nm) snprintf(fullname, sizeof fullname, "%s", nm);
        else snprintf(fullname, sizeof fullname, "?");
      }
      /* a builtin class with no cls_id that only a path names
         (Enumerator::Chain): the name-backed value the bare form above
         gives Random or Process */
      if (is_builtin_class_name(fullname) || is_builtin_module_name(fullname)) {
        buf_printf(b, "((sp_Class){(sp_int)-1, SPL(\"%s\")})", fullname);
        return;
      }
      if (!const_ref_is_rescued(c, id)) warn_undefined_constant(c, id, fullname);
      buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized constant %s\"), ((sp_Class){-1}))", fullname);
    }
    return;
  }
  if (sp_streq(ty, "DefinedNode")) {
    /* defined? examines its argument recursively without evaluating: any
       unresolvable constant anywhere in the subtree makes the whole answer
       nil (CRuby re-checks each reference). */
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
        /* in an instance method, a slot nothing has set yet is not defined
           (ivar_set_kind): asked of self at run time */
        Scope *ds = comp_scope_of(c, v);
        int dcid = ds && !ds->is_cmethod && ds->class_id >= 0 && g_ie_class_id < 0 ? ds->class_id : -1;
        if (res && dcid >= 0 && comp_ivar_index(&c->classes[dcid], inm) >= 0 &&
            ivar_set_kind(c, dcid, inm) == 1) {
          char ex[200], tb[300];
          snprintf(ex, sizeof ex, "%s%siv_%s", g_self, g_self_deref, iv_c(inm + 1));
          buf_printf(b, "(%s ? SPL(\"instance-variable\") : NULL)", ivar_set_test(c, dcid, inm, ex, tb, sizeof tb));
          return;
        }
      }
      else if (sp_streq(vt, "ClassVariableReadNode")) {
        /* set or not is a run-time question: the cvar's __set flag */
        const char *cnm = nt_str(nt, v, "name");
        Scope *cs = comp_scope_of(c, v);
        int cid = cs && cs->class_id >= 0 ? cs->class_id : g_class_body_id;
        if (cid < 0) cid = comp_class_index(c, "Toplevel");
        if (cid >= 0 && cnm) cid = comp_cvar_owner(c, cid, cnm);
        if (cid >= 0 && cnm && comp_cvar_index(&c->classes[cid], cnm) < 0) cid = -1;
        if (cid >= 0 && cnm)
          buf_printf(b, "(cvar_%s_%s__set ? SPL(\"class variable\") : NULL)",
                     c->classes[cid].name, cnm + 2);
        else buf_puts(b, "NULL");
        return;
      }
      else if (sp_streq(vt, "SelfNode")) res = "self";
      else if (sp_streq(vt, "NilNode")) res = "nil";
      else if (sp_streq(vt, "TrueNode")) res = "true";
      else if (sp_streq(vt, "FalseNode")) res = "false";
      else if (sp_streq(vt, "IntegerNode") || sp_streq(vt, "FloatNode") ||
               sp_streq(vt, "StringNode") || sp_streq(vt, "SymbolNode") || sp_streq(vt, "ArrayNode")) res = "expression";
      else if (sp_streq(vt, "GlobalVariableReadNode")) {
        const char *gn = nt_str(nt, v, "name");
        /* runtime-provided special globals exist regardless of writes */
        if (gn && gn[0] == '$' && gn[1] && (!gn[2] || sp_streq(gn + 1, "stdin") ||
            sp_streq(gn + 1, "stdout") || sp_streq(gn + 1, "stderr") ||
            sp_streq(gn + 1, "PROGRAM_NAME"))) {
          const char sg = gn[1];
          if (!gn[2] && (sg == '!' || sg == '~' || sg == '0' || sg == '$' ||
                         sg == '?' || sg == ';' || sg == ',' || sg == '/' ||
                         sg == '\\' || sg == '*' || sg == '&' ||
                         sg == '\'' || sg == '`' || sg == '+'))
            res = "global-variable";
          else if (gn[2]) res = "global-variable";
        }
        for (int kk = 0; kk < nt->count && !res; kk++) {
          const char *kt = nt_type(nt, kk);
          if (kt && (sp_streq(kt, "GlobalVariableWriteNode") || sp_streq(kt, "GlobalVariableOperatorWriteNode")) &&
              gn && nt_str(nt, kk, "name") && sp_streq(nt_str(nt, kk, "name"), gn))
            res = "global-variable";
        }
      }
      else if (sp_streq(vt, "ConstantReadNode")) {
        const char *cn = nt_str(nt, v, "name");
        if (cn) {
          if (comp_const(c, cn) || comp_class_index(c, cn) >= 0) res = "constant";
          if (!res && comp_is_wellknown_const(cn)) res = "constant";
          /* exception classes are constants too (#2767) */
          if (!res && (is_builtin_exception_name(cn) || is_builtin_class_name(cn)))
            res = "constant";
        }
      }
      else if (sp_streq(vt, "ConstantPathNode")) {
        /* a fully-resolved qualified path answers "constant"; any unresolved
           segment leaves nil (the segment walk lives in the guard helpers) */
        if (comp_defined_guard_true(c, id)) res = "constant";
      }
      /* `sym.to_proc` was lowered to a lambda before this point (the
         "stp_arity" mark): it is still the Symbol's method, not an iterator */
      else if (sp_streq(vt, "CallNode") && nt_int(nt, v, "stp_arity", 0))
        res = "method";
      /* an iterator is an expression, whatever it calls */
      else if (sp_streq(vt, "CallNode") && nt_kind(nt, nt_ref(nt, v, "block")) == NK_BlockNode)
        res = "expression";
      /* see desugar_defined_method_call; a guard known nil at compile time
         leaves the receiver unevaluated. A respond_to? left untyped (an IO
         handle's, which spinel refuses to answer) keeps the answer nil. */
      else if (sp_streq(vt, "CallNode") && nt_ref(nt, id, "method_cond") >= 0 &&
               comp_ntype(c, nt_ref(nt, id, "method_cond")) == TY_BOOL) {
        int ng = 0; const int *gs = nt_arr(nt, id, "method_guards", &ng);
        Buf g; memset(&g, 0, sizeof g);
        for (int gi = 0; gi < ng; gi++) {
          Buf gb; memset(&gb, 0, sizeof gb);
          emit_expr(c, gs[gi], &gb);
          if (sp_streq(gb.p, "NULL")) { free(gb.p); free(g.p); buf_puts(b, "NULL"); return; }
          buf_printf(&g, "(%s) && ", gb.p);
          free(gb.p);
        }
        buf_printf(b, "(%s(", g.p ? g.p : "");
        emit_cond(c, nt_ref(nt, id, "method_cond"), b);
        buf_puts(b, ") ? SPL(\"method\") : NULL)");
        free(g.p);
        return;
      }
      else if (sp_streq(vt, "CallNode") && nt_ref(nt, v, "receiver") < 0) {
        const char *cn = nt_str(nt, v, "name");
        if (cn && comp_method_index(c, cn) >= 0) res = "method";
        /* an implicit-self call inside a class resolves through the
           enclosing class's method chain (readers included) */
        else if (cn && comp_scope_of(c, id) && comp_scope_of(c, id)->class_id >= 0 &&
                 (comp_method_in_chain(c, comp_scope_of(c, id)->class_id, cn, NULL) >= 0 ||
                  comp_reader_in_chain(c, comp_scope_of(c, id)->class_id, cn, NULL)))
          res = "method";
        /* builtin kernel functions the compiler always resolves */
        else if (cn) {
          static const char *const kfns[] = {
            "puts", "print", "p", "pp", "require", "require_relative", "raise",
            "loop", "lambda", "proc", "rand", "srand", "gets", "sleep", "exit",
            "format", "sprintf", "printf", "at_exit", "catch", "throw", NULL };
          for (int bi = 0; kfns[bi]; bi++)
            if (sp_streq(cn, kfns[bi])) { res = "method"; break; }
        }
      }
      /* An assignment of any kind (local/ivar/gvar/cvar/constant/index/attr,
         plain or operator) answers "assignment" without evaluating. */
      else if (strstr(vt, "WriteNode") || sp_streq(vt, "MultiWriteNode"))
        res = "assignment";
      /* A composite expression answers "expression" even when its OPERANDS
         are undefined -- defined? never evaluates its argument. */
      else if (sp_streq(vt, "AndNode") || sp_streq(vt, "OrNode") ||
               sp_streq(vt, "NotNode") || sp_streq(vt, "RangeNode") ||
               sp_streq(vt, "HashNode") || sp_streq(vt, "KeywordHashNode") ||
               sp_streq(vt, "InterpolatedStringNode") ||
               sp_streq(vt, "RegularExpressionNode") ||
               sp_streq(vt, "LambdaNode") || sp_streq(vt, "IfNode") ||
               sp_streq(vt, "UnlessNode") || sp_streq(vt, "CaseNode") ||
               sp_streq(vt, "DefinedNode") || sp_streq(vt, "BeginNode") ||
               sp_streq(vt, "ParenthesesNode") ||
               sp_streq(vt, "InterpolatedRegularExpressionNode") ||
               sp_streq(vt, "SourceFileNode") || sp_streq(vt, "SourceLineNode") ||
               sp_streq(vt, "SourceEncodingNode") || sp_streq(vt, "ForNode") ||
               sp_streq(vt, "WhileNode") || sp_streq(vt, "UntilNode"))
        res = "expression";
    }
    /* a CONTAINER literal builds its elements, so an unresolvable constant
       anywhere inside nils the answer; short-circuiting forms (&&/||) do
       not examine their operands (CRuby). */
    if (res && v >= 0 && vt &&
        (sp_streq(vt, "ArrayNode") || sp_streq(vt, "HashNode") ||
         sp_streq(vt, "KeywordHashNode")) &&
        subtree_has_unresolved_const(c, v)) res = NULL;
    /* dynamic cases: the answer depends on runtime state, so emit a
       conditional instead of a compile-time label. */
    if (!res && vt && sp_streq(vt, "BackReferenceReadNode")) {
      /* $& / $~ / $` / $' / $+ are defined only after a successful match;
         the backing slot is NULL before one. */
      buf_puts(b, "(sp_re_match_str ? SPL(\"global-variable\") : NULL)");
      return;
    }
    if (!res && vt && sp_streq(vt, "NumberedReferenceReadNode")) {
      int refn = (int)nt_int(nt, v, "number", 0);
      if (refn >= 1 && refn <= 9) {
        /* value reads use sp_re_captures[n] directly ($N = captures[N]) */
        buf_printf(b, "(sp_re_captures[%d] ? SPL(\"global-variable\") : NULL)", refn);
        return;
      }
    }
    /* defined?(yield) answers "yield" only when the current method actually
       received a block, else nil -- the same runtime question as block_given?.
       An inlined yielding scope statically has a block; a lowered scope tests
       its runtime __yblk__ parameter; any other scope has no block. */
    if (!res && vt && sp_streq(vt, "YieldNode")) {
      if (g_block_id >= 0) { buf_puts(b, "SPL(\"yield\")"); return; }
      if (g_current_scope_is_lowered) {
        buf_puts(b, "(");
        emit_yblk_ref(b);
        buf_puts(b, " != NULL ? SPL(\"yield\") : NULL)");
        return;
      }
      buf_puts(b, "NULL");
      return;
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
    /* The paren's value is its tail's, and a consumer that sees this node
       typed POLY (the union of what several inline copies of the same
       parenthesized body answer) is handed the tail's own C value: a
       `const char *` went into an sp_RbVal slot unboxed and the C build
       stopped (Dir.chdir's block splice, one copy per call site). Box the
       tail where the node's own type says the consumer expects a box. */
    if (comp_ntype(c, id) == TY_POLY) {
      TyKind ptt = comp_ntype(c, bd[n - 1]);
      if (ptt != TY_POLY && ptt != TY_UNKNOWN && ptt != TY_VOID && ptt != TY_NIL) {
        Buf inner; memset(&inner, 0, sizeof inner);
        if (n == 1) { buf_puts(&inner, "("); emit_expr(c, bd[0], &inner); buf_puts(&inner, ")"); }
        else {
          buf_puts(&inner, "({ ");
          for (int j = 0; j < n - 1; j++) emit_stmt(c, bd[j], &inner, 0);
          emit_expr(c, bd[n - 1], &inner);
          buf_puts(&inner, "; })");
        }
        emit_boxed_text(c, ptt, inner.p ? inner.p : "0", b);
        free(inner.p);
        return;
      }
    }
    if (n == 1) {
      buf_puts(b, "("); emit_expr(c, bd[0], b); buf_puts(b, ")");
      return;
    }
    /* Multi-stmt parens: `(s1; s2; expr)` -- run leading stmts, then the
       value. The tail's emission may hoist statements into g_pre (an
       instance_exec splice, a constructed receiver): the shared prelude is
       flushed BEFORE the statement containing this paren, which would run
       the tail's hoisted code ahead of the leading statements. When the tail
       hoists, hoist the leading statements ahead of it too, preserving
       intra-paren order. */
    {
      Buf cap; memset(&cap, 0, sizeof cap);
      Buf vb; memset(&vb, 0, sizeof vb);
      Buf *sv_pre = g_pre; g_pre = &cap;
      emit_expr(c, bd[n - 1], &vb);
      g_pre = sv_pre;
      if (!(cap.p && cap.p[0])) {
        buf_puts(b, "({ ");
        for (int j = 0; j < n - 1; j++) {
          emit_stmt(c, bd[j], b, 0);
        }
        buf_puts(b, vb.p ? vb.p : "0");
        buf_puts(b, "; })");
      }
      else {
        for (int j = 0; j < n - 1; j++) emit_stmt(c, bd[j], g_pre, g_indent);
        buf_puts(g_pre, cap.p);
        TyKind pvt = comp_ntype(c, bd[n - 1]);
        if (is_scalar_ret(pvt) && pvt != TY_VOID && pvt != TY_NIL && pvt != TY_UNKNOWN) {
          int tpv = ++g_tmp;
          emit_indent(g_pre, g_indent); emit_ctype(c, pvt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tpv, vb.p ? vb.p : "0");
          buf_printf(b, "_t%d", tpv);
        }
        else {
          /* no usable scalar value: run the tail for effect, yield nil */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "(void)(%s);\n", vb.p ? vb.p : "0");
          buf_puts(b, "sp_box_nil()");
        }
      }
      free(cap.p); free(vb.p);
    }
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
          /* [*arr] or [*range] -- expand into poly */
          int inner = nt_ref(nt, els[j], "expression");
          if (is_empty_array_lit(nt, inner)) continue;
          TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
          Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
          const char *ep = el.p ? el.p : "NULL";
          emit_indent(g_pre, g_indent);
          if (it == TY_RANGE || it == TY_STR_RANGE) {
            /* check if it's a string range (bounds are TY_STRING) */
            int rn = nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "RangeNode") ? inner : -1;
            int rlo = rn >= 0 ? nt_ref(nt, rn, "left") : -1;
            int rhi = rn >= 0 ? nt_ref(nt, rn, "right") : -1;
            int rexcl = rn >= 0 ? (int)(nt_int(nt, rn, "flags", 0) & 4) : 0;
            if (rlo >= 0 && comp_ntype(c, rlo) == TY_STRING) {
              Buf lo_b; memset(&lo_b, 0, sizeof lo_b); emit_expr(c, rlo, &lo_b);
              Buf hi_b; memset(&hi_b, 0, sizeof hi_b); emit_expr(c, rhi, &hi_b);
              buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }\n",
                         lo_b.p ? lo_b.p : "NULL", hi_b.p ? hi_b.p : "NULL", rexcl, t);
              free(lo_b.p); free(hi_b.p);
            }
            else {
              buf_printf(g_pre, "{ sp_Range _sr = %s; sp_int _e = _sr.last+(_sr.excl?0:1); for (sp_int _si = _sr.first; _si < _e; _si++) sp_PolyArray_push(_t%d, sp_box_int(_si)); }\n", ep, t);
            }
          }
          else if (it == TY_INT_ARRAY) {
            Buf nf; memset(&nf, 0, sizeof nf); emit_may_nil_text(c, inner, it, "_sa", &nf);
            buf_printf(g_pre, "{ sp_IntArray *_sa = %s; int _snf = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s(_snf, _sa->data[_sa->start+_si])); }\n",
                       ep, nf.p, t, typed_elem_box_fn(it));
            free(nf.p);
          }
          else if (it == TY_STR_ARRAY)
            buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }\n", ep, t);
          else if (it == TY_FLOAT_ARRAY) {
            Buf nf; memset(&nf, 0, sizeof nf); emit_may_nil_text(c, inner, it, "_sa", &nf);
            buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; int _snf = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s(_snf, _sa->data[_si])); }\n",
                       ep, nf.p, t, typed_elem_box_fn(it));
            free(nf.p);
          }
          else if (it == TY_POLY_ARRAY)
            buf_printf(g_pre, "{ sp_PolyArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }\n", ep, t);
          else if (it == TY_POLY)
            /* `*poly`: whether it holds an array is only known at runtime, so
               splice one level if it is an array, drop nil, else push as-is
               (CRuby splat semantics). */
            buf_printf(g_pre, "{ sp_RbVal _sv = %s; if (_sv.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_sv.cls_id)) _sv = sp_box_poly_array(sp_poly_to_a_arr(_sv)); if (!sp_poly_nil_p(_sv)) sp_PolyArray_flatten_into_n(_t%d, _sv, 1); }\n", ep, t);
          else if (it == TY_NIL)
            /* a statically-nil splat contributes nothing (`[*nil]` == []) */
            buf_printf(g_pre, ";\n");
          /* a nullable Integer or Float holding its sentinel is that nil */
          else if ((it == TY_INT || it == TY_FLOAT) && call_returns_nullable_int(c, inner)) {
            Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, inner, &bx);
            buf_printf(g_pre, "{ sp_RbVal _sv = %s; if (_sv.tag != SP_TAG_NIL) sp_PolyArray_push(_t%d, _sv); }\n",
                       bx.p ? bx.p : "sp_box_nil()", t);
            free(bx.p);
          }
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
        if (is_empty_array_lit(nt, inner)) continue;
        TyKind it = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        Buf el; memset(&el, 0, sizeof el); emit_expr(c, inner, &el);
        const char *ep = el.p ? el.p : "NULL";
        emit_indent(g_pre, g_indent);
        if (it == TY_RANGE || it == TY_STR_RANGE) {
          int rn2 = nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "RangeNode") ? inner : -1;
          int rlo2 = rn2 >= 0 ? nt_ref(nt, rn2, "left") : -1;
          int rexcl2 = rn2 >= 0 ? (int)(nt_int(nt, rn2, "flags", 0) & 4) : 0;
          if (rlo2 >= 0 && comp_ntype(c, rlo2) == TY_STRING) {
            int rhi2 = nt_ref(nt, rn2, "right");
            Buf lo2; memset(&lo2, 0, sizeof lo2); emit_expr(c, rlo2, &lo2);
            Buf hi2; memset(&hi2, 0, sizeof hi2); emit_expr(c, rhi2, &hi2);
            buf_printf(g_pre, "{ sp_StrArray *_sa = sp_StrArray_from_string_range(%s, %s, %d); if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_si]); }\n",
                       lo2.p ? lo2.p : "NULL", hi2.p ? hi2.p : "NULL", rexcl2, k, t);
            free(lo2.p); free(hi2.p);
          }
          else {
            buf_printf(g_pre, "{ sp_Range _sr = %s; sp_int _e = _sr.last+(_sr.excl?0:1); for (sp_int _si = _sr.first; _si < _e; _si++) sp_%sArray_push(_t%d, _si); }\n", ep, k, t);
          }
        }
        else if (it == TY_INT_ARRAY && sp_streq(k, "Int"))
          buf_printf(g_pre, "{ sp_IntArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_sa->start+_si]); sp_IntArray_nil_from(_t%d, _sa); }\n", ep, k, t, t);
        else if (it == TY_STR_ARRAY && sp_streq(k, "Str"))
          buf_printf(g_pre, "{ sp_StrArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_%sArray_push(_t%d, _sa->data[_si]); }\n", ep, k, t);
        else if (it == TY_FLOAT_ARRAY && sp_streq(k, "Float"))
          buf_printf(g_pre, "{ sp_FloatArray *_sa = %s; if (_sa) for (sp_int _si = 0; _si < _sa->len; _si++) sp_FloatArray_push(_t%d, _sa->data[_si]); sp_FloatArray_nil_from(_t%d, _sa); }\n", ep, t, t);
        else if (it == TY_NIL)
          /* a statically-nil splat contributes nothing (`[*nil]` == []) */
          buf_printf(g_pre, ";\n");
        /* nor does a nullable Integer or Float holding its sentinel */
        else if (((it == TY_INT && sp_streq(k, "Int")) || (it == TY_FLOAT && sp_streq(k, "Float"))) &&
                 call_returns_nullable_int(c, inner)) {
          Buf nz; memset(&nz, 0, sizeof nz);
          emit_slot_truthy(it, "_sv", &nz);
          buf_printf(g_pre, "{ %s _sv = %s; if %s sp_%sArray_push(_t%d, _sv); }\n",
                     it == TY_INT ? "sp_int" : "sp_float", ep, nz.p, k, t);
          free(nz.p);
        }
        else {
          /* Mismatched or unknown element type: emit_expr fallback */
          buf_printf(g_pre, "sp_%sArray_push%s(_t%d, %s);\n", k, nil_store_sfx(c, k, inner), t, ep);
        }
        free(el.p);
      }
else {
        Buf el; memset(&el, 0, sizeof el);
        /* element preludes flow to g_pre first; an untyped element (a raise
           token, a void call) is coerced to the element type */
        if (comp_ntype(c, els[j]) == TY_UNKNOWN) emit_unresolved_coerced(c, els[j], ty_array_elem(at), &el);
        else emit_coerce(c, els[j], ty_array_elem(at), CO_HOLD, "an Array literal's element", &el);
        emit_indent(g_pre, g_indent);
        /* an element that can be nil sets the literal's may_nil */
        buf_printf(g_pre, "sp_%sArray_push%s(_t%d, ", k, nil_store_sfx(c, k, els[j]), t);
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
      const char *ety = nt_type(nt, els[j]);
      if (kwh_elem_dropped(nt, id, j)) { emit_dropped_value(c, nt_ref(nt, els[j], "value"), g_pre); continue; }
      if (ety && sp_streq(ety, "AssocSplatNode")) {
        /* `**h`: merge the spread hash into the fresh literal. In `{ **h, k: v }`
           the splat is emitted before the explicit assocs, so a following key
           overrides the merged one (Ruby's last-wins). Only a same-variant
           source merges directly; a differently-typed spread is rejected loudly
           rather than emitting a layout-mismatching update. */
        int src = nt_ref(nt, els[j], "value");
        TyKind sh = src >= 0 ? comp_ntype(c, src) : TY_UNKNOWN;
        const char *shn = ty_hash_cname(sh);
        int nsrc = -1;
        if (sh == TY_UNKNOWN && src >= 0 && nt_kind(nt, src) == NK_HashNode) nt_arr(nt, src, "elements", &nsrc);
        /* `**{}` adds nothing */
        if (nsrc == 0) continue;
        if (src >= 0 && (sh == TY_NIL || kw_splat_checked_boxed(c, src) || kw_splat_raises(c, src))) {
          /* `**nil`, `**true`, `**1`, which inference takes for a spread of
             nothing: the operand runs where it stands and converts, raising
             CRuby's TypeError unless it is nil -- or it already has, ahead of
             the call's keywords (emit_ds_hash_materialize), and reads as
             nothing here */
          int ran = nt_kind(nt, src) == NK_NilNode;
          for (int i = 0; i < g_n_argov && !ran; i++) ran = g_argov_node[i] == src;
          if (!ran) {
            Buf sb; memset(&sb, 0, sizeof sb); emit_kw_splat_operand_inline(c, src, &sb);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "%s\n", sb.p ? sb.p : "");
            free(sb.p);
          }
          continue;
        }
        if (shn && sp_streq(shn, hn)) {
          /* same-variant source: a direct typed merge. */
          Buf sb; memset(&sb, 0, sizeof sb); emit_expr(c, src, &sb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sHash_update(_t%d, %s);\n", hn, t, sb.p ? sb.p : "");
          free(sb.p);
        }
        else if ((sh == TY_POLY || shn) && poly_poly) {
          /* a poly spread source, or a hash of another variant, into a
             poly-poly literal: its boxed (key,value) pairs set at run time
             (any hash variant). nil spreads nothing, and anything else
             converts through its #to_hash or raises CRuby's TypeError
             (sp_kw_merge_any): the walk alone took a user object or an
             Integer for no pairs at all. */
          int st = ++g_tmp;
          Buf sb; memset(&sb, 0, sizeof sb); emit_boxed(c, src, &sb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", st, sb.p ? sb.p : "sp_box_nil()", st);
          free(sb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_kw_merge_any(_t%d, _t%d);\n", t, st);
        }
        else {
          unsupported(c, id, "hash double-splat of an unmergeable source"); return;
        }
        continue;
      }
      int key = nt_ref(nt, els[j], "key");
      int val = nt_ref(nt, els[j], "value");
      Buf kb; memset(&kb, 0, sizeof kb);
      if (poly_poly) emit_boxed(c, key, &kb);
      else if (ty_is_hash(ht)) emit_coerce(c, key, ty_hash_key(ht), CO_HOLD, "a Hash literal's key", &kb);
      else emit_expr(c, key, &kb);
      Buf vb; memset(&vb, 0, sizeof vb);
      if (sym_poly || poly_poly) emit_boxed(c, val, &vb);
      else if (ty_is_hash(ht)) emit_coerce(c, val, ty_hash_val(ht), CO_HOLD, "a Hash literal's value", &vb);
      else emit_expr(c, val, &vb);
      emit_indent(g_pre, g_indent);
      /* A pair's key and value are the set's sibling arguments, as a store's
         are: a key that can allocate goes into a rooted temp ahead of the
         value's build, as the store arm's does. */
      int tk = -1;
      if (ty_is_hash(ht) && subtree_may_allocate(nt, key)) {
        TyKind kt = ty_hash_key(ht);
        tk = ++g_tmp;
        buf_printf(g_pre, "{ %s _t%d = %s; ", c_type_name(kt), tk, kb.p ? kb.p : "");
        if (needs_root(kt)) { emit_gc_root_tmp(c, kt, tk, g_pre); buf_puts(g_pre, " "); }
      }
      buf_printf(g_pre, "sp_%sHash_set(_t%d, ", hn, t);
      if (tk >= 0) buf_printf(g_pre, "_t%d", tk); else buf_puts(g_pre, kb.p ? kb.p : "");
      buf_puts(g_pre, ", "); buf_puts(g_pre, vb.p ? vb.p : "");
      buf_puts(g_pre, tk >= 0 ? "); }\n" : ");\n");
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
    /* A statically-answered defined?, is_a? or block_given? predicate folds to
       its live arm: the dead arm may not even type-check against the
       receiver's storage type (a blockless yield has no type of its own).
       Mirrors emit_if's statement-form fold and the inference fold. */
    {
      int df = comp_defined_guard_false(c, pred);
      int dt = df ? 0 : comp_defined_guard_true(c, pred);
      int known = df ? 0 : (dt ? 1 : static_isa_cond(c, pred));
      if (known < 0 && !df && !dt) known = static_respond_to_cond(c, pred);
      /* a `block_given? ? a : b` pair: the live arm alone, rendered at the
         result type as the unfolded pair renders each arm */
      if (known < 0 && !df && !dt && tn == 1 && en == 1) {
        int bg = static_block_given_cond(c, pred);
        if (bg >= 0) {
          TyKind res = comp_ntype(c, id);
          if (res == TY_VOID || res == TY_NIL) res = TY_POLY;
          emit_ternary_arm(c, (is_unless ? !bg : bg) ? tb[0] : eb[0], res, b);
          return;
        }
      }
      if (known >= 0) {
        int take_then = is_unless ? !known : known;
        if (!take_then && !is_unless && sub >= 0 && nt_type(nt, sub) &&
            sp_streq(nt_type(nt, sub), "IfNode")) {
          emit_expr(c, sub, b);  /* the elsif chain continues as the value */
          return;
        }
        int live = take_then ? then_b : else_stmts;
        int ln = 0;
        const int *lb = live >= 0 ? nt_arr(nt, live, "body", &ln) : NULL;
        if (ln == 0) { buf_puts(b, "sp_box_nil()"); return; }
        /* The live arm still has to be rendered at the WHOLE expression's
           type, the way the unfolded pair below is: the slot receiving this
           was declared from that type, and the arm's own may be narrower.
           Emitting the arm raw put a `const char *` into an sp_RbVal local
           whenever an is_a? predicate folded (#4280's tmpdir package could
           not compile at its simplest call). */
        TyKind fres = comp_ntype(c, id);
        if (fres == TY_VOID || fres == TY_NIL) fres = TY_POLY;
        /* Only the WIDENING conversion, into a poly result. Forcing a narrower
           one would convert rather than carry -- and where the result type is
           narrower than the arm the analysis has already gone wrong, so the C
           type error that raises is the honest answer, not a silent value. */
        int fbox = fres == TY_POLY && comp_ntype(c, lb[ln - 1]) != TY_POLY;
        if (ln == 1) {
          if (fbox) emit_ternary_arm(c, lb[0], fres, b); else emit_expr(c, lb[0], b);
          return;
        }
        /* The leading statements go into the PRELUDE, not inside a statement
           expression around the value. The last element is emitted with
           emit_expr, and an arm that is itself a conditional hoists its own
           branches into the prelude -- which is emitted before this whole
           expression. Holding the leading statements here instead put them
           AFTER the code that reads what they assign: `range = H[expected]`
           landed below the `if range.nil?` testing it, so the nil arm always
           won and a branch reached its tail without running its own first
           line (#4139). The prelude keeps them in source order. */
        for (int j = 0; j < ln - 1; j++) emit_stmt(c, lb[j], g_pre, g_indent);
        if (fbox) emit_ternary_arm(c, lb[ln - 1], fres, b);
        else emit_expr(c, lb[ln - 1], b);
        return;
      }
    }
    if (tn == 1 && en == 1) {
      TyKind res = comp_ntype(c, id);
      /* A void/nil-typed if (e.g. an arm that is a writer call, doom's
         `self.fullscreen = value if respond_to?(...)`), or an untyped one
         whose every arm diverges, has no C storage type -- emit_ctype would
         declare `void _tN` -- so hold the result boxed; void arms degrade
         to nil. */
      if (res == TY_VOID || res == TY_NIL || res == TY_UNKNOWN) res = TY_POLY;
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
      /* a `raise`/`fail` arm diverges: it produces no value to assign, so it
         cannot ride the flat C ternary (which needs both arms to be values of
         the result type) -- force the branch form and emit it as a statement. */
      int then_raise = node_is_raise(c, tb[0]);
      int else_raise = node_is_raise(c, eb[0]);
      Buf *sv_pre = g_pre;
      g_pre = &pa; emit_ternary_arm(c, tb[0], res, &ta);
      g_pre = &pe; emit_ternary_arm(c, eb[0], res, &te);
      g_pre = sv_pre;
      int hoists = (pa.p && pa.p[0]) || (pe.p && pe.p[0]) || then_raise || else_raise;
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
        buf_puts(b, ") {\n");
        buf_puts(b, pa.p ? pa.p : "");
        /* a diverging (raise/fail) arm is emitted as a statement; the result
           temp keeps its default (never read, since the arm never returns) */
        if (then_raise) buf_printf(b, " %s;\n", ta.p ? ta.p : "0");
        else buf_printf(b, " _t%d = %s;\n", tr, ta.p ? ta.p : "0");
        buf_puts(b, "}\nelse {\n");
        buf_puts(b, pe.p ? pe.p : "");
        if (else_raise) buf_printf(b, " %s;\n", te.p ? te.p : "0");
        else buf_printf(b, " _t%d = %s;\n", tr, te.p ? te.p : "0");
        buf_printf(b, "} _t%d; })", tr);
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
                 res == TY_RANGE ? "(sp_Range){0}" : default_value_from_compiler(c, res));
      /* The temp is set here, in the statement's prelude, and read where the
         `if` stands in the statement, after what is written ahead of it
         there has run: the receiver, in `src(i).merge(note: ("n#{i}" if
         i > 0))`. A value the branch allocated has no other holder in
         between, and a collection there freed it. Root it, as the case/in
         value's temp is rooted. */
      if (ty_gc_holds_refs(c, res)) {
        emit_indent(g_pre, g_indent);
        emit_gc_root_tmp_refs(c, res, tr, g_pre);
        buf_puts(g_pre, "\n");
      }
      /* Emit the condition into its own buffer: any prolog it hoists is a
         statement, and writing it to g_pre after "if (" had been written left
         the declaration in the middle of the expression. */
      Buf cnd; memset(&cnd, 0, sizeof cnd);
      emit_cond(c, pred, &cnd);
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "if (");
      if (is_unless) buf_puts(g_pre, "!(");
      buf_puts(g_pre, cnd.p ? cnd.p : "0");
      if (is_unless) buf_puts(g_pre, ")");
      buf_puts(g_pre, ") {\n");
      free(cnd.p);
      /* Then branch: side-effect stmts, then assign last expr to temp. */
      for (int i = 0; i < tn - 1; i++) emit_stmt(c, tb[i], g_pre, g_indent + 2);
      if (tn > 0) {
        int last_then = tb[tn - 1];
        TyKind lt = comp_ntype(c, last_then);
        /* An empty `[]` / `{}` caches TY_UNKNOWN for a reason that is not
           "has no value": it has no ELEMENT type until something supplies
           one. Running it for effect leaves the slot at its default, which
           is how `if c then [] end` answered nil where CRuby answers [].
           emit_ternary_arm already builds it against a result type. */
        int elen = 0;
        const char *ety = nt_type(nt, last_then);
        int empty_lit = lt == TY_UNKNOWN && ety &&
                        (sp_streq(ety, "ArrayNode") || sp_streq(ety, "HashNode")) &&
                        (nt_arr(nt, last_then, "elements", &elen), elen == 0);
        if (empty_lit) {
          Buf le; memset(&le, 0, sizeof le);
          emit_ternary_arm(c, last_then, res, &le);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "_t%d = %s;\n", tr, le.p ? le.p : default_value_from_compiler(c, res));
          free(le.p);
        }
        else if (lt == TY_NIL || lt == TY_UNKNOWN || lt == TY_VOID) {
          emit_stmt(c, last_then, g_pre, g_indent + 2);
        }
        else emit_if_arm_value(c, last_then, res, tr);
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
            else emit_if_arm_value(c, last_else, res, tr);
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
          buf_printf(g_pre, "_t%d = ", tr);
          /* The nested elsif chain types on its own arms: a concrete chain
             (string/string) under a poly outer if (the empty then-arm's nil)
             must box into the poly temp, same as the then/else arms above. */
          TyKind subt = comp_ntype(c, sub);
          if (res == TY_POLY && subt != TY_POLY && subt != TY_NIL &&
              subt != TY_UNKNOWN && subt != TY_VOID) {
            Buf bx3; memset(&bx3, 0, sizeof bx3);
            emit_boxed_text(c, subt, sub_e.p ? sub_e.p : default_value_from_compiler(c, subt), &bx3);
            buf_puts(g_pre, bx3.p ? bx3.p : "sp_box_nil()"); free(bx3.p);
          }
          /* a nested chain whose every written arm raises is the implicit
             nil, held in a boxed temp; into a concrete slot with a nil of
             its own (a nullable String, #4567) it is that nil, not the box */
          else if (subt == TY_NIL && res != TY_POLY && nil_value(res))
            buf_puts(g_pre, nil_value(res));
          else if (emit_arm_text_as_bigint(res, subt, sub_e.p ? sub_e.p : default_value_from_compiler(c, subt), g_pre)) { }
          else buf_puts(g_pre, sub_e.p ? sub_e.p : default_value_from_compiler(c, res));
          buf_puts(g_pre, ";\n");
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
    /* `v = lookup or return`: the right operand diverges, so it has no value
       for an arm to assign. Run it as a statement inside the short-circuit and
       answer the left, which is the only value the chain can produce (#3777). */
    if (right >= 0 && nt_type(nt, right) &&
        (sp_streq(nt_type(nt, right), "ReturnNode") ||
         sp_streq(nt_type(nt, right), "BreakNode") ||
         sp_streq(nt_type(nt, right), "NextNode"))) {
      TyKind vt = lt;
      if (vt == TY_VOID || vt == TY_UNKNOWN || vt == TY_NIL) vt = TY_POLY;
      int t2 = ++g_tmp;
      Buf lb; memset(&lb, 0, sizeof lb);
      if (vt == TY_POLY && lt != TY_POLY) emit_boxed(c, left, &lb);
      else emit_expr(c, left, &lb);
      Buf tc2; memset(&tc2, 0, sizeof tc2);
      if (vt == TY_POLY)       buf_printf(&tc2, "sp_poly_truthy(_t%d)", t2);
      else if (vt == TY_BOOL)  buf_printf(&tc2, "_t%d", t2);
      else if (vt == TY_INT)   buf_printf(&tc2, "(_t%d != SP_INT_NIL)", t2);
      else if (vt == TY_FLOAT) buf_printf(&tc2, "(!sp_float_is_nil(_t%d))", t2);
      else if (vt == TY_SYMBOL) buf_printf(&tc2, "(_t%d != (sp_sym)-1)", t2);
      else if (vt == TY_CLASS) buf_printf(&tc2, "(!sp_class_nil_p(_t%d))", t2);
      else if (vt == TY_STRING || ty_is_array(vt) || ty_is_hash(vt) || ty_is_object(vt) ||
               vt == TY_PROC || vt == TY_MATCHDATA || vt == TY_EXCEPTION ||
               ty_nullable_builtin_id(vt))
        buf_printf(&tc2, "(_t%d != 0)", t2);
      else buf_puts(&tc2, "1");
      buf_puts(b, "({ ");
      emit_ctype(c, vt, b);
      buf_printf(b, " _t%d = %s; if (%s%s) {\n", t2, lb.p ? lb.p : "0",
                 is_and ? "" : "!", tc2.p ? tc2.p : "1");
      emit_stmt(c, right, b, g_indent + 1);
      buf_printf(b, "}\n _t%d; })", t2);
      free(lb.p); free(tc2.p);
      return;
    }
    if (lt == TY_BOOL && comp_ntype(c, right) == TY_BOOL) {
      /* Capture the right operand's prelude: an object subexpression there
         (`a && b.c == x`) hoists a GC-rooted temp that must run inside the
         short-circuit, not above the whole chain (issue #1773). No prelude ->
         the flat C `&&`/`||`; otherwise a real branch scopes it. */
      Buf rarm; memset(&rarm, 0, sizeof rarm);
      Buf rpre; memset(&rpre, 0, sizeof rpre);
      { Buf *sv_pre2 = g_pre; g_pre = &rpre; emit_expr(c, right, &rarm); g_pre = sv_pre2; }
      if (!(rpre.p && rpre.p[0])) {
        buf_puts(b, "(");
        emit_expr(c, left, b);
        buf_puts(b, is_and ? " && " : " || ");
        buf_puts(b, rarm.p ? rarm.p : "0");
        buf_puts(b, ")");
      }
      else {
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_bool _t%d; if (", tr);
        emit_expr(c, left, b);
        if (is_and) buf_printf(b, ") {\n%s_t%d = %s;\n}\nelse { _t%d = 0; } _t%d; })",
                               rpre.p, tr, rarm.p ? rarm.p : "0", tr, tr);
        else        buf_printf(b, ") { _t%d = 1; }\nelse {\n%s_t%d = %s;\n} _t%d; })",
                               tr, rpre.p, tr, rarm.p ? rarm.p : "0", tr);
      }
      free(rarm.p); free(rpre.p);
      return;
    }
    /* value form: a || b  ->  truthy(a) ? a : b ;  a && b -> truthy(a) ? b : a.
       Evaluate the left once into a temp; results widen to the unified type.
       The result is always a real value (a nil left boxes to sp_box_nil), never
       void -- a VOID/UNKNOWN unified type (`() && true`, an empty-parens operand
       whose cached type settled void) becomes poly so the temp is not `void`. */
    if (res == TY_VOID || res == TY_UNKNOWN || res == TY_NIL) res = TY_POLY;
    int t = ++g_tmp;
    int lt_falsy_const = (lt == TY_NIL || lt == TY_VOID);  /* a nil/void left has no C-typed value */
    buf_puts(b, "({ ");
    emit_ctype(c, (lt == TY_UNKNOWN || lt_falsy_const) ? res : lt, b);
    buf_printf(b, " _t%d = ", t);
    if (lt_falsy_const) {
      buf_puts(b, "("); emit_expr(c, left, b); buf_puts(b, ", ");
      buf_puts(b, res == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, res == TY_UNKNOWN ? TY_INT : res));
      buf_puts(b, ")");
    }
    /* an unresolved (TY_UNKNOWN) left emits a poly fallback (sp_box_nil); coerce
       it to the unified scalar result so the temp's declared type matches. */
    else if (lt == TY_UNKNOWN && res == TY_INT) { buf_puts(b, "sp_poly_to_i_or_nil("); emit_expr(c, left, b); buf_puts(b, ")"); }
    else if (lt == TY_UNKNOWN && res == TY_FLOAT) { buf_puts(b, "sp_poly_to_f_or_nil("); emit_expr(c, left, b); buf_puts(b, ")"); }
    /* a bool-unified chain with an unresolved left (a poly dispatch whose
       node type stayed unknown, e.g. alias-to-reader through a poly element):
       take its truthiness, mirroring the int/float coercions (#3276) */
    else if (lt == TY_UNKNOWN && res == TY_BOOL) { buf_puts(b, "sp_poly_truthy("); emit_expr(c, left, b); buf_puts(b, ")"); }
    /* The left may be an unresolved call emitting an sp_RbVal raise token
       (`x.details || "~"`, where details is typed String from the `||` but
       lowers to sp_raise_nomethod); coerce it to the temp's DECLARED type (res
       when the left itself is unknown) rather than assigning the raw token. A
       normal left emits unchanged. */
    else emit_unresolved_coerced(c, left, lt == TY_UNKNOWN ? res : lt, b);
    buf_puts(b, "; ");
    /* Truthiness of the left, built into its own buffer so the arms can be
       captured before it is committed to `b`. */
    Buf tcond; memset(&tcond, 0, sizeof tcond);
    if (lt == TY_POLY)      buf_printf(&tcond, "sp_poly_truthy(_t%d)", t);
    else if (lt == TY_BOOL) buf_printf(&tcond, "_t%d", t);
    else if (lt_falsy_const) buf_puts(&tcond, "0");
    else if (lt == TY_INT)  buf_printf(&tcond, "(_t%d != SP_INT_NIL)", t);  /* a nullable int reads falsy at the sentinel; a plain int is always truthy */
    else if (lt == TY_FLOAT) buf_printf(&tcond, "(!sp_float_is_nil(_t%d))", t);
    else if (lt == TY_STRING || ty_is_array(lt) || ty_is_hash(lt) || ty_is_object(lt) ||
             lt == TY_PROC || lt == TY_MATCHDATA || lt == TY_EXCEPTION ||
             lt == TY_BIGINT || ty_nullable_builtin_id(lt))
      buf_printf(&tcond, "(_t%d != 0)", t);  /* nullable pointer: NULL reads falsy */
    else if (lt == TY_SYMBOL) buf_printf(&tcond, "(_t%d != (sp_sym)-1)", t);  /* nilable symbol sentinel */
    else if (lt == TY_UNKNOWN && res == TY_BOOL) buf_printf(&tcond, "_t%d", t);  /* temp holds sp_poly_truthy(left) (#3276) */
    else if (lt == TY_CLASS) buf_printf(&tcond, "(!sp_class_nil_p(_t%d))", t);
    else                    buf_puts(&tcond, "1");  /* concrete value: always truthy */
    /* Capture each arm (widened to res). The RIGHT arm's prelude is captured
       separately: an object subexpression there (`a && b.c`) hoists a GC-rooted
       temp, which must be evaluated INSIDE the short-circuit -- after the left,
       and only when the left is truthy -- not lifted above the whole chain
       (issue #1773). The kept-left arm is pure temp/box and never hoists. */
    Buf larm; memset(&larm, 0, sizeof larm);
    Buf rarm; memset(&rarm, 0, sizeof rarm);
    Buf rpre; memset(&rpre, 0, sizeof rpre);
    #define EMIT_ARM(IS_RIGHT, TB) do { \
      if (IS_RIGHT) { emit_ternary_arm(c, right, res, (TB)); } \
      else { if (res == TY_POLY && lt != TY_POLY) { /* box the temp by left type */ \
               /* In `a && b` the kept-left arm is reached only when the left is
                  FALSY, and for a scalar slot that means it holds the nil
                  sentinel: box it as nil, or the result answers nil? with
                  false and survives compact. `a || b` keeps a truthy left, so
                  there the plain box is right. */ \
               if (lt==TY_INT) { if (is_and) buf_puts((TB), "sp_box_nil()"); \
                                 else buf_printf((TB), "sp_box_int(_t%d)", t); } \
               else if (lt==TY_STRING) buf_printf((TB), "sp_box_nullable_str(_t%d)", t); \
               else if (lt==TY_FLOAT) { if (is_and) buf_puts((TB), "sp_box_nil()"); \
                                        else buf_printf((TB), "sp_box_float(_t%d)", t); } \
               else if (lt==TY_BOOL) buf_printf((TB), "sp_box_bool(_t%d)", t); \
               else if (lt==TY_SYMBOL) buf_printf((TB), "(_t%d != (sp_sym)-1 ? sp_box_sym(_t%d) : sp_box_nil())", t, t); \
               else if (ty_is_object(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %d)", t, ty_object_class(lt)); \
               else if (ty_is_hash(lt) && hash_box_cls(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, hash_box_cls(lt)); \
               else if (ty_is_array(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, \
                        lt==TY_INT_ARRAY ? "SP_BUILTIN_INT_ARRAY" : lt==TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : \
                        lt==TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY" : "SP_BUILTIN_POLY_ARRAY"); \
               else if (lt == TY_MATCHDATA && is_and) buf_puts((TB), "sp_box_nil()"); /* MatchData has no poly box; in `m && x` the kept-left arm is reached only when m is NULL, i.e. nil (#2896) */ \
               /* every remaining nullable builtin handle -- a Mutex, Queue,
                  Fiber, Thread, ConditionVariable -- boxes like the object and
                  container arms above. Without this the arm assigned a raw
                  sp_mutex * where the sibling arm is an sp_RbVal, and the C
                  compiler rejected the ternary outright (#3484). */ \
               else if (ty_nullable_builtin_id(lt)) buf_printf((TB), "sp_box_nullable_obj((void *)_t%d, %s)", t, ty_nullable_builtin_id(lt)); \
               else if (lt == TY_CLASS) buf_printf((TB), "sp_box_class(_t%d)", t); \
               else buf_printf((TB), "_t%d", t); } \
             else buf_printf((TB), "_t%d", t); } \
    } while (0)
    EMIT_ARM(0, &larm);
    { Buf *sv_pre2 = g_pre; g_pre = &rpre; EMIT_ARM(1, &rarm); g_pre = sv_pre2; }
    #undef EMIT_ARM
    int rhoists = rpre.p && rpre.p[0];
    if (!rhoists) {
      buf_printf(b, "%s ? %s : %s; })", tcond.p ? tcond.p : "1",
                 is_and ? (rarm.p ? rarm.p : "0") : (larm.p ? larm.p : "0"),
                 is_and ? (larm.p ? larm.p : "0") : (rarm.p ? rarm.p : "0"));
    }
    else {
      /* Right arm hoists: run it inside a real branch so its prelude is scoped
         and short-circuited. `res` may be void/nil (a writer arm) -- hold poly. */
      TyKind rres = (res == TY_VOID || res == TY_NIL) ? TY_POLY : res;
      int tr = ++g_tmp;
      emit_ctype(c, rres, b); buf_printf(b, " _t%d = {0}; if (%s) {\n", tr, tcond.p ? tcond.p : "1");
      if (is_and) buf_printf(b, "%s_t%d = %s;\n}\nelse { _t%d = %s; } _t%d; })",
                             rpre.p ? rpre.p : "", tr, rarm.p ? rarm.p : "0", tr, larm.p ? larm.p : "0", tr);
      else        buf_printf(b, "_t%d = %s;\n}\nelse { %s_t%d = %s; } _t%d; })",
                             tr, larm.p ? larm.p : "0", rpre.p ? rpre.p : "", tr, rarm.p ? rarm.p : "0", tr);
    }
    free(tcond.p); free(larm.p); free(rarm.p); free(rpre.p);
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
    buf_printf(b, " _t%d = %s; sp_exc_check_depth(); sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp; sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n", t, slot_zero(c, rt));
    buf_puts(b, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
    /* expression arm -- assign result to temp (skip diverging exprs like raise) */
    TyKind et = e >= 0 ? comp_ntype(c, e) : TY_UNKNOWN;
    /* An empty container's type reads UNKNOWN for want of an element type; it
       still produces a value, so it must be assigned rather than emitted for
       effect and discarded (#3495). */
    int e_diverges = (et == TY_UNKNOWN || et == TY_VOID) && !node_is_empty_container(nt, e);
    /* A call spinel folds into an unconditional raise carries whatever C type
       that raise helper returns, which need not match the merged slot (the
       constant-folded `nil.clone(freeze: false)` yields an int against an
       sp_Class slot). Control never reaches the assignment, so treat it as
       diverging and emit it for effect alone. (#3021) */
    if (!e_diverges && e >= 0 && rt != TY_POLY && et != rt &&
        !ty_is_numeric(rt) && nt_type(nt, e) && sp_streq(nt_type(nt, e), "CallNode"))
      e_diverges = 1;
    buf_puts(b, "  ");
    /* the expression arm's own preludes (an inline-spliced body, a rooted
       temp) must land INSIDE the protected region -- with the enclosing
       statement's g_pre they would run before the setjmp, unprotected
       (#2723). Same swap the rescue arm below has always done. */
    if (e >= 0 && !e_diverges) {
      Buf epre; memset(&epre, 0, sizeof epre);
      Buf *sv_pre0 = g_pre; int sv_ind0 = g_indent;
      g_pre = &epre; g_indent = 1;
      Buf ev; memset(&ev, 0, sizeof ev);
      if (rt == TY_POLY && et != TY_POLY) emit_boxed(c, e, &ev);
      else emit_expr_slot(c, e, rt, &ev);
      g_pre = sv_pre0; g_indent = sv_ind0;
      if (epre.p) buf_puts(b, epre.p);
      buf_printf(b, "_t%d = %s;", t, ev.p ? ev.p : "");
      free(epre.p); free(ev.p);
    }
    else if (e >= 0) {
      /* diverging expression like raise: emit as stmt (no assignment) */
      Buf epre; memset(&epre, 0, sizeof epre);
      Buf *sv_pre0 = g_pre; int sv_ind0 = g_indent;
      g_pre = &epre; g_indent = 1;
      Buf ev; memset(&ev, 0, sizeof ev);
      emit_expr(c, e, &ev);
      g_pre = sv_pre0; g_indent = sv_ind0;
      if (epre.p) buf_puts(b, epre.p);
      buf_printf(b, "%s;", ev.p ? ev.p : "");
      free(epre.p); free(ev.p);
    }
    /* restore the handled-exception depth too: a body that exits by raising
       out of its own rescue leaves its push behind, and `$!` would keep
       reading it long after the handler is gone (#3726) */
    buf_puts(b, " sp_exc_top--;\n}\nelse {\n  sp_exc_top--;\n  sp_gc_nroots = sp_exc_rootmark[sp_exc_top];\n  "
                "sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n  "
                "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n  "
                /* a bare rescue catches StandardError and its descendants only:
                   this arm caught everything, so a subclass of Exception was
                   swallowed (#3725) */
                "if (!sp_exc_is_standard_error((const char *)sp_last_exc_cls)) {\n    "
                "  sp_pending_exc_obj = sp_exc_obj[sp_exc_top]; sp_bt_keep = 1;\n    "
                "  sp_raise_cls((const char *)sp_last_exc_cls, sp_exc_msg[sp_exc_top]);\n  "
                "}\n  ");
    /* materialize the handled exception and push it so `$!` (and #cause
       threading for a nested raise) see it inside the fallback, exactly like
       a full rescue arm; popped when the arm value settles. */
    int tce = ++g_tmp;
    buf_printf(b, "sp_Exception *_t%d = sp_exc_obj[sp_exc_top] ? (sp_Exception *)sp_exc_obj[sp_exc_top]"
                  " : sp_exc_new_for_catch(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]);\n  ", tce);
    buf_printf(b, "sp_gc_wb((void *)_t%d); _t%d->cause = (sp_Exception *)sp_pending_cause; sp_pending_cause = NULL;\n  ", tce, tce);
    buf_printf(b, "sp_rescue_push((void *)_t%d);\n  ", tce);
    /* rescue arm: its preludes (e.g. a hoisted `$!` read) must land INSIDE
       this else block, after the push -- swap g_pre to a local buffer. */
    if (r >= 0) {
      Buf rpre; memset(&rpre, 0, sizeof rpre);
      Buf *sv_pre = g_pre; int sv_ind = g_indent;
      g_pre = &rpre; g_indent = 1;
      Buf rv; memset(&rv, 0, sizeof rv);
      if (rt == TY_POLY && comp_ntype(c, r) != TY_POLY) emit_boxed(c, r, &rv);
      else emit_expr_slot(c, r, rt, &rv);
      g_pre = sv_pre; g_indent = sv_ind;
      if (rpre.p) buf_puts(b, rpre.p);
      free(rpre.p);
      buf_printf(b, "_t%d = %s;", t, rv.p ? rv.p : "");
      free(rv.p);
    }
    buf_puts(b, "\n  sp_rescue_sp--;");
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
    TyKind srt = g_result_ty; g_result_ty = rt;
    /* the value sits in the temp while the ensure body runs, which may
       allocate; the root goes in front of the region so the landing's
       watermark restore keeps it. An empty ensure clause runs nothing between
       the write and the read, so it gets no root. */
    int ec = nt_ref(c->nt, id, "ensure_clause");
    int hold = ec >= 0 && nt_ref(c->nt, ec, "statements") >= 0 && ty_gc_rootable(c, rt);
    if (g_pre) {
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s;", t, slot_zero(c, rt));
      if (hold) { buf_puts(g_pre, " "); emit_gc_root_tmp(c, rt, t, g_pre); }
      buf_puts(g_pre, "\n");
      emit_begin(c, id, g_pre, g_indent, rv);
    }
    else {
      /* No prelude available (e.g. inside another expression's prelude):
         fall back to a GCC statement expression. */
      buf_puts(b, "({ ");
      emit_ctype(c, rt, b); buf_printf(b, " _t%d = %s;", t, slot_zero(c, rt));
      if (hold) { buf_puts(b, " "); emit_gc_root_tmp(c, rt, t, b); }
      buf_puts(b, "\n");
      emit_begin(c, id, b, 0, rv);
      buf_printf(b, "_t%d; })", t);
      g_result_poly = sp; g_result_ty = srt;
      return;
    }
    g_result_poly = sp; g_result_ty = srt;
    buf_printf(b, "_t%d", t);
    return;
  }

  /* MultiWriteNode as expression: execute the destructuring (side effect),
     then return the RHS value (Ruby semantics: value of `a, b = arr` is arr). */
  if (sp_streq(ty, "MultiWriteNode")) {
    int value = nt_ref(nt, id, "value");
    /* The right-hand side runs once: a tuple's elements one by one, or any
       other right-hand side whole, are held in temps that the statement and
       the result both read through the override table. Literals are read
       again in place. */
    int en = 0;
    const int *els = nt_kind(nt, value) == NK_ArrayNode ? nt_arr(nt, value, "elements", &en) : NULL;
    for (int i = 0; i < en; i++)
      if (nt_kind(nt, els[i]) == NK_SplatNode) { els = NULL; en = 0; break; }
    int one = els ? -1 : value;
    int held = 0;
    /* a target's receiver and index that run code go first, as Ruby
       evaluates them before the values */
    int ln = 0;
    const int *lefts = nt_arr(nt, id, "lefts", &ln);
    for (int i = 0; i < ln; i++) {
      NodeKind tk = nt_kind(nt, lefts[i]);
      if (tk != NK_IndexTargetNode && tk != NK_CallTargetNode) continue;
      int args = tk == NK_IndexTargetNode ? nt_ref(nt, lefts[i], "arguments") : -1, an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      int parts[2] = { nt_ref(nt, lefts[i], "receiver"), an == 1 ? av[0] : -1 };
      for (int p = 0; p < 2; p++) {
        int v = parts[p];
        if (v < 0 || g_n_argov >= MAX_ARG_OVERRIDE || !subtree_has_side_effect(c, v)) continue;
        TyKind vt = comp_ntype(c, v);
        if (vt == TY_UNKNOWN || vt == TY_NIL || vt == TY_VOID ||
            ty_is_struct_valued(vt) || comp_ty_value_obj(c, vt)) continue;
        Buf vb; memset(&vb, 0, sizeof vb);
        emit_expr(c, v, &vb);
        int t = ++g_tmp;
        emit_indent(g_pre, g_indent);
        emit_ctype(c, vt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", t, vb.p ? vb.p : "");
        emit_gc_root_tmp(c, vt, t, g_pre);
        buf_puts(g_pre, "\n");
        free(vb.p);
        view_bind(v, "_t%d", t);
        held++;
      }
    }
    for (int i = els ? 0 : -1; i < en; i++) {
      int v = i < 0 ? one : els[i];
      if (v < 0 || g_n_argov >= MAX_ARG_OVERRIDE) continue;
      NodeKind vk = nt_kind(nt, v);
      int vn = 0;
      if (vk == NK_NilNode || vk == NK_IntegerNode || vk == NK_FloatNode || vk == NK_SymbolNode ||
          vk == NK_TrueNode || vk == NK_FalseNode || vk == NK_StringNode ||
          ((vk == NK_ArrayNode || vk == NK_HashNode) && (nt_arr(nt, v, "elements", &vn), vn == 0)))
        continue;
      TyKind vt = vk == NK_SplatNode ? TY_POLY_ARRAY : comp_ntype(c, v);
      if (vt == TY_UNKNOWN) continue;
      Buf vb; memset(&vb, 0, sizeof vb);
      emit_expr(c, v, &vb);
      emit_indent(g_pre, g_indent);
      if (vt == TY_NIL || vt == TY_VOID) {
        buf_printf(g_pre, "(void)(%s);\n", vb.p ? vb.p : "0");
        view_bind(v, "sp_box_nil()");
      }
      else {
        int t = ++g_tmp;
        emit_ctype(c, vt, g_pre);
        buf_printf(g_pre, " _t%d = %s; ", t, vb.p ? vb.p : "");
        emit_gc_root_tmp(c, vt, t, g_pre);
        buf_puts(g_pre, "\n");
        view_bind(v, "_t%d", t);
      }
      free(vb.p);
      held++;
    }
    emit_stmt(c, id, g_pre, g_indent);
    emit_expr(c, value, b);
    view_unbind(g_n_argov - (held));
    return;
  }

  /* ivar OP= as expression: emit the mutation then read back the ivar. */
  if (sp_streq(ty, "InstanceVariableOperatorWriteNode")) {
    const char *nm = nt_str(nt, id, "name");
    int sc = comp_scope_of(c, id)->class_id;
    char ref[300];
    Scope *cs = comp_scope_of(c, id);
    /* the slot is read back as the read of the ivar finds it: a top-level
       ivar is the Toplevel pseudo-class's file-scope global, where `self`
       names nothing (`x = (@i += 1)` at the top level, or a min/max block) */
    int tl = cs && cs->class_id < 0 && g_ie_class_id < 0 ? comp_class_index(c, "Toplevel") : -1;
    if (cs && cs->is_cmethod && cs->class_id >= 0)
      snprintf(ref, sizeof ref, "civ_%s_%s", c->classes[cs->class_id].name, iv_c(nm + 1));
    else if (tl >= 0) {
      snprintf(ref, sizeof ref, "civ_Toplevel_%s", iv_c(nm + 1));
      sc = tl;
    }
    else
      snprintf(ref, sizeof ref, "%s%siv_%s", g_self, g_self_deref, iv_c(nm + 1));
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
      /* a Float slot's operator converts its operand as Float's does, as
         the statement form (emit_scalar_op_assign) takes it */
      emit_coerce(c, nt_ref(nt, id, "value"), vt, vt == TY_FLOAT ? CO_CONVERT : CO_HOLD,
                  "the operand of an `op=`", b);
      buf_printf(b, "; _t%d = %s; _t%d; })", t, ref, t);
    }
    return;
  }

  if (sp_streq(ty, "InterpolatedRegularExpressionNode")) {
    /* a dynamic regexp in general value position: the shared pattern emitter
       builds the interpolated source and re_compiles into a prelude temp. */
    if (emit_regex_pat_to_buf(c, id, b)) return;
  }
  if (sp_streq(ty, "DefNode")) {
    /* a def in value position: the definition emits separately; the
       expression's value is the method-name symbol. */
    const char *dn = nt_str(nt, id, "name");
    buf_printf(b, "(sp_sym)%d", comp_sym_intern(c, dn ? dn : ""));
    return;
  }
  if (sp_streq(ty, "CallOrWriteNode") || sp_streq(ty, "CallAndWriteNode")) {
    /* value position `x = (a.v ||= 5)`: run the conditional write, then read
       the attribute back as the expression's value (the assigned-or-existing
       one, matching CRuby). The guard is the ivar's own (emit_slot_orw_value). */
    int is_or = sp_streq(ty, "CallOrWriteNode");
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    int v = nt_ref(nt, id, "value");
    if (emit_call_or_write_via_methods(c, id, is_or, b)) return;
    TyKind rt = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
    if (recv >= 0 && attr && ty_is_object(rt)) {
      int class_id = ty_object_class(rt);
      char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", attr);
      int iidx = comp_ivar_index(&c->classes[class_id], ivn);
      TyKind ivt = iidx >= 0 ? c->classes[class_id].ivar_types[iidx] : TY_UNKNOWN;
      int tr = ++g_tmp;
      buf_puts(b, "({ ");
      emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
      char lhs[300]; snprintf(lhs, sizeof lhs, "_t%d->iv_%s", tr, iv_c(attr));
      emit_slot_orw_value(c, ivt, lhs, v, is_or, b);
      buf_puts(b, "; })");
      return;
    }
  }
  if (sp_streq(ty, "SplatNode")) {
    /* `*x` in unboxed value position (a next/break value flowing into a
       typed PolyArray slot): splat-to-array, then unwrap the pointer. */
    int sp_inner = nt_ref(nt, id, "expression");
    /* splat semantics first (nil -> [], scalar -> [v], array kept), then
       normalize the KIND: the slot is sp_PolyArray*, and a kept IntArray
       must be rebuilt, not pointer-cast (different layout). */
    buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array(");
    if (sp_inner >= 0) emit_boxed(c, sp_inner, b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, "))");
    return;
  }

  /* a[i] op= v as an expression (e.g. the tail of a block whose proc result
     is consumed): emit the mutation via the statement form, then read the
     slot back as the value -- Ruby yields the stored (post-op) value. The
     read-back re-evaluates receiver and key, so this path is gated on both
     being effect-free simple reads; anything else keeps the clean reject. */
  if (sp_streq(ty, "IndexOperatorWriteNode")) {
    int ir = nt_ref(nt, id, "receiver");
    int ia = nt_ref(nt, id, "arguments");
    int iac = 0; const int *iav = ia >= 0 ? nt_arr(nt, ia, "arguments", &iac) : NULL;
    if (ir >= 0 && iac == 1) {
      /* A variable or a literal can simply be re-emitted for the read-back; a
         method-call receiver or key cannot, because CRuby evaluates each once
         and re-emitting would run it twice. Hoist those into temps that both
         the write and the read then share. Without this the value form
         declined outright, which is what `v[s.tag] += 1` as a block's last
         expression hit (#3417). */
      int cheap = idx_opw_node_is_cheap(nt, ir) && idx_opw_node_is_cheap(nt, iav[0]);
      if (g_pre) {
        int hoisted = cheap ? 0 : emit_index_opw_hoist(c, id, g_pre, g_indent);
        if (cheap || hoisted) {
          /* The mutation cannot be emitted straight into g_pre: an RHS that
             wants a prelude of its own (a user method call hoists its receiver
             and spills each argument into a temp) drains into g_pre too, and
             appending to the buffer mid-statement dropped the declaration
             INSIDE the sp_..._set(...) argument list -- structurally invalid C,
             an unbalanced paren and a _tN used before its declaration. Collect
             the RHS's prelude and the statement separately, then splice them in
             prelude-first, as the no-g_pre branch below already does. */
          Buf *sv_pre = g_pre;
          Buf pre; memset(&pre, 0, sizeof pre);
          Buf stmt; memset(&stmt, 0, sizeof stmt);
          g_pre = &pre;
          emit_index_op_write(c, id, &stmt, g_indent);
          g_pre = sv_pre;
          if (pre.p) buf_puts(sv_pre, pre.p);
          if (stmt.p) buf_puts(sv_pre, stmt.p);
          free(pre.p); free(stmt.p);
          /* the read-back runs after the mutation, so its own prelude (if any)
             belongs at the end of g_pre -- where it now lands naturally. */
          emit_index_get(c, ir, iav[0], b);
          if (hoisted) emit_index_opw_unhoist();
          return;
        }
      }
      else {
        /* No prelude buffer: statement-expression fallback. The RHS inside
           the mutation may itself need a prelude, so redirect g_pre into a
           local buffer and splice it in front, mirroring emit_with_prelude
           (writing into a NULL g_pre would crash the compiler). */
        Buf pre; memset(&pre, 0, sizeof pre);
        Buf stmt; memset(&stmt, 0, sizeof stmt);
        int sv_ind = g_indent;
        g_pre = &pre; g_indent = 0;
        int hoisted = cheap ? 0 : emit_index_opw_hoist(c, id, &pre, 0);
        if (cheap || hoisted) {
          emit_index_op_write(c, id, &stmt, 0);
          g_pre = NULL; g_indent = sv_ind;
          buf_puts(b, "({ ");
          if (pre.p) buf_puts(b, pre.p);
          if (stmt.p) buf_puts(b, stmt.p);
          emit_index_get(c, ir, iav[0], b);
          buf_puts(b, "; })");
          free(pre.p); free(stmt.p);
          if (hoisted) emit_index_opw_unhoist();
          return;
        }
        g_pre = NULL; g_indent = sv_ind;
        free(pre.p); free(stmt.p);
      }
    }
  }

  /* retry in value position (e.g. the tail of a parenthesized group): the
     jump back to its begin makes the value dead -- emit the goto as a
     prelude statement, exactly like the statement form. */
  if (sp_streq(ty, "RetryNode")) {
    if (g_retry_label) {
      /* inline, not a g_pre prelude: statements sequenced before the retry in
         the same group (e.g. `(c = false; retry)`) must run first. GCC/clang
         permit a goto out of a statement expression; the dead 0 satisfies the
         value slot. */
      buf_printf(b, "({ %sgoto %s; 0; })",
                 g_rescue_save_depth > 0 ? "sp_rescue_sp--; " : "", g_retry_label);
    }
    else unsupported(c, id, "retry (outside rescue)");
    return;
  }
  /* `next v` in value position -- the tail of a proc/block body, as in
     `proc { next 5 }`. Leaving the block with a value IS the body's value
     there, so the expression is just v (a bare `next` yields nil). The
     early-exit forms are handled by the statement emitter (#3026). */
  if (sp_streq(ty, "NextNode")) {
    int nargs = nt_ref(nt, id, "arguments");
    int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
    if (nvc == 1) { emit_expr(c, nv[0], b); return; }
    if (nvc == 0) { buf_puts(b, "sp_box_nil()"); return; }
  }

  unsupported(c, id, "expression");
}

/* ---- output statements (puts/print/p) ---- */
