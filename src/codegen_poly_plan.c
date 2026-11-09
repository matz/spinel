/* codegen_poly_plan.c -- the poly dispatch's arm families, moved out of
   emit_poly_method_dispatch (codegen_call.c), and the --plan-check shadow
   that holds the arms it writes against the resolver's (cplan_poly,
   call_plan.c; #7100 Phase C). */

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "repr.h"
#include "call_plan.h"
#include "share.h"
#include "codegen_call_arms.h"

/* ---- --plan-check: the arms one emitted switch wrote ----
   A frame per switch being written; a nested dispatch (an argument's
   default) opens its own. A probe that longjmps out of a frame leaves it
   behind, and the enclosing pa_end drops it with its own. */
typedef struct { int id; TyKind box_ty; unsigned flags; int n, cap; PolyArm *arm; } PaFrame;
static PaFrame *g_pa;
static int g_pa_n, g_pa_cap;
static long g_pa_compared, g_pa_arms, g_pa_conflict, g_pa_missing, g_pa_extra;
static long g_pa_trial_kept, g_pa_trial_dropped;

/* (codegen_internal.h) the kinds sp_poly_enum_proc walks, and a Float Range,
   which it answers with the TypeError a walk of one raises (without it the
   Range fell to the user-class switch's NoMethodError) */
void emit_penum_kind_test(Buf *b, int tv) {
  buf_printf(b, "_t%d.tag == SP_TAG_OBJ && (sp_poly_is_array_kind(_t%d.cls_id) || sp_poly_is_hash_kind(_t%d.cls_id) || "
                "_t%d.cls_id == SP_BUILTIN_RANGE || _t%d.cls_id == SP_BUILTIN_STR_RANGE || _t%d.cls_id == SP_BUILTIN_ENUMERATOR || "
                "_t%d.cls_id == SP_BUILTIN_FLOAT_RANGE)",
             tv, tv, tv, tv, tv, tv, tv);
}

int pa_begin(int id) {
  if (g_pa_n == g_pa_cap) {
    int ncap = g_pa_cap ? g_pa_cap * 2 : 8;
    g_pa = realloc(g_pa, (size_t)ncap * sizeof *g_pa);
    memset(g_pa + g_pa_cap, 0, (size_t)(ncap - g_pa_cap) * sizeof *g_pa);
    g_pa_cap = ncap;
  }
  PaFrame *f = &g_pa[g_pa_n];
  f->id = id; f->n = 0; f->flags = 0; f->box_ty = TY_UNKNOWN;
  return g_pa_n++;
}

int pa_current(void) { return g_pa_n - 1; }

void pa_resume(int frame) {
  if (frame >= 0 && frame < g_pa_n) g_pa_n = frame + 1;
}

void pa_drop(int frame) {
  if (frame >= 0 && frame < g_pa_n) g_pa_n = frame;
}

void pa_flags(unsigned flags) {
  if (g_pa_n > 0) g_pa[g_pa_n - 1].flags = flags | PPF_SEEN;
}

/* The last text box is the arm result. Record its actual representation,
   including boxes made by a shared-return helper before pa_observe. */
void pa_box_text(TyKind ty) {
  if (g_pa_n > 0) g_pa[g_pa_n - 1].box_ty = ty;
}

void pa_observe(int kind, int key, int mi, TyKind vty, int conv) {
  if (g_pa_n <= 0) return;
  PaFrame *f = &g_pa[g_pa_n - 1];
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->arm = realloc(f->arm, (size_t)f->cap * sizeof *f->arm);
  }
  if (conv == PC_BOX && vty == TY_STRING && f->box_ty == TY_STRBUF) conv = PC_BOX_HANDLE;
  f->box_ty = TY_UNKNOWN;
  PolyArm *a = &f->arm[f->n++];
  a->kind = (unsigned char)kind; a->key = (short)key; a->mi = mi;
  a->vty = (unsigned char)vty; a->conv = (unsigned char)conv; a->def = -1;
}

void pa_observe_at(int id, int kind, int key, int mi, TyKind vty, int conv) {
  if (g_pa_n > 0 && g_pa[g_pa_n - 1].id == id) pa_observe(kind, key, mi, vty, conv);
}

static const char *pa_kind_name(int k) {
  static const char *const nm[] = { "user", "proc-form", "reader", "native", "arity", "visibility", "synth-enum",
                                    "struct-set", "builtin", "trial" };
  return k >= 0 && k < (int)(sizeof nm / sizeof nm[0]) ? nm[k] : "?";
}

static void pa_arm_text(Compiler *c, const PolyArm *a, char *out, size_t n) {
  static const char *const fam[] = { "len", "empty", "class-named", "class-reflect", "ostruct", "to_a",
                                     "io-rewind", "io-puts", "io-zero", "reduce", "int-chr", "str-transform",
                                     "split", "enum-proc", "synchronize", "callable", "class-members",
                                     "len-cases", "clear", "empty-cases", "compare_by_identity?",
                                     "default enum-each", "default to_s", "default case-conv", "default numeric",
                                     "default digits", "default array-transform", "default predicate",
                                     "default to_i/to_f", "default any?/none?", "default to_h", "default display",
                                     "default hash",
                                     "each_index", "join", "alive?", "kill", "status", "queue", "io-read",
                                     "io-flush", "io-close", "enum-to_a",
                                     "cover?", "try_convert", "gcdlcm", "unpack1", "include?", "str-delete",
                                     "str-partition", "str-setop", "store", "str-encode", "str-split",
                                     "int-bitref", "index-cases", "io-read_nonblock", "io-readpartial", "io-readpartial(buf)",
                                     "io-setsockopt", "io-write",
                                     "io-syswrite", "io-print", "io-putc", "io-seek/read", "unshift", "push",
                                     "pack", "join(sep)", "include?-cases", "array-index", "intersect?",
                                     "strftime", "aref-str", "aref-sym", "aref-poly", "predicate(arg)",
                                     "default generic", "default replace", "default round(n)",
                                     "default numeric(n)", "default str-index", "default find_index",
                                     "default upto/downto",
                                     "default first/last(n)", "default delete/dig/values_at",
                                     "default merge", "default aref2", "default aref/fetch",
                                     "block map!", "subclass re-entry", "array re-entry",
                                     "array re-entry (receiver)", "face str-bang", "io-timeout",
                                     "enum-inline" };
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
  /* PC_BOX of a String writes sp_box_str. A method promising a shared
     return must retain its published handle across this boxing boundary.
     PC_BOX_HANDLE records that emitted alternative to the plain box. */
  for (int a = 0; a < f->n; a++) {
    const PolyArm *o = &f->arm[a];
    if (o->mi <= 0 || o->mi >= c->nscopes || o->vty != TY_STRING) continue;
    int mi = o->kind == PA_PROC_FORM ? scope_proc_form_of(c, o->mi) : o->mi;
    if (mi < 0) continue;
    if (o->conv == PC_BOX && c->scopes[mi].ret_handle)
      fprintf(stderr, "plan-check: channel-box-conflict: node %d %s arm %d method %s: "
              "emitted sp_box_str, predicted published handle\n",
              f->id, nm ? nm : "?", o->key, c->scopes[mi].name);
    if (g_repr_check && o->conv == PC_SAME) repr_channel_call(c, f->id, mi);
  }
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
      int conv = ca->conv == PC_BOX_HANDLE ? PC_BOX : ca->conv;
      if (pa->kind != ca->kind || pa->mi != ca->mi || pa->vty != ca->vty || pa->conv != conv) {
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

/* A user arm boxes the String its return facts describe. A shared return
   publishes its handle; a fresh return needs its own handle only when the
   call's value is shared. The nil and fresh tails use the same handover as
   the deep-return pickup, so an earlier publication cannot replace them.
   A fresh result a builtin only reads, or the caller drops, keeps no alias. */
void emit_poly_user_box(Compiler *c, int id, Scope *m, const char *call, Buf *b) {
  if (m->ret != TY_STRING ||
      (!m->ret_handle && !(m->ret_fresh && !an_node_dropped(c, id)))) {
    emit_boxed_ret_call(c, m, call, b);   /* an oint return boxes with its nil */
    return;
  }
  Buf hb; memset(&hb, 0, sizeof hb);
  int t = ++g_tmp;
  buf_puts(&hb, "({ ");
  if (m->ret_handle) buf_puts(&hb, "_sp_ret_strbuf = NULL; ");
  buf_printf(&hb, "const char *_t%d = %s; ", t, call);
  if (m->ret_nil_pickup) buf_printf(&hb, "!_t%d ? NULL : ", t);
  if (m->ret_handle) buf_puts(&hb, "_sp_ret_strbuf ? (sp_String *)_sp_ret_strbuf : ");
  str_new_handle_open(&hb);
  buf_printf(&hb, "_t%d", t);
  str_new_handle_close(&hb);
  buf_puts(&hb, "; })");
  emit_boxed_text(c, TY_STRBUF, hb.p, b);
  free(hb.p);
}

/* Does the default a zero-argument call passes for parameter ai make a new
   object: an empty rest's Array, an empty **kwrest's Hash, or a default
   expression that allocates? */
static int zero_arg_default_allocates(Compiler *c, Scope *ds, int ai) {
  if (ai == ds->rest_idx || ai == ds->kwrest_idx) return 1;
  return ds->pdefault && ds->pdefault[ai] >= 0 && subtree_may_allocate(c->nt, ds->pdefault[ai]);
}

/* Does a default after the first run something (`def z(a = @n, b = (@n
   += 1))`)? Made in the argument list, its effect could land ahead of an
   earlier default's read; bound in order (ctor_arm_arg), a reads 1 first. */
static int later_default_runs(Compiler *c, Scope *ds) {
  for (int ai = 1; ds->pdefault && ai < ds->nparams; ai++)
    if (ds->pdefault[ai] >= 0 && subtree_has_side_effect(c, ds->pdefault[ai])) return 1;
  return 0;
}

/* The defaults of a zero-argument call's parameters, `, d0, d1...` into cb.
   Each is made in the C argument list, so a new object one makes (an empty
   rest's Array, an empty **kwrest's Hash) was freed by a later default's
   allocation: such a default, with a later one that allocates, is made into
   a rooted temp in `pre`, ahead of the call, and the call reads the temp.
   So is every one when the callee makes a captured local's cell as it
   enters, which it does before it roots the parameters (emit_rest_held). */
static void emit_zero_arg_defaults(Compiler *c, Scope *ds, Buf *cb, Buf *pre) {
  int later = 0, cells = 0;
  for (int ai = 0; ai < ds->nparams; ai++) later += zero_arg_default_allocates(c, ds, ai);
  for (int i = 0; i < ds->nlocals && !cells; i++) cells = ds->locals[i].is_cell;
  if (cells) later++;
  for (int ai = 0; ai < ds->nparams; ai++) {
    int allocates = zero_arg_default_allocates(c, ds, ai);
    later -= allocates;
    const char *pnm = ds->pnames ? ds->pnames[ai] : NULL;
    LocalVar *pv = pnm ? scope_local(ds, pnm) : NULL;
    TyKind pt = pv && pv->type != TY_UNKNOWN ? pv->type : TY_POLY;
    buf_puts(cb, ", ");
    if (!allocates || !later || !needs_root(pt)) {
      emit_arg_or_default(c, ds, ai, -1, cb);
      continue;
    }
    Buf d; memset(&d, 0, sizeof d);
    emit_arg_or_default(c, ds, ai, -1, &d);
    int t = ++g_tmp;
    emit_ctype(c, pt, pre);
    buf_printf(pre, " _t%d = %s; ", t, d.p ? d.p : "");
    buf_printf(pre, pt == TY_POLY ? "SP_GC_ROOT_RBVAL(_t%d); " : "SP_GC_ROOT(_t%d); ", t);
    buf_printf(cb, "_t%d", t);
    free(d.p);
  }
}

/* One user-class arm of a zero-argument poly dispatch, as the plan (or the
   decision above) gives it: `case k:` writing the result temp _t<tr> from
   receiver _t<tv>, the arm's value fitted to the call's type ret. */
/* The zero-argument dispatch's result slot holds its nil beside the value
   (an sp_oint / sp_ofloat, declared so by the dispatcher) where the call's
   consumer wants that form: an arm's plain value is wrapped as never nil,
   a boxed one unboxed with its nil, an oint one assigned as it is. */
static int plan_slot_oint(Compiler *c, int id, TyKind ret) { return is_scalar_ret(ret) && oint_kind(ret) && node_is_oint(c, id); }
static void plan_put_plain(int so, TyKind ret, const char *text, Buf *b) { if (so) buf_printf(b, "%s(%s)", oint_of(ret), text); else buf_puts(b, text); }
static void plan_put_boxed(Compiler *c, int so, TyKind slotty, const char *text, Buf *b) { if (so) buf_printf(b, "%s(%s)", oint_unbox(slotty), text); else emit_unbox_text(c, slotty, text, b); }

/* An arm whose method answers an instance of a String subclass (a
   SafeBuffer's `def to_s; self; end`) into a String slot: the slot takes the
   instance's bytes, as a String slot reads such an instance anywhere else
   (emit_obj_conv), or the instance itself as the handle, since its struct
   begins with its String. The call's type is the String the other arms
   answer, and the raw struct pointer did not build. 0 when the pair is not
   that one. */
static int plan_put_strsub(Compiler *c, TyKind ret, TyKind mret, const char *call, Buf *b) {
  if (!ty_is_object(mret) || !comp_ary_is_str(c, ty_object_class(mret))) return 0;
  if (ret == TY_STRING) buf_printf(b, "sp_strbuf_read((sp_String *)(%s))", call);
  else if (ret == TY_STRBUF) buf_printf(b, "(sp_String *)(%s)", call);
  else return 0;
  return 1;
}

/* so: the call's result slot is an oint (plan_slot_oint), one slot for
   every arm */
/* Class k's struct text after its name, through the line of field
   iv_<f>, or NULL when the struct has no such field: two classes whose
   texts are equal lay out every field up to that one at the same offset.
   Kept per class and field: the structs are final once codegen runs, and a
   reader dispatch asks for each class of each site. */
static char **slp_key, **slp_val; static int slp_cap, slp_n, slp_nclasses = -1;
static const Compiler *slp_c;
static const char *struct_layout_prefix(Compiler *c, int k, const char *f) {
  if (slp_c != c || slp_nclasses != c->nclasses) {
    for (int i = 0; i < slp_cap; i++) { free(slp_key ? slp_key[i] : NULL); free(slp_val ? slp_val[i] : NULL); }
    free(slp_key); free(slp_val); slp_key = slp_val = NULL; slp_cap = slp_n = 0;
    slp_c = c; slp_nclasses = c->nclasses;
  }
  if (slp_n * 2 >= slp_cap) {
    int ocap = slp_cap; char **ok = slp_key, **ov = slp_val;
    slp_cap = ocap ? ocap * 2 : 64;
    slp_key = calloc((size_t)slp_cap, sizeof *slp_key); slp_val = calloc((size_t)slp_cap, sizeof *slp_val);
    if (!slp_key || !slp_val) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int i = 0; i < ocap; i++) {
      if (!ok[i]) continue;
      unsigned j = sp_strhash(ok[i]) & (unsigned)(slp_cap - 1);
      while (slp_key[j]) j = (j + 1) & (unsigned)(slp_cap - 1);
      slp_key[j] = ok[i]; slp_val[j] = ov[i];
    }
    free(ok); free(ov);
  }
  char key[300];
  snprintf(key, sizeof key, "%d %s", k, f);
  unsigned j = sp_strhash(key) & (unsigned)(slp_cap - 1);
  while (slp_key[j]) {
    if (sp_streq(slp_key[j], key)) return slp_val[j];
    j = (j + 1) & (unsigned)(slp_cap - 1);
  }
  Buf sb; memset(&sb, 0, sizeof sb);
  emit_class_struct(c, &c->classes[k], &sb);
  char line[300];
  snprintf(line, sizeof line, " iv_%s;\n", f);
  const char *body = sb.p ? strchr(sb.p, '\n') : NULL, *at = body ? strstr(body, line) : NULL;
  char *v = at ? strndup(body, (size_t)(at + strlen(line) - body)) : NULL;
  free(sb.p);
  slp_key[j] = strdup(key); slp_val[j] = v; slp_n++;
  if (!slp_key[j]) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  return v;
}
/* The class whose struct a reader arm reads ivar @rn of class `def`
   through: the topmost ancestor that holds the ivar at the same index, of
   the same type and nil bit, with the struct laid out the same up to it
   (struct_layout_prefix). A hierarchy of subclasses inheriting one
   attr_reader then reads it through the same struct, and its arms are one
   (#10549). `def` itself when no ancestor qualifies. */
static int reader_layout_class(Compiler *c, int def, const char *rn) {
  if (def < 0 || def >= c->nclasses || !rn) return def;
  char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn);
  char f[256]; snprintf(f, sizeof f, "%s", iv_c(rn));
  ClassInfo *dc = &c->classes[def];
  int ivx = comp_ivar_index(dc, ivn);
  const char *mine = ivx >= 0 && !dc->is_native_class ? struct_layout_prefix(c, def, f) : NULL;
  if (!mine) return def;
  char mbit[400] = "";
  int mnil = ivar_has_nilbit(c, def, ivx);
  if (mnil) ivar_nilbit_test(c, def, ivx, "o->", mbit, sizeof mbit);
  int best = def;
  for (int p = dc->parent, g = 0; p >= 0 && p < c->nclasses && g < 256; p = c->classes[p].parent, g++) {
    ClassInfo *pc = &c->classes[p];
    if (pc->is_native_class || comp_ivar_index(pc, ivn) != ivx || pc->ivar_types[ivx] != dc->ivar_types[ivx] ||
        ivar_has_nilbit(c, p, ivx) != mnil) break;
    if (mnil) {
      char pbit[400]; ivar_nilbit_test(c, p, ivx, "o->", pbit, sizeof pbit);
      if (!sp_streq(pbit, mbit)) break;
    }
    const char *theirs = struct_layout_prefix(c, p, f);
    if (!theirs || !sp_streq(theirs, mine)) break;
    best = p;
  }
  return best;
}
static void emit_poly_user_arm0(Compiler *c, int id, const char *name, TyKind ret, int tv, int tr,
                                int blk_tmp0, int so, const PolyArm *a, Buf *b) {
  int k = a->key, mi = a->mi;
  if (a->kind == PA_NATIVE || (a->kind == PA_ARITY && mi < 0)) {
    int nk = comp_native_binding_class(c, k, name);
    int nmi = comp_native_method_find(c, nk >= 0 ? nk : k, name, 0, 0);
    if (a->kind == PA_NATIVE) {
      NativeMethod *nmet = &c->native_methods[nmi];
      char nbuf[300], cbuf[240];
      const char *rest0 = nmet->rest ? ", 0, NULL" : "";
      /* --share-strings: a binding answering the String its object keeps,
         where the rule shares the call's String, publishes that String's
         handle, read through its handle form (repr_native_arm_handle), as
         a method arm's tail read does: the call's pickup takes it */
      if (repr_native_arm_handle(c, id, k)) {
        Buf hc, rb;
        memset(&hc, 0, sizeof hc);
        memset(&rb, 0, sizeof rb);
        buf_printf(&hc, "%s((%s *)_t%d.v.p)", nmet->share_csym, c->classes[nmet->class_id].c_struct, tv);
        str_read_handle(&rb, hc.p, 1);
        snprintf(cbuf, sizeof cbuf, "%s", rb.p);
        free(hc.p);
        free(rb.p);
      }
      else
        snprintf(cbuf, sizeof cbuf, "%s((%s *)_t%d.v.p%s)", nmet->csym, c->classes[nmet->class_id].c_struct, tv, rest0);
      if (sp_streq(nmet->ret, "string?"))
        snprintf(nbuf, sizeof nbuf, "SP_STR_BOX(%s)", cbuf);
      else
        snprintf(nbuf, sizeof nbuf, "%s", cbuf);
      TyKind mret = comp_native_ret_ty(c, nmi, k);
      buf_printf(b, " case %d: ", k);
      int pconv = PC_SAME;
      if (mret == TY_NIL) { buf_puts(b, nbuf); pconv = PC_VOID; }
      else {
        buf_printf(b, "_t%d = ", tr);
        if (ret == TY_POLY && mret != TY_POLY) { emit_boxed_text(c, mret, nbuf, b); pconv = PC_BOX; }
        else if (ret != TY_POLY && mret == TY_POLY) {
          if (so) buf_printf(b, "%s(%s)", oint_unbox(ret), nbuf);
          else emit_unbox_poly_ret(c, is_scalar_ret(ret) ? ret : TY_INT, nbuf, b);
          pconv = PC_UNBOX;
        }
        else plan_put_plain(so, ret, nbuf, b);
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
    Buf dpre; memset(&dpre, 0, sizeof dpre);   /* defaults held ahead of the call */
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
    else if (repr_self_handle(c, mi)) snprintf(_dself, sizeof _dself, "sp_poly_as_strbuf(_t%d)", tv);
    else if (sp_streq(_dcn, "String")) snprintf(_dself, sizeof _dself, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn, "Symbol")) snprintf(_dself, sizeof _dself, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn, "NilClass")) snprintf(_dself, sizeof _dself, "0");
    /* Object's (and Array's) methods take self boxed */
    else if (is_array_or_object_class(_dcn))
      snprintf(_dself, sizeof _dself, "_t%d", tv);
    else if (is_boolean_class_name(_dcn))
      snprintf(_dself, sizeof _dself, "(int)_t%d.v.b", tv);
    else if (sp_streq(_dcn, "NilClass")) snprintf(_dself, sizeof _dself, "0");
    else if (is_boolean_class_name(_dcn)) snprintf(_dself, sizeof _dself, "(int)_t%d.v.i", tv);
    else if (is_array_hash_or_object_class(_dcn))
      snprintf(_dself, sizeof _dself, "_t%d", tv);
    /* a boxed Time / Range points at the value; the reopen takes it by value */
    else if (is_range_or_time_class(_dcn))
      snprintf(_dself, sizeof _dself, sp_streq(_dcn, "Time") ? "(sp_%s *)_t%d.v.p" : "*(sp_%s *)_t%d.v.p", _dcn, tv);   /* a Time is a reference */
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
    buf_printf(&cb, "%s(%s", mc_method(c, &c->scopes[pfi9 >= 0 ? pfi9 : mi]), _dself);
    /* a default reading an earlier parameter (Thor's `say(message = "", color =
       nil, force_new_line = (message.to_s !~ ...))`) reads it through a local
       each argument binds to first, as the other arms do (ctor_arm_arg) */
    Buf pdpre; memset(&pdpre, 0, sizeof pdpre);
    /* the parameters are the called symbol's: a proc form's clone has
       them where a dispatcher's own def, which nothing calls, has none */
    Scope *cs9 = &c->scopes[pfi9 >= 0 ? pfi9 : mi];
    if (cs9->nparams > 0) {
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
      /* and their bare calls are the callee class's methods */
      int saved_cls = g_emitting_class_id;
      g_emitting_class_id = ds->class_id;
      /* a default reading an earlier parameter: each argument is bound to
         a rooted local in order (ctor_arm_arg), which a later default reads
         and which holds what an earlier default made; else the defaults go
         in the argument list, an allocating one held in a temp */
      int pd_uid = default_refs_earlier_param(c, ds) || later_default_runs(c, ds) ? ++g_tmp : 0, pd_base = g_nren;
      if (!pd_uid) {
        /* what a default hoists runs inside this arm, on its receiver:
           ahead of the switch it read a receiver not yet there */
        Buf *sv_pre9 = g_pre;
        g_pre = &pdpre;
        emit_zero_arg_defaults(c, ds, &cb, &dpre);
        g_pre = sv_pre9;
      }
      else {
        Buf *sv_pre9 = g_pre;
        g_pre = &pdpre;
        for (int ai = 0; ai < ds->nparams; ai++) {
          buf_puts(&cb, ", ");
          Buf vb; memset(&vb, 0, sizeof vb);
          emit_arg_or_default(c, ds, ai, -1, &vb);
          ctor_arm_arg(c, ds, ai, vb.p ? vb.p : "", pd_uid, &pdpre, &cb);
          free(vb.p);
        }
        g_nren = pd_base;
        g_pre = sv_pre9;
      }
      g_self = saved_self; g_self_deref = saved_deref; g_emitting_class_id = saved_cls;
    }
    /* self is always the first argument here, so a zero-param method
       still needs the separator the helper only adds after a real
       parameter list. */
    if (cs9->nparams == 0 && c->scopes[mi].blk_param &&
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
    if (dpre.len) {
      Buf w; memset(&w, 0, sizeof w);
      buf_printf(&w, "({ %s%s; })", dpre.p, cb.p ? cb.p : "");
      free(cb.p); cb = w;
    }
    free(dpre.p);
    const char *call = cb.p ? cb.p : "";
    buf_printf(b, " case %d: ", k);
    if (pdpre.p) buf_printf(b, "{ %s", pdpre.p);
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
      if (ret == TY_STRBUF && cret9 == TY_STRING && repr_boxed_to_s_operand(c, id) >= 0) {
        buf_puts(b, "sp_poly_as_strbuf(");
        emit_poly_user_box(c, id, &c->scopes[pf9 ? pfi9 : mi], call, b);
        buf_puts(b, ")");
      }
      /* a String answer where a reader's arm beside it answers its field's
         handle: the handle the method publishes, else one around the String
         (NULL for nil) */
      else if (ret == TY_STRBUF && cret9 == TY_STRING) {
        if (c->scopes[pf9 ? pfi9 : mi].ret_handle) {
          int pv = ++g_tmp;
          buf_printf(b, "({ _sp_ret_strbuf = NULL; __auto_type _v%d = (%s); !_v%d ? NULL : _sp_ret_strbuf ? "
                        "(sp_String *)_sp_ret_strbuf : ", pv, call, pv);
          str_new_handle_open(b);
          buf_printf(b, "_v%d", pv);
          str_new_handle_close(b);
          buf_puts(b, "; })");
        }
        else {
          str_new_handle_open(b);
          buf_puts(b, call);
          str_new_handle_close(b);
        }
      }
      else if (ret == TY_POLY && cret9 != TY_POLY) {
        emit_poly_user_box(c, id, &c->scopes[pf9 ? pfi9 : mi], call, b); pconv = PC_BOX;
      }
      /* a method answering a String's handle (`h["k"] = "v"`'s value)
         into a plain String slot: read through the handle, as a reader's
         handle field is below; assigned bare, the C did not build */
      else if (cret9 == TY_STRBUF && ret == TY_STRING) {
        str_read_handle(b, call, 1);
        pconv = PC_COPY;
      }
      /* The slot is scalar (e.g. a length dispatch fixed to sp_int) but
         this class's method widened its return to poly: coerce down. */
      else if (ret != TY_POLY && cret9 == TY_POLY) {
        if (so) buf_printf(b, "%s(%s)", oint_unbox(slotty), call);
        else emit_unbox_poly_ret(c, slotty, call, b);
        pconv = PC_UNBOX;
      }
      /* Two classes own the name and answer different types (one an
         Integer, another a Bignum): the slot took one of them, so the
         odd arm converts into it rather than emitting a type error. */
      else if (slotty == TY_INT && cret9 == TY_BIGINT) { if (so) buf_puts(b, "sp_oint_of("); buf_printf(b, "sp_bigint_to_int(%s)", call); if (so) buf_puts(b, ")"); pconv = PC_NUM; }
      else if (slotty == TY_FLOAT && cret9 == TY_BIGINT) { if (so) buf_puts(b, "sp_ofloat_of("); buf_printf(b, "sp_bigint_to_double(%s)", call); if (so) buf_puts(b, ")"); pconv = PC_NUM; }
      /* a callee that answers its nil beside the value hands the slot its own form */
      else if (so && method_ret_is_oint(&c->scopes[pf9 ? pfi9 : mi])) buf_puts(b, call);
      /* an oint answer into a plain slot: unwrapped */
      else if (method_ret_is_oint(&c->scopes[pf9 ? pfi9 : mi])) buf_printf(b, "%s(%s)", oint_arg(cret9), call);
      /* a String subclass instance into the String slot: its bytes (the
         plan records this pair as the plain store it was) */
      else if (plan_put_strsub(c, ret, cret9, call, b)) {}
      else plan_put_plain(so, ret, call, b);
    }
    buf_puts(b, pdpre.p ? "; } break;" : "; break;");
    free(cb.p); free(pdpre.p);
    if (g_plan_check) pa_observe(pf9 ? PA_PROC_FORM : PA_USER, k, mi, cret9, pconv);
    return;
  }
  /* a reader: the ivar of the class that holds the attr, read through the
     struct of the topmost ancestor that lays the field out the same way
     (reader_layout_class), so the arms of the subclasses that inherit it
     are one text and share one case (arm_texts_flush) */
  const char *rn3 = comp_resolve_alias(c, k, name);
  int rdcls = reader_layout_class(c, a->def, rn3);
  char fld[600];
  snprintf(fld, sizeof fld, "((sp_%s *)_t%d.v.p)->iv_%s", c->classes[rdcls].c_name, tv, iv_c(rn3));
  char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", rn3);
  int ivx = comp_ivar_index(&c->classes[rdcls], ivn);
  TyKind ivt = ivx >= 0 ? c->classes[rdcls].ivar_types[ivx] : TY_INT;
  buf_printf(b, " case %d: _t%d = ", k, tr);
  int pconv = PC_SAME;
  /* an Integer / Float ivar with a nil bit boxes as nil where its bit is
     set (#3288); a plain one boxes its value */
  if (ret == TY_POLY && oint_kind(ivt) && ivx >= 0 && ivar_has_nilbit(c, rdcls, ivx)) {
    char objp[200]; snprintf(objp, sizeof objp, "((sp_%s *)_t%d.v.p)->", c->classes[rdcls].c_name, tv);
    char bit[400]; ivar_nilbit_test(c, rdcls, ivx, objp, bit, sizeof bit);
    buf_printf(b, "(%s ? sp_box_nil() : %s(%s))", bit, ivt == TY_INT ? "sp_box_int" : "sp_box_float", fld);
    pconv = PC_BOX_OR_NIL;
  }
  /* an oint slot takes the field with its bit: nil where the bit is set */
  else if (so && oint_kind(ivt) && ivx >= 0 && ivar_has_nilbit(c, rdcls, ivx)) {
    char objp[200]; snprintf(objp, sizeof objp, "((sp_%s *)_t%d.v.p)->", c->classes[rdcls].c_name, tv);
    char bit[400]; ivar_nilbit_test(c, rdcls, ivx, objp, bit, sizeof bit);
    buf_printf(b, "(%s ? %s : %s(%s))", bit, oint_nil(ret), oint_of(ret), fld);
    pconv = PC_BOX_OR_NIL;
  }
  else if (ret == TY_POLY && ivt != TY_POLY) { emit_boxed_text(c, ivt, fld, b); pconv = PC_BOX; }
  /* The slot is scalar (e.g. a length dispatch fixed to sp_int) but
     this class's ivar widened to poly: coerce down. */
  else if (ret != TY_POLY && ivt == TY_POLY) {
    plan_put_boxed(c, so, is_scalar_ret(ret) ? ret : TY_INT, fld, b);
    pconv = PC_UNBOX;
  }
  /* shared-mutable ivar read into a plain string slot: the safe
     copy, not the raw handle pointer (#3227) */
  else if (ivt == TY_STRBUF && ret == TY_STRING) {
    /* (--share-strings: always published. A consumer that picks the
       handle up clears the channel before the call, so a publication no one
       picks up is harmless; asked here, repr_call_returns_handle reads the
       call under the pickup's own view, which has dropped the box mark, and
       answered no, so the pickup wrapped the bytes in a new handle and the
       field's String lost its identity: `{ host: req.host }` held a copy) */
    str_read_handle(b, fld, 1);
    pconv = PC_COPY;
  }
  /* a plain String field (a Struct member) where the call answers a handle
     (other classes' fields are handles): wrapped in a new one, as a String
     argument to a handle parameter is; assigned bare, the C did not build */
  else if (ivt == TY_STRING && ret == TY_STRBUF) {
    str_new_handle_open(b);
    buf_puts(b, fld);
    str_new_handle_close(b);
  }
  else plan_put_plain(so, ret, fld, b);
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

/* The arms of one dispatch, written apart and then put out with the classes
   whose arms are the same text sharing one body: subclasses that call the
   method they inherit. Each was its own copy of the call, and with the
   method forced inline its own copy of the method, ten at every
   `u.userinfo = v` on a URI, which held clang for half a minute on one file.
   An arm is shared only when it is its class's label and a body that leaves
   the switch, so no fallthrough moves. */
static const char *arm_text_body(const ArmText *a) {
  char lab[32];
  int ll = snprintf(lab, sizeof lab, " case %d: ", a->k);
  if (!a->t.p || strncmp(a->t.p, lab, (size_t)ll) != 0) return NULL;
  const char *body = a->t.p + ll;
  size_t n = strlen(body);
  while (n > 0 && (body[n - 1] == ' ' || body[n - 1] == '}')) n--;
  return n >= 6 && strncmp(body + n - 6, "break;", 6) == 0 ? body : NULL;
}

/* one more arm slot, for class k (grown as the arms are written, so a
   dispatch over a few classes allocates for a few) */
ArmText *arm_text_next(ArmText *a, int *n, int *cap, int k) {
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 8;
    a = realloc(a, sizeof *a * (size_t)*cap);
    if (!a) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  memset(&a[*n], 0, sizeof a[*n]);
  a[*n].k = k;
  (*n)++;
  return a;
}

/* The arms grouped by body through a hash table, each group put out where
   its first arm stood: one pass over the texts, not a comparison of every
   pair (a dispatch over many classes was quadratic in them, scale-test). */
void arm_texts_flush(ArmText *a, int n, Buf *b) {
  if (n <= 0) return;
  const char **body = malloc(sizeof *body * (size_t)n);
  size_t *len = malloc(sizeof *len * (size_t)n);
  int *lead = malloc(sizeof *lead * (size_t)n), *next = malloc(sizeof *next * (size_t)n), *tail = malloc(sizeof *tail * (size_t)n);
  int cap = 16; while (cap < 2 * n) cap <<= 1;
  int *tab = malloc(sizeof *tab * (size_t)cap);
  if (!body || !len || !lead || !next || !tail || !tab) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int q = 0; q < cap; q++) tab[q] = -1;
  for (int i = 0; i < n; i++) {
    next[i] = -1; tail[i] = i; lead[i] = i;
    body[i] = arm_text_body(&a[i]);
    if (!body[i]) continue;
    len[i] = strlen(body[i]);
    unsigned h = 2166136261u;
    for (size_t q = 0; q < len[i]; q++) h = (h ^ (unsigned char)body[i][q]) * 16777619u;
    for (unsigned q = h & (unsigned)(cap - 1);; q = (q + 1) & (unsigned)(cap - 1)) {
      int g = tab[q];
      if (g < 0) { tab[q] = i; break; }
      if (len[g] == len[i] && memcmp(body[g], body[i], len[i]) == 0) {
        lead[i] = g; next[tail[g]] = i; tail[g] = i;
        break;
      }
    }
  }
  for (int i = 0; i < n; i++) {
    if (lead[i] != i || !a[i].t.p) continue;
    if (!body[i]) { buf_puts(b, a[i].t.p); continue; }
    for (int m = i; m >= 0; m = next[m]) buf_printf(b, " case %d:", a[m].k);
    buf_printf(b, " %s", body[i]);
  }
  for (int i = 0; i < n; i++) { free(a[i].t.p); a[i].t.p = NULL; }
  free(body); free(len); free(lead); free(next); free(tail); free(tab);
}

/* Emit the zero-argument poly plan's user-class arms: a method or proc
   form, reader, native binding or arity refusal for each runtime class. */
void emit_poly_user_arms0(Compiler *c, int id, const char *name, int argc, TyKind ret, int tv, int tr,
                          int blk_tmp0, const PolyPlan *p, Buf *b) {
  /* the class arms, copied: an arm's emission may resolve another plan */
  PolyArm *arms = malloc(sizeof *arms * (size_t)(p->n > 0 ? p->n : 1));
  int n = 0;
  for (int i = 0; i < p->n; i++)
    if (p->arm[i].key >= 0 && p->arm[i].key < c->nclasses) arms[n++] = p->arm[i];
  if (g_plan_check) cplan_served("poly-user0");
  ArmText *at = calloc((size_t)(n > 0 ? n : 1), sizeof *at);
  int so = n > 0 && plan_slot_oint(c, id, ret);
  for (int i = 0; i < n; i++) {
    if (at) at[i].k = arms[i].key;
    emit_poly_user_arm0(c, id, name, ret, tv, tr, blk_tmp0, so, &arms[i], at ? &at[i].t : b);
  }
  if (at) arm_texts_flush(at, n, b);
  free(at);
  free(arms);
}

/* The block-taking Enumerable methods a class with an #each that mixes in
   Enumerable answers through Enumerable, for which the builtin walk of its
   elements (sp_poly_enum_proc over the elements its #to_a or #each gives)
   is the answer; NULL for another name: a Hash-only walk, a mutator, or a
   walk that answers the receiver (each_with_index, reverse_each), which
   the elements' Array would stand in for. */
static const char *poly_enum_includer_op(const char *name) {
  static const char *const names[] = {
    "map", "collect", "select", "filter", "find_all", "reject", "find", "detect",
    "find_index", "sort_by", "count", "sum", "any?", "all?", "none?", "uniq", "to_h",
    "min", "max", "min_by", "max_by", NULL };
  for (int i = 0; names[i]; i++) if (sp_streq(name, names[i])) return poly_enum_op_for(name);
  return NULL;
}

/* A zero-argument poly dispatch with a block that exists because a user
   class defines the name: the instances of a class that answers it through
   Enumerable (Set, a class of the program's with an #each that includes
   it) had no arm and fell to the NoMethodError. They walk the elements
   their #each yields (the synthesized __enum_to_a, not a #to_a the class
   may define otherwise) with the shared proc, as a builtin container does
   in the pre-arm. P's arms are the classes that answer it themselves. */
void emit_poly_enum_includer_arms0(Compiler *c, int id, const char *name, TyKind ret, int tv, int tr,
                                   int blk_tmp0, const PolyPlan *p, Buf *b) {
  const char *op = blk_tmp0 >= 0 && nt_ref(c->nt, id, "block") >= 0 ? poly_enum_includer_op(name) : NULL;
  if (!op) return;
  char v[32];
  snprintf(v, sizeof v, "_t%d", tv);
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->instantiated || ci->is_native_class || comp_class_is_module(c, ci)) continue;
    int armed = 0;
    for (int i = 0; p && i < p->n && !armed; i++) armed = p->arm[i].key == k;
    char elems[320];
    if (armed || !comp_enum_reaches(c, k) || !comp_enum_answers_first(c, k, name) ||
        !obj_each_elems_text(c, k, v, elems, sizeof elems)) continue;
    char pcall[512];
    snprintf(pcall, sizeof pcall, "sp_poly_enum_proc(%s, %s, _t%d)", elems, op, blk_tmp0);
    buf_printf(b, " case %d: _t%d = ", k, tr);
    if (ret == TY_POLY) buf_puts(b, pcall);
    else emit_unbox_text(c, ret, pcall, b);
    buf_puts(b, "; break;");
  }
}

/* Reuse the typed Struct writer with the receiver and arguments already
   evaluated by the dispatch. The arm's views leave the original call intact. */
static void emit_poly_struct_set(Compiler *c, int id, const PolyUserArgs *U, int k, Buf *b) {
  int recv = nt_ref(c->nt, id, "receiver");
  int rv = view_push(c, recv, ty_object(k));
  int rb = view_bind(recv, "((sp_%s *)_t%d.v.p)", c->classes[k].c_name, U->tv);
  int av[2];
  for (int i = 0; i < 2; i++) {
    av[i] = view_push(c, U->argv[i], U->atmp_ty[i]);
    view_bind(U->argv[i], "_t%d", U->atmp[i]);
  }
  buf_printf(b, " case %d: _t%d = ", k, U->tr);
  emit_object_call(c, id, b);
  buf_puts(b, "; break;");
  view_unbind(rb);
  for (int i = 1; i >= 0; i--) view_pop(c, av[i]);
  view_pop(c, rv);
  if (g_plan_check) pa_observe(PA_STRUCT_SET, k, -1, U->ret, PC_SAME);
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
  if (!has_splat_arg && kwh < 0 && cplan_struct_aset(c, k, name, argc)) {
    emit_poly_struct_set(c, id, U, k, b);
    return 1;
  }
  /* native (C-backed) class arm: a declared method of this arity takes
     the hoisted temps in its native representation, as the zero-arg
     dispatch's arm and the typed-receiver call do. The argument
     dispatch had no such arm, so a native object reached through a
     poly slot answered NoMethodError for every call with an argument
     while its zero-arg calls dispatched (#4504). Positional calls
     only: a binding declares no keywords and no splat. */
  int nk = comp_native_binding_class(c, k, name);
  if (nk >= 0) {
    /* a lone splat goes whole to a binding that is all :rest */
    if (kw_pos && has_splat_arg && argc == 1 && splat_a == 0 && kwh < 0 &&
        c->classes[k].instantiated) {
      const NativeMethod *rm = NULL;
      for (int i = 0; i < c->n_native_methods && !rm; i++) {
        const NativeMethod *m = &c->native_methods[i];
        if (m->class_id == nk && m->kind == 0 && m->rest && m->nargs == 0 && sp_streq(m->name, name))
          rm = m;
      }
      if (!rm) return 0;
      Buf cb; memset(&cb, 0, sizeof cb);
      int sp = stk >= 0 ? stk : atmp[0];
      if (sp_streq(rm->ret, "string?")) buf_puts(&cb, "sp_box_nullable_str(");
      buf_printf(&cb, "%s((%s *)_t%d.v.p, _t%d->len, _t%d->data)",
                 rm->csym, c->classes[nk].c_struct, tv, sp, sp);
      if (sp_streq(rm->ret, "string?")) buf_puts(&cb, ")");
      TyKind mret = sp_streq(rm->ret, "self") ? ty_object(k) : native_spec_to_ty(rm->ret);
      buf_printf(b, " case %d: ", k);
      int pconv = emit_poly_native_arm_stmt(c, cb.p, mret, ret, plan_slot_oint(c, id, ret), tr, is_setter_val, b);
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
      int pconv = emit_poly_native_arm_stmt(c, cb.p, mret, ret, plan_slot_oint(c, id, ret), tr, is_setter_val, b);
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
        emit_poly_native_arm_stmt(c, cb.p, mret, ret, plan_slot_oint(c, id, ret), tr, is_setter_val, &arm);
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
  MethodLookup ml = { .def_class = -1 };
  int mi = call_target(c, id, k, name, 0, &ml), defcls = ml.def_class;
  /* an exception class with no method of its own for the name takes
     the builtin reopening's (`class Exception; def brief`) */
  if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
    int xd = exc_arm_definer(c, k, name);
    if (xd >= 0) { defcls = xd; mi = call_target(c, id, xd, name, 0, NULL); }
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
    if (repr_of_slot(c, pv0).handle && (at0 == TY_STRING || at0 == TY_STRBUF))
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
         /* a boolean against any other scalar: `@border[x] = !shown` on
            a boxed Array of booleans took an RBS-typed Memory#[]= arm
            and handed its sp_bool to an sp_oint value slot, a hard C
            error; into a plain sp_int it read true as 1 */
         (pt0 == TY_BOOL) != (at0 == TY_BOOL) ||
         /* two concretely different kinds, either of them a builtin
            pointer kind (a Hash, an Exception, a Proc, a Regexp, an Array
            variant): the other is a different C struct, a scalar, or an
            sp_PtrArray * holding other elements, so the raw pass is a hard C error, not a coercion that works --
            the same argument the collapsed-keyword slot below already
            makes. Without it, a `merge` seeded Hash[String, String] on an
            unrelated class took the arm for a plain Hash's call and the
            build stopped inside that class (#4172); a computed-name `send`
            reached an unrelated `create(attrs = {})` with an Exception, an
            object-array parameter with an Exception, or an Integer
            parameter with a Regexp the same way. cplan_arm_args_fit
            (call_plan.c) is this same test; keep them identical. */
         (ty_is_builtin_ptr(pt0) || ty_is_builtin_ptr(at0)))) {
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
      if (!kwh_param_fits(kpt9, kwall_any))
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
    else if (is_boolean_class_name(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.i", tv);
    else if (is_range_or_time_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, sp_streq(_dcn2, "Time") ? "(sp_%s *)_t%d.v.p" : "*(sp_%s *)_t%d.v.p", _dcn2, tv);   /* a Time is a reference */
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
    else if (is_array_hash_or_object_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "Float"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.f", tv);
    else if (repr_self_handle(c, mi))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_as_strbuf(_t%d)", tv);
    else if (sp_streq(_dcn2, "String"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn2, "Symbol"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (is_array_or_object_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (is_boolean_class_name(_dcn2))
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
  buf_printf(&cb, "%s(%s%s", mc_method(c, &c->scopes[pfi8 >= 0 ? pfi8 : mi]),
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
  int pconv = emit_poly_user_arm_n(c, id, k, cb.p, mret8, &c->scopes[pf8 ? pfi8 : mi], ret, tr,
                                   plan_slot_oint(c, id, ret), is_setter_val, b);
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
  if (a->kind == PA_STRUCT_SET) { emit_poly_struct_set(c, id, U, k, b); return; }
  if (comp_native_binding_class(c, k, name) >= 0) { poly_user_arm_n_replay(c, id, name, U, k, b); return; }
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
    else if (is_boolean_class_name(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(int)_t%d.v.i", tv);
    else if (is_range_or_time_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, sp_streq(_dcn2, "Time") ? "(sp_%s *)_t%d.v.p" : "*(sp_%s *)_t%d.v.p", _dcn2, tv);   /* a Time is a reference */
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
    else if (is_array_hash_or_object_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (sp_streq(_dcn2, "Float"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d.v.f", tv);
    else if (repr_self_handle(c, mi))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_as_strbuf(_t%d)", tv);
    else if (sp_streq(_dcn2, "String"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "sp_poly_strbuf_deref(_t%d).v.s", tv);
    else if (sp_streq(_dcn2, "Symbol"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "(sp_sym)_t%d.v.i", tv);
    else if (sp_streq(_dcn2, "NilClass"))
      snprintf(selfpbuf2, sizeof selfpbuf2, "0");
    else if (is_array_or_object_class(_dcn2))
      snprintf(selfpbuf2, sizeof selfpbuf2, "_t%d", tv);
    else if (is_boolean_class_name(_dcn2))
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
  buf_printf(&cb, "%s(%s%s", mc_method(c, &c->scopes[pfi8 >= 0 ? pfi8 : mi]),
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
  int pconv = emit_poly_user_arm_n(c, id, k, cb.p, mret8, &c->scopes[pf8 ? pfi8 : mi], ret, tr,
                                   plan_slot_oint(c, id, ret), is_setter_val, b);
  free(cb.p);
  if (g_plan_check) pa_observe(pf8 ? PA_PROC_FORM : PA_USER, k, mi, mret8, pconv);
}

/* Class k's arm in call id's poly dispatch with arguments, decided as the
   dispatch did, without writing or noting anything: --plan-check's
   assertion. */
static int poly_user_arm_n_decide(Compiler *c, int id, const char *name, const PolyUserArgs *U, int k, PolyArm *a) {
  memset(a, 0, sizeof *a);
  a->key = (short)k; a->mi = -1; a->def = -1; a->vty = TY_UNKNOWN; a->conv = PC_SAME;
  TyKind ret = U->ret;
  if (!U->has_splat_arg && U->kwh < 0 && cplan_struct_aset(c, k, name, U->argc)) {
    a->kind = PA_STRUCT_SET; a->vty = (unsigned char)ret;
    return 1;
  }
  int nk = comp_native_binding_class(c, k, name);
  if (nk >= 0) {
    TyKind mret = TY_UNKNOWN;
    if (U->kw_pos && U->has_splat_arg && U->argc == 1 && U->splat_a == 0 && U->kwh < 0 &&
        c->classes[k].instantiated) {
      const NativeMethod *rm = NULL;
      for (int i = 0; i < c->n_native_methods && !rm; i++) {
        const NativeMethod *m = &c->native_methods[i];
        if (m->class_id == nk && m->kind == 0 && m->rest && m->nargs == 0 && sp_streq(m->name, name)) rm = m;
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
  MethodLookup ml = { .def_class = -1 };
  int mi = call_target(c, id, k, name, 0, &ml), defcls = ml.def_class;
  if (mi < 0 && any_exc_reopen(c) && (class_has_exc_name(c, k) || class_is_exc_subclass(c, k))) {
    int xd = exc_arm_definer(c, k, name);
    if (xd >= 0) { defcls = xd; mi = call_target(c, id, xd, name, 0, NULL); }
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
  /* The shadow sees the held temps, whereas the plan sees the source
     expressions. emit_poly_temp_as reads a held handle into a String
     parameter; compare the binding's representation after that read. */
  int n = U->pos_argc;
  TyKind *bound = malloc((size_t)(n > 0 ? n : 1) * sizeof *bound);
  if (!bound) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  if (n > 0) memcpy(bound, U->atmp_ty, (size_t)n * sizeof *bound);
  for (int p = 0; p < ks->nparams; p++) {
    if (L.from[p] != ARG_NODE || L.arg[p] >= n) continue;
    LocalVar *lv = ks->pnames && ks->pnames[p] ? scope_local(ks, ks->pnames[p]) : NULL;
    if (bound[L.arg[p]] == TY_STRBUF && repr_of_slot(c, lv).as_ty == TY_STRING)
      bound[L.arg[p]] = TY_STRING;
  }
  int fits = cplan_arm_args_fit(c, ks, &L, n, bound, U->kwall_any);
  free(bound);
  arg_layout_free(&L);
  if (!fits) return 0;
  TyKind mret = ks->ret;
  a->kind = pfi >= 0 ? PA_PROC_FORM : PA_USER; a->mi = mi; a->def = (short)defcls; a->vty = (unsigned char)mret;
  a->conv = U->is_setter_val || mret == TY_VOID || mret == TY_NIL || method_is_void(ks) ? PC_VOID
          : ret == TY_POLY && mret != TY_POLY ? PC_BOX
          : ret != TY_POLY && mret == TY_POLY ? PC_UNBOX
          : mret == TY_STRBUF && ret == TY_STRING ? PC_COPY : PC_SAME;
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
      if (!cls_fact_excludes(c, nt_ref(c->nt, id, "receiver"), k) &&   /* as the plan leaves them out (#8530) */
          poly_user_arm_n_decide(c, id, name, U, k, &old[nold])) nold++;
    poly_arms_assert(c, id, "poly-user-n", arms, n, old, nold);
    free(old);
    cplan_served("poly-user-n");
  }
  if (!served && g_plan_check) fprintf(stderr, "plan-check: cplan-fallback: poly-user-n node %d %s\n", id, name);
  int j = 0, recv = nt_ref(c->nt, id, "receiver");
  ArmText *at = NULL;
  int nat = 0, cat = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (cls_fact_excludes(c, recv, k)) continue;   /* a class the receiver never holds has no arm (#8530) */
    at = arm_text_next(at, &nat, &cat, k);
    Buf *ab = &at[nat - 1].t;
    if (!served) { poly_user_arm_n_replay(c, id, name, U, k, ab); continue; }
    if (j < n && arms[j].key == k) { emit_poly_user_arm_n_plan(c, id, name, U, &arms[j], ab); j++; continue; }
    Buf scratch; memset(&scratch, 0, sizeof scratch);
    if (poly_user_arm_n_replay(c, id, name, U, k, &scratch) && g_plan_check)
      fprintf(stderr, "plan-check: cplan-conflict: poly-user-n node %d %s: class %d: decided only\n", id, name, k);
    free(scratch.p);
  }
  arm_texts_flush(at, nat, b);
  free(at);
  free(arms);
}

/* One user-class arm of a poly dispatch with arguments: `case k:` calling
   `call`, its value (mret, from the method or its proc form ms) into the
   result temp _t<tr> as the call's type ret takes it. The conversion
   applied (PolyConv). */
int emit_poly_user_arm_n(Compiler *c, int id, int k, const char *call, TyKind mret, Scope *ms, TyKind ret,
                         int tr, int so, int is_setter_val, Buf *b) {
  int conv = PC_SAME;
  buf_printf(b, " case %d: ", k);
  if (is_setter_val || mret == TY_VOID || mret == TY_NIL || method_is_void(ms)) {
    buf_puts(b, call);  /* no usable value */
    /* ...but nil is its value: the result temp may start as a builtin
       arm's answer (Hash#fetch's default), which a user `fetch` returning
       nil left in place (#8200). A setter's value is its argument. */
    if (!is_setter_val && ret == TY_POLY) buf_printf(b, "; _t%d = sp_box_nil()", tr);
    conv = PC_VOID;
  }
  else {
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY && mret != TY_POLY) { emit_poly_user_box(c, id, ms, call, b); conv = PC_BOX; }
    else if (c->n_cext_extensions && ret == TY_STRBUF && mret == TY_STRING) {
      int temp = ++g_tmp;
      buf_printf(b, "({ _sp_ret_strbuf = NULL; __auto_type _v%d = (%s); !_v%d ? NULL : ", temp, call, temp);
      if (ms && ms->ret_handle) buf_puts(b, "_sp_ret_strbuf ? (sp_String *)_sp_ret_strbuf : ");
      str_new_handle_open(b);
      buf_printf(b, "_v%d", temp);
      str_new_handle_close(b);
      buf_puts(b, "; })");
    }
    else if (ret != TY_POLY && mret == TY_POLY) {
      plan_put_boxed(c, so, is_scalar_ret(ret) ? ret : TY_INT, call, b);
      conv = PC_UNBOX;
    }
    /* a String's handle into a plain String slot (as the zero-argument arm) */
    else if (mret == TY_STRBUF && ret == TY_STRING) {
      str_read_handle(b, call, 1);
      conv = PC_COPY;
    }
    /* so: the result slot is an sp_oint / sp_ofloat (plan_slot_oint) */
    else if (ms && ms->ret == mret && method_ret_is_oint(ms)) {
      if (so) buf_puts(b, call); else buf_printf(b, "%s(%s)", oint_arg(mret), call);
    }
    /* a String subclass instance into the String slot: its bytes (as the
       zero-argument arm) */
    else if (plan_put_strsub(c, ret, mret, call, b)) {}
    else plan_put_plain(so, ret, call, b);
  }
  buf_puts(b, "; break;");
  return conv;
}

/* The builtin array arms of a poly `x[i]`: the element at index expression
   idxref, into the result temp. */
void emit_poly_index_cases(TyKind ret, int tr, int tv, const char *idxref, int so, Buf *b) {
  if (ret == TY_POLY) {
    /* An Integer or Float array answers a nil element, and any index
       past its end, as nil: the _oget read, boxed */
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_box_oint(sp_IntArray_oget((sp_IntArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_get%s((sp_StrArray *)_t%d.v.p, %s)); break;", tr, ONE_SA_BOXO, ONE_SA_GE, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = sp_box_ofloat(sp_FloatArray_oget((sp_FloatArray *)_t%d.v.p, %s)); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: _t%d = sp_PolyArray_get((sp_PolyArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
    buf_printf(b, " case SP_BUILTIN_PTR_ARRAY: _t%d = sp_PtrArray_get_box((sp_PtrArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
  }
  /* so: the result slot is an sp_oint, which takes a nil element and a miss */
  else if (so) {
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_IntArray_oget((sp_IntArray *)_t%d.v.p, %s)%s; break;",
               tr, ret == TY_FLOAT ? "sp_oint_to_ofloat(" : "", tv, idxref, ret == TY_FLOAT ? ")" : "");
  }
  else {
    buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = sp_IntArray_get((sp_IntArray *)_t%d.v.p, %s); break;", tr, tv, idxref);
  }
}

/* Whether a user class defines `name` as a class-level method. Only that one
   can answer a class-tagged value: an instance method's receiver carries
   SP_TAG_OBJ, so the class-tagged arm stays beside the user arms (a class
   value must not reach them by its cls_id, which is the class's own id). */
static int recv_user_cdefines(Compiler *c, const char *name) {
  if (g_poly_builtin_arm) return 0;
  for (int uk = 0; uk < c->nclasses; uk++)
    if (comp_cmethod_in_chain(c, uk, name, NULL) >= 0) return 1;
  return 0;
}

/* Can a class value (a boxed class) answer `name` with a method of the
   program, not Class's or Module's own? Only a class method (`def
   self.superclass`), a module's method (an `extend` gives it to a class),
   or a method of a reopened Class (the module Class__reopen) or Module. A
   plain class's instance method (Object's too) is not in a class value's
   chain ahead of Class's. A module value has no Class method, so it keeps
   those instance methods: the caller guards the arm on the value's kind
   (*cls_only). */
static int recv_class_value_defines(Compiler *c, const char *name) {
  if (g_poly_builtin_arm) return 0;
  if (comp_name_is_shadow_copy(c, name, SHADOW_PREP)) return 0;
  if (recv_user_cdefines(c, name)) return 1;
  for (int uk = 0; uk < c->nclasses; uk++) {
    const char *kn = c->classes[uk].name;
    if ((comp_class_is_module(c, &c->classes[uk]) || (kn && sp_streq(kn, "Module"))) &&
        comp_method_in_chain(c, uk, name, NULL) >= 0) return 1;
  }
  return 0;
}

/* The Module reflection a class-tagged poly value answers. `ancestors` and
   friends had an arm only for a receiver typed TY_CLASS -- a constant --
   so iterating an Array of classes and asking the block parameter left the
   call with no arm at all and it reported the method as undefined (#4018).
   superclass, subclasses and allocate are Class's only: where a program's
   instance method of the name stands beside the arm, a module value takes
   that method, so the arm answers a class value only (*cls_only). */
int poly_class_reflect_arm(Compiler *c, int id, const char *name, int *cls_only) {
  const NodeTable *nt = c->nt;
  int class_side = sp_streq(name, "subclasses") || sp_streq(name, "allocate");
  int arm = (sp_streq(name, "ancestors") || sp_streq(name, "included_modules") ||
             sp_streq(name, "superclass") ||
             /* the class-side names the generated sp_cls_* answer
                (members has its arm in emit_poly_call) */
             (g_gen_cls_answers && nt_ref(nt, id, "block") < 0 &&
              (class_side || sp_streq(name, "keyword_init?")))) &&
            !recv_class_value_defines(c, name);
  if (cls_only)
    *cls_only = arm && (class_side || sp_streq(name, "superclass")) && recv_user_defines(c, name);
  return arm;
}

/* The names a blockless-or-not zero-argument poly dispatch answers
   beside its user arms (the pre-arms and builtin cases of
   emit_poly_method_dispatch), and its user candidates: ncand classes own
   the name at all, ncall_arm of them get a calling arm (the root
   decision). Shared by the dispatch and the resolver (cplan_poly). */
/* poly_specials0's candidate counts by (name, has a block), under the
   stamps of what they read */
struct sp0_entry { char *name; int blk, ncand, ncall_arm; struct sp0_entry *next; };
#define SP0_BUCKETS 1024
static struct sp0_entry *sp0_tab[SP0_BUCKETS];
static struct { unsigned sgen, tgen, igen; int nscopes, nclasses; const Compiler *c; } sp0_of;
static void sp0_stamp(const Compiler *c) {
  if (sp0_of.c == c && sp0_of.sgen == comp_scope_index_gen() && sp0_of.tgen == comp_table_gen &&
      sp0_of.igen == comp_instantiated_gen && sp0_of.nscopes == c->nscopes && sp0_of.nclasses == c->nclasses) return;
  for (int b = 0; b < SP0_BUCKETS; b++)
    for (struct sp0_entry *e = sp0_tab[b], *nx; e; e = nx) { nx = e->next; free(e->name); free(e); }
  memset(sp0_tab, 0, sizeof sp0_tab);
  sp0_of.c = c; sp0_of.sgen = comp_scope_index_gen(); sp0_of.tgen = comp_table_gen;
  sp0_of.igen = comp_instantiated_gen; sp0_of.nscopes = c->nscopes; sp0_of.nclasses = c->nclasses;
}
static int sp0_lookup(const Compiler *c, const char *name, int blk, int *ncand, int *ncall_arm) {
  sp0_stamp(c);
  for (struct sp0_entry *e = sp0_tab[(sp_strhash(name) * 2u + (unsigned)blk) % SP0_BUCKETS]; e; e = e->next)
    if (e->blk == blk && sp_streq(e->name, name)) { *ncand = e->ncand; *ncall_arm = e->ncall_arm; return 1; }
  return 0;
}
static void sp0_store(const Compiler *c, const char *name, int blk, int ncand, int ncall_arm) {
  sp0_stamp(c);
  struct sp0_entry *e = malloc(sizeof *e);
  if (!e || !(e->name = strdup(name))) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  unsigned b = (sp_strhash(name) * 2u + (unsigned)blk) % SP0_BUCKETS;
  e->blk = blk; e->ncand = ncand; e->ncall_arm = ncall_arm; e->next = sp0_tab[b]; sp0_tab[b] = e;
}
void poly_specials0(Compiler *c, int id, const char *name, PolySpecials0 *s) {
  const NodeTable *nt = c->nt;
  int argc = 0;
  int is_lengthlike = is_count_alias(name);
  int is_empty = sp_streq(name, "empty?");
  /* A class-tagged poly value answers these with its class name (#2656); only
     when no user class defines them, or that user method is the real target.
     `name` stays beside a program's instance methods of the name (#9567:
     only a class-level `name` can answer a class-tagged value); to_s and
     inspect keep the older rule, as a program's own to_s is common and the
     class arm's text beside it does not fit a shared String slot. */
  int is_class_named = is_name_reader(name) &&
                       !(sp_streq(name, "name") ? recv_user_cdefines(c, name) : recv_user_defines(c, name));
  int is_class_reflect_cls_only = 0;
  int is_class_reflect = poly_class_reflect_arm(c, id, name, &is_class_reflect_cls_only);
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
  /* the builtins' names an OpenStruct reads as a member
     (ostruct_member_name: size, first, max, ...) read it too where one can
     reach the receiver (cls_fact_may_ostruct): the containers' arms read 0,
     nil or NoMethodError for it, and a class's own method took the rest */
  if (argc == 0 && nt_ref(nt, id, "block") < 0 && ostruct_member_name(name) &&
      cls_fact_may_ostruct(c, nt_ref(nt, id, "receiver")))
    is_ostruct = 1;
  /* `rewind` on a poly stream (a param unioning StringIO and IO, #3257):
     both are builtins/native classes with no user arm, so without this
     pre-arm the call was silently dropped. */
  /* ...and an Enumerator's, beside a class of the program's own that
     defines rewind too: the builtin arms test their runtime kind first, so
     a user arm still takes its objects */
  int is_io_rewind = sp_streq(name, "rewind") && argc == 0;
  /* to_a on a poly value that is really a builtin hash/array (a yield-result
     union of an rbs-seeded Hash and a class instance, #3278): the user-class
     switch has no builtin arm, so the hash fell through to the nil seed. */
  int is_poly_to_a = sp_streq(name, "to_a") &&
                     (comp_ntype(c, id) == TY_POLY_ARRAY || repr_of(c, id).kind == RK_BOXED);
  /* to_h on a poly value that is really a builtin hash (or an Array of
     pairs, or a Struct): the user-class switch carries an arm per class that
     defines to_h and none for the builtin, so a plain Hash reached the
     default and raised -- naming Hash, the class whose method it is. Every
     sibling (to_a, to_s, keys, length) already had its arm (#4170). */
  int is_poly_to_h = sp_streq(name, "to_h") && argc == 0 &&
                     nt_ref(nt, id, "block") < 0 &&
                     repr_of(c, id).kind == RK_BOXED;
  int ncand = 0, ncall_arm = 0;
  /* a class neither defining nor reading the name, and not native, counts
     for neither: the name's memoized candidates are the classes to ask */
  int npc0 = 0;
  const PolyCand *pc0 = comp_poly_candidates(c, name, &npc0);
  /* The counts depend on the call only through whether it has a block, and,
     for a block-carrying call, a native class's call_target: for every other
     call they are the name's, kept while nothing they read can move (the
     frozen scope index, the scope, class and table epochs, the instantiated
     flags). Every poly call of a name asked every class of it (#10546). */
  int blk0 = nt_ref(nt, id, "block") >= 0;
  int sp0_memo = comp_scope_index_is_frozen() && comp_scope_move_depth() == 0;
  if (sp0_memo && sp0_lookup(c, name, blk0, &ncand, &ncall_arm)) npc0 = 0;
  int sp0_native = 0;
  for (int ki = 0; ki < npc0; ki++) {
    if (blk0 && c->classes[pc0[ki].cls].is_native_class) sp0_native = 1;
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
                   call_target(c, id, k, name, 0, NULL) >= 0);
    if (is_call || (!c->classes[k].is_native_class && comp_reader_in_chain(c, k, name, NULL))) ncand++;
    /* The root decision counts only the arms the switch below will carry:
       a class no reachable code constructs gets no arm, so it must not
       decide the root either. A dead FFI wrapper's Vector2 counted as a
       calling arm, and every `x` read of a Struct field paid a root push
       and pop for an arm that could not run (#4460). ncand keeps every
       candidate, as the choice to emit a dispatch at all always has. */
    if (is_call && (c->classes[k].instantiated || class_is_prim_reopen(c, k))) ncall_arm++;
  }
  if (sp0_memo && npc0 > 0 && !sp0_native) sp0_store(c, name, blk0, ncand, ncall_arm);
  s->lengthlike = is_lengthlike;
  s->empty = is_empty;
  s->class_named = is_class_named;
  s->class_reflect = is_class_reflect;
  s->class_reflect_cls_only = is_class_reflect_cls_only;
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
  { int obj_cls = comp_core_class(c, CORE_OBJECT);
    if (obj_cls >= 0) {
      MethodLookup ml = { .def_class = -1 };
      int obj_mi = call_target(c, id, obj_cls, name, 0, &ml);
      /* A yielding one is reached with the call's block through its proc
         form: a user class's chain does not name Object, so without this
         arm an instance of it took the raise (#5101) */
      int obj_pf = (obj_mi >= 0 && ml.def_class == obj_cls && argc == 0 &&
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
        buf_puts(&oc, mc_method(c, &c->scopes[obj_pf]));
        buf_printf(&oc, "(_t%d, _t%d)", tv, (*blk_tmp0));
        TyKind pr = (TyKind)c->scopes[obj_pf].ret;
        buf_puts(b, " default: ");
        int pconv = PC_SAME;
        if (method_is_void(&c->scopes[obj_pf])) { buf_puts(b, oc.p); pconv = PC_VOID; }
        else {
          buf_printf(b, "_t%d = ", tr);
          int so = plan_slot_oint(c, id, ret);
          if (ret == TY_POLY && pr != TY_POLY) {
            emit_poly_user_box(c, id, &c->scopes[obj_pf], oc.p, b); pconv = PC_BOX;
          }
          else if (ret != TY_POLY && pr == TY_POLY) {
            plan_put_boxed(c, so, is_scalar_ret(ret) ? ret : TY_INT, oc.p, b);
            pconv = PC_UNBOX;
          }
          else if (so && method_ret_is_oint(&c->scopes[obj_pf])) buf_puts(b, oc.p);
          else plan_put_plain(so, ret, oc.p, b);
        }
        buf_puts(b, "; break;");
        free(oc.p);
        done = 1;
        if (g_plan_check) pa_observe(PA_PROC_FORM, PA_KEY_DEFAULT, obj_mi, pr, pconv);
      }
      else if (obj_mi >= 0 && ml.def_class == obj_cls && c->scopes[obj_mi].nrequired == 0 &&
          scope_has_callable_symbol(c, obj_mi)) {
        /* an optional parameter takes its default, spelled with the
           boxed receiver as self. What a default puts ahead of the call (a
           temp of its own, an earlier parameter it reads) is this arm's
           alone, in a block of the arm: in the statement's prelude it ran
           for a value of every class, or named a temp the prelude never
           declared. A default that reads an earlier parameter reads it
           from that parameter's temp (emit_pd_param_bind), as a direct
           call's does. */
        Scope *om = &c->scopes[obj_mi];
        Buf ob; memset(&ob, 0, sizeof ob);
        Buf apre; memset(&apre, 0, sizeof apre);
        buf_printf(&ob, "%s(_t%d", mc_method(c, om), tv);
        { const char *saved_self = g_self;
          char oselfbuf[32]; snprintf(oselfbuf, sizeof oselfbuf, "_t%d", tv);
          g_self = oselfbuf;
          Buf *sv_pre = g_pre; g_pre = &apre;
          int sibling = om->nparams <= 64 && default_refs_earlier_param(c, om);
          int uid = sibling ? ++g_tmp : 0, ren_base = g_nren;
          char tn[64][64];
          /* (an empty rest's Array and an empty **kwrest's Hash are held
             in the arm's temps as a class arm's are) */
          if (!sibling) emit_zero_arg_defaults(c, om, &ob, &apre);
          for (int a = 0; sibling && a < om->nparams; a++) {
            Buf vb; memset(&vb, 0, sizeof vb);
            emit_arg_or_default(c, om, a, -1, &vb);
            emit_pd_param_bind(c, om, a, uid, vb.p, tn[a], sizeof tn[a]);
            free(vb.p);
          }
          if (sibling) {
            emit_pd_cell_copyback(c, om, uid, tn);
            for (int a = 0; a < om->nparams; a++) { buf_puts(&ob, ", "); buf_puts(&ob, tn[a]); }
          }
          g_nren = ren_base;
          g_pre = sv_pre;
          g_self = saved_self; }
        emit_trailing_blk_arg(c, om, id, (*blk_tmp0), &ob);
        buf_puts(&ob, ")");
        const char *ocall = ob.p;
        buf_puts(b, " default: ");
        if (apre.p && apre.p[0]) { buf_puts(b, "{ "); buf_puts(b, apre.p); }
        int pconv = PC_SAME;
        if (method_is_void(&c->scopes[obj_mi])) { buf_puts(b, ocall); pconv = PC_VOID; }
        else {
          TyKind oslot = is_scalar_ret(ret) ? ret : TY_INT;
          int so = plan_slot_oint(c, id, ret);
          buf_printf(b, "_t%d = ", tr);
          if (ret == TY_POLY && c->scopes[obj_mi].ret != TY_POLY) {
            emit_poly_user_box(c, id, &c->scopes[obj_mi], ocall, b);
            pconv = PC_BOX;
          }
          else if (ret != TY_POLY && c->scopes[obj_mi].ret == TY_POLY) {
            plan_put_boxed(c, so, oslot, ocall, b);
            pconv = PC_UNBOX;
          }
          else if (so && method_ret_is_oint(&c->scopes[obj_mi])) buf_puts(b, ocall);
          else if (method_ret_is_oint(&c->scopes[obj_mi])) buf_printf(b, "%s(%s)", oint_arg(c->scopes[obj_mi].ret), ocall);
          else plan_put_plain(so, ret, ocall, b);
        }
        if (apre.p && apre.p[0]) buf_puts(b, "; }");
        buf_puts(b, "; break;");
        free(apre.p);
        free(ob.p);
        done = 1;
        if (g_plan_check) pa_observe(PA_USER, PA_KEY_DEFAULT, obj_mi, c->scopes[obj_mi].ret, pconv);
      }
      /* ... and one that needs arguments refuses them for any receiver */
      else { char oexp[600];
        if (ml.def_class == obj_cls && poly_arm_refuses_none(c, obj_mi, oexp, sizeof oexp)) {
          buf_puts(b, " default: "); emit_poly_arity_raise(b, oexp); buf_puts(b, " break;");
          done = 1;
          if (g_plan_check) pa_observe(PA_ARITY, PA_KEY_DEFAULT, obj_mi, TY_UNKNOWN, PC_VOID);
        } }
    } }
  return done;
}

/* Does class 0 take a `case 0:` arm in call `call`'s poly dispatch of name
   with argc arguments (kwh, pos_argc, splat_a as poly_arm_count reads
   them)? The dispatch key then keeps a non-object value off it. */
int poly_key_cls0(Compiler *c, int call, const char *name, int argc, int kwh, int pos_argc, int splat_a) {
  if (argc == 0) {
    int cls0_rd = -1;
    int cls0_mi = c->nclasses > 0 ? call_target(c, call, 0, name, 0, NULL) : -1;
    char cls0_exp[600];
    return ((cls0_mi >= 0 && c->scopes[cls0_mi].nrequired == 0) ||
            /* an arm refusing no arguments raises, and is a `case 0:` all the same */
            poly_arm_refuses_none(c, cls0_mi, cls0_exp, sizeof cls0_exp) ||
            (c->nclasses > 0 && comp_reader_in_chain(c, 0, name, &cls0_rd))) &&
           c->nclasses > 0 &&
           (c->classes[0].instantiated || class_is_prim_reopen(c, 0));
  }
  int cls0_mi2 = c->nclasses > 0 ? call_target(c, call, 0, name, 0, NULL) : -1;
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
  /* A Struct's builtin `[]=` has no method scope but takes a `case 0:` all
     the same (cplan_struct_aset). A boxed String, whose cls_id is 0 too,
     entered it and had the member written into its bytes. */
  if (!cls0_cand2 && c->nclasses > 0 && kwh < 0 && splat_a < 0)
    cls0_cand2 = cplan_struct_aset(c, 0, name, argc);
  return cls0_cand2;
}

/* Does a primitive reopening (String, Integer, ...) take an arm in that
   dispatch of call `call`? The key then maps a runtime tag to its class
   (#4219). */
int poly_key_prim(Compiler *c, int call, const char *name, int argc, int kwh, int pos_argc, int splat_a) {
  /* a reopening without the method takes no arm: only one of the name's
     candidates (a class whose chain has the method or a reader,
     comp_poly_candidates), asked of the primitive reopenings alone, which
     are few, where every candidate of the name was asked (#10546) */
  IMEMO_DEP_GLOBAL();   /* (the candidates are read over every class) */
  int npk = 0;
  const int *pks = comp_prim_reopen_classes(c, &npk);
  for (int ki = 0; ki < npk; ki++) {
    int k = pks[ki];
    if (comp_method_in_chain(c, k, name, NULL) < 0 && !comp_reader_in_chain(c, k, name, NULL)) continue;
    int pmi = call_target(c, call, k, name, 0, NULL);
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
  /* ...and an oint result slot (plan_slot_oint) takes it lifted */
  int lso = ret == TY_INT && plan_slot_oint(c, id, ret);
  const char *bopen = (ret == TY_POLY) ? "sp_box_int(" : lso ? "sp_oint_of(" : "";
  const char *bclose = (ret == TY_POLY || lso) ? ")" : "";
  const char *ebopen = (ret == TY_POLY) ? "sp_box_bool(" : "";
  const char *ebclose = (ret == TY_POLY) ? ")" : "";
  /* string/symbol-tagged poly values answer length/size directly */
  if (is_lengthlike) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_LEN, -1, TY_UNKNOWN, PC_SAME);
    if (sp_streq(name, "count")) {
      /* ...but not count: String#count needs the characters to count, so
         sp_poly_count raises its ArgumentError here, ahead of an Object or
         Kernel count the program defines. A String reopening's own count
         takes its arm below instead, and so does a Symbol, which has no
         count of its own. */
      MethodLookup ml = { .def_class = -1 };
      int sci = comp_core_class(c, CORE_STRING);
      int str_own = sci >= 0 && call_target(c, id, sci, name, 0, &ml) >= 0 && ml.def_class == sci;
      if (!str_own)
        buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %ssp_poly_count(_t%d)%s; else ", tv, tr, bopen, tv, bclose);
    } else {
      buf_printf(b, "if (_t%d.tag == SP_TAG_SYM) _t%d = %ssp_str_length(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ", tv, tr, bopen, tv, bclose);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) _t%d = %s(sp_int)sp_str_length(_t%d.v.s)%s; else ", tv, tr, bopen, tv, bclose);
    }
  }
  if (is_lengthlike) {
    /* A handle answers File#size through the runtime's own dispatch,
       which knows whether it is a File (fstat) or an IO (CRuby's
       NoMethodError). This chain is built when a user class owns the
       name too, and its default arm raised for the File the same
       program keeps beside those objects in one Hash (#4734). */
    if (sp_streq(name, "size")) {
      /* sp_poly_size answers an sp_oint: boxed with its nil, bare into an
         oint slot, unwrapped (TypeError for nil) into a plain one */
      const char *so = ret == TY_POLY ? "sp_box_oint(" : plan_slot_oint(c, id, ret) ? "(" : "sp_oint_arg(";
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) _t%d = %ssp_poly_size(_t%d)); else ",
                 tv, tv, tr, so, tv);
    }
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
  /* (a result slot that is a shared String's handle, beside user arms
     answering handles, takes the arms' text wrapped as one) */
  Buf hob, hcb;
  memset(&hob, 0, sizeof hob);
  memset(&hcb, 0, sizeof hcb);
  if (ret == TY_STRBUF) {
    str_new_handle_open(&hob);
    str_new_handle_close(&hcb);
  }
  const char *hopen = hob.p ? hob.p : "", *hclose = hcb.p ? hcb.p : "";
  if (is_class_named) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CLASS_NAMED, -1, TY_UNKNOWN, PC_SAME);
    const char *sbopen = (ret == TY_POLY) ? "sp_box_str(" : hopen;
    const char *sbclose = (ret == TY_POLY) ? ")" : hclose;
    /* a boxed class's #name is the interned frozen String the typed
       forms answer (sp_str_frozen_name, which emit_call wraps around
       those), as the Encoding and Symbol arms below intern theirs; its
       to_s and inspect are not frozen */
    int fzn = sp_streq(name, "name");
    /* A Class.new class answers its name only once a constant names it: nil
       before, its display form for to_s and inspect (sp_class_val_display). */
    if ((c->has_anonymous_classes || comp_has_sclass_values(c)) && fzn)
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) { const char *_cn%d = sp_class_name_or_nil(sp_unbox_class(_t%d)); "
                    "_t%d = _cn%d ? %ssp_str_uminus_val(_cn%d)%s : %s; } else ",
                 tv, tv, tv, tr, tv, sbopen, tv, sbclose, ret == TY_POLY ? "sp_box_nil()" : "NULL");
    else
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %s%s%s(_t%d)%s%s; else ",
                 tv, tr, sbopen, fzn ? "sp_str_uminus_val(" : "",
                 c->has_anonymous_classes ? "sp_class_val_display" : "sp_class_val_name", tv, fzn ? ")" : "", sbclose);
    /* `name` on an Encoding (always carried boxed) and on a Symbol: a
       frozen String, as CRuby answers and as the typed Symbol#name does */
    if (sp_streq(name, "name"))
      buf_printf(b, "if (_t%d.tag == SP_TAG_ENCODING) _t%d = %ssp_str_uminus_val(_t%d.v.s)%s; "
                    "else if (_t%d.tag == SP_TAG_SYM) "
                    "_t%d = %ssp_str_uminus_val(sp_sym_to_s((sp_sym)_t%d.v.i))%s; else ",
                 tv, tr, sbopen, tv, sbclose, tv, tr, sbopen, tv, sbclose);
  }
  /* A class of the program defining name declines the arms above, but
     an Encoding's and a Symbol's are guarded on a tag no object carries:
     they stand beside the program's arm (`instance_methods(false).each
     { |m| m.name }` in a program with a name of its own) */
  else if (sp_streq(name, "name") && (ret == TY_POLY || ret == TY_STRING || ret == TY_STRBUF)) {
    const char *sbopen = (ret == TY_POLY) ? "sp_box_str(" : hopen;
    const char *sbclose = (ret == TY_POLY) ? ")" : hclose;
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
    if (ps->class_reflect_cls_only)
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS && !sp_class_is_module_val(sp_unbox_class(_t%d))) _t%d = %s", tv, tv, tr, cbo);
    else
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
    /* the result slot is user-typed (#3264): an Integer or Float one that
       holds nil (an oint) takes the member with its nil, as the other arms'
       answers are taken */
    else plan_put_boxed(c, plan_slot_oint(c, id, ret), ret, osget, b);
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
    /* a stream answers its 0 where the result is boxed */
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO)"
                  " { sp_int _rw = sp_File_rewind((sp_File *)_t%d.v.p);", tv, tv, tv);
    if (ret == TY_POLY) buf_printf(b, " _t%d = sp_box_int(_rw);", tr);
    else if (ret == TY_INT) buf_printf(b, " _t%d = _rw;", tr);
    buf_puts(b, " (void)_rw; }\nelse ");
    /* a Dir also rewinds and answers its own handle, where the program can
       make one */
    if (an_program_makes_dir(c)) {
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_DIR)"
                    " { sp_Dir_rewind((sp_Dir *)_t%d.v.p);", tv, tv, tv);
      if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
      buf_puts(b, " }\nelse ");
    }
    /* an Enumerator rewinds, and answers itself where the result is boxed */
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_ENUMERATOR)"
                  " { sp_Enumerator_rewind((sp_Enumerator *)_t%d.v.p);", tv, tv, tv);
    if (ret == TY_POLY) buf_printf(b, " _t%d = _t%d;", tr, tv);
    buf_puts(b, " }\nelse ");
    int sio_cid3 = comp_core_class(c, CORE_STRINGIO);
    if (sio_cid3 >= 0)
    {
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == %d)"
                    " { sp_int _rw = sp_StringIO_rewind((sp_StringIO *)_t%d.v.p);",
                 tv, tv, sio_cid3, tv);
      if (ret == TY_POLY) buf_printf(b, " _t%d = sp_box_int(_rw);", tr);
      else if (ret == TY_INT) buf_printf(b, " _t%d = _rw;", tr);
      buf_puts(b, " (void)_rw; }\nelse ");
    }
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
       nil (an oint), readline/readchar/readbyte raise EOFError */
    const BuiltinZeroOp *io_op = bop_zero_find(TY_IO, name);
    /* a stat handle's readers (boxed_stat_reader: mtime, atime, ftype),
       rendered through their TY_IO rows, which also hold that a File handle
       has no ftype (sp_stat_extended_recv): the row gives the answer's kind.
       It is one arm of the dispatch, which records nothing for it, as the
       hand-written arm before it did not (emit_builtin_op_arm_text) */
    const BuiltinOp *sop = boxed_stat_reader(name) ? bop_find(TY_IO, name, 0, 0) : NULL;
    if (sop && (ret == TY_POLY || ret == sop->result)) {
      Buf sb = {0};
      char sr[48];
      snprintf(sr, sizeof sr, "((sp_File *)_t%d.v.p)", tv);
      if (emit_builtin_op_arm_text(c, id, nt_ref(nt, id, "receiver"), TY_IO, name, sr, 0, &sb)) {
        buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) { _t%d = ", tv, tv, tr);
        if (ret == TY_POLY) emit_boxed_text(c, sop->result, sb.p, b);
        else buf_puts(b, sb.p);
        buf_puts(b, "; }\nelse ");
      }
      free(sb.p);
    }
    /* a bare puts writes the newline and answers nil (#6158) */
    if (sp_streq(name, "puts")) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_PUTS, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) { "
                    "sp_File_puts((sp_File *)_t%d.v.p, \"\"); ", tv, tv, tv);
      if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_nil(); ", tr);
      buf_puts(b, "}\nelse ");
    }
    if (io_op) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IOZ, -1, TY_UNKNOWN, PC_SAME);
      char ioex[128];
      snprintf(ioex, sizeof ioex, "%s((sp_File *)_t%d.v.p%s)", io_op->fn, tv,
               io_op->tail);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_IO) { ",
                 tv, tv);
      /* the value only lands when the result slot can hold it: poly boxes
         it, an exactly matching concrete slot takes it raw, anything else
         keeps the call for its effect and leaves the seed */
      if (ret == TY_POLY && sp_streq(name, "getbyte"))
        buf_printf(b, "_t%d = sp_box_oint(%s)", tr, ioex);
      else if (ret == TY_POLY && io_op->result != TY_VOID) {
        buf_printf(b, "_t%d = ", tr);
        emit_boxed_text(c, io_op->result, ioex, b);
      }
      /* an oint slot (lso) takes a never-nil answer lifted; getbyte's
         answer is an oint already */
      else if (lso && ret == io_op->result && !sp_streq(name, "getbyte"))
        buf_printf(b, "_t%d = sp_oint_of(%s)", tr, ioex);
      else if (ret == io_op->result && io_op->result != TY_VOID)
        buf_printf(b, "_t%d = %s", tr, ioex);
      else buf_puts(b, ioex);
      buf_puts(b, "; }\nelse ");
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
      int tn = snprintf(tg, sizeof tg, " || _t%d.cls_id == SP_BUILTIN_RANGE || _t%d.cls_id == SP_BUILTIN_STR_RANGE", tv, tv);
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
  { int sci = comp_core_class(c, CORE_STRING);
    if (sci >= 0 && call_target(c, id, sci, name, 0, NULL) >= 0) str_reopen_owns = 1; }
  if (argc == 0 && !str_reopen_owns) {
    const BuiltinZeroOp *str_op = bop_zero_find(TY_STRING, name);
    /* The result has to fit the slot the dispatch assigns into. */
    if (str_op && (ret == TY_POLY || ret == str_op->result)) {
      /* #chr is Integer#chr on an int tag: stringifying first turns
         65.chr into "65".chr == "6" (#3328). */
      if (sp_streq(name, "chr")) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INT_CHR, -1, TY_UNKNOWN, PC_SAME);
        buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = ", tv, tr);
        if (ret == TY_STRING) buf_printf(b, "sp_int_chr(_t%d.v.i)", tv);
        else buf_printf(b, "sp_box_str_fresh(sp_int_chr(_t%d.v.i))", tv);
        buf_puts(b, "; }\nelse ");
      }
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STRT, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
      if (ret != TY_POLY) buf_printf(b, "%s(_t%d.v.s)", str_op->fn, tv);
      else if (str_op->result == TY_INT_ARRAY) buf_printf(b, "sp_box_int_array(%s(_t%d.v.s))", str_op->fn, tv);
      else if (str_op->result == TY_STR_ARRAY) buf_printf(b, "sp_box_str_array(%s(_t%d.v.s))", str_op->fn, tv);
      else buf_printf(b, "sp_box_str(%s(_t%d.v.s))", str_op->fn, tv);
      buf_puts(b, "; }\nelse ");
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
  free(hob.p);
  free(hcb.p);
}

/* The proc a boxed callable's pre-arm hands a `call`/`()`/`[]` with a
   block (the dispatch's own, `have`, or one built here), or -1 */
static int callable_prearm_blk(Compiler *c, int id, const char *name, int have) {
  if (!name || !is_call_alias(name) || nt_ref(c->nt, id, "block") < 0) return -1;
  return poly_call_blk_proc(c, id, have);
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
    if (cblk0 >= 0 || (nt_ref(nt, id, "block") >= 0 && g_yield_proc_ref)) {
      int npc0 = 0;
      const PolyCand *pc0 = comp_poly_candidates(c, name, &npc0);   /* (#4966) */
      for (int ki = 0; ki < npc0 && blk_tmp0 < 0; ki++) {
        int k = pc0[ki].cls;
        /* a reopened builtin (Hash, Array, String...) is never `.new`ed, and its arm runs too */
        if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) continue;
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
          blk_tmp0 = hoist_dispatch_blk_proc(c, id, cblk0);
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
         user-class switch's NoMethodError (#4840). So does a String Range
         (a Range of Symbols too), over its members. */
      buf_puts(b, "if (");
      emit_penum_kind_test(b, tv);
      buf_printf(b, ") { _t%d = ", tr);
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
  /* each_index with a block on a boxed Array, in a dispatch that exists
     because a user class also defines it: the shared proc is driven over
     the indices, and the Array answers itself (the pre-arm above has no
     walk for it) */
  if (sp_streq(name, "each_index") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    if (blk_tmp0 < 0) {
      int cblk3 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      if (cblk3 >= 0) blk_tmp0 = hoist_block_proc(c, cblk3);
    }
    if (blk_tmp0 >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ENUM_PROC, -1, TY_UNKNOWN, PC_SAME);
      int ti = ++g_tmp;
      char icall[256];
      snprintf(icall, sizeof icall, "({ for (sp_int _t%d = 0; _t%d < sp_poly_length(_t%d); _t%d++)"
                                    " sp_penum_call1(_t%d, sp_box_int(_t%d)); _t%d; })",
               ti, ti, tv, ti, blk_tmp0, ti, tv);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)) { _t%d = ", tv, tv, tr);
      if (ret == TY_POLY) buf_puts(b, icall);
      else emit_unbox_text(c, ret, icall, b);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* times with a block on a boxed Integer, in a dispatch that exists
     because a user class also defines it (an options object's `times`
     reader): the shared proc is driven over 0...n, and the Integer
     answers itself */
  if (sp_streq(name, "times") && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    if (blk_tmp0 < 0) {
      int cblk4 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      if (cblk4 >= 0) blk_tmp0 = hoist_block_proc(c, cblk4);
    }
    if (blk_tmp0 >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ENUM_PROC, -1, TY_UNKNOWN, PC_SAME);
      int ti = ++g_tmp;
      char tcall[256];
      snprintf(tcall, sizeof tcall, "({ for (sp_int _t%d = 0; _t%d < _t%d.v.i; _t%d++)"
                                    " sp_penum_call1(_t%d, sp_box_int(_t%d)); _t%d; })",
               ti, ti, tv, ti, blk_tmp0, ti, tv);
      buf_printf(b, "if (_t%d.tag == SP_TAG_INT) { _t%d = ", tv, tr);
      if (ret == TY_POLY) buf_puts(b, tcall);
      else emit_unbox_text(c, ret, tcall, b);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* each_line, each_char, each_byte or each_codepoint with a block on a
     boxed String, in a dispatch that exists because a user class also
     defines it (a buffer that hands the block on): the shared proc is
     driven over the lines or characters, each a String of its own, or over
     the bytes or codepoints as Integers, and the String answers itself.
     Without the arm a String reaching `t.each_byte(&b)` fell to the raise. */
  int str_walk = sp_streq(name, "each_line") || sp_streq(name, "each_char");
  int int_walk = sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint");
  if ((str_walk || int_walk) && argc == 0 && nt_ref(nt, id, "block") >= 0) {
    if (blk_tmp0 < 0) {
      int cblk5 = resolve_forwarded_block(c, nt_ref(nt, id, "block"));
      if (cblk5 >= 0) blk_tmp0 = hoist_block_proc(c, cblk5);
    }
    if (blk_tmp0 >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ENUM_PROC, -1, TY_UNKNOWN, PC_SAME);
      int ta = ++g_tmp, ti = ++g_tmp;
      char lcall[400];
      const char *walk = sp_streq(name, "each_line") ? "sp_str_lines" : sp_streq(name, "each_char") ? "sp_str_chars"
                       : sp_streq(name, "each_byte") ? "sp_str_bytes" : "sp_str_codepoints";
      if (str_walk)
        snprintf(lcall, sizeof lcall, "({ sp_StrArray *_t%d = %s(sp_poly_recv_s(_t%d, \"%s\")); SP_GC_ROOT(_t%d);"
                                      " for (sp_int _t%d = 0; _t%d < sp_StrArray_length(_t%d); _t%d++)"
                                      " sp_penum_call1(_t%d, SP_SEBOX(sp_StrArray_get_e(_t%d, _t%d))); _t%d; })",
                 ta, walk, tv, name, ta, ti, ti, ta, ti, blk_tmp0, ta, ti, tv);
      else
        snprintf(lcall, sizeof lcall, "({ sp_IntArray *_t%d = %s(sp_poly_recv_s(_t%d, \"%s\")); SP_GC_ROOT(_t%d);"
                                      " for (sp_int _t%d = 0; _t%d < sp_IntArray_length(_t%d); _t%d++)"
                                      " sp_penum_call1(_t%d, sp_box_int(sp_IntArray_get(_t%d, _t%d))); _t%d; })",
                 ta, walk, tv, name, ta, ti, ti, ta, ti, blk_tmp0, ta, ti, tv);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) { _t%d = ", tv, tv, tr);
      if (ret == TY_POLY) buf_puts(b, lcall);
      else emit_unbox_text(c, ret, lcall, b);
      buf_puts(b, "; }\nelse ");
    }
  }
  /* a boxed Proc/Curry/Method in a slot a user `call`/`[]` shadows (#4395) */
  if (emit_poly_callable_prearm(c, name, 0, NULL, NULL, NULL, tv, tr, ret, 0,
                                callable_prearm_blk(c, id, name, blk_tmp0), b) && g_plan_check)
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
  /* ...and an oint result slot (plan_slot_oint) takes it lifted */
  int lso = ret == TY_INT && plan_slot_oint(c, id, ret);
  const char *bopen = (ret == TY_POLY) ? "sp_box_int(" : lso ? "sp_oint_of(" : "";
  const char *bclose = (ret == TY_POLY || lso) ? ")" : "";
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
    /* what the re-entered emission hoists -- a temp of the receiver, an
       argument check on it -- reads `_t<tv>`, which this case's dispatch
       declares: it runs inside the case, not ahead of the dispatch */
    Buf pre9; memset(&pre9, 0, sizeof pre9);
    Buf *sv_pre9 = g_pre; g_pre = &pre9;
    if (bt9 != TY_UNKNOWN && bt9 != TY_POLY) {
      Buf nb9; memset(&nb9, 0, sizeof nb9);
      emit_expr(c, id, &nb9);
      emit_boxed_text(c, bt9, nb9.p ? nb9.p : "0", &ib9);
      free(nb9.p);
    }
    else emit_boxed(c, id, &ib9);
    g_pre = sv_pre9;
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
      if (pre9.p && pre9.len) buf_printf(b, " { %s _t%d = %s; } break;", pre9.p, tr, ib9.p);
      else buf_printf(b, " _t%d = %s; break;", tr, ib9.p);
    }
    free(ib9.p); free(pre9.p);
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
  if (!obj_default_done && (is_text_conversion(name))) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_TO_S, -1, TY_UNKNOWN, PC_SAME);
    /* (to_s as the program calls it: a Symbol's is a new String) */
    const char *pfn = sp_streq(name, "to_s") ? "sp_poly_to_s_call" : "sp_poly_inspect";
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_STRBUF && repr_boxed_to_s_operand(c, id) >= 0) {
      /* (a String subclass instance's to_s is a new String, #9968) */
      buf_printf(b, program_has_strsub(c) ? "sp_poly_is_strbuf(_t%d) ? sp_poly_to_s_strbuf(_t%d) : "
                                          : "sp_poly_is_strbuf(_t%d) ? (sp_String *)_t%d.v.p : ", tv, tv);
      buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && (_t%d.cls_id == SP_BUILTIN_EXCEPTION || "
                    "sp_is_exc_subclass_cls(_t%d.cls_id))) ? "
                    "sp_exc_kept_message_handle((sp_Exception *)_t%d.v.p) : ", tv, tv, tv, tv);
      str_new_handle_open(b);
      buf_printf(b, "%s(_t%d)", pfn, tv);
      str_new_handle_close(b);
    }
    /* A boxed to_s answers a String receiver itself: with sharing, the
       box it holds is the answer, not a copy of its bytes. */
    else if (ret == TY_POLY && is_to_s_name(name))
      buf_printf(b, program_has_strsub(c) ? "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) ? sp_poly_to_s_box(_t%d) : sp_box_str(%s(_t%d))"
                                          : "(_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) ? _t%d : sp_box_str(%s(_t%d))",
                 tv, tv, tv, pfn, tv);
    else if (ret == TY_POLY) buf_printf(b, "sp_box_str(%s(_t%d))", pfn, tv);
    /* (one-String mode: the bytes of a String subclass instance lead back
       to its handle, so its to_s, a new String, is a copy of them, #9968) */
    else if (program_has_strsub(c) && is_to_s_name(name))
      buf_printf(b, "(sp_poly_is_str_subclass(_t%d) ? sp_str_dup(%s(_t%d)) : %s(_t%d))", tv, pfn, tv, pfn, tv);
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
    if (is_succ_alias(name))
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
    /* an oint slot (a target of the name can answer nil) takes the answer
       with its nil */
    int so = plan_slot_oint(c, id, ret);
    if (int_valued) {
      if (ret == TY_POLY) emit_boxed_text(c, TY_INT, nv, b);
      else plan_put_plain(so, ret, nv, b);
    }
    else if (ret == TY_POLY) buf_puts(b, nv);
    else plan_put_boxed(c, so, ret, nv, b);
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
      (is_numeric_conversion(name))) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_TO_IF, -1, TY_UNKNOWN, PC_SAME);
    char cv[64];
    snprintf(cv, sizeof cv, "%s(_t%d)", sp_streq(name, "to_i") ? "sp_poly_to_i_meth" : "sp_poly_to_f_meth", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, sp_streq(name, "to_i") ? TY_INT : TY_FLOAT, cv, b);
    else plan_put_plain(plan_slot_oint(c, id, ret), ret, cv, b);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* Blockless any? / none?, which every builtin container answers. The
     switch carries an arm per user class owning the name and none for a
     builtin, so a slot that may hold an Array OR such an object raised
     NoMethodError on the Array -- naming Array, whose method it is (#4264).
     In the DEFAULT, like the conversions around it: a class that defines
     the name has its own case and never arrives. They ask whether an
     element is truthy, which the boxed call's element walk answers
     (emit_poly_quantifier_walk): asking whether there is one answered
     `[nil].any?` true and an Integer Range's false. */
  if (!obj_default_done && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      (sp_streq(name, "any?") || sp_streq(name, "none?"))) {
        if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_ANY_NONE, -1, TY_UNKNOWN, PC_SAME);
    int tn = ++g_tmp, tcnt = ++g_tmp, ti = ++g_tmp;
    char av[96], ans[64];
    buf_printf(b, " default: { SP_GC_ROOT_RBVAL(_t%d); ", tv);
    emit_poly_quantifier_walk(c, id, tv, -1, tn, tcnt, ti, name, ans, sizeof ans, b);
    snprintf(av, sizeof av, "(%s)", ans);
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_BOOL, av, b);
    else buf_puts(b, av);
    buf_puts(b, "; break; }");
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
  /* display, the same shape: a class that defines it has its own case, and
     every other receiver is Kernel#display's, to_s with no newline and nil */
  if (!obj_default_done && argc == 0 && sp_streq(name, "display") && nt_ref(nt, id, "block") < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_DISPLAY, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " default: fputs(sp_poly_to_s(_t%d), stdout);", tv);
    if (ret == TY_POLY) buf_printf(b, " _t%d = sp_box_nil();", tr);
    buf_puts(b, " break;");
    obj_default_done = 1;
  }
  /* hash, the same shape: the dispatch exists because a class answers hash
     with a reader (emit_call_identity_arms), and every other receiver --
     a builtin, an object with a def of hash -- has the key the containers
     use, which reaches a def through the runtime's hook */
  if (!obj_default_done && argc == 0 && sp_streq(name, "hash") && nt_ref(nt, id, "block") < 0 &&
      (ret == TY_INT || ret == TY_POLY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_D_HASH, -1, TY_UNKNOWN, PC_SAME);
    char hv[64];
    snprintf(hv, sizeof hv, "sp_rbval_hash_key(_t%d)", tv);
    buf_printf(b, " default: _t%d = ", tr);
    if (ret == TY_POLY) emit_boxed_text(c, TY_INT, hv, b);
    else plan_put_plain(plan_slot_oint(c, id, ret), ret, hv, b);
    buf_puts(b, "; break;");
    obj_default_done = 1;
  }
  /* The blockless index enumerators, same shape: an Array reaching this
     dispatch still answers them with an Enumerator. Given a block, the
     builtin containers' pre-arm (sp_poly_enum_proc) runs it; an arm here
     stored the Enumerator into the block form's result slot, and the C did
     not compile. */
  if (argc == 0 && (is_indexed_each(name)) && nt_ref(nt, id, "block") < 0) {
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
      else plan_put_boxed(c, plan_slot_oint(c, id, ret), is_scalar_ret(ret) ? ret : TY_INT, qv, b);
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
    if (ret == TY_POLY) buf_printf(b, "SP_STR_BOX(%s)", rv);
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
                 (comp_ntype(c, argv[0]) == TY_INT || repr_of(c, argv[0]).kind == RK_BOXED);
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
  int has_splat_arg = nt_argv_has_splat(nt, argv, argc);
  int is_pdelete = sp_streq(name, "delete") && argc == 1 && !has_splat_arg;
  int is_pdig = sp_streq(name, "dig") && argc >= 1 && !has_splat_arg;
  int is_pvalues_at = sp_streq(name, "values_at") && argc >= 1 && !has_splat_arg;
  /* `xs.first(n)` / `xs.last(n)` on a poly value that is a container at run
     time: the zero-arg forms have had an arm for a long time, the counted
     ones fell through to the raise (#3781 follow-up). */
  int is_pfirstn = (is_endpoint_query(name)) &&
                   argc == 1 && !has_splat_arg;
  int is_include = is_key_query(name) && argc == 1;
  /* intersect? on a poly value that is a builtin array. The typed-receiver
     forms resolve, so only the union receiver was missing an arm and the
     call raised NoMethodError naming Array -- which is what the receiver
     was (#3414). Every array kind coerces to a poly array, so one arm
     covers them all rather than one per element type. */
  int is_intersect = sp_streq(name, "intersect?") && argc == 1;
  /* index / rindex / find_index with a VALUE argument on a poly value that
     is an array at run time. include? has had this arm for a long time; the
     index family answering the position rather than a bool was simply never
     added, so the call raised NoMethodError naming Array (#3409). A block
     beside the value is not used (CRuby warns "given block not used"), so
     these builtin arms take the call with or without one. */
  int is_arr_index = (is_index_query(name) && argc == 1) ||
                     /* the two-argument form is String's alone -- Array#index
                        takes one argument -- so only the default arm below
                        answers it, and the array cases stay out (#4149). */
                     ((is_string_index(name)) && argc == 2);
  /* push/<</append on a poly value that is actually a builtin array: the
     array-mutate statement path skips it when a user class also defines the
     name (the value could be that object), so the switch needs a builtin-array
     arm or the append is silently dropped. sp_poly_shl handles every array
     kind; the user arms above cover the object case. */
  int is_push = is_push_alias(name) && argc >= 1;
  /* unshift/prepend are the same arm at the other end: without one they fell
     to the switch's NoMethodError default on a genuine Array (#4320). */
  int is_unshift = (is_prepend_alias(name)) && argc >= 1;
  /* delete(chars) with a string arg: the poly value may be a string even
     when a user class also defines `delete` (the bundled Set does), so the
     switch needs a TAG_STR pre-arm routing to String#delete (doom's
     `data[offset, 8].delete("\x00").upcase` WAD name fields). */
  int is_strdel = sp_streq(name, "delete") && argc == 1 &&
                  comp_ntype(c, argv[0]) == TY_STRING;
  /* partition / rpartition on a TAG_STR receiver. They have no poly arm of
     their own ahead of the name-collision test -- which is why `split`,
     `upcase` and `strip` survived a same-named user method and these did
     not -- so a genuine String fell to the switch's raising default and
     answered NoMethodError, naming String, for a method String has
     (#4413). A class nothing instantiates no longer takes the name away;
     this is the same hole for one that IS instantiated. */
  int is_strpart = (is_partition_family(name)) &&
                   argc == 1 && comp_ntype(c, argv[0]) == TY_STRING;
  /* The multi-set forms of count/delete/squeeze (String's alone) when a
     user class also owns the name: the switch needs a TAG_STR pre-arm or
     a genuine String receiver falls to its NoMethodError default, the
     same hole the single-set delete had (#4195). String-typed sets only:
     the temps below carry them as const char *. */
  int is_strsetop_n = ((sp_streq(name, "count") || sp_streq(name, "squeeze") || sp_streq(name, "strip") ||
                          sp_streq(name, "lstrip") || sp_streq(name, "rstrip"))
                         ? argc >= 1     /* their 1-set form has no other pre-arm */
                         : sp_streq(name, "delete") && argc >= 2) &&
                      argc <= 8 && !has_splat_arg &&
                      nt_ref(nt, id, "block") < 0;
  if (is_strsetop_n)
    for (int a = 0; a < argc; a++)
      if (comp_ntype(c, argv[a]) != TY_STRING) { is_strsetop_n = 0; break; }
  /* Hash#store when a user class also owns the name: a boxed hash takes
     the runtime store, anything else its own arm or the default (#4195). */
  int is_pstore = sp_streq(name, "store") && argc == 2 && !has_splat_arg &&
                  nt_ref(nt, id, "block") < 0;
  /* `s[i] = v` on a boxed String when a user class owns `[]=` (any Struct
     does): the switch has no String arm, so the store was dropped -- see
     emit_poly_str_aset_prearm. An Integer index, a String or Regexp key,
     or a boxed one (an Integer, a Range, a String or a Regexp at run time),
     the forms the builtin path writes back. */
  TyKind straset_k = argc == 2 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
  int is_straset = is_element_access(name) && is_store_alias(name) && argc == 2 && !has_splat_arg &&
                   nt_ref(nt, id, "block") < 0 &&
                   (straset_k == TY_INT || straset_k == TY_STRING || straset_k == TY_REGEX || straset_k == TY_POLY);
  /* split(sep) on a TAG_STR receiver, when a user class also owns `split`
     (the bundled Pathname does) and the dispatch therefore lost the String
     arm. Same hole #3394 closed for the zero-arg form (#3401). */
  int is_strsplit = sp_streq(name, "split") && argc == 1 &&
                    nt_ref(nt, id, "block") < 0 &&
                    (comp_ntype(c, argv[0]) == TY_STRING ||
                     comp_ntype(c, argv[0]) == TY_NIL ||
                     comp_ntype(c, argv[0]) == TY_REGEX);
  int is_pred = poly_pred_kind(name, argc);
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
  { NodeKind l_k = nt_kind(nt, argv[argc - 1]);
    if (l_k == NK_KeywordHashNode) {
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
    if (!has_splat_arg && kwh < 0 && cplan_struct_aset(c, k, name, argc)) { ncand++; continue; }
    /* a native class's methods are its declared bindings (#4504), as
       are those of a subclass of one where it does not answer the name
       itself (#9269) */
    int nk = comp_native_binding_class(c, k, name);
    if (nk >= 0) {
      if (kw_pos && !has_splat_arg && c->classes[k].instantiated) {
        for (int n = pos_argc; n <= argc; n++) {
          int nmi = comp_native_method_find(c, nk, name, n, 0);
          if (nmi >= 0 && native_takes(&c->native_methods[nmi], n)) { ncand++; break; }
        }
      }
      continue;
    }
    int mi = call_target(c, id, k, name, 0, NULL);
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
     (issue #2457, the family2 nilable value-method dispatch gap). Beside
     user classes that define strftime the Time arm joins theirs, and the
     switch's own default raises (#7334): a program-defined Date left a real
     Time with no arm at all. */
  /* the format may be boxed too: a String formats, anything else raises the
     TypeError CRuby's does (sp_poly_arg_str_chk) */
  int is_strftime = sp_streq(name, "strftime") && argc == 1 &&
                    (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_POLY);
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
    if (jat == TY_STRBUF) jat = TY_STRING;
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
    if (pat == TY_STRBUF) pat = TY_STRING;
    if (!(pat == TY_STRING || pat == TY_POLY || pat == TY_UNKNOWN)) is_ppack = 0;
  }
  /* Both arms answer a String, so they can only be emitted where the result
     temp can hold one: the call typed from the user method alone (a Crate,
     say) has no room for the builtin answer, and assigning it there did not
     compile. Standing down leaves the builtin case at the raise, which is the
     trade the dig / values_at arms below already make (#4319). */
  if (is_ppack || is_pjoin) {
    TyKind pjr = repr_of(c, id).as_ty;
    if (!(pjr == TY_POLY || pjr == TY_STRING || pjr == TY_UNKNOWN)) {
      is_ppack = 0; is_pjoin = 0;
    }
  }
  /* a class of the program's own that defines cover? (a wrapper of a
     Range) takes its arm after the Range pre-arm, when the call answers a
     truth value or a box the pre-arm's answer fits */
  TyKind cvr = repr_of(c, id).as_ty;
  int is_cover = sp_streq(name, "cover?") && argc == 1 &&
                 (!recv_user_defines(c, name) || cvr == TY_BOOL || cvr == TY_POLY);
  int is_gcdlcm = sp_streq(name, "gcdlcm") && argc == 1 && !recv_user_defines(c, name);
  /* try_convert on a class known only at run time: a constant receiver
     has its typed emitter, but `[Array, 0][0].try_convert(x)` reached no
     arm at all and the call lowered to the unresolved-method raise. The
     runtime answers by the class's name, as the typed emitters answer by
     the constant's (#2325, #2585). */
  int is_ctryconv = sp_streq(name, "try_convert") && argc == 1 && !has_splat_arg && kw_pos &&
                    nt_ref(nt, id, "block") < 0 && !recv_user_defines(c, name) &&
                    repr_of(c, id).kind == RK_BOXED;
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
  s->straset = is_straset;
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

/* `s[i] = v` on a String that reaches a poly dispatch because a user class
   owns `[]=`, as every Struct does. The switch has an arm per class and none
   for a String: a shared String fell to the default, whose sp_poly_set_poly
   has no String arm, and the store was lost. The String takes the builtin
   store (sp_poly_arr_widen_and_set for an Integer index, sp_poly_str_aset_key
   for a String, Regexp or boxed key). A shared String changes in place, in an arm
   ahead of the switch. A plain one splices into a fresh buffer, which a
   receiver that is a local or an instance variable gets written back after
   the dispatch (emit_poly_str_aset_writeback), as the builtin path writes it
   back when no user class owns the name (#3172); the arm itself reads only
   the dispatch's temps, so pd_hoist can still move the switch out of line.
   Another receiver keeps what the builtin path does for it. */
static int str_aset_writes_back(Compiler *c, int recv) {
  NodeKind rk = nt_kind(c->nt, recv);
  return rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode;
}
/* The store call on the receiver `rv`, into out. */
static void str_aset_store_text(Compiler *c, const PolyTemps *T, const char *rv, Buf *out) {
  TyKind vty = T->atmp_ty[1], kty = T->atmp_ty[0];
  char k0[48], v0[24];
  snprintf(k0, sizeof k0, "_t%d", T->atmp[0]);
  snprintf(v0, sizeof v0, "_t%d", T->atmp[1]);
  /* an Integer index rides raw (unboxed from a poly temp in promote mode),
     a String or Regexp key boxed */
  int int_key = comp_ntype(c, T->argv[0]) == TY_INT;
  buf_printf(out, "%s(%s, ", int_key ? "sp_poly_arr_widen_and_set" : "sp_poly_str_aset_key", rv);
  if (int_key && kty == TY_POLY) buf_printf(out, "sp_poly_arg_i(%s)", k0);
  else if (int_key || kty == TY_POLY) buf_puts(out, k0);
  else emit_boxed_text(c, kty, k0, out);
  buf_puts(out, ", ");
  if (vty == TY_POLY) buf_puts(out, v0);
  else emit_boxed_text(c, vty, v0, out);
  buf_puts(out, ")");
}
void emit_poly_str_aset_prearm(Compiler *c, int recv, const PolySpecialsN *ps, const PolyTemps *T, Buf *b) {
  if (!ps->straset || recv < 0) return;
  if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_ASET, -1, TY_UNKNOWN, PC_SAME);
  int tv = T->tv, tr = T->tr;
  TyKind ret = T->ret, vty = T->atmp_ty[1];
  Buf st; memset(&st, 0, sizeof st);
  char rv[24]; snprintf(rv, sizeof rv, "_t%d", tv);
  str_aset_store_text(c, T, rv, &st);
  buf_puts(b, "if ("); emit_poly_str_guard(c, tv, "[]=", b); buf_puts(b, ") { ");
  /* a plain String that is written back is stored after the dispatch */
  if (str_aset_writes_back(c, recv)) buf_printf(b, "if (_t%d.tag != SP_TAG_STR) ", tv);
  buf_printf(b, "%s;", st.p);
  if (tr >= 0) {
    char v0[24]; snprintf(v0, sizeof v0, "_t%d", T->atmp[1]);
    buf_printf(b, " _t%d = ", tr);
    if (ret == vty) buf_puts(b, v0);
    else {
      Buf vb; memset(&vb, 0, sizeof vb);
      if (vty == TY_POLY) buf_puts(&vb, v0); else emit_boxed_text(c, vty, v0, &vb);
      if (ret == TY_POLY) buf_puts(b, vb.p);
      else emit_unbox_text(c, is_scalar_ret(ret) ? ret : TY_INT, vb.p, b);
      free(vb.p);
    }
    buf_puts(b, ";");
  }
  buf_puts(b, " }\nelse ");
  free(st.p);
}
/* The dispatch's receiver: a String store whose key or value rebinds a
   local or instance variable receiver (aset_recv_rebinds_only) reads it
   ahead of them, and of a prelude they move ahead of the statement, as
   CRuby reads it; any other call reads it in place. */
void emit_poly_str_aset_recv(Compiler *c, int recv, const PolySpecialsN *ps, const int *argv, Buf *b) {
  if (ps->straset && ps->kw_pos && aset_recv_rebinds_only(c, recv, argv[0], argv[1])) emit_aset_recv_read(c, recv, b);
  else emit_expr(c, recv, b);
}
/* After the dispatch: a plain String receiver that is a local or an
   instance variable takes the spliced String (see the pre-arm above). The
   receiver was read ahead of the key and value. When one of them may give
   the variable another value and cannot mutate it (`s[(s = t; 0)] = v`,
   aset_recv_rebinds_only), CRuby changes the String read first and keeps
   the new binding: the splice goes back only to a variable that still
   holds it. When one may mutate it (`s[(s[0] = x; 1)] = v`,
   aset_recv_may_mutate), the String read first is stale: the store reads
   the variable after them, as the store without a user class's `[]=`
   does, while it still holds a String. */
void emit_poly_str_aset_writeback(Compiler *c, int recv, const PolySpecialsN *ps, const PolyTemps *T, Buf *b) {
  if (!ps->straset || recv < 0 || !str_aset_writes_back(c, recv)) return;
  Buf st; memset(&st, 0, sizeof st);
  Buf var = expr_buf(c, recv);
  const char *vp = var.p ? var.p : "";
  int mut = aset_recv_may_mutate(c, recv, T->argv[0], T->argv[1]);
  char rv[24]; snprintf(rv, sizeof rv, "_t%d", T->tv);
  str_aset_store_text(c, T, mut ? vp : rv, &st);
  buf_printf(b, " if (_t%d.tag == SP_TAG_STR", T->tv);
  if (mut) buf_printf(b, " && (%s.tag == SP_TAG_STR || sp_poly_is_strbuf(%s))", vp, vp);
  else if (aset_recv_rebinds_only(c, recv, T->argv[0], T->argv[1]))
    buf_printf(b, " && %s.tag == SP_TAG_STR && %s.v.s == _t%d.v.s", vp, vp, T->tv);
  buf_printf(b, ") %s = %s;", vp, st.p);
  free(st.p);
  free(var.p);
}

/* The tag pre-arms of a poly dispatch with arguments, ahead of its cls_id
   switch: a Range's cover?, a class's try_convert, an Integer's gcdlcm and
   bit reference, a String's unpack1, include?, delete, partition, count
   and squeeze, encode, split and index, a Hash's store. */
/* The methods sp_trange_send answers for a boxed Range of Times */
int trange_poly_name(const char *name, int argc) {
  static const char *const z[] = { "begin", "first", "end", "last", "min", "max", "exclude_end?", "to_s",
                                   "inspect", "hash", "frozen?", "to_a", "entries", "size", "count", "sum", NULL };
  static const char *const o[] = { "cover?", "include?", "member?", "===", "==", "eql?", "overlap?", NULL };
  const char *const *l = argc == 0 ? z : argc == 1 ? o : NULL;
  for (; l && *l; l++) if (sp_streq(name, *l)) return 1;
  return 0;
}
void emit_poly_prearms_n(Compiler *c, const char *name, const PolySpecialsN *ps, const PolyTemps *T, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc, kwall = T->kwall, kwn = T->kwn;
  const int *argv = T->argv, *atmp = T->atmp, *kwels = T->kwels, *kwtmp = T->kwtmp;
  const TyKind *atmp_ty = T->atmp_ty, *kwty = T->kwty;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  const char *idxref = T->idxref;
  /* Byte-taking builtin arms read the held handle; boxed arms keep it. */
  char strarg[64];
  snprintf(strarg, sizeof strarg, argc > 0 && atmp_ty[0] == TY_STRBUF
           ? "sp_strbuf_read_pub(_t%d)"
           : argc > 0 && atmp_ty[0] == TY_STRING ? "SP_STR_B(_t%d)" : "_t%d",   /* (one representation: a handle held) */
           argc > 0 ? atmp[0] : 0);
  int is_cover = ps->cover, is_ctryconv = ps->ctryconv, is_gcdlcm = ps->gcdlcm, is_ppack = ps->ppack;
  int is_include = ps->include, is_strdel = ps->strdel, is_strpart = ps->strpart;
  int is_strsetop_n = ps->strsetop_n, is_pstore = ps->pstore, is_strencode = ps->strencode;
  int is_strsplit = ps->strsplit, is_index = ps->index;
  /* a boxed Range of Times answers its methods through sp_trange_send */
  if (trange_poly_name(name, pos_argc) && kwn == 0 && pos_argc == argc && !nt_argv_has_splat(nt, argv, argc) &&
      (ret == TY_POLY || is_scalar_ret(ret))) {
    Buf call; memset(&call, 0, sizeof call);
    buf_printf(&call, "sp_trange_send((sp_TimeRange *)_t%d.v.p, \"%s\", %d, ", tv, name, pos_argc);
    if (pos_argc == 0) buf_puts(&call, "sp_box_nil()");
    else if (atmp_ty[0] == TY_POLY) buf_printf(&call, "_t%d", atmp[0]);
    else if (atmp_ty[0] == TY_REGEX) buf_printf(&call, "sp_box_regexp(_t%d)", atmp[0]);
    else { char an[24]; snprintf(an, sizeof an, "_t%d", atmp[0]); emit_boxed_text(c, atmp_ty[0], an, &call); }
    buf_puts(&call, ")");
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_TIME_RANGE && _t%d.v.p) { _t%d = ", tv, tv, tv, tr);
    if (ret == TY_POLY) buf_puts(b, call.p); else emit_unbox_text(c, ret, call.p, b);
    buf_puts(b, "; }\nelse ");
    /* ...and a Range of objects through sp_orange_send, the same names but size and sum */
    if (!sp_streq(name, "size") && !sp_streq(name, "sum")) {
      Buf oc; memset(&oc, 0, sizeof oc);
      buf_printf(&oc, "sp_orange_send((sp_ObjRange *)_t%d.v.p%s", tv, call.p + strlen("sp_trange_send((sp_TimeRange *)_t") + snprintf(NULL, 0, "%d", tv) + strlen(".v.p"));
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_OBJ_RANGE && _t%d.v.p) { _t%d = ", tv, tv, tv, tr);
      if (ret == TY_POLY) buf_puts(b, oc.p); else emit_unbox_text(c, ret, oc.p, b);
      buf_puts(b, "; }\nelse ");
      free(oc.p);
    }
    free(call.p);
  }
  /* Range#cover? on a runtime Range receiver (#3234) */
  if (is_cover) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_COVER, -1, TY_UNKNOWN, PC_SAME);
    const char *fn = atmp_ty[0] == TY_POLY ? "cover_poly" : atmp_ty[0] == TY_FLOAT ? "cover_f" : "include";
    /* a Range argument: whether both its ends lie inside, as the typed
       cover?(range) answers -- the Integer test took the sp_Range as a value */
    if (atmp_ty[0] == TY_RANGE)
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)"
                    " { _t%d = %ssp_range_cover_rng(*(sp_Range *)_t%d.v.p, _t%d)%s; }\nelse ",
                 tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, atmp[0],
                 ret == TY_POLY ? ")" : "");
    /* ...and a boxed one, which sp_range_cover_poly (include?'s too) does
       not read as a Range */
    else if (atmp_ty[0] == TY_POLY) {
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)"
                    " { _t%d = %s(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE && _t%d.v.p"
                    " ? sp_range_cover_rng(*(sp_Range *)_t%d.v.p, *(sp_Range *)_t%d.v.p)"
                    " : sp_range_cover_poly((sp_Range *)_t%d.v.p, _t%d))%s; }\nelse ",
                 tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "",
                 atmp[0], atmp[0], atmp[0], tv, atmp[0], tv, atmp[0],
                 ret == TY_POLY ? ")" : "");
      /* a boxed String Range: a String by comparison, a String Range by its
         ends (sp_srange_cover_poly); it answered false */
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_STR_RANGE && _t%d.v.p)"
                    " { _t%d = %ssp_srange_cover_poly(_t%d, _t%d)%s; }\nelse ",
                 tv, tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, atmp[0], ret == TY_POLY ? ")" : "");
    }
    /* an argument of another class (a String, an Array, nil) compares
       boxed, as a boxed one does: it went into sp_range_include's sp_int
       slot raw, and did not build */
    else if (atmp_ty[0] != TY_INT && atmp_ty[0] != TY_FLOAT) {
      char tn7[24]; snprintf(tn7, sizeof tn7, "_t%d", atmp[0]);
      Buf ab7; memset(&ab7, 0, sizeof ab7); emit_boxed_text(c, atmp_ty[0], tn7, &ab7);
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)"
                    " { _t%d = %ssp_range_cover_poly((sp_Range *)_t%d.v.p, %s)%s; }\nelse ",
                 tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, ab7.p ? ab7.p : "sp_box_nil()",
                 ret == TY_POLY ? ")" : "");
      /* a String Range covers by string comparison, as its === does, and a
         String Range by its ends (sp_srange_cover_poly) */
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_STR_RANGE)"
                    " { _t%d = %ssp_srange_cover_poly(_t%d, %s)%s; }\nelse ",
                 tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, ab7.p ? ab7.p : "sp_box_nil()",
                 ret == TY_POLY ? ")" : "");
      free(ab7.p);
    }
    else
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE)"
                  " { _t%d = %ssp_range_%s((sp_Range *)_t%d.v.p, _t%d)%s; }\nelse ",
               tv, tv, tr,
               ret == TY_POLY ? "sp_box_bool(" : "", fn, tv, atmp[0],
               ret == TY_POLY ? ")" : "");
    /* a Float range covers by value (it fell through to false) */
    if (atmp_ty[0] == TY_RANGE)
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_FLOAT_RANGE && _t%d.v.p)"
                    " { _t%d = %ssp_frange_cover_rng(*(sp_FloatRange *)_t%d.v.p, _t%d)%s; }\nelse ",
                 tv, tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, atmp[0],
                 ret == TY_POLY ? ")" : "");
    if (atmp_ty[0] == TY_POLY || atmp_ty[0] == TY_FLOAT || atmp_ty[0] == TY_INT) {
      buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_FLOAT_RANGE && _t%d.v.p)"
                    " { _t%d = %s", tv, tv, tv, tr, ret == TY_POLY ? "sp_box_bool(" : "");
      /* a boxed Range argument is covered by its ends, not as a scalar */
      if (atmp_ty[0] == TY_POLY)
        buf_printf(b, "(_t%d.tag == SP_TAG_OBJ && _t%d.cls_id == SP_BUILTIN_RANGE && _t%d.v.p"
                      " ? sp_frange_cover_rng(*(sp_FloatRange *)_t%d.v.p, *(sp_Range *)_t%d.v.p)"
                      " : sp_frange_cover_poly(*(sp_FloatRange *)_t%d.v.p, _t%d))",
                   atmp[0], atmp[0], atmp[0], tv, atmp[0], tv, atmp[0]);
      /* an Integer against the Float bounds exactly (#7505) */
      else if (atmp_ty[0] == TY_INT) buf_printf(b, "sp_frange_cover_i(*(sp_FloatRange *)_t%d.v.p, _t%d)", tv, atmp[0]);
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
    /* the format read as the pack arm below reads it: a boxed argument is
       checked for a String, which CRuby's TypeError wants for anything else
       (the boxed value went straight into sp_str_unpack's const char * slot) */
    if (atmp_ty[0] == TY_POLY)
      buf_printf(&ub, "sp_PolyArray_get(sp_str_unpack(_t%d.v.s, sp_poly_arg_str_chk(_t%d)), 0)", tv, atmp[0]);
    else buf_printf(&ub, "sp_PolyArray_get(sp_str_unpack(_t%d.v.s, %s), 0)", tv, strarg);
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
    { buf_puts(b, "if ("); emit_poly_str_guard(c, tv, name, b);
      buf_printf(b, ") sp_raise_poly_nomethod(\"%s\", _t%d);\nelse ", name, tv); }
  else if (is_include && comp_ntype(c, argv[0]) == TY_STRING)
    { buf_puts(b, "if ("); emit_poly_str_guard(c, tv, name, b);
      buf_printf(b, ") { _t%d = %ssp_str_include(sp_poly_recv_s(_t%d, \"include?\"), %s)%s; }\nelse ",
                 tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, strarg,
                 ret == TY_POLY ? ")" : ""); }
  else if (is_include) {
    Buf ab6; memset(&ab6, 0, sizeof ab6);
    char tn6[24]; snprintf(tn6, sizeof tn6, "_t%d", atmp[0]);
    if (atmp_ty[0] == TY_POLY) buf_puts(&ab6, tn6);
    else emit_boxed_text(c, atmp_ty[0], tn6, &ab6);
    buf_puts(b, "if ("); emit_poly_str_guard(c, tv, name, b);
    buf_printf(b, ") { _t%d = %ssp_str_include(sp_poly_recv_s(_t%d, \"include?\"), sp_poly_arg_str_chk(sp_poly_strbuf_deref(%s)))%s; }\nelse ",
               tr, ret == TY_POLY ? "sp_box_bool(" : "", tv, ab6.p ? ab6.p : "sp_box_nil()",
               ret == TY_POLY ? ")" : "");
    free(ab6.p);
  }
  /* delete(chars) on a TAG_STR receiver: String#delete, boxed when the
     dispatch result stays poly. */
  if (is_strdel && (ret == TY_POLY || ret == TY_STRING)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_DELETE, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(sp_str_delete(_t%d.v.s, %s))", tv, strarg);
    else buf_printf(b, "sp_str_delete(_t%d.v.s, %s)", tv, strarg);
    buf_puts(b, "; }\nelse ");
  }
  /* partition / rpartition on a TAG_STR receiver: sp_str_partition answers
     the sp_StrArray CRuby's three-element result is, boxed when a user arm
     widened the dispatch's result to poly. */
  if (is_strpart && (ret == TY_POLY || ret == TY_STR_ARRAY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_PARTITION, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY)
      buf_printf(b, "sp_box_str_array(sp_str_%s(_t%d.v.s, %s))", name, tv, strarg);
    else
      buf_printf(b, "sp_str_%s(_t%d.v.s, %s)", name, tv, strarg);
    buf_puts(b, "; }\nelse ");
  }
  /* the multi-set forms on a TAG_STR receiver (#4195) */
  if (is_strsetop_n) {
    int is_cnt = sp_streq(name, "count");
    if ((is_cnt && (ret == TY_POLY || ret == TY_INT)) ||
        (!is_cnt && (ret == TY_POLY || ret == TY_STRING))) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_SETOP, -1, TY_UNKNOWN, PC_SAME);
      buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { ", tv);
      char sets[256]; int sl;
      sl = snprintf(sets, sizeof sets, "(const char *[]){");
      for (int a = 0; a < argc && sl < (int)sizeof sets - 16; a++) {
        int t = atmp[a];
        /* A later set read can allocate before the helper consumes this one. */
        if (atmp_ty[a] == TY_STRBUF) {
          t = ++g_tmp;
          buf_printf(b, "const char *_t%d = sp_strbuf_read_pub(_t%d); SP_GC_ROOT(_t%d); ", t, atmp[a], t);
        }
        sl += snprintf(sets + sl, sizeof sets - (size_t)sl, "%s_t%d", a ? ", " : "", t);
      }
      snprintf(sets + sl, sizeof sets - (size_t)sl, "}, %d", argc);
      char call[384];
      snprintf(call, sizeof call, "sp_str_%s_n(_t%d.v.s ? _t%d.v.s : \"\", %s)", name, tv, tv, sets);
      buf_printf(b, "_t%d = ", tr);
      if (ret == TY_POLY) buf_printf(b, "%s(%s)", is_cnt ? "sp_box_int" : "sp_box_str", call);
      /* a result slot that holds a nil another arm answers (an oint,
         plan_slot_oint) takes the count lifted (#8492) */
      else if (is_cnt && plan_slot_oint(c, T->id, ret)) buf_printf(b, "sp_oint_of(%s)", call);
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
    buf_printf(&eb, "sp_str_encode(_t%d.v.s, %d", tv, pos_argc);
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
        if (kn && sp_streq(kn, EKW[k])) found = e;   /* the last one binds */
      }
      if (found >= 0) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", kwtmp[found]);
        if (kwty[found] == TY_POLY) buf_puts(&eb, tn); else emit_boxed_text(c, kwty[found], tn, &eb);
      }
      else buf_puts(&eb, "sp_box_nil()");
    }
    buf_puts(&eb, ")");
    /* an option the runtime does not read changes the bytes (xml:, the
       newline ones): the String raises rather than answer without it */
    const char *unread = encode_opt_unread(nt, kwels, kwn);
    if (unread) {
      eb.len = 0;
      buf_printf(&eb, "(sp_raise_cls(\"NotImplementedError\", \"String#encode with the %s: option\"), (const char *)0)",
                 unread);
    }
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_POLY) buf_printf(b, "SP_STR_BOX(%s)", eb.p); else buf_puts(b, eb.p);
    buf_puts(b, "; }\nelse ");
    free(eb.p);
  }
  /* split(sep) on a TAG_STR receiver. A nil separator splits on
     whitespace, as CRuby's does. */
  if (is_strsplit && (ret == TY_STR_ARRAY || ret == TY_POLY_ARRAY || ret == TY_POLY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STR_SPLIT_N, -1, TY_UNKNOWN, PC_SAME);
    TyKind sat = comp_ntype(c, argv[0]);
    char call[192];
    if (sat == TY_NIL) snprintf(call, sizeof call, "sp_str_split_ws(_t%d.v.s)", tv);
    else if (sat == TY_REGEX) snprintf(call, sizeof call, "sp_re_split(_t%d, _t%d.v.s)", atmp[0], tv);
    else snprintf(call, sizeof call, "sp_str_split_drop_trailing(_t%d.v.s, %s)", tv, strarg);
    buf_printf(b, "if (_t%d.tag == SP_TAG_STR) { _t%d = ", tv, tr);
    if (ret == TY_STR_ARRAY) buf_puts(b, call);
    else if (ret == TY_POLY_ARRAY) buf_printf(b, "sp_StrArray_to_poly_fmt(%s)", call);
    else buf_printf(b, "sp_box_str_array(%s)", call);
    buf_puts(b, "; }\nelse ");
  }
  /* Integer#[N] bit-extraction: poly recv may hold a tagged int */
  if (is_index) {
    /* A boxed index need not be an Integer: a Range with a boxed end is
       boxed too (`a[cut..]`). The Integer, String and Array arms below
       read the index as an Integer and raised TypeError for it, so any
       other index on those receivers takes the generic lookup, as it does
       where no user class defines `[]`. */
    if (atmp_ty[0] == TY_POLY && ret == TY_POLY)
      buf_printf(b, "if (_t%d.tag != SP_TAG_INT && (_t%d.tag == SP_TAG_INT || _t%d.tag == SP_TAG_STR || "
                    "(_t%d.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_t%d.cls_id)))) "
                    "{ _t%d = sp_poly_index_poly(_t%d, _t%d); }\nelse ",
                 atmp[0], tv, tv, tv, tv, tr, tv, atmp[0]);
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
    if (cblk2 >= 0 || (nt_ref(nt, id, "block") >= 0 && g_yield_proc_ref)) {
      for (int k = 0; k < c->nclasses && blk_tmp2 < 0; k++) {
        /* a reopened builtin (Hash, Array, String...) is never `.new`ed, and its arm runs too */
        if (!c->classes[k].instantiated && !class_is_prim_reopen(c, k)) continue;
        int mi2 = call_target(c, id, k, name, 0, NULL);
        if (mi2 < 0) continue;
        Scope *cm2 = &c->scopes[mi2];
        if (!scope_has_callable_symbol(c, mi2) && !scope_needs_proc_form(c, mi2)) continue;
        if ((cm2->blk_param && cm2->blk_param[0] && !cm2->yields) ||
            scope_needs_proc_form(c, mi2)) {
          /* a forwarded &blk / anonymous & is a live proc, not a literal
             to lower -- the same guard the other dispatch arms carry */
          blk_tmp2 = hoist_dispatch_blk_proc(c, id, cblk2);
        }
      }
    } }
  /* a class-valued receiver dispatches class-side, ahead of the instance
     arms (#4218). */
  int cls_side = emit_poly_cls_value_prearm(c, id, name, pos_argc, atmp, atmp_ty, htmp, kw, tv,
                                            is_setter_val ? -1 : tr, ret, blk_tmp2, b);
  /* Where no class method of the program's reaches a class value, so no
     class-side switch answers it, a Class value answers as a typed one
     does: through the call's Class row (Module#include?), the receiver's
     temp the row's $T and each argument read from its temp. The bank
     defines the class tests the rows read. */
  if (!cls_side && g_needs_class_machinery && argc == pos_argc && !has_splat_arg && pos_argc <= 4 &&
      bop_find(TY_CLASS, name, pos_argc, nt_ref(nt, id, "block") >= 0)) {
    char crt[48];
    snprintf(crt, sizeof crt, "sp_unbox_class(_t%d)", tv);
    int slot[4];
    for (int i = 0; i < pos_argc; i++)
      slot[i] = view_bind(argv[i], atmp_ty[i] == TY_POLY ? "sp_unbox_class(_t%d)" : "_t%d", atmp[i]);
    Buf cb; memset(&cb, 0, sizeof cb);
    if (emit_builtin_op_arm_text(c, id, nt_ref(nt, id, "receiver"), TY_CLASS, name, crt, tv, &cb))
      buf_printf(b, "if (_t%d.tag == SP_TAG_CLASS) _t%d = %s%s%s;\nelse ", tv, tr,
                 ret == TY_POLY ? "sp_box_bool(" : "", cb.p ? cb.p : "0", ret == TY_POLY ? ")" : "");
    if (pos_argc > 0) view_unbind(slot[0]);
    free(cb.p);
  }
  /* a boxed Proc/Curry/Method in a slot a user `call`/`[]` shadows (#4395);
     the keyword split binds by name to a user candidate, so skip it. A
     splatted argument is one temp holding an array, not one temp per
     value, so the positional publish sequence below cannot spread it. */
  int tblk = (kw_pos || kwh >= 0) && !has_splat_arg ? callable_prearm_blk(c, id, name, blk_tmp2) : -1;
  if (kw_pos && !has_splat_arg) {
    if (kwall >= 0) {
      for (int e = 0; e < 2; e++) {
        char kg[48];
        snprintf(kg, sizeof kg, "_t%d->len %s 0 && ", kwall, e ? ">" : "==");
        called |= emit_poly_callable_prearm(c, name, pos_argc + e, atmp, atmp_ty, kg, tv, tr, ret,
                                            e ? 2 : pos_argc > 0 ? 1 : 0, tblk, b);
      }
    }
    else called = emit_poly_callable_prearm(c, name, argc, atmp, atmp_ty, NULL, tv, tr, ret,
                                            argc == 0 ? 0 : nt_argv_kwh(nt, argv, argc) >= 0 ? 2 : 1,
                                            tblk, b);
  }
  else if (kwh >= 0 && !has_splat_arg &&
           is_call_alias(name)) {
    /* The keyword split has no hash for a callable to take: the one built
       from the per-key temps ahead of the pre-arms (emit_poly_callable_kwh_decl,
       the dispatch declared it where a declaration can stand), passed as
       the trailing argument marked as keywords. */
    int kht = T->kwh_tmp;
    if (kht <= 0) {
      kht = ++g_tmp;
      buf_printf(b, "sp_SymPolyHash *_t%d = NULL; SP_GC_ROOT(_t%d); "
                    "if (_t%d.tag == SP_TAG_OBJ && (_t%d.cls_id == SP_BUILTIN_PROC"
                    " || _t%d.cls_id == SP_BUILTIN_CURRY || _t%d.cls_id == SP_BUILTIN_METHOD)) _t%d = ",
                 kht, kht, tv, tv, tv, tv, kht);
      emit_kwh_sym_hash(c, kw, NULL, b);
      buf_puts(b, "; ");
    }
    atmp[pos_argc] = kht;
    atmp_ty[pos_argc] = TY_SYM_POLY_HASH;
    called = emit_poly_callable_prearm(c, name, argc, atmp, atmp_ty, NULL, tv, tr, ret, 2, tblk, b);
  }
  else if (splat_last)
    called = emit_poly_callable_spread_prearm(c, name, splat_a, atmp, atmp_ty,
                                              stk >= 0 ? stk : atmp[splat_a], tv, tr, ret, b);
  else if (splat_a >= 0 && is_call_alias(name))
    unsupported(c, id, "a splat before other arguments into a method called on a value of more than one type");
  if (called && g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_CALLABLE, -1, TY_UNKNOWN, PC_SAME);
  return blk_tmp2;
}

/* The Hash a callable (a Proc, a Curry, a Method) takes for a keyword
   split, declared and filled ahead of every pre-arm: the class-value and
   the tag pre-arms end in `else `, where a declaration cannot stand, and
   its scope would end before the callable arm reads it (#8646). Filled
   only when the slot holds a callable, as before. Answers the temp, or 0
   when the dispatch has no such arm. */
int emit_poly_callable_kwh_decl(Compiler *c, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                                const PolyKw *kw, Buf *b) {
  if (ps->kwh < 0 || ps->has_splat_arg || ps->kw_pos || !is_call_alias(name)) return 0;
  int kht = ++g_tmp, tv = T->tv;
  buf_printf(b, "sp_SymPolyHash *_t%d = NULL; SP_GC_ROOT(_t%d); "
                "if (_t%d.tag == SP_TAG_OBJ && (_t%d.cls_id == SP_BUILTIN_PROC"
                " || _t%d.cls_id == SP_BUILTIN_CURRY || _t%d.cls_id == SP_BUILTIN_METHOD)) _t%d = ",
             kht, kht, tv, tv, tv, tv, kht);
  emit_kwh_sym_hash(c, kw, NULL, b);
  buf_puts(b, "; ");
  return kht;
}

/* A builtin arm's argument a, boxed, into ab: its temp, or for the keyword
   hash (which has no temp; the keywords were split off) a Hash built from
   them, as CRuby hands a method without keywords one. That Hash is held
   in a rooted temp opened in b: 1 when the caller must close it with `}`. */
static int poly_case_arg_boxed(Compiler *c, const PolyTemps *T, const PolyKw *kw, int a, Buf *ab, Buf *b) {
  if (kw && kw->kwh >= 0 && a >= T->pos_argc) {
    int th = ++g_tmp;
    buf_printf(b, " { sp_RbVal _t%d = ", th);
    emit_kwh_pos_hash(c, kw, 1, b);
    buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", th);
    buf_printf(ab, "_t%d", th);
    return 1;
  }
  char tn[32]; snprintf(tn, sizeof tn, "_t%d", T->atmp[a]);
  if (T->atmp_ty[a] == TY_POLY) buf_puts(ab, tn);
  else emit_boxed_text(c, T->atmp_ty[a], tn, ab);
  return 0;
}

/* The builtin `case` arms a poly dispatch with arguments writes after its
   class arms: an Array's index, push, unshift, pack, join, include?,
   index and intersect?, an IO's write, read and the rest, a Time's
   strftime, a Hash's `[]` and fetch by key kind, a predicate. */
void emit_poly_cases_n(Compiler *c, int id, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                       const PolyKw *kw, int splat_a, int is_aref, int is_fetch, Buf *b) {
  const NodeTable *nt = c->nt;
  int argc = T->argc, pos_argc = T->pos_argc, kwh = ps->kwh;
  const int *argv = T->argv, *atmp = T->atmp;
  const TyKind *atmp_ty = T->atmp_ty;
  TyKind ret = T->ret;
  int tv = T->tv, tr = T->tr;
  const char *idxref = T->idxref;
  char strarg[64];
  snprintf(strarg, sizeof strarg, argc > 0 && atmp_ty[0] == TY_STRBUF
           ? "sp_strbuf_read_pub(_t%d)"
           : argc > 0 && atmp_ty[0] == TY_STRING ? "SP_STR_B(_t%d)" : "_t%d",   /* (one representation: a handle held) */
           argc > 0 ? atmp[0] : 0);
  int is_index = ps->index, is_unshift = ps->unshift, is_push = ps->push, is_ppack = ps->ppack;
  int is_pjoin = ps->pjoin, is_include = ps->include, is_arr_index = ps->arr_index;
  int is_intersect = ps->intersect, is_strftime = ps->strftime, is_pred = ps->pred;
  if (is_index) {
    emit_poly_index_cases(ret, tr, tv, idxref, plan_slot_oint(c, id, ret), b);
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INDEX_CASES, -1, TY_UNKNOWN, PC_SAME);
  }
  if (sp_streq(name, "timeout=") && argc == 1 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_TIMEOUT, -1, TY_UNKNOWN, PC_SAME);
    int tf = ++g_tmp;
    buf_printf(b, " case SP_BUILTIN_IO: { sp_File *_t%d = (sp_File *)_t%d.v.p; ", tf, tv);
    emit_stat_handle_nomethod(tf, tv, name, b);
    Buf arg = {0};
    int held = poly_case_arg_boxed(c, T, kw, 0, &arg, b);
    buf_printf(b, "sp_File_set_timeout(_t%d, %s); ", tf, arg.p ? arg.p : "sp_box_nil()");
    if (held) buf_puts(b, " }");
    free(arg.p);
    if (!call_is_setter_assign(nt, id)) {
      if (ret == TY_POLY) buf_printf(b, "_t%d = _t%d; ", tr, tv);
      else if (ret == TY_IO) buf_printf(b, "_t%d = _t%d; ", tr, tf);
    }
    buf_puts(b, "break; }");
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
      no_exc7 = e7 >= 0 && nt_kind(nt, e7) == NK_FalseNode;
    }
    int trd7 = ++g_tmp, te7 = ++g_tmp;
    buf_printf(b, " case SP_BUILTIN_IO: { sp_bool _e%d; const char *_t%d = sp_sock_read_nb("
                  "(sp_File *)_t%d.v.p, ", te7, trd7, tv);
    if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_poly_arg_int_chk(_t%d)", atmp[0]);
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
  /* readpartial / sysread(len) on the builtin IO tag, the same shape: a
     TCPSocket beside an SSLSocket (an openssl package class) reached the
     class-id switch, which had an arm only for the SSLSocket (#7315) */
  if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc == 1 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_READPARTIAL, -1, TY_UNKNOWN, PC_SAME);
    int trp = ++g_tmp;
    buf_printf(b, " case SP_BUILTIN_IO: { const char *_t%d = %s((sp_File *)_t%d.v.p, ", trp, io_readpartial_entry(name), tv);
    if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_poly_arg_int_chk(_t%d)", atmp[0]);
    else buf_printf(b, "(sp_int)_t%d", atmp[0]);
    buf_printf(b, "); _t%d = ", tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(_t%d)", trp);
    else buf_printf(b, "_t%d", trp);
    buf_puts(b, "; break; }");
  }
  /* readpartial / sysread(len, buf) on the builtin IO tag: the bytes read go
     into the caller's buffer as the typed arm puts them (a String handle has
     its contents replaced, a plain local is rebound), and are the answer */
  if ((sp_streq(name, "readpartial") || sp_streq(name, "sysread")) && argc == 2 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_READPARTIAL_BUF, -1, TY_UNKNOWN, PC_SAME);
    int trp = ++g_tmp;
    Buf hr; memset(&hr, 0, sizeof hr);
    /* a String handle slot is checked for a freeze before the read, which
       then goes straight into it (sp_File_readpartial_into): the bytes were
       read into a new String and the freeze was found only on the copy,
       with the bytes gone from the stream */
    if (strbuf_slot_text(c, argv[1], &hr)) {
      int tn = ++g_tmp, th = ++g_tmp;
      buf_printf(b, " case SP_BUILTIN_IO: { sp_int _t%d = ", tn);
      if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_poly_arg_int_chk(_t%d)", atmp[0]);
      else buf_printf(b, "(sp_int)_t%d", atmp[0]);
      buf_puts(b, "; ");
      if (sp_streq(name, "readpartial"))
        buf_printf(b, "if (_t%d < 0) sp_raise_cls(\"ArgumentError\", sp_sprintf(\"negative length %%lld given\", (long long)_t%d)); ",
                   tn, tn);
      buf_printf(b, "sp_String *_t%d = %s; SP_GC_ROOT(_t%d); ", th, hr.p, th);
      buf_printf(b, "if (_t%d) { if (sp_String_is_frozen(_t%d)) sp_raise_frozen_str(_t%d->data); "
                    "sp_str_check_mutable(_t%d->data); } ", th, th, th, th);
      buf_printf(b, "const char *_t%d = _t%d ? sp_File_readpartial_into((sp_File *)_t%d.v.p, _t%d, _t%d)"
                    " : sp_File_readpartial_or_nil((sp_File *)_t%d.v.p, _t%d); ", trp, th, tv, tn, th, tv, tn);
      buf_printf(b, "if (!_t%d) sp_raise_cls(\"EOFError\", \"end of file reached\"); ", trp);
    }
    else {
      /* (readpartial checks its length as the one-argument form does;
         sysread with a buffer is CRuby's Errno::EFAULT, which this is not) */
      buf_printf(b, " case SP_BUILTIN_IO: { const char *_t%d = %s((sp_File *)_t%d.v.p, ", trp,
                 sp_streq(name, "readpartial") ? "sp_File_readpartial_checked" : "sp_File_readpartial", tv);
      if (atmp_ty[0] == TY_POLY) buf_printf(b, "sp_poly_arg_int_chk(_t%d)", atmp[0]);
      else buf_printf(b, "(sp_int)_t%d", atmp[0]);
      buf_puts(b, "); ");
      if (nt_kind(nt, argv[1]) == NK_LocalVariableReadNode)
        buf_printf(b, "lv_%s = _t%d; ", rename_local(nt_str(nt, argv[1], "name")), trp);
    }
    free(hr.p);
    buf_printf(b, "_t%d = ", tr);
    if (ret == TY_POLY) buf_printf(b, "sp_box_str(_t%d)", trp);
    else buf_printf(b, "_t%d", trp);
    buf_puts(b, "; break; }");
  }
  /* setsockopt(level, opt, value) on the builtin IO tag, beside a class that
     owns the name (SSLSocket): the Socket raised NoMethodError */
  if (sp_streq(name, "setsockopt") && argc == 3 && kwh < 0 && splat_a < 0) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_SETSOCKOPT, -1, TY_UNKNOWN, PC_SAME);
    buf_printf(b, " case SP_BUILTIN_IO: { sp_sock_setsockopt((sp_File *)_t%d.v.p", tv);
    for (int k = 0; k < 3; k++) {
      if (atmp_ty[k] == TY_POLY) buf_printf(b, ", sp_poly_arg_i(_t%d)", atmp[k]);
      else if (atmp_ty[k] == TY_BOOL) buf_printf(b, ", (sp_int)(_t%d ? 1 : 0)", atmp[k]);
      else buf_printf(b, ", (sp_int)_t%d", atmp[k]);
    }
    buf_printf(b, "); _t%d = ", tr);
    if (ret == TY_POLY) buf_puts(b, "sp_box_int(0)");
    else if (ret == TY_INT) buf_puts(b, "0");
    else buf_printf(b, "_t%d", tr);
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
      /* an oint result slot (plan_slot_oint) takes the count as never nil */
      else if (plan_slot_oint(c, id, ret))
        buf_printf(b, " case SP_BUILTIN_IO: _t%d = %s(sp_File_write_bin("
                       "(sp_File *)_t%d.v.p, _t%d)); break;",
                   tr, oint_of(ret), tv, atmp[0]);
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
      /* a shared String's box (SP_BUILTIN_STRBUF) is its bytes with their
         length: read before the tag test, or the String arm below sized
         it by strlen and a frame stopped at its first NUL (#9602) */
      buf_printf(b, "_t%d = sp_poly_strbuf_deref(_t%d); ", wrr, wrr);
      buf_printf(b, "sp_RbVal _t%d = (_t%d.tag == SP_TAG_STR) ? "
                     "sp_box_int(sp_File_write_bin((sp_File *)_t%d.v.p, _t%d.v.s)) : "
                     "sp_box_int(sp_File_write((sp_File *)_t%d.v.p, sp_poly_to_s(_t%d))); ",
                   wrv, wrr, tv, wrr, tv, wrr);
      if (ret == TY_POLY) buf_printf(b, "_t%d = _t%d; ", tr, wrv);
      else if (plan_slot_oint(c, id, ret)) buf_printf(b, "_t%d = %s(_t%d); ", tr, oint_unbox(ret), wrv);
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
      buf_printf(b, "_t%d = sp_poly_strbuf_deref(_t%d); SP_GC_ROOT_RBVAL(_t%d); ", wrr, wrr, wrr);   /* (a shared String's box: its bytes, #9602) */
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
      if (sp_streq(name, "seek") && argc == 1 && an_program_makes_dir(c)) {
        buf_puts(b, " case SP_BUILTIN_DIR: ");
        if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_obj(", tr);
        buf_printf(b, "sp_Dir_seek((sp_Dir *)_t%d.v.p, %s)", tv, iv[0]);
        if (ret == TY_POLY) buf_puts(b, ", SP_BUILTIN_DIR)");
        buf_puts(b, "; break;");
      }
    }
  }
  /* read(*args) on a poly value (WEBrick's `io.__send__(method, *arg)`):
     the File arm reads the gathered list the dispatch holds in its temp, a
     length, nil or none, as read(n) and read() do. Left out, a File beside a
     class of the program's own with `read` raised NoMethodError for the
     File. The outbuf form is no arm of this one. */
  if (sp_streq(name, "read") && argc == 1 && splat_a == 0 && kwh < 0 && (ret == TY_POLY || ret == TY_STRING) &&
      atmp_ty[0] == TY_POLY_ARRAY) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_IO_SEEK_READ, -1, TY_UNKNOWN, PC_SAME);
    int ta = atmp[0];
    buf_printf(b, " case SP_BUILTIN_IO: { sp_File *_f%d = (sp_File *)_t%d.v.p; const char *_s%d; "
                  "if (_t%d->len > 1) sp_raise_cls(\"NotImplementedError\", \"IO#read with a buffer, through a splat\"); "
                  "if (_t%d->len == 0 || _t%d->data[0].tag == SP_TAG_NIL) _s%d = sp_File_read(_f%d); "
                  "else _s%d = sp_File_read_n(_f%d, sp_poly_arg_i(_t%d->data[0])); ",
               tr, tv, tr, ta, ta, ta, tr, tr, tr, tr, ta);
    if (ret == TY_POLY) buf_printf(b, "_t%d = sp_box_nullable_str(_s%d); } break;", tr, tr);
    else buf_printf(b, "_t%d = _s%d; } break;", tr, tr);
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
      Buf ab; memset(&ab, 0, sizeof ab);
      int kb = poly_case_arg_boxed(c, T, kw, a, &ab, b);
      buf_printf(b, " sp_poly_insert(_t%d, %d, %s);", tv, a, ab.p ? ab.p : "sp_box_nil()");
      if (kb) buf_puts(b, " }");
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
      Buf ab; memset(&ab, 0, sizeof ab);
      int kb = poly_case_arg_boxed(c, T, kw, a, &ab, b);
      buf_printf(b, " sp_poly_shl(_t%d, %s);", tv, ab.p ? ab.p : "sp_box_nil()");
      if (kb) buf_puts(b, " }");
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
        if (kwh >= 0 && a >= pos_argc) emit_kwh_pos_hash(c, kw, 1, b);
        else if (atmp_ty[a] == TY_POLY) buf_puts(b, tn);
        else emit_boxed_text(c, atmp_ty[a], tn, b);
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
      Buf ab; memset(&ab, 0, sizeof ab);
      int kb = poly_case_arg_boxed(c, T, kw, a, &ab, b);
      buf_printf(b, " sp_poly_shl(_t%d, %s);", tv, ab.p ? ab.p : "sp_box_nil()");
      if (kb) buf_puts(b, " }");
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
      else if (atmp_ty[0] == TY_STRBUF) {
        int t = ++g_tmp;
        /* Packing can convert elements before it finishes reading the format. */
        buf_printf(&pb2, "({ const char *_t%d = %s; SP_GC_ROOT(_t%d); sp_poly_pack(_t%d, _t%d); })", t, strarg, t, tv, t);
      }
      else
        buf_printf(&pb2, "sp_poly_pack(_t%d, %s)", tv, strarg);
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
    else if (argc >= 1) buf_puts(&jb, strarg);
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
      buf_printf(b, " default: { sp_poly_enum_name_chk(_t%d, \"%s\");"
                    " int _ui%d = sp_poly_user_include(_t%d, %s);"
                    " _t%d = %s_ui%d > 0%s; break; }",
                 tv, name, tv, tv, ab5.p ? ab5.p : "sp_box_nil()", tr, ibo, tv, ibc);
      free(ab5.p); }
    /* an Integer or a Float that can be nil is held boxed (the dispatch's
       argument temps): the arms take it as promote's boxed argument */
    TyKind at = comp_ntype(c, argv[0]);
    if (oint_kind(at) && atmp_ty[0] == TY_POLY) at = TY_POLY;
    switch (at) {
    case TY_INT:
      buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_IntArray_include((sp_IntArray *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      /* an Integer is == the Float of its value (`[2.0].include?(2)`);
         key? is no Array method */
      if (is_membership_alias(name))
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_poly_arr_include_eq(_t%d, sp_box_int(_t%d))%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_include((sp_Range *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      buf_printf(b, " case SP_BUILTIN_FLOAT_RANGE: _t%d = %ssp_frange_cover_i(*(sp_FloatRange *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      break;
    case TY_FLOAT:
      buf_printf(b, " case SP_BUILTIN_RANGE: _t%d = %ssp_range_cover_f((sp_Range *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
      /* ...and a Float the Integer of its value (`[2].include?(2.0)`) */
      if (is_membership_alias(name))
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_poly_arr_include_eq(_t%d, sp_box_float(_t%d))%s; break;", tr, ibo, tv, atmp[0], ibc);
      if (is_membership_alias(name))
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_include((sp_FloatArray *)_t%d.v.p, _t%d)%s; break;", tr, ibo, tv, atmp[0], ibc);
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
      /* ...and is == an Integer or a Float element of its value (`[2].include?(2r)`) */
      if (is_membership_alias(name))
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_poly_arr_include_eq(_t%d, %s)%s; break;", tr, ibo, tv, ab6.p ? ab6.p : "sp_box_nil()", ibc);
      free(ab6.p);
      break;
    }
    case TY_STRING:
      buf_printf(b, " case SP_BUILTIN_STR_ARRAY: _t%d = %ssp_StrArray_include((sp_StrArray *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, strarg, ibc);
      buf_printf(b, " case SP_BUILTIN_STR_INT_HASH: _t%d = %ssp_StrIntHash_has_key((sp_StrIntHash *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, strarg, ibc);
      buf_printf(b, " case SP_BUILTIN_STR_STR_HASH: _t%d = %ssp_StrStrHash_has_key((sp_StrStrHash *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, strarg, ibc);
      buf_printf(b, " case SP_BUILTIN_STR_POLY_HASH: _t%d = %ssp_StrPolyHash_has_key((sp_StrPolyHash *)_t%d.v.p, %s)%s; break;", tr, ibo, tv, strarg, ibc);
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
      /* a boxed nil finds the typed array's nil elements */
      /* a boxed value of another kind (a Float against Integers, an
         Integer, Rational or Bignum against Floats) compares as == does
         (include? and member?: key? is no Array method) */
      { char other[96];
        if (is_membership_alias(name)) snprintf(other, sizeof other, "sp_poly_arr_include_eq(_t%d, _t%d)", tv, atmp[0]);
        else snprintf(other, sizeof other, "0");
        buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %s((_t%d.tag == SP_TAG_INT || _t%d.tag == SP_TAG_NIL) ?"
                      " sp_IntArray_include_o((sp_IntArray *)_t%d.v.p, sp_poly_as_int_or_nil(_t%d)) : %s)%s; break;",
                   tr, ibo, atmp[0], atmp[0], tv, atmp[0], other, ibc);
        buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %s((_t%d.tag == SP_TAG_FLT || _t%d.tag == SP_TAG_NIL) ?"
                      " sp_FloatArray_include_o((sp_FloatArray *)_t%d.v.p, sp_poly_as_float_or_nil(_t%d)) : %s)%s; break;",
                   tr, ibo, atmp[0], atmp[0], tv, atmp[0], other, ibc); }
      /* a String handle's box (a String some name changes in place) is
         a String too: its bytes are what the elements are compared with */
      { int ts = ++g_tmp;
        buf_printf(b, " case SP_BUILTIN_STR_ARRAY: { sp_RbVal _t%d = sp_poly_strbuf_deref(_t%d);"
                      " _t%d = %s_t%d.tag == SP_TAG_STR && sp_StrArray_include((sp_StrArray *)_t%d.v.p, _t%d.v.s)%s; break; }",
                   ts, atmp[0], tr, ibo, ts, tv, ts, ibc); }
      break;
    case TY_NIL:
      /* is there a nil element */
      buf_printf(b, " case SP_BUILTIN_INT_ARRAY: _t%d = %ssp_IntArray_has_nil((sp_IntArray *)_t%d.v.p)%s; break;", tr, ibo, tv, ibc);
      buf_printf(b, " case SP_BUILTIN_FLT_ARRAY: _t%d = %ssp_FloatArray_has_nil((sp_FloatArray *)_t%d.v.p)%s; break;", tr, ibo, tv, ibc);
      break;
    default: break;
    }
    /* A String Range: a String argument is a member by succession, anything
       else is not one, and an open Range raises CRuby's TypeError either way
       (sp_srange_include_poly). It fell to the default arm's false. key? and
       has_key? are no Range methods. */
    if (is_membership_alias(name)) {
      Buf ab7; memset(&ab7, 0, sizeof ab7);
      char tn7[24]; snprintf(tn7, sizeof tn7, "_t%d", atmp[0]);
      if (atmp_ty[0] == TY_POLY) buf_puts(&ab7, tn7);
      else emit_boxed_text(c, atmp_ty[0], tn7, &ab7);
      buf_printf(b, " case SP_BUILTIN_STR_RANGE: _t%d = %ssp_srange_include_poly(SP_SRANGE_OF(_t%d.v.p), %s)%s; break;",
                 tr, ibo, tv, ab7.p ? ab7.p : "sp_box_nil()", ibc);
      free(ab7.p); }
    else buf_printf(b, " case SP_BUILTIN_STR_RANGE: sp_raise_poly_nomethod(\"%s\", _t%d); break;", name, tv);
    /* PolyArray: box the arg for runtime comparison */
    {
      int tbox = ++g_tmp;
      buf_printf(b, " case SP_BUILTIN_POLY_ARRAY: case SP_BUILTIN_PTR_ARRAY: { sp_RbVal _t%d = ", tbox);
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
      emit_boxed_text(c, atmp_ty[0] == TY_STRBUF ? TY_STRBUF : at, tn, b);
      /* a pointer array compares through its boxed elements (#4486) */
      buf_printf(b, "; _t%d = %ssp_poly_arr_include_eq(_t%d, _t%d)%s; break; }", tr, ibo, tv, tbox, ibc);
    }
    /* Every hash kind the arms above did not claim: the key boxed and
       looked up by the storage's key kind (a key of another kind is
       simply absent). Only the general hash had an arm, so a
       Symbol-keyed hash read out of a nested literal answered
       `key?(k)` false for a boxed k, whatever it held. */
    {
      int tbox = ++g_tmp;
      buf_puts(b, " case SP_BUILTIN_POLY_POLY_HASH:");
      if (sp_streq(name, "key?")) buf_puts(b, " case SP_BUILTIN_THREAD:");
      if (at != TY_STRING)
        buf_puts(b, " case SP_BUILTIN_STR_INT_HASH: case SP_BUILTIN_STR_STR_HASH: case SP_BUILTIN_STR_POLY_HASH:");
      if (at != TY_SYMBOL) buf_puts(b, " case SP_BUILTIN_SYM_POLY_HASH:");
      buf_puts(b, " case SP_BUILTIN_INT_INT_HASH: case SP_BUILTIN_INT_STR_HASH:");
      buf_printf(b, " { sp_RbVal _t%d = ", tbox);
      char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
      emit_boxed_text(c, atmp_ty[0] == TY_STRBUF ? TY_STRBUF : at, tn, b);
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
    buf_printf(b, " sp_oint _t%d = sp_poly_arr_index_val(_t%d, %s, %d); _t%d = ",
               tix, tv, ab4.p ? ab4.p : "sp_box_nil()",
               sp_streq(name, "rindex") ? 1 : 0, tr);
    if (ret == TY_INT) buf_printf(b, "%s_t%d%s", node_is_oint(c, id) ? "" : "sp_oint_arg(", tix, node_is_oint(c, id) ? "" : ")");
    else buf_printf(b, "sp_box_oint(_t%d)", tix);
    buf_puts(b, "; break; }");
    /* an Enumerator's find_index walks it only as far as the hit */
    if (sp_streq(name, "find_index")) {
      int tix2 = ++g_tmp;
      buf_printf(b, " case SP_BUILTIN_ENUMERATOR: { sp_oint _t%d = sp_enum_find_index_val(_t%d, %s); _t%d = ",
                 tix2, tv, ab4.p ? ab4.p : "sp_box_nil()", tr);
      if (ret == TY_INT) buf_printf(b, "%s_t%d%s", node_is_oint(c, id) ? "" : "sp_oint_arg(", tix2, node_is_oint(c, id) ? "" : ")");
      else buf_printf(b, "sp_box_oint(_t%d)", tix2);
      buf_puts(b, "; break; }");
    }
    free(ab4.p);
  }
  if (is_intersect) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_INTERSECT, -1, TY_UNKNOWN, PC_SAME);
    Repr ar2 = repr_of(c, argv[0]);
    TyKind at2 = ar2.as_ty;
    char abox[96];
    if (ar2.kind == RK_BOXED) snprintf(abox, sizeof abox, "_t%d", atmp[0]);
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
    buf_printf(b, "sp_poly_intersect_p_eql(_t%d, %s)", tv, abox);
    if (ret == TY_POLY) buf_puts(b, ")");
    buf_puts(b, "; break;");
  }
  /* strftime on a poly value that is really a Time: format it; nil or any
     other runtime class raises NoMethodError as CRuby does. */
  /* beside user arms the call's type is theirs: the Time arm joins only
     where its String fits (a user strftime answering something else keeps
     the switch it had) */
  if (is_strftime && (ps->ncand == 0 || ret == TY_STRING || ret == TY_POLY)) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_STRFTIME, -1, TY_UNKNOWN, PC_SAME);
    char fa[64];
    if (repr_of(c, argv[0]).kind == RK_BOXED) snprintf(fa, sizeof fa, "sp_poly_arg_str_chk(_t%d)", atmp[0]);
    else snprintf(fa, sizeof fa, "%s", strarg);
    if (ret == TY_POLY)
      buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_box_str(sp_time_strftime((sp_Time *)_t%d.v.p, %s)); break;", tr, tv, fa);
    else
      buf_printf(b, " case SP_BUILTIN_TIME: _t%d = sp_time_strftime((sp_Time *)_t%d.v.p, %s); break;", tr, tv, fa);
    /* with user arms in the switch, the default is theirs to emit */
    if (ps->ncand == 0)
      buf_printf(b, " default: sp_raise_cls(\"NoMethodError\", sp_nomethod_msg(\"strftime\", _t%d)); break;", tv);
  }
  /* the poly value may actually be a string-keyed hash: dispatch `[]` /
     `fetch` to the matching hash storage, boxing the value into the poly
     result. */
  if ((is_aref || is_fetch) && comp_ntype(c, argv[0]) == TY_STRING) {
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
      snprintf(getx, sizeof getx, "sp_%sHash_get((sp_%sHash *)_t%d.v.p, %s)", HV[hvi].hn, HV[hvi].hn, tv, strarg);
      /* `[]` answers the hash's default on a miss, which the storage's
         own read already knows: the has_key gate below is fetch's, and
         in front of `[]` it answered nil for a Hash.new("") (#5544) */
      if (is_aref) {
        char gx[96]; snprintf(gx, sizeof gx, "sp_poly_get_str(_t%d, %s)", tv, strarg);
        buf_printf(b, " case %s: _t%d = ", HV[hvi].cls, tr);
        if (ret == TY_POLY) buf_puts(b, gx); else emit_unbox_text(c, trt, gx, b);
        buf_puts(b, "; break;");
        continue;
      }
      buf_printf(b, " case %s: _t%d = sp_%sHash_has_key((sp_%sHash *)_t%d.v.p, %s) ? ",
                 HV[hvi].cls, tr, HV[hvi].hn, HV[hvi].hn, tv, strarg);
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
      snprintf(getx, sizeof getx, "sp_PolyPolyHash_get((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(%s))", tv, strarg);
      snprintf(hx, sizeof hx, "sp_PolyPolyHash_has_key((sp_PolyPolyHash *)_t%d.v.p, sp_box_str(%s))", tv, strarg);
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
  if ((is_aref || is_fetch) && comp_ntype(c, argv[0]) == TY_SYMBOL) {
    if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_AREF_SYM, -1, TY_UNKNOWN, PC_SAME);
    TyKind trt = is_scalar_ret(ret) ? ret : TY_INT;
    char getx[200];
    snprintf(getx, sizeof getx, "sp_SymPolyHash_get((sp_SymPolyHash *)_t%d.v.p, _t%d)", tv, atmp[0]);
    /* `[]` answers the hash's default on a miss, which the storage's own
       read already knows: the has_key gate is fetch's, as for the String
       keys above (#5544), and in front of `[]` it answered nil for a
       Hash.new("") */
    buf_printf(b, " case SP_BUILTIN_SYM_POLY_HASH: _t%d = ", tr);
    if (is_fetch) buf_printf(b, "sp_SymPolyHash_has_key((sp_SymPolyHash *)_t%d.v.p, _t%d) ? ", tv, atmp[0]);
    if (ret == TY_POLY) buf_puts(b, getx);
    else if (trt == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", getx);
    else if (trt == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", getx);
    else if (trt == TY_INT) buf_printf(b, "sp_poly_to_i(%s)", getx);
    else buf_printf(b, "sp_poly_to_i(%s)", getx);
    if (is_fetch) {
      buf_puts(b, " : ");
      emit_poly_fetch_absent(c, argc, atmp, argc == 2 ? atmp_ty[1] : TY_UNKNOWN, argv[0], ret, trt, b);
    }
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
      (repr_of(c, argv[0]).kind == RK_BOXED || comp_ntype(c, argv[0]) == TY_UNKNOWN)) {
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
  {"pow",       1, 2, "sp_poly_int_pow",     "sp_poly_int_powmod_recv", NPA_BOXED,     0, -1, TY_UNKNOWN},
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

/* The generic `default:` tail the plan gives a poly dispatch with
   arguments (cpoly_obj_default_n): the Object reopening's arity refusal,
   or its visibility's NoMethodError for the receiver _t<tv>. 1 when
   written; the builtin surface's trial is the tail otherwise. */
static int emit_poly_plan_default_n(Compiler *c, int id, const char *name, int kwh, int pos_argc, int splat_a,
                                    int tv, Buf *b) {
  const PolyPlan *p = cplan_poly(c, id);
  for (int i = 0; i < p->n; i++) {
    const PolyArm *a = &p->arm[i];
    if (a->key != PA_KEY_DEFAULT || (a->kind != PA_ARITY && a->kind != PA_VIS)) continue;
    if (a->kind == PA_VIS) {
      buf_puts(b, " ");
      emit_vis_raise_boxed(a->vis, name, tv, b);
      buf_puts(b, "; break;");
    }
    else {
      char exp[600];
      poly_arm_count(c, &c->scopes[a->mi], kwh, pos_argc, splat_a, exp, sizeof exp);
      buf_puts(b, " "); emit_poly_arity_raise(b, exp); buf_puts(b, " break;");
    }
    if (g_plan_check) pa_observe(a->kind, PA_KEY_DEFAULT, a->mi, TY_UNKNOWN, PC_VOID);
    return 1;
  }
  return 0;
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
  int pa_frame = g_plan_check ? pa_current() : -1;
  int kwmark = g_n_argov;
  if (bop_share_named(BOP_ANY_ARRAY, name) == BSH_PACK) {
    for (int e = 0; e < kw->kwn; e++) {
      int v = nt_ref(nt, kw->kwels[e], "value");
      if (v >= 0) view_bind(v, "_t%d", kw->kwtmp[e]);
    }
  }
  /* Same fallthrough rule as the zero-arg dispatch, but only when the
     switch is made of user-class arms alone. Where a builtin pre-arm is in
     play the fallthrough can mean "right receiver, wrong argument" --
     `"abc".include?(:x)` is a TypeError in CRuby, not a NoMethodError --
     so those names keep their existing answer rather than gain a
     mislabelled raise (#3394). */
  if (!is_pred && !(is_strftime && ps->ncand == 0) && !is_aref && !is_aref2 && !is_fetch && !is_include &&
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
    /* blockless upto / downto: a class defining the name took the
       dispatch, and the Integer (a promote-mode sum is boxed) or String in
       the same slot fell to the raise below (tzinfo's `(year + 1).upto(n)`
       beside the date package's Date#upto) */
    else if ((sp_streq(name, "upto") || sp_streq(name, "downto")) && argc == 1 && pos_argc == 1 &&
             splat_a < 0 && blk_tmp2 < 0 && nt_ref(nt, id, "block") < 0 && ret == TY_POLY) {
      Buf ub; memset(&ub, 0, sizeof ub);
      { char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[0]);
        if (atmp_ty[0] == TY_POLY) buf_puts(&ub, tn);
        else emit_boxed_text(c, atmp_ty[0], tn, &ub); }
      buf_printf(b, " _t%d = sp_poly_upto_enum(_t%d, %s, %d); break;",
                 tr, tv, ub.p ? ub.p : "sp_box_nil()", sp_streq(name, "upto"));
      free(ub.p);
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_UPTO, -1, TY_UNKNOWN, PC_SAME);
    }
    /* round(n) / ceil(n) / floor(n) / truncate(n): a class defining
       `round(digits)` took over the name, and the Integer or Float in
       the same slot fell to the raise below (#4532). The zero-arg forms
       have their arm in the other dispatch; these answer through the
       boxed helpers the no-user-class path uses. */
    else if (is_round_family(name) && argc == 1 && splat_a < 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_ROUND, -1, TY_UNKNOWN, PC_SAME);
      char nd9[64];
      /* a Float count is range-checked where the receiver is a number: the
         _fdigits helpers, whose call is as long as the cast it replaces */
      int fd9 = atmp_ty[0] == TY_FLOAT;
      if (atmp_ty[0] == TY_POLY) snprintf(nd9, sizeof nd9, "sp_poly_arg_int_chk(_t%d)", atmp[0]);
      else if (fd9) snprintf(nd9, sizeof nd9, "_t%d", atmp[0]);
      else snprintf(nd9, sizeof nd9, "(sp_int)_t%d", atmp[0]);
      Buf nv9; memset(&nv9, 0, sizeof nv9);
      if (sp_streq(name, "round")) buf_printf(&nv9, fd9 ? "sp_poly_round_n_fdigits(_t%d, %s)" : "sp_poly_round_n(_t%d, %s)", tv, nd9);
      else buf_printf(&nv9, fd9 ? "sp_poly_prec_n_fdigits(_t%d, %s, %s)" : "sp_poly_prec_n(_t%d, %s, %s)", tv, nd9,
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
       no-user-class path calls, and fit the answer to the slot. The count
       is the positional one: a keyword hash is no hoisted argument, and a
       call of a name here with keywords alone (`xml.div(xmlns: ...) { }`
       on a boxed builder) read the temp of a positional it never had. */
    else if (splat_a < 0 && poly_num_arm(name, pos_argc) >= 0) {
      if (g_plan_check) pa_observe(PA_BUILTIN, PA_KEY_BUILTIN + PB_ND_NUM, -1, TY_UNKNOWN, PC_SAME);
      int ai = poly_num_arm(name, pos_argc);
      Buf ab9; memset(&ab9, 0, sizeof ab9);
      { char an9[32]; snprintf(an9, sizeof an9, "_t%d", atmp[0]);
        if (SP_NUM_ARM[ai].arg_int) {
          if (atmp_ty[0] == TY_POLY) buf_printf(&ab9, "sp_poly_arg_int_chk_w(_t%d, 1)", atmp[0]);
          else buf_printf(&ab9, "(sp_int)_t%d", atmp[0]);
        }
        else if (atmp_ty[0] == TY_POLY) buf_puts(&ab9, an9);
        else emit_boxed_text(c, atmp_ty[0], an9, &ab9); }
      const char *a9 = ab9.p ? ab9.p : "sp_box_nil()";
      Buf bb9; memset(&bb9, 0, sizeof bb9);
      const char *fn9 = SP_NUM_ARM[ai].fn;
      if (pos_argc > SP_NUM_ARM[ai].min_argc && SP_NUM_ARM[ai].fn2) {
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
        char sx[160];
        if (atmp_ty[1] == TY_INT || atmp_ty[1] == TY_FLOAT) snprintf(sx, sizeof sx, "(sp_int)_t%d", atmp[1]);
        else if (atmp_ty[1] == TY_POLY) snprintf(sx, sizeof sx, "sp_poly_arg_int_chk(_t%d)", atmp[1]);
        /* a keyword hash or any other object is no offset: TypeError, as in CRuby */
        else { Buf ob; memset(&ob, 0, sizeof ob); char on[32]; snprintf(on, sizeof on, "_t%d", atmp[1]);
          emit_boxed_text(c, atmp_ty[1], on, &ob);
          snprintf(sx, sizeof sx, "sp_poly_arg_int_chk(%s)", ob.p ? ob.p : "sp_box_nil()"); free(ob.p); }
        buf_printf(b, " if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) {"
                      " sp_oint _t%d = sp_poly_str_index_from_val(_t%d, %s, %s, %d); _t%d = ",
                   tv, tv, tsi, tv, ab5.p ? ab5.p : "sp_box_nil()", sx,
                   sp_streq(name, "rindex") ? 1 : 0, tr);
        /* the result slot carries whatever the call was inferred as: a
           nullable Integer rides as its oint, a poly slot boxes */
        if (ret == TY_POLY) buf_printf(b, "sp_box_oint(_t%d)", tsi);
        else buf_printf(b, "%s_t%d%s", node_is_oint(c, id) ? "" : "sp_oint_arg(", tsi, node_is_oint(c, id) ? "" : ")");
        buf_puts(b, "; break; }");
      }
      else {
        buf_printf(b, " if (_t%d.tag == SP_TAG_STR || sp_poly_is_strbuf(_t%d)) {"
                      " sp_oint _t%d = sp_poly_str_index_val(_t%d, %s, %d); _t%d = ",
                   tv, tv, tsi, tv, ab5.p ? ab5.p : "sp_box_nil()",
                   sp_streq(name, "rindex") ? 1 : 0, tr);
        if (ret == TY_INT) buf_printf(b, "%s_t%d%s", node_is_oint(c, id) ? "" : "sp_oint_arg(", tsi, node_is_oint(c, id) ? "" : ")");
        else buf_printf(b, "sp_box_oint(_t%d)", tsi);
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
      buf_printf(b, " { sp_oint _t%d; if (sp_poly_enum_find_index_val(_t%d, %s, &_t%d)) { _t%d = ",
                 tsi, tv, ab7.p ? ab7.p : "sp_box_nil()", tsi, tr);
      if (ret == TY_INT) buf_printf(b, "%s_t%d%s", node_is_oint(c, id) ? "" : "sp_oint_arg(", tsi, node_is_oint(c, id) ? "" : ")");
      else buf_printf(b, "sp_box_oint(_t%d)", tsi);
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
    int obj_arity = dl_open && emit_poly_plan_default_n(c, id, name, kw->kwh, pos_argc, splat_a, tv, b);
    int sum_done = !obj_arity && dl_open && ret == TY_POLY &&
                   emit_poly_default_blk_arm(c, id, name, argc, argv, atmp, atmp_ty, tv, tr, blk_tmp2, b);
    int answer = sum_done ? 1 : 0;
    if (!obj_arity && !sum_done && (!dl_open ||
        (!(answer = emit_poly_aset_default(c, name, argc, atmp, atmp_ty, ret, tv, tr, b) ? 2 : 0) &&
         !(answer = emit_poly_builtin_default(c, id, recv, name, argc, argv, atmp, atmp_ty,
                                              ret, tv, is_setter_val ? -1 : tr, 0, b) ? 3 : 0))))
      buf_printf(b, " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d)); break;", name, tv);
    /* A declined builtin trial may leave a nested frame behind. Its
       result belongs to this switch, as the plan's trial does. */
    if (g_plan_check) pa_resume(pa_frame);
    if (g_plan_check && dl_open && !obj_arity)
      pa_observe(PA_TRIAL, PA_KEY_TRIAL + PT_GENERIC_TAIL, -1, TY_UNKNOWN, answer);
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
    /* a value that is no collection is the call's NoMethodError, as on the
       route without a user class (sp_poly_enum_chk): "hello".first(1)
       answered [] (last's helper asks itself) */
    char rv[64];
    if (sp_streq(name, "first")) snprintf(rv, sizeof rv, "sp_poly_enum_chk(_t%d, \"first\")", tv);
    else snprintf(rv, sizeof rv, "_t%d", tv);
    /* The count: an Integer as it is, a value known only at run time through
       the runtime's conversion (a Float truncates, anything else is CRuby's
       TypeError). A count of another known type (the keyword Hash a computed
       `public_send(*args, **kwargs)` hands every one-argument name, `first`
       among them) takes the same conversion boxed: its pointer went into the
       sp_int parameter as it was, and the C did not compile. */
    Buf cx; memset(&cx, 0, sizeof cx);
    if (atmp_ty[0] == TY_INT) buf_puts(&cx, nx);
    else {
      buf_puts(&cx, "sp_poly_arg_int_chk(");
      if (atmp_ty[0] == TY_POLY) buf_puts(&cx, nx);
      else emit_boxed_text(c, atmp_ty[0], nx, &cx);
      buf_puts(&cx, ")");
    }
    snprintf(gen, sizeof gen, "sp_poly_%s_n(%s, %s)",
             sp_streq(name, "first") ? "first" : "last", rv, cx.p ? cx.p : nx);
    free(cx.p);
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
    int splat_n = nt_argv_has_splat(nt, argv, argc);
    int tkl = -1;
    if (splat_n && !is_pdelete) {
      tkl = ++g_tmp;
      buf_printf(b, " sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", tkl, tkl);
      for (int a = 0; a < argc; a++) {
        char tn[32]; snprintf(tn, sizeof tn, "_t%d", atmp[a]);
        if (nt_kind(nt, argv[a]) == NK_SplatNode) {
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
      snprintf(gen, sizeof gen, "sp_poly_delete_key_blk_bytes(_t%d, %s, _t%d)", tv, ab.p ? ab.p : "sp_box_nil()", dblk);
    else if (is_pdelete)
      snprintf(gen, sizeof gen, "sp_poly_delete_key_bytes(_t%d, %s)", tv, ab.p ? ab.p : "sp_box_nil()");
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
    if (ret == TY_POLY)
      buf_printf(b, " default: _t%d = sp_box_obj(sp_poly_hash_merge(_t%d, %s), SP_BUILTIN_POLY_POLY_HASH); break;",
                 tr, tv, mb.p ? mb.p : "sp_box_nil()");
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
  /* cover?: no builtin but a Range has the method; a receiver of another
     class raises, as in CRuby, where the switch left it at the result's
     initializer (false). A Range the pre-arms did not take answers false:
     the argument is of no class the Range covers. A boxed result starts at
     nil, so it is given the false. */
  else if (is_cover) {
    buf_printf(b, " default: if (!(_t%d.tag == SP_TAG_OBJ && (_t%d.cls_id == SP_BUILTIN_RANGE ||"
                  " _t%d.cls_id == SP_BUILTIN_FLOAT_RANGE || _t%d.cls_id == SP_BUILTIN_STR_RANGE)))"
                  " sp_raise_nomethod(sp_nomethod_msg(\"%s\", _t%d));", tv, tv, tv, tv, name, tv);
    if (ret == TY_POLY) buf_printf(b, " else _t%d = sp_box_bool(0);", tr);
    buf_puts(b, " break;");
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
  view_unbind(kwmark);
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
  /* A callable forwarded to a name the enumerable driver serves
     (`xs.sort_by(&f)`) has no block body for the array emitters to splice:
     re-entered as an Array call, it found no emitter and raised
     NoMethodError naming Array. The driver takes it, where it answers for
     every value the receiver holds (cplan_driver_walks_recv). A literal
     block the forward resolves to (a spliced caller's) is spliced. */
  int blk = nt_ref(nt, id, "block"), rfb = resolve_forwarded_block(c, blk);
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode && (rfb < 0 || nt_kind(nt, rfb) == NK_BlockArgumentNode) &&
      argc == 0 && poly_enum_op_for(name) && cplan_driver_walks_recv(c, id)) return 0;
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
         is_each_window(name)));
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
