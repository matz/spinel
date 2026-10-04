/* codegen_call_string.c -- emit_call_body's arms of String and Regexp.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

/* a Regexp literal's match / match? / ===, the match family on a String, gsub, and String#% */
int emit_call_regexp_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0) {
  /* regex literal match predicates (bool-returning, no MatchData/globals):
     /re/.match?(str[, pos])  and  str.match?(/re/[, pos]) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && sp_streq(name, "match?") && argc == 1) {
      /* /re/.match?(str): a match boolean that leaves $~ alone (CRuby) */
      if (a0 == TY_POLY) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, sp_poly_to_s(", rre); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
      else { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", rre); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")"); }  /* nil subject: no match */
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "===") && argc == 1) {
      /* /re/ === x updates the $~ registers, unlike match? */
      if (a0 == TY_STRING) { buf_printf(b, "(sp_re_match(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b); buf_puts(b, ") >= 0)"); }
      else { buf_printf(b, "sp_re_case_eq(sp_re_pat_%d, ", rre); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "match?") && argc == 2) {
      buf_printf(b, "sp_re_match_p_at(sp_re_pat_%d, ", rre); emit_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
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
      comp_ntype(c, argv[0]) != TY_REGEX && comp_ntype(c, argv[0]) != TY_POLY) {
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
    TyKind pt = comp_ntype(c, argv[0]);
    if (gre < 0 && pt != TY_REGEX && pt != TY_STRING && pt != TY_POLY) goto no_gsub_enum;
    int tsg = ++g_tmp, tpat = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = ", tsg);
    emit_expr(c, recv, b);
    buf_printf(b, "; SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", tsg, tpat);
    emit_boxed(c, argv[0], b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); "
                  "sp_enum_with_src(sp_Enumerator_new_from(sp_box_str_array(", tpat);
    if (gre >= 0) buf_printf(b, "sp_re_scan(sp_re_pat_%d, _t%d)", gre, tsg);
    else buf_printf(b, "sp_scan_boxed(_t%d, _t%d)", tsg, tpat);
    buf_printf(b, ")), sp_box_str(_t%d), sp_sprintf(\"%s(%%s)\", sp_poly_inspect(_t%d))); })",
               tsg, name, tpat);
    return 1;
  }
no_gsub_enum:
  /* Object receivers (incl. native-bound classes like StringScanner) dispatch
     their own match?/match methods; only string-ish receivers belong here. */
  if (recv >= 0 && argc >= 1 && rt != TY_SYMBOL && rt != TY_NIL && !ty_is_object(rt) &&
      (sp_streq(name, "match?") || sp_streq(name, "!~") || sp_streq(name, "=~") || sp_streq(name, "match"))) {
    int are = re_lit_index(c, argv[0]);
    /* a receiver of no known type (a bare name that resolves to nothing and
       raises NameError) is boxed: the tag checks below take it */
    int rpoly = rt == TY_POLY || rt == TY_UNKNOWN;
    /* a numeric receiver has no =~/!~/match?/match (Object#=~ was removed):
       raise NoMethodError rather than matching the number as a string. */
    if ((rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) &&
        (sp_streq(name, "=~") || sp_streq(name, "!~") ||
         sp_streq(name, "match?") || sp_streq(name, "match"))) {
      const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
      const char *dv9 = default_value_from_compiler(c, comp_ntype(c, id));
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
       (e.g. an element read out of an array that widened to poly), nil when it
       holds nil (NilClass#=~ is always nil); any other tag has no =~ (Object#=~
       was removed) -> NoMethodError, matching CRuby. */
    if (are >= 0 && sp_streq(name, "=~") && rpoly) {
      /* Self-contained statement-expression: this can appear in a pure
         expression position (an `if`/ternary condition) where a g_pre prelude
         would not be flushed and would splice a stray statement into the
         condition (#3187). */
      /* a shared String handle matches its live bytes, through the deref */
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
      buf_printf(b, "); (_t%d.tag == SP_TAG_STR ? sp_re_match_poly(sp_re_pat_%d, _t%d.v.s)"
                    " : _t%d.tag == SP_TAG_NIL ? sp_box_nil()"
                    " : sp_raise_nomethod(\"undefined method '=~' for poly\")); })",
                 tv, are, tv, tv);
      return 1;
    }
    /* poly receiver `poly !~ /re/`: nil !~ is always true, a string tests the
       negated match; any other tag has no =~ so !~ raises NoMethodError. A
       non-poly (string) receiver keeps the direct negated-match emit. */
    if (are >= 0 && sp_streq(name, "!~") && rpoly) {
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
      buf_printf(b, "); (_t%d.tag == SP_TAG_STR ? sp_re_match(sp_re_pat_%d, _t%d.v.s) < 0"
                    " : _t%d.tag == SP_TAG_NIL ? 1"
                    " : (sp_raise_nomethod(\"undefined method '=~' for poly\"), 0)); })",
                 tv, are, tv, tv);
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
    if (are >= 0 && sp_streq(name, "match?") && rpoly) {
      int tv = ++g_tmp;
      /* a shared-string handle is a String (#4279) */
      buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
      buf_puts(b, ")");
      buf_printf(b, "; const char *_s%d = _t%d.tag == SP_TAG_SYM ? sp_sym_to_s((sp_sym)_t%d.v.i) : _t%d.v.s;",
                 tv, tv, tv, tv);
      buf_printf(b, " (sp_bool)((_t%d.tag == SP_TAG_STR || _t%d.tag == SP_TAG_SYM) ? ", tv, tv);
      if (argc == 1) buf_printf(b, "sp_re_match_p(sp_re_pat_%d, _s%d)", are, tv);
      else {
        buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, _s%d, ", are, tv);
        emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      buf_printf(b, " : (sp_raise_nomethod(sp_sprintf(\"undefined method 'match?' for an instance of %%s\","
                    " sp_poly_class_name(_t%d))), 0)); })", tv);
      return 1;
    }
    if (are >= 0 && sp_streq(name, "match?")) {
      if (argc == 1) { buf_printf(b, "sp_re_match_p(sp_re_pat_%d, ", are); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
      buf_printf(b, "sp_str_re_match_p_at(sp_re_pat_%d, ", are); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
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
      return 1;
    }
    if (are >= 0 && sp_streq(name, "match")) {
      if (argc == 1) {
        /* recv is the subject string; emit_str_expr coerces a poly/nilable
           receiver (`@message.subject.match(/re/)`) to the const char* slot */
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", are); emit_str_expr(c, recv, b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", are); emit_str_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
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
      buf_printf(b, "({ const char *_t%d = ", ts); emit_str_expr(c, argv[0], b);
      buf_printf(b, "; mrb_regexp_pattern *_t%dp = re_compile(_t%d, (int64_t)(_t%d ? sp_str_byte_len(_t%d) : 0), 0); ",
                 ts, ts, ts, ts);
      buf_printf(b, "%s(_t%dp, ", fn, ts); emit_expr(c, recv, b); buf_puts(b, "); })");
      return 1;
    }
  }
  /* /re/.match(str) and /re/.match(str, pos) */
  {
    int rre = re_lit_index(c, recv);
    if (rre >= 0 && sp_streq(name, "match") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
      /* /re/.match(str) { |m| body }: the same block form String#match already
         had -- yield the MatchData on a hit and evaluate to the block's value,
         nil on a miss. Without the arm the MatchData itself was the value and
         the block never ran (#3642). */
      int mblk = nt_ref(nt, id, "block");
      const char *mp0 = block_param_name(c, mblk, 0);
      const char *mp0r = mp0 ? rename_local(mp0) : NULL;
      int mbody = nt_ref(nt, mblk, "body");
      int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
      int tm = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata(sp_re_pat_%d, ", tm, rre);
      emit_str_expr(c, argv[0], b);
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
      return 1;
    }
    if (rre >= 0 && sp_streq(name, "match") && (argc == 1 || argc == 2)) {
      /* the subject is a string; emit_str_expr coerces a poly/nullable-string
         value (e.g. a `string?` attr read) to const char*, which emit_expr would
         leave as an sp_RbVal into sp_re_matchdata's const char* slot (#3219). */
      if (argc == 1) {
        buf_printf(b, "sp_re_matchdata(sp_re_pat_%d, ", rre); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        buf_printf(b, "sp_re_matchdata_at(sp_re_pat_%d, ", rre); emit_str_expr_nilable(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
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
      const char *a0ty = nt_type(nt, argv[0]);
      int is_interp_arg = a0ty && sp_streq(a0ty, "InterpolatedRegularExpressionNode");
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
      int is_regex_val_arg = !is_interp_arg && argc >= 1 && comp_ntype(c, argv[0]) == TY_REGEX
                             && a0nt && !sp_streq(a0nt, "RegularExpressionNode")
                             && !ty_is_object(rt);
      if (is_interp_arg || is_regex_val_arg) {
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
              (sp_streq(name, "=~") || sp_streq(name, "!~") ||
               sp_streq(name, "match?") || sp_streq(name, "match"))) {
            const char *tn9 = rt == TY_FLOAT ? "Float" : "Integer";
            const char *dv9 = default_value_from_compiler(c, comp_ntype(c, id));
            buf_puts(b, "((void)("); emit_expr(c, recv, b);
            buf_printf(b, "), (sp_raise_cls(\"NoMethodError\", \"undefined method '%s' for an instance of %s\"), %s))",
                       name, tn9, dv9 ? dv9 : "sp_box_nil()");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match?") && (argc == 1 || argc == 2)) {
            /* A symbol receiver matches over its name, so feed the runtime
               pattern the symbol's string rather than the raw sp_sym. */
            if (rt == TY_SYMBOL) { buf_printf(b, "sp_re_match_p(%s, sp_sym_to_s(", rp.p); emit_expr(c, recv, b); buf_puts(b, "))"); }
            /* emit_str_expr, not emit_expr: a poly receiver (a String read out
               of a widened container) needs coercing into the const char* slot
               the same way the precompiled-literal arms do it (#3389). */
            else if (argc == 1) { buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_str_expr(c, recv, b); buf_puts(b, ")"); }
            /* match?(re, pos) starts the scan at pos */
            else {
              buf_printf(b, "sp_str_re_match_p_at(%s, ", rp.p); emit_str_expr(c, recv, b);
              buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
            }
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
          if (sp_streq(name, "!~")) {
            buf_printf(b, "(sp_re_match(%s, ", rp.p); emit_expr(c, recv, b); buf_puts(b, ") < 0)");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && (argc == 1 || argc == 2)) {
            /* emit_str_expr, not emit_expr: a poly receiver needs coercing into
               the const char* slot, as the precompiled-literal arms do (#3389) */
            if (argc == 1) { buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_str_expr(c, recv, b); buf_puts(b, ")"); }
            else {
              buf_printf(b, "sp_re_matchdata_at(%s, ", rp.p); emit_str_expr(c, recv, b);
              buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
            }
            free(rp.p); return 1;
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
            else { buf_printf(b, "sp_re_match_p(%s, ", rp.p); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")"); }  /* nil subject: no match */
            free(rp.p); return 1;
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
              buf_printf(g_pre, "if (_t%d.tag != SP_TAG_STR && _t%d.tag != SP_TAG_NIL) sp_raise_no_str_conversion(_t%d);\n", tv, tv, tv);
              buf_printf(b, "sp_re_match_poly(%s, _t%d.v.s)", rp.p, tv);
            }
            else if (a0 == TY_NIL) {
              /* nil is the one non-String `re =~ x` accepts: it answers nil
                 rather than raising (#3633) */
              buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
              free(rp.p); return 1;
            }
            else {
              /* statically known non-string: always raises TypeError */
              const char *tn = (a0 == TY_INT) ? "Integer" : (a0 == TY_FLOAT) ? "Float"
                             : (a0 == TY_BOOL) ? "true/false" : "Object";
              buf_printf(b, "((void)(");
              emit_expr(c, argv[0], b);
              buf_printf(b, "), sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into String\"), sp_box_nil())", tn);
            }
            free(rp.p); return 1;
          }
          if (sp_streq(name, "!~") && argc == 1 && a0 == TY_STRING) {
            buf_printf(b, "(sp_re_match(%s, ", rp.p); emit_expr(c, argv[0], b); buf_puts(b, ") < 0)");
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && argc == 1 && nt_ref(nt, id, "block") >= 0) {
            /* the block form: yield the MatchData on a hit, evaluate to the
               block's value, nil on a miss (#3642) */
            int mblk = nt_ref(nt, id, "block");
            const char *mp0 = block_param_name(c, mblk, 0);
            const char *mp0r = mp0 ? rename_local(mp0) : NULL;
            int mbody = nt_ref(nt, mblk, "body");
            int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
            int tm = ++g_tmp, tr2 = ++g_tmp;
            buf_printf(b, "({ sp_MatchData *_t%d = sp_re_matchdata(%s, ", tm, rp.p);
            emit_str_expr(c, argv[0], b);
            buf_printf(b, "); sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); if (_t%d) { ",
                       tr2, tr2, tm);
            if (mp0r) buf_printf(b, "lv_%s = _t%d; ", mp0r, tm);
            for (int j = 0; j + 1 < mbn; j++) { emit_stmt(c, mbb[j], b, 0); }
            if (mbn > 0) { buf_printf(b, "_t%d = ", tr2); emit_boxed(c, mbb[mbn - 1], b); buf_puts(b, "; "); }
            buf_printf(b, "} _t%d; })", tr2);
            free(rp.p); return 1;
          }
          if (sp_streq(name, "match") && (argc == 1 || argc == 2)) {
            /* the subject can arrive boxed -- one call site passing an
               untyped block param is enough -- so unbox it into the const
               char * slot the way match? and =~ already do */
            if (argc == 1) { buf_printf(b, "sp_re_matchdata(%s, ", rp.p); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")"); }
            else { buf_printf(b, "sp_re_matchdata_at(%s, ", rp.p); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
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
      return 1;
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
      return 1;
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
       (a one-element list) -- the distinction is only known at runtime. */
    if (at == TY_POLY) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", sp_format_args("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if (at == TY_UNKNOWN && emit_str_format_untyped_array(c, recv, argv[0], fck, b)) return 1;
    /* a single non-array scalar argument formats as a one-element array
       (nil renders empty for %s; Rational/Complex coerce inside the
       formatter's numeric directives) */
    if (at == TY_INT || at == TY_FLOAT || at == TY_STRING || at == TY_SYMBOL ||
        at == TY_NIL || at == TY_BOOL || at == TY_RATIONAL || at == TY_COMPLEX) {
      buf_puts(b, "sp_str_format_polyarr("); emit_expr(c, recv, b);
      buf_puts(b, ", ({ sp_PolyArray *_fa = sp_PolyArray_new(); sp_PolyArray_push(_fa, ");
      emit_boxed(c, argv[0], b); buf_puts(b, "); _fa; }))");
      return 1;
    }
  }
  return 0;
}

/* Regexp's class methods: last_match, try_convert, timeout, escape / quote, union, linear_time? and compile */
int emit_call_regexp_class_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* Regexp.last_match -> the last MatchData ($~), or nil */
  if (recv >= 0 && argc == 0 && sp_streq(name, "last_match") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    buf_puts(b, "sp_re_last_matchdata()");
    return 1;
  }
  /* Regexp.try_convert(x) -> x if it is a Regexp, else nil */
  if (recv >= 0 && argc == 1 && sp_streq(name, "try_convert") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
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
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (argc == 0) { buf_puts(b, "sp_box_nil()"); return 1; }
    if (argc == 1) { buf_puts(b, "("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); return 1; }
  }
  /* Regexp.last_match(n) -> nth capture group string, or whole match for n=0 */
  if (recv >= 0 && argc == 1 && sp_streq(name, "last_match") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    const char *aty = nt_type(nt, argv[0]);
    if (aty && sp_streq(aty, "IntegerNode")) {
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
    if (aty && (sp_streq(aty, "StringNode") || sp_streq(aty, "SymbolNode"))) {
      buf_puts(b, "sp_re_named_capture(sp_re_last_pat, ");
      if (sp_streq(aty, "SymbolNode")) emit_str_literal(b, nt_str(nt, argv[0], "value"));
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
    buf_printf(g_pre, "sp_int _t%d = ", tv); emit_int_expr(c, argv[0], g_pre); buf_puts(g_pre, ";\n");
    buf_printf(b, "(_t%d == 0 ? sp_re_match_str : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL))",
               tv, tv, tv, tv);
    return 1;
  }
  /* Regexp.escape / Regexp.quote -> escape special regex characters */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "escape") || sp_streq(name, "quote")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    TyKind _re_at = comp_ntype(c, argv[0]);
    if (_re_at == TY_POLY) { buf_puts(b, "sp_re_escape(sp_poly_to_s("); emit_expr(c, argv[0], b); buf_puts(b, "))"); }
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
     elements. A runtime Regexp value has no recoverable source (patterns compile
     to bytecode), so that lone form still loud-rejects. */
  if (recv >= 0 && argc >= 0 && sp_streq(name, "union") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    /* `Regexp.union([a, b])`: a lone Array argument supplies the operands. */
    const int *ops = argv; int nops = argc;
    if (argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode"))
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
      TyKind uat = comp_ntype(c, ua);
      if (uat == TY_POLY_ARRAY || uat == TY_STR_ARRAY || (splat && uat != TY_UNKNOWN)) {
        buf_puts(b, "sp_re_union_array(");
        if (uat == TY_STR_ARRAY) { buf_puts(b, "sp_StrArray_to_poly_fmt("); emit_expr(c, ua, b); buf_puts(b, ")"); }
        else if (uat == TY_POLY_ARRAY) emit_expr(c, ua, b);
        /* any other splatted value -- boxed, a scalar, nil, a typed array --
           is the Array its splat makes, and each element is checked at run
           time (a non-String, non-Regexp one raises TypeError) */
        else { buf_puts(b, "sp_poly_to_poly_array(sp_splat_to_array("); emit_boxed(c, ua, b); buf_puts(b, "))"); }
        buf_puts(b, ")");
        return 1;
      }
    }
    /* A single Regexp operand is returned unchanged (CRuby keeps its source and
       flags verbatim, no option-group wrapper). */
    if (nops == 1 && re_lit_src(c, ops[0]) && emit_regex_pat_to_buf(c, ops[0], b))
      return 1;
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
        TyKind at = comp_ntype(c, ops[i]);
        if (at != TY_STRING && at != TY_POLY)
          unsupported(c, id, "Regexp.union operand without a compile-time source (runtime Regexp or non-String value)");
        if (at == TY_POLY) { buf_puts(&ab, "sp_re_escape(sp_poly_to_s("); emit_expr(c, ops[i], &ab); buf_puts(&ab, "))"); }
        else { buf_puts(&ab, "sp_re_escape("); emit_expr(c, ops[i], &ab); buf_puts(&ab, ")"); }
      }
      emit_indent(g_pre, g_indent);
      if (i == 0) buf_printf(g_pre, "const char *_t%d = %s;\n", ts, ab.p ? ab.p : "\"\"");
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
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RegularExpressionNode"))
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
  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && argc >= 1 && sp_streq(name, "compile") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    /* given a Regexp, the pattern is copied: hand back the pattern itself
       rather than reading its pointer as a source string (#3686) */
    if (comp_ntype(c, argv[0]) == TY_REGEX) { emit_expr(c, argv[0], b); return 1; }
    int tp = ++g_tmp, ts = ++g_tmp;
    /* See the Regexp.new path: emit the pattern into a local buffer so an
       interpolated arg's embedded-call arg roots land in g_pre as whole
       statements before this temp's decl, not inside its initializer. */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_expr(c, argv[0], &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    Buf flagbuf; memset(&flagbuf, 0, sizeof flagbuf);
    emit_re_opts_flags(c, argc, argv, &flagbuf);   /* (#3055) */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)(_t%d ? sp_str_byte_len(_t%d) : 0), %s);\n",
               tp, ts, ts, ts, flagbuf.p ? flagbuf.p : "0");
    free(flagbuf.p);
    buf_printf(b, "_t%d", tp);
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
      char none_hi[64];
      snprintf(none_hi, sizeof none_hi, "(sp_int)sp_str_length(_t%d)", t);
      buf_printf(b, "); sp_str_sub_range_r(_t%d, ", t);
      if (lo >= 0) emit_int_expr_bound(c, lo, "0", b); else buf_puts(b, "0");
      buf_puts(b, ", ");
      if (hi >= 0) { emit_int_expr_bound(c, hi, none_hi, b); buf_printf(b, ", %d); })", excl); }
      else buf_printf(b, "(sp_int)sp_str_length(_t%d), 0); })", t);
      return 1;
    }
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY)) {
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
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) {
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
    int body = nt_ref(nt, block, "body");
    const char *p0 = block_param_name(c, block, 0); if (p0) p0 = rename_local(p0);
    int ts = ++g_tmp, ti = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    int is_line = sp_streq(name, "each_line") || sp_streq(name, "lines");
    int is_byte = sp_streq(name, "each_byte") || sp_streq(name, "bytes");
    int is_cp = sp_streq(name, "codepoints");
    Scope *cs_ech = p0 ? comp_scope_of(c, id) : NULL;
    LocalVar *clv_ech = (p0 && cs_ech) ? scope_local(cs_ech, p0) : NULL;
    int p0_box_poly_ech = clv_ech && clv_ech->type == TY_POLY;
    /* The loop below reads the receiver on every turn -- as its bound, and as
       the string it takes the next character or byte out of -- and the block
       between two turns may allocate. A temporary receiver (`array.join.
       each_char { }`) has no other holder at that point, so the temp is a
       root, the way the array iterators root their hoisted receiver. */
    buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); ", ts, rb.p ? rb.p : "", ts); free(rb.p);
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
      /* a separator argument splits on it, the way the blockless enumerator
         form already does (#3594) */
      int eline_sep = (argc == 1 && argv && comp_ntype(c, argv[0]) == TY_STRING) ? argv[0] : -1;
      if (eline_sep >= 0) {
        buf_printf(b, "sp_StrArray *_t%d = sp_str_lines_sep(_t%d, ", tl, ts);
        emit_expr(c, eline_sep, b);
        buf_puts(b, "); ");
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
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d)); ", p0, tl, ti);
        else buf_printf(b, "lv_%s = sp_StrArray_get(_t%d, _t%d); ", p0, tl, ti);
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
      buf_printf(b, "for (sp_int _t%d = 0; _t%d < sp_str_length(_t%d); _t%d++) { ", ti, ti, ts, ti);
      if (p0) {
        if (p0_box_poly_ech) buf_printf(b, "lv_%s = sp_box_str(sp_str_char_at_or_nil(_t%d, _t%d)); ", p0, ts, ti);
        else buf_printf(b, "lv_%s = sp_str_char_at_or_nil(_t%d, _t%d); ", p0, ts, ti);
      }
    }
    emit_iter_loop_stmts(c, body, b, 0);
    if (p0 && tsv_ech > 0) buf_printf(b, " lv_%s = _t%d;", p0, tsv_ech);
    buf_printf(b, " } _t%d; })", ts);
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
      (sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
      comp_ntype(c, argv[0]) == TY_SYMBOL) {
    buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == (");
    emit_expr(c, argv[0], b); buf_puts(b, "))");
    return 1;
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
    int plain_args = 1;
    for (int i = 0; i < argc; i++) {
      NodeKind ak = nt_kind(nt, argv[i]);
      if (ak == NK_SplatNode || ak == NK_KeywordHashNode || ak == NK_BlockArgumentNode) plain_args = 0;
    }
    if (plain_args) {
      const char *sfx = argc == 1 ? case_map_suffix(c, argc, argv) : "";
      int tv = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tv);
      if (!*sfx) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR || _t%d.tag == SP_TAG_SYM || sp_poly_is_strbuf(_t%d)) "
                      "sp_case_opts_check(%d, (sp_RbVal[]){", tv, tv, tv, argc);
        for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); emit_boxed(c, argv[i], b); }
        buf_printf(b, "}, %s, _t%d); ", x->op->arg, tv);
      }
      buf_printf(b, "sp_poly_case_conv(_t%d, sp_str_%s%s, \"%s\"); })", tv, name, sfx, name);
      return 1;
    }
  }
  return 0;
}
