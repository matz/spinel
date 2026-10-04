/* codegen_call_class.c -- emit_call_body's arms of Class and Module: reflection, constructors, class methods.
   Moved from emit_call_body (codegen_call.c) unchanged: each function holds
   a run of its arms, in their order, and answers 1 when one emitted the call
   (codegen_call_arms.h). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"
#include "codegen_call_arms.h"

static void emit_attr_writer_converted(Compiler *c, int arg, TyKind ivt, int tmp,
                                       const char *name, Buf *b) {
  TyKind avk = store_value_kind(c, arg);
  /* A nil-valued argument has no C type for a temporary. Keep its
     effects, store the slot's nil, and answer nil as the writer
     does rather than reading the slot as an ordinary number. */
  if (avk == TY_NIL || avk == TY_VOID) {
    buf_printf(b, "_t%d->iv_%s = ", tmp, iv_c(name));
    emit_coerce(c, arg, ivt, CO_HOLD, "an attribute writer", b);
    buf_puts(b, "; 0; })");
    return;
  }
  int tv = ++g_tmp;
  char tn[32]; snprintf(tn, sizeof tn, "_t%d", tv);
  emit_ctype(c, avk, b); buf_printf(b, " %s = ", tn); emit_expr(c, arg, b);
  buf_printf(b, "; _t%d->iv_%s = ", tmp, iv_c(name));
  emit_coerce_text(c, arg, avk, ivt, CO_HOLD, tn, "an attribute writer", b);
  buf_printf(b, "; %s; })", tn);
  return;
}

/* respond_to?, method_defined? and its kin, const_set / const_get / const_defined? */
int emit_call_reflection_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt) {
  if (sp_streq(name, "respond_to?") && argc >= 1 && !respond_to_user_defined(c, id, recv)) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) {
      qm = nt_str(nt, argv[0], "content");
      if (!qm) qm = nt_str(nt, argv[0], "unescaped");
    }
    /* an exception answers a name a builtin exception's reopening defines
       only when its runtime class is under that reopening: asked at run
       time, the builtin surface beside it */
    int xr[8], xn = 0;
    if (qm && recv >= 0 && rt == TY_EXCEPTION && argc == 1) xn = exc_reopen_definers(c, qm, xr, 8);
    if (xn > 0) {
      int xt = ++g_tmp;
      buf_printf(b, "({ sp_Exception *_t%d = (sp_Exception *)(", xt);
      emit_expr(c, recv, b);
      buf_puts(b, "); ");
      char xcls[64]; snprintf(xcls, sizeof xcls, "(_t%d ? _t%d->cls_name : NULL)", xt, xt);
      int pk = emit_exc_reopen_pick_head(c, xr, xn, xcls, b);
      buf_printf(b, "(_xi%d >= 0 || (_t%d && sp_poly_responds_builtin(", pk, xt);
      char xv[32]; snprintf(xv, sizeof xv, "_t%d", xt);
      emit_boxed_text(c, TY_EXCEPTION, xv, b);
      buf_puts(b, ", \"");
      emit_c_escaped(b, qm);
      buf_puts(b, "\"))); })");
      return 1;
    }
    if (qm) {
      int ans = respond_to_static_answer(c, id, recv, rt, qm, argc, argv);
      if (ans >= 0 && g_rto_nil_obj) {
        int nil_too = nil_answers_name(qm) || sp_streq(qm, "rationalize");
        if (ans && !nil_too) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") != NULL)"); return 1; }
        if (!ans && nil_too) { buf_puts(b, "(("); emit_expr(c, recv, b); buf_puts(b, ") == NULL)"); return 1; }
      }
      /* the answer is the slot kind's; a nullable Integer or Float holding
         its sentinel is nil, which answers its own, smaller surface */
      if (ans >= 0 && recv >= 0 && (rt == TY_INT || rt == TY_FLOAT) &&
          call_returns_nullable_int(c, recv)) {
        char ref[24];
        buf_puts(b, "({ "); emit_sentinel_bind(c, rt, recv, ref, sizeof ref, b);
        emit_slot_truthy(rt, ref, b);
        buf_printf(b, " ? %d : sp_poly_responds_builtin(sp_box_nil(), ", ans);
        emit_str_literal(b, qm);
        buf_puts(b, "); })");
        return 1;
      }
      /* boxed when the call is typed so ($stderr, a global, is folded here
         as the IO it holds, where the analysis typed the call poly) */
      if (ans >= 0) {
        buf_printf(b, comp_ntype(c, id) == TY_POLY ? "sp_box_bool(%d)" : "%d", ans);
        return 1;
      }
      /* the runtime answers: a Range value's builtin surface (the probe has
         no reading for it, #3619), an IO (the handle knows its kind), and a
         poly receiver */
      if (recv >= 0 && (rt == TY_RANGE || rt == TY_FLOAT_RANGE || rt == TY_STR_RANGE || rt == TY_IO)) {
        int tv = ++g_tmp;
        if (comp_ntype(c, id) == TY_POLY) buf_puts(b, "sp_box_bool(");
        buf_printf(b, "({ sp_RbVal _t%d = ", tv);
        emit_boxed(c, recv, b);
        buf_printf(b, "; %s(_t%d, \"", rt == TY_IO ? "sp_io_typed_responds" : "sp_poly_responds_builtin", tv);
        emit_c_escaped(b, qm);
        buf_puts(b, "\")");
        /* a private one too when the literal second argument is true */
        if (rt == TY_IO)
          emit_io_reopen_responds(c, tv, qm, argc >= 2 && nt_kind(nt, argv[1]) == NK_TrueNode, b);
        buf_puts(b, "; })");
        if (comp_ntype(c, id) == TY_POLY) buf_puts(b, ")");
        return 1;
      }
      if (recv >= 0) {
        if (0) { }
        else if ((rt == TY_CLASS && class_value_responds(c, 0, qm, NULL)) ||
                 ((rt == TY_POLY || rt == TY_UNKNOWN) && !any_class_responds(c, qm))) {
          int tv = class_value_responds(c, 0, qm, NULL) ? ++g_tmp : 0;
          if (tv) { buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_boxed(c, recv, b); buf_printf(b, "; sp_poly_responds_builtin(_t%d", tv); }
          else { buf_puts(b, "sp_poly_responds_builtin("); emit_boxed(c, recv, b); }
          buf_puts(b, ", \"");
          emit_c_escaped(b, qm);
          buf_puts(b, "\")");
          if (tv) { class_value_responds(c, tv, qm, b); buf_puts(b, "; })"); }
          return 1;
        }
        else if ((rt == TY_POLY || rt == TY_UNKNOWN) && any_class_responds(c, qm)) {
          /* poly receiver + a user protocol method (some user class defines qm):
             the analyze probe answers "SOME union member responds", which
             mis-decides per value -- a String member of String|UserClass would
             report true for the user's method and take the wrong branch. Emit a
             runtime check against the exact user classes that define qm; a
             builtin value (which cannot define a user protocol method) answers
             false. This mirrors the poly is_a? runtime cls_id check. */
          int tv = ++g_tmp;
          buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_expr(c, recv, b); buf_puts(b, "; ");
          /* The value's class as the poly dispatch keys it: a boxed builtin a
             reopening extended -- a Time is an OBJ box naming SP_BUILTIN_TIME,
             a String the string tag -- maps to the reopening's entry, where
             the plain cls_id never did (activesupport's Object#acts_like?
             asking a Time for acts_like_time? read false). A root class's
             method (a `class Object` reopening) is every value's. */
          buf_printf(b, "sp_int _k%d = ", tv);
          emit_poly_dispatch_key(c, tv, 1, 1, poly_exc_cand(c, qm), b);
          buf_puts(b, "; (");
          size_t ql = strlen(qm);
          char wbase[256]; wbase[0] = '\0';
          int is_wr = ql > 0 && qm[ql - 1] == '=' && ql - 1 < sizeof wbase;
          if (is_wr) { memcpy(wbase, qm, ql - 1); wbase[ql - 1] = '\0'; }
          int first = 1;
          for (int k = 0; k < c->nclasses; k++) {
            if (comp_is_undeffed_in_chain(c, k, qm)) continue;
            int has = (comp_method_in_chain(c, k, qm, NULL) >= 0 &&
                       !method_hidden_from_reflection(c, k, qm)) ||
                      comp_reader_in_chain(c, k, qm, NULL) ||
                      (is_wr && comp_writer_in_chain(c, k, wbase, NULL)) ||
                      (c->classes[k].is_native_class &&
                       comp_native_method_find(c, k, qm, 0, 0) >= 0) ||
                      /* the names the class carries without a method entry
                         answer as they do on the typed receiver */
                      class_implicit_responds(c, k, qm);
            /* an exception class with no method of its own for the name
               answers through a builtin reopening its runtime class is under */
            int xr[8], xn = 0;
            if (!has && (class_is_exc_subclass(c, k) || class_is_exc_reopen(c, k))) xn = exc_reopen_definers(c, qm, xr, 8);
            if (!has && xn <= 0) continue;
            const char *kn = c->classes[k].name;
            int root = kn && is_object_root(kn);
            if (root) buf_printf(b, "%s1", first ? "" : " || ");
            else if (!has) {
              buf_printf(b, "%s(_k%d == %d && (", first ? "" : " || ", tv, k);
              for (int q = 0; q < xn; q++)
                buf_printf(b, "%ssp_exc_cls_matches(((sp_Exception *)_t%d.v.p)->cls_name, \"%s\")",
                           q ? " || " : "", tv, c->classes[xr[q]].name);
              buf_puts(b, "))");
            }
            else buf_printf(b, "%s_k%d == %d", first ? "" : " || ", tv, k);
            first = 0;
          }
          if (first) buf_puts(b, "0");
          /* a builtin member of the union cannot carry a user method, but it
             does have its own surface: Array really responds to :each. Ask
             the runtime rather than answering a flat false here (#3072). */
          buf_printf(b, ") || sp_poly_responds_builtin(_t%d, ", tv);
          buf_puts(b, "\"");
          emit_c_escaped(b, qm);
          buf_puts(b, "\")");
          emit_io_reopen_responds(c, tv, qm, argc >= 2 && nt_kind(nt, argv[1]) == NK_TrueNode, b);
          class_value_responds(c, tv, qm, b);
          buf_puts(b, "; })");
          return 1;
        }
      }
      /* a class value the probe answers for: nil (the slot's own nil) responds
         to nothing, so the answer is the value's non-nilness */
      if (recv >= 0 && rt == TY_CLASS) {
        int pyes = 0;
        if (rt_probe_answer(c, id, &pyes) && pyes) {
          buf_puts(b, "!sp_class_nil_p("); emit_expr(c, recv, b); buf_puts(b, ")"); return 1;
        }
      }
    }
    else if (recv >= 0 && rt != TY_CLASS && nt_kind(nt, argv[0]) != NK_SplatNode &&
             /* an override answers only for its own class's objects (and a
                boxed value, which dispatches) */
             (!any_class_defines(c, "respond_to?") || g_poly_builtin_arm ||
              !(rt == TY_POLY ||
                (ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), "respond_to?", NULL) >= 0)))) {
      int tv = ++g_tmp;
      /* boxed when the call is typed so (an IO's is, see the fold above) */
      int boxed = comp_ntype(c, id) == TY_POLY;
      if (boxed) buf_puts(b, "sp_box_bool(");
      buf_printf(b, "({ sp_RbVal _t%d = ", tv); emit_boxed(c, recv, b);
      buf_printf(b, "; const char *_n%d = sp_poly_to_name(", tv); emit_boxed(c, argv[0], b);
      buf_printf(b, "); sp_bool _a%d = ", tv);
      if (argc >= 2) emit_cond(c, argv[1], b); else buf_puts(b, "0");
      /* the value's class as the poly dispatch keys it: a boxed builtin a
         reopening extended (a Time is an OBJ box naming SP_BUILTIN_TIME, a
         String the string tag) maps to the reopening's entry, where the
         plain cls_id never matched it */
      buf_printf(b, "; sp_int _k%d = ", tv);
      emit_poly_dispatch_key(c, tv, 1, 1, 0, b);
      buf_puts(b, "; ((");
      for (int k = 0; k < c->nclasses; k++) {
        if (comp_class_is_module(c, &c->classes[k])) continue;
        buf_printf(b, "(_k%d == %d && (", tv, k);
        for (int s = 0; s < c->nscopes; s++)
          if (c->scopes[s].name && !c->scopes[s].is_cmethod && !c->scopes[s].is_proc_form &&
              comp_method_in_chain(c, k, c->scopes[s].name, NULL) == s)
            emit_responds_name(c, k, c->scopes[s].name, tv, b);
        for (int m = 0; m < c->n_native_methods; m++)
          if (c->native_methods[m].class_id == k && !c->native_methods[m].kind)
            emit_responds_name(c, k, c->native_methods[m].name, tv, b);
        for (int p = k; p >= 0; p = c->classes[p].parent) {
          ClassInfo *cl = &c->classes[p];
          for (int r = 0; r < cl->nreaders; r++) emit_responds_name(c, k, cl->readers[r], tv, b);
          for (int w = 0; w < cl->nwriters; w++) {
            char wn[256];
            snprintf(wn, sizeof wn, "%s=", cl->writers[w]);
            emit_responds_name(c, k, wn, tv, b);
          }
          for (int a = 0; a < cl->naliases; a++) emit_responds_name(c, k, cl->alias_new[a], tv, b);
        }
        const char *const *imp[] = { enumerable_names, comparable_names, struct_names,
                                     (const char *const[]){ "members", "to_h", "deconstruct", "deconstruct_keys", "with", NULL } };
        for (int l = 0; l < 4; l++)
          for (int i = 0; imp[l][i]; i++)
            if (class_implicit_responds(c, k, imp[l][i])) emit_responds_name(c, k, imp[l][i], tv, b);
        buf_puts(b, "0)) || ");
      }
      buf_printf(b, "0)) || (_a%d && (!strcmp(_n%d, \"initialize\") || !strcmp(_n%d, \"initialize_copy\"))) || "
                 "sp_poly_responds_builtin(_t%d, _n%d)", tv, tv, tv, tv, tv);
      /* a boxed IO: the methods IO reopenings add, by the handle's kind */
      for (int s2 = 0; s2 < c->nscopes; s2++) {
        Scope *ms = &c->scopes[s2];
        if (!ms->name || ms->is_cmethod || ms->is_proc_form || ms->class_id < 0) continue;
        if (!io_family_class(c, ms->class_id)) continue;
        int dup = 0;
        for (int s3 = 0; s3 < s2 && !dup; s3++)
          dup = c->scopes[s3].name && !c->scopes[s3].is_cmethod && c->scopes[s3].class_id >= 0 &&
                io_family_class(c, c->scopes[s3].class_id) && sp_streq(c->scopes[s3].name, ms->name);
        if (dup) continue;
        /* a private one too when the second argument is true (_a) */
        Buf ab;
        memset(&ab, 0, sizeof ab);
        emit_io_pick(c, tv, ms->name, 0, &ab);
        int pt = ++g_tmp;
        char pn[24]; snprintf(pn, sizeof pn, "_p%d", pt);
        buf_printf(b, " || (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO && !strcmp(_n%d, \"", tv, tv, tv);
        emit_c_escaped(b, ms->name);
        buf_printf(b, "\") && ({ int %s = %s; %s != 0x7fffffff && (_a%d || ", pn, ab.p, pn, tv);
        emit_io_pick_public(c, ms->name, pn, b);
        buf_puts(b, "); }))");
        free(ab.p);
      }
      buf_puts(b, "; })");
      if (boxed) buf_puts(b, ")");
      return 1;
    }
  }

  /* Class.{,public_,private_,protected_}method_defined?(:m[, inherit]):
     compile-time decided from the class's recorded method table (instance
     methods + attr readers/writers) filtered by visibility. `method_defined?`
     matches public+protected (not private); the prefixed forms match exactly
     one visibility. inherit=false restricts the lookup to own definitions. */
  int md_pub = 0, md_prot = 0, md_priv = 0, md_family = 1;
  if (sp_streq(name, "method_defined?"))              { md_pub = 1; md_prot = 1; }
  else if (sp_streq(name, "public_method_defined?"))    { md_pub = 1; }
  else if (sp_streq(name, "protected_method_defined?")) { md_prot = 1; }
  else if (sp_streq(name, "private_method_defined?"))   { md_priv = 1; }
  else md_family = 0;
  /* a constant path receiver (`CGI::EscapeExt.method_defined?`, cgi/escape.rb)
     names its class by the leaf, as constant paths resolve here */
  if (md_family && recv >= 0 && argc >= 1 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (qm && ci >= 0) {
      int inherit = 1;
      if (argc >= 2) {
        const char *it = nt_type(nt, argv[1]);
        if (it && sp_streq(it, "FalseNode")) inherit = 0;
      }
      /* a writer query (`m=`) consults the writer table under its base name */
      size_t ln = strlen(qm);
      int is_setter = ln > 0 && qm[ln - 1] == '=';
      char base[256];
      base[0] = '\0';
      if (is_setter && ln - 1 < sizeof base) { memcpy(base, qm, ln - 1); base[ln - 1] = '\0'; }
      /* Ruby makes the initialize family private on every class, defined or
         not: private_method_defined? is true for all of them and the public /
         protected / plain queries are false (#3874). */
      if (sp_streq(qm, "initialize") || sp_streq(qm, "initialize_copy") ||
          sp_streq(qm, "initialize_clone") || sp_streq(qm, "initialize_dup")) {
        buf_printf(b, "%d", md_priv ? 1 : 0);
        return 1;
      }
      if (name_is_synth_method(c, qm) || comp_is_undeffed_in_chain(c, ci, qm)) { buf_puts(b, "FALSE"); return 1; }
      int parent = c->classes[ci].parent;
      int mc = -1;
      int mi = comp_method_in_chain(c, ci, qm, &mc);
      /* a generated Struct iterator is Struct's or Enumerable's, never the
         class's own, and a Data class has none */
      if (mi >= 0 && scope_is_struct_synth(c, mi) &&
          (!inherit || class_struct_kind(c, ci) == 2)) mi = -1;
      int found;
      if (inherit) {
        found = mi >= 0 || comp_reader_in_chain(c, ci, qm, NULL) ||
                (is_setter && comp_writer_in_chain(c, ci, base, NULL));
      }
      else {
        /* attr readers/writers are flattened into descendants at analyze
           time, so "own" means present here but not in the parent chain */
        int rd_own = comp_is_reader(&c->classes[ci], qm) &&
                     (parent < 0 || !comp_reader_in_chain(c, parent, qm, NULL)) &&
                     !refl_member_of_struct_superclass(c, ci, qm);
        int wr_own = is_setter && comp_is_writer(&c->classes[ci], base) &&
                     (parent < 0 || !comp_writer_in_chain(c, parent, base, NULL)) &&
                     !refl_member_of_struct_superclass(c, ci, base);
        /* a method copied in from an included module is the module's */
        found = (mi >= 0 && mc == ci && c->scopes[mi].origin_module_ci == 0) || rd_own || wr_own;
      }
      /* Methods inherited from Object/Kernel are defined on every class. With
         `inherit` (the default) method_defined? must report the public ones as
         true even though they have no entry in the user class chain (#2673). */
      if (!found && inherit && md_pub) {
        for (int u = 0; respond_universal[u]; u++)
          if (sp_streq(qm, respond_universal[u])) { buf_puts(b, "1"); return 1; }
      }
      int yes = 0;
      if (found) {
        int v = inherit ? comp_method_vis_in_chain(c, ci, qm)
                        : comp_method_vis(&c->classes[ci], qm);
        yes = (v == SP_VIS_PUBLIC && md_pub) || (v == SP_VIS_PROTECTED && md_prot) ||
              (v == SP_VIS_PRIVATE && md_priv);
      }
      buf_printf(b, "%d", yes);
      return 1;
    }
  }

  /* The fully dynamic form (class held in a variable, or a non-literal method
     name) cannot be answered ahead of time: there is no runtime reflection
     table, and builtin classes have no enumerable method set. Emit a specific
     diagnostic rather than a generic unsupported-call node dump. Covers both an
     explicit receiver and an implicit-self call (recv < 0). */
  if (md_family) {
    unsupported(c, id, "method_defined? (needs a compile-time-known class and literal method name)");
    return 1;
  }

  /* Class.const_set(:K, v) with a literal name: a constant is a C global
     (cst_<name>) assigned at its definition site, so re-assigning an EXISTING
     one is just that store. The constant must already be defined -- a name the
     program never writes has no global to store into, and its type is what the
     definition inferred, so a value of another type has nowhere to go; both
     fall through to the diagnostic. CRuby returns the value (and warns about
     the reinitialization; spinel does not). #2675 */
  if (sp_streq(name, "const_set") && recv >= 0 && argc == 2) {
    const char *cs_aty = nt_type(nt, argv[0]);
    const char *cs_qm = NULL;
    if (cs_aty && sp_streq(cs_aty, "SymbolNode")) cs_qm = nt_str(nt, argv[0], "value");
    else if (cs_aty && sp_streq(cs_aty, "StringNode")) cs_qm = nt_str(nt, argv[0], "content");
    if (cs_qm) {
      LocalVar *cv = comp_const(c, cs_qm);
      if (cv && cv->type != TY_UNKNOWN && comp_ntype(c, argv[1]) == cv->type) {
        buf_printf(b, "(cst_%s = ", cs_qm);
        emit_expr(c, argv[1], b);
        buf_printf(b, ", cst_%s)", cs_qm);
        return 1;
      }
      static char csbuf[512];
      if (!cv || cv->type == TY_UNKNOWN)
        snprintf(csbuf, sizeof csbuf,
                 "const_set(:%s, ...) can only re-assign a constant the program already "
                 "defines: a name never written has no storage to set, because constants "
                 "are compile-time globals. Declare `%s = ...` first "
                 "(see docs/limitations.md)", cs_qm, cs_qm);
      else
        snprintf(csbuf, sizeof csbuf,
                 "const_set(:%s, ...) cannot change the constant's type: %s was inferred as "
                 "%s at its definition and is a C global of that type, so a %s value has "
                 "nowhere to go (see docs/limitations.md)", cs_qm, cs_qm,
                 ty_name(cv->type), ty_name(comp_ntype(c, argv[1])));
      unsupported_feature(c, id, csbuf);
    }
  }
  /* Class.const_get(:K) with a literal name: constants live in a flat namespace
     (cst_<name>), so resolve it like a ConstantRead. A literal name that does not
     resolve raises NameError at runtime, matching CRuby: "uninitialized constant
     <Name>" for a valid constant name, "wrong constant name <name>" for one that
     is not (no leading uppercase). A dynamic name can't be resolved ahead of time
     and is diagnosed. */
  if (sp_streq(name, "const_get") && recv >= 0 && argc >= 1) {
    const char *cg_aty = nt_type(nt, argv[0]);
    const char *cg_qm = NULL;
    if (cg_aty && sp_streq(cg_aty, "SymbolNode")) cg_qm = nt_str(nt, argv[0], "value");
    else if (cg_aty && sp_streq(cg_aty, "StringNode")) cg_qm = nt_str(nt, argv[0], "content");
    /* const_get(name, false) searches only the receiver's own constants */
    int cg_own = 0;
    if (cg_qm && argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "FalseNode")) {
      const char *cg_rty = nt_type(nt, recv);
      const char *cg_rnm = (cg_rty && (sp_streq(cg_rty, "ConstantReadNode") ||
                                       sp_streq(cg_rty, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
      if (cg_rnm && !const_owned_by_class(c, cg_rnm, cg_qm)) cg_own = 1;
    }
    if (cg_qm && !cg_own) {
      LocalVar *cv = comp_const(c, cg_qm);
      if (cv && cv->type != TY_UNKNOWN) { buf_printf(b, "cst_%s", cg_qm); return 1; }
      /* A CLASS or module name: const_get answers the class object. The lookup
         above knows only VALUE constants, so `Object.const_get(:Foo)` on a
         class the program defines fell through to the NameError below (#3969). */
      { int cgc = comp_class_index(c, cg_qm);
        if (cgc >= 0) {
          if (comp_ntype(c, id) == TY_CLASS) buf_printf(b, "((sp_Class){%d})", cgc);
          else buf_printf(b, "sp_box_class((sp_Class){%d})", cgc);
          return 1;
        } }
      /* Builtin module constants: Klass.const_get(:C) resolves to the same value
         as Klass::C. const_get's result is poly, so box it (#2685). */
      {
        const char *rvt = nt_type(nt, recv);
        const char *rnm = (rvt && (sp_streq(rvt, "ConstantReadNode") ||
                                   sp_streq(rvt, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
        if (rnm && sp_streq(rnm, "Float")) {
          if (sp_streq(cg_qm, "INFINITY")) { buf_puts(b, "sp_box_float(1.0/0.0)"); return 1; }
          if (sp_streq(cg_qm, "NAN"))      { buf_puts(b, "sp_box_float(0.0/0.0)"); return 1; }
          if (sp_streq(cg_qm, "MAX"))      { buf_puts(b, "sp_box_float(DBL_MAX)"); return 1; }
          if (sp_streq(cg_qm, "MIN"))      { buf_puts(b, "sp_box_float(DBL_MIN)"); return 1; }
          if (sp_streq(cg_qm, "EPSILON"))  { buf_puts(b, "sp_box_float(DBL_EPSILON)"); return 1; }
          if (sp_streq(cg_qm, "DIG"))      { buf_puts(b, "sp_box_int(DBL_DIG)"); return 1; }
          if (sp_streq(cg_qm, "MANT_DIG")) { buf_puts(b, "sp_box_int(DBL_MANT_DIG)"); return 1; }
          if (sp_streq(cg_qm, "RADIX"))    { buf_puts(b, "sp_box_int(FLT_RADIX)"); return 1; }
        }
        if (rnm && sp_streq(rnm, "Math")) {
          if (sp_streq(cg_qm, "PI")) { buf_puts(b, "sp_box_float(M_PI)"); return 1; }
          if (sp_streq(cg_qm, "E"))  { buf_puts(b, "sp_box_float(M_E)"); return 1; }
        }
      }
      /* literal but unresolved: evaluate the receiver for side effects, then raise.
         CRuby qualifies "uninitialized constant" by a named module receiver
         (M::Missing) but not by Object/top-level; "wrong constant name" is never
         qualified. Qualify when the receiver is a constant other than Object. */
      /* stage the receiver (the module const_get was sent to) so #receiver is
         it, not nil; #name is recovered from the message (#3034) */
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_exc_stage_recv("); emit_boxed(c, recv, b);
      buf_puts(b, "), sp_raise_cls(\"NameError\", ");
      if (cg_qm[0] >= 'A' && cg_qm[0] <= 'Z') {
        /* Qualify by the receiver's full Ruby name when it resolves to a known
           class/module (M, or nested M::N); a builtin like Object resolves to no
           user-class index and stays unqualified, matching CRuby. */
        const char *rcv_ty = nt_type(nt, recv);
        const char *rcv_nm = (rcv_ty && (sp_streq(rcv_ty, "ConstantReadNode") ||
                                         sp_streq(rcv_ty, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
        int rcid = rcv_nm ? comp_class_index(c, rcv_nm) : -1;
        if (rcid >= 0) {
          const char *qn = class_ruby_name(c, rcid); if (!qn) qn = c->classes[rcid].name;
          buf_printf(b, "\"uninitialized constant %s::%s\"", qn, cg_qm);
        }
        else {
          buf_printf(b, "\"uninitialized constant %s\"", cg_qm);
        }
      }
      else {
        buf_printf(b, "\"wrong constant name %s\"", cg_qm);
      }
      buf_puts(b, "), sp_box_nil())");
      return 1;
    }
    if (cg_own) {
      /* the name exists, but not on the receiver itself, and the search was
         asked not to climb (#3762) */
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_puts(b, "), sp_exc_stage_recv("); emit_boxed(c, recv, b);
      buf_puts(b, "), sp_raise_cls(\"NameError\", ");
      {
        const char *cg_rnm2 = nt_str(nt, recv, "name");
        char qb[192];
        snprintf(qb, sizeof qb, "uninitialized constant %s::%s", cg_rnm2 ? cg_rnm2 : "", cg_qm);
        emit_str_literal(b, qb);
      }
      buf_puts(b, "), sp_box_nil())");
      return 1;
    }
    unsupported(c, id, "const_get (needs a compile-time-known constant name)");
    return 1;
  }

  /* Class.const_defined?(:K): compile-time presence check. Constants are
     recorded in a flat namespace, so this consults the global const and class
     tables rather than the receiver's own constants. */
  if (sp_streq(name, "const_defined?") && recv >= 0 && argc >= 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *aty = nt_type(nt, argv[0]);
    const char *qm = NULL;
    if (aty && sp_streq(aty, "SymbolNode")) qm = nt_str(nt, argv[0], "value");
    else if (aty && sp_streq(aty, "StringNode")) qm = nt_str(nt, argv[0], "content");
    if (qm) {
      if (const_name_is_wrong(qm)) {
        buf_printf(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), sp_raise_cls(\"NameError\", sp_sprintf(\"wrong constant name %%s\", ");
        emit_str_literal(b, qm);
        buf_printf(b, ")), 0)");
        return 1;
      }
      int yes = comp_const(c, qm) != NULL || comp_class_index(c, qm) >= 0 || comp_is_wellknown_const(qm);
      /* const_defined?(name, false) restricts the search to the receiver's own
         constants, so an inherited one does not count (#3762) */
      if (yes && argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "FalseNode")) {
        const char *cd_rnm = nt_str(nt, recv, "name");
        if (cd_rnm && !const_owned_by_class(c, cd_rnm, qm)) yes = 0;
      }
      buf_printf(b, "%d", yes);
      return 1;
    }
  }
  return 0;
}

/* a call on a module or a class: native and FFI functions, singleton accessors, a writer in an instance_eval block, class methods */
int emit_call_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* native binding dispatch (Path B): Module.func(...) where Module declared
     native_func. Emit a direct C call to the declared symbol, passing each arg
     in its runtime representation (any -> boxed sp_RbVal, string -> sp_Str*,
     int/float/bool -> the scalar). No FFI boxing. */
  if (recv >= 0) {
    const char *rty_nv = nt_type(nt, recv);
    const char *nvmod = NULL;
    if (rty_nv && (sp_streq(rty_nv, "ConstantReadNode") || sp_streq(rty_nv, "ConstantPathNode")))
      nvmod = nt_str(nt, recv, "name");
    int nvi = nvmod ? comp_native_find(c, nvmod, name) : -1;
    if (nvi >= 0) {
      const char *feat = c->native_funcs[nvi].feat;
      if (!feat || !feat[0] || sp_feature_enabled(feat)) {
        NativeFunc *nf = &c->native_funcs[nvi];
        /* a `cstring` return is a borrowed C string (typically the callee's
           static buffer, e.g. sp_crypto's per-function buffers): the next call
           to the same symbol clobbers it, so dup onto the GC string heap
           before the value escapes into Ruby. */
        int nv_cstr_ret = sp_streq(nf->ret, "cstring");
        /* a `cbinstr` return is the same borrowed buffer, but holding raw
           bytes rather than a C string: dup exactly the count the callee
           published in sp_ffi_bin_len, since strlen would stop at the first
           NUL byte in (say) a binary digest. The result is tagged BINARY --
           declaring this return mode is the callee saying its answer is
           bytes, which is the same thing CRuby says by handing back
           ASCII-8BIT, and an untagged one does not == the same bytes from
           pack or File.binread once any of them is >= 0x80. */
        int nv_bin_ret = sp_streq(nf->ret, "cbinstr");
        int nv_bin_tmp = nv_bin_ret ? ++g_tmp : 0;
        /* JSON.parse's symbolize_names: the option rides a keyword hash the
           1-arg native signature would silently drop. Route the parse
           through the deep key-symbolizer: a literal true wraps directly, a
           dynamic value decides at runtime over a single parse. Other
           options keep the prior ignored-with-string-keys behavior. */
        if (sp_streq(nvmod, "JSON") && sp_streq(name, "parse") && argc == 2 &&
            nt_type(nt, argv[1]) &&
            (sp_streq(nt_type(nt, argv[1]), "KeywordHashNode") ||
             sp_streq(nt_type(nt, argv[1]), "HashNode"))) {
          int symv = struct_kwarg_value(c, argv[1], "symbolize_names");
          if (symv >= 0) {
            const char *svty = nt_type(nt, symv);
            int lit_true  = svty && sp_streq(svty, "TrueNode");
            int lit_false = svty && (sp_streq(svty, "FalseNode") || sp_streq(svty, "NilNode"));
            if (lit_true) { buf_printf(b, "sp_json_symbolize(%s(", nf->csym); emit_str_expr(c, argv[0], b); buf_puts(b, "))"); return 1; }
            if (!lit_false) {
              int tj = ++g_tmp;
              buf_printf(b, "({ sp_RbVal _t%d = %s(", tj, nf->csym); emit_str_expr(c, argv[0], b);
              buf_puts(b, "); (");
              emit_cond(c, symv, b);
              buf_printf(b, ") ? sp_json_symbolize(_t%d) : _t%d; })", tj, tj);
              return 1;
            }
            /* literal false/nil: fall through to the plain 1-arg call */
          }
        }
        if (nv_cstr_ret) buf_puts(b, "sp_str_dup_external(");
        /* Sequence the call before the sp_ffi_bin_len read: C leaves argument
           evaluation order unspecified, so bind the result to a temp first. */
        if (nv_bin_ret) buf_printf(b, "({ const char *_t%d = ", nv_bin_tmp);
        buf_puts(b, nf->csym); buf_puts(b, "(");
        for (int ai = 0; ai < nf->nargs && ai < argc; ai++) {
          if (ai) buf_puts(b, ", ");
          const char *spec = nf->args[ai];
          /* the typed-slot emitters carry the implicit conversion protocol
             (poly unboxing, #to_str / #to_int, nil / bool TypeError):
             Base64.encode64(nil) answered "" and JSON.parse(nil) parsed "" */
          if (sp_streq(spec, "any")) emit_boxed(c, argv[ai], b);
          else if (sp_streq(spec, "string")) emit_str_expr(c, argv[ai], b);
          else if (sp_streq(spec, "int")) emit_int_expr(c, argv[ai], b);
          else emit_expr(c, argv[ai], b);
        }
        buf_puts(b, ")");
        if (nv_cstr_ret) buf_puts(b, ")");
        if (nv_bin_ret)
          buf_printf(b, "; sp_str_as_binary(sp_str_from_bytes(_t%d, (size_t)(sp_ffi_bin_len < 0 ? 0 : sp_ffi_bin_len))); })",
                     nv_bin_tmp);
        return 1;
      }
    }
  }

  /* FFI call dispatch: Module.func(...) where Module declared ffi_func */
  if (recv >= 0) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (rty_ffi && sp_streq(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (rty_ffi && sp_streq(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      int fi = -1;
      for (int ffi_i = 0; ffi_i < c->n_ffi_funcs; ffi_i++)
        if (sp_streq(c->ffi_funcs[ffi_i].mod, rcmod) && sp_streq(c->ffi_funcs[ffi_i].name, name)) {
          fi = ffi_i; break;
        }
      if (fi >= 0) {
        const char *ret_spec = c->ffi_funcs[fi].ret;
        int is_void_ret = sp_streq(ret_spec, "void");
        int is_ptr_ret  = sp_streq(ret_spec, "ptr");
        int is_str_ret  = sp_streq(ret_spec, "str");
        int is_binstr_ret = sp_streq(ret_spec, "binstr");
        int call_argc = c->ffi_funcs[fi].nargs;
        /* A function taking an ffi_callback is called through its own extern
           like any other (codegen.c); the callback may run Ruby, so it is
           never a blocking call. */
        int takes_cb = 0;
        for (int hi = 0; hi < call_argc; hi++)
          if (ffi_find_callback(c, rcmod, c->ffi_funcs[fi].args[hi]) >= 0) { takes_cb = 1; break; }
        /* A trailing :varargs spec: the declared specs cover only the fixed
           leading args; every extra actual arg is passed through with C's
           default argument promotions. A variadic function with fixed args
           has its own `...` extern under the private name (codegen.c); one
           with none has no extern, and the call casts the header-declared symbol to a variadic function pointer --
           `((ret (*)(...))name)`. Neither carries a `format` attribute, so gcc
           does not format-check the call. */
        int is_vararg = call_argc > 0 && sp_streq(c->ffi_funcs[fi].args[call_argc - 1], "varargs");
        int fixed_argc = is_vararg ? call_argc - 1 : call_argc;
        /* `blocking: true`: the arguments are evaluated into temps first (they
           may allocate, or run Ruby), then the worker leaves the world for
           the call itself and comes back for the return. A callback-taking
           or variadic function keeps the plain call. */
        int blocking = c->ffi_funcs[fi].blocking && !takes_cb && !is_vararg;
        /* An IO::Buffer in a pointer slot hands C its base address. The
           buffer is evaluated in argument order into a rooted temp, and its
           base is taken once every argument has run (a later one may resize
           it), so a call carrying one takes the temp form too. A boxed
           pointer argument asks at run time once the program loads the
           class. A user-class instance has no address C can use; a native
           class's struct is its package's to hand over. */
        int iob_cls = ffi_iobuffer_class(c);
        int iob_temps = 0;
        for (int ai = 0; ai < fixed_argc && ai < argc; ai++) {
          const FfiSpecInfo *psi = ffi_spec_lookup(c->ffi_funcs[fi].args[ai]);
          if (!psi || !sp_streq(psi->c_type, "void *")) continue;
          TyKind pat = comp_ntype(c, argv[ai]);
          /* a boxed argument that may be a buffer takes the same ordered,
             rooted temp, its address taken after every argument has run */
          if (pat == TY_POLY && iob_cls >= 0) { iob_temps = 1; continue; }
          if (!ty_is_object(pat)) continue;
          if (ty_object_class(pat) == iob_cls) { iob_temps = 1; continue; }
          if (!c->classes[ty_object_class(pat)].is_native_class)
            unsupported(c, argv[ai], "ffi pointer argument (a Ruby object has no C address; pass an IO::Buffer, a String or a :ptr value)");
        }
        int use_temps = blocking || iob_temps;
        Buf pre_buf; memset(&pre_buf, 0, sizeof pre_buf);
        Buf base_buf; memset(&base_buf, 0, sizeof base_buf);
        /* blocking: the buffers are locked across the call (hold after every
           base is taken, since taking one may raise) and released after it */
        Buf hold_buf; memset(&hold_buf, 0, sizeof hold_buf);
        Buf rel_buf; memset(&rel_buf, 0, sizeof rel_buf);
        int tb = use_temps ? ++g_tmp : 0;
        /* Build the raw C call */
        Buf call_buf; memset(&call_buf, 0, sizeof call_buf);
        if (is_vararg && fixed_argc == 0) {
          buf_printf(&call_buf, "((%s (*)(", ffi_c_type(ret_spec));
          for (int ai = 0; ai < fixed_argc; ai++) {
            if (ai) buf_puts(&call_buf, ", ");
            buf_puts(&call_buf, ffi_c_type(c->ffi_funcs[fi].args[ai]));
          }
          if (fixed_argc) buf_puts(&call_buf, ", ");
          buf_printf(&call_buf, "...))%s)", c->ffi_funcs[fi].csym ? c->ffi_funcs[fi].csym : c->ffi_funcs[fi].name);
        }
        else ffi_extern_name(c, fi, &call_buf);   /* the asm-labelled extern (codegen.c) */
        buf_puts(&call_buf, "(");
        for (int ai = 0; ai < fixed_argc && ai < argc; ai++) {
          if (ai) buf_puts(&call_buf, ", ");
          const char *spec = c->ffi_funcs[fi].args[ai];
          TyKind at = comp_ntype(c, argv[ai]);
          int cbidx = ffi_find_callback(c, rcmod, spec);
          if (cbidx >= 0) { emit_ffi_callback_arg(c, cbidx, argv[ai], &call_buf); continue; }
          size_t arg_at = call_buf.len;   /* the converted argument, for the temp form */
          const FfiSpecInfo *asi = ffi_spec_lookup(spec);
          int ptr_slot = asi && sp_streq(asi->c_type, "void *");
          int iob_writing = !sp_streq(spec, "buffer_in");
          if (ptr_slot && iob_cls >= 0 && (at == ty_object(iob_cls) || (at == TY_POLY && use_temps))) {
            if (at == TY_POLY) {
              buf_printf(&pre_buf, "sp_RbVal _bk%d_%d = ", tb, ai);
              emit_expr(c, argv[ai], &pre_buf);
              buf_printf(&pre_buf, "; SP_GC_ROOT_RBVAL(_bk%d_%d); ", tb, ai);
              /* a user-class instance has no C address: refused here, as the
                 typed argument is refused while compiling, rather than
                 handing C the object to write over */
              buf_printf(&pre_buf, "if (_bk%d_%d.tag == SP_TAG_OBJ && _bk%d_%d.cls_id >= 0 && "
                                   "_bk%d_%d.cls_id < (sp_int)sizeof sp_ffi_user_cls && sp_ffi_user_cls[_bk%d_%d.cls_id]) "
                                   "sp_raise_cls(\"TypeError\", sp_sprintf(\"no implicit conversion of %%s into a C pointer\", "
                                   "sp_poly_class_name(_bk%d_%d))); ",
                         tb, ai, tb, ai, tb, ai, tb, ai, tb, ai);
              buf_printf(&base_buf, "void * _b%d_%d = sp_IOBuffer_ffi_ptr(_bk%d_%d, %d, %d); ",
                         tb, ai, tb, ai, iob_cls, iob_writing);
              buf_printf(&hold_buf, "sp_int _bh%d_%d = sp_IOBuffer_ffi_hold_v(_bk%d_%d, %d); ", tb, ai, tb, ai, iob_cls);
              buf_printf(&rel_buf, "sp_IOBuffer_ffi_release_v(_bk%d_%d, %d, _bh%d_%d); ", tb, ai, iob_cls, tb, ai);
            }
            else {
              buf_printf(&pre_buf, "sp_IOBuffer *_bk%d_%d = ", tb, ai);
              emit_expr(c, argv[ai], &pre_buf);
              buf_printf(&pre_buf, "; SP_GC_ROOT(_bk%d_%d); ", tb, ai);
              buf_printf(&base_buf, "void * _b%d_%d = sp_IOBuffer_ffi_base(_bk%d_%d, %d); ",
                         tb, ai, tb, ai, iob_writing);
              buf_printf(&hold_buf, "sp_int _bh%d_%d = sp_IOBuffer_ffi_hold(_bk%d_%d); ", tb, ai, tb, ai);
              buf_printf(&rel_buf, "sp_IOBuffer_ffi_release(_bk%d_%d, _bh%d_%d); ", tb, ai, tb, ai);
            }
            buf_printf(&call_buf, "_b%d_%d", tb, ai);
            continue;
          }
          if (sp_streq(spec, "str")) {
            if (at == TY_POLY) {
              /* a boxed String may be a shared handle (a mutable String boxed
                 as SP_BUILTIN_STRBUF), whose .v.s is the handle, not the
                 bytes; nil is FFI's NULL, anything else a TypeError (#5679) */
              buf_puts(&call_buf, "sp_poly_arg_str_or_null("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")");
            }
            else {
              /* An UNKNOWN argument lowers to the gate's raise token, an
                 sp_RbVal -- it cannot land raw in a const char* slot
                 (#3330). The raise diverges first, so coerce the dead
                 value to keep the C well-typed. */
              Buf fb; memset(&fb, 0, sizeof fb);
              emit_expr(c, argv[ai], &fb);
              const char *ft = fb.p ? fb.p : "";
              if (strncmp(ft, "sp_raise_nomethod(", 18) == 0 ||
                  strncmp(ft, "(sp_raise_cls(", 14) == 0)
                buf_printf(&call_buf, "((void)(%s), (const char *)0)", ft);
              else buf_puts(&call_buf, ft);
              free(fb.p);
            }
          }
          else if (sp_streq(spec, "ptr")) {
            if (at == TY_POLY) {
              buf_puts(&call_buf, "((void *)(");
              emit_expr(c, argv[ai], &call_buf);
              buf_puts(&call_buf, ").v.p)");
            }
            else {
              buf_puts(&call_buf, "((void *)(uintptr_t)(");
              emit_expr(c, argv[ai], &call_buf);
              buf_puts(&call_buf, "))");
            }
          }
          else if (sp_streq(spec, "float") || sp_streq(spec, "double")) {
            /* nil is no double: a boxed nil, or a slot holding its kind's nil
               sentinel, raises TypeError as the ffi gem's NUM2DBL does */
            emit_ffi_num_arg(c, argv[ai], at, spec, 1, &call_buf);
          }
          else if (sp_streq(spec, "int_array")) {
            /* Hand off element data, never the array struct pointer (which
               would pun the header / read boxed sp_RbVal tags as ints). */
            if (at == TY_INT_ARRAY)        { buf_puts(&call_buf, "sp_IntArray_ffi_data(");   emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_int_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_ffi_int_array_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else                           { buf_puts(&call_buf, "((const int64_t *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else if (sp_streq(spec, "float_array")) {
            if (at == TY_FLOAT_ARRAY)      { buf_puts(&call_buf, "sp_FloatArray_ffi_data(");  emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY_ARRAY)  { buf_puts(&call_buf, "sp_PolyArray_ffi_float_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else if (at == TY_POLY)        { buf_puts(&call_buf, "sp_ffi_float_array_data("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, ")"); }
            else                           { buf_puts(&call_buf, "((const double *)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))"); }
          }
          else {
            /* integer-like: int, uint32, size_t, long, etc. A nil raises
               TypeError, as the ffi gem's NUM2INT does. */
            if (at != TY_BIGINT) {
              emit_ffi_num_arg(c, argv[ai], at, spec, 0, &call_buf);
            }
            else {
              /* An overflow-promoted integer (e.g. a backoff computed by
                 repeated *2) arrives as sp_Bigint*. Narrow it to the C
                 integer the FFI arg expects, not the pointer value. */
              buf_puts(&call_buf, "(("); buf_puts(&call_buf, ffi_c_type(spec)); buf_puts(&call_buf, ")sp_bigint_to_int(");
              emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
          }
          if (use_temps) {
            /* move the converted argument out to a temp ahead of the call */
            buf_printf(&pre_buf, "%s _b%d_%d = %s; ", ffi_c_type(spec), tb, ai, call_buf.p + arg_at);
            buf_erase(&call_buf, arg_at, call_buf.len - arg_at);
            buf_printf(&call_buf, "_b%d_%d", tb, ai);
          }
        }
        /* Extra variadic args: promote by inferred type (int->sp_int,
           float->double, str->const char*, ptr->void*). A poly-typed vararg
           has no compile-time C type to promote to, so reject it loudly.
           An Integer goes at the target's Integer width, sp_int, not at
           long long: on an ILP32 target a `%d` reads 4 bytes, and an 8-byte
           long long shifted every later vararg by 4 (a following `%s` then
           read an integer as a pointer). On LP64 sp_int is 8 bytes, as
           long long was, so the call is the same. */
        if (is_vararg) {
          for (int ai = fixed_argc; ai < argc; ai++) {
            if (ai) buf_puts(&call_buf, ", ");
            TyKind at = comp_ntype(c, argv[ai]);
            if (at == TY_INT || at == TY_BOOL) {
              buf_puts(&call_buf, "((sp_int)("); emit_int_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
            else if (at == TY_FLOAT) {
              buf_puts(&call_buf, "((double)("); emit_expr(c, argv[ai], &call_buf); buf_puts(&call_buf, "))");
            }
            else if (at == TY_STRING) {
              emit_expr(c, argv[ai], &call_buf);
            }
            else {
              /* A poly (or otherwise non-scalar) vararg has no compile-time C
                 type -- int vs string is indistinguishable in a C varargs call,
                 so promoting it would be a silent-wrong. Reject loudly. */
              free(call_buf.p);
              unsupported(c, argv[ai], "ffi variadic argument (needs a concrete int/float/str type)");
              return 1;
            }
          }
        }
        buf_puts(&call_buf, ")");
        if (blocking) {
          /* the worker is out of the world for exactly the call */
          Buf w; memset(&w, 0, sizeof w);
          if (is_void_ret)
            buf_printf(&w, "({ %s%s%ssp_native_enter(); %s; sp_native_leave(); %s})",
                       pre_buf.p ? pre_buf.p : "", base_buf.p ? base_buf.p : "", hold_buf.p ? hold_buf.p : "",
                       call_buf.p, rel_buf.p ? rel_buf.p : "");
          else
            buf_printf(&w, "({ %s%s%s%s _b%d_r; sp_native_enter(); _b%d_r = %s; sp_native_leave(); %s_b%d_r; })",
                       pre_buf.p ? pre_buf.p : "", base_buf.p ? base_buf.p : "", hold_buf.p ? hold_buf.p : "",
                       ffi_c_type(ret_spec), tb, tb, call_buf.p, rel_buf.p ? rel_buf.p : "", tb);
          free(call_buf.p); call_buf = w;
        }
        else if (use_temps) {
          Buf w; memset(&w, 0, sizeof w);
          if (is_void_ret)
            buf_printf(&w, "({ %s%s%s; })", pre_buf.p ? pre_buf.p : "", base_buf.p ? base_buf.p : "", call_buf.p);
          else
            buf_printf(&w, "({ %s%s%s _b%d_r = %s; _b%d_r; })", pre_buf.p ? pre_buf.p : "",
                       base_buf.p ? base_buf.p : "", ffi_c_type(ret_spec), tb, call_buf.p, tb);
          free(call_buf.p); call_buf = w;
        }
        free(pre_buf.p);
        free(base_buf.p);
        free(hold_buf.p);
        free(rel_buf.p);
        if (is_void_ret) {
          buf_puts(b, "("); buf_puts(b, call_buf.p); buf_puts(b, ", (sp_int)0)");
        }
        else if (is_ptr_ret) {
          /* wrap the foreign void* in a poly sp_RbVal that the GC won't trace */
          buf_printf(b, "sp_box_foreign_ptr((void *)(%s))", call_buf.p);
        }
        else if (is_str_ret) {
          buf_printf(b, "sp_str_dup_external(%s)", call_buf.p);
        }
        else if (is_binstr_ret) {
          /* Binary-safe: build the String from the exact byte count the callee
             published in sp_ffi_bin_len, not strlen (which truncates at an
             embedded NUL). Sequence the call before reading the length -- C
             leaves argument evaluation order unspecified -- via a temp. Tagged
             BINARY for the reason the `cbinstr` arm above gives. */
          int tp = ++g_tmp;
          buf_printf(b, "({ const char *_t%d = %s; "
                        "sp_str_as_binary(sp_str_from_bytes(_t%d, (size_t)(sp_ffi_bin_len < 0 ? 0 : sp_ffi_bin_len))); })",
                     tp, call_buf.p, tp);
        }
        else {
          /* numeric / bool: cast to sp_int or sp_float */
          int ffi_ret_is_float = (sp_streq(ret_spec, "float") || sp_streq(ret_spec, "double"));
          if (ffi_ret_is_float) {
            buf_puts(b, "((sp_float)("); buf_puts(b, call_buf.p); buf_puts(b, "))");
          }
          else {
            buf_puts(b, "((sp_int)("); buf_puts(b, call_buf.p); buf_puts(b, "))");
          }
        }
        free(call_buf.p);
        return 1;
      }
      /* ffi_buffer access: Module.buf_name returns static char* as void* poly */
      {
        int bi = -1;
        for (int fbi = 0; fbi < c->n_ffi_bufs; fbi++)
          if (sp_streq(c->ffi_bufs[fbi].mod, rcmod) && sp_streq(c->ffi_bufs[fbi].name, name)) {
            bi = fbi; break;
          }
        if (bi >= 0) {
          buf_printf(b, "sp_box_foreign_ptr((void *)sp_ffi_buf_%s_%s)",
                     c->ffi_bufs[bi].mod, c->ffi_bufs[bi].name);
          return 1;
        }
      }
      /* ffi_read_* access: Module.reader_name(buf) */
      {
        int ri = -1;
        for (int fri = 0; fri < c->n_ffi_readers; fri++)
          if (sp_streq(c->ffi_readers[fri].mod, rcmod) && sp_streq(c->ffi_readers[fri].name, name)) {
            ri = fri; break;
          }
        if (ri >= 0 && argc >= 1) {
          const char *kind = c->ffi_readers[ri].kind;
          int off = c->ffi_readers[ri].offset;
          ffi_check_buffer_bounds(c, id, argv[0], kind, off, "read");
          /* The suffix names the load width; taking every one but i32 as a
             32-bit unsigned read meant a declared 1- or 2-byte field pulled in
             its neighbours, and a read at the last bytes of a buffer ran off
             the end of it (#3928). */
          const char *ctype = ffi_scalar_ctype(kind);
          if (!ctype) ctype = "uint32_t";
          if (argc >= 1) {
            if (kind && sp_streq(kind, "ptr")) {
              int rt3 = ++g_tmp;
              buf_printf(b, "({ void *_t%d = (*(void **)((char *)(", rt3);
              /* unbox a poly buffer to its void*; a non-poly arg (e.g. a
                 pointer passed as sp_int through a callback param) is cast
                 directly -- close its wrapping paren the same way .v.p does. */
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
              buf_printf(b, " + %d)); sp_box_foreign_ptr(_t%d); })", off, rt3);
            }
            else {
              /* `+ off` must apply to the char* (byte offset), not the typed
                 pointer (which would scale it by sizeof(elem)). */
              buf_printf(b, "((sp_int)(*(%s *)((char *)(", ctype);
              TyKind at = comp_ntype(c, argv[0]);
              if (at == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
              else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
              buf_printf(b, " + %d)))", off);
            }
          }
          return 1;
        }
      }
      /* ffi_struct accessors: Module.Name_new (alloc, boxed ptr),
         Module.Name_get_<f>(ptr) (read field, boxed by type),
         Module.Name_set_<f>(ptr, val) (write field, returns nil). */
      {
        int fsi, ffi;
        int fsm = ffi_struct_method(c, rcmod, name, &fsi, &ffi);
        if (fsm == FFI_SM_NEW) {
          buf_printf(b, "sp_box_foreign_ptr(calloc(1, sizeof(sp_ffi_struct_%s_%s)))",
                     c->ffi_structs[fsi].mod, c->ffi_structs[fsi].name);
          return 1;
        }
        if (fsm == FFI_SM_GET && argc >= 1) {
          const char *sm2 = c->ffi_structs[fsi].mod, *sn2 = c->ffi_structs[fsi].name;
          const char *spec = c->ffi_structs[fsi].fields[ffi].spec;
          const char *fname = c->ffi_structs[fsi].fields[ffi].name;
          TyKind rt2 = ffi_spec_to_ty(spec);
          buf_puts(b, rt2 == TY_POLY ? "sp_box_foreign_ptr((void *)("
                    : rt2 == TY_STRING ? "((const char *)("
                    : rt2 == TY_FLOAT ? "((sp_float)(" : "((sp_int)(");
          buf_printf(b, "((sp_ffi_struct_%s_%s *)", sm2, sn2);
          if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
          else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          buf_printf(b, ")->%s))", fname);
          return 1;
        }
        if (fsm == FFI_SM_SET && argc >= 2) {
          const char *sm2 = c->ffi_structs[fsi].mod, *sn2 = c->ffi_structs[fsi].name;
          const char *spec = c->ffi_structs[fsi].fields[ffi].spec;
          const char *fname = c->ffi_structs[fsi].fields[ffi].name;
          TyKind rt2 = ffi_spec_to_ty(spec);
          buf_printf(b, "(((sp_ffi_struct_%s_%s *)", sm2, sn2);
          if (comp_ntype(c, argv[0]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
          else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[0], b); buf_puts(b, ")"); }
          buf_printf(b, ")->%s = (%s)", fname, ffi_c_type(spec));
          if (rt2 == TY_POLY) {
            if (comp_ntype(c, argv[1]) == TY_POLY) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_puts(b, ").v.p"); }
            else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
          }
          else if (rt2 == TY_STRING) { buf_puts(b, "("); emit_str_expr(c, argv[1], b); buf_puts(b, ")"); }
          else if (rt2 == TY_FLOAT) { buf_puts(b, "("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
          else { buf_puts(b, "("); emit_int_expr(c, argv[1], b); buf_puts(b, ")"); }
          buf_puts(b, ", sp_box_nil())");
          return 1;
        }
      }
      /* ffi_write_* access: Module.writer_name(buf, val) stores val at
         `offset` bytes into buf and returns the written value. Symmetric to
         the reader branch above; the `+ off` applies to the char* byte view. */
      {
        int wi = -1;
        for (int fwi = 0; fwi < c->n_ffi_writers; fwi++)
          if (sp_streq(c->ffi_writers[fwi].mod, rcmod) && sp_streq(c->ffi_writers[fwi].name, name)) {
            wi = fwi; break;
          }
        if (wi >= 0 && argc >= 2) {
          const char *kind = c->ffi_writers[wi].kind;
          int off = c->ffi_writers[wi].offset;
          ffi_check_buffer_bounds(c, id, argv[0], kind, off, "write");
          int tv = ++g_tmp;
          if (kind && sp_streq(kind, "ptr")) {
            TyKind vt = comp_ntype(c, argv[1]);
            buf_printf(b, "({ void *_t%d = ", tv);
            if (vt == TY_POLY) { buf_puts(b, "(void *)("); emit_expr(c, argv[1], b); buf_puts(b, ").v.p"); }
            else { buf_puts(b, "(void *)(uintptr_t)("); emit_expr(c, argv[1], b); buf_puts(b, ")"); }
            buf_puts(b, "; *(void **)((char *)(");
            TyKind bt = comp_ntype(c, argv[0]);
            if (bt == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
            else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            buf_printf(b, " + %d) = _t%d; sp_box_foreign_ptr(_t%d); })", off, tv, tv);
          }
          else {
            const char *ctype = ffi_scalar_ctype(kind);
            if (!ctype) ctype = "uint32_t";
            buf_printf(b, "({ %s _t%d = (%s)(", ctype, tv, ctype);
            emit_int_expr(c, argv[1], b);
            buf_printf(b, "); *(%s *)((char *)(", ctype);
            TyKind bt = comp_ntype(c, argv[0]);
            if (bt == TY_POLY) { emit_expr(c, argv[0], b); buf_puts(b, ").v.p"); }
            else { emit_expr(c, argv[0], b); buf_puts(b, ")"); }
            buf_printf(b, " + %d) = _t%d; (sp_int)_t%d; })", off, tv, tv);
          }
          return 1;
        }
      }
    }
  }

  /* Module.field = val  /  Module.field  -> singleton accessor sg_Mod_field */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0 && emit_sg_accessor(c, ci, cn, name, argc, argv, b)) return 1;
    }
  }

  /* self.field = val  /  self.field  inside a class method or module body --
     and the same call written without a receiver, which a method reached
     through `extend` makes ordinary (#3788) */
  if ((recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) ||
      (recv < 0 && argc == 0 && nt_ref(nt, id, "block") < 0)) {
    Scope *_sgencl = comp_scope_of(c, id);
    int _sg_cid = (_sgencl && _sgencl->is_cmethod && _sgencl->class_id >= 0)
                  ? _sgencl->class_id : g_class_body_id;
    if (_sg_cid >= 0 && emit_sg_accessor(c, _sg_cid, c->classes[_sg_cid].name, name, argc, argv, b))
      return 1;
  }

  /* obj.attr = val as an expression: store into the ivar and yield the value.
     The statement form is handled in emit_stmt; this expression form is hit
     when the assignment is the last statement of an instance_eval block. */
  if (recv >= 0) {
    int _alen = (int)strlen(name);
    TyKind _art = comp_ntype(c, recv);
    if (_alen > 1 && name[_alen - 1] == '=' && ty_is_object(_art)) {
      char _abase[256]; int _ablen = _alen - 1;
      if (_ablen < (int)sizeof _abase) {
        memcpy(_abase, name, (size_t)_ablen); _abase[_ablen] = '\0';
        int _arc = ty_object_class(_art), _adefc = -1, _awmdc = -1;
        /* attr writer -> field write, UNLESS an explicit `def x=` overrides it
           at an equal-or-more-derived class; then fall through to dispatch.
           CRuby: attr_accessor defines an ordinary writer method. */
        int _awins = comp_writer_in_chain(c, _arc, _abase, &_adefc);
        if (_awins && comp_method_in_chain(c, _arc, name, &_awmdc) >= 0) {
          for (int k = _arc; k >= 0; k = c->classes[k].parent) {
            if (k == _awmdc) { _awins = 0; break; }
            if (k == _adefc) { _awins = 1; break; }
          }
        }
        if (_awins) {
          if (emit_or_take_back(c, id, b, emit_vis_refusal)) return 1;   /* `private :x=` on the writer */
          char _aivn[258]; snprintf(_aivn, sizeof _aivn, "@%s", _abase);
          int _aiv = comp_ivar_index(&c->classes[_adefc < 0 ? _arc : _adefc], _aivn);
          TyKind _aivt = _aiv >= 0 ? c->classes[_adefc < 0 ? _arc : _adefc].ivar_types[_aiv] : TY_UNKNOWN;
          /* materialize the receiver so a frozen instance raises FrozenError
             before the store, even in value position (#3078) */
          int _atmp = ++g_tmp;
          char _aself[32]; snprintf(_aself, sizeof _aself, "_t%d", _atmp);
          buf_printf(b, "({ sp_%s *_t%d = ", c->classes[_arc].c_name, _atmp); emit_expr(c, recv, b); buf_puts(b, "; ");
          emit_frozen_obj_guard(c, _arc, _aself, b);
          /* a typed slot (an --rbs seed pins one) given a boxed value: the
             slot takes it unboxed, and the assignment's value is still the
             right-hand side, boxed as it came. Stored raw, an sp_RbVal went
             into an sp_int and the C did not compile (#4856). */
          if (argc >= 1 && _aivt != TY_POLY && _aivt != TY_UNKNOWN &&
              comp_ntype(c, argv[0]) == TY_POLY) {
            int _tvv = ++g_tmp;
            char _tvn[32]; snprintf(_tvn, sizeof _tvn, "_t%d", _tvv);
            buf_printf(b, "sp_RbVal %s = ", _tvn); emit_one_arg(c, argv[0], 0, b);
            buf_printf(b, "; SP_GC_ROOT_RBVAL(%s); _t%d->iv_%s = ", _tvn, _atmp, iv_c(_abase));
            emit_unbox_text(c, _aivt, _tvn, b);
            TyKind _nt = comp_ntype(c, id);
            if (_nt == TY_POLY || _nt == TY_UNKNOWN) buf_printf(b, "; %s; })", _tvn);
            else buf_printf(b, "; _t%d->iv_%s; })", _atmp, iv_c(_abase));
            return 1;
          }
          /* a value of another C type than the slot (an Integer into a slot
             widened to Bignum) converts into it, through a temp: the
             assignment's value is still the right-hand side as it came */
          if (argc >= 1 && _aivt != TY_POLY && _aivt != TY_UNKNOWN &&
              comp_ntype(c, argv[0]) != TY_UNKNOWN &&
              !store_fits(c, store_value_kind(c, argv[0]), _aivt)) {
            emit_attr_writer_converted(c, argv[0], _aivt, _atmp, _abase, b);
            return 1;
          }
          buf_printf(b, "_t%d->iv_%s = ", _atmp, iv_c(_abase));
          if (argc >= 1) {
            if (_aivt == TY_POLY && comp_ntype(c, argv[0]) != TY_POLY) emit_one_arg(c, argv[0], 1, b);
            /* the other way: a typed slot (an --rbs seed pins it) given a
               boxed value, which the statement form already unboxes; stored
               raw, an sp_RbVal went into an sp_int (#4856) */
            else emit_one_arg(c, argv[0], 0, b);
          }
          else buf_puts(b, "0");
          buf_puts(b, "; })");
          return 1;
        }
        /* An explicit `def x=(v)` reached as `obj.x = v` in value position:
           the expression's value is the right-hand side, as for every
           assignment, not what the writer's body returns. The generic call
           answered the body (a void, or a poly), and a block ending in
           `status.interrupt = true` handed that to a slot inference had
           typed from the right-hand side (#4516). A side-effect-free
           argument (a literal, a variable) is re-emitted after the call; any
           other is bound once to a temporary local that the call reads. */
        if (argc == 1 && comp_method_in_chain(c, _arc, name, NULL) >= 0 &&
            nt_ref(nt, id, "block") < 0 && !g_setter_value_inner &&
            call_is_setter_assign(nt, id) &&   /* not a send's plain call (#4921) */
            (name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z'))) {   /* a setter, not ==, <=, [] = */
          const char *aty = nt_type(nt, argv[0]);
          int simple = aty && (sp_streq(aty, "IntegerNode") || sp_streq(aty, "FloatNode") ||
                               sp_streq(aty, "TrueNode") || sp_streq(aty, "FalseNode") ||
                               sp_streq(aty, "NilNode") || sp_streq(aty, "SymbolNode") ||
                               sp_streq(aty, "StringNode") || sp_streq(aty, "LocalVariableReadNode") ||
                               sp_streq(aty, "InstanceVariableReadNode") || sp_streq(aty, "SelfNode"));
          TyKind at = comp_ntype(c, argv[0]);
          if (simple) {
            buf_puts(b, "({ (void)(");
            g_setter_value_inner++; emit_call_body(c, id, b); g_setter_value_inner--;
            buf_puts(b, "); ");
            emit_expr(c, argv[0], b);
            buf_puts(b, "; })");
            return 1;
          }
          if (at != TY_UNKNOWN && at != TY_VOID) {
            Scope *esc = comp_scope_of(c, id);
            if (esc) {
              char svn[32]; snprintf(svn, sizeof svn, "__sv%d", ++g_tmp);
              LocalVar *lv = scope_local_intern(esc, svn);
              lv->type = at;
              int rd = nt_new_node((NodeTable *)nt, "LocalVariableReadNode");
              nt_node_set_str((NodeTable *)nt, rd, "name", svn);
              comp_grow_node_arrays(c);
              c->nscope[rd] = c->nscope[id];
              c->ntype[rd] = at;
              int saved0 = argv[0];
              int argsn = nt_ref(nt, id, "arguments");
              int one[1] = { rd };
              /* the local is declared in the prelude, ahead of whatever the
                 call's own emission hoists there (its arguments included) */
              Buf *decl = g_pre ? g_pre : b;
              /* the argument's own prelude (a block-taking call's loop) goes
                 ahead of the declaration, not into the middle of it */
              Buf apre; memset(&apre, 0, sizeof apre);
              Buf aval; memset(&aval, 0, sizeof aval);
              { Buf *sv_pre = g_pre; g_pre = &apre; emit_one_arg(c, saved0, 0, &aval); g_pre = sv_pre; }
              if (!g_pre) buf_puts(b, "({ ");
              if (apre.p) buf_puts(decl, apre.p);
              if (g_pre) emit_indent(g_pre, g_indent);
              emit_ctype(c, at, decl); buf_printf(decl, " lv_%s = %s; ", svn, aval.p ? aval.p : "0");
              free(apre.p); free(aval.p);
              /* a poly temporary is an sp_RbVal, and its root is the RbVal
                 kind -- needs_root() answers yes for TY_POLY too, so that
                 test has to come second, or the plain root reads the
                 RbVal's tag word as an object pointer at the next mark */
              if (at == TY_POLY) buf_printf(decl, "SP_GC_ROOT_RBVAL(lv_%s); ", svn);
              else if (needs_root(at) && !comp_ty_value_obj(c, at)) buf_printf(decl, "SP_GC_ROOT(lv_%s); ", svn);
              if (g_pre) { buf_puts(g_pre, "\n"); buf_puts(b, "({ "); }
              nt_node_set_arr((NodeTable *)nt, argsn, "arguments", one, 1);
              buf_puts(b, "(void)(");
              g_setter_value_inner++; emit_call_body(c, id, b); g_setter_value_inner--;
              buf_puts(b, "); ");
              int back[1] = { saved0 };
              nt_node_set_arr((NodeTable *)nt, argsn, "arguments", back, 1);
              buf_printf(b, "lv_%s; })", svn);
              return 1;
            }
          }
        }
      }
    }
  }

  /* `Module.accessor.cmethod(args)` folded to a constant (Stage-1): emit the
     resolved constant's class method directly. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int defcls = -1;
      int mi = comp_cmethod_in_chain(c, fold_ci, name, &defcls);
      if (mi >= 0) {
        nd_callee(c, id, mi, defcls, 0);
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
        const char *lead0 = emit_cmethod_self_cls_arg(c, mi, fold_ci, b);
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), lead0, b);
        emit_cmethod_block_arg(c, id, &c->scopes[mi], -1, b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* Stage-2: the accessor holds one of several constants (stored as a boxed
       Class). Dispatch the class method via a cls_id cascade over the slot. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      int valid = 0;
      for (int k = 0; k < ncand; k++) if (comp_cmethod_in_chain(c, cand[k], name, NULL) >= 0) valid++;
      if (valid > 0) {
        TyKind res = comp_ntype(c, id);
        int void_res = (res == TY_VOID || res == TY_UNKNOWN);
        /* A literal block at the call site is lowered to one sp_Proc * temp
           shared by every candidate branch (lowering it per-branch would
           emit the proc function once per candidate). */
        int blk_tmp = -1;
        int casc_blk = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
        if (casc_blk >= 0) {
          for (int k = 0; k < ncand && blk_tmp < 0; k++) {
            int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
            if (mi < 0) continue;
            Scope *cm = &c->scopes[mi];
            if (cm->blk_param && cm->blk_param[0] && !cm->yields) {
              blk_tmp = ++g_tmp;
              Buf pb; memset(&pb, 0, sizeof pb);
              emit_proc_literal(c, casc_blk, &pb);
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "sp_Proc *_t%d = %s;\n", blk_tmp, pb.p ? pb.p : "NULL");
              emit_indent(g_pre, g_indent);
              buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", blk_tmp);
              free(pb.p);
            }
          }
        }
        int tcid = ++g_tmp;
        buf_printf(b, "({ int _t%d = (", tcid); emit_expr(c, recv, b); buf_puts(b, ").cls_id; ");
        nd_stamp(id, ND_SWITCH);
        for (int k = 0; k < ncand; k++) {
          int defcls = -1;
          int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
          if (mi >= 0) nd_callee(c, id, mi, defcls, 1);
        }
        if (void_res) {
          for (int k = 0; k < ncand; k++) {
            int defcls = -1;
            int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
            if (mi < 0) continue;
            buf_printf(b, "if (_t%d == %d) sp_%s_s_%s(", tcid, cand[k], c->classes[defcls].c_name, mc(c->scopes[mi].name));
            { const char *leadv = emit_cmethod_self_cls_arg(c, mi, cand[k], b);
              emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), leadv, b); }
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, b);
            buf_puts(b, "); ");
          }
          buf_printf(b, "0; })");
          return 1;
        }
        emit_ctype(c, res, b); buf_printf(b, " _t%d_r = %s; ", tcid, default_value_from_compiler(c, res));
        for (int k = 0; k < ncand; k++) {
          int defcls = -1;
          int mi = comp_cmethod_in_chain(c, cand[k], name, &defcls);
          if (mi < 0) continue;
          buf_printf(b, "if (_t%d == %d) _t%d_r = ", tcid, cand[k], tcid);
          if (res == TY_POLY && c->scopes[mi].ret != TY_POLY) {
            Buf cb; memset(&cb, 0, sizeof cb);
            buf_printf(&cb, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
            { const char *leadb = emit_cmethod_self_cls_arg(c, mi, cand[k], &cb);
              emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), leadb, &cb); }
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, &cb);
            buf_puts(&cb, ")");
            emit_boxed_text(c, c->scopes[mi].ret, cb.p ? cb.p : "0", b);
            free(cb.p);
          }
          else {
            buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
            { const char *leadc = emit_cmethod_self_cls_arg(c, mi, cand[k], b);
              emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), leadc, b); }
            emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp, b);
            buf_puts(b, ")");
          }
          buf_puts(b, "; ");
        }
        buf_printf(b, "_t%d_r; })", tcid);
        return 1;
      }
    }
  }

  /* Class.cmethod(args) / M::Sub.cmethod(args) -> sp_<Class>_s_<method>(args) */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      int defcls = -1, mi = -1;
      /* the target is the call's plan (call_plan.c): the constant's class
         method, or a method the program adds to Class (its owner, not the
         constant's class, is then the plan's) */
      const CallPlan *pl = cplan_user(c, id);
      if (pl->mi >= 0 && pl->via == UC_CMETH) {
        mi = pl->mi;
        defcls = c->scopes[mi].class_id;
        if (g_plan_check) cplan_served("cmethod-on-constant");
        if (pl->owner_ci != ci) ci = builtin_class_id(nt_str(nt, recv, "name"));
      }
      /* --plan-check keeps the arm's own lookup as the assertion; without a
         plan it is the arm's answer */
      if (g_plan_check || mi < 0) {
        int oci = comp_class_index(c, nt_str(nt, recv, "name"));
        int odef = -1;
        int omi = oci >= 0 ? comp_cmethod_in_chain(c, oci, name, &odef) : -1;
        if (omi < 0 && (omi = class_reopen_cmethod(c, recv, name)) >= 0) {
          odef = c->scopes[omi].class_id;
          oci = builtin_class_id(nt_str(nt, recv, "name"));
        }
        if (mi < 0) {
          /* a method the plan did not serve */
          if (g_plan_check && omi >= 0)
            fprintf(stderr, "plan-check: cplan-fallback: cmethod-on-constant node %d %s\n", id, name ? name : "?");
          mi = omi; defcls = odef; ci = oci;
        }
        else if (omi != mi || odef != defcls || oci != ci)
          fprintf(stderr, "plan-check: cplan-conflict: cmethod-on-constant node %d %s: plan %d/%d/%d, arm %d/%d/%d\n",
                  id, name ? name : "?", mi, defcls, ci, omi, odef, oci);
      }
      if (mi >= 0) {
        nd_callee(c, id, mi, defcls, 0);
        buf_printf(b, "sp_%s_s_%s(", c->classes[defcls].c_name, mc(c->scopes[mi].name));
        const char *lead1 = emit_cmethod_self_cls_arg(c, mi, ci, b);
        emit_args_filled(c, mi, nt_ref(nt, id, "arguments"), lead1, b);
        /* Pass &block as sp_Proc * when the class method keeps a real &blk
           param and isn't yield-inlined -- the instance-method and bare-call
           paths already do this; a module/class-method call must too. */
        emit_cmethod_block_arg(c, id, &c->scopes[mi], -1, b);
        buf_puts(b, ")");
        return 1;
      }
    }
  }
  return 0;
}

/* new and allocate on a Class value, a poly receiver, self's class or a constant, and Cls.exception */
int emit_call_new_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* .new WITH arguments on a Class value whose class is only known at run time.
     A local statically holding one class folds to that class and never gets
     here; a call returning a class cannot fold, so it had no emitter at all
     (#3415). Same shape as the poly-receiver form below: one arm per class
     whose constructor takes exactly these positional args, each argument
     hoisted once and coerced per arm. */
  if (recv >= 0 && sp_streq(name, "new") && comp_ntype(c, recv) == TY_CLASS &&
      argc > 0 && ctor_block_dispatchable(c, id) &&
      (class_recv_is_dynamic(c, recv) ||
       /* `self.new(args)` in a class method: self is the RECEIVING class (a
          subclass inherits the method with self = itself), so the receiver
          is exactly as dynamic as a class-valued variable. The zero-arg arm
          below has dispatched it all along; the argument-taking form was
          refused as an unsupported node shape (#4202). */
       (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode"))) &&
      comp_ntype(c, id) == TY_POLY) {
    int kt = ++g_tmp, rt2 = ++g_tmp;
    /* Keyword arguments: the positional-only arms below counted the keyword
       hash as one more positional and matched no class, so the switch came
       out empty and every call raised NoMethodError (#4845). Here each class
       whose initialize takes this call's shape gets an arm that lays the
       arguments out as a call to that initialize would, keywords by name and
       defaults filled -- the same emit_args_filled a static `K.new(...)`
       uses. The arguments are evaluated inside the arm taken, once.
       A `*splat` goes the same way: the arms below boxed the whole array as
       one argument and bound it to the first parameter. */
    if (call_has_keyword_args(nt, argv, argc) || call_has_splat_arg(nt, argv, argc) ||
        ctor_block_splices(c, id)) {
      emit_class_value_new_kw(c, id, recv, 0, b);
      return 1;
    }
    int *atmp = calloc((size_t)argc, sizeof(int));
    buf_printf(b, "({ sp_Class _t%d = ", kt); emit_expr(c, recv, b); buf_puts(b, "; ");
    int sv_cbt = hoist_ctor_block(c, id, b);
    for (int a = 0; a < argc; a++) {
      atmp[a] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", atmp[a]); emit_boxed(c, argv[a], b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", atmp[a]);
    }
    buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); switch(_t%d.cls_id){", rt2, kt);
    CtorArityArms aerr = {0};
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (is_builtin_reopen(c->classes[ci].name) || c->classes[ci].is_native_class) continue;
      if (emit_user_new_arm(c, id, ci, argc, atmp, 1, rt2, b)) continue;
      if (emit_exc_sub_new_arm(c, ci, argc, atmp, rt2, b)) continue;
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      /* a class whose constructor takes some other count: CRuby's
         ArgumentError, where the default arm raised NoMethodError */
      { char am[256];
        if (ctor_arity_error(c, ci, initm, argc, am, sizeof am)) {
          ctor_arity_add(&aerr, ci, am);
          continue;
        } }
      int np = initm >= 0 ? c->scopes[initm].nparams : 0;
      int nreq = initm >= 0 ? c->scopes[initm].nrequired : 0;
      /* A Struct or Data class has a GENERATED constructor taking its members
         positionally rather than an `initialize` scope, so the arity test above
         finds nothing and the class was skipped entirely -- the switch came out
         empty and `instance.class.new(...)` answered the nil seed (#3945). */
      int kw_ctor = 0;
      /* A Struct with its own initialize constructs through it, like any
         class: its sp_<S>_new takes that initialize's parameters. Skipping it
         here left `k.new(v)` on such a class to the NoMethodError default,
         while the splat form (emit_class_value_new_kw) built it. A yielding
         one is spliced at static sites only and has no constructor to call. */
      if (c->classes[ci].is_struct && initm >= 0 && c->scopes[initm].yields) continue;
      if (c->classes[ci].is_struct && initm < 0) {
        np = nreq = c->classes[ci].nreaders;
        /* a positional Hash standing for every member (keywords take the
           layout of emit_class_value_new_kw) */
        if (argc == 1 && argv && nt_kind(nt, argv[0]) == NK_HashNode) kw_ctor = 1;
      }
      /* An `initialize` with OPTIONAL parameters is reachable at any argc
         between its required count and its total, with the rest filled from
         their defaults -- which is what a statically known `Klass.new` has
         always done. The test here demanded argc == nparams == nrequired, so a
         constructor as ordinary as `initialize(path, initheader = nil)` matched
         no class at all, the switch came out EMPTY, and `k.new(x)` answered the
         nil seed with no diagnostic (#4417). The report read the trigger as
         nesting because the class that bit was `Net::HTTP::Get`; a top-level
         class with the same signature does it too.
         A struct's generated constructor has no scope to take defaults from, so
         it keeps the exact test, and a default that reads self cannot be
         evaluated at a call site. Both of those now decline into the raise
         below rather than into nil. */
      /* A plain Struct takes fewer members than it has (the rest are nil)
         and refuses more, as the static `S.new(...)` does; the exact test
         turned both into NoMethodError. Data and keyword_init keep it. */
      int nil_fill = initm < 0 && struct_nil_fills(&c->classes[ci]);
      if (nil_fill && argc > np) {
        buf_printf(b, "case %d: sp_raise_cls(\"ArgumentError\", \"struct size differs\"); break;", ci);
        continue;
      }
      if (!kw_ctor && !nil_fill) {
        if (initm < 0) {
          if (argc != np || nreq != np) continue;
        }
        else {
          if (!ctor_arm_takes(c, &c->scopes[initm], argc)) continue;
        }
      }
      /* the arguments' places by the call's layout, and no class whose
         parameters could not hold them (ctor_arm_incompat) */
      Scope *is2 = initm >= 0 ? &c->scopes[initm] : NULL;
      ArgLayout L;
      arg_layout(c, is2, NULL, argc, -1, 0, &L);
      if (ctor_arm_incompat(c, is2, &L, argv)) { arg_layout_free(&L); continue; }
      int pd_uid = is2 && default_refs_earlier_param(c, is2) ? ++g_tmp : 0, pd_base = g_nren;
      Buf pdpre; memset(&pdpre, 0, sizeof pdpre);
      Buf ab; memset(&ab, 0, sizeof ab);
      /* a default reading the instance: allocate first, then run initialize
         on the object with self bound to it, as emit_ctor_alloc_init does */
      const char *sv_cs = g_ctor_self, *sv_csd = g_ctor_self_deref;
      char selftxt[24];
      int self_t = -1;
      if (ctor_needs_self_defaults(c, initm, argc)) {
        self_t = ctor_alloc_decl(c, ci, &pdpre);
        snprintf(selftxt, sizeof selftxt, "_t%d", self_t);
        g_ctor_self = selftxt;
        g_ctor_self_deref = comp_ty_value_obj(c, ty_object(ci)) ? "." : "->";
      }
      Buf *sv_pre = g_pre;
      if (pd_uid || self_t >= 0) g_pre = &pdpre;
      for (int j = 0; j < np; j++) {
        if (j) buf_puts(&ab, ", ");
        if (is2) {
          Buf ub; memset(&ub, 0, sizeof ub);
          emit_ctor_arm_param(c, is2, j, &L, atmp, &pdpre, &ub);
          ctor_arm_arg(c, is2, j, ub.p ? ub.p : "", pd_uid, &pdpre, &ab); free(ub.p);
          continue;
        }
        TyKind pt = TY_POLY;
        /* a generated member constructor takes the member's own slot type */
        if (c->classes[ci].is_struct) {
          char mvn[300]; snprintf(mvn, sizeof mvn, "@%s", c->classes[ci].readers[j]);
          int mvi = comp_ivar_index(&c->classes[ci], mvn);
          if (mvi >= 0 && c->classes[ci].ivar_types[mvi] != TY_UNKNOWN)
            pt = c->classes[ci].ivar_types[mvi];
        }
        char tn[24];
        if (kw_ctor) {
          char probe[420];
          snprintf(probe, sizeof probe,
                   "sp_poly_hash_probe(_t%d, sp_box_sym(sp_sym_intern(\"%s\")), &_kwf)",
                   atmp[0], c->classes[ci].readers[j]);
          buf_puts(&ab, "({ sp_bool _kwf; sp_RbVal _kwv = ");
          buf_puts(&ab, probe);
          buf_puts(&ab, "; (void)_kwf; ");
          if (pt == TY_POLY) buf_puts(&ab, "_kwv");
          else emit_unbox_text(c, pt, "_kwv", &ab);
          buf_puts(&ab, "; })");
          continue;
        }
        if (j >= argc) { buf_puts(&ab, default_value_from_compiler(c, pt)); continue; }   /* a nil-filled member */
        snprintf(tn, sizeof tn, "_t%d", atmp[j]);
        if (pt != TY_POLY) emit_unbox_text(c, pt, tn, &ab);
        else buf_puts(&ab, tn);
      }
      arg_layout_free(&L);
      g_nren = pd_base;
      g_pre = sv_pre;
      g_ctor_self = sv_cs; g_ctor_self_deref = sv_csd;
      emit_ctor_block_slot(c, id, initm, np > 0 ? ", " : "", &ab);
      emit_ctor_arm_case(c, ci, rt2, self_t, pdpre.p, ab.p ? ab.p : "", b);
      free(pdpre.p); free(ab.p);
    }
    /* A class the switch has no arm for is a program that cannot construct it,
       which is a raise -- the sibling emitter below has always said so. Without
       this the seed fell out unchanged and the caller got nil, which is how
       #4417 presented: no crash, no diagnostic, a nil that only misbehaves
       later. */
    ctor_arity_emit(&aerr, b);
    emit_builtin_new_arms(c, argc, atmp, rt2, kt, 0, b);
    buf_printf(b, "} _t%d; })", rt2);
    g_ctor_blk_tmp = sv_cbt;
    free(atmp);
    return 1;
  }

  /* TY_CLASS variable .new -> runtime switch over user classes, returns TY_POLY.
     A local that statically holds one class is not that: it dispatches like the
     constant it holds, and the arm below has no case for a Struct, so a
     `s = Struct.new(:a); s.new` came out as an empty switch whose nil seed was
     then cast to the struct pointer (#3992). */
  if (recv >= 0 && sp_streq(name, "new") && comp_ntype(c, recv) == TY_CLASS &&
      nt_type(nt, recv) &&
      !sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      !sp_streq(nt_type(nt, recv), "ConstantPathNode") &&
      class_var_static_ci(c, recv) < 0 && self_class_static_ci(c, recv) < 0 &&
      argc == 0) {
    int kt = ++g_tmp, rt2 = ++g_tmp;
    buf_printf(b, "({ sp_Class _t%d = ", kt); emit_expr(c, recv, b); buf_printf(b, "; ");
    int sv_cbt = hoist_ctor_block(c, id, b);
    buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); ", rt2);
    buf_printf(b, "switch(_t%d.cls_id){", kt);
    CtorArityArms aerr = {0};
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (is_builtin_reopen(c->classes[ci].name)) continue;
      /* A MODULE has no `new`, and no constructor is emitted for one: giving it
         an arm called an sp_<Module>_new that nothing defines, and the program
         failed to link (#3965). */
      { int mdn = c->classes[ci].def_node;
        const char *mdt = mdn >= 0 ? nt_type(nt, mdn) : NULL;
        if (mdt && sp_streq(mdt, "ModuleNode")) continue; }
      if (emit_user_new_arm(c, id, ci, 0, NULL, 1, rt2, b)) continue;
      /* a zero-arg .new can only construct a class whose initialize takes no
         required args; an arg-requiring ctor is CRuby's ArgumentError, and its
         C function has parameters, so its arm raises (#2450) */
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      { char am[256];
        if (!c->classes[ci].is_native_class && ctor_arity_error(c, ci, initm, 0, am, sizeof am)) {
          ctor_arity_add(&aerr, ci, am);
          continue;
        } }
      if (initm >= 0 && c->scopes[initm].nrequired != 0) continue;
      if (c->classes[ci].is_native_class) continue;
      if (emit_ctor_splice_arm(c, id, ci, initm, rt2, b)) continue;
      /* A Struct DOES answer a zero-arg `new` -- every member takes nil, which
         is what the constant spelling emits. Skipping it left a local holding
         two anon struct classes (so no single one resolves) with an EMPTY
         switch: the call answered nil and the next write through it took the
         nil box's cls_id 0 as a real class (#4048). */
      if (c->classes[ci].is_struct) {
        int nmem = c->classes[ci].nmembers;
        buf_printf(b, "case %d: _t%d=sp_box_obj(sp_%s_new(", ci, rt2, c->classes[ci].c_name);
        for (int mq = 0; mq < nmem; mq++) {
          if (mq) buf_puts(b, ", ");
          TyKind mt = c->classes[ci].ivar_types[mq];
          buf_puts(b, (mt == TY_POLY || mt == TY_UNKNOWN) ? "sp_box_nil()" : default_value_from_compiler(c, mt));
        }
        buf_printf(b, "),%d);break;", ci);
        continue;
      }
      /* fill any optional constructor params with their defaults (an
         optional-arg initialize is zero-arg-compatible but its C function
         still declares the slots, #2452); the call site supplies no args, so
         emit_args_filled emits each param's default. A default reading the
         instance runs on the allocated object, as in the poly arm below. */
      if (ctor_needs_self_defaults(c, initm, 0)) {
        buf_printf(b, "case %d: _t%d=", ci, rt2);
        buf_printf(b, c->classes[ci].is_value_type ? "sp_box_vobj_%s(" : "sp_box_obj(",
                   c->classes[ci].c_name);
        emit_ctor_alloc_init(c, ci, initm, -1, id, b);
        if (c->classes[ci].is_value_type) buf_puts(b, "); break;");
        else buf_printf(b, ",%d); break;", ci);
        continue;
      }
      Buf ab9; memset(&ab9, 0, sizeof ab9);
      if (initm >= 0 && c->scopes[initm].nparams > 0)
        emit_args_filled(c, initm, -1, "", &ab9);
      emit_ctor_block_slot(c, id, initm, ab9.p && ab9.p[0] ? ", " : "", &ab9);
      const char *args9 = ab9.p ? ab9.p : "";
      /* a value-type object returns by value: box via its vobj boxer, not
         sp_box_obj which expects a heap pointer (#2450) */
      if (c->classes[ci].is_value_type)
        buf_printf(b, "case %d: _t%d=sp_box_vobj_%s(sp_%s_new(%s));break;",
                   ci, rt2, c->classes[ci].c_name, c->classes[ci].c_name, args9);
      else
        buf_printf(b, "case %d: _t%d=sp_box_obj(sp_%s_new(%s),%d);break;",
                   ci, rt2, c->classes[ci].c_name, args9, ci);
      free(ab9.p);
    }
    ctor_arity_emit(&aerr, b);
    emit_builtin_new_arms(c, 0, NULL, rt2, kt, 0, b);
    buf_printf(b, "} _t%d; })", rt2);
    g_ctor_blk_tmp = sv_cbt;
    return 1;
  }

  /* poly.new(args): the receiver is a Class value read out of a container
     (`REG["c"].new(2)`). Switch on its runtime cls_id and construct, coercing
     each argument to the target constructor's parameter type. Only a class whose
     constructor takes exactly `argc` positional params (all required) gets an
     arm; any other runtime class lands in the NoMethodError default, matching
     CRuby's ArgumentError/NoMethodError. (#2888) */
  if (recv >= 0 && sp_streq(name, "new") && comp_ntype(c, recv) == TY_POLY &&
      ctor_block_dispatchable(c, id)) {
    /* keyword arguments: laid out per class by name, as in the Class-valued
       form above (#4845) -- positionally they bound `k: v` to a parameter;
       likewise a `*splat`, which bound the whole array */
    if (call_has_keyword_args(nt, argv, argc) || call_has_splat_arg(nt, argv, argc) ||
        (argc > 0 && ctor_block_splices(c, id))) {
      emit_class_value_new_kw(c, id, recv, 1, b);
      return 1;
    }
    int kt = ++g_tmp, rt2 = ++g_tmp;
    int *atmp = argc ? calloc(argc, sizeof(int)) : NULL;
    buf_printf(b, "({ sp_RbVal _t%d = ", kt); emit_expr(c, recv, b); buf_puts(b, "; ");
    int sv_cbt = hoist_ctor_block(c, id, b);
    for (int a = 0; a < argc; a++) {
      atmp[a] = ++g_tmp;
      buf_printf(b, "sp_RbVal _t%d = ", atmp[a]); emit_boxed(c, argv[a], b); buf_puts(b, "; ");
    }
    buf_printf(b, "sp_RbVal _t%d = sp_box_nil(); switch(_t%d.cls_id){", rt2, kt);
    CtorArityArms aerr = {0};
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (is_builtin_reopen(c->classes[ci].name) || c->classes[ci].is_native_class) continue;
      /* a class built by its own `self.new` may never be instantiated at all */
      if (emit_user_new_arm(c, id, ci, argc, atmp, 1, rt2, b)) continue;
      if (emit_exc_sub_new_arm(c, ci, argc, atmp, rt2, b)) continue;
      int initm = comp_method_in_chain(c, ci, "initialize", NULL);
      { char am[256];   /* as in the Class-valued form above */
        if (ctor_arity_error(c, ci, initm, argc, am, sizeof am)) {
          ctor_arity_add(&aerr, ci, am);
          continue;
        } }
      if (!c->classes[ci].instantiated) continue;
      { int mdn = c->classes[ci].def_node;   /* a module has no `new` (#3965) */
        const char *mdt = mdn >= 0 ? nt_type(nt, mdn) : NULL;
        if (mdt && sp_streq(mdt, "ModuleNode")) continue; }
      int np = initm >= 0 ? c->scopes[initm].nparams : 0;
      int nreq = initm >= 0 ? c->scopes[initm].nrequired : 0;
      if (argc >= nreq && emit_ctor_splice_arm(c, id, ci, initm, rt2, b)) continue;
      /* A Struct or Data class had no arm here at all, so `{0 => S}.fetch(0)
         .new(5)` raised NoMethodError. Its generated constructor takes the
         members positionally, with the exact test the class-value emitter
         above gives it; one with its own initialize constructs through that
         like any class, and a yielding one has no constructor to call. */
      int gen_ctor = c->classes[ci].is_struct && initm < 0;
      if (c->classes[ci].is_struct && initm >= 0 && c->scopes[initm].yields) continue;
      if (gen_ctor) np = nreq = c->classes[ci].nreaders;
      /* A zero-arg construction also reaches a constructor whose params are all
         optional: the arm fills each with its default, exactly as the
         statically-known `Klass.new` does. */
      if (argc == 0 && nreq == 0 && np > 0) {
        buf_printf(b, "case %d: _t%d=", ci, rt2);
        if (ctor_needs_self_defaults(c, initm, 0)) {
          buf_printf(b, c->classes[ci].is_value_type ? "sp_box_vobj_%s(" : "sp_box_obj(",
                     c->classes[ci].c_name);
          emit_ctor_alloc_init(c, ci, initm, -1, id, b);
          if (c->classes[ci].is_value_type) buf_puts(b, "); break;");
          else buf_printf(b, ",%d); break;", ci);
          continue;
        }
        Buf ad; memset(&ad, 0, sizeof ad);
        emit_args_filled(c, initm, -1, "", &ad);
        emit_ctor_block_slot(c, id, initm, ad.p && ad.p[0] ? ", " : "", &ad);
        if (c->classes[ci].is_value_type)
          buf_printf(b, "sp_box_vobj_%s(sp_%s_new(%s)); break;",
                     c->classes[ci].c_name, c->classes[ci].c_name, ad.p ? ad.p : "");
        else
          buf_printf(b, "sp_box_obj(sp_%s_new(%s),%d); break;",
                     c->classes[ci].c_name, ad.p ? ad.p : "", ci);
        free(ad.p);
        continue;
      }
      /* Same rule as the class-value emitter above: optional parameters are
         filled from their defaults rather than making the class unreachable,
         and a plain Struct nil-fills its missing members. */
      int nil_fill = gen_ctor && struct_nil_fills(&c->classes[ci]);
      if (nil_fill && argc > np) {
        buf_printf(b, "case %d: sp_raise_cls(\"ArgumentError\", \"struct size differs\"); break;", ci);
        continue;
      }
      if (initm < 0) { if (!nil_fill && (argc != np || nreq != np)) continue; }
      else {
        if (!ctor_arm_takes(c, &c->scopes[initm], argc)) continue;
      }
      /* as in the Class-valued form above */
      Scope *is = initm >= 0 ? &c->scopes[initm] : NULL;
      ArgLayout L;
      arg_layout(c, is, NULL, argc, -1, 0, &L);
      if (ctor_arm_incompat(c, is, &L, argv)) { arg_layout_free(&L); continue; }
      int pd_uid = is && default_refs_earlier_param(c, is) ? ++g_tmp : 0, pd_base = g_nren;
      Buf pdpre; memset(&pdpre, 0, sizeof pdpre);
      Buf ab; memset(&ab, 0, sizeof ab);
      /* a default reading the instance: allocate first, then run initialize
         on the object with self bound to it, as emit_ctor_alloc_init does */
      const char *sv_cs = g_ctor_self, *sv_csd = g_ctor_self_deref;
      char selftxt[24];
      int self_t = -1;
      if (ctor_needs_self_defaults(c, initm, argc)) {
        self_t = ctor_alloc_decl(c, ci, &pdpre);
        snprintf(selftxt, sizeof selftxt, "_t%d", self_t);
        g_ctor_self = selftxt;
        g_ctor_self_deref = comp_ty_value_obj(c, ty_object(ci)) ? "." : "->";
      }
      Buf *sv_pre = g_pre;
      if (pd_uid || self_t >= 0) g_pre = &pdpre;
      for (int j = 0; j < np; j++) {
        if (j) buf_puts(&ab, ", ");
        if (is) {
          Buf ub; memset(&ub, 0, sizeof ub);
          emit_ctor_arm_param(c, is, j, &L, atmp, &pdpre, &ub);
          ctor_arm_arg(c, is, j, ub.p ? ub.p : "", pd_uid, &pdpre, &ab); free(ub.p);
          continue;
        }
        TyKind pt = TY_POLY;
        /* a generated member constructor takes the member's own slot type */
        if (gen_ctor) {
          char mvn[300]; snprintf(mvn, sizeof mvn, "@%s", c->classes[ci].readers[j]);
          int mvi = comp_ivar_index(&c->classes[ci], mvn);
          if (mvi >= 0 && c->classes[ci].ivar_types[mvi] != TY_UNKNOWN)
            pt = c->classes[ci].ivar_types[mvi];
        }
        if (j >= argc) { buf_puts(&ab, default_value_from_compiler(c, pt)); continue; }   /* a nil-filled member */
        char tn[24]; snprintf(tn, sizeof tn, "_t%d", atmp[j]);
        emit_unbox_text(c, pt, tn, &ab);
      }
      arg_layout_free(&L);
      g_nren = pd_base;
      g_pre = sv_pre;
      g_ctor_self = sv_cs; g_ctor_self_deref = sv_csd;
      emit_ctor_block_slot(c, id, initm, np > 0 ? ", " : "", &ab);
      emit_ctor_arm_case(c, ci, rt2, self_t, pdpre.p, ab.p ? ab.p : "", b);
      free(pdpre.p); free(ab.p);
    }
    ctor_arity_emit(&aerr, b);
    emit_builtin_new_arms(c, argc, atmp, rt2, kt, 1, b);
    buf_printf(b, "} _t%d; })", rt2);
    g_ctor_blk_tmp = sv_cbt;
    free(atmp);
    return 1;
  }

  /* self.class.new(args) in a leaf-class instance method -> construct the
     enclosing class statically (no subclass can shadow it at runtime). */
  /* Class#allocate: a bare instance with default/nil ivars and no initialize.
     Exception subclasses carry raise/message state set up by their dedicated
     constructor, so they are excluded (fall through to the generic reject). */
  if (recv >= 0 && sp_streq(name, "allocate")) {
    int oc = allocate_on_own_class(c, id);
    if (oc >= 0 && !class_is_exc_subclass(c, oc)) { emit_own_class_alloc(c, id, oc, b); return 1; }
  }
  if (recv >= 0 && sp_streq(name, "allocate") && argc == 0 && comp_ntype(c, recv) == TY_CLASS &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    int acid = comp_class_index(c, nt_str(nt, recv, "name"));
    if (acid >= 0 && !class_is_exc_subclass(c, acid)) {
      emit_obj_alloc_expr(c, acid, b);
      return 1;
    }
    /* builtin allocables: the empty value of the class (#2655). CRuby's
       un-allocatable builtins (Integer, Symbol, ...) keep their TypeError
       path by falling through. */
    if (acid < 0) {
      const char *bcn = nt_str(nt, recv, "name");
      if (bcn && sp_streq(bcn, "String")) { buf_puts(b, "sp_str_dup_external((&(\"\\xff\")[1]))"); return 1; }
      if (bcn && sp_streq(bcn, "Array"))  { buf_puts(b, "sp_PolyArray_new()"); return 1; }
      if (bcn && sp_streq(bcn, "Hash"))   { buf_puts(b, "sp_PolyPolyHash_new()"); return 1; }
      if (bcn && sp_streq(bcn, "Object")) { buf_puts(b, "sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT)"); return 1; }
    }
  }

  if (recv >= 0 && sp_streq(name, "new") && self_class_static_ci(c, recv) >= 0) {
    int cid = self_class_static_ci(c, recv);
    buf_printf(b, "sp_%s_new(", c->classes[cid].c_name);
    /* the arguments converted to initialize's parameter types, as
       `V.new(...)` does: an `x.to_i` narrowed to sp_int passed raw into a
       poly parameter did not compile (#4534) */
    int initm = comp_method_in_chain(c, cid, "initialize", NULL);
    if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
    else for (int a = 0; a < argc; a++) { if (a) buf_puts(b, ", "); emit_expr(c, argv[a], b); }
    if (initm >= 0) emit_ctor_block_slot(c, id, initm, c->scopes[initm].nparams > 0 ? ", " : "", b);
    buf_puts(b, ")");
    return 1;
  }

  /* namespaced class M::Sub.new -> check for user-defined `def self.new` first,
     then fall back to sp_<Sub>_new(args) */
  if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    /* native (C-backed) class reached as ::Name (root-qualified) */
    if (emit_native_ctor(c, id, ci, argc, argv, b)) return 1;
    if (ci >= 0) {
      if (class_is_exc_subclass(c, ci)) {
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) {
          /* user initialize: call the generated sp_ClassName_new(args) constructor */
          buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
          emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
          emit_ctor_block_slot(c, id, initm, c->scopes[initm].nparams > 0 ? ", " : "", b);
          buf_puts(b, ")");
        }
        else {
          /* no user initialize: create directly with first arg as message.
             An ivar-bearing subclass needs its dedicated struct size (#2772). */
          const char *cn2 = class_ruby_name(c, ci); if (!cn2) cn2 = c->classes[ci].name;
          const char *par = exc_builtin_parent(c, ci);
          if (c->classes[ci].nivars > 0)
            buf_printf(b, "((sp_%s *)sp_exc_new_sub_sized(sizeof(sp_%s), \"%s\", ",
                       c->classes[ci].c_name, c->classes[ci].c_name, cn2);
          else
            buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", ", cn2, par);
          if (class_is_syserr(c, ci)) {
            /* SystemCallError#initialize: the errno text, " - msg" */
            char lead[192]; snprintf(lead, sizeof lead, "\"%s\", ", cn2);
            emit_syserr_call(c, id, "sp_syserr_msg_a", lead, argc, argv, b);
          }
          else emit_exc_msg_arg(c, argc >= 1 ? argv[0] : -1, b);
          buf_puts(b, c->classes[ci].nivars > 0 ? "))" : ")");
        }
        return 1;
      }
      int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
      if (ucnew >= 0) {
        /* user-defined def self.new: call it as a regular class method */
        emit_method_cname(c, &c->scopes[ucnew], b);
        buf_puts(b, "(");
        const char *ld = emit_cmethod_self_cls_arg(c, ucnew, ci, b);   /* #4217 */
        emit_args_filled(c, ucnew, nt_ref(nt, id, "arguments"), ld, b);
        /* a `self.new` that keeps a named `&blk` takes it as a trailing C
           parameter, the same slot a stored-block initialize gets; leaving it
           out emitted a call with one argument too few and the C build stopped */
        emit_ctor_block_slot(c, id, ucnew, ctor_blk_lead(b), b);
        buf_puts(b, ")");
        return 1;
      }
      if (!c->classes[ci].is_struct) {
        /* the generic constructor path first: it fills the defaults, threads a
           literal or forwarded block into a stored-block initialize and passes
           NULL for an absent one. This arm ran ahead of it for a constant PATH
           and rendered the positional arguments alone, so `NS::Reg.new` and
           `NS::Reg.new { ... }` on an `initialize(o = nil, &blk)` were calls
           with too few arguments (a plain `Reg.new` never came this way). */
        if (emit_or_take_back(c, id, b, emit_class_new_call)) return 1;
        buf_printf(b, "sp_%s_new(", c->classes[ci].c_name);
        int initm = comp_method_in_chain(c, ci, "initialize", NULL);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        if (initm >= 0) emit_ctor_block_slot(c, id, initm, c->scopes[initm].nparams > 0 ? ", " : "", b);
        buf_puts(b, ")");
        return 1;
      }
    }
    if (ci < 0) {
      const char *qn = superclass_builtin_exc_name(nt, recv);
      if (qn) cn = qn;
    }
    if (cn && is_exc_name(cn)) {
      if (emit_syserr_family_new(c, id, cn, argc, argv, b)) return 1;
      buf_printf(b, "sp_exc_new(\"%s\", ", cn);
      emit_exc_msg_arg(c, argc >= 1 ? argv[0] : -1, b);
      buf_puts(b, ")");
      return 1;
    }
  }

  /* Cls.exception(msg) is Cls.new for a builtin exception reached by its
     path, too (Errno::ENOENT.exception) */
  if (recv >= 0 && sp_streq(name, "exception") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "ConstantPathNode") &&
      comp_class_index(c, nt_str(nt, recv, "name")) < 0) {
    const char *qn = superclass_builtin_exc_name(nt, recv);
    if (qn) {
      if (emit_syserr_family_new(c, id, qn, argc, argv, b)) return 1;
      buf_printf(b, "sp_exc_new(\"%s\", ", qn);
      emit_exc_msg_arg(c, argc >= 1 ? argv[0] : -1, b);
      buf_puts(b, ")");
      return 1;
    }
  }
  return 0;
}

/* superclass, class, the comparison of two classes, and the methods of a Class value (TY_CLASS) */
int emit_call_class_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* SomeClass.superclass -> the parent class as sp_Class value */
  if (recv >= 0 && argc == 0 && sp_streq(name, "superclass") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name")) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (ci >= 0) {
      int par = c->classes[ci].parent;
      if (par >= 0) { buf_printf(b, "((sp_Class){%d})", par); return 1; }
      /* the builtin above it: the row the generated sp_class_superclass
         table carries, by name for an id-less exception (LoadError) */
      const char *bpn = class_builtin_superclass_name(c, ci);
      if (bpn) buf_printf(b, "((sp_Class){-1, SPL(\"%s\")})", bpn);
      else buf_printf(b, "((sp_Class){%d})", class_builtin_superclass(c, ci));
      return 1;
    }
  }

  /* x.class -> the class-name string (compile-time for known types) */
  /* top-level `self.class` (and inside a top-level def): self is main, an
     Object instance, so the class is Object -- the SelfNode has no C slot at
     top level and previously fell through unsupported (#3035) */
  if (recv >= 0 && sp_streq(name, "class") && argc == 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode") &&
      self_is_main(c, recv)) {
    buf_puts(b, "((sp_Class){(sp_int)-116, SPL(\"Object\")})");
    return 1;
  }
  if (recv >= 0 && sp_streq(name, "class") && argc == 0 &&
      !obj_member_shadows(c, comp_recv_type(c, recv), "class")) {
    TyKind rt = comp_recv_type(c, recv);  /* empty-literal receivers coerce */
    /* When emitting a scope transplanted from a builtin-reopen class (Object/Array/
       Numeric), self is sp_RbVal even if the nscope-based type says otherwise.
       Override the inferred type to TY_POLY so we get sp_poly_class_name(self).
       Exception: TrueClass/FalseClass use int self; keep TY_BOOL for ternary. */
    if (g_emitting_class_id >= 0 &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode") &&
        is_builtin_reopen(c->classes[g_emitting_class_id].name)) {
      const char *ecn = c->classes[g_emitting_class_id].name;
      /* an IO reopening's self is the sp_File handle, typed so */
      if (!sp_streq(ecn, "TrueClass") && !sp_streq(ecn, "FalseClass") && !io_family_class(c, g_emitting_class_id))
        rt = TY_POLY;
    }
    /* MatchData is nullable (nil on no-match): .class checks at run time so a
       non-match reports NilClass, not MatchData (#2311) */
    if (rt == TY_MATCHDATA) {
      int tm = ++g_tmp;
      buf_printf(b, "({ sp_MatchData *_t%d = ", tm); emit_expr(c, recv, b);
      buf_printf(b, "; _t%d ? ((sp_Class){(sp_int)-1, SPL(\"MatchData\")})"
                    " : ((sp_Class){(sp_int)-1, SPL(\"NilClass\")}); })", tm);
      return 1;
    }
    const char *cn = NULL;
    switch (rt) {
    case TY_INT: cn = "Integer"; break;
    case TY_FLOAT: cn = "Float"; break;
    case TY_STRING: cn = "String"; break;
    case TY_SYMBOL: cn = "Symbol"; break;
    case TY_RANGE: cn = "Range"; break;
    case TY_TIME: cn = "Time"; break;
    case TY_FIBER: cn = "Fiber"; break;
    case TY_ENUMERATOR: {
      /* a chain built by Enumerable#chain / Enumerator#+ reports as
         Enumerator::Chain, and a product Enumerator::Product; every other
         enumerator is an Enumerator (#2545). The slot holds NULL for nil, as
         `v&.each` answers for a nil `v`: that is a NilClass, as a MatchData
         slot's no-match is. */
      int te = ++g_tmp;
      buf_printf(b, "({ sp_Enumerator *_t%d = ", te); emit_expr(c, recv, b);
      buf_printf(b, "; !_t%d ? ((sp_Class){(sp_int)-1, SPL(\"NilClass\")})"
                    " : _t%d->is_chain ? ((sp_Class){(sp_int)-1, SPL(\"Enumerator::Chain\")})"
                    " : _t%d->is_product ? ((sp_Class){(sp_int)-1, SPL(\"Enumerator::Product\")})"
                    " : ((sp_Class){(sp_int)-1, SPL(\"Enumerator\")}); })", te, te, te);
      return 1;
    }
    case TY_DIR: cn = "Dir"; break;
    case TY_ADDRINFO: cn = "Addrinfo"; break;
    case TY_SOCKOPT: cn = "Socket::Option"; break;
    case TY_OPENSTRUCT: cn = "OpenStruct"; break;
    case TY_TMS: cn = "Process::Tms"; break;
    case TY_THREAD: cn = "Thread"; break;
    case TY_QUEUE: {
      /* Queue and SizedQueue share one runtime object, so the name comes from
         the bound rather than the static type -- a SizedQueue reported itself
         as Thread::Queue (#3466). */
      int tq2 = ++g_tmp;
      buf_printf(b, "({ sp_queue *_t%d = ", tq2); emit_expr(c, recv, b);
      /* a NULL slot is nil, whose class is NilClass */
      if (node_may_be_null_nil(c, recv))
        buf_printf(b, "; _t%d ? ((sp_Class){(sp_int)%d, sp_Queue_class_name(_t%d)})"
                      " : ((sp_Class){(sp_int)-1, SPL(\"NilClass\")}); })",
                   tq2, builtin_class_id("Queue"), tq2);
      else
        buf_printf(b, "; ((sp_Class){(sp_int)%d, sp_Queue_class_name(_t%d)}); })",
                   builtin_class_id("Queue"), tq2);
      return 1;
    }
    case TY_MUTEX: cn = "Thread::Mutex"; break;
    case TY_CONDVAR: cn = "Thread::ConditionVariable"; break;
    case TY_IO: {
      /* a stat handle is a File::Stat (#2841); a path-backed handle is a
         File; a raw stream (STDOUT, pipe end) is an IO (#2797) */
      /* The handle kind names its class; sp_io_kind_name is the single
         authority (it also drives #is_a? and the NoMethodError texts). Each
         socket class carries its real builtin id so the class VALUE walks the
         same ancestor chain a constant does. */
      int tio = ++g_tmp;
      buf_printf(b, "({ sp_File *_t%d = ", tio); emit_expr(c, recv, b);
      buf_printf(b, "; const char *_k%d = (_t%d && _t%d->mode &&"
                    " (strcmp(_t%d->mode, \"stat\") == 0 || strcmp(_t%d->mode, \"lstat\") == 0))"
                    " ? SPL(\"File::Stat\") : sp_io_kind_name(_t%d); ",
                 tio, tio, tio, tio, tio, tio);
      static const struct { const char *k; int id; } IO_KIND_CLS[] = {
        { "TCPSocket", -168 }, { "TCPServer", -169 }, { "UDPSocket", -170 },
        { "UNIXSocket", -171 }, { "UNIXServer", -172 }, { "Socket", -173 },
        { "File", -121 }, { "IO", -120 }, { NULL, 0 }
      };
      for (int ki = 0; IO_KIND_CLS[ki].k; ki++)
        buf_printf(b, "strcmp(_k%d, \"%s\") == 0 ? ((sp_Class){(sp_int)%d, NULL}) : ",
                   tio, IO_KIND_CLS[ki].k, IO_KIND_CLS[ki].id);
      buf_printf(b, "((sp_Class){(sp_int)-1, _k%d}); })", tio);
      return 1;
    }
    case TY_ARGF: cn = "ARGF.class"; break;  /* ARGF's singleton class name (CRuby) */
    case TY_NIL: cn = "NilClass"; break;
    case TY_METHOD: cn = method_expr_is_unbound(c, recv) ? "UnboundMethod" : "Method"; break;
    case TY_MATCHDATA: cn = "MatchData"; break;
    case TY_REGEX: cn = "Regexp"; break;
    case TY_PROC: case TY_CURRY: cn = "Proc"; break;  /* a curried proc is a Proc (#2651) */
    case TY_COMPLEX: cn = "Complex"; break;
    case TY_RATIONAL: cn = "Rational"; break;
    default: break;
    }
    if (ty_is_array(rt)) cn = "Array";
    else if (ty_is_hash(rt)) cn = "Hash";
    else if (ty_is_object(rt)) {
      /* user object: .class returns a TY_CLASS value */
      int _cidx = ty_object_class(rt);
      /* a value-type instance has the class's static cls_id (no NULL case) */
      if (comp_ty_value_obj(c, rt)) { buf_printf(b, "((sp_Class){%d})", _cidx); return 1; }
      /* A bare Object/BasicObject instance uses the runtime sp_Object struct
         ({uint8_t _pad}) -- it has no cls_id field to read (every generated
         user-class struct does). Its class is statically that base, so emit the
         name-backed value and side-effect-eval the receiver. */
      const char *_ocn = _cidx >= 0 && _cidx < c->nclasses ? c->classes[_cidx].name : NULL;
      if (_ocn && (sp_streq(_ocn, "Object") || sp_streq(_ocn, "BasicObject"))) {
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(sp_int)-1, SPL(\"%s\")}))", _ocn);
        return 1;
      }
      /* a native-bound class's struct is opaque in the generated TU (no
         cls_id deref possible); its class is statically known. The carried
         name is the qualified Ruby one (a native_struct may declare
         "IO::Buffer" over a leaf-keyed class). */
      if (_cidx >= 0 && c->classes[_cidx].is_native_class) {
        const char *_nqn = class_ruby_name(c, _cidx);
        buf_puts(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), ((sp_Class){(sp_int)%d, SPL(\"%s\")}))", _cidx,
                   _nqn ? _nqn : c->classes[_cidx].name);
        return 1;
      }
      /* an exception subclass shares sp_Exception's layout (no cls_id
         member); its runtime class is the carried cls_name */
      if (_cidx >= 0 && class_is_exc_subclass(c, _cidx)) {
        /* evaluate the receiver BEFORE writing the temp's declaration head:
           its emission may hoist declarations of its own into g_pre */
        int _texc = ++g_tmp;
        Buf _eb = expr_buf(c, recv);
        emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", _texc);
        buf_puts(g_pre, _eb.p ? _eb.p : ""); buf_puts(g_pre, ";\n"); free(_eb.p);
        buf_printf(b, "((sp_Class){(sp_int)-1, _t%d ? sp_exc_class_name((sp_Exception *)_t%d) : SPL(\"NilClass\")})", _texc, _texc);
        return 1;
      }
      int _tobj = ++g_tmp;
      Buf _rb = expr_buf(c, recv);
      emit_ctype(c, rt, g_pre); buf_printf(g_pre, " _t%d = ", _tobj);
      buf_puts(g_pre, _rb.p ? _rb.p : ""); buf_puts(g_pre, ";\n"); free(_rb.p);
      /* a heap object slot's NULL is nil, whose class is NilClass (self is
         never nil) */
      buf_printf(b, "((sp_Class){_t%d ? _t%d->cls_id : %d})", _tobj, _tobj,
                 comp_ty_value_obj(c, rt) || nt_kind(c->nt, recv) == NK_SelfNode
                   ? _cidx : builtin_class_id("NilClass"));
      return 1;
    }
    if (cn && rt == TY_INT) {
      /* an int slot can hold the nil sentinel (a nil-returning <=>, a
         missing key): report NilClass then, matching how p prints it */
      int tcv = ++g_tmp;
      buf_printf(b, "({ sp_int _t%d = ", tcv); emit_expr(c, recv, b);
      buf_printf(b, "; ((sp_Class){(sp_int)-1, _t%d == SP_INT_NIL ? SPL(\"NilClass\") : SPL(\"Integer\")}); })", tcv);
      return 1;
    }
    if (cn && rt == TY_FLOAT) {
      /* same for the float nil sentinel (NaN-boxed nil) */
      int tcv = ++g_tmp;
      buf_printf(b, "({ sp_float _t%d = ", tcv); emit_expr(c, recv, b);
      buf_printf(b, "; ((sp_Class){(sp_int)-1, sp_float_is_nil(_t%d) ? SPL(\"NilClass\") : SPL(\"Float\")}); })", tcv);
      return 1;
    }
    if (cn && ty_null_is_nil(rt) && node_may_be_null_nil(c, recv)) {
      /* NULL is this pointer slot's nil, whose class is NilClass */
      int tcv = ++g_tmp;
      Buf crb = expr_buf(c, recv);
      buf_puts(b, "({ "); emit_ctype(c, rt, b);
      buf_printf(b, " _t%d = %s; ((sp_Class){(sp_int)-1, _t%d ? SPL(\"%s\") : SPL(\"NilClass\")}); })",
                 tcv, crb.p ? crb.p : "0", tcv, cn);
      free(crb.p);
      return 1;
    }
    if (cn) {
      /* a first-class name-backed Class value; the receiver is side-effect-
         evaluated when it is not a plain read */
      buf_puts(b, "((void)("); emit_expr(c, recv, b);
      buf_printf(b, "), ((sp_Class){(sp_int)-1, SPL(\"%s\")}))", cn);
      return 1;
    }
    if (rt == TY_BOOL) {
      buf_puts(b, "(("); emit_expr(c, recv, b);
      buf_puts(b, ") ? ((sp_Class){(sp_int)-1, SPL(\"TrueClass\")}) : ((sp_Class){(sp_int)-1, SPL(\"FalseClass\")}))");
      return 1;
    }
    if (rt == TY_POLY) {
      buf_puts(b, "sp_poly_class_val("); emit_expr(c, recv, b); buf_puts(b, ")");
      return 1;
    }
  }

  /* Module#< / <= / > / >= / <=> with one side boxed: a class value read out
     of an Array or Hash ordered against a class, or a class ordered against
     a boxed operand. Answered at run time with the tri-state result. A class
     that defines the operator itself (`def self.<(o)`) keeps its own method,
     which the class-method dispatch below calls. */
  if (recv >= 0 && argc == 1 && (is_cmp_op(name) || sp_streq(name, "<=>")) &&
      comp_ntype(c, id) == TY_POLY) {
    TyKind crt = comp_ntype(c, recv), cat = comp_ntype(c, argv[0]);
    int own_op = 0;
    if (crt == TY_CLASS) {
      int oci = class_recv_static_ci(c, recv);
      own_op = oci >= 0 && comp_cmethod_in_chain(c, oci, name, NULL) >= 0;
    }
    if (!own_op && ((crt == TY_CLASS && cat != TY_CLASS) || (crt == TY_POLY && cat == TY_CLASS))) {
      int op = sp_streq(name, "<") ? 0 : sp_streq(name, "<=") ? 1 :
               sp_streq(name, ">") ? 2 : sp_streq(name, ">=") ? 3 : 4;
      buf_puts(b, "sp_class_op_rv("); emit_boxed(c, recv, b);
      buf_puts(b, ", "); emit_boxed(c, argv[0], b);
      buf_printf(b, ", %d)", op);
      return 1;
    }
  }

  /* TY_CLASS method dispatch */
  if (recv >= 0 && comp_ntype(c, recv) == TY_CLASS) {
    int _clt = ++g_tmp;
    /* A user-defined singleton (def self.name / to_s / inspect) shadows the
       builtin stringification; a statically-named receiver falls through to
       the class-method dispatch below. */
    int cls_shadowed = 0;
    if (nt_type(nt, recv) &&
        (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
         sp_streq(nt_type(nt, recv), "ConstantPathNode")) &&   /* `Outer::Nested.name(x)`: the leaf keys the class (#4526) */
        nt_str(nt, recv, "name")) {
      int sci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (sci >= 0 && comp_cmethod_in_chain(c, sci, name, NULL) >= 0) cls_shadowed = 1;
    }
    if (!cls_shadowed &&
        is_name_reader(name)) {
      /* An anonymous Struct/Data class has no name: CRuby answers nil for
         #name (and an `#<Class:0x...>`-shaped #to_s). The synthetic
         StructAnon_<n> the compiler keys it by is not a Ruby-visible name. */
      if (sp_streq(name, "name")) {
        /* the anon class is registered under a name keyed by the node that
           produced it (analyze_scope), so the receiver node identifies it */
        int acid = -1;
        { char an[64]; snprintf(an, sizeof an, "StructAnon_%s", comp_node_tag(c, recv));
          int rcid = comp_class_index(c, an);
          if (rcid >= 0 && rcid < c->nclasses && c->classes[rcid].is_anon_struct) acid = rcid; }
        /* the class is just as anonymous when a local holds it (`k =
           Struct.new(:a); k.name`): resolve the read to the same class the
           write registered (#3827) */
        if (acid < 0) {
          int rcid = class_var_static_ci(c, recv);
          if (rcid >= 0 && rcid < c->nclasses && c->classes[rcid].is_anon_struct) acid = rcid;
        }
        if (acid >= 0) {
          buf_puts(b, "((void)("); emit_expr(c, recv, b);
          buf_puts(b, "), (const char *)NULL)");
          return 1;
        }
      }
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_%s(_cl%d); })",
                 sp_streq(name, "name") ? "name_or_nil" : "to_s", _clt);
      return 1;
    }
    if (sp_streq(name, "nil?")) {
      /* a class value is nil only when it is the nil-class sentinel
         (BasicObject#superclass); every real class is non-nil (#2654) */
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_nil_p(_cl%d); })", _clt);
      return 1;
    }
    /* Thread.current / Fiber.current through a class value -- activesupport's
       IsolatedExecutionState keeps its scope as the class itself,
       `@scope.current.active_support_execution_state` -- answers the boxed
       handle of the class the value names; a user class method of that name
       keeps the dispatch below. */
    if (sp_streq(name, "current") && argc == 0 && nt_type(nt, recv) &&
        !sp_streq(nt_type(nt, recv), "ConstantReadNode") && !sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
      int ncc = 0; comp_cmethod_candidates(c, name, &ncc);
      if (ncc == 0) {
        char cid_expr[40]; snprintf(cid_expr, sizeof cid_expr, "_cl%d.cls_id", _clt);
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_puts(b, "; "); emit_class_id_is(c, "Thread", cid_expr, b); buf_puts(b, " ? ");
        emit_boxed_text(c, TY_THREAD, "sp_Thread_current()", b);
        buf_puts(b, " : "); emit_class_id_is(c, "Fiber", cid_expr, b); buf_puts(b, " ? ");
        emit_boxed_text(c, TY_FIBER, "sp_fiber_current", b);
        buf_puts(b, " : (sp_raise_cls(\"NoMethodError\", (&(\"\\xff\" \"undefined method 'current' for class\")[1])), sp_box_nil()); })");
        return 1;
      }
    }
    /* Class/Module#freeze flips the per-class runtime flag (a class value is
       an unboxed {cls_id, name}, so the flag lives in a global map); frozen?
       reads it back (#3101). */
    if (sp_streq(name, "freeze") && argc == 0) {
      int _cft = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _cft); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_freeze_id(_cl%d.cls_id); _cl%d; })", _cft, _cft);
      return 1;
    }
    if (sp_streq(name, "frozen?") && argc == 0) {
      int _cft = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _cft); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_frozen_id(_cl%d.cls_id); })", _cft);
      return 1;
    }
    /* const_defined?(:NAME) with a literal name answers at compile time from
       the (flat) constant and class tables -- constants carry no class
       qualifier in the registry, so this is the same namespace a read
       resolves against. */
    if (sp_streq(name, "const_defined?") && argc == 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      const char *cn0 = NULL;
      if (a0ty && sp_streq(a0ty, "SymbolNode")) cn0 = nt_str(nt, argv[0], "value");
      else if (a0ty && sp_streq(a0ty, "StringNode")) cn0 = nt_str(nt, argv[0], "unescaped");
      if (cn0) {
        if (const_name_is_wrong(cn0)) {
          buf_printf(b, "((void)("); emit_expr(c, recv, b);
          buf_printf(b, "), sp_raise_cls(\"NameError\", sp_sprintf(\"wrong constant name %%s\", ");
          emit_str_literal(b, cn0);
          buf_printf(b, ")), 0)");
          return 1;
        }
        int yes = (comp_const(c, cn0) != NULL) || comp_class_index(c, cn0) >= 0 || comp_is_wellknown_const(cn0);
        buf_printf(b, "((void)("); emit_expr(c, recv, b); buf_printf(b, "), %d)", yes);
        return 1;
      }
    }
    if (sp_streq(name, "class")) {
      buf_printf(b, "({ sp_Class _cl%da = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%da)"
                    "?((sp_Class){(sp_int)-1, SPL(\"Module\")})"
                    ":((sp_Class){(sp_int)-1, SPL(\"Class\")}); })", _clt);
      return 1;
    }
    if (sp_streq(name, "superclass") && argc == 0) {
      /* sp_class_superclass only knows the user chain; a builtin class needs
         sp_builtin_superclass (Integer -> Numeric), as sp_class_is_ancestor
         already dispatches. BasicObject (the root) yields the nil-class
         sentinel there (#2654). A Module has no #superclass -> NoMethodError. */
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_is_module_val(_cl%d) ? "
                    "(sp_raise_cls(\"NoMethodError\", sp_sprintf(\"undefined method 'superclass' for module %%s\", sp_class_to_s(_cl%d))), (sp_Class){0}) : "
                    "(_cl%d.cls_id>=0?sp_class_superclass(_cl%d):sp_builtin_superclass(_cl%d)); })",
                 _clt, _clt, _clt, _clt, _clt);
      return 1;
    }
    if (sp_streq(name, "ancestors") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_ancestors(_cl%d); })", _clt);
      return 1;
    }
    /* Module#included_modules: the module ancestors (#2674) */
    if (sp_streq(name, "included_modules") && argc == 0) {
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_class_included_modules(_cl%d); })", _clt);
      return 1;
    }
    /* Module#constants: the constant registry is a flat namespace with no class
       qualifier, so recover the ownership from the AST -- a class/module's own
       constants are the ConstantWriteNodes in its body (across reopenings).
       CRuby lists them per ancestor in ancestors order (own, then prepended and
       included modules, then the superclass chain), stopping before Object,
       whose constants are the top-level ones. `constants(false)` is own-only.
       #2674 */
    if (sp_streq(name, "constants") && argc <= 1) {
      const char *rcn = nt_type(nt, recv) &&
                        (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
                         sp_streq(nt_type(nt, recv), "ConstantPathNode"))
                        ? nt_str(nt, recv, "name") : NULL;
      int rci = rcn ? comp_class_index(c, rcn) : -1;
      int inherit = 1;
      if (argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        if (aty && sp_streq(aty, "FalseNode")) inherit = 0;
        else if (!aty || !sp_streq(aty, "TrueNode")) rci = -1;  /* non-literal: leave it */
      }
      if (rci >= 0) {
        const char *names[128];
        int n = collect_class_constants(c, rci, inherit, names, 128, 0, 0);
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        /* intern at runtime: a constant name is not otherwise a symbol in the
           program, so comp_sym_intern here would come too late for the
           generated name table and the symbol would render empty */
        for (int k = 0; k < n; k++) {
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", ta);
          emit_str_literal(b, names[k]);
          buf_puts(b, ")));");
        }
        buf_printf(b, " _t%d; })", ta);
        return 1;
      }
    }
    /* ClassName.{,public_,private_,protected_}instance_methods(false):
       compile-time sym array of own methods, filtered by visibility. CRuby's
       `instance_methods` is public+protected; the prefixed forms narrow to a
       single visibility. Only the own-methods (`false`) form is foldable -- the
       inherited form needs built-in ancestor method sets (left to reject). */
    int im_pub = 0, im_prot = 0, im_priv = 0, im_ok = 1;
    if (sp_streq(name, "instance_methods"))             { im_pub = 1; im_prot = 1; }
    else if (sp_streq(name, "public_instance_methods")) { im_pub = 1; }
    else if (sp_streq(name, "protected_instance_methods")) { im_prot = 1; }
    else if (sp_streq(name, "private_instance_methods")) { im_priv = 1; }
    else im_ok = 0;
    if (im_ok && argc == 1) {
      const char *argt = nt_type(nt, argv[0]);
      int is_false_arg = argt && sp_streq(argt, "FalseNode");
      if (is_false_arg) {
        const char *cn2 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")
                          ? nt_str(nt, recv, "name") : NULL;
        int ci2 = cn2 ? comp_class_index(c, cn2) : -1;
        if (ci2 >= 0) {
          ReflNames r = {0};
          refl_own_instance_methods(c, ci2, im_pub, im_prot, im_priv, 0, &r);
          /* a real sp_PolyArray of boxed symbols, so chained ops like
             `.map(&:to_s).sort` iterate it (a boxed SYM_ARRAY obj is opaque
             to the poly-array path and iterated as empty) */
          buf_puts(b, "sp_box_poly_array(");
          refl_emit_sym_array(c, ci2, &r, b);
          buf_puts(b, ")");
          refl_free(&r);
          return 1;
        }
      }
    }
    /* ClassName.singleton_methods / singleton_methods(false): the class
       methods the program defines, a compile-time list like the one above. */
    if (sp_streq(name, "singleton_methods") && argc <= 1) {
      const char *cn2 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")
                        ? nt_str(nt, recv, "name") : NULL;
      int ci2 = cn2 ? comp_class_index(c, cn2) : -1;
      int all = argc == 0 || nt_kind(nt, argv[0]) == NK_TrueNode;
      if (ci2 >= 0 && (argc == 0 || all || nt_kind(nt, argv[0]) == NK_FalseNode) &&
          an_class_singleton_methods_listable(c, ci2)) {
        ReflNames r = {0};
        refl_class_singleton_methods(c, ci2, all, &r);
        buf_puts(b, "sp_box_poly_array(");
        refl_emit_sym_array(c, -1, &r, b);
        buf_puts(b, ")");
        refl_free(&r);
        return 1;
      }
    }
    /* `equal?` joins them: a class is one object per name, so identity and
       equality are the same question here (#4271). */
    if (is_equality_name(name) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; sp_class_eq(_cl%d, _cl%da); })", _clt, _clt);
        return 1;
      }
    }
    if (sp_streq(name, "!=" ) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; !sp_class_eq(_cl%d, _cl%da); })", _clt, _clt);
        return 1;
      }
    }
    if ((sp_streq(name, "<") || sp_streq(name, "<=") || sp_streq(name, ">") ||
         sp_streq(name, ">=") || sp_streq(name, "<=>")) && argc == 1) {
      TyKind at = comp_ntype(c, argv[0]);
      if (at == TY_CLASS) {
        /* CRuby returns nil (not false) when the classes are unrelated, so the
           tri-state helpers yield a boxed true/false/nil (or -1/0/1/nil for
           <=>). */
        const char *fn = sp_streq(name, "<") ? "sp_class_lt3" :
                         sp_streq(name, "<=") ? "sp_class_le3" :
                         sp_streq(name, ">") ? "sp_class_gt3" :
                         sp_streq(name, ">=") ? "sp_class_ge3" : "sp_class_cmp3";
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_Class _cl%da = ", _clt); emit_expr(c, argv[0], b);
        buf_printf(b, "; %s(_cl%d, _cl%da); })", fn, _clt, _clt);
        return 1;
      }
    }
    /* Class#subclasses: the direct, instantiated subclasses, known from the
       compile-time class graph. Constant class receiver only. (#2656) */
    if (sp_streq(name, "subclasses") && argc == 0) {
      const char *scty = nt_type(nt, recv);
      int scid = (scty && sp_streq(scty, "ConstantReadNode")) ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
      if (scid >= 0) {
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        for (int k = 0; k < c->nclasses; k++) {
          if (c->classes[k].parent != scid) continue;   /* defined subclasses, even if never .new'd */
          if (is_builtin_reopen(c->classes[k].name)) continue;
          /* the class a `def obj.m` synthesizes under the object's class is a
             singleton class, which Class#subclasses leaves out */
          if (c->classes[k].is_singleton_of) continue;
          const char *kn = class_ruby_name(c, k); if (!kn) kn = c->classes[k].name;
          buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_class(((sp_Class){%d, SPL(\"%s\")})));", ta, k, kn);
        }
        buf_printf(b, " _t%d; })", ta);
        return 1;
      }
    }
    /* a named class/module value is never a singleton class (spinel has no
       singleton-class objects), so #singleton_class? is always false. */
    if (sp_streq(name, "singleton_class?") && argc == 0) {
      buf_puts(b, "((void)("); emit_expr(c, recv, b); buf_puts(b, "), 0)");
      return 1;
    }
    /* Module#include?(mod): mod must be a Module (a Class arg is a TypeError);
       true when mod is a proper ancestor of the receiver (#2674). */
    if (sp_streq(name, "include?") && argc == 1 && comp_ntype(c, argv[0]) == TY_CLASS) {
      int m = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      buf_printf(b, "; sp_Class _cl%d = ", m); emit_expr(c, argv[0], b);
      buf_printf(b, "; if (!sp_class_is_module_val(_cl%d)) sp_raise_cls(\"TypeError\", "
                    "\"wrong argument type Class (expected Module)\");", m);
      buf_printf(b, " _cl%d.cls_id != _cl%d.cls_id && sp_class_le(_cl%d, _cl%d); })", _clt, m, _clt, m);
      return 1;
    }
    /* Module#class_variables: the registered cvars, own + ancestors (#2719) */
    if (sp_streq(name, "class_variables") && argc == 0) {
      const char *rcn9 = nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")
                         ? nt_str(nt, recv, "name") : NULL;
      int rci9 = rcn9 ? comp_class_index(c, rcn9) : -1;
      if (rci9 >= 0) {
        int ta = ++g_tmp;
        buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
        for (int k9 = rci9; k9 >= 0; k9 = c->classes[k9].parent)
          for (int q9 = 0; q9 < c->classes[k9].ncvars; q9++) {
            buf_printf(b, " sp_PolyArray_push(_t%d, sp_box_sym(sp_sym_intern(", ta);
            emit_str_literal(b, c->classes[k9].cvars[q9]);
            buf_puts(b, ")));");
          }
        buf_printf(b, " _t%d; })", ta);
        return 1;
      }
    }
    /* class-variable reflection with a literal @@name on a constant class
       receiver: resolve to the cvar's C global (#2694). */
    if ((sp_streq(name, "class_variable_get") || sp_streq(name, "class_variable_set") ||
         sp_streq(name, "class_variable_defined?")) && argc >= 1) {
      const char *aty2 = nt_type(nt, argv[0]);
      const char *cvn = (aty2 && sp_streq(aty2, "SymbolNode")) ? nt_str(nt, argv[0], "value")
                      : (aty2 && sp_streq(aty2, "StringNode")) ? nt_str(nt, argv[0], "content") : NULL;
      const char *rty2 = nt_type(nt, recv);
      int ccid = (rty2 && sp_streq(rty2, "ConstantReadNode")) ? comp_class_index(c, nt_str(nt, recv, "name")) : -1;
      if (cvn && cvn[0] == '@' && cvn[1] == '@' && ccid >= 0) {
        ccid = comp_cvar_owner(c, ccid, cvn);
        int cvi = comp_cvar_index(&c->classes[ccid], cvn);
        char ref[300]; snprintf(ref, sizeof ref, "cvar_%s_%s", c->classes[ccid].name, cvn + 2);
        if (sp_streq(name, "class_variable_defined?")) {
          if (cvi >= 0) buf_printf(b, "(%s__set != 0)", ref);
          else buf_puts(b, "0");
          return 1;
        }
        if (cvi >= 0) {
          TyKind ct = c->classes[ccid].cvar_types[cvi];
          if (sp_streq(name, "class_variable_get")) { emit_boxed_text(c, ct, ref, b); return 1; }
          if (sp_streq(name, "class_variable_set") && argc == 2) {
            buf_printf(b, "(%s = ", ref);
            if (ct == TY_POLY) emit_boxed(c, argv[1], b); else emit_expr(c, argv[1], b);
            buf_puts(b, ", ");
            emit_cvar_set_flag(c, ccid, cvn, 1, b);
            emit_boxed_text(c, ct, ref, b); buf_puts(b, ")");
            return 1;
          }
        }
        else if (sp_streq(name, "class_variable_get")) {
          buf_printf(b, "(sp_raise_cls(\"NameError\", \"uninitialized class variable %s in %s\"), sp_box_nil())",
                     cvn, c->classes[ccid].name);
          return 1;
        }
      }
    }
    /* Klass === obj is obj.is_a?(Klass): does the operand's runtime class have
       the receiver class among its ancestors. Only for a user-class receiver --
       the primitive type names (Integer === 5, Comparable === 5) have their own
       tag-based fold elsewhere, which this must not shadow. */
    if (sp_streq(name, "===") && argc == 1) {
      const char *rvt2 = nt_type(nt, recv);
      const char *rcn2 = (rvt2 && (sp_streq(rvt2, "ConstantReadNode") ||
                                   sp_streq(rvt2, "ConstantPathNode"))) ? nt_str(nt, recv, "name") : NULL;
      if (rcn2 && (comp_class_index(c, rcn2) >= 0 ||
                   sp_streq(rcn2, "TrueClass") || sp_streq(rcn2, "FalseClass") ||
                   /* the roots: every object is one, and `Object === x` used to
                      fall past this arm into the missing-method gate */
                   is_object_root(rcn2) ||
                   /* a class or module value is an instance of these */
                   sp_streq(rcn2, "Class") || sp_streq(rcn2, "Module"))) {
        /* Module#=== is `arg.is_a?(self)`. TrueClass/FalseClass receivers read
           the arg's runtime class, so a non-literal boolean matches (#2966).
           (Only these builtins emit as a usable class value here.) */
        int o = ++g_tmp;
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", o); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_poly_is_a(_t%d, _cl%d); })", o, _clt);
        return 1;
      }
      /* The same question with the class carried as a VALUE -- a parameter,
         a local, an element read: `pattern === x` inside `def grep(pattern)`
         -- has no name to fold on, so it goes to sp_poly_is_a at run time,
         which reads the operand's class off its tag and walks the builtin
         and user chains alike (`Integer === 5`, `Numeric === 2.5`, a user
         class, `Object`). It used to fall through to the missing-method
         gate, which took `arr.grep(Integer)` down with it the moment the
         literal became a parameter. */
      if (!rcn2) {
        int o = ++g_tmp;
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
        buf_printf(b, "; sp_RbVal _t%d = ", o); emit_boxed(c, argv[0], b);
        buf_printf(b, "; sp_poly_is_a(_t%d, _cl%d); })", o, _clt);
        return 1;
      }
    }
    /* klass.is_a?/kind_of?(Module|Class|Object|BasicObject) */
    if (argc == 1 && is_kind_query(name)) {
      int exact = sp_streq(name, "instance_of?");
      const char *cn2 = nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ConstantReadNode")
                        ? nt_str(nt, argv[0], "name") : NULL;
      if (cn2) {
        buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b); buf_puts(b, "; ");
        /* Module: all class/module values are instances of Module */
        if (sp_streq(cn2, "Module")) {
          if (exact) buf_printf(b, "sp_class_is_module_val(_cl%d); })", _clt);
          else buf_printf(b, "1; })");
        }
        /* Class: every class value, builtin or user, but no module */
        else if (sp_streq(cn2, "Class")) {
          buf_printf(b, "!sp_class_is_module_val(_cl%d); })", _clt);
        }
        else if (sp_streq(cn2, "Object") || sp_streq(cn2, "BasicObject")) {
          buf_printf(b, "1; })");
        }
        else {
          /* A module a class extends is among its singleton's ancestors, and
             so are the modules that module includes and the ones the
             superclasses extend: `Klass.is_a?(M)` is true for exactly those
             classes. Any other target answers false. */
          int mcid = comp_class_index(c, cn2);
          int any = 0;
          buf_puts(b, "(");
          if (mcid >= 0 && comp_class_is_module(c, &c->classes[mcid]) && !exact)
            for (int k = 0; k < c->nclasses; k++)
              if (comp_class_singleton_has_module(c, k, mcid)) {
                buf_printf(b, "%s_cl%d.cls_id == %d", any ? " || " : "", _clt, k);
                any = 1;
              }
          if (!any) buf_printf(b, "(void)_cl%d, 0", _clt);
          buf_puts(b, "); })");
        }
        return 1;
      }
    }
    /* a user class method called on a Class-typed value carried in a plain
       variable / parameter (`model.table_name` where model is a Class object):
       switch on the boxed class id and call the matching class's static
       method. A constant / accessor receiver (`Foo.bar`, `Reg.handler.run`)
       keeps its existing direct or Stage-2 dispatch. Only user classes that
       define (or inherit) the name get an arm; the result unifies their return
       types (poly when they disagree). (#2445) */
    {
      int recv_is_var9 = class_recv_is_dynamic(c, recv);
      int ncand9 = 0, defmi9 = -1;
      if (!recv_is_var9) goto skip_cls_cmethod9;
      /* `new` on such a value is a construction, which the `new` dispatches
         below build with an arm per class -- the user `self.new` ones among
         them. Taken here, the classes that construct normally had no arm. */
      if (sp_streq(name, "new") && comp_ntype(c, id) == TY_POLY) goto skip_cls_cmethod9;
      TyKind uret9 = TY_UNKNOWN; int uret_set9 = 0, splat9 = 0;
      for (int a = 0; a < argc; a++)
        if (nt_kind(nt, argv[a]) == NK_SplatNode) splat9++;
      for (int k = 0; k < c->nclasses; k++) {
        if (is_builtin_reopen(c->classes[k].name)) continue;
        int kmi = comp_cmethod_in_chain(c, k, name, NULL);
        if (kmi < 0) continue;
        /* Only the candidates this call could actually reach set the return
           type. One that cannot take this many arguments is not a possible
           receiver here, so its return type is not part of the answer. */
        if (!cls_arm_takes_argc(&c->scopes[kmi], argc, splat9)) continue;
        ncand9++; defmi9 = kmi;
        TyKind kr = (TyKind)c->scopes[kmi].ret;
        if (!uret_set9) { uret9 = kr; uret_set9 = 1; }
        else if (kr != uret9) uret9 = TY_POLY;
      }
      if (ncand9 > 0) {
        /* Every candidate must accept exactly this call's positional args (no
           block/yield). Different candidates may declare different param
           types, so emit_args_filled runs per arm against that method's
           signature -- the args are hoisted into poly temps first so a
           side-effecting argument is evaluated once, not once per arm. */
        int simple9 = (nt_ref(nt, id, "block") < 0);
        /* A candidate taking a block -- a yielding one through its
           proc form -- is an arm of the poly receiver's class-tag dispatch,
           which hands the block over as a proc: box the class and take it. */
        if (comp_ntype(c, id) == TY_POLY && g_n_argov < MAX_ARG_OVERRIDE) {
          int any_pf9 = 0;
          for (int k = 0; k < c->nclasses && !any_pf9; k++) {
            int kmi = comp_cmethod_in_chain(c, k, name, NULL);
            if (kmi < 0) continue;
            Scope *ks9 = &c->scopes[kmi];
            if (scope_proc_form_of(c, kmi) >= 0 ||
                (ks9->blk_param && ks9->blk_param[0] && !ks9->yields)) any_pf9 = 1;
          }
          if (any_pf9) {
            int tsd = ++g_tmp;
            Buf rb9; memset(&rb9, 0, sizeof rb9); emit_boxed(c, recv, &rb9);
            emit_indent(g_pre, g_indent);
            buf_printf(g_pre, "sp_RbVal _t%d = %s; SP_GC_ROOT_RBVAL(_t%d);\n",
                       tsd, rb9.p ? rb9.p : "sp_box_nil()", tsd);
            free(rb9.p);
            view_bind(recv, "_t%d", tsd);
            int vw = view_push(c, recv, TY_POLY);
            int svcv = g_cls_value_recv; g_cls_value_recv = recv;
            int done9 = emit_unresolved_call(c, id, b);
            g_cls_value_recv = svcv;
            view_pop(c, vw);
            view_unbind(g_n_argov - 1);
            if (done9) return 1;
          }
        }
        for (int k = 0; simple9 && k < c->nclasses; k++) {
          if (is_builtin_reopen(c->classes[k].name)) continue;
          int kmi = comp_cmethod_in_chain(c, k, name, NULL);
          if (kmi < 0) continue;
          /* A candidate that yields, takes a block, or has a rest param has no
             arm this emitter can build at all, so it still vetoes the
             dispatch. A candidate with the WRONG ARITY does not: it cannot be
             the receiver of this call, and vetoing on it refused to compile a
             program whose call was never ambiguous (#4129). */
          /* A *rest candidate has an arm when the rest is the boxed array
             every splat parameter starts as: the surplus arguments are
             packed into it (FFI::Struct.layout, a getter and a DSL in one). */
          int rest_ok9 = rest_packable_arm(c, &c->scopes[kmi]);
          if (c->scopes[kmi].yields ||
              (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0]) ||
              (c->scopes[kmi].rest_idx >= 0 && !rest_ok9)) simple9 = 0;
        }
        if (simple9) {
          TyKind slot9 = is_scalar_ret(uret9) && uret9 != TY_VOID && uret9 != TY_UNKNOWN
                         ? uret9 : (ty_is_object(uret9) ? uret9 : TY_POLY);
          int tk9 = ++g_tmp, tr9 = ++g_tmp, argsN9 = nt_ref(nt, id, "arguments");
          buf_puts(b, "({ ");
          /* The receiver, then each argument once, as the boxed class value's
             dispatch takes them (hoist_dispatch_args), here in the call's own
             statement expression: each arm below binds them for its callee
             through the shared binder. The arm bound them by hand, one
             argument to one parameter, out of a table of 16: every argument
             past the 16th was dropped unevaluated, a post after a rest read
             an argument the rest had taken, and a splat or a keyword reached
             a positional slot whole. */
          int *hsv9, *hty9;
          Buf hp9; memset(&hp9, 0, sizeof hp9);
          Buf *sv_pre9 = g_pre; g_pre = &hp9;
          Buf rb9; memset(&rb9, 0, sizeof rb9);
          emit_expr(c, recv, &rb9);
          buf_printf(g_pre, "sp_Class _t%d = %s; ", tk9, rb9.p ? rb9.p : "");
          free(rb9.p);
          int hn9 = hoist_dispatch_args(c, argsN9, &hsv9, &hty9);
          g_pre = sv_pre9;
          if (hp9.p) buf_puts(b, hp9.p);
          free(hp9.p);
          emit_ctype(c, slot9, b);
          buf_printf(b, " _t%d = %s; switch (_t%d.cls_id) {", tr9,
                     slot9 == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, slot9), tk9);
          char rcls9[24], rraise9[256];
          snprintf(rcls9, sizeof rcls9, "_t%d", tk9);
          snprintf(rraise9, sizeof rraise9, "sp_raise_nomethod(sp_sprintf(\"undefined method '%s' for %%s\", sp_class_to_s(_t%d)))", name, tk9);
          int rdef9 = 0;
          for (int k = 0; k < c->nclasses; k++) {
            if (is_builtin_reopen(c->classes[k].name)) continue;
            int defcls9 = -1;
            int kmi = comp_cmethod_in_chain(c, k, name, &defcls9);
            if (kmi < 0) continue;
            int rarm9 = emit_reopen_arm_test(c, kmi, name, rcls9, rraise9, b);
            if (!rarm9) buf_printf(b, " case %d: ", k);
            rdef9 |= rarm9;
            /* A class whose method cannot take this many arguments gets the
               answer CRuby gives, rather than no arm (which would fall to the
               default's NoMethodError, naming the wrong failure). */
            if (!cls_arm_takes_argc(&c->scopes[kmi], argc, splat9)) {
              /* the range the arm judged, which a default widens */
              char kw[256], am[512];
              scope_arity_kw_suffix(c, &c->scopes[kmi], kw, sizeof kw);
              arity_message(am, sizeof am, argc, c->scopes[kmi].nrequired, c->scopes[kmi].nparams, kw);
              buf_printf(b, "sp_raise_cls(\"ArgumentError\", \"%s\"); break;", am);
              continue;
            }
            TyKind kr = (TyKind)c->scopes[kmi].ret;
            Buf cb9; memset(&cb9, 0, sizeof cb9);
            emit_method_cname(c, &c->scopes[kmi], &cb9);
            buf_printf(&cb9, "(");
            /* A class method whose body reads the receiving class (a class
               with descendants, `def self.find; name; end`) is compiled with a
               leading sp_Class token. Pass the class THIS arm's case selected,
               not the defining class: every arm makes the same call otherwise,
               and an inherited body would answer `name` as the wrong class for
               all but one of them (#4217). */
            const char *lead9 = rarm9 ? (buf_puts(&cb9, rcls9), ", ") : emit_cmethod_self_cls_arg(c, kmi, k, &cb9);
            /* Every candidate is a separate C function with its own parameter
               list, so the arm fills THAT list, by the binder a direct call
               to it uses. What the binding puts ahead of the call (a splat's
               gather, a `**` merge, a count check) is this arm's alone: in
               the statement's prelude it ran for a value of every class. */
            Buf apre9; memset(&apre9, 0, sizeof apre9);
            Buf *sv_apre9 = g_pre; g_pre = &apre9;
            emit_args_filled(c, kmi, argsN9, lead9, &cb9);
            g_pre = sv_apre9;
            buf_puts(&cb9, ")");
            if (apre9.p && apre9.p[0]) { buf_puts(b, "{ "); buf_puts(b, apre9.p); }
            buf_printf(b, "_t%d = ", tr9);
            if (slot9 == TY_POLY && kr != TY_POLY) emit_boxed_text(c, kr, cb9.p ? cb9.p : "", b);
            else if (slot9 != TY_POLY && kr == TY_POLY) emit_unbox_text(c, slot9, cb9.p ? cb9.p : "", b);
            else buf_puts(b, cb9.p ? cb9.p : "");
            if (apre9.p && apre9.p[0]) buf_puts(b, "; }");
            free(apre9.p);
            free(cb9.p);
            buf_puts(b, "; break;");
          }
          /* A class the switch has no arm for is a class that does not define
             the method: CRuby raises NoMethodError, and falling out of the
             switch left the result slot at its default -- a junk value the
             caller could not tell from a real answer. */
          if (!rdef9) buf_printf(b, " default: %s; break;", rraise9);
          buf_printf(b, " } _t%d; })", tr9);
          unhoist_dispatch_args(c, hn9, hsv9, hty9);
          (void)defmi9;
          return 1;
        }
      }
      skip_cls_cmethod9:;
    }
  }
  return 0;
}

/* a call that reaches a class method: a bare call inside a class method, a class body or a module_function's includer, obj.class.name / obj.class.cmeth, and a bare name in a class method */
int emit_call_class_method_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv) {
  /* A bare call inside a method: the enclosing class's own chain answers it
     before the top-level table does. A top-level `def` lands on Object, which
     is BELOW the class in the ancestry, so a class method of the same name
     shadows it -- spinel kept top-level defs in a flat table consulted here
     and picked the top-level one, so a `log` helper in a class lost to a `log`
     helper beside it. Fall through to the class-member resolution instead. */
  if (recv < 0 && comp_method_index(c, name) >= 0) {
    Scope *esc = comp_scope_of(c, id);
    int ecls = esc ? esc->class_id : -1;
    int shadowed = ecls >= 0 && ecls < c->nclasses &&
                   (esc->is_cmethod
                      ? comp_cmethod_in_chain(c, ecls, name, NULL) >= 0
                      : (comp_method_in_chain(c, ecls, name, NULL) >= 0 ||
                         comp_is_reader(&c->classes[ecls], name)));
    if (!shadowed) { emit_method_call(c, id, b); return 1; }
  }
  /* bare call to a sibling class method (inside def self.foo, calling bar()) */
  if (recv < 0) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0) {
      /* bare `new` inside a class method -> construct the *emitting* class.
         For an inherited cls method specialized into a subclass, the emitting
         class is that subclass, so `new` resolves to the subclass constructor. */
      int new_cls = (g_emitting_class_id >= 0) ? g_emitting_class_id : encl->class_id;
      /* ...unless the class defines its own `self.new`: bare `new` calls that
         method, whose value the analysis types (#5405). The sibling class
         method call below reaches it. */
      int user_new = sp_streq(name, "new") ? comp_cmethod_in_chain(c, new_cls, "new", NULL) : -1;
      if (user_new >= 0 && (&c->scopes[user_new] == encl ||
                            !scope_has_callable_symbol(c, user_new))) user_new = -1;
      if (sp_streq(name, "allocate")) {
        int oc = allocate_on_own_class(c, id);
        if (oc >= 0 && !class_is_exc_subclass(c, oc)) { emit_own_class_alloc(c, id, oc, b); return 1; }
      }
      if (sp_streq(name, "new") && user_new < 0) {
        ClassInfo *ncls = &c->classes[new_cls];
        /* a native class's bare `new` uses the declared constructor, exactly
           as the receiver `Klass.new(...)` path does */
        if (ncls->is_native_class) {
          int nargc; const int *nargv = call_args(nt, id, &nargc);
          if (emit_native_ctor(c, id, new_cls, nargc, nargv, b)) return 1;
        }
        int initm = comp_method_in_chain(c, new_cls, "initialize", NULL);
        /* A Data/Struct class has no user `initialize`; its generated constructor
           takes one arg per member. Fill member-wise -- positionally, or by
           keyword when a trailing kwarg hash names each member -- exactly as the
           receiver `Klass.new(...)` path does. Without this, a bare `new(...)`
           inside a `def self.default`/`self.initial` factory emitted an empty
           `sp_Klass_new()`, dropping every argument. */
        if (ncls->is_struct && initm < 0) {
          int sargc; const int *sargv = call_args(nt, id, &sargc);
          int kwh = (sargc == 1 && nt_type(nt, sargv[0]) &&
                     sp_streq(nt_type(nt, sargv[0]), "KeywordHashNode")) ? sargv[0] : -1;
          /* a call CRuby refuses raises its ArgumentError, and a positional
             `*` spreads across the members, as in the receiver path: these
             took the arguments as they stood, dropping a key that names no
             member and an argument past the last, and leaving nil a Data
             member none names */
          int nunk, late;
          if (emit_struct_new_early(c, new_cls, sargc, sargv,
                                    ncls->kw_init == -1 && !kwh_has_splat(nt, kwh) ? -1 : kwh,
                                    &nunk, &late, b)) return 1;
          /* a keyword_init: false Struct takes the keywords as one positional
             Hash, its first member: with a `**` they merge into it, nil when
             none came, as in the receiver path */
          int kwf_mh = -1;
          if (kwh >= 0 && ncls->kw_init == -1) {
            TyKind mty;
            if (kwh_has_splat(nt, kwh)) kwf_mh = emit_ds_hash_merge(c, kwh, 1, &mty);
            kwh = -1;
          }
          /* keywords beside a `**`, or binding late, merge into one hash the
             members read and the run-time check judges, as in the receiver
             path */
          int kw_ht = -1;
          if (kwh >= 0 && (kwh_has_splat(nt, kwh) || late)) {
            kw_ht = emit_struct_kw_hash(c, kwh);
            emit_struct_kw_check(c, ncls, kw_ht, -1);
          }
          buf_printf(b, "sp_%s_new(", ncls->c_name);
          for (int a = 0; a < ncls->nmembers; a++) {
            if (a) buf_puts(b, ", ");
            int vnode = -1;
            if (kwh >= 0) vnode = struct_kwarg_value(c, kwh, ncls->ivars[a] + 1);
            else if (a < sargc) vnode = sargv[a];
            if (kwf_mh >= 0) {
              char hv[160];
              snprintf(hv, sizeof hv, "(sp_PolyPolyHash_length(_t%d) ? sp_box_obj(_t%d, SP_BUILTIN_POLY_POLY_HASH) : sp_box_nil())",
                       kwf_mh, kwf_mh);
              if (a == 0) emit_unbox_text(c, ncls->ivar_types[a], hv, b);
              else buf_puts(b, default_value_from_compiler(c, ncls->ivar_types[a]));
            }
            else if (kw_ht >= 0) emit_struct_kw_member(c, ncls, a, kw_ht, b);
            else if (vnode >= 0) {
              if (ncls->ivar_types[a] == TY_POLY && comp_ntype(c, vnode) != TY_POLY) emit_boxed(c, vnode, b);
              /* and the reverse: a poly value into a concrete member slot
                 (#4348), the same coercion the receiver path does */
              else if (ncls->ivar_types[a] != TY_POLY && ncls->ivar_types[a] != TY_UNKNOWN &&
                       comp_ntype(c, vnode) == TY_POLY) {
                Buf pv2; memset(&pv2, 0, sizeof pv2);
                emit_expr(c, vnode, &pv2);
                emit_unbox_text(c, ncls->ivar_types[a], pv2.p ? pv2.p : "sp_box_nil()", b);
                free(pv2.p);
              }
              else emit_expr(c, vnode, b);
            }
            else buf_puts(b, default_value_from_compiler(c, ncls->ivar_types[a]));
          }
          buf_puts(b, ")");
          return 1;
        }
        /* yielding initialize: inline its body at the call site exactly as
           the Klass.new receiver path does -- the emitted constructor only
           allocates, so without the inline the body's @ivar writes vanish
           (with or without a block at this site) */
        if (initm >= 0 && c->scopes[initm].yields &&
            (emit_ctor_yield_inline(c, id, new_cls, b) || emit_ctor_new_with_proc(c, id, new_cls, b))) return 1;
        /* A class method a subclass inherits runs with self the subclass, so
           its bare `new` builds that subclass (#5995): a body that takes the
           receiving class picks the constructor by it. Each arm spells the
           arguments, so only arguments with no effect are spelled more than
           once; a subclass the plain constructor cannot build keeps the
           static one. */
        if (cmethod_takes_self_cls(c, (int)(encl - c->scopes)) && !(encl->yields && g_self) &&
            !ncls->is_value_type) {
          int nargs0 = 0; const int *av0 = call_args(nt, id, &nargs0);
          int plain = nt_ref(nt, id, "block") < 0;
          for (int a = 0; plain && a < nargs0; a++)
            if (subtree_has_side_effect(c, av0[a])) plain = 0;
          int nsub = 0;
          for (int k = 0; plain && k < c->nclasses; k++) {
            if (k == new_cls || !is_descendant(c, k, new_cls)) continue;
            ClassInfo *kc = &c->classes[k];
            int ik = comp_method_in_chain(c, k, "initialize", NULL);
            if (kc->is_value_type || kc->is_struct || kc->is_native_class ||
                (ik >= 0 && c->scopes[ik].yields) ||
                comp_cmethod_in_chain(c, k, "new", NULL) >= 0) { plain = 0; break; }
            nsub++;
          }
          if (plain && nsub > 0) {
            buf_puts(b, "(");
            for (int k = 0; k < c->nclasses; k++) {
              if (k == new_cls || !is_descendant(c, k, new_cls)) continue;
              int ik = comp_method_in_chain(c, k, "initialize", NULL);
              buf_printf(b, "_sp_cls.cls_id == %d ? (sp_%s *)sp_%s_new(", k, ncls->c_name, c->classes[k].c_name);
              if (ik >= 0) emit_args_filled(c, ik, nt_ref(nt, id, "arguments"), "", b);
              buf_puts(b, ") : ");
            }
            buf_printf(b, "sp_%s_new(", ncls->c_name);
            if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
            buf_puts(b, "))");
            return 1;
          }
        }
        buf_printf(b, "sp_%s_new(", ncls->c_name);
        if (initm >= 0) emit_args_filled(c, initm, nt_ref(nt, id, "arguments"), "", b);
        if (initm >= 0) emit_ctor_block_slot(c, id, initm, c->scopes[initm].nparams > 0 ? ", " : "", b);
        buf_puts(b, ")");
        return 1;
      }
      int smi = comp_cmethod_in_chain(c, encl->class_id, name, NULL);
      if (smi >= 0) {
        Scope *ms = &c->scopes[smi];
        if (g_plan_check) ucall_observe(c, id, smi, encl->class_id, 0);
        Buf cb; memset(&cb, 0, sizeof cb);
        emit_method_cname(c, ms, &cb);
        buf_puts(&cb, "(");
        /* a sibling call keeps the receiving class: forward ours when this
           body has one, else the class it is emitted for. A yielding body
           has no function of its own and is inlined, where no `_sp_cls` is
           declared: its receiving class is the self the inliner bound
           (#5092). */
        const char *lead2 = "";
        if (cmethod_takes_self_cls(c, smi)) {
          if (cmethod_takes_self_cls(c, (int)(encl - c->scopes))) {
            buf_puts(&cb, encl->yields && g_self ? g_self : "_sp_cls");
            lead2 = ", ";
          }
          else lead2 = emit_cmethod_self_cls_arg(c, smi, new_cls, &cb);
        }
        emit_args_filled(c, smi, nt_ref(nt, id, "arguments"), lead2, &cb);
        emit_cmethod_block_arg(c, id, ms, -1, &cb);
        buf_puts(&cb, ")");
        /* The site's slot and the callee's return can disagree: this call is
           devirtualized to the sibling's own C function, whose return is
           concrete, while the inference typed the SITE poly -- a `create!`
           inherited by several STI subclasses unifies to poly across them, and
           `create!(attrs).tap { }` inside an inlined `transaction { }` then put
           an sp_Room * straight into an sp_RbVal temp (#4428). The class-method
           dispatch a few thousand lines up has always boxed this seam; the bare
           sibling call is the same seam without the switch around it. */
        TyKind slot_t = comp_ntype(c, id);
        TyKind kr = (TyKind)ms->ret;
        if (slot_t == TY_POLY && kr != TY_POLY && kr != TY_UNKNOWN && kr != TY_VOID &&
            !method_is_void(ms))
          emit_boxed_text(c, kr, cb.p ? cb.p : "", b);
        /* ...and the other way: a user `self.new` answers a boxed object
           where the site is typed as the class it builds (#5409) */
        else if (kr == TY_POLY && slot_t != TY_POLY && slot_t != TY_UNKNOWN &&
                 slot_t != TY_VOID && is_scalar_ret(slot_t))
          emit_unbox_text(c, slot_t, cb.p ? cb.p : "sp_box_nil()", b);
        /* a sibling that only raises (no value) where the site wants one */
        else if (slot_t == TY_POLY && method_is_void(ms))
          buf_printf(b, "(%s, sp_box_nil())", cb.p ? cb.p : "0");
        else
          buf_puts(b, cb.p ? cb.p : "");
        free(cb.p);
        return 1;
      }
    }
  }
  /* bare call to a class method of the enclosing module/class body */
  if (recv < 0 && g_class_body_id >= 0) {
    int smi = comp_cmethod_in_chain(c, g_class_body_id, name, NULL);
    if (smi >= 0) {
      Scope *ms = &c->scopes[smi];
      if (g_plan_check) ucall_observe(c, id, smi, g_class_body_id, 0);
      emit_method_cname(c, ms, b);
      buf_puts(b, "(");
      const char *lead3 = emit_cmethod_self_cls_arg(c, smi, g_class_body_id, b);
      emit_args_filled(c, smi, nt_ref(nt, id, "arguments"), lead3, b);
      emit_cmethod_block_arg(c, id, ms, -1, b);
      buf_puts(b, ")");
      return 1;
    }
  }
  /* bare call to a module_function method made available via top-level include */
  if (recv < 0) {
    /* Inside a class that includes the same module, the receiverless call is
       that class's own transplanted copy -- with its own self and its own
       inferred types. Taking the top-level copy here compiled `fill("=")` in
       an instance method into the module's null-receiver function, whose
       return type the class copy does not share (#3795). */
    { Scope *iencl = comp_scope_of(c, id);
      if (iencl && iencl->class_id >= 0 && !iencl->is_cmethod &&
          comp_method_in_chain(c, iencl->class_id, name, NULL) >= 0)
        goto skip_toplevel_include;
    }
    int imi = comp_included_method_index(c, name, id);
    if (imi >= 0) {
      Scope *ms = &c->scopes[imi];
      if (g_plan_check) ucall_observe(c, id, imi, ms->class_id, 0);
      /* An INSTANCE method reached this way runs with self bound to main, which
         carries none of the module's state, so it takes a null receiver -- the
         emitted function still declares one (#3775). A body that reads an ivar
         would want main's slots, which no module instance owns: refuse that
         with a spinel diagnostic rather than emitting a wrong receiver. */
      const char *lead = "";
      if (!ms->is_cmethod) {
        if (scope_uses_ivars(c, imi)) {
          unsupported(c, id, "top-level include of a module method that uses instance variables");
          return 1;
        }
        lead = ms->nparams > 0 ? ", " : "";
        emit_method_cname(c, ms, b);
        buf_puts(b, "(NULL");
        emit_args_filled(c, imi, nt_ref(nt, id, "arguments"), lead, b);
        /* The declared &block is a parameter like any other: without this the
           call passed one argument to a two-parameter function and the C did
           not compile, whether or not a block was written at the call (#3803).
           The receiver is already in the list, so the separator is needed even
           when the helper decides it is not. */
        { Buf bb; memset(&bb, 0, sizeof bb);
          emit_cmethod_block_arg(c, id, ms, -1, &bb);
          if (bb.p && bb.p[0]) {
            if (bb.p[0] != ',') buf_puts(b, ", ");
            buf_puts(b, bb.p);
          }
          free(bb.p); }
        buf_puts(b, ")");
        return 1;
      }
      emit_method_cname(c, ms, b);
      buf_puts(b, "(");
      emit_args_filled(c, imi, nt_ref(nt, id, "arguments"), lead, b);
      buf_puts(b, ")");
      return 1;
    }
  }  skip_toplevel_include: ;


  /* X.class.name / .to_s -> identity when .class yields a string;
     for user-object receivers .class now yields TY_CLASS, so wrap with sp_class_to_s. */
  if (recv >= 0 && argc == 0 && is_name_reader(name) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
    if (comp_ntype(c, recv) == TY_CLASS) {
      /* every .class now yields an sp_Class (the poly path included, via
         sp_poly_class_val): stringify uniformly. */
      int _clt = ++g_tmp;
      buf_printf(b, "({ sp_Class _cl%d = ", _clt); emit_expr(c, recv, b);
      /* inspect carries a keyword-init Struct class's suffix; to_s stays the
         bare name (#3947), and name is nil for an anonymous class (#4031) */
      buf_printf(b, "; sp_class_%s(_cl%d); })",
                 sp_streq(name, "inspect") ? "inspect_name"
                 : sp_streq(name, "name")  ? "name_or_nil" : "to_s", _clt);
    }
    else emit_expr(c, recv, b);
    return 1;
  }
  /* obj.class.cmeth(...) -> dispatch class method on obj's runtime class
     Emits a cls_id switch: each case calls the right class method. */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
    int robj = nt_ref(nt, recv, "receiver");
    TyKind rrt = robj >= 0 ? comp_ntype(c, robj) : TY_UNKNOWN;
    if (ty_is_object(rrt)) {
      int cid = ty_object_class(rrt);
      int defmi = comp_cmethod_in_chain(c, cid, name, NULL), dyield = 0;
      for (int k = 0; defmi >= 0 && k < c->nclasses; k++) {
        int km = is_descendant(c, k, cid) ? comp_cmethod_in_chain(c, k, name, NULL) : -1;
        if (km >= 0 && c->scopes[km].yields) dyield = 1;
      }
      if (defmi >= 0 && !dyield) {
        /* Count distinct class method impls across the hierarchy */
        int nimpl = 0;
        for (int k = 0; k < c->nclasses; k++) {
          if (!is_descendant(c, k, cid)) continue;
          if (comp_cmethod_in_class(c, k, name) >= 0) nimpl++;
        }
        TyKind cret = (TyKind)c->scopes[defmi].ret;
        /* Stash the receiver object in a temp (referenced in every switch case) */
        Buf objptr; memset(&objptr, 0, sizeof objptr);   /* heap text: a 64-byte buffer cut long local names silently */
        const char *rty = nt_type(nt, robj);
        if (rty && (sp_streq(rty, "LocalVariableReadNode") || sp_streq(rty, "InstanceVariableReadNode") || sp_streq(rty, "SelfNode")))
          objptr = expr_buf(c, robj);
        else {
          int ot = ++g_tmp;
          Buf rb = expr_buf(c, robj);
          emit_indent(g_pre, g_indent);
          emit_ctype(c, rrt, g_pre);
          buf_printf(g_pre, " _t%d = %s;\n", ot, rb.p ? rb.p : ""); free(rb.p);
          buf_printf(&objptr, "_t%d", ot);
        }
        if (nimpl <= 1) {
          /* single implementation: call directly. The value still has to fit
             the slot the INFERENCE gave this call: the many-implementation
             branch below unifies the returns and boxes each arm, and this one
             handed back whatever the one implementation returns. An interface
             receiver whose call the inference typed poly, resolving to one
             implementation that answers Integer, assigned a raw sp_int into an
             sp_RbVal local (#4182). */
          Buf dcb; memset(&dcb, 0, sizeof dcb);
          if (g_plan_check) ucall_observe(c, id, defmi, cid, 0);
          emit_method_cname(c, &c->scopes[defmi], &dcb);
          buf_puts(&dcb, "(");
          /* The one implementation still runs with self = the receiver's
             runtime class: `self.class.label` from a Base method on a Child
             names Child in the body. Naming the static class (cid) answered
             Base for every subclass instance. */
          int rt_cls = cmethod_takes_self_cls(c, defmi) && !comp_ty_value_obj(c, rrt) &&
                       class_has_subclass(c, cid);
          if (rt_cls) {
            int self_recv = sp_streq(rty ? rty : "", "SelfNode");
            const char *op = objptr.p ? objptr.p : "";
            buf_puts(&dcb, "((sp_Class){");
            if (!self_recv) buf_printf(&dcb, "(%s) ? ", op);
            emit_obj_dispatch_key(c, cid, op, &dcb);
            if (!self_recv) buf_printf(&dcb, " : %d", cid);
            buf_puts(&dcb, ", NULL})");
          }
          { const char *ld = rt_cls ? ", " : emit_cmethod_self_cls_arg(c, defmi, cid, &dcb);
            emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), ld, &dcb); }
          buf_puts(&dcb, ")");
          TyKind want = comp_ntype(c, id);
          const char *dc = dcb.p ? dcb.p : "";
          if (want == TY_POLY && cret != TY_POLY && cret != TY_UNKNOWN &&
              cret != TY_VOID && cret != TY_NIL)
            emit_boxed_text(c, cret, dc, b);
          else if (cret == TY_POLY && want != TY_POLY && want != TY_UNKNOWN &&
                   is_scalar_ret(want) && want != TY_VOID && want != TY_NIL)
            emit_unbox_text(c, want, dc, b);
          else buf_puts(b, dc);
          free(dcb.p);
        }
        else {
          /* Check if all descendants agree on return type */
          TyKind unified = cret;
          for (int k2 = 0; k2 < c->nclasses; k2++) {
            if (!is_descendant(c, k2, cid)) continue;
            int kmi2 = comp_cmethod_in_chain(c, k2, name, NULL);
            if (kmi2 < 0) continue;
            TyKind kr = (TyKind)c->scopes[kmi2].ret;
            if (kr != unified) { unified = TY_POLY; break; }
          }
          int rtmp = ++g_tmp;
          buf_puts(b, "({ ");
          if (unified == TY_POLY) buf_puts(b, "sp_RbVal");
          else emit_ctype(c, unified, b);
          buf_printf(b, " _t%d; switch (", rtmp);
          emit_obj_dispatch_key(c, cid, objptr.p ? objptr.p : "", b);
          buf_puts(b, ") {");
          for (int k = 0; k < c->nclasses; k++) {
            if (!is_descendant(c, k, cid)) continue;
            int kmi = comp_cmethod_in_chain(c, k, name, NULL);
            if (kmi < 0) continue;
            TyKind kr = (TyKind)c->scopes[kmi].ret;
            buf_printf(b, " case %d: ", k);
            if (unified == TY_POLY && method_is_void(&c->scopes[kmi])) {
              /* void-return (raises): call then fall through with nil */
              emit_method_cname(c, &c->scopes[kmi], b);
              buf_puts(b, "(");
              { const char *ld = emit_cmethod_self_cls_arg(c, kmi, k, b);
                emit_args_filled(c, kmi, nt_ref(nt, id, "arguments"), ld, b); }
              buf_printf(b, "); _t%d = sp_box_nil(); break;", rtmp);
            }
            else {
              /* emit_boxed_text, not a hand-listed set of box functions: an arm
                 whose return type was not in that list (a Symbol, a range, an
                 object) assigned its raw value to the sp_RbVal result, which
                 does not compile (#4053). */
              Buf ab; memset(&ab, 0, sizeof ab);
              emit_method_cname(c, &c->scopes[kmi], &ab);
              buf_puts(&ab, "(");
              { const char *ld = emit_cmethod_self_cls_arg(c, kmi, k, &ab);
                emit_args_filled(c, kmi, nt_ref(nt, id, "arguments"), ld, &ab); }
              buf_puts(&ab, ")");
              buf_printf(b, "_t%d = ", rtmp);
              if (unified == TY_POLY && kr != TY_POLY)
                emit_boxed_text(c, kr, ab.p ? ab.p : "0", b);
              else buf_puts(b, ab.p ? ab.p : "0");
              free(ab.p);
              buf_puts(b, "; break;");
            }
          }
          buf_printf(b, " default: ");
          {
            TyKind dr = (TyKind)c->scopes[defmi].ret;
            if (unified == TY_POLY && method_is_void(&c->scopes[defmi])) {
              emit_method_cname(c, &c->scopes[defmi], b);
              buf_puts(b, "(");
              { const char *ld = emit_cmethod_self_cls_arg(c, defmi, cid, b);
                emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), ld, b); }
              buf_printf(b, "); _t%d = sp_box_nil(); break;", rtmp);
            }
            else {
              Buf db; memset(&db, 0, sizeof db);
              emit_method_cname(c, &c->scopes[defmi], &db);
              buf_puts(&db, "(");
              { const char *ld = emit_cmethod_self_cls_arg(c, defmi, cid, &db);
                emit_args_filled(c, defmi, nt_ref(nt, id, "arguments"), ld, &db); }
              buf_puts(&db, ")");
              buf_printf(b, "_t%d = ", rtmp);
              if (unified == TY_POLY && dr != TY_POLY)
                emit_boxed_text(c, dr, db.p ? db.p : "0", b);
              else buf_puts(b, db.p ? db.p : "0");
              free(db.p);
              buf_printf(b, "; break;");
            }
          }
          buf_printf(b, " } _t%d; })", rtmp);
        }
        free(objptr.p);
        return 1;
      }
      Buf sgb; memset(&sgb, 0, sizeof sgb);
      if (!class_has_subclass(c, cid) &&
          emit_sg_accessor(c, cid, c->classes[cid].name, name, argc, argv, &sgb)) {
        buf_puts(b, "((void)("); emit_expr(c, robj, b); buf_printf(b, "), %s)", sgb.p);
        free(sgb.p);
        return 1;
      }
      free(sgb.p);
    }
  }
  /* SomeClass.name / .to_s / .inspect -> the class-name string. A
     user-defined singleton (def self.name) shadows the builtin: skip the
     fold and let the normal class-method dispatch emit the call. */
  if (recv >= 0 && argc == 0 &&
      is_name_reader(name) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0 &&
      comp_cmethod_in_chain(c, comp_class_index(c, nt_str(nt, recv, "name")), name, NULL) < 0) {
    { int qci = comp_class_index(c, nt_str(nt, recv, "name"));
      const char *qn8 = qci >= 0 ? class_ruby_name(c, qci) : NULL;
      /* A keyword-init Struct class INSPECTS as `K(keyword_init: true)`; its
         name and to_s stay the bare name, and a positional Struct or a Data
         class has no suffix at all (#3947). */
      int kwq = (qci >= 0 && c->classes[qci].is_struct && c->classes[qci].kw_init == 1 &&
                 sp_streq(name, "inspect"));
      buf_printf(b, "SPL(\"%s%s\")", qn8 ? qn8 : nt_str(nt, recv, "name"),
                 kwq ? "(keyword_init: true)" : ""); }
    return 1;
  }
  /* self.name / self.to_s / self.inspect inside a class method -> class name.
     A method a subclass inherits runs with self = that subclass, so when the
     receiving class rides in (cmethod_takes_self_cls) resolve the name at run
     time rather than folding the defining class's. */
  /* bare `name` inside a class method body -> the class name */
  if (argc == 0 && (recv < 0 ? sp_streq(name, "name") :
      (is_name_reader(name) &&
       nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")))) {
    Scope *encl = comp_scope_of(c, id);
    if (encl && encl->is_cmethod && encl->class_id >= 0 &&
        comp_cmethod_in_chain(c, encl->class_id, name, NULL) < 0) {
      if (cmethod_takes_self_cls(c, (int)(encl - c->scopes)))
        buf_printf(b, "sp_class_to_s(%s)", encl->yields && g_self ? g_self : "_sp_cls");
      else {
        /* the Ruby-visible name: the enclosing path, with any collision
           qualification undone (`Brainfuck__Array` is `Brainfuck::Array`) */
        const char *qn9 = class_ruby_name(c, encl->class_id);
        buf_printf(b, "SPL(\"%s\")", qn9 ? qn9 : c->classes[encl->class_id].name);
      }
      return 1;
    }
  }
  return 0;
}

/* a method a program's reopen of a builtin type defines, on a receiver of that type (not for an alias that captured the builtin) */
int emit_call_reopen_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, TyKind rt) {
  /* dispatch user-defined methods on reopened built-in types -- not for an
     alias that captured the builtin (builtin_only) */
  if (recv >= 0 && !nt_int(nt, id, "builtin_only", 0)) {
    const char *oc_cn = NULL;
    switch (rt) {
    case TY_STRING: oc_cn = "String"; break;
    case TY_INT:    oc_cn = "Integer"; break;
    case TY_FLOAT:  oc_cn = "Float"; break;
    case TY_SYMBOL: oc_cn = "Symbol"; break;
    case TY_RANGE:  oc_cn = "Range"; break;
    case TY_TIME:   oc_cn = "Time"; break;
    case TY_THREAD: oc_cn = "Thread"; break;
    case TY_FIBER:  oc_cn = "Fiber"; break;
    case TY_IO:     oc_cn = "File"; break;
    case TY_CLASS:  oc_cn = "Class"; break;
    default: break;
    }
    if (oc_cn) {
      int oc_ci = rt == TY_IO ? io_reopen_class(c, name) : comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0 && rt == TY_IO) { emit_io_reopen_call(c, id, recv, name, b); return 1; }
        if (oc_mi >= 0) {
          if (g_plan_check) ucall_observe(c, id, oc_mi, oc_ci, 0);
          buf_printf(b, "sp_%s_%s(", mc_reopen_cls(c, oc_ci, name), mc(name));
          emit_expr(c, recv, b);
          emit_args_filled(c, oc_mi, nt_ref(nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          return 1;
        }
      }
    }
    /* bool: dispatch based on value to correct TrueClass/FalseClass impl */
    if (rt == TY_BOOL) {
      int tc_ci = comp_class_index(c, "TrueClass");
      int fc_ci = comp_class_index(c, "FalseClass");
      int tc_mi = tc_ci >= 0 ? comp_method_in_chain(c, tc_ci, name, NULL) : -1;
      int fc_mi = fc_ci >= 0 ? comp_method_in_chain(c, fc_ci, name, NULL) : -1;
      if (tc_mi >= 0 && fc_mi >= 0) {
        /* both defined: ternary dispatch */
        if (g_plan_check) { ucall_observe(c, id, tc_mi, tc_ci, 1); ucall_observe(c, id, fc_mi, fc_ci, 1); }
        int bt = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = ", bt); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(_t%d ? sp_TrueClass_%s(_t%d", bt, mc(name), bt);
        emit_args_filled(c, tc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_printf(b, ") : sp_FalseClass_%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "))");
        return 1;
      }
      if (tc_mi >= 0) {
        /* only TrueClass defined */
        if (g_plan_check) ucall_observe(c, id, tc_mi, tc_ci, 0);
        buf_printf(b, "sp_TrueClass_%s(", mc(name));
        emit_expr(c, recv, b);
        emit_args_filled(c, tc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, ")");
        return 1;
      }
      if (fc_mi >= 0) {
        /* only FalseClass defined: ternary still needed */
        if (g_plan_check) ucall_observe(c, id, fc_mi, fc_ci, 0);
        int bt = ++g_tmp;
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "int _t%d = ", bt); emit_expr(c, recv, g_pre); buf_puts(g_pre, ";\n");
        buf_printf(b, "(_t%d ? (", bt);
        buf_printf(b, "sp_FalseClass_%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "), 0) : sp_FalseClass_");
        buf_printf(b, "%s(_t%d", mc(name), bt);
        emit_args_filled(c, fc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "))");
        return 1;
      }
    }
    /* Hash reopening: any hash-typed receiver -> box to sp_RbVal */
    if (ty_is_hash(rt)) {
      int hc_ci = comp_class_index(c, "Hash");
      int hc_mi = hc_ci >= 0 ? comp_method_in_chain(c, hc_ci, name, NULL) : -1;
      if (hc_mi >= 0 && emit_reopen_block_call(c, id, recv, hc_mi, NULL, b)) return 1;
      if (hc_mi >= 0) {
        if (g_plan_check) ucall_observe(c, id, hc_mi, hc_ci, 0);
        buf_printf(b, "sp_Hash_%s(", mc(c->scopes[hc_mi].name));
        emit_boxed(c, recv, b);
        emit_args_filled(c, hc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* NilClass methods on a receiver known to be nil (self is the `int self`
       the bool reopens take; nil carries nothing) */
    if (rt == TY_NIL) {
      int nc_ci = comp_class_index(c, "NilClass");
      int nc_mi = nc_ci >= 0 ? comp_method_in_chain(c, nc_ci, name, NULL) : -1;
      if (nc_mi >= 0) {
        if (g_plan_check) ucall_observe(c, id, nc_mi, nc_ci, 0);
        buf_printf(b, "((void)("); emit_expr(c, recv, b);
        buf_printf(b, "), sp_NilClass_%s(0", mc(name));
        emit_args_filled(c, nc_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, "))");
        return 1;
      }
    }
    /* Numeric reopening: an Integer / Float receiver whose own reopen (above)
       did not answer; the inference twin has typed it. Self is boxed. */
    if (rt == TY_INT || rt == TY_FLOAT || rt == TY_BIGINT) {
      int nm_ci = comp_class_index(c, "Numeric");
      int nm_mi = nm_ci >= 0 ? comp_method_in_chain(c, nm_ci, name, NULL) : -1;
      if (nm_mi >= 0 && emit_reopen_block_call(c, id, recv, nm_mi, NULL, b)) return 1;
      if (nm_mi >= 0) {
        if (g_plan_check) ucall_observe(c, id, nm_mi, nm_ci, 0);
        buf_printf(b, "sp_Numeric_%s(", mc(c->scopes[nm_mi].name));
        emit_boxed(c, recv, b);
        emit_args_filled(c, nm_mi, nt_ref(nt, id, "arguments"), ", ", b);
        buf_puts(b, ")");
        return 1;
      }
    }
    /* Array reopening: any array-typed receiver -> box to sp_RbVal */
    if (ty_is_array(rt)) {
      int oc_ci2 = comp_class_index(c, "Array");
      if (oc_ci2 >= 0) {
        int oc_mi2 = comp_method_in_chain(c, oc_ci2, name, NULL);
        if (oc_mi2 >= 0) {
          const char *box_fn = (rt == TY_INT_ARRAY) ? "sp_box_int_array" :
                               (rt == TY_STR_ARRAY) ? "sp_box_str_array" :
                               (rt == TY_FLOAT_ARRAY) ? "sp_box_float_array" : "sp_box_poly_array";
          if (emit_reopen_block_call(c, id, recv, oc_mi2, box_fn, b)) return 1;
          buf_printf(b, "sp_Array_%s(", mc(c->scopes[oc_mi2].name));
          buf_printf(b, "%s(", box_fn); emit_expr(c, recv, b); buf_puts(b, ")");
          emit_args_filled(c, oc_mi2, nt_ref(nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          return 1;
        }
      }
    }
    /* Object reopening: universal fallback -> box receiver to sp_RbVal */
    {
      int oc_ci3 = comp_class_index(c, "Object");
      if (oc_ci3 >= 0) {
        int oc_mi3 = comp_method_in_chain(c, oc_ci3, name, NULL);
        /* a yielding one takes the call's block through its proc form
           (#5779): it has no symbol of its own, being spliced where it can */
        int pf3 = oc_mi3 >= 0 && c->scopes[oc_mi3].yields ? scope_proc_form_of(c, oc_mi3) : -1;
        int cblk3 = pf3 >= 0 && nt_ref(nt, id, "block") >= 0
                    ? resolve_forwarded_block(c, nt_ref(nt, id, "block")) : -1;
        if (cblk3 >= 0) {
          Buf rb3; memset(&rb3, 0, sizeof rb3); emit_boxed(c, recv, &rb3);
          emit_reopen_pf_call(c, id, pf3, cblk3, rb3.p ? rb3.p : "sp_box_nil()", b);
          free(rb3.p);
          return 1;
        }
        if (oc_mi3 >= 0) {
          /* a method with no value (it raises, or ends in a void call) where
             the site's slot wants one: nil in that slot's type */
          TyKind want3 = comp_ntype(c, id);
          int void3 = method_is_void(&c->scopes[oc_mi3]) && want3 != TY_VOID &&
                      want3 != TY_UNKNOWN && want3 != TY_NIL;
          if (g_plan_check) ucall_observe(c, id, oc_mi3, oc_ci3, 0);
          if (void3) buf_puts(b, "(");
          buf_printf(b, "sp_Object_%s(", mc(c->scopes[oc_mi3].name));
          emit_boxed(c, recv, b);
          emit_args_filled(c, oc_mi3, nt_ref(nt, id, "arguments"), ", ", b);
          emit_trailing_blk_arg(c, &c->scopes[oc_mi3], id, -1, b);
          buf_puts(b, ")");
          if (void3) buf_printf(b, ", %s)", want3 == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, want3));
          return 1;
        }
      }
    }
  }
  return 0;
}

/* an implicit-self call inside an instance method (or an instance_eval block): a member read,
   a dispatch on self's class (a reopen's own method, a descendant's override), or an Object
   reopening's method */
int emit_call_implicit_self_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv) {
  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    /* Inside an instance_eval/exec block, g_ie_class_id is the rebound
       receiver class and takes priority -- the splice may sit inside a class
       method whose own class (g_emitting_class_id) is unrelated to the block's
       self. Otherwise, when emitting a scope transplanted by include
       (g_emitting_class_id is set), dispatch through the emitting class so
       overrides are found correctly. */
    int dispatch_cid = (g_ie_class_id >= 0) ? g_ie_class_id
                     : (g_emitting_class_id >= 0) ? g_emitting_class_id : self->class_id;
    if (dispatch_cid >= 0) {
      if (emit_or_take_back(c, id, b, emit_implicit_self_member)) return 1;
      /* the method is the call's plan for self of dispatch_cid
         (implicit_self_plan_mi); --plan-check holds it against the lookup */
      int mi = implicit_self_plan_mi(c, id, dispatch_cid);
      if (g_plan_check) {
        cplan_served("implicit-self-late");
        int omi = comp_method_in_chain(c, dispatch_cid, name, NULL);
        if (omi != mi)
          fprintf(stderr, "plan-check: cplan-conflict: implicit-self-late node %d %s: plan %d, lookup %d\n",
                  id, name, mi, omi);
      }
      /* Template-method pattern: a base-class method calls an abstract method
         that is implemented only in subclasses. Not found up the chain, but if a
         descendant defines it, emit_dispatch can still resolve it virtually on
         self's runtime class. */
      if (mi < 0 && !self->is_cmethod) {
        for (int k = 0; k < c->nclasses; k++) {
          if (k == dispatch_cid || !is_descendant(c, k, dispatch_cid)) continue;
          if (comp_method_in_chain(c, k, name, NULL) >= 0) { mi = k; break; }
        }
      }
      if (mi >= 0 && emit_reopen_own_call(c, id, dispatch_cid, b)) return 1;
      if (mi >= 0) {
        emit_dispatch(c, dispatch_cid, name, g_self, nt_ref(nt, id, "arguments"), nt_ref(nt, id, "block"), b);
        return 1;
      }
      /* A reopened Object's method is every object's: a bare call to it from
         an instance method -- activesupport's `acts_like?(:time)` inside
         DateAndTime::Zones#in_time_zone, copied into Date and Time -- reaches
         it with self as the receiver, boxed the way the explicit `obj.m`
         fallback boxes its receiver. Only inlining served it before; the
         method body itself raised NoMethodError. */
      if (mi < 0 && !self->is_cmethod && nt_ref(nt, id, "block") < 0 && g_self) {
        int oc = comp_class_index(c, "Object");
        int omi = oc >= 0 && oc != dispatch_cid ? comp_method_in_chain(c, oc, name, NULL) : -1;
        if (omi >= 0) {
          const char *scn = c->classes[dispatch_cid].name;
          TyKind st = TY_UNKNOWN;
          if (!scn) st = TY_UNKNOWN;
          else if (sp_streq(scn, "String"))  st = TY_STRING;
          else if (sp_streq(scn, "Integer")) st = TY_INT;
          else if (sp_streq(scn, "Float"))   st = TY_FLOAT;
          else if (sp_streq(scn, "Symbol"))  st = TY_SYMBOL;
          else if (sp_streq(scn, "Time"))    st = TY_TIME;
          else if (sp_streq(scn, "Array") || sp_streq(scn, "Hash") || sp_streq(scn, "Numeric")) st = TY_POLY;
          else if (!is_builtin_reopen(scn) && !comp_class_is_module(c, &c->classes[dispatch_cid]) &&
                   !comp_ty_value_obj(c, ty_object(dispatch_cid))) st = ty_object(dispatch_cid);
          if (st != TY_UNKNOWN) {
            if (g_plan_check) ucall_observe(c, id, omi, oc, 0);
            buf_printf(b, "sp_Object_%s(", mc(c->scopes[omi].name));
            if (ty_is_object(st)) buf_printf(b, "sp_box_obj(%s, %d)", g_self, dispatch_cid);
            else emit_boxed_text(c, st, g_self, b);
            emit_args_filled(c, omi, nt_ref(nt, id, "arguments"), ", ", b);
            buf_puts(b, ")");
            return 1;
          }
        }
      }
    }
  }
  return 0;
}
