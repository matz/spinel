/* codegen_poly_plan.c -- the poly dispatch's arm families, moved out of
   emit_poly_method_dispatch (codegen_call.c), and the --plan-check shadow
   that holds the arms it writes against the resolver's (cplan_poly,
   call_plan.c; #7100 Phase C). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "call_plan.h"

/* ---- --plan-check: the arms one emitted switch wrote ----
   A frame per switch being written; a nested dispatch (an argument's
   default) opens its own. A probe that longjmps out of a frame leaves it
   behind, and the enclosing pa_end drops it with its own. */
typedef struct { int id; unsigned flags; int n, cap; PolyArm *arm; } PaFrame;
static PaFrame *g_pa;
static int g_pa_n, g_pa_cap;
static long g_pa_compared, g_pa_arms, g_pa_conflict, g_pa_missing, g_pa_extra;
static long g_pa_trial_kept, g_pa_trial_dropped;

int pa_begin(int id) {
  if (g_pa_n == g_pa_cap) {
    int ncap = g_pa_cap ? g_pa_cap * 2 : 8;
    g_pa = realloc(g_pa, (size_t)ncap * sizeof *g_pa);
    memset(g_pa + g_pa_cap, 0, (size_t)(ncap - g_pa_cap) * sizeof *g_pa);
    g_pa_cap = ncap;
  }
  PaFrame *f = &g_pa[g_pa_n];
  f->id = id; f->n = 0; f->flags = 0;
  return g_pa_n++;
}

void pa_resume(int frame) {
  if (frame >= 0 && frame < g_pa_n) g_pa_n = frame + 1;
}

void pa_drop(int frame) {
  if (frame >= 0 && frame < g_pa_n) g_pa_n = frame;
}

void pa_flags(unsigned flags) {
  if (g_pa_n > 0) g_pa[g_pa_n - 1].flags = flags | PPF_SEEN;
}

void pa_observe(int kind, int key, int mi, TyKind vty, int conv) {
  if (g_pa_n <= 0) return;
  PaFrame *f = &g_pa[g_pa_n - 1];
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->arm = realloc(f->arm, (size_t)f->cap * sizeof *f->arm);
  }
  PolyArm *a = &f->arm[f->n++];
  a->kind = (unsigned char)kind; a->key = (short)key; a->mi = mi;
  a->vty = (unsigned char)vty; a->conv = (unsigned char)conv; a->def = -1;
}

void pa_observe_at(int id, int kind, int key, int mi, TyKind vty, int conv) {
  if (g_pa_n > 0 && g_pa[g_pa_n - 1].id == id) pa_observe(kind, key, mi, vty, conv);
}

static const char *pa_kind_name(int k) {
  static const char *const nm[] = { "user", "proc-form", "reader", "native", "arity", "synth-enum",
                                    "builtin", "trial" };
  return k >= 0 && k < (int)(sizeof nm / sizeof nm[0]) ? nm[k] : "?";
}

static void pa_arm_text(Compiler *c, const PolyArm *a, char *out, size_t n) {
  static const char *const fam[] = { "len", "empty", "class-named", "class-reflect", "ostruct", "to_a",
                                     "io-rewind", "io-puts", "io-zero", "reduce", "int-chr", "str-transform",
                                     "split", "enum-proc", "synchronize", "callable", "class-members",
                                     "len-cases", "clear", "empty-cases", "compare_by_identity?",
                                     "default enum-each", "default to_s", "default case-conv", "default numeric",
                                     "default digits", "default array-transform", "default predicate",
                                     "default to_i/to_f", "default any?/none?", "default to_h",
                                     "each_index", "join", "alive?", "kill", "status", "queue", "io-read",
                                     "io-flush", "io-close", "enum-to_a",
                                     "cover?", "try_convert", "gcdlcm", "unpack1", "include?", "str-delete",
                                     "str-partition", "str-setop", "store", "str-encode", "str-split",
                                     "int-bitref", "index-cases", "io-read_nonblock", "io-write",
                                     "io-syswrite", "io-print", "io-putc", "io-seek/read", "unshift", "push",
                                     "pack", "join(sep)", "include?-cases", "array-index", "intersect?",
                                     "strftime", "aref-str", "aref-sym", "aref-poly", "predicate(arg)",
                                     "default generic", "default replace", "default round(n)",
                                     "default numeric(n)", "default str-index", "default find_index",
                                     "default first/last(n)", "default delete/dig/values_at",
                                     "default merge", "default aref2", "default aref/fetch",
                                     "block map!", "subclass re-entry", "array re-entry",
                                     "array re-entry (receiver)", "face str-bang" };
  char kb[48];
  if (a->key >= 0 && a->key < c->nclasses) snprintf(kb, sizeof kb, "%s", c->classes[a->key].name);
  else if (a->key == PA_KEY_DEFAULT) snprintf(kb, sizeof kb, "default");
  else if (a->key >= PA_KEY_FACE && a->key < PA_KEY_TRIAL) {
    static const char *const fk[] = { "String", "Array", "Enumerable", "Hash", "Integer", "Float" };
    int f = a->key - PA_KEY_FACE;
    snprintf(kb, sizeof kb, "face %s", f < (int)(sizeof fk / sizeof fk[0]) ? fk[f] : "?");
  }
  else if (a->key >= PA_KEY_TRIAL && a->key < PA_KEY_DEFAULT) {
    static const char *const tn[] = { "string-prearm", "container-read", "array-fallback", "default0",
                                      "generic-tail", "default-n", "block-default" };
    int t = a->key - PA_KEY_TRIAL;
    snprintf(kb, sizeof kb, "%s", t < (int)(sizeof tn / sizeof tn[0]) ? tn[t] : "?");
  }
  else if (a->key >= PA_KEY_CLASS_VALUE && a->key < PA_KEY_BUILTIN && a->key - PA_KEY_CLASS_VALUE < c->nclasses)
    snprintf(kb, sizeof kb, "%s (class value)", c->classes[a->key - PA_KEY_CLASS_VALUE].name);
  else if (a->key >= PA_KEY_BUILTIN && a->key - PA_KEY_BUILTIN < (int)(sizeof fam / sizeof fam[0]))
    snprintf(kb, sizeof kb, "%s", fam[a->key - PA_KEY_BUILTIN]);
  else snprintf(kb, sizeof kb, "key %d", a->key);
  snprintf(out, n, "%s %s mi %d vty %d conv %d", kb, pa_kind_name(a->kind), a->mi, a->vty, a->conv);
}

static int pa_key_cmp(const void *x, const void *y) {
  return ((const PolyArm *)x)->key - ((const PolyArm *)y)->key;
}

void pa_end(Compiler *c, int frame, const PolyPlan *p) {
  if (frame < 0 || frame >= g_pa_n) return;
  PaFrame *f = &g_pa[frame];
  const char *nm = nt_str(c->nt, f->id, "name");
  char pt[400], ct[400];
  g_pa_compared++;
  g_pa_arms += f->n;
  if ((f->flags & PPF_SEEN) && (f->flags & ~PPF_SEEN) != (p->flags & ~PPF_SEEN)) {
    fprintf(stderr, "plan-check: poly-conflict: node %d %s: flags: plan %#x, codegen %#x\n", f->id,
            nm ? nm : "?", p->flags & ~PPF_SEEN, f->flags & ~PPF_SEEN);
    g_pa_conflict++;
  }
  /* both lists by key: the switch writes its tag pre-arms and builtin cases
     around the class arms */
  PolyArm *pl = p->n ? malloc((size_t)p->n * sizeof *pl) : NULL;
  if (p->n) memcpy(pl, p->arm, (size_t)p->n * sizeof *pl);
  qsort(pl, (size_t)p->n, sizeof *pl, pa_key_cmp);
  qsort(f->arm, (size_t)f->n, sizeof *f->arm, pa_key_cmp);
  int i = 0, j = 0;
  while (i < p->n || j < f->n) {
    const PolyArm *pa = i < p->n ? &pl[i] : NULL, *ca = j < f->n ? &f->arm[j] : NULL;
    if (pa && (!ca || pa->key < ca->key)) {
      pa_arm_text(c, pa, pt, sizeof pt);
      fprintf(stderr, "plan-check: poly-missing: node %d %s: plan %s\n", f->id, nm ? nm : "?", pt);
      g_pa_missing++; i++;
    }
    else if (ca && (!pa || ca->key < pa->key)) {
      pa_arm_text(c, ca, ct, sizeof ct);
      fprintf(stderr, "plan-check: poly-extra: node %d %s: codegen %s\n", f->id, nm ? nm : "?", ct);
      g_pa_extra++; j++;
    }
    else if (pa->kind == PA_TRIAL && ca->kind == PA_TRIAL) {
      /* offered by both: what it answered is the emission's to know */
      if (ca->conv) g_pa_trial_kept++; else g_pa_trial_dropped++;
      i++; j++;
    }
    else {
      if (pa->kind != ca->kind || pa->mi != ca->mi || pa->vty != ca->vty || pa->conv != ca->conv) {
        pa_arm_text(c, pa, pt, sizeof pt); pa_arm_text(c, ca, ct, sizeof ct);
        fprintf(stderr, "plan-check: poly-conflict: node %d %s: plan %s, codegen %s\n", f->id,
                nm ? nm : "?", pt, ct);
        g_pa_conflict++;
      }
      i++; j++;
    }
  }
  free(pl);
  g_pa_n = frame;
}

void pa_report(void) {
  fprintf(stderr, "plan-check: poly-arms: %ld switches, %ld arms, %ld conflicts, %ld missing, %ld extra,"
          " %ld trials kept, %ld dropped\n",
          g_pa_compared, g_pa_arms, g_pa_conflict, g_pa_missing, g_pa_extra, g_pa_trial_kept,
          g_pa_trial_dropped);
}

/* The arm class k takes in a zero-argument poly dispatch, decided class by
   class as the dispatch did before the plan (cplan_poly) took it over:
   --plan-check's assertion, and the arms of a dispatch the plan cannot
   serve. 0 for none. */
static int poly_user_arm0_decide(Compiler *c, int id, const char *name, int argc, TyKind ret, int k,
                                 PolyArm *a) {
  const NodeTable *nt = c->nt;
  memset(a, 0, sizeof *a);
  a->key = (short)k; a->mi = -1; a->def = -1; a->vty = TY_UNKNOWN; a->conv = PC_SAME;
  /* A never-instantiated class can't be this poly value's runtime class,
     so drop its arm (method or reader); the referenced symbol then DCEs
     as an unreferenced static (#1608). A primitive reopen's instances
     exist without any constructor, so it keeps its arm (#4219). */
  if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) return 0;
  /* native (C-backed) class arm: dispatch a declared no-arg method to its
     C symbol on the cast receiver, coercing the result into the slot. */
  if (c->classes[k].is_native_class) {
    int nmi = comp_native_method_find(c, k, name, 0, 0);
    /* only a binding that takes no argument: the lookup falls back to
       any arity, and StringScanner#peek(len) beside two user `peek`s
       got an arm calling it with none, which C refused (#5359). A
       receiver of that class then lands in the default arm. */
    if (nmi >= 0 && native_takes(&c->native_methods[nmi], 0)) {
      TyKind mret = native_spec_to_ty(c->native_methods[nmi].ret);
      a->kind = PA_NATIVE; a->vty = (unsigned char)mret;
      a->conv = mret == TY_NIL ? PC_VOID
              : ret == TY_POLY && mret != TY_POLY ? PC_BOX
              : ret != TY_POLY && mret == TY_POLY ? PC_UNBOX : PC_SAME;
      return 1;
    }
    /* one that needs arguments is CRuby's ArgumentError for this call */
    if (nmi >= 0 && c->native_methods[nmi].nargs > 0 && c->classes[k].instantiated) {
      a->kind = PA_ARITY; a->conv = PC_VOID;
      return 1;
    }
    return 0;
  }
  int defcls = -1;
  int mi = comp_method_in_chain(c, k, name, &defcls);
  /* an exception class with no method of its own for the name takes
     the builtin reopening's (`class Exception; def brief`) */
  if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
    int xd = exc_arm_definer(c, k, name);
    if (xd >= 0) { defcls = xd; mi = comp_method_in_chain(c, xd, name, NULL); }
  }
  /* A Struct's synthesized each or each_pair, called with no block on a
     boxed receiver: the typed receiver answers an Enumerator over the
     members (its blockless each is redirected through __enum_to_a, its
     each_pair through to_h), and the synthesized method, which yields,
     raised LocalJumpError when this arm called it with no block. The
     each Enumerator is built over the member array the generated
     per-class dispatch answers, the each_pair one over the member hash
     the to_h hook answers, so it yields [name, value] pairs. Not for a
     Data class, which has neither name in CRuby. */
  if (mi >= 0 && c->classes[k].is_struct && !c->classes[k].is_data &&
      g_gen_obj_struct_values && ret == TY_POLY && argc == 0 &&
      nt_ref(nt, id, "block") < 0 &&
      nt_str(nt, c->scopes[mi].def_node, "synth") &&
      (sp_streq(name, "each") || sp_streq(name, "each_pair"))) {
    a->kind = PA_SYNTH_ENUM; a->mi = mi; a->vty = TY_ENUMERATOR;
    return 1;
  }
  /* Skip a method with no standalone definition to call: DCE-pruned (its
     params stayed TY_UNKNOWN so it was marked unreachable) or inlined at
     call sites because it yields. Emitting a `case` arm that calls the
     absent `sp_Class_method` symbol dangles at link (issue #1583). The
     class can never be the receiver of this poly value anyway. */
  /* A yielding candidate has a proc form emitted for exactly this
     dispatch (#3399); it is as callable as any other symbol here. */
  { char zexp[600]; int rdc0 = -1;
    if (poly_arm_refuses_none(c, mi, zexp, sizeof zexp) &&
        !comp_reader_in_chain(c, k, name, &rdc0)) {
      a->kind = PA_ARITY; a->mi = mi; a->conv = PC_VOID;
      return 1;
    } }
  if (mi >= 0 && c->scopes[mi].nrequired == 0 &&
      comp_resolve_member(c, k, name, 0, NULL, NULL) != SP_MEMBER_ATTR &&
      (scope_has_callable_symbol(c, mi) || scope_needs_proc_form(c, mi)) &&
      !(c->classes[defcls].name && sp_streq(c->classes[defcls].name, "Class"))) {
    /* A proc form is a separately inferred clone, so its own return type
       is the one to read -- not the original's, which an inlined-only
       method never needed (#3399). */
    int pfi9 = scope_proc_form_of(c, mi);
    Scope *ms = &c->scopes[pfi9 >= 0 ? pfi9 : mi];
    TyKind cret9 = ms->ret;
    TyKind slotty = is_scalar_ret(ret) ? ret : TY_INT;
    a->kind = pfi9 >= 0 ? PA_PROC_FORM : PA_USER; a->mi = mi; a->def = (short)defcls;
    a->vty = (unsigned char)cret9;
    a->conv = method_is_void(ms) ? PC_VOID
            : ret == TY_POLY && cret9 != TY_POLY ? PC_BOX
            : ret != TY_POLY && cret9 == TY_POLY ? PC_UNBOX
            : (slotty == TY_INT || slotty == TY_FLOAT) && cret9 == TY_BIGINT ? PC_NUM : PC_SAME;
    return 1;
  }
  int rdcls = -1;
  if (comp_reader_in_chain(c, k, name, &rdcls)) {
    const char *rn3 = comp_resolve_alias(c, k, name);
    char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn3);
    int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
    TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
    a->kind = PA_READER; a->def = (short)rdcls; a->vty = (unsigned char)ivt;
    a->conv = ret == TY_POLY && ivt == TY_INT ? PC_BOX_OR_NIL
            : ret == TY_POLY && ivt != TY_POLY ? PC_BOX
            : ret != TY_POLY && ivt == TY_POLY ? PC_UNBOX
            : ivt == TY_STRBUF && ret == TY_STRING ? PC_COPY : PC_SAME;
    return 1;
  }
  return 0;
}

/* One user-class arm of a zero-argument poly dispatch, as the plan (or the
   decision above) gives it: `case k:` writing the result temp _t<tr> from
   receiver _t<tv>, the arm's value fitted to the call's type ret. */
static void emit_poly_user_arm0(Compiler *c, int id, const char *name, TyKind ret, int tv, int tr,
                                int blk_tmp0, const PolyArm *a, Buf *b) {
  int k = a->key, mi = a->mi;
  if (a->kind == PA_NATIVE || (a->kind == PA_ARITY && mi < 0)) {
    int nmi = comp_native_method_find(c, k, name, 0, 0);
    if (a->kind == PA_NATIVE) {
      NativeMethod *nmet = &c->native_methods[nmi];
      char nbuf[300];
      const char *rest0 = nmet->rest ? ", 0, NULL" : "";
      if (sp_streq(nmet->ret, "string?"))
        snprintf(nbuf, sizeof nbuf, "sp_box_nullable_str(%s((%s *)_t%d.v.p%s))", nmet->csym, c->classes[k].c_struct, tv, rest0);
      else
        snprintf(nbuf, sizeof nbuf, "%s((%s *)_t%d.v.p%s)", nmet->csym, c->classes[k].c_struct, tv, rest0);
      TyKind mret = native_spec_to_ty(nmet->ret);
      buf_printf(b, " case %d: ", k);
      int pconv = PC_SAME;
      if (mret == TY_NIL) { buf_puts(b, nbuf); pconv = PC_VOID; }
      else {
        buf_printf(b, "_t%d = ", tr);
        if (ret == TY_POLY && mret != TY_POLY) { emit_boxed_text(c, mret, nbuf, b); pconv = PC_BOX; }
        else if (ret != TY_POLY && mret == TY_POLY) {
          emit_unbox_poly_ret(c, is_scalar_ret(ret) ? ret : TY_INT, nbuf, b);
          pconv = PC_UNBOX;
        }
        else buf_puts(b, nbuf);
      }
      buf_puts(b, "; break;");
      if (g_plan_check) pa_observe(PA_NATIVE, k, -1, mret, pconv);
      return;
    }
    char am[128];
    arity_message(am, sizeof am, 0, c->native_methods[nmi].nargs, c->native_methods[nmi].nargs, NULL);
    buf_printf(b, " case %d: sp_raise_cls(\"ArgumentError\", \"%s\"); break;", k, am);
    if (g_plan_check) pa_observe(PA_ARITY, k, -1, TY_UNKNOWN, PC_VOID);
    return;
  }
  if (a->kind == PA_SYNTH_ENUM) {
    buf_printf(b, " case %d: _t%d = sp_box_obj(sp_Enumerator_new_from(%s(_t%d)), "
                  "SP_BUILTIN_ENUMERATOR); break;",
               k, tr, sp_streq(name, "each") ? "sp_obj_struct_values_fn" : "sp_obj_to_h_fn", tv);
    if (g_plan_check) pa_observe(PA_SYNTH_ENUM, k, mi, TY_ENUMERATOR, PC_SAME);
    return;
  }
  if (a->kind == PA_ARITY) {
    char zexp[600];
    poly_arm_refuses_none(c, mi, zexp, sizeof zexp);
    buf_printf(b, " case %d: ", k); emit_poly_arity_raise(b, zexp); buf_puts(b, " break;");
    if (g_plan_check) pa_observe(PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID);
    return;
  }
  if (a->kind == PA_USER || a->kind == PA_PROC_FORM) {
    int defcls = a->def;
    nd_callee(c, id, mi, defcls, 1);   /* one switch arm (#4557) */
    /* Build the call; append default values for any optional params
       not provided by the (zero-arg) call site. */
    Buf cb; memset(&cb, 0, sizeof cb);
    /* A reopened primitive (Integer/Float/String/Symbol) method takes the
       unboxed value, not a struct pointer -- read the matching union field
       instead of casting .v.p to a non-existent sp_<Prim> struct. */
    const char *_dcn = c->classes[defcls].c_name;
    char _dself[320];
    int _dstruct = 0;   /* the receiver is a struct pointer in .v.p */
    if (sp_streq(_dcn, "Integer")) snprintf(_dself, sizeof _dself, "_t%d.v.i", tv);
    /* a Numeric reopen takes self BOXED (it serves Integer and Float
       alike), not the int payload (blank.rb) */
    else if (sp_streq(_dcn, "Numeric")) snprintf(_dself, sizeof _dself, "_t%d", tv);
    else if (sp_streq(_dcn, "Float")) snprintf(_dself, sizeof _dself, "_t%d.v.f", tv);
    /* a plain string box or a mutable handle: deref answers the live
       text for the handle and is the identity for the plain box */
    else if (sp_streq(_dcn, "String")) snprintf(_dself, sizeof _dself, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn, "Symbol")) snprintf(_dself, sizeof _dself, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn, "NilClass")) snprintf(_dself, sizeof _dself, "0");
    /* Object's (and Array's) methods take self boxed */
    else if (sp_streq(_dcn, "Object") || sp_streq(_dcn, "Array"))
      snprintf(_dself, sizeof _dself, "_t%d", tv);
    else if (sp_streq(_dcn, "TrueClass") || sp_streq(_dcn, "FalseClass"))
      snprintf(_dself, sizeof _dself, "(int)_t%d.v.b", tv);
    else if (sp_streq(_dcn, "NilClass")) snprintf(_dself, sizeof _dself, "0");
    else if (sp_streq(_dcn, "TrueClass") || sp_streq(_dcn, "FalseClass")) snprintf(_dself, sizeof _dself, "(int)_t%d.v.i", tv);
    else if (sp_streq(_dcn, "Array") || sp_streq(_dcn, "Hash") || sp_streq(_dcn, "Object"))
      snprintf(_dself, sizeof _dself, "_t%d", tv);
    /* a boxed Time / Range points at the value; the reopen takes it by value */
    else if (sp_streq(_dcn, "Time") || sp_streq(_dcn, "Range"))
      snprintf(_dself, sizeof _dself, "*(sp_%s *)_t%d.v.p", _dcn, tv);
    /* a boxed thread is the runtime's sp_thread handle (not an sp_Thread struct) */
    else if (sp_streq(_dcn, "Thread")) { snprintf(_dself, sizeof _dself, "(sp_thread *)_t%d.v.p", tv); _dstruct = 1; }
    /* a boxed IO or socket is the runtime's sp_File handle */
    else if (io_family_class(c, defcls)) {
      /* a block-taking IO reopening has no standalone function (as typed) */
      if (c->scopes[mi].yields || (c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]))
        unsupported(c, id, "call");
      snprintf(_dself, sizeof _dself, "(sp_File *)_t%d.v.p", tv); _dstruct = 1;
    }
    /* a boxed exception is the runtime's sp_Exception, whatever its class */
    else if (class_has_exc_name(c, defcls)) { snprintf(_dself, sizeof _dself, "(sp_Exception *)_t%d.v.p", tv); _dstruct = 1; }
    /* a by-value (value-type) class method takes self by value:
       dereference the boxed pointer instead of passing it (#2441) */
    else if (c->classes[defcls].is_value_type) {
      snprintf(_dself, sizeof _dself, "*(sp_%s *)_t%d.v.p", _dcn, tv); _dstruct = 1; }
    else { snprintf(_dself, sizeof _dself, "(sp_%s *)_t%d.v.p", _dcn, tv); _dstruct = 1; }
    /* a yielding candidate is called through its proc-form clone (#3399) */
    int pfi9 = scope_proc_form_of(c, mi);
    buf_printf(&cb, "sp_%s_%s(%s", mc_reopen_cls(c, defcls, c->scopes[mi].name),
               mc(pfi9 >= 0 ? c->scopes[pfi9].name : c->scopes[mi].name), _dself);
    if (c->scopes[mi].nparams > 0) {
      const char *saved_self = g_self;
      const char *saved_deref = g_self_deref;
      char selfpbuf[320];  /* stack-local: nested inlines each need their own receiver buffer */
      /* A default reads the receiver's ivars as `<self><deref><iv>`, so
         the receiver is parenthesized: a bare cast binds looser than
         `->`. A by-value class is spelled as the dereferenced value, so
         a `self` default passes what the callee takes. */
      if (_dstruct && c->classes[defcls].is_value_type) {
        snprintf(selfpbuf, sizeof selfpbuf, "(*(sp_%s *)_t%d.v.p)", _dcn, tv);
        g_self_deref = ".";
      }
      else if (_dstruct && io_family_class(c, defcls)) {
        snprintf(selfpbuf, sizeof selfpbuf, "((sp_File *)_t%d.v.p)", tv);
        g_self_deref = "->";
      }
      else if (_dstruct && class_has_exc_name(c, defcls)) {
        snprintf(selfpbuf, sizeof selfpbuf, "((sp_Exception *)_t%d.v.p)", tv);
        g_self_deref = "->";
      }
      else if (_dstruct) {
        snprintf(selfpbuf, sizeof selfpbuf, "((sp_%s *)_t%d.v.p)", _dcn, tv);
        g_self_deref = "->";
      }
      else snprintf(selfpbuf, sizeof selfpbuf, "%s", _dself);
      g_self = selfpbuf;
      /* the defaults are spelled for the proc form's own parameter
         types when that is the symbol called (#4492) */
      Scope *ds = &c->scopes[pfi9 >= 0 ? pfi9 : mi];
      for (int ai = 0; ai < ds->nparams; ai++) {
        buf_puts(&cb, ", "); emit_arg_or_default(c, ds, ai, -1, &cb);
      }
      g_self = saved_self; g_self_deref = saved_deref;
    }
    /* self is always the first argument here, so a zero-param method
       still needs the separator the helper only adds after a real
       parameter list. */
    if (c->scopes[mi].nparams == 0 && c->scopes[mi].blk_param &&
        c->scopes[mi].blk_param[0] && !c->scopes[mi].yields)
      buf_puts(&cb, ", ");
    if (scope_needs_proc_form(c, mi)) {
      /* the proc form always takes the block, and its scope still reads
         as yielding, so the helper declines it (#3399) */
      if (blk_tmp0 >= 0) buf_printf(&cb, ", _t%d", blk_tmp0);
      else buf_puts(&cb, ", NULL");
    }
    else emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp0, &cb);
    buf_puts(&cb, ")");
    const char *call = cb.p ? cb.p : "";
    buf_printf(b, " case %d: ", k);
    /* A proc form is a separately inferred clone, so its own return type
       is the one to read -- not the original's, which an inlined-only
       method never needed (#3399). */
    int pf9 = pfi9 >= 0;
    TyKind cret9 = pf9 ? c->scopes[pfi9].ret : c->scopes[mi].ret;
    int pconv = PC_SAME;
    if ((pf9 ? method_is_void(&c->scopes[pfi9]) : method_is_void(&c->scopes[mi]))) {
      buf_puts(b, call);  /* void: no usable value */
      pconv = PC_VOID;
    }
    else {
      TyKind slotty = is_scalar_ret(ret) ? ret : TY_INT;
      buf_printf(b, "_t%d = ", tr);
      if (ret == TY_POLY && cret9 != TY_POLY) { emit_boxed_text(c, cret9, call, b); pconv = PC_BOX; }
      /* The slot is scalar (e.g. a length dispatch fixed to sp_int) but
         this class's method widened its return to poly: coerce down. */
      else if (ret != TY_POLY && cret9 == TY_POLY) { emit_unbox_poly_ret(c, slotty, call, b); pconv = PC_UNBOX; }
      /* Two classes own the name and answer different types (one an
         Integer, another a Bignum): the slot took one of them, so the
         odd arm converts into it rather than emitting a type error. */
      else if (slotty == TY_INT && cret9 == TY_BIGINT) { buf_printf(b, "sp_bigint_to_int(%s)", call); pconv = PC_NUM; }
      else if (slotty == TY_FLOAT && cret9 == TY_BIGINT) { buf_printf(b, "sp_bigint_to_double(%s)", call); pconv = PC_NUM; }
      else buf_puts(b, call);
    }
    buf_puts(b, "; break;");
    free(cb.p);
    if (g_plan_check) pa_observe(pf9 ? PA_PROC_FORM : PA_USER, k, mi, cret9, pconv);
    return;
  }
  /* a reader: the ivar of the class that holds the attr */
  int rdcls = a->def;
  const char *rn3 = comp_resolve_alias(c, k, name);
  char fld[600];
  snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[rdcls].c_name, tv, iv_c(rn3));
  char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn3);
  int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
  TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
  buf_printf(b, " case %d: _t%d = ", k, tr);
  int pconv = PC_SAME;
  /* an int ivar is SP_INT_NIL-defaulted: box the sentinel as nil
     (#3288); or_nil is a no-op on a real int */
  if (ret == TY_POLY && ivt == TY_INT) {
    buf_printf(b, "sp_box_int_or_nil(%s)", fld);
    pconv = PC_BOX_OR_NIL;
  }
  else if (ret == TY_POLY && ivt != TY_POLY) { emit_boxed_text(c, ivt, fld, b); pconv = PC_BOX; }
  /* The slot is scalar (e.g. a length dispatch fixed to sp_int) but
     this class's ivar widened to poly: coerce down. */
  else if (ret != TY_POLY && ivt == TY_POLY) {
    emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, fld, b);
    pconv = PC_UNBOX;
  }
  /* shared-mutable ivar read into a plain string slot: the safe
     copy, not the raw handle pointer (#3227) */
  else if (ivt == TY_STRBUF && ret == TY_STRING) {
    buf_printf(b, "%s ? sp_str_concat(sp_String_cstr(%s), (&(\"\\xff\")[1])) : NULL", fld, fld);
    pconv = PC_COPY;
  }
  else buf_puts(b, fld);
  buf_puts(b, "; break;");
  if (g_plan_check) pa_observe(PA_READER, k, -1, ivt, pconv);
}

/* --plan-check: the arms the plan gives a dispatch held against the ones
   its own decision would have taken, class by class */
static void poly_arms_assert(Compiler *c, int id, const char *site, const PolyArm *pl, int npl,
                             const PolyArm *old, int nold) {
  const char *nm = nt_str(c->nt, id, "name");
  int i = 0, j = 0;
  while (i < npl || j < nold) {
    const PolyArm *p = i < npl ? &pl[i] : NULL, *o = j < nold ? &old[j] : NULL;
    if (p && o && p->key == o->key) {
      if (p->kind != o->kind || p->mi != o->mi || p->def != o->def || p->vty != o->vty || p->conv != o->conv)
        fprintf(stderr, "plan-check: cplan-conflict: %s node %d %s: class %d: plan %d/%d/%d/%d/%d, "
                "decided %d/%d/%d/%d/%d\n", site, id, nm ? nm : "?", p->key, p->kind, p->mi, p->def, p->vty,
                p->conv, o->kind, o->mi, o->def, o->vty, o->conv);
      i++; j++;
    }
    else if (p && (!o || p->key < o->key)) {
      fprintf(stderr, "plan-check: cplan-conflict: %s node %d %s: class %d: plan only\n", site, id,
              nm ? nm : "?", p->key);
      i++;
    }
    else {
      fprintf(stderr, "plan-check: cplan-conflict: %s node %d %s: class %d: decided only\n", site, id,
              nm ? nm : "?", o->key);
      j++;
    }
  }
}

/* The user-class arms of a zero-argument poly dispatch
   (emit_poly_method_dispatch): one `case` per class a boxed receiver can be
   at run time -- its method (or proc form), a reader, a native binding, an
   arity refusal -- as the dispatch's plan lists them (cplan_poly). A plan
   made for another result type than the dispatch's own (none is) falls
   back on the class-by-class decision, which --plan-check also holds the
   plan against. */
void emit_poly_user_arms0(Compiler *c, int id, const char *name, int argc, TyKind ret, int tv, int tr,
                          int blk_tmp0, const PolyPlan *p, Buf *b) {
  int served = p->ret == ret;
  /* the class arms, copied: an arm's emission may resolve another plan */
  PolyArm *arms = malloc(sizeof *arms * (size_t)(p->n > 0 ? p->n : 1));
  int n = 0;
  if (served)
    for (int i = 0; i < p->n; i++)
      if (p->arm[i].key >= 0 && p->arm[i].key < c->nclasses) arms[n++] = p->arm[i];
  if (g_plan_check || !served) {
    PolyArm *old = malloc(sizeof *old * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
    int nold = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (poly_user_arm0_decide(c, id, name, argc, ret, k, &old[nold])) nold++;
    if (served) poly_arms_assert(c, id, "poly-user0", arms, n, old, nold);
    else {
      if (g_plan_check) fprintf(stderr, "plan-check: cplan-fallback: poly-user0 node %d %s\n", id, name);
      free(arms);
      arms = old; n = nold; old = NULL;
    }
    free(old);
  }
  if (g_plan_check && served) cplan_served("poly-user0");
  for (int i = 0; i < n; i++) emit_poly_user_arm0(c, id, name, ret, tv, tr, blk_tmp0, &arms[i], b);
  free(arms);
}

/* Class k's arm in a poly dispatch with arguments, decided and written as
   the dispatch did before the plan took the arms over: what a dispatch the
   plan cannot serve writes, and, into a scratch buffer, what a class with
   no arm in the plan still does (a callee note, a refusal, a temp it
   numbers). 1 when it wrote an arm. */
static int poly_user_arm_n_replay(Compiler *c, int id, const char *name, const PolyUserArgs *U, int k,
                                  Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = U->argc, pos_argc = U->pos_argc, kwh = U->kwh, kwall = U->kwall, kwall_any = U->kwall_any;
  int kw_pos = U->kw_pos, has_splat_arg = U->has_splat_arg, splat_a = U->splat_a, stk = U->stk;
  int is_setter_val = U->is_setter_val, blk_tmp2 = U->blk_tmp2, tv = U->tv, tr = U->tr;
  TyKind ret = U->ret;
  const int *argv = U->argv, *atmp = U->atmp, *htmp = U->htmp;
  const TyKind *atmp_ty = U->atmp_ty;
  const PolyKw *kw = U->kw;
  (void)nt; (void)kwall_any;
  /* native (C-backed) class arm: a declared method of this arity takes
     the hoisted temps in its native representation, as the zero-arg
     dispatch's arm and the typed-receiver call do. The argument
     dispatch had no such arm, so a native object reached through a
     poly slot answered NoMethodError for every call with an argument
     while its zero-arg calls dispatched (#4504). Positional calls
     only: a binding declares no keywords and no splat. */
  if (c->classes[k].is_native_class) {
    /* a lone splat goes whole to a binding that is all :rest */
    if (kw_pos && has_splat_arg && argc == 1 && splat_a == 0 && kwh < 0 &&
        c->classes[k].instantiated) {
      const NativeMethod *rm = NULL;
      for (int i = 0; i < c->n_native_methods && !rm; i++) {
        const NativeMethod *m = &c->native_methods[i];
        if (m->class_id == k && m->kind == 0 && m->rest && m->nargs == 0 && sp_streq(m->name, name))
          rm = m;
      }
      if (!rm) return 0;
      Buf cb; memset(&cb, 0, sizeof cb);
      int sp = stk >= 0 ? stk : atmp[0];
      if (sp_streq(rm->ret, "string?")) buf_puts(&cb, "sp_box_nullable_str(");
      buf_printf(&cb, "%s((%s *)_t%d.v.p, _t%d->len, _t%d->data)",
                 rm->csym, c->classes[k].c_struct, tv, sp, sp);
      if (sp_streq(rm->ret, "string?")) buf_puts(&cb, ")");
      TyKind mret = sp_streq(rm->ret, "self") ? ty_object(k) : native_spec_to_ty(rm->ret);
      buf_printf(b, " case %d: ", k);
      int pconv = emit_poly_native_arm_stmt(c, cb.p, mret, ret, tr, is_setter_val, b);
      buf_puts(b, " break;");
      free(cb.p);
      if (g_plan_check) pa_observe(PA_NATIVE, k, -1, mret, pconv);
      return 1;
    }
    if (!kw_pos || has_splat_arg || !c->classes[k].instantiated) return 0;
    Buf cb; TyKind mret = TY_UNKNOWN;
    if (kwall < 0) {
      if (!poly_native_arm_call(c, k, name, argc, argv, atmp, atmp_ty, tv, &cb, &mret)) return 0;
      buf_printf(b, " case %d: ", k);
      int pconv = emit_poly_native_arm_stmt(c, cb.p, mret, ret, tr, is_setter_val, b);
      buf_puts(b, " break;");
      free(cb.p);
      if (g_plan_check) pa_observe(PA_NATIVE, k, -1, mret, pconv);
      return 1;
    }
    Buf arm; memset(&arm, 0, sizeof arm);
    int fit = 0;
    buf_printf(&arm, " case %d: if (_t%d->len == 0) { ", k, kwall);
    for (int n = pos_argc; n <= argc; n++) {
      if (n == argc) buf_puts(&arm, " }\nelse { ");
      if (poly_native_arm_call(c, k, name, n, argv, atmp, atmp_ty, tv, &cb, &mret) &&
          (mret == TY_NIL || is_setter_val || ret == TY_POLY || mret == TY_POLY || mret == ret)) {
        emit_poly_native_arm_stmt(c, cb.p, mret, ret, tr, is_setter_val, &arm);
        fit = 1;
      }
      else buf_printf(&arm, "sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d));", name, tv);
      free(cb.p);
    }
    buf_puts(&arm, " } break;");
    if (fit) buf_puts(b, arm.p);
    free(arm.p);
    if (g_plan_check && fit) pa_observe(PA_NATIVE, k, -1, TY_UNKNOWN, PC_SAME);
    return fit;
  }
  int defcls = -1;
  int mi = comp_method_in_chain(c, k, name, &defcls);
  /* an exception class with no method of its own for the name takes
     the builtin reopening's (`class Exception; def brief`) */
  if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
    int xd = exc_arm_definer(c, k, name);
    if (xd >= 0) { defcls = xd; mi = comp_method_in_chain(c, xd, name, NULL); }
  }
  if (mi < 0) return 0;
  /* the same widened arity a candidate was counted with above: a
     keyword hash funds the DECLARED keyword params it names (#4205) */
  char arm_exp[600];
  int arm_fit = poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, splat_a,
                               arm_exp, sizeof arm_exp);
  if (arm_fit == 0) return 0;
  /* A class no value can ever be (never `.new`/`.allocate`/`raise`d, no
     Struct, no Marshal escape) cannot be this poly value's receiver, so
     its arm is dead. Dropping it makes sp_<Class>_<name> an unreferenced
     static the C compiler then DCEs -- spinel supplies the accurate
     reference graph, the C compiler removes the code (#1608). A
     primitive reopen's instances exist without any constructor, so it
     keeps its arm (#4219). */
  if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) return 0;
  if (arm_fit < 0) {
    buf_printf(b, " case %d: ", k); emit_poly_arity_raise(b, arm_exp); buf_puts(b, " break;");
    if (g_plan_check) pa_observe(PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID);
    return 1;
  }
  /* Skip a method with no standalone definition (DCE-pruned, or inlined
     at call sites because it yields): a `case` arm calling its absent
     `sp_Class_method` symbol would dangle at link. The class can't be
     this poly value's receiver anyway -- that is why it was pruned, and a
     yielding method's value-position dispatch is moot here (issue #1583). */
  if (!scope_has_callable_symbol(c, mi) && !scope_needs_proc_form(c, mi)) return 0;
  nd_callee(c, id, mi, defcls, 1);   /* one switch arm (#4557) */
  /* A candidate whose concrete key parameter type is incompatible with the
     concrete call-site key cannot be this poly value's receiver for that
     key -- e.g. a Symbol-keyed user `[]` reached by a String key, where the
     value's real class is a string-keyed Hash. Passing the key raw would be
     a C pointer/integer type error (const char * into an sp_sym slot), so
     skip the arm. Mirrors the key-type-mismatch handling for typed hashes. */
  int arm_key_incompat = 0;
  /* The arm calls the proc form when the method has one, and the proc
     form is a separately inferred clone: its parameters carry the
     types the C signature was emitted with, where the inlined original's
     may have stayed unknown. `fetch(key, opts = {})` inlined at its
     block sites had an untyped `opts`, so a Hash's `fetch("k", "")`
     through a poly slot took the Cache arm and handed a String to its
     sp_SymPolyHash * (#4492). */
  Scope *ks = &c->scopes[scope_proc_form_of(c, mi) >= 0 ? scope_proc_form_of(c, mi) : mi];
  /* which argument each parameter takes, decided once for this arm
     (arg_layout): the type test below and the binding read the same */
  PolyArgs pargs = { argv, pos_argc, atmp, atmp_ty, kw, htmp };
  ArgLayout L;
  poly_arm_layout(c, ks, &pargs, &L);
  if (splat_a >= 0 && !L.gather)
    unsupported(c, id, "a splat into this parameter list, on a value of more than one type");
  for (int a = 0; a < ks->nparams; a++) {
    /* Only a parameter an argument's temp funds is tested: a keyword
       binds by name, a rest packs any argument type into a PolyArray
       (comparing it against a String arg dropped the whole arm, #3218),
       and a gathered element arrives boxed and unboxes to each arm's
       type. */
    if (L.from[a] != ARG_NODE || L.arg[a] >= pos_argc) continue;
    LocalVar *pv0 = (ks->pnames && ks->pnames[a])
                      ? scope_local(ks, ks->pnames[a]) : NULL;
    TyKind pt0 = pv0 ? pv0->type : TY_UNKNOWN;
    int sa0 = L.arg[a];
    TyKind at0 = atmp_ty[sa0];
    /* a shared-handle parameter takes a String of either form: its
       arm passes the handle (emit_poly_shared_arg) */
    if (pt0 == TY_STRBUF && pv0->str_shared && (at0 == TY_STRING || at0 == TY_STRBUF))
      continue;
    int pc = pt0 != TY_POLY && pt0 != TY_UNKNOWN && pt0 != TY_NIL && pt0 != TY_VOID;
    int ac = at0 != TY_POLY && at0 != TY_UNKNOWN && at0 != TY_NIL && at0 != TY_VOID;
    if (pc && ac && pt0 != at0 &&
        (pt0 == TY_STRING || at0 == TY_STRING ||
         /* a heap pointer against a scalar, whatever the kinds: a mutable
            String (sp_String *) reaching an Integer-seeded `[]=` value
            slot was passed raw (#4929) */
         needs_root(pt0) != needs_root(at0) ||
         /* pointer/scalar C-representation mismatch: e.g. an int-typed
            param (bound by an unrelated poly==int) receiving a typed
            object arg -- the raw pass would be a C int-conversion
            error and semantic garbage; the arm cannot be this call's
            real target shape */
         ty_is_object(pt0) != ty_is_object(at0) ||
         /* two UNRELATED object classes. A subclass in an ancestor-typed
            slot is fine and gets a cast (#3418), but nothing converts
            between siblings -- and this arm cannot be the call's real
            target, since a receiver of that class would raise in CRuby
            rather than reinterpret the argument. Dropping it is also
            what the un-seeded build does, by diagnosing the body
            honestly instead (#3419). */
         (ty_is_object(pt0) && ty_is_object(at0) &&
          obj_class_unrelated(c, ty_object_class(pt0), ty_object_class(at0))) ||
         /* a struct passed by value converts to nothing: a Range slice
            `s[0...-5]` on an untyped receiver was matching a seeded
            `#[](Symbol)` by name and handing sp_Range to an sp_sym slot
            (#3384). The arm passes its temps raw, so a disagreement here
            is always a hard C error, never a coercion that works. */
         ty_is_struct_valued(pt0) || ty_is_struct_valued(at0) ||
         /* two concretely different CONTAINER kinds. sp_StrStrHash * and
            sp_SymPolyHash * are different structs, so the raw pass is a
            hard C error, not a coercion that works -- the same argument
            the collapsed-keyword slot below already makes. Without it, a
            `merge` seeded Hash[String, String] on an unrelated class
            took the arm for a plain Hash's call and the build stopped
            inside that class (#4172). */
         (ty_is_hash(pt0) && ty_is_hash(at0)) ||
         (ty_is_array(pt0) && ty_is_array(at0)))) {
      arm_key_incompat = 1; break;
    }
  }
  /* The same test for the slot the COLLAPSED keyword hash funds. The
     hash is built sym-keyed, so an arm whose parameter is a concretely
     different hash kind cannot be this call's target -- passing the
     pointer raw is a hard C error, not a coercion that works, and the
     un-seeded build drops the arm for the same reason (#4033). */
  { int kslot = L.kwh_slot;
    if (kslot >= 0 && L.from[kslot] != ARG_BY_NAME && ks->pnames && ks->pnames[kslot]) {
      LocalVar *kpv9 = scope_local(ks, ks->pnames[kslot]);
      TyKind kpt9 = kpv9 ? kpv9->type : TY_UNKNOWN;
      if (ty_is_hash(kpt9) && kpt9 != (kwall_any ? TY_POLY_POLY_HASH : TY_SYM_POLY_HASH))
        arm_key_incompat = 1;
    } }
  if (arm_key_incompat) { arg_layout_free(&L); return 0; }
  TyKind mret = c->scopes[mi].ret;
  Buf cb; memset(&cb, 0, sizeof cb);
  int pfi8 = scope_proc_form_of(c, mi);   /* yielding: call the clone (#3399) */
  /* the proc form is a separately inferred clone whose parameter types
     are the ones its C signature carries: read the arguments against
     it, or a default `{}` for its sp_SymPolyHash * arrived boxed as a
     PolyPolyHash (#4492) */
  Scope *ms = &c->scopes[pfi8 >= 0 ? pfi8 : mi];
  /* a by-value (value-type) class takes self BY VALUE: dereference the
     boxed pointer rather than passing it, as the sibling arm at the
     default-dispatch above already does (#2441). Passing the pointer
     stopped the C build the moment ceaea73e gave `join` a user arm and
     the user's class happened to be a value type (#4091). */
  /* Sized for the longest class name a bundle produces: at 64 the cast
     was silently truncated to `..._t2.v` and the build stopped. */
  char selfpbuf2[320];  /* stack-local: nested inlines each need their own receiver buffer */
  int self2_struct = 0;
  /* A reopened primitive's method takes the unboxed value, not a struct
     pointer -- read the matching union field instead of casting .v.p to
     a non-existent sp_<Prim> struct (#4219), as the zero-arg dispatch's
     arm already does. */
  { const char *_dcn2 = c->classes[defcls].c_name;
    if (sp_streq(_dcn2, "Integer"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "Numeric"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (sp_streq(_dcn2, "TrueClass") || sp_streq(_dcn2, "FalseClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "Time") || sp_streq(_dcn2, "Range"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "*(sp_%s *)_t%d.v.p", _dcn2, tv);
    else if (sp_streq(_dcn2, "Thread"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_thread *)_t%d.v.p", tv);
    else if (io_family_class(c, defcls)) {
      if (c->scopes[mi].yields || (c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]))
        unsupported(c, id, "call");
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_File *)_t%d.v.p)", tv);
    }
    /* a boxed exception is the runtime's sp_Exception, whatever its class */
    else if (class_has_exc_name(c, defcls)) {
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_Exception *)_t%d.v.p)", tv);
      self2_struct = 1; }
    else if (sp_streq(_dcn2, "Array") || sp_streq(_dcn2, "Hash") || sp_streq(_dcn2, "Object"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "Float"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.f", tv);
    else if (sp_streq(_dcn2, "String"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn2, "Symbol"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (sp_streq(_dcn2, "Object") || sp_streq(_dcn2, "Array"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "TrueClass") || sp_streq(_dcn2, "FalseClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.b", tv);
    /* parenthesized: a default reading an ivar spells `<self>->iv_x`,
       and a bare cast binds looser than `->` */
    else {
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_%s *)_t%d.v.p)", _dcn2, tv);
      self2_struct = 1; } }
  /* The ARGUMENT differs from the inline receiver: a by-value class
     takes self BY VALUE, so the boxed pointer is dereferenced for the
     call while g_self keeps the pointer form its ivar reads want. The
     sibling arm at the default dispatch has spelled this out since
     #2441; without it the C build stopped the moment ceaea73e gave
     `join` a user arm and that user's class was a value type. */
  buf_printf(&cb, "sp_%s_%s(%s%s", mc_reopen_cls(c, defcls, c->scopes[mi].name),
             mc(pfi8 >= 0 ? c->scopes[pfi8].name : c->scopes[mi].name),
             c->classes[defcls].is_value_type ? "*" : "", selfpbuf2);
  const char *saved_self = g_self;
  /* the defaults below read ivars off this receiver, whatever the
     calling scope's own self is; a by-value class is the value form */
  const char *saved_deref = g_self_deref;
  char selfdbuf2[340];
  snprintf(selfdbuf2, sizeof selfdbuf2, "%s", selfpbuf2);
  if (self2_struct && c->classes[defcls].is_value_type) {
    snprintf(selfdbuf2, sizeof selfdbuf2, "(*%s)", selfpbuf2);
    g_self_deref = ".";
  }
  else if (self2_struct) g_self_deref = "->";
  Buf pdpre; memset(&pdpre, 0, sizeof pdpre);
  emit_poly_arm_args(c, &c->scopes[mi], ms, &L, &pargs, selfdbuf2, ", ", &pdpre, &cb);
  arg_layout_free(&L);
  g_self = saved_self; g_self_deref = saved_deref;
  if (c->scopes[mi].nparams == 0 && c->scopes[mi].blk_param &&
      c->scopes[mi].blk_param[0] && !c->scopes[mi].yields)
    buf_puts(&cb, ", ");   /* self is the first argument; see the zero-arg dispatch */
  if (scope_needs_proc_form(c, mi)) {
    if (blk_tmp2 >= 0) buf_printf(&cb, ", _t%d", blk_tmp2);
    else buf_puts(&cb, ", NULL");
  }
  else emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp2, &cb);
  buf_puts(&cb, ")");
  if (pdpre.len > 0) {
    /* the bindings and the call in one statement expression, so the
       arm stays a single expression for the boxing below */
    Buf wb; memset(&wb, 0, sizeof wb);
    buf_printf(&wb, "({ %s%s; })", pdpre.p ? pdpre.p : "", cb.p ? cb.p : "");
    free(cb.p); cb = wb;
  }
  free(pdpre.p);
  /* a proc form carries its own inferred return type (#3399) */
  int pf8 = pfi8 >= 0;
  TyKind mret8 = pf8 ? c->scopes[pfi8].ret : mret;
  int pconv = emit_poly_user_arm_n(c, k, cb.p, mret8, &c->scopes[pf8 ? pfi8 : mi], ret, tr,
                                   is_setter_val, b);
  free(cb.p);
  if (g_plan_check) pa_observe(pf8 ? PA_PROC_FORM : PA_USER, k, mi, mret8, pconv);
  return 1;
}

/* The plan's arm (a PolyArm of cplan_poly) in a poly dispatch with
   arguments: a native binding, which writes itself by its own lookup, an
   arity refusal, the method or its proc form (mi, its defining class def). */
static void emit_poly_user_arm_n_plan(Compiler *c, int id, const char *name, const PolyUserArgs *U,
                                      const PolyArm *a, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = U->argc, pos_argc = U->pos_argc, kwh = U->kwh, kwall = U->kwall, kwall_any = U->kwall_any;
  int kw_pos = U->kw_pos, has_splat_arg = U->has_splat_arg, splat_a = U->splat_a, stk = U->stk;
  int is_setter_val = U->is_setter_val, blk_tmp2 = U->blk_tmp2, tv = U->tv, tr = U->tr;
  TyKind ret = U->ret;
  const int *argv = U->argv, *atmp = U->atmp, *htmp = U->htmp;
  const TyKind *atmp_ty = U->atmp_ty;
  const PolyKw *kw = U->kw;
  (void)nt; (void)argc; (void)kwall; (void)kwall_any; (void)kw_pos; (void)has_splat_arg; (void)stk;
  int k = a->key;
  if (c->classes[k].is_native_class) { poly_user_arm_n_replay(c, id, name, U, k, b); return; }
  int mi = a->mi;
  if (a->kind == PA_ARITY) {
    char arm_exp[600];
    poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, splat_a, arm_exp, sizeof arm_exp);
    buf_printf(b, " case %d: ", k); emit_poly_arity_raise(b, arm_exp); buf_puts(b, " break;");
    if (g_plan_check) pa_observe(PA_ARITY, k, mi, TY_UNKNOWN, PC_VOID);
    return;
  }
  int defcls = a->def;
  nd_callee(c, id, mi, defcls, 1);   /* one switch arm (#4557) */
  /* The arm calls the proc form when the method has one, and the proc
     form is a separately inferred clone: its parameters carry the
     types the C signature was emitted with, where the inlined original's
     may have stayed unknown. `fetch(key, opts = {})` inlined at its
     block sites had an untyped `opts`, so a Hash's `fetch("k", "")`
     through a poly slot took the Cache arm and handed a String to its
     sp_SymPolyHash * (#4492). */
  Scope *ks = &c->scopes[scope_proc_form_of(c, mi) >= 0 ? scope_proc_form_of(c, mi) : mi];
  /* which argument each parameter takes, decided once for this arm
     (arg_layout): the type test below and the binding read the same */
  PolyArgs pargs = { argv, pos_argc, atmp, atmp_ty, kw, htmp };
  ArgLayout L;
  poly_arm_layout(c, ks, &pargs, &L);
  if (splat_a >= 0 && !L.gather)
    unsupported(c, id, "a splat into this parameter list, on a value of more than one type");
  TyKind mret = c->scopes[mi].ret;
  Buf cb; memset(&cb, 0, sizeof cb);
  int pfi8 = scope_proc_form_of(c, mi);   /* yielding: call the clone (#3399) */
  /* the proc form is a separately inferred clone whose parameter types
     are the ones its C signature carries: read the arguments against
     it, or a default `{}` for its sp_SymPolyHash * arrived boxed as a
     PolyPolyHash (#4492) */
  Scope *ms = &c->scopes[pfi8 >= 0 ? pfi8 : mi];
  /* a by-value (value-type) class takes self BY VALUE: dereference the
     boxed pointer rather than passing it, as the sibling arm at the
     default-dispatch above already does (#2441). Passing the pointer
     stopped the C build the moment ceaea73e gave `join` a user arm and
     the user's class happened to be a value type (#4091). */
  /* Sized for the longest class name a bundle produces: at 64 the cast
     was silently truncated to `..._t2.v` and the build stopped. */
  char selfpbuf2[320];  /* stack-local: nested inlines each need their own receiver buffer */
  int self2_struct = 0;
  /* A reopened primitive's method takes the unboxed value, not a struct
     pointer -- read the matching union field instead of casting .v.p to
     a non-existent sp_<Prim> struct (#4219), as the zero-arg dispatch's
     arm already does. */
  { const char *_dcn2 = c->classes[defcls].c_name;
    if (sp_streq(_dcn2, "Integer"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "Numeric"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (sp_streq(_dcn2, "TrueClass") || sp_streq(_dcn2, "FalseClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "Time") || sp_streq(_dcn2, "Range"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "*(sp_%s *)_t%d.v.p", _dcn2, tv);
    else if (sp_streq(_dcn2, "Thread"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_thread *)_t%d.v.p", tv);
    else if (io_family_class(c, defcls)) {
      if (c->scopes[mi].yields || (c->scopes[mi].blk_param && c->scopes[mi].blk_param[0]))
        unsupported(c, id, "call");
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_File *)_t%d.v.p)", tv);
    }
    /* a boxed exception is the runtime's sp_Exception, whatever its class */
    else if (class_has_exc_name(c, defcls)) {
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_Exception *)_t%d.v.p)", tv);
      self2_struct = 1; }
    else if (sp_streq(_dcn2, "Array") || sp_streq(_dcn2, "Hash") || sp_streq(_dcn2, "Object"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "Float"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.f", tv);
    else if (sp_streq(_dcn2, "String"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn2, "Symbol"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (sp_streq(_dcn2, "Object") || sp_streq(_dcn2, "Array"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "TrueClass") || sp_streq(_dcn2, "FalseClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.b", tv);
    /* parenthesized: a default reading an ivar spells `<self>->iv_x`,
       and a bare cast binds looser than `->` */
    else {
      snprintf(selfpbuf2, sizeof selfpbuf2, "((sp_%s *)_t%d.v.p)", _dcn2, tv);
      self2_struct = 1; } }
  /* The ARGUMENT differs from the inline receiver: a by-value class
     takes self BY VALUE, so the boxed pointer is dereferenced for the
     call while g_self keeps the pointer form its ivar reads want. The
     sibling arm at the default dispatch has spelled this out since
     #2441; without it the C build stopped the moment ceaea73e gave
     `join` a user arm and that user's class was a value type. */
  buf_printf(&cb, "sp_%s_%s(%s%s", mc_reopen_cls(c, defcls, c->scopes[mi].name),
             mc(pfi8 >= 0 ? c->scopes[pfi8].name : c->scopes[mi].name),
             c->classes[defcls].is_value_type ? "*" : "", selfpbuf2);
  const char *saved_self = g_self;
  /* the defaults below read ivars off this receiver, whatever the
     calling scope's own self is; a by-value class is the value form */
  const char *saved_deref = g_self_deref;
  char selfdbuf2[340];
  snprintf(selfdbuf2, sizeof selfdbuf2, "%s", selfpbuf2);
  if (self2_struct && c->classes[defcls].is_value_type) {
    snprintf(selfdbuf2, sizeof selfdbuf2, "(*%s)", selfpbuf2);
    g_self_deref = ".";
  }
  else if (self2_struct) g_self_deref = "->";
  Buf pdpre; memset(&pdpre, 0, sizeof pdpre);
  emit_poly_arm_args(c, &c->scopes[mi], ms, &L, &pargs, selfdbuf2, ", ", &pdpre, &cb);
  arg_layout_free(&L);
  g_self = saved_self; g_self_deref = saved_deref;
  if (c->scopes[mi].nparams == 0 && c->scopes[mi].blk_param &&
      c->scopes[mi].blk_param[0] && !c->scopes[mi].yields)
    buf_puts(&cb, ", ");   /* self is the first argument; see the zero-arg dispatch */
  if (scope_needs_proc_form(c, mi)) {
    if (blk_tmp2 >= 0) buf_printf(&cb, ", _t%d", blk_tmp2);
    else buf_puts(&cb, ", NULL");
  }
  else emit_cmethod_block_arg(c, id, &c->scopes[mi], blk_tmp2, &cb);
  buf_puts(&cb, ")");
  if (pdpre.len > 0) {
    /* the bindings and the call in one statement expression, so the
       arm stays a single expression for the boxing below */
    Buf wb; memset(&wb, 0, sizeof wb);
    buf_printf(&wb, "({ %s%s; })", pdpre.p ? pdpre.p : "", cb.p ? cb.p : "");
    free(cb.p); cb = wb;
  }
  free(pdpre.p);
  /* a proc form carries its own inferred return type (#3399) */
  int pf8 = pfi8 >= 0;
  TyKind mret8 = pf8 ? c->scopes[pfi8].ret : mret;
  int pconv = emit_poly_user_arm_n(c, k, cb.p, mret8, &c->scopes[pf8 ? pfi8 : mi], ret, tr,
                                   is_setter_val, b);
  free(cb.p);
  if (g_plan_check) pa_observe(pf8 ? PA_PROC_FORM : PA_USER, k, mi, mret8, pconv);
}

/* Class k's arm in a poly dispatch with arguments, decided as the dispatch
   did, without writing or noting anything: --plan-check's assertion. */
static int poly_user_arm_n_decide(Compiler *c, const char *name, const PolyUserArgs *U, int k, PolyArm *a) {
  memset(a, 0, sizeof *a);
  a->key = (short)k; a->mi = -1; a->def = -1; a->vty = TY_UNKNOWN; a->conv = PC_SAME;
  TyKind ret = U->ret;
  if (c->classes[k].is_native_class) {
    TyKind mret = TY_UNKNOWN;
    if (U->kw_pos && U->has_splat_arg && U->argc == 1 && U->splat_a == 0 && U->kwh < 0 &&
        c->classes[k].instantiated) {
      const NativeMethod *rm = NULL;
      for (int i = 0; i < c->n_native_methods && !rm; i++) {
        const NativeMethod *m = &c->native_methods[i];
        if (m->class_id == k && m->kind == 0 && m->rest && m->nargs == 0 && sp_streq(m->name, name)) rm = m;
      }
      if (!rm) return 0;
      mret = sp_streq(rm->ret, "self") ? ty_object(k) : native_spec_to_ty(rm->ret);
    }
    else {
      if (!U->kw_pos || U->has_splat_arg || !c->classes[k].instantiated) return 0;
      if (U->kwall >= 0) {
        int fit = 0;
        for (int n = U->pos_argc; n <= U->argc && !fit; n++)
          if (poly_native_arm_fits(c, k, name, n, U->argv, U->atmp_ty, &mret) >= 0 &&
              (mret == TY_NIL || U->is_setter_val || ret == TY_POLY || mret == TY_POLY || mret == ret))
            fit = 1;
        a->kind = PA_NATIVE;
        return fit;
      }
      if (poly_native_arm_fits(c, k, name, U->argc, U->argv, U->atmp_ty, &mret) < 0) return 0;
    }
    a->kind = PA_NATIVE; a->vty = (unsigned char)mret;
    a->conv = mret == TY_NIL || U->is_setter_val ? PC_VOID
            : ret == TY_POLY && mret != TY_POLY ? PC_BOX
            : ret != TY_POLY && mret == TY_POLY ? PC_UNBOX : PC_SAME;
    return 1;
  }
  int defcls = -1;
  int mi = comp_method_in_chain(c, k, name, &defcls);
  if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
    int xd = exc_arm_definer(c, k, name);
    if (xd >= 0) { defcls = xd; mi = comp_method_in_chain(c, xd, name, NULL); }
  }
  if (mi < 0) return 0;
  char arm_exp[600];
  int arm_fit = poly_arm_count(c, &c->scopes[mi], U->kwh, U->pos_argc, U->splat_a, arm_exp, sizeof arm_exp);
  if (arm_fit == 0) return 0;
  if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) return 0;
  if (arm_fit < 0) { a->kind = PA_ARITY; a->mi = mi; a->conv = PC_VOID; return 1; }
  if (!scope_has_callable_symbol(c, mi) && !scope_needs_proc_form(c, mi)) return 0;
  int pfi = scope_proc_form_of(c, mi);
  Scope *ks = &c->scopes[pfi >= 0 ? pfi : mi];
  PolyArgs pargs = { U->argv, U->pos_argc, U->atmp, U->atmp_ty, U->kw, U->htmp };
  ArgLayout L;
  poly_arm_layout(c, ks, &pargs, &L);
  int fits = cplan_arm_args_fit(c, ks, &L, U->pos_argc, U->atmp_ty, U->kwall_any);
  arg_layout_free(&L);
  if (!fits) return 0;
  TyKind mret = ks->ret;
  a->kind = pfi >= 0 ? PA_PROC_FORM : PA_USER; a->mi = mi; a->def = (short)defcls; a->vty = (unsigned char)mret;
  a->conv = U->is_setter_val || mret == TY_VOID || mret == TY_NIL || method_is_void(ks) ? PC_VOID
          : ret == TY_POLY && mret != TY_POLY ? PC_BOX
          : ret != TY_POLY && mret == TY_POLY ? PC_UNBOX : PC_SAME;
  return 1;
}

/* The user-class arms of a poly dispatch with arguments
   (emit_poly_method_dispatch), as its plan lists them (cplan_poly), in
   class order. A class the plan gives no arm is still taken through the
   dispatch's own decision, into a scratch buffer, for what it does besides
   an arm; a plan for another result type falls back on that decision for
   every class. --plan-check holds the plan against the decision. */
void emit_poly_user_arms_n(Compiler *c, int id, const char *name, const PolyUserArgs *U, Buf *b) {
  const PolyPlan *p = U->plan;
  int served = p->ret == U->ret;
  PolyArm *arms = malloc(sizeof *arms * (size_t)(p->n > 0 ? p->n : 1));
  int n = 0;
  if (served)
    for (int i = 0; i < p->n; i++)
      if (p->arm[i].key >= 0 && p->arm[i].key < c->nclasses) arms[n++] = p->arm[i];
  if (g_plan_check && served) {
    PolyArm *old = malloc(sizeof *old * (size_t)(c->nclasses > 0 ? c->nclasses : 1));
    int nold = 0;
    for (int k = 0; k < c->nclasses; k++)
      if (poly_user_arm_n_decide(c, name, U, k, &old[nold])) nold++;
    poly_arms_assert(c, id, "poly-user-n", arms, n, old, nold);
    free(old);
    cplan_served("poly-user-n");
  }
  if (!served && g_plan_check) fprintf(stderr, "plan-check: cplan-fallback: poly-user-n node %d %s\n", id, name);
  int j = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!served) { poly_user_arm_n_replay(c, id, name, U, k, b); continue; }
    if (j < n && arms[j].key == k) { emit_poly_user_arm_n_plan(c, id, name, U, &arms[j], b); j++; continue; }
    Buf scratch; memset(&scratch, 0, sizeof scratch);
    if (poly_user_arm_n_replay(c, id, name, U, k, &scratch) && g_plan_check)
      fprintf(stderr, "plan-check: cplan-conflict: poly-user-n node %d %s: class %d: decided only\n", id, name, k);
    free(scratch.p);
  }
  free(arms);
}

/* One user-class arm of a poly dispatch with arguments: `case k:` calling
   `call`, its value (mret, from the method or its proc form ms) into the
   result temp _t<tr> as the call's type ret takes it. The conversion
   applied (PolyConv). */
int emit_poly_user_arm_n(Compiler *c, int k, const char *call, TyKind mret, Scope *ms, TyKind ret,
                         int tr, int is_setter_val, Buf *b) {
  int conv = PC_SAME;
  buf_printf(b, " case %d: ", k);
  if (is_setter_val || mret == TY_VOID || mret == TY_NIL || method_is_void(ms)) {
    buf_puts(b, call);  /* no usable value */
    conv = PC_VOID;
  }
  else {
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY && mret != TY_POLY) { emit_boxed_text(c, mret, call, b); conv = PC_BOX; }
    else if (ret != TY_POLY && mret == TY_POLY) {
      emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, call, b);
      conv = PC_UNBOX;
    }
    else buf_puts(b, call);
  }
  buf_puts(b, "; break;");
  return conv;
}

/* The builtin array arms of a poly `x[i]`: the element at index expression
   idxref, into the result temp. */
void emit_poly_index_cases(TyKind ret, int tr, int tv, const char *idxref, Buf *b) {
  if (ret == TY_POLY) {
    /* An Integer or Float array answers a nil element, and any index
       past its end, with its sentinel: the _or_nil box reads that as
       nil. The plain box made `x[9]` the sentinel as a number -- nil?
       false, and NaN for a Float array. */
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_box_int_or_nil(sp_IntArray_get((sp_IntArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = sp_box_str(sp_StrArray_get((sp_StrArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_box_float_or_nil(sp_FloatArray_get((sp_FloatArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_PTR_ARRAY: _t%d = sp_PtrArray_get_box((sp_PtrArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
  }
  else {
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_get((sp_IntArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
  }
}

/* The names a blockless-or-not zero-argument poly dispatch answers
   beside its user arms (the pre-arms and builtin cases of
   emit_poly_method_dispatch), and its user candidates: ncand classes own
   the name at all, ncall_arm of them get a calling arm (the root
   decision). Shared by the dispatch and the resolver (cplan_poly). */
void poly_specials0(Compiler *c, int id, const char *name, PolySpecials0 *s) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  int is_lengthlike = is_count_alias(name);
  int is_empty = sp_streq(name, "empty?");
  /* A class-tagged poly value answers these with its class name (#2656); only
     when no user class defines them, or that user method is the real target. */
  int is_class_named = is_name_reader(name) && !recv_user_defines(c, name);
  /* The Module reflection a class-tagged poly value answers. `ancestors` and
     friends had an arm only for a receiver typed TY_CLASS -- a constant --
     so iterating an Array of classes and asking the block parameter left the
     call with no arm at all and it reported the method as undefined (#4018). */
  int is_class_reflect = (sp_streq(name, "ancestors") || sp_streq(name, "included_modules") ||
                          sp_streq(name, "superclass") ||
                          /* the class-side names the generated sp_cls_* answer
                             (members has its arm in emit_poly_call) */
                          (g_gen_cls_answers && nt_ref(nt, id, "block") < 0 &&
                           (sp_streq(name, "subclasses") || sp_streq(name, "allocate") ||
                            sp_streq(name, "keyword_init?")))) &&
                         !recv_user_defines(c, name);
  /* `members` on a Class read out of a container is its member list (the
     generated sp_cls_members). emit_poly_call's arm answers that, unless
     a class reads a value of its own as `members` or defines the method:
     then the call is this dispatch's, and a boxed class, which carries the
     CLASS's id, would take that class's instance arm. Where no class-side
     switch takes class values (emit_poly_cls_value_prearm), they are
     told apart ahead of the instance switch. */
  int is_cls_members = sp_streq(name, "members") && g_gen_cls_answers &&
                       nt_ref(nt, id, "block") < 0;
  int is_pred = nt_ref(nt, id, "block") < 0 && poly_pred_kind(name, 0);
  /* When ostruct is in the program a bare `obj.reader` on a poly value may be
     an OpenStruct member access (any name) -- read it at runtime (#3197).
     The check keys on cls_id == SP_BUILTIN_OPENSTRUCT, so it cannot alias a
     user-class arm and coexists with them: a poly value that unions an
     OpenStruct with user objects (OpenStruct|nil return) still reads the
     member when a user class happens to define the same name (#3264). */
  /* An OpenStruct answers ANY reader with a member; but a name the poly
     dispatch already serves with a real builtin arm must keep that arm, or
     the member fetch replaces it and returns nil (#3341). */
  int is_ostruct = argc == 0 && nt_ref(nt, id, "block") < 0 &&
                   !is_lengthlike && !is_empty && !is_pred &&
                   !poly_builtin_zero_arg_name(name) &&
                   sp_feature_required("ostruct");
  /* `rewind` on a poly stream (a param unioning StringIO and IO, #3257):
     both are builtins/native classes with no user arm, so without this
     pre-arm the call was silently dropped. */
  int is_io_rewind = sp_streq(name, "rewind") && !recv_user_defines(c, name);
  /* to_a on a poly value that is really a builtin hash/array (a yield-result
     union of an rbs-seeded Hash and a class instance, #3278): the user-class
     switch has no builtin arm, so the hash fell through to the nil seed. */
  int is_poly_to_a = sp_streq(name, "to_a") &&
                     (comp_ntype(c, id) == TY_POLY_ARRAY || comp_ntype(c, id) == TY_POLY);
  /* to_h on a poly value that is really a builtin hash (or an Array of
     pairs, or a Struct): the user-class switch carries an arm per class that
     defines to_h and none for the builtin, so a plain Hash reached the
     default and raised -- naming Hash, the class whose method it is. Every
     sibling (to_a, to_s, keys, length) already had its arm (#4170). */
  int is_poly_to_h = sp_streq(name, "to_h") && argc == 0 &&
                     nt_ref(nt, id, "block") < 0 &&
                     comp_ntype(c, id) == TY_POLY;
  int ncand = 0, ncall_arm = 0;
  /* a class neither defining nor reading the name, and not native, counts
     for neither: the name's memoized candidates are the classes to ask */
  int npc0 = 0;
  const PolyCand *pc0 = comp_poly_candidates(c, name, &npc0);
  for (int ki = 0; ki < npc0; ki++) {
    int k = pc0[ki].cls;
    /* comp_poly_arm_defines: a native class counts only through its
       declared bindings (#4504) -- its Ruby-side defs get no arm in the
       BLOCKLESS switch below. A block-carrying call is different: the
       block dispatch reaches Ruby-side defs through their proc form, and
       poly_block_call_needs_dispatch stands the element-loop emitters
       down on their account -- so the claim here has to keep counting
       them, or `arr.each { }` beside a loaded StringIO had no emitter at
       all and became the terminal raise. */
    int is_call = comp_poly_arm_defines(c, k, name) ||
                  (nt_ref(nt, id, "block") >= 0 && c->classes[k].is_native_class &&
                   comp_method_in_chain(c, k, name, NULL) >= 0);
    if (is_call || (!c->classes[k].is_native_class && comp_reader_in_chain(c, k, name, NULL))) ncand++;
    /* The root decision counts only the arms the switch below will carry:
       a class no reachable code constructs gets no arm, so it must not
       decide the root either. A dead FFI wrapper's Vector2 counted as a
       calling arm, and every `x` read of a Struct field paid a root push
       and pop for an arm that could not run (#4460). ncand keeps every
       candidate, as the choice to emit a dispatch at all always has. */
    if (is_call && (c->classes[k].instantiated || class_is_prim_reopen(c, k))) ncall_arm++;
  }
  s->lengthlike = is_lengthlike;
  s->empty = is_empty;
  s->class_named = is_class_named;
  s->class_reflect = is_class_reflect;
  s->cls_members = is_cls_members;
  s->pred = is_pred;
  s->ostruct = is_ostruct;
  s->io_rewind = is_io_rewind;
  s->poly_to_a = is_poly_to_a;
  s->poly_to_h = is_poly_to_h;
  s->ncand = ncand;
  s->ncall_arm = ncall_arm;
}

/* The Object reopening's method of the name, as a zero-argument poly
   dispatch's `default:` arm (any receiver no class arm took): through its
   proc form with the call's block, plainly, or an arity refusal. 1 when the
   arm was written. *blk_tmp0 is the hoisted block, made here when the proc
   form needs it. */
int emit_poly_obj_default0(Compiler *c, int id, const char *name, int argc, TyKind ret, int tv, int tr,
                           int *blk_tmp0, Buf *b) {
  const NodeTable *nt = c->nt;
  int done = 0;
  { int obj_cls = comp_class_index(c, "Object");
    if (obj_cls >= 0) {
      int obj_def = -1;
      int obj_mi = comp_method_in_chain(c, obj_cls, name, &obj_def);
      /* A yielding one is reached with the call's block through its proc
         form: a user class's chain does not name Object, so without this
         arm an instance of it took the raise (#5101) */
      int obj_pf = (obj_mi >= 0 && obj_def == obj_cls && argc == 0 &&
                    c->scopes[obj_mi].yields && nt_ref(nt, id, "block") >= 0)
                   ? scope_proc_form_of(c, obj_mi) : -1;
      if (obj_pf >= 0 && (c->scopes[obj_pf].nparams != 0 || c->scopes[obj_pf].rest_idx >= 0))
        obj_pf = -1;
      if (obj_pf >= 0 && (*blk_tmp0) < 0) {
        int cblk3 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
        if (cblk3 < 0) obj_pf = -1;
        else (*blk_tmp0) = hoist_block_proc(c, cblk3);
      }
      if (obj_pf >= 0) {
        Buf oc; memset(&oc, 0, sizeof oc);
        emit_method_cname(c, &c->scopes[obj_pf], &oc);
        buf_printf(&oc, "(_t%d, _t%d)", tv, (*blk_tmp0));
        TyKind pr = (TyKind)c->scopes[obj_pf].ret;
        buf_puts(b, " default: ");
        int pconv = PC_SAME;
        if (method_is_void(&c->scopes[obj_pf])) { buf_puts(b, oc.p); pconv = PC_VOID; }
        else {
          buf_printf(b, "_t%d = ", tr);
          if (ret == TY_POLY && pr != TY_POLY) { emit_boxed_text(c, pr, oc.p, b); pconv = PC_BOX; }
          else if (ret != TY_POLY && pr == TY_POLY) {
            emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, oc.p, b);
            pconv = PC_UNBOX;
          }
          else buf_puts(b, oc.p);
        }
        buf_puts(b, "; break;");
        free(oc.p);
        done = 1;
        if (g_plan_check) pa_observe(PA_PROC_FORM, PA_KEY_DEFAULT, obj_mi, pr, pconv);
      }
      else if (obj_mi >= 0 && obj_def == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
          scope_has_callable_symbol(c, obj_mi)) {
        /* an optional parameter takes its default, spelled with the
           boxed receiver as self */
        Buf ob; memset(&ob, 0, sizeof ob);
        buf_printf(&ob, "sp_Object_%s(_t%d", mc(c->scopes[obj_mi].name), tv);
        { const char *saved_self = g_self;
          char oselfbuf[32]; snprintf(oselfbuf, sizeof oselfbuf, "_t%d", tv);
          g_self = oselfbuf;
          for (int a = 0; a < c->scopes[obj_mi].nparams; a++) {
            buf_puts(&ob, ", "); emit_arg_or_default(c, &c->scopes[obj_mi], a, -1, &ob);
          }
          g_self = saved_self; }
        emit_trailing_blk_arg(c, &c->scopes[obj_mi], id, (*blk_tmp0), &ob);
        buf_puts(&ob, ")");
        const char *ocall = ob.p;
        buf_puts(b, " default: ");
        int pconv = PC_SAME;
        if (method_is_void(&c->scopes[obj_mi])) { buf_puts(b, ocall); pconv = PC_VOID; }
        else {
          TyKind oslot = is_scalar_ret(ret) ? ret : TY_INT;
          buf_printf(b, "_t%d = ", tr);
          if (ret == TY_POLY && c->scopes[obj_mi].ret != TY_POLY) {
            emit_boxed_text(c, c->scopes[obj_mi].ret, ocall, b);
            pconv = PC_BOX;
          }
          else if (ret != TY_POLY && c->scopes[obj_mi].ret == TY_POLY) {
            emit_unbox_text(c, oslot, ocall, b);
            pconv = PC_UNBOX;
          }
          else buf_puts(b, ocall);
        }
        buf_puts(b, "; break;");
        free(ob.p);
        done = 1;
        if (g_plan_check) pa_observe(PA_USER, PA_KEY_DEFAULT, obj_mi, c->scopes[obj_mi].ret, pconv);
      }
      /* ... and one that needs arguments refuses them for any receiver */
      else { char oexp[600];
        if (obj_def == obj_cls && poly_arm_refuses_none(c, obj_mi, oexp, sizeof oexp)) {
          buf_puts(b, " default: "); emit_poly_arity_raise(b, oexp); buf_puts(b, " break;");
          done = 1;
          if (g_plan_check) pa_observe(PA_ARITY, PA_KEY_DEFAULT, obj_mi, TY_UNKNOWN, PC_VOID);
        } }
    } }
  return done;
}

/* Does class 0 take a `case 0:` arm in a poly dispatch of name with argc
   arguments (kwh, pos_argc, splat_a as poly_arm_count reads them)? The
   dispatch key then keeps a non-object value off it. */
int poly_key_cls0(Compiler *c, const char *name, int argc, int kwh, int pos_argc, int splat_a) {
  if (argc == 0) {
    int cls0_d = -1, cls0_rd = -1;
    int cls0_mi = c->nclasses > 0 ? comp_method_in_chain(c, 0, name, &cls0_d) : -1;
    char cls0_exp[600];
    return ((cls0_mi >= 0 && c->scopes[cls0_mi].nrequired == 0) ||
            /* an arm refusing no arguments raises, and is a `case 0:` all the same */
            poly_arm_refuses_none(c, cls0_mi, cls0_exp, sizeof cls0_exp) ||
            (c->nclasses > 0 && comp_reader_in_chain(c, 0, name, &cls0_rd))) &&
           c->nclasses > 0 &&
           (c->classes[0].instantiated || class_is_prim_reopen(c, 0));
  }
  int cls0_mi2 = c->nclasses > 0 ? comp_method_in_chain(c, 0, name, NULL) : -1;
  int cls0_cand2 = cls0_mi2 >= 0 &&
                   (c->classes[0].instantiated || class_is_prim_reopen(c, 0));
  if (cls0_cand2) {
    /* the same widened arity as the candidate count: a keyword hash
       funds the declared keyword params it names (#4205) */
    /* an arm refusing the count raises, and is a `case 0:` all the same */
    char exp0[600];
    cls0_cand2 = poly_arm_count(c, &c->scopes[cls0_mi2], kwh, pos_argc, splat_a,
                                exp0, sizeof exp0) != 0;
  }
  return cls0_cand2;
}

/* Does a primitive reopening (String, Integer, ...) take an arm in that
   dispatch? The key then maps a runtime tag to its class (#4219). */
int poly_key_prim(Compiler *c, const char *name, int argc, int kwh, int pos_argc, int splat_a) {
  /* a reopening without the method takes no arm: the name's candidates */
  int npc = 0;
  const PolyCand *pcs = comp_poly_candidates(c, name, &npc);
  for (int ki = 0; ki < npc; ki++) {
    int k = pcs[ki].cls;
    if (!class_is_prim_reopen(c, k)) continue;
    int pmi = comp_method_in_chain(c, k, name, NULL);
    char pexp[600];
    if (pmi < 0) continue;
    int fits = argc == 0 ? c->scopes[pmi].nrequired == 0 || poly_arm_refuses_none(c, pmi, pexp, sizeof pexp)
                         : poly_arm_count(c, &c->scopes[pmi], kwh, pos_argc, splat_a, pexp, sizeof pexp) != 0;
    if (fits && (scope_has_callable_symbol(c, pmi) || scope_needs_proc_form(c, pmi))) return 1;
  }
  return 0;
}

/* The tag pre-arms of a zero-argument poly dispatch: the if-chain ahead of
   its cls_id switch, for a builtin value (a String, a Symbol, a class, an
   IO, a container) whose method shares its name with a user class's. Each
   writes `if (<tag test>) <result>; else `. */
void emit_poly_prearms0(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                        int tv, int tr, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  int is_lengthlike = ps->lengthlike, is_empty = ps->empty, is_class_named = ps->class_named;
  int is_class_reflect = ps->class_reflect, is_ostruct = ps->ostruct, is_poly_to_a = ps->poly_to_a;
  int is_io_rewind = ps->io_rewind;
  /* When the dispatch result feeds a poly context, tr is sp_RbVal, so the
     length-like answer is boxed */
  const char *bopen = (ret == TY_POLY) ? "sp_box_int(" : "";
  const char *bclose = (ret == TY_POLY) ? ")" : "";
  const char *ebopen = (ret == TY_POLY) ? "sp_box_bool(" : "";
  const char *ebclose = (ret == TY_POLY) ? ")" : "";
  /* string/symbol-tagged poly values answer length/size directly */
  if (is_lengthlike) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_LEN, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %ssp_str_length(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ", tv, tr, bopen, tv, bclose);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %s(sp_int)sp_str_length(_t%d.v.s)%s; else ", tv, tr, bopen, tv, bclose);
    /* A handle answers File#size through the runtime's own dispatch,
       which knows whether it is a File (fstat) or an IO (CRuby's
       NoMethodError). This chain is built when a user class owns the
       name too, and its default arm raised for the File the same
       program keeps beside those objects in one Hash (#4734). */
    if (sp_streq(name, "size"))
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) _t%d = %ssp_poly_size(_t%d)%s; else ",
                 tv, tv, tr, bopen, tv, bclose);
  }
  /* a string/symbol-tagged poly value answers empty? directly (#1438) */
  if (is_empty) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_EMPTY, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %ssp_str_length(_t%d.v.s) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
    buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %sstrlen(sp_sym_to_s((sp_sym)_t%d.v.i)) == 0%s; else ", tv, tr, ebopen, tv, ebclose);
  }
  /* a class-tagged poly value answers its name: `Base.subclasses` and
     `#ancestors` hand back boxed classes, so `.map { |c| c.name }` reaches
     here (#2656). The tag is checked ahead of the cls_id switch, because a
     boxed class carries the CLASS's id and would otherwise alias that
     user class's arm. Declined when a user class defines the method --
     then a user object is the likelier receiver and it must win. */
  if (is_class_named) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CLASS_NAMED, -1, TY_UNKNOWN, PC_SAME);
    const char *sbopen = (ret == TY_POLY) ? "sp_box_str(" : "";
    const char *sbclose = (ret == TY_POLY) ? ")" : "";
    /* a boxed class's #name is the interned frozen String the typed
       forms answer (sp_str_frozen_name, which emit_call wraps around
       those), as the Encoding and Symbol arms below intern theirs; its
       to_s and inspect are not frozen */
    int fzn = sp_streq(name, "name");
    buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %s%ssp_class_val_name(_t%d)%s%s; else ",
               tv, tr, sbopen, fzn ? "sp_str_uminus_val(" : "", tv, fzn ? ")" : "", sbclose);
    /* `name` on an Encoding (always carried boxed) and on a Symbol: a
       frozen String, as CRuby answers and as the typed Symbol#name does */
    if (sp_streq(name, "name"))
      buf_printf(b, "if (_t%d.tag == SP_TAG_ENCODING) _t%d = %ssp_str_uminus_val(_t%d.v.s)%s; "
                    "else if (_t%d.tag == SP_TAG_SYM) "
                    "_t%d = %ssp_str_uminus_val(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ",
                 tv, tr, sbopen, tv, sbclose, tv, tr, sbopen, tv, sbclose);
  }
  if (is_class_reflect) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CLASS_REFLECT, -1, TY_UNKNOWN, PC_SAME);
    /* sp_class_superclass only knows the user chain; a builtin class needs
       sp_builtin_superclass, exactly as the typed arm does. allocate and
       keyword_init? answer a boxed value already. */
    int boxed_ans = sp_streq(name, "allocate") || sp_streq(name, "keyword_init?");
    const char *cbo = (ret == TY_POLY && !boxed_ans)
                        ? (sp_streq(name, "superclass") ? "sp_box_class(" : "sp_box_poly_array(")
                        : "";
    const char *cbc = (ret == TY_POLY && !boxed_ans) ? ")" : "";
    buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %s", tv, tr, cbo);
    if (sp_streq(name, "ancestors"))
      buf_printf(b, "sp_class_ancestors(sp_unbox_class(_t%d))", tv);
    else if (sp_streq(name, "included_modules"))
      buf_printf(b, "sp_class_included_modules(sp_unbox_class(_t%d))", tv);
    else if (sp_streq(name, "subclasses"))
      buf_printf(b, "sp_cls_subclasses(_t%d)", tv);
    else if (sp_streq(name, "allocate"))
      buf_printf(b, "sp_cls_allocate(_t%d)", tv);
    else if (sp_streq(name, "keyword_init?"))
      buf_printf(b, "sp_cls_keyword_init_p(_t%d)", tv);
    else
      buf_printf(b, "({ sp_Class _cs%d = sp_unbox_class(_t%d); _cs%d.cls_id >= 0 ? sp_class_superclass(_cs%d) : sp_builtin_superclass(_cs%d); })",
                 tv, tv, tv, tv, tv);
    buf_printf(b, "%s; else ", cbc);
  }
  /* an OpenStruct answers ANY reader with its member value; checked ahead of
     the cls_id switch since its id is a builtin, not a user class (#3197). */
  if (is_ostruct) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_OSTRUCT, -1, TY_UNKNOWN, PC_SAME);
    char osget[192];
    snprintf(osget, sizeof osget,
             "sp_OpenStruct_get((sp_OpenStruct *)_t%d.v.p, sp_sym_intern(\"%s\"))", tv, name);
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_OPENSTRUCT) _t%d = ",
               tv, tv, tr);
    if (ret == TY_POLY) buf_puts(b, osget);
    else emit_unbox_text(c, ret, osget, b);   /* result slot is user-typed (#3264) */
    buf_puts(b, "; else ");
  }
  /* to_a on a runtime builtin hash/array: pair-array via the boxed
     converter (#3278) */
  if (is_poly_to_a) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_TO_A, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && (sp_poly_is_hash_kind(_t%d.cls_id)"
                  " || sp_poly_is_array_kind(_t%d.cls_id)"
                  " || _t%d.cls_id == SP_BUILTIN_RANGE"
                  " || _t%d.cls_id == SP_BUILTIN_STR_RANGE)) { _t%d = ",
               tv, tv, tv, tv, tv, tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_poly_array(sp_poly_to_a_arr(_t%d))", tv);
    else buf_printf(b, "sp_poly_to_a_arr(_t%d)", tv);
    buf_puts(b, "; }\nelse ");
    /* an Enumerator materializes through its own reader (#3624) */
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_ENUMERATOR) { _t%d = ",
               tv, tv, tr);
    if (ret == TY_POLY)
      buf_printf(b, "sp_box_poly_array(sp_Enumerator_to_a((sp_Enumerator *)_t%d.v.p))", tv);
    else buf_printf(b, "sp_Enumerator_to_a((sp_Enumerator *)_t%d.v.p)", tv);
    buf_puts(b, "; }\nelse ");
  }
  /* rewind on a runtime IO / StringIO stream (#3257); value is the seed
     (rewind's return is rarely consumed through a poly union) */
  if (is_io_rewind) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_REWIND, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO)"
                  " { sp_File_rewind((sp_File *)_t%d.v.p); }\nelse ", tv, tv, tv);
    int sio_cid3 = comp_class_index(c, "StringIO");
    if (sio_cid3 >= 0)
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d)"
                    " { sp_StringIO_rewind((sp_StringIO *)_t%d.v.p); }\nelse ",
                 tv, tv, sio_cid3, tv);
  }
  /* A zero-arg IO method whose name a user class ALSO owns. The cls_id
     switch below carries an arm per user class only, so an `@io` that
     holds a Socket here and a plain object there left the real stream at
     the NoMethodError default (#4341): `def close; @io.close; end` on a
     wrapper reported `close` as undefined for the Socket. Guarded on
     SP_BUILTIN_IO, which no user-class arm can alias, so an object still
     takes its own arm -- the same shape the rewind pre-arm above uses.
     `close` leaves the seed alone: nil is what it answers. */
  if (argc == 0 && nt_ref(nt, id, "block") < 0) {
    /* the readers answer what the typed receiver's arms answer: gets and
       getc a nil-able String (NULL boxes to nil), getbyte an Integer or
       the nil sentinel, readline/readchar/readbyte raise EOFError */
    static const struct { const char *nm, *fn; TyKind rt; const char *tail; } IOZ[] = {
      {"close",   "sp_File_close",    TY_VOID},
      {"closed?", "sp_File_closed_p", TY_BOOL},
      {"eof?",    "sp_File_eof_p",    TY_BOOL},
      {"eof",     "sp_File_eof_p",    TY_BOOL},
      {"tty?",    "sp_File_tty_p",    TY_BOOL},
      {"isatty",  "sp_File_tty_p",    TY_BOOL},
      {"flush",   "sp_File_flush",    TY_VOID},
      {"fileno",  "sp_File_fileno",   TY_INT},
      {"tell",    "sp_File_tell",     TY_INT},
      {"pos",     "sp_File_tell",     TY_INT},
      {"lineno",  "sp_File_lineno",   TY_INT},
      {"sync",    "sp_File_sync_p",   TY_BOOL},
      {"gets",    "sp_File_gets",     TY_STRING},
      {"getc",    "sp_File_getc",     TY_STRING},
      {"readchar", "sp_File_readchar", TY_STRING},
      {"readline", "sp_File_readline_sep", TY_STRING, ", \"\\n\", 0, 0"},
      {"readbyte", "sp_File_readbyte", TY_INT},
      {"getbyte", "sp_File_getbyte",  TY_INT},
      {"readlines", "sp_File_readlines", TY_STR_ARRAY},
      {NULL, NULL, TY_VOID, NULL}
    };
    /* a bare puts writes the newline and answers nil (#6158) */
    if (sp_streq(name, "puts")) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_PUTS, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) { "
                    "sp_File_puts((sp_File *)_t%d.v.p, \"\"); ", tv, tv, tv);
      if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_nil(); ", tr);
      buf_puts(b, "}\nelse ");
    }
    for (int i = 0; IOZ[i].nm; i++) {
      if (!sp_streq(name, IOZ[i].nm)) continue;
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IOZ, -1, TY_UNKNOWN, PC_SAME);
      char ioex[128];
      snprintf(ioex, sizeof ioex, "%s((sp_File *)_t%d.v.p%s)", IOZ[i].fn, tv,
               IOZ[i].tail ? IOZ[i].tail : "");
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) { ",
                 tv, tv);
      /* the value only lands when the result slot can hold it: poly boxes
         it, an exactly matching concrete slot takes it raw, anything else
         keeps the call for its effect and leaves the seed */
      if (ret == TY_POLY && sp_streq(name, "getbyte"))
        buf_printf(b, "_t%d = sp_box_int_or_nil(%s)", tr, ioex);
      else if (ret == TY_POLY && IOZ[i].rt != TY_VOID) {
        buf_printf(b, "_t%d = ", tr);
        emit_boxed_text(c, IOZ[i].rt, ioex, b);
      }
      else if (ret == IOZ[i].rt && IOZ[i].rt != TY_VOID)
        buf_printf(b, "_t%d = %s", tr, ioex);
      else buf_puts(b, ioex);
      buf_puts(b, "; }\nelse ");
      break;
    }
  }
  /* A zero-arg CONTAINER reduction whose name a user class also owns
     (`TreeNode#sum` next to a real Array's). The switch below covers
     SP_TAG_OBJ user classes only, so an Array receiver fell through to the
     NoMethodError default. Runtime-guarded on the container kinds, so a
     user object still takes its own arm. */
  if (argc == 0 && nt_ref(nt, id, "block") < 0) {
    const char *cfn = sp_streq(name, "sum")   ? "sp_poly_sum"
                    : sp_streq(name, "min")   ? "sp_poly_min"
                    : sp_streq(name, "max")   ? "sp_poly_max"
                    : sp_streq(name, "first") ? "sp_poly_first"
                    : sp_streq(name, "last")  ? "sp_poly_last" : NULL;
    if (cfn) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_REDUCE, -1, TY_UNKNOWN, PC_SAME);
      char cex[96];
      snprintf(cex, sizeof cex, "%s(_t%d)", cfn, tv);
      /* Time#min is the MINUTE, and sp_poly_min answers it. A boxed Time
         is neither an array nor a hash kind, so without this arm it fell
         past the guard into the user-class switch and raised
         NoMethodError -- the second symptom of #4192, reached only when
         some user class happens to define `min`. A boxed Range is in the
         same position for all five names: the helpers own it (Range#sum
         always did; min/max/first/last since #4192's follow-up), so it
         must not fall into the user switch either. */
      char tg[128];
      int tn = snprintf(tg, sizeof tg, " || _t%d.cls_id == SP_BUILTIN_RANGE", tv);
      if (sp_streq(name, "min"))
        snprintf(tg + tn, sizeof tg - (size_t)tn,
                 " || _t%d.cls_id == SP_BUILTIN_TIME", tv);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && (sp_poly_is_array_kind(_t%d.cls_id) ||"
                    " sp_poly_is_hash_kind(_t%d.cls_id)%s)) { _t%d = ", tv, tv, tv, tg, tr);
      if (ret == TY_POLY) buf_puts(b, cex);
      else emit_unbox_text(c, ret, cex, b);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* A zero-arg String transform whose name a user class ALSO owns. The
     poly String shortcuts decline to this dispatch so a Struct member or
     attr_reader called `upcase` answers the member rather than the upcased
     #inspect of the object holding it (#3364, #3380) -- but the cls_id
     switch below only covers SP_TAG_OBJ, so a genuine String receiver then
     fell through to the seed (nil, or 0 for #bytes). Same tag pre-arm the
     `[]` and #include? cases above use: String at run time takes the
     String method, an object takes its member. */
  /* ... unless the program REOPENED String with that very name, in which
     case the String arm below (case 0 / the reopen's own method) is the
     answer and this shortcut would take it away: `class String; def
     upcase; "nope"; end` has to reach "nope" for a run-time-typed
     receiver too, the way it now does for a concrete one. */
  int str_reopen_owns = 0;
  { int sci = comp_class_index(c, "String");
    if (sci >= 0 && comp_method_in_chain(c, sci, name, NULL) >= 0) str_reopen_owns = 1; }
  if (argc == 0 && !str_reopen_owns) {
    static const struct { const char *nm, *fn; int arr; } STRT[] = {
      {"upcase","sp_str_upcase",0}, {"downcase","sp_str_downcase",0},
      {"capitalize","sp_str_capitalize",0}, {"swapcase","sp_str_swapcase",0},
      {"strip","sp_str_strip",0}, {"reverse","sp_str_reverse",0},
      {"chomp","sp_str_chomp",0}, {"chop","sp_str_chop",0},
      {"succ","sp_str_succ",0}, {"next","sp_str_succ",0},
      {"chr","sp_str_chr",0},
      {"bytes","sp_str_bytes",1}, {"chars","sp_str_chars",2}, {NULL,NULL,0}
    };
    for (int si = 0; STRT[si].nm; si++) {
      if (!sp_streq(name, STRT[si].nm)) continue;
      /* the result has to fit the slot the dispatch assigns into */
      int ok = STRT[si].arr == 0 ? (ret == TY_POLY || ret == TY_STRING)
             : STRT[si].arr == 1 ? (ret == TY_POLY || ret == TY_INT_ARRAY)
             :                     (ret == TY_POLY || ret == TY_STR_ARRAY);
      if (!ok) break;
      /* #chr is Integer#chr on an int tag: stringifying first turns
         65.chr into "65".chr == "6" (#3328). */
      if (sp_streq(name, "chr")) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INT_CHR, -1, TY_UNKNOWN, PC_SAME);
        buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = ", tv, tr);
        if (ret == TY_STRING) buf_printf(b, "sp_int_chr(_t%d.v.i)", tv);
        else buf_printf(b, "sp_box_str(sp_int_chr(_t%d.v.i))", tv);
        buf_puts(b, "; }\nelse ");
      }
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STRT, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
      if (ret != TY_POLY) buf_printf(b, "%s(_t%d.v.s)", STRT[si].fn, tv);
      else if (STRT[si].arr == 1) buf_printf(b, "sp_box_int_array(%s(_t%d.v.s))", STRT[si].fn, tv);
      else if (STRT[si].arr == 2) buf_printf(b, "sp_box_str_array(%s(_t%d.v.s))", STRT[si].fn, tv);
      else buf_printf(b, "sp_box_str(%s(_t%d.v.s))", STRT[si].fn, tv);
      buf_puts(b, "; }\nelse ");
      break;
    }
    /* #split is the same shape but answers an ARRAY, so it needs the slot
       conversion the table above cannot express: a user class owning
       `split` (Pathname does) turned `str.split.join(" ")` into a switch
       with no String arm, and the NULL that fell out joined to "" (#3394). */
    if (sp_streq(name, "split") &&
        (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY)) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_SPLIT, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
      if (ret == TY_STR_ARRAY) buf_printf(b, "sp_str_split_ws(_t%d.v.s)", tv);
      else if (ret == TY_POLY_ARRAY) buf_printf(b, "sp_StrArray_to_poly_fmt(sp_str_split_ws(_t%d.v.s))", tv);
      else buf_printf(b, "sp_box_str_array(sp_str_split_ws(_t%d.v.s))", tv);
      buf_puts(b, "; }\nelse ");
    }
  }
}

/* The rest of a zero-argument poly dispatch's pre-arms, after the tag
   chain (emit_poly_prearms0): the block a candidate takes, hoisted once as
   a proc (its temp is the result), a builtin container or Mutex driving
   that proc, a callable value, a class value's class-side arms. */
int emit_poly_prearms0_blk(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                           int tv, int tr, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  /* class 0 emits a `case 0:` arm here when it defines/inherits the method
     (nrequired 0) or exposes it as a reader; the dispatch key is then guarded
     so a boxed scalar (cls_id 0) does not alias it (issue #1576). */
  /* A candidate whose method takes `&blk` needs the call's block passed
     to it. Materialize the proc ONCE, ahead of the switch, and hand the
     same temp to every arm -- only one arm runs, and building it per arm
     would allocate a proc per candidate class (#3399). Mirrors the
     class-method cascade, which already does this. */
  int blk_tmp0 = -1;
  { int cblk0 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
    if (cblk0 >= 0) {
      int npc0 = 0;
      const PolyCand *pc0 = comp_poly_candidates(c, name, &npc0);   /* (#4966) */
      for (int ki = 0; ki < npc0 && blk_tmp0 < 0; ki++) {
        int k = pc0[ki].cls;
        if (!c->classes[k].instantiated) continue;
        int mi0 = pc0[ki].mi;
        if (mi0 < 0) continue;
        Scope *cm0 = &c->scopes[mi0];
        /* a yielding candidate is reachable through its proc form */
        if (!scope_has_callable_symbol(c, mi0) && !scope_needs_proc_form(c, mi0)) continue;
        if ((cm0->blk_param && cm0->blk_param[0] && !cm0->yields) ||
            scope_needs_proc_form(c, mi0)) {
          /* `&blk` that survived the forwarding resolution names a REAL
             proc (this function's own block param), not a literal to
             materialize: write the proc expression itself. */
          blk_tmp0 = hoist_block_proc(c, cblk0);
        }
      }
    } }
  /* A builtin Array receiver reaching a dispatch that exists only because
     a USER class defines this name. Without an arm it falls to the raise:
     `NoMethodError: undefined method 'map' for an instance of Array` at a
     site where the block-carrying call is plainly Array#map. The builtin
     is normally served by splicing the block inline, which is not
     available here -- the block was materialized once as a proc and shared
     by every arm, and a second spliced copy would disagree with whichever
     arm ran. Drive the same proc over the elements instead (#3409).

     Only reachable at all since a yielding method became dispatchable: a
     non-yielding user `map` leaves a block-carrying call to the builtin
     path entirely, which is why the same shape is correct without the
     yield. */
  { const char *pen_op = argc == 0 && nt_ref(nt, id, "block") >= 0
                       ? poly_enum_op_for(name) : NULL;
    /* A candidate that neither yields nor keeps a real &blk left no proc
       materialized -- it ignores the block. The builtin arm still needs
       one, so build it here; only one arm runs either way. */
    if (pen_op && blk_tmp0 < 0) {
      int cblk1 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      if (cblk1 < 0) pen_op = NULL;
      else blk_tmp0 = hoist_block_proc(c, cblk1);
    }
    if (pen_op) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ENUM_PROC, -1, TY_UNKNOWN, PC_SAME);
      char pcall[160];
      snprintf(pcall, sizeof pcall, "sp_poly_enum_proc(_t%d, %s, _t%d)", tv, pen_op, blk_tmp0);
      /* an Integer Range walks through the same helper (its length and
         members are known to it); without it a boxed Range fell to the
         user-class switch's NoMethodError (#4840) */
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && (sp_poly_is_array_kind(_t%d.cls_id) || sp_poly_is_hash_kind(_t%d.cls_id) || _t%d.cls_id == SP_BUILTIN_RANGE || _t%d.cls_id == SP_BUILTIN_ENUMERATOR)) { _t%d = ", tv, tv, tv, tv, tv, tr);
      if (ret == TY_POLY) buf_puts(b, pcall);
      else emit_unbox_text(c, ret, pcall, b);
      buf_puts(b, "; }\nelse ");
    } }
  /* A boxed Mutex reaching a dispatch that exists because a user class
     also defines `synchronize`: without an arm the Mutex fell to the
     raise. The static arm (the lock/ensure shape in the synchronize
     emitter) cannot serve it here, the block being a materialized proc
     shared by every arm, so the runtime arm locks around the proc. */
  if (sp_streq(name, "synchronize") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    if (blk_tmp0 < 0) {
      int cblk2 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      if (cblk2 >= 0) blk_tmp0 = hoist_block_proc(c, cblk2);
    }
    if (blk_tmp0 >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_SYNC, -1, TY_UNKNOWN, PC_SAME);
      char mcall[96];
      snprintf(mcall, sizeof mcall, "sp_Mutex_synchronize_proc((sp_mutex *)_t%d.v.p, _t%d)", tv, blk_tmp0);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_MUTEX) { _t%d = ", tv, tv, tr);
      if (ret == TY_POLY) buf_puts(b, mcall);
      else emit_unbox_text(c, ret, mcall, b);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* a boxed Proc/Curry/Method in a slot a user `call`/`[]` shadows (#4395) */
  if (emit_poly_callable_prearm(c, name, 0, NULL, NULL, NULL, tv, tr, ret, 0, b) && g_plan_check)
    pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CALLABLE, -1, TY_UNKNOWN, PC_SAME);
  /* a class-valued receiver dispatches class-side, ahead of the instance
     arms (#4218) */
  if (!emit_poly_cls_value_prearm(c, id, name, 0, NULL, NULL, NULL, NULL, tv, tr, ret, blk_tmp0, b) &&
      ps->cls_members) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CLS_MEMBERS, -1, TY_UNKNOWN, PC_SAME);
    if (ret == TY_POLY || ret == TY_POLY_ARRAY)
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %ssp_cls_members(_t%d)%s; else ",
                 tv, tr, ret == TY_POLY ? "sp_box_poly_array(" : "", tv, ret == TY_POLY ? ")" : "");
    else
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) sp_raise_nomethod(sp_nomethod_msg(\"members\", _t%d)); else ",
                 tv, tv);
  }
  return blk_tmp0;
}

/* The builtin `case` arms a zero-argument poly dispatch writes after its
   class arms: a container's length, clear and empty?, a container read
   re-entered as the builtin it is, a Hash's compare_by_identity?. */
void emit_poly_cases0(Compiler *c, int id, int recv, const char *name, const PolySpecials0 *ps,
                      TyKind ret, int tv, int tr, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = 0, is_empty = ps->empty;
  const char *bopen = (ret == TY_POLY) ? "sp_box_int(" : "";
  const char *bclose = (ret == TY_POLY) ? ")" : "";
  const char *ebopen = (ret == TY_POLY) ? "sp_box_bool(" : "";
  const char *ebclose = (ret == TY_POLY) ? ")" : "";
  /* built-in array receivers reaching a length-like poly dispatch */
  if (is_count_alias(name)) {
    emit_builtin_len_cases(b, tr, tv, bopen, bclose);
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_LEN_CASES, -1, TY_UNKNOWN, PC_SAME);
  }
  /* built-in container receivers reaching a poly clear dispatch (a seeded
     boxed ivar array): empty in place through the runtime kind dispatch;
     without these arms the switch missed the cls_id and silently kept
     the contents (#3326). */
  if (sp_streq(name, "clear") && argc == 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CLEAR, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY:"
                  " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                  " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: case SP_BUILTIN_STRBUF:"
                  " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:"
                  " case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_INT_INT_HASH:"
                  " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH:"
                  " case SP_BUILTIN_POLY_POLY_HASH:");
    if (ret == TY_POLY)
      buf_printf(b, " _t%d = sp_poly_clear(_t%d); break;", tr, tv);
    else
      buf_printf(b, " sp_poly_clear(_t%d); break;", tv);
  }
  /* built-in array / hash receivers reaching a poly empty? dispatch (#1438) */
  if (is_empty) {
    emit_builtin_len_cases(b, tr, tv, ebopen, ret == TY_POLY ? " == 0)" : " == 0");
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_EMPTY_CASES, -1, TY_UNKNOWN, PC_SAME);
  }
  /* Container reads on a builtin receiver that reached this dispatch only
     because a user class happens to own the name. The user arms are above;
     without an arm of its own the switch left every builtin tag on the
     raise default, so `hash.keys` / `array.first` on a genuine Hash or
     Array raised NoMethodError (#3459).

     Rather than re-implement each read here, re-enter the ordinary call
     emission on the guarded temp with g_poly_builtin_arm set: inside these
     case labels the value IS a container, so the user classes owning the
     name are not candidates and the builtin surface should serve the call
     exactly as it would with no user class in the program. That is the
     invariant -- a user class owning a name must not change what a builtin
     receiver does -- and it holds for every name the surface serves, not a
     list maintained here. Only for a poly result slot: a scalar slot means
     the dispatch was pinned to the user return type (analyze widens the
     ones it can, see poly_container_read_p), and the re-entered emission
     would not fit it. */
  if (argc == 0 && ret == TY_POLY && nt_ref(nt, id, "block") < 0 &&
      poly_container_read_p(name) && g_pd_skip != id &&
      g_n_argov < MAX_ARG_OVERRIDE) {
    int slot9 = view_bind(recv, "_t%d", tv);
    int va = view_push_arm(id, g_prbd_skip, 1);
    /* The builtin surface reads the node's own type to pick its shape, and
       this node was widened to poly to hold both answers. Restore the
       builtin-only type analyze recorded (an array read lowers to a
       pointer, a scalar read to a boxed value) for the duration, then box
       the result back into the dispatch's poly slot. */
    TyKind bt9 = (c->poly_builtin_ty && id < c->node_cap)
                   ? c->poly_builtin_ty[id] : TY_UNKNOWN;
    int vw = bt9 != TY_UNKNOWN ? view_push(c, id, bt9) : -1;
    Buf ib9; memset(&ib9, 0, sizeof ib9);
    if (bt9 != TY_UNKNOWN && bt9 != TY_POLY) {
      Buf nb9; memset(&nb9, 0, sizeof nb9);
      emit_expr(c, id, &nb9);
      emit_boxed_text(c, bt9, nb9.p ? nb9.p : "0", &ib9);
      free(nb9.p);
    }
    else emit_boxed(c, id, &ib9);
    if (vw >= 0) view_pop(c, vw);
    view_pop(c, va);
    view_unbind(g_n_argov - 1);
    /* an emission that fell through to the raise token adds nothing: leave
       those tags on the switch's own default so the message is the same */
    int kept9 = ib9.p && strncmp(ib9.p, "sp_raise_nomethod(", 18) != 0;
    if (g_plan_check) pa_observe(PA_TRIAL, PA_KEY_TRIAL + PT_CONTAINER, -1, TY_UNKNOWN, kept9);
    if (kept9) {
      buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_SYM_ARRAY:"
                  " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                  " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY:"
                  " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH:"
                  " case SP_BUILTIN_INT_STR_HASH: case SP_BUILTIN_INT_INT_HASH:"
                  " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH:"
                  " case SP_BUILTIN_POLY_POLY_HASH:");
      buf_printf(b, " _t%d = %s; break;", tr, ib9.p);
    }
    free(ib9.p);
  }
  /* compare_by_identity? on a poly-carried hash: every spinel hash is
     value-keyed (the mutating variant is a compile error), so any hash
     tag answers false; a non-hash receiver falls through to the gate. */
  if (sp_streq(name, "compare_by_identity?")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CMP_BY_ID, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: case SP_BUILTIN_SYM_POLY_HASH:"
                  " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_STR_STR_HASH:"
                  " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_INT_STR_HASH:"
                  " _t%d = %s0%s; break;", tr, ebopen, ebclose);
  }
}

/* The `default:` arms a zero-argument poly dispatch writes for a builtin
   value when no Object method took the default (obj_default_done), the
   first that applies, then the named builtin cases a user class's name
   would otherwise take from a builtin receiver. Answers obj_default_done. */
int emit_poly_defaults0(Compiler *c, int id, int recv, const char *name, const PolySpecials0 *ps,
                        TyKind ret, int tv, int tr, int obj_default_done, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = 0, is_pred = ps->pred;
  (void)recv;
  /* `x.enum_for` on a boxed value, rewritten to the generator helper the
     classes with a yielding each carry: any other value is enumerated
     as the builtin it is */
  if (!obj_default_done && sp_streq(name, "__to_enum_each") &&
      (ret == TY_POLY || ret == TY_ENUMERATOR)) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_ENUM_EACH, -1, TY_UNKNOWN, PC_SAME);
    char ev[24]; snprintf(ev, sizeof ev, "_t%d", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, "sp_box_obj(");
    emit_poly_enum_for(c, ev, b);
    if (ret == TY_POLY) buf_puts(b, ", SP_BUILTIN_ENUMERATOR)");
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* to_s / inspect are universal: a poly value that is a builtin scalar
     (int, float, string, ...) rather than one of the enumerated user
     classes still answers them. Without a default arm the result stayed
     the empty-string default, so `@x.to_s` on a poly-widened int printed
     blank. Route the fallthrough through the runtime poly converter. */
  if (!obj_default_done && (sp_streq(name, "to_s") || sp_streq(name, "inspect"))) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_TO_S, -1, TY_UNKNOWN, PC_SAME);
    const char *pfn = sp_streq(name, "to_s") ? "sp_poly_to_s" : "sp_poly_inspect";
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(%s(_t%d))", pfn, tv);
    else buf_printf(b, "%s(_t%d)", pfn, tv);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* The case conversions are String's AND Symbol's. A String receiver is
     recognised by a tag pre-arm before the switch, but a Symbol had no arm
     once a user class owned the name, so `:ab.upcase` through a poly slot
     raised NoMethodError. sp_poly_case_conv is the same runtime the
     no-user-class path uses: it converts a Symbol through its name, a
     string as a string, and refuses anything else the way CRuby does. */
  if (!obj_default_done && argc == 0 &&
      (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
       sp_streq(name, "capitalize") || sp_streq(name, "swapcase"))) {
         if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_CASE_CONV, -1, TY_UNKNOWN, PC_SAME);
    char cv[96];
    snprintf(cv, sizeof cv, "sp_poly_case_conv(_t%d, sp_str_%s, \"%s\")", tv, name, name);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, cv);
    else emit_unbox_text(c, ret, cv, b);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* The numeric methods a builtin receiver answers for itself. A class
     defining `abs` (or `round`, `succ`) takes over the name's dispatch,
     and an Integer arriving at it had no arm: `@units.abs` on a poly ivar
     raised "undefined method 'abs' for an instance of Integer" (#4012).
     The runtime helpers are the same ones the no-user-class path uses, and
     they refuse a receiver that is not a number, as CRuby does. */
  if (!obj_default_done && argc == 0 &&
      (sp_streq(name, "abs") || sp_streq(name, "round") ||
       sp_streq(name, "succ") || sp_streq(name, "next") ||
       sp_streq(name, "pred") || sp_streq(name, "ceil") ||
       sp_streq(name, "floor") || sp_streq(name, "truncate") ||
       /* the value-answering numeric queries a user class can shadow the
          same way: abs2, infinite?, numerator and denominator answer
          boxed values. bit_length answers a machine int, and its helper is
          already named to match `sp_poly_%s` below. */
       sp_streq(name, "abs2") || sp_streq(name, "infinite?") ||
       sp_streq(name, "numerator") || sp_streq(name, "denominator") ||
       sp_streq(name, "nonzero?") || sp_streq(name, "bit_length"))) {
         if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_NUM, -1, TY_UNKNOWN, PC_SAME);
    char nv[96];
    int int_valued = sp_streq(name, "bit_length");
    if (sp_streq(name, "succ") || sp_streq(name, "next"))
      snprintf(nv, sizeof nv, "sp_poly_succ_m(_t%d, %d)", tv, sp_streq(name, "next") ? 1 : 0);
    else if (sp_streq(name, "pred"))
      snprintf(nv, sizeof nv, "sp_poly_sub(_t%d, sp_box_int(1))", tv);
    else if (sp_streq(name, "infinite?"))
      snprintf(nv, sizeof nv, "sp_poly_infinite(_t%d)", tv);
    else if (sp_streq(name, "nonzero?"))
      snprintf(nv, sizeof nv, "sp_poly_nonzero(_t%d)", tv);
    else
      snprintf(nv, sizeof nv, "sp_poly_%s(_t%d)", name, tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (int_valued) {
      if (ret == TY_POLY) emit_boxed_text(c, TY_INT, nv, b);
      else buf_puts(b, nv);
    }
    else if (ret == TY_POLY) buf_puts(b, nv);
    else emit_unbox_text(c, ret, nv, b);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* Blockless `digits` (base 10 implicit), the same shape as the block
     above but for a receiver answering an array rather than a scalar:
     `sp_poly_int_digits` is the runtime helper the no-user-class path
     already calls (codegen_call_recv.c), and it raises for a
     non-Integer receiver, as CRuby does. */
  if (!obj_default_done && argc == 0 && sp_streq(name, "digits")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_DIGITS, -1, TY_UNKNOWN, PC_SAME);
    char nv[64]; snprintf(nv, sizeof nv, "sp_poly_int_digits(_t%d, 10)", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_INT_ARRAY, nv, b);
    else buf_puts(b, nv);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* The zero-argument Array transforms. A class defining flatten (or
     compact, uniq) takes over the name's dispatch, and an Array arriving
     at it had no arm: activesupport's Uncountables defines #flatten, and
     its `words.flatten` on a rest array reaching the dispatch as poly
     raised "undefined method 'flatten' for an instance of Array". The
     coercion is the one the no-user-class path uses; a receiver that is
     not an Array keeps the raise (a Hash answers these with its own pair
     semantics and is left to the switch). Only a poly or poly-array slot
     can take the PolyArray result. */
  if (!obj_default_done && argc == 0 &&
      (sp_streq(name, "flatten") || sp_streq(name, "compact") || sp_streq(name, "uniq")) &&
      (ret == TY_POLY || ret == TY_POLY_ARRAY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_ARRAY_TRANSFORM, -1, TY_UNKNOWN, PC_SAME);
    char nv[96];   /* the value-taking runtime helpers the no-user-class path calls */
    snprintf(nv, sizeof nv, "sp_poly_%s(_t%d)", name, tv);
    buf_printf(b, " default: if (sp_rbval_is_array(_t%d)) { _t%d = ", tv, tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_POLY_ARRAY, nv, b);
    else buf_puts(b, nv);
    buf_puts(b, "; break; }");
    /* anything else -- a Hash, with its own pair semantics -- answers as
       it would with no class of that name */
    int kept = emit_poly_builtin_default(c, id, recv, name, 0, NULL, NULL, NULL, ret, tv, tr, 0, b);
    if (g_plan_check) pa_observe(PA_TRIAL, PA_KEY_TRIAL + PT_ARRAY_FALLBACK, -1, TY_UNKNOWN, kept);
    if (!kept)
      buf_printf(b, " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
    obj_default_done = 1;
  }
  /* frozen?/nil? on a builtin-scalar (or un-overridden object) poly value:
     the switch default answers via the runtime predicate. */
  if (!obj_default_done && is_pred) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_PRED, -1, TY_UNKNOWN, PC_SAME);
    char tvref[24]; snprintf(tvref, sizeof tvref, "_t%d", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) { buf_puts(b, "sp_box_bool("); emit_poly_pred_value(c, id, tvref, NULL, b); buf_puts(b, ")"); }
    else emit_poly_pred_value(c, id, tvref, NULL, b);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* Nothing claimed the fallthrough: the value is not one of the enumerated
     classes and no pre-arm recognised its tag, so it does not answer this
     method. Raise, as every other unresolved call does. Leaving the slot at
     its zero handed callers a NULL container that read back as empty --
     `str.split.join(" ")` answered "" once a user class owned `split`
     (#3394), which is the silent form of the same gap. */
  /* A conversion the poly runtime serves for every builtin (`5.to_i`,
     `"7".to_f`) only reached this dispatch because a user class owns the
     name; a builtin receiver still has to get its own answer. */
  if (!obj_default_done && argc == 0 &&
      (sp_streq(name, "to_i") || sp_streq(name, "to_f"))) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_TO_IF, -1, TY_UNKNOWN, PC_SAME);
    char cv[64];
    snprintf(cv, sizeof cv, "%s(_t%d)", sp_streq(name, "to_i") ? "sp_poly_to_i_meth" : "sp_poly_to_f_meth", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, sp_streq(name, "to_i") ? TY_INT : TY_FLOAT, cv, b);
    else buf_puts(b, cv);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* Blockless any? / none?: "is there an element", which every builtin
     container answers. The switch carries an arm per user class owning the
     name and none for a builtin, so a slot that may hold an Array OR such
     an object raised NoMethodError on the Array -- naming Array, whose
     method it is (#4264). In the DEFAULT, like the conversions around it:
     a class that defines the name has its own case and never arrives. */
  if (!obj_default_done && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "any?") || sp_streq(name, "none?"))) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_ANY_NONE, -1, TY_UNKNOWN, PC_SAME);
    int neg = sp_streq(name, "none?");
    char av[96];
    snprintf(av, sizeof av, "(sp_poly_length(_t%d) %s 0)", tv, neg ? "==" : ">");
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_BOOL, av, b);
    else buf_puts(b, av);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* to_h, the same shape: the switch carries an arm per class that defines
     it and none for a builtin, so a plain Hash reached the default and
     raised -- naming Hash, whose method it is. In the DEFAULT, not as a
     pre-arm: a class that defines to_h has its own case and never gets
     here, and a Struct, whose to_h is generated rather than emitted as a
     method, does (#4170). */
  if (!obj_default_done && argc == 0 && sp_streq(name, "to_h") &&
      nt_ref(nt, id, "block") < 0 && ret == TY_POLY) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_TO_H, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " default: _t%d = sp_poly_to_h_val(_t%d); break;", tr, tv);
    obj_default_done = 1;
  }
  /* The blockless index enumerators, same shape: an Array reaching this
     dispatch still answers them with an Enumerator. */
  if (argc == 0 && (sp_streq(name, "each_index") || sp_streq(name, "each_with_index"))) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_EACH_INDEX, -1, TY_UNKNOWN, PC_SAME);
    int ewi = !sp_streq(name, "each_index");
    char ev[96];
    snprintf(ev, sizeof ev, "%s(_t%d%s)",
             ewi ? "sp_Enumerator_new_ewi" : "sp_Enumerator_new_indices", tv, ewi ? ", 0" : "");
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: ");
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_ENUMERATOR, ev, b);
    else buf_puts(b, ev);
    buf_puts(b, "; break;");
  }
  /* Same shape one step on: `join` reaches this dispatch only because a
     user class owns the name, and a builtin array receiver still has to be
     joined rather than told it has no such method (#4071). Named arms, not
     the default: an OBJECT of a class whose `join` takes an argument is an
     arity error, which the raise below words. */
  if (argc == 0 && sp_streq(name, "join")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_JOIN, -1, TY_UNKNOWN, PC_SAME);
    char jv[80];
    snprintf(jv, sizeof jv, "sp_poly_join(_t%d, sp_str_empty)", tv);
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: ");
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_STRING, jv, b);
    else buf_puts(b, jv);
    buf_puts(b, "; break;");
    /* and a Thread, whose join is the wait: the arm answers the thread
       itself, as Thread#join does, where the arrays answer a string; a
       user class owning `join` (a model's path joiner) took this arm away
       from every `threads.each { |x| x.join }` in the program (#4466) */
    snprintf(jv, sizeof jv, "sp_poly_fiber_join(_t%d)", tv);
    buf_printf(b, " case SP_BUILTIN_THREAD: _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, jv);
    else if (ret == TY_STRING) buf_printf(b, "(%s, sp_str_empty)", jv);   /* the slot is the arrays' string; the wait still happens */
    else emit_unbox_text(c, ret, jv, b);
    buf_puts(b, "; break;");
  }
  /* the same for the names a pool polls on its workers (#4463) */
  if (argc == 0 && sp_streq(name, "alive?")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_ALIVE, -1, TY_UNKNOWN, PC_SAME);
    char av[80];
    snprintf(av, sizeof av, "sp_poly_fiber_alive(_t%d)", tv);
    buf_printf(b, " case SP_BUILTIN_THREAD: case SP_BUILTIN_FIBER: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_BOOL, av, b);
    else if (ret == TY_BOOL) buf_puts(b, av);
    else emit_unbox_text(c, ret, av, b);
    buf_puts(b, "; break;");
  }
  if (argc == 0 && sp_streq(name, "kill")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_KILL, -1, TY_UNKNOWN, PC_SAME);
    char kv[80];
    snprintf(kv, sizeof kv, "sp_poly_thread_kill(_t%d)", tv);
    buf_printf(b, " case SP_BUILTIN_THREAD: case SP_BUILTIN_FIBER: _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, kv);
    else emit_unbox_text(c, ret, kv, b);
    buf_puts(b, "; break;");
  }
  if (argc == 0 && sp_streq(name, "status")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_STATUS, -1, TY_UNKNOWN, PC_SAME);
    char sv[80];
    snprintf(sv, sizeof sv, "sp_poly_thread_status(_t%d)", tv);
    buf_printf(b, " case SP_BUILTIN_THREAD: _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, sv);
    else emit_unbox_text(c, ret, sv, b);
    buf_puts(b, "; break;");
  }
  /* and a boxed Queue, for the names a user class may own too (a Stack's
     #size or #pop): the queue still answers them itself */
  if (argc == 0) {
    const char *qf = NULL;
    if (is_len_alias(name)) qf = "sp_box_int(sp_Queue_size((sp_queue *)_t%d.v.p))";
    else if (sp_streq(name, "empty?")) qf = "sp_box_bool(sp_Queue_empty((sp_queue *)_t%d.v.p))";
    else if (sp_streq(name, "pop") || sp_streq(name, "shift") || sp_streq(name, "deq")) qf = "sp_Queue_pop((sp_queue *)_t%d.v.p)";
    else if (sp_streq(name, "num_waiting")) qf = "sp_box_int(sp_Queue_num_waiting((sp_queue *)_t%d.v.p))";
    else if (sp_streq(name, "closed?")) qf = "sp_box_bool(sp_Queue_closed((sp_queue *)_t%d.v.p))";
    else if (sp_streq(name, "max")) qf = "sp_box_int(sp_Queue_max((sp_queue *)_t%d.v.p))";
    /* close answers the queue itself */
    else if (sp_streq(name, "close")) qf = "((void)sp_Queue_close((sp_queue *)_t%d.v.p), _t%d)";
    if (qf) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_QUEUE, -1, TY_UNKNOWN, PC_SAME);
      char qv[120];
      snprintf(qv, sizeof qv, qf, tv, tv);
      buf_printf(b, " case SP_BUILTIN_QUEUE: _t%d = ", tr);
      if (ret == TY_POLY) buf_puts(b, qv);
      else emit_unbox_text(c, ret, qv, b);
      buf_puts(b, "; break;");
    }
  }
  /* IO#read on a poly value when a user class owns `read` (ffi's
     Pointer#read(type)): a File in the same slot still reads itself */
  if (argc == 0 && sp_streq(name, "read") && (ret == TY_POLY || ret == TY_STRING)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_IO_READ, -1, TY_UNKNOWN, PC_SAME);
    char rv[80];
    snprintf(rv, sizeof rv, "sp_File_read((sp_File *)_t%d.v.p)", tv);
    buf_printf(b, " case SP_BUILTIN_IO: _t%d = ", tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_nullable_str(%s)", rv);
    else buf_puts(b, rv);
    buf_puts(b, "; break;");
  }
  /* IO#flush on a poly value: the zero-arg sibling of the write arm in
     the argument-carrying dispatch. A Socket or File reaches this switch
     when any user class owns the name and fell to the raise default
     without an arm of its own (#4227). CRuby answers the receiver. */
  if (argc == 0 && sp_streq(name, "flush")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_IO_FLUSH, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " case SP_BUILTIN_IO: ");
    if (ret == TY_POLY)
      buf_printf(b, "sp_File_flush((sp_File *)_t%d.v.p); _t%d = _t%d;", tv, tr, tv);
    else
      buf_printf(b, "_t%d = sp_File_flush((sp_File *)_t%d.v.p);", tr, tv);
    buf_puts(b, " break;");
  }
  /* and close, for an IO or a Dir in the same slot: they answer nil
     (a Queue's arm above answers the queue) */
  if (argc == 0 && sp_streq(name, "close")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_IO_CLOSE, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " case SP_BUILTIN_IO: sp_File_close((sp_File *)_t%d.v.p); _t%d = ", tv, tr);
    emit_unbox_text(c, ret, "sp_box_nil()", b);
    buf_printf(b, "; break; case SP_BUILTIN_DIR: sp_Dir_close((sp_Dir *)_t%d.v.p); _t%d = ", tv, tr);
    emit_unbox_text(c, ret, "sp_box_nil()", b);
    buf_puts(b, "; break;");
  }
  /* And once more for `__enum_to_a`, which is not a name a user writes:
     the Enumerable desugar puts it in front of `obj.map { }` when the
     receiver's class defines #each. That rewrite is on the AST and
     permanent, so a receiver that was an object type when the desugar ran
     and widened to poly afterwards arrives here as an Array -- and was
     told it has no such method. An Array's own `to_a` is itself (#4150). */
  if (argc == 0 && sp_streq(name, "__enum_to_a")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_N_ENUM_TO_A, -1, TY_UNKNOWN, PC_SAME);
    char av[80];
    snprintf(av, sizeof av, "sp_poly_to_a_arr(_t%d)", tv);
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: ");
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_POLY_ARRAY, av, b);
    else buf_puts(b, av);
    buf_puts(b, "; break;");
  }
  return obj_default_done;
}

/* What a poly dispatch with arguments answers beside its user arms (its
   pre-arms and builtin cases), how its trailing keyword hash splits off,
   and its user candidates. Shared by the dispatch and the resolver
   (cplan_poly). */
void poly_specials_n(Compiler *c, int id, const char *name, int argc, const int *argv, PolySpecialsN *s) {
  const NodeTable *nt = c->nt;
  /* the builtin-array `[]` / Integer#[] bit-ref arm applies to an integer
     index; in promote mode that index variable may have widened to poly, so
     accept poly too (the index is unboxed where it is used below). */
  int is_index = sp_streq(name, "[]") && argc == 1 &&
                 (comp_ntype(c, argv[0]) == TY_INT || comp_ntype(c, argv[0]) == TY_POLY);
  /* `fetch(key[, default])` on a poly value that is actually a str/sym-keyed
     hash: without a user `fetch` candidate the dispatch was skipped and the
     call collapsed to default_value (an empty string), dropping the lookup.
     The str/sym-keyed hash arms below handle it, so admit it here. */
  /* Any key kind: the arms below cover the string- and symbol-keyed hashes,
     and the default at the end of the switch answers for every other
     receiver and key. Restricted to those key types, a `fetch` on a
     float-keyed hash emitted no dispatch at all and raised NoMethodError. */
  int is_fetch = sp_streq(name, "fetch") && (argc == 1 || argc == 2) && nt_ref(nt, id, "block") < 0;
  /* Names a user class can own, replacing the whole dispatch with its arms:
     a Hash or Array arriving at the same call matched nothing and raised
     NoMethodError naming its own class. They end in a runtime helper that
     lets the receiver answer for itself (the default at the switch's end). */
  /* A splatted list is one temp holding an array, not one temp per key, so
     the arms below cannot address the keys at all -- and admitting the name
     into the dispatch is what makes those temps exist. Leave the splat form
     exactly as it was before this arm: whatever the general path emits. */
  int has_splat_arg = 0;
  for (int a = 0; a < argc; a++) {
    const char *at2 = argv ? nt_type(nt, argv[a]) : NULL;
    if (at2 && sp_streq(at2, "SplatNode")) { has_splat_arg = 1; break; }
  }
  int is_pdelete = sp_streq(name, "delete") && argc == 1 && !has_splat_arg;
  int is_pdig = sp_streq(name, "dig") && argc >= 1 && !has_splat_arg;
  int is_pvalues_at = sp_streq(name, "values_at") && argc >= 1 && !has_splat_arg;
  /* `xs.first(n)` / `xs.last(n)` on a poly value that is a container at run
     time: the zero-arg forms have had an arm for a long time, the counted
     ones fell through to the raise (#3781 follow-up). */
  int is_pfirstn = (sp_streq(name, "first") || sp_streq(name, "last")) &&
                   argc == 1 && !has_splat_arg;
  int is_include = (sp_streq(name, "include?") || sp_streq(name, "member?") ||
                    sp_streq(name, "has_key?") || sp_streq(name, "key?")) && argc == 1;
  /* intersect? on a poly value that is a builtin array. The typed-receiver
     forms resolve, so only the union receiver was missing an arm and the
     call raised NoMethodError naming Array -- which is what the receiver
     was (#3414). Every array kind coerces to a poly array, so one arm
     covers them all rather than one per element type. */
  int is_intersect = sp_streq(name, "intersect?") && argc == 1;
  /* index / rindex / find_index with a VALUE argument on a poly value that
     is an array at run time. include? has had this arm for a long time; the
     index family answering the position rather than a bool was simply never
     added, so the call raised NoMethodError naming Array (#3409). */
  int is_arr_index = (is_index_query(name) && argc == 1 &&
                      nt_ref(nt, id, "block") < 0) ||
                     /* the two-argument form is String's alone -- Array#index
                        takes one argument -- so only the default arm below
                        answers it, and the array cases stay out (#4149). */
                     ((sp_streq(name, "index") || sp_streq(name, "rindex")) &&
                      argc == 2 && nt_ref(nt, id, "block") < 0);
  /* push/<</append on a poly value that is actually a builtin array: the
     array-mutate statement path skips it when a user class also defines the
     name (the value could be that object), so the switch needs a builtin-array
     arm or the append is silently dropped. sp_poly_shl handles every array
     kind; the user arms above cover the object case. */
  int is_push = is_push_alias(name) && argc >= 1;
  /* unshift/prepend are the same arm at the other end: without one they fell
     to the switch's NoMethodError default on a genuine Array (#4320). */
  int is_unshift = (sp_streq(name, "unshift") || sp_streq(name, "prepend")) && argc >= 1;
  /* delete(chars) with a string arg: the poly value may be a string even
     when a user class also defines `delete` (the bundled Set does), so the
     switch needs a TAG_STR pre-arm routing to String#delete (doom's
     `data[offset, 8].delete("\x00").upcase` WAD name fields). */
  int is_strdel = sp_streq(name, "delete") && argc == 1 &&
                  infer_type(c, argv[0]) == TY_STRING;
  /* partition / rpartition on a TAG_STR receiver. They have no poly arm of
     their own ahead of the name-collision test -- which is why `split`,
     `upcase` and `strip` survived a same-named user method and these did
     not -- so a genuine String fell to the switch's raising default and
     answered NoMethodError, naming String, for a method String has
     (#4413). A class nothing instantiates no longer takes the name away;
     this is the same hole for one that IS instantiated. */
  int is_strpart = (sp_streq(name, "partition") || sp_streq(name, "rpartition")) &&
                   argc == 1 && infer_type(c, argv[0]) == TY_STRING;
  /* The multi-set forms of count/delete/squeeze (String's alone) when a
     user class also owns the name: the switch needs a TAG_STR pre-arm or
     a genuine String receiver falls to its NoMethodError default, the
     same hole the single-set delete had (#4195). String-typed sets only:
     the temps below carry them as const char *. */
  int is_strsetop_n = ((sp_streq(name, "count") || sp_streq(name, "squeeze"))
                         ? argc >= 1     /* their 1-set form has no other pre-arm */
                         : sp_streq(name, "delete") && argc >= 2) &&
                      argc <= 8 && !has_splat_arg &&
                      nt_ref(nt, id, "block") < 0;
  if (is_strsetop_n)
    for (int a = 0; a < argc; a++)
      if (infer_type(c, argv[a]) != TY_STRING) { is_strsetop_n = 0; break; }
  /* Hash#store when a user class also owns the name: a boxed hash takes
     the runtime store, anything else its own arm or the default (#4195). */
  int is_pstore = sp_streq(name, "store") && argc == 2 && !has_splat_arg &&
                  nt_ref(nt, id, "block") < 0;
  /* split(sep) on a TAG_STR receiver, when a user class also owns `split`
     (the bundled Pathname does) and the dispatch therefore lost the String
     arm. Same hole #3394 closed for the zero-arg form (#3401). */
  int is_strsplit = sp_streq(name, "split") && argc == 1 &&
                    nt_ref(nt, id, "block") < 0 &&
                    (infer_type(c, argv[0]) == TY_STRING ||
                     infer_type(c, argv[0]) == TY_NIL ||
                     infer_type(c, argv[0]) == TY_REGEX);
  int is_pred = nt_ref(nt, id, "block") < 0 && poly_pred_kind(name, argc);
  /* String#encode when a user class also owns `encode`: the TAG_STR receiver
     needs a pre-arm, or a genuine String falls to the switch's raising
     default -- `content.encode("UTF-8", "binary", invalid: :replace, ...)`
     on an untyped value raised NoMethodError naming String once Active
     Storage's Variation#encode existed (#4452). The same transcode the
     typed and the unshadowed poly receivers take (sp_str_encode); the
     positionals and keywords come from the evaluated temps below. */
  int is_strencode = sp_streq(name, "encode") && argc >= 1 && !has_splat_arg &&
                     nt_ref(nt, id, "block") < 0;
  /* A trailing KeywordHashNode carries the call's keyword arguments: split
     it off so the user-method arms match keyword params by NAME, not by
     position (the whole hash used to flow into the *rest / first keyword
     slot, garbling both -- #3268). Any key and `**` of a named hash, an
     anonymous `**` or a literal are recognized. A key that is not a
     literal Symbol is a keyword too: a String one no keyword parameter
     takes, a computed one (`tr(:key) => 4`) whichever its value names.
     The hash was left whole as one more positional, so `o.m(1, "s" => 2)`
     against `def m(a, k: 1)` raised `wrong number of arguments` where
     CRuby names the unknown keyword. Such a hash is built whole, of any
     key, in source order, like one a `**` brings (kw_ds, kwall_any). */
  int kwh = -1, pos_argc = argc, kw_ds = 0, kw_strkey = 0;
  { const char *l_ty = nt_type(nt, argv[argc - 1]);
    if (l_ty && sp_streq(l_ty, "KeywordHashNode")) {
      int en = 0; const int *els = nt_arr(nt, argv[argc - 1], "elements", &en);
      int plain = en > 0, nds = 0;
      for (int e = 0; e < en; e++) {
        if (nt_kind(nt, els[e]) == NK_AssocSplatNode) {
          if (!poly_kw_splat_ok(c, els[e])) { plain = 0; break; }
          nds++;
          continue;
        }
        int key = nt_ref(nt, els[e], "key");
        if (key < 0) { plain = 0; break; }
        if (nt_kind(nt, key) != NK_SymbolNode) kw_strkey = 1;
      }
      if (plain) { kwh = argv[argc - 1]; pos_argc = argc - 1; kw_ds = nds > 0 || kw_strkey; }
    }
  }
  if (kw_ds) is_strencode = 0;
  int kw_pos = kwh < 0 || kw_ds;
  int ncand = 0;
  for (int k = 0; k < c->nclasses; k++) {
    /* a native class's methods are its declared bindings (#4504) */
    if (c->classes[k].is_native_class) {
      if (kw_pos && !has_splat_arg && c->classes[k].instantiated) {
        for (int n = pos_argc; n <= argc; n++) {
          int nmi = comp_native_method_find(c, k, name, n, 0);
          if (nmi >= 0 && native_takes(&c->native_methods[nmi], n)) { ncand++; break; }
        }
      }
      continue;
    }
    int mi = comp_method_in_chain(c, k, name, NULL);
    /* Include if call supplies all required params (pad defaults / truncate
       extras). A collapsed keyword hash funds one of them: without counting
       it, `r.where(cond: 1)` reaching `def where(condition)` looked like an
       arity mismatch, every arm was dropped, and the call lowered to the
       unresolved-method raise (#4030). */
    if (mi < 0) continue;
    { char exp0[600];
      if (poly_arm_count(c, &c->scopes[mi], kwh, pos_argc, -1, exp0, sizeof exp0)) ncand++; }
  }
  /* strftime on a poly value that is really a Time: a nilable Time
     (`created_at : Time?`) is held as a poly sp_RbVal, so `t.strftime(fmt)`
     would otherwise lower to the unresolved-call raise even when the value
     is a genuine Time. Give the switch a SP_BUILTIN_TIME arm so a real Time
     formats and nil/anything-else raises NoMethodError, matching CRuby
     (issue #2457, the family2 nilable value-method dispatch gap). Only when
     no user class defines strftime, so the default-raise arm is unambiguous. */
  int is_strftime = ncand == 0 && sp_streq(name, "strftime") && argc == 1 &&
                    infer_type(c, argv[0]) == TY_STRING;
  /* cover? on a container-read Range; gcdlcm on a container-read int
     receiver (#3234): builtin pre-arms, no user candidates required */
  /* `merge` on a poly value that is really a builtin Hash. A user class
     owning the name replaces the whole dispatch with its arms, and a Hash
     arriving at the same call matched nothing and raised NoMethodError
     naming its own class (#4033). The braceless-keyword form is the one
     that gets here, so the argument may be the collapsed hash rather than a
     positional. */
  int is_pmerge = sp_streq(name, "merge") && nt_ref(nt, id, "block") < 0 &&
                  !has_splat_arg && (pos_argc >= 1 || kwh >= 0);
  /* join on a poly value that is a builtin array, alongside the user arms.
     The dedicated poly-join arm stands down when a user class owns the name
     (#4071), so without this the Array case reached the raise. */
  int is_pjoin = sp_streq(name, "join") && argc <= 1 && !has_splat_arg &&
                 nt_ref(nt, id, "block") < 0;
  /* ...but only where the argument could BE a separator. The arm passes the
     call's own argument temp into sp_poly_join's const char * slot, so a
     user object there did not compile -- and this arm exists precisely
     because a user class owns the name, which is the program that passes
     one (#4292). Without the arm the builtin case reaches the raise, which
     is what CRuby answers for a non-String separator anyway. */
  if (is_pjoin && argc == 1) {
    TyKind jat = comp_ntype(c, argv[0]);
    if (!(jat == TY_STRING || jat == TY_POLY || jat == TY_NIL || jat == TY_UNKNOWN))
      is_pjoin = 0;
  }
  /* pack / unpack1 on a poly value that is a builtin container or string,
     alongside the user arms: their own arms stand down when a user class owns
     the name, and without these the builtin case reached the raise -- or, for
     unpack1, read an object through a char * (#4071's shape). */
  int is_ppack = (sp_streq(name, "pack") || sp_streq(name, "unpack1")) &&
                 argc == 1 && !has_splat_arg && nt_ref(nt, id, "block") < 0;
  /* ...and only where the argument could BE a format string, the same guard
     the join arm above carries: the arm passes the call's own argument temp
     into sp_poly_pack's const char * slot, so a user object there did not
     compile -- and this arm exists precisely because a user class owns the
     name, which is the program that passes one (#4319). Without the arm the
     builtin case reaches the raise, which is what CRuby answers for a
     non-String format anyway. */
  if (is_ppack) {
    TyKind pat = comp_ntype(c, argv[0]);
    if (!(pat == TY_STRING || pat == TY_POLY || pat == TY_UNKNOWN)) is_ppack = 0;
  }
  /* Both arms answer a String, so they can only be emitted where the result
     temp can hold one: the call typed from the user method alone (a Crate,
     say) has no room for the builtin answer, and assigning it there did not
     compile. Standing down leaves the builtin case at the raise, which is the
     trade the dig / values_at arms below already make (#4319). */
  if (is_ppack || is_pjoin) {
    TyKind pjr = comp_ntype(c, id);
    if (!(pjr == TY_POLY || pjr == TY_STRING || pjr == TY_UNKNOWN)) {
      is_ppack = 0; is_pjoin = 0;
    }
  }
  int is_cover = sp_streq(name, "cover?") && argc == 1 && !recv_user_defines(c, name);
  int is_gcdlcm = sp_streq(name, "gcdlcm") && argc == 1 && !recv_user_defines(c, name);
  /* try_convert on a class known only at run time: a constant receiver
     has its typed emitter, but `[Array, 0][0].try_convert(x)` reached no
     arm at all and the call lowered to the unresolved-method raise. The
     runtime answers by the class's name, as the typed emitters answer by
     the constant's (#2325, #2585). */
  int is_ctryconv = sp_streq(name, "try_convert") && argc == 1 && !has_splat_arg && kw_pos &&
                    nt_ref(nt, id, "block") < 0 && !recv_user_defines(c, name) &&
                    comp_ntype(c, id) == TY_POLY;
  s->index = is_index;
  s->fetch = is_fetch;
  s->pdelete = is_pdelete;
  s->pdig = is_pdig;
  s->pvalues_at = is_pvalues_at;
  s->pfirstn = is_pfirstn;
  s->include = is_include;
  s->intersect = is_intersect;
  s->arr_index = is_arr_index;
  s->push = is_push;
  s->unshift = is_unshift;
  s->strdel = is_strdel;
  s->strpart = is_strpart;
  s->strsetop_n = is_strsetop_n;
  s->pstore = is_pstore;
  s->strsplit = is_strsplit;
  s->pred = is_pred;
  s->strencode = is_strencode;
  s->strftime = is_strftime;
  s->pmerge = is_pmerge;
  s->pjoin = is_pjoin;
  s->ppack = is_ppack;
  s->cover = is_cover;
  s->gcdlcm = is_gcdlcm;
  s->ctryconv = is_ctryconv;
  s->has_splat_arg = has_splat_arg;
  s->kwh = kwh;
  s->pos_argc = pos_argc;
  s->kw_ds = kw_ds;
  s->kw_strkey = kw_strkey;
  s->kw_pos = kw_pos;
  s->ncand = ncand;
}

/* A splatted argument: the first splat's position (splat_a) and whether it
   is the last positional, alone (splat_last); the arms that read one value
   per argument temp stand down. */
void poly_specials_n_splat(Compiler *c, const int *argv, PolySpecialsN *s, int *splat_a, int *splat_last) {
  const NodeTable *nt = c->nt;
  int pos_argc = s->pos_argc;
  if (s->has_splat_arg) {
    int nspl = 0;
    for (int a = 0; a < pos_argc; a++) {
      if (nt_kind(nt, argv[a]) != NK_SplatNode) continue;
      if ((*splat_a) < 0) (*splat_a) = a;
      nspl++;
    }
    (*splat_last) = nspl == 1 && (*splat_a) == pos_argc - 1;
    /* unshift inserts each value at its own index: after a splat that is
       the run time's */
    if (!(*splat_last)) s->unshift = 0;
    /* the builtin arms below read their argument temps one value each,
       but push and unshift, which spread the splat's temp themselves */
    s->index = s->fetch = s->include = s->intersect = s->arr_index = 0;
    s->strdel = s->strpart = s->strsplit = s->pred = 0;
    s->strftime = s->cover = s->gcdlcm = s->pfirstn = 0;
  }
}

/* The tag pre-arms of a poly dispatch with arguments, ahead of its cls_id
   switch: a Range's cover?, a class's try_convert, an Integer's gcdlcm and
   bit reference, a String's unpack1, include?, delete, partition, count
   and squeeze, encode, split and index, a Hash's store. */
void emit_poly_prearms_n(Compiler *c, const char *name, const PolySpecialsN *ps, const PolyTemps *T, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc, kwall = T->kwall, kwn = T->kwn;
  const int *argv = T->argv, *atmp = T->atmp, *kwels = T->kwels, *kwtmp = T->kwtmp;
  const TyKind *atmp_ty = T->atmp_ty, *kwty = T->kwty;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  const char *idxref = T->idxref;
  int is_cover = ps->cover, is_ctryconv = ps->ctryconv, is_gcdlcm = ps->gcdlcm, is_ppack = ps->ppack;
  int is_include = ps->include, is_strdel = ps->strdel, is_strpart = ps->strpart;
  int is_strsetop_n = ps->strsetop_n, is_pstore = ps->pstore, is_strencode = ps->strencode;
  int is_strsplit = ps->strsplit, is_index = ps->index;
  /* Range#cover? on a runtime Range receiver (#3234) */
  if (is_cover) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_COVER, -1, TY_UNKNOWN, PC_SAME);
    const char *fn = atmp_ty[0] == TY_POLY ? "cover_poly" : atmp_ty[0] == TY_FLOAT ? "cover_f" : "include";
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)"
                  " { _t%d = %ssp_range_%s((sp_Range *)_t%d.v.p, _t%d)%s; }\nelse ",
               tv, tv, tr,
               ret == TY_POLY ? "sp_box_bool(" : "", fn, tv, atmp[0],
               ret == TY_POLY ? ")" : "");
    /* a Float range covers by value (it fell through to false) */
    if (atmp_ty[0] == TY_POLY || atmp_ty[0] == TY_FLOAT || atmp_ty[0] == TY_INT) {
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_FLOAT_RANGE && _t%d.v.p)"
                    " { _t%d = %s", tv, tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "");
      if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_frange_cover_poly(*(sp_FloatRange *)_t%d.v.p, _t%d)", tv, atmp[0]);
      else buf_printf(b, "sp_frange_cover(*(sp_FloatRange *)_t%d.v.p, (sp_float)_t%d)", tv, atmp[0]);
      buf_printf(b, "%s; }\nelse ", ret == TY_POLY ? ")" : "");
    }
  }
  /* Klass.try_convert(x) on a class-tagged receiver, checked ahead of
     the cls_id switch: no user class defines the name, so no arm below
     answers it, and the default raises for every other receiver. */
  if (is_ctryconv) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_TRY_CONVERT, -1, TY_UNKNOWN, PC_SAME);
    char an[40]; snprintf(an, sizeof an, "_t%d", atmp[0]);
    buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) { ", tv);
    if (kwall >= 0)
      buf_printf(b, "if (_t%d->len == 0) { (void)sp_poly_class_try_convert(_t%d, sp_box_nil());"
                    " sp_raise_cls(\"ArgumentError\", \"wrong number of arguments (given 0, expected 1)\"); } ",
                 kwall, tv);
    buf_printf(b, "_t%d = sp_poly_class_try_convert(_t%d, ", tr, tv);
    /* a pattern temp has no boxed spelling in emit_boxed_text (it boxes
       nil there); the node form's sp_box_regexp is the one to use */
    if (atmp_ty[0] == TY_REGEX) buf_printf(b, "sp_box_regexp(%s)", an);
    else emit_boxed_text(c, atmp_ty[0], an, b);
    buf_puts(b, "); }\nelse ");
  }
  /* Integer#gcdlcm on a runtime int receiver (#3234): [gcd, lcm] */
  if (is_gcdlcm) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_GCDLCM, -1, TY_UNKNOWN, PC_SAME);
    char ax[64];
    if (atmp_ty[0] == TY_POLY) snprintf(ax, sizeof ax, "sp_poly_arg_i_msg(_t%d, \"not an integer\")", atmp[0]);
    else snprintf(ax, sizeof ax, "_t%d", atmp[0]);
    int tg2 = ++g_tmp;
    buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { sp_IntArray *_t%d = sp_IntArray_new(); SP_GC_ROOT(_t%d);"
                  " sp_IntArray_push(_t%d, sp_gcd(_t%d.v.i, %s));"
                  " sp_IntArray_push(_t%d, sp_lcm(_t%d.v.i, %s)); _t%d = ",
               tv, tg2, tg2, tg2, tv, ax, tg2, tv, ax, tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_int_array(_t%d)", tg2);
    else if (ret == TY_INT_ARRAY) buf_printf(b, "_t%d", tg2);
    else buf_printf(b, "(sp_int)(uintptr_t)_t%d", tg2);
    buf_puts(b, "; }\nelse ");
  }
  /* unpack1 on a TAG_STR receiver: its own poly arm stands down when a user
     class owns the name, so the String case needs one here -- and the old
     fallthrough read the receiver as a char * whatever it held. */
  if (is_ppack && sp_streq(name, "unpack1")) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_UNPACK1, -1, TY_UNKNOWN, PC_SAME);
    Buf ub; memset(&ub, 0, sizeof ub);
    buf_printf(&ub, "sp_PolyArray_get(sp_str_unpack(_t%d.v.s, _t%d), 0)", tv, atmp[0]);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY) buf_puts(b, ub.p ? ub.p : "");
    else emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, ub.p ? ub.p : "", b);
    buf_puts(b, "; }\nelse ");
    free(ub.p);
  }
  /* include? on a TAG_STR receiver: check tag before entering cls_id switch.
     Boxed when a user arm widened the dispatch result to poly (#4072). */
  /* A shared-string handle is a String too. The receiver temp is NOT
     dereferenced at the spill -- the mutating arms sharing it need the
     handle -- so this arm widens its own guard and reads the bytes with
     sp_poly_recv_s. Without it a heap String fell through to the cls_id
     switch and answered false, silently (#4279). */
  /* String answers include? alone (member?/key? are NoMethodError), and
     its argument must be a String: another class is CRuby's TypeError,
     and a boxed one is checked at run time. */
  if (is_include) if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INCLUDE, -1, TY_UNKNOWN, PC_SAME);
  if (is_include && !sp_streq(name, "include?"))
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) sp_raise_poly_nomethod(\"%s\", _t%d);\nelse ",
               tv, tv, name, tv);
  else if (is_include && infer_type(c, argv[0]) == TY_STRING)
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) { _t%d = %ssp_str_include(sp_poly_recv_s(_t%d, \"include?\"), _t%d)%s; }\nelse ",
               tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, atmp[0],
               ret == TY_POLY ? ")" : "");
  else if (is_include) {
    Buf ab6; memset(&ab6, 0, sizeof ab6);
    char tn6[24]; snprintf(tn6, sizeof tn6, "_t%d", atmp[0]);
    if (atmp_ty[0] == TY_POLY) buf_puts(&ab6, tn6);
    else emit_boxed_text(c, atmp_ty[0], tn6, &ab6);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) { _t%d = %ssp_str_include(sp_poly_recv_s(_t%d, \"include?\"), sp_poly_arg_str_chk(sp_poly_strbuf_deref(%s)))%s; }\nelse ",
               tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, ab6.p ? ab6.p : "sp_box_nil()",
               ret == TY_POLY ? ")" : "");
    free(ab6.p);
  }
  /* delete(chars) on a TAG_STR receiver: String#delete, boxed when the
     dispatch result stays poly. */
  if (is_strdel && (ret == TY_POLY || ret == TY_STRING)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_DELETE, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(sp_str_delete(_t%d.v.s, _t%d))", tv, atmp[0]);
    else buf_printf(b, "sp_str_delete(_t%d.v.s, _t%d)", tv, atmp[0]);
    buf_puts(b, "; }\nelse ");
  }
  /* partition / rpartition on a TAG_STR receiver: sp_str_partition answers
     the sp_StrArray CRuby's three-element result is, boxed when a user arm
     widened the dispatch's result to poly. */
  if (is_strpart && (ret == TY_POLY || ret == TY_STR_ARRAY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_PARTITION, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY)
      buf_printf(b, "sp_box_str_array(sp_str_%s(_t%d.v.s, _t%d))", name, tv, atmp[0]);
    else
      buf_printf(b, "sp_str_%s(_t%d.v.s, _t%d)", name, tv, atmp[0]);
    buf_puts(b, "; }\nelse ");
  }
  /* the multi-set forms on a TAG_STR receiver (#4195) */
  if (is_strsetop_n) {
    int is_cnt = sp_streq(name, "count");
    if ((is_cnt && (ret == TY_POLY || ret == TY_INT)) ||
        (!is_cnt && (ret == TY_POLY || ret == TY_STRING))) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_SETOP, -1, TY_UNKNOWN, PC_SAME);
      char sets[256]; int sl;
      sl = snprintf(sets, sizeof sets, "(const char *[]){");
      for (int a = 0; a < argc && sl < (int)sizeof sets - 16; a++)
        sl += snprintf(sets + sl, sizeof sets - (size_t)sl, "%s_t%d", a ? ", " : "", atmp[a]);
      snprintf(sets + sl, sizeof sets - (size_t)sl, "}, %d", argc);
      char call[384];
      snprintf(call, sizeof call, "sp_str_%s_n(_t%d.v.s ? _t%d.v.s : \"\", %s)",
               is_cnt ? "count" : sp_streq(name, "delete") ? "delete" : "squeeze",
               tv, tv, sets);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
      if (ret == TY_POLY) buf_printf(b, "%s(%s)", is_cnt ? "sp_box_int" : "sp_box_str", call);
      else buf_puts(b, call);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* Hash#store on a boxed hash receiver (#4195) */
  if (is_pstore && ret == TY_POLY) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STORE, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && sp_poly_is_hash_kind(_t%d.cls_id)) { _t%d = sp_poly_store(_t%d, ", tv, tv, tr, tv);
    char a0[32], a1[32];
    snprintf(a0, sizeof a0, "_t%d", atmp[0]);
    snprintf(a1, sizeof a1, "_t%d", atmp[1]);
    if (atmp_ty[0] == TY_POLY) buf_puts(b, a0); else emit_boxed_text(c, atmp_ty[0], a0, b);
    buf_puts(b, ", ");
    if (atmp_ty[1] == TY_POLY) buf_puts(b, a1); else emit_boxed_text(c, atmp_ty[1], a1, b);
    buf_puts(b, "); }\nelse ");
  }
  /* encode(enc[, from][, invalid:, undef:, replace:]) on a TAG_STR receiver */
  if (is_strencode && pos_argc <= 2 && (ret == TY_POLY || ret == TY_STRING)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_ENCODE, -1, TY_UNKNOWN, PC_SAME);
    Buf eb; memset(&eb, 0, sizeof eb);
    buf_printf(&eb, "sp_str_encode(_t%d.v.s", tv);
    for (int a = 0; a < 2; a++) {
      buf_puts(&eb, ", ");
      if (a < pos_argc) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
        if (atmp_ty[a] == TY_POLY) buf_puts(&eb, tn); else emit_boxed_text(c, atmp_ty[a], tn, &eb);
      }
      else buf_puts(&eb, "sp_box_nil()");
    }
    static const char *const EKW[] = { "invalid", "undef", "replace" };
    for (int k = 0; k < 3; k++) {
      buf_puts(&eb, ", ");
      int found = -1;
      for (int e = 0; e < kwn; e++) {
        int key = nt_ref(nt, kwels[e], "key");
        const char *kn = key >= 0 ? nt_str(nt, key, "value") : NULL;
        if (kn && sp_streq(kn, EKW[k])) { found = e; break; }
      }
      if (found >= 0) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", kwtmp[found]);
        if (kwty[found] == TY_POLY) buf_puts(&eb, tn); else emit_boxed_text(c, kwty[found], tn, &eb);
      }
      else buf_puts(&eb, "sp_box_nil()");
    }
    buf_puts(&eb, ")");
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(%s)", eb.p); else buf_puts(b, eb.p);
    buf_puts(b, "; }\nelse ");
    free(eb.p);
  }
  /* split(sep) on a TAG_STR receiver. A nil separator splits on
     whitespace, as CRuby's does. */
  if (is_strsplit && (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_SPLIT_N, -1, TY_UNKNOWN, PC_SAME);
    TyKind sat = infer_type(c, argv[0]);
    char call[192];
    if (sat == TY_NIL) snprintf(call, sizeof call, "sp_str_split_ws(_t%d.v.s)", tv);
    else if (sat == TY_REGEX) snprintf(call, sizeof call, "sp_re_split(_t%d, _t%d.v.s)", atmp[0], tv);
    else snprintf(call, sizeof call, "sp_str_split_drop_trailing(_t%d.v.s, _t%d)", tv, atmp[0]);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_STR_ARRAY) buf_puts(b, call);
    else if (ret == TY_POLY_ARRAY) buf_printf(b, "sp_StrArray_to_poly_fmt(%s)", call);
    else buf_printf(b, "sp_box_str_array(%s)", call);
    buf_puts(b, "; }\nelse ");
  }
  /* Integer#[N] bit-extraction: poly recv may hold a tagged int */
  if (is_index) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INT_BITREF, -1, TY_UNKNOWN, PC_SAME);
    if (ret == TY_POLY)
      buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = sp_box_int((_t%d.v.i >> %s) & 1); }\nelse ", tv, tr, tv, idxref);
    else
      buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = (_t%d.v.i >> %s) & 1; }\nelse ", tv, tr, tv, idxref);
    /* String#[int]: a poly value that is really a String (e.g. a method
       with multiple return paths widened to poly) answers `[]` with the
       single character at the index, or nil. The cls_id switch below only
       covers SP_TAG_OBJ variants, so without this tag arm a String receiver
       fell through and returned the seed (nil/0). */
    if (ret == TY_POLY)
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_box_nullable_str(sp_str_char_at_or_nil(_t%d.v.s, %s)); }\nelse ", tv, tr, tv, idxref);
    else if (ret == TY_STRING)
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = sp_str_char_at_or_nil(_t%d.v.s, %s); }\nelse ", tv, tr, tv, idxref);
  }
}

/* The pre-arms of a poly dispatch with arguments after its tag pre-arms:
   the block a candidate takes, hoisted once as a proc (its temp is the
   result), a class value's class-side arms, a callable value's call (with
   the keyword split rebuilt as the trailing hash, or a lone splat spread).
   atmp and atmp_ty are the dispatch's own: the callable's keyword hash
   takes the slot after the positionals. */
int emit_poly_prearms_n_blk(Compiler *c, int id, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                            int *atmp, TyKind *atmp_ty, const PolyKw *kw, const int *htmp, int is_setter_val,
                            int splat_a, int splat_last, int stk, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc, kwall = T->kwall, kwh = ps->kwh;
  const int *argv = T->argv;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  int kw_pos = ps->kw_pos, has_splat_arg = ps->has_splat_arg;
  int called = 0;
  /* class 0 emits a `case 0:` arm here when it defines/inherits the method
     with its arity satisfied; guard the key so a boxed scalar (cls_id 0)
     cannot alias it (issue #1576). */
  /* Same shared-proc materialization the zero-arg dispatch does (#3399). */
  int blk_tmp2 = -1;
  { int cblk2 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
    if (cblk2 >= 0) {
      for (int k = 0; k < c->nclasses && blk_tmp2 < 0; k++) {
        if (!c->classes[k].instantiated) continue;
        int mi2 = comp_method_in_chain(c, k, name, NULL);
        if (mi2 < 0) continue;
        Scope *cm2 = &c->scopes[mi2];
        if (!scope_has_callable_symbol(c, mi2) && !scope_needs_proc_form(c, mi2)) continue;
        if ((cm2->blk_param && cm2->blk_param[0] && !cm2->yields) ||
            scope_needs_proc_form(c, mi2)) {
          /* a forwarded &blk / anonymous & is a live proc, not a literal
             to lower -- the same guard the other dispatch arms carry */
          blk_tmp2 = hoist_block_proc(c, cblk2);
        }
      }
    } }
  /* a class-valued receiver dispatches class-side, ahead of the instance
     arms (#4218). */
  emit_poly_cls_value_prearm(c, id, name, pos_argc, atmp, atmp_ty, htmp, kw, tv,
                             is_setter_val ? -1 : tr, ret, blk_tmp2, b);
  /* a boxed Proc/Curry/Method in a slot a user `call`/`[]` shadows (#4395);
     the keyword split binds by name to a user candidate, so skip it. A
     splatted argument is one temp holding an array, not one temp per
     value, so the positional publish sequence below cannot spread it. */
  if (kw_pos && !has_splat_arg) {
    if (kwall >= 0) {
      for (int e = 0; e < 2; e++) {
        char kg[48];
        snprintf(kg, sizeof kg, "_t%d->len %s 0 && ", kwall, e ? ">" : "==");
        called |= emit_poly_callable_prearm(c, name, pos_argc + e, atmp, atmp_ty, kg, tv, tr, ret,
                                            e ? 2 : pos_argc > 0 ? 1 : 0, b);
      }
    }
    else called = emit_poly_callable_prearm(c, name, argc, atmp, atmp_ty, NULL, tv, tr, ret,
                                            argc == 0 ? 0 : nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? 2 : 1, b);
  }
  else if (kwh >= 0 && !has_splat_arg &&
           is_call_alias(name) &&
           argc <= SP_PROC_ARG_SLOTS) {
    /* The keyword split has no hash for a callable to take: build one
       from the per-key temps, only when the slot holds a callable, and
       pass it as the trailing argument marked as keywords. */
    int kht = ++g_tmp;
    buf_printf(b, "sp_SymPolyHash *_t%d = NULL; SP_GC_ROOT(_t%d); "
                  "if (_t%d.tag == SP_TAG_OBJ && (_t%d.cls_id == SP_BUILTIN_PROC"
                  " || _t%d.cls_id == SP_BUILTIN_CURRY || _t%d.cls_id == SP_BUILTIN_METHOD)) _t%d = ",
               kht, kht, tv, tv, tv, tv, kht);
    emit_kwh_sym_hash(c, kw, NULL, b);
    buf_puts(b, "; ");
    atmp[pos_argc] = kht;
    atmp_ty[pos_argc] = TY_SYM_POLY_HASH;
    called = emit_poly_callable_prearm(c, name, argc, atmp, atmp_ty, NULL, tv, tr, ret, 2, b);
  }
  else if (splat_last)
    called = emit_poly_callable_spread_prearm(c, name, splat_a, atmp, atmp_ty,
                                              stk >= 0 ? stk : atmp[splat_a], tv, tr, ret, b);
  else if (splat_a >= 0 && is_call_alias(name))
    unsupported(c, id, "a splat before other arguments into a method called on a value of more than one type");
  if (called && g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CALLABLE, -1, TY_UNKNOWN, PC_SAME);
  return blk_tmp2;
}

/* The builtin `case` arms a poly dispatch with arguments writes after its
   class arms: an Array's index, push, unshift, pack, join, include?,
   index and intersect?, an IO's write, read and the rest, a Time's
   strftime, a Hash's `[]` and fetch by key kind, a predicate. */
void emit_poly_cases_n(Compiler *c, int id, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                       int splat_a, int is_aref, int is_fetch, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc, kwh = ps->kwh;
  const int *argv = T->argv, *atmp = T->atmp;
  const TyKind *atmp_ty = T->atmp_ty;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  const char *idxref = T->idxref;
  int is_index = ps->index, is_unshift = ps->unshift, is_push = ps->push, is_ppack = ps->ppack;
  int is_pjoin = ps->pjoin, is_include = ps->include, is_arr_index = ps->arr_index;
  int is_intersect = ps->intersect, is_strftime = ps->strftime, is_pred = ps->pred;
  if (is_index) {
    emit_poly_index_cases(ret, tr, tv, idxref, b);
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INDEX_CASES, -1, TY_UNKNOWN, PC_SAME);
  }
  /* IO#write on a poly value when a user class owns the name: a Socket or
     File from Socket.pair/File.open has no user-class arm in the switch,
     but the dispatch was still opened (some other class does define
     #write), so the builtin tag needs its own case. Use the arg's
     already-materialised temp `_t<atmp[0]>` (re-emitting argv[0] would
     re-evaluate side effects like `poly_io.write(next_chunk())` and
     write a different chunk on each call). Skip when the call is
     keyword-only: atmp[0] is then uninitialised because pos_argc == 0
     when kwh >= 0, and IO#write has no keyword form. */
  /* read_nonblock on the builtin IO tag, the same shape as the write arm
     below: a Socket reaching this dispatch because some user class owns
     the name had no arm and raised NoMethodError (#4236). The keyword
     hash is read off the CALL, not off a temp -- `exception: false` is
     part of the shape, not an argument that flows -- and the positional
     length rides its already-materialised temp. */
  if (sp_streq(name, "read_nonblock") && pos_argc == 1 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_READ_NB, -1, TY_UNKNOWN, PC_SAME);
    int no_exc7 = 0;
    if (kwh >= 0) {
      int e7 = kwh_lookup(nt, kwh, "exception");
      no_exc7 = e7 >= 0 && nt_type(nt, e7) && sp_streq(nt_type(nt, e7), "FalseNode");
    }
    int trd7 = ++g_tmp, te7 = ++g_tmp;
    buf_printf(b, " case SP_BUILTIN_IO: { sp_bool _e%d; const char *_t%d = sp_sock_read_nb("
                  "(sp_File *)_t%d.v.p, ", te7, trd7, tv);
    if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_poly_arg_i(_t%d)", atmp[0]);
    else buf_printf(b, "(sp_int)_t%d", atmp[0]);
    buf_printf(b, ", %d, 0, &_e%d); ", no_exc7 ? 0 : 1, te7);
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) {
      if (no_exc7)
        buf_printf(b, "_t%d ? sp_box_str(_t%d) : (_e%d ? sp_box_nil() : sp_box_sym(sp_sym_intern(\"wait_readable\")))",
                   trd7, trd7, te7);
      else buf_printf(b, "sp_box_str(_t%d)", trd7);
    }
    else buf_printf(b, "_t%d", trd7);
    buf_puts(b, "; break; }");
  }
  if (sp_streq(name, "write") && argc == 1 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_WRITE, -1, TY_UNKNOWN, PC_SAME);
    int wrv = ++g_tmp;
    if (atmp_ty[0] == TY_STRING) {
      /* String arg: the temp is a const char * from a String source.
         sp_File_write_bin sizes the operand with sp_str_byte_len, so
         an embedded NUL reaches the descriptor; sp_File_write
         truncates on NUL. */
      if (ret == TY_POLY)
        buf_printf(b, " case SP_BUILTIN_IO: { sp_int _t%d = sp_File_write_bin("
                       "(sp_File *)_t%d.v.p, _t%d); "
                       "_t%d = sp_box_int(_t%d); break; }",
                   wrv, tv, atmp[0], tr, wrv);
      else
        buf_printf(b, " case SP_BUILTIN_IO: _t%d = sp_File_write_bin("
                       "(sp_File *)_t%d.v.p, _t%d); break;",
                   tr, tv, atmp[0]);
    }
    else {
      /* Non-String arg: atmp[0] is either sp_RbVal (TY_POLY/TY_NIL/
         TY_VOID/TY_UNKNOWN were stored boxed at allocation) or a
         scalar slot (TY_INT, TY_FLOAT, ...). Box the scalar into
         sp_RbVal first, then route the boxed value through
         sp_poly_to_s. When the slot is TY_POLY, the runtime value
         may carry SP_TAG_STR (a String that came in through a poly
         param or ivar read); in that case the SP_TAG_STR branch uses
         sp_File_write_bin directly so an embedded NUL survives,
         otherwise sp_poly_to_s may return either a marked String
         (also safe to binary-write) or a static SPL() literal
         (strlen-based, which sp_File_write already handles). The
         result is always boxed to sp_RbVal (wrv) so the surrounding
         _t<tr> either takes it directly (ret == TY_POLY) or unboxes
         to sp_int (ret == TY_INT). */
      int wrr = ++g_tmp, wrv = ++g_tmp;
      char a0n[24]; snprintf(a0n, sizeof a0n, "_t%d", atmp[0]);
      buf_puts(b, " case SP_BUILTIN_IO: { ");
      if (atmp_ty[0] != TY_POLY) {
        buf_printf(b, "sp_RbVal _t%d = ", wrr);
        emit_boxed_text(c, atmp_ty[0], a0n, b);
        buf_puts(b, "; ");
      }
      else buf_printf(b, "sp_RbVal _t%d = %s; ", wrr, a0n);
      buf_printf(b, "sp_RbVal _t%d = (_t%d.tag == SP_TAG_STR) ? "
                     "sp_box_int(sp_File_write_bin((sp_File *)_t%d.v.p, _t%d.v.s)) : "
                     "sp_box_int(sp_File_write((sp_File *)_t%d.v.p, sp_poly_to_s(_t%d))); ",
                   wrv, wrr, tv, wrr, tv, wrr);
      if (ret == TY_POLY) buf_printf(b, "_t%d = _t%d; ", tr, wrv);
      else                buf_printf(b, "_t%d = sp_poly_to_i(_t%d); ", tr, wrv);
      buf_puts(b, "break; }");
    }
  }
  /* syswrite on a poly value: the same shape as the write arm above --
     a Socket reaching this dispatch because some user class owns the
     name had no arm and raised NoMethodError. syswrite takes one
     String arg and returns the byte count. The byte length is sized
     by the caller and handed to sp_File_syswrite directly: a String
     source's length (sp_str_byte_len, so an embedded NUL reaches the
     descriptor) or a converted value's length (strlen of sp_poly_to_s),
     rather than reading it off a marker byte inside the write core. */
  if (sp_streq(name, "syswrite") && argc == 1 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_SYSWRITE, -1, TY_UNKNOWN, PC_SAME);
    int wrv = ++g_tmp;
    if (atmp_ty[0] == TY_STRING) {
      if (ret == TY_POLY)
        buf_printf(b, " case SP_BUILTIN_IO: { sp_int _t%d = sp_File_syswrite("
                       "(sp_File *)_t%d.v.p, _t%d, sp_str_byte_len(_t%d)); "
                       "_t%d = sp_box_int(_t%d); break; }",
                   wrv, tv, atmp[0], atmp[0], tr, wrv);
      else
        buf_printf(b, " case SP_BUILTIN_IO: _t%d = sp_File_syswrite("
                       "(sp_File *)_t%d.v.p, _t%d, sp_str_byte_len(_t%d)); break;",
                   tr, tv, atmp[0], atmp[0]);
    }
    else {
      int wrr = ++g_tmp, slen = ++g_tmp, wlen = ++g_tmp, wres = ++g_tmp;
      char a0n[24]; snprintf(a0n, sizeof a0n, "_t%d", atmp[0]);
      buf_puts(b, " case SP_BUILTIN_IO: { ");
      if (atmp_ty[0] != TY_POLY) {
        buf_printf(b, "sp_RbVal _t%d = ", wrr);
        emit_boxed_text(c, atmp_ty[0], a0n, b);
        buf_puts(b, "; ");
      }
      else buf_printf(b, "sp_RbVal _t%d = %s; ", wrr, a0n);
      /* Root the boxed operand before calling sp_poly_to_s: the
         conversion can allocate and trigger GC, which would lose the
         operand if it were only reachable through a local. */
      buf_printf(b, "SP_GC_ROOT_RBVAL(_t%d); ", wrr);
      /* Resolve the operand to a (pointer, length) pair once. A marked
         String keeps its header length (binary-safe, embedded NULs
         survive); anything else is a NUL-terminated C string from
         sp_poly_to_s, whose length is the C-string length. */
      buf_printf(b, "const char *_t%d = (_t%d.tag == SP_TAG_STR) ? _t%d.v.s : sp_poly_to_s(_t%d); "
                 "sp_int _t%d = (_t%d.tag == SP_TAG_STR) ? "
                 "sp_str_byte_len(_t%d) : strlen(_t%d); "
                 "sp_int _t%d = sp_File_syswrite((sp_File *)_t%d.v.p, _t%d, _t%d); ",
                 slen, wrr, wrr, wrr, wlen, wrr, slen, slen,
                 wres, tv, slen, wlen);
      if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_int(_t%d); ", tr, wres);
      else                buf_printf(b, "_t%d = _t%d; ", tr, wres);
      buf_puts(b, "break; }");
    }
  }
  /* print and puts on a poly value, beside the write arm: an IO held
     where a StringIO can be, or reached through a user class that owns
     the name (Zlib::GzipWriter#print), had no arm and raised
     NoMethodError (#6158). Each argument is boxed; puts takes it through
     sp_File_puts_val, which flattens an Array as Kernel#puts does, and
     print writes its to_s. Both answer nil. */
  if ((sp_streq(name, "puts") || (sp_streq(name, "print") && argc > 0)) &&
      kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_PRINT, -1, TY_UNKNOWN, PC_SAME);
    int is_puts = sp_streq(name, "puts");
    buf_puts(b, " case SP_BUILTIN_IO: { ");
    if (is_puts && argc == 0) buf_printf(b, "sp_File_write((sp_File *)_t%d.v.p, \"\\n\"); ", tv);
    for (int a = 0; a < argc; a++) {
      int pv = ++g_tmp;
      char an[24]; snprintf(an, sizeof an, "_t%d", atmp[a]);
      buf_printf(b, "sp_RbVal _t%d = ", pv);
      if (atmp_ty[a] == TY_POLY) buf_puts(b, an);
      else emit_boxed_text(c, atmp_ty[a], an, b);
      buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", pv);
      if (is_puts) buf_printf(b, "sp_File_puts_val((sp_File *)_t%d.v.p, _t%d); ", tv, pv);
      else buf_printf(b, "if (_t%d.tag == SP_TAG_STR) sp_File_write_bin((sp_File *)_t%d.v.p, _t%d.v.s); "
                         "else sp_File_write((sp_File *)_t%d.v.p, sp_poly_to_s(_t%d)); ",
                      pv, tv, pv, tv, pv);
    }
    if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_nil(); ", tr);
    buf_puts(b, "break; }");
  }
  /* putc on a poly value, beside print: an IO held where a StringIO can
     be had no arm and raised NoMethodError. sp_File_putc takes the boxed
     argument (an Integer's low byte or a String's first character) and
     answers it; a concrete result slot keeps the call for its effect. */
  if (sp_streq(name, "putc") && argc == 1 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_PUTC, -1, TY_UNKNOWN, PC_SAME);
    char an[24]; snprintf(an, sizeof an, "_t%d", atmp[0]);
    buf_puts(b, " case SP_BUILTIN_IO: ");
    if (ret == TY_POLY) buf_printf(b, "_t%d = ", tr);
    buf_printf(b, "sp_File_putc((sp_File *)_t%d.v.p, ", tv);
    if (atmp_ty[0] == TY_POLY) buf_puts(b, an);
    else emit_boxed_text(c, atmp_ty[0], an, b);
    buf_puts(b, "); break;");
  }
  /* seek and read(n) on a poly value, the positioning pair beside the
     write arm: a File held in the same ivar as a StringIO reached this
     dispatch because StringIO owns the names, and with no arm of its own
     `@io.seek(4)` raised NoMethodError for the File. The offset, whence
     and length ride their already-materialised temps. */
  if (((sp_streq(name, "seek") && (argc == 1 || argc == 2)) ||
       (sp_streq(name, "read") && argc == 1 && (ret == TY_POLY || ret == TY_STRING))) &&
      kwh < 0 && splat_a < 0) {
    int int_args = 1;
    for (int a = 0; a < argc; a++)
      if (atmp_ty[a] != TY_INT && atmp_ty[a] != TY_POLY) int_args = 0;
    if (int_args) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_SEEK_READ, -1, TY_UNKNOWN, PC_SAME);
      char iv[2][48];
      for (int a = 0; a < argc; a++) {
        if (atmp_ty[a] == TY_POLY) snprintf(iv[a], sizeof iv[a], "sp_poly_arg_i(_t%d)", atmp[a]);
        else snprintf(iv[a], sizeof iv[a], "_t%d", atmp[a]);
      }
      buf_puts(b, " case SP_BUILTIN_IO: ");
      if (ret == TY_POLY || ret == (sp_streq(name, "seek") ? TY_INT : TY_STRING))
        buf_printf(b, "_t%d = ", tr);
      if (sp_streq(name, "seek")) {
        if (ret == TY_POLY) buf_puts(b, "sp_box_int(");
        buf_printf(b, "sp_File_seek((sp_File *)_t%d.v.p, %s, %s)", tv, iv[0], argc == 2 ? iv[1] : "0");
      }
      else {
        if (ret == TY_POLY) buf_puts(b, "sp_box_nullable_str(");
        buf_printf(b, "sp_File_read_n((sp_File *)_t%d.v.p, %s)", tv, iv[0]);
      }
      if (ret == TY_POLY) buf_puts(b, ")");
      buf_puts(b, "; break;");
    }
  }
  if (is_unshift) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_UNSHIFT, -1, TY_UNKNOWN, PC_SAME);
    /* sp_poly_insert is the kind dispatch for a positional splice, so
       `unshift(a, b)` is a insert at 0 and b insert at 1 -- CRuby's order.
       Answers the receiver, like push. */
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY:");
    for (int a = 0; a < argc; a++) {
      if (nt_kind(nt, argv[a]) == NK_SplatNode) {
        int ti = ++g_tmp;
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " sp_poly_insert(_t%d, %d + _t%d, sp_PolyArray_get(_t%d, _t%d));",
                   ti, ti, atmp[a], ti, tv, a, ti, atmp[a], ti);
        continue;
      }
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
      Buf ab; memset(&ab, 0, sizeof ab);
      if (atmp_ty[a] == TY_POLY) buf_puts(&ab, tn);
      else emit_boxed_text(c, atmp_ty[a], tn, &ab);
      buf_printf(b, " sp_poly_insert(_t%d, %d, %s);", tv, a, ab.p ? ab.p : "sp_box_nil()");
      free(ab.p);
    }
    if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
    buf_puts(b, " break;");
  }
  if (is_push) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_PUSH, -1, TY_UNKNOWN, PC_SAME);
    /* The value is a builtin array: append each (boxed) arg via sp_poly_shl,
       which dispatches on the array kind. `push`/`<<`/`append` return the
       receiver, so yield it when the result is used (chained). */
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY: case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY:");
    for (int a = 0; a < argc; a++) {
      if (nt_kind(nt, argv[a]) == NK_SplatNode) {
        int ti = ++g_tmp;
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " sp_poly_shl(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                   ti, ti, atmp[a], ti, tv, atmp[a], ti);
        continue;
      }
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
      Buf ab; memset(&ab, 0, sizeof ab);
      if (atmp_ty[a] == TY_POLY) buf_puts(&ab, tn);
      else emit_boxed_text(c, atmp_ty[a], tn, &ab);
      buf_printf(b, " sp_poly_shl(_t%d, %s);", tv, ab.p ? ab.p : "sp_box_nil()");
      free(ab.p);
    }
    if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
    buf_puts(b, " break;");
    /* This branch is kept out of the shared default below, so a receiver
       no arm claimed left the result at its nil initializer: `nil.push(1)`
       answered nil with nothing raised (#4485). A Queue is the one other
       builtin with a push, and sp_poly_shl owns it. */
    buf_printf(b, " default: if (!(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_QUEUE))"
                  " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d));", tv, tv, name, tv);
    /* push with other than one argument on a queue: SizedQueue#push(obj,
       non_block) is one push with a flag, and a count the queue does not
       take is its ArgumentError, named by its own range */
    int q_flag = sp_streq(name, "push") && argc != 1 && splat_a < 0;
    if (q_flag) {
      int tq = ++g_tmp;
      buf_printf(b, " { sp_RbVal _t%d[%d] = {", tq, argc > 0 ? argc : 1);
      for (int a = 0; a < argc; a++) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
        buf_puts(b, a ? ", " : " ");
        if (atmp_ty[a] == TY_POLY) buf_puts(b, tn); else emit_boxed_text(c, atmp_ty[a], tn, b);
      }
      if (argc == 0) buf_puts(b, " sp_box_nil()");
      buf_printf(b, " }; sp_poly_queue_push_n(_t%d, %d, _t%d); }", tv, argc, tq);
    }
    for (int a = 0; a < argc && !q_flag; a++) {
      if (nt_kind(nt, argv[a]) == NK_SplatNode) {
        int ti = ++g_tmp;
        buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                      " sp_poly_shl(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                   ti, ti, atmp[a], ti, tv, atmp[a], ti);
        continue;
      }
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
      Buf ab; memset(&ab, 0, sizeof ab);
      if (atmp_ty[a] == TY_POLY) buf_puts(&ab, tn);
      else emit_boxed_text(c, atmp_ty[a], tn, &ab);
      buf_printf(b, " sp_poly_shl(_t%d, %s);", tv, ab.p ? ab.p : "sp_box_nil()");
      free(ab.p);
    }
    if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
    buf_puts(b, " break;");
  }
  if (is_ppack) {
    int upk = sp_streq(name, "unpack1");
    Buf pb2; memset(&pb2, 0, sizeof pb2);
    if (upk) { /* a String receiver: handled by the tag pre-arm above */ }
    else {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_PACK, -1, TY_UNKNOWN, PC_SAME);
      buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                  " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: ");
      /* the same boxed-argument read as the join arm above: pack's format
         is a String, and CRuby raises TypeError for anything else */
      if (atmp_ty[0] == TY_POLY)
        buf_printf(&pb2, "sp_poly_pack(_t%d, sp_poly_arg_str_chk(_t%d))", tv, atmp[0]);
      else
        buf_printf(&pb2, "sp_poly_pack(_t%d, _t%d)", tv, atmp[0]);
      buf_printf(b, "_t%d = ", tr);
      if (ret == TY_POLY) emit_boxed_text(c, TY_STRING, pb2.p ? pb2.p : "", b);
      else buf_puts(b, pb2.p ? pb2.p : "");
      buf_puts(b, "; break;");
    }
    free(pb2.p);
  }
  if (is_pjoin) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_JOIN_N, -1, TY_UNKNOWN, PC_SAME);
    /* every array kind joins through the same runtime helper; the separator
       is the call's own argument (absent means "") */
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: ");
    Buf jb; memset(&jb, 0, sizeof jb);
    buf_printf(&jb, "sp_poly_join(_t%d, ", tv);
    /* a BOXED separator has to be read as a String here: the temp is an
       sp_RbVal and the slot is a const char *. sp_poly_arg_str_chk names
       CRuby's TypeError for a value that is not one; nil is the separator
       CRuby does accept, and it means "" (#4319). */
    if (argc >= 1 && atmp_ty[0] == TY_POLY)
      buf_printf(&jb, "(_t%d.tag == SP_TAG_NIL ? sp_str_empty : sp_poly_arg_str_chk(_t%d))",
                 atmp[0], atmp[0]);
    else if (argc >= 1) buf_printf(&jb, "_t%d", atmp[0]);
    else buf_puts(&jb, "sp_str_empty");
    buf_puts(&jb, ")");
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_STRING, jb.p ? jb.p : "", b);
    else buf_puts(b, jb.p ? jb.p : "");
    buf_puts(b, "; break;");
    free(jb.p);
  }
  if (is_include) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INCLUDE_CASES, -1, TY_UNKNOWN, PC_SAME);
    /* The builtin arms answer a C bool. When a user class's own include?
       answers something else the call widened to poly (#4072), so the
       accumulator is an sp_RbVal and these have to box. */
    const char *ibo = (ret == TY_POLY) ? "sp_box_bool(" : "";
    const char *ibc = (ret == TY_POLY) ? ")" : "";
    /* a user Enumerable read out of a container answers from its elements;
       -1 means "not one", and the arms below still decide (#3761) */
    { Buf ab5; memset(&ab5, 0, sizeof ab5);
      char tn5[24]; snprintf(tn5, sizeof tn5, "_t%d", atmp[0]);
      if (atmp_ty[0] == TY_POLY) buf_puts(&ab5, tn5);
      else emit_boxed_text(c, atmp_ty[0], tn5, &ab5);
      /* not a user Enumerable -> the answer the switch used to fall
         through to (false), so no other receiver changes */
      /* nil, a number and a boolean have no include?/key?/member? at
         all: they are not user Enumerables either, so the -1 read as
         false (#4485). A String took the tag arm ahead of the switch. */
      buf_printf(b, " default: { sp_poly_coll_chk(_t%d, \"%s\");"
                    " int _ui%d = sp_poly_user_include(_t%d, %s);"
                    " _t%d = %s_ui%d > 0%s; break; }",
                 tv, name, tv, tv, ab5.p ? ab5.p : "sp_box_nil()", tr, ibo, tv, ibc);
      free(ab5.p); }
    TyKind at = infer_type(c, argv[0]);
    switch (at) {
    case TY_INT:
      buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_include((sp_Range *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = %ssp_frange_cover(*(sp_FloatRange *)_t%d.v.p, (sp_float)_t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      break;
    case TY_FLOAT:
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_cover_f((sp_Range *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = %ssp_frange_cover(*(sp_FloatRange *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      break;
    case TY_RATIONAL: case TY_BIGINT: {
      /* a Rational or a Bignum compares against a numeric Range's bounds
         (an end written as a Float as written), where the call fell to the
         default arm and answered false */
      char tn6[24]; snprintf(tn6, sizeof tn6, "_t%d", atmp[0]);
      Buf ab6; memset(&ab6, 0, sizeof ab6); emit_boxed_text(c, at, tn6, &ab6);
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_cover_poly((sp_Range *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, ab6.p ? ab6.p : "sp_box_nil()", ibc);
      buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = %ssp_frange_cover_poly(*(sp_FloatRange *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, ab6.p ? ab6.p : "sp_box_nil()", ibc);
      free(ab6.p);
      break;
    }
    case TY_STRING:
      buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_include((sp_StrArray *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_STR_INT_HASH: _t%d = %ssp_StrIntHash_has_key((sp_StrIntHash *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_STR_STR_HASH: _t%d = %ssp_StrStrHash_has_key((sp_StrStrHash *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = %ssp_StrPolyHash_has_key((sp_StrPolyHash *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      break;
    case TY_SYMBOL:
      /* sym array is stored as IntArray (sp_sym == sp_int) */
      buf_printf(b, " case SP_BUILTIN_SYM_ARRAY: _t%d = %ssp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = %ssp_SymPolyHash_has_key((sp_SymPolyHash *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      break;
    case TY_POLY:
      /* promote: the include? arg widened to poly. A Range receiver
         (`case x when Range; x.include?(n)`) tests numeric membership, so
         unbox the arg; the PolyArray/PolyPolyHash arms below cover the
         container cases. Typed arrays match only when the boxed arg's tag
         fits the element type (a Set difference against an Array literal
         reaches these; a mismatched tag is simply not a member). */
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_cover_poly((sp_Range *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = %ssp_frange_cover_poly(*(sp_FloatRange *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      /* a boxed nil is the typed array's sentinel, which the search
         finds wherever a nil was stored */
      buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %s(_t%d.tag == SP_TAG_INT || _t%d.tag == SP_TAG_NIL) &&"
                    " sp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d.tag == SP_TAG_NIL ? SP_INT_NIL : _t%d.v.i)%s; break;",
                 tr, ibo, atmp[0], atmp[0], tv, atmp[0], atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %s(_t%d.tag == SP_TAG_FLT || _t%d.tag == SP_TAG_NIL) &&"
                    " sp_FloatArray_include((sp_FloatArray *)_t%d.v.p, _t%d.tag == SP_TAG_NIL ? sp_float_nil() : _t%d.v.f)%s; break;",
                 tr, ibo, atmp[0], atmp[0], tv, atmp[0], atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %s_t%d.tag == SP_TAG_STR && sp_StrArray_include((sp_StrArray *)_t%d.v.p, _t%d.v.s)%s; break;", tr, ibo, atmp[0], tv, atmp[0], ibc);
      break;
    case TY_NIL:
      /* an Integer or Float array holds nil as its sentinel */
      buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_IntArray_include((sp_IntArray *)_t%d.v.p, SP_INT_NIL)%s; break;", tr, ibo, tv, ibc);
      buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_include((sp_FloatArray *)_t%d.v.p, sp_float_nil())%s; break;", tr, ibo, tv, ibc);
      break;
    default: break;
    }
    /* PolyArray: box the arg for runtime comparison */
    {
      int tbox = ++g_tmp;
      buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: { sp_RbVal _t%d = ", tbox);
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
      emit_boxed_text(c, at, tn, b);
      /* a pointer array compares through its boxed elements (#4486) */
      buf_printf(b, "; _t%d = %ssp_PolyArray_include(sp_poly_to_poly_array(_t%d), _t%d)%s; break; }", tr, ibo, tv, tbox, ibc);
    }
    /* Every hash kind the arms above did not claim: the key boxed and
       looked up by the storage's key kind (a key of another kind is
       simply absent). Only the general hash had an arm, so a
       Symbol-keyed hash read out of a nested literal answered
       `key?(k)` false for a boxed k, whatever it held. */
    {
      int tbox = ++g_tmp;
      buf_puts(b, " case SP_BUILTIN_POLY_POLY_HASH:");
      if (at != TY_STRING)
        buf_puts(b, " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:");
      if (at != TY_SYMBOL) buf_puts(b, " case SP_BUILTIN_SYM_POLY_HASH:");
      buf_puts(b, " case SP_BUILTIN_INT_INT_HASH: case SP_BUILTIN_INT_STR_HASH:");
      buf_printf(b, " { sp_RbVal _t%d = ", tbox);
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
      emit_boxed_text(c, at, tn, b);
      buf_printf(b, "; _t%d = %ssp_poly_has_key(_t%d, _t%d)%s; break; }", tr, ibo, tv, tbox, ibc);
    }
  }
  if (is_arr_index && argc == 1) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ARR_INDEX, -1, TY_UNKNOWN, PC_SAME);
    int tix = ++g_tmp;
    Buf ab4; memset(&ab4, 0, sizeof ab4);
    { char tn4[32]; snprintf(tn4, sizeof tn4, "_t%d", atmp[0]);
      if (atmp_ty[0] == TY_POLY) buf_puts(&ab4, tn4);
      /* emit_boxed_text boxes a pattern temp as nil */
      else if (atmp_ty[0] == TY_REGEX) buf_printf(&ab4, "sp_box_regexp(%s)", tn4);
      else emit_boxed_text(c, atmp_ty[0], tn4, &ab4); }
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_SYM_ARRAY:"
                " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: {");
    buf_printf(b, " sp_int _t%d = sp_poly_arr_index_val(_t%d, %s, %d); _t%d = ",
               tix, tv, ab4.p ? ab4.p : "sp_box_nil()",
               sp_streq(name, "rindex") ? 1 : 0, tr);
    if (ret == TY_INT) buf_printf(b, "_t%d", tix);
    else buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tix, tix);
    buf_puts(b, "; break; }");
    /* an Enumerator's find_index walks it only as far as the hit */
    if (sp_streq(name, "find_index")) {
      int tix2 = ++g_tmp;
      buf_printf(b, " case SP_BUILTIN_ENUMERATOR: { sp_int _t%d = sp_enum_find_index_val(_t%d, %s); _t%d = ",
                 tix2, tv, ab4.p ? ab4.p : "sp_box_nil()", tr);
      if (ret == TY_INT) buf_printf(b, "_t%d", tix2);
      else buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tix2, tix2);
      buf_puts(b, "; break; }");
    }
    free(ab4.p);
  }
  if (is_intersect) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INTERSECT, -1, TY_UNKNOWN, PC_SAME);
    TyKind at2 = infer_type(c, argv[0]);
    char abox[96];
    if (at2 == TY_POLY) snprintf(abox, sizeof abox, "_t%d", atmp[0]);
    else {
      Buf ab2; memset(&ab2, 0, sizeof ab2);
      char tn2[32]; snprintf(tn2, sizeof tn2, "_t%d", atmp[0]);
      emit_boxed_text(c, at2, tn2, &ab2);
      snprintf(abox, sizeof abox, "%s", ab2.p ? ab2.p : "sp_box_nil()");
      free(ab2.p);
    }
    buf_puts(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_STR_ARRAY:"
                " case SP_BUILTIN_FLT_ARRAY: case SP_BUILTIN_SYM_ARRAY:"
                " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: "); 
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, "sp_box_bool(");
    buf_printf(b, "sp_poly_intersect_p(_t%d, %s)", tv, abox);
    if (ret == TY_POLY) buf_puts(b, ")");
    buf_puts(b, "; break;");
  }
  /* strftime on a poly value that is really a Time: format it; nil or any
     other runtime class raises NoMethodError as CRuby does. */
  if (is_strftime) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STRFTIME, -1, TY_UNKNOWN, PC_SAME);
    if (ret == TY_POLY)
      buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_box_str(sp_time_strftime(*(sp_Time *)_t%d.v.p, _t%d)); break;", tr, tv, atmp[0]);
    else
      buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_time_strftime(*(sp_Time *)_t%d.v.p, _t%d); break;", tr, tv, atmp[0]);
    buf_printf(b, " default: sp_raise_cls(\"NoMethodError\", sp_nomethod_msg(\"strftime\", _t%d)); break;", tv);
  }
  /* the poly value may actually be a string-keyed hash: dispatch `[]` /
     `fetch` to the matching hash storage, boxing the value into the poly
     result. */
  if ((is_aref || is_fetch) && infer_type(c, argv[0]) == TY_STRING) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_AREF_STR, -1, TY_UNKNOWN, PC_SAME);
    TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;  /* the result temp's type */
    static const struct { const char *cls, *hn; TyKind vt; } HV[] = {
      {"SP_BUILTIN_STR_STR_HASH", "StrStr", TY_STRING},
      {"SP_BUILTIN_STR_INT_HASH", "StrInt", TY_INT},
      {"SP_BUILTIN_STR_POLY_HASH", "StrPoly", TY_POLY},
    };
    for (unsigned hvi = 0; hvi < sizeof HV / sizeof HV[0]; hvi++) {
      /* only a variant whose value fits the result temp can be emitted */
      if (ret != TY_POLY && HV[hvi].vt != trt) continue;
      char getx[200];
      snprintf(getx, sizeof getx, "sp_%sHash_get((sp_%sHash *)_t%d.v.p, _t%d)", HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
      /* `[]` answers the hash's default on a miss, which the storage's
         own read already knows: the has_key gate below is fetch's, and
         in front of `[]` it answered nil for a Hash.new("") (#5544) */
      if (is_aref) {
        char gx[96]; snprintf(gx, sizeof gx, "sp_poly_get_str(_t%d, _t%d)", tv, atmp[0]);
        buf_printf(b, " case %s: _t%d = ", HV[hvi].cls, tr);
        if (ret == TY_POLY) buf_puts(b, gx); else emit_unbox_text(c, trt, gx, b);
        buf_puts(b, "; break;");
        continue;
      }
      buf_printf(b, " case %s: _t%d = sp_%sHash_has_key((sp_%sHash *)_t%d.v.p, _t%d) ? ",
                 HV[hvi].cls, tr, HV[hvi].hn, HV[hvi].hn, tv, atmp[0]);
      if (ret == TY_POLY) emit_boxed_text(c, HV[hvi].vt, getx, b); else buf_puts(b, getx);
      buf_puts(b, " : ");
      if (is_fetch) emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, trt, b);
      else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, trt));
      buf_puts(b, "; break;");
    }
    /* the poly value may be a generic PolyPolyHash keyed by (boxed) strings
       -- a `to_h { |x| [x.name, x] }` result widened to poly, indexed by a
       string (doom's `@flats[anim_flat(name)]`). The STR_*_HASH arms above
       only match native-string-keyed storage, so box the string key and
       look it up in the poly-keyed storage. Only for `[]` (nil on miss);
       `fetch` keeps falling through to its default-seed (a bare get would
       drop the caller's supplied default). */
    /* `[]` returns nil on a miss; `fetch` must return the present value or
       fall back to its supplied default / raise KeyError -- so gate the get
       on a has_key check (a bare get would drop the caller's default and
       mistake a stored nil for absence). */
    if (is_aref || is_fetch) {
      char getx[220], hx[220];
      snprintf(getx, sizeof getx, "sp_PolyPolyHash_get((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(_t%d))", tv, atmp[0]);
      snprintf(hx, sizeof hx, "sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(_t%d))", tv, atmp[0]);
      buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = ", tr);
      if (is_fetch) buf_printf(b, "%s ? ", hx);
      if (ret == TY_POLY) buf_puts(b, getx);
      else emit_unbox_text(c, trt, getx, b);
      if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, trt, b); }
      buf_puts(b, "; break;");
    }
  }
  /* a symbol-keyed hash (`{ name: ... }`) reaches here as SymPolyHash; add
     its `[]` / `fetch` arm so a Hash receiver indexed by a symbol is not
     dropped when a user class also defines an instance `[]` (#1437). */
  if ((is_aref || is_fetch) && infer_type(c, argv[0]) == TY_SYMBOL) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_AREF_SYM, -1, TY_UNKNOWN, PC_SAME);
    TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;
    char getx[200];
    snprintf(getx, sizeof getx, "sp_SymPolyHash_get((sp_SymPolyHash *)_t%d.v.p, _t%d)", tv, atmp[0]);
    buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = sp_SymPolyHash_has_key((sp_SymPolyHash *)_t%d.v.p, _t%d) ? ", tr, tv, atmp[0]);
    if (ret == TY_POLY) buf_puts(b, getx);
    else if (trt == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getx);
    else if (trt == TY_FLOAT) buf_printf(b, "sp_poly_to_f_or_nil(%s)", getx);
    else if (trt == TY_INT) buf_printf(b, "sp_poly_to_i_or_nil(%s)", getx);
    else buf_printf(b, "sp_poly_to_i(%s)", getx);
    buf_puts(b, " : ");
    if (is_fetch) emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, trt, b);
    else buf_puts(b, ret == TY_POLY ? "sp_box_nil()" : default_value_from_compiler(c, trt));
    buf_puts(b, "; break;");
    /* a symbol key against generic poly-keyed storage: an empty `{}`
       literal boxes as PolyPolyHash, so a symbol-keyed [] / fetch must
       reach it too (the arm above only matches SymPolyHash) */
    {
      char getx2[220], hx2[220];
      snprintf(getx2, sizeof getx2, "sp_PolyPolyHash_get((sp_PolyPolyHash *)_t%d.v.p, sp_box_sym(_t%d))", tv, atmp[0]);
      snprintf(hx2, sizeof hx2, "sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, sp_box_sym(_t%d))", tv, atmp[0]);
      buf_printf(b, " case SP_BUILTIN_POLY_POLY_HASH: _t%d = ", tr);
      if (is_fetch) buf_printf(b, "%s ? ", hx2);
      if (ret == TY_POLY) buf_puts(b, getx2);
      else emit_unbox_text(c, trt, getx2, b);
      if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, trt, b); }
      buf_puts(b, "; break;");
    }
  }
  /* a poly-keyed `[]` on a poly value that is actually a Hash: dispatch to
     the hash storage by the (boxed) poly key. The string/symbol-key arms
     above only fire for a statically-typed key; a key that stayed poly
     (a method param, e.g. `@textures[name]` in doom's TextureManager, where
     @textures = result[:textures] widened the Hash to a poly local) has no
     static key type, so without this arm the receiver switch fell through
     every Hash cls_id and returned nil. */
  /* fetch mirrors `[]` here: same runtime storage kinds, but gated on a
     has_key check so a present key returns its value while an absent key
     falls back to the supplied default / raises KeyError (a bare
     sp_poly_index_poly returns nil on a miss, which fetch must not do). */
  /* A TY_UNKNOWN key is held boxed-as-poly (atmp_ty == TY_POLY above), so
     it flows through sp_poly_index_poly / sp_poly_has_key exactly like an
     explicit poly key -- cover it here so a Hash reached by such a key is
     not dropped to nil (gemini review). */
  if ((is_aref || is_fetch) &&
      (infer_type(c, argv[0]) == TY_POLY || infer_type(c, argv[0]) == TY_UNKNOWN)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_AREF_POLY, -1, TY_UNKNOWN, PC_SAME);
    TyKind ptrt = is_scalar_ret(ret) ? ret : TY_INT;
    buf_puts(b, " case SP_BUILTIN_STR_POLY_HASH: case SP_BUILTIN_POLY_POLY_HASH:"
                " case SP_BUILTIN_SYM_POLY_HASH: case SP_BUILTIN_STR_STR_HASH:"
                " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_INT_STR_HASH:");
    char gx[64], hx[64]; snprintf(gx, sizeof gx, "sp_poly_index_poly(_t%d, _t%d)", tv, atmp[0]);
    snprintf(hx, sizeof hx, "sp_poly_has_key(_t%d, _t%d)", tv, atmp[0]);
    buf_printf(b, " _t%d = ", tr);
    if (is_fetch) buf_printf(b, "%s ? ", hx);
    if (ret == TY_POLY) buf_puts(b, gx);
    else emit_unbox_text(c, ptrt, gx, b);
    if (is_fetch) { buf_puts(b, " : "); emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, ptrt, b); }
    buf_puts(b, "; break;");
  }
  /* eql?/equal?/is_a?/kind_of?/instance_of? on a builtin-scalar (or
     un-overridden object) poly value: the switch default answers via the
     universal predicate, with the (boxed) argument reused from atmp. */
  if (is_pred) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_PRED_N, -1, TY_UNKNOWN, PC_SAME);
    char tvref[24]; snprintf(tvref, sizeof tvref, "_t%d", tv);
    Buf ab; memset(&ab, 0, sizeof ab);
    if (atmp_ty[0] == TY_POLY) buf_printf(&ab, "_t%d", atmp[0]);
    else { char at[24]; snprintf(at, sizeof at, "_t%d", atmp[0]); emit_boxed_text(c, atmp_ty[0], at, &ab); }
    const char *argref = ab.p ? ab.p : "sp_box_nil()";
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) { buf_puts(b, "sp_box_bool("); emit_poly_pred_value(c, id, tvref, argref, b); buf_puts(b, ")"); }
    else emit_poly_pred_value(c, id, tvref, argref, b);
    buf_puts(b, "; break;");
    free(ab.p);
  }
}

/* The numeric surface a poly receiver answers for ITSELF, when the poly
   method dispatch exists only because a user class happens to own the name.
   Each row is a name whose ordinary emitter declines a contested name (its
   own `!user_defines_or_reads` guard) and falls through to the dispatch,
   where the switch's default arm has to answer for a receiver that really is
   a number.

   Three separate fixes each copied the arm beside the last one -- the
   `ob13` / `ob14` / `ob15` numbering is what that looks like in the end --
   so a fourth name is one row here instead.

   The table ALSO gated whether the switch opened at all, because a
   colliding class may define the name at an arity the call site does not
   use, which leaves the candidate count 0 and every other flag false. That
   was the wrong place to answer it, and the gate below answers it properly
   now by asking whether any user class owns the NAME, which is the question
   the emitters themselves ask when they stand down. So this is a table of
   arms only. Reading it as a gate had swallowed `str.start_with?`,
   `arr.flatten`, `h.default` and a dozen more that no numeric row was ever
   going to cover.

   What the rows differ in is exactly three things: which runtime helper
   answers, how the argument reaches it, and how its answer is fitted to the
   slot the dispatch assigns into. */
typedef enum {
  NPA_BOXED,       /* the helper answers sp_RbVal: straight into a poly slot, unboxed into a scalar one */
  NPA_PAIR,        /* ... and answers a PAIR, which no scalar slot can hold: divmod, gcdlcm */
  NPA_FLOAT,       /* sp_float */
  NPA_INT_ARRAY,   /* sp_IntArray * */
  NPA_BOOL         /* sp_bool */
} NumArmKind;

static const struct {
  const char *nm;
  int min_argc, max_argc;
  const char *fn;     /* the helper at min_argc */
  const char *fn2;    /* ... and the one a second argument selects (pow's modulus) */
  NumArmKind kind;
  int arg_int;        /* the argument goes over as a machine int, not boxed (digits' base) */
  int extra;          /* a trailing constant the helper takes, -1 for none */
  TyKind box_as;      /* for NPA_PAIR / NPA_INT_ARRAY: what the poly slot boxes it as */
} SP_NUM_ARM[] = {
  {"gcd",       1, 1, "sp_poly_int_gcd",     NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"lcm",       1, 1, "sp_poly_int_lcm",     NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"ceildiv",   1, 1, "sp_poly_int_ceildiv", NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"gcdlcm",    1, 1, "sp_poly_int_gcdlcm",  NULL,                  NPA_PAIR,      0, -1, TY_POLY_ARRAY},
  {"pow",       1, 2, "sp_poly_pow",         "sp_poly_int_powmod",  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"digits",    1, 1, "sp_poly_int_digits",  NULL,                  NPA_INT_ARRAY, 1, -1, TY_INT_ARRAY},
  {"allbits?",  1, 1, "sp_poly_int_bits_test", NULL,                NPA_BOOL,      0,  0, TY_UNKNOWN},
  {"anybits?",  1, 1, "sp_poly_int_bits_test", NULL,                NPA_BOOL,      0,  1, TY_UNKNOWN},
  {"nobits?",   1, 1, "sp_poly_int_bits_test", NULL,                NPA_BOOL,      0,  2, TY_UNKNOWN},
  {"divmod",    1, 1, "sp_poly_divmod",      NULL,                  NPA_PAIR,      0, -1, TY_UNKNOWN},
  {"remainder", 1, 1, "sp_poly_remainder",   NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"fdiv",      1, 1, "sp_poly_fdiv",        NULL,                  NPA_FLOAT,     0, -1, TY_UNKNOWN},
  {"quo",       1, 1, "sp_poly_quo",         NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"modulo",    1, 1, "sp_poly_modulo",      NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {"div",       1, 1, "sp_poly_div_m",       NULL,                  NPA_BOXED,     0, -1, TY_UNKNOWN},
  {NULL, 0, 0, NULL, NULL, NPA_BOXED, 0, -1, TY_UNKNOWN}
};

int poly_num_arm(const char *name, int argc) {
  if (!name) return -1;
  for (int i = 0; SP_NUM_ARM[i].nm; i++)
    if (sp_streq(name, SP_NUM_ARM[i].nm) &&
        argc >= SP_NUM_ARM[i].min_argc && argc <= SP_NUM_ARM[i].max_argc) return i;
  return -1;
}

/* The `default:` arm of a poly dispatch with arguments: for a switch of
   user-class arms alone, the generic one (`replace`, the numeric surface,
   String#index, find_index, then the builtin surface or the raise); for a
   name a builtin answers by its own kind, that name's (first(n), delete,
   dig, values_at, merge, `[]` and fetch). */
void emit_poly_defaults_n(Compiler *c, int id, int recv, const char *name, const PolySpecialsN *ps,
                          const PolyTemps *T, const PolyKw *kw, int splat_a, int is_aref, int is_aref2,
                          int is_fetch, int blk_tmp2, int is_setter_val, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc;
  const int *argv = T->argv, *atmp = T->atmp;
  const TyKind *atmp_ty = T->atmp_ty;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  int is_pred = ps->pred, is_strftime = ps->strftime, is_include = ps->include, is_push = ps->push;
  int is_cover = ps->cover, is_gcdlcm = ps->gcdlcm, is_strdel = ps->strdel, is_strsplit = ps->strsplit;
  int is_pdelete = ps->pdelete, is_pdig = ps->pdig, is_pvalues_at = ps->pvalues_at;
  int is_pfirstn = ps->pfirstn, is_pmerge = ps->pmerge, is_arr_index = ps->arr_index;
  /* Same fallthrough rule as the zero-arg dispatch, but only when the
     switch is made of user-class arms alone. Where a builtin pre-arm is in
     play the fallthrough can mean "right receiver, wrong argument" --
     `"abc".include?(:x)` is a TypeError in CRuby, not a NoMethodError --
     so those names keep their existing answer rather than gain a
     mislabelled raise (#3394). */
  if (!is_pred && !is_strftime && !is_aref && !is_aref2 && !is_fetch && !is_include &&
      !is_push && !is_cover && !is_gcdlcm && !is_strdel && !is_strsplit &&
      !is_pdelete && !is_pdig && !is_pvalues_at && !is_pfirstn && !is_pmerge) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_GENERIC, -1, TY_UNKNOWN, PC_SAME);
    buf_puts(b, " default:");
    size_t dl_pos = b->len;   /* the builtin fallback below reads it */
    /* `replace` reaches this dispatch only because a user class owns the
       name; a String, Array or Hash receiver still has to be replaced
       rather than told it has no such method. Same shape as the to_i /
       to_h / join arms of the zero-argument dispatch (#4240). */
    if (sp_streq(name, "replace") && argc == 1 && splat_a < 0 && ret == TY_POLY) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_REPLACE, -1, TY_UNKNOWN, PC_SAME);
      Buf rb9; memset(&rb9, 0, sizeof rb9);
      { char tn9[32]; snprintf(tn9, sizeof tn9, "_t%d", atmp[0]);
        if (atmp_ty[0] == TY_POLY) buf_puts(&rb9, tn9);
        else emit_boxed_text(c, atmp_ty[0], tn9, &rb9); }
      buf_printf(b, " _t%d = sp_poly_replace_any(_t%d, %s); break;",
                 tr, tv, rb9.p ? rb9.p : "sp_box_nil()");
      free(rb9.p);
    }
    /* round(n) / ceil(n) / floor(n) / truncate(n): a class defining
       `round(digits)` took over the name, and the Integer or Float in
       the same slot fell to the raise below (#4532). The zero-arg forms
       have their arm in the other dispatch; these answer through the
       boxed helpers the no-user-class path uses. */
    else if (is_round_family(name) && argc == 1 && splat_a < 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_ROUND, -1, TY_UNKNOWN, PC_SAME);
      char nd9[64];
      if (atmp_ty[0] == TY_POLY) snprintf(nd9, sizeof nd9, "sp_poly_arg_i(_t%d)", atmp[0]);
      else snprintf(nd9, sizeof nd9, "(sp_int)_t%d", atmp[0]);
      Buf nv9; memset(&nv9, 0, sizeof nv9);
      if (sp_streq(name, "round")) buf_printf(&nv9, "sp_poly_round_n(_t%d, %s)", tv, nd9);
      else buf_printf(&nv9, "sp_poly_prec_n(_t%d, %s, %s)", tv, nd9,
                      name[0] == 'c' ? "SP_PREC_CEIL" : name[0] == 'f' ? "SP_PREC_FLOOR" : "SP_PREC_TRUNC");
      buf_printf(b, " _t%d = ", tr);
      if (ret == TY_POLY) buf_puts(b, nv9.p ? nv9.p : "");
      else emit_unbox_text(c, ret, nv9.p ? nv9.p : "", b);
      buf_puts(b, "; break;");
      free(nv9.p);
    }
    /* Integer-only arithmetic a user class can shadow the same way
       : a class defining `gcd`/`lcm`/`ceildiv`/`pow`/`digits`/
       `allbits?` etc. took over the name, and the Integer or Bignum in
       the same slot fell to the raise below with no arm of its own --
       the same gap `bit_length`, `round(n)` and friends had. The
       runtime helpers are the ones the no-user-class path already calls
       (codegen_call_recv.c's `sp_poly_int_*` table), and they raise for
       a receiver that is not a number, as CRuby does. */
    /* The numeric surface, from the table beside emit_poly_method_dispatch:
       box the argument the dispatch already hoisted, call the helper the
       no-user-class path calls, and fit the answer to the slot. */
    else if (splat_a < 0 && poly_num_arm(name, argc) >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_NUM, -1, TY_UNKNOWN, PC_SAME);
      int ai = poly_num_arm(name, argc);
      Buf ab9; memset(&ab9, 0, sizeof ab9);
      { char an9[32]; snprintf(an9, sizeof an9, "_t%d", atmp[0]);
        if (SP_NUM_ARM[ai].arg_int) {
          if (atmp_ty[0] == TY_POLY) buf_printf(&ab9, "sp_poly_arg_i(_t%d)", atmp[0]);
          else buf_printf(&ab9, "(sp_int)_t%d", atmp[0]);
        }
        else if (atmp_ty[0] == TY_POLY) buf_puts(&ab9, an9);
        else emit_boxed_text(c, atmp_ty[0], an9, &ab9); }
      const char *a9 = ab9.p ? ab9.p : "sp_box_nil()";
      Buf bb9; memset(&bb9, 0, sizeof bb9);
      const char *fn9 = SP_NUM_ARM[ai].fn;
      if (argc > SP_NUM_ARM[ai].min_argc && SP_NUM_ARM[ai].fn2) {
        fn9 = SP_NUM_ARM[ai].fn2;
        char bn9[32]; snprintf(bn9, sizeof bn9, "_t%d", atmp[1]);
        if (atmp_ty[1] == TY_POLY) buf_puts(&bb9, bn9);
        else emit_boxed_text(c, atmp_ty[1], bn9, &bb9);
      }
      char gv9[320];
      if (bb9.p)
        snprintf(gv9, sizeof gv9, "%s(_t%d, %s, %s)", fn9, tv, a9, bb9.p);
      else if (SP_NUM_ARM[ai].extra >= 0)
        snprintf(gv9, sizeof gv9, "%s(_t%d, %s, %d)", fn9, tv, a9, SP_NUM_ARM[ai].extra);
      else
        snprintf(gv9, sizeof gv9, "%s(_t%d, %s)", fn9, tv, a9);
      /* A pair fits a poly slot and an array one, and nothing else: a
         scalar slot cannot hold it, so the arm raises there rather than
         build a value it would have to throw away. Asking the slot rather
         than asking only whether it is poly is the fourth instance of
         this family -- `n.gcdlcm(8)` raised "undefined method 'gcdlcm'"
         whenever the dispatch had typed the call from the builtin answer
         alone, which is every program where the colliding class's own
         arity keeps it out of the candidate set. */
      if (SP_NUM_ARM[ai].kind == NPA_PAIR &&
          ret != TY_POLY && ret != TY_POLY_ARRAY && ret != TY_INT_ARRAY)
        buf_printf(b, " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
      else {
        buf_printf(b, " _t%d = ", tr);
        switch (SP_NUM_ARM[ai].kind) {
          case NPA_BOXED:
            if (ret == TY_POLY) buf_puts(b, gv9);
            else emit_unbox_text(c, ret, gv9, b);
            break;
          case NPA_PAIR: {
            /* box_as names the pointer kind the helper answers directly;
               without one it answers an sp_RbVal already. */
            int raw9 = SP_NUM_ARM[ai].box_as != TY_UNKNOWN;
            if (ret == TY_POLY) {
              if (raw9) emit_boxed_text(c, SP_NUM_ARM[ai].box_as, gv9, b);
              else buf_puts(b, gv9);
            }
            else if (raw9 && ret == SP_NUM_ARM[ai].box_as) buf_puts(b, gv9);
            else emit_unbox_text(c, ret, gv9, b);
            break; }
          case NPA_FLOAT:
            if (ret == TY_POLY) emit_boxed_text(c, TY_FLOAT, gv9, b);
            else buf_puts(b, gv9);
            break;
          case NPA_INT_ARRAY:
            if (ret == TY_POLY) emit_boxed_text(c, TY_INT_ARRAY, gv9, b);
            else buf_puts(b, gv9);
            break;
          case NPA_BOOL:
            if (ret == TY_POLY) buf_printf(b, "sp_box_bool(%s)", gv9);
            else buf_puts(b, gv9);
            break;
        }
        buf_puts(b, "; break;");
      }
      free(ab9.p); free(bb9.p);
    }
    /* index/rindex also belong to String, whose box carries no cls_id, so
       no case above can claim it. Answer it here, ahead of the raise, or a
       substring search on a boxed String reports a missing method (#3445).
       find_index is Enumerable-only and keeps falling through. */
    else if (is_arr_index && !sp_streq(name, "find_index")) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_STR_INDEX, -1, TY_UNKNOWN, PC_SAME);
      int tsi = ++g_tmp;
      Buf ab5; memset(&ab5, 0, sizeof ab5);
      { char tn5[32]; snprintf(tn5, sizeof tn5, "_t%d", atmp[0]);
        if (atmp_ty[0] == TY_POLY) buf_puts(&ab5, tn5);
        else if (atmp_ty[0] == TY_REGEX) buf_printf(&ab5, "sp_box_regexp(%s)", tn5);
        else emit_boxed_text(c, atmp_ty[0], tn5, &ab5); }
      if (argc == 2) {
        char sx[64];
        if (atmp_ty[1] == TY_POLY) snprintf(sx, sizeof sx, "sp_poly_arg_i(_t%d)", atmp[1]);
        else snprintf(sx, sizeof sx, "(sp_int)_t%d", atmp[1]);
        buf_printf(b, " if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) {"
                      " sp_int _t%d = sp_poly_str_index_from_val(_t%d, %s, %s, %d); _t%d = ",
                   tv, tv, tsi, tv, ab5.p ? ab5.p : "sp_box_nil()", sx,
                   sp_streq(name, "rindex") ? 1 : 0, tr);
        /* the result slot carries whatever the call was inferred as: the
           nullable int rides raw (SP_INT_NIL is its nil), a poly slot boxes */
        if (ret == TY_POLY)
          buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tsi, tsi);
        else buf_printf(b, "_t%d", tsi);
        buf_puts(b, "; break; }");
      }
      else {
        buf_printf(b, " if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) {"
                      " sp_int _t%d = sp_poly_str_index_val(_t%d, %s, %d); _t%d = ",
                   tv, tv, tsi, tv, ab5.p ? ab5.p : "sp_box_nil()",
                   sp_streq(name, "rindex") ? 1 : 0, tr);
        if (ret == TY_INT) buf_printf(b, "_t%d", tsi);
        else buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tsi, tsi);
        buf_puts(b, "; break; }");
      }
      free(ab5.p);
    }
    /* find_index(value) over the other Enumerables: a Hash's pairs, a
       Range's members, an Enumerator's values, a user Enumerable's
       elements. Anything else still reports the missing method. */
    else if (is_arr_index && argc == 1) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_FIND_INDEX, -1, TY_UNKNOWN, PC_SAME);
      int tsi = ++g_tmp;
      Buf ab7; memset(&ab7, 0, sizeof ab7);
      { char tn7[32]; snprintf(tn7, sizeof tn7, "_t%d", atmp[0]);
        if (atmp_ty[0] == TY_POLY) buf_puts(&ab7, tn7);
        else emit_boxed_text(c, atmp_ty[0], tn7, &ab7); }
      buf_printf(b, " { sp_int _t%d; if (sp_poly_enum_find_index_val(_t%d, %s, &_t%d)) { _t%d = ",
                 tsi, tv, ab7.p ? ab7.p : "sp_box_nil()", tsi, tr);
      if (ret == TY_INT) buf_printf(b, "_t%d", tsi);
      else buf_printf(b, "(_t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d))", tsi, tsi);
      buf_puts(b, "; break; } }");
      free(ab7.p);
    }
    /* a receiver no arm above claimed may still be a builtin that answers
       the name (Array#shift(n) under a user shift(n), #4831): ask the
       builtin surface before raising. Only where the default is still
       open -- nothing after the label yet, or only guarded arms, which
       end in `}`. A name arm above that already wrote the default's
       whole body (`gcd`'s ends `break;`) leaves what follows unreachable,
       and a builtin answer there was not even of the slot's type. */
    int dl_open = b->len == dl_pos || (b->len > 0 && b->p[b->len - 1] == '}');
    int sum_done = dl_open && ret == TY_POLY &&
                   emit_poly_default_blk_arm(c, id, name, argc, argv, atmp, atmp_ty, tv, tr, blk_tmp2, b);
    int answer = sum_done ? 1 : 0;
    if (!sum_done && (!dl_open ||
        (!(answer = emit_poly_aset_default(c, name, argc, atmp, atmp_ty, ret, tv, tr, b) ? 2 : 0) &&
         !(answer = emit_poly_builtin_default(c, id, recv, name, argc, argv, atmp, atmp_ty,
                                              ret, tv, is_setter_val ? -1 : tr, 0, b) ? 3 : 0))))
      buf_printf(b, " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
    if (g_plan_check && dl_open) pa_observe(PA_TRIAL, PA_KEY_TRIAL + PT_GENERIC_TAIL, -1, TY_UNKNOWN, answer);
  }
  /* `[]` gets a default of its own. The arms above enumerate the kinds
     someone thought to add -- every ARRAY kind, the string- and
     symbol-keyed hashes -- and a receiver of any other kind matched
     nothing, leaving the result at its nil initializer: a read that
     answered nil with nothing raised (#3507). The runtime index dispatches
     on the receiver's own kind, and raises where there is no `[]` at all.
     The key goes boxed, since a Hash key is not an offset. */
  else if (is_pfirstn) {
    char nx[64]; snprintf(nx, sizeof nx, "_t%d", atmp[0]);
    char gen[256];
    snprintf(gen, sizeof gen, "sp_poly_%s_n(_t%d, %s)",
             sp_streq(name, "first") ? "first" : "last", tv,
             atmp_ty[0] == TY_POLY ? ({ static char cx[80]; snprintf(cx, sizeof cx, "sp_poly_arg_i(%s)", nx); cx; }) : nx);
    if (ret == TY_POLY) {
      buf_printf(b, " default: _t%d = %s; break;", tr, gen);
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_FIRSTN, -1, TY_UNKNOWN, PC_SAME);
    }
  }
  else if (is_pdelete || is_pdig || is_pvalues_at) {
    /* A splatted key list has a length only the run time knows, so the
       fixed `(sp_RbVal[]){...}` cannot hold it -- every argument temp
       carried the whole array as ONE key and the call answered from the
       first (#4164). Flatten into a PolyArray and hand the callee its
       buffer instead. */
    int splat_n = 0;
    for (int a = 0; a < argc; a++)
      if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode")) splat_n = 1;
    int tkl = -1;
    if (splat_n && !is_pdelete) {
      tkl = ++g_tmp;
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tkl, tkl);
      for (int a = 0; a < argc; a++) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
        if (nt_type(nt, argv[a]) && sp_streq(nt_type(nt, argv[a]), "SplatNode")) {
          int tsp = ++g_tmp, tsj = ++g_tmp;
          buf_printf(b, " sp_PolyArray *_t%d = sp_poly_to_poly_array(", tsp);
          if (atmp_ty[a] == TY_POLY) buf_puts(b, tn);
          else emit_boxed_text(c, atmp_ty[a], tn, b);
          buf_printf(b, "); SP_GC_ROOT(_t%d);", tsp);
          buf_printf(b, " for (sp_int _t%d = 0; _t%d < sp_PolyArray_length(_t%d); _t%d++)"
                        " sp_PolyArray_push(_t%d, sp_PolyArray_get(_t%d, _t%d));",
                     tsj, tsj, tsp, tsj, tkl, tsp, tsj);
          continue;
        }
        buf_printf(b, " sp_PolyArray_push(_t%d, ", tkl);
        if (atmp_ty[a] == TY_POLY) buf_puts(b, tn);
        else emit_boxed_text(c, atmp_ty[a], tn, b);
        buf_puts(b, ");");
      }
    }
    Buf ab; memset(&ab, 0, sizeof ab);
    for (int a = 0; a < argc; a++) {
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
      if (a) buf_puts(&ab, ", ");
      if (atmp_ty[a] == TY_POLY) buf_puts(&ab, tn);
      else emit_boxed_text(c, atmp_ty[a], tn, &ab);
    }
    char gen[512];
    /* delete(key) { |k| }: the block answers a key that was not there */
    int dblk = is_pdelete && nt_ref(nt, id, "block") >= 0 ? poly_call_blk_proc(c, id, blk_tmp2) : -1;
    if (is_pdelete && dblk >= 0)
      snprintf(gen, sizeof gen, "sp_poly_delete_key_blk(_t%d, %s, _t%d)", tv, ab.p ? ab.p : "sp_box_nil()", dblk);
    else if (is_pdelete)
      snprintf(gen, sizeof gen, "sp_poly_delete_key(_t%d, %s)", tv, ab.p ? ab.p : "sp_box_nil()");
    else if (tkl >= 0)
      snprintf(gen, sizeof gen, "sp_poly_%s(_t%d, _t%d->len, _t%d->data)",
               is_pdig ? "dig_n" : "values_at_n", tv, tkl, tkl);
    else
      snprintf(gen, sizeof gen, "sp_poly_%s(_t%d, %d, (sp_RbVal[]){%s})",
               is_pdig ? "dig_n" : "values_at_n", tv, argc, ab.p ? ab.p : "sp_box_nil()");
    /* Only when the result temp is the boxed one. The generic answer is
       whatever the receiver's own kind returns, and a switch whose result
       was typed from the user arm alone has no room for it: unboxing an
       Array into a `const char *` slot is worse than the raise. */
    if (ret == TY_POLY) {
      buf_printf(b, " default: _t%d = %s; break;", tr, gen);
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_KEYS, -1, TY_UNKNOWN, PC_SAME);
    }
    free(ab.p);
  }
  else if (is_pmerge) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_MERGE, -1, TY_UNKNOWN, PC_SAME);
    /* the operand is either a positional hash or the collapsed keywords */
    Buf mb; memset(&mb, 0, sizeof mb);
    if (pos_argc >= 1) {
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
      if (atmp_ty[0] == TY_POLY) buf_puts(&mb, tn);
      else emit_boxed_text(c, atmp_ty[0], tn, &mb);
    }
    else {
      emit_kwh_pos_hash(c, kw, 1, &mb);
    }
    char gen[600];
    snprintf(gen, sizeof gen, "sp_box_obj(sp_poly_hash_merge(_t%d, %s), SP_BUILTIN_POLY_POLY_HASH)",
             tv, mb.p ? mb.p : "sp_box_nil()");
    if (ret == TY_POLY) buf_printf(b, " default: _t%d = %s; break;", tr, gen);
    /* Never leave the switch without a default: with every user arm
       dropped as incompatible, an armless switch fell through and the
       call answered the result temp's zero initializer, silently. */
    else buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
    free(mb.p);
  }
  /* `x[a, b]` beside a user class's own two-argument `[]`: a String or
     Array receiver slices, a Proc or bound Method is called, as the
     dispatch-free form does (#5522). */
  else if (is_aref2) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_AREF2, -1, TY_UNKNOWN, PC_SAME);
    Buf ab2; memset(&ab2, 0, sizeof ab2);
    for (int a = 0; a < 2; a++) {
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
      if (a) buf_puts(&ab2, ", ");
      if (atmp_ty[a] == TY_POLY) buf_puts(&ab2, tn);
      else emit_boxed_text(c, atmp_ty[a], tn, &ab2);
    }
    char gen[400];
    snprintf(gen, sizeof gen, "sp_poly_slice_or_call(_t%d, %s)", tv, ab2.p ? ab2.p : "sp_box_nil(), sp_box_nil()");
    if (ret == TY_POLY) buf_printf(b, " default: _t%d = %s; break;", tr, gen);
    else { buf_printf(b, " default: _t%d = ", tr);
           emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, gen, b);
           buf_puts(b, "; break;"); }
    free(ab2.p);
  }
  else if (is_aref || is_fetch) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_AREF, -1, TY_UNKNOWN, PC_SAME);
    Buf kb; memset(&kb, 0, sizeof kb);
    { char keyt[64]; snprintf(keyt, sizeof keyt, "_t%d", atmp[0]);
      if (atmp_ty[0] == TY_POLY) buf_puts(&kb, keyt);
      else emit_boxed_text(c, atmp_ty[0], keyt, &kb); }
    Buf db; memset(&db, 0, sizeof db);
    if (is_fetch && argc == 2) {
      char dn[64]; snprintf(dn, sizeof dn, "_t%d", atmp[1]);
      if (atmp_ty[1] == TY_POLY) buf_puts(&db, dn);
      else emit_boxed_text(c, atmp_ty[1], dn, &db);
    }
    char gen[400];
    if (is_fetch)
      snprintf(gen, sizeof gen, "sp_poly_fetch(_t%d, %s, %d, %s)", tv,
               kb.p ? kb.p : "sp_box_nil()", argc == 2,
               db.p ? db.p : "sp_box_nil()");
    else
      snprintf(gen, sizeof gen, "sp_poly_index_poly(_t%d, %s)", tv, kb.p ? kb.p : "sp_box_nil()");
    if (ret == TY_POLY) buf_printf(b, " default: _t%d = %s; break;", tr, gen);
    else { buf_printf(b, " default: _t%d = ", tr);
           emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, gen, b);
           buf_puts(b, "; break;"); }
    free(kb.p); free(db.p);
  }
}

/* The last `default:` a poly dispatch writes: the builtin surface's answer
   for a receiver no arm took, asked by re-entering the call (a trial), and
   for the zero-argument dispatch (done 0) the raise when it declines; done
   -1 is the dispatch with arguments, which leaves the switch as it is. */
void emit_poly_last_default(Compiler *c, int id, int recv, const char *name, int argc, const int *argv,
                            const int *atmp, const TyKind *atmp_ty, TyKind ret, int tv, int tr, int done,
                            Buf *b) {
  if (done > 0) return;
  int kept = emit_poly_builtin_default(c, id, recv, name, argc, argv, atmp, atmp_ty, ret, tv, tr, 1, b);
  if (g_plan_check)
    pa_observe(PA_TRIAL, PA_KEY_TRIAL + (done < 0 ? PT_DEFAULT_N : PT_DEFAULT0), -1, TY_UNKNOWN, kept);
  if (!kept && done == 0)
    buf_printf(b, " default: sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
}

/* Does a call on a poly receiver re-enter as an Array call, its elements
   materialized (#2935, #4290)? 1 for the sort_by, comparator and grouping
   forms, 2 for the block forms that answer the receiver (each_slice,
   each_cons, the repeated pair), 0 for any other. */
int poly_redispatch_kind(Compiler *c, int id, const char *name, int argc) {
  const NodeTable *nt = c->nt;
  int takes = (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "sort_by")) ||
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
         sp_streq(name, "combination") || sp_streq(name, "permutation") ||
         sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation"))) ||
       /* and their BLOCK forms, which had no arm of their own and fell to the
          loud NoMethodError -- the array emitters they re-dispatch to serve
          the block and the blockless shape alike. */
       (nt_ref(nt, id, "block") >= 0 && argc == 1 && !user_defines_or_reads(c, name) &&
        (sp_streq(name, "zip") ||
         /* the repeated pair, which the array emitters serve as they do
            combination's; the boxed receiver had no arm and raised */
         sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation") ||
         /* each_slice / each_cons answer the receiver, and the wrapper that
            hands it back re-enters this node -- which a pending safe-nav guard
            re-enters too, and the two do not compose: the inner pass finds no
            emitter and bakes a NoMethodError whose argument does not even
            typecheck. Leave the guarded shape on its existing path (it raises
            at run time, as it did before) rather than failing the build. */
         sp_streq(name, "each_cons") || sp_streq(name, "each_slice")));
  if (!takes) return 0;
  /* `each_slice(n) { }` / `each_cons(n) { }` answer the RECEIVER, and for a
     Hash that is the hash itself, not the pairs the re-dispatch materializes
     from it. Bind the receiver once, materialize from that binding, and hand
     the binding back -- the same shape emit_iter_value_expr uses for a
     receiver rewritten to `__enum_to_a`. */
  int ret_recv = (nt_ref(nt, id, "block") >= 0 &&
                  (sp_streq(name, "each_cons") || sp_streq(name, "each_slice") ||
                   sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation")));
  return ret_recv ? 2 : 1;
}

/* The receiver form a poly dispatch takes: the plan's (form) when the
   plan answers the dispatch's result type (served), else the dispatch's
   own reading (oform); --plan-check holds the two against each other. */
unsigned poly_form_check(int id, const char *name, const char *site, int served, unsigned form, unsigned oform) {
  if (!served) {
    if (g_plan_check) fprintf(stderr, "plan-check: cplan-fallback: %s node %d %s\n", site, id, name);
    return oform;
  }
  if (g_plan_check) {
    cplan_served(site);
    if (form != oform)
      fprintf(stderr, "plan-check: cplan-conflict: %s node %d %s: plan %#x, read %#x\n", site, id, name,
              form, oform);
  }
  return form;
}
