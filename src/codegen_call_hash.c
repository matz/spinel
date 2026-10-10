/* codegen_call_hash.c -- the builtin-op emitters of the Hash receiver arms
   that are more than one C expression around the receiver and its
   operands: they branch on the receiver's variant or an operand's kind, or
   build a statement expression of their own. The rest are templates in
   builtin_ops.c. Each answers 1 when it emitted the call, 0 (having emitted
   nothing and taken no temp) to leave it to the arms after the lookup in
   emit_hash_call. */

#include "codegen_internal.h"
#include "builtin_ops.h"
#include "codegen_call_arms.h"
#include "repr.h"

/* ---- nil out of band: an emitter whose C result is an sp_oint / sp_ofloat
   leaves it bare when the node is one (node_is_oint: the dispatcher's
   consumer takes the oint) and otherwise reads it as the plain scalar through
   sp_oint_arg (TypeError for nil) -- the same wrap the dispatcher applies to an
   oint producer, applied here because this emitter decides the form. */
static void oint_open(Compiler *c, int id, TyKind t, Buf *b) { if (!node_is_oint(c, id)) buf_printf(b, "%s(", oint_arg(t)); }
static void oint_close(Compiler *c, int id, Buf *b) { if (!node_is_oint(c, id)) buf_puts(b, ")"); }

/* any?(pattern) / none? / one? / count with one argument and no block:
   compare each [key, value] pair by == (sp_poly_eq covers array-vs-array
   value equality, which is what a pair pattern is) */
int emit_op_hash_pattern(Compiler *c, const BopCtx *x, Buf *b) {
  const char *name = x->name;
  int recv = x->recv;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int th = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, tc2 = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
  buf_puts(b, "({ "); th = hold_operand(c, recv, TY_POLY, 1, th, 1, " ", b);
  buf_printf(b, "sp_RbVal _t%d = ", tv);
  emit_boxed(c, argv[0], b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_int _t%d = sp_poly_length(_t%d); sp_int _t%d = 0;",
             tv, tn, th, tc2);
  /* a CLASS pattern is a kind-of test, not equality: `h.any?(Array)`
     compared each pair to the class value and answered false (#3565).
     #count is the exception: it counts elements EQUAL to its argument
     (Enumerable#count uses ==, the predicates use ===), so a class
     argument counts the class itself, not its instances (#3817). */
  if (comp_ntype(c, argv[0]) == TY_CLASS && !sp_streq(name, "count"))
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                  " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);"
                  " if (sp_poly_is_a(_t%d, (sp_Class){(sp_int)_t%d.v.i, NULL})) _t%d++; }",
               ti, ti, tn, ti, tp, th, ti, tp, tv, tc2);
  else
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                  " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);"
                  " if (sp_poly_rb_equal(_t%d, _t%d)) _t%d++; }",
               ti, ti, tn, ti, tp, th, ti, tp, tv, tc2);
  if (sp_streq(name, "any?"))       buf_printf(b, " _t%d > 0; })", tc2);
  else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc2);
  else if (sp_streq(name, "one?"))  buf_printf(b, " _t%d == 1; })", tc2);
  else                              buf_printf(b, " _t%d; })", tc2);
  return 1;
}

/* all?(pattern) with no block: test each [key, value] pair with
   `pattern === pair`. An Array pattern (the common destructured-pair form)
   compares by ==, served by sp_poly_eq; a CLASS pattern is a kind-of test,
   and comparing the pair to the class value by equality answered false for
   every pair (#3565). any?/none?/one? with a pattern take
   emit_op_hash_pattern. */
int emit_op_hash_pattern_all(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int tp = ++g_tmp, tpat = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  buf_printf(b, "; sp_RbVal _t%d = ", tpat); emit_boxed(c, argv[0], b);
  buf_printf(b, "; sp_int _t%d = 0;", tc);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tp, ti);
  if (comp_ntype(c, argv[0]) == TY_CLASS)
    buf_printf(b, " if (sp_poly_is_a(_t%d->data[_t%d], (sp_Class){(sp_int)_t%d.v.i, NULL})) _t%d++;", tp, ti, tpat, tc);
  else
    buf_printf(b, " if (sp_poly_rb_equal(_t%d->data[_t%d], _t%d)) _t%d++;", tp, ti, tpat, tc);
  buf_printf(b, " _t%d == _t%d->len; })", tc, tp);
  return 1;
}

/* Hash#default_proc: wrap the stored Hash.new{} dproc (a raw C fn +
   captures pointer) in a first-class Proc via a per-variant trampoline
   that adapts the sp_proc_call ABI (boxed side-channel args) back to the
   dproc signature. A hash without a dproc -- or a variant that cannot
   carry one -- yields NULL (nil). */
int emit_op_hash_default_proc(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hnn = ty_hash_cname(rt);
  int hdp_v = !hnn ? -1
            : sp_streq(hnn, "SymPoly") ? 0
            : sp_streq(hnn, "StrPoly") ? 1
            : sp_streq(hnn, "PolyPoly") ? 2 : -1;
  if (hdp_v < 0) {
    buf_puts(b, "((void)(");
    emit_expr(c, recv, b);
    buf_puts(b, "), (sp_Proc *)NULL)");
    return 1;
  }
  static char hdp_done[3];
  if (!hdp_done[hdp_v]) {
    hdp_done[hdp_v] = 1;
    if (!g_needs_proc_poly_argslot) {
      g_needs_proc_poly_argslot = 1;
      buf_puts(&g_proc_protos, "extern SP_TLS sp_RbVal _sp_proc_poly_args[SP_PROC_ARG_SLOTS];\n");
    }
    const char *kexpr = hdp_v == 0 ? "(sp_sym)sp_poly_to_i(_sp_proc_poly_args[1])"
                      : hdp_v == 1 ? "_sp_proc_poly_args[1].v.s"
                      : "_sp_proc_poly_args[1]";
    buf_printf(&g_procs,
      "static sp_int _hdp_tramp_%s(void *cap, sp_int argc, sp_int *args) {\n"
      "  sp_%sHash *src = (sp_%sHash *)cap; (void)args;\n"
      "  sp_%sHash *h = (argc >= 1 && _sp_proc_poly_args[0].tag == SP_TAG_OBJ)"
      " ? (sp_%sHash *)_sp_proc_poly_args[0].v.p : src;\n"
      "  _sp_proc_poly_ret = (src && src->dproc && argc >= 2)"
      " ? src->dproc(h, %s, src->dproc_self) : sp_box_nil();\n"
      "  return 0;\n}\n"
      "static sp_Proc *_hdp_%s(sp_%sHash *h) {\n"
      "  if (!h || !h->dproc) return NULL;\n"
      "  return sp_proc_new_meta((void *)_hdp_tramp_%s, h, sp_bm_cap_scan, 2, FALSE, 0, NULL, NULL);\n}\n",
      hnn, hnn, hnn, hnn, hnn, kexpr, hnn, hnn, hnn);
  }
  buf_printf(b, "_hdp_%s(", hnn);
  emit_expr(c, recv, b);
  buf_puts(b, ")");
  return 1;
}

/* Hash#to_proc: a Proc mapping a key to the hash value, closing over the
   hash. Emit a per-variant lookup fn matching the sp_proc_call ABI. */
int emit_op_hash_to_proc(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  int pn = ++g_proc_counter;
  /* The key is read from the proc's boxed side-channel, which every caller
     publishes, not from the sp_int slot: the proc can be called with a key
     of any class, and the slot's bits read as a `const char *` for a
     String-keyed Hash called with an Integer crashed. A key of a class the
     storage cannot hold (an Integer or a Symbol on a String-keyed Hash, a
     Float 1.0 on an Integer-keyed one, which is not eql? to 1) is a miss
     answering the Hash's default, as `h[key]` answers it. */
  const char *keyexpr = (kt == TY_SYMBOL) ? "(sp_sym)_k.v.i"
                      : (kt == TY_STRING) ? "_k.v.s"
                      : (kt == TY_INT) ? "_k.v.i"
                      : "_k";
  const char *keytag = (kt == TY_SYMBOL) ? "SP_TAG_SYM"
                     : (kt == TY_STRING) ? "SP_TAG_STR"
                     : (kt == TY_INT) ? "SP_TAG_INT" : NULL;
  g_needs_proc_poly_argslot = 1;
  buf_printf(&g_proc_protos, "static sp_int _hashproc_%d(void *cap, sp_int argc, sp_int *args);\n", pn);
  buf_printf(&g_procs, "static sp_int _hashproc_%d(void *cap, sp_int argc, sp_int *args) {\n", pn);
  /* the hash proc is a lambda: exactly one key, as CRuby's raises --
     the old `argc < 1 -> return 0` left the return slot holding the
     previous call's value */
  buf_printf(&g_procs, "  if (argc != 1) sp_raise_cls(\"ArgumentError\","
             " sp_sprintf(\"wrong number of arguments (given %%lld, expected 1)\", (long long)argc));\n");
  buf_printf(&g_procs, "  sp_%sHash *_h = (sp_%sHash *)cap; (void)args;\n", hn, hn);
  buf_puts(&g_procs, "  sp_RbVal _k = _sp_proc_poly_args[0];\n");
  /* a mutable String key looks its contents up */
  if (kt == TY_STRING)
    buf_puts(&g_procs, "  if (sp_poly_is_strbuf(_k)) _k = sp_poly_strbuf_deref(_k);\n");
  if (keytag) {
    buf_printf(&g_procs, "  if (_k.tag != %s) { _sp_proc_poly_ret = sp_poly_hash_foreign_miss(", keytag);
    emit_boxed_text(c, rt, "_h", &g_procs);
    buf_puts(&g_procs, ", _k); return 0; }\n");
  }
  /* Universal return ABI: publish the boxed value into _sp_proc_poly_ret
     for every value type; the .call site reads the slot back. */
  buf_puts(&g_procs, "  _sp_proc_poly_ret = ");
  { char _ge[256];
    /* an Integer-valued hash answers a miss as its default or nil (oget) */
    if (vt == TY_INT) buf_printf(&g_procs, "sp_box_oint(sp_%sHash_oget(_h, %s))", hn, keyexpr);
    else {
      snprintf(_ge, sizeof _ge, "sp_%sHash_get(_h, %s)", hn, keyexpr);
      emit_boxed_text(c, vt, _ge, &g_procs);
    } }
  buf_puts(&g_procs, ";\n  return 0;\n}\n");
  buf_printf(b, "sp_proc_new_meta((void *)_hashproc_%d, (void *)(", pn);
  emit_expr(c, recv, b);
  /* CRuby's Hash#to_proc is a lambda: lambda? answers true and a
     composed call enforces its 1-arity instead of reading a stale slot */
  buf_puts(b, "), sp_hashproc_cap_scan, 1, TRUE, 1, NULL, NULL)");
  return 1;
}

/* h[key]: the variant's getter, or the hash's default for a key of a kind
   the table cannot hold */
int emit_op_hash_aref(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind arg_kt = comp_ntype(c, argv[0]);
  TyKind hash_kt = ty_hash_key(rt);
  /* key type mismatch: sym key on str-keyed hash (or vice versa) -- the key
     can never exist in the hash, so always return the hash's default
     value. A Symbol on a String-keyed hash was excepted here and
     coerced to its name; that was an older Hash.new{} model (#4531). */
  if (hash_kt != TY_POLY && hash_kt != TY_UNKNOWN &&
      arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt &&
      !(hash_kt == TY_STRING && arg_kt == TY_STRBUF) &&
      !hash_nil_key_stored(c, argv[0], hash_kt)) {
    TyKind vt = ty_hash_val(rt);
    int t = ++g_tmp;
    buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b); buf_puts(b, "; ");
    buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");  /* the key still evaluates */
    if (vt == TY_INT) { oint_open(c, x->id, TY_INT, b); buf_printf(b, "({ %s _d%d = _t%d; (_d%d && !_d%d->default_nil) ? sp_oint_of(_d%d->default_v) : sp_oint_nil(); }); })", c_type_name(rt), t, t, t, t, t); oint_close(c, x->id, b); }
    /* absent means the hash's default, which is nil unless one was
       given -- not the empty string (#3790) */
    else if (vt == TY_STRING) buf_printf(b, "_t%d ? _t%d->default_v : NULL; })", t, t);
    else buf_printf(b, "_t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
    return 1;
  }
  if (rt == TY_POLY_POLY_HASH) {
    buf_printf(b, "sp_%sHash_get(", hn);
    emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
  }
  else {
    /* an int-valued hash answers a miss as nil beside the value (oget);
       a string-valued one as NULL (get) */
    int oint = ty_hash_val(rt) == TY_INT;
    if (oint) oint_open(c, x->id, TY_INT, b);
    buf_printf(b, "sp_%sHash_%s%s(", hn, oint ? "oget" : "get", hash_okey_sfx(c, argv[0], ty_hash_key(rt)));
    emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key_o(c, argv[0], ty_hash_key(rt), b); buf_puts(b, ")");
    if (oint) oint_close(c, x->id, b);
  }
  return 1;
}

/* has_key? / key? / include? / member? */
int emit_op_hash_has_key(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind hash_kt = ty_hash_key(rt);
  if (hash_key_misses(c, argv[0], hash_kt) && !hash_nil_key_stored(c, argv[0], hash_kt)) {
    /* a key of a class the table cannot hold: false, the receiver and
       the key still evaluated */
    buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
    buf_puts(b, "); 0; })");
    return 1;
  }
  buf_printf(b, "sp_%sHash_has_key(", hn);
  emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], hash_kt, b); buf_puts(b, ")");
  return 1;
}

/* Hash#key(value): the first key mapping to value. A Symbol-keyed hash
   has a runtime helper; any other variant scans the boxed [key, value]
   pair list, answering the key or nil. */
int emit_op_hash_key(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (rt == TY_SYM_POLY_HASH) {
    buf_puts(b, "sp_SymPolyHash_key(");
    emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  int tp = ++g_tmp, tv = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  buf_printf(b, "; sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b);
  buf_printf(b, "; sp_RbVal _t%d = sp_box_nil();", tr);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, tp, ti);
  buf_printf(b, " sp_PolyArray *_pr = (sp_PolyArray *)_t%d->data[_t%d].v.p;", tp, ti);
  buf_printf(b, " if (sp_poly_rb_equal(_pr->data[1], _t%d)) { _t%d = _pr->data[0]; break; } }", tv, tr);
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

/* Hash#default and #default(key) */
int emit_op_hash_default(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int t = ++g_tmp;
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
  if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
    /* default(key): a hash built with a block calls its default_proc with
       (self, key); default() (or a hash with no proc) returns default_v
       (#2464). Only the poly-value variants carry a dproc. */
    /* The proc takes the key in the hash's own key representation, so an
       argument of another type cannot be handed to it -- passing an
       Integer where a `const char *` key is expected did not even
       typecheck. Such a key can never be in this hash, so answer the
       plain default. */
    TyKind dkt = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    int dkey_ok = rt == TY_POLY_POLY_HASH ||
                  (rt == TY_SYM_POLY_HASH && dkt == TY_SYMBOL) ||
                  (rt == TY_STR_POLY_HASH && (dkt == TY_STRING || dkt == TY_STRBUF));
    if (argc == 1 && dkey_ok) {
      buf_printf(b, "; (_t%d && _t%d->dproc) ? _t%d->dproc(_t%d, ", t, t, t, t);
      if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b);
      else emit_expr(c, argv[0], b);
      buf_printf(b, ", _t%d->dproc_self) : (_t%d ? _t%d->default_v : sp_box_nil()); })", t, t, t);
    }
    else if (argc == 1) {
      buf_printf(b, "; (void)("); emit_expr(c, argv[0], b);
      buf_printf(b, "); _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
    }
    else {
      buf_printf(b, "; _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
    }
  }
  else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
    buf_printf(b, "; (_t%d && !_t%d->default_nil) ? sp_box_int(_t%d->default_v) : sp_box_nil(); })", t, t, t);
  }
  else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
    buf_printf(b, "; (_t%d && _t%d->default_v) ? sp_box_str(_t%d->default_v) : sp_box_nil(); })", t, t, t);
  }
  else {
    buf_printf(b, "; (void)_t%d; sp_box_nil(); })", t);
  }
  return 1;
}

/* Hash#keys. A Symbol-keyed hash's runtime returns the sym ids as an
   IntArray, boxed here into a poly (Symbol) array. */
int emit_op_hash_keys(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  if (rt == TY_SYM_POLY_HASH) {
    /* runtime returns sym ids as an IntArray; box into a poly (sym) array */
    int ki = ++g_tmp, kp = ++g_tmp, ii = ++g_tmp;
    buf_printf(b, "({ sp_IntArray *_t%d = sp_SymPolyHash_keys(", ki); emit_expr(c, recv, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ki, kp, kp);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                  " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)sp_IntArray_get(_t%d, _t%d)));",
               ii, ii, ki, ii, kp, ki, ii);
    buf_printf(b, " _t%d; })", kp);
    return 1;
  }
  buf_printf(b, "sp_%sHash_keys(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
  return 1;
}

/* fetch(key) with no default and no block raises KeyError on a miss */
int emit_op_hash_fetch(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind vt = ty_hash_val(rt);
  /* a hash whose values may be nil (DESIGN.md D3b-ii) answers the hit with
     its nil, as an oint */
  int vo = oint_kind(vt) && node_is_oint(c, x->id);
  const char *miss_v = vo ? "sp_oint_nil()" : vt == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, vt);
  int th = ++g_tmp, tk = ++g_tmp;
  char keytmp[32], htmp[32];
  snprintf(keytmp, sizeof keytmp, "_t%d", tk);
  snprintf(htmp, sizeof htmp, "_t%d", th);
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d)", th);   /* rooted across the key, as the array arms are */
  if (hash_key_misses(c, argv[0], ty_hash_key(rt)) && !hash_nil_key_stored(c, argv[0], ty_hash_key(rt))) {
    /* a key of a kind the table cannot hold: the KeyError names the
       key itself, so box it once rather than look it up */
    buf_printf(b, "; sp_RbVal _t%d = ", tk); emit_boxed(c, argv[0], b);
    buf_puts(b, "; sp_exc_stage_recv(");
    emit_boxed_text(c, rt, htmp, b);
    buf_printf(b, "); sp_raise_key_not_found(_t%d); %s; })", tk, miss_v);
    return 1;
  }
  buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
  buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_%s(_t%d, _t%d) : (",
             hn, th, tk, hn, vo ? "vget" : "get", th, tk);
  buf_puts(b, "sp_exc_stage_recv(");
  emit_boxed_text(c, rt, htmp, b);
  buf_puts(b, "), sp_raise_key_not_found(");
  emit_boxed_text(c, ty_hash_key(rt), keytmp, b);
  buf_printf(b, "), %s); })", miss_v);
  return 1;
}

/* Hash#to_s: the inspect text, or "" for a receiver that may be nil */
int emit_op_hash_to_s(Compiler *c, const BopCtx *x, Buf *b) {
  char fn[64]; snprintf(fn, sizeof fn, "sp_%sHash_inspect", ty_hash_cname(x->rt));
  emit_null_guarded_call(c, x->recv, x->rt, fn, "sp_str_empty", b);
  return 1;
}

/* compact!: drop nil-valued pairs in place; self when changed, nil when a
   no-op. Only the poly-valued variants can hold nil; the others are left
   to the arms after the lookup. */
int emit_op_hash_compact_bang(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  if (!(rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH)) return 0;
  const char *hnc = ty_hash_cname(rt);
  /* PolyPoly's order[] holds slot indexes, not keys; the other variants
     store the key itself in order[] (#2430) */
  int ppk = rt == TY_POLY_POLY_HASH;
  int th = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tc2 = ++g_tmp;
  buf_printf(b, "({ sp_%sHash *_t%d = ", hnc, th); emit_expr(c, recv, b);
  buf_printf(b, "; if (_t%d && sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);",
             th, th, th, hash_box_cls(rt));
  buf_printf(b, " sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);"
                " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {",
             hnc, tf, hnc, tf, ti, ti, th, ti);
  if (ppk)
    buf_printf(b, " sp_RbVal _k9 = _t%d->keys[_t%d->order[_t%d]];"
                  " sp_RbVal _t%d = sp_%sHash_get(_t%d, _k9);"
                  " if (!sp_poly_nil_p(_t%d)) sp_%sHash_set(_t%d, _k9, _t%d); }",
               th, th, ti, tv, hnc, th, tv, hnc, tf, tv);
  else
    buf_printf(b, " sp_RbVal _t%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);"
                  " if (!sp_poly_nil_p(_t%d)) sp_%sHash_set(_t%d, _t%d->order[_t%d], _t%d); }",
               tv, hnc, th, th, ti, tv, hnc, tf, th, ti, tv);
  buf_printf(b, " int _t%d = _t%d->len != _t%d->len;"
                " if (_t%d) sp_%sHash_replace(_t%d, _t%d);"
                " _t%d ? sp_box_obj(_t%d, %s) : sp_box_nil(); })",
             tc2, tf, th,
             tc2, hnc, th, tf,
             tc2, th, hash_box_cls(rt));
  return 1;
}

/* rehash. A key changed since it was stored is under the hash it was
   stored with; the general hash keeps each key's hash, so rehash asks
   every key again. A typed table holds Integer, Symbol or String keys,
   which CRuby's rehash leaves where they are, so there is nothing to
   rebuild: it answers the receiver, and a frozen one raises, as CRuby's
   does. */
int emit_op_hash_rehash(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  if (rt == TY_POLY_POLY_HASH) {
    buf_puts(b, "sp_PolyPolyHash_rehash("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  int t = ++g_tmp;
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
  buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s); _t%d; })", t, t, hash_box_cls(rt), t);
  return 1;
}

/* replace(other). An argument of the receiver's own variant is copied in
   place. Replace with a DIFFERENT hash variant: the receiver slot has
   widened to the universal PolyPoly hash (see infer), so it takes the boxed
   other's entries (sp_poly_hash_replace) -- never the raw-pointer mispatch
   that used to hang inspect (#2374). Either way the other's default value
   and default proc come with its entries, as in CRuby
   (`Hash.new(1).replace(b: 2).default` is nil); they stayed the receiver's.
   A lowered bang transform keeps the receiver's defaults instead.
   Any other argument is left to the arms after the lookup. */
int emit_op_hash_replace(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int keep_default = nt_str(c->nt, x->id, "bang_splice") != NULL;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  /* an empty `{}` has no variant of its own: it is read as the receiver's */
  Buf eb; memset(&eb, 0, sizeof eb);
  int same = repr_hash_is(repr_of(c, argv[0]), ty_hash_key(rt), ty_hash_val(rt));
  int empty = !same && emit_empty_literal_as(c, argv[0], rt, &eb);
  if (same || empty) {
    int trp = ++g_tmp, to = ++g_tmp;
    buf_printf(b, "({ %s _t%d = ", c_type_name(rt), trp); emit_expr(c, recv, b);
    buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", trp, trp, hash_box_cls(rt));   /* (#3001) */
    buf_printf(b, " SP_GC_ROOT(_t%d); %s _t%d = ", trp, c_type_name(rt), to);
    if (empty) buf_puts(b, eb.p); else emit_expr(c, argv[0], b);
    free(eb.p);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash_replace(_t%d, _t%d);", to, hn, trp, to);
    if (!keep_default) {
      buf_printf(b, " if (_t%d && _t%d) { sp_gc_wb((void *)_t%d); _t%d->default_v = _t%d->default_v;",
                 trp, to, trp, trp, to);
      /* an Integer-valued hash's nil default is its flag beside the value */
      if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH)
        buf_printf(b, " _t%d->default_nil = _t%d->default_nil;", trp, to);
      if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH)   /* the dproc variants */
        buf_printf(b, " _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", trp, to, trp, to);
      buf_puts(b, " }");
    }
    buf_printf(b, " _t%d; })", trp);
    return 1;
  }
  /* any Hash, or a boxed value, which sp_poly_hash_replace checks is one */
  TyKind ot = comp_ntype(c, argv[0]);
  if (rt == TY_POLY_POLY_HASH && (ty_is_hash(ot) || ot == TY_POLY)) {
    int th = ++g_tmp;
    buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", th); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); (void)sp_poly_hash_replace(sp_box_obj(_t%d, SP_BUILTIN_POLY_POLY_HASH), ", th, th);
    emit_boxed(c, argv[0], b);
    buf_printf(b, ", %d); _t%d; })", keep_default, th);
    return 1;
  }
  return 0;
}

/* default=(value) */
int emit_op_hash_set_default(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  /* The value is evaluated once, before a frozen receiver refuses it,
     as a setter's argument is, and the same value is the result. A
     nil-typed value (a nil literal, or a call that returns nil as void)
     is evaluated for its effects and stored as nil. */
  TyKind at = comp_ntype(c, argv[0]);
  int shared_default = strbuf_hash_default_arg(c, x->id) >= 0;
  if (shared_default) at = TY_STRBUF;
  int is_nil = at == TY_NIL || at == TY_VOID;
  int held = !is_nil && (ty_is_object(at) || c_type_name(at));
  int t = ++g_tmp, tv = ++g_tmp;
  char av[32];
  snprintf(av, sizeof av, is_nil ? "0" : "_t%d", tv);
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
  if (shared_default) {
    buf_printf(b, " sp_String *_t%d = ", tv); emit_strbuf_handle_of(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT(_t%d);", tv);
  }
  else if (held) { buf_puts(b, " "); emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";"); }
  else if (is_nil) { buf_puts(b, " (void)("); emit_expr(c, argv[0], b); buf_puts(b, ");"); }
  buf_printf(b, " if (_t%d && sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);",
             t, t, t, hash_box_cls(rt));
  /* --share-strings: a String the rule shares is the default itself (`h.default
     << x`, `h[:missing] << x` change it): its handle, boxed */
  char dref[1024];
  int dhandle = !shared_default && held && (at == TY_STRING || at == TY_STRBUF) && strbuf_var_handle(c, argv[0], dref, sizeof dref);
  if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
    buf_printf(b, " if (_t%d) _t%d->default_v = ", t, t);
    if (is_nil) buf_puts(b, "sp_box_nil()");
    else if (dhandle) buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", dref);
    else if (held) emit_boxed_text(c, at, av, b); else emit_boxed(c, argv[0], b);
    buf_puts(b, ";");
  }
  /* The typed variants keep the default in the values' slot: a value that
     does not fit goes through the store coercion an element store takes
     (a boxed one unboxed, another class refused), where it was assigned as
     it was -- a Float truncated, a Symbol read as its id. The inference
     takes the default as value evidence (infer_write_container_usage), so
     a Hash it can widen has boxed values by now. A boxed default (one a
     parameter's Hash, which the inference does not widen, is given) is
     unboxed as a boxed element store's value is (emit_hash_store_val):
     nil and the values' class are kept, another class raises. */
  else if (rt == TY_STR_INT_HASH || rt == TY_INT_INT_HASH) {
    /* a nil default is the hash's default_nil flag, not a value */
    if (is_nil) buf_printf(b, " if (_t%d) { _t%d->default_nil = 1; _t%d->default_v = 0; }", t, t, t);
    /* a boxed default that is nil is the flag too (the comment above: nil
       is kept); any other is unboxed as before */
    else if (held && at == TY_POLY)
      buf_printf(b, " if (_t%d) { _t%d->default_nil = %s.tag == SP_TAG_NIL; _t%d->default_v = _t%d->default_nil ? 0 : sp_poly_hval_i(%s); }",
                 t, t, av, t, t, av);
    else {
      buf_printf(b, " if (_t%d) { _t%d->default_nil = 0; _t%d->default_v = ", t, t, t);
      if (held && at == TY_POLY) buf_printf(b, "sp_poly_hval_i(%s)", av);
      else if (held) emit_coerce_text(c, argv[0], at, TY_INT, CO_HOLD, av, "a Hash default", b);
      else emit_coerce(c, argv[0], TY_INT, CO_HOLD, "a Hash default", b);
      buf_puts(b, "; }");
    }
  }
  else if (rt == TY_STR_STR_HASH || rt == TY_INT_STR_HASH) {
    buf_printf(b, " if (_t%d) _t%d->default_v = ", t, t);
    if (is_nil) buf_puts(b, "NULL");
    else if (held && at == TY_POLY) buf_printf(b, "sp_poly_hval_s(%s)", av);
    else if (held) emit_coerce_text(c, argv[0], at, TY_STRING, CO_HOLD, av, "a Hash default", b);
    else emit_coerce(c, argv[0], TY_STRING, CO_HOLD, "a Hash default", b);
    buf_puts(b, ";");
  }
  buf_puts(b, " ");
  if (shared_default && !repr_of(c, x->id).demand) buf_printf(b, "sp_strbuf_read_pub(%s)", av);
  else if (held || is_nil) buf_puts(b, av); else emit_expr(c, argv[0], b);
  buf_puts(b, "; })"); return 1;
}

/* merge!/update with several hash arguments: fold each one in, in
   order (#2431). Blockless, same-variant arguments only; a PolyPoly
   receiver, which folds any variant in, is left to the arms after the
   lookup. */
int emit_op_hash_merge_bang_many(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (rt == TY_POLY_POLY_HASH) return 0;
  TyKind kt = ty_hash_key(rt);
  for (int ai = 0; ai < argc; ai++)
    if (!repr_hash_is(repr_of(c, argv[ai]), ty_hash_key(rt), ty_hash_val(rt))) return 0;
  int tr = ++g_tmp;
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
  buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
  for (int ai = 0; ai < argc; ai++) {
    int to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
    buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[ai], b); buf_puts(b, ";");
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
    buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
    int mvn = ty_hash_val(rt) == TY_INT && hash_vals_nullable(c, recv);   /* with its nil (D3b-ii) */
    buf_printf(b, " sp_%sHash_%s(_t%d, _t%d, sp_%sHash_%s(_t%d, _t%d)); }", hn, mvn ? "oset" : "set", tr, tk, hn, mvn ? "vget" : "get", to, tk);
  }
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

/* Hash#shift: remove and return the first-inserted [key, value] pair, or
   nil when empty. */
int emit_op_hash_shift(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  int th = ++g_tmp, tp = ++g_tmp, tr = ++g_tmp, tk = ++g_tmp;
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = sp_box_nil();", th, tr);
  buf_printf(b, " if (_t%d && sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", th, th, th, hash_box_cls(rt));
  buf_printf(b, " if (_t%d && _t%d->len > 0) {", th, th);
  /* bind the first key (raw), used for both the pair and the delete */
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(b, " sp_RbVal _t%d = _t%d->keys[_t%d->order[0]];", tk, th, th);
  else if (kt == TY_SYMBOL)
    buf_printf(b, " sp_sym _t%d = _t%d->order[0];", tk, th);
  else if (kt == TY_STRING)
    buf_printf(b, " const char *_t%d = _t%d->order[0];", tk, th);
  else
    buf_printf(b, " sp_int _t%d = _t%d->order[0];", tk, th);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
  if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_PolyArray_push(_t%d, _t%d);", tp, tk);
  else if (kt == TY_SYMBOL) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d));", tp, tk);
  else if (kt == TY_STRING) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d));", tp, tk);
  else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d));", tp, tk);
  if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[0]]);", tp, th, th);
  else if (vt == TY_POLY) buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d));", tp, hn, th, tk);
  else if (vt == TY_INT && hash_vals_nullable(c, recv)) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_oint(sp_%sHash_vget(_t%d, _t%d)));", tp, hn, th, tk);
  else if (vt == TY_INT) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d)));", tp, hn, th, tk);
  else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d)));", tp, hn, th, tk);
  buf_printf(b, " _t%d = sp_box_poly_array(_t%d);", tr, tp);
  buf_printf(b, " sp_%sHash_delete%s(_t%d, _t%d); }", hn, rt == TY_STR_POLY_HASH ? "_bytes" : "", th, tk);
  buf_printf(b, " _t%d; })", tr);
  return 1;
}

/* delete(key): the deleted value (or nil on a miss), then the key is
   removed. The block form, whose value stands in for a missing key, has a
   row of its own, left to the arm after the lookup. */
int emit_op_hash_delete(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(nt, x->id, &argc);
  TyKind vt = ty_hash_val(rt);
  int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
  buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
  buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", th, th, hash_box_cls(rt));   /* (#3001) */
  /* a key no entry matches (hash_okey_miss) goes through the _okey ops */
  const char *ks = hash_okey_sfx(c, argv[0], ty_hash_key(rt));
  buf_printf(b, " %s _t%d = ", hash_key_ctype(c, argv[0], ty_hash_key(rt)), tk); emit_hash_key_o(c, argv[0], ty_hash_key(rt), b);
  /* a miss answers nil: an Integer value's nil is out of band (an sp_oint),
     not a deleted value of zero (#4531) */
  if (vt == TY_INT)
    buf_printf(b, "; sp_oint _t%d = sp_%sHash_has_key%s(_t%d, _t%d) ? sp_oint_of(sp_%sHash_get%s(_t%d, _t%d)) : sp_oint_nil();",
               tv, hn, ks, th, tk, hn, ks, th, tk);
  else
    buf_printf(b, "; %s _t%d = sp_%sHash_has_key%s(_t%d, _t%d) ? sp_%sHash_get%s(_t%d, _t%d) : %s;",
               c_type_name(vt), tv, hn, ks, th, tk, hn, ks, th, tk,
               vt == TY_POLY ? "sp_box_nil()" : vt == TY_STRING ? "NULL" : default_value_from_compiler(c, vt));
  buf_printf(b, " sp_%sHash_delete%s(_t%d, _t%d); ", hn, rt == TY_STR_POLY_HASH ? "_bytes" : ks, th, tk);
  if (vt == TY_INT) { oint_open(c, x->id, TY_INT, b); buf_printf(b, "_t%d", tv); oint_close(c, x->id, b); buf_puts(b, "; })"); }
  else buf_printf(b, "_t%d; })", tv);
  return 1;
}

/* Hash#invert: the String- and Integer-keyed variants have runtime
   helpers; any other builds a PolyPolyHash by swapping each entry's key
   and value */
int emit_op_hash_invert(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
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
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
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
    /* emit value as sp_RbVal (a PolyPoly receiver reads vals[] directly:
       its _get takes an sp_RbVal key, not the raw order index) (#2407) */
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(b, " sp_RbVal _v%d = _t%d->vals[_t%d->order[_t%d]];", ti, th, th, ti);
    else if (vt == TY_POLY)
      buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
    else if (vt == TY_INT && hash_vals_nullable(c, recv))   /* a nil value becomes a nil key (D3b-ii) */
      buf_printf(b, " sp_RbVal _v%d = sp_box_oint(sp_%sHash_vget(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
    else if (vt == TY_INT) {
      buf_printf(b, " sp_RbVal _v%d = sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
    }
    else {
      buf_printf(b, " sp_RbVal _v%d = sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d]));", ti, hn, th, th, ti);
    }
    buf_printf(b, " sp_PolyPolyHash_set(_t%d, _v%d, _k%d); }", tr, ti, ti);
    buf_printf(b, " _t%d; })", tr);
  }
  return 1;
}

/* Hash#flatten and #flatten(depth) */
int emit_op_hash_flatten(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  if (argc == 1) {
    /* Hash#flatten(d) == to_a.flatten(d): d == 1 is the plain interleave
       (argc == 0 below), d >= 2 also expands array values, d == 0 keeps
       the pairs, negative flattens completely -- all served by the
       depth-limited array flatten over the pair list */
    buf_puts(b, "sp_PolyArray_flatten_depth(");
    emit_hash_pairs_expr(c, recv, rt, hn, b);
    buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }
  /* interleave keys and values into a flat PolyArray */
  int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
  buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
  emit_push_hash_key(kt, tr, th, ti, b);
  /* a PolyPoly table's order lists slots, not keys: the value is read at
     the slot, as emit_hash_pairs_expr reads it (a slot index handed to
     sp_PolyPolyHash_get as the key did not build) */
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", tr, th, th, ti);
  else if (vt == TY_POLY)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tr, hn, th, th, ti);
  else if (vt == TY_INT && hash_vals_nullable(c, recv))
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_oint(sp_%sHash_vget(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
  else if (vt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tr, hn, th, th, ti);
  buf_printf(b, " } _t%d; })", tr);
  return 1;
}

/* to_a / entries: the [key, value] pairs */
int emit_op_hash_to_a(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  return 1;
}

/* sort with no block */
int emit_op_hash_sort(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  /* sort entries by Array#<=> over each [key, value] pair */
  buf_puts(b, "sp_PolyArray_sort_pairs(");
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  buf_puts(b, ")");
  return 1;
}

/* Enumerable first/take/drop over the [key, value] pair list. `first`
   with no argument yields the first pair (nil when empty); the argument
   forms and take/drop return a poly array slice. */
int emit_op_hash_first(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int tp = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  buf_printf(b, "; _t%d->len > 0 ? _t%d->data[0] : sp_box_nil(); })", tp, tp);
  return 1;
}

/* first(n) / take(n) */
int emit_op_hash_take(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int tn = ++g_tmp;
  buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
  buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to take negative size\"); sp_PolyArray_slice(", tn);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  buf_printf(b, ", 0, _t%d); })", tn);
  return 1;
}

/* drop(n) */
int emit_op_hash_drop(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int tp = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
  emit_hash_pairs_expr(c, recv, rt, hn, b);
  /* the fresh pairs array is rooted across the count, as the array
     take/drop arm roots its receiver */
  buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", tp, tn); emit_int_expr(c, argv[0], b);
  buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to drop negative size\"); sp_PolyArray_slice(_t%d, _t%d, _t%d->len - _t%d); })", tn, tp, tn, tp, tn);
  return 1;
}

/* assoc / rassoc */
int emit_op_hash_assoc(Compiler *c, const BopCtx *x, Buf *b) {
  const char *name = x->name;
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  /* find first pair where key==arg (assoc) or value==arg (rassoc); returns [k,v] or nil */
  int is_rassoc = sp_streq(name, "rassoc");
  int env = nt_int(c->nt, x->id, "env_snapshot", 0);
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, ta = ++g_tmp;
  /* PolyPolyHash's order[] holds SLOT INDEXES; keys/vals index directly.
     The other variants store the KEY in order[] and read values through
     sp_<hn>Hash_get(key). Build the value-read expression accordingly. */
  char vget[96];
  /* a value that may be nil is read and boxed with its nil (D3b-ii) */
  int avn = vt == TY_INT && hash_vals_nullable(c, recv);
  const char *ibox = avn ? "sp_box_oint" : "sp_box_int";
  if (rt == TY_POLY_POLY_HASH)
    snprintf(vget, sizeof vget, "_t%d->vals[_t%d->order[_t%d]]", th, th, ti);
  else
    snprintf(vget, sizeof vget, "sp_%sHash_%s(_t%d, _t%d->order[_t%d])", hn, avn ? "vget" : "get", th, th, ti);
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b); buf_puts(b, ";");
  if (env) buf_printf(b, " SP_GC_ROOT(_t%d);", th);
  /* store argument */
  if (!is_rassoc) {
    buf_printf(b, " %s _t%d = ", hash_key_ctype(c, argv[0], kt), ta); emit_hash_key_o(c, argv[0], kt, b); buf_puts(b, ";");
  }
  else {
    /* rassoc: arg has value type */
    buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b); buf_puts(b, ";");
  }
  if (env) buf_printf(b, is_rassoc ? " SP_GC_ROOT_RBVAL(_t%d);" : " SP_GC_ROOT_STR(_t%d);", ta);
  buf_printf(b, " sp_PolyArray *_t%d = NULL;", tr);
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
  if (!is_rassoc) {
    /* assoc: compare key */
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(b, " if (sp_rbval_eql_key(_t%d->keys[_t%d->order[_t%d]], _t%d)) {", th, th, ti, ta);
    else if (kt == TY_STRING)
      /* sp_str_eq, not strcmp: a key of a class the table cannot hold
         reaches here as the NULL miss sentinel emit_hash_key answers */
      buf_printf(b, " if (sp_str_eq(_t%d->order[_t%d], _t%d)) {", th, ti, ta);
    /* a key no entry matches (hash_okey_miss) is a nil sp_oint */
    else if (!is_rassoc && hash_okey_form(c, argv[0], kt))
      buf_printf(b, " if (!_t%d.nil && _t%d->order[_t%d] == _t%d.v) {", ta, th, ti, ta);
    else
      buf_printf(b, " if (_t%d->order[_t%d] == _t%d) {", th, ti, ta);
  }
  else {
    /* rassoc: compare value (boxed) */
    buf_printf(b, " sp_RbVal _rv%d = ", ti);
    if (vt == TY_POLY) buf_printf(b, "%s;", vget);
    else if (vt == TY_INT) buf_printf(b, "%s(%s);", ibox, vget);
    else buf_printf(b, "sp_box_str(%s);", vget);
    buf_printf(b, " if (sp_poly_rb_equal(_rv%d, _t%d)) {", ti, ta);
  }
  /* build pair */
  buf_printf(b, " _t%d = sp_PolyArray_new();", tr);
  if (env) {
    buf_printf(b, " SP_GC_ROOT(_t%d); sp_PolyArray_push(_t%d, sp_box_str(", tr, tr);
    if (is_rassoc) buf_printf(b, "sp_str_dup_external(_t%d->order[_t%d])", th, ti);
    else buf_printf(b, "_t%d", ta);
    buf_puts(b, "));");
  }
  else emit_push_hash_key(kt, tr, th, ti, b);
  if (env && is_rassoc)
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d);", tr, ta);
  else if (vt == TY_POLY)
    buf_printf(b, " sp_PolyArray_push(_t%d, %s);", tr, vget);
  else if (vt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, %s(%s));", tr, ibox, vget);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(%s));", tr, vget);
  buf_printf(b, " break; } } _t%d; })", tr);  /* NULL = nil in poly context */
  return 1;
}

/* compact: a copy without the nil-valued pairs, keeping the default and
   the default proc */
int emit_op_hash_compact(Compiler *c, const BopCtx *x, Buf *b) {
  int recv = x->recv;
  TyKind rt = x->rt;
  const char *hn = ty_hash_cname(rt);
  TyKind vt = ty_hash_val(rt);
  if (vt != TY_POLY) {
    /* Non-poly values can't be nil; compact is equivalent to dup */
    buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
  }
  else if (rt == TY_POLY_POLY_HASH) {
    int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", th); emit_expr(c, recv, b);
    buf_printf(b, "; sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
    /* compact keeps the default and default proc, like dup */
    buf_printf(b, " _t%d->default_v = _t%d->default_v; _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, th, tr, th, tr, th);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
    buf_printf(b, " sp_RbVal _v%d = _t%d->vals[_t%d->order[_t%d]];", ti, th, th, ti);
    buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_PolyPolyHash_set(_t%d, _t%d->keys[_t%d->order[_t%d]], _v%d); }", ti, tr, th, th, ti, ti);
    buf_printf(b, " _t%d; })", tr);
  }
  else {
    /* SYM_POLY_HASH or other poly-valued hash */
    int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
    buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", hn, tr, hn, tr);
    buf_printf(b, " _t%d->default_v = _t%d->default_v; _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, th, tr, th, tr, th);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
    buf_printf(b, " sp_RbVal _v%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);", ti, hn, th, th, ti);
    buf_printf(b, " if (!sp_poly_nil_p(_v%d)) sp_%sHash_set(_t%d, _t%d->order[_t%d], _v%d); }", ti, hn, tr, th, ti, ti);
    buf_printf(b, " _t%d; })", tr);
  }
  return 1;
}

/* an OpenStruct's methods (the TY_OPENSTRUCT arm): [] / []=, to_h, respond_to?, to_s / inspect,
   freeze / frozen?, dup / clone, instance_of? */
int emit_call_openstruct_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* OpenStruct: a dynamic member bag (#3135). `o.k` / `o[:k]` read a boxed
     value (nil when absent), `o.k = v` / `o[:k] = v` write, plus a small
     fixed method surface. Any other bare name is a member access; a handful
     of Object/Kernel names fall through to their generic handlers. */
  if (recv >= 0 && comp_ntype(c, recv) == TY_OPENSTRUCT) {
    if (sp_streq(name, "to_h") && argc == 0) {
      buf_puts(b, "sp_OpenStruct_to_h("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* dup / clone copy the member table. The generic identity shortcut handed
       back the receiver itself, so a write through the copy landed in the
       original. */
    if ((is_copy_alias(name)) && argc == 0) {
      buf_puts(b, "sp_OpenStruct_dup("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "clone") ? 1 : 0);
      return 1;
    }
    if (sp_streq(name, "inspect") || (sp_streq(name, "to_s") && argc == 0)) {
      /* inspect/to_s is a TY_STRING (const char*); wrapping it in sp_String_new
         produced an sp_String* that was then cast straight to const char*, so
         puts printed the struct's raw bytes (#3270). Return the const char*.
         A slot left nil is NULL: nil.inspect is "nil", a new String on each
         call, and nil.to_s is the one shared empty String. */
      int ov = ++g_tmp;
      buf_printf(b, "({ sp_OpenStruct *_t%d = ", ov); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d ? sp_OpenStruct_inspect(_t%d) : %s; })", ov, ov,
                 sp_streq(name, "inspect") ? "sp_str_from_bytes(\"nil\", 3)" : "sp_str_empty");
      return 1;
    }
    if (sp_streq(name, "respond_to?") && argc >= 1) {
      buf_puts(b, "(sp_OpenStruct_has("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (comp_ntype(c, argv[0]) == TY_SYMBOL) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_sym_intern("); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ") ? 1 : 0)");
      return 1;
    }
    /* == / != / === / eql? / equal?: Object's protocol arm -- members for
       == and ===, the table's eql? for eql?, identity for equal?, a poly
       operand unwrapped in place */
    if (argc == 1 && emit_native_object_protocol(c, id, b)) return 1;
    if (is_kind_query(name) && argc == 1 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")) {
      const char *tcn = nt_str(nt, argv[0], "name");
      int yes;
      if (sp_streq(name, "instance_of?")) yes = tcn && sp_streq(tcn, "OpenStruct");
      else yes = tcn && (sp_streq(tcn, "OpenStruct") || sp_streq(tcn, "Object") ||
                         sp_streq(tcn, "Kernel") || sp_streq(tcn, "BasicObject"));
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes ? 1 : 0);
      return 1;
    }
    if (sp_streq(name, "[]") && argc == 1) {
      buf_puts(b, "sp_OpenStruct_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (comp_ntype(c, argv[0]) == TY_SYMBOL) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_sym_intern("); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _v%d = ", tv); emit_boxed(c, argv[1], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_v%d); sp_OpenStruct_set(", tv); emit_expr(c, recv, b); buf_puts(b, ", ");
      if (comp_ntype(c, argv[0]) == TY_SYMBOL) emit_expr(c, argv[0], b);
      else { buf_puts(b, "sp_sym_intern("); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      buf_printf(b, ", _v%d); _v%d; })", tv, tv);
      return 1;
    }
    /* a member writer `o.k = v` (parsed as name "k=") */
    {
      size_t nl = strlen(name);
      if (nl > 1 && name[nl - 1] == '=' && argc == 1 &&
          name[0] != '=' && name[0] != '<' && name[0] != '>' && name[0] != '!') {
        char mem[256];
        if (nl - 1 < sizeof mem) {
          memcpy(mem, name, nl - 1); mem[nl - 1] = 0;
          int tv = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _v%d = ", tv); emit_boxed(c, argv[0], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_v%d); sp_OpenStruct_set(", tv); emit_expr(c, recv, b);
          buf_printf(b, ", sp_sym_intern(\"%s\"), _v%d); _v%d; })", mem, tv, tv);
          return 1;
        }
      }
    }
    /* freeze / frozen? carry the GC-header frozen bit, like the container and
       plain-object freeze paths -- a subsequent member write then raises
       FrozenError (checked in sp_OpenStruct_set) (#3272). */
    if (sp_streq(name, "freeze") && argc == 0) {
      buf_puts(b, "((sp_OpenStruct *)sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (sp_streq(name, "frozen?") && argc == 0) {
      buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* a bare member read `o.k`, unless it is an Object/Kernel method that must
       keep its normal behaviour */
    if (argc == 0) {
      static const char *const OS_METHODS[] = {
        "class", "nil?", "frozen?", "freeze", "dup", "clone", "hash",
        "object_id", "itself", "tap", "then", "yield_self", "inspect",
        "to_s", "to_h", "members", "each_pair", "send", "__send__",
        "instance_variables", "methods", "is_a?", "kind_of?", NULL };
      int reserved = 0;
      for (int mi = 0; OS_METHODS[mi]; mi++)
        if (sp_streq(name, OS_METHODS[mi])) { reserved = 1; break; }
      if (!reserved) {
        buf_puts(b, "sp_OpenStruct_get("); emit_expr(c, recv, b);
        buf_printf(b, ", sp_sym_intern(\"%s\"))", name);
        return 1;
      }
    }
  }
  return 0;
}

/* a Hash receiver: a literal Hash.new(d) that never narrowed (default, values_at, []), default= / default / default_proc on a receiver not typed a hash, then the Hash emitters (emit_hash_call) */
int emit_call_hash_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
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
      TyKind dt = repr_of(c, dn).as_ty;
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
        return 1;
      }
      if (argc == 1) { buf_puts(b, "(void)("); emit_boxed(c, argv[0], b); buf_puts(b, "); "); }
      buf_printf(b, "_t%d; })", t);
      return 1;
    }
  }
  /* h.default = v in value position: store when the receiver is a typed
     hash lvalue; a literal {} receiver just yields the value. */
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && !ty_is_hash(rt) &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
    TyKind vt = repr_of(c, argv[0]).as_ty;
    int t = ++g_tmp;
    buf_puts(b, "({ ");
    emit_ctype(c, vt, b);
    buf_printf(b, " _t%d = ", t);
    emit_expr(c, argv[0], b);
    buf_printf(b, "; _t%d; })", t);
    return 1;
  }
  /* {}.default (empty hash literal with unknown type) always returns nil; a
     boxed hash answers through its face row below, from the copy's default */
  if (recv >= 0 && sp_streq(name, "default") && argc == 0 && !ty_is_hash(rt) && rt != TY_POLY) {
    buf_puts(b, "sp_box_nil()");
    return 1;
  }
  /* default_proc on a hash that never narrowed to a variant: a blockless
     Hash.new (and the empty literal) has no default block, so nil (#3568) */
  if (recv >= 0 && sp_streq(name, "default_proc") && argc == 0 && !ty_is_hash(rt) &&
      hash_new_blockless(c, recv)) {
    /* the hash itself has no C value here (it never narrowed), so evaluate
       only a default argument the receiver may carry, for its effect */
    int dn = hash_new_default_arg(c, recv);
    buf_puts(b, "(");
    if (dn >= 0) { buf_puts(b, "(void)("); emit_expr(c, dn, b); buf_puts(b, "), "); }
    buf_puts(b, "(sp_Proc *)NULL)");
    return 1;
  }
  if (emit_or_take_back(c, id, b, emit_hash_call)) return 1;
  return 0;
}
