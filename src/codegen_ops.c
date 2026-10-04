/* codegen_ops.c -- the emitters behind builtin_ops rows.

   emit_builtin_op looks the call up in the builtin-op table and runs the
   row's emitter. It sits where the receiver families it covers sat in
   emit_call_body, so the chain above it still claims what it claimed. */

#include "codegen_internal.h"
#include "builtin_ops.h"

/* The receiver's C text, emitted once. A row names it as $r, possibly
   more than once; every $r prints the same text, as the arms the rows
   replace did. */
static char *op_recv_text(Compiler *c, const BopCtx *x) {
  if (x->rtext) return strdup(x->rtext);
  Buf r; memset(&r, 0, sizeof r);
  emit_expr(c, x->recv, &r);
  if (!r.p) return strdup("");
  return r.p;
}

/* A row's C text with its placeholders filled in, each where it stands,
   so the C is emitted in the order the arm the row replaces emitted it:
     $r   the receiver, emitted at its first occurrence (or the text the
          caller rendered, x->rtext); a later $r repeats the same text
     $R   the receiver emitted again, for an arm that emitted it twice
     $h   the receiver held across the arguments (hold_recv_open, rooted
          in a temp of its C type): the hold opens before anything else is
          emitted or any temp is taken, and closes after the row's text
     $g   the receiver followed by its root in _t$t, "; SP_GC_ROOT(_t$t); "
          (emit_recv_rooted), for an arm that spilled it into $t
     $T   the temp the family took before its arms (emit_builtin_op_tmp)
     $t $u $v $w $x $y $z  temp numbers. The ones a row names are all taken
          (++g_tmp, in that order) before anything is emitted, as the arms
          took them
     $H   a Hash receiver's variant name, for sp_<H>Hash_* (ty_hash_cname)
     $K   a Hash receiver's boxed class id (hash_box_cls)
     $A   an Array receiver's element-kind name, for sp_<A>Array_* ("Poly"
          for a poly array, else array_kind)
     $eN  argument N by emit_expr
     $bN  argument N boxed (emit_boxed)
     $fN  argument N as a double (emit_float_expr)
     $iN  argument N as an sp_int (emit_int_expr)
     $sN  argument N as a String (emit_str_expr)
     $cN  argument N as an sp_Complex (emit_complex_coerce)
     $qN  argument N as an sp_Rational (emit_rat_coerce) */
static int emit_op_template(Compiler *c, const BopCtx *x, Buf *b) {
  static const char tnames[] = "tuvwxyz";
  int tn[7] = { 0, 0, 0, 0, 0, 0, 0 };
  Buf hb; memset(&hb, 0, sizeof hb);
  int held = strstr(x->op->arg, "$h") &&
             hold_recv_open(c, x->recv, 0, c_type_name(x->rt), "SP_GC_ROOT", b, &hb);
  for (int k = 0; k < 7; k++) {
    char pat[3] = { '$', tnames[k], 0 };
    if (strstr(x->op->arg, pat)) tn[k] = ++g_tmp;
  }
  char *r = NULL;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  for (const char *p = x->op->arg; *p; p++) {
    const char *tk = p[0] == '$' && p[1] ? strchr(tnames, p[1]) : NULL;
    if (p[0] == '$' && p[1] == 'r') {
      if (!r && x->rtext) { r = strdup(x->rtext); buf_puts(b, r); }
      else if (!r) {
        size_t mark = b->len;
        emit_expr(c, x->recv, b);
        r = strndup(b->p ? b->p + mark : "", b->len - mark);
      }
      else buf_puts(b, r);
      p++;
    }
    else if (p[0] == '$' && p[1] == 'h') {
      buf_puts(b, hb.p ? hb.p : "");
      p++;
    }
    else if (p[0] == '$' && p[1] == 'g') {
      emit_recv_rooted(c, x->recv, tn[0], "SP_GC_ROOT", b);
      p++;
    }
    else if (p[0] == '$' && p[1] == 'H') {
      buf_puts(b, ty_hash_cname(x->rt));
      p++;
    }
    else if (p[0] == '$' && p[1] == 'A') {
      const char *ak = x->rt == TY_POLY_ARRAY ? "Poly" : array_kind(x->rt);
      buf_puts(b, ak ? ak : "");
      p++;
    }
    else if (p[0] == '$' && p[1] == 'K') {
      buf_puts(b, hash_box_cls(x->rt));
      p++;
    }
    else if (p[0] == '$' && p[1] == 'T') {
      buf_printf(b, "%d", x->t0);
      p++;
    }
    else if (p[0] == '$' && p[1] == 'R') {
      emit_expr(c, x->recv, b);
      p++;
    }
    else if (tk) {
      buf_printf(b, "%d", tn[tk - tnames]);
      p++;
    }
    else if (p[0] == '$' && p[1] && strchr("ebficqs", p[1]) &&
             p[2] >= '0' && p[2] <= '9' && p[2] - '0' < argc) {
      int a = argv[p[2] - '0'];
      switch (p[1]) {
      case 'e': emit_expr(c, a, b); break;
      case 'b': emit_boxed(c, a, b); break;
      case 'f': emit_float_expr(c, a, b); break;
      case 'i': emit_int_expr(c, a, b); break;
      case 's': emit_str_expr(c, a, b); break;
      case 'c': emit_complex_coerce(c, a, b); break;
      default:  emit_rat_coerce(c, a, b); break;
      }
      p += 2;
    }
    else { char ch[2] = { *p, 0 }; buf_puts(b, ch); }
  }
  free(r);
  if (held) buf_puts(b, "; })");
  free(hb.p);
  return 1;
}

/* Process::Status#success?: the runtime answers -1 for CRuby's nil, when
   the process did not exit normally */
static int emit_op_pstatus_success(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  int t = ++g_tmp;
  buf_printf(b, "({ int _t%d = sp_process_status_success_p(sp_process_status_recv(%s, \"success?\")->status);"
                " _t%d < 0 ? sp_box_nil() : sp_box_bool((sp_bool)_t%d); })", t, r, t, t);
  free(r);
  return 1;
}

/* Process::Status#== / #eql? with no operand: false, the receiver still
   evaluated */
static int emit_op_pstatus_eq(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  free(r);
  buf_puts(b, "((void)("); emit_boxed(c, x->recv, b); buf_puts(b, "), (sp_bool)0)");
  return 1;
}

/* --plan-check: the row codegen emitted the call with should be the row
   inference answered it with (the same receiver kind, name and result; a
   row guarded on its first argument stands for its unguarded sibling).
   Inside a view the node is being read as another kind on purpose, so only
   calls emitted at view depth 0 are compared, and a node codegen renamed
   is not compared with the row of its original name.
     conflict    inference answered from a different row: the two halves
                 decided the call differently
     unrecorded  inference answered without a row (a rule ahead of the
                 lookup, or a codegen-only row whose result is TY_UNKNOWN)
     respecialized  inference's row is for another receiver kind: the node
                 is emitted more than once with its receiver typed per copy
                 (a yield's value, read per call site), and inference keeps
                 the last copy's answer */
static void plan_check_observe(Compiler *c, int id, TyKind rt, const BuiltinOp *op) {
  if (view_depth() > 0 || id < 0 || id >= c->node_cap) return;
  const BuiltinOp *inf = c->bop_inf[id];
  /* A different name is a different call: codegen renamed the node to emit
     it (a String bang's value form runs the plain method), so the row
     inference chose for the original name says nothing about this one. */
  if (inf && !sp_streq(inf->name, op->name)) return;
  if (inf && inf->recv == op->recv && sp_streq(inf->name, op->name) &&
      bop_result(inf, rt) == bop_result(op, rt)) return;
  if (inf && inf->recv != op->recv)
    fprintf(stderr, "plan-check: respecialized: node %d %s#%s: codegen row %s, inference row %s\n",
            id, ty_name(rt), op->name, ty_name(op->recv), ty_name(inf->recv));
  else if (inf)
    fprintf(stderr, "plan-check: conflict: node %d %s#%s: codegen row %d..%d -> %s, inference row %s#%s -> %s\n",
            id, ty_name(rt), op->name, op->argc_min, op->argc_max, ty_name(bop_result(op, rt)),
            ty_name(inf->recv), inf->name, ty_name(bop_result(inf, rt)));
  else
    fprintf(stderr, "plan-check: unrecorded: node %d %s#%s: codegen row %d..%d -> %s, inference answered without a row\n",
            id, ty_name(rt), op->name, op->argc_min, op->argc_max, ty_name(bop_result(op, rt)));
}

static int (*const bop_emitters[BOPE__COUNT])(Compiler *, const BopCtx *, Buf *) = {
  [BOPE_NONE] = NULL,
  [BOPE_TEMPLATE] = emit_op_template,
  [BOPE_PSTATUS_SUCCESS] = emit_op_pstatus_success,
  [BOPE_PSTATUS_EQ] = emit_op_pstatus_eq,
  [BOPE_THREAD_SET_REPORT] = emit_op_thread_set_report,
  [BOPE_THREAD_RAISE] = emit_op_thread_raise,
  [BOPE_THREAD_TLS] = emit_op_thread_tls,
  [BOPE_MUTEX_SLEEP] = emit_op_mutex_sleep,
  [BOPE_CONDVAR_WAIT] = emit_op_condvar_wait,
  [BOPE_QUEUE_PUSH] = emit_op_queue_push,
  [BOPE_QUEUE_POP] = emit_op_queue_pop,
  [BOPE_FIBER_RESUME] = emit_op_fiber_resume,
  [BOPE_FIBER_TRANSFER] = emit_op_fiber_transfer,
  [BOPE_FIBER_RAISE] = emit_op_fiber_raise,
  [BOPE_RATIONAL_ROUND] = emit_op_rational_round,
  [BOPE_STR_SET_N] = emit_op_str_set_n,
  [BOPE_STR_AFFIX_ANY] = emit_op_str_affix_any,
  [BOPE_HASH_PATTERN] = emit_op_hash_pattern,
  [BOPE_HASH_PATTERN_ALL] = emit_op_hash_pattern_all,
  [BOPE_HASH_DEFAULT_PROC] = emit_op_hash_default_proc,
  [BOPE_HASH_TO_PROC] = emit_op_hash_to_proc,
  [BOPE_HASH_AREF] = emit_op_hash_aref,
  [BOPE_HASH_HAS_KEY] = emit_op_hash_has_key,
  [BOPE_HASH_KEY] = emit_op_hash_key,
  [BOPE_HASH_DEFAULT] = emit_op_hash_default,
  [BOPE_HASH_KEYS] = emit_op_hash_keys,
  [BOPE_HASH_FETCH] = emit_op_hash_fetch,
  [BOPE_HASH_TO_S] = emit_op_hash_to_s,
  [BOPE_HASH_COMPACT_BANG] = emit_op_hash_compact_bang,
  [BOPE_HASH_REHASH] = emit_op_hash_rehash,
  [BOPE_HASH_REPLACE] = emit_op_hash_replace,
  [BOPE_HASH_SET_DEFAULT] = emit_op_hash_set_default,
  [BOPE_HASH_MERGE_BANG_MANY] = emit_op_hash_merge_bang_many,
  [BOPE_HASH_SHIFT] = emit_op_hash_shift,
  [BOPE_HASH_DELETE] = emit_op_hash_delete,
  [BOPE_HASH_INVERT] = emit_op_hash_invert,
  [BOPE_HASH_FLATTEN] = emit_op_hash_flatten,
  [BOPE_HASH_TO_A] = emit_op_hash_to_a,
  [BOPE_HASH_SORT] = emit_op_hash_sort,
  [BOPE_HASH_FIRST] = emit_op_hash_first,
  [BOPE_HASH_TAKE] = emit_op_hash_take,
  [BOPE_HASH_DROP] = emit_op_hash_drop,
  [BOPE_HASH_ASSOC] = emit_op_hash_assoc,
  [BOPE_HASH_COMPACT] = emit_op_hash_compact,
  [BOPE_ARRAY_SHIFT_N] = emit_op_array_shift_n,
  [BOPE_ARRAY_CYCLE_N] = emit_op_array_cycle_n,
  [BOPE_ARRAY_LAST] = emit_op_array_last,
  [BOPE_ARRAY_JOIN] = emit_op_array_join,
  [BOPE_ARRAY_SORT_BANG] = emit_op_array_sort_bang,
  [BOPE_ARRAY_SLICE_BANG_RANGE] = emit_op_array_slice_bang_range,
  [BOPE_ARRAY_PLUS] = emit_op_array_plus,
  [BOPE_ARRAY_SETOP] = emit_op_array_setop,
  [BOPE_ARRAY_INTERSECT_P] = emit_op_array_intersect_p,
  [BOPE_ARRAY_REPLACE] = emit_op_array_replace,
  [BOPE_ARRAY_MINMAX] = emit_op_array_minmax,
  [BOPE_ARRAY_SORT] = emit_op_array_sort,
  [BOPE_ARRAY_UNIQ] = emit_op_array_uniq,
  [BOPE_ARRAY_NMIN] = emit_op_array_nmin,
  [BOPE_ARRAY_SUM0] = emit_op_array_sum0,
  [BOPE_ARRAY_COMPACT_BANG] = emit_op_array_compact_bang,
  [BOPE_ARRAY_FLATTEN] = emit_op_array_flatten,
  [BOPE_ARRAY_PUSH] = emit_op_array_push,
  [BOPE_ARRAY_INSERT_N] = emit_op_array_insert_n,
  [BOPE_ARRAY_TRANSPOSE] = emit_op_array_transpose,
  [BOPE_ARRAY_ASSOC] = emit_op_array_assoc,
  [BOPE_ARRAY_COMBINATION] = emit_op_array_combination,
  [BOPE_ARRAY_PRODUCT] = emit_op_array_product,
  [BOPE_ARRAY_FETCH_VALUES0] = emit_op_array_fetch_values0,
  [BOPE_ARRAY_PRED0] = emit_op_array_pred0,
  [BOPE_ARRAY_DIG_N] = emit_op_array_dig_n,
  [BOPE_ARRAY_SUM1] = emit_op_array_sum1,
  [BOPE_ARRAY_CONCAT] = emit_op_array_concat,
  [BOPE_ARRAY_INDEX_V] = emit_op_array_index_v,
  [BOPE_ARRAY_CYCLE_ENDLESS] = emit_op_array_cycle_endless,
  [BOPE_ARRAY_SLICE_GROUPS] = emit_op_array_slice_groups,
  [BOPE_ARRAY_JOIN_STR] = emit_op_array_join_str,
  [BOPE_IVAR_REFLECTION] = emit_op_ivar_reflection,
  [BOPE_ARRAY_PRED_CLASS] = emit_op_array_pred_class,
};

/* an argument's kind, for a row's argument guard */
typedef struct { const Compiler *c; const int *argv; } BopArgs;
static TyKind bop_arg_ntype(const void *ud, int i) {
  const BopArgs *a = ud;
  return comp_ntype(a->c, a->argv[i]);
}

static int emit_builtin_op_ex(Compiler *c, int id, int recv, TyKind rt, const char *name,
                              const char *rtext, int t0, int stage, Buf *b) {
  if (recv < 0) return 0;
  /* a Hash or Array receiver of any variant reads its family's rows */
  TyKind lk = rt;
  if (!bop_covers(rt)) {
    if (ty_is_hash(rt)) lk = BOP_ANY_HASH;
    else if (ty_is_array(rt)) lk = BOP_ANY_ARRAY;
    if (lk == rt || !bop_covers(lk)) return 0;
  }
  int argc;
  const int *argv = call_args(c->nt, id, &argc);
  BopArgs a = { c, argv };
  const BuiltinOp *op = bop_find_stage(lk, name, argc, nt_ref(c->nt, id, "block") >= 0,
                                       bop_arg_ntype, &a, stage);
  if (!op || op->emit == BOPE_NONE || !bop_emitters[op->emit]) return 0;
  BopCtx x = { id, recv, argc, rt, name, op, rtext, t0 };
  if (!bop_emitters[op->emit](c, &x, b)) return 0;
  if (g_plan_check) plan_check_observe(c, id, rt, op);
  return 1;
}

int emit_builtin_op_text(Compiler *c, int id, int recv, TyKind rt, const char *name,
                         const char *rtext, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, rtext, 0, 0, b);
}

int emit_builtin_op_tmp(Compiler *c, int id, int recv, TyKind rt, const char *name,
                        int t0, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, t0, 0, b);
}

int emit_builtin_op(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, 0, 0, b);
}

int emit_builtin_op_stage(Compiler *c, int id, int recv, TyKind rt, const char *name,
                          int stage, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, 0, stage, b);
}
