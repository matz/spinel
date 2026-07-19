#include "codegen_internal.h"

/* A reference-backed builtin (IO/Fiber/Thread/Queue/Mutex/ConditionVariable/
   Enumerator/Exception/Proc/Method) is a genuinely nilable C pointer: an unset
   ivar, a `return nil` method, or a cache miss yields NULL. It must box via
   sp_box_nullable_obj so a NULL becomes SP_TAG_NIL rather than a "truthy"
   SP_TAG_OBJ wrapping NULL v.p -- which passes `unless x`/`if x`, misreads as
   non-nil (a wrong `nil?`), then segfaults on the first field/method read.
   Value-type builtins (Range/Time/Complex/Rational, boxed as a by-value copy)
   and non-pointer scalars are deliberately excluded -- they are never NULL. */
const char *ty_nullable_builtin_id(TyKind t) {
  switch (t) {
    case TY_IO:         return "SP_BUILTIN_IO";
    case TY_FIBER:      return "SP_BUILTIN_FIBER";
    case TY_THREAD:     return "SP_BUILTIN_THREAD";
    case TY_QUEUE:      return "SP_BUILTIN_QUEUE";
    case TY_MUTEX:      return "SP_BUILTIN_MUTEX";
    case TY_CONDVAR:    return "SP_BUILTIN_CONDVAR";
    case TY_ENUMERATOR: return "SP_BUILTIN_ENUMERATOR";
    case TY_EXCEPTION:  return "SP_BUILTIN_EXCEPTION";
    case TY_PROC:       return "SP_BUILTIN_PROC";
    case TY_METHOD:     return "SP_BUILTIN_METHOD";
    case TY_DIR:        return "SP_BUILTIN_DIR";
    default:            return NULL;
  }
}

void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  /* An untyped or void value is already emitted as a boxed sp_RbVal (a nil
     sentinel or poly call result) -- or carries side effects to preserve. Box
     it by evaluating for effect and yielding nil, mirroring emit_boxed. Without
     this, the int-boxing fallback below produced sp_box_int(<sp_RbVal>) -- an
     mrb_int slot fed a boxed value (the recurring poly-box bug family). */
  if (t == TY_UNKNOWN || t == TY_VOID || t == TY_REGEX) { buf_printf(b, "(%s, sp_box_nil())", expr); return; }
  /* Reference-backed builtins are nilable C pointers -- box NULL as nil (see
     ty_nullable_builtin_id). This also covers TY_PROC/TY_METHOD, which used to
     fall to the sp_box_proc/sp_box_method switch cases below (both wrapped NULL
     as a truthy proc/method). */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) { buf_printf(b, "sp_box_nullable_obj((void *)(%s), %s)", expr, nbid); return; } }
  if (ty_is_object(t)) {
    /* A reference-type object is a genuinely nilable C pointer (a hash/cache
       lookup or a method that can `return nil` -- e.g. doom's
       TextureManager#[] boxing build_composite's nullable result). Box via
       sp_box_nullable_obj so a NULL pointer becomes a proper SP_TAG_NIL rather
       than a "truthy" SP_TAG_OBJ wrapping NULL v.p, which passes `unless x` and
       then segfaults on the first field/method read (renderer draw_wall_column
       `texture.width`). A value-type object (a small Struct passed by value) is
       never NULL and `expr` is not a pointer, so the plain box is correct. */
    if (comp_ty_value_obj(c, t))
      buf_printf(b, "sp_box_vobj_%s(%s)", c->classes[ty_object_class(t)].c_name, expr);
    else buf_printf(b, "sp_box_nullable_obj((void *)(%s), %d)", expr, ty_object_class(t));
    return;
  }
  if (ty_is_hash(t) && hash_box_cls(t)) { buf_printf(b, "sp_box_obj(%s, %s)", expr, hash_box_cls(t)); return; }
  const char *fn = NULL;
  switch (t) {
    case TY_INT: fn = "sp_box_int"; break;       case TY_FLOAT: fn = "sp_box_float"; break;
    case TY_BIGINT: fn = "sp_box_bigint"; break;
    case TY_STRING: fn = "sp_box_str"; break;     case TY_BOOL: fn = "sp_box_bool"; break;
    case TY_SYMBOL: fn = "sp_box_sym"; break;     case TY_RANGE: fn = "sp_box_range"; break;
    case TY_FLOAT_RANGE: fn = "sp_box_frange"; break;
    case TY_TIME: fn = "sp_box_time"; break;
    case TY_COMPLEX: fn = "sp_box_complex"; break;  case TY_RATIONAL: fn = "sp_box_rational"; break;
    /* TY_PROC / TY_METHOD are handled by the nullable-builtin box above. */
    case TY_CLASS: fn = "sp_box_class"; break;
    case TY_INT_ARRAY: fn = "sp_box_int_array"; break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY: fn = "sp_box_str_array"; break;
    case TY_POLY_ARRAY: fn = "sp_box_poly_array"; break;
    case TY_NIL:
      /* a nil-typed expression can still have side effects (puts/print as a
         block tail): evaluate it for effect, then yield nil */
      buf_printf(b, "(%s, sp_box_nil())", expr && *expr ? expr : "(void)0"); return;
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
  if (t == TY_CLASS) { buf_printf(b, "sp_unbox_class(%s)", expr); return; }  /* a by-value struct, not a pointer (#2797) */
  if (ty_is_object(t)) { buf_printf(b, "(sp_%s *)(%s).v.p", c->classes[ty_object_class(t)].c_name, expr); return; }
  const char *cn = c_type_name(t);
  if (cn) buf_printf(b, "(%s)(%s).v.p", cn, expr);
  else buf_printf(b, "(%s).v.i", expr);
}

/* An unresolved constant read lowers to a runtime NameError raise whose C
   value is an sp_Class struct (the class-position shape). In a scalar slot
   that struct fails the C compile ("incompatible types"), so the scalar
   emitters keep the raise, void the struct, and yield a typed zero -- dead
   code, the raise fires first. Text-matched on the gate's own token, like
   emit_str_expr's sp_raise_nomethod coerce (the node stays TY_UNKNOWN). */
static int coerce_const_raise(const char *txt, const char *zero, Buf *b) {
  if (strncmp(txt, "(sp_raise_cls(", 14) != 0 || !strstr(txt, "(sp_Class)")) return 0;
  buf_printf(b, "((void)%s, %s)", txt, zero);
  return 1;
}

/* Emit a node as an mrb_int, coercing a poly value through sp_poly_to_i. Used
   where the runtime ABI demands a raw integer (array indices, etc.) but the
   expression's static type widened to poly. */
void emit_int_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_i("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  emit_scalar_operand(c, node, "0", b);
}

/* Emit a node as an mrb_float. A poly value is unboxed via sp_poly_to_f; any
   other (numeric) value is plain-cast, matching the legacy `(mrb_float)(...)`. */
void emit_float_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_f("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  if (!coerce_const_raise(tmp.p ? tmp.p : "", "0.0", b)) {
    buf_puts(b, "(mrb_float)(");
    buf_puts(b, tmp.p ? tmp.p : "");
    buf_puts(b, ")");
  }
  free(tmp.p);
}

/* See codegen_internal.h. */
void emit_scalar_operand(Compiler *c, int node, const char *zero, Buf *b) {
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  if (!coerce_const_raise(tmp.p ? tmp.p : "", zero, b)) buf_puts(b, tmp.p ? tmp.p : "");
  free(tmp.p);
}

/* Emit a node as a `const char *` string. A poly value (e.g. a `String | nil`
   local narrowed to String by `is_a?(String)`, which keeps an sp_RbVal
   representation) is unboxed via sp_poly_to_s; a string-typed value emits
   directly. Used at string-primitive arg boundaries (sp_str_include, ...). */
void emit_str_expr(Compiler *c, int node, Buf *b) {
  if (comp_ntype(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_s("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  /* The unresolved-call gate's sp_raise_nomethod(...) is a side-effecting poly
     value (it raises): coerce it to the const char* slot, keeping the call,
     rather than passing the sp_RbVal through raw (doom's
     `File.join(Dir.tmpdir, ...)`). A text match on the gate's own token is
     reliable where comp_ntype is not (the node stays TY_UNKNOWN). */
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  const char *txt = tmp.p ? tmp.p : "";
  if (strncmp(txt, "sp_raise_nomethod(", 18) == 0)
    buf_printf(b, "sp_poly_to_s(%s)", txt);
  else buf_puts(b, txt);
  free(tmp.p);
}

/* Coerce an unresolved-call value into a concretely-typed slot. An unresolved
   call is typed TY_UNKNOWN and lowers to the gate's sp_raise_nomethod(...) poly
   token (an sp_RbVal that always raises); when it lands in a non-poly slot the
   emitted C would assign sp_RbVal to a scalar/pointer. The token never returns,
   so any type-correct wrapper keeps the C compiling: coerce it to `target`.
   A value that is NOT the token is emitted raw -- callers reach this only for a
   TY_UNKNOWN RHS, where a raw emit is exactly the prior behavior. Returns 1 if
   it coerced the token, 0 if it emitted raw. */
int emit_unresolved_coerced(Compiler *c, int node, TyKind target, Buf *b) {
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  const char *txt = tmp.p ? tmp.p : "";
  int is_tok = strncmp(txt, "sp_raise_nomethod(", 18) == 0;
  /* The missing-super arm emits a `(sp_raise_cls(...), 0)` comma expression
     whose dummy tail is a bare int; in a pointer-typed slot that is ill-typed
     C. The raise never returns, so evaluate it for the raise and yield the
     slot's default instead. */
  int is_cls_tok = strncmp(txt, "(sp_raise_cls(", 14) == 0;
  if (is_tok) {
    if (target == TY_STRING) buf_printf(b, "sp_poly_to_s(%s)", txt);
    else if (target == TY_FLOAT) buf_printf(b, "sp_poly_to_f(%s)", txt);
    else if (target == TY_SYMBOL) buf_printf(b, "(sp_sym)sp_poly_to_i(%s)", txt);
    else if (target == TY_INT || target == TY_BOOL) buf_printf(b, "sp_poly_to_i(%s)", txt);
    else if (target == TY_POLY) buf_puts(b, txt);   /* already sp_RbVal */
    else emit_unbox_text(c, target, txt, b);         /* pointer/object/hash slot */
  }
  else if (is_cls_tok && target != TY_POLY && target != TY_UNKNOWN) {
    buf_printf(b, "({ (void)%s; %s; })", txt, default_value(target));
    is_tok = 1;
  }
  else buf_puts(b, txt);
  free(tmp.p);
  return is_tok;
}

/* A handful of builtins are typed TY_INT but return the SP_INT_NIL sentinel for
   their nil case (bsearch/bsearch_index find-miss, nonzero?/infinite? on the
   boundary, MatchData#begin/end of an unmatched optional group). Typed
   operations sentinel-check, but boxing the raw int into poly (e.g. through a
   `.should` receiver) would carry the sentinel as a truthy integer, so `== nil`
   fails. Box these through sp_box_int_or_nil. Kept to the specific nullable
   builtins so the hot int-into-poly path (optcarrot's pixels) stays sp_box_int. */
static int call_returns_nullable_int(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, node);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int blk = nt_ref(nt, node, "block");
  if ((sp_streq(nm, "bsearch") || sp_streq(nm, "bsearch_index")) && blk >= 0) return 1;
  if (sp_streq(nm, "nonzero?") || sp_streq(nm, "infinite?") || sp_streq(nm, "getbyte")) return 1;
  /* String#index/rindex (search miss -> nil) and Array element removers
     (delete_at/pop/shift/delete out of range / not found -> nil) are typed
     TY_INT when the element/position is an int; box_int_or_nil is a no-op on a
     real int, so the name gate plus the TY_INT case is enough. */
  if (sp_streq(nm, "index") || sp_streq(nm, "rindex") || sp_streq(nm, "delete_at") ||
      sp_streq(nm, "byteindex") || sp_streq(nm, "byterindex") ||
      sp_streq(nm, "pop") || sp_streq(nm, "shift") || sp_streq(nm, "delete")) return 1;
  if (sp_streq(nm, "begin") || sp_streq(nm, "end")) {
    int r = nt_ref(nt, node, "receiver");
    if (r >= 0 && comp_ntype(c, r) == TY_MATCHDATA) return 1;
  }
  return 0;
}

/* assoc/rassoc/dig return a [k,v] pair (or the dug value) or nil; the miss case
   yields a NULL sp_PolyArray*, which sp_box_poly_array would box as a truthy
   SP_TAG_OBJ over NULL (so `== nil` fails). A real result array is never NULL,
   so a NULL unambiguously means nil -- box these through sp_box_nullable_obj. */
static int call_returns_nullable_array(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, node);
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  const char *nm = nt_str(nt, node, "name");
  return nm && (sp_streq(nm, "assoc") || sp_streq(nm, "rassoc") || sp_streq(nm, "dig"));
}

void emit_boxed(Compiler *c, int node, Buf *b) {
  /* Parentheses are transparent: box the inner expression directly so a
     wrapped yield (`out << (yield x)`) reaches the per-call-site yield boxing
     below rather than being boxed by the shared node type (#2454). */
  {
    const char *pty = nt_type(c->nt, node);
    if (pty && sp_streq(pty, "ParenthesesNode")) {
      int pbody = nt_ref(c->nt, node, "body"); int pbn = 0;
      const int *pbd = pbody >= 0 ? nt_arr(c->nt, pbody, "body", &pbn) : NULL;
      if (pbn == 1) { emit_boxed(c, pbd[0], b); return; }
    }
  }
  {
    const char *bty0 = nt_type(c->nt, node);
    /* `*x` in a boxed value position (break *x / next *x): Ruby's
       splat-to-array (nil -> [], array -> itself, scalar -> [v]). */
    if (bty0 && sp_streq(bty0, "SplatNode")) {
      int inner0 = nt_ref(c->nt, node, "expression");
      buf_puts(b, "sp_splat_to_array(");
      if (inner0 >= 0) emit_boxed(c, inner0, b); else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ")");
      return;
    }
  }
  TyKind t = comp_ntype(c, node);
  /* Lowered self-recursive yield in a boxed value position (a `{ yield }` block
     whose value rides the universal proc slot): the enclosing method's block is
     the runtime __yblk__ proc, which publishes its boxed result into
     _sp_proc_poly_ret. Call it for effect and take the boxed slot -- do NOT
     fall through to the no-block raise below (a lowered scope has g_block_id
     == -1 but is not a missing-block case). Checked before the raise because
     emit_boxed, unlike the expr-position YieldNode emitter, is now reached for
     a lowered yield tail once every proc returns through the boxed channel. */
  if (nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode") && g_current_scope_is_lowered) {
    int yargs = nt_ref(c->nt, node, "arguments");
    int yargc = 0; const int *yargv = yargs >= 0 ? nt_arr(c->nt, yargs, "arguments", &yargc) : NULL;
    buf_puts(b, "((void)sp_proc_call(");
    emit_yblk_ref(b);
    buf_puts(b, ", ");
    emit_proc_call_args(c, yargc, yargv, b, 1);
    buf_puts(b, ", _sp_proc_poly_ret)");
    return;
  }
  /* An inlined yield's value type is per-CALL-SITE: the method AST has ONE
     YieldNode but each call site supplies its own block, so the node's cached
     type is whichever site inference visited (a string-block site poisoned an
     int-block site into sp_box_str(int) -- a segfault). Box by the CURRENT
     block's inferred result instead. */
  if (nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode") && g_block_id < 0) {
    /* An unguarded yield with no block raises LocalJumpError. A guarded yield
       folds its guard to a compile-time false and sits inside an `if (0)`, so
       the raise never executes there; the boxed nil keeps the comma expression
       well-typed for the value position. */
    buf_puts(b, "(sp_exc_stage_key(sp_box_str((&(\"\\xff\" \"noreason\")[1]))), "
                "sp_raise_cls(\"LocalJumpError\", \"no block given (yield)\"), sp_box_nil())");
    return;
  }
  if (g_block_id >= 0 && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode")) {
    int bbody = nt_ref(c->nt, g_block_id, "body");
    int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
    TyKind bt = bn > 0 ? comp_ntype(c, bb[bn - 1]) : TY_NIL;
    if (bt != t && bt != TY_UNKNOWN) {
      if (bt == TY_POLY) { emit_expr(c, node, b); return; }
      Buf yb; memset(&yb, 0, sizeof yb);
      emit_expr(c, node, &yb);
      if (bt == TY_NIL || bt == TY_VOID)
        buf_printf(b, "({ %s; sp_box_nil(); })", yb.p ? yb.p : "0");
      else
        emit_boxed_text(c, bt, yb.p ? yb.p : "0", b);
      free(yb.p);
      return;
    }
  }
  if (t == TY_POLY) { emit_expr(c, node, b); return; }
  /* Reference-backed builtins (IO/Fiber/Thread/Queue/Mutex/ConditionVariable/
     Enumerator/Exception/Proc/Method) are nilable C pointers -- box NULL as nil
     via sp_box_nullable_obj, not a truthy SP_TAG_OBJ over NULL. */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) {
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_printf(b, "), %s)", nbid);
      return;
    } }
  if (ty_is_object(t)) {
    /* A reference-type object is a nilable C pointer (a hash/cache lookup or a
       method that can `return nil`, e.g. doom's TextureManager#[] boxing
       build_composite's nullable result). Box via sp_box_nullable_obj so a
       NULL pointer becomes SP_TAG_NIL rather than a truthy SP_TAG_OBJ over a
       NULL v.p (which passes `unless x` then segfaults on the first field
       read). A value-type object is never NULL and is not a pointer. */
    int is_val = comp_ty_value_obj(c, t);
    if (is_val) buf_printf(b, "sp_box_vobj_%s(", c->classes[ty_object_class(t)].c_name);
    else buf_puts(b, "sp_box_nullable_obj((void *)(");
    emit_expr(c, node, b);
    buf_printf(b, is_val ? ")" : "), %d)", ty_object_class(t));
    return;
  }
  if (ty_is_hash(t)) {
    const char *hid = hash_box_cls(t);
    if (hid) { buf_printf(b, "sp_box_obj("); emit_expr(c, node, b); buf_printf(b, ", %s)", hid); return; }
    unsupported(c, node, "boxing value into poly"); return;
  }
  /* regex values can appear in poly context (multi-typed local); evaluate for
     side effects (e.g. Regexp.new(str)) and yield nil */
  if (t == TY_REGEX) { buf_puts(b, "sp_box_regexp("); emit_expr(c, node, b); buf_puts(b, ")"); return; }
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
  /* Hash.new / Hash.new(default) whose variant no key usage ever narrowed:
     box an empty PolyPolyHash carrying the default (it used to fall to the
     constant path and raise "uninitialized constant Hash"). */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "CallNode") &&
      nt_str(c->nt, node, "name") && sp_streq(nt_str(c->nt, node, "name"), "new") &&
      nt_ref(c->nt, node, "block") < 0) {
    int hrecv = nt_ref(c->nt, node, "receiver");
    const char *hty = hrecv >= 0 ? nt_type(c->nt, hrecv) : NULL;
    const char *hcn = hrecv >= 0 ? nt_str(c->nt, hrecv, "name") : NULL;
    int han = 0; int haN = nt_ref(c->nt, node, "arguments");
    const int *hav = haN >= 0 ? nt_arr(c->nt, haN, "arguments", &han) : NULL;
    if (hty && (sp_streq(hty, "ConstantReadNode") || sp_streq(hty, "ConstantPathNode")) &&
        hcn && sp_streq(hcn, "Hash") && han <= 1) {
      if (han == 1 && hav) {
        buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new_with_default(");
        emit_boxed(c, hav[0], b);
        buf_puts(b, "), SP_BUILTIN_POLY_POLY_HASH)");
      }
      else buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)");
      return;
    }
  }
  const char *fn = NULL;
  switch (t) {
    /* A nullable-int sentinel (SP_INT_NIL) only flows into a poly box under
       --int-overflow=promote (where int? widens to poly); in default/wrap mode
       a real int is never the sentinel, so skip the per-box check there -- it is
       on the hot path (every int boxed into poly, e.g. optcarrot's pixels). */
    case TY_INT:    fn = (g_promote_mode || call_returns_nullable_int(c, node)) ? "sp_box_int_or_nil" : "sp_box_int"; break;
    case TY_FLOAT:  fn = "sp_box_float"; break;
    case TY_BIGINT: fn = "sp_box_bigint"; break;
    case TY_STRING: fn = "sp_box_str";   break;
    case TY_BOOL:   fn = "sp_box_bool";  break;
    case TY_SYMBOL: fn = "sp_box_sym";   break;
    case TY_RANGE:  fn = "sp_box_range"; break;
    case TY_FLOAT_RANGE: fn = "sp_box_frange"; break;
    case TY_TIME:   fn = "sp_box_time";  break;
    case TY_COMPLEX:  fn = "sp_box_complex";  break;
    case TY_RATIONAL: fn = "sp_box_rational"; break;
    /* TY_PROC / TY_METHOD are handled by the nullable-builtin box above. */
    case TY_INT_ARRAY:   fn = "sp_box_int_array";   break;
    case TY_FLOAT_ARRAY: fn = "sp_box_float_array"; break;
    case TY_STR_ARRAY:   fn = "sp_box_str_array";   break;
    case TY_POLY_ARRAY:
      if (call_returns_nullable_array(c, node)) {
        buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
        buf_puts(b, "), SP_BUILTIN_POLY_ARRAY)"); return;
      }
      fn = "sp_box_poly_array";  break;
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
    case TY_FLOAT_RANGE: buf_puts(&cty, "sp_FloatRange"); init = "{0}"; break;
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
  /* A real (non-yield-inlined) &blk param is an sp_Proc * C parameter; root it
     so the proc box survives a GC fired by an allocation in the block body (or
     by the cell allocations just below). Without this the box's only reference
     is an untracked C parameter -- use-after-free on the next blk.call. */
  if (s->blk_param && s->blk_param[0] && !s->yields)
    buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", s->blk_param);
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
    /* Byref string out-param: the C parameter already IS the cell (the
       caller's rooted slot), so no heap cell, no copy-in, and no root --
       the pointee lives in the caller's frame, not on the GC heap. */
    if (lv->byref_out) continue;
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
      /* a Symbol is int-represented (sp_sym), so it rides the mrb_int cell */
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_SYMBOL &&
          lv->type != TY_UNKNOWN && !ptr_cell)
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
    buf_printf(b, "sp_%s_s_%s", c->classes[s->class_id].c_name, mc(s->name));
  else if (s->class_id >= 0)
    buf_printf(b, "sp_%s_%s", c->classes[s->class_id].c_name, mc(s->name));
  else
    buf_printf(b, "sp_%s", mc(s->name));
}

/* A poly each-receiver can be a user object at runtime (e.g. a Set operand
   whose parameter also sees Array arguments at another call site, so it
   widened to poly). The arr_len/each_elem lowering only understands builtin
   containers -- normalize such an object through its 0-arg #to_a (when the
   class defines one returning a concrete array) so the loop iterates its
   elements. `tv` names an already-rooted sp_RbVal temp; emits nothing when
   no instantiated class qualifies. */
void emit_poly_iter_obj_normalize(Compiler *c, int tv, Buf *b) {
  Buf arms; memset(&arms, 0, sizeof arms);
  for (int k = 0; k < c->nclasses; k++) {
    /* a never-instantiated class can't be the runtime class (#1608) */
    if (!c->classes[k].instantiated) continue;
    int mi = comp_method_in_chain(c, k, "to_a", NULL);
    if (mi < 0 || c->scopes[mi].nrequired != 0 || c->scopes[mi].is_cmethod) continue;
    TyKind ret = (TyKind)c->scopes[mi].ret;
    const char *box = ret == TY_POLY_ARRAY  ? "sp_box_poly_array"
                    : ret == TY_INT_ARRAY   ? "sp_box_int_array"
                    : ret == TY_STR_ARRAY   ? "sp_box_str_array"
                    : ret == TY_FLOAT_ARRAY ? "sp_box_float_array" : NULL;
    if (!box) continue;
    buf_printf(&arms, " case %d: _t%d = %s(", k, tv, box);
    emit_method_cname(c, &c->scopes[mi], &arms);
    buf_printf(&arms, "((sp_%s *)_t%d.v.p)); break;", c->classes[k].c_name, tv);
  }
  if (arms.p && arms.p[0])
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0) switch (_t%d.cls_id) {%s }\n",
               tv, tv, tv, arms.p);
  free(arms.p);
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
    const char *cn = c->classes[s->class_id].c_name;
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
    /* byref string out-param: the caller's slot, so body mutation propagates.
       Named _cell_<name> so the ordinary is_cell deref forms read/write it. */
    if (p && p->byref_out) {
      buf_printf(b, "const char * *_cell_%s", s->pnames[i]);
      continue;
    }
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
    const char *ecn = c->classes[defcls].c_name;
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
      dl->byref_out = sl.byref_out;
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
  const char *saved_lbn = g_lowered_blk_name;
  g_lowered_blk_name = s->is_lowered_yield ? s->blk_param : NULL;
  /* a method body is a fresh break context: a stray enclosing serial must
     not leak into it (its own wrapped iterators re-establish scopes) */
  const char *saved_bser = g_brk_ser_var; g_brk_ser_var = NULL;
  int saved_bskip = g_brk_skip_id; g_brk_skip_id = -1;
  /* value-type reader methods receive self by value, so ivar access uses `.` */
  const char *saved_deref = g_self_deref;
  g_self_deref = (s->class_id >= 0 && !s->is_cmethod && c->classes[s->class_id].is_value_type &&
                  s->name && !sp_streq(s->name, "initialize")) ? "." : "->";
  /* inside a class method, bare `self` is the Class object -- there is no
     `self` C parameter to name (#2443) */
  const char *saved_self9 = g_self;
  char cm_self9[32];
  if (s->class_id >= 0 && s->is_cmethod) {
    snprintf(cm_self9, sizeof cm_self9, "((sp_Class){%d})", s->class_id);
    g_self = cm_self9;
  }
  g_ret_type = method_is_void(s) ? TY_VOID : s->ret;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0; g_rescue_save_depth = 0;
  /* real-function funnel mirror: no proc-return frame yet (set below when
     one exists); block bodies spliced by yield-inlines restore from these. */
  g_fn_pr_label = NULL; g_fn_pr_var = NULL; g_fn_ret_type = g_ret_type;
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
    g_fn_pr_label = g_method_pr_label; g_fn_pr_var = g_method_pr_var;
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
      if (comp_ty_value_obj(c, s->ret)) buf_printf(b, "(sp_%s){0};\n", c->classes[ty_object_class(s->ret)].c_name);
      else buf_puts(b, "NULL;\n"); /* unreachable default (object pointer) */
    }
    else buf_printf(b, "%s;\n", default_value(s->ret));
  }
  g_result_var = sv_rv2; g_result_poly = sv_rp2;
  g_method_pr_label = NULL; g_method_pr_var = NULL;
  g_fn_pr_label = NULL; g_fn_pr_var = NULL; g_fn_ret_type = TY_UNKNOWN;
  g_self_deref = saved_deref;
  g_self = saved_self9;
  g_ret_type = saved_rt; g_ensure_depth = saved_ed;
  g_emitting_class_id = saved_emcls;
  g_dm_subst_name = saved_dmn; g_dm_subst_node = saved_dmnode;
  g_current_scope_is_lowered = saved_lowered;
  g_lowered_blk_name = saved_lbn;
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
/* rest / post parameter accessors of a proc-creating node. A splat rest
   (|*a, b|) and trailing post-required params were previously invisible to
   the classifier, so their names were misdiagnosed as uncaptured OUTER
   variables (the ruby/spec harness's biggest implementable cluster). */
const char *proc_rest_name(Compiler *c, int create) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return NULL;
  int r = nt_ref(c->nt, pn, "rest");
  if (r < 0) return NULL;
  const char *rt = nt_type(c->nt, r);
  if (!rt || !sp_streq(rt, "RestParameterNode")) return NULL;
  return nt_str(c->nt, r, "name");
}
int proc_has_rest(Compiler *c, int create) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return 0;
  int r = nt_ref(c->nt, pn, "rest");
  if (r < 0) return 0;
  const char *rt = nt_type(c->nt, r);
  return rt && sp_streq(rt, "RestParameterNode");
}
int proc_post_count(Compiler *c, int create) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return 0;
  int n = 0; nt_arr(c->nt, pn, "posts", &n);
  return n;
}
const char *proc_post_name(Compiler *c, int create, int idx) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return NULL;
  int n = 0; const int *posts = nt_arr(c->nt, pn, "posts", &n);
  return idx < n ? nt_str(c->nt, posts[idx], "name") : NULL;
}

/* Numbered parameters (_1.._9): they surface as plain LocalVariableReadNodes
   with no parameters node at all, so the classifier must derive them from the
   used-name set. Returns the highest _N used (0 when none). Only meaningful
   when the proc declares no explicit parameters (Ruby forbids mixing). */
int proc_numbered_max(const NameSet *used) {
  int mx = 0;
  for (int i = 0; i < used->n; i++) {
    const char *nm = used->v[i];
    if (nm && nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' && nm[2] == '\0') {
      int k = nm[1] - '0';
      if (k > mx) mx = k;
    }
  }
  return mx;
}

int proc_opt_count(Compiler *c, int create) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return 0;
  int n = 0; nt_arr(c->nt, pn, "optionals", &n);
  return n;
}
const char *proc_opt_name(Compiler *c, int create, int idx) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return NULL;
  int n = 0; const int *opts = nt_arr(c->nt, pn, "optionals", &n);
  return idx < n ? nt_str(c->nt, opts[idx], "name") : NULL;
}
int proc_opt_value(Compiler *c, int create, int idx) {
  int pn = proc_params_node(c, create);
  if (pn < 0) return -1;
  int n = 0; const int *opts = nt_arr(c->nt, pn, "optionals", &n);
  return idx < n ? nt_ref(c->nt, opts[idx], "value") : -1;
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
         t == TY_EXCEPTION ||
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

/* The size argument for a generator Enumerator (Enumerator.new(size) { |y| },
   or the threaded __size of a to_enum size-callable): a boxed value/proc, or nil
   when none was given. #size returns it (calling it when it is a Proc). */
static void emit_enum_size_arg(Compiler *c, int size_node, Buf *b) {
  if (size_node >= 0) emit_boxed(c, size_node, b);
  else buf_puts(b, "sp_box_nil()");
}


/* Is `id` a yielder push -- `y << v` on the generator's yielder param `yname`?
   Such a statement lowers to sp_Fiber_yield but its CRuby value is the yielder
   itself (not modeled), so as a generator's terminal statement it must force a
   nil StopIteration#result rather than be captured as yielded_value. `y.yield(v)`
   is deliberately excluded: it returns the fed value, which IS the correct
   result, so it compiles through the normal terminal path. */
static int stmt_is_yielder_push(Compiler *c, int id, const char *yname) {
  const NodeTable *nt = c->nt;
  if (id < 0 || !yname || !nt_type(nt, id) || !sp_streq(nt_type(nt, id), "CallNode")) return 0;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "<<")) return 0;
  int rcv = nt_ref(nt, id, "receiver");
  if (rcv < 0 || !nt_type(nt, rcv) || !sp_streq(nt_type(nt, rcv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, rcv, "name");
  return rn && sp_streq(rn, yname);
}


void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen, int size_node) {
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  if (blk < 0) {
    if (as_gen) { buf_puts(b, "sp_Enumerator_new_gen(NULL, NULL, "); emit_enum_size_arg(c, size_node, b); buf_puts(b, ")"); }
    else buf_puts(b, "sp_Fiber_new(NULL)");
    return;
  }

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
  int self_is_value = 0;   /* value-type self is captured by value (sp_X), not sp_X* */
  int self_is_ptr = 1;     /* but a value-type `initialize` still receives self as sp_X* */
  if (encl && encl->class_id >= 0 && !encl->is_cmethod && body >= 0 && fiber_body_uses_self(c, body)) {
    cap_self = 1;
    cap_self_class = c->classes[encl->class_id].c_name;
    self_is_value = c->classes[encl->class_id].is_value_type;
    self_is_ptr = !self_is_value || (encl->name && sp_streq(encl->name, "initialize"));
  }

  /* Emit capture struct + GC scan function when there are captured vars or self */
  if (ncap > 0 || cap_self) {
    buf_printf(&g_proc_protos, "typedef struct {");
    if (cap_self) buf_printf(&g_proc_protos, self_is_value ? " sp_%s self_val;" : " sp_%s *self_ptr;", cap_self_class);
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
    if (cap_self && !self_is_value)
      buf_printf(&g_proc_protos, "  if (_c->self_ptr) sp_gc_mark((void *)_c->self_ptr);\n");
    else if (cap_self && class_needs_scan(&c->classes[encl->class_id]))
      buf_printf(&g_proc_protos, "  sp_%s_scan(&_c->self_val);\n", cap_self_class);
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
  /* Value-type self is captured by value (sp_X self), so ivar access in the
     body uses `.`; a pointer self uses `->`. Override the global for the body
     (restored below). */
  const char *sv_fbderef = g_self_deref;
  g_self_deref = (cap_self && self_is_value) ? "." : "->";
  const char *sv_fn_prl2 = g_fn_pr_label, *sv_fn_prv2 = g_fn_pr_var; TyKind sv_fn_rt2 = g_fn_ret_type;
  g_fn_pr_label = NULL; g_fn_pr_var = NULL; g_fn_ret_type = TY_POLY;
  const char *sv_fbser = g_brk_ser_var; g_brk_ser_var = NULL;   /* fresh function context */
  int sv_fbskip = g_brk_skip_id; g_brk_skip_id = -1;
  int sv_fbexcd = g_exc_frame_depth, sv_fbprexcd = g_method_pr_exc_depth;
  int sv_fbrsd = g_rescue_save_depth;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0; g_rescue_save_depth = 0;

  /* Unpack capture struct */
  if (ncap > 0 || cap_self) {
    buf_printf(pb, "    _fib_cap_%d *_fc = (_fib_cap_%d *)_fb->user_data;\n", fid, fid);
    if (cap_self && self_is_value) {
      /* value-type self: a by-value copy; its heap fields stay reachable through
         the rooted capture struct (scanned above), so no separate root. */
      const char *svar = sv_self ? sv_self : "self";
      buf_printf(pb, "    sp_%s %s = _fc->self_val;\n", cap_self_class, svar);
    }
    else if (cap_self) {
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

  /* Emit body: all-but-last as side-effect statements, last sets yielded_value.
     For void/nil last statements emit as stmt first then set yielded=nil. A
     generator (as_gen) yields imperatively via `y << v` mid-body, but its final
     body value still lands in yielded_value: that is the value read on the
     terminating resume, which sp_enum_gen_pull surfaces as StopIteration#result. */
  if (body >= 0) {
    int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
    for (int k = 0; k < bn - 1; k++) emit_stmt(c, bb[k], pb, 1);
    if (bn > 0) {
      int last = bb[bn - 1];
      TyKind lty = comp_ntype(c, last);
      if (as_gen && stmt_is_yielder_push(c, last, bp0)) {
        /* A generator ending in a bare `y << v` yields v, then terminates with a
           nil result (its value is the yielder in CRuby -- not modeled). */
        emit_stmt(c, last, pb, 1);
        buf_puts(pb, "    _fb->yielded_value = sp_box_nil();\n");
      }
      else if (lty == TY_VOID || lty == TY_UNKNOWN) {
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
  g_self_deref = sv_fbderef;
  g_result_poly = sv_rp; g_result_var = sv_rv; g_yielder_name = sv_yld;
  g_fn_pr_label = sv_fn_prl2; g_fn_pr_var = sv_fn_prv2; g_fn_ret_type = sv_fn_rt2;
  g_brk_ser_var = sv_fbser; g_brk_skip_id = sv_fbskip;
  g_exc_frame_depth = sv_fbexcd; g_method_pr_exc_depth = sv_fbprexcd;
  g_rescue_save_depth = sv_fbrsd;

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
    /* Root the capture struct before it is handed off: for a generator it goes
       straight into sp_Enumerator_new_gen, whose enumerator allocation can
       trigger a GC while the struct is reachable only through this unrooted C
       local (the !as_gen path links it into the already-rooted fiber below, but
       rooting here covers both uniformly). */
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", tc);
    if (cap_self) {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, self_is_value ? (self_is_ptr ? "_t%d->self_val = *%s;\n"
                                                      : "_t%d->self_val = %s;\n")
                                      : "_t%d->self_ptr = %s;\n",
                 tc, sv_self ? sv_self : "self");
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
      buf_printf(b, "sp_Enumerator_new_gen(%s, _t%d, ", fname, tc);
      emit_enum_size_arg(c, size_node, b);
      buf_puts(b, ")");
    }
    else {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "_t%d->user_data = _t%d;\n", tf, tc);
      buf_printf(b, "_t%d", tf);
    }
  }
  else if (as_gen) {
    buf_printf(b, "sp_Enumerator_new_gen(%s, NULL, ", fname);
    emit_enum_size_arg(c, size_node, b);
    buf_puts(b, ")");
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
  const char *restn = proc_rest_name(c, create);
  int nposts = proc_post_count(c, create);
  if (restn && restn[0]) nameset_add(&params, restn);
  for (int j = 0; j < nposts; j++) {
    const char *pp = proc_post_name(c, create, j);
    if (pp) nameset_add(&params, pp);
  }
  /* optional and keyword params bind below (CRuby distribution / extraction
     from the call-site kwargs hash); their NAMES join the param set so they
     are never misdiagnosed as uncaptured outer variables. */
  int nopts = proc_opt_count(c, create);
  int nkw = 0;
  { int pn0 = proc_params_node(c, create);
    const int *kws  = pn0 >= 0 ? nt_arr(nt, pn0, "keywords", &nkw) : NULL;
    for (int j = 0; j < nopts; j++) {
      const char *on = proc_opt_name(c, create, j);
      if (on) nameset_add(&params, on);
    }
    for (int j = 0; j < nkw; j++) {
      const char *kn = nt_str(nt, kws[j], "name");
      if (kn) nameset_add(&params, kn);
    }
    /* `&b` binds from the block side-channel in the prologue below */
    {
      int bpar0 = pn0 >= 0 ? nt_ref(nt, pn0, "block") : -1;
      const char *bpty0 = bpar0 >= 0 ? nt_type(nt, bpar0) : NULL;
      if (bpty0 && sp_streq(bpty0, "BlockParameterNode")) {
        const char *bpn0 = nt_str(nt, bpar0, "name");
        if (bpn0) nameset_add(&params, bpn0);
      }
    }
    /* `**kw` binds the whole trailing kwargs hash in the prologue below --
       only the collect-all form; alongside named keywords the remainder split
       is not implemented, so that mix keeps the old diagnostic (#2648) */
    int kwrest0 = pn0 >= 0 ? nt_ref(nt, pn0, "keyword_rest") : -1;
    const char *kwrty0 = kwrest0 >= 0 ? nt_type(nt, kwrest0) : NULL;
    if (kwrty0 && sp_streq(kwrty0, "KeywordRestParameterNode") && nkw == 0) {
      const char *krn = nt_str(nt, kwrest0, "name");
      if (krn) nameset_add(&params, krn);
    }
  }
  proc_collect_used(c, body, &used);
  int nnumbered = 0;
  if (arity == 0 && nposts == 0 && !(restn && restn[0]) && nopts == 0) {
    nnumbered = proc_numbered_max(&used);
    for (int k = 1; k <= nnumbered; k++) {
      /* NameSet stores the POINTER: use the scope-interned stable name, not
         a stack buffer. The analyze pass interned _k on this scope already. */
      char nb[4] = { '_', (char)('0' + k), 0, 0 };
      LocalVar *nlv = scope_local_intern(bs, nb);
      nameset_add(&params, nlv->name);
    }
  }
  /* Keyword params bind in the prologue below (extracted by name from the
     call-site kwargs hash delivered on the boxed proc ABI). */
  /* deep: include nested blocks' params/locals so a name used only inside a
     nested block in the proc is classified as body-local, not flagged as an
     uncaptured outer variable. */
  collect_locals_deep(c, body, &locals);
  for (int u = 0; u < used.n; u++) {
    const char *nm = used.v[u];
    if (nameset_has(&params, nm)) continue;
    LocalVar *lv = scope_local(bs, nm);
    if (lv && lv->is_cell) {
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      /* Float captures ride the capture struct (a real mrb_float field), not the
         proc's argument slot, so they are safe even alongside a float parameter
         -- a first-class proc's `.call` passes float args through the boxed
         side-channel (sp_box_float / sp_poly_to_f), not the truncating slot. */
      int float_cell = lv->type == TY_FLOAT;
      int poly_cell = lv->type == TY_POLY;
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_SYMBOL &&
          lv->type != TY_UNKNOWN &&
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
     enclosing method's block param (declared &block name or the synthetic
     __yblk__) down via capture.  The YieldNode is not a LocalVariableRead so
     proc_collect_used never picks it up -- force it. */
  if (g_current_scope_is_lowered) {
    int pb2 = proc_body_node(c, create);
    /* g_lowered_blk_name, not bs->blk_param: for an include-transplanted
       method comp_scope_of maps body nodes to the SOURCE scope, while the
       name in force is the emitting copy's -- the same one emit_yblk_ref
       will reference inside the proc body. */
    const char *ybn =
        (g_lowered_blk_name && g_lowered_blk_name[0]) ? g_lowered_blk_name : "__yblk__";
    if (pb2 >= 0 && proc_body_has_yield(c, pb2) && !nameset_has(&caps, ybn)) {
      LocalVar *yblk_lv = scope_local(bs, ybn);
      if (yblk_lv && yblk_lv->is_cell) nameset_add(&caps, ybn);
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
  /* numbered params have no parameters node; the highest _N used is the
     mandatory count (-> { _2 }.arity == 2). */
  if (nnumbered > 0) meta_arity = nnumbered;
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
  /* Universal boxed return (CRuby's uniform proc VALUE ABI): every
     value-carrying proc publishes its result through the _sp_proc_poly_ret
     slot, boxed, regardless of the value's static type. Compiling the body as
     a poly return routes every exit -- implicit tail, explicit return, break,
     next, and ensure-deferred return -- through the one slot-writing path, so
     the call site can read the slot for ANY proc. A polymorphic call site (a
     stored &blk, a forwarded proc) can no longer read an unwritten slot behind
     a raw-only body. The call site re-derives the proc's true return type and
     unboxes, so no caller observes a poly value; the raw mrb_int carrier is
     unused (`return 0`). optcarrot emits no first-class proc calls, so this is
     perf-neutral there; a hot .call boxes/unboxes a tagged scalar (no alloc),
     exactly as CRuby does. */
  if (ret != TY_VOID && ret != TY_NIL) ret = TY_POLY;
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
  /* boxed through the poly return slot: a float unboxes via sp_poly_to_f at the
     call site; a range/time (by-value structs that don't fit the mrb_int
     carrier) box via sp_box_range/sp_box_time and the call site dereferences
     the heap copy back to the value. */
  int ret_fbox = (ret == TY_FLOAT || ret == TY_RANGE || ret == TY_TIME);
  if (!proc_slot_is_direct(ret) && !ret_ptr && !ret_no_value && !ret_poly && !ret_fbox) {
    free(params.v); free(used.v); free(locals.v); free(caps.v);
    unsupported(c, create, "proc with unsupported return kind");
    return;
  }

  int pid = ++g_proc_counter;
  int ncap = caps.n;
  /* A block that references self and escapes into a real proc must capture self
     through _cap (#1436). A heap-object self is captured by pointer; a
     value-type (by-value) self is captured by value in a `sp_X __self_val`
     field. A class-method self has no instance and is left as-is. */
  int cap_self = bs && bs->class_id >= 0 && !bs->is_cmethod &&
                 proc_body_uses_self(c, body, bs->class_id);
  int self_is_value = cap_self && c->classes[bs->class_id].is_value_type;
  const char *self_cls = cap_self ? c->classes[bs->class_id].c_name : NULL;

  /* parameter metadata for Proc#parameters: every parameter kind in signature
     order. Positionals (leading + post) are :req for a lambda and :opt for a
     proc; defaulted positionals are :opt in both; then :rest, :keyreq / :key,
     :keyrest, :block. An anonymous rest/kwrest/block reports the CRuby
     placeholder name (:*, :**, :&); numbered params report as (:opt, :_N).
     Kinds and names are interned symbol ids. */
  char meta_args[64];
  int meta_count = 0;
  {
    enum { PMETA_MAX = 64 };
    const char *pkind[PMETA_MAX]; int pname[PMETA_MAX];
    int pn = proc_params_node(c, create);
    if (pn >= 0) {
      int n = 0; const int *ids;
      /* kinds are stored CANONICALLY (lambda-style): a plain positional is
         "req", a defaulted one "opt". sp_proc_parameters_ids remaps req->opt
         at print time when the wanted mode is proc, so parameters() and
         parameters(lambda:) both read the same array (#2693). */
      const char *pos_kind = "req";
      (void)is_lambda;
      ids = nt_arr(nt, pn, "requireds", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        pkind[meta_count] = pos_kind;
        pname[meta_count++] = comp_sym_intern(c, nt_str(nt, ids[i], "name"));
      }
      ids = nt_arr(nt, pn, "optionals", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        pkind[meta_count] = "opt";
        pname[meta_count++] = comp_sym_intern(c, nt_str(nt, ids[i], "name"));
      }
      int rest = nt_ref(nt, pn, "rest");
      const char *rty = rest >= 0 ? nt_type(nt, rest) : NULL;
      if (rty && sp_streq(rty, "RestParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = nt_str(nt, rest, "name");
        pkind[meta_count] = "rest";
        pname[meta_count++] = comp_sym_intern(c, nm ? nm : "*");
      }
      ids = nt_arr(nt, pn, "posts", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        pkind[meta_count] = pos_kind;
        pname[meta_count++] = comp_sym_intern(c, nt_str(nt, ids[i], "name"));
      }
      ids = nt_arr(nt, pn, "keywords", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        const char *kt = nt_type(nt, ids[i]);
        pkind[meta_count] = (kt && sp_streq(kt, "OptionalKeywordParameterNode")) ? "key" : "keyreq";
        pname[meta_count++] = comp_sym_intern(c, nt_str(nt, ids[i], "name"));
      }
      int kwrest = nt_ref(nt, pn, "keyword_rest");
      const char *kwty = kwrest >= 0 ? nt_type(nt, kwrest) : NULL;
      if (kwty && sp_streq(kwty, "KeywordRestParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = nt_str(nt, kwrest, "name");
        pkind[meta_count] = "keyrest";
        pname[meta_count++] = comp_sym_intern(c, nm ? nm : "**");
      }
      int bpar = nt_ref(nt, pn, "block");
      const char *bty = bpar >= 0 ? nt_type(nt, bpar) : NULL;
      if (bty && sp_streq(bty, "BlockParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = nt_str(nt, bpar, "name");
        pkind[meta_count] = "block";
        pname[meta_count++] = comp_sym_intern(c, nm ? nm : "&");
      }
    }
    else if (nnumbered > 0) {
      for (int i = 0; i < nnumbered && meta_count < PMETA_MAX; i++) {
        char nbuf[4];
        snprintf(nbuf, sizeof nbuf, "_%d", i + 1);
        pkind[meta_count] = "req";  /* canonical, like a named plain positional */
        pname[meta_count++] = comp_sym_intern(c, nbuf);
      }
    }
    if (meta_count > 0) {
      /* the print-time remap needs both ids resolvable regardless of which
         kinds this particular proc uses */
      comp_sym_intern(c, "req");
      comp_sym_intern(c, "opt");
      buf_printf(&g_procs, "static const sp_sym _proc_kinds_%d[] = {", pid);
      for (int k = 0; k < meta_count; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", comp_sym_intern(c, pkind[k]));
      buf_puts(&g_procs, "};\n");
      buf_printf(&g_procs, "static const sp_sym _proc_names_%d[] = {", pid);
      for (int k = 0; k < meta_count; k++) buf_printf(&g_procs, "%s(sp_sym)%d", k ? ", " : "", pname[k]);
      buf_puts(&g_procs, "};\n");
      snprintf(meta_args, sizeof meta_args, "_proc_kinds_%d, _proc_names_%d", pid, pid);
    }
    else snprintf(meta_args, sizeof meta_args, "NULL, NULL");
  }

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
    if (cap_self && self_is_value) buf_printf(&g_procs, " sp_%s __self_val;", self_cls);
    else if (cap_self) buf_puts(&g_procs, " void *__self;");
    if (ret_proc) buf_puts(&g_procs, " mrb_int _home;");  /* home method's proc-return id (sp_proc_home.id) */
    buf_printf(&g_procs, " } _proc_cap_%d;\n", pid);
    buf_printf(&g_procs, "static void _proc_cap_scan_%d(void *p) {\n", pid);
    buf_printf(&g_procs, "  sp_gc_mark(p);\n");
    buf_printf(&g_procs, "  _proc_cap_%d *_c = (_proc_cap_%d *)p;\n", pid, pid);
    for (int i = 0; i < ncap; i++) buf_printf(&g_procs, "  if (_c->%s) sp_gc_mark((void *)_c->%s);\n", caps.v[i], caps.v[i]);
    if (cap_self && self_is_value) {
      if (class_needs_scan(&c->classes[bs->class_id]))
        buf_printf(&g_procs, "  sp_%s_scan(&_c->__self_val);\n", self_cls);
    }
    else if (cap_self) buf_puts(&g_procs, "  if (_c->__self) sp_gc_mark(_c->__self);\n");
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
  int sv_rsd = g_rescue_save_depth;
  g_exc_frame_depth = 0; g_method_pr_exc_depth = 0; g_rescue_save_depth = 0;
  const char *sv_fn_prl = g_fn_pr_label, *sv_fn_prv = g_fn_pr_var; TyKind sv_fn_rt = g_fn_ret_type;
  g_fn_pr_label = NULL; g_fn_pr_var = NULL; g_fn_ret_type = ret;
  char home_acc[48] = "";
  if (ret_proc) { snprintf(home_acc, sizeof home_acc, "((_proc_cap_%d *)_cap)->_home", pid); g_proc_return_home = home_acc; }
  else g_proc_return_home = NULL;
  g_pre = NULL; g_indent = 0; g_nren = 0; g_block_id = -1; g_block_param_name = NULL;
  g_self = "self"; g_result_var = NULL; g_ret_type = ret; g_ensure_depth = 0; g_result_poly = 0;
  /* The proc body reads its captured self by value for a value-type class
     (sp_X self) but by pointer otherwise (sp_X *self); ivar access inside the
     body must match. g_self_deref is global, so override it for the body and
     restore below -- before the capture code reads the enclosing method's
     deref to decide whether to dereference self at the store site. */
  const char *sv_deref = g_self_deref;
  g_self_deref = (cap_self && self_is_value) ? "." : "->";
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

  /* Build the function into a LOCAL buffer and append it to g_procs only when
     complete: a nested proc literal in this body re-enters this emitter, and
     writing both straight into g_procs would splice the inner function into
     the middle of ours (invalid C). Same shape as emit_fiber_new's nested-
     Fiber fix: the inner body appends itself first, we follow -- both at file
     scope, and the prototypes in g_proc_protos keep call order irrelevant. */
  int sv_loopd = g_c_loop_depth, sv_inproc = g_in_proc_body;
  g_c_loop_depth = 0; g_in_proc_body = 1;   /* fresh fn: outer loops don't count */
  Buf proc_body_buf; memset(&proc_body_buf, 0, sizeof proc_body_buf);
  Buf *pb = &proc_body_buf;
  buf_printf(pb, "static mrb_int _proc_%d(void *_cap, mrb_int argc, mrb_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  if (ncap == 0 && !cap_self && !ret_proc) buf_puts(pb, "    (void)_cap;\n");
  buf_puts(pb, "    (void)args;\n");
  buf_puts(pb, "    (void)argc;\n");
  /* Captured instance self, read back from _cap (#1436). (void) guards the
     over-approximating use-of-self detection. */
  if (cap_self && self_is_value) {
    buf_printf(pb, "    sp_%s self = ((_proc_cap_%d *)_cap)->__self_val;\n", self_cls, pid);
    buf_puts(pb, "    (void)self;\n");
  }
  else if (cap_self) {
    buf_printf(pb, "    sp_%s *self = (sp_%s *)((_proc_cap_%d *)_cap)->__self;\n", self_cls, self_cls, pid);
    buf_puts(pb, "    (void)self;\n");
  }
  /* Lambda: strict arity -- requireds + trailing posts mandatory, optionals
     widen the max, a splat rest lifts it entirely. */
  int has_kwrest = 0;
  { int pnk = proc_params_node(c, create);
    if (pnk >= 0 && nt_ref(nt, pnk, "keyword_rest") >= 0) has_kwrest = 1; }
  if (is_lambda) buf_printf(pb, "    sp_proc_lambda_arity_check(argc, %d, %d, %s, %s);\n",
                            arity + nposts + nnumbered, nopts,
                            proc_has_rest(c, create) ? "TRUE" : "FALSE",
                            (nkw > 0 || has_kwrest) ? "TRUE" : "FALSE");
  /* CRuby proc auto-splat: a single Array passed to a non-lambda proc taking
     more than one positional is destructured across the parameters. Rewrite
     the argument view (both the mrb_int[] slots and the boxed side-channel)
     from the array's elements before binding. */
  if (!is_lambda && arity >= 2) {
    g_needs_proc_poly_argslot = 1;
    buf_puts(pb, "    mrb_int _sp_as_buf[16];\n");
    buf_puts(pb, "    if (argc == 1 && _sp_proc_poly_args[0].tag == SP_TAG_OBJ && sp_poly_is_array_kind(_sp_proc_poly_args[0].cls_id)) {\n");
    buf_puts(pb, "      sp_RbVal _sp_as_a = _sp_proc_poly_args[0];\n");
    buf_puts(pb, "      mrb_int _sp_as_n = sp_poly_length(_sp_as_a); if (_sp_as_n > 16) _sp_as_n = 16;\n");
    buf_puts(pb, "      for (mrb_int _i = 0; _i < _sp_as_n; _i++) {\n");
    buf_puts(pb, "        sp_RbVal _e = sp_poly_arr_get(_sp_as_a, _i);\n");
    buf_puts(pb, "        _sp_proc_poly_args[_i] = _e;\n");
    buf_puts(pb, "        _sp_as_buf[_i] = (_e.tag == SP_TAG_OBJ || _e.tag == SP_TAG_STR) ? (mrb_int)(uintptr_t)_e.v.p : _e.v.i;\n");
    buf_puts(pb, "      }\n");
    buf_puts(pb, "      args = _sp_as_buf; argc = _sp_as_n;\n");
    buf_puts(pb, "    }\n");
  }
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
    if (pt == TY_POLY || pt == TY_FLOAT) {
      /* A poly param doesn't fit the mrb_int slot, so it rides the
         _sp_proc_poly_args side-channel the call site published. A float rides
         it too: a raw mrb_float in the slot is value-truncated (0.7 -> 0), so
         read the boxed value back, unboxing a float with sp_poly_to_f. The
         side-channel array holds 16 slots (the proc-call ABI cap). */
      if (k < 16) {
        g_needs_proc_poly_argslot = 1;  /* channel array now lives in sp_runtime.h */
        if (pt == TY_FLOAT)
          buf_printf(pb, "(argc > %d) ? sp_poly_to_f(_sp_proc_poly_args[%d]) : sp_float_nil();\n", k, k);
        else
          buf_printf(pb, "(argc > %d) ? _sp_proc_poly_args[%d] : sp_box_nil();\n", k, k);
      }
      else buf_puts(pb, pt == TY_FLOAT ? "sp_float_nil();\n" : "0;\n");
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
      else if (pt == TY_PROC) {
        /* a Proc block param (e.g. the accumulator of a proc-composing reduce)
           rides the mrb_int slot as a stuffed pointer; cast it back rather than
           assigning mrb_int straight to sp_Proc * (-Wint-conversion). (#2874) */
        buf_printf(pb, "(argc > %d) ? (sp_Proc *)(uintptr_t)args[%d] : NULL;\n", k, k);
      }
      else buf_printf(pb, "args[%d];\n", k);
    }
  }
  /* A celled param: this proc's frame OWNS a variable an inner proc captures,
     so materialize its heap cell here and copy the bound value in -- reads and
     writes below go through the (*_cell_x) forms, and the inner proc's capture
     fill takes the cell pointer (#2648). Mirrors the method-prologue shapes. */
  for (int k = 0; k < arity; k++) {
    const char *p = proc_param_name(c, create, k);
    LocalVar *lv = p ? scope_local(bs, p) : NULL;
    if (!lv || !lv->is_cell) continue;
    if (lv->type == TY_PROC) {
      buf_printf(pb, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = (mrb_int)(uintptr_t)lv_%s;%c", p, p, p, p, 10);
    }
    else if (lv->type == TY_FLOAT) {
      buf_printf(pb, "    mrb_float *_cell_%s = (mrb_float *)sp_gc_alloc(sizeof(mrb_float), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
    else if (lv->type == TY_POLY) {
      buf_printf(pb, "    sp_RbVal *_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
    else if (proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type)) {
      buf_puts(pb, "    ");
      emit_ctype(c, lv->type, pb);
      buf_printf(pb, " *_cell_%s = (", p);
      emit_ctype(c, lv->type, pb);
      buf_printf(pb, " *)sp_gc_alloc(sizeof(void *), NULL, %s);",
                 lv->type == TY_STRING ? "sp_cell_scan_str" : "sp_cell_scan_ptr");
      buf_printf(pb, ""
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, 10);
    }
    else {
      buf_printf(pb, "    mrb_int *_cell_%s = (mrb_int *)sp_gc_alloc(sizeof(mrb_int), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
  }
  /* Splat rest and trailing post params. Both read the boxed side-channel:
     every call path now publishes all args boxed (yield's lean ABI was
     retired for this), so any position is recoverable regardless of the
     callee's static types. CRuby non-lambda distribution: leading requireds
     from the front, posts from the back, the remainder (possibly empty) is
     the rest; missing posts bind nil. */
  if ((restn && restn[0]) || nposts > 0 || nopts > 0 || nnumbered > 0) {
    g_needs_proc_poly_argslot = 1;  /* channel array now lives in sp_runtime.h */
    for (int k = 0; k < nnumbered; k++) {
      buf_printf(pb, "    sp_RbVal lv__%d = (argc > %d) ? _sp_proc_poly_args[%d] : sp_box_nil();\n",
                 k + 1, k, k);
      buf_printf(pb, "    (void)lv__%d;\n", k + 1);
    }
    /* Optionals fill from the front with whatever arguments remain after the
       requireds and the trailing posts; a slot with no argument evaluates its
       default (which may reference earlier params -- they are bound above /
       to the left). Boxed like rest/post: a first-class proc's optionals are
       type-erased at the call site. */
    for (int j = 0; j < nopts; j++) {
      const char *on = proc_opt_name(c, create, j);
      if (!on) continue;
      /* A default that reads another parameter needs that read renamed into
         the per-block namespace, which the parse-time block rename does not
         reach inside default expressions yet -- reject precisely rather than
         emit an unrenamed (undeclared or wrong) variable reference. */
      { int dv0 = proc_opt_value(c, create, j);
        NameSet dused = {0};
        if (dv0 >= 0) proc_collect_used(c, dv0, &dused);
        int refs_param = 0;
        for (int u2 = 0; u2 < dused.n && !refs_param; u2++)
          if (nameset_has(&params, dused.v[u2])) refs_param = 1;
        free(dused.v);
        if (refs_param) {
          unsupported(c, create, "proc optional default referencing another parameter (later slice)");
          return;
        } }
      buf_printf(pb, "    sp_RbVal lv_%s = (%d + %d < argc - %d && %d + %d < 16)\n", on, arity, j, nposts, arity, j);
      buf_printf(pb, "      ? _sp_proc_poly_args[%d + %d] : ", arity, j);
      { int dv = proc_opt_value(c, create, j);
        if (dv >= 0) emit_boxed(c, dv, pb); else buf_puts(pb, "sp_box_nil()"); }
      buf_puts(pb, ";\n");
      buf_printf(pb, "    (void)lv_%s;\n", on);
    }
    if (restn && restn[0]) {
      buf_printf(pb, "    sp_PolyArray *lv_%s = sp_PolyArray_new(); SP_GC_ROOT(lv_%s);\n", restn, restn);
      buf_printf(pb, "    { mrb_int __k = %d + %d, __hi = argc - %d; if (__hi > 16) __hi = 16;\n",
                 arity, nopts, nposts);
      buf_printf(pb, "      for (; __k < __hi; __k++) sp_PolyArray_push(lv_%s, _sp_proc_poly_args[__k]); }\n",
                 restn);
    }
    for (int j = 0; j < nposts; j++) {
      const char *pp = proc_post_name(c, create, j);
      if (!pp) continue;
      buf_printf(pb, "    sp_RbVal lv_%s = ({ mrb_int __i = argc - %d + %d;\n", pp, nposts, j);
      buf_printf(pb, "      (__i >= %d && __i < argc && __i < 16) ? _sp_proc_poly_args[__i] : sp_box_nil(); });\n",
                 arity);
      buf_printf(pb, "    (void)lv_%s;\n", pp);
    }
  }
  /* Keyword params (`proc { |a:, b: 5| }`): the caller's kwargs arrive as a
     boxed hash in the trailing arg slot. Extract each keyword by symbol name;
     an optional keyword absent from the hash falls back to its default. */
  if (nkw > 0) {
    g_needs_proc_poly_argslot = 1;
    int pnk = proc_params_node(c, create);
    int nkw2 = 0;
    const int *kwn = pnk >= 0 ? nt_arr(nt, pnk, "keywords", &nkw2) : NULL;
    for (int j = 0; j < nkw2; j++) {
      const char *kn = nt_str(nt, kwn[j], "name");
      if (!kn) continue;
      const char *kpty = nt_type(nt, kwn[j]);
      int dv = (kpty && sp_streq(kpty, "OptionalKeywordParameterNode"))
                 ? nt_ref(nt, kwn[j], "value") : -1;
      int sym_id = comp_sym_intern(c, kn);
      buf_printf(pb, "    sp_RbVal lv_%s = (argc > 0 && sp_poly_has_key(_sp_proc_poly_args[argc-1], sp_box_sym((sp_sym)%d)))\n", kn, sym_id);
      buf_printf(pb, "      ? sp_poly_index_poly(_sp_proc_poly_args[argc-1], sp_box_sym((sp_sym)%d)) : ", sym_id);
      /* A required keyword absent from the call raises ArgumentError (mirrors the
         method-keyword arm); an optional one falls back to its default. */
      if (dv >= 0) emit_boxed(c, dv, pb);
      else buf_printf(pb, "(sp_raise_cls(\"ArgumentError\", \"missing keyword: :%s\"), sp_box_nil())", kn);
      buf_puts(pb, ";\n");
      buf_printf(pb, "    (void)lv_%s;\n", kn);
    }
  }
  /* `&b`: the block the caller attached to .call, delivered on the
     _sp_proc_blk side-channel; nil (NULL) when none was given (#2648). */
  {
    int pnb = proc_params_node(c, create);
    int bpar = pnb >= 0 ? nt_ref(nt, pnb, "block") : -1;
    const char *bpty = bpar >= 0 ? nt_type(nt, bpar) : NULL;
    const char *bpn = (bpty && sp_streq(bpty, "BlockParameterNode")) ? nt_str(nt, bpar, "name") : NULL;
    if (bpn) {
      g_needs_proc_poly_argslot = 1;
      buf_printf(pb, "    sp_Proc *lv_%s = _sp_proc_blk; _sp_proc_blk = NULL; (void)lv_%s;%c",
                 bpn, bpn, 10);
    }
  }
  /* `**kw` (no named keywords alongside): the whole trailing kwargs hash, or
     an empty hash when the caller passed none (#2648). */
  {
    int pnr = proc_params_node(c, create);
    int kwr = pnr >= 0 ? nt_ref(nt, pnr, "keyword_rest") : -1;
    const char *kwrty = kwr >= 0 ? nt_type(nt, kwr) : NULL;
    if (kwrty && sp_streq(kwrty, "KeywordRestParameterNode") && nkw == 0) {
      const char *krn = nt_str(nt, kwr, "name");
      if (krn) {
        g_needs_proc_poly_argslot = 1;
        buf_printf(pb, "    sp_RbVal lv_%s = (argc > 0 && _sp_proc_poly_args[argc-1].tag == SP_TAG_OBJ"
                       " && sp_poly_is_hash_kind(_sp_proc_poly_args[argc-1].cls_id))"
                       " ? _sp_proc_poly_args[argc-1]"
                       " : sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH);"
                       " (void)lv_%s;%c", krn, krn, 10);
      }
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
       reads it back after sp_proc_call returns. Per-worker (SP_TLS) in the
       threaded build: concurrent Proc#call from several threads would race
       on a shared slot and corrupt each other's return values. The slot is
       safe per-worker because no safepoint poll (the only migration /
       preemption point) lies between the store and the call-site read. */
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
       publish nil to the return slot (so a .call reading the slot sees nil,
       not a stale value), return nil (0) */
    emit_stmts(c, body, pb, 1);
    buf_puts(pb, "  _sp_proc_poly_ret = sp_box_nil();\n  return 0;\n");
  }
  else {
    /* Direct-slot return (mrb_int carrier). A TY_UNKNOWN tail can still emit
       an sp_RbVal-valued expression -- the unresolved-call gate's
       sp_raise_nomethod(...), or a NameError-raising constant read -- which
       must not flow into the mrb_int return raw. Present the carrier type to
       emit_stmts_tail so its existing gate-token coercion fires (that
       coercion deliberately skips when g_ret_type is UNKNOWN). */
    if (ret == TY_UNKNOWN) g_ret_type = TY_INT;
    emit_stmts_tail(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  buf_puts(pb, "}\n");
  buf_puts(&g_procs, proc_body_buf.p ? proc_body_buf.p : "");
  free(proc_body_buf.p);
  g_c_loop_depth = sv_loopd; g_in_proc_body = sv_inproc;

  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block;
  g_block_param_name = sv_bpn; g_self = sv_self; g_result_var = sv_rv; g_ret_type = sv_rt;
  g_self_deref = sv_deref;
  g_cap_struct = sv_cap_struct; g_cap_names = sv_cap_names; g_ensure_depth = sv_ensure_depth;
  g_brk_ser_var = sv_bser; g_brk_skip_id = sv_bskip;
  g_proc_body_kind = sv_pbk; g_proc_brk_home = sv_pbh;
  g_result_poly = sv_rp;
  g_method_pr_label = sv_pr_label; g_method_pr_var = sv_pr_var; g_proc_return_home = sv_prh;
  g_exc_frame_depth = sv_excd; g_method_pr_exc_depth = sv_prexcd;
  g_rescue_save_depth = sv_rsd;
  g_fn_pr_label = sv_fn_prl; g_fn_pr_var = sv_fn_prv; g_fn_ret_type = sv_fn_rt;

  if (ncap == 0 && !cap_self && !ret_proc) {
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, NULL, NULL, %d, %s, %d, %s)",
               pid, meta_arity, is_lambda ? "TRUE" : "FALSE", meta_count, meta_args);
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
      /* Capture the enclosing instance self: by value for a value-type class
         (deref if the enclosing method holds self as a pointer, e.g. an
         initialize body), else by pointer (#1436). */
      if (cap_self && self_is_value) {
        int self_ptr = g_self_deref && sp_streq(g_self_deref, "->");
        emit_indent(g_pre, g_indent);
        buf_printf(g_pre, "_capv_%d->__self_val = %s%s;\n", pid, self_ptr ? "*" : "", sv_self ? sv_self : "self");
      }
      else if (cap_self) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->__self = (void *)%s;\n", pid, sv_self ? sv_self : "self"); }
      /* Capture the home method's proc-return frame so the proc's `return`
         longjmps to it (the creating method declared `_pr`). */
      if (ret_proc) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->_home = _h.id;\n", pid); }
    }
    buf_printf(b, "sp_proc_new_meta((void *)_proc_%d, _capv_%d, _proc_cap_scan_%d, %d, %s, %d, %s)",
               pid, pid, pid, meta_arity, is_lambda ? "TRUE" : "FALSE", meta_count, meta_args);
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
    "FloatDomainError", "Math::DomainError", "FrozenError", "EncodingError",
    "LoadError", "RegexpError", "StringScanner_Error", "FiberError",
    "UncaughtThrowError", "SyntaxError", "SecurityError", "Interrupt",
    "SignalException", "ThreadError", "ClosedQueueError",
    "NoMatchingPatternError", "NoMatchingPatternKeyError", "SystemExit", NULL
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
  /* a synthesized singleton subclass answers with its parent's name (CRuby's
     singleton class is invisible to #class / inspect / #name). */
  ci = singleton_visible_ci(c, ci);
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
    return c->classes[defcls].c_name;
  if (c->classes[cid].is_struct) return c->classes[cid].c_name;  /* generated #inspect/#to_s */
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
  /* Native (C-backed) class: the package owns the struct; the generated TU has
     only its forward-decl (`typedef struct sp_X_s sp_X;`) and holds pointers. */
  if (ci->is_native_class) return;
  /* Exception subclasses share sp_Exception as their underlying type. */
  int cid = comp_class_index(c, ci->name);
  if (cid >= 0 && class_is_exc_subclass(c, cid)) {
    /* An ivar-less exception subclass is forward-declared as
       `typedef sp_Exception` and needs no struct of its own. One with
       ivars gets a dedicated struct whose leading members mirror
       sp_Exception (cls_name/parent_cls_name/msg/cause/result/xname/xkey/
       xrecv) -- a common initial sequence -- so every `(sp_Exception *)` cast
       in the raise/rescue and message machinery stays valid, with the ivar
       fields after (#1415). Every base member must be mirrored: the rescue
       machinery and the GC scan / constructor write them through the base
       cast, so omitting one would alias (and overrun into) the first ivar. */
    if (ci->nivars == 0) return;
    buf_printf(b, "struct sp_%s_s {\n", ci->c_name);
    buf_puts(b, "  const char *cls_name;\n");
    buf_puts(b, "  const char *parent_cls_name;\n");
    buf_puts(b, "  const char *msg;\n");
    buf_puts(b, "  struct sp_Exception_s *cause;\n");
    buf_puts(b, "  sp_RbVal result;\n");
    buf_puts(b, "  sp_RbVal xname;\n");
    buf_puts(b, "  sp_RbVal xkey;\n");
    buf_puts(b, "  sp_RbVal xrecv;\n");
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
  buf_printf(b, "struct sp_%s_s {\n", ci->c_name);
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
  if (ci->is_native_class) return;  /* the package owns the struct + its GC scan */
  int cid = comp_class_index(c, ci->name);
  int is_exc_iv = cid >= 0 && ci->nivars > 0 && class_is_exc_subclass(c, cid);
  /* An ivar-bearing exception subclass always needs a scan: even with no
     heap ivar, its `msg` (a managed string in the dedicated struct) must
     be marked or it is swept while the exception is in flight. */
  if (!class_needs_scan(ci) && !is_exc_iv) return;
  buf_printf(b, "static void sp_%s_scan(void *p) {\n", ci->c_name);
  buf_printf(b, "  sp_%s *o = (sp_%s *)p;\n", ci->c_name, ci->c_name);
  if (is_exc_iv) {
    buf_puts(b, "  sp_mark_string(o->msg);\n");
    buf_puts(b, "  if (o->cause) sp_gc_mark(o->cause);\n");
    buf_puts(b, "  sp_mark_rbval(o->result);\n");
    buf_puts(b, "  sp_mark_rbval(o->xname);\n");
    buf_puts(b, "  sp_mark_rbval(o->xkey);\n");
    buf_puts(b, "  sp_mark_rbval(o->xrecv);\n");
  }
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
  /* Native (C-backed) class: constructor + methods live in the package; nothing
     is generated here (see the native_method externs + .new emission). */
  if (ci->is_native_class) return;
  int cid = comp_class_index(c, ci->name);
  if (ci->is_struct) {
    int sinit_def = cid;
    int sinit = comp_method_in_chain(c, cid, "initialize", &sinit_def);
    if (sinit >= 0 && c->scopes[sinit].reachable) {
      /* Custom `initialize` override (a `Struct.new(...) do def initialize ...
         super(...) end end` block, e.g. doom's Visplane): the constructor
         allocates a blank instance and delegates to the user initialize -- the
         member ivars are set by its own super(...) call (see emit_super's
         is_struct branch), not positionally here, since the args at the .new
         site are the custom initialize's own params, not one-per-member.
         A yielding initialize has no standalone C symbol (its body runs inlined
         at the .new site); the constructor just allocates blank and the inliner
         runs the body -- so skip the sp_X_initialize call for that case. */
      Scope *si = &c->scopes[sinit];
      buf_printf(b, "SP_POOL_DEFINE(%s)\n", ci->c_name);
      buf_printf(b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
      if (si->nparams > 0) {
        for (int i = 0; i < si->nparams; i++) {
          if (i) buf_puts(b, ", ");
          LocalVar *p = scope_local(si, si->pnames[i]);
          TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
          emit_ctype(c, pt, b);
          buf_printf(b, " lv_%s", si->pnames[i]);
        }
      }
      else buf_puts(b, "void");
      buf_printf(b, ") {\n  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
                ci->c_name, ci->c_name,
                class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->c_name : "NULL",
                class_needs_scan(ci) ? "_scan" : "");
      buf_puts(b, "  memset(self, 0, sizeof(*self));\n");
      buf_puts(b, "  SP_GC_ROOT(self);\n");
      buf_printf(b, "  self->cls_id = %d;\n", cid);
      emit_ivar_nil_inits(b, ci, "self->", "  ", ";\n");
      /* Call the initialize under the name of the class that actually defines
         it: when it is inherited from an ancestor the C symbol is
         sp_<ancestor>_initialize (not sp_<this>_initialize), and self must be
         cast to the ancestor's struct type. (For a direct definition
         sinit_def == cid and this is unchanged.) */
      if (!si->yields) {
        buf_printf(b, "  sp_%s_initialize(", c->classes[sinit_def].c_name);
        if (sinit_def != cid) buf_printf(b, "(sp_%s *)", c->classes[sinit_def].c_name);
        buf_puts(b, "self");
        for (int i = 0; i < si->nparams; i++) buf_printf(b, ", lv_%s", si->pnames[i]);
        buf_puts(b, ");\n");
      } else {
        /* The body runs inlined at the .new site, so these params are unused
           here; the signature exists only to match the inliner's call. */
        for (int i = 0; i < si->nparams; i++) buf_printf(b, "  (void)lv_%s;\n", si->pnames[i]);
      }
      if (ci->is_data) buf_puts(b, "  sp_gc_freeze(self);\n");
      buf_puts(b, "  return self;\n}\n");
      goto struct_meta;
    }
    /* Struct constructor: one parameter per member, set the backing ivars. */
    buf_printf(b, "SP_POOL_DEFINE(%s)\n", ci->c_name);
    buf_printf(b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
    for (int i = 0; i < ci->nivars; i++) {
      if (i) buf_puts(b, ", ");
      emit_ctype(c, ci->ivar_types[i], b);
      buf_printf(b, " a%d", i);
    }
    if (ci->nivars == 0) buf_puts(b, "void");
    buf_puts(b, ") {\n");
    /* Root every heap-backed member argument before allocating the struct:
       SP_POOL_NEW can trigger a GC, and a member value that is a fresh,
       otherwise-unrooted temporary (e.g. a `data[8, 8].delete("\0").upcase`
       WAD lump name) would be swept before it is stored into the ivar,
       leaving a dangling pointer the collector later reads (use-after-free). */
    for (int i = 0; i < ci->nivars; i++) {
      TyKind mt = ci->ivar_types[i];
      /* Gate on needs_root() -- the codebase's authoritative "this type is a
         heap pointer the GC scans" predicate -- so EVERY heap-backed member
         (bigint, obj_array, proc/method/exception, io/fiber/etc.) is rooted,
         not just strings/poly/object/array/hash. A fresh bigint or obj_array
         temporary is just as sweepable across SP_POOL_NEW's GC as a string. */
      if (mt == TY_STRING)      buf_printf(b, "  SP_GC_ROOT_STR(a%d);\n", i);
      else if (mt == TY_POLY)   buf_printf(b, "  SP_GC_ROOT_RBVAL(a%d);\n", i);
      else if (needs_root(mt))  buf_printf(b, "  SP_GC_ROOT(a%d);\n", i);
    }
    buf_printf(b, "  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
              ci->c_name, ci->c_name,
              class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->c_name : "NULL",
              class_needs_scan(ci) ? "_scan" : "");
    buf_puts(b, "  memset(self, 0, sizeof(*self));\n");  /* recycled slots are not zeroed */
    buf_puts(b, "  SP_GC_ROOT(self);\n");
    buf_printf(b, "  self->cls_id = %d;\n", cid);
    for (int i = 0; i < ci->nivars; i++)
      buf_printf(b, "  self->iv_%s = a%d;\n", ci->ivars[i] + 1, i);  /* skip leading '@' */
    /* Data instances are frozen from construction (CRuby); Struct is mutable. */
    if (ci->is_data) buf_puts(b, "  sp_gc_freeze(self);\n");
    buf_puts(b, "  return self;\n}\n");
    struct_meta:;
    /* Struct/Data #inspect (== #to_s): `#<struct Name m=v, ...>` (Data uses
       `data`). Generated unless the user redefined the method, so a user
       override wins. to_s defers to inspect, which always exists here. */
    if (comp_method_in_chain(c, cid, "inspect", NULL) < 0) {
      const char *rn = class_ruby_name(c, cid); if (!rn) rn = ci->name;
      buf_printf(b, "static const char *sp_%s_inspect(sp_%s *self) {\n", ci->c_name, ci->c_name);
      buf_puts(b, "  if (!self) return \"nil\";\n");
      /* an anonymous struct class has no name to show: #<struct a=1, b=2> */
      if (ci->is_anon_struct)
        buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s\"); SP_GC_ROOT(s);\n",
                   ci->is_data ? "data" : "struct");
      else
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
  /* An explicit `&blk` block parameter on a non-yielding initialize is a real
     sp_Proc* the constructor must accept and forward (a yielding initialize is
     inlined at the call site instead, see emit_ctor_yield_inline). */
  int init_has_blk = init >= 0 && c->scopes[init].blk_param &&
                     c->scopes[init].blk_param[0] && !c->scopes[init].yields;
  if (ci->is_value_type) {
    /* value-type: build on the stack and return by value (no heap / GC) */
    buf_printf(b, "static sp_%s sp_%s_new(", ci->c_name, ci->c_name);
    if (init >= 0 && (c->scopes[init].nparams > 0 || init_has_blk)) {
      Scope *s = &c->scopes[init];
      for (int i = 0; i < s->nparams; i++) {
        if (i) buf_puts(b, ", ");
        LocalVar *p = scope_local(s, s->pnames[i]);
        TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
        emit_ctype(c, pt, b);
        buf_printf(b, " lv_%s", s->pnames[i]);
      }
      if (init_has_blk) {
        if (s->nparams > 0) buf_puts(b, ", ");
        buf_printf(b, "sp_Proc *lv_%s", s->blk_param);
      }
    }
    else buf_puts(b, "void");
    buf_printf(b, ") {\n  sp_%s self = {0};\n  self.cls_id = %d;\n", ci->c_name, cid);
    emit_ivar_nil_inits(b, ci, "self.", "  ", ";\n");
    if (init >= 0 && c->scopes[init].reachable && !c->scopes[init].yields) {
      buf_printf(b, "  sp_%s_initialize(&self", c->classes[initcls].c_name);
      Scope *s = &c->scopes[init];
      for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
      if (init_has_blk) buf_printf(b, ", lv_%s", s->blk_param);
      buf_puts(b, ");\n");
    }
    buf_puts(b, "  return self;\n}\n");
    /* Boxing a by-value instance into a poly slot heap-copies it, like
       sp_box_range (the poly dispatch unboxes with *(sp_X *)v.p, so v.p must
       be a heap pointer). The value is fully evaluated at the call boundary;
       its heap ivars are rooted across the allocation. */
    buf_printf(b, "__attribute__((unused)) static sp_RbVal sp_box_vobj_%s(sp_%s v) {\n",
               ci->c_name, ci->c_name);
    for (int i = 0; i < ci->nivars; i++) {
      TyKind it = ci->ivar_types[i];
      const char *iv = ci->ivars[i] + 1;
      if (it == TY_STRING) buf_printf(b, "  SP_GC_ROOT(v.iv_%s);\n", iv);
      else if (it == TY_POLY) buf_printf(b, "  SP_GC_ROOT_RBVAL(v.iv_%s);\n", iv);
      else if (needs_root(it)) buf_printf(b, "  SP_GC_ROOT(v.iv_%s);\n", iv);
    }
    if (class_needs_scan(ci))
      buf_printf(b, "  sp_%s *p = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s_scan);\n",
                 ci->c_name, ci->c_name, ci->c_name, ci->c_name);
    else
      buf_printf(b, "  sp_%s *p = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, NULL);\n",
                 ci->c_name, ci->c_name, ci->c_name);
    buf_printf(b, "  *p = v;\n  return sp_box_obj(p, %d);\n}\n", cid);
    return;
  }
  /* per-class free-list pool: sp_gc_collect recycles unmarked instances onto
     the pool instead of free()ing them, and sp_X_new reuses them -- this
     removes the malloc/free churn of allocation-heavy workloads. Exception
     subclasses use sp_exc_new_sub storage, so they are not pooled. */
  if (!class_is_exc_subclass(c, cid)) buf_printf(b, "SP_POOL_DEFINE(%s)\n", ci->c_name);
  buf_printf(b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
  if (init >= 0 && (c->scopes[init].nparams > 0 || init_has_blk)) {
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) {
      if (i) buf_puts(b, ", ");
      LocalVar *p = scope_local(s, s->pnames[i]);
      TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
      emit_ctype(c, pt, b);
      buf_printf(b, " lv_%s", s->pnames[i]);
    }
    if (init_has_blk) {
      if (s->nparams > 0) buf_puts(b, ", ");
      buf_printf(b, "sp_Proc *lv_%s", s->blk_param);
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
                 ci->c_name, cn2, par);
      buf_printf(b, "  SP_GC_ROOT(self);\n");
    }
    else {
      /* ivar-bearing exception subclass: allocate the dedicated struct
         (sp_exc_new_sub would only size the 3-field base). The leading
         members mirror sp_Exception so the raise/message machinery's casts
         work; the ivars live after and are set by initialize. */
      buf_printf(b, ") {\n  sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s_scan);\n",
                 ci->c_name, ci->c_name, ci->c_name, ci->c_name);
      buf_puts(b, "  memset(self, 0, sizeof(*self));\n");
      buf_printf(b, "  self->cls_name = \"%s\";\n", cn2);
      buf_printf(b, "  self->parent_cls_name = \"%s\";\n", par);
      buf_puts(b, "  self->msg = (&(\"\\xff\")[1]);\n");
      buf_puts(b, "  self->result = sp_box_nil();\n");  /* memset left tag 0 (int 0); #result wants nil */
      buf_puts(b, "  self->xname = sp_box_nil();\n");
      buf_puts(b, "  self->xkey = sp_box_nil();\n");
      buf_puts(b, "  self->xrecv = sp_box_nil();\n");
      buf_printf(b, "  SP_GC_ROOT(self);\n");
      emit_ivar_nil_inits(b, ci, "self->", "  ", ";\n");
    }
  }
  else {
  buf_printf(b, ") {\n  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
            ci->c_name, ci->c_name,
            class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->c_name : "NULL",
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
    buf_printf(b, "  sp_%s_initialize(", c->classes[initcls].c_name);
    if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].c_name);
    buf_puts(b, "self");
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    if (init_has_blk) buf_printf(b, ", lv_%s", s->blk_param);
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
    buf_printf(b, "({ sp_%s _t%d = {0}; _t%d.cls_id = %d;", ci->c_name, t, t, cid);
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
               ci->c_name, t, ci->c_name,
               class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->c_name : "NULL",
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
  if (ci->is_native_class) return 0;  /* the package owns the struct; not generically marshalable */
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
    buf_printf(b, "(val.tag == SP_TAG_OBJ ? (sp_%s *)val.v.p : NULL)", c->classes[ty_object_class(t)].c_name);
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
/* Generic object->hash reflection, installed as sp_obj_to_hash_fn. Given a
   boxed Struct, build a StrPoly hash of its members {name -> boxed value} and
   return it boxed. No output-format knowledge -- a consumer (e.g. the json
   package) serializes the resulting hash. This is the compiler's whole role in
   serializing a user object: expose its fields; the format lives in the
   package. Only emitted when g_gen_obj_hash (a package declared it wants this
   and a Struct exists). */
static void emit_obj_to_hash_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_obj_hash) return;
  buf_puts(b, "static sp_RbVal sp_obj_to_hash(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_struct) continue;
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)v.v.p; (void)o;\n", ci->c_name, ci->c_name);
    buf_puts(b, "      sp_StrPolyHash *h = sp_StrPolyHash_new(); SP_GC_ROOT(h);\n");
    for (int j = 0; j < ci->nivars; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ */
      buf_printf(b, "      sp_StrPolyHash_set(h, SPL(\"%s\"), ", iv);
      if (mt == TY_INT) buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", iv, iv);
      else if (mt == TY_STRING) buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", iv, iv);
      else if (mt == TY_FLOAT) buf_printf(b, "sp_box_float(o->iv_%s)", iv);
      else if (mt == TY_BOOL) buf_printf(b, "sp_box_bool(o->iv_%s)", iv);
      else if (mt == TY_SYMBOL) buf_printf(b, "sp_box_sym(o->iv_%s)", iv);
      else if (mt == TY_POLY) buf_printf(b, "o->iv_%s", iv);
      else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_obj(h, SP_BUILTIN_STR_POLY_HASH);\n    }\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* Symbol-keyed Struct/Data #to_h, installed as sp_obj_to_h_fn. Mirrors the
   per-struct inline to_h emitter, but keyed by cls_id so a Struct/Data read out
   of a poly container can answer #to_h at run time (#2906). Data members are
   ivars like a Struct's, so both are covered. */
static void emit_obj_to_h_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_obj_to_h) return;
  buf_puts(b, "static sp_RbVal sp_obj_to_h(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!(ci->is_struct || ci->is_data) || !ci->instantiated) continue;
    buf_printf(b, "    case %d: {\n", comp_class_index(c, ci->name));
    buf_printf(b, "      sp_%s *o = (sp_%s *)v.v.p; (void)o;\n", ci->c_name, ci->c_name);
    buf_puts(b, "      sp_SymPolyHash *h = sp_SymPolyHash_new(); SP_GC_ROOT(h);\n");
    for (int j = 0; j < ci->nivars; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ */
      buf_printf(b, "      sp_SymPolyHash_set(h, sp_sym_intern(\"%s\"), ", iv);
      if (mt == TY_INT) buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", iv, iv);
      else if (mt == TY_STRING) buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", iv, iv);
      else if (mt == TY_FLOAT) buf_printf(b, "sp_box_float(o->iv_%s)", iv);
      else if (mt == TY_BOOL) buf_printf(b, "sp_box_bool(o->iv_%s)", iv);
      else if (mt == TY_SYMBOL) buf_printf(b, "sp_box_sym(o->iv_%s)", iv);
      else if (mt == TY_POLY) buf_printf(b, "o->iv_%s", iv);
      else {
        char fb[128]; snprintf(fb, sizeof fb, "o->iv_%s", iv);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mt, fb, &bx);
        buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
      }
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_obj(h, SP_BUILTIN_SYM_POLY_HASH);\n    }\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* Data#with copy-update, installed as sp_obj_with_fn. cls_id switch over every
   instantiated Data: construct a fresh instance whose members come from the
   symbol-keyed override hash `ov` where present, else copied from the receiver.
   Mirrors the typed Data#with emitter for a poly receiver (#2890). */
static void emit_obj_with_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_obj_with) return;
  buf_puts(b, "static sp_RbVal sp_obj_with(sp_RbVal v, sp_RbVal ov) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_data || !ci->instantiated) continue;
    /* A custom `initialize` gives sp_X_new the init's own param signature (not
       one-per-member), and #with must not re-run it -- skip; poly #with on such
       a Data falls through to NoMethodError (rare). */
    int scust = comp_method_in_chain(c, i, "initialize", NULL);
    if (scust >= 0 && c->scopes[scust].reachable) continue;
    int idx = comp_class_index(c, ci->name);
    buf_printf(b, "    case %d: {\n", idx);
    buf_printf(b, "      sp_%s *o = (sp_%s *)v.v.p; (void)o;\n", ci->c_name, ci->c_name);
    buf_printf(b, "      return sp_box_obj(sp_%s_new(", ci->c_name);
    for (int j = 0; j < ci->nivars; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *iv = ci->ivars[j] + 1;
      if (j) buf_puts(b, ", ");
      /* a per-member statement-expr with its OWN found flag: sp_X_new's args
         evaluate in unspecified order, so a shared flag would race. */
      buf_printf(b, "({ mrb_bool _f; sp_RbVal _pv = sp_poly_hash_probe(ov, sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? ", iv);
      Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, mt, "_pv", &ub);
      buf_puts(b, ub.p ? ub.p : "_pv"); free(ub.p);
      buf_printf(b, " : o->iv_%s; })", iv);
    }
    buf_printf(b, "), %d);\n    }\n", idx);
  }
  buf_puts(b, "    default: sp_raise_cls(\"NoMethodError\", sp_sprintf(\"undefined method 'with' for %s\", sp_poly_class_name(v))); return sp_box_nil();\n  }\n}\n");
}

/* Default Object#inspect: one switch over every user class, walking its
   typed ivars boxed through the marshal box helper into sp_poly_inspect --
   so nested containers, strings (quoted), nil and nested objects (via the
   sp_obj_inspect_fn hook recursion) all render like CRuby's
   #<Name:0xADDR @a=1, @b="x">. Mirrors sp_marshal_obj_dump's shape. */
static int class_inspectable(Compiler *c, int i) {
  ClassInfo *ci = &c->classes[i];
  if (is_builtin_reopen(ci->name)) return 0;
  if (ci->is_native_class) return 0;   /* the package owns the struct */
  if (class_is_exc_subclass(c, i)) return 0;  /* #<Cls: msg> path exists */
  if (comp_ty_value_obj(c, ty_object(i))) return 0;  /* no stable address */
  return 1;
}
static void emit_obj_inspect_dispatch(Compiler *c, Buf *b) {
  /* Forward-declare the user to_s/inspect methods the switches dispatch to --
     the switch bodies are emitted ahead of the method definitions. */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *fci = &c->classes[i];
    if (is_builtin_reopen(fci->name) || fci->is_native_class) continue;
    if (comp_ty_value_obj(c, ty_object(i))) continue;
    const char *mnames[2] = { "to_s", "inspect" };
    for (int m = 0; m < 2; m++) {
      int fdef = -1;
      int fmi = comp_method_in_chain(c, i, mnames[m], &fdef);
      if (fmi < 0 || !c->scopes[fmi].reachable || c->scopes[fmi].ret != TY_STRING ||
          c->scopes[fmi].nparams != 0 || fdef != i) continue;
      /* a debug (-g) build gives user methods external linkage (see
         emit_method_signature); this forward decl must match it */
      buf_printf(b, "%sconst char *sp_%s_%s(sp_%s *self);\n",
                 g_debug ? "" : "static ",
                 c->classes[fdef].c_name, mc(c->scopes[fmi].name), c->classes[fdef].c_name);
    }
  }
  /* user #to_s dispatcher: only classes defining one get an arm; NULL means
     "no user to_s" and the caller renders the #<Name:0xADDR> default. */
  buf_puts(b, "static const char *sp_obj_to_s_sw(int cls_id, void *p) {\n  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *tci = &c->classes[i];
    if (is_builtin_reopen(tci->name) || tci->is_native_class) continue;
    if (comp_ty_value_obj(c, ty_object(i))) continue;
    int tdef = -1;
    int tmi = comp_method_in_chain(c, i, "to_s", &tdef);
    if (tmi < 0 || !c->scopes[tmi].reachable || c->scopes[tmi].ret != TY_STRING ||
        c->scopes[tmi].nparams != 0) continue;
    buf_printf(b, "    case %d: return sp_%s_%s((sp_%s *)p);\n",
               i, c->classes[tdef].c_name, mc(c->scopes[tmi].name), c->classes[tdef].c_name);
  }
  buf_puts(b, "    default: return NULL;\n  }\n}\n");
  buf_puts(b, "static const char *sp_obj_inspect_sw(int cls_id, void *p) {\n");
  buf_puts(b, "  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_inspectable(c, i)) continue;
    ClassInfo *ci = &c->classes[i];
    /* a user #inspect wins over the default ivar walk, so a contained
       element renders the same as a directly-inspected one */
    int uidef = -1;
    int uimi = comp_method_in_chain(c, i, "inspect", &uidef);
    if (uimi >= 0 && c->scopes[uimi].reachable && c->scopes[uimi].ret == TY_STRING &&
        c->scopes[uimi].nparams == 0) {
      buf_printf(b, "    case %d: return sp_%s_%s((sp_%s *)p);\n",
                 i, c->classes[uidef].c_name, mc(c->scopes[uimi].name), c->classes[uidef].c_name);
      continue;
    }
    /* Struct/Data have a generated #inspect (#<struct Name a=1> / #<data ...>);
       route the contained-element render to it instead of the object default
       so [d] and {k: d} print like a directly-inspected d (#2698). */
    if (ci->is_struct || ci->is_data) {
      buf_printf(b, "    case %d: return sp_%s_inspect((sp_%s *)p);\n", i, ci->c_name, ci->c_name);
      continue;
    }
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)p; (void)o;\n", ci->c_name, ci->c_name);
    buf_printf(b, "      sp_String *_s = sp_String_new(sp_sprintf(\"#<%s:0x%%016llx\", (unsigned long long)(uintptr_t)p));\n",
               class_ruby_name(c, i) ? class_ruby_name(c, i) : ci->name);
    for (int j = 0; j < ci->nivars; j++) {
      char expr[160]; snprintf(expr, sizeof expr, "o->iv_%s", ci->ivars[j] + 1);
      buf_printf(b, "      sp_String_append(_s, \"%s%s=\"); sp_String_append(_s, ",
                 j ? ", " : " ", ci->ivars[j]);
      TyKind ivt = ci->ivar_types[j];
      /* containers have their own typed inspect; scalars box through the
         marshal helper into sp_poly_inspect; an UNKNOWN (never usefully
         typed) ivar occupies an int slot in the layout and renders as its
         nil default. */
      if (ty_is_array(ivt) && ivt != TY_POLY_ARRAY && array_kind(ivt))
        buf_printf(b, "(%s ? sp_%sArray_inspect(%s) : \"nil\")", expr, array_kind(ivt), expr);
      else if (ivt == TY_POLY_ARRAY)
        buf_printf(b, "(%s ? sp_PolyArray_inspect(%s) : \"nil\")", expr, expr);
      else if (ty_is_hash(ivt) && ty_hash_cname(ivt))
        buf_printf(b, "(%s ? sp_%sHash_inspect(%s) : \"nil\")", expr, ty_hash_cname(ivt), expr);
      else if (marshal_ivar_type_ok(ivt) && ivt != TY_UNKNOWN) {
        buf_puts(b, "sp_poly_inspect(");
        emit_marshal_box_ivar(ivt, expr, b);
        buf_puts(b, ")");
      }
      else if (ivt == TY_UNKNOWN) {
        buf_puts(b, "sp_poly_inspect(");
        emit_marshal_box_ivar(TY_INT, expr, b);
        buf_puts(b, ")");
      }
      else
        buf_puts(b, "\"#<?>\"");
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      sp_String_append(_s, \">\");\n");
    buf_puts(b, "      return _s->data;\n    }\n");
  }
  buf_puts(b, "    default: return \"#<Object>\";\n  }\n}\n");
}

static void emit_marshal_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static int sp_marshal_obj_dump(sp_mar_buf *b, int cls_id, void *p) {\n");
  buf_puts(b, "  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_marshalable(c, i)) continue;
    ClassInfo *ci = &c->classes[i];
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)p; (void)o;\n", ci->c_name, ci->c_name);
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
    buf_printf(b, "    sp_%s *o = ", ci->c_name);
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
               c->classes[s->class_id].c_name, mc(shadow),
               c->classes[s->class_id].c_name, g_self);
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
  /* super inside a class method: resolve through the parent's CLASS-method
     chain and call the sp_<Cls>_s_ form (class methods take no instance
     self). The instance path below would miss `def self.x` entirely. */
  if (s->is_cmethod) {
    int cdef = -1;
    int cmi = p >= 0 ? comp_cmethod_in_chain(c, p, uname, &cdef) : -1;
    if (cmi < 0) {
      const char *scn2 = class_ruby_name(c, s->class_id);
      if (!scn2) scn2 = c->classes[s->class_id].name;
      buf_printf(b, "(sp_raise_cls(\"NoMethodError\", \"super: no superclass method '%s' for %s\"), %s)",
                 uname, scn2, default_value(comp_ntype(c, id)));
      return;
    }
    buf_printf(b, "sp_%s_s_%s(", c->classes[cdef].c_name, mc(uname));
    if (ty && sp_streq(ty, "ForwardingSuperNode")) {
      Scope *pm = &c->scopes[cmi];
      int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
      for (int i = 0; i < n; i++) {
        LocalVar *src = scope_local(s, s->pnames[i]);
        LocalVar *dst = scope_local(pm, pm->pnames[i]);
        TyKind st = src ? src->type : TY_UNKNOWN;
        TyKind dt = dst ? dst->type : TY_UNKNOWN;
        buf_puts(b, i == 0 ? "" : ", ");
        if (dt == TY_POLY && st != TY_POLY && st != TY_UNKNOWN) {
          Buf _bx; memset(&_bx, 0, sizeof _bx);
          buf_printf(&_bx, "lv_%s", rename_local(s->pnames[i]));
          emit_boxed_text(c, st, _bx.p, b);
          free(_bx.p);
        }
        else buf_printf(b, "lv_%s", rename_local(s->pnames[i]));
      }
    }
    else emit_args_filled(c, cmi, nt_ref(c->nt, id, "arguments"), "", b);
    buf_puts(b, ")");
    return;
  }
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
        /* msg is a const char*; a poly message (e.g. an un-instantiated
           subclass whose `message` param never got constrained to a String)
           must be coerced, not assigned raw. emit_str_expr also coerces the
           unresolved-call gate (TY_UNKNOWN sp_raise_nomethod) that comp_ntype
           can't see. */
        buf_printf(b, "(%s->msg = ", g_self);
        emit_str_expr(c, argv2[0], b);
        buf_puts(b, ")");
      }
      else if (ty && sp_streq(ty, "ForwardingSuperNode") && s->nparams > 0) {
        LocalVar *p0 = scope_local(s, s->pnames[0]);
        const char *rn = rename_local(s->pnames[0]);
        /* Effective type mirrors emit_method_signature: a NULL/TY_UNKNOWN
           param is declared TY_POLY (sp_RbVal), so it too must be coerced. */
        TyKind pt = (p0 && p0->type != TY_UNKNOWN) ? p0->type : TY_POLY;
        if (pt == TY_POLY)
          buf_printf(b, "(%s->msg = sp_poly_to_s(lv_%s))", g_self, rn);
        else
          buf_printf(b, "(%s->msg = lv_%s)", g_self, rn);
      }
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
    /* `super(...)` inside a Struct's custom `initialize`: Struct's own
       initialize positionally assigns each member from the args. There is no
       C `initialize` symbol for it (members are set inline at the .new site),
       so route super to the same per-member ivar assignment. Without this the
       members set only via super (e.g. doom's Visplane `super(..., Array.new,
       Array.new, ...)`) stayed nil and later comparisons hit NilClass. */
    if (c->classes[s->class_id].is_struct && uname && sp_streq(uname, "initialize")) {
      ClassInfo *cls = &c->classes[s->class_id];
      /* A bare `super` (ForwardingSuperNode) forwards the current initialize's
         own params positionally into the members -- exactly what Struct's
         initialize does with them. An explicit `super(...)` (SuperNode) uses its
         argument list instead. Without the forwarding arm a bare super assigned
         nothing and every member stayed nil. */
      int is_fwd = ty && sp_streq(ty, "ForwardingSuperNode");
      int args_id = ty && sp_streq(ty, "SuperNode") ? nt_ref(c->nt, id, "arguments") : -1;
      int an = 0;
      const int *sargv = args_id >= 0 ? nt_arr(c->nt, args_id, "arguments", &an) : NULL;
      /* Data's `super(x: e, y: e2)` passes a single KeywordHashNode: map each
         member to the like-named keyword's value. Assigning positionally would
         drop the whole hash into the first (scalar) member slot. */
      int kwh = (!is_fwd && an == 1 && sargv && nt_type(c->nt, sargv[0]) &&
                 sp_streq(nt_type(c->nt, sargv[0]), "KeywordHashNode")) ? sargv[0] : -1;
      int cnt = kwh >= 0 ? cls->nivars : (is_fwd ? s->nparams : an);
      buf_puts(b, "(");
      for (int a = 0; a < cls->nivars && a < cnt; a++) {
        TyKind ivt = cls->ivar_types[a];
        buf_printf(b, "%s->iv_%s = ", g_self, cls->ivars[a] + 1);
        if (is_fwd) {
          LocalVar *pv = scope_local(s, s->pnames[a]);
          TyKind at = pv && pv->type != TY_UNKNOWN ? pv->type : TY_POLY;
          char src[64]; snprintf(src, sizeof src, "lv_%s", rename_local(s->pnames[a]));
          if (ivt == TY_POLY && at != TY_POLY) { Buf ex; memset(&ex, 0, sizeof ex); emit_boxed_text(c, at, src, &ex); buf_puts(b, ex.p ? ex.p : ""); free(ex.p); }
          else if (ivt != TY_POLY && at == TY_POLY) emit_unbox_text(c, ivt, src, b);
          else buf_puts(b, src);
        }
        else if (kwh >= 0) {
          int vnode = struct_kwarg_value(c, kwh, cls->ivars[a] + 1);
          if (vnode < 0) {
            /* CRuby raises ArgumentError when a keyword super omits a member.
               The trailing zero-value is dead (sp_raise_cls is noreturn) but
               still type-checks against the member slot -- a value-type-object
               member is an inline struct, so `NULL` won't assign; give it the
               compound-literal zero instead. */
            buf_printf(b, "(sp_raise_cls(\"ArgumentError\", \"missing keyword: :%s\"), ",
                       cls->ivars[a] + 1);
            if (comp_ty_value_obj(c, ivt))
              buf_printf(b, "(sp_%s){0})", c->classes[ty_object_class(ivt)].c_name);
            else
              buf_printf(b, "%s)", default_value(ivt));
          }
          else {
            TyKind at = comp_ntype(c, vnode);
            if (ivt == TY_POLY && at != TY_POLY) emit_boxed(c, vnode, b);
            else if (ivt != TY_POLY && at == TY_POLY) {
              Buf ex; memset(&ex, 0, sizeof ex); emit_expr(c, vnode, &ex);
              emit_unbox_text(c, ivt, ex.p ? ex.p : "", b); free(ex.p);
            }
            else emit_expr(c, vnode, b);
          }
        }
        else {
          TyKind at = comp_ntype(c, sargv[a]);
          if (ivt == TY_POLY && at != TY_POLY) emit_boxed(c, sargv[a], b);
          else if (ivt != TY_POLY && at == TY_POLY) {
            /* poly arg (e.g. an initialize param that stayed poly) into a scalar
               member slot: unbox to the member's C type. */
            Buf ex; memset(&ex, 0, sizeof ex); emit_expr(c, sargv[a], &ex);
            emit_unbox_text(c, ivt, ex.p ? ex.p : "", b); free(ex.p);
          }
          else emit_expr(c, sargv[a], b);
        }
        buf_puts(b, ", ");
      }
      buf_printf(b, "%s)", default_value(comp_ntype(c, id)));
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
  buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].c_name, mc(uname), c->classes[defcls].c_name, g_self);
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
    if (m->ret != TY_INT && m->ret != TY_POLY && m->ret != TY_FLOAT) continue;  /* unusable return -> not-comparable */
    LocalVar *p = scope_local(m, m->pnames[0]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    const char *dcn = c->classes[defcls].c_name;
    int self_vt = c->classes[defcls].is_value_type;
    int cid = comp_class_index(c, c->classes[k].name);
    char argbuf[160];
    int obj_operand = 0;
    int pcid = -1;
    const char *scalar_guard = NULL;  /* operand tag a scalar-typed param requires */
    if (ty_is_object(pt)) {
      int pcls = ty_object_class(pt);
      const char *pcn = c->classes[pcls].name;
      pcid = comp_class_index(c, pcn);   /* guard b against the OPERAND's class, not the receiver's */
      snprintf(argbuf, sizeof argbuf, "%s(sp_%s *)b.v.p",
               c->classes[pcls].is_value_type ? "*" : "", pcn);
      obj_operand = 1;
    } else if (pt == TY_POLY) {
      snprintf(argbuf, sizeof argbuf, "b");
    } else if (pt == TY_INT) {
      /* a scalar-typed `<=>` param (bound from scalar call sites) still gets
         an arm: guard the operand's tag, unbox, and let the body's own nil
         return (the is_a? mismatch branch) drive the not-comparable path */
      scalar_guard = "SP_TAG_INT";
      snprintf(argbuf, sizeof argbuf, "b.v.i");
    } else if (pt == TY_FLOAT) {
      scalar_guard = "SP_TAG_FLT";
      snprintf(argbuf, sizeof argbuf, "b.v.f");
    } else {
      continue;
    }
    buf_printf(b, "    case %d: {\n", cid);
    if (obj_operand)
      buf_printf(b, "      if (b.tag != SP_TAG_OBJ || b.cls_id != %d) { *comparable = FALSE; return 0; }\n", pcid);
    if (scalar_guard)
      buf_printf(b, "      if (b.tag != %s) { *comparable = FALSE; return 0; }\n", scalar_guard);
    if (m->ret == TY_INT) {
      buf_printf(b, "      *comparable = TRUE; return (mrb_int)sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
    } else if (m->ret == TY_FLOAT) {
      /* a Float `<=>` result is a valid comparison (CRuby): use its sign */
      buf_printf(b, "      mrb_float _rf = sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
      buf_puts(b, "      *comparable = TRUE; return (_rf > 0) - (_rf < 0);\n");
    } else {
      /* poly `<=>`: an Integer or Float result is comparable (use its sign);
         nil or any other type (String, ...) is incomparable -> ArgumentError */
      buf_printf(b, "      sp_RbVal _r = sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
      buf_puts(b, "      if (_r.tag == SP_TAG_INT) { *comparable = TRUE; return _r.v.i; }\n");
      buf_puts(b, "      if (_r.tag == SP_TAG_FLT) { *comparable = TRUE; return (_r.v.f > 0) - (_r.v.f < 0); }\n");
      buf_puts(b, "      *comparable = FALSE; return 0;\n");
    }
    buf_puts(b, "    }\n");
  }
  buf_puts(b, "    default: break;\n");
  buf_puts(b, "  }\n  *comparable = FALSE; return 0;\n}\n");
}

/* 1 if instantiated class k defines both #hash and #eql? with emittable shapes --
   the Ruby idiom for a custom Hash key. Validates BOTH signatures here so the two
   hooks are generated all-or-nothing: a class that passes gets both a hash arm and
   an eql arm, one that fails gets neither and falls through to pointer identity.
   #eql?'s typed-object param must be class k itself (the hook only ever compares
   two keys of the same cls_id), so the arg cast is never to an unrelated struct. */
static int class_is_hashkey(Compiler *c, int k) {
  if (!c->classes[k].instantiated) return 0;
  int h_mi = comp_method_in_chain(c, k, "hash", NULL);
  int e_mi = comp_method_in_chain(c, k, "eql?", NULL);
  if (h_mi < 0 || e_mi < 0) return 0;

  Scope *h_m = &c->scopes[h_mi];
  if (h_m->nparams > 0 || (h_m->ret != TY_INT && h_m->ret != TY_POLY)) return 0;

  Scope *e_m = &c->scopes[e_mi];
  if (e_m->nparams < 1 || e_m->rest_idx >= 0 || (e_m->ret != TY_BOOL && e_m->ret != TY_POLY)) return 0;
  LocalVar *pp = scope_local(e_m, e_m->pnames[0]);
  TyKind pt = (pp && pp->type != TY_UNKNOWN) ? pp->type : TY_POLY;
  if (ty_is_object(pt)) { if (ty_object_class(pt) != k) return 0; }
  else if (pt != TY_POLY) return 0;
  return 1;
}

/* 1 if instantiated class k is a pure Struct/Data (no user ==/eql?/hash), so
   it is a value hash key: hash combines the member hashes and eql? is the
   field-wise value == (via sp_obj_eq_dispatch). #2660 */
static int class_is_valuekey(Compiler *c, int k) {
  ClassInfo *ci = &c->classes[k];
  return ci->instantiated && (ci->is_struct || ci->is_data) &&
         comp_method_in_chain(c, k, "==", NULL) < 0 &&
         comp_method_in_chain(c, k, "eql?", NULL) < 0 &&
         comp_method_in_chain(c, k, "hash", NULL) < 0;
}

/* Generate sp_gen_obj_hash / sp_gen_obj_eql: cls_id switches calling each such
   class's user #hash / #eql?, installed as sp_obj_hash_hook / sp_obj_eql_hook so
   the PolyPolyHash key machinery honors value semantics for user objects (two
   value-equal keys collide and compare equal). A class whose methods have an
   unusable shape omits its arm and falls through to pointer identity (the
   Object#hash / equal? default). The eql hook is only reached for two keys of
   the same cls_id (the runtime pre-filters), so both pointers are that class. */
static void emit_obj_hashkey_dispatch(Compiler *c, Buf *b) {
  /* Emission uses mc(m->name) throughout: `alias eql? ==` resolves to the
     target method's scope, whose C symbol carries the target's name. */
  buf_puts(b, "static mrb_int sp_gen_obj_hash(int cls_id, void *p) {\n  switch (cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_hashkey(c, k)) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "hash", &defcls);
    Scope *m = &c->scopes[mi];   /* signature validated in class_is_hashkey */
    const char *dcn = c->classes[defcls].c_name;
    const char *slf = c->classes[defcls].is_value_type ? "*" : "";
    buf_printf(b, "    case %d: ", comp_class_index(c, c->classes[k].name));
    if (m->ret == TY_INT)
      buf_printf(b, "return (mrb_int)sp_%s_%s(%s(sp_%s *)p);\n", dcn, mc(m->name), slf, dcn);
    else
      buf_printf(b, "return sp_rbval_hash_key(sp_%s_%s(%s(sp_%s *)p));\n", dcn, mc(m->name), slf, dcn);
  }
  /* Struct/Data value keys: hash is the running combination of member hashes. */
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_valuekey(c, k)) continue;
    ClassInfo *ci = &c->classes[k];
    buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)p; mrb_int _h = %d;\n",
               comp_class_index(c, ci->name), ci->c_name, ci->c_name, ci->nivars + 1);
    for (int i = 0; i < ci->nivars; i++) {
      char fe[128]; snprintf(fe, sizeof fe, "o->iv_%s", ci->ivars[i] + 1);
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, ci->ivar_types[i], fe, &bx);
      buf_printf(b, "      _h = _h * 31 + sp_rbval_hash_key(%s);\n", bx.p ? bx.p : fe);
      free(bx.p);
    }
    buf_puts(b, "      return _h; }\n");
  }
  buf_puts(b, "    default: break;\n  }\n  return (mrb_int)(uintptr_t)p;\n}\n");

  buf_puts(b, "static mrb_bool sp_gen_obj_eql(int cls_id, void *a, void *b_) {\n  switch (cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_hashkey(c, k)) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "eql?", &defcls);
    Scope *m = &c->scopes[mi];   /* signature validated in class_is_hashkey */
    LocalVar *pp = scope_local(m, m->pnames[0]);
    TyKind pt = (pp && pp->type != TY_UNKNOWN) ? pp->type : TY_POLY;
    char argbuf[160];
    if (ty_is_object(pt)) {
      /* class_is_hashkey guarantees this object class == k, and b_ is a k key */
      int pcls = ty_object_class(pt);
      snprintf(argbuf, sizeof argbuf, "%s(sp_%s *)b_",
               c->classes[pcls].is_value_type ? "*" : "", c->classes[pcls].c_name);
    } else {
      snprintf(argbuf, sizeof argbuf, "sp_box_obj(b_, cls_id)");
    }
    const char *dcn = c->classes[defcls].c_name;
    const char *slf = c->classes[defcls].is_value_type ? "*" : "";
    buf_printf(b, "    case %d: ", comp_class_index(c, c->classes[k].name));
    if (m->ret == TY_BOOL)
      buf_printf(b, "return sp_%s_%s(%s(sp_%s *)a, %s);\n", dcn, mc(m->name), slf, dcn, argbuf);
    else
      buf_printf(b, "return sp_poly_truthy(sp_%s_%s(%s(sp_%s *)a, %s));\n", dcn, mc(m->name), slf, dcn, argbuf);
  }
  /* Struct/Data value keys: eql? is the field-wise value == (sp_obj_eq_dispatch). */
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_valuekey(c, k)) continue;
    int cid = comp_class_index(c, c->classes[k].name);
    buf_printf(b, "    case %d: return sp_obj_eq_dispatch(sp_box_obj(a, %d), sp_box_obj(b_, %d));\n",
               cid, cid, cid);
  }
  buf_puts(b, "    default: break;\n  }\n  return a == b_;\n}\n");
}

/* Generate sp_obj_eq_dispatch: a cls_id switch that compares two same-class
   Struct/Data instances field by field (each field via sp_poly_eq), so they
   compare by VALUE inside Array/Hash equality, include?/index/uniq, and as
   nested members. Installed as sp_obj_eq_hook. */
static void emit_obj_valeq_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static mrb_bool sp_obj_eq_dispatch(sp_RbVal a, sp_RbVal b) {\n  switch (a.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->instantiated || !(ci->is_struct || ci->is_data)) continue;
    /* a user-defined == wins; leave those to their own dispatch (they fall
       through to FALSE here, unchanged) */
    if (comp_method_in_chain(c, k, "==", NULL) >= 0) continue;
    buf_printf(b, "    case %d: { sp_%s *_a = (sp_%s *)a.v.p, *_b = (sp_%s *)b.v.p; if (!_a || !_b) return _a == _b; return ",
               comp_class_index(c, ci->name), ci->c_name, ci->c_name, ci->c_name);
    if (ci->nivars == 0) buf_puts(b, "1");
    for (int i = 0; i < ci->nivars; i++) {
      const char *iv = ci->ivars[i] + 1;  /* skip leading '@' */
      Buf ea; memset(&ea, 0, sizeof ea); Buf eb; memset(&eb, 0, sizeof eb);
      char fa[128], fb[128];
      snprintf(fa, sizeof fa, "_a->iv_%s", iv);
      snprintf(fb, sizeof fb, "_b->iv_%s", iv);
      emit_boxed_text(c, ci->ivar_types[i], fa, &ea);
      emit_boxed_text(c, ci->ivar_types[i], fb, &eb);
      buf_printf(b, "%ssp_poly_eq(%s, %s)", i ? " && " : "", ea.p ? ea.p : fa, eb.p ? eb.p : fb);
      free(ea.p); free(eb.p);
    }
    buf_puts(b, "; }\n");
  }
  /* A class with a reachable user-defined `==`: dispatch to it so Array#include?
     / #index / uniq (all through sp_poly_eq -> this hook) honor value equality.
     sp_poly_eq only consults the hook when both operands share a cls_id, so `a`
     and `b` are provably this class -- a poly `==` param takes `b` boxed, a
     same-class-typed param takes the unboxed pointer; other param shapes are
     left to pointer identity (#2884). */
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->instantiated) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "==", &defcls);
    if (mi < 0 || !c->scopes[mi].reachable || c->scopes[mi].ret != TY_BOOL) continue;
    Scope *m = &c->scopes[mi];
    if (m->nparams < 1) continue;
    LocalVar *p = scope_local(m, m->pnames[0]);
    TyKind pt = p ? p->type : TY_POLY;
    int poly_param = (pt == TY_POLY || pt == TY_UNKNOWN);
    int obj_param = ty_is_object(pt) && !comp_ty_value_obj(c, pt);
    if (!poly_param && !obj_param) continue;
    const char *dcn = c->classes[defcls].c_name;
    const char *slf = c->classes[defcls].is_value_type ? "*" : "";
    buf_printf(b, "    case %d: return sp_%s_%s(%s(sp_%s *)a.v.p, ",
               comp_class_index(c, ci->name), dcn, mc("=="), slf, dcn);
    if (obj_param) buf_printf(b, "(sp_%s *)b.v.p", c->classes[ty_object_class(pt)].c_name);
    else buf_puts(b, "b");
    buf_puts(b, ");\n");
  }
  buf_puts(b, "    default: break;\n  }\n  return FALSE;\n}\n");
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
  if (g_needs_class_machinery)
    buf_puts(b, "static int sp_poly_is_a(sp_RbVal obj, sp_Class klass);\n");
  if (g_gen_obj_hash)
    buf_puts(b, "static sp_RbVal sp_obj_to_hash(sp_RbVal v);\n");
  if (g_gen_obj_to_h)
    buf_puts(b, "static sp_RbVal sp_obj_to_h(sp_RbVal v);\n");
  if (g_gen_obj_with)
    buf_puts(b, "static sp_RbVal sp_obj_with(sp_RbVal v, sp_RbVal ov);\n");
  if (g_gen_obj_hashkey)
    buf_puts(b, "static mrb_int sp_gen_obj_hash(int cls_id, void *p);\n"
                "static mrb_bool sp_gen_obj_eql(int cls_id, void *a, void *b_);\n");
  if (g_gen_obj_valeq)
    buf_puts(b, "static mrb_bool sp_obj_eq_dispatch(sp_RbVal a, sp_RbVal b);\n");
  buf_puts(b, "static void sp_re_init(void) {\n");
  if (g_uses_symbols)
    buf_puts(b, "  sp_sym_name_fn = sp_sym_to_s;\n");
  if (g_has_user_cmp)
    buf_puts(b, "  sp_obj_cmp_hook = sp_obj_cmp_dispatch;\n");
  if (g_gen_obj_hashkey)
    buf_puts(b, "  sp_obj_hash_hook = sp_gen_obj_hash;\n  sp_obj_eql_hook = sp_gen_obj_eql;\n");
  if (g_gen_obj_valeq)
    buf_puts(b, "  sp_obj_eq_hook = sp_obj_eq_dispatch;\n");
  if (g_needs_class_machinery)
    buf_puts(b, "  sp_user_exc_parent_fn = sp_user_exc_parent;\n"
                "  sp_poly_is_a_hook = sp_poly_is_a;\n");
  /* Replace the runtime's hook with the superset that also marks this
     program's heap-typed globals/constants/class-ivars (it chains to
     sp_re_mark_globals itself). Skipped when there are none -- the marker would
     equal the constructor-installed default, so it isn't emitted either. */
  if (g_has_user_global_marks)
    buf_puts(b, "  sp_gc_mark_globals_hook = sp_mark_user_globals;\n");
  /* Install the Marshal vtable: the construction wrappers (sp_runtime.h) plus
     the generated symbol interner and per-class object dump/load. */
  if (g_emit_sym_rt)
    buf_puts(b, "  sp_json_sym_intern_fn = sp_sym_intern;\n");
  if (g_emit_obj_dispatch) {
    buf_puts(b, "  sp_obj_inspect_fn = sp_obj_inspect_sw;\n");
    buf_puts(b, "  sp_obj_to_s_fn = sp_obj_to_s_sw;\n");
  }
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
  if (g_gen_obj_hash)
    buf_puts(b, "  sp_obj_to_hash_fn = sp_obj_to_hash;\n");
  if (g_gen_obj_to_h)
    buf_puts(b, "  sp_obj_to_h_fn = sp_obj_to_h;\n");
  if (g_gen_obj_with)
    buf_puts(b, "  sp_obj_with_fn = sp_obj_with;\n");
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
    case TY_FLOAT_RANGE:           buf_puts(b, "Range[Float]"); break;
    case TY_TIME:                  buf_puts(b, "Time"); break;
    case TY_REGEX:                 buf_puts(b, "Regexp"); break;
    case TY_MATCHDATA:             buf_puts(b, "MatchData"); break;
    case TY_EXCEPTION:             buf_puts(b, "Exception"); break;
    case TY_COMPLEX:               buf_puts(b, "Complex"); break;
    case TY_RATIONAL:              buf_puts(b, "Rational"); break;
    case TY_PROC: case TY_CURRY:   buf_puts(b, "Proc"); break;
    case TY_FIBER:                 buf_puts(b, "Fiber"); break;
    case TY_THREAD:                buf_puts(b, "Thread"); break;
    case TY_QUEUE:                 buf_puts(b, "Thread::Queue"); break;
    case TY_MUTEX:                 buf_puts(b, "Thread::Mutex"); break;
    case TY_CONDVAR:               buf_puts(b, "Thread::ConditionVariable"); break;
    case TY_RANDOM:                buf_puts(b, "Random"); break;
    case TY_DIR:                   buf_puts(b, "Dir"); break;
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
  g_uses_regex = 0; g_uses_argv = 0; g_uses_threads = 0;
  g_uses_program_name = 0;
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
      else if (nm && (sp_streq(nm, "$0") || sp_streq(nm, "$PROGRAM_NAME"))) g_uses_program_name = 1;
    }
    else if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (!nm) continue;
      /* Reflection that YIELDS symbols the source never spells: without this
         the program has no SymbolNode, so sp_sym_name_fn stays uninstalled and
         sp_poly_cmp falls back to comparing symbols by id -- `constants.sort`
         would come out in intern order rather than by name (#2674). */
      /* `refine` is dropped rather than emitted (the block's defs are registered
         on the enclosing module), so emit_call never sees it and the failure
         surfaces inside the block instead -- report the refinement itself here,
         before any of that runs. `using` is caught at its call site. #2652 */
      if (sp_streq(nm, "refine") && nt_ref(nt, i, "receiver") < 0 &&
          nt_ref(nt, i, "block") >= 0 && !diag_user_defines(c, "refine"))
        unsupported_feature(c, i,
          "Refinements are not supported by AOT compilation: scope-keyed dispatch is "
          "incompatible with direct C calls. Reopen the class instead (see docs/limitations.md)");
      if (sp_streq(nm, "to_sym") || sp_streq(nm, "intern") ||
          sp_streq(nm, "constants") || sp_streq(nm, "members") ||
          sp_streq(nm, "instance_methods") || sp_streq(nm, "public_instance_methods") ||
          sp_streq(nm, "private_instance_methods") || sp_streq(nm, "protected_instance_methods") ||
          sp_streq(nm, "methods") || sp_streq(nm, "instance_variables") ||
          sp_streq(nm, "class_variables")) g_uses_symbols = 1;
    }
  }
  /* Generic object reflection: when a native package declared it consumes
     object->hash reflection (native_obj_reflect, e.g. json) and the program
     defines any Struct, emit + install sp_obj_to_hash. No feature is named
     here -- the package's require is the declaration. */
  g_gen_obj_hash = 0;
  if (c->native_obj_reflect) {
    for (int i = 0; i < c->nclasses; i++)
      if (c->classes[i].is_struct) { g_gen_obj_hash = 1; break; }
  }
  /* Any instantiated Struct/Data gets the symbol-keyed to_h dispatch, so a
     Struct/Data read out of a poly container answers #to_h (#2906). */
  g_gen_obj_to_h = 0;
  for (int i = 0; i < c->nclasses; i++)
    if ((c->classes[i].is_struct || c->classes[i].is_data) && c->classes[i].instantiated) { g_gen_obj_to_h = 1; break; }
  /* A plain (no custom initialize) instantiated Data gets the poly Data#with
     dispatch; a custom-init Data is skipped there, so don't count it (#2890). */
  g_gen_obj_with = 0;
  for (int i = 0; i < c->nclasses; i++) {
    if (!c->classes[i].is_data || !c->classes[i].instantiated) continue;
    int sc = comp_method_in_chain(c, i, "initialize", NULL);
    if (sc >= 0 && c->scopes[sc].reachable) continue;
    g_gen_obj_with = 1; break;
  }
  /* Runtime-render reach: does any slot in the program carry a type whose
     rendering can call into the symbol runtime (sp_sym_to_s / sp_sym_intern)
     or the class-name table (sp_class_to_s)? A boxed poly value can hold ANY
     tag at run time (SP_TAG_SYM, SP_TAG_CLASS, a boxed hash), so poly and the
     poly-carrying containers count. Conservative: a flag set just means the
     helper is emitted; `puts "hello"` sets neither. */
  g_emit_sym_rt = g_uses_symbols;
  g_emit_class_names = (c->nclasses > 0) || g_needs_class_machinery;
  g_emit_obj_dispatch = (c->nclasses > 0);
  {
    int polyish = 0, has_class = 0;
#define SP_TT(t) do { TyKind _t = (t);       if (_t == TY_POLY || ty_is_array(_t) || ty_is_hash(_t)) polyish = 1;       else if (_t == TY_SYMBOL) g_emit_sym_rt = 1;       else if (_t == TY_CLASS) has_class = 1; } while (0)
    for (int i = 0; i < nt->count && i < c->node_cap; i++) SP_TT(c->ntype[i]);
    for (int sc = 0; sc < c->nscopes; sc++) {
      SP_TT(c->scopes[sc].ret);
      for (int l = 0; l < c->scopes[sc].nlocals; l++) SP_TT(c->scopes[sc].locals[l].type);
    }
    for (int k = 0; k < c->nclasses; k++)
      for (int j = 0; j < c->classes[k].nivars; j++) SP_TT(c->classes[k].ivar_types[j]);
#undef SP_TT
    if (polyish) { g_emit_sym_rt = 1; g_emit_class_names = 1; }
    if (has_class) g_emit_class_names = 1;
    /* The two banks reference each other through the header's render helpers
       (sp_poly_inspect renders both symbols and class names), so they are
       emitted together or not at all -- SP_TU_NO_POLY_RENDER supplies the
       fallbacks only when BOTH are absent. */
    if (g_emit_sym_rt || g_emit_class_names) { g_emit_sym_rt = 1; g_emit_class_names = 1; }
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
/* A reified Binding (name->slot local environment) does not exist in an AOT
   binary. The one statically-decidable use, `binding.local_variable_get(:name)`
   for an in-scope name, is rewritten to a direct read in analyze; any binding
   call that survives (local_variable_set, local_variables, a non-literal name, a
   name that is not in scope, a bare `binding` value, ...) has no static answer,
   so reject it loudly at build time instead of aborting at runtime. */
static void reject_binding(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int s = 0; s < c->nscopes; s++)
    if (c->scopes[s].name && sp_streq(c->scopes[s].name, "binding")) return;  /* user-defined */
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty || !sp_streq(ty, "CallNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, "binding")) continue;
    if (nt_ref(nt, id, "receiver") >= 0) continue;             /* Kernel#binding is receiverless */
    int args = nt_ref(nt, id, "arguments");
    if (args >= 0) { int ac = 0; nt_arr(nt, args, "arguments", &ac); if (ac > 0) continue; }
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !c->scopes[sc].reachable) continue;
    unsupported(c, id, "binding is unsupported (no reified local environment in an "
                       "AOT binary; only binding.local_variable_get(:name) for an "
                       "in-scope name is)");
  }
}

/* A method result (`.to_sym`, `.join`, ...) or a string/symbol interpolation
   produces a fresh value that need not appear anywhere as a source literal. */
static int node_is_computed_name(const NodeTable *nt, int n) {
  const char *ty = n >= 0 ? nt_type(nt, n) : NULL;
  if (!ty) return 0;
  return sp_streq(ty, "CallNode") || sp_streq(ty, "InterpolatedStringNode") ||
         sp_streq(ty, "InterpolatedSymbolNode");
}

/* A dynamic-send name is genuinely runtime-computed -- and so cannot be covered
   by desugar_dynamic_send's literal-derived arm set -- when it is a computed
   expression directly, or a local assigned such a computation. A bare variable
   or block parameter whose values are the program's symbol/string literals
   (`m = :upcase; x.send(m)`, `%w[a b].each { |k| x.send(k) }`) is NOT computed:
   its runtime value is one of those literals, which the arm set already covers.
   Only a computed name is rejected, so it fails loudly at build time rather than
   silently raising the wrong NoMethodError for a method the arms never built. */
static int send_name_is_computed(Compiler *c, int arg) {
  const NodeTable *nt = c->nt;
  if (node_is_computed_name(nt, arg)) return 1;
  const char *aty = arg >= 0 ? nt_type(nt, arg) : NULL;
  if (!aty || !sp_streq(aty, "LocalVariableReadNode")) return 0;
  const char *vn = nt_str(nt, arg, "name");
  int scope = (arg >= 0 && arg < nt->count) ? c->nscope[arg] : -1;
  if (!vn) return 0;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, id) {
    if (c->nscope[id] != scope) continue;
    const char *wn = nt_str(nt, id, "name");
    if (!wn || !sp_streq(wn, vn)) continue;
    if (node_is_computed_name(nt, nt_ref(nt, id, "value"))) return 1;
  }
  return 0;
}

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
    /* lowered to a static name-dispatch (desugar_dynamic_send): the arm set can
       only cover a name that is provably one of the program's literals; a
       genuinely runtime-computed name falls through to the loud reject below. */
    { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn);
      if (dn > 0 && !send_name_is_computed(c, av[0])) continue; }
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
  /* Feature flags must be computed BEFORE the preamble emit below: the
     symbol runtime, class-name table, inspect dispatch, and Marshal stub
     emissions are gated on them (a `puts "hello"` program gets none). */
  g_needs_class_machinery = program_needs_class_machinery(c);
  scan_prologue_features(c);

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
  reject_binding(c);

  Buf b; memset(&b, 0, sizeof b);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_proc_counter = 0;
  g_needs_at_exit = 0;
  g_re_count = 0;
  buf_puts(&b, "/* Generated by Spinel AOT compiler */\n");
  /* No poly-renderable value anywhere: skip the sp_poly_inspect hook install
     in the header (it would force sp_sym_to_s / sp_class_to_s definitions
     this TU deliberately omits). */
  if (!g_emit_sym_rt)
    buf_puts(&b, "#define SP_TU_NO_POLY_RENDER 1\n");
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
      /* A function taking a callback (e.g. qsort, bsearch) is declared by a
         system header; emitting our own extern -- whose array/pointer specs may
         not match the header's void* -- conflicts under gcc. Skip it and call
         the header-declared symbol directly (implicit pointer conversions). */
      int has_cb = 0;
      for (int ai = 0; ai < cf->ffi_funcs[fi].nargs; ai++)
        if (ffi_find_callback(cf, cf->ffi_funcs[fi].mod, cf->ffi_funcs[fi].args[ai]) >= 0) { has_cb = 1; break; }
      if (has_cb) continue;
      int na = cf->ffi_funcs[fi].nargs;
      /* A variadic function (trailing :varargs) gets NO extern: redeclaring a
         libc variadic already declared by a system header -- e.g. printf, which
         glibc declares with fortify attributes/inlines -- conflicts under some
         libc + compiler combinations (notably gcc + glibc _FORTIFY_SOURCE). The
         call site instead casts the header-declared symbol to a variadic
         function pointer, which cannot conflict. A user (non-libc) variadic
         function must be declared via a header supplied through ffi_cflags. */
      if (na > 0 && sp_streq(cf->ffi_funcs[fi].args[na - 1], "varargs")) continue;
      buf_puts(&b, "extern ");
      buf_puts(&b, ffi_c_type(ret));
      buf_puts(&b, " ");
      buf_puts(&b, cf->ffi_funcs[fi].csym ? cf->ffi_funcs[fi].csym : cf->ffi_funcs[fi].name);
      buf_puts(&b, "(");
      for (int ai = 0; ai < na; ai++) {
        if (ai) buf_puts(&b, ", ");
        buf_puts(&b, ffi_c_type(cf->ffi_funcs[fi].args[ai]));
      }
      if (na == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    /* Byte count for the :binstr return mode (defined in sp_net.c). */
    if (any_binstr) buf_puts(&b, "extern int sp_net_bin_len;\n");

    /* native_func externs (Path B): prototype each bound C symbol so the
       generated TU needs no package header. Deduped by symbol (generate and
       dump may share one). Type specs are the spinel type language. */
    for (int nvi = 0; nvi < cf->n_native_funcs; nvi++) {
      const char *csym = cf->native_funcs[nvi].csym;
      int seen = 0;
      for (int pj = 0; pj < nvi; pj++)
        if (sp_streq(cf->native_funcs[pj].csym, csym)) { seen = 1; break; }
      if (seen) continue;
      buf_puts(&b, "extern ");
      buf_puts(&b, native_c_type(cf->native_funcs[nvi].ret));
      buf_puts(&b, " "); buf_puts(&b, csym); buf_puts(&b, "(");
      for (int ai = 0; ai < cf->native_funcs[nvi].nargs; ai++) {
        if (ai) buf_puts(&b, ", ");
        buf_puts(&b, native_c_type(cf->native_funcs[nvi].args[ai]));
      }
      if (cf->native_funcs[nvi].nargs == 0) buf_puts(&b, "void");
      buf_puts(&b, ");\n");
    }
    /* forward-declare each native class's package struct (incomplete: the TU
       holds only pointers) so the method externs below can name it. The struct
       tag comes from c_struct (the package's C type), but the *type name* the
       generated code references is the class's own c_name -- when several native
       classes share one backing struct (e.g. every socket class reuses
       sp_Socket) their c_names differ, so declare a typedef alias per class.
       Also cover native classes whose c_name was mangled (u_<Name>) because it
       collides with a runtime typedef, and classes that are native but whose
       *user* method forward-decls reference the c_name (the user codegen
       declares sp_<CName>_<method> taking a `sp_<CName> *` self). */
    for (int nci = 0; nci < cf->nclasses; nci++)
      if (cf->classes[nci].is_native_class && cf->classes[nci].c_struct) {
        const char *cs = cf->classes[nci].c_struct;
        const char *nm = cf->classes[nci].c_name;
        buf_printf(&b, "typedef struct %s_s %s;\n", cs, cs);
        if (strcmp(nm, cs))
          buf_printf(&b, "typedef struct %s_s %s;\n", cs, nm);
        /* The user-method forward decls emit `sp_<CName>` (not the mangled
           c_name). Synthesize that alias too so the self pointer type resolves. */
        char ubuf[64];
        snprintf(ubuf, sizeof ubuf, "sp_%s", cf->classes[nci].name);
        if (strcmp(ubuf, cs) && strcmp(ubuf, nm))
          buf_printf(&b, "typedef struct %s_s %s;\n", cs, ubuf);
      }
    /* native_method/native_new externs: prototype each C-backed method so the
       generated TU needs no package header. A constructor returns the struct
       pointer and takes cls_id first (the compiler stamps the assigned id); an
       instance method takes the receiver pointer first. Deduped by symbol. */
    for (int mi = 0; mi < cf->n_native_methods; mi++) {
      NativeMethod *m = &cf->native_methods[mi];
      int seen = 0;
      for (int pj = 0; pj < mi; pj++)
        if (sp_streq(cf->native_methods[pj].csym, m->csym)) { seen = 1; break; }
      if (seen) continue;
      const char *cstruct = cf->classes[m->class_id].c_struct;
      buf_puts(&b, "extern ");
      if (m->kind == 1) buf_printf(&b, "%s *%s(mrb_int", cstruct, m->csym);   /* ctor: cls_id first */
      else if (sp_streq(m->ret, "self")) buf_printf(&b, "%s *%s(%s *", cstruct, m->csym, cstruct);
      else { buf_printf(&b, "%s %s(%s *", native_c_type(m->ret), m->csym, cstruct); }
      for (int ai = 0; ai < m->nargs; ai++) { buf_puts(&b, ", "); buf_puts(&b, native_c_type(m->args[ai])); }
      buf_puts(&b, ");\n");
    }
    /* native_obj link markers: the spinel driver links each object only when
       its module's require-gate feature is enabled (i.e. the require appears). */
    for (int noi = 0; noi < cf->n_native_objs; noi++) {
      const char *feat = cf->native_objs[noi].feat;
      if (!feat || !feat[0] || sp_feature_enabled(feat))
        buf_printf(&b, "/* SPINEL_LINK_OBJ: %s */\n", cf->native_objs[noi].path);
    }
    for (int bi = 0; bi < cf->n_ffi_bufs; bi++) {
      buf_printf(&b, "static char sp_ffi_buf_%s_%s[%d];\n",
                 cf->ffi_bufs[bi].mod, cf->ffi_bufs[bi].name, cf->ffi_bufs[bi].size);
    }
    /* ffi_struct typedefs: the C compiler owns the layout (offsets/padding),
       so the generated accessors use plain member access, not manual offsets. */
    for (int si = 0; si < cf->n_ffi_structs; si++) {
      buf_puts(&b, "typedef struct { ");
      for (int f = 0; f < cf->ffi_structs[si].nfields; f++)
        buf_printf(&b, "%s %s; ", ffi_c_type(cf->ffi_structs[si].fields[f].spec),
                   cf->ffi_structs[si].fields[f].name);
      buf_printf(&b, "} sp_ffi_struct_%s_%s;\n",
                 cf->ffi_structs[si].mod, cf->ffi_structs[si].name);
    }
  }
  if (g_emit_sym_rt) {
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
  /* sp_class_to_s serves the runtime's SP_TAG_CLASS render arms (sp_poly_puts
     / sp_poly_to_s / sp_poly_inspect). Emitted whenever anything in the
     program could reach those (user classes, class values, any poly-capable
     slot -- see the render-reach scan); a purely-scalar program skips it. */
  if (g_emit_class_names) {
    buf_puts(&b, "static const char *sp_class_to_s(sp_Class c){if(sp_class_nil_p(c))return SPL(\"nil\");if(c.name)return c.name;switch(c.cls_id){");
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
    buf_puts(&b, "case -132:return SPL(\"IndexError\");case -133:return SPL(\"KeyError\");");
    buf_puts(&b, "case -134:return SPL(\"RangeError\");case -135:return SPL(\"FloatDomainError\");");
    buf_puts(&b, "case -136:return SPL(\"ZeroDivisionError\");case -137:return SPL(\"FrozenError\");");
    buf_puts(&b, "case -138:return SPL(\"IOError\");case -139:return SPL(\"LocalJumpError\");");
    buf_puts(&b, "case -140:return SPL(\"NotImplementedError\");case -141:return SPL(\"ScriptError\");");
    buf_puts(&b, "case -142:return SPL(\"Rational\");case -143:return SPL(\"Regexp\");");
    buf_puts(&b, "case -144:return SPL(\"Enumerator\");case -145:return SPL(\"Struct\");");
    buf_puts(&b, "case -146:return SPL(\"Data\");");
    buf_puts(&b, "case -147:return SPL(\"SyntaxError\");case -148:return SPL(\"SecurityError\");");
    buf_puts(&b, "case -149:return SPL(\"RegexpError\");case -150:return SPL(\"EncodingError\");");
    buf_puts(&b, "case -151:return SPL(\"SignalException\");case -152:return SPL(\"Interrupt\");");
    buf_puts(&b, "case -153:return SPL(\"ThreadError\");case -154:return SPL(\"FiberError\");");
    buf_puts(&b, "case -155:return SPL(\"ClosedQueueError\");case -156:return SPL(\"UncaughtThrowError\");");
    buf_puts(&b, "case -157:return SPL(\"NoMatchingPatternError\");case -158:return SPL(\"NoMatchingPatternKeyError\");");
    buf_puts(&b, "case -159:return SPL(\"EOFError\");case -160:return SPL(\"Math::DomainError\");");
    buf_puts(&b, "case -161:return SPL(\"SystemExit\");case -162:return SPL(\"Signal\");");
    buf_puts(&b, "case -163:return SPL(\"Process::Status\");");
    buf_puts(&b, "default:return \"\";} }\n\n");
  }
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
  buf_puts(&b, "  return(c.cls_id==-114||c.cls_id==-115||c.cls_id==-119||c.cls_id==-162);\n}\n");

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
        /* a Struct/Data-generated class sits under the Struct/Data builtin
           (Pt = Struct.new(:x) -> Pt.superclass == Struct, CRuby) */
        int builtin_par = c->classes[i].is_struct ? (c->classes[i].is_data ? -146 : -145)
                                                  : -116;  /* Object */
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
  /* nil has no superclass: stay nil rather than falling to the Object default,
     which would resurrect a terminated chain into a cycle (#2654) */
  buf_puts(&b, "  if(sp_class_nil_p(c))return SP_CLASS_NIL;\n");
  buf_puts(&b, "  switch(c.cls_id){\n");
  /* Integer, Float, Complex, Rational -> Numeric -> Object */
  buf_puts(&b, "  case -100:case -101:case -131:case -142: return ((sp_Class){-113});\n"); /* -> Numeric */
  /* Numeric, String, Array, Hash, Range, Symbol, Time -> Object */
  buf_puts(&b, "  case -102:case -103:case -104:case -105:case -106:case -107:case -113: return ((sp_Class){-116});\n");
  /* Exception -> Object */
  buf_puts(&b, "  case -122: return ((sp_Class){-116});\n");
  /* StandardError, ScriptError -> Exception */
  buf_puts(&b, "  case -123:case -141: return ((sp_Class){-122});\n");
  /* TypeError, ArgumentError, NameError, StopIteration, RuntimeError,
     IndexError, RangeError, ZeroDivisionError, IOError, LocalJumpError
     -> StandardError (RuntimeError previously said Exception; CRuby says
     StandardError) */
  buf_puts(&b, "  case -124:case -125:case -126:case -127:"
               "case -132:case -134:case -136:case -138:case -139: return ((sp_Class){-123});\n");
  /* StopIteration -> IndexError (#2760) */
  buf_puts(&b, "  case -129: return ((sp_Class){-132});\n");
  /* KeyError -> IndexError; FloatDomainError -> RangeError; FrozenError ->
     RuntimeError; NotImplementedError -> ScriptError */
  buf_puts(&b, "  case -133: return ((sp_Class){-132});\n");
  buf_puts(&b, "  case -135: return ((sp_Class){-134});\n");
  buf_puts(&b, "  case -137: return ((sp_Class){-124});\n");
  buf_puts(&b, "  case -140: return ((sp_Class){-141});\n");
  /* NoMethodError -> NameError */
  buf_puts(&b, "  case -128: return ((sp_Class){-127});\n");
  /* the rarer exception subclasses (#2768):
     RegexpError, EncodingError, ThreadError, FiberError,
     NoMatchingPatternError, Math::DomainError -> StandardError */
  buf_puts(&b, "  case -149:case -150:case -153:case -154:case -157:case -160: return ((sp_Class){-123});\n");
  /* SyntaxError -> ScriptError; SecurityError, SignalException -> Exception */
  buf_puts(&b, "  case -147: return ((sp_Class){-141});\n");
  buf_puts(&b, "  case -148:case -151:case -161: return ((sp_Class){-122});\n");
  /* Interrupt -> SignalException; ClosedQueueError -> StopIteration;
     UncaughtThrowError -> ArgumentError; NoMatchingPatternKeyError ->
     NoMatchingPatternError; EOFError -> IOError */
  buf_puts(&b, "  case -152: return ((sp_Class){-151});\n");
  buf_puts(&b, "  case -155: return ((sp_Class){-129});\n");
  buf_puts(&b, "  case -156: return ((sp_Class){-126});\n");
  buf_puts(&b, "  case -158: return ((sp_Class){-157});\n");
  buf_puts(&b, "  case -159: return ((sp_Class){-138});\n");
  /* NilClass, TrueClass, FalseClass, Proc, Struct, Data -> Object */
  buf_puts(&b, "  case -110:case -111:case -112:case -118:case -145:case -146: return ((sp_Class){-116});\n");
  /* Module -> Object, Class -> Module */
  buf_puts(&b, "  case -108: return ((sp_Class){-116});\n");
  buf_puts(&b, "  case -109: return ((sp_Class){-108});\n");
  /* Object -> BasicObject */
  buf_puts(&b, "  case -116: return ((sp_Class){-117});\n");
  /* BasicObject: the hierarchy root -- its superclass is nil (#2654) */
  buf_puts(&b, "  case -117: return SP_CLASS_NIL;\n");
  buf_puts(&b, "  default: return ((sp_Class){-116});\n  }\n}\n");

  buf_puts(&b, "static int sp_class_lt(sp_Class a,sp_Class b){return a.cls_id!=b.cls_id&&sp_class_is_ancestor(b,a);}\n");
  buf_puts(&b, "static int sp_class_le(sp_Class a,sp_Class b){return sp_class_is_ancestor(b,a);}\n");
  buf_puts(&b, "static int sp_class_gt(sp_Class a,sp_Class b){return sp_class_lt(b,a);}\n");
  buf_puts(&b, "static int sp_class_ge(sp_Class a,sp_Class b){return sp_class_le(b,a);}\n");
  /* Tri-state class ordering: CRuby's Class#< / <= / > / >= / <=> answer nil
     for two classes with no subclass relationship (not false / not raising).
     Macros so `sp_class_le` resolves at the call site to whichever version is
     in effect there -- the module-aware sp_class_le_mod when the program mixes
     in modules (Integer < Comparable), the plain chain walk otherwise. */
  buf_puts(&b, "#define sp_class_lt3(A,B) ({ sp_Class _cx=(A),_cy=(B); _cx.cls_id==_cy.cls_id?sp_box_bool(0):(sp_class_le(_cx,_cy)?sp_box_bool(1):(sp_class_le(_cy,_cx)?sp_box_bool(0):sp_box_nil())); })\n");
  buf_puts(&b, "#define sp_class_le3(A,B) ({ sp_Class _cx=(A),_cy=(B); sp_class_le(_cx,_cy)?sp_box_bool(1):(sp_class_le(_cy,_cx)?sp_box_bool(0):sp_box_nil()); })\n");
  buf_puts(&b, "#define sp_class_gt3(A,B) sp_class_lt3(B,A)\n");
  buf_puts(&b, "#define sp_class_ge3(A,B) sp_class_le3(B,A)\n");
  buf_puts(&b, "#define sp_class_cmp3(A,B) ({ sp_Class _cx=(A),_cy=(B); _cx.cls_id==_cy.cls_id?sp_box_int(0):(sp_class_le(_cx,_cy)?sp_box_int(-1):(sp_class_le(_cy,_cx)?sp_box_int(1):sp_box_nil())); })\n");
  /* module-aware versions (replace after sp_class_ancestors is defined) */
  /* sp_class_includes_<i>: static array of included module cls_ids per class */
  /* Also update sp_class_is_ancestor to walk includes. */
  /* Build per-class includes array by scanning the AST. */
  {
    /* For each user class, collect included module ids (in include order). */
    int **cls_incs = calloc((size_t)c->nclasses, sizeof(int *));
    int  *cls_nincs = calloc((size_t)c->nclasses, sizeof(int));
    /* `prepend M` puts M BEFORE the class in #ancestors, so it is collected
       separately from the includes that follow the class (#2702). */
    int **cls_preps = calloc((size_t)c->nclasses, sizeof(int *));
    int  *cls_npreps = calloc((size_t)c->nclasses, sizeof(int));
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
          if (!nm2 || (!sp_streq(nm2, "include") && !sp_streq(nm2, "prepend"))) continue;
          int is_prep2 = sp_streq(nm2, "prepend");
          int **tgt_mods = is_prep2 ? cls_preps : cls_incs;
          int  *tgt_n    = is_prep2 ? cls_npreps : cls_nincs;
          if (nt_ref(c->nt, stmts2[k2], "receiver") >= 0) continue;
          int anode2 = nt_ref(c->nt, stmts2[k2], "arguments");
          int an2 = 0;
          const int *aargs = anode2 >= 0 ? nt_arr(c->nt, anode2, "arguments", &an2) : NULL;
          for (int j2 = 0; j2 < an2; j2++) {
            const char *aty2 = nt_type(c->nt, aargs[j2]);
            const char *mname2 = (aty2 && sp_streq(aty2, "ConstantReadNode")) ? nt_str(c->nt, aargs[j2], "name") : NULL;
            if (!mname2 && aty2 && sp_streq(aty2, "ConstantPathNode")) mname2 = nt_str(c->nt, aargs[j2], "name");
            int mid2 = mname2 ? comp_class_index(c, mname2) : -1;
            int is_builtin_mod = 0;
            /* a builtin module (Enumerable/Comparable/Kernel/Math) has no user
               class index; record its (negative) builtin id so ancestors
               reflects it -- the generic `mid2 < 0` skip must not drop it. */
            if (mid2 < 0 && mname2) {
              if (sp_streq(mname2, "Enumerable")) { mid2 = -115; is_builtin_mod = 1; }
              else if (sp_streq(mname2, "Comparable")) { mid2 = -114; is_builtin_mod = 1; }
              else if (sp_streq(mname2, "Kernel")) { mid2 = -119; is_builtin_mod = 1; }
              else if (sp_streq(mname2, "Math")) { mid2 = -130; is_builtin_mod = 1; }
            }
            if (mid2 < 0 && !is_builtin_mod) continue;
            /* deduplicate */
            int found2 = 0;
            for (int q = 0; q < tgt_n[ci]; q++) if (tgt_mods[ci][q] == mid2) { found2 = 1; break; }
            if (found2) continue;
            tgt_mods[ci] = realloc(tgt_mods[ci], sizeof(int) * (size_t)(tgt_n[ci] + 1));
            tgt_mods[ci][tgt_n[ci]++] = mid2;
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
    /* a builtin Module (Comparable/Enumerable/Kernel/Math) has no superclass
       chain: its ancestors are just itself (#2285). */
    buf_puts(&b, "      if(cur.cls_id==-114||cur.cls_id==-115||cur.cls_id==-119||cur.cls_id==-130){\n");
    buf_puts(&b, "        sp_PolyArray_push(a,sp_box_class(cur)); break;\n      }\n");
    buf_puts(&b, "      while(1){\n");
    buf_puts(&b, "        sp_PolyArray_push(a,sp_box_class(cur));\n");
    /* Numeric includes Comparable; Array/Hash include Enumerable; String includes Comparable */
    buf_puts(&b, "        if(cur.cls_id==-113) sp_PolyArray_push(a,sp_box_class(((sp_Class){-114})));\n");  /* Numeric->Comparable */
    buf_puts(&b, "        if(cur.cls_id==-104||cur.cls_id==-105||cur.cls_id==-106||cur.cls_id==-144||cur.cls_id==-145) sp_PolyArray_push(a,sp_box_class(((sp_Class){-115})));\n");  /* Array/Hash/Range/Enumerator/Struct->Enumerable */
    buf_puts(&b, "        if(cur.cls_id==-102||cur.cls_id==-103) sp_PolyArray_push(a,sp_box_class(((sp_Class){-114})));\n");  /* String/Symbol->Comparable */
    buf_puts(&b, "        if(cur.cls_id==-116) sp_PolyArray_push(a,sp_box_class(((sp_Class){-119})));\n");  /* Object->Kernel */
    buf_puts(&b, "        sp_Class bn=sp_builtin_superclass(cur);\n");
    /* the root (BasicObject) yields the nil class: that terminates the walk.
       Chain end used to be marked by a self-reference, so keep that check too. */
    buf_puts(&b, "        if(sp_class_nil_p(bn)||bn.cls_id==cur.cls_id)break;\n");
    buf_puts(&b, "        cur=bn;\n");
    buf_puts(&b, "      }\n");
    buf_puts(&b, "      break;\n    }\n");
    /* prepended modules come BEFORE the class itself (#2702) */
    {
      int any_prep = 0;
      for (int ci = 0; ci < c->nclasses; ci++) if (cls_npreps[ci]) { any_prep = 1; break; }
      if (any_prep) {
        buf_puts(&b, "    switch(cur.cls_id){\n");
        for (int ci = 0; ci < c->nclasses; ci++) {
          if (cls_npreps[ci] == 0) continue;
          buf_printf(&b, "    case %d:", ci);
          /* last prepend wins, so it lands closest to the front */
          for (int q = cls_npreps[ci] - 1; q >= 0; q--)
            buf_printf(&b, " sp_PolyArray_push(a,sp_box_class(((sp_Class){%d})));", cls_preps[ci][q]);
          buf_puts(&b, " break;\n");
        }
        buf_puts(&b, "    }\n");
      }
    }
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
    /* Module#included_modules: the ancestors that are modules (#2674). The
       ancestors are id-backed boxes (sp_box_class of a name-less sp_Class), so
       the cls_id rides the int slot. */
    buf_puts(&b, "static sp_PolyArray *sp_class_included_modules(sp_Class c) __attribute__((unused));\n");
    buf_puts(&b, "static sp_PolyArray *sp_class_included_modules(sp_Class c){\n");
    buf_puts(&b, "  sp_PolyArray *a=sp_class_ancestors(c); SP_GC_ROOT(a);\n");
    buf_puts(&b, "  sp_PolyArray *r=sp_PolyArray_new(); SP_GC_ROOT(r);\n");
    buf_puts(&b, "  for(mrb_int i=0;a&&i<a->len;i++){ sp_Class m={a->data[i].v.i,NULL};\n");
    buf_puts(&b, "    if(sp_class_is_module_val(m)) sp_PolyArray_push(r,a->data[i]); }\n");
    buf_puts(&b, "  return r;\n}\n\n");
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
      "    if(v.cls_id>=-20||v.cls_id==-34)return ((sp_Class){-105});\n"  /* hashes */
      "    return ((sp_Class){-116});\n"
      "  default: return ((sp_Class){-116});\n"
      "  }\n}\n"
      "static int sp_poly_is_a(sp_RbVal obj,sp_Class klass){\n"
      "  return sp_class_le(sp_poly_get_class(obj),klass);\n}\n");
    for (int ci = 0; ci < c->nclasses; ci++) { free(cls_incs[ci]); free(cls_preps[ci]); }
    free(cls_incs); free(cls_nincs); free(cls_preps); free(cls_npreps);
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
        /* snapshot: class_ruby_name returns a shared static buffer for nested
           names, and the parent canonicalization below calls it again. Copy to
           the heap, not a fixed buffer: the top-level path returns the class's
           own arbitrary-length name, which a fixed size would truncate out of
           agreement with the constructor emission. An unnamed entry has
           nothing to match on. */
        const char *cn0 = class_ruby_name(c, i);
        if (!cn0) cn0 = c->classes[i].name;
        if (!cn0) continue;
        char *cn = strdup(cn0);
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
        /* canonicalize a user parent to its qualified Ruby name so the
           hierarchy walk meets the raised / rescue-arm names (both emitted
           qualified); a builtin parent keeps its runtime name */
        if (par && !is_exc_name(par)) {
          int pci = comp_class_index(c, par);
          if (pci >= 0) {
            const char *pqn = class_ruby_name(c, pci);
            if (pqn) par = pqn;
          }
        }
        if (par) {
          buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return \"%s\";\n", cn, par);
          /* also register the leaf name if different from qualified name */
          if (c->classes[i].name && !sp_streq(cn, c->classes[i].name))
            buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return \"%s\";\n", c->classes[i].name, par);
        }
        free(cn);
      }
    }
    buf_puts(&b, "  return 0;\n}\n");
  }
  }  /* if (g_needs_class_machinery) */

  /* class structs + GC scan functions. Forward-declare every typedef first so
     a class struct may embed a pointer to a class defined later. */
  for (int i = 0; i < c->nclasses; i++) {
    if (is_builtin_reopen(c->classes[i].name)) continue;
    if (c->classes[i].is_native_class) continue;  /* forward-declared with the native externs */
    if (class_is_exc_subclass(c, i) && c->classes[i].nivars == 0)
      buf_printf(&b, "typedef sp_Exception sp_%s;\n", c->classes[i].c_name);
    else
      buf_printf(&b, "typedef struct sp_%s_s sp_%s;\n", c->classes[i].c_name, c->classes[i].c_name);
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

  /* default-inspect dispatch prototype: bodies call it, the switch itself is
     emitted with the marshal dispatch near the end of the TU. Only user-class
     instances route through it, so a classless program emits neither. */
  if (g_emit_obj_dispatch) {
    buf_puts(&b, "static const char *sp_obj_inspect_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static const char *sp_obj_to_s_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
  }
  /* method prototypes (scope 0 is top-level) */
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s) || c->scopes[s].is_transplanted_source) continue; emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }

  /* User exception #message / #to_s overrides: a cls_name-keyed dispatcher so
     the default message path yields the user-overridden text. Ruby's #message
     calls #to_s, so #to_s uses a user #to_s if defined else the stored message,
     and #message uses a user #message, else a user #to_s, else the stored
     message. Emitted after the method prototypes it calls. Marked unused: a
     program can define an override yet never query it, and the call sites only
     reference these when a query is compiled. */
  if (exc_has_user_msg_override(c)) {
    for (int pass = 0; pass < 2; pass++) {
      int want_message = pass;  /* 0 = to_s dispatcher, 1 = message dispatcher */
      buf_printf(&b, "__attribute__((unused)) static const char *%s(sp_Exception *e){\n",
                 want_message ? "sp_user_exc_message" : "sp_user_exc_to_s");
      buf_puts(&b, "  if(!e)return (&(\"\\xff\")[1]);\n  const char *cls=e->cls_name;\n");
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i)) continue;
        int dmsg = -1, dtos = -1;
        int mi_msg = comp_method_in_chain(c, i, "message", &dmsg);
        int mi_tos = comp_method_in_chain(c, i, "to_s", &dtos);
        int mi = -1, dcls = -1;
        const char *fn = NULL;
        if (want_message) {
          if (mi_msg >= 0)      { mi = mi_msg; dcls = dmsg; fn = "message"; }
          else if (mi_tos >= 0) { mi = mi_tos; dcls = dtos; fn = "to_s"; }
        } else if (mi_tos >= 0) { mi = mi_tos; dcls = dtos; fn = "to_s"; }
        if (mi < 0) continue;
        if ((TyKind)c->scopes[mi].ret != TY_STRING) continue;  /* string-returning only */
        const char *dcn = c->classes[dcls].c_name;
        const char *cn0 = class_ruby_name(c, i);
        if (!cn0) cn0 = c->classes[i].name;
        if (!cn0) continue;
        buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return (const char*)sp_%s_%s((sp_%s*)e);\n",
                   cn0, dcn, fn, dcn);
        if (c->classes[i].name && !sp_streq(cn0, c->classes[i].name))
          buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return (const char*)sp_%s_%s((sp_%s*)e);\n",
                     c->classes[i].name, dcn, fn, dcn);
      }
      buf_puts(&b, "  return sp_exc_message(e);\n}\n");
    }
  }
  /* constructor prototypes + definitions (after method protos: new calls initialize) */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (is_builtin_reopen(ci->name)) continue;
    if (ci->is_native_class) continue;  /* constructor lives in the package */
    if (ci->is_struct) {
      /* struct constructor takes typed member params -- the prototype must
         match the definition (an empty () prototype + a _Bool param differ) */
      int scust = comp_method_in_chain(c, i, "initialize", NULL);
      /* a custom initialize -- yielding or not -- gives sp_X_new the init's own
         param signature (a yielding one is run inlined; see emit_class_new). */
      int has_custom = scust >= 0 && c->scopes[scust].reachable;
      buf_printf(&b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
      if (has_custom) {
        /* custom initialize: the .new params are its params, not one-per-member */
        Scope *s = &c->scopes[scust];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          LocalVar *p = scope_local(s, s->pnames[m]);
          TyKind pm = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
          emit_ctype(c, pm, &b);
        }
        if (s->nparams == 0) buf_puts(&b, "void");
      }
      else {
        for (int m = 0; m < ci->nivars; m++) { if (m) buf_puts(&b, ", "); emit_ctype(c, ci->ivar_types[m], &b); }
        if (ci->nivars == 0) buf_puts(&b, "void");
      }
      buf_puts(&b, ");\n");
      /* forward-declare the generated stringifiers so one struct's #inspect may
         recurse into a struct-typed member regardless of definition order */
      if (comp_method_in_chain(c, i, "inspect", NULL) < 0)
        buf_printf(&b, "static const char *sp_%s_inspect(sp_%s *self);\n", ci->c_name, ci->c_name);
      if (comp_method_in_chain(c, i, "to_s", NULL) < 0)
        buf_printf(&b, "static const char *sp_%s_to_s(sp_%s *self);\n", ci->c_name, ci->c_name);
    }
    else {
      int icid = i;
      int init = comp_method_in_chain(c, i, "initialize", &icid);
      const char *star = ci->is_value_type ? "" : "*";
      int p_has_blk = init >= 0 && c->scopes[init].blk_param &&
                      c->scopes[init].blk_param[0] && !c->scopes[init].yields;
      if (init >= 0 && (c->scopes[init].nparams > 0 || p_has_blk)) {
        buf_printf(&b, "static sp_%s %ssp_%s_new(", ci->c_name, star, ci->c_name);
        Scope *s = &c->scopes[init];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          LocalVar *p = scope_local(s, s->pnames[m]);
          TyKind pm = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
          emit_ctype(c, pm, &b);
        }
        if (p_has_blk) { if (s->nparams > 0) buf_puts(&b, ", "); buf_puts(&b, "sp_Proc *"); }
        buf_puts(&b, ");\n");
      }
      else buf_printf(&b, "static sp_%s %ssp_%s_new(void);\n", ci->c_name, star, ci->c_name);
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
  g_gen_obj_hashkey = 0;
  for (int k = 0; k < c->nclasses; k++)
    if (class_is_hashkey(c, k) || class_is_valuekey(c, k)) { g_gen_obj_hashkey = 1; break; }
  g_gen_obj_valeq = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    /* Struct/Data auto field-wise == (no user override). */
    if ((c->classes[k].is_struct || c->classes[k].is_data) &&
        comp_method_in_chain(c, k, "==", NULL) < 0) { g_gen_obj_valeq = 1; break; }
    /* A reachable, bool-returning user == is dispatched so include?/index/uniq
       honor it (#2884). */
    int mi = comp_method_in_chain(c, k, "==", NULL);
    if (mi >= 0 && c->scopes[mi].reachable && c->scopes[mi].ret == TY_BOOL) { g_gen_obj_valeq = 1; break; }
  }

  /* sp_re_init is worth emitting only if it would set at least one hook. */
  g_re_init_needed = g_uses_symbols || g_uses_marshal || g_uses_regex || g_needs_class_machinery ||
                     g_has_user_global_marks || g_has_user_cmp || g_gen_obj_hash || g_gen_obj_to_h || g_gen_obj_with || g_gen_obj_hashkey ||
                     g_gen_obj_valeq;

  /* Constructor defs, method defs, and main go into a separate buffer. Any
     proc literals they contain accumulate static functions into g_procs /
     g_proc_protos; we splice those in ahead of these bodies, since a proc
     function must be declared before the body that references it. */
  Buf *body = (Buf *)calloc(1, sizeof *body);  /* heap: must survive a collect-mode longjmp (see EMIT_COLLECT_UNIT) */
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      EMIT_COLLECT_UNIT(emit_class_new(c, &c->classes[i], body));
  /* user-object Marshal dispatchers (after every class struct + pool define) */
  if (g_uses_marshal) emit_marshal_dispatch(c, body);
  if (g_emit_obj_dispatch) emit_obj_inspect_dispatch(c, body);
  emit_obj_to_hash_dispatch(c, body);
  emit_obj_to_h_dispatch(c, body);
  emit_obj_with_dispatch(c, body);
  for (int s = 1; s < c->nscopes; s++) {
    if (c->scopes[s].yields || !c->scopes[s].reachable || scope_is_shadowed(c, s) || c->scopes[s].is_transplanted_source) continue; EMIT_COLLECT_UNIT(emit_method(c, &c->scopes[s], body));
  }
  /* Comparable cmp-hook dispatcher (after the user `<=>` definitions it calls). */
  if (g_has_user_cmp) emit_obj_cmp_dispatch(c, body);
  /* Struct/Data value-== hook (after the class struct definitions); emitted
     before the hash-key dispatch, which references sp_obj_eq_dispatch. */
  if (g_gen_obj_valeq) emit_obj_valeq_dispatch(c, body);
  /* Hash-key hooks (after the user #hash/#eql? definitions they call). */
  if (g_gen_obj_hashkey) emit_obj_hashkey_dispatch(c, body);

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
  if (g_uses_program_name)
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
  /* No PRNG seeding here: the shared Kernel stream (lib/sp_random.c)
     self-seeds lazily on its first draw, so a program that consumes no
     randomness pays nothing and one that does still varies per run. */
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
  g_needs_proc_poly_argslot = 0;

  comp_free(c);
  return b.p;
}

