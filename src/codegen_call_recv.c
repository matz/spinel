/* codegen_call_recv.c -- receiver-typed method-call emitters (array/hash/
   scalar/object/value/range/poly), split out of codegen_call.c. Pure code
   movement, no logic change. */

#include "codegen_internal.h"

/* Receiver type with the empty-container-literal coercion the inference
   layer applies (`[].m` -> poly array, `{}.m` -> str-keyed poly hash, the
   same C type the emitters build for the bare literals): comp_ntype answers
   UNKNOWN for them, which stranded direct calls like `{}.size`. */
TyKind comp_recv_type(Compiler *c, int recv) {
  TyKind t = recv >= 0 ? comp_ntype(c, recv) : TY_UNKNOWN;
  if (t != TY_UNKNOWN || recv < 0) return t;
  const char *ty = nt_type(c->nt, recv);
  int en = 0;
  if (ty && sp_streq(ty, "ArrayNode")) {
    nt_arr(c->nt, recv, "elements", &en);
    if (en == 0) return TY_POLY_ARRAY;
  }
  else if (ty && (sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode"))) {
    nt_arr(c->nt, recv, "elements", &en);
    if (en == 0) return TY_STR_POLY_HASH;
  }
  return t;
}

/* Boxing function that lifts an array of kind `kk` ("Int"/"Str"/"Float"/"Poly")
   into a poly sp_RbVal. */
static const char *array_box_fn(const char *kk) {
  if (sp_streq(kk, "Int"))   return "sp_box_int_array";
  if (sp_streq(kk, "Str"))   return "sp_box_str_array";
  if (sp_streq(kk, "Float")) return "sp_box_float_array";
  return "sp_box_poly_array";
}

/* Emit the return value of an in-place filter mutator (select!/filter!/reject!/
   keep_if/delete_if) into `b`, after the compaction loop has run. Shared by the
   typed-array, poly-array, and hash bang handlers.
   Temps available:
     _t<trecv> : the (now-mutated) receiver (typed array/hash or poly array)
     _t<torig> : element count BEFORE compaction (mrb_int)
     _t<twp>   : element count AFTER compaction (mrb_int) == survivor count
   `boxed_self` is the receiver boxed into a poly sp_RbVal (e.g.
   "sp_box_int_array(_t4)" or "sp_box_obj(_t7, SP_BUILTIN_SYM_INT_HASH)").

   CRuby contract:
     - reject! / select! / filter!  ->  nil when nothing was removed, else self.
       These infer TY_POLY, so self must be boxed (a typed array/hash can't hold
       nil), hence the boxed ternary.
     - keep_if / delete_if          ->  always self, returned bare as the
       receiver type. */
static void emit_filter_bang_result(const char *name, int trecv, int torig,
                                    int twp, const char *boxed_self, Buf *b) {
  if (sp_streq(name, "reject!") || sp_streq(name, "select!") || sp_streq(name, "filter!"))
    buf_printf(b, "(_t%d != _t%d ? %s : sp_box_nil())", torig, twp, boxed_self);
  else
    buf_printf(b, "_t%d", trecv);  /* keep_if / delete_if: always self */
}

int emit_array_call(Compiler *c, int id, Buf *b) {
  /* Array#slice(i) / #slice(range) are exactly #[](...) -- reuse that arm
     through a rename re-entry (the two-argument slice already works). */
  {
    const NodeTable *nt0 = c->nt;
    const char *nm0 = nt_str(nt0, id, "name");
    if (nm0 && sp_streq(nm0, "slice")) {
      int recv0 = nt_ref(nt0, id, "receiver");
      int args0 = nt_ref(nt0, id, "arguments");
      int an0 = 0;
      if (args0 >= 0) nt_arr(nt0, args0, "arguments", &an0);
      if (recv0 >= 0 && an0 == 1 && ty_is_array(comp_ntype(c, recv0)) &&
          nt_ref(nt0, id, "block") < 0) {
        nt_node_set_str((NodeTable *)nt0, id, "name", "[]");
        int h = emit_array_call(c, id, b);
        nt_node_set_str((NodeTable *)nt0, id, "name", "slice");
        if (h) return 1;
      }
    }
    /* combination-family, slice/cons, and cycle block forms in VALUE
       position: run the statement emitter against a hoisted receiver, then
       evaluate to the receiver (combination family returns self) or nil
       (cycle; a valued break routes through the brk wrapper instead). */
    if (nm0 && nt_ref(nt0, id, "block") >= 0 && g_n_argov < MAX_ARG_OVERRIDE &&
        (sp_streq(nm0, "combination") || sp_streq(nm0, "permutation") ||
         sp_streq(nm0, "repeated_combination") || sp_streq(nm0, "repeated_permutation") ||
         sp_streq(nm0, "each_slice") || sp_streq(nm0, "each_cons") ||
         sp_streq(nm0, "cycle") || sp_streq(nm0, "zip"))) {
      int recv0 = nt_ref(nt0, id, "receiver");
      TyKind rt0 = recv0 >= 0 ? comp_ntype(c, recv0) : TY_UNKNOWN;
      if (recv0 >= 0 && ty_is_array(rt0)) {
        int ta0 = ++g_tmp;
        Buf ra0 = expr_buf(c, recv0);
        emit_indent(g_pre, g_indent);
        emit_ctype(c, rt0, g_pre);
        buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", ta0, ra0.p ? ra0.p : "NULL", ta0);
        free(ra0.p);
        g_argov_node[g_n_argov] = recv0;
        snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta0);
        g_n_argov++;
        buf_puts(b, "({ ");
        emit_stmt(c, id, b, 0);
        g_n_argov--;
        if (sp_streq(nm0, "cycle") || sp_streq(nm0, "zip"))
          buf_puts(b, " sp_box_nil(); })");   /* cycle { } / zip { } return nil */
        else
          buf_printf(b, " _t%d; })", ta0);  /* the others return self (Ruby >= 3.1) */
        return 1;
      }
    }
    /* Array#equal? -- object identity is pointer identity; a non-pointer or
       differently-shaped argument can never be the same object. */
    if (nm0 && sp_streq(nm0, "equal?")) {
      int recv0 = nt_ref(nt0, id, "receiver");
      int args0 = nt_ref(nt0, id, "arguments");
      int an0 = 0;
      const int *av0 = args0 >= 0 ? nt_arr(nt0, args0, "arguments", &an0) : NULL;
      if (recv0 >= 0 && an0 == 1 && ty_is_array(comp_ntype(c, recv0))) {
        TyKind at0 = comp_ntype(c, av0[0]);
        if (ty_is_array(at0) || ty_is_hash(at0)) {
          Buf rb = expr_buf(c, recv0), ab = expr_buf(c, av0[0]);
          buf_printf(b, "((void *)(%s) == (void *)(%s))",
                     rb.p ? rb.p : "0", ab.p ? ab.p : "0");
          free(rb.p); free(ab.p);
        }
        else {
          buf_puts(b, "0");
        }
        return 1;
      }
    }
  }
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  TyKind res = comp_ntype(c, id);
  /* [].first / [].last on an empty literal: there is no element type to read;
     the value is nil (boxed -- the call types poly). */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "first") || sp_streq(name, "last")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int fe_n = 0; nt_arr(nt, recv, "elements", &fe_n);
    if (fe_n == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
  }
  /* Homogeneous object array (sp_PtrArray of unboxed sp_X*), produced by the
     post-fixpoint narrow_object_arrays pass. Indexing yields a typed `sp_X *`
     directly -- no sp_RbVal box, no cls-id dispatch. Only the op set the pass
     admits reaches here; the pass and this block stay in lockstep. */
  if (recv >= 0 && ty_is_obj_array(rt)) {
    int ecls = ty_obj_array_class(rt);
    const char *ecn = c->classes[ecls].name;
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b);
      buf_puts(b, sp_streq(name, "first") ? ", 0))" : ", -1))");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_%s *_t%d = ", ecn, tv); emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_PtrArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " sp_PtrArray_push(_t%d, ", tr); emit_expr(c, argv[a], b); buf_puts(b, ");");
      }
      buf_printf(b, " _t%d; })", tr);
      return 1;
    }
    if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0) {
      buf_puts(b, "sp_PtrArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "empty?") && argc == 0) {
      buf_puts(b, "sp_PtrArray_empty("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* no-block comparisons via the boxed comparator (user `<=>` through the
       cmp hook); the narrowing pass admits these only when the element class
       has `<=>` and (for sort) the result lands in a modeled consumer. */
    if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PtrArray_sort_obj("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", ecls);
      return 1;
    }
    if (sp_streq(name, "sort!") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PtrArray_sort_obj_bang(_t%d, %d); _t%d; })", tr, ecls, tr);
      return 1;
    }
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "((sp_%s *)sp_PtrArray_minmax_obj(", ecn);
      emit_expr(c, recv, b);
      buf_printf(b, ", %d, %d))", ecls, sp_streq(name, "max") ? 1 : 0);
      return 1;
    }
    return 0;  /* unsupported obj-array op: pass should have prevented this. */
  }
  /* String value-form mutators: the expression yields the post-mutation
     string -- or nil for the no-change bang contract -- and reassigns an
     lvalue receiver (value-semantics strings). The transform reuses the
     non-bang emitter through a temporary node rename. */
  if (rt == TY_STRING && recv >= 0) {
    static const struct { const char *bang, *plain; int nil_nc; } SBANG[] = {
      {"gsub!", "gsub", 1}, {"sub!", "sub", 1}, {"upcase!", "upcase", 1},
      {"downcase!", "downcase", 1}, {"capitalize!", "capitalize", 1},
      {"swapcase!", "swapcase", 1}, {"strip!", "strip", 1}, {"lstrip!", "lstrip", 1},
      {"rstrip!", "rstrip", 1}, {"chomp!", "chomp", 1}, {"chop!", "chop", 1},
      {"squeeze!", "squeeze", 1}, {"tr!", "tr", 1}, {"delete!", "delete", 1},
      {"tr_s!", "tr_s", 1}, {"delete_prefix!", "delete_prefix", 1},
      {"delete_suffix!", "delete_suffix", 1},
      {"reverse!", "reverse", 0}, {"succ!", "succ", 0}, {"next!", "next", 0},
      {NULL, NULL, 0}
    };
    int sbi = -1;
    for (int j = 0; SBANG[j].bang; j++) if (sp_streq(name, SBANG[j].bang)) { sbi = j; break; }
    if (sbi >= 0) {
      const char *rvt2 = nt_type(nt, recv);
      int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                         sp_streq(rvt2, "InstanceVariableReadNode"));
      int to = ++g_tmp, tn2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b); buf_puts(b, "; (void)_t"); buf_printf(b, "%d; ", to);
      nt_node_set_str((NodeTable *)nt, id, "name", SBANG[sbi].plain);
      Buf nb; memset(&nb, 0, sizeof nb);
      emit_expr(c, id, &nb);
      nt_node_set_str((NodeTable *)nt, id, "name", SBANG[sbi].bang);
      buf_printf(b, "const char *_t%d = %s; ", tn2, nb.p ? nb.p : "");
      free(nb.p);
      if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn2); }
      if (SBANG[sbi].nil_nc)
        buf_printf(b, "sp_str_eq(_t%d, _t%d) ? NULL : _t%d; })", to, tn2, tn2);
      else
        buf_printf(b, "_t%d; })", tn2);
      return 1;
    }
    if ((sp_streq(name, "concat") || sp_streq(name, "<<")) && argc >= 1) {
      /* a STRBUF-promoted local (repeated `<<`) appends in place: the read
         form sp_String_cstr(lv) is not an lvalue, so the generic write-back
         below would emit an invalid assignment (#2020) */
      const char *rvt0 = nt_type(nt, recv);
      if (rvt0 && sp_streq(rvt0, "LocalVariableReadNode")) {
        const char *rnm0 = nt_str(nt, recv, "name");
        Scope *rsc0 = rnm0 ? comp_scope_of(c, recv) : NULL;
        LocalVar *rlv0 = rsc0 ? scope_local(rsc0, rnm0) : NULL;
        if (rlv0 && rlv0->type == TY_STRBUF) {
          int tb2 = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = lv_%s;", tb2, rename_local(rnm0));
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " sp_String_append(_t%d, ", tb2);
            emit_str_expr(c, argv[j], b);
            buf_puts(b, ");");
          }
          buf_printf(b, " sp_String_cstr(_t%d); })", tb2);
          return 1;
        }
      }
    }
    /* chained append in value position (`t = s << a << b`): the generic form
       below writes back only when the receiver is a direct lvalue read, so a
       chain's outer links never reach the base -- `s` kept just the first
       append. Unroll the chain onto the base, one write-back per link, and
       yield the base (each `<<` returns its receiver). */
    if (sp_streq(name, "<<") && argc == 1) {
      int chain[64]; int nchain = 0; int cur = recv;
      while (nchain < 64) {
        while (nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "ParenthesesNode")) {
          int pb = nt_ref(nt, cur, "body");
          if (pb < 0) break;
          int bn = 0; const int *bb = nt_arr(nt, pb, "body", &bn);
          if (bn != 1) break;
          cur = bb[0];
        }
        const char *cty = nt_type(nt, cur);
        if (!cty || !sp_streq(cty, "CallNode")) break;
        const char *cnm = nt_str(nt, cur, "name");
        int crecv = nt_ref(nt, cur, "receiver");
        if (!cnm || !sp_streq(cnm, "<<") || crecv < 0 || comp_ntype(c, crecv) != TY_STRING) break;
        int cargs = nt_ref(nt, cur, "arguments");
        int cac = 0; const int *cav = cargs >= 0 ? nt_arr(nt, cargs, "arguments", &cac) : NULL;
        if (cac != 1) break;
        chain[nchain++] = cav[0];
        cur = crecv;
      }
      const char *bty = nt_type(nt, cur);
      LocalVar *blv = (bty && sp_streq(bty, "LocalVariableReadNode"))
                      ? scope_local(comp_scope_of(c, cur), nt_str(nt, cur, "name")) : NULL;
      /* STRBUF base: the buffer appends in place (its cstr read is not an
         lvalue, so the concat-and-write-back form below can't serve it) */
      if (nchain > 0 && blv && blv->type == TY_STRBUF &&
          bty && sp_streq(bty, "LocalVariableReadNode")) {
        int tb9 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = lv_%s;", tb9, rename_local(nt_str(nt, cur, "name")));
        for (int j = nchain; j >= 0; j--) {  /* innermost link first */
          int arg = j > 0 ? chain[j - 1] : argv[0];
          buf_printf(b, " sp_String_append(_t%d, ", tb9);
          emit_str_expr(c, arg, b);
          buf_puts(b, ");");
        }
        buf_printf(b, " sp_String_cstr(_t%d); })", tb9);
        return 1;
      }
      if (nchain > 0 && bty && !(blv && blv->type == TY_STRBUF) &&
          (sp_streq(bty, "LocalVariableReadNode") || sp_streq(bty, "InstanceVariableReadNode"))) {
        buf_puts(b, "({ ");
        for (int j = nchain; j >= 0; j--) {  /* innermost link first, outer arg last */
          int arg = j > 0 ? chain[j - 1] : argv[0];
          buf_puts(b, "sp_str_check_mutable("); emit_expr(c, cur, b); buf_puts(b, "); ");
          emit_expr(c, cur, b); buf_puts(b, " = sp_str_concat(");
          emit_expr(c, cur, b); buf_puts(b, ", ");
          emit_str_expr(c, arg, b);
          buf_puts(b, "); ");
        }
        emit_expr(c, cur, b);
        buf_puts(b, "; })");
        return 1;
      }
    }
    if ((sp_streq(name, "concat") || sp_streq(name, "<<") ||
         sp_streq(name, "prepend")) && argc >= 1) {
      const char *rvt2 = nt_type(nt, recv);
      int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                         sp_streq(rvt2, "InstanceVariableReadNode"));
      int tn2 = ++g_tmp, trc = ++g_tmp;
      /* Evaluate the receiver once into a temp: it feeds both the frozen-mutability
         check and the concatenation, and a chained `s << a << b` receiver has a
         side effect that must not run twice. */
      buf_printf(b, "({ const char *_t%d = ", trc); emit_expr(c, recv, b); buf_puts(b, "; ");
      buf_printf(b, "const char *_t%d = ", tn2);
      if (sp_streq(name, "prepend")) {
        /* args first (in order), then the receiver */
        for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
        emit_str_expr(c, argv[0], b);
        for (int j = 1; j < argc; j++) { buf_puts(b, ", "); emit_str_expr(c, argv[j], b); buf_puts(b, ")"); }
        buf_printf(b, ", _t%d)", trc);
      }
      else {
        for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
        buf_printf(b, "_t%d", trc);
        for (int j = 0; j < argc; j++) { buf_puts(b, ", "); emit_str_expr(c, argv[j], b); buf_puts(b, ")"); }
      }
      buf_puts(b, "; ");
      /* Ruby evaluates the argument(s) before invoking the mutator, so the
         frozen check must fire AFTER the concatenation builds (which is what
         evaluates the args). sp_str_concat allocates a fresh string and never
         mutates the receiver, so a frozen receiver is still untouched here. */
      buf_printf(b, "sp_str_check_mutable(_t%d); ", trc);
      if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn2); }
      buf_printf(b, "_t%d; })", tn2);
      return 1;
    }
    if (sp_streq(name, "insert") && argc == 2) {
      const char *rvt2 = nt_type(nt, recv);
      int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                         sp_streq(rvt2, "InstanceVariableReadNode"));
      int to = ++g_tmp, ti2 = ++g_tmp, tn2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (mrb_int)sp_str_length(_t%d) + 1;", ti2, ti2, to);
      buf_printf(b, " const char *_t%d = sp_str_splice_at(_t%d, _t%d, 0, ", tn2, to, ti2);
      emit_str_expr(c, argv[1], b); buf_puts(b, ", 0); ");
      if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn2); }
      buf_printf(b, "_t%d; })", tn2);
      return 1;
    }
    if (sp_streq(name, "replace") && argc == 1) {
      const char *rvt2 = nt_type(nt, recv);
      int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                         sp_streq(rvt2, "InstanceVariableReadNode"));
      int tn2 = ++g_tmp;
      buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); const char *_t%d = ", tn2); emit_str_expr(c, argv[0], b); buf_puts(b, "; ");
      if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn2); }
      buf_printf(b, "_t%d; })", tn2);
      return 1;
    }
  }
  /* String#slice! in VALUE position: returns the removed part (or nil) and
     reassigns the receiver; statement position has its own arm. The
     receiver must be an lvalue (re-read and re-assigned). */
  if (rt == TY_STRING && sp_streq(name, "slice!") && (argc == 1 || argc == 2)) {
    const char *rvt2 = nt_type(nt, recv);
    int sb_asgn = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                           sp_streq(rvt2, "InstanceVariableReadNode"));
    if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
      int tp2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", tp2); emit_expr(c, argv[0], b);
      buf_printf(b, "; const char *_hit%d = (_t%d && ", tp2, tp2);
      emit_expr(c, recv, b);
      buf_printf(b, ") ? strstr(", tp2); emit_expr(c, recv, b);
      buf_printf(b, ", _t%d) : NULL;", tp2);
      if (sb_asgn) {
        buf_printf(b, " if (_hit%d) ", tp2);
        emit_expr(c, recv, b);
        buf_printf(b, " = sp_str_sub("); emit_expr(c, recv, b);
        buf_printf(b, ", _t%d, (&(\"\\xff\")[1]));", tp2);
      }
      buf_printf(b, " _hit%d ? _t%d : (const char *)0; })", tp2, tp2);
      return 1;
    }
    if (argc == 1 && re_lit_index(c, argv[0]) >= 0) {
      /* slice!(/re/): remove the first match, evaluate to it (or nil).
         sp_re_match fills sp_re_match_str with the matched run; the splice
         helper replaces it with the empty string. */
      int tm3 = ++g_tmp, ts3 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", ts3); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = sp_re_match(sp_re_pat_%d, _t%d);"
                    " const char *_hit%d = _t%d >= 0 ? sp_re_match_str : NULL;",
                 tm3, re_lit_index(c, argv[0]), ts3, tm3, tm3);
      if (sb_asgn) {
        buf_printf(b, " if (_hit%d) ", tm3);
        emit_expr(c, recv, b);
        buf_printf(b, " = sp_str_splice_re(sp_re_pat_%d, _t%d, (&(\"\\xff\")[1]));",
                   re_lit_index(c, argv[0]), ts3);
      }
      buf_printf(b, " _hit%d; })", tm3);
      return 1;
    }
    if (argc == 1 && re_lit_index(c, argv[0]) >= 0) {
      /* slice!(regexp): the removed first match (or nil), reassigning an
         lvalue receiver with the remainder; sets the match registers. */
      int to = ++g_tmp, ts2 = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b);
      buf_printf(b, "; const char *_t%d = _t%d;"
                    " const char *_t%d = sp_str_slice_re(sp_re_pat_%d, _t%d, &_t%d);",
                 ts2, to, tr2, re_lit_index(c, argv[0]), to, ts2);
      if (sb_asgn) { buf_puts(b, " "); emit_expr(c, recv, b); buf_printf(b, " = _t%d;", ts2); }
      buf_printf(b, " _t%d; })", tr2);
      return 1;
    }
    if (argc == 1 && (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_RANGE)) {
      /* slice!(i) / slice!(range): the removed part (or nil), reassigning an
         lvalue receiver; a literal receiver just yields the removed part. */
      int to = ++g_tmp, tb2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b);
      buf_printf(b, "; mrb_int _t%d = (mrb_int)sp_str_length(_t%d); mrb_int _t%d, _t%d;",
                 tn2, to, tb2, tl2);
      if (comp_ntype(c, argv[0]) == TY_RANGE) {
        int trg = ++g_tmp;
        buf_printf(b, " sp_Range _t%d = ", trg); emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d = _t%d.first < 0 ? _t%d.first + _t%d : _t%d.first;",
                   tb2, trg, trg, tn2, trg);
        buf_printf(b, " _t%d = (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                   tl2, trg, trg, tn2, trg, tb2, trg);
        buf_printf(b, " if (_t%d < 0) _t%d = 0;", tl2, tl2);
      }
      else {
        buf_printf(b, " _t%d = ", tb2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; _t%d = 1; if (_t%d < 0) _t%d += _t%d;", tl2, tb2, tb2, tn2);
      }
      buf_printf(b, " const char *_t%d = NULL;"
                    " if (_t%d >= 0 && _t%d < _t%d && _t%d > 0) {"
                    " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d;"
                    " _t%d = sp_str_sub_range(_t%d, _t%d, _t%d);",
                 tr2,
                 tb2, tb2, tn2, tl2,
                 tl2, tn2, tb2, tl2, tn2, tb2,
                 tr2, to, tb2, tl2);
      if (sb_asgn) {
        buf_puts(b, " ");
        emit_expr(c, recv, b);
        buf_printf(b, " = sp_str_concat(sp_str_sub_range(_t%d, 0, _t%d), sp_str_sub_range(_t%d, _t%d + _t%d, _t%d - _t%d - _t%d));",
                   to, tb2, to, tb2, tl2, tn2, tb2, tl2);
      }
      buf_printf(b, " } _t%d; })", tr2);
      return 1;
    }
    if (sb_asgn && argc == 2) {
      int ti2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; mrb_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; mrb_int _t%d = (mrb_int)strlen(", tn2);
      emit_expr(c, recv, b);
      buf_printf(b, "); const char *_t%d = NULL;"
                    " if (_t%d < 0) _t%d += _t%d;"
                    " if (_t%d >= 0 && _t%d <= _t%d && _t%d > 0) {"
                    " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d;"
                    " _t%d = sp_str_substr(",
                 tr2,
                 ti2, ti2, tn2,
                 ti2, ti2, tn2, tl2,
                 tl2, tn2, ti2, tl2, tn2, ti2,
                 tr2);
      emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, _t%d); ", ti2, tl2);
      emit_expr(c, recv, b);
      buf_puts(b, " = sp_str_concat(sp_str_substr(");
      emit_expr(c, recv, b);
      buf_printf(b, ", 0, _t%d), sp_str_substr(", ti2);
      emit_expr(c, recv, b);
      buf_printf(b, ", _t%d + _t%d, _t%d - _t%d - _t%d)); } _t%d; })",
                 ti2, tl2, tn2, ti2, tl2, tr2);
      return 1;
    }
  }
  /* String#bytesplice(start, len, str): byte-range replace returning self
     (value-semantics strings: the helper builds the new value and an lvalue
     receiver is rebound to it). */
  /* bytesplice(range, str): lower the Range index to (start, len) (#2396) */
  if (rt == TY_STRING && sp_streq(name, "bytesplice") && argc == 2 && recv >= 0 &&
      comp_ntype(c, argv[0]) == TY_RANGE) {
    const char *rvt9 = nt_type(nt, recv);
    int lvw9 = rvt9 && (sp_streq(rvt9, "LocalVariableReadNode") ||
                        sp_streq(rvt9, "InstanceVariableReadNode"));
    int tr9 = ++g_tmp, tn9 = ++g_tmp;
    buf_printf(b, "({ sp_Range _t%d = ", tr9); emit_expr(c, argv[0], b);
    buf_printf(b, "; const char *_t%d = sp_str_bytesplice(", tn9);
    emit_expr(c, recv, b);
    buf_printf(b, ", _t%d.first, _t%d.last - _t%d.first + (_t%d.excl ? 0 : 1), ", tr9, tr9, tr9, tr9);
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    if (lvw9) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn9); }
    buf_printf(b, "; _t%d; })", tn9);
    return 1;
  }
  /* append_as_bytes: raw byte append == << for spinel's byte strings (#2397) */
  if (rt == TY_STRING && sp_streq(name, "append_as_bytes") && argc >= 1 && recv >= 0) {
    const char *rvt9 = nt_type(nt, recv);
    int lvw9 = rvt9 && (sp_streq(rvt9, "LocalVariableReadNode") ||
                        sp_streq(rvt9, "InstanceVariableReadNode"));
    int tn9 = ++g_tmp;
    /* append_as_bytes accepts String AND Integer arguments; an Integer is the
       raw byte value (100 -> "d"), materialized via sp_int_chr (#2463). */
    buf_printf(b, "({ const char *_t%d = sp_str_concat(", tn9);  /* reassigned per extra arg: keep non-const? const char* variable is reassignable (the POINTEE is const) */
    emit_expr(c, recv, b); buf_puts(b, ", ");
    if (comp_ntype(c, argv[0]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    else emit_str_expr(c, argv[0], b);
    buf_puts(b, ")");
    for (int a9 = 1; a9 < argc; a9++) {
      buf_printf(b, "; _t%d = sp_str_concat(_t%d, ", tn9, tn9);
      if (comp_ntype(c, argv[a9]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[a9], b); buf_puts(b, ")"); }
      else emit_str_expr(c, argv[a9], b);
      buf_puts(b, ")");
    }
    if (lvw9) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn9); }
    buf_printf(b, "; _t%d; })", tn9);
    return 1;
  }
  if (rt == TY_STRING && sp_streq(name, "bytesplice") && argc == 3 && recv >= 0) {
    const char *rvt2 = nt_type(nt, recv);
    int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                       sp_streq(rvt2, "InstanceVariableReadNode"));
    int tn2 = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_str_bytesplice(", tn2);
    emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    buf_puts(b, ", "); emit_expr(c, argv[2], b); buf_puts(b, ")");
    if (lvw) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn2); }
    buf_printf(b, "; _t%d; })", tn2);
    return 1;
  }

  /* find/detect over a bare poly value that is only known to be an array at
     runtime (an inner array read out of a poly container:
     `[[1,2],[3,4]].map { |row| row.find { } }`): coerce to a poly array and
     scan, mirroring the poly-array form inside the ty_is_array block below,
     which this receiver type does not enter (#2904). */
  if (recv >= 0 && rt == TY_POLY && (sp_streq(name, "find") || sp_streq(name, "detect")) &&
      nt_ref(nt, id, "block") >= 0 && argc == 0) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int fbody = nt_ref(nt, fblock, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fbn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
                 trecv, rb.p ? rb.p : "sp_box_nil()", name, trecv);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tres, tres);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      for (int j = 0; j < fbn - 1; j++) emit_stmt(c, fbb[j], g_pre, g_indent + 1);
      int sv = g_indent; g_indent++;
      Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, fbb[fbn - 1], &cb); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      if (!splat && bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
      else buf_printf(g_pre, "if (%s) { _t%d = %s; break; }\n", cb.p ? cb.p : "0", tres, es);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", tres); return 1;
    }
  }

  /* sort / reject / each_index over a bare poly value that is an array at
     runtime (an inner array read out of a poly container), following #2904.
     Coerce to a poly array and run the operation. (#2928) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "sort") && argc == 0 &&
      nt_ref(nt, id, "block") < 0) {
    buf_puts(b, "sp_poly_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* `enum.drop(n)` / `enum.reject|select|filter { }` on an each_with_index-style
     Enumerator: materialize its pairs to a poly array and re-dispatch as the
     array form (drop returns a slice; the block forms run the block over each
     pair). (#2878, #2943) */
  if (recv >= 0 && rt == TY_ENUMERATOR && g_n_argov < MAX_ARG_OVERRIDE &&
      ((sp_streq(name, "drop") && argc == 1 && nt_ref(nt, id, "block") < 0) ||
       ((sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "max_by") || sp_streq(name, "min_by") || sp_streq(name, "sort_by") ||
         sp_streq(name, "sum")) &&
        argc == 0 && nt_ref(nt, id, "block") >= 0))) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_Enumerator_to_a(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "", ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_POLY_ARRAY;
    emit_expr(c, id, b);
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  /* `poly.reject/select/filter { |x| ... }` where poly is an array read out of
     a container: coerce to a poly array and filter it into a fresh poly array,
     mirroring the find arm above (this TY_POLY receiver would otherwise skip to
     the loud NoMethodError). (#2930) */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "reject") || sp_streq(name, "select") || sp_streq(name, "filter"))) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int fbody = nt_ref(nt, fblock, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fbn >= 1) {
      int keep_truthy = !sp_streq(name, "reject");  /* select/filter keep truthy */
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
                 trecv, rb.p ? rb.p : "sp_box_nil()", name, trecv);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      for (int j = 0; j < fbn - 1; j++) emit_stmt(c, fbb[j], g_pre, g_indent + 1);
      int sv = g_indent; g_indent++;
      Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, fbb[fbn - 1], &cb); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "if (%s(%s)) sp_PolyArray_push(_t%d, %s);\n",
                 keep_truthy ? "" : "!", cb.p ? cb.p : "0", tres, es);
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      buf_printf(b, "_t%d", tres);
      return 1;
    }
  }
  /* `poly.each_index { |i| ... }` on a poly array read out of a container:
     iterate the index range, yield each index, and return self. (#2930) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "each_index") &&
      nt_ref(nt, id, "block") >= 0 && argc == 0) {
    int eb = nt_ref(nt, id, "block");
    const char *ip_orig = block_param_name(c, eb, 0);
    Scope *eic = comp_scope_of(c, eb);
    /* An unused index param is pruned by liveness (scope_local NULL, no lv_<name>
       declared): gate the binding so we never assign to an undeclared C name. */
    LocalVar *eilv = (ip_orig && eic) ? scope_local(eic, ip_orig) : NULL;
    int body = nt_ref(nt, eb, "body");
    int tself = ++g_tmp, trecv = ++g_tmp, ti = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
               tself, rb.p ? rb.p : "sp_box_nil()", tself);
    free(rb.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"each_index\"); SP_GC_ROOT(_t%d);\n",
               trecv, tself, trecv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
    if (eilv) {
      const char *ip = rename_local(ip_orig);
      emit_indent(g_pre, g_indent + 1);
      if (eilv->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
      else buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
    }
    emit_stmts(c, body, g_pre, g_indent + 1);
    emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
    buf_printf(b, "_t%d", tself);
    return 1;
  }
  /* `poly.sort_by { |k, v| ... }` where poly is a hash/array read out of a
     container: materialize its elements (a hash yields [k, v] pairs) as a poly
     array and re-dispatch as an array sort_by -- the array path's 2-param
     autosplat destructures each pair, matching the typed Hash#sort_by. (#2935) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "sort_by") &&
      nt_ref(nt, id, "block") >= 0 && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_to_a_arr(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_POLY_ARRAY;
    emit_call(c, id, b);
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  if (recv >= 0 && ty_is_array(rt)) {
    if (sp_streq(name, "pack") && argc == 1 &&
        (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_POLY_ARRAY || rt == TY_STR_ARRAY)) {
      const char *kind = rt == TY_POLY_ARRAY ? "Poly"
                       : rt == TY_STR_ARRAY  ? "Str"
                       : rt == TY_FLOAT_ARRAY ? "Float" : "Int";
      buf_printf(b, "sp_%sArray_pack(", kind);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* product(b, c, ...) with two or more array arguments: the n-way Cartesian
       product. The single-argument form is specialized below (per element-type
       boxing); for 2+ arguments box the receiver and every argument into rooted
       locals -- the GC is precise, so they must stay reachable across the
       helper's allocations -- and hand them to sp_poly_product as one vector. */
    if (sp_streq(name, "product") && argc >= 2) {
      int nn = argc + 1;
      int *ids = (int *)malloc(sizeof(int) * nn);
      if (!ids) { perror("malloc"); exit(1); }
      buf_puts(b, "({ ");
      ids[0] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", ids[0]); emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[0]);
      for (int i = 0; i < argc; i++) {
        ids[i + 1] = ++g_tmp;
        buf_printf(b, "sp_RbVal _t%d = ", ids[i + 1]); emit_boxed(c, argv[i], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[i + 1]);
      }
      buf_printf(b, "sp_RbVal _tp[%d] = { _t%d", nn, ids[0]);
      for (int i = 1; i < nn; i++) buf_printf(b, ", _t%d", ids[i]);
      buf_printf(b, " }; sp_poly_product(_tp, %d); })", nn);
      free(ids);
      return 1;
    }
    /* values_at(i, j, ...) -> fresh same-kind array of the picked elements
       (works for typed and poly arrays alike, and range args) */
    if (sp_streq(name, "values_at") && argc >= 1) {
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
        return 1;
      }
    }
    /* fetch_values(i, ...): like values_at but raises IndexError on an
       out-of-range index (#2321) */
    /* fetch_values(i, ...) { |i| fallback }: an out-of-range index takes the
       block's value instead of raising; the mixed result is a poly array. */
    if (sp_streq(name, "fetch_values") && argc >= 1 && nt_ref(nt, id, "block") >= 0 &&
        nt_type(nt, nt_ref(nt, id, "block")) &&
        sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode")) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int fblk = nt_ref(nt, id, "block");
        const char *fp0 = block_param_name(c, fblk, 0);
        const char *fp0r = fp0 ? rename_local(fp0) : NULL;
        int fbody = nt_ref(nt, fblk, "body");
        int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", to, to);
        for (int a = 0; a < argc; a++) {
          int ti = ++g_tmp;
          buf_printf(b, "{ mrb_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
          buf_printf(b, "; mrb_int _len = sp_%sArray_length(_t%d);"
                        " mrb_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
                        " if (_ix < 0 || _ix >= _len) { ",
                     an, tr, ti, ti, ti);
          if (fp0r) buf_printf(b, "lv_%s = _t%d; ", fp0r, ti);
          for (int j = 0; j + 1 < fbn; j++) emit_stmt(c, fbb[j], b, 0);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", to);
          if (fbn > 0) emit_boxed(c, fbb[fbn - 1], b); else buf_puts(b, "sp_box_nil()");
          buf_puts(b, "); } else { ");
          { char getx[96]; snprintf(getx, sizeof getx, "sp_%sArray_get(_t%d, _ix)", an, tr);
            buf_printf(b, "sp_PolyArray_push(_t%d, ", to);
            if (rt == TY_POLY_ARRAY) buf_puts(b, getx);
            else emit_boxed_text(c, ty_array_elem(rt), getx, b);
            buf_puts(b, "); } } "); }
        }
        buf_printf(b, "_t%d; })", to);
        return 1;
      }
    }
    if (sp_streq(name, "fetch_values") && argc >= 1) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d); ", an, to, an, to);
        for (int a = 0; a < argc; a++) {
          int ti = ++g_tmp;
          buf_printf(b, "{ mrb_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
          buf_printf(b, "; mrb_int _len = sp_%sArray_length(_t%d);"
                        " mrb_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
                        " if (_ix < 0 || _ix >= _len) sp_raise_cls(\"IndexError\","
                        " sp_sprintf(\"index %%lld outside of array bounds: %%lld...%%lld\","
                        " (long long)_t%d, (long long)-_len, (long long)_len));"
                        " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _ix)); } ",
                     an, tr, ti, ti, ti, ti, an, to, an, tr);
        }
        buf_printf(b, "_t%d; })", to);
        return 1;
      }
    }
    const char *k = array_kind(rt);
    /* drop(n) / take(n): subarrays via slice (all kinds incl. poly). */
    if ((sp_streq(name, "drop") || sp_streq(name, "take")) && argc == 1) {
      const char *dk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      if (dk) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", dk, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        /* a negative count raises ArgumentError; the no-block take/drop otherwise
           silently returns a slice (a tail slice for drop). */
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to %s negative size\");",
                   tn, name);
        if (sp_streq(name, "take"))
          buf_printf(b, " sp_%sArray_slice(_t%d, 0, _t%d); })", dk, t, tn);
        else
          buf_printf(b, " sp_%sArray_slice(_t%d, _t%d, _t%d->len - _t%d); })", dk, t, tn, t, tn);
        return 1;
      }
    }
    /* poly-array collection readers whose runtime backing already exists but
       whose typed-array forms live in the array_kind()-gated `if (k)` block
       below -- that gate is NULL for poly, so mirror them with explicit "Poly"
       dispatch (as drop/take above do). All return a fresh poly array. */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "reverse") && argc == 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_dup(", t); emit_expr(c, recv, b);
      buf_printf(b, "); sp_PolyArray_reverse_bang(_t%d); _t%d; })", t, t);
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "uniq") && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_dup(", t); emit_expr(c, recv, b);
      buf_printf(b, "); sp_PolyArray_uniq_bang(_t%d); _t%d; })", t, t);
      return 1;
    }
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) {
      /* first(n)/last(n) -> subarray via slice; a negative n is an ArgumentError. */
      int tn = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_PolyArray_slice(", tn);
      emit_expr(c, recv, b);
      if (sp_streq(name, "first")) buf_printf(b, ", 0, _t%d); })", tn);
      else                        buf_printf(b, ", -_t%d, _t%d); })", tn, tn);
      return 1;
    }
    /* poly-array max/min: boxed elements compared at runtime (numerics,
       strings, int-array tuples lexicographically). */
    if ((sp_streq(name, "max") || sp_streq(name, "min")) && argc == 0 &&
        rt == TY_POLY_ARRAY && nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* fill(val[, start[, len]]): fill a range with val, evaluate to self. */
    /* fill([start[, length]]) { |i| ... } / fill(range) { |i| ... }: the block
       form takes NO value argument -- the positional args are the index span and
       the value at each index comes from the block. (The no-block forms, where
       the first argument IS the value, are handled below.) */
    if (sp_streq(name, "fill") && argc <= 2 && nt_ref(nt, id, "block") >= 0) {
      const char *fk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
      int fblk = nt_ref(nt, id, "block");
      int fbody = nt_ref(nt, fblk, "body");
      int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
      if (fk && fbn > 0) {
        TyKind et = ty_array_elem(rt);
        int trecv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp, ti = ++g_tmp;
        const char *ip = block_param_name(c, fblk, 0); if (ip) ip = rename_local(ip);
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
        buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", tn, fk, trecv);
        /* resolve the [start, end) span from the arguments */
        int is_range = (argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE);
        if (is_range) {
          int tr = ++g_tmp;
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_Range _t%d = ", tr); emit_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;\n",
                     ts, tr, ts, ts, tn, ts, ts);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) + (_t%d.excl ? 0 : 1);\n",
                     te, tr, tr, tn, tr, tr);
        }
        else {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0;", ts);
          if (argc >= 1) { buf_printf(g_pre, " _t%d = ", ts); emit_int_expr(c, argv[0], g_pre);
                           buf_printf(g_pre, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts); }
          buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d;", te, tn);
          if (argc == 2) { buf_printf(g_pre, " { mrb_int _tl = "); emit_int_expr(c, argv[1], g_pre);
                           buf_printf(g_pre, "; if (_tl < 0) _tl = 0; _t%d = _t%d + _tl; }", te, ts); }
          buf_puts(g_pre, "\n");
        }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = _t%d; _t%d < _t%d; _t%d++) {\n", ti, ts, ti, te, ti);
        if (ip) {
          Scope *fic = comp_scope_of(c, fblk);
          LocalVar *filv = fic ? scope_local(fic, ip) : NULL;
          TyKind fit = filv ? filv->type : TY_INT;
          emit_indent(g_pre, g_indent + 1);
          if (fit == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", ip, ti);
          else buf_printf(g_pre, "lv_%s = _t%d;\n", ip, ti);
        }
        for (int bi = 0; bi < fbn - 1; bi++) {
          Buf sb; memset(&sb, 0, sizeof sb);
          emit_expr(c, fbb[bi], &sb);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, sb.p ? sb.p : ""); buf_puts(g_pre, ";\n"); free(sb.p);
        }
        Buf vb; memset(&vb, 0, sizeof vb);
        emit_expr(c, fbb[fbn - 1], &vb);
        emit_indent(g_pre, g_indent + 1);
        if (sp_streq(fk, "Poly")) {
          TyKind vt = comp_ntype(c, fbb[fbn - 1]);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, ", trecv, ti);
          if (vt != TY_POLY && vt != TY_UNKNOWN) emit_boxed_text(c, vt, vb.p ? vb.p : "sp_box_nil()", g_pre);
          else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed(c, fbb[fbn - 1], &bx);
                 buf_puts(g_pre, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
          buf_puts(g_pre, ");\n");
        }
        else buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, %s);\n", fk, trecv, ti, vb.p ? vb.p : "");
        free(vb.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", trecv);
        return 1;
      }
    }
    if (sp_streq(name, "fill") && argc >= 1 && argc <= 3) {
      /* a fill VALUE incompatible with the element type rebuilds through a
         poly array (inference typed the result poly to match); only literal
         and temp receivers reach this -- a conflicting fill on a LOCAL
         already widened the local itself at the write site */
      int fill_conflict = 0;
      {
        TyKind fe = ty_array_elem(rt), fv = comp_ntype(c, argv[0]);
        fill_conflict = rt != TY_POLY_ARRAY && fe != TY_POLY && fv != TY_UNKNOWN &&
                        fv != fe && !(ty_is_numeric(fv) && ty_is_numeric(fe));
      }
      const char *fk = (rt == TY_POLY_ARRAY || fill_conflict) ? "Poly" : k;
      TyKind fill_rt = fill_conflict ? TY_POLY_ARRAY : rt;
      if (fk) {
        int t = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", fk, t);
        if (fill_conflict) {
          buf_puts(b, "sp_poly_to_poly_array(");
          emit_boxed(c, recv, b);
          buf_puts(b, ")");
        }
        else emit_expr(c, recv, b);
        buf_puts(b, "; ");
        emit_ctype(c, ty_array_elem(fill_rt), b); buf_printf(b, " _t%d = ", tv);
        if (fill_rt == TY_POLY_ARRAY) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
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
          buf_printf(b, " mrb_int _t%d = ", ts); emit_int_expr(c, argv[1], b);
          buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
          if (argc == 3) {
            int tl = ++g_tmp;
            buf_printf(b, " mrb_int _t%d = ", tl); emit_int_expr(c, argv[2], b);
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
        return 1;
      }
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      /* fold via sp_poly_add so a Float (or Rational/Bignum) element promotes
         the result instead of being dropped by the int-only sum (#2627) */
      buf_puts(b, "sp_PolyArray_sum_poly("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      TyKind init_t = comp_ntype(c, argv[0]);
      /* an Array initial value concatenates one level ([[1],[2]].sum([])) */
      if (ty_is_array(init_t) ||
          (init_t == TY_UNKNOWN && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode"))) {
        buf_puts(b, "sp_PolyArray_sum_concat("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* a String initial value folds by concatenation ([str].sum("")) */
      if (init_t == TY_STRING) {
        buf_puts(b, "sp_PolyArray_sum_str("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* a Float initial value folds to a Float (bare mrb_float, not boxed) */
      if (init_t == TY_FLOAT) {
        buf_puts(b, "("); emit_float_expr(c, argv[0], b);
        buf_puts(b, " + sp_PolyArray_sum_float("); emit_expr(c, recv, b); buf_puts(b, "))");
        return 1;
      }
      buf_puts(b, "sp_box_int(");
      if (init_t == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { emit_expr(c, argv[0], b); }
      buf_puts(b, " + sp_PolyArray_sum_int("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "cycle") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp, tj = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); mrb_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr2, tr2);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
      buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->data[_t%d]);", tr2, t, ti2);
      buf_printf(b, " _t%d; })", tr2);
      return 1;
    }
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) {
      int t = ++g_tmp, tn2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); mrb_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn2);
      buf_printf(b, " if (_t%d > _t%d->len) _t%d = _t%d->len;", tn2, t, tn2, t);
      if (sp_streq(name, "pop"))
        buf_printf(b, " sp_PolyArray_slice_bang(_t%d, _t%d->len - _t%d, _t%d); })", t, t, tn2, tn2);
      else
        buf_printf(b, " sp_PolyArray_slice_bang(_t%d, 0, _t%d); })", t, tn2);
      return 1;
    }
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 0) {
      buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "dig") && argc >= 1) {
      if (argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_arr_get_hash(");
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_expr(c, argv[di], b); buf_puts(b, ")"); }
      }
      return 1;
    }
    /* concat(*arrays): append each argument array's elements onto the receiver
       in place, return the receiver. Coerce a typed-array argument to poly. */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "concat")) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
      /* evaluate (and root) every argument left-to-right BEFORE any append, so a
         side-effecting argument or one that reads the receiver sees pre-mutation
         state, per Ruby's arg-before-call evaluation order. */
      int base = g_tmp + 1; g_tmp += argc;
      for (int ai = 0; ai < argc; ai++) {
        TyKind at = comp_ntype(c, argv[ai]);
        const char *from = at == TY_INT_ARRAY   ? "sp_PolyArray_from_int_array"
                         : at == TY_STR_ARRAY   ? "sp_PolyArray_from_str_array"
                         : at == TY_FLOAT_ARRAY ? "sp_PolyArray_from_float_array" : NULL;
        buf_printf(b, " sp_PolyArray *_t%d = ", base + ai);
        if (from) { buf_printf(b, "%s(", from); emit_expr(c, argv[ai], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[ai], b);   /* already a poly array */
        buf_printf(b, "; SP_GC_ROOT(_t%d);", base + ai);
      }
      for (int ai = 0; ai < argc; ai++)
        buf_printf(b, " sp_PolyArray_append_all(_t%d, _t%d);", t, base + ai);
      buf_printf(b, " _t%d; })", t);
      return 1;
    }
    /* unshift/prepend(*elems): insert each element at the front (reverse order
       so the arg order is preserved), return the receiver. */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "unshift") || sp_streq(name, "prepend")) && argc >= 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
      /* evaluate (and root) every element left-to-right first, THEN insert them
         at the front in reverse so the arg order is preserved -- keeps Ruby's
         left-to-right evaluation independent of the receiver mutations. */
      int base = g_tmp + 1; g_tmp += argc;
      for (int ai = 0; ai < argc; ai++) {
        buf_printf(b, " sp_RbVal _t%d = ", base + ai); emit_boxed(c, argv[ai], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", base + ai);
      }
      for (int ai = argc - 1; ai >= 0; ai--)
        buf_printf(b, " sp_PolyArray_insert(_t%d, 0, _t%d);", t, base + ai);
      buf_printf(b, " _t%d; })", t);
      return 1;
    }
    /* rindex(obj): last matching index, or nil (SP_INT_NIL sentinel, matching
       the index/find_index int-or-nil convention). */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "rindex") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp;
      buf_printf(b, "({ mrb_int _t%d = sp_PolyArray_rindex(", t); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_printf(b, "); _t%d < 0 ? SP_INT_NIL : _t%d; })", t, t);
      return 1;
    }
    /* each_index { |i| ... } - iterate with index (works for all array kinds) */
    {
      int ei_blk = nt_ref(nt, id, "block");
      if (sp_streq(name, "each_index") && ei_blk >= 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          const char *ip = block_param_name(c, ei_blk, 0); if (ip) ip = rename_local(ip);
          int body = nt_ref(nt, ei_blk, "body");
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
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
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
    }
    /* take_while / drop_while (works for typed and poly arrays alike) */
    if ((sp_streq(name, "take_while") || sp_streq(name, "drop_while")) && argc == 0
        && nt_ref(nt, id, "block") >= 0) {
      int is_drop = sp_streq(name, "drop_while");
      int tw_blk = nt_ref(nt, id, "block");
      const char *tw_bp = block_param_name(c, tw_blk, 0); if (tw_bp) tw_bp = rename_local(tw_bp);
      int tw_body = nt_ref(nt, tw_blk, "body");
      int tw_bn = 0; const int *tw_bb = tw_body >= 0 ? nt_arr(nt, tw_body, "body", &tw_bn) : NULL;
      if (tw_bn > 0) {
        const char *ek = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (ek) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
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
          char es_tw[64]; snprintf(es_tw, sizeof es_tw, "sp_%sArray_get(_t%d, _t%d)", ek, trecv, ti);
          if (emit_iter_autosplat(c, tw_blk, rt, es_tw, g_indent + 1)) { }
          else if (tw_bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", tw_bp, ek, trecv, ti);
          }
          for (int j = 0; j < tw_bn - 1; j++) emit_stmt(c, tw_bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent = g_indent + 1;
          Buf cb = expr_buf(c, tw_bb[tw_bn - 1]); g_indent = sv;
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
          buf_printf(b, "_t%d", tout); return 1;
        }
      }
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "tally") && argc == 0) {
      buf_puts(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    /* tally(accumulator_hash): count into the given hash and return it, via the
       generic boxed helper (works for any hash variant, #2628). */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "tally") && argc == 1) {
      buf_puts(b, "sp_array_tally_into_poly("); emit_boxed(c, recv, b); buf_puts(b, ", ");
      emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "delete_at") && argc == 1) {
      buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* Array#delete(v) (value-based, not index-based) on TY_POLY_ARRAY --
       same array_kind()==NULL gating gap as delete_at above. The typed
       forms live inside the `if (k)` block below; poly arrays never get
       there. Needed for the array-backed Set package's #delete, whose
       @data widens to poly for mixed-element sets. */
    if (rt == TY_POLY_ARRAY && sp_streq(name, "delete") && argc == 1) {
      int dblk = nt_ref(nt, id, "block");
      if (dblk >= 0 && nt_type(nt, dblk) && sp_streq(nt_type(nt, dblk), "BlockNode")) {
        /* delete(v) { not-found value }: yield the block's value on a miss */
        int dbody = nt_ref(nt, dblk, "body");
        int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
        if (dbn >= 1) {
          int tdr = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = sp_PolyArray_delete(", tdr);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
          buf_printf(b, "); _t%d.tag != SP_TAG_NIL ? _t%d : ", tdr, tdr);
          emit_boxed(c, dbb[dbn - 1], b);
          buf_puts(b, "; })");
          return 1;
        }
      }
      buf_puts(b, "sp_PolyArray_delete("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* find / detect { |x| cond } on a poly array -> the element or nil. The
       typed-array forms live inside the `if (k)` block below, but array_kind is
       NULL for a poly array, so handle it here with the boxed element type. */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "find") || sp_streq(name, "detect"))) {
      int fblock = nt_ref(nt, id, "block");
      /* find(ifnone) { }: the proc is called on no-match, so its value (any
         type) rides the boxed result. A non-proc ifnone stays a loud reject. */
      int f_ifnone = argc == 1 && comp_ntype(c, argv[0]) == TY_PROC;
      if (fblock >= 0 && (argc == 0 || f_ifnone)) {
        const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, fblock, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tfn = f_ifnone ? ++g_tmp : -1;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "", trecv); free(rb.p);
          if (f_ifnone) {
            /* bind the ifnone proc up front (CRuby evaluates args first) plus
               a found flag: a matched nil element must NOT call the proc */
            Buf nb = expr_buf(c, argv[0]);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); int _tf%d = 0;\n",
                       tfn, nb.p ? nb.p : "NULL", tfn, tfn); free(nb.p);
          }
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tres, default_value(TY_POLY), tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          /* Declare the block param in the loop body so the form is self-contained
             when this find is a parameter default hoisted to the call site (whose
             function has no top-level declaration for the block local). */
          char es_fd[64]; snprintf(es_fd, sizeof es_fd, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          int splat_fd = emit_iter_autosplat(c, fblock, rt, es_fd, g_indent + 1);
          if (!splat_fd && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          {
            char fset[24] = "";
            if (f_ifnone) snprintf(fset, sizeof fset, " _tf%d = 1;", tfn);
            if (!splat_fd && bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s;%s break; }\n", cb.p ? cb.p : "0", tres, bp, fset);
            else buf_printf(g_pre, "if (%s) { _t%d = sp_PolyArray_get(_t%d, _t%d);%s break; }\n", cb.p ? cb.p : "0", tres, trecv, ti, fset);
          }
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (f_ifnone) {
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (mrb_int[16]){0}), _sp_proc_poly_ret);\n",
                       tfn, tres, tfn);
          }
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
    }
    /* find_index / index { |x| cond } on a poly array -> the index or nil.
       The typed-array form lives inside the `if (k)` block below; array_kind
       is NULL for a poly array, so handle it here. Unlike the int/str-array
       forms (which infer TY_POLY and box), index/find_index on a poly array
       infer as a plain nullable mrb_int -- return the bare SP_INT_NIL
       sentinel, don't box. */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "find_index") || sp_streq(name, "index")) &&
        nt_ref(nt, id, "block") >= 0) {
      int fblock = nt_ref(nt, id, "block");
      const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
      int body = nt_ref(nt, fblock, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn >= 1) {
        int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                   ti, ti, trecv, ti);
        /* Declare the block param in the loop body so the form is self-contained
           (same rationale as find/detect above); a |k, v| header destructures
           the pair element (#1876 family). */
        {
          char es_fi[64]; snprintf(es_fi, sizeof es_fi, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          int splat_fi = emit_iter_autosplat(c, fblock, rt, es_fi, g_indent + 1);
          if (!splat_fi && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
        }
        for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
        int sv = g_indent; g_indent++;
        Buf cb; memset(&cb, 0, sizeof cb); emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
        free(cb.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tres);
        return 1;
      }
    }
    /* index(v) / find_index(v) on a poly array (no block) -> the first
       position whose element == v (sp_poly_eq), or nil (SP_INT_NIL),
       mirroring the count(v)/any?(v) idiom (doom: @map.sectors.index(sector)). */
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "index") || sp_streq(name, "find_index")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) {
      int trecv = ++g_tmp, ta = ++g_tmp, tres = ++g_tmp, ti = ++g_tmp;
      Buf ra = expr_buf(c, recv);
      /* Root the receiver and the boxed needle: sp_poly_eq can allocate
         (bigint promotion), so a collection may run mid-loop. */
      buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", trecv, ra.p ? ra.p : "NULL", trecv); free(ra.p);
      buf_printf(b, " sp_RbVal _t%d = ", ta); emit_boxed(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ta);
      buf_printf(b, " mrb_int _t%d = SP_INT_NIL;", tres);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, trecv, ti);
      buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) { _t%d = _t%d; break; }",
                 trecv, ti, ta, tres, ti);
      buf_printf(b, " _t%d; })", tres);
      return 1;
    }
    if (sp_streq(name, "insert") && argc == 2 && rt == TY_POLY_ARRAY) {
      /* poly array (outside the typed-kind block -- array_kind(POLY_ARRAY) is
         NULL): the inserted value boxes into the sp_RbVal slot */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PolyArray_insert(_t%d, ", t); emit_int_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_printf(b, "); _t%d; })", t);
      return 1;
    }
    if (sp_streq(name, "delete_at") && argc == 1 && rt == TY_POLY_ARRAY) {
      buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (k) {
      if ((sp_streq(name, "to_a") || sp_streq(name, "to_ary") || sp_streq(name, "entries") ||
           sp_streq(name, "deconstruct") ||
           sp_streq(name, "flatten") || sp_streq(name, "compact")) && argc == 0) {
        /* a scalar-element array can't nest or hold nil: these are identity */
        emit_expr(c, recv, b); return 1;
      }
      if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_printf(b, "sp_%sArray_slice_range(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 2) {
        /* arr[start, len] -> subarray; a negative length is nil in CRuby
           (slice() itself would return the empty array), and so is a start
           outside [-len, len] (start == len is the empty slice) */
        int ta = ++g_tmp, ts = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", ts); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = ", tl); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d)", tn, k, ta);
        buf_printf(b, "; (_t%d < 0 || _t%d > _t%d || _t%d < -_t%d) ? (sp_%sArray *)0 : sp_%sArray_slice(_t%d, _t%d, _t%d); })",
                   tl, ts, tn, ts, tn, k, k, ta, ts, tl);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* arr[range] where the range is a variable/param (a literal RangeNode is
           folded above). Resolve beginless (INTPTR_MIN), endless (INTPTR_MAX),
           and negative endpoints against the length, then slice -- a start
           outside [-len, len] is nil, matching Array#[]. */
        int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " mrb_int _t%d = _t%d.first == INTPTR_MIN ? 0 :"
                      " (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
                   tf, tr, tr, tr, tn, tr);
        buf_printf(b, " mrb_int _t%d = _t%d.last == INTPTR_MAX ? _t%d - _t%d :"
                      " ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
                   tl, tr, tn, tf, tr, tr, tn, tr, tf, tr);
        /* a start before the array (`first < -len`, so the resolved `_tf` is
           still negative) or past its end (`_tf > len`) is nil in Ruby, not a
           clamped slice; `_tf == len` is the empty slice, which slice() yields. */
        buf_printf(b, " (_t%d < 0 || _t%d > _t%d) ? (sp_%sArray *)0 : sp_%sArray_slice(_t%d, _t%d, _t%d); })",
                   tf, tf, tn, k, k, ta, tf, tl);
        return 1;
      }
      if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) {
        buf_printf(b, "sp_%sArray_get(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (infer_type(c, argv[0]) == TY_POLY) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", t);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.v.i; })", t);
        }
else {
          /* emit_int_expr, not raw: an unresolved-constant index lowers to a
             NameError raise whose C value is an sp_Class, which the int slot
             rejects at C compile time. */
          emit_int_expr(c, argv[0], b);
        }
        buf_puts(b, ")");
        return 1;
      }
      /* concat(*arrays) as a value: append in place, evaluate to the
         receiver (the mutating statement form lives in emit_array_mutate_stmt).
         Same-kind arguments only; a differently-typed argument has already
         widened the receiver to poly in inference. */
      if (sp_streq(name, "concat") && argc >= 1) {
        int same = 1;
        for (int j = 0; j < argc; j++) if (comp_ntype(c, argv[j]) != rt) { same = 0; break; }
        if (same) {
          int ta = ++g_tmp;
          Buf ra = expr_buf(c, recv);
          buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
          free(ra.p);
          int base = g_tmp + 1; g_tmp += argc;
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " sp_%sArray *_t%d = ", k, base + j);
            emit_expr(c, argv[j], b);
            buf_printf(b, "; SP_GC_ROOT(_t%d);", base + j);
          }
          for (int j = 0; j < argc; j++) {
            int ii = ++g_tmp, sn = ++g_tmp;
            buf_printf(b, " { mrb_int _t%d = sp_%sArray_length(_t%d);"
                          " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                          " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d)); }",
                       sn, k, base + j, ii, ii, sn, ii, k, ta, k, base + j, ii);
          }
          buf_printf(b, " _t%d; })", ta);
          return 1;
        }
      }
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
        int blk = nt_ref(nt, id, "block");
        TyKind et = ty_array_elem(rt);
        /* the whole expression's inferred type: poly when the default (or
           block value) type differs from the element type -- box both arms */
        TyKind ft = comp_ntype(c, id);
        int boxed = ft != et;
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_%sArray *_t%d = %s;", k, ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? ", tnorm, tnorm, tn);
        if (boxed) {
          char getexpr[96];
          snprintf(getexpr, sizeof getexpr, "sp_%sArray_get(_t%d, _t%d)", k, ta, tnorm);
          emit_boxed_text(c, et, getexpr, b);
        }
        else buf_printf(b, "sp_%sArray_get(_t%d, _t%d)", k, ta, tnorm);
        buf_puts(b, " :");
        if (argc == 2) {
          buf_puts(b, " ");
          if (boxed && comp_ntype(c, argv[1]) != TY_POLY) emit_boxed(c, argv[1], b);
          else emit_expr(c, argv[1], b);
          buf_puts(b, "; })");
        }
        else if (blk >= 0) {
          /* fetch(i) { |i| default }: an out-of-bounds index yields the
             (original) index to the block; its value is the result */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          buf_puts(b, " ({ ");
          const char *fp0 = block_param_name(c, blk, 0);
          if (fp0) buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), ti);
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], b, 0);
          if (bval >= 0) {
            if (boxed && comp_ntype(c, bval) != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, boxed ? "sp_box_nil()" : default_value(et));
          buf_puts(b, "; }); })");
        }
        else {
          /* CRuby's message names the index and the valid bounds */
          buf_printf(b, " (sp_raise_cls(\"IndexError\","
                        " sp_sprintf(\"index %%lld outside of array bounds: -%%lld...%%lld\","
                        " (long long)_t%d, (long long)_t%d, (long long)_t%d)), %s); })",
                     ti, tn, tn, boxed ? "sp_box_nil()" : default_value(et));
        }
        return 1;
      }
      if (sp_streq(name, "dig") && argc >= 1) {
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
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        /* array + array of the same kind -> a fresh concatenation. An empty
           literal `[]` arg (TY_UNKNOWN) concatenates a null (copies recv). */
        buf_printf(b, "sp_%sArray_concat(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != rt) {
        /* array + different-kind array -> poly_array */
        const char *k2 = (a0 == TY_POLY_ARRAY) ? "Poly" : array_kind(a0);
        if (k2) {
          int tL = ++g_tmp, tR = ++g_tmp, tO = ++g_tmp, ti = ++g_tmp;
          Buf lbuf = expr_buf(c, recv);
          Buf rbuf = expr_buf(c, argv[0]);
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
          return 1;
        }
      }
      if (sp_streq(name, "clear") && argc == 0) {
        /* empty the array in place, evaluate to it (Ruby returns self) */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d && _t%d->frozen) sp_raise_cls(\"FrozenError\", sp_sprintf(\"can't modify frozen Array: %%s\", sp_%sArray_inspect(_t%d))); if (_t%d) _t%d->len = 0; _t%d; })", t, t, k, t, t, t, t);
        return 1;
      }
      if (sp_streq(name, "cycle") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* blockless cycle(n): the receiver repeated n times, materialized */
        int t = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp, tj = ++g_tmp, ti2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);", k, tr2, k, tr2);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
        buf_printf(b, " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));", k, tr2, k, t, ti2);
        buf_printf(b, " _t%d; })", tr2);
        return 1;
      }
      if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) {
        /* pop(n)/shift(n): the removed subarray, via the slice! splice
           (pop takes the tail, shift the head; n clamps to the length) */
        int t = ++g_tmp, tn2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn2);
        buf_printf(b, " if (_t%d > _t%d->len) _t%d = _t%d->len;", tn2, t, tn2, t);
        if (sp_streq(name, "pop"))
          buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d->len - _t%d, _t%d); })", k, t, t, tn2, tn2);
        else
          buf_printf(b, " sp_%sArray_slice_bang(_t%d, 0, _t%d); })", k, t, tn2);
        return 1;
      }
      if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 0) {
        /* remove and return first/last element (nil sentinel when empty) */
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "unshift") || sp_streq(name, "prepend")) && argc >= 1) {
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
          /* FloatArray (the only other element kind that reaches this typed
             dispatch; poly arrays route elsewhere). Evaluate the arguments
             left to right into temporaries (Ruby's argument-evaluation order),
             then prepend them in reverse so a multi-arg unshift keeps order. */
          buf_printf(b, "({ sp_FloatArray *_t%d = ", t); emit_expr(c, recv, b); buf_puts(b, ";");
          for (int a = 0; a < argc; a++) {
            buf_printf(b, " mrb_float _u%d_%d = ", t, a); emit_float_expr(c, argv[a], b); buf_puts(b, ";");
          }
          for (int a = argc - 1; a >= 0; a--) {
            buf_printf(b, " sp_FloatArray_unshift(_t%d, _u%d_%d);", t, t, a);
          }
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      /* non-mutating copy-then-operate methods */
      if (sp_streq(name, "shuffle") && argc == 0) {
        buf_printf(b, "sp_%sArray_shuffle(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* in-place mutators that return self (raise FrozenError when frozen) */
      {
        const char *base = NULL;
        if      (sp_streq(name, "reverse!")) base = "reverse_bang";
        else if (sp_streq(name, "sort!"))    base = "sort_bang";
        else if (sp_streq(name, "shuffle!")) base = "shuffle_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sArray_%s(_t%d); _t%d; })", k, base, t, t);
          return 1;
        }
      }
      if (sp_streq(name, "uniq!") && argc == 0 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* value form: self when changed, nil when a no-op (CRuby) */
        buf_printf(b, "sp_%sArray_uniq_bangq(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "flatten!") || sp_streq(name, "compact!")) && argc == 0 &&
          (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
        /* a typed array can hold neither sub-arrays nor nils: both bangs are
           always a no-op, and CRuby's no-op contract is nil */
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())");
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && (argc == 0 || argc == 1)) {
        /* a real copy: arrays are mutable, so dup/clone must not alias.
           clone (unlike dup) carries the frozen flag over; the freeze:
           keyword forces it. */
        int fz = -1;  /* -1: dup semantics; -2: copy receiver's flag; 0/1: forced */
        if (argc == 0) fz = sp_streq(name, "clone") ? -2 : -1;
        else if (sp_streq(name, "clone") && nt_type(nt, argv[0]) &&
                 sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
          int fv = kwh_lookup(nt, argv[0], "freeze");
          const char *fvt = fv >= 0 ? nt_type(nt, fv) : NULL;
          if (fvt && sp_streq(fvt, "FalseNode")) fz = 0;
          else if (fvt && sp_streq(fvt, "TrueNode")) fz = 1;
          else if (fvt && sp_streq(fvt, "NilNode")) fz = -2;
        }
        if (argc == 1 && fz == -1) { /* not a recognized keyword: fall through */ }
        else if (fz == -1) {
          buf_printf(b, "sp_%sArray_dup(", k); emit_expr(c, recv, b); buf_puts(b, ")");
          return 1;
        }
        else {
          int ts = ++g_tmp, td = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, ts); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_dup(_t%d); ", k, td, k, ts);
          if (fz == -2) buf_printf(b, "_t%d->frozen = _t%d ? _t%d->frozen : 0; ", td, ts, ts);
          else buf_printf(b, "_t%d->frozen = %d; ", td, fz);
          buf_printf(b, "_t%d; })", td);
          return 1;
        }
      }
      if (sp_streq(name, "reverse") && argc == 0) {
        /* copy + reverse in place; sp_*Array_dup exists for Int/Str/Float/Poly */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_dup(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); sp_%sArray_reverse_bang(_t%d); _t%d; })", k, t, t);
        return 1;
      }
      if (sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        /* recv.zip(b, c...) → [[recv[0],b[0],c[0],...], ...] as PolyArray of PolyArrays */
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        const char *ka = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        buf_printf(b, "({ sp_%sArray *_t%d = ", ka, ta); emit_expr(c, recv, b); buf_puts(b, ";");
        for (int j = 0; j < nargs; j++) {
          /* a Range argument materializes to its int array */
          if (at[j] == TY_RANGE) {
            int trj = ++g_tmp;
            buf_printf(b, " sp_IntArray *_t%d = ({ sp_Range _t%d = ", tb[j], trj);
            emit_expr(c, argv[j], b);
            buf_printf(b, "; sp_range_to_ia(_t%d); });", trj);
            at[j] = TY_INT_ARRAY;
            continue;
          }
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
        return 1;
      }
      /* product(other) { |pair| }: yield each pair to the block, evaluate to
         the receiver (CRuby returns self) */
      if (sp_streq(name, "product") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        TyKind at = comp_ntype(c, argv[0]);
        const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        const char *fp0 = block_param_name(c, blk, 0);
        int ta = ++g_tmp, tb = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        Buf ra; memset(&ra, 0, sizeof ra); Buf rb2; memset(&rb2, 0, sizeof rb2);
        emit_expr(c, recv, &ra); emit_expr(c, argv[0], &rb2);
        buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);",
                   k, ta, ra.p ? ra.p : "NULL", ta, kb, tb, rb2.p ? rb2.p : "NULL", tb);
        free(ra.p); free(rb2.p);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tpair, tpair);
        char e1[96], e2[96];
        snprintf(e1, sizeof e1, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
        snprintf(e2, sizeof e2, "sp_%sArray_get(_t%d, _t%d)", kb, tb, tj);
        buf_printf(b, " sp_PolyArray_push(_t%d, ", tpair);
        emit_boxed_text(c, ty_array_elem(rt), e1, b);
        buf_printf(b, "); sp_PolyArray_push(_t%d, ", tpair);
        emit_boxed_text(c, ty_array_elem(at), e2, b);
        buf_puts(b, ");");
        if (fp0) buf_printf(b, " lv_%s = sp_box_poly_array(_t%d);", rename_local(fp0), tpair);
        buf_puts(b, " {");
        for (int j2 = 0; j2 < bn; j2++) emit_stmt(c, bb[j2], b, 0);
        buf_printf(b, " } } } _t%d; })", ta);
        return 1;
      }
      if ((sp_streq(name, "flatten!") || sp_streq(name, "flatten")) && argc == 1) {
        /* a typed (scalar-element) array has no nesting: flatten(n) copies,
           flatten!(n) is a no-op returning nil */
        if (name[7] == '!') {
          buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
          emit_int_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
        }
        else {
          buf_puts(b, "((void)(");
          emit_int_expr(c, argv[0], b);
          buf_printf(b, "), sp_%sArray_dup(", k);
          emit_expr(c, recv, b);
          buf_puts(b, "))");
        }
        return 1;
      }
      if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* product with no arguments: each element wrapped in its own array */
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
        free(ra.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); sp_PolyArray_push(_t%d, ", te, te, te);
        char ee[96]; snprintf(ee, sizeof ee, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
        emit_boxed_text(c, ty_array_elem(rt), ee, b);
        buf_printf(b, "); sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr, te);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "product") && argc == 1) {
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
        return 1;
      }
      if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
           sp_streq(name, "permutation") || sp_streq(name, "repeated_permutation")) &&
          (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
          rt == TY_INT_ARRAY && nt_ref(nt, id, "block") < 0) {
        const char *combfn = sp_streq(name, "combination") ? "sp_IntArray_combination"
                           : sp_streq(name, "permutation") ? "sp_IntArray_permutation"
                           : sp_streq(name, "repeated_permutation") ? "sp_IntArray_repeated_permutation"
                           : "sp_IntArray_repeated_combination";
        int ta = ++g_tmp, tc = ++g_tmp, tout = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_IntArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_PtrArray *_t%d = %s(_t%d, ", tc, combfn, ta);
        if (argc == 1) emit_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);   /* argless permutation: full length */
        buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tout, tout);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tc, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int_array(_t%d->data[_t%d]));", tout, tc, ti);
        buf_printf(b, " _t%d; })", tout);
        return 1;
      }
      if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
           sp_streq(name, "permutation") || sp_streq(name, "repeated_permutation")) &&
          (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
          nt_ref(nt, id, "block") < 0) {
        /* any other element kind rides the boxed PolyArray implementation */
        const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                           : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                           : sp_streq(name, "repeated_permutation") ? "sp_PolyArray_repeated_permutation"
                           : "sp_PolyArray_repeated_combination";
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_poly_to_poly_array(", ta);
        emit_boxed(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); %s(_t%d, ", ta, combfn, ta);
        if (argc == 1) emit_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
        buf_puts(b, "); })");
        return 1;
      }
      if (sp_streq(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_rotate_bang(_t%d, ", k, t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && a0 == rt) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_replace(_t%d, ", k, t); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "insert") && argc >= 2 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* insert(i, v1, v2, ...): normalize a negative index ONCE against the
           pre-insert length (per-element normalization would drift as the
           array grows), then insert consecutively. */
        int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
        /* normalize ONCE, keeping the too-negative IndexError the runtime
           helper would have raised (it must not see a pre-added index) */
        buf_printf(b, "; mrb_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
                      " if (_t%d < 0) sp_raise_cls(\"IndexError\","
                      " sp_sprintf(\"index %%lld too small for array; minimum: %%lld\","
                      " (long long)_t%d, (long long)(-((_t%d ? _t%d->len : 0) + 1)))); }",
                   to2, ti2, ti2, ti2, t, t, ti2, to2, t, t);
        for (int a2 = 1; a2 < argc; a2++) {
          buf_printf(b, " sp_%sArray_insert(_t%d, _t%d + %d, ", k, t, ti2, a2 - 1);
          emit_expr(c, argv[a2], b); buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "delete_at") && argc == 1) {
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        int dblk = nt_ref(nt, id, "block");
        if (dblk >= 0 && nt_type(nt, dblk) && sp_streq(nt_type(nt, dblk), "BlockNode")) {
          int dbody = nt_ref(nt, dblk, "body");
          int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
          if (dbn >= 1) {
            int tdr = ++g_tmp;
            if (rt == TY_INT_ARRAY) {
              buf_printf(b, "({ mrb_int _t%d = sp_IntArray_delete(", tdr);
              emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
              buf_printf(b, "); _t%d != SP_INT_NIL ? sp_box_int(_t%d) : ", tdr, tdr);
            }
            else {
              buf_printf(b, "({ const char *_t%d = sp_StrArray_delete(", tdr);
              emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
              buf_printf(b, "); _t%d ? sp_box_str(_t%d) : ", tdr, tdr);
            }
            emit_boxed(c, dbb[dbn - 1], b);
            buf_puts(b, "; })");
            return 1;
          }
        }
        buf_printf(b, "sp_%sArray_delete(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "tally") && argc == 0) {
        if (rt == TY_INT_ARRAY) { buf_printf(b, "sp_IntArray_tally_int("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
        if (rt == TY_STR_ARRAY) { buf_printf(b, "sp_StrArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
        if (rt == TY_POLY_ARRAY) { buf_printf(b, "sp_PolyArray_tally("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      }
      /* tally(hash): count INTO the given accumulator (any hash variant, boxed)
         and return it (#2533). The accumulator's own type is immaterial at run
         time -- it is a Hash. */
      if (sp_streq(name, "tally") && argc == 1) {
        buf_puts(b, "sp_array_tally_into_poly("); emit_boxed(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 2) {
        /* slice!(start, len): remove and return the subarray (raises
           FrozenError inside the runtime helper when the array is frozen) */
        buf_printf(b, "sp_%sArray_slice_bang(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* slice!(range): normalize begin/length against the live length */
        int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = _t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first;",
                   tf, tr, tr, ta, ta, tr);
        buf_printf(b, " mrb_int _t%d = (_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                   tn, tr, tr, ta, ta, tr, tf, tr);
        buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", k, ta, tf, tn, tn);
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1) {
        /* slice!(i): remove and return the element (nil sentinel on miss) */
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      int block = nt_ref(nt, id, "block");
      /* bsearch { |x| cond } on typed arrays - find-minimum mode */
      if (sp_streq(name, "bsearch") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1;\n", tlo, thi, k, trecv);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tres,
                     et == TY_INT ? "SP_INT_NIL" :
                     et == TY_FLOAT ? "sp_float_nil()" : "NULL");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          /* An Integer-valued block selects find-ANY mode (CRuby dispatches on
             the block value's kind): 0 means found, negative searches left,
             positive right. A boolean block is find-minimum, as before. */
          if (comp_ntype(c, bb[bn - 1]) == TY_INT) {
            int tcmp = ++g_tmp;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "mrb_int _t%d = %s;\n", tcmp, cb.p ? cb.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (_t%d == 0) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                       tcmp, tres, k, trecv, tmid);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "else if (_t%d < 0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          }
          else if (comp_ntype(c, bb[bn - 1]) == TY_POLY) {
            /* mixed block: Integer is find-any (0 found, positive right,
               negative left), other truthy is find-min, nil/false right */
            int tv = ++g_tmp;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_RbVal _t%d = %s;\n", tv, cb.p ? cb.p : "sp_box_nil()");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (_t%d.tag == SP_TAG_INT) {\n", tv);
            emit_indent(g_pre, g_indent + 2);
            buf_printf(g_pre, "if (_t%d.v.i == 0) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                       tv, tres, k, trecv, tmid);
            emit_indent(g_pre, g_indent + 2);
            buf_printf(g_pre, "else if (_t%d.v.i > 0) { _t%d = _t%d + 1; }\n", tv, tlo, tmid);
            emit_indent(g_pre, g_indent + 2);
            buf_printf(g_pre, "else { _t%d = _t%d - 1; }\n", thi, tmid);
            emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "else if (sp_poly_truthy(_t%d)) { _t%d = sp_%sArray_get(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                       tv, tres, k, trecv, tmid, thi, tmid);
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          }
          else {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); _t%d = _t%d - 1; }\n",
                       cb.p ? cb.p : "0", tres, k, trecv, tmid, thi, tmid);
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          }
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* bsearch_index { |x| cond }: find-minimum binary search returning the
         index of the first element satisfying the predicate, or nil. */
      if (sp_streq(name, "bsearch_index") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1, _t%d = SP_INT_NIL;\n", tlo, thi, k, trecv, tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "mrb_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* The block value is the search predicate: route through emit_cond so a
             poly / nullable-scalar result becomes a valid C truthiness test rather
             than `if (sp_RbVal)` or `if (SP_INT_NIL)`. */
          Buf cb; memset(&cb, 0, sizeof cb);
          /* Integer-valued block: find-ANY mode (0 found, <0 left, >0 right),
             yielding the index. Boolean block: find-minimum, as before. */
          if (comp_ntype(c, bb[bn - 1]) == TY_INT) {
            /* g_indent was already bumped for the block-body statements above;
               evaluate the comparator at that depth, then restore. */
            Buf ib = expr_buf(c, bb[bn - 1]); g_indent = sv;
            int tcmp = ++g_tmp;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "mrb_int _t%d = %s;\n", tcmp, ib.p ? ib.p : "0");
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "if (_t%d == 0) { _t%d = _t%d; break; }\n", tcmp, tres, tmid);
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "else if (_t%d < 0) { _t%d = _t%d - 1; }\n", tcmp, thi, tmid);
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
            free(ib.p);
            emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
            buf_printf(b, "_t%d", tres); return 1;
          }
          emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; _t%d = _t%d - 1; }\n", cb.p ? cb.p : "0", tres, tmid, thi, tmid);
          free(cb.p);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "else { _t%d = _t%d + 1; }\n", tlo, tmid);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* find_index { |x| cond } / index { |x| cond } / rindex { |x| cond } on
         typed arrays - returns the index or nil (rindex scans from the end). */
      if ((sp_streq(name, "find_index") || sp_streq(name, "index") ||
           sp_streq(name, "rindex")) && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rfi = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rfi.p ? rfi.p : "NULL"); free(rfi.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = SP_INT_NIL;\n", tres);
          emit_indent(g_pre, g_indent);
          if (sp_streq(name, "rindex"))
            buf_printf(g_pre, "for (mrb_int _t%d = sp_%sArray_length(_t%d) - 1; _t%d >= 0; _t%d--) {\n",
                       ti, k, trecv, ti, ti);
          else
            buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                       ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb.p ? cb.p : "0", tres, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tres, tres);
          return 1;
        }
      }
      /* find(ifnone) { |x| cond } on a typed array: the element (boxed) or
         the ifnone proc's value on no-match; the result rides poly since the
         proc can return anything. A non-proc ifnone stays a loud reject. */
      if ((sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0 &&
          argc == 1 && comp_ntype(c, argv[0]) == TY_PROC) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tfn = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          /* root the receiver: the block body and the ifnone proc run arbitrary
             Ruby that can trigger GC while this array is live (as the poly-array
             find(ifnone) path already does) */
          buf_printf(g_pre, " _t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "", trecv); free(rb.p);
          { Buf nb = expr_buf(c, argv[0]);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_Proc *_t%d = %s; SP_GC_ROOT(_t%d); int _tf%d = 0;\n",
                       tfn, nb.p ? nb.p : "NULL", tfn, tfn); free(nb.p); }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d);\n", tres, tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (%s) { _t%d = ", cb.p ? cb.p : "0", tres);
          { char eltxt[128];
            if (bp) snprintf(eltxt, sizeof eltxt, "lv_%s", bp);
            else snprintf(eltxt, sizeof eltxt, "sp_%sArray_get(_t%d, _t%d)", k, trecv, ti);
            emit_boxed_text(c, et, eltxt, g_pre); }
          buf_printf(g_pre, "; _tf%d = 1; break; }\n", tfn);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (mrb_int[16]){0}), _sp_proc_poly_ret);\n",
                     tfn, tres, tfn);
          buf_printf(b, "_t%d", tres);
          return 1;
        }
      }
      /* find / detect { |x| cond } - returns element or nil */
      if ((sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0 && argc == 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          if (et == TY_STRING) buf_printf(g_pre, " _t%d = NULL;\n", tres);
          else if (et == TY_INT) buf_printf(g_pre, " _t%d = SP_INT_NIL;\n", tres);
          else buf_printf(g_pre, " _t%d = 0;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          /* Declare the block param in the loop body (not a bare assignment) so
             the find is self-contained: when this call is a parameter default
             hoisted to the call site, the enclosing function has no top-level
             declaration for the block local. Shadows the method-scope slot in
             the ordinary in-body case, which is harmless. */
          if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          if (bp) buf_printf(g_pre, "if (%s) { _t%d = lv_%s; break; }\n", cb.p ? cb.p : "0", tres, bp);
          else buf_printf(g_pre, "if (%s) { _t%d = sp_%sArray_get(_t%d, _t%d); break; }\n",
                          cb.p ? cb.p : "0", tres, k, trecv, ti);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tres); return 1;
        }
      }
      /* map! / collect! { |x| body } - in-place transform, returns receiver */
      if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && block >= 0) {
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *ms = comp_scope_of(c, block);
          LocalVar *mlv = (ms && bp0) ? scope_local(ms, bp0) : NULL;
          TyKind msaved = mlv ? mlv->type : TY_UNKNOWN;
          if (mlv) { mlv->type = et; for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
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
          Buf vb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sArray_set(_t%d, _t%d, %s);\n", k, trecv, ti, vb.p ? vb.p : "0");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          if (mlv) mlv->type = msaved;
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond } - in-place
         filter. Works on typed (int/str/float) AND poly arrays. */
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && block >= 0) {
        int is_rej = sp_streq(name, "reject!") || sp_streq(name, "delete_if");
        const char *kk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        const char *bp0 = block_param_name(c, block, 0);
        const char *bp = bp0 ? rename_local(bp0) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (kk && bn >= 1) {
          TyKind et = ty_array_elem(rt);
          Scope *fs = comp_scope_of(c, block);
          LocalVar *flv = (fs && bp0) ? scope_local(fs, bp0) : NULL;
          TyKind fsaved = flv ? flv->type : TY_UNKNOWN;
          if (flv) { flv->type = et; for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]); }
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp, torig = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = sp_%sArray_length(_t%d);\n", torig, kk, trecv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, kk, trecv, ti);
          emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _telt%d = sp_%sArray_get(_t%d, _t%d);\n", ti, kk, trecv, ti);
          if (bp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre);
            buf_printf(g_pre, " lv_%s = _telt%d;\n", bp, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          TyKind cty = comp_ntype(c, bb[bn - 1]);
          /* Ruby truthiness on the predicate value: only nil/false are falsy, so a
             nilable int/float reads falsy at its sentinel but 0/0.0 stay truthy.
             Mirrors emit_each_with_index_terminal's select/reject test. */
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          if (cty == TY_POLY)       buf_printf(g_pre, "sp_poly_truthy(%s)", cb.p ? cb.p : "sp_box_nil()");
          else if (cty == TY_INT)   buf_printf(g_pre, "((%s) != SP_INT_NIL)", cb.p ? cb.p : "0");
          else if (cty == TY_FLOAT) buf_printf(g_pre, "(!sp_float_is_nil(%s))", cb.p ? cb.p : "0");
          else                      buf_printf(g_pre, "(%s)", cb.p ? cb.p : "0");
          buf_printf(g_pre, ") { sp_%sArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     kk, trecv, twp, ti, twp);
          free(cb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          if (flv) flv->type = fsaved;
          char box[64]; snprintf(box, sizeof box, "%s(_t%d)", array_box_fn(kk), trecv);
          emit_filter_bang_result(name, trecv, torig, twp, box, b);
          return 1;
        }
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* scalar-element arrays never hold nil/false: predicate is length-based */
        const char *op = sp_streq(name, "all?") ? ">= 0" : sp_streq(name, "any?") ? "> 0"
                       : sp_streq(name, "none?") ? "== 0" : "== 1";
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_printf(b, ") %s)", op);
        return 1;
      }
      /* array.none?(a..b) / any?/all?/one? with a Range pattern -- membership
         test (===) over an integer array. */
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0 &&
          rt == TY_INT_ARRAY && comp_ntype(c, argv[0]) == TY_RANGE) {
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_IntArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_Range _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_range_include(&_t%d, sp_IntArray_get(_t%d, _t%d))) _t%d++;", tv, ta, ti, tc);
        if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == sp_IntArray_length(_t%d); })", tc, ta);
        else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
        else                              buf_printf(b, " _t%d == 1; })", tc);
        return 1;
      }
      /* array.none?(/re/) / any?/all?/one? with a Regexp pattern over strings. */
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0 &&
          rt == TY_STR_ARRAY && re_lit_index(c, argv[0]) >= 0) {
        int rei = re_lit_index(c, argv[0]);
        int ta = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_StrArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_re_match(sp_re_pat_%d, sp_StrArray_get(_t%d, _t%d)) >= 0) _t%d++;", rei, ta, ti, tc);
        if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == sp_StrArray_length(_t%d); })", tc, ta);
        else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
        else                              buf_printf(b, " _t%d == 1; })", tc);
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") || sp_streq(name, "none?") ||
           sp_streq(name, "one?") || sp_streq(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* array.all?(v)/any?(v)/none?(v)/one?(v)/count(v) -- compare by == */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
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
        if (sp_streq(name, "all?"))        buf_printf(b, " _t%d == sp_%sArray_length(_t%d); })", tc, k, ta);
        else if (sp_streq(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } -- loop and count truthy block results */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2 = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          /* The block value is a condition: route through emit_cond so a poly /
             nil / scalar predicate becomes a valid C truthiness test (e.g.
             `count(&:alive)` where the element method is poly-dispatched would
             otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_cond(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return 1;
        }
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sArray_length(", k); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        TyKind init_t = comp_ntype(c, argv[0]);
        /* a String initial value over numeric elements is a TypeError (String#+
           rejects an Integer), matching CRuby (#2504). */
        if ((rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) && init_t == TY_STRING) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (void)(");
          emit_expr(c, argv[0], b);
          buf_puts(b, "), sp_raise_cls(\"TypeError\", \"no implicit conversion of Integer into String\"), (mrb_int)0)");
          return 1;
        }
        /* a String initial value concatenates (["a","b"].sum("") == "ab") */
        if (rt == TY_STR_ARRAY && init_t == TY_STRING) {
          buf_puts(b, "sp_StrArray_sum_str("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        /* a float initial value promotes an integer-array sum to Float: add the
           float init to the integer total in floating point (sp_IntArray_sum
           returns mrb_int, so accumulating the init through it would truncate). */
        if (rt == TY_INT_ARRAY && init_t == TY_FLOAT) {
          buf_puts(b, "((mrb_float)("); emit_expr(c, argv[0], b);
          buf_puts(b, ") + (mrb_float)sp_IntArray_sum("); emit_expr(c, recv, b); buf_puts(b, ", 0))");
          return 1;
        }
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
        return 1;
      }
      if (sp_streq(name, "join") && argc <= 1) {
        buf_printf(b, "sp_%sArray_join(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sArray_inspect(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 0) {
        buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 1) {
        /* first(-1) is an ArgumentError in CRuby, not an empty slice */
        int tn0 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn0); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_%sArray_slice(", tn0, k);
        emit_expr(c, recv, b);
        buf_printf(b, ", 0, _t%d); })", tn0);
        return 1;
      }
      if (sp_streq(name, "last") && argc == 1) {
        /* slice's negative start counts from the end -> the last n elements;
           a negative count is an ArgumentError in CRuby */
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_%sArray_slice(", tn, k);
        emit_expr(c, recv, b);
        buf_printf(b, ", -_t%d, _t%d); })", tn, tn);
        return 1;
      }
      if (sp_streq(name, "pop") && argc == 0) {
        buf_printf(b, "sp_%sArray_pop(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) {
        buf_printf(b, "sp_%sArray_%s(", k, name); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "minmax") && argc == 0 && block < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); sp_%sArray_push(_t%d, sp_%sArray_min(_t%d));"
                      " sp_%sArray_push(_t%d, sp_%sArray_max(_t%d)); _t%d; })",
                   k, o, k, k, o, k, t, k, o, k, t, o);
        return 1;
      }
      if ((sp_streq(name, "index") || sp_streq(name, "find_index") || sp_streq(name, "rindex")) && argc == 1 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* nil-on-miss -> poly */
        const char *fn = sp_streq(name, "rindex") ? "rindex_poly" : "index_poly";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (sp_streq(k, "Int")) emit_int_expr(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "include?") && argc == 1) {
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
          return 1;
        }
      }
      if ((sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1 && rt == TY_FLOAT_ARRAY) {
        buf_puts(b, "sp_FloatArray_include("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_float_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "include?") || sp_streq(name, "member?") || sp_streq(name, "index") || sp_streq(name, "find_index")) && argc == 1 && rt != TY_FLOAT_ARRAY) {
        const char *fn = (sp_streq(name, "include?") || sp_streq(name, "member?")) ? "include" : "index";
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a poly argument into a string array's const char* slot (`arr.include?(
           params[k])`) needs coercing; emit_str_expr passes a plain string
           through and sp_poly_to_s's a poly value. */
        if (sp_streq(k, "Int")) emit_int_expr(c, argv[0], b);
        else if (rt == TY_STR_ARRAY) emit_str_expr(c, argv[0], b);
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 &&
          (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_STR_ARRAY)) {
        buf_printf(b, "sp_%sArray_sort(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "uniq") && argc == 0 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
        buf_printf(b, "sp_%sArray_uniq(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "last") && argc == 0) {
        /* Self-contained statement-expression: the receiver is bound to a temp
           (needed twice, for length and index) inside `({ ... })` rather than
           spilled to g_pre. A g_pre decl leaks into an expression context when
           `.last` is itself hoisted -- e.g. as the receiver of a following
           `.call` (`pipe.last.call(x)`), where it landed mid-`_t = ...`. */
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ %s _t%d = %s; sp_%sArray_get(_t%d, sp_%sArray_length(_t%d) - 1); })",
                   c_type_name(rt), t, rb.p ? rb.p : "", k, t, k, t);
        free(rb.p);
        return 1;
      }
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : ((sp_streq(name, "|") || sp_streq(name, "union")) ? "union" : "difference");
        /* empty literal [] arg: use a null pointer (safe for all sp_*Array_* set ops) */
        if (a0 == TY_UNKNOWN) { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", NULL)"); }
        else { buf_printf(b, "sp_%sArray_%s(", k, fn); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        return 1;
      }
      /* typed-array receiver, different-kind typed-array or poly-array argument:
         box both operands to poly and run the poly set op (result poly). */
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
          (a0 == TY_INT_ARRAY || a0 == TY_STR_ARRAY || a0 == TY_FLOAT_ARRAY || a0 == TY_POLY_ARRAY) && a0 != rt) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                             rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
        const char *conv_r = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                             a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" :
                             a0 == TY_FLOAT_ARRAY ? "sp_FloatArray_to_poly" : NULL;
        buf_printf(b, "sp_PolyArray_%s(%s(", fn, conv_l); emit_expr(c, recv, b); buf_puts(b, "), ");
        if (conv_r) { buf_printf(b, "%s(", conv_r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);  /* already poly */
        buf_puts(b, ")"); return 1;
      }
      /* variadic named set ops: union/intersection/difference(*others) fold the
         binary operator over each argument, accumulating in a rooted temp. */
      if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
           sp_streq(name, "difference")) && argc >= 2) {
        int ok = 1;
        for (int j = 0; j < argc; j++) {
          TyKind atj = comp_ntype(c, argv[j]);
          if (atj != rt && atj != TY_UNKNOWN) { ok = 0; break; }
        }
        if (ok) {
          const char *fn = sp_streq(name, "intersection") ? "intersect" :
                           sp_streq(name, "union") ? "union" : "difference";
          int t = ++g_tmp;
          buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " _t%d = sp_%sArray_%s(_t%d, ", t, k, fn, t);
            if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
            else emit_expr(c, argv[j], b);
            buf_puts(b, ");");
          }
          buf_printf(b, " _t%d; })", t);
          return 1;
        }
      }
      if (sp_streq(name, "intersect?") && argc == 1 && (a0 == rt || a0 == TY_UNKNOWN)) {
        buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_printf(b, "sp_%sArray_union(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 0) {
        buf_printf(b, "sp_%sArray_sample(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "rotate") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_dup(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%sArray_rotate_bang(_t%d, ", t, k, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "slice") || sp_streq(name, "[]")) && argc == 2) {
        /* a negative length or a start outside [-len, len] is nil in CRuby
           (start == len is the empty slice) */
        int ta2 = ++g_tmp, ts2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta2); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", ts2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; mrb_int _t%d = sp_%sArray_length(_t%d)", tn2, k, ta2);
        buf_printf(b, "; (_t%d < 0 || _t%d > _t%d || _t%d < -_t%d) ? (sp_%sArray *)0 : sp_%sArray_slice(_t%d, _t%d, _t%d); })",
                   tl2, ts2, tn2, ts2, tn2, k, k, ta2, ts2, tl2);
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_shuffle(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); mrb_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                      " sp_%sArray_slice(_t%d, 0, _t%d); })", tn, k, t, tn);
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && block < 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_sort(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        if (sp_streq(name, "max")) buf_printf(b, " sp_%sArray_reverse_bang(_t%d);", k, t);
        buf_printf(b, " sp_%sArray_slice(_t%d, 0, ", k, t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
    }
    /* poly (mixed-element) array methods: elements are boxed sp_RbVal */
    if (rt == TY_POLY_ARRAY) {
      if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_puts(b, "sp_PolyArray_slice_range("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_int_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_POLY) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; if (_t%d && _t%d->frozen) sp_raise_cls(\"FrozenError\", sp_sprintf(\"can't modify frozen Array: %%s\", sp_PolyArray_inspect(_t%d))); if (_t%d) _t%d->len = 0; _t%d; })", t, t, t, t, t, t);
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_concat("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* poly_array + typed array: box the typed operand to poly, then concat. */
      if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != TY_POLY_ARRAY) {
        const char *conv = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                           a0 == TY_FLOAT_ARRAY ? "sp_FloatArray_to_poly" :
                           a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : NULL;
        if (conv) {
          buf_puts(b, "sp_PolyArray_concat("); emit_expr(c, recv, b);
          buf_printf(b, ", %s(", conv); emit_expr(c, argv[0], b); buf_puts(b, "))");
          return 1;
        }
      }
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")"); return 1;
      }
      /* poly-array set-op with a typed-array argument (different element type):
         box the argument to a poly array, then run the poly op. */
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
          (a0 == TY_INT_ARRAY || a0 == TY_STR_ARRAY || a0 == TY_FLOAT_ARRAY)) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        const char *conv = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                           a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_printf(b, ", %s(", conv); emit_expr(c, argv[0], b);
        buf_puts(b, "))"); return 1;
      }
      /* variadic named set ops on a poly array: fold over each argument */
      if ((sp_streq(name, "intersection") || sp_streq(name, "union") ||
           sp_streq(name, "difference")) && argc >= 2) {
        int ok = 1;
        for (int j = 0; j < argc; j++) {
          TyKind atj = comp_ntype(c, argv[j]);
          if (atj != TY_POLY_ARRAY && atj != TY_UNKNOWN) { ok = 0; break; }
        }
        if (ok) {
          const char *fn = sp_streq(name, "intersection") ? "intersect" :
                           sp_streq(name, "union") ? "union" : "difference";
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " _t%d = sp_PolyArray_%s(_t%d, ", t, fn, t);
            if (comp_ntype(c, argv[j]) == TY_UNKNOWN) buf_puts(b, "NULL");
            else emit_expr(c, argv[j], b);
            buf_puts(b, ");");
          }
          buf_printf(b, " _t%d; })", t);
          return 1;
        }
      }
      if (sp_streq(name, "intersect?") && argc == 1 && (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN)) {
        buf_puts(b, "sp_PolyArray_intersect_p("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL"); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_puts(b, "sp_PolyArray_union("); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 0) {
        buf_puts(b, "sp_PolyArray_sample("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "rotate") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_dup(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray_rotate_bang(_t%d, ", t, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "slice") || sp_streq(name, "[]")) && argc == 2) {
        /* a negative length is nil in CRuby (slice() would return []), and
           so is a start outside [-len, len] (start == len: empty slice) */
        int ta = ++g_tmp, ts = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = ", ts); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = ", tl); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; mrb_int _t%d = sp_PolyArray_length(_t%d)", tn, ta);
        buf_printf(b, "; (_t%d < 0 || _t%d > _t%d || _t%d < -_t%d) ? (sp_PolyArray *)0 : sp_PolyArray_slice(_t%d, _t%d, _t%d); })",
                   tl, ts, tn, ts, tn, ta, ts, tl);
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_shuffle(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); mrb_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                      " sp_PolyArray_slice(_t%d, 0, _t%d); })", tn, t, tn);
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_sort(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        if (sp_streq(name, "max")) buf_printf(b, " sp_PolyArray_reverse_bang(_t%d);", t);
        buf_printf(b, " sp_PolyArray_slice(_t%d, 0, ", t); emit_int_expr(c, argv[0], b);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* count truthy elements; a poly element may be nil/false */
        int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; mrb_int _t%d = 0; for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " if (sp_poly_truthy(sp_PolyArray_get(_t%d, _t%d))) _t%d++;",
                   tn, ti, ti, t, ti, t, ti, tn);
        const char *expr = sp_streq(name, "all?") ? "_t%d == sp_PolyArray_length(_t%d)"
                         : sp_streq(name, "any?") ? "_t%d > 0"
                         : sp_streq(name, "none?") ? "_t%d == 0" : "_t%d == 1";
        buf_puts(b, " (");
        if (sp_streq(name, "all?")) buf_printf(b, expr, tn, t);
        else buf_printf(b, expr, tn);
        buf_puts(b, "); })");
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") || sp_streq(name, "none?") ||
           sp_streq(name, "one?") || sp_streq(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* poly_array.all?(v)/one?(v)/any?(v)/none?(v)/count(v) -- also serves a
           nil pattern (`[nil, nil].all?(nil)`, sp_poly_eq handles NIL) (#2366) */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) _t%d++;", ta, ti, tv, tc);
        if (sp_streq(name, "all?"))        buf_printf(b, " _t%d == sp_PolyArray_length(_t%d); })", tc, ta);
        else if (sp_streq(name, "any?"))   buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?"))  buf_printf(b, " _t%d == 0; })", tc);
        else if (sp_streq(name, "one?"))   buf_printf(b, " _t%d == 1; })", tc);
        else                              buf_printf(b, " _t%d; })", tc);
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0
          && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "count") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        /* count { |x| cond } on PolyArray */
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body2 = nt_ref(nt, blk, "body");
        int bn2 = 0; const int *bb2 = body2 >= 0 ? nt_arr(nt, body2, "body", &bn2) : NULL;
        if (bn2 > 0) {
          int trecv = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
          Buf rb2 = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          char es_ct[64]; snprintf(es_ct, sizeof es_ct, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          if (!emit_iter_autosplat(c, blk, rt, es_ct, g_indent + 1) && bp) {
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti);
          }
          for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
          int saveI = g_indent; g_indent = g_indent + 1;
          /* The block value is a condition: route through emit_cond so a poly /
             nil / scalar predicate becomes a valid C truthiness test (e.g.
             `count(&:alive)` where the element method is poly-dispatched would
             otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
          Buf vb2; memset(&vb2, 0, sizeof vb2); emit_cond(c, bb2[bn2 - 1], &vb2);
          g_indent = saveI;
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (%s) _t%d++;\n", vb2.p ? vb2.p : "0", tcnt);
          free(vb2.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tcnt);
          return 1;
        }
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_puts(b, "(sp_PolyArray_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc == 1) {
        buf_puts(b, "sp_PolyArray_push("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "unshift") && argc >= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d);", t);
        for (int a2 = argc - 1; a2 >= 0; a2--) {
          buf_printf(b, " sp_PolyArray_insert(_t%d, 0, ", t); emit_boxed(c, argv[a2], b); buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "insert") && argc >= 2) {
        int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); mrb_int _t%d = ", t, ti2); emit_int_expr(c, argv[0], b);
        buf_puts(b, ";");
        /* normalize ONCE (per-element normalization would drift as the array
           grows), keeping the too-negative IndexError the helper would raise */
        buf_printf(b, " mrb_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
                      " if (_t%d < 0) sp_raise_cls(\"IndexError\","
                      " sp_sprintf(\"index %%lld too small for array; minimum: %%lld\","
                      " (long long)_t%d, (long long)(-((_t%d ? _t%d->len : 0) + 1)))); }",
                   to2, ti2, ti2, ti2, t, t, ti2, to2, t, t);
        for (int a2 = 1; a2 < argc; a2++) {
          buf_printf(b, " sp_PolyArray_insert(_t%d, _t%d + %d, ", t, ti2, a2 - 1);
          emit_boxed(c, argv[a2], b); buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "concat") && argc == 1) {
        buf_puts(b, "sp_PolyArray_concat_into("); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "first") && argc == 0) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
        int blk = nt_ref(nt, id, "block");
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " mrb_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " mrb_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
        buf_printf(b, " mrb_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
        buf_printf(b, " (_t%d >= 0 && _t%d < _t%d) ? sp_PolyArray_get(_t%d, _t%d) :", tnorm, tnorm, tn, ta, tnorm);
        if (argc == 2) {
          buf_puts(b, " "); emit_boxed(c, argv[1], b); buf_puts(b, "; })");
        }
        else if (blk >= 0) {
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          buf_puts(b, " ({ ");
          const char *fp0 = block_param_name(c, blk, 0);
          if (fp0) buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), ti);
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], b, 0);
          if (bval >= 0) {
            if (comp_ntype(c, bval) != TY_POLY) emit_boxed(c, bval, b);
            else emit_expr(c, bval, b);
          }
          else buf_puts(b, "sp_box_nil()");
          buf_puts(b, "; }); })");
        }
        else {
          buf_printf(b, " (sp_raise_cls(\"IndexError\", \"index out of bounds\"), sp_box_nil()); })");
        }
        return 1;
      }
      if (sp_streq(name, "zip") && argc >= 1 && nt_ref(nt, id, "block") < 0) {
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tpair = ++g_tmp;
        int tb[16]; TyKind at[16]; int nargs = argc < 16 ? argc : 16;
        for (int j = 0; j < nargs; j++) { tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]); }
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        for (int j = 0; j < nargs; j++) {
          /* a Range argument materializes to its int array */
          if (at[j] == TY_RANGE) {
            int trj = ++g_tmp;
            buf_printf(b, " sp_IntArray *_t%d = ({ sp_Range _t%d = ", tb[j], trj);
            emit_expr(c, argv[j], b);
            buf_printf(b, "; sp_range_to_ia(_t%d); });", trj);
            at[j] = TY_INT_ARRAY;
            continue;
          }
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
        return 1;
      }
      if (sp_streq(name, "last") && argc == 0) {
        /* self-contained stmt-expr, not a g_pre decl: `.last` is hoistable as a
           receiver (e.g. `arr.last.call(x)`) where a g_pre decl would leak into
           the surrounding expression (#2942). */
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s; sp_PolyArray_get(_t%d, sp_PolyArray_length(_t%d) - 1); })",
                   t, rb.p ? rb.p : "", t, t);
        free(rb.p);
        return 1;
      }
      if ((sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1) {
        /* member? is a pure alias of include? for arrays. An empty [] literal
           receiver contains nothing; folding avoids the kind mismatch when the
           literal narrowed to a typed array elsewhere */
        int iel = 0;
        if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
          int ien = 0; nt_arr(nt, recv, "elements", &ien);
          iel = ien == 0;
        }
        if (iel) {
          buf_puts(b, "((void)("); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)");
          return 1;
        }
        buf_puts(b, "sp_PolyArray_include("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "clone") && argc == 0) {
        /* clone carries the frozen flag over (dup does not) */
        int ts = ++g_tmp, td = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", ts); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_dup(_t%d); "
                      "_t%d->frozen = _t%d ? _t%d->frozen : 0; _t%d; })",
                   td, ts, td, ts, ts, td);
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "compact") && argc == 0) {
        buf_puts(b, "sp_PolyArray_compact("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "compact!") && argc == 0) {
        /* value form: self when changed, nil when a no-op (CRuby) */
        buf_puts(b, "sp_PolyArray_compact_bangq("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "flatten") && argc <= 1) {
        if (argc == 1) { buf_puts(b, "sp_PolyArray_flatten_n("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else { buf_puts(b, "sp_PolyArray_flatten("); emit_expr(c, recv, b); buf_puts(b, ")"); }
        return 1;
      }
      if (sp_streq(name, "flatten!") && argc == 0) {
        /* value form: self when changed, nil when a no-op (CRuby) */
        buf_puts(b, "sp_PolyArray_flatten_bangq("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "flatten!") && argc == 1) {
        buf_puts(b, "sp_PolyArray_flatten_bangq_depth("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "flatten") && argc == 1) {
        buf_puts(b, "sp_PolyArray_flatten_depth("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "product") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        /* poly product with one list: all [x, y] pairs (an empty receiver or
           argument yields []) */
        int pta = ++g_tmp, ptb = ++g_tmp, ptr = ++g_tmp, pti = ++g_tmp, ptj = ++g_tmp, pte = ++g_tmp;
        Buf pra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", pta, pra.p ? pra.p : "NULL", pta);
        free(pra.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_enum_items_from(", ptb);
        emit_boxed(c, argv[0], b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", ptb);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ptr, ptr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", pti, pti, pta, pti);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ptj, ptj, ptb, ptj);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }",
                   pte, pte, pte, pta, pti, pte, ptb, ptj, ptr, pte);
        buf_printf(b, " _t%d; })", ptr);
        return 1;
      }
      if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* product with no arguments: each element wrapped in its own array */
        int ta = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, te = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);", ta, ra.p ? ra.p : "NULL", ta);
        free(ra.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }",
                   te, te, te, ta, ti, tr, te);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "transpose") && argc == 0) {
        buf_puts(b, "sp_int_array_transpose("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) {
        buf_printf(b, "sp_PolyArray_%s(", name); emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "join") && argc <= 1) {
        buf_puts(b, "sp_PolyArray_join("); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* the separator must be a const char*; a poly separator (e.g. a reader
           whose ivar widened to poly) is converted with sp_poly_to_s. */
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_POLY) {
          buf_puts(b, "sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (argc == 1) emit_expr(c, argv[0], b);
        else buf_puts(b, "\"\"");
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_puts(b, "sp_PolyArray_inspect("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 2) {
        buf_puts(b, "sp_PolyArray_slice_bang("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "repeated_combination") || sp_streq(name, "combination") ||
           sp_streq(name, "permutation")) &&
          (argc == 1 || (sp_streq(name, "permutation") && argc == 0)) &&
          nt_ref(nt, id, "block") < 0) {
        const char *combfn = sp_streq(name, "combination") ? "sp_PolyArray_combination"
                           : sp_streq(name, "permutation") ? "sp_PolyArray_permutation"
                           : "sp_PolyArray_repeated_combination";
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); %s(_t%d, ", ta, combfn, ta);
        if (argc == 1) emit_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
        buf_puts(b, "); })");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = _t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first;",
                   tf, tr, tr, ta, ta, tr);
        buf_printf(b, " mrb_int _t%d = (_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                   tn, tr, tr, ta, ta, tr, tf, tr);
        buf_printf(b, " sp_PolyArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", ta, tf, tn, tn);
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1) {
        buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "shuffle") && argc == 0) {
        buf_puts(b, "sp_PolyArray_shuffle("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_puts(b, "sp_PolyArray_sort("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* minmax (no block): [min, max] via the poly comparator (user `<=>`
         through the cmp hook); incomparable raises the Comparable
         ArgumentError; empty -> [nil, nil]. Both temps rooted: min/max can
         allocate inside sp_poly_cmp (bigint temps) and push reallocs. */
      if (sp_streq(name, "minmax") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_min(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_PolyArray_max(_t%d)); _t%d; })",
                   t, o, o, o, t, o, t, o);
        return 1;
      }
      {
        const char *base = NULL;
        if      (sp_streq(name, "reverse!")) base = "reverse_bang";
        else if (sp_streq(name, "shuffle!")) base = "shuffle_bang";
        else if (sp_streq(name, "sort!"))    base = "sort_bang";
        if (base && argc == 0) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; sp_PolyArray_%s(_t%d); _t%d; })", base, t, t);
          return 1;
        }
      }
      if (sp_streq(name, "uniq!") && argc == 0) {
        /* value form: self when changed, nil when a no-op (CRuby) */
        buf_puts(b, "sp_PolyArray_uniq_bangq("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "product") && argc == 1 && a0 == TY_POLY_ARRAY) {
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
        return 1;
      }
      if (sp_streq(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_PolyArray_rotate_bang(_t%d, ", t);
        if (argc == 1) emit_expr(c, argv[0], b); else buf_puts(b, "1");
        buf_printf(b, "); _t%d; })", t);
        return 1;
      }
      if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf vb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && nt_ref(nt, id, "block") >= 0) {
        int is_rej = sp_streq(name, "reject!") || sp_streq(name, "delete_if");
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, ti = ++g_tmp, twp = ++g_tmp, torig = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s;\n", trecv, rb.p ? rb.p : ""); free(rb.p);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = sp_PolyArray_length(_t%d);\n", torig, trecv);
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "mrb_int _t%d = 0;\n", twp);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal _telt%d = sp_PolyArray_get(_t%d, _t%d);\n", ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = _telt%d;\n", bp, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* Capture the predicate in its own buffer: a multi-statement terminal
             (a block ending in an if/else expression) lowers its statements
             through g_pre, and emitting the condition straight into g_pre would
             splice them after the already-written `if (!`. */
          Buf cb; memset(&cb, 0, sizeof cb);
          emit_cond(c, bb[bn - 1], &cb);
          emit_indent(g_pre, g_indent);
          buf_puts(g_pre, "if (");
          if (is_rej) buf_puts(g_pre, "!");
          buf_puts(g_pre, cb.p ? cb.p : "0");
          free(cb.p);
          g_indent = sv;
          buf_printf(g_pre, ") { sp_PolyArray_set(_t%d, _t%d, _telt%d); _t%d++; }\n",
                     trecv, twp, ti, twp);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "if (_t%d) _t%d->len = _t%d;\n", trecv, trecv, twp);
          char box[64]; snprintf(box, sizeof box, "sp_box_poly_array(_t%d)", trecv);
          emit_filter_bang_result(name, trecv, torig, twp, box, b);
          return 1;
        }
      }
      if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        TyKind res = comp_ntype(c, id);
        const char *hn = ty_hash_cname(res);
        if (!hn) hn = "SymPoly";
        TyKind kty = ty_hash_key(res), vty = ty_hash_val(res);
        int tr = ++g_tmp, th = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", tr, hn, th, hn, th);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
        /* Each pair is a boxed array whose own kind varies (IntArray for [1,2],
           StrArray for ["a","b"], PolyArray for mixed); sp_poly_arr_get boxes an
           element from any of them, so key/value extraction works regardless. */
        buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tp, tr, ti);
        buf_printf(b, " sp_%sHash_set(_t%d, ", hn, th);
        char kexpr[128];
        if (kty == TY_SYMBOL)      snprintf(kexpr, sizeof kexpr, "(sp_sym)sp_poly_arr_get(_t%d, 0).v.i", tp);
        else if (kty == TY_STRING) snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.s", tp);
        else if (kty == TY_POLY)   snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0)", tp);
        else                       snprintf(kexpr, sizeof kexpr, "sp_poly_arr_get(_t%d, 0).v.i", tp);
        buf_puts(b, kexpr); buf_puts(b, ", ");
        /* value extraction */
        if (vty == TY_POLY)        buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
        else if (vty == TY_INT)    buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.i", tp);
        else if (vty == TY_STRING) buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.s", tp);
        else if (vty == TY_FLOAT)  buf_printf(b, "sp_poly_arr_get(_t%d, 1).v.f", tp);
        else                       buf_printf(b, "sp_poly_arr_get(_t%d, 1)", tp);
        buf_printf(b, "); } _t%d; })", th);
        return 1;
      }
      /* to_h { |x| [k, v] } on a poly array -> a hash keyed by the block's
         literal [k, v] tail pair (the only shape analyze_infer types):
         string/symbol keys get their own hash kind, anything else a fully
         boxed hash (doom: flats.to_h { |f| [f.name, f] }). */
      if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
        int blk = nt_ref(nt, id, "block");
        const char *bp = block_param_name(c, blk, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        int tail = bn > 0 ? bb[bn - 1] : -1;
        const char *tty = tail >= 0 ? nt_type(nt, tail) : NULL;
        int pairn = 0;
        const int *pair = (tty && sp_streq(tty, "ArrayNode")) ? nt_arr(nt, tail, "elements", &pairn) : NULL;
        const char *hn = pair && pairn == 2 ? ty_hash_cname(res) : NULL;
        if (hn) {
          TyKind kt = comp_ntype(c, pair[0]);
          int trecv = ++g_tmp, ti = ++g_tmp, th = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s; SP_GC_ROOT(_t%d);\n", trecv, rb.p ? rb.p : "NULL", trecv); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);\n", hn, th, hn, th);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          Buf kb; memset(&kb, 0, sizeof kb);
          if (kt == TY_STRING) {
            /* A TY_STRING slot can carry nil (NULL); a Str-keyed hash can't
               store it (NULL marks an empty bucket and sp_str_hash reads
               k[-1]), so raise instead of segfaulting on a nil key. */
            int tk = ++g_tmp;
            buf_printf(&kb, "({ const char *_t%d = sp_str_dup(", tk);
            emit_expr(c, pair[0], &kb);
            buf_printf(&kb, "); if (!_t%d) sp_raise_cls(\"TypeError\", \"nil key in a string-keyed Hash\"); _t%d; })", tk, tk);
          }
          else if (kt == TY_SYMBOL) emit_expr(c, pair[0], &kb);
          else emit_boxed(c, pair[0], &kb);
          Buf vb; memset(&vb, 0, sizeof vb); emit_boxed(c, pair[1], &vb);
          g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_%sHash_set(_t%d, %s, %s);\n", hn, th, kb.p ? kb.p : "", vb.p ? vb.p : "");
          free(kb.p); free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", th);
          return 1;
        }
      }
    }
  }
  return 0;
}

/* Emit a statement-expression materializing a hash's entries as a PolyArray of
   [key, value] poly pairs in insertion order. The source hash is GC-rooted
   because each pair allocates inside the walk. Shared by Hash#to_a/#entries and
   Hash#sort. */
void emit_hash_pairs_expr(Compiler *c, int recv, TyKind rt, const char *hn, Buf *b) {
  int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
  TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
  buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d);", th);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
  buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
  buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
  if (kt == TY_SYMBOL)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d->order[_t%d]));", tp, th, ti);
  else if (kt == TY_STRING)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d->order[_t%d]));", tp, th, ti);
  else if (kt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d->order[_t%d]));", tp, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->keys[_t%d->order[_t%d]]);", tp, th, th, ti);
  if (rt == TY_POLY_POLY_HASH)
    buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[_t%d]]);", tp, th, th, ti);
  else if (vt == TY_POLY)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d->order[_t%d]));", tp, hn, th, th, ti);
  else if (vt == TY_INT)
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tp, hn, th, th, ti);
  else
    buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d->order[_t%d])));", tp, hn, th, th, ti);
  buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d));", tr, tp);
  buf_printf(b, " } _t%d; })", tr);
}

int emit_hash_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  if (recv >= 0 && ty_is_hash(rt)) {
    /* compare_by_identity? is always false for a value-keyed hash; the mutating
       compare_by_identity cannot be honored (keys are compared by value) and is
       rejected loudly rather than silently no-op'd. */
    if (sp_streq(name, "compare_by_identity?") && argc == 0) { buf_puts(b, "0"); return 1; }
    /* compact!: drop nil-valued pairs in place; self when changed, nil
       when a no-op (only the poly-valued variants can hold nil) */
    if (sp_streq(name, "compact!") && argc == 0 &&
        (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH)) {
      const char *hnc = ty_hash_cname(rt);
      /* PolyPoly's order[] holds slot indexes, not keys; the other variants
         store the key itself in order[] (#2430) */
      int ppk = rt == TY_POLY_POLY_HASH;
      int th = ++g_tmp, tf = ++g_tmp, ti = ++g_tmp, tv = ++g_tmp, tc2 = ++g_tmp;
      buf_printf(b, "({ sp_%sHash *_t%d = ", hnc, th); emit_expr(c, recv, b);
      buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);"
                    " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {",
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
    /* any?(pattern) / none? / one? / count with one arg: compare each
       [key, value] pair by == (sp_poly_eq covers array-vs-array value
       equality, which is what a pair pattern is) */
    if (argc == 1 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "any?") || sp_streq(name, "none?") ||
         sp_streq(name, "one?") || sp_streq(name, "count"))) {
      int th = ++g_tmp, tv = ++g_tmp, tn = ++g_tmp, tc2 = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", th);
      emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", th, tv);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); mrb_int _t%d = sp_poly_length(_t%d); mrb_int _t%d = 0;",
                 tv, tn, th, tc2);
      buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                    " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _t%d);"
                    " if (sp_poly_eq(_t%d, _t%d)) _t%d++; }",
                 ti, ti, tn, ti, tp, th, ti, tp, tv, tc2);
      if (sp_streq(name, "any?"))       buf_printf(b, " _t%d > 0; })", tc2);
      else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc2);
      else if (sp_streq(name, "one?"))  buf_printf(b, " _t%d == 1; })", tc2);
      else                              buf_printf(b, " _t%d; })", tc2);
      return 1;
    }
    /* blockless Enumerable predicates fold on the pair count (a pair is
       always truthy, so all? is unconditionally true) */
    if (argc == 0 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "any?") || sp_streq(name, "none?") || sp_streq(name, "all?"))) {
      const char *hn0 = ty_hash_cname(rt);
      if (hn0) {
        int th0 = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn0, th0); emit_expr(c, recv, b);
        if (sp_streq(name, "any?")) buf_printf(b, "; (_t%d && _t%d->len > 0); })", th0, th0);
        else if (sp_streq(name, "none?")) buf_printf(b, "; (!_t%d || _t%d->len == 0); })", th0, th0);
        else buf_printf(b, "; (void)_t%d; 1; })", th0);
        return 1;
      }
    }
    /* Hash#default_proc: wrap the stored Hash.new{} dproc (a raw C fn +
       captures pointer) in a first-class Proc via a per-variant trampoline
       that adapts the sp_proc_call ABI (boxed side-channel args) back to the
       dproc signature. A hash without a dproc -- or a variant that cannot
       carry one -- yields NULL (nil). */
    if (sp_streq(name, "default_proc") && argc == 0 && nt_ref(nt, id, "block") < 0) {
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
          buf_puts(&g_proc_protos, "extern SP_TLS sp_RbVal _sp_proc_poly_args[16];\n");
        }
        const char *kexpr = hdp_v == 0 ? "(sp_sym)sp_poly_to_i(_sp_proc_poly_args[1])"
                          : hdp_v == 1 ? "_sp_proc_poly_args[1].v.s"
                          : "_sp_proc_poly_args[1]";
        buf_printf(&g_procs,
          "static mrb_int _hdp_tramp_%s(void *cap, mrb_int argc, mrb_int *args) {\n"
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
    /* deconstruct_keys(keys or nil): CRuby returns the hash itself */
    if (sp_streq(name, "deconstruct_keys") && argc == 1) {
      buf_puts(b, "((void)(");
      emit_boxed(c, argv[0], b);
      buf_puts(b, "), ");
      emit_expr(c, recv, b);
      buf_puts(b, ")");
      return 1;
    }
    /* Hash#equal? -- object identity is pointer identity */
    if (sp_streq(name, "equal?") && argc == 1) {
      TyKind at0 = comp_ntype(c, argv[0]);
      if (ty_is_hash(at0) || ty_is_array(at0)) {
        Buf rb = expr_buf(c, recv), ab = expr_buf(c, argv[0]);
        buf_printf(b, "((void *)(%s) == (void *)(%s))",
                   rb.p ? rb.p : "0", ab.p ? ab.p : "0");
        free(rb.p); free(ab.p);
      }
      else {
        buf_puts(b, "0");
      }
      return 1;
    }
    if (sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
      unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
    const char *hn = ty_hash_cname(rt);
    if (hn) {
      /* select! / filter! / reject! / keep_if / delete_if { |k, v| cond } in
         expression position (the statement form lives in emit_iteration_stmt).
         Mutates in place; `!` forms yield nil when nothing was removed else self,
         keep_if/delete_if always yield self. */
      if ((sp_streq(name, "delete_if") || sp_streq(name, "reject!") || sp_streq(name, "select!") ||
           sp_streq(name, "filter!") || sp_streq(name, "keep_if")) &&
          nt_ref(nt, id, "block") >= 0 && rt != TY_POLY_POLY_HASH) {
        int block = nt_ref(nt, id, "block");
        int is_rej = sp_streq(name, "delete_if") || sp_streq(name, "reject!");
        const char *p0_raw = block_param_name(c, block, 0);
        const char *p1_raw = block_param_name(c, block, 1);
        const char *kp = p0_raw ? rename_local(p0_raw) : NULL;
        const char *vp = p1_raw ? rename_local(p1_raw) : NULL;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          Scope *hs = comp_scope_of(c, block);
          TyKind hkt = ty_hash_key(rt), hvt = ty_hash_val(rt);
          LocalVar *klv = (kp && hs) ? scope_local(hs, p0_raw) : NULL;
          LocalVar *vlv = (vp && hs) ? scope_local(hs, p1_raw) : NULL;
          TyKind ksaved = klv ? klv->type : TY_UNKNOWN;
          TyKind vsaved = vlv ? vlv->type : TY_UNKNOWN;
          if (klv) klv->type = hkt;
          if (vlv) vlv->type = hvt;
          for (int j = 0; j < bn; j++) infer_subtree(c, bb[j]);
          int tr = ++g_tmp, ti = ++g_tmp, torig = ++g_tmp, twp = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", tr, rb.p ? rb.p : "NULL"); free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash();\n", tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d ? _t%d->len : 0;\n", torig, tr, tr);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d && _t%d < _t%d->len; ) {\n", ti, tr, ti, tr);
          if (kp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, hkt, g_pre);
            buf_printf(g_pre, " lv_%s = _t%d->order[_t%d];\n", kp, tr, ti);
          }
          if (vp) {
            emit_indent(g_pre, g_indent + 1); emit_ctype(c, hvt, g_pre);
            buf_printf(g_pre, " lv_%s = sp_%sHash_get(_t%d, _t%d->order[_t%d]);\n", vp, hn, tr, tr, ti);
          }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          Buf *sp_save = g_pre; int gi_save = g_indent;
          Buf cpre; memset(&cpre, 0, sizeof cpre); g_pre = &cpre; g_indent = gi_save + 1;
          Buf cexpr = expr_buf(c, bb[bn - 1]);
          g_pre = sp_save; g_indent = gi_save;
          if (cpre.p) { buf_puts(g_pre, cpre.p); free(cpre.p); }
          emit_indent(g_pre, g_indent + 1);
          if (is_rej) buf_printf(g_pre, "if (%s) {\n", cexpr.p ? cexpr.p : "0");
          else        buf_printf(g_pre, "if (!(%s)) {\n", cexpr.p ? cexpr.p : "0");
          free(cexpr.p);
          emit_indent(g_pre, g_indent + 2);
          buf_printf(g_pre, "sp_%sHash_delete(_t%d, _t%d->order[_t%d]);\n", hn, tr, tr, ti);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "} else {\n");
          emit_indent(g_pre, g_indent + 2); buf_printf(g_pre, "_t%d++;\n", ti);
          emit_indent(g_pre, g_indent + 1); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = _t%d ? _t%d->len : 0;\n", twp, tr, tr);
          if (klv) klv->type = ksaved;
          if (vlv) vlv->type = vsaved;
          char box[96]; snprintf(box, sizeof box, "sp_box_obj(_t%d, %s)", tr, hash_box_cls(rt));
          emit_filter_bang_result(name, tr, torig, twp, box, b);
          return 1;
        }
      }
      /* Hash#to_proc: a Proc mapping a key to the hash value, closing over the
         hash. Emit a per-variant lookup fn matching the sp_proc_call ABI. */
      if (sp_streq(name, "to_proc") && argc == 0) {
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int pn = ++g_proc_counter;
        /* a PolyPolyHash key is an sp_RbVal, delivered on the proc's poly
           side-channel (args[] carries only scalar bits); the get() takes it
           directly. Scalar-keyed variants read the mrb_int slot. */
        const char *keyexpr = (kt == TY_SYMBOL) ? "(sp_sym)args[0]"
                            : (kt == TY_STRING) ? "(const char *)(uintptr_t)args[0]"
                            : (rt == TY_POLY_POLY_HASH) ? "_sp_proc_poly_args[0]"
                            : "args[0]";
        if (rt == TY_POLY_POLY_HASH) g_needs_proc_poly_argslot = 1;
        buf_printf(&g_proc_protos, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args);\n", pn);
        buf_printf(&g_procs, "static mrb_int _hashproc_%d(void *cap, mrb_int argc, mrb_int *args) {\n", pn);
        buf_printf(&g_procs, "  if (argc < 1) return 0;\n");
        buf_printf(&g_procs, "  sp_%sHash *_h = (sp_%sHash *)cap;\n", hn, hn);
        /* Universal return ABI: publish the boxed value into _sp_proc_poly_ret
           for every value type; the .call site reads the slot back. */
        buf_puts(&g_procs, "  _sp_proc_poly_ret = ");
        emit_box_open(c, vt, &g_procs);
        buf_printf(&g_procs, "sp_%sHash_get(_h, %s)", hn, keyexpr);
        emit_box_close(c, vt, &g_procs);
        buf_puts(&g_procs, ";\n  return 0;\n}\n");
        buf_printf(b, "sp_proc_new_meta((void *)_hashproc_%d, (void *)(", pn);
        emit_expr(c, recv, b);
        buf_puts(b, "), sp_hashproc_cap_scan, 1, FALSE, 1, NULL, NULL)");
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1) {
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
          return 1;
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
        return 1;
      }
      if (sp_streq(name, "dig") && argc >= 1) {
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, etc.) -> nil. */
        TyKind arg0t = comp_ntype(c, argv[0]);
        if ((kt == TY_SYMBOL && arg0t == TY_STRING) ||
            (kt == TY_STRING && arg0t == TY_SYMBOL)) {
          if (vt == TY_INT) buf_puts(b, "SP_INT_NIL");
          else if (vt == TY_STRING) buf_puts(b, "NULL");
          else buf_puts(b, "sp_box_nil()");
          return 1;
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
            /* Past the first key the receiver `_tr` is whatever the previous
               step returned (a nested hash, an Array element, ...), whose key
               type is not the top hash's key type. So `{a:[10,20]}.dig(:a,1)`
               must index the Array with `1`, not look up symbol `1`. Infer the
               sub-key type from the argument node itself; sp_poly_arr_get_hash
               then dispatches on the runtime receiver (array/hash/etc.). */
            TyKind dkt = comp_ntype(c, argv[di]);
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
            else if (dkt == TY_POLY) {
              /* A poly sub-key is stored as sp_RbVal, not mrb_int; dispatch on
                 both the runtime receiver and key kind. */
              buf_printf(b, " sp_RbVal _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_index_poly(_t%d, _t%d);", tr, tr, tk);
            }
            else {
              buf_printf(b, " mrb_int _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_arr_get_hash(_t%d, _t%d);", tr, tr, tk);
            }
          }
          buf_printf(b, " _t%d; })", tr);
        }
        return 1;
      }
      if (sp_streq(name, "values_at") && argc == 0) {
        /* zero keys: an empty array; evaluate the receiver for effects (#2408) */
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())");
        return 1;
      }
      if ((sp_streq(name, "values_at") || sp_streq(name, "fetch_values")) && argc >= 1) {
        /* collect looked-up values into a poly array; values_at yields nil for
           a missing key, fetch_values raises KeyError */
        int is_fetch = sp_streq(name, "fetch_values");
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
          int fv_blk = nt_ref(nt, id, "block");
          if (is_fetch && fv_blk >= 0 && nt_type(nt, fv_blk) &&
              sp_streq(nt_type(nt, fv_blk), "BlockNode")) {
            /* fetch_values(...) { |k| fallback }: the block supplies the
               value for each MISSING key instead of raising */
            const char *fp0 = block_param_name(c, fv_blk, 0);
            int fvb = nt_ref(nt, fv_blk, "body");
            int fvn = 0; const int *fvv = fvb >= 0 ? nt_arr(nt, fvb, "body", &fvn) : NULL;
            buf_puts(b, " else {");
            if (fp0) {
              char keytmp[32]; snprintf(keytmp, sizeof keytmp, "_t%d", tk);
              buf_printf(b, " lv_%s = ", rename_local(fp0));
              if (kt == TY_POLY) buf_puts(b, keytmp);
              else emit_boxed_text(c, kt, keytmp, b);
              buf_puts(b, ";");
            }
            if (fvn > 0) {
              buf_printf(b, " sp_PolyArray_push(_t%d, ", tr);
              emit_boxed(c, fvv[fvn - 1], b);
              buf_puts(b, ");");
            }
            buf_puts(b, " }");
          }
          else if (is_fetch) {
            char keytmp[32], htmp[32];
            snprintf(keytmp, sizeof keytmp, "_t%d", tk);
            snprintf(htmp, sizeof htmp, "_t%d", th);
            buf_puts(b, " else { sp_exc_stage_recv(");
            emit_boxed_text(c, rt, htmp, b);
            buf_puts(b, "); sp_raise_key_not_found(");
            emit_boxed_text(c, kt, keytmp, b);
            buf_puts(b, "); }");
          }
          else buf_printf(b, " else sp_PolyArray_push(_t%d, sp_box_nil());", tr);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "fetch") && argc == 1) {
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
          return 1;
        }
        /* fetch(key) with no default raises KeyError on a miss */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : (",
                   hn, th, tk, hn, th, tk);
        char keytmp[32], htmp[32];
        snprintf(keytmp, sizeof keytmp, "_t%d", tk);
        snprintf(htmp, sizeof htmp, "_t%d", th);
        buf_puts(b, "sp_exc_stage_recv(");
        emit_boxed_text(c, rt, htmp, b);
        buf_puts(b, "), sp_raise_key_not_found(");
        emit_boxed_text(c, ty_hash_key(rt), keytmp, b);
        buf_printf(b, "), %s); })", vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        return 1;
      }
      if (sp_streq(name, "fetch") && argc == 2) {
        /* fetch(key, default) -> has_key? ? value : default */
        TyKind vt = ty_hash_val(rt);
        TyKind dt = comp_ntype(c, argv[1]);
        /* Empty `{}` default infers TY_UNKNOWN but is a hash — incompatible with int/str etc. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(c->nt, argv[1]);
          if (atn && (sp_streq(atn, "HashNode") || sp_streq(atn, "KeywordHashNode")))
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
        return 1;
      }
      if ((sp_streq(name, "length") || sp_streq(name, "size") ||
           (sp_streq(name, "count") && nt_ref(nt, id, "block") < 0)) && argc == 0) {
        buf_printf(b, "sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "empty?") && argc == 0) {
        buf_printf(b, "(sp_%sHash_length(", hn); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc <= 1 && nt_ref(nt, id, "block") < 0) {
        /* Hash#sum without a block folds each [k,v] PAIR into the init value;
           `init + [k,v]` is Integer#+ Array -> TypeError, so only an empty hash
           (which returns the init unchanged) is well-defined. */
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_length(_t%d) == 0 ? (mrb_int)(", hn, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "0");
        buf_puts(b, ") : (sp_raise_cls(\"TypeError\", \"Array can't be coerced into Integer\"), (mrb_int)0); })");
        return 1;
      }
      if (sp_streq(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_clear(_t%d); _t%d; })", hn, t, t);
        return 1;
      }
      /* no-arg merge -> a copy; no-arg slice -> an empty hash of the same
         variant; to_hash / rehash -> self (#2340/#2349) */
      if ((sp_streq(name, "merge") || sp_streq(name, "to_hash") ||
           sp_streq(name, "rehash")) && argc == 0) {
        buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice") && argc == 0) {
        buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
        buf_printf(b, "); sp_%sHash_new(); })", hn);
        return 1;
      }
      /* blockless one? -> exactly one pair (#2354) */
      if (sp_streq(name, "one?") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_length(_t%d) == 1; })", hn, t);
        return 1;
      }
      if ((sp_streq(name, "has_key?") || sp_streq(name, "key?") ||
           sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1) {
        TyKind arg_kt = comp_ntype(c, argv[0]);
        TyKind hash_kt = ty_hash_key(rt);
        if (hash_kt != TY_POLY && arg_kt != TY_POLY && arg_kt != TY_UNKNOWN && arg_kt != hash_kt) {
          /* type mismatch (e.g. string arg on sym-keyed hash): always false */
          buf_puts(b, "0"); return 1;
        }
        buf_printf(b, "sp_%sHash_has_key(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], hash_kt, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "value?") || sp_streq(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH ||
                    rt == TY_POLY_POLY_HASH);  /* boxed-value variants (#2373) */
        buf_printf(b, "sp_%sHash_has_value(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (poly) emit_boxed(c, argv[0], b); else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* Hash#key(value): the first key mapping to value (sym-keyed hash). */
      if (sp_streq(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) {
        buf_puts(b, "sp_SymPolyHash_key(");
        emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      /* Hash#key(value) for any variant: the first key whose value == the arg,
         or nil. Scans the boxed [key, value] pair list. */
      if (sp_streq(name, "key") && argc == 1) {
        int tp = ++g_tmp, tv = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_RbVal _t%d = sp_box_nil();", tr);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, tp, ti);
        buf_printf(b, " sp_PolyArray *_pr = (sp_PolyArray *)_t%d->data[_t%d].v.p;", tp, ti);
        buf_printf(b, " if (sp_poly_eq(_pr->data[1], _t%d)) { _t%d = _pr->data[0]; break; } }", tv, tr);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && comp_ntype(c, argv[0]) == rt) {
        buf_printf(b, "sp_%sHash_replace(", hn);
        emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* replace with a DIFFERENT hash variant: the receiver slot has widened to
         the universal PolyPoly hash (see infer), so clear it and re-fill from
         the boxed other's [k, v] pairs -- never the raw-pointer mispatch that
         used to hang inspect (#2374). */
      if (sp_streq(name, "replace") && argc == 1 && rt == TY_POLY_POLY_HASH &&
          ty_is_hash(comp_ntype(c, argv[0]))) {
        int th = ++g_tmp, to = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", th); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", th, to); emit_boxed(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyPolyHash_clear(_t%d);", to, th);
        buf_printf(b, " mrb_int _t%d = sp_poly_length(_t%d);", tn, to);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                      " sp_RbVal _k, _v; sp_poly_hash_pair(_t%d, _t%d, &_k, &_v);"
                      " sp_PolyPolyHash_set(_t%d, _k, _v); } _t%d; })",
                   ti, ti, tn, ti, to, ti, th, th);
        return 1;
      }
      if (sp_streq(name, "default") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        if (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH) {
          /* default(key): a hash built with a block calls its default_proc with
             (self, key); default() (or a hash with no proc) returns default_v
             (#2464). Only the poly-value variants carry a dproc. */
          if (argc == 1) {
            buf_printf(b, "; (_t%d && _t%d->dproc) ? _t%d->dproc(_t%d, ", t, t, t, t);
            if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[0], b);
            else emit_expr(c, argv[0], b);
            buf_printf(b, ", _t%d->dproc_self) : (_t%d ? _t%d->default_v : sp_box_nil()); })", t, t, t);
          } else {
            buf_printf(b, "; _t%d ? _t%d->default_v : sp_box_nil(); })", t, t);
          }
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
        return 1;
      }
      if (sp_streq(name, "default=") && argc == 1) {
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
        emit_expr(c, argv[0], b); buf_puts(b, "; })"); return 1;
      }
      if (sp_streq(name, "keys") && argc == 0 && rt == TY_SYM_POLY_HASH) {
        /* runtime returns sym ids as an IntArray; box into a poly (sym) array */
        int ki = ++g_tmp, kp = ++g_tmp, ii = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_SymPolyHash_keys(", ki); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ki, kp, kp);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                      " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)sp_IntArray_get(_t%d, _t%d)));",
                   ii, ii, ki, ii, kp, ki, ii);
        buf_printf(b, " _t%d; })", kp);
        return 1;
      }
      if (sp_streq(name, "keys") && argc == 0) {
        buf_printf(b, "sp_%sHash_keys(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "values") && argc == 0) {
        buf_printf(b, "sp_%sHash_values(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
        buf_printf(b, "sp_%sHash_inspect(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* PolyPoly receiver: any hash-variant argument folds in through the
         boxed [k, v] pair walk (a heterogeneous hash routinely absorbs a
         typed one). Blockless. */
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc >= 1 &&
          nt_ref(nt, id, "block") < 0 && rt == TY_POLY_POLY_HASH) {
        for (int ai = 0; ai < argc; ai++)
          if (!ty_is_hash(comp_ntype(c, argv[ai]))) return 0;
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
        for (int ai = 0; ai < argc; ai++) {
          int to = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = ", to); emit_boxed(c, argv[ai], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);"
                        " mrb_int _t%d = sp_poly_arr_len_ex(_t%d);"
                        " for (mrb_int _i9 = 0; _i9 < _t%d; _i9++) {"
                        " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _i9);"
                        " sp_PolyPolyHash_set(_t%d,"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 0),"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 1)); }",
                     to, ti, to, ti, tp, to, tr, tp, tp);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* merge!/update with several hash arguments: fold each one in, in
         order (#2431). Blockless, same-variant arguments only. */
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc >= 2 &&
          nt_ref(nt, id, "block") < 0 && rt != TY_POLY_POLY_HASH) {
        TyKind kt = ty_hash_key(rt);
        for (int ai = 0; ai < argc; ai++)
          if (comp_ntype(c, argv[ai]) != rt) return 0;
        int tr = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        for (int ai = 0; ai < argc; ai++) {
          int to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
          buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[ai], b); buf_puts(b, ";");
          buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
          buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
          buf_printf(b, " sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }", hn, tr, tk, hn, to, tk);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc == 1) {
        /* In-place merge: insert each key of `other` into the receiver (a
           conflict-resolution block, if present, picks the kept value), then
           yield the receiver. A typed receiver can't change variant in place,
           so only a same-variant argument is accepted; any other argument
           (including a poly-boxed hash, which can't be variant-checked here
           without risking type confusion) falls through to the unsupported
           path rather than silently dropping or mistyping. */
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        /* merging an empty hash literal is a no-op; yield the receiver. (An
           empty `{}` has no inferable variant, so it can't take the loop.) */
        const char *aty0 = nt_type(nt, argv[0]);
        if (aty0 && (sp_streq(aty0, "HashNode") || sp_streq(aty0, "KeywordHashNode"))) {
          int en = 0; nt_arr(nt, argv[0], "elements", &en);
          if (en == 0) { emit_expr(c, recv, b); return 1; }
        }
        TyKind at = comp_ntype(c, argv[0]);
        if (at != rt) return 0;
        int blk = nt_ref(nt, id, "block");
        int tr = ++g_tmp, to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
        buf_printf(b, " %s _t%d = _t%d->order[_t%d];", c_type_name(kt), tk, to, ti);
        if (blk >= 0) {
          const char *bp0 = block_param_name(c, blk, 0);
          const char *bp1 = block_param_name(c, blk, 1);
          const char *bp2 = block_param_name(c, blk, 2);
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
          buf_printf(b, "); } else { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }", hn, tr, tk, hn, to, tk);
        }
        else {
          buf_printf(b, " sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d));", hn, tr, tk, hn, to, tk);
        }
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
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
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH || rt == TY_POLY_POLY_HASH || rt == TY_INT_INT_HASH ||
           rt == TY_INT_STR_HASH)) {
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
          return 1;
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
        return 1;
      }
      /* except(*keys): a copy of the hash without the given keys (the variants
         that have a runtime delete: str/sym/poly-keyed). */
      /* Hash#slice(k, ...): a fresh hash of the present keys, in argument
         order (CRuby keeps hash order; argument order matches for the common
         literal-key use). Same variants as #except below. */
      if (sp_streq(name, "slice") && hn && argc >= 1 &&
          (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_STR_STR_HASH ||
           rt == TY_STR_INT_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        int th = ++g_tmp, tr = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th);
        emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_%sHash *_t%d = sp_%sHash_new(); SP_GC_ROOT(_t%d);", th, hn, tr, hn, tr);
        TyKind skt = ty_hash_key(rt);
        for (int i = 0; i < argc; i++) {
          int tk = ++g_tmp;
          if (rt == TY_POLY_POLY_HASH) {
            buf_printf(b, " { sp_RbVal _t%d = ", tk); emit_boxed(c, argv[i], b);
          }
          else if (skt == TY_SYMBOL) {
            buf_printf(b, " { sp_sym _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          else if (skt == TY_INT) {
            buf_printf(b, " { mrb_int _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          else {
            buf_printf(b, " { const char *_t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          buf_printf(b, "; if (sp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }",
                     hn, th, tk, hn, tr, tk, hn, th, tk);
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "except") && hn &&
          (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH || rt == TY_STR_STR_HASH ||
           rt == TY_STR_INT_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = sp_%sHash_dup(", hn, t, hn);
        emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d);", t);
        for (int i = 0; i < argc; i++) {
          buf_printf(b, " sp_%sHash_delete(_t%d, ", hn, t);
          if (rt == TY_POLY_POLY_HASH) emit_boxed(c, argv[i], b); else emit_hash_key(c, argv[i], ty_hash_key(rt), b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", t);
        return 1;
      }
      if (sp_streq(name, "invert") && argc == 0) {
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
          /* emit value as sp_RbVal (a PolyPoly receiver reads vals[] directly:
             its _get takes an sp_RbVal key, not the raw order index) (#2407) */
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(b, " sp_RbVal _v%d = _t%d->vals[_t%d->order[_t%d]];", ti, th, th, ti);
          else if (vt == TY_POLY)
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
        return 1;
      }
      if (sp_streq(name, "flatten") && argc == 1) {
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
      if (sp_streq(name, "flatten") && argc == 0) {
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
        return 1;
      }
      if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) {
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        return 1;
      }
      if (sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* sort entries by Array#<=> over each [key, value] pair */
        buf_puts(b, "sp_PolyArray_sort_pairs(");
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_puts(b, ")");
        return 1;
      }
      /* Hash#all?/any?/none?/one? with a pattern argument (no block): test each
         [key, value] pair with `pattern === pair`. An Array pattern (the common
         destructured-pair form) compares by ==, served by sp_poly_eq. */
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        int tp = ++g_tmp, tpat = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tpat); emit_boxed(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = 0;", tc);
        buf_printf(b, " for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tp, ti);
        buf_printf(b, " if (sp_poly_eq(_t%d->data[_t%d], _t%d)) _t%d++;", tp, ti, tpat, tc);
        if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == _t%d->len; })", tc, tp);
        else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
        else                              buf_printf(b, " _t%d == 1; })", tc);
        return 1;
      }
      /* Hash#shift: remove and return the first-inserted [key, value] pair, or
         nil when empty. */
      if (sp_streq(name, "shift") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tp = ++g_tmp, tr = ++g_tmp, tk = ++g_tmp;
        buf_printf(b, "({ sp_%sHash *_t%d = ", hn, th); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = sp_box_nil();", th, tr);
        buf_printf(b, " if (_t%d && _t%d->len > 0) {", th, th);
        /* bind the first key (raw), used for both the pair and the delete */
        if (rt == TY_POLY_POLY_HASH)
          buf_printf(b, " sp_RbVal _t%d = _t%d->keys[_t%d->order[0]];", tk, th, th);
        else if (kt == TY_SYMBOL)
          buf_printf(b, " sp_sym _t%d = _t%d->order[0];", tk, th);
        else if (kt == TY_STRING)
          buf_printf(b, " const char *_t%d = _t%d->order[0];", tk, th);
        else
          buf_printf(b, " mrb_int _t%d = _t%d->order[0];", tk, th);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
        if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_PolyArray_push(_t%d, _t%d);", tp, tk);
        else if (kt == TY_SYMBOL) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(_t%d));", tp, tk);
        else if (kt == TY_STRING) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(_t%d));", tp, tk);
        else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(_t%d));", tp, tk);
        if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->vals[_t%d->order[0]]);", tp, th, th);
        else if (vt == TY_POLY) buf_printf(b, " sp_PolyArray_push(_t%d, sp_%sHash_get(_t%d, _t%d));", tp, hn, th, tk);
        else if (vt == TY_INT) buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_%sHash_get(_t%d, _t%d)));", tp, hn, th, tk);
        else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_%sHash_get(_t%d, _t%d)));", tp, hn, th, tk);
        buf_printf(b, " _t%d = sp_box_poly_array(_t%d);", tr, tp);
        buf_printf(b, " sp_%sHash_delete(_t%d, _t%d); }", hn, th, tk);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* Enumerable first/take/drop over the [key, value] pair list. `first`
         with no argument yields the first pair (nil when empty); the arg forms
         and take/drop return a poly array slice. */
      if (sp_streq(name, "first") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        int tp = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; _t%d->len > 0 ? _t%d->data[0] : sp_box_nil(); })", tp, tp);
        return 1;
      }
      if ((sp_streq(name, "first") || sp_streq(name, "take")) && argc == 1 &&
          nt_ref(nt, id, "block") < 0) {
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to take negative size\"); sp_PolyArray_slice(", tn);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, ", 0, _t%d); })", tn);
        return 1;
      }
      if (sp_streq(name, "drop") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        int tp = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to drop negative size\"); sp_PolyArray_slice(_t%d, _t%d, _t%d->len - _t%d); })", tn, tp, tn, tp, tn);
        return 1;
      }
      if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) {
        /* find first pair where key==arg (assoc) or value==arg (rassoc); returns [k,v] or nil */
        int is_rassoc = sp_streq(name, "rassoc");
        TyKind kt = ty_hash_key(rt), vt = ty_hash_val(rt);
        int th = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, ta = ++g_tmp;
        /* PolyPolyHash's order[] holds SLOT INDEXES; keys/vals index directly.
           The other variants store the KEY in order[] and read values through
           sp_<hn>Hash_get(key). Build the value-read expression accordingly. */
        char vget[96];
        if (rt == TY_POLY_POLY_HASH)
          snprintf(vget, sizeof vget, "_t%d->vals[_t%d->order[_t%d]]", th, th, ti);
        else
          snprintf(vget, sizeof vget, "sp_%sHash_get(_t%d, _t%d->order[_t%d])", hn, th, th, ti);
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
          if (vt == TY_POLY) buf_printf(b, "%s;", vget);
          else if (vt == TY_INT) buf_printf(b, "sp_box_int(%s);", vget);
          else buf_printf(b, "sp_box_str(%s);", vget);
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
          buf_printf(b, " sp_PolyArray_push(_t%d, %s);", tr, vget);
        else if (vt == TY_INT)
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(%s));", tr, vget);
        else
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(%s));", tr, vget);
        buf_printf(b, " break; } } _t%d; })", tr);  /* NULL = nil in poly context */
        return 1;
      }
      if (sp_streq(name, "compact") && argc == 0) {
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
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        int hd_blk = nt_ref(nt, id, "block");
        if (hd_blk >= 0 && nt_type(nt, hd_blk) && sp_streq(nt_type(nt, hd_blk), "BlockNode")) {
          /* delete(key) { |k| fallback }: the block's value stands in for a
             missing key (boxed: the fallback can be any type) */
          const char *dp0 = block_param_name(c, hd_blk, 0);
          int hdb = nt_ref(nt, hd_blk, "body");
          int hdn = 0; const int *hdv = hdb >= 0 ? nt_arr(nt, hdb, "body", &hdn) : NULL;
          int tvv = ++g_tmp;
          buf_printf(b, "; sp_RbVal _t%d; if (sp_%sHash_has_key(_t%d, _t%d)) { _t%d = ",
                     tvv, hn, th, tk, tvv);
          { char getx[96]; snprintf(getx, sizeof getx, "sp_%sHash_get(_t%d, _t%d)", hn, th, tk);
            if (vt == TY_POLY) buf_puts(b, getx);
            else emit_boxed_text(c, vt, getx, b); }
          buf_printf(b, "; sp_%sHash_delete(_t%d, _t%d); } else {", hn, th, tk);
          if (dp0) {
            char keytmp[32]; snprintf(keytmp, sizeof keytmp, "_t%d", tk);
            buf_printf(b, " lv_%s = ", rename_local(dp0));
            if (ty_hash_key(rt) == TY_POLY) buf_puts(b, keytmp);
            else emit_boxed_text(c, ty_hash_key(rt), keytmp, b);
            buf_puts(b, ";");
          }
          buf_printf(b, " _t%d = ", tvv);
          if (hdn > 0) emit_boxed(c, hdv[hdn - 1], b);
          else buf_puts(b, "sp_box_nil()");
          buf_printf(b, "; } _t%d; })", tvv);
          return 1;
        }
        buf_printf(b, "; %s _t%d = sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : %s;",
                   c_type_name(vt), tv, hn, th, tk, hn, th, tk, vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
        buf_printf(b, " sp_%sHash_delete(_t%d, _t%d); _t%d; })", hn, th, tk, tv);
        return 1;
      }
    }
  }
  return 0;
}

/* True when nodes a and b are the same side-effect-free lvalue -- the same local
   or instance variable read. Used to resolve `x.equal?(x)` (object identity) for
   receivers whose value identity is not otherwise modeled: re-reading one
   variable yields the same object, so the reflexive case is certainly true,
   while method calls / literals (which would produce fresh objects, or which the
   C compiler merges) are excluded. */
/* String#upcase and friends take an optional casemap symbol. `:ascii`
   restricts folding to A-Z/a-z; return the "_ascii" runtime suffix for it so
   non-ASCII bytes pass through. Full-Unicode folding (no arg) returns "". */
static const char *case_map_suffix(Compiler *c, int argc, const int *argv) {
  if (argc >= 1 && nt_type(c->nt, argv[0]) &&
      sp_streq(nt_type(c->nt, argv[0]), "SymbolNode") &&
      nt_str(c->nt, argv[0], "value") &&
      sp_streq(nt_str(c->nt, argv[0], "value"), "ascii"))
    return "_ascii";
  return "";
}

static int same_sefree_lvalue(Compiler *c, int a, int b) {
  if (a < 0 || b < 0) return 0;
  const char *ta = nt_type(c->nt, a), *tb = nt_type(c->nt, b);
  if (!ta || !tb || !sp_streq(ta, tb)) return 0;
  if (!sp_streq(ta, "LocalVariableReadNode") && !sp_streq(ta, "InstanceVariableReadNode")) return 0;
  const char *na = nt_str(c->nt, a, "name"), *nb = nt_str(c->nt, b, "name");
  return na && nb && sp_streq(na, nb);
}

int emit_scalar_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
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
    if (rt == TY_STRING && sp_streq(r, "sp_box_nil()")) r = "sp_poly_to_s(sp_box_nil())";
    /* Same shape, but the unresolved-call gate raised (SPINEL_GATE_RAISE): its
       sp_raise_nomethod(...) is a side-effecting poly value, so coerce it (the
       raise diverges before the result is read) rather than feed the raw
       sp_RbVal into a const char* string op. */
    else if (rt == TY_STRING && strncmp(r, "sp_raise_nomethod(", 18) == 0) {
      Buf cb; memset(&cb, 0, sizeof cb); buf_printf(&cb, "sp_poly_to_s(%s)", r); r = cb.p ? cb.p : r;
    }
    int handled = 1;

    if (rt == TY_STRING) {
      /* blockless "a".upto("c") materializes the succ-sequence as an array */
      if (sp_streq(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", 0)");
      }
      /* string methods taking a regex-literal argument route to the engine */
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 &&
               nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
        Buf rp; memset(&rp, 0, sizeof rp);
        emit_regex_pat_to_buf(c, argv[0], &rp);
        buf_printf(b, "sp_re_%s(%s, %s, ", name, rp.p ? rp.p : "NULL", r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
        free(rp.p);
      }
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 &&
               comp_ntype(c, argv[0]) == TY_REGEX) {
        /* pattern held in a regex-typed value (e.g. a local bound to an
           interpolated /.../); dispatch to the compiled-pattern overload
           rather than the string-pattern one. */
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(", name, suf);
        emit_expr(c, argv[0], b); buf_printf(b, ", %s, ", r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "split") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "split") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_split_limit(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "split") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split("); emit_expr(c, argv[0], b);
        buf_printf(b, ", %s)", r);
      }
      else if (sp_streq(name, "split") && argc == 2 && comp_ntype(c, argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split_limit("); emit_expr(c, argv[0], b);
        buf_printf(b, ", %s, ", r); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "scan") && argc == 1 &&
               (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_STRING) &&
               nt_ref(nt, id, "block") >= 0) {
        /* value-form scan { }: iterate in the prelude; the value is the
           receiver string (CRuby returns self from the block form). With
           capture groups the rows come from sp_re_scan_poly: one param
           binds the group row itself, several destructure it (a group that
           did not participate binds nil). */
        int blk = nt_ref(nt, id, "block");
        int re_idx = re_lit_index(c, argv[0]);
        int has_cap = re_idx >= 0 && re_has_captures(re_lit_src(c, argv[0]));
        int np = 0; while (block_param_name(c, blk, np)) np++;
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        int tr = ++g_tmp, tm = ++g_tmp, ti = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "const char *_t%d = %s;\n", tr, r);
        emit_indent(g_pre, g_indent);
        if (has_cap)
          buf_printf(g_pre, "sp_PolyArray *_t%d = sp_re_scan_poly(sp_re_pat_%d, _t%d); SP_GC_ROOT(_t%d);\n",
                     tm, re_idx, tr, tm);
        else if (re_idx >= 0)
          buf_printf(g_pre, "sp_StrArray *_t%d = sp_re_scan(sp_re_pat_%d, _t%d); SP_GC_ROOT(_t%d);\n",
                     tm, re_idx, tr, tm);
        else {
          buf_printf(g_pre, "sp_StrArray *_t%d = sp_str_scan(_t%d, ", tm, tr);
          emit_expr(c, argv[0], g_pre);
          buf_printf(g_pre, "); SP_GC_ROOT(_t%d);\n", tm);
        }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tm, ti);
        if (has_cap && np >= 2) {
          int trow = ++g_tmp;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray *_t%d = (sp_PolyArray *)_t%d->data[_t%d].v.p;\n", trow, tm, ti);
          for (int pj = 0; pj < np; pj++) {
            const char *pn = rename_local(block_param_name(c, blk, pj));
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "lv_%s = (_t%d && _t%d->len > %d && _t%d->data[%d].tag == SP_TAG_STR) ? _t%d->data[%d].v.s : NULL;\n",
                       pn, trow, trow, pj, trow, pj, trow, pj);
          }
        }
        else if (block_param_name(c, blk, 0)) {
          const char *p0r = rename_local(block_param_name(c, blk, 0));
          emit_indent(g_pre, g_indent + 1);
          if (has_cap)
            buf_printf(g_pre, "lv_%s = (sp_PolyArray *)_t%d->data[_t%d].v.p;\n", p0r, tm, ti);
          else
            buf_printf(g_pre, "lv_%s = _t%d->data[_t%d];\n", p0r, tm, ti);
        }
        int svind = g_indent; g_indent++;
        for (int j = 0; j < bn; j++) emit_stmt(c, bb[j], g_pre, g_indent);
        g_indent = svind;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", tr);
      }
      else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               !re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "scan") && argc == 1 && re_lit_index(c, argv[0]) >= 0 &&
               re_has_captures(re_lit_src(c, argv[0]))) {
        buf_printf(b, "sp_re_scan_poly(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "scan") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_scan(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "to_sym") || sp_streq(name, "intern")) buf_printf(b, "sp_sym_intern(%s)", r);
      else if (sp_streq(name, "to_c") && argc == 0) buf_printf(b, "sp_str_to_c(%s)", r);
      else if (sp_streq(name, "chr") && argc == 0) buf_printf(b, "sp_str_substr(%s, 0, 1)", r);
      else if (sp_streq(name, "length") || sp_streq(name, "size")) {
        if (g_hoist_len_var && g_hoist_len_recv && recv >= 0 && nt_type(nt, recv) &&
            sp_streq(nt_type(nt, recv), "LocalVariableReadNode") && nt_str(nt, recv, "name") &&
            sp_streq(nt_str(nt, recv, "name"), g_hoist_len_recv))
          buf_puts(b, g_hoist_len_var);
        else buf_printf(b, "sp_str_length_m(%s)", r);
      }
      else if (sp_streq(name, "bytesize")) buf_printf(b, "sp_str_bytesize_m(%s)", r);
      else if (sp_streq(name, "upcase"))     buf_printf(b, "sp_str_upcase%s(%s)", case_map_suffix(c, argc, argv), r);
      else if (sp_streq(name, "downcase"))   buf_printf(b, "sp_str_downcase%s(%s)", case_map_suffix(c, argc, argv), r);
      else if (sp_streq(name, "capitalize")) buf_printf(b, "sp_str_capitalize%s(%s)", case_map_suffix(c, argc, argv), r);
      else if (sp_streq(name, "swapcase"))   buf_printf(b, "sp_str_swapcase%s(%s)", case_map_suffix(c, argc, argv), r);
      else if (sp_streq(name, "dedup") && argc == 0) buf_printf(b, "sp_str_uminus_val(%s)", r);
      else if (sp_streq(name, "delete_prefix") && argc == 1) { buf_printf(b, "sp_str_delete_prefix(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete_suffix") && argc == 1) { buf_printf(b, "sp_str_delete_suffix(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "reverse"))    buf_printf(b, "sp_str_reverse(%s)", r);
      else if (sp_streq(name, "strip"))      buf_printf(b, "sp_str_strip(%s)", r);
      else if (sp_streq(name, "lstrip"))     buf_printf(b, "sp_str_lstrip(%s)", r);
      else if (sp_streq(name, "rstrip"))     buf_printf(b, "sp_str_rstrip(%s)", r);
      else if (sp_streq(name, "chomp") && argc == 1) {
        const char *a0ty = nt_type(nt, argv[0]);
        if (a0ty && sp_streq(a0ty, "NilNode")) {
          /* chomp(nil) returns the string unchanged */
          buf_puts(b, r);
        }
        else {
          buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
      }
      else if (sp_streq(name, "chomp"))      buf_printf(b, "sp_str_chomp(%s)", r);
      else if (sp_streq(name, "chop"))       buf_printf(b, "sp_str_chop(%s)", r);
      else if (sp_streq(name, "to_s")) {
        /* NOT the identity: a nullable string carries nil as NULL, and
           CRuby's nil.to_s is "" -- the coalesce keeps `ENV[missing].to_s`
           comparable against "" (#1664). A provably non-nil receiver costs
           one always-taken branch. */
        int tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; _t%d ? _t%d : SPL(\"\"); })", tv, r, tv, tv);
      }
      else if (sp_streq(name, "to_str")) {
        /* Unlike to_s, CRuby's nil has no to_str: raise. */
        int tv = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; if (!_t%d) sp_nil_recv(\"to_str\"); _t%d; })", tv, r, tv, tv);
      }
      else if ((sp_streq(name, "dup") || sp_streq(name, "clone")) &&
               (argc == 0 ||
                (argc == 1 && sp_streq(name, "clone") && nt_type(nt, argv[0]) &&
                 sp_streq(nt_type(nt, argv[0]), "KeywordHashNode") &&
                 ({ int _fv = kwh_lookup(nt, argv[0], "freeze");
                    const char *_ft = _fv >= 0 ? nt_type(nt, _fv) : NULL;
                    _ft && (sp_streq(_ft, "FalseNode") || sp_streq(_ft, "TrueNode") ||
                            sp_streq(_ft, "NilNode")); })))) {
        /* sp_str_dup, not dup_external: the receiver is a spinel string, and
           the byte_len-aware copy carries embedded NULs (dup_external is for
           unmarked C pointers and must stay strlen-based). clone's literal
           freeze: keyword forces the copy's frozen state (nil/absent keeps
           clone's default); a non-literal value stays a loud reject. */
        int fz1 = 0;
        if (argc == 1) {
          int fv = kwh_lookup(nt, argv[0], "freeze");
          const char *ft = fv >= 0 ? nt_type(nt, fv) : NULL;
          fz1 = ft && sp_streq(ft, "TrueNode");
        }
        if (fz1) buf_printf(b, "sp_str_freeze_val(sp_str_dup(%s))", r);
        else buf_printf(b, "sp_str_dup(%s)", r);
      }
      else if (sp_streq(name, "inspect"))    { int tv = ++g_tmp; buf_printf(b, "({ const char *_t%d = %s; _t%d ? sp_str_inspect(_t%d) : SPL(\"nil\"); })", tv, r, tv, tv); }
      else if (sp_streq(name, "empty?"))     buf_printf(b, "sp_str_empty_p(%s)", r);
      else if (sp_streq(name, "include?") && argc == 1) {
        buf_printf(b, "sp_str_include(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "start_with?") || sp_streq(name, "end_with?")) && argc >= 2) {
        /* several candidates: true when any matches (receiver bound once) */
        int tv = ++g_tmp;
        const char *fn = sp_streq(name, "start_with?") ? "sp_str_start_with" : "sp_str_end_with";
        buf_printf(b, "({ const char *_t%d = %s; (", tv, r);
        for (int j = 0; j < argc; j++) {
          if (j) buf_puts(b, " || ");
          buf_printf(b, "%s(_t%d, ", fn, tv);
          emit_str_expr(c, argv[j], b);
          buf_puts(b, ")");
        }
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "start_with?") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s.start_with?(/re/): true when the pattern matches at index 0 */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) == 0)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "start_with?") && argc == 1) {
        buf_printf(b, "sp_str_start_with(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "end_with?") && argc == 1) {
        buf_printf(b, "sp_str_end_with(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ascii_only?") && argc == 0) buf_printf(b, "sp_str_ascii_only(%s)", r);
      else if (sp_streq(name, "valid_encoding?") && argc == 0) buf_printf(b, "sp_str_valid_encoding(%s)", r);
      else if (sp_streq(name, "index") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* nullable-int carrier (SP_INT_NIL on miss), matching the inferred
           type -- the poly-boxed form broke a variable-regexp argument */
        int tmi = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = sp_re_match(sp_re_pat_%d, %s); _t%d < 0 ? SP_INT_NIL : _t%d; })",
                   tmi, re_lit_index(c, argv[0]), r, tmi, tmi);
      }
      else if (sp_streq(name, "index") && argc == 1) {
        /* nil-on-miss carried as the SP_INT_NIL sentinel (a nullable int) */
        buf_printf(b, "sp_str_index_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "index") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_index_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "index") && argc == 2) {
        buf_printf(b, "sp_str_index_from_opt(%s, ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ", ");
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      /* byteindex/byterindex over a String needle: BYTE-offset search (result +
         start are byte offsets). The runtime helpers already carry nil as
         SP_INT_NIL. A Regexp needle is a separate feature -- not handled here,
         so it falls through to the unsupported-call reject. */
      else if (sp_streq(name, "byteindex") && (argc == 1 || argc == 2) &&
               re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_byteindex_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        if (argc == 2) emit_int_expr(c, argv[1], b); else buf_puts(b, "0");
        buf_puts(b, ")");
      }
      else if (sp_streq(name, "byterindex") && (argc == 1 || argc == 2) &&
               re_lit_index(c, argv[0]) >= 0) {
        int tsr = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; sp_re_byterindex_opt(sp_re_pat_%d, _t%d, ",
                   tsr, r, re_lit_index(c, argv[0]), tsr);
        if (argc == 2) emit_int_expr(c, argv[1], b);
        else buf_printf(b, "(mrb_int)sp_str_byte_len(_t%d)", tsr);
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "byteindex") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_byteindex(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byteindex") && argc == 2 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_byteindex_from(%s, ", r); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byterindex") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_byterindex(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byterindex") && argc == 2 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_byterindex_from(%s, ", r); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "partition") || sp_streq(name, "rpartition")) && argc == 1 &&
               re_lit_index(c, argv[0]) < 0) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "partition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
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
      else if (sp_streq(name, "rpartition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rpartition(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "rindex") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rindex_opt(sp_re_pat_%d, %s)", re_lit_index(c, argv[0]), r);
      }
      else if (sp_streq(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex_opt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "rindex") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rindex_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "crypt") && argc == 1) { buf_printf(b, "sp_str_crypt(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "scrub") && argc == 0) buf_printf(b, "sp_str_scrub(%s, 0)", r);
      else if (sp_streq(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/] -> the matched substring, or nil (NULL) on no match */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2 && re_lit_index(c, argv[0]) >= 0 &&
               nt_type(c->nt, argv[1]) && sp_streq(nt_type(c->nt, argv[1]), "SymbolNode")) {
        /* s[/(?<g>...)/, :g] -> the named group, or nil */
        int pi = re_lit_index(c, argv[0]);
        const char *gname = nt_str(c->nt, argv[1], "value");
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_named_capture(sp_re_pat_%d, \"%s\") : NULL)",
                   pi, r, pi, gname ? gname : "");
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/, n] -> capture group n (0 = whole match), or nil */
        int pi = re_lit_index(c, argv[0]);
        int tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; sp_re_match(sp_re_pat_%d, %s) >= 0 ? "
                      "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL)) : NULL; })",
                   pi, r, tn, tn, tn, tn);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_RANGE &&
               !(nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode"))) {
        /* a Range VALUE (variable / expression): slice through the runtime
           bounds (the literal form keeps its specialized arm below) */
        int trg2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", trg2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_str_sub_range_r(%s, _t%d.first, _t%d.last, (int)_t%d.excl); })",
                   r, trg2, trg2, trg2);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && nt_type(c->nt, argv[0]) &&
               sp_streq(nt_type(c->nt, argv[0]), "RangeNode")) {
        /* s[a..b] / s[a...b]; beginless/endless ranges use 0 / length */
        int rn = argv[0];
        int excl = (int)(nt_int(c->nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
        buf_printf(b, "sp_str_sub_range_r(%s, ", r);
        if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) { emit_int_expr(c, hi, b); buf_printf(b, ", %d)", excl); }
        else buf_printf(b, "(mrb_int)sp_str_length(%s), 0)", r);  /* endless: to the end */
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2) {
        /* s[start, len] */
        buf_printf(b, "sp_str_sub_range(%s, ", r);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        /* s["sub"] -> the substring if present, else nil */
        int tsub = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tsub); emit_str_expr(c, argv[0], b);
        buf_printf(b, "; (strstr(%s, _t%d) ? _t%d : NULL); })", r, tsub, tsub);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1) {
        buf_printf(b, "sp_str_char_at_or_nil(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "split") && argc == 0) buf_printf(b, "sp_str_split_ws(%s)", r);
      else if (sp_streq(name, "split") && argc == 1) {
        /* split(nil) and split(" ") are whitespace-mode; split(sep) drops trailing empties */
        const char *aty = nt_type(c->nt, argv[0]);
        int nil_arg = aty && sp_streq(aty, "NilNode");
        int ws = nil_arg || (aty && sp_streq(aty, "StringNode") && nt_str(c->nt, argv[0], "content") &&
                 sp_streq(nt_str(c->nt, argv[0], "content"), " "));
        if (ws) buf_printf(b, "sp_str_split_ws(%s)", r);
        else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (sp_streq(name, "split") && argc == 2) {
        buf_printf(b, "sp_str_split_limit(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && (argc == 2 ||
               (argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")))) {
        int lo_n, hi_n;
        if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
        else { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
        int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; const char *_t%d = ", tc, r, tlo); emit_expr(c, lo_n, b);
        buf_printf(b, "; const char *_t%d = ", thi); emit_expr(c, hi_n, b);
        buf_printf(b, "; strcmp(_t%d, _t%d) < 0 ? _t%d : (strcmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                   tc, tlo, tlo, tc, thi, thi, tc);
      }
      else if (sp_streq(name, "oct") && argc == 0) buf_printf(b, "sp_str_oct(%s)", r);
      else if (sp_streq(name, "hex") && argc == 0) buf_printf(b, "sp_str_to_i_base(%s, 16)", r);
      else if (sp_streq(name, "to_r") && argc == 0) buf_printf(b, "sp_str_to_r(%s)", r);
      else if (sp_streq(name, "ord") && argc == 0) buf_printf(b, "sp_str_ord(%s)", r);
      else if ((sp_streq(name, "force_encoding") || sp_streq(name, "b") || sp_streq(name, "encode")) && argc <= 1) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "encoding") && argc == 0) buf_printf(b, "((void)(%s), sp_box_encoding(sp_encoding_utf8()))", r);
      else if (sp_streq(name, "dump") && argc == 0) buf_printf(b, "sp_str_dump(%s)", r);
      else if (sp_streq(name, "undump") && argc == 0) buf_printf(b, "sp_str_undump(%s)", r);
      else if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_POLY) {
        /* runtime tag decides: a string argument compares, anything else is
           nil (the call typed TY_POLY) */
        int tb2 = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", tb2); emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_STR ? ", tb2);
        if (sp_streq(name, "casecmp"))
          buf_printf(b, "sp_box_int(sp_str_casecmp(%s, _t%d.v.s ? _t%d.v.s : \"\"))", r, tb2, tb2);
        else
          buf_printf(b, "sp_box_bool(sp_str_casecmp(%s, _t%d.v.s ? _t%d.v.s : \"\") == 0)", r, tb2, tb2);
        buf_puts(b, " : sp_box_nil(); })");
      }
      else if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
               comp_ntype(c, argv[0]) != TY_STRING && comp_ntype(c, argv[0]) != TY_UNKNOWN) {
        /* statically non-string argument: nil (the call typed TY_NIL); the
           argument still evaluates for effect */
        buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
      }
      else if (sp_streq(name, "casecmp") && argc == 1) { buf_printf(b, "sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "casecmp?") && argc == 1) { buf_printf(b, "(sp_str_casecmp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ") == 0)"); }
      else if (sp_streq(name, "byteslice") && argc == 2) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      /* byteslice(range): resolve endpoints against the bytesize (#2348) */
      else if (sp_streq(name, "byteslice") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        int trg = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", trg); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_str_byteslice_range(%s, _t%d.first, _t%d.last, _t%d.excl,"
                      " _t%d.first == INTPTR_MIN, _t%d.last == INTPTR_MAX); })",
                   r, trg, trg, trg, trg, trg);
      }
      /* single-index byteslice(i): nil at the bytesize boundary (#2333) */
      else if (sp_streq(name, "byteslice") && argc == 1) { buf_printf(b, "sp_str_byteslice1(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "setbyte") && argc == 2) {
        /* copy-on-write: rebind an lvalue receiver to the mutated copy
           (a literal's bytes live in static storage, #2029) */
        const char *rvt2 = nt_type(nt, recv);
        int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                           sp_streq(rvt2, "InstanceVariableReadNode"));
        int tv2 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tv2); emit_expr(c, argv[1], b);
        buf_puts(b, "; ");
        if (lvw) { emit_expr(c, recv, b); buf_puts(b, " = "); }
        buf_printf(b, "sp_str_setbyte_cow(%s, ", r); emit_expr(c, argv[0], b);
        buf_printf(b, ", _t%d); _t%d; })", tv2, tv2);
      }
      else if (sp_streq(name, "getbyte") && argc == 1) {
        /* Bounds/negative-correct: a negative index counts from the end and an
           out-of-range index is nil (SP_INT_NIL) -- getbyte is a nullable int. */
        buf_printf(b, "sp_str_getbyte_opt(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "squeeze") && argc == 0) buf_printf(b, "sp_str_squeeze(%s)", r);
      else if (sp_streq(name, "squeeze") && argc == 1) { buf_printf(b, "sp_str_squeeze_chars(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "squeeze") && argc >= 2) {
        buf_printf(b, "sp_str_squeeze_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if ((sp_streq(name, "tr") || sp_streq(name, "tr_s")) && argc == 2) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "delete") && argc == 0) { buf_printf(b, "(%s)", r); return 1; }
      else if (sp_streq(name, "delete") && argc == 1) { buf_printf(b, "sp_str_delete(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete") && argc >= 2) {
        buf_printf(b, "sp_str_delete_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "count") && argc == 0) { buf_printf(b, "(sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\"), 0LL)"); return 1; }
      else if (sp_streq(name, "count") && argc == 1) { buf_printf(b, "sp_str_count(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "count") && argc >= 2) {
        buf_printf(b, "sp_str_count_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (sp_streq(name, "lines") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_lines_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "lines") && argc == 1 && nt_type(nt, argv[0]) &&
               sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
        int is_chomp = (chomp_v >= 0 && nt_type(nt, chomp_v) &&
                        sp_streq(nt_type(nt, chomp_v), "TrueNode"));
        buf_printf(b, "%s(%s)", is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", r);
      }
      else if (sp_streq(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (sp_streq(name, "codepoints") && argc == 0) buf_printf(b, "sp_str_codepoints(%s)", r);
      else if (sp_streq(name, "unpack") && argc == 1)  { buf_printf(b, "sp_str_unpack(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* unpack(fmt, offset: n): a trailing KeywordHashNode carries the offset. */
      else if ((sp_streq(name, "unpack") || sp_streq(name, "unpack1")) && argc == 2 &&
               nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") &&
               struct_kwarg_value(c, argv[1], "offset") >= 0) {
        int offv = struct_kwarg_value(c, argv[1], "offset");
        int one = sp_streq(name, "unpack1");
        TyKind u1t = one ? comp_ntype(c, id) : TY_POLY;
        if (one && u1t == TY_INT)        buf_puts(b, "sp_poly_to_i(sp_PolyArray_get(");
        else if (one && u1t == TY_FLOAT) buf_puts(b, "sp_poly_to_f_opt(sp_PolyArray_get(");
        else if (one)                    buf_puts(b, "sp_PolyArray_get(");
        buf_printf(b, "sp_str_unpack_off(%s, ", r); emit_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, offv, b); buf_puts(b, ")");
        if (one) buf_puts(b, (u1t == TY_INT || u1t == TY_FLOAT) ? ", 0))" : ", 0)");
      }
      else if (sp_streq(name, "unpack1") && argc == 1) {
        /* A literal single-directive numeric format fixes the value's type
           (the analyzer's an_unpack1_lit_type): unbox the extracted element
           (int, or float? -- the _opt keeps a padded nil from short input
           as float-nil instead of 0.0). */
        TyKind u1t = comp_ntype(c, id);
        if (u1t == TY_INT)        buf_printf(b, "sp_poly_to_i(sp_PolyArray_get(sp_str_unpack(%s, ", r);
        else if (u1t == TY_FLOAT) buf_printf(b, "sp_poly_to_f_opt(sp_PolyArray_get(sp_str_unpack(%s, ", r);
        else                      buf_printf(b, "sp_PolyArray_get(sp_str_unpack(%s, ", r);
        emit_expr(c, argv[0], b);
        buf_puts(b, (u1t == TY_INT || u1t == TY_FLOAT) ? "), 0))" : "), 0)");
      }
      else if (sp_streq(name, "sum") && argc <= 1) {
        /* byte checksum: sum of byte values modulo 2**bits (default 16;
           bits <= 0 or >= 64 leaves the sum untruncated like CRuby) */
        int ts = ++g_tmp, tp = ++g_tmp, tacc = ++g_tmp, tbits = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; mrb_int _t%d = ", ts, r, tbits);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "16");
        buf_printf(b, "; mrb_int _t%d = 0; for (const char *_t%d = _t%d; *_t%d; _t%d++)"
                      " _t%d += (unsigned char)*_t%d;"
                      " (_t%d <= 0 || _t%d >= 64) ? _t%d : (_t%d & ((((mrb_int)1) << _t%d) - 1)); })",
                   tacc, tp, ts, tp, tp, tacc, tp, tbits, tbits, tacc, tacc, tbits);
      }
      else if (sp_streq(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
      else if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) buf_printf(b, "sp_str_succ(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "to_f") && argc == 0)    buf_printf(b, "sp_str_to_f_cruby(%s)", r);  /* underscores (#2330) */
      else if (sp_streq(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2 && comp_ntype(c, argv[1]) == TY_STR_STR_HASH) {
        buf_printf(b, "sp_str_sub_str_str_hash(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2) {
        buf_printf(b, "sp_str_sub(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 2) {
        buf_printf(b, "sp_str_center2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 2) {
        buf_printf(b, "sp_str_ljust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 2) {
        buf_printf(b, "sp_str_rjust2(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      /* String#eql?(x): byte-equal only when x is itself String-typed (no
         coercion, unlike ==). A poly arg checks its tag; any other concrete
         type is never equal. */
      else if (sp_streq(name, "eql?") && argc == 1) {
        if (a0 == TY_STRING) { buf_printf(b, "sp_str_eq(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else if (a0 == TY_POLY) {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_STR && sp_str_eq(_t%d.v.s, %s); })", te, te, r);
        }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      /* String#equal?(x): object identity. A String is a `const char *` whose
         literals the C compiler merges at -O2, so raw pointer equality would
         wrongly equate distinct equal-valued literals (`a = "x"; b = "x"`).
         Only the unambiguous reflexive case -- the same side-effect-free local
         or ivar read on both sides (`x.equal?(x)`) -- is certainly identity-
         true; every other form is conservatively false, still evaluating the
         argument for its side effects. */
      else if (sp_streq(name, "equal?") && argc == 1) {
        TyKind eqa = comp_ntype(c, argv[0]);
        /* a mutable StringBuffer local as the argument: compare the buffer's
           OWN cstr pointer -- the plain read emits a defensive snapshot copy
           (sp_str_concat(cstr, "")), which would break `(s << "x").equal?(s)`
           (#2307). Hoist the receiver first so its in-place append lands
           before the argument's cstr is read. */
        int eq_sblv = 0;
        {
          const char *aty0 = nt_type(nt, argv[0]);
          if (aty0 && sp_streq(aty0, "LocalVariableReadNode")) {
            const char *an0 = nt_str(nt, argv[0], "name");
            LocalVar *alv0 = an0 ? scope_local(comp_scope_of(c, argv[0]), an0) : NULL;
            if (alv0 && alv0->type == TY_STRBUF) {
              int teq = ++g_tmp;
              buf_printf(b, "({ const char *_t%d = %s; "
                            "(const void *)_t%d == (const void *)sp_String_cstr(lv_%s); })",
                         teq, r, teq, rename_local(an0));
              eq_sblv = 1;
            }
          }
        }
        if (eq_sblv) { /* emitted above */ }
        else if (eqa == TY_STRING) {
          /* string identity IS pointer identity (s.freeze.equal?(s) must be
             true: freeze marks in place and returns the same pointer) */
          buf_printf(b, "((const void *)(%s) == (const void *)(", r);
          emit_expr(c, argv[0], b);
          buf_puts(b, "))");
        }
        else if (same_sefree_lvalue(c, recv, argv[0])) { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)"); }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      else handled = 0;
    }
    else if (rt == TY_INT) {
      /* a nullable int's to_s/inspect tests the value and converts it -- bind
         the receiver to a temp first so a side-effecting `r` (e.g. ARGF.read,
         a method call) is evaluated exactly once, not twice. */
      if (sp_streq(name, "to_s") && argc == 0) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (sp_streq(name, "inspect")) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (sp_streq(name, "to_f"))   buf_printf(b, "((mrb_float)(%s))", r);
      else if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "floor") ||
                sp_streq(name, "ceil") || sp_streq(name, "round") || sp_streq(name, "truncate")) &&
               argc == 0) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "round") && argc >= 1 && nt_type(nt, argv[argc - 1]) &&
               sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
        int hv2 = kwh_lookup(nt, argv[argc - 1], "half");
        const char *hm = (hv2 >= 0 && nt_type(nt, hv2) && sp_streq(nt_type(nt, hv2), "SymbolNode"))
                           ? nt_str(nt, hv2, "value") : NULL;
        if (argc == 1) buf_printf(b, "(%s)", r);   /* no digits: self */
        else {
          int md = hm && sp_streq(hm, "even") ? 0 : hm && sp_streq(hm, "down") ? 2 : 1;
          buf_printf(b, "sp_int_round_half(%s, ", r);
          emit_int_expr(c, argv[0], b);
          buf_printf(b, ", %d)", md);
        }
      }
      else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
                sp_streq(name, "round") || sp_streq(name, "truncate")) && argc == 1) {
        buf_printf(b, "sp_int_%s(%s, ", name, r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "abs"))    buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "chr") && argc == 0) buf_printf(b, "sp_int_chr(%s)", r);
      else if (sp_streq(name, "chr") && argc == 1) {
        /* Integer#chr(Encoding::X): the encoding argument is resolved at
           compile time from the constant path (Encoding values barely exist
           as runtime objects). UTF_8 encodes the codepoint (1-4 bytes);
           the single-byte encodings keep byte semantics. A dynamic or
           unknown encoding is a loud reject, not a silent byte-truncation
           (which is what this arm previously did for EVERY chr(enc)). */
        const char *enm = NULL, *parnm = NULL;
        if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantPathNode")) {
          enm = nt_str(nt, argv[0], "name");
          int par = nt_ref(nt, argv[0], "parent");
          parnm = (par >= 0 && nt_type(nt, par) &&
                   sp_streq(nt_type(nt, par), "ConstantReadNode"))
                  ? nt_str(nt, par, "name") : NULL;
        }
        if (parnm && sp_streq(parnm, "Encoding") && enm && sp_streq(enm, "UTF_8"))
          buf_printf(b, "sp_int_chr_utf8(%s)", r);
        else if (parnm && sp_streq(parnm, "Encoding") && enm &&
                 (sp_streq(enm, "US_ASCII") || sp_streq(enm, "ASCII_8BIT") ||
                  sp_streq(enm, "BINARY")))
          buf_printf(b, "sp_int_chr(%s)", r);
        else
          unsupported(c, id, "Integer#chr with a non-constant or unsupported encoding");
      }
      else if (sp_streq(name, "[]") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* bit-slice: n[lo..hi] extracts hi-lo+1 bits starting at lo; an
           endless range keeps everything above lo; a beginless range raises
           like CRuby (the field below bit 0 is infinite) */
        int trb = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", trb); emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _lo%d = _t%d.first == INTPTR_MIN"
                      " ? (sp_raise_cls(\"ArgumentError\","
                      " \"The beginless range for Integer#[] results in infinity\"), 0)"
                      " : _t%d.first;"
                      " mrb_int _sh%d = ((%s) >> _lo%d);"
                      " _t%d.last == INTPTR_MAX ? _sh%d"
                      " : (_sh%d & ((((mrb_int)1) << (_t%d.last - _lo%d + (_t%d.excl ? 0 : 1))) - 1)); })",
                   trb, trb, trb,
                   trb, r, trb,
                   trb, trb,
                   trb, trb, trb, trb);
      }
      else if (sp_streq(name, "[]") && argc == 1) {
        /* clamped: a literal-folded out-of-range index was an undefined C
           shift (right answer on x86's masked shifts, garbage elsewhere) */
        buf_printf(b, "sp_int_bit((%s), ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "bit_length") && argc == 0) buf_printf(b, "sp_int_bit_length(%s)", r);
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((mrb_float)(%s) / (", r); emit_float_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (sp_streq(name, "[]") && argc == 2) {
        /* n[start, len]: the len-bit field starting at bit `start`. Routed
           through a runtime helper that clamps an out-of-range start/len so
           the shift never goes undefined. */
        buf_printf(b, "sp_int_bit_range((%s), ", r); emit_int_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ord") || sp_streq(name, "to_int")) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "integer?")) { buf_printf(b, "((void)(%s), TRUE)", r); }
      /* Integer is always finite and real; #infinite? is nil (#2329) */
      else if (sp_streq(name, "finite?")) buf_printf(b, "((void)(%s), TRUE)", r);
      else if (sp_streq(name, "real?"))   buf_printf(b, "((void)(%s), TRUE)", r);
      else if (sp_streq(name, "infinite?")) buf_printf(b, "((void)(%s), SP_INT_NIL)", r);
      /* Numeric / Complex-projection methods on a real Integer (#2328) */
      else if (sp_streq(name, "abs2"))    buf_printf(b, "sp_int_mul(%s, %s)", r, r);  /* overflow-checked (#2424) */
      else if (sp_streq(name, "real"))    buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "imaginary") || sp_streq(name, "imag")) buf_printf(b, "((void)(%s), 0)", r);
      else if (sp_streq(name, "conj") || sp_streq(name, "conjugate")) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "i") && argc == 0) buf_printf(b, "((sp_Complex){0.0, (mrb_float)(%s), 0})", r);
      /* arg/angle/phase: 0 (Integer) for >= 0, PI (Float) for < 0 -> poly */
      else if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase"))
        buf_printf(b, "((%s) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))", r);
      else if ((sp_streq(name, "rect") || sp_streq(name, "rectangular")) && argc == 0) {
        int o = ++g_tmp;
        buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_IntArray_push(_t%d, (%s)); sp_IntArray_push(_t%d, 0); _t%d; })",
                   o, o, o, r, o, o);
      }
      else if (sp_streq(name, "polar") && argc == 0) {
        int o = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int((%s) < 0 ? -(%s) : (%s)));"
                      " sp_PolyArray_push(_t%d, (%s) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0)); _t%d; })",
                   o, o, o, r, r, r, o, r, o);
      }
      else if (sp_streq(name, "even?"))  buf_printf(b, "((%s) %% 2 == 0)", r);
      else if (sp_streq(name, "odd?"))   buf_printf(b, "((%s) %% 2 != 0)", r);
      else if (sp_streq(name, "zero?"))  buf_printf(b, "((%s) == 0)", r);
      else if (sp_streq(name, "nonzero?")) buf_printf(b, "((%s) == 0 ? SP_INT_NIL : (%s))", r, r);
      else if (sp_streq(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (sp_streq(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* a Float divisor divides as floats: [floor-quotient Integer, Float mod] */
        int tb = ++g_tmp, tq = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " mrb_int _t%d = (mrb_int)floor((double)(%s) / _t%d);"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float((double)(%s) - (double)_t%d * _t%d)); _t%d; })",
                   tb, tq, r, tb, o, o, o, tq, o, r, tq, tb, o);
      }
      else if (sp_streq(name, "divmod") && argc == 1 &&
               comp_ntype(c, argv[0]) != TY_RATIONAL) {
        int tb = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
      }
      else if (sp_streq(name, "div") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* Integer#div(Float) floors the real quotient (7.div(2.5) == 2) (#2425) */
        buf_printf(b, "((mrb_int)floor((double)(%s) / (", r); emit_expr(c, argv[0], b); buf_puts(b, ")))");
      }
      else if (sp_streq(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "({ (void)(");
        emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_raise_cls(\"TypeError\", \"not an integer\"); (mrb_int)(%s); })", r);
      }
      else if (sp_streq(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "lcm") && argc == 1) { buf_printf(b, "sp_lcm(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "magnitude") && argc == 0) buf_printf(b, "((%s) < 0 ? -(%s) : (%s))", r, r, r);
      else if (sp_streq(name, "modulo") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        int tb = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; (double)(%s) - _t%d * floor((double)(%s) / _t%d); })",
                   r, tb, r, tb);
      }
      else if ((sp_streq(name, "modulo") || sp_streq(name, "%%")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_RATIONAL) {
        /* Integer % Rational lifts the receiver to n/1 (floor modulo) */
        buf_printf(b, "sp_rational_mod(sp_rational_new((mrb_int)(%s), 1), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "remainder") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* x - y * (x/y).truncate, in doubles (7.remainder(2.5) is 2.0) */
        int tb = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; (double)(%s) - _t%d * trunc((double)(%s) / _t%d); })",
                   r, tb, r, tb);
      }
      else if (sp_streq(name, "remainder") && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_RATIONAL) {
        buf_printf(b, "sp_rational_rem(sp_rational_new((mrb_int)(%s), 1), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "remainder") && argc == 1) { buf_printf(b, "sp_iremainder(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
        /* [floor quotient (Integer), self - q*b (Rational)] */
        int ta = ++g_tmp, tb2 = ++g_tmp, tq2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = sp_rational_new((mrb_int)(%s), 1); sp_Rational _t%d = ", ta, r, tb2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; mrb_int _t%d = sp_rational_floor_i(sp_rational_div(_t%d, _t%d));"
                      " sp_Rational _r = sp_rational_sub(_t%d, sp_rational_mul(sp_rational_new(_t%d, 1), _t%d));"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_rational(_r)); _t%d; })",
                   tq2, ta, tb2, ta, tq2, tb2, to2, to2, to2, tq2, to2, to2);
      }
      else if (sp_streq(name, "size") && argc == 0) buf_puts(b, "((mrb_int)sizeof(mrb_int))");
      else if (sp_streq(name, "gcdlcm") && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "); (void)(%s); sp_raise_cls(\"TypeError\", \"not an integer\");"
                      " sp_IntArray_new(); })", r);
      }
      else if (sp_streq(name, "gcdlcm") && argc == 1) {
        int ta = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_gcd(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_lcm(%s, _t%d)); _t%d; })", o, o, r, ta, o, r, ta, o);
      }
      /* a nil bound is an open side: clamp one-sided (or return the receiver),
         boxed so the chosen operand keeps its class (#2588) */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL)) {
        buf_printf(b, "sp_num_clamp_open(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      /* A Float (or runtime-typed poly) bound makes the applied bound or the
         in-range receiver decide the result class at runtime, so box the
         operands and return whichever is chosen unchanged via sp_num_clamp. */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_FLOAT || comp_ntype(c, argv[1]) == TY_FLOAT ||
                comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[1]) == TY_POLY)) {
        buf_printf(b, "sp_num_clamp(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) {
        /* int.clamp(float_range): the clamped-to bound is the Float endpoint (a
           boxed result); an in-range Int receiver stays Int. */
        int tv3 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s); sp_FloatRange _fr%d = ", tv3, r, tv3); emit_expr(c, argv[0], b);
        buf_printf(b, "; ((double)_t%d < _fr%d.first) ? sp_box_float(_fr%d.first)"
                      " : ((double)_t%d > _fr%d.last) ? sp_box_float(_fr%d.last)"
                      " : sp_box_int(_t%d); })", tv3, tv3, tv3, tv3, tv3, tv3, tv3);
      }
      else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE &&
               nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode") &&
               ((nt_ref(nt, argv[0], "left") >= 0 && comp_ntype(c, nt_ref(nt, argv[0], "left")) == TY_FLOAT) ||
                (nt_ref(nt, argv[0], "right") >= 0 && comp_ntype(c, nt_ref(nt, argv[0], "right")) == TY_FLOAT))) {
        /* float bounds cannot ride sp_Range's int fields: compare as doubles,
           the clamped-to bound is the Float endpoint itself */
        int lo3 = nt_ref(nt, argv[0], "left"), hi3 = nt_ref(nt, argv[0], "right");
        int tv3 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = (%s);", tv3, r);
        buf_printf(b, " double _lo%d = ", tv3);
        if (lo3 >= 0) emit_float_expr(c, lo3, b); else buf_puts(b, "-HUGE_VAL");
        buf_printf(b, "; double _hi%d = ", tv3);
        if (hi3 >= 0) emit_float_expr(c, hi3, b); else buf_puts(b, "HUGE_VAL");
        buf_printf(b, "; ((double)_t%d < _lo%d) ? sp_box_float(_lo%d)"
                      " : ((double)_t%d > _hi%d) ? sp_box_float(_hi%d)"
                      " : sp_box_int(_t%d); })",
                   tv3, tv3, tv3, tv3, tv3, tv3, tv3);
      }
      else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* the helper raises on an exclusive range with a real end (CRuby) */
        buf_printf(b, "sp_int_clamp_range_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "digits") && argc == 0) buf_printf(b, "sp_int_digits(%s, 10)", r);
      else if (sp_streq(name, "digits") && argc == 1) { buf_printf(b, "sp_int_digits(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) &&
               argc == 1 && comp_ntype(c, argv[0]) == TY_BIGINT) {
        /* A Bignum mask exceeds int64, so an int receiver can never cover all
           its bits (allbits? is always false); anybits?/nobits? test the
           receiver against the mask's low 64 bits -- the only ones an int
           receiver can share (#2470). */
        if (sp_streq(name, "allbits?")) {
          buf_printf(b, "((void)(%s), (void)(", r); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        } else {
          buf_printf(b, "(((%s) & sp_bigint_to_int(", r); emit_expr(c, argv[0], b);
          buf_printf(b, ")) %s 0)", sp_streq(name, "anybits?") ? "!=" : "==");
        }
      }
      else if (sp_streq(name, "allbits?") && argc == 1) { int t = ++g_tmp; buf_printf(b, "({ mrb_int _t%d = ", t); emit_int_expr(c, argv[0], b); buf_printf(b, "; (((%s) & _t%d) == _t%d); })", r, t, t); }
      else if (sp_streq(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
      else if (sp_streq(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
      else if (sp_streq(name, "ceildiv") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_printf(b, "((mrb_int)ceil((double)(%s) / (", r); emit_expr(c, argv[0], b); buf_puts(b, ")))");  /* (#2425) */
      }
      else if (sp_streq(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "pow") && argc == 2) { buf_printf(b, "sp_powmod(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      /* pow with a literal negative exponent is the exact Rational
         1 / base**|exp| (matching **'s CRuby behavior) */
      else if (sp_streq(name, "pow") && argc == 1 &&
               nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
               nt_int(nt, argv[0], "value", 0) < 0) {
        long long pe9 = -(long long)nt_int(nt, argv[0], "value", 0);
        buf_printf(b, "sp_rational_new(1, sp_int_pow(%s, %lldLL))", r, pe9);
      }
      /* pow with a Float exponent is real exponentiation -> Float (#2604) */
      else if (sp_streq(name, "pow") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_printf(b, "pow((double)(%s), ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "pow") && argc == 1) { buf_printf(b, "sp_int_pow(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "pred") && argc == 0) buf_printf(b, "((%s) - 1)", r);
      else if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) buf_printf(b, "((%s) + 1)", r);
      else if (sp_streq(name, "to_s") && argc == 1) { buf_printf(b, "sp_int_to_s_base(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_BIGINT) {
          /* [big_arg, receiver promoted to Bignum] -- a poly pair (#2419) */
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ sp_Bigint *_t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                        " sp_PolyArray_push(_t%d, sp_box_bigint(_t%d));"
                        " sp_PolyArray_push(_t%d, sp_box_bigint(sp_bigint_new_int(%s))); _t%d; })",
                     o, o, o, ta, o, r, o);
        }
        else if (a0 == TY_FLOAT) {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (mrb_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        /* coerce against a Rational computes in floats: [Float(other), Float(self)] (#2606) */
        else if (a0 == TY_RATIONAL) {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (mrb_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        /* an Integer can't coerce with a Complex -> RangeError (#2606) */
        else if (a0 == TY_COMPLEX) {
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b);
          buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"can't convert Complex into Integer\"), (sp_FloatArray *)0))");
        }
        else {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new();"
                        " sp_IntArray_push(_t%d, _t%d);"
                        " sp_IntArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      /* Integer#eql?/equal?(x): value-equal only when x is itself Integer-typed
         (no numeric coercion -- 1.eql?(1.0) is false). For a fixnum receiver
         equal? is value identity, so it behaves the same as eql?. A Float or
         any other concrete arg is never equal; a poly arg checks its tag. */
      else if ((sp_streq(name, "eql?") || sp_streq(name, "equal?")) && argc == 1) {
        if (a0 == TY_INT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
        else if (a0 == TY_POLY) {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_INT && _t%d.v.i == (%s); })", te, te, r);
        }
        else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      }
      else handled = 0;
    }
    else { /* TY_FLOAT */
      /* round/ceil/floor/truncate(n>0) -> Float to n decimals; else Integer.
         A non-literal ndigits can't be classified statically; compute the exact
         value at runtime, typed Float (see infer_method_name_type / FLOAT-ROUNDING). */
      int ndig = 0;
      int nonlit = 0;
      /* round(half: :even/:down/:up): tie-break mode as a trailing keyword,
         with or without a digits argument. The keyword hash is peeled off
         the positional view. */
      const char *half_fn = NULL;
      int eff_argc = argc;
      if (sp_streq(name, "round") && argc >= 1 && nt_type(c->nt, argv[argc - 1]) &&
          sp_streq(nt_type(c->nt, argv[argc - 1]), "KeywordHashNode")) {
        int hv = kwh_lookup(nt, argv[argc - 1], "half");
        if (hv >= 0 && nt_type(c->nt, hv) && sp_streq(nt_type(c->nt, hv), "SymbolNode")) {
          const char *hm = nt_str(c->nt, hv, "value");
          if (hm && sp_streq(hm, "even")) half_fn = "sp_round_half_even";
          else if (hm && sp_streq(hm, "down")) half_fn = "sp_round_half_down";
          else half_fn = "round";  /* :up is the default rounding */
          eff_argc = argc - 1;
        }
      }
      if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
           sp_streq(name, "round") || sp_streq(name, "truncate")) && eff_argc == 1) {
        const char *aty = nt_type(c->nt, argv[0]);
        if (aty && sp_streq(aty, "IntegerNode")) ndig = (int)nt_int(c->nt, argv[0], "value", 0);
        else nonlit = 1;
      }
      const char *cfn = sp_streq(name, "floor") ? "floor" : sp_streq(name, "ceil") ? "ceil"
                      : sp_streq(name, "truncate") ? "trunc" : "round";
      if (half_fn) cfn = half_fn;
      if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
           sp_streq(name, "round") || sp_streq(name, "truncate"))) {
        if (nonlit) {
          /* The class depends on the runtime ndigits: Float when n > 0, Integer
             when n <= 0 (CRuby). Choose at runtime and return a boxed poly. */
          int tn = ++g_tmp, tv = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; double _t%d = (%s); (_t%d > 0)", tv, r, tn);
          buf_printf(b, " ? ({ double _f = pow(10, (double)_t%d); sp_box_float(isinf(_f) ? _t%d : %s(_t%d * _f) / _f); })", tn, tv, cfn, tv);
          buf_printf(b, " : ({ if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " double _f = pow(10, (double)(-_t%d)); sp_box_int(isinf(_f) ? 0 : (mrb_int)(%s(_t%d / _f) * _f)); }); })",
                     tv, tv, tv, tn, cfn, tv);
        }
        else if (ndig > 0)
          buf_printf(b, "({ double _f = pow(10, %d); %s((%s) * _f) / _f; })", ndig, cfn, r);
        else if (ndig < 0) {  /* round to a power of ten left of the decimal -> Integer */
          int tg = ++g_tmp;
          buf_printf(b, "({ double _t%d = (%s);"
                        " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " double _f = pow(10, %d); (mrb_int)(%s(_t%d / _f) * _f); })",
                     tg, r, tg, tg, tg, -ndig, cfn, tg);
        }
        else {
          int tg = ++g_tmp;
          buf_printf(b, "({ double _t%d = (%s);"
                        " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " (mrb_int)%s(_t%d); })",
                     tg, r, tg, tg, tg, cfn, tg);
        }
      }
      else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE &&
               nt_type(nt, unwrap_parens(c, argv[0])) && !sp_streq(nt_type(nt, unwrap_parens(c, argv[0])), "RangeNode")) {
        /* Float#clamp(float_range) held in a variable: clamp against sp_FloatRange
           (boxed, matching the literal path and TY_POLY inference). */
        int tf = ++g_tmp;
        buf_printf(b, "({ double _t%d = (%s); sp_FloatRange _fr%d = ", tf, r, tf); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_box_float(_t%d < _fr%d.first ? _fr%d.first : (_t%d > _fr%d.last ? _fr%d.last : _t%d)); })",
                   tf, tf, tf, tf, tf, tf, tf);
      }
      else if (sp_streq(name, "clamp") && argc == 1 &&
               (comp_ntype(c, argv[0]) == TY_RANGE || comp_ntype(c, argv[0]) == TY_FLOAT_RANGE)) {
        /* the clamped-to bound is the range's endpoint itself (keeping its
           own class); an in-range receiver stays the Float. A literal range
           with a Float bound cannot ride sp_Range (mrb_int bounds truncate
           it), so it clamps against typed endpoint temps directly. */
        int rn3 = unwrap_parens(c, argv[0]);
        int is_lit = rn3 >= 0 && nt_type(nt, rn3) && sp_streq(nt_type(nt, rn3), "RangeNode");
        int flo = is_lit ? nt_ref(nt, rn3, "left") : -1;
        int fhi = is_lit ? nt_ref(nt, rn3, "right") : -1;
        int any_f = is_lit && (comp_ntype(c, argv[0]) == TY_FLOAT_RANGE ||
                               (flo >= 0 && comp_ntype(c, flo) == TY_FLOAT) ||
                               (fhi >= 0 && comp_ntype(c, fhi) == TY_FLOAT));
        if (any_f) {
          int excl3 = (int)(nt_int(nt, rn3, "flags", 0) & 4) ? 1 : 0;
          int tf3 = ++g_tmp, tlo = -1, thi = -1;
          int lo_f = flo >= 0 && comp_ntype(c, flo) == TY_FLOAT;
          int hi_f = fhi >= 0 && comp_ntype(c, fhi) == TY_FLOAT;
          buf_printf(b, "({ double _t%d = (%s);", tf3, r);
          if (flo >= 0) {
            tlo = ++g_tmp;
            buf_printf(b, " %s _t%d = ", lo_f ? "double" : "mrb_int", tlo);
            emit_expr(c, flo, b); buf_puts(b, ";");
          }
          if (fhi >= 0) {
            thi = ++g_tmp;
            buf_printf(b, " %s _t%d = ", hi_f ? "double" : "mrb_int", thi);
            emit_expr(c, fhi, b); buf_puts(b, ";");
          }
          if (excl3 && fhi >= 0)
            buf_puts(b, " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");");
          buf_puts(b, " ");
          if (flo >= 0)
            buf_printf(b, "(_t%d < (double)_t%d) ? %s(_t%d) : ", tf3, tlo,
                       lo_f ? "sp_box_float" : "sp_box_int", tlo);
          if (fhi >= 0)
            buf_printf(b, "(_t%d > (double)_t%d) ? %s(_t%d) : ", tf3, thi,
                       hi_f ? "sp_box_float" : "sp_box_int", thi);
          buf_printf(b, "sp_box_float(_t%d); })", tf3);
        }
        else {
          int tf2 = ++g_tmp, trg2 = ++g_tmp;
          buf_printf(b, "({ double _t%d = (%s); sp_Range _t%d = ", tf2, r, trg2);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; if (_t%d.excl && _t%d.last != INTPTR_MAX)"
                        " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");"
                        " (_t%d.first != INTPTR_MIN && _t%d < (double)_t%d.first) ? sp_box_int(_t%d.first)"
                        " : (_t%d.last != INTPTR_MAX && _t%d > (double)_t%d.last) ? sp_box_int(_t%d.last)"
                        " : sp_box_float(_t%d); })",
                     trg2, trg2,
                     trg2, tf2, trg2, trg2,
                     trg2, tf2, trg2, trg2,
                     tf2);
        }
      }
      else if (sp_streq(name, "to_i"))  buf_printf(b, "sp_float_to_i_checked(%s)", r);
      else if (sp_streq(name, "to_f"))  buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "divmod") && argc == 1) {
        /* Float#divmod(n) -> [floor(x/n) (Integer), x - q*n (Float)] */
        int tx = ++g_tmp, tn = ++g_tmp, tq = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ mrb_float _t%d = (%s); mrb_float _t%d = ", tx, r, tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (isnan(_t%d) || isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " if (isinf(_t%d)) {"
                      /* an infinite divisor: same sign -> [0, x], opposite -> [-1, divisor] */
                      " if (_t%d == 0.0 || (_t%d > 0) == (_t%d > 0)) {"
                      " sp_PolyArray_push(_t%d, sp_box_int(0)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); }"
                      " else { sp_PolyArray_push(_t%d, sp_box_int(-1)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); } }"
                      " else {"
                      " mrb_int _t%d = (mrb_int)floor(_t%d / _t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float(_t%d - (mrb_float)_t%d * _t%d)); } _t%d; })",
                   tx, tn, tn,
                   o, o,
                   tn,
                   tx, tx, tn,
                   o, o, tx,
                   o, o, tn,
                   tq, tx, tn,
                   o, tq,
                   o, tx, tq, tn, o);
      }
      else if (sp_streq(name, "to_s"))    buf_printf(b, "sp_float_opt_to_s(%s)", r);
      else if (sp_streq(name, "inspect")) buf_printf(b, "sp_float_opt_inspect(%s)", r);
      else if (sp_streq(name, "to_r") && argc == 0) buf_printf(b, "sp_float_to_rational(%s)", r);
      else if (sp_streq(name, "rationalize") && argc == 0) buf_printf(b, "sp_float_rationalize0(%s)", r);
      else if (sp_streq(name, "rationalize") && argc == 1) {
        buf_printf(b, "sp_float_rationalize(%s, ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "abs"))   buf_printf(b, "fabs(%s)", r);
      /* Float arg/angle/phase: Integer 0 for >= 0, Float PI for < 0 -> poly (#2316) */
      else if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase"))
        buf_printf(b, "((%s) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0))", r);
      else if (sp_streq(name, "to_int")) buf_printf(b, "sp_float_to_i_checked(%s)", r);  /* alias of to_i (#2317); raises on Inf/NaN */
      else if (sp_streq(name, "zero?")) buf_printf(b, "((%s) == 0.0)", r);
      else if (sp_streq(name, "nan?"))  buf_printf(b, "(isnan(%s) != 0)", r);
      else if (sp_streq(name, "finite?")) buf_printf(b, "(isfinite(%s) != 0)", r);
      else if (sp_streq(name, "infinite?")) buf_printf(b, "(isinf(%s) ? ((%s) > 0 ? 1LL : -1LL) : SP_INT_NIL)", r, r);
      else if (sp_streq(name, "positive?")) buf_printf(b, "((%s) > 0)", r);
      else if (sp_streq(name, "negative?")) buf_printf(b, "((%s) < 0)", r);
      else if (sp_streq(name, "next_float")) buf_printf(b, "nextafter(%s, INFINITY)", r);
      else if (sp_streq(name, "prev_float")) buf_printf(b, "nextafter(%s, -INFINITY)", r);
      /* numerator/denominator of the exact rational value of the double
         (0.5.numerator == 1), via the frexp conversion behind Float#to_r. */
      else if (sp_streq(name, "numerator") && argc == 0) buf_printf(b, "sp_float_to_rational(%s).num", r);
      else if (sp_streq(name, "denominator") && argc == 0) buf_printf(b, "sp_float_to_rational(%s).den", r);
      else if (sp_streq(name, "magnitude")) buf_printf(b, "fabs(%s)", r);
      else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_fmod(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* Numeric query methods: a Float is never an Integer, is always real */
      else if (sp_streq(name, "integer?")) buf_printf(b, "((void)(%s), FALSE)", r);
      else if (sp_streq(name, "real?"))     buf_printf(b, "((void)(%s), TRUE)", r);
      else if (sp_streq(name, "nonzero?"))  buf_printf(b, "((%s) != 0.0 ? sp_box_float(%s) : sp_box_nil())", r, r);
      /* Float#div: integer floor-division; a zero divisor raises ZeroDivisionError,
         an infinite/NaN receiver raises FloatDomainError (Inf/NaN has no floor). */
      else if (sp_streq(name, "div") && argc == 1) {
        int tx = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ mrb_float _t%d = (%s); mrb_float _t%d = ", tx, r, tn);
        emit_float_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " (mrb_int)floor(_t%d / _t%d); })",
                   tn, tx, tx, tx, tx, tn);
      }
      /* Float#remainder: truncated remainder, sign following the dividend -- exactly
         C fmod (distinct from Ruby's floored % / modulo). */
      else if (sp_streq(name, "remainder") && argc == 1) {
        buf_printf(b, "fmod(%s, ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
      }
      /* Complex-view methods: a Float is a real Complex (imaginary part 0). */
      else if (sp_streq(name, "abs2"))               buf_printf(b, "((%s) * (%s))", r, r);
      else if (sp_streq(name, "real") || sp_streq(name, "conj") ||
               sp_streq(name, "conjugate"))          buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "imag") || sp_streq(name, "imaginary"))
        buf_printf(b, "((void)(%s), (mrb_int)0)", r);
      else if (sp_streq(name, "rect") || sp_streq(name, "rectangular")) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_float(%s));"
                      " sp_PolyArray_push(_t%d, sp_box_int(0)); _t%d; })", t, t, t, r, t, t);
      }
      else if (sp_streq(name, "polar")) {
        /* [magnitude, angle]: angle is Float PI when negative, else Integer 0 */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_float(fabs(%s)));"
                      " sp_PolyArray_push(_t%d, (%s) < 0 ? sp_box_float(3.141592653589793) : sp_box_int(0));"
                      " _t%d; })", t, t, t, r, t, r, t);
      }
      else if (sp_streq(name, "i"))  buf_printf(b, "((sp_Complex){0.0, (%s), 2})", r);
      /* a nil bound is an open side: clamp one-sided (or return the receiver),
         boxed so the chosen operand keeps its class (#2588) */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL)) {
        buf_printf(b, "sp_num_clamp_open(sp_box_float(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      /* Float#clamp with float bounds always yields a float (the returned bound
         is itself a float), so emit only when both bounds are float-typed; the
         mixed-bound case (int bound returned as Integer) is poly and left alone.
         Mirrors the inference condition in analyze_infer.c. */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               comp_ntype(c, argv[0]) == TY_FLOAT && comp_ntype(c, argv[1]) == TY_FLOAT) {
        buf_printf(b, "sp_float_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_FLOAT) &&
               (comp_ntype(c, argv[1]) == TY_INT || comp_ntype(c, argv[1]) == TY_FLOAT)) {
        /* mixed-class bounds: the applied bound keeps its own class, so the
           result is boxed (0.5.clamp(1, 3) is the Integer 1) */
        int lo_f2 = comp_ntype(c, argv[0]) == TY_FLOAT;
        int hi_f2 = comp_ntype(c, argv[1]) == TY_FLOAT;
        int tf4 = ++g_tmp, tlo2 = ++g_tmp, thi2 = ++g_tmp;
        buf_printf(b, "({ double _t%d = (%s); %s _t%d = ", tf4, r, lo_f2 ? "double" : "mrb_int", tlo2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; %s _t%d = ", hi_f2 ? "double" : "mrb_int", thi2);
        emit_expr(c, argv[1], b);
        buf_printf(b, "; if ((double)_t%d > (double)_t%d)"
                      " sp_raise_cls(\"ArgumentError\", \"min argument must be less than or equal to max argument\");"
                      " (_t%d < (double)_t%d) ? %s(_t%d)"
                      " : (_t%d > (double)_t%d) ? %s(_t%d)"
                      " : sp_box_float(_t%d); })",
                   tlo2, thi2,
                   tf4, tlo2, lo_f2 ? "sp_box_float" : "sp_box_int", tlo2,
                   tf4, thi2, hi_f2 ? "sp_box_float" : "sp_box_int", thi2,
                   tf4);
      }
      else if (sp_streq(name, "coerce") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        int ta = ++g_tmp, o = ++g_tmp;
        if (a0 == TY_RATIONAL) {
          buf_printf(b, "({ mrb_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else if (a0 == TY_COMPLEX) {
          /* a real-valued Complex coerces to its real part; an imaginary
             component can't become a Float (CRuby raises RangeError) */
          int tc9 = ++g_tmp;
          buf_printf(b, "({ sp_Complex _t%d = ", tc9); emit_expr(c, argv[0], b);
          buf_printf(b, "; if (_t%d.im != 0) sp_raise_cls(\"RangeError\", \"can't convert complex into Float\");"
                        " sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d.re);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", tc9, o, o, tc9, o, r, o);
        }
        else if (a0 == TY_INT) {
          buf_printf(b, "({ mrb_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
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
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((%s) / (", r); emit_float_expr(c, argv[0], b); buf_puts(b, "))"); }
      /* Float#eql?(x): true only when x is itself a Float of equal value (no
         numeric coercion, unlike ==). A float-typed arg compares directly; any
         other arg is boxed and rejected unless it is tagged float at runtime. */
      else if (sp_streq(name, "eql?") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_FLOAT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
        else {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_FLT && _t%d.v.f == (%s); })", te, te, r);
        }
      }
      /* Float#===(x) is #== -- numeric compare for a numeric arg, false for
         anything else (nil / Rational / Complex compare by value) (#2400) */
      else if (sp_streq(name, "===") && argc == 1) {
        TyKind a0q = comp_ntype(c, argv[0]);
        if (a0q == TY_FLOAT || a0q == TY_INT) {
          buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))");
        }
        else if (a0q == TY_RATIONAL) {
          int tq = ++g_tmp;
          buf_printf(b, "({ sp_Rational _t%d = ", tq); emit_expr(c, argv[0], b);
          buf_printf(b, "; ((double)_t%d.num / (double)_t%d.den) == (%s); })", tq, tq, r);
        }
        else if (a0q == TY_COMPLEX) {
          int tq = ++g_tmp;
          buf_printf(b, "({ sp_Complex _t%d = ", tq); emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.im == 0.0 && _t%d.re == (%s); })", tq, tq, r);
        }
        else if (a0q == TY_POLY) {
          int tq = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tq); emit_boxed(c, argv[0], b);
          buf_printf(b, "; sp_poly_eq(_t%d, sp_box_float(%s)); })", tq, r);
        }
        else {
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        }
      }
      /* Float#equal?: an unboxed double is an immediate value -- identity IS
         the value, exactly CRuby's flonum behavior (1.0.equal?(1.0) is true). */
      else if (sp_streq(name, "equal?") && argc == 1) {
        TyKind a0 = comp_ntype(c, argv[0]);
        if (a0 == TY_FLOAT) { buf_printf(b, "((%s) == (", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
        else {
          int te = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
          buf_printf(b, "; _t%d.tag == SP_TAG_FLT && _t%d.v.f == (%s); })", te, te, r);
        }
      }
      else handled = 0;
    }
    free(rs.p);
    if (handled) return 1;
  }
  return 0;
}

int emit_object_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind res = comp_ntype(c, id);
  /* Object#equal? -- reference identity. A heap instance IS its pointer, so
     this is a plain pointer comparison; a poly argument unwraps to tag +
     pointer; an argument of any other concrete type is never identical. A
     value-type instance is copied inline and has no stable identity, so only
     the reflexive same-lvalue read is knowably true (the string arm's rule). */
  /* Object#eql? default (no user override) is identity, exactly equal? --
     route it through the same arm (#2361) */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0 &&
      comp_method_in_chain(c, ty_object_class(rt), "eql?", NULL) < 0) {
    TyKind a0 = comp_ntype(c, argv[0]);
    if (!c->classes[ty_object_class(rt)].is_value_type) {
      if (a0 == rt) {
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
        emit_expr(c, argv[0], b); buf_puts(b, "))");
      }
      else if (a0 == TY_POLY) {
        int te = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = ", te); emit_boxed(c, argv[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_OBJ && _t%d.v.p == (void*)(", te, te);
        emit_expr(c, recv, b); buf_puts(b, "); })");
      }
      else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
      return 1;
    }
    if (same_sefree_lvalue(c, recv, argv[0])) { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 1)"); }
    else { buf_puts(b, "(("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
    return 1;
  }

  /* Object#freeze / #frozen? on a user instance: the frozen state lives in
     the object's GC header bit (shared with the container freeze paths).
     freeze sets it and returns self; frozen? reads it. Mutation of a frozen
     plain object's ivars is NOT trapped (raw C stores); the flag round-trip
     is what reflection-driven code observes. */
  if (recv >= 0 && ty_is_object(rt) && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "freeze") || sp_streq(name, "frozen?")) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0) {
    if (comp_ty_value_obj(c, rt)) {
      /* A value-type object (sp_X by value, no heap GC header to carry the
         frozen bit): freeze is a self-returning no-op and frozen? is false,
         matching the pre-stateful-freeze behavior for these unboxed classes. */
      if (sp_streq(name, "freeze")) { emit_expr(c, recv, b); return 1; }
      /* frozen? folds to constant-false, but the receiver may carry side
         effects (e.g. get_point().frozen?), so evaluate and discard it via a
         comma expression, mirroring the is_a?/instance_of? value-fold below. */
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
      return 1;
    }
    if (sp_streq(name, "freeze")) {
      buf_puts(b, "((");
      emit_ctype(c, rt, b);
      buf_puts(b, ")sp_gc_freeze("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    buf_puts(b, "sp_gc_is_frozen("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }

  /* obj.is_a?/kind_of?/instance_of?(Class): resolved via sp_class_le for
     correctness with module includes; falls back to constant for builtins. */
  if (recv >= 0 && ty_is_object(rt) && argc == 1 &&
      (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?")) &&
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0) {
    const char *cn = isa_const_name(nt, argv[0]);
    if (cn) {
      int cid = ty_object_class(rt);
      int target = comp_class_index(c, cn);
      /* an exception-subclass instance walks its carried cls_name chain --
         the class-index fold below answers 0 for builtin targets like
         StandardError, which have no user class index */
      if (class_is_exc_subclass(c, cid)) {
        if (sp_streq(name, "instance_of?")) {
          buf_puts(b, "(strcmp(((sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "))->cls_name, \"%s\") == 0)", cn);
        }
        else {
          buf_puts(b, "sp_exc_is_a((volatile sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "), \"%s\")", cn);
        }
        return 1;
      }
      if (target >= 0) {
        if (sp_streq(name, "instance_of?")) {
          /* a synthesized singleton subclass is instance_of? its parent
             (CRuby hides the singleton class) */
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), %d)", singleton_visible_ci(c, cid) == target);
        }
        else {
          /* use sp_class_le_mod (via macro) so includes chain is checked */
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_class_le(((sp_Class){%d}),((sp_Class){%d})))", cid, target);
        }
        return 1;
      }
      else {
        /* no user class index: universal ancestors still answer true for the
           hierarchy predicates (every object is_a? Object/BasicObject/Kernel);
           instance_of? stays exact and answers false */
        int uni = !sp_streq(name, "instance_of?") &&
                  (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject") ||
                   sp_streq(cn, "Kernel"));
        /* a builtin CLASS ancestor in the superclass chain (Data -146, Struct
           -145, Numeric ...): check the object's class against its cls_id so a
           Data/Struct instance is_a? Data/Struct (#2662). */
        int bid = builtin_class_id(cn);
        if (!uni && !sp_streq(name, "instance_of?") && bid < 0) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_class_le(((sp_Class){%d}),((sp_Class){%d})))", cid, bid);
          return 1;
        }
        buf_puts(b, "(("); emit_expr(c, recv, b); buf_printf(b, "), %d)", uni);
        return 1;
      }
    }
    /* Dynamic klass argument typed as TY_CLASS: runtime sp_class_le check */
    if (comp_ntype(c, argv[0]) == TY_CLASS) {
      int cid = ty_object_class(rt);
      int k = ++g_tmp;
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), ");
      buf_printf(b, "({ sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
      if (sp_streq(name, "instance_of?"))
        buf_printf(b, "((sp_Class){%d}).cls_id == _t%d.cls_id; }))", cid, k);
      else
        buf_printf(b, "sp_class_le(((sp_Class){%d}),_t%d); }))", cid, k);
      return 1;
    }
  }

  /* Comparable#clamp(lo, hi) on a user object: dispatch the user `<=>` through
     sp_obj_clamp. The result is self or the APPLIED BOUND, so it keeps the
     receiver's class only when both bounds are statically that class (the
     inference arm matches); otherwise it stays boxed. */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 2 &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    TyKind clo = comp_ntype(c, argv[0]), chi = comp_ntype(c, argv[1]);
    int same_cls = (clo == rt || clo == TY_NIL) && (chi == rt || chi == TY_NIL);
    /* a by-value receiver class unboxes by dereferencing the heap copy the
       boxing made (v.p is always a pointer); a ref class casts the pointer */
    if (same_cls && comp_ty_value_obj(c, rt))
      buf_printf(b, "(*(sp_%s *)", c->classes[ty_object_class(rt)].c_name);
    else if (same_cls) { buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")"); }
    buf_puts(b, "sp_obj_clamp(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_boxed(c, argv[0], b); buf_puts(b, ", ");
    emit_boxed(c, argv[1], b);
    buf_puts(b, ")");
    if (same_cls) buf_puts(b, ".v.p)");
    return 1;
  }
  /* Comparable#clamp(lo_obj..hi_obj) with same-class endpoints in a literal
     range: unfold to the two-argument object clamp (an sp_Range cannot carry
     the endpoints' class, so the range helper would compare raw pointers). */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 1 &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    int rn2 = unwrap_parens(c, argv[0]);
    if (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "RangeNode")) {
      int rlo = nt_ref(nt, rn2, "left"), rhi = nt_ref(nt, rn2, "right");
      /* a side is present unless it is absent (-1) or an explicit nil */
      int has_lo = rlo >= 0 && !(nt_type(nt, rlo) && sp_streq(nt_type(nt, rlo), "NilNode"));
      int has_hi = rhi >= 0 && !(nt_type(nt, rhi) && sp_streq(nt_type(nt, rhi), "NilNode"));
      int lo_obj = has_lo && comp_ntype(c, rlo) == rt;
      int hi_obj = has_hi && comp_ntype(c, rhi) == rt;
      /* At least one endpoint is the receiver's class and no present endpoint is
         a different type -- covers two-sided (`lo..hi`), beginless (`..hi`), and
         endless (`lo..`) object ranges. An sp_Range cannot carry the endpoints'
         class (its bounds are mrb_int), so unfold to sp_obj_clamp with a nil
         bound for the missing side; sp_obj_clamp skips a nil side. */
      if ((lo_obj || hi_obj) && (!has_lo || lo_obj) && (!has_hi || hi_obj)) {
        /* an exclusive range with a real end (`lo...hi`, `...hi`) cannot clamp
           -- CRuby raises ArgumentError regardless of whether the end would be
           applied (#2587). An endless `lo...` has no end, so it is fine.
           Evaluate the operands in order, then raise. */
        if (has_hi && (int)(nt_int(nt, rn2, "flags", 0) & 4)) {
          const char *ccn = c->classes[ty_object_class(rt)].c_name;
          buf_puts(b, "({ (void)("); emit_boxed(c, recv, b);
          if (has_lo) { buf_puts(b, "); (void)("); emit_boxed(c, rlo, b); }
          buf_puts(b, "); (void)("); emit_boxed(c, rhi, b);
          buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\"); ");
          /* dead default in the receiver's own C type (value vs pointer) */
          if (comp_ty_value_obj(c, rt)) buf_printf(b, "(sp_%s){0}; })", ccn);
          else buf_printf(b, "(sp_%s *)NULL; })", ccn);
          return 1;
        }
        if (comp_ty_value_obj(c, rt))
          buf_printf(b, "(*(sp_%s *)", c->classes[ty_object_class(rt)].c_name);
        else { buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")"); }
        buf_puts(b, "sp_obj_clamp(");
        emit_boxed(c, recv, b); buf_puts(b, ", ");
        if (lo_obj) emit_boxed(c, rlo, b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ", ");
        if (hi_obj) emit_boxed(c, rhi, b); else buf_puts(b, "sp_box_nil()");
        buf_puts(b, ").v.p)");
        return 1;
      }
    }
  }
  /* Comparable#clamp(range) on a user object: int endpoints become bounds fed
     to the user `<=>`; beginless/endless clamp one-sided; an exclusive range
     with a real end raises (CRuby). A clamped result IS the Integer endpoint
     itself, so the value stays boxed (inference: TY_POLY). */
  if (recv >= 0 && ty_is_object(rt) && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_RANGE &&
      comp_method_in_chain(c, ty_object_class(rt), "<=>", NULL) >= 0) {
    buf_puts(b, "sp_obj_clamp_range(");
    emit_boxed(c, recv, b); buf_puts(b, ", ");
    emit_expr(c, argv[0], b);
    buf_puts(b, ")");
    return 1;
  }

  /* Default Object#to_s / #inspect on a plain user object with no override:
     box and route through the poly renderers, which produce CRuby's
     "#<Name:0x...>" (inspect appends the ivar list via the registered
     per-class walker). A by-value class has no boxable pointer, so its
     renderer is emitted inline over a stack temp. */
  if (recv >= 0 && ty_is_object(rt) && !c->classes[ty_object_class(rt)].is_struct &&
      (sp_streq(name, "to_s") || sp_streq(name, "inspect")) && argc == 0 &&
      !obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect"))) {
    int cid2 = ty_object_class(rt);
    ClassInfo *ci2 = &c->classes[cid2];
    int want_ins = sp_streq(name, "inspect");
    if (ci2->is_value_type) {
      const char *rn2 = class_ruby_name(c, cid2);
      int tv2 = ++g_tmp;
      buf_printf(b, "({ sp_%s _t%d = ", ci2->c_name, tv2); emit_expr(c, recv, b);
      buf_printf(b, "; sp_sprintf(\"#<%s:0x%%016llx", rn2 ? rn2 : ci2->name);
      if (want_ins)
        for (int vi = 0; vi < ci2->nivars; vi++)
          buf_printf(b, "%s %s=%%s", vi ? "," : "", ci2->ivars[vi]);
      buf_printf(b, ">\", (unsigned long long)(uintptr_t)&_t%d", tv2);
      if (want_ins)
        for (int vi = 0; vi < ci2->nivars; vi++) {
          char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d.iv_%s", tv2, ci2->ivars[vi] + 1);
          buf_puts(b, ", sp_poly_inspect(");
          emit_boxed_text(c, ci2->ivar_types[vi], fb2, b);
          buf_puts(b, ")");
        }
      buf_puts(b, "); })");
      return 1;
    }
    buf_printf(b, "sp_poly_%s(", want_ins ? "inspect" : "to_s");
    emit_boxed(c, recv, b);
    buf_puts(b, ")");
    return 1;
  }

  /* Struct instance methods (to_h / to_a / values / members / dig). */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    /* #inspect / #to_s -> the generated (or user-overridden) struct/data stringifier */
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
      const char *cn = obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect"));
      if (cn) { buf_printf(b, "sp_%s_%s((sp_%s *)", cn, name, cn); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    }
    int is_to_a = (sp_streq(name, "to_a") || sp_streq(name, "values") || sp_streq(name, "deconstruct"));
    if (is_to_a && argc == 0) {
      int t = ++g_tmp; int rt2 = ++g_tmp;
      Buf rb = expr_buf(c, recv);
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
      return 1;
    }
    if (sp_streq(name, "to_h") && argc == 0) {
      int block = nt_ref(nt, id, "block");
      int t = ++g_tmp, rh = ++g_tmp;
      Buf rb = expr_buf(c, recv);
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
        if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
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
      return 1;
    }
    /* values_at(i, j, ... / range): member values by index, boxed */
    if (sp_streq(name, "values_at") && argc >= 1) {
      int tv4 = ++g_tmp, to4 = ++g_tmp;
      Buf rb4 = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->c_name, tv4, rb4.p ? rb4.p : "", to4, to4);
      free(rb4.p);
      int ok4 = 1;
      for (int a4 = 0; a4 < argc && ok4; a4++) {
        const char *aty4 = nt_type(nt, argv[a4]);
        if (aty4 && sp_streq(aty4, "IntegerNode")) {
          long long ix = nt_int(nt, argv[a4], "value", 0);
          if (ix < 0) ix += sc->nivars;
          if (ix < 0 || ix >= sc->nivars) { ok4 = 0; break; }
          char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, sc->ivars[(int)ix] + 1);
          buf_printf(b, " sp_PolyArray_push(_t%d, ", to4);
          emit_boxed_text(c, sc->ivar_types[(int)ix], fb4, b);
          buf_puts(b, ");");
        }
        else if (aty4 && sp_streq(aty4, "RangeNode")) {
          int rl4 = nt_ref(nt, argv[a4], "left"), rr4 = nt_ref(nt, argv[a4], "right");
          long long lo4 = rl4 >= 0 && nt_type(nt, rl4) && sp_streq(nt_type(nt, rl4), "IntegerNode")
                            ? nt_int(nt, rl4, "value", 0) : 0;
          long long hi4 = rr4 >= 0 && nt_type(nt, rr4) && sp_streq(nt_type(nt, rr4), "IntegerNode")
                            ? nt_int(nt, rr4, "value", 0) : sc->nivars - 1;
          if (nt_int(nt, argv[a4], "flags", 0) & 4) hi4--;
          if (lo4 < 0) lo4 += sc->nivars;
          if (hi4 < 0) hi4 += sc->nivars;
          for (long long ix = lo4; ix <= hi4 && ix < sc->nivars; ix++) {
            if (ix < 0) continue;
            char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, sc->ivars[(int)ix] + 1);
            buf_printf(b, " sp_PolyArray_push(_t%d, ", to4);
            emit_boxed_text(c, sc->ivar_types[(int)ix], fb4, b);
            buf_puts(b, ");");
          }
        }
        else ok4 = 0;
      }
      if (ok4) {
        buf_printf(b, " _t%d; })", to4);
        return 1;
      }
      /* non-literal keys: rebuild b is awkward -- fall through loudly */
    }
    /* #hash: combine the boxed member hashes so equal-valued structs agree */
    if (sp_streq(name, "hash") && argc == 0) {
      int tv5 = ++g_tmp, th5 = ++g_tmp;
      Buf rb5 = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; uint64_t _t%d = 1469598103934665603ULL;",
                 sc->c_name, tv5, rb5.p ? rb5.p : "", th5);
      free(rb5.p);
      for (int i5 = 0; i5 < sc->nivars; i5++) {
        char fb5[300]; snprintf(fb5, sizeof fb5, "_t%d->iv_%s", tv5, sc->ivars[i5] + 1);
        buf_printf(b, " _t%d = (_t%d ^ (uint64_t)sp_rbval_hash_key(", th5, th5);
        emit_boxed_text(c, sc->ivar_types[i5], fb5, b);
        buf_puts(b, ")) * 1099511628211ULL;");
      }
      buf_printf(b, " (mrb_int)(_t%d >> 1); })", th5);
      return 1;
    }
    if ((sp_streq(name, "size") || sp_streq(name, "length")) && argc == 0) {
      char szn[272]; snprintf(szn, sizeof szn, "@%s", name);
      if (comp_ivar_index(sc, szn) < 0) {
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "((void)(%s), %dLL)", rb.p ? rb.p : "0", sc->nivars);
        free(rb.p);
        return 1;
      }
    }
    /* deconstruct_keys([:a, :b]) / deconstruct_keys(nil): the requested
       members (all for nil) as a symbol-keyed hash. */
    if (sp_streq(name, "deconstruct_keys") && argc == 1) {
      int keyed[64]; int nkey = 0; int ok = 1;
      const char *aty = nt_type(nt, argv[0]);
      if (aty && sp_streq(aty, "NilNode")) {
        for (int i = 0; i < sc->nivars && nkey < 64; i++) keyed[nkey++] = i;
      }
      else if (aty && sp_streq(aty, "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, argv[0], "elements", &en);
        for (int e = 0; e < en && ok; e++) {
          const char *ety = nt_type(nt, els[e]);
          if (!ety || !sp_streq(ety, "SymbolNode")) { ok = 0; break; }
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, els[e], "value"));
          int mi2 = comp_ivar_index(sc, ivn);
          if (mi2 < 0 || nkey >= 64) { ok = 0; break; }
          keyed[nkey++] = mi2;
        }
      }
      else ok = 0;
      if (ok) {
        int t = ++g_tmp, rh = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);",
                   sc->c_name, t, rb.p ? rb.p : "", rh, rh);
        free(rb.p);
        for (int e = 0; e < nkey; e++) {
          int i = keyed[e];
          buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
          char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d->iv_%s", t, sc->ivars[i] + 1);
          emit_boxed_text(c, sc->ivar_types[i], fb2, b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", rh);
        return 1;
      }
    }
    if ((sp_streq(name, "members")) && argc == 0) {
      int rm = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", rm, rm);
      for (int i = 0; i < sc->nivars; i++)
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym((sp_sym)%d));", rm, comp_sym_intern(c, sc->ivars[i] + 1));
      buf_printf(b, " _t%d; })", rm);
      return 1;
    }
    if (sp_streq(name, "with") && sc->is_data) {
      /* Data#with copy-update: a new instance with the given members
         overridden, the rest copied from the receiver. Members are passed to
         the generated constructor in declaration order. */
      int wargs = nt_ref(nt, id, "arguments");
      int wargc = 0; const int *wargv = wargs >= 0 ? nt_arr(nt, wargs, "arguments", &wargc) : NULL;
      int wkwh = -1;
      if (wargv && wargc >= 1) {
        const char *lty = nt_type(nt, wargv[wargc - 1]);
        if (lty && sp_streq(lty, "KeywordHashNode")) wkwh = wargv[wargc - 1];
      }
      /* Data#with takes keyword arguments only; a positional argument (the only
         arg, or one alongside the keyword hash) is an ArgumentError in CRuby. */
      if (wargc > 0 && (wkwh < 0 || wargc > 1)) {
        unsupported(c, id, "Data#with with a positional argument (keywords only)");
        return 0;
      }
      if (wkwh >= 0) {
        int en = 0; const int *els = nt_arr(nt, wkwh, "elements", &en);
        for (int e = 0; e < en; e++) {
          int key = nt_ref(nt, els[e], "key");
          const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
          const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
          char ivn[256];
          if (kn) snprintf(ivn, sizeof ivn, "@%s", kn);
          /* an unknown member keyword is a runtime ArgumentError in CRuby (not a
             compile error): evaluate the receiver, then raise. (#2664) */
          if (!kn || comp_ivar_index(sc, ivn) < 0) {
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
            buf_printf(b, "); sp_raise_cls(\"ArgumentError\", \"unknown keyword: :%s\"); (sp_%s *)NULL; })",
                       kn ? kn : "?", sc->c_name);
            return 1;
          }
        }
      }
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_%s_new(", sc->c_name, t, rb.p ? rb.p : "", sc->c_name); free(rb.p);
      for (int i = 0; i < sc->nivars; i++) {
        if (i) buf_puts(b, ", ");
        int val = wkwh >= 0 ? kwh_lookup(nt, wkwh, sc->ivars[i] + 1) : -1;
        if (val >= 0) {
          TyKind mt = sc->ivar_types[i];
          TyKind vt = comp_ntype(c, val);
          if (mt == TY_POLY && vt != TY_POLY) {
            emit_boxed(c, val, b);  /* box a concrete value into a poly member */
          } else if (mt != TY_POLY && vt == TY_POLY) {
            /* A poly (sp_RbVal) value into a concrete member: coerce it, mirroring
               the poly-arg path in emit_arg_or_default. The regular `.new` call
               goes through that path; this hand-rolled constructor call did not,
               so it assigned an sp_RbVal straight into a const char* / mrb_int /
               sp_<T>* slot (a C type error). */
            const char *mtn = c_type_name(mt);
            if (mt == TY_STRING) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, val, b); buf_puts(b, ")"); }
            else if (mt == TY_FLOAT) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, val, b); buf_puts(b, ")"); }
            else if (mt == TY_SYMBOL) { buf_puts(b, "(sp_sym)sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
            else if (mt == TY_BOOL) { buf_puts(b, "sp_poly_truthy("); emit_expr(c, val, b); buf_puts(b, ")"); }
            else if (mt == TY_INT) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, val, b); buf_puts(b, ")"); }
            else if (ty_is_object(mt) || (mtn && mtn[0] && mtn[strlen(mtn) - 1] == '*')) {
              Buf ub = expr_buf(c, val);
              emit_unbox_text(c, mt, ub.p ? ub.p : "", b); free(ub.p);
            }
            else emit_expr(c, val, b);
          } else {
            emit_expr(c, val, b);
          }
        } else {
          buf_printf(b, "_t%d->iv_%s", t, sc->ivars[i] + 1);
        }
      }
      buf_puts(b, "); })");
      return 1;
    }
    if (sp_streq(name, "dig") && argc >= 1) {
      /* literal key resolves a member at compile time */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "SymbolNode")) {
        char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nt_str(nt, argv[0], "value"));
        mi = comp_ivar_index(sc, ivn);
      }
      else if (kty && sp_streq(kty, "IntegerNode")) {
        int v = (int)nt_int(nt, argv[0], "value", -1);
        if (v >= 0 && v < sc->nivars) mi = v;
      }
      if (mi >= 0) {
        /* nested struct members resolve the remaining literal keys at compile
           time: n.dig(:b, :c) walks member structs field by field */
        {
          char path[512]; path[0] = 0;
          ClassInfo *cur = sc; int cmi = mi; int di = 1; int all = 1;
          while (di < argc) {
            TyKind mt2 = cur->ivar_types[cmi];
            if (!ty_is_object(mt2) || !c->classes[ty_object_class(mt2)].is_struct) { all = 0; break; }
            ClassInfo *nx = &c->classes[ty_object_class(mt2)];
            const char *k2ty = nt_type(nt, argv[di]);
            int nmi = -1;
            if (k2ty && sp_streq(k2ty, "SymbolNode")) {
              char ivn2[256]; snprintf(ivn2, sizeof ivn2, "@%s", nt_str(nt, argv[di], "value"));
              nmi = comp_ivar_index(nx, ivn2);
            }
            else if (k2ty && sp_streq(k2ty, "IntegerNode")) {
              int v2 = (int)nt_int(nt, argv[di], "value", -1);
              if (v2 >= 0 && v2 < nx->nivars) nmi = v2;
            }
            if (nmi < 0) { all = 0; break; }
            size_t pl = strlen(path);
            snprintf(path + pl, sizeof path - pl, "->iv_%s", cur->ivars[cmi] + 1);
            cur = nx; cmi = nmi; di++;
          }
          if (all && di == argc && argc >= 2) {
            int t2 = ++g_tmp;
            Buf rb2 = expr_buf(c, recv);
            buf_printf(b, "({ sp_%s *_t%d = %s; _t%d%s->iv_%s; })",
                       sc->c_name, t2, rb2.p ? rb2.p : "", t2, path, cur->ivars[cmi] + 1);
            free(rb2.p);
            return 1;
          }
        }
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        char fld[300]; snprintf(fld, sizeof fld, "_t%d->iv_%s", t, sc->ivars[mi] + 1);
        TyKind mt = sc->ivar_types[mi];
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
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
        return 1;
      }
    }
    if (sp_streq(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int_literal]: return member value boxed to poly */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"))) {
        const char *kv = sp_streq(kty, "SymbolNode") ? nt_str(nt, argv[0], "value")
                                                     : nt_str(nt, argv[0], "content");
        if (kv) {
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kv);
          mi = comp_ivar_index(sc, ivn);
        }
      }
      else if (kty && sp_streq(kty, "IntegerNode")) {
        long long v = (long long)nt_int(nt, argv[0], "value", 0);
        if (v < 0) v += (long long)sc->nivars;
        if (v >= 0 && v < sc->nivars) mi = (int)v;
      }
      if (mi >= 0) {
        int t = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
        buf_printf(b, "_t%d->iv_%s; })", t, sc->ivars[mi] + 1);
        return 1;
      }
      /* general: generate chain of comparisons */
      if (sc->nivars > 0) {
        int t = ++g_tmp, tk = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->c_name, t, rb.p ? rb.p : "", tk);
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
        return 1;
      }
    }
  }

  /* object method call: sp_<DefClass>_<m>((sp_<DefClass>*)&recv, args) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    /* native (C-backed) class: dispatch a declared instance method to its C
       symbol, receiver first. `string?` returns are wrapped nil-safe. Overload
       selection is type-keyed (putc(65) vs putc("A")). */
    if (c->classes[cid].is_native_class) {
      TyKind natys[8];
      int nta = argc < 8 ? argc : 8;
      for (int a = 0; a < nta; a++) natys[a] = comp_ntype(c, argv[a]);
      int nm = comp_native_method_find_typed(c, cid, name, argc, 0, nta == argc ? natys : NULL);
      if (nm >= 0) {
        /* a :regexp arg binds only to a regex LITERAL at the call site (it
           compiles to the generated sp_re_pat_<n> pattern); anything else
           falls through to the generic paths. */
        NativeMethod *mre = &c->native_methods[nm];
        for (int ai = 0; ai < mre->nargs && ai < argc; ai++)
          if (sp_streq(mre->args[ai], "regexp") && re_lit_index(c, argv[ai]) < 0) { nm = -1; break; }
      }
      if (nm >= 0) {
        NativeMethod *m = &c->native_methods[nm];
        int wrap = sp_streq(m->ret, "string?");
        if (wrap) buf_puts(b, "sp_box_nullable_str(");
        buf_puts(b, m->csym); buf_puts(b, "("); emit_expr(c, recv, b);
        for (int ai = 0; ai < m->nargs && ai < argc; ai++) {
          buf_puts(b, ", ");
          if (sp_streq(m->args[ai], "any")) emit_boxed(c, argv[ai], b);
          else if (sp_streq(m->args[ai], "regexp"))
            buf_printf(b, "sp_re_pat_%d", re_lit_index(c, argv[ai]));
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
        if (wrap) buf_puts(b, ")");
        return 1;
      }
    }
    /* undef'd method: raise NoMethodError */
    if (comp_is_undeffed_in_chain(c, cid, name)) {
      TyKind ret_ty = comp_ntype(c, id);
      buf_printf(b, "(sp_raise_cls(\"NoMethodError\",\"undefined method '%s' for an instance of %s\"),%s)",
                 name, c->classes[cid].name,
                 ret_ty == TY_RANGE ? "(sp_Range){0}" : default_value(ret_ty));
      return 1;
    }
    /* instance_variable_get(:@x) / instance_variable_set(:@x, v) with a literal
       symbol or string name. A name present in the known layout lowers to a
       direct field read/write. An undefined-but-valid `@`-name reads as nil
       (get), matching CRuby; a name without a leading `@` raises NameError at
       runtime, also matching CRuby. A dynamic name -- or instance_variable_set
       to a valid name absent from the fixed object layout (no field to write) --
       cannot be represented and is diagnosed. */
    /* instance_variables: the class's ivar layout is static, so the list is
       a compile-time symbol array (the receiver evaluates for effect). */
    if (sp_streq(name, "instance_variables") && argc == 0 && ty_is_object(rt)) {
      int ivcid = ty_object_class(rt);
      if (ivcid >= 0 && ivcid < c->nclasses) {
        ClassInfo *ivc = &c->classes[ivcid];
        int tia = ++g_tmp;
        buf_printf(b, "({ (void)("); emit_expr(c, recv, b);
        buf_printf(b, "); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); ", tia, tia);
        /* Data/Struct members are NOT @-instance variables in CRuby (#2849) */
        if (!ivc->is_struct)
          for (int ji = 0; ji < ivc->nivars; ji++)
            buf_printf(b, "sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(\"%s\"))); ", tia, ivc->ivars[ji]);
        buf_printf(b, "_t%d; })", tia);
        return 1;
      }
    }
    if ((sp_streq(name, "instance_variable_get") || sp_streq(name, "instance_variable_set")) &&
        argc >= 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      int is_set = sp_streq(name, "instance_variable_set");
      /* Arity is statically known: get takes just the name, set the name and a
         value. A wrong count is a clear diagnostic rather than falling through to
         the misleading by-value-receiver message below. */
      if (is_set && argc != 2) { unsupported(c, id, "instance_variable_set takes exactly 2 arguments"); return 1; }
      if (!is_set && argc != 1) { unsupported(c, id, "instance_variable_get takes exactly 1 argument"); return 1; }
      int is_val = comp_ty_value_obj(c, rt);
      const char *rty = nt_type(nt, recv);
      int recv_lvalue = rty && (sp_streq(rty, "LocalVariableReadNode") ||
                                sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"));
      /* A name without a leading `@` is never a valid ivar name: raise NameError
         at runtime (evaluating the receiver first for its side effects). */
      if (!sym || sym[0] != '@') {
        if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), "); }
        else buf_puts(b, "(");
        buf_printf(b, "sp_raise_cls(\"NameError\", \"'%s' is not allowed as an instance variable name\"), sp_box_nil())",
                   sym ? sym : "");
        return 1;
      }
      int mi = -1;
      /* Data/Struct members live in the layout but are NOT @-instance
         variables in CRuby: a get answers nil, not the member (#2849) */
      if (!c->classes[cid].is_struct)
        for (int i = 0; i < c->classes[cid].nivars; i++)
          if (sp_streq(c->classes[cid].ivars[i], sym)) { mi = i; break; }
      if (mi >= 0) {
        /* A value object is passed by value, so a field write only sticks when
           the receiver is an lvalue (a local / ivar / self); a pointer object
           can be mutated through any reference. */
        if (is_set && is_val && !recv_lvalue) {
          unsupported(c, id, "instance_variable_set on a by-value object requires an lvalue receiver");
          return 1;
        }
        TyKind mt = c->classes[cid].ivar_types[mi];
        const char *acc = is_val ? "." : "->";
        if (is_set) {
          buf_puts(b, "(("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s = ", acc, sym + 1);
          if (mt == TY_POLY) emit_boxed(c, argv[1], b); else emit_expr(c, argv[1], b);
          buf_puts(b, ")");
        }
        else {
          buf_puts(b, "("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s", acc, sym + 1);
        }
        return 1;
      }
      /* A valid `@`-name not in the layout: get reads as nil (CRuby returns nil
         for an unset ivar); set has no field to write under the fixed layout. */
      if (is_set) {
        unsupported(c, id, "instance_variable_set to an ivar absent from the fixed object layout");
        return 1;
      }
      if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), sp_box_nil())"); }
      else buf_puts(b, "sp_box_nil()");
      return 1;
    }

    /* attr reader -> field access (recv).iv_x, UNLESS an explicit method of
       the same name overrides it at an equal-or-more-derived class. CRuby:
       attr_reader defines an ordinary method, so a subclass `def x` (or a
       same-class `def x`) overrides it via normal dispatch rather than
       reading the field. Whichever definition sits in the more-derived class
       wins; on a same-class tie the explicit method wins. */
    int rdc = -1, mdc = -1;
    if (comp_reader_in_chain(c, cid, name, &rdc)) {
      int reader_wins = 1;
      if (comp_method_in_chain(c, cid, name, &mdc) >= 0) {
        for (int k = cid; k >= 0; k = c->classes[k].parent) {
          if (k == mdc) { reader_wins = 0; break; }
          if (k == rdc) { reader_wins = 1; break; }
        }
      }
      if (reader_wins) {
        const char *rn2 = comp_resolve_alias(c, cid, name);
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", rn2);
        return 1;
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      /* a value-type receiver is passed by value; an ordinary object by
         pointer. For a value recv we hand emit_dispatch the value expression
         (lvalue or hoisted temp); the method takes `self` by value. */
      if (comp_ty_value_obj(c, rt)) {
        char selfv[64];
        const char *rty = nt_type(nt, recv);
        if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
          Buf rb = expr_buf(c, recv);
          snprintf(selfv, sizeof selfv, "%s", rb.p ? rb.p : ""); free(rb.p);
        }
        else {
          int t = ++g_tmp;
          Buf rb = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = ", t); buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
          snprintf(selfv, sizeof selfv, "_t%d", t);
        }
        emit_dispatch(c, cid, name, selfv, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return 1;
      }
      /* receiver is a pointer; reuse it directly if it's a simple lvalue,
         else stash in a temp (the virtual-dispatch switch references it
         multiple times) */
      char selfptr[64];
      const char *rty = nt_type(nt, recv);
      if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode"))) {
        Buf rb = expr_buf(c, recv);
        snprintf(selfptr, sizeof selfptr, "%s", rb.p ? rb.p : "");
        free(rb.p);
      }
      else {
        int t = ++g_tmp;
        /* emit the receiver first so any setup it pushes into g_pre is fully
           flushed before we write this temp's declaration line */
        Buf rb = expr_buf(c, recv);
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
      return 1;
    }
  }
  return 0;
}

int emit_value_recv_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* Time instance methods: sp_Time is a value -- splice the receiver once. */
  if (recv >= 0 && rt == TY_TIME) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    /* CRuby's #utc/#gmtime/#localtime mutate the receiver (and return it);
       the get* flavors copy. sp_Time is a value struct, so when the receiver
       is an LVALUE (a local, an ivar slot) the mutation is a write-back
       assignment -- `v.utc` then really updates v (#2637). A temporary
       receiver has nothing to observe afterwards, so the copy serves it. */
    int r_lval = nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "LocalVariableReadNode") ||
                                       sp_streq(nt_type(nt, recv), "InstanceVariableReadNode"));
    if ((sp_streq(name, "utc") || sp_streq(name, "gmtime")) && r_lval)
      buf_printf(b, "(%s = sp_time_utc(%s))", r, r);
    else if (sp_streq(name, "localtime") && argc == 0 && r_lval)
      buf_printf(b, "(%s = sp_time_localtime(%s))", r, r);
    else if (sp_streq(name, "utc") || sp_streq(name, "gmtime") || sp_streq(name, "getutc")) buf_printf(b, "sp_time_utc(%s)", r);
    else if (sp_streq(name, "localtime") || sp_streq(name, "getlocal")) buf_printf(b, "sp_time_localtime(%s)", r);
    else if (sp_streq(name, "year"))  buf_printf(b, "sp_time_year(%s)", r);
    else if (sp_streq(name, "mon") || sp_streq(name, "month")) buf_printf(b, "sp_time_mon(%s)", r);
    else if (sp_streq(name, "day") || sp_streq(name, "mday"))  buf_printf(b, "sp_time_mday(%s)", r);
    else if (sp_streq(name, "hour")) buf_printf(b, "sp_time_hour(%s)", r);
    else if (sp_streq(name, "min"))  buf_printf(b, "sp_time_min(%s)", r);
    else if (sp_streq(name, "sec"))  buf_printf(b, "sp_time_sec(%s)", r);
    else if (sp_streq(name, "wday")) buf_printf(b, "sp_time_wday(%s)", r);
    else if (sp_streq(name, "yday")) buf_printf(b, "sp_time_yday(%s)", r);
    else if (sp_streq(name, "to_i") || sp_streq(name, "tv_sec")) buf_printf(b, "(%s).tv_sec", r);
    else if (sp_streq(name, "to_f")) {
      /* hoist the receiver into a temp: emitting `r` twice would evaluate a
         side-effecting receiver (`c.utc`, which mutates the local) twice --
         unsequenced modification (#2865). */
      int tf = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = (%s); (mrb_float)_t%d.tv_sec + (mrb_float)_t%d.tv_nsec / 1e9; })", tf, r, tf, tf);
    }
    else if (sp_streq(name, "subsec")) {
      /* CRuby: Integer 0 for a whole second, else the exact Rational */
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; _t%d.tv_nsec == 0 ? sp_box_int(0) "
                    ": sp_box_rational(sp_rational_new((mrb_int)_t%d.tv_nsec, 1000000000)); })",
                 tt, r, tt, tt);
    }
    else if (sp_streq(name, "tv_usec") || sp_streq(name, "usec")) buf_printf(b, "((mrb_int)(%s).tv_nsec / 1000)", r);
    else if (sp_streq(name, "tv_nsec") || sp_streq(name, "nsec")) buf_printf(b, "((mrb_int)(%s).tv_nsec)", r);
    else if (sp_streq(name, "utc?") || sp_streq(name, "gmt?")) buf_printf(b, "((%s).is_utc == 1)", r);
    else if (sp_streq(name, "dst?") || sp_streq(name, "isdst")) buf_printf(b, "(sp_time_isdst(%s) != 0)", r);
    else if (sp_streq(name, "utc_offset") || sp_streq(name, "gmt_offset") || sp_streq(name, "gmtoff")) buf_printf(b, "sp_time_utc_offset(%s)", r);
    else if (sp_streq(name, "inspect")) buf_printf(b, "sp_time_inspect_v(%s)", r);
    else if (sp_streq(name, "to_s")) buf_printf(b, "sp_time_to_s_v(%s)", r);
    else if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) buf_printf(b, "sp_time_iso8601(%s)", r);
    else if (sp_streq(name, "zone")) buf_printf(b, "sp_time_zone(%s)", r);
    else if (sp_streq(name, "class")) buf_puts(b, "((sp_Class){(mrb_int)-1, \"Time\"})");
    else if (sp_streq(name, "getgm")) buf_printf(b, "sp_time_utc(%s)", r);  /* alias for getutc */
    else if (sp_streq(name, "xmlschema") && argc == 0) buf_printf(b, "sp_time_iso8601(%s)", r);
    else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round")) && argc == 0) {
      /* whole-second rounding of the subsecond part */
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; ", tt, r);
      if (sp_streq(name, "floor")) buf_printf(b, "_t%d.tv_nsec = 0;", tt);
      else if (sp_streq(name, "ceil")) buf_printf(b, "if (_t%d.tv_nsec > 0) { _t%d.tv_sec += 1; _t%d.tv_nsec = 0; }", tt, tt, tt);
      else buf_printf(b, "if (_t%d.tv_nsec >= 500000000) _t%d.tv_sec += 1; _t%d.tv_nsec = 0;", tt, tt, tt);
      buf_printf(b, " _t%d; })", tt);
    }
    else if (sp_streq(name, "sunday?"))    buf_printf(b, "(sp_time_wday(%s) == 0)", r);
    else if (sp_streq(name, "monday?"))    buf_printf(b, "(sp_time_wday(%s) == 1)", r);
    else if (sp_streq(name, "tuesday?"))   buf_printf(b, "(sp_time_wday(%s) == 2)", r);
    else if (sp_streq(name, "wednesday?")) buf_printf(b, "(sp_time_wday(%s) == 3)", r);
    else if (sp_streq(name, "thursday?"))  buf_printf(b, "(sp_time_wday(%s) == 4)", r);
    else if (sp_streq(name, "friday?"))    buf_printf(b, "(sp_time_wday(%s) == 5)", r);
    else if (sp_streq(name, "saturday?"))  buf_printf(b, "(sp_time_wday(%s) == 6)", r);
    /* asctime/ctime: the fixed C-style stamp, always in the receiver's own broken-down form */
    else if (sp_streq(name, "asctime") || sp_streq(name, "ctime"))
      buf_printf(b, "sp_time_strftime(%s, \"%%a %%b %%e %%H:%%M:%%S %%Y\")", r);
    else if (sp_streq(name, "eql?") && argc == 1) {
      if (comp_ntype(c, argv[0]) == TY_TIME) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, tu); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_time_cmp(_t%d, _t%d) == 0; })", tt, tu);
      } else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
    }
    else if (sp_streq(name, "to_a") && argc == 0) {
      /* [sec, min, hour, mday, mon, year, wday, yday, isdst, zone] */
      int tt = ++g_tmp, ta = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tt, r, ta, ta);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_sec(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_min(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_hour(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_mday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_mon(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_year(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_wday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int(sp_time_yday(_t%d)));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_bool(sp_time_isdst(_t%d) != 0));", ta, tt);
      buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_str(sp_time_zone(_t%d)));", ta, tt);
      buf_printf(b, " _t%d; })", ta);
    }
    else if (sp_streq(name, "to_r") && argc == 0) {
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_rational_new((mrb_int)_t%d.tv_sec * 1000000000 + _t%d.tv_nsec, 1000000000); })",
                 tt, r, tt, tt);
    }
    else if (sp_streq(name, "deconstruct_keys") && argc == 1) {
      /* a Hash of the requested keys (or all when the argument is nil). Each
         key's value carries its own boxing (int / bool / string / rational);
         `%d` in `vfmt` is the time temp id. */
      static const struct { const char *k, *vfmt; } TK[] = {
        {"year", "sp_box_int(sp_time_year(_t%d))"}, {"month", "sp_box_int(sp_time_mon(_t%d))"},
        {"mon", "sp_box_int(sp_time_mon(_t%d))"}, {"day", "sp_box_int(sp_time_mday(_t%d))"},
        {"mday", "sp_box_int(sp_time_mday(_t%d))"}, {"hour", "sp_box_int(sp_time_hour(_t%d))"},
        {"min", "sp_box_int(sp_time_min(_t%d))"}, {"sec", "sp_box_int(sp_time_sec(_t%d))"},
        {"wday", "sp_box_int(sp_time_wday(_t%d))"}, {"yday", "sp_box_int(sp_time_yday(_t%d))"},
        {"subsec", "(_t%d.tv_nsec == 0 ? sp_box_int(0) : sp_box_rational(sp_rational_new((mrb_int)_t%d.tv_nsec, 1000000000)))"},
        {"dst", "sp_box_bool(sp_time_isdst(_t%d) != 0)"},
        {"zone", "sp_box_str(sp_time_zone(_t%d))"}, {NULL, NULL} };
      /* which keys: a literal array selects them; nil (or non-literal) is all */
      int arr = argv[0];
      int is_arr = nt_type(nt, arr) && sp_streq(nt_type(nt, arr), "ArrayNode");
      int tt = ++g_tmp, th = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);", tt, r, th, th);
      /* runtime sp_sym_intern for the key: these symbols are synthesized during
         body emission, after the static sp_sym_names table is written, so a
         compile-time id would have no name at run time (rendered as "") (#2866). */
      #define SG_EMIT_KEY(K, VFMT) do { \
        buf_printf(b, " sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", th, (K)); \
        buf_printf(b, (VFMT), tt, tt); buf_puts(b, ");"); \
      } while (0)
      if (is_arr) {
        int en = 0; const int *els = nt_arr(nt, arr, "elements", &en);
        for (int e = 0; e < en; e++) {
          const char *ety = nt_type(nt, els[e]);
          const char *sk = (ety && sp_streq(ety, "SymbolNode")) ? nt_str(nt, els[e], "value") : NULL;
          if (!sk) continue;
          for (int t = 0; TK[t].k; t++)
            if (sp_streq(sk, TK[t].k)) { SG_EMIT_KEY(TK[t].k, TK[t].vfmt); break; }
        }
      } else {
        /* CRuby's full key set, in order */
        static const char *const allk[] = {"year","month","day","yday","wday","hour","min","sec","subsec","dst","zone",NULL};
        for (int a = 0; allk[a]; a++)
          for (int t = 0; TK[t].k; t++)
            if (sp_streq(allk[a], TK[t].k)) { SG_EMIT_KEY(TK[t].k, TK[t].vfmt); break; }
      }
      #undef SG_EMIT_KEY
      buf_printf(b, " sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH); })", th);
    }
    else if (sp_streq(name, "strftime") && argc == 1) { buf_printf(b, "sp_time_strftime(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "between?") && argc == 2) {
      int tt = ++g_tmp, ta = ++g_tmp, tb2 = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_Time _t%d = ", tb2); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) >= 0 && sp_time_cmp(_t%d, _t%d) <= 0; })", tt, ta, tt, tb2);
    }
    else if (sp_streq(name, "clamp") && argc == 2) {
      int tt = ++g_tmp, ta = ++g_tmp, tb2 = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; sp_Time _t%d = ", tt, r, ta); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_Time _t%d = ", tb2); emit_expr(c, argv[1], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) < 0 ? _t%d : (sp_time_cmp(_t%d, _t%d) > 0 ? _t%d : _t%d); })",
                 tt, ta, ta, tt, tb2, tb2, tt);
    }
    else if ((sp_streq(name, "+") || sp_streq(name, "-")) && argc == 1) {
      buf_printf(b, "sp_time_add(%s, %s(mrb_float)(", r, name[0] == '-' ? "-" : "");
      emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else if ((sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
              sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=")) && argc == 1 &&
             comp_ntype(c, argv[0]) == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_time_cmp(_t%d, _t%d) %s 0; })", tt, tu, name);
    }
    /* a relational comparison against a non-Time operand: CRuby's Comparable
       raises ArgumentError (its <=> returned nil). Evaluate the operand first. */
    else if ((sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
              sp_streq(name, ">=")) && argc == 1) {
      buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", \"comparison of Time with an incompatible value failed\"); 0; })");
    }
    else if (sp_streq(name, "<=>") && argc == 1 && comp_ntype(c, argv[0]) == TY_TIME) {
      int tt = ++g_tmp, tu = ++g_tmp;
      buf_puts(b, "({ sp_Time _t"); buf_printf(b, "%d = %s; sp_Time _t%d = ", tt, r, tu);
      emit_expr(c, argv[0], b);
      buf_printf(b, "; (mrb_int)sp_time_cmp(_t%d, _t%d); })", tt, tu);
    }
    /* Time <=> non-Time is nil (poly). A poly operand is checked at runtime. */
    else if (sp_streq(name, "<=>") && argc == 1) {
      TyKind a0t = comp_ntype(c, argv[0]);
      if (a0t == TY_POLY || a0t == TY_UNKNOWN) {
        int tt = ++g_tmp, tu = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_RbVal _t%d = ", tt, r, tu); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME) ? "
                      "sp_box_int(sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p)) : sp_box_nil(); })",
                   tu, tu, tt, tu);
      }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())"); }
    }
    else done = 0;
    free(rs.p);
    if (done) return 1;
  }

  /* StringScanner instance methods. String-returning methods may yield NULL
     (nil) on a miss; the NULL-aware string output operators render that. */
  /* StringScanner dispatch: native-bound (packages/strscan); no arms here. */
  /* MatchData instance methods (sp_MatchData *, nullable on no-match). */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    if (sp_streq(name, "[]") && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_RANGE ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")))) {
      /* md[range]: the groups over that index range (#2532) */
      int t = ++g_tmp;
      buf_printf(b, "({ sp_Range _t%d = ", t); emit_expr(c, argv[0], b);
      buf_printf(b, "; sp_MatchData_aref_range(%s, _t%d.first, _t%d.last, (int)_t%d.excl); })", r, t, t, t);
    }
    else if (sp_streq(name, "[]") && argc == 1) {
      /* A Symbol/String key selects a named capture group; an Integer key is a
         positional group (the existing path). */
      TyKind kt = comp_ntype(c, argv[0]);
      if (kt == TY_SYMBOL) { buf_printf(b, "sp_MatchData_aref_name(%s, sp_sym_to_s(", r); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else if (kt == TY_STRING) { buf_printf(b, "sp_MatchData_aref_name(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (kt == TY_POLY) {
        /* a poly key dispatches at runtime: a Symbol/String resolves by name,
           anything else is an index -- passing the raw sp_RbVal to
           sp_MatchData_aref (mrb_int) would be a C type error. */
        int mtmp = ++g_tmp, ktmp = ++g_tmp;
        buf_printf(b, "({ sp_MatchData *_t%d = %s; sp_RbVal _t%d = ", mtmp, r, ktmp);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_SYM ? sp_MatchData_aref_name(_t%d, sp_sym_to_s((sp_sym)_t%d.v.i)) :"
                      " _t%d.tag == SP_TAG_STR ? sp_MatchData_aref_name(_t%d, _t%d.v.s) :"
                      " sp_MatchData_aref(_t%d, sp_poly_to_i(_t%d)); })",
                   ktmp, mtmp, ktmp, ktmp, mtmp, ktmp, mtmp, ktmp);
      }
      else { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    }
    /* md[start, length]: an Array of `length` groups from `start` (#2507) */
    else if (sp_streq(name, "[]") && argc == 2) {
      buf_printf(b, "sp_MatchData_aref_len(%s, ", r); emit_int_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    }
    /* MatchData#== / #eql?: structural equality (#2529) */
    else if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_MATCHDATA) { buf_printf(b, "sp_MatchData_eq(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else { buf_printf(b, "((void)(%s), (void)(", r); emit_boxed(c, argv[0], b); buf_puts(b, "), 0)"); }
    }
    else if (sp_streq(name, "named_captures") && argc == 0) buf_printf(b, "sp_md_named_captures(%s)", r);
    /* named_captures(symbolize_names: true): symbol keys (#2530) */
    else if (sp_streq(name, "named_captures") && argc == 1) buf_printf(b, "sp_md_named_captures_sym(%s)", r);
    else if (sp_streq(name, "inspect") && argc == 0) buf_printf(b, "sp_MatchData_inspect(%s)", r);   /* #2500 */
    /* MatchData#match(n) is the group substring, #match_length(n) its byte
       length (nil when the group did not participate) (#2501) */
    else if (sp_streq(name, "match") && argc == 1) { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "match_length") && argc == 1) { buf_printf(b, "sp_MatchData_match_length(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    /* #deconstruct is the captures array; #deconstruct_keys the named captures
       as a symbol-keyed hash (#2503) */
    else if (sp_streq(name, "deconstruct") && argc == 0) buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (sp_streq(name, "deconstruct_keys") && argc == 1) buf_printf(b, "sp_md_named_captures_sym(%s)", r);
    else if (sp_streq(name, "regexp") && argc == 0) buf_printf(b, "((mrb_regexp_pattern *)(%s)->pat)", r);   /* #2499 */
    else if (sp_streq(name, "names") && argc == 0) buf_printf(b, "sp_MatchData_names(%s)", r);
    else if (sp_streq(name, "string") && argc == 0) buf_printf(b, "sp_MatchData_string(%s)", r);
    else if (sp_streq(name, "pre_match"))  buf_printf(b, "sp_MatchData_pre_match(%s)", r);
    else if (sp_streq(name, "post_match")) buf_printf(b, "sp_MatchData_post_match(%s)", r);
    else if (sp_streq(name, "to_s"))       buf_printf(b, "sp_MatchData_to_s(%s)", r);
    else if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0)
      buf_printf(b, "sp_MatchData_length(%s)", r);
    /* begin/end/offset/byte* accept a group NAME (String/Symbol) as well as an
       index; route those to the _name variant, which resolves the name like #[].
       A Symbol argument is passed as its interned string. */
    else if ((sp_streq(name, "begin") || sp_streq(name, "end") || sp_streq(name, "offset") ||
              sp_streq(name, "bytebegin") || sp_streq(name, "byteend") || sp_streq(name, "byteoffset")) &&
             argc == 1) {
      TyKind kt2 = comp_ntype(c, argv[0]);
      int by_name = (kt2 == TY_STRING || kt2 == TY_SYMBOL);
      buf_printf(b, "sp_MatchData_%s%s(%s, ", name, by_name ? "_name" : "", r);
      if (kt2 == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
    else if (sp_streq(name, "values_at") && argc >= 1) {
      /* values_at(i, ...) / values_at(:name, ...) -> a poly array of the
         selected groups (nil when a group did not participate). A Symbol/String
         argument resolves by name against this MatchData's own group table (like
         #[]); an integer argument is a group index. Routing a name through the
         index accessor would consult a wrong (first-seen) global name table. */
      int mt = ++g_tmp, at = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = %s; SP_GC_ROOT(_t%d); sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 mt, r, mt, at, at);
      for (int i = 0; i < argc; i++) {
        TyKind kt3 = comp_ntype(c, argv[i]);
        if (kt3 == TY_SYMBOL) {
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref_name(_t%d, sp_sym_to_s(", at, mt);
          emit_expr(c, argv[i], b); buf_puts(b, "))));");
        }
        else if (kt3 == TY_STRING) {
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref_name(_t%d, ", at, mt);
          emit_expr(c, argv[i], b); buf_puts(b, ")));");
        }
        else if (kt3 == TY_POLY) {
          /* a poly key dispatches at runtime like #[]: a Symbol/String resolves
             by name, anything else is an index. Passing the raw sp_RbVal to
             sp_MatchData_aref (mrb_int) would be a C type error. */
          int kt = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = ", kt); emit_expr(c, argv[i], b);
          buf_printf(b, "; sp_PolyArray_push(_t%d, sp_box_nullable_str("
                        "_t%d.tag == SP_TAG_SYM ? sp_MatchData_aref_name(_t%d, sp_sym_to_s((sp_sym)_t%d.v.i)) :"
                        " _t%d.tag == SP_TAG_STR ? sp_MatchData_aref_name(_t%d, _t%d.v.s) :"
                        " sp_MatchData_aref(_t%d, sp_poly_to_i(_t%d))));",
                     at, kt, mt, kt, kt, mt, kt, mt, kt);
        }
        else {
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref(_t%d, ", at, mt);
          emit_expr(c, argv[i], b); buf_puts(b, ")));");
        }
      }
      buf_printf(b, " _t%d; })", at);
    }
    else if (sp_streq(name, "captures"))  buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (sp_streq(name, "to_a"))      buf_printf(b, "sp_MatchData_to_a(%s)", r);
    else if (sp_streq(name, "nil?"))      buf_printf(b, "(%s == 0)", r);
    else unsupported(c, id, "MatchData method");
    free(rs.p);
    return 1;
  }

  /* StringIO instance methods (a non-GC heap buffer behind sp_StringIO *). */
  /* StringIO dispatch: native-bound (packages/stringio); no arms here. */
  return 0;
}

/* Emit the expression that materialises (range).step(k) as a typed array,
   returning its array TyKind. A float step -- or a literal range with float
   bounds -- yields a FloatArray; sp_Range stores mrb_int bounds, so a literal
   float-bounded range reads begin/end from the AST to keep the float values.
   Integer steps use the faithful int helper (step 0 raises ArgumentError, a
   negative step descends, an exclusive range drops the endpoint). Shared by the
   no-block materialisation and the block walk so both yield identical values. */
TyKind emit_range_step_array(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
  if (argc < 1) { buf_puts(b, "sp_IntArray_new()"); return TY_INT_ARRAY; }
  int rn = unwrap_parens(c, recv);
  int is_lit = rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode");
  int lo = is_lit ? nt_ref(nt, rn, "left") : -1;
  int hi = is_lit ? nt_ref(nt, rn, "right") : -1;
  int excl = (is_lit && (nt_int(nt, rn, "flags", 0) & 4)) ? 1 : 0;
  int is_float = comp_ntype(c, argv[0]) == TY_FLOAT ||
                 (lo >= 0 && comp_ntype(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && comp_ntype(c, hi) == TY_FLOAT);
  if (is_float && is_lit && lo >= 0 && hi >= 0) {
    buf_puts(b, "sp_FloatArray_from_step(");
    emit_float_expr(c, lo, b); buf_puts(b, ", ");
    emit_float_expr(c, hi, b); buf_puts(b, ", ");
    emit_float_expr(c, argv[0], b); buf_printf(b, ", %d)", excl);
    return TY_FLOAT_ARRAY;
  }
  int t = ++g_tmp;
  Buf rb = expr_buf(c, recv);
  if (is_float)
    buf_printf(b, "({ sp_Range _t%d = %s; sp_FloatArray_from_step((mrb_float)_t%d.first, (mrb_float)_t%d.last, ",
               t, rb.p ? rb.p : "", t, t);
  else
    buf_printf(b, "({ sp_Range _t%d = %s; sp_IntArray_from_range_step(_t%d.first, _t%d.last, ",
               t, rb.p ? rb.p : "", t, t);
  if (is_float) emit_float_expr(c, argv[0], b); else emit_int_expr(c, argv[0], b);
  buf_printf(b, ", _t%d.excl); })", t);
  free(rb.p);
  return is_float ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
}

int emit_range_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* Float range (1.0..3.0): a distinct sp_FloatRange receiver. It is not
     iterable, so its face is endpoint reads, membership tests, step, and
     bsearch (the fold handles bsearch; the iteration forms raise earlier). */
  if (recv >= 0 && rt == TY_FLOAT_RANGE) {
    int a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    int tr = ++g_tmp;
    /* begin/first, end/last, min, max (no arg) -> the float endpoints */
    if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "first") ||
                      sp_streq(name, "min"))) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.first; })", tr); return 1;
    }
    if (argc == 0 && (sp_streq(name, "end") || sp_streq(name, "last"))) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.last; })", tr); return 1;
    }
    if (argc == 0 && sp_streq(name, "max")) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_frange_max(_t%d); })", tr); return 1;
    }
    if ((sp_streq(name, "cover?") || sp_streq(name, "include?") ||
         sp_streq(name, "member?") || sp_streq(name, "===")) && argc == 1) {
      if (a0 == TY_INT || a0 == TY_FLOAT) {
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_puts(b, "; sp_frange_cover(_t"); buf_printf(b, "%d, ", tr);
        emit_float_expr(c, argv[0], b); buf_puts(b, "); })"); return 1;
      }
      if (a0 == TY_POLY) {
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _a%d = ", tr); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (mrb_bool)((_a%d.tag == SP_TAG_INT || _a%d.tag == SP_TAG_FLT) &&"
                      " sp_frange_cover(_t%d, sp_poly_to_f(_a%d))); })", tr, tr, tr, tr);
        return 1;
      }
      /* a non-numeric argument can never be covered: false (eval for effect) */
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if (sp_streq(name, "exclude_end?") && argc == 0) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; (mrb_bool)_t%d.excl; })", tr); return 1;
    }
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) {
      if (a0 == TY_FLOAT_RANGE) {
        int tr2 = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_FloatRange _t%d = ", tr2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_frange_eq(_t%d, _t%d); })", tr, tr2); return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if ((sp_streq(name, "to_s") || sp_streq(name, "inspect")) && argc == 0) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_frange_inspect(_t%d); })", tr); return 1;
    }
    if (sp_streq(name, "step") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_puts(b, "; sp_frange_step(_t"); buf_printf(b, "%d, ", tr);
      emit_float_expr(c, argv[0], b); buf_puts(b, "); })"); return 1;
    }
    if (sp_streq(name, "class") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), ((sp_Class){0, SPL(\"Range\")}))"); return 1;
    }
    if (sp_streq(name, "frozen?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_bool)1)"); return 1;
    }
    if (argc == 0 && (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
                      sp_streq(name, "dup") || sp_streq(name, "clone"))) {
      emit_expr(c, recv, b); return 1;
    }
    /* a Range value is never nil */
    if (sp_streq(name, "nil?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (mrb_bool)0)"); return 1;
    }
    /* is_a?/kind_of?/instance_of?/equal? via the boxed value's builtin identity
       (its class is "Range"; the helpers key on the SP_BUILTIN_FLOAT_RANGE tag) */
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                      sp_streq(name, "instance_of?"))) {
      int is_iof = sp_streq(name, "instance_of?");
      const char *cn = isa_const_name(nt, argv[0]);
      buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_boxed(c, recv, b); buf_puts(b, "; ");
      if (cn && is_iof) buf_printf(b, "(mrb_bool)(strcmp(sp_poly_class_name(_t%d), \"%s\") == 0); })", tr, cn);
      else if (cn)      buf_printf(b, "sp_poly_kind_of_builtin(_t%d, \"%s\"); })", tr, cn);
      else { buf_printf(b, "sp_poly_is_a_dyn(_t%d, ", tr); emit_boxed(c, argv[0], b);
             buf_printf(b, ", %d); })", is_iof ? 1 : 0); }
      return 1;
    }
    if (sp_streq(name, "equal?") && argc == 1) {
      if (a0 == TY_FLOAT_RANGE) {
        int tr2 = ++g_tmp;
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_FloatRange _t%d = ", tr2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_frange_eq(_t%d, _t%d); })", tr, tr2); return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), ((void)(");
      emit_expr(c, argv[0], b); buf_puts(b, "), (mrb_bool)0))"); return 1;
    }
    /* Every remaining enumerating form (each/map/to_a/sum/size/first(n)/...)
       raises "can't iterate from Float" like CRuby. The receiver still
       evaluates for its side effects; the value is the boxed nil the raise
       never actually returns (infer types these as poly). */
    {
      static const char *const iter[] = {
        "each", "map", "collect", "select", "filter", "reject", "to_a", "to_h",
        "entries", "find", "detect", "find_index", "count", "sum", "sort",
        "sort_by", "min_by", "max_by", "reduce", "inject", "each_with_index",
        "flat_map", "collect_concat", "any?", "all?", "none?", "one?", "take",
        "drop", "take_while", "drop_while", "filter_map", "partition",
        "group_by", "each_with_object", "tally", "find_all", "zip", "grep",
        "grep_v", "uniq", "reverse", "minmax", "join", "index", "size", "lazy",
        "each_cons", "each_slice", "chunk", "chunk_while", "cycle",
        "first", "last", NULL };
      for (int k = 0; iter[k]; k++) {
        if (sp_streq(name, iter[k])) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
          buf_puts(b, "); sp_raise_cls(\"TypeError\", \"can't iterate from Float\");"
                      " sp_box_nil(); })");
          return 1;
        }
      }
    }
  }
  /* range value methods (evaluate the range once into a temp) */
  if (recv >= 0 && rt == TY_RANGE) {
    int block = nt_ref(nt, id, "block");
    /* endless literal: size is infinite; take/first(n) count from the start
       (an endless range cannot materialize) */
    {
      int rn9 = unwrap_parens(c, recv);
      /* a local holding only such a literal counts too (sole-assignment);
         the arms below never evaluate the receiver, so skipping the local
         read loses no side effect */
      if (rn9 >= 0 && nt_type(nt, rn9) && !sp_streq(nt_type(nt, rn9), "RangeNode")) {
        int sl9 = local_sole_range_node(c, rn9);
        if (sl9 >= 0) rn9 = sl9;
      }
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") < 0 && sp_streq(name, "size") && argc == 0) {
        /* beginless: CRuby cannot iterate from nil */
        buf_puts(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from NilClass\"); (mrb_int)0; })");
        return 1;
      }
      /* a Float begin cannot iterate: the enumerating forms raise like
         CRuby (int begin + float end iterates fine; first/last/minmax read
         endpoints without iterating and stay served below) */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") >= 0 &&
          comp_ntype(c, nt_ref(nt, rn9, "left")) == TY_FLOAT &&
          ((argc == 0 && (sp_streq(name, "size") || sp_streq(name, "sum") ||
                          sp_streq(name, "count") || sp_streq(name, "to_a"))) ||
           (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last"))))) {
        const char *dflt9 = (sp_streq(name, "to_a") || argc == 1)
                              ? "(sp_IntArray*)0" : "(mrb_int)0";
        buf_printf(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from Float\"); %s; })", dflt9);
        return 1;
      }
      /* an int begin with a finite Float end sizes by the floored span */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          nt_ref(nt, rn9, "left") >= 0 && nt_ref(nt, rn9, "right") >= 0 &&
          comp_ntype(c, nt_ref(nt, rn9, "right")) == TY_FLOAT &&
          !lazy_endpoint_is_infinite(c, nt_ref(nt, rn9, "right")) &&
          sp_streq(name, "size") && argc == 0) {
        int excl9 = (int)(nt_int(nt, rn9, "flags", 0) & 4) ? 1 : 0;
        int tb9 = ++g_tmp, te9 = ++g_tmp;
        buf_printf(b, "({ mrb_int _t%d = ", tb9);
        emit_int_expr(c, nt_ref(nt, rn9, "left"), b);
        buf_printf(b, "; double _t%d = ", te9);
        emit_expr(c, nt_ref(nt, rn9, "right"), b);
        buf_printf(b, "; double _d = _t%d - (double)_t%d;"
                      " _d < 0 ? 0 : (%d && _t%d == floor(_t%d)) ? (mrb_int)_d : (mrb_int)floor(_d) + 1; })",
                   te9, tb9, excl9, te9, te9);
        return 1;
      }
      /* String-endpoint range accessors: the int-backed sp_Range stores the
         endpoint string POINTERS in its first/last fields, so begin/end/first/
         last/min/max must read them back as strings, not raw ints (#2467). */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode")) {
        int lo9 = nt_ref(nt, rn9, "left"), hi9 = nt_ref(nt, rn9, "right");
        if (lo9 >= 0 && hi9 >= 0 &&
            comp_ntype(c, lo9) == TY_STRING && comp_ntype(c, hi9) == TY_STRING) {
          int excl9 = (int)(nt_int(nt, rn9, "flags", 0) & 4) ? 1 : 0;
          if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "first") ||
                            sp_streq(name, "min"))) { emit_expr(c, lo9, b); return 1; }
          if (argc == 0 && (sp_streq(name, "end") || sp_streq(name, "last"))) {
            emit_expr(c, hi9, b); return 1;
          }
          if (argc == 0 && sp_streq(name, "max") && !excl9) { emit_expr(c, hi9, b); return 1; }
          if (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last"))) {
            int ta9 = ++g_tmp, tn9 = ++g_tmp;
            buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta9);
            emit_expr(c, lo9, b); buf_puts(b, ", "); emit_expr(c, hi9, b);
            buf_printf(b, ", %d); mrb_int _t%d = ", excl9, tn9); emit_int_expr(c, argv[0], b);
            if (sp_streq(name, "first"))
              buf_printf(b, "; sp_StrArray_slice(_t%d, 0, _t%d); })", ta9, tn9);
            else
              buf_printf(b, "; mrb_int _s9 = _t%d->len - _t%d; if (_s9 < 0) _s9 = 0;"
                            " sp_StrArray_slice(_t%d, _s9, _t%d); })", ta9, tn9, ta9, tn9);
            return 1;
          }
        }
      }
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          (nt_ref(nt, rn9, "right") < 0 ||
           lazy_endpoint_is_infinite(c, nt_ref(nt, rn9, "right"))) &&
          nt_ref(nt, rn9, "left") >= 0) {
        if (sp_streq(name, "size") && argc == 0) {
          buf_puts(b, "(HUGE_VAL)");
          return 1;
        }
        if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) {
          int lo9 = nt_ref(nt, rn9, "left");
          int ts9 = ++g_tmp, tn9 = ++g_tmp, ti9 = ++g_tmp, to9 = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", ts9); emit_int_expr(c, lo9, b);
          buf_printf(b, "; mrb_int _t%d = ", tn9); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);"
                        " for (mrb_int _t%d = 0; _t%d < _t%d; _t%d++)"
                        " sp_IntArray_push(_t%d, _t%d + _t%d); _t%d; })",
                     to9, to9, ti9, ti9, tn9, ti9, to9, ts9, ti9, to9);
          return 1;
        }
      }
    }
    if (sp_streq(name, "step") && argc == 1 && block < 0) {
      emit_range_step_array(c, id, b);
      return 1;
    }
    if (sp_streq(name, "each") && block < 0) {  /* external enumerator, or to_a materialize */
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      if (comp_ntype(c, id) == TY_ENUMERATOR) {
        /* pass the boxed range itself: sp_enum_items_from expands the members
           and #inspect keeps the range printable as the source */
        buf_printf(b, "sp_Enumerator_new_from(sp_box_range(%s))", rb.p ? rb.p : "");
      }
      else {
        buf_printf(b, "({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); })",
                   t, rb.p ? rb.p : "", t);
      }
      free(rb.p);
      return 1;
    }
    /* to_s / inspect render the range itself ("1..3"); a string-endpoint
       literal renders statically (int-backed sp_Range cannot). */
    if ((sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
        argc == 0 && nt_ref(nt, id, "block") < 0) {
      int rq = unwrap_parens(c, recv);
      if (rq >= 0 && nt_type(nt, rq) && !sp_streq(nt_type(nt, rq), "RangeNode")) {
        int sl = local_sole_range_node(c, rq);
        if (sl >= 0) rq = sl;
      }
      int lo_q = rq >= 0 && nt_type(nt, rq) && sp_streq(nt_type(nt, rq), "RangeNode")
                   ? nt_ref(nt, rq, "left") : -1;
      int hi_q = lo_q >= 0 ? nt_ref(nt, rq, "right") : -1;
      int str_ends = lo_q >= 0 && hi_q >= 0 &&
                     comp_ntype(c, lo_q) == TY_STRING && comp_ntype(c, hi_q) == TY_STRING;
      if (str_ends && nt_kind(nt, lo_q) == NK_StringNode && nt_kind(nt, hi_q) == NK_StringNode) {
        const char *lv2 = nt_str(nt, lo_q, "unescaped");
        if (!lv2) lv2 = nt_str(nt, lo_q, "content");
        const char *hv2 = nt_str(nt, hi_q, "unescaped");
        if (!hv2) hv2 = nt_str(nt, hi_q, "content");
        int plain = lv2 && hv2;
        for (const char *q2 = lv2; plain && q2 && *q2; q2++)
          if (!((*q2 >= 'a' && *q2 <= 'z') || (*q2 >= 'A' && *q2 <= 'Z') ||
                (*q2 >= '0' && *q2 <= '9') || *q2 == '_')) plain = 0;
        for (const char *q2 = hv2; plain && q2 && *q2; q2++)
          if (!((*q2 >= 'a' && *q2 <= 'z') || (*q2 >= 'A' && *q2 <= 'Z') ||
                (*q2 >= '0' && *q2 <= '9') || *q2 == '_')) plain = 0;
        if (plain) {
          int exq = (int)(nt_int(nt, rq, "flags", 0) & 4) ? 1 : 0;
          int quoted = sp_streq(name, "inspect");
          buf_puts(b, "SPL(\"");
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, lv2);
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, exq ? "..." : "..");
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, hv2);
          if (quoted) buf_puts(b, "\\\"");
          buf_puts(b, "\")");
          return 1;
        }
      }
      /* string endpoints without a static rendering: leave to other arms
         (the int-backed sp_Range cannot render them) */
      if (!str_ends) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tq);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_range_str(_t%d); })", tq);
        return 1;
      }
    }
    static const char *const rmeths[] = {
      "to_a", "entries", "include?", "member?", "cover?", "===", "sum", "min", "max",
      "first", "last", "size", "count", "begin", "end",
      "exclude_end?", "eql?", "equal?", "minmax", "overlap?", NULL };
    int known = 0;
    for (int i = 0; rmeths[i]; i++) if (sp_streq(name, rmeths[i])) known = 1;
    /* `count` with a block or argument is Enumerable#count, not Range#size:
       let it fall through to the int-array redispatch below. */
    if (sp_streq(name, "count") && (block >= 0 || argc >= 1)) known = 0;
    /* `sum` with a block is Enumerable#sum { }: the native Range sum ignored
       the block; let it fall through to the int-array redispatch below. */
    if (sp_streq(name, "sum") && block >= 0) known = 0;
    /* min(n)/max(n) return arrays of the smallest/largest n: Enumerable forms,
       served by the int-array redispatch below (the native arm is argless). */
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc >= 1) known = 0;
    /* min/max/minmax with a comparator block: the comparator emitter serves
       the lowerable shapes; anything else must reject rather than silently
       ignore the block. */
    if ((sp_streq(name, "min") || sp_streq(name, "max") ||
         sp_streq(name, "minmax")) && block >= 0) known = 0;
    if (known) {
      /* size/count on a string-literal range: no integer size -> nil, skip creating sp_Range */
      if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0) {
        int rn = unwrap_parens(c, recv);
        if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
          int lo = nt_ref(nt, rn, "left");
          if (lo >= 0 && comp_ntype(c, lo) == TY_STRING) {
            buf_puts(b, "SP_INT_NIL"); return 1;
          }
        }
      }
      int t = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Range _t%d = ", t);
      buf_puts(g_pre, rb.p ? rb.p : ""); buf_puts(g_pre, ";\n"); free(rb.p);
      if (sp_streq(name, "to_a") || sp_streq(name, "entries"))
        buf_printf(b, "sp_range_to_ia(_t%d)", t);
      else if (sp_streq(name, "include?") || sp_streq(name, "member?") ||
               sp_streq(name, "cover?") || sp_streq(name, "===")) {
        /* ===(range) / include?(range): CRuby compares endpoints against the
           value via <=>, and Integer <=> Range is nil, so these are always
           false. Only cover?(range) does endpoint containment. */
        if (argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE &&
            (sp_streq(name, "===") || sp_streq(name, "include?") ||
             sp_streq(name, "member?"))) {
          buf_puts(b, "0");
        }
        /* cover?(range) checks that both endpoints of the arg fit inside self */
        else if (sp_streq(name, "cover?") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
          int t2 = ++g_tmp;
          buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
          buf_printf(b, "; _t%d.first >= _t%d.first && (_t%d.last - _t%d.excl) <= (_t%d.last - _t%d.excl); })",
                     t2, t, t2, t2, t, t);
        }
        else {
          /* sp_range_include takes mrb_int; a float arg (`(1..).include?(2.4)`)
             needs an explicit cast, else clang -Werror flags the implicit
             float-literal->int conversion (gcc truncates silently). A poly arg
             (e.g. under --int-overflow=promote) is coerced with sp_poly_to_i. */
          TyKind at0 = comp_ntype(c, argv[0]);
          int arg_is_float = at0 == TY_FLOAT;
          int arg_is_poly = at0 == TY_POLY;
          buf_printf(b, "sp_range_include(&_t%d, ", t);
          if (arg_is_float) buf_puts(b, "(mrb_int)(");
          if (arg_is_poly) buf_puts(b, "sp_poly_to_i(");
          emit_expr(c, argv[0], b);
          if (arg_is_poly) buf_puts(b, ")");
          if (arg_is_float) buf_puts(b, ")");
          buf_puts(b, ")");
        }
      }
      else if (sp_streq(name, "min"))  /* smallest enumerated element (direction-aware) */
        buf_printf(b, "sp_range_min_v(_t%d)", t);
      else if (sp_streq(name, "first") || sp_streq(name, "begin")) {
        if (argc == 1) {
          /* first(n): the first n elements from `first`, walking by step. */
          int tf = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tc = ++g_tmp;
          buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); mrb_int _t%d = ", tf, tn);
          emit_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = sp_range_count(_t%d); mrb_int _t%d = sp_range_step(_t%d);"
                        " for (mrb_int _i%d = 0; _i%d < _t%d && _i%d < _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _t%d.first + _i%d * _t%d); _t%d; })",
                     tc, t, ti, t, tf, tf, tn, tf, tc, tf, tf, t, tf, ti, tf);
        }
        else buf_printf(b, "(_t%d.first)", t);
      }
      else if (sp_streq(name, "max"))  /* largest enumerated element (direction-aware) */
        buf_printf(b, "sp_range_max_v(_t%d)", t);
      else if (sp_streq(name, "end") && ({ int _rr = unwrap_parens(c, recv);
               nt_type(nt, _rr) && sp_streq(nt_type(nt, _rr), "RangeNode") &&
               nt_ref(nt, _rr, "right") < 0; })) {
        /* an ENDLESS literal range: #end is nil (#2413) */
        buf_puts(b, "sp_box_nil()"); (void)t;
      }
      else if (sp_streq(name, "last") || sp_streq(name, "end")) {
        if (argc == 1 && sp_streq(name, "last")) {
          /* last(n): collect up to n elements ending at last */
          int tf = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ mrb_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; mrb_int _t%d = _t%d.last - _t%d.excl;", te, t, t);
          buf_printf(b, " mrb_int _t%d = _t%d - _t%d + 1; if (_t%d < _t%d.first) _t%d = _t%d.first;", ts, te, tn, ts, t, ts, t);
          buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); for (mrb_int _i%d = _t%d; _i%d <= _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _i%d); _t%d; })",
                     tf, tf, ts, tf, te, tf, tf, tf, tf);
        }
        else buf_printf(b, "(_t%d.last)", t);
      }
      else if (sp_streq(name, "size") || sp_streq(name, "count"))
        buf_printf(b, "sp_range_count(_t%d)", t);
      else if (sp_streq(name, "sum") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "(("); emit_expr(c, argv[0], b);
        buf_printf(b, ") + (double)sp_IntArray_sum(sp_range_to_ia(_t%d), 0))", t);
      }
      else if (sp_streq(name, "sum") && argc == 1) {
        buf_printf(b, "sp_IntArray_sum(sp_range_to_ia(_t%d), ", t);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ")");
      }
      else if (sp_streq(name, "sum"))
        buf_printf(b, "sp_IntArray_sum(sp_range_to_ia(_t%d), 0)", t);
      else if (sp_streq(name, "exclude_end?"))
        buf_printf(b, "(_t%d.excl != 0)", t);
      else if (sp_streq(name, "eql?") || sp_streq(name, "equal?")) {
        /* the unboxed sp_Range has no object identity: equal? compares
           components, like the Complex/Rational value arms */
        buf_printf(b, "sp_range_eq(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "overlap?")) {
        int t2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", t2); emit_expr(c, argv[0], b);
        buf_printf(b, "; (_t%d.first <= _t%d.last - _t%d.excl && _t%d.first <= _t%d.last - _t%d.excl); })",
                   t, t2, t2, t2, t, t, t);
      }
      else if (sp_streq(name, "minmax")) {
        /* a poly pair: an empty (backwards) range yields [nil, nil] (#2412) */
        int ma = ++g_tmp, mv = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " mrb_int _t%d = sp_range_min_v(_t%d);"
                      " sp_PolyArray_push(_t%d, _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d));"
                      " _t%d = sp_range_max_v(_t%d);"
                      " sp_PolyArray_push(_t%d, _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d));"
                      " _t%d; })", ma, ma, mv, t, ma, mv, mv, mv, t, ma, mv, mv, ma);
      }
      return 1;
    }
  }
  /* Enumerable method on a Range that arrays support but Range does not handle
     natively (reduce(:sym), group_by, partition, flat_map, count(&block), ...):
     materialize the range into an int array once, then re-dispatch the call as
     an array by overriding the receiver's emission and type. Inference already
     typed the call as the array version (range_enum_redispatch). */
  /* A Hash Enumerable served by the pair-array redispatch (reduce/inject/
     each_with_index block forms): materialize the [k, v] pairs once and
     re-dispatch as a poly array, mirroring the range redispatch below. */
  if (recv >= 0 && ty_is_hash(rt) && hash_enum_redispatch(c, id) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_enum_items_from(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_POLY_ARRAY;
    /* find_all on the pair array is Enumerable select (a hash receiver only
       lands here through the redispatch, so the hash-returning Hash#select
       emitter is out of the picture) */
    const char *svn = nt_str(c->nt, id, "name");
    int fa = svn && sp_streq(svn, "find_all");
    /* the block form of each_with_index returns the RECEIVER hash in CRuby,
       not the pair array the redispatch iterates (#2417) */
    int ewi = svn && sp_streq(svn, "each_with_index") && nt_ref(c->nt, id, "block") >= 0;
    if (fa) nt_node_set_str((NodeTable *)c->nt, id, "name", "select");
    if (ewi) {
      Buf db; memset(&db, 0, sizeof db);
      emit_call(c, id, &db);
      buf_printf(b, "({ (void)(%s); ", db.p ? db.p : "0");
      free(db.p);
      c->ntype[recv] = sv;
      g_n_argov--;              /* re-emit the REAL receiver, not the override */
      emit_expr(c, recv, b);
      g_n_argov++;
      c->ntype[recv] = TY_POLY_ARRAY;
      buf_puts(b, "; })");
    }
    else emit_call(c, id, b);
    if (fa) nt_node_set_str((NodeTable *)c->nt, id, "name", "find_all");
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  if (recv >= 0 && rt == TY_RANGE && range_enum_redispatch(c, id) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp, tr = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_IntArray *_t%d = ({ sp_Range _t%d = %s; sp_range_to_ia(_t%d); }); SP_GC_ROOT(_t%d);\n",
               ta, tr, rb.p ? rb.p : "", tr, ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_INT_ARRAY;
    emit_call(c, id, b);
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  return 0;
}

/* If `recv` is an index expression `outer[oidx]` (a `[]` CallNode with a single
   int argument), set *outer/*oidx and return 1. Lets a `[]=`/splice on such a
   receiver write a promoted array back into outer's slot instead of dropping the
   write-back (which would lose a typed->poly promotion for a computed receiver). */
static int splice_recv_index_slot(Compiler *c, int recv, int *outer, int *oidx) {
  const NodeTable *nt = c->nt;
  const char *rty = nt_type(nt, recv);
  if (!rty || !sp_streq(rty, "CallNode")) return 0;
  const char *rn = nt_str(nt, recv, "name");
  if (!rn || !sp_streq(rn, "[]")) return 0;
  int ro = nt_ref(nt, recv, "receiver");
  if (ro < 0) return 0;
  int rargc; const int *rargv = call_args(nt, recv, &rargc);
  if (rargc != 1 || comp_ntype(c, rargv[0]) != TY_INT) return 0;
  *outer = ro; *oidx = rargv[0];
  return 1;
}

int emit_poly_call(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  /* Hash#compare_by_identity switches a hash to identity (equal?/object_id)
     key comparison. Spinel's hash machinery compares keys by value, so the
     mutator can't take effect; emitting it as a no-op would silently diverge
     (subsequent lookups behave as a value-keyed hash). Reject loudly instead.
     The `compare_by_identity?` predicate is left to report false, which is
     correct for any hash this mutator never (successfully) ran on. */
  if (sp_streq(name, "compare_by_identity"))  /* any arity: identity hashing is unsupported */
    unsupported(c, id, "Hash#compare_by_identity (identity-keyed hashing)");
  /* nil-aware conversions on a boxed receiver (a nil local widens to poly).
     The call's settled type may predate the widening (the receiver inferred
     TY_NIL on an early fixpoint pass and typed a captured local concretely);
     unbox the helper's boxed result to match it -- the receiver provably held
     nil there, so the payload really is the concrete kind. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "to_h") ||
       sp_streq(name, "to_r") || sp_streq(name, "rationalize") || sp_streq(name, "to_c"))) {
    int has_user = 0;
    for (int k = 0; k < c->nclasses && !has_user; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
    if (!has_user) {
      /* rationalize with no argument equals to_r for the values a poly nil/int
         can hold (nil -> (0/1), int -> (n/1)) (#2460). */
      if (sp_streq(name, "to_r") || sp_streq(name, "rationalize")) {
        buf_puts(b, "(*(sp_Rational *)sp_poly_to_r_m(");
        emit_expr(c, recv, b);
        buf_puts(b, ").v.p)");
      }
      else if (sp_streq(name, "to_c")) {
        buf_puts(b, "(*(sp_Complex *)sp_poly_to_c_m(");
        emit_expr(c, recv, b);
        buf_puts(b, ").v.p)");
      }
      else {
        /* to_h: the helper passes a real hash through unchanged, so guard
           the variant before unboxing (a non-sym-keyed hash rejects loudly
           rather than reading through the wrong layout) */
        int th2 = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = sp_poly_to_h_m(", th2);
        emit_expr(c, recv, b);
        buf_printf(b, "); if (_t%d.cls_id != SP_BUILTIN_SYM_POLY_HASH)"
                      " sp_raise_cls(\"TypeError\", \"to_h on a non-symbol-keyed boxed hash\");"
                      " (sp_SymPolyHash *)_t%d.v.p; })", th2, th2);
      }
      return 1;
    }
  }
  /* poly === arg is == (Object#===) when no user class overrides it */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "===")) {
    int has_user = 0;
    for (int k = 0; k < c->nclasses && !has_user; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
    if (!has_user) {
      buf_puts(b, "sp_poly_eq(");
      emit_expr(c, recv, b);
      buf_puts(b, ", ");
      emit_boxed(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
  }
  /* encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 && comp_ntype(c, recv) == TY_POLY) {
    const char *rty2 = nt_type(nt, recv);
    int is_enc = (rty2 && sp_streq(rty2, "SourceEncodingNode")) ||
                 (rty2 && sp_streq(rty2, "CallNode") &&
                  nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"));
    if (is_enc) { buf_puts(b, "sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
  }

  /* instance_variable_get(:@x) on a POLY receiver with a literal symbol or
     string name: dispatch the field read over every instantiated class that
     has the slot, boxing per the slot's declared type (the poly twin of the
     concrete lowering; see the matching inference rule in analyze_infer.c).
     A receiver whose runtime class lacks the slot reads as nil, matching
     CRuby's unset-ivar behavior; the SP_TAG_OBJ guard keeps a boxed scalar
     (cls_id 0) from aliasing the user class at index 0 (cf. issue #1576). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") &&
      argc == 1 && nt_ref(nt, id, "block") < 0 && nt_type(nt, argv[0]) &&
      (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
    const char *a0ty = nt_type(nt, argv[0]);
    const char *sym = sp_streq(a0ty, "SymbolNode")
                        ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
    if (sym && sym[0] == '@') {
      TyKind res = comp_ntype(c, id);
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv);
      emit_expr(c, recv, b);
      buf_printf(b, "; sp_RbVal _ivg%d = sp_box_nil(); if (_t%d.tag == SP_TAG_OBJ) switch (_t%d.cls_id) {",
                 tv, tv, tv);
      for (int k = 0; k < c->nclasses; k++) {
        if (!c->classes[k].instantiated) continue;
        int iv = comp_ivar_index(&c->classes[k], sym);
        if (iv < 0) continue;
        TyKind t = c->classes[k].ivar_types[iv];
        char fld[320];
        snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[k].c_name, tv, sym + 1);
        buf_printf(b, " case %d: _ivg%d = ", k, tv);
        emit_boxed_text(c, t, fld, b);
        buf_puts(b, "; break;");
      }
      buf_puts(b, " } ");
      if (res != TY_POLY && res != TY_UNKNOWN) {
        char ivn[24]; snprintf(ivn, sizeof ivn, "_ivg%d", tv);
        emit_unbox_text(c, res, ivn, b);
        buf_puts(b, "; })");
      }
      else buf_printf(b, "_ivg%d; })", tv);
      return 1;
    }
  }

  /* poly receiver `.to_i(base)`: only String#to_i takes a radix. When the value
     is a String at runtime, parse it (mirroring String#to_i(base)); any other
     type -- Integer/Float/nil -- has a zero-arity to_i, so CRuby raises
     ArgumentError. Guard on the tag rather than blindly sp_poly_to_s'ing, which
     would silently parse "42".to_i(16) => 66 instead of raising. The no-arg
     conversions live in the argc == 0 block below, which this form would skip.
     Receiver then argument are bound in that order to keep CRuby's evaluation
     order (both are evaluated before the call raises). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "to_i")) {
    int tr = ++g_tmp, tb = ++g_tmp;
    buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_expr(c, recv, b);
    buf_printf(b, "; mrb_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_str_to_i_base(_t%d.v.s, _t%d)"
                  " : (sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 1, expected 0)\"), (mrb_int)0); })",
               tr, tr, tb);
    return 1;
  }

  /* poly receiver count(v): value-equality element count over a boxed array */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "count") &&
      nt_ref(nt, id, "block") < 0) {
    int has_user_cnt = 0;
    for (int kk = 0; kk < c->nclasses && !has_user_cnt; kk++)
      if (comp_method_in_chain(c, kk, "count", NULL) >= 0 ||
          comp_reader_in_chain(c, kk, "count", NULL)) has_user_cnt = 1;
    if (!has_user_cnt) {
      buf_puts(b, "sp_poly_count_val("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }
  /* Numeric#round(ndigits) on a poly: the digit-taking form the no-arg
     numeric path cannot express. A user `round` still wins (poly dispatch). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "round") &&
      nt_ref(nt, id, "block") < 0) {
    int has_user_rnd = 0;
    for (int kk = 0; kk < c->nclasses && !has_user_rnd; kk++)
      if (comp_method_in_chain(c, kk, "round", NULL) >= 0 ||
          comp_reader_in_chain(c, kk, "round", NULL)) has_user_rnd = 1;
    if (!has_user_rnd) {
      buf_puts(b, "sp_poly_round_n("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }
  /* poly receiver: nil? / conversions / a few type-agnostic queries */
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    if (sp_streq(name, "nil?")) { buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    /* to_a on a runtime-tagged value: nil -> [], array -> itself, hash -> its
       pairs, anything else CRuby's NoMethodError. Skip when a user class
       defines to_a so its method wins the dispatch. */
    /* to_a / deconstruct on a poly value: for a Struct/Data both are the
       member values in order (sp_poly_to_a_arr derives them from the to_h
       hook); for an array/hash it is the elements/pairs. */
    if ((sp_streq(name, "to_a") || sp_streq(name, "deconstruct")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int has_user_ta = 0;
      for (int kk = 0; kk < c->nclasses && !has_user_ta; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user_ta = 1;
      if (!has_user_ta) {
        buf_puts(b, "sp_poly_to_a_arr("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
    }
    /* Struct#members on a Struct/Data read out of a container. */
    if (sp_streq(name, "members") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int has_user_m = 0;
      for (int kk = 0; kk < c->nclasses && !has_user_m; kk++)
        if (comp_method_in_chain(c, kk, "members", NULL) >= 0) has_user_m = 1;
      if (!has_user_m) {
        buf_puts(b, "sp_poly_struct_members("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
    }
    /* Hash#keys / #values on a poly value (e.g. an evidence-free empty `{}` that
       stayed poly). Skip when a user class defines keys/values so its method wins. */
    if (sp_streq(name, "keys") || sp_streq(name, "values")) {
      int has_user = 0;
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
      if (!has_user) {
        buf_printf(b, "sp_poly_%s(", name); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    if (sp_streq(name, "count")) {
      /* count / count(v) / count { |x| } on a boxed array (skip when any
         user class defines count -- same rule as length below) */
      int has_user_cnt = 0;
      for (int kk = 0; kk < c->nclasses && !has_user_cnt; kk++)
        if (comp_method_in_chain(c, kk, "count", NULL) >= 0 ||
            comp_reader_in_chain(c, kk, "count", NULL)) has_user_cnt = 1;
      int cblk = nt_ref(nt, id, "block");
      if (!has_user_cnt && argc == 0 && cblk >= 0) {
        int cbody = nt_ref(nt, cblk, "body");
        int cbn = 0; const int *cbb = cbody >= 0 ? nt_arr(nt, cbody, "body", &cbn) : NULL;
        const char *cp0 = block_param_name(c, cblk, 0);
        const char *cp0r = cp0 ? rename_local(cp0) : NULL;
        if (cbn >= 1) {
          int tr = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
          Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", tr, rb.p ? rb.p : "sp_box_nil()", tr);
          free(rb.p);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "mrb_int _t%d = 0;\n", tc);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (mrb_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {\n", ti, ti, tr, ti);
          if (cp0r) {
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "lv_%s = sp_poly_arr_get(_t%d, _t%d);\n", cp0r, tr, ti);
          }
          int svind = g_indent; g_indent++;
          for (int j = 0; j < cbn - 1; j++) emit_stmt(c, cbb[j], g_pre, g_indent);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "if (sp_poly_truthy(");
          emit_boxed(c, cbb[cbn - 1], g_pre);
          buf_printf(g_pre, ")) _t%d++;\n", tc);
          g_indent = svind;
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", tc);
          return 1;
        }
      }
      if (!has_user_cnt && argc == 0 && cblk < 0) {
        buf_puts(b, "sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
    }
    if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "empty?")) {
      /* has_user_len must also consult comp_reader_in_chain: a user class's
         `.size`/`.length` is very often an attr_reader/attr_accessor -- or a
         Struct member, which registers the same way -- rather than a `def`
         method. comp_method_in_chain alone missed those, so this branch took
         the built-in-only sp_poly_length() path and silently returned 0 for
         any object whose class exposes the name only as a reader (e.g. a
         `Struct.new(:offset, :size, :name)` entry answering `.size`). */
      int has_user_len = 0;
      const char *lcheck = (sp_streq(name, "empty?")) ? "length" : name;
      for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
        if (comp_method_in_chain(c, kk, lcheck, NULL) >= 0 ||
            comp_reader_in_chain(c, kk, lcheck, NULL)) has_user_len = 1;
      if (sp_streq(name, "empty?") && !has_user_len)
        for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
          if (comp_method_in_chain(c, kk, "empty?", NULL) >= 0) has_user_len = 1;
      if (!has_user_len) {
        if (sp_streq(name, "empty?")) {
          buf_puts(b, "(sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ") == 0)");
        }
        else {
          buf_puts(b, "sp_poly_length("); emit_expr(c, recv, b); buf_puts(b, ")");
        }
        return 1;
      }
    }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) {
      int has_user_method = 0;
      for (int k = 0; k < c->nclasses; k++)
        if (comp_method_in_chain(c, k, name, NULL) >= 0) { has_user_method = 1; break; }
      if (!has_user_method) {
        buf_printf(b, "%s(", sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect");
        emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    if (sp_streq(name, "to_i")) { buf_puts(b, "sp_poly_to_i("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    if (sp_streq(name, "to_f")) { buf_puts(b, "sp_poly_to_f("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    /* Complex#real / #imaginary on a poly value (a Complex read out of a
       container). A user class defining the same name wins via poly dispatch. */
    if ((sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag") ||
         sp_streq(name, "conjugate") || sp_streq(name, "conj")) && argc == 0) {
      int has_user = 0;
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0 || comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
      if (!has_user) {
        const char *pfn = sp_streq(name, "real") ? "sp_poly_real"
                        : (sp_streq(name, "imaginary") || sp_streq(name, "imag")) ? "sp_poly_imaginary"
                        : "sp_poly_conjugate";
        buf_printf(b, "%s(", pfn);
        emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    /* String#to_sym interns; Symbol#to_sym is identity; every other tag raises
       CRuby's NoMethodError. A user class defining to_sym wins via poly dispatch. */
    if (sp_streq(name, "to_sym")) {
      int has_user = 0;
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
      if (!has_user) {
        int t = ++g_tmp;
        /* Root the boxed receiver: sp_sym_intern reads through the String's
           data pointer and allocates, so a GC mid-intern could otherwise free
           an unrooted temporary String out from under it. */
        buf_printf(b, "({ sp_RbVal _t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); _t%d.tag == SP_TAG_STR ? sp_sym_intern(_t%d.v.s)"
                      " : (_t%d.tag == SP_TAG_SYM ? (sp_sym)_t%d.v.i"
                      " : (sp_raise_poly_nomethod(\"to_sym\", _t%d), (sp_sym)0)); })",
                   t, t, t, t, t, t);
        return 1;
      }
    }
    /* Numeric queries / rounding: dispatch on the runtime tag (a non-numeric
       tag raises CRuby's NoMethodError). A user method or attr reader with
       the same name wins -- the poly method dispatch handles it instead. */
    {
      const char *pfn =
        sp_streq(name, "nan?")      ? "sp_poly_nan_p" :
        sp_streq(name, "finite?")   ? "sp_poly_finite_p" :
        sp_streq(name, "infinite?") ? "sp_poly_infinite" :
        sp_streq(name, "zero?")     ? "sp_poly_zero_p" :
        sp_streq(name, "positive?") ? "sp_poly_positive_p" :
        sp_streq(name, "negative?") ? "sp_poly_negative_p" :
        sp_streq(name, "abs") || sp_streq(name, "magnitude") ? "sp_poly_abs" :
        sp_streq(name, "abs2")      ? "sp_poly_abs2" :
        sp_streq(name, "floor")     ? "sp_poly_floor" :
        sp_streq(name, "ceil")      ? "sp_poly_ceil" :
        sp_streq(name, "round")     ? "sp_poly_round" :
        sp_streq(name, "truncate")  ? "sp_poly_truncate" :
        sp_streq(name, "bytesize")  ? "sp_poly_bytesize" :
        sp_streq(name, "ord")       ? "sp_poly_ord" :
        sp_streq(name, "bit_length") ? "sp_poly_bit_length" :
        sp_streq(name, "numerator")   ? "sp_poly_numerator" :
        sp_streq(name, "denominator") ? "sp_poly_denominator" :
        sp_streq(name, "begin")       ? "sp_poly_range_begin" :
        sp_streq(name, "end")         ? "sp_poly_range_end" : NULL;
      if (pfn) {
        int has_user = 0;
        for (int kk = 0; kk < c->nclasses && !has_user; kk++)
          if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
              comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
        if (!has_user) {
          buf_printf(b, "%s(", pfn); emit_expr(c, recv, b); buf_puts(b, ")");
          return 1;
        }
      }
    }
    if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) {
      buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_succ)"); return 1;
    }
    if (sp_streq(name, "upcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_upcase)"); return 1; }
    if (sp_streq(name, "downcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_downcase)"); return 1; }
    if (sp_streq(name, "capitalize"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_capitalize)"); return 1; }
    if (sp_streq(name, "swapcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_swapcase)"); return 1; }
    if (sp_streq(name, "strip"))      { buf_puts(b, "sp_box_str(sp_str_strip(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "reverse"))    { buf_puts(b, "sp_poly_reverse("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    if (sp_streq(name, "chomp"))      { buf_puts(b, "sp_box_str(sp_str_chomp(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "chop"))       { buf_puts(b, "sp_box_str(sp_str_chop(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    if (sp_streq(name, "chr"))        { buf_puts(b, "sp_box_str(sp_str_chr(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, ")))"); return 1; }
    /* poly.bytes / poly.codepoints -> concrete TY_INT_ARRAY, no boxing (matches
       the inference rule). A String that widened to poly (a binary lump slice)
       reaches here; without this arm .bytes hit the generic poly method
       dispatch and raised "undefined method 'bytes' for poly". */
    if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "sp_str_%s(sp_poly_to_s(", sp_streq(name, "bytes") ? "bytes" : "codepoints");
      emit_expr(c, recv, b); buf_puts(b, "))"); return 1;
    }
    /* poly.chars -> TY_STR_ARRAY: a String read out of a container or
       destructured from a pair (`|a, b|`) reaches here poly-typed (#2909). */
    if (sp_streq(name, "chars") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int has_user = 0;
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, "chars", NULL) >= 0) has_user = 1;
      if (!has_user) {
        buf_puts(b, "sp_str_chars(sp_poly_to_s("); emit_expr(c, recv, b); buf_puts(b, "))"); return 1;
      }
    }
    if (sp_streq(name, "freeze"))     { buf_puts(b, "sp_poly_freeze("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
  }
  /* Data#with on a poly receiver (a Data read out of a container): build a
     symbol-keyed override hash from the keyword args, then dispatch by cls_id
     to a copy-update constructor (#2890). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "with") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    int has_user = 0;
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_method_in_chain(c, kk, "with", NULL) >= 0) has_user = 1;
    if (!has_user) {
      int en = 0; const int *els = nt_arr(nt, argv[0], "elements", &en);
      int th = ++g_tmp;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_SymPolyHash *_t%d = sp_SymPolyHash_new(); SP_GC_ROOT(_t%d);\n", th, th);
      for (int e = 0; e < en; e++) {
        int key = nt_ref(nt, els[e], "key");
        const char *kty = key >= 0 ? nt_type(nt, key) : NULL;
        const char *kn = (kty && sp_streq(kty, "SymbolNode")) ? nt_str(nt, key, "value") : NULL;
        int val = nt_ref(nt, els[e], "value");
        if (!kn || val < 0) continue;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", th, kn);
        emit_boxed(c, val, g_pre); buf_puts(g_pre, ");\n");
      }
      buf_puts(b, "sp_poly_with_m("); emit_expr(c, recv, b);
      buf_printf(b, ", sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH))", th);
      return 1;
    }
  }
  /* poly receiver: String#getbyte (a non-string tag raises NoMethodError).
     A user method or attr reader with the same name wins. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "getbyte")) {
    int has_user = 0;
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
          comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
    if (!has_user) {
      buf_puts(b, "sp_poly_getbyte("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }
  /* poly receiver: String#unpack1(fmt) -- unbox via sp_poly_to_s first. A
     String value that widened to poly (doom's binary WAD/texture parsing)
     was entirely unhandled here and hit the generic poly method dispatch,
     raising "undefined method 'unpack1' for poly". Mirrors the rt==TY_STRING
     codegen and its inference rule (a single-directive numeric format
     yields an unboxed int or float). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "unpack1") && argc == 1) {
    TyKind u1t = comp_ntype(c, id);
    if (u1t == TY_INT)        buf_puts(b, "sp_poly_to_i(");
    else if (u1t == TY_FLOAT) buf_puts(b, "sp_poly_to_f_opt(");
    buf_puts(b, "sp_PolyArray_get(sp_str_unpack(sp_poly_to_s(");
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
    if (u1t == TY_INT || u1t == TY_FLOAT) buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: arr[start, len] = src -- 3-arg splice assign
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 3 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    int tv = ++g_tmp;
    const char *rcvty = nt_type(nt, recv);
    int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                   sp_streq(rcvty, "InstanceVariableReadNode"));
    int outer, oidx;
    TyKind rty2 = comp_ntype(c, argv[2]);
    int tam = splice_to_ary_mi(c, rty2);
    buf_puts(b, "({ ");
    if (tam >= 0) {
      /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
      Buf call; memset(&call, 0, sizeof call);
      TyKind cty = emit_splice_to_ary_src(c, argv[2], rty2, tam, tv, b, &call);
      buf_printf(b, "sp_RbVal _t%d = ", tv);
      emit_boxed_text(c, cty, call.p ? call.p : "", b);
      buf_puts(b, "; ");
      free(call.p);
    }
    else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[2], b); buf_puts(b, "; "); }
    /* Store the possibly-promoted array back into the receiver so a typed->poly
       promotion survives: assign to a local/ivar lvalue, or write to outer's slot
       for a computed `outer[idx]` receiver; otherwise splice in place. */
    if (recv_is_lvalue) {
      emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
      buf_puts(b, "sp_poly_slot_splice("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    else {
      buf_puts(b, "sp_poly_splice("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    }
    if (tam >= 0)
      buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty2));
    else
      buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    return 1;
  }
  /* poly receiver: []= with symbol, string, int, or poly key -> runtime dispatch
     Skip Fiber/Fiber.current storage receivers (handled later). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]=") && argc == 2 &&
      !sp_is_fiber_storage_recv(nt, recv)) {
    /* arr[range] = rhs on a poly receiver: a splice over the range's span. */
    if (comp_ntype(c, argv[0]) == TY_RANGE) {
      int tv = ++g_tmp;
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      TyKind rty1 = comp_ntype(c, argv[1]);
      int tam = splice_to_ary_mi(c, rty1);
      buf_puts(b, "({ ");
      if (tam >= 0) {
        /* object RHS with to_ary: splice the coercion; the OBJECT is the value */
        Buf call; memset(&call, 0, sizeof call);
        TyKind cty = emit_splice_to_ary_src(c, argv[1], rty1, tam, tv, b, &call);
        buf_printf(b, "sp_RbVal _t%d = ", tv);
        emit_boxed_text(c, cty, call.p ? call.p : "", b);
        buf_puts(b, "; ");
        free(call.p);
      }
      else { buf_printf(b, "sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b); buf_puts(b, "; "); }
      if (recv_is_lvalue) {
        emit_expr(c, recv, b); buf_puts(b, " = sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_splice_range("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_splice_range("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b);
      }
      if (tam >= 0)
        buf_printf(b, ", _t%d); sp_box_obj(_tq%d, %d); })", tv, tv, ty_object_class(rty1));
      else
        buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
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
      /* widen_and_set returns a *different* boxed value when a typed array is
         promoted to a PolyArray (element-kind mismatch); otherwise it mutates in
         place. Store the result back so promotion survives: assign to a
         local/ivar lvalue, or write to outer's slot for a computed `outer[idx]`
         receiver; a receiver we cannot address falls back to in-place mutation. */
      const char *rcvty = nt_type(nt, recv);
      int recv_is_lvalue = rcvty && (sp_streq(rcvty, "LocalVariableReadNode") ||
                                     sp_streq(rcvty, "InstanceVariableReadNode"));
      int outer, oidx;
      if (recv_is_lvalue) {
        emit_expr(c, recv, b);
        buf_puts(b, " = sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else if (splice_recv_index_slot(c, recv, &outer, &oidx)) {
        buf_puts(b, "sp_poly_slot_set("); emit_boxed(c, outer, b); buf_puts(b, ", "); emit_int_expr(c, oidx, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
      else {
        buf_puts(b, "sp_poly_arr_widen_and_set("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
      }
    }
    else {
      buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    }
    buf_printf(b, ", _t%d); _t%d; })", tv, tv);
    (void)vt;
    return 1;
  }
  /* poly receiver: [] with symbol or string key -> runtime dispatch */
  /* poly receiver: arr[start, len] -> sp_poly_slice (string or typed array) */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 2) {
    /* The runtime dispatches on the receiver's tag: a string/array does a
       two-arg slice, a bound Method (optcarrot's poke handlers) is called with
       both int args. Both operands are raw integers. */
    buf_puts(b, "sp_poly_slice("); emit_expr(c, recv, b); buf_puts(b, ", ");
    emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "[]") && argc == 1) {
    /* `@table[i][j]` dispatch table narrowed to int (poly_double_index_int):
       call the entry (bound method / int array) for an unboxed int result. */
    if (comp_ntype(c, id) == TY_INT) {
      buf_puts(b, "sp_poly_index_int("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
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
        return 1;
      }
      if (at == TY_STRING) {
        buf_puts(b, "sp_poly_get_str("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (at == TY_INT) {
        buf_puts(b, "sp_poly_arr_get_hash("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (at == TY_POLY) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* a non-poly key (e.g. a Method): box it, then index polymorphically */
      if (at != TY_UNKNOWN) {
        buf_puts(b, "sp_poly_index_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* poly receiver: join */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join")) {
    buf_puts(b, "sp_poly_join("); emit_expr(c, recv, b);
    buf_puts(b, ", "); if (argc >= 1) emit_expr(c, argv[0], b); else buf_puts(b, "\"\"");
    buf_puts(b, ")"); return 1;
  }
  /* poly receiver: clamp(lo, hi) tag-dispatches int/float at runtime; the range
     form clamp(a..b) routes through the same helper with boxed bounds. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 2) {
    buf_puts(b, "sp_poly_clamp("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b);
    buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "clamp") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
    /* raises on an exclusive range with a real end; routes a user-object
       receiver through the `<=>` hook (sp_obj_clamp_range) */
    buf_puts(b, "sp_poly_clamp_range("); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: replace(other) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "replace") && argc == 1) {
    buf_puts(b, "sp_poly_replace("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: pack(fmt) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "pack") && argc == 1) {
    buf_puts(b, "sp_poly_pack("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: delete(chars) -> String#delete on the unboxed payload.
     Mirrors the analyzer's poly rule (result is a concrete TY_STRING): a
     string that widened to poly -- `data[offset, 8].delete("\x00")` stripping
     NUL padding off a fixed-width WAD name field in doom's texture parser.
     Like the analyzer rule, skipped when a user class defines `delete` (the
     per-class poly dispatch below then generates the proper arms). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "delete") && argc == 1 &&
      comp_ntype(c, id) == TY_STRING) {
    int has_user_delete = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (comp_method_in_chain(c, k, "delete", NULL) >= 0) { has_user_delete = 1; break; }
    if (!has_user_delete) {
      buf_puts(b, "sp_str_delete(sp_poly_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* poly receiver: gsub/sub with a regex literal -- extract the string
     payload (poly values reaching here are strings) and route to the
     engine, just like a TY_STRING receiver. */
  if (recv >= 0 && rt == TY_POLY && (sp_streq(name, "gsub") || sp_streq(name, "sub")) &&
      argc == 2 && re_lit_index(c, argv[0]) >= 0) {
    const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
    buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, sp_poly_to_s(", name, suf, re_lit_index(c, argv[0]));
    emit_expr(c, recv, b); buf_puts(b, "), ");
    emit_expr(c, argv[1], b); buf_puts(b, ")");
    return 1;
  }
  return 0;
}
