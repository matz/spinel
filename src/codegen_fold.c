#include "codegen_internal.h"

void emit_method_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int mi = comp_method_index(c, name);
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  buf_printf(b, "sp_%s(", mc(name));
  emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), "", b);
  /* pass &block as sp_Proc * when the callee has a blk_param and isn't inlined */
  if (m && m->blk_param && m->blk_param[0] && !m->yields) {
    int blk_node = nt_ref(nt, id, "block");
    int wrote_args = m->nparams > 0;
    if (wrote_args) buf_puts(b, ", ");
    if (blk_node >= 0) {
      int blk_tmp = ++g_tmp;
      Buf pb; memset(&pb, 0, sizeof pb);
      emit_proc_literal(c, blk_node, &pb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
      free(pb.p);
      buf_printf(b, "_t%d", blk_tmp);
    }
    else {
      buf_puts(b, "NULL");
    }
  }
  buf_puts(b, ")");
}

int patch_lv_reads(Compiler *c, int id, const char *nm, TyKind ty,
                           int *ids_out, TyKind *ty_out, int cap);

/* hash.map / collect { |k, v| ... } as an expression -> an array of the block
   values, built via a loop over the hash entries in the statement prelude. */
int emit_hash_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
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
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  (void)kt;

  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;

  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;

  /* Detect type mismatch: block params unified with other blocks in same scope.
     For 2-param |k,v|: p0=key, p1=value. For 1-param |v|: p0=value. */
  Scope *pscope = comp_scope_of(c, block);
  TyKind p0_actual = p1_orig ? kt : vt;   /* p0 is key if 2 params, else value */
  TyKind p1_actual = vt;
  LocalVar *p0_lv = p0_orig ? scope_local(pscope, p0_orig) : NULL;
  LocalVar *p1_lv = p1_orig ? scope_local(pscope, p1_orig) : NULL;
  TyKind p0_decl = p0_lv ? p0_lv->type : TY_UNKNOWN;
  TyKind p1_decl = p1_lv ? p1_lv->type : TY_UNKNOWN;
  int ns0 = p0_orig && p0_actual != TY_UNKNOWN && p0_decl != TY_UNKNOWN && p0_decl != p0_actual;
  int ns1 = p1_orig && p1_actual != TY_UNKNOWN && p1_decl != TY_UNKNOWN && p1_decl != p1_actual;
  int st0 = -1, sri0 = -1, srn0 = 0; char sro0[112]; sro0[0] = '\0';
  int st1 = -1, sri1 = -1, srn1 = 0; char sro1[112]; sro1[0] = '\0';

  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p); }

  if (is_sel || is_rej) {
    /* select/reject: produce a same-type hash with matching pairs */
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new();\n", tres, hn);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    if (p0_orig) {
      emit_indent(g_pre, g_indent + 1);
      if (ns0) {
        st0 = ++g_tmp; emit_ctype(c, p0_actual, g_pre);
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, " lv__bp%d = _t%d->keys[_t%d->order[_t%d]];\n", st0, trecv, trecv, ti);
        else
          buf_printf(g_pre, " lv__bp%d = _t%d->order[_t%d];\n", st0, trecv, ti);
        for (int ri = 0; ri < g_nren; ri++) {
          if (strcmp(g_ren_from[ri], p0_orig) == 0) {
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
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(g_pre, "lv_%s = _t%d->keys[_t%d->order[_t%d]];\n", p0, trecv, trecv, ti);
        else
          buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", p0, trecv, ti);
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
          if (strcmp(g_ren_from[ri], p1_orig) == 0) {
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
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    int save = g_indent; g_indent++;
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
    if (ns0 && p0_lv) p0_lv->type = p0_decl;
    if (ns1 && p1_lv) p1_lv->type = p1_decl;
    if (sri1 >= 0) { if (srn1) g_nren = sri1; else strncpy(g_ren_to[sri1], sro1, sizeof g_ren_to[0]-1); }
    if (sri0 >= 0) { if (srn0) g_nren = sri0; else strncpy(g_ren_to[sri0], sro0, sizeof g_ren_to[0]-1); }
    emit_indent(g_pre, g_indent + 1);
    TyKind vtt = ty_hash_val(rt);
    buf_printf(g_pre, "if (%s(%s)) { ", is_rej ? "!" : "", vb.p ? vb.p : "0"); free(vb.p);
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
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    if (p0_orig) {
      emit_indent(g_pre, g_indent + 1);
      if (ns0) {
        st0 = ++g_tmp; emit_ctype(c, p0_actual, g_pre);
        if (p1_orig) {
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(g_pre, " lv__bp%d = _t%d->keys[_t%d->order[_t%d]];\n", st0, trecv, trecv, ti);
          else
            buf_printf(g_pre, " lv__bp%d = _t%d->order[_t%d];\n", st0, trecv, ti);
        }
        else {
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(g_pre, " lv__bp%d = _t%d->vals[_t%d->order[_t%d]];\n", st0, trecv, trecv, ti);
          else
            buf_printf(g_pre, " lv__bp%d = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", st0, hn, trecv, trecv, ti);
        }
        for (int ri = 0; ri < g_nren; ri++) {
          if (strcmp(g_ren_from[ri], p0_orig) == 0) {
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
        if (p1_orig) {
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(g_pre, "lv_%s = _t%d->keys[_t%d->order[_t%d]];\n", p0, trecv, trecv, ti);
          else
            buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", p0, trecv, ti);
        }
        else {
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", p0, trecv, trecv, ti);
          else
            buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", p0, hn, trecv, trecv, ti);
        }
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
          if (strcmp(g_ren_from[ri], p1_orig) == 0) {
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
    /* Temporarily set scope LocalVar types so infer_type reads the shadow type.
       This ensures both c->ntype cache and emit_call see the correct operand types. */
    if (ns0 && p0_lv) p0_lv->type = p0_actual;
    if (ns1 && p1_lv) p1_lv->type = p1_actual;
    TyKind bret = infer_type(c, bb[bn - 1]);
    if (bret == TY_UNKNOWN) bret = (ns1 ? p1_actual : p0_actual);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
    int save = g_indent; g_indent++;
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
    if (ns0 && p0_lv) p0_lv->type = p0_decl;
    if (ns1 && p1_lv) p1_lv->type = p1_decl;
    if (sri1 >= 0) { if (srn1) g_nren = sri1; else strncpy(g_ren_to[sri1], sro1, sizeof g_ren_to[0]-1); }
    if (sri0 >= 0) { if (srn0) g_nren = sri0; else strncpy(g_ren_to[sri0], sro0, sizeof g_ren_to[0]-1); }
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
    if (res_poly && bret != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, bret, vb.p ? vb.p : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_puts(g_pre, vb.p ? vb.p : "");
    buf_puts(g_pre, ");\n"); free(vb.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  buf_printf(b, "_t%d", tres);
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
  if (node_ty && !strcmp(node_ty, "LocalVariableReadNode")) {
    const char *vname = nt_str(c->nt, id, "name");
    if (vname && !strcmp(vname, nm)) {
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
    if (!ty || strcmp(ty, "LocalVariableReadNode")) continue;
    if (c->nscope[i] != scope_idx) continue;
    const char *nm = nt_str(c->nt, i, "name");
    if (!nm || strcmp(nm, name)) continue;
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
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "transform_keys") && strcmp(name, "transform_values"))) return 0;
  int keys = !strcmp(name, "transform_keys");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  const char *shn = ty_hash_cname(rt);
  if (!shn) return 0;
  TyKind dt = comp_ntype(c, id);
  const char *dhn = ty_hash_cname(dt);
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
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td2, shn, td2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti2, ti2, ts2, ti2);
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
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv preludes flush to g_pre first */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, dt, g_pre); buf_printf(g_pre, " _t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", td, dhn, td);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, ts, ti);
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
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  TyKind dkt = ty_hash_key(dt);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_%sHash_set(_t%d, ", dhn, td);
  if (keys) {
    /* new key = block result; unbox if block returned poly but key type is typed */
    const char *vbp = vb.p ? vb.p : "0";
    if (dkt == TY_STRING && (bret == TY_POLY || bret == TY_UNKNOWN))
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
    buf_printf(g_pre, "_t%d, ", tk);
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
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "bsearch")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_RANGE) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int tr = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_Range _t%d = ", tr); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = _t%d.first;\n", tlo, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = _t%d.last - _t%d.excl;\n", thi, tr, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tmid); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = save;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = _t%d - 1; }\n", cb.p ? cb.p : "0", tres, tmid, thi, tmid); free(cb.p);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* array.max_by / min_by { |x| key } -> the element with the largest/smallest
   (int/float) key. Loop in the statement prelude; value is the best element. */
/* Emit `src` (a poly sp_RbVal C-expression) coerced to scalar type `dst`. */
static void flatmap_coerce_from_poly(TyKind dst, const char *src, Buf *out) {
  if (dst == TY_INT || dst == TY_BOOL) buf_printf(out, "sp_poly_to_i(%s)", src);
  else if (dst == TY_FLOAT) buf_printf(out, "sp_poly_to_f(%s)", src);
  else buf_puts(out, src);  /* poly (or other): pass through */
}

/* `array.flat_map { |params| block-returning-array }` as an expression: map each
   element through the block and concatenate the returned arrays (flatten one
   level). Handles a single block param (bound to the element) and a flat
   multi-param destructure of a poly-array element. Returns 1 if handled. */
int emit_flat_map_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "flat_map") && strcmp(name, "collect_concat"))) return 0;
  int block = nt_ref(nt, id, "block");
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *rk = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!rk) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* Identity block `{ |x| x }` over an array of arrays is a one-level flatten,
     equivalent to `flatten(1)`: each sub-array element is unboxed inline and
     scalars pass through. The block returns the bare element (a poly element,
     not statically an array), so the array-returning path below cannot handle
     it; emit the runtime flatten directly. */
  if (bn == 1 && rt == TY_POLY_ARRAY) {
    const char *p0 = block_param_name(c, block, 0);
    const char *sty = nt_type(nt, bb[0]);
    if (p0 && !block_param_name(c, block, 1) && sty &&
        !strcmp(sty, "LocalVariableReadNode") && nt_str(nt, bb[0], "name") &&
        !strcmp(nt_str(nt, bb[0], "name"), p0)) {
      buf_puts(b, "sp_PolyArray_flatten_n(");
      emit_expr(c, recv, b);
      buf_puts(b, ", 1)");
      return 1;
    }
  }
  TyKind bret = comp_ntype(c, bb[bn - 1]);
  if (!ty_is_array(bret)) return 0;  /* only array-returning blocks */
  const char *bk = (bret == TY_POLY_ARRAY) ? "Poly" : array_kind(bret);
  if (!bk) return 0;
  int np = 0; while (block_param_name(c, block, np)) np++;
  if (np > 1 && rt != TY_POLY_ARRAY) return 0;  /* destructure needs poly elements */
  TyKind et = ty_array_elem(rt);
  int ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", bk, tres, bk, tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, rk, ta, ti);
  if (np <= 1) {
    const char *p0 = block_param_name(c, block, 0);
    if (p0) {
      const char *p0r = rename_local(p0);
      LocalVar *lv = scope_local(comp_scope_of(c, block), p0);
      TyKind pt = lv ? lv->type : et;
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = ", p0r);
      char src[64]; snprintf(src, sizeof src, "sp_%sArray_get(_t%d, _t%d)", rk, ta, ti);
      if (pt == et) buf_puts(g_pre, src);
      else if (et == TY_POLY) { Buf cv; memset(&cv,0,sizeof cv); flatmap_coerce_from_poly(pt, src, &cv); buf_puts(g_pre, cv.p?cv.p:src); free(cv.p); }
      else buf_puts(g_pre, src);
      buf_puts(g_pre, ";\n");
    }
  }
  else {
    int te = ++g_tmp;
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);\n", te, ta, ti);
    for (int pj = 0; pj < np; pj++) {
      const char *pn = block_param_name(c, block, pj); if (!pn) break;
      const char *pnr = rename_local(pn);
      LocalVar *lv = scope_local(comp_scope_of(c, block), pn);
      TyKind pt = lv ? lv->type : TY_POLY;
      char src[96]; snprintf(src, sizeof src, "sp_poly_index_poly(_t%d, sp_box_int(%d))", te, pj);
      emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = ", pnr);
      Buf cv; memset(&cv, 0, sizeof cv); flatmap_coerce_from_poly(pt, src, &cv);
      buf_puts(g_pre, cv.p ? cv.p : src); free(cv.p); buf_puts(g_pre, ";\n");
    }
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int tbv = ++g_tmp, tj = ++g_tmp;
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, bret, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", tbv, vb.p ? vb.p : "NULL"); free(vb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
             tj, tj, bk, tbv, tj, bk, tres, bk, tbv, tj);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* recv.filter_map { |x| body } -- map, then drop falsy results. The result is a
   poly array (the body is often a nilable `expr if cond`); each truthy boxed
   value is kept. Output matches CRuby for both nilable and concrete bodies. */
int emit_filter_map_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "filter_map")) return 0;
  int block = nt_ref(nt, id, "block");
  int recv = nt_ref(nt, id, "receiver");
  if (block < 0 || !nt_type(nt, block) || strcmp(nt_type(nt, block), "BlockNode") || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;  /* range filter_map: a later slice */
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0 = block_param_name(c, block, 0);
  if (p0) p0 = rename_local(p0);

  int ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_expr(c, recv, &rb);
  TyKind et = ty_array_elem(rt);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, ta, ti);

  Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et && et != TY_UNKNOWN;
  int din = g_indent + 1;
  if (use_shadow) {
    clv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    emit_indent(g_pre, din); buf_puts(g_pre, "{\n"); din++;
    emit_indent(g_pre, din); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, ti);
  }
  else if (p0) { emit_indent(g_pre, din); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, ta, ti); }

  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, din);
  int save = g_indent; g_indent = din;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  TyKind vt = comp_ntype(c, bb[bn - 1]);
  int tv = ++g_tmp;
  emit_indent(g_pre, din); buf_printf(g_pre, "sp_RbVal _t%d = ", tv);
  if (vt == TY_POLY) buf_puts(g_pre, vb.p ? vb.p : "sp_box_nil()");
  else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, vt, vb.p ? vb.p : "", &bx); buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p); }
  buf_puts(g_pre, ";\n"); free(vb.p);
  emit_indent(g_pre, din); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tv);
  emit_indent(g_pre, din); buf_printf(g_pre, "if (sp_poly_truthy(_t%d)) sp_PolyArray_push(_t%d, _t%d);\n", tv, tres, tv);
  if (use_shadow) { din--; emit_indent(g_pre, din); buf_puts(g_pre, "}\n"); }
  if (clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tres);
  return 1;
}

int emit_minmax_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_max = !strcmp(name, "max_by"), is_min = !strcmp(name, "min_by");
  if (!is_max && !is_min) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind bvt = infer_type(c, bb[bn - 1]);
  if (bvt != TY_INT && bvt != TY_FLOAT && bvt != TY_POLY) return 0;
  int trecv = ++g_tmp, tbest = ++g_tmp, tbv = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tcur = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);  /* recv value; its own preludes flow to g_pre */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trecv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tbest, et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s; int _t%d = 1;\n", tbv, default_value(bvt), tf);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  Scope *mbsc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *mlv0 = (mbsc && p0) ? scope_local(mbsc, p0) : NULL;
  TyKind mpt0 = mlv0 ? mlv0->type : TY_UNKNOWN;
  if (mlv0) mlv0->type = et;
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  if (mlv0) mlv0->type = mpt0;
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, bvt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tcur, vb.p ? vb.p : default_value(bvt)); free(vb.p);
  emit_indent(g_pre, g_indent + 1);
  if (bvt == TY_POLY)
    buf_printf(g_pre, "if (_t%d || sp_poly_%s(_t%d, _t%d)) { _t%d = lv_%s; _t%d = _t%d; _t%d = 0; }\n",
               tf, is_max ? "gt" : "lt", tcur, tbv, tbest, p0 ? p0 : "", tbv, tcur, tf);
  else
    buf_printf(g_pre, "if (_t%d || _t%d %s _t%d) { _t%d = lv_%s; _t%d = _t%d; _t%d = 0; }\n",
               tf, tcur, is_max ? ">" : "<", tbv, tbest, p0 ? p0 : "", tbv, tcur, tf);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tbest);
  return 1;
}

/* poly `uniq`/`uniq!` with a block, as an expression: the receiver boxes an
   array; keep the first element for each distinct block-key value (compared with
   sp_poly_eq), and for the bang form write the survivors back in place. Yields
   the (boxed) array. Returns 1 if handled. */
int emit_poly_uniq_block(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "uniq") && strcmp(name, "uniq!"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  int block = nt_ref(nt, id, "block");
  if (recv < 0 || block < 0) return 0;
  if (comp_ntype(c, recv) != TY_POLY) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (argc != 0) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (!p0 || bn < 1) return 0;
  int bang = !strcmp(name, "uniq!");
  int trecv = ++g_tmp, tarr = ++g_tmp, tseen = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", trecv, rb.p ? rb.p : "sp_box_nil()"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = (sp_PolyArray *)_t%d.v.p;\n", tarr, trecv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tseen, tseen);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tarr, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _t%d->data[_t%d];\n", p0, tarr, ti);
  int save = g_indent; g_indent++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  int tkey = ++g_tmp, tdup = ++g_tmp, tj = ++g_tmp;
  Buf kb; memset(&kb, 0, sizeof kb); emit_boxed(c, bb[bn - 1], &kb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tkey, kb.p ? kb.p : "sp_box_nil()"); free(kb.p);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "int _t%d = 0; for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) if (sp_poly_eq(_t%d->data[_t%d], _t%d)) { _t%d = 1; break; }\n",
             tdup, tj, tj, tseen, tj, tseen, tj, tkey, tdup);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (!_t%d) { sp_PolyArray_push(_t%d, _t%d); sp_PolyArray_push(_t%d, lv_%s); }\n", tdup, tseen, tkey, tres, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (bang) {
    int tm = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_t%d->len = 0; for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) sp_PolyArray_push(_t%d, _t%d->data[_t%d]);\n",
               tarr, tm, tm, tres, tm, tarr, tres, tm);
    buf_printf(b, "_t%d", trecv);
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
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "gsub") && strcmp(name, "sub"))) return 0;
  int once = !strcmp(name, "sub");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || comp_ntype(c, recv) != TY_STRING) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc != 1) return 0;
  int reidx = re_lit_index(c, argv[0]);
  if (reidx < 0) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  int ts = ++g_tmp, tpos = ++g_tmp, tslen = ++g_tmp, tout = ++g_tmp,
      tm = ++g_tmp, tms = ++g_tmp, tme = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = ", ts); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tpos);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = (mrb_int)strlen(_t%d);\n", tslen, ts);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"\"); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tpos, tslen);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_match(sp_re_pat_%d, _t%d + _t%d);\n", tm, reidx, ts, tpos);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d < 0) { sp_String_append(_t%d, _t%d + _t%d); break; }\n", tm, tout, ts, tpos);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_caps[0];\n", tms);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "mrb_int _t%d = sp_re_caps[1];\n", tme);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, sp_str_substr(_t%d + _t%d, 0, _t%d));\n", tout, ts, tpos, tms);
  if (p0) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_str_substr(_t%d + _t%d, _t%d, _t%d - _t%d);\n", p0, ts, tpos, tms, tme, tms); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
  int save = g_indent; g_indent++;
  Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = save;
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, %s);\n", tout, vb.p ? vb.p : "\"\""); free(vb.p);
  if (once) {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_String_append(_t%d, _t%d + _t%d + _t%d); break;\n", tout, ts, tpos, tme);
  }
  else {
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d == _t%d) { if (_t%d + _t%d < _t%d) sp_String_append(_t%d, sp_str_substr(_t%d + _t%d, _t%d, 1)); _t%d += _t%d + 1; }\n",
               tme, tms, tpos, tme, tslen, tout, ts, tpos, tme, tpos, tme);
    emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d += _t%d; }\n", tpos, tme);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d->data", tout);
  return 1;
}

/* array.sum([init]) { |x| f(x) } as an expression: sum the block's result
   over every element. Returns 1 if handled. */
int emit_sum_block_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "sum")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind acct = comp_ntype(c, bb[bn - 1]);
  if (acct != TY_INT && acct != TY_FLOAT) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d); ", tn, k, ta);
  emit_ctype(c, acct, b); buf_printf(b, " _t%d = ", tacc);
  if (argc == 1) {
    TyKind init_t = comp_ntype(c, argv[0]);
    if (acct == TY_FLOAT && init_t == TY_INT) {
      buf_puts(b, "(mrb_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
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
  buf_printf(b, "; for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) { ", ti, ti, tn, ti);
  if (p0) buf_printf(b, "lv_%s = sp_%sArray_get(_t%d, _t%d); ", p0, k, ta, ti);
  /* The block's value expression may spill setup statements to g_pre (e.g.
     a nested count loop). Those must run per iteration: redirect g_pre into
     a local buffer while emitting the value, then splice it into the loop
     body ahead of the accumulation. */
  {
    Buf inner; memset(&inner, 0, sizeof inner);
    Buf valb; memset(&valb, 0, sizeof valb);
    Buf *saved_pre = g_pre; g_pre = &inner;
    if (acct == TY_INT) { buf_printf(&valb, "sp_int_add(_t%d, ", tacc); emit_expr(c, bb[bn - 1], &valb); buf_puts(&valb, ")"); }
    else { buf_printf(&valb, "_t%d + ", tacc); emit_expr(c, bb[bn - 1], &valb); }
    g_pre = saved_pre;
    if (inner.p) buf_puts(b, inner.p);
    buf_printf(b, "_t%d = %s", tacc, valb.p ? valb.p : "0");
    free(inner.p); free(valb.p);
  }
  buf_printf(b, "; } _t%d; })", tacc);
  return 1;
}

/* int_array.slice_when { |a, b| cond }[.to_a].inspect  or
   int_array.chunk { |x| key }[.to_a].inspect  ->  inspect string.
   Emits setup to g_pre and the result variable to b. Returns 1 if handled. */
int emit_slice_when_chunk_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow .to_a wrapper */
  if (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_sw = !strcmp(m, "slice_when");
  int is_ck = !strcmp(m, "chunk");
  if (!is_sw && !is_ck) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0 || comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
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
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "sp_IntArray_push(_t%d, lv_%s);\n", tcur, p0);
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
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tkeys, tkeys);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PtrArray *_t%d = sp_PtrArray_new(); SP_GC_ROOT(_t%d);\n", tgrps, tgrps);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = NULL;\n", tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _t%d = 0;\n", tpk);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
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
  buf_printf(g_pre, "mrb_int _tkey_%d = %s;\n", ta, kb.p ? kb.p : "0"); free(kb.p);
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
  buf_printf(g_pre, "sp_IntArray_push(_t%d, lv_%s);\n", tcur, p0);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* build inspect string */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"[\"); SP_GC_ROOT(_t%d);\n", tstr, tstr);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", tj, tj, tkeys, tj);
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

/* int_array.chunk_while { |a, b| cond }.to_a -> array of runs. Adjacent elements
   stay in one run while the block is true; a boundary falls where it is false
   (the inverse of slice_when). Materialized as a poly array of boxed int arrays
   so the result is first-class -- `p`, indexing, and further iteration work.
   Emits setup to g_pre and the result var to b. Returns 1 if handled. */
int emit_chunk_while_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "to_a")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m || strcmp(m, "chunk_while")) return 0;
  int block = nt_ref(nt, recv, "block");
  if (block < 0) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  if (pr < 0 || comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0n = block_param_name(c, block, 0);
  const char *p1n = block_param_name(c, block, 1);
  if (!p0n || !p1n) return 0;
  const char *p0 = rename_local(p0n), *p1 = rename_local(p1n);

  int ta = ++g_tmp, tout = ++g_tmp, tcur = ++g_tmp, ti = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, pr, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = %s;\n", ta, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ta);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", tcur, tcur);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {\n", ti, ti, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "lv_%s = sp_IntArray_get(_t%d, _t%d);\n", p0, ta, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_IntArray_push(_t%d, lv_%s);\n", tcur, p0);
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
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb); g_indent = save;
  if (lva) lva->type = pta;
  if (lvb) lvb->type = ptb;
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "if (!(%s)) {\n", cb.p ? cb.p : "0"); free(cb.p);
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

/* int_array.product(int_array)[.to_a].inspect -> the Cartesian product
   rendered as a nested-array string. The product result has no first-class
   type, so only this inline inspect chain is supported. Returns 1 if handled. */
int emit_product_inspect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "inspect")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  /* allow an intervening .to_a */
  if (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "to_a"))
    recv = nt_ref(nt, recv, "receiver");
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  const char *m = nt_str(nt, recv, "name");
  if (!m) return 0;
  int is_product = !strcmp(m, "product");
  int is_slice = !strcmp(m, "slice_before") || !strcmp(m, "slice_after");
  if (!is_product && !is_slice) return 0;
  int pr = nt_ref(nt, recv, "receiver");
  int pargs = nt_ref(nt, recv, "arguments");
  int pac = 0; const int *pav = pargs >= 0 ? nt_arr(nt, pargs, "arguments", &pac) : NULL;
  if (pr < 0 || pac != 1) return 0;
  if (comp_ntype(c, pr) != TY_INT_ARRAY) return 0;
  if (is_product) {
    /* the other operand is an int_array or an empty array literal */
    TyKind at = comp_ntype(c, pav[0]);
    int empty_lit = nt_type(nt, pav[0]) && !strcmp(nt_type(nt, pav[0]), "ArrayNode") &&
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
  if (!name || strcmp(name, "step")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (rt != TY_INT && rt != TY_FLOAT) return 0;
  int args = nt_ref(nt, id, "arguments");
  int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
  if (sc < 1) return 0;
  int is_float = (rt == TY_FLOAT) || comp_ntype(c, sv[0]) == TY_FLOAT ||
                 (sc >= 2 && comp_ntype(c, sv[1]) == TY_FLOAT);
  int tr = ++g_tmp, tl = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
  if (!is_float) {
    buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d); mrb_int _t%d = ", tr, tr, tl);
    emit_expr(c, sv[0], b); buf_printf(b, "; mrb_int _t%d = ", ts);
    if (sc >= 2) emit_expr(c, sv[1], b); else buf_puts(b, "1");
    buf_printf(b, "; for (mrb_int _t%d = ", ti); emit_expr(c, recv, b);
    buf_printf(b, "; _t%d >= 0 ? _t%d <= _t%d : _t%d >= _t%d; _t%d += _t%d) sp_IntArray_push(_t%d, _t%d); _t%d; })",
               ts, ti, tl, ti, tl, ti, ts, tr, ti, tr);
    return 1;
  }
  int tb = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_FloatArray *_t%d = sp_FloatArray_new(); SP_GC_ROOT(_t%d); mrb_float _t%d = ", tr, tr, tb);
  emit_expr(c, recv, b); buf_printf(b, "; mrb_float _t%d = ", tl); emit_expr(c, sv[0], b);
  buf_printf(b, "; mrb_float _t%d = ", ts);
  if (sc >= 2) emit_expr(c, sv[1], b); else buf_puts(b, "1.0");
  buf_printf(b, "; mrb_float _t%d_e = (fabs(_t%d)+fabs(_t%d)+fabs(_t%d-_t%d))/fabs(_t%d)*DBL_EPSILON;"
                " if (_t%d_e > 0.5) _t%d_e = 0.5;"
                " mrb_int _t%d = (mrb_int)floor((_t%d-_t%d)/_t%d + _t%d_e);"
                " for (mrb_int _t%d = 0; _t%d <= _t%d; _t%d++) sp_FloatArray_push(_t%d, _t%d + _t%d * _t%d); _t%d; })",
             tn, tb, tl, tl, tb, ts, tn, tn, tn, tl, tb, ts, tn, ti, ti, tn, ti, tr, tb, ti, ts, tr);
  return 1;
}

/* inject(:op) / reduce(:op) / inject(&:op) / inject(init, :op) as an
   expression: fold the array with a symbol-named arithmetic operator. The
   block-fold form (inject { |a, e| ... }) is not handled here. Returns 1 if
   handled. */
int emit_inject_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "inject") && strcmp(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* empty array literal `[]` has TY_UNKNOWN; treat as TY_INT_ARRAY */
  if (rt == TY_UNKNOWN && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    if (en == 0) rt = TY_INT_ARRAY;
  }
  /* `[[ints],...].inject(&:&|:||:-)`: fold the inner int arrays with a set op.
     Handled before the typed-array path since poly arrays have no array_kind. */
  if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
    int sblk = nt_ref(nt, id, "block");
    const char *sop = NULL;
    if (sblk >= 0 && nt_type(nt, sblk) && !strcmp(nt_type(nt, sblk), "BlockArgumentNode")) {
      int ex = nt_ref(nt, sblk, "expression");
      if (ex >= 0 && nt_type(nt, ex) && !strcmp(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
    }
    if (sop && (!strcmp(sop, "&") || !strcmp(sop, "|") || !strcmp(sop, "-"))) {
      const char *sfn = !strcmp(sop, "&") ? "sp_IntArray_intersect"
                      : !strcmp(sop, "|") ? "sp_IntArray_union" : "sp_IntArray_difference";
      int ta = ++g_tmp, tn = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = sp_PolyArray_length(_t%d); ", tn, ta);
      buf_printf(b, "sp_IntArray *_t%d = _t%d > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", tacc, tn, ta);
      buf_printf(b, "for (mrb_int _t%d = 1; _t%d < _t%d; _t%d++) _t%d = %s(_t%d, (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p); ",
                 ti, ti, tn, ti, tacc, sfn, tacc, ta, ti);
      buf_printf(b, "_t%d; })", tacc);
      return 1;
    }
  }
  if (!ty_is_array(rt)) return 0;
  const char *k = array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);

  /* find the operator symbol (from a &:op block or a trailing :op arg) and
     any explicit initial value */
  const char *op = NULL; int init = -1;
  int block = nt_ref(nt, id, "block");
  if (block >= 0 && nt_type(nt, block) && !strcmp(nt_type(nt, block), "BlockArgumentNode")) {
    int ex = nt_ref(nt, block, "expression");
    if (ex >= 0 && nt_type(nt, ex) && !strcmp(nt_type(nt, ex), "SymbolNode")) op = nt_str(nt, ex, "value");
  }
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (!op && argc >= 1) {
    int last = argv[argc - 1];
    if (nt_type(nt, last) && !strcmp(nt_type(nt, last), "SymbolNode")) {
      op = nt_str(nt, last, "value");
      if (argc == 2) init = argv[0];
    }
  }
  if (!op) return 0;

  const char *ifn = (et == TY_INT) ? int_arith_fn(op) : NULL;
  /* bitwise ops on integers: &, |, ^, <<, >> -- use operator directly */
  int int_bitop = (et == TY_INT) && !ifn &&
                  (!strcmp(op, "&") || !strcmp(op, "|") || !strcmp(op, "^") ||
                   !strcmp(op, "<<") || !strcmp(op, ">>"));
  int float_op = (et == TY_FLOAT) && (!strcmp(op, "+") || !strcmp(op, "-") ||
                                      !strcmp(op, "*") || !strcmp(op, "/"));
  int str_op = (et == TY_STRING) && !strcmp(op, "+");
  if (!ifn && !int_bitop && !float_op && !str_op) return 0;

  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
  buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
  buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d); ", tn, k, ta);
  emit_ctype(c, et, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); start = 0; }
  else { buf_printf(b, "_t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s", tn, k, ta, default_value(et)); start = 1; }
  buf_printf(b, "; for (mrb_int _t%d = %d; _t%d < _t%d; _t%d++) _t%d = ", ti, start, ti, tn, ti, tacc);
  if (ifn)
    buf_printf(b, "%s(_t%d, sp_%sArray_get(_t%d, _t%d))", ifn, tacc, k, ta, ti);
  else if (str_op)
    buf_printf(b, "sp_str_concat(_t%d, sp_%sArray_get(_t%d, _t%d))", tacc, k, ta, ti);
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
  if (!name || (strcmp(name, "inject") && strcmp(name, "reduce"))) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  /* `[[ints],...].inject { |a, b| a & b }`: the inner int arrays are boxed in a
     poly array; fold them as int arrays (unboxing each element). The poly array
     itself has no array_kind, so detect this before the typed-array bail. */
  int nested = (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv));
  const char *k = array_kind(rt);
  if (!k && !nested) return 0;
  if (nested) k = "Poly";  /* length via sp_PolyArray_length; elements unboxed below */
  TyKind et = nested ? TY_INT_ARRAY : ty_array_elem(rt);
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || strcmp(bty, "BlockNode")) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  if (!p0_orig || !p1_orig) return 0;
  const char *p0 = rename_local(p0_orig);
  const char *p1 = rename_local(p1_orig);
  int bbody = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
  if (bn == 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  int init = (argc > 0 && argv) ? argv[0] : -1;

  /* Accumulator type comes from the seed init when provided, else from the element type. */
  TyKind acc_ty = et;
  if (init >= 0) {
    TyKind it = comp_ntype(c, init);
    if (it != TY_UNKNOWN) acc_ty = it;
  }
  /* An int seed folded over floats accumulates float (matches the reduce
     return-type promotion in infer_type); keep the C accumulator type in step.
     Only a numeric body promotes -- a poly body keeps the seed type (codegen
     re-types the params and re-infers the body under the shadow below). */
  { TyKind bt = comp_ntype(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc_ty = ty_promote_numeric(acc_ty, bt); }
  int ta = ++g_tmp, tacc = ++g_tmp, ti = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, rt, b); buf_printf(b, " _t%d = ", ta); emit_expr(c, recv, b); buf_puts(b, "; ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " _t%d = ", tacc);
  int start;
  if (init >= 0) { emit_expr(c, init, b); buf_puts(b, "; "); start = 0; }
  else if (nested) { buf_printf(b, "sp_PolyArray_length(_t%d) > 0 ? (sp_IntArray *)sp_PolyArray_get(_t%d, 0).v.p : sp_IntArray_new(); ", ta, ta); start = 1; }
  else { buf_printf(b, "sp_%sArray_length(_t%d) > 0 ? sp_%sArray_get(_t%d, 0) : 0; ", k, ta, k, ta); start = 1; }
  /* Temporarily override block param types to match acc_ty/et so the body
     expression uses the correct C types (same pattern as emit_sort_cmp_expr). */
  Scope *rsc = comp_scope_of(c, block);
  LocalVar *rlv0 = rsc ? scope_local(rsc, p0_orig) : NULL;
  LocalVar *rlv1 = rsc ? scope_local(rsc, p1_orig) : NULL;
  TyKind rpt0 = rlv0 ? rlv0->type : TY_UNKNOWN;
  TyKind rpt1 = rlv1 ? rlv1->type : TY_UNKNOWN;
  if (rlv0) rlv0->type = acc_ty;
  if (rlv1) rlv1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh ntype cache */
  buf_printf(b, "for (mrb_int _t%d = %d; _t%d < sp_%sArray_length(_t%d); _t%d++) { ",
             ti, start, ti, k, ta, ti);
  buf_puts(b, "{ ");
  emit_ctype(c, acc_ty, b); buf_printf(b, " lv_%s = _t%d; ", p0, tacc);
  if (nested) { emit_ctype(c, et, b); buf_printf(b, " lv_%s = (sp_IntArray *)sp_PolyArray_get(_t%d, _t%d).v.p; ", p1, ta, ti); }
  else { emit_ctype(c, et, b); buf_printf(b, " lv_%s = sp_%sArray_get(_t%d, _t%d); ", p1, k, ta, ti); }
  for (int j = 0; j < bn - 1; j++) {
    emit_stmt(c, bb[j], b, 0);
    buf_puts(b, " ");
  }
  buf_printf(b, "_t%d = ", tacc); emit_expr(c, bb[bn - 1], b); buf_puts(b, "; } } ");
  buf_printf(b, "_t%d; })", tacc);
  if (rlv0) rlv0->type = rpt0;
  if (rlv1) rlv1->type = rpt1;
  return 1;
}

/* sort_by { |x| key } as an expression: a stable bubble sort of a copy of
   the receiver, ordering by the block's computed (scalar) key. Returns 1 if
   handled. */
int emit_sortby_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  if (strcmp(name, "sort_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  TyKind kt = comp_ntype(c, bb[bn - 1]);
  if (kt != TY_INT && kt != TY_FLOAT && kt != TY_STRING && kt != TY_POLY) return 0;  /* scalar or poly key */
  int trv = ++g_tmp, tr = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp, tka = ++g_tmp, tkb = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv);
  buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  /* copy so the receiver is left unsorted (sort_by is non-mutating) */
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = sp_%sArray_slice(_t%d, 0, sp_%sArray_length(_t%d));\n", tr, k, trv, k, trv);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1; _t%d++)\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1 - _t%d; _t%d++) {\n", tj, tj, tn, ti, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", ta, k, tr, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d + 1);\n", tb, k, tr, tj);
  int save = g_indent; g_indent += 2;
  /* key of _ta */
  if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, ta); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf ka; memset(&ka, 0, sizeof ka); emit_expr(c, bb[bn - 1], &ka);
  emit_indent(g_pre, g_indent); emit_ctype(c, kt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tka, ka.p ? ka.p : "0"); free(ka.p);
  /* key of _tb */
  if (p0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, tb); }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf kb; memset(&kb, 0, sizeof kb); emit_expr(c, bb[bn - 1], &kb);
  emit_indent(g_pre, g_indent); emit_ctype(c, kt, g_pre); buf_printf(g_pre, " _t%d = %s;\n", tkb, kb.p ? kb.p : "0"); free(kb.p);
  g_indent = save;
  emit_indent(g_pre, g_indent + 2);
  if (kt == TY_STRING) buf_printf(g_pre, "if (strcmp(_t%d, _t%d) > 0) {", tka, tkb);
  else if (kt == TY_POLY) { int tcmp = ++g_tmp; buf_printf(g_pre, "{ mrb_bool _tcmp%d = 0; if (sp_poly_cmp(_t%d, _t%d, &_tcmp%d) > 0 && _tcmp%d) {", tcmp, tka, tkb, tcmp, tcmp); }
  else buf_printf(g_pre, "if (_t%d > _t%d) {", tka, tkb);
  buf_printf(g_pre, " sp_%sArray_set(_t%d, _t%d, _t%d); sp_%sArray_set(_t%d, _t%d + 1, _t%d); }%s\n",
             k, tr, tj, tb, k, tr, tj, ta, kt == TY_POLY ? " }" : "");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  buf_printf(b, "_t%d", tr);
  return 1;
}

/* sort { |a, b| a <=> b } as an expression: stable bubble sort of a copy,
   ordered by the comparator block (which yields the <=> sign). Returns 1 if
   handled. */
int emit_sort_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_bang = !strcmp(name, "sort!");
  if (strcmp(name, "sort") && !is_bang) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (!p0 || !p1) return 0;
  p0 = rename_local(p0); p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 || infer_type(c, bb[bn - 1]) != TY_INT) return 0;
  int trv = ++g_tmp, tr = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, ta = ++g_tmp, tb = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  if (!is_bang) {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sArray_slice(_t%d, 0, sp_%sArray_length(_t%d));\n", tr, k, trv, k, trv);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tr);
  }
else {
    emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
    buf_printf(g_pre, " _t%d = _t%d;\n", tr, trv);  /* sort! operates on self */
  }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, tr);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1; _t%d++)\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d - 1 - _t%d; _t%d++) {\n", tj, tj, tn, ti, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", ta, k, tr, tj);
  emit_indent(g_pre, g_indent + 2); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d + 1);\n", tb, k, tr, tj);
  Scope *sbsc = comp_scope_of(c, block);
  LocalVar *slv0 = sbsc ? scope_local(sbsc, p0) : NULL;
  LocalVar *slv1 = sbsc ? scope_local(sbsc, p1) : NULL;
  TyKind spt0 = slv0 ? slv0->type : TY_UNKNOWN;
  TyKind spt1 = slv1 ? slv1->type : TY_UNKNOWN;
  if (slv0) slv0->type = et;
  if (slv1) slv1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh ntype cache */
  int save = g_indent; g_indent += 2;
  /* Shadow the outer (possibly poly) block params with et-typed locals */
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, ta);
  emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tb);
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  Buf cb; memset(&cb, 0, sizeof cb); emit_expr(c, bb[bn - 1], &cb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "if ((%s) > 0) { sp_%sArray_set(_t%d, _t%d, _t%d); sp_%sArray_set(_t%d, _t%d + 1, _t%d); }\n",
             cb.p ? cb.p : "0", k, tr, tj, tb, k, tr, tj, ta); free(cb.p);
  g_indent--; g_indent = save;
  emit_indent(g_pre, g_indent + 2); buf_puts(g_pre, "}\n");
  if (slv0) slv0->type = spt0;
  if (slv1) slv1->type = spt1;
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
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
    if (et == TY_INT)    buf_printf(b, "lv_%s = sp_box_int(_t%d);", nm, tidx);
    else if (et == TY_STRING) buf_printf(b, "lv_%s = sp_box_str(_t%d);", nm, tidx);
    else if (et == TY_FLOAT)  buf_printf(b, "lv_%s = sp_box_float(_t%d);", nm, tidx);
    else if (et == TY_BOOL)   buf_printf(b, "lv_%s = sp_box_bool(_t%d);", nm, tidx);
    else buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
else {
    buf_printf(b, "lv_%s = _t%d;", nm, tidx);
  }
}

/* min / max / minmax { |a, b| a <=> b } as an expression: a single scan
   tracking the extreme(s) under the comparator block. min/max yield one
   element; minmax yields a fresh [min, max]. Returns 1 if handled. */
int emit_minmax_cmp_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int is_min = !strcmp(name, "min"), is_max = !strcmp(name, "max"), is_mm = !strcmp(name, "minmax");
  if (!is_min && !is_max && !is_mm) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = infer_type(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  const char *p0 = block_param_name(c, block, 0);
  const char *p1 = block_param_name(c, block, 1);
  if (!p0 || !p1) return 0;
  p0 = rename_local(p0); p1 = rename_local(p1);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1 || infer_type(c, bb[bn - 1]) != TY_INT) return 0;
  int trv = ++g_tmp, tn = ++g_tmp, tmin = ++g_tmp, tmax = ++g_tmp, ti = ++g_tmp, te = ++g_tmp, tres = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", trv); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, k, trv);
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
  buf_printf(g_pre, " _t%d = _t%d > 0 ? sp_%sArray_get(_t%d, 0) : %s;\n", tmin, tn, k, trv, et == TY_RANGE ? "(sp_Range){0}" : default_value(et));
  emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = _t%d;\n", tmax, tmin);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "for (mrb_int _t%d = 1; _t%d < _t%d; _t%d++) {\n", ti, ti, tn, ti);
  emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_get(_t%d, _t%d);\n", te, k, trv, ti);
  /* Block params may be widened to TY_POLY across multiple block sites.
     Pin them to the element type for body emission:
     - temporarily set scope types to `et`
     - refresh ntype cache for body nodes (infer_type writes to cache)
     - emit C shadow declarations inside { } to give lv_p0/lv_p1 the right C type */
  Scope *bsc = comp_scope_of(c, block);
  LocalVar *lv_p0 = bsc ? scope_local(bsc, p0) : NULL;
  LocalVar *lv_p1 = bsc ? scope_local(bsc, p1) : NULL;
  TyKind saved_p0 = lv_p0 ? lv_p0->type : TY_UNKNOWN;
  TyKind saved_p1 = lv_p1 ? lv_p1->type : TY_UNKNOWN;
  if (lv_p0) lv_p0->type = et;
  if (lv_p1) lv_p1->type = et;
  for (int j = 0; j < bn; j++) infer_type(c, bb[j]);  /* refresh cache */
  int save = g_indent; g_indent++;
  if (is_min || is_mm) {
    /* Open C shadow scope with et-typed block param vars */
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, te);
    emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tmin);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
    Buf cm; memset(&cm, 0, sizeof cm); emit_expr(c, bb[bn - 1], &cm);
    g_indent--;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "if ((%s) < 0) _t%d = _t%d;\n", cm.p ? cm.p : "0", tmin, te); free(cm.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  if (is_max || is_mm) {
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n"); g_indent++;
    emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d; ", p0, te);
    emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tmax);
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
    Buf cx; memset(&cx, 0, sizeof cx); emit_expr(c, bb[bn - 1], &cx);
    g_indent--;
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "if ((%s) > 0) _t%d = _t%d;\n", cx.p ? cx.p : "0", tmax, te); free(cx.p);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  if (lv_p0) lv_p0->type = saved_p0;
  if (lv_p1) lv_p1->type = saved_p1;
  g_indent = save;
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (is_min) { buf_printf(b, "_t%d", tmin); return 1; }
  if (is_max) { buf_printf(b, "_t%d", tmax); return 1; }
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = sp_%sArray_new();\n", tres, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d > 0) { sp_%sArray_push(_t%d, _t%d); sp_%sArray_push(_t%d, _t%d); }\n", tn, k, tres, tmin, k, tres, tmax);
  buf_printf(b, "_t%d", tres);
  return 1;
}

/* partition { |x| ... } as an expression: emits two typed result arrays
   pushed into a PolyArray. Returns 1 if handled. */
int emit_partition_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "partition")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = infer_type(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;

  TyKind et = ty_array_elem(rt);
  Scope *psc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *plv0 = (psc && p0) ? scope_local(psc, p0) : NULL;
  TyKind psaved0 = plv0 ? plv0->type : TY_UNKNOWN;
  int use_shadow = plv0 && plv0->type != et && et != TY_UNKNOWN;
  if (use_shadow) {
    plv0->type = et;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  int trecv = ++g_tmp, ttrue = ++g_tmp, tfalse = ++g_tmp, ti = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);

  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, ttrue, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", ttrue);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tfalse, k);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tfalse);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
             ti, ti, k, trecv, ti);

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
  int saveIndent = g_indent; g_indent = innerIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);
  g_indent = saveIndent;

  emit_indent(g_pre, innerIndent);
  buf_printf(g_pre, "if (%s) sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
             vb.p ? vb.p : "0", k, ttrue, k, trecv, ti);
  emit_indent(g_pre, innerIndent);
  buf_printf(g_pre, "else sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n",
             k, tfalse, k, trecv, ti);
  free(vb.p);

  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  if (use_shadow && plv0) plv0->type = psaved0;

  int tres = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", tres);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);

  const char *box_fn = (rt == TY_INT_ARRAY) ? "sp_box_int_array"
                     : (rt == TY_STR_ARRAY) ? "sp_box_str_array"
                     : (rt == TY_FLOAT_ARRAY) ? "sp_box_float_array"
                     : "sp_box_poly_array";
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d));\n", tres, box_fn, ttrue);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(_t%d));\n", tres, box_fn, tfalse);

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* map/select/reject/filter as an expression: build a result array via a
   loop emitted into the statement prelude; the expression value is the
   temp array. Returns 1 if handled. */
int emit_collect_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (!name || recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  /* array.each_slice(n).map { |x, y, ...| } chain: unroll into a direct slice loop */
  if (ty_iter_shape(name) == TY_ITER_MAP &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_slice") &&
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
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "mrb_int _t%d = ", ts_es); emit_expr(c, es_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_es, tres_es, rk_es);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_es);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d += _t%d) {\n",
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
            for (int j = 0; j < bn_es - 1; j++) emit_stmt(c, bb_es[j], g_pre, g_indent + 1);
            int saveInd_es = g_indent; g_indent = g_indent + 1;
            Buf vb_es; memset(&vb_es, 0, sizeof vb_es); emit_expr(c, bb_es[bn_es - 1], &vb_es);
            g_indent = saveInd_es;
            TyKind bty_es = comp_ntype(c, bb_es[bn_es - 1]);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk_es, tres_es);
            if (res_poly_es && bty_es != TY_POLY) {
              Buf bx_es; memset(&bx_es, 0, sizeof bx_es);
              emit_boxed_text(c, bty_es, vb_es.p ? vb_es.p : "", &bx_es);
              buf_puts(g_pre, bx_es.p ? bx_es.p : ""); free(bx_es.p);
            }
            else buf_puts(g_pre, vb_es.p ? vb_es.p : "");
            buf_puts(g_pre, ");\n"); free(vb_es.p);
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
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_cons") &&
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
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "mrb_int _t%d = ", tn_ec); emit_expr(c, ec_argv[0], g_pre); buf_puts(g_pre, ";\n");
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_ec, tres_ec, rk_ec);
            emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_ec);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++) {\n",
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
              /* |a, b| flat multi-param: each element */
              for (int pj = 0; pj < np_ec; pj++) {
                const char *pn = block_param_name(c, block, pj); if (!pn) break;
                emit_indent(g_pre, g_indent + 1);
                buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d + %d);\n",
                           rename_local(pn), kec, ta_ec, ti_ec, pj);
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
            for (int j = 0; j < bn_ec - 1; j++) emit_stmt(c, bb_ec[j], g_pre, g_indent + 1);
            int saveInd_ec = g_indent; g_indent = g_indent + 1;
            Buf vb_ec; memset(&vb_ec, 0, sizeof vb_ec); emit_expr(c, bb_ec[bn_ec - 1], &vb_ec);
            g_indent = saveInd_ec;
            TyKind bty_ec = comp_ntype(c, bb_ec[bn_ec - 1]);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk_ec, tres_ec);
            if (res_poly_ec && bty_ec != TY_POLY) {
              Buf bx_ec; memset(&bx_ec, 0, sizeof bx_ec);
              emit_boxed_text(c, bty_ec, vb_ec.p ? vb_ec.p : "", &bx_ec);
              buf_puts(g_pre, bx_ec.p ? bx_ec.p : ""); free(bx_ec.p);
            }
            else buf_puts(g_pre, vb_ec.p ? vb_ec.p : "");
            buf_puts(g_pre, ");\n"); free(vb_ec.p);
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
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && !strcmp(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && !strcmp(nt_str(nt, wi_recv, "name"), "each_cons") &&
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
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = ", tn_wi); emit_expr(c, ec_argv2[0], g_pre); buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = ", toff_wi);
              if (wi_argc > 0 && wi_argv) { emit_expr(c, wi_argv[0], g_pre); }
              else { buf_puts(g_pre, "0"); }
              buf_puts(g_pre, ";\n");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk_wi, tres_wi, rk_wi);
              emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "mrb_int _t%d = _t%d;\n", tidx_wi, toff_wi);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d + _t%d - 1 < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
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
                  emit_indent(g_pre, g_indent + 1);
                  buf_printf(g_pre, "lv_%s = sp_%sArray_slice(_t%d, _t%d, _t%d);\n",
                             rename_local(pair_p_wi), kwi, ta_wi, ti_wi, tn_wi);
                }
              }
              for (int j = 0; j < bn_wi - 1; j++) emit_stmt(c, bb_wi[j], g_pre, g_indent + 1);
              int saveInd_wi = g_indent; g_indent = g_indent + 1;
              Buf vb_wi; memset(&vb_wi, 0, sizeof vb_wi); emit_expr(c, bb_wi[bn_wi - 1], &vb_wi);
              g_indent = saveInd_wi;
              TyKind bty_wi = comp_ntype(c, bb_wi[bn_wi - 1]);
              emit_indent(g_pre, g_indent + 1);
              buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk_wi, tres_wi);
              if (res_poly_wi && bty_wi != TY_POLY) {
                Buf bx_wi; memset(&bx_wi, 0, sizeof bx_wi);
                emit_boxed_text(c, bty_wi, vb_wi.p ? vb_wi.p : "", &bx_wi);
                buf_puts(g_pre, bx_wi.p ? bx_wi.p : ""); free(bx_wi.p);
              }
              else buf_puts(g_pre, vb_wi.p ? vb_wi.p : "");
              buf_puts(g_pre, ");\n"); free(vb_wi.p);
              emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
              buf_printf(b, "_t%d", tres_wi);
              return 1;
            }
          }
        }
      }
    }
  }

  if (ty_is_hash(rt)) return emit_hash_collect_expr(c, id, b);
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
    if (bn2 < 1) return 0;
    int trecv2 = ++g_tmp, tn2 = ++g_tmp, tres2 = ++g_tmp, ti2 = ++g_tmp;
    Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = ", trecv2);
    buf_puts(g_pre, rb2.p ? rb2.p : "sp_box_nil()");
    buf_puts(g_pre, ";\n");
    free(rb2.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_int _t%d = sp_poly_arr_len(_t%d);\n", tn2, trecv2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk2, tres2, rk2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres2);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {\n",
               ti2, ti2, tn2, ti2);
    /* pin block param to TY_POLY via shadow declaration */
    Scope *csc2 = p0p ? comp_scope_of(c, block) : NULL;
    LocalVar *clv2 = (csc2 && p0p) ? scope_local(csc2, p0p) : NULL;
    TyKind csaved2 = clv2 ? clv2->type : TY_UNKNOWN;
    if (clv2) clv2->type = TY_POLY;
    for (int j2 = 0; j2 < bn2; j2++) infer_type(c, bb2[j2]);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "{\n");
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "sp_RbVal lv_%s = sp_poly_arr_get_hash(_t%d, _t%d);\n",
               p0p ? p0p : "_dummy", trecv2, ti2);
    for (int j2 = 0; j2 < bn2 - 1; j2++) emit_stmt(c, bb2[j2], g_pre, g_indent + 2);
    int saveIndent2 = g_indent; g_indent = g_indent + 2;
    Buf vb2; memset(&vb2, 0, sizeof vb2); emit_expr(c, bb2[bn2 - 1], &vb2);
    g_indent = saveIndent2;
    TyKind bty2 = comp_ntype(c, bb2[bn2 - 1]);
    emit_indent(g_pre, g_indent + 2);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk2, tres2);
    if (res_poly2 && bty2 != TY_POLY) {
      Buf bx2; memset(&bx2, 0, sizeof bx2);
      emit_boxed_text(c, bty2, vb2.p ? vb2.p : "", &bx2);
      buf_puts(g_pre, bx2.p ? bx2.p : ""); free(bx2.p);
    }
    else {
      buf_puts(g_pre, vb2.p ? vb2.p : "");
    }
    buf_puts(g_pre, ");\n");
    free(vb2.p);
    emit_indent(g_pre, g_indent + 1);
    buf_puts(g_pre, "}\n");
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "}\n");
    if (clv2) clv2->type = csaved2;
    buf_printf(b, "_t%d", tres2);
    return 1;
  }
  if (!ty_is_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k) return 0;

  TyIterShape shp = ty_iter_shape(name);
  int is_map = shp == TY_ITER_MAP;
  int is_sel = shp == TY_ITER_SELECT;
  int is_rej = shp == TY_ITER_REJECT;
  if (!is_map && !is_sel && !is_rej) return 0;

  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = res_poly ? "Poly" : array_kind(restype);
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
      buf_printf(g_pre, "mrb_int _t%d = _t%d.last - _t%d.excl - _t%d.first + 1; if (_t%d < 0) _t%d = 0;\n",
                 tlen, tr, tr, tr, tlen, tlen);
    }
    else {
      buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(%s);\n", tlen, k, rb0.p ? rb0.p : "NULL");
    }
    free(rb0.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres0, tres0);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) sp_PolyArray_push(_t%d, sp_box_nil());\n",
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
    buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
    buf_printf(&rb, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", tr, tr, tr);
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
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);

  TyKind et_elem = ty_array_elem(rt);
  /* If the block param's scope type was widened (e.g. TY_POLY), pin it to
     the element type and use a C shadow declaration so body emission sees the
     right type. */
  Scope *csc = p0 ? comp_scope_of(c, block) : NULL;
  LocalVar *clv0 = (csc && p0) ? scope_local(csc, p0) : NULL;
  TyKind csaved0 = clv0 ? clv0->type : TY_UNKNOWN;
  int use_shadow = clv0 && clv0->type != et_elem && et_elem != TY_UNKNOWN;
  if (use_shadow) {
    clv0->type = et_elem;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
  }

  int bodyIndent = g_indent + 1;
  int innerIndent = use_shadow ? bodyIndent + 1 : bodyIndent;
  if (use_shadow) {
    emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "{\n");
    emit_indent(g_pre, innerIndent); emit_ctype(c, et_elem, g_pre);
    buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
else if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  /* body statements except the last */
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);

  int saveIndent = g_indent;
  g_indent = innerIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);  /* value preludes flow to g_pre at innerIndent */
  g_indent = saveIndent;

  TyKind body_ty = comp_ntype(c, bb[bn - 1]);
  if (is_map) {
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
    /* a poly result array stores boxed values */
    if (res_poly && body_ty != TY_POLY) {
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, body_ty, vb.p ? vb.p : "", &bx);
      buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
    }
    else buf_puts(g_pre, vb.p ? vb.p : "");
    buf_puts(g_pre, ");\n");
  }
  else {
    emit_indent(g_pre, innerIndent);
    buf_printf(g_pre, "if (%s(", is_rej ? "!" : "");
    buf_puts(g_pre, vb.p ? vb.p : ""); buf_puts(g_pre, ")) ");
    buf_printf(g_pre, "sp_%sArray_push(_t%d, lv_%s);\n", rk, tres, p0 ? p0 : "");
  }
  free(vb.p);
  if (use_shadow) { emit_indent(g_pre, bodyIndent); buf_puts(g_pre, "}\n"); }
  if (use_shadow && clv0) clv0->type = csaved0;
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

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
  if (!name || strcmp(name, "with_index")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");  /* the blockless inner enumerator */
  if (recv < 0 || !nt_type(nt, recv) || strcmp(nt_type(nt, recv), "CallNode")) return 0;
  if (nt_ref(nt, recv, "block") >= 0) return 0;
  const char *inner = nt_str(nt, recv, "name");
  if (!inner) return 0;
  int is_each = !strcmp(inner, "each");
  TyIterShape shp = ty_iter_shape(inner);  /* map/select/reject; NONE for each */
  if (!is_each && shp == TY_ITER_NONE) return 0;
  int arr_recv = nt_ref(nt, recv, "receiver");
  if (arr_recv < 0) return 0;
  TyKind rt = comp_ntype(c, arr_recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;

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

  TyKind restype = comp_ntype(c, id);
  int res_poly = (restype == TY_POLY_ARRAY);
  const char *rk = collecting ? (res_poly ? "Poly" : array_kind(restype)) : NULL;
  if (collecting && !rk) rk = "Int";

  int trecv = ++g_tmp, ti = ++g_tmp, tidx = ++g_tmp;
  int tres = collecting ? ++g_tmp : 0;

  /* evaluate the source array once (its preludes land in g_pre first) */
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, arr_recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  /* running index, seeded with the offset */
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = ", tidx);
  if (wi_argc > 0 && wi_argv) emit_expr(c, wi_argv[0], g_pre); else buf_puts(g_pre, "0");
  buf_puts(g_pre, ";\n");
  if (collecting) {
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", rk, tres, rk);
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  }
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++, _t%d++) {\n",
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
  if (is_each) {
    for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
  }
  else {
    for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, innerIndent);
    int saveInd = g_indent; g_indent = innerIndent;
    Buf vb; memset(&vb, 0, sizeof vb); emit_expr(c, bb[bn - 1], &vb); g_indent = saveInd;
    if (is_map) {
      TyKind body_ty = comp_ntype(c, bb[bn - 1]);
      emit_indent(g_pre, innerIndent); buf_printf(g_pre, "sp_%sArray_push(_t%d, ", rk, tres);
      if (res_poly && body_ty != TY_POLY) {
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, body_ty, vb.p ? vb.p : "", &bx);
        buf_puts(g_pre, bx.p ? bx.p : ""); free(bx.p);
      }
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

  if (collecting) buf_printf(b, "_t%d", tres);
  else buf_printf(b, "_t%d", trecv);  /* each.with_index yields the receiver */
  return 1;
}

/* all?/any?/none?/one? with a block: loop, count the truthy block results,
   and reduce to the predicate. Returns 1 if handled. */
int emit_predicate_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int is_all = !strcmp(name, "all?"), is_any = !strcmp(name, "any?"),
      is_none = !strcmp(name, "none?"), is_one = !strcmp(name, "one?");
  if (!(is_all || is_any || is_none || is_one)) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  int range_recv = (rt == TY_RANGE);
  if (!ty_is_array(rt) && !range_recv) return 0;
  const char *k = range_recv ? "Int" : (rt == TY_POLY_ARRAY ? "Poly" : array_kind(rt));
  if (!k) return 0;
  int body = nt_ref(nt, block, "body");
  int bn = 0;
  const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  /* the block's last expression is the C condition: require a bool. A bare
     `if (value)` would mis-handle Ruby truthiness for other types (0 / 0.0
     are truthy in Ruby but false in C), so leave those unsupported. */
  if (comp_ntype(c, bb[bn - 1]) != TY_BOOL) return 0;

  const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
  int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb);
  if (range_recv) {
    int tr = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
    buf_printf(&rb, "sp_IntArray_from_range(_t%d.first, _t%d.last - _t%d.excl)", tr, tr, tr);
    rt = TY_INT_ARRAY;
  }
  else emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", trecv);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  int bodyIndent = g_indent + 1;
  if (p0) {
    emit_indent(g_pre, bodyIndent);
    buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
  }
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, bodyIndent);
  int saveIndent = g_indent;
  g_indent = bodyIndent;
  Buf vb; memset(&vb, 0, sizeof vb);
  emit_expr(c, bb[bn - 1], &vb);
  g_indent = saveIndent;
  emit_indent(g_pre, bodyIndent);
  buf_printf(g_pre, "if (%s) _t%d++;\n", vb.p ? vb.p : "0", tcnt);
  free(vb.p);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  if (is_all) buf_printf(b, "(_t%d == sp_%sArray_length(_t%d))", tcnt, k, trecv);
  else if (is_any) buf_printf(b, "(_t%d > 0)", tcnt);
  else if (is_none) buf_printf(b, "(_t%d == 0)", tcnt);
  else buf_printf(b, "(_t%d == 1)", tcnt);
  return 1;
}

/* Emit the `pattern === elem` membership test for grep, given the element
   bound to C variable `ev`. Returns 1 if the pattern kind is supported. */
int emit_grep_pred(Compiler *c, int pat, const char *ev, Buf *b) {
  const NodeTable *nt = c->nt;
  int re = re_lit_index(c, pat);
  if (re >= 0) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, %s)", re, ev); return 1; }
  const char *pty = nt_type(nt, pat);
  if (pty && !strcmp(pty, "RangeNode")) {
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = ", tr); emit_expr(c, pat, b);
    buf_printf(b, "; sp_range_include(&_t%d, %s); })", tr, ev);
    return 1;
  }
  if (pty && !strcmp(pty, "ConstantReadNode")) {
    const char *cn = nt_str(nt, pat, "name");
    if (!cn) return 0;
    if (!strcmp(cn, "Integer") || !strcmp(cn, "Fixnum")) buf_printf(b, "(%s).tag == SP_TAG_INT", ev);
    else if (!strcmp(cn, "String"))   buf_printf(b, "(%s).tag == SP_TAG_STR", ev);
    else if (!strcmp(cn, "Float"))    buf_printf(b, "(%s).tag == SP_TAG_FLT", ev);
    else if (!strcmp(cn, "Symbol"))   buf_printf(b, "(%s).tag == SP_TAG_SYM", ev);
    else if (!strcmp(cn, "Numeric"))  buf_printf(b, "((%s).tag == SP_TAG_INT || (%s).tag == SP_TAG_FLT)", ev, ev);
    else return 0;
    return 1;
  }
  return 0;
}

/* grep(pattern) / grep_v(pattern) without a block: collect elements for which
   `pattern === e` holds (or fails, for grep_v). Returns 1 if handled. */
int emit_grep_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || (strcmp(name, "grep") && strcmp(name, "grep_v"))) return 0;
  if (nt_ref(nt, id, "block") >= 0) return 0;   /* block form unsupported */
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc != 1) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  int pat = argv[0];

  /* probe predicate support before emitting anything */
  Buf probe; memset(&probe, 0, sizeof probe);
  if (!emit_grep_pred(c, pat, "_e", &probe)) { free(probe.p); return 0; }
  free(probe.p);

  int neg = !strcmp(name, "grep_v");
  TyKind et = ty_array_elem(rt);
  int trecv = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;

  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new();\n", k, tres, k);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tres);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n", ti, ti, k, trecv, ti);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "%s _t%d = sp_%sArray_get(_t%d, _t%d);\n", c_type_name(et), te, k, trecv, ti);
  emit_indent(g_pre, g_indent + 1);
  char ev[16]; snprintf(ev, sizeof ev, "_t%d", te);
  buf_printf(g_pre, "if (%s(", neg ? "!" : "");
  emit_grep_pred(c, pat, ev, g_pre);
  buf_printf(g_pre, ")) sp_%sArray_push(_t%d, _t%d);\n", k, tres, te);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "}\n");

  buf_printf(b, "_t%d", tres);
  return 1;
}

/* Emit the value for callee param `idx`: the provided arg node if any,
   else the param's default (a nil default becomes the type's default). */
void emit_arg_or_default(Compiler *c, Scope *m, int idx, int provided, Buf *out) {
  LocalVar *p = scope_local(m, m->pnames[idx]);
  TyKind pt = p ? p->type : TY_INT;
  if (provided >= 0) {
    if (pt == TY_POLY) emit_boxed(c, provided, out);   /* box into a poly param */
    else {
      TyKind at = comp_ntype(c, provided);
      /* Bare call inside a class/module body: analyze may not have resolved the
         type because g_cbody_class_id is not set during fixpoint. Look it up now. */
      if (at == TY_UNKNOWN && g_class_body_id >= 0) {
        const char *ptn = nt_type(c->nt, provided);
        if (ptn && !strcmp(ptn, "CallNode") && nt_ref(c->nt, provided, "receiver") < 0) {
          const char *bn = nt_str(c->nt, provided, "name");
          int bsmi = bn ? comp_cmethod_in_chain(c, g_class_body_id, bn, NULL) : -1;
          if (bsmi >= 0) at = (TyKind)c->scopes[bsmi].ret;
        }
      }
      /* empty array literal `[]` defaults to IntArray in emit_expr; if the
         parameter expects a different array type, emit the right constructor */
      int nen = 0;
      const char *pty_node = nt_type(c->nt, provided);
      int is_empty_arr = pty_node && !strcmp(pty_node, "ArrayNode") &&
                         (nt_arr(c->nt, provided, "elements", &nen), nen == 0);
      if (is_empty_arr && ty_is_array(pt) && pt != TY_INT_ARRAY) {
        if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
        else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, provided, out); }
      }
      /* empty hash literal `{}` with unknown type: emit the param's hash constructor.
         Must be checked before the poly-unbox path below, as at==TY_UNKNOWN for {}.  */
      else {
        int phn = 0;
        int is_empty_hash = pty_node && (!strcmp(pty_node, "HashNode") || !strcmp(pty_node, "KeywordHashNode")) &&
                             (nt_arr(c->nt, provided, "elements", &phn), phn == 0);
        if (is_empty_hash && at == TY_UNKNOWN && ty_is_hash(pt)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) { buf_printf(out, "sp_%sHash_new()", hn); return; }
        }
        /* When the param is a typed hash pointer but the caller passes a poly
           or nil value (e.g. an uninit ivar), extract .v.p from the RbVal.
           sp_box_nil() stores v.i=0 so .v.p is NULL, which hash getters handle
           safely via their NULL guards. */
        if (ty_is_hash(pt) && (at == TY_POLY || at == TY_NIL || at == TY_UNKNOWN)) {
          const char *hn = ty_hash_cname(pt);
          if (hn) {
            int ht = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = ", ht);
            Buf ab2; memset(&ab2, 0, sizeof ab2);
            emit_expr(c, provided, &ab2);
            buf_puts(g_pre, ab2.p ? ab2.p : "sp_box_nil()"); free(ab2.p);
            buf_puts(g_pre, ";\n");
            buf_printf(out, "(sp_%sHash *)_t%d.v.p", hn, ht);
            return;
          }
        }
        /* poly arg into a concrete param (holds the right type at runtime):
           coerce, else the generated C assigns sp_RbVal to a const char* /
           mrb_int / mrb_float / sp_<Class>* slot. */
        const char *ptn = c_type_name(pt);
        if (at == TY_POLY && pt == TY_STRING) { buf_puts(out, "sp_poly_to_s("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && pt == TY_FLOAT) { buf_puts(out, "sp_poly_to_f("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        else if (at == TY_POLY && (pt == TY_INT || pt == TY_BOOL)) { buf_puts(out, "sp_poly_to_i("); emit_expr(c, provided, out); buf_puts(out, ")"); }
        /* poly arg into an object or other pointer-backed param (array, proc,
           ...): unbox the pointer via emit_unbox_text (a nil box has v.p ==
           NULL, the pointer-nil representation). The callee's RBS asserts the
           type, mirroring the seed-trusting coercion on the return side. (Typed-
           value hashes are handled by the ty_is_hash block above; by-value types
           have no .v.p form and fall through to a plain emit.) */
        else if (at == TY_POLY && (ty_is_object(pt) || (ptn && ptn[0] && ptn[strlen(ptn) - 1] == '*'))) {
          Buf ub; memset(&ub, 0, sizeof ub);
          emit_expr(c, provided, &ub);
          emit_unbox_text(c, pt, ub.p ? ub.p : "", out);
          free(ub.p);
        }
        else emit_expr(c, provided, out);
      }
    }
    return;
  }
  int dv = m->pdefault[idx];
  const char *dty = dv >= 0 ? nt_type(c->nt, dv) : NULL;
  if (dv < 0) {
    buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
else if (dty && !strcmp(dty, "NilNode")) {
    /* nil default: emit the nil sentinel for the type */
    if (pt == TY_INT)    buf_puts(out, "SP_INT_NIL");
    else if (pt == TY_FLOAT) buf_puts(out, "sp_float_nil()");
    else if (pt == TY_STRING) buf_puts(out, "NULL");
    else buf_puts(out, pt == TY_RANGE ? "(sp_Range){0}" : default_value(pt));
  }
  else if (pt == TY_POLY) emit_boxed(c, dv, out);
  else {
    /* Default empty `[]` literal: emit the correct array constructor for
       the parameter type rather than always sp_IntArray_new(). */
    int den = 0;
    int is_empty_arr_dv = dty && !strcmp(dty, "ArrayNode") &&
                          (nt_arr(c->nt, dv, "elements", &den), den == 0);
    if (is_empty_arr_dv && ty_is_array(pt) && pt != TY_INT_ARRAY) {
      if (pt == TY_POLY_ARRAY) buf_puts(out, "sp_PolyArray_new()");
      else { const char *k = array_kind(pt); if (k) buf_printf(out, "sp_%sArray_new()", k); else emit_expr(c, dv, out); }
    }
    /* Default empty `{}` literal: emit the correct hash constructor for the
       parameter type, avoiding the "unsupported hash literal" fallback. */
    else {
      int dhn = 0;
      int is_empty_hash_dv = dty && (!strcmp(dty, "HashNode") || !strcmp(dty, "KeywordHashNode")) &&
                              (nt_arr(c->nt, dv, "elements", &dhn), dhn == 0);
      if (is_empty_hash_dv && ty_is_hash(pt)) {
        const char *hn = ty_hash_cname(pt);
        if (hn) buf_printf(out, "sp_%sHash_new()", hn);
        else emit_expr(c, dv, out);
      }
      else emit_expr(c, dv, out);
    }
  }
}

/* Emit a comma-separated argument list filling defaults for omitted
   optional params. `lead` is prepended before the first arg. */
/* Find the value node for keyword param named `kname` in a KeywordHashNode `kwh`. */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname) {
  if (kwh < 0 || !kname) return -1;
  int en = 0;
  const int *elems = nt_arr(nt, kwh, "elements", &en);
  for (int e = 0; e < en; e++) {
    int key = nt_ref(nt, elems[e], "key");
    if (key < 0) continue;
    const char *kty = nt_type(nt, key);
    const char *kn = (kty && !strcmp(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
    if (kn && !strcmp(kn, kname)) return nt_ref(nt, elems[e], "value");
  }
  return -1;
}

/* Emit a PolyArray expression that collects call args[from..pos_argc-1].
   SplatNode arguments are expanded element-by-element into the array. */
void emit_rest_pack(Compiler *c, int from, int pos_argc, const int *argv, Buf *b) {
  const NodeTable *nt = c->nt;
  /* Optimize: single pure-splat → direct conversion */
  if (pos_argc == from + 1) {
    const char *aty = argv ? nt_type(nt, argv[from]) : NULL;
    if (aty && !strcmp(aty, "SplatNode")) {
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
        emit_expr(c, inner, b);
        return;
      }
    }
  }
  /* Empty rest */
  if (!argv || pos_argc <= from) {
    buf_puts(b, "sp_PolyArray_new()");
    return;
  }
  /* General case: build PolyArray as statement expression */
  int t = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", t, t);
  for (int i = from; i < pos_argc; i++) {
    const char *aty = nt_type(nt, argv[i]);
    if (aty && !strcmp(aty, "SplatNode")) {
      int inner = nt_ref(nt, argv[i], "expression");
      TyKind at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
      Buf arr; memset(&arr, 0, sizeof arr);
      emit_expr(c, inner, &arr);
      const char *ap = arr.p ? arr.p : "NULL";
      if (at == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }", ap, t);
      else if (at == TY_STR_ARRAY)
        buf_printf(b, " { sp_StrArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_sa->data[_si])); }", ap, t);
      else if (at == TY_FLOAT_ARRAY)
        buf_printf(b, " { sp_FloatArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_sa->data[_si])); }", ap, t);
      else if (at == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap, t);
      else { /* scalar splat: single element */
        Buf el; memset(&el, 0, sizeof el);
        emit_boxed(c, inner, &el);
        buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
        free(el.p);
      }
      free(arr.p);
    }
else {
      Buf el; memset(&el, 0, sizeof el);
      emit_boxed(c, argv[i], &el);
      buf_printf(b, " sp_PolyArray_push(_t%d, %s);", t, el.p ? el.p : "sp_box_nil()");
      free(el.p);
    }
  }
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

/* Emit a PolyArray containing elements from array temp `tmp` starting at `from_idx`,
   then the remaining positional args from argv[argv_from..pos_argc-1]. */
void emit_rest_from_splat_and_argv(int tmp, TyKind at, int from_idx,
                                          Compiler *c, int argv_from, int pos_argc,
                                          const int *argv, Buf *b) {
  int t = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", t, t);
  /* elements from the splatted array starting at from_idx */
  if (at == TY_INT_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_t%d->data[_t%d->start+_si]));", tmp, from_idx, tmp, t, tmp, tmp);
  else if (at == TY_STR_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_str(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp);
  else if (at == TY_FLOAT_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, sp_box_float(_t%d->data[_si]));", tmp, from_idx, tmp, t, tmp);
  else if (at == TY_POLY_ARRAY)
    buf_printf(b, " if (_t%d) for (mrb_int _si = %d; _si < _t%d->len; _si++) sp_PolyArray_push(_t%d, _t%d->data[_si]);", tmp, from_idx, tmp, t, tmp);
  /* then suffix args after the splat */
  for (int j = argv_from; j < pos_argc; j++) {
    const char *jty = argv ? nt_type(c->nt, argv[j]) : NULL;
    if (jty && !strcmp(jty, "SplatNode")) {
      int inner2 = nt_ref(c->nt, argv[j], "expression");
      TyKind at2 = inner2 >= 0 ? comp_ntype(c, inner2) : TY_UNKNOWN;
      Buf arr2; memset(&arr2, 0, sizeof arr2); emit_expr(c, inner2, &arr2);
      const char *ap2 = arr2.p ? arr2.p : "NULL";
      if (at2 == TY_INT_ARRAY)
        buf_printf(b, " { sp_IntArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, sp_box_int(_sa->data[_sa->start+_si])); }", ap2, t);
      else if (at2 == TY_POLY_ARRAY)
        buf_printf(b, " { sp_PolyArray *_sa = %s; for (mrb_int _si = 0; _si < _sa->len; _si++) sp_PolyArray_push(_t%d, _sa->data[_si]); }", ap2, t);
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
  int poly = (pt == TY_POLY);
  if (!poly && !needs_root(pt)) { emit_arg_or_default(c, m, idx, provided, out); return; }
  Buf ab; memset(&ab, 0, sizeof ab);
  emit_arg_or_default(c, m, idx, provided, &ab);
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  emit_ctype(c, pt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", t, ab.p ? ab.p : default_value(pt));
  emit_indent(g_pre, g_indent);
  if (poly) buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", t);
  else buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  buf_printf(out, "_t%d", t);
  free(ab.p);
}

void emit_args_filled(Compiler *c, int callee_idx, int argsNode, const char *lead, Buf *out) {
  Scope *m = &c->scopes[callee_idx];
  const NodeTable *nt = c->nt;
  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* `bar(...)`: the ArgumentsNode holds a single ForwardingArgumentsNode.
     Forward the enclosing `def foo(...)` method's synthesized __fwd_* params
     directly to the callee, positionally (#1288). The compiler already knows
     foo's args; no rest array / splat is materialized. */
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      !strcmp(nt_type(nt, argv[0]), "ForwardingArgumentsNode")) {
    Scope *encl = comp_scope_of(c, argv[0]);
    for (int i = 0; i < m->nparams; i++) {
      buf_puts(out, i == 0 ? lead : ", ");
      if (encl && i < encl->nparams) {
        LocalVar *ep = scope_local(encl, encl->pnames[i]);
        LocalVar *mp = scope_local(m, m->pnames[i]);
        TyKind et = ep ? ep->type : TY_POLY;
        TyKind mt = mp ? mp->type : TY_POLY;
        char txt[80]; snprintf(txt, sizeof txt, "lv_%s", encl->pnames[i]);
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
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh = argv[argc - 1];
    pos_argc = argc - 1;
  }
  /* Detect double-splat (**hash) inside kwh: AssocSplatNode wrapping a hash expr.
     Pre-evaluate the hash to a temp so we can do per-param lookups. */
  int ds_hash_tmp = -1;  TyKind ds_hash_type = TY_UNKNOWN;
  if (kwh >= 0) {
    int en2 = 0; const int *elems2 = nt_arr(nt, kwh, "elements", &en2);
    for (int e = 0; e < en2; e++) {
      const char *ety2 = nt_type(nt, elems2[e]);
      if (ety2 && !strcmp(ety2, "AssocSplatNode")) {
        int inner2 = nt_ref(nt, elems2[e], "value");
        if (inner2 >= 0) {
          ds_hash_type = comp_ntype(c, inner2);
          if (ty_is_hash(ds_hash_type)) {
            ds_hash_tmp = ++g_tmp;
            emit_indent(g_pre, g_indent);
            emit_ctype(c, ds_hash_type, g_pre);
            buf_printf(g_pre, " _t%d = ", ds_hash_tmp);
            emit_expr(c, inner2, g_pre);
            buf_puts(g_pre, ";\n");
          }
        }
        break;
      }
    }
  }

  /* Find the first SplatNode in positional args. If it comes before rest_idx
     (or before nparams for rest-less methods), pre-evaluate it to a temp so
     we can index into it per fixed param. */
  int splat_idx = -1;  /* index into argv[] of the SplatNode */
  int splat_tmp = -1;  TyKind splat_at = TY_UNKNOWN;
  for (int k = 0; k < pos_argc; k++) {
    if (argv && nt_type(nt, argv[k]) && !strcmp(nt_type(nt, argv[k]), "SplatNode")) {
      int need_expand = (m->rest_idx >= 0 && k < m->rest_idx) ||
                        (m->rest_idx < 0 && k < m->nparams);
      if (need_expand) {
        splat_idx = k;
        int inner = nt_ref(nt, argv[k], "expression");
        splat_at = inner >= 0 ? comp_ntype(c, inner) : TY_UNKNOWN;
        if (ty_is_array(splat_at) || splat_at == TY_POLY_ARRAY) {
          splat_tmp = ++g_tmp;
          emit_indent(g_pre, g_indent);
          emit_ctype(c, splat_at, g_pre);
          buf_printf(g_pre, " _t%d = ", splat_tmp);
          emit_expr(c, inner, g_pre);
          buf_puts(g_pre, ";\n");
        }
      }
      break;
    }
  }
  /* GC hazard: a freshly-allocated heap argument sits in an unrooted C
     temporary while the rest of the call is evaluated AND while the callee
     runs. Either a later argument or the callee's own body can trigger a
     collection that sweeps it — and a constructor (sp_X_new) always
     allocates, so even `Ray.new(Vec.new(...), eye)` is exposed. Pre-evaluate
     each allocating heap arg into a rooted temp, left to right; emit_expr
     substitutes the temp via g_argov. Plain positional calls only — the
     splat/kwarg machinery has its own evaluation order. */
  int argov_saved = g_n_argov;
  if (splat_idx < 0 && kwh < 0 && argv) {
    for (int k = 0; k < pos_argc && k < m->nparams; k++) {
      if (g_n_argov >= MAX_ARG_OVERRIDE) break;
      TyKind at = comp_ntype(c, argv[k]);
      if (at != TY_POLY && !needs_root(at)) continue;
      const char *aty = nt_type(nt, argv[k]);
      /* a bare read is already rooted where it lives */
      if (aty && (!strcmp(aty, "LocalVariableReadNode") ||
                  !strcmp(aty, "InstanceVariableReadNode") ||
                  !strcmp(aty, "ConstantReadNode") ||
                  !strcmp(aty, "SelfNode") || !strcmp(aty, "NilNode") ||
                  !strcmp(aty, "StringNode"))) continue;
      /* only a fresh allocation needs protecting; a non-allocating heap
         expression (e.g. a ternary over two already-live reads) does not. */
      if (!subtree_may_allocate(nt, argv[k])) continue;
      int ht = ++g_tmp;
      /* Evaluate into a side buffer first: the expression may push its own
         setup into g_pre, which must be fully flushed before this temp's
         declaration line is written. */
      Buf hb; memset(&hb, 0, sizeof hb);
      emit_expr(c, argv[k], &hb);
      emit_indent(g_pre, g_indent);
      if (at == TY_POLY) {
        buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                   ht, hb.p ? hb.p : "sp_box_nil()", ht);
      }
else {
        emit_ctype(c, at, g_pre);
        buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n",
                   ht, hb.p ? hb.p : "0", ht);
      }
      free(hb.p);
      g_argov_node[g_n_argov] = argv[k];
      snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ht);
      g_n_argov++;
    }
  }
  for (int i = 0; i < m->nparams; i++) {
    buf_puts(out, i == 0 ? lead : ", ");
    if (m->rest_idx >= 0 && i == m->rest_idx) {
      /* rest collects middle args; stop before post-splat params */
      int rest_end = pos_argc - m->npost_rest;
      if (splat_tmp >= 0) {
        emit_rest_from_splat_and_argv(splat_tmp, splat_at, i - splat_idx,
                                      c, splat_idx + 1, rest_end, argv, out);
      }
else {
        emit_rest_pack(c, i, rest_end, argv, out);
      }
    }
else if (m->rest_idx >= 0 && m->npost_rest > 0 && i > m->rest_idx) {
      /* post-splat required param: take from the end of the call args */
      int post_j = i - m->rest_idx - 1;  /* 0-based index in posts */
      int argv_idx = pos_argc - m->npost_rest + post_j;
      if (argv && argv_idx >= 0 && argv_idx < pos_argc)
        emit_arg_rooted(c, m, i, argv[argv_idx], out);
      else
        emit_arg_rooted(c, m, i, -1, out);
    }
else if (splat_tmp >= 0 && i >= splat_idx) {
      /* this param comes from the splatted array at offset (i - splat_idx) */
      emit_array_elem_at(splat_at, splat_tmp, i - splat_idx, out);
    }
else {
      /* Check if this param has a keyword match (lookup by param name in kwh). */
      int kv = kwh >= 0 ? kwh_lookup(nt, kwh, m->pnames[i]) : -1;
      if (kv >= 0) {
        emit_arg_rooted(c, m, i, kv, out);
      }
      else if (ds_hash_tmp >= 0 && m->pnames[i]) {
        /* Double-splat: extract param by name from the pre-eval'd hash. */
        const char *hn = ty_hash_cname(ds_hash_type);
        LocalVar *plv = scope_local(m, m->pnames[i]);
        TyKind pt = plv ? plv->type : TY_INT;
        if (hn) {
          /* SymPoly: get returns sp_RbVal, unbox to param type.
             Other sym/str keyed hashes: get returns the value type directly. */
          TyKind hval = ty_hash_val(ds_hash_type);
          if (hval == TY_POLY) {
            char get_expr[128];
            snprintf(get_expr, sizeof get_expr,
                     "sp_%sHash_get(_t%d, sp_sym_intern(\"%s\"))",
                     hn, ds_hash_tmp, m->pnames[i]);
            emit_unbox_text(c, pt, get_expr, out);
          }
          else {
            buf_printf(out, "sp_%sHash_get(_t%d, sp_sym_intern(\"%s\"))",
                       hn, ds_hash_tmp, m->pnames[i]);
          }
        }
        else {
          buf_printf(out, "%s", default_value(pt));
        }
      }
      else if (m->kwrest_idx >= 0 && i == m->kwrest_idx) {
        /* Collect remaining (unbound) keyword args into a sp_SymPolyHash. */
        int krhash = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new();\n", krhash);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", krhash);
        if (kwh >= 0 && ds_hash_tmp < 0) {
          int en3 = 0; const int *elems3 = nt_arr(nt, kwh, "elements", &en3);
          for (int e3 = 0; e3 < en3; e3++) {
            int key3 = nt_ref(nt, elems3[e3], "key");
            int val3 = nt_ref(nt, elems3[e3], "value");
            if (key3 < 0 || val3 < 0) continue;
            const char *kty3 = nt_type(nt, key3);
            const char *kname3 = (kty3 && !strcmp(kty3, "SymbolNode")) ? nt_str(nt, key3, "value") : NULL;
            if (!kname3) continue;
            int consumed = 0;
            for (int jj = 0; jj < m->nparams; jj++) {
              if (jj == m->kwrest_idx) continue;
              if (m->pnames[jj] && !strcmp(m->pnames[jj], kname3)) { consumed = 1; break; }
            }
            if (consumed) continue;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", krhash, kname3);
            emit_boxed(c, val3, g_pre);
            buf_puts(g_pre, ");\n");
          }
        }
        buf_printf(out, "_t%d", krhash);
      }
      else if (i < pos_argc) {
        emit_arg_rooted(c, m, i, argv[i], out);
      }
      else {
        /* No positional arg and no keyword match. If the param is hash-typed
           (required `def f(attrs)` or optional `def f(opts = {})`) and the
           call site passed a KeywordHashNode (e.g. `f(key: val)`), Ruby packs
           the keywords into that hash parameter -- treat the whole kwh as the
           implicit hash argument rather than using the default. */
        LocalVar *p = scope_local(m, m->pnames[i]);
        TyKind pt = p ? p->type : TY_INT;
        int use_kwh = (kwh >= 0 && ty_is_hash(pt));
        emit_arg_rooted(c, m, i, use_kwh ? kwh : -1, out);
      }
    }
  }
  g_n_argov = argov_saved;  /* drop this call's hoisted-arg overrides */
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

/* Emit a (possibly virtual) method call. `selfptr` is a reusable C
   expression yielding sp_<static>* (e.g. "self", "&lv_x", "&_t3"). Args
   are pre-evaluated into temps so they're emitted once.
   `blk_node` is the BlockNode id of the attached block, or -1 if none. */
void emit_dispatch(Compiler *c, int cid, const char *name,
                          const char *selfptr, int argsNode, int blk_node, Buf *b) {
  const NodeTable *nt = c->nt;
  int defcls = cid;
  int mi = comp_method_in_chain(c, cid, name, &defcls);
  Scope *m = mi >= 0 ? &c->scopes[mi] : NULL;
  TyKind ret = m ? m->ret : TY_UNKNOWN;
  /* Unify return type across all descendant implementations so that even
     when the base method has TY_VOID/TY_UNKNOWN, a subclass override
     with a real return type makes the dispatch virtual and typed. */
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi >= 0 && (TyKind)c->scopes[kmi].ret != TY_UNKNOWN)
      ret = ty_unify(ret, (TyKind)c->scopes[kmi].ret);
  }

  int argc = 0;
  const int *argv = argsNode >= 0 ? nt_arr(nt, argsNode, "arguments", &argc) : NULL;
  /* `callee(...)`: forward the enclosing `def foo(...)` method's synthesized
     __fwd_* params positionally (#1288), same as the emit_args_filled path. */
  Scope *fwd_encl = NULL;
  if (argc == 1 && argv && nt_type(nt, argv[0]) &&
      !strcmp(nt_type(nt, argv[0]), "ForwardingArgumentsNode"))
    fwd_encl = comp_scope_of(c, argv[0]);
  /* separate keyword-hash arg */
  int kwh_d = -1, pos_argc_d = argc;
  if (argc > 0 && nt_type(nt, argv[argc - 1]) &&
      !strcmp(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
    kwh_d = argv[argc - 1]; pos_argc_d = argc - 1;
  }
  int np = m ? m->nparams : pos_argc_d;
  /* evaluate each param value (provided arg or default) into a temp so the
     virtual-dispatch cases reuse them without re-evaluating */
  int *atmp = np ? malloc(sizeof(int) * np) : NULL;
  const char *saved_self = g_self;
  for (int k = 0; k < np; k++) {
    atmp[k] = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    LocalVar *p = m ? scope_local(m, m->pnames[k]) : NULL;
    if (m && m->rest_idx >= 0 && k == m->rest_idx) {
      /* rest param: pack remaining positional args into PolyArray */
      emit_rest_pack(c, k, pos_argc_d, argv, &ab);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", atmp[k], ab.p ? ab.p : "sp_PolyArray_new()");
    }
    else if (fwd_encl && k < fwd_encl->nparams) {
      LocalVar *ep = scope_local(fwd_encl, fwd_encl->pnames[k]);
      TyKind et = ep ? ep->type : TY_POLY;
      char txt[80]; snprintf(txt, sizeof txt, "lv_%s", fwd_encl->pnames[k]);
      if (p && p->type == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, &ab);
      else buf_puts(&ab, txt);
      TyKind att = p ? p->type : et;
      emit_indent(g_pre, g_indent);
      emit_ctype(c, att, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
      if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      free(ab.p);
      continue;
    }
else {
      int kv = (m && kwh_d >= 0) ? kwh_lookup(nt, kwh_d, m->pnames[k]) : -1;
      int provided = kv >= 0 ? kv : (k < pos_argc_d ? argv[k] : -1);
      /* Default expressions (e.g. `@ivar * 10`) reference the callee's self and
         callee's class, not the caller's. Temporarily redirect both. A value-
         type receiver is a by-value struct, so its ivars dereference with `.`,
         not `->` (without this, `def m(r = @r)` emits `selfval->iv_r`). */
      int saved_emcls2 = g_emitting_class_id;
      const char *saved_deref3 = g_self_deref;
      if (provided < 0) {
        g_self = selfptr;
        g_self_deref = comp_ty_value_obj(c, ty_object(cid)) ? "." : "->";
        if (m) g_emitting_class_id = m->class_id;
      }
      emit_arg_or_default(c, m, k, provided, &ab);
      g_self = saved_self;
      g_self_deref = saved_deref3;
      g_emitting_class_id = saved_emcls2;
      TyKind att = p ? p->type : comp_ntype(c, k < argc ? argv[k] : -1);
      emit_indent(g_pre, g_indent);
      emit_ctype(c, att, g_pre);
      buf_printf(g_pre, " _t%d = ", atmp[k]);
      buf_puts(g_pre, ab.p ? ab.p : ""); buf_puts(g_pre, ";\n");
      /* Root heap-typed arg temps: evaluating a later argument may allocate
         and collect an earlier one still sitting in its temp. */
      if (att == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", atmp[k]); }
      else if (needs_root(att)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", atmp[k]); }
    }
    free(ab.p);
  }

  /* &block param that escapes: pre-evaluate the block as sp_Proc * temp.
     When the call site has no block, blk_tmp stays -1 and we pass NULL. */
  int blk_tmp = -1;
  int needs_blk_arg = m && m->blk_param && m->blk_param[0] && !m->yields;
  if (needs_blk_arg && blk_node >= 0) {
    blk_tmp = ++g_tmp;
    Buf pb; memset(&pb, 0, sizeof pb);
    emit_proc_literal(c, blk_node, &pb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
    free(pb.p);
  }

  /* The aliased name may differ from the defining method's real name. */
  const char *mname = m ? m->name : name;

  /* Force a runtime switch when there is no base implementation (m == NULL):
     a template method defined only in subclasses cannot be called directly as
     sp_<base>_<name>, so even a single descendant impl must dispatch virtually. */
  int impl_n = dispatch_impl_count(c, cid, name);
  /* A void/nil-returning method that subclasses override must still dispatch on
     the runtime class -- an implicit-self call to it from a base method (e.g.
     `def run; validate; end` where each subclass overrides `validate`) would
     otherwise bind statically to the base impl and skip the override (#1443).
     The GCC statement-expression wrapper can't declare a `void` result, so a
     void dispatch uses a dummy int temp (its value is discarded). */
  int ret_is_void = (ret == TY_VOID || ret == TY_NIL);
  TyKind disp_ret = ret_is_void ? TY_INT : ret;
  int virtual = (is_scalar_ret(ret) || ret_is_void) && (impl_n > 1 || (!m && impl_n >= 1));

  if (!virtual) {
    /* a value-type receiver is passed by value (no pointer cast) */
    if (comp_ty_value_obj(c, ty_object(cid)))
      buf_printf(b, "sp_%s_%s(%s", c->classes[defcls].name, mc(mname), selfptr);
    else
      buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
    for (int k = 0; k < np; k++) buf_printf(b, ", _t%d", atmp[k]);
    if (needs_blk_arg) {
      if (blk_tmp >= 0) buf_printf(b, ", _t%d", blk_tmp);
      else buf_puts(b, ", NULL");
    }
    buf_puts(b, ")");
    free(atmp);
    return;
  }

  /* runtime dispatch on cls_id (GCC statement-expression) */
  int rtmp = ++g_tmp;
  buf_puts(b, "({ ");
  emit_ctype(c, disp_ret, b);
  buf_printf(b, " _t%d; switch ((%s)->cls_id) {", rtmp, selfptr);
  for (int k = 0; k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kd = -1;
    int kmi = comp_method_in_chain(c, k, name, &kd);
    if (kmi < 0) continue;
    TyKind arm_ret = (TyKind)c->scopes[kmi].ret;
    const char *kfn = mc(c->scopes[kmi].name);
    if (method_is_void(&c->scopes[kmi])) {
      /* override emitted as a void C function (method_is_void: VOID/NIL/UNKNOWN
         ret, or initialize) -- call it, assign nil/zero to the result temp */
      buf_printf(b, " case %d: sp_%s_%s((sp_%s *)%s", k,
                 c->classes[kd].name, kfn, c->classes[kd].name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
      buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
    }
    else if (arm_ret != ret && ret == TY_POLY) {
      /* arm returns a concrete type but switch expects sp_RbVal: box it */
      buf_printf(b, " case %d: { ", k);
      Buf _bx; memset(&_bx, 0, sizeof _bx);
      buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
                 c->classes[kd].name, kfn, c->classes[kd].name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(&_bx, ", _t%d", atmp[a]);
      buf_puts(&_bx, ")");
      buf_printf(b, "_t%d = ", rtmp);
      emit_boxed_text(c, arm_ret, _bx.p ? _bx.p : "0", b);
      free(_bx.p);
      buf_puts(b, "; break; }");
    }
    else {
      buf_printf(b, " case %d: _t%d = sp_%s_%s((sp_%s *)%s", k, rtmp,
                 c->classes[kd].name, kfn, c->classes[kd].name, selfptr);
      for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
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
    free(atmp);
    return;
  }
  /* default arm uses the base-class (defcls) implementation */
  TyKind def_ret = (TyKind)m->ret;
  if (method_is_void(m)) {
    buf_printf(b, " default: sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_printf(b, "); _t%d = %s; break;", rtmp, default_value(disp_ret));
  }
  else if (def_ret != ret && ret == TY_POLY) {
    buf_printf(b, " default: { ");
    Buf _bx; memset(&_bx, 0, sizeof _bx);
    buf_printf(&_bx, "sp_%s_%s((sp_%s *)%s",
               c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(&_bx, ", _t%d", atmp[a]);
    buf_puts(&_bx, ")");
    buf_printf(b, "_t%d = ", rtmp);
    emit_boxed_text(c, def_ret, _bx.p ? _bx.p : "0", b);
    free(_bx.p);
    buf_puts(b, "; break; }");
  }
  else {
    buf_printf(b, " default: _t%d = sp_%s_%s((sp_%s *)%s", rtmp,
               c->classes[defcls].name, mc(mname), c->classes[defcls].name, selfptr);
    for (int a = 0; a < np; a++) buf_printf(b, ", _t%d", atmp[a]);
    buf_puts(b, "); break;");
  }
  buf_printf(b, " } _t%d; })", rtmp);
  free(atmp);
}

/* array.group_by { |x| key_expr } -> sp_PolyPolyHash (pre-statements to g_pre) */
int emit_group_by_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "group_by")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  if (bn < 1) return 0;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;

  int trecv = ++g_tmp, thash = ++g_tmp, tkey = ++g_tmp, tarr = ++g_tmp, ti = ++g_tmp;

  /* emit receiver */
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
  /* result hash */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);\n", thash, thash);
  /* loop */
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
             ti, ti, k, trecv, ti);
  /* assign element to block param */
  if (p0) {
    Scope *cs = comp_scope_of(c, id);
    LocalVar *outer_p0 = cs ? scope_local(cs, p0) : NULL;
    TyKind p0_type = outer_p0 ? outer_p0->type : et;
    emit_indent(g_pre, g_indent + 1);
    if (p0_type == TY_POLY && et != TY_POLY) {
      char elem_s[64];
      snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
      buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : elem_s); free(bx.p);
    }
    else {
      buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
    }
  }
  /* evaluate block body side-effect stmts, then key expression */
  int save_indent = g_indent; g_indent++;
  for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent);
  g_indent = save_indent;
  TyKind key_t = comp_ntype(c, bb[bn - 1]);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_RbVal _t%d = ", tkey);
  if (key_t != TY_POLY) {
    Buf kbuf; memset(&kbuf, 0, sizeof kbuf);
    int save2 = g_indent; g_indent += 1;
    emit_expr(c, bb[bn - 1], &kbuf);
    g_indent = save2;
    emit_boxed_text(c, key_t, kbuf.p ? kbuf.p : "0", g_pre);
    free(kbuf.p);
  }
  else {
    int save2 = g_indent; g_indent += 1;
    emit_expr(c, bb[bn - 1], g_pre);
    g_indent = save2;
  }
  buf_puts(g_pre, ";\n");
  /* get-or-create the PolyArray for this key */
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_PolyArray *_t%d;\n", tarr);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "if (sp_PolyPolyHash_has_key(_t%d, _t%d)) {\n", thash, tkey);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = (sp_PolyArray *)sp_PolyPolyHash_get(_t%d, _t%d).v.p;\n", tarr, thash, tkey);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "else {\n");
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tarr, tarr);
  emit_indent(g_pre, g_indent + 2);
  buf_printf(g_pre, "sp_PolyPolyHash_set(_t%d, _t%d, sp_box_obj(_t%d, SP_BUILTIN_POLY_ARRAY));\n",
             thash, tkey, tarr);
  emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
  /* push element (boxed) */
  emit_indent(g_pre, g_indent + 1);
  if (et != TY_POLY) {
    char elem_s[64];
    snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
    Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tarr, bx.p ? bx.p : "sp_box_nil()");
    free(bx.p);
  }
  else {
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));\n", tarr, k, trecv, ti);
  }
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  /* the expression evaluates to the hash */
  buf_printf(b, "_t%d", thash);
  return 1;
}

/* array.each_with_object(init) { |x, acc| ... } → acc (pre-statements to g_pre) */
int emit_each_with_object_expr(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || strcmp(name, "each_with_object")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || strcmp(bty, "BlockNode")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1 || !argv) return 0;
  TyKind rt = comp_ntype(c, recv);

  /* Hash receiver: hash.each_with_object(memo) { |(k,v), acc| } or { |k,v,acc| } */
  if (ty_is_hash(rt)) {
    const char *hn = ty_hash_cname(rt);
    if (!hn) return 0;
    int body = nt_ref(nt, block, "body");
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn < 1) return 0;
    /* Detect block param form: |(k,v), memo| (multi at idx 0) or |k, v, memo| (flat 3) */
    int is_multi = block_param_is_multi(c, block, 0);
    const char *kname_orig = NULL, *vname_orig = NULL, *mname_orig = NULL;
    if (is_multi) {
      kname_orig = block_param_multi_leaf(c, block, 0, 0);
      vname_orig = block_param_multi_leaf(c, block, 0, 1);
      mname_orig = block_param_name(c, block, 1);
    }
    else {
      kname_orig = block_param_name(c, block, 0);
      vname_orig = block_param_name(c, block, 1);
      mname_orig = block_param_name(c, block, 2);
    }
    const char *kname = kname_orig ? rename_local(kname_orig) : NULL;
    const char *vname = vname_orig ? rename_local(vname_orig) : NULL;
    const char *mname = mname_orig ? rename_local(mname_orig) : NULL;
    TyKind accT = infer_type(c, argv[0]);
    if (accT == TY_UNKNOWN) {
      const char *a0ty = nt_type(nt, argv[0]);
      int an0 = 0;
      if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
      if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) accT = TY_INT_ARRAY;
      else return 0;
    }
    /* Receiver */
    int trecv = ++g_tmp;
    { Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
      emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
      buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p); }
    /* Accumulator: use memo's declared type (may differ from accT if widened) */
    Scope *cs = comp_scope_of(c, id);
    LocalVar *memo_lv = mname_orig && cs ? scope_local(cs, mname_orig) : NULL;
    TyKind memo_decl = (memo_lv && memo_lv->type != TY_UNKNOWN) ? memo_lv->type : accT;
    int tacc = ++g_tmp;
    { Buf ab; memset(&ab, 0, sizeof ab); emit_expr(c, argv[0], &ab);
      emit_indent(g_pre, g_indent); emit_ctype(c, memo_decl, g_pre);
      if (memo_decl != accT)
        buf_printf(g_pre, " _t%d = %s_new();\n", tacc,
                   memo_decl == TY_POLY_ARRAY ? "sp_PolyArray" : "sp_IntArray");
      else
        buf_printf(g_pre, " _t%d = %s;\n", tacc, ab.p ? ab.p : default_value(memo_decl));
      free(ab.p); }
    emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tacc);
    /* Bind accumulator to memo param before loop */
    if (mname) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "lv_%s = _t%d;\n", mname, tacc);
    }
    /* Loop */
    int ti = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    /* Assign key */
    if (kname) {
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->keys[_t%d->order[_t%d]];\n", kname, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = _t%d->order[_t%d];\n", kname, trecv, ti);
    }
    /* Assign value */
    if (vname) {
      emit_indent(g_pre, g_indent + 1);
      if (rt == TY_POLY_POLY_HASH)
        buf_printf(g_pre, "lv_%s = _t%d->vals[_t%d->order[_t%d]];\n", vname, trecv, trecv, ti);
      else
        buf_printf(g_pre, "lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", vname, hn, trecv, trecv, ti);
    }
    /* Body */
    int save_indent = g_indent; g_indent++;
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", tacc);
    return 1;
  }

  if (!ty_is_array(rt)) return 0;
  const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
  if (!k) return 0;
  TyKind et = ty_array_elem(rt);
  TyKind accT = infer_type(c, argv[0]);
  int empty_seed = 0;
  if (accT == TY_UNKNOWN) {
    const char *a0ty = nt_type(nt, argv[0]);
    int an0 = 0;
    if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
    if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) {
      empty_seed = 1;
      TyKind me = ewo_memo_elem_type(c, id);
      accT = (me != TY_UNKNOWN) ? ty_array_of(me) : TY_INT_ARRAY;
    }
    else return 0;
  }
  int body = nt_ref(nt, block, "body");
  int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
  const char *p0_orig = block_param_name(c, block, 0);
  const char *p1_orig = block_param_name(c, block, 1);
  const char *p0 = p0_orig ? rename_local(p0_orig) : NULL;
  const char *p1 = p1_orig ? rename_local(p1_orig) : NULL;

  /* Receiver */
  int trecv = ++g_tmp;
  Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
  emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
  buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);

  /* Accumulator: an empty `[]` seed is built as a fresh typed array of the
     inferred element type, so a string/poly memo isn't materialized as int
     storage; a non-empty seed is emitted as written. */
  int tacc = ++g_tmp;
  if (empty_seed) {
    const char *ak = (accT == TY_POLY_ARRAY) ? "Poly" : array_kind(accT);
    emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
    buf_printf(g_pre, " _t%d = sp_%sArray_new();\n", tacc, ak ? ak : "Int");
  } else {
    /* a non-empty seed's emit_expr writes its construction to the prelude first,
       so the ctype/assign must follow it. */
    Buf accb; memset(&accb, 0, sizeof accb); emit_expr(c, argv[0], &accb);
    emit_indent(g_pre, g_indent); emit_ctype(c, accT, g_pre);
    buf_printf(g_pre, " _t%d = %s;\n", tacc, accb.p ? accb.p : default_value(accT)); free(accb.p);
  }

  /* Save outer vars if block params shadow them (same-type only) */
  Scope *cs = comp_scope_of(c, id);
  LocalVar *outer_p0 = (p0 && cs) ? scope_local(cs, p0) : NULL;
  int ts_p0 = 0;
  int p0_mismatch = outer_p0 && outer_p0->type != et;
  if (outer_p0 && !p0_mismatch) {
    ts_p0 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p0->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p0, p0); free(ot.p);
  }
  LocalVar *outer_p1 = (p1 && cs) ? scope_local(cs, p1) : NULL;
  int ts_p1 = 0;
  int p1_mismatch = outer_p1 && outer_p1->type != accT;
  if (outer_p1 && !p1_mismatch) {
    ts_p1 = ++g_tmp;
    Buf ot; memset(&ot, 0, sizeof ot); emit_ctype(c, outer_p1->type, &ot);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s _t%d = lv_%s;\n", ot.p ? ot.p : "mrb_int", ts_p1, p1); free(ot.p);
  }

  /* When block params shadow outer vars with different types, open a C scope
     and declare typed inner shadows so the body dispatches correctly. */
  int need_scope = p0_mismatch || p1_mismatch;
  if (need_scope) {
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "{\n");
    g_indent++;
    if (p1_mismatch) {
      emit_indent(g_pre, g_indent);
      emit_ctype(c, accT, g_pre);
      buf_printf(g_pre, " lv_%s = _t%d;\n", p1, tacc);
    }
    /* Temporarily override types for body dispatch */
    TyKind saved_p0_type = outer_p0 ? outer_p0->type : TY_UNKNOWN;
    TyKind saved_p1_type = outer_p1 ? outer_p1->type : TY_UNKNOWN;
    if (p0_mismatch) outer_p0->type = et;
    if (p1_mismatch) outer_p1->type = accT;
    for (int j = 0; j < bn; j++) infer_type(c, bb[j]);
    /* Bind accumulator to p1 before loop (type now matches) */
    if (p1 && !p1_mismatch) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tacc);
    }
    /* Loop */
    int ti = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k, trecv, ti);
    if (p0) {
      emit_indent(g_pre, g_indent + 1);
      if (p0_mismatch) {
        emit_ctype(c, et, g_pre);
        buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
      }
      else if (outer_p0 && outer_p0->type == TY_POLY && et != TY_POLY) {
        char elem_s[64];
        snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : elem_s); free(bx.p);
      }
      else {
        buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
      }
    }
    int save_indent = g_indent; g_indent++;
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    /* Restore types */
    if (p0_mismatch) { outer_p0->type = saved_p0_type; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
    if (p1_mismatch) { outer_p1->type = saved_p1_type; for (int j = 0; j < bn; j++) infer_type(c, bb[j]); }
    g_indent--;
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  }
  else {
    /* Bind accumulator to p1 before loop */
    if (p1) {
      emit_indent(g_pre, g_indent);
      TyKind p1_type = outer_p1 ? outer_p1->type : accT;
      if (p1_type == TY_POLY && accT != TY_POLY) {
        char tacc_s[32]; snprintf(tacc_s, sizeof tacc_s, "_t%d", tacc);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, accT, tacc_s, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p1, bx.p ? bx.p : tacc_s); free(bx.p);
      }
      else {
        buf_printf(g_pre, "lv_%s = _t%d;\n", p1, tacc);
      }
    }

    /* Loop */
    int ti = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
               ti, ti, k, trecv, ti);

    /* Assign element to p0 */
    if (p0) {
      emit_indent(g_pre, g_indent + 1);
      TyKind p0_type = outer_p0 ? outer_p0->type : et;
      if (p0_type == TY_POLY && et != TY_POLY) {
        char elem_s[64];
        snprintf(elem_s, sizeof elem_s, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, et, elem_s, &bx);
        buf_printf(g_pre, "lv_%s = %s;\n", p0, bx.p ? bx.p : elem_s); free(bx.p);
      }
      else {
        buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", p0, k, trecv, ti);
      }
    }

    /* Body */
    int save_indent = g_indent; g_indent++;
    emit_loop_body(c, body, g_pre, g_indent);
    g_indent = save_indent;

    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");

    /* Restore outer vars */
    if (p0 && ts_p0 > 0) {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p0, ts_p0);
    }
    if (p1 && ts_p1 > 0) {
      emit_indent(g_pre, g_indent); buf_printf(g_pre, "lv_%s = _t%d;\n", p1, ts_p1);
    }
  }

  /* The expression evaluates to the accumulator */
  buf_printf(b, "_t%d", tacc);
  return 1;
}

/* True if `recv` names the constant `name` either as a bare ConstantReadNode
   or as a root-anchored absolute path `::name` (ConstantPathNode, no parent). */
int recv_is_const(const NodeTable *nt, int recv, const char *name) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (!strcmp(rty, "ConstantReadNode") ||
      (!strcmp(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *n = nt_str(nt, recv, "name");
    return n && !strcmp(n, name);
  }
  return 0;
}

int sp_is_fiber_storage_recv(const NodeTable *nt, int recv) {
  if (recv < 0) return 0;
  const char *rty = nt_type(nt, recv);
  if (!rty) return 0;
  if (!strcmp(rty, "ConstantReadNode") ||
      (!strcmp(rty, "ConstantPathNode") && nt_ref(nt, recv, "parent") < 0)) {
    const char *rn = nt_str(nt, recv, "name");
    return rn && !strcmp(rn, "Fiber");
  }
  if (!strcmp(rty, "CallNode")) {
    const char *rn = nt_str(nt, recv, "name");
    int rr = nt_ref(nt, recv, "receiver");
    if (!rn || strcmp(rn, "current") || rr < 0) return 0;
    const char *rrty = nt_type(nt, rr);
    const char *rrn = nt_str(nt, rr, "name");
    return rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber");
  }
  return 0;
}

/* `Klass.new(args) { block }` where Klass#initialize yields: the constructor
   only allocates (a yielding initialize is never emitted), so inline the
   initialize body at the call site with self bound to the fresh object and the
   literal block feeding its yields -- the same per-call-site specialization as
   ordinary yield-method inlining. Returns 1 if handled. */
