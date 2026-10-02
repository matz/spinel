#include "codegen_internal.h"
#include "call_plan.h"

/* Defined lower in this file; declared here so the collecting emitters above
   its definition (the hash block-walk binder, flat_map) can route a block's
   next-aware value into a caller-declared lvalue. */
void emit_block_value_into(Compiler *c, int block, const char *dest,
                           int want_poly, int indent);

int resolve_forwarded_block(Compiler *c, int block) {
  const NodeTable *nt = c->nt;
  if (block < 0) return block;
  const char *type = nt_type(nt, block);
  if (!type || !sp_streq(type, "BlockArgumentNode")) return block;
  int fwd_expr = nt_ref(nt, block, "expression");
  int forwards_param = 0;
  if (fwd_expr < 0) {
    forwards_param = 1;  /* anonymous `&` */
  }
  else if (g_block_param_name) {
    const char *fwd_type = nt_type(nt, fwd_expr);
    if (fwd_type && sp_streq(fwd_type, "LocalVariableReadNode")) {
      const char *en = nt_str(nt, fwd_expr, "name");
      forwards_param = en && sp_streq(en, g_block_param_name);
    }
  }
  /* g_block_id is -1 when the caller passed no block, so a forwarded nil block
     falls through to a NULL argument (the callee's own nil-check handles it). */
  return forwards_param ? g_block_id : block;
}

/* The value of a `&expr` block argument as the sp_Proc * a `&blk`
   parameter takes: a Proc as itself, a boxed value through
   sp_poly_to_block (nil is no block), a Method through its trampoline
   proc. Writes nothing and returns 0 for any other type. Each site that
   hands a block argument to a real `&blk` parameter goes through here;
   they each had the Proc arm alone and passed NULL for the rest, so the
   method ran without its block and the expression was never evaluated. */
int emit_block_arg_proc(Compiler *c, int fe, Buf *b) {
  TyKind t = comp_ntype(c, fe);
  if (t == TY_PROC) { emit_expr(c, fe, b); return 1; }
  if (t == TY_POLY) {
    buf_puts(b, "sp_poly_to_block("); emit_boxed(c, fe, b); buf_puts(b, ")");
    return 1;
  }
  if (t == TY_METHOD) {
    /* rooted across the proc's allocation, as Method#to_proc roots it */
    int tp = ++g_tmp;
    buf_printf(b, "({ sp_BoundMethod *_t%d = ", tp);
    emit_expr(c, fe, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_method_to_proc(_t%d); })", tp, tp);
    return 1;
  }
  return 0;
}

/* A BlockArgumentNode that survives resolve_forwarded_block has no inline
   block to splice: it forwards a REAL proc -- a proc-valued expression
   (`&block` from a real-function body, e.g. a self-recursive block method;
   see emit_block_arg_proc), or the caller's own proc param via anonymous
   `&`. Write that proc expression (or NULL) into b and return 1; return 0
   when blk_node isn't that shape (a literal block, for emit_proc_literal).
   Mirrors the same branch in emit_cmethod_block_arg. */
int emit_forwarded_proc_arg(Compiler *c, int blk_node, Buf *b) {
  const NodeTable *nt = c->nt;
  if (blk_node < 0) return 0;
  const char *ty = nt_type(nt, blk_node);
  if (!ty || !sp_streq(ty, "BlockArgumentNode")) return 0;
  int fe = nt_ref(nt, blk_node, "expression");
  if (fe >= 0 && emit_block_arg_proc(c, fe, b)) return 1;
  if (fe < 0) {
    /* anonymous `&`: the BlockArgumentNode sits in the caller's body, so its
       scope is the caller -- forward that method's own proc param */
    Scope *caller = comp_scope_of(c, blk_node);
    if (caller && caller->blk_param && caller->blk_param[0] && !caller->yields) {
      buf_printf(b, "lv_%s", caller->blk_param);
      return 1;
    }
  }
  buf_puts(b, "NULL");
  return 1;
}

static int emit_blk_proc_tmp(Compiler *c, int blk_node) {
  int blk_tmp = ++g_tmp;
  Buf pb; memset(&pb, 0, sizeof pb);
  if (!emit_forwarded_proc_arg(c, blk_node, &pb))
    emit_proc_literal(c, blk_node, &pb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", blk_tmp);
  free(pb.p);
  return blk_tmp;
}

void emit_method_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  /* the target is the call's plan (call_plan.c): a top-level def, reached
     bare or through a retargeted send. A plan of another kind does not
     serve this site, which then takes the top-level def by name as it
     always did; --plan-check reports both that fallback and any plan that
     names another method than the by-name lookup. */
  const CallPlan *pl = cplan_user(c, id);
  int mi;
  if (pl->mi >= 0 && (pl->via == UC_TOP || pl->via == UC_SEND_BLIND)) {
    mi = pl->mi;
    if (g_plan_check) cplan_served("emit_method_call");
    if (g_plan_check && mi != comp_method_index(c, name))
      fprintf(stderr, "plan-check: cplan-conflict: emit_method_call node %d %s: plan %d, by name %d\n",
              id, name ? name : "?", mi, comp_method_index(c, name));
  }
  else {
    mi = comp_method_index(c, name);
    if (g_plan_check)
      fprintf(stderr, "plan-check: cplan-fallback: emit_method_call node %d %s\n", id, name ? name : "?");
  }
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  /* a top-level alias reaches the target's one function: hand it the spelled
     name for __callee__ (#3729) */
  if (m && m->name && !sp_streq(m->name, name) && scope_reads_callee(c, mi)) {
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "sp_callee_name = ");
    emit_str_literal(g_pre, name);
    buf_puts(g_pre, ";\n");
  }
  /* a top-level alias resolves to the target's scope: emit ITS symbol, since
     the alias has no function of its own (#3730) */
  if (m) nd_callee(c, id, mi, -1, 0);
  buf_printf(b, "sp_%s(", mc_top(c, m && m->name ? m->name : name));
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  /* pass &block as sp_Proc * when the callee has a blk_param and isn't inlined */
  if (m && m->blk_param && m->blk_param[0] && !m->yields) {
    /* A forwarded `&blk` can't be materialized directly by emit_proc_literal;
       resolve it to the caller's inlined block. Without this, forwarding `&blk`
       into a callee that keeps a real proc param (e.g. one that nil-checks the
       block) is rejected as "proc literal without a block". */
    int blk_node = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
    int wrote_args = m->nparams > 0;
    if (wrote_args) buf_puts(b, ", ");
    if (blk_node >= 0) {
      buf_printf(b, "_t%d", emit_blk_proc_tmp(c, blk_node));
    }
    else {
      buf_puts(b, "NULL");
    }
  }
  buf_puts(b, ")");
}

int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty,
                           int *ids_out, TyKind *ty_out, int cap);

/* Emit, into g_pre after the caller's `<lhs> = `, the rest of the statement
   binding a hash block's first parameter to entry `ti` of `_t<trecv>`: the
   boxed [k, v] pair when `pair`, else the key or the value per `is_key`. */
static void emit_hash_p0_rhs(Compiler *c, TyKind rt, const char *hn,
                             int trecv, int ti, int pair, int is_key) {
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  if (pair) {
    int tpp = ++g_tmp;
    buf_printf(g_pre, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tpp, tpp);
    if (rt == TY_POLY_POLY_HASH) {
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]); ", tpp, trecv, trecv, ti);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]); ", tpp, trecv, trecv, ti);
    }
    else {
      char kx[96], vx[128];
      snprintf(kx, sizeof kx, "_t%d->order[_t%d]", trecv, ti);
      snprintf(vx, sizeof vx, "sp_%sHash_get(_t%d, _t%d->order[_t%d])", hn, trecv, trecv, ti);
      Buf bk; memset(&bk, 0, sizeof bk); emit_boxed_text(c, kt, kx, &bk);
      Buf bv; memset(&bv, 0, sizeof bv); emit_boxed_text(c, vt, vx, &bv);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s); sp_PolyArray_push(_t%d, %s); ",
                 tpp, bk.p ? bk.p : "sp_box_nil()", tpp, bv.p ? bv.p : "sp_box_nil()");
      free(bk.p); free(bv.p);
    }
    buf_printf(g_pre, "sp_box_poly_array(_t%d); });\n", tpp);
  }
  else if (is_key) {
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, "_t%d->keys[_t%d->order[_t%d]];\n", trecv, trecv, ti);
    else
      buf_printf(g_pre, "_t%d->order[_t%d];\n", trecv, ti);
  }
  else {
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, "_t%d->vals[_t%d->order[_t%d]];\n", trecv, trecv, ti);
    else
      buf_printf(g_pre, "sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", hn, trecv, trecv, ti);
  }
}

/* Bind a hash-iteration block's parameters to C locals for entry `ti` of the
   materialized hash temp `_t<trecv>` (type rt, runtime cname hn), emit the
   block's leading statements into g_pre at g_indent+1, evaluate its final
   expression, then restore every temporary shadow-type and rename change.
   Returns the final expression's text (caller frees) and its inferred type via
   *out_bret. `p0_solo_is_value` selects map-style single-parameter binding (the
   lone parameter receives the value) over select-style (it receives the key).
   The caller emits the loop header and consumes the returned text; this routine
   owns the intricate |k, v| binding shared by every hash block walk. */
static char *emit_hash_block_eval(Compiler *c, int block, TyKind rt, const char *hn,
                                  int trecv, int ti, int p0_solo_is_value, TyKind *out_bret) {
  const NodeTable *nt = c->nt;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;

  Scope *pscope = comp_scope_of(c, block);
  /* p0 is the key if 2 params; solo it binds by mode: 0 = key (Hash-specific
     methods yield k, v), 1 = value, 2 = the boxed [k, v] pair (Enumerable
     methods yield the pair as ONE argument). */
  TyKind p0_actual = p1_orig ? kt
                   : p0_solo_is_value == 2 ? TY_POLY
                   : p0_solo_is_value ? vt : kt;   /* mode 0 solo READS the key: type it so */
  TyKind p1_actual = vt;
  LocalVar *p0_lv = p0_orig ? scope_local(pscope, p0_orig) : NULL;
  LocalVar *p1_lv = p1_orig ? scope_local(pscope, p1_orig) : NULL;
  TyKind p0_decl = p0_lv ? p0_lv->type : TY_UNKNOWN;
  TyKind p1_decl = p1_lv ? p1_lv->type : TY_UNKNOWN;
  int ns0 = p0_orig && p0_actual != TY_UNKNOWN && p0_decl != TY_UNKNOWN && p0_decl != p0_actual;
  int ns1 = p1_orig && p1_actual != TY_UNKNOWN && p1_decl != TY_UNKNOWN && p1_decl != p1_actual;
  int st0 = -1, sri0 = -1, srn0 = 0; char sro0[112]; sro0[0] = '\0';
  int st1 = -1, sri1 = -1, srn1 = 0; char sro1[112]; sro1[0] = '\0';
  /* p0 reads the key for a 2-param block, or for select-style solo binding. */
  int p0_is_key = p1_orig || !p0_solo_is_value;

  if (p0_orig) {
    emit_indent(g_pre, g_indent + 1);
    if (ns0) {
      st0 = ++g_tmp; emit_ctype(c, p0_actual, g_pre);
      buf_printf(g_pre, " lv__bp%d = ", st0);
      emit_hash_p0_rhs(c, rt, hn, trecv, ti, !p1_orig && p0_solo_is_value == 2, p0_is_key);
      for (int ri = 0; ri < g_nren; ri++) {
        if (sp_streq(g_ren_from[ri], p0_orig)) {
          sri0 = ri; strncpy(sro0, g_ren_to[ri], sizeof sro0 - 1);
          snprintf(g_ren_to[ri], sizeof g_ren_to[0], "_bp%d", st0); break;
        }
      }
      if (sri0 < 0) { sri0 = g_nren; srn0 = 1;
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", p0_orig);
        snprintf(g_ren_to[g_nren++], sizeof g_ren_to[0], "_bp%d", st0);
      }
    }
    else {
      buf_printf(g_pre, "lv_%s = ", p0);
      emit_hash_p0_rhs(c, rt, hn, trecv, ti, !p1_orig && p0_solo_is_value == 2, p0_is_key);
    }
  }
  if (p1_orig) {
    emit_indent(g_pre, g_indent + 1);
    if (ns1) {
      st1 = ++g_tmp; emit_ctype(c, p1_actual, g_pre);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, " lv__bp%d = _t%d->vals[_t%d->order[_t%d]];\n", st1, trecv, trecv, ti);
      else
        buf_printf(g_pre, " lv__bp%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", st1, hn, trecv, trecv, ti);
      for (int ri = 0; ri < g_nren; ri++) {
        if (sp_streq(g_ren_from[ri], p1_orig)) {
          sri1 = ri; strncpy(sro1, g_ren_to[ri], sizeof sro1 - 1);
          snprintf(g_ren_to[ri], sizeof g_ren_to[0], "_bp%d", st1); break;
        }
      }
      if (sri1 < 0) { sri1 = g_nren; srn1 = 1;
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", p1_orig);
        snprintf(g_ren_to[g_nren++], sizeof g_ren_to[0], "_bp%d", st1);
      }
    }
    else {
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", p1, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", p1, hn, trecv, trecv, ti);
    }
  }
  if (ns0 && p0_lv) p0_lv->type = p0_actual;
  if (ns1 && p1_lv) p1_lv->type = p1_actual;
  TyKind bret = (bb && bn > 0) ? comp_ntype(c, bb[bn - 1]) : TY_UNKNOWN;
  /* A value-carrying next widens the block value past the tail, so the temp is
     boxed when a next yields a different type than the tail expression. */
  TyKind bnt = ie_block_break_next_ty(c, body);
  if (bnt != TY_UNKNOWN) bret = (bret == TY_UNKNOWN) ? bnt : ty_unify(bret, bnt);
  if (bret == TY_UNKNOWN) bret = (ns1 ? p1_actual : p0_actual);
  /* Collect the block's value next-aware into a temp: a tail or interior
     `next <v>` assigns the temp and falls through to the caller's collection
     rather than dropping the entry as a bare continue would. */
  int tvv = ++g_tmp; char tvvb[24]; snprintf(tvvb, sizeof tvvb, "_t%d", tvv);
  /* a block that always yields nil has TY_NIL/TY_VOID element type, which has
     no C storage (emit_ctype -> void); collect it as a boxed poly nil (#2343). */
  int want_poly = (bret == TY_POLY || bret == TY_NIL || bret == TY_VOID);
  emit_indent(g_pre, g_indent + 1);
  if (want_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tvv);
  else { emit_ctype(c, bret, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tvv, default_value(bret)); }
  emit_block_value_into(c, block, tvvb, want_poly, g_indent + 1);
  if (ns0 && p0_lv) p0_lv->type = p0_decl;
  if (ns1 && p1_lv) p1_lv->type = p1_decl;
  if (sri1 >= 0) { if (srn1) g_nren = sri1; else strncpy(g_ren_to[sri1], sro1, sizeof g_ren_to[0]-1); }
  if (sri0 >= 0) { if (srn0) g_nren = sri0; else strncpy(g_ren_to[sri0], sro0, sizeof g_ren_to[0]-1); }
  if (out_bret) *out_bret = bret;
  Buf rv; memset(&rv, 0, sizeof rv); buf_puts(&rv, tvvb); return rv.p;
}

static void emit_pre_rooted_recv(Compiler *c, int recv, TyKind rt, int t) {
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = ", t); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
}

/* hash.map / collect { |k, v| ... } as an expression -> an array of the block
   values, built via a loop over the hash entries in the statement prelude. */
int emit_hash_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  /* A `&b` that forwards the enclosing method's block parameter resolves to
     the caller's literal block, exactly as the array collect path resolves
     it: without that these arms saw a BlockArgumentNode with no body, all
     declined, and every Hash iterator reached through a forwarding method
     raised NoMethodError at run time. */
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  TyIterShape shp = ty_iter_shape(name);
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  int is_map = shp == TY_ITER_MAP;
  if (!is_map && !is_sel && !is_rej) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  /* The loop below reads this temp's `len` as its bound on every turn and
     takes the entry out of it on every yield, and the block between two
     turns allocates. A receiver with no other holder -- the hash a method
     call returned -- is rooted here, as the Hash sort_by, sum and group_by
     hoists in this file already were. */
  emit_pre_rooted_recv(c, recv, rt, trecv);

  if (is_sel || is_rej) {
    /* select/reject: produce a same-type hash with matching pairs */
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new();\n", tres, hn);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, 0, NULL);
    emit_indent(g_pre, g_indent + 1);
    TyKind vtt = ty_hash_val(rt);
    /* Ruby truthiness on the block's value. A boxed one -- the block calls a
       Proc held as the hash value, so its result is only known at run time --
       is a struct, and `!(struct)` does not compile at all (#3426). */
    { int bn2 = 0; int bbody = nt_ref(nt, block, "body");
      const int *bb2 = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn2) : NULL;
      TyKind bvt2 = bn2 > 0 ? comp_ntype(c, bb2[bn2 - 1]) : TY_UNKNOWN;
      /* A body whose value IS nil types VOID/NIL, which is neither a boxed
         value to ask sp_poly_truthy about nor a scalar to test -- `select
         { nil }` did not compile. It is statically FALSY, so the pair is
         dropped (kept, for reject); the body still runs, for its effects. */
      if (bvt2 == TY_VOID || bvt2 == TY_NIL)
        buf_printf(g_pre, "if (((void)(%s), %d)) { ", vb ? vb : "0", is_rej ? 1 : 0);
      else if (bvt2 == TY_POLY || bvt2 == TY_UNKNOWN)
        buf_printf(g_pre, "if (%ssp_poly_truthy(%s)) { ", is_rej ? "!" : "", vb ? vb : "sp_box_nil()");
      else
        buf_printf(g_pre, "if (%s(%s)) { ", is_rej ? "!" : "", vb ? vb : "0");
    }
    free(vb);
    if (rt == TY_POLY_POLY_HASH) {
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d->keys[_t%d->order[_t%d]], _t%d->vals[_t%d->order[_t%d]]); }",
                 hn, tres, trecv, trecv, ti, trecv, trecv, ti);
    }
    else {
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d->order[_t%d], sp_%sHash_%s(_t%d, _t%d->order[_t%d])); }",
                 hn, tres, trecv, ti, hn, vtt == TY_INT ? "get_opt" : "get", trecv, trecv, ti);
    }
    buf_puts(g_pre, "\n");
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  else {
    /* map: produce a result array */
    TyKind restype = comp_ntype(c, id);
    int res_poly = (restype == TY_POLY_ARRAY);
    const char *rk = res_poly ? "Poly" : array_kind(restype);
    if (!rk) return 0;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    TyKind bret;
    char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, block_param_name(c, block, 1) ? 1 : 2, &bret);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_%sArray_push%s(_t%d, ", rk, nil_store_sfx(c, rk, NIL_STORE_BOXED), tres);
    if (res_poly && bret != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, bret, vb ? vb : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_puts(g_pre, vb ? vb : "");
    buf_puts(g_pre, ");\n"); free(vb);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit, into g_pre at `indent`, `_t<dest> = sp_PolyArray_new();` then push entry
   `ti`'s boxed key and value. `dest` must already be a rooted sp_PolyArray*. */
static void emit_hash_pair_at(TyKind rt, const char *hn,
                              int trecv, int ti, int dest, int indent) {
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  emit_indent(g_pre, indent);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new();", dest);
  if (kt == TY_SYMBOL)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", dest, trecv, ti);
  else if (kt == TY_STRING)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", dest, trecv, ti);
  else if (kt == TY_INT)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", dest, trecv, ti);
  else
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", dest, trecv, trecv, ti);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", dest, trecv, trecv, ti);
  else if (vt == TY_POLY)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", dest, hn, trecv, trecv, ti);
  else if (vt == TY_INT)
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", dest, hn, trecv, trecv, ti);
  else
    buf_printf(g_pre, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", dest, hn, trecv, trecv, ti);
  buf_puts(g_pre, "\n");
}

/* hash.min_by / max_by / find / detect { |k, v| ... } -> the winning [k, v]
   pair, or nil when no entry qualifies. Emitted as prelude statements that
   produce a result temp, like the collect walk. */
int emit_hash_reduce_search_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  int is_min = sp_streq(name, "min_by"), is_max = sp_streq(name, "max_by");
  int is_find = sp_streq(name, "find") || sp_streq(name, "detect");
  if ((!is_min && !is_max && !is_find) || argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp, tbest = ++g_tmp, twin = ++g_tmp;
  emit_pre_rooted_recv(c, recv, rt, trecv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tres, tres);
  /* Index of the winning entry, or -1 if none qualified. The pair is built
     once after the loop, so no result array is allocated until the walk is
     done (and never at all for a no-match find, which renders as nil). */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = -1;\n", twin);
  if (is_min || is_max) {
    emit_indent(g_pre, g_indent);
    /* The best-so-far block value is held across allocating iterations, so it
       must be rooted; SP_GC_ROOT_RBVAL roots by address, so the later
       reassignment is tracked without a re-root. Seed it nil so the root scan
       sees a valid value before the first assignment. */
    buf_printf(g_pre, "sp_RbVal _bk%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_bk%d);\n", tbest, tbest);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, block_param_name(c, block, 1) ? 0 : 2, &bret);
  if (is_find) {
    emit_indent(g_pre, g_indent + 1);
    /* a nil block value types VOID/NIL: statically falsy, so no entry ever
       wins -- but the body still runs, for its effects (see select above) */
    if (bret == TY_VOID || bret == TY_NIL)
      buf_printf(g_pre, "if (((void)(%s), 0)) {\n", vb ? vb : "0");
    else if (bret == TY_POLY)
      buf_printf(g_pre, "if (sp_poly_truthy(%s)) {\n", vb ? vb : "sp_box_nil()");
    else
      buf_printf(g_pre, "if (%s) {\n", vb ? vb : "0");
    emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d = _t%d; break;\n", twin, ti);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  }
  else {
    /* box the block's value so any comparable type sorts uniformly */
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _kv%d = ", ti);
    if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent + 1);
    /* the ORDERING entry, not the Comparable operator: a key that is always
       nil ties everywhere, so every entry keeps the first (#4006) */
    buf_printf(g_pre, "if (_t%d == -1 || sp_poly_order_%s(_kv%d, _bk%d)) {\n",
               twin, is_min ? "lt" : "gt", ti, tbest);
    emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d = _t%d; _bk%d = _kv%d;\n", twin, ti, tbest, ti);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  }
  free(vb);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d >= 0) {\n", twin);
  emit_hash_pair_at(rt, hn, trecv, twin, tres, g_indent + 1);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* hash.sort_by { |k, v| ... } -> the [k, v] pairs ordered by the block's value.
   Builds [sort_key, pair] tuples in the prelude, then sorts and projects them. */
int emit_hash_sort_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!sp_streq(name, "sort_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int trecv = ++g_tmp, ttmp = ++g_tmp, ti = ++g_tmp, tup = ++g_tmp, tpair = ++g_tmp;
  emit_pre_rooted_recv(c, recv, rt, trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ttmp, ttmp);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, block_param_name(c, block, 1) ? 0 : 2, &bret);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tup, tup);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tup);
  if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
         buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
  buf_puts(g_pre, ");\n");
  free(vb);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tpair, tpair);
  emit_hash_pair_at(rt, hn, trecv, ti, tpair, g_indent + 1);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tup, tpair);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", ttmp, tup);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "sp_PolyArray_sort_by_first(_t%d)", ttmp);
  return 1;
}

/* hash.sum(init) / count / all? / any? { |k, v| ... } -> a scalar reduction.
   sum accumulates the block value (boxed, via sp_poly_add); count tallies truthy
   results; all?/any? short-circuit to a boolean. */
int emit_hash_reduce_scalar_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_sum = sp_streq(name, "sum"), is_count = sp_streq(name, "count");
  int is_all = sp_streq(name, "all?"), is_any = sp_streq(name, "any?");
  int is_none = sp_streq(name, "none?"), is_one = sp_streq(name, "one?");
  if (!is_sum && !is_count && !is_all && !is_any && !is_none && !is_one) return 0;
  int argc = 0; { int ar = nt_ref(nt, id, "arguments"); if (ar >= 0) nt_arr(nt, ar, "arguments", &argc); }
  if (is_sum ? argc > 1 : argc != 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  TyKind rt = comp_ntype(c, recv);
  const char *hn = ty_hash_cname(rt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; if (body >= 0) nt_arr(nt, body, "body", &bn);
  if (bn < 1) return 0;

  int sum_init = -1;
  if (is_sum && argc == 1) {
    int ar = nt_ref(nt, id, "arguments"); int an = 0;
    const int *aa = ar >= 0 ? nt_arr(nt, ar, "arguments", &an) : NULL;
    if (aa && an >= 1) sum_init = aa[0];
  }

  int trecv = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  emit_pre_rooted_recv(c, recv, rt, trecv);
  /* The initial value is rendered into a side buffer FIRST, the way the
     receiver above is. Written straight into g_pre it landed in the middle of
     the line being built there: an empty array literal needs construction
     statements, and those hoist into g_pre too, so `sum([]) { }` emitted
     `sp_RbVal _t2 = sp_box_nullable_obj((void *)(  sp_PolyArray *_t7 = ...;`
     and the program did not compile (#4289). Rendering first puts the hoists
     ahead of the line instead of inside it. */
  Buf sib; memset(&sib, 0, sizeof sib);
  if (is_sum && sum_init >= 0) emit_boxed(c, sum_init, &sib);
  emit_indent(g_pre, g_indent);
  if (is_sum) {
    buf_printf(g_pre, "sp_RbVal _t%d = ", tacc);
    if (sum_init >= 0) buf_puts(g_pre, sib.p ? sib.p : "sp_box_nil()");
    else buf_puts(g_pre, "sp_box_int(0)");
    buf_puts(g_pre, ";\n");
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tacc);
  }
  else if (is_count || is_one)
    buf_printf(g_pre, "sp_int _t%d = 0;\n", tacc);   /* one? tallies, checks == 1 */
  else
    buf_printf(g_pre, "sp_bool _t%d = %s;\n", tacc, (is_all || is_none) ? "TRUE" : "FALSE");
  free(sib.p);

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
  TyKind bret;
  char *vb = emit_hash_block_eval(c, block, rt, hn, trecv, ti, block_param_name(c, block, 1) ? 0 : 2, &bret);
  if (is_sum) {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "_t%d = sp_poly_add(_t%d, ", tacc, tacc);
    if (bret == TY_POLY) buf_puts(g_pre, vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(g_pre, ");\n");
  }
  else {
    /* truthiness of the block result, formatted into a dynamic Buf so a long
       boxed expression can never be silently truncated */
    Buf cond; memset(&cond, 0, sizeof cond);
    if (bret == TY_BOOL) buf_printf(&cond, "(%s)", vb ? vb : "0");
    else if (bret == TY_POLY) buf_printf(&cond, "sp_poly_truthy(%s)", vb ? vb : "sp_box_nil()");
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb ? vb : "", &bx);
           buf_printf(&cond, "sp_poly_truthy(%s)", bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    const char *cs = cond.p ? cond.p : "0";
    emit_indent(g_pre, g_indent + 1);
    if (is_count || is_one) buf_printf(g_pre, "if (%s) _t%d++;\n", cs, tacc);  /* one?: tally, break >1 below */
    else if (is_all) buf_printf(g_pre, "if (!(%s)) { _t%d = FALSE; break; }\n", cs, tacc);
    else if (is_none) buf_printf(g_pre, "if (%s) { _t%d = FALSE; break; }\n", cs, tacc);
    else buf_printf(g_pre, "if (%s) { _t%d = TRUE; break; }\n", cs, tacc);
    if (is_one) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d > 1) break;\n", tacc); }
    free(cond.p);
  }
  free(vb);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (is_one) buf_printf(b, "(_t%d == 1)", tacc);
  else buf_printf(b, "_t%d", tacc);
  return 1;
}

/* Recursively patch c->ntype[id]=ty for every LocalVariableReadNode named nm
   within the subtree at id. Saved old values in ids_out/ty_out (max cap).
   Returns count of patched nodes. */
int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty,
                           int *ids_out, TyKind *ty_out, int cap) {
  if (id < 0 || id >= c->nt->count || cap <= 0) return 0;
  int n = 0;
  const char *node_ty = nt_type(c->nt, id);
  if (node_ty && sp_streq(node_ty, "LocalVariableReadNode")) {
    const char *vname = nt_str(c->nt, id, "name");
    if (vname && sp_streq(vname, nm)) {
      ids_out[0] = id; ty_out[0] = c->ntype[id]; c->ntype[id] = ty;
      return 1;
    }
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr && n < cap; i++) {
    int r = nt_ref_at(c->nt, id, i);
    n += patch_lv_reads(c, r, nm, ty, ids_out + n, ty_out + n, cap - n);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na && n < cap; i++) {
    int cn = 0; const int *ids = nt_arr_at(c->nt, id, i, &cn);
    for (int j = 0; j < cn && n < cap; j++)
      n += patch_lv_reads(c, ids[j], nm, ty, ids_out + n, ty_out + n, cap - n);
  }
  return n;
}

/* Scan nodes with IDs in [min_id, max_id) for LocalVariableReadNode with the
   given name in the given scope, and patch c->ntype[id] to `new_ty`.
   Prism assigns IDs in pre-order, so all descendants of `block` have id > block.
   Passing min_id=block+1 restricts patching to the block's own subtree.
   Stores original types in saved[] (caller must free). Returns the count. */
int patch_lv_read_ntype(Compiler *c, int scope_idx, const char *name,
                                TyKind new_ty, int min_id,
                                int **saved_ids, TyKind **saved_tys) {
  int n = 0, cap = 8;
  *saved_ids = malloc(sizeof(int) * (size_t)cap);
  *saved_tys = malloc(sizeof(TyKind) * (size_t)cap);
  for (int i = min_id; i < c->nt->count; i++) {
    const char *ty = nt_type(c->nt, i);
    if (!ty || !sp_streq(ty, "LocalVariableReadNode")) continue;
    if (c->nscope[i] != scope_idx) continue;
    const char *nm = nt_str(c->nt, i, "name");
    if (!nm || !sp_streq(nm, name)) continue;
    if (c->ntype[i] == new_ty) continue;
    if (n >= cap) { cap *= 2; *saved_ids = realloc(*saved_ids, sizeof(int) * (size_t)cap); *saved_tys = realloc(*saved_tys, sizeof(TyKind) * (size_t)cap); }
    (*saved_ids)[n] = i;
    (*saved_tys)[n] = c->ntype[i];
    c->ntype[i] = new_ty;
    n++;
  }
  return n;
}

void restore_lv_read_ntype(Compiler *c, int *saved_ids, TyKind *saved_tys, int n) {
  for (int i = 0; i < n; i++) c->ntype[saved_ids[i]] = saved_tys[i];
  free(saved_ids); free(saved_tys);
}

/* hash.transform_keys { |k| nk } / transform_values { |v| nv }: rebuild the
   hash applying the block to every key (or value), keeping the other half.
   Returns 1 if handled. */
int emit_transform_hash_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "transform_keys") && !sp_streq(name, "transform_values"))) return 0;
  int keys = sp_streq(name, "transform_keys");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *shn = ty_hash_cname(rt);
  if (!shn) return 0;
  TyKind dt = comp_ntype(c, id);
  const char *dhn = ty_hash_cname(dt);
  /* a call typed boxed (a dispatch's builtin arm asks this way): build the
     Hash of any keys and values, and box it */
  if (!dhn && dt == TY_POLY) {
    Buf hb; memset(&hb, 0, sizeof hb);
    int v = view_push(c, id, TY_POLY_POLY_HASH);
    int ok = emit_transform_hash_expr(c, id, &hb);
    view_pop(c, v);
    if (ok) emit_boxed_text(c, TY_POLY_POLY_HASH, hb.p ? hb.p : "NULL", b);
    free(hb.p);
    return ok;
  }
  if (!dhn) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* Empty block: transform_values { } → all values become nil (keep key set) */
  if (bn < 1) {
    if (keys) return 0;
    int ts2 = ++g_tmp, td2 = ++g_tmp, ti2 = ++g_tmp, tk2 = ++g_tmp;
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", ts2); buf_puts(g_pre, rb2.p ? rb2.p : ""); buf_puts(g_pre, ";\n"); free(rb2.p);
    /* rooted for the walk: the entry count below is re-read from this temp
       on every turn and the block can allocate */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ts2);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td2, shn, td2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, ts2, ti2);
    emit_indent(g_pre, g_indent + 1); emit_ctype(c, ty_hash_key(rt), g_pre);
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk2, ts2, ts2, ti2);
    else
      buf_printf(g_pre, " _t%d = _t%d->order[_t%d];\n", tk2, ts2, ti2);
    emit_indent(g_pre, g_indent + 1);
    { TyKind vt2 = ty_hash_val(rt);
      const char *nil_v = (vt2 == TY_INT) ? "SP_INT_NIL" :
                          (vt2 == TY_POLY) ? "sp_box_nil()" : "NULL";
      buf_printf(g_pre, "sp_%sHash_set(_t%d, _t%d, %s);\n", shn, td2, tk2, nil_v); }
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", td2);
    return 1;
  }
  TyKind skt = ty_hash_key(rt), svt = ty_hash_val(rt);
  TyKind dvt = ty_hash_val(dt);
  /* When the scope declares v as TY_POLY but the hash has typed (non-poly) values,
     box the value on assignment so the poly variable receives sp_RbVal. */
  Scope *pscope_tv = comp_scope_of(c, block);
  LocalVar *p0_lv_tv = p0_orig ? scope_local(pscope_tv, p0_orig) : NULL;
  TyKind p0_scope_ty = p0_lv_tv ? p0_lv_tv->type : TY_UNKNOWN;
  int needs_box_assign = (p0_scope_ty == TY_POLY && (!keys ? svt != TY_POLY : skt != TY_POLY));
  int ts = ++g_tmp, td = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
  emit_pre_rooted_recv(c, recv, rt, ts);  /* recv preludes flush to g_pre first */
  emit_indent(g_pre, g_indent); emit_ctype(c, dt, g_pre); buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td, dhn, td);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ts, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, skt, g_pre);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(g_pre, " _t%d = _t%d->keys[_t%d->order[_t%d]];\n", tk, ts, ts, ti);
  else
    buf_printf(g_pre, " _t%d = _t%d->order[_t%d];\n", tk, ts, ti);
  if (p0) {
    emit_indent(g_pre, g_indent + 1);
    if (keys) {
      if (needs_box_assign) {
        Buf bx; memset(&bx, 0, sizeof bx); char gk[64]; snprintf(gk, sizeof gk, "_t%d", tk);
        emit_boxed_text(c, skt, gk, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : ""); free(bx.p);
      }
      else buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tk);
    }
    else {
      if (needs_box_assign) {
        char gv[128]; snprintf(gv, sizeof gv, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk);
        Buf bx; memset(&bx, 0, sizeof bx);
        emit_boxed_text(c, svt, gv, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : ""); free(bx.p);
      }
      else buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d);\n", p0, shn, ts, tk);
    }
  }
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  Buf vb; memset(&vb, 0, sizeof vb);
  /* a `next <v>` answers for this pair: the body writes a slot and the
     set below reads it, where the bare `continue` of a tail read skipped
     the set and dropped the pair */
  if (fold_body_has_next(c, body)) {
    int tv = ++g_tmp;
    char dst[32]; snprintf(dst, sizeof dst, "_t%d", tv);
    emit_indent(g_pre, g_indent + 1); emit_ctype(c, bret, g_pre);
    buf_printf(g_pre, " %s = %s;\n", dst, bret == TY_POLY ? "sp_box_nil()" : default_value(bret));
    emit_block_value_into(c, block, dst, bret == TY_POLY, g_indent + 1);
    buf_puts(&vb, dst);
  }
  else {
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    int save = g_indent; g_indent++;
    emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  }
  TyKind dkt = ty_hash_key(dt);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sHash_set(_t%d, ", dhn, td);
  if (keys) {
    /* new key = block result; unbox if block returned poly but key type is typed */
    const char *vbp = vb.p ? vb.p : "0";
    if (dkt == TY_POLY && bret != TY_POLY && bret != TY_UNKNOWN) {
      /* PolyPoly destination: box the typed new key */
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vbp, &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else if (dkt == TY_STRING && (bret == TY_POLY || bret == TY_UNKNOWN))
      buf_printf(g_pre, "sp_poly_to_s(%s)", vbp);
    else if (dkt == TY_INT && (bret == TY_POLY || bret == TY_UNKNOWN))
      buf_printf(g_pre, "sp_poly_to_i(%s)", vbp);
    else
      buf_puts(g_pre, vbp);
    buf_puts(g_pre, ", ");
    if (rt == TY_POLY_POLY_HASH)
      buf_printf(g_pre, "_t%d->vals[_t%d->order[_t%d]]", ts, ts, ti);
    else if (dvt == TY_POLY && svt != TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); char g[64]; snprintf(g, sizeof g, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk); emit_boxed_text(c, svt, g, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_printf(g_pre, "sp_%sHash_get(_t%d, _t%d)", shn, ts, tk);
  }
  else {
    /* key carried over; new value = block result (box/unbox to match dest type) */
    /* When the block result forced a poly-valued dest hash (no concrete
       (key, result) variant, e.g. Int-key + Float value -> PolyPoly), the
       carried-over concrete key must be boxed for the poly-keyed set (#3173). */
    if (dkt == TY_POLY && skt != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx); char gk[64]; snprintf(gk, sizeof gk, "_t%d", tk);
      emit_boxed_text(c, skt, gk, &bx);
      buf_printf(g_pre, "%s, ", bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_printf(g_pre, "_t%d, ", tk);
    if (dvt == TY_POLY && bret != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bret, vb.p ? vb.p : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else if (dvt == TY_STRING && (bret == TY_POLY || bret == TY_UNKNOWN)) {
      buf_printf(g_pre, "sp_poly_to_s(%s)", vb.p ? vb.p : "sp_box_nil()");
    }
    else if (dvt == TY_INT && (bret == TY_POLY || bret == TY_UNKNOWN)) {
      buf_printf(g_pre, "sp_poly_to_i(%s)", vb.p ? vb.p : "sp_box_nil()");
    }
    else buf_puts(g_pre, vb.p ? vb.p : "0");
  }
  buf_puts(g_pre, ");\n"); free(vb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", td);
  return 1;
}

/* (lo..hi).bsearch { |x| cond } in find-minimum mode: binary search for the
   smallest member where the block is truthy, or nil (the SP_INT_NIL sentinel)
   when none qualifies. Loop in the statement prelude; value is the result.
   Returns 1 if handled. */
int emit_bsearch_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "bsearch")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* Float-range bsearch: a range literal with a float bound cannot ride the
     sp_int sp_Range, so bisect the float interval directly (CRuby find-minimum
     over the reals, a fixed ~100-iteration halving to double precision). The
     block is truthy at/after the answer, falsy before. */
  if (recv >= 0 && ((comp_ntype(c, recv) == TY_RANGE && range_float_begin(c, recv)) ||
                    comp_ntype(c, recv) == TY_FLOAT_RANGE)) {
    int rn9 = unwrap_parens(c, recv);
    if (rn9 >= 0 && nt_type(nt, rn9) && !sp_streq(nt_type(nt, rn9), "RangeNode"))
      rn9 = local_sole_range_node(c, rn9);
    int rleft = rn9 >= 0 ? nt_ref(nt, rn9, "left") : -1;
    int rright = rn9 >= 0 ? nt_ref(nt, rn9, "right") : -1;
    if (comp_ntype(c, recv) == TY_FLOAT_RANGE && (rleft < 0 || rright < 0)) return 0;  /* variable float range: not yet */
    TyKind blt9 = rleft >= 0 ? infer_type(c, rleft) : TY_NIL;
    TyKind brt9 = rright >= 0 ? infer_type(c, rright) : TY_NIL;
    if ((blt9 == TY_INT || blt9 == TY_FLOAT) && (brt9 == TY_INT || brt9 == TY_FLOAT)) {
      int flo = ++g_tmp, fhi = ++g_tmp, fres = ++g_tmp, fi = ++g_tmp, fmid = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "double _t%d = ", flo); emit_float_expr(c, rleft, g_pre); buf_puts(g_pre, ";\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "double _t%d = ", fhi); emit_float_expr(c, rright, g_pre); buf_puts(g_pre, ";\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_float _t%d = sp_float_nil(); int _t%d;\n", fres, fi);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (_t%d = 0; _t%d < 100; _t%d++) {\n", fi, fi, fi);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "double _t%d = _t%d + (_t%d - _t%d) / 2.0;\n", fmid, flo, fhi, flo);
      if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, fmid); }
      IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
      int save = g_indent; g_indent++;
      Buf cb; memset(&cb, 0, sizeof cb);
      TyKind bt = emit_iter_step_tail(c, &st, &cb); g_indent = save;
      if (bt == TY_INT || bt == TY_FLOAT) {
        /* find-any (CRuby: a Numeric block is `target <=> x`): 0 found,
           positive means the target sorts after the probe, negative before.
           No exact 0 within the bisection is a miss (nil) (#3067). */
        int fcmp = ++g_tmp;
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "double _t%d = (double)(%s);\n", fcmp, cb.p ? cb.p : "0");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (_t%d == 0.0) { _t%d = _t%d; break; }\n", fcmp, fres, fmid);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "else if (_t%d > 0.0) { _t%d = _t%d; }\n", fcmp, flo, fmid);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "else { _t%d = _t%d; }\n", fhi, fmid);
      }
      else if (bt == TY_POLY) {
        /* mixed block: an Integer/Float value is find-any, any other truthy
           value is find-minimum, nil/false searches up (#3067) */
        int fv = ++g_tmp;
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", fv, cb.p ? cb.p : "sp_box_nil()");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (_t%d.tag == SP_TAG_INT || _t%d.tag == SP_TAG_FLT) {\n", fv, fv);
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "double _c = sp_poly_to_f(_t%d);\n", fv);
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "if (_c == 0.0) { _t%d = _t%d; break; }\n", fres, fmid);
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "else if (_c > 0.0) { _t%d = _t%d; }\n", flo, fmid);
        emit_indent(g_pre, g_indent + 2);
        buf_printf(g_pre, "else { _t%d = _t%d; }\n", fhi, fmid);
        emit_indent(g_pre, g_indent + 1);
        buf_puts(g_pre, "}\n");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "else if (sp_poly_truthy(_t%d)) { _t%d = _t%d; _t%d = _t%d; }\n",
                   fv, fres, fmid, fhi, fmid);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "else { _t%d = _t%d; }\n", flo, fmid);
      }
      else {
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = _t%d; }\n",
                   cb.p ? cb.p : "0", fres, fmid, fhi, fmid);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "else { _t%d = _t%d; }\n", flo, fmid);
      }
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", fres);
      return 1;
    }
  }
  if (recv < 0 || comp_ntype(c, recv) != TY_RANGE) return 0;
  int tr = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
  /* an Integer-typed block is CRuby's find-any mode (0 found, positive means
     the target sorts after the probe, negative before); a truthy block is
     find-minimum mode */
  int find_any = comp_ntype(c, bb[bn - 1]) == TY_INT;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_Range _t%d = ", tr); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = _t%d.first;\n", tlo, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = _t%d.last - _t%d.excl;\n", thi, tr, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = SP_INT_NIL;\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
  /* A beginless or endless range spans most of sp_int, so the width and the
     step past the probe are taken unsigned or guarded: `hi - lo` and `mid +
     1` overflowed, which is undefined, and gcc compiled the loop into one
     that never ended. */
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_int _t%d = _t%d + (sp_int)(((uint64_t)_t%d - (uint64_t)_t%d) >> 1);\n", tmid, tlo, thi, tlo);
  char right[128], left[128];
  snprintf(right, sizeof right, "{ if (_t%d == _t%d) break; _t%d = _t%d + 1; }", tmid, thi, tlo, tmid);
  snprintf(left, sizeof left, "{ if (_t%d == _t%d) break; _t%d = _t%d - 1; }", tmid, tlo, thi, tmid);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tmid); }
  IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
  int save = g_indent; g_indent++;
  Buf cb; memset(&cb, 0, sizeof cb); emit_iter_step_tail(c, &st, &cb); g_indent = save;
  if (find_any) {
    int tcmp = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_int _t%d = %s;\n", tcmp, cb.p ? cb.p : "0");
    /* an Integer block that also answers nil is the combined dispatch: nil
       searches right (see the Array form) */
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d == SP_INT_NIL) %s\n", tcmp, right);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "else if (_t%d == 0) { _t%d = _t%d; break; }\n", tcmp, tres, tmid);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "else if (_t%d > 0) %s\n", tcmp, right);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "else %s\n", left);
  }
  else if (comp_ntype(c, bb[bn - 1]) == TY_POLY) {
    /* a mixed block (int-or-nil ternary) is CRuby's combined dispatch: an
       Integer is find-any (0 found, positive right, negative left), any
       other truthy value is find-minimum, nil/false searches right */
    int tv = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tv, cb.p ? cb.p : "sp_box_nil()");
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d.tag == SP_TAG_INT) {\n", tv);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "if (_t%d.v.i == 0) { _t%d = _t%d; break; }\n", tv, tres, tmid);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "else if (_t%d.v.i > 0) %s\n", tv, right);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "else %s\n", left);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "else if (sp_poly_truthy(_t%d)) { _t%d = _t%d; %s }\n", tv, tres, tmid, left);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else %s\n", right);
  }
  else {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (%s) { _t%d = _t%d; %s }\n", cb.p ? cb.p : "0", tres, tmid, left);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else %s\n", right);
  }
  free(cb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* array.max_by / min_by { |x| key } -> the element with the largest/smallest
   (int/float) key. Loop in the statement prelude; value is the best element. */
/* Emit `src` (a poly sp_RbVal C-expression) coerced to scalar type `dst`. */
static void flatmap_coerce_from_poly(TyKind dst, const char *src, Buf *out) {
  switch (dst) {
  case TY_INT: case TY_BOOL: buf_printf(out, "sp_poly_to_i(%s)", src); break;
  case TY_FLOAT: buf_printf(out, "sp_poly_to_f(%s)", src); break;
  /* a String / Symbol param unboxes the field directly (matching emit_unbox_text);
     without this a `const char *`/`sp_sym` slot took a raw sp_RbVal (#2929) */
  case TY_STRING: buf_printf(out, "sp_poly_unbox_s(%s)", src); break;
  case TY_SYMBOL: buf_printf(out, "(sp_sym)(%s).v.i", src); break;
  default: buf_puts(out, src); break;  /* poly (or other): pass through */
  }
}

/* CRuby proc auto-splat: bind each of the block's params to a positional element
   of the poly sub-array held in temp `_t<elem_temp>` (an sp_RbVal), coerced to
   each param's pinned type. Used where a multi-param block iterates a poly array
   whose elements are themselves arrays (map/select/reject/sort_by). */
void emit_autosplat_params(Compiler *c, int block, int np, int elem_temp, int indent) {
  Scope *asc = comp_scope_of(c, block);
  for (int pj = 0; pj < np; pj++) {
    const char *pn = block_param_name(c, block, pj); if (!pn) continue;
    const char *pnr = rename_local(pn);
    LocalVar *lvp = asc ? scope_local(asc, pn) : NULL;
    /* an unused param has no C declaration; the element read is pure (#2734) */
    if (!lvp || lvp->type == TY_UNKNOWN) continue;
    TyKind pty = lvp ? lvp->type : TY_POLY;
    char src[96]; snprintf(src, sizeof src, "sp_poly_massign_get(_t%d, %d)", elem_temp, pj);
    emit_indent(g_pre, indent); buf_printf(g_pre, "lv_%s = ", pnr);
    Buf cv; memset(&cv, 0, sizeof cv); flatmap_coerce_from_poly(pty, src, &cv);
    buf_puts(g_pre, cv.p ? cv.p : src); free(cv.p); buf_puts(g_pre, ";\n");
  }
}

/* The block of an emitter that yields one Array per step (product's tuple)
   auto-splats it across a block taking only leading requireds (`|q, r|`,
   `|q, |`), as CRuby's does. Binds those params from the boxed Array
   `tuple_src` (a pure, rooted expression) into `out` and answers 1. Answers 0
   for a block the caller binds whole (one plain parameter, a destructuring
   one, or none). A rest, an optional or a post-required parameter takes
   CRuby's full proc distribution, which desugar_builtin_iter_block_shapes
   binds before this runs; one that reaches here is refused. */
int emit_tuple_block_params(Compiler *c, int id, int block, const char *tuple_src, Buf *out) {
  if (block_opt_name(c, block, 0) || block_post_name(c, block, 0) ||
      (block_rest_marker(c, block) && !block_lead_only(c, block)))
    unsupported_feature(c, id, "a block with a rest, optional or post parameter on this Array iterator");
  if (!block_lead_only(c, block)) return 0;
  Scope *asc = comp_scope_of(c, block);
  for (int pj = 0; block_param_name(c, block, pj); pj++) {
    const char *pn = block_param_name(c, block, pj);
    LocalVar *lvp = asc ? scope_local(asc, pn) : NULL;
    /* an unused param has no C declaration; the element read is pure (#2734) */
    if (!lvp || lvp->type == TY_UNKNOWN) continue;
    char src[160]; snprintf(src, sizeof src, "sp_poly_massign_get(%s, %d)", tuple_src, pj);
    buf_printf(out, " lv_%s = ", rename_local(pn));
    Buf cv; memset(&cv, 0, sizeof cv); flatmap_coerce_from_poly(lvp->type, src, &cv);
    buf_puts(out, cv.p ? cv.p : src); free(cv.p); buf_puts(out, ";");
  }
  return 1;
}

/* Shared entry for element-loop emitters: when a flat multi-param block runs
   over a poly array, its element (itself an array) auto-splats across the
   params; bind them from `elem_src` and return the param count. Returns 0 when
   the ordinary single-param bind applies (any other receiver/param shape). */
int emit_iter_autosplat(Compiler *c, int block, TyKind rt, const char *elem_src, int indent) {
  if (rt != TY_POLY_ARRAY) return 0;
  if (block_param_is_multi(c, block, 0)) return 0;
  int np = 0; while (block_param_name(c, block, np)) np++;
  if (np < 2) return 0;
  int te = ++g_tmp;
  emit_indent(g_pre, indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", te, elem_src, te);
  emit_autosplat_params(c, block, np, te, indent);
  return np;
}

/* Whether `node` is a call the resolution gate will lower to its raising
   token: a settled receiver type, and no user class owns the name. Used to
   tell "the block names a method that does not exist" (compile it, let it
   raise) apart from "codegen has no shape for this" (decline). */
int block_tail_is_unresolved(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, node, "name");
  int r = nt_ref(nt, node, "receiver");
  if (!nm || r < 0) return 0;
  TyKind rt2 = comp_ntype(c, r);
  if (rt2 == TY_UNKNOWN || rt2 == TY_POLY) return 0;
  return !diag_user_defines(c, nm);
}

/* poly `uniq`/`uniq!` with a block, as an expression: the receiver boxes an
   array; keep the first element for each distinct block-key value (compared with
   sp_poly_eq), and for the bang form write the survivors back in place. Yields
   the (boxed) array. Returns 1 if handled. */
int emit_poly_uniq_block(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "uniq") && !sp_streq(name, "uniq!"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (recv < 0 || block < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc != 0) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* a block of any other shape than plain requireds, over elements known
     only at run time, binds each by the proc distribution */
  int gather = (rt == TY_POLY_ARRAY || rt == TY_POLY) && block_binds_gathered(c, block);
  /* a block that binds nothing (`uniq { 1 }`) keys every element the same */
  int bare = !p0 && !gather;
  if ((bare && rt != TY_POLY_ARRAY && rt != TY_POLY) || bn < 1) return 0;
  int bang = sp_streq(name, "uniq!");

  /* Typed or poly array receiver (sp_<K>Array): dedup keeping the same element
     type, using the block's return value as the uniqueness key. */
  const char *rk = array_kind(rt);
  if (!rk && rt == TY_POLY_ARRAY) rk = "Poly";
  if (rk) {
    TyKind et = ty_array_elem(rt);
    /* If the block param was widened to a wider type than the receiver's
       element type (e.g. the same name is poly in another block in this scope),
       its lv_ is declared at the wider C type. Shadow it with an et-typed local
       inside a fresh C block and re-infer the body, so the typed-array get
       assigns into a matching lvalue. */
    Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
    LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
    TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
    int trecv = ++g_tmp, tseen = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
    /* a multi-param block over a poly array (a Hash's pairs) splats each
       element across its params; the survivor pushed is the element */
    char es[64]; snprintf(es, sizeof es, "sp_%sArray_get(_t%d, _t%d)", rk, trecv, ti);
    int np = 0; while (block_param_name(c, block, np)) np++;
    int splat = gather || bare || (rt == TY_POLY_ARRAY && np >= 2 && !block_param_is_multi(c, block, 0));
    int use_shadow = !splat && clv0 && clv0->type != et && et != TY_UNKNOWN;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : "NULL"); free(rb.p);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tseen, tseen);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", rk, tres, rk, tres);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    int din = g_indent + 1;
    if (use_shadow) {
      clv0->type = et;
      for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
      emit_indent(g_pre, din); buf_puts(g_pre, "{\n"); din++;
      emit_indent(g_pre, din); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, rk, trecv, ti);
    }
    else {
      /* the element is kept as it was before the block ran, which may
         replace the receiver's entry */
      char sel[24] = "";
      if (splat) {
        int tel = ++g_tmp;
        snprintf(sel, sizeof sel, "_t%d", tel);
        emit_indent(g_pre, din); buf_printf(g_pre, "sp_RbVal %s = %s; SP_GC_ROOT_RBVAL(%s);\n", sel, es, sel);
      }
      if (gather) {
        char vals[64]; snprintf(vals, sizeof vals, "sp_yielded_args(0, %s)", sel);
        emit_boxed_step_binds(c, block, vals, g_pre, din, 0);
        snprintf(es, sizeof es, "%s", sel);
      }
      else if (bare) snprintf(es, sizeof es, "%s", sel);
      else if (!splat || !emit_iter_autosplat(c, block, rt, sel, din)) {
        splat = 0;
        emit_indent(g_pre, din); buf_printf(g_pre, "lv_%s = %s;\n", p0, es);
      }
      else snprintf(es, sizeof es, "%s", sel);
    }
    IterStep st; emit_iter_step_open(c, block, 1, din, &st);
    int tkey = ++g_tmp, tdup = ++g_tmp, tj = ++g_tmp;
    int save = g_indent; g_indent = din;
    Buf kb; memset(&kb, 0, sizeof kb); emit_iter_step_tail(c, &st, &kb); g_indent = save;
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
    emit_indent(g_pre, din);
    buf_printf(g_pre, "int _t%d = 0; for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) if (sp_poly_eq(_t%d->data[_t%d], _t%d)) { _t%d = 1; break; }\n",
               tdup, tj, tj, tseen, tj, tseen, tj, tkey, tdup);
    emit_indent(g_pre, din);
    if (splat) buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_%sArray_push(_t%d, %s); }\n", tdup, tseen, tkey, rk, tres, es);
    else buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_%sArray_push(_t%d, lv_%s); }\n", tdup, tseen, tkey, rk, tres, p0);
    if (use_shadow) { din--; emit_indent(g_pre, din); buf_puts(g_pre, "}\n"); }
    if (clv0) clv0->type = csaved0;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    if (bang) {
      int tm = ++g_tmp, tn = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_int _t%d = _t%d->len; _t%d->len = 0; for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
                 tn, tres, trecv, tm, tm, tn, tm, rk, trecv, rk, tres, tm);
      buf_printf(b, "_t%d", trecv);
    }
    else {
      buf_printf(b, "_t%d", tres);
    }
    return 1;
  }

  if (rt != TY_POLY) return 0;
  int trecv = ++g_tmp, tarr = ++g_tmp, tseen = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", trecv, rb.p ? rb.p : "sp_box_nil()", trecv); free(rb.p);
  /* uniq hands its block every value one step of an Enumerator yielded,
     which the walk below reads packed as one item: a lone `|x|` takes the
     first of them, any other shape but plain requireds all of them */
  int lone = !gather && p0 && !block_param_name(c, block, 1) && !block_rest_marker(c, block) &&
             !block_param_is_multi(c, block, 0);
  int tpair = 0;
  if (gather || lone) {
    tpair = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = sp_poly_yields_pair(_t%d);\n", tpair, trecv);
  }
  emit_indent(g_pre, g_indent);
  if (bang) buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_array_recv(_t%d, \"uniq!\", 1); SP_GC_ROOT(_t%d);\n", tarr, trecv, tarr);
  else buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"uniq\"); SP_GC_ROOT(_t%d);\n", tarr, trecv, tarr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tseen, tseen);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tarr, ti);
  char es[64]; snprintf(es, sizeof es, "_t%d->data[_t%d]", tarr, ti);
  char vals[96]; snprintf(vals, sizeof vals, "sp_yielded_args(_t%d, %s)", tpair, es);
  if (gather) emit_boxed_step_binds(c, block, vals, g_pre, g_indent + 1, 0);
  else if (lone) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_yielded_first(_t%d, %s);\n", p0, tpair, es);
  }
  else if (p0 && !emit_iter_autosplat(c, block, TY_POLY_ARRAY, es, g_indent + 1)) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = %s;\n", p0, es);
  }
  int save = g_indent; g_indent++;
  IterStep st; emit_iter_step_open(c, block, 1, g_indent, &st);
  int tkey = ++g_tmp, tdup = ++g_tmp, tj = ++g_tmp;
  Buf kb; memset(&kb, 0, sizeof kb); emit_iter_step_tail(c, &st, &kb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "int _t%d = 0; for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) if (sp_poly_eq(_t%d->data[_t%d], _t%d)) { _t%d = 1; break; }\n",
             tdup, tj, tj, tseen, tj, tseen, tj, tkey, tdup);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_PolyArray_push(_t%d, %s); }\n", tdup, tseen, tkey, tres, es);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (bang) {
    int tm = ++g_tmp, tn = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_int _t%d = _t%d->len; _t%d->len = 0; for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) sp_PolyArray_push(_t%d, _t%d->data[_t%d]);\n",
               tn, tarr, tarr, tm, tm, tres, tm, tarr, tres, tm);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_poly_arr_writeback(_t%d, _t%d);\n", trecv, tarr);
    buf_printf(b, "(_t%d->len == _t%d ? sp_box_nil() : _t%d)", tarr, tn, trecv);
  }
  else buf_printf(b, "sp_box_poly_array(_t%d)", tres);
  return 1;
}

/* "str".gsub(/re/) { |m| repl } / sub as an expression: iterate the matches
   of a regex literal, binding the block param to each matched substring and
   appending its return value as the replacement. sub replaces only the first
   match. Anchored patterns (^/$) are matched per-remainder, so this targets
   the unanchored block forms. Returns 1 if handled. */
int emit_gsub_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "gsub") && !sp_streq(name, "sub"))) return 0;
  int once = sp_streq(name, "sub");
  int recv = nt_ref(nt, id, "receiver");
  TyKind recv_ty = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (recv < 0 || (recv_ty != TY_STRING && recv_ty != TY_POLY)) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc != 1) return 0;
  int reidx = re_lit_index(c, argv[0]);
  int strpat = 0, dynre = 0;
  if (reidx < 0) {
    /* a Regexp VALUE (a parameter, a reader, a local): the compiled pattern
       itself at C level, hoisted once and scanned with like a literal's.
       Only a literal was admitted before, so activesupport's
       `word.gsub!(inflections.acronyms_underscore_regex) { ... }` fell to
       a static NoMethodError. */
    if (comp_ntype(c, argv[0]) == TY_REGEX) dynre = 1;
    /* a plain-String pattern: the same scan loop, matching by strstr (an
       empty needle degenerates to the zero-width branch, like CRuby) */
    else if (comp_ntype(c, argv[0]) == TY_STRING) strpat = 1;
    else return 0;
  }
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int ts = ++g_tmp, tpos = ++g_tmp, tslen = ++g_tmp, tout = ++g_tmp,
      tm = ++g_tmp, tms = ++g_tmp, tme = ++g_tmp;
  /* poly values reaching here are strings, like the blockless poly gsub/sub
     arm in codegen_call_recv.c -- unbox through sp_poly_to_s to get the same
     `const char *` the typed String receiver emits directly. */
  Buf rb; memset(&rb, 0, sizeof rb);
  if (recv_ty == TY_POLY) buf_puts(&rb, "sp_poly_to_s(");
  emit_expr(c, recv, &rb);
  if (recv_ty == TY_POLY) buf_puts(&rb, ")");
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  /* The SUBJECT is walked across every turn of the loop below, and every turn
     allocates: the substring before the match, the block's own value, the
     append into the accumulator. A receiver built by this same statement --
     `text.gsub(re) { ... }` hoists a fresh concat -- has nothing else holding
     it, so a collection landing in one of those freed it mid-walk and
     sp_re_match_at read the freed bytes. The accumulator below has been rooted
     since it was written; the subject never was (#4369's rule, at the one site
     that walks a STRING rather than an array). */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_STR(_t%d);\n", ts);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tpos);
  /* the SUBJECT's length bounds the scan, and strlen stops at an embedded
     NUL: `"a\0b".gsub(/./m) { }` walked one character and stopped. */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = (sp_int)sp_str_byte_len(_t%d);\n", tslen, ts);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"\"); SP_GC_ROOT(_t%d);\n", tout, tout);
  int tnd = 0, tnl = 0, tre = 0;
  if (dynre) {
    tre = ++g_tmp;
    Buf pb; memset(&pb, 0, sizeof pb); emit_expr(c, argv[0], &pb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = %s;\n", tre, pb.p ? pb.p : "NULL");
    free(pb.p);
  }
  if (strpat) {
    tnd = ++g_tmp; tnl = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab); emit_str_expr(c, argv[0], &ab);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", tnd, ab.p ? ab.p : "\"\"");
    free(ab.p);
    /* the needle is read by every strstr below, for the same reason */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT_STR(_t%d);\n", tnd);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_int _t%d = (sp_int)sp_str_byte_len(_t%d);\n", tnl, tnd);
  }
  /* `$~` is each turn's match inside the block and the last one after the
     call, or nil when nothing matched: the registers start cleared, every
     match sets them, and the miss that ends the loop leaves them alone
     (sp_re_match_next). Only a program that reads them pays for it. */
  const char *re_next = g_reads_match_regs ? "sp_re_match_next" : "sp_re_match_at";
  if (g_reads_match_regs) { emit_indent(g_pre, g_indent); buf_puts(g_pre, "sp_re_clear_last_match();\n"); }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tpos, tslen);
  if (strpat) {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_int _t%d = ({ const char *_h = strstr(_t%d + _t%d, _t%d); _h ? (sp_int)(_h - (_t%d + _t%d)) : (sp_int)-1; });\n",
               tm, ts, tpos, tnd, ts, tpos);
  }
  else if (dynre) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = %s(_t%d, _t%d, _t%d);\n", tm, re_next, tre, ts, tpos);
  }
  else {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = %s(sp_re_pat_%d, _t%d, _t%d);\n", tm, re_next, reidx, ts, tpos);
  }
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d < 0) { sp_String_append_bin(_t%d, _t%d + _t%d); break; }\n", tm, tout, ts, tpos);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "sp_re_sub_matched = 1;\n");   /* the bang forms' nil contract */
  if (strpat) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = _t%d;\n", tms, tm);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = _t%d + _t%d;\n", tme, tm, tnl);
    if (g_reads_match_regs) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_re_set_lit_match(_t%d, _t%d + _t%d, _t%d + _t%d);\n", ts, tpos, tms, tpos, tme);
    }
  }
  else {
    /* sp_re_match_at leaves sp_re_caps full-string-relative; the scan loop works
       in offsets from `str + pos`, so rebase both onto pos (#2910). */
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = sp_re_caps[0] - _t%d;\n", tms, tpos);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_int _t%d = sp_re_caps[1] - _t%d;\n", tme, tpos);
  }
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append_bin(_t%d, sp_str_substr(_t%d + _t%d, 0, _t%d));\n", tout, ts, tpos, tms);
  if (p0) {
    Scope *ps = comp_scope_of(c, block);
    LocalVar *plv = ps ? scope_local(ps, block_param_name(c, block, 0)) : NULL;
    int box = plv && plv->type == TY_POLY;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = %ssp_str_substr(_t%d + _t%d, _t%d, _t%d - _t%d)%s;\n",
               p0, box ? "sp_box_str(" : "", ts, tpos, tms, tme, tms, box ? ")" : "");
  }
  IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
  int save = g_indent; g_indent++;
  /* CRuby stringifies a non-string block value (gsub { 2 } -> "2"): box a
     concretely non-string result and render it through sp_poly_to_s. */
  TyKind gvt = st.slot ? st.slot_ty : comp_ntype(c, bb[bn - 1]);
  Buf vb; memset(&vb, 0, sizeof vb);
  if (gvt == TY_POLY) { buf_puts(&vb, "sp_poly_to_s("); emit_iter_step_tail(c, &st, &vb); buf_puts(&vb, ")"); }
  else if (gvt != TY_STRING && gvt != TY_UNKNOWN && st.slot) {
    Buf sb; memset(&sb, 0, sizeof sb); emit_iter_step_tail(c, &st, &sb);
    buf_puts(&vb, "sp_poly_to_s("); emit_boxed_text(c, gvt, sb.p, &vb); buf_puts(&vb, ")"); free(sb.p);
  }
  else if (gvt != TY_STRING && gvt != TY_UNKNOWN) {
    buf_puts(&vb, "sp_poly_to_s("); emit_boxed(c, bb[bn - 1], &vb); buf_puts(&vb, ")");
  }
  else emit_iter_step_tail(c, &st, &vb);
  g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append_bin(_t%d, %s);\n", tout, vb.p ? vb.p : "\"\""); free(vb.p);
  if (once) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append_bin(_t%d, _t%d + _t%d + _t%d); break;\n", tout, ts, tpos, tme);
  }
  else {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d == _t%d) { if (_t%d + _t%d < _t%d) sp_String_append_bin(_t%d, sp_str_substr(_t%d + _t%d, _t%d, 1)); _t%d += _t%d + 1; }\n",
               tme, tms, tpos, tme, tslen, tout, ts, tpos, tme, tpos, tme);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d += _t%d; }\n", tpos, tme);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d->data", tout);
  return 1;
}

/* poly_recv.sum([init]) { |x| f(x) } as an expression: the receiver is a value
   only known to be an Array at runtime (a group_by bucket, a `case`-merged
   local), so coerce it to a poly array and fold the (boxed) block result with
   sp_poly_add, starting from the initial value or Integer 0. The block param is
   pinned to poly so a user-method call on the element dispatches dynamically
   (`bucket.sum(&:value)`). (#2872) */
int emit_sum_block_poly_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "sum")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* concrete typed arrays keep the tuned integer/float/string path below */
  if (comp_ntype(c, recv) != TY_POLY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  if (block_param_is_multi(c, block, 0)) return 0;
  const char *p0 = block_param_name(c, block, 0);
  const char *p0r = p0 ? rename_local(p0) : NULL;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc > 1) return 0;
  /* pin the block param to poly before typing the body, so `x.<user method>`
     resolves to the runtime dynamic-dispatch path rather than a static one. */
  Scope *bsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *blv = (bsc && p0) ? scope_local(bsc, p0) : NULL;
  TyKind saved = blv ? blv->type : TY_UNKNOWN;
  if (blv) blv->type = TY_POLY;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_poly_to_a_arr(", ta);
  emit_expr(c, recv, b);
  buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = _t%d->len; sp_RbVal _t%d = ",
             ta, tn, ta, tacc);
  if (argc == 1) emit_boxed(c, argv[0], b);
  else buf_puts(b, "sp_box_int(0)");
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) { ",
             tacc, ti, ti, tn, ti);
  /* a block of any other shape than plain requireds binds the element by
     the proc distribution */
  if (block_binds_gathered(c, block)) {
    if (p0r) buf_printf(b, "sp_RbVal lv_%s; ", p0r);
    char vals[96]; snprintf(vals, sizeof vals, "sp_yielded_args(0, _t%d->data[_t%d])", ta, ti);
    emit_boxed_step_binds(c, block, vals, b, 0, 1);
  }
  else if (p0r) buf_printf(b, "sp_RbVal lv_%s = _t%d->data[_t%d]; ", p0r, ta, ti);
  /* A two-param block over a boxed HASH walks [key, value] pairs, so the
     element auto-splats across the params -- binding only the first left the
     second nil, and `h.sum { |k, v| v }` added nil to the accumulator. */
  { const char *p1 = block_param_name(c, block, 1);
    if (p0r && p1 && !block_binds_gathered(c, block)) {
      const char *p1r = rename_local(p1);
      buf_printf(b, "sp_RbVal lv_%s = sp_poly_massign_get(lv_%s, 1LL); "
                    "lv_%s = sp_poly_massign_get(lv_%s, 0LL); ", p1r, p0r, p0r, p0r);
    } }
  {
    Buf inner; memset(&inner, 0, sizeof inner);
    Buf valb; memset(&valb, 0, sizeof valb);
    Buf *saved_pre = g_pre; g_pre = &inner;
    /* the whole body: its leading statements ran nowhere */
    { int svlm = g_line_map; g_line_map = 0;
      IterStep st; emit_iter_step_open(c, block, 1, 0, &st);
      emit_iter_step_tail(c, &st, &valb);
      g_line_map = svlm; }
    g_pre = saved_pre;
    if (inner.p) buf_puts(b, inner.p);
    buf_printf(b, "_t%d = sp_poly_add(_t%d, %s); }", tacc, tacc, valb.p ? valb.p : "sp_box_nil()");
    free(inner.p); free(valb.p);
  }
  buf_printf(b, " _t%d; })", tacc);
  if (blv) blv->type = saved;
  return 1;
}
/* array.sum([init]) { |x| f(x) } as an expression: sum the block's result
   over every element. Returns 1 if handled. */
int emit_sum_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "sum")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* An empty block body (`sum {}`) answers nil for every element, which the
     boxed accumulation adds: an empty receiver never runs it and keeps the 0
     it starts from, and a non-empty one raises on the first nil, as CRuby's
     "nil can't be coerced" does (#3991). */
  TyKind acct = bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_POLY;
  if (bn > 0 && acct == TY_NIL) acct = TY_POLY;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc > 1) return 0;
  /* A String accumulator needs a String seed -- without one the sum starts at
     the Integer 0 and CRuby raises "String can't be coerced into Integer". */
  if (acct == TY_STRING && !(argc == 1 && argv && comp_ntype(c, argv[0]) == TY_STRING))
    acct = TY_POLY;
  /* Every other block value accumulates BOXED rather than bailing out. The
     answer CRuby gives for `[1, 2].sum { true }` is a TypeError from `0 + true`,
     and only sp_poly_add can raise it: declining the fold left the call to the
     generic dispatch, which answered NoMethodError instead (#4327). TY_NIL was
     already routed this way for exactly that reason. */
  if (acct != TY_INT && acct != TY_FLOAT && acct != TY_STRING) acct = TY_POLY;
  /* So does a block with a `next` or a `redo` of its own, whose step answers
     through a slot (emit_iter_step_value): a bare `next` answers nil, which
     only the boxed add refuses as CRuby does. */
  if (iter_step_needs_frame(c, block)) acct = TY_POLY;
  /* A poly block value (e.g. a product of values read out of poly containers,
     as in a range sum redispatched over an int array) accumulates into a boxed
     sp_RbVal via sp_poly_add, like the poly-receiver sum path. An empty range
     leaves the boxed init (0), so `(1...1).sum { ... }` is a well-typed 0. */
  if (acct == TY_POLY) {
    int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = sp_%sArray_length(_t%d); sp_RbVal _t%d = ",
               ta, tn, k, ta, tacc);
    if (argc == 1) emit_boxed(c, argv[0], b); else buf_puts(b, "sp_box_int(0)");
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) { ",
               tacc, ti, ti, tn, ti);
    /* A 2+-param block over an array of sub-arrays auto-splats each element
       into the params (`sum { |v, i| v }` over [v, i] pairs); a single param
       binds the whole element. Each param is gated on liveness. */
    {
      int nps = 0; while (block_param_name(c, block, nps)) nps++;
      if (nps >= 2 && !block_param_is_multi(c, block, 0)) {
        int te = ++g_tmp;
        buf_printf(b, "sp_RbVal _t%d = sp_%sArray_get(_t%d, _t%d); ", te, k, ta, ti);
        Scope *bsc = comp_scope_of(c, block);
        for (int pj = 0; pj < nps; pj++) {
          const char *pn = block_param_name(c, block, pj);
          LocalVar *plv = (pn && bsc) ? scope_local(bsc, pn) : NULL;
          if (plv) buf_printf(b, "lv_%s = sp_poly_massign_get(_t%d, %d); ", rename_local(pn), te, pj);
        }
      }
      else if (p0) buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d); ", p0, k, ta, ti);
    }
    {
      Buf inner; memset(&inner, 0, sizeof inner);
      Buf valb; memset(&valb, 0, sizeof valb);
      Buf *saved_pre = g_pre; g_pre = &inner;
      { int svlm = g_line_map; g_line_map = 0;  /* a #line directive mid stmt-expr is a stray '#' */
        if (bn > 0) {
          IterStep st; emit_iter_step_open(c, block, 1, 0, &st);
          emit_iter_step_tail(c, &st, &valb);
        }
        else buf_puts(&valb, "sp_box_nil()");
        g_line_map = svlm; }
      g_pre = saved_pre;
      if (inner.p) buf_puts(b, inner.p);
      buf_printf(b, "_t%d = sp_poly_add(_t%d, %s); }", tacc, tacc, valb.p ? valb.p : "sp_box_nil()");
      free(inner.p); free(valb.p);
    }
    /* The call's own type may be a scalar the inference settled on (an int
       array's `sum {}` is an Integer where it answers at all), so hand back
       what the caller's slot holds; a nil term raises inside the loop before
       this is reached. */
    TyKind sret = comp_ntype(c, id);
    if (sret == TY_INT || sret == TY_FLOAT || sret == TY_STRING) {
      char accsrc[32]; snprintf(accsrc, sizeof accsrc, "_t%d", tacc);
      buf_puts(b, " ");
      emit_unbox_text(c, sret, accsrc, b);
      buf_puts(b, "; })");
    }
    else buf_printf(b, " _t%d; })", tacc);
    return 1;
  }
  /* a float initial value promotes the whole sum to Float, even when the block
     yields integers (matches analyze and CRuby): accumulate in floating point
     rather than truncating the init into an integer accumulator. */
  if (argc == 1 && argv && comp_ntype(c, argv[0]) == TY_FLOAT) acct = TY_FLOAT;
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  /* Float accumulation uses Kahan-Babuska-Neumaier compensation (matches
     CRuby's Array#sum), so it needs a running compensation temp plus
     per-iteration x/t temps. Integer sums use none of them. */
  int tc = -1, tx = -1, tt = -1;
  if (acct == TY_FLOAT) { tc = ++g_tmp; tx = ++g_tmp; tt = ++g_tmp; }
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  /* rooted the way the poly-accumulator arm above roots its receiver: the
     element is taken out of this temp on every turn and the block allocates
     in between */
  buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = sp_%sArray_length(_t%d); ", ta, tn, k, ta);
  emit_ctype(c, acct, b); buf_printf(b, " _t%d = ", tacc);
  if (argc == 1) {
    TyKind init_t = comp_ntype(c, argv[0]);
    if (acct == TY_FLOAT && init_t == TY_INT) {
      buf_puts(b, "(sp_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (acct == TY_FLOAT && init_t == TY_POLY) {
      buf_puts(b, "sp_poly_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (acct == TY_INT && init_t == TY_POLY) {
      buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else {
      emit_expr(c, argv[0], b);
    }
  }
  else {
    buf_puts(b, acct == TY_FLOAT ? "0.0" : "0");
  }
  if (acct == TY_FLOAT) buf_printf(b, "; sp_float _t%d = 0.0", tc);
  /* A block value that can be its slot's nil is tested before it is added
     (below). The failure is the accumulator's own: `0 + nil` is Integer#+'s
     until a Float has been added, however the slot is typed, so an Integer
     start keeps a flag the first Float value clears. */
  TyKind vt9 = acct == TY_FLOAT && bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_UNKNOWN;
  int vnil = (vt9 == TY_INT || vt9 == TY_FLOAT) && nullable_int_value(c, bb[bn - 1]);
  int tkind = -1;
  if (vnil && (argc == 0 || comp_ntype(c, argv[0]) == TY_INT)) {
    tkind = ++g_tmp;
    buf_printf(b, "; sp_bool _t%d = 1", tkind);
  }
  if (acct == TY_STRING) buf_printf(b, "; SP_GC_ROOT(_t%d)", tacc);
  buf_printf(b, "; for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) { ", ti, ti, tn, ti);
  /* A 2+-param block over an array of sub-arrays auto-splats each element into
     the params -- `sum { |v, i| a[i] }` over the [value, index] pairs an
     each_with_index enumerator answers. The poly-accumulator branch above does
     the same; without it here only the first param was bound and the rest
     stayed nil, so `a[i]` read index nil on every iteration (#3989). Only a
     poly element can BE a sub-array, so a typed array keeps the plain bind. */
  {
    int nps = 0; while (block_param_name(c, block, nps)) nps++;
    if (nps >= 2 && sp_streq(k, "Poly") && !block_param_is_multi(c, block, 0)) {
      int te = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d); ", te, ta, ti);
      Scope *bsc = comp_scope_of(c, block);
      for (int pj = 0; pj < nps; pj++) {
        const char *pn = block_param_name(c, block, pj);
        LocalVar *plv = (pn && bsc) ? scope_local(bsc, pn) : NULL;
        if (!plv) continue;
        char src[96];
        snprintf(src, sizeof src, "sp_poly_massign_get(_t%d, %d)", te, pj);
        buf_printf(b, "lv_%s = ", rename_local(pn));
        if (plv->type == TY_POLY || plv->type == TY_UNKNOWN) buf_puts(b, src);
        else emit_unbox_text(c, plv->type, src, b);
        buf_puts(b, "; ");
      }
    }
    else if (p0) buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d); ", p0, k, ta, ti);
  }
  /* The block's value expression may spill setup statements to g_pre (e.g.
     a nested count loop). Those must run per iteration: redirect g_pre into
     a local buffer while emitting the value, then splice it into the loop
     body ahead of the accumulation. */
  {
    Buf inner; memset(&inner, 0, sizeof inner);
    Buf valb; memset(&valb, 0, sizeof valb);
    Buf *saved_pre = g_pre; g_pre = &inner;
    { int svlm = g_line_map; g_line_map = 0;  /* a #line directive mid stmt-expr is a stray '#' */
      IterStep st; emit_iter_step_open(c, block, 0, 0, &st);
      emit_iter_step_tail(c, &st, &valb);
      g_line_map = svlm; }
    g_pre = saved_pre;
    if (inner.p) buf_puts(b, inner.p);
    if (acct == TY_INT) {
      buf_printf(b, "_t%d = sp_int_add(_t%d, %s)", tacc, tacc, valb.p ? valb.p : "0");
    }
    else if (acct == TY_STRING) {
      buf_printf(b, "_t%d = sp_str_concat(_t%d, %s)", tacc, tacc, valb.p ? valb.p : "\"\"");
    }
    else {
      /* A block value that can be its slot's nil (an element the block hands
         back from a nil-holding array) is the accumulator's coercion failure;
         added, a Float's NaN payload rode through the sum and it read back as
         nil, and an Integer's sentinel became -9.2e18. */
      if (vnil) {
        int tv = ++g_tmp;
        buf_printf(b, "%s _t%d = %s; ", vt9 == TY_INT ? "sp_int" : "sp_float", tv, valb.p ? valb.p : "0.0");
        if (vt9 == TY_INT) buf_printf(b, "if (SP_UNLIKELY(_t%d == SP_INT_NIL)) ", tv);
        else buf_printf(b, "if (SP_UNLIKELY(sp_float_is_nil(_t%d))) ", tv);
        if (tkind >= 0) buf_printf(b, "{ if (_t%d) sp_raise_nil_int_op(0, 0, \"+\"); sp_raise_nil_float_op(0, \"+\"); } ", tkind);
        else buf_puts(b, "sp_raise_nil_float_op(0, \"+\"); ");
        if (tkind >= 0 && vt9 == TY_FLOAT) buf_printf(b, "_t%d = 0; ", tkind);
        free(valb.p); memset(&valb, 0, sizeof valb);
        buf_printf(&valb, "_t%d", tv);
      }
      /* KBN step: fold the low-order bits dropped by _tacc + _tx into _tc. */
      buf_printf(b, "sp_float _t%d = %s; sp_float _t%d = _t%d + _t%d; "
                    "if (fabs(_t%d) >= fabs(_t%d)) _t%d += (_t%d - _t%d) + _t%d; "
                    "else _t%d += (_t%d - _t%d) + _t%d; _t%d = _t%d",
                 tx, valb.p ? valb.p : "0.0", tt, tacc, tx,
                 tacc, tx, tc, tacc, tt, tx,
                 tc, tx, tt, tacc, tacc, tt);
    }
    free(inner.p); free(valb.p);
  }
  /* The expression has to carry the type the CALL was inferred at, the way the
     general fold does. A block-forwarding method is inlined per call site, so
     the site handed a real Proc accumulates boxed while the site handed a
     literal block accumulates concretely -- one node, one inferred type, and
     the concrete accumulator went into the boxed slot the method's return
     type declared (#3916). */
  {
    char accn[48];
    if (acct == TY_FLOAT) snprintf(accn, sizeof accn, "_t%d + _t%d", tacc, tc);
    else snprintf(accn, sizeof accn, "_t%d", tacc);
    buf_puts(b, "; } ");
    if (comp_ntype(c, id) == TY_POLY) emit_boxed_text(c, acct, accn, b);
    else buf_puts(b, accn);
    buf_puts(b, "; })");
  }
  return 1;
}

/* int_array.slice_when { |a, b| cond }[.to_a].inspect  or
   int_array.chunk { |x| key }[.to_a].inspect  ->  inspect string.
   Emits setup to g_pre and the result variable to b. Returns 1 if handled. */
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow .to_a wrapper */
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_sw = sp_streq(m, "slice_when");
  int is_ck = sp_streq(m, "chunk");
  if (!is_sw && !is_ck) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0) return 0;
  /* an empty array literal has nothing to slice/chunk: the whole chain is
     statically "[]" (the literal has no side effects to preserve) */
  if (comp_ntype(c, pr) == TY_UNKNOWN && nt_type(nt, pr) &&
      sp_streq(nt_type(nt, pr), "ArrayNode")) {
    int en = 0; nt_arr(nt, pr, "elements", &en);
    if (en == 0) { buf_puts(b, "\"[]\""); return 1; }
  }
  if (comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  if (!p0n) return 0;
  const char *p0 = rename_local(p0n);

  if (is_sw) {
    /* slice_when { |a, b| cond } */
    const char *p1n = block_param_name(c, block, 1);
    if (!p1n) return 0;
    const char *p1 = rename_local(p1n);
    int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
    /* rooted like the two arrays built below it: the length is the loop
       bound, re-read every turn, and the block runs between two reads */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_IntArray_push_nilable(_t%d, lv_%s);\n", tcur, p0);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d + 1 < sp_IntArray_length(_t%d)) {\n", ti, ta);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d + 1);\n", p1, ta, ti);
    /* emit block body condition */
    Scope *bsc = comp_scope_of(c, block);
    LocalVar *lva = bsc ? scope_local(bsc, p0n) : NULL;
    LocalVar *lvb = bsc ? scope_local(bsc, p1n) : NULL;
    TyKind pta = lva ? lva->type : TY_UNKNOWN, ptb = lvb ? lvb->type : TY_UNKNOWN;
    if (lva) lva->type = TY_INT;
    if (lvb) lvb->type = TY_INT;
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 2);
    int save = g_indent; g_indent += 2;
    Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = save;
    if (lva) lva->type = pta;
    if (lvb) lvb->type = ptb;
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "if (%s) {\n", cb.p ? cb.p : "0"); free(cb.p);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "sp_PtrArray_push(_t%d, _t%d);\n", tout, tcur);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
    emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (sp_IntArray_length(_t%d) > 0) sp_PtrArray_push(_t%d, _t%d);\n", tcur, tout, tcur);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = sp_IntArrayPtrArray_inspect(_t%d);\n", tres, tout);
    buf_printf(b, "_t%d", tres);
    return 1;
  }

  /* chunk { |x| key_expr } -- group consecutive elements by key */
  int ta = ++g_tmp, tkeys = ++g_tmp, tgrps = ++g_tmp, tcur = ++g_tmp;
  int tpk = ++g_tmp, ti = ++g_tmp, tstr = ++g_tmp, tj = ++g_tmp, tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  /* rooted for the walk, as the slice_when arm above roots its receiver */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tgrps, tgrps);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = NULL;\n", tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = 0;\n", tpk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
  /* emit key expression */
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv0 = bsc ? scope_local(bsc, p0n) : NULL;
  TyKind pt0 = lv0 ? lv0->type : TY_UNKNOWN;
  if (lv0) lv0->type = TY_INT;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb); g_indent = save;
  if (lv0) lv0->type = pt0;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_int _tkey_%d = %s;\n", ta, kb.p ? kb.p : "0"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d == 0 || _tkey_%d != _t%d) {\n", ti, ta, tpk);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_IntArray_push(_t%d, _tkey_%d);\n", tkeys, ta);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PtrArray_push(_t%d, _t%d);\n", tgrps, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = _tkey_%d;\n", tpk, ta);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_IntArray_push_nilable(_t%d, lv_%s);\n", tcur, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* build inspect string */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"[\"); SP_GC_ROOT(_t%d);\n", tstr, tstr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", tj, tj, tkeys, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d > 0) sp_String_append(_t%d, \", \");\n", tj, tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \"[\");\n", tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, sp_int_to_s(sp_IntArray_get(_t%d, _t%d)));\n", tstr, tkeys, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \", \");\n", tstr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, sp_IntArray_inspect((sp_IntArray*)_t%d->data[_t%d]));\n", tstr, tgrps, tj);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append(_t%d, \"]\");\n", tstr);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_String_append(_t%d, \"]\");\n", tstr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "const char *_t%d = _t%d->data;\n", tres, tstr);
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* hash.chunk { |k, v| key }.to_a -> a poly array of [key, [[k, v], ...]] pairs
   grouping consecutive entries by the block key. The key is boxed, so any key
   type works, and CRuby's separator protocol applies: a nil or :_separator key
   drops the entry and breaks the current run; :_alone chunks one entry per
   group. Emits setup to g_pre and the result variable to b. */
static int emit_hash_chunk_first_class(Compiler *c, int pr, TyKind prt, int block, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *hn = ty_hash_cname(prt);
  if (!hn) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  const char *p1n = block_param_name(c, block, 1);
  if (!p0n || !p1n) return 0;
  const char *p0 = rename_local(p0n);
  const char *p1 = rename_local(p1n);
  TyKind kt = ty_hash_key(prt), vt = ty_hash_val(prt);
  int sep_id = comp_sym_intern(c, "_separator");
  int alone_id = comp_sym_intern(c, "_alone");

  int th = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, tpair = ++g_tmp, tkv = ++g_tmp;
  int tkey = ++g_tmp, tpk = ++g_tmp, thas = ++g_tmp, tnew = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sHash *_t%d = %s; SP_GC_ROOT(_t%d);\n", hn, th, rb.p ? rb.p : "", th);
  free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tpair, tpair);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tkv, tkv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tkey, tkey);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tpk, tpk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "int _t%d = 0, _t%d = 0;\n", thas, tnew);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, th, ti);

  /* typed key/value sources, following the hash-each iteration idiom */
  char ksrc[256], vsrc[256];
  if (prt == TY_POLY_POLY_HASH) {
    snprintf(ksrc, sizeof ksrc, "_t%d->keys[_t%d->order[_t%d]]", th, th, ti);
    snprintf(vsrc, sizeof vsrc, "_t%d->vals[_t%d->order[_t%d]]", th, th, ti);
  }
  else {
    snprintf(ksrc, sizeof ksrc, "_t%d->order[_t%d]", th, ti);
    snprintf(vsrc, sizeof vsrc, "sp_%sHash_get(_t%d, _t%d->order[_t%d])", hn, th, th, ti);
  }
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv0 = bsc ? scope_local(bsc, p0n) : NULL;
  LocalVar *lv1 = bsc ? scope_local(bsc, p1n) : NULL;
  int box0 = lv0 && lv0->type == TY_POLY && kt != TY_POLY;
  int box1 = lv1 && lv1->type == TY_POLY && vt != TY_POLY;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = ", p0);
  if (box0) emit_boxed_text(c, kt, ksrc, g_pre); else buf_puts(g_pre, ksrc);
  buf_puts(g_pre, ";\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = ", p1);
  if (box1) emit_boxed_text(c, vt, vsrc, g_pre); else buf_puts(g_pre, vsrc);
  buf_puts(g_pre, ";\n");

  /* a `next <key>` writes the key slot itself */
  if (fold_body_has_next(c, nt_ref(nt, block, "body"))) {
    char dst[24]; snprintf(dst, sizeof dst, "_t%d", tkey);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "%s = sp_box_nil();\n", dst);
    int save = g_indent; g_indent++;
    emit_block_value_into(c, block, dst, 1, g_indent);
    g_indent = save;
  }
  else {
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    int save = g_indent; g_indent++;
    Buf kb; memset(&kb, 0, sizeof kb); emit_boxed(c, bb[bn - 1], &kb); g_indent = save;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "_t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
  }

  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d.tag == SP_TAG_NIL || (_t%d.tag == SP_TAG_SYM && _t%d.v.i == (sp_sym)%d))"
                    " { _t%d = 0; continue; }\n", tkey, tkey, tkey, sep_id, thas);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d.tag == SP_TAG_SYM && _t%d.v.i == (sp_sym)%d) { _t%d = 1; _t%d = 0; }\n",
             tkey, tkey, alone_id, tnew, thas);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "else if (!_t%d || !sp_poly_eq(_t%d, _t%d)) { _t%d = 1; _t%d = 1; _t%d = _t%d; }\n",
             thas, tkey, tpk, tnew, thas, tpk, tkey);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "else _t%d = 0;\n", tnew);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d) {\n", tnew);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tpair);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d);\n", tpair, tkey);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tpair, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tout, tpair);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tkv);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tkv);
  if (kt == TY_POLY) buf_puts(g_pre, ksrc); else emit_boxed_text(c, kt, ksrc, g_pre);
  buf_puts(g_pre, ");\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tkv);
  if (vt == TY_POLY) buf_puts(g_pre, vsrc); else emit_boxed_text(c, vt, vsrc, g_pre);
  buf_puts(g_pre, ");\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tcur, tkv);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tout);
  return 1;
}

/* int_array.chunk { |x| int_key }.to_a -> a poly array of [key, [members]] pairs
   (each a 2-element poly array), first-class so p/indexing/iteration work.
   Integer keys only, matching the chunk inspect path. Returns 1 if handled. */
int emit_chunk_first_class_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "to_a")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m || !sp_streq(m, "chunk")) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0) return 0;
  TyKind prt = comp_ntype(c, pr);
  if (ty_is_hash(prt)) return emit_hash_chunk_first_class(c, pr, prt, block, b);
  if (prt != TY_INT_ARRAY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  if (!p0n) return 0;
  const char *p0 = rename_local(p0n);

  int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, tpk = ++g_tmp, ti = ++g_tmp, thas = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", ta, rb.p ? rb.p : "", ta); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = NULL; SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_tpair_%d = NULL; SP_GC_ROOT(_tpair_%d);\n", ta, ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = 0; int _t%d = 0;\n", tpk, thas);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv0 = bsc ? scope_local(bsc, p0n) : NULL;
  TyKind pt0 = lv0 ? lv0->type : TY_UNKNOWN;
  if (lv0) lv0->type = TY_INT;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb); g_indent = save;
  if (lv0) lv0->type = pt0;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_int _tkey_%d = %s;\n", ta, kb.p ? kb.p : "0"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (!_t%d || _tkey_%d != _t%d) {\n", thas, ta, tpk);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_IntArray_new();\n", tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_tpair_%d = sp_PolyArray_new();\n", ta);
  emit_indent(g_pre, g_indent + 2);
  /* a boolean group key boxes as true/false, not its 0/1 bits */
  buf_printf(g_pre, "sp_PolyArray_push(_tpair_%d, %s(_tkey_%d));\n", ta,
             comp_ntype(c, bb[bn - 1]) == TY_BOOL ? "sp_box_bool" : "sp_box_int", ta);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyArray_push(_tpair_%d, sp_box_int_array(_t%d));\n", ta, tcur);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_tpair_%d));\n", tout, ta);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = _tkey_%d; _t%d = 1;\n", tpk, ta, thas);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_IntArray_push_nilable(_t%d, lv_%s);\n", tcur, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tout);
  return 1;
}

/* <array>.cycle.first(n) / .cycle.take(n) -> the first n elements of the infinite
   cycle: arr[i % len] for i in [0, n). Only the bounded consumers are handled; an
   unbounded cycle (bare, .to_a, .each, .map) is left to the loud reject so it can
   never hang. Returns 1 if handled. */
int emit_cycle_bounded_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "first") && !sp_streq(name, "take"))) return 0;
  int args = nt_ref(nt, id, "arguments");
  int ac = 0; const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &ac) : NULL;
  if (ac != 1) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn || !sp_streq(rn, "cycle") || nt_ref(nt, recv, "block") >= 0) return 0;
  int cargs = nt_ref(nt, recv, "arguments");
  int cac = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &cac);
  if (cac != 0) return 0;  /* only the argless (infinite) cycle */
  int pr = nt_ref(nt, recv, "receiver");
  TyKind rt = pr >= 0 ? comp_ntype(c, pr) : TY_UNKNOWN;
  if (!ty_is_array(rt)) return 0;
  /* a poly array has no array_kind; name it like every other Poly emit (#3604) */
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  int ta = ++g_tmp, tn = ++g_tmp, tr = ++g_tmp, tlen = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  Buf nb; memset(&nb, 0, sizeof nb);
  Buf npre; memset(&npre, 0, sizeof npre);
  Buf *sv = g_pre; g_pre = &npre; emit_expr(c, av[0], &nb); g_pre = sv;
  if (npre.p) buf_puts(g_pre, npre.p); free(npre.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", k, ta, rb.p ? rb.p : "", ta); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = %s;\n", tn, nb.p ? nb.p : "0"); free(nb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to take negative size\");\n", tn);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", k, tr, k, tr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tlen, k, ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (_t%d > 0) for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) "
             "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d %% _t%d));\n",
             tlen, ti, ti, tn, ti, k, tr, k, ta, ti, tlen);
  if (sp_streq(k, "Int") || sp_streq(k, "Float")) {   /* the receiver's nils, repeated */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray_nil_from(_t%d, _t%d);\n", k, tr, ta);
  }
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* int_array.chunk_while { |a, b| cond }.to_a -> array of runs. Adjacent elements
   stay in one run while the block is true; a boundary falls where it is false
   (the inverse of slice_when). Materialized as a poly array of boxed int arrays
   so the result is first-class -- `p`, indexing, and further iteration work.
   Emits setup to g_pre and the result var to b. Returns 1 if handled. */
int emit_chunk_while_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "to_a")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m || (!sp_streq(m, "chunk_while") && !sp_streq(m, "slice_when"))) return 0;
  int is_sw = sp_streq(m, "slice_when");  /* slice_when boundary = block TRUE */
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0) return 0;
  TyKind prt = comp_ntype(c, pr);
  /* an int Range materializes to the int array the walk below expects */
  int pr_range = (prt == TY_RANGE && range_enum_redispatch(c, recv));
  if (prt != TY_INT_ARRAY && !pr_range) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  const char *p1n = block_param_name(c, block, 1);
  if (!p0n || !p1n) return 0;
  const char *p0 = rename_local(p0n), *p1 = rename_local(p1n);

  int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  if (pr_range) {
    int trg = ++g_tmp;
    buf_printf(&rb, "({ sp_Range _t%d = ", trg);
    emit_expr(c, pr, &rb);
    buf_printf(&rb, "; sp_range_to_ia(_t%d); })", trg);
  }
  else emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_IntArray_push_nilable(_t%d, lv_%s);\n", tcur, p0);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (_t%d + 1 < sp_IntArray_length(_t%d)) {\n", ti, ta);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d + 1);\n", p1, ta, ti);
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lva = bsc ? scope_local(bsc, p0n) : NULL;
  LocalVar *lvb = bsc ? scope_local(bsc, p1n) : NULL;
  TyKind pta = lva ? lva->type : TY_UNKNOWN, ptb = lvb ? lvb->type : TY_UNKNOWN;
  if (lva) lva->type = TY_INT;
  if (lvb) lvb->type = TY_INT;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 2);
  int save = g_indent; g_indent += 2;
  /* the block's value by Ruby's truth: a boxed one (`a.send(s, b)`) is no C
     scalar, and an Integer one is true even at 0 */
  Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = save;
  if (lva) lva->type = pta;
  if (lvb) lvb->type = ptb;
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, is_sw ? "if (%s) {\n" : "if (!(%s)) {\n", cb.p ? cb.p : "0"); free(cb.p);
  emit_indent(g_pre, g_indent + 3);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_int_array(_t%d));\n", tout, tcur);
  emit_indent(g_pre, g_indent + 3);
  buf_printf(g_pre, "_t%d = sp_IntArray_new();\n", tcur);
  emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (sp_IntArray_length(_t%d) > 0) sp_PolyArray_push(_t%d, sp_box_int_array(_t%d));\n", tcur, tout, tcur);
  buf_printf(b, "_t%d", tout);
  return 1;
}

/* Bind a chunk / slice_before / slice_after block's params to element ti of
   the snapshot ta, on the line the caller has indented. A block of two or
   more params over a poly snapshot splats an element that is an Array across
   the first two, as CRuby's yield of one value does (a Hash's [key, value]
   pairs); any other element binds the first, with the second nil. The second
   is written only where it has a C declaration (`_2` beside `_1` and `_3`
   has none) and is not the first's own name (`|_, _|`). */
static void emit_chunk_elem_bind(Compiler *c, int ta, int ti, const char *p0,
                                 const char *p1, int p1_declared, TyKind at0, int pin_poly) {
  char gv[48]; snprintf(gv, sizeof gv, "sp_PolyArray_get(_t%d, _t%d)", ta, ti);
  if (pin_poly && p1) {
    int te = ++g_tmp;
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", te, gv, te);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "int _fs%d = (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id));\n", te, te, te);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = _fs%d ? sp_poly_index_poly(_t%d, sp_box_int(0)) : _t%d;\n", p0, te, te, te);
    if (p1_declared && !sp_streq(p0, p1)) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "lv_%s = _fs%d ? sp_poly_index_poly(_t%d, sp_box_int(1)) : sp_box_nil();\n", p1, te, te);
    }
    return;
  }
  buf_printf(g_pre, "lv_%s = ", p0);
  if (at0 == TY_POLY) buf_puts(g_pre, gv); else emit_unbox_text(c, at0, gv, g_pre);
  buf_puts(g_pre, ";\n");
}

/* Core of the chunk-family emissions: emit the runs poly-array (or [key,
   run] pairs for chunk) for the chunk-family call node `ck` into g_pre and
   return its temp id (-1 when the shape is not servable). The receiver may
   be a poly array (a redirected user-Enumerable receiver included) or a
   typed int/str array, snapshotted through sp_enum_items_from. Block params
   pin to TY_POLY for the body emission. */
static int emit_chunk_family_runs(Compiler *c, int ck) {
  const NodeTable *nt = c->nt;
  const char *m = nt_str(nt, ck, "name");
  if (!m) return -1;
  int is_sw = sp_streq(m, "slice_when");
  int is_cw = sp_streq(m, "chunk_while");
  int is_ck = sp_streq(m, "chunk");
  int is_sb = sp_streq(m, "slice_before");
  int is_sa = sp_streq(m, "slice_after");
  if (!is_sw && !is_cw && !is_ck && !is_sb && !is_sa) return -1;
  /* the block forms only; the value-pattern forms are served elsewhere */
  if ((is_sb || is_sa) && nt_ref(nt, ck, "arguments") >= 0) return -1;
  int block = nt_ref(nt, ck, "block");
  if (block < 0) return -1;
  int pr = nt_ref(nt, ck, "receiver");
  TyKind prt = pr >= 0 ? comp_ntype(c, pr) : TY_UNKNOWN;
  /* an empty [] literal never narrowed; serve it as an (empty) poly array */
  int pr_empty_lit = pr >= 0 && prt == TY_UNKNOWN && nt_type(nt, pr) &&
                     sp_streq(nt_type(nt, pr), "ArrayNode");
  if (pr_empty_lit) {
    int pen = 0; nt_arr(nt, pr, "elements", &pen);
    if (pen != 0) pr_empty_lit = 0;
  }
  if (pr < 0 || (prt != TY_POLY_ARRAY && prt != TY_INT_ARRAY && prt != TY_POLY &&
                 prt != TY_STR_ARRAY && prt != TY_FLOAT_ARRAY && !pr_empty_lit))
    return -1;
  /* A `&.` call has to reach the safe-nav guard first: this lowering walks the
     receiver and never looks at the operator, so `v&.chunk_while { }` walked a
     nil receiver and raised where CRuby answers nil. Stand down only BEFORE
     the guard runs -- it re-enters this emission on the guarded temp with
     g_sn_skip set, and that pass has to lower normally. */
  { const char *sop = nt_str(nt, ck, "call_operator");
    if (sop && sp_streq(sop, "&.") && g_sn_skip != ck && prt == TY_POLY) return -1; }
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return -1;
  const char *p0n = block_param_name(c, block, 0);
  if (!p0n) return -1;
  const char *p1n = block_param_name(c, block, 1);
  if ((is_sw || is_cw) && !p1n) return -1;
  const char *p0 = rename_local(p0n);
  const char *p1 = p1n ? rename_local(p1n) : NULL;

  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lva = bsc ? scope_local(bsc, p0n) : NULL;
  LocalVar *lvb = (bsc && p1n) ? scope_local(bsc, p1n) : NULL;
  TyKind pta = lva ? lva->type : TY_UNKNOWN, ptb = lvb ? lvb->type : TY_UNKNOWN;
  /* A poly receiver pins the params poly for the body emission. A typed
     int/str receiver keeps the pass-assigned param types -- the snapshot's
     boxed elements unbox into them below. */
  int pin_poly = (prt == TY_POLY_ARRAY || prt == TY_POLY);
  if (pin_poly && lva) lva->type = TY_POLY;
  if (pin_poly && lvb) lvb->type = TY_POLY;
  TyKind at0 = (!pin_poly && lva && lva->type != TY_UNKNOWN) ? lva->type : TY_POLY;
  TyKind at1 = (!pin_poly && lvb && lvb->type != TY_UNKNOWN) ? lvb->type : TY_POLY;

  int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  if (!pr_empty_lit) emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  if (pr_empty_lit)
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ta, ta);
  else if (prt == TY_POLY_ARRAY)
    buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", ta, rb.p ? rb.p : "", ta);
  /* A boxed receiver (a container-read row, a boxed Hash) walks the elements
     sp_poly_arr_recv renders -- a hash's [key, value] pairs (#3451). */
  else if (prt == TY_POLY)
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", m, ta);
  else
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_enum_items_from(sp_box_%s_array(%s)); SP_GC_ROOT(_t%d);\n",
               ta, prt == TY_INT_ARRAY ? "int" : prt == TY_STR_ARRAY ? "str" : "float",
               rb.p ? rb.p : "", ta);
  free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
  int tpk = -1, thas = -1;
  if (is_ck) {
    tpk = ++g_tmp; thas = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tpk, tpk);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "int _t%d = 0;\n", thas);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  if (is_ck) {
    int tk = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    emit_chunk_elem_bind(c, ta, ti, p0, p1, ptb != TY_UNKNOWN, at0, pin_poly);
    Buf kb; memset(&kb, 0, sizeof kb);
    /* a `next <key>` answers the key through a slot the body writes */
    if (fold_body_has_next(c, body)) {
      int tnk = ++g_tmp;
      char dst[24]; snprintf(dst, sizeof dst, "_t%d", tnk);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_RbVal %s = sp_box_nil(); SP_GC_ROOT_RBVAL(%s);\n", dst, dst);
      int save = g_indent; g_indent += 1;
      emit_block_value_into(c, block, dst, 1, g_indent);
      g_indent = save;
      buf_puts(&kb, dst);
    }
    else {
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
      int save = g_indent; g_indent += 1;
      emit_boxed(c, bb[bn - 1], &kb); g_indent = save;
    }
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tk, kb.p ? kb.p : "sp_box_nil()", tk); free(kb.p);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (!_t%d || !sp_poly_eq(_t%d, _t%d)) {\n", thas, tk, tpk);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "if (_t%d) {\n", thas);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "sp_PolyArray *_pr = sp_PolyArray_new(); SP_GC_ROOT(_pr); sp_PolyArray_push(_pr, _t%d); sp_PolyArray_push(_pr, sp_box_poly_array(_t%d)); sp_PolyArray_push(_t%d, sp_box_poly_array(_pr));\n", tpk, tcur, tout);
    emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "_t%d = sp_PolyArray_new(); _t%d = _t%d; _t%d = 1;\n", tcur, tpk, tk, thas);
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tcur, ta, ti);
  }
  else if (is_sb || is_sa) {
    /* single-element predicate: slice_before opens a new run at a true
       element (flushing the current run first); slice_after closes the run
       after a true element */
    int tc = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    emit_chunk_elem_bind(c, ta, ti, p0, p1, ptb != TY_UNKNOWN, at0, pin_poly);
    Buf cb; memset(&cb, 0, sizeof cb);
    if (!emit_block_cond_next(c, block, g_indent + 1, &cb)) {
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
      int save = g_indent; g_indent += 1;
      emit_cond(c, bb[bn - 1], &cb); g_indent = save;
    }
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "int _t%d = (%s) ? 1 : 0;\n", tc, cb.p ? cb.p : "0"); free(cb.p);
    if (is_sb) {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d && sp_PolyArray_length(_t%d) > 0) {\n", tc, tcur);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tout, tcur);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tcur);
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tcur, ta, ti);
    }
    else {
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tcur, ta, ti);
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (_t%d) {\n", tc);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tout, tcur);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tcur);
      emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    }
  }
  else {
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "if (_t%d > 0) {\n", ti);
    emit_indent(g_pre, g_indent + 2);
    {
      char gv[48]; snprintf(gv, sizeof gv, "sp_PolyArray_get(_t%d, _t%d - 1)", ta, ti);
      buf_printf(g_pre, "lv_%s = ", p0);
      if (at0 == TY_POLY) buf_puts(g_pre, gv); else emit_unbox_text(c, at0, gv, g_pre);
      buf_puts(g_pre, ";\n");
    }
    emit_indent(g_pre, g_indent + 2);
    {
      char gv[48]; snprintf(gv, sizeof gv, "sp_PolyArray_get(_t%d, _t%d)", ta, ti);
      buf_printf(g_pre, "lv_%s = ", p1);
      if (at1 == TY_POLY) buf_puts(g_pre, gv); else emit_unbox_text(c, at1, gv, g_pre);
      buf_puts(g_pre, ";\n");
    }
    Buf cb; memset(&cb, 0, sizeof cb);
    if (!emit_block_cond_next(c, block, g_indent + 2, &cb)) {
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 2);
      int save = g_indent; g_indent += 2;
      emit_cond(c, bb[bn - 1], &cb); g_indent = save;
    }
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, is_sw ? "if (%s) {\n" : "if (!(%s)) {\n", cb.p ? cb.p : "0"); free(cb.p);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tout, tcur);
    emit_indent(g_pre, g_indent + 3);
    buf_printf(g_pre, "_t%d = sp_PolyArray_new();\n", tcur);
    emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tcur, ta, ti);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  if (is_ck)
    buf_printf(g_pre, "if (_t%d) { sp_PolyArray *_pr = sp_PolyArray_new(); SP_GC_ROOT(_pr); sp_PolyArray_push(_pr, _t%d); sp_PolyArray_push(_pr, sp_box_poly_array(_t%d)); sp_PolyArray_push(_t%d, sp_box_poly_array(_pr)); }\n", thas, tpk, tcur, tout);
  else
    buf_printf(g_pre, "if (sp_PolyArray_length(_t%d) > 0) sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));\n", tcur, tout, tcur);
  if (lva) lva->type = pta;
  if (lvb) lvb->type = ptb;
  return tout;
}

/* array.{chunk_while|slice_when|chunk} { }.to_a -> a poly array of runs
   (or [key, run] pairs for chunk). The int-array twins above keep their lean
   typed loops for int receivers reached through them; this one drives boxed
   elements. Returns 1 if handled. */
int emit_chunk_family_poly_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "to_a")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  int tout = emit_chunk_family_runs(c, recv);
  if (tout < 0) return 0;
  buf_printf(b, "_t%d", tout);
  return 1;
}

/* array.{chunk_while|slice_when|chunk} { } standing on its own (stored in a
   local, passed to p, ...): a first-class Enumerator over the eagerly
   materialized runs, inspecting as the Generator wrapper CRuby shows.
   Terminal chains (.to_a and the typed int-array forms) are matched earlier
   at their terminal node and never reach this. Returns 1 if handled. */
int emit_chunk_family_enum_expr(Compiler *c, int id, Buf *b) {
  int tout = emit_chunk_family_runs(c, id);
  if (tout < 0) return 0;
  buf_printf(b, "sp_enum_as_gen(sp_Enumerator_new_from_items(_t%d))", tout);
  return 1;
}

/* int_array.product(int_array)[.to_a].inspect -> the Cartesian product
   rendered as a nested-array string. The product result has no first-class
   type, so only this inline inspect chain is supported. Returns 1 if handled. */
int emit_product_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow an intervening .to_a */
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_product = sp_streq(m, "product");
  int is_slice = sp_streq(m, "slice_before") || sp_streq(m, "slice_after");
  if (!is_product && !is_slice) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  int pargs = nt_ref(nt, recv, "arguments");
  int pac = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pac) : NULL;
  if (pr < 0 || pac != 1) return 0;
  if (comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  if (is_product) {
    /* the other operand is an int_array or an empty array literal */
    TyKind at = comp_ntype(c, pav[0]);
    int empty_lit = nt_type(nt, pav[0]) && sp_streq(nt_type(nt, pav[0]), "ArrayNode") &&
                    ({ int en = 0; nt_arr(nt, pav[0], "elements", &en); en == 0; });
    if (at != TY_INT_ARRAY && !empty_lit) return 0;
    buf_puts(b, "sp_IntArrayPtrArray_inspect(sp_IntArray_product(");
    emit_expr(c, pr, b); buf_puts(b, ", ");
    if (empty_lit) buf_puts(b, "sp_IntArray_new()"); else emit_expr(c, pav[0], b);
    buf_puts(b, "))");
    return 1;
  }
  /* slice_before / slice_after with an int delimiter */
  if (comp_ntype(c, pav[0]) != TY_INT) return 0;
  buf_printf(b, "sp_IntArrayPtrArray_inspect(sp_IntArray_%s(", m);
  emit_expr(c, pr, b); buf_puts(b, ", "); emit_expr(c, pav[0], b); buf_puts(b, "))");
  return 1;
}

/* numeric.step(limit[, step]) without a block, materialized as an int or
   float array (so a following .to_a / .inspect works). Returns 1 if handled. */
int emit_step_array_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "step")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (rt != TY_INT && rt != TY_FLOAT && rt != TY_RATIONAL) return 0;
  int args = nt_ref(nt, id, "arguments");
  int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
  if (sc < 1) return 0;
  /* Rational receiver: walk the exact sequence through the poly numeric tower
     and collect the boxed Rational/Integer values into a PolyArray (#2566). */
  /* A Bignum limit or step does not fit the sp_int loop below; walk the
     sequence boxed, exactly as a Rational receiver does (#3006). */
  int bn_bound = 0;
  for (int sk = 0; sk < sc; sk++) if (comp_ntype(c, sv[sk]) == TY_BIGINT) bn_bound = 1;
  if (rt == TY_RATIONAL || bn_bound) {
    int trr = ++g_tmp, tcc = ++g_tmp, tll = ++g_tmp, tss = ++g_tmp, tdd = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", trr, trr);
    buf_printf(b, " sp_RbVal _t%d = ", tcc);
    if (rt == TY_RATIONAL) { buf_puts(b, "sp_box_rational("); emit_expr(c, recv, b); buf_puts(b, ")"); }
    else emit_boxed(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tcc);
    buf_printf(b, " sp_RbVal _t%d = ", tll); emit_boxed(c, sv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tll);
    buf_printf(b, " sp_RbVal _t%d = ", tss);
    if (sc >= 2) emit_boxed(c, sv[1], b); else buf_puts(b, "sp_box_int(1)");
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tss);
    buf_printf(b, " if (sp_poly_cmp_ck(_t%d, sp_box_int(0)) == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");", tss);
    buf_printf(b, " sp_bool _t%d = sp_poly_cmp_ck(_t%d, sp_box_int(0)) > 0;", tdd, tss);
    buf_printf(b, " for (; _t%d ? sp_poly_le(_t%d, _t%d) : sp_poly_ge(_t%d, _t%d); _t%d = sp_poly_add(_t%d, _t%d)) sp_PolyArray_push(_t%d, _t%d);",
               tdd, tcc, tll, tcc, tll, tcc, tcc, tss, trr, tcc);
    buf_printf(b, " _t%d; })", trr);
    return 1;
  }
  int is_float = (rt == TY_FLOAT) || comp_ntype(c, sv[0]) == TY_FLOAT ||
                 (sc >= 2 && comp_ntype(c, sv[1]) == TY_FLOAT);
  int tr = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
  if (!is_float) {
    buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d); sp_int _t%d = ", tr, tr, tl);
    emit_int_expr(c, sv[0], b); buf_printf(b, "; sp_int _t%d = ", ts);
    if (sc >= 2) emit_int_expr(c, sv[1], b); else buf_puts(b, "1");
    /* a zero step never advances, so CRuby rejects it outright (#3648) */
    buf_printf(b, "; if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");", ts);
    buf_printf(b, " for (sp_int _t%d = ", ti); emit_expr(c, recv, b);
    buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) sp_IntArray_push(_t%d, _t%d); _t%d; })",
               ts, ti, tl, ti, tl, ti, ts, tr, ti, tr);
    return 1;
  }
  int tb = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_FloatArray *_t%d = sp_FloatArray_new(); SP_GC_ROOT(_t%d); sp_float _t%d = ", tr, tr, tb);
  /* the operands through the Float slot's conversion, as the block form
     takes them: a boxed limit or step (an Integer local under
     --int-overflow=promote) unboxes rather than landing in the slot as a
     box (#4779) */
  emit_float_expr(c, recv, b); buf_printf(b, "; sp_float _t%d = ", tl); emit_float_expr(c, sv[0], b);
  buf_printf(b, "; sp_float _t%d = ", ts);
  if (sc >= 2) emit_float_expr(c, sv[1], b); else buf_puts(b, "1.0");
  buf_printf(b, "; if (_t%d == 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");", ts);
  /* an infinite or huge count cannot be materialised (CRuby's blockless
     form is lazy): raise as sp_FloatArray_from_step does rather than push
     until memory runs out */
  buf_printf(b, " sp_float _t%d = sp_float_step_size(_t%d, _t%d, _t%d, 0);"
                " if (_t%d >= (sp_float)(1LL << 30)) sp_raise_cls(\"RangeError\", \"range too large to materialize\");"
                " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_FloatArray_push(_t%d, sp_float_step_at(_t%d, _t%d, _t%d, _t%d)); _t%d; })",
             tn, tb, tl, ts, tn, ti, ti, tn, ti, tr, tb, tl, ts, ti, tr);
  return 1;
}

/* inject(:op) / reduce(:op) / inject(&:op) / inject(init, :op) as an
   expression: fold the array with a symbol-named arithmetic operator. The
   block-fold form (inject { |a, e| ... }) is not handled here. Returns 1 if
   handled. */
int emit_inject_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "inject") && !sp_streq(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* empty array literal `[]` has TY_UNKNOWN; treat as TY_INT_ARRAY */
  if (rt == TY_UNKNOWN && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) rt = TY_INT_ARRAY;
  }
  /* `[[ints],...].inject(&:&|:||:-)`: fold the inner int arrays with a set op.
     Handled before the typed-array path since poly arrays have no array_kind. */
  if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
    int sblk = nt_ref(nt, id, "block");
    const char *sop = NULL;
    if (sblk >= 0 && nt_type(nt, sblk) && sp_streq(nt_type(nt, sblk), "BlockArgumentNode")) {
      int ex = nt_ref(nt, sblk, "expression");
      if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
    }
    if (sop && (sp_streq(sop, "&") || sp_streq(sop, "|") || sp_streq(sop, "-"))) {
      const char *sfn = sp_streq(sop, "&") ? "sp_IntArray_intersect"
                      : sp_streq(sop, "|") ? "sp_IntArray_union" : "sp_IntArray_difference";
      int ta = ++g_tmp, tn = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = sp_PolyArray_length(_t%d); ", tn, ta);
      buf_printf(b, "sp_IntArray *_t%d = _t%d > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", tacc, tn, ta);
      buf_printf(b, "for (sp_int _t%d = 1; _t%d < _t%d; _t%d++) _t%d = %s(_t%d, (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p); ",
                 ti, ti, tn, ti, tacc, sfn, tacc, ta, ti);
      buf_printf(b, "_t%d; })", tacc);
      return 1;
    }
  }
  /* generic poly array with a :sym operator (or an initial value + :sym):
     fold via the tag-dispatching sp_poly_<op> over boxed elements. Covers
     Hash#values / #map results, whose elements are boxed. */
  if (rt == TY_POLY_ARRAY) {
    int pblk = nt_ref(nt, id, "block");
    const char *pop = NULL;
    if (pblk >= 0 && nt_type(nt, pblk) && sp_streq(nt_type(nt, pblk), "BlockArgumentNode")) {
      int ex = nt_ref(nt, pblk, "expression");
      if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) pop = nt_str(nt, ex, "value");
    }
    int pargs = nt_ref(nt, id, "arguments");
    int pac = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pac) : NULL;
    int init_node = -1;
    if (!pop && pac >= 1 && pav) {
      /* a symbol literal, or a local statically holding one (s = :+) */
      const char *psv = sym_static_value(c, pav[pac - 1]);
      if (psv) {
        pop = psv;
        if (pac == 2) init_node = pav[0];
      }
    }
    else if (pop && pac == 1 && pav) init_node = pav[0];
    const char *pfn = pop ? (sp_streq(pop, "+") ? "sp_poly_add"
                           : sp_streq(pop, "-") ? "sp_poly_sub"
                           : sp_streq(pop, "*") ? "sp_poly_mul"
                           : sp_streq(pop, "/") ? "sp_poly_div"
                           : sp_streq(pop, "&") ? "sp_poly_band"
                           : sp_streq(pop, "|") ? "sp_poly_bor"
                           : sp_streq(pop, "^") ? "sp_poly_bxor" : NULL) : NULL;
    if (pfn) {
      int ta = ++g_tmp, tn = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = sp_PolyArray_length(_t%d);"
                    " sp_RbVal _t%d = ", ta, tn, ta, tacc);
      int start;
      if (init_node >= 0) { emit_boxed(c, init_node, b); start = 0; }
      else { buf_printf(b, "_t%d > 0 ? sp_PolyArray_get(_t%d, 0) : sp_box_nil()", tn, ta); start = 1; }
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);"
                    " for (sp_int _t%d = %d; _t%d < _t%d; _t%d++)"
                    " _t%d = %s(_t%d, sp_PolyArray_get(_t%d, _t%d)); ",
                 tacc, ti, start, ti, tn, ti, tacc, pfn, tacc, ta, ti);
      /* the fold accumulates boxed; unbox to the call's inferred scalar type
         so `c = [].reduce(5, :+)` lands in an sp_int slot (#2365) */
      {
        TyKind want = comp_ntype(c, id);
        char accs[24]; snprintf(accs, sizeof accs, "_t%d", tacc);
        /* no initial value and an empty receiver: nil, as the slot's sentinel */
        if (init_node < 0 && want == TY_INT)
          buf_printf(b, "sp_poly_as_int_or_nil(%s)", accs);
        else if (init_node < 0 && want == TY_FLOAT)
          buf_printf(b, "sp_poly_as_float_or_nil(%s)", accs);
        else if (want != TY_POLY && want != TY_UNKNOWN && is_scalar_ret(want))
          emit_unbox_text(c, want, accs, b);
        else
          buf_puts(b, accs);
      }
      buf_puts(b, "; })");
      return 1;
    }
  }
  if (!ty_is_array(rt)) return 0;
  /* A poly array folds through the boxed sp_poly_binop_sym path below; the
     concretely-typed arms bail on it via their own et checks (#2880). */
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);

  /* Find the operator symbol (from a &:op block or a trailing :op arg), its
     NODE, and any explicit initial value. The two spellings put the seed in
     different places: `reduce(seed, :op)` in front of the symbol argument,
     `reduce(seed, &:op)` as the only argument, the symbol being in the block.
     Only the first was read for a seed, so the &:op spelling folded from the
     first ELEMENT and dropped the seed entirely. */
  const char *op = NULL; int init = -1, sym_node = -1;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block >= 0 && nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode")) {
    int ex = nt_ref(nt, block, "expression");
    if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode"))
      { op = nt_str(nt, ex, "value"); sym_node = ex; }
  }
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (op && sym_node >= 0 && argc == 1) init = argv[0];
  else if (!op && argc >= 1) {
    int last = argv[argc - 1];
    /* a symbol literal, or a local that statically holds one (s = :+) */
    const char *sv = sym_static_value(c, last);
    if (sv) {
      op = sv;
      sym_node = last;
      if (argc == 2) init = argv[0];
    }
  }
  /* A runtime symbol operator (`arr.reduce(sym)` where sym is not statically
     known, e.g. a `|sym|` block param): fold with sp_poly_binop_sym over boxed
     operands, accumulating a boxed poly. */
  /* A poly array's elements are already boxed, so even a STATICALLY known
     operator folds through sp_poly_binop_sym -- the static arms below only
     serve the concretely-typed element kinds. This is what lets an array of
     lambdas fold with reduce(:>>) (#2880). */
  int runtime_sym = (!op && block < 0 && argc >= 1 &&
                     (comp_ntype(c, argv[argc - 1]) == TY_SYMBOL ||
                      comp_ntype(c, argv[argc - 1]) == TY_POLY));
  int poly_sym_fold = (k && sp_streq(k, "Poly") && block < 0 && argc >= 1 &&
                       comp_ntype(c, argv[argc - 1]) == TY_SYMBOL);
  /* A seed of a class other than the elements' folds boxed as well. Ruby's
     accumulator IS the seed object and every step is the seed's own operator,
     so `[1, 2, 3].reduce(0.5, :+)` accumulates Float (6.5) where the typed arm
     below truncated 0.5 into its sp_int accumulator and answered 6.0 -- and a
     Rational, Bignum or String seed has no sp_int spelling at all, so the
     generated C did not compile. Either spelling of the operator gets here:
     the symbol node is the one found above, not always a trailing argument. */
  int seed_boxed_fold = (op && init >= 0 && sym_node >= 0 &&
                         (comp_ntype(c, sym_node) == TY_SYMBOL ||
                          comp_ntype(c, sym_node) == TY_POLY) &&
                         !fold_seed_typed(fold_seed_ntype(c, init), et));
  if (runtime_sym || poly_sym_fold || seed_boxed_fold) {
    int symarg = (sym_node >= 0) ? sym_node : argv[argc - 1];
    int rinit = (init >= 0) ? init : ((block < 0 && argc == 2) ? argv[0] : -1);
    int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tsy = ++g_tmp;
    const char *boxfn = et == TY_INT ? "sp_box_int" : et == TY_FLOAT ? "sp_box_float"
                      : et == TY_STRING ? "sp_box_str" : NULL;
    buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
    /* The receiver temp has to be a GC root for the whole fold: the boxed seed
       allocates before the loop and every sp_poly_binop_sym allocates inside
       it, so a receiver built by this same statement was collected out from
       under the walk. The poly-array arm above roots its own for this reason;
       this one never did, which the seeded fold now makes reachable from
       `arr.reduce(Rational(1, 2), :+)`. */
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = sp_%sArray_length(_t%d); sp_sym _t%d = ",
               ta, tn, k, ta, tsy);
    /* a statically-poly operand (a `|sym|` block param over a poly array)
       carries the symbol in its .v.i slot */
    if (comp_ntype(c, symarg) == TY_SYMBOL) emit_expr(c, symarg, b);
    else { buf_puts(b, "(sp_sym)("); emit_expr(c, symarg, b); buf_puts(b, ").v.i"); }
    buf_printf(b, "; sp_RbVal _t%d = ", tacc);
    int start;
    if (rinit >= 0) { emit_boxed(c, rinit, b); start = 0; }
    else {
      buf_printf(b, "_t%d > 0 ? ", tn);
      if (boxfn) buf_printf(b, "%s(sp_%sArray_get(_t%d, 0))", boxfn, k, ta);
      else buf_printf(b, "sp_%sArray_get(_t%d, 0)", k, ta);
      buf_puts(b, " : sp_box_nil()"); start = 1;
    }
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); for (sp_int _t%d = %d; _t%d < _t%d; _t%d++) _t%d = sp_poly_binop_sym(_t%d, _t%d, ",
               tacc, ti, start, ti, tn, ti, tacc, tacc, tsy);
    if (boxfn) buf_printf(b, "%s(sp_%sArray_get(_t%d, _t%d))", boxfn, k, ta, ti);
    else buf_printf(b, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
    buf_printf(b, "); _t%d; })", tacc);
    return 1;
  }
  if (!op) return 0;

  const char *ifn = (et == TY_INT) ? int_arith_fn(op) : NULL;
  /* bitwise ops on integers: &, |, ^, <<, >> -- use operator directly */
  int int_bitop = (et == TY_INT) && !ifn &&
                  (sp_streq(op, "&") || sp_streq(op, "|") || sp_streq(op, "^") ||
                   sp_streq(op, "<<") || sp_streq(op, ">>"));
  int float_op = (et == TY_FLOAT) && (sp_streq(op, "+") || sp_streq(op, "-") ||
                                      sp_streq(op, "*") || sp_streq(op, "/"));
  int str_op = (et == TY_STRING) && sp_streq(op, "+");
  if (!ifn && !int_bitop && !float_op && !str_op) return 0;

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  /* The String arm is the one of the three that allocates between turns:
     every sp_str_concat below builds a fresh String, and the loop takes the
     next element out of this temp after it. An array a method call returned
     has no other holder, so a collection landing in the concat freed it
     mid-walk. Nothing the Integer and Float arms' walk continues past
     allocates, so they keep the bare hoist. The accumulator needs no root of
     its own: sp_str_concat roots its arguments on entry, and nothing runs
     between one turn's answer and the next turn's call. */
  if (str_op) buf_printf(b, "; SP_GC_ROOT(_t%d)", ta);
  buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d); ", tn, k, ta);
  emit_ctype(c, et, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); start = 0; }
  else {
    /* CRuby: a seedless fold over an empty collection is nil */
    const char *mt = (et == TY_INT) ? "SP_INT_NIL"
                   : (et == TY_FLOAT) ? "sp_float_nil()"
                   : (et == TY_STRING) ? "NULL" : default_value(et);
    buf_printf(b, "_t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s", tn, k, ta, mt); start = 1;
  }
  buf_printf(b, "; for (sp_int _t%d = %d; _t%d < _t%d; _t%d++) _t%d = ", ti, start, ti, tn, ti, tacc);
  if (ifn)
    buf_printf(b, "%s(_t%d, sp_%sArray_get(_t%d, _t%d))", ifn, tacc, k, ta, ti);
  /* String#+'s nil checks, inline: a nil accumulator raises NoMethodError
     and a nil element TypeError, as CRuby's do, where sp_str_concat reads a
     nil as "" */
  else if (str_op) {
    int te = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_%sArray_get(_t%d, _t%d); if (!_t%d) sp_nil_recv(\"+\");"
                  " if (!_t%d) sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\");"
                  " sp_str_concat(_t%d, _t%d); })", te, k, ta, ti, tacc, te, tacc, te);
  }
  else /* int_bitop or float direct-op */
    buf_printf(b, "_t%d %s sp_%sArray_get(_t%d, _t%d)", tacc, op, k, ta, ti);
  buf_printf(b, "; _t%d; })", tacc);
  return 1;
}

/* reduce/inject with a block { |acc, elem| body } as an expression.
   Handles typed (non-poly) arrays where both params are scalar. */
int emit_reduce_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (!sp_streq(name, "inject") && !sp_streq(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* a literally-empty receiver never runs the block: the whole call IS the
     init argument, or nil without one -- rendered in the call's own type
     (SP_INT_NIL for an int-typed slot, boxed nil for poly) */
  if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int ren = 0; nt_arr(nt, recv, "elements", &ren);
    if (ren == 0) {
      int rargs = nt_ref(nt, id, "arguments");
      int rac = 0; const int *rav = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &rac) : NULL;
      TyKind selfty = comp_ntype(c, id);
      int has_init = rac >= 1 && rav &&
                     !(nt_type(nt, rav[0]) && sp_streq(nt_type(nt, rav[0]), "SymbolNode"));
      if (has_init) {
        /* an empty {} / [] seed defaults to StrPolyHash / a poly array, but the
           reduce RESULT may have inferred a different concrete variant (e.g.
           PolyPolyHash). On an empty receiver the seed IS the result, so emit
           it in the result's variant or it is read back through the wrong
           struct (#3100 follow-up). */
        const char *ity = nt_type(nt, rav[0]);
        int seed_empty = 0;
        if (ity && (sp_streq(ity, "HashNode") || sp_streq(ity, "ArrayNode"))) {
          int en = 0; nt_arr(nt, rav[0], "elements", &en);
          seed_empty = (en == 0);
        }
        if (seed_empty && ty_is_hash(selfty) && ty_hash_cname(selfty))
          buf_printf(b, "sp_%sHash_new()", ty_hash_cname(selfty));
        else if (seed_empty && selfty == TY_POLY_ARRAY)
          buf_puts(b, "sp_PolyArray_new()");
        else if (seed_empty && ty_is_array(selfty) && array_kind(selfty))
          buf_printf(b, "sp_%sArray_new()", array_kind(selfty));
        else if (selfty == TY_POLY) emit_boxed(c, rav[0], b);
        else emit_expr(c, rav[0], b);
      }
      else if (selfty == TY_INT) buf_puts(b, "SP_INT_NIL");
      else if (selfty == TY_FLOAT) buf_puts(b, "sp_float_nil()");
      else buf_puts(b, "sp_box_nil()");
      return 1;
    }
  }
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  /* `[[ints],...].inject { |a, b| a & b }`: the inner int arrays are boxed in a
     poly array; fold them as int arrays (unboxing each element). The poly array
     itself has no array_kind, so detect this before the typed-array bail. */
  int nested = (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv));
  const char *k = (rt == TY_POLY_ARRAY && !nested) ? "Poly" : array_iter_kind(rt);
  if (!k && !nested) return 0;
  if (nested) k = "Poly";  /* length via sp_PolyArray_length; elements unboxed below */
  TyKind et = nested ? TY_INT_ARRAY : ty_array_elem(rt);
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  /* |acc, (a, b)|: the element (an array, e.g. a hash's [k, v] pair)
     destructures across the second param's leaves */
  int p1_multi = !p1_orig && rt == TY_POLY_ARRAY && block_param_is_multi(c, block, 1);
  /* A block of fewer parameters (`{ 5 }`, `|s|`, `|*|`, `|s, *|`) takes
     what it names of the (accumulator, element) pair and drops the rest,
     as a proc does. A named rest or an optional parameter would collect
     or default the missing ones, which this binding does not do. */
  int short_ok = !block_rest_name(c, block) && !block_opt_name(c, block, 0);
  if ((!p0_orig && !short_ok) || (!p1_orig && !p1_multi && !short_ok)) return 0;
  if (!p0_orig && p1_orig) return 0;
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
  int bbody = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  if (bn == 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int init = (argc > 0 && argv) ? argv[0] : -1;

  /* Accumulator type comes from the seed init when provided, else from the element type. */
  TyKind acc_ty = et;
  int init_empty_arr = 0, init_empty_hash = 0;
  if (init >= 0) {
    TyKind it = comp_ntype(c, init);
    /* an empty array-literal seed accumulates a poly array (mirrors the
       inference rule; the block decides the element mix) */
    if (it == TY_UNKNOWN && nt_type(nt, init) && sp_streq(nt_type(nt, init), "ArrayNode")) {
      int sen = 0; nt_arr(nt, init, "elements", &sen);
      if (sen == 0) { it = TY_POLY_ARRAY; init_empty_arr = 1; }
    }
    /* an empty `{}` seed accumulates a general (boxed key/value) hash so any
       key type the block writes fits -- matches each_with_object({}) (#2958) */
    else if (it == TY_UNKNOWN && nt_type(nt, init) && sp_streq(nt_type(nt, init), "HashNode")) {
      int sen = 0; nt_arr(nt, init, "elements", &sen);
      if (sen == 0) { it = TY_POLY_POLY_HASH; init_empty_hash = 1; }
    }
    if (it != TY_UNKNOWN) acc_ty = it;
    /* a nil seed carries no accumulator type -- TY_NIL emits as `void` -- and
       the fold really is "nothing yet, then whatever the block returns", so
       accumulate boxed (#3356) */
    if (nt_kind(nt, init) == NK_NilNode) acc_ty = TY_POLY;
  }
  /* An int seed folded over floats accumulates float (matches the reduce
     return-type promotion in infer_type); keep the C accumulator type in step.
     Only a numeric body promotes -- a poly body keeps the seed type (codegen
     re-types the params and re-infers the body under the shadow below), and an
     array accumulator never numeric-promotes (the pre-shadow body may have
     typed `a << x` as an int shift). */
  if (!ty_is_array(acc_ty) && !ty_is_hash(acc_ty)) {
    TyKind bt = comp_ntype(c, bb[bn - 1]);
    if (ty_is_numeric(bt)) acc_ty = ty_promote_numeric(acc_ty, bt);
    /* Folding a POLY element array: the block's value is boxed, and the
       seed's slot cannot take it back -- a numeric seed would truncate
       (#2982), an object or value-type seed cannot hold an sp_RbVal at all
       (#2886, served by sp_user_binop_hook). Keep the accumulator boxed. */
    else if (acc_ty != TY_POLY && init >= 0 && et == TY_POLY &&
             (bt == TY_POLY || ty_is_object(bt) || bt == TY_RATIONAL ||
              bt == TY_COMPLEX || bt == TY_BIGINT))
      acc_ty = TY_POLY;
    /* Even over a concretely-typed (int/float) element array, a block whose
       value is boxed (a Rational/Complex, or poly because a fold OPERAND is
       poly -- a parameter called with Integer and Rational call sites) cannot
       fold back into a scalar numeric accumulator slot -- keep the
       accumulator boxed (#3220, #3308). */
    else if (acc_ty != TY_POLY && init >= 0 && ty_is_numeric(acc_ty) &&
             (bt == TY_RATIONAL || bt == TY_COMPLEX || bt == TY_POLY ||
              bt == TY_BIGINT))
      acc_ty = TY_POLY;
    /* With no seed the accumulator starts at the first element and kept that
       type however the block answered. A body that is an UNRESOLVED call --
       `inject(:nope)`, whose value is the NoMethodError raise -- answers the
       boxed token, and assigning it into the scalar slot did not build
       (#3831). Codegen runs after inference settles, so an unknown body type
       here means genuinely unresolved, not not-yet-inferred. */
    else if (acc_ty != TY_POLY && init < 0 && ty_is_numeric(acc_ty) &&
             (bt == TY_POLY || bt == TY_UNKNOWN))
      acc_ty = TY_POLY;
  }
  /* A hash/array/object seed whose block body evaluates to a boxed poly value
     (e.g. a method returning poly because it is also folded in a poly context)
     cannot take that value back into the concrete accumulator slot -- keep the
     accumulator boxed (mirrors the inference widening, #3240). A body that just
     returns the accumulator param (`h[x]=...; h`) keeps the seed's type. */
  if (acc_ty != TY_POLY && init >= 0 &&
      (ty_is_hash(acc_ty) || ty_is_array(acc_ty) || ty_is_object(acc_ty)) &&
      !reduce_tail_from_acc(c, bb[bn - 1], p0_orig) &&
      comp_ntype(c, bb[bn - 1]) == TY_POLY)
    acc_ty = TY_POLY;
  /* A typed-array seed whose block value is BOXED -- `a + r` over poly elements
     runs through the poly adder -- cannot take that value back into its pointer
     slot, and the C compiler rejected the program; an empty `[]` seed escaped
     only because it already accumulates poly (#3854). Probe the body under the
     accumulator shadow the loop installs below, and widen. */
  if (init >= 0 && ty_is_array(acc_ty) && acc_ty != TY_POLY_ARRAY) {
    Scope *psc = comp_scope_of(c, block);
    LocalVar *pl0 = (psc && p0_orig) ? scope_local(psc, p0_orig) : NULL;
    LocalVar *pl1 = (psc && p1_orig) ? scope_local(psc, p1_orig) : NULL;
    TyKind s0 = pl0 ? pl0->type : TY_UNKNOWN, s1 = pl1 ? pl1->type : TY_UNKNOWN;
    if (pl0) pl0->type = acc_ty;
    if (pl1) pl1->type = et;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
    TyKind bt9 = comp_ntype(c, bb[bn - 1]);
    /* a body answering a DIFFERENT array kind widens the same way a boxed one
       does: `reduce([9]) { |a, b| a & b }` over an array of arrays answers a
       poly array, which an int-array accumulator slot cannot hold (#3966) */
    if (bt9 == TY_POLY || (ty_is_array(bt9) && bt9 != acc_ty)) acc_ty = TY_POLY_ARRAY;
    if (pl0) pl0->type = s0;
    if (pl1) pl1->type = s1;
  }
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b); buf_puts(b, "; ");
  /* The loop below re-reads this temp's length as its bound on every turn and
     takes the element out of it on every turn, with the block running in
     between. The array a method call returned has no other holder at all, and
     one held in a named local loses that holder the moment the block rebinds
     the local -- the walk still belongs to the array the fold started on. So
     the hoist is rooted itself, whatever the receiver expression was. No
     needs_root test here, unlike the accumulator below: this emitter has
     already returned 0 unless rt is an array kind, and every array kind needs
     a root and none of them is a value-type object, so the root and its
     separator are always wanted. */
  emit_gc_root_tmp(c, rt, ta, b); buf_puts(b, " ");
  /* --int-overflow=promote: the body is re-inferred under the parameter
     shadow below, and an Integer `+` / `*` in it is typed poly then (it may
     promote), so a numeric accumulator has to be boxed to take it back.
     Look ahead with the same shadow and widen before the slot is declared. */
  if (g_promote_mode && ty_is_numeric(acc_ty) && acc_ty != TY_BIGINT) {
    Scope *lsc = comp_scope_of(c, block);
    LocalVar *l0 = (lsc && p0_orig) ? scope_local(lsc, p0_orig) : NULL;
    LocalVar *l1 = (lsc && p1_orig) ? scope_local(lsc, p1_orig) : NULL;
    TyKind s0 = l0 ? l0->type : TY_UNKNOWN, s1 = l1 ? l1->type : TY_UNKNOWN;
    if (l0) l0->type = acc_ty;
    if (l1) l1->type = et;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
    TyKind bt9 = comp_ntype(c, bb[bn - 1]);
    if (l0) l0->type = s0;
    if (l1) l1->type = s1;
    if (bt9 == TY_POLY || bt9 == TY_BIGINT) acc_ty = TY_POLY;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
  }
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init_empty_arr) {
    /* the empty [] would emit as an IntArray without the poly context; the
       heap accumulator is rooted below, since block-body pushes may collect.
       A slot that widened to poly (the block hands the accumulator to a
       callable, whose result is boxed) takes the boxed form (#3657). */
    if (acc_ty == TY_POLY) buf_puts(b, "sp_box_poly_array(sp_PolyArray_new()); ");
    else buf_puts(b, "sp_PolyArray_new(); ");
    start = 0;
  }
  else if (init_empty_hash) {
    /* the empty {} would emit as its default variant; force the general
       boxed-key/value hash so any key type fits (#2958) */
    if (acc_ty == TY_POLY)
      buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH); ");
    else buf_puts(b, "sp_PolyPolyHash_new(); ");
    start = 0;
  }
  else if (init >= 0) {
    /* a boxed accumulator wants a boxed seed */
    if (acc_ty == TY_POLY && comp_ntype(c, init) != TY_POLY) emit_boxed(c, init, b);
    /* a seed of a narrower array kind than the widened accumulator converts */
    else if (acc_ty == TY_POLY_ARRAY && comp_ntype(c, init) != TY_POLY_ARRAY) {
      buf_puts(b, "sp_poly_to_poly_array("); emit_boxed(c, init, b); buf_puts(b, ")");
    }
    else emit_expr(c, init, b);
    buf_puts(b, "; "); start = 0;
  }
  else if (nested) { buf_printf(b, "sp_PolyArray_length(_t%d) > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", ta, ta); start = 1; }
  else if (acc_ty == TY_POLY && et != TY_POLY) {
    /* a boxed accumulator over a typed element array: box the first element,
       or the two ternary arms disagree about their C type */
    char first[96]; snprintf(first, sizeof first, "sp_%sArray_get(_t%d, 0)", k, ta);
    buf_printf(b, "sp_%sArray_length(_t%d) > 0 ? ", k, ta);
    emit_boxed_text(c, et, first, b);
    buf_puts(b, " : sp_box_nil(); ");
    start = 1;
  }
  else { buf_printf(b, "sp_%sArray_length(_t%d) > 0 ? sp_%sArray_get(_t%d, 0) : %s; ", k, ta, k, ta,
                    acc_ty == TY_INT ? "SP_INT_NIL" : acc_ty == TY_FLOAT ? "sp_float_nil()"
                    : acc_ty == TY_STRING ? "NULL"
                    : acc_ty == TY_POLY ? "sp_box_nil()"
                    : (ty_is_array(acc_ty) || ty_is_object(acc_ty)) ? "NULL" : "0"); start = 1; }
  /* The loop reassigns this slot from a freshly allocated value on every turn,
     and the next turn's block reads it back, so it is a root for the whole
     walk -- as the empty-[] and empty-{} seeds above already were. The root
     records the slot's address, so one push covers every reassignment. The
     test is here rather than left to emit_gc_root_tmp because the separator
     goes with the root: a scalar accumulator gets neither, and neither does a
     value-type object, which lives in the temp itself. */
  if (needs_root(acc_ty) && !comp_ty_value_obj(c, acc_ty)) {
    emit_gc_root_tmp(c, acc_ty, tacc, b); buf_puts(b, " ");
  }
  /* Temporarily override block param types to match acc_ty/et so the body
     expression uses the correct C types (same pattern as emit_sort_cmp_expr). */
  Scope *rsc = comp_scope_of(c, block);
  LocalVar *rlv0 = (rsc && p0_orig) ? scope_local(rsc, p0_orig) : NULL;
  LocalVar *rlv1 = (rsc && p1_orig) ? scope_local(rsc, p1_orig) : NULL;
  TyKind rpt0 = rlv0 ? rlv0->type : TY_UNKNOWN;
  TyKind rpt1 = rlv1 ? rlv1->type : TY_UNKNOWN;
  if (rlv0) rlv0->type = acc_ty;
  if (rlv1) rlv1->type = et;
  for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);  /* refresh ntype cache */
  buf_printf(b, "for (sp_int _t%d = %d; _t%d < sp_%sArray_length(_t%d); _t%d++) { ",
             ti, start, ti, k, ta, ti);
  buf_puts(b, "{ ");
  if (p0) { emit_ctype(c, acc_ty, b); buf_printf(b, " lv_%s = _t%d; ", p0, tacc); }
  if (p1_multi) {
    int te2 = ++g_tmp;
    buf_printf(b, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d); ", te2, ta, ti);
    int lc2 = block_param_multi_count(c, block, 1);
    for (int li = 0; li < lc2; li++) {
      const char *ln = block_param_multi_leaf(c, block, 1, li);
      if (!ln) continue;
      buf_printf(b, "sp_RbVal lv_%s = sp_poly_massign_get(_t%d, %d); (void)lv_%s; ",
                 rename_local(ln), te2, li, rename_local(ln));
    }
  }
  else if (!p1) { }
  else if (nested) { emit_ctype(c, et, b); buf_printf(b, " lv_%s = (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p; ", p1, ta, ti); }
  else { emit_ctype(c, et, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", p1, k, ta, ti); }
  /* `next v` inside a fold block sets the accumulator and moves on, so point
     the next-value channel at the accumulator temp for this body (#3356). The
     for above is a real C loop, so the depth must say so or a `next` at the
     top level of a proc body would take the proc-return path instead. */
  char nx_acc[24]; snprintf(nx_acc, sizeof nx_acc, "_t%d", tacc);
  const char *sv_nxv = g_ie_next_var; int sv_nxp = g_ie_res_poly;
  int sv_cld = g_c_loop_depth;
  int sv_lexcf = g_loop_exc_base, sv_lensf = g_loop_ensure_base;
  g_loop_exc_base = g_exc_frame_depth; g_loop_ensure_base = g_ensure_depth;
  g_ie_next_var = nx_acc; g_ie_res_poly = (acc_ty == TY_POLY);
  g_c_loop_depth++;
  /* block locals are fresh for every step, and a redo re-runs the step
     after the parameters' setup */
  emit_block_locals_reset(c, block, b, 0);
  int rd_lbl = emit_iter_step_stmts(c, nt_ref(nt, block, "body"), b, 0, " ");
  /* The tail's own prelude (a proc call publishing its args to the boxed
     side-channel, a rooted temp) must land INSIDE the loop, before this
     iteration's accumulator assignment: with the enclosing statement's g_pre
     it would run once, ahead of the whole fold, and every iteration would
     reuse the first publication (#2684). Statements are legal here -- we are
     inside the loop body of a statement expression. */
  {
    Buf tail; memset(&tail, 0, sizeof tail);
    Buf *saved_pre = g_pre;
    g_pre = b;
    TyKind rbt = comp_ntype(c, bb[bn - 1]);
    if (rbt == TY_POLY && acc_ty == TY_INT) { buf_puts(&tail, "sp_poly_to_i("); emit_expr(c, bb[bn - 1], &tail); buf_puts(&tail, ")"); }
    else if (rbt == TY_POLY && acc_ty == TY_FLOAT) { buf_puts(&tail, "sp_poly_to_f("); emit_expr(c, bb[bn - 1], &tail); buf_puts(&tail, ")"); }
    else if (rbt == TY_POLY && acc_ty == TY_STRING) { buf_puts(&tail, "sp_poly_to_s("); emit_expr(c, bb[bn - 1], &tail); buf_puts(&tail, ")"); }
    else if (rbt == TY_POLY && acc_ty == TY_SYMBOL) { buf_puts(&tail, "(sp_sym)("); emit_expr(c, bb[bn - 1], &tail); buf_puts(&tail, ").v.i"); }
    /* `acc + elem` on an array accumulator answers a BOXED array (the concat
       runs through the poly adder), which cannot be assigned to the array
       pointer the accumulator slot holds (#3609) */
    else if (rbt == TY_POLY && acc_ty == TY_POLY_ARRAY) {
      buf_puts(&tail, "sp_poly_to_poly_array("); emit_expr(c, bb[bn - 1], &tail); buf_puts(&tail, ")");
    }
    /* The mirror: a concretely typed block value going back into a boxed
       accumulator has to be boxed. Without this a fold with no init over
       Hashes assigned a hash pointer into the sp_RbVal seed slot. */
    else if (acc_ty == TY_POLY && rbt != TY_POLY && rbt != TY_UNKNOWN && rbt != TY_VOID) {
      Buf raw; memset(&raw, 0, sizeof raw); emit_expr(c, bb[bn - 1], &raw);
      emit_boxed_text(c, rbt, raw.p ? raw.p : "0", &tail);
      free(raw.p);
    }
    else emit_expr(c, bb[bn - 1], &tail);
    g_pre = saved_pre;
    buf_printf(b, "_t%d = %s; } } ", tacc, tail.p ? tail.p : "0");
    free(tail.p);
  }
  if (rd_lbl) g_redo_depth--;
  g_c_loop_depth = sv_cld;
  g_loop_exc_base = sv_lexcf; g_loop_ensure_base = sv_lensf;
  g_ie_next_var = sv_nxv; g_ie_res_poly = sv_nxp;
  /* the expression must carry the INFERRED type: a poly-typed reduce
     (e.g. a dyn-send body) boxes its scalar accumulator */
  if (comp_ntype(c, id) == TY_POLY && acc_ty != TY_POLY) {
    char accn[24]; snprintf(accn, sizeof accn, "_t%d", tacc);
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, acc_ty, accn, &bx);
    buf_printf(b, "%s; })", bx.p ? bx.p : accn);
    free(bx.p);
  }
  else buf_printf(b, "_t%d; })", tacc);
  if (rlv0) rlv0->type = rpt0;
  if (rlv1) rlv1->type = rpt1;
  return 1;
}

/* arr.each.with_index(off).<terminal> { ... } : `each.with_index` is a blockless
   enumerator yielding [element, index] pairs, and the chained terminal folds or
   consumes them. The block binds the pair either as two params |v, i| (auto-split),
   a destructured |(v, i)|, or a single |pair| (a 2-element array). inject also
   takes the accumulator as its first param: |acc, (v, i)| / |acc, pair|.
   (matz/spinel#1481 inject/reduce; #1483 map/to_a/select/count/any?/...)

   Returns 0 unless the exact each.with_index chain shape matches, so no other
   call path is affected. */

/* Recognise the chain; fill *out_arr (the source array node) and *out_off (the
   with_index offset arg node, or -1). Returns 1 on match. */
/* True if the block body carries a `next` that is not inside a nested block
   of its own -- that next leaves THIS block, and its value is the block's
   answer. Nested blocks own their own next, so the walk stops at them. */
int fold_body_has_next(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_NextNode) return 1;
  if (k == NK_BlockNode || k == NK_LambdaNode || k == NK_DefNode) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (fold_body_has_next(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int m = 0; const int *ids = nt_arr_at(nt, node, i, &m);
    for (int j = 0; j < m; j++) if (fold_body_has_next(c, ids[j])) return 1;
  }
  return 0;
}

/* A fold that reads a block body as "leading statements, then the tail as the
   answer" drops a `next <value>`: the next leaves the block WITH that value and
   never reaches the tail. Read that way it became a bare `continue` and the
   fold tested the tail instead, so `count { next true if i == 1; false }`
   answered 0 where CRuby answers 1, and find / find_index / take_while lost
   their element the same way (#4324; #4301 is this bug in the any? / all?
   folds).

   When the body carries such a next, emit the whole body here and hand back
   the C truthiness test for its answer -- emit_block_value_into wraps it in
   do{}while(0), so an interior next assigns a slot and falls through to the
   test rather than skipping it. Returns 0 for a body with no next of its own,
   leaving the caller's own emission untouched; a nested block owns its next,
   which is where the walk stops. */
int emit_block_cond_next(Compiler *c, int block, int indent, Buf *out) {
  int body = block >= 0 ? nt_ref(c->nt, block, "body") : -1;
  if (body < 0 || !fold_body_has_next(c, body)) return 0;
  int t = ++g_tmp;
  emit_indent(g_pre, indent);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", t);
  emit_indent(g_pre, indent);
  buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t);
  char dest[24]; snprintf(dest, sizeof dest, "_t%d", t);
  int sv = g_indent; g_indent = indent;
  emit_block_value_into(c, block, dest, 1, indent);
  g_indent = sv;
  buf_printf(out, "sp_poly_truthy(_t%d)", t);
  return 1;
}

/* The statements of block body `body` but its tail, into `b` at `indent`
   (one line each, or, with `sep`, inline, each followed by it), with the
   body's own `redo` label: a redo re-runs the step without the locals'
   reset or the parameter rebindings (block_param_rebind_len), so the label
   goes after them. With no label of its own a redo fell back to
   `continue`, which left the block as `next` does (or jumped to an
   enclosing loop's label). Returns the label, pushed on the redo stack,
   which the caller pops (g_redo_depth--) once the tail is out; 0 when the
   body has no redo of its own. */
int emit_iter_step_stmts(Compiler *c, int body, Buf *b, int indent, const char *sep) {
  const NodeTable *nt = c->nt;
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  int rd_lbl = 0;
  if (body >= 0 && subtree_has_own_redo(nt, body) &&
      g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0])) {
    rd_lbl = ++g_tmp;
    g_redo_owner[g_redo_depth] = body;
    g_redo_stack[g_redo_depth++] = rd_lbl;
  }
  int rd_head = rd_lbl ? block_param_rebind_len(nt, body) : 0;
  for (int j = 0; j <= bn - 1; j++) {
    if (rd_lbl && j == (rd_head < bn - 1 ? rd_head : bn - 1)) {
      if (sep) buf_printf(b, "_redo_%d: ; ", rd_lbl);
      else { emit_indent(b, indent); buf_printf(b, "_redo_%d: ;\n", rd_lbl); }
    }
    if (j == bn - 1) break;
    emit_stmt(c, bb[j], b, indent);
    if (sep) buf_puts(b, sep);
  }
  return rd_lbl;
}

/* A step's whole body as statements, its answer dropped, inside the
   iterator's own C loop (a `next` is its `continue`): the block's locals
   reset, then the body, with its own redo label after that setup. */
void emit_iter_step_body(Compiler *c, int block, Buf *b, int indent) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  emit_block_locals_reset(c, block, b, indent);
  int rd_lbl = emit_iter_step_stmts(c, body, b, indent, NULL);
  if (bn > 0) emit_stmt(c, bb[bn - 1], b, indent);
  if (rd_lbl) g_redo_depth--;
}

/* A step's body inside the iterator's own C loop, through emit_stmts (the
   locals' reset, the statements), with the body's own redo label after
   that setup (g_redo_pending), where it had none. */
void emit_iter_loop_stmts(Compiler *c, int body, Buf *b, int indent) {
  int lbl = 0;
  if (body >= 0 && subtree_has_own_redo(c->nt, body) &&
      g_redo_depth < (int)(sizeof g_redo_stack / sizeof g_redo_stack[0])) {
    lbl = ++g_tmp;
    g_redo_owner[g_redo_depth] = body;
    g_redo_stack[g_redo_depth++] = lbl;
    g_redo_pending = lbl;
  }
  emit_stmts(c, body, b, indent);
  if (lbl) g_redo_depth--;
}

/* Does block `block` need the step's frame: a `next` or a `redo` of its
   own (emit_iter_step_value)? */
int iter_step_needs_frame(Compiler *c, int block) {
  int body = block >= 0 ? nt_ref(c->nt, block, "body") : -1;
  return body >= 0 && (fold_body_has_next(c, body) || subtree_has_own_redo(c->nt, body));
}

/* One step of a builtin iterator's block, as a value, in two parts that
   stand where an emitter read the body as "leading statements, then the
   tail as the answer": emit_iter_step_open emits the statements into g_pre
   at `indent`, and emit_iter_step_tail hands back the C expression of the
   answer, boxed when `want_poly`, with its type.

   A body with a `next` or a `redo` of its own runs whole inside the step's
   frame (emit_block_value_into), its answer landing in a slot: the next
   leaves this step with its value, and the redo re-runs it after the
   parameters' setup. Read the other way, the next was a `continue` that
   skipped the iterator's own work for the step (a sum dropped the value, a
   sort or a gsub never advanced) and the redo had no label. Any other body
   resets the block's locals, which are fresh for every step, then emits its
   leading statements, and its tail is the answer; g_indent, which the
   tail's own setup indents by, is the caller's. */
void emit_iter_step_open(Compiler *c, int block, int want_poly, int indent, IterStep *st) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  TyKind tt = bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_NIL;
  st->block = block; st->want_poly = want_poly; st->slot = 0; st->slot_ty = TY_UNKNOWN;
  if (iter_step_needs_frame(c, block)) {
    /* the slot holds every answer the step can give, a `next`'s too */
    TyKind vt = want_poly || tt == TY_NIL || tt == TY_UNKNOWN || tt == TY_VOID ? TY_POLY : tt;
    st->slot = ++g_tmp; st->slot_ty = vt;
    char dst[32]; snprintf(dst, sizeof dst, "_t%d", st->slot);
    emit_indent(g_pre, indent); emit_ctype(c, vt, g_pre);
    buf_printf(g_pre, " %s = %s;\n", dst, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
    if (vt == TY_POLY) { emit_indent(g_pre, indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(%s);\n", dst); }
    emit_block_value_into(c, block, dst, vt == TY_POLY, indent);
    return;
  }
  emit_block_locals_reset(c, block, g_pre, indent);
  for (int j = 0; j + 1 < bn; j++) emit_stmt(c, bb[j], g_pre, indent);
}

TyKind emit_iter_step_tail(Compiler *c, const IterStep *st, Buf *vb) {
  const NodeTable *nt = c->nt;
  if (st->slot) {
    buf_printf(vb, "_t%d", st->slot);
    return st->slot_ty;
  }
  int body = nt_ref(nt, st->block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn == 0) { buf_puts(vb, "sp_box_nil()"); return TY_POLY; }
  TyKind tt = comp_ntype(c, bb[bn - 1]);
  if (st->want_poly) { emit_boxed(c, bb[bn - 1], vb); return TY_POLY; }
  emit_expr(c, bb[bn - 1], vb);
  return tt;
}

/* The answer of a step opened for a condition (want_poly), as a C truth
   test by Ruby's rules (emit_cond), or, with `raw`, the tail as it is, for
   the emitters that read a typed tail as the test themselves. */
void emit_iter_step_cond(Compiler *c, const IterStep *st, int raw, Buf *cb) {
  if (st->slot) { buf_printf(cb, "sp_poly_truthy(_t%d)", st->slot); return; }
  int body = nt_ref(c->nt, st->block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(c->nt, body, "body", &bn) : NULL;
  if (bn == 0) { buf_puts(cb, "0"); return; }
  if (raw) emit_expr(c, bb[bn - 1], cb);
  else emit_cond(c, bb[bn - 1], cb);
}

static int ewi_chain(Compiler *c, int id, int *out_arr, int *out_off) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  if (nt_ref(nt, recv, "block") >= 0) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn) return 0;
  /* `arr.each_with_index` is the same [elem, index] pair enumerator (offset 0). */
  if (sp_streq(rn, "each_with_index")) {
    int arr = nt_ref(nt, recv, "receiver");
    if (arr < 0) return 0;
    *out_arr = arr; *out_off = -1;
    return 1;
  }
  /* `arr.each.with_index(off)` */
  if (!sp_streq(rn, "with_index")) return 0;
  int wir = nt_ref(nt, recv, "receiver");
  if (wir < 0 || !nt_type(nt, wir) || !sp_streq(nt_type(nt, wir), "CallNode")) return 0;
  const char *en = nt_str(nt, wir, "name");
  if (!en || !sp_streq(en, "each") || nt_ref(nt, wir, "block") >= 0) return 0;
  int arr = nt_ref(nt, wir, "receiver");
  if (arr < 0) return 0;
  int wargs = nt_ref(nt, recv, "arguments");
  int wargc = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
  *out_arr = arr;
  *out_off = (wargc > 0 && wargv) ? wargv[0] : -1;
  return 1;
}

int emit_each_with_index_chain(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_inject = sp_streq(name, "inject") || sp_streq(name, "reduce");
  if (!is_inject) return 0;  /* other terminals handled in a later pass */

  int arr = -1, off = -1;
  if (!ewi_chain(c, id, &arr, &off)) return 0;
  TyKind rt = comp_ntype(c, arr);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind elem_t = ty_array_elem(rt);

  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *p0o = block_param_name(c, block, 0);   /* accumulator */
  if (!p0o) return 0;
  /* pair binding lives in param 1: either a |(v,i)| multi-target or a |pair| name */
  int multi = block_param_is_multi(c, block, 1);
  const char *vo = NULL, *io = NULL, *pairo = NULL;
  if (multi) {
    if (block_param_multi_count(c, block, 1) < 2) return 0;
    vo = block_param_multi_leaf(c, block, 1, 0);
    io = block_param_multi_leaf(c, block, 1, 1);
    if (!vo || !io) return 0;
  }
  else {
    pairo = block_param_name(c, block, 1);
    if (!pairo) return 0;
  }
  int bbody = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  if (bn == 0) return 0;

  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int init = (argc > 0 && argv) ? argv[0] : -1;
  TyKind acc_ty = elem_t;
  if (init >= 0) { TyKind it = comp_ntype(c, init); if (it != TY_UNKNOWN) acc_ty = it; }
  { TyKind bt = comp_ntype(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc_ty = ty_promote_numeric(acc_ty, bt); }
  const char *p0 = rename_local(p0o);
  TyKind pair_ty = (elem_t == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;
  /* --int-overflow=promote: the body is re-inferred under the parameter
     shadow below, where an Integer `+` / `*` types poly (it may promote), so
     a numeric accumulator has to be boxed to take it back: look ahead with
     the same shadow before the slot is declared (see the array inject). */
  if (g_promote_mode && ty_is_numeric(acc_ty) && acc_ty != TY_BIGINT) {
    Scope *lsc = comp_scope_of(c, block);
    LocalVar *l0 = lsc ? scope_local(lsc, p0o) : NULL;
    TyKind s0 = l0 ? l0->type : TY_UNKNOWN; if (l0) l0->type = acc_ty;
    LocalVar *lv9 = NULL, *li9 = NULL, *lp9 = NULL; TyKind sv9 = TY_UNKNOWN, si9 = TY_UNKNOWN, sp9 = TY_UNKNOWN;
    if (multi) {
      lv9 = lsc ? scope_local(lsc, vo) : NULL; li9 = lsc ? scope_local(lsc, io) : NULL;
      sv9 = lv9 ? lv9->type : TY_UNKNOWN; si9 = li9 ? li9->type : TY_UNKNOWN;
      if (lv9) lv9->type = elem_t; if (li9) li9->type = TY_INT;
    }
    else { lp9 = lsc ? scope_local(lsc, pairo) : NULL; sp9 = lp9 ? lp9->type : TY_UNKNOWN; if (lp9) lp9->type = pair_ty; }
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
    TyKind bt9 = comp_ntype(c, bb[bn - 1]);
    if (l0) l0->type = s0;
    if (lv9) lv9->type = sv9; if (li9) li9->type = si9; if (lp9) lp9->type = sp9;
    if (bt9 == TY_POLY || bt9 == TY_BIGINT) acc_ty = TY_POLY;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
  }
  const char *pk = (elem_t == TY_INT) ? "Int" : "Poly";

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, arr, b); buf_puts(b, "; ");
  /* rooted the way emit_reduce_block_expr roots its own hoist: the loop below
     re-reads this temp's length as its bound and takes the element out of it
     every turn, with the block running in between */
  emit_gc_root_tmp(c, rt, ta, b); buf_puts(b, " ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  if (init >= 0 && acc_ty == TY_POLY && comp_ntype(c, init) != TY_POLY) emit_boxed(c, init, b);
  else if (init >= 0) emit_expr(c, init, b);
  else buf_puts(b, acc_ty == TY_POLY ? "sp_box_nil()" : "0");
  buf_puts(b, "; ");
  /* and the accumulator, for the reason emit_reduce_block_expr gives: it is
     rebound to the block's fresh answer every turn, and the next block body
     allocates before it reads it. With only the receiver rooted, a String seed
     came back 3 characters of 112 under GC stress. Same guard as there. */
  if (needs_root(acc_ty) && !comp_ty_value_obj(c, acc_ty)) {
    emit_gc_root_tmp(c, acc_ty, tacc, b); buf_puts(b, " ");
  }
  buf_printf(b, "sp_int _t%d = ", tidx);
  if (off >= 0) emit_expr(c, off, b); else buf_puts(b, "0");
  buf_puts(b, "; ");

  /* Override block-param types so the body expression types correctly, then
     re-infer (same pattern as emit_reduce_block_expr). */
  Scope *rsc = comp_scope_of(c, block);
  LocalVar *lacc = rsc ? scope_local(rsc, p0o) : NULL;
  TyKind sacc = lacc ? lacc->type : TY_UNKNOWN; if (lacc) lacc->type = acc_ty;
  LocalVar *lv = NULL, *li = NULL, *lp = NULL; TyKind sv = TY_UNKNOWN, si = TY_UNKNOWN, sp = TY_UNKNOWN;
  if (multi) {
    lv = rsc ? scope_local(rsc, vo) : NULL; li = rsc ? scope_local(rsc, io) : NULL;
    sv = lv ? lv->type : TY_UNKNOWN; si = li ? li->type : TY_UNKNOWN;
    if (lv) lv->type = elem_t; if (li) li->type = TY_INT;
  }
  else {
    lp = rsc ? scope_local(rsc, pairo) : NULL; sp = lp ? lp->type : TY_UNKNOWN;
    if (lp) lp->type = pair_ty;
  }
  for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);

  buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) { ",
             ti, ti, k, ta, ti, tidx);
  buf_puts(b, "{ ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " lv_%s = _t%d; ", p0, tacc);
  if (multi) {
    emit_ctype(c, elem_t, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", rename_local(vo), k, ta, ti);
    buf_printf(b, "sp_int lv_%s = _t%d; ", rename_local(io), tidx);
  }
  else {
    buf_printf(b, "sp_%sArray *lv_%s = sp_%sArray_new(); ", pk, rename_local(pairo), pk);
    if (elem_t == TY_INT) {
      buf_printf(b, "sp_IntArray_push_nilable(lv_%s, sp_%sArray_get(_t%d, _t%d)); sp_IntArray_push(lv_%s, _t%d); ",
                 rename_local(pairo), k, ta, ti, rename_local(pairo), tidx);
    }
    else {
      buf_printf(b, "sp_PolyArray_push(lv_%s, ", rename_local(pairo));
      char src[96]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, src, &bx); buf_puts(b, bx.p ? bx.p : ""); free(bx.p);
      buf_printf(b, "); sp_PolyArray_push(lv_%s, sp_box_int(_t%d)); ", rename_local(pairo), tidx);
    }
  }
  for (int j = 0; j < bn - 1; j++) { emit_stmt(c, bb[j], b, 0); buf_puts(b, " "); }
  /* The tail is rendered with the prelude pointed INSIDE the loop. Left at
     the caller's g_pre, a tail that hoists -- `acc + [pair]` builds the
     one-element array as statements -- put that construction ahead of the
     loop, where it read the pair local before any iteration had set it. So
     every element folded the same stale value: [[9,9],[0,0],[0,0]] (#4297).
     The loop body is a statement expression, so statements are valid here. */
  { Buf tb; memset(&tb, 0, sizeof tb);
    Buf inner; memset(&inner, 0, sizeof inner);
    Buf *sv_pre = g_pre; g_pre = &inner;
    emit_expr(c, bb[bn - 1], &tb);
    g_pre = sv_pre;
    if (inner.p) buf_puts(b, inner.p);
    buf_printf(b, "_t%d = ", tacc);
    buf_puts(b, tb.p ? tb.p : "0");
    buf_puts(b, "; } } ");
    free(tb.p); free(inner.p); }
  buf_printf(b, "_t%d; })", tacc);

  if (lacc) lacc->type = sacc;
  if (lv) lv->type = sv; if (li) li->type = si; if (lp) lp->type = sp;
  return 1;
}

/* defined below, before emit_collect_expr; used by the each-chain terminals */
void emit_block_value_into(Compiler *c, int block, const char *dest,
                           int want_poly, int indent);

/* The non-fold terminals over arr.each.with_index / arr.each_with_index:
   map/collect (collect block value), select/filter & reject & to_a/entries
   (collect the [v,i] pair), count, any?/all?/none? (scalar), each (side effect,
   returns the receiver). Emits the loop into g_pre; the result tmp lands in `b`.
   Returns 1 if handled. (matz/spinel#1483) */
int emit_each_with_index_terminal(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_map = sp_streq(name, "map") || sp_streq(name, "collect");
  int is_sel = sp_streq(name, "select") || sp_streq(name, "filter") ||
               sp_streq(name, "find_all");
  int is_rej = sp_streq(name, "reject");
  int is_toa = sp_streq(name, "to_a") || sp_streq(name, "entries");
  int is_cnt = sp_streq(name, "count");
  int is_any = sp_streq(name, "any?"), is_all = sp_streq(name, "all?"), is_none = sp_streq(name, "none?");
  int is_each = sp_streq(name, "each");
  int is_toh = sp_streq(name, "to_h");
  if (!(is_map || is_sel || is_rej || is_toa || is_cnt || is_any || is_all || is_none || is_each || is_toh)) return 0;

  int arr = -1, off = -1;
  if (!ewi_chain(c, id, &arr, &off)) return 0;
  TyKind rt = comp_ntype(c, arr);
  /* A union-typed source (e.g. a `= []`-defaulted param, inferred poly) is
     materialized to a poly array, so the [elem, index] pair enumerator drains
     it the same as a typed array with poly elements; a Hash, a Range or an
     Enumerator there gives its items (sp_poly_ewi_items). */
  int poly_src = (rt == TY_POLY);
  if (poly_src) rt = TY_POLY_ARRAY;
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind elem_t = ty_array_elem(rt);

  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (is_toh && block >= 0) return 0;   /* block-form to_h maps each pair; not this {elem => index} lowering */
  if (!is_toa && !is_toh && block < 0) return 0;   /* only to_a/entries/to_h work without a block */

  /* each.with_index yields (element, index) as multiple values, so block-arg
     semantics are ordinary: |v, i| binds both; a single |x| binds the element
     (index discarded); a single destructure |(v,i)| would destructure the
     element (an MRI corner, v=elem/i=nil) -- not worth it, so bail on it. */
  /* Only the unambiguous |v, i| two-param form is handled; a single |x| param
     has method-dependent yield semantics in MRI (map binds the element,
     select binds the pair), so leave those to other rules. */
  const char *vo = NULL, *io = NULL;
  if (block >= 0) {
    if (block_param_is_multi(c, block, 0)) return 0;
    vo = block_param_name(c, block, 0);
    io = block_param_name(c, block, 1);
    if (!vo || !io) return 0;
  }

  int collect_pair = is_sel || is_rej || is_toa;   /* select/reject/to_a collect the [elem,index] pair */
  int need_pair = collect_pair;
  const char *pk = (elem_t == TY_INT) ? "Int" : "Poly";
  TyKind pair_ty = (elem_t == TY_INT) ? TY_INT_ARRAY : TY_POLY_ARRAY;

  int ta = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  int tres = 0, tcnt = 0, tflag = 0;
  TyKind toh_ht = TY_UNKNOWN; const char *toh_hcn = NULL;
  /* Build the receiver expression first: it may push prelude decls (for a
     literal source) that must precede this statement. */
  Buf rb; memset(&rb, 0, sizeof rb);
  if (poly_src) {
    Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, arr, &bx);
    buf_printf(&rb, "sp_poly_ewi_items(%s)", bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
  }
  else emit_expr(c, arr, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = ", tidx);
  if (off >= 0) emit_expr(c, off, g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");

  const char *rk = NULL;
  if (is_map) {
    TyKind restype = comp_ntype(c, id);
    rk = (restype == TY_POLY_ARRAY) ? "Poly" : array_kind(restype);
    if (!rk) rk = "Poly";
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", rk, tres, rk, tres);
  }
  else if (collect_pair) {
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  }
  else if (is_toh) {
    /* each_with_index.to_h builds {element => index}; the key type is the
       element type, the value type is the index (int). */
    toh_ht = comp_ntype(c, id);
    if (!ty_is_hash(toh_ht)) toh_ht = TY_POLY_POLY_HASH;
    toh_hcn = ty_hash_cname(toh_ht);
    tres = ++g_tmp;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", toh_hcn, tres, toh_hcn, tres);
  }
  else if (is_cnt) {
    tcnt = ++g_tmp; emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tcnt);
  }
  else if (is_any || is_all || is_none) {
    tflag = ++g_tmp; emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = %d;\n", tflag, (is_all || is_none) ? 1 : 0);
  }

  /* override block-param types so the body expression types correctly */
  Scope *bsc = block >= 0 ? comp_scope_of(c, block) : NULL;
  LocalVar *lv = NULL, *li = NULL; TyKind sv = TY_UNKNOWN, si = TY_UNKNOWN;
  if (block >= 0) {
    lv = scope_local(bsc, vo); sv = lv ? lv->type : TY_UNKNOWN; if (lv) lv->type = elem_t;
    li = scope_local(bsc, io); si = li ? li->type : TY_UNKNOWN; if (li) li->type = TY_INT;
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
  }

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n", ti, ti, k, ta, ti, tidx);
  int din = g_indent + 1;

  int tpair = 0;
  if (need_pair) {
    tpair = ++g_tmp;
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", pk, tpair, pk, tpair);
    if (elem_t == TY_INT) {
      emit_indent(g_pre, din);
      buf_printf(g_pre, "sp_IntArray_push_nilable(_t%d, sp_%sArray_get(_t%d, _t%d)); sp_IntArray_push(_t%d, _t%d);\n", tpair, k, ta, ti, tpair, tidx);
    }
    else {
      emit_indent(g_pre, din); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tpair);
      char src[96]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, src, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      buf_printf(g_pre, "); sp_PolyArray_push(_t%d, sp_box_int(_t%d));\n", tpair, tidx);
    }
  }
  if (block >= 0) {
    emit_indent(g_pre, din); emit_ctype(c, elem_t, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", rename_local(vo), k, ta, ti);
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_int lv_%s = _t%d;\n", rename_local(io), tidx);
  }

  int body = block >= 0 ? nt_ref(nt, block, "body") : -1;
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* fresh block-locals per iteration (this emitter walks the body itself
     instead of going through emit_stmts, so reset explicitly) */
  if (block >= 0) emit_block_locals_reset(c, block, g_pre, din);
  if (is_each) {
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, din);
  }
  else if (collect_pair && block < 0) {   /* to_a / entries */
    emit_indent(g_pre, din); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tres);
    Buf bx; memset(&bx, 0, sizeof bx); char pe[32]; snprintf(pe, sizeof pe, "_t%d", tpair); emit_boxed_text(c, pair_ty, pe, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    buf_puts(g_pre, ");\n");
  }
  else if (is_toh) {   /* to_h: set {element => index} directly into the hash */
    emit_indent(g_pre, din);
    TyKind kty = ty_hash_key(toh_ht), vty = ty_hash_val(toh_ht);
    char keyexpr[96]; snprintf(keyexpr, sizeof keyexpr, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
    buf_printf(g_pre, "sp_%sHash_set(_t%d, ", toh_hcn, tres);
    if (kty == TY_POLY) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, elem_t, keyexpr, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
    else buf_puts(g_pre, keyexpr);
    if (vty == TY_POLY) buf_printf(g_pre, ", sp_box_int(_t%d));\n", tidx);
    else buf_printf(g_pre, ", _t%d);\n", tidx);
  }
  else {   /* map / select / reject / count / any? / all? / none? -- need the block value */
    /* collect the block's value (next-aware) into a temp so an interior or tail
       `next <v>` contributes <v> rather than being dropped as a skip. */
    /* An empty block body (e.g. `map { |x, i| }`) has bn == 0, so guard the
       bb[bn - 1] read and treat the absent tail value as nil -- a poly value so
       the temp initializes to sp_box_nil() rather than an integer 0. */
    TyKind bt = bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_NIL;
    /* A body whose value IS nil types VOID here, and `void _tN` is not a
       declaration -- `select { nil }` never compiled, and `select {}` reaches
       the same place now that an empty body carries the nil it means (#4006).
       nil is a value in this slot, not the absence of one. */
    if (bt == TY_VOID) bt = TY_NIL;
    if (bt == TY_UNKNOWN) bt = TY_INT;
    int vpoly = (bt == TY_POLY || bt == TY_NIL);
    int tv = ++g_tmp; char tvb[24]; snprintf(tvb, sizeof tvb, "_t%d", tv);
    emit_indent(g_pre, din);
    if (vpoly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, bt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(bt)); }
    emit_block_value_into(c, block, tvb, vpoly, din);
    /* Ruby truthiness of the block value: only nil/false are falsy. C
       zero-falsiness ("(_tN)") would wrongly drop a numeric 0 / 0.0 (or an
       empty string), which Ruby keeps -- so mirror emit_cond's per-type test
       on the temp: a boxed poly goes through sp_poly_truthy, a nilable scalar
       tests its sentinel, and every other concrete value is always truthy. */
    char truth[48];
    if (vpoly)                        snprintf(truth, sizeof truth, "sp_poly_truthy(_t%d)", tv);
    else if (bt == TY_BOOL)           snprintf(truth, sizeof truth, "(_t%d)", tv);
    else if (bt == TY_INT)            snprintf(truth, sizeof truth, "(_t%d != SP_INT_NIL)", tv);
    else if (bt == TY_FLOAT)          snprintf(truth, sizeof truth, "(!sp_float_is_nil(_t%d))", tv);
    else if (bt == TY_SYMBOL)         snprintf(truth, sizeof truth, "(_t%d != (sp_sym)-1)", tv);
    else if (comp_ty_value_obj(c, bt)) snprintf(truth, sizeof truth, "1");
    else if (bt == TY_STRING || ty_is_array(bt) || ty_is_hash(bt) || ty_is_object(bt) ||
             bt == TY_PROC || bt == TY_MATCHDATA ||
             bt == TY_EXCEPTION || bt == TY_BIGINT || bt == TY_REGEX)
                                      snprintf(truth, sizeof truth, "(_t%d != 0)", tv);
    else                              snprintf(truth, sizeof truth, "1");  /* concrete value: always truthy */
    if (is_map) {
      emit_indent(g_pre, din); buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
      if (sp_streq(rk, "Poly") && !vpoly) { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, bt, tvb, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
      else buf_puts(g_pre, tvb);
      buf_puts(g_pre, ");\n");
    }
    else if (is_sel || is_rej) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s%s) sp_PolyArray_push(_t%d, ", is_rej ? "!" : "", truth, tres);
      Buf bx; memset(&bx, 0, sizeof bx); char pe[32]; snprintf(pe, sizeof pe, "_t%d", tpair); emit_boxed_text(c, pair_ty, pe, &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      buf_puts(g_pre, ");\n");
    }
    else if (is_cnt) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) _t%d++;\n", truth, tcnt);
    }
    else if (is_any) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) { _t%d = 1; break; }\n", truth, tflag);
    }
    else if (is_none) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (%s) { _t%d = 0; break; }\n", truth, tflag);
    }
    else if (is_all) {
      emit_indent(g_pre, din); buf_printf(g_pre, "if (!%s) { _t%d = 0; break; }\n", truth, tflag);
    }
  }

  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

  if (lv) lv->type = sv; if (li) li->type = si;

  char res[32]; TyKind res_t;
  if (is_map || collect_pair || is_toh) {
    snprintf(res, sizeof res, "_t%d", tres);
    res_t = is_toh ? toh_ht : collect_pair || sp_streq(rk, "Poly") ? TY_POLY_ARRAY : comp_ntype(c, id);
  }
  else if (is_cnt) { snprintf(res, sizeof res, "_t%d", tcnt); res_t = TY_INT; }
  else if (is_any || is_all || is_none) { snprintf(res, sizeof res, "_t%d", tflag); res_t = TY_BOOL; }
  else { snprintf(res, sizeof res, "_t%d", ta); res_t = rt; }   /* each -> receiver */
  /* A call the node types boxed -- a user class that also defines map,
     select, count... makes the name poly (#6293) -- answers the result
     boxed; left raw, a typed array went out through an sp_RbVal return. */
  if (comp_ntype(c, id) == TY_POLY && res_t != TY_POLY) emit_boxed_text(c, res_t, res, b);
  else buf_puts(b, res);
  return 1;
}

/* sort_by { |x| key } as an expression: a stable bubble sort of a copy of
   the receiver, ordering by the block's computed (scalar) key. Returns 1 if
   handled. */
int emit_sortby_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = sp_streq(name, "sort_by!");
  if (!sp_streq(name, "sort_by") && !is_bang) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind kt = comp_ntype(c, bb[bn - 1]);
  /* an unresolved key (a symbol-proc naming a method the element does not
     have) is the raising token: sort as poly so the call compiles and raises */
  if (kt == TY_UNKNOWN && block_tail_is_unresolved(c, bb[bn - 1])) kt = TY_POLY;
  /* scalar, poly, symbol, or ARRAY key (the multi-key sort idiom
     `sort_by { [a, b] }` -- sp_poly_cmp orders boxed arrays element-wise) */
  /* A key whose value IS nil types VOID/NIL. It still boxes (emit_boxed_text
     evaluates the expression and yields nil), and nil ties with nil, so the
     sort is the stable identity -- which is what CRuby answers (#4006). */
  /* A Rational or a Bignum key is a comparable number carried as a pointer.
     The keys are boxed here regardless, and sp_poly_cmp orders both, so the
     only thing refusing them did was drop the call to the unresolved-call
     raise: NoMethodError for `sort_by` on an Array (#4061). */
  if (kt != TY_INT && kt != TY_FLOAT && kt != TY_STRING && kt != TY_POLY &&
      kt != TY_SYMBOL && kt != TY_VOID && kt != TY_NIL && kt != TY_BOOL &&
      kt != TY_RATIONAL && kt != TY_BIGINT && !ty_is_array(kt)) return 0;

  /* Schwartzian transform: compute each element's sort key exactly once (CRuby
     semantics -- the old bubble sort re-ran the block per comparison), stable-sort
     the indices by key, then gather the elements in sorted order. Non-mutating:
     the receiver is read by sorted index into a fresh result, never reordered. */
  int trv = ++g_tmp, tn = ++g_tmp, tkeys = ++g_tmp, tidx = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tg = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trv, rb.p ? rb.p : ""); free(rb.p);
  /* root the receiver: the key loop allocates (boxing keys, growing the key/index
     arrays), so a freshly-built receiver held only here must survive a mid-build GC */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
  /* boxed keys (rooted so they survive later iterations' allocations) + indices */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tidx, tidx);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
  int np_sb = 0; while (block_param_name(c, block, np_sb)) np_sb++;
  /* a block of any other shape than plain requireds, over elements known
     only at run time, binds the element by the proc distribution */
  if (rt == TY_POLY_ARRAY && block_binds_gathered(c, block)) {
    char vals[128];
    snprintf(vals, sizeof vals, "sp_yielded_args(0, sp_PolyArray_get(_t%d, _t%d))", trv, ti);
    emit_boxed_step_binds(c, block, vals, g_pre, g_indent + 1, 0);
  }
  else if (np_sb >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0)) {
    /* 2-param auto-splat: |name, age| over a poly array of sub-arrays. */
    int te = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te, trv, ti);
    emit_autosplat_params(c, block, np_sb, te, g_indent + 1);
  }
  else if (p0) {
    /* a boxed element into a parameter the analysis typed (the receiver a
       poly array only here, read back from a box) is unboxed */
    Scope *sbs = comp_scope_of(c, block);
    LocalVar *plv = sbs ? scope_local(sbs, p0_orig) : NULL;
    TyKind pt = plv ? plv->type : TY_UNKNOWN;
    char src[96]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, trv, ti);
    emit_indent(g_pre, g_indent + 1);
    if (rt == TY_POLY_ARRAY && pt != TY_POLY && pt != TY_UNKNOWN) emit_block_param_from_boxed(c, p0, pt, src, g_pre);
    else buf_printf(g_pre, "lv_%s = %s;\n", p0, src);
  }
  IterStep st; emit_iter_step_open(c, block, 0, g_indent + 1, &st);
  int save = g_indent; g_indent += 1;
  Buf kb; memset(&kb, 0, sizeof kb);
  if (emit_iter_step_tail(c, &st, &kb) == TY_POLY) kt = TY_POLY;
  g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", tkeys);
  if (kt == TY_POLY) buf_puts(g_pre, kb.p ? kb.p : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, kt, kb.p ? kb.p : "0", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
  buf_puts(g_pre, ");\n"); free(kb.p);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_IntArray_push(_t%d, _t%d);\n", tidx, ti);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_sort_idx_by_poly(_t%d->data + _t%d->start, _t%d->data, _t%d);\n", tidx, tidx, tkeys, tn);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", tres, k, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)\n", tg, tg, tn, tg);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, sp_IntArray_get(_t%d, _t%d)));\n", k, tres, k, trv, tidx, tg);
  /* the same elements, reordered: the receiver's nils come along */
  if (sp_streq(k, "Int") || sp_streq(k, "Float")) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray_nil_from(_t%d, _t%d);\n", k, tres, trv);
  }
  if (is_bang) {
    /* sort_by!: write the gathered order back through the receiver pointer
       (aliases observe it) and yield the receiver -- CRuby returns self.
       The gather copy is needed anyway: writing in place while reading by
       sorted index would clobber the source. */
    int tw = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)\n", tw, tw, tn, tw);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, sp_%sArray_get(_t%d, _t%d));\n", k, trv, tw, k, tres, tw);
    buf_printf(b, "_t%d", trv);
    return 1;
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* sort { |a, b| a <=> b } as an expression: stable bubble sort of a copy,
   ordered by the comparator block (which yields the <=> sign). Returns 1 if
   handled. */
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = sp_streq(name, "sort!");
  if (!sp_streq(name, "sort") && !is_bang) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* Hash#sort { |x, y| ... }: sort the [key, value] pair list (a poly array).
     sort! is not valid on a Hash, so only the non-bang form applies. */
  int hash_sort = ty_is_hash(rt) && !is_bang;
  if (!ty_is_array(rt) && !hash_sort) return 0;
  const char *k = hash_sort ? "Poly" : (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = hash_sort ? TY_POLY : ty_array_elem(rt);
  const char *hn = hash_sort ? ty_hash_cname(rt) : NULL;
  TyKind eff_rt = hash_sort ? TY_POLY_ARRAY : rt;
  /* A comparator block of fewer than two parameters still runs: CRuby hands
     it the first of the two values, or none. Standing down here let the
     blockless arm run without it, and the block vanished from the C. */
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (p0) p0 = rename_local(p0);
  if (p1) p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* the comparator answers the <=> sign; a boxed answer -- a poly element, or
     a user <=> anywhere in the program -- is unwrapped (#3622) */
  if (bn < 1) return 0;
  TyKind cmp_ty = comp_ntype(c, bb[bn - 1]);
  if (cmp_ty != TY_INT && cmp_ty != TY_POLY) return 0;
  const char *cmp_o = cmp_ty == TY_POLY ? "sp_poly_to_i(" : "(";
  int trv = ++g_tmp, tr = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  if (hash_sort) emit_hash_pairs_expr(c, recv, rt, hn, &rb); else emit_expr(c, recv, &rb);
  rt = eff_rt;
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  if (!is_bang) {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sArray_slice(_t%d, 0, sp_%sArray_length(_t%d));\n", tr, k, trv, k, trv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  }
else {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = _t%d;\n", tr, trv);  /* sort! operates on self */
    /* rooted like the copy the non-bang arm sorts: the comparator below is
       user code, and the array it sorts in place has no other holder when
       the receiver was a temporary */
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  }
  /* Bottom-up merge sort: stable, and O(n log n) comparisons. (A bubble sort
     with the comparator inlined was quadratic -- a 40K-element sort took five
     seconds.) The scratch buffer only ever holds elements the array still
     holds too, so a collection during the comparator cannot lose one. */
  int tw = ++g_tmp, tlo = ++g_tmp, tmid = ++g_tmp, thi = ++g_tmp, to = ++g_tmp, tbuf = ++g_tmp, tc = ++g_tmp;
  Buf ect; memset(&ect, 0, sizeof ect); emit_ctype(c, et, &ect);
  const char *ecs = ect.p ? ect.p : "sp_int";
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, tr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "%s *_t%d = _t%d > 1 ? (%s *)malloc(sizeof(%s) * (size_t)_t%d) : NULL;\n", ecs, tbuf, tn, ecs, ecs, tn);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 1; _t%d && _t%d < _t%d; _t%d *= 2)\n", tw, tbuf, tw, tn, tw);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d += 2 * _t%d) {\n", tlo, tlo, tn, tlo, tw);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_int _t%d = _t%d + _t%d; if (_t%d > _t%d) _t%d = _t%d;\n", tmid, tlo, tw, tmid, tn, tmid, tn);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_int _t%d = _t%d + 2 * _t%d; if (_t%d > _t%d) _t%d = _t%d;\n", thi, tlo, tw, thi, tn, thi, tn);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "if (_t%d >= _t%d) continue;\n", tmid, thi);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_int _t%d = _t%d, _t%d = _t%d, _t%d = _t%d;\n", ti, tlo, tj, tmid, to, tlo);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "while (_t%d < _t%d && _t%d < _t%d) {\n", ti, tmid, tj, thi);
  emit_indent(g_pre, g_indent + 3); buf_printf(g_pre, "%s _t%d = sp_%sArray_get(_t%d, _t%d);\n", ecs, ta, k, tr, ti);
  emit_indent(g_pre, g_indent + 3); buf_printf(g_pre, "%s _t%d = sp_%sArray_get(_t%d, _t%d);\n", ecs, tb, k, tr, tj);
  Scope *sbsc = comp_scope_of(c, block);
  LocalVar *slv0 = (sbsc && p0) ? scope_local(sbsc, p0) : NULL;
  LocalVar *slv1 = (sbsc && p1) ? scope_local(sbsc, p1) : NULL;
  TyKind spt0 = slv0 ? slv0->type : TY_UNKNOWN;
  TyKind spt1 = slv1 ? slv1->type : TY_UNKNOWN;
  if (slv0) slv0->type = et;
  if (slv1) slv1->type = et;
  for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);  /* refresh ntype cache */
  int save = g_indent; g_indent += 3;
  /* Shadow the outer (possibly poly) block params with et-typed locals */
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
  emit_indent(g_pre, g_indent);
  if (p0) { emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, ta); }
  if (p1) { emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;", p1, tb); }
  buf_puts(g_pre, "\n");
  IterStep st; emit_iter_step_open(c, block, 0, g_indent, &st);
  Buf cb; memset(&cb, 0, sizeof cb); emit_iter_step_tail(c, &st, &cb);
  emit_indent(g_pre, g_indent);
  /* take from the left on a tie, so equal elements keep their order */
  buf_printf(g_pre, "sp_int _t%d = %s%s);\n", tc, cmp_o, cb.p ? cb.p : "0"); free(cb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if (_t%d > 0) { _t%d[_t%d++] = _t%d; _t%d++; }\nelse { _t%d[_t%d++] = _t%d; _t%d++; }\n",
             tc, tbuf, to, tb, tj, tbuf, to, ta, ti);
  g_indent--; g_indent = save;
  emit_indent(g_pre, g_indent + 3); buf_puts(g_pre, "}\n");
  if (slv0) slv0->type = spt0;
  if (slv1) slv1->type = spt1;
  emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");   /* while merge */
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "while (_t%d < _t%d) _t%d[_t%d++] = sp_%sArray_get(_t%d, _t%d++);\n", ti, tmid, tbuf, to, k, tr, ti);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "while (_t%d < _t%d) _t%d[_t%d++] = sp_%sArray_get(_t%d, _t%d++);\n", tj, thi, tbuf, to, k, tr, tj);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "for (sp_int _q = _t%d; _q < _t%d; _q++) sp_%sArray_set(_t%d, _q, _t%d[_q]);\n", tlo, thi, k, tr, tbuf);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");   /* for lo */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "free(_t%d);\n", tbuf);
  free(ect.p);
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* Emit "lv_<nm> = _t<tidx>" with boxing if the outer local is TY_POLY
   but the element type et is scalar (string, int, float, bool). */
void emit_block_param_assign(Compiler *c, int scope_id, const char *nm, int tidx, TyKind et, Buf *b) {
  Scope *sc = comp_scope_of(c, scope_id);
  LocalVar *lv = sc ? scope_local(sc, nm) : NULL;
  int box = lv && lv->type == TY_POLY && et != TY_POLY;
  if (box) {
    switch (et) {
    case TY_INT:    buf_printf(b, "lv_%s = sp_box_int(_t%d);", nm, tidx); break;
    case TY_STRING: buf_printf(b, "lv_%s = sp_box_str(_t%d);", nm, tidx); break;
    case TY_FLOAT:  buf_printf(b, "lv_%s = sp_box_float(_t%d);", nm, tidx); break;
    case TY_BOOL:   buf_printf(b, "lv_%s = sp_box_bool(_t%d);", nm, tidx); break;
    default:        buf_printf(b, "lv_%s = _t%d;", nm, tidx); break;
    }
  }
else {
    buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
}

/* min / max { |a, b| a <=> b } as an expression: a single scan tracking the
   extreme under the comparator block. minmax's own block form is written in
   Ruby (builtins/enumerable.rb): every receiver desugar_builtin_enum_calls
   accepts (Array, Hash, an Enumerable includer, and a Range even though its
   BLOCKLESS form stays here, since a custom comparator forces an each-walk
   CRuby's own Range#minmax cannot avoid either) is rewritten to a generic
   clone before codegen ever sees a "minmax" node. Returns 1 if handled. */
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_min = sp_streq(name, "min"), is_max = sp_streq(name, "max");
  if (!is_min && !is_max) return 0;
  /* This lowers only the no-argument comparator form (one extreme element).
     `min(n)`/`max(n)` with a block takes the n extremes by the comparator and
     is not lowered; let it fall through to a clean reject rather than emitting
     a single-element scalar that silently ignores n. */
  int mm_args = nt_ref(nt, id, "arguments");
  if (mm_args >= 0) { int mm_argc = 0; nt_arr(nt, mm_args, "arguments", &mm_argc); if (mm_argc > 0) return 0; }
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);   /* cache read: the mirror override below must stick */
  /* a hash receiver rides the pair-array redispatch: materialize the [k, v]
     pairs and re-enter with the receiver overridden (same mirror pattern as
     the emit_range_call tail) */
  if (ty_is_hash(rt) && hash_enum_redispatch(c, id) && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_enum_items_from(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_POLY_ARRAY);
    int handled = emit_minmax_cmp_expr(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return handled;
  }
  /* a range receiver materializes to its int array once and re-enters with
     the receiver overridden, so the comparator loop below serves it */
  if (rt == TY_RANGE && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp, tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); }); SP_GC_ROOT(_t%d);\n",
               ta, tr, rb.p ? rb.p : "", tr, ta);
    free(rb.p);
    view_bind(recv, "_t%d", ta);
    int v = view_push(c, recv, TY_INT_ARRAY);
    int handled = emit_minmax_cmp_expr(c, id, b);
    view_pop(c, v);
    view_unbind(g_n_argov - 1);
    return handled;
  }
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  /* A comparator block of fewer than two parameters still runs: CRuby hands
     it the first of the two values, or none. Standing down here let the
     blockless arm run without it, and the block vanished from the C. */
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (p0) p0 = rename_local(p0);
  if (p1) p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  /* the comparator answers the <=> sign; a boxed answer -- a poly element, or
     a user <=> anywhere in the program -- is unwrapped (#3622) */
  if (bn < 1) return 0;
  TyKind cmp_ty = comp_ntype(c, bb[bn - 1]);
  if (cmp_ty != TY_INT && cmp_ty != TY_POLY) return 0;
  const char *cmp_o = cmp_ty == TY_POLY ? "sp_poly_to_i(" : "(";
  int trv = ++g_tmp, tn = ++g_tmp, tmin = ++g_tmp, tmax = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;
  /* the length is hoisted once, but every turn takes its element out of this
     temp after the comparator has run, and the comparator is user code that
     allocates: rooted. A range or hash receiver arrives here already rooted
     by the arm above that materialized it, and takes a second slot. */
  emit_pre_rooted_recv(c, recv, rt, trv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  /* An empty comparator reduction returns nil, so use the carrier's nil
     sentinel rather than the ordinary zero default for scalar elements. */
  buf_printf(g_pre, " _t%d = _t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s;\n", tmin, tn, k, trv,
             et == TY_INT ? "SP_INT_NIL" : et == TY_FLOAT ? "sp_float_nil()" :
             et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = _t%d;\n", tmax, tmin);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (sp_int _t%d = 1; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", te, k, trv, ti);
  /* Block params may be widened to TY_POLY across multiple block sites.
     Pin them to the element type for body emission:
     - temporarily set scope types to `et`
     - refresh ntype cache for body nodes (infer_type writes to cache)
     - emit C shadow declarations inside { } to give lv_p0/lv_p1 the right C type */
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv_p0 = (bsc && p0) ? scope_local(bsc, p0) : NULL;
  LocalVar *lv_p1 = (bsc && p1) ? scope_local(bsc, p1) : NULL;
  TyKind saved_p0 = lv_p0 ? lv_p0->type : TY_UNKNOWN;
  TyKind saved_p1 = lv_p1 ? lv_p1->type : TY_UNKNOWN;
  if (lv_p0) lv_p0->type = et;
  if (lv_p1) lv_p1->type = et;
  for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);  /* refresh cache */
  int save = g_indent; g_indent++;
  int tacc = is_min ? tmin : tmax;
  /* Open C shadow scope with et-typed block param vars */
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
  emit_indent(g_pre, g_indent);
  if (p0) { emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, te); }
  if (p1) { emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;", p1, tacc); }
  buf_puts(g_pre, "\n");
  IterStep st; emit_iter_step_open(c, block, 0, g_indent, &st);
  Buf cm; memset(&cm, 0, sizeof cm); emit_iter_step_tail(c, &st, &cm);
  g_indent--;
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (%s%s) %c 0) _t%d = _t%d;\n", cmp_o, cm.p ? cm.p : "0", is_min ? '<' : '>', tacc, te); free(cm.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (lv_p0) lv_p0->type = saved_p0;
  if (lv_p1) lv_p1->type = saved_p1;
  g_indent = save;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", is_min ? tmin : tmax);
  return 1;
}


/* Emit a block body so its Ruby value lands in the already-declared C lvalue
   `dest`. Interior or tail `next <v>` assign `dest` (boxing when want_poly)
   then `continue`; a plain tail expression assigns `dest`. The caller declares
   `dest`, owns the surrounding loop, and consumes `dest` afterwards (push it for
   map, test its truthiness for select). Emits into g_pre at `indent`. This is
   the shared substrate that makes `next <value>` work inside a collecting
   block instead of dropping the value. */
void emit_block_value_into(Compiler *c, int block, const char *dest,
                           int want_poly, int indent) {
  const NodeTable *nt = c->nt;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  const char *sv_nx = g_ie_next_var; int sv_poly = g_ie_res_poly;
  TyKind sv_nty = g_ie_next_ty;
  int sv_lexc = g_loop_exc_base; g_loop_exc_base = g_exc_frame_depth;
  g_ie_next_var = dest; g_ie_res_poly = want_poly;
  /* The destination holds whatever the block answers, and the TAIL is what the
     inference typed for that -- so an empty `[]` reached through `next` is
     built at the same kind rather than its own untyped default (#3978). */
  /* A `then` whose value joined a `next` arm of another array kind names the
     slot's kind itself: the poly array, which the tail converts to below
     (#4747). */
  TyKind dest_ty = g_bv_dest_ty; g_bv_dest_ty = TY_UNKNOWN;
  g_ie_next_ty = TY_UNKNOWN;
  if (!want_poly && bn > 0) {
    TyKind dt = dest_ty != TY_UNKNOWN ? dest_ty : comp_ntype(c, bb[bn - 1]);
    if (ty_is_array(dt) || ty_is_hash(dt)) g_ie_next_ty = dt;
    /* an Integer or Float slot: a `next nil` spells the slot's sentinel */
    else if (dt == TY_INT || dt == TY_FLOAT) g_ie_next_ty = dt;
  }
  int sv_lensd = g_loop_ensure_base; g_loop_ensure_base = g_ensure_depth;
  g_c_loop_depth++;   /* the do{}while(0) wrapper makes `continue` valid */
  int sd = g_indent;
  /* Wrap the body in do{}while(0): an interior or tail `next <v>` assigns
     `dest` (via g_ie_next_var) then emits `continue`, which against while(0)
     exits this wrapper and falls through to the caller's collection rather
     than skipping it (a bare `continue` of the host loop would drop the
     value). Bodies without a next still run once -- the wrapper is free. */
  emit_indent(g_pre, indent); buf_puts(g_pre, "do {\n");
  int bi = indent + 1; g_indent = bi;
  /* fresh block-locals on every invocation (this path bypasses emit_stmts) */
  emit_block_locals_reset(c, block, g_pre, bi);
  int rd_lbl = emit_iter_step_stmts(c, body, g_pre, bi, NULL);
  if (bn > 0) {
    int tail = bb[bn - 1];
    const char *tty = nt_type(nt, tail);
    /* a control-flow tail (next/break/return/redo) is emitted as a statement;
       its own lowering writes `dest` where it carries a value. */
    int is_cf = tty && (sp_streq(tty, "NextNode") || sp_streq(tty, "BreakNode") ||
                        sp_streq(tty, "ReturnNode") || sp_streq(tty, "RedoNode"));
    if (is_cf) emit_stmt(c, tail, g_pre, bi);
    else {
      /* Emit the value into its own buffer so the tail's own preludes (e.g. a
         nested map's loop) flow to g_pre ahead of the assignment line rather
         than splicing into it. */
      TyKind tt = comp_ntype(c, tail);
      Buf vb; memset(&vb, 0, sizeof vb);
      /* a typed-array tail into the poly-array slot a `next` arm widened */
      const char *apf = (g_ie_next_ty == TY_POLY_ARRAY && tt != TY_POLY_ARRAY) ? array_to_poly_fn(tt) : NULL;
      if (want_poly && tt != TY_POLY) emit_boxed(c, tail, &vb);
      else if (apf) { buf_printf(&vb, "%s(", apf); emit_expr(c, tail, &vb); buf_puts(&vb, ")"); }
      else emit_expr(c, tail, &vb);
      emit_indent(g_pre, bi);
      buf_printf(g_pre, "%s = %s;\n", dest, vb.p ? vb.p : "");
      free(vb.p);
    }
  }
  if (rd_lbl) g_redo_depth--;
  g_indent = sd;
  emit_indent(g_pre, indent); buf_puts(g_pre, "} while (0);\n");
  g_c_loop_depth--;
  g_ie_next_var = sv_nx; g_ie_res_poly = sv_poly; g_loop_exc_base = sv_lexc;
  g_loop_ensure_base = sv_lensd;
  g_ie_next_ty = sv_nty;
}

/* map/select/reject/filter as an expression: build a result array via a
   loop emitted into the statement prelude; the expression value is the
   temp array. Returns 1 if handled. */
int emit_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  /* `arr.map(&blk)` inside `def m(&blk)`: a BlockArgumentNode that forwards
     the current method's block param maps over the caller's (already-inlined)
     literal block rather than treating it as an empty (nil-producing) block.
     Only a forward of the active block param is redirected -- `&:sym` and
     `&proc_value` are left to their own handlers. */
  if (nt_type(nt, block) && sp_streq(nt_type(nt, block), "BlockArgumentNode")) {
    int fwd_expr = nt_ref(nt, block, "expression");
    int forwards_param = 0;
    if (fwd_expr < 0) forwards_param = 1;  /* anonymous `&` */
    else if (g_block_param_name && nt_type(nt, fwd_expr) &&
             sp_streq(nt_type(nt, fwd_expr), "LocalVariableReadNode")) {
      const char *en = nt_str(nt, fwd_expr, "name");
      forwards_param = en && sp_streq(en, g_block_param_name);
    }
    if (!forwards_param || g_block_id < 0) return 0;
    block = g_block_id;
  }
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* (range).each_slice/each_cons(n).map chain: materialize the range once
     into an int array and re-enter with the range node's emission and type
     overridden, so the array chain unrolls below serve it (the expression
     mirror of the block-form redispatch in emit_iteration_stmt). */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "each_slice") ||
       sp_streq(nt_str(nt, recv, "name"), "each_cons")) &&
      nt_ref(nt, recv, "block") < 0) {
    int es_recv = nt_ref(nt, recv, "receiver");
    if (es_recv >= 0 && comp_ntype(c, es_recv) == TY_RANGE &&
        range_enum_redispatch(c, recv) && g_n_argov < MAX_ARG_OVERRIDE) {
      int ta = ++g_tmp, tr = ++g_tmp;
      Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, es_recv, &rb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); }); SP_GC_ROOT(_t%d);\n",
                 ta, tr, rb.p ? rb.p : "", tr, ta);
      free(rb.p);
      view_bind(es_recv, "_t%d", ta);
      int v = view_push(c, es_recv, TY_INT_ARRAY);
      int done = emit_collect_expr(c, id, b);
      view_pop(c, v);
      view_unbind(g_n_argov - 1);
      return done;
    }
  }
  /* array.each_slice(n).map { |x, y, ...| } chain: unroll into a direct slice loop */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int es_recv = nt_ref(nt, recv, "receiver");
    int es_args = nt_ref(nt, recv, "arguments");
    int es_argc = 0; const int *es_argv = es_args >= 0 ? nt_arr(nt, es_args, "arguments", &es_argc) : NULL;
    if (es_argc == 1 && es_recv >= 0) {
      TyKind arr_rt = comp_ntype(c, es_recv);
      if (ty_is_array(arr_rt)) {
        const char *k = (arr_rt == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_rt);
        if (k) {
          TyKind restype_es = comp_ntype(c, id);
          int res_poly_es = (restype_es == TY_POLY_ARRAY);
          const char *rk_es = res_poly_es ? "Poly" : array_kind(restype_es);
          if (!rk_es) rk_es = "Int";
          int np_es = 0; while (block_param_name(c, block, np_es)) np_es++;
          int body_es = nt_ref(nt, block, "body");
          int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
          if (bn_es >= 1) {
            int ta_es = ++g_tmp, ts_es = ++g_tmp, tres_es = ++g_tmp, ti_es = ++g_tmp;
            Buf rb_es; memset(&rb_es, 0, sizeof rb_es); emit_expr(c, es_recv, &rb_es);
            emit_indent(g_pre, g_indent); emit_ctype(c, arr_rt, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", ta_es, rb_es.p ? rb_es.p : ""); free(rb_es.p);
            /* rooted like the result array below: the length is the loop
               bound and the block runs between two reads of it */
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta_es);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_int _t%d = ", ts_es); emit_int_expr(c, es_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_es, tres_es, rk_es);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_es);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
                       ti_es, ti_es, k, ta_es, ti_es, ts_es);
            if (block_param_is_multi(c, block, 0)) {
              /* |(a, b)| destructuring: assign each leaf from the slice */
              int lc_es = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc_es; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(ln), k, ta_es, ti_es, li);
              }
            }
            else if (np_es > 1) {
              for (int pj = 0; pj < np_es; pj++) {
                const char *pn = block_param_name(c, block, pj); if (!pn) break;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(pn), k, ta_es, ti_es, pj);
              }
            }
            else {
              const char *p0_es = block_param_name(c, block, 0); if (p0_es) p0_es = rename_local(p0_es);
              if (p0_es) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n",
                           p0_es, k, ta_es, ti_es, ts_es);
              }
            }
            /* collect the block's value (next-aware) into a result temp, push it */
            TyKind melem_es = res_poly_es ? TY_POLY : ty_array_elem(restype_es);
            int tv_es = ++g_tmp; char tvb_es[24]; snprintf(tvb_es, sizeof tvb_es, "_t%d", tv_es);
            emit_indent(g_pre, g_indent + 1);
            if (res_poly_es) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv_es);
            else { emit_ctype(c, melem_es, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv_es, default_value(melem_es)); }
            emit_block_value_into(c, block, tvb_es, res_poly_es, g_indent + 1);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push%s(_t%d, _t%d);\n", rk_es, nil_store_sfx(c, rk_es, bb_es[bn_es - 1]), tres_es, tv_es);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
            buf_printf(b, "_t%d", tres_es);
            return 1;
          }
        }
      }
    }
  }
  /* array.each_cons(n).map { |pair| } or { |a,b| } or { |(a,b)| } chain */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int ec_recv = nt_ref(nt, recv, "receiver");
    int ec_args = nt_ref(nt, recv, "arguments");
    int ec_argc = 0; const int *ec_argv = ec_args >= 0 ? nt_arr(nt, ec_args, "arguments", &ec_argc) : NULL;
    if (ec_argc == 1 && ec_recv >= 0) {
      TyKind arr_ec = comp_ntype(c, ec_recv);
      if (ty_is_array(arr_ec)) {
        const char *kec = (arr_ec == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_ec);
        if (kec) {
          TyKind restype_ec = comp_ntype(c, id);
          int res_poly_ec = (restype_ec == TY_POLY_ARRAY);
          const char *rk_ec = res_poly_ec ? "Poly" : array_kind(restype_ec);
          if (!rk_ec) rk_ec = "Int";
          int body_ec = nt_ref(nt, block, "body");
          int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
          if (bn_ec >= 1) {
            int ta_ec = ++g_tmp, tn_ec = ++g_tmp, tres_ec = ++g_tmp, ti_ec = ++g_tmp;
            Buf rb_ec; memset(&rb_ec, 0, sizeof rb_ec); emit_expr(c, ec_recv, &rb_ec);
            emit_indent(g_pre, g_indent); emit_ctype(c, arr_ec, g_pre);
            buf_printf(g_pre, " _t%d = %s;\n", ta_ec, rb_ec.p ? rb_ec.p : ""); free(rb_ec.p);
            /* rooted like the result array below, for the same reason as the
               each_slice chain above */
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta_ec);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_int _t%d = ", tn_ec); emit_int_expr(c, ec_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_ec, tres_ec, rk_ec);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_ec);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n",
                       ti_ec, ti_ec, tn_ec, kec, ta_ec, ti_ec);
            int np_ec = 0; while (block_param_name(c, block, np_ec)) np_ec++;
            int is_multi_ec = block_param_is_multi(c, block, 0);
            if (is_multi_ec) {
              /* |(a, b)| destructuring: assign each leaf from window */
              int lc_ec = block_param_multi_count(c, block, 0);
              for (int li = 0; li < lc_ec; li++) {
                const char *ln = block_param_multi_leaf(c, block, 0, li);
                if (!ln) continue;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(ln), kec, ta_ec, ti_ec, li);
              }
            }
            else if (np_ec > 1) {
              /* |a, b| flat multi-param: each element. A poly-typed param over a
                 scalar-kind window (an int/float/str array) boxes the element,
                 else a raw scalar would land in an sp_RbVal slot (#2915). */
              TyKind et_ec = ty_array_elem(arr_ec);
              Scope *bsc_ec = comp_scope_of(c, block);
              for (int pj = 0; pj < np_ec; pj++) {
                const char *pn = block_param_name(c, block, pj); if (!pn) break;
                LocalVar *plv = bsc_ec ? scope_local(bsc_ec, pn) : NULL;
                TyKind ppt = plv ? plv->type : TY_POLY;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = ", rename_local(pn));
                char acc[80]; snprintf(acc, sizeof acc, "sp_%sArray_get(_t%d, _t%d + %d)", kec, ta_ec, ti_ec, pj);
                if (ppt == TY_POLY && et_ec == TY_INT) buf_printf(g_pre, "sp_box_int(%s)", acc);
                else if (ppt == TY_POLY && et_ec == TY_FLOAT) buf_printf(g_pre, "sp_box_float(%s)", acc);
                else if (ppt == TY_POLY && et_ec == TY_STRING) buf_printf(g_pre, "sp_box_str(%s)", acc);
                else buf_puts(g_pre, acc);
                buf_puts(g_pre, ";\n");
              }
            }
            else if (np_ec == 1) {
              /* |pair| single param: slice of the window */
              const char *p0_ec = block_param_name(c, block, 0);
              const char *p0_ec_r = p0_ec ? rename_local(p0_ec) : NULL;
              if (p0_ec_r) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n",
                           p0_ec_r, kec, ta_ec, ti_ec, tn_ec);
              }
            }
            IterStep st_ec; emit_iter_step_open(c, block, res_poly_ec, g_indent + 1, &st_ec);
            int saveInd_ec = g_indent; g_indent = g_indent + 1;
            Buf vb_ec; memset(&vb_ec, 0, sizeof vb_ec);
            emit_iter_step_tail(c, &st_ec, &vb_ec);
            g_indent = saveInd_ec;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push%s(_t%d, %s);\n", rk_ec, nil_store_sfx(c, rk_ec, bb_ec[bn_ec - 1]), tres_ec, vb_ec.p ? vb_ec.p : "");
            free(vb_ec.p);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
            buf_printf(b, "_t%d", tres_ec);
            return 1;
          }
        }
      }
    }
  }

  /* array.each_cons(n).with_index(off).map { |pair, i| } or { |(a,b), i| } chain */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int ec_recv2 = nt_ref(nt, wi_recv, "receiver");
      int ec_args2 = nt_ref(nt, wi_recv, "arguments");
      int ec_argc2 = 0; const int *ec_argv2 = ec_args2 >= 0 ? nt_arr(nt, ec_args2, "arguments", &ec_argc2) : NULL;
      int wi_args = nt_ref(nt, recv, "arguments");
      int wi_argc = 0; const int *wi_argv = wi_args >= 0 ? nt_arr(nt, wi_args, "arguments", &wi_argc) : NULL;
      if (ec_argc2 == 1 && ec_recv2 >= 0) {
        TyKind arr_wi = comp_ntype(c, ec_recv2);
        if (ty_is_array(arr_wi)) {
          const char *kwi = (arr_wi == TY_POLY_ARRAY) ? "Poly" : array_kind(arr_wi);
          if (kwi) {
            TyKind restype_wi = comp_ntype(c, id);
            int res_poly_wi = (restype_wi == TY_POLY_ARRAY);
            const char *rk_wi = res_poly_wi ? "Poly" : array_kind(restype_wi);
            if (!rk_wi) rk_wi = "Int";
            int body_wi = nt_ref(nt, block, "body");
            int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
            if (bn_wi >= 1) {
              int ta_wi = ++g_tmp, tn_wi = ++g_tmp, tres_wi = ++g_tmp;
              int ti_wi = ++g_tmp, toff_wi = ++g_tmp, tidx_wi = ++g_tmp;
              Buf rb_wi; memset(&rb_wi, 0, sizeof rb_wi); emit_expr(c, ec_recv2, &rb_wi);
              emit_indent(g_pre, g_indent); emit_ctype(c, arr_wi, g_pre);
              buf_printf(g_pre, " _t%d = %s;\n", ta_wi, rb_wi.p ? rb_wi.p : ""); free(rb_wi.p);
              /* rooted like the result array below, as the each_cons chain is */
              emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_int _t%d = ", tn_wi); emit_int_expr(c, ec_argv2[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_int _t%d = ", toff_wi);
              if (wi_argc > 0 && wi_argv) { emit_expr(c, wi_argv[0], g_pre); }
              else { buf_puts(g_pre, "0"); }
              buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_wi, tres_wi, rk_wi);
              emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_int _t%d = _t%d;\n", tidx_wi, toff_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
                         ti_wi, ti_wi, tn_wi, kwi, ta_wi, ti_wi, tidx_wi);
              /* assign second param (index) */
              const char *idx_p_wi = block_param_name(c, block, 1);
              if (idx_p_wi) {
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = _t%d;\n", rename_local(idx_p_wi), tidx_wi);
              }
              if (block_param_is_multi(c, block, 0)) {
                int lc_wi = block_param_multi_count(c, block, 0);
                Scope *bsc_wi = comp_scope_of(c, block);
                TyKind elem_ty_wi = ty_array_elem(arr_wi);
                for (int li = 0; li < lc_wi; li++) {
                  const char *ln = block_param_multi_leaf(c, block, 0, li);
                  if (!ln) continue;
                  const char *lnr = rename_local(ln);
                  LocalVar *lvw = bsc_wi ? scope_local(bsc_wi, ln) : NULL;
                  TyKind lv_ty_wi = lvw ? lvw->type : TY_UNKNOWN;
                  emit_indent(g_pre, g_indent + 1);
                  if (lv_ty_wi == TY_POLY && elem_ty_wi != TY_POLY && elem_ty_wi != TY_UNKNOWN) {
                    char esw[64];
                    snprintf(esw, sizeof esw, "sp_%sArray_get(_t%d, _t%d + %d)", kwi, ta_wi, ti_wi, li);
                    Buf bxw; memset(&bxw, 0, sizeof bxw); emit_boxed_text(c, elem_ty_wi, esw, &bxw);
                    buf_printf(g_pre, "lv_%s = %s;\n", lnr, bxw.p ? bxw.p : esw); free(bxw.p);
                  }
                  else {
                    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                               lnr, kwi, ta_wi, ti_wi, li);
                  }
                }
              }
              else {
                const char *pair_p_wi = block_param_name(c, block, 0);
                if (pair_p_wi) {
                  Scope *bsc_wi = comp_scope_of(c, block);
                  LocalVar *lvp_wi = bsc_wi ? scope_local(bsc_wi, pair_p_wi) : NULL;
                  char slice_wi[80];
                  snprintf(slice_wi, sizeof slice_wi, "sp_%sArray_slice(_t%d, _t%d, _t%d)",
                           kwi, ta_wi, ti_wi, tn_wi);
                  emit_indent(g_pre, g_indent + 1);
                  /* When the window element type didn't resolve at analyze time
                     the pair param is declared poly; box the typed slice so the
                     assignment types match (a desugared |(x,y), i| destructure
                     over a receiver like `n.times.map { ... }`). */
                  if (lvp_wi && lvp_wi->type == TY_POLY && !sp_streq(kwi, "Poly")) {
                    Buf bxs; memset(&bxs, 0, sizeof bxs);
                    emit_boxed_text(c, arr_wi, slice_wi, &bxs);
                    buf_printf(g_pre, "lv_%s = %s;\n", rename_local(pair_p_wi), bxs.p ? bxs.p : slice_wi);
                    free(bxs.p);
                  }
                  else {
                    buf_printf(g_pre, "lv_%s = %s;\n", rename_local(pair_p_wi), slice_wi);
                  }
                }
              }
              IterStep st_wi; emit_iter_step_open(c, block, res_poly_wi, g_indent + 1, &st_wi);
              int saveInd_wi = g_indent; g_indent = g_indent + 1;
              Buf vb_wi; memset(&vb_wi, 0, sizeof vb_wi);
              emit_iter_step_tail(c, &st_wi, &vb_wi);
              g_indent = saveInd_wi;
              emit_indent(g_pre, g_indent + 1);
              buf_printf(g_pre, "sp_%sArray_push%s(_t%d, ", rk_wi, nil_store_sfx(c, rk_wi, bb_wi[bn_wi - 1]), tres_wi);
              if (res_poly_wi) buf_puts(g_pre, vb_wi.p ? vb_wi.p : "");
              else emit_typed_sink_text(c, bb_wi[bn_wi - 1], sp_streq(rk_wi, "Int") ? TY_INT : sp_streq(rk_wi, "Float") ? TY_FLOAT : TY_UNKNOWN, vb_wi.p ? vb_wi.p : "", g_pre);
              buf_puts(g_pre, ");\n");
              free(vb_wi.p);
              emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
              buf_printf(b, "_t%d", tres_wi);
              return 1;
            }
          }
        }
      }
    }
  }

  if (ty_is_hash(rt)) {
    if (emit_hash_collect_expr(c, id, b)) return 1;
    if (emit_hash_reduce_search_expr(c, id, b)) return 1;
    if (emit_hash_sort_by_expr(c, id, b)) return 1;
    if (emit_hash_reduce_scalar_expr(c, id, b)) return 1;
    return 0;
  }
  int range_recv = (rt == TY_RANGE);
  if (rt == TY_POLY) {
    /* poly-typed receiver (e.g. `arr = nil` default): iterate via
       sp_poly_arr_len / sp_poly_arr_get and build a typed result array */
    int is_map2 = ty_iter_shape(name) == TY_ITER_MAP;
    if (!is_map2) return 0;
    TyKind restype2 = comp_ntype(c, id);
    int res_poly2 = (restype2 == TY_POLY_ARRAY);
    const char *rk2 = res_poly2 ? "Poly" : array_kind(restype2);
    if (!rk2) return 0;
    const char *p0p = block_param_name(c, block, 0); if (p0p) p0p = rename_local(p0p);
    int body2 = nt_ref(nt, block, "body");
    int bn2 = 0;
    const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
    /* `map {}` runs an empty block per element, so the result is one nil per
       element -- a poly array. The typed-array path already answers that shape;
       without it here a poly receiver fell through to the dispatch, which has no
       arm for it, and the call raised NoMethodError (#3905). */
    if (bn2 < 1 && !res_poly2) return 0;
    int trecv2 = ++g_tmp, tn2 = ++g_tmp, tres2 = ++g_tmp, ti2 = ++g_tmp;
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = ", trecv2);
    buf_puts(g_pre, rb2.p ? rb2.p : "sp_box_nil()");
    buf_puts(g_pre, ";\n");
    free(rb2.p);
    /* The receiver is what the loop reads from, and the body allocates -- the
       result array, a boxed element, an inspect string. A freshly built
       receiver (`(g - [vertex]).map { ... }`) was held by nothing, so the
       first collection inside the loop freed the array being walked and the
       walk answered nil from its recycled memory (#3801). */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv2);
    /* nil is no collection: the length read below gave it a zero-trip loop
       and `nil.map { }` answered [] (#4485) */
    emit_indent(g_pre, g_indent);
    emit_poly_iter_obj_reject(c, trecv2, name, g_pre);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_poly_iter_check(_t%d, \"%s\");\n", trecv2, name);
    const char *restn2 = block_rest_name(c, block);
    int has_rest2 = restn2 && *restn2;
    int np2 = 0; while (block_param_name(c, block, np2)) np2++;
    /* an Enumerator yielding two values gives a lone `|x|` the first and a
       lone `|*r|` both (sp_Enumerator.yields_pair) */
    int tpair2 = 0;
    /* a block of any other shape than plain requireds or a lone rest binds
       the step's values by the proc distribution (emit_boxed_step_binds) */
    int gather2 = block_binds_gathered(c, block) && !block_lone_rest(c, block);
    if ((np2 == 1 && !has_rest2 && !block_param_is_multi(c, block, 0)) || (np2 == 0 && has_rest2) || gather2) {
      tpair2 = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "int _t%d = sp_poly_yields_pair(_t%d);\n", tpair2, trecv2);
    }
    /* a Range walks as its members (#4837) */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_t%d = sp_poly_iter_subject(_t%d);\n", trecv2, trecv2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_int _t%d = sp_poly_length(_t%d);\n", tn2, trecv2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk2, tres2, rk2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {\n",
               ti2, ti2, tn2, ti2);
    /* pin block param to TY_POLY via shadow declaration */
    Scope *csc2 = p0p ? comp_scope_of(c, block) : NULL;
    LocalVar *clv2 = (csc2 && p0p) ? scope_local(csc2, p0p) : NULL;
    TyKind csaved2 = clv2 ? clv2->type : TY_UNKNOWN;
    if (clv2) clv2->type = TY_POLY;
    for (int j2 = 0; j2 < bn2; j2++) infer_type(c, bb2[j2]);
    int spair = tpair2 && np2 == 1 && nt_str(nt, block, "sym_proc_arg") ? nt_ref(nt, block, "sym_proc_pair") : -1;
    int tpacked2 = 0;
    if (spair >= 0) infer_type(c, spair);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "{\n");
    if (gather2) {
      char vals[160];
      snprintf(vals, sizeof vals, "sp_yielded_args(_t%d, sp_poly_iter_elem(_t%d, _t%d))", tpair2, trecv2, ti2);
      if (p0p) { emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "sp_RbVal lv_%s;\n", p0p); }
      emit_boxed_step_binds(c, block, vals, g_pre, g_indent + 2, 0);
    }
    else if (p0p && np2 >= 2 && !has_rest2 && !block_param_is_multi(c, block, 0)) {
      /* `|k, val|` over a poly hash: each element is a [key, value] pair, so
         auto-splat it across the params (only p0 was bound before, leaving val
         nil and losing every value) (#2873). */
      int te2 = ++g_tmp;
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_poly_iter_elem(_t%d, _t%d); SP_GC_ROOT_RBVAL(_t%d);\n",
                 te2, trecv2, ti2, te2);
      emit_autosplat_params(c, block, np2, te2, g_indent + 2);
    }
    else if (p0p && tpair2 && spair >= 0) {
      /* whether this step yielded several values, which picks the call */
      int te3 = ++g_tmp;
      tpacked2 = ++g_tmp;
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_poly_iter_elem(_t%d, _t%d);\n", te3, trecv2, ti2);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "int _t%d = sp_yielded_packed(_t%d, _t%d);\n", tpacked2, tpair2, te3);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal lv_%s = sp_yielded_first(_t%d, _t%d);\n", p0p, tpair2, te3);
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal lv_%s = _t%d ? sp_poly_arr_get(_t%d, 1) : sp_box_nil();\n",
                 rename_local(nt_str(nt, block, "sym_proc_arg")), tpacked2, te3);
    }
    else if (p0p && tpair2) {
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal lv_%s = sp_yielded_first(_t%d, sp_poly_iter_elem(_t%d, _t%d));\n",
                 p0p, tpair2, trecv2, ti2);
    }
    else if (p0p) {
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal lv_%s = sp_poly_iter_elem(_t%d, _t%d);\n",
                 p0p, trecv2, ti2);
    }
    else if (!has_rest2) {
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "sp_RbVal lv__dummy = sp_poly_iter_elem(_t%d, _t%d); (void)lv__dummy;\n",
                 trecv2, ti2);
    }
    /* a lone rest takes the step's values */
    if (has_rest2 && !gather2) {
      emit_indent(g_pre, g_indent + 2);
      buf_printf(g_pre, "lv_%s = sp_yielded_args(_t%d, sp_poly_iter_elem(_t%d, _t%d));\n",
                 rename_local(restn2), tpair2, trecv2, ti2);
    }
    for (int j2 = 0; j2 < bn2 - 1; j2++) emit_stmt(c, bb2[j2], g_pre, g_indent + 2);
    int saveIndent2 = g_indent; g_indent = g_indent + 2;
    Buf vb2; memset(&vb2, 0, sizeof vb2);
    if (spair >= 0 && bn2 >= 1) {
      /* the call with the second value, when this Enumerator yields two */
      int tv3 = ++g_tmp;
      emit_indent(g_pre, g_indent);
      if (res_poly2) buf_puts(g_pre, "sp_RbVal");
      else emit_ctype(c, ty_array_elem(restype2), g_pre);
      buf_printf(g_pre, " _t%d;\n", tv3);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (_t%d) {\n", tpacked2 ? tpacked2 : tpair2);
      g_indent++;
      Buf pv; memset(&pv, 0, sizeof pv);
      emit_boxed(c, spair, &pv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d = ", tv3);
      if (res_poly2) buf_puts(g_pre, pv.p ? pv.p : "sp_box_nil()");
      else emit_unbox_text(c, ty_array_elem(restype2), pv.p ? pv.p : "sp_box_nil()", g_pre);
      buf_puts(g_pre, ";\n");
      free(pv.p);
      g_indent--;
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "}\nelse {\n");
      g_indent++;
      Buf ov; memset(&ov, 0, sizeof ov);
      if (res_poly2) emit_boxed(c, bb2[bn2 - 1], &ov);
      else emit_expr(c, bb2[bn2 - 1], &ov);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d = %s;\n", tv3, ov.p ? ov.p : "");
      free(ov.p);
      g_indent--;
      emit_indent(g_pre, g_indent);
      buf_puts(g_pre, "}\n");
      buf_printf(&vb2, "_t%d", tv3);
    }
    else if (bn2 < 1) buf_puts(&vb2, "sp_box_nil()");
    else if (res_poly2) emit_boxed(c, bb2[bn2 - 1], &vb2);
    else emit_expr(c, bb2[bn2 - 1], &vb2);
    g_indent = saveIndent2;
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "sp_%sArray_push%s(_t%d, %s);\n", rk2, nil_store_sfx(c, rk2, bn2 >= 1 ? bb2[bn2 - 1] : -1), tres2, vb2.p ? vb2.p : "");
    free(vb2.p);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "}\n");
    if (clv2) clv2->type = csaved2;
    buf_printf(b, "_t%d", tres2);
    return 1;
  }
  if (!ty_is_array(rt) && !ty_is_ptr_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : array_iter_kind(rt);
  if (!k) return 0;

  TyIterShape shp = ty_iter_shape(name);
  int is_map = shp == TY_ITER_MAP;
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  if (!is_map && !is_sel && !is_rej) return 0;

  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  /* A nested table result (`idx.map { |k| cols[k] }`) is an sp_PtrArray of
     row pointers. The rest of this arm already generalizes -- ty_array_elem
     gives the row type, emit_ctype declares the temp as that row pointer, and
     emit_block_value_into fills it unboxed -- so naming the container is all
     that was missing. Without it array_kind answered NULL and the whole fold
     bailed, leaving map to build a poly array and box every row on the way
     in, only for each read to unbox it again. */
  const char *rk = res_poly ? "Poly" : ty_is_ptr_array(restype) ? "Ptr" : array_kind(restype);
  if (!rk) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;

  /* map {} with empty block: poly array of nil with same length as receiver */
  if (bn == 0 && is_map) {
    int tlen = ++g_tmp, tres0 = ++g_tmp, ti0 = ++g_tmp;
    Buf rb0; memset(&rb0, 0, sizeof rb0);
    emit_expr(c, recv, &rb0);  /* preludes land in g_pre, value in rb0 */
    emit_indent(g_pre, g_indent);
    if (range_recv) {
      int tr = ++g_tmp;
      buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr, rb0.p ? rb0.p : "");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_int _t%d = _t%d.last - _t%d.excl - _t%d.first + 1; if (_t%d < 0) _t%d = 0;\n",
                 tlen, tr, tr, tr, tlen, tlen);
    }
    else {
      buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(%s);\n", tlen, k, rb0.p ? rb0.p : "NULL");
    }
    free(rb0.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres0, tres0);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_box_nil());\n",
               ti0, ti0, tlen, ti0, tres0);
    buf_printf(b, "_t%d", tres0);
    return 1;
  }

  if (bn < 1) return 0;

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;

  /* eval receiver once (its own preludes must land before the decl line);
     a range receiver is materialized to an int array first */
  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_recv) {
    int tr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    /* rendered first: see the note at the poly-callable temp in codegen_call.c
       -- emit_expr may want g_pre lines of its own (#4065) */
    { Buf rgb; memset(&rgb, 0, sizeof rgb); emit_expr(c, recv, &rgb);
      buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr, rgb.p ? rgb.p : "(sp_Range){0}");
      free(rgb.p); }
    buf_printf(&rb, "sp_range_to_ia(_t%d)", tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = ", trecv);
  buf_puts(g_pre, rb.p ? rb.p : "");
  buf_puts(g_pre, ";\n");
  free(rb.p);
  /* root the iteration source: the loop body may allocate and collect it */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);

  TyKind et_elem = ty_array_elem(rt);
  /* 2-param auto-splat: |a, b| over a poly array whose elements are sub-arrays
     binds each param to a positional element of the sub-array, matching CRuby's
     proc auto-splat. The per-param types were pinned by infer_block_params, so
     bind directly (no shadow). select/reject still push the whole element. */
  int np_cl = 0; while (block_param_name(c, block, np_cl)) np_cl++;
  int autosplat = (np_cl >= 2 && rt == TY_POLY_ARRAY && !block_param_is_multi(c, block, 0));

  /* If the block param's scope type was widened (e.g. TY_POLY), pin it to
     the element type and use a C shadow declaration so body emission sees the
     right type. */
  Scope *csc = (p0 && !autosplat) ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et_elem && et_elem != TY_UNKNOWN;
  if (use_shadow) {
    clv0->type = et_elem;
    for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
  }

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  int te_split = -1;
  if (autosplat) {
    te_split = ++g_tmp;
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te_split, trecv, ti);
    emit_autosplat_params(c, block, np_cl, te_split, bodyIndent);
  }
  else if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et_elem, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  /* a `*rest` param: splat-only wraps the whole element; alongside required
     params it binds empty (scalar elements never distribute) */
  if (!autosplat && block_rest_name(c, block)) {
    char es_r[256];
    snprintf(es_r, sizeof es_r, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
    if (emit_iter_bind_rest(c, block, np_cl, et_elem, es_r, g_pre,
                            use_shadow ? innerIndent : bodyIndent) < 0) {
      unsupported(c, id, "block splat parameter alongside required params over a poly element");
      return 1;
    }
  }
  if (is_map) {
    /* map: collect the block's value (next-aware) into a result temp, then
       push it -- so `next <v>` inside the block contributes <v> rather than
       dropping the element. */
    TyKind elem = ty_array_elem(restype);
    int tv = ++g_tmp;
    char tvbuf[24]; snprintf(tvbuf, sizeof tvbuf, "_t%d", tv);
    emit_indent(g_pre, innerIndent);
    if (res_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, elem, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(elem)); }
    emit_block_value_into(c, block, tvbuf, res_poly, innerIndent);
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "sp_%sArray_push%s(_t%d, _t%d);\n", rk, nil_store_sfx(c, rk, bn > 0 ? bb[bn - 1] : -1), tres, tv);
  }
  else {
    /* select/reject: collect the block's value (next-aware) into a temp, then
       push the element when that value (negated for reject) is truthy -- so a
       `next <cond>` inside the block decides inclusion instead of being lost. */
    TyKind cty = comp_ntype(c, bb[bn - 1]);
    /* A body whose value IS nil types VOID, and `void _tN` is not a
       declaration: `select { nil }` never compiled, and `select {}` reaches
       here now that an empty body carries the nil it means (#4006). Carry it
       boxed -- nil is a value in this slot, and a falsy one. */
    if (cty == TY_VOID || cty == TY_NIL) cty = TY_POLY;
    if (cty == TY_UNKNOWN) cty = TY_INT;
    int cond_poly = (cty == TY_POLY);
    int tv = ++g_tmp;
    char tvbuf[24]; snprintf(tvbuf, sizeof tvbuf, "_t%d", tv);
    emit_indent(g_pre, innerIndent);
    if (cond_poly) buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tv);
    else { emit_ctype(c, cty, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tv, default_value(cty)); }
    emit_block_value_into(c, block, tvbuf, cond_poly, innerIndent);
    emit_indent(g_pre, innerIndent);
    /* Ruby truthiness: only nil and false are falsy. A nilable int/float reads
       falsy at its sentinel, but 0 / 0.0 are truthy -- so a block returning
       `x % 2` must keep the element. A bool keeps C-truthiness (0/1); a pointer
       value is falsy only when NULL (nil). Mirrors emit_cond. */
    if (cond_poly)         buf_printf(g_pre, "if (%ssp_poly_truthy(_t%d)) ", is_rej ? "!" : "", tv);
    else if (cty == TY_INT)   buf_printf(g_pre, "if (%s(_t%d != SP_INT_NIL)) ", is_rej ? "!" : "", tv);
    else if (cty == TY_FLOAT) buf_printf(g_pre, "if (%s(!sp_float_is_nil(_t%d))) ", is_rej ? "!" : "", tv);
    else                   buf_printf(g_pre, "if (%s(_t%d)) ", is_rej ? "!" : "", tv);
    if (autosplat)
      buf_printf(g_pre, "sp_%sArray_push(_t%d, _t%d);\n", rk, tres, te_split);
    else if (p0)
      buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0);
    else
      /* splat-only block: push the element straight from the receiver */
      buf_printf(g_pre, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n", rk, tres, k, trecv, ti);
  }
  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  if (use_shadow && clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");
  /* the kept (or mapped-to-itself) elements carry the receiver's nils: an
     element read out of an Integer or Float array is typed as a number even
     where a gap left the sentinel, so its may_nil is handed on whole */
  if (!range_recv && k && rk && sp_streq(rk, k) && (sp_streq(k, "Int") || sp_streq(k, "Float"))) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray_nil_from(_t%d, _t%d);\n", k, tres, trecv);
  }

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* arr.map.with_index(off) { |x, i| } / arr.each.with_index(off) { |x, i| } /
   arr.select.with_index(off) { |x, i| } (and collect/filter/reject): the
   receiver is a blockless enumerator over an array, and with_index binds the
   element plus a running index that starts at `off` (default 0). `map` collects
   the block value; `select`/`reject` collect the element conditionally; `each`
   runs the body for side effect and yields the receiver array. Arrays only --
   a range enumerator is a later slice. Returns 1 if handled. */
int emit_with_index_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "with_index")) return 0;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");  /* the blockless inner enumerator */
  if (recv < 0 || !nt_type(nt, recv) || !sp_streq(nt_type(nt, recv), "CallNode")) return 0;
  if (nt_ref(nt, recv, "block") >= 0) return 0;
  const char *inner = nt_str(nt, recv, "name");
  if (!inner) return 0;
  int is_each = sp_streq(inner, "each");
  /* map!.with_index: collect like map, then write the result back into the
     receiver in place; the chain evaluates to the receiver */
  int is_mapbang = sp_streq(inner, "map!") || sp_streq(inner, "collect!");
  TyIterShape shp = ty_iter_shape(inner);  /* map/select/reject; NONE for each */
  if (is_mapbang) shp = TY_ITER_MAP;
  if (!is_each && !is_mapbang && shp == TY_ITER_NONE) return 0;
  int arr_recv = nt_ref(nt, recv, "receiver");
  if (arr_recv < 0) return 0;
  TyKind rt = comp_ntype(c, arr_recv);
  /* an Integer Range source materializes to an int array once, then the
     array machinery below applies unchanged (#3228) */
  int range_src = (rt == TY_RANGE);
  if (range_src) rt = TY_INT_ARRAY;
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  if (range_src && is_mapbang) return 0;   /* no in-place write-back on a range */

  int is_map = shp == TY_ITER_MAP;
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  int collecting = is_map || is_sel || is_rej;

  int wi_args = nt_ref(nt, id, "arguments");
  int wi_argc = 0; const int *wi_argv = wi_args >= 0 ? nt_arr(nt, wi_args, "arguments", &wi_argc) : NULL;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  const char *p1 = block_param_name(c, block, 1); if (p1) p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 && collecting) return 0;  /* map/select need a value */

  TyKind restype = is_mapbang ? rt : comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = collecting ? (res_poly ? "Poly" : array_kind(restype)) : NULL;
  if (collecting && !rk) rk = "Int";

  int trecv = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  int tres = collecting ? ++g_tmp : 0;

  /* evaluate the source array once (its preludes land in g_pre first);
     a Range source is hoisted whole (each.with_index yields IT back) and
     materializes through sp_range_to_ia (#3228) */
  int trng = 0;
  if (range_src) {
    trng = ++g_tmp;
    Buf rgb; memset(&rgb, 0, sizeof rgb); emit_expr(c, arr_recv, &rgb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Range _t%d = %s;\n", trng, rgb.p ? rgb.p : "(sp_Range){0}");
    free(rgb.p);
  }
  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_src) buf_printf(&rb, "sp_range_to_ia(_t%d)", trng);
  else emit_expr(c, arr_recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  /* running index, seeded with the offset */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = ", tidx);
  if (wi_argc > 0 && wi_argv) emit_expr(c, wi_argv[0], g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");
  if (collecting) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
             ti, ti, k, trecv, ti, tidx);

  int innerIndent = g_indent + 1;
  TyKind elem_t = ty_array_elem(rt);
  Scope *csc = comp_scope_of(c, block);
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  LocalVar *clv1 = (csc && p1) ? scope_local(csc, p1) : NULL;
  if (p0) {
    emit_indent(g_pre, innerIndent);
    if (clv0 && clv0->type == TY_POLY && elem_t != TY_POLY && elem_t != TY_UNKNOWN) {
      char src[256]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
      buf_printf(g_pre, "lv_%s = ", p0); emit_boxed_text(c, elem_t, src, g_pre); buf_puts(g_pre, ";\n");
    }
    else buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  if (p1) {
    emit_indent(g_pre, innerIndent);
    if (clv1 && clv1->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", p1, tidx);
    else buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tidx);
  }
  if (is_each) emit_iter_step_body(c, block, g_pre, innerIndent);
  else {
    IterStep st; emit_iter_step_open(c, block, !is_map, innerIndent, &st);
    int saveInd = g_indent; g_indent = innerIndent;
    Buf vb; memset(&vb, 0, sizeof vb);
    TyKind body_ty = TY_UNKNOWN;
    if (is_map) body_ty = emit_iter_step_tail(c, &st, &vb);
    else emit_iter_step_cond(c, &st, 1, &vb);
    g_indent = saveInd;
    if (is_map) {
      emit_indent(g_pre, innerIndent); buf_printf(g_pre, "sp_%sArray_push%s(_t%d, ", rk, nil_store_sfx(c, rk, bb[bn - 1]), tres);
      if (res_poly && body_ty != TY_POLY) {
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, body_ty, vb.p ? vb.p : "", &bx);
        buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      }
      else if (!res_poly) emit_typed_sink_text(c, bb[bn - 1], sp_streq(rk, "Int") ? TY_INT : sp_streq(rk, "Float") ? TY_FLOAT : TY_UNKNOWN, vb.p ? vb.p : "", g_pre);
      else buf_puts(g_pre, vb.p ? vb.p : "");
      buf_puts(g_pre, ");\n");
    }
    else {  /* select / reject: push the element on the (negated) predicate */
      emit_indent(g_pre, innerIndent);
      buf_printf(g_pre, "if (%s(", is_rej ? "!" : "");
      buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ")) ");
      buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
    }
    free(vb.p);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* a kept element carries the receiver's nils (the map arm notes its own) */
  if (collecting && !is_map && !range_src && k && rk && sp_streq(rk, k) && (sp_streq(k, "Int") || sp_streq(k, "Float"))) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray_nil_from(_t%d, _t%d);\n", k, tres, trecv);
  }

  if (is_mapbang) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray_replace(_t%d, _t%d);\n", rk, trecv, tres);
    buf_printf(b, "_t%d", trecv);   /* map! returns the mutated receiver */
  }
  else if (collecting) buf_printf(b, "_t%d", tres);
  else if (range_src) buf_printf(b, "_t%d", trng);  /* each yields the Range itself */
  else buf_printf(b, "_t%d", trecv);  /* each.with_index yields the receiver */
  return 1;
}

/* <stored enumerator>.with_index(off) { |x, i| } in VALUE position: drain
   the snapshot once, drive the block with the offset index, and yield the
   enumerator's with_index return -- the boxed source for an each-family
   enumerator (the meth-gated runtime helper raises loudly for a stored
   collector enumerator, whose result we cannot rebuild here). The statement
   form is served by the iter emitter; immediate array chains by
   emit_with_index_expr above. Returns 1 if handled. */
/* enum.find/detect/take_while { |v| pred } driven lazily via #next inside a
   StopIteration setjmp frame (the Kernel#loop pattern), so an infinite
   generator enum (blockless `loop`, possibly .with_index-chained) works and
   a finite enum without a match yields nil (#3236). A user `break val` in
   the block routes through the enclosing break wrapper as usual. */
int emit_enum_find_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int take = name && sp_streq(name, "take_while");
  int inc = name && (sp_streq(name, "include?") || sp_streq(name, "member?"));
  if (!name || (!take && !inc && !sp_streq(name, "find") && !sp_streq(name, "detect"))) return 0;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  int iargs = nt_ref(nt, id, "arguments");
  int iargc = 0;
  const int *iargv = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &iargc) : NULL;
  if (inc) {
    /* include?/member? scan for one value: no block, exactly one argument */
    if (block >= 0 || iargc != 1 || !iargv) return 0;
  }
  else if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_ENUMERATOR) return 0;
  /* find(ifnone) { }: the proc answers when nothing matched, exactly as the
     array form serves it. Any other argument stays a loud reject. (#3814) */
  int f_ifnone = !inc && !take && iargc == 1 && iargv &&
                 comp_ntype(c, iargv[0]) == TY_PROC;
  if (!inc && iargc > 0 && !f_ifnone) return 0;
  const char *p0_orig = inc ? NULL : block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1_orig = inc ? NULL : block_param_name(c, block, 1);
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
  int body = inc ? -1 : nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  Scope *bsc = inc ? NULL : comp_scope_of(c, block);
  /* the sought value is evaluated once, before the pull loop */
  int tneedle = 0;
  if (inc) {
    tneedle = ++g_tmp;
    Buf nb; memset(&nb, 0, sizeof nb); emit_boxed(c, iargv[0], &nb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
               tneedle, nb.p ? nb.p : "sp_box_nil()", tneedle);
    free(nb.p);
  }

  int te = ++g_tmp, tres = ++g_tmp, tv = ++g_tmp, tg = ++g_tmp;
  int tfn = f_ifnone ? ++g_tmp : -1;
  if (f_ifnone) {
    /* bound up front (CRuby evaluates arguments first); the found flag keeps a
       matched nil element from calling the proc */
    Buf nb; memset(&nb, 0, sizeof nb); emit_expr(c, iargv[0], &nb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); int _tf%d = 0;\n",
               tfn, nb.p ? nb.p : "NULL", tfn, tfn);
    free(nb.p);
  }
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_Enumerator *_t%d = sp_enum_fresh_run(%s); SP_GC_ROOT(_t%d);\n", te, rb.p ? rb.p : "", te);
  free(rb.p);
  emit_indent(g_pre, g_indent);
  /* take_while collects the passing prefix; find/detect keep one element;
     include?/member? answer whether the scan ever hit the sought value */
  if (inc)
    buf_printf(g_pre, "int _t%d = FALSE;\n", tres);
  else if (take)
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  else
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tv, tv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "int _t%d = sp_gc_nroots; (void)_t%d;\n", tg, tg);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "sp_exc_check_depth();\n");
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n");
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "for (;;) {\n");
  int din = g_indent + 2;
  emit_indent(g_pre, din);
  buf_printf(g_pre, "_t%d = sp_Enumerator_next(_t%d);\n", tv, te);
  if (inc) {
    emit_indent(g_pre, din);
    buf_printf(g_pre, "if (sp_poly_eq(_t%d, _t%d)) { _t%d = TRUE; break; }\n", tv, tneedle, tres);
  }
  /* bind block params: two params autosplat an array element; one binds it,
     or for take_while the first of the values a step yielded */
  LocalVar *lv0f = p0_orig && bsc ? scope_local(bsc, p0_orig) : NULL;
  if (p0 && lv0f) {   /* a discard param (`_`) has no declared local: skip */
    TyKind pt0 = lv0f->type;
    emit_indent(g_pre, din);
    buf_printf(g_pre, "lv_%s = ", p0);
    { char vx[96];
      if (p1) snprintf(vx, sizeof vx, "sp_poly_massign_get(_t%d, 0)", tv);
      else if (take) snprintf(vx, sizeof vx, "sp_yielded_first(_t%d->yields_pair, _t%d)", te, tv);
      else snprintf(vx, sizeof vx, "_t%d", tv);
      if (pt0 == TY_POLY) buf_puts(g_pre, vx);
      else emit_unbox_text(c, pt0, vx, g_pre); }
    buf_puts(g_pre, ";\n");
  }
  LocalVar *lv1f = p1_orig && bsc ? scope_local(bsc, p1_orig) : NULL;
  if (p1 && lv1f) {
    TyKind pt1 = lv1f->type;
    emit_indent(g_pre, din);
    buf_printf(g_pre, "lv_%s = ", p1);
    { char vx[48]; snprintf(vx, sizeof vx, "sp_poly_massign_get(_t%d, 1)", tv);
      if (pt1 == TY_POLY) buf_puts(g_pre, vx);
      else emit_unbox_text(c, pt1, vx, g_pre); }
    buf_puts(g_pre, ";\n");
  }
  { int sv_in = g_indent; g_indent = din;
    for (int j = 0; j + 1 < bn; j++) emit_stmt(c, bb[j], g_pre, din);
    if (bn > 0) {
      int tt = ++g_tmp;
      Buf tb; memset(&tb, 0, sizeof tb);
      emit_boxed(c, bb[bn - 1], &tb);
      emit_indent(g_pre, din);
      buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tt, tb.p ? tb.p : "sp_box_nil()");
      free(tb.p);
      emit_indent(g_pre, din);
      if (take)
        buf_printf(g_pre, "if (!sp_poly_truthy(_t%d)) break;\n"
                          "        sp_PolyArray_push(_t%d, _t%d);\n", tt, tres, tv);
      else if (f_ifnone)
        buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) { _t%d = _t%d; _tf%d = 1; break; }\n",
                   tt, tres, tv, tfn);
      else
        buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) { _t%d = _t%d; break; }\n", tt, tres, tv);
    }
    g_indent = sv_in; }
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "sp_exc_top--;\n");
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "else {\n");
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "sp_exc_top--;\n");
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_gc_nroots = _t%d;\n", tg);
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "if (sp_unwind_kind != SP_UNWIND_NONE) sp_unwind_resume();\n");
  emit_indent(g_pre, g_indent + 1);
  buf_puts(g_pre, "if (!sp_exc_cls_matches((const char *)sp_last_exc_cls, \"StopIteration\")) { sp_pending_exc_obj = sp_exc_obj[sp_exc_top]; sp_raise_cls(sp_exc_cls[sp_exc_top], sp_exc_msg[sp_exc_top]); }\n");
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");
  if (f_ifnone) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (sp_int[16]){0}), _sp_proc_poly_ret);\n",
               tfn, tres, tfn);
  }
  buf_printf(b, "_t%d", tres);
  return 1;
}

int emit_enum_with_index_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !sp_streq(name, "with_index")) return 0;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_ENUMERATOR) return 0;
  int wargs = nt_ref(nt, id, "arguments");
  int wargc = 0;
  const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
  if (wargc > 1) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;
  int body = nt_ref(nt, block, "body");
  /* Collect the block results as we drive it; the return value is then decided by
     the enumerator's method at run time: a map/collect enumerator returns the
     collected array (#2510), each/each_with_index the source receiver. */
  int te = ++g_tmp, ta = ++g_tmp, ti = ++g_tmp, toff = ++g_tmp;
  int tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_Enumerator *_t%d = %s; SP_GC_ROOT(_t%d);\n",
             te, rb.p ? rb.p : "", te);
  free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_Enumerator_to_a(_t%d); SP_GC_ROOT(_t%d);\n",
             ta, te, ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = ", toff);
  if (wargc == 1 && wargv) emit_int_expr(c, wargv[0], g_pre);
  else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
             ti, ti, ta, ti);
  Scope *bs = comp_scope_of(c, block);
  if (p0) {
    LocalVar *b0 = p0_orig ? scope_local(bs, p0_orig) : NULL;
    TyKind p0t = (b0 && b0->type != TY_UNKNOWN) ? b0->type : TY_POLY;
    char vb0[48];
    snprintf(vb0, sizeof vb0, "sp_PolyArray_get(_t%d, _t%d)", ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = ", p0);
    if (p0t == TY_POLY) buf_puts(g_pre, vb0);
    else emit_unbox_text(c, p0t, vb0, g_pre);
    buf_puts(g_pre, ";\n");
  }
  if (p1) {
    LocalVar *b1 = p1_orig ? scope_local(bs, p1_orig) : NULL;
    TyKind p1t = (b1 && b1->type != TY_UNKNOWN) ? b1->type : TY_POLY;
    emit_indent(g_pre, g_indent + 1);
    if (p1t == TY_POLY)
      buf_printf(g_pre, "lv_%s = sp_box_int(_t%d + _t%d);\n", p1, ti, toff);
    else
      buf_printf(g_pre, "lv_%s = _t%d + _t%d;\n", p1, ti, toff);
  }
  {
    int tv = ++g_tmp;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_RbVal _t%d;\n", tv);
    char dv[24]; snprintf(dv, sizeof dv, "_t%d", tv);
    emit_block_value_into(c, block, dv, 1, g_indent + 1);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, _t%d);\n", tres, tv);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "sp_enum_with_index_result(_t%d, _t%d)", te, tres);
  (void)body;
  return 1;
}

enum { PRED_BOOL, PRED_ALWAYS, PRED_NEVER, PRED_POLY };

/* Update the all? flag or count truthy block results, using Ruby truthiness
   even for a concrete non-bool result (#3141). */
static void emit_pred_cond(Buf *b, int pred_kind, const char *cond, int acc, int is_all) {
  if (is_all) {
    /* Record failure directly: the block can change the receiver's length. */
    switch (pred_kind) {
      case PRED_ALWAYS: buf_printf(b, "(void)(%s);\n", cond); break;
      case PRED_NEVER: buf_printf(b, "{ (void)(%s); _t%d = FALSE; break; }\n", cond, acc); break;
      case PRED_POLY: buf_printf(b, "if (!sp_poly_truthy(%s)) { _t%d = FALSE; break; }\n", cond, acc); break;
      default: buf_printf(b, "if (!(%s)) { _t%d = FALSE; break; }\n", cond, acc); break;
    }
    return;
  }
  switch (pred_kind) {
    case PRED_ALWAYS: buf_printf(b, "{ (void)(%s); _t%d++; }\n", cond, acc); break;
    case PRED_NEVER: buf_printf(b, "(void)(%s);\n", cond); break;
    case PRED_POLY: buf_printf(b, "if (sp_poly_truthy(%s)) _t%d++;\n", cond, acc); break;
    default: buf_printf(b, "if (%s) _t%d++;\n", cond, acc); break;
  }
}
/* index / rindex WITH A BLOCK on a poly receiver.
   The typed-array emitter is keyed on the storage kind, so a value only known
   to be an array at run time never reached it and the call fell through to the
   unresolved-call raise -- while every sibling name (find, select, count) had
   a poly loop of its own. Same loop, answering the index or nil (#3409).
   find_index used to be here too: now a Ruby definition (builtins/
   enumerable.rb), it goes through the generic __enum_find_index__ dispatch
   the same way find/detect/count do. Leaving it here raced that dispatch --
   this loop is unconditionally hoisted into g_pre ahead of the runtime
   is_a? guard the dispatch builds when some instantiated class defines its
   own find_index (any Enumerable includer does now that Enumerable#find_index
   is real, `require "set"` among them), corrupting the guarded branch's temps
   even on a receiver no such class ever holds: a poly-typed method answering
   different array types per call site answered nil for every element once
   Set was merely required, nowhere near the call site or the value itself. */
int emit_find_index_poly_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !(sp_streq(name, "index") || sp_streq(name, "rindex"))) return 0;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || recv < 0) return 0;
  if (comp_ntype(c, recv) != TY_POLY || infer_type(c, recv) != TY_POLY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int rev = sp_streq(name, "rindex");
  const char *p0raw = block_param_name(c, block, 0);
  const char *p0 = p0raw ? rename_local(p0raw) : NULL;
  int trecv = ++g_tmp, tlen = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", trecv, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tlen, trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = SP_INT_NIL;\n", tres);
  emit_indent(g_pre, g_indent);
  if (rev)
    buf_printf(g_pre, "for (sp_int _t%d = _t%d - 1; _t%d >= 0; _t%d--) {\n", ti, tlen, ti, ti);
  else
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {\n", ti, ti, tlen, ti);
  int bi = g_indent + 1;
  int nparam = 0; while (block_param_name(c, block, nparam)) nparam++;
  if (nparam >= 2 && !block_param_is_multi(c, block, 0)) {
    /* a Hash receiver renders each entry as a boxed [k, v] pair, which a
       two-parameter block autosplats -- the same binding the spliced loops do */
    int te = ++g_tmp;
    emit_indent(g_pre, bi);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d); SP_GC_ROOT_RBVAL(_t%d);\n", te, trecv, ti, te);
    emit_autosplat_params(c, block, nparam, te, bi);
  }
  else if (p0) {
    Scope *blkv = comp_scope_of(c, block);
    LocalVar *plv = (blkv && p0raw) ? scope_local(blkv, p0raw) : NULL;
    TyKind pt = plv ? plv->type : TY_POLY;
    char src[64]; snprintf(src, sizeof src, "sp_poly_each_elem(_t%d, _t%d)", trecv, ti);
    emit_indent(g_pre, bi);
    emit_block_param_from_boxed(c, p0, pt, src, g_pre);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bi);
  int sv = g_indent; g_indent = bi;
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb);
  g_indent = sv;
  /* Ruby truthiness: a poly condition consults the tag, a nil/void one is
     constant false, and every other type is truthy even at zero. */
  TyKind bvt = comp_ntype(c, bb[bn - 1]);
  emit_indent(g_pre, bi);
  if (bvt == TY_NIL || bvt == TY_VOID)
    buf_printf(g_pre, "(void)(%s);\n", cb.p ? cb.p : "0");
  else if (bvt == TY_POLY || bvt == TY_UNKNOWN)
    buf_printf(g_pre, "if (sp_poly_truthy(%s)) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
  else if (bvt == TY_BOOL)
    buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
  else
    buf_printf(g_pre, "{ (void)(%s); _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
  free(cb.p);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (comp_ntype(c, id) == TY_INT) buf_printf(b, "_t%d", tres);
  else buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tres, tres);
  return 1;
}

/* The answer of an any?/all?/none?/one? loop: its count test, boxed when
   the call's slot is (a poly receiver whose candidate classes answer the
   name with something other than a Boolean); the raw test assigned into an
   sp_RbVal was a C error (#4961). */
static void pred_fold_answer(Compiler *c, int id, int tacc, int is_all, int is_any, int is_none, Buf *b) {
  int boxed = comp_ntype(c, id) == TY_POLY;
  if (boxed) buf_puts(b, "sp_box_bool(");
  if (is_all) buf_printf(b, "_t%d", tacc);
  else if (is_any) buf_printf(b, "(_t%d > 0)", tacc);
  else if (is_none) buf_printf(b, "(_t%d == 0)", tacc);
  else buf_printf(b, "(_t%d == 1)", tacc);
  if (boxed) buf_puts(b, ")");
}

int emit_predicate_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_all = sp_streq(name, "all?"), is_any = sp_streq(name, "any?"),
      is_none = sp_streq(name, "none?"), is_one = sp_streq(name, "one?");
  if (!(is_all || is_any || is_none || is_one)) return 0;
  int block = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int range_recv = (rt == TY_RANGE);
  int poly_recv = (rt == TY_POLY);
  if (!ty_is_array(rt) && !range_recv && !poly_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k && !poly_recv) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* The block's last expression is the loop condition. A bool emits as-is; a
     type that is ALWAYS truthy in Ruby (int/float/string/symbol/object -- even
     0 and 0.0 are truthy) emits as constant true after evaluating for effect;
     nil is constant false; a poly value routes through sp_poly_truthy. This is
     the Ruby truthiness a bare `if (value)` would get wrong (#3141). */
  TyKind bvt = comp_ntype(c, bb[bn - 1]);
  int pred_kind;
  if (bvt == TY_BOOL) pred_kind = PRED_BOOL;
  else if (bvt == TY_NIL || bvt == TY_VOID) pred_kind = PRED_NEVER;
  else if (bvt == TY_POLY || bvt == TY_UNKNOWN) pred_kind = PRED_POLY;
  else pred_kind = PRED_ALWAYS;   /* int/float/string/sym/object/... : always truthy */

  const char *p0raw = block_param_name(c, block, 0);
  const char *p0 = p0raw ? rename_local(p0raw) : NULL;
  int trecv = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;

  if (poly_recv) {
    /* boxed receiver (a widened array): the same runtime-dispatch loop
       poly `each` uses -- sp_poly_arr_len_ex + sp_poly_each_elem, the block
       param unboxed to its analyzed type. */
    int tlen = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", trecv, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", trecv);
    emit_indent(g_pre, g_indent);
    emit_poly_iter_obj_normalize(c, trecv, g_pre);
    emit_indent(g_pre, g_indent);
    emit_poly_iter_obj_reject(c, trecv, name, g_pre);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_int _t%d = sp_poly_arr_len_ex(_t%d);\n", tlen, trecv);
    emit_indent(g_pre, g_indent);
    if (is_all) buf_printf(g_pre, "sp_bool _t%d = TRUE;\n", tacc);
    else buf_printf(g_pre, "sp_int _t%d = 0;\n", tacc);
    emit_indent(g_pre, g_indent);
    /* A cached Array length visits removed elements or misses appended ones.
       Keep the existing traversal bound for other boxed receivers. */
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < "
               "(sp_rbval_is_array(_t%d) ? sp_poly_arr_len(_t%d) : _t%d); _t%d++) {\n",
               ti, ti, trecv, trecv, tlen, ti);
    int bodyIndentP = g_indent + 1;
    {
      /* A multi-param block over a boxed receiver auto-splats each element,
         exactly as the sibling element loops do: `h.none? { |k, v| ... }` on a
         boxed Hash walks [key, value] pairs, and binding only the first param
         left the second nil (#3448). */
      char src[64]; snprintf(src, sizeof src, "sp_poly_each_elem(_t%d, _t%d)", trecv, ti);
      int splatP = emit_iter_autosplat(c, block, TY_POLY_ARRAY, src, bodyIndentP);
      if (!splatP && p0) {
        Scope *blkv = comp_scope_of(c, block);
        LocalVar *plv = (blkv && p0raw) ? scope_local(blkv, p0raw) : NULL;
        /* A parameter the body never reads was never interned, so it has no
           declaration to assign into: binding it emitted `lv_x = ...` for an
           undeclared name and the C build failed (#3967). */
        if (plv) {
          emit_indent(g_pre, bodyIndentP);
          emit_block_param_from_boxed(c, p0, plv->type, src, g_pre);
        }
      }
    }
    /* A `next <value>` leaves the block WITH that value, which is the
       predicate's answer. Emitting the leading statements and then the tail
       expression drops it: the next became a bare `continue` and the
       condition read the tail, so `any? { next true; false }` answered false
       (#4301). emit_block_value_into is the machinery for exactly this --
       it wraps the body so a next assigns the slot and falls through. */
    int nx_used = 0;
    Buf vb; memset(&vb, 0, sizeof vb);
    int saveIndentP = g_indent;
    if (fold_body_has_next(c, body)) {
      int tnv = ++g_tmp;
      emit_indent(g_pre, bodyIndentP);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tnv);
      emit_indent(g_pre, bodyIndentP);
      buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tnv);
      char dest[24]; snprintf(dest, sizeof dest, "_t%d", tnv);
      g_indent = bodyIndentP;
      emit_block_value_into(c, block, dest, 1, bodyIndentP);
      g_indent = saveIndentP;
      buf_printf(&vb, "sp_poly_truthy(_t%d)", tnv);
      nx_used = 1;
    }
    if (!nx_used) {
      for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bodyIndentP);
      g_indent = bodyIndentP;
      emit_expr(c, bb[bn - 1], &vb);
      g_indent = saveIndentP;
    }
    emit_indent(g_pre, bodyIndentP);
    emit_pred_cond(g_pre, nx_used ? PRED_BOOL : pred_kind, vb.p ? vb.p : "0", tacc, is_all);
    free(vb.p);
    if (!is_all) {
      emit_indent(g_pre, bodyIndentP);
      buf_printf(g_pre, "if (_t%d > %d) break;\n", tacc, is_one ? 1 : 0);
    }
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "}\n");

    pred_fold_answer(c, id, tacc, is_all, is_any, is_none, b);
    return 1;
  }

  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_recv) {
    int tr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
    buf_printf(&rb, "sp_range_to_ia(_t%d)", tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  if (is_all) buf_printf(g_pre, "sp_bool _t%d = TRUE;\n", tacc);
  else buf_printf(g_pre, "sp_int _t%d = 0;\n", tacc);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  int bodyIndent = g_indent + 1;
  char es_pr[64]; snprintf(es_pr, sizeof es_pr, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
  if (!emit_iter_autosplat(c, block, rt, es_pr, bodyIndent) && p0) {
    /* the block param's declared type may be wider than the array's element
       type -- a numbered `_1` shared across differently-typed blocks widens
       to poly -- so box the element when the slot is poly (#3141). */
    Scope *pbs = comp_scope_of(c, block);
    LocalVar *pblv = (pbs && p0raw) ? scope_local(pbs, p0raw) : NULL;
    TyKind pbt = pblv ? pblv->type : ty_array_elem(rt);
    emit_indent(g_pre, bodyIndent);
    /* Declare the block param in the loop body (not a bare assignment) so
       this is self-contained: when the call is a parameter default hoisted
       to the call site, the enclosing function has no top-level declaration
       for the block local (find/detect's own arms already do this).
       Shadows the method-scope slot in the ordinary in-body case, which is
       harmless. */
    if (pbt == TY_POLY && ty_array_elem(rt) != TY_POLY) {
      /* poly slot fed by a concrete element: box it */
      emit_ctype(c, pbt, g_pre);
      buf_printf(g_pre, " lv_%s = ", p0);
      emit_boxed_text(c, ty_array_elem(rt), es_pr, g_pre);
      buf_puts(g_pre, ";\n");
    }
    else {
      emit_ctype(c, pbt, g_pre);
      buf_printf(g_pre, " lv_%s = %s;\n", p0, es_pr);
    }
  }
  /* see the sibling above: a `next <value>` is the block's answer, and
     emitting the leading statements plus the tail expression drops it */
  int nx2 = 0;
  Buf vb; memset(&vb, 0, sizeof vb);
  int saveIndent = g_indent;
  if (fold_body_has_next(c, body)) {
    int tnv2 = ++g_tmp;
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", tnv2);
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tnv2);
    char dest2[24]; snprintf(dest2, sizeof dest2, "_t%d", tnv2);
    g_indent = bodyIndent;
    emit_block_value_into(c, block, dest2, 1, bodyIndent);
    g_indent = saveIndent;
    buf_printf(&vb, "sp_poly_truthy(_t%d)", tnv2);
    nx2 = 1;
  }
  if (!nx2) {
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bodyIndent);
    g_indent = bodyIndent;
    emit_expr(c, bb[bn - 1], &vb);
    g_indent = saveIndent;
  }
  emit_indent(g_pre, bodyIndent);
  emit_pred_cond(g_pre, nx2 ? PRED_BOOL : pred_kind, vb.p ? vb.p : "0", tacc, is_all);
  free(vb.p);
  if (!is_all) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "if (_t%d > %d) break;\n", tacc, is_one ? 1 : 0);
  }
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  pred_fold_answer(c, id, tacc, is_all, is_any, is_none, b);
  return 1;
}

/* grep/grep_v are Ruby definitions now (builtins/enumerable.rb); the C
   fold this used to serve (a typed Array receiver, ty_is_array only --
   Hash/Range/Enumerator/object/poly receivers never reached it) is gone
   with it. See desugar_builtin_enum_calls. */


/* Emit the value for callee param `idx`: the provided arg node if any,
   else the param's default (a nil default becomes the type's default). */
/* An object pointer flowing into a slot declared as one of its ANCESTOR
   classes. Each class gets its own C struct, and a subclass replicates its
   parent's fields in order at the same offsets, so the pointer is layout-
   compatible and the conversion is a no-op at run time -- but C still requires
   it spelled out. Clang only warns; GCC 14 made -Wincompatible-pointer-types an
   error by default, so the same emitted C built on one host and not the other
   (#3418). Emits nothing when the two types are unrelated: a cast there would
   paper over a real mismatch. */
void emit_obj_upcast_prefix(Compiler *c, TyKind slot, TyKind val, Buf *b) {
  if (!ty_is_object(slot) || !ty_is_object(val)) return;
  int sc = ty_object_class(slot), vc = ty_object_class(val);
  if (sc < 0 || vc < 0 || sc == vc) return;
  if (c->classes[sc].is_value_type || c->classes[vc].is_value_type) return;
  for (int k = c->classes[vc].parent; k >= 0; k = c->classes[k].parent)
    if (k == sc) { buf_printf(b, "(sp_%s *)", c->classes[sc].c_name); return; }
}

/* Which actual argument fills parameter `idx`, or -1 for its default.

   Ruby funds the required parameters first and spends what is left on the
   optional ones, so with a leading optional (`def f(x = 1, y)`) the single
   argument of `f(8)` goes to `y` and `x` takes its default. Reading argv[idx]
   positionally put it in the wrong slot and left the required parameter at its
   zero value, and the arity check -- walking the list and raising at the first
   undefaulted parameter past the argument count -- rejected the call outright.

   Everything here is gated on opt_before_required, which is false for every
   conventional shape: the rest of the file's reading of a parameter list,
   including Scope's documented "requireds then optionals" order and
   nrequired's index-past-the-last-required meaning, is left exactly as it
   was. Prism files a required parameter after an optional under "posts",
   so an optional with posts beside it is one Ruby funds late. nrequired --
   the index past the LAST required parameter -- could not say it: a required
   KEYWORD counts there too, and `def f(a, b = 0, k:)` read as a leading
   optional. It is still the answer for a scope with no def's parameter list
   to read (a synthesized one, a block's). */
int opt_before_required(Compiler *c, Scope *m) {
  int pn = m->def_node >= 0 ? nt_ref(c->nt, m->def_node, "parameters") : -1;
  if (pn >= 0 && nt_kind(c->nt, pn) == NK_ParametersNode) {
    int on = 0, postn = 0;
    nt_arr(c->nt, pn, "optionals", &on);
    nt_arr(c->nt, pn, "posts", &postn);
    return on > 0 && postn > 0;
  }
  if (pn < 0 && m->def_node >= 0 && nt_kind(c->nt, m->def_node) == NK_DefNode) return 0;
  for (int i = 0; i < m->nrequired && i < m->nparams; i++)
    if (m->pdefault && m->pdefault[i] >= 0) return 1;
  return 0;
}
int arg_slot_for_param(Compiler *c, Scope *m, int idx, int argc) {
  if (idx < 0 || idx >= m->nparams) return -1;
  if (!opt_before_required(c, m)) return idx < argc ? idx : -1;
  /* keywords and a **rest sit in pnames too but take no positional
     argument; map over the positional prefix only. A **kw was mapped by
     position once, so `def m(a = 1, b, **kw)` given one argument bound it
     to a and left b nil. */
  int n = m->nparams;
  while (n > 0 && (n - 1 == m->kwrest_idx || callee_param_is_declared_kwarg(c, m, m->pnames[n - 1]))) n--;
  if (idx >= n || idx == m->rest_idx) return -1;
  int pre = 0;
  while (pre < n && (!m->pdefault || m->pdefault[pre] < 0)) pre++;
  int opt_end = pre;
  while (opt_end < n && m->pdefault && m->pdefault[opt_end] >= 0) opt_end++;
  /* a *rest sits between the optionals and the trailing requireds and takes
     only what is left once both are funded: `def r(a = {}, *rest, c)` called
     `r(5)` binds c and leaves a at its default, where mapping it positionally gave a the 5 too */
  int post_from = (m->rest_idx >= 0 && m->rest_idx == opt_end) ? opt_end + 1 : opt_end;
  if (post_from >= n) return idx < argc ? idx : -1;   /* optionals trail after all */
  int post = n - post_from;
  if (idx < pre) return idx < argc ? idx : -1;
  if (idx < opt_end) {
    int avail = argc - pre - post;      /* optionals this call can fund */
    int k = idx - pre;
    return k < avail ? pre + k : -1;
  }
  int at = argc - (n - idx);            /* trailing required: count from the end */
  return at >= pre ? at : -1;
}

/* Does scope mi's body mutate the array held by its local `name` in place: a
   receiver-mutating Array method on a plain read of it, or the name passed
   on, positionally, into a method whose parameter is mutated the same way
   (depth-limited)? Asked about a poly-array parameter a caller wants to
   pass a TYPED array to. */
static int scope_mutates_array_local(Compiler *c, int mi, const char *name, int depth) {
  const NodeTable *nt = c->nt;
  if (depth > 3 || mi < 0) return 0;
  for (int q = 0; q < nt->count; q++) {
    if (c->nscope[q] != mi || nt_kind(nt, q) != NK_CallNode) continue;
    int r = nt_ref(nt, q, "receiver");
    if (r >= 0 && nt_kind(nt, r) == NK_LocalVariableReadNode) {
      const char *rn = nt_str(nt, r, "name");
      if (rn && sp_streq(rn, name) && array_mutator_name(nt_str(nt, q, "name"))) return 1;
    }
    /* passed on: only a receiverless (or self) call to a user method resolves
       statically enough to follow */
    const char *cnm = nt_str(nt, q, "name");
    if (!cnm || (r >= 0 && nt_kind(nt, r) != NK_SelfNode)) continue;
    int aa = nt_ref(nt, q, "arguments"); int an = 0;
    const int *av = aa >= 0 ? nt_arr(nt, aa, "arguments", &an) : NULL;
    for (int k = 0; k < an; k++) {
      if (nt_kind(nt, av[k]) != NK_LocalVariableReadNode) continue;
      const char *vn = nt_str(nt, av[k], "name");
      if (!vn || !sp_streq(vn, name)) continue;
      Scope *cs = &c->scopes[mi];
      int tmi = cs->class_id >= 0 ? comp_method_in_chain(c, cs->class_id, cnm, NULL) : -1;
      if (tmi < 0) tmi = comp_method_index(c, cnm);
      if (tmi < 0 || k >= c->scopes[tmi].nparams || !c->scopes[tmi].pnames[k]) continue;
      if (scope_mutates_array_local(c, tmi, c->scopes[tmi].pnames[k], depth + 1)) return 1;
    }
  }
  return 0;
}

static void emit_arg_or_default_at(Compiler *c, Scope *m, int idx, int provided, Buf *out);

/* A write to a shared-mutable-string local, `buf = +"abc"`, where the value
   has to be the HANDLE (a shared-string parameter's argument). Its node type
   is String, so the value forms produce the const char * copy; run the write
   as a statement instead and yield the local's sp_String *. A write already
   hoisted into a temp (emit_args_filled) is that temp. Returns 0 for any
   other node. */
static int emit_strbuf_local_write_handle(Compiler *c, int node, Buf *out) {
  if (node < 0 || nt_kind(c->nt, node) != NK_LocalVariableWriteNode) return 0;
  const char *nm = nt_str(c->nt, node, "name");
  LocalVar *lv = nm ? scope_local(comp_scope_of(c, node), nm) : NULL;
  if (!lv || lv->type != TY_STRBUF) return 0;
  for (int i = g_n_argov - 1; i >= 0; i--)
    if (g_argov_node[i] == node) { buf_puts(out, g_argov_text[i]); return 1; }
  buf_puts(out, "({ ");
  emit_assign(c, node, out, 0);
  buf_puts(out, " ");
  emit_local_ref(c, node, nm, out);
  buf_puts(out, "; })");
  return 1;
}

/* A parameter a closure in its method captures is a heap cell, and a default
   that builds such a closure (`b: -> { a }`) captures the hoisted alias through
   its cell spelling: give the alias `lv_<uniq>` that cell too. */
static void emit_pd_cell_alias(Compiler *c, LocalVar *plv, const char *uniq) {
  if (!plv || !plv->is_cell || plv->byref_out) return;
  emit_inlined_local_decl(c, plv, uniq, g_pre, g_indent);
  emit_indent(g_pre, g_indent);
  if (plv->type == TY_PROC) buf_printf(g_pre, "*_cell_%s = (sp_int)(uintptr_t)lv_%s;\n", uniq, uniq);
  else buf_printf(g_pre, "*_cell_%s = lv_%s;\n", uniq, uniq);
}

/* A default of a method spliced in place (see g_inl_dflt_scope) is callee
   code: an earlier parameter is the inline's renamed local, and self is the
   receiver. Setup the default hoists would land ahead of the whole call,
   before those parameters are bound, so it stays inside the value. */
static const Scope *g_inl_dflt_on_recv = NULL;
static void emit_inlined_default(Compiler *c, Scope *m, int idx, Buf *out) {
  const Scope *sv_on_recv = g_inl_dflt_on_recv;
  g_inl_dflt_on_recv = g_inl_dflt_self ? m : NULL;
  int sv_nren = g_nren, sv_indent = g_indent, sv_cls = g_emitting_class_id;
  const char *sv_self = g_self, *sv_deref = g_self_deref;
  Buf *sv_pre = g_pre;
  g_nren = g_inl_dflt_nren;
  if (g_inl_dflt_self) { g_self = g_inl_dflt_self; g_self_deref = g_inl_dflt_deref; }
  if (g_inl_dflt_class >= 0) g_emitting_class_id = g_inl_dflt_class;
  g_inl_dflt_scope = NULL;
  Buf pre; memset(&pre, 0, sizeof pre);
  Buf val; memset(&val, 0, sizeof val);
  g_pre = &pre; g_indent = 0;
  /* a class method inherited by the receiving class builds that class's
     object where the slot is typed with the defining class's */
  LocalVar *pv = m->pnames[idx] ? scope_local(m, m->pnames[idx]) : NULL;
  if (pv && ty_is_object(pv->type) && !comp_ty_value_obj(c, pv->type) &&
      g_emitting_class_id != ty_object_class(pv->type))
    buf_printf(&val, "(sp_%s *)", c->classes[ty_object_class(pv->type)].c_name);
  emit_arg_or_default_at(c, m, idx, -1, &val);
  g_inl_dflt_on_recv = sv_on_recv;
  g_pre = sv_pre; g_indent = sv_indent;
  g_inl_dflt_scope = m;
  g_nren = sv_nren; g_self = sv_self; g_self_deref = sv_deref; g_emitting_class_id = sv_cls;
  if (pre.len) buf_printf(out, "({\n%s%s; })", pre.p, val.p ? val.p : "");
  else buf_puts(out, val.p ? val.p : "");
  free(pre.p); free(val.p);
}

/* `+"lit"` bound to parameter `idx` of `m`, which is the shared handle
   because a Method naming the method reaches it (LocalVar.dyn_handle): the
   handle is made straight from the literal (sp_String_new_unfrozen). */
static int dyn_handle_lit_arg(Compiler *c, Scope *m, int idx, int arg) {
  const NodeTable *nt = c->nt;
  if (!m || idx < 0 || idx >= m->nparams || !m->pnames[idx] || arg < 0) return 0;
  LocalVar *p = scope_local(m, m->pnames[idx]);
  if (!p || !p->dyn_handle || p->type != TY_STRBUF || nt_kind(nt, arg) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, arg, "name");
  int r = nt_ref(nt, arg, "receiver");
  return nm && sp_streq(nm, "+@") && nt_ref(nt, arg, "arguments") < 0 && r >= 0 &&
         nt_kind(nt, r) == NK_StringNode;
}
/* A default a dispatch arm omits runs on the receiver as the arm's class: the
   caller's self may be another class, or none at all at top level (#4873). */
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  if (provided < 0 && g_inl_dflt_scope == m && g_inl_dflt_depth == g_expr_depth && m->pdefault &&
      m->pdefault[idx] >= 0) {
    emit_inlined_default(c, m, idx, out);
    return;
  }
  if (provided >= 0 || !g_arm_self || g_arm_scope != m || g_arm_depth != g_expr_depth ||
      !m->pdefault || m->pdefault[idx] < 0) {
    emit_arg_or_default_at(c, m, idx, provided, out);
    return;
  }
  const char *sv_self = g_self, *sv_deref = g_self_deref, *sv_arm = g_arm_self;
  int sv_emcls = g_emitting_class_id;
  g_self = g_arm_self;
  g_self_deref = "->";
  g_emitting_class_id = m->class_id;
  g_arm_self = NULL;
  emit_arg_or_default_at(c, m, idx, provided, out);
  g_self = sv_self; g_self_deref = sv_deref; g_arm_self = sv_arm;
  g_emitting_class_id = sv_emcls;
}

static void emit_arg_or_default_fill(Compiler *c, Scope *m, int idx, int provided, Buf *out);

/* A default is emitted in place at the call site, so one whose expression
   omits the same argument of the same method again has no finite emission.
   desugar_recursive_param_defaults moves such a default into a method of its
   own; one it has to leave (it reads the caller's block, or `super`) is
   refused here rather than recursing until the compiler's stack runs out.
   The same default may be entered twice legitimately: the by-reference path
   re-enters for the plain value. A silent probe that unwinds past an open
   default restores g_open_defaults along with its other state. */
enum { OPEN_DEFAULTS_MAX = 256 };
static struct { Scope *m; int idx; } g_open_default[OPEN_DEFAULTS_MAX];
int g_open_defaults = 0;

static void emit_arg_or_default_at(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  if (provided >= 0 || !m->pdefault || m->pdefault[idx] < 0) {
    emit_arg_or_default_fill(c, m, idx, provided, out);
    return;
  }
  if (g_open_defaults >= OPEN_DEFAULTS_MAX)
    unsupported(c, m->pdefault[idx], "parameter defaults nested more than 256 deep at one call site");
  int seen = 0;
  for (int i = 0; i < g_open_defaults; i++)
    if (g_open_default[i].m == m && g_open_default[i].idx == idx) seen++;
  if (seen >= 2) {
    char msg[320];
    snprintf(msg, sizeof msg,
             "default of `%s`'s parameter `%s` that calls `%s` again with that argument omitted "
             "(one reading the caller's block or `super` cannot be moved into a method of its own)",
             m->name ? m->name : "?", m->pnames[idx] ? m->pnames[idx] : "?",
             m->name ? m->name : "?");
    unsupported(c, m->pdefault[idx], msg);
  }
  g_open_default[g_open_defaults].m = m;
  g_open_default[g_open_defaults].idx = idx;
  g_open_defaults++;
  int sv_nren = declare_default_locals(c, m, m->pdefault[idx]);
  emit_arg_or_default_fill(c, m, idx, provided, out);
  g_nren = sv_nren;
  g_open_defaults--;
}

/* A byref parameter (LocalVar.byref_out) takes a slot, and a value with no
   caller variable behind it -- a literal, an expression, a default, what a
   binder pulls out of a splat, a gather or a `**` hash -- has none to lend:
   it binds into a rooted temp and the temp's address is passed. The callee's
   appends stay in the temp, as the pre-byref value ABI kept them. */
void emit_lent_temp(const char *val, Buf *out) {
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "const char *_t%d = %s;\n", t, val ? val : "NULL");
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  buf_printf(out, "&_t%d", t);
}

/* The slot a String local `vn` (lv its variable, when the scope declares it)
   lends a byref parameter, into out; 0 when it has none and the caller lends
   a temp (emit_lent_temp). A plain local passes its address, a heap cell
   itself, and inside a proc body the capture struct's cell pointer. What a
   call site's local argument lends, and a bare `super`'s forwarded parameter
   (emit_zsuper_arg): the two are the same lending, and a super that spelled
   its own passed a captured parameter's value in a temp, where the parent's
   appends stayed. */
int emit_lent_local(LocalVar *lv, const char *vn, Buf *out) {
  if (!vn) return 0;
  /* Forwarding a by-reference parameter pins nothing: the cell is
     whatever the ORIGINAL lending site handed down, and that site
     already decided. Pinning here would offer a stack address to
     sp_gc_pin_remembered, which reads a header off it -- the fault
     #4391's first half was. */
  int fwd = lv && (lv->byref_out || lv->inline_alias);   /* an inline alias is a forward too: it points at whatever the caller lent */
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, vn)) {
    /* a capture of another type has a cell of that type, no String slot */
    if (lv && lv->type != TY_STRING) return 0;
    /* inside a proc body: the capture struct holds the cell pointer */
    if (!fwd) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_gc_pin_remembered((void *)((%s *)_cap)->c_%s);\n",
                 g_cap_struct, vn);
    }
    buf_printf(out, "((%s *)_cap)->c_%s", g_cap_struct, vn);
    return 1;
  }
  if (!lv || lv->type != TY_STRING) return 0;
  if (lv->is_cell) {
    /* a heap cell: the callee stores through it and cannot name it, so
       the owner is recorded HERE, stickily, since this runs before the
       store rather than after (#4391) */
    if (!fwd) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_gc_pin_remembered((void *)_cell_%s);\n", rename_local(vn));
    }
    /* renamed like the plain slot below: inside an inlined body the
       cell is the inline's own (or its alias of the caller's) */
    buf_printf(out, "_cell_%s", rename_local(vn));
    return 1;
  }
  /* Keep a borrowed slot's volatile qualifier through the callee's stores:
     the caller may read it after a setjmp/longjmp. */
  buf_printf(out, "&lv_%s", rename_local(vn));
  return 1;
}

/* The handles emit_arg_temp took of the shared String slots it ran first:
   each names the override it rode with -- its index in g_argov, the node,
   and the value temp that override reads -- and the temp holding the handle.
   An override the call has dropped since (a later call reuses its index)
   is pruned when the next one is taken, so the list is no longer than the
   overrides live at once. */
typedef struct { int idx, node, t, th; } RanHandle;
static RanHandle *g_ran_hnd;
static int g_n_ran_hnd, g_cap_ran_hnd;

/* The handle temp emit_arg_temp took of the shared String slot `node` that
   the call ran first, -1 when there is none. It is the one recorded with the
   live override itself. Guessing it as the value temp's number one down,
   declared anywhere in the statement's pending lines, matched another
   String's temp when the variable held no handle: `h((buf.upcase!; 1), d,
   (d = +"q"; 2))` bound the block-scoped temp upcase! took of buf. */
int ran_first_handle(int node) {
  for (int i = g_n_argov - 1; i >= 0; i--) {
    if (g_argov_node[i] != node) continue;
    int t;
    if (sscanf(g_argov_text[i], "_t%d", &t) != 1) return -1;
    for (int j = 0; j < g_n_ran_hnd; j++)
      if (g_ran_hnd[j].idx == i && g_ran_hnd[j].node == node && g_ran_hnd[j].t == t)
        return g_ran_hnd[j].th;
    return -1;
  }
  return -1;
}

static void emit_arg_or_default_fill(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_INT;
  /* An omitted `*rest` is an empty Array, not a NULL the body reads as nil:
     a dispatch arm that calls with no arguments (the boxed `call` switch)
     fills every parameter through here. */
  if (provided < 0 && idx == m->rest_idx && ty_is_array(pt) && !ty_is_obj_array(pt) &&
      !(p && p->byref_out)) {
    const char *k = (pt == TY_POLY_ARRAY) ? "Poly" : array_kind(pt);
    if (k) { buf_printf(out, "sp_%sArray_new()", k); return; }
  }
  /* ...and an omitted `**kwrest` an empty Hash: the same arm handed it
     NULL, which `o.class`, `o.is_a?(Hash)` and `o.empty?` read as nil. */
  if (provided < 0 && idx == m->kwrest_idx && (ty_is_hash(pt) || pt == TY_POLY) &&
      !(p && p->byref_out)) {
    const char *hcn = ty_is_hash(pt) ? ty_hash_cname(pt) : NULL;
    if (hcn) { buf_printf(out, "sp_%sHash_new()", hcn); return; }
    if (pt == TY_POLY) { buf_puts(out, "sp_box_obj(sp_SymPolyHash_new(), SP_BUILTIN_SYM_POLY_HASH)"); return; }
  }
  /* A nil-typed argument (a void call, an always-nil method) into a pointer
     parameter: evaluated for its effects, it passes the pointer's nil. Raw,
     the void call or its sp_int 0 was a C type error (#4930). */
  if (provided >= 0 && needs_root(pt) && pt != TY_POLY && !comp_ty_value_obj(c, pt) &&
      nt_kind(c->nt, provided) != NK_NilNode && !(p && (p->byref_out || p->str_shared))) {
    TyKind at = comp_ntype(c, provided);
    if (at == TY_NIL || at == TY_VOID) {
      buf_puts(out, "({ (void)("); emit_expr(c, provided, out); buf_puts(out, "); NULL; })");
      return;
    }
  }
  /* A String that is the shared handle, handed to an initialize that only
     reads its parameter (#6179): the handle's live bytes, where its plain
     read copies the whole String for a callee that may keep it. A String an
     appending initialize made the handle is read by other constructors too,
     and the copy per construction was most of their cost. */
  if (provided >= 0 && pt == TY_STRING && p && !p->byref_out && m->name && sp_streq(m->name, "initialize") &&
      nt_kind(c->nt, provided) == NK_LocalVariableReadNode && !c->strbuf_box[provided] &&
      !arg_ran_first(provided, 0)) {
    const char *vn = nt_str(c->nt, provided, "name");
    Scope *vs = vn ? comp_scope_of(c, provided) : NULL;
    LocalVar *lv = vs ? scope_local(vs, vn) : NULL;
    if (lv && !lv->is_cell && lv->type == TY_STRBUF && lv->str_shared &&
        ctor_param_reads_only(c, (int)(m - c->scopes), idx)) {
      Buf lr; memset(&lr, 0, sizeof lr);
      emit_local_ref(c, provided, vn, &lr);
      if (lv->dyn_handle) buf_printf(out, "(%s ? sp_String_cstr(%s) : NULL)", lr.p, lr.p);
      else buf_printf(out, "sp_String_cstr(%s)", lr.p);
      free(lr.p);
      return;
    }
  }
  /* A hash argument of a different KIND than the parameter's slot: the two are
     different C structs, so the assignment is not one C accepts. It is reachable
     through an RBS seed, which pins a parameter to `Hash[Symbol, untyped]` while
     the caller's own inference widens the value to the boxed-key hash (#3994).
     Convert through the runtime, which walks the pairs and raises on a key the
     target kind cannot hold. */
  if (provided >= 0 && ty_is_hash(pt) && hash_box_cls(pt)) {
    TyKind at = comp_ntype(c, provided);
    const char *conv = pt == TY_SYM_POLY_HASH ? "sp_seed_sym_hash_arg" : NULL;
    if (conv && ty_is_hash(at) && at != pt && hash_box_cls(at)) {
      buf_printf(out, "%s(sp_box_obj(", conv);
      emit_expr(c, provided, out);
      buf_printf(out, ", %s))", hash_box_cls(at));
      return;
    }
  }
  /* An empty `[]` argument carries no element type of its own and otherwise
     falls back to an IntArray, which mismatches a parameter typed from another
     call site (`P.new(deps: ["d1"])` then `P.new(deps: [])`). Build it at the
     parameter's type instead, the way the ternary arms already do (#3359). */
  if (provided >= 0 && ty_is_array(pt) && !ty_is_obj_array(pt) &&
      nt_kind(c->nt, provided) == NK_ArrayNode) {
    int en = 0; nt_arr(c->nt, provided, "elements", &en);
    if (en == 0) {
      const char *k = (pt == TY_POLY_ARRAY) ? "Poly" : array_kind(pt);
      if (k) { buf_printf(out, "sp_%sArray_new()", k); return; }
    }
  }
  /* Byref string out-param: pass the caller's slot (const char**) so the
     callee's mutation lands in the caller's variable. A plain string local
     passes its address; an already-celled local (captured, or itself a byref
     param) passes its cell. Anything else -- literal, expression, ivar,
     filled default -- materializes a rooted temp and passes that: for a
     non-lvalue argument CRuby's mutation is equally invisible to the caller,
     for the rest this keeps the pre-byref behavior. */
  /* shared-handle string parameter (#3227 P5): a shared slot argument passes
     the handle itself; a plain value wraps a fresh handle (a non-lvalue
     argument's mutation is invisible to the caller in CRuby too). */
  /* `show(buf = +"abc")`: the write runs, and the argument is the local's
     handle -- the parameter and the local are one object. The analyzer
     types the parameter from the write, which is the local's sp_String *,
     while the write's value form is the const char * copy. */
  if (p && pt == TY_STRBUF && emit_strbuf_local_write_handle(c, provided, out)) return;
  if (p && pt == TY_STRBUF && p->str_shared) {
    if (provided >= 0) {
      char srefP[192];
      /* A variable the call ran first is read where it ran, not at its
         slot (see the byref slot below), which a later argument can
         overwrite. What it read then is an OBJECT: the handle taken with
         it, so the callee and the caller's other names still share one
         String. A fresh handle of the bytes read was a second String --
         a caller writing into its own afterwards went unseen. */
      NodeKind pk = nt_kind(c->nt, provided);
      int late = (pk == NK_LocalVariableReadNode || pk == NK_InstanceVariableReadNode) &&
                 arg_ran_first(provided, 0);
      if (late) {
        int th = ran_first_handle(provided);
        if (th >= 0) { buf_printf(out, "_t%d", th); return; }
      }
      if (!late && strbuf_slot_ref(c, provided, srefP, sizeof srefP)) {
        buf_puts(out, srefP);
        return;
      }
      /* A parameter that is the handle because a Method reaches it (#6179):
         a literal or a temporary is a String nobody else holds, so the
         handle is made with its bytes inside the object, and `+"lit"`
         straight from the literal rather than from a copy of it. */
      if (!arg_ran_first(provided, 0) && dyn_handle_lit_arg(c, m, idx, provided)) {
        buf_puts(out, "sp_String_new_unfrozen(");
        emit_expr(c, nt_ref(c->nt, provided, "receiver"), out);
        buf_puts(out, ")");
        return;
      }
      /* nil, a default's or an argument's, is the NULL handle a nullable
         handle parameter reads as nil (`def initialize(o: nil)`) */
      if (comp_ntype(c, provided) == TY_NIL) { buf_puts(out, "NULL"); return; }
      /* a boxed argument that holds nil at run time is the NULL handle too:
         converted as a String first, it raised TypeError where CRuby binds
         nil (`n = nil` beside a String, `def go(v) = run(v) { |t| ... }`) */
      if (comp_ntype(c, provided) == TY_POLY) {
        int tpv = ++g_tmp;
        buf_printf(out, "({ sp_RbVal _t%d = ", tpv);
        emit_expr(c, provided, out);
        buf_printf(out, "; SP_GC_ROOT_RBVAL(_t%d); sp_poly_nil_p(_t%d) ? NULL : %s(sp_poly_arg_str_chk(_t%d)); })", tpv, tpv,
                   p->dyn_handle && pk != NK_LocalVariableReadNode && pk != NK_InstanceVariableReadNode
                     ? "sp_String_new_fresh" : "sp_String_new_shared", tpv);
        return;
      }
      buf_puts(out, p->dyn_handle && pk != NK_LocalVariableReadNode && pk != NK_InstanceVariableReadNode
                      ? "sp_String_new_fresh(" : "sp_String_new_shared(");
      emit_str_expr(c, provided, out);
      buf_puts(out, ")");
      return;
    }
    int dvP = m->pdefault[idx];
    if (dvP >= 0 && comp_ntype(c, dvP) == TY_NIL) { buf_puts(out, "NULL"); return; }
    /* a default that is an earlier parameter binds that parameter's handle,
       the one String both names hold (promote_default_alias_params) */
    { char srefD[192];
      if (dvP >= 0 && nt_kind(c->nt, dvP) == NK_LocalVariableReadNode &&
          strbuf_slot_ref(c, dvP, srefD, sizeof srefD)) {
        buf_puts(out, srefD);
        return;
      } }
    buf_puts(out, "sp_String_new_shared(");
    if (dvP >= 0) emit_str_expr(c, dvP, out);
    else buf_puts(out, "(&(\"\\xff\")[1])");
    buf_puts(out, ")");
    return;
  }
  if (p && p->byref_out) {
    /* A variable the call ran first (emit_args_before_binding) is one a
       later argument can give another value, `g(s, (s = +"q"; 1))`: its
       slot, read when the callee runs, holds the new String, and CRuby
       binds the one read first. It binds the value the call read, in a temp
       like any other value, as a shared handle binds a fresh one. */
    /* a class body's ivar, read through its synthesized getter
       (`C.__spinel_civget_x`, desugar_body_ivars): the class's civ_ C
       global, lent as a class method's ivar is, also when the getter ran
       first (it only reads the slot). Lent a temp, the callee's appends
       stayed in the copy. */
    if (provided >= 0 && nt_kind(c->nt, provided) == NK_CallNode && comp_ntype(c, provided) == TY_STRING &&
        nt_str(c->nt, provided, "name") && !strncmp(nt_str(c->nt, provided, "name"), "__spinel_civget_", 16)) {
      int cr = nt_ref(c->nt, provided, "receiver");
      const char *crn = cr >= 0 && nt_kind(c->nt, cr) == NK_ConstantReadNode ? nt_str(c->nt, cr, "name") : NULL;
      int ccid = crn ? comp_class_index(c, crn) : -1;
      char ivn[260]; snprintf(ivn, sizeof ivn, "@%s", nt_str(c->nt, provided, "name") + 16);
      int civ = ccid >= 0 ? comp_ivar_index(&c->classes[ccid], ivn) : -1;
      if (civ >= 0 && c->classes[ccid].ivar_types[civ] == TY_STRING) {
        buf_printf(out, "&civ_%s_%s", c->classes[ccid].name, iv_c(ivn + 1));
        return;
      }
    }
    if (provided >= 0 && !arg_ran_first(provided, 0)) {
      const char *aty = nt_type(c->nt, provided);
      char gref[256];
      if (aty && sp_streq(aty, "LocalVariableReadNode")) {
        const char *vn = nt_str(c->nt, provided, "name");
        if (emit_lent_local(vn ? scope_local(comp_scope_of(c, provided), vn) : NULL, vn, out))
          return;
      }
      /* an IVAR argument: pass the slot itself so the callee's append lands in
         the object. The owner is pinned rather than marked dirty, because the
         store happens inside the callee and a dirty bit set before the call
         is cleared by any collection the call makes (#4378). */
      if (aty && sp_streq(aty, "InstanceVariableReadNode")) {
        const char *ivn = nt_str(c->nt, provided, "name");
        Scope *ivs = comp_scope_of(c, provided);
        if (ivn && ivs && ivs->class_id >= 0 && !ivs->is_cmethod &&
            comp_ntype(c, provided) == TY_STRING) {
          /* a value-type receiver is a struct, not a heap object: it has no
             header to mark dirty, and casting it to void* does not compile */
          if (!comp_ty_value_obj(c, ty_object(ivs->class_id))) {
            /* Sticky, not dirty. This runs before the call and the store is
               inside it, so a dirty bit set here is cleared by any collection
               the call makes and the store that follows is unrecorded (the
               rule #4378 established). A pinned owner is scanned on every
               minor cycle for as long as it lives, which makes the placement
               stop mattering -- and it is the only thing that can, since the
               callee holds a slot address and cannot name what owns it. */
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_gc_pin_remembered((void *)%s);\n", g_self);
          }
          buf_printf(out, "&%s%siv_%s", g_self, g_self_deref, iv_c(ivn + 1));
          return;
        }
        /* A top-level ivar, or a class method's, lives in a C global (the
           read emitter's civ_ slot), a root every collection marks: its
           address is lent as an instance's slot is, with no owner to pin.
           Lent a temp, the callee's appends stayed in the copy. */
        if (comp_ntype(c, provided) == TY_STRING && ivar_global_slot(c, provided, gref, sizeof gref)) {
          refuse_lent_global_rebound(c, provided, gref, m->name, m->pnames[idx]);
          buf_printf(out, "&%s", gref);
          return;
        }
      }
      /* a global variable's C global likewise, when the read is the plain
         gv_ slot (not a special global's runtime accessor) */
      if (aty && sp_streq(aty, "GlobalVariableReadNode") && comp_ntype(c, provided) == TY_STRING &&
          gvar_global_slot(c, provided, gref, sizeof gref)) {
        refuse_lent_global_rebound(c, provided, gref, m->name, m->pnames[idx]);
        buf_printf(out, "&%s", gref);
        return;
      }
      /* a class variable's C global likewise: lent a temp, the callee's
         appends stayed in the copy */
      if (aty && sp_streq(aty, "ClassVariableReadNode") && comp_ntype(c, provided) == TY_STRING &&
          cvar_global_slot(c, provided, gref, sizeof gref)) {
        refuse_lent_global_rebound(c, provided, gref, m->name, m->pnames[idx]);
        buf_printf(out, "&%s", gref);
        return;
      }
    }
    Buf ab; memset(&ab, 0, sizeof ab);
    p->byref_out = 0;   /* reenter for the plain coerced value */
    emit_arg_or_default(c, m, idx, provided, &ab);
    p->byref_out = 1;
    emit_lent_temp(ab.p, out);
    free(ab.p);
    return;
  }
  /* An unused/unresolved param is declared poly (sp_RbVal) in the method
     signature (codegen.c maps TY_UNKNOWN params to TY_POLY); box the argument
     to match, so a virtually-dispatched call into such a slot passes a valid
     sp_RbVal rather than a raw value (or `void` temp). */
  if (p && pt == TY_UNKNOWN) pt = TY_POLY;
  if (provided >= 0) {
    /* A typed array into a boxed parameter the method stores an element
       into that the array cannot hold: the store promotes a copy, and the
       caller's array never sees it. The binding widens the arrays it can
       follow back to where they are built (widen_arg_array); a literal or a
       new array is storage nobody else holds, and the rest are refused
       rather than miscompiled. */
    if (pt == TY_POLY && c->store_misfit_arg && provided < c->node_cap && c->store_misfit_arg[provided] &&
        !is_fresh_array(c, provided)) {
      TyKind at = comp_ntype(c, provided);
      if (at == TY_INT_ARRAY || at == TY_STR_ARRAY || at == TY_FLOAT_ARRAY) {
        const char *ek = at == TY_INT_ARRAY ? "Integer" : at == TY_STR_ARRAY ? "String" : "Float";
        char msg[512];
        snprintf(msg, sizeof msg,
                 "an Array[%s] is passed to `%s`'s parameter `%s`, which the method stores elements of "
                 "other kinds into: the caller's array cannot hold them, and the store would go to a "
                 "copy it never sees. Build the argument where the call can follow it (an array literal "
                 "or a new array, through locals and method values), so it is widened with the parameter.",
                 ek, m->name ? m->name : "?", m->pnames[idx]);
        unsupported_feature(c, provided, msg);
      }
    }
    if (pt == TY_POLY) emit_boxed(c, provided, out);   /* box into a poly param */
    else {
      TyKind at = comp_ntype(c, provided);
      /* An int argument reaching a bigint parameter is promoted at the
         boundary. That is the whole premise of ty_unify keeping int-and-bigint
         at bigint rather than widening to poly -- but the promotion was only
         wired into the arithmetic operands, so `def f(x) = x.to_s` called with
         both 1 and 2**200 typed its parameter sp_Bigint* and then passed it a
         raw sp_int. */
      if (pt == TY_BIGINT && at != TY_BIGINT && at != TY_POLY && ty_is_numeric(at)) {
        buf_puts(out, "sp_bigint_new_int("); emit_int_expr(c, provided, out); buf_puts(out, ")");
        return;
      }
      if (pt == TY_BIGINT && at == TY_POLY) {
        buf_puts(out, "sp_poly_as_bigint("); emit_expr(c, provided, out); buf_puts(out, ")");
        return;
      }
      /* A parameter a --rbs seed pins to a typed array (`Array[String]`)
         keeps its kind where inference would have widened it, so a poly
         array reaches it as it is: the elements are converted at the
         boundary, the way a seeded store converts them (#1827). Passed
         unconverted, the sp_PolyArray * was bound as the typed array and
         the C did not build (#5147). */
      if ((pt == TY_INT_ARRAY || pt == TY_FLOAT_ARRAY || pt == TY_STR_ARRAY) &&
          (at == TY_POLY_ARRAY || at == TY_POLY)) {
        Buf ab; memset(&ab, 0, sizeof ab);
        Buf bx; memset(&bx, 0, sizeof bx);
        emit_expr(c, provided, &ab);
        if (at == TY_POLY_ARRAY) emit_boxed_text(c, TY_POLY_ARRAY, ab.p ? ab.p : "NULL", &bx);
        else buf_puts(&bx, ab.p ? ab.p : "sp_box_nil()");
        emit_unbox_text(c, pt, bx.p ? bx.p : "sp_box_nil()", out);
        free(ab.p); free(bx.p);
        return;
      }
      /* Bare call inside a class/module body: analyze may not have resolved the
         type because g_cbody_class_id is not set during fixpoint. Look it up now. */
      if (at == TY_UNKNOWN && g_class_body_id >= 0) {
        const char *ptn = nt_type(c->nt, provided);
        if (ptn && sp_streq(ptn, "CallNode") && nt_ref(c->nt, provided, "receiver") < 0) {
          const char *bn = nt_str(c->nt, provided, "name");
          int bsmi = bn ? comp_cmethod_in_chain(c, g_class_body_id, bn, NULL) : -1;
          if (bsmi >= 0) at = (TyKind)c->scopes[bsmi].ret;
        }
      }
      /* empty array literal `[]` defaults to IntArray in emit_expr; if the
         parameter expects a different array type, emit the right constructor */
      int nen = 0;
      const char *pty_node = nt_type(c->nt, provided);
      int is_empty_arr = pty_node && sp_streq(pty_node, "ArrayNode") &&
                         (nt_arr(c->nt, provided, "elements", &nen), nen == 0);
      if (is_empty_arr && ty_is_array(pt) && pt != TY_INT_ARRAY) {
        if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
        else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, provided, out); }
      }
      /* empty hash literal `{}` with unknown type: emit the param's hash constructor.
         Must be checked before the poly-unbox path below, as at==TY_UNKNOWN for {}.  */
      else {
        int phn = 0;
        int is_empty_hash = pty_node && (sp_streq(pty_node, "HashNode") || sp_streq(pty_node, "KeywordHashNode")) &&
                             (nt_arr(c->nt, provided, "elements", &phn), phn == 0);
        if (is_empty_hash && at == TY_UNKNOWN && ty_is_hash(pt)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) { buf_printf(out, "sp_%sHash_new()", hn); return; }
        }
        /* A typed hash into a parameter the method stores foreign keys or
           values into (push_widened): the binding widens the caller's hash
           where it can see how it is built, and a copy would drop the
           stores, so the rest are refused rather than miscompiled. */
        if (pt == TY_POLY_POLY_HASH && ty_is_hash(at) && at != TY_POLY_POLY_HASH &&
            m && idx >= 0 && idx < m->nparams && m->pnames[idx]) {
          LocalVar *hp = scope_local(m, m->pnames[idx]);
          if (hp && hp->push_widened) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "a typed Hash is passed to `%s`'s parameter `%s`, which the method stores keys or "
                     "values of other types into: the caller's hash cannot hold them, and a converted copy "
                     "would not see the stores. Build the argument from a hash literal the call can see, "
                     "so it is widened with the parameter.",
                     m->name ? m->name : "?", m->pnames[idx]);
            unsupported_feature(c, provided, msg);
          }
        }
        /* A concrete str-keyed hash arg (StrStrHash / StrIntHash) into a
           poly-valued hash param (StrPolyHash): the two structs store values
           differently (const char* / sp_int vs sp_RbVal), so a raw pointer
           pass reinterprets the layout and corrupts every read. Rebuild via the
           value-boxing converter, mirroring the local-assignment coercion in
           codegen_stmt.c. */
        if (pt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(out, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, provided, out); buf_puts(out, ")");
          return;
        }
        /* When the param is a typed hash pointer but the caller passes a poly
           or nil value (e.g. an uninit ivar), extract .v.p from the RbVal.
           sp_box_nil() stores v.i=0 so .v.p is NULL, which hash getters handle
           safely via their NULL guards. */
        if (ty_is_hash(pt) && (at == TY_POLY || at == TY_NIL || at == TY_UNKNOWN)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) {
            /* The argument is rendered before its temp's declaration is
               written: what it hoists goes to the prelude too, and written
               after `sp_RbVal _tN = ` it took the initializer's place. A call
               whose own argument hoists a rooted box (`upd(perm(input))`)
               left the temp holding that box, the call ran as a statement
               of its own, and its result was dropped. */
            int ht = ++g_tmp;
            Buf ab2; memset(&ab2, 0, sizeof ab2);
            emit_expr(c, provided, &ab2);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", ht, ab2.p ? ab2.p : "sp_box_nil()");
            free(ab2.p);
            /* A poly-VALUED variant takes the converting entry rather than a
               pointer cast: the variants are separate C structs, so a boxed
               hash of another one read through the cast kept its keys and read
               every value as another type's zero -- silently (#3998). The
               entry hands back the pointer itself when the variant already
               matches, so an already-right hash keeps its identity. */
            const char *conv2 = pt == TY_STR_POLY_HASH  ? "sp_poly_as_str_poly_hash"
                              : pt == TY_SYM_POLY_HASH  ? "sp_poly_as_sym_poly_hash"
                              : pt == TY_POLY_POLY_HASH ? "sp_poly_as_poly_poly_hash" : NULL;
            if (conv2) buf_printf(out, "%s(_t%d)", conv2, ht);
            else       buf_printf(out, "(sp_%sHash *)_t%d.v.p", hn, ht);
            return;
          }
        }
        /* A concrete typed array arg (IntArray/StrArray/FloatArray) into a
           poly-array param: the structs store elements differently (raw
           sp_int / const char ptr / sp_float vs boxed sp_RbVal), a raw pointer
           pass reinterprets the layout and every read is garbage -- pop
           returned a boxed nil built from a char* (#3137). Rebuild through
           the boxing converter, mirroring the local-assignment coercion. */
        if (pt == TY_POLY_ARRAY &&
            (at == TY_INT_ARRAY || at == TY_STR_ARRAY || at == TY_FLOAT_ARRAY)) {
          /* The rebuild is a COPY, which is right for a value and wrong for
             storage the caller still holds: a callee that appends to the
             parameter appended to the copy and the caller's array never
             changed, silently (#4480). The binding widens the caller's
             array instead wherever it can follow it back to where it is
             built (widen_arg_array): a literal, a new array, a local, a
             method's value, an ivar that only ever holds new arrays. What
             it cannot follow is refused here rather than miscompiled. An
             array literal, or a new array a builtin answers, is storage
             nobody else holds, so its copy is the only one there is. A
             parameter the binding widened (push_widened) is mutated all the
             same, perhaps after the method hands it back as its value. */
          int direct = 0;
          if (m && idx >= 0 && idx < m->nparams && m->pnames[idx] &&
              nt_kind(c->nt, provided) != NK_ArrayNode && !is_fresh_array(c, provided) &&
              ((direct = scope_mutates_array_local(c, (int)(m - c->scopes), m->pnames[idx], 0)) ||
               (p && p->push_widened))) {
            char msg[512];
            snprintf(msg, sizeof msg,
                     "an Array[%s] is passed to `%s`'s parameter `%s`, which the method %s: the "
                     "parameter is a general Array and the argument would be copied into it, so the "
                     "mutation would not reach the caller's array. Give the parameter the argument's kind "
                     "(an rbs seed, or callers that all pass Array[%s]), or build the argument as a general Array.",
                     at == TY_INT_ARRAY ? "Integer" : at == TY_STR_ARRAY ? "String" : "Float",
                     m->name ? m->name : "?", m->pnames[idx],
                     direct ? "mutates" : "hands on to where it is mutated",
                     at == TY_INT_ARRAY ? "Integer" : at == TY_STR_ARRAY ? "String" : "Float");
            unsupported_feature(c, provided, msg);
          }
          const char *cv = at == TY_INT_ARRAY ? "sp_PolyArray_from_int_array"
                         : at == TY_STR_ARRAY ? "sp_PolyArray_from_str_array"
                         : "sp_PolyArray_from_float_array";
          buf_printf(out, "%s(", cv); emit_expr(c, provided, out); buf_puts(out, ")");
          return;
        }
        /* poly arg into a concrete param (holds the right type at runtime):
           coerce, else the generated C assigns sp_RbVal to a const char* /
           sp_int / sp_float / sp_<Class>* slot. */
        const char *ptn = c_type_name(pt);
        /* a literal/derived nil argument into a nullable scalar param carries
           the type's nil sentinel, exactly like a nil DEFAULT does below --
           a plain emit rendered nil as 0 and the callee saw an integer (#2438) */
        if (at == TY_NIL && pt == TY_INT) { buf_puts(out, "((void)("); emit_expr(c, provided, out); buf_puts(out, "), SP_INT_NIL)"); }
        else if (at == TY_NIL && pt == TY_FLOAT) { buf_puts(out, "((void)("); emit_expr(c, provided, out); buf_puts(out, "), sp_float_nil())"); }
        else if (at == TY_NIL && pt == TY_STRING) { buf_puts(out, "((void)("); emit_expr(c, provided, out); buf_puts(out, "), NULL)"); }
        else if (at == TY_POLY && pt == TY_STRING) { buf_puts(out, "sp_poly_to_s_or_nil("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        /* An unresolved call typed TY_UNKNOWN whose emitted value is the gate's
           sp_raise_nomethod(...) poly token, landing in a concretely-typed
           param slot (`raw(rec.created_at.strftime(...))` on a nilable
           receiver): coerce the token to the slot type, keeping the raise,
           instead of passing the sp_RbVal through raw. */
        else if (at == TY_UNKNOWN && pt != TY_POLY && pt != TY_UNKNOWN) {
          emit_unresolved_coerced(c, provided, pt, out);
        }
        else if (at == TY_POLY && pt == TY_FLOAT) { buf_puts(out, "sp_poly_to_f_or_nil("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && pt == TY_SYMBOL) { buf_puts(out, "(sp_sym)sp_poly_to_i("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        /* the Range value types are boxed behind a pointer: dereference rather
           than assigning the box to the struct slot (#3619) */
        else if (at == TY_POLY && (pt == TY_RANGE || pt == TY_FLOAT_RANGE || pt == TY_STR_RANGE)) {
          Buf rbx; memset(&rbx, 0, sizeof rbx);
          emit_expr(c, provided, &rbx);
          emit_unbox_text(c, pt, rbx.p ? rbx.p : "sp_box_nil()", out);
          free(rbx.p);
        }
        /* A poly argument narrowing into a declared int/float/String parameter keeps
           nil distinguishable: the plain conversions answer the type's zero, which
           in those slots is a real value. A LITERAL nil already lands on the
           sentinel just above; this is the case where nil-ness is only known at
           run time -- an element of a mixed array, a Hash miss, an untyped call
           (#3465). bool keeps the plain conversion: it has no sentinel. */
        else if (at == TY_POLY && pt == TY_INT) { buf_puts(out, "sp_poly_to_i_or_nil("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && pt == TY_BOOL) { buf_puts(out, "sp_poly_to_i("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        /* poly arg into an object or other pointer-backed param (array, proc,
           ...): unbox the pointer via emit_unbox_text (a nil box has v.p ==
           NULL, the pointer-nil representation). The callee's RBS asserts the
           type, mirroring the seed-trusting coercion on the return side. (Typed-
           value hashes are handled by the ty_is_hash block above; by-value types
           have no .v.p form and fall through to a plain emit.) */
        else if (at == TY_POLY && (ty_is_object(pt) || (ptn && ptn[0] && ptn[strlen(ptn) - 1] == '*'))) {
          Buf ub; memset(&ub, 0, sizeof ub);
          emit_expr(c, provided, &ub);
          Buf uck; memset(&uck, 0, sizeof uck);
          /* a seeded parameter is asserted where the dynamic value becomes a
             static type -- the same place an ivar seed is (#3412) */
          if (p && p->rbs_seeded)
            emit_rbs_checked_text(c, pt, m->pnames[idx], ub.p ? ub.p : "sp_box_nil()", &uck);
          else buf_puts(&uck, ub.p ? ub.p : "sp_box_nil()");
          emit_unbox_text(c, pt, uck.p ? uck.p : "", out);
          free(uck.p);
          free(ub.p);
        }
        /* A string param whose argument is context-typed TY_STRING but whose
           value is really the unresolved-call gate's sp_raise_nomethod(...)
           token (`html_escape(obj.details)` on an unknown receiver): emit_str_expr
           passes a real string through and coerces the token to the slot. */
        else if (pt == TY_STRING) emit_str_expr(c, provided, out);
        else {
          emit_obj_upcast_prefix(c, pt, at, out);
          emit_coerce(c, provided, pt, CO_HOLD, "a method argument", out);
        }
      }
    }
    return;
  }
  int dv = m->pdefault[idx];
  const char *dty = dv >= 0 ? nt_type(c->nt, dv) : NULL;
  /* A default expression evaluates in the CALLEE's context: a `self` inside
     it (e.g. `def self.f(rel = Wrap.new(self))`) is the callee's class, not
     whatever `self` the caller happens to have (#2443). Only the class-method
     case is representable at the call site (the Class object is a constant);
     an instance-method default referencing self keeps the caller's g_self,
     correct for the common same-class implicit-self call. */
  const char *sv_self_dv = g_self, *sv_deref_dv = g_self_deref;
  int sv_emcls_dv = g_emitting_class_id;
  char dv_self9[32];
  /* an inlined class method's default already runs on the class it was
     called on, which may inherit it (emit_inlined_default) */
  if (dv >= 0 && m->class_id >= 0 && m->is_cmethod && g_inl_dflt_on_recv != m) {
    snprintf(dv_self9, sizeof dv_self9, "((sp_Class){%d})", m->class_id);
    g_self = dv_self9;
  }
  /* A constructor's default runs on the object being built, which exists by
     the time this is emitted (the ctor allocates first, then initializes).
     Its receiverless calls are that class's methods too: emitted with the
     caller's class, `N.new` from another class's method could not find the
     helper a recursive default calls, and refused the call or raised
     NoMethodError at run time. */
  else if (dv >= 0 && g_ctor_self && m->name && sp_streq(m->name, "initialize")) {
    g_self = g_ctor_self;
    g_self_deref = g_ctor_self_deref ? g_ctor_self_deref : "->";
    g_emitting_class_id = m->class_id;
  }
  if (dv < 0) {
    /* A missing required arg pads the slot with a zero-ish compat value so
       codegen completes (a compile-time warning already flagged the call).
       A poly-widened slot must mirror the scalar `0` an int slot emits, not
       sp_box_nil() -- otherwise the padded value renders as blank. */
    if (pt == TY_POLY) buf_puts(out, "sp_box_int(0)");
    else buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
else if (dty && sp_streq(dty, "NilNode")) {
    /* nil default: emit the nil sentinel for the type */
    if (pt == TY_INT)    buf_puts(out, "SP_INT_NIL");
    else if (pt == TY_FLOAT) buf_puts(out, "sp_float_nil()");
    else if (pt == TY_STRING) buf_puts(out, "NULL");
    else buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
  /* A default that cannot complete (`x: (raise "...")`) runs for its effect
     and never reaches the slot; the comma gives the slot's C type a value */
  else if (pt != TY_POLY && comp_ntype(c, dv) == TY_VOID) {
    buf_puts(out, "(");
    emit_expr(c, dv, out);
    buf_printf(out, ", %s)", pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
  else if (pt == TY_POLY) emit_boxed(c, dv, out);
  /* A default expression typed poly landing in a concrete parameter slot: it
     was typed in a scope whose class differs from the emitted receiver (a
     module method transplanted into an including class), so its ivar-typed
     arithmetic comes out boxed. Coerce it exactly as a supplied poly argument
     would, rather than assigning the sp_RbVal to the slot's C type. */
  else if (comp_ntype(c, dv) == TY_POLY) {
    Buf db; memset(&db, 0, sizeof db);
    /* emit_boxed, not emit_expr: a bare `@x` default typed poly in the
       module's scope is emitted by emit_expr as the raw concrete field, so
       wrapping it in the poly converters was invalid C (sp_int to an sp_RbVal
       parameter). emit_boxed boxes that same field first. A genuine poly
       expression passes through unchanged. */
    emit_boxed(c, dv, &db);
    const char *dx = db.p ? db.p : "sp_box_nil()";
    switch (pt) {
    case TY_INT: buf_printf(out, "sp_poly_to_i_or_nil(%s)", dx); break;
    case TY_FLOAT: buf_printf(out, "sp_poly_to_f_or_nil(%s)", dx); break;
    case TY_STRING: buf_printf(out, "sp_poly_to_s_or_nil(%s)", dx); break;
    case TY_SYMBOL: buf_printf(out, "(sp_sym)sp_poly_to_i(%s)", dx); break;
    case TY_BOOL: buf_printf(out, "sp_poly_to_i(%s)", dx); break;
    case TY_BIGINT: buf_printf(out, "sp_poly_as_bigint(%s)", dx); break;
    default: emit_unbox_text(c, pt, dx, out); break;
    }
    free(db.p);
  }
  /* Same boundary promotion the supplied-argument path does: an int DEFAULT
     (`def f(x = 7)`) reaching a bigint parameter is an sp_int in a
     sp_Bigint* slot without it. */
  else if (pt == TY_BIGINT && comp_ntype(c, dv) != TY_BIGINT &&
           ty_is_numeric(comp_ntype(c, dv))) {
    buf_puts(out, "sp_bigint_new_int("); emit_int_expr(c, dv, out); buf_puts(out, ")");
  }
  else {
    /* Default empty `[]` literal: emit the correct array constructor for
       the parameter type rather than always sp_IntArray_new(). */
    int den = 0;
    int is_empty_arr_dv = dty && sp_streq(dty, "ArrayNode") &&
                          (nt_arr(c->nt, dv, "elements", &den), den == 0);
    if (is_empty_arr_dv && ty_is_array(pt) && pt != TY_INT_ARRAY) {
      if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
      else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, dv, out); }
    }
    /* Default empty `{}` literal: emit the correct hash constructor for the
       parameter type, avoiding the "unsupported hash literal" fallback. */
    else {
      int dhn = 0;
      int is_empty_hash_dv = dty && (sp_streq(dty, "HashNode") || sp_streq(dty, "KeywordHashNode")) &&
                              (nt_arr(c->nt, dv, "elements", &dhn), dhn == 0);
      if (is_empty_hash_dv && ty_is_hash(pt)) {
        const char *hn = ty_hash_cname(pt);
        if (hn) buf_printf(out, "sp_%sHash_new()", hn);
        else emit_expr(c, dv, out);
      }
      else emit_coerce(c, dv, pt, CO_HOLD, "a parameter default", out);
    }
  }
  g_self = sv_self_dv; g_self_deref = sv_deref_dv;
  g_emitting_class_id = sv_emcls_dv;
}

/* Emit a comma-separated argument list filling defaults for omitted
   optional params. `lead` is prepended before the first arg. */
/* Find the value node for keyword param named `kname` in a KeywordHashNode
   `kwh`: the last one when the key is written twice, as CRuby binds it. */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname) {
  if (kwh < 0 || !kname) return -1;
  int en = 0, v = -1;
  const int *elems = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, elems[e], "key");
    if (key < 0) continue;
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && sp_streq(kn, kname)) v = nt_ref(nt, elems[e], "value");
  }
  return v;
}

/* Emit a PolyArray expression that collects call args[from..pos_argc-1].
   SplatNode arguments are expanded element-by-element into the array. */
/* An anonymous `*` at a forwarding call site (`f(a, *)`) is a SplatNode with no
   expression; it forwards the enclosing method's anonymous rest local. Returns
   that local's poly-array C expression into `buf` and 1, or 0 if `splat` is not
   an anonymous forward. */
int emit_anon_rest_ref(Compiler *c, int splat, Buf *buf) {
  if (nt_ref(c->nt, splat, "expression") >= 0) return 0;
  Scope *sc = comp_scope_of(c, splat);
  if (!sc || sc->rest_idx < 0 || sc->rest_idx >= sc->nparams) return 0;
  buf_printf(buf, "lv_%s", rename_local(sc->pnames[sc->rest_idx]));
  return 1;
}

/* An anonymous `**` at a forwarding call site (`f(**)`) is an AssocSplatNode with
   no value; it forwards the enclosing method's anonymous kwrest local. Returns
   that local's name, or NULL if `node` is not inside a scope with an anon kwrest. */
static const char *anon_kwrest_name(Compiler *c, int node) {
  Scope *sc = comp_scope_of(c, node);
  if (!sc || sc->kwrest_idx < 0 || sc->kwrest_idx >= sc->nparams || !sc->pnames) return NULL;
  const char *nm = sc->pnames[sc->kwrest_idx];
  return (nm && sp_streq(nm, "__anon_kwrest")) ? nm : NULL;
}

/* The hash type of the anonymous kwrest such a `**` forwards: Symbol-keyed,
   or of any key where a call brings the enclosing method one (kwrest_any_key). */
static TyKind anon_kwrest_type(Compiler *c, int node) {
  return kwrest_any_key(c, comp_scope_of(c, node)) ? TY_POLY_POLY_HASH : TY_SYM_POLY_HASH;
}

static int kwh_consumed_by_kwparam(Compiler *c, Scope *m, int kwh);

/* Set while inference asks for a layout (arg_layout_untyped): it types the
   parameters from it, so their types are not settled and are not asked. */
static int layout_untyped;

/* The parameter index a collapsed keyword hash fills, or -1 when none does.
   Ruby passes a braceless `f(k: 1)` as one more positional argument when the
   callee declares no keyword parameter that takes a key -- `def f(opts)` and
   `def f(opts = {})` alike -- so it lands where that argument binds: a
   required parameter after the optionals before any optional (#4877). The slot has to be able to hold
   a hash, so a concretely-typed one (an int param bound elsewhere) declines
   and the rest takes it instead. Inference, which gives the slot its type,
   asks without it (arg_layout_untyped).

   The poly dispatch asked none of this: its arms matched keywords by name
   only, so an optional positional silently kept its default and a required one
   made the arm look like an arity mismatch, dropping it from the switch
   entirely (#4030). */
int kwh_positional_slot(Compiler *c, Scope *m, int kwh, int pos_argc) {
  if (kwh < 0 || !m || m->kwrest_idx >= 0) return -1;
  if (kwh_consumed_by_kwparam(c, m, kwh)) return -1;
  int slot = kwh_arg_param(c, m, pos_argc);
  if (slot < 0 || slot == m->rest_idx) return -1;
  const char *pn = m->pnames ? m->pnames[slot] : NULL;
  if (!pn || callee_param_is_declared_kwarg(c, m, pn)) return -1;
  LocalVar *p = scope_local(m, pn);
  TyKind pt = p ? p->type : TY_UNKNOWN;
  if (!layout_untyped && !ty_is_hash(pt) && pt != TY_POLY) return -1;
  return slot;
}

/* The parameter a keyword hash binds as one more positional argument after
   `pos_argc` others, or -1. It is the last of pos_argc + 1 arguments, bound the
   way arg_slot_for_param binds any argument: the required parameters are
   funded first, so with a leading optional `def h(a = {}, c); h(k: 9)` binds
   `c` and leaves `a` at its default. Answering parameter pos_argc gave `a` the
   hash and `c` nothing. Blind to parameter types: kwh_positional_slot adds
   whether the parameter can hold the hash, and inference asks here to type it.
   Past a *rest the last argument is the last post's however many came before
   it: `def f(*r, z); f(1, k: 9)` binds z the hash and r `[1]`, where asking
   arg_slot_for_param -- which leaves the posts to its callers -- gave the hash
   to the rest and z the 1. */
int kwh_arg_param(Compiler *c, Scope *m, int pos_argc) {
  if (m && m->rest_idx >= 0 && m->npost_rest > 0 && pos_argc >= 0)
    return m->rest_idx + m->npost_rest;
  if (!m || pos_argc < 0 || pos_argc >= m->nparams) return -1;
  for (int i = 0; i < m->nparams; i++)
    if (arg_slot_for_param(c, m, i, pos_argc + 1) == pos_argc) return i;
  return -1;
}

/* Ruby packs a keyword hash no parameter consumed into the *rest as one
   positional hash. Returns the hash node to append, or -1: -1 when there is no
   keyword hash, when a **kwrest will take it, or when some declared keyword
   parameter binds one of its keys.

   One function because the rule had to be written three times before it was
   right in all of them -- #3503 was it missing from the dispatch path after
   the direct one had it, and #3528 was it missing from the poly-dispatch arm
   after both. A call path that packs a rest asks here rather than
   reimplementing the test. */
int rest_kwh_tail(Compiler *c, Scope *m, int kwh, int pos_argc) {
  /* A callee taking keywords takes the hash as keywords whatever its keys,
     a `**h` (named or anonymous) included, which has none to find by name;
     a `**nil` one binds it nowhere. */
  if (kwh < 0 || !m || kwh_consumed_by_kwparam(c, m, kwh)) return -1;
  /* a `...` forwarder binds a key by name to the parameter synthesized for it */
  for (int i = 0; i < m->nparams; i++)
    if (m->pnames[i] && callee_has_kwarg(c, m, m->pnames[i]) &&
        kwh_lookup(c->nt, kwh, m->pnames[i]) >= 0) return -1;
  /* Ruby funds POSITIONAL parameters before the rest, so a hash that collapses
     into an unfilled positional slot never reaches the rest as well. Both took
     it: `def f(condition = nil, *args); f(k: 1)` gave args `[{k: 1}]` where
     Ruby leaves it empty (found while fixing #4030). */
  if (kwh_positional_slot(c, m, kwh, pos_argc) >= 0) return -1;
  return kwh;
}

/* The arguments a *rest and its posts are laid out over: the positionals,
   and the keyword hash too when it binds as the last post, which in argv it
   follows at argv[pos_argc]. The rest stops npost_rest short of this and
   each post counts back from it. */
int rest_bind_argc(Compiler *c, Scope *m, int kwh, int pos_argc) {
  int slot = kwh_positional_slot(c, m, kwh, pos_argc);
  return pos_argc + (m && m->rest_idx >= 0 && slot > m->rest_idx);
}

/* A keyword hash of `**` spreads alone that binds positionally is one
   argument when the run time finds it non-empty and none when it is empty:
   `f(1, **h)` is `f(1)` for an empty h, so `def f(a, b = nil)` keeps b's
   default, `def g(a, b)` is short an argument and `def p(*r, z)` gives z
   the 1. Its parameters bind by a count only the run time knows, from all
   the arguments gathered (emit_splat_gather), for a parameter list with
   none synthesized. `def hh(a = 5, c)` and `def pd(a, b = a)` bind from it
   too: `hh(1, **h)` is `[5, 1]` and `pd(1, **h)` `[1, 1]` for an empty h,
   where they kept the old binding and took the `{}`. So does one no
   parameter is left for, which the gather's count refuses when it holds a
   key (`g(1, 2, **h)` on `def g(a, b)`), and one after a splat, whose count
   is the run time's too (`f(*[], 1, **h)` on `def f(a = nil, b)` binds b
   the hash). The rest alone takes such a hash at its tail (rest_kwh_tail),
   which tests the length itself, unless a splat spreads ahead of it. */
int kwh_gathers(Compiler *c, Scope *m, int kwh, const int *argv, int pos_argc) {
  if (!m || !kwh_only_spreads(c->nt, kwh) || m->cs_synth) return 0;
  if (kwh_positional_slot(c, m, kwh, pos_argc) < 0) {
    int spl = 0;
    for (int k = 0; argv && k < pos_argc; k++)
      if (nt_kind(c->nt, argv[k]) == NK_SplatNode) spl = 1;
    if (kwh_consumed_by_kwparam(c, m, kwh) ||
        !(spl || (m->rest_idx < 0 && kwh_arg_param(c, m, pos_argc) < 0))) return 0;
  }
  for (int i = 0; i < m->nparams; i++)
    if (i != m->rest_idx && i != m->kwrest_idx &&
        (!m->pnames[i] || (m->pnames[i][0] == '_' && m->pnames[i][1] == '_'))) return 0;
  return 1;
}

/* Positional arity excludes declared keyword parameters, which are stored in
   the same pnames[] array as positional parameters. This matters when an
   anonymous `*` from a forwarding wrapper is expanded into a fixed callee:
   `def f(x, k:)` has one positional slot, not two. */
void positional_arity(Compiler *c, Scope *m, int *required, int *total) {
  int pn = m ? nt_ref(c->nt, m->def_node, "parameters") : -1;
  if (pn >= 0) {
    int rn = 0, on = 0, postn = 0;
    nt_arr(c->nt, pn, "requireds", &rn);
    nt_arr(c->nt, pn, "optionals", &on);
    nt_arr(c->nt, pn, "posts", &postn);
    *required = rn + postn;
    *total = rn + on + postn;
    return;
  }
  *required = m ? m->nrequired : 0;
  *total = m ? m->nparams : 0;
}

/* Did a declared keyword parameter take one of this hash's keys? If so the
   hash was keywords, not a positional argument, and handing it to a poly or
   hash-typed positional binds it a second time (#3525). One function because
   the options-hash collapse is written in two call paths. */
static int kwh_consumed_by_kwparam(Compiler *c, Scope *m, int kwh) {
  if (kwh < 0 || !m) return 0;
  /* What the hash is, is the plan's (kw_plan): keywords for a callee that
     takes any -- a `**kwrest` takes every one, so `def f(a = nil, **kw);
     f(k: 1)` bound the hash to `a` as well (#3808), and a declared keyword
     takes a `**h` with no key to match by name, which bound `f(1, **h)`
     against `def f(a, b = 5, k: 1)` to b as well -- and nothing a `**nil`
     callee binds: holding a key the call is refused. */
  KwPlan P;
  kw_plan(c, m, kwh, &P);
  return P.role == KWH_KEYWORDS || P.role == KWH_REFUSED;
}

void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b) {
  emit_rest_pack_kwh(c, from, pos_argc, argv, -1, b);
}

/* Like emit_rest_pack, but appends `kwh` (an unconsumed keyword hash that
   degrades to one positional hash argument) as the trailing element. */
void emit_rest_pack_kwh(Compiler *c, int from, int pos_argc, const int *argv, int kwh, Buf *b) {
  const NodeTable *nt = c->nt;
  /* Optimize: single pure-splat → direct conversion */
  if (kwh < 0 && pos_argc == from + 1) {
    const char *aty = argv ? nt_type(nt, argv[from]) : NULL;
    if (aty && sp_streq(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[from], "expression");
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      if (at == TY_INT_ARRAY) {
        buf_puts(b, "sp_IntArray_to_poly("); emit_expr(c, inner, b); buf_puts(b, ")");
        return;
      }
      if (at == TY_STR_ARRAY) {
        buf_puts(b, "sp_StrArray_to_poly_fmt("); emit_expr(c, inner, b); buf_puts(b, ")");
        return;
      }
      if (at == TY_FLOAT_ARRAY) {
        buf_puts(b, "sp_typed_to_poly("); emit_expr(c, inner, b); buf_puts(b, ", SP_BUILTIN_FLT_ARRAY)");
        return;
      }
      if (at == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, inner, b); buf_puts(b, ")");
        return;
      }
    }
  }
  /* Empty rest */
  if (kwh < 0 && (!argv || pos_argc <= from)) {
    buf_puts(b, "sp_PolyArray_new()");
    return;
  }
  /* General case: build PolyArray as statement expression. The temp is
     DECLARED and rooted in the enclosing frame rather than inside the
     expression: a cleanup root inside a statement expression pops when that
     expression ends, which is before the call it is an argument to runs, and
     the callee allocates (sp_X_new's SP_POOL_NEW) before it stores the array
     anywhere the collector can see. Only the declaration moves, so the pushes
     still run in their original place in the enclosing expression. */
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = NULL;\n", t);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  buf_printf(b, "({ _t%d = sp_PolyArray_new();", t);
  for (int i = from; i < pos_argc; i++) {
    const char *aty = nt_type(nt, argv[i]);
    if (aty && sp_streq(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[i], "expression");
      Buf arr; memset(&arr, 0, sizeof arr);
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      /* render the operand only for the arms that read this rendering: a
         render writes the operand's setup into the statement prelude, so a
         discarded one still ran there, and a side-effecting operand ran
         twice */
      int reads_arr = at == TY_INT_ARRAY || at == TY_STR_ARRAY ||
                      at == TY_FLOAT_ARRAY || at == TY_POLY_ARRAY;
      if (inner >= 0 && reads_arr) emit_expr(c, inner, &arr);
      else if (inner < 0 && emit_anon_rest_ref(c, argv[i], &arr)) at = TY_POLY_ARRAY;  /* anonymous `*` */
      const char *ap = arr.p ? arr.p : "NULL";
      Buf snf; memset(&snf, 0, sizeof snf);
      if (at == TY_INT_ARRAY || at == TY_FLOAT_ARRAY) emit_may_nil_text(c, inner, at, "_sa", &snf);
      if (at == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; int _snf = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s(_snf, _sa->data[_sa->start+_si])); }",
                   ap, snf.p, t, typed_elem_box_fn(at));
      else if (at == TY_STR_ARRAY)
        buf_printf(b, " { sp_StrArray *_sa = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }", ap, t);
      else if (at == TY_FLOAT_ARRAY)
        buf_printf(b, " { sp_FloatArray *_sa = %s; int _snf = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, %s(_snf, _sa->data[_si])); }",
                   ap, snf.p, t, typed_elem_box_fn(at));
      else if (at == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap, t);
      /* a boxed operand is an array only at run time: the splat's lowering
         normalizes it, and its elements are the rest's, where the operand
         once went in whole as one element */
      else if (inner >= 0 && (at == TY_POLY || at == TY_UNKNOWN)) {
        Buf el; memset(&el, 0, sizeof el);
        emit_expr(c, argv[i], &el);
        buf_printf(b, " { sp_PolyArray *_sa = %s; SP_GC_ROOT(_sa); for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", el.p ? el.p : "NULL", t);
        free(el.p);
      }
      else if (at == TY_NIL && inner >= 0) {
        /* `*nil` spreads to nothing: CRuby's `r(*nil)` passes no argument,
           where it went in as one nil. The operand still runs. */
        Buf el; memset(&el, 0, sizeof el);
        emit_expr(c, inner, &el);
        buf_printf(b, " (void)(%s);", el.p ? el.p : "0");
        free(el.p);
      }
      else { /* scalar splat: single element */
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, inner, &el);
        /* ... or none, when the scalar is nil: a String's NULL, or the
           sentinel of an Integer or Float slot that can hold one. `*x` of
           such a nil went in as one nil element, where CRuby's `*nil` is
           empty (`r(*x)` gave [nil]) */
        int may_nil = at == TY_STRING ||
                      ((at == TY_INT || at == TY_FLOAT) && call_returns_nullable_int(c, inner));
        if (may_nil)
          buf_printf(b, " { sp_RbVal _sv = %s; if (!sp_poly_nil_p(_sv)) sp_PolyArray_push(_t%d, _sv); }",
                     el.p ? el.p : "sp_box_nil()", t);
        else buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      free(arr.p); free(snf.p);
    }
else {
      Buf el; memset(&el, 0, sizeof el);
      emit_boxed(c, argv[i], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  Buf kel; memset(&kel, 0, sizeof kel);
  if (kwh >= 0 && !kwh_only_spreads(nt, kwh)) {
    /* the degraded keyword hash: a literal kw list boxes as a hash */
    emit_boxed(c, kwh, &kel);
    buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, kel.p ? kel.p : "sp_box_nil()");
  }
  else if (kwh >= 0 && emit_kwh_spread_arg(c, kwh, &kel)) {
    /* `**` spreads alone: a new Hash of their keywords, as CRuby passes, and
       pushed only when it holds one. Its temp is declared and rooted in the
       enclosing frame, as the array's is: the push can grow the array, and the
       hash would be held by nothing but a C local while it collects. */
    int kt = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil();\n", kt);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", kt);
    buf_printf(b, " _t%d = %s; if (sp_poly_length(_t%d) > 0) sp_PolyArray_push(_t%d, _t%d);",
               kt, kel.p ? kel.p : "sp_box_nil()", kt, t, kt);
  }
  free(kel.p);
  buf_printf(b, " _t%d; })", t);
}

/* Emit the element at index `elem_idx` from a typed array temp `tmp`. */
void emit_array_elem_at(TyKind at, int tmp, int elem_idx, Buf *b) {
  if (at == TY_INT_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[_t%d->start+%d] : 0)", tmp, elem_idx, tmp, tmp, tmp, elem_idx);
  else if (at == TY_STR_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : NULL)", tmp, elem_idx, tmp, tmp, elem_idx);
  else if (at == TY_FLOAT_ARRAY)
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : 0.0)", tmp, elem_idx, tmp, tmp, elem_idx);
  else
    buf_printf(b, "(_t%d && %d < _t%d->len ? _t%d->data[%d] : sp_box_nil())", tmp, elem_idx, tmp, tmp, elem_idx);
}

/* The same element of a typed array temp the caller knows to hold it (a
   splat of a local splat_local_sure_lit vouches for): no length test. */
void emit_array_elem_sure(TyKind at, int tmp, int elem_idx, Buf *b) {
  if (at == TY_INT_ARRAY) buf_printf(b, "_t%d->data[_t%d->start+%d]", tmp, tmp, elem_idx);
  else buf_printf(b, "_t%d->data[%d]", tmp, elem_idx);
}

/* Emit a PolyArray containing elements from array temp `tmp` starting at `from_idx`,
   then the remaining positional args from argv[argv_from..pos_argc-1]. */
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx,
                                          Compiler *c, int argv_from, int pos_argc,
                                          const int *argv, Buf *b) {
  int t = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", t, t);
  /* elements from the splatted array starting at from_idx */
  switch (at) {
  case TY_INT_ARRAY:
    buf_printf(b, " if (_t%d) for (sp_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_t%d->data[_t%d->start+_si]));", tmp, from_idx, tmp, t, tmp, tmp); break;
  case TY_STR_ARRAY:
    buf_printf(b, " if (_t%d) for (sp_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp); break;
  case TY_FLOAT_ARRAY:
    buf_printf(b, " if (_t%d) for (sp_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp); break;
  case TY_POLY_ARRAY:
    buf_printf(b, " if (_t%d) for (sp_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, _t%d->data[_si]);", tmp, from_idx, tmp, t, tmp); break;
  default: break;
  }
  /* then suffix args after the splat */
  for (int j = argv_from; j < pos_argc; j++) {
    const char *jty = argv ? nt_type(c->nt, argv[j]) : NULL;
    if (jty && sp_streq(jty, "SplatNode")) {
      int inner2 = nt_ref(c->nt, argv[j], "expression");
      TyKind at2 = inner2 >= 0 ? comp_ntype(c, inner2) : TY_UNKNOWN;
      /* rendered only for the arms that read it: a render writes the
         operand's setup into the prelude, where a discarded one still runs */
      Buf arr2; memset(&arr2, 0, sizeof arr2);
      if (at2 == TY_INT_ARRAY || at2 == TY_POLY_ARRAY) emit_expr(c, inner2, &arr2);
      const char *ap2 = arr2.p ? arr2.p : "NULL";
      if (at2 == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }", ap2, t);
      else if (at2 == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap2, t);
      /* a boxed operand is an array only at run time, and nil spreads to
         nothing: both go through the splat's own lowering */
      else if (inner2 >= 0 && (at2 == TY_POLY || at2 == TY_UNKNOWN || at2 == TY_NIL)) {
        Buf el2; memset(&el2, 0, sizeof el2); emit_expr(c, argv[j], &el2);
        buf_printf(b, " { sp_PolyArray *_sa = %s; SP_GC_ROOT(_sa); for (sp_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", el2.p ? el2.p : "NULL", t);
        free(el2.p);
      }
      else { Buf el2; memset(&el2, 0, sizeof el2); emit_boxed(c, inner2, &el2); buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el2.p ? el2.p : "sp_box_nil()"); free(el2.p); }
      free(arr2.p);
    }
else {
      Buf el; memset(&el, 0, sizeof el); emit_boxed(c, argv[j], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
  buf_printf(b, " _t%d; })", t);
}

/* True when an argument of type `pt` built by node `provided` has to be hoisted
   into a rooted temp before the call runs. A scalar (int/float/...) holds no
   heap pointer and needs none. A bare read (local/ivar/const/self/nil/string
   literal) is already reachable from a root where it lives, so it needs none
   either -- and hoisting one into g_pre is WRONG when the call sits in a
   sequence-expression that assigns the read variable before the call: the g_pre
   line is flushed at the statement boundary, capturing the value ABOVE that
   in-sequence assignment (`a = {...}; foo(a)` as an operand passed a stale `a`).
   That matches the g_argov skip in emit_args_filled. A param default like `{}`
   (provided < 0) is a fresh allocation and does want the root -- #1445. */
int arg_wants_root(Compiler *c, TyKind pt, int provided) {
  if (pt != TY_POLY && !needs_root(pt)) return 0;
  if (provided < 0) return 1;
  const char *aty = nt_type(c->nt, provided);
  return !(aty && (sp_streq(aty, "LocalVariableReadNode") ||
                   sp_streq(aty, "InstanceVariableReadNode") ||
                   sp_streq(aty, "ConstantReadNode") ||
                   sp_streq(aty, "SelfNode") || sp_streq(aty, "NilNode") ||
                   sp_streq(aty, "StringNode")));
}

/* Evaluate the already-rendered argument text `expr` into a g_pre temp of type
   `pt` and root it, leaving `_tN` in `out`. The root lives in the caller's frame
   and so covers the whole call, which the callee's own entry root cannot: a
   sibling argument evaluated after this one can collect before the call is even
   entered, and C leaves the order between them unspecified. `provided` is the
   argument's node (or < 0 for a synthesised default), used only for the upcast
   a narrower object type needs to reach the parameter's. */
void emit_rooted_operand(Compiler *c, TyKind pt, int provided, const char *expr, Buf *out) {
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  emit_ctype(c, pt, g_pre);
  buf_printf(g_pre, " _t%d = ", t);
  if (provided >= 0) emit_obj_upcast_prefix(c, pt, comp_ntype(c, provided), g_pre);
  buf_printf(g_pre, "%s;\n", expr);
  emit_indent(g_pre, g_indent);
  if (pt == TY_POLY) buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t);
  else buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  buf_printf(out, "_t%d", t);
}

/* Like emit_arg_or_default, but hoists a pointer-backed / poly argument into a
   g_pre temp and roots it before the call. A fresh allocation passed straight
   into a callee that allocates before it roots the parameter -- the canonical
   case being the `{}` for `def initialize(attrs = {})` into sp_<C>_new, which
   SP_POOL_NEWs (can GC) before sp_<C>_initialize roots lv_attrs (#1445) -- would
   otherwise be collected mid-call (use-after-free / SIGSEGV@0x0). This is the
   #1052-deferred "fresh temp passed straight into a call" shape. emit_args_filled
   (the .new / super arg path) emitted args inline; normal method calls already
   hoist+root via emit_dispatch. Rooting in the caller's frame keeps the value
   alive across the whole call. A scalar (int/float/...) arg needs no root and is
   emitted inline. */
static void emit_arg_rooted(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_UNKNOWN;
  /* a byref out-param arg is a slot address, not a heap value: it hoists its
     own rooted temp when one is needed (see emit_arg_or_default) */
  if (p && p->byref_out) { emit_arg_or_default(c, m, idx, provided, out); return; }
  if (!arg_wants_root(c, pt, provided)) { emit_arg_or_default(c, m, idx, provided, out); return; }
  Buf ab; memset(&ab, 0, sizeof ab);
  emit_arg_or_default(c, m, idx, provided, &ab);
  emit_rooted_operand(c, pt, provided, ab.p ? ab.p : default_value(pt), out);
  free(ab.p);
}

/* True if `name` is one of the callee's explicit keyword parameters (`k:` /
   `k: default`). Only keyword params consume a key from a forwarded `**hash`;
   positional params with the same name do not. Read from the callee's AST
   `keywords` array rather than pnames[], which mixes positional and keyword. */
int callee_has_kwarg(Compiler *c, Scope *m, const char *name) {
  if (!m || !name || m->def_node < 0) return 0;
  int pn = nt_ref(c->nt, m->def_node, "parameters");
  if (pn < 0) return 0;
  /* A `def m(...)` forwarding method synthesizes a key-NAMED param for every
     keyword its call sites pass (analyze_scope), so those params are
     keyword-matchable even though the AST declares no keywords. Positional
     synthesized params (__fwd_N) can never collide with a real key name. */
  int kwr = nt_ref(c->nt, pn, "keyword_rest");
  const char *kwr_type = kwr >= 0 ? nt_type(c->nt, kwr) : NULL;
  if (kwr_type && sp_streq(kwr_type, "ForwardingParameterNode"))
    return 1;
  int kn = 0; const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) {
    const char *kpn = nt_str(c->nt, kws[i], "name");
    if (kpn && sp_streq(kpn, name)) return 1;
  }
  return 0;
}

/* Like callee_has_kwarg, but for a param that is a REAL declared keyword param
   (present in the def's keywords list). Unlike callee_has_kwarg it does NOT
   treat every param of a `...` forwarding method as keyword-matchable, so it
   answers "can this param be bound by position?" (#3114). */
int callee_param_is_declared_kwarg(Compiler *c, Scope *m, const char *name) {
  if (!m || !name || m->def_node < 0) return 0;
  int pn = nt_ref(c->nt, m->def_node, "parameters");
  if (pn < 0) return 0;
  int kn = 0; const int *kws = nt_arr(c->nt, pn, "keywords", &kn);
  for (int i = 0; i < kn; i++) {
    const char *kpn = nt_str(c->nt, kws[i], "name");
    if (kpn && sp_streq(kpn, name)) return 1;
  }
  return 0;
}

/* A *rest method a class-value or boxed dispatch can call by packing the
   arguments no positional parameter takes into its rest: the rest is the
   boxed array a splat parameter starts as, and no keyword rides beside it
   (FFI::Struct.layout, a getter and a DSL in one, read as `klass.layout`). */
int rest_packable_arm(Compiler *c, Scope *s) {
  if (!s || s->rest_idx < 0 || s->kwrest_idx >= 0) return 0;
  LocalVar *rl = scope_local(s, s->pnames[s->rest_idx]);
  if (rl && rl->type != TY_POLY_ARRAY && rl->type != TY_UNKNOWN) return 0;
  for (int a = 0; a < s->nparams; a++)
    if (callee_param_is_declared_kwarg(c, s, s->pnames[a])) return 0;
  return 1;
}

/* True when the callee declares a keyword parameter (`k:` / `k: 1`). Ruby
   then takes a braceless `f(a, j: 2)` as keywords whatever its keys, so the
   hash is never one more positional argument and a key no parameter names is
   an unknown keyword -- not the options hash it is for a callee without
   keywords. */
int callee_declares_kwargs(Compiler *c, Scope *m) {
  if (!m || m->def_node < 0) return 0;
  int pn = nt_ref(c->nt, m->def_node, "parameters");
  if (pn < 0) return 0;
  int kn = 0; nt_arr(c->nt, pn, "keywords", &kn);
  return kn > 0;
}

/* Materialize the first `**hash` source inside `kwh` (a KeywordHashNode) into
   a typed temp so per-param extraction / kwrest collection can read it -- or,
   for a call into callee `m` that kwh_merged, every keyword source merged
   into one SymPolyHash. Returns the temp id, or -1 when kwh carries no
   double-splat (or its source is not a known hash). Sets *out_type to the
   materialized hash's type. Shared by emit_args_filled, emit_dispatch and
   emit_inline_call_x. */
static int empty_hash_literal(const NodeTable *nt, int id) {
  int n = 0;
  nt_arr(nt, id, "elements", &n);
  return n == 0;
}

/* The one argument a keyword hash that kwh_gathers is when it holds a key:
   a new Hash of its keywords, boxed, as CRuby passes. Answers 0, emitting
   nothing, when every operand is a literal `nil` or `{}`, which carry no
   keyword, so the hash is no argument at all. */
int emit_kwh_spread_arg(Compiler *c, int kwh, Buf *b) {
  const NodeTable *nt = c->nt;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  int any = 0;
  for (int e = 0; e < en && !any; e++) {
    int v = nt_ref(nt, el[e], "value");
    any = v < 0 || !(nt_kind(nt, v) == NK_NilNode ||
                     (nt_kind(nt, v) == NK_HashNode && empty_hash_literal(nt, v)));
  }
  if (any) emit_boxed(c, kwh, b);
  return any;
}

/* The class a `**` operand of the settled kind `t` names in CRuby's
   TypeError, or NULL when it may convert: a Hash is itself, nil carries no
   keywords, an object that defines #to_hash was already rewritten to call
   it (desugar_to_hash_splat) and one with method_missing may still answer
   it, and a boxed value is only known at run time. true and false raise
   too, but CRuby names the value, which only their boxed check knows
   (kw_splat_checked_boxed). */
static const char *kw_splat_bad_cls(Compiler *c, TyKind t) {
  if (ty_is_hash(t) || t == TY_NIL) return NULL;
  if (t == TY_BOOL) return "true or false";
  /* a Class or Module value has no #to_hash; it may be nil, which its
     boxed check tells apart (kw_splat_operand_nilable) and which names a
     Module as one */
  if (t == TY_CLASS) return "Class";
  /* conv_cls_name_of leaves these to the numeric slots, which convert them */
  if (t == TY_RATIONAL) return "Rational";
  if (t == TY_COMPLEX) return "Complex";
  if (ty_is_object(t) && (ty_object_class(t) < 0 ||
      comp_method_in_chain(c, ty_object_class(t), "method_missing", NULL) >= 0)) return NULL;
  return conv_cls_name_of(c, t);
}

/* May a boxed user object the conversion finds no #to_hash for answer one
   anyway? Only when some class of the program defines method_missing;
   otherwise its conversion is a TypeError as surely as a builtin's. A
   class's own #to_hash is called through the bridge (sp_kw_splat_conv). */
static int kw_splat_user_may_convert(Compiler *c) {
  for (int k = 0; k < c->nclasses; k++)
    if (comp_method_in_chain(c, k, "method_missing", NULL) >= 0) return 1;
  return 0;
}

/* See codegen_internal.h. */
int kw_splat_user_to_hash(Compiler *c) {
  for (int k = 0; k < c->nclasses; k++)
    if (comp_method_in_chain(c, k, "to_hash", NULL) >= 0) return 1;
  return 0;
}

/* Can the `**` operand `node`, of the settled kind `t`, be nil at run time? */
static int kw_splat_operand_nilable(Compiler *c, int node, TyKind t) {
  switch (nt_kind(c->nt, node)) {
    case NK_IntegerNode: case NK_FloatNode: case NK_RationalNode: case NK_ImaginaryNode:
    case NK_StringNode: case NK_InterpolatedStringNode: case NK_XStringNode:
    case NK_SymbolNode: case NK_InterpolatedSymbolNode: case NK_ArrayNode:
    case NK_HashNode: case NK_RangeNode: case NK_RegularExpressionNode: case NK_LambdaNode:
    case NK_TrueNode: case NK_FalseNode:
      return 0;   /* a literal is never nil */
    case NK_CallNode: {
      /* nor is the object `Foo.new` builds, short of a `def self.new` */
      const char *cn = nt_str(c->nt, node, "name");
      int rcv = nt_ref(c->nt, node, "receiver");
      if (cn && sp_streq(cn, "new") && rcv >= 0 &&
          (nt_kind(c->nt, rcv) == NK_ConstantReadNode || nt_kind(c->nt, rcv) == NK_ConstantPathNode)) {
        int ci = comp_class_index(c, nt_str(c->nt, rcv, "name"));
        if (ci >= 0 && comp_cmethod_in_chain(c, ci, "new", NULL) < 0) return 0;
      }
      break;
    }
    default: break;
  }
  /* the reading emit_boxed boxes a scalar by: its sentinel where the value
     is nilable */
  if (t == TY_INT)
    return call_returns_nullable_int(c, node) || box_nullable_arg(c, node) ||
           nt_kind(c->nt, node) == NK_InstanceVariableReadNode;
  if (t == TY_FLOAT) return call_returns_nullable_int(c, node) || box_nullable_arg(c, node);
  /* a Class value holds nil as SP_CLASS_NIL (`BasicObject.superclass`),
     which it boxes as nil (sp_box_class) */
  if (t == TY_CLASS) return 1;
  return needs_root(t);   /* a pointer-backed kind holds nil as NULL */
}

/* See codegen_internal.h. */
int kw_splat_checked_boxed(Compiler *c, int node) {
  TyKind t = comp_ntype(c, node);
  return t == TY_BOOL || (kw_splat_bad_cls(c, t) && kw_splat_operand_nilable(c, node, t));
}

/* See codegen_internal.h. */
int kw_splat_raises(Compiler *c, int node) {
  TyKind t = comp_ntype(c, node);
  return t == TY_BOOL || (kw_splat_bad_cls(c, t) && !kw_splat_operand_nilable(c, node, t));
}

/* A `**` operand of a kind kw_splat_bad_cls names, evaluated into g_pre where
   it stands: the TypeError raised outright, or, when the operand may be nil
   or is true or false, decided at run time on its boxed value. */
static void emit_kw_splat_bad_operand(Compiler *c, int node) {
  Buf hb; memset(&hb, 0, sizeof hb);
  if (kw_splat_checked_boxed(c, node)) {
    emit_boxed(c, node, &hb);
    emit_kw_splat_conv_check(c, TY_POLY, hb.p ? hb.p : "sp_box_nil()");
  }
  else {
    emit_expr(c, node, &hb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "(void)(%s);\n", hb.p ? hb.p : "0");
    emit_kw_splat_conv_check(c, comp_ntype(c, node), NULL);
  }
  free(hb.p);
}

/* See codegen_internal.h. */
void emit_kw_splat_conv_check(Compiler *c, TyKind t, const char *val) {
  if (t == TY_POLY) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "(void)sp_kw_splat_conv(%s, %d);\n", val, kw_splat_user_may_convert(c));
    return;
  }
  const char *cn = kw_splat_bad_cls(c, t);
  if (!cn) return;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Hash\");\n", cn);
}

/* See codegen_internal.h. */
void emit_kw_splat_conv_temp(Compiler *c, const char *tmp) {
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "%s = sp_kw_splat_conv(%s, %d);\n", tmp, tmp, kw_splat_user_may_convert(c));
}

/* See codegen_internal.h. */
void emit_kw_splat_operand_inline(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY || kw_splat_checked_boxed(c, node)) {
    buf_puts(b, "(void)sp_kw_splat_conv("); emit_boxed(c, node, b);
    buf_printf(b, ", %d); ", kw_splat_user_may_convert(c));
    return;
  }
  buf_puts(b, "(void)("); emit_boxed(c, node, b); buf_puts(b, "); ");
  const char *cn = kw_splat_bad_cls(c, t);
  if (cn) buf_printf(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Hash\"); ", cn);
}

/* A keyword key that is an expression (`f(key(1) => v)`), not a literal
   Symbol or String: what it names is known only when it has run. */
static int kw_key_computed(const NodeTable *nt, int key) {
  return nt_kind(nt, key) != NK_SymbolNode && nt_kind(nt, key) != NK_StringNode;
}

/* See codegen_internal.h. */
int kwh_sources_overlap(const NodeTable *nt, int kwh) {
  if (kwh < 0) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  /* a computed key's name is the run time's: it may name any keyword, so
     every source binds from one hash built in order -- wherever it stands,
     a String key ahead of it included */
  for (int e = 0; e < en; e++)
    if (nt_kind(nt, el[e]) == NK_AssocNode && kw_key_computed(nt, nt_ref(nt, el[e], "key"))) return 1;
  int nsplat = 0, lit = 0, merge = 0;
  for (int e = 0; e < en; e++) {
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) {
      if (nsplat++ || lit) merge = 1;
      continue;
    }
    /* a String key is a literal too: ahead of a `**` it lets a Symbol key
       beside it be overridden (`k1: 3, "s" => 4, **{k1: 5}` binds k1 5,
       where giving up on the String bound the 3) */
    int key = nt_ref(nt, el[e], "key");
    if (key < 0) return 0;
    lit = 1;
    /* a String key's text is its "content"; a key of neither kind has no
       text to compare */
    const char *field = nt_kind(nt, key) == NK_SymbolNode ? "value" :
                        nt_kind(nt, key) == NK_StringNode ? "content" : NULL;
    const char *kt = field ? nt_str(nt, key, field) : NULL;
    for (int e2 = 0; e2 < e && !merge && kt; e2++) {
      int k2 = nt_ref(nt, el[e2], "key");
      const char *t2 = nt_kind(nt, el[e2]) == NK_AssocNode && nt_kind(nt, k2) == nt_kind(nt, key)
                       ? nt_str(nt, k2, field) : NULL;
      if (t2 && sp_streq(t2, kt)) merge = 1;
    }
  }
  return merge;
}

/* See codegen_internal.h. */
int kwh_elem_dropped(const NodeTable *nt, int kwh, int e) {
  if (kwh < 0 || nt_kind(nt, kwh) != NK_KeywordHashNode) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  for (int i = 0; i < en; i++)
    if (nt_kind(nt, el[i]) != NK_AssocNode || nt_kind(nt, nt_ref(nt, el[i], "key")) != NK_SymbolNode) return 0;
  const char *kn = nt_str(nt, nt_ref(nt, el[e], "key"), "value");
  for (int i = e + 1; i < en; i++)
    if (sp_streq(nt_str(nt, nt_ref(nt, el[i], "key"), "value"), kn)) return 1;
  return 0;
}

/* See codegen_internal.h. */
void emit_dropped_value(Compiler *c, int v, Buf *b) {
  if (v < 0 || !subtree_has_side_effect(c, v)) return;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, v, &vb);
  emit_indent(b, g_indent);
  buf_printf(b, "(void)(%s);\n", vb.p ? vb.p : "0");
  free(vb.p);
}

/* See codegen_internal.h. */
int kwh_has_splat(const NodeTable *nt, int kwh) {
  int en = 0; const int *el = kwh >= 0 ? nt_arr(nt, kwh, "elements", &en) : NULL;
  for (int e = 0; e < en; e++)
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) return 1;
  return 0;
}

/* See codegen_internal.h. */
int kwh_merged(Compiler *c, Scope *m, int kwh) {
  return callee_declares_kwargs(c, m) && kwh_sources_overlap(c->nt, kwh);
}

/* See codegen_internal.h. */
int kwh_runs_ahead(Compiler *c, Scope *m, int kwh) {
  const NodeTable *nt = c->nt;
  if (kwh_merged(c, m, kwh)) return 1;
  if (kwh < 0) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode) continue;
    int v = nt_ref(nt, el[e], "value");
    TyKind t = v >= 0 ? comp_ntype(c, v) : TY_UNKNOWN;
    return v >= 0 && subtree_has_side_effect(c, v) &&
           (ty_is_hash(t) || t == TY_POLY || t == TY_NIL || kw_splat_bad_cls(c, t));
  }
  return 0;
}

/* See codegen_internal.h. */
int kwh_out_of_order(Compiler *c, Scope *m, int kwh) {
  const NodeTable *nt = c->nt;
  if (!m || kwh < 0 || nt_kind(nt, kwh) != NK_KeywordHashNode ||
      (!callee_declares_kwargs(c, m) && m->kwrest_idx < 0)) return 0;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  int last = -1;
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, el[e], "key");
    if (nt_kind(nt, el[e]) != NK_AssocNode || nt_kind(nt, key) != NK_SymbolNode) continue;
    const char *kn = nt_str(nt, key, "value");
    /* a key no keyword parameter names goes to the `**kw` rest, bound last */
    int at = m->nparams;
    for (int i = 0; i < m->nparams; i++)
      if (m->pnames[i] && sp_streq(m->pnames[i], kn) && callee_has_kwarg(c, m, kn)) { at = i; break; }
    if (at < last || (at == last && at < m->nparams)) return 1;
    last = at;
  }
  return 0;
}

/* The value `v` evaluated into a rooted temp in g_pre, pushed onto the
   g_argov overrides so its uses read the temp. */
static void emit_arg_temp(Compiler *c, int v) {
  TyKind at = comp_ntype(c, v);
  /* A shared String slot's read is the value form, a copy; a shared-handle
     parameter wants the OBJECT read here, not a fresh one of its bytes. So
     a variable's handle is taken too, just ahead, and recorded with the
     override below (ran_first_handle). */
  char sref[192];
  NodeKind vk = nt_kind(c->nt, v);
  int th = -1;
  if ((vk == NK_LocalVariableReadNode || vk == NK_InstanceVariableReadNode) &&
      strbuf_slot_ref(c, v, sref, sizeof sref)) {
    th = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_String *_t%d = %s; SP_GC_ROOT(_t%d);\n", th, sref, th);
  }
  int t = ++g_tmp;
  Buf hb; memset(&hb, 0, sizeof hb);
  emit_expr(c, v, &hb);
  emit_indent(g_pre, g_indent);
  if (at == TY_POLY) buf_puts(g_pre, "sp_RbVal");
  else emit_ctype(c, at, g_pre);
  buf_printf(g_pre, " _t%d = %s;", t, hb.p ? hb.p : default_value(at));
  if (at == TY_POLY) buf_printf(g_pre, " SP_GC_ROOT_RBVAL(_t%d);", t);
  else if (needs_root(at)) buf_printf(g_pre, " SP_GC_ROOT(_t%d);", t);
  buf_puts(g_pre, "\n");
  free(hb.p);
  /* every argument, however many: past the table's first MAX_ARG_OVERRIDE
     entries the rest never ran where a static check refuses the call, or ran
     at their slots, after the ones that follow them */
  argov_reserve();
  int k = 0;
  for (int j = 0; j < g_n_ran_hnd; j++)
    if (g_ran_hnd[j].idx < g_n_argov) g_ran_hnd[k++] = g_ran_hnd[j];
  g_n_ran_hnd = k;
  if (th >= 0) {
    if (g_n_ran_hnd == g_cap_ran_hnd) {
      g_cap_ran_hnd = g_cap_ran_hnd ? g_cap_ran_hnd * 2 : 16;
      RanHandle *nr = realloc(g_ran_hnd, sizeof *g_ran_hnd * (size_t)g_cap_ran_hnd);
      if (!nr) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
      g_ran_hnd = nr;
    }
    g_ran_hnd[g_n_ran_hnd++] = (RanHandle){ g_n_argov, v, t, th };
  }
  view_bind(v, "_t%d", t);
}

/* See codegen_internal.h. */
int arg_ran_first(int node, int from) {
  for (int i = from; i < g_n_argov; i++) if (g_argov_node[i] == node) return 1;
  return 0;
}

/* The temp an argument that ran first reads (arg_ran_first) from the
   `from`th override on, when `text` is its slot's rendering of it, the temp
   unconverted; -1 otherwise. */
static int ran_first_temp(int node, int from, const char *text) {
  for (int i = from; text && i < g_n_argov; i++) {
    int t;
    if (g_argov_node[i] == node && sp_streq(g_argov_text[i], text) && sscanf(text, "_t%d", &t) == 1) return t;
  }
  return -1;
}

/* See codegen_internal.h. */
int read_rebound_by(Compiler *c, int x, int after) {
  const NodeTable *nt = c->nt;
  if (after < 0) return 0;
  switch (nt_kind(nt, x)) {
    case NK_LocalVariableReadNode: {
      const char *nm = nt_str(nt, x, "name");
      if (!nm) return 0;
      if (subtree_writes_local(c, after, nm)) return 1;
      /* A proc that assigns the local through its cell rebinds it wherever
         it is called from: `m2(v, la.call)` for `la = -> { v = 0; 5 }` read
         v beside the call, in C's order, and bound the 0. Anything that
         may run code of the program's may call such a proc
         (subtree_may_run_proc); a builtin over plain values cannot, and a
         local no proc assigns keeps its read in place. */
      LocalVar *lv = scope_local(comp_scope_of(c, x), nm);
      return lv && lv->is_cell && lv->proc_rebinds && subtree_may_run_proc(c, after);
    }
    case NK_InstanceVariableReadNode: {
      /* nothing that runs no code and stores nothing can: arithmetic, typed-
         array reads and plain field reads (`Ctx.new(@buf, @opts.threads)`) */
      if (!subtree_may_reassign_state(c, after)) return 0;
      /* a later argument written in the same method, where self is the
         method's own (not a block run under another self), that calls no
         method of self's that assigns it cannot change it: `m(@head,
         tick)` reads @head in place when tick only counts */
      const char *iv = nt_str(nt, x, "name");
      Scope *xs = comp_scope_of(c, x);
      if (iv && xs && !xs->is_cmethod && xs->class_id >= 0 && xs == comp_scope_of(c, after) &&
          g_self && sp_streq(g_self, "self"))
        return subtree_may_write_ivar(c, after, iv, xs->class_id, 0);
      return subtree_has_side_effect(c, after);
    }
    case NK_GlobalVariableReadNode: case NK_ClassVariableReadNode:
      return subtree_may_reassign_state(c, after) && subtree_has_side_effect(c, after);
    /* a block's body reads when it runs, not where it is written */
    case NK_BlockNode: case NK_LambdaNode: return 0;
    default: {
      /* a value built of reads reads each where it is written: `m(*[x, 2],
         k: (x = 3))` spread the Array built after the keyword ran */
      if (x < 0) return 0;
      for (int i = 0; i < nt_num_refs(nt, x); i++)
        if (read_rebound_by(c, nt_ref_at(nt, x, i), after)) return 1;
      for (int i = 0; i < nt_num_arrs(nt, x); i++) {
        int n = 0; const int *ids = nt_arr_at(nt, x, i, &n);
        for (int j = 0; j < n; j++) if (read_rebound_by(c, ids[j], after)) return 1;
      }
      return 0;
    }
  }
}

/* The argument `v` of a call binding in parameter order, evaluated ahead of
   the binding into a rooted temp its uses read (emit_arg_temp). A value with no C type -- nil -- is evaluated for its effect
   alone, and its uses read a 0 the binding takes as nil. A read with no
   effect of its own is taken too when `rebound` says a later value can
   change what it reads (read_rebound_by): `m(b: x, a: (x = 2))` bound b the
   2. */
static void emit_arg_first(Compiler *c, int v, int rebound, Buf *b) {
  const NodeTable *nt = c->nt;
  int x = nt_kind(nt, v) == NK_SplatNode ? nt_ref(nt, v, "expression") : v;
  if (x < 0 || nt_kind(nt, v) == NK_BlockArgumentNode) return;
  /* one a hoist around the call ran already reads its temp: a class
     value's arms bind what the dispatch hoisted */
  if (arg_ran_first(x, 0)) return;
  int effect = subtree_has_side_effect(c, x);
  if (!effect && !rebound) return;
  TyKind at = comp_ntype(c, x);
  if (ty_is_object(at) || c_type_name(at)) {
    emit_arg_temp(c, x);
    return;
  }
  if (!effect) return;   /* a nil read reads nil whenever it runs */
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, x, &vb);
  emit_indent(b, g_indent);
  buf_printf(b, "(void)(%s);\n", vb.p ? vb.p : "0");
  /* A value that raises (`h.id`, a NoMethodError the compiler knows) never
     answers one, so the call after it is dead, but its slot still has to
     build. A bare 0 is no sp_RbVal for a slot that boxes it (#6024); a
     raise's own shape is what every slot already coerces. */
  int raises = vb.p && strncmp(past_open_parens(vb.p), "sp_raise_", 9) == 0;
  free(vb.p);
  argov_reserve();
  view_bind(x, "%s", raises ? "sp_raise_nomethod(\"\")" : "0");
}

/* The values of a call's arguments in the order CRuby runs them: each
   positional, and each keyword's value, a key that is an expression
   (`f(*xs, key(1) => v)`) ahead of it; a literal Symbol or String key has
   nothing to run. `dsplat[i]` marks a `**` operand. The caller frees both. */
static int *source_values(const NodeTable *nt, const int *argv, int argc, int *n, char **dsplat) {
  int nv = 0, cap = 8;
  int *vals = malloc(sizeof(int) * (size_t)cap);
  char *ds = malloc((size_t)cap);
  for (int k = 0; argv && k < argc; k++) {
    int kn = 1; const int *kv = &argv[k];
    if (nt_kind(nt, argv[k]) == NK_KeywordHashNode) kv = nt_arr(nt, argv[k], "elements", &kn);
    for (int e = 0; kv && e < kn; e++) {
      int pair[2] = { -1, kv[e] };
      if (kv != &argv[k]) {
        int key = nt_kind(nt, kv[e]) == NK_AssocNode ? nt_ref(nt, kv[e], "key") : -1;
        if (key >= 0 && nt_kind(nt, key) != NK_SymbolNode && nt_kind(nt, key) != NK_StringNode) pair[0] = key;
        pair[1] = nt_ref(nt, kv[e], "value");
      }
      for (int p = 0; p < 2; p++) {
        if (pair[p] < 0) continue;
        if (nv == cap) { cap *= 2; vals = realloc(vals, sizeof(int) * (size_t)cap); ds = realloc(ds, (size_t)cap); }
        ds[nv] = p == 1 && kv != &argv[k] && nt_kind(nt, kv[e]) == NK_AssocSplatNode;
        vals[nv++] = pair[p];
      }
    }
  }
  *n = nv;
  *dsplat = ds;
  return vals;
}

/* Can a value after the i-th of `vals`, or a node of `after`, change what
   the i-th reads? */
static int value_rebound(Compiler *c, const int *vals, int nv, int i, const int *after, int nafter) {
  const NodeTable *nt = c->nt;
  int x = nt_kind(nt, vals[i]) == NK_SplatNode ? nt_ref(nt, vals[i], "expression") : vals[i];
  for (int j = i + 1; j < nv; j++) if (read_rebound_by(c, x, vals[j])) return 1;
  for (int j = 0; j < nafter; j++) if (read_rebound_by(c, x, after[j])) return 1;
  return 0;
}

/* See codegen_internal.h. */
int args_order_matters(Compiler *c, const int *argv, int argc, const int *after, int nafter) {
  int nv = 0, eff = 0, matters = 0; char *ds = NULL;
  int *vals = source_values(c->nt, argv, argc, &nv, &ds);
  for (int i = 0; i < nv && !matters; i++) {
    eff += subtree_has_side_effect(c, vals[i]);
    matters = eff > 1 || value_rebound(c, vals, nv, i, after, nafter);
  }
  free(vals); free(ds);
  return matters;
}

/* The defaults a call into `m` may fill at its site, where the binding
   renders them in their parameters' slots: each optional's, and each
   keyword's no literal key of `kwh` names. The caller frees them. */
static int *call_site_defaults(Compiler *c, Scope *m, int kwh, int *n) {
  int *d = malloc(sizeof(int) * (size_t)((m ? m->nparams : 0) + 1)), nd = 0;
  for (int i = 0; m && m->pdefault && i < m->nparams; i++) {
    if (m->pdefault[i] < 0) continue;
    if (kwh >= 0 && m->pnames[i] && callee_has_kwarg(c, m, m->pnames[i]) &&
        kwh_lookup(c->nt, kwh, m->pnames[i]) >= 0) continue;
    d[nd++] = m->pdefault[i];
  }
  *n = nd;
  return d;
}

/* Can the argument `after` change what the default `d`, filled at the call
   site, reads? The binding renders the default at its parameter's slot,
   ahead of the arguments bound after it: `def g(a = @y, b)` read @y before
   `g(@y = 2)` assigned it. An instance, global or class variable it reads
   is asked of read_rebound_by; its locals are the callee's parameters,
   which no argument of the caller's assigns. */
static int default_rebound_by(Compiler *c, int d, int after) {
  const NodeTable *nt = c->nt;
  if (d < 0) return 0;
  switch (nt_kind(nt, d)) {
    case NK_LocalVariableReadNode: case NK_BlockNode: case NK_LambdaNode: return 0;
    case NK_InstanceVariableReadNode: case NK_GlobalVariableReadNode:
    case NK_ClassVariableReadNode:
      return read_rebound_by(c, d, after);
    default:
      for (int i = 0; i < nt_num_refs(nt, d); i++)
        if (default_rebound_by(c, nt_ref_at(nt, d, i), after)) return 1;
      for (int i = 0; i < nt_num_arrs(nt, d); i++) {
        int n = 0; const int *ids = nt_arr_at(nt, d, i, &n);
        for (int j = 0; j < n; j++) if (default_rebound_by(c, ids[j], after)) return 1;
      }
      return 0;
  }
}

/* See codegen_internal.h. */
int emit_args_before_binding(Compiler *c, Scope *m, const int *argv, int argc, Buf *b) {
  const NodeTable *nt = c->nt;
  int kwh = argc > 0 && argv && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  int nd = 0, *dfl = call_site_defaults(c, m, kwh, &nd);
  int nv = 0, ev = 0, ed = 0, run = 0; char *ds = NULL;
  int *vals = source_values(nt, argv, argc, &nv, &ds);
  for (int i = 0; i < nv; i++) {
    ev += subtree_has_side_effect(c, vals[i]);
    if (value_rebound(c, vals, nv, i, dfl, nd)) run = 1;
    for (int j = 0; j < nd && !run; j++) run = default_rebound_by(c, dfl[j], vals[i]);
  }
  for (int i = 0; i < nd; i++) ed += subtree_has_side_effect(c, dfl[i]);
  if ((ev && ed) || (kwh >= 0 && ev > 1) || kwh_out_of_order(c, m, kwh)) run = 1;
  if (run) emit_args_before(c, argv, argc, dfl, nd, b);
  free(vals); free(ds); free(dfl);
  return run;
}

/* See codegen_internal.h. */
void emit_args_before(Compiler *c, const int *argv, int argc, const int *after, int nafter, Buf *b) {
  int nv = 0; char *ds = NULL;
  int *vals = source_values(c->nt, argv, argc, &nv, &ds);
  Buf *sv_pre = g_pre; g_pre = b;
  for (int i = 0; i < nv; i++)
    emit_arg_first(c, vals[i], value_rebound(c, vals, nv, i, after, nafter), b);
  g_pre = sv_pre;
  free(vals); free(ds);
}

/* See codegen_internal.h. */
void emit_args_in_source_order(Compiler *c, const int *argv, int argc, Buf *b) {
  emit_args_before(c, argv, argc, NULL, 0, b);
}

/* See codegen_internal.h. */
void emit_args_off_self(Compiler *c, const int *argv, int argc, Buf *b) {
  int nv = 0; char *ds = NULL;
  int *vals = source_values(c->nt, argv, argc, &nv, &ds);
  Buf *sv_pre = g_pre; g_pre = b;
  for (int i = 0; i < nv; i++) {
    int x = nt_kind(c->nt, vals[i]) == NK_SplatNode ? nt_ref(c->nt, vals[i], "expression") : vals[i];
    emit_arg_first(c, vals[i], value_rebound(c, vals, nv, i, NULL, 0) || fiber_body_uses_self(c, x), b);
  }
  g_pre = sv_pre;
  free(vals); free(ds);
}

/* See codegen_internal.h. */
int emit_ds_hash_merge(Compiler *c, int kwh, int any_key, TyKind *out_type) {
  const NodeTable *nt = c->nt;
  int mh = ++g_tmp;
  const char *hk = any_key ? "PolyPoly" : "SymPoly";
  *out_type = any_key ? TY_POLY_POLY_HASH : TY_SYM_POLY_HASH;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hk, mh, hk, mh);
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int v = nt_ref(nt, el[e], "value");
    /* each source renders into a side buffer first: a literal drains its
       own construction into g_pre, which must land before the line using it */
    Buf vb; memset(&vb, 0, sizeof vb);
    int key = nt_ref(nt, el[e], "key");
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode && any_key && nt_kind(nt, key) != NK_SymbolNode) {
      /* a key of another class, into the hash that takes any: a computed
         one runs ahead of its value, into a rooted temp */
      int kt = ++g_tmp;
      Buf kb; memset(&kb, 0, sizeof kb);
      emit_boxed(c, key, &kb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", kt, kb.p ? kb.p : "sp_box_nil()", kt);
      free(kb.p);
      emit_boxed(c, v, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyPolyHash_set(_t%d, _t%d, %s);\n", mh, kt, vb.p ? vb.p : "sp_box_nil()");
    }
    else if (nt_kind(nt, el[e]) != NK_AssocSplatNode && kw_key_computed(nt, key)) {
      /* a computed key into the Symbol-keyed hash a `**kwrest` collects:
         it has to answer a Symbol, as every key there does */
      int kt = ++g_tmp;
      Buf kb; memset(&kb, 0, sizeof kb);
      emit_boxed(c, key, &kb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", kt, kb.p ? kb.p : "sp_box_nil()", kt);
      free(kb.p);
      emit_boxed(c, v, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_poly_hkey_sym(_t%d), %s);\n", mh, kt, vb.p ? vb.p : "sp_box_nil()");
    }
    else if (nt_kind(nt, el[e]) != NK_AssocSplatNode && kwh_elem_dropped(nt, kwh, e))
      emit_dropped_value(c, v, g_pre);
    else if (nt_kind(nt, el[e]) != NK_AssocSplatNode) {
      emit_boxed(c, v, &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, any_key ? "sp_PolyPolyHash_set(_t%d, sp_box_sym(sp_sym_intern(\"%s\")), %s);\n"
                                : "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), %s);\n", mh,
                 nt_str(nt, key, "value"), vb.p ? vb.p : "sp_box_nil()");
    }
    else if (v < 0) {
      const char *akw = anon_kwrest_name(c, el[e]);
      if (akw) {
        int aany = anon_kwrest_type(c, el[e]) == TY_POLY_POLY_HASH;
        emit_indent(g_pre, g_indent);
        if (any_key && !aany) buf_printf(g_pre, "sp_kw_merge_any(_t%d, sp_box_obj(lv_%s, SP_BUILTIN_SYM_POLY_HASH));\n",
                                         mh, rename_local(akw));
        else buf_printf(g_pre, "sp_%sHash_update(_t%d, lv_%s);\n", hk, mh, rename_local(akw));
      }
    }
    else {
      TyKind t = comp_ntype(c, v);
      const char *hn = ty_hash_cname(t);
      if (!any_key && hn && sp_streq(hn, "SymPoly")) {
        int src = ++g_tmp;
        emit_expr(c, v, &vb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash *_t%d = %s; SP_GC_ROOT(_t%d);\n", src, vb.p ? vb.p : "", src);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash_update(_t%d, _t%d);\n", mh, src);
      }
      else if (ty_is_hash(t) || t == TY_POLY) {
        /* another hash kind, or one only known at run time: merged by a
           runtime walk, which takes nil as no keywords and raises CRuby's
           TypeError for anything else that is not a Hash */
        emit_boxed(c, v, &vb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "%s(_t%d, %s);\n", any_key ? "sp_kw_merge_any" : "sp_kwrest_merge_poly",
                   mh, vb.p ? vb.p : "sp_box_nil()");
      }
      else if (nt_kind(nt, v) != NK_NilNode &&
               !(nt_kind(nt, v) == NK_HashNode && empty_hash_literal(nt, v))) {
        /* no keywords to merge (`**f` answering nil, `**1`): only the operand
           to evaluate and, for another class, CRuby's TypeError to raise */
        if (kw_splat_bad_cls(c, t)) emit_kw_splat_bad_operand(c, v);
        else {
          emit_expr(c, v, &vb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "(void)(%s);\n", vb.p ? vb.p : "0");
        }
      }
    }
    free(vb.p);
  }
  return mh;
}

/* The `**` operand `node`, materialized into temp `tmp`, reads that temp
   wherever the call renders it again: a keyword hash bound to a positional
   or rest parameter builds itself from its operands, which ran the operand
   a second time. A `tmp` of -1 is an operand that brings no keywords, run
   and converted already, which reads as nothing. The caller pops the
   override with its own. */
static void ds_operand_reads_temp(int node, int tmp) {
  argov_reserve();
  if (tmp < 0) view_bind(node, "((void)0)");
  else view_bind(node, "_t%d", tmp);
}

/* `s` as a C string literal: a key a message names (`"q\"z"`) may carry
   what a C literal escapes. */
static void emit_c_str(Buf *b, const char *s) {
  buf_puts(b, "\"");
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') buf_printf(b, "\\%c", *p);
    else if (*p < 0x20 || *p == 0x7f) buf_printf(b, "\\%03o", *p);
    else buf_printf(b, "%c", *p);
  }
  buf_puts(b, "\"");
}

/* Can `kwh` bring a key that is no Symbol: a literal String or computed
   key, or a `**` operand of a hash kind keyed by something else, or only
   known at run time? An anonymous `**` can when the rest it forwards takes
   any key. */
static int kwh_spreads_any_key(Compiler *c, int kwh) {
  const NodeTable *nt = c->nt;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    if (nt_kind(nt, el[e]) == NK_AssocNode && nt_kind(nt, nt_ref(nt, el[e], "key")) != NK_SymbolNode) return 1;
    if (nt_kind(nt, el[e]) != NK_AssocSplatNode) continue;
    int v = nt_ref(nt, el[e], "value");
    TyKind t = v >= 0 ? comp_ntype(c, v) : anon_kwrest_name(c, el[e]) ? anon_kwrest_type(c, el[e]) : TY_UNKNOWN;
    if (t == TY_POLY || (ty_is_hash(t) && ty_hash_key(t) != TY_SYMBOL)) return 1;
  }
  return 0;
}

int emit_ds_hash_materialize(Compiler *c, Scope *m, int kwh, TyKind *out_type) {
  const NodeTable *nt = c->nt;
  int ds_hash_tmp = -1;
  *out_type = TY_UNKNOWN;
  if (kwh < 0) return -1;
  /* A `**` that may bring a key other than a Symbol merges into a hash
     taking any key, for a callee without a `**kwrest`, where such a key can
     only be unknown and the check names it: merged into the Symbol-keyed
     one it raised a TypeError instead (`def m(k:)` called `m(k: 1, **{"s"
     => 2})`). A `**kwrest` takes the hash it collects: of any key where a
     call brings it one (kwrest_any_key), Symbol-keyed otherwise. */
  if (kwh_merged(c, m, kwh))
    return emit_ds_hash_merge(c, kwh, m->kwrest_idx < 0 ? kwh_spreads_any_key(c, kwh) : kwrest_any_key(c, m),
                              out_type);
  int en2 = 0; const int *elems2 = nt_arr(nt, kwh, "elements", &en2);
  for (int e = 0; e < en2; e++) {
    const char *ety2 = nt_type(nt, elems2[e]);
    if (!ety2 || !sp_streq(ety2, "AssocSplatNode")) continue;
    int inner2 = nt_ref(nt, elems2[e], "value");
    if (inner2 >= 0) {
      *out_type = comp_ntype(c, inner2);
      if (ty_is_hash(*out_type)) {
        ds_hash_tmp = ++g_tmp;
        /* Render the source into a side buffer first: a hash LITERAL
           (`**{ ... }`) drains its own construction into g_pre, which must
           land before -- not inside -- this temp's declaration line. A bare
           variable emits with no prelude, so this is a no-op there. */
        Buf hb; memset(&hb, 0, sizeof hb);
        emit_expr(c, inner2, &hb);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, *out_type, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", ds_hash_tmp, hb.p ? hb.p : "");
        /* A source the call builds (`**opts(i)`) is held by this temp alone,
           and the binding allocates before it reads it -- the new Hash a
           positional or rest parameter takes is a copy of it. */
        if (arg_wants_root(c, *out_type, inner2)) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ds_hash_tmp);
        }
        free(hb.p);
        if (kw_splat_operand_nilable(c, inner2, *out_type)) {
          /* nil (a NULL hash, `**f` where f answers an unset slot) carries
             no keywords: an empty hash stands in for it, so the keyword
             check, extraction and keyword-rest read no NULL */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT(_t%d); if (!_t%d) _t%d = sp_%sHash_new();\n",
                     ds_hash_tmp, ds_hash_tmp, ds_hash_tmp, ty_hash_cname(*out_type));
        }
        ds_operand_reads_temp(inner2, ds_hash_tmp);
      }
      else if (nt_kind(nt, inner2) == NK_HashNode && empty_hash_literal(nt, inner2)) {
        /* `**{}` types as no hash at all; it carries no keywords */
        *out_type = TY_SYM_POLY_HASH;
        ds_hash_tmp = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n",
                   ds_hash_tmp, ds_hash_tmp);
      }
      else if (*out_type == TY_POLY) {
        /* The `**` source is a bare poly value that holds a Hash at run time
           (e.g. an element read from a poly array, `arr.map { |c| f(**c) }`).
           Materialize it; per-param values are pulled by a runtime key lookup
           in emit_ds_param_extract. (#2885) */
        ds_hash_tmp = ++g_tmp;
        Buf hb; memset(&hb, 0, sizeof hb);
        emit_expr(c, inner2, &hb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", ds_hash_tmp, hb.p ? hb.p : "sp_box_nil()");
        /* as above; and a user object's #to_hash answers a new Hash, which
           the temp holds alone once converted */
        if (arg_wants_root(c, TY_POLY, inner2) || kw_splat_user_to_hash(c)) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", ds_hash_tmp);
        }
        free(hb.p);
        ds_operand_reads_temp(inner2, ds_hash_tmp);
        /* the keywords bound, checked and collected are the converted
           operand's: a user object converts through its #to_hash here, once */
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", ds_hash_tmp);
        emit_kw_splat_conv_temp(c, tn);
      }
      else if (kw_splat_checked_boxed(c, inner2)) {
        /* A slot that is no Hash but may hold nil (`**f` where f answers an
           Integer or nil), or true or false: boxed, it is checked, binds and
           is judged as a boxed operand is, so nil carries no keywords. */
        *out_type = TY_POLY;
        ds_hash_tmp = ++g_tmp;
        Buf hb; memset(&hb, 0, sizeof hb);
        emit_boxed(c, inner2, &hb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", ds_hash_tmp, hb.p ? hb.p : "sp_box_nil()");
        if (arg_wants_root(c, TY_POLY, inner2)) {   /* as above */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", ds_hash_tmp);
        }
        free(hb.p);
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", ds_hash_tmp);
        emit_kw_splat_conv_check(c, TY_POLY, tn);
        ds_operand_reads_temp(inner2, -1);
      }
      else if (kw_splat_bad_cls(c, *out_type)) {
        /* `**1`: no keywords to bind, only the operand to evaluate and
           CRuby's TypeError to raise before any keyword is checked */
        emit_kw_splat_bad_operand(c, inner2);
        ds_operand_reads_temp(inner2, -1);
      }
      else if (*out_type == TY_NIL) {
        /* `**nil`, or `**f` where f answers nil (f still runs, once): no
           keywords to bind, so for a callee with keyword parameters an empty
           hash stands in for it as for `**{}`, and a required keyword it
           leaves unbound is missing */
        if (nt_kind(nt, inner2) != NK_NilNode) {
          Buf hb; memset(&hb, 0, sizeof hb);
          emit_expr(c, inner2, &hb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "(void)(%s);\n", hb.p ? hb.p : "0");
          free(hb.p);
          ds_operand_reads_temp(inner2, -1);
        }
        if (callee_declares_kwargs(c, m)) {
          *out_type = TY_SYM_POLY_HASH;
          ds_hash_tmp = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n",
                     ds_hash_tmp, ds_hash_tmp);
        }
      }
    }
    else {
      /* Anonymous `**`: materialize the enclosing __anon_kwrest (SymPolyHash)
         so the per-param extraction and kwrest collection can read it. */
      const char *akw = anon_kwrest_name(c, elems2[e]);
      if (akw) {
        *out_type = anon_kwrest_type(c, elems2[e]);
        ds_hash_tmp = ++g_tmp;
        emit_indent(g_pre, g_indent);
        emit_ctype(c, *out_type, g_pre);
        buf_printf(g_pre, " _t%d = lv_%s;\n", ds_hash_tmp, rename_local(akw));
      }
    }
    break;
  }
  return ds_hash_tmp;
}

/* True when a `**hash` of this type can hold no Symbol key, so no keyword
   parameter can take anything from it. */
static int ds_hash_keys_not_symbols(TyKind t) {
  TyKind k = ty_hash_key(t);
  return k == TY_STRING || k == TY_INT;
}

static void kw_param_names(const NodeTable *nt, const int *kws, int kn, int required, Buf *out) {
  for (int ki = 0; ki < kn; ki++) {
    const char *kpn = nt_str(nt, kws[ki], "name");
    const char *kty = nt_type(nt, kws[ki]);
    if (kpn && (!required || (kty && sp_streq(kty, "RequiredKeywordParameterNode"))))
      buf_printf(out, "\"%s\", ", kpn);
  }
}

/* The keywords of a call carrying a `**hash`, checked at run time against the
   callee's keyword params the way CRuby checks them: a required keyword that
   neither a literal key nor the hash supplies raises `missing keyword`, and
   with no **kwrest a key, literal or from the hash, naming no parameter
   raises `unknown keyword`. The static checks in emit_call_arity_check stand
   aside for a call with a `**`, which left `f(**{})` against `def f(k:)`
   binding nil and `f(z: 2, **h)` dropping the z, and the object-receiver and
   inlined calls ran no check at all. A call kwh_merged has every key, the
   literal ones too, in its merged hash, which is checked alone. */
void emit_ds_kwarg_check(Compiler *c, Scope *m, int kwh, int ds_hash_tmp, TyKind ds_hash_type) {
  const NodeTable *nt = c->nt;
  if (ds_hash_tmp < 0 || kwh < 0 || !m || m->def_node < 0) return;
  if (ds_hash_type != TY_POLY && !ty_is_hash(ds_hash_type)) return;
  int pn = nt_ref(nt, m->def_node, "parameters");
  int kn = 0; const int *kws = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn) : NULL;
  if (kn == 0) return;
  int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
  int nsplat = 0, merged = kwh_merged(c, m, kwh);
  for (int e = 0; e < en; e++)
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) nsplat++;
  if (nsplat != 1 && !merged) return;
  int chk = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "static const char *const _kw%d[] = {", chk);
  kw_param_names(nt, kws, kn, 0, g_pre);
  buf_printf(g_pre, "0}, *const _kr%d[] = {", chk);
  kw_param_names(nt, kws, kn, 1, g_pre);
  buf_printf(g_pre, "0}, *const _kl%d[] = {", chk);
  for (int e = 0; e < en && !merged; e++) {
    int key = nt_ref(nt, el[e], "key");
    const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
    const char *kname = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kname) buf_printf(g_pre, "\"%s\", ", kname);
  }
  buf_puts(g_pre, "0};\n");
  /* A literal key that is no Symbol is unknown to a callee without a
     `**kwrest` as surely as one no keyword names, and CRuby names each in
     the order the hash holds it: the plan's list, inspected here, stands
     for the literal keys (kw_plan). Those written after the `**` come
     after the keys it brings (`f(**{z: 2}, y: 3)` is `unknown keywords:
     :z, :y`), so a call writing one there says how many stand ahead of it
     (sp_kwargs_verify_at). */
  KwPlan P;
  kw_plan(c, m, kwh, &P);
  int strkey = 0, splat_el = -1, nbefore = 0;
  for (int e = 0; e < en && !merged; e++) {
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode && splat_el < 0) splat_el = e;
    if (nt_kind(nt, el[e]) == NK_AssocNode && nt_kind(nt, nt_ref(nt, el[e], "key")) == NK_StringNode) strkey = 1;
  }
  int nunk = P.nunknown < 32 ? P.nunknown : 32;
  while (nbefore < nunk && P.unknown_el[nbefore] < splat_el) nbefore++;
  int after = !merged && nbefore < nunk;
  int listed = (strkey || after) && m->kwrest_idx < 0;
  if (listed) {
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "static const char *const _ku%d[] = {", chk);
    for (int u = 0; u < nunk; u++) {
      int key = nt_ref(nt, el[P.unknown_el[u]], "key");
      int is_sym = nt_kind(nt, key) == NK_SymbolNode;
      char iv[300];
      kw_key_inspect(nt_str(nt, key, is_sym ? "value" : "content"), is_sym, iv, sizeof iv);
      emit_c_str(g_pre, iv);
      buf_puts(g_pre, ", ");
    }
    buf_puts(g_pre, "0};\n");
  }
  char tn[32]; snprintf(tn, sizeof tn, "_t%d", ds_hash_tmp);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, !listed ? "sp_kwargs_verify(" : after ? "sp_kwargs_verify_at(" : "sp_kwargs_verify_lit(");
  if (ds_hash_type == TY_POLY) buf_puts(g_pre, tn);
  else emit_boxed_text(c, ds_hash_type, tn, g_pre);
  buf_printf(g_pre, ", _kw%d, _kr%d, _kl%d, ", chk, chk, chk);
  if (listed) buf_printf(g_pre, "_ku%d, ", chk);
  if (listed && after) buf_printf(g_pre, "%d, ", nbefore);
  buf_printf(g_pre, "%d);\n", m->kwrest_idx < 0);
}

void emit_kwhash_verify(Compiler *c, Scope *m, int hash_tmp, TyKind hash_type, Buf *out) {
  const NodeTable *nt = c->nt;
  if (!m || m->def_node < 0) return;
  int pn = nt_ref(nt, m->def_node, "parameters");
  int kn = 0; const int *kws = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn) : NULL;
  if (kn == 0) return;
  char tn[32]; snprintf(tn, sizeof tn, "_t%d", hash_tmp);
  buf_puts(out, "sp_kwargs_verify(");
  emit_boxed_text(c, hash_type, tn, out);
  buf_puts(out, ", (const char *const[]){");
  kw_param_names(nt, kws, kn, 0, out);
  buf_puts(out, "0}, (const char *const[]){");
  kw_param_names(nt, kws, kn, 1, out);
  buf_printf(out, "0}, (const char *const[]){0}, %d); ", m->kwrest_idx < 0);
}

/* Emit the value for KEYWORD param `i` extracted by name from a materialized
   `**hash` temp, falling back to the param's default when the key is absent.
   Shared by emit_args_filled and emit_dispatch. */
/* Param i's default as one expression: the statements it hoists run inside
   it, so only where the default is taken. */
static void emit_ds_default(Compiler *c, Scope *m, int i, Buf *out) {
  Buf dp; memset(&dp, 0, sizeof dp);
  Buf dv; memset(&dv, 0, sizeof dv);
  Buf *sv_pre = g_pre; g_pre = &dp;
  emit_arg_or_default(c, m, i, -1, &dv);
  g_pre = sv_pre;
  if (dp.p && dp.p[0]) buf_printf(out, "({ %s %s; })", dp.p, dv.p ? dv.p : "0");
  else buf_puts(out, dv.p ? dv.p : "");
  free(dp.p); free(dv.p);
}

void emit_ds_param_extract(Compiler *c, Scope *m, int i, int ds_hash_tmp,
                                  TyKind ds_hash_type, Buf *out) {
  const char *hn = ty_hash_cname(ds_hash_type);
  LocalVar *plv = scope_local(m, m->pnames[i]);
  if (plv && plv->byref_out) {
    Buf vb; memset(&vb, 0, sizeof vb);
    plv->byref_out = 0;
    emit_ds_param_extract(c, m, i, ds_hash_tmp, ds_hash_type, &vb);
    plv->byref_out = 1;
    emit_lent_temp(vb.p, out);
    free(vb.p);
    return;
  }
  TyKind pt = plv ? plv->type : TY_INT;
  if (ds_hash_type == TY_POLY) {
    /* Bare-poly `**` source (a Hash only known at run time): pull each keyword
       by a runtime key lookup, unboxing to the param type. (#2885) */
    Buf ub; memset(&ub, 0, sizeof ub);
    emit_unbox_nilable_text(c, pt, "_v", &ub);
    if (m->pdefault && m->pdefault[i] >= 0) {
      Buf db; memset(&db, 0, sizeof db);
      emit_ds_default(c, m, i, &db);
      buf_printf(out,
                 "({ sp_bool _f=0; sp_RbVal _v = sp_poly_hash_get_pair_val(_t%d, "
                 "sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? (%s) : (%s); })",
                 ds_hash_tmp, m->pnames[i], ub.p ? ub.p : "_v",
                 db.p ? db.p : default_value(pt));
      free(db.p);
    }
    else {
      buf_printf(out,
                 "({ sp_bool _f=0; sp_RbVal _v = sp_poly_hash_get_pair_val(_t%d, "
                 "sp_box_sym(sp_sym_intern(\"%s\")), &_f); (void)_f; (%s); })",
                 ds_hash_tmp, m->pnames[i], ub.p ? ub.p : "_v");
    }
    free(ub.p);
    return;
  }
  if (hn && !ds_hash_keys_not_symbols(ds_hash_type)) {
    /* SymPoly: get returns sp_RbVal, unbox to param type.
       Other sym/str keyed hashes: get returns the value type directly. */
    TyKind hval = ty_hash_val(ds_hash_type);
    Buf vb; memset(&vb, 0, sizeof vb);
    /* A hash whose keys are of any class (`{ :a => 1, 1 => 2 }`) takes its
       key boxed: a bare sp_sym is no sp_RbVal, and the C compiler refused
       the call. */
    char key[160];
    snprintf(key, sizeof key, ty_hash_key(ds_hash_type) == TY_POLY ?
             "sp_box_sym(sp_sym_intern(\"%s\"))" : "sp_sym_intern(\"%s\")", m->pnames[i]);
    char get_expr[256];
    snprintf(get_expr, sizeof get_expr, "sp_%sHash_get(_t%d, %s)", hn, ds_hash_tmp, key);
    if (hval == TY_POLY) emit_unbox_nilable_text(c, pt, get_expr, &vb);
    else buf_puts(&vb, get_expr);
    /* An optional keyword param (one with a default) whose key may be
       absent from the forwarded hash falls back to its default: a bare
       get returns nil and silently drops the callee's default value. */
    if (m->pdefault && m->pdefault[i] >= 0) {
      Buf db; memset(&db, 0, sizeof db);
      emit_ds_default(c, m, i, &db);
      buf_printf(out, "(sp_%sHash_has_key(_t%d, %s) ? (%s) : (%s))",
                 hn, ds_hash_tmp, key,
                 vb.p ? vb.p : "", db.p ? db.p : default_value(pt));
      free(db.p);
    }
    else buf_puts(out, vb.p ? vb.p : "");
    free(vb.p);
  }
  else {
    /* Hash type unknown (no C-name), so the key can't be extracted; bind the
       param's declared default rather than the bare type-default, matching the
       key-absent fallback in the typed branch above. A String- or
       Integer-keyed hash (an empty `{}` types as one) holds no Symbol key to
       extract either: looking one up passed an sp_sym as its key, which the
       C compiler refused. */
    emit_arg_or_default(c, m, i, -1, out);
  }
}

/* See codegen_internal.h. */
int kwrest_any_key(Compiler *c, const Scope *m) {
  if (!m || m->kwrest_idx < 0 || m->kwrest_idx >= m->nparams || !m->pnames[m->kwrest_idx]) return 0;
  LocalVar *lv = scope_local((Scope *)m, m->pnames[m->kwrest_idx]);
  (void)c;
  return lv && lv->type == TY_POLY_POLY_HASH;
}

/* Collect the call's unbound keyword args -- literal pairs not naming an
   explicit keyword param, plus merged `**hash` sources -- into a fresh hash
   for the callee's `**kwrest` param, of the kind the parameter is: a
   SymPolyHash, or a PolyPolyHash keeping a String or other key where some
   call brings one (kwrest_any_key). Returns the hash temp id. Shared by
   emit_args_filled and emit_dispatch. */
int emit_kwrest_collect(Compiler *c, Scope *m, int kwh, int ds_hash_tmp,
                               TyKind ds_hash_type, int argsNode) {
  const NodeTable *nt = c->nt;
  int any = kwrest_any_key(c, m);
  const char *hk = any ? "PolyPoly" : "SymPoly";
  int krhash = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new();\n", hk, krhash, hk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", krhash);
  if (kwh >= 0) {
    int en3 = 0; const int *elems3 = nt_arr(nt, kwh, "elements", &en3);
    int splat_seen = 0, nsplat3 = 0, merged = kwh_merged(c, m, kwh);
    if (merged) {
      /* every source is already merged, in order, into the materialized
         hash, which takes any key where the rest does (emit_ds_hash_materialize) */
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_%sHash_update(_t%d, _t%d);\n", hk, krhash, ds_hash_tmp);
      splat_seen = 1;
    }
    for (int e3 = 0; e3 < en3 && !merged; e3++) {
      const char *ety3 = nt_type(nt, elems3[e3]);
      if (ety3 && sp_streq(ety3, "AssocSplatNode")) {
        nsplat3++;
        /* Forwarded `**hash`: merge its entries into the keyword-rest
           (later entries win, so order with literals is preserved). Only
           a symbol-keyed hash can flow into a keyword-rest parameter. */
        int inner3 = nt_ref(nt, elems3[e3], "value");
        if (inner3 < 0) {
          /* Anonymous `**`: merge the enclosing __anon_kwrest directly. */
          const char *akw = anon_kwrest_name(c, elems3[e3]);
          if (!akw) continue;
          splat_seen = 1;
          emit_indent(g_pre, g_indent);
          if (any && anon_kwrest_type(c, elems3[e3]) != TY_POLY_POLY_HASH)
            buf_printf(g_pre, "sp_kw_merge_any(_t%d, sp_box_obj(lv_%s, SP_BUILTIN_SYM_POLY_HASH));\n",
                       krhash, rename_local(akw));
          else buf_printf(g_pre, "sp_%sHash_update(_t%d, lv_%s);\n", hk, krhash, rename_local(akw));
          continue;
        }
        TyKind sty = comp_ntype(c, inner3);
        /* `**{}` carries no keywords */
        { int nsrc = -1;
          if (sty == TY_UNKNOWN && nt_kind(nt, inner3) == NK_HashNode) nt_arr(nt, inner3, "elements", &nsrc);
          if (nsrc == 0) { splat_seen = 1; continue; } }
        /* `**nil` carries no keywords, and a first operand of another
           class already raised where emit_ds_hash_materialize evaluated it */
        const char *bad3 = kw_splat_bad_cls(c, sty);
        if (sty == TY_NIL && nsplat3 > 1 && nt_kind(nt, inner3) != NK_NilNode) {
          /* a later operand answering nil still runs */
          Buf hb; memset(&hb, 0, sizeof hb);
          emit_expr(c, inner3, &hb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "(void)(%s);\n", hb.p ? hb.p : "0");
          free(hb.p);
        }
        if (bad3 && nsplat3 > 1) {
          /* a later operand of another class raises where it stands */
          emit_kw_splat_bad_operand(c, inner3);
        }
        if (sty == TY_NIL || bad3) continue;
        const char *shn = ty_hash_cname(sty);
        if (sty == TY_POLY || (shn && !sp_streq(shn, any ? "PolyPoly" : "SymPoly"))) {
          /* A Hash only known at run time (a value read out of a poly-valued
             hash), or of another kind: its entries are merged by a runtime
             walk, which into a rest of any key keeps every key. */
          Buf pb; memset(&pb, 0, sizeof pb);
          if (!splat_seen && ds_hash_tmp >= 0 && ds_hash_type == sty) {
            char tn[32]; snprintf(tn, sizeof tn, "_t%d", ds_hash_tmp);
            emit_boxed_text(c, sty, tn, &pb);
          }
          else emit_boxed(c, inner3, &pb);
          splat_seen = 1;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "%s(_t%d, %s);\n", any ? "sp_kw_merge_any" : "sp_kwrest_merge_poly",
                     krhash, pb.p ? pb.p : "sp_box_nil()");
          free(pb.p);
          continue;
        }
        if (!shn || !sp_streq(shn, any ? "PolyPoly" : "SymPoly")) {
          unsupported(c, argsNode, "double-splat forward of a non-symbol-keyed hash into a keyword-rest parameter");
          continue;
        }
        int src;
        if (nsplat3 == 1 && ds_hash_tmp >= 0) {
          /* Reuse the first splat's materialized temp -- only for the first
             splat itself: a `**nil` ahead of this one materialized an empty
             stand-in there, which is not this operand. It is declared with
             ds_hash_type's C type, so it must be SymPoly to flow into
             sp_SymPolyHash_update (the inner3 check above guarantees this
             for the matching first splat; assert it explicitly so the
             type-punned reuse can't silently emit a mismatched pointer). */
          const char *dshn = ty_hash_cname(ds_hash_type);
          if (!dshn || !sp_streq(dshn, hk)) {
            unsupported(c, argsNode, "double-splat forward of a non-symbol-keyed hash into a keyword-rest parameter");
            continue;
          }
          src = ds_hash_tmp;  /* first splat already materialized above */
        }
        else {
          src = ++g_tmp;
          /* Render into a side buffer first: a hash LITERAL (`**{ ... }`) drains
             its own construction into g_pre, which must land before -- not
             inside -- this temp's declaration line (see emit_ds_hash_materialize). */
          Buf hb; memset(&hb, 0, sizeof hb);
          emit_expr(c, inner3, &hb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sHash *_t%d = %s;\n", hk, src, hb.p ? hb.p : "");
          free(hb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", src);
        }
        splat_seen = 1;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_%sHash_update(_t%d, _t%d);\n", hk, krhash, src);
        continue;
      }
      int key3 = nt_ref(nt, elems3[e3], "key");
      int val3 = nt_ref(nt, elems3[e3], "value");
      if (key3 < 0 || val3 < 0) continue;
      const char *kty3 = nt_type(nt, key3);
      const char *kname3 = (kty3 && sp_streq(kty3, "SymbolNode")) ? nt_str(nt, key3, "value") : NULL;
      if (!kname3 && (any || kw_key_computed(nt, key3))) {
        /* a computed key: its pair joins the rest under the Symbol it
           answers, key first, as it runs; into a rest of any key, under
           whatever it answers, as a String or other literal key does */
        int kt = ++g_tmp;
        Buf kb, vb3c;
        memset(&kb, 0, sizeof kb);
        memset(&vb3c, 0, sizeof vb3c);
        emit_boxed(c, key3, &kb);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", kt, kb.p ? kb.p : "sp_box_nil()", kt);
        emit_boxed(c, val3, &vb3c);
        emit_indent(g_pre, g_indent);
        if (any) buf_printf(g_pre, "sp_PolyPolyHash_set(_t%d, _t%d, %s);\n",
                            krhash, kt, vb3c.p ? vb3c.p : "sp_box_nil()");
        else buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_poly_hkey_sym(_t%d), %s);\n",
                        krhash, kt, vb3c.p ? vb3c.p : "sp_box_nil()");
        free(kb.p); free(vb3c.p);
        /* a key it answers that a keyword takes leaves the rest below */
        if (kw_key_computed(nt, key3)) splat_seen = 1;
        continue;
      }
      if (!kname3) continue;
      /* A literal `k: v` whose name is an explicit keyword param is bound
         to that param, not the keyword-rest. A positional param of the
         same name does not consume it. */
      if (callee_has_kwarg(c, m, kname3)) continue;
      if (kwh_elem_dropped(nt, kwh, e3)) { emit_dropped_value(c, val3, g_pre); continue; }
      /* Render the boxed value into a side buffer first: an Array/Hash literal
         value drains its own construction (`_tN = ..._new(); push...`) into
         g_pre, which must land BEFORE -- not inside -- the set line (#3111). */
      Buf vb3; memset(&vb3, 0, sizeof vb3);
      emit_boxed(c, val3, &vb3);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, any ? "sp_PolyPolyHash_set(_t%d, sp_box_sym(sp_sym_intern(\"%s\")), %s);\n"
                            : "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), %s);\n",
                 krhash, kname3, vb3.p ? vb3.p : "sp_box_nil()");
      free(vb3.p);
    }
    /* Keys merged from a `**hash` that name an explicit keyword param are
       consumed by that param, so drop them from the keyword-rest. Only
       keyword params consume keys -- a positional param of the same name
       leaves its key in the rest. */
    if (splat_seen && m->def_node >= 0) {
      int dpn = nt_ref(nt, m->def_node, "parameters");
      int kpn = 0; const int *kwps = dpn >= 0 ? nt_arr(nt, dpn, "keywords", &kpn) : NULL;
      for (int kk = 0; kk < kpn; kk++) {
        const char *kpname = nt_str(nt, kwps[kk], "name");
        if (!kpname) continue;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, any ? "sp_PolyPolyHash_delete(_t%d, sp_box_sym(sp_sym_intern(\"%s\")));\n"
                              : "sp_SymPolyHash_delete(_t%d, sp_sym_intern(\"%s\"));\n",
                   krhash, kpname);
      }
    }
  }
  return krhash;
}


/* True if the subtree at `id` reads a local variable named `name`. */
int subtree_reads_local(const NodeTable *nt, int id, const char *name) {
  if (id < 0 || !name) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "LocalVariableReadNode")) {
    const char *n = nt_str(nt, id, "name");
    if (n && sp_streq(n, name)) return 1;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_reads_local(nt, nt_ref_at(nt, id, i), name)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_reads_local(nt, ids[j], name)) return 1;
  }
  return 0;
}

/* True if some parameter's default expression references an EARLIER parameter.
   Ruby evaluates defaults left-to-right in the callee where earlier params are
   already bound; spinel fills defaults at the call site, where those bindings
   are absent, so such a default needs the sibling-binding path below. */
int default_refs_earlier_param(Compiler *c, Scope *m) {
  const NodeTable *nt = c->nt;
  if (!m->pnames) return 0;
  for (int i = 1; i < m->nparams; i++) {
    if (!m->pdefault || m->pdefault[i] < 0) continue;
    for (int j = 0; j < i; j++)
      if (m->pnames[j] && subtree_reads_local(nt, m->pdefault[i], m->pnames[j]))
        return 1;
  }
  return 0;
}

static void emit_argument_error(const char *msg);
/* See codegen_internal.h. CRuby evaluates every argument before the callee
   refuses them or judges a keyword, and converts each `**` operand where it
   stands, so its TypeError comes ahead of the values after it and of any
   count: `def m` called `m("s" => 1, **true)` is no implicit conversion of
   true, and `m(**h, **1, **src)` never runs src. Each value runs into the
   temp its uses read (emit_args_in_source_order). */
void emit_args_run(Compiler *c, const int *argv, int argc) {
  int nv = 0; char *ds = NULL;
  int *vals = source_values(c->nt, argv, argc, &nv, &ds);
  for (int i = 0; i < nv; i++) {
    int v = vals[i];
    emit_arg_first(c, v, value_rebound(c, vals, nv, i, NULL, 0), g_pre);
    if (!ds[i]) continue;
    TyKind t = comp_ntype(c, v);
    if (t == TY_POLY) {
      /* converted into the temp the binding reads, which merges the Hash
         (or nil) it holds: converted apart, the binding converted the
         operand again and a #to_hash ran twice */
      if (!arg_ran_first(v, 0)) emit_arg_temp(c, v);
      for (int o = g_n_argov - 1; o >= 0; o--)
        if (g_argov_node[o] == v) { emit_kw_splat_conv_temp(c, g_argov_text[o]); break; }
    }
    else if (kw_splat_bad_cls(c, t)) emit_kw_splat_bad_operand(c, v);
  }
  free(vals); free(ds);
}
/* Inject a runtime ArgumentError ahead of the call statement: the raise
   fires exactly when the bad call would run (dead code stays silent, like
   CRuby), once the call's arguments `argv` have run, and the argument slots
   still fill with their compat pads below, reading what ran. */
static void args_raise(Compiler *c, const int *argv, int argc, const char *fmt, ...) {
  char msg[512];
  va_list ap; va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  emit_args_run(c, argv, argc);
  emit_argument_error(msg);
}
/* The raise of that ArgumentError, its arguments already run. */
static void emit_argument_error(const char *msg) {
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "sp_raise_cls(\"ArgumentError\", ");
  emit_c_str(g_pre, msg);
  buf_puts(g_pre, ");\n");
}

/* The keyword decisions of a call into `m`, taken once: every binder read
   them off the hash on its own, each by a different subset of the rules, so
   a `**` beside the positionals was counted by one and not another, a String
   key was never checked, and a rest or a `**kwrest` beside a required
   keyword stood the missing-keyword check down.

   What the hash is comes first. A callee that declares a keyword or a
   `**kwrest` takes it as keywords, whatever its keys and whatever
   its `**` operands hold: it never counts as a positional, so `def r(a, k:)`
   called `r(**{k: 5})` is short a positional. A `**nil` callee refuses it.
   Any other callee takes it as one more positional Hash, which counts one
   when a literal key is in it and, made of `**` spreads alone, one only when
   the run time finds a key in it (`f(1, **{})` is `f(1)`).

   Then the keywords the call gets wrong in a way known here, for a callee
   declaring keywords: each required keyword no literal key names, when no
   `**` may name it (the run time checks those that may, emit_ds_kwarg_check),
   and, without a `**kwrest`, each literal key no keyword parameter takes --
   a String one as much as a Symbol: `def f(a, k: 0)` called `f(1, "s" => 1)`
   is `unknown keyword: "s"`. A key written twice is named once, where it
   first stands; a computed key is the run time's (`computed`). */
void kw_plan(Compiler *c, Scope *m, int kwh, KwPlan *P) {
  const NodeTable *nt = c->nt;
  memset(P, 0, sizeof *P);
  P->kwh = kwh;
  if (!m) return;
  int en = 0; const int *el = kwh >= 0 ? nt_arr(nt, kwh, "elements", &en) : NULL;
  for (int e = 0; e < en; e++) {
    if (nt_kind(nt, el[e]) == NK_AssocSplatNode) {
      P->spread = 1;
      /* a `**` whose conversion may refuse it: `**true`, or a value only the
         run time knows */
      int v = nt_ref(nt, el[e], "value");
      TyKind t = v >= 0 ? comp_ntype(c, v) : TY_UNKNOWN;
      if (v >= 0 && (t == TY_POLY || kw_splat_bad_cls(c, t))) P->args_first = 1;
      continue;
    }
    P->literal = 1;
    int key = nt_ref(nt, el[e], "key");
    if (key >= 0 && nt_kind(nt, key) != NK_SymbolNode && nt_kind(nt, key) != NK_StringNode) P->computed = 1;
  }
  int pn = m->def_node >= 0 ? nt_ref(nt, m->def_node, "parameters") : -1;
  int declares = callee_declares_kwargs(c, m);
  /* CRuby runs every argument before it converts a `**` or judges a
     keyword: when the run time does either, the arguments run first, in
     source order, and the check reads what they left (emit_ds_kwarg_check
     ran ahead of the positionals and the literal values after the `**`) */
  if (P->spread && declares) P->args_first = 1;
  if (kwh < 0) P->role = KWH_NONE;
  else if (scope_refuses_keywords(c, m)) P->role = KWH_REFUSED;
  else if (declares || m->kwrest_idx >= 0) P->role = KWH_KEYWORDS;
  else {
    P->role = KWH_POSITIONAL;
    P->count = P->literal ? KWC_ONE : KWC_NONEMPTY;
  }
  if (!declares) return;
  int kn = 0; const int *kws = nt_arr(nt, pn, "keywords", &kn);
  /* a `**` or a computed key may name any keyword: which are missing is
     the run time's (emit_ds_kwarg_check, on the hash they merge into) */
  for (int i = 0; i < kn && !P->spread && !P->computed; i++) {
    const char *kty = nt_type(nt, kws[i]);
    const char *kpn = nt_str(nt, kws[i], "name");
    if (!kty || !sp_streq(kty, "RequiredKeywordParameterNode") || !kpn || kwh_lookup(nt, kwh, kpn) >= 0) continue;
    char iv[300];
    kw_key_inspect(kpn, 1, iv, sizeof iv);
    kw_names_add(P->missing, sizeof P->missing, &P->nmissing, iv);
  }
  if (m->kwrest_idx >= 0) return;
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, el[e], "key");
    int is_sym = key >= 0 && nt_kind(nt, key) == NK_SymbolNode;
    int is_str = key >= 0 && nt_kind(nt, key) == NK_StringNode;
    const char *kn = is_sym ? nt_str(nt, key, "value") : is_str ? nt_str(nt, key, "content") : NULL;
    if (!kn) continue;
    /* a key names a keyword parameter or nothing: a positional parameter of
       the same name does not take it (`def f(x, k: 1)` called `f(1, x: 2)`
       is "unknown keyword: :x") */
    if (is_sym && callee_param_is_declared_kwarg(c, m, kn)) continue;
    int dup = 0;
    for (int e2 = 0; e2 < e && !dup; e2++) {
      int k2 = nt_ref(nt, el[e2], "key");
      const char *n2 = k2 >= 0 && nt_kind(nt, k2) == nt_kind(nt, key)
                         ? nt_str(nt, k2, is_sym ? "value" : "content") : NULL;
      dup = n2 && sp_streq(n2, kn);
    }
    if (dup) continue;
    char iv[300];
    kw_key_inspect(kn, is_sym, iv, sizeof iv);
    if (is_sym && !P->first_sym[0]) snprintf(P->first_sym, sizeof P->first_sym, "%s", iv);
    if (P->nunknown < 32) P->unknown_el[P->nunknown] = e;
    kw_names_add(P->unknown, sizeof P->unknown, &P->nunknown, iv);
  }
}

/* The keyword error a plan finds here, in CRuby's order -- a missing
   keyword ahead of an unknown one -- into `msg`; 0 when it finds none. A
   literal key no keyword takes beside a `**` is the run time's to name, with
   the keys the `**` brings (emit_ds_kwarg_check). */
int kw_plan_error(const KwPlan *P, char *msg, size_t n) {
  if (P->nmissing) {
    kw_error_message(msg, n, "missing", P->nmissing, P->missing);
    return 1;
  }
  if (!P->nunknown || P->spread) return 0;
  if (!P->computed) kw_error_message(msg, n, "unknown", P->nunknown, P->unknown);
  else if (P->first_sym[0]) kw_error_message(msg, n, "unknown", 1, P->first_sym);
  else return 0;
  return 1;
}

/* The `unknown keyword` raise for a call's keyword hash, the last of its
   arguments `argv`, into `m`, once those have run: the plan's (kw_plan). A
   call that is INLINED walks the parameters looking for keys rather than the
   keys looking for parameters, so a key nobody claimed was never read: the
   DNS-rebinding pin `Net::HTTP.start(..., ipaddr: ip)` carries went out with
   the keyword silently gone (#4419). CRuby names every such key once, in the
   order the hash first holds it; beside a key only the run time knows, the
   first unknown Symbol alone is named here. Returns 1 when it raised. */
int emit_unknown_kwarg_raise(Compiler *c, Scope *m, const int *argv, int argc) {
  const NodeTable *nt = c->nt;
  int kwh = argc > 0 && argv && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  KwPlan P;
  kw_plan(c, m, kwh, &P);
  P.nmissing = 0;
  char msg[600];
  if (!kw_plan_error(&P, msg, sizeof msg)) return 0;
  args_raise(c, argv, argc, "%s", msg);
  return 1;
}

/* The positional count a rest-parameter method requires, when a call that
   supplies fewer is judged: the parameters without a default, the rest and
   any keyword left out. -1 when it is not judged -- no rest, a keyword or
   keyword-rest parameter (those bind by other rules), a synthesized
   parameter or scope. One rule for the raise in emit_args_filled and for a
   dispatch that lists candidates by what the call's count allows, so a
   candidate the raise would refuse is never emitted as an arm (#3520). */
int rest_shortfall_required(Compiler *c, Scope *m) {
  if (m->rest_idx < 0 || m->kwrest_idx >= 0 || m->cs_synth) return -1;
  int nreq = 0;
  for (int i = 0; i < m->nparams; i++) {
    if (i == m->rest_idx) continue;
    if (!m->pnames[i] || (m->pnames[i][0] == '_' && m->pnames[i][1] == '_') ||
        callee_has_kwarg(c, m, m->pnames[i])) return -1;
    if (!m->pdefault || m->pdefault[i] < 0) nreq++;
  }
  return nreq;
}

/* The positional count a rest target requires of a call, or -1 when it is
   not judged: rest_shortfall_required, and for a callee taking keywords,
   whose hash is never a positional (kw_plan), its positionals without a
   default -- where the keyword parameters stood the check down. */
static int rest_required_for(Compiler *c, Scope *m) {
  int r = rest_shortfall_required(c, m);
  if (r >= 0 || m->rest_idx < 0 || m->cs_synth) return r;
  if (!callee_declares_kwargs(c, m) && m->kwrest_idx < 0) return -1;
  for (int i = 0; i < m->nparams; i++)
    if (!m->pnames[i] || (m->pnames[i][0] == '_' && m->pnames[i][1] == '_')) return -1;
  int req = 0, tot = 0;
  positional_arity(c, m, &req, &tot);
  return req;
}

/* The count check of a call whose positional count only the run time knows
   (a splat, a gather), the C expression `given`, against `m`'s positional
   parameters -- keywords apart, and named in the message as CRuby names
   them. */
static int bam_variadic_kernel(const NodeTable *nt, const Scope *m);
static void emit_positional_count_check(Compiler *c, Scope *m, const char *given) {
  int pos_required = 0, pos_params = 0;
  positional_arity(c, m, &pos_required, &pos_params);
  char kw[256];
  scope_arity_kw_suffix(c, m, kw, sizeof kw);
  emit_indent(g_pre, g_indent);
  /* no upper bound where the static count has none: a rest, or a Kernel
     wrapper whose builtin takes any count (bam_variadic_kernel) */
  int unbounded = m && (m->rest_idx >= 0 || bam_variadic_kernel(c->nt, m));
  emit_arity_check(g_pre, given, pos_required, unbounded ? -1 : pos_params, kw);
  buf_puts(g_pre, ";\n");
}

/* True when `m` is a synthesized receiverless Kernel wrapper
   (`method(:puts)`) whose builtin takes a variable or optional count. The
   wrapper declares as many parameters as its call sites agree on (one when
   they do not: analyze.c, bam_call_argc), so it cannot express the
   builtin's real arity; a call with more arguments than that is truncated
   rather than raising ArgumentError, matching the behavior before #4395;
   the UB fix (a zero-argument call reads an undefined register) still raises
   on the shortfall. A receiver-bound wrapper is excluded -- its __bam_r is
   the receiver, not an argument. */
static int bam_variadic_kernel(const NodeTable *nt, const Scope *m) {
  if (!m->name || strncmp(m->name, "__bam_", 6) != 0 || m->body < 0) return 0;
  int bn = 0;
  const int *bb = nt_arr(nt, m->body, "body", &bn);
  if (bn < 1 || !bb || !nt_type(nt, bb[0]) || !sp_streq(nt_type(nt, bb[0]), "CallNode")) return 0;
  if (nt_ref(nt, bb[0], "receiver") >= 0) return 0;   /* receiver-bound wrapper */
  const char *nm = nt_str(nt, bb[0], "name");
  if (!nm) return 0;
  return sp_streq(nm, "puts") || sp_streq(nm, "print") || sp_streq(nm, "p") ||
         sp_streq(nm, "pp") || sp_streq(nm, "Rational") || sp_streq(nm, "Complex");
}

/* Does `m` take parameters that are not the call's arguments one for one,
   so no count of them can be judged: a synthesized scope, a `(...)`
   forwarder (it takes whatever its call passes on, through the parameters
   synthesized for its call sites), or any other synthesized (__-prefixed)
   parameter. A __bam_ wrapper's parameters are real call arguments here: a
   receiverless Kernel wrapper (`method(:String)`) has one, and only it
   reaches the binders that ask -- a receiver-bound wrapper's Method call
   goes through the object-bound path, whose self slot carries param[0], and
   the dispatch stands its own check down for it. Counting a receiverless
   wrapper's parameter as compiler plumbing skipped the arity check, so
   `method(:String).call` invoked it with a filled-in 0 instead of raising
   ArgumentError. One rule for the static count (emit_call_arity_check) and
   the counts a splat leaves to the run time (emit_splat_given_count,
   emit_unreached_splat_count), which judged such a scope's synthesized
   slots as if they were arguments. */
static int arity_unjudged(Compiler *c, Scope *m) {
  if (m->cs_synth) return 1;
  for (int i = 0; i < m->nparams; i++)
    if (m->pnames[i] && m->pnames[i][0] == '_' && m->pnames[i][1] == '_' &&
        strncmp(m->pnames[i], "__bam_", 6) != 0) return 1;
  int pn = m->def_node >= 0 ? nt_ref(c->nt, m->def_node, "parameters") : -1;
  int kwr = pn >= 0 ? nt_ref(c->nt, pn, "keyword_rest") : -1;
  return kwr >= 0 && nt_type(c->nt, kwr) && sp_streq(nt_type(c->nt, kwr), "ForwardingParameterNode");
}

int splat_operand_is_scalar(TyKind t) {
  return t == TY_NIL || t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_STRING ||
         t == TY_STRBUF || t == TY_SYMBOL || t == TY_BOOL;
}

/* The positional count of a call with a splat among its positionals, as
   the run time measures it, into `b`: each splat's length, one for each
   other positional, and the one a keyword hash adds that is a positional
   Hash with a literal key (kw_plan). */
static void emit_rt_positional_count(Compiler *c, const int *argv, int pos_argc, const KwPlan *P,
                                     Buf *b) {
  const NodeTable *nt = c->nt;
  int fixed = P->count == KWC_ONE;
  buf_puts(b, "(");
  for (int k = 0; argv && k < pos_argc; k++) {
    if (nt_kind(nt, argv[k]) == NK_BlockArgumentNode) continue;
    if (nt_kind(nt, argv[k]) != NK_SplatNode) { fixed++; continue; }
    int op = nt_ref(nt, argv[k], "expression");
    if (op < 0) {
      Buf ab; memset(&ab, 0, sizeof ab);
      if (emit_anon_rest_ref(c, argv[k], &ab)) buf_printf(b, "sp_PolyArray_length(%s) + ", ab.p);
      free(ab.p);
      continue;
    }
    buf_puts(b, "sp_PolyArray_length(");
    emit_splat_operand_array(c, op, b);
    buf_puts(b, ") + ");
  }
  buf_printf(b, "%d)", fixed);
}

/* A splat past every parameter of a callee without a rest binds nowhere,
   and no binder measured it: `def m(a)` called `m(1, *x)` ran with x's
   elements dropped, and `def n` called `n(*[], k: 1)` with the positional
   Hash too. Its count is judged here, the keyword hash counted as the plan
   says, once the arguments have run. */
void emit_unreached_splat_count(Compiler *c, Scope *m, const int *argv, int argc, int pos_argc,
                                const KwPlan *P) {
  if (arity_unjudged(c, m)) return;
  /* arguments the plan already ran first are not run again: a key or value
     that found no override slot free would repeat its effect */
  if (!P->args_first) emit_args_in_source_order(c, argv, argc, g_pre);
  Buf gb; memset(&gb, 0, sizeof gb);
  emit_rt_positional_count(c, argv, pos_argc, P, &gb);
  emit_positional_count_check(c, m, gb.p);
  free(gb.p);
}

/* Arity / keyword validation, in CRuby's words, raised at RUNTIME just
   before the call would run (dead code stays silent, matching CRuby;
   the argument slots keep their compat pads), once every argument has run.
   CRuby's order: a `**nil` callee refuses keywords, then the positional
   count, then a missing keyword, then an unknown one. What the keyword hash
   is, what it adds to the count and which keywords the call gets wrong are
   the plan's (kw_plan). The count is judged here when it is known here: no
   splat among the positionals, and a hash that counts nothing or one; a
   synthesized scope or synthesized (__-prefixed, e.g. forwarding) params
   skip. A rest target has only its shortfall judged. A keyword error the plan finds after a splat is raised
   once the run time has judged the count, which CRuby judges first:
   `def m(k:)` called `m(*[])` is missing k, called `m(*[1])` given 1.
   One rule for the direct call (emit_args_filled), the instance dispatch
   (emit_dispatch) and the inlined yield path (emit_inline_call_x): the
   dispatch kept its own count, which took keyword parameters for positional
   slots and skipped every keyword hash, so `obj.m(3, 4)` against
   `def m(x, k: 1)` bound silently where `m(3, 4)` raised. */
void emit_call_arity_check(Compiler *c, Scope *m, int argc, const int *argv) {
  const NodeTable *nt = c->nt;
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && argv && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  int has_splat = 0;
  for (int k = 0; k < pos_argc && !has_splat; k++)
    if (argv && nt_kind(nt, argv[k]) == NK_SplatNode) has_splat = 1;
  KwPlan P;
  kw_plan(c, m, kwh, &P);
  /* A `**nil` method refuses a keyword before it counts anything: a literal
     key here, `**` spreads when the run time finds a key in them. Either way
     the hash binds nowhere (kwh_consumed_by_kwparam). */
  if (P.role == KWH_REFUSED) {
    if (P.literal) { args_raise(c, argv, argc, "no keywords accepted"); return; }
    Buf kb; memset(&kb, 0, sizeof kb);
    emit_args_in_source_order(c, argv, argc, g_pre);
    if (emit_kwh_spread_arg(c, kwh, &kb)) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "if (sp_poly_length(%s) > 0) sp_raise_cls(\"ArgumentError\", \"no keywords accepted\");\n",
                 kb.p);
    }
    free(kb.p);
  }
  if (arity_unjudged(c, m)) return;
  int nfixed = 0, nreq = 0;
  for (int i = 0; i < m->nparams; i++) {
    /* Keyword parameters share pnames[] with the positional ones but are
       no positional slot: counting them let `def m(x, k: 1)` take `m(3, 4)`
       and bind the 4 nowhere. The rest is no slot either. */
    if (i == m->kwrest_idx || i == m->rest_idx || callee_param_is_declared_kwarg(c, m, m->pnames[i])) continue;
    nfixed++;
    if (!m->pdefault || m->pdefault[i] < 0) nreq++;
  }
  /* CRuby names the callee's required keywords in every count error:
     `def h(x, k:)` called `h(k: 3)` is "(given 0, expected 1; required
     keyword: k)" */
  char kwsuf[256], msg[512];
  scope_arity_kw_suffix(c, m, kwsuf, sizeof kwsuf);
  if (!has_splat && P.count != KWC_NONEMPTY) {
    /* A keyword hash that is keywords is no positional, `**` spreads among
       them too: `def r(a, k:)` called `r(**{k: 5})` is given 0. With a rest
       there is no upper bound: `def f(a, *r)` called bare ran the body with
       a padded a. */
    int given = pos_argc + P.count;
    int over = m->rest_idx < 0 && given > nfixed && !bam_variadic_kernel(nt, m);
    int under = given < nreq;
    if (over || under) {
      arity_message(msg, sizeof msg, given, nreq, m->rest_idx >= 0 ? -1 : nfixed, kwsuf);
      args_raise(c, argv, argc, "%s", msg);
      return;
    }
  }
  if (!kw_plan_error(&P, msg, sizeof msg)) return;
  if (has_splat) {
    /* the count first, as the run time measures it, once the arguments
       have run */
    emit_args_run(c, argv, argc);
    Buf gb; memset(&gb, 0, sizeof gb);
    emit_rt_positional_count(c, argv, pos_argc, &P, &gb);
    emit_positional_count_check(c, m, gb.p);
    free(gb.p);
    emit_argument_error(msg);
    return;
  }
  args_raise(c, argv, argc, "%s", msg);
}

/* The positional layout of one call into one callee: which argument, splat
   element or gathered element each positional parameter takes, decided once
   and read by every binder that walks the parameters (emit_args_filled,
   emit_dispatch, the inlined yielding method and initialize). Each of them
   decided it on its own, and a fix to one left the others binding the old
   way. Keyword parameters bind by name and are the binders' own business;
   the layout only marks them.

   The layout is static when the count ahead of every positional parameter
   is known here: the arguments fund the parameters as arg_slot_for_param
   says, the posts from the end (rest_bind_argc), and a trailing splat
   spreads in place, element by element, over the parameters from its own
   index. It gathers when that count is the run time's (arg_layout_gathers):
   every positional argument goes into one array (emit_splat_gather) and
   each parameter binds from it by the count the array holds. */

/* Can the gather carry the keyword hash as its last positional? A literal
   one into a callee that takes no keywords is always one more argument, and
   after a splat the parameter it lands in is the run time's, so each one it
   may reach (gather_reaches, which inference types by) has to be able to
   hold it: a Hash or a boxed slot. The rest always can. Where one of them
   cannot, the call keeps the static layout it had. Asking every parameter
   from the first splat argument's index on missed a post funded from the
   end: `m(1, 1, *v, k: 3)` on `def m(*r, q)` bound the hash into an Integer
   q. */
static int kwh_rides_gather(Compiler *c, Scope *m, int kwh, const int *argv, int pos_argc) {
  const NodeTable *nt = c->nt;
  if (kwh < 0 || kwh_only_spreads(nt, kwh) || kwh_consumed_by_kwparam(c, m, kwh)) return 0;
  int fk = -1;
  for (int k = 0; k < pos_argc && fk < 0; k++)
    if (nt_kind(nt, argv[k]) == NK_SplatNode) fk = k;
  if (fk < 0) return 0;   /* the static layout places it (kwh_positional_slot) */
  for (int i = 0; i < m->nparams && !layout_untyped; i++) {
    if (i == m->rest_idx || !gather_reaches(c, m, argv, pos_argc, 2, pos_argc, i)) continue;
    LocalVar *p = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
    TyKind pt = p ? p->type : TY_UNKNOWN;
    if (!ty_is_hash(pt) && pt != TY_POLY) return 0;
  }
  return 1;
}

/* Does the call bind by a count only the run time knows? A splat with a
   positional argument after it (`f(*a, 3)`, or `f(*a, k: 1)` whose hash is
   one more positional) ahead of a positional parameter: which parameter the
   3 fills depends on a's length. The static layout assumed the splat filled
   exactly the gap, so `m(*[], 1)` on `def m(a, *r)` bound a nil and r [1],
   and `f(*[1, 2], 3)` on `def f(a, b)` raised nothing. A trailing splat
   gathers too where the layout funds a parameter from the end -- a rest's
   posts, the requireds after a leading optional -- or a default reads an
   earlier parameter, and a `**` hash that may be no argument (kwh_gathers)
   always does. An inlined call has no in-place spread, so any splat gathers
   there. Not for a synthesized parameter list, nor a keyword hash the gather
   cannot carry (kwh_rides_gather) and no keyword parameter takes. A rest or a
   `**kwrest` beside them no longer keeps the static layout: the gather
   funds a rest (emit_gathered_param), and a `**kwrest` binds by name. */
static int arg_layout_gathers(Compiler *c, Scope *m, const int *argv, int pos_argc, int kwh,
                              int inlined) {
  const NodeTable *nt = c->nt;
  if (!m || !argv || m->cs_synth) return 0;
  for (int i = 0; i < m->nparams; i++)
    if (i != m->rest_idx && i != m->kwrest_idx &&
        (!m->pnames[i] || (m->pnames[i][0] == '_' && m->pnames[i][1] == '_'))) return 0;
  if (kwh_gathers(c, m, kwh, argv, pos_argc)) return 1;
  int rides = kwh_rides_gather(c, m, kwh, argv, pos_argc);
  int lead = m->rest_idx >= 0 ? m->rest_idx : m->nparams;
  int nspl = 0, sk = -1, dyn = 0;
  for (int k = 0; k < pos_argc; k++) {
    if (nt_kind(nt, argv[k]) != NK_SplatNode) continue;
    nspl++; sk = k;
    /* an argument after it: a later positional, or the hash as one */
    int after = rides;
    for (int j = k + 1; j < pos_argc && !after; j++)
      if (nt_kind(nt, argv[j]) != NK_BlockArgumentNode) after = 1;
    if (after && k < lead) dyn = 1;
  }
  if (nspl == 0) return 0;
  if (!dyn && !inlined && !opt_before_required(c, m) && !default_refs_earlier_param(c, m) &&
      !(m->rest_idx >= 0 && m->npost_rest > 0 && sk >= pos_argc - m->npost_rest))
    return 0;
  /* a literal hash is keywords, or the gather carries it */
  if (kwh >= 0 && !rides && !kwh_consumed_by_kwparam(c, m, kwh)) return 0;
  return 1;
}

/* Does a splat have an array form to spread in place: an array, a boxed
   operand the splat's own lowering normalizes, a nil or scalar one, or an
   anonymous `*`? Inference, which types the parameters from the operand's
   elements, takes any. */
static int splat_spreads_in_place(Compiler *c, int splat) {
  int inner = nt_ref(c->nt, splat, "expression");
  if (layout_untyped) return 1;
  if (inner < 0) {
    Buf anon; memset(&anon, 0, sizeof anon);
    int ok = emit_anon_rest_ref(c, splat, &anon);
    free(anon.p);
    return ok;
  }
  TyKind at = comp_ntype(c, inner);
  return at == TY_POLY || at == TY_UNKNOWN || splat_operand_is_scalar(at) || ty_is_array(at) ||
         at == TY_POLY_ARRAY;
}

/* `argv` may be NULL for `pos_argc` plain arguments that are no nodes (a
   bare `super`'s forwarded parameters): ARG_NODE then names their index. */
void arg_layout(Compiler *c, Scope *m, const int *argv, int pos_argc, int kwh, int inlined,
                ArgLayout *L) {
  const NodeTable *nt = c->nt;
  memset(L, 0, sizeof *L);
  L->kwh = kwh; L->pos_argc = pos_argc; L->splat = -1;
  L->n = m ? m->nparams : 0;
  L->from = L->n ? calloc((size_t)L->n, sizeof *L->from) : NULL;
  L->arg = L->n ? calloc((size_t)L->n, sizeof *L->arg) : NULL;
  if (!m) return;
  L->kwh_slot = kwh_positional_slot(c, m, kwh, pos_argc);
  L->bind_argc = pos_argc + (L->kwh_slot >= 0 ? 1 : 0);
  L->rest_argc = rest_bind_argc(c, m, kwh, pos_argc);
  L->rest_kwh = rest_kwh_tail(c, m, kwh, pos_argc);
  kw_plan(c, m, kwh, &L->kw);
  /* a splat leaves the count to the run time, which judges it before the
     keywords, their values run */
  for (int k = 0; kwh >= 0 && argv && k < pos_argc; k++)
    if (nt_kind(nt, argv[k]) == NK_SplatNode) L->kw.args_first = 1;
  /* ... which matters only where another argument than a lone `**` operand
     has an effect to run out of place, and not for a hash merged in source
     order (kwh_merged), whose positionals already run first
     (kwh_runs_ahead) and whose sources merge converting each in turn */
  if (L->kw.args_first && kwh_merged(c, m, kwh)) L->kw.args_first = 0;
  if (L->kw.args_first) {
    int neff = 0, spread_eff = 0;
    for (int k = 0; argv && k < pos_argc; k++) {
      int v = nt_kind(nt, argv[k]) == NK_SplatNode ? nt_ref(nt, argv[k], "expression") : argv[k];
      if (v >= 0 && nt_kind(nt, v) != NK_BlockArgumentNode && subtree_has_side_effect(c, v)) neff++;
    }
    int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
    for (int e = 0; e < en; e++) {
      int v = nt_ref(nt, el[e], "value"), key = nt_ref(nt, el[e], "key");
      if (key >= 0 && subtree_has_side_effect(c, key)) neff++;
      if (v < 0 || !subtree_has_side_effect(c, v)) continue;
      neff++;
      if (nt_kind(nt, el[e]) == NK_AssocSplatNode) spread_eff++;
    }
    if (neff == 0 || (neff == 1 && spread_eff == 1)) L->kw.args_first = 0;
  }
  L->gather = arg_layout_gathers(c, m, argv, pos_argc, kwh, inlined);
  if (L->gather) {
    if (kwh_gathers(c, m, kwh, argv, pos_argc)) L->gather_kwh = 1;
    else if (kwh_rides_gather(c, m, kwh, argv, pos_argc)) L->gather_kwh = 2;
  }
  /* A plain gather funds the parameters from its front, element i for
     parameter i, as a trailing splat spreads; one that counts from the end
     (a rest's posts, the requireds after a leading optional) or may hold a
     `**` that is nothing binds each through emit_gathered_param. */
  int counted = inlined || L->gather_kwh == 1 || opt_before_required(c, m) || m->rest_idx >= 0;
  /* the splat a static layout spreads in place: the first, when it reaches a
     positional parameter. An inlined call has no in-place spread: a splat it
     cannot gather binds as written. */
  if (!L->gather && !inlined)
    for (int k = 0; argv && k < pos_argc; k++) {
      if (nt_kind(nt, argv[k]) != NK_SplatNode) continue;
      int reaches = m->rest_idx >= 0 ? k < m->rest_idx : k < m->nparams;
      if (reaches && splat_spreads_in_place(c, argv[k])) L->splat = k;
      break;
    }
  int ntrail = L->splat >= 0 ? pos_argc - L->splat - 1 : 0;
  int trail_from = m->nparams - ntrail;
  for (int i = 0; i < m->nparams; i++) {
    const char *pn = m->pnames[i];
    int declkw = pn && callee_param_is_declared_kwarg(c, m, pn);
    int kwp = pn && callee_has_kwarg(c, m, pn);
    int is_post = m->rest_idx >= 0 && i > m->rest_idx && i <= m->rest_idx + m->npost_rest;
    L->arg[i] = -1;
    if (i == m->kwrest_idx || declkw) { L->from[i] = ARG_BY_NAME; continue; }
    if (L->gather) {
      L->from[i] = counted ? ARG_GATHERED : ARG_ELEM;
      L->arg[i] = i;
      continue;
    }
    if (i == m->rest_idx) { L->from[i] = ARG_REST; continue; }
    if (is_post) {
      int aidx = L->rest_argc - m->npost_rest + (i - m->rest_idx - 1);
      if (aidx >= 0 && aidx < L->rest_argc) { L->from[i] = ARG_NODE; L->arg[i] = aidx; }
      else L->from[i] = ARG_DEFAULT;
      continue;
    }
    /* anything past the rest that is not a post binds by name */
    if (m->rest_idx >= 0 && i > m->rest_idx) { L->from[i] = ARG_BY_NAME; continue; }
    if (L->splat >= 0 && i >= L->splat && !kwp) {
      /* a positional after a mid-list splat the layout could not gather
         (`g(1, *m, 4)`) takes the tail parameters */
      if (ntrail > 0 && i >= trail_from && trail_from > L->splat) {
        int aidx = L->splat + 1 + (i - trail_from);
        if (aidx < pos_argc) { L->from[i] = ARG_NODE; L->arg[i] = aidx; }
        else L->from[i] = ARG_DEFAULT;
      }
      else { L->from[i] = ARG_ELEM; L->arg[i] = i - L->splat; }
      continue;
    }
    int slot = arg_slot_for_param(c, m, i, L->bind_argc);
    /* ahead of a rest, only the arguments before the posts */
    int lim = m->rest_idx >= 0 ? L->rest_argc - m->npost_rest : pos_argc;
    if (slot >= 0 && slot < lim) { L->from[i] = ARG_NODE; L->arg[i] = slot; }
    else if (i == L->kwh_slot && !kwp) L->from[i] = ARG_KWH;
    else L->from[i] = ARG_DEFAULT;
  }
}

/* The layout inference types a call's parameters from. It is asked before
   they have the types it gives them, so it asks none: a parameter the
   keyword hash may bind as a positional takes it (kwh_positional_slot), the
   gather carries it (kwh_rides_gather), and each splat spreads in place.
   Codegen then finds each such parameter able to hold what reaches it. The
   inlined layout differs only in gathering more, which moves no value to
   another parameter. */
void arg_layout_untyped(Compiler *c, Scope *m, const int *argv, int pos_argc, int kwh,
                        ArgLayout *L) {
  layout_untyped = 1;
  arg_layout(c, m, argv, pos_argc, kwh, 0, L);
  layout_untyped = 0;
}

void arg_layout_free(ArgLayout *L) {
  free(L->from); free(L->arg);
  L->from = NULL; L->arg = NULL;
}

/* The argument parameter i takes among `pos_argc` plain positional ones, by
   the layout (arg_layout), or -1 when none reaches it: what inference reads
   to type a parameter from a bare `super`'s forwarded positionals. */
int arg_layout_plain_arg(Compiler *c, Scope *m, int pos_argc, int i) {
  ArgLayout L;
  arg_layout(c, m, NULL, pos_argc, -1, 0, &L);
  int a = i >= 0 && i < L.n && L.from[i] == ARG_NODE ? L.arg[i] : -1;
  arg_layout_free(&L);
  return a;
}

/* Does parameter i of m take the argument written at index i, ahead of the
   first splat, however many values the splats hold? A required leading
   positional always does. An optional one does when the arguments written
   outside the splats cover every required positional, leading and after
   the rest, and the optionals up to i: the run time fills the required
   ones first, so with fewer a short splat moves the argument onto a later
   parameter (`def g(a = nil, *r, z)` called `g(s, *e)` binds s to z when e
   is empty). `argv` holds all `argc` arguments, the keyword hash included. */
int gather_lead_placed(Compiler *c, Scope *m, const int *argv, int argc, int i) {
  const NodeTable *nt = c->nt;
  int kwh = argc > 0 && argv && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  int pos_argc = kwh >= 0 ? argc - 1 : argc;
  if (!argv || i < 0 || i >= pos_argc || i >= m->nparams) return 0;
  int nopt = 0;
  for (int k = 0; k <= i; k++) {
    NodeKind ak = nt_kind(nt, argv[k]);
    if (ak == NK_SplatNode || ak == NK_BlockArgumentNode || !m->pnames[k] ||
        k == m->rest_idx || k == m->kwrest_idx || callee_param_is_declared_kwarg(c, m, m->pnames[k])) return 0;
    if (nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "ForwardingArgumentsNode")) return 0;
    if (m->pdefault && m->pdefault[k] >= 0) nopt++;
  }
  if (!nopt) return 1;
  int written = 0, nreq = 0;
  for (int k = 0; k < pos_argc; k++) {
    NodeKind ak = nt_kind(nt, argv[k]);
    if (ak != NK_SplatNode && ak != NK_BlockArgumentNode) written++;
  }
  for (int k = 0; k < m->nparams; k++)
    if (m->pnames[k] && k != m->rest_idx && k != m->kwrest_idx && !(m->pdefault && m->pdefault[k] >= 0) &&
        !callee_param_is_declared_kwarg(c, m, m->pnames[k])) nreq++;
  return written - nreq >= nopt;
}

/* The argument parameter i of m takes out of a gather whatever the splats
   hold, or -1: the gather keeps the arguments in order, and the leading
   positionals take its first elements, so one ahead of the first splat
   that the splats cannot move (gather_lead_placed) is the argument written
   at its index. The binders fund it from the gather all the same; what
   lends it (emit_gather_lead_lent) and the analysis that follows a String
   into it (arg_layout_param_node) read it here, and both only for an argument
   nothing after it can give another value: the gather ran every argument,
   and the call fills the defaults, before the callee reads what was lent,
   so `grow(s, *xs, (s = +"q"; 9))` would bind the new String where CRuby
   binds the one read first -- lent the slot, or, followed by the analysis,
   the local's shared handle the assignment overwrites. A local changes by
   an assignment written after it (a block's or a branch's too), or, when a
   proc captures it, by any later effect, which may call one; an ivar by
   any later effect (read_rebound_by for the rest). `argv` holds all `argc`
   arguments, the keyword hash included. */
static int gather_lead_arg(Compiler *c, Scope *m, const int *argv, int argc, int i) {
  const NodeTable *nt = c->nt;
  int kwh = argc > 0 && argv && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  if (!gather_lead_placed(c, m, argv, argc, i)) return -1;
  int x = argv[i];
  NodeKind xk = nt_kind(nt, x);
  const char *vn = xk == NK_LocalVariableReadNode ? nt_str(nt, x, "name") : NULL;
  LocalVar *lv = vn ? scope_local(comp_scope_of(c, x), vn) : NULL;
  int nd = 0, *dfl = call_site_defaults(c, m, kwh, &nd), rebound = 0;
  for (int j = i + 1; j < argc + nd && !rebound; j++) {
    int after = j < argc ? argv[j] : dfl[j - argc];
    if (vn) rebound = subtree_writes_local(c, after, vn) ||
                      (lv && lv->is_cell && subtree_has_side_effect(c, after));
    else if (xk == NK_InstanceVariableReadNode) rebound = subtree_has_side_effect(c, after);
    else rebound = read_rebound_by(c, x, after);
  }
  free(dfl);
  return rebound ? -1 : i;
}

/* A lent parameter a gather funds from a variable written ahead of the
   first splat (gather_lead_arg) lends that variable's slot, as the call
   without the splat would: bound out of the gather into a temp,
   `grow(s, *xs, 9)` appended to the temp and s stayed as it was. Only a
   local or an ivar, whose read the gather already ran and a second read
   repeats; any other argument keeps the gather's value in a temp. `argv`
   holds all `argc` arguments. 1 when it emitted. */
static int emit_gather_lead_lent(Compiler *c, Scope *m, int i, const int *argv, int argc,
                                 Buf *out) {
  LocalVar *p = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
  if (!p || !p->byref_out) return 0;
  int k = gather_lead_arg(c, m, argv, argc, i);
  if (k < 0) return 0;
  NodeKind ak = nt_kind(c->nt, argv[k]);
  if (ak != NK_LocalVariableReadNode && ak != NK_InstanceVariableReadNode) return 0;
  emit_arg_or_default(c, m, i, argv[k], out);
  return 1;
}

/* The argument node call `call` binds parameter i of m to, by the layout the
   binders follow: the positional placed there, or the value of the keyword
   naming it (the last one written). -1 when no one node is the parameter's
   value -- a default, a splat's element, a value out of a `**` or a merged
   hash; `*spread` (when asked) then names the splat's or the `**`'s operand
   the value comes out of, or -1. What the analysis reads to follow a String
   into the parameter that mutates it: indexing the call's arguments by the
   parameter's position read a keyword hash, or the argument beside a rest,
   for a keyword or a post. */
int arg_layout_param_node(Compiler *c, Scope *m, int call, int i, int *spread) {
  const NodeTable *nt = c->nt;
  if (spread) *spread = -1;
  if (!m || i < 0 || i >= m->nparams) return -1;
  int args = nt_ref(nt, call, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int kwh = -1, pos_argc = argc, nsplat = 0, splat = -1;
  if (argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode) { kwh = argv[argc - 1]; pos_argc--; }
  for (int k = 0; k < pos_argc; k++) {
    NodeKind ak = nt_kind(nt, argv[k]);
    if (nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "ForwardingArgumentsNode")) return -1;
    if (ak == NK_SplatNode) { nsplat++; splat = argv[k]; }
  }
  /* plain arguments into required positionals alone: the one layout there is */
  if (kwh < 0 && !nsplat && m->rest_idx < 0 && m->kwrest_idx < 0) {
    int plain = 1;
    for (int k = 0; k < m->nparams && plain; k++)
      if ((m->pdefault && m->pdefault[k] >= 0) ||
          (m->pnames[k] && callee_param_is_declared_kwarg(c, m, m->pnames[k]))) plain = 0;
    if (plain) return i < argc ? argv[i] : -1;
  }
  ArgLayout L;
  arg_layout(c, m, argv, pos_argc, kwh, 0, &L);
  int a = -1;
  const char *pn = m->pnames[i];
  int lead = L.gather ? gather_lead_arg(c, m, argv, argc, i) : -1;
  if (L.from[i] == ARG_NODE) a = argv[L.arg[i]];
  else if (lead >= 0) a = argv[lead];
  else if (L.from[i] == ARG_ELEM || L.from[i] == ARG_GATHERED) {
    if (nsplat == 1 && spread) *spread = nt_ref(nt, splat, "expression");
    /* Splats of array literals alone (`m(*[], s)`, `m(*[s])`) have a count
       the program states: the arguments they spread are the elements, laid
       out as if written in their place. */
    int flat_n = 0, ok = 1;
    for (int k = 0; k < pos_argc && ok; k++) {
      if (nt_kind(nt, argv[k]) != NK_SplatNode) { flat_n++; continue; }
      int lit = nt_ref(nt, argv[k], "expression");
      int en = 0; const int *el = lit >= 0 && nt_kind(nt, lit) == NK_ArrayNode ? nt_arr(nt, lit, "elements", &en) : NULL;
      if (!el && !(lit >= 0 && nt_kind(nt, lit) == NK_ArrayNode)) { ok = 0; break; }
      for (int e = 0; e < en; e++) if (nt_kind(nt, el[e]) == NK_SplatNode) ok = 0;
      flat_n += en;
    }
    if (ok) {
      int *flat = malloc(sizeof(int) * (size_t)(flat_n + 1)), f = 0;
      for (int k = 0; k < pos_argc; k++) {
        if (nt_kind(nt, argv[k]) != NK_SplatNode) { flat[f++] = argv[k]; continue; }
        int en = 0; const int *el = nt_arr(nt, nt_ref(nt, argv[k], "expression"), "elements", &en);
        for (int e = 0; e < en; e++) flat[f++] = el[e];
      }
      if (kwh >= 0) flat[f] = kwh;
      ArgLayout F;
      arg_layout(c, m, flat, flat_n, kwh, 0, &F);
      if (F.from[i] == ARG_NODE) a = flat[F.arg[i]];
      arg_layout_free(&F);
      free(flat);
    }
  }
  else if (L.from[i] == ARG_BY_NAME && i != m->kwrest_idx && pn && kwh >= 0 &&
           callee_has_kwarg(c, m, pn)) {
    int en = 0; const int *el = nt_arr(nt, kwh, "elements", &en);
    int nds = 0, ds = -1;
    for (int e = 0; e < en; e++)
      if (nt_kind(nt, el[e]) == NK_AssocSplatNode) { nds++; ds = el[e]; }
    if (!kwh_merged(c, m, kwh)) a = kwh_lookup(nt, kwh, pn);
    if (a < 0 && nds == 1 && spread) *spread = nt_ref(nt, ds, "value");
  }
  arg_layout_free(&L);
  return a;
}

/* How far from the end of the positionals parameter j of m takes its value,
   whatever their count: a post (behind a rest or an optional) the one as far
   from the last, and any positional of a method that takes a fixed count.
   0 when the count decides. */
static int positional_offset_from_end(Compiler *c, Scope *m, int j) {
  int end = 0, fixed = m->rest_idx < 0;
  while (end < m->nparams && end != m->kwrest_idx &&
         !callee_param_is_declared_kwarg(c, m, m->pnames[end])) {
    if (m->pdefault && m->pdefault[end] >= 0) fixed = 0;
    end++;
  }
  if (j < 0 || j >= end || j == m->rest_idx) return 0;
  return fixed || j >= end - m->npost_rest ? end - j : 0;
}

/* The parameter of s whose value a bare `super` in s hands parameter j of pm,
   or -1: a keyword the like-named keyword, a positional by the layout of s's
   positionals over pm's (as zsuper_begin lays them), and around s's rest,
   where the gather holds them in order, the one at j when pm takes element j
   there, or s's post as far from the gather's end as pm's parameter j takes
   it. What the byref analysis follows through a bare super, and what the
   gathered bare super lends. */
int zsuper_param_source(Compiler *c, Scope *s, Scope *pm, int j) {
  if (!s || !pm || j < 0 || j >= pm->nparams || !pm->pnames[j] || j == pm->kwrest_idx) return -1;
  if (callee_param_is_declared_kwarg(c, pm, pm->pnames[j])) {
    if (!callee_param_is_declared_kwarg(c, s, pm->pnames[j])) return -1;
    for (int k = 0; k < s->nparams; k++)
      if (s->pnames[k] && sp_streq(s->pnames[k], pm->pnames[j])) return k;
    return -1;
  }
  int npos = 0;
  while (npos < s->nparams && npos != s->rest_idx && npos != s->kwrest_idx &&
         !callee_param_is_declared_kwarg(c, s, s->pnames[npos])) npos++;
  if (s->rest_idx >= 0) {
    if (j < npos && j != pm->rest_idx && !(pm->rest_idx >= 0 && j > pm->rest_idx) &&
        !(pm->rest_idx < 0 && opt_before_required(c, pm))) return j;
    /* The gather ends with s's posts, and a parameter of pm taking an
       element as far from the end is one of them: `def m(a, *r, last) =
       super` into a parent's `m(a, *r, last)`. Not when s's keywords would
       ride at the gather's end, as the hash of a parent that takes none. */
    int off = positional_offset_from_end(c, pm, j);
    int s_kw = s->kwrest_idx >= 0;
    for (int k = npos; k < s->nparams && !s_kw; k++)
      if (k != s->rest_idx && callee_param_is_declared_kwarg(c, s, s->pnames[k])) s_kw = 1;
    if (off < 1 || off > s->npost_rest || !s->pnames[s->rest_idx] ||
        (s_kw && !callee_declares_kwargs(c, pm) && pm->kwrest_idx < 0)) return -1;
    return s->rest_idx + s->npost_rest + 1 - off;
  }
  return arg_layout_plain_arg(c, pm, npos, j);
}

/* Gather a call's positionals, the splat spread in place, into one rooted
   PolyArray and refuse a count the parameters cannot take. The keyword hash
   is the last of them where the layout says (gather_kwh): one of `**`
   spreads alone when it is not empty, a literal one always. Returns the
   temp. */
int emit_splat_gather(Compiler *c, Scope *m, const int *argv, const ArgLayout *L) {
  const NodeTable *nt = c->nt;
  int pos_argc = L->pos_argc, kwh = L->kwh;
  int ct = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ct, ct);
  for (int k = 0; k < pos_argc; k++) {
    Buf ab; memset(&ab, 0, sizeof ab);
    if (nt_kind(nt, argv[k]) == NK_SplatNode) {
      int inner = nt_ref(nt, argv[k], "expression");
      if (inner < 0) {
        if (!emit_anon_rest_ref(c, argv[k], &ab)) buf_puts(&ab, "sp_PolyArray_new()");
      }
      else {
        buf_puts(&ab, "sp_poly_to_poly_array(sp_splat_to_array(");
        emit_boxed(c, inner, &ab);
        buf_puts(&ab, "))");
      }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray_append_all(_t%d, %s);\n", ct, ab.p ? ab.p : "NULL");
    }
    else {
      emit_boxed(c, argv[k], &ab);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ct, ab.p ? ab.p : "sp_box_nil()");
    }
    free(ab.p);
  }
  Buf kb; memset(&kb, 0, sizeof kb);
  if (L->gather_kwh == 2) {
    emit_boxed(c, kwh, &kb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ct, kb.p ? kb.p : "sp_box_nil()");
  }
  else if (L->gather_kwh == 1 && emit_kwh_spread_arg(c, kwh, &kb)) {
    int kt = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);"
                      " if (sp_poly_length(_t%d) > 0) sp_PolyArray_push(_t%d, _t%d);\n",
               kt, kb.p ? kb.p : "sp_box_nil()", kt, kt, ct, kt);
  }
  free(kb.p);
  emit_gather_arity_check(c, m, ct);
  return ct;
}

/* Refuse a gathered positional count the parameters cannot take. */
void emit_gather_arity_check(Compiler *c, Scope *m, int ct) {
  char gv[32]; snprintf(gv, sizeof gv, "_t%d->len", ct);
  emit_positional_count_check(c, m, gv);
}

/* Where positional parameter i finds its argument among `len` gathered
   ones (a C expression), by the count they are: the index into `idx`, a
   post from the end. Answers the count the arguments must exceed for it to
   be there -- an optional past the end takes its default -- or -1 for one
   the count check guarantees. A rest's own slice is [rest, len - *npost). */
int gathered_param_index(Compiler *c, Scope *m, int i, const char *len, char *idx, size_t cap,
                         int *npost_out) {
  int rest = m->rest_idx, npost = rest >= 0 ? m->npost_rest : 0;
  /* the first parameter funded from the end: a rest's first post, or with a
     leading optional and no rest the first required after the optionals,
     which Ruby funds ahead of them (arg_slot_for_param) */
  int post_from = rest + 1;
  if (rest < 0 && opt_before_required(c, m)) {
    int n = m->nparams;
    while (n > 0 && (n - 1 == m->kwrest_idx || callee_param_is_declared_kwarg(c, m, m->pnames[n - 1]))) n--;
    post_from = 0;
    while (post_from < n && (!m->pdefault || m->pdefault[post_from] < 0)) post_from++;
    while (post_from < n && m->pdefault && m->pdefault[post_from] >= 0) post_from++;
    npost = n - post_from;
  }
  if (npost_out) *npost_out = npost;
  int is_post = rest >= 0 ? i > rest : npost > 0 && i >= post_from;
  if (is_post) snprintf(idx, cap, "%s - %d", len, post_from + npost - i);
  else snprintf(idx, cap, "%d", i);
  if ((rest < 0 || i < rest) && (i >= m->nrequired || (m->pdefault && m->pdefault[i] >= 0)))
    return i + npost;
  return -1;
}

/* Positional parameter i from the emit_splat_gather temp, by the count it
   holds: a rest's slice, a post from the end, an optional past the end
   taking its default. */
void emit_gathered_param(Compiler *c, Scope *m, int i, int ct, Buf *out) {
  char len[32], idx[64];
  snprintf(len, sizeof len, "_t%d->len", ct);
  int npost = 0;
  int need = gathered_param_index(c, m, i, len, idx, sizeof idx, &npost);
  int rest = m->rest_idx;
  if (i == rest) {
    int t = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb);
    buf_printf(&rb,
               "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
               " for (sp_int _si = (%d < _t%d->len - %d ? %d : _t%d->len - %d); _si < _t%d->len - %d; _si++)"
               " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _si)); _t%d; })",
               t, t, rest, ct, npost, rest, ct, npost, ct, npost, t, ct, t);
    LocalVar *rp = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
    if (rp && rp->type == TY_POLY) emit_boxed_text(c, TY_POLY_ARRAY, rb.p, out);
    else buf_puts(out, rb.p);
    free(rb.p);
    return;
  }
  LocalVar *sp = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
  if (sp && sp->byref_out) {
    Buf vb; memset(&vb, 0, sizeof vb);
    sp->byref_out = 0;
    emit_gathered_param(c, m, i, ct, &vb);
    sp->byref_out = 1;
    emit_lent_temp(vb.p, out);
    free(vb.p);
    return;
  }
  TyKind pt = sp ? sp->type : TY_POLY;
  Buf eb; memset(&eb, 0, sizeof eb);
  char raw[128];
  snprintf(raw, sizeof raw, "sp_PolyArray_get(_t%d, %s)", ct, idx);
  if (pt != TY_POLY && pt != TY_UNKNOWN) emit_unbox_nilable_text(c, pt, raw, &eb);
  else buf_puts(&eb, raw);
  if (need >= 0) {
    Buf db; memset(&db, 0, sizeof db);
    emit_arg_or_default(c, m, i, -1, &db);
    buf_printf(out, "(%d < _t%d->len ? %s : %s)", need, ct, eb.p ? eb.p : "",
               db.p ? db.p : default_value(pt));
    free(db.p);
  }
  else buf_puts(out, eb.p ? eb.p : raw);
  free(eb.p);
}

/* The splat a static layout spreads in place (ArgLayout.splat), evaluated
   once into a rooted temp; its array type into `*at`. A boxed operand -- a
   block parameter, a value read out of a container -- is an array only at
   run time: the splat's own lowering normalizes it (nil to [], a scalar to
   [v], an array kept), where it once fell through as one positional
   argument. A statically nil or scalar operand takes the same lowering:
   bound as it was, `f(*nil)` passed [] and `f(*5)` passed [5] as the first
   argument. Returns the temp. */
static int emit_splat_in_place(Compiler *c, int splat, TyKind *at) {
  int inner = nt_ref(c->nt, splat, "expression");
  TyKind t = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
  Buf anon; memset(&anon, 0, sizeof anon);
  int is_anon = inner < 0 && emit_anon_rest_ref(c, splat, &anon);
  int boxed = !is_anon && inner >= 0 && (t == TY_POLY || t == TY_UNKNOWN || splat_operand_is_scalar(t));
  if (is_anon || boxed) t = TY_POLY_ARRAY;
  *at = t;
  int tmp = ++g_tmp;
  /* Evaluate the splat operand into a side buffer: a literal array or a
     call result emits its own setup (a fresh `_tN = ..._new()` decl) into
     g_pre, which must land before this temp's declaration line -- a bare
     local read has no setup, which is why those already worked. */
  emit_indent(g_pre, g_indent);
  if (is_anon) buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", tmp, anon.p ? anon.p : "sp_PolyArray_new()");
  else {
    Buf sb; memset(&sb, 0, sizeof sb);
    emit_expr(c, boxed ? splat : inner, &sb);
    emit_ctype(c, t, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tmp, sb.p ? sb.p : "");
    free(sb.p);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tmp);
  free(anon.p);
  return tmp;
}

/* The count a trailing splat spread in place supplies, measured into a temp
   where the array is (`splat_tmp`), or -1 when it is not judged here. A
   target without a rest is judged on any count, a positional Hash of
   literal keys one more argument. A rest target has only its shortfall,
   judged by the rule the static check applies (rest_required_for) where the
   hash adds nothing: a hash the callee takes as keywords is no positional
   (kw_plan), so keywords beside the rest do not stand it down, and `def
   m(a, *r, k: 1)` called `m(*[])` is given 0. */
static int emit_splat_given_count(Compiler *c, Scope *m, const ArgLayout *L, int splat_tmp) {
  if (L->splat < 0 || L->splat != L->pos_argc - 1 || arity_unjudged(c, m)) return -1;
  if (m->rest_idx >= 0 && (L->kw.count != KWC_NONE || rest_required_for(c, m) < 0)) return -1;
  int gv = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = %d + (_t%d ? _t%d->len : 0);\n", gv,
             L->splat + (m->rest_idx < 0 && L->kw.count == KWC_ONE), splat_tmp, splat_tmp);
  return gv;
}

/* Parameter i from element `off` of the splat temp `tmp` (of array type
   `at`), boxed from a gather (`gathered`) or typed from a splat spread in
   place (ArgLayout's ARG_ELEM). */
static void emit_elem_param(Compiler *c, Scope *m, int i, int off, int tmp, TyKind at, int gathered,
                            Buf *out) {
  LocalVar *sp = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
  if (sp && sp->byref_out) {
    /* an element (or the default it falls back to) has no caller variable
       to write back to: bind the value, then lend a temp */
    Buf vb; memset(&vb, 0, sizeof vb);
    sp->byref_out = 0;
    emit_elem_param(c, m, i, off, tmp, at, gathered, &vb);
    sp->byref_out = 1;
    emit_lent_temp(vb.p, out);
    free(vb.p);
    return;
  }
  TyKind set = ty_array_elem(at);
  Buf eb; memset(&eb, 0, sizeof eb);
  if (sp && sp->type == TY_POLY && set != TY_POLY && set != TY_UNKNOWN) {
    /* a scalar splat element into a poly-widened param: box it */
    Buf raw; memset(&raw, 0, sizeof raw);
    emit_array_elem_at(at, tmp, off, &raw);
    emit_boxed_text(c, set, raw.p ? raw.p : "0", &eb); free(raw.p);
  }
  else if (sp && sp->type == TY_STRBUF && sp->str_shared && set == TY_STRING) {
    /* a String element into a shared-handle parameter: a handle of its own,
       as any value that is not a caller's variable gets */
    Buf raw; memset(&raw, 0, sizeof raw);
    emit_array_elem_at(at, tmp, off, &raw);
    buf_printf(&eb, "sp_String_new_shared(%s)", raw.p ? raw.p : "NULL"); free(raw.p);
  }
  else emit_array_elem_at(at, tmp, off, &eb);
  /* The gathered positionals are boxed, and so is an element of a boxed
     splat spread in place (a poly array, or a scalar the splat normalized
     into one): a typed parameter unboxes. Inference widens a parameter a
     boxed splat reaches to poly, unless an --rbs seed pins it, so the pinned
     one is where a boxed element met a typed slot and the C did not compile;
     the seed is asserted there, as for a boxed argument (#3412). */
  if ((gathered || set == TY_POLY) && sp && sp->type != TY_POLY && sp->type != TY_UNKNOWN) {
    Buf ck; memset(&ck, 0, sizeof ck);
    if (sp->rbs_seeded) emit_rbs_checked_text(c, sp->type, m->pnames[i], eb.p ? eb.p : "sp_box_nil()", &ck);
    else buf_puts(&ck, eb.p ? eb.p : "sp_box_nil()");
    Buf ub; memset(&ub, 0, sizeof ub);
    emit_unbox_nilable_text(c, sp->type, ck.p, &ub);
    free(ck.p); free(eb.p); eb = ub;
  }
  /* An optional param may fall past the end of a (runtime-sized) splat
     array; the arity check guarantees the required params are present, so
     guard only the optionals and fall back to their default. An optional
     ahead of a required keyword sits below nrequired, which counts that
     keyword, so its default says it is one. */
  if (i >= m->nrequired || (m->pdefault && m->pdefault[i] >= 0)) {
    Buf db; memset(&db, 0, sizeof db);
    emit_arg_or_default(c, m, i, -1, &db);
    TyKind pt = sp ? sp->type : TY_INT;
    buf_printf(out, "(%d < (_t%d ? _t%d->len : 0) ? %s : %s)", off, tmp, tmp,
               eb.p ? eb.p : "", db.p ? db.p : default_value(pt));
    free(db.p);
  }
  else buf_puts(out, eb.p ? eb.p : "");
  free(eb.p);
}

void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out) {
  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(c->nt, argsNode, "arguments", &argc) : NULL;
  emit_args_filled_argv(c, callee_idx, argv, argc, argsNode, lead, out);
}

/* See codegen_internal.h. */
void emit_args_filled_argv(Compiler *c, int callee_idx, const int *argv, int argc, int argsNode,
                           const char *lead, Buf *out) {
  Scope *m = &c->scopes[callee_idx];
  const NodeTable *nt = c->nt;
  /* `bar(...)`: the ArgumentsNode holds a single ForwardingArgumentsNode.
     Forward the enclosing `def foo(...)` method's synthesized __fwd_* params
     directly to the callee, positionally (#1288). The compiler already knows
     foo's args; no rest array / splat is materialized. */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    /* A leading concrete param before `...` (`def f(a, ...)`) is NOT forwarded:
       the forward carries only the __fwd_ slots (and synthesized keyword
       params). Skip the enclosing method's leading concrete params by starting
       at its first __fwd_ slot. */
    int fwd_base = 0;
    if (encl) {
      while (fwd_base < encl->nparams &&
             (!encl->pnames[fwd_base] ||
              strncmp(encl->pnames[fwd_base], "__fwd_", 6) != 0)) fwd_base++;
      if (fwd_base >= encl->nparams) fwd_base = 0;  /* no __fwd_ slot: forward all */
    }
    for (int i = 0; i < m->nparams; i++) {
      buf_puts(out, i == 0 ? lead : ", ");
      int ei = fwd_base + i;
      if (encl && ei < encl->nparams) {
        LocalVar *ep = scope_local(encl, encl->pnames[ei]);
        LocalVar *mp = scope_local(m, m->pnames[i]);
        TyKind et = ep ? ep->type : TY_POLY;
        TyKind mt = mp ? mp->type : TY_POLY;
        /* forwarding into a byref out-param slot: pass the enclosing param's
           slot (its cell when it is itself byref/celled, else its address) */
        if (mp && mp->byref_out && ep && et == TY_STRING) {
          if (ep->is_cell) buf_printf(out, "_cell_%s", encl->pnames[ei]);
          else buf_printf(out, "&lv_%s", encl->pnames[ei]);
          continue;
        }
        char txt[80]; snprintf(txt, sizeof txt, "lv_%s", encl->pnames[ei]);
        if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, out);
        else buf_puts(out, txt);
      }
      else emit_arg_rooted(c, m, i, -1, out);
    }
    return;
  }
  /* Separate trailing keyword-hash arg (if any) from positional args. */
  int kwh = -1;
  int pos_argc = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Which argument each positional parameter takes. A keyword hash that
     binds positionally is one more argument, and with a leading optional it
     moves the others too: `def h(a = {}, c)` called `h(1, k: 9)` gives `a`
     the 1 and `c` the hash. */
  ArgLayout L;
  arg_layout(c, m, argv, pos_argc, kwh, 0, &L);
  int rest_argc = L.rest_argc;
  int argov_saved = g_n_argov;
  emit_call_arity_check(c, m, argc, argv);

  /* Detect double-splat (**hash) inside kwh: AssocSplatNode wrapping a hash expr.
     Pre-evaluate the hash to a temp so we can do per-param lookups. */
  int kw_merged = kwh_merged(c, m, kwh);
  if (L.kw.args_first || kwh_runs_ahead(c, m, kwh)) emit_args_run(c, argv, argc);
  /* the splat spread in place runs into its temp ahead of the call, so the
     arguments written to its left run first, into theirs: `m(lg(1), *lg(a))`
     ran lg(a) first */
  else if (!emit_args_before_binding(c, m, argv, argc, g_pre) && L.splat > 0)
    emit_args_in_source_order(c, argv, L.splat, g_pre);
  TyKind ds_hash_type = TY_UNKNOWN;
  int ds_hash_tmp = emit_ds_hash_materialize(c, m, kwh, &ds_hash_type);

  /* Find the first SplatNode in positional args. If it comes before rest_idx
     (or before nparams for rest-less methods), pre-evaluate it to a temp so
     we can index into it per fixed param. */
  int splat_idx = -1;  /* index into argv[] of the SplatNode */
  int splat_tmp = -1;  TyKind splat_at = TY_UNKNOWN;
  /* A call whose count is the run time's gathers every positional into one
     array, as CRuby does, then checks the count and binds from it. */
  int splat_all = L.gather;
  if (splat_all) {
    splat_idx = 0; splat_tmp = emit_splat_gather(c, m, argv, &L);
    splat_at = TY_POLY_ARRAY;
  }
  for (int k = 0; k < pos_argc && !splat_all; k++) {
    if (argv && nt_type(nt, argv[k]) && sp_streq(nt_type(nt, argv[k]), "SplatNode")) {
      int need_expand = (m->rest_idx >= 0 && k < m->rest_idx) ||
                        (m->rest_idx < 0 && k < m->nparams);
      if (need_expand) {
        splat_idx = k;
        if (k == L.splat) {
          splat_tmp = emit_splat_in_place(c, argv[k], &splat_at);
          int gv = emit_splat_given_count(c, m, &L, splat_tmp);
          if (gv >= 0) {
            char gvn[32]; snprintf(gvn, sizeof gvn, "_t%d", gv);
            emit_positional_count_check(c, m, gvn);
          }
        }
      }
      else if (m->rest_idx < 0) emit_unreached_splat_count(c, m, argv, argc, pos_argc, &L.kw);
      break;
    }
  }
  /* the keywords a `**` brings, judged once the count has been: CRuby
     counts first, so `def r(a, k:)` called `r(*[], **h)` is given 0 */
  emit_ds_kwarg_check(c, m, kwh, ds_hash_tmp, ds_hash_type);
  /* GC hazard: a freshly-allocated heap argument sits in an unrooted C
     temporary while the rest of the call is evaluated AND while the callee
     runs. Either a later argument or the callee's own body can trigger a
     collection that sweeps it -- and a constructor (sp_X_new) always
     allocates, so even `Ray.new(Vec.new(...), eye)` is exposed. Pre-evaluate
     each allocating heap arg into a rooted temp, left to right; emit_expr
     substitutes the temp via g_argov. Plain positional calls only -- the
     splat/kwarg machinery has its own evaluation order. */
  /* A parameter default that references an earlier parameter (`def f(a, b=a*2)`
     or `def f(a:, b: a*2)`) must be evaluated with that parameter bound. Ruby
     evaluates defaults left-to-right in the callee; spinel fills them at the
     call site, where the sibling's binding is absent, so a naive emit produces
     an undeclared `lv_<sibling>`. Bind each param to a uniquely-named call-site
     temp in order, registering a rename so a later default reads the earlier
     temp, then pass the temps. Restricted to calls with no splat expanding
     into fixed parameters. A *rest, its posts and a **kwrest are hoisted the
     way the path below binds them: leaving them out let
     `def m(n, *r, k: n + r.size)` and `def h(x, z = x * 2, **kw)` emit the
     default against a parameter nothing at the call site declared. A keyword
     a `**h` may supply is read out of h, its default under the same renames.
     A gather binds each positional from the gathered array, its default under
     the renames too. */
  if ((splat_idx < 0 || splat_all) &&
      m->nparams <= 64 && default_refs_earlier_param(c, m)) {
    int uid = ++g_tmp;
    int ren_base = g_nren;
    char tmpnames[64][64];
    for (int i = 0; i < m->nparams; i++) {
      LocalVar *plv = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
      TyKind pt = plv ? plv->type : TY_POLY;
      int byref = plv && plv->byref_out;
      int is_rest = L.from[i] == ARG_REST, is_kwrest = i == m->kwrest_idx;
      int from_gather = L.from[i] == ARG_GATHERED || L.from[i] == ARG_ELEM;
      int provided = L.from[i] == ARG_NODE ? argv[L.arg[i]] : L.from[i] == ARG_KWH ? kwh : -1;
      /* only a keyword parameter binds a key by name; a keyword hash no
         parameter takes is one more positional argument, and fills the
         first unfilled slot -- the rules the path below follows (#4869) */
      if (provided < 0 && kwh >= 0 && !kw_merged && m->pnames[i] && callee_has_kwarg(c, m, m->pnames[i]))
        provided = kwh_lookup(nt, kwh, m->pnames[i]);
      Buf vb; memset(&vb, 0, sizeof vb);
      /* A provided (caller) argument is emitted with the sibling-param renames
         OFF -- only a callee default expression should resolve param references
         to the hoisted temps. */
      int active_nren = g_nren;
      if (!from_gather && (provided >= 0 || is_rest || is_kwrest)) g_nren = ren_base;
      if (from_gather) {
        /* a lent leading argument is the caller's, read without the renames */
        int sv_lead = g_nren;
        g_nren = ren_base;
        int lent = L.gather && emit_gather_lead_lent(c, m, i, argv, argc, &vb);
        g_nren = sv_lead;
        if (!lent) emit_gathered_param(c, m, i, splat_tmp, &vb);
      }
      else if (is_rest)
        emit_rest_pack_kwh(c, i, rest_argc - m->npost_rest, argv, L.rest_kwh, &vb);
      else if (is_kwrest) {
        int krhash = emit_kwrest_collect(c, m, kwh, ds_hash_tmp, ds_hash_type, argsNode);
        if (pt == TY_POLY) buf_printf(&vb, "sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH)", krhash);
        else buf_printf(&vb, "_t%d", krhash);
      }
      else if (provided < 0 && ds_hash_tmp >= 0 && m->pnames[i] && callee_has_kwarg(c, m, m->pnames[i]))
        emit_ds_param_extract(c, m, i, ds_hash_tmp, ds_hash_type, &vb);
      else emit_arg_or_default(c, m, i, provided, &vb);
      g_nren = active_nren;
      char uniq[48];
      snprintf(uniq, sizeof uniq, "_pd%d_%d", uid, i);
      emit_indent(g_pre, g_indent);
      /* A lent parameter's hoist is the LENT ADDRESS, not a copy of the
         string. emit_arg_or_default has already produced whichever of the
         four call-site forms this argument takes (`&lv_x`, `_cell_x`, a
         capture slot, `&_tN` for a default), all borrowing a String slot,
         so declaring it with the parameter's plain type gave
         `const char *lv__pdN_0 = &lv_s;`. And a sibling default that READS
         the parameter emits the cell spelling through the same rename map,
         `(*_cell__pdN_0)`, which nothing declared. Naming the hoist
         `_cell_<uniq>` makes the two meet, and it needs no root of its own:
         it points at a slot the caller already roots. */
      if (byref) {
        buf_printf(g_pre, "%s *_cell_%s = %s;\n", borrowed_string_type(plv), uniq, vb.p ? vb.p : "NULL");
      }
      else {
        emit_ctype(c, pt, g_pre);
        buf_printf(g_pre, " lv_%s = %s;\n", uniq, vb.p ? vb.p : default_value(pt));
        if (needs_root(pt)) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, pt == TY_POLY ? "SP_GC_ROOT_RBVAL(lv_%s);\n" : "SP_GC_ROOT(lv_%s);\n", uniq);
        }
        emit_pd_cell_alias(c, plv, uniq);
      }
      free(vb.p);
      /* Register the rename AFTER emitting temp i so param i+1's default reads
         it (rename_local rewrites the callee param name to the temp). */
      if (m->pnames[i] && g_nren < MAX_RENAME) {
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", m->pnames[i]);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "%s", uniq);
        g_nren++;
      }
      snprintf(tmpnames[i], sizeof tmpnames[0], byref ? "_cell_%s" : "lv_%s", uniq);
    }
    g_nren = ren_base;  /* pop the renames before emitting the call args */
    for (int i = 0; i < m->nparams; i++) {
      buf_puts(out, i == 0 ? lead : ", ");
      buf_puts(out, tmpnames[i]);
    }
    view_unbind(argov_saved);
    arg_layout_free(&L);
    return;
  }

  if (splat_idx < 0 && kwh < 0 && argv) {
    /* Ruby evaluates arguments left to right; C leaves a call's operand order
       unspecified (gcc walks it right to left). Once two arguments can observe
       each other's side effects, every one of them but the last has to be
       sequenced into a temp -- including scalars, which need no root. */
    int last_se = -1, n_se = 0;
    for (int k = 0; k < pos_argc && k < m->nparams; k++)
      if (subtree_has_side_effect(c, argv[k])) { last_se = k; n_se++; }
    for (int k = 0; k < pos_argc && k < m->nparams; k++) {
      argov_reserve();   /* past MAX_ARG_OVERRIDE arguments too */
      TyKind at = comp_ntype(c, argv[k]);
      /* An argument emit_ctype would spell `void` has no C storage to
         sequence into -- `void _tN = ...` is not a declaration C accepts.
         Nor is there anything to sequence: a valueless argument is a raise
         fallback (an unresolved call becomes sp_raise_nomethod), which does
         not return, so no sibling can observe it. Leave it to the argument
         slot below, which coerces it to the parameter's type. */
      int has_storage = ty_is_object(at) || c_type_name(at) != NULL;
      int seq = (has_storage && n_se >= 2 && k < last_se &&
                 subtree_has_side_effect(c, argv[k]));
      int root = (at == TY_POLY || needs_root(at));
      if (!root && !seq) continue;
      const char *aty = nt_type(nt, argv[k]);
      /* a splat at/after the rest slot (splat_idx only marks splats needing
         ELEMENT expansion) is consumed by the rest collection below, which
         evaluates and roots the operand itself -- hoisting here both
         double-evaluates it and emits an ill-typed sp_RbVal temp (the splat
         lowers to sp_PolyArray*) (#3242) */
      if (aty && sp_streq(aty, "SplatNode")) continue;
      /* one the call ran first (emit_args_before_binding) reads its temp */
      if (arg_ran_first(argv[k], argov_saved)) continue;
      /* `+"lit"` into a parameter that is the handle because a Method
         reaches it: the binding builds the handle from the literal itself
         (emit_arg_or_default), and a copy made here first went unused */
      if (!seq && dyn_handle_lit_arg(c, m, k, argv[k])) continue;
      /* a bare read is already rooted where it lives */
      if (aty && (sp_streq(aty, "LocalVariableReadNode") ||
                  sp_streq(aty, "InstanceVariableReadNode") ||
                  sp_streq(aty, "ConstantReadNode") ||
                  sp_streq(aty, "SelfNode") || sp_streq(aty, "NilNode") ||
                  sp_streq(aty, "StringNode"))) root = 0;
      /* only a fresh allocation needs protecting; a non-allocating heap
         expression (e.g. a ternary over two already-live reads) does not. */
      else if (!subtree_may_allocate(nt, argv[k])) root = 0;
      if (!root && !seq) continue;
      int ht = ++g_tmp;
      /* Evaluate into a side buffer first: the expression may push its own
         setup into g_pre, which must be fully flushed before this temp's
         declaration line is written. */
      Buf hb; memset(&hb, 0, sizeof hb);
      /* a write to a shared-string local handed to a mutable-string parameter
         is sequenced as the local's handle, which is what the slot takes */
      LocalVar *hp = m->pnames[k] ? scope_local(m, m->pnames[k]) : NULL;
      if (hp && hp->type == TY_STRBUF &&
          emit_strbuf_local_write_handle(c, argv[k], &hb)) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_String *_t%d = %s; SP_GC_ROOT(_t%d);\n", ht, hb.p, ht);
        free(hb.p);
        view_bind(argv[k], "_t%d", ht);
        continue;
      }
      emit_expr(c, argv[k], &hb);
      emit_indent(g_pre, g_indent);
      if (at == TY_POLY) {
        buf_printf(g_pre, "sp_RbVal _t%d = %s;", ht, hb.p ? hb.p : "sp_box_nil()");
        if (root) buf_printf(g_pre, " SP_GC_ROOT_RBVAL(_t%d);", ht);
        buf_puts(g_pre, "\n");
      }
else {
        emit_ctype(c, at, g_pre);
        buf_printf(g_pre, " _t%d = %s;", ht, hb.p ? hb.p : "0");
        if (root) buf_printf(g_pre, " SP_GC_ROOT(_t%d);", ht);
        buf_puts(g_pre, "\n");
      }
      free(hb.p);
      view_bind(argv[k], "_t%d", ht);
    }
  }
  for (int i = 0; i < m->nparams; i++) {
    buf_puts(out, i == 0 ? lead : ", ");
    if (L.gather && emit_gather_lead_lent(c, m, i, argv, argc, out)) {}
    else if (L.from[i] == ARG_GATHERED)
      emit_gathered_param(c, m, i, splat_tmp, out);
    else if (L.from[i] == ARG_REST) {
      /* rest collects middle args; stop before post-splat params */
      int rest_end = rest_argc - m->npost_rest;
      if (splat_tmp >= 0) {
        emit_rest_from_splat_and_argv(splat_tmp, splat_at, i - splat_idx,
                                      c, splat_idx + 1, rest_end, argv, out);
      }
else {
        emit_rest_pack_kwh(c, i, rest_end, argv, L.rest_kwh, out);
      }
    }
else if (L.from[i] == ARG_ELEM)
      emit_elem_param(c, m, i, L.arg[i], splat_tmp, splat_at, splat_all, out);
else {
      /* Check if this param has a keyword match (lookup by param name in kwh).
         Only a true KEYWORD param consumes a key -- a positional param whose
         name happens to match (e.g. `def target(a, **info)` called as
         `target(a, **info)`, the forwarding idiom) must take the provided
         positional below, not steal the key from the kwargs. Same rule the
         keyword-rest collection applies via callee_has_kwarg. */
      int is_kwparam = m->pnames[i] && callee_has_kwarg(c, m, m->pnames[i]);
      int kv = (kwh >= 0 && is_kwparam && !kw_merged) ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      if (kv >= 0) {
        emit_arg_rooted(c, m, i, kv, out);
      }
      else if (ds_hash_tmp >= 0 && is_kwparam && i != m->kwrest_idx) {
        /* Double-splat: extract param by name from the pre-eval'd hash. */
        emit_ds_param_extract(c, m, i, ds_hash_tmp, ds_hash_type, out);
      }
      else if (m->kwrest_idx >= 0 && i == m->kwrest_idx) {
        /* Collect remaining (unbound) keyword args into a sp_SymPolyHash. When
           the kwrest param is typed poly (sp_RbVal) rather than a concrete
           SymPolyHash* -- as happens forwarding `**kwargs` into a `**extra`
           where the param inference stayed poly -- box the collected hash so it
           matches the C signature (#3176). */
        int krhash = emit_kwrest_collect(c, m, kwh, ds_hash_tmp, ds_hash_type, argsNode);
        LocalVar *krp = m->pnames[i] ? scope_local(m, m->pnames[i]) : NULL;
        if (krp && krp->type == TY_POLY)
          buf_printf(out, "sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH)", krhash);
        else
          buf_printf(out, "_t%d", krhash);
      }
      else if (L.from[i] == ARG_NODE) {
        /* a declared KEYWORD param is never bound by position: only a
           positional param takes a surplus positional arg here. An unmatched
           keyword param falls through to its default below (#3114). (A `...`
           forwarding method's synthesized positional params are not declared
           keywords, so they still bind here.) A post takes its argument from
           the end of the call's, and a positional after a mid-list splat the
           layout could not gather (`g(1, *m, 4)`) a tail parameter. */
        emit_arg_rooted(c, m, i, argv[L.arg[i]], out);
      }
      else {
        /* No positional arg and no keyword match. If the param is hash-typed
           (required `def f(attrs)` or optional `def f(opts = {})`) and the
           call site passed a KeywordHashNode (e.g. `f(key: val)`), Ruby packs
           the keywords into that hash parameter -- treat the whole kwh as the
           implicit hash argument rather than using the default. */
        /* a POLY POSITIONAL param (widened over hash + non-hash call sites)
           takes the packed keywords boxed, same as a hash-typed one (#2009).
           A declared KEYWORD param never takes the whole kwh: unmatched
           keyword params fall back to their default. */
        /* ...but only when nothing else consumed those keywords. A declared
           keyword param that took a key means the hash was keywords, not a
           positional argument, and passing it here binds it TWICE:
           `def f(a = nil, k: :default); f(k: 1)` gave `a` the whole `{k: 1}`
           while `k` also bound (#3525). The same call reached the right
           binding as soon as some other call site in the program supplied `a`
           positionally, because then `a` was not poly -- which is why it
           looked environmental and why my own test file, holding both shapes,
           immunised itself. */
        /* ...and only into the FIRST unfilled positional slot. Every later
           one hit this same fallback, so `def f(a = nil, b = nil); f(k: 1)`
           handed the hash to both (found while fixing #4030). */
        emit_arg_rooted(c, m, i, L.from[i] == ARG_KWH ? kwh : -1, out);
      }
    }
  }
  view_unbind(argov_saved);  /* drop this call's hoisted-arg overrides */
  arg_layout_free(&L);
}

int is_descendant(Compiler *c, int k, int anc) {
  for (int x = k; x >= 0; x = c->classes[x].parent) if (x == anc) return 1;
  return 0;
}

/* Number of distinct implementations of `name` across cid's subtree
   (cid + all descendants). >1 means a self/obj call needs runtime dispatch. */
int dispatch_impl_count(Compiler *c, int cid, const char *name) {
  int impls[256], n = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int def = -1;
    if (comp_method_in_chain(c, k, name, &def) < 0) continue;
    int seen = 0;
    for (int j = 0; j < n; j++) if (impls[j] == def) seen = 1;
    if (!seen && n < 256) impls[n++] = def;
  }
  return n;
}

/* Append `, <arg>` for dispatch arm `arm`'s parameter `a`, coercing the shared
   pre-evaluated temp `atmp[a]` (of C type `from`) to that arm's declared param
   type: a concrete value flowing into an sp_RbVal (untyped/poly) param is boxed,
   a poly temp flowing into a concrete param is unboxed, matching types pass raw.
   Different overrides of one method may type the same param differently (#3214). */
static void emit_arm_arg(Compiler *c, Scope *arm, int a, int atmp_id, TyKind from, Buf *b) {
  char tn[24]; snprintf(tn, sizeof tn, "_t%d", atmp_id);
  TyKind pt = TY_POLY;
  if (arm && arm->pnames && a < arm->nparams && arm->pnames[a]) {
    LocalVar *pl = scope_local(arm, arm->pnames[a]);
    pt = (pl && pl->type != TY_UNKNOWN) ? pl->type : TY_POLY;
  }
  buf_puts(b, ", ");
  if (pt == TY_POLY && from != TY_POLY && from != TY_UNKNOWN) emit_boxed_text(c, from, tn, b);
  else if (from == TY_POLY && pt != TY_POLY && pt != TY_UNKNOWN) emit_unbox_text(c, pt, tn, b);
  else buf_puts(b, tn);
}

/* The scope a dispatch arm calls for `kmi`: the method itself when it has a
   symbol, else its proc-form clone -- an inline-only method (a yield, or an
   `&blk` used only by `.call`/`.nil?`) has no function of its own, and its
   arm was dropped, so the ancestor's method ran for that class (#3399 made
   the clone for the poly dispatch). -1 when neither exists. */
static int dispatch_arm_scope(Compiler *c, int kmi) {
  if (kmi < 0) return -1;
  if (scope_has_callable_symbol(c, kmi)) return kmi;
  /* emitted whatever reachability says (codegen.c): nothing names the clone
     until this arm does */
  return scope_proc_form_of(c, kmi);
}

/* Does an arm of a dispatch switch need the call's block as an sp_Proc *? */
static int arm_takes_blk(Scope *s) {
  return s->blk_param && s->blk_param[0] && !s->yields;
}

/* How an arm's parameter takes a String: 1 the lent slot (byref), 2 the
   shared handle, 0 anything else. */
static int arm_string_abi(const LocalVar *p) {
  if (!p) return 0;
  if (p->byref_out) return 1;
  return p->type == TY_STRBUF && p->str_shared ? 2 : 0;
}

/* Do the switch's arms bind the call's arguments differently? The shared path
   evaluates the arguments once, laid out for the base method, and hands every
   arm the same temps. That is only right when every arm takes the same plain
   list: an override with another count, a rest, a keyword or a block slot got
   a C call with the wrong number of arguments, and one with a default got the
   base method's default instead of its own (#4866). */
int dispatch_arms_disagree(Compiler *c, int cid, const char *name) {
  Scope *first = NULL;
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = dispatch_arm_scope(c, comp_method_in_chain(c, k, name, &kd));
    if (kmi < 0) continue;
    Scope *s = &c->scopes[kmi];
    /* a clone always takes the block, which the shared path never passes; nor
       does it pass one to an arm declaring `&blk` (a bare super's forwarding
       slot included), so `A.new.m(2) { }` with every arm taking it called
       each with one argument short */
    if (s->is_proc_form || arm_takes_blk(s)) return 1;
    if (!first) { first = s; }
    if (s == first) continue;
    if (s->nparams != first->nparams || s->rest_idx != first->rest_idx ||
        s->kwrest_idx != first->kwrest_idx || s->npost_rest != first->npost_rest ||
        arm_takes_blk(s) != arm_takes_blk(first))
      return 1;
    for (int i = 0; i < s->nparams; i++) {
      if ((s->pdefault && s->pdefault[i] >= 0) || (first->pdefault && first->pdefault[i] >= 0))
        return 1;
      if (!s->pnames[i] || !first->pnames[i]) return 1;
      int kw_s = callee_has_kwarg(c, s, s->pnames[i]);
      if (kw_s != callee_has_kwarg(c, first, first->pnames[i])) return 1;
      if (kw_s && !sp_streq(s->pnames[i], first->pnames[i])) return 1;
      /* a String the arms take in different forms -- the lent slot, the
         shared handle, the value -- is one no single temp can be: a Sub
         whose parameter became the handle took the base method's copy, or
         its `const char **`, and the C build stopped (#6065) */
      if (arm_string_abi(scope_local(s, s->pnames[i])) !=
          arm_string_abi(scope_local(first, first->pnames[i]))) return 1;
    }
  }
  return 0;
}

/* One arm of a per-arm dispatch: the call of `kmi` (defined in class `kd`) with
   its own argument list, assigned to the result temp. */
static void emit_dispatch_arm_call(Compiler *c, int kd, int kmi, const char *selfptr,
                                   int argsNode, int blk_tmp, TyKind ret, TyKind disp_ret,
                                   int rtmp, Buf *b) {
  Scope *s = &c->scopes[kmi];
  const char *cn = c->classes[kd].c_name;
  Buf apre; memset(&apre, 0, sizeof apre);
  Buf call; memset(&call, 0, sizeof call);
  Buf *sv_pre = g_pre; int sv_ind = g_indent;
  const char *sv_arm_self = g_arm_self; const Scope *sv_arm_scope = g_arm_scope;
  int sv_arm_depth = g_arm_depth;
  char arm_self[160];
  snprintf(arm_self, sizeof arm_self, "((sp_%s *)%s)", cn, selfptr);
  g_pre = &apre; g_indent = 0;
  g_arm_self = arm_self; g_arm_scope = s; g_arm_depth = g_expr_depth;
  buf_printf(&call, "sp_%s_%s((sp_%s *)%s", cn, mc(s->name), cn, selfptr);
  emit_args_filled(c, kmi, argsNode, ", ", &call);
  g_pre = sv_pre; g_indent = sv_ind;
  g_arm_self = sv_arm_self; g_arm_scope = sv_arm_scope; g_arm_depth = sv_arm_depth;
  if (arm_takes_blk(s)) {
    if (blk_tmp >= 0) buf_printf(&call, ", _t%d", blk_tmp);
    else buf_puts(&call, ", NULL");
  }
  buf_puts(&call, ")");
  buf_puts(b, "{ ");
  if (apre.p) buf_puts(b, apre.p);
  TyKind arm_ret = (TyKind)s->ret;
  if (method_is_void(s))
    buf_printf(b, "%s; _t%d = %s; ", call.p, rtmp, default_value(disp_ret));
  else if (arm_ret != ret && ret == TY_POLY) {
    buf_printf(b, "_t%d = ", rtmp);
    emit_boxed_text(c, arm_ret, call.p, b);
    buf_puts(b, "; ");
  }
  /* a proc-form clone answers boxed, whatever slot the switch fills */
  else if (arm_ret == TY_POLY && disp_ret != TY_POLY) {
    buf_printf(b, "_t%d = ", rtmp);
    emit_unbox_text(c, disp_ret, call.p, b);
    buf_puts(b, "; ");
  }
  else buf_printf(b, "_t%d = %s; ", rtmp, call.p);
  buf_puts(b, "break; }");
  free(apre.p); free(call.p);
}

/* The runtime class a virtual dispatch switches on. A user exception is an
   sp_Exception, whose header carries no cls_id, so it keys by its class
   name through sp_exc_user_cls_id. */
void emit_obj_dispatch_key(Compiler *c, int cid, const char *selfptr, Buf *b) {
  if (cid >= 0 && class_is_exc_subclass(c, cid))
    buf_printf(b, "sp_exc_user_cls_id(sp_box_obj((void *)(%s), SP_BUILTIN_EXCEPTION))", selfptr);
  else
    buf_printf(b, "(%s)->cls_id", selfptr);
}

/* The dispatch switch for arms that disagree on their parameters: each arm
   binds the call's arguments by its own method's list, the way a direct call
   to that method would, defaults and arity check included. The argument
   expressions sit inside the arms, so only the entered one evaluates them. */
static void emit_dispatch_per_arm(Compiler *c, int cid, const char *name, const char *selfptr,
                                  int argsNode, int blk_node, int mi, int defcls,
                                  TyKind ret, TyKind disp_ret, Buf *b) {
  int want_blk = 0;
  for (int k = 0; k < c->nclasses && !want_blk; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = dispatch_arm_scope(c, comp_method_in_chain(c, k, name, &kd));
    if (kmi >= 0 && arm_takes_blk(&c->scopes[kmi])) want_blk = 1;
  }
  int blk_tmp = -1;
  if (want_blk) blk_node = resolve_forwarded_block(c, blk_node);
  if (want_blk && blk_node >= 0) blk_tmp = emit_blk_proc_tmp(c, blk_node);
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, disp_ret, b);
  buf_printf(b, " _t%d; switch (", rtmp);
  emit_obj_dispatch_key(c, cid, selfptr, b);
  buf_puts(b, ") {");
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi0 = comp_method_in_chain(c, k, name, &kd);
    int kmi = dispatch_arm_scope(c, kmi0);
    if (kmi < 0) continue;
    nd_callee(c, g_nd_call_id, kmi0, kd, 1);
    buf_printf(b, " case %d: ", k);
    emit_dispatch_arm_call(c, kd, kmi, selfptr, argsNode, blk_tmp, ret, disp_ret, rtmp, b);
  }
  buf_puts(b, " default: ");
  int dmi = mi >= 0 && dispatch_arm_scope(c, mi) >= 0 ? dispatch_arm_scope(c, mi) : mi;
  if (dmi >= 0)
    emit_dispatch_arm_call(c, defcls, dmi, selfptr, argsNode, blk_tmp, ret, disp_ret, rtmp, b);
  else
    buf_printf(b, "_t%d = %s; break;", rtmp,
               ret == TY_POLY ? "sp_box_nil()" : default_value(disp_ret));
  buf_printf(b, " } _t%d; })", rtmp);
}

/* Will a call `id` of `name` on a `cid` whose chain answers it with an attr
   reader (of type reader_ty) dispatch on the runtime class: some descendant
   overrides the reader with a def, and the arms agree on the call's type? */
static int reader_override_arms(Compiler *c, int id, int cid, const char *name, TyKind reader_ty) {
  const NodeTable *nt = c->nt;
  if (nt_ref(nt, id, "block") >= 0) return 0;
  int base_mi = comp_method_in_chain(c, cid, name, NULL);
  int any = 0;
  for (int k = 0; k < c->nclasses && !any; k++) {
    if (k == cid || !is_descendant(c, k, cid)) continue;
    int kmi = comp_method_in_chain(c, k, name, NULL);
    if (kmi >= 0 && kmi != base_mi) any = 1;
  }
  if (!any) return 0;
  TyKind ret = comp_ntype(c, id);
  if (ret == TY_UNKNOWN || ret == TY_VOID || ret == TY_NIL) return 0;
  if (ret != reader_ty && ret != TY_POLY) return 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid || !is_descendant(c, k, cid)) continue;
    int kmi = comp_method_in_chain(c, k, name, NULL);
    if (kmi < 0 || kmi == base_mi) continue;
    if (dispatch_arm_scope(c, kmi) < 0) return 0;
    TyKind kr = (TyKind)c->scopes[kmi].ret;
    if (kr != ret && ret != TY_POLY) return 0;
  }
  return 1;
}

/* The type of the field read a reader of `name` on `cid` lowers to, when a
   call `id` of it has to dispatch on the runtime class instead (see
   reader_override_arms); TY_UNKNOWN otherwise, for an alias of a reader, and
   for a shared-mutable String slot, whose read is a copy the dispatch's
   default arm does not make. */
TyKind reader_override_ty(Compiler *c, int id, int cid, const char *name) {
  int rdc = -1;
  if (!comp_reader_in_chain(c, cid, name, &rdc)) return TY_UNKNOWN;
  /* an alias of a reader: a def of the ORIGINAL name in a subclass does not
     override the alias, and comp_method_in_chain would follow the alias to it */
  { const char *al = comp_resolve_alias(c, cid, name);
    if (al && !sp_streq(al, name)) return TY_UNKNOWN; }
  char ivn[300]; snprintf(ivn, sizeof ivn, "@%s", comp_resolve_alias(c, cid, name));
  ClassInfo *owner = &c->classes[rdc >= 0 ? rdc : cid];
  int iv = comp_ivar_index(owner, ivn);
  if (iv < 0 || owner->ivar_types[iv] == TY_UNKNOWN || owner->ivar_types[iv] == TY_STRBUF) return TY_UNKNOWN;
  return reader_override_arms(c, id, cid, name, owner->ivar_types[iv]) ? owner->ivar_types[iv] : TY_UNKNOWN;
}

/* A call of `name` that cid's chain answers with an attr reader, in a class
   some descendant of which overrides the reader with a def: a switch on the
   runtime class, with an arm calling the def for each overriding descendant
   and the reader's text (`reader`, of type reader_ty) for the rest. 0 when no
   descendant overrides it, or when the arms cannot agree on the call's type. */
int emit_reader_override_dispatch(Compiler *c, int id, int cid, const char *name,
                                  const char *selfptr, const char *reader,
                                  TyKind reader_ty, Buf *b) {
  const NodeTable *nt = c->nt;
  if (!reader_override_arms(c, id, cid, name, reader_ty)) return 0;
  int base_mi = comp_method_in_chain(c, cid, name, NULL);
  TyKind ret = comp_ntype(c, id);
  int argsNode = nt_ref(nt, id, "arguments");
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, ret, b);
  buf_printf(b, " _t%d; switch (", rtmp);
  emit_obj_dispatch_key(c, cid, selfptr, b);
  buf_puts(b, ") {");
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid || !is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi0 = comp_method_in_chain(c, k, name, &kd);
    if (kmi0 < 0 || kmi0 == base_mi) continue;
    nd_callee(c, g_nd_call_id, kmi0, kd, 1);
    buf_printf(b, " case %d: ", k);
    emit_dispatch_arm_call(c, kd, dispatch_arm_scope(c, kmi0), selfptr, argsNode, -1,
                           ret, ret, rtmp, b);
  }
  buf_printf(b, " default: _t%d = ", rtmp);
  if (ret == TY_POLY && reader_ty != TY_POLY) emit_boxed_text(c, reader_ty, reader, b);
  else buf_puts(b, reader);
  buf_printf(b, "; break; } _t%d; })", rtmp);
  return 1;
}

/* Emit a (possibly virtual) method call. `selfptr` is a reusable C
   expression yielding sp_<static>* (e.g. "self", "&lv_x", "&_t3"). Args
   are pre-evaluated into temps so they're emitted once.
   `blk_node` is the BlockNode id of the attached block, or -1 if none. */
/* An Object, Array, Hash or Numeric reopening: its instance methods take
   `sp_RbVal self` (emit_method_signature), any value, with no struct of the
   class's own to cast it to. */
static int reopen_takes_boxed_self(Compiler *c, int cid) {
  const char *cn = cid >= 0 ? c->classes[cid].c_name : NULL;
  return cn && (sp_streq(cn, "Object") || sp_streq(cn, "Array") ||
                sp_streq(cn, "Hash") || sp_streq(cn, "Numeric"));
}

void emit_dispatch(Compiler *c, int cid, const char *name,
                          const char *selfptr, int argsNode, int blk_node, Buf *b) {
  const NodeTable *nt = c->nt;
  int defcls = cid;
  /* the target is the plan of the call being emitted (g_nd_call_id) when
     this dispatch is that call's own name: the node's own plan when it is
     cid's lookup, otherwise the plan read with cid for its self or
     receiver (an instance_exec self, a body emitted for an inheriting
     class, a poly arm). A dispatch under another name (an operator answered
     through another method, a to_ary probe), and under --plan-check as the
     assertion, looks the name up itself */
  CallPlan dpc;
  const CallPlan *dpl = NULL;
  int mi = -1, served = 0;
  if (g_nd_call_id >= 0) {
    const char *cnm = nt_str(nt, g_nd_call_id, "name");
    if (cnm && sp_streq(cnm, name)) {
      dpc = *cplan_user(c, g_nd_call_id);
      if (!(dpc.chain && dpc.via == UC_INST && dpc.owner_ci == cid))
        dpc = *cplan_user_in(c, g_nd_call_id, cid,
                             g_ie_class_id == cid ? CPX_IE : g_emitting_class_id == cid ? CPX_EMIT : CPX_ARM);
      served = 1;
      if (dpc.chain && dpc.via == UC_INST && dpc.owner_ci == cid) {
        mi = dpc.mi;
        if (mi >= 0) defcls = c->scopes[mi].class_id;
        dpl = &dpc;
      }
      if (g_plan_check) cplan_served("dispatch");
    }
    else if (cnm) {
      /* an operator the plan answers through another of the class's methods
         (`!=` through `==`, the comparisons through `<=>`): the dispatch of
         that method is the plan's */
      dpc = *cplan_user(c, g_nd_call_id);
      if (!dpc.chain && dpc.via == UC_INST && dpc.owner_ci == cid && dpc.mi >= 0 &&
          c->scopes[dpc.mi].name && sp_streq(c->scopes[dpc.mi].name, name)) {
        served = 1;
        mi = dpc.mi;
        defcls = c->scopes[mi].class_id;
        dpl = &dpc;
        if (g_plan_check) cplan_served("dispatch");
      }
    }
  }
  if (g_plan_check || !served) {
    int odef = cid;
    int omi = comp_method_in_chain(c, cid, name, &odef);
    if (!served) {
      if (g_plan_check && omi >= 0)
        fprintf(stderr, "plan-check: cplan-fallback: dispatch node %d %s\n", g_nd_call_id, name);
      mi = omi; defcls = odef;
    }
    else if (omi != mi || odef != defcls)
      fprintf(stderr, "plan-check: cplan-conflict: dispatch node %d %s: plan %d/%d, lookup %d/%d\n",
              g_nd_call_id, name, mi, defcls, omi, odef);
  }
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  /* An alias shares the definition's function, so `__callee__` in the body can
     only learn the spelled name from here (#3729). */
  if (m && m->name && !sp_streq(m->name, name) && scope_reads_callee(c, mi)) {
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "sp_callee_name = ");
    emit_str_literal(g_pre, name);
    buf_puts(g_pre, ";\n");
  }
  TyKind ret = m ? m->ret : TY_UNKNOWN;
  /* Unify return type across all descendant implementations so that even
     when the base method has TY_VOID/TY_UNKNOWN, a subclass override
     with a real return type makes the dispatch virtual and typed. A
     yielding override answers what this call's block makes it answer, as
     inference typed the call (dispatch_ret_over), when the dispatch is the
     call's own. */
  {
    const char *cnm = g_nd_call_id >= 0 ? nt_str(nt, g_nd_call_id, "name") : NULL;
    ret = dispatch_ret_over(c, cid, name, 0, mi, ret,
                            cnm && sp_streq(cnm, name) ? g_nd_call_id : -1);
  }
  /* A yielding method answers what this call's block makes it answer, which
     its scope's return type (the last splice's) does not say: the arms are its
     proc-form clones, whose boxed value the switch unboxes into the call's type */
  if (m && m->yields && blk_node >= 0 && g_nd_call_id >= 0 &&
      nt_ref(nt, g_nd_call_id, "block") == blk_node &&
      scope_proc_form_of(c, mi) >= 0) {
    TyKind ct = comp_ntype(c, g_nd_call_id);
    if (ct != TY_UNKNOWN) ret = ct;
  }

  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* Force a runtime switch when there is no base implementation (m == NULL):
     a template method defined only in subclasses cannot be called directly as
     sp_<base>_<name>, so even a single descendant impl must dispatch virtually. */
  /* the form -- one method, a switch, a switch whose arms lay the arguments
     out each for itself -- is the plan's (cplan_dispatch_form); without
     one (a dispatch under another name the plan has no word for), and
     under --plan-check as the assertion, it is counted here */
  int form = dpl ? dpl->dispatch : -1;
  if (!dpl || g_plan_check) {
    int impl_n = dispatch_impl_count(c, cid, name);
    int sw = impl_n > 1 || (!m && impl_n >= 1);
    int oform = !sw ? (m ? CP_DIRECT : CP_NONE) : dispatch_arms_disagree(c, cid, name) ? CP_PER_ARM : CP_SWITCH;
    if (!dpl) form = oform;
    else if (form != oform)
      fprintf(stderr, "plan-check: cplan-conflict: dispatch-form node %d %s: plan %d, counted %d (%d implementations)\n",
              g_nd_call_id, name, form, oform, impl_n);
  }
  /* A void/nil-returning method that subclasses override must still dispatch on
     the runtime class -- an implicit-self call to it from a base method (e.g.
     `def run; validate; end` where each subclass overrides `validate`) would
     otherwise bind statically to the base impl and skip the override (#1443).
     The GCC statement-expression wrapper can't declare a `void` result, so a
     void dispatch uses a dummy int temp (its value is discarded). */
  int ret_is_void = (ret == TY_VOID || ret == TY_NIL);
  TyKind disp_ret = ret_is_void ? TY_INT : ret;
  /* the switch, for a return it can carry */
  int virtual = (is_scalar_ret(ret) || ret_is_void) && form >= CP_SWITCH;
  nd_stamp(g_nd_call_id, virtual ? ND_SWITCH : ND_DIRECT);
  if (!virtual && m) nd_callee(c, g_nd_call_id, mi, defcls, 0);
  if (virtual && form == CP_PER_ARM) {
    emit_dispatch_per_arm(c, cid, name, selfptr, argsNode, blk_node, mi, defcls, ret, disp_ret, b);
    return;
  }

  /* The parameters the shared temps are laid out for: the base method's,
     or for a method only subclasses define the first arm's, which every arm
     takes alike (dispatch_arms_disagree). Laid out as the call's positionals
     given, a rest arm got them one by one, and the C call did not build. */
  Scope *pm = m;
  for (int k = 0; !pm && k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kmi = comp_method_in_chain(c, k, name, NULL);
    if (kmi >= 0 && scope_has_callable_symbol(c, kmi)) pm = &c->scopes[kmi];
  }
  /* Arity check, the free-function path's own: an over- or under-supplied
     instance call went through with the extra arguments simply dropped
     (#3677). A count only a splat knows is measured with the splat below. */
  int argov_saved_d = g_n_argov;
  /* the count is judged here, and with the splat below, unless an argument
     is forwarded or a block argument, or a parameter is synthesized --
     every __-prefixed one, a __bam_ wrapper's too: a receiver-bound
     wrapper's first one is its receiver, not an argument */
  int judged_d = pm && (argv || argc == 0);
  for (int k = 0; k < argc && argv && judged_d; k++) {
    const char *at = nt_type(nt, argv[k]);
    if (at && (sp_streq(at, "ForwardingArgumentsNode") || sp_streq(at, "BlockArgumentNode")))
      judged_d = 0;
  }
  for (int i = 0; judged_d && i < pm->nparams; i++)
    if (pm->pnames[i] && pm->pnames[i][0] == '_' && pm->pnames[i][1] == '_') judged_d = 0;
  if (judged_d) emit_call_arity_check(c, pm, argc, argv);
  /* `callee(...)`: forward the enclosing `def foo(...)` method's synthesized
     __fwd_* params positionally (#1288), same as the emit_args_filled path. */
  Scope *fwd_encl = NULL;
  int fwd_base_d = 0;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      sp_streq(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    fwd_encl = comp_scope_of(c, argv[0]);
    /* skip the enclosing method's leading concrete params (`def f(a, ...)`) --
       only the __fwd_ slots are forwarded. */
    if (fwd_encl) {
      while (fwd_base_d < fwd_encl->nparams &&
             (!fwd_encl->pnames[fwd_base_d] ||
              strncmp(fwd_encl->pnames[fwd_base_d], "__fwd_", 6) != 0)) fwd_base_d++;
      if (fwd_base_d >= fwd_encl->nparams) fwd_base_d = 0;
    }
  }
  /* separate keyword-hash arg */
  int kwh_d = -1, pos_argc_d = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh_d = argv[argc - 1]; pos_argc_d = argc - 1;
  }
  /* Which argument, splat element or gathered element each parameter takes
     is the layout's (arg_layout), as it is a direct call's: this path renders
     it into its shared temps and decides nothing itself. It laid the static
     arm out on its own -- a splat spread in place into a rest target only for
     a lone `obj.m(*x)` into a callee whose shortfall rest_shortfall_required
     judges, every post left to its default beside a splat, a post index and a
     cap for the parameters ahead of the rest of its own -- so `o.m(*[])`
     against `def m(p1, *r, k1: 70)` bound the whole array to p1 where `m(*[])`
     raises, and `o.m(lg(1), *a)` against `def m(x, y, *r)` bound a to y. */
  ArgLayout L;
  arg_layout(c, pm, argv, pos_argc_d, kwh_d, 0, &L);
  /* Materialize a forwarded `**hash` inside kwh_d so keyword params extract
     from it and a `**kwrest` callee param collects it -- the same handling
     emit_args_filled applies (previously this path dropped every keyword
     into a NULL kwrest and let positionals steal keys by name). */
  int kw_merged_d = kwh_merged(c, pm, kwh_d);
  if (pm && (L.kw.args_first || kwh_runs_ahead(c, pm, kwh_d))) emit_args_run(c, argv, argc);
  /* the arguments to the left of a splat spread in place run ahead of it */
  else if (!emit_args_before_binding(c, pm, argv, argc, g_pre) && L.splat > 0)
    emit_args_in_source_order(c, argv, L.splat, g_pre);
  TyKind ds_type_d = TY_UNKNOWN;
  int ds_tmp_d = (pm && kwh_d >= 0) ? emit_ds_hash_materialize(c, pm, kwh_d, &ds_type_d) : -1;
  int np = pm ? pm->nparams : pos_argc_d;
  /* evaluate each param value (provided arg or default) into a temp so the
     virtual-dispatch cases reuse them without re-evaluating */
  int *atmp = np ? malloc(sizeof(int) * np) : NULL;
  /* C type each atmp[k] temp was declared with (the base method's param type).
     A subclass override may declare the same param differently (e.g. base
     `(Symbol)` vs override `(untyped)` -> sp_RbVal), so each dispatch arm coerces
     the shared temp to ITS param type instead of passing it raw (#3214). */
  TyKind *atmp_ty = np ? malloc(sizeof(TyKind) * np) : NULL;
  const char *saved_self = g_self;
  /* The splat the parameters read: every positional gathered, or the one
     spread in place. A trailing one's count is measured where the array is
     and refused once the other arguments have run: CRuby evaluates them all
     before it judges the count. */
  int splat_tmp_d = -1; TyKind splat_at_d = TY_UNKNOWN;
  int given_d = -1;
  if (L.gather) {
    splat_at_d = TY_POLY_ARRAY;
    splat_tmp_d = emit_splat_gather(c, pm, argv, &L);
  }
  else if (L.splat >= 0) {
    splat_tmp_d = emit_splat_in_place(c, argv[L.splat], &splat_at_d);
    if (judged_d) given_d = emit_splat_given_count(c, pm, &L, splat_tmp_d);
  }
  else
    for (int k = 0; judged_d && pm->rest_idx < 0 && argv && k < pos_argc_d; k++) {
      if (nt_kind(nt, argv[k]) != NK_SplatNode) continue;
      if (k >= pm->nparams) emit_unreached_splat_count(c, pm, argv, argc, pos_argc_d, &L.kw);
      break;
    }
  /* the keywords a `**` brings, once the count has been judged: CRuby
     counts first, so with such a hash the count is refused here, where the
     arguments have run ahead of the `**` (kw_plan's args_first) */
  if (given_d >= 0 && ds_tmp_d >= 0) {
    char gvn[32]; snprintf(gvn, sizeof gvn, "_t%d", given_d);
    emit_positional_count_check(c, pm, gvn);
    given_d = -1;
  }
  emit_ds_kwarg_check(c, pm, kwh_d, ds_tmp_d, ds_type_d);
  /* A default that reads an earlier parameter (`def g(u, v = u.upcase)`)
     evaluates in the callee, where that parameter is bound; here it is filled
     at the call site. emit_args_filled hoists every parameter into a named
     temp and renames the callee's parameter to it; this path already has the
     per-parameter temps (_tN), so it aliases each one under the rename's
     spelling and registers the rename. Without it the default emitted the
     callee's `lv_u`, which nothing at the call site declared (#4431). Same
     restriction as the other path: a *rest and a **kwrest are temps like the
     rest, and are aliased too. A gather's parameters are temps as well, and
     so is a keyword read out of a `**`'s hash (emit_ds_param_extract), its
     default under the renames: leaving a call with a `**` out emitted
     `C.new.m(1, **h)` into `def m(p1, p2 = p1, **kw)` against `lv_p1`. */
  int pd_ren_base = g_nren, pd_uid = 0;
  int pd_active = pm && (splat_tmp_d < 0 || L.gather) && default_refs_earlier_param(c, pm);
  if (pd_active) pd_uid = ++g_tmp;
  for (int k = 0; k < np; k++) {
    atmp[k] = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    LocalVar *p = pm ? scope_local(pm, pm->pnames[k]) : NULL;
    if (pm && pm->rest_idx >= 0 && k == pm->rest_idx) {
      /* rest param: pack the arguments the layout leaves it into a PolyArray,
         an unconsumed keyword hash at its tail (#3503), stopping before the
         posts. */
      int rest_end_d = L.rest_argc - pm->npost_rest;
      /* the packed arguments are the caller's: no parameter renames */
      int rest_nren_sv = g_nren;
      g_nren = pd_ren_base;
      if (L.from[k] == ARG_GATHERED)
        emit_gathered_param(c, pm, k, splat_tmp_d, &ab);
      else if (L.splat >= 0)
        emit_rest_from_splat_and_argv(splat_tmp_d, splat_at_d, k - L.splat,
                                      c, L.splat + 1, rest_end_d, argv, &ab);
      else
        emit_rest_pack_kwh(c, k, rest_end_d, argv, L.rest_kwh, &ab);
      g_nren = rest_nren_sv;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", atmp[k], ab.p ? ab.p : "sp_PolyArray_new()");
      /* The packed rest is a fresh array in a plain C temporary: a splat's
         packing roots its accumulator only while it builds it, and the other
         packing's shortcuts (a lone typed splat converted whole, an empty
         rest) hand back an allocation with no root at all. Root it whenever
         the call still has something to evaluate -- a parameter after the
         rest, or a block literal -- because each of those allocates, and a
         collected rest is recycled straight into the next array. */
      if (k < np - 1 || blk_node >= 0) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]);
      }
      atmp_ty[k] = TY_POLY_ARRAY;
      if (pd_active && pm->pnames[k] && g_nren < MAX_RENAME) {
        /* a later default reading the rest (`k: r.size`) reads this temp */
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *lv__pd%d_%d = _t%d; (void)lv__pd%d_%d;\n",
                   pd_uid, k, atmp[k], pd_uid, k);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", pm->pnames[k]);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_pd%d_%d", pd_uid, k);
        g_nren++;
      }
    }
    else if (fwd_encl && fwd_base_d + k < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[fwd_base_d + k]);
      TyKind et = ep ? ep->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[fwd_base_d + k]);
      if (p && (p->type == TY_POLY || p->type == TY_UNKNOWN) && et != TY_POLY) emit_boxed_text(c, et, txt, &ab);
      else buf_puts(&ab, txt);
      TyKind att = p ? (p->type == TY_UNKNOWN ? TY_POLY : p->type) : et;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, att, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
      atmp_ty[k] = att;
      if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      free(ab.p);
      continue;
    }
else {
      ArgFrom from = pm ? L.from[k] : ARG_NODE;
      /* Only a true KEYWORD param consumes a key -- a positional param whose
         name happens to match must take the provided positional instead
         (mirrors emit_args_filled). */
      int is_kwp_d = pm && pm->pnames[k] && callee_has_kwarg(c, pm, pm->pnames[k]);
      int by_splat = from == ARG_GATHERED || from == ARG_ELEM;
      int kv = (pm && kwh_d >= 0 && is_kwp_d && !kw_merged_d && !by_splat)
                 ? kwh_lookup(nt, kwh_d, pm->pnames[k]) : -1;
      /* no implementation to lay out for: the slots are the call's
         positionals as given (#4514) */
      int provided = kv >= 0 ? kv
                   : !pm ? (k < pos_argc_d ? argv[k] : -1)
                   : from == ARG_NODE ? argv[L.arg[k]]
                   : from == ARG_KWH ? kwh_d : -1;
      if (pm && pm->kwrest_idx >= 0 && k == pm->kwrest_idx) {
        /* `**kwrest` callee param: collect the call's unbound keywords --
           the caller's expressions, so with no parameter renames */
        int kr_nren_sv = g_nren;
        g_nren = pd_ren_base;
        int krhash = emit_kwrest_collect(c, pm, kwh_d, ds_tmp_d, ds_type_d, argsNode);
        g_nren = kr_nren_sv;
        buf_printf(&ab, "_t%d", krhash);
      }
      else if (!by_splat && kv < 0 && ds_tmp_d >= 0 && is_kwp_d) {
        /* keyword param fed by a forwarded `**hash`: extract by name. The
           default a missing key falls back to reads the callee's self, as
           the defaults below do: `def m(k1: @d)` called `C.new.m(**h)` read
           the caller's @d, and at the top level named no self at all. */
        const char *saved_deref5 = g_self_deref;
        int saved_emcls5 = g_emitting_class_id;
        g_self = selfptr;
        g_self_deref = comp_ty_value_obj(c, ty_object(cid)) ? "." : "->";
        g_emitting_class_id = pm->class_id;
        emit_ds_param_extract(c, pm, k, ds_tmp_d, ds_type_d, &ab);
        g_self = saved_self;
        g_self_deref = saved_deref5;
        g_emitting_class_id = saved_emcls5;
      }
      else if (by_splat) {
        /* a lent leading argument is the caller's: its self, no renames */
        int lead_nren_sv = g_nren;
        g_nren = pd_ren_base;
        int lent = L.gather && emit_gather_lead_lent(c, pm, k, argv, argc, &ab);
        g_nren = lead_nren_sv;
        /* from the gather or the splat spread in place; a default past its
           end reads the callee's self, as the defaults below do: `def m(a =
           @x, c)` read the caller's */
        const char *saved_deref4 = g_self_deref;
        int saved_emcls4 = g_emitting_class_id;
        g_self = selfptr;
        g_self_deref = comp_ty_value_obj(c, ty_object(cid)) ? "." : "->";
        g_emitting_class_id = pm->class_id;
        if (lent) {}
        else if (from == ARG_GATHERED) emit_gathered_param(c, pm, k, splat_tmp_d, &ab);
        else emit_elem_param(c, pm, k, L.arg[k], splat_tmp_d, splat_at_d, L.gather, &ab);
        g_self = saved_self;
        g_self_deref = saved_deref4;
        g_emitting_class_id = saved_emcls4;
      }
      else {
        /* Default expressions (e.g. `@ivar * 10`) reference the callee's self and
           callee's class, not the caller's. Temporarily redirect both. A value-
           type receiver is a by-value struct, so its ivars dereference with `.`,
           not `->` (without this, `def m(r = @r)` emits `selfval->iv_r`). */
        int saved_emcls2 = g_emitting_class_id;
        const char *saved_deref3 = g_self_deref;
        if (provided < 0) {
          g_self = selfptr;
          g_self_deref = comp_ty_value_obj(c, ty_object(cid)) ? "." : "->";
          if (pm) g_emitting_class_id = pm->class_id;
        }
        /* a provided argument is the caller's expression: a caller local that
           happens to share a parameter's name must not resolve to the temp */
        int pd_nren_sv = g_nren;
        if (provided >= 0) g_nren = pd_ren_base;
        /* no implementation: there is no parameter list to read a default
           or a coercion from, so the argument is the caller's expression as
           written (#4514) */
        if (!pm) { if (provided >= 0) emit_expr(c, provided, &ab); else buf_puts(&ab, "0"); }
        else emit_arg_or_default(c, pm, k, provided, &ab);
        g_nren = pd_nren_sv;
        g_self = saved_self;
        g_self_deref = saved_deref3;
        g_emitting_class_id = saved_emcls2;
      }
      TyKind att = p ? p->type : comp_ntype(c, k < argc ? argv[k] : -1);
      if (p && att == TY_UNKNOWN) att = TY_POLY;  /* poly in the callee signature */
      atmp_ty[k] = att;
      /* A byref out-param takes the SLOT's address, so its temp is a
         pointer to a possibly volatile String slot, not the parameter's
         own type. */
      if (p && p->byref_out) {
        emit_indent(g_pre, g_indent);
        buf_puts(g_pre, borrowed_string_type(p));
        buf_printf(g_pre, " *_t%d = ", atmp[k]);
        buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
        free(ab.p);
        if (pd_active && pm->pnames[k] && g_nren < MAX_RENAME) {
          /* the lent address under the cell spelling a reading default emits */
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "%s *_cell__pd%d_%d = _t%d;\n", borrowed_string_type(p), pd_uid, k, atmp[k]);
          snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", pm->pnames[k]);
          snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_pd%d_%d", pd_uid, k);
          g_nren++;
        }
        continue;
      }
      /* an argument the call ran first (emit_args_before_binding) is its
         rooted temp already, when the slot takes it unconverted */
      int ran_t = provided >= 0 && comp_ntype(c, provided) == att
                    ? ran_first_temp(provided, argov_saved_d, ab.p) : -1;
      if (ran_t >= 0) atmp[k] = ran_t;
      else {
        emit_indent(g_pre, g_indent);
        emit_ctype(c, att, g_pre);
        buf_printf(g_pre, " _t%d = ", atmp[k]);
        buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
        /* Root heap-typed arg temps: evaluating a later argument may allocate
           and collect an earlier one still sitting in its temp. */
        if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
        else if (needs_root(att)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]); }
      }
      if (pd_active && pm->pnames[k] && g_nren < MAX_RENAME) {
        /* alias the temp (already rooted) under the rename's spelling, and
           register the rename AFTER it so only a LATER default reads it */
        emit_indent(g_pre, g_indent);
        emit_ctype(c, att, g_pre);
        buf_printf(g_pre, " lv__pd%d_%d = _t%d; (void)lv__pd%d_%d;\n", pd_uid, k, atmp[k], pd_uid, k);
        char pdn[48]; snprintf(pdn, sizeof pdn, "_pd%d_%d", pd_uid, k);
        emit_pd_cell_alias(c, p, pdn);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", pm->pnames[k]);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_pd%d_%d", pd_uid, k);
        g_nren++;
      }
    }
    free(ab.p);
  }
  g_nren = pd_ren_base;   /* the renames served the defaults only */
  view_unbind(argov_saved_d);

  /* a trailing splat's count, refused once every argument has run */
  if (given_d >= 0) {
    char gvn[32]; snprintf(gvn, sizeof gvn, "_t%d", given_d);
    emit_positional_count_check(c, pm, gvn);
  }
  /* &block param that escapes: pre-evaluate the block as sp_Proc * temp.
     When the call site has no block, blk_tmp stays -1 and we pass NULL. */
  int blk_tmp = -1;
  int needs_blk_arg = m && m->blk_param && m->blk_param[0] && !m->yields;
  if (needs_blk_arg) blk_node = resolve_forwarded_block(c, blk_node);
  if (needs_blk_arg && blk_node >= 0) blk_tmp = emit_blk_proc_tmp(c, blk_node);

  /* The aliased name may differ from the defining method's real name. */
  const char *mname = m ? m->name : name;

  if (!virtual) {
    /* a value-type receiver is passed by value (no pointer cast); so is
       the self of an Object / Array / Hash / Numeric reopening, which its
       methods take boxed (emit_method_signature) and which has no struct */
    if (comp_ty_value_obj(c, ty_object(cid)) || reopen_takes_boxed_self(c, defcls))
      buf_printf(b, "sp_%s_%s(%s", c->classes[defcls].c_name, mc(mname), selfptr);
    else
      buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int k = 0; k < np; k++) buf_printf(b, ", _t%d", atmp[k]);
    if (needs_blk_arg) {
      if (blk_tmp >= 0) buf_printf(b, ", _t%d", blk_tmp);
      else buf_puts(b, ", NULL");
    }
    buf_puts(b, ")");
    free(atmp); free(atmp_ty); arg_layout_free(&L);
    return;
  }

  /* runtime dispatch on cls_id (GCC statement-expression) */
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, disp_ret, b);
  buf_printf(b, " _t%d; switch (", rtmp);
  emit_obj_dispatch_key(c, cid, selfptr, b);
  buf_puts(b, ") {");
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi < 0) continue;
    /* A `case` arm calling an override with no standalone definition (DCE-pruned
       or yield-inlined) would reference an absent symbol and dangle at link; the
       class can't be the receiver here anyway. Same guard as the poly-dispatch
       loops in codegen_call.c (issue #1583). */
    if (!scope_has_callable_symbol(c, kmi)) continue;
    nd_callee(c, g_nd_call_id, kmi, kd, 1);
    TyKind arm_ret = (TyKind)c->scopes[kmi].ret;
    const char *kfn = mc(c->scopes[kmi].name);
    if (method_is_void(&c->scopes[kmi])) {
      /* override emitted as a void C function (method_is_void: VOID/NIL/UNKNOWN
         ret, or initialize) -- call it, assign nil/zero to the result temp */
      buf_printf(b, " case %d: sp_%s_%s((sp_%s *)%s", k,
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) emit_arm_arg(c, &c->scopes[kmi], a, atmp[a], atmp_ty[a], b);
      buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
    }
    else if (arm_ret != ret && ret == TY_POLY) {
      /* arm returns a concrete type but switch expects sp_RbVal: box it */
      buf_printf(b, " case %d: { ", k);
      Buf _bx; memset(&_bx, 0, sizeof _bx);
      buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) emit_arm_arg(c, &c->scopes[kmi], a, atmp[a], atmp_ty[a], &_bx);
      buf_puts(&_bx, ")");
      buf_printf(b, "_t%d = ", rtmp);
      emit_boxed_text(c, arm_ret, _bx.p ? _bx.p : "0", b);
      free(_bx.p);
      buf_puts(b, "; break; }");
    }
    else {
      buf_printf(b, " case %d: _t%d = sp_%s_%s((sp_%s *)%s", k, rtmp,
                 c->classes[kd].c_name, kfn, c->classes[kd].c_name, selfptr);
      for (int a = 0; a < np; a++) emit_arm_arg(c, &c->scopes[kmi], a, atmp[a], atmp_ty[a], b);
      buf_puts(b, "); break;");
    }
  }
  /* When the method is defined only in descendants (m == NULL), the base class
     has no implementation. The default arm is unreachable (self is always a
     descendant that has the method), so emit a typed placeholder rather than a
     call to a nonexistent sp_<base>_<name>. */
  if (!m) {
    buf_printf(b, " default: _t%d = %s; break; } _t%d; })", rtmp,
               ret == TY_POLY ? "sp_box_nil()" : default_value(disp_ret), rtmp);
    free(atmp); free(atmp_ty); arg_layout_free(&L);
    return;
  }
  /* default arm uses the base-class (defcls) implementation */
  TyKind def_ret = (TyKind)m->ret;
  if (method_is_void(m)) {
    buf_printf(b, " default: sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
  }
  else if (def_ret != ret && ret == TY_POLY) {
    buf_printf(b, " default: { ");
    Buf _bx; memset(&_bx, 0, sizeof _bx);
    buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(&_bx, ", _t%d", atmp[a]);
    buf_puts(&_bx, ")");
    buf_printf(b, "_t%d = ", rtmp);
    emit_boxed_text(c, def_ret, _bx.p ? _bx.p : "0", b);
    free(_bx.p);
    buf_puts(b, "; break; }");
  }
  else {
    buf_printf(b, " default: _t%d = sp_%s_%s((sp_%s *)%s", rtmp,
               c->classes[defcls].c_name, mc(mname), c->classes[defcls].c_name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_puts(b, "); break;");
  }
  buf_printf(b, " } _t%d; })", rtmp);
  free(atmp); free(atmp_ty); arg_layout_free(&L);
}



/* True if `recv` names the constant `name` either as a bare ConstantReadNode
   or as a root-anchored absolute path `::name` (ConstantPathNode, no parent). */
int recv_is_const(const NodeTable *nt, int recv, const char *name) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (sp_streq(rty, "ConstantReadNode") ||
      (sp_streq(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *n = nt_str(nt, recv, "name");
    return n && sp_streq(n, name);
  }
  return 0;
}

int sp_is_fiber_storage_recv(const NodeTable *nt, int recv) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (sp_streq(rty, "ConstantReadNode") ||
      (sp_streq(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *rn = nt_str(nt, recv, "name");
    return rn && sp_streq(rn, "Fiber");
  }
  if (sp_streq(rty, "CallNode")) {
    const char *rn = nt_str(nt, recv, "name");
    int rr = nt_ref(nt, recv, "receiver");
    if (!rn || !sp_streq(rn, "current") || rr < 0) return 0;
    const char *rrty = nt_type(nt, rr);
    const char *rrn = nt_str(nt, rr, "name");
    return rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber");
  }
  return 0;
}

/* `Klass.new(args) { block }` where Klass#initialize yields: the constructor
   only allocates (a yielding initialize is never emitted), so inline the
   initialize body at the call site with self bound to the fresh object and the
   literal block feeding its yields -- the same per-call-site specialization as
   ordinary yield-method inlining. Returns 1 if handled. */
