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
     $N   an Integer or Float receiver with its nil beside the value (an
          sp_oint / sp_ofloat, emit_oint_expr): a receiver that can be nil
          hands the row its flag, one that cannot is wrapped as never nil.
          Emitted at its first occurrence, repeated after, as $r is. The
          caller's x->rtext is the plain rendering, so a receiver with an
          oint form is rendered again from the node (as the nil? arm does)
     $O   "_o" for an Integer or Float array receiver, "" for the other
          kinds: the runtime's `_o` twin answers those elements with their
          nil (sp_$AArray_pop$O)
     $<   open and $> close an unwrap of the row's sp_oint / sp_ofloat
          answer where the call's consumer wants it plain (sp_oint_arg, by
          node_is_oint of the call); nothing where it takes the oint. The
          kind is the row's result, or the call's type when the row has none
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
     $Y   an Array receiver's boxed class id, for sp_raise_frozen_array_at
          (array_frozen_cls)
     $eN  argument N by emit_expr
     $B / $E  a String Range's begin / end in _t$T, through its handle
          under --share-strings (sp_srange_first / sp_srange_last)
     $bN  argument N boxed (emit_boxed)
     $fN  argument N as a double (emit_float_expr)
     $FN  argument N as an arithmetic method's Float operand: any other
          class than a Numeric is "X can't be coerced into Float"
          (emit_float_coerce_expr)
     $iN  argument N as an sp_int (emit_int_expr)
     $oN  argument N as an IO offset, an sp_int whose nil CRuby words by
          NUM2OFFT (emit_int_expr_offt)
     $sN  argument N as a String (emit_str_expr)
     $cN  argument N as an sp_Complex (emit_complex_coerce)
     $qN  argument N as an sp_Rational (emit_rat_coerce) */
/* The template of an Integer row for a receiver that can never be nil: the
   row's own C text asks for its nil first (an Integer slot some write
   leaves nil in), and a plain Integer needs no such test (#7612). NULL: the
   row has no such variant. */
static const char *int_plain_template(const char *name) {
  static const struct { const char *name, *arg; } rows[] = {
    { "to_s",      "sp_int_to_s($r)" },
    { "inspect",   "sp_int_to_s($r)" },
    { "to_f",      "((sp_float)($r))" },
    { "to_i",      "($r)" },
    { "even?",     "((($r) % 2) == 0)" },
    { "odd?",      "((($r) % 2) != 0)" },
    { "zero?",     "(($r) == 0)" },
    { "positive?", "(($r) > 0)" },
    { "negative?", "(($r) < 0)" },
  };
  for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
    if (!strcmp(name, rows[i].name)) return rows[i].arg;
  return NULL;
}

static int emit_op_template(Compiler *c, const BopCtx *x, Buf *b) {
  static const char tnames[] = "tuvwxyz";
  const char *tmpl = x->op->arg;
  if (x->op->recv == TY_INT && x->rt == TY_INT && x->op->name) {
    const char *plain = int_plain_template(x->op->name);
    int pargc = 0;
    call_args(c->nt, x->id, &pargc);
    if (plain && pargc == 0 && int_value_plain(c, x->recv)) tmpl = plain;   /* to_s(2) keeps its row */
  }
  /* the plain form of a sub! / gsub! named in g_sub_bang_id enters the
     runtime through its wrapper (sub_bang_reenters) */
  if (g_sub_bang_id == x->id) {
    if (sp_streq(tmpl, "sp_str_sub($r, $s0, $s1)")) tmpl = "sp_str_sub_own($r, $s0, $s1)";
    else if (sp_streq(tmpl, "sp_str_gsub($r, $s0, $s1)")) tmpl = "sp_str_gsub_own($r, $s0, $s1)";
  }
  int tn[7] = { 0, 0, 0, 0, 0, 0, 0 };
  Buf hb; memset(&hb, 0, sizeof hb);
  int held = strstr(tmpl, "$h") &&
             hold_recv_open(c, x->recv, 0, c_type_name(x->rt), "SP_GC_ROOT", b, &hb);
  for (int k = 0; k < 7; k++) {
    char pat[3] = { '$', tnames[k], 0 };
    if (strstr(tmpl, pat)) tn[k] = ++g_tmp;
  }
  char *r = NULL, *o = NULL;
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  /* a length row (length, size, count: a plain Integer, never nil) where
     the call's slot holds its nil (a site of a call another receiver of
     which can answer nil): the whole text lifted. Only these names: a row
     can call a C helper that answers the oint itself (IO#getbyte), which
     its text does not show. */
  int tlift = x->op->result == TY_INT && x->op->name &&
              (is_len_alias(x->op->name) || sp_streq(x->op->name, "count")) && node_is_oint(c, x->id) &&
              !strstr(tmpl, "$[") && !strstr(tmpl, "$<") && !strstr(tmpl, "$O") && !strstr(tmpl, "$N") &&
              !strstr(tmpl, "oint") && !strstr(tmpl, "_o(") && !strstr(tmpl, "_opt");
  if (tlift) buf_printf(b, "%s(", oint_of(x->op->result));
  for (const char *p = tmpl; *p; p++) {
    const char *tk = p[0] == '$' && p[1] ? strchr(tnames, p[1]) : NULL;
    if (p[0] == '$' && p[1] == 'r') {
      /* (one-String mode: a String row's receiver is its bytes face,
         whichever face its text has -- an `a || b` of handles) */
      int rface = x->rt == TY_STRING;
      if (!r && x->rtext) {
        if (rface) { Buf fb; memset(&fb, 0, sizeof fb); buf_printf(&fb, "SP_STR_B(%s)", x->rtext); r = fb.p; }
        else r = strdup(x->rtext);
        buf_puts(b, r);
      }
      else if (!r) {
        size_t mark = b->len;
        if (rface) emit_expr_face(c, x->recv, TY_STRING, b);
        else emit_expr(c, x->recv, b);
        r = strndup(b->p ? b->p + mark : "", b->len - mark);
      }
      else buf_puts(b, r);
      p++;
    }
    else if (p[0] == '$' && p[1] == 'N') {
      /* a receiver already rendered (rtext) is reused, never emitted again:
         its hoisted preludes and side effects ran once. An unwrapped oint
         (`sp_oint_arg(<x>)`) is <x> itself; a plain value is lifted. */
      if (!o && x->rtext) {
        const char *rt = x->rtext, *ua = oint_arg(x->rt);
        size_t ul = strlen(ua), rl = strlen(rt);
        Buf ob; memset(&ob, 0, sizeof ob);
        int bal = 0, outer = rl > ul + 1 && strncmp(rt, ua, ul) == 0 && rt[ul] == '(' && rt[rl - 1] == ')';
        /* the parenthesis after the name must close at the very end */
        for (size_t q = ul; outer && q < rl; q++) {
          if (rt[q] == '(') bal++;
          else if (rt[q] == ')' && --bal == 0 && q != rl - 1) outer = 0;
        }
        if (outer) buf_printf(&ob, "%.*s", (int)(rl - ul - 2), rt + ul + 1);
        else buf_printf(&ob, "%s(%s)", oint_of(x->rt), rt);
        o = ob.p ? ob.p : strdup("");
        buf_puts(b, o);
      }
      else if (!o) {
        size_t mark = b->len;
        emit_oint_expr(c, x->recv, x->rt, b);
        o = strndup(b->p ? b->p + mark : "", b->len - mark);
      }
      else buf_puts(b, o);
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
    else if (p[0] == '$' && p[1] == 'C') {
      const char *sfx = case_map_suffix(c, argc, argv);
      buf_printf(b, "%s%s", sfx, sp_streq(sfx, "_turkic") ? "_sym" : "");
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
    else if (p[0] == '$' && p[1] == 'Y') {
      const char *cls = array_frozen_cls(x->rt);
      buf_puts(b, cls ? cls : "0");
      p++;
    }
    else if (p[0] == '$' && p[1] == 'O') {
      if (ty_is_array(x->rt) && oint_kind(ty_array_elem(x->rt))) buf_puts(b, "_o");
      p++;
    }
    else if (p[0] == '$' && (p[1] == '<' || p[1] == '>')) {
      TyKind rk = bop_result(x->op, x->rt);
      if (!oint_kind(rk)) rk = repr_of(c, x->id).as_ty;
      if (oint_kind(rk) && !node_is_oint(c, x->id)) buf_printf(b, p[1] == '<' ? "%s(" : ")", oint_arg(rk));
      p++;
    }
    /* `$[` ... `$]`: a row answering the plain value, lifted (sp_oint_of)
       where the consumer takes the oint (node_is_oint) */
    else if (p[0] == '$' && (p[1] == '[' || p[1] == ']')) {
      TyKind rk = bop_result(x->op, x->rt);
      if (!oint_kind(rk)) rk = repr_of(c, x->id).as_ty;
      if (oint_kind(rk) && node_is_oint(c, x->id)) buf_printf(b, p[1] == '[' ? "%s(" : ")", oint_of(rk));
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
    /* a String Range's begin / end held in _t$T: the field, or under
       --share-strings the endpoint read off the handle it may keep
       (sp_srange_first / sp_srange_last, #8321), where #first / #last
       raise for a beginless / endless Range, as CRuby's do
       (sp_srange_need_end) */
    else if (p[0] == '$' && (p[1] == 'B' || p[1] == 'E')) {
      int beg = p[1] == 'B';
      int chk = x->name && is_endpoint_query(x->name);
      if (chk)
        buf_printf(b, "sp_srange_%s(sp_srange_need_end(_t%d, %d))", beg ? "first" : "last", x->t0, !beg);
      else buf_printf(b, "sp_srange_%s(_t%d)", beg ? "first" : "last", x->t0);
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
    else if (p[0] == '$' && p[1] && strchr("ebfFicoqs", p[1]) &&
             p[2] >= '0' && p[2] <= '9' && p[2] - '0' < argc) {
      int a = argv[p[2] - '0'];
      switch (p[1]) {
      case 'e': emit_expr(c, a, b); break;
      case 'b': emit_boxed(c, a, b); break;
      case 'f': emit_float_expr(c, a, b); break;
      case 'F': emit_float_coerce_expr(c, a, b); break;
      case 'i': emit_int_expr(c, a, b); break;
      case 'o': emit_int_expr_offt(c, a, b); break;
      case 's': emit_str_expr(c, a, b); break;
      case 'c': emit_complex_coerce(c, a, b); break;
      default:  emit_rat_coerce(c, a, b); break;
      }
      p += 2;
    }
    else { char ch[2] = { *p, 0 }; buf_puts(b, ch); }
  }
  free(r); free(o);
  if (tlift) buf_puts(b, ")");
  if (held) buf_puts(b, "; })");
  free(hb.p);
  return 1;
}

/* Array#first with no count: the element, which for an Integer or Float
   array is read with its nil (an empty array, a nil element) and unwrapped
   where the call's consumer wants it plain. */
static int emit_op_array_first(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  const char *ak = x->rt == TY_POLY_ARRAY ? "Poly" : array_kind(x->rt);
  TyKind et = ty_array_elem(x->rt);
  if (oint_kind(et)) {
    if (!node_is_oint(c, x->id)) buf_printf(b, "%s(", oint_arg(et));
    buf_printf(b, "sp_%sArray_oget(%s, 0)", ak ? ak : "", r);
    if (!node_is_oint(c, x->id)) buf_puts(b, ")");
  }
  else if (recv_never_nil(c, x->recv)) buf_printf(b, "sp_%sArray_get(%s, 0)", ak ? ak : "", r);
  else buf_printf(b, "sp_%sArray_get(sp_nil_recv_named(%s, \"first\"), 0)", ak ? ak : "", r);   /* first, not the [] the read names */
  free(r);
  return 1;
}

/* Array#pop / #shift with no count: the element, nil when the array is
   empty -- an sp_oint / sp_ofloat from the _o runtime call for an Integer
   or Float array (unwrapped where the consumer wants it plain), NULL or a
   boxed nil from the plain call for the other kinds. */
static int emit_op_array_pop_shift(Compiler *c, const BopCtx *x, Buf *b) {
  char *r = op_recv_text(c, x);
  const char *ak = x->rt == TY_POLY_ARRAY ? "Poly" : array_kind(x->rt);
  TyKind et = ty_array_elem(x->rt);
  if (oint_kind(et)) {
    if (!node_is_oint(c, x->id)) buf_printf(b, "%s(", oint_arg(et));
    buf_printf(b, "sp_%sArray_%s_o(%s)", ak ? ak : "", x->name, r);
    if (!node_is_oint(c, x->id)) buf_puts(b, ")");
  }
  else buf_printf(b, "sp_%sArray_%s(%s)", ak ? ak : "", x->name, r);
  free(r);
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

/* an argument's kind, for a row's argument guard */
typedef struct { const Compiler *c; const int *argv; } BopArgs;
static TyKind bop_arg_ntype(const void *ud, int i) {
  const BopArgs *a = ud;
  return comp_ntype(a->c, a->argv[i]);
}

/* The same empty-literal seed kind that inference supplied to the row. */
static TyKind bop_seed_ntype(const void *ud, int i) {
  const BopArgs *a = ud;
  return fold_seed_kind(bop_arg_ntype(ud, i), nt_type(a->c->nt, a->argv[i]));
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
                 lookup, or a codegen-only row whose result is TY_UNKNOWN
                 and not BOPF_INFERRED)
     respecialized  inference's row is for another receiver kind: the node
                 is emitted more than once with its receiver typed per copy
                 (a yield's value, read per call site), and inference keeps
                 the last copy's answer */
static void plan_check_observe(Compiler *c, int id, TyKind rt, const BuiltinOp *op) {
  if (view_depth() > 0 || id < 0 || id >= c->node_cap) return;
  const BuiltinOp *inf = c->bop_inf[id];
  /* A call on a yield's value is emitted once per site of the yield, its
     receiver typed per site, and inference keeps the last site's row (or
     none, when a later pass erased it). The copy is compared with the row
     inference finds for its own site's kind; a copy whose site has no row,
     or another, still reports below. */
  if (!inf || inf->recv != op->recv) {
    int yv = nt_ref(c->nt, id, "receiver");
    TyKind sk;
    if (yv >= 0 && sp_yield_site_type(c, yv, &sk)) inf = an_bop_row_for_kind(c, id, rt);
  }
  /* A row flagged BOPF_INFERRED says inference types the call in a rule of
     its own; inference records no row for it (an_bop_find), so answering
     without one is what the row says. The call still has to have been
     typed: an untyped one stays unrecorded. */
  if (!inf && (op->flags & BOPF_INFERRED) && comp_ntype(c, id) != TY_UNKNOWN) return;
  /* A different name is a different call: codegen renamed the node to emit
     it (a String bang's value form runs the plain method), so the row
     inference chose for the original name says nothing about this one. */
  if (inf && !sp_streq(inf->name, op->name)) return;
  /* a row whose result turns on the arguments' kinds states it for the
     arguments the call has */
  int argc = 0;
  const int *argv = call_args(c->nt, id, &argc);
  BopArgs ba = { c, argv };
  BopArgKind arg_of = op->result == BOPR_ARRAY_SUM_SEED ? bop_seed_ntype : bop_arg_ntype;
  int facts = an_bop_facts(c, op, id);
  if (inf && inf->recv == op->recv && sp_streq(inf->name, op->name) &&
      bop_result_args(inf, rt, argc, arg_of, &ba, facts) == bop_result_args(op, rt, argc, arg_of, &ba, facts)) return;
  /* inference answered from a row that stands for a boxed receiver
     (BOPF_BOXED), and codegen emitted the call typed (the box respecialized,
     a face): the call answers the same whatever the box holds, so the row of
     the kind it was emitted with agrees when it states the same result */
  if (inf && (inf->flags & BOPF_BOXED) && inf->recv != op->recv &&
      bop_result_args(inf, rt, argc, arg_of, &ba, facts) == bop_result_args(op, rt, argc, arg_of, &ba, facts)) return;
  /* a call over a String Range that a pass reached through a `to_a` hop
     (srange_member_hop): inference answered it on the hop's Array of
     Strings, codegen emits the Range's own row. They agree when the two rows
     state the same result */
  if (inf && inf->recv != op->recv) {
    int hv = nt_ref(c->nt, id, "receiver");
    if (hv >= 0 && srange_member_hop(c, hv) >= 0 &&
        bop_result_args(inf, TY_STR_ARRAY, argc, arg_of, &ba, facts) == bop_result_args(op, rt, argc, arg_of, &ba, facts)) return;
  }
  if (inf && inf->recv != op->recv)
    fprintf(stderr, "plan-check: respecialized: node %d %s#%s: codegen row %s, inference row %s\n",
            id, ty_name(rt), op->name, ty_name(op->recv), ty_name(inf->recv));
  else if (inf)
    fprintf(stderr, "plan-check: conflict: node %d %s#%s: codegen row %d..%d -> %s, inference row %s#%s -> %s\n",
            id, ty_name(rt), op->name, op->argc_min, op->argc_max, ty_name(bop_result_args(op, rt, argc, arg_of, &ba, facts)),
            ty_name(inf->recv), inf->name, ty_name(bop_result_args(inf, rt, argc, arg_of, &ba, facts)));
  else
    fprintf(stderr, "plan-check: unrecorded: node %d %s#%s: codegen row %d..%d -> %s, inference answered without a row\n",
            id, ty_name(rt), op->name, op->argc_min, op->argc_max, ty_name(bop_result_args(op, rt, argc, arg_of, &ba, facts)));
}

static int (*const bop_emitters[BOPE__COUNT])(Compiler *, const BopCtx *, Buf *) = {
  [BOPE_NONE] = NULL,
  [BOPE_RANGE_CLONE] = emit_op_range_clone,
  [BOPE_RANGE_FREEZE] = emit_op_range_freeze,
  [BOPE_TEMPLATE] = emit_op_template,
  [BOPE_PSTATUS_SUCCESS] = emit_op_pstatus_success,
  [BOPE_PSTATUS_EQ] = emit_op_pstatus_eq,
  [BOPE_THREAD_SET_REPORT] = emit_op_thread_set_report,
  [BOPE_FLOAT_RATIONALIZE] = emit_op_float_rationalize,
  [BOPE_STRING_SCAN_CHECKED] = emit_op_string_scan_checked,
  [BOPE_STRING_SLICE] = emit_op_string_slice,
  [BOPE_STRING_DEDUP] = emit_op_string_dedup,
  [BOPE_STRING_SCRUB_BLOCK] = emit_op_string_scrub_block,
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
  [BOPE_POLY_CASE_OPTIONS] = emit_op_poly_case_options,
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
  [BOPE_HASH_NIL_RECV] = emit_op_hash_nil_recv,
  [BOPE_HASH_SORT] = emit_op_hash_sort,
  [BOPE_HASH_FIRST] = emit_op_hash_first,
  [BOPE_HASH_TAKE] = emit_op_hash_take,
  [BOPE_HASH_DROP] = emit_op_hash_drop,
  [BOPE_HASH_ASSOC] = emit_op_hash_assoc,
  [BOPE_HASH_COMPACT] = emit_op_hash_compact,
  [BOPE_ARRAY_SHIFT_N] = emit_op_array_shift_n,
  [BOPE_ARRAY_CYCLE_N] = emit_op_array_cycle_n,
  [BOPE_ARRAY_LAST] = emit_op_array_last,
  [BOPE_ARRAY_FIRST] = emit_op_array_first,
  [BOPE_ARRAY_POP_SHIFT] = emit_op_array_pop_shift,
  [BOPE_ARRAY_JOIN] = emit_op_array_join,
  [BOPE_ARRAY_PACK_BUFFER] = emit_op_array_pack_buffer,
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
  [BOPE_ARRAY_NIL_RECV] = emit_op_array_nil_recv,
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
  [BOPE_METHOD_UNBIND] = emit_op_method_unbind,
  [BOPE_METHOD_SUPER_METHOD] = emit_op_method_super_method,
  [BOPE_METHOD_BIND] = emit_op_method_bind,
  [BOPE_METHOD_TO_PROC] = emit_op_method_to_proc,
  [BOPE_METHOD_ORIGINAL_NAME] = emit_op_method_original_name,
  [BOPE_METHOD_SOURCE_LOCATION] = emit_op_method_source_location,
  [BOPE_METHOD_PARAMETERS] = emit_op_method_parameters,
  [BOPE_METHOD_OWNER] = emit_op_method_owner,
  [BOPE_METHOD_RECEIVER] = emit_op_method_receiver,
  [BOPE_METHOD_ARITY] = emit_op_method_arity,
  [BOPE_PROC_PARAMETERS] = emit_op_proc_parameters,
  [BOPE_PROC_SOURCE_LOCATION] = emit_op_proc_source_location,
  [BOPE_ARRAY_PRED_CLASS] = emit_op_array_pred_class,
};

static int emit_builtin_op_ex(Compiler *c, int id, int recv, TyKind rt, const char *name,
                              const char *rtext, int t0, int stage, BopArgKind arg_of, int observe, Buf *b) {
  if (recv < 0) return 0;
  /* a Hash or Array receiver of any variant reads its family's rows */
  TyKind lk = bop_family_of(rt);
  if (!bop_covers(lk)) return 0;
  int argc;
  const int *argv = call_args(c->nt, id, &argc);
  BopArgs a = { c, argv };
  const BuiltinOp *op = bop_find_stage(lk, name, argc, nt_ref(c->nt, id, "block") >= 0,
                                       arg_of, &a, stage);
  if (!op || op->emit == BOPE_NONE || !bop_emitters[op->emit]) return 0;
  BopCtx x = { id, recv, argc, rt, name, op, rtext, t0 };
  if (!bop_emitters[op->emit](c, &x, b)) return 0;
  if (g_plan_check && observe) plan_check_observe(c, id, rt, op);
  return 1;
}

int emit_builtin_op_text(Compiler *c, int id, int recv, TyKind rt, const char *name,
                         const char *rtext, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, rtext, 0, 0, bop_arg_ntype, 1, b);
}

/* A row rendered as one arm of a boxed dispatch: the node answers what the
   dispatch answers, so the row is not compared with inference's for the
   node (--plan-check) */
int emit_builtin_op_arm_text(Compiler *c, int id, int recv, TyKind rt, const char *name,
                             const char *rtext, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, rtext, 0, 0, bop_arg_ntype, 0, b);
}

int emit_builtin_op_tmp(Compiler *c, int id, int recv, TyKind rt, const char *name,
                        int t0, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, t0, 0, bop_arg_ntype, 1, b);
}

int emit_builtin_op(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, 0, 0, bop_arg_ntype, 1, b);
}

int emit_builtin_op_stage(Compiler *c, int id, int recv, TyKind rt, const char *name,
                          int stage, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, 0, stage, bop_arg_ntype, 1, b);
}

/* The call emitted through the row of its name that guards no argument kind,
   whatever the arguments are read as: a boxed index that may be a Range is
   viewed as an Integer to emit the element branch, and the row of an
   Integer index would state a result the held branch does not take. */
int emit_builtin_op_unguarded(Compiler *c, int id, int recv, TyKind rt, const char *name, Buf *b) {
  return emit_builtin_op_ex(c, id, recv, rt, name, NULL, 0, -1, NULL, 1, b);
}
