/* codegen_call_recv.c -- receiver-typed method-call emitters (array/hash/
   scalar/object/value/range/poly), split out of codegen_call.c. Pure code
   movement, no logic change. */

#include "codegen_internal.h"

/* Object's identity protocol, text form (defined with its node form at the end of this file). */
static void emit_native_object_protocol_text(Compiler *c, const char *name, TyKind rt, const char *r, TyKind at, const char *a, Buf *b);

/* Receiver type with the empty-container-literal coercion the inference
   layer applies (`[].m` -> poly array, `{}.m` -> str-keyed poly hash, the
   same C type the emitters build for the bare literals): comp_ntype answers
   UNKNOWN for them, which stranded direct calls like `{}.size`. */
/* The node whose poly receiver is being re-dispatched as a poly array. */
static int g_poly_redispatch_id = -1;

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
     _t<torig> : element count BEFORE compaction (sp_int)
     _t<twp>   : element count AFTER compaction (sp_int) == survivor count
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

/* String#<< and String#concat take an Integer as a CODEPOINT, not a string:
   `s << 100` appends "d". Sent through the string slot, the integer reached
   sp_str_concat as a char pointer and the program died (#3544). */
static void emit_str_append_arg(Compiler *c, int arg, Buf *b) {
  if (comp_ntype(c, arg) == TY_INT) {
    buf_puts(b, "sp_int_codepoint_to_str("); emit_expr(c, arg, b); buf_puts(b, ")");
    return;
  }
  emit_str_expr(c, arg, b);
}

/* One member read by an arbitrary key: an index (negative counts from the
   end), a Symbol or a String naming a member. CRuby raises for a key no member
   matches, so the miss is IndexError / NameError rather than nil. `rtxt` names
   a temp already holding the receiver. */
static void emit_struct_member_by_key(Compiler *c, ClassInfo *sc, const char *rtxt,
                                      int key, int int_only, int nil_on_miss, Buf *b) {
  int tk = ++g_tmp, tk0 = ++g_tmp, tr = ++g_tmp;
  buf_printf(b, "({ sp_RbVal _t%d = ", tk);
  emit_boxed(c, key, b);
  buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
  /* Struct#values_at takes offsets only, unlike #[] / #dig */
  if (int_only)
    buf_printf(b, " if (_t%d.tag != SP_TAG_INT) sp_raise_cls(\"TypeError\","
                  " sp_sprintf(\"no implicit conversion of %%s into Integer\","
                  " sp_poly_class_name(_t%d)));", tk, tk);
  buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
             tk, tk, tk, tk, sc->nivars);
  buf_printf(b, " sp_RbVal _t%d = sp_box_nil();", tr);
  for (int i = 0; i < sc->nivars; i++) {
    buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%dLL))"
                  "||sp_rbval_eql_key(_t%d,sp_box_str(\"%s\"))){ _t%d = ",
               tk, comp_sym_intern(c, sc->ivars[i] + 1), tk, (long long)i,
               tk, sc->ivars[i] + 1, tr);
    char fld[300]; snprintf(fld, sizeof fld, "%s->iv_%s", rtxt, iv_c(sc->ivars[i] + 1));
    emit_boxed_text(c, sc->ivar_types[i], fld, b);
    buf_puts(b, ";}\nelse");
  }
  /* #dig answers nil for a key no member matches, where #[] raises (#3892) */
  if (nil_on_miss) buf_printf(b, " { (void)_t%d; } _t%d; })", tk0, tr);
  else
    buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                  " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                  " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                  " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                  " } _t%d; })",
               tk0, sc->nivars, tk0, tk0, tk0, tr);
}

/* 1 when the node is a user object whose class compares -- defines ==, ===
   or <=> (a Comparable includer) -- so a typed container's lookup would have
   to call it, and the typed slot has no room for the object. The arms refuse
   these at compile time, naming the case, rather than hand the C compiler the
   pointer. */
static int value_obj_compares(Compiler *c, int node) {
  TyKind t = comp_ntype(c, node);
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && (comp_method_in_chain(c, cid, "==", NULL) >= 0 ||
                      comp_method_in_chain(c, cid, "===", NULL) >= 0 ||
                      comp_method_in_chain(c, cid, "<=>", NULL) >= 0);
}

/* 1 when a value of the node's static kind can never be == to an element
   of kind `ek`: a String searched for in an Integer Array, a Symbol in a
   String Array, nil in either. CRuby compares and finds nothing -- Array#delete
   answers nil, count 0, all? false -- where the raw value in the typed slot
   stopped the C build. Integer and Float compare equal across kinds, so they
   are never a static miss of each other; a user object converts nothing
   here, and its class may define == (or ===, which the predicates use), so
   it stays on the comparing path when it does. Also the value slots that
   compare the same way: Hash#value?, Range#include? and #eql?. */
static int value_kind_misses(Compiler *c, int node, TyKind ek) {
  TyKind t = comp_ntype(c, node);
  if (t == ek || t == TY_POLY || t == TY_UNKNOWN) return 0;
  int num = ek == TY_INT || ek == TY_FLOAT || ek == TY_BIGINT;
  if (num && (t == TY_INT || t == TY_FLOAT || (t == TY_BIGINT && ek != TY_INT))) return 0;
  if (ek == TY_STRING && (t == TY_STRING || t == TY_STRBUF)) return 0;
  if (!num && ek != TY_STRING) return 0;
  if (ty_is_object(t)) {
    /* a class defining <=> is a Comparable includer, whose == the module
       supplies (the same reading as respond_to?'s) */
    int cid = ty_object_class(t);
    return cid >= 0 && comp_method_in_chain(c, cid, "==", NULL) < 0 &&
           comp_method_in_chain(c, cid, "===", NULL) < 0 &&
           comp_method_in_chain(c, cid, "<=>", NULL) < 0;
  }
  if (ek == TY_INT && t == TY_BIGINT) return 1;  /* no sp_int equals a Bignum */
  return t == TY_NIL || t == TY_BOOL || t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
         t == TY_SYMBOL || t == TY_STRING || t == TY_STRBUF || t == TY_RANGE ||
         t == TY_FLOAT_RANGE || t == TY_STR_RANGE || t == TY_TIME || t == TY_REGEX ||
         ty_is_array(t) || ty_is_hash(t);
}

/* The direct call of the container conversion obj_container_conv found:
   the compiled #to_ary / #to_hash of the defining class on the operand. */
static void emit_obj_container_conv(Compiler *c, int node, int def, const char *conv, Buf *b) {
  TyKind t = comp_ntype(c, node);
  buf_printf(b, "sp_%s_%s(", c->classes[def].c_name, mc(conv));
  if (!comp_ty_value_obj(c, t)) buf_printf(b, "(sp_%s *)", c->classes[def].c_name);
  buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, "))");
}

/* Array#product's operand as an Array: an array passes; an object converts
   through #to_ary; any other class is CRuby's TypeError. Answers the Array
   kind the text is typed as. */
static TyKind emit_product_operand(Compiler *c, int node, TyKind at, Buf *b) {
  int def = -1;
  TyKind k = obj_container_conv(c, at, "to_ary", &def);
  if (k != TY_UNKNOWN) { emit_obj_container_conv(c, node, def, "to_ary", b); return k; }
  const char *cn = conv_cls_name_of(c, at);
  if (cn && !ty_is_array(at) && at != TY_POLY_ARRAY) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Array\"); (sp_PolyArray *)0; })", cn);
    return TY_POLY_ARRAY;
  }
  emit_expr(c, node, b);
  return at;
}

/* The class a builtin type answers to #class, for a "no implicit conversion of
   X into Array" message. Only the kinds conv_to_ary_impossible admits. */
static const char *conv_builtin_class_name(TyKind t) {
  if (t == TY_STRING || t == TY_STRBUF) return "String";
  if (t == TY_INT || t == TY_BIGINT) return "Integer";
  if (t == TY_FLOAT) return "Float";
  if (t == TY_SYMBOL) return "Symbol";
  if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) return "Range";
  if (t == TY_PROC) return "Proc";
  if (t == TY_TIME) return "Time";
  if (ty_is_hash(t)) return "Hash";
  return "Object";
}
/* True for a builtin type that certainly has no #to_ary, so an Array method
   taking "something Array-like" can say so at compile time rather than reach
   an arm that cannot serve it. Deliberately excludes TY_POLY / TY_UNKNOWN (may
   be an array at run time) and every OBJECT type (a user class may define
   #to_ary, which CRuby honours). */
static int conv_to_ary_impossible(TyKind t) {
  return t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
         t == TY_FLOAT || t == TY_SYMBOL || t == TY_PROC || t == TY_TIME ||
         t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE ||
         ty_is_hash(t);
}

/* The shared-mutable shim over an IVAR receiver, in expression position. The
   local form renames the slot so the value arm's reads and its write-back both
   land on a plain shadow; an ivar has no name to rename, so the shadow is
   published to the ivar emitter instead and the same re-run works unchanged.
   `rerun` is the emitter whose arms are being borrowed (#4363). */
static int sb_iv_expr_shim(Compiler *c, int id, int recvS, Buf *b,
                           int (*rerun)(Compiler *, int, Buf *)) {
  const NodeTable *nt = c->nt;
  if (strbuf_local_name(c, recvS)) return 0;
  if (nt_kind(nt, recvS) != NK_InstanceVariableReadNode || g_sb_iv_name) return 0;
  char srefI[1024];
  int icid = strbuf_ivar_owner(c, recvS);
  const char *ivn = nt_str(nt, recvS, "name");
  if (!ivn || icid < 0 || !strbuf_slot_ref(c, recvS, srefI, sizeof srefI)) return 0;
  int tH = ++g_tmp;
  Buf armb; memset(&armb, 0, sizeof armb);
  snprintf(g_sb_iv_repl, sizeof g_sb_iv_repl, "lv__sb%d", tH);
  g_sb_iv_name = ivn; g_sb_iv_cid = icid;
  int handled = rerun(c, id, &armb);
  g_sb_iv_name = NULL; g_sb_iv_cid = -1;
  if (!handled) { free(armb.p); return 0; }
  TyKind resty = comp_ntype(c, id);
  buf_printf(b, "({ sp_String *_t%d = %s;"
                " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                " SP_GC_ROOT(lv__sb%d); ",
             tH, srefI, tH, tH, tH, tH, tH);
  emit_ctype(c, resty == TY_UNKNOWN || resty == TY_VOID ? TY_STRING : resty, b);
  buf_printf(b, " _res%d = %s;", tH, armb.p ? armb.p : "0");
  free(armb.p);
  buf_printf(b, " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })", tH, tH, tH);
  return 1;
}

int emit_array_call(Compiler *c, int id, Buf *b) {
  /* The variadic Array mutators accept zero elements and return the receiver
     unchanged; every arm below is written for argc >= 1, so a no-argument call
     fell through to the unsupported-call refusal (#3340). */
  {
    const NodeTable *ntZ = c->nt;
    const char *nmZ = nt_str(ntZ, id, "name");
    int recvZ = nt_ref(ntZ, id, "receiver");
    int aZ = nt_ref(ntZ, id, "arguments"); int acZ = 0;
    if (aZ >= 0) nt_arr(ntZ, aZ, "arguments", &acZ);
    if (nmZ && recvZ >= 0 && acZ == 0 && nt_ref(ntZ, id, "block") < 0 &&
        ty_is_array(comp_ntype(c, recvZ)) &&
        (sp_streq(nmZ, "push") || sp_streq(nmZ, "append") ||
         sp_streq(nmZ, "concat") || sp_streq(nmZ, "unshift") ||
         sp_streq(nmZ, "prepend"))) {
      emit_expr(c, recvZ, b);
      return 1;
    }
    /* `zip(*xs)` / `product(*xs)`: the splat spreads across the ARGUMENT LIST,
       one operand per element, and its length is only known at run time. Every
       arm below reads a splat as a SINGLE operand, so zip handed an array where
       a value was expected and stopped the C build, and product refused the
       shape outright rather than answer wrongly (#4322, #4323). Build the
       operand list here -- a splat contributes each of its elements, anything
       else contributes itself -- and hand it to the variadic runtime. */
    if (nmZ && recvZ >= 0 && acZ >= 1 && nt_ref(ntZ, id, "block") < 0 &&
        (sp_streq(nmZ, "zip") || sp_streq(nmZ, "product")) &&
        (ty_is_array(comp_ntype(c, recvZ)) || comp_ntype(c, recvZ) == TY_POLY)) {
      const int *avZ = nt_arr(ntZ, aZ, "arguments", &acZ);
      int splZ = 0;
      for (int ai = 0; ai < acZ; ai++)
        if (nt_kind(ntZ, avZ[ai]) == NK_SplatNode) { splZ = 1; break; }
      if (splZ) {
        int tops = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tops, tops);
        for (int ai = 0; ai < acZ; ai++) {
          if (nt_kind(ntZ, avZ[ai]) == NK_SplatNode) {
            int sx = nt_ref(ntZ, avZ[ai], "expression");
            int tsp = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = sp_enum_items_from(", tsp);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);"
                          " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                          " sp_PolyArray_push(_t%d, _t%d->data[_t%d]); }",
                       tsp, tsi, tsi, tsp, tsi, tops, tsp, tsi);
          }
          else {
            buf_printf(b, " sp_PolyArray_push(_t%d, ", tops);
            emit_boxed(c, avZ[ai], b);
            buf_puts(b, ");");
          }
        }
        buf_printf(b, " sp_poly_%s_n(", sp_streq(nmZ, "zip") ? "zip" : "product");
        emit_boxed(c, recvZ, b);
        buf_printf(b, ", _t%d); })", tops);
        return 1;
      }
    }
    /* zip with nothing to zip against: each element alone in a one-element
       array. Every zip arm below is written for argc >= 1 (#3612). */
    if (nmZ && recvZ >= 0 && acZ == 0 && nt_ref(ntZ, id, "block") < 0 &&
        sp_streq(nmZ, "zip") && ty_is_array(comp_ntype(c, recvZ))) {
      buf_puts(b, "sp_poly_zip_none(");
      emit_boxed(c, recvZ, b);
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Shared-mutable shim, value position (#3227): same shadow-copy re-entry
     as emit_array_mutate_stmt's -- the existing arm computes the value and
     reassigns the shadow, then the handle's buffer swaps in place. */
  {
    const NodeTable *ntS = c->nt;
    const char *nmS = nt_str(ntS, id, "name");
    int recvS = nt_ref(ntS, id, "receiver");
    if (nmS && recvS >= 0 && comp_ntype(c, recvS) == TY_STRING &&
        (sp_streq(nmS, "slice!") || sp_streq(nmS, "setbyte") ||
         sp_streq(nmS, "insert") || sp_streq(nmS, "clear") ||
         sp_streq(nmS, "[]="))) {
      if (sb_iv_expr_shim(c, id, recvS, b, emit_array_call)) return 1;
      const char *sbn = strbuf_local_name(c, recvS);
      if (sbn && g_nren < MAX_RENAME) {
        Scope *shs = comp_scope_of(c, recvS);
        LocalVar *shlv = scope_local(shs, sbn);
        int tH = ++g_tmp;
        Buf armb; memset(&armb, 0, sizeof armb);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", sbn);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_sb%d", tH);
        g_nren++;
        TyKind sv_ty = shlv->type; shlv->type = TY_STRING;
        int handled = emit_array_call(c, id, &armb);
        shlv->type = sv_ty;
        g_nren--;
        if (!handled) { free(armb.p); }
        else {
          TyKind resty = comp_ntype(c, id);
          buf_printf(b, "({ sp_String *_t%d = lv_%s;"
                        " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                        " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                        " SP_GC_ROOT(lv__sb%d); ",
                     tH, rename_local(sbn), tH, tH, tH, tH, tH);
          emit_ctype(c, resty == TY_UNKNOWN || resty == TY_VOID ? TY_STRING : resty, b);
          buf_printf(b, " _res%d = %s;", tH, armb.p ? armb.p : "0");
          free(armb.p);
          buf_printf(b, " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })", tH, tH, tH);
          return 1;
        }
      }
    }
  }
  /* String#clear in VALUE position on an unnamed mutable receiver
     ((+"abc").clear): the temp's mutation is unobservable, so evaluate the
     receiver (a frozen value still raises, as CRuby) and yield a fresh
     unfrozen empty string. Named receivers keep the assignable arms. */
  {
    const NodeTable *ntC = c->nt;
    const char *nmC = nt_str(ntC, id, "name");
    int recvC = nt_ref(ntC, id, "receiver");
    int rcore = recvC;
    while (rcore >= 0 && nt_type(ntC, rcore) &&
           sp_streq(nt_type(ntC, rcore), "ParenthesesNode")) {
      int pb0 = nt_ref(ntC, rcore, "body"); int pbn0 = 0;
      const int *pbb0 = pb0 >= 0 ? nt_arr(ntC, pb0, "body", &pbn0) : NULL;
      rcore = pbn0 == 1 ? pbb0[0] : -1;
    }
    if (nmC && sp_streq(nmC, "clear") && recvC >= 0 && rcore >= 0 &&
        comp_ntype(c, recvC) == TY_STRING &&
        nt_type(ntC, rcore) &&
        (sp_streq(nt_type(ntC, rcore), "CallNode") ||
         sp_streq(nt_type(ntC, rcore), "StringNode") ||
         sp_streq(nt_type(ntC, rcore), "InterpolatedStringNode")) &&
        !strbuf_local_name(c, rcore)) {
      int aC = nt_ref(ntC, id, "arguments"); int anC = 0;
      if (aC >= 0) nt_arr(ntC, aC, "arguments", &anC);
      if (anC == 0 && nt_ref(ntC, id, "block") < 0) {
        int tC = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = ", tC);
        emit_expr(c, recvC, b);
        buf_printf(b, "; sp_str_check_mutable(_t%d); (void)_t%d; sp_str_from_bytes(\"\", 0); })", tC, tC);
        return 1;
      }
    }
  }
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
  /* An empty [] literal receiver has no element type of its own, so emit_expr
     would default it to sp_IntArray_new() -- but comp_recv_type coerces it to
     TY_POLY_ARRAY for dispatch, and the poly-array arms below build/consume it
     as a PolyArray. Pin the node's cached type so the receiver emits as a
     PolyArray too, keeping the generated C well-typed (#3223). */
  if (recv >= 0 && rt == TY_POLY_ARRAY &&
      (comp_ntype(c, recv) == TY_UNKNOWN || ty_is_array(comp_ntype(c, recv))) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
    int en = 0; nt_arr(nt, recv, "elements", &en);
    /* A cached typed-array kind is no better than UNKNOWN here: the literal is
       empty, so the kind is a default rather than an element type, and emitting
       (say) sp_IntArray_new() into a poly-array slot reads back the wrong
       struct (#3608). */
    if (en == 0) c->ntype[recv] = TY_POLY_ARRAY;
  }
  /* The same literal one pass-through call down (`[].freeze.rotate`): the
     freeze/dup/clone arm builds the literal at ITS type, which for an empty one
     is the int-array default, and the poly-array arm below then handed an
     sp_IntArray * to sp_PolyArray_dup. Pin the literal, and the call that
     carries it, to the poly array this dispatch is about to build. */
  if (recv >= 0 && rt == TY_POLY_ARRAY && nt_kind(nt, recv) == NK_CallNode) {
    const char *pnm = nt_str(nt, recv, "name");
    int pin_recv = nt_ref(nt, recv, "receiver");
    if (pnm && pin_recv >= 0 && nt_ref(nt, recv, "block") < 0 &&
        (sp_streq(pnm, "freeze") || sp_streq(pnm, "dup") || sp_streq(pnm, "clone") ||
         sp_streq(pnm, "itself")) &&
        nt_type(nt, pin_recv) && sp_streq(nt_type(nt, pin_recv), "ArrayNode")) {
      int pen = 0; nt_arr(nt, pin_recv, "elements", &pen);
      if (pen == 0 &&
          (comp_ntype(c, pin_recv) == TY_UNKNOWN || ty_is_array(comp_ntype(c, pin_recv)))) {
        c->ntype[pin_recv] = TY_POLY_ARRAY;
        c->ntype[recv] = TY_POLY_ARRAY;
      }
    }
  }
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
  if (recv >= 0 && ty_is_ptr_array(rt)) {
    int is_ia = (rt == TY_INT_ARRAY_ARRAY);
    int ecls = is_ia ? -1 : ty_obj_array_class(rt);
    /* element C type: the indexed pointer type. For an int-array-array the
       element is an sp_IntArray*; for an object array it is the class's own
       struct, which for a native class is the name its `native_struct`
       declared rather than one derived from the Ruby name. Copied out of
       class_ctype's rotating buffer, since it is held across emit calls. */
    char ecbuf[192];
    snprintf(ecbuf, sizeof ecbuf, "%s", is_ia ? "sp_IntArray" : class_ctype(c, ecls));
    const char *ecn = ecbuf;
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) {
      buf_printf(b, "((%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) {
      buf_printf(b, "((%s *)sp_PtrArray_get(", ecn);
      emit_expr(c, recv, b);
      buf_puts(b, sp_streq(name, "first") ? ", 0))" : ", -1))");
      return 1;
    }
    if (sp_streq(name, "[]=") && argc == 2) {
      int tv = ++g_tmp;
      buf_printf(b, "({ %s *_t%d = ", ecn, tv); emit_expr(c, argv[1], b);
      buf_puts(b, "; sp_PtrArray_set("); emit_expr(c, recv, b); buf_puts(b, ", ");
      emit_int_expr(c, argv[0], b); buf_printf(b, ", _t%d); _t%d; })", tv, tv);
      return 1;
    }
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) && argc >= 1) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
      for (int a = 0; a < argc; a++) {
        buf_printf(b, " sp_PtrArray_push(_t%d, ", tr);
        /* A poly value carries its pointer under a tag: the slot takes the
           pointer, not the sp_RbVal. A method of the array's own class whose
           return widened to poly -- one that answers its argument, reached
           once with a boxed one -- pushed the whole struct and the C did not
           compile (#4293). */
        if (comp_ntype(c, argv[a]) == TY_POLY) {
          buf_puts(b, "sp_poly_obj_ptr("); emit_expr(c, argv[a], b); buf_puts(b, ")");
        }
        else emit_expr(c, argv[a], b);
        buf_puts(b, ");");
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
    if (!is_ia && sp_streq(name, "sort") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "sp_PtrArray_sort_obj("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", ecls);
      return 1;
    }
    if (!is_ia && sp_streq(name, "sort!") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      int tr = ++g_tmp;
      buf_printf(b, "({ sp_PtrArray *_t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_PtrArray_sort_obj_bang(_t%d, %d); _t%d; })", tr, ecls, tr);
      return 1;
    }
    if (!is_ia && (sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      buf_printf(b, "((%s *)sp_PtrArray_minmax_obj(", ecn);
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
      /* A shared-mutable (STRBUF) local mutates its buffer IN PLACE so every
         alias/container observes it: recompute via the non-bang transform of
         the current contents, then replace the buffer (#3227). */
      { char srefB[1024];
        if (strbuf_slot_ref(c, recv, srefB, sizeof srefB)) {
          int tsb = ++g_tmp, tob = ++g_tmp, tnb = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = %s; const char *_t%d = sp_String_cstr(_t%d); (void)_t%d; ",
                     tsb, srefB, tob, tsb, tob);
          nt_node_set_str((NodeTable *)nt, id, "name", SBANG[sbi].plain);
          Buf nbB; memset(&nbB, 0, sizeof nbB);
          emit_expr(c, id, &nbB);
          nt_node_set_str((NodeTable *)nt, id, "name", SBANG[sbi].bang);
          /* The "did it change?" test has to run BEFORE the write: _tob is
             sp_String_cstr, a pointer INTO the buffer rather than a snapshot
             of it, so comparing after set_bin compared the new content with
             itself and every successful mutation answered nil (#4014). */
          int tchg = ++g_tmp;
          buf_printf(b, "const char *_t%d = %s; ", tnb, nbB.p ? nbB.p : "");
          if (SBANG[sbi].nil_nc)
            buf_printf(b, "int _t%d = !sp_str_eq(_t%d, _t%d); ", tchg, tob, tnb);
          buf_printf(b, "sp_String_set_bin(_t%d, _t%d); ", tsb, tnb);
          free(nbB.p);
          if (SBANG[sbi].nil_nc)
            buf_printf(b, "_t%d ? _t%d : NULL; })", tchg, tnb);
          else
            buf_printf(b, "_t%d; })", tnb);
          return 1;
        }
      }
      int to = ++g_tmp, tn2 = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", to); emit_expr(c, recv, b); buf_puts(b, "; (void)_t"); buf_printf(b, "%d; ", to);
      /* an in-place mutator on a frozen string raises FrozenError (#3003) */
      buf_printf(b, "if (sp_str_is_frozen_val(_t%d)) sp_raise_frozen_str(_t%d); ", to, to);
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
    if ((sp_streq(name, "concat") || sp_streq(name, "<<") ||
         sp_streq(name, "prepend")) && argc >= 1) {
      /* a STRBUF-promoted local (repeated `<<`) appends in place: the read
         form sp_String_cstr(lv) is not an lvalue, so the generic write-back
         below would emit an invalid assignment (#2020). prepend replaces the
         buffer with args-then-contents, keeping the handle stable (#3227). */
      { char sref0[1024];
        if (strbuf_slot_ref(c, recv, sref0, sizeof sref0)) {
          int tb2 = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = %s;", tb2, sref0);
          if (sp_streq(name, "prepend")) {
            int tp3 = ++g_tmp;
            buf_printf(b, " const char *_t%d = ", tp3);
            for (int j = 0; j < argc; j++) buf_puts(b, "sp_str_concat(");
            emit_str_expr(c, argv[0], b);
            for (int j = 1; j < argc; j++) { buf_puts(b, ", "); emit_str_expr(c, argv[j], b); buf_puts(b, ")"); }
            buf_printf(b, ", sp_String_cstr(_t%d)); sp_String_set_bin(_t%d, _t%d);", tb2, tb2, tp3);
          }
          else {
            for (int j = 0; j < argc; j++) {
              buf_printf(b, " sp_String_append(_t%d, ", tb2);
              emit_str_append_arg(c, argv[j], b);
              buf_puts(b, ");");
            }
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
          emit_str_append_arg(c, arg, b);
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
          emit_str_append_arg(c, arg, b);
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
        for (int j = 0; j < argc; j++) { buf_puts(b, ", "); emit_str_append_arg(c, argv[j], b); buf_puts(b, ")"); }
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
      buf_printf(b, "; sp_str_check_mutable(_t%d);", to);   /* frozen -> FrozenError (#3003) */
      buf_printf(b, " sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) _t%d += (sp_int)sp_str_length(_t%d) + 1;", ti2, ti2, to);
      buf_printf(b, " const char *_t%d = sp_str_splice_at(_t%d, _t%d, 0, ", tn2, to, ti2);
      emit_str_expr(c, argv[1], b); buf_puts(b, ", 0); ");
      if (lvw) { emit_expr(c, recv, b); buf_printf(b, " = _t%d; ", tn2); }
      buf_printf(b, "_t%d; })", tn2);
      return 1;
    }
    if (sp_streq(name, "replace") && argc == 1) {
      const char *rvt2 = nt_type(nt, recv);
      /* shared-mutable local: swap the buffer contents in place (#3227) */
      { char srefR[1024];
        if (strbuf_slot_ref(c, recv, srefR, sizeof srefR)) {
          int tbR = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = %s; sp_String_set_bin(_t%d, ",
                     tbR, srefR, tbR);
          emit_str_expr(c, argv[0], b);
          buf_printf(b, "); sp_String_cstr(_t%d); })", tbR);
          return 1;
        }
      }
      int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                         sp_streq(rvt2, "InstanceVariableReadNode"));
      int tn2 = ++g_tmp;
      buf_printf(b, "({ sp_str_check_mutable(");   /* frozen -> FrozenError (#3003) */
      emit_expr(c, recv, b);
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
      buf_printf(b, "; sp_int _t%d = sp_re_match(sp_re_pat_%d, _t%d);"
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
      buf_printf(b, "; sp_str_check_mutable(_t%d);", to);   /* frozen -> FrozenError (#3003) */
      buf_printf(b, " sp_int _t%d = (sp_int)sp_str_length(_t%d); sp_int _t%d, _t%d;",
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
    if (argc == 2 && re_lit_index(c, argv[0]) >= 0) {
      /* slice!(/re/, n): remove the nth capture group of the first match and
         evaluate to it. sp_re_caps holds each group's byte span, so the removal
         is the group's own occurrence rather than the first textual one, which
         is a different character when the group repeats (#3543). */
      int ts = ++g_tmp, tn = ++g_tmp, th = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = ", ts); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; const char *_t%d = sp_re_match(sp_re_pat_%d, _t%d) >= 0"
                    " ? (_t%d == 0 ? sp_re_match_str"
                    "    : (_t%d >= 1 && _t%d <= 9 ? sp_re_captures[_t%d] : NULL)) : NULL;",
                 th, re_lit_index(c, argv[0]), ts, tn, tn, tn, tn);
      if (sb_asgn) {
        buf_printf(b, " if (_t%d && _t%d >= 0 && _t%d <= 9) { sp_str_check_mutable(_t%d);"
                      " sp_int _b = sp_re_caps[2 * _t%d], _e = sp_re_caps[2 * _t%d + 1]; ",
                   th, tn, tn, ts, tn, tn);
        emit_expr(c, recv, b);
        buf_printf(b, " = sp_str_concat(sp_str_byteslice(_t%d, 0, _b),"
                      " sp_str_byteslice(_t%d, _e, (sp_int)sp_str_byte_len(_t%d) - _e)); }",
                   ts, ts, ts);
      }
      buf_printf(b, " _t%d; })", th);
      return 1;
    }
    if (sb_asgn && argc == 2) {
      /* character-indexed splice, not byte-indexed, for a multibyte receiver (#3084) */
      int ti2 = ++g_tmp, tl2 = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
      buf_printf(b, "; sp_int _t%d = (sp_int)sp_str_length(", tn2);
      emit_expr(c, recv, b);
      buf_printf(b, "); const char *_t%d = NULL;"
                    " if (_t%d < 0) _t%d += _t%d;"
                    " if (_t%d >= 0 && _t%d <= _t%d && _t%d > 0) {"
                    " if (_t%d > _t%d - _t%d) _t%d = _t%d - _t%d;"
                    " _t%d = sp_str_sub_range(",
                 tr2,
                 ti2, ti2, tn2,
                 ti2, ti2, tn2, tl2,
                 tl2, tn2, ti2, tl2, tn2, ti2,
                 tr2);
      emit_expr(c, recv, b);
      buf_printf(b, ", _t%d, _t%d); ", ti2, tl2);
      emit_expr(c, recv, b);
      buf_puts(b, " = sp_str_concat(sp_str_sub_range(");
      emit_expr(c, recv, b);
      buf_printf(b, ", 0, _t%d), sp_str_sub_range(", ti2);
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
    { char srefBR[1024];
      if (strbuf_slot_ref(c, recv, srefBR, sizeof srefBR)) {
        int tm2 = ++g_tmp, tr3 = ++g_tmp, tn3 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s; sp_Range _t%d = ", tm2, srefBR, tr3);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; const char *_t%d = sp_str_bytesplice(sp_String_cstr(_t%d),"
                      " _t%d.first, _t%d.last - _t%d.first + (_t%d.excl ? 0 : 1), ",
                   tn3, tm2, tr3, tr3, tr3, tr3);
        emit_str_expr(c, argv[1], b);
        buf_printf(b, "); sp_String_set_bin(_t%d, _t%d); _t%d; })", tm2, tn3, tn3);
        return 1;
      } }
    const char *rvt9 = nt_type(nt, recv);
    int lvw9 = rvt9 && (sp_streq(rvt9, "LocalVariableReadNode") ||
                        sp_streq(rvt9, "InstanceVariableReadNode"));
    int tr9 = ++g_tmp, tn9 = ++g_tmp;
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "sp_Range _t%d = ", tr9); emit_expr(c, argv[0], b);
    buf_printf(b, "; const char *_t%d = sp_str_bytesplice(", tn9);
    emit_expr(c, recv, b);
    buf_printf(b, ", _t%d.first, _t%d.last - _t%d.first + (_t%d.excl ? 0 : 1), ", tr9, tr9, tr9, tr9);
    emit_str_expr(c, argv[1], b); buf_puts(b, ")");
    if (lvw9) { buf_puts(b, "; "); emit_expr(c, recv, b); buf_printf(b, " = _t%d", tn9); }
    buf_printf(b, "; _t%d; })", tn9);
    return 1;
  }
  /* append_as_bytes: raw byte append == << for spinel's byte strings (#2397) */
  if (rt == TY_STRING && sp_streq(name, "append_as_bytes") && argc >= 1 && recv >= 0) {
    { char srefAB[1024];
      if (strbuf_slot_ref(c, recv, srefAB, sizeof srefAB)) {
        int tm2 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;", tm2, srefAB);
        for (int a9 = 0; a9 < argc; a9++) {
          buf_printf(b, " sp_String_append_bin(_t%d, ", tm2);
          if (comp_ntype(c, argv[a9]) == TY_INT) { buf_puts(b, "sp_int_chr("); emit_int_expr(c, argv[a9], b); buf_puts(b, ")"); }
          else emit_str_expr(c, argv[a9], b);
          buf_puts(b, ");");
        }
        buf_printf(b, " sp_String_cstr(_t%d); })", tm2);
        return 1;
      } }
    const char *rvt9 = nt_type(nt, recv);
    int lvw9 = rvt9 && (sp_streq(rvt9, "LocalVariableReadNode") ||
                        sp_streq(rvt9, "InstanceVariableReadNode"));
    int tn9 = ++g_tmp;
    /* append_as_bytes accepts String AND Integer arguments; an Integer is the
       raw byte value (100 -> "d"), materialized via sp_int_chr (#2463). A
       frozen receiver raises first, like every other in-place append (#3333). */
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "const char *_t%d = sp_str_concat(", tn9);  /* reassigned per extra arg: keep non-const? const char* variable is reassignable (the POINTEE is const) */
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
    /* shared handle receiver: swap the buffer in place (#3227) */
    { char srefBS[1024];
      if (strbuf_slot_ref(c, recv, srefBS, sizeof srefBS)) {
        int tm2 = ++g_tmp, tn3 = ++g_tmp;
        buf_printf(b, "({ sp_String *_t%d = %s;"
                      " const char *_t%d = sp_str_bytesplice(sp_String_cstr(_t%d), ",
                   tm2, srefBS, tn3, tm2);
        emit_int_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
        buf_puts(b, ", "); emit_str_expr(c, argv[2], b);
        buf_printf(b, "); sp_String_set_bin(_t%d, _t%d); _t%d; })", tm2, tn3, tn3);
        return 1;
      } }
    const char *rvt2 = nt_type(nt, recv);
    int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                       sp_streq(rvt2, "InstanceVariableReadNode"));
    int tn2 = ++g_tmp;
    /* in-place mutator: a frozen receiver raises before the splice (#3333) */
    buf_puts(b, "({ sp_str_check_mutable("); emit_expr(c, recv, b); buf_puts(b, "); ");
    buf_printf(b, "const char *_t%d = sp_str_bytesplice(", tn2);
    emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_int_expr(c, argv[0], b);
    buf_puts(b, ", "); emit_int_expr(c, argv[1], b);
    buf_puts(b, ", "); emit_str_expr(c, argv[2], b); buf_puts(b, ")");
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
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      Buf cb; memset(&cb, 0, sizeof cb);
      if (!emit_block_cond_next(c, fblock, g_indent + 1, &cb)) {
        for (int j = 0; j < fbn - 1; j++) emit_stmt(c, fbb[j], g_pre, g_indent + 1);
        int sv = g_indent; g_indent++;
        emit_cond(c, fbb[fbn - 1], &cb); g_indent = sv;
      }
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
  /* uniq on a bare poly value that is an array at runtime (an ivar assigned a
     caller-splat rest array widens to poly): the distinct elements (#3341). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "uniq") && argc == 0 &&
      nt_ref(nt, id, "block") < 0 && !diag_user_defines(c, name)) {
    buf_puts(b, "sp_poly_uniq("); emit_expr(c, recv, b); buf_puts(b, ")");
    return 1;
  }
  /* compact / flatten on the same shape: an Array read out of a container.
     uniq had an arm and these did not, so they raised NoMethodError naming
     Array -- which is what the receiver was (#3423). */
  if (recv >= 0 && rt == TY_POLY && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "compact") || sp_streq(name, "flatten")) &&
      !diag_user_defines(c, name)) {
    /* compact keeps the receiver's kind (a Hash drops its nil VALUES and stays
       a Hash), so it answers boxed; flatten is an Array either way. */
    buf_printf(b, "sp_poly_%s(", sp_streq(name, "compact") ? "compact_val" : "flatten");
    emit_expr(c, recv, b); buf_puts(b, ")");
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
         sp_streq(name, "map") || sp_streq(name, "collect") || sp_streq(name, "flat_map") ||
         sp_streq(name, "filter_map") ||
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
  /* `poly.reduce/inject { }` (with or without a seed) where poly is an
     array read out of a container (a transpose row, a nested element):
     coerce to a poly array and re-enter the array fold emitter with the
     receiver overridden (#3312). */
  /* The operator-SYMBOL form takes the same route: `reduce(:+)` and
     `reduce(init, :+)` carry no block, and without this they fell through to
     the Hash/Enumerable face, which converts the receiver to a hash and refuses
     an Array at run time -- so a partitioned array answered NoMethodError
     (#4079). */
  int red_sym = (nt_ref(nt, id, "block") < 0 && argc >= 1 && argc <= 2 &&
                 nt_type(nt, argv[argc - 1]) &&
                 sp_streq(nt_type(nt, argv[argc - 1]), "SymbolNode"));
  if (recv >= 0 && rt == TY_POLY &&
      (nt_ref(nt, id, "block") >= 0 || red_sym) &&
      (sp_streq(name, "reduce") || sp_streq(name, "inject")) &&
      (argc <= 1 || red_sym) && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", name, ta);
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
  /* find_all, take_while and drop_while ride the same loop. They differ only
     in what the loop does with the predicate and in what the value is: those
     three answer the plain Array of elements whatever the receiver is (CRuby's
     Hash#find_all gives the [k, v] pairs, unlike Hash#select), so they skip
     the sp_poly_kept_result hop that hands a Hash receiver a Hash back. */
  int pf_rej = sp_streq(name, "reject");
  int pf_sel = sp_streq(name, "select") || sp_streq(name, "filter");
  int pf_fa  = sp_streq(name, "find_all");
  int pf_tw  = sp_streq(name, "take_while");
  int pf_dw  = sp_streq(name, "drop_while");
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (pf_rej || pf_sel || pf_fa || pf_tw || pf_dw)) {
    int fblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, fblock, 0); if (bp) bp = rename_local(bp);
    int fbody = nt_ref(nt, fblock, "body");
    int fbn = 0; const int *fbb = fbody >= 0 ? nt_arr(nt, fbody, "body", &fbn) : NULL;
    if (fbn >= 1) {
      int keep_truthy = !pf_rej;  /* select/filter/find_all keep truthy */
      int trecv = ++g_tmp, ti = ++g_tmp, tres = ++g_tmp, tbox = ++g_tmp;
      int tdrop = pf_dw ? ++g_tmp : 0;
      Buf rb = expr_buf(c, recv);
      /* Keep the receiver boxed as well as coerced: Hash#select answers a
         Hash, Array#select an Array, and only the runtime value says which
         (#3449). sp_poly_arr_recv renders a hash as its [key, value] pairs, so
         the loop below is the same either way. */
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 tbox, rb.p ? rb.p : "sp_box_nil()", tbox);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"%s\"); SP_GC_ROOT(_t%d);\n",
                 trecv, tbox, name, trecv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", tres, tres);
      if (pf_dw) {
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = 1;\n", tdrop);
      }
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, fblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      Buf cb; memset(&cb, 0, sizeof cb);
      if (!emit_block_cond_next(c, fblock, g_indent + 1, &cb)) {
        for (int j = 0; j < fbn - 1; j++) emit_stmt(c, fbb[j], g_pre, g_indent + 1);
        int sv = g_indent; g_indent++;
        emit_cond(c, fbb[fbn - 1], &cb); g_indent = sv;
      }
      emit_indent(g_pre, g_indent + 1);
      if (pf_tw) {
        buf_printf(g_pre, "if (!(%s)) break;\n", cb.p ? cb.p : "0");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tres, es);
      }
      else if (pf_dw) {
        buf_printf(g_pre, "if (_t%d && (%s)) continue;\n", tdrop, cb.p ? cb.p : "0");
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "_t%d = 0;\n", tdrop);
        emit_indent(g_pre, g_indent + 1);
        buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", tres, es);
      }
      else {
        buf_printf(g_pre, "if (%s(%s)) sp_PolyArray_push(_t%d, %s);\n",
                   keep_truthy ? "" : "!", cb.p ? cb.p : "0", tres, es);
      }
      free(cb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      if (pf_sel || pf_rej) buf_printf(b, "sp_poly_kept_result(_t%d, _t%d)", tbox, tres);
      else buf_printf(b, "_t%d", tres);
      return 1;
    }
  }
  /* `poly.split(sep[, limit])` where poly holds a string (a String param
     widened to poly by a poly call site): dispatch String#split at runtime.
     Without this the whole `str.split.map` chain stayed UNKNOWN and a following
     multiple assignment was rejected (#3186 / #3164). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "split") &&
      (argc == 0 || argc == 1 || argc == 2) && nt_ref(nt, id, "block") < 0) {
    int tv = ++g_tmp;
    /* A shared-string handle (`s = +""; s << "a;b"`) is a String, and every
       arm below reads `.v.s`, so the handle is dereferenced into the
       immediate form first -- the same retry sp_poly_add makes. Without it
       the guard was single-armed and a heap String raised NoMethodError
       naming String, for a method String has (#4279). */
    buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", tv); emit_expr(c, recv, b);
    buf_puts(b, ")");
    buf_printf(b, "; _t%d.tag == SP_TAG_STR ? ", tv);
    if (argc == 0) buf_printf(b, "sp_str_split_ws(_t%d.v.s)", tv);
    else if (argc == 1) {
      /* a regex separator splits with sp_re_split, not the string-separator path
         (which would coerce the pattern to a bogus literal separator) (#3212). */
      if (comp_ntype(c, argv[0]) == TY_REGEX) {
        buf_puts(b, "sp_re_split("); emit_expr(c, argv[0], b); buf_printf(b, ", _t%d.v.s)", tv);
      }
      else {
        /* the split separator slot, shared with the String-receiver path:
           handles the nil whitespace mode, statically and at run time, and
           raises split's own TypeError wording for a wrong class (#4223) */
        buf_printf(b, "sp_str_split_drop_trailing(_t%d.v.s, ", tv);
        emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ")");
      }
    }
    else {
      /* The separator of String#split can be nil ("split on whitespace" in
         CRuby); sp_str_split_limit treats a NULL separator as that mode.
         emit_str_pattern_expr is the split-separator slot shared with the
         String-receiver path: it evaluates a statically nil separator for
         its side effects and passes NULL, keeps the same answer for a
         separator that is nil only at run time, and raises split's own
         TypeError wording for a wrong class (#4223). */
      buf_printf(b, "sp_str_split_limit(_t%d.v.s, ", tv);
      emit_str_pattern_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
    }
    buf_printf(b, " : (sp_StrArray *)(sp_raise_nomethod(sp_nomethod_msg(\"split\", _t%d)), (void *)0); })", tv);
    return 1;
  }
  /* `poly.map! { |x| ... }` / `collect!` where poly is an array read out of a
     container: coerce to a poly array and rewrite each element in place with
     the block result, returning the (mutated) array (#3162). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") >= 0 && argc == 0 &&
      (sp_streq(name, "map!") || sp_streq(name, "collect!"))) {
    int mblock = nt_ref(nt, id, "block");
    const char *bp = block_param_name(c, mblock, 0); if (bp) bp = rename_local(bp);
    int mbody = nt_ref(nt, mblock, "body");
    int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
    if (mbn >= 1) {
      int trecv = ++g_tmp, ti = ++g_tmp, torig = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 torig, rb.p ? rb.p : "sp_box_nil()", torig);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(_t%d, \"map!\"); SP_GC_ROOT(_t%d);\n",
                 trecv, torig, trecv);
      free(rb.p);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
      char es[64]; snprintf(es, sizeof es, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
      int splat = emit_iter_autosplat(c, mblock, TY_POLY_ARRAY, es, g_indent + 1);
      if (!splat && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = %s;\n", bp, es); }
      for (int j = 0; j < mbn - 1; j++) emit_stmt(c, mbb[j], g_pre, g_indent + 1);
      int sv = g_indent; g_indent++;
      Buf vb; memset(&vb, 0, sizeof vb); emit_boxed(c, mbb[mbn - 1], &vb); g_indent = sv;
      emit_indent(g_pre, g_indent + 1);
      buf_printf(g_pre, "_t%d->data[_t%d] = %s;\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
      free(vb.p);
      emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_poly_arr_writeback(_t%d, _t%d);\n", torig, trecv);
      buf_printf(b, "_t%d", trecv);
      return 1;
    }
  }
  /* `poly.zip(other...)` on a poly array read out of a container (e.g. a row
     that is a block param of an outer nested-array iterator): coerce the
     receiver to a poly array and re-dispatch as the array zip form, whose arg
     handling already unboxes a poly argument too (#3190). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "zip") && argc >= 1 &&
      nt_ref(nt, id, "block") < 0 && g_n_argov < MAX_ARG_OVERRIDE) {
    int ta = ++g_tmp;
    Buf rb = expr_buf(c, recv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_arr_recv(%s, \"zip\"); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
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
    buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, trecv, ti);
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
  /* The blockless grouping enumerators take the same route: CRuby answers an
     Enumerator, spinel materializes it, so re-dispatching as the array form
     gives the groups a later .map / .to_a can walk. */
  if (recv >= 0 && rt == TY_POLY && g_n_argov < MAX_ARG_OVERRIDE &&
      g_poly_redispatch_id != id &&
      ((nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(name, "sort_by") || sp_streq(name, "max_by") || sp_streq(name, "min_by"))) ||
       /* and the COMPARATOR-block forms. `sort` blockless has an arm of its
          own above; with a block it had none, so a method whose parameter
          sees two element types -- which is what makes it poly rather than a
          poly ARRAY -- raised NoMethodError naming Array, on the first call,
          having printed nothing (#4290). The array emitters serve the
          comparator block on a poly array. */
       (nt_ref(nt, id, "block") >= 0 && argc == 0 && !user_defines_or_reads(c, name) &&
        (sp_streq(name, "sort") || sp_streq(name, "min") || sp_streq(name, "max"))) ||
       (nt_ref(nt, id, "block") < 0 && argc == 1 && !user_defines_or_reads(c, name) &&
        (sp_streq(name, "each_cons") || sp_streq(name, "each_slice") ||
         sp_streq(name, "combination") || sp_streq(name, "permutation"))) ||
       /* and their BLOCK forms, which had no arm of their own and fell to the
          loud NoMethodError -- the array emitters they re-dispatch to serve
          the block and the blockless shape alike. */
       (nt_ref(nt, id, "block") >= 0 && argc == 1 && !user_defines_or_reads(c, name) &&
        (sp_streq(name, "cycle") || sp_streq(name, "zip") ||
         /* each_slice / each_cons answer the receiver, and the wrapper that
            hands it back re-enters this node -- which a pending safe-nav guard
            re-enters too, and the two do not compose: the inner pass finds no
            emitter and bakes a NoMethodError whose argument does not even
            typecheck. Leave the guarded shape on its existing path (it raises
            at run time, as it did before) rather than failing the build. */
         sp_streq(name, "each_cons") || sp_streq(name, "each_slice"))))) {
    int ta = ++g_tmp;
    /* `each_slice(n) { }` / `each_cons(n) { }` answer the RECEIVER, and for a
       Hash that is the hash itself, not the pairs the re-dispatch materializes
       from it. Bind the receiver once, materialize from that binding, and hand
       the binding back -- the same shape emit_iter_value_expr uses for a
       receiver rewritten to `__enum_to_a`. */
    int ret_recv = (nt_ref(nt, id, "block") >= 0 &&
                    (sp_streq(name, "each_cons") || sp_streq(name, "each_slice")));
    int tbox = ret_recv ? ++g_tmp : 0;
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    if (ret_recv) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                 tbox, rb.p ? rb.p : "sp_box_nil()", tbox);
      free(rb.p); memset(&rb, 0, sizeof rb);
      buf_printf(&rb, "_t%d", tbox);
    }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_to_a_arr(%s); SP_GC_ROOT(_t%d);\n",
               ta, rb.p ? rb.p : "sp_box_nil()", ta);
    free(rb.p);
    g_argov_node[g_n_argov] = recv;
    snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", ta);
    g_n_argov++;
    TyKind sv = c->ntype[recv]; c->ntype[recv] = TY_POLY_ARRAY;
    /* and pin it for the inference too, the way the hash face does: the cached
       type alone does not survive a safe-navigation guard, whose re-emission
       asks again and re-establishes the receiver as poly -- the array emitters
       then decline the very call this arm re-entered to have them serve. */
    int sv_face = an_face_node(); TyKind sv_fk = an_face_kind();
    an_set_face_node(recv, TY_POLY_ARRAY);
    /* The re-entry below is the SAME node, and neither the type nor the pin
       stops it reaching this arm again. Latch the node. */
    int sv_rd = g_poly_redispatch_id; g_poly_redispatch_id = id;
    if (ret_recv) {
      Buf vb; memset(&vb, 0, sizeof vb);
      emit_call(c, id, &vb);
      buf_printf(b, "({ (void)(%s); _t%d; })", vb.p ? vb.p : "0", tbox);
      free(vb.p);
    }
    else emit_call(c, id, b);
    g_poly_redispatch_id = sv_rd;
    an_set_face_node(sv_face, sv_fk);
    c->ntype[recv] = sv;
    g_n_argov--;
    return 1;
  }
  if (recv >= 0 && ty_is_array(rt)) {
    /* a nil / true / false OPERAND to the Array-expecting family is CRuby's
       TypeError ("no implicit conversion of nil into Array") -- concat fell
       to NoMethodError, product answered [] -- with every argument still
       evaluated in order first, as a real call would */
    /* `product(*xs)` spreads xs across the ARGUMENT LIST, one operand array
       per element. The arms below read a splat as a single operand instead,
       so `[1,2].product(*[])` answered [] where CRuby answers [[1],[2]], and
       `product(*[[3]])` nested the operand. Spreading a runtime-length list
       needs a variadic helper these arms do not have; refuse rather than
       answer wrongly (#4298). A splat of a literal empty array is the one
       case with an answer here: no operands at all. */
    if (sp_streq(name, "product") && argc >= 1) {
      int spl = -1;
      for (int ai = 0; ai < argc; ai++)
        if (nt_kind(nt, argv[ai]) == NK_SplatNode) { spl = ai; break; }
      if (spl >= 0) {
        int se = nt_ref(nt, argv[spl], "expression");
        int sen = 0;
        int empty_lit = se >= 0 && nt_kind(nt, se) == NK_ArrayNode &&
                        (nt_arr(nt, se, "elements", &sen), sen == 0);
        /* ...and a local whose every write is an empty literal is the same
           empty list, which is the shape the report is about. */
        if (!empty_lit && se >= 0 && nt_kind(nt, se) == NK_LocalVariableReadNode) {
          const char *sn = nt_str(nt, se, "name");
          Scope *ssc = sn ? comp_scope_of(c, se) : NULL;
          if (sn && ssc && local_all_writes_empty_array(c, ssc, sn)) empty_lit = 1;
        }
        if (!(argc == 1 && empty_lit)) {
          unsupported_feature(c, id, "Array#product with a splatted argument list");
          return 1;
        }
        /* an empty splat is no operands at all: `[1,2].product` -- each
           element wrapped in a one-element array */
        {
          int tp = ++g_tmp, ti2 = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tp, tp);
          buf_printf(b, " sp_RbVal _t%d = ", ti2); emit_boxed(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ti2);
          int tk = ++g_tmp;
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {"
                        " sp_PolyArray *_t%d_e = sp_PolyArray_new();"
                        " sp_PolyArray_push(_t%d_e, sp_poly_arr_get(_t%d, _t%d));"
                        " sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d_e)); }",
                     tk, tk, ti2, tk, tk, tk, ti2, tk, tp, tk);
          buf_printf(b, " _t%d; })", tp);
          return 1;
        }
      }
    }
    if ((sp_streq(name, "concat") || sp_streq(name, "replace") ||
         sp_streq(name, "product") || sp_streq(name, "union") ||
         sp_streq(name, "difference") || sp_streq(name, "intersection")) &&
        argc >= 1) {
      int bad = -1;
      for (int ai = 0; ai < argc; ai++) {
        TyKind at = comp_ntype(c, argv[ai]);
        if (at == TY_NIL || at == TY_BOOL || conv_to_ary_impossible(at)) { bad = ai; break; }
      }
      if (bad >= 0) {
        TyKind arty = comp_ntype(c, id);
        int tb = ++g_tmp;
        buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
        for (int ai = 0; ai < argc; ai++) {
          if (ai == bad && comp_ntype(c, argv[ai]) == TY_BOOL) {
            buf_printf(b, "int _t%d = (", tb); emit_expr(c, argv[ai], b); buf_puts(b, "); ");
          }
          else {
            buf_puts(b, "(void)("); emit_expr(c, argv[ai], b); buf_puts(b, "); ");
          }
        }
        if (comp_ntype(c, argv[bad]) == TY_NIL)
          buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into Array\");");
        else if (comp_ntype(c, argv[bad]) == TY_BOOL)
          buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                        " ? \"no implicit conversion of true into Array\""
                        " : \"no implicit conversion of false into Array\");", tb);
        else
          buf_printf(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Array\");",
                     conv_builtin_class_name(comp_ntype(c, argv[bad])));
        buf_printf(b, " %s; })", raise_tail_value(arty));
        return 1;
      }
    }
    if (sp_streq(name, "pack") && argc == 1 &&
        (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY || rt == TY_POLY_ARRAY || rt == TY_STR_ARRAY)) {
      const char *kind = rt == TY_POLY_ARRAY ? "Poly"
                       : rt == TY_STR_ARRAY  ? "Str"
                       : rt == TY_FLOAT_ARRAY ? "Float" : "Int";
      buf_printf(b, "sp_%sArray_pack(", kind);
      emit_expr(c, recv, b); buf_puts(b, ", "); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    /* product(b, c, ...) with two or more array arguments: the n-way Cartesian
       product. The single-argument form is specialized below (per element-type
       boxing); for 2+ arguments box the receiver and every argument into rooted
       locals -- the GC is precise, so they must stay reachable across the
       helper's allocations -- and hand them to sp_poly_product as one vector. */
    /* product(b, c, ...) WITH a block: CRuby runs the block for each tuple and
       answers the receiver. Emitting the product alone answered the tuple array
       instead, so `arr.product(x, y) { }.equal?(arr)` was false (and the
       inference, which already said self, disagreed with the emission). */
    if (sp_streq(name, "product") && argc >= 2 && nt_ref(nt, id, "block") >= 0) {
      int blk = nt_ref(nt, id, "block");
      int bbody = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
      const char *fp0 = block_param_name(c, blk, 0);
      int nn = argc + 1;
      int *ids = (int *)malloc(sizeof(int) * nn);
      if (!ids) { perror("malloc"); exit(1); }
      int trecv = ++g_tmp, tprod = ++g_tmp, ti = ++g_tmp;
      buf_puts(b, "({ ");
      buf_printf(b, "sp_RbVal _t%d = ", trecv); emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", trecv);
      ids[0] = trecv;
      for (int i = 0; i < argc; i++) {
        ids[i + 1] = ++g_tmp;
        buf_printf(b, "sp_RbVal _t%d = ", ids[i + 1]); emit_boxed(c, argv[i], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", ids[i + 1]);
      }
      buf_printf(b, "sp_RbVal _tp%d[%d] = { _t%d", tprod, nn, ids[0]);
      for (int i = 1; i < nn; i++) buf_printf(b, ", _t%d", ids[i]);
      buf_printf(b, " }; sp_PolyArray *_t%d = sp_poly_product(_tp%d, %d); SP_GC_ROOT(_t%d);",
                 tprod, tprod, nn, tprod);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
                 ti, ti, tprod, ti);
      if (fp0) buf_printf(b, " lv_%s = sp_PolyArray_get(_t%d, _t%d);", rename_local(fp0), tprod, ti);
      buf_puts(b, " {");
      for (int j2 = 0; j2 < bn; j2++) emit_stmt(c, bb[j2], b, 0);
      buf_puts(b, " } } ");
      /* the receiver, in the C type this call is inferred to have */
      TyKind pres = comp_ntype(c, id);
      if (pres == TY_POLY || pres == TY_UNKNOWN) buf_printf(b, "_t%d; })", trecv);
      else { emit_unbox_text(c, pres, ({ static char rb9[32]; snprintf(rb9, sizeof rb9, "_t%d", trecv); rb9; }), b); buf_puts(b, "; })"); }
      free(ids);
      return 1;
    }
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
    if (sp_streq(name, "values_at") && argc == 0) {
      /* values_at with no indices is the empty array, of the receiver's kind
         (matching the inferred type) (#2980) */
      const char *an0 = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an0) {
        buf_printf(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), sp_%sArray_new())", an0);
        return 1;
      }
    }
    if (sp_streq(name, "values_at") && argc >= 1) {
      const char *an = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
      if (an) {
        int tr = ++g_tmp, to = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", an, tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); ", an, to, an);
        for (int a = 0; a < argc; a++) {
          TyKind at = comp_ntype(c, argv[a]);
          if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode")) {
            /* values_at(*idx): each element of the splatted array is a
               separate index (#3277). */
            int ts = ++g_tmp, tk = ++g_tmp;
            buf_printf(b, "{ sp_PolyArray *_t%d = ", ts); emit_expr(c, argv[a], b);
            buf_printf(b, "; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                          " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d,"
                          " sp_poly_to_i(sp_PolyArray_get(_t%d, _t%d)))); } ",
                       tk, tk, ts, tk, an, to, an, tr, ts, tk);
          }
          else if (at == TY_RANGE) {
            /* an open or negative endpoint resolves against the length, the
               way Array#[] resolves it: read raw, an endless range ran to
               INTPTR_MAX and pushed until the process died (#3847) */
            int trng = ++g_tmp, ti = ++g_tmp, tlen = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
            buf_printf(b, "{ sp_Range _t%d = ", trng); emit_expr(c, argv[a], b);
            buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tlen, an, tr);
            buf_printf(b, " sp_int _t%d = _t%d.first == INTPTR_MIN ? 0"
                          " : (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
                       tlo, trng, trng, trng, tlen, trng);
            buf_printf(b, " sp_int _t%d = _t%d.last == INTPTR_MAX ? _t%d - 1"
                          " : ((_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) - (_t%d.excl ? 1 : 0));",
                       thi, trng, tlen, trng, trng, tlen, trng, trng);
            buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                          " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d)); } ",
                       ti, tlo, ti, thi, ti, an, to, an, tr, ti);
          }
          else {
            /* the index is an index: a Float one converts here, as it does for
               every other index-taking method. Emitted raw, a literal Float
               reached `sp_XArray_get`'s sp_int parameter and the C compiler
               truncated it with a warning of its own (#3936). */
            buf_printf(b, "sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, ", an, to, an, tr);
            emit_int_expr(c, argv[a], b); buf_puts(b, ")); ");
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
          buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
          buf_printf(b, "; sp_int _len = sp_%sArray_length(_t%d);"
                        " sp_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
                        " if (_ix < 0 || _ix >= _len) { ",
                     an, tr, ti, ti, ti);
          if (fp0r) buf_printf(b, "lv_%s = _t%d; ", fp0r, ti);
          for (int j = 0; j + 1 < fbn; j++) emit_stmt(c, fbb[j], b, 0);
          buf_printf(b, "sp_PolyArray_push(_t%d, ", to);
          if (fbn > 0) emit_boxed(c, fbb[fbn - 1], b); else buf_puts(b, "sp_box_nil()");
          buf_puts(b, "); }\nelse { ");
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
          buf_printf(b, "{ sp_int _t%d = ", ti); emit_int_expr(c, argv[a], b);
          buf_printf(b, "; sp_int _len = sp_%sArray_length(_t%d);"
                        " sp_int _ix = _t%d < 0 ? _t%d + _len : _t%d;"
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
        buf_printf(b, "; sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
      buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
        buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
        /* rooted, as the TY_POLY map!/collect! near the top of this file
           already roots its own hoist: fill stores into the receiver on every
           turn, and the block never mentions the receiver, so this temporary is
           the only thing holding it while the block allocates */
        emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "sp_int _t%d = sp_%sArray_length(_t%d);\n", tn, fk, trecv);
        /* resolve the [start, end) span from the arguments */
        int is_range = (argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE);
        if (is_range) {
          int tr = ++g_tmp;
          emit_indent(g_pre, g_indent);
          /* rendered first: emit_expr may want g_pre lines of its own (#4065) */
          { Buf rgb2; memset(&rgb2, 0, sizeof rgb2); emit_expr(c, argv[0], &rgb2);
            buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr, rgb2.p ? rgb2.p : "(sp_Range){0}");
            free(rgb2.p); }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;\n",
                     ts, tr, ts, ts, tn, ts, ts);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = (_t%d.last < 0 ? _t%d.last + _t%d : _t%d.last) + (_t%d.excl ? 0 : 1);\n",
                     te, tr, tr, tn, tr, tr);
        }
        else {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = 0;", ts);
          if (argc >= 1) { buf_printf(g_pre, " _t%d = ", ts); emit_int_expr(c, argv[0], g_pre);
                           buf_printf(g_pre, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts); }
          buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = _t%d;", te, tn);
          if (argc == 2) { buf_printf(g_pre, " { sp_int _tl = "); emit_int_expr(c, argv[1], g_pre);
                           buf_printf(g_pre, "; if (_tl < 0) _tl = 0; _t%d = _t%d + _tl; }", te, ts); }
          buf_puts(g_pre, "\n");
        }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = _t%d; _t%d < _t%d; _t%d++) {\n", ti, ts, ti, te, ti);
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
      /* fill(value, start[, length]): start and length are offsets, and a
         value with no integer conversion is CRuby's TypeError. They went into
         the offset slot as-is, so a String start read as a pointer and the
         fill quietly did nothing (#3611). */
      for (int fa = 1; fa < argc; fa++) {
        TyKind ft = comp_ntype(c, argv[fa]);
        /* nil is allowed: it means "from the start" / "to the end" */
        const char *fcn = ft == TY_STRING ? "String" : ft == TY_SYMBOL ? "Symbol"
                        : ty_is_array(ft) ? "Array"
                        : ty_is_hash(ft) ? "Hash" : NULL;
        if (!fcn) continue;
        int tf = ++g_tmp;
        buf_printf(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
        emit_expr(c, argv[fa], b);
        buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Integer\");"
                      " (sp_%sArray *)0; })", fcn, (rt == TY_POLY_ARRAY) ? "Poly" : k);
        (void)tf;
        return 1;
      }
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
        buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tn, fk, t);
        if (argc >= 2 && comp_ntype(c, argv[1]) == TY_RANGE) {
          /* fill(val, range): use range as index span */
          int tr = ++g_tmp, te = ++g_tmp;
          buf_printf(b, " sp_Range _t%d = ", tr); emit_expr(c, argv[1], b);
          buf_printf(b, "; sp_int _t%d = _t%d.first; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;",
                     ts, tr, ts, ts, tn, ts, ts);
          /* a negative end counts from the end, an endless one runs to the
             last element -- unnormalized, `fill(v, 2..)` grew the array
             forever (#3605) and `fill(v, 1..-1)` filled nothing (#3606) */
          buf_printf(b, " sp_int _t%d = _t%d.last;"
                        " if (_t%d == INTPTR_MAX) _t%d = _t%d - 1;"
                        " else { if (_t%d < 0) _t%d += _t%d; _t%d -= _t%d.excl; }",
                     te, tr,
                     te, te, tn,
                     te, te, tn, te, tr);
          buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, te, ti, fk, t, ti, tv, t);
        }
        else if (argc >= 2) {
          /* nil start / length are legal: "from the start" / "to the end" */
          buf_printf(b, " sp_int _t%d = ", ts); emit_int_expr_nilable(c, argv[1], b);
          buf_printf(b, "; if (_t%d < 0) _t%d += _t%d; if (_t%d < 0) _t%d = 0;", ts, ts, tn, ts, ts);
          if (argc == 3 && comp_ntype(c, argv[2]) == TY_NIL) {
            /* a nil LENGTH is "to the end": keep the array-length bound */
            buf_puts(b, " (void)("); emit_expr(c, argv[2], b); buf_puts(b, ");");
          }
          else if (argc == 3) {
            int tl = ++g_tmp;
            buf_printf(b, " sp_int _t%d = ", tl); emit_int_expr_nilable(c, argv[2], b);
            /* end = start+len; negative len = no-op (empty range) */
            buf_printf(b, "; if (_t%d < 0) _t%d = 0; _t%d = _t%d + _t%d;",
                       tl, tl, tn, ts, tl);
          }
          buf_printf(b, " for (sp_int _t%d = _t%d; _t%d < _t%d; _t%d++)"
                        " sp_%sArray_set(_t%d, _t%d, _t%d); _t%d; })",
                     ti, ts, ti, tn, ti, fk, t, ti, tv, t);
        }
        else {
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
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
      if (ty_is_array(init_t)) {
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
      /* a Float initial value folds to a Float (bare sp_float, not boxed) */
      if (init_t == TY_FLOAT) {
        buf_puts(b, "("); emit_float_expr(c, argv[0], b);
        buf_puts(b, " + sp_PolyArray_sum_float("); emit_expr(c, recv, b); buf_puts(b, "))");
        return 1;
      }
      /* an Integer (or poly) seed folds via sp_poly_add so Float/Rational/
         Bignum elements promote the result instead of being dropped by the
         int-only sum (matches the no-arg poly fold above) (#2959) */
      buf_puts(b, "sp_poly_add(");
      if (init_t == TY_POLY) emit_expr(c, argv[0], b);
      else emit_boxed(c, argv[0], b);
      buf_puts(b, ", sp_PolyArray_sum_poly("); emit_expr(c, recv, b); buf_puts(b, "))");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "cycle") && argc == 1 &&
        nt_ref(nt, id, "block") < 0 && comp_ntype(c, id) == TY_ENUMERATOR) {
      /* the call is typed as an Enumerator, so it has to BE one: materializing
         the repeated array here handed a poly array to sp_Enumerator_to_a,
         which read it as an Enumerator (#3617) */
      buf_puts(b, "sp_Enumerator_new_cycle("); emit_boxed(c, recv, b);
      buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (rt == TY_POLY_ARRAY && sp_streq(name, "cycle") && argc == 1 && nt_ref(nt, id, "block") < 0) {
      int t = ++g_tmp, tn2 = ++g_tmp, tr2 = ++g_tmp, tj = ++g_tmp, ti2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr2, tr2);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
      buf_printf(b, " sp_PolyArray_push(_t%d, _t%d->data[_t%d]);", tr2, t, ti2);
      buf_printf(b, " _t%d; })", tr2);
      return 1;
    }
    if (rt == TY_POLY_ARRAY && (sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) {
      int t = ++g_tmp, tn2 = ++g_tmp;
      buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn2); emit_int_expr(c, argv[0], b);
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
      /* dig(*keys): walk the runtime key list (see the hash arm) */
      if (nt_kind(nt, argv[0]) == NK_SplatNode) {
        buf_puts(b, "sp_poly_dig_list("); emit_boxed(c, recv, b);
        buf_puts(b, ", sp_poly_to_poly_array("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      if (argc == 1) {
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else {
        /* each later step goes through the dig-specific helper so a scalar
           intermediate raises TypeError instead of bit/char-indexing (#2983) */
        /* the key goes boxed: a Struct member can be named, and a String or
           Symbol reached the integer offset slot as a pointer (#3575) */
        for (int di = argc - 1; di >= 1; di--) buf_printf(b, "sp_poly_dig_step_key(");
        buf_puts(b, "sp_PolyArray_get("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        for (int di = 1; di < argc; di++) { buf_puts(b, ", "); emit_boxed(c, argv[di], b); buf_puts(b, ")"); }
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
        else if (at == TY_POLY || at == TY_UNKNOWN) {
          /* a boxed argument (a rest param widened to poly): unbox to the
             working array through the runtime kind dispatch (#3317); one
             that is no Array is CRuby's TypeError, not an empty list */
          buf_puts(b, "sp_poly_set_operand("); emit_boxed(c, argv[ai], b); buf_puts(b, ")");
        }
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
      buf_printf(b, "({ sp_int _t%d = sp_PolyArray_rindex(", t); emit_expr(c, recv, b);
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
          /* rooted, as the poly each_index above already roots its own hoist:
             the length is the loop bound and the block can allocate */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
          /* rooted, as the each_index hoist above is, and as the TY_POLY arm of
             this same pair of methods near the top of the file already is: the
             length is the loop bound, the element comes out of the receiver on
             every turn, and the block between two turns allocates */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          /* The result array is rooted for the same reason and by the same
             precedent: the TY_POLY arm of this pair roots its own result
             beside its receiver. It is built empty here and pushed into on
             every kept turn, so nothing but this temporary holds it while the
             block allocates. */
          buf_printf(g_pre, "sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);\n", ek, tout, ek, tout);
          if (is_drop) {
            emit_indent(g_pre, g_indent);
            buf_puts(g_pre, "{ sp_bool _dropping = 1;\n");
          }
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, ek, trecv, ti);
          char es_tw[64]; snprintf(es_tw, sizeof es_tw, "sp_%sArray_get(_t%d, _t%d)", ek, trecv, ti);
          if (emit_iter_autosplat(c, tw_blk, rt, es_tw, g_indent + 1)) { }
          else if (tw_bp) {
            /* The block parameter is an ordinary local, and its SLOT may have
               widened to poly -- another method of the receiver's name being
               widened is enough to get there. The loop then shadowed the
               sp_RbVal slot with a const char * of the element type, while
               everything READING the parameter (the predicate through
               sp_poly_truthy, any use in the body) is compiled against the
               slot's type. Bind the element boxed into the slot's own type
               instead, the way the for-loop variable is since #4168 (#4188). */
            Scope *twsc = comp_scope_of(c, tw_blk);
            LocalVar *twlv = twsc ? scope_local(twsc, tw_bp) : NULL;
            emit_indent(g_pre, g_indent + 1);
            if (twlv && twlv->type == TY_POLY && et != TY_POLY && et != TY_UNKNOWN) {
              buf_printf(g_pre, "sp_RbVal lv_%s = ", tw_bp);
              emit_boxed_text(c, et, es_tw, g_pre);
              buf_puts(g_pre, ";\n");
            }
            else {
              emit_ctype(c, et, g_pre);
              buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", tw_bp, ek, trecv, ti);
            }
          }
          Buf cb; memset(&cb, 0, sizeof cb);
          int tw_nx = emit_block_cond_next(c, tw_blk, g_indent + 1, &cb);
          if (!tw_nx) {
            for (int j = 0; j < tw_bn - 1; j++) emit_stmt(c, tw_bb[j], g_pre, g_indent + 1);
            int sv = g_indent; g_indent = g_indent + 1;
            cb = expr_buf(c, tw_bb[tw_bn - 1]); g_indent = sv;
          }
          /* a boxed block value is a struct, so `!(rbval)` is not valid C;
             Ruby's truthiness is what the condition wants anyway */
          if (!tw_nx && comp_ntype(c, tw_bb[tw_bn - 1]) == TY_POLY) {
            Buf tb2; memset(&tb2, 0, sizeof tb2);
            buf_printf(&tb2, "sp_poly_truthy(%s)", cb.p ? cb.p : "sp_box_nil()");
            free(cb.p); cb = tb2;
          }
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
      buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
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
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          /* Declare the block param in the loop body so the form is self-contained
             when this find is a parameter default hoisted to the call site (whose
             function has no top-level declaration for the block local). */
          char es_fd[64]; snprintf(es_fd, sizeof es_fd, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          int splat_fd = emit_iter_autosplat(c, fblock, rt, es_fd, g_indent + 1);
          if (!splat_fd && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          Buf cb; memset(&cb, 0, sizeof cb);
          if (!emit_block_cond_next(c, fblock, g_indent + 1, &cb)) {
            for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
            int sv = g_indent; g_indent++;
            emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
          }
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
            buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (sp_int[16]){0}), _sp_proc_poly_ret);\n",
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
       infer as a plain nullable sp_int -- return the bare SP_INT_NIL
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
        emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = SP_INT_NIL;\n", tres);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                   ti, ti, trecv, ti);
        /* Declare the block param in the loop body so the form is self-contained
           (same rationale as find/detect above); a |k, v| header destructures
           the pair element (#1876 family). */
        {
          char es_fi[64]; snprintf(es_fi, sizeof es_fi, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          int splat_fi = emit_iter_autosplat(c, fblock, rt, es_fi, g_indent + 1);
          if (!splat_fi && bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "sp_RbVal lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
        }
        Buf cb; memset(&cb, 0, sizeof cb);
        if (!emit_block_cond_next(c, fblock, g_indent + 1, &cb)) {
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          emit_cond(c, bb[bn - 1], &cb); g_indent = sv;
        }
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
      buf_printf(b, " sp_int _t%d = SP_INT_NIL;", tres);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, trecv, ti);
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
    {
      int block = nt_ref(nt, id, "block");
      /* bsearch { |x| cond } - find-minimum mode. Every array kind including
         the poly one, whose elements are already boxed (#2892). */
      if (sp_streq(name, "bsearch") && block >= 0) {
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          TyKind et = ty_array_elem(rt);
          const char *k = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
          if (!k) return 0;
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s; ", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          /* rooted, as the poly-array find_index above already roots its own
             hoist: a halving search still reads its element out of the receiver
             on every turn, and the block allocates between turns */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1;\n", tlo, thi, k, trecv);
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          buf_printf(g_pre, " _t%d = %s;", tres,
                     et == TY_INT ? "SP_INT_NIL" :
                     et == TY_FLOAT ? "sp_float_nil()" :
                     et == TY_POLY ? "sp_box_nil()" : "NULL");
          /* The running answer is lifted OUT of the receiver and held while the
             search narrows, so rooting the receiver does not cover it: a turn
             that drops the captured element from the array leaves this
             temporary as its only holder. The element type picks the macro --
             an Integer or Float answer roots to nothing, which is why
             bsearch_index needs none. */
          if (needs_root(et)) { buf_puts(g_pre, " "); emit_gc_root_tmp(c, et, tres, g_pre); }
          buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
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
            buf_printf(g_pre, "sp_int _t%d = %s;\n", tcmp, cb.p ? cb.p : "0");
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
         index of the first element satisfying the predicate, or nil. Kept here,
         before the typed-kind `if (k)` gate, so a poly array (one widened by
         flowing through a method param) reaches it too -- like bsearch (#3165). */
      if (sp_streq(name, "bsearch_index") && block >= 0) {
        const char *bk = (rt == TY_POLY_ARRAY) ? "Poly" : array_kind(rt);
        if (!bk) return 0;
        const char *bp = block_param_name(c, block, 0); if (bp) bp = rename_local(bp);
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn >= 1) {
          int trecv = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp, tres = ++g_tmp, tmid = ++g_tmp;
          Buf rbs = expr_buf(c, recv);
          emit_indent(g_pre, g_indent); emit_ctype(c, rt, g_pre);
          buf_printf(g_pre, " _t%d = %s; ", trecv, rbs.p ? rbs.p : "NULL"); free(rbs.p);
          /* rooted, as the poly-array find_index above already roots its own
             hoist: the element the block judges comes out of the receiver on
             every turn, and the block allocates between turns. The answer here
             is an index rather than an element, so it needs no root of its own. */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = 0, _t%d = sp_%sArray_length(_t%d) - 1, _t%d = SP_INT_NIL;\n", tlo, thi, bk, trecv, tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "while (_t%d <= _t%d) {\n", tlo, thi);
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_int _t%d = _t%d + (_t%d - _t%d) / 2;\n", tmid, tlo, thi, tlo);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, bk, trecv, tmid); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* The block value is the search predicate: route through emit_cond so a
             poly / nullable-scalar result becomes a valid C truthiness test rather
             than `if (sp_RbVal)` or `if (SP_INT_NIL)`. */
          Buf cb; memset(&cb, 0, sizeof cb);
          /* Integer-valued block: find-ANY mode (0 found, <0 left, >0 right),
             yielding the index. Boolean block: find-minimum, as before. */
          if (comp_ntype(c, bb[bn - 1]) == TY_INT) {
            Buf ib = expr_buf(c, bb[bn - 1]); g_indent = sv;
            int tcmp = ++g_tmp;
            emit_indent(g_pre, g_indent + 1);
            buf_printf(g_pre, "sp_int _t%d = %s;\n", tcmp, ib.p ? ib.p : "0");
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
    }
    if (k) {
      if ((sp_streq(name, "to_a") || sp_streq(name, "to_ary") || sp_streq(name, "entries") ||
           sp_streq(name, "deconstruct") || sp_streq(name, "flatten")) && argc == 0) {
        /* a scalar-element array can't nest: these are identity */
        emit_expr(c, recv, b); return 1;
      }
      /* compact is NOT identity: a scalar array still holds the nil sentinel a
         nullable read leaves behind, so `["a".rindex("/"), 1].compact` has to
         drop that first element rather than keep it. */
      if (sp_streq(name, "compact") && argc == 0) {
        buf_printf(b, "sp_%sArray_compact(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 1 && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) {
        /* arr[a..b] / arr[a...b] -> subarray */
        int rn = argv[0];
        int excl = (int)(nt_int(nt, rn, "flags", 0) & 4) ? 1 : 0;
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        buf_printf(b, "sp_%sArray_slice_range(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a poly bound (a destructured tuple element, #2923) unboxes here */
        if (lo >= 0) emit_int_expr(c, lo, b); else buf_puts(b, "0");
        buf_puts(b, ", ");
        if (hi >= 0) emit_int_expr(c, hi, b); else buf_puts(b, "-1");
        buf_printf(b, ", %d)", hi >= 0 ? excl : 0);
        return 1;
      }
      if (sp_streq(name, "[]") && argc == 2) {
        /* arr[start, len] -> subarray; a negative length is nil in CRuby
           (slice() itself would return the empty array), and so is a start
           outside [-len, len] (start == len is the empty slice) */
        int ta = ++g_tmp, ts = ++g_tmp, tl = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_int _t%d = ", ts); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = ", tl); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d)", tn, k, ta);
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
        buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " sp_int _t%d = _t%d.first == INTPTR_MIN ? 0 :"
                      " (_t%d.first < 0 ? _t%d.first + _t%d : _t%d.first);",
                   tf, tr, tr, tr, tn, tr);
        buf_printf(b, " sp_int _t%d = _t%d.last == INTPTR_MAX ? _t%d - _t%d :"
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
          /* sp_poly_to_i, not a raw `.v.i`: the union read assumed the box
             held an Integer, so a boxed user object indexed by its pointer
             bits and the read answered a wrong element in silence. */
          buf_puts(b, "sp_poly_to_i(");
          emit_expr(c, argv[0], b);
          buf_puts(b, ")");
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
        int same = 1, all_poly = 1;
        for (int j = 0; j < argc; j++) {
          /* Ask the SLOT, not the node: a block parameter's node type can
             read as the element kind while its declaration is boxed, and
             emitting the boxed local into a typed pointer does not compile
             (#3850). */
          TyKind at = comp_ntype(c, argv[j]);
          if (nt_type(nt, argv[j]) && sp_streq(nt_type(nt, argv[j]), "LocalVariableReadNode")) {
            Scope *asc = comp_scope_of(c, argv[j]);
            LocalVar *alv = asc ? scope_local(asc, nt_str(nt, argv[j], "name")) : NULL;
            if (alv && alv->type != TY_UNKNOWN) at = alv->type;
          }
          if (at != rt) same = 0;
          if (at != TY_POLY) all_poly = 0;
        }
        /* A boxed argument -- an element read out of a poly array, which is
           what `g.each_with_object([]) { |r, acc| acc.concat(r) }` hands it --
           is an array at run time; reading it as this array's C type did not
           compile (#3850). Append its elements through the boxed surface. */
        if (!same && all_poly && k) {
          int ta = ++g_tmp;
          Buf ra = expr_buf(c, recv);
          buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);", k, ta, ra.p ? ra.p : "NULL", ta);
          free(ra.p);
          for (int j = 0; j < argc; j++) {
            int tv = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp;
            buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[j], b);
            buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tv);
            buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", tn, tv);
            buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", ti, ti, tn, ti);
            buf_printf(b, " sp_%sArray_push(_t%d, ", k, ta);
            { char el[64]; snprintf(el, sizeof el, "sp_poly_each_elem(_t%d, _t%d)", tv, ti);
              emit_unbox_text(c, ty_array_elem(rt), el, b); }
            buf_puts(b, ");");
          }
          buf_printf(b, " _t%d; })", ta);
          return 1;
        }
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
            buf_printf(b, " { sp_int _t%d = sp_%sArray_length(_t%d);"
                          " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
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
        buf_printf(b, " sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " sp_int _t%d = sp_%sArray_length(_t%d);", tn, k, ta);
        buf_printf(b, " sp_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
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
          buf_printf(b, "sp_%sArray_get(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else {
          /* multi-step: hand the whole key list to the runtime walk, which
             stops at nil and raises TypeError on a step that cannot be dug.
             Chaining index reads instead read the scalar the first step
             answered as if it were an array, so `[1].dig(0, 0)` answered 1
             where Ruby raises (#3825). */
          buf_puts(b, "sp_poly_dig_n(sp_box_obj(");
          emit_expr(c, recv, b);
          buf_printf(b, ", SP_BUILTIN_%s_ARRAY), %d, (sp_RbVal[]){",
                     rt == TY_INT_ARRAY ? "INT" : rt == TY_FLOAT_ARRAY ? "FLT" : "STR", argc);
          for (int di = 0; di < argc; di++) { if (di) buf_puts(b, ", "); emit_boxed(c, argv[di], b); }
          buf_puts(b, "})");
        }
        return 1;
      }
      if (sp_streq(name, "+") && argc == 1 && a0 == rt) {
        /* array + array of the same kind -> a fresh concatenation */
        buf_printf(b, "sp_%sArray_concat(", k);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        emit_expr(c, argv[0], b);
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
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k, tL, ti);
          emit_indent(g_pre, g_indent + 1);
          if (rt == TY_POLY_ARRAY)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));\n", tO, tL, ti);
          else if (box_l)
            buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s(sp_%sArray_get(_t%d, _t%d)));\n", tO, box_l, k, tL, ti);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)\n", ti, ti, k2, tR, ti);
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
        buf_printf(b, "; sp_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_%sArray *_t%d = sp_%sArray_new(); SP_GC_ROOT(_t%d);", k, tr2, k, tr2);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)", tj, tj, tn2, tj);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti2, ti2, t, ti2);
        buf_printf(b, " sp_%sArray_push(_t%d, sp_%sArray_get(_t%d, _t%d));", k, tr2, k, t, ti2);
        buf_printf(b, " _t%d; })", tr2);
        return 1;
      }
      if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) {
        /* pop(n)/shift(n): the removed subarray, via the slice! splice
           (pop takes the tail, shift the head; n clamps to the length) */
        int t = ++g_tmp, tn2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_int _t%d = ", tn2); emit_int_expr(c, argv[0], b);
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
            buf_printf(b, " sp_IntArray_unshift(_t%d, ", t); emit_int_expr(c, argv[a], b); buf_puts(b, ");");
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
            buf_printf(b, " sp_float _u%d_%d = ", t, a); emit_float_expr(c, argv[a], b); buf_puts(b, ";");
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
        for (int j = 0; j < nargs; j++) {
          tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]);
        }
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
          /* A scalar argument responds to no :each at all, which is CRuby's
             TypeError naming its class. Read as a container regardless, a nil
             became a column of nils, silently, and an Integer or a String
             stopped the C build. */
          if (ty_is_object(at[j])) {
            /* an object answering #to_ary zips as that Array; one answering
               #each enumerates; any other is the scalar's TypeError */
            int zdef = -1;
            TyKind zk = obj_container_conv(c, at[j], "to_ary", &zdef);
            if (zk != TY_UNKNOWN) {
              const char *kz = zk == TY_POLY_ARRAY ? "Poly" : array_kind(zk);
              buf_printf(b, " sp_%sArray *_t%d = ", kz ? kz : "Poly", tb[j]);
              emit_obj_container_conv(c, argv[j], zdef, "to_ary", b);
              /* rooted: the answer is the conversion's own allocation, read
                 across every row the loop below allocates */
              buf_printf(b, "; SP_GC_ROOT(_t%d);", tb[j]);
              at[j] = kz ? zk : TY_POLY_ARRAY;
              continue;
            }
            int zcid = ty_object_class(at[j]);
            if (zcid >= 0 && comp_method_in_chain(c, zcid, "each", NULL) < 0) {
              /* sp_zip_arg would send :each and answer NoMethodError; the
                 class is settled, so name CRuby's TypeError here */
              buf_printf(b, " sp_PolyArray *_t%d = ({ (void)(", tb[j]); emit_expr(c, argv[j], b);
              buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (must respond to :each)\"); (sp_PolyArray *)0; });",
                         class_ruby_name(c, zcid));
              at[j] = TY_POLY_ARRAY;
              continue;
            }
          }
          if (at[j] == TY_NIL || at[j] == TY_BOOL || at[j] == TY_INT ||
              at[j] == TY_FLOAT || at[j] == TY_STRING || at[j] == TY_STRBUF ||
              at[j] == TY_SYMBOL || at[j] == TY_VOID || ty_is_object(at[j]) ||
              /* a Hash or an Enumerator DOES respond to :each; the same helper
                 materializes it, where the typed line below spelled the slot
                 sp_PolyArray* and assigned an sp_SymPolyHash* to it */
              ty_is_hash(at[j]) || at[j] == TY_ENUMERATOR) {
            buf_printf(b, " sp_PolyArray *_t%d = sp_zip_arg(", tb[j]);
            emit_boxed(c, argv[j], b);
            buf_puts(b, ");");
            at[j] = TY_POLY_ARRAY;
            continue;
          }
          /* a boxed (poly) argument -- e.g. an outer block param that holds an
             array at runtime -- must be unboxed to a poly array, not assigned
             raw into an sp_PolyArray* slot (#3190). */
          if (at[j] == TY_POLY) {
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tb[j]);
            emit_expr(c, argv[j], b);
            buf_puts(b, ");");
            at[j] = TY_POLY_ARRAY;
            continue;
          }
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {",
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
        /* an empty `[]` argument has no element type of its own and would emit
           as the int-array default, which this arm then reads as the poly array
           it dispatches on (#3975 sweep) */
        if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")) {
          int aen = 0; nt_arr(nt, argv[0], "elements", &aen);
          if (aen == 0 && (comp_ntype(c, argv[0]) == TY_UNKNOWN ||
                           ty_is_array(comp_ntype(c, argv[0]))))
            c->ntype[argv[0]] = TY_POLY_ARRAY;
        }
        TyKind at = comp_ntype(c, argv[0]);
        Buf ra; memset(&ra, 0, sizeof ra);
        emit_expr(c, recv, &ra);  /* the receiver's prelude first, as Ruby evaluates */
        Buf rb2; memset(&rb2, 0, sizeof rb2);
        at = emit_product_operand(c, argv[0], at, &rb2);
        const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        const char *fp0 = block_param_name(c, blk, 0);
        int ta = ++g_tmp, tb = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);",
                   k, ta, ra.p ? ra.p : "NULL", ta, kb, tb, rb2.p ? rb2.p : "NULL", tb);
        free(ra.p); free(rb2.p);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d); sp_PolyArray_push(_t%d, ", te, te, te);
        char ee[96]; snprintf(ee, sizeof ee, "sp_%sArray_get(_t%d, _t%d)", k, ta, ti);
        emit_boxed_text(c, ty_array_elem(rt), ee, b);
        buf_printf(b, "); sp_PolyArray_push(_t%d, sp_box_poly_array(_t%d)); }", tr, te);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "product") && argc == 1) {
        TyKind at = comp_ntype(c, argv[0]);
        Buf ra; memset(&ra, 0, sizeof ra);
        emit_expr(c, recv, &ra);  /* the receiver's prelude first, as Ruby evaluates */
        Buf rb2; memset(&rb2, 0, sizeof rb2);
        at = emit_product_operand(c, argv[0], at, &rb2);
        const char *kb = (at == TY_POLY_ARRAY) ? "Poly" : (array_kind(at) ? array_kind(at) : "Poly");
        int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, ti = ++g_tmp, tj = ++g_tmp, tpair = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d); sp_%sArray *_t%d = %s; SP_GC_ROOT(_t%d);",
                   k, ta, ra.p ? ra.p : "NULL", ta, kb, tb, rb2.p ? rb2.p : "NULL", tb);
        free(ra.p); free(rb2.p);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyArray *_t%d = NULL;", tpair);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", ti, ti, k, ta, ti);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {", tj, tj, kb, tb, tj);
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
        if (argc == 1) emit_int_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);   /* argless permutation: full length */
        /* the combinations are only in this temp until the loop below boxes
           them, and the array it boxes them into allocates first */
        buf_printf(b, "); SP_GC_ROOT(_t%d);", tc);
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tout, tout);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tc, ti);
        buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_int_array(_t%d->data[_t%d]));", tout, tc, ti);
        /* blockless: an Enumerator over those tuples (#3614) */
        buf_printf(b, " sp_Enumerator_new_from(sp_box_poly_array(_t%d)); })", tout);
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
        buf_printf(b, "); SP_GC_ROOT(_t%d); ", ta);
        buf_puts(b, "sp_Enumerator_new_from(sp_box_poly_array(");
        buf_printf(b, "%s(_t%d, ", combfn, ta);
        if (argc == 1) emit_expr(c, argv[0], b);
        else buf_printf(b, "_t%d ? _t%d->len : 0", ta, ta);
        buf_puts(b, ")))");
        buf_puts(b, "; })");
        return 1;
      }
      if (sp_streq(name, "rotate!") && argc <= 1) {
        int t = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sArray_rotate_bang(_t%d, ", k, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
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
      /* insert(i) with no values leaves the array as it is and answers it;
         only the value-carrying form had an emitter (#3855) */
      if (sp_streq(name, "insert") && argc == 1) {
        int t0 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t0); emit_expr(c, recv, b);
        buf_puts(b, "; (void)("); emit_int_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t0);
        return 1;
      }
      if (sp_streq(name, "insert") && argc >= 2 && (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) {
        /* insert(i, v1, v2, ...): normalize a negative index ONCE against the
           pre-insert length (per-element normalization would drift as the
           array grows), then insert consecutively. */
        int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_int _t%d = ", ti2); emit_int_expr(c, argv[0], b);
        /* normalize ONCE, keeping the too-negative IndexError the runtime
           helper would have raised (it must not see a pre-added index) */
        buf_printf(b, "; sp_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
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
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b); buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "delete") && argc == 1 &&
          (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
        /* A Float receiver classifies its needle here (the delete arm's form
           of elem_mismatch): only an Integer or Float compares. A Rational or
           a bare {} needle broke the C build inside emit_float_expr, and a
           boxed needle rides the tag-guarded helper instead of a lenient
           to-f coercion. Bignum, Rational and Complex miss where CRuby's ==
           can match -- the Int arm fails the C build on those same shapes. */
        int df_boxed = rt == TY_FLOAT_ARRAY && (a0 == TY_POLY || a0 == TY_UNKNOWN);
        int df_never = rt == TY_FLOAT_ARRAY && !df_boxed && a0 != TY_INT && a0 != TY_FLOAT;
        int dblk = nt_ref(nt, id, "block");
        if (dblk >= 0 && nt_type(nt, dblk) && sp_streq(nt_type(nt, dblk), "BlockNode")) {
          int dbody = nt_ref(nt, dblk, "body");
          int dbn = 0; const int *dbb = dbody >= 0 ? nt_arr(nt, dbody, "body", &dbn) : NULL;
          if (value_obj_compares(c, argv[0])) {
            unsupported_feature(c, id, "Array#delete of a user object defining == from a typed Array");
            return 0;
          }
          if (dbn >= 1 && (df_never || value_kind_misses(c, argv[0], ty_array_elem(rt)))) {
            /* nothing to delete: the block, handed the value, supplies the answer */
            const char *dp0 = block_param_name(c, dblk, 0);
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
            if (dp0) { buf_printf(b, "lv_%s = ", rename_local(dp0)); emit_boxed(c, argv[0], b); buf_puts(b, "; "); }
            else { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
            emit_boxed(c, dbb[dbn - 1], b); buf_puts(b, "; })");
            return 1;
          }
          if (dbn >= 1) {
            int tdr = ++g_tmp;
            if (rt == TY_INT_ARRAY) {
              buf_printf(b, "({ sp_int _t%d = sp_IntArray_delete(", tdr);
              emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b);
              buf_printf(b, "); _t%d != SP_INT_NIL ? sp_box_int(_t%d) : ", tdr, tdr);
            }
            else if (rt == TY_FLOAT_ARRAY) {
              buf_printf(b, "({ sp_float _t%d = sp_FloatArray_delete%s(", tdr, df_boxed ? "_key" : "");
              emit_expr(c, recv, b); buf_puts(b, ", ");
              if (df_boxed) emit_boxed(c, argv[0], b); else emit_float_expr(c, argv[0], b);
              buf_printf(b, "); !sp_float_is_nil(_t%d) ? sp_box_float(_t%d) : ", tdr, tdr);
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
        if (value_obj_compares(c, argv[0])) {
          unsupported_feature(c, id, "Array#delete of a user object defining == from a typed Array");
          return 0;
        }
        if (df_never || value_kind_misses(c, argv[0], ty_array_elem(rt))) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
          buf_printf(b, "); %s; })", rt == TY_INT_ARRAY ? "SP_INT_NIL" : rt == TY_STR_ARRAY ? "(const char *)0" : "sp_float_nil()");
          return 1;
        }
        buf_printf(b, "sp_%sArray_delete%s(", k, df_boxed ? "_key" : ""); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (df_boxed) emit_boxed(c, argv[0], b);
        else if (rt == TY_FLOAT_ARRAY) emit_float_expr(c, argv[0], b);
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
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
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        /* slice!(range): normalize begin/length against the live length */
        int ta = ++g_tmp, tr = ++g_tmp, tf = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = ", k, ta); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Range _t%d = ", tr); emit_expr(c, argv[0], b);
        /* a beginless bound starts at 0 and an endless one runs to the end,
           the same sentinels Array#[] resolves (#3835) */
        buf_printf(b, "; sp_int _t%d = _t%d.first == INTPTR_MIN ? 0"
                      " : (_t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first);",
                   tf, tr, tr, tr, ta, ta, tr);
        buf_printf(b, " sp_int _t%d = _t%d.last == INTPTR_MAX ? ((_t%d ? _t%d->len : 0) - _t%d)"
                      " : ((_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1));",
                   tn, tr, ta, ta, tf, tr, tr, ta, ta, tr, tf, tr);
        buf_printf(b, " sp_%sArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", k, ta, tf, tn, tn);
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1) {
        /* slice!(i): remove and return the element (nil sentinel on miss) */
        buf_printf(b, "sp_%sArray_delete_at(", k); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      int block = nt_ref(nt, id, "block");
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rfi.p ? rfi.p : "NULL"); free(rfi.p);
          /* rooted, as the poly-array find_index above already roots its own
             hoist. rindex takes its bound once and then counts down, so a
             collection mid-walk shows up in the elements rather than in the
             turn count. */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = SP_INT_NIL;\n", tres);
          emit_indent(g_pre, g_indent);
          if (sp_streq(name, "rindex"))
            buf_printf(g_pre, "for (sp_int _t%d = sp_%sArray_length(_t%d) - 1; _t%d >= 0; _t%d--) {\n",
                       ti, k, trecv, ti, ti);
          else
            buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                       ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          Buf cb; memset(&cb, 0, sizeof cb);
          if (!emit_block_cond_next(c, block, g_indent + 1, &cb)) {
            for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
            int sv = g_indent; g_indent++;
            cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          }
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
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
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
          buf_printf(g_pre, "if (!_tf%d) _t%d = ((void)sp_proc_call(_t%d, 0, (sp_int[16]){0}), _sp_proc_poly_ret);\n",
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
          /* rooted, as the find(ifnone) arm above and the poly-array find
             already are: same loop, same per-turn reads, same allocating block */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent); emit_ctype(c, et, g_pre);
          if (et == TY_STRING) buf_printf(g_pre, " _t%d = NULL;\n", tres);
          else if (et == TY_INT) buf_printf(g_pre, " _t%d = SP_INT_NIL;\n", tres);
          else buf_printf(g_pre, " _t%d = 0;\n", tres);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          /* Declare the block param in the loop body (not a bare assignment) so
             the find is self-contained: when this call is a parameter default
             hoisted to the call site, the enclosing function has no top-level
             declaration for the block local. Shadows the method-scope slot in
             the ordinary in-body case, which is harmless. */
          if (bp) { emit_indent(g_pre, g_indent + 1); emit_ctype(c, et, g_pre); buf_printf(g_pre, " lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          Buf cb; memset(&cb, 0, sizeof cb);
          if (!emit_block_cond_next(c, block, g_indent + 1, &cb)) {
            for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
            int sv = g_indent; g_indent++;
            cb = expr_buf(c, bb[bn - 1]); g_indent = sv;
          }
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
          /* rooted, as the TY_POLY map!/collect! near the top of this file
             already roots its own hoist: this loop WRITES the block value back
             into the receiver on every turn, so an unrooted hoist is a store
             into freed memory and not only a short walk */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
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
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond }: the
         in-place filter, on a typed or a poly array (emit_array_filter_loop) */
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && block >= 0) {
        const char *kk = (rt == TY_POLY_ARRAY) ? "Poly" : k;
        int trecv, torig, twp;
        if (kk && emit_array_filter_loop(c, recv, block, rt, name, g_pre, g_indent, &trecv, &torig, &twp)) {
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
        buf_printf(b, " sp_int _t%d = 0;", tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
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
        buf_printf(b, " sp_int _t%d = 0;", tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        buf_printf(b, " if (sp_re_match(sp_re_pat_%d, sp_StrArray_get(_t%d, _t%d)) >= 0) _t%d++;", rei, ta, ti, tc);
        if (sp_streq(name, "all?"))       buf_printf(b, " _t%d == sp_StrArray_length(_t%d); })", tc, ta);
        else if (sp_streq(name, "any?"))  buf_printf(b, " _t%d > 0; })", tc);
        else if (sp_streq(name, "none?")) buf_printf(b, " _t%d == 0; })", tc);
        else                              buf_printf(b, " _t%d == 1; })", tc);
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") || sp_streq(name, "none?") ||
           sp_streq(name, "one?") || sp_streq(name, "count")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0 &&
          comp_ntype(c, argv[0]) == TY_CLASS) {
        /* A class argument on a TYPED array: the predicates ask `Class === e`,
           which for an int/float/string array is decided by the element type
           alone, and #count asks `e == Class`, which no element of one can
           satisfy. Emitting the class into the element slot did not even
           compile (#3817). */
        int cls_all = 0;
        { const char *cn2 = isa_const_name(nt, argv[0]);
          const char *want = rt == TY_INT_ARRAY ? "Integer" : rt == TY_FLOAT_ARRAY ? "Float"
                           : rt == TY_STR_ARRAY ? "String" : NULL;
          cls_all = (cn2 && want && (sp_streq(cn2, want) || sp_streq(cn2, "Object") ||
                                     sp_streq(cn2, "Comparable") ||
                                     (sp_streq(cn2, "Numeric") &&
                                      (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY)))); }
        int tlen = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = sp_%sArray_length(", tlen, k);
        emit_expr(c, recv, b); buf_puts(b, ");");
        if (sp_streq(name, "count")) buf_printf(b, " (void)_t%d; (sp_int)0; })", tlen);
        else if (sp_streq(name, "all?"))
          buf_printf(b, cls_all ? " (void)_t%d; TRUE; })" : " _t%d == 0; })", tlen);
        else if (sp_streq(name, "any?"))
          buf_printf(b, cls_all ? " _t%d > 0; })" : " (void)_t%d; FALSE; })", tlen);
        else if (sp_streq(name, "none?"))
          buf_printf(b, cls_all ? " _t%d == 0; })" : " (void)_t%d; TRUE; })", tlen);
        else
          buf_printf(b, cls_all ? " _t%d == 1; })" : " (void)_t%d; FALSE; })", tlen);
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
        if (value_obj_compares(c, argv[0])) {
          unsupported_feature(c, id, "a user object defining == compared against a typed Array's elements");
          return 0;
        }
        if (value_kind_misses(c, argv[0], ty_array_elem(rt))) {
          /* a value no element can equal: only an empty array is all? of it */
          buf_puts(b, " (void)("); emit_expr(c, argv[0], b); buf_puts(b, ");");
          if (sp_streq(name, "all?"))       buf_printf(b, " sp_%sArray_length(_t%d) == 0; })", k, ta);
          else if (sp_streq(name, "none?")) buf_puts(b, " 1; })");
          else                              buf_puts(b, " 0; })");
          return 1;
        }
        buf_printf(b, " "); emit_ctype(c, ty_array_elem(rt), b);
        buf_printf(b, " _t%d = ", tv); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " sp_int _t%d = 0;", tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++)", ti, ti, k, ta, ti);
        if (rt == TY_STR_ARRAY)
          buf_printf(b, " if (sp_str_cmp_bytes(sp_%sArray_get(_t%d, _t%d), _t%d) == 0) _t%d++;", k, ta, ti, tv, tc);
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
          buf_printf(g_pre, " _t%d = %s; ", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          /* rooted, as the find(ifnone) arm above already is: the same walk
             over the same hoist, differing only in what it does with the
             predicate */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_%sArray_length(_t%d); _t%d++) {\n",
                     ti, ti, k, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_%sArray_get(_t%d, _t%d);\n", bp, k, trecv, ti); }
          Buf vb2; memset(&vb2, 0, sizeof vb2);
          if (!emit_block_cond_next(c, blk, g_indent + 1, &vb2)) {
            for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
            int saveI = g_indent; g_indent = g_indent + 1;
            /* The block value is a condition: route through emit_cond so a poly /
               nil / scalar predicate becomes a valid C truthiness test (e.g.
               `count(&:alive)` where the element method is poly-dispatched would
               otherwise emit `if (sp_RbVal)` -- a struct in scalar position). */
            emit_cond(c, bb2[bn2 - 1], &vb2);
            g_indent = saveI;
          }
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
      /* A blockless SEEDLESS sum over Strings adds each element to the implied
         Integer 0, which CRuby rejects with "String can't be coerced into
         Integer". There is no sp_StrArray_sum, so the generic arms emitted a
         call to a function that does not exist and the C compiler stopped on
         its implicit declaration (#4327). An EMPTY receiver adds nothing and
         answers the 0, which is why the test is at run time. A seed of any
         class takes the boxed fold below, which reaches the same raise through
         the operator itself. */
      if (sp_streq(name, "sum") && rt == TY_STR_ARRAY && argc == 0 &&
          nt_ref(nt, id, "block") < 0) {
        int ts = ++g_tmp;
        buf_printf(b, "({ sp_StrArray *_t%d = ", ts); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); if (sp_StrArray_length(_t%d) != 0)"
                      " sp_raise_cls(\"TypeError\", \"String can't be coerced into Integer\"); ", ts, ts);
        buf_puts(b, "sp_box_int(0); })");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 0 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", 0)");
        return 1;
      }
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        TyKind init_t = fold_seed_ntype(c, argv[0]);
        /* a String initial value concatenates (["a","b"].sum("") == "ab") */
        if (rt == TY_STR_ARRAY && init_t == TY_STRING) {
          buf_puts(b, "sp_StrArray_sum_str("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        /* a float initial value promotes an integer-array sum to Float: add the
           float init to the integer total in floating point (sp_IntArray_sum
           returns sp_int, so accumulating the init through it would truncate). */
        if (rt == TY_INT_ARRAY && init_t == TY_FLOAT) {
          buf_puts(b, "((sp_float)("); emit_expr(c, argv[0], b);
          buf_puts(b, ") + (sp_float)sp_IntArray_sum("); emit_expr(c, recv, b); buf_puts(b, ", 0))");
          return 1;
        }
        /* Any other seed keeps its OWN class for the whole fold: CRuby's
           accumulator is the seed object and every step is Ruby's `+` on it. A
           Rational seed therefore answers a Rational total, a Bignum one stops
           wrapping into an sp_int, and nil / a String / an Array reach the raise
           that operator itself produces -- worded for the ELEMENT's class, which
           the hard-coded String-seed raise that used to stand here could only
           get right over Integers. sp_poly_sum_seed runs CRuby's own phases. */
        if (rt == TY_STR_ARRAY || !fold_seed_typed(init_t, ty_array_elem(rt))) {
          emit_poly_sum_seed(c, recv, argv[0], b);
          return 1;
        }
        /* A FLOAT seed over Floats does not compensate: CRuby reaches the
           compensated loop only out of the exact phase, and a seed that is
           already a Float never has one. An INTEGER seed does have one, so it
           keeps the compensated call -- `[0.1, 0.2, 0.3].sum(0)` is 0.6 and
           `.sum(0.0)` is 0.6000000000000001. The boxed fold draws the same
           line; the two paths must not disagree. */
        if (rt == TY_FLOAT_ARRAY && init_t == TY_FLOAT) {
          buf_puts(b, "sp_FloatArray_sum_plain("); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        buf_printf(b, "sp_%sArray_sum(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_FLOAT_ARRAY && init_t == TY_INT) {
          buf_puts(b, "(sp_float)("); emit_expr(c, argv[0], b); buf_puts(b, ")");
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
        /* nil is a legal separator (it means ""); false is not. The raw
           emit_expr passed both straight into the const char* slot, and the
           join then read a NULL as a string -- a segfault for either. */
        else if (argc == 1) emit_str_expr_nilable(c, argv[0], b);
        else buf_puts(b, "sp_str_empty");
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
        buf_printf(b, "({ sp_int _t%d = ", tn0); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\"); sp_%sArray_slice(", tn0, k);
        emit_expr(c, recv, b);
        buf_printf(b, ", 0, _t%d); })", tn0);
        return 1;
      }
      if (sp_streq(name, "last") && argc == 1) {
        /* slice's negative start counts from the end -> the last n elements;
           a negative count is an ArgumentError in CRuby */
        int tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
      /* a typed array never holds an element of another kind: include? is
         false and index is nil, with both operands still evaluated */
      int elem_mismatch = 0;
      if (argc == 1 && rt == TY_STR_ARRAY && a0 != TY_STRING && a0 != TY_UNKNOWN && a0 != TY_POLY) elem_mismatch = 1;
      if (argc == 1 && (rt == TY_INT_ARRAY || rt == TY_FLOAT_ARRAY) &&
          a0 != TY_INT && a0 != TY_FLOAT && a0 != TY_UNKNOWN && a0 != TY_POLY) elem_mismatch = 1;
      if ((sp_streq(name, "index") || sp_streq(name, "find_index") || sp_streq(name, "rindex")) && argc == 1 &&
          (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY || rt == TY_FLOAT_ARRAY)) {
        if (elem_mismatch) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_puts(b, "), (void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_box_nil())");
          return 1;
        }
        /* nil-on-miss -> poly */
        const char *fn = sp_streq(name, "rindex") ? "rindex_poly" : "index_poly";
        if (value_obj_compares(c, argv[0])) {
          unsupported_feature(c, id, "Array#index of a user object defining == in a typed Array");
          return 0;
        }
        if (value_kind_misses(c, argv[0], ty_array_elem(rt))) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
          buf_puts(b, "); sp_box_nil(); })");
          return 1;
        }
        if (rt == TY_FLOAT_ARRAY && (a0 == TY_POLY || a0 == TY_UNKNOWN)) {
          /* boxed needle: the tag-guarded helper compares Float and Integer
             tags and misses every other, where a to-f coercion made false hits */
          buf_printf(b, "sp_FloatArray_%s(", sp_streq(name, "rindex") ? "rindex_key" : "index_key");
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        buf_printf(b, "sp_%sArray_%s(", k, fn);
        emit_expr(c, recv, b); buf_puts(b, ", ");
        if (rt == TY_INT_ARRAY) emit_int_expr(c, argv[0], b);
        else if (rt == TY_FLOAT_ARRAY) emit_float_expr(c, argv[0], b);
        else emit_expr(c, argv[0], b);
        buf_puts(b, ")");
        return 1;
      }
      if ((sp_streq(name, "include?") || sp_streq(name, "member?")) && argc == 1) {
        if (elem_mismatch) {
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
      /* typed-array receiver, POLY argument (a value whose static type widened,
         not a poly array): coerce it at run time -- an Array becomes the poly
         array the set-op primitives take, anything else raises the TypeError
         CRuby raises. Without this arm the call had nowhere to go and `&`/`|`
         failed to compile (#3475). */
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
          a0 == TY_POLY) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        const char *conv_l = rt == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                             rt == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : "sp_FloatArray_to_poly";
        buf_printf(b, "sp_PolyArray_%s(%s(", fn, conv_l); emit_expr(c, recv, b);
        buf_puts(b, "), sp_poly_set_operand("); emit_expr(c, argv[0], b);
        buf_puts(b, "))"); return 1;
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
      if (sp_streq(name, "intersect?") && argc == 1 &&
          (a0 == rt || a0 == TY_UNKNOWN || ty_is_array(a0) || a0 == TY_POLY)) {
        /* Ruby has one Array; the storage kinds are ours. A receiver and an
           argument of different kinds -- a mapped String array against a
           poly-array constant, the shape this turned up in -- go through the
           generic comparison rather than declining to a NoMethodError. */
        if (a0 == rt) {
          buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", ");
          emit_expr(c, argv[0], b);
          buf_puts(b, ")");
          return 1;
        }
        if (a0 == TY_UNKNOWN) {
          buf_printf(b, "sp_%sArray_intersect_p(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
          return 1;
        }
        buf_puts(b, "sp_PolyArray_intersect_p(sp_poly_to_poly_array(");
        { Buf rb2; memset(&rb2, 0, sizeof rb2); emit_expr(c, recv, &rb2);
          emit_boxed_text(c, rt, rb2.p ? rb2.p : "NULL", b); free(rb2.p); }
        buf_puts(b, "), sp_poly_to_poly_array(");
        { Buf ab2; memset(&ab2, 0, sizeof ab2); emit_expr(c, argv[0], &ab2);
          if (a0 == TY_POLY) buf_puts(b, ab2.p ? ab2.p : "sp_box_nil()");
          else emit_boxed_text(c, a0, ab2.p ? ab2.p : "NULL", b);
          free(ab2.p); }
        buf_puts(b, "))");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_printf(b, "sp_%sArray_union(", k); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      /* intersection / difference with no argument fold over nothing: a copy
         of the receiver, the way the union form already answered (#3851) */
      if ((sp_streq(name, "intersection") || sp_streq(name, "difference")) && argc == 0) {
        buf_printf(b, "sp_%sArray_dup(", k); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* fetch_values with no keys reads nothing: an empty Array */
      if (sp_streq(name, "fetch_values") && argc == 0) {
        buf_printf(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), sp_%sArray_new())", k);
        return 1;
      }
      if (sp_streq(name, "sample") &&
          (argc == 0 || (argc == 1 && nt_type(nt, argv[0]) &&
                         sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")))) {
        /* sample or sample(random: rng): one element (the RNG kwarg uses the
           global generator here) (#2970) */
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
        buf_printf(b, "; sp_int _t%d = ", ts2); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = ", tl2); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; sp_int _t%d = sp_%sArray_length(_t%d)", tn2, k, ta2);
        buf_printf(b, "; (_t%d < 0 || _t%d > _t%d || _t%d < -_t%d) ? (sp_%sArray *)0 : sp_%sArray_slice(_t%d, _t%d, _t%d); })",
                   tl2, ts2, tn2, ts2, tn2, k, k, ta2, ts2, tl2);
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_shuffle(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                      " sp_%sArray_slice(_t%d, 0, _t%d); })", tn, k, t, tn);
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && block < 0) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_%sArray *_t%d = sp_%sArray_sort(", k, t, k); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
        if (sp_streq(name, "max")) buf_printf(b, " sp_%sArray_reverse_bang(_t%d);", k, t);
        buf_printf(b, " sp_%sArray_slice(_t%d, 0, _t%d); })", k, t, tn);
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
        /* Spill the receiver: evaluating the operand can allocate, and until
           the concat runs the receiver is in nothing but this temp. */
        int t = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_concat(_t%d, ", t, t);
        emit_expr(c, argv[0], b); buf_puts(b, "); })");
        return 1;
      }
      /* poly_array + typed array: box the typed operand to poly, then concat. */
      if (sp_streq(name, "+") && argc == 1 && ty_is_array(a0) && a0 != TY_POLY_ARRAY) {
        const char *conv = a0 == TY_INT_ARRAY ? "sp_IntArray_to_poly" :
                           a0 == TY_FLOAT_ARRAY ? "sp_FloatArray_to_poly" :
                           a0 == TY_STR_ARRAY ? "sp_StrArray_to_poly_fmt" : NULL;
        if (conv) {
          int t = ++g_tmp;
          buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
          buf_printf(b, "; SP_GC_ROOT(_t%d); sp_PolyArray_concat(_t%d, %s(", t, t, conv);
          emit_expr(c, argv[0], b); buf_puts(b, ")); })");
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
      /* poly-array receiver, POLY argument: same run-time coercion (#3475) */
      if ((sp_streq(name, "&") || sp_streq(name, "intersection") ||
           sp_streq(name, "|") || sp_streq(name, "union") ||
           sp_streq(name, "-") || sp_streq(name, "difference")) && argc == 1 &&
          a0 == TY_POLY) {
        const char *fn = (sp_streq(name, "&") || sp_streq(name, "intersection")) ? "intersect" : (sp_streq(name, "|") || sp_streq(name, "union") ? "union" : "difference");
        buf_printf(b, "sp_PolyArray_%s(", fn);
        emit_expr(c, recv, b); buf_puts(b, ", sp_poly_set_operand(");
        emit_expr(c, argv[0], b); buf_puts(b, "))"); return 1;
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
      if (sp_streq(name, "intersect?") && argc == 1 &&
          (a0 == TY_POLY_ARRAY || a0 == TY_UNKNOWN || ty_is_array(a0) || a0 == TY_POLY)) {
        buf_puts(b, "sp_PolyArray_intersect_p("); emit_expr(c, recv, b); buf_puts(b, ", ");
        if (a0 == TY_UNKNOWN) buf_puts(b, "NULL");
        else if (a0 == TY_POLY_ARRAY) emit_expr(c, argv[0], b);
        else {
          /* a differently-stored Array argument coerces; Ruby has one Array */
          buf_puts(b, "sp_poly_to_poly_array(");
          Buf ab3; memset(&ab3, 0, sizeof ab3); emit_expr(c, argv[0], &ab3);
          if (a0 == TY_POLY) buf_puts(b, ab3.p ? ab3.p : "sp_box_nil()");
          else emit_boxed_text(c, a0, ab3.p ? ab3.p : "NULL", b);
          free(ab3.p);
          buf_puts(b, ")");
        }
        buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "union") && argc == 0) {
        buf_puts(b, "sp_PolyArray_union("); emit_expr(c, recv, b); buf_puts(b, ", NULL)");
        return 1;
      }
      if (sp_streq(name, "sample") &&
          (argc == 0 || (argc == 1 && nt_type(nt, argv[0]) &&
                         sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")))) {
        /* sample or sample(random: rng): one element (#2970) */
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
        buf_printf(b, "; sp_int _t%d = ", ts); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = ", tl); emit_int_expr(c, argv[1], b);
        buf_printf(b, "; sp_int _t%d = sp_PolyArray_length(_t%d)", tn, ta);
        buf_printf(b, "; (_t%d < 0 || _t%d > _t%d || _t%d < -_t%d) ? (sp_PolyArray *)0 : sp_PolyArray_slice(_t%d, _t%d, _t%d); })",
                   tl, ts, tn, ts, tn, ta, ts, tl);
        return 1;
      }
      if (sp_streq(name, "sample") && argc == 1) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_shuffle(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative sample number\");"
                      " sp_PolyArray_slice(_t%d, 0, _t%d); })", tn, t, tn);
        return 1;
      }
      if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 1 && nt_ref(nt, id, "block") < 0) {
        int t = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_sort(", t); emit_expr(c, recv, b);
        buf_printf(b, "); SP_GC_ROOT(_t%d); sp_int _t%d = ", t, tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
        if (sp_streq(name, "max")) buf_printf(b, " sp_PolyArray_reverse_bang(_t%d);", t);
        buf_printf(b, " sp_PolyArray_slice(_t%d, 0, _t%d); })", t, tn);
        return 1;
      }
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 0 && nt_ref(nt, id, "block") < 0) {
        /* count truthy elements; a poly element may be nil/false */
        int t = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_int _t%d = 0; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
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
        /* poly_array.all?(pat)/one?/any?/none?/count(pat) -- Enumerable's
           pattern form is `pat === element` (Range cover, Regexp match, Class
           is_a, else ==); sp_poly_case_eq folds all of these and NIL (#2366,
           #2960) */
        int ta = ++g_tmp, tv = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " sp_int _t%d = 0;", tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", ti, ti, ta, ti);
        /* #count is the exception: it counts elements EQUAL to its argument,
           where the predicates match a PATTERN with === (#3817) */
        if (sp_streq(name, "count"))
          buf_printf(b, " if (sp_poly_eq(sp_PolyArray_get(_t%d, _t%d), _t%d)) _t%d++;", ta, ti, tv, tc);
        else
          buf_printf(b, " if (sp_poly_case_eq(_t%d, sp_PolyArray_get(_t%d, _t%d))) _t%d++;", tv, ta, ti, tc);
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
          buf_printf(g_pre, "sp_PolyArray *_t%d = %s; ", trecv, rb2.p ? rb2.p : ""); free(rb2.p);
          /* rooted, as the poly find/detect and find_index above already are:
             the length is the loop bound and the block allocates */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_int _t%d = 0;\n", tcnt);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
                     ti, ti, trecv, ti);
          char es_ct[64]; snprintf(es_ct, sizeof es_ct, "sp_PolyArray_get(_t%d, _t%d)", trecv, ti);
          if (!emit_iter_autosplat(c, blk, rt, es_ct, g_indent + 1) && bp) {
            emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti);
          }
          Buf vb2; memset(&vb2, 0, sizeof vb2);
          if (!emit_block_cond_next(c, blk, g_indent + 1, &vb2)) {
            for (int j = 0; j < bn2 - 1; j++) emit_stmt(c, bb2[j], g_pre, g_indent + 1);
            int saveI = g_indent; g_indent = g_indent + 1;
            /* The block value is a condition: route through emit_cond so a poly /
               nil / scalar predicate becomes a valid C truthiness test. */
            emit_cond(c, bb2[bn2 - 1], &vb2);
            g_indent = saveI;
          }
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
      if (sp_streq(name, "insert") && argc == 1) {
        int t0 = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t0); emit_expr(c, recv, b);
        buf_puts(b, "; (void)("); emit_int_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", t0);
        return 1;
      }
      if (sp_streq(name, "insert") && argc >= 2) {
        int t = ++g_tmp, ti2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", t); emit_expr(c, recv, b);
        buf_printf(b, "; SP_GC_ROOT(_t%d); sp_int _t%d = ", t, ti2); emit_int_expr(c, argv[0], b);
        buf_puts(b, ";");
        /* normalize ONCE (per-element normalization would drift as the array
           grows), keeping the too-negative IndexError the helper would raise */
        buf_printf(b, " sp_int _t%d = _t%d; if (_t%d < 0) { _t%d += (_t%d ? _t%d->len : 0) + 1;"
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
      if ((sp_streq(name, "to_a") || sp_streq(name, "entries") || sp_streq(name, "to_ary") ||
           sp_streq(name, "deconstruct")) && argc == 0) { emit_expr(c, recv, b); return 1; }
      if ((sp_streq(name, "union") || sp_streq(name, "difference") || sp_streq(name, "intersection")) &&
          argc == 0) {
        buf_puts(b, "sp_PolyArray_dup("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
        int blk = nt_ref(nt, id, "block");
        int ta = ++g_tmp, ti = ++g_tmp, tn = ++g_tmp, tnorm = ++g_tmp;
        Buf ra = expr_buf(c, recv);
        buf_printf(b, "({ sp_PolyArray *_t%d = %s;", ta, ra.p ? ra.p : "NULL"); free(ra.p);
        buf_printf(b, " sp_int _t%d = ", ti); emit_int_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " sp_int _t%d = sp_PolyArray_length(_t%d);", tn, ta);
        buf_printf(b, " sp_int _t%d = _t%d < 0 ? _t%d + _t%d : _t%d;", tnorm, ti, ti, tn, ti);
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
        for (int j = 0; j < nargs; j++) {
          tb[j] = ++g_tmp; at[j] = comp_ntype(c, argv[j]);
        }
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
          /* A scalar argument responds to no :each at all, which is CRuby's
             TypeError naming its class. Read as a container regardless, a nil
             became a column of nils, silently, and an Integer or a String
             stopped the C build. */
          if (ty_is_object(at[j])) {
            /* an object answering #to_ary zips as that Array; one answering
               #each enumerates; any other is the scalar's TypeError */
            int zdef = -1;
            TyKind zk = obj_container_conv(c, at[j], "to_ary", &zdef);
            if (zk != TY_UNKNOWN) {
              const char *kz = zk == TY_POLY_ARRAY ? "Poly" : array_kind(zk);
              buf_printf(b, " sp_%sArray *_t%d = ", kz ? kz : "Poly", tb[j]);
              emit_obj_container_conv(c, argv[j], zdef, "to_ary", b);
              /* rooted: the answer is the conversion's own allocation, read
                 across every row the loop below allocates */
              buf_printf(b, "; SP_GC_ROOT(_t%d);", tb[j]);
              at[j] = kz ? zk : TY_POLY_ARRAY;
              continue;
            }
            int zcid = ty_object_class(at[j]);
            if (zcid >= 0 && comp_method_in_chain(c, zcid, "each", NULL) < 0) {
              /* sp_zip_arg would send :each and answer NoMethodError; the
                 class is settled, so name CRuby's TypeError here */
              buf_printf(b, " sp_PolyArray *_t%d = ({ (void)(", tb[j]); emit_expr(c, argv[j], b);
              buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (must respond to :each)\"); (sp_PolyArray *)0; });",
                         class_ruby_name(c, zcid));
              at[j] = TY_POLY_ARRAY;
              continue;
            }
          }
          if (at[j] == TY_NIL || at[j] == TY_BOOL || at[j] == TY_INT ||
              at[j] == TY_FLOAT || at[j] == TY_STRING || at[j] == TY_STRBUF ||
              at[j] == TY_SYMBOL || at[j] == TY_VOID || ty_is_object(at[j]) ||
              /* a Hash or an Enumerator DOES respond to :each; the same helper
                 materializes it, where the typed line below spelled the slot
                 sp_PolyArray* and assigned an sp_SymPolyHash* to it */
              ty_is_hash(at[j]) || at[j] == TY_ENUMERATOR) {
            buf_printf(b, " sp_PolyArray *_t%d = sp_zip_arg(", tb[j]);
            emit_boxed(c, argv[j], b);
            buf_puts(b, ");");
            at[j] = TY_POLY_ARRAY;
            continue;
          }
          /* a boxed (poly) argument -- e.g. an outer block param that holds an
             array at runtime -- must be unboxed to a poly array, not assigned
             raw into an sp_PolyArray* slot (#3190). */
          if (at[j] == TY_POLY) {
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tb[j]);
            emit_expr(c, argv[j], b);
            buf_puts(b, ");");
            at[j] = TY_POLY_ARRAY;
            continue;
          }
          const char *kj = (at[j] == TY_POLY_ARRAY) ? "Poly" : (array_kind(at[j]) ? array_kind(at[j]) : "Poly");
          buf_printf(b, " sp_%sArray *_t%d = ", kj, tb[j]); emit_expr(c, argv[j], b); buf_puts(b, ";");
        }
        buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
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
        if (argc == 1) {
          buf_puts(b, "sp_PolyArray_flatten_n("); emit_expr(c, recv, b); buf_puts(b, ", ");
          /* a nil depth is legal and means "no limit" (flatten_n: < 0) */
          if (comp_ntype(c, argv[0]) == TY_NIL) { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), (sp_int)-1)"); }
          else emit_int_expr(c, argv[0], b);
          buf_puts(b, ")");
        }
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)", pti, pti, pta, pti);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ptj, ptj, ptb, ptj);
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
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
        else if (argc == 1) emit_str_expr_nilable(c, argv[0], b);   /* nil ok, false not */
        else buf_puts(b, "sp_str_empty");
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
        /* a poly-array receiver keeps materializing the tuples: an Enumerator
           here would reach chain sites that read the array directly */
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
        buf_printf(b, "; sp_int _t%d = _t%d.first < 0 ? _t%d.first + (_t%d ? _t%d->len : 0) : _t%d.first;",
                   tf, tr, tr, ta, ta, tr);
        buf_printf(b, " sp_int _t%d = (_t%d.last < 0 ? _t%d.last + (_t%d ? _t%d->len : 0) : _t%d.last) - _t%d + (_t%d.excl ? 0 : 1);",
                   tn, tr, tr, ta, ta, tr, tf, tr);
        buf_printf(b, " sp_PolyArray_slice_bang(_t%d, _t%d, _t%d < 0 ? 0 : _t%d); })", ta, tf, tn, tn);
        return 1;
      }
      if (sp_streq(name, "slice!") && argc == 1) {
        buf_puts(b, "sp_PolyArray_delete_at("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && a0 == TY_POLY_ARRAY) {
        buf_puts(b, "sp_PolyArray_replace("); emit_expr(c, recv, b); buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
      /* ...and a source of ANOTHER kind, which `[1, 2].replace(["x"])` is:
         the widening makes the receiver poly, and the source is read through
         the boxed accessors rather than needing an arm of its own (#4339). */
      if (sp_streq(name, "replace") && argc == 1 && ty_is_array(rt) &&
          (ty_is_array(a0) || a0 == TY_POLY)) {
        buf_puts(b, "sp_PolyArray_replace_from("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", tj, tj, tb, tj);
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
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "1");
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
          emit_indent(g_pre, g_indent); buf_printf(g_pre, "sp_PolyArray *_t%d = %s; ", trecv, rb.p ? rb.p : ""); free(rb.p);
          /* rooted, as the TY_POLY map!/collect! near the top of this file
             already roots its own hoist: the loop stores into the receiver on
             every turn, so an unrooted hoist is a write into freed memory */
          emit_gc_root_tmp(c, rt, trecv, g_pre); buf_puts(g_pre, "\n");
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n", ti, ti, trecv, ti);
          if (bp) { emit_indent(g_pre, g_indent + 1); buf_printf(g_pre, "lv_%s = sp_PolyArray_get(_t%d, _t%d);\n", bp, trecv, ti); }
          for (int j = 0; j < bn - 1; j++) emit_stmt(c, bb[j], g_pre, g_indent + 1);
          int sv = g_indent; g_indent++;
          /* The slot takes a boxed value: a block whose tail is statically
             typed (`[].map! { 0 }`, where the empty receiver leaves nothing to
             widen the tail against) would otherwise put a raw scalar in it. */
          Buf vb; memset(&vb, 0, sizeof vb); emit_boxed(c, bb[bn - 1], &vb); g_indent = sv;
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "sp_PolyArray_set(_t%d, _t%d, %s);\n", trecv, ti, vb.p ? vb.p : "sp_box_nil()");
          free(vb.p);
          emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
          buf_printf(b, "_t%d", trecv); return 1;
        }
      }
      /* select! / filter! / keep_if / reject! / delete_if { |x| cond }: the
         in-place filter (emit_array_filter_loop) */
      if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "keep_if") ||
           sp_streq(name, "reject!") || sp_streq(name, "delete_if")) && nt_ref(nt, id, "block") >= 0) {
        int trecv, torig, twp;
        if (emit_array_filter_loop(c, recv, nt_ref(nt, id, "block"), rt, name, g_pre, g_indent, &trecv, &torig, &twp)) {
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, tr, ti);
        /* Each pair is a boxed array whose own kind varies (IntArray for [1,2],
           StrArray for ["a","b"], PolyArray for mixed); sp_poly_arr_get boxes an
           element from any of them, so key/value extraction works regardless. */
        buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tp, tr, ti);
        /* every element must be a two-element array; a longer or shorter one
           is an ArgumentError and a non-array a TypeError, where the extra
           elements were simply dropped (#3616). Hash[...] desugars to this
           same emitter but is laxer, so it opts out. */
        if (!nt_int(nt, id, "hash_brackets", 0))
        buf_printf(b, " if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_array_kind(_t%d.cls_id))"
                      " sp_raise_cls(\"TypeError\", sp_sprintf(\"wrong element type %%s at %%lld (expected array)\","
                      " sp_poly_class_name(_t%d), (long long)_t%d));"
                      " { sp_int _n%d = sp_poly_arr_len(_t%d);"
                      " if (_n%d != 2) sp_raise_cls(\"ArgumentError\","
                      " sp_sprintf(\"wrong array length at %%lld (expected 2, was %%lld)\","
                      " (long long)_t%d, (long long)_n%d)); }",
                   tp, tp, tp, ti, tp, tp, tp, ti, tp);
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
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {\n",
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
  buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
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

/* An empty hash literal: no variant to infer, nothing to fold into a merge. */
static int hash_lit_empty(const NodeTable *nt, int n) {
  const char *ty = nt_type(nt, n);
  if (!ty || !(sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode"))) return 0;
  int en = 0; nt_arr(nt, n, "elements", &en);
  return en == 0;
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
         expression position (the statement form lives in emit_iteration_stmt;
         the loop is emit_hash_filter_loop's). Mutates in place; `!` forms
         yield nil when nothing was removed else self, keep_if/delete_if
         always yield self. */
      if ((sp_streq(name, "delete_if") || sp_streq(name, "reject!") || sp_streq(name, "select!") ||
           sp_streq(name, "filter!") || sp_streq(name, "keep_if")) &&
          nt_ref(nt, id, "block") >= 0) {
        int block = nt_ref(nt, id, "block");
        Buf rb = expr_buf(c, recv);
        int tr, torig, twp;
        int ok = emit_hash_filter_loop(c, recv, block, rt, name, rb.p ? rb.p : "NULL", g_pre, g_indent, &tr, &torig, &twp);
        free(rb.p);
        if (ok) {
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
           directly. Scalar-keyed variants read the sp_int slot. */
        const char *keyexpr = (kt == TY_SYMBOL) ? "(sp_sym)args[0]"
                            : (kt == TY_STRING) ? "(const char *)(uintptr_t)args[0]"
                            : (rt == TY_POLY_POLY_HASH) ? "_sp_proc_poly_args[0]"
                            : "args[0]";
        if (rt == TY_POLY_POLY_HASH) g_needs_proc_poly_argslot = 1;
        buf_printf(&g_proc_protos, "static sp_int _hashproc_%d(void *cap, sp_int argc, sp_int *args);\n", pn);
        buf_printf(&g_procs, "static sp_int _hashproc_%d(void *cap, sp_int argc, sp_int *args) {\n", pn);
        /* the hash proc is a lambda: exactly one key, as CRuby's raises --
           the old `argc < 1 -> return 0` left the return slot holding the
           previous call's value */
        buf_printf(&g_procs, "  if (argc != 1) sp_raise_cls(\"ArgumentError\","
                   " sp_sprintf(\"wrong number of arguments (given %%lld, expected 1)\", (long long)argc));\n");
        buf_printf(&g_procs, "  sp_%sHash *_h = (sp_%sHash *)cap;\n", hn, hn);
        /* Universal return ABI: publish the boxed value into _sp_proc_poly_ret
           for every value type; the .call site reads the slot back. */
        buf_puts(&g_procs, "  _sp_proc_poly_ret = ");
        { char _ge[256];
          snprintf(_ge, sizeof _ge, "sp_%sHash_get(_h, %s)", hn, keyexpr);
          emit_boxed_text(c, vt, _ge, &g_procs); }
        buf_puts(&g_procs, ";\n  return 0;\n}\n");
        buf_printf(b, "sp_proc_new_meta((void *)_hashproc_%d, (void *)(", pn);
        emit_expr(c, recv, b);
        /* CRuby's Hash#to_proc is a lambda: lambda? answers true and a
           composed call enforces its 1-arity instead of reading a stale slot */
        buf_puts(b, "), sp_hashproc_cap_scan, 1, TRUE, 1, NULL, NULL)");
        return 1;
      }
      if ((sp_streq(name, "dup") || sp_streq(name, "clone")) && argc == 0) {
        if (sp_streq(name, "clone")) {
          /* clone carries the frozen flag over, dup does not (#3751) */
          int ts = ++g_tmp, td = ++g_tmp;
          buf_printf(b, "({ sp_%sHash *_t%d = ", hn, ts); emit_expr(c, recv, b);
          buf_printf(b, "; sp_%sHash *_t%d = sp_%sHash_dup(_t%d);"
                        " if (_t%d && sp_gc_is_frozen(_t%d)) sp_gc_freeze(_t%d);"
                        " _t%d; })",
                     hn, td, hn, ts, ts, ts, td, td);
          return 1;
        }
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
          buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");  /* the key still evaluates */
          if (vt == TY_INT) buf_printf(b, "_t%d ? _t%d->default_v : SP_INT_NIL; })", t, t);
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
          /* int-valued hashes have a nullable get_opt; string-valued use get */
          const char *getter = ty_hash_val(rt) == TY_INT ? "get_opt" : "get";
          buf_printf(b, "sp_%sHash_%s(", hn, getter);
          emit_expr(c, recv, b); buf_puts(b, ", "); emit_hash_key(c, argv[0], ty_hash_key(rt), b); buf_puts(b, ")");
        }
        return 1;
      }
      if (sp_streq(name, "dig") && argc >= 1) {
        /* dig(*keys): the key list only exists at run time, so walk it there.
           Emitting the splat as a single key read the key array through the
           key's own type and the C did not compile. */
        if (nt_kind(nt, argv[0]) == NK_SplatNode) {
          buf_puts(b, "sp_poly_dig_list("); emit_boxed(c, recv, b);
          buf_puts(b, ", sp_poly_to_poly_array(");
          emit_boxed(c, argv[0], b);
          buf_puts(b, "))");
          return 1;
        }
        TyKind vt = ty_hash_val(rt);
        TyKind kt = ty_hash_key(rt);
        /* Static key-type mismatch (string key on sym hash, etc.) -> nil. */
        TyKind arg0t = comp_ntype(c, argv[0]);
        if ((kt == TY_SYMBOL && arg0t == TY_STRING) ||
            (kt == TY_STRING && arg0t == TY_SYMBOL)) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
          if (vt == TY_INT) buf_puts(b, "); SP_INT_NIL; })");
          else if (vt == TY_STRING) buf_puts(b, "); NULL; })");
          else buf_puts(b, "); sp_box_nil(); })");
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
            buf_printf(b, "({ sp_int _t%d = sp_%sHash_%s(_t%d, ", tk0, hn, getter, th);
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
            /* Ruby's dig stops at nil and raises on anything else that has no
               #dig; only the first receiver is known to be a container, so
               every later step has to check what it landed on (#3567). */
            buf_printf(b, " sp_poly_dig_check(_t%d);", tr);
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
              /* A poly sub-key is stored as sp_RbVal, not sp_int; dispatch on
                 both the runtime receiver and key kind. */
              buf_printf(b, " sp_RbVal _t%d = ", tk);
              emit_expr(c, argv[di], b);
              buf_printf(b, "; _t%d = sp_poly_index_poly(_t%d, _t%d);", tr, tr, tk);
            }
            else {
              buf_printf(b, " sp_int _t%d = ", tk);
              emit_int_expr(c, argv[di], b);
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
          int is_splat = nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode");
          int ts = 0, ti = 0;
          if (is_splat) {
            /* values_at(*keys): each element of the splatted array is a
               separate key (#3277). */
            ts = ++g_tmp; ti = ++g_tmp;
            buf_printf(b, " { sp_PolyArray *_t%d = ", ts); emit_expr(c, argv[a], b);
            buf_printf(b, "; for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {", ti, ti, ts, ti);
            buf_printf(b, " %s _t%d = ", c_type_name(kt), tk);
            if (kt == TY_STRING) buf_printf(b, "sp_poly_to_s(sp_PolyArray_get(_t%d, _t%d));", ts, ti);
            else buf_printf(b, "(%s)sp_poly_to_i(sp_PolyArray_get(_t%d, _t%d));", c_type_name(kt), ts, ti);
          }
          else if (hash_key_misses(c, argv[a], kt)) {
            /* a key of a kind the table cannot hold: values_at answers nil,
               fetch_values raises naming the key, boxed once here */
            int fv_blk = nt_ref(nt, id, "block");
            if (is_fetch && fv_blk >= 0) {
              unsupported_feature(c, id, "Hash#fetch_values with a block and a key of another class than the hash's keys");
              return 0;
            }
            buf_printf(b, " sp_RbVal _t%d = ", tk); emit_boxed(c, argv[a], b); buf_puts(b, ";");
            if (is_fetch) {
              char htmp[32]; snprintf(htmp, sizeof htmp, "_t%d", th);
              buf_puts(b, " sp_exc_stage_recv(");
              emit_boxed_text(c, rt, htmp, b);
              buf_printf(b, "); sp_raise_key_not_found(_t%d);", tk);
            }
            else buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_nil());", tr);
            continue;
          }
          else {
            buf_printf(b, " %s _t%d = ", c_type_name(kt), tk); emit_hash_key(c, argv[a], kt, b); buf_puts(b, ";");
          }
          /* A boxed-value hash answers its default on a miss, and values_at
             wants that default; only a typed-value hash needs the has_key
             guard, whose zero would otherwise read as a real value. */
          int use_default = !is_fetch && vt == TY_POLY;
          if (!use_default) buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d))", hn, th, tk);
          buf_printf(b, " sp_PolyArray_push(_t%d, ", tr);
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
          else if (!use_default) buf_printf(b, " else sp_PolyArray_push(_t%d, sp_box_nil());", tr);
          if (is_splat) buf_puts(b, " } }");
        }
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* A block supersedes a positional default: CRuby warns and calls the
           block, where the default was being returned (#3566). Treating the
           two-argument-with-block form as the one-argument-with-block form is
           exactly that rule. */
      if (sp_streq(name, "fetch") && (argc == 1 || (argc == 2 && nt_ref(nt, id, "block") >= 0))) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && hash_key_misses(c, argv[0], ty_hash_key(rt))) {
          /* the block receives the missing key, and its parameter is typed
             as the table's key kind; a key of another kind has no slot */
          unsupported_feature(c, id, "Hash#fetch with a block and a key of another class than the hash's keys");
          return 0;
        }
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
          if (fp0) {
            /* the block is spliced inline, so the parameter's slot is the
               enclosing scope's local; a body that reassigns it widened the
               slot to poly, and the key boxes on the way in */
            Scope *fbs = comp_scope_of(c, blk);
            LocalVar *flv = fbs ? scope_local(fbs, fp0) : NULL;
            if (!flv) { Scope *fes = comp_scope_of(c, id); flv = fes ? scope_local(fes, fp0) : NULL; }
            TyKind kt = ty_hash_key(rt);
            if (flv && flv->type == TY_POLY && kt != TY_POLY) {
              char ktn[32]; snprintf(ktn, sizeof ktn, "_t%d", tk);
              buf_printf(b, "lv_%s = ", rename_local(fp0)); emit_boxed_text(c, kt, ktn, b); buf_puts(b, "; ");
            }
            else buf_printf(b, "lv_%s = _t%d; ", rename_local(fp0), tk);
          }
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
        char keytmp[32], htmp[32];
        snprintf(keytmp, sizeof keytmp, "_t%d", tk);
        snprintf(htmp, sizeof htmp, "_t%d", th);
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        if (hash_key_misses(c, argv[0], ty_hash_key(rt))) {
          /* a key of a kind the table cannot hold: the KeyError names the
             key itself, so box it once rather than look it up */
          buf_printf(b, "; sp_RbVal _t%d = ", tk); emit_boxed(c, argv[0], b);
          buf_puts(b, "; sp_exc_stage_recv(");
          emit_boxed_text(c, rt, htmp, b);
          buf_printf(b, "); sp_raise_key_not_found(_t%d); %s; })", tk,
                     vt == TY_POLY ? "sp_box_nil()" : default_value(vt));
          return 1;
        }
        buf_printf(b, "; %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
        buf_printf(b, "; sp_%sHash_has_key(_t%d, _t%d) ? sp_%sHash_get(_t%d, _t%d) : (",
                   hn, th, tk, hn, th, tk);
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
      /* an Array init concatenates the pairs onto it, flat (#3571) */
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          (ty_is_array(comp_ntype(c, argv[0])) ||
           (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")))) {
        buf_puts(b, "sp_poly_hash_sum_arr("); emit_boxed(c, recv, b); buf_puts(b, ", ");
        if (ty_is_array(comp_ntype(c, argv[0]))) {
          if (comp_ntype(c, argv[0]) == TY_POLY_ARRAY) emit_expr(c, argv[0], b);
          else { buf_puts(b, "sp_poly_to_poly_array("); emit_boxed(c, argv[0], b); buf_puts(b, ")"); }
        }
        else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), sp_PolyArray_new())"); }
        buf_puts(b, ")");
        return 1;
      }
      /* A seed of any other class folds the pairs into IT: `nil + [k, v]` is
         nil's own missing `+` (NoMethodError), not the Integer coercion the
         int arm below reports -- and an empty hash adds nothing and answers
         the seed itself, so `{}.sum(nil)` is nil. */
      if (sp_streq(name, "sum") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          !fold_seed_typed(fold_seed_ntype(c, argv[0]), TY_INT)) {
        emit_poly_sum_seed(c, recv, argv[0], b);
        return 1;
      }
      if (sp_streq(name, "sum") && argc <= 1 && nt_ref(nt, id, "block") < 0) {
        /* Hash#sum without a block folds each [k,v] PAIR into the init value;
           `init + [k,v]` is Integer#+ Array -> TypeError, so only an empty hash
           (which returns the init unchanged) is well-defined. */
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t); emit_expr(c, recv, b);
        buf_printf(b, "; sp_%sHash_length(_t%d) == 0 ? (sp_int)(", hn, t);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "0");
        buf_puts(b, ") : (sp_raise_cls(\"TypeError\", \"Array can't be coerced into Integer\"), (sp_int)0); })");
        return 1;
      }
      if (sp_streq(name, "clear") && argc == 0) {
        int t = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), t);
        emit_expr(c, recv, b);
        buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", t, t, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " sp_%sHash_clear(_t%d); _t%d; })", hn, t, t);
        return 1;
      }
      /* a key changed since it was stored is under the hash it was stored
         with; the general hash keeps each key's hash, so rehash asks every
         key again */
      if (sp_streq(name, "rehash") && argc == 0 && rt == TY_POLY_POLY_HASH) {
        buf_puts(b, "sp_PolyPolyHash_rehash("); emit_expr(c, recv, b); buf_puts(b, ")");
        return 1;
      }
      /* no-arg merge -> a copy; no-arg slice -> an empty hash of the same
         variant; to_hash / rehash -> a copy of self (#2340/#2349) */
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
        if (hash_key_misses(c, argv[0], hash_kt)) {
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
      if ((sp_streq(name, "value?") || sp_streq(name, "has_value?")) && argc == 1) {
        int poly = (rt == TY_SYM_POLY_HASH || rt == TY_STR_POLY_HASH ||
                    rt == TY_POLY_POLY_HASH);  /* boxed-value variants (#2373) */
        if (!poly && value_obj_compares(c, argv[0])) {
          unsupported_feature(c, id, "Hash#value? of a user object defining == in a typed Hash");
          return 0;
        }
        if (!poly && value_kind_misses(c, argv[0], ty_hash_val(rt))) {
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
          return 1;
        }
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, tp, ti);
        buf_printf(b, " sp_PolyArray *_pr = (sp_PolyArray *)_t%d->data[_t%d].v.p;", tp, ti);
        buf_printf(b, " if (sp_poly_eq(_pr->data[1], _t%d)) { _t%d = _pr->data[0]; break; } }", tv, tr);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "replace") && argc == 1 && comp_ntype(c, argv[0]) == rt) {
        int trp = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), trp); emit_expr(c, recv, b);
        buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", trp, trp, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " sp_%sHash_replace(_t%d, ", hn, trp); emit_expr(c, argv[0], b);
        buf_printf(b, "); _t%d; })", trp);
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
        buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", th, th, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " SP_GC_ROOT(_t%d); sp_RbVal _t%d = ", th, to); emit_boxed(c, argv[0], b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_PolyPolyHash_clear(_t%d);", to, th);
        buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", tn, to);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
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
      /* merge!(*hashes): the sources are an array whose elements are only
         known at run time, so walk it and merge each element in (#3848). */
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc == 1 &&
          nt_ref(nt, id, "block") < 0 && nt_kind(nt, argv[0]) == NK_SplatNode) {
        int inner = nt_ref(nt, argv[0], "expression");
        if (inner < 0) return 0;
        TyKind it = comp_ntype(c, inner);
        if (!ty_is_array(it) && it != TY_POLY_ARRAY && it != TY_POLY) return 0;
        int tr = ++g_tmp, ts = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));
        buf_printf(b, " sp_RbVal _t%d = ", ts); emit_boxed(c, inner, b);
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", ts);
        buf_printf(b, " sp_int _t%d = sp_poly_arr_len_ex(_t%d);", ti, ts);
        char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", tr);
        buf_printf(b, " for (sp_int _i8 = 0; _i8 < _t%d; _i8++)"
                      " sp_poly_hash_merge_into(", ti);
        emit_boxed_text(c, rt, rtxt, b);
        buf_printf(b, ", sp_poly_each_elem(_t%d, _i8));", ts);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc >= 1 &&
          nt_ref(nt, id, "block") < 0 && rt == TY_POLY_POLY_HASH) {
        /* An argument that is a Hash at run time only is checked there, as
           CRuby's implicit conversion would, so a boxed hash merges into
           another; an empty literal has no variant to infer and nothing to
           fold in. */
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = comp_ntype(c, argv[ai]);
          if (!ty_is_hash(at) && at != TY_POLY && !hash_lit_empty(nt, argv[ai])) return 0;
        }
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = ", tr); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
        for (int ai = 0; ai < argc; ai++) {
          if (hash_lit_empty(nt, argv[ai])) continue;
          int to = ++g_tmp, ti = ++g_tmp, tp = ++g_tmp;
          buf_printf(b, " sp_RbVal _t%d = ", to); emit_boxed(c, argv[ai], b);
          buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", to);
          if (comp_ntype(c, argv[ai]) == TY_POLY)
            buf_printf(b, " if (_t%d.tag != SP_TAG_OBJ || !sp_poly_is_hash_kind(_t%d.cls_id))"
                          " sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into Hash\", sp_convert_src_name(_t%d)));",
                       to, to, to);
          buf_printf(b, " sp_int _t%d = sp_poly_arr_len_ex(_t%d);"
                        " for (sp_int _i9 = 0; _i9 < _t%d; _i9++) {"
                        " sp_RbVal _t%d = sp_poly_each_elem(_t%d, _i9);"
                        " sp_PolyPolyHash_set(_t%d,"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 0),"
                        " sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, 1)); }",
                     ti, to, ti, tp, to, tr, tp, tp);
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
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
        for (int ai = 0; ai < argc; ai++) {
          int to = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
          buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[ai], b); buf_puts(b, ";");
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
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
        buf_printf(b, " if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", tr, tr, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
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
          buf_printf(b, "); }\nelse { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }", hn, tr, tk, hn, to, tk);
        }
        else {
          buf_printf(b, " sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d));", hn, tr, tk, hn, to, tk);
        }
        buf_printf(b, " } _t%d; })", tr);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
          rt == TY_POLY_POLY_HASH) {
        /* merge(other) { |k, o, n| } on a PolyPolyHash (e.g. a reduce({})
           accumulator). PolyPolyHash is open-addressing: order[i] is a table
           slot, so the key is keys[order[i]] -- the generic order[i]-as-key
           path below miscompiles it. `other` can be any boxed hash (an element
           read out of a poly Array loses its concrete variant), so iterate it
           through sp_poly_hash_pair. Block params are declared locally here so
           a merge nested inside a fold (where they are not scope locals) still
           resolves them (#3100). */
        int blk = nt_ref(nt, id, "block");
        const char *bp0 = block_param_name(c, blk, 0);
        const char *bp1 = block_param_name(c, blk, 1);
        const char *bp2 = block_param_name(c, blk, 2);
        int tr = ++g_tmp, tc = ++g_tmp, tj = ++g_tmp, to = ++g_tmp,
            tn = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = sp_PolyPolyHash_new(); SP_GC_ROOT(_t%d);", tr, tr);
        buf_printf(b, " sp_PolyPolyHash *_t%d = ", tc); emit_expr(c, recv, b); buf_puts(b, ";");
        buf_printf(b, " _t%d->default_v = _t%d->default_v; _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, tc, tr, tc, tr, tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {"
                      " sp_int _ix = _t%d->order[_t%d];"
                      " sp_PolyPolyHash_set(_t%d, _t%d->keys[_ix], _t%d->vals[_ix]); }",
                   tj, tj, tc, tj, tc, tj, tr, tc, tc);
        buf_printf(b, " sp_RbVal _t%d = ", to); emit_boxed(c, argv[0], b); buf_puts(b, ";");
        if (bp0) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp0));
        if (bp1) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp1));
        if (bp2) buf_printf(b, " sp_RbVal lv_%s;", rename_local(bp2));
        buf_printf(b, " sp_int _t%d = sp_poly_length(_t%d);", tn, to);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {", ti, ti, tn, ti);
        buf_printf(b, " sp_RbVal _t%d, _t%d; sp_poly_hash_pair(_t%d, _t%d, &_t%d, &_t%d);",
                   tk, tv, to, ti, tk, tv);
        buf_printf(b, " if (sp_PolyPolyHash_has_key(_t%d, _t%d)) {", tr, tk);
        if (bp0) buf_printf(b, " lv_%s = _t%d;", rename_local(bp0), tk);
        if (bp1) buf_printf(b, " lv_%s = sp_PolyPolyHash_get(_t%d, _t%d);", rename_local(bp1), tr, tk);
        if (bp2) buf_printf(b, " lv_%s = _t%d;", rename_local(bp2), tv);
        buf_printf(b, " sp_PolyPolyHash_set(_t%d, _t%d, ({ ", tr, tk);
        {
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          int bval = bn > 0 ? bb[bn - 1] : -1;
          /* redirect g_pre so an allocating value expression (e.g. a `[o, n]`
             array literal) drains its setup INSIDE this stmt-expr -- after the
             block params were assigned above -- rather than being hoisted
             before the merge loop, which would read the params' initial nil
             (#3100 follow-up). */
          Buf lpre; memset(&lpre, 0, sizeof lpre);
          Buf lval; memset(&lval, 0, sizeof lval);
          Buf *saved_pre = g_pre; g_pre = &lpre;
          for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], &lval, 0);
          if (bval >= 0) {
            if (comp_ntype(c, bval) != TY_POLY) emit_boxed(c, bval, &lval);
            else emit_expr(c, bval, &lval);
          }
          else buf_puts(&lval, "sp_box_nil()");
          g_pre = saved_pre;
          if (lpre.p) buf_puts(b, lpre.p);
          if (lval.p) buf_puts(b, lval.p);
          free(lpre.p); free(lval.p);
        }
        buf_puts(b, "; })");
        buf_printf(b, "); }\nelse { sp_PolyPolyHash_set(_t%d, _t%d, _t%d); } }", tr, tk, tv);
        buf_printf(b, " _t%d; })", tr);
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
        buf_printf(b, " _t%d->default_v = _t%d->default_v;", tr, tc);
        if (vt == TY_POLY)
          buf_printf(b, " _t%d->dproc = _t%d->dproc; _t%d->dproc_self = _t%d->dproc_self;", tr, tc, tr, tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)"
                      " sp_%sHash_set(_t%d, _t%d->order[_t%d], sp_%sHash_get(_t%d, _t%d->order[_t%d]));",
                   tj, tj, tc, tj, hn, tr, tc, tj, hn, tc, tc, tj);
        buf_printf(b, " %s _t%d = ", c_type_name(rt), to); emit_expr(c, argv[0], b); buf_puts(b, ";");
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, to, ti);
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
        buf_printf(b, "); }\nelse { sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); } }", hn, tr, tk, hn, to, tk);
        buf_printf(b, " _t%d; })", tr);
        return 1;
      }
      /* merge(*hashes): fold each member of the splatted list in, through the
         universal boxed merge (#3561) */
      if (sp_streq(name, "merge") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
          nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "SplatNode")) {
        int sx = nt_ref(nt, argv[0], "expression");
        int tacc = ++g_tmp, tls = ++g_tmp, tmi = ++g_tmp;
        buf_printf(b, "({ sp_PolyPolyHash *_t%d = sp_poly_hash_merge(", tacc);
        emit_boxed(c, recv, b);
        buf_printf(b, ", sp_box_nil()); SP_GC_ROOT(_t%d);", tacc);
        buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tls);
        if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
        buf_printf(b, "); SP_GC_ROOT(_t%d);", tls);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " _t%d = sp_poly_hash_merge(sp_box_nullable_obj((void *)_t%d, SP_BUILTIN_POLY_POLY_HASH),"
                      " sp_PolyArray_get(_t%d, _t%d));",
                   tmi, tmi, tls, tmi, tacc, tacc, tls, tmi);
        buf_printf(b, " _t%d; })", tacc);
        return 1;
      }
      if (sp_streq(name, "merge") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_POLY_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_STR_HASH || rt == TY_POLY_POLY_HASH || rt == TY_INT_INT_HASH ||
           rt == TY_INT_STR_HASH)) {
        TyKind at = comp_ntype(c, argv[0]);
        /* an empty Hash literal settles at its own default variant, which is
           rarely the receiver's; merging it passed one hash struct as another
           (#3597). It contributes nothing, so the merge is a copy. */
        { const char *aty0 = nt_type(nt, argv[0]); int aen = 0;
          if (aty0 && sp_streq(aty0, "HashNode") &&
              (nt_arr(nt, argv[0], "elements", &aen), aen == 0)) {
            buf_printf(b, "sp_%sHash_dup(", hn); emit_expr(c, recv, b); buf_puts(b, ")");
            return 1;
          } }
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
        /* any other cross-variant merge (mismatched key or value layout):
           fold both sides through the universal boxed merge -- passing the
           argument raw into the receiver-layout helper read it through the
           wrong struct (#3261). Matches the TY_POLY_POLY_HASH inference. */
        if (ty_is_hash(at) && at != rt &&
            !(rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH))) {
          buf_puts(b, "sp_poly_hash_merge("); emit_boxed(c, recv, b);
          buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        /* a BOXED argument holds whichever variant the value really is: casting
           it to the receiver's layout read a Sym-keyed hash through a Str-keyed
           struct, and the Symbol key was then dereferenced as a char * (#3975).
           Fold through the universal boxed merge instead, as a cross-variant
           merge already does. */
        if (at == TY_POLY) {
          buf_puts(b, "sp_poly_hash_merge("); emit_boxed(c, recv, b);
          buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
          return 1;
        }
        if (!ty_is_hash(at) && at != TY_POLY && at != TY_UNKNOWN) {
          /* an object answering #to_hash of the receiver's own layout merges
             as that Hash; any other class is CRuby's TypeError */
          int mdef = -1;
          TyKind mk = obj_container_conv(c, at, "to_hash", &mdef);
          if (mk == rt) {
            buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
            emit_obj_container_conv(c, argv[0], mdef, "to_hash", b); buf_puts(b, ")");
            return 1;
          }
          if (mk != TY_UNKNOWN) {
            /* the analysis typed this call as the receiver's layout; a
               #to_hash of another layout would widen it, which is an
               inference question, so say so rather than miscompile */
            unsupported_feature(c, id, "Hash#merge with a #to_hash of another layout than the receiver");
            return 0;
          }
          const char *mcn = conv_cls_name_of(c, at);
          if (mcn) {
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)("); emit_expr(c, argv[0], b);
            buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into Hash\"); (%s)0; })", mcn, c_type_name(rt));
            return 1;
          }
        }
        buf_printf(b, "sp_%sHash_merge(", hn); emit_expr(c, recv, b); buf_puts(b, ", ");
        /* a str_poly receiver may be merged with a concrete str-keyed hash;
           coerce the argument to the receiver's variant first */
        if (rt == TY_STR_POLY_HASH && (at == TY_STR_STR_HASH || at == TY_STR_INT_HASH)) {
          buf_printf(b, "sp_StrPolyHash_from_%s(", at == TY_STR_STR_HASH ? "str_str_hash" : "str_int_hash");
          emit_expr(c, argv[0], b); buf_puts(b, ")");
        }
        else if (at == TY_POLY && rt == TY_POLY_POLY_HASH) {
          /* A boxed argument holds whichever variant the value really is, so
             the pointer cast below would read a Sym-keyed hash through a
             Poly-keyed struct. The general hash can be rebuilt from any of
             them: merge it into an empty one. */
          buf_puts(b, "sp_poly_hash_merge("); emit_expr(c, argv[0], b); buf_puts(b, ", sp_box_nil())");
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
          /* A splatted key list contributes each of its members, not one key.
             `except` has had this arm since #3561; `slice` never did, so
             `h.slice(*ATTRS)` handed the whole array to the key coercion and
             kept whatever that answered -- no keys for a Symbol-keyed hash,
             the first one for a local (#4164). */
          if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
            int sx = nt_ref(nt, argv[i], "expression");
            int tsa = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsa);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);", tsa);
            buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
                       tsi, tsi, tsa, tsi);
            { char el[64]; snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, _t%d)", tsa, tsi);
              int tsk = ++g_tmp;
              if (rt == TY_POLY_POLY_HASH) buf_printf(b, " sp_RbVal _t%d = %s;", tsk, el);
              else if (skt == TY_SYMBOL) buf_printf(b, " sp_sym _t%d = (sp_sym)sp_poly_to_i(%s);", tsk, el);
              else if (skt == TY_INT) buf_printf(b, " sp_int _t%d = sp_poly_to_i(%s);", tsk, el);
              else buf_printf(b, " const char *_t%d = sp_poly_to_s(%s);", tsk, el);
              buf_printf(b, " if (sp_%sHash_has_key(_t%d, _t%d)) sp_%sHash_set(_t%d, _t%d, sp_%sHash_get(_t%d, _t%d)); }",
                         hn, th, tsk, hn, tr, tsk, hn, th, tsk);
            }
            continue;
          }
          int tk = ++g_tmp;
          if (rt == TY_POLY_POLY_HASH) {
            buf_printf(b, " { sp_RbVal _t%d = ", tk); emit_boxed(c, argv[i], b);
          }
          else if (skt == TY_SYMBOL) {
            buf_printf(b, " { sp_sym _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
          }
          else if (skt == TY_INT) {
            buf_printf(b, " { sp_int _t%d = ", tk); emit_hash_key(c, argv[i], skt, b);
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
        /* except answers a fresh hash: no default, no default proc */
        {
          TyKind evt = ty_hash_val(rt);
          if (evt == TY_POLY)
            buf_printf(b, " _t%d->default_v = sp_box_nil(); _t%d->dproc = NULL; _t%d->dproc_self = NULL;", t, t, t);
          else if (evt == TY_STRING) buf_printf(b, " _t%d->default_v = NULL;", t);
          else buf_printf(b, " _t%d->default_v = SP_INT_NIL;", t);
        }
        for (int i = 0; i < argc; i++) {
          /* a splatted key list deletes each of its members (#3561) */
          if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
            int sx = nt_ref(nt, argv[i], "expression");
            int tsa = ++g_tmp, tsi = ++g_tmp;
            buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsa);
            if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
            buf_printf(b, "); SP_GC_ROOT(_t%d);", tsa);
            buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                          " sp_%sHash_delete(_t%d, ", tsi, tsi, tsa, tsi, hn, t);
            {
              char el[64]; snprintf(el, sizeof el, "sp_PolyArray_get(_t%d, _t%d)", tsa, tsi);
              TyKind kt2 = ty_hash_key(rt);
              if (rt == TY_POLY_POLY_HASH) buf_puts(b, el);
              else if (kt2 == TY_SYMBOL) buf_printf(b, "(sp_sym)sp_poly_to_i(%s)", el);
              else if (kt2 == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", el);
              else buf_printf(b, "sp_poly_to_i(%s)", el);
            }
            buf_puts(b, ");");
            continue;
          }
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
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
         destructured-pair form) compares by ==, served by sp_poly_eq; a CLASS
         pattern is a kind-of test, and comparing the pair to the class value
         by equality answered false for every pair (#3565). */
      if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
           sp_streq(name, "none?") || sp_streq(name, "one?")) &&
          argc == 1 && nt_ref(nt, id, "block") < 0) {
        int tp = ++g_tmp, tpat = ++g_tmp, tc = ++g_tmp, ti = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; sp_RbVal _t%d = ", tpat); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = 0;", tc);
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++)", ti, ti, tp, ti);
        if (comp_ntype(c, argv[0]) == TY_CLASS)
          buf_printf(b, " if (sp_poly_is_a(_t%d->data[_t%d], (sp_Class){(sp_int)_t%d.v.i, NULL})) _t%d++;", tp, ti, tpat, tc);
        else
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
          buf_printf(b, " sp_int _t%d = _t%d->order[0];", tk, th);
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
        buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"attempt to take negative size\"); sp_PolyArray_slice(", tn);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, ", 0, _t%d); })", tn);
        return 1;
      }
      if (sp_streq(name, "drop") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        int tp = ++g_tmp, tn = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = ", tp);
        emit_hash_pairs_expr(c, recv, rt, hn, b);
        buf_printf(b, "; sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
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
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {", ti, ti, th, ti);
        if (!is_rassoc) {
          /* assoc: compare key */
          if (rt == TY_POLY_POLY_HASH)
            buf_printf(b, " if (sp_rbval_eql_key(_t%d->keys[_t%d->order[_t%d]], _t%d)) {", th, th, ti, ta);
          else if (kt == TY_STRING)
            /* sp_str_eq, not strcmp: a key of a class the table cannot hold
               reaches here as the NULL miss sentinel emit_hash_key answers */
            buf_printf(b, " if (sp_str_eq(_t%d->order[_t%d], _t%d)) {", th, ti, ta);
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
      if (sp_streq(name, "delete") && argc == 1 &&
          (rt == TY_STR_INT_HASH || rt == TY_STR_STR_HASH || rt == TY_SYM_POLY_HASH ||
           rt == TY_STR_POLY_HASH || rt == TY_POLY_POLY_HASH ||
           rt == TY_INT_INT_HASH || rt == TY_INT_STR_HASH)) {
        /* returns the deleted value (or nil on a miss), then removes the key */
        TyKind vt = ty_hash_val(rt);
        int th = ++g_tmp, tk = ++g_tmp, tv = ++g_tmp;
        if (nt_ref(nt, id, "block") >= 0 && hash_key_misses(c, argv[0], ty_hash_key(rt))) {
          /* the block receives the missing key, typed as the table's key kind */
          unsupported_feature(c, id, "Hash#delete with a block and a key of another class than the hash's keys");
          return 0;
        }
        buf_printf(b, "({ %s _t%d = ", c_type_name(rt), th); emit_expr(c, recv, b);
        buf_printf(b, "; if (sp_gc_is_frozen(_t%d)) sp_raise_frozen_hash_at(_t%d, %s);", th, th, hash_box_cls(rt));   /* (#3001) */
        buf_printf(b, " %s _t%d = ", c_type_name(ty_hash_key(rt)), tk); emit_hash_key(c, argv[0], ty_hash_key(rt), b);
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
          buf_printf(b, "; sp_%sHash_delete(_t%d, _t%d); }\nelse {", hn, th, tk);
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

/* Does this subtree read the regexp match globals ($~, $1..$9, $&, $`, $')?
   A scan block that does needs the match registers refreshed per iteration,
   which the pre-computed rows alone do not do (#3601). */
static int subtree_reads_match_globals(Compiler *c, int root) {
  if (root < 0) return 0;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, root);
  if (ty) {
    if (sp_streq(ty, "BackReferenceReadNode") || sp_streq(ty, "NumberedReferenceReadNode"))
      return 1;
    if (sp_streq(ty, "GlobalVariableReadNode")) {
      const char *gn = nt_str(nt, root, "name");
      if (gn && (sp_streq(gn, "$~") || sp_streq(gn, "$&") || sp_streq(gn, "$`") ||
                 sp_streq(gn, "$'") || sp_streq(gn, "$+")))
        return 1;
    }
  }
  int nr = nt_num_refs(nt, root);
  for (int i = 0; i < nr; i++) if (subtree_reads_match_globals(c, nt_ref_at(nt, root, i))) return 1;
  int na = nt_num_arrs(nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, root, i, &n);
    for (int j = 0; j < n; j++) if (subtree_reads_match_globals(c, el[j])) return 1;
  }
  return 0;
}

/* String methods that only read the receiver's bytes: they answer a scalar or
   build a new string, and never retain the pointer they were handed. A
   shared-mutable receiver can hand them its live buffer instead of a copy. */
static int str_recv_reads_only(const char *name) {
  static const char *const ro[] = {
    "[]", "slice", "byteslice", "getbyte", "ord", "chr",
    "index", "rindex", "include?", "start_with?", "end_with?",
    "count", "length", "size", "bytesize", "empty?",
    "to_i", "to_f", "hex", "oct", "match?", "casecmp", "casecmp?",
    "upcase", "downcase", "capitalize", "swapcase", "reverse",
    "strip", "lstrip", "rstrip", "chomp", "chop", "center", "ljust", "rjust",
    "each_char", "each_byte", "each_line", "chars", "bytes", "lines", "split",
    "sum", "hash", "unpack", "unpack1", "codepoints", "scan", NULL };
  for (int i = 0; ro[i]; i++) if (sp_streq(name, ro[i])) return 1;
  return 0;
}

/* A byte-offset search takes a String needle. A poly one is a String at run
   time, or the conversion protocol's TypeError -- either way emit_str_expr
   makes it a `const char *` -- so it belongs on the same arm as a static
   String rather than falling through to "no such method" (#4004's family). */
static int str_needle_p(Compiler *c, int a) {
  TyKind t = comp_ntype(c, a);
  return t == TY_STRING || t == TY_STRBUF || t == TY_POLY;
}

/* The names CRuby's nil answers: NilClass's own methods plus the Object /
   Kernel surface every object carries. Everything else on a nil receiver is a
   NoMethodError, which is what makes this a list of exceptions rather than a
   list of rules -- a name missing from here raises, the safe direction. */
/* Can this expression hand back the nil sentinel? call_returns_nullable_int is
   the boxing side's answer and is deliberately narrow -- widening it would put
   sp_box_int_or_nil on optcarrot's pixel path -- so a container read is asked
   here instead. A miss on a specialized Array or Hash answers the element
   type's own C nil (#4070), which is exactly the shape the receiver guard is
   for. */
int recv_may_be_sentinel(Compiler *c, int node) {
  if (node < 0) return 0;
  if (call_returns_nullable_int(c, node)) return 1;
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, node);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int rr = nt_ref(nt, node, "receiver");
  if (rr < 0) return 0;
  TyKind rrt = comp_ntype(c, rr);
  if (!ty_is_array(rrt) && !ty_is_hash(rrt)) return 0;
  int aa = 0; (void)call_args(nt, node, &aa);
  /* fetch(k, default) and dig with a default never miss into nil */
  if (sp_streq(nm, "[]") || sp_streq(nm, "at") || sp_streq(nm, "dig") ||
      sp_streq(nm, "first") || sp_streq(nm, "last") ||
      sp_streq(nm, "find") || sp_streq(nm, "detect") ||
      (sp_streq(nm, "fetch") && aa == 1))
    return 1;
  return 0;
}

int nil_answers_name(const char *n) {
  static const char *const names[] = {
    "to_s", "inspect", "to_i", "to_f", "to_r", "to_c", "to_a", "to_h",
    "nil?", "hash", "class", "object_id", "frozen?", "dup", "clone", "freeze",
    "itself", "tap", "then", "yield_self", "display",
    "==", "!=", "===", "eql?", "equal?", "=~", "!",
    "is_a?", "kind_of?", "instance_of?", "respond_to?",
    "&", "|", "^",
    "send", "__send__", "public_send", "method", "methods",
    "instance_variables", "instance_variable_get", "instance_variable_set",
    "instance_variable_defined?", "singleton_class", "define_singleton_method",
    "extend", "enum_for", "to_enum", "pretty_print",
  };
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
    if (sp_streq(n, names[i])) return 1;
  return 0;
}

/* A String slot of the pattern family (String#split's separator): a
   String or nil passes as the String slot does, but a value of any other
   class -- true and false included -- is CRuby's "wrong argument type X
   (expected Regexp)", not the implicit-conversion wording. A Regexp is the
   one class the family is not wrong about, and it belongs to the arm's own
   Regexp path, never here. */
void emit_str_pattern_expr(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_REGEX) unsupported_feature(c, node, "a Regexp separator reached the String pattern slot");
  if (t == TY_BOOL) {
    int tb = ++g_tmp;
    buf_printf(b, "({ int _t%d = (", tb); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", _t%d"
                  " ? \"wrong argument type true (expected Regexp)\""
                  " : \"wrong argument type false (expected Regexp)\");"
                  " (const char *)0; })", tb);
    return;
  }
  const char *cn = conv_wrong_cls_name(t);
  if (cn && t != TY_STRING && t != TY_STRBUF) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"wrong argument type %s (expected Regexp)\"); (const char *)0; })", cn);
    return;
  }
  /* nil is split's documented whitespace mode: evaluate for side effects and
     pass NULL, which every sp_str_split_* entry answers as that mode (#4223).
     A separator that is only nil AT RUN TIME (a poly slot) has to keep the
     same answer, where the loose string conversion below renders nil as ""
     and silently turns the call into a character split; a non-nil non-string
     in that slot still raises through the strict conversion. */
  if (t == TY_NIL) {
    buf_puts(b, "({ (void)("); emit_expr(c, node, b);
    buf_puts(b, "); (const char *)NULL; })");
    return;
  }
  if (t == TY_POLY || t == TY_UNKNOWN) {
    buf_puts(b, "sp_poly_arg_str_or_null(");
    emit_boxed(c, node, b);
    buf_puts(b, ")");
    return;
  }
  emit_str_expr_nilable(c, node, b);
}

int emit_scalar_call(Compiler *c, int id, Buf *b) {
  /* Shared-mutable shim (#3227): setbyte on a strbuf local -- shadow-copy
     re-entry, same as emit_array_call's. */
  {
    const NodeTable *ntS = c->nt;
    const char *nmS = nt_str(ntS, id, "name");
    int recvS = nt_ref(ntS, id, "receiver");
    if (nmS && recvS >= 0 && comp_ntype(c, recvS) == TY_STRING &&
        sp_streq(nmS, "setbyte")) {
      if (sb_iv_expr_shim(c, id, recvS, b, emit_scalar_call)) return 1;
      const char *sbn = strbuf_local_name(c, recvS);
      if (sbn && g_nren < MAX_RENAME) {
        Scope *shs = comp_scope_of(c, recvS);
        LocalVar *shlv = scope_local(shs, sbn);
        int tH = ++g_tmp;
        Buf armb; memset(&armb, 0, sizeof armb);
        snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", sbn);
        snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_sb%d", tH);
        g_nren++;
        TyKind sv_ty = shlv->type; shlv->type = TY_STRING;
        int handled = emit_scalar_call(c, id, &armb);
        shlv->type = sv_ty;
        g_nren--;
        if (!handled) { free(armb.p); }
        else {
          buf_printf(b, "({ sp_String *_t%d = lv_%s;"
                        " if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data);"
                        " const char *lv__sb%d = sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1]));"
                        " SP_GC_ROOT(lv__sb%d);"
                        " sp_int _res%d = %s;"
                        " sp_String_set_bin(_t%d, lv__sb%d); _res%d; })",
                     tH, rename_local(sbn), tH, tH, tH, tH, tH,
                     tH, armb.p ? armb.p : "0", tH, tH, tH);
          free(armb.p);
          return 1;
        }
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
  /* scalar receiver methods: evaluate the receiver once into rs, then
     splice its text (so a literal/complex receiver isn't rebuilt). */
  if (recv >= 0 && (rt == TY_STRING || rt == TY_INT || rt == TY_FLOAT)) {
    Buf rs; memset(&rs, 0, sizeof rs);
    /* Reading a shared-mutable string as a value copies its whole buffer so
       the value cannot alias the handle (#3227). A method that only looks at
       the bytes and answers a scalar or a freshly built string keeps nothing,
       so it can read the live buffer instead -- `text[i]` in a scan loop was
       copying the whole subject on every character. */
    if (rt == TY_STRING && name && str_recv_reads_only(name))
      emit_strbuf_read_ref(c, recv, &rs);
    if (!rs.p) emit_expr(c, recv, &rs);
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
    /* A receiver that can carry the nil sentinel IS nil, and CRuby's nil
       answers only the names NilClass defines -- every other name is a
       NoMethodError. The arms below read the sentinel as an ordinary value, so
       `h["zz"].succ` answered -9223372036854775807 and `h["zz"].bit_length`
       answered 63, silently. #4070 spelled the check out per name (to_s,
       inspect, to_i, to_f) and the names it did not reach kept the old
       behaviour; this asks once, in front of all of them. Only a receiver the
       compiler already knows to be nullable pays for the test, so the hot int
       path is unchanged, and a safe-navigation call is left alone -- there the
       nil arm is the point. */
    Buf gbody; memset(&gbody, 0, sizeof gbody);
    Buf *g_outer_b = NULL; int g_tmpid = 0; char g_rname[24];
    if ((rt == TY_INT || rt == TY_STRING) && name && recv >= 0 && !nil_answers_name(name) &&
        recv_may_be_sentinel(c, recv)) {
      const char *sop_g = nt_str(nt, id, "call_operator");
      if (!(sop_g && sp_streq(sop_g, "&."))) {
        /* Bind the receiver once and let every arm below read the temp: some
           of them fold the call to a constant (`size` is sizeof(sp_int)) or to
           the receiver itself (`numerator`) and never render the receiver
           text at all, so a guard spliced into that text would vanish. The
           arms emit into gbody and the guard wraps whatever they produced. */
        g_tmpid = ++g_tmp;
        snprintf(g_rname, sizeof g_rname, "_t%d", g_tmpid);
        g_outer_b = b; b = &gbody; r = g_rname;
        /* conversions the arms emit belong BELOW the guard's nil check --
           CRuby raises its NoMethodError without asking #to_str -- so the
           call-level hold, which would hoist them above it, stands down and
           they render inline inside the guarded body */
        if (g_conv_hold) g_conv_hold->guarded = 1;
      }
    }
    int handled = 1;

    if (rt == TY_STRING) {
      /* blockless "a".upto("c") materializes the succ-sequence as an array */
      if (sp_streq(name, "upto") && argc == 1 && nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "sp_StrArray_from_string_range(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", 0)");
      }
      /* a nil / true / false PATTERN in the regexp-expected family is CRuby's
         TypeError ("wrong argument type nil (expected Regexp)") -- it used to
         fall past every pattern-typed arm into NoMethodError, or silently
         skip the substitution */
      else if ((sp_streq(name, "sub") || sp_streq(name, "sub!") ||
                sp_streq(name, "gsub") || sp_streq(name, "gsub!") ||
                sp_streq(name, "match") || sp_streq(name, "match?") ||
                sp_streq(name, "scan")) && argc >= 1 &&
               (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[0]) == TY_BOOL)) {
        TyKind prty = comp_ntype(c, id);
        int prb = ++g_tmp;
        buf_printf(b, "({ (void)(%s); ", r);
        /* every argument evaluates in order before the raise, as a real
           dispatch would */
        if (comp_ntype(c, argv[0]) == TY_NIL) {
          buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");
        }
        else {
          buf_printf(b, "int _t%d = (", prb); emit_expr(c, argv[0], b); buf_puts(b, "); ");
        }
        for (int pa = 1; pa < argc; pa++) {
          buf_puts(b, "(void)("); emit_expr(c, argv[pa], b); buf_puts(b, "); ");
        }
        if (comp_ntype(c, argv[0]) == TY_NIL)
          buf_puts(b, "sp_raise_cls(\"TypeError\", \"wrong argument type nil (expected Regexp)\");");
        else
          buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                        " ? \"wrong argument type true (expected Regexp)\""
                        " : \"wrong argument type false (expected Regexp)\");", prb);
        buf_printf(b, " %s; })", raise_tail_value_c(c, prty));
      }
      /* string methods taking a regex-literal argument route to the engine */
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        const char *suf = comp_ntype(c, argv[1]) == TY_STR_STR_HASH ? "_str_str_hash" : "";
        buf_printf(b, "sp_re_%s%s(sp_re_pat_%d, %s, ", name, suf, re_lit_index(c, argv[0]), r);
        if (comp_ntype(c, argv[1]) == TY_STR_STR_HASH) emit_expr(c, argv[1], b);
        else emit_str_expr(c, argv[1], b);
        buf_puts(b, ")");
      }
      else if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2 &&
               nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "InterpolatedRegularExpressionNode")) {
        Buf rp; memset(&rp, 0, sizeof rp);
        emit_regex_pat_to_buf(c, argv[0], &rp);
        buf_printf(b, "sp_re_%s(%s, %s, ", name, rp.p ? rp.p : "NULL", r);
        emit_str_expr(c, argv[1], b); buf_puts(b, ")");
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
        if (comp_ntype(c, argv[1]) == TY_STR_STR_HASH) emit_expr(c, argv[1], b);
        else emit_str_expr(c, argv[1], b);
        buf_puts(b, ")");
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
               (re_lit_index(c, argv[0]) >= 0 || comp_ntype(c, argv[0]) == TY_STRING ||
                comp_ntype(c, argv[0]) == TY_REGEX || comp_ntype(c, argv[0]) == TY_POLY) &&
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
        /* pattern only known at run time (an inline `Regexp.new(s)`, a local
           holding one): the value already IS the mrb_regexp_pattern*. has_cap
           is 0 for such a pattern, so the block param stays a whole-match
           String -- the same shape a local bound to a capturing literal
           already yields here (#3389). */
        else if (comp_ntype(c, argv[0]) == TY_REGEX) {
          /* render the pattern to a scratch buffer: `Regexp.new(s)` roots its
             own argument, and those decls go to g_pre, which must receive them
             as whole statements rather than spliced into this initializer */
          Buf eb; memset(&eb, 0, sizeof eb);
          emit_expr(c, argv[0], &eb);
          buf_printf(g_pre, "sp_StrArray *_t%d = sp_re_scan(%s, _t%d); SP_GC_ROOT(_t%d);\n",
                     tm, eb.p ? eb.p : "NULL", tr, tm);
          free(eb.p);
        }
        else if (comp_ntype(c, argv[0]) == TY_POLY) {
          /* the pattern is a Regexp read out of a table, so it arrives boxed:
             its payload IS the compiled pattern */
          Buf pb2; memset(&pb2, 0, sizeof pb2);
          emit_boxed(c, argv[0], &pb2);
          buf_printf(g_pre, "sp_StrArray *_t%d = sp_re_scan((mrb_regexp_pattern *)(%s).v.p, _t%d); SP_GC_ROOT(_t%d);\n",
                     tm, pb2.p ? pb2.p : "sp_box_nil()", tr, tm);
          free(pb2.p);
        }
        else {
          buf_printf(g_pre, "sp_StrArray *_t%d = sp_str_scan(_t%d, ", tm, tr);
          emit_expr(c, argv[0], g_pre);
          buf_printf(g_pre, "); SP_GC_ROOT(_t%d);\n", tm);
        }
        /* the rows are pre-computed, so the match registers still hold the
           last match; walk the subject again per iteration when the body
           reads $~ or a capture global (#3601) */
        int sc_pos = (re_idx >= 0 && subtree_reads_match_globals(c, body)) ? ++g_tmp : -1;
        if (sc_pos >= 0) {
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "sp_int _t%d = 0;\n", sc_pos);
        }
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < _t%d->len; _t%d++) {\n", ti, ti, tm, ti);
        if (sc_pos >= 0) {
          emit_indent(g_pre, g_indent + 1);
          buf_printf(g_pre, "if (sp_re_match_at(sp_re_pat_%d, _t%d, _t%d) >= 0)"
                            " _t%d = sp_re_caps[1] > sp_re_caps[0] ? sp_re_caps[1] : sp_re_caps[1] + 1;\n",
                     re_idx, tr, sc_pos, sc_pos);
        }
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
      /* scan against a regex VALUE the arms above could not resolve to a
         precompiled literal (an interpolated pattern, a local holding one, an
         inline `Regexp.new(s)`): the value already IS the
         mrb_regexp_pattern*. Without this arm the call fell through to the
         unresolved-call gate and raised NoMethodError on the String (#3389).
         The result shape follows the type analyze settled on, so the two stay
         in step: an unresolvable pattern is typed poly_array and
         sp_re_scan_poly decides per match whether the row is the whole match
         or its captures. */
      else if (sp_streq(name, "scan") && argc == 1 && comp_ntype(c, argv[0]) == TY_REGEX &&
               nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "%s(", comp_ntype(c, id) == TY_POLY_ARRAY ? "sp_re_scan_poly" : "sp_re_scan");
        emit_expr(c, argv[0], b); buf_printf(b, ", %s)", r);
      }
      /* the same, for a pattern that arrives BOXED (a Regexp read out of a
         table): its payload is the compiled pattern */
      else if (sp_streq(name, "scan") && argc == 1 && comp_ntype(c, argv[0]) == TY_POLY &&
               nt_ref(nt, id, "block") < 0) {
        buf_printf(b, "%s((mrb_regexp_pattern *)(",
                   comp_ntype(c, id) == TY_POLY_ARRAY ? "sp_re_scan_poly" : "sp_re_scan");
        emit_boxed(c, argv[0], b);
        buf_printf(b, ").v.p, %s)", r);
      }
      /* the receiver is a spinel string, so its own byte length is what the
         symbol's name is -- a NUL in it is a byte of the name (#nul) */
      else if (sp_streq(name, "to_sym") || sp_streq(name, "intern")) {
        int tsy = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; sp_sym_intern_n(_t%d, sp_str_byte_len(_t%d)); })", tsy, r, tsy, tsy);
      }
      else if (sp_streq(name, "to_c") && argc == 0) buf_printf(b, "sp_str_to_c(%s)", r);
      else if (sp_streq(name, "chr") && argc == 0) buf_printf(b, "sp_str_chr(%s)", r);
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
      else if (sp_streq(name, "delete_prefix") && argc == 1) { buf_printf(b, "sp_str_delete_prefix(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete_suffix") && argc == 1) { buf_printf(b, "sp_str_delete_suffix(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
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
          buf_printf(b, "sp_str_chomp_sep(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
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
      else if ((sp_streq(name, "start_with?") || sp_streq(name, "end_with?")) && argc == 0) {
        /* both take any number of candidates, so none is false */
        buf_printf(b, "((void)(%s), (sp_bool)0)", r);
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
        int tmi = ++g_tmp, tsi = ++g_tmp;
        /* report the match position in characters, not bytes (#3056) */
        buf_printf(b, "({ const char *_t%d = %s; sp_int _t%d = sp_re_match(sp_re_pat_%d, _t%d);"
                      " _t%d < 0 ? SP_INT_NIL : sp_str_byte_to_char(_t%d, _t%d); })",
                   tsi, r, tmi, re_lit_index(c, argv[0]), tsi, tmi, tsi, tmi);
      }
      else if (sp_streq(name, "index") && argc == 1) {
        /* nil-on-miss carried as the SP_INT_NIL sentinel (a nullable int) */
        buf_printf(b, "sp_str_index_opt(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "index") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_index_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "index") && argc == 2) {
        buf_printf(b, "sp_str_index_from_opt(%s, ", r);
        emit_str_expr(c, argv[0], b); buf_puts(b, ", ");
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
        else buf_printf(b, "(sp_int)sp_str_byte_len(_t%d)", tsr);
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "byteindex") && argc == 1 && str_needle_p(c, argv[0])) {
        buf_printf(b, "sp_str_byteindex(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byteindex") && argc == 2 && str_needle_p(c, argv[0])) {
        buf_printf(b, "sp_str_byteindex_from(%s, ", r); emit_str_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byterindex") && argc == 1 && str_needle_p(c, argv[0])) {
        buf_printf(b, "sp_str_byterindex(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "byterindex") && argc == 2 && str_needle_p(c, argv[0])) {
        buf_printf(b, "sp_str_byterindex_from(%s, ", r); emit_str_expr(c, argv[0], b);
        buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if ((sp_streq(name, "partition") || sp_streq(name, "rpartition")) && argc == 1 &&
               re_lit_index(c, argv[0]) < 0) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "partition") && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* [before, match, after] from the first regex match, else [s, "", ""] */
        int tr = ++g_tmp;
        buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_new();"
                      " if (sp_re_match(sp_re_pat_%d, %s) >= 0) {"
                      " sp_StrArray_push(_t%d, sp_re_pre_match()); sp_StrArray_push(_t%d, sp_re_match_str);"
                      " sp_StrArray_push(_t%d, sp_re_post_match()); }\nelse {"
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
      else if (sp_streq(name, "rindex") && argc == 1) { buf_printf(b, "sp_str_rindex_opt(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "rindex") && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        buf_printf(b, "sp_re_rindex_from_opt(sp_re_pat_%d, %s, ", re_lit_index(c, argv[0]), r);
        emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rindex") && argc == 2) { buf_printf(b, "sp_str_rindex_from(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "crypt") && argc == 1) { buf_printf(b, "sp_str_crypt(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* scrub! mutates in place, so a frozen receiver raises -- but only when
         it would actually replace something: CRuby returns a frozen string
         with no invalid bytes unchanged (#3333, #3338). */
      else if (sp_streq(name, "scrub!") && argc == 0)
        buf_printf(b, "sp_str_scrub_bang(%s, 0)", r);
      else if (sp_streq(name, "scrub!") && argc == 1) {
        buf_printf(b, "sp_str_scrub_bang(%s, ", r); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "scrub") && argc == 0) buf_printf(b, "sp_str_scrub(%s, 0)", r);
      else if (sp_streq(name, "scrub") && argc == 1) { buf_printf(b, "sp_str_scrub(%s, ", r); emit_str_expr_nilable(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 1 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/] -> the matched substring, or nil (NULL) on no match */
        buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_match_str : NULL)", re_lit_index(c, argv[0]), r);
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2 && re_lit_index(c, argv[0]) >= 0 &&
               nt_type(c->nt, argv[1]) &&
               (sp_streq(nt_type(c->nt, argv[1]), "SymbolNode") ||
                sp_streq(nt_type(c->nt, argv[1]), "StringNode") ||
                comp_ntype(c, argv[1]) == TY_STRING)) {
        /* s[/(?<g>...)/, :g] or s[/(?<g>...)/, "g"] -> the named group, or nil (#3082) */
        int pi = re_lit_index(c, argv[0]);
        const char *nty = nt_type(c->nt, argv[1]);
        if (sp_streq(nty, "SymbolNode")) {
          const char *gname = nt_str(c->nt, argv[1], "value");
          buf_printf(b, "(sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_named_capture(sp_re_pat_%d, \"%s\") : NULL)",
                     pi, r, pi, gname ? gname : "");
        }
        else {
          /* a String name (literal or dynamic): evaluate it and look it up */
          int tnm = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = ", tnm); emit_str_expr(c, argv[1], b);
          buf_printf(b, "; sp_re_match(sp_re_pat_%d, %s) >= 0 ? sp_re_named_capture(sp_re_pat_%d, _t%d) : NULL; })",
                     pi, r, pi, tnm);
        }
      }
      else if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && argc == 2 && re_lit_index(c, argv[0]) >= 0) {
        /* s[/re/, n] -> capture group n (0 = whole match), or nil */
        int pi = re_lit_index(c, argv[0]);
        int tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[1], b);
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
        else buf_printf(b, "(sp_int)sp_str_length(%s), 0)", r);  /* endless: to the end */
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
        else { buf_printf(b, "sp_str_split_drop_trailing(%s, ", r); emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (sp_streq(name, "split") && argc == 2) {
        buf_printf(b, "sp_str_split_limit(%s, ", r); emit_str_pattern_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "clamp") && (argc == 2 ||
               (argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "RangeNode")))) {
        int lo_n, hi_n;
        if (argc == 2) { lo_n = argv[0]; hi_n = argv[1]; }
        else { int rn = argv[0]; lo_n = nt_ref(c->nt, rn, "left"); hi_n = nt_ref(c->nt, rn, "right"); }
        /* an exclusive Range has no greatest member to clamp to, and a
           two-argument min above max is out of order: both raise (#3593) */
        int excl_r = (argc == 1 && (nt_int(c->nt, argv[0], "flags", 0) & 4)) ? 1 : 0;
        int tc = ++g_tmp, tlo = ++g_tmp, thi = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; const char *_t%d = ", tc, r, tlo);
        if (lo_n >= 0) emit_expr(c, lo_n, b); else buf_puts(b, "NULL");
        buf_printf(b, "; const char *_t%d = ", thi);
        if (hi_n >= 0) emit_expr(c, hi_n, b); else buf_puts(b, "NULL");
        buf_puts(b, ";");
        if (excl_r)
          buf_puts(b, " sp_raise_cls(\"ArgumentError\", \"cannot clamp with an exclusive range\");");
        buf_printf(b, " if (_t%d && _t%d && sp_str_cmp_bytes(_t%d, _t%d) > 0)"
                      " sp_raise_cls(\"ArgumentError\", \"min argument must be less than or equal to max argument\");",
                   tlo, thi, tlo, thi);
        /* a one-sided Range clamps on the side it has (#3593) */
        buf_printf(b, " (_t%d && sp_str_cmp_bytes(_t%d, _t%d) < 0) ? _t%d :"
                      " ((_t%d && sp_str_cmp_bytes(_t%d, _t%d) > 0) ? _t%d : _t%d); })",
                   tlo, tc, tlo, tlo, thi, tc, thi, thi, tc);
      }
      else if (sp_streq(name, "oct") && argc == 0) buf_printf(b, "sp_str_oct(%s)", r);
      else if (sp_streq(name, "hex") && argc == 0) buf_printf(b, "sp_str_to_i_base(%s, 16)", r);
      else if (sp_streq(name, "to_r") && argc == 0) buf_printf(b, "sp_str_to_r(%s)", r);
      else if (sp_streq(name, "ord") && argc == 0) buf_printf(b, "sp_str_ord(%s)", r);
      /* force_encoding / encode! set state ON the receiver: CRuby raises on a
         frozen string whether or not the call would change anything (#3334).
         `b` and non-bang `encode` return a NEW string, so they never raise. */
      /* zero-argument concat / prepend return the receiver; a frozen one still
         raises, as CRuby checks before the (empty) append (#3339). */
      else if ((sp_streq(name, "concat") || sp_streq(name, "prepend")) && argc == 0)
        buf_printf(b, "(sp_str_check_mutable(%s), (%s))", r, r);
      else if ((sp_streq(name, "force_encoding") || sp_streq(name, "encode!")) && argc <= 2) {
        /* The argument was ignored entirely, so `force_encoding("ASCII-8BIT")`
           left the string naming UTF-8 -- and spinel's one tag is exactly what
           that argument asks for. A constant path (Encoding::BINARY) or a
           string literal both name it; anything else keeps today's no-op,
           since spinel has no third encoding to move to. */
        const char *fe_nm = NULL;
        if (argc >= 1) {
          const char *at = nt_type(nt, argv[0]);
          if (at && sp_streq(at, "ConstantPathNode")) fe_nm = nt_str(nt, argv[0], "name");
          else if (at && sp_streq(at, "StringNode")) {
            fe_nm = nt_str(nt, argv[0], "unescaped");
            if (!fe_nm) fe_nm = nt_str(nt, argv[0], "content");
          }
        }
        int fe_bin = 0, fe_txt = 0;
        if (fe_nm) {
          char fe_up[32]; size_t fl = 0;
          for (; fe_nm[fl] && fl < sizeof fe_up - 1; fl++) {
            char ch = fe_nm[fl];
            fe_up[fl] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : (ch == '_' ? '-' : ch);
          }
          fe_up[fl] = 0;
          fe_bin = sp_streq(fe_up, "ASCII-8BIT") || sp_streq(fe_up, "BINARY");
          fe_txt = sp_streq(fe_up, "UTF-8");
        }
        if (fe_bin) buf_printf(b, "(sp_str_check_mutable(%s), sp_str_as_binary(%s))", r, r);
        else if (fe_txt) buf_printf(b, "(sp_str_check_mutable(%s), sp_str_as_text(%s))", r, r);
        else buf_printf(b, "(sp_str_check_mutable(%s), (%s))", r, r);
      }
      else if ((sp_streq(name, "=~") || sp_streq(name, "!~")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_STRING) {
        /* `str =~ str` is a TypeError in CRuby, not a missing method: only a
           Regexp (or an object answering =~) is a valid right operand */
        buf_printf(b, "((void)(%s), sp_raise_cls(\"TypeError\", \"type mismatch: String given\"), (sp_bool)0)", r);
      }
      else if ((sp_streq(name, "=~") || sp_streq(name, "!~")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_NIL) {
        /* `str =~ nil` is nil in CRuby (and `!~` its negation), not a missing
           method; the operand still evaluates (it can be a nil-typed call) */
        buf_printf(b, "((void)(%s), (void)(", r);
        emit_expr(c, argv[0], b);
        if (sp_streq(name, "!~")) buf_puts(b, "), (sp_bool)1)");
        else if (comp_ntype(c, id) == TY_POLY) buf_puts(b, "), sp_box_nil())");
        else buf_printf(b, "), %s)", raise_tail_value(comp_ntype(c, id)));
      }
      else if ((sp_streq(name, "=~") || sp_streq(name, "!~")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_BOOL) {
        /* CRuby hands the operand back to the operand's own #=~, and
           booleans have none: NoMethodError, naming the value */
        buf_printf(b, "((void)(%s), sp_raise_cls(\"NoMethodError\", (", r);
        emit_expr(c, argv[0], b);
        buf_puts(b, ") ? \"undefined method '=~' for true\""
                  " : \"undefined method '=~' for false\"), (sp_bool)0)");
      }
      else if (sp_streq(name, "b") && argc == 0) {
        /* a fresh copy, not the receiver: CRuby's #b is never frozen, and
           handing back a frozen literal made `s.b << x` raise FrozenError */
        buf_printf(b, "sp_str_b(%s)", r);
      }
      else if ((sp_streq(name, "b") || sp_streq(name, "encode")) && argc <= 2) buf_printf(b, "(%s)", r);
      /* the answer is the receiver's own tag, not the constant UTF-8 this arm
         used to fold to while discarding the receiver: pack and String#b tag
         their answer BINARY, and every other reader of that tag agreed */
      else if (sp_streq(name, "encoding") && argc == 0)
        buf_printf(b, "sp_box_encoding(sp_str_is_binary(%s) ? sp_encoding_binary() : sp_encoding_utf8())", r);
      else if (sp_streq(name, "dump") && argc == 0) buf_printf(b, "sp_str_dump(%s)", r);
      else if (sp_streq(name, "undump") && argc == 0) buf_printf(b, "sp_str_undump(%s)", r);
      else if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_POLY) {
        /* runtime tag decides: a string argument compares, a boxed object
           that answers #to_str converts and compares (rb_check_string_type),
           anything else is nil (the call typed TY_POLY). The receiver is
           bound and rooted first: #to_str allocates, and the receiver may be
           a fresh string nothing else holds. The OPERAND is rooted one level
           down, inside sp_poly_check_str, which is where this arm and the
           runtime's own boxed comparison meet. */
        int ta2 = ++g_tmp, tb2 = ++g_tmp, tc2 = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d);"
                      " sp_RbVal _t%d = ", ta2, r, ta2, tb2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; const char *_t%d = sp_poly_check_str(_t%d);"
                      " (_t%d || _t%d.tag == SP_TAG_STR) ? ", tc2, tb2, tc2, tb2);
        if (sp_streq(name, "casecmp"))
          buf_printf(b, "sp_box_int(sp_str_casecmp(_t%d, _t%d ? _t%d : \"\"))", ta2, tc2, tc2);
        else
          buf_printf(b, "sp_box_bool(sp_str_casecmp(_t%d, _t%d ? _t%d : \"\") == 0)", ta2, tc2, tc2);
        buf_puts(b, " : sp_box_nil(); })");
      }
      /* an operand whose class answers #to_str: CRuby converts it and
         compares, where the arm below discarded it and answered nil. The
         answer is boxed because the conversion can still come back empty --
         a #to_str that answers nil is CRuby's nil casecmp, not a comparison
         with "" -- so the call is typed TY_POLY, as it is for a poly operand
         above (analyze_infer.c, analyze_infer_recv.c). */
      else if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1 &&
               str_cmp_conv_shape(c, argv[0])) {
        int tr, to, ts;
        emit_str_cmp_prologue(c, r, argv[0], &tr, &to, &ts, b);
        if (sp_streq(name, "casecmp"))
          buf_printf(b, "sp_box_int(sp_str_casecmp(_t%d, _t%d))", tr, ts);
        else
          buf_printf(b, "sp_box_bool(sp_str_casecmp(_t%d, _t%d) == 0)", tr, ts);
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
      else if (sp_streq(name, "byteslice") && argc == 2) { buf_printf(b, "sp_str_byteslice(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
      /* byteslice(range): resolve endpoints against the bytesize (#2348) */
      else if (sp_streq(name, "byteslice") && argc == 1 && comp_ntype(c, argv[0]) == TY_RANGE) {
        int trg = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", trg); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_str_byteslice_range(%s, _t%d.first, _t%d.last, _t%d.excl,"
                      " _t%d.first == INTPTR_MIN, _t%d.last == INTPTR_MAX); })",
                   r, trg, trg, trg, trg, trg);
      }
      /* single-index byteslice(i): nil at the bytesize boundary (#2333) */
      else if (sp_streq(name, "byteslice") && argc == 1) { buf_printf(b, "sp_str_byteslice1(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "setbyte") && argc == 2) {
        /* copy-on-write: rebind an lvalue receiver to the mutated copy
           (a literal's bytes live in static storage, #2029) */
        const char *rvt2 = nt_type(nt, recv);
        int lvw = rvt2 && (sp_streq(rvt2, "LocalVariableReadNode") ||
                           sp_streq(rvt2, "InstanceVariableReadNode"));
        int tv2 = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", tv2); emit_int_expr(c, argv[1], b);
        buf_puts(b, "; ");
        if (lvw) { emit_expr(c, recv, b); buf_puts(b, " = "); }
        buf_printf(b, "sp_str_setbyte_cow(%s, ", r); emit_int_expr(c, argv[0], b);
        buf_printf(b, ", _t%d); _t%d; })", tv2, tv2);
      }
      else if (sp_streq(name, "getbyte") && argc == 1) {
        /* Bounds/negative-correct: a negative index counts from the end and an
           out-of-range index is nil (SP_INT_NIL) -- getbyte is a nullable int. */
        buf_printf(b, "sp_str_getbyte_opt(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "squeeze") && argc == 0) buf_printf(b, "sp_str_squeeze(%s)", r);
      else if (sp_streq(name, "squeeze") && argc == 1) { buf_printf(b, "sp_str_squeeze_chars(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "squeeze") && argc >= 2) {
        buf_printf(b, "sp_str_squeeze_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_str_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if ((sp_streq(name, "tr") || sp_streq(name, "tr_s")) && argc == 2) {
        buf_printf(b, "sp_str_%s(%s, ", name, r); emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "delete") && argc == 0) { buf_printf(b, "(%s)", r); return 1; }
      else if (sp_streq(name, "delete") && argc == 1) { buf_printf(b, "sp_str_delete(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "delete") && argc >= 2) {
        buf_printf(b, "sp_str_delete_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_str_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "count") && argc == 0) { buf_printf(b, "(sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\"), 0LL)"); return 1; }
      else if (sp_streq(name, "count") && argc == 1) { buf_printf(b, "sp_str_count(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "count") && argc >= 2) {
        buf_printf(b, "sp_str_count_n(%s, (const char *[]){", r);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_str_expr(c, argv[a], b); }
        buf_printf(b, "}, %d)", argc);
      }
      else if (sp_streq(name, "lines") && argc == 0) buf_printf(b, "sp_str_lines(%s)", r);
      else if (sp_streq(name, "lines") && argc == 1 && comp_ntype(c, argv[0]) == TY_STRING) {
        buf_printf(b, "sp_str_lines_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      /* lines(sep, chomp: true): a separator and the keyword together (#3546) */
      else if (sp_streq(name, "lines") && argc == 2 &&
               comp_ntype(c, argv[0]) == TY_STRING && nt_type(nt, argv[1]) &&
               sp_streq(nt_type(nt, argv[1]), "KeywordHashNode")) {
        int chv = struct_kwarg_value(c, argv[1], "chomp");
        int isc = kw_flag_static(c, chv);
        if (isc < 0) { buf_puts(b, "("); emit_cond(c, chv, b); buf_puts(b, " ? "); }
        if (isc != 0) { buf_printf(b, "sp_str_lines_sep_chomp(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        if (isc < 0) buf_puts(b, " : ");
        if (isc != 1) { buf_printf(b, "sp_str_lines_sep(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        if (isc < 0) buf_puts(b, ")");
      }
      else if (sp_streq(name, "lines") && argc == 1 && nt_type(nt, argv[0]) &&
               sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
        int chomp_v = struct_kwarg_value(c, argv[0], "chomp");
        int is_chomp = kw_flag_static(c, chomp_v);
        if (is_chomp < 0) {
          buf_puts(b, "("); emit_cond(c, chomp_v, b);
          buf_printf(b, " ? sp_str_lines_chomp(%s) : sp_str_lines(%s))", r, r);
        }
        else buf_printf(b, "%s(%s)", is_chomp ? "sp_str_lines_chomp" : "sp_str_lines", r);
      }
      else if (sp_streq(name, "bytes") && argc == 0)   buf_printf(b, "sp_str_bytes(%s)", r);
      else if (sp_streq(name, "codepoints") && argc == 0) buf_printf(b, "sp_str_codepoints(%s)", r);
      else if (sp_streq(name, "unpack") && argc == 1)  { buf_printf(b, "sp_str_unpack(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
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
        buf_printf(b, "sp_str_unpack_off(%s, ", r); emit_str_expr(c, argv[0], b);
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
        emit_str_expr(c, argv[0], b);
        buf_puts(b, (u1t == TY_INT || u1t == TY_FLOAT) ? "), 0))" : "), 0)");
      }
      else if (sp_streq(name, "sum") && argc <= 1) {
        /* byte checksum: sum of byte values modulo 2**bits (default 16;
           bits <= 0 or >= 64 leaves the sum untruncated like CRuby) */
        int ts = ++g_tmp, tp = ++g_tmp, tacc = ++g_tmp, tbits = ++g_tmp;
        buf_printf(b, "({ const char *_t%d = %s; sp_int _t%d = ", ts, r, tbits);
        if (argc == 1) emit_int_expr(c, argv[0], b); else buf_puts(b, "16");
        buf_printf(b, "; sp_int _t%d = 0; for (const char *_t%d = _t%d; *_t%d; _t%d++)"
                      " _t%d += (unsigned char)*_t%d;"
                      " (_t%d <= 0 || _t%d >= 64) ? _t%d : (_t%d & ((((sp_int)1) << _t%d) - 1)); })",
                   tacc, tp, ts, tp, tp, tacc, tp, tbits, tbits, tacc, tacc, tbits);
      }
      else if (sp_streq(name, "chars") && argc == 0)   buf_printf(b, "sp_str_chars(%s)", r);
      else if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) buf_printf(b, "sp_str_succ(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 0)    buf_printf(b, "sp_str_to_i_cruby(%s)", r);
      else if (sp_streq(name, "to_i") && argc == 1)    { buf_printf(b, "sp_str_to_i_base(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "to_f") && argc == 0)    buf_printf(b, "sp_str_to_f_cruby(%s)", r);  /* underscores (#2330) */
      else if (sp_streq(name, "gsub") && argc == 2) {
        buf_printf(b, "sp_str_gsub(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2 && comp_ntype(c, argv[1]) == TY_STR_STR_HASH) {
        buf_printf(b, "sp_str_sub_str_str_hash(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "sub") && argc == 2) {
        /* pattern and replacement coerce to strings: an accessor / poly arg is
           a tagged sp_RbVal, not a const char*, so emit_str_expr unboxes it
           (#3198). */
        buf_printf(b, "sp_str_sub(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "tr") && argc == 2) {
        buf_printf(b, "sp_str_tr(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 1) {
        buf_printf(b, "sp_str_center(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "center") && argc == 2) {
        buf_printf(b, "sp_str_center2(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 1) {
        buf_printf(b, "sp_str_ljust(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "ljust") && argc == 2) {
        buf_printf(b, "sp_str_ljust2(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 1) {
        buf_printf(b, "sp_str_rjust(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "rjust") && argc == 2) {
        buf_printf(b, "sp_str_rjust2(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
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
        /* a demand-marked reader-call argument already emits the handle */
        if (!eq_sblv && comp_ntype(c, argv[0]) == TY_STRBUF &&
            nt_kind(nt, argv[0]) == NK_CallNode) {
          char rrefE2[192];
          if (strbuf_slot_ref(c, recv, rrefE2, sizeof rrefE2)) {
            buf_printf(b, "(%s == ", rrefE2);
            emit_expr(c, argv[0], b);
            buf_puts(b, ")");
            eq_sblv = 1;
          }
        }
        /* strbuf receiver vs a POLY operand (a container read): runtime
           handle identity against the boxed value (#3227 P6) */
        if (!eq_sblv && comp_ntype(c, argv[0]) == TY_POLY) {
          char rrefE3[192];
          if (strbuf_slot_ref(c, recv, rrefE3, sizeof rrefE3)) {
            int teq3 = ++g_tmp;
            buf_printf(b, "({ sp_RbVal _t%d = ", teq3);
            emit_boxed(c, argv[0], b);
            buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_STRBUF"
                          " && (sp_String *)_t%d.v.p == %s); })",
                       teq3, teq3, teq3, rrefE3);
            eq_sblv = 1;
          }
        }
        if (!eq_sblv) {
          char arefE[192];
          if (strbuf_slot_ref(c, argv[0], arefE, sizeof arefE)) {
            /* If the receiver is ALSO a strbuf slot (local or ivar), compare
               the two sp_String handles directly: a shared alias is one
               object, so `s1.equal?(s2)` is true (#3227). Otherwise `r` is a
               live-buffer expr (e.g. `(s << "x")`) and its cstr is compared. */
            char rrefE[192];
            if (strbuf_slot_ref(c, recv, rrefE, sizeof rrefE))
              buf_printf(b, "(%s == %s)", rrefE, arefE);
            else {
              int teq = ++g_tmp;
              buf_printf(b, "({ const char *_t%d = %s; "
                            "(const void *)_t%d == (const void *)sp_String_cstr(%s); })",
                         teq, r, teq, arefE);
            }
            eq_sblv = 1;
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
        buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      else if (sp_streq(name, "inspect")) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ? SPL(\"nil\") : sp_int_to_s(_t%d); })", _tn, r, _tn, _tn);
      }
      /* A miss on a specialized container hands this slot SP_INT_NIL, and the
         conversions are the ones CRuby answers FOR nil rather than refusing:
         `nil.to_i` is 0, `nil.to_f` is 0.0. Identity used to pass the sentinel
         straight through, so `h["zz"].to_i` printed nil (#4070). The to_s and
         inspect arms above already spell the same check. */
      else if (sp_streq(name, "to_f")) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ? 0.0 : ((sp_float)_t%d); })",
                   _tn, r, _tn, _tn);
      }
      else if ((sp_streq(name, "to_i") || sp_streq(name, "to_int")) && argc == 0) {
        int _tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ? 0 : _t%d; })",
                   _tn, r, _tn, _tn);
      }
      else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
                sp_streq(name, "round") || sp_streq(name, "truncate")) &&
               argc == 0) buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "round") && argc >= 1 && nt_type(nt, argv[argc - 1]) &&
               sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) {
        int hv2 = kwh_lookup(nt, argv[argc - 1], "half");
        const char *hm = (hv2 >= 0 && nt_type(nt, hv2) && sp_streq(nt_type(nt, hv2), "SymbolNode"))
                           ? nt_str(nt, hv2, "value") : NULL;
        /* any mode other than the three CRuby names is an ArgumentError */
        if (hm && !sp_streq(hm, "even") && !sp_streq(hm, "down") && !sp_streq(hm, "up")) {
          buf_printf(b, "({ (void)(%s); sp_raise_cls(\"ArgumentError\","
                        " sp_sprintf(\"invalid rounding mode: %%s\", ", r);
          emit_str_literal(b, hm);
          buf_puts(b, ")); (sp_int)0; })");
        }
        else if (argc == 1) buf_printf(b, "(%s)", r);   /* no digits: self */
        else {
          int md = hm && sp_streq(hm, "even") ? 0 : hm && sp_streq(hm, "down") ? 2 : 1;
          buf_printf(b, "sp_int_round_half(%s, ", r);
          emit_int_expr(c, argv[0], b);
          buf_printf(b, ", %d)", md);
        }
      }
      else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
                sp_streq(name, "round") || sp_streq(name, "truncate")) && argc == 1) {
        buf_printf(b, "sp_int_%s(%s, ", name, r); emit_int_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "abs"))    buf_printf(b, "sp_int_abs(%s)", r);
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
        buf_printf(b, "; sp_int _lo%d = _t%d.first == INTPTR_MIN"
                      " ? (sp_raise_cls(\"ArgumentError\","
                      " \"The beginless range for Integer#[] results in infinity\"), 0)"
                      " : _t%d.first;"
                      " sp_int _sh%d = ((%s) >> _lo%d);"
                      " _t%d.last == INTPTR_MAX ? _sh%d"
                      " : (_sh%d & ((((sp_int)1) << (_t%d.last - _lo%d + (_t%d.excl ? 0 : 1))) - 1)); })",
                   trb, trb, trb,
                   trb, r, trb,
                   trb, trb,
                   trb, trb, trb, trb);
      }
      else if (sp_streq(name, "[]") && argc == 1) {
        /* clamped: a literal-folded out-of-range index was an undefined C
           shift (right answer on x86's masked shifts, garbage elsewhere).
           A Bignum index is far past the receiver's width, so the bit is the
           sign bit: 0 for a non-negative receiver, 1 for a negative one. */
        if (comp_ntype(c, argv[0]) == TY_BIGINT) {
          buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
          buf_printf(b, "); (sp_int)((%s) < 0 ? 1 : 0); })", r);
        }
        else { buf_printf(b, "sp_int_bit((%s), ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      }
      else if (sp_streq(name, "bit_length") && argc == 0) buf_printf(b, "sp_int_bit_length(%s)", r);
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((sp_float)(%s) / (", r); emit_float_expr(c, argv[0], b); buf_puts(b, "))"); }
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
      else if (sp_streq(name, "i") && argc == 0) buf_printf(b, "((sp_Complex){0.0, (sp_float)(%s), 0})", r);
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
      /* An Integer slot carries SP_INT_NIL for a miss on a specialized
         container, and CRuby REFUSES these on nil rather than answering
         false: `h["zz"].positive?` was a silent false (#4070). The
         conversions nil does answer are checked further up. */
      else if (sp_streq(name, "even?") || sp_streq(name, "odd?") ||
               sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
               sp_streq(name, "negative?")) {
        const char *op = sp_streq(name, "even?") ? "% 2 == 0"
                       : sp_streq(name, "odd?")  ? "% 2 != 0"
                       : sp_streq(name, "zero?") ? "== 0"
                       : sp_streq(name, "positive?") ? "> 0" : "< 0";
        int _tn = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ?"
                      " (sp_raise_cls(\"NoMethodError\","
                      " \"undefined method '%s' for nil\"), FALSE) : (_t%d ",
                   _tn, r, _tn, name, _tn);
        buf_puts(b, op);
        buf_puts(b, "); })");
      }
      else if (sp_streq(name, "nonzero?")) buf_printf(b, "((%s) == 0 ? SP_INT_NIL : (%s))", r, r);
      else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* a Float divisor divides as floats: [floor-quotient Integer, Float mod] */
        int tb = ++g_tmp, tq = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " sp_int _t%d = (sp_int)floor((double)(%s) / _t%d);"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float((double)(%s) - (double)_t%d * _t%d)); _t%d; })",
                   tb, tq, r, tb, o, o, o, tq, o, r, tq, tb, o);
      }
      else if (sp_streq(name, "divmod") && argc == 1 &&
               comp_ntype(c, argv[0]) != TY_RATIONAL) {
        int tb = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
        buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); sp_IntArray_push(_t%d, sp_idiv(%s, _t%d));"
                      " sp_IntArray_push(_t%d, sp_imod(%s, _t%d)); _t%d; })", o, o, r, tb, o, r, tb, o);
      }
      else if (sp_streq(name, "div") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* Integer#div(Float) floors the real quotient (7.div(2.5) == 2) (#2425) */
        buf_printf(b, "((sp_int)floor((double)(%s) / (", r); emit_expr(c, argv[0], b); buf_puts(b, ")))");
      }
      else if (sp_streq(name, "div") && argc == 1) { buf_printf(b, "sp_idiv(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "gcd") || sp_streq(name, "lcm")) && argc == 1 &&
               (comp_ntype(c, argv[0]) == TY_FLOAT ||
                comp_ntype(c, argv[0]) == TY_STRING ||
                comp_ntype(c, argv[0]) == TY_NIL ||
                comp_ntype(c, argv[0]) == TY_BOOL ||
                comp_ntype(c, argv[0]) == TY_SYMBOL ||
                ty_is_array(comp_ntype(c, argv[0])) ||
                ty_is_hash(comp_ntype(c, argv[0])))) {
        /* every non-Integer argument is CRuby's "not an integer" TypeError;
           only a Float was caught, so a String went into sp_gcd's sp_int slot
           as a pointer (#3644) */
        buf_puts(b, "({ (void)(");
        emit_expr(c, argv[0], b);
        buf_printf(b, "); sp_raise_cls(\"TypeError\", \"not an integer\"); (sp_int)(%s); })", r);
      }
      else if (sp_streq(name, "gcd") && argc == 1 && comp_ntype(c, argv[0]) == TY_BIGINT) {
        /* gcd(int, bignum) divides the int receiver, so it always fits an
           sp_int; compute via the bigint gcd then narrow (#3006) */
        buf_printf(b, "sp_bigint_to_int(sp_bigint_gcd(sp_bigint_new_int(%s), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, "))");
      }
      else if (sp_streq(name, "gcd") && argc == 1) { buf_printf(b, "sp_gcd(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* lcm(bignum) is at least as large as the argument, so it stays big */
      else if (sp_streq(name, "lcm") && argc == 1 && comp_ntype(c, argv[0]) == TY_BIGINT) {
        buf_printf(b, "sp_bigint_lcm(sp_bigint_new_int(%s), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
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
        buf_printf(b, "sp_rational_mod(sp_rational_new((sp_int)(%s), 1), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "modulo") && argc == 1) { buf_printf(b, "sp_imod(%s, ", r); emit_int_divisor(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "remainder") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        /* x - y * (x/y).truncate, in doubles (7.remainder(2.5) is 2.0); a zero
           divisor raises like every other division-derived operation (#3649) */
        int tb = ++g_tmp;
        buf_printf(b, "({ double _t%d = ", tb); emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d == 0 ? (sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\"), 0.0)"
                      " : (double)(%s) - _t%d * trunc((double)(%s) / _t%d); })",
                   tb, r, tb, r, tb);
      }
      else if (sp_streq(name, "remainder") && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_RATIONAL) {
        buf_printf(b, "sp_rational_rem(sp_rational_new((sp_int)(%s), 1), ", r);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else if (sp_streq(name, "remainder") && argc == 1) { buf_printf(b, "sp_iremainder(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "divmod") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
        /* [floor quotient (Integer), self - q*b (Rational)] */
        int ta = ++g_tmp, tb2 = ++g_tmp, tq2 = ++g_tmp, to2 = ++g_tmp;
        buf_printf(b, "({ sp_Rational _t%d = sp_rational_new((sp_int)(%s), 1); sp_Rational _t%d = ", ta, r, tb2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_int _t%d = sp_rational_floor_i(sp_rational_div(_t%d, _t%d));"
                      " sp_Rational _r = sp_rational_sub(_t%d, sp_rational_mul(sp_rational_new(_t%d, 1), _t%d));"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_rational(_r)); _t%d; })",
                   tq2, ta, tb2, ta, tq2, tb2, to2, to2, to2, tq2, to2, to2);
      }
      else if (sp_streq(name, "size") && argc == 0) buf_puts(b, "((sp_int)sizeof(sp_int))");
      else if (sp_streq(name, "gcdlcm") && argc == 1 &&
               comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "); (void)(%s); sp_raise_cls(\"TypeError\", \"not an integer\");"
                      " sp_IntArray_new(); })", r);
      }
      else if (sp_streq(name, "gcdlcm") && argc == 1) {
        int ta = ++g_tmp, o = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
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
                comp_ntype(c, argv[0]) == TY_POLY || comp_ntype(c, argv[1]) == TY_POLY ||
                comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
        /* a Rational bound (like a Float bound) makes the applied bound decide
           the result class at runtime; box the operands and let sp_num_clamp
           return whichever is chosen unchanged (#3232) */
        buf_printf(b, "sp_num_clamp(sp_box_int(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      }
      /* clamp(lo, hi) with a Bignum bound: an sp_int receiver is inside any
         Bignum bound on that side, so only the sp_int side can bind (#3006) */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_BIGINT || comp_ntype(c, argv[1]) == TY_BIGINT)) {
        int tlo = comp_ntype(c, argv[0]) == TY_BIGINT, thi = comp_ntype(c, argv[1]) == TY_BIGINT;
        buf_puts(b, "({ ");
        if (tlo) { buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); "); }
        if (thi) { buf_puts(b, "(void)("); emit_expr(c, argv[1], b); buf_puts(b, "); "); }
        if (tlo && thi) buf_printf(b, "(sp_int)(%s); })", r);
        else if (tlo) {
          /* a Bignum LOW bound is above every sp_int receiver... unless it is
             negative, in which case the receiver already exceeds it */
          int tb2 = ++g_tmp;
          buf_printf(b, "sp_Bigint *_t%d = ", tb2); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_bigint_cmp(_t%d, sp_bigint_new_int(%s)) > 0"
                        " ? sp_bigint_to_int(_t%d) : (sp_int)(%s); })", tb2, r, tb2, r);
        }
        else {
          int tb2 = ++g_tmp;
          buf_printf(b, "sp_Bigint *_t%d = ", tb2); emit_expr(c, argv[1], b);
          buf_printf(b, "; sp_bigint_cmp(_t%d, sp_bigint_new_int(%s)) < 0"
                        " ? sp_bigint_to_int(_t%d) : sp_int_clamp_ck(%s, ", tb2, r, tb2, r);
          emit_expr(c, argv[0], b);
          buf_printf(b, ", %s); })", r);
        }
      }
      else if (sp_streq(name, "clamp") && argc == 2) { buf_printf(b, "sp_int_clamp_ck(%s, ", r); emit_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
      else if (sp_streq(name, "clamp") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) {
        /* int.clamp(float_range): the clamped-to bound is the Float endpoint (a
           boxed result); an in-range Int receiver stays Int. */
        int tv3 = ++g_tmp;
        buf_printf(b, "({ sp_int _t%d = (%s); sp_FloatRange _fr%d = ", tv3, r, tv3); emit_expr(c, argv[0], b);
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
        buf_printf(b, "({ sp_int _t%d = (%s);", tv3, r);
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
      /* digits(base) with a Bignum base: every digit of an sp_int receiver is
         below such a base, so the answer is the receiver itself (#3006) */
      else if (sp_streq(name, "digits") && argc == 1 && comp_ntype(c, argv[0]) == TY_BIGINT) {
        int tdb = ++g_tmp;
        buf_printf(b, "({ (void)("); emit_expr(c, argv[0], b);
        buf_printf(b, "); if ((%s) < 0) sp_raise_cls(\"Math::DomainError\", \"out of domain\");", r);
        buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", tdb, tdb);
        buf_printf(b, " sp_IntArray_push(_t%d, %s); _t%d; })", tdb, r, tdb);
      }
      else if (sp_streq(name, "digits") && argc == 1) { buf_printf(b, "sp_int_digits(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) &&
               argc == 1 && comp_ntype(c, argv[0]) == TY_BIGINT) {
        /* A Bignum mask exceeds int64, so an int receiver can never cover all
           its bits (allbits? is always false); anybits?/nobits? test the
           receiver against the mask's low 64 bits -- the only ones an int
           receiver can share (#2470). */
        if (sp_streq(name, "allbits?")) {
          buf_printf(b, "((void)(%s), (void)(", r); emit_expr(c, argv[0], b); buf_puts(b, "), 0)");
        }
        else {
          buf_printf(b, "(((%s) & sp_bigint_to_int(", r); emit_expr(c, argv[0], b);
          buf_printf(b, ")) %s 0)", sp_streq(name, "anybits?") ? "!=" : "==");
        }
      }
      else if (sp_streq(name, "allbits?") && argc == 1) { int t = ++g_tmp; buf_printf(b, "({ sp_int _t%d = ", t); emit_int_expr(c, argv[0], b); buf_printf(b, "; (((%s) & _t%d) == _t%d); })", r, t, t); }
      else if (sp_streq(name, "anybits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) != 0)"); }
      else if (sp_streq(name, "nobits?") && argc == 1) { buf_printf(b, "(((%s) & (", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")) == 0)"); }
      else if (sp_streq(name, "ceildiv") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) {
        buf_printf(b, "((sp_int)ceil((double)(%s) / (", r); emit_expr(c, argv[0], b); buf_puts(b, ")))");  /* (#2425) */
      }
      else if (sp_streq(name, "ceildiv") && argc == 1) { buf_printf(b, "sp_ceildiv(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      /* pow(exp, mod) with a Bignum modulus: the result is bounded by the
         modulus but the intermediates are not, so run it in bigint (#3006) */
      else if (sp_streq(name, "pow") && argc == 2 && comp_ntype(c, argv[1]) == TY_BIGINT) {
        buf_printf(b, "sp_bigint_powmod(sp_bigint_new_int(%s), ", r);
        emit_int_expr(c, argv[0], b); buf_puts(b, ", "); emit_expr(c, argv[1], b); buf_puts(b, ")");
      }
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
          buf_printf(b, "({ sp_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        /* coerce against a Rational computes in floats: [Float(other), Float(self)] (#2606) */
        else if (a0 == TY_RATIONAL) {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ sp_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, ta, o, r, o);
        }
        /* an Integer can't coerce with a Complex -> RangeError (#2606) */
        else if (a0 == TY_COMPLEX) {
          buf_puts(b, "((void)("); emit_expr(c, argv[0], b);
          buf_puts(b, "), (sp_raise_cls(\"RangeError\", \"can't convert Complex into Integer\"), (sp_FloatArray *)0))");
        }
        /* Only a NUMBER coerces to an Integer pair. Everything else is
           `[Float(other), Float(self)]`, which is where CRuby's messages come
           from -- and the argument went into the sp_int slot as itself before,
           so a String stopped the C build and a nil answered a coerced 0
           (#4011). */
        else if (a0 != TY_INT && a0 != TY_POLY && a0 != TY_UNKNOWN) {
          int o = ++g_tmp;
          buf_printf(b, "({ sp_float _tc%d = sp_poly_Float(", o); emit_boxed(c, argv[0], b);
          buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _tc%d);"
                        " sp_FloatArray_push(_t%d, (sp_float)(%s)); _t%d; })", o, o, o, o, r, o);
        }
        else if (a0 == TY_POLY) {
          /* the tag decides at run time, through the same helper the boxed
             receiver path uses */
          int o = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = sp_poly_coerce(sp_box_int(%s), ", o, r);
          emit_boxed(c, argv[0], b);
          buf_printf(b, "); sp_poly_to_poly_array(_t%d); })", o);
        }
        else {
          int ta = ++g_tmp, o = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
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
      int half_dyn = -1;
      int eff_argc = argc;
      /* only #round takes a tie-break mode; the other three reject a keyword
         argument outright (#3646) */
      if (!sp_streq(name, "round") && argc >= 1 && nt_type(c->nt, argv[argc - 1]) &&
          sp_streq(nt_type(c->nt, argv[argc - 1]), "KeywordHashNode") &&
          (sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "truncate"))) {
        buf_printf(b, "({ (void)(%s); sp_raise_cls(\"TypeError\","
                      " \"no implicit conversion of Hash into Integer\"); 0.0; })", r);
        return 1;
      }
      if (sp_streq(name, "round") && argc >= 1 && nt_type(c->nt, argv[argc - 1]) &&
          sp_streq(nt_type(c->nt, argv[argc - 1]), "KeywordHashNode")) {
        int hvd = kwh_lookup(nt, argv[argc - 1], "half");
        /* a nil mode is the default; a mode only known at run time is chosen
           there rather than aborting the build (#3646) */
        if (hvd >= 0 && nt_type(c->nt, hvd) && sp_streq(nt_type(c->nt, hvd), "NilNode"))
          eff_argc = argc - 1;
        else if (hvd >= 0 && !(nt_type(c->nt, hvd) && sp_streq(nt_type(c->nt, hvd), "SymbolNode"))) {
          half_dyn = hvd;
          eff_argc = argc - 1;
        }
      }
      if (half_dyn >= 0) {
        int tmv = ++g_tmp, tsm = ++g_tmp;
        int nd_lit = (eff_argc == 1 && nt_type(c->nt, argv[0]) &&
                      sp_streq(nt_type(c->nt, argv[0]), "IntegerNode"))
                     ? (int)nt_int(c->nt, argv[0], "value", 0) : 0;
        int nd_nonlit = (eff_argc == 1 && !(nt_type(c->nt, argv[0]) &&
                                            sp_streq(nt_type(c->nt, argv[0]), "IntegerNode")));
        buf_printf(b, "({ double _t%d = (%s); sp_sym _t%d = ", tmv, r, tsm);
        emit_expr(c, half_dyn, b);
        buf_puts(b, "; ");
        if (nd_nonlit) {
          int tnn = ++g_tmp;
          buf_printf(b, "sp_int _t%d = ", tnn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; (_t%d > 0)"
                        " ? ({ double _f = pow(10, (double)_t%d); sp_box_float(isinf(_f) ? _t%d"
                        " : sp_round_half_mode(_t%d * _f, _t%d) / _f); })"
                        " : ({ double _f = pow(10, (double)(-_t%d));"
                        " sp_box_int(isinf(_f) ? 0 : (sp_int)(sp_round_half_mode(_t%d / _f, _t%d) * _f)); }); })",
                     tnn, tnn, tmv, tmv, tsm, tnn, tmv, tsm);
        }
        else if (nd_lit > 0)
          buf_printf(b, "double _f = pow(10, %d); sp_round_half_mode(_t%d * _f, _t%d) / _f; })",
                     nd_lit, tmv, tsm);
        else if (nd_lit < 0)
          buf_printf(b, "double _f = pow(10, %d); (sp_int)(sp_round_half_mode(_t%d / _f, _t%d) * _f); })",
                     -nd_lit, tmv, tsm);
        else
          buf_printf(b, "(sp_int)sp_round_half_mode(_t%d, _t%d); })", tmv, tsm);
        return 1;
      }
      if (sp_streq(name, "round") && argc >= 1 && nt_type(c->nt, argv[argc - 1]) &&
          sp_streq(nt_type(c->nt, argv[argc - 1]), "KeywordHashNode")) {
        int hv = kwh_lookup(nt, argv[argc - 1], "half");
        if (hv >= 0 && nt_type(c->nt, hv) && sp_streq(nt_type(c->nt, hv), "SymbolNode")) {
          const char *hm = nt_str(c->nt, hv, "value");
          if (hm && sp_streq(hm, "even")) half_fn = "sp_round_half_even";
          else if (hm && sp_streq(hm, "down")) half_fn = "sp_round_half_down";
          else if (hm && sp_streq(hm, "up")) half_fn = "round";
          else {
            /* any other mode is CRuby's ArgumentError, not the default (#3647) */
            buf_printf(b, "({ (void)(%s); sp_raise_cls(\"ArgumentError\","
                          " sp_sprintf(\"invalid rounding mode: %%s\", ", r);
            emit_str_literal(b, hm ? hm : "?");
            buf_puts(b, ")); 0.0; })");
            return 1;
          }
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
      /* A POSITIVE digit count goes through the runtime helper: scaling by a
         power of ten and rounding the product answers a decimal short when the
         product's own representation error crosses the tie (#3983). */
      const char *precop = sp_streq(name, "floor") ? "SP_PREC_FLOOR"
                         : sp_streq(name, "ceil") ? "SP_PREC_CEIL"
                         : sp_streq(name, "truncate") ? "SP_PREC_TRUNC" : "SP_PREC_ROUND";
      if (half_fn) cfn = half_fn;
      if (half_fn && sp_streq(half_fn, "sp_round_half_even")) precop = "SP_PREC_HALF_EVEN";
      else if (half_fn && sp_streq(half_fn, "sp_round_half_down")) precop = "SP_PREC_HALF_DOWN";
      if ((sp_streq(name, "floor") || sp_streq(name, "ceil") ||
           sp_streq(name, "round") || sp_streq(name, "truncate"))) {
        if (nonlit) {
          /* The class depends on the runtime ndigits: Float when n > 0, Integer
             when n <= 0 (CRuby). Choose at runtime and return a boxed poly. */
          int tn = ++g_tmp, tv = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; double _t%d = (%s); (_t%d > 0)", tv, r, tn);
          buf_printf(b, " ? sp_box_float(sp_float_prec_op(_t%d, _t%d, %s))", tv, tn, precop);
          buf_printf(b, " : ({ if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " double _f = pow(10, (double)(-_t%d)); sp_box_int(isinf(_f) ? 0 : (sp_int)(%s(_t%d / _f) * _f)); }); })",
                     tv, tv, tv, tn, cfn, tv);
        }
        else if (ndig > 0 && sp_streq(name, "round")) {
          /* CRuby normalizes a nonzero value that rounds to zero to +0.0
             (a genuine -0.0 input keeps its sign) (#3235). */
          int tx = ++g_tmp;
          /* the tie-break mode applies here too: this branch hard-coded the
             default rounding, so `half:` was silently ignored (#3647) */
          buf_printf(b, "({ double _t%d = (%s);"
                        " double _r = sp_float_prec_op(_t%d, %d, %s);"
                        " (_t%d != 0.0 && _r == 0.0) ? 0.0 : _r; })",
                     tx, r, tx, ndig, precop, tx);
        }
        else if (ndig > 0)
          buf_printf(b, "sp_float_prec_op((%s), %d, %s)", r, ndig, precop);
        else if (ndig < 0) {  /* round to a power of ten left of the decimal -> Integer */
          int tg = ++g_tmp;
          buf_printf(b, "({ double _t%d = (%s);"
                        " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " double _f = pow(10, %d); (sp_int)(%s(_t%d / _f) * _f); })",
                     tg, r, tg, tg, tg, -ndig, cfn, tg);
        }
        else {
          int tg = ++g_tmp;
          buf_printf(b, "({ double _t%d = (%s);"
                        " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                        " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                        " (sp_int)%s(_t%d); })",
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
           with a Float bound cannot ride sp_Range (sp_int bounds truncate
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
            buf_printf(b, " %s _t%d = ", lo_f ? "double" : "sp_int", tlo);
            emit_expr(c, flo, b); buf_puts(b, ";");
          }
          if (fhi >= 0) {
            thi = ++g_tmp;
            buf_printf(b, " %s _t%d = ", hi_f ? "double" : "sp_int", thi);
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
        buf_printf(b, "({ sp_float _t%d = (%s); sp_float _t%d = ", tx, r, tn); emit_expr(c, argv[0], b);
        buf_printf(b, "; if (isnan(_t%d) || isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      /* an infinite dividend has no quotient: FloatDomainError (#3008) */
                      " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);"
                      " if (isinf(_t%d)) {"
                      /* an infinite divisor: same sign -> [0, x], opposite -> [-1, divisor] */
                      " if (_t%d == 0.0 || (_t%d > 0) == (_t%d > 0)) {"
                      " sp_PolyArray_push(_t%d, sp_box_int(0)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); }"
                      "\nelse { sp_PolyArray_push(_t%d, sp_box_int(-1)); sp_PolyArray_push(_t%d, sp_box_float(_t%d)); } }"
                      "\nelse {"
                      " sp_int _t%d = (sp_int)floor(_t%d / _t%d);"
                      " sp_PolyArray_push(_t%d, sp_box_int(_t%d));"
                      " sp_PolyArray_push(_t%d, sp_box_float(_t%d - (sp_float)_t%d * _t%d)); } _t%d; })",
                   tx, tn, tx, tx, tn,
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
        /* The epsilon must reach sp_float_rationalize as a float. emit_float_expr
           casts a Rational arg with (sp_float)(<struct>), which the C compiler
           rejects; convert it through sp_rational_to_f instead (#3224). */
        buf_printf(b, "sp_float_rationalize(%s, ", r);
        if (comp_ntype(c, argv[0]) == TY_RATIONAL) { buf_puts(b, "sp_rational_to_f("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
        else emit_float_expr(c, argv[0], b);
        buf_puts(b, ")");
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
        buf_printf(b, "({ sp_float _t%d = (%s); sp_float _t%d = ", tx, r, tn);
        emit_float_expr(c, argv[0], b);
        buf_printf(b, "; if (_t%d == 0.0) sp_raise_cls(\"ZeroDivisionError\", \"divided by 0\");"
                      " if (isinf(_t%d)) sp_raise_cls(\"FloatDomainError\", _t%d > 0 ? \"Infinity\" : \"-Infinity\");"
                      " if (isnan(_t%d)) sp_raise_cls(\"FloatDomainError\", \"NaN\");"
                      " (sp_int)floor(_t%d / _t%d); })",
                   tn, tx, tx, tx, tx, tn);
      }
      /* Float#remainder: truncated remainder, sign following the dividend -- exactly
         C fmod (distinct from Ruby's floored % / modulo). */
      else if (sp_streq(name, "remainder") && argc == 1) {
        buf_printf(b, "sp_fremainder(%s, ", r); emit_float_expr(c, argv[0], b); buf_puts(b, ")");
      }
      /* Complex-view methods: a Float is a real Complex (imaginary part 0). */
      else if (sp_streq(name, "abs2"))               buf_printf(b, "((%s) * (%s))", r, r);
      else if (sp_streq(name, "real") || sp_streq(name, "conj") ||
               sp_streq(name, "conjugate"))          buf_printf(b, "(%s)", r);
      else if (sp_streq(name, "imag") || sp_streq(name, "imaginary"))
        buf_printf(b, "((void)(%s), (sp_int)0)", r);
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
      /* a Rational bound: box the operands and clamp through sp_num_clamp, which
         understands Rational and returns the applied operand unchanged (#3232) */
      else if (sp_streq(name, "clamp") && argc == 2 &&
               (comp_ntype(c, argv[0]) == TY_RATIONAL || comp_ntype(c, argv[1]) == TY_RATIONAL)) {
        buf_printf(b, "sp_num_clamp(sp_box_float(%s), ", r); emit_boxed(c, argv[0], b); buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
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
        buf_printf(b, "({ double _t%d = (%s); %s _t%d = ", tf4, r, lo_f2 ? "double" : "sp_int", tlo2);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; %s _t%d = ", hi_f2 ? "double" : "sp_int", thi2);
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
          buf_printf(b, "({ sp_float _t%d = sp_rational_to_f(", ta); emit_expr(c, argv[0], b);
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
          buf_printf(b, "({ sp_int _t%d = ", ta); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, (sp_float)_t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
        /* Float#coerce is [Float(other), self], and Float() is where CRuby's
           errors come from: a nil answered a coerced 0.0 before (#4011). */
        else if (a0 != TY_FLOAT && a0 != TY_BIGINT && a0 != TY_UNKNOWN) {
          buf_printf(b, "({ sp_float _t%d = sp_poly_Float(", ta); emit_boxed(c, argv[0], b);
          buf_printf(b, "); sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
        else {
          buf_printf(b, "({ sp_float _t%d = ", ta); emit_expr(c, argv[0], b);
          buf_printf(b, "; sp_FloatArray *_t%d = sp_FloatArray_new();"
                        " sp_FloatArray_push(_t%d, _t%d);"
                        " sp_FloatArray_push(_t%d, (%s)); _t%d; })", o, o, ta, o, r, o);
        }
      }
      else if (sp_streq(name, "fdiv") && argc == 1) { buf_printf(b, "((%s) / (", r); emit_float_coerce_expr(c, argv[0], b); buf_puts(b, "))"); }
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
    if (g_outer_b) {
      Buf *ib = b; b = g_outer_b;
      if (handled) {
        /* the string sentinel is the NULL pointer, the int's is SP_INT_NIL */
        buf_printf(b, "({ %s _t%d = (%s); if (%s_t%d%s)"
                      " sp_raise_nomethod(sp_nomethod_msg(\"%s\", sp_box_nil())); ",
                   rt == TY_STRING ? "const char *" : "sp_int", g_tmpid,
                   rs.p ? rs.p : "",
                   rt == TY_STRING ? "!" : "", g_tmpid,
                   rt == TY_STRING ? "" : " == SP_INT_NIL", name);
        if (ib->p) buf_puts(b, ib->p);
        buf_puts(b, "; })");
      }
      else if (ib->p) buf_puts(b, ib->p);
      free(gbody.p);
    }
    free(rs.p);
    if (handled) return 1;
  }
  return 0;
}

/* The class operand for a hierarchy check on `recv`. A synthesized singleton
   subclass is NOT the object's class until the extend / `def obj.m` that made
   it has run, so read the id the object actually carries rather than folding
   the static one (#4084). Everything else keeps the fold, with the receiver
   evaluated for its effects. */
/* Does any user class have `cid` in its superclass chain? Then a slot typed
   `cid` can hold one of them at run time. */
static int class_has_descendants(Compiler *c, int cid) {
  for (int k = 0; k < c->nclasses; k++) {
    if (k == cid) continue;
    for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
      if (p == cid) return 1;
  }
  return 0;
}

/* The class to test a receiver's `is_a?` against. The receiver's STATIC type is
   only an upper bound: a `Base`-typed slot legitimately holds a `Sub`, which is
   the whole point of a subclass, so a class with descendants has to be asked at
   run time. Using the static id there made `is_a?(Sub)` inside a method defined
   on Base answer false for every Sub -- silently, and the identical test written
   at the call site answered true, because there the receiver's type IS Sub
   (#4142). A leaf class is exact, and a value type carries no tag, so both keep
   the constant. */
static void emit_isa_self_class(Compiler *c, int recv, int cid, Buf *b) {
  if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_value_type &&
      (c->classes[cid].is_singleton_of || class_has_descendants(c, cid))) {
    buf_puts(b, "((sp_Class){("); emit_expr(c, recv, b); buf_puts(b, ")->cls_id})");
    return;
  }
  buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), (sp_Class){%d})", cid);
}

/* `obj.x = v` is an assignment expression: its value is v as written, whatever
   the writer's body returns (`def x=(v); @x = v.to_s; end` still yields v). The
   argument is evaluated once, after the receiver, into a rooted temp; the
   dispatch reads the temp through g_argov, and the temp is the result. A call
   emit_stmt is lowering (g_setter_stmt_id) has no reader for the value and
   emits as before. Returns the temp, or -1 when the call is left alone, and
   the temp's type in *vt_out. */
static int setter_value_open(Compiler *c, int id, Buf *b, TyKind *vt_out) {
  const NodeTable *nt = c->nt;
  int argc; const int *argv = call_args(nt, id, &argc);
  if (id == g_setter_stmt_id || argc != 1 || nt_ref(nt, id, "block") >= 0 ||
      !name_is_plain_setter(nt_str(nt, id, "name")) || g_n_argov >= MAX_ARG_OVERRIDE)
    return -1;
  TyKind vt = comp_ntype(c, argv[0]);
  if (vt == TY_UNKNOWN) return -1;
  /* nil and void have no C storage type of their own: hold them boxed */
  int boxed = (vt == TY_NIL || vt == TY_VOID);
  Buf ab; memset(&ab, 0, sizeof ab);
  if (boxed) emit_boxed(c, argv[0], &ab);
  else emit_expr(c, argv[0], &ab);
  int tv = ++g_tmp;
  emit_indent(g_pre, g_indent);
  emit_ctype(c, boxed ? TY_POLY : vt, g_pre);
  buf_printf(g_pre, " _t%d = ", tv);
  buf_puts(g_pre, ab.p ? ab.p : "sp_box_nil()"); buf_puts(g_pre, ";\n");
  free(ab.p);
  if (boxed || vt == TY_POLY) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT_RBVAL(_t%d);\n", tv); }
  else if (needs_root(vt)) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tv); }
  g_argov_node[g_n_argov] = argv[0];
  snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", tv);
  g_n_argov++;
  buf_puts(b, "({ (void)(");
  *vt_out = boxed ? TY_POLY : vt;
  return tv;
}
/* The temp holds the argument as the dispatch reads it; the expression
   answers in the CALL's type, which the inference may have widened to poly
   (a class with both an attr_accessor and a `def x=` for the name) -- box
   the temp on the way out then. */
static void setter_value_close(Compiler *c, int id, TyKind vt, Buf *b, int tv) {
  if (tv < 0) return;
  g_n_argov--;
  buf_puts(b, "); ");
  if (comp_ntype(c, id) == TY_POLY && vt != TY_POLY) {
    char tn[32]; snprintf(tn, sizeof tn, "_t%d", tv);
    emit_boxed_text(c, vt, tn, b);
  }
  else buf_printf(b, "_t%d", tv);
  buf_puts(b, "; })");
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
  /* Object#frozen? / #freeze on a concurrency handle. A Mutex, Fiber, Thread
     or ConditionVariable is an ordinary heap instance and freezes like one --
     the answer used to be a flat "never frozen", so `freeze` was a no-op and
     `frozen?` stayed false after it (#3483). A Queue is the exception Ruby
     itself makes: freezing one raises, because a frozen queue could never be
     pushed to again. (A Fiber reached the front-end reject where its Thread
     sibling did not, #3470.) */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (rt == TY_MUTEX || rt == TY_QUEUE || rt == TY_CONDVAR ||
       rt == TY_FIBER || rt == TY_THREAD) &&
      (sp_streq(name, "frozen?") || sp_streq(name, "freeze")) &&
      !user_defines_or_reads(c, name)) {
    int tq = ++g_tmp;
    if (rt == TY_QUEUE) {
      if (sp_streq(name, "frozen?")) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_bool)0)");
      }
      else {
        buf_printf(b, "({ sp_queue *_t%d = ", tq); emit_expr(c, recv, b);
        buf_printf(b, "; sp_raise_cannot_freeze(sp_Queue_class_name(_t%d), (void *)_t%d); _t%d; })",
                   tq, tq, tq);
      }
      return 1;
    }
    if (sp_streq(name, "frozen?")) {
      buf_puts(b, "sp_gc_is_frozen((void *)("); emit_expr(c, recv, b); buf_puts(b, "))");
    }
    else {
      buf_puts(b, "(("); emit_ctype(c, rt, b); buf_puts(b, ")sp_gc_freeze((void *)(");
      emit_expr(c, recv, b); buf_puts(b, ")))");
    }
    return 1;
  }
  /* The concurrency handles are heap instances too -- a Mutex, Queue,
     SizedQueue, ConditionVariable or Fiber IS its pointer -- so identity is
     the same pointer comparison. They are not ty_is_object (no user class
     behind them), which left equal?/eql? on them refused by the front end,
     where it could not even be rescued (#3470). */
  if (recv >= 0 && argc == 1 &&
      (rt == TY_MUTEX || rt == TY_QUEUE || rt == TY_CONDVAR ||
       rt == TY_FIBER || rt == TY_THREAD) &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?")) &&
      !user_defines_or_reads(c, name)) {
    TyKind a0 = comp_ntype(c, argv[0]);
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
      comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0 &&
      /* a generated reader of the name owns it, as in CRuby (#4190) */
      comp_resolve_member(c, ty_object_class(rt), name, 0, NULL, NULL) != SP_MEMBER_ATTR) {
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
         StandardError, which have no user class index. Exception class names
         are registered fully qualified ("PG::Error"), so a nested-path
         argument must compare with the whole path (#3260). */
      if (class_is_exc_subclass(c, cid)) {
        char qbuf[192];
        const char *qn = isa_const_qualname(nt, argv[0], qbuf, sizeof qbuf);
        if (!qn) qn = cn;
        /* When two modules each hold a class of the same leaf name, the
           colliding one is carried in the AST already flattened (`A::Error`
           arrives as `A__Error`), and prefixing its module again produced
           "A::A__Error" -- a name no exception answers to, so is_a? was
           silently false for both while `rescue` and #ancestors stayed right
           (#4133, found under net/http's Timeout::Error beside URI::Error).
           The class table's own qualified name is the one the raise site
           uses, so ask it whenever the argument names a class we know. */
        {
          int qi = comp_class_index(c, qn);
          if (qi < 0) qi = comp_class_index(c, cn);
          if (qi >= 0) {
            const char *rn = class_ruby_name(c, qi);
            /* COPIED, not pointed at: class_ruby_name hands back a shared
               static buffer, and the emit_expr for the receiver below asks it
               for the receiver's own name -- which overwrote the target and
               made every check compare the receiver against itself. */
            if (rn) { snprintf(qbuf, sizeof qbuf, "%s", rn); qn = qbuf; }
          }
        }
        if (sp_streq(name, "instance_of?")) {
          buf_puts(b, "(strcmp(((sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "))->cls_name, \"%s\") == 0)", qn);
        }
        else {
          buf_puts(b, "sp_exc_is_a((volatile sp_Exception *)(");
          emit_expr(c, recv, b);
          buf_printf(b, "), \"%s\")", qn);
        }
        return 1;
      }
      if (target >= 0) {
        if (sp_streq(name, "instance_of?")) {
          /* a synthesized singleton subclass is instance_of? its parent
             (CRuby hides the singleton class) */
          if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_value_type &&
              class_has_descendants(c, cid)) {
            /* Same upper-bound problem as is_a? just below, and exactness is
               what instance_of? is FOR: ask the object. The ids that answer
               are the target and any singleton class of it, which is a set the
               compiler can enumerate (#4142). */
            int t9 = ++g_tmp;
            buf_printf(b, "({ sp_int _t%d = (", t9); emit_expr(c, recv, b);
            buf_printf(b, ")->cls_id; ");
            int first = 1;
            for (int k = 0; k < c->nclasses; k++) {
              if (singleton_visible_ci(c, k) != target) continue;
              buf_printf(b, "%s_t%d == %d", first ? "" : " || ", t9, k);
              first = 0;
            }
            if (first) buf_puts(b, "0");
            buf_puts(b, "; })");
          }
          else {
            buf_puts(b, "((void)("); emit_expr(c, recv, b);
            buf_printf(b, "), %d)", singleton_visible_ci(c, cid) == target);
          }
        }
        else {
          /* use sp_class_le_mod (via macro) so includes chain is checked */
          buf_puts(b, "sp_class_le(");
          emit_isa_self_class(c, recv, cid, b);
          buf_printf(b, ",((sp_Class){%d}))", target);
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
          buf_puts(b, "sp_class_le(");
          emit_isa_self_class(c, recv, cid, b);
          buf_printf(b, ",((sp_Class){%d}))", bid);
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
      buf_printf(b, "({ sp_Class _t%d = ", k); emit_expr(c, argv[0], b); buf_printf(b, "; ");
      if (sp_streq(name, "instance_of?")) {
        buf_printf(b, "((sp_Class){%d}).cls_id == _t%d.cls_id; })", singleton_visible_ci(c, cid), k);
      }
      else {
        buf_puts(b, "sp_class_le(");
        emit_isa_self_class(c, recv, cid, b);
        buf_printf(b, ",_t%d); })", k);
      }
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
         class (its bounds are sp_int), so unfold to sp_obj_clamp with a nil
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
          char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d.iv_%s", tv2, iv_c(iv_c(ci2->ivars[vi] + 1)));
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
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct &&
      /* A method written in the `Struct.new` / `Data.define` block overrides the
         generated one of the same name, as it does in CRuby: `[]` defined there
         has to run instead of the member lookup, which raised NameError for a
         key that is not a member (#3794). The generated accessors are not
         methods, so they are unaffected; the iterator this file synthesizes for
         a struct is a method and is served by the object path below. */
      !(name && comp_method_in_chain(c, ty_object_class(rt), name, NULL) >= 0)) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    /* #inspect / #to_s -> the generated (or user-overridden) struct/data stringifier */
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) {
      const char *cn = obj_str_cname(c, ty_object_class(rt), sp_streq(name, "inspect"));
      if (cn) { buf_printf(b, "sp_%s_%s((sp_%s *)", cn, name, cn); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    }
    int is_to_a = (sp_streq(name, "to_a") || sp_streq(name, "values") || sp_streq(name, "deconstruct"));
    /* CRuby's Data has neither #to_a nor #values (Struct has both); only
       #deconstruct answers its members, and asking for the others is a
       NoMethodError rather than the member list. */
    if (is_to_a && sc->is_data && !sp_streq(name, "deconstruct")) is_to_a = 0;
    if (is_to_a && argc == 0) {
      int t = ++g_tmp; int rt2 = ++g_tmp;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->name, t, rb.p ? rb.p : "", rt2, rt2);
      for (int i = 0; i < sc->nivars; i++) {
        buf_printf(b, " sp_PolyArray_push(_t%d, ", rt2);
        Buf fb; memset(&fb, 0, sizeof fb); buf_printf(&fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
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
            char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
            buf_printf(b, " lv_%s = ", vp); emit_boxed_text(c, sc->ivar_types[i], fb, b); buf_puts(b, ";");
          }
          /* a composite key/value (an Array or Hash literal built from the
             block parameters) hoists its construction into the prelude, which
             runs BEFORE these per-member assignments -- so it read stale
             parameters. Emit that setup here, after them (#3603). */
          Buf kpre; memset(&kpre, 0, sizeof kpre);
          Buf kbuf; memset(&kbuf, 0, sizeof kbuf);
          Buf vbuf; memset(&vbuf, 0, sizeof vbuf);
          Buf *sv_pre = g_pre; g_pre = &kpre;
          if (ke >= 0) { if (kt == TY_POLY && comp_ntype(c, ke) != TY_POLY) emit_boxed(c, ke, &kbuf); else emit_expr(c, ke, &kbuf); }
          if (ve >= 0) { if (vt == TY_POLY && comp_ntype(c, ve) != TY_POLY) emit_boxed(c, ve, &vbuf); else emit_expr(c, ve, &vbuf); }
          g_pre = sv_pre;
          if (kpre.p) { buf_puts(b, " "); buf_puts(b, kpre.p); }
          free(kpre.p);
          buf_printf(b, " sp_%sHash_set(_t%d, ", hn, rh);
          buf_puts(b, kbuf.p ? kbuf.p : "0"); free(kbuf.p);
          buf_puts(b, ", ");
          buf_puts(b, vbuf.p ? vbuf.p : "0"); free(vbuf.p);
          buf_puts(b, ");");
        }
      }
      else {
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", rh, comp_sym_intern(c, sc->ivars[i] + 1));
          char fb[300]; snprintf(fb, sizeof fb, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
          emit_boxed_text(c, sc->ivar_types[i], fb, b);
          buf_puts(b, ");");
        }
      }
      buf_printf(b, " _t%d; })", rh);
      return 1;
    }
    /* values_at with no keys selects nothing, as Array#values_at does */
    if (sp_streq(name, "values_at") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), sp_PolyArray_new())");
      return 1;
    }
    /* values_at(i, j, ... / range): member values by index, boxed */
    if (sp_streq(name, "values_at") && argc >= 1) {
      int tv4 = ++g_tmp, to4 = ++g_tmp;
      Buf rb4 = expr_buf(c, recv);
      /* built aside: a key the literal walk cannot resolve falls back to the
         runtime form below, and appending to the caller's buffer first would
         leave the abandoned prefix in it */
      Buf lit4; memset(&lit4, 0, sizeof lit4);
      Buf *b4 = &lit4;
      buf_printf(b4, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                 sc->c_name, tv4, rb4.p ? rb4.p : "", to4, to4);
      int ok4 = 1;
      for (int a4 = 0; a4 < argc && ok4; a4++) {
        const char *aty4 = nt_type(nt, argv[a4]);
        if (aty4 && sp_streq(aty4, "IntegerNode")) {
          long long ix = nt_int(nt, argv[a4], "value", 0);
          if (ix < 0) ix += sc->nivars;
          if (ix < 0 || ix >= sc->nivars) { ok4 = 0; break; }
          char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, iv_c(sc->ivars[(int)ix] + 1));
          buf_printf(b4, " sp_PolyArray_push(_t%d, ", to4);
          emit_boxed_text(c, sc->ivar_types[(int)ix], fb4, b4);
          buf_puts(b4, ");");
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
          /* a Range that runs past the last member pads with nil, the way
             Array#values_at does; the walk used to stop at the last member */
          for (long long ix = lo4; ix <= hi4; ix++) {
            if (ix < 0) continue;
            if (ix >= sc->nivars) { buf_printf(b4, " sp_PolyArray_push(_t%d, sp_box_nil());", to4); continue; }
            char fb4[300]; snprintf(fb4, sizeof fb4, "_t%d->iv_%s", tv4, iv_c(sc->ivars[(int)ix] + 1));
            buf_printf(b4, " sp_PolyArray_push(_t%d, ", to4);
            emit_boxed_text(c, sc->ivar_types[(int)ix], fb4, b4);
            buf_puts(b4, ");");
          }
        }
        else ok4 = 0;
      }
      if (ok4) {
        buf_printf(b4, " _t%d; })", to4);
        buf_puts(b, lit4.p ? lit4.p : "");
        free(lit4.p); free(rb4.p);
        return 1;
      }
      free(lit4.p);
      /* A key the loop above could not resolve at compile time (a local, an
         out-of-range offset, a name) resolves at run time instead of taking
         the whole file down (#3849). The partial output above is discarded by
         re-emitting from scratch. */
      if (!ok4) {
        int tv5 = ++g_tmp, to5 = ++g_tmp;
        Buf rb5; memset(&rb5, 0, sizeof rb5); buf_puts(&rb5, rb4.p ? rb4.p : "");
        free(rb4.p);
        char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", tv5);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);",
                   sc->c_name, tv5, rb5.p ? rb5.p : "", to5, to5);
        free(rb5.p);
        for (int a5 = 0; a5 < argc; a5++) {
          buf_printf(b, " sp_PolyArray_push(_t%d, ", to5);
          emit_struct_member_by_key(c, sc, rtxt, argv[a5], 1, 0, b);
          buf_puts(b, ");");
        }
        buf_printf(b, " _t%d; })", to5);
        return 1;
      }
    }
    /* #hash: combine the boxed member hashes so equal-valued structs agree.
       A member literally named `hash` shadows this with its reader (#2975). */
    if (sp_streq(name, "hash") && argc == 0 && comp_ivar_index(sc, "@hash") < 0) {
      int tv5 = ++g_tmp, th5 = ++g_tmp;
      Buf rb5 = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; uint64_t _t%d = 1469598103934665603ULL;",
                 sc->c_name, tv5, rb5.p ? rb5.p : "", th5);
      free(rb5.p);
      for (int i5 = 0; i5 < sc->nivars; i5++) {
        char fb5[300]; snprintf(fb5, sizeof fb5, "_t%d->iv_%s", tv5, iv_c(sc->ivars[i5] + 1));
        buf_printf(b, " _t%d = (_t%d ^ (uint64_t)sp_rbval_hash_key(", th5, th5);
        emit_boxed_text(c, sc->ivar_types[i5], fb5, b);
        buf_puts(b, ")) * 1099511628211ULL;");
      }
      buf_printf(b, " (sp_int)(_t%d >> 1); })", th5);
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
          if (nkey >= 64) { ok = 0; break; }
          if (mi2 < 0) continue;   /* a non-member key is omitted, not an error (#2974) */
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
          char fb2[300]; snprintf(fb2, sizeof fb2, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
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
      /* a `**hash` double-splat in the keyword hash carries member overrides
         only known at run time; look each member up in it (#2972) */
      int wds = -1;
      if (wkwh >= 0) {
        int en = 0; const int *els = nt_arr(nt, wkwh, "elements", &en);
        for (int e = 0; e < en; e++)
          if (nt_type(nt, els[e]) && sp_streq(nt_type(nt, els[e]), "AssocSplatNode"))
            wds = nt_ref(nt, els[e], "value");
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
          if (nt_type(nt, els[e]) && sp_streq(nt_type(nt, els[e]), "AssocSplatNode")) continue;
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
      int th = wds >= 0 ? ++g_tmp : -1;
      Buf rb = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s;", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
      if (th >= 0) { buf_printf(b, " sp_RbVal _t%d = ", th); emit_boxed(c, wds, b); buf_puts(b, ";"); }
      buf_printf(b, " sp_%s_new(", sc->c_name);
      for (int i = 0; i < sc->nivars; i++) {
        if (i) buf_puts(b, ", ");
        int val = wkwh >= 0 ? kwh_lookup(nt, wkwh, sc->ivars[i] + 1) : -1;
        if (val >= 0) {
          TyKind mt = sc->ivar_types[i];
          TyKind vt = comp_ntype(c, val);
          if (mt == TY_POLY && vt != TY_POLY) {
            emit_boxed(c, val, b);  /* box a concrete value into a poly member */
          }
          else if (mt != TY_POLY && vt == TY_POLY) {
            /* A poly (sp_RbVal) value into a concrete member: coerce it, mirroring
               the poly-arg path in emit_arg_or_default. The regular `.new` call
               goes through that path; this hand-rolled constructor call did not,
               so it assigned an sp_RbVal straight into a const char* / sp_int /
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
          }
          else {
            emit_expr(c, val, b);
          }
        }
        else if (th >= 0) {
          /* member not given literally: take it from the **hash if present,
             else copy from the receiver (#2972) */
          buf_printf(b, "({ sp_bool _f = 0; sp_RbVal _v = sp_poly_hash_get_pair_val(_t%d, "
                        "sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? (", th, sc->ivars[i] + 1);
          if (sc->ivar_types[i] == TY_POLY) buf_puts(b, "_v");
          else emit_unbox_text(c, sc->ivar_types[i], "_v", b);
          buf_printf(b, ") : _t%d->iv_%s; })", t, iv_c(sc->ivars[i] + 1));
        }
        else {
          buf_printf(b, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
        }
      }
      buf_puts(b, "); })");
      return 1;
    }
    /* CRuby's Data defines no #dig at all (Struct does), so digging into one
       is a NoMethodError on a direct call and a TypeError through an
       intermediate -- not a member read (#3919). */
    if (sp_streq(name, "dig") && sc->is_data) {
      TyKind dgr = comp_ntype(c, id);
      const char *dgv = default_value(dgr);
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); sp_raise_nomethod(sp_nomethod_msg(\"dig\", sp_box_obj((void *)0, %d))); %s; })",
                 ty_object_class(rt), dgv ? dgv : "0");
      return 1;
    }
    /* CRuby's Data defines no #[] either: a member is read by name only,
       and indexing is a NoMethodError -- not Struct's member access */
    if (sp_streq(name, "[]") && sc->is_data) {
      TyKind dar = comp_ntype(c, id);
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
      for (int da = 0; da < argc; da++) {
        buf_puts(b, "(void)("); emit_boxed(c, argv[da], b); buf_puts(b, "); ");
      }
      buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"[]\", sp_box_obj((void *)0, %d))); %s; })",
                 ty_object_class(rt), raise_tail_value_c(c, dar));
      return 1;
    }
    /* a Struct's [] / dig / deconstruct_keys validate like CRuby: a missing
       argument is ArgumentError (dig says "1+"), a nil / bool index is the
       Integer-conversion TypeError. Data keeps its own dispatch above. */
    if (((!sc->is_data && (sp_streq(name, "[]") || sp_streq(name, "dig"))) ||
         sp_streq(name, "deconstruct_keys")) && argc == 0) {
      TyKind z0 = comp_ntype(c, id);
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b);
      buf_printf(b, "); sp_raise_cls(\"ArgumentError\","
                    " \"wrong number of arguments (given 0, expected %s)\"); %s; })",
                 sp_streq(name, "dig") ? "1+" : "1",
                 raise_tail_value_c(c, z0));
      return 1;
    }
    if (!sc->is_data && sp_streq(name, "[]") && argc >= 2) {
      TyKind za = comp_ntype(c, id);
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
      for (int sa = 0; sa < argc; sa++) {
        buf_puts(b, "(void)("); emit_boxed(c, argv[sa], b); buf_puts(b, "); ");
      }
      buf_printf(b, "sp_raise_cls(\"ArgumentError\","
                    " \"wrong number of arguments (given %d, expected 1)\"); %s; })",
                 argc, raise_tail_value_c(c, za));
      return 1;
    }
    if (!sc->is_data && (sp_streq(name, "[]") || sp_streq(name, "dig")) && argc >= 1 &&
        (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[0]) == TY_BOOL)) {
      TyKind z1 = comp_ntype(c, id);
      int zb = ++g_tmp;
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
      if (comp_ntype(c, argv[0]) == TY_NIL) {
        buf_puts(b, "(void)("); emit_expr(c, argv[0], b); buf_puts(b, "); ");
      }
      else {
        buf_printf(b, "int _t%d = (", zb); emit_expr(c, argv[0], b); buf_puts(b, "); ");
      }
      for (int sa = 1; sa < argc; sa++) {   /* dig's trailing keys evaluate too */
        buf_puts(b, "(void)("); emit_boxed(c, argv[sa], b); buf_puts(b, "); ");
      }
      if (comp_ntype(c, argv[0]) == TY_NIL)
        buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion from nil to integer\");");
      else
        buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                      " ? \"no implicit conversion of true into Integer\""
                      " : \"no implicit conversion of false into Integer\");", zb);
      buf_printf(b, " %s; })", raise_tail_value_c(c, z1));
      return 1;
    }
    if (sp_streq(name, "dig") && argc >= 1) {
      /* literal key resolves a member at compile time */
      int mi = -1;
      const char *kty = nt_type(nt, argv[0]);
      if (kty && (sp_streq(kty, "SymbolNode") || sp_streq(kty, "StringNode"))) {
        /* a String names a member too, and inference resolves one: leaving it
           to the runtime walk answered a boxed value into the member-typed
           slot the call site declares (#3892) */
        const char *kv = sp_streq(kty, "SymbolNode") ? nt_str(nt, argv[0], "value")
                                                     : nt_str(nt, argv[0], "content");
        if (kv) { char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", kv);
                  mi = comp_ivar_index(sc, ivn); }
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
            snprintf(path + pl, sizeof path - pl, "->iv_%s", iv_c(cur->ivars[cmi] + 1));
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
        char fld[300]; snprintf(fld, sizeof fld, "_t%d->iv_%s", t, iv_c(sc->ivars[mi] + 1));
        TyKind mt = sc->ivar_types[mi];
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, t, rb.p ? rb.p : ""); free(rb.p);
        if (argc == 1) buf_puts(b, fld);
        else if (ty_is_hash(mt) && argc == 2) {
          const char *hn = ty_hash_cname(mt);
          buf_printf(b, "sp_%sHash_%s(%s, ", hn, ty_hash_val(mt) == TY_INT ? "get_opt" : "get", fld);
          emit_expr(c, argv[1], b); buf_puts(b, ")");
        }
        else if (ty_is_array(mt) && argc == 2) {
          /* array_kind has no name for a poly array (nor for the pointer-array
             kinds), and the NULL went straight into the C symbol (#3574) */
          const char *ak = (mt == TY_POLY_ARRAY) ? "Poly" : array_kind(mt);
          if (ak) {
            buf_printf(b, "sp_%sArray_get(%s, ", ak, fld); emit_expr(c, argv[1], b); buf_puts(b, ")");
          }
          else {
            buf_puts(b, "sp_poly_dig_step_key(");
            emit_boxed_text(c, mt, fld, b);
            buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
          }
        }
        else if (argc >= 2) {
          /* every other remaining key walks at run time: the arms above cover
             one step into a member, and the rest were silently dropped, which
             emitted the member itself where a dug value was wanted (#3881) */
          buf_printf(b, "sp_poly_dig_n(");
          emit_boxed_text(c, mt, fld, b);
          buf_printf(b, ", %d, (sp_RbVal[]){", argc - 1);
          for (int a = 1; a < argc; a++) { if (a > 1) buf_puts(b, ", "); emit_boxed(c, argv[a], b); }
          buf_puts(b, "})");
        }
        else buf_puts(b, fld);
        buf_puts(b, "; })");
        return 1;
      }
      /* a key no literal member matches (a local, an offset, a name) resolves
         at run time; each further key then digs from that value (#3849) */
      if (sc->nivars > 0) {
        int td = ++g_tmp;
        Buf rbd = expr_buf(c, recv);
        char rtxt[32]; snprintf(rtxt, sizeof rtxt, "_t%d", td);
        buf_printf(b, "({ sp_%s *_t%d = %s; ", sc->c_name, td, rbd.p ? rbd.p : "");
        free(rbd.p);
        if (argc == 1) emit_struct_member_by_key(c, sc, rtxt, argv[0], 0, 1, b);
        else {
          buf_puts(b, "sp_poly_dig_n(");
          emit_struct_member_by_key(c, sc, rtxt, argv[0], 0, 1, b);
          buf_printf(b, ", %d, (sp_RbVal[]){", argc - 1);
          for (int a = 1; a < argc; a++) { if (a > 1) buf_puts(b, ", "); emit_boxed(c, argv[a], b); }
          buf_puts(b, "})");
        }
        buf_puts(b, "; })");
        return 1;
      }
    }
    /* struct[key] = v: the member the key names takes the value. Only an
       in-range literal member name had an emitter, so a variable key, an
       out-of-range offset or a missing name was refused outright (#3849). */
    if (sp_streq(name, "[]=") && argc == 2) {
      int tw = ++g_tmp, tk = ++g_tmp, tk0 = ++g_tmp, tv = ++g_tmp;
      Buf rbw = expr_buf(c, recv);
      buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->c_name, tw, rbw.p ? rbw.p : "", tk);
      free(rbw.p);
      emit_boxed(c, argv[0], b);
      buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
      buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
                 tk, tk, tk, tk, sc->nivars);
      /* The assignment's own value is the right-hand side in ITS type -- that
         is what the call site is typed for -- so keep it, and box a copy for
         the per-member stores (#3897). */
      TyKind vt = comp_ntype(c, argv[1]);
      int tvraw = ++g_tmp;
      if (vt != TY_POLY && vt != TY_UNKNOWN) {
        buf_printf(b, " "); emit_ctype(c, vt, b);
        buf_printf(b, " _t%d = ", tvraw); emit_expr(c, argv[1], b); buf_puts(b, ";");
        char rawtxt[32]; snprintf(rawtxt, sizeof rawtxt, "_t%d", tvraw);
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed_text(c, vt, rawtxt, b); buf_puts(b, ";");
      }
      else {
        buf_printf(b, " sp_RbVal _t%d = ", tv); emit_boxed(c, argv[1], b); buf_puts(b, ";");
        tvraw = tv;
      }
      for (int i = 0; i < sc->nivars; i++) {
        buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%dLL))"
                      "||sp_rbval_eql_key(_t%d,sp_box_str(\"%s\"))){ _t%d->iv_%s = ",
                   tk, comp_sym_intern(c, sc->ivars[i] + 1), tk, (long long)i,
                   tk, sc->ivars[i] + 1, tw, iv_c(sc->ivars[i] + 1));
        char vtxt[32]; snprintf(vtxt, sizeof vtxt, "_t%d", tv);
        if (sc->ivar_types[i] == TY_POLY) buf_puts(b, vtxt);
        else emit_unbox_text(c, sc->ivar_types[i], vtxt, b);
        buf_puts(b, ";}\nelse");
      }
      buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                    " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                    " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                    " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                    " } _t%d; })",
                 tk0, sc->nivars, tk0, tk0, tk0, tvraw);
      return 1;
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
        buf_printf(b, "_t%d->iv_%s; })", t, iv_c(sc->ivars[mi] + 1));
        return 1;
      }
      /* general: generate chain of comparisons. Each arm has to ASSIGN into a
         result temp -- written as bare statements the chain is a void
         expression, which is not a value the caller can read (#3572). */
      if (sc->nivars > 0) {
        int t = ++g_tmp, tk = ++g_tmp, tr = ++g_tmp, tk0 = ++g_tmp;
        Buf rb = expr_buf(c, recv);
        buf_printf(b, "({ sp_%s *_t%d = %s; sp_RbVal _t%d = ", sc->c_name, t, rb.p ? rb.p : "", tk);
        free(rb.p);
        emit_boxed(c, argv[0], b);
        /* a negative offset counts from the end; keep the original for the
           error message */
        buf_printf(b, "; sp_RbVal _t%d = _t%d;", tk0, tk);
        buf_printf(b, " if (_t%d.tag == SP_TAG_INT && _t%d.v.i < 0) _t%d = sp_box_int(_t%d.v.i + %d);",
                   tk, tk, tk, tk, sc->nivars);
        buf_printf(b, " sp_RbVal _t%d = sp_box_nil();", tr);
        for (int i = 0; i < sc->nivars; i++) {
          buf_printf(b, " if(sp_rbval_eql_key(_t%d,sp_box_sym((sp_sym)%d))||sp_rbval_eql_key(_t%d,sp_box_int(%dLL))){ _t%d = ",
                     tk, comp_sym_intern(c, sc->ivars[i]+1), tk, (long long)i, tr);
          char fld2[300]; snprintf(fld2, sizeof fld2, "_t%d->iv_%s", t, iv_c(sc->ivars[i] + 1));
          emit_boxed_text(c, sc->ivar_types[i], fld2, b);
          buf_printf(b, ";}\nelse");
        }
        /* a miss is an error: IndexError for an offset, NameError for a name */
        buf_printf(b, " { if (_t%d.tag == SP_TAG_INT)"
                      " sp_raise_cls(\"IndexError\", sp_sprintf(\"offset %%lld too %%s for struct(size:%d)\","
                      " (long long)_t%d.v.i, _t%d.v.i < 0 ? \"small\" : \"large\"));"
                      " sp_raise_cls(\"NameError\", sp_sprintf(\"no member '%%s' in struct\", sp_poly_to_s(_t%d)));"
                      " } _t%d; })",
                   tk0, sc->nivars, tk0, tk0, tk0, tr);
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
        for (int ai = 0; ai < mre->nargs && ai < argc; ai++) {
          if (!sp_streq(mre->args[ai], "regexp") || re_lit_index(c, argv[ai]) >= 0) continue;
          /* a nil / true / false where the binding wants a pattern is CRuby's
             TypeError (StringScanner accepts String patterns, so its wording
             is the String one), not a missing method */
          TyKind pat = comp_ntype(c, argv[ai]);
          if (pat == TY_NIL || pat == TY_BOOL) {
            TyKind nrty = comp_ntype(c, id);
            int nrb = ++g_tmp;
            buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
            /* every argument evaluates in order before the raise */
            for (int na = 0; na < argc; na++) {
              if (na == ai && pat == TY_BOOL) {
                buf_printf(b, "int _t%d = (", nrb); emit_expr(c, argv[na], b); buf_puts(b, "); ");
              }
              else {
                buf_puts(b, "(void)("); emit_boxed(c, argv[na], b); buf_puts(b, "); ");
              }
            }
            if (pat == TY_NIL)
              buf_puts(b, "sp_raise_cls(\"TypeError\", \"no implicit conversion of nil into String\");");
            else
              buf_printf(b, "sp_raise_cls(\"TypeError\", _t%d"
                            " ? \"no implicit conversion of true into String\""
                            " : \"no implicit conversion of false into String\");", nrb);
            buf_printf(b, " %s; })", raise_tail_value_c(c, nrty));
            return 1;
          }
          nm = -1; break;
        }
      }
      if (nm >= 0) {
        NativeMethod *m = &c->native_methods[nm];
        native_arg_check(c, id, "native method", m, argc, argv);
        int wrap = sp_streq(m->ret, "string?");
        if (wrap) buf_puts(b, "sp_box_nullable_str(");
        buf_puts(b, m->csym); buf_puts(b, "("); emit_expr(c, recv, b);
        for (int ai = 0; ai < m->nargs && ai < argc; ai++) {
          buf_puts(b, ", ");
          TyKind aw = ffi_spec_to_ty(m->args[ai]);
          if (sp_streq(m->args[ai], "any")) emit_boxed(c, argv[ai], b);
          else if (sp_streq(m->args[ai], "regexp"))
            buf_printf(b, "sp_re_pat_%d", re_lit_index(c, argv[ai]));
          /* a write payload is the operand's #to_s, as IO#write takes it */
          else if (sp_streq(m->args[ai], "text")) emit_to_s_expr(c, argv[ai], b);
          /* the typed-slot emitters carry the implicit conversion protocol
             (poly unboxing, #to_str / #to_int on a user object) */
          else if (aw == TY_STRING) emit_str_expr(c, argv[ai], b);
          else if (aw == TY_INT) emit_int_expr(c, argv[ai], b);
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
          /* the write is a mutation like any other: a frozen receiver raises
             FrozenError rather than taking it (#3872) */
          if (!is_val && c->classes[cid].freeze_observed) {
            int tf9 = ++g_tmp;
            Buf rbf; memset(&rbf, 0, sizeof rbf); emit_expr(c, recv, &rbf);
            buf_printf(b, "({ sp_%s *_t%d = %s; ", c->classes[cid].c_name, tf9,
                       rbf.p ? rbf.p : "NULL");
            free(rbf.p);
            char selft[32]; snprintf(selft, sizeof selft, "_t%d", tf9);
            emit_frozen_obj_guard(c, cid, selft, b);
            buf_printf(b, "_t%d->iv_%s = ", tf9, iv_c(sym + 1));
            if (mt == TY_POLY) emit_boxed(c, argv[1], b);
            else emit_expr(c, argv[1], b);
            buf_puts(b, "; })");
            return 1;
          }
          buf_puts(b, "(("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s = ", acc, iv_c(sym + 1));
          if (mt == TY_POLY) emit_boxed(c, argv[1], b);
          else if (mt == TY_STRBUF) {
            char srefIS[1024];
            if (strbuf_slot_ref(c, argv[1], srefIS, sizeof srefIS)) buf_puts(b, srefIS);
            else {
              buf_puts(b, "sp_String_new_shared(");
              emit_str_expr(c, argv[1], b);
              buf_puts(b, ")");
            }
          }
          else emit_expr(c, argv[1], b);
          buf_puts(b, ")");
        }
        else if (mt == TY_STRBUF) {
          /* a shared-mutable slot reads out as a GC copy; the raw handle
             must not leak into a plain string context (#3227) */
          int tvG = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = (", tvG);
          emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s; _t%d ? sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1])) : NULL; })",
                     acc, iv_c(sym + 1), tvG, tvG);
        }
        else {
          buf_puts(b, "("); emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s", acc, iv_c(sym + 1));
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
    /* remove_instance_variable(:@x) returns the removed value. The fixed object
       layout can't truly undefine a slot, so a later read still sees the field;
       an undefined name raises NameError, matching CRuby (#3020). */
    if (sp_streq(name, "remove_instance_variable") && argc == 1 && nt_type(nt, argv[0]) &&
        (sp_streq(nt_type(nt, argv[0]), "SymbolNode") || sp_streq(nt_type(nt, argv[0]), "StringNode"))) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      int mi = -1;
      if (sym && sym[0] == '@' && !c->classes[cid].is_struct)
        for (int i = 0; i < c->classes[cid].nivars; i++)
          if (sp_streq(c->classes[cid].ivars[i], sym)) { mi = i; break; }
      if (mi >= 0) {
        const char *acc = comp_ty_value_obj(c, rt) ? "." : "->";
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", acc, iv_c(sym + 1));
      }
      else {
        if (recv >= 0) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, "), "); }
        else buf_puts(b, "(");
        buf_printf(b, "sp_raise_cls(\"NameError\", \"instance variable %s not defined\"), sp_box_nil())",
                   sym ? sym : "");
      }
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
      int reader_wins = comp_resolve_member(c, cid, name, 0, &mdc, NULL) == SP_MEMBER_ATTR;
      if (reader_wins) {
        /* a reader is zero-arity: excess arguments are CRuby's ArgumentError
           (a Struct member read with an argument answered the member). A
           splat / keyword-hash / forwarding argument can be empty at run
           time -- zero arguments to CRuby -- so those stay unguarded. */
        int rdr_dynamic = 0;
        for (int ra = 0; ra < argc; ra++) {
          const char *rat = nt_type(nt, argv[ra]);
          if (rat && (sp_streq(rat, "SplatNode") || sp_streq(rat, "KeywordHashNode") ||
                      sp_streq(rat, "ForwardingArgumentsNode") ||
                      sp_streq(rat, "BlockArgumentNode")))
            { rdr_dynamic = 1; break; }
        }
        if (argc > 0 && !rdr_dynamic) {
          TyKind rrty2 = comp_ntype(c, id);
          buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); ");
          for (int ra = 0; ra < argc; ra++) {
            buf_puts(b, "(void)("); emit_expr(c, argv[ra], b); buf_puts(b, "); ");
          }
          buf_printf(b, "sp_raise_cls(\"ArgumentError\","
                        " \"wrong number of arguments (given %d, expected 0)\"); %s; })",
                     argc, raise_tail_value_c(c, rrty2));
          return 1;
        }
        const char *rn2 = comp_resolve_alias(c, cid, name);
        /* a shared-mutable string slot reads out as a GC COPY of the current
           contents (the raw sp_String* handle must not leak into a plain
           string context); a demand-marked read hands out the handle (#3227) */
        char ivfull[300]; snprintf(ivfull, sizeof ivfull, "@%s", rn2);
        int rdiv = comp_ivar_index(&c->classes[rdc >= 0 ? rdc : cid], ivfull);
        if (rdiv >= 0 &&
            c->classes[rdc >= 0 ? rdc : cid].ivar_types[rdiv] == TY_STRBUF) {
          int tvR = ++g_tmp;
          buf_printf(b, "({ sp_String *_t%d = (", tvR);
          emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s; ", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(rn2));
          if (c->strbuf_box[id])
            buf_printf(b, "_t%d; })", tvR);
          else
            buf_printf(b, "_t%d ? sp_str_concat(sp_String_cstr(_t%d), (&(\"\\xff\")[1])) : NULL; })",
                       tvR, tvR);
          return 1;
        }
        buf_puts(b, "("); emit_expr(c, recv, b);
        buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(rn2));
        return 1;
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    /* a demand-marked read through a simple hand-written reader
       (`def body = @body`) hands out the ivar HANDLE via a field access:
       the C reader function returns the safe copy (#3227 P5) */
    if (mi >= 0 && c->strbuf_box[id]) {
      int lastH = scope_body_last(c, mi);
      if (lastH >= 0 && nt_kind(nt, lastH) == NK_InstanceVariableReadNode) {
        const char *ivnH = nt_str(nt, lastH, "name");
        int defcH = c->scopes[mi].class_id;
        int ivH = (ivnH && defcH >= 0) ? comp_ivar_index(&c->classes[defcH], ivnH) : -1;
        if (ivH >= 0 && c->classes[defcH].ivar_types[ivH] == TY_STRBUF) {
          buf_puts(b, "(");
          emit_expr(c, recv, b);
          buf_printf(b, ")%siv_%s", comp_ty_value_obj(c, rt) ? "." : "->", iv_c(ivnH + 1));
          return 1;
        }
      }
    }
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
        TyKind svt = TY_UNKNOWN;
        int sv = setter_value_open(c, id, b, &svt);
        emit_dispatch(c, cid, name, selfv, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        setter_value_close(c, id, svt, b, sv);
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
      TyKind svt = TY_UNKNOWN;
      int sv = setter_value_open(c, id, b, &svt);
      emit_dispatch(c, cid, name, selfptr, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
      setter_value_close(c, id, svt, b, sv);
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
    /* getlocal(off)/localtime(off): a fixed UTC offset, given as seconds or a
       "+HH:MM" string, reinterprets the instant in that zone (#3093) */
    else if ((sp_streq(name, "localtime") || sp_streq(name, "getlocal")) && argc == 1) {
      int mutate = sp_streq(name, "localtime") && r_lval;
      if (mutate) buf_printf(b, "(%s = ", r);
      buf_printf(b, "sp_time_getlocal_off(%s, ", r);
      /* a String offset ("+01:00"), or a user object naming one through
         #to_str, parses; anything else is a second count */
      TyKind ot = comp_ntype(c, argv[0]);
      if (ot == TY_STRING || obj_conv_method(c, ot, "to_str", TY_STRING, NULL) >= 0) {
        buf_puts(b, "sp_time_offset_from_str("); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else emit_int_expr(c, argv[0], b);
      buf_puts(b, ")");
      if (mutate) buf_puts(b, ")");
    }
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
      buf_printf(b, "({ sp_Time _t%d = (%s); (sp_float)_t%d.tv_sec + (sp_float)_t%d.tv_nsec / 1e9; })", tf, r, tf, tf);
    }
    else if (sp_streq(name, "subsec")) {
      /* CRuby: Integer 0 for a whole second, else the exact Rational */
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; _t%d.tv_nsec == 0 ? sp_box_int(0) "
                    ": sp_box_rational(sp_rational_new((sp_int)_t%d.tv_nsec, 1000000000)); })",
                 tt, r, tt, tt);
    }
    else if (sp_streq(name, "tv_usec") || sp_streq(name, "usec")) buf_printf(b, "((sp_int)(%s).tv_nsec / 1000)", r);
    else if (sp_streq(name, "tv_nsec") || sp_streq(name, "nsec")) buf_printf(b, "((sp_int)(%s).tv_nsec)", r);
    else if (sp_streq(name, "utc?") || sp_streq(name, "gmt?")) buf_printf(b, "((%s).is_utc == 1)", r);
    else if (sp_streq(name, "dst?") || sp_streq(name, "isdst")) buf_printf(b, "(sp_time_isdst(%s) != 0)", r);
    else if (sp_streq(name, "utc_offset") || sp_streq(name, "gmt_offset") || sp_streq(name, "gmtoff")) buf_printf(b, "sp_time_utc_offset(%s)", r);
    else if (sp_streq(name, "inspect")) buf_printf(b, "sp_time_inspect_v(%s)", r);
    else if (sp_streq(name, "to_s")) buf_printf(b, "sp_time_to_s_v(%s)", r);
    else if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) {
      if (argc == 1) { buf_printf(b, "sp_time_iso8601_frac(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else buf_printf(b, "sp_time_iso8601(%s)", r);
    }
    else if (sp_streq(name, "zone")) buf_printf(b, "sp_time_zone(%s)", r);
    else if (sp_streq(name, "class")) buf_puts(b, "((sp_Class){(sp_int)-1, SPL(\"Time\")})");
    else if (sp_streq(name, "getgm")) buf_printf(b, "sp_time_utc(%s)", r);  /* alias for getutc */
    else if (sp_streq(name, "xmlschema")) {
      if (argc == 1) { buf_printf(b, "sp_time_iso8601_frac(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
      else buf_printf(b, "sp_time_iso8601(%s)", r);
    }
    else if ((sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round")) &&
             argc <= 1) {
      /* The subsecond part to `ndigits` decimal places (#3089); no argument is
         ndigits 0, whole seconds. A negative count is CRuby's ArgumentError
         rather than a clamp to zero (#3700). The arithmetic lives in the
         runtime so the boxed receiver answers exactly the same (#4109). */
      int mode = sp_streq(name, "floor") ? 0 : sp_streq(name, "ceil") ? 1 : 2;
      buf_printf(b, "sp_time_round_to(%s, ", r);
      if (argc == 1) emit_int_expr(c, argv[0], b);
      else buf_puts(b, "0");
      buf_printf(b, ", %d)", mode);
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
      }
      /* a poly operand may hold a Time: unwrap and compare by instant */
      else if (comp_ntype(c, argv[0]) == TY_POLY) {
        int tt = ++g_tmp, tq = ++g_tmp;
        buf_printf(b, "({ sp_Time _t%d = %s; sp_RbVal _t%d = ", tt, r, tq); emit_expr(c, argv[0], b);
        buf_printf(b, "; (sp_bool)(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME && "
                      "sp_time_cmp(_t%d, *(sp_Time *)_t%d.v.p) == 0); })", tq, tq, tt, tq);
      }
      else { buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); }
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
      buf_printf(b, "({ sp_Time _t%d = %s; sp_rational_new((sp_int)_t%d.tv_sec * 1000000000 + _t%d.tv_nsec, 1000000000); })",
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
        {"subsec", "(_t%d.tv_nsec == 0 ? sp_box_int(0) : sp_box_rational(sp_rational_new((sp_int)_t%d.tv_nsec, 1000000000)))"},
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
      }
      else {
        /* CRuby's full key set, in order */
        static const char *const allk[] = {"year","month","day","yday","wday","hour","min","sec","subsec","dst","zone",NULL};
        for (int a = 0; allk[a]; a++)
          for (int t = 0; TK[t].k; t++)
            if (sp_streq(allk[a], TK[t].k)) { SG_EMIT_KEY(TK[t].k, TK[t].vfmt); break; }
      }
      #undef SG_EMIT_KEY
      buf_printf(b, " sp_box_obj(_t%d, SP_BUILTIN_SYM_POLY_HASH); })", th);
    }
    else if (sp_streq(name, "strftime") && argc == 1) { buf_printf(b, "sp_time_strftime(%s, ", r); emit_str_expr(c, argv[0], b); buf_puts(b, ")"); }
    /* Comparable#between? / #clamp compare a Time with a Time; CRuby raises
       ArgumentError ("comparison of Time with 1 failed") for anything else,
       where reading the operand as an sp_Time did not compile (#3865). */
    else if ((sp_streq(name, "between?") || sp_streq(name, "clamp")) && argc == 2 &&
             (comp_ntype(c, argv[0]) != TY_TIME || comp_ntype(c, argv[1]) != TY_TIME)) {
      int tt = ++g_tmp;
      buf_printf(b, "({ sp_Time _t%d = %s; (void)(", tt, r);
      emit_boxed(c, argv[0], b); buf_puts(b, "); (void)(");
      emit_boxed(c, argv[1], b);
      buf_puts(b, "); sp_raise_cls(\"ArgumentError\", sp_sprintf(\"comparison of Time with %s failed\", sp_poly_inspect(");
      emit_boxed(c, argv[0], b);
      if (sp_streq(name, "clamp")) buf_printf(b, "))); _t%d; })", tt);
      else buf_puts(b, "))); (sp_bool)0; })");
    }
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
      buf_printf(b, "sp_time_add(%s, %s(sp_float)(", r, name[0] == '-' ? "-" : "");
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
      buf_printf(b, "; (sp_int)sp_time_cmp(_t%d, _t%d); })", tt, tu);
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

  /* Process::Status instance methods. The boxed receiver is a
     sp_ProcessStatus *; the runtime helpers take the int status word
     (or the boxed struct for pid) and return unboxed scalars. The
     call-site codegen auto-boxes according to the analyze-infer
     return type. */
  if (recv >= 0 && rt == TY_PROCESS_STATUS) {
    Buf rs = expr_buf(c, recv);
    const char *r = rs.p ? rs.p : "";
    int done = 1;
    if (argc == 0) {
      if (sp_streq(name, "signaled?"))   buf_printf(b, "sp_process_status_signaled_p((%s)->status)", r);
      else if (sp_streq(name, "exited?"))     buf_printf(b, "sp_process_status_exited_p((%s)->status)", r);
      else if (sp_streq(name, "coredump?"))   buf_printf(b, "sp_process_status_coredump_p((%s)->status)", r);
      /* tri-state: -1 is CRuby's nil for a process that did not exit */
      else if (sp_streq(name, "success?"))
        { int tsx = ++g_tmp;
          buf_printf(b, "({ int _t%d = sp_process_status_success_p((%s)->status);"
                        " _t%d < 0 ? sp_box_nil() : sp_box_bool((sp_bool)_t%d); })",
                     tsx, r, tsx, tsx); }
      else if (sp_streq(name, "exitstatus"))  buf_printf(b, "sp_process_status_exitstatus((%s)->status)", r);
      else if (sp_streq(name, "termsig"))     buf_printf(b, "sp_process_status_termsig((%s)->status)", r);
      else if (sp_streq(name, "pid"))         buf_printf(b, "(%s)->pid", r);
      else if (sp_streq(name, "to_s"))        buf_printf(b, "sp_process_status_to_s((%s)->status, 0)", r);
      else if (sp_streq(name, "inspect"))     buf_printf(b, "sp_process_status_to_s((%s)->status, 1)", r);
      else if (sp_streq(name, "class"))       buf_puts(b, "((sp_Class){(sp_int)-163, NULL})");
      else if (sp_streq(name, "==") || sp_streq(name, "eql?"))
        { buf_puts(b, "((void)("); emit_boxed(c, recv, b); buf_puts(b, "), (sp_bool)0)"); }
      else done = 0;
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
           sp_MatchData_aref (sp_int) would be a C type error. */
        int mtmp = ++g_tmp, ktmp = ++g_tmp;
        buf_printf(b, "({ sp_MatchData *_t%d = %s; sp_RbVal _t%d = ", mtmp, r, ktmp);
        emit_expr(c, argv[0], b);
        buf_printf(b, "; _t%d.tag == SP_TAG_SYM ? sp_MatchData_aref_name(_t%d, sp_sym_to_s((sp_sym)_t%d.v.i)) :"
                      " _t%d.tag == SP_TAG_STR ? sp_MatchData_aref_name(_t%d, _t%d.v.s) :"
                      " sp_MatchData_aref(_t%d, sp_poly_to_i(_t%d)); })",
                   ktmp, mtmp, ktmp, ktmp, mtmp, ktmp, mtmp, ktmp);
      }
      else { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
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
    /* MatchData#=== is Object's: == (structural, above); #equal? is identity */
    else if ((sp_streq(name, "===") || sp_streq(name, "equal?")) && argc == 1) {
      Buf as = expr_buf(c, argv[0]);
      emit_native_object_protocol_text(c, name, TY_MATCHDATA, r, comp_ntype(c, argv[0]), as.p ? as.p : "0", b);
      free(as.p);
    }
    else if (sp_streq(name, "hash") && argc == 0) buf_printf(b, "sp_MatchData_hash(%s)", r);  /* content-based (#3014) */
    /* a MatchData is a heap instance: frozen? reads the bit freeze sets (#3638
       answered a flat false, which freeze then contradicted) */
    else if (sp_streq(name, "frozen?") && argc == 0) buf_printf(b, "sp_gc_is_frozen((void *)(%s))", r);
    else if (sp_streq(name, "freeze") && argc == 0) buf_printf(b, "((sp_MatchData *)sp_gc_freeze((void *)(%s)))", r);
    else if (sp_streq(name, "named_captures") && argc == 0) buf_printf(b, "sp_md_named_captures(%s)", r);
    /* named_captures(symbolize_names: true): symbol keys (#2530) */
    else if (sp_streq(name, "named_captures") && argc == 1) {
      /* symbolize_names: FALSE asks for the string keys the no-argument form
         gives; the argument was ignored and the keys came back symbols (#3640) */
      int sym_on = 1;
      { int kv = kwh_lookup(nt, argv[0], "symbolize_names");
        const char *kvt = kv >= 0 ? nt_type(nt, kv) : NULL;
        if (kvt && sp_streq(kvt, "FalseNode")) sym_on = 0; }
      buf_printf(b, sym_on ? "sp_md_named_captures_sym(%s)" : "sp_md_named_captures(%s)", r);
    }
    else if (sp_streq(name, "inspect") && argc == 0) buf_printf(b, "sp_MatchData_inspect(%s)", r);   /* #2500 */
    /* MatchData#match(n) is the group substring, #match_length(n) its byte
       length (nil when the group did not participate) (#2501) */
    /* a Symbol or String argument names a group; the integer slot read the
       symbol's id as an index (#3630) */
    else if ((sp_streq(name, "match") || sp_streq(name, "match_length")) && argc == 1 &&
             (comp_ntype(c, argv[0]) == TY_SYMBOL || comp_ntype(c, argv[0]) == TY_STRING)) {
      buf_printf(b, "sp_MatchData_%s(%s, ",
                 sp_streq(name, "match") ? "aref_name" : "match_length_name", r);
      if (comp_ntype(c, argv[0]) == TY_SYMBOL) {
        buf_puts(b, "sp_sym_to_s("); emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      else emit_expr(c, argv[0], b);
      buf_puts(b, ")");
    }
    else if (sp_streq(name, "match") && argc == 1) { buf_printf(b, "sp_MatchData_aref(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    else if (sp_streq(name, "match_length") && argc == 1) { buf_printf(b, "sp_MatchData_match_length(%s, ", r); emit_int_expr(c, argv[0], b); buf_puts(b, ")"); }
    /* #deconstruct is the captures array; #deconstruct_keys the named captures
       as a symbol-keyed hash (#2503) */
    else if (sp_streq(name, "deconstruct") && argc == 0) buf_printf(b, "sp_MatchData_captures(%s)", r);
    else if (sp_streq(name, "deconstruct_keys") && argc == 1) {
      buf_printf(b, "sp_md_deconstruct_keys(%s, ", r); emit_boxed(c, argv[0], b); buf_puts(b, ")");  /* filters by keys (#3015) */
    }
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
      else if (by_name) emit_expr(c, argv[0], b);
      else emit_int_expr(c, argv[0], b);
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
        else if (kt3 == TY_RANGE) {
          /* a Range argument selects a run of groups, as Array#values_at does;
             it went into sp_MatchData_aref's sp_int slot as a struct (#3627) */
          int rk = ++g_tmp, rj = ++g_tmp, rlo = ++g_tmp, rhi = ++g_tmp;
          buf_printf(b, " sp_Range _t%d = ", rk); emit_expr(c, argv[i], b);
          buf_printf(b, "; sp_int _t%d = _t%d.first, _t%d = _t%d.last - (_t%d.excl ? 1 : 0);",
                     rlo, rk, rhi, rk, rk);
          buf_printf(b, " for (sp_int _t%d = _t%d; _t%d <= _t%d; _t%d++)"
                        " sp_PolyArray_push(_t%d, sp_box_nullable_str(sp_MatchData_aref(_t%d, _t%d)));",
                     rj, rlo, rj, rhi, rj, at, mt, rj);
        }
        else if (kt3 == TY_POLY) {
          /* a poly key dispatches at runtime like #[]: a Symbol/String resolves
             by name, anything else is an index. Passing the raw sp_RbVal to
             sp_MatchData_aref (sp_int) would be a C type error. */
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
          emit_int_expr(c, argv[i], b); buf_puts(b, ")));");
        }
      }
      buf_printf(b, " _t%d; })", at);
    }
    /* no arguments selects nothing: an empty Array, as Array#values_at does
       (#3846) */
    else if (sp_streq(name, "values_at") && argc == 0)
      buf_printf(b, "((void)(%s), sp_PolyArray_new())", r);
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
   bounds -- yields a FloatArray; sp_Range stores sp_int bounds, so a literal
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
    buf_printf(b, "({ sp_Range _t%d = %s; sp_FloatArray_from_step((sp_float)_t%d.first, (sp_float)_t%d.last, ",
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
  /* String range ("a".."e"): a distinct sp_StrRange receiver. The endpoints
     answer natively; every traversal materializes the element array (#3064). */
  if (recv >= 0 && rt == TY_STR_RANGE) {
    TyKind a0 = argc >= 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    /* a stale cache reads UNKNOWN; re-infer so a String operand is not
       mistaken for an uncoverable one */
    if (argc >= 1 && (a0 == TY_UNKNOWN || a0 == TY_POLY)) a0 = infer_type(c, argv[0]);
    int tr = ++g_tmp;
    if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "first") ||
                      sp_streq(name, "min"))) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.first; })", tr); return 1;
    }
    if (argc == 0 && (sp_streq(name, "end") || sp_streq(name, "last") ||
                      sp_streq(name, "max"))) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d.last; })", tr); return 1;
    }
    /* step(n) / %(n): every nth member, as an Enumerator (#3671) */
    if (argc == 1 && (sp_streq(name, "step") || sp_streq(name, "%")) &&
        nt_ref(nt, id, "block") < 0) {
      int ta = ++g_tmp, tn = ++g_tmp, to = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_StrArray *_t%d = sp_srange_to_a(_t%d); SP_GC_ROOT(_t%d);", ta, tr, ta);
      buf_printf(b, " sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d <= 0) sp_raise_cls(\"ArgumentError\", \"step can't be 0\");", tn);
      buf_printf(b, " sp_StrArray *_t%d = sp_StrArray_new(); SP_GC_ROOT(_t%d);", to, to);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d += _t%d)"
                    " sp_StrArray_push(_t%d, sp_StrArray_get(_t%d, _t%d));",
                 ti, ti, ta, ti, tn, to, ta, ti);
      buf_printf(b, " sp_Enumerator_new_from(sp_box_str_array(_t%d)); })", to);
      return 1;
    }
    /* min(n) / max(n): the n smallest or largest members (#3665) */
    if (argc == 1 && (sp_streq(name, "min") || sp_streq(name, "max")) &&
        nt_ref(nt, id, "block") < 0) {
      int ta = ++g_tmp, tn = ++g_tmp;
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_StrArray *_t%d = sp_srange_to_a(_t%d); SP_GC_ROOT(_t%d);", ta, tr, ta);
      buf_printf(b, " sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
      if (sp_streq(name, "max")) buf_printf(b, " sp_StrArray_reverse_bang(_t%d);", ta);
      buf_printf(b, " sp_StrArray_slice(_t%d, 0, _t%d); })", ta, tn);
      return 1;
    }
    if ((sp_streq(name, "cover?") || sp_streq(name, "include?") ||
         sp_streq(name, "member?") || sp_streq(name, "===")) && argc == 1) {
      if (a0 == TY_STRING) {
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_srange_cover(_t%d, ", tr); emit_str_expr(c, argv[0], b);
        buf_puts(b, "); })"); return 1;
      }
      if (a0 == TY_POLY) {
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _a%d = ", tr); emit_boxed(c, argv[0], b);
        buf_printf(b, "; (sp_bool)(_a%d.tag == SP_TAG_STR &&"
                      " sp_srange_cover(_t%d, _a%d.v.s)); })", tr, tr, tr);
        return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if (sp_streq(name, "exclude_end?") && argc == 0) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; (sp_bool)_t%d.excl; })", tr); return 1;
    }
    /* Range#size counts integer elements: nil for a string range (CRuby) */
    if (sp_streq(name, "size") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), SP_INT_NIL)"); return 1;
    }
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) {
      if (a0 == TY_STR_RANGE) {
        int tr2 = ++g_tmp;
        buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; sp_StrRange _t%d = ", tr2); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_srange_eq(_t%d, _t%d); })", tr, tr2); return 1;
      }
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_srange_to_a(_t%d); })", tr); return 1;
    }
    if (sp_streq(name, "to_s") && argc == 0) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_srange_to_s(_t%d); })", tr); return 1;
    }
    if (sp_streq(name, "inspect") && argc == 0) {
      buf_printf(b, "({ sp_StrRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_srange_inspect(_t%d); })", tr); return 1;
    }
    if (sp_streq(name, "class") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), ((sp_Class){0, SPL(\"Range\")}))"); return 1;
    }
    if (sp_streq(name, "frozen?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_bool)1)"); return 1;
    }
    if (argc == 0 && (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
                      sp_streq(name, "dup") || sp_streq(name, "clone"))) {
      emit_expr(c, recv, b); return 1;
    }
  }
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
      int as_int2 = comp_ntype(c, id) == TY_INT;
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; %s_t%d.last; })", as_int2 ? "(sp_int)" : "", tr); return 1;
    }
    if (argc == 0 && sp_streq(name, "max")) {
      /* the endpoint the caller wrote: an Integer end answers an Integer,
         whatever the other endpoint made of the range's kind (#3837) */
      int as_int = comp_ntype(c, id) == TY_INT;
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; %ssp_frange_max(_t%d); })", as_int ? "(sp_int)" : "", tr); return 1;
    }
    /* min(n)/max(n) enumerate, which a Float bound cannot (#3665) */
    if (argc == 1 && (sp_streq(name, "min") || sp_streq(name, "max")) &&
        nt_ref(nt, id, "block") < 0) {
      buf_puts(b, "({ (void)("); emit_expr(c, recv, b); buf_puts(b, "); (void)(");
      emit_expr(c, argv[0], b);
      buf_puts(b, "); sp_raise_cls(\"TypeError\", \"can't iterate from Float\");"
                  " sp_box_nil(); })");
      return 1;
    }
    /* minmax reads the endpoints instead of iterating, which a Float begin
       cannot do (#3690) */
    if (argc == 0 && sp_streq(name, "minmax") && nt_ref(nt, id, "block") < 0) {
      int tm = ++g_tmp;
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_float _t%d = sp_frange_max(_t%d);"
                    " sp_FloatArray *_r%d = sp_FloatArray_new(); SP_GC_ROOT(_r%d);"
                    " sp_FloatArray_push(_r%d, _t%d.first); sp_FloatArray_push(_r%d, _t%d);"
                    " _r%d; })", tm, tr, tr, tr, tr, tr, tr, tm, tr);
      return 1;
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
        buf_printf(b, "; (sp_bool)((_a%d.tag == SP_TAG_INT || _a%d.tag == SP_TAG_FLT) &&"
                      " sp_frange_cover(_t%d, sp_poly_to_f(_a%d))); })", tr, tr, tr, tr);
        return 1;
      }
      /* a non-numeric argument can never be covered: false (eval for effect) */
      buf_puts(b, "((void)("); emit_expr(c, argv[0], b); buf_puts(b, "), 0)"); return 1;
    }
    if (sp_streq(name, "exclude_end?") && argc == 0) {
      buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
      buf_printf(b, "; (sp_bool)_t%d.excl; })", tr); return 1;
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
    /* Range#size counts the integers a range enumerates, so it answers only
       for an Integer begin -- and Infinity when the end is unbounded, which is
       exactly the shape an infinite bound puts on the float representation
       (#3670). A Float begin has no enumeration, as CRuby's TypeError says. */
    if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int rq2 = unwrap_parens(c, recv);
      int lo2 = (rq2 >= 0 && nt_type(nt, rq2) && sp_streq(nt_type(nt, rq2), "RangeNode"))
                  ? nt_ref(nt, rq2, "left") : -1;
      if (lo2 >= 0 && comp_ntype(c, lo2) == TY_INT) {
        buf_printf(b, "({ sp_FloatRange _t%d = ", tr); emit_expr(c, recv, b);
        buf_printf(b, "; _t%d.last == HUGE_VAL ? HUGE_VAL"
                      " : (sp_float)((sp_int)_t%d.last - (sp_int)_t%d.first"
                      " + (_t%d.excl ? 0 : 1)); })", tr, tr, tr, tr);
        return 1;
      }
    }
    if (sp_streq(name, "class") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), ((sp_Class){0, SPL(\"Range\")}))"); return 1;
    }
    if (sp_streq(name, "frozen?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_bool)1)"); return 1;
    }
    if (argc == 0 && (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
                      sp_streq(name, "dup") || sp_streq(name, "clone"))) {
      emit_expr(c, recv, b); return 1;
    }
    /* a Range value is never nil */
    if (sp_streq(name, "nil?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), (sp_bool)0)"); return 1;
    }
    /* is_a?/kind_of?/instance_of?/equal? via the boxed value's builtin identity
       (its class is "Range"; the helpers key on the SP_BUILTIN_FLOAT_RANGE tag) */
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                      sp_streq(name, "instance_of?"))) {
      int is_iof = sp_streq(name, "instance_of?");
      const char *cn = isa_const_name(nt, argv[0]);
      buf_printf(b, "({ sp_RbVal _t%d = ", tr); emit_boxed(c, recv, b); buf_puts(b, "; ");
      if (cn && is_iof) buf_printf(b, "(sp_bool)(strcmp(sp_poly_class_name(_t%d), \"%s\") == 0); })", tr, cn);
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
      emit_expr(c, argv[0], b); buf_puts(b, "), (sp_bool)0))"); return 1;
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
    /* (1..5.5): the end readers answer the literal Float, which the sp_int
       fields cannot hold; #to_s renders it too (#3896). */
    if (argc == 0 && block < 0) {
      int fe = range_lit_float_end(c, recv);
      if (fe >= 0 && (sp_streq(name, "end") || sp_streq(name, "last") ||
                      sp_streq(name, "max"))) {
        emit_float_expr(c, fe, b);
        return 1;
      }
      if (fe >= 0 && (sp_streq(name, "to_s") || sp_streq(name, "inspect"))) {
        int tr7 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tr7); emit_expr(c, recv, b);
        buf_printf(b, "; sp_sprintf(\"%%lld%s%%s\", (long long)_t%d.first, sp_float_to_s(",
                   (int)(nt_int(nt, unwrap_parens(c, recv), "flags", 0) & 4) ? "..." : "..", tr7);
        emit_float_expr(c, fe, b);
        buf_puts(b, ")); })");
        return 1;
      }
    }
    /* find / detect / take_while over an ENDLESS Range: there is no array to
       materialize, so walk up from the bounded end the way `each` does. A
       search that never succeeds does not terminate in CRuby either (#3863). */
    if (block >= 0 && argc == 0 && nt_type(nt, block) &&
        sp_streq(nt_type(nt, block), "BlockNode") &&
        (sp_streq(name, "find") || sp_streq(name, "detect") ||
         sp_streq(name, "take_while"))) {
      int rn8 = unwrap_parens(c, recv);
      if (rn8 >= 0 && nt_type(nt, rn8) && !sp_streq(nt_type(nt, rn8), "RangeNode")) {
        int sl8 = local_sole_range_node(c, rn8);
        if (sl8 >= 0) rn8 = sl8;
      }
      int endless = rn8 >= 0 && nt_type(nt, rn8) && sp_streq(nt_type(nt, rn8), "RangeNode") &&
                    nt_ref(nt, rn8, "right") < 0 && nt_ref(nt, rn8, "left") >= 0;
      const char *bp8 = block_param_name(c, block, 0);
      int body8 = nt_ref(nt, block, "body");
      int bn8 = 0; const int *bb8 = body8 >= 0 ? nt_arr(nt, body8, "body", &bn8) : NULL;
      if (endless && bp8 && bn8 >= 1) {
        int want_take = sp_streq(name, "take_while");
        Scope *bsc8 = comp_scope_of(c, block);
        LocalVar *lv8 = bsc8 ? scope_local(bsc8, bp8) : NULL;
        const char *bpr = rename_local(bp8);
        int tr8 = ++g_tmp, ti8 = ++g_tmp, to8 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        Buf rb8 = expr_buf(c, recv);
        buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr8, rb8.p ? rb8.p : "");
        free(rb8.p);
        emit_indent(g_pre, g_indent);
        if (want_take)
          buf_printf(g_pre, "sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);\n", to8, to8);
        else
          buf_printf(g_pre, "sp_int _t%d = SP_INT_NIL;\n", to8);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = _t%d.first; ; _t%d++) {\n", ti8, tr8, ti8);
        emit_indent(g_pre, g_indent + 1);
        if (lv8 && lv8->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", bpr, ti8);
        else buf_printf(g_pre, "lv_%s = _t%d;\n", bpr, ti8);
        for (int j = 0; j + 1 < bn8; j++) emit_stmt(c, bb8[j], g_pre, g_indent + 1);
        Buf cb8; memset(&cb8, 0, sizeof cb8);
        { int sv8 = g_indent; g_indent += 1; emit_cond(c, bb8[bn8 - 1], &cb8); g_indent = sv8; }
        emit_indent(g_pre, g_indent + 1);
        if (want_take)
          buf_printf(g_pre, "if (!(%s)) break; sp_IntArray_push(_t%d, _t%d);\n",
                     cb8.p ? cb8.p : "0", to8, ti8);
        else
          buf_printf(g_pre, "if (%s) { _t%d = _t%d; break; }\n", cb8.p ? cb8.p : "0", to8, ti8);
        free(cb8.p);
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        buf_printf(b, "_t%d", to8);
        return 1;
      }
    }
    /* reverse_each { } over a BEGINLESS Range: there is no array to
       materialize and no lower bound to stop at, so count down from the
       bounded end the way CRuby does -- a `break` in the block is what ends
       it (#3914). */
    if (block >= 0 && argc == 0 && nt_type(nt, block) &&
        sp_streq(nt_type(nt, block), "BlockNode") && sp_streq(name, "reverse_each")) {
      int rn7 = unwrap_parens(c, recv);
      if (rn7 >= 0 && nt_type(nt, rn7) && !sp_streq(nt_type(nt, rn7), "RangeNode")) {
        int sl7 = local_sole_range_node(c, rn7);
        if (sl7 >= 0) rn7 = sl7;
      }
      int beginless = rn7 >= 0 && nt_type(nt, rn7) && sp_streq(nt_type(nt, rn7), "RangeNode") &&
                      nt_ref(nt, rn7, "left") < 0 && nt_ref(nt, rn7, "right") >= 0;
      const char *bp7 = block_param_name(c, block, 0);
      int body7 = nt_ref(nt, block, "body");
      int bn7 = 0; const int *bb7 = body7 >= 0 ? nt_arr(nt, body7, "body", &bn7) : NULL;
      if (beginless && bp7 && bn7 >= 1) {
        Scope *bsc7 = comp_scope_of(c, block);
        LocalVar *lv7 = bsc7 ? scope_local(bsc7, bp7) : NULL;
        const char *bpr7 = rename_local(bp7);
        int tr7 = ++g_tmp, ti7 = ++g_tmp;
        emit_indent(g_pre, g_indent);
        Buf rb7 = expr_buf(c, recv);
        buf_printf(g_pre, "sp_Range _t%d = %s;\n", tr7, rb7.p ? rb7.p : "");
        free(rb7.p);
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "for (sp_int _t%d = _t%d.last - (_t%d.excl ? 1 : 0); ; _t%d--) {\n",
                   ti7, tr7, tr7, ti7);
        emit_indent(g_pre, g_indent + 1);
        if (lv7 && lv7->type == TY_POLY) buf_printf(g_pre, "lv_%s = sp_box_int(_t%d);\n", bpr7, ti7);
        else buf_printf(g_pre, "lv_%s = _t%d;\n", bpr7, ti7);
        /* a real C loop, so a `break` in the body lowers to a C break */
        g_c_loop_depth++;
        for (int j = 0; j < bn7; j++) emit_stmt(c, bb7[j], g_pre, g_indent + 1);
        g_c_loop_depth--;
        emit_indent(g_pre, g_indent); buf_puts(g_pre, "}\n");
        /* #reverse_each answers its receiver */
        buf_printf(b, "_t%d", tr7);
        return 1;
      }
    }
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
        buf_puts(b, "({ sp_raise_cls(\"TypeError\", \"can't iterate from NilClass\"); (sp_int)0; })");
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
                              ? "(sp_IntArray*)0" : "(sp_int)0";
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
        buf_printf(b, "({ sp_int _t%d = ", tb9);
        emit_int_expr(c, nt_ref(nt, rn9, "left"), b);
        buf_printf(b, "; double _t%d = ", te9);
        emit_expr(c, nt_ref(nt, rn9, "right"), b);
        buf_printf(b, "; double _d = _t%d - (double)_t%d;"
                      " _d < 0 ? 0 : (%d && _t%d == floor(_t%d)) ? (sp_int)_d : (sp_int)floor(_d) + 1; })",
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
          /* blockless count: the succ-sequence length (#3070). Range#size is
             nil for a non-numeric range, so it is not served here. */
          if (argc == 0 && nt_ref(nt, id, "block") < 0 && sp_streq(name, "count")) {
            int ta9 = ++g_tmp;
            buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta9);
            emit_expr(c, lo9, b); buf_puts(b, ", "); emit_expr(c, hi9, b);
            buf_printf(b, ", %d); (sp_int)_t%d->len; })", excl9, ta9);
            return 1;
          }
          if (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last"))) {
            int ta9 = ++g_tmp, tn9 = ++g_tmp;
            buf_printf(b, "({ sp_StrArray *_t%d = sp_StrArray_from_string_range(", ta9);
            emit_expr(c, lo9, b); buf_puts(b, ", "); emit_expr(c, hi9, b);
            buf_printf(b, ", %d); sp_int _t%d = ", excl9, tn9); emit_int_expr(c, argv[0], b);
            if (sp_streq(name, "first"))
              buf_printf(b, "; sp_StrArray_slice(_t%d, 0, _t%d); })", ta9, tn9);
            else
              buf_printf(b, "; sp_int _s9 = _t%d->len - _t%d; if (_s9 < 0) _s9 = 0;"
                            " sp_StrArray_slice(_t%d, _s9, _t%d); })", ta9, tn9, ta9, tn9);
            return 1;
          }
        }
      }
      /* an unbounded end has no maximum and an unbounded begin has no minimum
         -- CRuby raises RangeError rather than reading the infinity sentinel
         stored in the endpoint (#3065) */
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") && argc == 0) {
        int lo9 = nt_ref(nt, rn9, "left"), hi9 = nt_ref(nt, rn9, "right");
        int endless9 = (hi9 < 0 || lazy_endpoint_is_infinite(c, hi9));
        if (endless9 && lo9 >= 0 && sp_streq(name, "max")) {
          buf_puts(b, "({ sp_raise_cls(\"RangeError\", \"cannot get the maximum of endless range\"); (sp_int)0; })");
          return 1;
        }
        if (lo9 < 0 && sp_streq(name, "min")) {
          buf_puts(b, "({ sp_raise_cls(\"RangeError\", \"cannot get the minimum of beginless range\"); (sp_int)0; })");
          return 1;
        }
      }
      if (rn9 >= 0 && nt_type(nt, rn9) && sp_streq(nt_type(nt, rn9), "RangeNode") &&
          (nt_ref(nt, rn9, "right") < 0 ||
           lazy_endpoint_is_infinite(c, nt_ref(nt, rn9, "right"))) &&
          nt_ref(nt, rn9, "left") >= 0) {
        /* #count enumerates forever on an endless range, so like #size it
           answers Infinity (#3668) */
        if ((sp_streq(name, "size") || sp_streq(name, "count")) && argc == 0 &&
            nt_ref(nt, id, "block") < 0) {
          buf_puts(b, "(HUGE_VAL)");
          return 1;
        }
        if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) {
          int lo9 = nt_ref(nt, rn9, "left");
          int ts9 = ++g_tmp, tn9 = ++g_tmp, ti9 = ++g_tmp, to9 = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", ts9); emit_int_expr(c, lo9, b);
          buf_printf(b, "; sp_int _t%d = ", tn9); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);"
                        " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++)"
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
      /* `x..Float::INFINITY`: the int range records only "unbounded", so the
         rendering comes from the literal that named the bound (#3670) */
      int hi_is_inf = hi_q >= 0 && nt_kind(nt, hi_q) == NK_ConstantPathNode &&
                      nt_str(nt, hi_q, "name") && sp_streq(nt_str(nt, hi_q, "name"), "INFINITY");
      if (!str_ends && lo_q >= 0 && hi_is_inf) {
        int tq2 = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tq2);
        emit_expr(c, recv, b);
        buf_printf(b, "; sp_sprintf(\"%%lld%%sInfinity\", (long long)_t%d.first,"
                      " _t%d.excl ? \"...\" : \"..\"); })", tq2, tq2);
        return 1;
      }
      if (!str_ends) {
        int tq = ++g_tmp;
        buf_printf(b, "({ sp_Range _t%d = ", tq);
        emit_expr(c, recv, b);
        /* #inspect names the absent bounds of a fully-unbounded range
           ("nil..nil"), where #to_s prints only the dots (#3670) */
        buf_printf(b, "; %s(_t%d); })",
                   sp_streq(name, "inspect") ? "sp_range_inspect" : "sp_range_str", tq);
        return 1;
      }
    }
    /* min(n) / max(n): the n smallest or largest members, walked from the
       endpoint the count starts at, so a one-sided Range answers without
       materializing (and raises from the side it has no end on) (#3665). */
    if (argc == 1 && (sp_streq(name, "min") || sp_streq(name, "max")) && block < 0) {
      int trr = ++g_tmp, tnn = ++g_tmp, too = ++g_tmp, thi = ++g_tmp, tii = ++g_tmp;
      int want_min = sp_streq(name, "min");
      buf_printf(b, "({ sp_Range _t%d = ", trr); emit_expr(c, recv, b);
      buf_printf(b, "; sp_int _t%d = ", tnn); emit_int_expr(c, argv[0], b);
      buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tnn);
      if (want_min)
        buf_printf(b, " if (_t%d.first == INTPTR_MIN) sp_raise_cls(\"RangeError\","
                      " \"cannot get the minimum of beginless range\");", trr);
      else
        buf_printf(b, " if (_t%d.last == INTPTR_MAX) sp_raise_cls(\"RangeError\","
                      " \"cannot get the maximum of endless range\");", trr);
      buf_printf(b, " sp_int _t%d = _t%d.last - (_t%d.excl ? 1 : 0);", thi, trr, trr);
      buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);", too, too);
      if (want_min)
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                      " sp_int _v = _t%d.first + _t%d;"
                      " if (_t%d.last != INTPTR_MAX && _v > _t%d) break;"
                      " sp_IntArray_push(_t%d, _v); }",
                   tii, tii, tnn, tii, trr, tii, trr, thi, too);
      else
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < _t%d; _t%d++) {"
                      " sp_int _v = _t%d - _t%d;"
                      " if (_t%d.first != INTPTR_MIN && _v < _t%d.first) break;"
                      " sp_IntArray_push(_t%d, _v); }",
                   tii, tii, tnn, tii, thi, tii, trr, trr, too);
      buf_printf(b, " _t%d; })", too);
      return 1;
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
          /* sp_range_include takes sp_int; a float arg (`(1..).include?(2.4)`)
             needs an explicit cast, else clang -Werror flags the implicit
             float-literal->int conversion (gcc truncates silently). A poly arg
             (e.g. under --int-overflow=promote) is coerced with sp_poly_to_i. */
          TyKind at0 = comp_ntype(c, argv[0]);
          int arg_is_float = at0 == TY_FLOAT;
          int arg_is_poly = at0 == TY_POLY;
          if (value_obj_compares(c, argv[0])) unsupported_feature(c, id, "Range#include? of a user object defining <=>");
          if (value_kind_misses(c, argv[0], TY_INT)) {
            /* an Integer compares with nothing of this class: not covered */
            buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
          }
          else {
            buf_printf(b, "sp_range_include(&_t%d, ", t);
            if (arg_is_float) buf_puts(b, "(sp_int)(");
            if (arg_is_poly) buf_puts(b, "sp_poly_to_i(");
            emit_expr(c, argv[0], b);
            if (arg_is_poly) buf_puts(b, ")");
            if (arg_is_float) buf_puts(b, ")");
            buf_puts(b, ")");
          }
        }
      }
      else if (sp_streq(name, "min"))  /* smallest enumerated element (direction-aware) */
        buf_printf(b, "sp_range_min_v(_t%d)", t);
      else if (sp_streq(name, "first") || sp_streq(name, "begin")) {
        /* #first enumerates, so a beginless range has none (#3668) */
        if (argc == 0 && sp_streq(name, "first"))
          buf_printf(b, "({ if (_t%d.first == INTPTR_MIN) sp_raise_cls(\"RangeError\","
                        " \"cannot get the first element of beginless range\"); _t%d.first; })", t, t);
        else if (argc == 1) {
          /* first(n): the first n elements from `first`, walking by step. */
          int tf = ++g_tmp, tn = ++g_tmp, ti = ++g_tmp, tc = ++g_tmp;
          buf_printf(b, "({ sp_IntArray *_t%d = sp_IntArray_new(); sp_int _t%d = ", tf, tn);
          emit_int_expr(c, argv[0], b);   /* first(nil) is CRuby's TypeError */
          buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
          buf_printf(b, " sp_int _t%d = sp_range_count(_t%d); sp_int _t%d = sp_range_step(_t%d);"
                        " for (sp_int _i%d = 0; _i%d < _t%d && _i%d < _t%d; _i%d++)"
                        " sp_IntArray_push(_t%d, _t%d.first + _i%d * _t%d); _t%d; })",
                     tc, t, ti, t, tf, tf, tn, tf, tc, tf, tf, t, tf, ti, tf);
        }
        else buf_printf(b, "(_t%d.first)", t);
      }
      else if (sp_streq(name, "max"))  /* largest enumerated element (direction-aware) */
        buf_printf(b, "sp_range_max_v(_t%d)", t);
      else if (sp_streq(name, "end") && argc == 0 && comp_ntype(c, id) == TY_FLOAT) {
        /* `x..Float::INFINITY`: the literal named the bound the int range can
           only record as "unbounded" -- answer the Float itself (#3670) */
        buf_puts(b, "HUGE_VAL"); (void)t;
      }
      else if (sp_streq(name, "end") && ({ int _rr = unwrap_parens(c, recv);
               nt_type(nt, _rr) && sp_streq(nt_type(nt, _rr), "RangeNode") &&
               nt_ref(nt, _rr, "right") < 0; })) {
        /* an ENDLESS literal range: #end is nil (#2413) */
        buf_puts(b, "sp_box_nil()"); (void)t;
      }
      else if (argc == 0 && sp_streq(name, "end")) {
        /* #end is nil for ANY endless range, however it was spelled: `1..nil`
           and a range held in a variable read the sentinel, where the
           literal-shape arm above sees no syntax to key on (#3670) */
        buf_printf(b, "(_t%d.last == INTPTR_MAX ? SP_INT_NIL : _t%d.last)", t, t);
      }
      else if (sp_streq(name, "last") || sp_streq(name, "end")) {
        /* #last enumerates, so an endless range has none (#3668) */
        if (argc == 0 && sp_streq(name, "last"))
          buf_printf(b, "({ if (_t%d.last == INTPTR_MAX) sp_raise_cls(\"RangeError\","
                        " \"cannot get the last element of endless range\"); _t%d.last; })", t, t);
        else if (argc == 1 && sp_streq(name, "last")) {
          /* last(n): collect up to n elements ending at last */
          int tf = ++g_tmp, tn = ++g_tmp, ts = ++g_tmp, te = ++g_tmp;
          buf_printf(b, "({ sp_int _t%d = ", tn); emit_int_expr(c, argv[0], b);
          buf_printf(b, "; if (_t%d < 0) sp_raise_cls(\"ArgumentError\", \"negative array size\");", tn);
          /* an endless range has no last n elements to walk back from: the
             loop counted down from INTPTR_MAX and allocated until the process
             died, where CRuby raises (#3861) */
          buf_printf(b, " if (_t%d.last == INTPTR_MAX) sp_raise_cls(\"RangeError\","
                        " \"cannot get the last element of endless range\");", t);
          buf_printf(b, " sp_int _t%d = _t%d.last - _t%d.excl;", te, t, t);
          buf_printf(b, " sp_int _t%d = _t%d - _t%d + 1; if (_t%d < _t%d.first) _t%d = _t%d.first;", ts, te, tn, ts, t, ts, t);
          buf_printf(b, " sp_IntArray *_t%d = sp_IntArray_new(); for (sp_int _i%d = _t%d; _i%d <= _t%d; _i%d++)"
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
      /* Range#sum takes an INTEGER seed exactly and runs every other class
         through Kernel#Float, answering a Float -- so a Bignum seed wrapped
         into the sp_int slot here, a Rational one did not compile, and nil and
         a String reported the wrong conversion. An empty range answers the
         seed untouched, which is why the helper decides at run time. */
      else if (sp_streq(name, "sum") && argc == 1 &&
               !fold_seed_typed(fold_seed_ntype(c, argv[0]), TY_INT)) {
        buf_printf(b, "sp_range_sum_seed(_t%d, ", t);
        emit_boxed(c, argv[0], b);
        buf_puts(b, ")");
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
        if (argc == 1 && comp_ntype(c, argv[0]) != TY_RANGE && comp_ntype(c, argv[0]) != TY_POLY &&
            comp_ntype(c, argv[0]) != TY_UNKNOWN) {
          /* a value of another class is never eql? to a Range */
          buf_puts(b, "({ (void)("); emit_expr(c, argv[0], b); buf_puts(b, "); 0; })");
        }
        else { buf_printf(b, "sp_range_eq(_t%d, ", t); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
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
                      " sp_int _t%d = sp_range_min_v(_t%d);"
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
  /* A boxed index is addressable too: emit_int_expr converts it. Requiring
     TY_INT here dropped `rows[r][c] = "*"` when `r` came from a destructured
     block parameter -- the store fell to the by-value form, which writes into
     a copy of the element and loses the assignment (#4078). */
  if (rargc != 1) return 0;
  { TyKind it = comp_ntype(c, rargv[0]);
    /* The slot helpers address the outer by INTEGER index, so a boxed index is
       only usable when the outer really is an array -- a boxed Hash KEY would
       be converted to an int and refused. Requiring TY_INT outright dropped
       `rows[r][c] = "*"` when `r` came from a destructured block parameter:
       the store fell to the by-value form, which writes into a copy of the
       element and loses the assignment (#4078). */
    if (it != TY_INT) {
      if (it != TY_POLY) return 0;
      TyKind ot = comp_ntype(c, ro);
      if (!ty_is_array(ot)) return 0;
    } }
  *outer = ro; *oidx = rargv[0];
  return 1;
}

/* Is the receiver a variable? A String mutator's new contents go back into
   one; a receiver that is no variable -- an element read, a Hash value -- can
   take them only through a shared handle, and only from a mutator whose value
   is those contents (PF_VAL_SELF): the typed emitter leaves them in the
   receiver's temp for a variable alone. */
static int face_str_var_recv(const NodeTable *nt, int recv) {
  const char *rvt = nt_type(nt, recv);
  return rvt && (sp_streq(rvt, "LocalVariableReadNode") || sp_streq(rvt, "InstanceVariableReadNode"));
}
/* One owner's arm: unbox `box` (a temp holding the boxed receiver, or 0 to
   unbox the receiver expression itself) to `kind`'s representation in the
   statement prelude, override the receiver node with the temp, retype and
   pin it, and re-enter the same call so the typed emitter takes it from
   there. A receiver that is not of that kind at run time raises the
   NoMethodError the call would have raised, from the coercion. Answers the
   arm's value text in `val` and its type under the pin -- the node's settled
   type is the union over the inference passes and the owners, and may be
   poly where the arm answers a pointer. */
static TyKind emit_face_arm(Compiler *c, int id, unsigned kind, unsigned flags, int box, Buf *val) {
  const NodeTable *nt = c->nt;
  /* The re-entered emitter may rename the node for its own re-entry (a
     String bang takes its plain form) and restore it into fresh storage, so
     the name is kept here, not borrowed from the node table. Only a table
     row's name arrives, so the buffer cannot truncate. */
  char name[128];
  snprintf(name, sizeof name, "%s", nt_str(nt, id, "name"));
  int recv = nt_ref(nt, id, "receiver");
  int has_blk = nt_ref(nt, id, "block") >= 0;
  int t = ++g_tmp;
  char bx[32];
  Buf rb; memset(&rb, 0, sizeof rb);
  if (box) snprintf(bx, sizeof bx, "_t%d", box);
  else {
    /* the elements of a collection materialize from the box itself */
    if (kind == PF_ENUM) emit_boxed(c, recv, &rb);
    else emit_expr(c, recv, &rb);
  }
  const char *rs = box ? bx : rb.p ? rb.p : "sp_box_nil()";
  emit_indent(g_pre, g_indent);
  switch (kind) {
    case PF_STRING:
      buf_printf(g_pre, "const char *_t%d = sp_poly_recv_s(%s, \"%s\"); SP_GC_ROOT(_t%d);\n", t, rs, name, t);
      break;
    case PF_INT:
      /* The block iterators check: `"x".times { }` is a NoMethodError in
         CRuby, and coercing would silently run the loop zero times. The
         blockless names keep the plain coercion they have always used. */
      if (has_blk) buf_printf(g_pre, "sp_int _t%d = sp_poly_int_recv(%s, \"%s\");\n", t, rs, name);
      else buf_printf(g_pre, "sp_int _t%d = sp_poly_to_i(%s);\n", t, rs);
      break;
    /* A mutator's coercion checks the original for frozenness first: the
       typed emitter would otherwise work on the copy, running a block over
       every element, and only the write-back would raise. */
    case PF_ARRAY:
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_array_recv(%s, \"%s\", %d); SP_GC_ROOT(_t%d);\n",
                 t, rs, name, (flags & PF_MUT) != 0, t);
      break;
    case PF_ENUM:
      /* a hash gives its [key, value] pairs; a receiver that is no collection
         raises the NoMethodError the call raised before */
      buf_printf(g_pre, "sp_PolyArray *_t%d = sp_poly_enum_recv(%s, \"%s\"); SP_GC_ROOT(_t%d);\n", t, rs, name, t);
      break;
    case PF_HASH:
      buf_printf(g_pre, "sp_PolyPolyHash *_t%d = sp_poly_hash_recv(%s, \"%s\", %d); SP_GC_ROOT(_t%d);\n",
                 t, rs, name, (flags & PF_MUT) != 0, t);
      break;
  }
  free(rb.p);
  g_argov_node[g_n_argov] = recv;
  snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", t);
  g_n_argov++;
  TyKind as = ty_poly_face_kind(kind);
  TyKind sv = c->ntype[recv]; c->ntype[recv] = as;
  int sv_face = an_face_node(); TyKind sv_fk = an_face_kind();
  an_set_face_node(recv, as);
  TyKind nat = infer_uncached(c, id);
  Buf cb; memset(&cb, 0, sizeof cb);
  emit_call(c, id, &cb);
  an_set_face_node(sv_face, sv_fk);
  c->ntype[recv] = sv;
  g_n_argov--;
  const char *call = cb.p ? cb.p : "0";
  /* A typed emitter that declines the call's argument shape answers the
     unresolved gate's raise token, a poly value that never returns: hand it
     on as poly, untouched, rather than bind it into a typed temp. */
  if (strncmp(call, "sp_raise_nomethod(", 18) == 0) {
    TyKind slot = comp_ntype(c, id);
    if (slot == TY_POLY || slot == TY_UNKNOWN || slot == TY_VOID) { buf_puts(val, call); slot = TY_POLY; }
    else emit_unbox_text(c, slot, call, val);   /* the token is an sp_RbVal; the slot is not */
    free(cb.p);
    return slot;
  }
  /* A mutator worked on the unboxed representation -- the poly copy a typed
     array was normalized to, the text a string box stands for -- and the
     original has to take the result back once the value is taken. */
  if ((flags & PF_MUT) && box && (kind == PF_ARRAY || kind == PF_STRING || kind == PF_HASH)) {
    Buf wb; memset(&wb, 0, sizeof wb);
    int tr = ++g_tmp;
    int has_val = nat != TY_VOID && nat != TY_UNKNOWN;
    if (kind == PF_ARRAY) buf_printf(&wb, "sp_poly_arr_writeback(_t%d, _t%d)", box, t);
    else if (kind == PF_HASH) buf_printf(&wb, "sp_poly_hash_writeback(_t%d, _t%d)", box, t);
    else {
      /* The new contents -- the value itself when the mutator answers self,
         else the temp the typed emitter took the receiver's variable from and
         wrote to -- go back into the receiver's variable: a shared handle
         absorbs them, so a container the value came from observes the change;
         a plain string box is replaced, the way the typed path replaces its
         own. A receiver that is no variable (face_str_var_recv) has a handle
         to absorb them or nowhere to send them, and then raises what the call
         raised before the row existed, rather than a mutation that silently
         goes nowhere. Contents that are the receiver's own mean no write at
         all when the row says so (scrub!). */
      int var = face_str_var_recv(nt, recv);
      int nv = ((flags & PF_VAL_SELF) && has_val) ? tr : t;
      if (var) {
        emit_expr(c, recv, &wb); buf_puts(&wb, " = ");
        if (flags & PF_SAME_OK) buf_printf(&wb, "sp_poly_str_is_own(_t%d, _t%d) ? _t%d : ", box, nv, box);
        buf_printf(&wb, "sp_poly_str_become(_t%d, _t%d)", box, nv);
      }
      else {
        if (flags & PF_SAME_OK) buf_printf(&wb, "if (!sp_poly_str_is_own(_t%d, _t%d)) ", box, nv);
        buf_printf(&wb, "sp_poly_str_become_handle(_t%d, _t%d, \"%s\")", box, nv, name);
      }
    }
    if (!has_val) buf_printf(val, "({ (void)(%s); %s; })", call, wb.p);
    else if (kind == PF_HASH && (flags & PF_VAL_SELF)) {
      /* The value is the receiver -- the box -- not the general copy the
         emitter worked on: a typed original has no general stand-in, and the
         copy is detached once written back, so a write through the value
         (h.merge!(a)[:k] = v, and the chain h.merge!(a, b) folds into) would
         go nowhere. compact! answers nil when it removed nothing, and the
         receiver else. */
      if (nat == TY_POLY) buf_printf(val, "({ sp_RbVal _t%d = %s; %s; sp_poly_nil_p(_t%d) ? _t%d : _t%d; })", tr, call, wb.p, tr, tr, box);
      else buf_printf(val, "({ (void)(%s); %s; _t%d; })", call, wb.p, box);
      nat = TY_POLY;
    }
    else buf_printf(val, "({ %s _t%d = %s; %s; _t%d; })", c_type_name(nat), tr, call, wb.p, tr);
    free(wb.p);
  }
  else buf_puts(val, call);
  free(cb.p);
  return nat;
}

/* The value of an arm in the call's result slot: as it is when the two
   types agree, boxed into a poly slot otherwise. */
static void emit_face_value(Compiler *c, TyKind slot, TyKind nat, const char *val, Buf *b) {
  /* a poly answer under the pin is not a boxed value: the Integer iterators
     answer their receiver's sp_int while the pinned inference says poly */
  if (slot == nat || nat == TY_POLY || nat == TY_UNKNOWN || nat == TY_VOID) buf_puts(b, val);
  else if (slot == TY_POLY) emit_boxed_text(c, nat, val, b);
  else {
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, nat, val, &bx);
    emit_unbox_text(c, slot, bx.p, b);
    free(bx.p);
  }
}

/* An arm under the silent emittability probe the dynamic-send dispatch
   uses: a typed emitter that declines the call longjmps out of emit, and
   the arm is dropped rather than the build. Everything the arm may have
   changed on the way out is put back -- the receiver's type override and
   the inference pin emit_face_arm restores only on its normal return, the
   argument overrides, the conversion hold, the prelude, and the recovery
   point itself, which the driver armed for the whole unit. The arm's
   prelude is captured to `pre` and its value to `val`; answers 0 when the
   arm was dropped. */
static int face_probe_arm(Compiler *c, int id, unsigned kind, unsigned flags, int box,
                          Buf *pre, Buf *val, TyKind *nat) {
  int recv = nt_ref(c->nt, id, "receiver");
  Buf *sv_pre = g_pre;
  int sv_probe = g_unsup_probe;
  ConvHold *sv_hold = g_conv_hold;
  int sv_argov = g_n_argov;
  TyKind sv_ty = c->ntype[recv];
  int sv_face = an_face_node(); TyKind sv_fk = an_face_kind();
  jmp_buf sv_jb; memcpy(sv_jb, g_unsup_recover, sizeof(jmp_buf));
  volatile int ok = 1;
  g_pre = pre; g_unsup_probe = 1;
  if (setjmp(g_unsup_recover) == 0) *nat = emit_face_arm(c, id, kind, flags, box, val);
  else ok = 0;
  memcpy(g_unsup_recover, sv_jb, sizeof(jmp_buf));
  an_set_face_node(sv_face, sv_fk);
  c->ntype[recv] = sv_ty;
  g_n_argov = sv_argov;
  g_conv_hold = sv_hold;
  g_unsup_probe = sv_probe;
  g_pre = sv_pre;
  return ok;
}

/* One owner: exactly the re-entry, with the box kept only when a mutator
   has to write back through it. Answers 0 when the typed emitter declined
   the call, and the call goes on to the arms after this one. */
static int emit_face_reentry(Compiler *c, int id, unsigned kind, unsigned flags, Buf *b) {
  const NodeTable *nt = c->nt;
  int recv = nt_ref(nt, id, "receiver");
  int box = 0;
  Buf pre = {0, 0, 0}, val = {0, 0, 0};
  TyKind nat = TY_UNKNOWN;
  if ((flags & PF_MUT) && kind == PF_STRING && !(flags & PF_VAL_SELF) && !face_str_var_recv(nt, recv)) return 0;
  if ((flags & PF_MUT) && (kind == PF_ARRAY || kind == PF_STRING || kind == PF_HASH)) box = ++g_tmp;
  if (!face_probe_arm(c, id, kind, flags, box, &pre, &val, &nat)) {
    free(pre.p); free(val.p);
    return 0;
  }
  if (box) {
    Buf rb; memset(&rb, 0, sizeof rb); emit_expr(c, recv, &rb);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", box, rb.p ? rb.p : "sp_box_nil()", box);
    free(rb.p);
  }
  if (pre.p) buf_puts(g_pre, pre.p);
  emit_face_value(c, comp_ntype(c, id), nat, val.p ? val.p : "0", b);
  free(pre.p); free(val.p);
  return 1;
}

/* The run-time test that the boxed value in temp `t` is of an owner's kind. */
static void emit_face_kind_test(unsigned kind, int t, Buf *b) {
  switch (kind) {
    case PF_STRING: buf_printf(b, "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d))", t, t); break;
    case PF_INT:    buf_printf(b, "(_t%d.tag == SP_TAG_INT)", t); break;
    case PF_ARRAY:  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id))", t, t); break;
    case PF_HASH:   buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id))", t, t); break;
    case PF_ENUM:   buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && (sp_poly_is_array_kind(_t%d.cls_id) || sp_poly_is_hash_kind(_t%d.cls_id)))", t, t, t); break;
    default:        buf_puts(b, "(0)"); break;
  }
}

/* Has the inference typed the argument as some other kind than the owner's
   own? A poly or unknown one may still be of the owner's kind at run time;
   any other kind cannot be, whether or not it has a class name of its own (a
   Boolean is true or false only at run time), so the noun CRuby's TypeError
   names is spelled from the value when the arm runs. */
static int face_arg_misfit(Compiler *c, unsigned kind, int arg) {
  TyKind at = comp_ntype(c, arg);
  if (at == TY_POLY || at == TY_UNKNOWN) return 0;
  if (kind == PF_STRING && (at == TY_STRING || at == TY_STRBUF || at == TY_INT)) return 0;  /* a codepoint concatenates too */
  if (kind == PF_ARRAY && (ty_is_array(at) || at == TY_POLY_ARRAY)) return 0;
  return 1;
}

/* Several owners: bind the box once and dispatch on its run-time kind, one
   re-entry per owner, each with its own coercion and prelude inside its own
   branch. An arm whose typed emitter declines the call is dropped, under the
   silent probe the dynamic-send dispatch uses; an arm ruled out by an
   argument's type raises CRuby's TypeError; a receiver of no owner's kind
   raises the NoMethodError the call raised before. Answers 0 when no arm
   survives, and the call falls through to the arms after this one. */
static int emit_face_switch(Compiler *c, int id, unsigned own, Buf *b) {
  const NodeTable *nt = c->nt;
  char name[128];   /* kept, not borrowed: an arm's re-entry may rename the node (see emit_face_arm) */
  snprintf(name, sizeof name, "%s", nt_str(nt, id, "name"));
  int recv = nt_ref(nt, id, "receiver");
  int argc;
  const int *argv = call_args(nt, id, &argc);
  int has_blk = nt_ref(nt, id, "block") >= 0;
  TyKind slot = comp_ntype(c, id);
  if (slot == TY_UNKNOWN || slot == TY_VOID) slot = TY_POLY;
  int plain = nt_call_args_plain(nt, id);
  Buf arms; memset(&arms, 0, sizeof arms);
  int narm = 0;
  int box = ++g_tmp, tr = ++g_tmp;
  for (unsigned kind = 1; kind & PF_OWNERS; kind <<= 1) {
    if (!(own & kind)) continue;
    unsigned fl = ty_poly_face_owner_flags(name, argc, has_blk, plain, kind);
    if ((fl & PF_MUT) && kind == PF_STRING && !(fl & PF_VAL_SELF) && !face_str_var_recv(nt, recv)) continue;
    int misfit = -1;
    if ((fl & PF_ARGS_OWN) && plain)
      for (int i = 0; i < argc && misfit < 0; i++) if (face_arg_misfit(c, kind, argv[i])) misfit = i;
    Buf pre = {0, 0, 0}, val = {0, 0, 0};
    TyKind nat = TY_UNKNOWN;
    int ok = 1;
    if (misfit >= 0) {
      /* the arguments are evaluated for their effects and in order, as the
         typed arm would, and under the arm's own prelude, so an argument
         that needs one runs it in this branch alone; the one that cannot
         convert is kept to name itself (nil, true and false spell themselves,
         an object its class) */
      Buf *sv_pre = g_pre; g_pre = &pre;
      int tm = ++g_tmp;
      buf_printf(&val, "sp_RbVal _t%d = sp_box_nil(); SP_GC_ROOT_RBVAL(_t%d); (void)(", tm, tm);
      for (int i = 0; i < argc; i++) {
        if (i) buf_puts(&val, ", ");
        if (i == misfit) buf_printf(&val, "(_t%d = ", tm);
        emit_boxed(c, argv[i], &val);
        if (i == misfit) buf_puts(&val, ")");
      }
      buf_printf(&val, "); sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into %s\", sp_convert_src_name(_t%d)))",
                 kind == PF_STRING ? "String" : "Array", tm);
      g_pre = sv_pre;
    }
    else ok = face_probe_arm(c, id, kind, fl, box, &pre, &val, &nat);
    if (!ok) { free(pre.p); free(val.p); continue; }
    if (narm) buf_puts(&arms, "}\nelse ");
    buf_puts(&arms, "if ");
    emit_face_kind_test(kind, box, &arms);
    buf_puts(&arms, " { ");
    if (pre.p) buf_puts(&arms, pre.p);
    if (misfit >= 0) buf_printf(&arms, "%s;", val.p);
    else {
      buf_printf(&arms, "_t%d = ", tr);
      emit_face_value(c, slot, nat, val.p ? val.p : "0", &arms);
      buf_puts(&arms, ";");
    }
    buf_puts(&arms, " ");
    narm++;
    free(pre.p); free(val.p);
  }
  if (!narm) { free(arms.p); return 0; }
  /* The box's declaration goes to the prelude only now: the arms name it,
     but whether any survived to need it is known only after they are built. */
  Buf rb; memset(&rb, 0, sizeof rb); emit_boxed(c, recv, &rb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n", box, rb.p ? rb.p : "sp_box_nil()", box);
  free(rb.p);
  buf_printf(b, "({ %s _t%d = %s; ", c_type_name(slot), tr, default_value(slot));
  buf_puts(b, arms.p);
  buf_printf(b, "}\nelse sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); _t%d; })", name, box, tr);
  free(arms.p);
  return 1;
}

/* A String value-form mutator on a boxed receiver: compute the non-bang
   transform against the unboxed contents, then write the result back
   through the box. */
static void emit_face_str_bang(Compiler *c, int id, unsigned own, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int nil_nc = !(own & PF_STR_SELF);
  /* The node's name is rewritten to the plain form for the re-entry, so
     both spellings live here, not in the node table. */
  char bang[64], plain[64];   /* a table row's bang name: never empty, never near the cap */
  snprintf(bang, sizeof bang, "%s", name);
  snprintf(plain, sizeof plain, "%.*s", (int)strlen(name) - 1, name);
  int tvb = ++g_tmp, tob = ++g_tmp, tnb = ++g_tmp;
  Buf rbb; memset(&rbb, 0, sizeof rbb); emit_expr(c, recv, &rbb);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);"
                    " const char *_t%d = sp_poly_to_s(_t%d); SP_GC_ROOT(_t%d);\n",
             tvb, rbb.p ? rbb.p : "sp_box_nil()", tvb, tob, tvb, tob);
  free(rbb.p);
  g_argov_node[g_n_argov] = recv;
  snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", tob);
  g_n_argov++;
  TyKind svb = c->ntype[recv]; c->ntype[recv] = TY_STRING;
  nt_node_set_str((NodeTable *)nt, id, "name", plain);
  Buf nbb; memset(&nbb, 0, sizeof nbb); emit_call(c, id, &nbb);
  nt_node_set_str((NodeTable *)nt, id, "name", bang);
  c->ntype[recv] = svb;
  g_n_argov--;
  buf_printf(b, "({ const char *_t%d = %s; ", tnb, nbb.p ? nbb.p : "\"\"");
  free(nbb.p);
  /* Decide "did it change?" BEFORE the mutation. The receiver's old text
     is the LIVE payload of a shared handle, so once become() has written
     the new contents into it the two compare equal and the bang method
     answered nil after a substitution that plainly happened (#4042). */
  int tchg = 0;
  if (nil_nc) {
    tchg = ++g_tmp;
    buf_printf(b, "int _t%d = !sp_str_eq(_t%d, _t%d); ", tchg, tob, tnb);
  }
  /* A shared handle absorbs the new contents; a plain string box cannot,
     so an lvalue receiver takes the value back the way the typed path
     does for the same case. */
  { const char *rvtb = nt_type(nt, recv);
    if (rvtb && (sp_streq(rvtb, "LocalVariableReadNode") ||
                 sp_streq(rvtb, "InstanceVariableReadNode"))) {
      emit_expr(c, recv, b);
      buf_printf(b, " = sp_poly_str_become(_t%d, _t%d); ", tvb, tnb);
    }
    else buf_printf(b, "sp_poly_str_become(_t%d, _t%d); ", tvb, tnb);
  }
  if (nil_nc) buf_printf(b, "_t%d ? _t%d : NULL; })", tchg, tnb);
  else buf_printf(b, "_t%d; })", tnb);
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
  /* #slice on a boxed receiver is two different methods: Hash#slice(*keys)
     answers a sub-Hash, while String#slice / Array#slice is exactly #[]. Only
     the runtime value tells them apart, so branch on it and hand the non-hash
     side to the boxed `[]` dispatch through a rename re-entry, like the
     typed-array slice above (#3445, #3449). */
  if (recv >= 0 && rt == TY_POLY && argc >= 1 && sp_streq(name, "slice") &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, "slice") &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int tsv = ++g_tmp;
    int has_splat = 0;
    for (int i = 0; i < argc; i++)
      if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) has_splat = 1;
    buf_printf(b, "({ sp_RbVal _t%d = ", tsv); emit_boxed(c, recv, b);
    buf_puts(b, "; ");
    int tkeys = -1;
    if (has_splat) {
      /* A splat contributes all of its elements, so the key list has a length
         only the run time knows: build it as a PolyArray and hand the callee
         its buffer. The fixed `(sp_RbVal[]){...}` below cannot express that --
         it passed the whole array as ONE key, and as the wrong C type at that,
         since the splat expression is an unboxed sp_PolyArray * (#4164). */
      tkeys = ++g_tmp;
      buf_printf(b, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tkeys, tkeys);
      for (int i = 0; i < argc; i++) {
        if (nt_type(nt, argv[i]) && sp_streq(nt_type(nt, argv[i]), "SplatNode")) {
          int sx = nt_ref(nt, argv[i], "expression");
          int tss = ++g_tmp, tsi = ++g_tmp;
          buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tss);
          if (sx >= 0) emit_boxed(c, sx, b); else buf_puts(b, "sp_box_nil()");
          buf_printf(b, "); SP_GC_ROOT(_t%d);", tss);
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                        " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                     tsi, tsi, tss, tsi, tkeys, tss, tsi);
          continue;
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, ", tkeys); emit_boxed(c, argv[i], b);
        buf_puts(b, ");");
      }
      buf_puts(b, " ");
    }
    buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id))", tsv, tsv);
    if (has_splat)
      buf_printf(b, " ? sp_poly_hash_slice(_t%d, (int)_t%d->len, _t%d->data) : ", tsv, tkeys, tkeys);
    else {
      buf_printf(b, " ? sp_poly_hash_slice(_t%d, %d, (sp_RbVal[]){", tsv, argc);
      for (int i = 0; i < argc; i++) { if (i) buf_puts(b, ", "); emit_boxed(c, argv[i], b); }
      buf_puts(b, "}) : ");
    }
    if (has_splat) {
      /* The non-hash side is String#slice / Array#slice, which is exactly #[]
         and takes one argument or two -- and with a splat only the run time
         knows which. Branch on the key list's length; any other length is the
         ArgumentError CRuby raises. */
      buf_printf(b, "(_t%d->len == 1 ? sp_poly_index_poly(_t%d, _t%d->data[0])"
                    " : _t%d->len == 2"
                    " ? sp_poly_slice(_t%d, sp_poly_to_i(_t%d->data[0]), sp_poly_to_i(_t%d->data[1]))"
                    " : (sp_raise_cls(\"ArgumentError\", \"wrong number of arguments\"), sp_box_nil()))",
                 tkeys, tsv, tkeys, tkeys, tsv, tkeys, tkeys);
    }
    else if (argc == 1 || argc == 2) {
      g_argov_node[g_n_argov] = recv;
      snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", tsv);
      g_n_argov++;
      nt_node_set_str((NodeTable *)nt, id, "name", "[]");
      Buf ib; memset(&ib, 0, sizeof ib); emit_call(c, id, &ib);
      nt_node_set_str((NodeTable *)nt, id, "name", "slice");
      g_n_argov--;
      buf_puts(b, ib.p ? ib.p : "sp_box_nil()");
      free(ib.p);
    }
    else buf_printf(b, "sp_raise_nomethod(sp_nomethod_msg(\"slice\", _t%d))", tsv);
    buf_puts(b, "; })");
    return 1;
  }
  /* The face table (types.h): unbox the receiver to the kind that owns the
     name, retype the receiver node and re-enter the same call, so the typed
     emitter IS the implementation and the inference, which answered under
     the same pin, has already sized the result slot to it. The last-resort
     rows are not taken here: the Hash face answers in emit_unresolved_call,
     once every poly-receiver emitter of its own has declined the name. */
  if (recv >= 0 && rt == TY_POLY && !user_defines_or_reads(c, name) &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int has_blk = nt_ref(nt, id, "block") >= 0;
    unsigned own = ty_poly_face_owners(name, argc, has_blk, nt_call_args_plain(nt, id), 0);
    unsigned kinds = own & PF_OWNERS;
    if (own & PF_STR_BANG) { emit_face_str_bang(c, id, own, b); return 1; }
    if (kinds && !(kinds & (kinds - 1)) && emit_face_reentry(c, id, kinds, own, b)) return 1;
    if (kinds && (kinds & (kinds - 1)) && emit_face_switch(c, id, kinds, b)) return 1;
    /* a declined re-entry may have renamed the node and restored it into
       fresh storage (see emit_face_arm): the name is read again */
    name = nt_str(nt, id, "name");
  }
  /* The one/two-String-argument transforms on a boxed receiver: a String
     arriving through a poly slot (a Fiber#resume value, a container read) had
     no arm for these and raised NoMethodError naming String, which is what it
     was. A regexp pattern keeps the dedicated regexp emitters. */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name)) {
    if (sp_streq(name, "squeeze") && argc == 1) {
      buf_puts(b, "sp_str_squeeze_chars(sp_poly_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
      return 1;
    }
    if (sp_streq(name, "tr") && argc == 2) {
      buf_puts(b, "sp_str_tr(sp_poly_to_s("); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_str_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
    if ((sp_streq(name, "sub") || sp_streq(name, "gsub")) && argc == 2 &&
        comp_ntype(c, argv[0]) == TY_STRING && comp_ntype(c, argv[1]) == TY_STRING) {
      buf_printf(b, "sp_str_%s(sp_poly_to_s(", name); emit_expr(c, recv, b);
      buf_puts(b, "), "); emit_str_expr(c, argv[0], b);
      buf_puts(b, ", "); emit_str_expr(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
  }
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
           rather than reading through the wrong layout). When the call's own
           inferred type stayed poly (an OpenStruct|nil receiver whose to_h
           result feeds a poly dispatch), yield the boxed value instead of
           the concrete pointer -- the consumer switches on cls_id (#3282). */
        int th2 = ++g_tmp;
        buf_printf(b, "({ sp_RbVal _t%d = sp_poly_to_h_m(", th2);
        emit_expr(c, recv, b);
        buf_puts(b, ");");
        /* A boxed slot takes any hash variant as-is. Only the concrete
           sym-keyed slot needs one: a hash built at runtime through the
           general merge path is a PolyPolyHash even when every key is a
           Symbol, and rejecting it outright was wrong (#3452). */
        if (comp_ntype(c, id) == TY_POLY) buf_printf(b, " _t%d; })", th2);
        else buf_printf(b, " sp_poly_as_sym_hash(_t%d); })", th2);
      }
      return 1;
    }
  }
  /* The Regexp surface on a boxed receiver: the names Regexp alone owns unbox
     the pattern and re-dispatch through the typed emitter, and the match forms
     -- which String owns too -- go to the runtime pair dispatch, where either
     operand may carry the pattern. A Regexp arriving through a block parameter
     had no arm at all and raised NoMethodError naming Regexp (#3961). */
  if (recv >= 0 && rt == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name) && g_n_argov < MAX_ARG_OVERRIDE) {
    static const char *const RXO[] = { "source", "options", "casefold?",
                                       "named_captures", "names", NULL };
    int want_rx = 0;
    for (int i = 0; RXO[i] && !want_rx; i++) if (sp_streq(name, RXO[i]) && argc == 0) want_rx = 1;
    if (want_rx) {
      int trx = ++g_tmp;
      Buf rbx; memset(&rbx, 0, sizeof rbx); emit_expr(c, recv, &rbx);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "mrb_regexp_pattern *_t%d = sp_poly_as_pattern(%s);\n",
                 trx, rbx.p ? rbx.p : "sp_box_nil()");
      free(rbx.p);
      g_argov_node[g_n_argov] = recv;
      snprintf(g_argov_text[g_n_argov], sizeof g_argov_text[0], "_t%d", trx);
      g_n_argov++;
      TyKind svrx = c->ntype[recv]; c->ntype[recv] = TY_REGEX;
      emit_call(c, id, b);
      c->ntype[recv] = svrx;
      g_n_argov--;
      return 1;
    }
  }
  if (recv >= 0 && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      !user_defines_or_reads(c, name) &&
      (sp_streq(name, "match?") || sp_streq(name, "match") || sp_streq(name, "=~")) &&
      (rt == TY_POLY || ((rt == TY_STRING || rt == TY_STRBUF) &&
                         comp_ntype(c, argv[0]) == TY_POLY))) {
    /* `=~` answers the match offset or nil, so it rides boxed like the typed
       form does; the other two answer a bool and a MatchData. */
    if (sp_streq(name, "=~")) {
      int tmi = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = sp_poly_match_index(", tmi);
      emit_boxed(c, recv, b); buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_printf(b, "); _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d); })", tmi, tmi);
      return 1;
    }
    const char *fn = sp_streq(name, "match?") ? "sp_poly_match_p" : "sp_poly_match_data";
    buf_printf(b, "%s(", fn); emit_boxed(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly === arg dispatches on the RECEIVER's runtime class, the way CRuby's
     case-equality does: a Regexp matches, a Range covers, a Class tests
     membership. Answering plain equality made every one of them false (#3963). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "===")) {
    int has_user = 0;
    for (int k = 0; k < c->nclasses && !has_user; k++)
      if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
    if (!has_user) {
      buf_puts(b, "sp_poly_case_eq(");
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
        snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[k].c_name, tv, iv_c(sym + 1));
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
    buf_printf(b, "; sp_int _t%d = ", tb); emit_int_expr(c, argv[0], b);
    buf_printf(b, "; _t%d.tag == SP_TAG_STR ? sp_str_to_i_base(_t%d.v.s, _t%d)"
                  " : (sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 1, expected 0)\"), (sp_int)0); })",
               tr, tr, tb);
    return 1;
  }

  /* poly receiver count(v): value-equality element count over a boxed array */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && sp_streq(name, "count") &&
      nt_ref(nt, id, "block") < 0) {
    /* Only a user definition that can TAKE one positional argument blocks
       this arm: a `count(a, b)` or a reader cannot answer the call, and
       counting it steered a genuine String or Array receiver into the
       dispatch, whose arity filter then dropped every arm and raised
       (#4195). Same judgement as the dispatch's own candidate filter. */
    int has_user_cnt = 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user_cnt; kk++) {
      int mi_k = comp_method_in_chain(c, kk, "count", NULL);
      if (mi_k >= 0) {
        Scope *cs_k = &c->scopes[mi_k];
        if (cs_k->rest_idx >= 0 || (1 >= cs_k->nrequired && 1 <= cs_k->nparams))
          has_user_cnt = 1;
      }
    }
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
    if (!g_poly_builtin_arm)
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
  /* poly.scan(re) -- a String read out of a `{}`-then-filled Hash reaches
     here poly-typed, and without an arm it hit the NoMethodError gate
     (#3368). Mirrors the rt==TY_STRING arms: no captures -> string array,
     captures -> array of arrays. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "scan") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, name)) {
    /* Guard on the tag: only a String actually answers #scan, and a nil (or an
       Integer) receiver must raise NoMethodError as CRuby does rather than be
       stringified into an empty scan (test/issue_3147.rb pins that). */
    int rli = re_lit_index(c, argv[0]);
    int str_arg = comp_ntype(c, argv[0]) == TY_STRING;
    /* A pattern the compiler cannot resolve to a literal -- an inline
       `Regexp.new(s)`, a local holding one, an interpolated literal -- is
       still an mrb_regexp_pattern* at run time, and the String-receiver arm
       has taken it since #3389. Without it here the call fell past this
       handler to the unresolved-call gate and raised on the String (#3392).
       The tag guard stays: only a String answers #scan. */
    int re_arg = !str_arg && rli < 0 && comp_ntype(c, argv[0]) == TY_REGEX;
    if (rli >= 0 || str_arg || re_arg) {
      /* follow the type analyze settled on, so emit and type stay in step for
         a run-time pattern (sp_re_scan_poly decides per match) */
      int poly_res = comp_ntype(c, id) == TY_POLY_ARRAY;
      int ts = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ts); emit_boxed(c, recv, b);
      buf_printf(b, "; (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) ? ", ts, ts);
      if (rli >= 0)
        buf_printf(b, "%s(sp_re_pat_%d, sp_poly_to_s(_t%d))",
                   poly_res ? "sp_re_scan_poly" : "sp_re_scan", rli, ts);
      else if (re_arg) {
        buf_printf(b, "%s(", poly_res ? "sp_re_scan_poly" : "sp_re_scan");
        emit_expr(c, argv[0], b);
        buf_printf(b, ", sp_poly_to_s(_t%d))", ts);
      }
      else {
        buf_printf(b, "sp_str_scan(sp_poly_to_s(_t%d), ", ts);
        emit_expr(c, argv[0], b); buf_puts(b, ")");
      }
      buf_printf(b, " : (%s *)(sp_raise_nomethod(sp_nomethod_msg(\"scan\", _t%d)), (void *)0); })",
                 poly_res ? "sp_PolyArray" : "sp_StrArray", ts);
      return 1;
    }
  }
  /* The one-String-argument transforms. The block below covers the poly String
     surface only for the zero-argument shapes, so a String arriving through a
     poly slot -- a Fiber#resume value, a container read -- had no arm for
     these and fell through to the unresolved-call raise, naming String, which
     is what it was (#3436). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) &&
      !user_defines_or_reads(c, name)) {
    /* the inference rule answers TY_STRING, so hand back the raw const char * */
    buf_printf(b, "sp_str_%s(sp_poly_recv_s(", name); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"), ", name); emit_str_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* `dig` on a receiver that stayed poly: the arms above are per container
     kind, and with none matching the call was coerced to a hash and raised
     NoMethodError on an Array that answers it. The runtime walk dispatches on
     the receiver's own kind at each step (#3509). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "dig") && argc >= 1 &&
      nt_ref(nt, id, "block") < 0) {
    int has_user_dig = 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user_dig; kk++)
      if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
          comp_reader_in_chain(c, kk, name, NULL)) has_user_dig = 1;
    if (!has_user_dig) {
      if (argc == 1 && nt_kind(nt, argv[0]) == NK_SplatNode) {
        buf_puts(b, "sp_poly_dig_list("); emit_boxed(c, recv, b);
        buf_puts(b, ", sp_poly_to_poly_array("); emit_boxed(c, argv[0], b); buf_puts(b, "))");
        return 1;
      }
      int any_splat = 0;
      for (int a = 0; a < argc; a++)
        if (nt_kind(nt, argv[a]) == NK_SplatNode) any_splat = 1;
      if (!any_splat) {
        buf_printf(b, "sp_poly_dig_n("); emit_boxed(c, recv, b);
        buf_printf(b, ", %d, (sp_RbVal[]){", argc);
        for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_boxed(c, argv[a], b); }
        buf_puts(b, "})");
        return 1;
      }
    }
  }
  /* The one-argument numeric methods, the same rule the no-argument table
     below uses: dispatch on the runtime tag unless a user class owns the name.
     They were missing entirely, so an exact Rational reaching divmod / modulo
     / quo through a block parameter raised NoMethodError on methods it
     answers (#3512). */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && nt_ref(nt, id, "block") < 0) {
    const char *pfn1 =
      sp_streq(name, "divmod")  ? "sp_poly_divmod" :
      sp_streq(name, "modulo")  ? "sp_poly_mod" :
      sp_streq(name, "div")     ? "sp_poly_div_m" :
      sp_streq(name, "remainder") ? "sp_poly_remainder" :
      sp_streq(name, "coerce")  ? "sp_poly_coerce" :
      sp_streq(name, "quo")     ? "sp_poly_quo" : NULL;
    if (pfn1) {
      int has_user1 = 0;
      if (!g_poly_builtin_arm)
      for (int kk = 0; kk < c->nclasses && !has_user1; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
            comp_reader_in_chain(c, kk, name, NULL)) has_user1 = 1;
      if (!has_user1) {
        buf_printf(b, "%s(", pfn1); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
        return 1;
      }
    }
  }
  /* poly.scan(pat) with no block: the rows themselves. The pattern may arrive
     boxed (read out of a table), where its payload IS the compiled pattern. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "scan") && argc == 1 &&
      nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, "scan")) {
    int sre = re_lit_index(c, argv[0]);
    TyKind spt = comp_ntype(c, argv[0]);
    TyKind sres = comp_ntype(c, id);
    const char *sfn = sres == TY_STR_ARRAY ? "sp_re_scan" : "sp_re_scan_poly";
    if (spt == TY_STRING) { sfn = "sp_str_scan"; }
    buf_printf(b, "%s(", sfn);
    if (sre >= 0) buf_printf(b, "sp_re_pat_%d", sre);
    else if (spt == TY_REGEX) emit_expr(c, argv[0], b);
    else if (spt == TY_STRING) { buf_puts(b, "sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"), ", name); }
    else { buf_puts(b, "(mrb_regexp_pattern *)("); emit_boxed(c, argv[0], b); buf_puts(b, ").v.p"); }
    if (spt == TY_STRING) { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else { buf_puts(b, ", sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name); }
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY)
  /* poly.scan(pat) { }: the block form over a receiver only known to be a
     String at run time. Rows are precomputed exactly as the typed-String arm
     does, then the block runs per row; the value is the receiver string
     (CRuby answers self). */
  if (sp_streq(name, "scan") && argc == 1 && nt_ref(nt, id, "block") >= 0 &&
      !user_defines_or_reads(c, "scan")) {
    int sblk = nt_ref(nt, id, "block");
    const char *sp0 = block_param_name(c, sblk, 0);
    const char *sp0r = sp0 ? rename_local(sp0) : NULL;
    int sbody = nt_ref(nt, sblk, "body");
    int sbn = 0; const int *sbb = sbody >= 0 ? nt_arr(nt, sbody, "body", &sbn) : NULL;
    int re_i = re_lit_index(c, argv[0]);
    TyKind pat_t = comp_ntype(c, argv[0]);
    int ts = ++g_tmp, tm = ++g_tmp, ti = ++g_tmp;
    buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
    buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
    buf_printf(b, " sp_StrArray *_t%d = ", tm);
    if (re_i >= 0) buf_printf(b, "sp_re_scan(sp_re_pat_%d, _t%d)", re_i, ts);
    else if (pat_t == TY_REGEX) { buf_puts(b, "sp_re_scan("); emit_expr(c, argv[0], b); buf_printf(b, ", _t%d)", ts); }
    else if (pat_t == TY_STRING) { buf_printf(b, "sp_str_scan(_t%d, ", ts); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
    else {
      /* the pattern arrived boxed (a Regexp read out of a table): its payload
         IS the compiled pattern */
      buf_puts(b, "sp_re_scan((mrb_regexp_pattern *)(");
      emit_boxed(c, argv[0], b);
      buf_printf(b, ").v.p, _t%d)", ts);
    }
    buf_printf(b, "; SP_GC_ROOT(_t%d);", tm);
    buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {", ti, ti, tm, ti);
    if (sp0r) {
      Scope *sbs = comp_scope_of(c, sblk);
      LocalVar *sblv = sbs ? scope_local(sbs, sp0r) : NULL;
      if (sblv && sblv->type == TY_POLY)
        buf_printf(b, " sp_RbVal lv_%s = sp_box_str(sp_StrArray_get(_t%d, _t%d));", sp0r, tm, ti);
      else
        buf_printf(b, " const char *lv_%s = sp_StrArray_get(_t%d, _t%d);", sp0r, tm, ti);
    }
    for (int k2 = 0; k2 < sbn; k2++) emit_stmt(c, sbb[k2], b, 0);
    buf_printf(b, " } _t%d; })", ts);
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && argc == 0) {
    /* Skip when a user class defines nil? so its method wins the dispatch --
       the same reason the to_a arm below gives. A Null Object answering true
       was folded to the tag test and its guard silently never fired. */
    if (sp_streq(name, "nil?") && !user_defines_or_reads(c, name)) {
      buf_puts(b, "sp_poly_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
    }
    /* to_a on a runtime-tagged value: nil -> [], array -> itself, hash -> its
       pairs, anything else CRuby's NoMethodError. Skip when a user class
       defines to_a so its method wins the dispatch. */
    /* to_a / deconstruct on a poly value: for a Struct/Data both are the
       member values in order (sp_poly_to_a_arr derives them from the to_h
       hook); for an array/hash it is the elements/pairs. */
    if ((sp_streq(name, "to_a") || sp_streq(name, "deconstruct")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      int has_user_ta = 0;
      if (!g_poly_builtin_arm)
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
      if (!g_poly_builtin_arm)
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
      if (!g_poly_builtin_arm)
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
      if (!g_poly_builtin_arm)
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
          buf_printf(g_pre, "sp_int _t%d = 0;\n", tc);
          emit_indent(g_pre, g_indent);
          buf_printf(g_pre, "for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++) {\n", ti, ti, tr, ti);
          {
            /* sp_poly_each_elem, not a raw index: a boxed Hash renders each
               entry as its [key, value] pair, which a two-parameter block
               autosplats the way every sibling element loop does (#3448). */
            char csrc[64]; snprintf(csrc, sizeof csrc, "sp_poly_each_elem(_t%d, _t%d)", tr, ti);
            if (!emit_iter_autosplat(c, cblk, TY_POLY_ARRAY, csrc, g_indent + 1) && cp0r) {
              emit_indent(g_pre, g_indent + 1);
              buf_printf(g_pre, "lv_%s = %s;\n", cp0r, csrc);
            }
          }
          int svind = g_indent; g_indent++;
          for (int j = 0; j < cbn - 1; j++) emit_stmt(c, cbb[j], g_pre, g_indent);
          /* Render the condition into its own buffer first: anything it has to
             hoist (a rooted argument temp) is a STATEMENT, and appending it to
             g_pre after "if (" was written put the declaration in the middle of
             the expression. */
          { Buf ccv; memset(&ccv, 0, sizeof ccv);
            emit_boxed(c, cbb[cbn - 1], &ccv);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "if (sp_poly_truthy(%s)) _t%d++;\n",
                       ccv.p ? ccv.p : "sp_box_nil()", tc);
            free(ccv.p); }
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
      /* The question is about the name being CALLED. Asking about `length`
         for an `empty?` call sent every program that defines `length`
         anywhere down the dispatch path, where nothing answers `empty?` --
         so the call became an unconditional raise whatever the receiver was
         (#3805). Defining `length` does not define `empty?` in Ruby either. */
      if (!g_poly_builtin_arm)
      for (int kk = 0; kk < c->nclasses && !has_user_len; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
            comp_reader_in_chain(c, kk, name, NULL)) has_user_len = 1;
      if (!has_user_len) {
        if (sp_streq(name, "empty?")) {
          /* A user object has no #empty? of its own here, and sp_poly_length
             answers 0 for one, which would make every such object empty.
             Raise instead, as Ruby does. */
          buf_puts(b, "({ sp_RbVal _ep = "); emit_boxed(c, recv, b);
          buf_puts(b, "; sp_poly_is_user_obj(_ep) ? (sp_raise_poly_nomethod(\"empty?\", _ep), 0)"
                      " : (sp_poly_length(_ep) == 0); })");
        }
        else if (sp_streq(name, "size")) {
          /* Integer#size is the byte width of the machine representation, not
             a length; sp_poly_length has no arm for it and answered 0. */
          buf_puts(b, "sp_poly_size("); emit_boxed(c, recv, b); buf_puts(b, ")");
        }
        else {
          /* nil / a number / a user object has no #length: answering 0 turned a
             NoMethodError into a silent zero (#3974) */
          buf_puts(b, "sp_poly_length_m("); emit_boxed(c, recv, b); buf_puts(b, ")");
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
    /* Same guard as #to_s above: a user class defining the conversion wins
       through poly dispatch. sp_poly_to_i answers 0 for an object, so a
       wrapper's `value.to_i` silently read zero. */
    if (sp_streq(name, "to_i") || sp_streq(name, "to_f")) {
      int has_user_conv = 0;
      if (!g_poly_builtin_arm)
        for (int k = 0; k < c->nclasses && !has_user_conv; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user_conv = 1;
      if (!has_user_conv) {
        /* sp_poly_to_i_meth: this is the METHOD, named by the program, so an
           object without it is NoMethodError rather than the conversion
           protocol's TypeError. */
        buf_printf(b, "%s(", sp_streq(name, "to_i") ? "sp_poly_to_i_meth" : "sp_poly_to_f");
        emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
      }
    }
    /* Complex#real / #imaginary on a poly value (a Complex read out of a
       container). A user class defining the same name wins via poly dispatch. */
    if ((sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag") ||
         sp_streq(name, "conjugate") || sp_streq(name, "conj")) && argc == 0) {
      int has_user = 0;
      if (!g_poly_builtin_arm)
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
      if (!g_poly_builtin_arm)
      for (int kk = 0; kk < c->nclasses && !has_user; kk++)
        if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
      if (!has_user) {
        int t = ++g_tmp;
        /* The arm yields a raw sp_sym. When the call's own slot is poly (a
           case-result carrier, a boxed argument) it must be boxed HERE -- the
           generic boxed-value emitter passes a poly-typed node through
           untouched, so a raw scalar would land in an sp_RbVal slot (#3331). */
        int box_sym = comp_ntype(c, id) == TY_POLY;
        if (box_sym) buf_puts(b, "sp_box_sym(");
        /* Root the boxed receiver: sp_sym_intern reads through the String's
           data pointer and allocates, so a GC mid-intern could otherwise free
           an unrooted temporary String out from under it. */
        /* a shared-string handle is a String: deref it into the immediate
           form the arm reads, or to_sym raised for it (#4279) */
        buf_printf(b, "({ sp_RbVal _t%d = sp_poly_strbuf_deref(", t); emit_expr(c, recv, b);
        buf_puts(b, ")");
        buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); _t%d.tag == SP_TAG_STR ? sp_sym_intern_n(_t%d.v.s, sp_str_byte_len(_t%d.v.s))"
                      " : (_t%d.tag == SP_TAG_SYM ? (sp_sym)_t%d.v.i"
                      " : (sp_raise_poly_nomethod(\"to_sym\", _t%d), (sp_sym)0)); })",
                   t, t, t, t, t, t, t);
        if (box_sym) buf_puts(b, ")");
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
        if (!g_poly_builtin_arm)
        for (int kk = 0; kk < c->nclasses && !has_user; kk++)
          if (comp_method_in_chain(c, kk, name, NULL) >= 0 ||
              comp_reader_in_chain(c, kk, name, NULL)) has_user = 1;
        if (!has_user) {
          buf_printf(b, "%s(", pfn); emit_expr(c, recv, b); buf_puts(b, ")");
          return 1;
        }
      }
    }
    /* These stringify the receiver and apply a String method to the result, so
       a user class owning the name must win: a Struct member, Data field or
       attr_reader called `upcase` otherwise answers the UPCASED #inspect of
       the object holding it (#3380). The `bytes` / `chars` arms below have
       carried this guard since #2909 / #3364; this is the same list of names
       that return a String rather than an array, which is why it was missed.
       Declining falls through to the general poly dispatch, which reads the
       member -- and still serves a genuine String receiver in the same
       program. */
    int str_conv_owned = user_defines_or_reads(c, name);
    if (!str_conv_owned) {
    if ((sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) {
      /* per kind, not per string: an Integer counts up and an Enumerator pulls
         its next value, where the string succ answered "" for both (#3843) */
      buf_puts(b, "sp_poly_succ_m("); emit_expr(c, recv, b);
      buf_printf(b, ", %d)", sp_streq(name, "next") ? 1 : 0);
      return 1;
    }
    if (sp_streq(name, "upcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_upcase, \"upcase\")"); return 1; }
    if (sp_streq(name, "downcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_downcase, \"downcase\")"); return 1; }
    if (sp_streq(name, "capitalize"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_capitalize, \"capitalize\")"); return 1; }
    if (sp_streq(name, "swapcase"))     { buf_puts(b, "sp_poly_case_conv("); emit_expr(c, recv, b); buf_puts(b, ", sp_str_swapcase, \"swapcase\")"); return 1; }
    if (sp_streq(name, "strip"))      { buf_puts(b, "sp_box_str(sp_str_strip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"strip\")))"); return 1; }
    /* `strip` had an arm and its one-sided siblings did not, which is the
       shape of most of what follows: a String reaching the dispatch through a
       poly slot answered NoMethodError naming String, for a method String
       has. Each of these already works on a concrete receiver and the runtime
       function is the one that arm calls. */
    if (sp_streq(name, "lstrip"))     { buf_puts(b, "sp_box_str(sp_str_lstrip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"lstrip\")))"); return 1; }
    if (sp_streq(name, "rstrip"))     { buf_puts(b, "sp_box_str(sp_str_rstrip(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"rstrip\")))"); return 1; }
    /* to_str is the implicit-conversion protocol, so a poly slot holding a
       String has to answer it: sp_poly_recv_s raises for anything else, which
       is what a non-String must do here. */
    if (sp_streq(name, "to_str") && argc == 0) {
      buf_puts(b, "sp_box_str(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"to_str\"))"); return 1;
    }
    if (sp_streq(name, "ascii_only?") && argc == 0) {
      buf_puts(b, "sp_box_bool(sp_str_ascii_only(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"ascii_only?\")))"); return 1;
    }
    if (sp_streq(name, "valid_encoding?") && argc == 0) {
      buf_puts(b, "sp_box_bool(sp_str_valid_encoding(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"valid_encoding?\")))"); return 1;
    }
    /* encode is a no-op on the concrete arm -- every string here is UTF-8 --
       so the poly one only has to unbox and re-box, and raise for a
       non-String the way the others do. */
    if (sp_streq(name, "encode") && argc == 0) {
      buf_puts(b, "sp_box_str(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"encode\"))"); return 1;
    }
    if (sp_streq(name, "scrub") && argc == 0) {
      buf_puts(b, "sp_box_str(sp_str_scrub(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"scrub\"), 0))"); return 1;
    }
    if (sp_streq(name, "reverse"))    { buf_puts(b, "sp_poly_reverse("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
    /* `encoding` on a boxed String: the concrete arm has answered it since
       #723, and the poly dispatch had no entry -- so a String read out of a
       poly array raised NoMethodError naming its own class. */
    if (sp_streq(name, "encoding") && argc == 0) {
      buf_puts(b, "sp_box_encoding(sp_str_is_binary(sp_poly_recv_s(");
      emit_expr(c, recv, b);
      buf_puts(b, ", \"encoding\")) ? sp_encoding_binary() : sp_encoding_utf8())");
      return 1;
    }
    if (sp_streq(name, "chomp"))      { buf_puts(b, "sp_box_str(sp_str_chomp(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"chomp\")))"); return 1; }
    if (sp_streq(name, "chop"))       { buf_puts(b, "sp_box_str(sp_str_chop(sp_poly_recv_s("); emit_expr(c, recv, b); buf_printf(b, ", \"chop\")))"); return 1; }
    /* The one-String-argument transforms, which the table above covers only for
       the zero-argument shapes. A String arriving through a poly slot -- a
       Fiber#resume value, a container read -- had no arm for these and raised
       NoMethodError naming String, which is what it was (#3436). */
    if ((sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) && argc == 1) {
      buf_printf(b, "sp_box_str(sp_str_%s(sp_poly_recv_s(", name); emit_expr(c, recv, b);
      buf_printf(b, ", \"%s\"), ", name); emit_str_expr(c, argv[0], b); buf_puts(b, "))");
      return 1;
    }
    }
    if (sp_streq(name, "chr") && !str_conv_owned) {
      /* dispatch on the runtime tag: (48 + n).chr through a widened int
         must be Integer#chr -- stringifying first turned 61.chr into
         "61".chr == "6", corrupting percent-encoding digits (#3328) */
      int tvC = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tvC); emit_boxed(c, recv, b);
      buf_printf(b, "; _t%d.tag == SP_TAG_INT ? sp_box_str(sp_int_chr(_t%d.v.i))"
                    " : sp_box_str(sp_str_chr(sp_poly_to_s(_t%d))); })", tvC, tvC, tvC);
      return 1;
    }
    /* poly.bytes / poly.codepoints -> concrete TY_INT_ARRAY, no boxing (matches
       the inference rule). A String that widened to poly (a binary lump slice)
       reaches here; without this arm .bytes hit the generic poly method
       dispatch and raised "undefined method 'bytes' for poly". */
    if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) {
      /* Skip when a user class owns the name -- the runtime value may be one of
         those, and stringifying it would answer the bytes of its #inspect. A
         Struct member or attr_reader called `bytes` hit exactly that (#3364);
         the `chars` arm below has carried this guard since #2909. */
      if (!user_defines_or_reads(c, name)) {
        buf_printf(b, "sp_str_%s(sp_poly_recv_s(", sp_streq(name, "bytes") ? "bytes" : "codepoints");
        emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name); return 1;
      }
    }
    /* poly.chars -> TY_STR_ARRAY: a String read out of a container or
       destructured from a pair (`|a, b|`) reaches here poly-typed (#2909). */
    if (sp_streq(name, "chars") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      if (!user_defines_or_reads(c, "chars")) {
        buf_puts(b, "sp_str_chars(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"chars\"))"); return 1;
      }
    }
    /* poly.each_char { }: walk the same char array #chars answers. A String
       receiver has its own emitter that does not materialize one; a poly
       receiver only learns it is a String at run time, so it pays the array
       and yields out of it. Answers the receiver's string, as String#each_char
       answers self (#3402). */
    if (sp_streq(name, "each_char") && argc == 0 && nt_ref(nt, id, "block") >= 0 &&
        !user_defines_or_reads(c, "each_char") && !user_defines_or_reads(c, "chars")) {
      int eblk = nt_ref(nt, id, "block");
      const char *ebp = block_param_name(c, eblk, 0);
      const char *ebpn = ebp ? rename_local(ebp) : NULL;
      int ebody = nt_ref(nt, eblk, "body");
      int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
      int ts = ++g_tmp, ta = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
      buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
      buf_printf(b, " sp_StrArray *_t%d = sp_str_chars(_t%d); SP_GC_ROOT(_t%d);", ta, ts, ta);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
      if (ebpn) buf_printf(b, " const char *lv_%s = sp_StrArray_get(_t%d, _t%d);", ebpn, ta, ti);
      for (int k2 = 0; k2 < ebn; k2++) emit_stmt(c, ebb[k2], b, 0);
      buf_printf(b, " } _t%d; })", ts);
      return 1;
    }
    /* poly.each_byte { } / .each_codepoint { }: the same shape as each_char
       above, over the integer array #bytes / #codepoints answers. */
    if ((sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) && argc == 0 &&
        nt_ref(nt, id, "block") >= 0 && !user_defines_or_reads(c, name)) {
      int eblk = nt_ref(nt, id, "block");
      const char *ebp = block_param_name(c, eblk, 0);
      const char *ebpn = ebp ? rename_local(ebp) : NULL;
      int ebody = nt_ref(nt, eblk, "body");
      int ebn = 0; const int *ebb = ebody >= 0 ? nt_arr(nt, ebody, "body", &ebn) : NULL;
      const char *fn = sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
      int ts = ++g_tmp, ta = ++g_tmp, ti = ++g_tmp;
      buf_printf(b, "({ const char *_t%d = sp_poly_recv_s(", ts); emit_expr(c, recv, b);
      buf_printf(b, ", \"%s\"); SP_GC_ROOT(_t%d);", name, ts);
      buf_printf(b, " sp_IntArray *_t%d = %s(_t%d); SP_GC_ROOT(_t%d);", ta, fn, ts, ta);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++) {", ti, ti, ta, ti);
      if (ebpn) {
        Scope *ebs = comp_scope_of(c, eblk);
        LocalVar *eblv = ebs ? scope_local(ebs, ebpn) : NULL;
        if (eblv && eblv->type == TY_POLY)
          buf_printf(b, " sp_RbVal lv_%s = sp_box_int(sp_IntArray_get(_t%d, _t%d));", ebpn, ta, ti);
        else
          buf_printf(b, " sp_int lv_%s = sp_IntArray_get(_t%d, _t%d);", ebpn, ta, ti);
      }
      for (int k2 = 0; k2 < ebn; k2++) emit_stmt(c, ebb[k2], b, 0);
      buf_printf(b, " } _t%d; })", ts);
      return 1;
    }
    /* poly.lines -> TY_STR_ARRAY, the same shape as #chars above (#3403) */
    if (sp_streq(name, "lines") && argc == 0 && nt_ref(nt, id, "block") < 0) {
      if (!user_defines_or_reads(c, "lines")) {
        buf_puts(b, "sp_str_lines(sp_poly_recv_s("); emit_expr(c, recv, b); buf_puts(b, ", \"lines\"))"); return 1;
      }
    }
    /* A blockless each_char / each_line / each_byte / each_codepoint is
       CRuby's Enumerator; materialize it into the array chars / lines / bytes
       answer, which is what the typed String path does too. */
    if (argc == 0 && nt_ref(nt, id, "block") < 0 && !user_defines_or_reads(c, name) &&
        (sp_streq(name, "each_char") || sp_streq(name, "each_line") ||
         sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint"))) {
      const char *fn = sp_streq(name, "each_char") ? "sp_str_chars"
                     : sp_streq(name, "each_line") ? "sp_str_lines"
                     : sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
      buf_printf(b, "%s(sp_poly_recv_s(", fn); emit_expr(c, recv, b); buf_printf(b, ", \"%s\"))", name);
      return 1;
    }
    if (sp_streq(name, "freeze"))     { buf_puts(b, "sp_poly_freeze("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1; }
  }
  /* Hash#merge(other) { |key, old, new| }: the block decides the value for a
     key both hashes carry. Walk the other hash's pairs into a copy of the
     receiver, consulting the block on a collision -- sp_poly_hash_merge has no
     block form, and this arm handles boxed/cross-layout receivers. */
  if (recv >= 0 && (rt == TY_POLY || (ty_is_hash(rt) && rt != TY_POLY_POLY_HASH)) &&
      sp_streq(name, "merge") && argc == 1 &&
      nt_ref(nt, id, "block") >= 0 && !user_defines_or_reads(c, "merge")) {
    int mblk = nt_ref(nt, id, "block");
    int mbody = nt_ref(nt, mblk, "body");
    int mbn = 0; const int *mbb = mbody >= 0 ? nt_arr(nt, mbody, "body", &mbn) : NULL;
    if (mbn > 0) {
      const char *mp[3];
      for (int i = 0; i < 3; i++) {
        const char *pn = block_param_name(c, mblk, i);
        mp[i] = pn ? rename_local(pn) : NULL;
      }
      /* the block sees three boxed values */
      Scope *ms = comp_scope_of(c, mblk);
      LocalVar *mlv[3]; TyKind msave[3];
      for (int i = 0; i < 3; i++) {
        const char *pn = block_param_name(c, mblk, i);
        mlv[i] = (ms && pn) ? scope_local(ms, pn) : NULL;
        msave[i] = mlv[i] ? mlv[i]->type : TY_UNKNOWN;
        if (mlv[i]) mlv[i]->type = TY_POLY;
      }
      for (int j = 0; j < mbn; j++) infer_subtree(c, mbb[j]);
      int ta = ++g_tmp, tb = ++g_tmp, tr = ++g_tmp, tp = ++g_tmp, ti = ++g_tmp, tk = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", ta); emit_boxed(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); sp_RbVal _t%d = ", ta, tb); emit_boxed(c, argv[0], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", tb);
      buf_printf(b, " sp_PolyPolyHash *_t%d = sp_poly_hash_merge(_t%d, sp_box_nil()); SP_GC_ROOT(_t%d);",
                 tr, ta, tr);
      buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_a_arr(_t%d); SP_GC_ROOT(_t%d);", tp, tb, tp);
      buf_printf(b, " for (sp_int _t%d = 0; _t%d && _t%d < sp_PolyArray_length(_t%d); _t%d++) {",
                 ti, tp, ti, tp, ti);
      buf_printf(b, " sp_RbVal _t%d = sp_PolyArray_get(_t%d, _t%d);", tk, tp, ti);
      buf_printf(b, " sp_RbVal _tk%d = sp_poly_arr_get(_t%d, 0), _tv%d = sp_poly_arr_get(_t%d, 1);",
                 tk, tk, tk, tk);
      buf_printf(b, " if (sp_PolyPolyHash_has_key(_t%d, _tk%d)) {", tr, tk);
      if (mp[0]) buf_printf(b, " sp_RbVal lv_%s = _tk%d;", mp[0], tk);
      if (mp[1]) buf_printf(b, " sp_RbVal lv_%s = sp_PolyPolyHash_get(_t%d, _tk%d);", mp[1], tr, tk);
      if (mp[2]) buf_printf(b, " sp_RbVal lv_%s = _tv%d;", mp[2], tk);
      { Buf *saved_pre = g_pre; g_pre = b;
        for (int j = 0; j < mbn - 1; j++) { emit_stmt(c, mbb[j], b, 0); buf_puts(b, " "); }
        /* the tail's own prelude (an interpolation temp) has to land BEFORE
           the set call, so render the value into its own buffer while g_pre
           still points at the statement stream */
        Buf tailv; memset(&tailv, 0, sizeof tailv);
        emit_boxed(c, mbb[mbn - 1], &tailv);
        g_pre = saved_pre;
        buf_printf(b, " sp_PolyPolyHash_set(_t%d, _tk%d, %s); }",
                   tr, tk, tailv.p ? tailv.p : "sp_box_nil()");
        free(tailv.p); }
      /* newline before `else`: the arm above ends with `}`, and the two would
         otherwise concatenate into the `} else` form the C style forbids */
      buf_printf(b, "\nelse sp_PolyPolyHash_set(_t%d, _tk%d, _tv%d); }", tr, tk, tk);
      buf_printf(b, " _t%d; })", tr);
      for (int i = 0; i < 3; i++) if (mlv[i]) mlv[i]->type = msave[i];
      return 1;
    }
  }
  /* poly.ljust/rjust/center(width[, pad]): a String read from a container
     widened to poly. Pad via sp_poly_to_s and re-box (#3222). Outside the
     argc==0 block above since these take a width (and optional pad) arg. Skip
     when a user class overrides the name (the runtime value may be it). */
  if (recv >= 0 && rt == TY_POLY &&
      (sp_streq(name, "ljust") || sp_streq(name, "rjust") || sp_streq(name, "center")) &&
      (argc == 1 || argc == 2)) {
    int has_user = 0;
    if (!g_poly_builtin_arm)
    for (int kk = 0; kk < c->nclasses && !has_user; kk++)
      if (comp_method_in_chain(c, kk, name, NULL) >= 0) has_user = 1;
    if (!has_user) {
      const char *fn = sp_streq(name, "ljust") ? "sp_str_ljust"
                     : sp_streq(name, "rjust") ? "sp_str_rjust" : "sp_str_center";
      buf_printf(b, "sp_box_str(%s%s(sp_poly_to_s(", fn, argc == 2 ? "2" : "");
      emit_expr(c, recv, b); buf_puts(b, "), ");
      emit_int_expr(c, argv[0], b);
      if (argc == 2) { buf_puts(b, ", "); emit_expr(c, argv[1], b); }
      buf_puts(b, "))");
      return 1;
    }
  }
  /* The argument-taking String methods on a poly receiver, outside the argc==0
     block for the same reason ljust is: that block is guarded on argc == 0, so
     an arm for these placed inside it can never be entered. Each reuses the
     runtime function the concrete arm calls, and declines to a user class
     owning the name, as the neighbours do. */
  /* The separator forms of the trimming methods. Their argc==0 spellings are
     in the table above; a line reader chomping with its OWN separator --
     `line.chomp(eol)` -- passes one, and that reached NoMethodError. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && !user_defines_or_reads(c, name) &&
      (sp_streq(name, "chomp") || sp_streq(name, "delete_prefix") ||
       sp_streq(name, "delete_suffix"))) {
    const char *fn = sp_streq(name, "chomp") ? "sp_str_chomp_sep"
                   : sp_streq(name, "delete_prefix") ? "sp_str_delete_prefix"
                   : "sp_str_delete_suffix";
    /* TY_STRING, not boxed: the analyze arm types these as the String they
       are, so the slot takes a const char * directly. */
    buf_printf(b, "%s(sp_poly_recv_s(", fn);
    emit_expr(c, recv, b); buf_printf(b, ", \"%s\"), ", name);
    emit_str_expr(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  if (recv >= 0 && rt == TY_POLY && !user_defines_or_reads(c, name) &&
      ((sp_streq(name, "unpack") && argc == 1) ||
       (sp_streq(name, "byteslice") && (argc == 1 || argc == 2)) ||
       (sp_streq(name, "scrub") && argc == 1) ||
       (sp_streq(name, "encode") && (argc == 1 || argc == 2)))) {
    if (sp_streq(name, "unpack")) {
      buf_puts(b, "sp_box_poly_array(sp_str_unpack(sp_poly_recv_s(");
      emit_expr(c, recv, b); buf_puts(b, ", \"unpack\"), ");
      emit_str_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else if (sp_streq(name, "byteslice") && argc == 1 &&
             comp_ntype(c, argv[0]) == TY_RANGE) {
      /* byteslice(RANGE) on a poly receiver. The arm emitted the single-index
         helper whatever the argument was, so the Range reached emit_int_expr
         and became an unconditional "no implicit conversion of Range into
         Integer" -- the range was built, cast to void and thrown away, and
         the raise ran (#4308). Same endpoint resolution the String receiver
         uses. */
      int trp = ++g_tmp;
      buf_printf(b, "({ sp_Range _t%d = ", trp); emit_expr(c, argv[0], b);
      buf_puts(b, "; sp_box_nullable_str(sp_str_byteslice_range(sp_poly_recv_s(");
      emit_expr(c, recv, b);
      buf_printf(b, ", \"byteslice\"), _t%d.first, _t%d.last, _t%d.excl,"
                    " _t%d.first == INTPTR_MIN, _t%d.last == INTPTR_MAX)); })",
                 trp, trp, trp, trp, trp);
    }
    else if (sp_streq(name, "byteslice")) {
      buf_printf(b, "sp_box_nullable_str(sp_str_byteslice%s(sp_poly_recv_s(",
                 argc == 1 ? "1" : "");
      emit_expr(c, recv, b); buf_puts(b, ", \"byteslice\"), ");
      emit_int_expr(c, argv[0], b);
      if (argc == 2) { buf_puts(b, ", "); emit_int_expr(c, argv[1], b); }
      buf_puts(b, "))");
    }
    else if (sp_streq(name, "scrub")) {
      buf_puts(b, "sp_box_str(sp_str_scrub(sp_poly_recv_s(");
      emit_expr(c, recv, b); buf_puts(b, ", \"scrub\"), ");
      emit_str_expr(c, argv[0], b); buf_puts(b, "))");
    }
    else {  /* encode: every string here is UTF-8, so it only unboxes */
      buf_puts(b, "sp_box_str(sp_poly_recv_s(");
      emit_expr(c, recv, b); buf_puts(b, ", \"encode\"))");
    }
    return 1;
  }
  /* Data#with on a poly receiver (a Data read out of a container): build a
     symbol-keyed override hash from the keyword args, then dispatch by cls_id
     to a copy-update constructor (#2890). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "with") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode")) {
    int has_user = 0;
    if (!g_poly_builtin_arm)
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
    if (!g_poly_builtin_arm)
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
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "unpack1") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
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
  /* `x = v` through a SYNTHESIZED writer (attr_writer / accessor, a Struct
     member) on a poly receiver, in value position: the statement form's
     cls_id switch over the writer arms, yielding the assigned value the way
     `[]=` below does. A name some class defines as a method instead takes
     the user-method dispatch, which yields the value itself. */
  if (recv >= 0 && rt == TY_POLY && argc == 1 && name_is_plain_setter(name) &&
      nt_ref(nt, id, "block") < 0 && !diag_user_defines(c, name)) {
    char base[256];
    int ncand = 0;
    if (setter_base_name(name, base, sizeof base))
      for (int k = 0; k < c->nclasses; k++)
        if (comp_is_writer(&c->classes[k], base)) ncand++;
    if (ncand > 0) {
      TyKind at = comp_ntype(c, argv[0]);
      int nil_rhs = (at == TY_NIL || at == TY_VOID);
      TyKind at_eff = nil_rhs ? TY_POLY : at;
      int tv = ++g_tmp, tval = ++g_tmp;
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", tv);
      if (nil_rhs) buf_printf(b, "sp_RbVal _t%d = sp_box_nil();", tval);
      else { emit_ctype(c, at, b); buf_printf(b, " _t%d = ", tval); emit_expr(c, argv[0], b); buf_puts(b, ";"); }
      buf_printf(b, " switch (_t%d.tag == SP_TAG_OBJ ? _t%d.cls_id : 0x7fffffff) {", tv, tv);
      char src[32]; snprintf(src, sizeof src, "_t%d", tval);
      char objp[32]; snprintf(objp, sizeof objp, "_t%d.v.p", tv);
      emit_boxed_writer_arms(c, base, name, objp, src, at_eff, b);
      buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
      buf_printf(b, " } _t%d; })", tval);
      return 1;
    }
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
      /* A computed `outer[idx]` receiver needs the store-back form even for a
         boxed key: a String inner splices into a fresh buffer, and writing to
         the inner value alone dropped the assignment (#4067). */
      int outer_k, oidx_k;
      if (splice_recv_index_slot(c, recv, &outer_k, &oidx_k)) {
        buf_puts(b, "sp_poly_slot_set_key("); emit_boxed(c, outer_k, b);
        buf_puts(b, ", "); emit_int_expr(c, oidx_k, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      }
      else {
        buf_printf(b, "sp_poly_set_poly("); emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      }
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
    /* ...but only a pair of statically Integer operands CAN be a slice. Proc#[]
       is #call, and its arguments are whatever the proc takes -- an Array here,
       which the two-integer reading rejected before any dispatch could happen
       (#4333). Anything else goes through the boxed dispatch; the int path is
       the hot one (optcarrot's poke tables) and keeps its raw operands. */
    if (!(comp_ntype(c, argv[0]) == TY_INT && comp_ntype(c, argv[1]) == TY_INT)) {
      buf_puts(b, "sp_poly_slice_or_call("); emit_expr(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_puts(b, ", "); emit_boxed(c, argv[1], b); buf_puts(b, ")");
      return 1;
    }
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
      if (at == TY_INT || at == TY_POLY) {
        /* The runtime read boxes a TYPED array's element without the sentinel
           check the hot path cannot afford, so a nilable scalar stored in one
           comes back as an ordinary number. Correct it here, at the sites
           analyze marked, rather than in the read itself (#3505). */
        int uns = nullable_int_elem_read(c, id);
        if (uns) buf_puts(b, "sp_unsentinel(");
        /* A receiver proved to hold only a poly array or nil reaches none of
           the hash, string or Struct arms, so the cls_id test and the cold
           call behind it are dead code on this read. analyze established the
           proof for the GC root elision; this is the same fact paying twice. */
        buf_puts(b, at != TY_INT ? "sp_poly_index_poly("
                    : expr_is_arr_or_nil(c, recv) ? "sp_poly_arr_get_aon("
                                                  : "sp_poly_arr_get_hash(");
        emit_expr(c, recv, b);
        buf_puts(b, ", "); emit_expr(c, argv[0], b); buf_puts(b, ")");
        if (uns) buf_puts(b, ")");
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
  /* poly receiver: join. Stands down for a user class that defines the name --
     the arm answered the receiver's #to_s where CRuby entered the method, and
     nothing was reported (#4071). The dispatch below builds the cls_id switch
     with an arm for the class and keeps a builtin one for the arrays. */
  /* ...but a NUMERIC argument cannot be a separator (Array#join(5) is a
     TypeError in CRuby), so it is Thread#join(limit) and answers the thread
     or nil rather than a string (#4287). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join") && argc == 1 &&
      ty_is_numeric(comp_ntype(c, argv[0])) && !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_join_timeout("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_float_expr(c, argv[0], b);
    { TyKind at0 = comp_ntype(c, argv[0]);
      buf_printf(b, ", \"%s\"", at0 == TY_FLOAT ? "Float" : at0 == TY_BIGINT ? "Integer" : "Integer"); }
    buf_puts(b, ")"); return 1;
  }
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "join") &&
      !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_join("); emit_expr(c, recv, b);
    buf_puts(b, ", "); if (argc >= 1) emit_str_expr_nilable(c, argv[0], b); else buf_puts(b, "sp_str_empty");
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
  /* poly receiver: replace(other) -> runtime dispatch (nullable array). The
     user-definition test its `pack` sibling below already carries: a user
     class owning the name means the runtime class decides, and taking the
     builtin here ran String#replace on an object -- answering the argument,
     silently (#4240). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "replace") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
    buf_puts(b, "sp_poly_replace("); emit_expr(c, recv, b);
    buf_puts(b, ", "); emit_boxed(c, argv[0], b); buf_puts(b, ")");
    return 1;
  }
  /* poly receiver: pack(fmt) -> runtime dispatch (nullable array). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "pack") && argc == 1 &&
      !user_defines_or_reads(c, name)) {
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

/* Object's universal protocol -- ===, ==, !=, equal?, eql?, frozen?, freeze,
   and on the IO family (a File/IO/File::Stat handle, a Dir handle) to_s and
   <=> as well -- on the native handle and value kinds that have no arm of
   their own. Reached from the tails of emit_call and emit_case_eq_call (and
   from the MatchData and OpenStruct arms and the case/when emitter, which
   delegate), after every typed arm and the user-method routing declined, so
   there it can never shadow a more specific answer: it turns a front-end
   rejection into the answer Ruby gives. The IO family is the exception: its
   site in emit_call runs early, ahead of the generic spaceship and to_s
   fallbacks that would otherwise claim the call, and like the rest of the IO
   surface it does not consult a user reopening of IO or Dir. WHICH calls it
   answers is decided once, in ty_object_protocol_answers (types.c), which
   infer_call types from as well.

   to_s is Object's #<Class:0xADDR> under the class the handle presents as
   (#inspect stays the handle's own render). <=> is Object's identity answer,
   0 or nil, boxed -- except two File::Stat handles, which Comparable orders by
   modification time, the same reading == takes for them (sp_io_cmp).

   Two semantics, per CRuby. A heap handle (Fiber, Thread, Queue, Mutex,
   ConditionVariable, Dir, Addrinfo, IO, Enumerator, Method, Exception,
   MatchData, a curried Proc) IS its pointer: the predicates are pointer
   identity and frozen? reads the bit freeze sets (the GC header, or the
   handle's own flag for an IO, whose standard streams are static storage).
   Random, OpenStruct, Exception, File::Stat, Time and Process::Tms answer ==
   and === structurally (state, members, class and message, modification
   time, instant, fields); eql? and equal? stay identity where the value has
   one, and OpenStruct's eql? is its table's. Time, Tms and a String range
   are by-value structs: equal? and (bar the Range, always frozen) frozen?
   are not answered for them, and `freeze` keeps the identity no-op it had.

   Evaluation order is Ruby's: the receiver first, into a rooted temp, then
   the operand, then the test -- an allocating operand must not collect a
   receiver held only in the expression. A poly operand is unwrapped in
   place: same tag, same builtin id, then the pointer or structural test. A
   kind with no builtin id (Random) has no boxed form at all, so a poly slot
   never holds one and the answer is false. */
static void emit_native_object_protocol_text(Compiler *c, const char *name, TyKind rt,
                                             const char *r, TyKind at, const char *a, Buf *b) {
  if (sp_streq(name, "to_s")) {
    buf_printf(b, "%s(%s)", rt == TY_IO ? "sp_io_to_s" : "sp_Dir_to_s", r);
    return;
  }
  Buf ct; memset(&ct, 0, sizeof ct); emit_ctype(c, rt, &ct);
  const char *cty = ct.p ? ct.p : "void *";
  if (sp_streq(name, "<=>")) {
    /* a value of another static kind is never the same object and cannot be
       a stat, and a NULL handle is nil, which is not it either: nil, with
       both sides evaluated for their effects and nothing boxed (a Random has
       no boxed form at all) */
    if (at != rt && at != TY_NIL && at != TY_POLY && at != TY_UNKNOWN) {
      buf_printf(b, "((void)(%s), (void)(%s), sp_box_nil())", r, a);
      free(ct.p);
      return;
    }
    /* receiver first, rooted across the operand, which may allocate; then one
       runtime reading of the operand as a boxed value: a same-kind operand
       and nil are boxed, a poly one already is */
    int t = ++g_tmp;
    Buf ab; memset(&ab, 0, sizeof ab);
    if (at == rt || at == TY_NIL) emit_boxed_text(c, at, a, &ab);
    else buf_puts(&ab, a);
    buf_printf(b, "({ %s _t%d = %s; SP_GC_ROOT(_t%d); sp_RbVal _u%d = %s; %s(_t%d, _u%d); })",
               cty, t, r, t, t, ab.p ? ab.p : a, rt == TY_IO ? "sp_io_cmp" : "sp_Dir_cmp", t, t);
    free(ab.p);
    free(ct.p);
    return;
  }
  int kind = ty_object_protocol_kind(rt);
  if (sp_streq(name, "frozen?")) {
    if (rt == TY_IO) buf_printf(b, "sp_io_frozen(%s)", r);
    else if (kind == 1) {
      int t = ++g_tmp;
      buf_printf(b, "({ %s _t%d = %s; _t%d ? sp_gc_is_frozen((void *)_t%d) : (sp_bool)1; })", cty, t, r, t, t);
    }
    else buf_printf(b, "((void)(%s), (sp_bool)1)", r);
    free(ct.p);
    return;
  }
  if (sp_streq(name, "freeze")) {
    if (rt == TY_IO) buf_printf(b, "sp_io_freeze(%s)", r);
    else buf_printf(b, "((%s)sp_gc_freeze((void *)(%s)))", cty, r);
    free(ct.p);
    return;
  }
  int is_ne = sp_streq(name, "!=");
  int is_eql = sp_streq(name, "eql?");
  int is_equal = sp_streq(name, "equal?");
  /* the cross-family tier: a Range, Array or Bignum against another family */
  if (kind == 0) {
    if (ty_is_array(rt) && ty_is_array(at)) {
      Buf rb; memset(&rb, 0, sizeof rb); emit_boxed_text(c, rt, r, &rb);
      Buf ab; memset(&ab, 0, sizeof ab); emit_boxed_text(c, at, a, &ab);
      buf_printf(b, "(%s%s(%s, %s))", is_ne ? "!" : "", is_eql ? "sp_poly_eql" : "sp_poly_eq",
                 rb.p ? rb.p : r, ab.p ? ab.p : a);
      free(rb.p); free(ab.p);
    }
    else buf_printf(b, "((void)(%s), (void)(%s), (sp_bool)%d)", r, a, is_ne ? 1 : 0);
    free(ct.p);
    return;
  }
  /* which comparisons look inside the value rather than at its address */
  const char *fn = NULL;
  if (rt == TY_RANDOM && !is_eql && !is_equal) fn = "sp_Random_eq";
  else if (rt == TY_OPENSTRUCT && !is_equal) fn = is_eql ? "sp_OpenStruct_eql" : "sp_OpenStruct_eq";
  else if (rt == TY_EXCEPTION && !is_eql && !is_equal) fn = "sp_exc_eq";
  else if (rt == TY_IO && !is_eql && !is_equal) fn = "sp_io_eq";
  else if (rt == TY_MATCHDATA && !is_eql && !is_equal) fn = "sp_MatchData_eq";
  int t = ++g_tmp;
  /* receiver, then operand, into temps; then the test (negated for !=) */
  buf_printf(b, "({ %s _t%d = %s; ", cty, t, r);
  if (kind == 1) buf_printf(b, "SP_GC_ROOT(_t%d); ", t);
  Buf test; memset(&test, 0, sizeof test);
  if (at == rt) {
    buf_printf(b, "%s _u%d = %s; ", cty, t, a);
    if (kind == 2 && rt == TY_TMS)
      buf_printf(&test, "(_t%d.utime == _u%d.utime && _t%d.stime == _u%d.stime && "
                        "_t%d.cutime == _u%d.cutime && _t%d.cstime == _u%d.cstime)", t, t, t, t, t, t, t, t);
    else if (kind == 2 && rt == TY_TIME) buf_printf(&test, "(sp_time_cmp(_t%d, _u%d) == 0)", t, t);
    else if (kind == 2) buf_printf(&test, "sp_srange_eq(_t%d, _u%d)", t, t);
    else if (fn) buf_printf(&test, "%s(_t%d, _u%d)", fn, t, t);
    else buf_printf(&test, "(_t%d == _u%d)", t, t);
  }
  else if (at == TY_POLY && kind == 2) {
    Buf rb; memset(&rb, 0, sizeof rb);
    char tref[24]; snprintf(tref, sizeof tref, "_t%d", t);
    emit_boxed_text(c, rt, tref, &rb);
    buf_printf(b, "sp_RbVal _u%d = %s; ", t, a);
    buf_printf(&test, "%s(%s, _u%d)", is_eql ? "sp_poly_eql" : "sp_poly_eq", rb.p ? rb.p : tref, t);
    free(rb.p);
  }
  else if (at == TY_POLY) {
    const char *bid = ty_nullable_builtin_id(rt);
    buf_printf(b, "sp_RbVal _u%d = %s; ", t, a);
    if (!bid) buf_printf(&test, "((void)_u%d, 0)", t);
    else {
      buf_printf(&test, "(_u%d.tag == SP_TAG_OBJ && _u%d.cls_id == %s && ", t, t, bid);
      if (fn) buf_printf(&test, "%s(_t%d, (%s)_u%d.v.p))", fn, t, cty, t);
      else buf_printf(&test, "_u%d.v.p == (void *)_t%d)", t, t);
    }
  }
  else if (at == TY_NIL && kind == 1) {
    /* A pointer-backed handle IS nil when it is NULL in this backend: that is
       what `nil?` and the dedicated `handle != nil` arm both answer. `== nil`
       reached the other-kind arm below and folded to false, so a slot holding
       nil answered false to `== nil` and false to `!= nil` at the same time
       (an empty pipe's `wait_readable(0)`). */
    buf_printf(b, "(void)(%s); ", a);
    buf_printf(&test, "(_t%d == NULL)", t);
  }
  else {
    /* a value of another static kind is never the same object, nor equal */
    buf_printf(b, "(void)(%s); ", a);
    buf_puts(&test, "0");
  }
  buf_printf(b, "(sp_bool)%s(%s); })", is_ne ? "!" : "", test.p ? test.p : "0");
  free(test.p);
  free(ct.p);
}

int emit_native_object_protocol(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !name || nt_ref(nt, id, "block") >= 0) return 0;
  int argc;
  const int *argv = call_args(nt, id, &argc);
  TyKind rt = comp_recv_type(c, recv);
  TyKind at = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  if (!ty_object_protocol_answers(rt, at, name, argc)) return 0;
  /* a rescued value is typed TY_EXCEPTION, not as its user class: a user
     exception subclass defining the method keeps it */
  if (rt == TY_EXCEPTION && exc_subclass_defines(c, name)) return 0;
  Buf rs = expr_buf(c, recv);
  Buf as; memset(&as, 0, sizeof as);
  if (argc == 1) as = expr_buf(c, argv[0]);
  emit_native_object_protocol_text(c, name, rt, rs.p ? rs.p : "0", at, as.p ? as.p : "0", b);
  free(rs.p); free(as.p);
  return 1;
}

/* `case subj when cond`: Ruby asks `cond === subj`. The case/when emitter
   calls this before its raw C `==` fallback so a native kind answers the
   same way there as an explicit `===` does (subj_ref is the subject's temp). */
int emit_native_case_eq(Compiler *c, int cond, TyKind subj_t, const char *subj_ref, Buf *b) {
  TyKind ct = comp_ntype(c, cond);
  if (!ty_object_protocol_answers(ct, subj_t, "===", 1)) return 0;
  if (ct == TY_EXCEPTION && exc_subclass_defines(c, "===")) return 0;
  Buf cs = expr_buf(c, cond);
  emit_native_object_protocol_text(c, "===", ct, cs.p ? cs.p : "0", subj_t, subj_ref, b);
  free(cs.p);
  return 1;
}
