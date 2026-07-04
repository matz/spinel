#include "codegen_internal.h"

void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  /* An untyped or void value is already emitted as a boxed sp_RbVal (a nil
     sentinel or poly call result) -- or carries side effects to preserve. Box
     it by evaluating for effect and yielding nil, mirroring emit_boxed. Without
     this, the int-boxing fallback below produced sp_box_int(<sp_RbVal>) -- an
     mrb_int slot fed a boxed value (the recurring poly-box bug family). */
  if (t == TY_UNKNOWN || t == TY_VOID || t == TY_REGEX) { buf_printf(b, "(%s, sp_box_nil())", expr); return; }
  if (t == TY_EXCEPTION) { buf_printf(b, "sp_box_obj(%s, SP_BUILTIN_EXCEPTION)", expr); return; }
  if (t == TY_FIBER) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_FIBER)", expr); return; }
  if (t == TY_THREAD) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_THREAD)", expr); return; }
  if (t == TY_QUEUE) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_QUEUE)", expr); return; }
  if (t == TY_MUTEX) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_MUTEX)", expr); return; }
  if (t == TY_CONDVAR) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_CONDVAR)", expr); return; }
  if (t == TY_ENUMERATOR) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_ENUMERATOR)", expr); return; }
  if (t == TY_IO) { buf_printf(b, "sp_box_obj((void *)(%s), SP_BUILTIN_IO)", expr); return; }
  if (ty_is_object(t)) { buf_printf(b, "sp_box_obj(%s, %d)", expr, ty_object_class(t)); return; }
  if (ty_is_hash(t) && hash_box_cls(t)) { buf_printf(b, "sp_box_obj(%s, %s)", expr, hash_box_cls(t)); return; }
  const char *fn = NULL;
  switch (t) {
    case TY_INT: fn = "sp_box_int"; break;       case TY_FLOAT: fn = "sp_box_float"; break;
    case TY_BIGINT: fn = "sp_box_bigint"; break;
    case TY_STRING: fn = "sp_box_str"; break;     case TY_BOOL: fn = "sp_box_bool"; break;
    case TY_SYMBOL: fn = "sp_box_sym"; break;     case TY_RANGE: fn = "sp_box_range"; break;
    case TY_TIME: fn = "sp_box_time"; break;
    case TY_COMPLEX: fn = "sp_box_complex"; break;  case TY_RATIONAL: fn = "sp_box_rational"; break;
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
  else {
    /* Never silently int-box an unexpected type (the root of the poly-box bug
       family): fail loudly so a missing case is caught at compile time rather
       than emitting wrong C. The known poly-context types are handled above. */
    fprintf(stderr, "spinel: emit_boxed_text: cannot box type %d into a poly value\n", (int)t);
    exit(1);
  }
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
    case TY_BIGINT: buf_printf(b, "(sp_Bigint *)(%s).v.p", expr); return;
    default: break;
  }
  if (t == TY_TIME) { buf_printf(b, "(*(sp_Time *)(%s).v.p)", expr); return; }  /* boxed by-value copy */
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

/* Emit a node as a `const char *` string. A poly value (e.g. a `String | nil`
   local narrowed to String by `is_a?(String)`, which keeps an sp_RbVal
   representation) is unboxed via sp_poly_to_s; a string-typed value emits
   directly. Used at string-primitive arg boundaries (sp_str_include, ...). */
void emit_str_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_s("); emit_expr(c, node, b); buf_puts(b, ")");
  }
  else emit_expr(c, node, b);
}

void emit_boxed(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY) { emit_expr(c, node, b); return; }
  if (t == TY_FIBER) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_FIBER)");
    return;
  }
  if (t == TY_THREAD) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_THREAD)");
    return;
  }
  if (t == TY_QUEUE) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_QUEUE)");
    return;
  }
  if (t == TY_MUTEX) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_MUTEX)");
    return;
  }
  if (t == TY_CONDVAR) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_CONDVAR)");
    return;
  }
  if (t == TY_IO) {
    buf_puts(b, "sp_box_obj((void *)("); emit_expr(c, node, b); buf_puts(b, "), SP_BUILTIN_IO)");
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
  /* regex values can appear in poly context (multi-typed local); evaluate for
     side effects (e.g. Regexp.new(str)) and yield nil */
  if (t == TY_REGEX) { buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())"); return; }
  /* class/module value: box into poly */
  if (t == TY_CLASS) {
    buf_puts(b, "sp_box_class("); emit_expr(c, node, b); buf_puts(b, ")"); return;
  }
  /* an empty array literal [] has TY_UNKNOWN; box it as an empty PolyArray so
     it can hold any element type when stored into a poly slot */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "ArrayNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_poly_array(sp_PolyArray_new())"); return; }
  }
  /* a bare `Array.new` is TY_UNKNOWN like an empty `[]` (so push-promotion can
     narrow it), and its handler emits a sp_PolyArray *. When it is never pushed
     and lands in a poly slot, box that array -- otherwise the fallback below
     evaluates it for side effect and yields nil, dropping the array. */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "CallNode")) {
    const char *nm = nt_str(c->nt, node, "name");
    int rc = nt_ref(c->nt, node, "receiver");
    const char *rcn = rc >= 0 ? nt_str(c->nt, rc, "name") : NULL;
    int an = 0; int aN = nt_ref(c->nt, node, "arguments");
    if (aN >= 0) nt_arr(c->nt, node, "arguments", &an);
    if (nm && sp_streq(nm, "new") && rcn && sp_streq(rcn, "Array") &&
        an == 0 && nt_ref(c->nt, node, "block") < 0) {
      buf_puts(b, "sp_box_poly_array("); emit_expr(c, node, b); buf_puts(b, ")"); return;
    }
  }
  /* an empty hash literal {} has TY_UNKNOWN; box it as an empty PolyPolyHash */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "HashNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"); return; }
  }
  const char *fn = NULL;
  switch (t) {
    /* A nullable-int sentinel (SP_INT_NIL) only flows into a poly box under
       --int-overflow=promote (where int? widens to poly); in default/wrap mode
       a real int is never the sentinel, so skip the per-box check there -- it is
       on the hot path (every int boxed into poly, e.g. optcarrot's pixels). */
    case TY_INT:    fn = g_promote_mode ? "sp_box_int_or_nil" : "sp_box_int"; break;
    case TY_FLOAT:  fn = "sp_box_float"; break;
    case TY_BIGINT: fn = "sp_box_bigint"; break;
    case TY_STRING: fn = "sp_box_str";   break;
    case TY_BOOL:   fn = "sp_box_bool";  break;
    case TY_SYMBOL: fn = "sp_box_sym";   break;
    case TY_RANGE:  fn = "sp_box_range"; break;
    case TY_TIME:   fn = "sp_box_time";  break;
    case TY_COMPLEX:  fn = "sp_box_complex";  break;
    case TY_RATIONAL: fn = "sp_box_rational"; break;
    case TY_PROC:   fn = "sp_box_proc";  break;
    case TY_METHOD: fn = "sp_box_method"; break;
    case TY_INT_ARRAY:   fn = "sp_box_int_array";   break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY:   fn = "sp_box_str_array";   break;
    case TY_POLY_ARRAY:  fn = "sp_box_poly_array";  break;
    case TY_NIL: {
      const char *nty = nt_type(c->nt, node);
      if (nty && sp_streq(nty, "NilNode")) { buf_puts(b, "sp_box_nil()"); return; }
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

/* A `loop { }` call emits a setjmp (to rescue StopIteration and terminate), so
   for the volatile analysis it behaves like a begin: a local written in its body
   and read after must survive the longjmp. */
static int is_stopiter_loop(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  if (nt_ref(nt, id, "receiver") >= 0) return 0;
  const char *nm = nt_str(nt, id, "name");
  return nm && sp_streq(nm, "loop") && nt_ref(nt, id, "block") >= 0;
}

/* Does scope index `si` contain a begin/rescue or a `loop {}` (so its locals
   need volatile across the setjmp it emits)? */
int scope_has_begin(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++) {
    if (c->nscope[id] != si) continue;
    const char *ty = nt_type(c->nt, id);
    if (ty && (sp_streq(ty, "BeginNode") || sp_streq(ty, "RescueNode")))
      return 1;
    if (is_stopiter_loop(c, id)) return 1;
  }
  return 0;
}

/* Mark every node id in the subtree rooted at `id` (ref fields + array-field
   elements are a node's children). Used to find the lexical extent of a
   begin/rescue construct. */
static void mark_subtree(const NodeTable *nt, int id, char *inb) {
  if (id < 0 || id >= nt->count || inb[id]) return;
  inb[id] = 1;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) mark_subtree(nt, nt_ref_at(nt, id, i), inb);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n; const int *a = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++) mark_subtree(nt, a[j], inb);
  }
}

static int lv_is_write_or_target(NodeKind k) {
  return k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode ||
         k == NK_LocalVariableAndWriteNode || k == NK_LocalVariableOperatorWriteNode ||
         k == NK_LocalVariableTargetNode;
}

/* Which locals in scope `si` need `volatile`? A `begin` emits a setjmp at its
   entry; per C99 7.13.2.1 only a local modified between that setjmp and a
   longjmp (a raise, or a retry) and read afterward is indeterminate -- i.e. a
   local *written inside the begin construct*. Locals written only outside it
   keep their setjmp-time value and need no volatile (the broad whole-scope
   qualifier this replaces was sound but pessimized hot loops that merely sit in
   the same method as an unrelated begin).

   Returns the list of such names via *out/*nout (names borrow the node table's
   storage; the array is the caller's to free). Sets *all = 1 when a bare
   (method-level) RescueNode -- one not nested in any BeginNode -- protects the
   whole body, in which case every local needs volatile and *out stays NULL. */
static void begin_volatile_names(Compiler *c, int si, char ***out, int *nout, int *all) {
  const NodeTable *nt = c->nt;
  *out = NULL; *nout = 0; *all = 0;
  char *inb = (char *)calloc((size_t)(nt->count > 0 ? nt->count : 1), 1);
  if (!inb) { *all = 1; return; }  /* OOM: fall back to the conservative whole-scope rule */
  for (int id = 0; id < nt->count; id++)
    if (((nt_kind(nt, id) == NK_BeginNode) || is_stopiter_loop(c, id)) && c->nscope[id] == si) mark_subtree(nt, id, inb);
  for (int id = 0; id < nt->count; id++)
    if (nt_kind(nt, id) == NK_RescueNode && c->nscope[id] == si && !inb[id]) { *all = 1; break; }
  if (*all) { free(inb); return; }
  char **names = NULL; int n = 0, cap = 0;
  for (int id = 0; id < nt->count; id++) {
    if (!inb[id] || c->nscope[id] != si || !lv_is_write_or_target(nt_kind(nt, id))) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int dup = 0;
    for (int j = 0; j < n; j++) if (sp_streq(names[j], nm)) { dup = 1; break; }
    if (dup) continue;
    if (n == cap) { cap = cap ? cap * 2 : 8; names = (char **)realloc(names, sizeof(char *) * cap); }
    names[n++] = (char *)nm;
  }
  *out = names; *nout = n;
  free(inb);
}

static int name_in(char **names, int n, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < n; i++) if (sp_streq(names[i], nm)) return 1;
  return 0;
}

/* Declare a scope's locals. Params are already C function parameters, so
   they only need a GC root; body locals get a full declaration. */
void emit_scope_decls(Compiler *c, Scope *s, Buf *b) {
  int si = (int)(s - c->scopes);
  int has_begin = scope_has_begin(c, si);
  char **volnames = NULL; int nvol = 0, all_vol = 0;
  if (has_begin) begin_volatile_names(c, si, &volnames, &nvol, &all_vol);
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *lv = &s->locals[i];
    /* define_method subst var: replaced inline by the literal, never a C
       local, so neither declare nor root it. */
    if (s->dm_subst_name && lv->name && sp_streq(lv->name, s->dm_subst_name)) continue;
    /* Virtual &block slot: skip declaration UNLESS it's a lowered __yblk__ that
       needs a cell (so forwarding procs can capture it). */
    if (s->blk_param && lv->name && sp_streq(lv->name, s->blk_param) && !lv->is_cell) continue;
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
      /* A float capture gets a native mrb_float cell rather than laundering the
         bits through the int slot: *_cell_x is then a real mrb_float lvalue, so
         the ordinary read / write / compound-assign paths work unchanged. The
         cell holds no GC pointer, so no cell scan is needed. */
      if (lv->type == TY_FLOAT) {
        buf_printf(b, "    mrb_float *_cell_%s = (mrb_float *)sp_gc_alloc(sizeof(mrb_float), NULL, NULL);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = 0.0;\n", lv->name);
        continue;
      }
      if (lv->type == TY_POLY) {
        buf_printf(b, "    sp_RbVal *_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = sp_box_nil();\n", lv->name);
        continue;
      }
      /* A pointer (string / array / hash / heap object) capture rides a real
         typed-pointer cell (`T *_cell_x`): deref is an ordinary lvalue, so both
         reads and reassignments work with no (mrb_int)(uintptr_t) cast, and the
         existing cell scan marks the referent. Int / bool stay direct in an
         mrb_int cell; float / poly have native cells above. */
      int ptr_cell = cell_is_typed_ptr(c, lv);
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN && !ptr_cell)
        unsupported(c, s->def_node, "closure capturing a non-integer variable (later slice)");
      if (ptr_cell) {
        const char *cell_scan = (lv->type == TY_STRING) ? "sp_cell_scan_str" : "sp_cell_scan_ptr";
        buf_puts(b, "    "); emit_ctype(c, lv->type, b);
        buf_printf(b, " *_cell_%s = (", lv->name); emit_ctype(c, lv->type, b);
        buf_puts(b, " *)sp_gc_alloc(sizeof("); emit_ctype(c, lv->type, b);
        buf_printf(b, "), NULL, %s);\n", cell_scan);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = NULL;\n", lv->name);
        continue;
      }
      buf_printf(b, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, NULL);\n", lv->name);
      buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
      if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
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
      int vol = has_begin && (all_vol || name_in(volnames, nvol, lv->name));
      declare_local(c, b, lv, vol);
    }
  }
  free(volnames);
}

/* ---- methods ---- */

int method_is_void(Scope *s) {
  /* initialize is always void (mutates *self); else by return type */
  if (s->class_id >= 0 && s->name && sp_streq(s->name, "initialize")) return 1;
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
  /* An instance method of a never-instantiated class has had its poly-dispatch
     arm dropped (compute_instantiated) and -- no instance ever existing -- has
     no direct call site either, so it is emitted but unreferenced. Mark it
     unused so the C compiler does not -Wunused-function before DCEing the
     orphan. Harmless if it turns out referenced (an instantiated subclass
     inheriting it). The attribute precedes the declarator so it is valid on the
     definition, not just the prototype. */
  const char *unused = (s->class_id >= 0 && !s->is_cmethod &&
                        !c->classes[s->class_id].instantiated)
                       ? "__attribute__((unused)) " : "";
  if (method_is_void(s)) { buf_puts(b, stor); buf_puts(b, unused); buf_puts(b, "void "); }
  else { buf_puts(b, stor); buf_puts(b, unused); emit_ctype(c, s->ret, b); buf_puts(b, " "); }
  emit_method_cname(c, s, b);
  buf_puts(b, "(");
  int wrote = 0;
  if (s->class_id >= 0 && !s->is_cmethod) {
    const char *cn = c->classes[s->class_id].name;
    if (sp_streq(cn, "String"))       { buf_puts(b, "const char *self"); }
    else if (sp_streq(cn, "Integer")) { buf_puts(b, "mrb_int self"); }
    else if (sp_streq(cn, "Float"))   { buf_puts(b, "double self"); }
    else if (sp_streq(cn, "Symbol"))  { buf_puts(b, "mrb_int self"); }
    else if (sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass") || sp_streq(cn, "NilClass")) { buf_puts(b, "int self"); }
    else if (sp_streq(cn, "Array"))   { buf_puts(b, "sp_RbVal self"); }
    else if (sp_streq(cn, "Object") || sp_streq(cn, "Numeric")) { buf_puts(b, "sp_RbVal self"); }
    else {
      /* value-type reader methods take self by value; initialize keeps a
         pointer so it can populate the fields during construction. */
      int vt = c->classes[s->class_id].is_value_type;
      int is_init = s->name && sp_streq(s->name, "initialize");
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
      if (sp_streq(k, "str")) emit_str_literal(b, "");
      else if (sp_streq(k, "sa")) buf_puts(b, "sp_StrArray_new()");
      else if (sp_streq(k, "ia")) buf_puts(b, "sp_IntArray_new()");
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
      if (!sp_streq(ci->cs_kinds[i], want)) continue;
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
  /* Map the whole function (signature + SP_GC_SAVE prologue + local decls,
     before the first body stmt) to the `def` line, so a breakpoint on the method
     lands on the .rb source rather than the generated C -- which is deleted after
     compile, so gdb couldn't find it. With this, --line-map / -g is enough to
     debug against the Ruby source; no need to keep the generated C (#1261). */
  emit_line_directive(c, s->def_node, b);
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
  /* a method body is a fresh break context: a stray enclosing serial must
     not leak into it (its own wrapped iterators re-establish scopes) */
  const char *saved_bser = g_brk_ser_var; g_brk_ser_var = NULL;
  int saved_bskip = g_brk_skip_id; g_brk_skip_id = -1;
  /* value-type reader methods receive self by value, so ivar access uses `.` */
  const char *saved_deref = g_self_deref;
  g_self_deref = (s->class_id >= 0 && !s->is_cmethod && c->classes[s->class_id].is_value_type &&
                  s->name && !sp_streq(s->name, "initialize")) ? "." : "->";
  g_ret_type = method_is_void(s) ? TY_VOID : s->ret;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0;
  int is_void = method_is_void(s);
  /* A method that creates a non-lambda proc with a `return` owns a proc-return
     frame: a setjmp target the proc longjmps to. All returns funnel through a
     single exit (_pr_done) that pops the frame, so the setjmp buffer is never
     left live past the method. */
  int si = (int)(s - c->scopes);
  int pr_frame = !s->is_lowered_yield && scope_creates_returning_proc(c, si);
  /* The setjmp catch returns directly (no goto) so it never jumps over a later
     GC-root cleanup local; the body runs inside a `{ }` block so a funnel
     `goto _pr_done` only ever exits that block (running its cleanups). On the
     longjmp path SP_GC_SAVE's cleanup restores the GC roots when the catch
     returns. */
  if (pr_frame) {
    /* Push a home node onto the per-fiber proc-return chain (CRuby tag-chain
       style): the node lives on this method's C stack, its fresh id is captured
       by the returning procs it creates, and every exit (setjmp catch + _pr_done)
       unlinks it. val starts nil so a GC before any return marks nothing. */
    buf_puts(b, "    sp_proc_home _h;\n");
    buf_puts(b, "    _h.val = sp_box_nil(); _h.id = sp_proc_home_next();\n");
    buf_puts(b, "    _h.exc_top = sp_exc_top; _h.catch_top = sp_catch_top;\n");
    buf_puts(b, "    _h.prev = sp_proc_ret_head; sp_proc_ret_head = &_h;\n");
    if (!is_void) {
      buf_puts(b, "    "); emit_ctype(c, s->ret, b); buf_puts(b, " _prret = ");
      if (ty_is_object(s->ret) && !comp_ty_value_obj(c, s->ret)) buf_puts(b, "NULL");
      else buf_puts(b, default_value(s->ret));
      buf_puts(b, ";\n");
      /* the longjmp-home delivery also restores sp_catch_top: a return out of a
         catch block inside the home (or a callee) must not leak its catch slot. */
      buf_puts(b, "    if (setjmp(_h.jb)) { sp_proc_ret_head = _h.prev; sp_catch_top = _h.catch_top; return ");
      emit_unbox_text(c, s->ret, "_h.val", b);
      buf_puts(b, "; }\n");
    }
    else {
      buf_puts(b, "    if (setjmp(_h.jb)) { sp_proc_ret_head = _h.prev; sp_catch_top = _h.catch_top; return; }\n");
    }
    buf_puts(b, "    {\n");
    g_method_pr_label = "_pr_done"; g_method_pr_var = is_void ? NULL : "_prret";
    g_method_pr_exc_depth = 0;   /* _pr_done sits outside every begin frame */
  }
  const char *sv_rv2 = g_result_var; int sv_rp2 = g_result_poly;
  if (pr_frame && !is_void) { g_result_var = "_prret"; g_result_poly = (s->ret == TY_POLY); }

  if (is_void) {
    emit_stmts(c, s->body, b, 1);
    if (pr_frame) { buf_puts(b, "    }\n  _pr_done: ;\n  sp_proc_ret_head = _h.prev;\n"); }
  }
  else if (pr_frame) {
    emit_stmts_tail(c, s->body, b, 1);
    g_result_var = sv_rv2; g_result_poly = sv_rp2;
    buf_puts(b, "    }\n  _pr_done: ;\n  sp_proc_ret_head = _h.prev;\n  return _prret;\n");
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
  g_result_var = sv_rv2; g_result_poly = sv_rp2;
  g_method_pr_label = NULL; g_method_pr_var = NULL;
  g_self_deref = saved_deref;
  g_ret_type = saved_rt; g_ensure_depth = saved_ed;
  g_emitting_class_id = saved_emcls;
  g_dm_subst_name = saved_dmn; g_dm_subst_node = saved_dmnode;
  g_current_scope_is_lowered = saved_lowered;
  g_brk_ser_var = saved_bser; g_brk_skip_id = saved_bskip;
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
  return ty && (sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode"));
}

/* Collect the local names WRITTEN in the proc body subtree (the proc's own
   locals), not descending into nested blocks. */
void proc_collect_locals(Compiler *c, int id, NameSet *locals) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableTargetNode") ||
      sp_streq(ty, "LocalVariableOperatorWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
      sp_streq(ty, "LocalVariableAndWriteNode"))
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

/* Collect the parameter names declared by a block/lambda node (requireds,
   optionals, posts, rest). Used to declare a nested block's params in the
   flat fiber-body C function it is inlined into. */
static void collect_block_param_names(Compiler *c, int blk, NameSet *out) {
  const NodeTable *nt = c->nt;
  int bp_node = nt_ref(nt, blk, "parameters");
  if (bp_node < 0) return;
  int inner = nt_ref(nt, bp_node, "parameters");
  int pn = inner >= 0 ? inner : bp_node;
  if (pn < 0) return;
  int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
  for (int i = 0; i < rn; i++) { const char *nm = nt_str(nt, reqs[i], "name"); if (nm) nameset_add(out, nm); }
  int on = 0; const int *opts = nt_arr(nt, pn, "optionals", &on);
  for (int i = 0; i < on; i++) { const char *nm = nt_str(nt, opts[i], "name"); if (nm) nameset_add(out, nm); }
  int psn = 0; const int *posts = nt_arr(nt, pn, "posts", &psn);
  for (int i = 0; i < psn; i++) { const char *nm = nt_str(nt, posts[i], "name"); if (nm) nameset_add(out, nm); }
  int rest = nt_ref(nt, pn, "rest");
  if (rest >= 0) { const char *nm = nt_str(nt, rest, "name"); if (nm) nameset_add(out, nm); }
}

/* Collect every local name DEFINED inside a proc/fiber body INCLUDING nested
   blocks: local-variable writes plus the parameters of nested blocks. A nested
   block (`3.times { |i| ... }`) is inlined into the body's flat C function, so
   its param `i` and any locals it writes must be declared there and must be
   classified as body-local (not as a captured enclosing var). Unlike
   proc_collect_locals this descends through nested blocks. */
static void collect_locals_deep(Compiler *c, int id, NameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (sp_streq(ty, "LocalVariableWriteNode") || sp_streq(ty, "LocalVariableTargetNode") ||
      sp_streq(ty, "LocalVariableOperatorWriteNode") || sp_streq(ty, "LocalVariableOrWriteNode") ||
      sp_streq(ty, "LocalVariableAndWriteNode"))
    nameset_add(out, nt_str(c->nt, id, "name"));
  if (is_nested_block(ty)) collect_block_param_names(c, id, out);
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) collect_locals_deep(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) collect_locals_deep(c, ids[k], out); }
}

/* Collect all local names used (read or written) anywhere in the proc/fiber
   body, INCLUDING nested blocks (which are inlined into the same flat C
   function). The caller classifies each as the proc's own param/local, a
   nested block's local, or a captured enclosing var (is_cell). Mirrors the
   analyze-side a_collect_used so codegen captures match the is_cell marking. */
void proc_collect_used(Compiler *c, int id, NameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode"))
    nameset_add(out, nt_str(c->nt, id, "name"));
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) proc_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) proc_collect_used(c, ids[k], out); }
}

/* The ParametersNode of a proc-creating node. A `->{}` LambdaNode carries it
   directly (`parameters`); a `proc {}` / `lambda {}` / block-escape pass-through
   nests it one level deeper (block/BlockNode -> BlockParametersNode -> ParametersNode). */
int proc_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  if (ty && sp_streq(ty, "LambdaNode")) return nt_ref(c->nt, create, "parameters");
  /* BlockNode used directly as a proc (escaped &block) */
  if (ty && sp_streq(ty, "BlockNode")) {
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
  if (ty && sp_streq(ty, "LambdaNode")) return nt_ref(c->nt, create, "body");
  if (ty && sp_streq(ty, "BlockNode")) return nt_ref(c->nt, create, "body");
  int block = nt_ref(c->nt, create, "block");
  return block >= 0 ? nt_ref(c->nt, block, "body") : -1;
}

/* Proc args + return ride the mrb_int slot of sp_proc_call. A value that fits
   an mrb_int directly (int/bool/symbol/nil) needs no conversion; a heap pointer
   (string/array/hash/object) is laundered through (mrb_int)(uintptr_t). Other
   shapes (float, poly, range, time) don't fit the slot and defer. */
int proc_slot_is_direct(TyKind t) { return t == TY_INT || t == TY_BOOL || t == TY_SYMBOL || t == TY_NIL || t == TY_UNKNOWN; }
int proc_slot_is_ptr(TyKind t) { return t == TY_STRING || ty_is_array(t) || ty_is_hash(t) || ty_is_object(t); }

/* True if a closure cell for `lv` carries the variable's real typed pointer
   (string / array / hash / object), as opposed to a laundered or scalar slot.
   A typed-pointer cell is a plain `T *_cell_x` whose deref is an ordinary
   lvalue, so reads and (re)assignments need no (mrb_int)(uintptr_t) cast. */
int cell_is_typed_ptr(Compiler *c, LocalVar *lv) {
  return lv && proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
}

/* Emit the C element type of `lv`'s closure cell (the cell itself is a pointer
   to this). Proc cells launder sp_Proc* through mrb_int; int/bool ride mrb_int;
   float/poly have native cells; a heap object uses its real pointer type. */
void emit_cell_elem_type(Compiler *c, LocalVar *lv, Buf *b) {
  if (lv && lv->type == TY_FLOAT) { buf_puts(b, "mrb_float"); return; }
  if (lv && lv->type == TY_POLY) { buf_puts(b, "sp_RbVal"); return; }
  if (cell_is_typed_ptr(c, lv)) { emit_ctype(c, lv->type, b); return; }
  buf_puts(b, "mrb_int");
}

/* True if the AST subtree at `id` has a YieldNode, not crossing DefNode. */
int proc_body_has_yield(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "YieldNode")) return 1;
  if (sp_streq(ty, "DefNode")) return 0;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (proc_body_has_yield(c, ch)) return 1; }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (proc_body_has_yield(c, ids[k])) return 1; }
  return 0;
}

/* When walking a proc body for a `return`, recurse from `id` into child `ch`?
   Descend into an INLINED iteration block (a BlockNode owned by an ordinary
   method call) so a `return` there is seen as non-local to the home method --
   but NOT into a nested proc/lambda literal (whose `return` is its own). A
   BlockNode is only ever the `block` ref of its owner, so `id` is that owner;
   skip the block when the owner is a proc/lambda literal. LambdaNode/DefNode
   children are stopped by proc_body_has_return's own type checks. */
static int proc_return_descend(Compiler *c, int id, int ch) {
  const char *t = nt_type(c->nt, ch);
  if (!t) return 0;
  if (sp_streq(t, "BlockNode")) return !is_proc_literal(c, id);
  return 1;
}

/* True if the proc body subtree contains a `return` that belongs to this proc:
   its own returns, plus returns inside inlined iteration blocks (those are also
   non-local to the home method). Does not descend into a nested proc/lambda
   literal or a def, whose returns are their own. */
int proc_body_has_return(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "ReturnNode")) return 1;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "LambdaNode")) return 0;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0 && proc_return_descend(c, id, ch) && proc_body_has_return(c, ch)) return 1; }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0 && proc_return_descend(c, id, ids[k]) && proc_body_has_return(c, ids[k])) return 1; }
  return 0;
}

/* A `proc {}` / `Proc.new {}` literal (non-lambda) whose body does a `return`:
   that `return` must return from the method that created the proc, so the proc
   longjmps to the home method's proc-return frame instead of returning locally.
   Lambdas and bare blocks are excluded (their `return` is local / inlined). */
int proc_does_nonlocal_return(Compiler *c, int create) {
  const char *cty = nt_type(c->nt, create);
  if (!cty || !sp_streq(cty, "CallNode")) return 0;          /* proc/Proc.new are calls */
  const char *cn = nt_str(c->nt, create, "name");
  if (!cn) return 0;
  int recv = nt_ref(c->nt, create, "receiver");
  int is_proc = (recv < 0 && sp_streq(cn, "proc"));
  int is_proc_new = (sp_streq(cn, "new") && recv >= 0 &&
                     nt_type(c->nt, recv) && sp_streq(nt_type(c->nt, recv), "ConstantReadNode") &&
                     nt_str(c->nt, recv, "name") && sp_streq(nt_str(c->nt, recv, "name"), "Proc"));
  if (!is_proc && !is_proc_new) return 0;
  if (nt_ref(c->nt, create, "block") < 0) return 0;
  return proc_body_has_return(c, proc_body_node(c, create));
}

/* True if scope `si` (a method) lexically creates a returning proc, so the
   method must set up a proc-return frame. Blocks/procs share their method's
   scope, so a returning proc's create node is nscope == si. */
int scope_creates_returning_proc(Compiler *c, int si) {
  for (int id = 0; id < c->nt->count; id++)
    if (c->nscope[id] == si && proc_does_nonlocal_return(c, id)) return 1;
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
         ty_is_object(t) || t == TY_POLY || t == TY_PROC || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR ||
         t == TY_EXCEPTION || t == TY_STRINGIO || t == TY_STRINGSCANNER ||
         t == TY_MATCHDATA || t == TY_REGEX || t == TY_TIME;
}

/* Returns 1 if the fiber body accesses instance state (ivars, self, or
   implicit self-dispatch calls) without crossing into nested blocks/lambdas. */
int fiber_body_uses_self(Compiler *c, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "InstanceVariableReadNode") || sp_streq(ty, "InstanceVariableWriteNode") ||
      sp_streq(ty, "InstanceVariableOperatorWriteNode") ||
      sp_streq(ty, "InstanceVariableOrWriteNode") || sp_streq(ty, "InstanceVariableAndWriteNode") ||
      sp_streq(ty, "SelfNode")) return 1;
  if (sp_streq(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) return 1;
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

void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) { buf_puts(b, as_gen ? "sp_Enumerator_new_gen(NULL, NULL)" : "sp_Fiber_new(NULL)"); return; }

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

  /* Locals to declare in the flat fiber-body C function: the body's own
     locals PLUS the params/locals of any nested blocks inlined into it
     (a nested `3.times { |i| ... }` needs `i` declared here). */
  NameSet fib_decls = {0};
  if (body >= 0) collect_locals_deep(c, body, &fib_decls);

  /* Compute captures: names used in the body that belong to the enclosing scope
     but are NOT defined by the fiber body itself and NOT the block param. */
  NameSet fib_used = {0};
  if (body >= 0) proc_collect_used(c, body, &fib_used);

  /* caps: outer-scope vars referenced in the body. A var written in the body is
     normally a fiber-body local (not captured) -- EXCEPT a celled one, which is
     captured by its shared heap cell pointer so the write reaches the enclosing
     scope (matching escaping-proc capture). */
  NameSet caps = {0};
  if (encl) {
    for (int u = 0; u < fib_used.n; u++) {
      const char *nm = fib_used.v[u];
      if (bp0 && sp_streq(nm, bp0)) continue;
      LocalVar *lv = scope_local(encl, nm);
      if (!lv || lv->type == TY_UNKNOWN) continue;
      /* A name defined in the body (including a nested block's param/local) is
         a body-local, not a capture -- unless it's a celled enclosing local
         (then the write must reach the outer scope through the cell). */
      if (nameset_has(&fib_decls, nm) && !lv->is_cell) continue;
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
      if (lv && lv->is_cell) {
        /* a shared cell pointer (see emit_scope_decls): float -> mrb_float*,
           poly -> sp_RbVal*, heap object -> its typed pointer, else mrb_int*. */
        buf_puts(&g_proc_protos, " "); emit_cell_elem_type(c, lv, &g_proc_protos);
        buf_printf(&g_proc_protos, " *%s;", caps.v[i]);
      }
      else {
        TyKind ct = lv ? lv->type : TY_POLY;
        buf_printf(&g_proc_protos, " "); emit_ctype(c, ct, &g_proc_protos);
        buf_printf(&g_proc_protos, " %s;", caps.v[i]);
      }
    }
    buf_printf(&g_proc_protos, " } _fib_cap_%d;\n", fid);
    buf_printf(&g_proc_protos, "static void _fib_cap_scan_%d(void *p) {\n", fid);
    buf_printf(&g_proc_protos, "  sp_gc_mark(p);\n");
    buf_printf(&g_proc_protos, "  _fib_cap_%d *_c = (_fib_cap_%d *)p;\n", fid, fid);
    if (cap_self) buf_printf(&g_proc_protos, "  if (_c->self_ptr) sp_gc_mark((void *)_c->self_ptr);\n");
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      TyKind ct = lv ? lv->type : TY_POLY;
      if (lv && lv->is_cell) {
        buf_printf(&g_proc_protos, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
      }
      else if (fiber_cap_needs_root(ct)) {
        if (ct == TY_POLY) buf_printf(&g_proc_protos, "  sp_mark_rbval(_c->%s);\n", caps.v[i]);
        else buf_printf(&g_proc_protos, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
      }
    }
    buf_printf(&g_proc_protos, "}\n");
  }

  /* Emit fiber body function prototype before main bodies */
  buf_printf(&g_proc_protos, "static void %s(sp_Fiber *_fb);\n", fname);

  /* Emit the fiber body function into a LOCAL buffer, not directly into
     g_procs. A nested `Fiber.new` in this body re-enters emit_fiber_new while
     we are mid-emission; if both wrote to g_procs the inner function
     definition would land inside this one (an illegal C nested function).
     Building into a local buffer lets the inner body append its own complete
     definition to g_procs first; we append ours after it (both at file
     scope). */
  Buf body_buf = {0};
  Buf *pb = &body_buf;
  buf_printf(pb, "static void %s(sp_Fiber *_fb) {\n", fname);
  buf_puts(pb, "    SP_GC_SAVE();\n");

  /* Save global emission state */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  const char *sv_yld = g_yielder_name;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  g_pre = NULL; g_indent = 1; g_nren = 0; g_block_id = blk;
  g_block_param_name = bp0; g_self = sv_self;
  g_yielder_name = as_gen ? bp0 : NULL;   /* `y << v` -> Fiber.yield in the body */
  g_ret_type = TY_POLY; g_result_poly = 0; g_result_var = NULL;
  const char *sv_fbser = g_brk_ser_var; g_brk_ser_var = NULL;   /* fresh function context */
  int sv_fbskip = g_brk_skip_id; g_brk_skip_id = -1;
  int sv_fbexcd = g_exc_frame_depth, sv_fbprexcd = g_method_pr_exc_depth;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0;

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
      if (lv && lv->is_cell) {
        /* unpack the shared cell pointer; reads/writes go through (*_cell_<name>)
           (emit_local_ref), so the write reaches the enclosing scope. */
        buf_puts(pb, "    "); emit_cell_elem_type(c, lv, pb);
        buf_printf(pb, " *_cell_%s = _fc->%s;\n", caps.v[i], caps.v[i]);
        buf_printf(pb, "    SP_GC_ROOT(_cell_%s);\n", caps.v[i]);
        continue;
      }
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
      if (bp0 && sp_streq(lv->name, bp0)) continue;
      if (nameset_has(&caps, lv->name)) continue;
      if (!nameset_has(&fib_locals, lv->name) && !nameset_has(&fib_decls, lv->name)) continue;
      if (lv->type == TY_UNKNOWN) continue;
      declare_local(c, pb, lv, 0);
    }
  }
  free(fib_locals.v);
  free(fib_decls.v);

  /* A generator yields imperatively via `y << v`; its body value is discarded
     (running off the end terminates the fiber, which #next maps to
     StopIteration). Emit every statement plainly and leave yielded_value nil. */
  if (as_gen) {
    if (body >= 0) { int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn); for (int k = 0; k < bn; k++) emit_stmt(c, bb[k], pb, 1); }
    buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
  }
  /* Emit body: all-but-last as side-effect statements, last sets yielded_value.
     For void/nil last statements emit as stmt first then set yielded=nil. */
  else if (body >= 0) {
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

  /* Append the completed body to g_procs. Any nested fiber bodies emitted
     while building body_buf already appended themselves to g_procs, so they
     precede this definition there; both sit at file scope. */
  buf_puts(&g_procs, body_buf.p ? body_buf.p : "");
  free(body_buf.p);

  /* Restore emission state */
  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_ret_type = sv_rt;
  g_result_poly = sv_rp; g_result_var = sv_rv; g_yielder_name = sv_yld;
  g_brk_ser_var = sv_fbser; g_brk_skip_id = sv_fbskip;
  g_exc_frame_depth = sv_fbexcd; g_method_pr_exc_depth = sv_fbprexcd;

  /* Emit creation expression:
     If there are captures, allocate a GC-managed capture struct, fill it,
     assign to fiber->user_data, then return the fiber.
     Without captures: just sp_Fiber_new(fname). */
  if (ncap > 0 || cap_self) {
    int tc = ++g_tmp;
    /* For a generator the fiber is created lazily by the enumerator, so only the
       capture struct is built here and handed to sp_Enumerator_new_gen. */
    int tf = as_gen ? -1 : ++g_tmp;
    if (!as_gen) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_Fiber *_t%d = sp_Fiber_new(%s);\n", tf, fname);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tf);
    }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "_fib_cap_%d *_t%d = (_fib_cap_%d *)sp_gc_alloc(sizeof(_fib_cap_%d), NULL, _fib_cap_scan_%d);\n",
               fid, tc, fid, fid, fid);
    if (cap_self) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d->self_ptr = %s;\n", tc, sv_self ? sv_self : "self");
    }
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      emit_indent(g_pre, g_indent);
      if (lv && lv->is_cell)
        buf_printf(g_pre, "_t%d->%s = _cell_%s;\n", tc, caps.v[i], caps.v[i]);   /* the shared cell pointer */
      else
        buf_printf(g_pre, "_t%d->%s = lv_%s;\n", tc, caps.v[i], rename_local(caps.v[i]));
    }
    if (as_gen) {
      buf_printf(b, "sp_Enumerator_new_gen(%s, _t%d)", fname, tc);
    }
    else {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d->user_data = _t%d;\n", tf, tc);
      buf_printf(b, "_t%d", tf);
    }
  }
  else {
    buf_printf(b, as_gen ? "sp_Enumerator_new_gen(%s, NULL)" : "sp_Fiber_new(%s)", fname);
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
  if (sp_streq(ty, "SelfNode")) return 1;
  if (!strncmp(ty, "InstanceVariable", 16)) return 1;
  if (sp_streq(ty, "SuperNode") || sp_streq(ty, "ForwardingSuperNode")) return 1;
  if (sp_streq(ty, "CallNode") && nt_ref(c->nt, id, "receiver") < 0) {
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
  if (sp_streq(ty, "InstanceVariableWriteNode") || sp_streq(ty, "InstanceVariableOrWriteNode")) {
    int v = nt_ref(c->nt, id, "value");
    if (v >= 0) {
      const char *vt = nt_type(c->nt, v);
      if (vt && sp_streq(vt, "LocalVariableReadNode")) {
        const char *vn = nt_str(c->nt, v, "name");
        if (vn && sp_streq(vn, bp)) return 1;
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

/* Lower a `proc {}` / `lambda {}` / `Proc.new {}` / `->(){}` literal: emit a
   standalone `static mrb_int _proc_N(void *cap, mrb_int argc, mrb_int *args)`
   (sp_proc_call's ABI) into g_procs, and emit the boxing `sp_proc_new_meta(...)`
   value into `b`. */
void emit_proc_literal(Compiler *c, int create, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, create);
  int is_lambda_node = cty && sp_streq(cty, "LambdaNode");
  int is_block_node = cty && sp_streq(cty, "BlockNode");
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
  /* deep: include nested blocks' params/locals so a name used only inside a
     nested block in the proc is classified as body-local, not flagged as an
     uncaptured outer variable. */
  collect_locals_deep(c, body, &locals);
  /* A float proc parameter still rides the truncating mrb_int arg slot, so a
     proc that takes one can't safely also enable float captures: keep the clean
     reject rather than emit a silently-wrong double. (Float args through the
     proc slot are a separate, pre-existing gap.) */
  int has_float_param = 0;
  for (int k = 0; k < arity; k++) {
    LocalVar *plv = scope_local(bs, proc_param_name(c, create, k));
    if (plv && plv->type == TY_FLOAT) { has_float_param = 1; break; }
  }
  for (int u = 0; u < used.n; u++) {
    const char *nm = used.v[u];
    if (nameset_has(&params, nm)) continue;
    LocalVar *lv = scope_local(bs, nm);
    if (lv && lv->is_cell) {
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      int float_cell = lv->type == TY_FLOAT && !has_float_param;
      int poly_cell = lv->type == TY_POLY;
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_UNKNOWN &&
          lv->type != TY_PROC && !float_cell && !ptr_cell && !poly_cell) {
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
  int is_lambda = is_lambda_node || (cn && sp_streq(cn, "lambda"));

  /* The arity reported by Proc#arity (CRuby's "at least this many" encoding).
     `arity` above counts the leading required positionals; a post param (after a
     rest) is also required, and a required keyword adds one mandatory slot (the
     keyword hash), keeping the count positive. The arity is negative when the
     signature accepts a variable count: a rest positional, an optional keyword /
     keyword-rest *without* a required keyword (a required keyword makes the hash
     mandatory), or an optional positional -- but only for a lambda. A non-lambda
     proc is lenient about optional positionals, so they keep the positive count
     (`proc { |a, b=1| }.arity == 1`, while `->(a, b=1){}.arity == -2`). */
  int meta_arity = arity;
  {
    int pn = proc_params_node(c, create);
    if (pn >= 0) {
      int nopt = 0, npost = 0, nkw = 0;
      nt_arr(nt, pn, "optionals", &nopt);
      nt_arr(nt, pn, "posts", &npost);
      const int *kw = nt_arr(nt, pn, "keywords", &nkw);
      int rest = nt_ref(nt, pn, "rest");
      int kwrest = nt_ref(nt, pn, "keyword_rest");
      int has_req_kw = 0, has_opt_kw = 0;
      for (int i = 0; i < nkw; i++) {
        const char *kt = nt_type(nt, kw[i]);
        if (kt && sp_streq(kt, "RequiredKeywordParameterNode")) has_req_kw = 1;
        else if (kt && sp_streq(kt, "OptionalKeywordParameterNode")) has_opt_kw = 1;
      }
      const char *kwt = kwrest >= 0 ? nt_type(nt, kwrest) : NULL;
      int has_kwrest = kwt && sp_streq(kwt, "KeywordRestParameterNode");
      int mandatory = arity + npost + (has_req_kw ? 1 : 0);
      int neg = rest >= 0 || (nopt > 0 && is_lambda) ||
                ((has_opt_kw || has_kwrest) && !has_req_kw);
      meta_arity = neg ? -(mandatory + 1) : mandatory;
    }
  }

  /* An explicit `return <expr>` tail is a ReturnNode; its value node is the
     argument. Treating it as the effective tail keeps the body's return ABI
     (pointer launder / poly slot) consistent with an implicit tail expression,
     so the value is not lost. (tail_ret_arg stays -1 when the tail is not a
     single-value return.) The body return type is that effective tail's type. */
  int tail_ret_arg = -1;
  int tail_is_return = 0;
  TyKind ret = TY_NIL;
  { int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    if (bn > 0) {
      const char *tty = nt_type(nt, bb[bn - 1]);
      if (tty && sp_streq(tty, "ReturnNode")) {
        tail_is_return = 1;
        int rargs = nt_ref(nt, bb[bn - 1], "arguments");
        int ran = 0; const int *rav = rargs >= 0 ? nt_arr(nt, rargs, "arguments", &ran) : NULL;
        if (ran == 1) tail_ret_arg = rav[0];
      }
      ret = comp_ntype(c, tail_ret_arg >= 0 ? tail_ret_arg : bb[bn - 1]);
    }
  }
  /* A non-lambda proc whose body does `return` returns non-locally to the method
     that created it (only meaningful inside a method that set up a proc-return
     frame). Every `return` becomes a longjmp to that frame. When the tail
     statement is itself a `return`, the proc always longjmps -- the trampoline's
     own (fall-through) value is dead, so carry no value (return 0) and let
     emit_return throw. But when the tail is a plain expression, that expression
     IS the proc's value on the fall-through path (no `return` fired): keep the
     analyzed `ret` and emit it normally, so the value is not lost. */
  int ret_proc = (g_method_pr_label != NULL) && proc_does_nonlocal_return(c, create);
  /* A non-lambda EXPLICIT proc (`proc {}` / `Proc.new {}`) whose body
     top-level-breaks raises LocalJumpError "break from proc-closure" when
     called -- CRuby 4 delivers a break only for a block-converted proc, never
     an explicitly created one. A lambda's break is a return from the lambda. */
  int brk_proc = !is_lambda && !is_block_node && block_has_top_break(c, body);
  /* a lambda whose body can top-level-break returns that value: box the ret */
  if (is_lambda && block_has_top_break(c, body)) ret = TY_POLY;
  if (ret_proc && tail_is_return) { tail_ret_arg = -1; ret = TY_NIL; }
  /* A block passed as a method's &block argument must return the value type the
     method expects across all its call sites (its blk_ret): if that unified type
     is poly, return poly here so the sp_proc_call ABI is consistent. */
  if (ret != TY_POLY && !ret_proc) {
    int owner = -1;
    for (int oid = 0; oid < nt->count; oid++) if (nt_ref(nt, oid, "block") == create) { owner = oid; break; }
    if (owner >= 0 && nt_type(nt, owner) && sp_streq(nt_type(nt, owner), "CallNode")) {
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
  if (ncap > 0 || cap_self || ret_proc) {
    buf_printf(&g_procs, "typedef struct {");
    for (int i = 0; i < ncap; i++) {
      LocalVar *clv = scope_local(bs, caps.v[i]);
      /* a float capture rides a native mrb_float cell, a poly capture an
         sp_RbVal cell, a heap object its typed pointer (see emit_scope_decls). */
      buf_puts(&g_procs, " "); emit_cell_elem_type(c, clv, &g_procs);
      buf_printf(&g_procs, " *%s;", caps.v[i]);
    }
    if (cap_self) buf_puts(&g_procs, " void *__self;");
    if (ret_proc) buf_puts(&g_procs, " mrb_int _home;");  /* home method's proc-return id (sp_proc_home.id) */
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
  /* The proc body is a fresh function: the method's proc-return funnel does not
     apply, but a non-local `return` longjmps to the home frame read from the
     capture. Save/clear the method funnel and set the proc-return home accessor. */
  const char *sv_pr_label = g_method_pr_label, *sv_pr_var = g_method_pr_var, *sv_prh = g_proc_return_home;
  g_method_pr_label = NULL; g_method_pr_var = NULL;
  int sv_excd = g_exc_frame_depth, sv_prexcd = g_method_pr_exc_depth;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0;
  char home_acc[48] = "";
  if (ret_proc) { snprintf(home_acc, sizeof home_acc, "((_proc_cap_%d *)_cap)->_home", pid); g_proc_return_home = home_acc; }
  else g_proc_return_home = NULL;
  g_pre = NULL; g_indent = 0; g_nren = 0; g_block_id = -1; g_block_param_name = NULL;
  g_self = "self"; g_result_var = NULL; g_ret_type = ret; g_ensure_depth = 0; g_result_poly = 0;
  /* the proc body is a fresh function: an enclosing break serial is out of
     scope inside it; a break here is a lambda-local return (kind 1) or a
     throw to the captured home serial (kind 2) */
  const char *sv_bser = g_brk_ser_var; g_brk_ser_var = NULL;
  int sv_bskip = g_brk_skip_id; g_brk_skip_id = -1;
  int sv_pbk = g_proc_body_kind; const char *sv_pbh = g_proc_brk_home;
  g_proc_body_kind = is_lambda ? 1 : (brk_proc ? 2 : 0);
  /* serial -1 never matches a live scope: sp_brk_throw raises the CRuby
     LocalJumpError after evaluating the break value */
  g_proc_brk_home = brk_proc ? "-1" : NULL;
  char cap_struct_name[32] = "";
  if (ncap > 0) { snprintf(cap_struct_name, sizeof cap_struct_name, "_proc_cap_%d", pid); g_cap_struct = cap_struct_name; g_cap_names = &caps; }
  else { g_cap_struct = NULL; g_cap_names = NULL; }

  Buf *pb = &g_procs;
  buf_printf(pb, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  if (ncap == 0 && !cap_self && !ret_proc) buf_puts(pb, "    (void)_cap;\n");
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
    /* a heap-pointer param is laundered back from the mrb_int slot; a TY_POLY
       (sp_RbVal) param doesn't fit the slot, so it rides the _sp_proc_poly_args
       side-channel the call site published before the call. */
    /* A param past the supplied argument count binds nil, not the typed zero
       (CRuby fills missing block/proc params with nil). The body is already
       nil-aware for these slots; supply the matching nil sentinel. */
    if (pt == TY_POLY) {
      /* the side-channel array holds 16 slots (the proc-call ABI cap) */
      if (k < 16) {
        if (!g_needs_proc_poly_argslot) {
          g_needs_proc_poly_argslot = 1;
          buf_puts(&g_proc_protos, "static sp_RbVal _sp_proc_poly_args[16];\n");
        }
        buf_printf(pb, "(argc > %d) ? _sp_proc_poly_args[%d] : sp_box_nil();\n", k, k);
      }
      else buf_puts(pb, "0;\n");
    }
    else if (proc_slot_is_ptr(pt)) {
      buf_printf(pb, "(argc > %d) ? (", k); emit_ctype(c, pt, pb);
      buf_printf(pb, ")(uintptr_t)args[%d] : NULL;\n", k);
    }
    else {
      const char *nilv = (pt == TY_INT || pt == TY_BOOL) ? "SP_INT_NIL"
                       : (pt == TY_FLOAT) ? "sp_float_nil()"
                       : (pt == TY_SYMBOL) ? "((sp_sym)-1)" : NULL;
      if (nilv) buf_printf(pb, "(argc > %d) ? args[%d] : %s;\n", k, k, nilv);
      else buf_printf(pb, "args[%d];\n", k);
    }
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
      int tnode = tail_ret_arg >= 0 ? tail_ret_arg : bb[bn - 1];
      Buf rpre = {0}, rval = {0};
      Buf *sv_rpre = g_pre; int sv_rind = g_indent;
      g_pre = &rpre; g_indent = 1;
      emit_expr(c, tnode, &rval);
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
    if (tail_ret_arg >= 0) {
      /* explicit `return <expr>` tail: emit the leading statements, then box the
         returned value into the poly slot (emit_stmts_tail would route the
         ReturnNode to a raw `return`, bypassing the slot). */
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
      Buf rpre = {0}, rval = {0};
      Buf *sv_rpre = g_pre; int sv_rind = g_indent;
      g_pre = &rpre; g_indent = 1;
      emit_boxed(c, tail_ret_arg, &rval);
      g_pre = sv_rpre; g_indent = sv_rind;
      if (rpre.p) buf_puts(pb, rpre.p);
      buf_puts(pb, "  _sp_proc_poly_ret = ");
      if (rval.p) buf_puts(pb, rval.p);
      buf_puts(pb, ";\n");
      free(rpre.p); free(rval.p);
    }
    else emit_stmts_tail(c, body, pb, 1);
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
  g_brk_ser_var = sv_bser; g_brk_skip_id = sv_bskip;
  g_proc_body_kind = sv_pbk; g_proc_brk_home = sv_pbh;
  g_result_poly = sv_rp;
  g_method_pr_label = sv_pr_label; g_method_pr_var = sv_pr_var; g_proc_return_home = sv_prh;
  g_exc_frame_depth = sv_excd; g_method_pr_exc_depth = sv_prexcd;

  if (ncap == 0 && !cap_self && !ret_proc) {
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, NULL, NULL, %d, %s, %d, %s)",
               pid, meta_arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
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
      /* Capture the home method's proc-return frame so the proc's `return`
         longjmps to it (the creating method declared `_pr`). */
      if (ret_proc) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->_home = _h.id;\n", pid); }
    }
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, _capv_%d, _proc_cap_scan_%d, %d, %s, %d, %s)",
               pid, pid, pid, meta_arity, is_lambda ? "TRUE" : "FALSE", arity, meta_args);
  }

  free(params.v); free(used.v); free(locals.v); free(caps.v);
}

/* Emit the struct + the constructor (sp_<Class>_new) for one class. */
/* Returns 1 if the class name shadows a built-in runtime type (no struct/new to emit). */
int is_builtin_reopen(const char *name) {
  return sp_streq(name, "Toplevel") ||
         sp_streq(name, "String")    || sp_streq(name, "Integer") ||
         sp_streq(name, "Float")     || sp_streq(name, "Symbol")  ||
         sp_streq(name, "TrueClass") || sp_streq(name, "FalseClass") ||
         sp_streq(name, "NilClass")  || sp_streq(name, "Array")   ||
         sp_streq(name, "Object")    || sp_streq(name, "Numeric");
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
    "LoadError", "RegexpError", "StringScanner_Error", "FiberError",
    "UncaughtThrowError", NULL
  };
  for (int i = 0; EX[i]; i++) if (sp_streq(n, EX[i])) return 1;
  return 0;
}

/* Returns 1 if user class ci (or any ancestor) directly inherits a builtin exception. */
int class_is_exc_subclass(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    const char *sn = nt_str(c->nt, sc, "name");
    if (sty && (sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode")) &&
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

/* The C function (sp_<Class>_inspect / _to_s) that stringifies an object of
   class `cid`, or NULL when none applies (a plain object with no inspect/to_s).
   A user-defined method routes to its defining class; a struct/data routes to
   the generated one. The caller emits sp_<name>_<meth>((sp_<name> *)expr). */
const char *obj_str_cname(Compiler *c, int cid, int want_inspect) {
  if (cid < 0 || cid >= c->nclasses) return NULL;
  int defcls = cid;
  if (comp_method_in_chain(c, cid, want_inspect ? "inspect" : "to_s", &defcls) >= 0)
    return c->classes[defcls].name;
  if (c->classes[cid].is_struct) return c->classes[cid].name;  /* generated #inspect/#to_s */
  return NULL;
}

/* Return the builtin exception parent name for user exc subclass ci,
   walking up the chain until a builtin exception name is found. */
const char *exc_builtin_parent(Compiler *c, int ci) {
  for (int k = ci; k >= 0; k = c->classes[k].parent) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    if (sc < 0) continue;
    const char *sty = nt_type(c->nt, sc);
    const char *sn = nt_str(c->nt, sc, "name");
    if (sty && (sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode")) && is_exc_name(sn))
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
      /* belt and suspenders: analyze widens void/nil ivar slots to poly
         (they have no C storage type); never declare a `void` field. */
      if (t == TY_VOID || t == TY_NIL) t = TY_POLY;
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
    /* belt and suspenders: analyze widens void/nil ivar slots to poly
       (they have no C storage type); never declare a `void` field. */
    if (t == TY_VOID || t == TY_NIL) t = TY_POLY;
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

/* An int ivar's nil default differs from its zero bit-pattern: its nil is
   SP_INT_NIL, not 0. The compiler already reads an unwritten int ivar as that
   sentinel (truthiness, `@x ||= v`, `.nil?`), so the constructor must seed it
   explicitly -- a memset/{0} slot reads back as a real 0 and makes `@x ||= 5`
   keep 0. Returns NULL for types whose zero-init already reads as nil
   (string->NULL) or which are nil-initialized separately (poly). */
static const char *ivar_scalar_nil_init(TyKind t) {
  if (t == TY_INT) return "SP_INT_NIL";
  return NULL;
}

/* Seed every ivar whose zero bit-pattern is not nil. A poly ivar's zero
   pattern has tag 0, not SP_TAG_NIL, so it must be set to sp_box_nil(); an int
   ivar's nil is SP_INT_NIL, not 0. `lv` is the receiver-and-accessor prefix
   ("self.", "self->", "_t3.", "_t3->"); each assignment is bracketed by `lead`
   (indentation) and `term` (`;\n` for a statement, `;` inside a compound expr).
   A string ivar's NULL zero-pattern already reads as nil, so it is skipped. */
static void emit_ivar_nil_inits(Buf *b, ClassInfo *ci, const char *lv,
                                const char *lead, const char *term) {
  for (int i = 0; i < ci->nivars; i++) {
    const char *name = ci->ivars[i] + 1;  /* skip leading '@' */
    if (ci->ivar_types[i] == TY_POLY)
      buf_printf(b, "%s%siv_%s = sp_box_nil()%s", lead, lv, name, term);
    else {
      const char *nv = ivar_scalar_nil_init(ci->ivar_types[i]);
      if (nv) buf_printf(b, "%s%siv_%s = %s%s", lead, lv, name, nv, term);
    }
  }
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
    /* Struct/Data #inspect (== #to_s): `#<struct Name m=v, ...>` (Data uses
       `data`). Generated unless the user redefined the method, so a user
       override wins. to_s defers to inspect, which always exists here. */
    if (comp_method_in_chain(c, cid, "inspect", NULL) < 0) {
      const char *rn = class_ruby_name(c, cid); if (!rn) rn = ci->name;
      buf_printf(b, "static const char *sp_%s_inspect(sp_%s *self) {\n", ci->name, ci->name);
      buf_puts(b, "  if (!self) return \"nil\";\n");
      buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s %s\"); SP_GC_ROOT(s);\n",
                 ci->is_data ? "data" : "struct", rn);
      for (int i = 0; i < ci->nivars; i++) {
        buf_printf(b, "  sp_String_append(s, \"%s%s=\");\n", i ? ", " : " ", ci->ivars[i] + 1);
        TyKind mt = ci->ivar_types[i];
        const char *mcn = ty_is_object(mt) ? obj_str_cname(c, ty_object_class(mt), 1) : NULL;
        if (mcn) {
          /* a struct/data (or user-#inspect) member recurses into its own inspect */
          buf_printf(b, "  sp_String_append(s, self->iv_%s ? sp_%s_inspect((sp_%s *)self->iv_%s) : \"nil\");\n",
                     ci->ivars[i] + 1, mcn, mcn, ci->ivars[i] + 1);
        } else {
          Buf ivb; memset(&ivb, 0, sizeof ivb); buf_printf(&ivb, "self->iv_%s", ci->ivars[i] + 1);
          Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, ci->ivar_types[i], ivb.p, &bx);
          buf_printf(b, "  sp_String_append(s, sp_poly_inspect(%s));\n", bx.p);
          free(bx.p); free(ivb.p);
        }
      }
      buf_puts(b, "  sp_String_append(s, \">\");\n  return s->data;\n}\n");
    }
    if (comp_method_in_chain(c, cid, "to_s", NULL) < 0)
      buf_printf(b, "static const char *sp_%s_to_s(sp_%s *self) { return sp_%s_inspect(self); }\n",
                 ci->name, ci->name, ci->name);
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
    emit_ivar_nil_inits(b, ci, "self.", "  ", ";\n");
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
      emit_ivar_nil_inits(b, ci, "self->", "  ", ";\n");
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
  /* memset zero-inits fields, but a poly ivar's zero pattern is not nil and an
     int ivar's nil is SP_INT_NIL, so seed them before initialize runs
     (read-only ivars stay nil; written ones are overwritten). */
  emit_ivar_nil_inits(b, ci, "self->", "  ", ";\n");
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

/* Emit a statement-expression that allocates an instance of class `cid` with
   its ivars zero/nil-initialized and cls_id stamped, but WITHOUT running
   initialize -- the Class#allocate primitive. Handles both value-type objects
   (returned by value) and pointer objects. The allocation mirrors the body of
   emit_class_new above, minus the initialize call. */
void emit_obj_alloc_expr(Compiler *c, int cid, Buf *b) {
  ClassInfo *ci = &c->classes[cid];
  int is_val = comp_ty_value_obj(c, ty_object(cid));
  int t = ++g_tmp;
  if (is_val) {
    buf_printf(b, "({ sp_%s _t%d = {0}; _t%d.cls_id = %d;", ci->name, t, t, cid);
    char lv[32]; snprintf(lv, sizeof lv, "_t%d.", t);
    emit_ivar_nil_inits(b, ci, lv, " ", ";");
    buf_printf(b, " _t%d; })", t);
  }
  else {
    /* No SP_GC_ROOT needed: allocate runs no initialize, so nothing after the
       SP_POOL_NEW allocates (memset and sp_box_nil are non-allocating), and the
       fresh pointer is consumed by the enclosing expression with no intervening
       allocation. (.new roots self because initialize runs allocating code.) */
    buf_printf(b, "({ sp_%s *_t%d = SP_POOL_NEW(%s, %s%s%s); memset(_t%d, 0, sizeof(*_t%d));"
                  " _t%d->cls_id = %d;",
               ci->name, t, ci->name,
               class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->name : "NULL",
               class_needs_scan(ci) ? "_scan" : "", t, t, t, cid);
    char lv[32]; snprintf(lv, sizeof lv, "_t%d->", t);
    emit_ivar_nil_inits(b, ci, lv, " ", ";");
    buf_printf(b, " _t%d; })", t);
  }
}

/* ---- Marshal of user objects (CRuby `o` form) ----
   For each marshalable class, codegen emits an arm in two dispatchers that
   sp_marshal.h calls: sp_marshal_obj_dump (by cls_id) writes `o`<:Class><nivar>
   (:@iv val)*, and sp_marshal_obj_load (by class name) allocates a blank
   instance and fills its ivars from the loaded name/value pairs. Only scalar,
   poly, and nested-user-object ivar types round-trip cleanly (a typed array or
   hash ivar would mismatch the loader's always-poly containers), so a class
   carrying any other ivar type is left out and raises at runtime. */
static int marshal_ivar_type_ok(TyKind t) {
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_STRING: case TY_BOOL:
    case TY_SYMBOL: case TY_BIGINT: case TY_POLY: case TY_NIL:
      return 1;
    default:
      return ty_is_object(t);  /* a nested user object reloads with its real cls_id */
  }
}
static int class_marshalable(Compiler *c, int i) {
  ClassInfo *ci = &c->classes[i];
  if (is_builtin_reopen(ci->name)) return 0;
  if (class_is_exc_subclass(c, i)) return 0;
  if (comp_ty_value_obj(c, ty_object(i))) return 0;  /* value types: out of scope for v1 */
  for (int j = 0; j < ci->nivars; j++)
    if (!marshal_ivar_type_ok(ci->ivar_types[j])) return 0;
  return 1;
}
/* Box ivar expression `expr` (typed t) into an sp_RbVal, mapping an unset ivar
   (SP_INT_NIL / NULL pointer) to nil. */
static void emit_marshal_box_ivar(TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  if (t == TY_NIL)  { buf_puts(b, "sp_box_nil()"); return; }
  if (ty_is_object(t)) {
    buf_printf(b, "(%s ? sp_box_obj(%s, %d) : sp_box_nil())", expr, expr, ty_object_class(t));
    return;
  }
  switch (t) {
    case TY_INT:    buf_printf(b, "(%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(%s))", expr, expr); break;
    case TY_FLOAT:  buf_printf(b, "sp_box_float(%s)", expr); break;
    case TY_STRING: buf_printf(b, "(%s ? sp_box_str(%s) : sp_box_nil())", expr, expr); break;
    case TY_BOOL:   buf_printf(b, "sp_box_bool(%s)", expr); break;
    case TY_SYMBOL: buf_printf(b, "sp_box_sym(%s)", expr); break;
    case TY_BIGINT: buf_printf(b, "(%s ? sp_box_bigint(%s) : sp_box_nil())", expr, expr); break;
    default:        buf_puts(b, "sp_box_nil()"); break;
  }
}
/* Unbox the loaded value `val` into ivar type t, mapping a nil back to the
   type's unset representation. */
static void emit_marshal_unbox_ivar(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, "val"); return; }
  if (ty_is_object(t)) {
    buf_printf(b, "(val.tag == SP_TAG_OBJ ? (sp_%s *)val.v.p : NULL)", c->classes[ty_object_class(t)].name);
    return;
  }
  switch (t) {
    case TY_INT:    buf_puts(b, "(val.tag == SP_TAG_NIL ? SP_INT_NIL : (mrb_int)sp_poly_to_i(val))"); break;
    case TY_FLOAT:  buf_puts(b, "(mrb_float)sp_poly_to_f(val)"); break;
    case TY_STRING: buf_puts(b, "(val.tag == SP_TAG_STR ? val.v.s : NULL)"); break;
    case TY_BOOL:   buf_puts(b, "(val.tag == SP_TAG_BOOL ? val.v.b : 0)"); break;
    case TY_SYMBOL: buf_puts(b, "(val.tag == SP_TAG_SYM ? (sp_sym)val.v.i : 0)"); break;
    case TY_BIGINT: buf_puts(b, "(val.tag == SP_TAG_BIGINT ? (sp_Bigint *)val.v.p : NULL)"); break;
    default:        buf_puts(b, "0"); break;
  }
}
static void emit_marshal_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static int sp_marshal_obj_dump(sp_mar_buf *b, int cls_id, void *p) {\n");
  buf_puts(b, "  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_marshalable(c, i)) continue;
    ClassInfo *ci = &c->classes[i];
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)p; (void)o;\n", ci->name, ci->name);
    buf_printf(b, "      sp_mar_b(b, 'o'); sp_mar_sym(b, \"%s\");\n", ci->name);
    buf_printf(b, "      sp_mar_long(b, %d);\n", ci->nivars);
    for (int j = 0; j < ci->nivars; j++) {
      char expr[160]; snprintf(expr, sizeof expr, "o->iv_%s", ci->ivars[j] + 1);
      buf_printf(b, "      sp_mar_sym(b, \"%s\"); sp_mar_w(b, ", ci->ivars[j]);
      emit_marshal_box_ivar(ci->ivar_types[j], expr, b);
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return 1;\n    }\n");
  }
  buf_puts(b, "    default: return 0;\n  }\n}\n");

  buf_puts(b, "static sp_RbVal sp_marshal_obj_load(const char *name, sp_RbVal iv_boxed, int *ok) {\n");
  buf_puts(b, "  sp_PolyArray *iv = (sp_PolyArray *)iv_boxed.v.p;\n");
  buf_puts(b, "  *ok = 1; (void)iv;\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_marshalable(c, i)) continue;
    ClassInfo *ci = &c->classes[i];
    buf_printf(b, "  if (!strcmp(name, \"%s\")) {\n", ci->name);
    buf_printf(b, "    sp_%s *o = ", ci->name);
    emit_obj_alloc_expr(c, i, b);
    buf_puts(b, ";\n");
    buf_puts(b, "    SP_GC_ROOT(o);\n");
    if (ci->nivars > 0) {
      buf_puts(b, "    for (mrb_int k = 0; k + 1 < iv->len; k += 2) {\n");
      buf_puts(b, "      const char *nm = sp_sym_to_s((sp_sym)sp_PolyArray_get(iv, k).v.i);\n");
      buf_puts(b, "      sp_RbVal val = sp_PolyArray_get(iv, k + 1); (void)val; (void)nm;\n");
      for (int j = 0; j < ci->nivars; j++) {
        buf_printf(b, "      %sif (!strcmp(nm, \"%s\")) o->iv_%s = ",
                   j ? "else " : "", ci->ivars[j], ci->ivars[j] + 1);
        emit_marshal_unbox_ivar(c, ci->ivar_types[j], b);
        buf_puts(b, ";\n");
      }
      buf_puts(b, "    }\n");
    }
    buf_printf(b, "    return sp_box_obj(o, %d);\n", i);
    buf_puts(b, "  }\n");
  }
  buf_puts(b, "  *ok = 0; return sp_box_nil();\n}\n");
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
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  const char *saved_bbv = g_block_brk_var, *saved_yfbv = g_yield_blk_brk_fallback;
  const char *saved_ser = g_brk_ser_var;
  int saved_bbe = g_block_brk_ebase, saved_yfbe = g_yield_blk_brk_efallback;
  int saved_bbexc = g_block_brk_exc_base, saved_bexc = g_brk_exc_base;
  int saved_ebase = g_brk_ensure_base;

  g_yield_block_fallback = saved_block;
  g_yield_blk_brk_fallback = saved_bbv;
  g_yield_blk_brk_efallback = saved_bbe;
  g_block_id = block;
  /* same break-context rules as emit_inline_call_x */
  g_block_brk_var = (block == saved_block) ? saved_bbv : saved_ser;
  g_block_brk_ebase = (block == saved_block) ? saved_bbe : saved_ebase;
  g_block_brk_exc_base = (block == saved_block) ? saved_bbexc : saved_bexc;
  g_brk_ser_var = NULL;
  g_block_param_name = m->blk_param;

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  int din = indent + 1;

  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
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
  int is_forwarding = ty && sp_streq(ty, "ForwardingSuperNode");
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
  g_block_brk_var = saved_bbv; g_yield_blk_brk_fallback = saved_yfbv;
  g_block_brk_ebase = saved_bbe; g_yield_blk_brk_efallback = saved_yfbe;
  g_block_brk_exc_base = saved_bbexc; g_brk_exc_base = saved_bexc;
  g_brk_ser_var = saved_ser; g_brk_ensure_base = saved_ebase;
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
    if (ty && sp_streq(ty, "ForwardingSuperNode")) {
      for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", rename_local(s->pnames[i]));
    }
    else {
      int smi = -1;
      for (int k = c->nscopes - 1; k >= 1; k--) {
        Scope *sc = &c->scopes[k];
        if (sc->class_id == s->class_id && sc->name && sp_streq(sc->name, shadow))
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
    if (class_is_exc_subclass(c, s->class_id) && s->name && sp_streq(s->name, "initialize")) {
      int args_id = nt_ref(c->nt, id, "arguments");
      int argc2 = 0;
      const int *argv2 = NULL;
      if (args_id >= 0) argv2 = nt_arr(c->nt, args_id, "arguments", &argc2);
      if (argc2 > 0) {
        buf_printf(b, "(%s->msg = (", g_self);
        emit_expr(c, argv2[0], b);
        buf_puts(b, "))");
      }
      else if (ty && sp_streq(ty, "ForwardingSuperNode") && s->nparams > 0)
        buf_printf(b, "(%s->msg = lv_%s)", g_self, rename_local(s->pnames[0]));
      else
        buf_puts(b, "((void)0)");
      return;
    }
    /* `super` in a copy hook (initialize_copy / initialize_dup /
       initialize_clone) whose only ancestor is Object: Object provides these
       as no-ops -- and spinel's dup/clone already memcpy'd the whole struct
       (cls_id + every ivar) before invoking the user hook, so Object's ivar
       copy is already done. Emit a no-op rather than raising NoMethodError. */
    if (uname && (sp_streq(uname, "initialize_copy") ||
                  sp_streq(uname, "initialize_dup") ||
                  sp_streq(uname, "initialize_clone"))) {
      buf_puts(b, "((void)0)");
      return;
    }
    /* No superclass method anywhere (parent chain, included-module shadow, and
       the exception-initialize special case all missed). CRuby raises
       NoMethodError at runtime, so emit that rather than rejecting at compile
       time -- the call may sit in a branch that never runs. */
    const char *scn = class_ruby_name(c, s->class_id);
    if (!scn) scn = c->classes[s->class_id].name;
    buf_printf(b, "(sp_raise_cls(\"NoMethodError\", \"super: no superclass method '%s' for an instance of %s\"), %s)",
               uname, scn, default_value(comp_ntype(c, id)));
    return;
  }
  buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].name, mc(uname), c->classes[defcls].name, g_self);
  if (ty && sp_streq(ty, "ForwardingSuperNode")) {
    Scope *pm = &c->scopes[mi];
    int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
    for (int i = 0; i < n; i++) {
      LocalVar *src = scope_local(s, s->pnames[i]);
      LocalVar *dst = scope_local(pm, pm->pnames[i]);
      TyKind st = src ? src->type : TY_UNKNOWN;
      TyKind dt = dst ? dst->type : TY_UNKNOWN;
      /* Use the local's emitted C name: when this method body is inlined at a
         block call site the params are renamed (e.g. `x` -> `_y5_x`), so bare
         `super`'s implicit forwarding must reference the renamed identifier. */
      if (dt == TY_POLY && st != TY_POLY && st != TY_UNKNOWN) {
        buf_puts(b, ", ");
        Buf _bx; memset(&_bx, 0, sizeof _bx);
        buf_printf(&_bx, "lv_%s", rename_local(s->pnames[i]));
        emit_boxed_text(c, st, _bx.p, b);
        free(_bx.p);
      }
      else {
        buf_printf(b, ", lv_%s", rename_local(s->pnames[i]));
      }
    }
  }
  else {
    emit_args_filled(c, mi, nt_ref(c->nt, id, "arguments"), ", ", b);
  }
  buf_puts(b, ")");
}

/* Generate sp_obj_cmp_dispatch: a cls_id switch calling each instantiated
   Comparable class's user `<=>`, reporting a nil result as not-comparable.
   Installed as sp_obj_cmp_hook so the runtime comparator (no-block
   sort/min/max/clamp) can order user objects. Only object- and poly-typed
   operands are handled; an exotic operand type omits its arm and falls through
   to not-comparable, which the callers raise as an ArgumentError. */
static void emit_obj_cmp_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static mrb_int sp_obj_cmp_dispatch(sp_RbVal a, sp_RbVal b, mrb_bool *comparable) {\n");
  buf_puts(b, "  switch (a.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "<=>", &defcls);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    if (m->nparams < 1 || m->rest_idx >= 0) continue;     /* need exactly the one operand */
    if (m->ret != TY_INT && m->ret != TY_POLY) continue;  /* unusable return -> not-comparable */
    LocalVar *p = scope_local(m, m->pnames[0]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    const char *dcn = c->classes[defcls].name;
    int self_vt = c->classes[defcls].is_value_type;
    int cid = comp_class_index(c, c->classes[k].name);
    char argbuf[160];
    int obj_operand = 0;
    int pcid = -1;
    if (ty_is_object(pt)) {
      int pcls = ty_object_class(pt);
      const char *pcn = c->classes[pcls].name;
      pcid = comp_class_index(c, pcn);   /* guard b against the OPERAND's class, not the receiver's */
      snprintf(argbuf, sizeof argbuf, "%s(sp_%s *)b.v.p",
               c->classes[pcls].is_value_type ? "*" : "", pcn);
      obj_operand = 1;
    } else if (pt == TY_POLY) {
      snprintf(argbuf, sizeof argbuf, "b");
    } else {
      continue;
    }
    buf_printf(b, "    case %d: {\n", cid);
    if (obj_operand)
      buf_printf(b, "      if (b.tag != SP_TAG_OBJ || b.cls_id != %d) { *comparable = FALSE; return 0; }\n", pcid);
    if (m->ret == TY_INT) {
      buf_printf(b, "      *comparable = TRUE; return (mrb_int)sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
    } else {
      buf_printf(b, "      sp_RbVal _r = sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
      buf_puts(b, "      if (_r.tag == SP_TAG_NIL) { *comparable = FALSE; return 0; }\n");
      buf_puts(b, "      *comparable = TRUE; return _r.v.i;\n");
    }
    buf_puts(b, "    }\n");
  }
  buf_puts(b, "    default: break;\n");
  buf_puts(b, "  }\n  *comparable = FALSE; return 0;\n}\n");
}

/* Emit the static regex-literal globals and, when g_re_init_needed, the
   sp_re_init() that installs the symbol/regex/class/global-mark hooks and
   compiles the literals at startup. */
void emit_regex_section(Buf *b) {
  for (int i = 0; i < g_re_count; i++) {
    buf_printf(b, "static mrb_regexp_pattern *sp_re_pat_%d;\n", i);
  }
  /* sp_re_init wires the hooks below. When none apply (a trivial program uses
     no symbols, regex, class machinery, or heap globals) neither the function
     nor its main() call is emitted, so the symbol/regex runtime it would pin
     stays unreferenced and links away. */
  if (!g_re_init_needed) return;
  /* Forward-declare the symbol interner and the Marshal object dispatchers so
     sp_re_init can take their addresses before their definitions (emitted into
     the later `body` buffer). */
  if (g_uses_marshal) {
    buf_puts(b,
      "static sp_sym sp_sym_intern(const char *s);\n"
      "static int sp_marshal_obj_dump(sp_mar_buf *b, int cls_id, void *p);\n"
      "static sp_RbVal sp_marshal_obj_load(const char *name, sp_RbVal iv, int *ok);\n");
  }
  if (g_has_user_cmp)
    buf_puts(b, "static mrb_int sp_obj_cmp_dispatch(sp_RbVal a, sp_RbVal b, mrb_bool *comparable);\n");
  buf_puts(b, "static void sp_re_init(void) {\n");
  if (g_uses_symbols)
    buf_puts(b, "  sp_sym_name_fn = sp_sym_to_s;\n");
  if (g_has_user_cmp)
    buf_puts(b, "  sp_obj_cmp_hook = sp_obj_cmp_dispatch;\n");
  if (g_needs_class_machinery)
    buf_puts(b, "  sp_user_exc_parent_fn = sp_user_exc_parent;\n");
  /* Replace the runtime's hook with the superset that also marks this
     program's heap-typed globals/constants/class-ivars (it chains to
     sp_re_mark_globals itself). Skipped when there are none -- the marker would
     equal the constructor-installed default, so it isn't emitted either. */
  if (g_has_user_global_marks)
    buf_puts(b, "  sp_gc_mark_globals_hook = sp_mark_user_globals;\n");
  /* Install the Marshal vtable: the construction wrappers (sp_runtime.h) plus
     the generated symbol interner and per-class object dump/load. */
  if (g_uses_marshal) {
    buf_puts(b,
      "  sp_marshal_v.sym_intern = sp_sym_intern;\n"
      "  sp_marshal_v.arr_new = sp_marv_arr_new;\n"
      "  sp_marshal_v.arr_push = sp_marv_arr_push;\n"
      "  sp_marshal_v.hash_new = sp_marv_hash_new;\n"
      "  sp_marshal_v.hash_set = sp_marv_hash_set;\n"
      "  sp_marshal_v.box_complex = sp_marv_box_complex;\n"
      "  sp_marshal_v.box_rational = sp_marv_box_rational;\n"
      "  sp_marshal_v.obj_dump = sp_marshal_obj_dump;\n"
      "  sp_marshal_v.obj_load = sp_marshal_obj_load;\n"
      "  sp_marshal_v.raise = sp_marv_raise;\n");
  }
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
     raises a catchable RegexpError via sp_raise_cls instead of aborting. Only
     needed when the program actually constructs a regex. */
  if (g_uses_regex)
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
    case TY_THREAD:                buf_puts(b, "Thread"); break;
    case TY_QUEUE:                 buf_puts(b, "Thread::Queue"); break;
    case TY_MUTEX:                 buf_puts(b, "Thread::Mutex"); break;
    case TY_CONDVAR:               buf_puts(b, "Thread::ConditionVariable"); break;
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
    if (sp_streq(cls->name, "Method")) continue;
    Buf nb; memset(&nb, 0, sizeof nb);
    class_ruby_name_into(&nb, cls->name);
    buf_printf(&b, "class %s", nb.p ? nb.p : "");
    free(nb.p);
    if (cls->parent >= 0 && cls->parent < c->nclasses) {
      const char *pn = c->classes[cls->parent].name;
      if (pn && *pn && !sp_streq(pn, "Object")) {
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
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && (sp_streq(nm, "class") || sp_streq(nm, "is_a?") ||
                 sp_streq(nm, "kind_of?") || sp_streq(nm, "instance_of?") ||
                 sp_streq(nm, "ancestors") || sp_streq(nm, "superclass") ||
                 sp_streq(nm, "===")))
        return 1;
    }
    else if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && is_builtin_class_name(nm)) return 1;
    }
  }
  return 0;
}

/* Whole-program scan for the prologue features (see codegen_internal.h). Each
   flag over-approximates (a user method named `rand` keeps srand; that is
   harmless), so a feature that is genuinely used is never missed: a symbol /
   regex / random value can only originate from one of the nodes below. */
static void scan_prologue_features(Compiler *c) {
  const NodeTable *nt = c->nt;
  g_uses_symbols = (c->nsymbols > 0);
  g_uses_marshal = 0;
  g_uses_regex = 0; g_uses_argv = 0; g_uses_random = 0; g_uses_threads = 0;
  for (int i = 0; i < nt->count; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    if (sp_streq(ty, "RegularExpressionNode") || sp_streq(ty, "InterpolatedRegularExpressionNode"))
      g_uses_regex = 1;
    else if (sp_streq(ty, "SymbolNode") || sp_streq(ty, "InterpolatedSymbolNode"))
      g_uses_symbols = 1;
    else if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (!nm) continue;
      if (sp_streq(nm, "Regexp")) g_uses_regex = 1;
      else if (sp_streq(nm, "Thread") || sp_streq(nm, "Queue") || sp_streq(nm, "SizedQueue") ||
               sp_streq(nm, "Mutex") || sp_streq(nm, "Monitor") ||
               sp_streq(nm, "ConditionVariable")) g_uses_threads = 1;
      else if (sp_streq(nm, "Random")) g_uses_random = 1;
      else if (sp_streq(nm, "ARGV") || sp_streq(nm, "ARGF")) g_uses_argv = 1;
      else if (sp_streq(nm, "Symbol")) g_uses_symbols = 1;
      /* Marshal.load reconstructs symbols at runtime, so it needs the symbol
         table (sp_sym_intern / sp_sym_to_s) emitted even if the program uses no
         symbol literals; it also drives the sp_marshal_v vtable install. */
      else if (sp_streq(nm, "Marshal")) { g_uses_marshal = 1; g_uses_symbols = 1; }
    }
    else if (sp_streq(ty, "GlobalVariableReadNode") || sp_streq(ty, "GlobalVariableWriteNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && sp_streq(nm, "$*")) g_uses_argv = 1;
    }
    else if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (!nm) continue;
      if (sp_streq(nm, "to_sym") || sp_streq(nm, "intern")) g_uses_symbols = 1;
      else if (sp_streq(nm, "rand") || sp_streq(nm, "srand") || sp_streq(nm, "sample") ||
               sp_streq(nm, "shuffle") || sp_streq(nm, "shuffle!")) g_uses_random = 1;
    }
  }
}

/* Emit one top-level output unit (a method, constructor, BEGIN/END block, or the
   top-level body). Outside SP_COLLECT_ERRORS this is just the bare call. In
   collect mode each unit runs under a setjmp: an `unsupported` gap longjmps back
   here (instead of exiting), so one run surfaces every unsupported construct --
   the gap is already printed, this unit's malformed output is discarded, and the
   driver proceeds to the next unit. `unsupported` re-sets all per-method globals
   on the next emit_method, so an abandoned unit cannot corrupt the next.
   On recovery the buffer is rolled back to its length before this unit, so the
   abandoned unit's partial output is dropped. `body` must be the heap pointer
   (not an automatic) so a longjmp doesn't leave it indeterminate (C99 7.13.2.1);
   _saved_len is set before setjmp and so stays determinate across the jump. */
#define EMIT_COLLECT_UNIT(emit_call)                          \
  do {                                                        \
    if (!collect_mode()) { emit_call; }                       \
    else {                                                    \
      size_t _saved_len = body->len;                          \
      if (setjmp(g_unsup_recover) == 0) {                     \
        g_unsup_armed = 1; emit_call; g_unsup_armed = 0;      \
      } \
      else {                                                \
        g_unsup_armed = 0;                                    \
        body->len = _saved_len;                               \
        if (body->p) body->p[_saved_len] = '\0';              \
      }                                                       \
    }                                                         \
  } while (0)

/* `send` / `__send__` / `public_send` with a literal symbol/string name is
   rewritten to a direct call before this point (textually for a receiver form,
   on the AST for implicit self). A call to one of these that survives therefore
   has a runtime method name, which AOT cannot dispatch in a closed world --
   turn the opaque downstream reject (or silent nil-stub) into one actionable
   diagnostic anchored to the call site. Skipped entirely if the program defines
   its own method by that name, since then the call resolves normally. */
static void reject_runtime_send(Compiler *c) {
  const NodeTable *nt = c->nt;
  static const char *const names[] = { "send", "__send__", "public_send", NULL };
  for (int s = 0; s < c->nscopes; s++) {
    const char *sn = c->scopes[s].name;
    if (!sn) continue;
    for (int k = 0; names[k]; k++)
      if (sp_streq(sn, names[k])) return;  /* user-defined: leave to normal dispatch */
  }
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int is_send = 0;
    for (int k = 0; names[k]; k++) if (sp_streq(nm, names[k])) { is_send = 1; break; }
    if (!is_send) continue;
    int args = nt_ref(nt, id, "arguments");
    if (args < 0) continue;
    int ac = 0; const int *av = nt_arr(nt, args, "arguments", &ac);
    if (ac < 1 || !av) continue;
    const char *a0 = nt_type(nt, av[0]);
    /* a literal name should have been rewritten already; only a runtime name
       (a variable, a method result, an interpolated string, ...) reaches here */
    if (a0 && (sp_streq(a0, "SymbolNode") || sp_streq(a0, "StringNode"))) continue;
    /* lowered to a static name-dispatch (desugar_dynamic_send): handled, not rejected */
    { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) continue; }
    /* Only diagnose a send that codegen will actually emit. A send in a dead
       (unreachable) method is pruned before emission, so rejecting it would
       fail otherwise-valid programs that merely contain an unused method.
       walk_scope assigns every node — including those inside blocks — the
       enclosing method's scope, and the emit loop emits a scope's body only
       when it is reachable; mirror that gate here. */
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !c->scopes[sc].reachable) continue;
    unsupported(c, id, "send with a runtime method name (AOT needs a compile-time-known name)");
  }
}

char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);

  /* `#line` directives are emitted only when the parser stamped per-node
     source lines (SPINEL_LINE_MAP / SPINEL_DEBUG); the same env gates both
     sides so codegen and the AST agree. */
  g_line_map = (getenv("SPINEL_LINE_MAP") || getenv("SPINEL_DEBUG")) ? 1 : 0;
  g_debug = getenv("SPINEL_DEBUG") ? 1 : 0;
  /* The unresolved-call gate raises NoMethodError, matching CRuby (a silent
     wrong answer is the worst failure mode). SPINEL_GATE_RAISE=0 restores the
     old silent typed default as a transition escape hatch. */
  {
    const char *e = getenv("SPINEL_GATE_RAISE");
    g_gate_raise = (e && *e == '0') ? 0 : 1;
  }

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

  /* Reject runtime-name send before any emission so the diagnostic fires
     regardless of how the call's result is later consumed. */
  reject_runtime_send(c);

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
      for (const char *s = cf->ffi_libs[li].names; ; ) {
        const char *semi = strchr(s, ';');
        int len = semi ? (int)(semi - s) : (int)strlen(s);
        if (len > 0) buf_printf(&b, "/* SPINEL_LINK: -l%.*s */\n", len, s);
        if (!semi) break;
        s = semi + 1;
      }
    }
    for (int ci = 0; ci < cf->n_ffi_cflags; ci++) {
      for (const char *s = cf->ffi_cflags[ci].val; ; ) {
        const char *semi = strchr(s, ';');
        int len = semi ? (int)(semi - s) : (int)strlen(s);
        if (len > 0) buf_printf(&b, "/* SPINEL_CFLAGS: %.*s */\n", len, s);
        if (!semi) break;
        s = semi + 1;
      }
    }
    int any_binstr = 0;
    for (int fi = 0; fi < cf->n_ffi_funcs; fi++) {
      const char *ret = cf->ffi_funcs[fi].ret;
      if (sp_streq(ret, "binstr")) any_binstr = 1;
      buf_puts(&b, "extern ");
      buf_puts(&b, ffi_c_type(ret));
      buf_puts(&b, " ");
      buf_puts(&b, cf->ffi_funcs[fi].name);
      buf_puts(&b, "(");
      for (int ai = 0; ai < cf->ffi_funcs[fi].nargs; ai++) {
        if (ai) buf_puts(&b, ", ");
        buf_puts(&b, ffi_c_type(cf->ffi_funcs[fi].args[ai]));
      }
      if (cf->ffi_funcs[fi].nargs == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    /* Byte count for the :binstr return mode (defined in sp_net.c). */
    if (any_binstr) buf_puts(&b, "extern int sp_net_bin_len;\n");
    for (int bi = 0; bi < cf->n_ffi_bufs; bi++) {
      buf_printf(&b, "static char sp_ffi_buf_%s_%s[%d];\n",
                 cf->ffi_bufs[bi].mod, cf->ffi_bufs[bi].name, cf->ffi_bufs[bi].size);
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
    buf_puts(&b, "static const char *sp_dyn_syms[SP_DYN_SYMS_MAX]; static int sp_ndyn = 0;\n");
    buf_printf(&b, "static const char *sp_sym_to_s(sp_sym id){"
                   "if(id>=0&&id<%d)return %s;"
                   "if(id>=%d&&id<%d+sp_ndyn)return sp_dyn_syms[id-%d];"
                   "return \"\";}\n", ns, ns > 0 ? "sp_sym_names[id]" : "\"\"", ns, ns, ns);
    buf_printf(&b, "static sp_sym sp_sym_intern(const char *s){"
                   "for(int i=0;i<%d;i++)if(strcmp(%s,s)==0)return (sp_sym)i;"
                   "for(int i=0;i<sp_ndyn;i++)if(strcmp(sp_dyn_syms[i],s)==0)return (sp_sym)(%d+i);"
                   "if(sp_ndyn<SP_DYN_SYMS_MAX){sp_dyn_syms[sp_ndyn]=sp_str_dup_external(s);return (sp_sym)(%d+sp_ndyn++);}"
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
  scan_prologue_features(c);
  /* Threaded-runtime marker: the driver greps for this and links the
     -DSP_THREADS runtime variant (libspinel_rt_mt.a) plus -lpthread instead of
     the byte-identical single-threaded archive. Emitted only when the program
     references Thread/Mutex/Queue/... -- so it must follow the feature scan. */
  if (g_uses_threads) buf_puts(&b, "/* SPINEL_USES_THREADS */\n");
  if (g_needs_class_machinery) {
  /* sp_cls_is_module[i]: 1 if user class i was defined as a module, 0 if class */
  if (c->nclasses > 0) {
    buf_printf(&b, "static const int sp_cls_is_module[%d] = {", c->nclasses);
    for (int i = 0; i < c->nclasses; i++) {
      if (i) buf_puts(&b, ",");
      const char *dt = nt_type(c->nt, c->classes[i].def_node);
      buf_printf(&b, "%d", (dt && sp_streq(dt, "ModuleNode")) ? 1 : 0);
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
          const char *sc_nm = (sc_ty && (sp_streq(sc_ty, "ConstantReadNode") || sp_streq(sc_ty, "ConstantPathNode"))) ? nt_str(c->nt, sc_node, "name") : NULL;
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
        if (!ty2 || (!sp_streq(ty2, "ClassNode") && !sp_streq(ty2, "ModuleNode"))) continue;
        int cp2 = nt_ref(c->nt, id, "constant_path");
        const char *cn2 = cp2 >= 0 ? nt_str(c->nt, cp2, "name") : NULL;
        if (!cn2 || comp_class_index(c, cn2) != ci) continue;
        int body2 = nt_ref(c->nt, id, "body");
        int bn2 = 0;
        const int *stmts2 = body2 >= 0 ? nt_arr(c->nt, body2, "body", &bn2) : NULL;
        for (int k2 = 0; k2 < bn2; k2++) {
          const char *sty2 = nt_type(c->nt, stmts2[k2]);
          if (!sty2 || !sp_streq(sty2, "CallNode")) continue;
          const char *nm2 = nt_str(c->nt, stmts2[k2], "name");
          if (!nm2 || !sp_streq(nm2, "include")) continue;
          if (nt_ref(c->nt, stmts2[k2], "receiver") >= 0) continue;
          int anode2 = nt_ref(c->nt, stmts2[k2], "arguments");
          int an2 = 0;
          const int *aargs = anode2 >= 0 ? nt_arr(c->nt, anode2, "arguments", &an2) : NULL;
          for (int j2 = 0; j2 < an2; j2++) {
            const char *aty2 = nt_type(c->nt, aargs[j2]);
            const char *mname2 = (aty2 && sp_streq(aty2, "ConstantReadNode")) ? nt_str(c->nt, aargs[j2], "name") : NULL;
            if (!mname2 && aty2 && sp_streq(aty2, "ConstantPathNode")) mname2 = nt_str(c->nt, aargs[j2], "name");
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
    /* When the walk reaches a builtin class (a user class's eventual Object
       parent, or a builtin start), follow the full builtin chain with module
       includes so e.g. Dog.ancestors == [Dog, Animal, Object, Kernel,
       BasicObject], matching CRuby. */
    buf_puts(&b, "    if(cur.cls_id<0){\n");
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
          if (sty && (sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode")))
            par = nt_str(c->nt, sc, "name");
        }
        if (!par && c->classes[i].parent >= 0)
          par = c->classes[c->classes[i].parent].name;
        if (par) {
          buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return \"%s\";\n", cn, par);
          /* also register the leaf name if different from qualified name */
          if (cn != c->classes[i].name && !sp_streq(cn, c->classes[i].name))
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
      /* static initializers must be constant.  default_value() returns the
         sp_box_nil() *call* for poly, which is not a constant initializer at
         file scope; emit the equivalent constant aggregate instead (mirrors the
         civ class-ivar decls below).  This matters under --int-overflow=promote
         where every int cvar is widened to poly. */
      const char *init = t == TY_RANGE ? "{0}"
                       : t == TY_POLY  ? "{SP_TAG_NIL, 0, {0}}"
                       : default_value(t);
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " cvar_%s_%s = %s;\n", ci->name, ci->cvars[j] + 2, init);
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
      /* forward-declare the generated stringifiers so one struct's #inspect may
         recurse into a struct-typed member regardless of definition order */
      if (comp_method_in_chain(c, i, "inspect", NULL) < 0)
        buf_printf(&b, "static const char *sp_%s_inspect(sp_%s *self);\n", ci->name, ci->name);
      if (comp_method_in_chain(c, i, "to_s", NULL) < 0)
        buf_printf(&b, "static const char *sp_%s_to_s(sp_%s *self);\n", ci->name, ci->name);
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
               lv->type == TY_RANGE ? "{0}" :
               lv->type == TY_POLY  ? "{SP_TAG_NIL, 0, {0}}" : default_value(lv->type));
  }
  for (int i = 0; i < c->nconsts; i++) {
    LocalVar *lv = &c->consts[i];
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " cst_%s = %s;\n", lv->name,
               lv->type == TY_RANGE ? "{0}" :
               lv->type == TY_POLY  ? "{SP_TAG_NIL, 0, {0}}" : default_value(lv->type));
    if (lv->init_guarded) buf_printf(&b, "static int sp_init_in_progress_%s;\n", lv->name);
  }
  if (c->ngvars || c->nconsts) buf_puts(&b, "\n");

  /* GC marking for the file-scope statics above: heap objects reachable
     only through a global/constant/class-ivar slot would otherwise be
     swept (RAND = Rand.new lost its PRNG mid-render). Chained ahead of
     the runtime's own sp_re_mark_globals via the hook in sp_re_init. */
  {
    /* Collect the user-global mark lines into a temp buffer first. If none are
       emitted, the marker would be identical to the runtime default
       (sp_re_mark_globals, installed by a constructor before main), so skip it
       and the sp_re_init hook override entirely -- a trivial program carries
       neither. g_has_user_global_marks gates the override (see emit_regex_section). */
    Buf mk; memset(&mk, 0, sizeof mk);
    for (int i = 0; i < c->ngvars; i++) {
      LocalVar *lv = &c->gvars[i];
      if (!is_scalar_ret(lv->type)) continue;
      if (lv->type == TY_STRING) buf_printf(&mk, "  sp_mark_string(gv_%s);\n", lv->name);
      else if (lv->type == TY_POLY) buf_printf(&mk, "  sp_mark_rbval(gv_%s);\n", lv->name);
      else if (needs_root(lv->type)) buf_printf(&mk, "  if (gv_%s) sp_gc_mark((void *)gv_%s);\n", lv->name, lv->name);
    }
    for (int i = 0; i < c->nconsts; i++) {
      LocalVar *lv = &c->consts[i];
      if (!is_scalar_ret(lv->type)) continue;
      if (lv->type == TY_STRING) buf_printf(&mk, "  sp_mark_string(cst_%s);\n", lv->name);
      else if (lv->type == TY_POLY) buf_printf(&mk, "  sp_mark_rbval(cst_%s);\n", lv->name);
      else if (needs_root(lv->type)) buf_printf(&mk, "  if (cst_%s) sp_gc_mark((void *)cst_%s);\n", lv->name, lv->name);
    }
    for (int i = 0; i < c->nclasses; i++) {
      ClassInfo *ci = &c->classes[i];
      for (int j = 0; j < ci->nivars; j++) {
        TyKind t = ci->ivar_types[j] == TY_UNKNOWN ? TY_INT : ci->ivar_types[j];
        const char *iv = ci->ivars[j] + 1;
        if (t == TY_STRING) buf_printf(&mk, "  sp_mark_string(civ_%s_%s);\n", ci->name, iv);
        else if (t == TY_POLY) buf_printf(&mk, "  sp_mark_rbval(civ_%s_%s);\n", ci->name, iv);
        else if (needs_root(t)) buf_printf(&mk, "  if (civ_%s_%s) sp_gc_mark((void *)civ_%s_%s);\n", ci->name, iv, ci->name, iv);
      }
      for (int j = 0; j < ci->nsg_readers; j++)
        buf_printf(&mk, "  sp_mark_rbval(sg_%s_%s);\n", ci->name, ci->sg_readers[j]);
    }
    g_has_user_global_marks = (mk.p && mk.len > 0);
    if (g_has_user_global_marks) {
      buf_puts(&b, "static void sp_mark_user_globals(void) {\n");
      buf_puts(&b, "  sp_re_mark_globals();\n");
      buf_puts(&b, mk.p);
      buf_puts(&b, "}\n\n");
    }
    free(mk.p);
  }
  /* An instantiated class that can reach a `<=>` (its own or an ancestor's)
     needs the cmp-hook so the runtime comparator can order its instances
     (no-block sort/min/max/clamp and the checked object comparisons). The
     chain walk matters: a subclass may inherit `<=>` from a base class that
     is itself never instantiated. */
  g_has_user_cmp = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    if (comp_method_in_chain(c, k, "<=>", NULL) >= 0) { g_has_user_cmp = 1; break; }
  }

  /* sp_re_init is worth emitting only if it would set at least one hook. */
  g_re_init_needed = g_uses_symbols || g_uses_marshal || g_uses_regex || g_needs_class_machinery ||
                     g_has_user_global_marks || g_has_user_cmp;

  /* Constructor defs, method defs, and main go into a separate buffer. Any
     proc literals they contain accumulate static functions into g_procs /
     g_proc_protos; we splice those in ahead of these bodies, since a proc
     function must be declared before the body that references it. */
  Buf *body = (Buf *)calloc(1, sizeof *body);  /* heap: must survive a collect-mode longjmp (see EMIT_COLLECT_UNIT) */
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      EMIT_COLLECT_UNIT(emit_class_new(c, &c->classes[i], body));
  /* user-object Marshal dispatchers (after every class struct + pool define) */
  emit_marshal_dispatch(c, body);
  for (int s = 1; s < c->nscopes; s++) {
    if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s) || c->scopes[s].is_transplanted_source) continue; EMIT_COLLECT_UNIT(emit_method(c, &c->scopes[s], body));
  }
  /* Comparable cmp-hook dispatcher (after the user `<=>` definitions it calls). */
  if (g_has_user_cmp) emit_obj_cmp_dispatch(c, body);

  /* Emit END block static functions for atexit registration */
  int end_count = 0;
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && sp_streq(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || !sp_streq(sty, "PostExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        end_count++;
        buf_printf(body, "static void sp_end_fn_%d(void) { SP_GC_SAVE();\n", end_count);
        EMIT_COLLECT_UNIT(emit_stmts(c, stmts, body, 1));
        buf_puts(body, "}\n");
      }
    }
  }

  buf_puts(body, "int main(int argc,char**argv){\n");
  buf_puts(body, "    SP_GC_SAVE();\n");
  if (g_re_init_needed) buf_puts(body, "    sp_re_init();\n");
  /* Adopt the main thread and chain the scheduler's GC root hook. Placed after
     sp_re_init so it chains whatever globals hook that installed. */
  if (g_uses_threads) buf_puts(body, "    sp_sched_init();\n");
  /* The ARGV copy loop only matters if the program reads ARGV / ARGF / $*. */
  if (g_uses_argv)
    buf_puts(body, "    { sp_argv.len = argc - 1; sp_argv.data = (const char**)malloc(sizeof(const char*) * (size_t)(argc > 1 ? argc - 1 : 1)); for (int _ai = 0; _ai < argc - 1; _ai++) sp_argv.data[_ai] = sp_str_dup_external(argv[_ai + 1]); }\n");
  buf_puts(body, "    sp_program_name = argc > 0 ? argv[0] : \"\";\n");
  /* Enable the backtrace substrate (Exception#backtrace, Kernel#caller) in
     debug builds only: --debug compiles at -O0 with non-inlined methods, so
     the captured frames demangle to Class#method. Optimized/release builds
     leave sp_bt_enabled = 0 (frames inline away), keeping the empty-array
     behavior. */
  if (getenv("SPINEL_DEBUG")) {
    buf_puts(body, "    sp_bt_enabled = 1;\n");
    buf_puts(body, "    sp_bt_srcfile = ");
    emit_str_literal(body, c->nt->source_file ? c->nt->source_file : "source.rb");
    buf_puts(body, ";\n");
  }
  /* Ruby auto-seeds its PRNG at startup, so rand/shuffle/sample vary per
     run. Seed once here; an explicit srand(seed) in user code runs later
     and overrides this for reproducible sequences. Skipped when the program
     consumes no randomness (no time() / srand call at all). */
  if (g_uses_random) buf_puts(body, "    srand((unsigned)time(NULL));\n");
  /* Register END blocks (atexit runs LIFO, so they execute in reverse registration order) */
  for (int e = 1; e <= end_count; e++)
    buf_printf(body, "    atexit(sp_end_fn_%d);\n", e);
  emit_scope_decls(c, &c->scopes[0], body);
  buf_puts(body, "\n");
  /* Hoist BEGIN blocks to run first */
  {
    int top_body = c->scopes[0].body;
    if (top_body >= 0) {
      const char *tty = nt_type(c->nt, top_body);
      int tn = 0;
      const int *tbody = (tty && sp_streq(tty, "StatementsNode"))
                         ? nt_arr(c->nt, top_body, "body", &tn) : NULL;
      for (int k = 0; k < tn; k++) {
        const char *sty = nt_type(c->nt, tbody[k]);
        if (!sty || !sp_streq(sty, "PreExecutionNode")) continue;
        int stmts = nt_ref(c->nt, tbody[k], "statements");
        EMIT_COLLECT_UNIT(emit_stmts(c, stmts, body, 1));
      }
    }
  }
  EMIT_COLLECT_UNIT(emit_stmts(c, c->scopes[0].body, body, 1));
  /* Run any fire-and-forget threads that never got a turn before the program
     exits, so their side effects happen. */
  if (g_uses_threads) buf_puts(body, "    sp_sched_drain();\n");
  if (g_needs_at_exit)
    buf_puts(body, "  { mrb_int _ax_args[16] = {0}; for (mrb_int _ax = sp_at_exit_count - 1; _ax >= 0; _ax--) sp_proc_call(sp_at_exit_hooks[_ax], 0, _ax_args); }\n");
  buf_puts(body, "  return 0;\n}\n");

  emit_regex_section(&b);
  if (g_proc_protos.len) { buf_puts(&b, g_proc_protos.p); buf_puts(&b, "\n"); }
  if (g_procs.len) { buf_puts(&b, g_procs.p); buf_puts(&b, "\n"); }
  buf_puts(&b, body->p ? body->p : "");
  free(body->p);
  free(body);
  free(g_procs.p); free(g_proc_protos.p);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_needs_proc_poly_retslot = 0;
  g_needs_proc_poly_argslot = 0;

  comp_free(c);
  return b.p;
}

