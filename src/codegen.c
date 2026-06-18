#include "codegen_internal.h"

void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  if (t == TY_EXCEPTION) { buf_printf(b, "sp_box_obj(%s, SP_BUILTIN_EXCEPTION)", expr); return; }
  if (t == TY_FIBER) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_FIBER)", expr); return; }
  if (t == TY_IO) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_IO)", expr); return; }
  if (t == TY_RACTOR_PORT) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_RACTOR_PORT)", expr); return; }
  if (ty_is_object(t)) { buf_printf(b, "sp_box_obj(%s, %d)", expr, ty_object_class(t)); return; }
  if (ty_is_hash(t) && hash_box_cls(t)) { buf_printf(b, "sp_box_obj(%s, %s)", expr, hash_box_cls(t)); return; }
  const char *fn = NULL;
  switch (t) {
    case TY_INT: fn = "sp_box_int"; break;       case TY_FLOAT: fn = "sp_box_float"; break;
    case TY_STRING: fn = "sp_box_str"; break;     case TY_BOOL: fn = "sp_box_bool"; break;
    case TY_SYMBOL: fn = "sp_box_sym"; break;     case TY_RANGE: fn = "sp_box_range"; break;
    case TY_TIME: fn = "sp_box_time"; break;
    case TY_PROC: fn = "sp_box_proc"; break;
    case TY_METHOD: fn = "sp_box_method"; break;
    case TY_CLASS: fn = "sp_box_class"; break;
    case TY_INT_ARRAY: fn = "sp_box_int_array"; break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY: fn = "sp_box_str_array"; break;
    case TY_POLY_ARRAY: fn = "sp_box_poly_array"; break;
    case TY_NIL: buf_puts(b, "sp_box_nil()"); return;
    default: break;
  }
  if (fn) buf_printf(b, "%s(%s)", fn, expr);
  else buf_printf(b, "sp_box_int(%s)", expr);  /* fallback */
}

/* Emit `expr` (a poly value) unboxed to its concrete C representation. */
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  switch (t) {
    case TY_INT:    buf_printf(b, "(%s).v.i", expr); return;
    case TY_FLOAT:  buf_printf(b, "(%s).v.f", expr); return;
    case TY_STRING: buf_printf(b, "(%s).v.s", expr); return;
    case TY_BOOL:   buf_printf(b, "(%s).v.b", expr); return;
    case TY_SYMBOL: buf_printf(b, "(sp_sym)(%s).v.i", expr); return;
    default: break;
  }
  if (ty_is_object(t)) { buf_printf(b, "(sp_%s *)(%s).v.p", c->classes[ty_object_class(t)].name, expr); return; }
  const char *cn = c_type_name(t);
  if (cn) buf_printf(b, "(%s)(%s).v.p", cn, expr);
  else buf_printf(b, "(%s).v.i", expr);
}

/* Emit a node as an mrb_int, coercing a poly value through sp_poly_to_i. Used
   where the runtime ABI demands a raw integer (array indices, etc.) but the
   expression's static type widened to poly. */
void emit_int_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_i("); emit_expr(c, node, b); buf_puts(b, ")");
  }
  else emit_expr(c, node, b);
}

/* Emit a node as an mrb_float. A poly value is unboxed via sp_poly_to_f; any
   other (numeric) value is plain-cast, matching the legacy `(mrb_float)(...)`. */
void emit_float_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_f("); emit_expr(c, node, b); buf_puts(b, ")");
  }
  else { buf_puts(b, "(mrb_float)("); emit_expr(c, node, b); buf_puts(b, ")"); }
}

void emit_boxed(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY) { emit_expr(c, node, b); return; }
  if (t == TY_FIBER) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_FIBER)");
    return;
  }
  if (t == TY_IO) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_IO)");
    return;
  }
  if (t == TY_RACTOR_PORT) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_RACTOR_PORT)");
    return;
  }
  if (t == TY_EXCEPTION) {
    buf_printf(b, "sp_box_obj("); emit_expr(c, node, b); buf_puts(b, ", SP_BUILTIN_EXCEPTION)");
    return;
  }
  if (ty_is_object(t)) {
    buf_printf(b, "sp_box_obj(");
    emit_expr(c, node, b);
    buf_printf(b, ", %d)", ty_object_class(t));
    return;
  }
  if (ty_is_hash(t)) {
    const char *hid = hash_box_cls(t);
    if (hid) { buf_printf(b, "sp_box_obj("); emit_expr(c, node, b); buf_printf(b, ", %s)", hid); return; }
    unsupported(c, node, "boxing value into poly"); return;
  }
  /* regex values can appear in poly context (multi-typed local); box as nil */
  if (t == TY_REGEX) { buf_puts(b, "sp_box_nil()"); return; }
  /* class/module value: box into poly */
  if (t == TY_CLASS) {
    buf_puts(b, "sp_box_class("); emit_expr(c, node, b); buf_puts(b, ")"); return;
  }
  /* an empty array literal [] has TY_UNKNOWN; box it as an empty PolyArray so
     it can hold any element type when stored into a poly slot */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && !strcmp(nt_type(c->nt, node), "ArrayNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_poly_array(sp_PolyArray_new())"); return; }
  }
  /* an empty hash literal {} has TY_UNKNOWN; box it as an empty PolyPolyHash */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && !strcmp(nt_type(c->nt, node), "HashNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"); return; }
  }
  const char *fn = NULL;
  switch (t) {
    case TY_INT:    fn = "sp_box_int";   break;
    case TY_FLOAT:  fn = "sp_box_float"; break;
    case TY_STRING: fn = "sp_box_str";   break;
    case TY_BOOL:   fn = "sp_box_bool";  break;
    case TY_SYMBOL: fn = "sp_box_sym";   break;
    case TY_RANGE:  fn = "sp_box_range"; break;
    case TY_TIME:   fn = "sp_box_time";  break;
    case TY_PROC:   fn = "sp_box_proc";  break;
    case TY_METHOD: fn = "sp_box_method"; break;
    case TY_INT_ARRAY:   fn = "sp_box_int_array";   break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY:   fn = "sp_box_str_array";   break;
    case TY_POLY_ARRAY:  fn = "sp_box_poly_array";  break;
    case TY_NIL: {
      const char *nty = nt_type(c->nt, node);
      if (nty && !strcmp(nty, "NilNode")) { buf_puts(b, "sp_box_nil()"); return; }
      /* a nil-typed expression can still have side effects (e.g. a void-valued
         block call): evaluate it for effect, then yield nil */
      buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())");
      return;
    }
    default: break;
  }
  if (!fn) {
    /* TY_UNKNOWN (e.g. unrecognized stdlib class .new): evaluate for side-effects, yield nil */
    buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())"); return;
  }
  buf_printf(b, "%s(", fn);
  emit_expr(c, node, b);
  buf_puts(b, ")");
}

/* `vol` makes the local volatile (required for locals live across a setjmp
   in a begin/rescue). Pointers need the volatile on the pointer itself
   (T * volatile), value types take a leading qualifier. */
void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol) {
  TyKind t = lv->type;
  Buf cty; memset(&cty, 0, sizeof cty);
  const char *init = "0";
  int ptr = 0, root = needs_root(t);
  switch (t) {
    case TY_INT:    buf_puts(&cty, "mrb_int"); init = "0"; break;
    case TY_BIGINT: buf_puts(&cty, "sp_Bigint *"); init = "NULL"; ptr = 1; break;
    case TY_FLOAT:  buf_puts(&cty, "mrb_float"); init = "0.0"; break;
    case TY_BOOL:   buf_puts(&cty, "mrb_bool"); init = "0"; break;
    case TY_SYMBOL: buf_puts(&cty, "sp_sym"); init = "((sp_sym)-1)"; break;
    case TY_RANGE:  buf_puts(&cty, "sp_Range"); init = "{0}"; break;
    case TY_TIME:   buf_puts(&cty, "sp_Time"); init = "{0}"; break;
    case TY_COMPLEX:  buf_puts(&cty, "sp_Complex"); init = "{0}"; break;
    case TY_RATIONAL: buf_puts(&cty, "sp_Rational"); init = "{0}"; break;
    case TY_STRING: buf_puts(&cty, "const char *"); init = "(&(\"\\xff\")[1])"; ptr = 1; break;
    case TY_POLY:   buf_puts(&cty, "sp_RbVal"); init = "sp_box_nil()"; break;
    case TY_CLASS:  buf_puts(&cty, "sp_Class"); init = "((sp_Class){-1})"; break;
    default:
      if (comp_ty_value_obj(c, t)) { emit_ctype(c, t, &cty); init = "{0}"; ptr = 0; }
      else if (is_scalar_ret(t) && t != TY_UNKNOWN) { emit_ctype(c, t, &cty); init = "NULL"; ptr = 1; }
      else {
        fprintf(stderr, "spinel: local '%s' has unsupported type %s\n", lv->name, ty_name(t));
        exit(1);
      }
  }
  buf_puts(b, "    ");
  if (vol && !ptr) buf_puts(b, "volatile ");
  buf_puts(b, cty.p ? cty.p : "");
  if (vol && ptr) buf_puts(b, "volatile ");  /* cty ends with "* "; -> "* volatile " */
  buf_printf(b, " lv_%s = %s;\n", lv->name, init);
  if (t == TY_POLY) buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", lv->name);
  else if (root && !comp_ty_value_obj(c, t)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
  else if (comp_ty_value_obj(c, t)) {
    /* a value-type local lives on the stack; root each heap-pointer (string)
       field so its referent survives GC. The field slot is a stable root. */
    ClassInfo *vc = &c->classes[ty_object_class(t)];
    for (int i = 0; i < vc->nivars; i++)
      if (vc->ivar_types[i] == TY_STRING)
        buf_printf(b, "    SP_GC_ROOT(lv_%s.iv_%s);\n", lv->name, vc->ivars[i] + 1);
  }
  free(cty.p);
}

/* Does scope index `si` contain a begin/rescue (so its locals need volatile)? */
int scope_has_begin(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    const char *ty = nt_type(c->nt, id);
    if (ty && (!strcmp(ty, "BeginNode") || !strcmp(ty, "RescueNode")) && c->nscope[id] == si)
      return 1;
  }
  return 0;
}

/* Declare a scope's locals. Params are already C function parameters, so
   they only need a GC root; body locals get a full declaration. */
void emit_scope_decls(Compiler *c, Scope *s, Buf *b) {
  int vol = scope_has_begin(c, (int)(s - c->scopes));
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *lv = &s->locals[i];
    /* define_method subst var: replaced inline by the literal, never a C
       local, so neither declare nor root it. */
    if (s->dm_subst_name && lv->name && !strcmp(lv->name, s->dm_subst_name)) continue;
    /* Virtual &block slot: skip declaration UNLESS it's a lowered __yblk__ that
       needs a cell (so forwarding procs can capture it). */
    if (s->blk_param && lv->name && !strcmp(lv->name, s->blk_param) && !lv->is_cell) continue;
    /* Captured-by-closure local: lives in a heap cell so the proc and this
       scope share storage. A param's incoming value is copied into the cell;
       a body local starts at 0. Int and proc cells supported. */
    if (lv->is_cell) {
      if (lv->type == TY_PROC) {
        buf_printf(b, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, NULL);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = (mrb_int)(uintptr_t)lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = 0;\n", lv->name);
        continue;
      }
      /* A pointer (string / array / hash / heap object) capture is laundered
         through the int cell as (uintptr_t)<ptr>, with a cell scan that marks
         the referent. Int / bool stay direct. Float / poly / by-value structs
         don't fit a single int cell -- still deferred. */
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN && !ptr_cell)
        unsupported(c, s->def_node, "closure capturing a non-integer variable (later slice)");
      const char *cell_scan = !ptr_cell ? "NULL"
                            : (lv->type == TY_STRING ? "sp_cell_scan_str" : "sp_cell_scan_ptr");
      buf_printf(b, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, %s);\n", lv->name, cell_scan);
      buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
      if (lv->is_param && ptr_cell) buf_printf(b, "    *_cell_%s = (mrb_int)(uintptr_t)lv_%s;\n", lv->name, lv->name);
      else if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
      else buf_printf(b, "    *_cell_%s = 0;\n", lv->name);
      continue;
    }
    if (lv->is_param) {
      /* A poly param is an sp_RbVal by value: root through the tagged
         RBVAL form so the collector reads the boxed pointer, not the
         struct's first word (the tag). */
      if (lv->type == TY_POLY) buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", lv->name);
      else if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
    }
    else {
      declare_local(c, b, lv, vol);
    }
  }
}

/* ---- methods ---- */

int method_is_void(Scope *s) {
  /* initialize is always void (mutates *self); else by return type */
  if (s->class_id >= 0 && s->name && !strcmp(s->name, "initialize")) return 1;
  return !is_scalar_ret(s->ret);
}

/* The mangled C name: sp_<name> for free functions, sp_<Class>_<name>
   for instance methods. */
void emit_method_cname(Compiler *c, Scope *s, Buf *b) {
  if (s->class_id >= 0 && s->is_cmethod)
    buf_printf(b, "sp_%s_s_%s", c->classes[s->class_id].name, mc(s->name));
  else if (s->class_id >= 0)
    buf_printf(b, "sp_%s_%s", c->classes[s->class_id].name, mc(s->name));
  else
    buf_printf(b, "sp_%s", mc(s->name));
}

void emit_method_signature(Compiler *c, Scope *s, Buf *b) {
  /* In a debug build, give instance/class methods external linkage so
     -rdynamic exposes sp_<Class>_<method> to backtrace_symbols and the
     frames demangle (Exception#backtrace / Kernel#caller). Toplevel methods
     keep `static` -- a bare sp_<name> could collide with a runtime helper. */
  const char *stor = (g_debug && s->class_id >= 0) ? "" : "static ";
  if (method_is_void(s)) { buf_puts(b, stor); buf_puts(b, "void "); }
  else { buf_puts(b, stor); emit_ctype(c, s->ret, b); buf_puts(b, " "); }
  emit_method_cname(c, s, b);
  buf_puts(b, "(");
  int wrote = 0;
  if (s->class_id >= 0 && !s->is_cmethod) {
    const char *cn = c->classes[s->class_id].name;
    if (!strcmp(cn, "String"))       { buf_puts(b, "const char *self"); }
    else if (!strcmp(cn, "Integer")) { buf_puts(b, "mrb_int self"); }
    else if (!strcmp(cn, "Float"))   { buf_puts(b, "double self"); }
    else if (!strcmp(cn, "Symbol"))  { buf_puts(b, "mrb_int self"); }
    else if (!strcmp(cn, "TrueClass") || !strcmp(cn, "FalseClass") || !strcmp(cn, "NilClass")) { buf_puts(b, "int self"); }
    else if (!strcmp(cn, "Array"))   { buf_puts(b, "sp_RbVal self"); }
    else if (!strcmp(cn, "Object") || !strcmp(cn, "Numeric")) { buf_puts(b, "sp_RbVal self"); }
    else {
      /* value-type reader methods take self by value; initialize keeps a
         pointer so it can populate the fields during construction. */
      int vt = c->classes[s->class_id].is_value_type;
      int is_init = s->name && !strcmp(s->name, "initialize");
      buf_printf(b, "sp_%s %sself", cn, (vt && !is_init) ? "" : "*");
    }
    wrote = 1;
  }
  for (int i = 0; i < s->nparams; i++) {
    if (wrote++) buf_puts(b, ", ");
    LocalVar *p = scope_local(s, s->pnames[i]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    if (!is_scalar_ret(pt)) {
      fprintf(stderr, "spinel: method '%s' param '%s' has unsupported type %s\n",
              s->name, s->pnames[i], ty_name(pt));
      exit(1);
    }
    emit_ctype(c, pt, b);
    buf_printf(b, " lv_%s", s->pnames[i]);
  }
  /* &block param that escapes (not inlined): passes the block as sp_Proc * */
  if (s->blk_param && s->blk_param[0] && !s->yields) {
    if (wrote++) buf_puts(b, ", ");
    buf_printf(b, "sp_Proc *lv_%s", s->blk_param);
  }
  if (!wrote) buf_puts(b, "void");
  buf_puts(b, ")");
}

/* CS_SYNTH_* markers (mirror of analyze_scope.c). */
enum { CG_CS_INIT = 1, CG_CS_DUMP, CG_CS_SET_INT, CG_CS_SET_STR, CG_CS_SET_SA, CG_CS_SET_IA };

/* Emit a synthesized compiler_state method body (no AST). */
static void emit_compiler_state_method(Compiler *c, Scope *s, Buf *b) {
  emit_method_signature(c, s, b);
  buf_puts(b, " {\n");
  ClassInfo *ci = &c->classes[s->class_id];
  const char *cn = ci->name;
  if (s->cs_synth == CG_CS_INIT) {
    for (int i = 0; i < ci->ncs; i++) {
      const char *nm = ci->cs_names[i], *k = ci->cs_kinds[i];
      buf_printf(b, "  self->iv_%s = ", nm);
      if (!strcmp(k, "str")) emit_str_literal(b, "");
      else if (!strcmp(k, "sa")) buf_puts(b, "sp_StrArray_new()");
      else if (!strcmp(k, "ia")) buf_puts(b, "sp_IntArray_new()");
      else buf_puts(b, "0");
      buf_puts(b, ";\n");
    }
    buf_puts(b, "  return 0;\n}\n");
  }
  else if (s->cs_synth == CG_CS_DUMP) {
    int defcls = -1;
    if (comp_method_in_chain(c, s->class_id, "ir_emit_int", &defcls) < 0) {
      buf_puts(b, "  return lv_buf;\n}\n");
      return;
    }
    const char *ecn = c->classes[defcls].name;
    for (int i = 0; i < ci->ncs; i++) {
      const char *nm = ci->cs_names[i], *k = ci->cs_kinds[i];
      char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nm);
      buf_printf(b, "  lv_buf = sp_%s_ir_emit_%s(self, lv_buf, ", ecn, k);
      emit_str_literal(b, ivn);
      buf_printf(b, ", self->iv_%s);\n", nm);
    }
    buf_puts(b, "  return lv_buf;\n}\n");
  }
  else {
    const char *want = s->cs_synth == CG_CS_SET_INT ? "int" :
                       s->cs_synth == CG_CS_SET_STR ? "str" :
                       s->cs_synth == CG_CS_SET_SA  ? "sa"  : "ia";
    for (int i = 0; i < ci->ncs; i++) {
      if (strcmp(ci->cs_kinds[i], want)) continue;
      const char *nm = ci->cs_names[i];
      char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", nm);
      buf_puts(b, "  if (sp_str_eq(lv_name, ");
      emit_str_literal(b, ivn);
      buf_printf(b, ")) { self->iv_%s = lv_val; }\n", nm);
    }
    buf_puts(b, "  return 0;\n}\n");
  }
  (void)cn;
}

/* An `include M` into a user class clones M's instance methods into a scope
   owned by the class, but that clone *shares* the source's body AST and only
   registers the params -- the body locals stay on the source scope, which
   codegen skips (is_transplanted_source). Since codegen emits the clone, its
   declarations would be missing and every `lv_<name>` body reference would be
   undeclared (#1435). Copy the source's body locals onto the clone, reusing
   their already-inferred types. Gated on a shared body so the builtin-target
   clone (which re-walks its own body copy and registers its own locals) is
   left untouched. */
static void inherit_transplant_locals(Compiler *c, Scope *s) {
  if (s->def_node < 0 || s->is_transplanted_source) return;
  for (int i = 0; i < c->nscopes; i++) {
    Scope *src = &c->scopes[i];
    if (src == s || !src->is_transplanted_source ||
        src->def_node != s->def_node || src->body != s->body) continue;
    for (int k = 0; k < src->nlocals; k++) {
      LocalVar sl = src->locals[k];  /* copy: intern may realloc src->locals */
      if (!sl.name || scope_local(s, sl.name)) continue;
      LocalVar *dl = scope_local_intern(s, sl.name);
      dl->type = sl.type;
      dl->is_param = sl.is_param;
      dl->is_block_param = sl.is_block_param;
      dl->proc_ret = sl.proc_ret;
      dl->is_cell = sl.is_cell;
    }
    break;
  }
}

void emit_method(Compiler *c, Scope *s, Buf *b) {
  if (s->cs_synth) { emit_compiler_state_method(c, s, b); return; }
  /* instance_eval/exec trampolines are inlined at every call site; the
     method body itself is an unreachable stub (matches the legacy compiler). */
  if (s->class_id >= 0 && !s->is_cmethod && s->name &&
      comp_trampoline_kind(c, s->class_id, s->name, NULL)) {
    emit_method_signature(c, s, b);
    if (method_is_void(s)) {
      /* A `return <value>;` in a void function is a constraint violation that
         MinGW gcc flags under -Werror (-Wno-all doesn't cover -Wreturn-type
         there); emit an empty body instead. */
      buf_puts(b, " {\n}\n");
    }
    else {
      buf_puts(b, " {\n  return ");
      if (ty_is_object(s->ret)) buf_puts(b, "NULL");
      else buf_puts(b, default_value(s->ret));
      buf_puts(b, ";\n}\n");
    }
    return;
  }
  inherit_transplant_locals(c, s);
  emit_method_signature(c, s, b);
  buf_puts(b, " {\n");
  buf_puts(b, "    SP_GC_SAVE();\n");
  emit_scope_decls(c, s, b);
  TyKind saved_rt = g_ret_type;
  int saved_ed = g_ensure_depth; g_ensure_depth = 0;
  int saved_emcls = g_emitting_class_id; g_emitting_class_id = s->class_id;
  const char *saved_dmn = g_dm_subst_name; int saved_dmnode = g_dm_subst_node;
  g_dm_subst_name = s->dm_subst_name; g_dm_subst_node = s->dm_subst_node;
  int saved_lowered = g_current_scope_is_lowered; g_current_scope_is_lowered = s->is_lowered_yield;
  /* value-type reader methods receive self by value, so ivar access uses `.` */
  const char *saved_deref = g_self_deref;
  g_self_deref = (s->class_id >= 0 && !s->is_cmethod && c->classes[s->class_id].is_value_type &&
                  s->name && strcmp(s->name, "initialize")) ? "." : "->";
  g_ret_type = method_is_void(s) ? TY_VOID : s->ret;
  if (method_is_void(s)) {
    emit_stmts(c, s->body, b, 1);
  }
  else {
    emit_stmts_tail(c, s->body, b, 1);
    buf_puts(b, "  return ");
    if (ty_is_object(s->ret)) {
      if (comp_ty_value_obj(c, s->ret)) buf_printf(b, "(sp_%s){0};\n", c->classes[ty_object_class(s->ret)].name);
      else buf_puts(b, "NULL;\n"); /* unreachable default (object pointer) */
    }
    else buf_printf(b, "%s;\n", default_value(s->ret));
  }
  g_self_deref = saved_deref;
  g_ret_type = saved_rt; g_ensure_depth = saved_ed;
  g_emitting_class_id = saved_emcls;
  g_dm_subst_name = saved_dmn; g_dm_subst_node = saved_dmnode;
  g_current_scope_is_lowered = saved_lowered;
  buf_puts(b, "}\n");
}

/* ---- first-class Proc ---- */

/* Block bodies don't get their own Scope: a block's params and the locals it
   assigns live in the ENCLOSING scope (the inline model). To emit a proc body
   as a standalone function we therefore work over the body SUBTREE, not a
   scope: its bound names are the block params plus the locals it writes; a
   read of any other name is a captured/free variable. (NameSet + helpers are
   defined near the top, shared with the cell-capture machinery.) */

/* True if `id` starts a nested block/lambda whose locals belong to it, not to
   the proc we're walking -- recursion stops there. */
int is_nested_block(const char *ty) {
  return ty && (!strcmp(ty, "BlockNode") || !strcmp(ty, "LambdaNode"));
}

/* Collect the local names WRITTEN in the proc body subtree (the proc's own
   locals), not descending into nested blocks. */
void proc_collect_locals(Compiler *c, int id, NameSet *locals) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (!strcmp(ty, "LocalVariableWriteNode") || !strcmp(ty, "LocalVariableTargetNode") ||
      !strcmp(ty, "LocalVariableOperatorWriteNode") || !strcmp(ty, "LocalVariableOrWriteNode") ||
      !strcmp(ty, "LocalVariableAndWriteNode"))
    nameset_add(locals, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(c->nt, id, i);
    if (ch >= 0 && !is_nested_block(nt_type(c->nt, ch))) proc_collect_locals(c, ch, locals);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (ids[k] >= 0 && !is_nested_block(nt_type(c->nt, ids[k]))) proc_collect_locals(c, ids[k], locals);
  }
}

/* Collect all local names used (read or written) directly in the proc body,
   not crossing nested blocks. The caller classifies each as the proc's own
   param/local or a captured enclosing var (is_cell). */
void proc_collect_used(Compiler *c, int id, NameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (!strcmp(ty, "LocalVariableReadNode") || !strcmp(ty, "LocalVariableWriteNode") ||
      !strcmp(ty, "LocalVariableTargetNode") || !strcmp(ty, "LocalVariableOperatorWriteNode") ||
      !strcmp(ty, "LocalVariableOrWriteNode") || !strcmp(ty, "LocalVariableAndWriteNode"))
    nameset_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0 && !is_nested_block(nt_type(c->nt, ch))) proc_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0 && !is_nested_block(nt_type(c->nt, ids[k]))) proc_collect_used(c, ids[k], out); }
}

/* The ParametersNode of a proc-creating node. A `->{}` LambdaNode carries it
   directly (`parameters`); a `proc {}` / `lambda {}` / block-escape pass-through
   nests it one level deeper (block/BlockNode -> BlockParametersNode -> ParametersNode). */
int proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  /* BlockNode used directly as a proc (escaped &block) */
  if (ty && !strcmp(ty, "BlockNode")) {
    int bp = nt_ref(c->nt, create, "parameters");
    if (bp < 0) return -1;
    return nt_ref(c->nt, bp, "parameters");
  }
  int block = nt_ref(c->nt, create, "block");
  if (block < 0) return -1;
  int bp = nt_ref(c->nt, block, "parameters");   /* BlockParametersNode */
  if (bp < 0) return -1;
  return nt_ref(c->nt, bp, "parameters");        /* ParametersNode */
}
const char *proc_param_name(Compiler *c, int create, int idx) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return NULL;
  int n = 0;
  const int *reqs = nt_arr(c->nt, pn, "requireds", &n);
  return idx < n ? nt_str(c->nt, reqs[idx], "name") : NULL;
}
/* The StatementsNode body of a proc-creating node. */
int proc_body_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && !strcmp(ty, "LambdaNode")) return nt_ref(c->nt, create, "body");
  if (ty && !strcmp(ty, "BlockNode")) return nt_ref(c->nt, create, "body");
  int block = nt_ref(c->nt, create, "block");
  return block >= 0 ? nt_ref(c->nt, block, "body") : -1;
}

/* Proc args + return ride the mrb_int slot of sp_proc_call. A value that fits
   an mrb_int directly (int/bool/symbol/nil) needs no conversion; a heap pointer
   (string/array/hash/object) is laundered through (mrb_int)(uintptr_t). Other
   shapes (float, poly, range, time) don't fit the slot and defer. */
int proc_slot_is_direct(TyKind t) { return t == TY_INT || t == TY_BOOL || t == TY_SYMBOL || t == TY_NIL || t == TY_UNKNOWN; }
int proc_slot_is_ptr(TyKind t) { return t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t); }

/* True if the AST subtree at `id` has a YieldNode, not crossing DefNode. */
int proc_body_has_yield(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "YieldNode")) return 1;
  if (!strcmp(ty, "DefNode")) return 0;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (proc_body_has_yield(c, ch)) return 1; }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (proc_body_has_yield(c, ids[k])) return 1; }
  return 0;
}

/* Lower `Fiber.new { |param| body }` into a static void fn and sp_Fiber_new.
   No-capture case: all locals in the body are fiber-function locals; any
   reference to an outer-scope variable that is NOT heap-celled will compile
   fine only when it's a parameter of the enclosing method (passed by value).
   Captured outer locals (is_cell) are not yet supported — those fibers will
   produce a C compile error rather than silently miscompiling. */
/* Returns 1 if a type needs a GC root when stored in a fiber capture struct. */
int fiber_cap_needs_root(TyKind t) {
  return t == TY_STRING || t == TY_BIGINT || ty_is_array(t) || ty_is_hash(t) ||
         ty_is_object(t) || t == TY_POLY || t == TY_PROC || t == TY_FIBER ||
         t == TY_EXCEPTION || t == TY_STRINGIO || t == TY_STRINGSCANNER ||
         t == TY_MATCHDATA || t == TY_REGEX || t == TY_TIME;
}

/* Returns 1 if the fiber body accesses instance state (ivars, self, or
   implicit self-dispatch calls) without crossing into nested blocks/lambdas. */
int fiber_body_uses_self(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "InstanceVariableReadNode") || !strcmp(ty, "InstanceVariableWriteNode") ||
      !strcmp(ty, "InstanceVariableOperatorWriteNode") ||
      !strcmp(ty, "InstanceVariableOrWriteNode") || !strcmp(ty, "InstanceVariableAndWriteNode") ||
      !strcmp(ty, "SelfNode")) return 1;
  if (!strcmp(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) return 1;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(c->nt, id, i);
    if (ch >= 0 && !is_nested_block(nt_type(c->nt, ch)) && fiber_body_uses_self(c, ch)) return 1;
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (ids[k] >= 0 && !is_nested_block(nt_type(c->nt, ids[k])) && fiber_body_uses_self(c, ids[k])) return 1;
  }
  return 0;
}

void emit_fiber_new(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) { buf_puts(b, "sp_Fiber_new(NULL)"); return; }

  int fid = ++g_fiber_counter;
  char fname[48];
  snprintf(fname, sizeof fname, "_fiber_body_%d", fid);

  /* Block parameter name (first required, if any) */
  const char *bp0 = NULL;
  int bp_node = nt_ref(nt, blk, "parameters");
  if (bp_node >= 0) {
    int inner = nt_ref(nt, bp_node, "parameters");
    int pn = inner >= 0 ? inner : bp_node;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    if (rn > 0) bp0 = nt_str(nt, reqs[0], "name");
  }
  int body = nt_ref(nt, blk, "body");
  Scope *encl = comp_scope_of(c, id);

  /* Collect locals written inside this fiber body (not in nested blocks). */
  NameSet fib_locals = {0};
  if (body >= 0) proc_collect_locals(c, body, &fib_locals);

  /* Compute captures: names used in the body that belong to the enclosing scope
     but are NOT defined by the fiber body itself and NOT the block param. */
  NameSet fib_used = {0};
  if (body >= 0) proc_collect_used(c, body, &fib_used);

  /* caps: outer-scope vars referenced in body but not written in body */
  NameSet caps = {0};
  if (encl) {
    for (int u = 0; u < fib_used.n; u++) {
      const char *nm = fib_used.v[u];
      if (bp0 && !strcmp(nm, bp0)) continue;
      if (nameset_has(&fib_locals, nm)) continue;
      LocalVar *lv = scope_local(encl, nm);
      if (!lv || lv->type == TY_UNKNOWN) continue;
      nameset_add(&caps, nm);
    }
  }
  free(fib_used.v);

  int ncap = caps.n;

  /* Capture self if the body accesses ivars or dispatches to self implicitly */
  int cap_self = 0;
  const char *cap_self_class = NULL;
  if (encl && encl->class_id >= 0 && !encl->is_cmethod && body >= 0 && fiber_body_uses_self(c, body)) {
    cap_self = 1;
    cap_self_class = c->classes[encl->class_id].name;
  }

  /* Emit capture struct + GC scan function when there are captured vars or self */
  if (ncap > 0 || cap_self) {
    buf_printf(&g_proc_protos, "typedef struct {");
    if (cap_self) buf_printf(&g_proc_protos, " sp_%s *self_ptr;", cap_self_class);
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      TyKind ct = lv ? lv->type : TY_POLY;
      buf_printf(&g_proc_protos, " "); emit_ctype(c, ct, &g_proc_protos);
      buf_printf(&g_proc_protos, " %s;", caps.v[i]);
    }
    buf_printf(&g_proc_protos, " } _fib_cap_%d;\n", fid);
    buf_printf(&g_proc_protos, "static void _fib_cap_scan_%d(void *p) {\n", fid);
    buf_printf(&g_proc_protos, "  sp_gc_mark(p);\n");
    buf_printf(&g_proc_protos, "  _fib_cap_%d *_c = (_fib_cap_%d *)p;\n", fid, fid);
    if (cap_self) buf_printf(&g_proc_protos, "  if (_c->self_ptr) sp_gc_mark((void *)_c->self_ptr);\n");
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      TyKind ct = lv ? lv->type : TY_POLY;
      if (fiber_cap_needs_root(ct)) {
        if (ct == TY_POLY) buf_printf(&g_proc_protos, "  sp_gc_mark_rbval(_c->%s);\n", caps.v[i]);
        else buf_printf(&g_proc_protos, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
      }
    }
    buf_printf(&g_proc_protos, "}\n");
  }

  /* Emit fiber body function prototype before main bodies */
  buf_printf(&g_proc_protos, "static void %s(sp_Fiber *_fb);\n", fname);

  /* Emit fiber body function into g_procs buffer */
  Buf *pb = &g_procs;
  buf_printf(pb, "static void %s(sp_Fiber *_fb) {\n", fname);
  buf_puts(pb, "    SP_GC_SAVE();\n");

  /* Save global emission state */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  g_pre = NULL; g_indent = 1; g_nren = 0; g_block_id = blk;
  g_block_param_name = bp0; g_self = sv_self;
  g_ret_type = TY_POLY; g_result_poly = 0; g_result_var = NULL;

  /* Unpack capture struct */
  if (ncap > 0 || cap_self) {
    buf_printf(pb, "    _fib_cap_%d *_fc = (_fib_cap_%d *)_fb->user_data;\n", fid, fid);
    if (cap_self) {
      const char *svar = sv_self ? sv_self : "self";
      buf_printf(pb, "    sp_%s *%s = _fc->self_ptr;\n", cap_self_class, svar);
      buf_printf(pb, "    SP_GC_ROOT(%s);\n", svar);
    }
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      TyKind ct = lv ? lv->type : TY_POLY;
      const char *rn = rename_local(caps.v[i]);
      buf_printf(pb, "    "); emit_ctype(c, ct, pb);
      buf_printf(pb, " lv_%s = _fc->%s;\n", rn, caps.v[i]);
      if (fiber_cap_needs_root(ct)) {
        if (ct == TY_POLY) buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", rn);
        else buf_printf(pb, "    SP_GC_ROOT(lv_%s);\n", rn);
      }
    }
  }

  /* Block param: first resume value (or nil on initial resume) */
  if (bp0) {
    const char *bpn = rename_local(bp0);
    buf_printf(pb, "    sp_RbVal lv_%s = _fb->resumed_value;\n", bpn);
    buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", bpn);
  }

  /* Declare fiber-body locals (those written in the body, not captured) */
  if (encl) {
    for (int i = 0; i < encl->nlocals; i++) {
      LocalVar *lv = &encl->locals[i];
      if (lv->is_param || lv->is_cell) continue;
      if (!lv->name) continue;
      if (bp0 && !strcmp(lv->name, bp0)) continue;
      if (nameset_has(&caps, lv->name)) continue;
      if (!nameset_has(&fib_locals, lv->name)) continue;
      if (lv->type == TY_UNKNOWN) continue;
      declare_local(c, pb, lv, 0);
    }
  }
  free(fib_locals.v);

  /* Emit body: all-but-last as side-effect statements, last sets yielded_value.
     For void/nil last statements emit as stmt first then set yielded=nil. */
  if (body >= 0) {
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
    if (bn > 0) {
      int last = bb[bn - 1];
      TyKind lty = comp_ntype(c, last);
      if (lty == TY_VOID || lty == TY_UNKNOWN) {
        emit_stmt(c, last, pb, 1);
        buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
      }
      else if (lty == TY_NIL) {
        emit_stmt(c, last, pb, 1);
        buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
      }
      else {
        Buf pre2 = {0}, vb = {0};
        Buf *sv2 = g_pre; int sv2i = g_indent;
        g_pre = &pre2; g_indent = 1;
        emit_expr(c, last, &vb);
        g_pre = sv2; g_indent = sv2i;
        if (pre2.p) buf_puts(pb, pre2.p);
        buf_printf(pb, "    _fb->yielded_value = ");
        if (lty == TY_POLY) {
          buf_puts(pb, vb.p ? vb.p : "sp_box_nil()");
        }
        else {
          emit_box_open(c, lty, pb); buf_puts(pb, vb.p ? vb.p : ""); emit_box_close(c, lty, pb);
        }
        buf_puts(pb, ";\n");
        free(pre2.p); free(vb.p);
      }
    }
    else {
      buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
    }
  }
  else {
    buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
  }

  buf_puts(pb, "}\n");

  /* Restore emission state */
  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_ret_type = sv_rt;
  g_result_poly = sv_rp; g_result_var = sv_rv;

  /* Emit creation expression:
     If there are captures, allocate a GC-managed capture struct, fill it,
     assign to fiber->user_data, then return the fiber.
     Without captures: just sp_Fiber_new(fname). */
  if (ncap > 0 || cap_self) {
    int tf = ++g_tmp, tc = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_Fiber *_t%d = sp_Fiber_new(%s);\n", tf, fname);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tf);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_fib_cap_%d *_t%d = (_fib_cap_%d *)sp_gc_alloc(sizeof(_fib_cap_%d), NULL, _fib_cap_scan_%d);\n",
               fid, tc, fid, fid, fid);
    if (cap_self) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d->self_ptr = %s;\n", tc, sv_self ? sv_self : "self");
    }
    for (int i = 0; i < ncap; i++) {
      const char *rn = rename_local(caps.v[i]);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d->%s = lv_%s;\n", tc, caps.v[i], rn);
    }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_t%d->user_data = _t%d;\n", tf, tc);
    buf_printf(b, "_t%d", tf);
  }
  else {
    buf_printf(b, "sp_Fiber_new(%s)", fname);
  }
  free(caps.v);
}

/* Does a proc body reference `self` -- explicitly, via an ivar, via `super`, or
   via a receiverless call that dispatches on an instance method of class_id?
   Such a block, when it escapes inlining into a real _proc_N(void*, ...), emits
   `self` with no parameter or capture for it (#1436). Recurses into nested
   blocks too: a nested block's self is forwarded from this proc's, so this proc
   must capture it. Over-approximation is harmless -- the readback is followed by
   `(void)self;`. */
static int proc_body_uses_self(Compiler *c, int id, int class_id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "SelfNode")) return 1;
  if (!strncmp(ty, "InstanceVariable", 16)) return 1;
  if (!strcmp(ty, "SuperNode") || !strcmp(ty, "ForwardingSuperNode")) return 1;
  if (!strcmp(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) {
    const char *nm = nt_str(c->nt, id, "name");
    if (nm && comp_method_in_chain(c, class_id, nm, NULL) >= 0) return 1;
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++)
    if (proc_body_uses_self(c, nt_ref_at(c->nt, id, i), class_id)) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++) if (proc_body_uses_self(c, ids[k], class_id)) return 1; }
  return 0;
}

/* Does node `id` (a method body subtree) store the &block param `bp` into an
   instance variable -- `@x = blk`? Such a block is type-erased into a generic
   sp_Proc* ivar; a later `@x.call` reads the boxed _sp_proc_poly_ret, so the
   block must use the poly return ABI. (A block merely captured into a local
   proc keeps its concrete return type tracked, so it is not forced.) */
static int block_stored_in_ivar(Compiler *c, int id, const char *bp) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (!strcmp(ty, "InstanceVariableWriteNode") || !strcmp(ty, "InstanceVariableOrWriteNode")) {
    int v = nt_ref(c->nt, id, "value");
    if (v >= 0) {
      const char *vt = nt_type(c->nt, v);
      if (vt && !strcmp(vt, "LocalVariableReadNode")) {
        const char *vn = nt_str(c->nt, v, "name");
        if (vn && !strcmp(vn, bp)) return 1;
      }
    }
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) if (block_stored_in_ivar(c, nt_ref_at(c->nt, id, i), bp)) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++) if (block_stored_in_ivar(c, ids[k], bp)) return 1; }
  return 0;
}

/* Lower `Ractor.new { body }` into a `static void _ractor_body_N(sp_Ractor *)`
   and sp_Ractor_new. Unlike a Fiber, a Ractor runs on its own pthread with a
   private heap, so it cannot share capture cells with the creator: Milestone 1
   enforces the isolation rule by rejecting any captured outer local or `self`
   (a later milestone copies shareable captures by value). The body talks to
   the outside world only through Ractor.receive / Ractor.yield, so there is no
   resume/yield slot to thread; the block's implicit return value is delivered
   to the first r.take as a final implicit yield (#1453). The body wraps itself in a top-level
   rescue landing pad so an uncaught exception terminates just this Ractor
   instead of exit(1)-ing the whole process. */
/* Recursively scan a Ractor body (including nested blocks) for cross-Ractor
   shared state: a class variable (@@), or a user-declared global ($). CRuby
   raises Ractor::IsolationError on such access from a non-main Ractor; we
   reject at compile time. Special globals ($~, $stdout, regex/status, ...) are
   thread-local or read-only and not user-declared, so they pass. Returns
   1 = class variable, 2 = user global, 0 = none. */
static int ractor_body_shared_state(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (ty) {
    if (!strncmp(ty, "ClassVariable", 13)) return 1;
    if (!strncmp(ty, "GlobalVariable", 14)) {
      const char *nm = nt_str(c->nt, id, "name");
      if (nm && nm[0] == '$') {
        const char *rn = comp_resolve_gvar(c, nm + 1);
        if (rn && comp_gvar(c, rn)) return 2;
      }
    }
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) { int v = ractor_body_shared_state(c, ch); if (v) return v; } }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) { int v = ractor_body_shared_state(c, ids[k]); if (v) return v; } }
  return 0;
}

void emit_ractor_new(Compiler *c, int id, Buf *b) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) { buf_puts(b, "sp_Ractor_new(NULL)"); return; }

  int rid = ++g_ractor_counter;
  char fname[48];
  snprintf(fname, sizeof fname, "_ractor_body_%d", rid);

  int body = nt_ref(nt, blk, "body");
  Scope *encl = comp_scope_of(c, id);

  /* Block parameters bind the Ractor.new spawn arguments positionally:
     Ractor.new(a, b) { |x, y| ... } deep-copies a->x, b->y into the new
     Ractor's heap. Collect the param names and the call args. */
  #define SP_RACTOR_MAX_BP 16
  const char *bparams[SP_RACTOR_MAX_BP]; int nbp = 0;
  {
    int bp_node = nt_ref(nt, blk, "parameters");
    if (bp_node >= 0) {
      int inner = nt_ref(nt, bp_node, "parameters");
      int pn = inner >= 0 ? inner : bp_node;
      int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
      if (rn > SP_RACTOR_MAX_BP)
        unsupported(c, id, "Ractor.new with more than 16 block parameters");
      for (int i = 0; i < rn && nbp < SP_RACTOR_MAX_BP; i++) {
        const char *p = nt_str(nt, reqs[i], "name");
        if (p) bparams[nbp++] = p;
      }
    }
  }
  int args_node = nt_ref(nt, id, "arguments");
  int argc = 0; const int *argv = args_node >= 0 ? nt_arr(nt, args_node, "arguments", &argc) : NULL;

  /* Isolation rule: a Ractor block must be self-contained -- it may not access
     outer local variables (CRuby raises ArgumentError; we reject at compile
     time, matching CRuby's "can not isolate a Proc because it accesses outer
     variables (a, b)" by naming every offender at once). Block params are not
     captures (they receive the deep-copied spawn args), so skip them. Shareable
     CONSTANTS remain accessible (shared read-only), like CRuby. */
  NameSet rac_locals = {0};
  if (body >= 0) proc_collect_locals(c, body, &rac_locals);
  NameSet rac_used = {0};
  if (body >= 0) proc_collect_used(c, body, &rac_used);
  if (encl) {
    char names[512]; names[0] = 0; size_t nlen = 0; int nnames = 0;
    for (int u = 0; u < rac_used.n; u++) {
      const char *nm = rac_used.v[u];
      if (nameset_has(&rac_locals, nm)) continue;
      int is_bp = 0; for (int i = 0; i < nbp; i++) if (!strcmp(nm, bparams[i])) { is_bp = 1; break; }
      if (is_bp) continue;
      LocalVar *lv = scope_local(encl, nm);
      if (!lv || lv->type == TY_UNKNOWN) continue;
      int n = snprintf(names + nlen, sizeof names - nlen, "%s%s", nnames ? ", " : "", nm);
      if (n > 0 && (size_t)n < sizeof names - nlen) nlen += (size_t)n;
      nnames++;
    }
    if (nnames) {
      char msg[700];
      snprintf(msg, sizeof msg,
               "Ractor::IsolationError: block accesses outer %s (%s) "
               "(a Ractor block must be self-contained; pass each value as a "
               "Ractor.new argument -- Ractor.new(%s) { |...| } -- or send it instead)",
               nnames == 1 ? "variable" : "variables", names, names);
      unsupported(c, id, msg);
    }
  }
  free(rac_used.v);
  if (encl && encl->class_id >= 0 && !encl->is_cmethod && body >= 0 && fiber_body_uses_self(c, body))
    unsupported(c, id, "Ractor::IsolationError: block accesses self "
                       "(a Ractor block must be self-contained)");
  if (body >= 0) {
    int shared = ractor_body_shared_state(c, body);
    if (shared == 1)
      unsupported(c, id, "Ractor::IsolationError: block accesses a class variable (@@) "
                         "(class variables are not shared across Ractors; pass data as an argument or message)");
    else if (shared == 2)
      unsupported(c, id, "Ractor::IsolationError: block accesses a global variable ($) "
                         "(globals are not shared across Ractors; pass data as an argument or message)");
  }

  buf_printf(&g_proc_protos, "static void %s(sp_Ractor *_rb);\n", fname);

  Buf *pb = &g_procs;
  buf_printf(pb, "static void %s(sp_Ractor *_rb) {\n", fname);
  buf_puts(pb, "    (void)_rb;\n");
  buf_puts(pb, "    SP_GC_SAVE();\n");

  /* Save global emission state (mirror emit_fiber_new). */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  g_pre = NULL; g_indent = 1; g_nren = 0; g_block_id = blk;
  g_block_param_name = NULL; g_self = sv_self;
  g_ret_type = TY_POLY; g_result_poly = 0; g_result_var = NULL;

  /* Declare body locals. */
  if (encl) {
    for (int i = 0; i < encl->nlocals; i++) {
      LocalVar *lv = &encl->locals[i];
      if (lv->is_param || lv->is_cell) continue;
      if (!lv->name) continue;
      if (!nameset_has(&rac_locals, lv->name)) continue;
      if (lv->type == TY_UNKNOWN) continue;
      int is_bp = 0; for (int i = 0; i < nbp; i++) if (!strcmp(lv->name, bparams[i])) { is_bp = 1; break; }
      if (is_bp) continue;   /* block params are declared from spawn args below */
      declare_local(c, pb, lv, 0);
    }
  }
  free(rac_locals.v);

  /* Block params: bind each to its spawn argument. A Ractor::Port arg crossed
     by reference (typed TY_RACTOR_PORT by the analyzer), so unbox it to the
     port pointer; everything else is a deep-copied poly value. */
  {
    Scope *bs = comp_scope_of(c, blk);
    for (int i = 0; i < nbp; i++) {
      const char *bpn = rename_local(bparams[i]);
      LocalVar *lvp = bs ? scope_local(bs, bparams[i]) : NULL;
      if (lvp && lvp->type == TY_RACTOR_PORT) {
        buf_printf(pb, "    sp_RactorPort *lv_%s = (sp_RactorPort *)sp_ractor_spawn_arg(%d).v.p;\n", bpn, i);
      } else {
        buf_printf(pb, "    sp_RbVal lv_%s = sp_ractor_spawn_arg(%d);\n", bpn, i);
        buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", bpn);
      }
    }
  }

  /* Top-level rescue landing pad: an uncaught raise longjmps here and the
     Ractor simply terminates (its taker then sees the closed outbox). */
  buf_puts(pb, "    sp_exc_top++;\n");
  buf_puts(pb, "    if (setjmp(sp_exc_stack[sp_exc_top-1]) == 0) {\n");
  /* Run the body. Its tail value is delivered to the first r.take, matching
     CRuby: the block's return value becomes a final implicit yield (pushed to
     the outbox after any explicit Ractor.yield, so FIFO order is preserved).
     Only on the success path -- an uncaught raise longjmps to the else arm and
     delivers nothing (the taker then sees the closed outbox). The fiber-body
     tail handling (emit_fiber_new) is the template. */
  if (body >= 0) {
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
    if (bn > 0) {
      int last = bb[bn - 1];
      TyKind lty = comp_ntype(c, last);
      if (lty == TY_VOID || lty == TY_UNKNOWN || lty == TY_NIL) {
        emit_stmt(c, last, pb, 1);
        buf_puts(pb, "        sp_Ractor_yield(sp_box_nil());\n");
      } else {
        Buf pre2 = {0}, vb = {0};
        Buf *sv2 = g_pre; int sv2i = g_indent;
        g_pre = &pre2; g_indent = 2;
        if (lty == TY_POLY) emit_expr(c, last, &vb);
        else emit_boxed(c, last, &vb);
        g_pre = sv2; g_indent = sv2i;
        if (pre2.p) buf_puts(pb, pre2.p);
        buf_printf(pb, "        sp_Ractor_yield(%s);\n", vb.p ? vb.p : "sp_box_nil()");
        free(pre2.p); free(vb.p);
      }
    } else {
      buf_puts(pb, "        sp_Ractor_yield(sp_box_nil());\n");
    }
  } else {
    buf_puts(pb, "        sp_Ractor_yield(sp_box_nil());\n");
  }
  buf_puts(pb, "        sp_exc_top--;\n");
  buf_puts(pb, "    } else {\n");
  buf_puts(pb, "        sp_exc_top--;\n");
  buf_puts(pb, "    }\n");
  buf_puts(pb, "}\n");

  /* Restore emission state. */
  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_ret_type = sv_rt;
  g_result_poly = sv_rp; g_result_var = sv_rv;

  /* Creation: with spawn args, serialize each (on this creator thread, reading
     this heap) into a malloc'd blob array, then hand ownership to the Ractor.
     Hoist the array build into g_pre so arg expressions can pre-emit cleanly. */
  if (argc > 0 && g_pre) {
    int ta = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RactorBlob *_t%d = (sp_RactorBlob *)malloc(sizeof(sp_RactorBlob) * %d);\n", ta, argc);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (!_t%d) sp_oom_die();\n", ta);
    for (int i = 0; i < argc; i++) {
      Buf vb = {0};
      if (comp_ntype(c, argv[i]) == TY_POLY) emit_expr(c, argv[i], &vb);
      else emit_boxed(c, argv[i], &vb);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d[%d] = sp_ractor_serialize_hook(%s);\n", ta, i, vb.p ? vb.p : "sp_box_nil()");
      free(vb.p);
    }
    buf_printf(b, "sp_Ractor_new_args(%s, _t%d, %d)", fname, ta, argc);
  } else {
    buf_printf(b, "sp_Ractor_new(%s)", fname);
  }
}

/* Lower a `proc {}` / `lambda {}` / `Proc.new {}` / `->(){}` literal: emit a
   standalone `static mrb_int _proc_N(void *cap, mrb_int argc, mrb_int *args)`
   (sp_proc_call's ABI) into g_procs, and emit the boxing `sp_proc_new_meta(...)`
   value into `b`. */
void emit_proc_literal(Compiler *c, int create, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, create);
  int is_lambda_node = cty && !strcmp(cty, "LambdaNode");
  int is_block_node = cty && !strcmp(cty, "BlockNode");
  if (!is_lambda_node && !is_block_node && nt_ref(nt, create, "block") < 0) { unsupported(c, create, "proc literal without a block"); return; }

  Scope *bs = comp_scope_of(c, create);  /* enclosing scope: holds params + locals */
  int body = proc_body_node(c, create);

  int arity = 0;
  while (proc_param_name(c, create, arity)) arity++;

  /* Classify the names used in the proc body: the proc's params, captured
     enclosing locals (marked is_cell by analyze), and the proc's own body
     locals. Captures populate the cap struct; body locals are declared inside
     the fn; params come from args[]. */
  NameSet params = {0}, used = {0}, locals = {0}, caps = {0};
  for (int k = 0; k < arity; k++) nameset_add(&params, proc_param_name(c, create, k));
  proc_collect_used(c, body, &used);
  proc_collect_locals(c, body, &locals);
  for (int u = 0; u < used.n; u++) {
    const char *nm = used.v[u];
    if (nameset_has(&params, nm)) continue;
    LocalVar *lv = scope_local(bs, nm);
    if (lv && lv->is_cell) {
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN &&
          lv->type != TY_PROC && !ptr_cell) {
        free(params.v); free(used.v); free(locals.v); free(caps.v);
        unsupported(c, create, "proc capturing a non-integer variable (later slice)");
        return;
      }
      nameset_add(&caps, nm);
    }
    else if (!nameset_has(&locals, nm)) {
      /* read of an enclosing var that wasn't celled and isn't proc-local:
         no storage exists for it inside the fn -- defer rather than miscompile */
      free(params.v); free(used.v); free(locals.v); free(caps.v);
      unsupported(c, create, "proc referencing an uncaptured outer variable (later slice)");
      return;
    }
  }
  /* Lowered self-recursive yield method: a `{ yield }` block forwards the
     enclosing method's __yblk__ down via capture.  The YieldNode is not a
     LocalVariableRead so proc_collect_used never picks it up -- force it. */
  if (g_current_scope_is_lowered) {
    int pb2 = proc_body_node(c, create);
    if (pb2 >= 0 && proc_body_has_yield(c, pb2) && !nameset_has(&caps, "__yblk__")) {
      LocalVar *yblk_lv = scope_local(bs, "__yblk__");
      if (yblk_lv && yblk_lv->is_cell) nameset_add(&caps, "__yblk__");
    }
  }

  /* proc {} / Proc.new {} are procs; lambda {} and ->(){} are lambdas */
  const char *cn = nt_str(nt, create, "name");
  int is_lambda = is_lambda_node || (cn && !strcmp(cn, "lambda"));

  /* body return type = last statement's type */
  TyKind ret = TY_NIL;
  { int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn > 0) ret = comp_ntype(c, bb[bn - 1]); }
  /* A block passed as a method's &block argument must return the value type the
     method expects across all its call sites (its blk_ret): if that unified type
     is poly, return poly here so the sp_proc_call ABI is consistent. */
  if (ret != TY_POLY) {
    int owner = -1;
    for (int oid = 0; oid < nt->count; oid++) if (nt_ref(nt, oid, "block") == create) { owner = oid; break; }
    if (owner >= 0 && nt_type(nt, owner) && !strcmp(nt_type(nt, owner), "CallNode")) {
      const char *onm = nt_str(nt, owner, "name");
      int orecv = nt_ref(nt, owner, "receiver");
      int mi = -1;
      if (orecv < 0 && onm) {
        mi = comp_method_index(c, onm);
        if (mi < 0) { Scope *osc = comp_scope_of(c, owner); if (osc && osc->class_id >= 0) mi = comp_method_in_chain(c, osc->class_id, onm, NULL); }
      }
else if (orecv >= 0 && onm) {
        TyKind ort = comp_ntype(c, orecv);
        if (ty_is_object(ort)) mi = comp_method_in_chain(c, ty_object_class(ort), onm, NULL);
      }
      /* Force the poly ABI when the owner method stores its &block into an ivar
         (`@x = blk`): the block becomes a type-erased sp_Proc* called later via
         a generic `@x.call` that reads the boxed _sp_proc_poly_ret. A proc
         returning its scalar directly would be read as that stale poly slot
         (returning nil). */
      int escapes = mi >= 0 && c->scopes[mi].blk_param && c->scopes[mi].blk_param[0] &&
                    !c->scopes[mi].yields &&
                    block_stored_in_ivar(c, c->scopes[mi].body, c->scopes[mi].blk_param);
      if (mi >= 0 && ((TyKind)c->scopes[mi].blk_ret == TY_POLY || escapes)) ret = TY_POLY;
    }
  }
  /* The proc fn returns mrb_int (the ABI); heap-pointer values (strings,
     arrays, hashes, objects) are laundered through (mrb_int)(uintptr_t).
     TY_POLY and float values are stored in _sp_proc_poly_ret (file-static
     sp_RbVal) before return -- float boxed via sp_box_float -- and the call
     site reads it back (unboxing float with sp_poly_to_f).
     Range/time don't fit the slot and defer. */
  int ret_ptr = proc_slot_is_ptr(ret);
  /* No usable value: run the body for effect and return nil (0). TY_NIL as
     well as TY_VOID -- a method whose tail has no value (e.g. `puts`, or a
     bare `yield if block`) is inferred TY_NIL but emitted as a C `void`
     function (method_is_void() keys on !is_scalar_ret, which excludes
     TY_NIL). Returning its result from the mrb_int proc trampoline would
     emit `return <void-call>;` and fail to compile; a TY_NIL proc returns
     nil regardless of the tail expression's value, so emit it as a
     statement and fall through to `return 0`. */
  int ret_no_value = (ret == TY_VOID || ret == TY_NIL);
  int ret_poly = (ret == TY_POLY);
  int ret_fbox = (ret == TY_FLOAT);  /* boxed through the poly return slot */
  if (!proc_slot_is_direct(ret) && !ret_ptr && !ret_no_value && !ret_poly && !ret_fbox) {
    free(params.v); free(used.v); free(locals.v); free(caps.v);
    unsupported(c, create, "proc with range/time return (later slice)");
    return;
  }

  int pid = ++g_proc_counter;
  int ncap = caps.n;
  /* A block that references self and escapes into a real proc must capture self
     through _cap (#1436). Only for a pointer (heap-object) instance self -- a
     class-method self or a by-value self is a different shape, left as-is. */
  int cap_self = bs && bs->class_id >= 0 && !bs->is_cmethod &&
                 !c->classes[bs->class_id].is_value_type &&
                 proc_body_uses_self(c, body, bs->class_id);
  const char *self_cls = cap_self ? c->classes[bs->class_id].name : NULL;

  /* parameter metadata for Proc#parameters: kinds (:req for lambdas, :opt for
     procs) + names, as interned symbol ids (pre-interned in analyze). */
  char meta_args[64];
  if (arity > 0) {
    buf_printf(&g_procs, "static const sp_sym _proc_kinds_%d[] = {", pid);
    for (int k = 0; k < arity; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", comp_sym_intern(c, is_lambda ? "req" : "opt"));
    buf_puts(&g_procs, "};\n");
    buf_printf(&g_procs, "static const sp_sym _proc_names_%d[] = {", pid);
    for (int k = 0; k < arity; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", comp_sym_intern(c, proc_param_name(c, create, k)));
    buf_puts(&g_procs, "};\n");
    snprintf(meta_args, sizeof meta_args, "_proc_kinds_%d, _proc_names_%d", pid, pid);
  }
  else snprintf(meta_args, sizeof meta_args, "NULL, NULL");

  /* capture struct + GC scan (only when the proc captures). cap_scan marks
     the cap struct itself first (sp_Proc_scan does not), then each cell --
     matching the sp_hashproc convention; marking only the cells would leave
     the cap struct unreachable and free it out from under the proc. */
  if (ncap > 0 || cap_self) {
    buf_printf(&g_procs, "typedef struct {");
    for (int i = 0; i < ncap; i++) buf_printf(&g_procs, " mrb_int *%s;", caps.v[i]);
    if (cap_self) buf_puts(&g_procs, " void *__self;");
    buf_printf(&g_procs, " } _proc_cap_%d;\n", pid);
    buf_printf(&g_procs, "static void _proc_cap_scan_%d(void *p) {\n", pid);
    buf_printf(&g_procs, "  sp_gc_mark(p);\n");
    buf_printf(&g_procs, "  _proc_cap_%d *_c = (_proc_cap_%d *)p;\n", pid, pid);
    for (int i = 0; i < ncap; i++) buf_printf(&g_procs, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
    if (cap_self) buf_puts(&g_procs, "  if (_c->__self) sp_gc_mark(_c->__self);\n");
    buf_puts(&g_procs, "}\n");
  }

  buf_printf(&g_proc_protos, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args);\n", pid);

  /* Save every emission global: the proc body is a fresh function context. */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  const char *sv_cap_struct = g_cap_struct; NameSet *sv_cap_names = g_cap_names;
  int sv_ensure_depth = g_ensure_depth;
  g_pre = NULL; g_indent = 0; g_nren = 0; g_block_id = -1; g_block_param_name = NULL;
  g_self = "self"; g_result_var = NULL; g_ret_type = ret; g_ensure_depth = 0; g_result_poly = 0;
  char cap_struct_name[32] = "";
  if (ncap > 0) { snprintf(cap_struct_name, sizeof cap_struct_name, "_proc_cap_%d", pid); g_cap_struct = cap_struct_name; g_cap_names = &caps; }
  else { g_cap_struct = NULL; g_cap_names = NULL; }

  Buf *pb = &g_procs;
  buf_printf(pb, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  if (ncap == 0 && !cap_self) buf_puts(pb, "    (void)_cap;\n");
  buf_puts(pb, "    (void)args;\n");
  buf_puts(pb, "    (void)argc;\n");
  /* Captured instance self, read back from _cap (#1436). (void) guards the
     over-approximating use-of-self detection. */
  if (cap_self) {
    buf_printf(pb, "    sp_%s *self = (sp_%s *)((_proc_cap_%d *)_cap)->__self;\n", self_cls, self_cls, pid);
    buf_puts(pb, "    (void)self;\n");
  }
  /* Lambda: enforce strict arity (required params only -- no optionals/rest yet). */
  if (is_lambda) buf_printf(pb, "    sp_proc_lambda_arity_check(argc, %d, 0, FALSE);\n", arity);
  for (int k = 0; k < arity; k++) {
    const char *p = proc_param_name(c, create, k);
    LocalVar *lv = scope_local(bs, p);
    TyKind pt = lv ? lv->type : TY_INT;
    buf_puts(pb, "    "); emit_ctype(c, pt, pb); buf_printf(pb, " lv_%s = ", p);
    /* a heap-pointer param is laundered back from the mrb_int slot */
    if (proc_slot_is_ptr(pt)) { buf_puts(pb, "("); emit_ctype(c, pt, pb); buf_printf(pb, ")(uintptr_t)args[%d];\n", k); }
    else buf_printf(pb, "args[%d];\n", k);
  }
  for (int i = 0; i < locals.n; i++) {
    LocalVar *lv = scope_local(bs, locals.v[i]);
    /* a celled local is a captured var (accessed via _cap), not a fn-local */
    /* skip virtual &block slots (TY_UNKNOWN) but allow rescue-bind vars (TY_EXCEPTION) */
    if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) declare_local(c, pb, lv, 0);
  }
  if (ret_ptr) {
    /* launder a heap-pointer return through the mrb_int slot: emit the body's
       leading statements, then a prelude-wrapped `return (mrb_int)(uintptr_t)(<value>)`.
       The last expression may itself need a prelude (e.g. array allocation), so wrap
       emit_expr in a temporary prelude buffer that drains before the return line. */
    int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
    if (bn > 0) {
      Buf rpre = {0}, rval = {0};
      Buf *sv_rpre = g_pre; int sv_rind = g_indent;
      g_pre = &rpre; g_indent = 1;
      emit_expr(c, bb[bn - 1], &rval);
      g_pre = sv_rpre; g_indent = sv_rind;
      if (rpre.p) buf_puts(pb, rpre.p);
      buf_puts(pb, "  return (mrb_int)(uintptr_t)(");
      if (rval.p) buf_puts(pb, rval.p);
      buf_puts(pb, ");\n");
      free(rpre.p); free(rval.p);
    }
    else buf_puts(pb, "  return 0;\n");
  }
  else if (ret_poly || ret_fbox) {
    /* Store the result in the file-static _sp_proc_poly_ret slot (boxed:
       a float tail becomes sp_box_float via g_result_poly); the call site
       reads it back after sp_proc_call returns. */
    if (!g_needs_proc_poly_retslot) {
      g_needs_proc_poly_retslot = 1;
      buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_ret;\n");
    }
    g_result_var = "_sp_proc_poly_ret"; g_result_poly = 1;
    emit_stmts_tail(c, body, pb, 1);
    g_result_var = NULL; g_result_poly = 0;
    buf_puts(pb, "  return 0;\n");
  }
  else if (ret_no_value) {
    /* no usable value (TY_VOID or TY_NIL): run the body as plain statements,
       return nil (0) */
    emit_stmts(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  else {
    emit_stmts_tail(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  buf_puts(pb, "}\n");

  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_result_var = sv_rv; g_ret_type = sv_rt;
  g_cap_struct = sv_cap_struct; g_cap_names = sv_cap_names; g_ensure_depth = sv_ensure_depth;
  g_result_poly = sv_rp;

  if (ncap == 0 && !cap_self) {
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, NULL, NULL, %d, %s, %d, %s)",
               pid, arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
  }
  else {
    /* Allocate + populate the cap struct in the enclosing statement's prelude
       (it shares the enclosing cells by pointer), then box the proc. */
    if (g_pre) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_proc_cap_%d *_capv_%d = (_proc_cap_%d *)sp_gc_alloc(sizeof(_proc_cap_%d), NULL, _proc_cap_scan_%d);\n", pid, pid, pid, pid, pid);
      /* Root the capture struct: sp_proc_new_meta allocates the proc box and
         can fire a GC that would otherwise sweep this still-unreferenced
         struct before the box adopts it. */
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_capv_%d);\n", pid);
      for (int i = 0; i < ncap; i++) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->%s = _cell_%s;\n", pid, caps.v[i], caps.v[i]); }
      /* Capture the enclosing instance self by pointer (#1436). */
      if (cap_self) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->__self = (void *)%s;\n", pid, sv_self ? sv_self : "self"); }
    }
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, _capv_%d, _proc_cap_scan_%d, %d, %s, %d, %s)",
               pid, pid, pid, arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
  }

  free(params.v); free(used.v); free(locals.v); free(caps.v);
}

/* Emit the struct + the constructor (sp_<Class>_new) for one class. */
/* Returns 1 if the class name shadows a built-in runtime type (no struct/new to emit). */
int is_builtin_reopen(const char *name) {
  return !strcmp(name, "Toplevel") ||
         !strcmp(name, "String")    || !strcmp(name, "Integer") ||
         !strcmp(name, "Float")     || !strcmp(name, "Symbol")  ||
         !strcmp(name, "TrueClass") || !strcmp(name, "FalseClass") ||
         !strcmp(name, "NilClass")  || !strcmp(name, "Array")   ||
         !strcmp(name, "Object")    || !strcmp(name, "Numeric");
}

/* Returns 1 if n is a known built-in exception class name. */
int is_exc_name(const char *n) {
  if (!n) return 0;
  static const char *const EX[] = {
    "Exception", "StandardError", "RuntimeError", "ArgumentError",
    "TypeError", "NameError", "NoMethodError", "IndexError",
    "KeyError", "RangeError", "IOError", "EOFError",
    "ZeroDivisionError", "NotImplementedError", "StopIteration",
    "FloatDomainError", "Math_DomainError", "FrozenError", "EncodingError",
    "LoadError", "RegexpError", "StringScanner_Error", "FiberError", NULL
  };
  for (int i = 0; EX[i]; i++) if (!strcmp(n, EX[i])) return 1;
  return 0;
}

/* Returns 1 if user class ci (or any ancestor) directly inherits a builtin exception. */
int class_is_exc_subclass(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    const char *sn = nt_str(c->nt, sc, "name");
    if (sty && (!strcmp(sty, "ConstantReadNode") || !strcmp(sty, "ConstantPathNode")) &&
        is_exc_name(sn))
      return 1;
  }
  return 0;
}

/* Build the full Ruby-style qualified name ("ActiveRecord::RecordNotFound") for
   class index ci by walking enclosing_class up to the top level. */
const char *class_ruby_name(Compiler *c, int ci) {
  if (ci < 0 || ci >= c->nclasses) return NULL;
  /* collect ancestry: max 16 levels deep */
  int chain[16]; int depth = 0;
  for (int k = ci; k >= 0 && depth < 16; ) {
    chain[depth++] = k;
    k = c->classes[k].enclosing_class;
  }
  if (depth == 1) return c->classes[ci].name; /* top-level: no qualification needed */
  /* build "A::B::C" from outermost to innermost */
  static char buf[256];
  buf[0] = '\0';
  for (int i = depth - 1; i >= 0; i--) {
    const char *seg = c->classes[chain[i]].name;
    if (!seg) continue;
    if (buf[0]) strncat(buf, "::", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, seg, sizeof(buf) - strlen(buf) - 1);
  }
  return buf;
}

/* Return the builtin exception parent name for user exc subclass ci,
   walking up the chain until a builtin exception name is found. */
const char *exc_builtin_parent(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    const char *sn = nt_str(c->nt, sc, "name");
    if (sty && (!strcmp(sty, "ConstantReadNode") || !strcmp(sty, "ConstantPathNode")) && is_exc_name(sn))
      return sn;
  }
  return "StandardError";
}

void emit_class_struct(Compiler *c, ClassInfo *ci, Buf *b) {
  /* Exception subclasses share sp_Exception as their underlying type. */
  int cid = comp_class_index(c, ci->name);
  if (cid >= 0 && class_is_exc_subclass(c, cid)) {
    /* An ivar-less exception subclass is forward-declared as
       `typedef sp_Exception` and needs no struct of its own. One with
       ivars gets a dedicated struct whose leading members mirror
       sp_Exception (cls_name/parent_cls_name/msg) -- a common initial
       sequence -- so every `(sp_Exception *)` cast in the raise/rescue
       and message machinery stays valid, with the ivar fields after (#1415). */
    if (ci->nivars == 0) return;
    buf_printf(b, "struct sp_%s_s {\n", ci->name);
    buf_puts(b, "  const char *cls_name;\n");
    buf_puts(b, "  const char *parent_cls_name;\n");
    buf_puts(b, "  const char *msg;\n");
    for (int i = 0; i < ci->nivars; i++) {
      TyKind t = ci->ivar_types[i];
      buf_puts(b, "  ");
      emit_ctype(c, t == TY_UNKNOWN ? TY_INT : t, b);
      buf_printf(b, " iv_%s;\n", ci->ivars[i] + 1);
    }
    buf_puts(b, "};\n");
    return;
  }
  /* the typedef is forward-declared for every class first (see codegen_program)
     so a class can embed a pointer to a class defined later in the file */
  buf_printf(b, "struct sp_%s_s {\n", ci->name);
  buf_puts(b, "  mrb_int cls_id;\n");  /* runtime class tag for virtual dispatch */
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    if (!is_scalar_ret(t) && t != TY_UNKNOWN) { /* ok */ }
    buf_puts(b, "  ");
    emit_ctype(c, t == TY_UNKNOWN ? TY_INT : t, b);
    /* ivar name includes '@'; strip it for the field */
    buf_printf(b, " iv_%s;\n", ci->ivars[i] + 1);
  }
  buf_puts(b, "};\n");
}

/* A class needs a GC scan iff any ivar holds a heap reference. */
int class_needs_scan(ClassInfo *ci) {
  for (int i = 0; i < ci->nivars; i++) {
    if (needs_root(ci->ivar_types[i])) return 1;
  }
  return 0;
}

/* Emit the GC scan function (marks heap ivars) for a class that needs one.
   Covers the same type set as needs_root: a heap reference reachable only
   through an unscanned ivar would be swept out from under the object
   (poly ivars holding tree children were the canonical case). */
void emit_class_scan(Compiler *c, ClassInfo *ci, Buf *b) {
  int cid = comp_class_index(c, ci->name);
  int is_exc_iv = cid >= 0 && ci->nivars > 0 && class_is_exc_subclass(c, cid);
  /* An ivar-bearing exception subclass always needs a scan: even with no
     heap ivar, its `msg` (a managed string in the dedicated struct) must
     be marked or it is swept while the exception is in flight. */
  if (!class_needs_scan(ci) && !is_exc_iv) return;
  buf_printf(b, "static void sp_%s_scan(void *p) {\n", ci->name);
  buf_printf(b, "  sp_%s *o = (sp_%s *)p;\n", ci->name, ci->name);
  if (is_exc_iv) buf_puts(b, "  sp_mark_string(o->msg);\n");
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    const char *iv = ci->ivars[i] + 1;
    if (t == TY_STRING) buf_printf(b, "  sp_mark_string(o->iv_%s);\n", iv);
    else if (t == TY_POLY) buf_printf(b, "  sp_mark_rbval(o->iv_%s);\n", iv);
    else if (needs_root(t))
      buf_printf(b, "  if (o->iv_%s) sp_gc_mark((void *)o->iv_%s);\n", iv, iv);
  }
  buf_puts(b, "}\n");
}

/* Per-class user-object message codec (Ractor deep copy). Emits the four hooks
   the runtime codec (sp_runtime.h) drives: nivars / getivar (box) / alloc /
   setivar (unbox), as switches over the class table. cls_id is a stable index
   shared by every Ractor, so it travels on the wire directly. Skips reopened
   builtins (no sp_ struct) and exception subclasses (special layout) -- those
   report nivars -1 and raise Ractor::Error if sent. Must be emitted after the
   class structs + scan fns and before sp_re_init (which installs the hooks). */
/* An ivar is codec-safe iff it is one of the deep-copyable types the value
   codec handles end-to-end AND emit_boxed_text/emit_unbox_text round-trip:
   scalars, nested objects, arrays, and hashes. Everything else -- by-value
   structs (Class/Range/Time/...), and non-copyable references (Proc/Fiber/IO/
   Method/StringIO/Regexp/...) -- makes the whole class non-deep-copyable, so
   it reports nivars -1 and raises Ractor::Error if sent. (A POLY ivar is safe
   to attempt; if it holds an unshareable value at runtime the value codec
   raises then.) */
static int ractor_ivar_safe(Compiler *c, TyKind t) {
  (void)c;
  if (t == TY_UNKNOWN) t = TY_INT;
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_STRING:
    case TY_SYMBOL: case TY_POLY:
    case TY_INT_ARRAY: case TY_FLOAT_ARRAY: case TY_STR_ARRAY: case TY_POLY_ARRAY:
      return 1;
    default: break;
  }
  return ty_is_object(t) || ty_is_hash(t);
}
static int ractor_obj_serializable(Compiler *c, int i) {
  if (is_builtin_reopen(c->classes[i].name) || class_is_exc_subclass(c, i)) return 0;
  ClassInfo *ci = &c->classes[i];
  for (int j = 0; j < ci->nivars; j++)
    if (!ractor_ivar_safe(c, ci->ivar_types[j])) return 0;
  return 1;
}

void emit_ractor_obj_codec(Compiler *c, Buf *b) {
  #define SP_RACTOR_OBJ_SUPPORTED(i) ractor_obj_serializable(c, i)
  char expr[320];

  buf_puts(b, "static int sp_ractor_obj_nivars(int cls_id) {\n  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++)
    if (SP_RACTOR_OBJ_SUPPORTED(i))
      buf_printf(b, "    case %d: return %d;\n", i, c->classes[i].nivars);
  buf_puts(b, "    default: return -1;\n  }\n}\n");

  buf_puts(b, "static sp_RbVal sp_ractor_obj_getivar(void *p, int cls_id, int i) {\n  (void)p; switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!SP_RACTOR_OBJ_SUPPORTED(i)) continue;
    ClassInfo *ci = &c->classes[i];
    if (ci->nivars == 0) continue;
    buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)p; switch (i) {\n", i, ci->name, ci->name);
    for (int j = 0; j < ci->nivars; j++) {
      TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
      snprintf(expr, sizeof expr, "o->iv_%s", ci->ivars[j] + 1);
      buf_printf(b, "      case %d: return ", j);
      emit_boxed_text(c, t, expr, b);
      buf_puts(b, ";\n");
    }
    buf_puts(b, "      default: break;\n    } break; }\n");
  }
  buf_puts(b, "  }\n  return sp_box_nil();\n}\n");

  buf_puts(b, "static void *sp_ractor_obj_alloc(int cls_id) {\n  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!SP_RACTOR_OBJ_SUPPORTED(i)) continue;
    ClassInfo *ci = &c->classes[i];
    if (class_needs_scan(ci))
      buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s_scan); memset(o, 0, sizeof(*o)); o->cls_id = %d; return o; }\n",
                 i, ci->name, ci->name, ci->name, ci->name, i);
    else
      buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, NULL); memset(o, 0, sizeof(*o)); o->cls_id = %d; return o; }\n",
                 i, ci->name, ci->name, ci->name, i);
  }
  buf_puts(b, "    default: return NULL;\n  }\n}\n");

  buf_puts(b, "static void sp_ractor_obj_setivar(void *p, int cls_id, int i, sp_RbVal v) {\n  (void)p; (void)v; switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!SP_RACTOR_OBJ_SUPPORTED(i)) continue;
    ClassInfo *ci = &c->classes[i];
    if (ci->nivars == 0) continue;
    buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)p; switch (i) {\n", i, ci->name, ci->name);
    for (int j = 0; j < ci->nivars; j++) {
      TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
      buf_printf(b, "      case %d: o->iv_%s = ", j, ci->ivars[j] + 1);
      emit_unbox_text(c, t, "v", b);
      buf_puts(b, "; break;\n");
    }
    buf_puts(b, "      default: break;\n    } break; }\n");
  }
  buf_puts(b, "  }\n}\n");

  buf_puts(b, "static void sp_ractor_obj_codec_install(void) {\n");
  buf_puts(b, "  sp_ractor_obj_nivars_hook = sp_ractor_obj_nivars;\n");
  buf_puts(b, "  sp_ractor_obj_getivar_hook = sp_ractor_obj_getivar;\n");
  buf_puts(b, "  sp_ractor_obj_alloc_hook = sp_ractor_obj_alloc;\n");
  buf_puts(b, "  sp_ractor_obj_setivar_hook = sp_ractor_obj_setivar;\n");
  buf_puts(b, "}\n\n");
  #undef SP_RACTOR_OBJ_SUPPORTED
}

void emit_class_new(Compiler *c, ClassInfo *ci, Buf *b) {
  int cid = comp_class_index(c, ci->name);
  if (ci->is_struct) {
    /* Struct constructor: one parameter per member, set the backing ivars. */
    buf_printf(b, "SP_POOL_DEFINE(%s)\n", ci->name);
    buf_printf(b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
    for (int i = 0; i < ci->nivars; i++) {
      if (i) buf_puts(b, ", ");
      emit_ctype(c, ci->ivar_types[i], b);
      buf_printf(b, " a%d", i);
    }
    if (ci->nivars == 0) buf_puts(b, "void");
    buf_printf(b, ") {\n  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
              ci->name, ci->name,
              class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->name : "NULL",
              class_needs_scan(ci) ? "_scan" : "");
    buf_puts(b, "  memset(self, 0, sizeof(*self));\n");  /* recycled slots are not zeroed */
    buf_puts(b, "  SP_GC_ROOT(self);\n");
    buf_printf(b, "  self->cls_id = %d;\n", cid);
    for (int i = 0; i < ci->nivars; i++)
      buf_printf(b, "  self->iv_%s = a%d;\n", ci->ivars[i] + 1, i);  /* skip leading '@' */
    buf_puts(b, "  return self;\n}\n");
    return;
  }
  int initcls = cid;
  int init = comp_method_in_chain(c, cid, "initialize", &initcls);
  if (ci->is_value_type) {
    /* value-type: build on the stack and return by value (no heap / GC) */
    buf_printf(b, "static sp_%s sp_%s_new(", ci->name, ci->name);
    if (init >= 0 && c->scopes[init].nparams > 0) {
      Scope *s = &c->scopes[init];
      for (int i = 0; i < s->nparams; i++) {
        if (i) buf_puts(b, ", ");
        LocalVar *p = scope_local(s, s->pnames[i]);
        TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
        emit_ctype(c, pt, b);
        buf_printf(b, " lv_%s", s->pnames[i]);
      }
    }
    else buf_puts(b, "void");
    buf_printf(b, ") {\n  sp_%s self = {0};\n  self.cls_id = %d;\n", ci->name, cid);
    if (init >= 0 && c->scopes[init].reachable && !c->scopes[init].yields) {
      buf_printf(b, "  sp_%s_initialize(&self", c->classes[initcls].name);
      Scope *s = &c->scopes[init];
      for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
      buf_puts(b, ");\n");
    }
    buf_puts(b, "  return self;\n}\n");
    return;
  }
  /* per-class free-list pool: sp_gc_collect recycles unmarked instances onto
     the pool instead of free()ing them, and sp_X_new reuses them -- this
     removes the malloc/free churn of allocation-heavy workloads. Exception
     subclasses use sp_exc_new_sub storage, so they are not pooled. */
  if (!class_is_exc_subclass(c, cid)) buf_printf(b, "SP_POOL_DEFINE(%s)\n", ci->name);
  buf_printf(b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
  if (init >= 0 && c->scopes[init].nparams > 0) {
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) {
      if (i) buf_puts(b, ", ");
      LocalVar *p = scope_local(s, s->pnames[i]);
      TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
      emit_ctype(c, pt, b);
      buf_printf(b, " lv_%s", s->pnames[i]);
    }
  }
  else {
    buf_puts(b, "void");
  }
  /* Exception subclasses: use sp_exc_new_sub as underlying storage so that
     sp_raise/rescue machinery sees the right cls_name and parent. */
  if (class_is_exc_subclass(c, cid)) {
    const char *cn2 = class_ruby_name(c, cid); if (!cn2) cn2 = ci->name;
    const char *par = exc_builtin_parent(c, cid);
    if (ci->nivars == 0) {
      buf_printf(b, ") {\n  sp_%s *self = sp_exc_new_sub(\"%s\", \"%s\", (&(\"\\xff\")[1]));\n",
                 ci->name, cn2, par);
      buf_printf(b, "  SP_GC_ROOT(self);\n");
    }
    else {
      /* ivar-bearing exception subclass: allocate the dedicated struct
         (sp_exc_new_sub would only size the 3-field base). The leading
         members mirror sp_Exception so the raise/message machinery's casts
         work; the ivars live after and are set by initialize. */
      buf_printf(b, ") {\n  sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s_scan);\n",
                 ci->name, ci->name, ci->name, ci->name);
      buf_puts(b, "  memset(self, 0, sizeof(*self));\n");
      buf_printf(b, "  self->cls_name = \"%s\";\n", cn2);
      buf_printf(b, "  self->parent_cls_name = \"%s\";\n", par);
      buf_puts(b, "  self->msg = (&(\"\\xff\")[1]);\n");
      buf_printf(b, "  SP_GC_ROOT(self);\n");
      for (int i = 0; i < ci->nivars; i++)
        if (ci->ivar_types[i] == TY_POLY)
          buf_printf(b, "  self->iv_%s = sp_box_nil();\n", ci->ivars[i] + 1);
    }
  }
  else {
  buf_printf(b, ") {\n  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
            ci->name, ci->name,
            class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->name : "NULL",
            class_needs_scan(ci) ? "_scan" : "");
  buf_puts(b, "  memset(self, 0, sizeof(*self));\n");  /* recycled slots are not zeroed */
  buf_printf(b, "  SP_GC_ROOT(self);\n");
  buf_printf(b, "  self->cls_id = %d;\n", cid);
  /* calloc zero-inits fields; a poly (boxed) ivar's zero pattern is not nil,
     so set poly ivars to boxed-nil before initialize runs (read-only ivars
     stay nil; written ones are overwritten). */
  for (int i = 0; i < ci->nivars; i++)
    if (ci->ivar_types[i] == TY_POLY)
      buf_printf(b, "  self->iv_%s = sp_box_nil();\n", ci->ivars[i] + 1);
  } /* close else (non-exception subclass allocation) */
  if (init >= 0 && c->scopes[init].reachable && !c->scopes[init].yields) {
    buf_printf(b, "  sp_%s_initialize(", c->classes[initcls].name);
    if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].name);
    buf_puts(b, "self");
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    buf_puts(b, ");\n");
  }
  buf_puts(b, "  return self;\n}\n");
}

/* Inline super { block } when the parent method uses yield.
   Returns 1 if the expansion was emitted, 0 if it should fall through to a
   regular function call (parent doesn't yield, has early return, etc.). */
int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) return 0;
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  int mi = p >= 0 ? comp_method_in_chain(c, p, s->name, &defcls) : -1;
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  int block = nt_ref(c->nt, id, "block");
  if (block < 0) return 0;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;

  g_yield_block_fallback = saved_block;
  g_block_id = block;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  int din = indent + 1;

  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && !strcmp(lv->name, m->blk_param)) continue;
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type));
    if (lv->type == TY_POLY) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT_RBVAL(lv_%s);\n", rn); }
    else if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn); }
  }

  const char *ty = nt_type(c->nt, id);
  int is_forwarding = ty && !strcmp(ty, "ForwardingSuperNode");
  int args = nt_ref(c->nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    buf_printf(b, "lv__y%d_%s = ", tag, m->pnames[i]);
    int sv = g_nren; g_nren = saved_nren;
    if (is_forwarding) {
      if (i < s->nparams) buf_printf(b, "lv_%s", rename_local(s->pnames[i]));
      else { g_nren = sv; emit_arg_or_default(c, m, i, -1, b); sv = g_nren; }
    }
    else {
      emit_arg_or_default(c, m, i, i < argc ? argv[i] : -1, b);
    }
    g_nren = sv;
    buf_puts(b, ";\n");
  }

  if (as_expr) {
    TyKind rt = comp_ntype(c, id);
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value(rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    emit_stmts_tail(c, m->body, b, din);
    g_result_var = sv_rv; g_result_poly = sp;
    emit_indent(b, din); buf_printf(b, "_t%d;\n", rtag);
  }
  else emit_stmts(c, m->body, b, din);

  if (as_expr) { emit_indent(b, indent); buf_puts(b, "})"); }
  else { emit_indent(b, indent); buf_puts(b, "}\n"); }

  g_nren = saved_nren;
  g_block_id = saved_block;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  return 1;
}

/* super(args) / super -> call the parent's same-named method. */
void emit_super(Compiler *c, int id, Buf *b) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) { unsupported(c, id, "super (not in a method)"); return; }
  const char *ty = nt_type(c->nt, id);
  /* Prepend chain: super goes to the next shadow in the same class. */
  const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
  if (shadow) {
    buf_printf(b, "sp_%s_%s((sp_%s *)%s",
               c->classes[s->class_id].name, mc(shadow),
               c->classes[s->class_id].name, g_self);
    if (ty && !strcmp(ty, "ForwardingSuperNode")) {
      for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    }
    else {
      int smi = -1;
      for (int k = c->nscopes - 1; k >= 1; k--) {
        Scope *sc = &c->scopes[k];
        if (sc->class_id == s->class_id && sc->name && !strcmp(sc->name, shadow))
          { smi = k; break; }
      }
      emit_args_filled(c, smi, nt_ref(c->nt, id, "arguments"), ", ", b);
    }
    buf_puts(b, ")");
    return;
  }
  /* Strip __prep_N_ prefix to get the user method name for parent chain lookup. */
  const char *uname = comp_prep_user_name(s->name);
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  int mi = p >= 0 ? comp_method_in_chain(c, p, uname, &defcls) : -1;
  if (mi < 0) {
    /* super(msg) in exception subclass initialize: capture msg into self->msg */
    if (class_is_exc_subclass(c, s->class_id) && s->name && !strcmp(s->name, "initialize")) {
      int args_id = nt_ref(c->nt, id, "arguments");
      int argc2 = 0;
      const int *argv2 = NULL;
      if (args_id >= 0) argv2 = nt_arr(c->nt, args_id, "arguments", &argc2);
      if (argc2 > 0) {
        buf_printf(b, "(%s->msg = (", g_self);
        emit_expr(c, argv2[0], b);
        buf_puts(b, "))");
      }
      else if (ty && !strcmp(ty, "ForwardingSuperNode") && s->nparams > 0)
        buf_printf(b, "(%s->msg = lv_%s)", g_self, s->pnames[0]);
      else
        buf_puts(b, "((void)0)");
      return;
    }
    unsupported(c, id, "super (no parent method)");
    return;
  }
  buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, mc(uname), c->classes[defcls].name, g_self);
  if (ty && !strcmp(ty, "ForwardingSuperNode")) {
    Scope *pm = &c->scopes[mi];
    int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
    for (int i = 0; i < n; i++) {
      LocalVar *src = scope_local(s, s->pnames[i]);
      LocalVar *dst = scope_local(pm, pm->pnames[i]);
      TyKind st = src ? src->type : TY_UNKNOWN;
      TyKind dt = dst ? dst->type : TY_UNKNOWN;
      if (dt == TY_POLY && st != TY_POLY && st != TY_UNKNOWN) {
        buf_puts(b, ", ");
        Buf _bx; memset(&_bx, 0, sizeof _bx);
        buf_printf(&_bx, "lv_%s", s->pnames[i]);
        emit_boxed_text(c, st, _bx.p, b);
        free(_bx.p);
      }
      else {
        buf_printf(b, ", lv_%s", s->pnames[i]);
      }
    }
  }
  else {
    emit_args_filled(c, mi, nt_ref(c->nt, id, "arguments"), ", ", b);
  }
  buf_puts(b, ")");
}

/* Emit the static regex-literal globals and the sp_re_init() that compiles
   them at startup. Always defines sp_re_init (empty when no literals) so
   main() can call it unconditionally. */
void emit_regex_section(Buf *b) {
  for (int i = 0; i < g_re_count; i++) {
    buf_printf(b, "static mrb_regexp_pattern *sp_re_pat_%d;\n", i);
  }
  buf_puts(b, "static void sp_re_init(void) {\n");
  buf_puts(b, "  sp_sym_name_fn = sp_sym_to_s;\n");
  if (g_needs_class_machinery)
    buf_puts(b, "  sp_user_exc_parent_fn = sp_user_exc_parent;\n");
  /* Replace the runtime's hook with the superset that also marks this
     program's heap-typed globals/constants/class-ivars (it chains to
     sp_re_mark_globals itself). */
  buf_puts(b, "  sp_gc_mark_globals_hook = sp_mark_user_globals;\n");
  /* Install the Ractor message codec (defined in sp_runtime.h, where the heap
     allocators it rebuilds into are visible). */
  buf_puts(b, "  sp_ractor_codec_install();\n");
  buf_puts(b, "  sp_ractor_obj_codec_install();\n");
  if (g_re_count > 0) {
    buf_puts(b, "  sp_re_set_error_handler(sp_re_startup_error_handler);\n");
    for (int i = 0; i < g_re_count; i++) {
      buf_puts(b, "  sp_re_startup_err = NULL;\n");
      buf_puts(b, "  if (setjmp(sp_re_startup_jmp) == 0) {\n");
      buf_printf(b, "    sp_re_pat_%d = re_compile(", i);
      emit_str_literal(b, g_re_src[i]);
      buf_printf(b, ", %d, %d);\n", (int)strlen(g_re_src[i]), g_re_flg[i]);
      buf_printf(b, "  } else {\n    sp_re_pat_%d = NULL;\n  }\n", i);
    }
  }
  /* From here on (runtime Regexp.new / dynamic patterns), a compile error
     raises a catchable RegexpError via sp_raise_cls instead of aborting. */
  buf_puts(b, "  sp_re_set_error_handler(sp_re_default_error_handler);\n");
  buf_puts(b, "}\n\n");
}

/* ---- analyze-only / side-artifact emit modes ----
   These mirror the legacy Ruby backend's --emit-* flags. Each is gated on an
   environment variable (set by the `spinel` driver) and consumes only the
   analysis result, so they run right after analyze_program and short-circuit
   codegen. */

/* Append `s` to `b`, escaping it as a JSON string body (no surrounding quotes). */
static void json_escape_into(Buf *b, const char *s) {
  if (!s) return;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    unsigned char ch = *p;
    switch (ch) {
      case '"':  buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\t': buf_puts(b, "\\t"); break;
      case '\r': buf_puts(b, "\\r"); break;
      default:
        if (ch < 0x20) buf_printf(b, "\\u%04x", (unsigned)ch);
        else buf_printf(b, "%c", (int)ch);
    }
  }
}

/* C class name `Foo_Bar` -> Ruby `Foo::Bar`. Lossy when a namespace segment
   itself contains a literal underscore (same assumption as the backtrace
   symbolizer). */
static void class_ruby_name_into(Buf *b, const char *cn) {
  for (const char *p = cn; *p; p++) {
    if (*p == '_') buf_puts(b, "::");
    else buf_printf(b, "%c", (int)*p);
  }
}

/* Resolve the source file/line of a method's `def`, falling back gracefully
   when positions weren't stamped (no SPINEL_DEBUG / SPINEL_LINE_MAP). */
static int scope_def_line(Compiler *c, Scope *s) {
  if (s->def_node < 0) return 0;
  return (int)nt_int(c->nt, s->def_node, "node_line", 0);
}
static const char *scope_def_file(Compiler *c, Scope *s) {
  int fid = s->def_node >= 0 ? (int)nt_int(c->nt, s->def_node, "node_file", 0) : 0;
  const char *path = nt_file_path(c->nt, fid);
  if (!path) path = c->nt->source_file;
  if (!path || !*path) path = "source.rb";
  return path;
}

/* Build the emitted-C-symbol -> Ruby-name map as JSON. The C name is taken
   from emit_method_cname so it matches exactly what codegen emits (e.g.
   `sp_<Class>_s_<m>` for a singleton method). */
static char *build_symbol_map_json(Compiler *c) {
  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "{\n  \"symbols\": [\n");
  int n = 0;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!s->name || !*s->name) continue;
    Buf cb; memset(&cb, 0, sizeof cb);
    emit_method_cname(c, s, &cb);
    Buf rb; memset(&rb, 0, sizeof rb);
    const char *kind;
    if (s->class_id < 0) {
      kind = "toplevel";
      buf_puts(&rb, s->name);
    }
    else {
      /* the namespace-qualified Ruby class path (Tep::Url). A class renamed by
         the colliding-class pass (#1425) already encodes its full path as
         `Mod__Leaf`, and its enclosing_class still points at the module, so a
         chain walk would double-count -- demangle `__`->`::` instead. Otherwise
         walk the enclosing-class chain (the leaf C name drops the module). */
      const char *cn = c->classes[s->class_id].name;
      if (cn && strstr(cn, "__")) {
        for (const char *p = cn; *p; ) {
          if (p[0] == '_' && p[1] == '_') { buf_puts(&rb, "::"); p += 2; }
          else { buf_printf(&rb, "%c", (int)*p); p++; }
        }
      }
      else {
        const char *qn = class_ruby_name(c, s->class_id);
        buf_puts(&rb, qn ? qn : (cn ? cn : ""));
      }
      buf_puts(&rb, s->is_cmethod ? "." : "#");
      buf_puts(&rb, s->name);
      kind = s->is_cmethod ? "cmeth" : "imeth";
    }
    if (n > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"c\":\"");
    json_escape_into(&b, cb.p ? cb.p : "");
    buf_puts(&b, "\",\"ruby\":\"");
    json_escape_into(&b, rb.p ? rb.p : "");
    buf_printf(&b, "\",\"kind\":\"%s\"", kind);
    int ln = scope_def_line(c, s);
    if (ln > 0) {
      buf_puts(&b, ",\"file\":\"");
      json_escape_into(&b, scope_def_file(c, s));
      buf_printf(&b, "\",\"line\":%d}", ln);
    }
    else {
      buf_puts(&b, ",\"file\":null,\"line\":null}");
    }
    free(cb.p);
    free(rb.p);
    n++;
  }
  buf_puts(&b, "\n  ]\n}\n");
  return b.p ? b.p : strdup("{\n  \"symbols\": [\n\n  ]\n}\n");
}

/* Append the RBS form of `t`. Containers recurse via the type lattice's
   element/key/value accessors; the boxed `poly` family and anything
   unrecognized degrade to `untyped`. */
static void ty_to_rbs_into(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].name)
      class_ruby_name_into(b, c->classes[cid].name);
    else
      buf_puts(b, "untyped");
    return;
  }
  if (ty_is_array(t)) {
    buf_puts(b, "Array[");
    ty_to_rbs_into(c, ty_array_elem(t), b);
    buf_puts(b, "]");
    return;
  }
  if (ty_is_hash(t)) {
    buf_puts(b, "Hash[");
    ty_to_rbs_into(c, ty_hash_key(t), b);
    buf_puts(b, ", ");
    ty_to_rbs_into(c, ty_hash_val(t), b);
    buf_puts(b, "]");
    return;
  }
  switch (t) {
    case TY_INT: case TY_BIGINT:   buf_puts(b, "Integer"); break;
    case TY_FLOAT:                 buf_puts(b, "Float"); break;
    case TY_STRING: case TY_STRBUF: buf_puts(b, "String"); break;
    case TY_SYMBOL:                buf_puts(b, "Symbol"); break;
    case TY_BOOL:                  buf_puts(b, "bool"); break;
    case TY_NIL:                   buf_puts(b, "nil"); break;
    case TY_VOID:                  buf_puts(b, "void"); break;
    case TY_RANGE:                 buf_puts(b, "Range[Integer]"); break;
    case TY_TIME:                  buf_puts(b, "Time"); break;
    case TY_REGEX:                 buf_puts(b, "Regexp"); break;
    case TY_MATCHDATA:             buf_puts(b, "MatchData"); break;
    case TY_EXCEPTION:             buf_puts(b, "Exception"); break;
    case TY_COMPLEX:               buf_puts(b, "Complex"); break;
    case TY_RATIONAL:              buf_puts(b, "Rational"); break;
    case TY_STRINGIO:              buf_puts(b, "StringIO"); break;
    case TY_STRINGSCANNER:         buf_puts(b, "StringScanner"); break;
    case TY_PROC: case TY_CURRY:   buf_puts(b, "Proc"); break;
    case TY_FIBER:                 buf_puts(b, "Fiber"); break;
    case TY_RACTOR:                buf_puts(b, "Ractor"); break;
    case TY_RANDOM:                buf_puts(b, "Random"); break;
    case TY_METHOD:                buf_puts(b, "Method"); break;
    case TY_IO:                    buf_puts(b, "IO"); break;
    case TY_ARGF:                  buf_puts(b, "ARGF"); break;
    case TY_CLASS:                 buf_puts(b, "Class"); break;
    default:                       buf_puts(b, "untyped"); break;
  }
}

/* A type that landed on the boxed slow path -- its RBS is `untyped`, so the
   method line gets a "widened" comment. */
static int ty_is_degraded(TyKind t) {
  return t == TY_POLY || t == TY_POLY_ARRAY || t == TY_POLY_POLY_HASH ||
         t == TY_SYM_POLY_HASH || t == TY_STR_POLY_HASH;
}

/* Emit one `  <defprefix>: (params) -> ret` RBS line for scope `s`, with a
   degrade comment when any param/return widened to untyped. */
static void rbs_method_line(Compiler *c, Buf *b, const char *defprefix, Scope *s) {
  int degraded = 0;
  buf_printf(b, "  %s: (", defprefix);
  int j = 0;
  for (int i = 0; i < s->nparams; i++) {
    LocalVar *p = scope_local(s, s->pnames[i]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    if (j > 0) buf_puts(b, ", ");
    ty_to_rbs_into(c, pt, b);
    if (ty_is_degraded(pt)) degraded = 1;
    j++;
  }
  buf_puts(b, ") -> ");
  if (s->ret == TY_UNKNOWN || s->ret == TY_VOID) {
    buf_puts(b, "void");
  }
  else {
    ty_to_rbs_into(c, s->ret, b);
    if (ty_is_degraded(s->ret)) degraded = 1;
  }
  if (degraded) buf_puts(b, " # spinel: widened to untyped (slow path)");
  buf_puts(b, "\n");
}

/* Append every method scope of class `ci` (instance methods when cmeth==0,
   singleton methods when cmeth==1) as RBS lines. */
static void rbs_class_methods(Compiler *c, Buf *b, int ci, int cmeth) {
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id != ci || !!s->is_cmethod != !!cmeth) continue;
    if (!s->name || !*s->name) continue;
    Buf pre; memset(&pre, 0, sizeof pre);
    buf_printf(&pre, "def %s%s", cmeth ? "self." : "", s->name);
    rbs_method_line(c, b, pre.p ? pre.p : "def ?", s);
    free(pre.p);
  }
}

/* Build the inferred-signature dump as RBS, mirroring the legacy backend:
   top-level methods wrapped in `class Object`, then a `class` block per user
   class with its ivars and instance/singleton methods. */
static char *build_rbs_text(Compiler *c) {
  Buf b; memset(&b, 0, sizeof b);
  int has_top = 0;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (s->class_id < 0 && s->name && *s->name) { has_top = 1; break; }
  }
  if (has_top) {
    buf_puts(&b, "class Object\n");
    for (int si = 1; si < c->nscopes; si++) {
      Scope *s = &c->scopes[si];
      if (s->class_id < 0 && s->name && *s->name) {
        Buf pre; memset(&pre, 0, sizeof pre);
        buf_printf(&pre, "def %s", s->name);
        rbs_method_line(c, &b, pre.p ? pre.p : "def ?", s);
        free(pre.p);
      }
    }
    buf_puts(&b, "end\n\n");
  }
  for (int ci = 0; ci < c->nclasses; ci++) {
    ClassInfo *cls = &c->classes[ci];
    if (!cls->name || !*cls->name) continue;
    /* Skip the Spinel-injected Method class so the .rbs reflects only the
       user's program (matches the legacy filter). */
    if (!strcmp(cls->name, "Method")) continue;
    Buf nb; memset(&nb, 0, sizeof nb);
    class_ruby_name_into(&nb, cls->name);
    buf_printf(&b, "class %s", nb.p ? nb.p : "");
    free(nb.p);
    if (cls->parent >= 0 && cls->parent < c->nclasses) {
      const char *pn = c->classes[cls->parent].name;
      if (pn && *pn && strcmp(pn, "Object") != 0) {
        Buf pb; memset(&pb, 0, sizeof pb);
        class_ruby_name_into(&pb, pn);
        buf_printf(&b, " < %s", pb.p ? pb.p : "");
        free(pb.p);
      }
    }
    buf_puts(&b, "\n");
    for (int k = 0; k < cls->nivars; k++) {
      const char *iv = cls->ivars[k];
      if (!iv || !*iv) continue;
      buf_printf(&b, "  %s: ", iv);
      ty_to_rbs_into(c, cls->ivar_types[k], &b);
      buf_puts(&b, "\n");
    }
    rbs_class_methods(c, &b, ci, 0);
    rbs_class_methods(c, &b, ci, 1);
    buf_puts(&b, "end\n\n");
  }
  return b.p ? b.p : strdup("");
}

/* The legacy string tag for `t` (e.g. "int", "int_array", "obj_Foo"). Objects
   aren't in ty_name's switch, so spell them as obj_<ClassName> here. */
static void ty_tag_into(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].name)
      buf_printf(b, "obj_%s", c->classes[cid].name);
    else
      buf_puts(b, "object");
    return;
  }
  buf_puts(b, ty_name(t));
}

/* Resolve `fid` to a source path for the position-keyed exports. */
static const char *emit_file_path(Compiler *c, int fid) {
  const char *path = nt_file_path(c->nt, fid);
  if (!path) path = c->nt->source_file;
  if (!path || !*path) path = "source.rb";
  return path;
}

/* 1 when scope `s`'s signature widened to the boxed poly slow path. */
static int scope_sig_degraded(Compiler *c, Scope *s) {
  if (ty_is_degraded(s->ret)) return 1;
  for (int i = 0; i < s->nparams; i++) {
    LocalVar *p = scope_local(s, s->pnames[i]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    if (ty_is_degraded(pt)) return 1;
  }
  return 0;
}

/* Build the position-keyed type + diagnostics JSON for the ruby-lsp addon:
   every node with a concrete inferred type keyed by {file,line,col}, plus one
   warning per method whose signature degraded to untyped. Positions come from
   the parser's node_line/node_col/node_file (the SPINEL_DEBUG machinery), so
   the driver enables it. */
static char *build_types_json(Compiler *c) {
  const NodeTable *nt = c->nt;
  Buf b; memset(&b, 0, sizeof b);
  buf_puts(&b, "{\n  \"types\": [\n");
  int tn = 0;
  for (int id = 0; id < nt->count && id < c->node_cap; id++) {
    TyKind t = c->ntype[id];
    if (t == TY_UNKNOWN || t == TY_VOID) continue;
    int ln = (int)nt_int(nt, id, "node_line", 0);
    if (ln <= 0) continue;
    int col = (int)nt_int(nt, id, "node_col", 0);
    int fid = (int)nt_int(nt, id, "node_file", 0);
    if (tn > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"file\":\"");
    json_escape_into(&b, emit_file_path(c, fid));
    buf_printf(&b, "\",\"line\":%d,\"col\":%d,\"type\":\"", ln, col);
    Buf tag; memset(&tag, 0, sizeof tag);
    ty_tag_into(c, t, &tag);
    json_escape_into(&b, tag.p ? tag.p : "");
    free(tag.p);
    buf_puts(&b, "\",\"rbs\":\"");
    Buf rbs; memset(&rbs, 0, sizeof rbs);
    ty_to_rbs_into(c, t, &rbs);
    json_escape_into(&b, rbs.p ? rbs.p : "");
    free(rbs.p);
    buf_puts(&b, "\"}");
    tn++;
  }
  buf_puts(&b, "\n  ],\n  \"diagnostics\": [\n");
  int dn = 0;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!s->name || !*s->name || s->def_node < 0) continue;
    if (!scope_sig_degraded(c, s)) continue;
    int ln = (int)nt_int(nt, s->def_node, "node_line", 0);
    if (ln <= 0) continue;
    int col = (int)nt_int(nt, s->def_node, "node_col", 0);
    int fid = (int)nt_int(nt, s->def_node, "node_file", 0);
    if (dn > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"file\":\"");
    json_escape_into(&b, emit_file_path(c, fid));
    buf_printf(&b, "\",\"line\":%d,\"col\":%d,\"severity\":\"warning\",\"message\":\"", ln, col);
    Buf msg; memset(&msg, 0, sizeof msg);
    buf_printf(&msg, "Spinel: `%s` has a parameter or return widened to untyped (boxed poly slow path)", s->name);
    json_escape_into(&b, msg.p ? msg.p : "");
    free(msg.p);
    buf_puts(&b, "\"}");
    dn++;
  }
  buf_puts(&b, "\n  ]\n}\n");
  return b.p ? b.p : strdup("{\n  \"types\": [\n\n  ],\n  \"diagnostics\": [\n\n  ]\n}\n");
}

/* Write `text` to `path`; warn (but don't abort) on failure. */
static int emit_write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) {
    fprintf(stderr, "spinel: cannot write '%s'\n", path);
    return 0;
  }
  fputs(text, f);
  fclose(f);
  return 1;
}

/* ---- top level ---- */

/* Conservative pre-scan: does the program use the class-introspection helper
   bank (sp_class_to_s / sp_class_superclass / sp_class_is_ancestor /
   sp_class_ancestors / sp_poly_is_a / sp_user_exc_parent / ...)? Any user
   class/module forces it on (the struct/scan emission and dispatch may touch
   it). With no user classes, only explicit builtin introspection needs it: a
   `.class` / `is_a?` / `kind_of?` / `instance_of?` / `ancestors` /
   `superclass` / `===` call, or a builtin class constant used as a value
   (e.g. `puts Integer`, `Integer < Numeric`). Over-approximating is safe (it
   only emits dead helpers); under-approximating would be a hard link error, so
   the set is deliberately broad. */
static int program_needs_class_machinery(Compiler *c) {
  if (c->nclasses > 0) return 1;
  const NodeTable *nt = c->nt;
  for (int i = 0; i < nt->count; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    if (!strcmp(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && (!strcmp(nm, "class") || !strcmp(nm, "is_a?") ||
                 !strcmp(nm, "kind_of?") || !strcmp(nm, "instance_of?") ||
                 !strcmp(nm, "ancestors") || !strcmp(nm, "superclass") ||
                 !strcmp(nm, "===")))
        return 1;
    }
    else if (!strcmp(ty, "ConstantReadNode") || !strcmp(ty, "ConstantPathNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && is_builtin_class_name(nm)) return 1;
    }
  }
  return 0;
}

char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  /* `#line` directives are emitted only when the parser stamped per-node
     source lines (SPINEL_LINE_MAP / SPINEL_DEBUG); the same env gates both
     sides so codegen and the AST agree. */
  g_line_map = (getenv("SPINEL_LINE_MAP") || getenv("SPINEL_DEBUG")) ? 1 : 0;
  g_debug = getenv("SPINEL_DEBUG") ? 1 : 0;

  /* Analyze-only emit modes (legacy --emit-*): write the requested artifact
     from the analysis result and skip codegen. Returns an empty translation
     unit so the driver writes no binary. */
  const char *sym_out = getenv("SPINEL_EMIT_SYMBOL_MAP");
  if (sym_out && *sym_out) {
    char *json = build_symbol_map_json(c);
    emit_write_file(sym_out, json);
    free(json);
    comp_free(c);
    return strdup("");
  }
  const char *rbs_out = getenv("SPINEL_EMIT_RBS");
  if (rbs_out && *rbs_out) {
    char *rbs = build_rbs_text(c);
    emit_write_file(rbs_out, rbs);
    free(rbs);
    comp_free(c);
    return strdup("");
  }
  const char *types_out = getenv("SPINEL_EMIT_TYPES");
  if (types_out && *types_out) {
    char *json = build_types_json(c);
    emit_write_file(types_out, json);
    free(json);
    comp_free(c);
    return strdup("");
  }

  Buf b; memset(&b, 0, sizeof b);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_proc_counter = 0;
  g_needs_at_exit = 0;
  g_re_count = 0;
  buf_puts(&b, "/* Generated by Spinel AOT compiler */\n");
  buf_puts(&b, "#include \"sp_runtime.h\"\n");
  /* FFI extern declarations and buffer storage */
  {
    Compiler *cf = c;
    /* Link/cflag markers: the spinel driver greps these out of the
       generated C and appends them to the cc command line. One marker
       per ';'-separated token, matching the legacy emitter's format. */
    for (int li = 0; li < cf->n_ffi_libs; li++) {
      for (const char *s = cf->ffi_lib_names[li]; ; ) {
        const char *semi = strchr(s, ';');
        int len = semi ? (int)(semi - s) : (int)strlen(s);
        if (len > 0) buf_printf(&b, "/* SPINEL_LINK: -l%.*s */\n", len, s);
        if (!semi) break;
        s = semi + 1;
      }
    }
    for (int ci = 0; ci < cf->n_ffi_cflags; ci++) {
      for (const char *s = cf->ffi_cflag_vals[ci]; ; ) {
        const char *semi = strchr(s, ';');
        int len = semi ? (int)(semi - s) : (int)strlen(s);
        if (len > 0) buf_printf(&b, "/* SPINEL_CFLAGS: %.*s */\n", len, s);
        if (!semi) break;
        s = semi + 1;
      }
    }
    for (int fi = 0; fi < cf->n_ffi_funcs; fi++) {
      const char *ret = cf->ffi_func_ret[fi];
      buf_puts(&b, "extern ");
      buf_puts(&b, ffi_c_type(ret));
      buf_puts(&b, " ");
      buf_puts(&b, cf->ffi_func_names[fi]);
      buf_puts(&b, "(");
      for (int ai = 0; ai < cf->ffi_func_nargs[fi]; ai++) {
        if (ai) buf_puts(&b, ", ");
        buf_puts(&b, ffi_c_type(cf->ffi_func_args[fi][ai]));
      }
      if (cf->ffi_func_nargs[fi] == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    for (int bi = 0; bi < cf->n_ffi_bufs; bi++) {
      buf_printf(&b, "static char sp_ffi_buf_%s_%s[%d];\n",
                 cf->ffi_buf_mods[bi], cf->ffi_buf_names[bi], cf->ffi_buf_sizes[bi]);
    }
  }
  {
    int ns = c->nsymbols;
    if (ns > 0) {
      buf_printf(&b, "static const char *const sp_sym_names[%d] = {", ns);
      for (int i = 0; i < ns; i++) {
        if (i) buf_puts(&b, ", ");
        emit_str_literal(&b, c->symbols[i]);
      }
      buf_puts(&b, "};\n");
    }
    /* dynamic intern pool: symbols minted at runtime (Symbol#upcase,
       :"interp", String#to_sym) get ids >= the static count. */
    buf_puts(&b, "static const char *sp_dyn_syms[8192]; static int sp_ndyn = 0;\n");
    buf_printf(&b, "static const char *sp_sym_to_s(sp_sym id){"
                   "if(id>=0&&id<%d)return %s;"
                   "if(id>=%d&&id<%d+sp_ndyn)return sp_dyn_syms[id-%d];"
                   "return \"\";}\n", ns, ns > 0 ? "sp_sym_names[id]" : "\"\"", ns, ns, ns);
    buf_printf(&b, "static sp_sym sp_sym_intern(const char *s){"
                   "for(int i=0;i<%d;i++)if(strcmp(%s,s)==0)return (sp_sym)i;"
                   "for(int i=0;i<sp_ndyn;i++)if(strcmp(sp_dyn_syms[i],s)==0)return (sp_sym)(%d+i);"
                   "if(sp_ndyn<8192){sp_dyn_syms[sp_ndyn]=sp_str_dup_external(s);return (sp_sym)(%d+sp_ndyn++);}"
                   "return (sp_sym)0;}\n\n", ns, ns > 0 ? "sp_sym_names[i]" : "\"\"", ns, ns);
  }
  /* sp_class_to_s is referenced by the runtime itself (sp_poly_puts /
     sp_poly_to_s SP_TAG_CLASS arms), so it is always emitted, independent of
     the gated introspection bank below. */
  {
    buf_puts(&b, "static const char *sp_class_to_s(sp_Class c){switch(c.cls_id){");
    for (int i = 0; i < c->nclasses; i++) {
      if (!is_builtin_reopen(c->classes[i].name)) {
        const char *qname = class_ruby_name(c, i);
        if (!qname) qname = c->classes[i].name;
        buf_printf(&b, "case %d:return SPL(\"%s\");", i, qname);
      }
    }
    /* builtin class name cases (negative cls_ids) */
    buf_puts(&b, "case -100:return SPL(\"Integer\");case -101:return SPL(\"Float\");");
    buf_puts(&b, "case -102:return SPL(\"String\");case -103:return SPL(\"Symbol\");");
    buf_puts(&b, "case -104:return SPL(\"Array\");case -105:return SPL(\"Hash\");");
    buf_puts(&b, "case -106:return SPL(\"Range\");case -107:return SPL(\"Time\");");
    buf_puts(&b, "case -108:return SPL(\"Module\");case -109:return SPL(\"Class\");");
    buf_puts(&b, "case -110:return SPL(\"NilClass\");case -111:return SPL(\"TrueClass\");");
    buf_puts(&b, "case -112:return SPL(\"FalseClass\");case -113:return SPL(\"Numeric\");");
    buf_puts(&b, "case -114:return SPL(\"Comparable\");case -115:return SPL(\"Enumerable\");");
    buf_puts(&b, "case -116:return SPL(\"Object\");case -117:return SPL(\"BasicObject\");");
    buf_puts(&b, "case -118:return SPL(\"Proc\");case -119:return SPL(\"Kernel\");");
    buf_puts(&b, "case -120:return SPL(\"IO\");case -121:return SPL(\"File\");");
    buf_puts(&b, "case -122:return SPL(\"Exception\");case -123:return SPL(\"StandardError\");");
    buf_puts(&b, "case -124:return SPL(\"RuntimeError\");case -125:return SPL(\"TypeError\");");
    buf_puts(&b, "case -126:return SPL(\"ArgumentError\");case -127:return SPL(\"NameError\");");
    buf_puts(&b, "case -128:return SPL(\"NoMethodError\");case -129:return SPL(\"StopIteration\");");
    buf_puts(&b, "case -130:return SPL(\"Math\");case -131:return SPL(\"Complex\");");
    buf_puts(&b, "default:return \"\";} }\n\n");
  }
  g_needs_class_machinery = program_needs_class_machinery(c);
  if (g_needs_class_machinery) {
  /* sp_cls_is_module[i]: 1 if user class i was defined as a module, 0 if class */
  if (c->nclasses > 0) {
    buf_printf(&b, "static const int sp_cls_is_module[%d] = {", c->nclasses);
    for (int i = 0; i < c->nclasses; i++) {
      if (i) buf_puts(&b, ",");
      const char *dt = nt_type(c->nt, c->classes[i].def_node);
      buf_printf(&b, "%d", (dt && !strcmp(dt, "ModuleNode")) ? 1 : 0);
    }
    buf_puts(&b, "};\n");
  }
  /* sp_class_is_module_val: true if sp_Class c is a module (not a class) */
  buf_puts(&b, "static int sp_class_is_module_val(sp_Class c){\n");
  if (c->nclasses > 0)
    buf_printf(&b, "  if(c.cls_id>=0&&c.cls_id<%d)return sp_cls_is_module[c.cls_id];\n", c->nclasses);
  /* builtin modules: Comparable(-114), Enumerable(-115), Kernel(-119) */
  buf_puts(&b, "  return(c.cls_id==-114||c.cls_id==-115||c.cls_id==-119);\n}\n");

  /* sp_class_superclass: parent class for user classes (negative ids map to
     Object builtin). Returns ((sp_Class){-116}) for unknown/root. */
  {
    buf_puts(&b, "static sp_Class sp_class_superclass(sp_Class c){\n");
    buf_puts(&b, "  switch(c.cls_id){\n");
    for (int i = 0; i < c->nclasses; i++) {
      if (is_builtin_reopen(c->classes[i].name)) continue;
      int par = c->classes[i].parent;
      if (par >= 0) {
        buf_printf(&b, "  case %d: return ((sp_Class){%d});\n", i, par);
      }
      else {
        /* Check if the ClassNode has a builtin superclass. */
        int sc_node = nt_ref(c->nt, c->classes[i].def_node, "superclass");
        int builtin_par = -116;  /* Object */
        if (sc_node >= 0) {
          const char *sc_ty = nt_type(c->nt, sc_node);
          const char *sc_nm = (sc_ty && (!strcmp(sc_ty, "ConstantReadNode") || !strcmp(sc_ty, "ConstantPathNode"))) ? nt_str(c->nt, sc_node, "name") : NULL;
          if (sc_nm) { int bid = builtin_class_id(sc_nm); if (bid != 0) builtin_par = bid; }
        }
        buf_printf(&b, "  case %d: return ((sp_Class){%d});\n", i, builtin_par);
      }
    }
    buf_puts(&b, "  default: return ((sp_Class){-116});\n  }\n}\n");
  }
  /* Forward decl: sp_builtin_superclass is defined below but used by sp_class_is_ancestor. */
  buf_puts(&b, "static sp_Class sp_builtin_superclass(sp_Class c);\n");
  /* sp_class_is_ancestor(anc, desc): 1 if anc is an ancestor of desc (or same). */
  {
    /* sp_class_is_ancestor is declared before sp_builtin_superclass; used only by
       the simple sp_class_le before modules. sp_class_le_mod (defined after
       sp_class_ancestors) supersedes it. Keep simple for non-module programs. */
    buf_puts(&b, "static int sp_class_is_ancestor(sp_Class anc, sp_Class desc);\n");
    buf_puts(&b, "static int sp_class_is_ancestor(sp_Class anc, sp_Class desc){\n");
    buf_puts(&b, "  sp_Class cur = desc;\n");
    int depth = c->nclasses + 40;
    buf_printf(&b, "  for(int _i=0;_i<%d;_i++){\n", depth);
    buf_puts(&b, "    if(cur.cls_id==anc.cls_id)return 1;\n");
    buf_puts(&b, "    if(cur.cls_id==-117)break;\n"); /* BasicObject: root */
    buf_puts(&b, "    sp_Class next=cur.cls_id>=0?sp_class_superclass(cur):sp_builtin_superclass(cur);\n");
    buf_puts(&b, "    if(next.cls_id==cur.cls_id)break;\n");
    buf_puts(&b, "    cur=next;\n");
    buf_puts(&b, "  }\n");
    buf_puts(&b, "  return 0;\n}\n");
  }
  /* Builtin superclass chain (simplified Ruby class hierarchy) */
  buf_puts(&b, "static sp_Class sp_builtin_superclass(sp_Class c){\n");
  buf_puts(&b, "  switch(c.cls_id){\n");
  /* Integer, Float -> Numeric -> Object */
  buf_puts(&b, "  case -100:case -101: return ((sp_Class){-113});\n"); /* -> Numeric */
  /* Numeric, String, Array, Hash, Range, Symbol, Time -> Object */
  buf_puts(&b, "  case -102:case -103:case -104:case -105:case -106:case -107:case -113: return ((sp_Class){-116});\n");
  /* Exception -> Object */
  buf_puts(&b, "  case -122: return ((sp_Class){-116});\n");
  /* StandardError, RuntimeError -> Exception */
  buf_puts(&b, "  case -123:case -124: return ((sp_Class){-122});\n");
  /* TypeError, ArgumentError, NameError, StopIteration -> StandardError */
  buf_puts(&b, "  case -125:case -126:case -127:case -129: return ((sp_Class){-123});\n");
  /* NoMethodError -> NameError */
  buf_puts(&b, "  case -128: return ((sp_Class){-127});\n");
  /* NilClass, TrueClass, FalseClass, Proc -> Object */
  buf_puts(&b, "  case -110:case -111:case -112:case -118: return ((sp_Class){-116});\n");
  /* Module -> Object, Class -> Module */
  buf_puts(&b, "  case -108: return ((sp_Class){-116});\n");
  buf_puts(&b, "  case -109: return ((sp_Class){-108});\n");
  /* Object -> BasicObject */
  buf_puts(&b, "  case -116: return ((sp_Class){-117});\n");
  /* BasicObject: root */
  buf_puts(&b, "  case -117: return ((sp_Class){-117});\n");
  buf_puts(&b, "  default: return ((sp_Class){-116});\n  }\n}\n");

  buf_puts(&b, "static int sp_class_lt(sp_Class a,sp_Class b){return a.cls_id!=b.cls_id&&sp_class_is_ancestor(b,a);}\n");
  buf_puts(&b, "static int sp_class_le(sp_Class a,sp_Class b){return sp_class_is_ancestor(b,a);}\n");
  buf_puts(&b, "static int sp_class_gt(sp_Class a,sp_Class b){return sp_class_lt(b,a);}\n");
  buf_puts(&b, "static int sp_class_ge(sp_Class a,sp_Class b){return sp_class_le(b,a);}\n");
  /* module-aware versions (replace after sp_class_ancestors is defined) */
  /* sp_class_includes_<i>: static array of included module cls_ids per class */
  /* Also update sp_class_is_ancestor to walk includes. */
  /* Build per-class includes array by scanning the AST. */
  {
    /* For each user class, collect included module ids (in include order). */
    int **cls_incs = calloc((size_t)c->nclasses, sizeof(int *));
    int  *cls_nincs = calloc((size_t)c->nclasses, sizeof(int));
    for (int ci = 0; ci < c->nclasses; ci++) {
      /* scan def_node body and all reopenings */
      for (int id = 0; id < c->nt->count; id++) {
        const char *ty2 = nt_type(c->nt, id);
        if (!ty2 || (strcmp(ty2, "ClassNode") && strcmp(ty2, "ModuleNode"))) continue;
        int cp2 = nt_ref(c->nt, id, "constant_path");
        const char *cn2 = cp2 >= 0 ? nt_str(c->nt, cp2, "name") : NULL;
        if (!cn2 || comp_class_index(c, cn2) != ci) continue;
        int body2 = nt_ref(c->nt, id, "body");
        int bn2 = 0;
        const int *stmts2 = body2 >= 0 ? nt_arr(c->nt, body2, "body", &bn2) : NULL;
        for (int k2 = 0; k2 < bn2; k2++) {
          const char *sty2 = nt_type(c->nt, stmts2[k2]);
          if (!sty2 || strcmp(sty2, "CallNode")) continue;
          const char *nm2 = nt_str(c->nt, stmts2[k2], "name");
          if (!nm2 || strcmp(nm2, "include")) continue;
          if (nt_ref(c->nt, stmts2[k2], "receiver") >= 0) continue;
          int anode2 = nt_ref(c->nt, stmts2[k2], "arguments");
          int an2 = 0;
          const int *aargs = anode2 >= 0 ? nt_arr(c->nt, anode2, "arguments", &an2) : NULL;
          for (int j2 = 0; j2 < an2; j2++) {
            const char *aty2 = nt_type(c->nt, aargs[j2]);
            const char *mname2 = (aty2 && !strcmp(aty2, "ConstantReadNode")) ? nt_str(c->nt, aargs[j2], "name") : NULL;
            if (!mname2 && aty2 && !strcmp(aty2, "ConstantPathNode")) mname2 = nt_str(c->nt, aargs[j2], "name");
            int mid2 = mname2 ? comp_class_index(c, mname2) : -1;
            if (mid2 < 0) continue;
            /* deduplicate */
            int found2 = 0;
            for (int q = 0; q < cls_nincs[ci]; q++) if (cls_incs[ci][q] == mid2) { found2 = 1; break; }
            if (found2) continue;
            cls_incs[ci] = realloc(cls_incs[ci], sizeof(int) * (size_t)(cls_nincs[ci] + 1));
            cls_incs[ci][cls_nincs[ci]++] = mid2;
          }
        }
      }
    }
    /* Emit sp_class_ancestors using the include info. */
    buf_puts(&b, "static sp_PolyArray *sp_class_ancestors(sp_Class c){\n");
    buf_puts(&b, "  sp_PolyArray *a=sp_PolyArray_new();\n");
    buf_puts(&b, "  sp_Class cur=c;\n");
    int depth2 = c->nclasses + 20;
    buf_printf(&b, "  for(int _i=0;_i<%d;_i++){\n", depth2);
    /* When we reach a builtin class while walking user-class ancestors:
       if we STARTED from a builtin (c.cls_id<0), follow the full builtin
       chain with module includes; if we STARTED from a user class (c.cls_id>=0),
       stop here so that user-class .ancestors only returns user ancestors. */
    buf_puts(&b, "    if(cur.cls_id<0){\n");
    buf_puts(&b, "      if(c.cls_id>=0)break;\n");  /* started from user class: stop */
    buf_puts(&b, "      while(1){\n");
    buf_puts(&b, "        sp_PolyArray_push(a,sp_box_class(cur));\n");
    /* Numeric includes Comparable; Array/Hash include Enumerable; String includes Comparable */
    buf_puts(&b, "        if(cur.cls_id==-113) sp_PolyArray_push(a,sp_box_class(((sp_Class){-114})));\n");  /* Numeric->Comparable */
    buf_puts(&b, "        if(cur.cls_id==-104||cur.cls_id==-105) sp_PolyArray_push(a,sp_box_class(((sp_Class){-115})));\n");  /* Array/Hash->Enumerable */
    buf_puts(&b, "        if(cur.cls_id==-102) sp_PolyArray_push(a,sp_box_class(((sp_Class){-114})));\n");  /* String->Comparable */
    buf_puts(&b, "        if(cur.cls_id==-116) sp_PolyArray_push(a,sp_box_class(((sp_Class){-119})));\n");  /* Object->Kernel */
    buf_puts(&b, "        sp_Class bn=sp_builtin_superclass(cur);\n");
    buf_puts(&b, "        if(bn.cls_id==cur.cls_id)break;\n");
    buf_puts(&b, "        cur=bn;\n");
    buf_puts(&b, "      }\n");
    buf_puts(&b, "      break;\n    }\n");
    buf_puts(&b, "    sp_PolyArray_push(a,sp_box_class(cur));\n");
    /* inline the includes switch for this class */
    buf_puts(&b, "    switch(cur.cls_id){\n");
    for (int ci = 0; ci < c->nclasses; ci++) {
      if (cls_nincs[ci] == 0) continue;
      buf_printf(&b, "    case %d:", ci);
      /* Ruby includes are prepended: last include is highest priority, so
         insert in reverse include order after the class itself. */
      for (int q = cls_nincs[ci] - 1; q >= 0; q--)
        buf_printf(&b, " sp_PolyArray_push(a,sp_box_class(((sp_Class){%d})));", cls_incs[ci][q]);
      buf_puts(&b, " break;\n");
    }
    buf_puts(&b, "    }\n");
    buf_puts(&b, "    sp_Class next=sp_class_superclass(cur);\n");
    buf_puts(&b, "    if(next.cls_id==cur.cls_id)break;\n");
    buf_puts(&b, "    cur=next;\n");
    buf_puts(&b, "  }\n");
    buf_puts(&b, "  return a;\n}\n\n");
    /* Module-aware <= by walking sp_class_ancestors (replaces simpler versions). */
    buf_puts(&b, "static int sp_class_le_mod(sp_Class a,sp_Class b){\n");
    buf_puts(&b, "  /* a<=b: b is an ancestor of a, so b must appear in a's ancestors */\n");
    buf_puts(&b, "  sp_PolyArray *ancs=sp_class_ancestors(a);\n");
    buf_puts(&b, "  for(mrb_int _i=0;_i<sp_PolyArray_length(ancs);_i++){\n");
    buf_puts(&b, "    sp_RbVal v=sp_PolyArray_get(ancs,_i);\n");
    buf_puts(&b, "    if(v.tag==7&&(int)v.cls_id==b.cls_id)return 1;\n");
    buf_puts(&b, "  }\n");
    /* User-class sp_class_ancestors stops before builtin parents.
       If the target is a builtin, fall back to the chain-walking check. */
    buf_puts(&b, "  if(b.cls_id<0)return sp_class_is_ancestor(b,a);\n");
    buf_puts(&b, "  return 0;\n}\n");
    buf_puts(&b, "#undef sp_class_le\n#define sp_class_le sp_class_le_mod\n");
    buf_puts(&b, "#undef sp_class_lt\n#define sp_class_lt(a,b) ((a).cls_id!=(b).cls_id&&sp_class_le_mod(a,b))\n");
    buf_puts(&b, "#undef sp_class_gt\n#define sp_class_gt(a,b) ((a).cls_id!=(b).cls_id&&sp_class_le_mod(b,a))\n");
    buf_puts(&b, "#undef sp_class_ge\n#define sp_class_ge(a,b) sp_class_le_mod(b,a)\n");
    /* sp_poly_get_class: maps a poly value to its sp_Class for dynamic is_a? */
    buf_puts(&b,
      "static sp_Class sp_poly_get_class(sp_RbVal v){\n"
      "  switch(v.tag){\n"
      "  case SP_TAG_INT: return ((sp_Class){-100});\n"
      "  case SP_TAG_STR: return ((sp_Class){-102});\n"
      "  case SP_TAG_FLT: return ((sp_Class){-101});\n"
      "  case SP_TAG_BOOL: return v.v.b?((sp_Class){-111}):((sp_Class){-112});\n"
      "  case SP_TAG_NIL: return ((sp_Class){-110});\n"
      "  case SP_TAG_SYM: return ((sp_Class){-103});\n"
      "  case SP_TAG_OBJ: if(v.cls_id>=0)return ((sp_Class){v.cls_id});\n"
      "    if(v.cls_id>=-12)return ((sp_Class){-104});\n"  /* arrays */
      "    if(v.cls_id>=-20)return ((sp_Class){-105});\n"  /* hashes */
      "    return ((sp_Class){-116});\n"
      "  default: return ((sp_Class){-116});\n"
      "  }\n}\n"
      "static int sp_poly_is_a(sp_RbVal obj,sp_Class klass){\n"
      "  return sp_class_le(sp_poly_get_class(obj),klass);\n}\n");
    for (int ci = 0; ci < c->nclasses; ci++) free(cls_incs[ci]);
    free(cls_incs); free(cls_nincs);
  }
  /* User exception hierarchy: sp_user_exc_parent(cls) -> parent class name.
     Used by sp_exc_cls_matches (rescue arms) and sp_exc_is_a (is_a?). */
  {
    int any = 0;
    for (int i = 0; i < c->nclasses; i++) {
      if (class_is_exc_subclass(c, i)) { any = 1; break; }
    }
    buf_puts(&b, "static const char *sp_user_exc_parent(const char *cls){\n");
    if (!any) buf_puts(&b, "  (void)cls;\n");
    if (any) {
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i)) continue;
        const char *cn = class_ruby_name(c, i);
        if (!cn) cn = c->classes[i].name;
        /* find the direct parent name (builtin or user) */
        const char *par = NULL;
        int sc = nt_ref(c->nt, c->classes[i].def_node, "superclass");
        if (sc >= 0) {
          const char *sty = nt_type(c->nt, sc);
          if (sty && (!strcmp(sty, "ConstantReadNode") || !strcmp(sty, "ConstantPathNode")))
            par = nt_str(c->nt, sc, "name");
        }
        if (!par && c->classes[i].parent >= 0)
          par = c->classes[c->classes[i].parent].name;
        if (par) {
          buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return \"%s\";\n", cn, par);
          /* also register the leaf name if different from qualified name */
          if (cn != c->classes[i].name && strcmp(cn, c->classes[i].name))
            buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return \"%s\";\n", c->classes[i].name, par);
        }
      }
    }
    buf_puts(&b, "  return 0;\n}\n");
  }
  }  /* if (g_needs_class_machinery) */

  /* class structs + GC scan functions. Forward-declare every typedef first so
     a class struct may embed a pointer to a class defined later. */
  for (int i = 0; i < c->nclasses; i++) {
    if (is_builtin_reopen(c->classes[i].name)) continue;
    if (class_is_exc_subclass(c, i) && c->classes[i].nivars == 0)
      buf_printf(&b, "typedef sp_Exception sp_%s;\n", c->classes[i].name);
    else
      buf_printf(&b, "typedef struct sp_%s_s sp_%s;\n", c->classes[i].name, c->classes[i].name);
  }
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      emit_class_struct(c, &c->classes[i], &b);
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      emit_class_scan(c, &c->classes[i], &b);
  if (c->nclasses > 0) buf_puts(&b, "\n");

  /* class variables: one file-scope static per (class, @@var) */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->ncvars; j++) {
      TyKind t = ci->cvar_types[j] == TY_UNKNOWN ? TY_INT : ci->cvar_types[j];
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " cvar_%s_%s = %s;\n", ci->name, ci->cvars[j] + 2,
                 t == TY_RANGE ? "{0}" : default_value(t));
    }
  }

  /* singleton accessor slots: `class << self; attr_accessor :x; end`
     backed by a file-scope sp_RbVal per (class, name), init = nil. */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->nsg_readers; j++)
      buf_printf(&b, "static sp_RbVal sg_%s_%s = {SP_TAG_NIL, 0, {0}};\n",
                 ci->name, ci->sg_readers[j]);
  }

  /* module/class-level instance variables (accessed from a `def self.X`):
     one file-scope static per (class, @ivar). */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->nivars; j++) {
      TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
      /* static initializers must be constant.  Class-level ivars start as nil:
         int → SP_INT_NIL, string → NULL, poly → {SP_TAG_NIL,0,{0}}.
         range/time zero-init with {0}. */
      const char *init = (t == TY_RANGE || t == TY_TIME) ? "{0}"
                       : (t == TY_POLY) ? "{SP_TAG_NIL, 0, {0}}"
                       : (t == TY_INT)  ? "SP_INT_NIL"
                       : (t == TY_STRING) ? "NULL"
                       : (is_scalar_ret(t)) ? default_value(t) : "0";
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " civ_%s_%s = %s;\n", ci->name, ci->ivars[j] + 1, init);
    }
  }

  /* method prototypes (scope 0 is top-level) */
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s) || c->scopes[s].is_transplanted_source) continue; emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }
  /* constructor prototypes + definitions (after method protos: new calls initialize) */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (is_builtin_reopen(ci->name)) continue;
    if (ci->is_struct) {
      /* struct constructor takes typed member params -- the prototype must
         match the definition (an empty () prototype + a _Bool param differ) */
      buf_printf(&b, "static sp_%s *sp_%s_new(", ci->name, ci->name);
      for (int m = 0; m < ci->nivars; m++) { if (m) buf_puts(&b, ", "); emit_ctype(c, ci->ivar_types[m], &b); }
      if (ci->nivars == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    else {
      int icid = i;
      int init = comp_method_in_chain(c, i, "initialize", &icid);
      const char *star = ci->is_value_type ? "" : "*";
      if (init >= 0 && c->scopes[init].nparams > 0) {
        buf_printf(&b, "static sp_%s %ssp_%s_new(", ci->name, star, ci->name);
        Scope *s = &c->scopes[init];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          LocalVar *p = scope_local(s, s->pnames[m]);
          TyKind pm = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
          emit_ctype(c, pm, &b);
        }
        buf_puts(&b, ");\n");
      }
      else buf_printf(&b, "static sp_%s %ssp_%s_new(void);\n", ci->name, star, ci->name);
    }
  }
  if (c->nscopes > 1 || c->nclasses > 0) buf_puts(&b, "\n");

  /* global variables and top-level constants (file-scope statics) -- emitted
     ahead of the proc functions so a proc body may reference them by name. */
  for (int i = 0; i < c->ngvars; i++) {
    LocalVar *lv = &c->gvars[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " gv_%s = %s;\n", lv->name,
               (lv->type == TY_RANGE || lv->type == TY_POLY) ? "{0}" : default_value(lv->type));
  }
  for (int i = 0; i < c->nconsts; i++) {
    LocalVar *lv = &c->consts[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " cst_%s = %s;\n", lv->name,
               (lv->type == TY_RANGE || lv->type == TY_POLY) ? "{0}" : default_value(lv->type));
    if (lv->init_guarded) buf_printf(&b, "static int sp_init_in_progress_%s;\n", lv->name);
  }
  if (c->ngvars || c->nconsts) buf_puts(&b, "\n");

  /* GC marking for the file-scope statics above: heap objects reachable
     only through a global/constant/class-ivar slot would otherwise be
     swept (RAND = Rand.new lost its PRNG mid-render). Chained ahead of
     the runtime's own sp_re_mark_globals via the hook in sp_re_init. */
  {
    buf_puts(&b, "static void sp_mark_user_globals(void) {\n");
    buf_puts(&b, "  sp_re_mark_globals();\n");
    for (int i = 0; i < c->ngvars; i++) {
      LocalVar *lv = &c->gvars[i];
      if (!is_scalar_ret(lv->type)) continue;
      if (lv->type == TY_STRING) buf_printf(&b, "  sp_mark_string(gv_%s);\n", lv->name);
      else if (lv->type == TY_POLY) buf_printf(&b, "  sp_mark_rbval(gv_%s);\n", lv->name);
      else if (needs_root(lv->type)) buf_printf(&b, "  if (gv_%s) sp_gc_mark((void *)gv_%s);\n", lv->name, lv->name);
    }
    for (int i = 0; i < c->nconsts; i++) {
      LocalVar *lv = &c->consts[i];
      if (!is_scalar_ret(lv->type)) continue;
      if (lv->type == TY_STRING) buf_printf(&b, "  sp_mark_string(cst_%s);\n", lv->name);
      else if (lv->type == TY_POLY) buf_printf(&b, "  sp_mark_rbval(cst_%s);\n", lv->name);
      else if (needs_root(lv->type)) buf_printf(&b, "  if (cst_%s) sp_gc_mark((void *)cst_%s);\n", lv->name, lv->name);
    }
    for (int i = 0; i < c->nclasses; i++) {
      ClassInfo *ci = &c->classes[i];
      for (int j = 0; j < ci->nivars; j++) {
        TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
        const char *iv = ci->ivars[j] + 1;
        if (t == TY_STRING) buf_printf(&b, "  sp_mark_string(civ_%s_%s);\n", ci->name, iv);
        else if (t == TY_POLY) buf_printf(&b, "  sp_mark_rbval(civ_%s_%s);\n", ci->name, iv);
        else if (needs_root(t)) buf_printf(&b, "  if (civ_%s_%s) sp_gc_mark((void *)civ_%s_%s);\n", ci->name, iv, ci->name, iv);
      }
      for (int j = 0; j < ci->nsg_readers; j++)
        buf_printf(&b, "  sp_mark_rbval(sg_%s_%s);\n", ci->name, ci->sg_readers[j]);
    }
    buf_puts(&b, "}\n\n");
  }

  /* Per-class Ractor object codec — emitted after the class structs/scans and
     before sp_re_init installs the hooks. */
  emit_ractor_obj_codec(c, &b);

  /* Constructor defs, method defs, and main go into a separate buffer. Any
     proc literals they contain accumulate static functions into g_procs /
     g_proc_protos; we splice those in ahead of these bodies, since a proc
     function must be declared before the body that references it. */
  Buf body; memset(&body, 0, sizeof body);
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      emit_class_new(c, &c->classes[i], &body);
  for (int s = 1; s < c->nscopes; s++) {
    if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s) || c->scopes[s].is_transplanted_source) continue; emit_method(c, &c->scopes[s], &body);
  }

  /* Emit END block static functions for atexit registration */
  int end_count = 0;
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && !strcmp(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || strcmp(sty, "PostExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        end_count++;
        buf_printf(&body, "static void sp_end_fn_%d(void) { SP_GC_SAVE();\n", end_count);
        emit_stmts(c, stmts, &body, 1);
        buf_puts(&body, "}\n");
      }
    }
  }

  buf_puts(&body, "int main(int argc,char**argv){\n");
  buf_puts(&body, "    SP_GC_SAVE();\n");
  buf_puts(&body, "    sp_re_init();\n");
  buf_puts(&body, "    { sp_argv.len = argc - 1; sp_argv.data = (const char**)malloc(sizeof(const char*) * (size_t)(argc > 1 ? argc - 1 : 1)); for (int _ai = 0; _ai < argc - 1; _ai++) sp_argv.data[_ai] = sp_str_dup_external(argv[_ai + 1]); }\n");
  buf_puts(&body, "    sp_program_name = argc > 0 ? argv[0] : \"\";\n");
  /* Enable the backtrace substrate (Exception#backtrace, Kernel#caller) in
     debug builds only: --debug compiles at -O0 with non-inlined methods, so
     the captured frames demangle to Class#method. Optimized/release builds
     leave sp_bt_enabled = 0 (frames inline away), keeping the empty-array
     behavior. */
  if (getenv("SPINEL_DEBUG")) {
    buf_puts(&body, "    sp_bt_enabled = 1;\n");
    buf_puts(&body, "    sp_bt_srcfile = ");
    emit_str_literal(&body, c->nt->source_file ? c->nt->source_file : "source.rb");
    buf_puts(&body, ";\n");
  }
  /* Ruby auto-seeds its PRNG at startup, so rand/shuffle/sample vary per
     run. Seed once here; an explicit srand(seed) in user code runs later
     and overrides this for reproducible sequences. */
  buf_puts(&body, "    srand((unsigned)time(NULL));\n");
  /* Register END blocks (atexit runs LIFO, so they execute in reverse registration order) */
  for (int e = 1; e <= end_count; e++)
    buf_printf(&body, "    atexit(sp_end_fn_%d);\n", e);
  emit_scope_decls(c, &c->scopes[0], &body);
  buf_puts(&body, "\n");
  /* Hoist BEGIN blocks to run first */
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && !strcmp(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || strcmp(sty, "PreExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        emit_stmts(c, stmts, &body, 1);
      }
    }
  }
  emit_stmts(c, c->scopes[0].body, &body, 1);
  if (g_needs_at_exit)
    buf_puts(&body, "  { mrb_int _ax_args[16] = {0}; for (mrb_int _ax = sp_at_exit_count - 1; _ax >= 0; _ax--) sp_proc_call(sp_at_exit_hooks[_ax], 0, _ax_args); }\n");
  buf_puts(&body, "  return 0;\n}\n");

  emit_regex_section(&b);
  if (g_proc_protos.len) { buf_puts(&b, g_proc_protos.p); buf_puts(&b, "\n"); }
  if (g_procs.len) { buf_puts(&b, g_procs.p); buf_puts(&b, "\n"); }
  buf_puts(&b, body.p ? body.p : "");
  free(body.p);
  free(g_procs.p); free(g_proc_protos.p);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_needs_proc_poly_retslot = 0;

  comp_free(c);
  return b.p;
}

