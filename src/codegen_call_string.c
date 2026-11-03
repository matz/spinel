/* codegen_call_string.c -- emit_call_body's arms of String and Regexp.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "repr.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "value_pos.h"
#include "codegen_call_arms.h"

/* a Regexp literal's match / match? / ===, the match family on a String, gsub, and String#% */
/* The text emit_boxed gives an Integer or a Float is most often one box
   call around the value, `FN(X)`. Where `text` has that shape its X is cut
   out in place and FN is answered; NULL for any other shape. A yield's
   Integer that is never nil is boxed by sp_box_int_nn. */
static const char *boxed_number_value(char *text, TyKind at, char **val) {
  const char *fns[3] = { ty_box_fn(at), ty_box_nil_fn(at), at == TY_INT ? "sp_box_int_nn" : NULL };
  size_t len = text ? strlen(text) : 0;
  for (int i = 0; i < 3; i++) {
    size_t n = fns[i] ? strlen(fns[i]) : 0, k = n;
    int depth = 0;
    if (!n || len < n + 2 || strncmp(text, fns[i], n) || text[n] != '(' || text[len - 1] != ')') continue;
    /* the call's own parenthesis has to be the one the text ends on */
    for (; k < len; k++) {
      char ch = text[k];
      if (ch == '"' || ch == '\'') { for (k++; k < len && text[k] != ch; k++) if (text[k] == '\\') k++; }
      else if (ch == '(') depth++;
      else if (ch == ')' && --depth == 0) break;
    }
    if (k != len - 1) continue;
    text[len - 1] = 0;
    *val = text + n + 1;
    return fns[i];
  }
  return NULL;
}

/* Does a Regexp matcher's operand convert at run time (sp_re_operand)? A
   String or nil goes in as it is, and an object whose class has a #to_str
   converts at compile time (emit_str_expr_nilable calls it); everything else
   -- a boxed value, a Symbol, an object with no #to_str, a number -- takes
   CRuby's reg_operand: a Symbol by its name, a #to_str found at run time, or
   the TypeError naming the operand's class. */
static int re_operand_at_runtime(Compiler *c, TyKind a0) {
  if (a0 == TY_STRING || a0 == TY_NIL) return 0;
  if (ty_is_object(a0) && comp_method_in_chain(c, ty_object_class(a0), "to_str", NULL) >= 0) return 0;
  return 1;
}
/* A matcher's operand as the `const char *` subject; `strict` is
   sp_re_operand's (all but === raise on a non-String). */
static void emit_re_subject(Compiler *c, int arg, TyKind a0, int strict, Buf *b) {
  if (!re_operand_at_runtime(c, a0)) { emit_str_expr_nilable(c, arg, b); return; }
  buf_puts(b, "sp_re_operand("); emit_boxed(c, arg, b); buf_printf(b, ", %d)", strict);
}
/* `fn(pat, subject, pos)`: a converted subject is held rooted while the
   position, which may allocate, is evaluated after it. */
static void emit_re_subject_at(Compiler *c, const char *fn, const char *pat, int arg, TyKind a0,
                               int pos, Buf *b) {
  if (!re_operand_at_runtime(c, a0)) {
    buf_printf(b, "%s(%s, ", fn, pat); emit_str_expr_nilable(c, arg, b);
    buf_puts(b, ", "); emit_int_expr(c, pos, b); buf_puts(b, ")");
    return;
  }
  int t = ++g_tmp;
  buf_printf(b, "({ const char *_t%d = ", t); emit_re_subject(c, arg, a0, 1, b);
  buf_printf(b, "; SP_GC_ROOT(_t%d); %s(%s, _t%d, ", t, fn, pat, t);
  emit_int_expr(c, pos, b); buf_puts(b, "); })");
}

/* The body of a match block, inside its hit arm with m bound: each
   statement's setup (an Array literal's pushes, an argument's temp) lands
   beside it rather than hoisted before the match, where it read m's nil;
   the last statement's value goes boxed into _t<tr>. */
static void emit_match_block_body(Compiler *c, const int *body, int n, int tr, Buf *b) {
  for (int j = 0; j < n; j++) {
    Buf spre; memset(&spre, 0, sizeof spre);
    Buf sval; memset(&sval, 0, sizeof sval);
    Buf *saved_pre = g_pre; g_pre = &spre;
    if (j + 1 < n) emit_stmt(c, body[j], &sval, 0);
    else {
      buf_printf(&sval, "_t%d = ", tr);
      emit_boxed(c, body[j], &sval);
      buf_puts(&sval, "; ");
    }
    g_pre = saved_pre;
    if (spre.p) buf_puts(b, spre.p);
    if (sval.p) buf_puts(b, sval.p);
    free(spre.p); free(sval.p);
  }
}

/* Bind the match block's parameter to the MatchData the hit arm holds in
   `_t<tm>`. The parameter's slot is the handle's own type when the arms here
   are its only writers; a `&:captures` block that a program class's `match`
   (one taking `&block`) also receives as a proc has a boxed slot, the proc's
   parameter being poly, and that slot takes the handle boxed: assigned the
   raw pointer, the C did not compile. */
static void emit_match_param_bind(Compiler *c, int mblk, int tm, Buf *b) {
  const char *mp0 = block_param_name(c, mblk, 0);
  if (!mp0) return;
  Scope *bs = comp_scope_of(c, mblk);
  LocalVar *lv = bs ? scope_local(bs, mp0) : NULL;
  char h[32]; snprintf(h, sizeof h, "_t%d", tm);
  buf_printf(b, "lv_%s = ", rename_local(mp0));
  if (lv && repr_of_slot(c, lv).kind == RK_BOXED) emit_boxed_text(c, TY_MATCHDATA, h, b);
  else buf_puts(b, h);
  buf_puts(b, "; ");
}

/* A `match` the String arms answer: its MatchData (or NULL), boxed where
   the call itself is typed boxed (a receiver known only at run time, whose
   slot took the box), as the value's other readers take it. */
static void emit_matchdata_as_call(Compiler *c, int id, const char *md, Buf *b) {
  if (repr_of(c, id).kind == RK_BOXED) emit_boxed_text(c, TY_MATCHDATA, md ? md : "NULL", b);
  else buf_puts(b, md ? md : "NULL");
}

int emit_call_regexp_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0) {
  /* regex literal match predicates (bool-returning, no MatchData/globals):
     /re/.match?(str[, pos])  and  str.match?(/re/[, pos]) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && sp_streq(name, "match?") && argc == 1) {
      /* /re/.match?(str): a match boolean that leaves $~ alone (CRuby) */
      buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", rre); emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ")");  /* nil subject: no match */
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "===") && argc == 1) {
      /* /re/ === x updates the $~ registers, unlike match? */
      if (a0 == TY_STRING) { buf_printf(b, "(sp_re_match(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ") >= 0)"); }
      else { buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, ", rre); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "match?") && argc == 2) {
      /* the subject converts as match(str, pos)'s does: a boxed or nil one
         went into the const char * slot raw */
      char pn[32]; snprintf(pn, sizeof pn, "sp_re_pat_%d", rre);
      emit_re_subject_at(c, "sp_re_match_p_at", pn, argv[0], a0, argv[1], b);
      return 1;
    }
    /* /re/ =~ str -> match offset or nil (poly) */
    if (rre >= 0 && sp_streq(name, "=~") && argc == 1 && a0 == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* ~ /re/ -> `/re/ =~ $_`: the match offset in the last-read line, or nil. */
    if (rre >= 0 && sp_streq(name, "~") && argc == 0) {
      const char *urn = comp_resolve_gvar(c, "_");
      LocalVar *ugv = urn ? comp_gvar(c, urn) : NULL;
      if (ugv) {
        if (ugv->type == TY_STRING) buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, gv_%s)", rre, urn);
        /* (one representation: a String global held as the handle) */
        else if (ugv->type == TY_STRBUF) buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, SP_STR_B(gv_%s))", rre, urn);
        else buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, sp_poly_to_s(gv_%s))", rre, urn);
      }
      else {
        buf_puts(b, "sp_box_nil()");  /* $_ unset: no line to match against */
      }
      return 1;
    }
    /* /re/.source and /re/.options are compile-time constants of the literal.
       The source/flags come from the RESOLVED literal's registration slot
       (g_re_src) -- re_lit_index also resolves variables and constants, whose
       own nodes carry no "unescaped" (a variable read fell to an empty
       string, #2018). */
    if (rre >= 0 && sp_streq(name, "source") && argc == 0) {
      emit_str_literal(b, g_re_src[rre]); return 1;
    }
    if (rre >= 0 && sp_streq(name, "options") && argc == 0) {
      int pf = g_re_flg[rre];
      int opt = ((pf & 1) ? 1 : 0) | ((pf & 8) ? 2 : 0) | ((pf & 4) ? 4 : 0) |
                re_lit_enc_opts(c, recv, rre);
      buf_printf(b, "%d", opt); return 1;
    }
    /* //u fixes UTF-8 even on an ASCII source; //n keeps an ASCII source
       US-ASCII and makes any other ASCII-8BIT */
    if (rre >= 0 && sp_streq(name, "encoding") && argc == 0) {
      int eo = re_lit_enc_opts(c, recv, rre);
      buf_printf(b, "sp_box_encoding(%s)",
                 (eo & 32) ? ((eo & 16) ? "sp_encoding_binary()" : "sp_encoding_us_ascii()")
                 : (eo & 16) ? "sp_encoding_utf8()" : "sp_encoding_us_ascii()");
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "fixed_encoding?") && argc == 0) {
      buf_puts(b, (re_lit_enc_opts(c, recv, rre) & 16) ? "TRUE" : "FALSE");
      return 1;
    }
  }
  /* Regexp VALUE receiver (a variable, or a literal in value position):
     rendering reads the pattern's retained source text at runtime. */
  /* Regexp#== / #eql? compare by pattern source (dup == original) (#2361) */
  /* Regexp#==/!=/eql?/equal? against a Regexp (by pattern source; equal?
     by identity), #hash (the source's, #3681/#3816) and the receiver-only
     readers below: builtin-op rows (builtin_ops.c) */
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX &&
      emit_builtin_op(c, id, recv, TY_REGEX, name, b)) return 1;
  /* A Regexp is never equal to an operand of any other type: answer false
     rather than rejecting the program (#3632). */
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 1 &&
      (sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "eql?") ||
       sp_streq(name, "equal?")) &&
      comp_ntype(c, argv[0]) != TY_REGEX && repr_of(c, argv[0]).kind != RK_BOXED) {
    /* except nil against the slot's own nil, the NULL pattern */
    if (comp_ntype(c, argv[0]) == TY_NIL && !sp_streq(name, "equal?") && !sp_streq(name, "eql?")) {
      int tn = ++g_tmp;
      buf_printf(b, "({ void *_t%d = (void *)(", tn); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
      emit_boxed(c, argv[0], b);
      buf_printf(b, "); _t%d %s NULL; })", tn, sp_streq(name, "!=") ? "!=" : "==");
      return 1;
    }
    emit_voided_operands(c, recv, argv[0], sp_streq(name, "!=") ? 1 : 0, b);
    return 1;
  }
  /* The IO family's share of Object's protocol, ahead of the generic
     spaceship and to_s fallbacks below: IO handles compare by pointer identity
     (f.flush.equal?(f), #2799) except two File::Stat handles, which compare by
     modification time as Comparable gives them, and a File/IO or Dir handle
     answers Object's to_s and <=>. Exactly these names: `!=` and `==` against
     nil keep their own arm (a NULL handle IS nil), which claiming every
     protocol name here shadowed. */
  if (recv >= 0 && (comp_ntype(c, recv) == TY_IO || comp_ntype(c, recv) == TY_DIR) &&
      ((argc == 0 && sp_streq(name, "to_s")) || (argc == 1 && sp_streq(name, "<=>")) ||
       (comp_ntype(c, recv) == TY_IO && argc == 1 &&
        is_equality_name(name))) &&
      emit_native_object_protocol(c, id, b)) return 1;
  if (recv >= 0 && comp_ntype(c, recv) == TY_REGEX && argc == 0) {
    if (sp_streq(name, "inspect")) {
      emit_null_guarded_call(c, recv, TY_REGEX, "sp_re_inspect_str", "SPL(\"nil\")", b);
      return 1;
    }
    if (sp_streq(name, "to_s")) {
      emit_null_guarded_call(c, recv, TY_REGEX, "sp_re_to_s_str", "sp_str_empty", b);
      return 1;
    }
  }
  /* str.gsub(pattern) / str.gsub!(pattern) with no block -> an Enumerator
     over the matches (the items scan yields), for a literal or a pattern
     held in a value: a Regexp, a String, or either one boxed (a splat's
     element read back out of a mixed array). gsub!'s each block is the edit,
     which a stored one gets as the block form (desugar_stored_enum_each).
     The #inspect names the pattern, as CRuby's does. */
  if (recv >= 0 && rt == TY_STRING && argc == 1 && (sp_streq(name, "gsub") || sp_streq(name, "gsub!")) &&
      nt_ref(nt, id, "block") < 0 && comp_ntype(c, id) == TY_ENUMERATOR) {
    int gre = re_lit_index(c, argv[0]);
    Repr pr = repr_of(c, argv[0]);
    TyKind pt = pr.as_ty;
    if (gre < 0 && pt != TY_REGEX && pt != TY_STRING && pr.kind != RK_BOXED) goto no_gsub_enum;
    int tsg = ++g_tmp, tpat = ++g_tmp, ten = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsg);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", tsg, tpat);
    emit_boxed(c, argv[0], b);
    /* The Enumerator is held in a rooted temporary: the label sp_sprintf
       builds in the same call allocates while it is otherwise held by
       nothing, and a collection there frees it. */
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); "
                  "sp_Enumerator *_t%d = sp_Enumerator_new_from(sp_box_str_array(", tpat, ten);
    if (gre >= 0) buf_printf(b, "sp_re_scan(sp_re_pat_%d, _t%d)", gre, tsg);
    else buf_printf(b, "sp_scan_boxed(_t%d, _t%d)", tsg, tpat);
    buf_printf(b, ")); SP_GC_ROOT(_t%d); sp_enum_with_src(_t%d, sp_box_str(_t%d), "
                  "sp_sprintf(\"%s(%%s)\", sp_poly_inspect(_t%d))); })",
               ten, ten, tsg, name, tpat);
    return 1;
  }
no_gsub_enum:
  /* Object receivers (incl. native-bound classes like StringScanner) dispatch
     their own match?/match methods; only string-ish receivers belong here. */
  if (recv >= 0 && argc >= 1 && rt != TY_SYMBOL && rt != TY_NIL && !ty_is_object(rt) &&
      (is_match_family(name))) {
    int are = re_lit_index(c, argv[0]);
    /* a receiver of no known type (a bare name that resolves to nothing and
       raises NameError) is boxed: the tag checks below take it */
    int rpoly = rt == TY_POLY || rt == TY_UNKNOWN;
    /* a numeric receiver has no =~/!~/match?/match (Object#=~ was removed):
       raise NoMethodError rather than matching the number as a string. */
    if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) &&
        (is_match_family(name))) {
      const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
      const char *dv9 = default_value_from_compiler(c, repr_of(c, id).as_ty);
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of %s\"), %s))",
                 name, tn9, dv9 ? dv9 : "sp_box_nil()");
      return 1;
    }
    if (are >= 0 && sp_streq(name, "=~") && rt == TY_STRING) {
      buf_printf(b, "sp_re_match_poly(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
    /* poly receiver `poly =~ /re/`: String#=~ when it holds a string at runtime
       (e.g. an element read out of an array that widened to poly), Symbol#=~
       (its name) when it holds a Symbol, nil when it holds nil (NilClass#=~ is
       always nil); any other tag has no =~ (Object#=~ was removed) ->
       NoMethodError naming its class, matching CRuby. */
    if (are >= 0 && sp_streq(name, "=~") && rpoly) {
      /* Self-contained statement-expression: this can appear in a pure
         expression position (an `if`/ternary condition) where a g_pre prelude
         would not be flushed and would splice a stray statement into the
         condition (#3187). */
      /* a shared String handle matches its live bytes, through the deref */
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
      buf_printf(b, "); (_t%d.tag == SP_TAG_STR ? sp_re_match_poly(sp_re_pat_%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_SYM ? sp_re_match_poly(sp_re_pat_%d, sp_sym_to_s((sp_sym)_t%d.v.i))"
                    " : _t%d.tag == SP_TAG_NIL ? sp_box_nil()"
                    " : (sp_raise_nomethod(sp_nomethod_msg(\"=~\", _t%d)), sp_box_nil())); })",
                 tv, are, tv, tv, are, tv, tv, tv);
      return 1;
    }
    /* poly receiver `poly !~ /re/`: nil !~ is always true, a string tests the
       negated match; any other tag has no =~ so !~ raises NoMethodError. A
       non-poly (string) receiver keeps the direct negated-match emit. */
    if (are >= 0 && sp_streq(name, "!~") && rpoly) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
      buf_printf(b, "); (_t%d.tag == SP_TAG_STR ? sp_re_match(sp_re_pat_%d, _t%d.v.s) < 0"
                    " : _t%d.tag == SP_TAG_SYM ? sp_re_match(sp_re_pat_%d, sp_sym_to_s((sp_sym)_t%d.v.i)) < 0"
                    " : _t%d.tag == SP_TAG_NIL ? 1"
                    " : (sp_raise_nomethod(sp_nomethod_msg(\"=~\", _t%d)), 0)); })",
                 tv, are, tv, tv, are, tv, tv, tv);
      return 1;
    }
    if (are >= 0 && sp_streq(name, "!~")) {
      buf_printf(b, "(sp_re_match(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ") < 0)");
      return 1;
    }
    /* Poly receiver for `poly.match?(/re/)`: String#match? when it holds a
       string, over its name when it holds a Symbol. NilClass has no match?, so
       nil raises here rather than answering false the way `nil !~` answers
       true. */
    /* ...unless a program class answers match? itself: the boxed dispatch
       has its arm, and the String one beside it */
    if (are >= 0 && sp_streq(name, "match?") && rpoly && poly_name_user_claimed(c, name, argc)) return 0;
    /* match?(pattern) or match?(pattern, pos): another count is the
       boxed dispatch's, which raises the arity error */
    if (are >= 0 && sp_streq(name, "match?") && rpoly && argc > 2) return 0;
    if (are >= 0 && sp_streq(name, "match?") && rpoly) {
      int tv = ++g_tmp, tr = ++g_tmp;
      /* boxed where the analyzer counted a class's match? this call cannot
         reach (a one-argument native one, for `match?(re, pos)`) */
      int bx = repr_of(c, id).kind == RK_BOXED;
      if (bx) buf_puts(b, "sp_box_bool(");
      /* The receiver is held, rooted, while the position runs, which may
         allocate; the String it reads is taken after. A shared-string handle
         is a String (#4279), its bytes kept by the held handle. */
      buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tr);
      /* the position runs before the receiver is judged, whatever it is */
      int tq = 0;
      if (argc == 2) {
        tq = ++g_tmp;
        buf_printf(b, " sp_RbVal _t%d = ", tq); emit_boxed(c, argv[1], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tq);
      }
      buf_printf(b, " sp_RbVal _t%d = sp_poly_strbuf_deref(_t%d);", tv, tr);
      buf_printf(b, " const char *_s%d = _t%d.tag == SP_TAG_SYM ? sp_sym_to_s((sp_sym)_t%d.v.i) : _t%d.v.s;",
                 tv, tv, tv, tv);
      buf_printf(b, " (sp_bool)((_t%d.tag == SP_TAG_STR || _t%d.tag == SP_TAG_SYM) ? ", tv, tv);
      if (argc == 1) buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _s%d)", are, tv);
      else buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, _s%d, sp_poly_arg_int_chk(_t%d))", are, tv, tq);
      /* any other receiver raises as CRuby does (sp_poly_match_check): a
         Regexp's TypeError for the pattern, anything else's NoMethodError
         (nil reads as itself) with the pattern and the position as its
         args */
      buf_printf(b, " : (sp_poly_match_check(_t%d, \"match?\", %d, (sp_RbVal[]){", tv, argc);
      emit_boxed(c, argv[0], b);
      if (argc == 2) buf_printf(b, ", _t%d", tq);
      buf_puts(b, "}), 0)); })");
      if (bx) buf_puts(b, ")");
      return 1;
    }
    /* String#match? on a String. Any other receiver declines: the boxed
       dispatch asks this for a Class value's default arm
       (emit_poly_cls_value_builtin), where it handed the class to the
       matcher as its String */
    if (are >= 0 && sp_streq(name, "match?") && rt == TY_STRING) {
      /* the matcher takes a NULL subject for no match, as Regexp#match?(nil)
         answers: a receiver that can be nil raises nil's NoMethodError first */
      int nn = !recv_never_nil(c, recv);
      if (argc == 1) {
        buf_printf(b, "sp_re_match_p(sp_re_pat_%d, %s", are, nn ? "sp_nil_recv_named(" : "");
        emit_expr(c, recv, b); buf_puts(b, nn ? ", \"match?\"))" : ")");
        return 1;
      }
      /* a position that runs code runs ahead of the test, as CRuby runs the
         arguments ahead of the missing method: the receiver and it are held */
      if (nn && subtree_has_side_effect(c, argv[1]) && !arg_ran_first(argv[1], 0)) {
        int tr = ++g_tmp, tp = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, "; ");
        if (operand_may_allocate(c, argv[1])) buf_printf(b, "SP_GC_ROOT_STR(_t%d); ", tr);
        buf_printf(b, "sp_int _t%d = ", tp); emit_int_expr(c, argv[1], b); buf_puts(b, "; ");
        nil_recv_check("match?", tr, b);
        buf_printf(b, " sp_str_re_match_p_at(sp_re_pat_%d, _t%d, _t%d); })", are, tr, tp);
        return 1;
      }
      buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, %s", are, nn ? "sp_nil_recv_named(" : ""); emit_expr(c, recv, b);
      buf_puts(b, nn ? ", \"match?\"), " : ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
    /* a program class answers match itself: the boxed dispatch has its arm,
       and the String one beside it, as for match? above */
    if (are >= 0 && sp_streq(name, "match") && rpoly && poly_name_user_claimed(c, name, argc)) return 0;
    if (are >= 0 && sp_streq(name, "match") && nt_ref(nt, id, "block") >= 0) {
      /* match(re) { |m| body }: yield the MatchData on a hit, evaluate to the
         block's value; nil (block not run) on a miss */
      int mblk = nt_ref(nt, id, "block");
      int mbody = nt_ref(nt, mblk, "body");
      int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
      int tm = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata(sp_re_pat_%d, ", tm, are);
      emit_str_expr(c, recv, b);
      buf_printf(b, "); sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); if (_t%d) { ",
                 tr2, tr2, tm);
      emit_match_param_bind(c, mblk, tm, b);
      emit_match_block_body(c, mbb, mbn, tr2, b);
      buf_printf(b, "} _t%d; })", tr2);
      return 1;
    }
    if (are >= 0 && sp_streq(name, "match")) {
      Buf mb; memset(&mb, 0, sizeof mb);
      if (argc == 1) {
        /* recv is the subject string; emit_str_expr coerces a poly/nilable
           receiver (`@message.subject.match(/re/)`) to the const char* slot */
        buf_printf(&mb, "sp_re_matchdata(sp_re_pat_%d, ", are); emit_str_expr(c, recv, &mb); buf_puts(&mb, ")");
      }
      else {
        buf_printf(&mb, "sp_re_matchdata_at(sp_re_pat_%d, ", are); emit_str_expr(c, recv, &mb);
        buf_puts(&mb, ", "); emit_int_expr(c, argv[1], &mb); buf_puts(&mb, ")");
      }
      emit_matchdata_as_call(c, id, mb.p, b);
      free(mb.p);
      return 1;
    }
    /* String#match?/#match with a String pattern argument: CRuby treats the
       string as a regexp source (Regexp.new(str)). Compile it at run time.
       (=~ / !~ with a String argument raise TypeError, so are excluded.) */
    if (are < 0 && (rt == TY_STRING || rt == TY_STRBUF) && argc == 1 &&
        comp_ntype(c, argv[0]) == TY_STRING &&
        (sp_streq(name, "match?") || sp_streq(name, "match"))) {
      int ts = ++g_tmp;
      const char *fn = sp_streq(name, "match?") ? "sp_re_match_p" : "sp_re_matchdata";
      /* (match? with a receiver that can be nil holds it and tests it once
         the pattern has run, ahead of the compile: nil's NoMethodError, not
         the pattern's RegexpError) */
      int tr = sp_streq(name, "match?") && !recv_never_nil(c, recv) ? ++g_tmp : 0;
      if (tr) { buf_printf(b, "({ const char *_t%d = ", tr); emit_expr(c, recv, b); buf_printf(b, "; SP_GC_ROOT_STR(_t%d); ", tr); }
      buf_printf(b, "({ const char *_t%d = ", ts); emit_str_expr(c, argv[0], b);
      buf_puts(b, "; ");
      if (tr) { nil_recv_check("match?", tr, b); buf_puts(b, " "); }
      buf_printf(b, "mrb_regexp_pattern *_t%dp = re_compile(_t%d, (int64_t)(_t%d ? sp_str_byte_len(_t%d) : 0), 0); ",
                 ts, ts, ts, ts);
      buf_printf(b, "%s(_t%dp, ", fn, ts);
      if (tr) buf_printf(b, "_t%d); }); })", tr);
      else { emit_expr(c, recv, b); buf_puts(b, "); })"); }
      return 1;
    }
  }
  /* /re/.match(str) and /re/.match(str, pos) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && sp_streq(name, "match") && (argc == 1 || argc == 2) && nt_ref(nt, id, "block") >= 0) {
      /* /re/.match(str) { |m| body }: the same block form String#match already
         had -- yield the MatchData on a hit and evaluate to the block's value,
         nil on a miss. Without the arm the MatchData itself was the value and
         the block never ran (#3642). */
      int mblk = nt_ref(nt, id, "block");
      int mbody = nt_ref(nt, mblk, "body");
      int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
      int tm = ++g_tmp, tr2 = ++g_tmp;
      /* match(str, pos) { |m| ... } starts the scan at pos, as without a
         block: its MatchData was the value, where the call is typed by the
         block's */
      buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata%s(sp_re_pat_%d, ", tm, argc == 2 ? "_at" : "", rre);
      /* a nil subject answers nil without running the block, as the
         blockless form's does: the matchers return NULL for it */
      emit_str_expr_nilable(c, argv[0], b);
      if (argc == 2) { buf_puts(b, ", "); emit_int_expr(c, argv[1], b); }
      buf_printf(b, "); sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); if (_t%d) { ",
                 tr2, tr2, tm);
      emit_match_param_bind(c, mblk, tm, b);
      emit_match_block_body(c, mbb, mbn, tr2, b);
      buf_printf(b, "} _t%d; })", tr2);
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "match") && (argc == 1 || argc == 2)) {
      /* the subject is a string; emit_str_expr coerces a poly/nullable-string
         value (e.g. a `string?` attr read) to const char*, which emit_expr would
         leave as an sp_RbVal into sp_re_matchdata's const char* slot (#3219). */
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", rre); emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ")");
      }
      else {
        char pn[32]; snprintf(pn, sizeof pn, "sp_re_pat_%d", rre);
        emit_re_subject_at(c, "sp_re_matchdata_at", pn, argv[0], a0, argv[1], b);
      }
      return 1;
    }
  }

  /* General handler for regex-related calls where the pattern is an
     interpolated regex (/foo_#{x}/) or a TY_REGEX local variable.
     Covers match?, =~, !~, match, gsub, sub, scan, split as regex arg. */
  {
    /* Pattern from argument (str.match?(/dyn/), str =~ /dyn/, etc.) */
    if (recv >= 0 && argc >= 1) {
      NodeKind a0k = nt_kind(nt, argv[0]);
      int is_interp_arg = a0k == NK_InterpolatedRegularExpressionNode;
      /* Any regex-typed argument EXPRESSION, not just a local or constant read:
         an inline `Regexp.new(s)` / `Regexp.union(..)` / a method returning a
         regex is the same mrb_regexp_pattern* in argument position, so
         `str.match(Regexp.new(s))` belongs here rather than falling through to
         the unresolved-call gate and raising NoMethodError on the String
         (#3389). Two shapes stay out. A bare regex literal is already served
         by the precompiled arms above; routing it here would pull e.g. a
         Symbol receiver off its own handler. And an Object receiver -- a user
         class, or a native-bound one like StringScanner -- dispatches its OWN
         match/match?, the same exclusion those precompiled arms carry: without
         it `matcher.match?(re_local)` fed the object into sp_re_match_p's
         const char* slot and the generated C did not compile. */
      const char *a0nt = nt_type(nt, argv[0]);
      NodeKind a0nk = nt_kind(nt, argv[0]);
      int is_regex_val_arg = !is_interp_arg && argc >= 1 && comp_ntype(c, argv[0]) == TY_REGEX
                             && a0nt && a0nk != NK_RegularExpressionNode
                             && !ty_is_object(rt);
      /* a boxed receiver's match? or match that a class answers too
         (StringScanner's match?, OptionParser::Completion's match) is the
         boxed dispatch's, as for a literal pattern above: the String arm
         converted a receiver of that class to a String (TypeError) */
      int poly_claimed = rt == TY_POLY &&
                         (sp_streq(name, "match?") || sp_streq(name, "match")) &&
                         poly_name_user_claimed(c, name, argc);
      /* Only the match family is answered below. The pattern's setup goes
         ahead of the statement as it is emitted, so building it for any other
         name (sub, gsub, scan, ...) and then declining left it there: an
         interpolated pattern's `#{}` ran once here and again in the arm that
         did take the call. */
      if ((is_interp_arg || is_regex_val_arg) && !poly_claimed && is_match_family(name)) {
        size_t pre_mark = g_pre->len;
        Buf rp; memset(&rp, 0, sizeof rp);
        int rp_ok = emit_regex_pat_to_buf(c, argv[0], &rp) && rp.p;
        /* Fallback: TY_REGEX local/constant/inline Regexp.new -- value IS the mrb_regexp_pattern* */
        if (!rp_ok && is_regex_val_arg) {
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
              (is_match_family(name))) {
            const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
            const char *dv9 = default_value_from_compiler(c, repr_of(c, id).as_ty);
            buf_puts(b, "((void)("); emit_expr(c, recv, b);
            buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of %s\"), %s))",
                       name, tn9, dv9 ? dv9 : "sp_box_nil()");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match?") && (argc == 1 || argc == 2)) {
            /* boxed where the analyzer counted a match? this call cannot
               reach, as for a literal pattern */
            int bx = repr_of(c, id).kind == RK_BOXED;
            if (bx) buf_puts(b, "sp_box_bool(");
            /* A symbol receiver matches over its name, so feed the runtime
               pattern the symbol's string rather than the raw sp_sym. */
            if (rt == TY_SYMBOL) { buf_printf(b, "sp_re_match_p(%s, sp_sym_to_s(", rp.p); emit_expr(c, recv, b); buf_puts(b, "))"); }
            /* emit_str_expr, not emit_expr: a poly receiver (a String read out
               of a widened container) needs coercing into the const char* slot
               the same way the precompiled-literal arms do it (#3389). */
            /* (a String receiver that can be nil raises, as the literal
               pattern's arm has it: held with the pattern and the position
               when one of them runs code, and tested once they have run) */
            else if (rt == TY_STRING && !recv_never_nil(c, recv) &&
                     ((subtree_has_side_effect(c, argv[0]) && !arg_ran_first(argv[0], 0)) ||
                      (argc == 2 && subtree_has_side_effect(c, argv[1]) && !arg_ran_first(argv[1], 0)))) {
              int tr = ++g_tmp, tq = ++g_tmp, tp = argc == 2 ? ++g_tmp : 0;
              buf_printf(b, "({ const char *_t%d = ", tr); emit_str_expr(c, recv, b); buf_puts(b, "; ");
              if (operand_may_allocate(c, argv[0]) || (tp && operand_may_allocate(c, argv[1])))
                buf_printf(b, "SP_GC_ROOT_STR(_t%d); ", tr);
              buf_printf(b, "mrb_regexp_pattern *_t%d = %s; ", tq, rp.p);
              if (tp) { buf_printf(b, "sp_int _t%d = ", tp); emit_int_expr(c, argv[1], b); buf_puts(b, "; "); }
              nil_recv_check("match?", tr, b);
              if (tp) buf_printf(b, " sp_str_re_match_p_at(_t%d, _t%d, _t%d); })", tq, tr, tp);
              else buf_printf(b, " sp_re_match_p(_t%d, _t%d); })", tq, tr);
            }
            else if (argc == 1) {
              int nn = rt == TY_STRING && !recv_never_nil(c, recv);
              buf_printf(b, "sp_re_match_p(%s, %s", rp.p, nn ? "sp_nil_recv_named(" : "");
              emit_str_expr(c, recv, b); buf_puts(b, nn ? ", \"match?\"))" : ")");
            }
            /* match?(re, pos) starts the scan at pos */
            else {
              int nn = rt == TY_STRING && !recv_never_nil(c, recv);
              buf_printf(b, "sp_str_re_match_p_at(%s, %s", rp.p, nn ? "sp_nil_recv_named(" : ""); emit_str_expr(c, recv, b);
              buf_puts(b, nn ? ", \"match?\"), " : ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
            }
            if (bx) buf_puts(b, ")");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "=~") && rt == TY_STRING) {
            buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ")");
            free(rp.p); return 1;
          }
          /* poly receiver `poly =~ /re/`: String#=~ when it holds a string at
             runtime (e.g. an element read out of an array that widened to poly);
             any other tag has no =~ (Object#=~ was removed) -> NoMethodError,
             matching CRuby. */
          if (sp_streq(name, "=~") && rt == TY_POLY) {
            int tv = ++g_tmp;
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, g_pre); buf_puts(g_pre, ");\n");
            buf_printf(b, "(_t%d.tag == SP_TAG_STR ? sp_re_match_poly(%s, _t%d.v.s)"
                          " : sp_raise_nomethod(\"undefined method '=~' for poly\"))",
                       tv, rp.p, tv);
            free(rp.p); return 1;
          }
          if (sp_streq(name, "!~") && argc == 1) {
            /* a poly receiver answers as the literal pattern's poly arm does:
               nil !~ is true, a String or a Symbol tests the negated match,
               any other class has no =~. Read raw, the box went into
               sp_re_match's const char * slot. */
            if (rt == TY_POLY) {
              int tv = ++g_tmp;
              buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
              buf_printf(b, "); SP_GC_ROOT_RBVAL(_t%d); (_t%d.tag == SP_TAG_STR ? sp_re_match(%s, _t%d.v.s) < 0"
                            " : _t%d.tag == SP_TAG_SYM ? sp_re_match(%s, sp_sym_to_s((sp_sym)_t%d.v.i)) < 0"
                            " : _t%d.tag == SP_TAG_NIL ? 1"
                            " : (sp_raise_nomethod(sp_nomethod_msg(\"=~\", _t%d)), 0)); })",
                         tv, tv, rp.p, tv, tv, rp.p, tv, tv, tv);
              free(rp.p); return 1;
            }
            buf_printf(b, "(sp_re_match(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ") < 0)");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && (argc == 1 || argc == 2)) {
            /* emit_str_expr, not emit_expr: a poly receiver needs coercing into
               the const char* slot, as the precompiled-literal arms do (#3389) */
            Buf mb; memset(&mb, 0, sizeof mb);
            if (argc == 1) { buf_printf(&mb, "sp_re_matchdata(%s, ", rp.p); emit_str_expr(c, recv, &mb); buf_puts(&mb, ")"); }
            else {
              buf_printf(&mb, "sp_re_matchdata_at(%s, ", rp.p); emit_str_expr(c, recv, &mb);
              buf_puts(&mb, ", "); emit_int_expr(c, argv[1], &mb); buf_puts(&mb, ")");
            }
            emit_matchdata_as_call(c, id, mb.p, b);
            free(mb.p);
            free(rp.p); return 1;
          }
        }
        /* no branch took it (a nil receiver's =~, match with three
           arguments): the arm that does must not find the setup for a
           pattern it builds again */
        free(rp.p);
        g_pre->len = pre_mark; if (g_pre->p) g_pre->p[pre_mark] = 0;
      }
    }
    /* Pattern from receiver (rx.match?(str), rx =~ str, etc.) */
    {
      NodeKind rk = nt_kind(nt, recv);
      int is_interp_recv = rk == NK_InterpolatedRegularExpressionNode;
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
          if (sp_streq(name, "match?") && argc == 1) {
            buf_printf(b, "sp_re_match_p(%s, ", rp.p);
            emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ")");  /* nil subject: no match */
            free(rp.p); return 1;
          }
          if (sp_streq(name, "===") && argc == 1) {
            /* === sets $~ as =~ does (the literal arm's sp_re_case_eq), and
               answers false for an operand match? raises on */
            if (re_operand_at_runtime(c, a0)) {
              buf_printf(b, "sp_re_case_eq(%s, ", rp.p); emit_boxed(c, argv[0], b); buf_puts(b, ")");
            }
            else {
              int t = ++g_tmp;
              buf_printf(b, "({ const char *_t%d = ", t); emit_str_expr_nilable(c, argv[0], b);
              buf_printf(b, "; SP_GC_ROOT(_t%d); _t%d ? sp_re_match(%s, _t%d) >= 0"
                            " : sp_re_case_eq(%s, sp_box_nil()); })", t, t, rp.p, t, rp.p);
            }
            free(rp.p); return 1;
          }
          if (sp_streq(name, "=~") && argc == 1) {
            if (a0 == TY_STRING) {
              buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ")");
            }
            else if (a0 == TY_NIL) {
              /* nil is the one non-String `re =~ x` accepts: it answers nil
                 rather than raising (#3633) */
              buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
              free(rp.p); return 1;
            }
            else {
              /* a boxed operand, a Symbol, an object (#to_str), or a
                 non-String that raises the TypeError naming its class; a
                 shared-string handle is a String (#4279) */
              buf_printf(b, "sp_re_match_poly(%s, ", rp.p); emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ")");
            }
            free(rp.p); return 1;
          }
          if (sp_streq(name, "!~") && argc == 1 && a0 == TY_STRING) {
            buf_printf(b, "(sp_re_match(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ") < 0)");
            free(rp.p); return 1;
          }
          /* `re !~ x` is !(re =~ x): nil does not match, and a poly operand
             is checked as =~ checks it -- a String is matched, nil answers
             true, anything else raises =~'s TypeError */
          if (sp_streq(name, "!~") && argc == 1 && a0 == TY_NIL) {
            buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), (sp_bool)1)");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "!~") && argc == 1) {
            /* a nil subject (NULL) does not match: true */
            buf_printf(b, "(sp_re_match(%s, ", rp.p); emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ") < 0)");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
            /* the block form: yield the MatchData on a hit, evaluate to the
               block's value, nil on a miss (#3642) */
            int mblk = nt_ref(nt, id, "block");
            int mbody = nt_ref(nt, mblk, "body");
            int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
            int tm = ++g_tmp, tr2 = ++g_tmp;
            buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata(%s, ", tm, rp.p);
            emit_str_expr(c, argv[0], b);
            buf_printf(b, "); sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); if (_t%d) { ",
                       tr2, tr2, tm);
            emit_match_param_bind(c, mblk, tm, b);
            emit_match_block_body(c, mbb, mbn, tr2, b);
            buf_printf(b, "} _t%d; })", tr2);
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && (argc == 1 || argc == 2)) {
            /* the subject can arrive boxed -- one call site passing an
               untyped block param is enough -- so unbox it into the const
               char * slot the way match? and =~ already do */
            if (argc == 1) { buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_re_subject(c, argv[0], a0, 1, b); buf_puts(b, ")"); }
            else emit_re_subject_at(c, "sp_re_matchdata_at", rp.p, argv[0], a0, argv[1], b);
            free(rp.p); return 1;
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
    NodeKind frk = nt_kind(nt, recv);
    int fck = (frk == NK_StringNode || frk == NK_InterpolatedStringNode)
              ? -1 : ++g_tmp;
    if (at == TY_POLY_ARRAY) {
      const char *fend = (operand_may_allocate(c, argv[0]) && !subtree_is_pure_read(c, argv[0])) ? emit_str_format_held(c, recv, argv[0], fck, b) : NULL;
      if (fend) buf_puts(b, ", ");
      else if (fck >= 0) {
        buf_printf(b, "sp_str_format_polyarr(({ const char *_t%d = ", fck);
        emit_expr(c, recv, b);
        buf_printf(b, "; if (!_t%d) sp_nil_recv(\"%%\"); _t%d; }), ", fck, fck);
      }
      else { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); buf_puts(b, ", "); }
      emit_expr(c, argv[0], b); buf_puts(b, fend ? fend : ")");
      return 1;
    }
    const char *ak = array_kind(at);
    if (ak) {
      const char *kind = at == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY"
                       : at == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY" : "SP_BUILTIN_INT_ARRAY";
      const char *fend = emit_str_format_held(c, recv, argv[0], fck, b);
      if (fend) {}
      else if (fck >= 0) {
        buf_printf(b, "sp_str_format_polyarr(({ const char *_t%d = ", fck);
        emit_expr(c, recv, b);
        buf_printf(b, "; if (!_t%d) sp_nil_recv(\"%%\"); _t%d; })", fck, fck);
      }
      else { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); }
      buf_puts(b, typed_array_src_held(c, argv[0]) ? ", sp_typed_to_poly((void *)(" : ", sp_typed_to_poly_unheld((void *)(");
      emit_expr(c, argv[0], b);
      buf_printf(b, "), %s)%s", kind, fend ? fend : ")");
      return 1;
    }
    /* named references ("%<name>spec" / "%{name}") reading from a symbol-keyed
       hash. Handled when the format is a string literal, so each name resolves
       to a compile-time symbol id; the looked-up values are pushed in order and
       the rewritten positional format reuses sp_str_format_polyarr. */
    NodeKind recv_ntype_k = nt_kind(nt, recv);
    if (ty_is_hash(at) && recv_ntype_k == NK_StringNode) {
      const char *fmt = nt_str(nt, recv, "content");
      const char *names[64]; int name_len[64];
      Buf rew; memset(&rew, 0, sizeof rew);
      int nref = fmt ? parse_named_format(fmt, &rew, names, name_len, 64) : -1;
      if (nref >= 0) {
        int th = ++g_tmp, ta = ++g_tmp;
        buf_puts(b, "({ "); th = hold_operand(c, argv[0], TY_POLY, 1, th, 1, " ", b);
        buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new();"
                      " SP_GC_ROOT(_t%d); ", ta, ta);
        for (int k = 0; k < nref; k++) {
          char disp[132];  /* "{name}" / "<name>"; the name itself is < 128 */
          memcpy(disp, names[k], (size_t)name_len[k]); disp[name_len[k]] = 0;
          char nm[128];
          memcpy(nm, disp + 1, (size_t)(name_len[k] - 2)); nm[name_len[k] - 2] = 0;
          buf_printf(b, "sp_PolyArray_push(_t%d, sp_fmt_hash_fetch(_t%d, (sp_sym)%d, ",
                     ta, th, comp_sym_intern(c, nm));
          emit_str_literal(b, disp);
          buf_puts(b, ")); ");
        }
        buf_puts(b, "sp_str_format_polyarr(");
        emit_str_literal(b, rew.p ? rew.p : "");
        buf_printf(b, ", _t%d); })", ta);
        free(rew.p);
        return 1;
      }
      free(rew.p);
    }
    /* a poly RHS may hold an Array (spread across the directives) or a scalar
       (a one-element list) -- the distinction is only known at runtime. An
       object of a class of the program's that is no Array and has no to_ary
       is one argument, whose %s is its to_s, as CRuby formats it boxed. */
    int plain_obj = ty_is_object(at) && class_builtin_parent(c, ty_object_class(at)) == -116 &&
                    comp_method_in_chain(c, ty_object_class(at), "to_ary", NULL) < 0;
    if (at == TY_POLY || plain_obj) {
      const char *fend = emit_str_format_held(c, recv, argv[0], -1, b);
      if (!fend) { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); }
      buf_puts(b, ", sp_format_args("); emit_boxed(c, argv[0], b); buf_printf(b, ")%s", fend ? fend : ")");
      return 1;
    }
    if (at == TY_UNKNOWN && emit_str_format_untyped_array(c, recv, argv[0], fck, b)) return 1;
    /* a single non-array scalar argument formats as a one-element array
       (nil renders empty for %s; Rational/Complex coerce inside the
       formatter's numeric directives; a Bignum is boxed as one) */
    if (at == TY_INT || at == TY_FLOAT || at == TY_STRING || at == TY_SYMBOL ||
        at == TY_NIL || at == TY_BOOL || at == TY_RATIONAL || at == TY_COMPLEX || at == TY_BIGINT) {
      const char *fend = emit_str_format_held(c, recv, argv[0], -1, b);
      if (!fend) { buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b); }
      /* the one-element list is held by `_fa` alone while an argument that
         allocates is built (`"%5s|" % ("q" + i.to_s)`); one that already
         ran into a temp was built before the list */
      int an = arg_ran_first(argv[0], 0) ? argv[0] : nt_paren_single(c->nt, argv[0]);
      /* not asked of a statement left alone, and the walk of the argument
         (subtree_is_pure_read) last */
      int runs = !(!fend && str_format_left_alone(c, recv, argv[0])) &&
                 operand_may_allocate(c, argv[0]) && !arg_ran_first(an, 0) && !subtree_is_pure_read(c, argv[0]);
      /* a number needs no holding: its value is taken ahead of the list
         (`"%5d|" % sq(i)`) and boxed there by the call emit_boxed wrote */
      if (runs && (at == TY_INT || at == TY_FLOAT)) {
        Buf vb; memset(&vb, 0, sizeof vb);
        emit_boxed(c, argv[0], &vb);
        char *val = NULL;
        const char *fn = vb.p ? boxed_number_value(vb.p, at, &val) : NULL;
        /* a value that can be nil is boxed from its oint / ofloat, so the
           temp holding it has that type */
        const char *vty = at == TY_INT ? "sp_int" : "sp_float";
        if (fn && fn == ty_box_nil_fn(at)) vty = at == TY_INT ? "sp_oint" : "sp_ofloat";
        if (fn)
          buf_printf(b, ", ({ %s _fv = %s; sp_PolyArray *_fa = sp_PolyArray_new(); sp_PolyArray_push(_fa, %s(_fv)); _fa; })%s",
                     vty, val, fn, fend ? fend : ")");
        else
          buf_printf(b, ", ({ sp_PolyArray *_fa = sp_PolyArray_new(); SP_GC_ROOT(_fa); sp_PolyArray_push(_fa, %s); _fa; })%s",
                     vb.p ? vb.p : "sp_box_nil()", fend ? fend : ")");
        free(vb.p);
        return 1;
      }
      buf_printf(b, ", ({ sp_PolyArray *_fa = sp_PolyArray_new(); %ssp_PolyArray_push(_fa, ", runs ? "SP_GC_ROOT(_fa); " : "");
      emit_boxed(c, argv[0], b); buf_printf(b, "); _fa; })%s", fend ? fend : ")");
      return 1;
    }
  }
  return 0;
}

/* Regexp's class methods: last_match, try_convert, timeout, escape / quote, union, linear_time? and compile */
int emit_call_regexp_class_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* Regexp.last_match -> the last MatchData ($~), or nil */
  if (recv >= 0 && argc == 0 && sp_streq(name, "last_match") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    buf_puts(b, "sp_re_last_matchdata()");
    return 1;
  }
  /* Regexp.try_convert(x) -> x if it is a Regexp, else nil */
  if (recv >= 0 && argc == 1 && sp_streq(name, "try_convert") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (comp_ntype(c, argv[0]) == TY_REGEX) {
      buf_puts(b, "sp_box_regexp((void *)("); emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else if (emit_try_convert_boxed(c, "Regexp", argv[0], b)) {}
    else {
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
    }
    return 1;
  }
  /* Regexp.timeout / Regexp.timeout= -> spinel enforces no global match timeout;
     the getter is nil and the setter is a no-op returning its argument. */
  if (recv >= 0 && (sp_streq(name, "timeout") || sp_streq(name, "timeout=")) &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (argc == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
    if (argc == 1) { buf_puts(b, "("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return 1; }
  }
  /* Regexp.last_match(n) -> nth capture group string, or whole match for n=0 */
  if (recv >= 0 && argc == 1 && sp_streq(name, "last_match") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    NodeKind ak = nt_kind(nt, argv[0]);
    if (ak == NK_IntegerNode) {
      long long idx = nt_int(nt, argv[0], "value", 0);
      if (idx == 0) { buf_puts(b, "sp_re_match_str"); return 1; }
      if (idx >= 1 && idx <= 9) { buf_printf(b, "sp_re_captures[%d]", (int)idx); return 1; }
      buf_puts(b, "NULL");
      return 1;
    }
    /* a name selects a named group of the pattern that last matched; it was
       read through the integer slot, where the pointer became a wild index
       (#3653) */
    TyKind lmat = comp_ntype(c, argv[0]);
    if (ak == NK_StringNode || ak == NK_SymbolNode) {
      buf_puts(b, "sp_re_named_capture(sp_re_last_pat, ");
      if (ak == NK_SymbolNode) emit_str_literal(b, nt_str(nt, argv[0], "value"));
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    if (lmat == TY_STRING || lmat == TY_SYMBOL) {
      buf_puts(b, "sp_re_named_capture(sp_re_last_pat, ");
      if (lmat == TY_SYMBOL) { buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
      return 1;
    }
    int tv = ++g_tmp;
    emit_indent(g_pre, g_indent);
    { Buf ib_; memset(&ib_, 0, sizeof ib_); emit_int_expr(c, argv[0], &ib_); buf_printf(g_pre, "sp_int _t%d = %s;\n", tv, ib_.p ? ib_.p : "0"); free(ib_.p); }
    buf_printf(b, "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL))",
               tv, tv, tv, tv);
    return 1;
  }
  /* Regexp.escape / Regexp.quote -> escape special regex characters */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "escape") || sp_streq(name, "quote")) &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    Repr rar = repr_of(c, argv[0]);
    TyKind _re_at = rar.as_ty;
    if (rar.kind == RK_BOXED) { buf_puts(b, "sp_re_escape_operand("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (_re_at == TY_SYMBOL) {
      /* rb_reg_operand takes a Symbol by its name -- Regexp.escape(:"a.b")
         is "a\\.b" -- where the #to_str protocol of the String slot would
         refuse it (Regexp.union really does refuse a Symbol; escape does not) */
      buf_puts(b, "sp_re_escape(sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else { buf_puts(b, "sp_re_escape("); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
    return 1;
  }
  /* Regexp.union(pat, ...) -> a pattern matching the alternation of its operands.
     A String operand is regexp-escaped; a Regexp operand (literal or a constant
     bound to one) contributes its source wrapped in CRuby's `(?on-off:src)` option
     group so its flags survive; a single Array argument is expanded into its
     elements. A Regexp built at run time (`Regexp.compile(s)`, a value held
     in a variable) keeps its source too, and joins by the same #to_s form. */
  if (recv >= 0 && argc >= 0 && sp_streq(name, "union") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    /* `Regexp.union([a, b])`: a lone Array argument supplies the operands. */
    const int *ops = argv; int nops = argc;
    if (argc == 1 && nt_kind(nt, argv[0]) == NK_ArrayNode)
      ops = nt_arr(nt, argv[0], "elements", &nops);
    /* `Regexp.union(*[a, b])`: so does a splatted literal */
    else if (argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode &&
             nt_ref(nt, argv[0], "expression") >= 0 &&
             nt_kind(nt, nt_ref(nt, argv[0], "expression")) == NK_ArrayNode)
      ops = nt_arr(nt, nt_ref(nt, argv[0], "expression"), "elements", &nops);
    /* A single Array-valued argument whose elements are only known at run time
       (a variable/expression, not a literal) is joined by the runtime helper. */
    else if (argc == 1) {
      /* `Regexp.union(*a)` is the union of a's elements, as `(a)` is */
      int ua = argv[0];
      int splat = nt_kind(nt, ua) == NK_SplatNode && nt_ref(nt, ua, "expression") >= 0;
      if (splat) ua = nt_ref(nt, ua, "expression");
      Repr uar = repr_of(c, ua);
      if (uar.elem == TY_POLY || uar.elem == TY_STRING || (splat && !uar.untyped)) {
        buf_puts(b, "sp_re_union_array(");
        if (uar.elem == TY_STRING) { buf_puts(b, "sp_StrArray_to_poly_fmt("); emit_expr(c, ua, b); buf_puts(b, ")"); }
        else if (uar.elem == TY_POLY) emit_expr(c, ua, b);
        /* any other splatted value -- boxed, a scalar, nil, a typed array --
           is the Array its splat makes, and each element is checked at run
           time (a non-String, non-Regexp one raises TypeError) */
        else { buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array("); emit_boxed(c, ua, b); buf_puts(b, "))"); }
        buf_puts(b, ")");
        return 1;
      }
      /* a lone boxed argument is told apart at run time: an Array joins
         its elements, a Regexp is the answer itself */
      if (!splat && repr_of(c, ua).kind == RK_BOXED) {
        buf_puts(b, "sp_re_union_boxed("); emit_expr(c, ua, b); buf_puts(b, ")");
        return 1;
      }
    }
    /* A single Regexp operand is returned unchanged (CRuby keeps its source and
       flags verbatim, no option-group wrapper). */
    if (nops == 1 && re_lit_src(c, ops[0]) && emit_regex_pat_to_buf(c, ops[0], b))
      return 1;
    /* a lone Regexp built at run time is the answer itself, as a literal is */
    if (nops == 1 && !re_lit_src(c, ops[0]) && repr_of(c, ops[0]).kind != RK_BOXED &&
        repr_of(c, ops[0]).as_ty == TY_REGEX) {
      emit_expr(c, ops[0], b);
      return 1;
    }
    /* a lone boxed element is told apart at run time as the lone argument
       is; `*[v]` is the argument v itself, so an Array there joins too */
    if (nops == 1 && !re_lit_src(c, ops[0]) && repr_of(c, ops[0]).kind == RK_BOXED) {
      buf_puts(b, nt_kind(nt, argv[0]) == NK_SplatNode ? "sp_re_union_boxed(" : "sp_re_union_one(");
      emit_expr(c, ops[0], b); buf_puts(b, ")");
      return 1;
    }
    /* ...and so is a lone operand of another kind, boxed for the check: a
       Symbol is escaped by name, anything else is the TypeError */
    if (nops == 1 && !re_lit_src(c, ops[0]) && repr_of(c, ops[0]).as_ty != TY_STRING &&
        nt_kind(nt, ops[0]) != NK_SplatNode) {
      buf_puts(b, "sp_re_union_one("); emit_boxed(c, ops[0], b); buf_puts(b, ")");
      return 1;
    }
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
      }
      else {
        Repr ar = repr_of(c, ops[i]);
        TyKind at = ar.as_ty;
        if (nt_kind(nt, ops[i]) == NK_SplatNode)
          unsupported(c, id, "Regexp.union operand splatted beside others");
        if (ar.kind == RK_BOXED) { buf_puts(&ab, "sp_re_union_operand("); emit_expr(c, ops[i], &ab); buf_puts(&ab, ")"); }
        /* any other kind is checked boxed: a Symbol or a number among
           several operands is CRuby's TypeError */
        else if (at != TY_STRING && at != TY_REGEX) { buf_puts(&ab, "sp_re_union_operand("); emit_boxed(c, ops[i], &ab); buf_puts(&ab, ")"); }
        /* a Regexp built at run time: its (?on-off:src) form, as a literal's */
        else if (at == TY_REGEX) { buf_puts(&ab, "sp_re_to_s_str((void *)("); emit_expr(c, ops[i], &ab); buf_puts(&ab, "))"); }
        else { buf_puts(&ab, "sp_re_escape("); emit_expr(c, ops[i], &ab); buf_puts(&ab, ")"); }
      }
      emit_indent(g_pre, g_indent);
      /* rooted: the next operand's to_s or escape allocates while the
         joined source is held */
      if (i == 0) buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n", ts, ab.p ? ab.p : "\"\"", ts);
      /* byte-wise: sp_sprintf's %s would end the branch at an embedded NUL */
      else buf_printf(g_pre, "_t%d = sp_re_alt_join(_t%d, %s);\n", ts, ts, ab.p ? ab.p : "\"\"");
      free(ab.p);
    }
    /* an empty union (`Regexp.union()` or `Regexp.union([])`) never matches */
    if (nops == 0) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = \"(?!)\";\n", ts); }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)(_t%d ? sp_str_byte_len(_t%d) : 0), 0);\n",
               tp, ts, ts, ts);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  /* Regexp.linear_time?(re) -> whether re matches in linear time. A literal arg
     is inspected for a backreference (the construct that defeats it); a
     non-literal regexp value defaults to true (the answer for backref-free
     patterns, which is the supported domain). */
  if (recv >= 0 && argc == 1 && sp_streq(name, "linear_time?") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (nt_kind(nt, argv[0]) == NK_RegularExpressionNode)
      buf_puts(b, re_src_has_backref(nt_str(nt, argv[0], "unescaped")) ? "FALSE" : "TRUE");
    /* a pattern reached through a variable, or given as a String source, is
       inspected at run time rather than assumed linear (#3684) */
    else if (comp_ntype(c, argv[0]) == TY_STRING) {
      buf_puts(b, "sp_re_src_linear_time("); emit_expr(c, argv[0], b); buf_puts(b, ")");
    }
    else if (comp_ntype(c, argv[0]) == TY_REGEX) {
      buf_puts(b, "sp_re_src_linear_time(sp_re_source((void *)(");
      emit_expr(c, argv[0], b); buf_puts(b, ")))");
    }
    else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), TRUE)"); }
    return 1;
  }
  /* Regexp.compile is an alias for Regexp.new, and compiles as it does: a
     String source held boxed is read as a String, a Regexp is copied */
  if (recv >= 0 && argc >= 1 && sp_streq(name, "compile") &&
      nt_kind(nt, recv) == NK_ConstantReadNode &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    emit_regexp_new(c, argc, argv, b);
    return 1;
  }
  return 0;
}

/* Symbol and true / false receivers' methods, a String's block iterators (each_char, each_line, chars, lines, bytes, ...), then the scalar builtins (emit_scalar_call) */
int emit_call_symbol_bool_string_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    /* the arms that read only the receiver and the arguments: builtin-op
       rows (builtin_ops.c) */
    if (emit_builtin_op(c, id, recv, TY_SYMBOL, name, b)) return 1;
    /* string-surface methods over the symbol's name; succ re-interns a symbol,
       index/slice yield a substring (or nil), the predicates yield a bool. */
    if ((is_slice_alias(name)) && argc == 1 &&
        nt_kind(c->nt, argv[0]) == NK_RangeNode) {
      /* :s[a..b] / :s[a...b] over the name; a beginless/endless bound is 0 /
         the name length. The name is materialized once to avoid re-evaluating
         the receiver for an endless range. */
      int rn = argv[0];
      int excl = nt_range_excl(c->nt, rn);
      int lo = nt_ref(c->nt, rn, "left"), hi = nt_ref(c->nt, rn, "right");
      int t = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = sp_sym_to_s(", t); emit_expr(c, recv, b);
      char none_hi[64];
      snprintf(none_hi, sizeof none_hi, "(sp_int)sp_str_length(_t%d)", t);
      buf_printf(b, "); sp_str_sub_range_r(_t%d, ", t);
      if (lo >= 0) emit_int_expr_bound(c, lo, "0", b); else buf_puts(b, "0");
      buf_puts(b, ", ");
      if (hi >= 0) { emit_int_expr_bound(c, hi, none_hi, b); buf_printf(b, ", %d); })", excl); }
      else buf_printf(b, "(sp_int)sp_str_length(_t%d), 0); })", t);
      return 1;
    }
    if ((is_slice_alias(name)) && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_INT || repr_of(c, argv[0]).kind == RK_BOXED)) {
      buf_puts(b, "sp_str_char_at_or_nil(sp_sym_to_s("); emit_expr(c, recv, b); buf_puts(b, "), ");
      emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "match?") && argc == 1) {
      int rre = re_lit_index(c, argv[0]);
      if (rre >= 0) {
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, sp_sym_to_s(", rre); emit_expr(c, recv, b);
        buf_puts(b, ")) >= 0)");
        return 1;
      }
    }
  }

  /* boolean receiver methods */
  if (recv >= 0 && rt == TY_BOOL) {
    if (is_text_conversion(name)) {
      buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") ? sp_str_frozen_true : sp_str_frozen_false)");
      return 1;
    }
    if (is_bit_op(name)) {
      buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, " %s ", name); emit_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
  }

  /* str.each_char / each_line / chars / lines / bytes / codepoints { |x| ... } -> iterate, return self. */
  if (recv >= 0 && rt == TY_STRING && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "each_byte") ||
       sp_streq(name, "chars") || sp_streq(name, "lines") || sp_streq(name, "bytes") || sp_streq(name, "codepoints"))) {
    int block = nt_ref(nt, id, "block");
    /* The loop below runs a literal block's body. A block argument that
       reached here (`s.bytes(&pr)`, `s.chars(&b)` for a method's own &b)
       has none: the loop ran nothing and answered the receiver, where CRuby
       calls the proc for each element, or answers the Array when it is nil. */
    if (nt_kind(nt, block) == NK_BlockArgumentNode)
      unsupported(c, id, "a String iterator given its block as a block argument (&blk): write the block out, or use each_char / each_byte / each_line");
    int body = nt_ref(nt, block, "body");
    /* the parameter's C name, which an inlined yield renames, and its own,
       which the scope knows it by */
    const char *p0n = block_param_name(c, block, 0), *p0 = p0n ? rename_local(p0n) : NULL;
    int ts = ++g_tmp, ti = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    int is_line = sp_streq(name, "each_line") || sp_streq(name, "lines");
    int is_byte = sp_streq(name, "each_byte") || sp_streq(name, "bytes");
    int is_cp = sp_streq(name, "codepoints");
    Scope *cs_ech = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *clv_ech = (p0 && cs_ech) ? scope_local(cs_ech, p0n) : NULL;
    int p0_box_poly_ech = clv_ech && clv_ech->type == TY_POLY;
    /* a parameter the analysis made a shared handle (the line is stored
       where an append reaches it): each line is a fresh String, so it is a
       fresh handle */
    int p0_handle_ech = clv_ech && clv_ech->type == TY_STRBUF;
    /* The loop below reads the receiver on every turn -- as its bound, and as
       the string it takes the next character or byte out of -- and the block
       between two turns may allocate. A temporary receiver (`array.join.
       each_char { }`) has no other holder at that point, so the temp is a
       root, the way the array iterators root their hoisted receiver. */
    /* One representation: the iterator answers its receiver, which for a
       variable the block can rebind (an ivar, a class variable, a global)
       is read as a copy of its bytes (sp_strbuf_read_pub_own): the handle
       is held ahead of the loop and its payload's view answered, which
       is the String itself where it is stored (`[@s.each_char { }]`
       stored the copy) */
    int th_ech = 0;
    NodeKind rk_ech = nt_kind(nt, recv);
    Buf hb_ech; memset(&hb_ech, 0, sizeof hb_ech);
    if ((rk_ech == NK_InstanceVariableReadNode || rk_ech == NK_ClassVariableReadNode ||
                         rk_ech == NK_GlobalVariableReadNode) && strbuf_slot_text(c, recv, &hb_ech)) {
      th_ech = ++g_tmp;
      buf_printf(b, "({ sp_String *_t%d = SP_STR_HDL(%s); SP_GC_ROOT(_t%d); ", th_ech, hb_ech.p, th_ech);
    }
    free(hb_ech.p);
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); ", ts, rb.p ? rb.p : "", ts); free(rb.p);
    /* a receiver that can be nil (`a[5].each_char { }`) raises NoMethodError,
       where the loop walked nil as an empty String */
    if (!recv_never_nil(c, recv)) buf_printf(b, "if (!_t%d) sp_nil_recv(\"%s\"); ", ts, name);
    /* Save outer variable before loop to restore it afterward */
    int tsv_ech = 0;
    if (p0 && clv_ech) {
      tsv_ech = ++g_tmp;
      Buf sv_ech; memset(&sv_ech, 0, sizeof sv_ech); emit_ctype(c, clv_ech->type, &sv_ech);
      /* (one representation: the slot's own type, whichever face it holds) */
      buf_printf(b, "%s _t%d = lv_%s; ", "__auto_type", tsv_ech, p0); free(sv_ech.p);
    }
    if (is_line) {
      int tl = ++g_tmp;
      /* chomp: true keyword arg uses the _chomp variant */
      int eline_chomp = 0, cv = -1;
      int kwh = nt_argv_kwh(nt, argv, argc);
      if (kwh >= 0) {
        cv = struct_kwarg_value(c, kwh, "chomp");
        eline_chomp = kw_flag_static(c, cv);
      }
      /* a separator argument splits on it, the way the blockless enumerator
         form already does (#3594) */
      int eline_sep = (argc >= 1 && argv && argv[0] != kwh && lines_sep_ty(c, argv[0])) ? argv[0] : -1;
      /* (one-String mode: a nil separator yields the receiver itself, as
         CRuby does, a String it holds the handle of; none for an empty one) */
      int eline_whole = eline_sep >= 0 && eline_chomp == 0 &&
                        repr_of(c, argv[0]).as_ty == TY_NIL;
      if (eline_whole)
        buf_printf(b, "sp_StrArray *_t%d = sp_StrArray_new(); if (sp_str_byte_len(_t%d) > 0) sp_StrArray_push(_t%d, _t%d); ",
                   tl, ts, tl, ts);
      else if (eline_sep >= 0) {
        buf_printf(b, "sp_StrArray *_t%d = ", tl);
        if (eline_chomp < 0) { emit_kw_flag(c, cv, b); buf_puts(b, " ? "); }
        if (eline_chomp != 0) { buf_printf(b, "sp_str_lines_sep_chomp(_t%d, ", ts); emit_lines_sep(c, eline_sep, b); buf_puts(b, ")"); }
        if (eline_chomp < 0) buf_puts(b, " : ");
        if (eline_chomp != 1) { buf_printf(b, "sp_str_lines_sep(_t%d, ", ts); emit_lines_sep(c, eline_sep, b); buf_puts(b, ")"); }
        buf_puts(b, "; ");
      }
      else if (eline_chomp < 0) {
        buf_printf(b, "sp_StrArray *_t%d = ", tl);
        emit_kw_flag(c, cv, b);
        buf_printf(b, " ? sp_str_lines_chomp(_t%d) : sp_str_lines(_t%d); ", ts, ts);
      }
      else
        buf_printf(b, "sp_StrArray *_t%d = %s(_t%d); ",
                   tl, eline_chomp ? "sp_str_lines_chomp" : "sp_str_lines", ts);
      /* The array of lines is this loop's own value -- nothing in the Ruby
         program names it -- and its length is the loop bound, re-read every
         turn. Unrooted, a block that allocated collected it, and the loop
         then ended early rather than yielding a wrong line: the symptom is a
         short answer, not a bad one. */
      buf_printf(b, "SP_GC_ROOT(_t%d); ", tl);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) { ",
                 ti, ti, tl, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = %ssp_StrArray_get%s(_t%d, _t%d)); ", p0, ONE_SA_BOXO, ONE_SA_GE, tl, ti);
        else if (p0_handle_ech) buf_printf(b, "lv_%s = sp_String_new_shared(sp_StrArray_get(_t%d, _t%d)); ", p0, tl, ti);
        else buf_printf(b, "lv_%s = SP_STR_B(sp_StrArray_get(_t%d, _t%d)); ", p0, tl, ti);   /* (one representation: the line is a handle, the parameter's slot bytes) */
      }
    }
    /* codepoints yields each character's codepoint, not each byte: the String
       walked to its byte length as #chars walks it (sp_str_codepoints_all) */
    else if (is_cp) {
      int tc = ++g_tmp;
      buf_printf(b, "sp_IntArray *_t%d = sp_str_codepoints_all(_t%d); SP_GC_ROOT(_t%d); ", tc, ts, tc);
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) { ", ti, ti, tc, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_int(sp_IntArray_get(_t%d, _t%d)); ", p0, tc, ti);
        else buf_printf(b, "lv_%s = sp_IntArray_get(_t%d, _t%d); ", p0, tc, ti);
      }
    }
    else if (is_byte) {
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < (sp_int)sp_str_byte_len(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_int((unsigned char)_t%d[_t%d]); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = (unsigned char)_t%d[_t%d]; ", p0, ts, ti);
      }
    }
    else {
      /* walked by byte offset, each turn stepping over one character: taking
         the i-th character counted the string from its start every turn once
         a multibyte character made it not fixed width, quadratic in its length */
      int tn = ++g_tmp;
      buf_printf(b, "size_t _t%d = sp_str_byte_len(_t%d); ", tn, ts);
      buf_printf(b, "for (size_t _t%d = 0; _t%d < _t%d; ) { ", ti, ti, tn);
      if (p0) {
        char step[96];
        snprintf(step, sizeof step, "sp_str_char_step(_t%d, _t%d, &_t%d)", ts, tn, ti);
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(%s); ", p0, step);
        else if (p0_handle_ech) buf_printf(b, "lv_%s = sp_String_new_shared(%s); ", p0, step);
        else buf_printf(b, "lv_%s = %s; ", p0, step);
      }
      else buf_printf(b, "_t%d += sp_str_char_width(_t%d, _t%d, _t%d); ", ti, ts, tn, ti);
    }
    emit_iter_loop_stmts(c, body, b, 0);
    if (p0 && tsv_ech > 0) buf_printf(b, " lv_%s = _t%d;", p0, tsv_ech);
    buf_printf(b, " } _t%d; })", ts);
    if (th_ech) buf_printf(b, "; sp_strbuf_read_pub(_t%d); })", th_ech);
    return 1;
  }

  if (emit_or_take_back(c, id, b, emit_scalar_call)) return 1;
  return 0;
}

/* Symbol#encoding (US-ASCII when the name is pure ASCII, UTF-8 otherwise), and equal? / eql? between two Symbols */
int emit_call_symbol_misc_arms(Compiler *c, Buf *b, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  /* Symbol#encoding: US-ASCII when the name is pure ASCII, UTF-8 otherwise */
  if (recv >= 0 && rt == TY_SYMBOL && argc == 0 && sp_streq(name, "encoding")) {
    int te = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_sym_to_s(", te);
    emit_expr(c, recv, b);
    buf_printf(b, "); int _a%d = 1; for (const char *_p%d = _t%d; *_p%d; _p%d++)"
                  " if ((unsigned char)*_p%d >= 0x80) { _a%d = 0; break; }"
                  " sp_box_encoding(_a%d ? sp_encoding_us_ascii() : sp_encoding_utf8()); })",
               te, te, te, te, te, te, te, te);
    return 1;
  }
  if (recv >= 0 && rt == TY_SYMBOL && argc == 1 &&
      (is_eql_or_equal(name)) &&
      comp_ntype(c, argv[0]) == TY_SYMBOL) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
    emit_expr(c, argv[0], b); buf_puts(b, "))");
    return 1;
  }
  return 0;
}

/* Invalid and boxed patterns share the runtime check; analysis has already
   settled whether each match is a String or a captures array. */
int emit_op_string_scan_checked(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  TyKind t = comp_ntype(c, argv[0]);
  if (t == TY_STRING || t == TY_STRBUF || t == TY_REGEX || t == TY_UNKNOWN) return 0;
  buf_printf(b, "%s(%s, ", repr_of(c, x->id).as_ty == TY_POLY_ARRAY ? "sp_scan_boxed_poly" : "sp_scan_boxed", x->rtext);
  emit_boxed(c, argv[0], b);
  buf_puts(b, ")");
  return 1;
}

/* String#scrub { |bytes| ... }: the receiver's valid runs copied through,
   and each invalid sequence (sp_str_scrub_bad) handed to the block, whose
   answer replaces it: a String, converted as CRuby's implicit conversion
   does, and valid UTF-8 (sp_str_scrub_repl). A replacement argument besides
   the block is CRuby's ArgumentError unless it is nil. An ASCII-8BIT
   receiver has no invalid sequence: the walk is skipped, the block never
   runs, and the answer is sp_str_scrub's ASCII-8BIT copy, as in CRuby. */
int emit_op_string_scrub_block(Compiler *c, const BopCtx *x, Buf *b) {
  const NodeTable *nt = c->nt;
  int block = nt_ref(nt, x->id, "block");
  if (block < 0 || nt_kind(nt, block) != NK_BlockNode) return 0;
  int argc;
  const int *argv = call_args(nt, x->id, &argc);
  int ts = ++g_tmp, tl = ++g_tmp, tp = ++g_tmp, tout = ++g_tmp, tb = ++g_tmp, tn = ++g_tmp, tbin = ++g_tmp;
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);\n", ts, x->rtext, ts);
  if (argc == 1 && nt_kind(nt, argv[0]) != NK_NilNode) {
    Buf ab; memset(&ab, 0, sizeof ab); emit_boxed(c, argv[0], &ab);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (!sp_poly_nil_p(%s)) sp_raise_cls(\"ArgumentError\", \"both of block and replacement given\");\n",
               ab.p ? ab.p : "sp_box_nil()");
    free(ab.p);
  }
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "int _t%d = sp_str_is_binary(_t%d);\n", tbin, ts);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_int _t%d = _t%d ? 0 : (sp_int)sp_str_byte_len(_t%d), _t%d = 0;\n", tl, tbin, ts, tp);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_String *_t%d = sp_String_new(\"\"); SP_GC_ROOT(_t%d);\n", tout, tout);
  emit_indent(g_pre, g_indent); buf_printf(g_pre, "while (_t%d < _t%d) {\n", tp, tl);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_int _t%d, _t%d = sp_str_scrub_bad(_t%d, _t%d, _t%d, &_t%d);\n", tn, tb, ts, tl, tp, tn);
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append_bin(_t%d, sp_str_substr(_t%d, _t%d, _t%d - _t%d));\n", tout, ts, tp, tb, tp);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "if (_t%d >= _t%d) break;\n", tb, tl);
  const char *p0 = block_param_name(c, block, 0);
  if (p0) {
    Scope *ps = comp_scope_of(c, block);
    LocalVar *plv = ps ? scope_local(ps, p0) : NULL;
    int box = plv && plv->type == TY_POLY;
    /* a parameter held as a shared handle (--share-strings) takes a fresh
       String of the match */
    int hnd = !box && plv && repr_of_slot(c, plv).handle;
    emit_indent(g_pre, g_indent + 1);
    buf_printf(g_pre, "lv_%s = %ssp_str_substr(_t%d, _t%d, _t%d)%s;\n",
               rename_local(p0), box ? "sp_box_str(" : hnd ? "SP_STR_HDL(" : "", ts, tb, tn,
               (box || hnd) ? ")" : "");
  }
  int save = g_indent; g_indent++;
  IterStep st; emit_iter_step_open(c, block, 1, g_indent, &st);
  Buf vb; memset(&vb, 0, sizeof vb); emit_iter_step_tail(c, &st, &vb);
  g_indent = save;
  emit_indent(g_pre, g_indent + 1);
  buf_printf(g_pre, "sp_String_append_bin(_t%d, sp_str_scrub_repl(sp_poly_arg_str_chk(%s)));\n", tout, vb.p ? vb.p : "sp_box_nil()");
  free(vb.p);
  emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "_t%d = _t%d + _t%d;\n", tp, tb, tn);
  emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
  buf_printf(b, "(_t%d ? sp_str_scrub(_t%d, 0) : _t%d->data)", tbin, ts, tout);
  return 1;
}

/* An append chain over an existing handle must hand that handle to the
   next link. Mark its receiver links before operand ordering can bind a
   String read into a const char * temp, and restore their emission types
   afterwards. No slot becomes shared here. */
int emit_str_append_chain_handle(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name || !is_string_append_or_prepend(name)) return 0;
  int recv = nt_ref(nt, id, "receiver"), args = nt_ref(nt, id, "arguments"), argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  if (recv < 0 || argc < 1 || nt_ref(nt, id, "block") >= 0) return 0;
  TyKind rt = comp_ntype(c, recv);
  if (rt != TY_STRING && rt != TY_STRBUF) return 0;
  int first = nt_paren_single(c->nt, recv);
  Repr rp = repr_of(c, first);
  if (rp.handle && rp.kind == RK_STRBUF) return 0;
  int links[64], nlinks = 0, cur = recv, calls = 0;
  while (cur >= 0 && nlinks < 64) {
    if (nt_kind(nt, cur) == NK_ParenthesesNode) {
      int body = nt_ref(nt, cur, "body"), n = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &n) : NULL;
      if (n != 1) break;
      links[nlinks++] = cur; cur = bb[0]; continue;
    }
    if (nt_kind(nt, cur) != NK_CallNode) break;
    const char *nm = nt_str(nt, cur, "name");
    int ca = nt_ref(nt, cur, "arguments"), ac = 0;
    if (ca >= 0) nt_arr(nt, ca, "arguments", &ac);
    if (!nm || !is_append_concat(nm) ||
        ac < 1 || nt_ref(nt, cur, "block") >= 0) break;
    links[nlinks++] = cur; calls++;
    cur = nt_ref(nt, cur, "receiver");
  }
  if (!calls || cur < 0 ||
      (nt_kind(nt, cur) != NK_LocalVariableReadNode &&
       nt_kind(nt, cur) != NK_InstanceVariableReadNode) ||
      !strbuf_slot_has(c, cur)) return 0;
  int sv[64], st[64];
  for (int i = 0; i < nlinks; i++) {
    sv[i] = view_push_repr(c, links[i], VR_STRBUF_BOX, 1);
    st[i] = view_push(c, links[i], TY_STRBUF);
  }
  emit_call(c, id, b);
  for (int i = nlinks - 1; i >= 0; i--) {
    view_pop(c, st[i]);
    view_pop(c, sv[i]);
  }
  return 1;
}

/* An unbound prepend operand can emit a prelude of its own. Capture each
   operand and its prelude together before concatenating their values. */
static void emit_string_prepend_ordered(Compiler *c, int recv_tmp, int argc,
                                        const int *argv, Buf *b) {
  int *temps = malloc(sizeof(int) * argc);
  buf_printf(b, " SP_GC_ROOT(_t%d);", recv_tmp);
  for (int j = 0; j < argc; j++) {
    Buf arg = {0};
    Buf *pre = g_pre;
    g_pre = b;
    emit_str_expr(c, argv[j], &arg);
    g_pre = pre;
    temps[j] = ++g_tmp;
    buf_printf(b, " const char *_t%d = %s; SP_GC_ROOT(_t%d);",
               temps[j], arg.p, temps[j]);
    free(arg.p);
  }
  int result = ++g_tmp;
  buf_printf(b, " const char *_t%d = ", result);
  for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
  buf_printf(b, "_t%d", temps[0]);
  for (int j = 1; j < argc; j++) buf_printf(b, ", _t%d)", temps[j]);
  buf_printf(b, ", sp_String_cstr(_t%d)); sp_String_set_bin(_t%d, _t%d);",
             recv_tmp, recv_tmp, result);
  free(temps);
}

int emit_string_handle_append(Compiler *c, int id, Buf *b, const char *name, int recv, int argc, const int *argv) {
  const NodeTable *nt = c->nt;
  /* The zero-argument row still checks frozen state and answers self,
     including when an outer call has held the receiver as a handle. */
  if (argc == 0 && is_string_append_or_prepend(name) &&
      bop_answers_self(TY_STRING, name, argc, 0) == BOPF_SELF) {
    Buf ref; memset(&ref, 0, sizeof ref);
    if (strbuf_recv_handle(c, id, nt_paren_single(c->nt, recv), &ref)) {
      int t = ++g_tmp;
      buf_printf(b, "({ sp_String *_t%d = SP_STR_HDL(%s); if (sp_String_is_frozen(_t%d)) "
                    "sp_raise_frozen_str(sp_String_cstr(_t%d)); ", t, ref.p, t, t);
      free(ref.p);
      if (repr_of(c, id).handle) buf_printf(b, "_t%d; })", t);
      else buf_printf(b, "sp_strbuf_read_pub(_t%d); })", t);
      return 1;
    }
  }
  if (is_string_append_or_prepend(name) && argc >= 1) {
    /* a STRBUF-promoted local (repeated `<<`) appends in place: the read
       form sp_String_cstr(lv) is not an lvalue, so the generic write-back
       below would emit an invalid assignment (#2020). prepend replaces the
       buffer with args-then-contents, keeping the handle stable (#3227). */
    { Buf sref0; memset(&sref0, 0, sizeof sref0);
      int sr = nt_paren_single(c->nt, recv);
      const char *sn = nt_kind(nt, sr) == NK_CallNode ? nt_str(nt, sr, "name") : NULL;
      Repr rp = repr_of(c, sr);
      int chain_handle = sn && is_append_concat(sn) && rp.handle && rp.kind == RK_STRBUF;
      /* or a route over a handle (`(+s) << x`, --share-strings), which
         appends to the String it hands on */
      int hr0 = chain_handle ? 0 : strbuf_recv_handle(c, id, sr, &sref0);
      /* a route over a handle (a chain of calls answering their receiver
         over one, `id(s).concat(x).insert(0, y) << z`) that
         strbuf_recv_handle did not take is emitted whole */
      Buf lb; memset(&lb, 0, sizeof lb);
      if (!chain_handle && !hr0 && emit_recv_route_handle(c, id, sr, &lb)) hr0 = 2;
      if (chain_handle || hr0) {
        int tb2 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = ", tb2);
        if (chain_handle) emit_expr(c, sr, b);
        else buf_puts(b, lb.p ? lb.p : sref0.p);
        free(lb.p); free(sref0.p);
        buf_puts(b, ";");
        /* (prepend builds its text with sp_str_concat, which allocates) */
        emit_route_recv_root(c, hr0, tb2, argc, is_append_concat(name) ? argv : NULL, TY_STRING, b);
        if (!is_append_concat(name)) {
          int ordered = 0;
          for (int j = 0; j < argc && argc > 1; j++)
            if (!arg_ran_first(argv[j], 0) && !subtree_is_pure_read(c, argv[j])) ordered = 1;
          if (ordered) emit_string_prepend_ordered(c, tb2, argc, argv, b);
          else {
            int tp3 = ++g_tmp;
            buf_printf(b, " const char *_t%d = ", tp3);
            for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
            emit_str_expr(c, argv[0], b);
            for (int j = 1; j < argc; j++) { buf_puts(b, ", "); emit_str_expr(c, argv[j], b); buf_puts(b, ")"); }
            buf_printf(b, ", sp_String_cstr(_t%d)); sp_String_set_bin(_t%d, _t%d);", tb2, tb2, tp3);
          }
        }
        else if (is_concat_name(name) && argc >= 2) {
          /* Value position takes the same argument snapshots as a
             statement before an alias of the receiver can grow. */
          char ref[48];
          snprintf(ref, sizeof ref, "_t%d", tb2);
          emit_str_concat_handle(c, ref, argc, argv, b, 0);
        }
        else {
          for (int j = 0; j < argc; j++) {
            buf_printf(b, " sp_String_append(_t%d, ", tb2);
            { char rt[48]; snprintf(rt, sizeof rt, "sp_String_cstr(_t%d)", tb2);
              emit_str_append_arg(c, argv[j], rt, b); }
            buf_puts(b, ");");
          }
        }
        /* an append marked to hand out the handle (`r = obj.buf << x`)
           answers the receiver itself; otherwise its String read */
        if (repr_of(c, id).handle) buf_printf(b, " _t%d; })", tb2);
        else buf_printf(b, " sp_strbuf_read_pub(_t%d); })", tb2);
        return 1;
      }
    }
  }
  return 0;
}

int emit_op_poly_case_options(Compiler *c, const BopCtx *x, Buf *b) {
  int argc;
  const int *argv = call_args(c->nt, x->id, &argc);
  int recv = x->recv;
  const char *name = x->name;
  const NodeTable *nt = c->nt;
  /* upcase / downcase / capitalize / swapcase given options, on a boxed
     String or Symbol: the arms below map it as they do with none, and the
     options are checked as on a typed receiver (emit_case_opts_guard) -- a
     literal :ascii alone picks the ASCII mapping and needs no check. With no
     arm the call raised NoMethodError naming String. */
  if (!user_defines_or_reads(c, name)) {
    int plain_args = !nt_argv_has(nt, argv, argc, NT_ARG_SPLAT | NT_ARG_KEYWORDS | NT_ARG_BLOCK);
    if (plain_args) {
      const char *sfx = case_map_suffix(c, argc, argv);
      int tv = ++g_tmp;
      buf_puts(b, "({ ");
      tv = hold_operand(c, recv, TY_POLY, 0, tv, 1, " ", b);
      int turkic_rt = 0;
      if (!*sfx) {
        int literals = 1;
        for (int i = 0; i < argc; i++)
          if (nt_kind(nt, argv[i]) != NK_SymbolNode) literals = 0;
        int *ta = calloc((size_t)argc + 1, sizeof *ta);
        if (!ta) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        if (!literals) {
          int first = g_tmp + 1;
          g_tmp += argc;
          for (int i = 0; i < argc; i++) ta[i] = hold_operand(c, argv[i], TY_POLY, 1, first + i, 1, " ", b);
          turkic_rt = 1;
          buf_printf(b, "sp_case_opts_is_turkic(%d, (sp_RbVal[]){", argc);
          for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); buf_printf(b, "_t%d", ta[i]); }
          buf_printf(b, "}) ? sp_poly_case_conv(_t%d, _t%d.tag == SP_TAG_SYM ? sp_str_%s_turkic_sym : sp_str_%s_turkic, \"%s\") : ({ ",
                     tv, tv, name, name, name);
        }
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR || _t%d.tag == SP_TAG_SYM || sp_poly_is_strbuf(_t%d)) "
                      "sp_case_opts_check(%d, (sp_RbVal[]){", tv, tv, tv, argc);
        for (int i = 0; i < argc; i++) {
          if (i) buf_puts(b, ", ");
          if (literals) emit_boxed(c, argv[i], b);
          else buf_printf(b, "_t%d", ta[i]);
        }
        free(ta);
        buf_printf(b, "}, %s, _t%d); ", x->op->arg, tv);
      }
      if (sp_streq(sfx, "_turkic")) buf_printf(b, "sp_poly_case_conv(_t%d, _t%d.tag == SP_TAG_SYM ? sp_str_%s_turkic_sym : sp_str_%s_turkic, \"%s\"); })", tv, tv, name, name, name);
      else buf_printf(b, "sp_poly_case_conv(_t%d, sp_str_%s%s, \"%s\")%s; })", tv, name, sfx, name, turkic_rt ? "; })" : "");
      return 1;
    }
  }
  return 0;
}

/* Boxed slice! arguments select the same overloads as typed arguments.
   Keep the argument rooted and evaluate it once before re-entering the arms. */
static int emit_string_slice_poly(Compiler *c, int id, int arg, Buf *b) {
  if (repr_of(c, arg).kind != RK_BOXED) return 0;
  int ta = ++g_tmp, tr = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, arg, b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); _t%d = sp_poly_strbuf_deref(_t%d);"
                " const char *_t%d; if (_t%d.tag == SP_TAG_STR) { _t%d = ",
             ta, ta, ta, tr, ta, tr);
  int bind = view_bind(arg, "_t%d.v.s", ta);
  int v = view_push(c, arg, TY_STRING);
  emit_array_call(c, id, b);
  view_pop(c, v);
  view_unbind(bind);
  buf_printf(b, "; }\nelse if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_REGEX) { _t%d = ", ta, ta, tr);
  bind = view_bind(arg, "((mrb_regexp_pattern *)_t%d.v.p)", ta);
  v = view_push(c, arg, TY_REGEX);
  emit_array_call(c, id, b);
  view_pop(c, v);
  view_unbind(bind);
  buf_printf(b, "; }\nelse if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE) { _t%d = ", ta, ta, tr);
  bind = view_bind(arg, "(*(sp_Range *)_t%d.v.p)", ta);
  v = view_push(c, arg, TY_RANGE);
  emit_array_call(c, id, b);
  view_pop(c, v);
  view_unbind(bind);
  buf_printf(b, "; }\nelse { _t%d = ", tr);
  bind = view_bind(arg, "sp_poly_arg_int_chk(_t%d)", ta);
  v = view_push(c, arg, TY_INT);
  emit_array_call(c, id, b);
  view_pop(c, v);
  view_unbind(bind);
  buf_printf(b, "; } _t%d; })", tr);
  return 1;
}

/* The variable at the bottom of a chain of String calls that each answer
   their receiver itself (BOPF_SELF on the builtin row: `s.insert(0, x)`,
   `s << x`), or -1: a mutator called on the chain's answer changes that
   variable's String. */
static int str_self_chain_root(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  switch (vp_one(nt, n, VPS_NONE | VP_PARENS_ONLY, &n)) {
  case VP1_NODE: break;
  case VP1_NIL: return -1;
  case VP1_MANY: return -1;
  }
  while (n >= 0 && nt_kind(nt, n) == NK_CallNode) {
    int r = nt_ref(nt, n, "receiver");
    const char *nm = nt_str(nt, n, "name");
    TyKind rt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    if (!nm || (rt != TY_STRING && rt != TY_STRBUF) || comp_builtin_kind_reopen_mi(c, TY_STRING, nm) >= 0 ||
        !(bop_answers_self(TY_STRING, nm, call_plain_argc(c, n), nt_ref(nt, n, "block") >= 0) & BOPF_SELF))
      return -1;
    switch (vp_one(nt, r, VPS_NONE | VP_PARENS_ONLY, &n)) {
    case VP1_NODE: break;
    case VP1_NIL: return -1;
    case VP1_MANY: return -1;
    }
    if (str_mut_var_recv(c, n) || sb_shadowed_reader(n)) return n;
  }
  return -1;
}

/* Is node n's value a String no other name can hold: an interpolation, a
   copy of a frozen literal (`+"abc"`, BOPF_SELF_CLASS), or a builtin String
   method that answers a String of its own (no SELF-like flag on its row,
   the String family's BSH_PURE share)? A mutator's change of it is seen by
   nobody.

   A yield receiver (a per-call-site-inlined builtin call on it, kilau
   K-035) answers the SAME question about the expression this specific
   site's block actually tails in -- the inlined copy substitutes that
   expression for `yield` one-for-one, so its freshness is the question's
   answer too, not a reason to decline outright the way every OTHER name
   in this file's dispatch treats a yield receiver (falling through to the
   unresolved-call gate). The lookup mirrors sp_yield_site_type's own
   (codegen_util.c): the current block's (g_block_id) own tail expression,
   across any level of yield-forwarding (`wrap { yield }`). */
int str_fresh_value(Compiler *c, int n) {
  const NodeTable *nt = c->nt;
  switch (vp_one(nt, n, VPS_NONE | VP_PARENS_ONLY, &n)) {
  case VP1_NODE: break;
  case VP1_NIL: return 0;
  case VP1_MANY: return 0;
  }
  if (nt_kind(nt, n) == NK_InterpolatedStringNode) return 1;
  if (nt_kind(nt, n) == NK_YieldNode) {
    int bbody = g_block_id >= 0 ? nt_ref(nt, g_block_id, "body") : -1;
    int bn = 0;
    const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
    if (bn <= 0 || !bb) return 0;
    int tail = bb[bn - 1];
    for (int k = 1; nt_kind(nt, tail) == NK_YieldNode && yield_block_out(k) >= 0; k++) {
      int fbody = nt_ref(nt, yield_block_out(k), "body");
      int fn = 0; const int *fb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fn) : NULL;
      if (fn <= 0 || !fb) return 0;
      tail = fb[fn - 1];
    }
    return nt_kind(nt, tail) == NK_YieldNode ? 0 : str_fresh_value(c, tail);
  }
  if (nt_kind(nt, n) != NK_CallNode || nt_ref(nt, n, "block") >= 0 || comp_ntype(c, n) != TY_STRING) return 0;
  int r = nt_ref(nt, n, "receiver");
  const char *nm = nt_str(nt, n, "name");
  if (r < 0 || !nm || comp_ntype(c, r) != TY_STRING || comp_builtin_kind_reopen_mi(c, TY_STRING, nm) >= 0)
    return 0;
  int self = bop_answers_self(TY_STRING, nm, call_plain_argc(c, n), 0);
  if (self & BOPF_SELF_CLASS) {
    switch (vp_one(nt, r, VPS_NONE | VP_PARENS_ONLY, &r)) {
    case VP1_NODE: return nt_kind(nt, r) == NK_StringNode;
    case VP1_NIL: return 0;
    case VP1_MANY: return 0;
    }
  }
  return (self == 0 || self == BOPF_COPY_CLASS) && !str_self_call(nt, n) && bop_share(TY_STRING, nm) == BSH_PURE;
}

/* String#dedup is -@, and sp_str_uminus_val raises under -@: a nil
   receiver raises under dedup (#9347). The receiver the nil fact says is
   never nil keeps the plain call. */
int emit_op_string_dedup(Compiler *c, const BopCtx *x, Buf *b) {
  int named = !recv_never_nil(c, x->recv);
  buf_printf(b, "sp_str_uminus_val(%s", named ? "sp_nil_recv_named(" : "");
  if (x->rtext) buf_puts(b, x->rtext);
  else emit_expr(c, x->recv, b);
  buf_printf(b, "%s)", named ? ", \"dedup\")" : "");
  return 1;
}

int emit_op_string_slice(Compiler *c, const BopCtx *x, Buf *b) {
  int id = x->id, recv = x->recv, argc;
  const int *argv = call_args(c->nt, id, &argc);
  if (argc == 1 && emit_string_slice_poly(c, id, argv[0], b)) return 1;
  int sb_asgn = str_mut_var_recv(c, recv) || sb_shadowed_reader(recv);
  /* `s.insert(0, "<").slice!(0, 2)` cuts s's String: the chain runs, in
     Ruby's order, then the slice! is the variable's own, which every arm
     below serves (reassigning it, or through a handle's shim). The answer
     of the chain is the same String, so the slice is the call's value. */
  if (!sb_asgn) {
    int root = str_self_chain_root(c, recv);
    if (root >= 0) {
      NodeTable *ntw = (NodeTable *)c->nt;
      int orig = nt_ref(ntw, id, "receiver");
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
      nt_node_set_ref(ntw, id, "receiver", root);
      emit_expr(c, id, b);
      nt_node_set_ref(ntw, id, "receiver", orig);
      buf_puts(b, "; })");
      return 1;
    }
  }
  /* slice!(start, len) on a String that is no assignable variable (sb_asgn
     false) answers the part it cuts, as slice!(i) does on any receiver
     below -- there is nowhere to write a mutated copy back to, so the
     change (if the receiver turns out mutable at runtime) is seen by
     nobody, same as the str_fresh_value case this used to require. A
     frozen literal still raises through sp_str_check_mutable below, the
     same way slice!(i) already does unconditionally; this arm no longer
     needs str_fresh_value to tell a literal/ternary/other-non-variable
     receiver apart from one that is provably fresh, since both get
     identical codegen once sb_asgn is false. */
  int span2 = argc == 2 && !sb_asgn && re_lit_index(c, argv[0]) < 0 &&
              comp_ntype(c, argv[0]) != TY_REGEX;
  if (argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
    int tp2 = ++g_tmp;
    buf_puts(b, "({ "); emit_str_frozen_check(c, recv, b);
    buf_printf(b, " const char *_t%d = ", tp2); emit_expr(c, argv[0], b);
    buf_printf(b, "; const char *_hit%d = (_t%d && ", tp2, tp2);
    emit_expr(c, recv, b);
    buf_puts(b, ") ? strstr("); emit_expr(c, recv, b);
    buf_printf(b, ", _t%d) : NULL;", tp2);
    if (sb_asgn) {
      buf_printf(b, " if (_hit%d) ", tp2);
      emit_expr(c, recv, b);
      /* sub would set `$~`, which slice! leaves alone; a program that
         never reads it has sub record nothing */
      if (g_reads_match_regs) {
        buf_printf(b, " = sp_str_remove_first("); emit_expr(c, recv, b);
        buf_printf(b, ", _t%d);", tp2);
      }
      else {
        buf_printf(b, " = sp_str_sub("); emit_expr(c, recv, b);
        buf_printf(b, ", _t%d, (&(\"\\xff\")[1]));", tp2);
      }
    }
    buf_printf(b, " _hit%d ? _t%d : (const char *)0; })", tp2, tp2);
    return 1;
  }
  if (argc == 1 && (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_REGEX)) {
    /* slice!(/re/): remove the first match, evaluate to it (or nil).
       sp_re_match fills sp_re_match_str with the matched run; the splice
       helper replaces it with the empty string. A Regexp held in a value
       is read once; a typed one after the receiver, a boxed one before it
       (emit_string_slice_poly holds it first). */
    int tm3 = ++g_tmp, ts3 = ++g_tmp;
    char pat[32];
    buf_printf(b, "({ const char *_t%d = ", ts3); emit_expr(c, recv, b);
    buf_printf(b, "; if (_t%d) sp_str_check_mutable(_t%d);", ts3, ts3);
    if (re_lit_index(c, argv[0]) >= 0) snprintf(pat, sizeof pat, "sp_re_pat_%d", re_lit_index(c, argv[0]));
    else {
      int tp3 = ++g_tmp;
      snprintf(pat, sizeof pat, "_t%d", tp3);
      buf_printf(b, " mrb_regexp_pattern *_t%d = ", tp3); emit_expr(c, argv[0], b); buf_puts(b, ";");
    }
    buf_printf(b, " sp_int _t%d = sp_re_match(%s, _t%d);"
                  " const char *_hit%d = _t%d >= 0 ? sp_re_match_str : NULL;",
               tm3, pat, ts3, tm3, tm3);
    if (sb_asgn) {
      buf_printf(b, " if (_hit%d) ", tm3);
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_splice_re(%s, _t%d, (&(\"\\xff\")[1]));", pat, ts3);
    }
    buf_printf(b, " _hit%d; })", tm3);
    return 1;
  }
  if (span2 || (argc == 1 && (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_RANGE))) {
    /* slice!(i) / slice!(range): the removed part (or nil), reassigning an
       lvalue receiver; a literal receiver just yields the removed part. */
    int to = ++g_tmp, tb2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp;
    /* rooted across the index or range, which may allocate */
    buf_printf(b, "({ const char *_t%d = ", to); emit_recv_rooted(c, recv, to, "SP_GC_ROOT_STR", b);
    buf_printf(b, "sp_str_check_mutable(_t%d);", to);   /* frozen -> FrozenError (#3003) */
    buf_printf(b, " sp_int _t%d = (sp_int)sp_str_length(_t%d); sp_int _t%d, _t%d;",
               tn2, to, tb2, tl2);
    /* slice!(range, n): Ruby has no two-argument overload that takes a
       Range first -- raises TypeError, after the receiver's own frozen
       check above (a frozen receiver with a Range first argument still
       raises FrozenError, not TypeError: CRuby checks mutability before
       argument shape) and after BOTH arguments' own side effects, in
       the same left-to-right order Ruby evaluates them in before
       raising. Neither argument is referenced anywhere else in this
       arm (the TypeError is unconditional once argc == 2 and argv[0]
       is a Range), so each needs its own explicit, discarded
       evaluation here -- the generic operand-hoisting pass ahead of
       this call only holds an argument it decides this arm's own code
       will read; CodeRabbit caught on review that leaving argv[0]
       unevaluated here (as an earlier version of this fix did) drops
       its side effect entirely, since the hoisting pass evidently
       does not always hold "the one argument not read downstream"
       either. Range is the ONLY argv[0] kind the TY_RANGE arm below
       answers for a plain index; argc == 1 is implied by reaching this
       `if` with a Range, since argc == 2 is only otherwise possible
       through span2, checked here first so the TY_RANGE arm no longer
       sees it. */
    if (argc == 2 && comp_ntype(c, argv[0]) == TY_RANGE) {
      buf_puts(b, " (void)("); emit_expr(c, argv[0], b); buf_puts(b, ");");
      buf_puts(b, " (void)("); emit_expr(c, argv[1], b); buf_puts(b, ");");
      buf_puts(b, " sp_raise_cls(\"TypeError\", \"no implicit conversion of Range into Integer\");"
                  " (const char *)0; })");
      return 1;
    }
    if (comp_ntype(c, argv[0]) == TY_RANGE) {
      int trg = ++g_tmp;
      buf_printf(b, " sp_Range _t%d = sp_range_ix(", trg); emit_expr(c, argv[0], b); buf_puts(b, ")");
      /* a beginless Range (first is INTPTR_MIN) starts at the beginning */
      buf_printf(b, "; _t%d = _t%d.first == INTPTR_MIN ? 0 : _t%d.first < 0 ? _t%d.first + _t%d : _t%d.first;",
                 tb2, trg, trg, trg, tn2, trg);
      /* an endless Range (last is INTPTR_MAX) runs to the end of the String */
      buf_printf(b, " _t%d = _t%d.last == INTPTR_MAX ? _t%d - _t%d :"
                    " (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                 tl2, trg, tn2, tb2, trg, trg, tn2, trg, tb2, trg);
      buf_printf(b, " if (_t%d < 0) _t%d = 0;", tl2, tl2);
    }
    else {
      buf_printf(b, " _t%d = ", tb2); emit_int_expr(c, argv[0], b);
      if (span2) { buf_printf(b, "; _t%d = ", tl2); emit_int_expr(c, argv[1], b); }
      else buf_printf(b, "; _t%d = 1", tl2);
      buf_printf(b, "; if (_t%d < 0) _t%d += _t%d;", tb2, tb2, tn2);
    }
    /* a Range may be empty at any position up to the end (`s.slice!(3..)`
       is ""); one index removes a character, so it must be inside */
    buf_printf(b, " const char *_t%d = NULL;"
                  " if (_t%d >= 0 && _t%d %s _t%d && _t%d >= 0) {"
                  " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d;"
                  " _t%d = sp_str_sub_range(_t%d, _t%d, _t%d);"
                  " SP_GC_ROOT_STR(_t%d);",
               tr2,
               tb2, tb2, span2 || comp_ntype(c, argv[0]) == TY_RANGE ? "<=" : "<", tn2, tl2,
               tl2, tn2, tb2, tl2, tn2, tb2,
               tr2, to, tb2, tl2, tr2);
    if (sb_asgn) {
      /* the head is rooted while the tail allocates */
      buf_printf(b, " { const char *_h = sp_str_sub_range(_t%d, 0, _t%d); SP_GC_ROOT_STR(_h); ", to, tb2);
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_concat(_h, sp_str_sub_range(_t%d, _t%d + _t%d, _t%d - _t%d - _t%d)); }",
                 to, tb2, tl2, tn2, tb2, tl2);
    }
    buf_printf(b, " } _t%d; })", tr2);
    return 1;
  }
  if (argc == 2 && (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_REGEX)) {
    /* slice!(/re/, n): remove the nth capture group of the first match and
       evaluate to it. sp_re_caps holds each group's byte span, so the removal
       is the group's own occurrence rather than the first textual one, which
       is a different character when the group repeats (#3543). A Regexp held
       in a value (a parameter, a local no literal resolves) is read once,
       right after the receiver, in Ruby's own left-to-right argument order --
       same place emit_string_slice_poly's one-arg sibling above reads it. */
    int ts = ++g_tmp, tn = ++g_tmp, th = ++g_tmp;
    char pat[32];
    /* rooted right after the receiver is evaluated: the Regexp and
       capture-index operands below can allocate (a non-literal Regexp or
       a method-call capture index), and a non-variable receiver (a fresh
       String nothing else holds, such as `(a + b).slice!(re, n)`) has
       nothing else keeping it alive across that (CodeRabbit, #10838). */
    buf_printf(b, "({ const char *_t%d = ", ts); emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT_STR(_t%d);", ts);
    buf_printf(b, " if (_t%d) sp_str_check_mutable(_t%d);", ts, ts);
    if (re_lit_index(c, argv[0]) >= 0) snprintf(pat, sizeof pat, "sp_re_pat_%d", re_lit_index(c, argv[0]));
    else {
      int tp3 = ++g_tmp;
      snprintf(pat, sizeof pat, "_t%d", tp3);
      buf_printf(b, " mrb_regexp_pattern *_t%d = ", tp3); emit_expr(c, argv[0], b); buf_puts(b, ";");
    }
    buf_printf(b, " sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
    buf_printf(b, "; const char *_t%d = sp_re_match(%s, _t%d) >= 0"
                  " ? (_t%d == 0 ? sp_re_match_str"
                  "    : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL)) : NULL;",
               th, pat, ts, tn, tn, tn, tn);
    if (sb_asgn) {
      buf_printf(b, " if (_t%d && _t%d >= 0 && _t%d <= 9) {"
                    " sp_int _b = sp_re_caps[2 * _t%d], _e = sp_re_caps[2 * _t%d + 1];"
                    " const char *_h = sp_str_byteslice(_t%d, 0, _b); SP_GC_ROOT_STR(_h); ",
                 th, tn, tn, tn, tn, ts);
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_str_concat(_h, sp_str_byteslice(_t%d, _e, (sp_int)sp_str_byte_len(_t%d) - _e)); }",
                 ts, ts);
    }
    buf_printf(b, " _t%d; })", th);
    return 1;
  }
  if (sb_asgn && argc == 2) {
    /* character-indexed splice, not byte-indexed, for a multibyte receiver (#3084) */
    int ti2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp;
    buf_puts(b, "({ "); emit_str_frozen_check(c, recv, b);
    buf_printf(b, " sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; sp_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
    buf_printf(b, "; sp_int _t%d = (sp_int)sp_str_length(", tn2);
    emit_expr(c, recv, b);
    buf_printf(b, "); const char *_t%d = NULL;"
                  " if (_t%d < 0) _t%d += _t%d;"
                  " if (_t%d >= 0 && _t%d <= _t%d && _t%d >= 0) {"
                  " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d;"
                  " _t%d = sp_str_sub_range(",
               tr2,
               ti2, ti2, tn2,
               ti2, ti2, tn2, tl2,
               tl2, tn2, ti2, tl2, tn2, ti2,
               tr2);
    emit_expr(c, recv, b);
    buf_printf(b, ", _t%d, _t%d); ", ti2, tl2);
    /* the removed part must outlive the three allocations that rebuild the receiver */
    buf_printf(b, "SP_GC_ROOT_STR(_t%d); ", tr2);
    /* and the head while the tail allocates */
    buf_puts(b, "const char *_h = sp_str_sub_range("); emit_expr(c, recv, b);
    buf_printf(b, ", 0, _t%d); SP_GC_ROOT_STR(_h); ", ti2);
    emit_expr(c, recv, b);
    buf_puts(b, " = sp_str_concat(_h, sp_str_sub_range(");
    emit_expr(c, recv, b);
    buf_printf(b, ", _t%d + _t%d, _t%d - _t%d - _t%d)); } _t%d; })",
               ti2, tl2, tn2, ti2, tl2, tr2);
    return 1;
  }
  return 0;
}
