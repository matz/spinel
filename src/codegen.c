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
    case TY_ADDRINFO:   return "SP_BUILTIN_ADDRINFO";
    case TY_SOCKOPT:    return "SP_BUILTIN_SOCKOPT";
    case TY_OPENSTRUCT: return "SP_BUILTIN_OPENSTRUCT";
    case TY_MATCHDATA:  return "SP_BUILTIN_MATCHDATA";
    /* A compiled pattern is a heap pointer like the rest: without a boxed
       identity a Regexp reaching a boxed slot -- an array element, a proc
       argument -- was evaluated for effect and answered nil (#3950). */
    case TY_REGEX:      return "SP_BUILTIN_REGEX";
    case TY_CURRY:      return "SP_BUILTIN_CURRY";
    /* TY_TMS is an unboxed VALUE type: it boxes by heap copy (sp_box_tms),
       never as a nullable pointer (#3132) */
    default:            return NULL;
  }
}

/* 1 iff some class inherits from `ocid`. A class with subclasses is only the
   STATIC type at a boxing site: an inherited method boxing `self`, or a base
   -typed slot holding a subclass instance, would stamp the base's id and the
   value then dispatched as the base (#3773, #4023). The object carries its own
   id in its first field, so the _dyn box reads that instead. */
int class_has_subclass(Compiler *c, int ocid) {
  for (int k = 0; k < c->nclasses; k++)
    if (k != ocid && c->classes[k].parent == ocid) return 1;
  return 0;
}

const char *g_ext_init_name = NULL;
const char *g_ext_entries = NULL;
const char *g_ext_target = NULL;       /* --ext cruby: generate the host shim */
const char *g_ext_feature = NULL;      /* Init_<feature> / require name (from -o) */
char *g_ext_header_text = NULL;
char *g_ext_shim_text = NULL;

void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  /* An untyped or void value is already emitted as a boxed sp_RbVal (a nil
     sentinel or poly call result) -- or carries side effects to preserve. Box
     it by evaluating for effect and yielding nil, mirroring emit_boxed. Without
     this, the int-boxing fallback below produced sp_box_int(<sp_RbVal>) -- an
     sp_int slot fed a boxed value (the recurring poly-box bug family). */
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
    else
      buf_printf(b, "sp_box_nullable_obj%s((void *)(%s), %d)",
                 class_has_subclass(c, ty_object_class(t)) ? "_dyn" : "",
                 expr, ty_object_class(t));
    return;
  }
  /* A hash slot is a nilable C pointer for the same reason the object arm
     above is: an omitted optional parameter, or any slot that can hold nil,
     arrives as NULL. sp_box_obj wrapped that NULL in a truthy SP_TAG_OBJ, so
     `initheader.nil?` answered false and the first read of it dereferenced
     NULL. Reduced from a Net::HTTP::Post whose second parameter one call site
     omitted and another passed a Hash to (#4134). */
  if (ty_is_hash(t) && hash_box_cls(t)) {
    buf_printf(b, "sp_box_nullable_obj((void *)(%s), %s)", expr, hash_box_cls(t));
    return;
  }
  /* a shared-mutable string HANDLE (#3227 phase 3) */
  if (t == TY_STRBUF) { buf_printf(b, "sp_box_obj(%s, SP_BUILTIN_STRBUF)", expr); return; }
  /* An sp_int slot can hold the nil sentinel a nullable read left behind
     (`"a".rindex("/")`), so boxing it has to yield nil rather than a boxed
     INTPTR_MIN that then answers every Integer method. The temp keeps
     a side-effecting expr evaluated once. */
  if (t == TY_INT) {
    int tb = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = (%s); _t%d == SP_INT_NIL ? sp_box_nil() : sp_box_int(_t%d); })",
               tb, expr, tb, tb);
    return;
  }
  /* A bigint slot's nil is NULL (nil_value), and sp_box_bigint would wrap it
     as a truthy Integer that prints 0: box it as nil, the way the nullable
     pointer types above do (#4800). */
  if (t == TY_BIGINT) { buf_printf(b, "sp_box_bigint_or_nil(%s)", expr); return; }
  const char *fn = NULL;
  switch (t) {
    case TY_FLOAT: fn = "sp_box_float"; break;
    case TY_BIGINT: fn = "sp_box_bigint"; break;
    case TY_STRING: fn = "sp_box_str"; break;     case TY_BOOL: fn = "sp_box_bool"; break;
    case TY_SYMBOL: fn = "sp_box_sym"; break;     case TY_RANGE: fn = "sp_box_range"; break;
    case TY_FLOAT_RANGE: fn = "sp_box_frange"; break;
    case TY_STR_RANGE: fn = "sp_box_srange"; break;
    case TY_TMS: fn = "sp_box_tms"; break;
    case TY_TIME: fn = "sp_box_time"; break;
    case TY_COMPLEX: fn = "sp_box_complex"; break;  case TY_RATIONAL: fn = "sp_box_rational"; break;
    /* TY_PROC / TY_METHOD are handled by the nullable-builtin box above. */
    case TY_CLASS: fn = "sp_box_class"; break;
    /* Array slots are nilable C pointers (`[x] if cond` in value position is
       NULL on the else path): box NULL as a proper nil, not a truthy OBJ
       wrapping NULL that passes truthy checks (#2992). */
    case TY_INT_ARRAY:   buf_printf(b, "sp_box_nullable_obj((void *)(%s), SP_BUILTIN_INT_ARRAY)", expr); return;
    case TY_FLOAT_ARRAY: buf_printf(b, "sp_box_nullable_obj((void *)(%s), SP_BUILTIN_FLT_ARRAY)", expr); return;
    case TY_STR_ARRAY:   buf_printf(b, "sp_box_nullable_obj((void *)(%s), SP_BUILTIN_STR_ARRAY)", expr); return;
    case TY_POLY_ARRAY:  buf_printf(b, "sp_box_nullable_obj((void *)(%s), SP_BUILTIN_POLY_ARRAY)", expr); return;
    case TY_OPENSTRUCT:  buf_printf(b, "sp_box_nullable_obj((void *)(%s), SP_BUILTIN_OPENSTRUCT)", expr); return;
    /* a nested table or an object array boxes by reference, stamped with what
       its elements are (#4486) */
    case TY_INT_ARRAY_ARRAY: case TY_FLOAT_ARRAY_ARRAY:
      buf_printf(b, "sp_box_ptr_array_k((void *)(%s), %s)", expr, ptr_array_stamp(c, t)); return;
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
    if (ty_is_obj_array(t)) { buf_printf(b, "sp_box_ptr_array_k((void *)(%s), %s)", expr, ptr_array_stamp(c, t)); return; }
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
    /* not a bare `.v.s`: a mutable String's box holds its handle there */
    case TY_STRING: buf_printf(b, "sp_poly_unbox_s(%s)", expr); return;
    case TY_BOOL:   buf_printf(b, "(%s).v.b", expr); return;
    case TY_SYMBOL: buf_printf(b, "(sp_sym)(%s).v.i", expr); return;
    /* NOT the bare `.v.p` cast: a poly slot holds a small Integer inline
       (SP_TAG_INT), and under promote mode that is the common case -- the
       cast then read the integer itself as an sp_Bigint pointer and the
       program segfaulted on the first use (`(x >= 0) ? x : (x & M64)`,
       whose two arms unify on Bignum, #4590). sp_poly_as_bigint hands an
       already-Bignum value straight back and converts the rest, the same
       shape the STRBUF line below uses. */
    case TY_BIGINT: buf_printf(b, "sp_poly_as_bigint(%s)", expr); return;
    case TY_STRBUF: buf_printf(b, "sp_poly_as_strbuf(%s)", expr); return;
    default: break;
  }
  if (t == TY_TIME) { buf_printf(b, "(*(sp_Time *)(%s).v.p)", expr); return; }  /* boxed by-value copy */
  /* Rational / Complex are by-value structs boxed behind a pointer, so unbox by
     dereferencing -- not the pointer-cast the generic arm below would emit,
     which casts a pointer straight to a struct (#3186). */
  if (t == TY_RATIONAL) { buf_printf(b, "(*(sp_Rational *)(%s).v.p)", expr); return; }
  if (t == TY_COMPLEX)  { buf_printf(b, "(*(sp_Complex *)(%s).v.p)", expr); return; }
  /* the Range value types are boxed behind a pointer too, so they unbox by
     dereferencing rather than by the pointer cast below (#3619) */
  if (t == TY_RANGE)       { buf_printf(b, "(*(sp_Range *)(%s).v.p)", expr); return; }
  if (t == TY_FLOAT_RANGE) { buf_printf(b, "(*(sp_FloatRange *)(%s).v.p)", expr); return; }
  if (t == TY_STR_RANGE)   { buf_printf(b, "(*(sp_StrRange *)(%s).v.p)", expr); return; }
  if (t == TY_CLASS) { buf_printf(b, "sp_unbox_class(%s)", expr); return; }  /* a by-value struct, not a pointer (#2797) */
  /* A hash variant is a distinct C struct, so a boxed hash of ANOTHER variant
     read through a pointer cast keeps its keys and reads its values as another
     type's zero -- silently (#3998). Go through the converting entry, which
     hands back the pointer itself when the variant already matches. */
  /* The array kinds are separate C structs too, and had no converting entry:
     a boxed PolyArray read through the pointer cast below became an IntArray's
     header, and the caller printed memory. Same rule as the hash variants
     below it, arriving late (#4424). */
  if (t == TY_INT_ARRAY)   { buf_printf(b, "sp_poly_as_int_array(%s)", expr); return; }
  if (t == TY_FLOAT_ARRAY) { buf_printf(b, "sp_poly_as_float_array(%s)", expr); return; }
  if (t == TY_STR_ARRAY)   { buf_printf(b, "sp_poly_as_str_array(%s)", expr); return; }
  /* a pointer array of this kind is handed back itself; any other array is
     copied element by element, each checked against the kind (#4486) */
  if (ty_is_ptr_array(t))  { buf_printf(b, "sp_poly_as_ptr_array(%s, %s)", expr, ptr_array_stamp(c, t)); return; }
  if (t == TY_STR_POLY_HASH)  { buf_printf(b, "sp_poly_as_str_poly_hash(%s)", expr); return; }
  if (t == TY_SYM_POLY_HASH)  { buf_printf(b, "sp_poly_as_sym_poly_hash(%s)", expr); return; }
  if (t == TY_POLY_POLY_HASH) { buf_printf(b, "sp_poly_as_poly_poly_hash(%s)", expr); return; }
  /* A boxed value read as a class-typed pointer: the tag and class are
     checked, because the alternative is reading another type's memory through
     the cast. A poly ivar that held an Array on one path and a Relation on
     another was passed to a parameter inference had left at Relation, and the
     Array's header was read as a table name (#4437). nil stays NULL, which is
     how an object pointer encodes it. */
  if (ty_is_object(t)) {
    int oc = ty_object_class(t);
    const char *rn = class_ruby_name(c, oc);
    buf_printf(b, "(%s *)sp_poly_unbox_cls(%s, %d, ", class_ctype(c, oc), expr, oc);
    emit_str_literal(b, rn ? rn : c->classes[oc].name);
    buf_puts(b, ")");
    return;
  }
  const char *cn = c_type_name(t);
  if (cn) buf_printf(b, "(%s)(%s).v.p", cn, expr);
  else buf_printf(b, "(%s).v.i", expr);
}

/* Emit `expr` (a poly value that MAY be nil) unboxed into a slot of type `t`,
   keeping nil as the slot's own nil rather than as the union payload sitting
   under the nil tag. `.v.i` / `.v.f` on a boxed nil read 0 / 0.0 -- ordinary
   values -- so the plain unbox turns nil into zero, silently: the C is
   well-formed and nothing downstream can tell the two apart (#3412).

   Only int and float need the guard. A pointer-backed slot takes NULL from the
   zeroed payload, which IS its nil; bool and symbol have no nil inhabitant at
   all, so a slot that must hold nil is never given those types (see
   parse_seed_type). Use this wherever a poly whose nil-ness is not already
   ruled out is narrowed to a concrete slot; emit_unbox_text stays the
   unguarded form for the many sites that have. */
void emit_unbox_nilable_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_INT)   { buf_printf(b, "sp_poly_as_int_or_nil(%s)", expr); return; }
  if (t == TY_FLOAT) { buf_printf(b, "sp_poly_as_float_or_nil(%s)", expr); return; }
  emit_unbox_text(c, t, expr, b);
}

/* True when an array of kind `vt` is stored into a slot of another scalar
   array kind (Int, Float or String elements). Inference widens such a slot
   unless an --rbs seed pins it, so this is where a seeded slot meets an array
   of the wrong kind; the store converts through emit_unbox_text. */
int seeded_array_kind_mismatch(TyKind slot, TyKind vt) {
  if (slot != TY_INT_ARRAY && slot != TY_FLOAT_ARRAY && slot != TY_STR_ARRAY) return 0;
  return vt != slot && (vt == TY_POLY_ARRAY || vt == TY_INT_ARRAY ||
                        vt == TY_FLOAT_ARRAY || vt == TY_STR_ARRAY);
}

/* Emit `v` for a store into a slot of array kind `slot`, converting it when
   seeded_array_kind_mismatch says the kinds differ: boxed, then read back
   through the slot's converting entry (#4424). Otherwise the plain value. */
void emit_array_store_value(Compiler *c, TyKind slot, int v, Buf *b) {
  if (!seeded_array_kind_mismatch(slot, comp_ntype(c, v))) { emit_expr(c, v, b); return; }
  Buf rb; memset(&rb, 0, sizeof rb);
  emit_boxed(c, v, &rb);
  emit_unbox_text(c, slot, rb.p ? rb.p : "sp_box_nil()", b);
  free(rb.p);
}

/* Wrap a boxed expression in the --rbs seed assertion before it is narrowed
   into a seeded slot. Emits the plain expression for a slot with no tag of its
   own (poly, or a by-value type), and for every slot when the program is built
   without -DSP_RBS_CHECK the macro itself collapses to the expression -- so
   this is free to emit unconditionally and is only about WHERE a seed's truth
   is checkable at all: the moment a dynamic value becomes a static type. */
void emit_rbs_checked_text(Compiler *c, TyKind slot, const char *slotname,
                           const char *expr, Buf *b) {
  const char *tag = NULL, *want = NULL;
  switch (slot) {
    case TY_INT:    tag = "SP_TAG_INT"; want = "Integer"; break;
    case TY_FLOAT:  tag = "SP_TAG_FLT"; want = "Float";   break;
    case TY_STRING: tag = "SP_TAG_STR"; want = "String";  break;
    case TY_SYMBOL: tag = "SP_TAG_SYM"; want = "Symbol";  break;
    case TY_BOOL:   tag = "SP_TAG_BOOL"; want = "a boolean"; break;
    default:
      if (ty_is_object(slot) && !comp_ty_value_obj(c, slot)) {
        tag = "SP_TAG_OBJ";
        want = c->classes[ty_object_class(slot)].name;
      }
      else if (ty_is_array(slot) || ty_is_hash(slot)) {
        tag = "SP_TAG_OBJ";
        want = ty_is_array(slot) ? "an Array" : "a Hash";
      }
      break;
  }
  if (!tag) { buf_puts(b, expr); return; }
  buf_printf(b, "SP_RBS_CHECK_TAG(%s, %s, \"", expr, tag);
  for (const char *p2 = slotname ? slotname : "?"; *p2; p2++) {
    if (*p2 == '"' || *p2 == '\\') buf_puts(b, "\\");
    buf_printf(b, "%c", *p2);
  }
  buf_printf(b, "\", \"%s\")", want ? want : "?");
}

/* An unresolved constant read lowers to a runtime NameError raise whose C
   value is an sp_Class struct (the class-position shape). In a scalar slot
   that struct fails the C compile ("incompatible types"), so the scalar
   emitters keep the raise, void the struct, and yield a typed zero -- dead
   code, the raise fires first. Text-matched on the gate's own token, like
   emit_str_expr's sp_raise_nomethod coerce (the node stays TY_UNKNOWN). */
static int coerce_const_raise(const char *txt, const char *zero, Buf *b) {
  /* Anything that DIVERGES: every sp_raise_ helper is SP_NORETURN, so an
     operand whose emission leads with one never yields its value -- discard
     it and answer the slot's own zero, keeping the call for its raise. The
     same token test emit_str_expr_ex settled on: this used to name two
     specific sp_raise_cls shapes (#3330), and the shape it did not name --
     the poly nomethod gate's bare sp_raise_nomethod(...), an sp_RbVal --
     landed raw in sp_int_sub's int slot and the C did not build
     (`io.stat.size - io.pos` on a boxed handle, #4563). */
  if (strncmp(past_open_parens(txt), "sp_raise_", 9) != 0) return 0;
  buf_printf(b, "((void)(%s), %s)", txt, zero);
  return 1;
}

/* Emit a node as an sp_int, coercing a poly value through sp_poly_to_i. Used
   where the runtime ABI demands a raw integer (array indices, etc.) but the
   expression's static type widened to poly. */
/* The value of a `yield` is the block's, and the block differs per call site:
   comp_ntype answers the union over every site. An emitter that unboxes has to
   ask the block being spliced right here, or a site whose block returns a
   scalar gets an unbox over a value that is already one -- ill-typed C, not
   even a silent wrong answer. Falls back to the union where there is no
   literal block (the proc ABI) or the tail's own type does not describe the
   emitted value (a control-flow tail can diverge or carry a `next`). */
TyKind yield_site_type(Compiler *c, int node) {
  TyKind u = comp_ntype(c, node);
  const NodeTable *nt = c->nt;
  if (node < 0 || nt_kind(nt, node) != NK_YieldNode || g_block_id < 0) return u;
  int body = nt_ref(nt, g_block_id, "body");
  if (body < 0 || nt_kind(nt, body) != NK_StatementsNode) return u;
  int n = 0; const int *st = nt_arr(nt, body, "body", &n);
  if (!st || n <= 0) return u;
  switch (nt_kind(nt, st[n - 1])) {
    case NK_IfNode: case NK_UnlessNode: case NK_CaseNode: case NK_CaseMatchNode:
    case NK_BeginNode: case NK_NextNode: case NK_BreakNode: case NK_ReturnNode:
    case NK_RescueModifierNode: case NK_StatementsNode:
      return u;
    default: break;
  }
  TyKind t = comp_ntype(c, st[n - 1]);
  return (t == TY_UNKNOWN || t == TY_VOID || t == TY_NIL) ? u : t;
}

/* A user object in a concretely-typed slot: CRuby's implicit conversion
   protocol. The argument's class is static here, so a class defining the
   conversion method (#to_str for a const char* slot, #to_int for sp_int)
   converts through a DIRECT call to the compiled method; a class without it
   is CRuby's TypeError ("no implicit conversion of X into Y") -- where the
   raw object pointer previously went into the scalar slot and stopped the C
   build. Handles only a conversion method taking no parameters whose static
   return type is the slot's type; anything else keeps the prior behavior.
   Returns 1 when it emitted, 0 to fall through. */
int obj_conv_method(Compiler *c, TyKind t, const char *conv, TyKind want, int *def_out) {
  if (!ty_is_object(t)) return -1;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return -1;
  int def = -1, mi = comp_method_in_chain(c, cid, conv, &def);
  if (mi < 0) return -1;
  Scope *um = &c->scopes[mi];
  /* only a no-parameter method whose static return IS the slot's type
     converts -- or one whose answer is BOXED, which is the same conversion
     described the way the analysis happens to describe it (a #to_int
     returning a plain `1` is poly under --int-overflow=promote). The answer
     is judged where it is used, exactly as the runtime bridge judges its
     own; refusing it here instead left the raw object pointer in the scalar
     slot and the generated C did not compile. Any other shape -- a #to_int
     answering a String -- is not this protocol. */
  if ((um->ret != want && um->ret != TY_POLY) || um->nparams != 0) return -1;
  if (def_out) *def_out = def;
  return mi;
}

/* The same protocol read STRICTLY: only a conversion whose static return is
   the slot's own type. Two emitters need that narrower question, because a
   boxed answer is not a fall-through for them but an arm they already have.
   emit_str_cmp_conv routes it to sp_str_cmp_conv -- which is what raises the
   TypeError naming the class -- and the #to_path site wraps the call in
   sp_poly_arg_str_chk itself. Asking the widened question there took both
   arms away: the comparison assigned an sp_RbVal to a `const char *` and the
   generated C stopped compiling. */
static int obj_conv_method_typed(Compiler *c, TyKind t, const char *conv, TyKind want, int *def_out) {
  int mi = obj_conv_method(c, t, conv, want, def_out);
  if (mi < 0) return -1;
  return c->scopes[mi].ret == want ? mi : -1;
}

/* The container half of the protocol: a class whose #to_ary / #to_hash is a
   no-parameter method returning a static Array (or Hash) kind converts
   through a direct call, typed as that container (Array#zip, #product,
   Hash#merge). Answers the container kind, or TY_UNKNOWN when the class has
   no such method -- the method's own return type is the answer, so there is
   nothing to search the kind space for. */
TyKind obj_container_conv(Compiler *c, TyKind t, const char *conv, int *def) {
  if (!ty_is_object(t)) return TY_UNKNOWN;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return TY_UNKNOWN;
  int d = -1, mi = comp_method_in_chain(c, cid, conv, &d);
  if (mi < 0 || c->scopes[mi].nparams != 0) return TY_UNKNOWN;
  TyKind ret = c->scopes[mi].ret;
  if (sp_streq(conv, "to_hash") ? !ty_is_hash(ret) : !ty_is_array(ret)) return TY_UNKNOWN;
  if (def) *def = d;
  return ret;
}

/* 1 iff any class in the program defines a usable #to_int / #to_str. A
   NARROWING into a typed slot compiles the conversion in only then: unlike an
   argument slot, a narrowing sits wherever the analysis put it -- including
   inside a per-pixel loop, where the object test is real work rather than a
   cold arm, and measured 6.7% more instructions on optcarrot. A program that
   defines no conversion method can never take that arm, so it pays nothing. */
int prog_has_conv_method(Compiler *c, const char *conv, TyKind want) {
  for (int i = 0; i < c->nclasses; i++)
    if (obj_conv_method(c, ty_object(i), conv, want, NULL) >= 0) return 1;
  return 0;
}

/* The direct call of a compiled conversion method on a statically-typed
   object: sp_Cls_to_str((sp_Cls *)(expr)). */
/* A conversion that answers a String -- an object's #to_path or #to_str, a
   boxed value through sp_poly_arg_path or sp_poly_to_s -- may build that
   String, and nothing holds it while a sibling operand converts or the
   callee allocates before reading it. emit_call collects every such
   conversion into a rooted temp declared in front of the call, so the
   converting emitters render `_tN` in the slot and the conversion itself
   into the hold. A conversion emitted outside a call renders inline as
   before. */
ConvHold *g_conv_hold = NULL;
unsigned g_conv_emitted = 0;
Buf *conv_hold_begin(Buf *b, int *tmp) {
  g_conv_emitted++;  /* counted before the hold test: a hold-less render converts too */
  if (!g_conv_hold) return NULL;
  if (g_conv_hold->n >= g_conv_hold->cap) {
    g_conv_hold->cap = g_conv_hold->cap ? g_conv_hold->cap * 2 : 8;
    g_conv_hold->tmp = (int *)realloc(g_conv_hold->tmp, sizeof(int) * (size_t)g_conv_hold->cap);
    if (!g_conv_hold->tmp) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  *tmp = ++g_tmp;
  g_conv_hold->tmp[g_conv_hold->n++] = *tmp;
  buf_printf(&g_conv_hold->b, "const char *_t%d = ", *tmp);
  buf_printf(b, "_t%d", *tmp);
  return &g_conv_hold->b;
}
void conv_hold_end(int tmp) {
  buf_printf(&g_conv_hold->b, "; SP_GC_ROOT(_t%d); ", tmp);
}

static void emit_obj_conv_call_inline(Compiler *c, int node, TyKind t, int def, const char *conv, Buf *b);
static void emit_obj_conv_call(Compiler *c, int node, TyKind t, int def, const char *conv, Buf *b) {
  int tmp;
  /* an Integer answer needs no root, so #to_int renders inline -- still a
     conversion for the operand-order gate's count */
  if (sp_streq(conv, "to_int")) g_conv_emitted++;
  Buf *hb = sp_streq(conv, "to_int") ? NULL : conv_hold_begin(b, &tmp);
  if (!hb) { emit_obj_conv_call_inline(c, node, t, def, conv, b); return; }
  emit_obj_conv_call_inline(c, node, t, def, conv, hb);
  conv_hold_end(tmp);
}
static void emit_obj_conv_call_inline(Compiler *c, int node, TyKind t, int def, const char *conv, Buf *b) {
  /* a boxed answer is judged, not merely read: a conversion that hands back
     something of the wrong kind is CRuby's TypeError, which is what these
     two raise -- the same judgement the bridge row makes for its own half */
  int mi_c = comp_method_in_chain(c, def, conv, NULL);
  const char *unbox = NULL;
  if (mi_c >= 0 && c->scopes[mi_c].ret == TY_POLY)
    unbox = sp_streq(conv, "to_int") ? "sp_poly_arg_int_chk("
          : sp_streq(conv, "to_str") ? "sp_poly_arg_str("
          : NULL;  /* #to_path is wrapped by its own site, not here */
  if (unbox) buf_puts(b, unbox);
  buf_printf(b, "sp_%s_%s(", c->classes[def].c_name, mc(conv));
  if (!comp_ty_value_obj(c, t)) buf_printf(b, "(sp_%s *)", c->classes[def].c_name);
  buf_puts(b, "(");
  emit_expr(c, node, b);
  buf_puts(b, "))");
  if (unbox) buf_puts(b, ")");
}

static int emit_obj_conv(Compiler *c, int node, const char *conv, TyKind want,
                         const char *into, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return 0;
  int def = -1;
  if (obj_conv_method(c, t, conv, want, &def) >= 0) {
    emit_obj_conv_call(c, node, t, def, conv, b);
    return 1;
  }
  /* The class is static and settled here, so a missing #to_str / #to_int is
     not a run-time question: the call can only ever raise. Say so at compile
     time, where the author can act on it, rather than emitting a raise -- and
     where the raw object pointer used to land in the scalar slot and stop the
     C build with a message about a generated symbol. */
  const char *cn = class_ruby_name(c, cid);
  char msg[256];
  /* The class DOES define the method, but its answer is statically of
     another kind -- `def to_int = "no"`. That call can only ever raise, the
     same way a missing method can, so it is refused here too rather than
     left to the old path, where the raw object pointer landed in the scalar
     slot and the C build stopped with a message about a generated symbol.
     CRuby names the offending kind, so this does as well. */
  { int wmi = comp_method_in_chain(c, cid, conv, NULL);
    if (wmi >= 0 && c->scopes[wmi].nparams == 0) {
      /* CRuby names the answer's CLASS, so the diagnostic does too rather
         than printing the analysis's own lowercase tag */
      TyKind wr = (TyKind)c->scopes[wmi].ret;
      const char *wn = wr == TY_STRING ? "String" : wr == TY_INT ? "Integer"
                     : wr == TY_FLOAT ? "Float" : wr == TY_BOOL ? "Boolean"
                     : wr == TY_NIL ? "NilClass" : wr == TY_SYMBOL ? "Symbol"
                     : wr == TY_RATIONAL ? "Rational" : wr == TY_BIGINT ? "Integer"
                     : ty_is_array(wr) ? "Array" : ty_is_hash(wr) ? "Hash"
                     : ty_name(wr);
      snprintf(msg, sizeof msg, "can't convert %s to %s (%s#%s gives %s)",
               cn ? cn : "Object", into, cn ? cn : "the class", conv, wn);
      unsupported_feature(c, node, msg);
    }
    if (wmi >= 0) return 0;  /* another shape entirely (parameters): old path */
  }
  snprintf(msg, sizeof msg, "no implicit conversion of %s into %s (%s defines no #%s)",
           cn ? cn : "Object", into, cn ? cn : "the class", conv);
  unsupported_feature(c, node, msg);
}

/* A value KNOWN at compile time to be nil / true / false entering a String-
   or Integer-typed builtin argument slot: CRuby raises TypeError where the
   slot's zero ("" / 0) used to stand in silently ([1].take(nil) answered [],
   File.join("a", nil) answered "a/"). The raise is a runtime one -- programs
   legitimately rescue it -- and CRuby words the nil-to-Integer pairing
   differently from all others ("from nil to integer"). The nilable entry
   points below skip this arm for the slots CRuby itself accepts nil in
   ("x".split(nil), StringIO#read(nil), File.open(path, nil), ...) -- but
   only for nil: those slots still reject true / false (ENV["k"] = false is
   CRuby's "no implicit conversion of false into String"), so the nilable
   forms stay bool-strict. */
/* The static Ruby class name of a scalar/container kind, for TypeError
   wording -- NULL for kinds a conversion arm already handles (poly, object,
   unknown) or that are legal in the slot. */
const char *conv_wrong_cls_name(TyKind t) {
  if (t == TY_INT || t == TY_BIGINT) return "Integer";
  if (t == TY_FLOAT) return "Float";
  if (t == TY_SYMBOL) return "Symbol";
  if (t == TY_STRING || t == TY_STRBUF) return "String";
  if (t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE) return "Range";
  if (t == TY_TIME) return "Time";
  if (t == TY_REGEX) return "Regexp";
  if (t == TY_PROC) return "Proc";
  if (ty_is_array(t)) return "Array";
  if (ty_is_hash(t)) return "Hash";
  return NULL;
}

/* The Ruby class name a TypeError should name for any settled static kind:
   the scalar and container names above, "nil" for nil, and a user class by
   its own name. NULL only where the kind is not settled (poly, unknown). */
const char *conv_cls_name_of(Compiler *c, TyKind t) {
  if (t == TY_NIL) return "nil";
  if (ty_is_object(t)) return class_ruby_name(c, ty_object_class(t));
  return conv_wrong_cls_name(t);
}

static int emit_nilbool_conv_raise_w(Compiler *c, int node, TyKind want, int nil_ok,
                                     int of_wording, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t != TY_NIL && t != TY_BOOL) {
    /* any other statically-known wrong kind: CRuby's class-naming TypeError
       ("no implicit conversion of Integer into String"). Every kind raised
       here previously emitted ill-typed C ([1].pack(1) stopped the build), so
       nothing working can be lost. A Float in an Integer slot converts (the
       truncation arm above); stringish kinds belong in a String slot. */
    if (want == TY_STRING && (t == TY_STRING || t == TY_STRBUF)) return 0;
    if (want == TY_INT && (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT)) return 0;
    const char *cn = conv_wrong_cls_name(t);
    if (!cn) return 0;
    buf_puts(b, "({ (void)(");
    emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"no implicit conversion of %s into %s\"); %s; })",
               cn, want == TY_STRING ? "String" : "Integer",
               want == TY_STRING ? "(const char *)0" : "(sp_int)0");
    return 1;
  }
  if (t == TY_NIL && nil_ok) return 0;
  const char *dv = want == TY_STRING ? "(const char *)0" : "(sp_int)0";
  if (t == TY_NIL) {
    buf_puts(b, "({ (void)(");
    emit_expr(c, node, b);
    /* CRuby's rb_num2long-style slots say "from nil to integer"; the
       rb_convert_type ones (Random.srand, Dir.mkdir's mode) say
       "of nil into Integer". String slots have only the one form. */
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"%s\"); %s; })",
               want == TY_STRING ? "no implicit conversion of nil into String"
               : of_wording      ? "no implicit conversion of nil into Integer"
                                 : "no implicit conversion from nil to integer",
               dv);
  }
  else {
    const char *into = want == TY_STRING ? "String" : "Integer";
    buf_puts(b, "({ sp_raise_cls(\"TypeError\", (");
    emit_expr(c, node, b);
    buf_printf(b, ") ? \"no implicit conversion of true into %s\""
                  " : \"no implicit conversion of false into %s\"); %s; })",
               into, into, dv);
  }
  return 1;
}

static void emit_int_expr_ex(Compiler *c, int node, int strict, Buf *b) {
  const char *nty = nt_type(c->nt, node);
  /* `*a` forwarded into a scalar int slot (a builtin arg): the value is the
     splat's first element, not the array box. */
  if (nty && sp_streq(nty, "SplatNode")) {
    int inner = nt_ref(c->nt, node, "expression");
    buf_puts(b, "sp_poly_to_i(sp_PolyArray_get(sp_poly_to_poly_array(sp_splat_to_array(");
    if (inner >= 0) emit_boxed(c, inner, b); else buf_puts(b, "sp_box_nil()");
    buf_puts(b, ")), 0))");
    return;
  }
  if (yield_site_type(c, node) == TY_POLY) {
    /* a boxed value may carry a user object whose #to_int runs here; only a
       program defining one makes this a conversion the order gate counts */
    if (strict && prog_has_conv_method(c, "to_int", TY_INT)) g_conv_emitted++;
    buf_puts(b, strict ? "sp_poly_arg_int_chk(" : "sp_poly_to_i(");
    emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  /* A value the analysis widened to Bignum (a doubling counter, a masked
     accumulator) used where an integer is wanted -- an array index, a repeat
     count -- is a pointer, not a number: convert it. */
  if (comp_ntype(c, node) == TY_BIGINT) {
    buf_puts(b, "sp_bigint_to_int("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  /* A Float in an sp_int slot (a mixed Range literal like `0.5..5`, which is
     deliberately carried on the integer representation) truncates; say so, or
     clang warns that the literal changes value and the build reads as broken. */
  if (comp_ntype(c, node) == TY_FLOAT) {
    buf_puts(b, "(sp_int)("); emit_scalar_operand(c, node, "0", b); buf_puts(b, ")");
    return;
  }
  if (emit_nilbool_conv_raise_w(c, node, TY_INT, strict == 0, strict == 2, b)) return;
  if (emit_obj_conv(c, node, "to_int", TY_INT, "Integer", b)) return;
  /* A strict Integer slot fed from a nullable int (a `String#index` miss, an
     ivar written nil, an `Integer?` seed) receives SP_INT_NIL as a plain
     sp_int: the arm folded it as a number instead of refusing it the way the
     compile-time `s[nil]` is refused above. Test for it here, at the one
     funnel every strict slot passes through, rather than in each arm -- the
     leak was never the index arms alone (#4896). The nilable slots
     (emit_int_expr_nilable, strict == 0) keep their looseness, and a Range
     endpoint is a nilable slot for exactly this reason: `s[ix..]` with a nil
     ix is a beginless Range in CRuby, not an error. */
  if (strict && comp_ntype(c, node) == TY_INT && nullable_int_value(c, node)) {
    int tn = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tn);
    emit_scalar_operand(c, node, "0", b);
    buf_printf(b, "; SP_INT_NIL_ARG_CK%s(_t%d); _t%d; })",
               strict == 2 ? "_OF" : "", tn, tn);
    return;
  }
  emit_scalar_operand(c, node, "0", b);
}

void emit_int_expr(Compiler *c, int node, Buf *b) {
  emit_int_expr_ex(c, node, 1, b);
}

/* The slot accepts nil in CRuby: keep the historical looseness. */
void emit_int_expr_nilable(Compiler *c, int node, Buf *b) {
  emit_int_expr_ex(c, node, 0, b);
}

/* A Range endpoint in an index: the one Integer slot where nil is not the
   TypeError the strict arms raise but an absent bound -- `s[nil..]` and
   `s[ix..]` on a missed `index` are both the beginless Range covering the
   whole receiver. `none` is the bound the arm already uses for the endpoint
   the literal omits (0 for a begin, -1 or the length for an end), so the
   written-out `s[nil..]` and the omitted `s[..]` take the same path (#4896). */
void emit_int_expr_bound(Compiler *c, int node, const char *none, Buf *b) {
  if (comp_ntype(c, node) == TY_NIL) {   /* the literal, and anything typed nil */
    buf_puts(b, "((void)("); emit_expr(c, node, b); buf_printf(b, "), (sp_int)(%s))", none);
    return;
  }
  if (comp_ntype(c, node) == TY_INT && nullable_int_value(c, node)) {
    int tn = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tn);
    emit_int_expr_ex(c, node, 0, b);
    buf_printf(b, "; _t%d == SP_INT_NIL ? (sp_int)(%s) : _t%d; })", tn, none, tn);
    return;
  }
  emit_int_expr_ex(c, node, 1, b);
}

/* Strict, but with CRuby's rb_convert_type wording ("of nil into Integer"):
   Random.srand's seed, Dir.mkdir's mode, Random#bytes' size. */
void emit_int_expr_conv(Compiler *c, int node, Buf *b) {
  emit_int_expr_ex(c, node, 2, b);
}

/* Emit a node as an sp_float. A poly value is unboxed via sp_poly_to_f; a
   numeric value is plain-cast, matching the legacy `(sp_float)(...)`. The
   slot follows CRuby's rb_to_float, which converts only a Numeric: a String,
   Symbol, nil, boolean, container or user object (its #to_f is not asked,
   unless the class is a Numeric of the program's own) is
   "can't convert X into Float" -- where the raw pointer used to be cast to
   double and stop the C build (Math.sqrt(obj), Float#rationalize("x")). */
/* Is class `cid` a Numeric of the program's own (`class D < Numeric`, or
   under one)? rb_to_float converts those through their #to_f. */
static int class_is_user_numeric(Compiler *c, int cid) {
  const NodeTable *nt = c->nt;
  for (int k = cid, hop = 0; k >= 0 && hop < 64; k = c->classes[k].parent, hop++) {
    int dn = c->classes[k].def_node;
    if (dn < 0 || nt_kind(nt, dn) != NK_ClassNode) continue;
    int sup = nt_ref(nt, dn, "superclass");
    if (sup < 0) continue;
    NodeKind sk = nt_kind(nt, sup);
    const char *sn = (sk == NK_ConstantReadNode || sk == NK_ConstantPathNode) ? nt_str(nt, sup, "name") : NULL;
    if (sn && sp_streq(sn, "Numeric")) return 1;
  }
  return 0;
}

void emit_float_expr(Compiler *c, int node, Buf *b) {
  if (yield_site_type(c, node) == TY_POLY) {
    buf_puts(b, "sp_poly_to_f("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  TyKind t = comp_ntype(c, node);
  if (t == TY_BIGINT) {
    buf_puts(b, "sp_bigint_to_double("); emit_expr(c, node, b); buf_puts(b, ")");
    return;
  }
  /* a Numeric of the program's own converts through its #to_f, as
     rb_to_float asks it to (BigDecimal into Math.sqrt) */
  if (ty_is_object(t) && ty_object_class(t) >= 0 && ty_object_class(t) < c->nclasses &&
      class_is_user_numeric(c, ty_object_class(t)) &&
      comp_method_in_chain(c, ty_object_class(t), "to_f", NULL) >= 0 &&
      emit_obj_conv(c, node, "to_f", TY_FLOAT, "Float", b))
    return;
  const char *cn = conv_cls_name_of(c, t);
  if (cn && t != TY_INT && t != TY_FLOAT) {
    buf_puts(b, "({ (void)(");
    emit_expr(c, node, b);
    buf_printf(b, "); sp_raise_cls(\"TypeError\", \"can't convert %s into Float\"); 0.0; })", cn);
    return;
  }
  if (t == TY_BOOL) {
    buf_puts(b, "({ sp_raise_cls(\"TypeError\", (");
    emit_expr(c, node, b);
    buf_puts(b, ") ? \"can't convert true into Float\" : \"can't convert false into Float\"); 0.0; })");
    return;
  }
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  if (!coerce_const_raise(tmp.p ? tmp.p : "", "0.0", b)) {
    buf_puts(b, "(sp_float)(");
    buf_puts(b, tmp.p ? tmp.p : "");
    buf_puts(b, ")");
  }
  free(tmp.p);
}

/* A Float operand of an arithmetic method (Float#quo, #fdiv): a Numeric
   converts as the float slot does, and any other class is the coercion
   failure, "X can't be coerced into Float", where X is the value's inspect
   for nil, true, false and a Symbol and its class name otherwise -- not the
   conversion slot's "can't convert X into Float". */
void emit_float_coerce_expr(Compiler *c, int node, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_POLY || t == TY_UNKNOWN ||
      t == TY_RATIONAL) {
    emit_float_expr(c, node, b);
    return;
  }
  if (t == TY_BOOL) {
    buf_puts(b, "({ sp_raise_cls(\"TypeError\", (");
    emit_expr(c, node, b);
    buf_puts(b, ") ? \"true can't be coerced into Float\" : \"false can't be coerced into Float\"); 0.0; })");
    return;
  }
  if (t == TY_SYMBOL) {
    buf_puts(b, "({ sp_raise_cls(\"TypeError\", sp_sprintf(\"%s can't be coerced into Float\", sp_sym_inspect(");
    emit_expr(c, node, b);
    buf_puts(b, "))); 0.0; })");
    return;
  }
  const char *cn = conv_cls_name_of(c, t);
  if (!cn) {
    emit_float_expr(c, node, b);
    return;
  }
  buf_puts(b, "({ (void)("); emit_expr(c, node, b);
  buf_printf(b, "); sp_raise_cls(\"TypeError\", \"%s can't be coerced into Float\"); 0.0; })", cn);
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
/* The gate's raise tokens reach a slot inside a wrapping paren as often as
   bare (an operand emitted as `(sp_raise_nomethod(...))`), so the token match
   has to look past any leading ones or the coercion silently does not fire and
   the sp_RbVal lands in the typed slot raw. */
const char *past_open_parens(const char *s) {
  while (*s == '(') s++;
  return s;
}

static void emit_str_expr_ex(Compiler *c, int node, int strict, Buf *b) {
  if (yield_site_type(c, node) == TY_POLY) {
    /* sp_poly_arg_str, not sp_poly_to_s: a boxed user object in this slot
       converts through #to_str or raises, where to_s rendered it as
       "#<Name ...>" and the builtin searched for that text; the _chk form
       additionally raises for a boxed nil / true / false as CRuby does */
    int tmp; Buf *hb = conv_hold_begin(b, &tmp);
    Buf *ob = hb ? hb : b;
    buf_puts(ob, strict ? "sp_poly_arg_str_chk(" : "sp_poly_arg_str(");
    emit_expr(c, node, ob); buf_puts(ob, ")");
    if (hb) conv_hold_end(tmp);
    return;
  }
  if (emit_nilbool_conv_raise_w(c, node, TY_STRING, !strict, 0, b)) return;
  if (emit_obj_conv(c, node, "to_str", TY_STRING, "String", b)) return;
  /* The unresolved-call gate's sp_raise_nomethod(...) is a side-effecting poly
     value (it raises): coerce it to the const char* slot, keeping the call,
     rather than passing the sp_RbVal through raw (doom's
     `File.join(Dir.tmpdir, ...)`). A text match on the gate's own token is
     reliable where comp_ntype is not (the node stays TY_UNKNOWN). */
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  const char *txt = tmp.p ? tmp.p : "";
  /* Anything that DIVERGES: the several raise emitters each close their
     expression with a dead placeholder of whatever type the node had, and that
     type is not the string this slot wants -- an sp_RbVal from
     raise_tail_value's UNKNOWN case (`"b" + super` in a module with no
     superclass), an sp_Class from an unresolvable constant read inside an
     interpolation (#4092). Discarding the whole thing and answering NULL
     type-checks for every one of them, and the raise means the NULL is never
     read. Keyed on the token rather than on the placeholder, since it is the
     token that says the expression cannot return. */
  if (strncmp(past_open_parens(txt), "sp_raise_", 9) == 0)
    buf_printf(b, "((void)(%s), (const char *)NULL)", txt);
  else buf_puts(b, txt);
  free(tmp.p);
}

void emit_str_expr(Compiler *c, int node, Buf *b) {
  emit_str_expr_ex(c, node, 1, b);
}

/* A node entering a PATH slot: File, Dir and IO's path arguments. CRuby's
   rb_get_path asks the object for #to_path before #to_str, which is how a
   Pathname, or any user class that names a file, is accepted wherever a
   String path is. A statically-typed object converts through a direct call;
   a boxed one goes through sp_poly_arg_path, whose generated bridge reaches
   the same methods. A class defining neither is refused at compile time the
   way the String slot refuses it, naming both methods. */
void emit_path_expr(Compiler *c, int node, Buf *b) {
  if (yield_site_type(c, node) == TY_POLY) {
    int tmp; Buf *hb = conv_hold_begin(b, &tmp);
    Buf *ob = hb ? hb : b;
    buf_puts(ob, "sp_poly_arg_path("); emit_expr(c, node, ob); buf_puts(ob, ")");
    if (hb) conv_hold_end(tmp);
    return;
  }
  TyKind t = comp_ntype(c, node);
  int cid = ty_is_object(t) ? ty_object_class(t) : -1;
  if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_native_class) {
    int def = -1;
    if (obj_conv_method_typed(c, t, "to_path", TY_STRING, &def) >= 0) {
      emit_obj_conv_call(c, node, t, def, "to_path", b);
      return;
    }
    int mi = comp_method_in_chain(c, cid, "to_path", &def);
    if (mi >= 0) {
      /* a #to_path the analysis could not pin to String -- one backed by an
         accessor, or with a nil branch -- answers a boxed value. CRuby checks
         the RESULT of #to_path, so the boxed answer takes the strict String
         slot's check, which raises CRuby's TypeError for a non-String. Any
         other shape is not this protocol, and says which method is at fault. */
      if (c->scopes[mi].nparams == 0 && c->scopes[mi].ret == TY_POLY) {
        int tmp; Buf *hb = conv_hold_begin(b, &tmp);
        Buf *ob = hb ? hb : b;
        buf_puts(ob, "sp_poly_arg_str_chk(");
        emit_obj_conv_call_inline(c, node, t, def, "to_path", ob);
        buf_puts(ob, ")");
        if (hb) conv_hold_end(tmp);
        return;
      }
      const char *cn = class_ruby_name(c, cid);
      char msg[256];
      snprintf(msg, sizeof msg,
               "no implicit conversion of %s into String (%s#to_path must take no arguments and answer a String)",
               cn ? cn : "Object", cn ? cn : "Object");
      unsupported_feature(c, node, msg);
    }
    if (comp_method_in_chain(c, cid, "to_str", NULL) < 0) {
      const char *cn = class_ruby_name(c, cid);
      char msg[256];
      snprintf(msg, sizeof msg,
               "no implicit conversion of %s into String (%s defines neither #to_path nor #to_str)",
               cn ? cn : "Object", cn ? cn : "the class");
      unsupported_feature(c, node, msg);
    }
  }
  emit_str_expr(c, node, b);
}

/* The operand of a String COMPARISON -- #<=>, #casecmp, #casecmp?, and the
   ordered operators and #between? Comparable builds on #<=>. CRuby asks a
   non-String operand for #to_str (rb_check_string_type) and compares the
   strings. Unlike the String argument slot above, a class that answers
   nothing is not an error here: it is the comparison's own nil, or its
   "comparison of String with X failed".

   1 iff the operand is a user object whose class answers a #to_str this rule
   converts through. The type rules ask the same predicate of the same class
   (analyze_util.c), so typing and emission agree on the typed side. */
int str_cmp_conv_shape(Compiler *c, int node) {
  TyKind t = comp_ntype(c, node);
  return ty_is_object(t) && class_has_to_str_shape(c, ty_object_class(t));
}

/* The conversion itself, reading the operand back out of the rooted sp_RbVal
   temp the prologue below spilled it into rather than re-emitting the operand
   expression: the object is otherwise reachable from nothing but the argument
   being converted, and its own #to_str allocates before it reads self.

   NULL means "no conversion", which each arm turns into its own refusal.
   Four shapes answer it: a class with no usable #to_str (the literal NULL --
   the arm is then the refusal, and the C compiler folds the compare away), a
   #to_str typed String that answers the nil String, a #to_str typed poly that
   answers nil, and a BOXED value that is neither a String nor an object the
   conversion bridge carries. A poly answer that is neither nil nor a String
   is CRuby's TypeError, which sp_str_cmp_conv raises.

   A boxed operand asks the runtime's own rb_check_string_type, the same
   sp_poly_check_str the boxed comparison and the poly casecmp arm ask, so a
   poly slot holding a String compares and one holding an object converts
   (rooted inside sp_poly_check_str_obj) rather than both being refused for
   want of a static type. Only #between? reaches this with a boxed bound: the
   other arms are entered on a statically OBJECT operand, and a boxed one goes
   to sp_poly_lt / sp_poly_spaceship / the poly casecmp arm long before here.

   The tag test on the object shapes keeps the direct call off a NULL self: an
   object-typed slot a method left nil boxes as nil rather than as SP_TAG_OBJ,
   and the refusal then names it "nil", which is CRuby's answer. The object is
   a pointer, never the by-value layout: a class with an object-typed instance
   in a call ARGUMENT -- which every one of these operands is -- is
   disqualified from that layout (detect_value_types). */
void emit_str_cmp_conv(Compiler *c, int node, int tmp, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY) { buf_printf(b, "sp_poly_check_str(_t%d)", tmp); return; }
  if (!str_cmp_conv_shape(c, node)) { buf_puts(b, "NULL"); return; }
  int def = -1;
  int poly = obj_conv_method_typed(c, t, "to_str", TY_STRING, &def) < 0;
  if (poly) comp_method_in_chain(c, ty_object_class(t), "to_str", &def);
  buf_printf(b, "(_t%d.tag == SP_TAG_OBJ ? ", tmp);
  if (poly) buf_puts(b, "sp_str_cmp_conv(");
  buf_printf(b, "sp_%s_to_str((sp_%s *)_t%d.v.p)",
             c->classes[def].c_name, c->classes[def].c_name, tmp);
  if (poly) buf_printf(b, ", _t%d)", tmp);
  buf_puts(b, " : NULL)");
}

/* The prologue every String-comparison arm shares. It opens a statement
   expression and emits, in Ruby's own evaluation order:

     ({ const char *_tr = <recv>;   SP_GC_ROOT_STR(_tr);
        sp_RbVal    _to = <operand>; SP_GC_ROOT_RBVAL(_to);
        const char *_ts = <conversion, or NULL>; _ts ?

   -- receiver, then operand, then #to_str, once each. Both spills are
   load-bearing: #to_str allocates, so the receiver has to survive it (a
   comparison's receiver is often a fresh string held in nothing else -- an
   interpolation, a `+`), and so does the operand OBJECT, which reads self
   after allocating. The conversion is deliberately NOT put in the call's
   conversion hold: the hold would hoist it in front of the receiver, and
   #between? needs it left where the compare that reads it is.

   The caller closes with `<compare> : <fallback>; })`. Nothing may allocate
   between the two -- the converted string is live only in _ts. */
void emit_str_cmp_prologue(Compiler *c, const char *rtxt, int operand,
                           int *tr, int *to, int *ts, Buf *b) {
  *tr = ++g_tmp; *to = ++g_tmp; *ts = ++g_tmp;
  buf_printf(b, "({ const char *_t%d = %s; SP_GC_ROOT_STR(_t%d); sp_RbVal _t%d = ",
             *tr, rtxt, *tr, *to);
  emit_boxed(c, operand, b);
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); const char *_t%d = ", *to, *ts);
  emit_str_cmp_conv(c, operand, *to, b);
  buf_printf(b, "; _t%d ? ", *ts);
}

/* A node entering a WRITE payload slot (IO#write, #pwrite, #write_nonblock):
   CRuby writes the operand's #to_s, so a String passes through and anything
   else renders the way puts renders it -- a user object through its own
   #to_s. The #to_str protocol of the String slots is the wrong one here. */
void emit_to_s_expr(Compiler *c, int node, Buf *b) {
  TyKind wt = comp_ntype(c, node);
  if (wt == TY_STRING) { emit_expr(c, node, b); return; }
  /* An UNTYPED node is not a boxed value, and emit_boxed renders one as
     `(expr, sp_box_nil())` -- it evaluates the payload and then throws it
     away. `@wrk.pack("C*")` on a nilable ivar is a const char * typed
     TY_UNKNOWN (optcarrot's ROM#save_battery), and boxing it wrote an empty
     file where the String slot these payloads used to take wrote the bytes.
     Keep that rendering for an untyped operand; a poly one still converts. */
  if (wt == TY_UNKNOWN || wt == TY_VOID) { emit_str_expr(c, node, b); return; }
  int tmp; Buf *hb = conv_hold_begin(b, &tmp);
  Buf *ob = hb ? hb : b;
  buf_puts(ob, "sp_poly_to_s("); emit_boxed(c, node, ob); buf_puts(ob, ")");
  if (hb) conv_hold_end(tmp);
}

/* The slot accepts nil in CRuby: keep the historical looseness. */
void emit_str_expr_nilable(Compiler *c, int node, Buf *b) {
  emit_str_expr_ex(c, node, 0, b);
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
  int is_tok = strncmp(past_open_parens(txt), "sp_raise_nomethod(", 18) == 0;
  /* The missing-super arm emits a `(sp_raise_cls(...), 0)` comma expression
     whose dummy tail is a bare int; in a pointer-typed slot that is ill-typed
     C. The raise never returns, so evaluate it for the raise and yield the
     slot's default instead. */
  int is_cls_tok = strncmp(past_open_parens(txt), "sp_raise_cls(", 13) == 0;
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
int call_returns_nullable_int(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *nty = nt_type(nt, node);
  /* The analyzer's own answer, which knows the shapes only whole-program
     inference can see: a `yield` (nilable per the block at each call site), a
     method whose nilable return no RBS signature declared, and a call through a
     receiver that stayed poly (#3505). The arms below stay as the local,
     name-based backstop for the builtins analyze does not model. */
  if (nullable_int_value(c, node)) return 1;
  /* A local that was assigned one of these results carries the sentinel just
     as the call did: `i = s.index("z")` then `i == nil` has to answer true.
     analyze marks the local; boxing an ordinary int stays on the plain path,
     which is the hot one (every int boxed into a poly slot). */
  if (nty && sp_streq(nty, "LocalVariableReadNode")) {
    const char *ln = nt_str(nt, node, "name");
    Scope *sc = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = sc ? scope_local(sc, ln) : NULL;
    return lv && lv->nullable_int;
  }
  if (!nty || !sp_streq(nty, "CallNode")) return 0;
  /* a safe-navigation call answers the scalar's nil sentinel when the receiver
     is nil, so boxing it plainly published a NaN (or a sentinel int) where the
     value has to read as nil (#3771) */
  { const char *sop = nt_str(nt, node, "call_operator");
    if (sop && sp_streq(sop, "&.")) return 1; }
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int blk = nt_ref(nt, node, "block");
  /* a proc's result comes back through the boxed slot and is read as the
     sentinel when the proc answered nil (`-> { return; 456 }.call`): box it
     back as nil */
  if (sp_streq(nm, "call") || sp_streq(nm, "()") || sp_streq(nm, "[]") || sp_streq(nm, "yield")) {
    int pr = nt_ref(nt, node, "receiver");
    if (pr >= 0 && comp_ntype(c, pr) == TY_PROC) return 1;
  }
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
    TyKind rrt = r >= 0 ? comp_ntype(c, r) : TY_UNKNOWN;
    /* MatchData#begin/end (an unmatched optional group -> nil) and,
       missed until a generic Comparable#clamp (builtins/comparable.rb)
       stored a beginless/endless Range's own #begin/#end into a plain
       local -- boxing the raw SP_INT_NIL sentinel as sp_box_int gave a
       value that answered `.nil?` false and `<=>` a huge fake number
       instead of the open bound CRuby's clamp/between? treat it as. A
       Float-bounded Range's #end/#begin instead reads back HUGE_VAL, a
       genuine Float value that already boxes correctly, so
       TY_FLOAT_RANGE is not part of this. */
    if (rrt == TY_MATCHDATA || rrt == TY_RANGE) return 1;
  }
  /* an attr-reader over an int ivar: int ivars are SP_INT_NIL-defaulted
     (ivar_scalar_nil_init), so the read can carry the sentinel -- boxing it
     as a plain int made `stored.nil?` false while inspect printed nil
     (#3288). box_int_or_nil is a no-op on a real int. */
  {
    int r = nt_ref(nt, node, "receiver");
    int a2 = nt_ref(nt, node, "arguments");
    int an2 = 0; if (a2 >= 0) nt_arr(nt, a2, "arguments", &an2);
    if (r >= 0 && an2 == 0 && nt_ref(nt, node, "block") < 0) {
      TyKind rt2 = comp_ntype(c, r);
      if (ty_is_object(rt2)) {
        int cid2 = ty_object_class(rt2), defc2 = -1;
        if (comp_reader_in_chain(c, cid2, nm, &defc2)) {
          char ivb2[300];
          snprintf(ivb2, sizeof ivb2, "@%s", comp_resolve_alias(c, cid2, nm));
          int iv2 = comp_ivar_index(&c->classes[defc2 >= 0 ? defc2 : cid2], ivb2);
          if (iv2 >= 0 &&
              c->classes[defc2 >= 0 ? defc2 : cid2].ivar_types[iv2] == TY_INT)
            return 1;
        }
      }
    }
  }
  return 0;
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
    buf_puts(b, "((void)sp_proc_yield(");
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
  /* A proc-form body has no inline block by construction -- the block arrives
     as an sp_Proc * parameter -- so the yield lowers to a call on it, not to
     the no-block raise (#3399). */
  if (g_yield_proc_ref && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode")) {
    /* This helper's callers want a BOXED value, so ask for the poly form. */
    emit_yield_proc_call(c, nt_ref(c->nt, node, "arguments"), TY_POLY, b, 0, 1);
    return;
  }
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
    /* A forwarding block (`wrap { yield }` handing its own block on) has a
       yield for its tail, and that node's cached type is one site's too:
       the value at THIS site is what the block one level out answers, the
       one the splice will run (#4495). One level is all the expander
       records (g_yield_block_fallback). */
    int tblk = g_block_id;
    TyKind bt = TY_NIL;
    for (int depth = 0; tblk >= 0; depth++) {
      int bbody = nt_ref(c->nt, tblk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn <= 0) { bt = TY_NIL; break; }
      int tail = bb[bn - 1];
      if (depth == 0 && g_yield_block_fallback >= 0 && nt_type(c->nt, tail) &&
          sp_streq(nt_type(c->nt, tail), "YieldNode")) { tblk = g_yield_block_fallback; continue; }
      bt = comp_ntype(c, tail);
      /* The tail is not the only value the block can produce: `next v` leaves
         it early with one, and that value is as much this site's answer as
         the tail is. Read from the tail alone, a block whose tail is `nil`
         typed the whole splice TY_NIL, and the nil arm below throws the
         splice's value away and hands back a constant -- so
         `{ |i| next 7 if i == 1; nil }` answered nil for the 7 as well.

         `next` only, through the very helper yield_value_type joins with the
         tail for the analysis. A `break` leaves the ITERATOR rather than the
         block, so its value belongs to the iterator call and not to this
         splice; counting it widened blocks that carry one and put an sp_int
         where the slot was an sp_RbVal. */
      { TyKind nx = block_next_value_ty(c, bbody);
        if (nx != TY_UNKNOWN) bt = (bt == TY_UNKNOWN) ? nx : ty_unify(bt, nx); }
      break;
    }
    if (bt != t && bt != TY_UNKNOWN) {
      if (bt == TY_POLY) { emit_expr(c, node, b); return; }
      Buf yb; memset(&yb, 0, sizeof yb);
      emit_expr(c, node, &yb);
      const char *yt = yb.p ? yb.p : "0";
      /* The invoke emitter boxes an object tail into a poly slot, so for that
         shape the splice above already yielded an sp_RbVal -- re-boxing would
         cast a struct through (void *) (#3329). */
      int pre_boxed = (t == TY_POLY && ty_is_object(bt));
      if (bt == TY_NIL || bt == TY_VOID)
        buf_printf(b, "({ %s; sp_box_nil(); })", yt);
      else if (pre_boxed)
        buf_puts(b, yt);
      else
        emit_boxed_text(c, bt, yt, b);
      free(yb.p);
      return;
    }
  }
  /* An instance-variable read whose node type came from a different class
     than the one whose struct it reads: a module method transplanted into an
     including class types `@x` in the module's own scope (there poly/unknown)
     while the emitted field lives on the including class. Box the concrete
     field so the two agree -- `sp_poly_add(self->iv_x, ...)` fed sp_int to an
     sp_RbVal parameter and did not compile. */
  if (t == TY_POLY && g_emitting_class_id >= 0 &&
      nt_kind(c->nt, node) == NK_InstanceVariableReadNode) {
    Scope *sc0 = comp_scope_of(c, node);
    if (!sc0 || sc0->class_id != g_emitting_class_id) {
      const char *nm0 = nt_str(c->nt, node, "name");
      int iv0 = nm0 ? comp_ivar_index(&c->classes[g_emitting_class_id], nm0) : -1;
      TyKind ft0 = iv0 >= 0 ? c->classes[g_emitting_class_id].ivar_types[iv0] : TY_UNKNOWN;
      if (ft0 != TY_POLY && ft0 != TY_UNKNOWN) {
        Buf ib; memset(&ib, 0, sizeof ib);
        unsigned char sv_m0 = c->strbuf_box[node];
        c->strbuf_box[node] = 0;
        emit_expr(c, node, &ib);
        c->strbuf_box[node] = sv_m0;
        emit_boxed_text(c, ft0, ib.p ? ib.p : "0", b);
        free(ib.p);
        return;
      }
    }
  }
  if (t == TY_POLY) {
    emit_expr(c, node, b);
    return;
  }
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
    int ocid = ty_object_class(t);
    /* A class with subclasses is only the STATIC type here: an inherited
       method boxing `self` would stamp the defining class, and the boxed value
       then dispatched as the parent (#3773). Read the id the object carries.
       Exception subclasses are a special case: the runtime layout (sp_Exception
       + ivars) starts with `cls_name` (a pointer), NOT a cls_id header, so
       sp_box_nullable_obj_dyn's read of the first sizeof(sp_int) bytes reads
       the low bits of a heap pointer. Use the static cls_id for them -- the
       static type always matches the runtime type because `Class.new` is the
       only path and it stamps the requested class. */
    int subclassed = !is_val && class_has_subclass(c, ocid) && !class_is_exc_subclass(c, ocid);
    if (is_val) buf_printf(b, "sp_box_vobj_%s(", c->classes[ocid].c_name);
    else if (subclassed) buf_puts(b, "sp_box_nullable_obj_dyn((void *)(");
    else buf_puts(b, "sp_box_nullable_obj((void *)(");
    emit_expr(c, node, b);
    buf_printf(b, is_val ? ")" : "), %d)", ocid);
    return;
  }
  if (ty_is_hash(t)) {
    /* Nullable, for the same reason the object arm just above is: a hash slot
       holding nil is a NULL pointer, and sp_box_obj wrapped that in a truthy
       SP_TAG_OBJ -- so `h.nil?` answered false and the first read of it
       dereferenced NULL (#4134). Kept in step with emit_boxed_text. */
    const char *hid = hash_box_cls(t);
    if (hid) {
      buf_puts(b, "sp_box_nullable_obj((void *)(");
      emit_expr(c, node, b);
      buf_printf(b, "), %s)", hid);
      return;
    }
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
    /* A parameter bound from an ivar read before anything had to assign it
       (box_nullable_arg) boxes as nil too; its typed reads stay unchecked,
       so the flag is asked here alone (#5085). */
    case TY_INT:    fn = (g_promote_mode || call_returns_nullable_int(c, node) ||
                          nt_kind(c->nt, node) == NK_InstanceVariableReadNode ||
                          box_nullable_arg(c, node))
                           ? "sp_box_int_or_nil" : "sp_box_int"; break;
    /* A float slot has its own reserved nil sentinel, and the same rule
       applies: box it as nil where the value can be one, or it goes out as an
       ordinary Float and no literal nil matches it (#3493). */
    case TY_FLOAT:  fn = (call_returns_nullable_int(c, node) || box_nullable_arg(c, node))
                           ? "sp_box_float_or_nil" : "sp_box_float"; break;
    /* NULL is a bigint slot's nil (nil_value), and boxing it as a Bignum made
       a truthy Integer that printed 0 (#4800). Unconditional: a live Bignum is
       never the NULL pointer, so the test costs one compare on a path that
       already allocates, and no analysis has to prove nilability. */
    case TY_BIGINT: fn = "sp_box_bigint_or_nil"; break;
    case TY_STRING: fn = "sp_box_str";   break;
    case TY_BOOL:   fn = "sp_box_bool";  break;
    case TY_SYMBOL: fn = "sp_box_sym";   break;
    case TY_RANGE:  fn = "sp_box_range"; break;
    case TY_FLOAT_RANGE: fn = "sp_box_frange"; break;
    case TY_STR_RANGE: fn = "sp_box_srange"; break;
    case TY_TMS: fn = "sp_box_tms"; break;
    case TY_STRBUF: {
      /* a marked container-store read of a shared-mutable string: box the
         sp_String* HANDLE so later in-place mutation is visible through the
         container (#3227 phase 3). Marked nodes are local reads. */
      const char *bn0 = nt_type(c->nt, node) &&
                        sp_streq(nt_type(c->nt, node), "LocalVariableReadNode")
                          ? nt_str(c->nt, node, "name") : NULL;
      if (bn0) {
        /* the mark can outlive the slot's own type: a local that settled
           POLY already holds a boxed value -- wrapping it as a raw handle
           would reinterpret an sp_RbVal as sp_String* (#3325) */
        Scope *bs0 = comp_scope_of(c, node);
        LocalVar *blv0 = bs0 ? scope_local(bs0, bn0) : NULL;
        if (blv0 && blv0->type == TY_POLY)
          buf_printf(b, "lv_%s", rename_local(bn0));
        else
          buf_printf(b, "sp_box_obj(lv_%s, SP_BUILTIN_STRBUF)", rename_local(bn0));
        return;
      }
      { char srefX[192];
        if (nt_kind(c->nt, node) == NK_InstanceVariableReadNode &&
            strbuf_slot_ref(c, node, srefX, sizeof srefX)) {
          buf_printf(b, "sp_box_obj(%s, SP_BUILTIN_STRBUF)", srefX);
          return;
        } }
      /* An ivar WRITE in value position lowers to `({ iv_x = ...; iv_x; })`,
         so its value IS the slot -- the same handle the read above boxes, not
         a string to wrap a fresh handle around (#3993). */
      if (nt_kind(c->nt, node) == NK_InstanceVariableWriteNode) {
        buf_puts(b, "sp_box_obj(");
        unsigned char sv_mw = c->strbuf_box[node];
        c->strbuf_box[node] = 0;
        emit_expr(c, node, b);
        c->strbuf_box[node] = sv_mw;
        buf_puts(b, ", SP_BUILTIN_STRBUF)");
        return;
      }
      /* an element read is ALREADY a boxed handle when the container holds
         one: pass it through (as a handle box either way) so the alias keeps
         the container's own string rather than a fresh copy of it (#3941) */
      if (strbuf_boxed_elem_read(c, node)) {
        buf_puts(b, "sp_box_obj(sp_poly_as_strbuf(");
        unsigned char sv_m2 = c->strbuf_box[node];
        c->strbuf_box[node] = 0;
        emit_expr(c, node, b);
        c->strbuf_box[node] = sv_m2;
        buf_puts(b, "), SP_BUILTIN_STRBUF)");
        return;
      }
      /* a demanded literal / expression store: wrap a FRESH handle so the
         container element is mutable in place (#3227 P3) */
      buf_puts(b, "sp_box_obj(sp_String_new_shared(");
      { TyKind sv_probe = comp_ntype(c, node);
        (void)sv_probe;
        Buf eb0; memset(&eb0, 0, sizeof eb0);
        unsigned char sv_mark = c->strbuf_box[node];
        c->strbuf_box[node] = 0;   /* emit the plain string value */
        emit_str_expr(c, node, &eb0);
        c->strbuf_box[node] = sv_mark;
        buf_puts(b, eb0.p ? eb0.p : "(&(\"\\xff\")[1])");
        free(eb0.p); }
      buf_puts(b, "), SP_BUILTIN_STRBUF)"); return;
    }
    case TY_OPENSTRUCT:
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_puts(b, "), SP_BUILTIN_OPENSTRUCT)"); return;
    case TY_TIME:   fn = "sp_box_time";  break;
    case TY_COMPLEX:  fn = "sp_box_complex";  break;
    case TY_RATIONAL: fn = "sp_box_rational"; break;
    /* TY_PROC / TY_METHOD are handled by the nullable-builtin box above. */
    /* Array slots are nilable C pointers: a nil-defaulting param, a nullable
       ivar, or `[x] if cond` in value position is NULL. Box NULL as a proper
       nil, not a truthy OBJ wrapping NULL that passes `if x`/`unless x` and
       then segfaults on the first access (#3275). A non-nil array is never
       NULL, so the guard's untaken branch is free on the hot path. Matches
       emit_boxed_text's array cases. */
    case TY_INT_ARRAY:
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_puts(b, "), SP_BUILTIN_INT_ARRAY)"); return;
    case TY_FLOAT_ARRAY:
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_puts(b, "), SP_BUILTIN_FLT_ARRAY)"); return;
    case TY_STR_ARRAY:
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_puts(b, "), SP_BUILTIN_STR_ARRAY)"); return;
    case TY_POLY_ARRAY:
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_puts(b, "), SP_BUILTIN_POLY_ARRAY)"); return;
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
    /* A kind narrow_object_arrays produces (an object array, a table of int
       or float rows) has no boxed form: the one cls_id for them is type-
       erased, so a reference box would read every element as #<Object>.
       The untyped fallback below evaluates the value and yields nil, which
       is exactly what a method answering the table on one path and nil on
       another used to return (#4486). Refuse the shape instead. */
    /* a nested table or an object array boxes by reference, stamped with what
       its elements are, where it used to be refused (#4486) */
    if (ty_is_ptr_array(t)) {
      buf_puts(b, "sp_box_ptr_array_k((void *)("); emit_expr(c, node, b);
      buf_printf(b, "), %s)", ptr_array_stamp(c, t));
      return;
    }
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
/* A cell that shadows a plain C slot (an INLINED block's param, bound by the
   loop emitters writing that slot) has to take the slot's current value before
   a proc built here reads the cell. Emitted at the capture fill, which is the
   one point every such proc goes through. */
void emit_cell_shadow_store(Compiler *c, Scope *encl, const char *name, Buf *b, int indent) {
  (void)c;
  LocalVar *lv = encl && name ? scope_local(encl, name) : NULL;
  if (!lv || !lv->is_cell || !lv->cell_shadow) return;
  emit_indent(b, indent);
  if (lv->type == TY_PROC) buf_printf(b, "*_cell_%s = (sp_int)(uintptr_t)lv_%s;\n", name, name);
  else buf_printf(b, "*_cell_%s = lv_%s;\n", name, name);
}

void declare_local(Compiler *c, Buf *b, LocalVar *lv, int vol) {
  declare_local_named(c, b, lv, lv->name, vol);
}

/* declare_local's body with the C name decoupled from lv->name. An inlined
   callee's frame (the Method#to_proc trampoline is separate, but the bound
   `.call` statement expression shares the caller's scope) declares its
   method-scope locals under per-frame unique names so a same-named caller
   local in an argument expression is not captured by the declaration. */
void declare_local_named(Compiler *c, Buf *b, LocalVar *lv, const char *name, int vol) {
  TyKind t = lv->type;
  Buf cty; memset(&cty, 0, sizeof cty);
  const char *init = "0";
  int ptr = 0, root = needs_root(t);
  switch (t) {
    case TY_INT:    buf_puts(&cty, "sp_int"); init = "0"; break;
    case TY_BIGINT: buf_puts(&cty, "sp_Bigint *"); init = "NULL"; ptr = 1; break;
    case TY_FLOAT:  buf_puts(&cty, "sp_float"); init = "0.0"; break;
    case TY_BOOL:   buf_puts(&cty, "sp_bool"); init = "0"; break;
    case TY_SYMBOL: buf_puts(&cty, "sp_sym"); init = "((sp_sym)-1)"; break;
    case TY_RANGE:  buf_puts(&cty, "sp_Range"); init = "{0}"; break;
    case TY_FLOAT_RANGE: buf_puts(&cty, "sp_FloatRange"); init = "{0}"; break;
    case TY_STR_RANGE: buf_puts(&cty, "sp_StrRange"); init = "{0}"; break;
    case TY_TIME:   buf_puts(&cty, "sp_Time"); init = "{0}"; break;
    case TY_COMPLEX:  buf_puts(&cty, "sp_Complex"); init = "{0}"; break;
    case TY_RATIONAL: buf_puts(&cty, "sp_Rational"); init = "{0}"; break;
    case TY_TMS:    buf_puts(&cty, "sp_Tms"); init = "{0}"; break;
    case TY_PROCESS_STATUS: buf_puts(&cty, "sp_ProcessStatus *"); init = "NULL"; ptr = 1; break;
    case TY_OPENSTRUCT: buf_puts(&cty, "sp_OpenStruct *"); init = "NULL"; ptr = 1; break;
    case TY_STRING: buf_puts(&cty, "const char *"); init = "NULL"; ptr = 1; break;  /* nil until assigned (#3295) */
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
  /* A local with no definite assignment anywhere starts as Ruby nil, not as
     its type's zero: `x ||= v` has to be able to tell "never assigned" from
     "assigned 0". The pointer kinds already start NULL; this is what gives the
     sentinel-carrying scalars the same footing (#3388). Block-locals are reset
     to nil_value on every iteration already (emit_block_locals_reset). */
  if (lv->or_write_only && !lv->is_param && !lv->is_block_param) {
    const char *nv = nil_value(t);   /* NULL for the kinds with no sentinel */
    if (nv) init = nv;
  }
  buf_puts(b, "    ");
  if (vol && !ptr) buf_puts(b, "volatile ");
  buf_puts(b, cty.p ? cty.p : "");
  if (vol && ptr) buf_puts(b, "volatile ");  /* cty ends with "* "; -> "* volatile " */
  buf_printf(b, " lv_%s = %s;\n", name, init);
  if (t == TY_POLY) buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", name);
  /* A String range is a by-value struct carrying two GC strings, so the
     struct's own address is not a root the collector can follow -- it would
     read the first endpoint as if it were the object. Each endpoint slot is
     rooted instead, the way a value-type object's string fields below are.
     Without this both endpoints were collected while the range still named
     them: `("a#{i}".."z#{i}")` read back wrong on 14 of 400 turns plainly and
     on all 400 under GC stress (#4353 left this open). */
  else if (t == TY_STR_RANGE) {
    buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.first);\n", name);
    buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.last);\n", name);
  }
  /* A String slot takes the STRING root form, not the object one. Both reach
     an ordinary heap string, but a mutable String's PAYLOAD (marker 0xfd) is
     owned by the handle in front of it, and only sp_mark_string -- which the
     tagged form runs -- follows the payload back to that handle. Through the
     object form the handle goes unreferenced and its finaliser frees the bytes
     the slot still names. Making sp_gc_mark follow it instead is not the fix:
     0xfd is also the guard byte lib/sp_fiber.c lays in front of the static
     root fiber precisely so that tag test SKIPS it, so the two meanings can
     only be told apart by knowing the slot holds a string, which is what the
     tag on the root entry says. */
  else if (root && t == TY_STRING) buf_printf(b, "    SP_GC_ROOT_STR(lv_%s);\n", name);
  else if (root && !comp_ty_value_obj(c, t)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", name);
  else if (comp_ty_value_obj(c, t)) {
    /* a value-type local lives on the stack; root each heap-pointer (string)
       field so its referent survives GC. The field slot is a stable root. */
    ClassInfo *vc = &c->classes[ty_object_class(t)];
    for (int i = 0; i < vc->nivars; i++)
      if (vc->ivar_types[i] == TY_STRING)
        buf_printf(b, "    SP_GC_ROOT(lv_%s.iv_%s);\n", name, iv_c(vc->ivars[i] + 1));
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

/* A break-carrying block call whose break cannot be delivered by a
   same-function goto (brk_wrapper_light declines: the receiver's each is
   only resolved at run time, or the call reaches a user method that lifts
   the block into a real Proc) is wrapped in the same serial-addressed
   sp_brk_push/setjmp/sp_brk_throw scope a begin/rescue uses -- a local
   written before the throw and read after (an accumulator set right before
   `break`, find_index's `idx = i; break`) is just as indeterminate there
   without volatile. A LIGHT wrapper (every break a goto within this same
   function) needs none of this. */
static int is_heavy_brk_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "block") < 0) return 0;
  return call_breaks(c, id) && !brk_wrapper_surely_light(c, id);
}

/* Does scope index `si` contain a begin/rescue, a `loop {}`, or a heavy
   (real-setjmp) break-carrying call (so its locals need volatile across the
   setjmp it emits)? */
int scope_has_begin(Compiler *c, int si) {
  int nids = 0; const int *ids = cg_scope_nodes(c, si, &nids);
  for (int k = 0; k < nids; k++) {
    int id = ids[k];
    const char *ty = nt_type(c->nt, id);
    if (ty && (sp_streq(ty, "BeginNode") || sp_streq(ty, "RescueNode")))
      return 1;
    if (is_stopiter_loop(c, id) || is_heavy_brk_call(c, id)) return 1;
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
  int nids = 0; const int *ids = cg_scope_nodes(c, si, &nids);
  for (int k = 0; k < nids; k++) {
    int id = ids[k];
    if ((nt_kind(nt, id) == NK_BeginNode) || is_stopiter_loop(c, id) || is_heavy_brk_call(c, id)) mark_subtree(nt, id, inb);
  }
  for (int k = 0; k < nids; k++)
    if (nt_kind(nt, ids[k]) == NK_RescueNode && !inb[ids[k]]) { *all = 1; break; }
  if (*all) { free(inb); return; }
  char **names = NULL; int n = 0, cap = 0;
  for (int k = 0; k < nids; k++) {
    int id = ids[k];
    if (!inb[id] || !lv_is_write_or_target(nt_kind(nt, id))) continue;
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
/* Does this scope perform a regexp match -- the operations that write the match
   registers `$~` / `$1`.. read? Such a method needs a frame of its own, since
   those registers are frame-local in Ruby (#3629). A block is part of the
   method it is spliced into, so this asks about the method scope as a whole. */
/* Does this scope read `__callee__`? Only such a method needs the called-name
   channel (#3729). */
int scope_reads_callee(Compiler *c, int si) {
  const NodeTable *nt = c->nt;
  int nids = 0; const int *ids = cg_scope_nodes(c, si, &nids);
  for (int k = 0; k < nids; k++) {
    int id = ids[k];
    if (nt_kind(nt, id) != NK_CallNode || nt_ref(nt, id, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, id, "name");
    if (nm && sp_streq(nm, "__callee__")) return 1;
  }
  return 0;
}

static int scope_performs_match(Compiler *c, int si) {
  const NodeTable *nt = c->nt;
  static const char *const mnames[] = {
    "=~", "match", "match?", "scan", "gsub", "gsub!", "sub", "sub!",
    "split", "slice", "index", "rindex", "partition", "rpartition",
    "start_with?", "end_with?", "grep", "grep_v", "[]", "===", NULL };
  int nids = 0; const int *ids = cg_scope_nodes(c, si, &nids);
  for (int k = 0; k < nids; k++) {
    int id = ids[k];
    if (nt_kind(nt, id) != NK_CallNode) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm) continue;
    int hit = 0;
    for (int k = 0; mnames[k] && !hit; k++) if (sp_streq(nm, mnames[k])) hit = 1;
    if (!hit) continue;
    /* only when a regexp is actually involved: the receiver or an argument */
    int r = nt_ref(nt, id, "receiver");
    if (r >= 0 && comp_ntype(c, r) == TY_REGEX) return 1;
    int a = nt_ref(nt, id, "arguments");
    int an = 0; const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    for (int k = 0; k < an && av; k++)
      if (comp_ntype(c, av[k]) == TY_REGEX) return 1;
  }
  return 0;
}

/* One captured local's heap cell: the allocation, the root and the initial
   value. Split out of emit_scope_decls because a fiber body needs the same
   emission for a cell it owns rather than inherits -- a local DECLARED inside
   a Thread.new block is one cell per thread, and taking it from the capture
   made eight threads share one counter (#4410). */
static void emit_cell_decl(Compiler *c, Scope *s, LocalVar *lv, Buf *b) {
      /* A cell over an INLINED block's param: the loop emitters bind the plain
         C slot, so declare it too and let the body's opening line copy it into
         the cell (emit_loop_body). */
      if (lv->cell_shadow && !lv->is_param) declare_local(c, b, lv, 0);
      if (lv->type == TY_PROC) {
        /* the cell is an int slot holding a collectable Proc: it needs a scan,
           or the capture keeps the cell and nothing keeps the proc (#4077) */
        buf_printf(b, "    sp_int *_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, sp_cell_scan_procint);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = (sp_int)(uintptr_t)lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = 0;\n", lv->name);
        return;
      }
      /* A float capture gets a native sp_float cell rather than laundering the
         bits through the int slot: *_cell_x is then a real sp_float lvalue, so
         the ordinary read / write / compound-assign paths work unchanged. The
         cell holds no GC pointer, so no cell scan is needed. */
      if (lv->type == TY_FLOAT) {
        buf_printf(b, "    sp_float *_cell_%s = (sp_float *)sp_gc_alloc(sizeof(sp_float), NULL, NULL);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = 0.0;\n", lv->name);
        return;
      }
      /* A class value is a small struct of a cls_id and a rodata name -- no
         GC pointer in it, so its cell needs no scan, as the float cell does
         not. Without a cell of its own a captured class variable hit the
         "non-integer capture" reject: `k = Struct.new(:x); a.each { k.new }`
         over a boxed receiver, where the block is a real closure (#3995). */
      { const char *vs = cell_value_struct(lv->type);
        if (vs) {
          buf_printf(b, "    %s *_cell_%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, NULL);\n", vs, lv->name, vs, vs);
          buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
          if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
          else buf_printf(b, "    *_cell_%s = %s;\n", lv->name, cell_value_struct_empty(lv->type));
          return;
        } }
      if (lv->type == TY_POLY) {
        buf_printf(b, "    sp_RbVal *_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = sp_box_nil();\n", lv->name);
        return;
      }
      /* A pointer (string / array / hash / heap object) capture rides a real
         typed-pointer cell (`T *_cell_x`): deref is an ordinary lvalue, so both
         reads and reassignments work with no (sp_int)(uintptr_t) cast, and the
         existing cell scan marks the referent. Int / bool stay direct in an
         sp_int cell; float / poly have native cells above. */
      int ptr_cell = cell_is_typed_ptr(c, lv);
      /* a Symbol is int-represented (sp_sym), so it rides the sp_int cell */
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_SYMBOL &&
          lv->type != TY_UNKNOWN && !ptr_cell)
        unsupported(c, s->def_node, "closure capturing a non-integer variable (later slice)");
      if (ptr_cell) {
        const char *cell_scan = cell_scan_fn(lv->type);
        buf_puts(b, "    "); emit_ctype(c, lv->type, b);
        buf_printf(b, " *_cell_%s = (", lv->name); emit_ctype(c, lv->type, b);
        buf_puts(b, " *)sp_gc_alloc(sizeof("); emit_ctype(c, lv->type, b);
        buf_printf(b, "), NULL, %s);\n", cell_scan);
        buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
        if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
        else buf_printf(b, "    *_cell_%s = NULL;\n", lv->name);
        return;
      }
      buf_printf(b, "    sp_int *_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, NULL);\n", lv->name);
      buf_printf(b, "    SP_GC_ROOT(_cell_%s);\n", lv->name);
      if (lv->is_param) buf_printf(b, "    *_cell_%s = lv_%s;\n", lv->name, lv->name);
      else buf_printf(b, "    *_cell_%s = 0;\n", lv->name);
      return;
}

void emit_scope_decls(Compiler *c, Scope *s, Buf *b) {
  int si = (int)(s - c->scopes);
  int has_begin = scope_has_begin(c, si);
  /* $~ and the $1.. globals derived from it are frame-local in Ruby: a match
     inside this method must not outlive it. The cleanup attribute puts the
     caller's registers back on every ordinary exit, early returns included. */
  if (s->name && s->def_node >= 0 && scope_performs_match(c, si))
    buf_puts(b, "    sp_re_frame _sp_rf __attribute__((cleanup(sp_re_frame_pop)));"
                " sp_re_frame_push(&_sp_rf);\n");
  /* Take the name this call spelled, and clear the channel so a call that did
     not write it (or a later nested one) cannot be mistaken for ours (#3729). */
  if (s->name && s->def_node >= 0 && scope_reads_callee(c, si))
    buf_puts(b, "    const char *_sp_cal = sp_callee_name; sp_callee_name = NULL; (void)_sp_cal;\n");
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
    if (lv->is_cell) { emit_cell_decl(c, s, lv, b); continue; }
    if (lv->is_param) {
      /* A poly param is an sp_RbVal by value: root through the tagged
         RBVAL form so the collector reads the boxed pointer, not the
         struct's first word (the tag). */
      if (lv->type == TY_POLY) buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", lv->name);
      else if (lv->type == TY_STR_RANGE) {   /* two GC strings by value; see emit_local_decl */
        buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.first);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.last);\n", lv->name);
      }
      else if (lv->type == TY_STRING) buf_printf(b, "    SP_GC_ROOT_STR(lv_%s);\n", lv->name);   /* see emit_local_decl */
      else if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
    }
    else {
      /* A BLOCK parameter the analyzer never typed still needs storage: the
         loop emitter binds it, and an empty literal receiver leaves no element
         type to infer, so `[].each_with_index { |x, i| }` referenced an
         undeclared lv_x (#3853). A boxed slot is what the binding writes. */
      if (lv->type == TY_UNKNOWN && lv->is_block_param) lv->type = TY_POLY;
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

/* Does this class method's body read the class it was called ON? A class
   method inherited by a subclass runs with `self` = that subclass -- CRuby's
   `def self.bench_name; self.name; end` answers the subclass's name -- so the
   receiving class has to reach the body. It rides a leading `sp_Class _sp_cls`
   parameter, added only where it can matter: a class with no descendant can
   only ever be its own receiver, and a body that never mentions self does not
   care. */
static int8_t *g_cm_selfcls = NULL;
static int g_cm_selfcls_n = -1;
int cmethod_takes_self_cls(Compiler *c, int si) {
  if (si < 0 || si >= c->nscopes) return 0;
  Scope *s = &c->scopes[si];
  if (!s->is_cmethod || s->class_id < 0 || s->body < 0) return 0;
  if (g_cm_selfcls_n != c->nscopes) {
    free(g_cm_selfcls);
    g_cm_selfcls = (int8_t *)calloc((size_t)c->nscopes, 1);
    g_cm_selfcls_n = c->nscopes;
    if (!g_cm_selfcls) { g_cm_selfcls_n = -1; return 0; }
    for (int k = 0; k < c->nscopes; k++) g_cm_selfcls[k] = -1;
  }
  if (!g_cm_selfcls) return 0;
  if (g_cm_selfcls[si] >= 0) return g_cm_selfcls[si];
  int ans = 0, has_desc = 0;
  for (int k = 0; k < c->nclasses && !has_desc; k++)
    if (k != s->class_id && is_descendant(c, k, s->class_id)) has_desc = 1;
  if (has_desc) {
    for (int nid = 0; nid < c->nt->count && !ans; nid++) {
      if (c->nscope[nid] != si) continue;
      if (nt_kind(c->nt, nid) == NK_SelfNode) { ans = 1; break; }
      const char *ty = nt_type(c->nt, nid);
      if (ty && sp_streq(ty, "CallNode") && nt_ref(c->nt, nid, "receiver") < 0) {
        const char *nm = nt_str(c->nt, nid, "name");
        if (nm && sp_streq(nm, "name")) ans = 1;
      }
    }
  }
  g_cm_selfcls[si] = (int8_t)ans;
  return ans;
}

/* Emit the receiving-class argument for such a call: the class the call names,
   which every call site knows statically (a constant receiver, or one arm of a
   cls_id cascade). Returns the separator the rest of the arguments need. */
const char *emit_cmethod_self_cls_arg(Compiler *c, int mi, int recv_cls, Buf *b) {
  if (!cmethod_takes_self_cls(c, mi)) return "";
  buf_printf(b, "((sp_Class){%d, NULL})", recv_cls >= 0 ? recv_cls : c->scopes[mi].class_id);
  return ", ";
}

/* The mangled C name: sp_<name> for free functions, sp_<Class>_<name>
   for instance methods. */
void emit_method_cname(Compiler *c, Scope *s, Buf *b) {
  if (s->class_id >= 0 && s->is_cmethod)
    buf_printf(b, "sp_%s_s_%s", c->classes[s->class_id].c_name, mc(s->name));
  else if (s->class_id >= 0)
    buf_printf(b, "sp_%s_%s", mc_reopen_cls(c, s->class_id, s->name), mc(s->name));
  else
    buf_printf(b, "sp_%s", mc_top(c, s->name));
}

/* A poly each-receiver can be a user object at runtime (e.g. a Set operand
   whose parameter also sees Array arguments at another call site, so it
   widened to poly). The arr_len/each_elem lowering only understands builtin
   containers -- normalize such an object through its 0-arg #to_a (when the
   class defines one returning a concrete array) so the loop iterates its
   elements. `tv` names an already-rooted sp_RbVal temp; emits nothing when
   no instantiated class qualifies. */
/* The collector a class's own #each is driven with when it has no #to_a: one
   function per program, pushing each yielded value into the PolyArray its cap
   points at. A yield of several values becomes one boxed array, which is what
   `to_a` answers for a Hash-like each. */
static int g_iter_collect_emitted = 0;
static void emit_iter_collect_proc(void) {
  if (g_iter_collect_emitted) return;
  g_iter_collect_emitted = 1;
  buf_puts(&g_proc_protos, "static sp_int _sp_iter_collect(void *cap, sp_int argc, sp_int *args);\n");
  buf_puts(&g_procs,
    "static sp_int _sp_iter_collect(void *cap, sp_int argc, sp_int *args) {\n"
    "  (void)args;\n"
    "  sp_PolyArray *_a = (sp_PolyArray *)cap;\n"
    "  if (argc <= 1) sp_PolyArray_push(_a, argc > 0 ? _sp_proc_poly_args[0] : sp_box_nil());\n"
    "  else {\n"
    "    sp_PolyArray *_p = sp_PolyArray_new(); SP_GC_ROOT(_p);\n"
    "    for (sp_int _i = 0; _i < argc && _i < 16; _i++) sp_PolyArray_push(_p, _sp_proc_poly_args[_i]);\n"
    "    sp_PolyArray_push(_a, sp_box_poly_array(_p));\n"
    "  }\n"
    "  _sp_proc_poly_ret = sp_box_nil();\n"
    "  return 0;\n}\n");
}

/* The class's #each in a form that takes a real block, or -1. A `def each(&b)`
   already does; a `def each; ...yield...; end` has a proc-form clone (#3399). */
static int iter_each_proc_form(Compiler *c, int k) {
  int mi = comp_method_in_chain(c, k, "each", NULL);
  if (mi < 0 || c->scopes[mi].is_cmethod || c->scopes[mi].nrequired != 0) return -1;
  int pf = scope_proc_form_of(c, mi);
  if (pf < 0 && c->scopes[mi].blk_param && c->scopes[mi].blk_param[0] && !c->scopes[mi].yields)
    pf = mi;
  if (pf < 0) return -1;
  /* The collector is the ONLY argument this can pass, so an #each that takes
     anything else of its own (StringIO#each(sep), with a separator) is not one
     the normalization can drive. */
  if (c->scopes[pf].nparams != 0 || c->scopes[pf].rest_idx >= 0) return -1;
  return pf;
}

/* The class a pointer to class k is handed to method mi as: the method's
   own class when it is k or an ancestor of k (an inherited method's C
   function takes its definer's type), k otherwise (a mixed-in body). */
static int iter_self_class(Compiler *c, int k, int mi) {
  int d = c->scopes[mi].class_id;
  for (int x = k; x >= 0; x = c->classes[x].parent) if (x == d) return d;
  return k;
}

/* The receiver argument for class `k`'s arm, in the form method `mi` takes
   self: the boxed value itself for a method of Object, Array or Numeric
   (emit_method_signature gives those `sp_RbVal self`), a pointer cast to
   iter_self_class's answer otherwise. */
static void emit_iter_recv(Compiler *c, int k, int mi, int tv, Buf *b) {
  const char *cn = c->classes[iter_self_class(c, k, mi)].c_name;
  if (sp_streq(cn, "Object") || sp_streq(cn, "Array") || sp_streq(cn, "Numeric"))
    buf_printf(b, "_t%d", tv);
  else
    buf_printf(b, "(sp_%s *)_t%d.v.p", cn, tv);
}

void emit_poly_iter_obj_normalize(Compiler *c, int tv, Buf *b) {
  Buf arms; memset(&arms, 0, sizeof arms);
  for (int k = 0; k < c->nclasses; k++) {
    /* a never-instantiated class can't be the runtime class (#1608) */
    if (!c->classes[k].instantiated) continue;
    int mi = comp_method_in_chain(c, k, "to_a", NULL);
    if (mi < 0 || c->scopes[mi].nrequired != 0 || c->scopes[mi].is_cmethod) {
      /* No #to_a, but the class defines #each -- the ordinary way to write an
         enumerable, forwarding the block on. Drive it with a collector and walk
         what it yielded; without this the lowering walked the object AS A
         CONTAINER and found nothing, so the loop body never ran (#4088). */
      int pf = iter_each_proc_form(c, k);
      if (pf < 0) continue;
      emit_iter_collect_proc();
      buf_printf(&arms, " case %d: { sp_PolyArray *_ia%d = sp_PolyArray_new(); SP_GC_ROOT(_ia%d);"
                        " sp_Proc *_ip%d = sp_proc_new_meta((void *)_sp_iter_collect, (void *)_ia%d,"
                        " sp_hashproc_cap_scan, 1, FALSE, 1, NULL, NULL); (void)",
                 k, tv, tv, tv, tv);
      emit_method_cname(c, &c->scopes[pf], &arms);
      /* the receiver as the class that DEFINES #each: a subclass inherits it,
         and handing the subclass's own pointer type to the definer's function
         is a C type error (lobsters: Nokogiri's Document classes under Node) */
      buf_puts(&arms, "(");
      emit_iter_recv(c, k, pf, tv, &arms);
      buf_printf(&arms, ", _ip%d); _t%d = sp_box_poly_array(_ia%d); break; }", tv, tv, tv);
      continue;
    }
    TyKind ret = (TyKind)c->scopes[mi].ret;
    const char *box = ret == TY_POLY_ARRAY  ? "sp_box_poly_array"
                    : ret == TY_INT_ARRAY   ? "sp_box_int_array"
                    : ret == TY_STR_ARRAY   ? "sp_box_str_array"
                    : ret == TY_FLOAT_ARRAY ? "sp_box_float_array" : NULL;
    if (!box) continue;
    buf_printf(&arms, " case %d: _t%d = %s(", k, tv, box);
    emit_method_cname(c, &c->scopes[mi], &arms);
    buf_puts(&arms, "(");
    emit_iter_recv(c, k, mi, tv, &arms);
    buf_puts(&arms, ")); break;");
  }
  if (arms.p && arms.p[0])
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ && _t%d.cls_id >= 0) switch (_t%d.cls_id) {%s }\n",
               tv, tv, tv, arms.p);
  free(arms.p);
}

/* The result type of a block literal: its body's last statement. */
static TyKind pf_block_result_ty(Compiler *c, int blk) {
  if (blk < 0) return TY_UNKNOWN;
  int body = nt_ref(c->nt, blk, "body");
  int n = 0; const int *bb = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
  return n > 0 ? comp_ntype(c, bb[n - 1]) : TY_NIL;
}
/* The type the method's own yield nodes were compiled against. */
static TyKind pf_yield_ty(Compiler *c, int id, int *found) {
  if (id < 0) return TY_UNKNOWN;
  if (nt_kind(c->nt, id) == NK_YieldNode) { *found = 1; return comp_ntype(c, id); }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    TyKind t = pf_yield_ty(c, nt_ref_at(c->nt, id, i), found);
    if (*found) return t;
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int j = 0; j < n; j++) {
      TyKind t = pf_yield_ty(c, ids[j], found);
      if (*found) return t;
    }
  }
  return TY_UNKNOWN;
}
/* Should this method carry an `inline` hint?

   The C compiler sizes a function by the machine code it has after our runtime
   helpers are expanded into it, and that is nothing like the size of the Ruby
   it came from: PPU#render_pixel is fifteen lines and compiles to 1.4KB,
   because every array read carries its bounds check and every unboxing its
   conversion arms. So the inliner declines exactly the methods a Ruby program
   most wants inlined -- small ones called from a hot loop -- and optcarrot
   pays a call and a frame per pixel for one.

   The emitter knows the size the C compiler cannot see. A method with a small
   body and few call sites gets the hint, which raises the C compiler's own
   limit for it without forcing anything: it still decides. Worth 3-4% on
   optcarrot for 0.6% of binary size and no measurable compile time. */
static int  g_mih_limit_override = 0;  /* the force pass raises the body budget */
static int  g_mih_callers_override = 0;
static int *g_mih_nodes = NULL;      /* AST nodes per scope */
static int *g_mih_calls = NULL;      /* call sites naming the scope's method */
static int  g_mih_nscopes = 0;
static const NodeTable *g_mih_nt = NULL;
static int  g_mih_ntcount = 0;
/* the per-name call count, built once: counting the calls by walking every
   CallNode per method was a scan of the whole table per scope, and on a
   program with thousands of methods (campfire) that was 80% of the front
   end (#4662) */
static unsigned mih_hash(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u; return h; }
/* name -> first scope of that name; scopes sharing a name chain through
   next. Kept with the counts, so a call node appended later is counted
   without rebuilding it. */
static int *g_mih_head = NULL, *g_mih_next = NULL, g_mih_cap = 0;
static void mih_count_call(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm) return;
  unsigned h = mih_hash(nm) & (unsigned)(g_mih_cap - 1);
  while (g_mih_head[h] >= 0 && !sp_streq(c->scopes[g_mih_head[h]].name, nm))
    h = (h + 1) & (unsigned)(g_mih_cap - 1);
  for (int si = g_mih_head[h]; si >= 0; si = g_mih_next[si]) g_mih_calls[si]++;
}
static void mih_count_calls(Compiler *c) {
  const NodeTable *nt = c->nt;
  int ns = c->nscopes;
  free(g_mih_calls);
  g_mih_calls = (int *)calloc((size_t)(ns > 0 ? ns : 1), sizeof(int));
  if (!g_mih_calls) return;
  int cap = 1; while (cap < ns * 2 + 8) cap <<= 1;
  free(g_mih_head); free(g_mih_next);
  int *head = g_mih_head = (int *)malloc(sizeof(int) * (size_t)cap);
  int *next = g_mih_next = (int *)malloc(sizeof(int) * (size_t)(ns > 0 ? ns : 1));
  g_mih_cap = cap;
  if (!head || !next) { free(g_mih_calls); g_mih_calls = NULL; return; }
  for (int i = 0; i < cap; i++) head[i] = -1;
  for (int si = 0; si < ns; si++) {
    next[si] = -1;
    const char *nm = c->scopes[si].name;
    if (!nm) continue;
    unsigned h = mih_hash(nm) & (unsigned)(cap - 1);
    while (head[h] >= 0 && !sp_streq(c->scopes[head[h]].name, nm)) h = (h + 1) & (unsigned)(cap - 1);
    if (head[h] < 0) head[h] = si;
    else { int t = head[h]; while (next[t] >= 0) t = next[t]; next[t] = si; }
  }
  NT_FOREACH_KIND(nt, NK_CallNode, id) mih_count_call(c, id);
}
static int method_inline_hint(Compiler *c, Scope *s) {
  if (g_debug) return 0;                      /* debug builds want real frames */
  if (!s->name || s->body < 0 || s->yields) return 0;
  const NodeTable *nt = c->nt;
  /* Codegen appends a few nodes as it goes, and each append used to rebuild
     both counts from the whole table: 349 rebuilds of ~277K nodes on
     lobsters. Nodes are only ever appended, so counting the new ones gives
     the same numbers. */
  if (g_mih_nt == nt && g_mih_nscopes == c->nscopes && g_mih_nodes && g_mih_calls &&
      g_mih_ntcount < nt->count) {
    for (int id = g_mih_ntcount; id < nt->count; id++) {
      int sc = c->nscope[id];
      if (sc >= 0 && sc < c->nscopes) g_mih_nodes[sc]++;
      if (nt_kind(nt, id) == NK_CallNode) mih_count_call(c, id);
    }
    g_mih_ntcount = nt->count;
  }
  if (g_mih_nt != nt || g_mih_ntcount != nt->count || g_mih_nscopes != c->nscopes) {
    free(g_mih_nodes);
    g_mih_nodes = (int *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(int));
    g_mih_nscopes = c->nscopes; g_mih_nt = nt; g_mih_ntcount = nt->count;
    if (!g_mih_nodes) return 0;
    for (int id = 0; id < nt->count; id++) {
      int sc = c->nscope[id];
      if (sc >= 0 && sc < c->nscopes) g_mih_nodes[sc]++;
    }
    mih_count_calls(c);
  }
  if (!g_mih_nodes || !g_mih_calls) return 0;
  int si = (int)(s - c->scopes);
  if (si < 0 || si >= g_mih_nscopes) return 0;
  int limit = 90;
  /* read once: this runs per method, and getenv walks the environment (#4966) */
  static int env_rd; static const char *env_nodes;
  if (!env_rd) { env_nodes = getenv("SPINEL_INLINE_NODES"); env_rd = 1; }
  { const char *e = env_nodes; if (e && *e) limit = atoi(e); }
  if (g_mih_limit_override > 0) limit = g_mih_limit_override;
  if (limit <= 0) return 0;
  if (g_mih_nodes[si] > limit) return 0;
  /* few enough call sites that expanding it cannot multiply the program */
  int callmax = 12;
  if (g_mih_callers_override > 0) callmax = g_mih_callers_override;
  int calls = g_mih_calls[si];
  if (calls > callmax) return 0;
  return calls > 0;
}


/* ---- forced inlining of small leaf methods -------------------------------
   The `inline` hint above lets the C compiler decide, and for the method that
   matters most it decides no: PPU#render_pixel is fifteen lines of Ruby and
   1.4KB of machine code, so its eight call sites in the PPU loop keep paying a
   call and a frame per pixel. Forcing it is worth 6% on optcarrot.

   always_inline is not a hint, though: it is a hard C error on a recursion
   cycle and on a body that uses setjmp. So the set is computed rather than
   guessed. A method qualifies when it is already hint-eligible, contains
   nothing the emitter lowers through setjmp (a rescue, a block -- which is how
   break, catch, StopIteration and a proc return all arrive -- a yield or a
   super), and does not lie on a cycle in the call graph restricted to the
   other qualifying methods. A cycle needs every member to be forced, so
   dropping every member of every cycle leaves a set that cannot recurse. */
static unsigned char *g_fi_state = NULL;   /* 0 unknown, 1 forced, 2 not */
static int g_fi_nscopes = 0;
static const NodeTable *g_fi_nt = NULL;
static int g_fi_ntcount = 0;

/* The scopes by name, each chain in ascending scope order: fi_callees is asked
   for every call of every body the cycle check walks, recursively, and a scan
   of all the scopes per ask made that check most of the C generation of a
   large program (campfire: 60 s of 77). Built once the scopes stop changing
   (the forced-inline set is decided after analysis) and rebuilt if their
   count moves. */
static int *g_fi_nm_head, *g_fi_nm_next, *g_fi_nm_tail;
static int g_fi_nm_nb, g_fi_nm_nscopes = -1;
static const Scope *g_fi_nm_scopes;

static unsigned fi_name_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h;
}

static int fi_first_scope_named(Compiler *c, const char *nm) {
  if (g_fi_nm_nscopes != c->nscopes || g_fi_nm_scopes != c->scopes) {
    free(g_fi_nm_head); free(g_fi_nm_next); free(g_fi_nm_tail);
    int nb = 64;
    while (nb < c->nscopes * 2) nb *= 2;
    g_fi_nm_head = (int *)malloc(sizeof(int) * (size_t)nb);
    g_fi_nm_tail = (int *)malloc(sizeof(int) * (size_t)nb);
    g_fi_nm_next = (int *)malloc(sizeof(int) * (size_t)(c->nscopes ? c->nscopes : 1));
    if (!g_fi_nm_head || !g_fi_nm_tail || !g_fi_nm_next) {
      free(g_fi_nm_head); free(g_fi_nm_next); free(g_fi_nm_tail);
      g_fi_nm_head = g_fi_nm_next = g_fi_nm_tail = NULL;
      g_fi_nm_nscopes = -1;
      return -2;   /* no index: the caller scans */
    }
    for (int b = 0; b < nb; b++) g_fi_nm_head[b] = g_fi_nm_tail[b] = -1;
    for (int si = 0; si < c->nscopes; si++) {
      g_fi_nm_next[si] = -1;
      const char *sn = c->scopes[si].name;
      if (!sn) continue;
      unsigned b = fi_name_hash(sn) & (unsigned)(nb - 1);
      if (g_fi_nm_tail[b] < 0) g_fi_nm_head[b] = si;
      else g_fi_nm_next[g_fi_nm_tail[b]] = si;
      g_fi_nm_tail[b] = si;
    }
    g_fi_nm_nb = nb;
    g_fi_nm_nscopes = c->nscopes;
    g_fi_nm_scopes = c->scopes;
  }
  return g_fi_nm_head[fi_name_hash(nm) & (unsigned)(g_fi_nm_nb - 1)];
}

/* Every user method a call could reach: by the receiver's class when it names
   one, by the bare name when the receiver is boxed or absent. */
static void fi_callees(Compiler *c, int callnode, int *out, int *n, int max) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, callnode, "name");
  if (!nm) return;
  int rcv = nt_ref(nt, callnode, "receiver");
  int cid = -1;
  if (rcv >= 0) {
    TyKind rt = comp_ntype(c, rcv);
    if (ty_is_object(rt)) cid = ty_object_class(rt);
    else if (rt != TY_POLY && rt != TY_UNKNOWN && rt != TY_CLASS) return;  /* builtin */
  }
  int first = fi_first_scope_named(c, nm);
  int indexed = first != -2;
  for (int si = indexed ? first : 0; si >= 0 && si < c->nscopes && *n < max;
       si = indexed ? g_fi_nm_next[si] : si + 1) {
    Scope *m = &c->scopes[si];
    if (!m->name || !sp_streq(m->name, nm)) continue;
    if (cid >= 0 && m->class_id != cid &&
        comp_method_in_chain(c, cid, nm, NULL) != si) continue;
    out[(*n)++] = si;
  }
}

/* Anything in this subtree that the emitter lowers through setjmp, or that
   reaches a user method through a route fi_callees cannot enumerate. */
static int fi_body_unforceable(Compiler *c, int id, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0) return 0;              /* an absent optional child is nothing */
  if (depth > 64) return 1;          /* deeper than we walked: assume it is */
  switch (nt_kind(nt, id)) {
    case NK_BlockNode: case NK_LambdaNode: case NK_BlockArgumentNode:
    case NK_BeginNode: case NK_RescueNode: case NK_RescueModifierNode:
    case NK_RetryNode: case NK_RedoNode: case NK_BreakNode: case NK_NextNode:
    case NK_YieldNode: case NK_SuperNode: case NK_ForwardingSuperNode:
      return 1;
    default: break;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (fi_body_unforceable(c, nt_ref_at(nt, id, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (fi_body_unforceable(c, ids[j], depth + 1)) return 1;
  }
  return 0;
}

/* Set when the walk below stopped early. A truncated call list under-counts,
   and every user of it is deciding whether something FITS, so the callers
   treat a truncated answer as "does not fit" rather than trusting the short
   count (a 350-arm elsif chain nests one level per arm, #3913). */
static int g_fi_trunc;

/* Collect every call node in a method's body subtree. */
static void fi_collect_calls(Compiler *c, int id, int *out, int *n, int max, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0) return;
  if (depth > 4096 || *n >= max) { g_fi_trunc = 1; return; }
  if (nt_kind(nt, id) == NK_CallNode) out[(*n)++] = id;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) fi_collect_calls(c, nt_ref_at(nt, id, i), out, n, max, depth + 1);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int cn = 0; const int *ids = nt_arr_at(nt, id, i, &cn);
    for (int j = 0; j < cn; j++) fi_collect_calls(c, ids[j], out, n, max, depth + 1);
  }
}

static int fi_reaches(Compiler *c, int from, int target, unsigned char *seen,
                      unsigned char *cand, int depth);

/* The methods whose defaults one search has walked (see fi_defaults_reach) */
static unsigned char *g_fi_dseen;

/* The calls a call to `callee` puts at its own site through the parameter
   defaults it fills in, and in turn those of the methods such a default
   calls, which omit arguments of their own (`def f(a = k)`, `def k(b = g)`):
   every one of them is emitted in the calling body. A method reached so
   counts as that body's callee: its body is followed when it is forced
   inline. A default whose call list was cut short is assumed to reach. */
static int fi_defaults_reach(Compiler *c, int callee, int target, unsigned char *seen,
                             unsigned char *cand, int depth) {
  if (depth > 64) return 1;
  if (!g_fi_dseen || g_fi_dseen[callee]) return 0;
  g_fi_dseen[callee] = 1;
  Scope *cs = &c->scopes[callee];
  for (int p = 0; cs->pdefault && p < cs->nparams; p++) {
    if (cs->pdefault[p] < 0) continue;
    int dcalls[64]; int nd = 0;
    int sv_trunc = g_fi_trunc;
    g_fi_trunc = 0;
    fi_collect_calls(c, cs->pdefault[p], dcalls, &nd, 64, 0);
    int cut = g_fi_trunc;
    g_fi_trunc = sv_trunc;
    if (cut) return 1;
    for (int d = 0; d < nd; d++) {
      int dcal[32]; int n3 = 0;
      fi_callees(c, dcalls[d], dcal, &n3, 32);
      for (int k = 0; k < n3; k++) {
        if (fi_defaults_reach(c, dcal[k], target, seen, cand, depth + 1)) return 1;
        if (!cand[dcal[k]]) continue;
        if (dcal[k] == target) return 1;
        if (fi_reaches(c, dcal[k], target, seen, cand, depth + 1)) return 1;
      }
    }
  }
  return 0;
}

static int fi_reaches(Compiler *c, int from, int target, unsigned char *seen,
                      unsigned char *cand, int depth) {
  if (depth > 64) return 1;                      /* too deep: assume it does */
  if (seen[from]) return 0;
  seen[from] = 1;
  int calls[512]; int nc = 0;
  fi_collect_calls(c, c->scopes[from].body, calls, &nc, 512, 0);
  for (int i = 0; i < nc; i++) {
    int cal[32]; int n2 = 0;
    fi_callees(c, calls[i], cal, &n2, 32);
    for (int j = 0; j < n2; j++) {
      /* a callee's parameter defaults are emitted at this call site, so the
         calls inside them are this body's calls too */
      if (fi_defaults_reach(c, cal[j], target, seen, cand, depth + 1)) return 1;
      if (!cand[cal[j]]) continue;               /* not forced: no cycle through it */
      if (cal[j] == target) return 1;
      if (fi_reaches(c, cal[j], target, seen, cand, depth + 1)) return 1;
    }
  }
  return 0;
}

/* A body that runs its own loop is not a "small leaf": the call it saves is
   already negligible against the work inside, and absorbing a loop nest into a
   bigger function costs register allocation. Forcing those cost matmul 2x,
   nqueens 1.6x and sudoku 1.3x while buying nothing anywhere. */
static int fi_body_has_loop(Compiler *c, int id, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0 || depth > 400) return 0;
  NodeKind k = nt_kind(nt, id);
  if (k == NK_WhileNode || k == NK_UntilNode || k == NK_ForNode) return 1;
  /* an iteration block (`xs.each { }`) is a loop once the emitter fuses it */
  if (k == NK_CallNode) {
    int blk = nt_ref(nt, id, "block");
    const char *bt = blk >= 0 ? nt_type(nt, blk) : NULL;
    if (bt && sp_streq(bt, "BlockNode")) return 1;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (fi_body_has_loop(c, nt_ref_at(nt, id, i), depth + 1)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (fi_body_has_loop(c, ids[j], depth + 1)) return 1;
  }
  return 0;
}

/* True when the program can run user code on a fiber stack -- a Fiber, a green
   thread, or a generator Enumerator. SP_FIBER_STACK_SIZE is the ceiling only
   there; on the process stack the same frame has megabytes to sit in. */
int fi_fiber_stack_risk(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int i = 0; i < nt->count; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    if (sp_streq(ty, "ConstantReadNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && (sp_streq(nm, "Fiber") || sp_streq(nm, "Thread") || sp_streq(nm, "Enumerator") ||
                 sp_streq(nm, "Queue") || sp_streq(nm, "SizedQueue") || sp_streq(nm, "Mutex") ||
                 sp_streq(nm, "Monitor") || sp_streq(nm, "ConditionVariable")))
        return 1;
    }
    else if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      /* an external enumerator's body runs on a generator fiber too */
      if (nm && (sp_streq(nm, "to_enum") || sp_streq(nm, "enum_for"))) return 1;
    }
  }
  return 0;
}

/* An estimate of the C frame a body contributes once it is absorbed into its
   caller. A local costs its own slot, and one that carries a GC root costs the
   padded cleanup int beside it as well; because the root takes the local's
   ADDRESS, the C compiler cannot share either slot with another absorbed body.
   8 and 16 bytes respectively, measured on gcc/amd64. */
static long fi_own_frame(Compiler *c, int si) {
  if (si < 0 || si >= c->nscopes) return 0;
  Scope *m = &c->scopes[si];
  long f = 32;
  for (int i = 0; i < m->nlocals; i++) {
    TyKind t = m->locals[i].type;
    /* Only a local carrying a GC root costs stack: SP_GC_ROOT takes its
       ADDRESS, which pins it to a slot the C compiler cannot share with
       another absorbed body, and adds the padded cleanup int beside it. A
       scalar stays in a register or shares a spill slot and costs nothing
       measurable -- counting those put optcarrot's estimate ten times over
       its real frame and trimmed forcing it wants. */
    int rooted = (t == TY_POLY || t == TY_STRING || t == TY_STRBUF || t == TY_PROC ||
                  ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || ty_is_object(t));
    if (rooted) f += 16;
  }
  return f;
}

static void fi_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  free(g_fi_state);
  g_fi_state = (unsigned char *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
  g_fi_nscopes = c->nscopes; g_fi_nt = nt; g_fi_ntcount = nt->count;
  if (!g_fi_state) return;
  unsigned char *cand = (unsigned char *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), 1);
  if (!cand) return;
  int ncand = 0;
  for (int si = 0; si < c->nscopes; si++) {
    Scope *m = &c->scopes[si];
    /* Forcing is about the call, not the code size, so it takes its own
       (larger) body budget: render_pixel is past the hint's, and it is
       exactly the method worth forcing. */
    int sv = g_mih_limit_override, sv2 = g_mih_callers_override;
    { static int env_rd; static const char *env_fn, *env_fc;
      if (!env_rd) { env_fn = getenv("SPINEL_INLINE_FORCE_NODES"); env_fc = getenv("SPINEL_INLINE_FORCE_CALLERS"); env_rd = 1; }
      const char *e = env_fn;
      g_mih_limit_override = (e && *e) ? atoi(e) : 250;
      const char *e2 = env_fc;
      g_mih_callers_override = (e2 && *e2) ? atoi(e2) : 12; }
    int ok = (method_inline_hint(c, m) == 1);
    g_mih_limit_override = sv; g_mih_callers_override = sv2;
    if (!ok) continue;
    if (fi_body_unforceable(c, m->body, 0)) continue;
    if (fi_body_has_loop(c, m->body, 0)) continue;
    /* A default moved into a method of its own (#4900) calls back into the
       method whose default it is, and that call emits the default -- a call
       to the helper -- in place: the helper calls itself in C through a
       route the call graph here does not follow. gcc refuses always_inline
       on it where clang drops the attribute quietly. */
    if (m->def_node >= 0 && nt_int(nt, m->def_node, "default_helper", 0)) continue;
    cand[si] = 1; ncand++;
  }
  /* A whole-program cycle search is O(candidates * edges); on a program with
     thousands of small methods that is not worth the compile time, and the
     hint alone still applies. */
  if (ncand > 0 && ncand <= 2048) {
    unsigned char *seen = (unsigned char *)malloc((size_t)c->nscopes);
    g_fi_dseen = (unsigned char *)malloc((size_t)c->nscopes);
    for (int si = 0; si < c->nscopes && seen && g_fi_dseen; si++) {
      if (!cand[si]) continue;
      memset(seen, 0, (size_t)c->nscopes);
      memset(g_fi_dseen, 0, (size_t)c->nscopes);
      if (fi_reaches(c, si, si, seen, cand, 0)) cand[si] = 0;   /* on a cycle */
    }
    free(seen);
    free(g_fi_dseen); g_fi_dseen = NULL;
  }
  else {
    for (int si = 0; si < c->nscopes; si++) cand[si] = 0;
  }
  /* Bound the DEPTH of the forced set, not just its size. always_inline
     collapses a whole path through it into one C frame, and every GC-visible
     local in that frame is address-taken (SP_GC_ROOT pushes &v), so the C
     compiler has little room to share slots between the absorbed bodies. A
     green thread runs on SP_FIBER_STACK_SIZE bytes, which a few hundred
     absorbed bodies overran straight into the guard page (#3913). Clearing the
     TOP of a too-long chain shortens every path through it while leaving the
     small leaves -- the ones worth forcing -- alone. */
  {
    const char *de = getenv("SPINEL_INLINE_FORCE_DEPTH");
    int dlimit = (de && *de) ? atoi(de) : 6;
    if (dlimit > 0 && ncand > 0 && ncand <= 2048) {
      int *dep = (int *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(int));
      if (dep) {
        /* iterate to a fixpoint: depth is 1 + the deepest forced callee */
        /* depth is monotone across rounds, so dlimit+1 of them is enough to
           separate "within the limit" from "deeper than it" */
        for (int round = 0; round <= dlimit; round++) {
          int changed = 0;
          for (int si = 0; si < c->nscopes; si++) {
            if (!cand[si]) { dep[si] = 0; continue; }
            int calls[512]; int nc = 0;
            fi_collect_calls(c, c->scopes[si].body, calls, &nc, 512, 0);
            int deepest = 0;
            for (int i = 0; i < nc; i++) {
              int cal[32]; int n2 = 0;
              fi_callees(c, calls[i], cal, &n2, 32);
              for (int j = 0; j < n2; j++)
                if (cand[cal[j]] && dep[cal[j]] > deepest) deepest = dep[cal[j]];
            }
            if (dep[si] != deepest + 1) { dep[si] = deepest + 1; changed = 1; }
          }
          if (!changed) break;
        }
        for (int si = 0; si < c->nscopes; si++)
          if (cand[si] && dep[si] > dlimit) cand[si] = 0;
        free(dep);
      }
    }
  }
  /* Bound the BREADTH too. Mutually exclusive calls reuse one stack region --
     each returns before the next -- but absorbed they become siblings in one
     frame, and the address-taken roots stop the C compiler sharing their slots,
     so the frame becomes the SUM over the arms rather than the max. A 350-arm
     router one level deep overran the 64K fiber stack with every arm forced,
     which the depth bound cannot see (#3913). Charge each forced candidate the
     frame it contributes, accumulate that at every call site, and stop forcing
     once a caller's absorbed total passes the budget. */
  {
    const char *fe = getenv("SPINEL_INLINE_FORCE_FRAME");
    /* 64 KB, the fiber stack's size when this budget was set; the stack is
       256 KB now (#4496) and the budget stays, since what it bounds is how
       much the forcing may add to a frame, not the frame itself */
    long fbudget = (fe && *fe) ? atol(fe) : 64 * 1024;
    int report = getenv("SPINEL_INLINE_FORCE_REPORT") != NULL;
    /* The budget exists for SP_FIBER_STACK_SIZE. A program that runs no user
       code on a fiber stack has the process stack (megabytes) to spend, and
       bounding it there costs real throughput -- optcarrot's hot path absorbs
       a frame far past this budget and wants to. */
    if (!fi_fiber_stack_risk(c)) fbudget = 0;
    if (fbudget > 0 && ncand > 0 && ncand <= 2048) {
      long *cost = (long *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(long));
      enum { FI_CALLS_MAX = 8192 };
      int *calls = (int *)malloc(sizeof(int) * FI_CALLS_MAX);
      int cal[32];
      if (!calls) { free(cost); cost = NULL; }
      if (cost) for (int round = 0; round < 8; round++) {
        /* what each forced candidate carries: its own frame plus everything it
           absorbs in turn (acyclic -- the cycle pass above cleared the rest) */
        for (int it = 0; it < 32; it++) {
          int ch = 0;
          for (int si = 0; si < c->nscopes; si++) {
            long v = 0;
            if (cand[si]) {
              v = fi_own_frame(c, si);
              int nc = 0;
              g_fi_trunc = 0;
              fi_collect_calls(c, c->scopes[si].body, calls, &nc, FI_CALLS_MAX, 0);
              for (int i = 0; i < nc; i++) {
                int n2 = 0;
                fi_callees(c, calls[i], cal, &n2, 32);
                for (int j = 0; j < n2; j++) if (cand[cal[j]]) v += cost[cal[j]];
              }
            }
            if (cost[si] != v) { cost[si] = v; ch = 1; }
          }
          if (!ch) break;
        }
        /* every scope, forced or not, has to fit: the caller that absorbs a
           wide fan-out is usually too big to be a candidate itself */
        int trimmed = 0;
        for (int si = 0; si < c->nscopes; si++) {
          long tot = fi_own_frame(c, si);
          int nc = 0;
          g_fi_trunc = 0;
          fi_collect_calls(c, c->scopes[si].body, calls, &nc, FI_CALLS_MAX, 0);
          /* a truncated list cannot say the total fits: stop forcing here */
          int over = g_fi_trunc;
          for (int i = 0; i < nc; i++) {
            int n2 = 0;
            fi_callees(c, calls[i], cal, &n2, 32);
            for (int j = 0; j < n2; j++) {
              if (!cand[cal[j]]) continue;
              if (!over && tot + cost[cal[j]] <= fbudget) { tot += cost[cal[j]]; continue; }
              /* past the budget: this and every later arm keeps its call */
              over = 1; cand[cal[j]] = 0; trimmed = 1;
            }
          }
          if (over && report)
            fprintf(stderr, "inline-force: %s absorbed past %ld bytes, trimmed\n",
                    c->scopes[si].name ? c->scopes[si].name : "(top)", fbudget);
        }
        if (!trimmed) break;
      }
      free(calls);
      free(cost);
    }
  }
  for (int si = 0; si < c->nscopes; si++) g_fi_state[si] = cand[si] ? 1 : 2;
  free(cand);
}

static int method_inline_force(Compiler *c, Scope *s) {
  static int env_rd; static const char *env_force;
  if (!env_rd) { env_force = getenv("SPINEL_INLINE_FORCE"); env_rd = 1; }
  { const char *e = env_force;
    if (e && *e) { if (*e == '0') return 0; }
    else if (!g_inline_hot) return 0; }
  if (g_debug) return 0;
  const NodeTable *nt = c->nt;
  if (g_fi_nt != nt || g_fi_ntcount != nt->count || g_fi_nscopes != c->nscopes || !g_fi_state)
    fi_build(c);
  if (!g_fi_state) return 0;
  int si = (int)(s - c->scopes);
  if (si < 0 || si >= g_fi_nscopes) return 0;
  return g_fi_state[si] == 1;
}

/* The cls_id a fresh instance is stamped with. A synthesized singleton subclass
   starts as its PARENT: the `extend` / `def obj.m` that created the subclass
   has not run yet, and in Ruby the override takes effect from there rather than
   from the object's construction (#4084). That statement flips the id, and the
   subclass's own methods check it in their prologue. */
static int ctor_cls_id(Compiler *c, int cid) {
  if (cid < 0 || cid >= c->nclasses) return cid;
  return c->classes[cid].is_singleton_of ? c->classes[cid].is_singleton_of - 1 : cid;
}

/* Two C slots the delegation can move a value between: identical, or one side
   boxed. emit_boxed_text / emit_unbox_text do the conversion. */
static int sg_slot_convertible(TyKind from, TyKind to) {
  if (from == to) return 1;
  if (from == TY_UNKNOWN || to == TY_UNKNOWN) return 0;
  return from == TY_POLY || to == TY_POLY;
}

/* The parent method a singleton subclass's `name` stands in front of, or -1.
   Only usable as a delegate when the C signature matches: same parameter count
   and types, and a return the caller's slot can take. A mismatch keeps today's
   behaviour rather than breaking the build. */
static int sg_delegate_scope(Compiler *c, Scope *s) {
  if (!s || s->class_id < 0 || s->is_cmethod || !s->name) return -1;
  int sub = s->class_id;
  if (!c->classes[sub].is_singleton_of) return -1;
  if (c->classes[sub].is_value_type) return -1;
  /* the immediate parent LINK, not the original class: a binding can carry a
     chain of them (one per extended module) and each body stands in front of
     the next, which is what makes `super` stack */
  int par = c->classes[sub].parent;
  int pm = comp_method_in_chain(c, par, s->name, NULL);
  if (pm < 0 || pm >= c->nscopes) return -1;
  Scope *ps = &c->scopes[pm];
  if (ps->nparams != s->nparams) return -1;
  if ((ps->blk_param && ps->blk_param[0]) != (s->blk_param && s->blk_param[0])) return -1;
  if (method_is_void(ps) != method_is_void(s)) return -1;
  /* The parent's own signature can be WIDER than the override's -- the
     singleton bodies flowing through it are what widened it -- so accept a
     poly on either side of any slot and convert at the boundary. Anything else
     keeps today's behaviour rather than emitting C that does not compile. */
  if (!sg_slot_convertible(s->ret, ps->ret)) return -1;
  for (int i = 0; i < s->nparams; i++) {
    LocalVar *a = scope_local(s, s->pnames[i]);
    LocalVar *bp = scope_local(ps, ps->pnames[i]);
    if (!a || !bp || a->byref_out || bp->byref_out) return -1;
    if (!sg_slot_convertible(a->type, bp->type)) return -1;
  }
  return pm;
}

void emit_method_signature(Compiler *c, Scope *s, Buf *b) {
  /* In a debug build, give instance/class methods external linkage so
     -rdynamic exposes sp_<Class>_<method> to backtrace_symbols and the
     frames demangle (Exception#backtrace / Kernel#caller). Toplevel methods
     keep `static` -- a bare sp_<name> could collide with a runtime helper. */
  const char *stor = ((g_debug && s->class_id >= 0) || s->is_ext_entry) ? "" : "static ";
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
  const char *ihint = s->is_ext_entry ? ""   /* exported: no inline linkage */
                    : method_inline_force(c, s) ? "inline __attribute__((always_inline)) "
                    : (method_inline_hint(c, s) ? "inline " : "");
  if (method_is_void(s)) { buf_puts(b, stor); buf_puts(b, ihint); buf_puts(b, unused); buf_puts(b, "void "); }
  else { buf_puts(b, stor); buf_puts(b, ihint); buf_puts(b, unused); emit_ctype(c, s->ret, b); buf_puts(b, " "); }
  emit_method_cname(c, s, b);
  buf_puts(b, "(");
  int wrote = 0;
  if (cmethod_takes_self_cls(c, (int)(s - c->scopes))) {
    buf_puts(b, "sp_Class _sp_cls");
    wrote = 1;
  }
  if (s->class_id >= 0 && !s->is_cmethod) {
    const char *cn = c->classes[s->class_id].c_name;
    if (sp_streq(cn, "String"))       { buf_puts(b, "const char *self"); }
    else if (sp_streq(cn, "Integer")) { buf_puts(b, "sp_int self"); }
    else if (sp_streq(cn, "Float"))   { buf_puts(b, "double self"); }
    else if (sp_streq(cn, "Symbol"))  { buf_puts(b, "sp_int self"); }
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
  /* &block param that escapes (not inlined): passes the block as sp_Proc *.
     const: a method's block parameter is read-only (check_blk_param_writes
     refuses an assignment), so a write that got past that check stops the C
     build instead of running. */
  if (s->blk_param && s->blk_param[0] && !s->yields) {
    if (wrote++) buf_puts(b, ", ");
    buf_printf(b, "sp_Proc *const lv_%s", s->blk_param);
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

/* SP_GC_SAVE restores the root-stack depth on the way out. A function that
   registers no root of its own moved nothing to restore, so the save is dead
   weight -- and it is what stops the C compiler from leaving such a function
   frameless. It has to be emitted before the body is known, so it is taken back
   here rather than predicted.

   Kept whenever the body catches a longjmp (`setjmp`) or touches the depth
   directly (`sp_gc_nroots`): an unwind skips the cleanup attributes of every
   frame it passes, so the landing frame is the one that has to put the depth
   back, whether or not it pushed anything itself. */
static void gc_save_take_back(Buf *b, size_t off, size_t save_len) {
  if (off + save_len > b->len) return;
  const char *body = b->p + off + save_len;
  if (strstr(body, "SP_GC_ROOT") || strstr(body, "setjmp") ||
      strstr(body, "sp_gc_nroots")) return;
  buf_erase(b, off, save_len);
}

/* Escape hatch: `--no-root-elision` keeps every root, so a suspected
   miscompile can be bisected against the same binary. */
int g_no_root_elision = 0;
int g_inline_hot = 1;   /* --no-inline-hot turns off forcing small leaf methods inline */
/* The C optimisation level the TU is built at (-O), for the one decision the
   generated program makes from it: an unoptimised build's frames are many
   times -O2's (a 24-column constructor over a poly Hash reserved 51 KB at -O0
   against 592 bytes at -O2), so such a build asks the runtime for larger
   fiber stacks (#4496). */
int g_opt_level = 2;
/* Escape hatch: `--no-write-barrier` emits the stores bare, so a suspected
   miscompile can be bisected against the same binary. */
int g_no_write_barrier = 0;
static int g_has_dyn_syms = 0;   /* the dynamic intern pool was emitted */

/* ---- GC root elision (M1') ----

   A root exists so a precise GC can find a value only the C stack references.
   A poly local proven to hold nothing but a poly array or nil does not need
   one: its index read takes the runtime's inline array arm, which neither
   allocates nor re-enters Ruby code, so nothing in the local's live range can
   collect or move the element -- the container it was read out of still holds
   it. Taking `&local` for the root is what forces the local into memory for the
   whole function and gives it a frame, so dropping it is worth several percent
   in a per-pixel function.

   Everything here is a veto: the region between the root and the local's last
   mention must contain only calls known to stay inside the runtime, and the
   poly reads among them must be reading THIS local. Anything unrecognised
   keeps the root. */
static int gc_elide_call_ok(const char *id, size_t n, const char *lvname) {
  static const char *const KW[] = { "if", "while", "for", "switch", "return",
                                    "sizeof", "do", "else", NULL };
  static const char *const SAFE[] = {
    "sp_box_int", "sp_box_bool", "sp_box_nil", "sp_box_sym", "sp_box_float",
    "sp_box_obj", "sp_box_nullable_obj", "sp_box_poly_array", "sp_box_int_or_nil",
    "sp_box_float_or_nil", "sp_poly_to_i", "sp_poly_to_f", "sp_poly_truthy",
    /* The _or_nil siblings are the same conversions with one tag test in front,
       so they are exactly as safe as the two above -- and they are what a
       narrowing into an int or float slot uses since #4288. Left out, the veto
       fired on the very calls the elision exists for: optcarrot's per-pixel
       sprite function grew a GC frame and the whole benchmark lost 8%,
       2310 fps to 2128. */
    "sp_poly_to_i_or_nil", "sp_poly_to_f_or_nil",
    "sp_poly_length", "sp_imod", "sp_idiv", "sp_int_bit",
    "sp_IntArray_get", "sp_FloatArray_get", "sp_StrArray_get", "sp_PolyArray_get",
    "sp_IntArray_length", "sp_PolyArray_length",
    /* the write barrier touches the remembered set and nothing else: it cannot
       allocate, and it cannot reach Ruby code */
    "sp_gc_wb", "SP_WBO", NULL };
  for (int i = 0; KW[i]; i++) if (strlen(KW[i]) == n && !strncmp(id, KW[i], n)) return 1;
  for (int i = 0; SAFE[i]; i++) if (strlen(SAFE[i]) == n && !strncmp(id, SAFE[i], n)) return 1;
  /* the poly index reads: safe only on the local this root protects, whose
     values are arrays or nil and so cannot reach the allocating arms */
  static const char *const RD[] = { "sp_poly_arr_get_hash", "sp_poly_arr_get",
                                    "sp_poly_arr_get_aon",
                                    "sp_poly_massign_get", NULL };
  for (int i = 0; RD[i]; i++) {
    if (strlen(RD[i]) != n || strncmp(id, RD[i], n)) continue;
    const char *a = id + n;
    while (*a == '(' || *a == ' ') a++;
    size_t ln = strlen(lvname);
    return !strncmp(a, lvname, ln) && (a[ln] == ',' || a[ln] == ' ');
  }
  return 0;
}

/* The region [from, to) calls nothing that could collect or re-enter. */
static int gc_region_inert(const char *from, const char *to, const char *lvname) {
  for (const char *p = from; p < to; p++) {
    if (*p != '(') continue;
    const char *e = p;
    while (e > from && (isalnum((unsigned char)e[-1]) || e[-1] == '_')) e--;
    if (e == p) continue;                       /* `(` after an operator */
    if (!gc_elide_call_ok(e, (size_t)(p - e), lvname)) return 0;
  }
  return 1;
}

/* The same proof for a TEMP. A temp has no LocalVar to carry a flag, so its
   evidence has to be read back out of the text it was just emitted from: a
   temp initialized straight from an element of a container ivar whose elements
   analyze proved array-or-nil is the same value under a different name. This
   is the destructuring shape -- `@io_addr, @lut = @attr_lut[i]` puts the pair
   in a temp and reads it twice. */
static int gc_temp_is_arr_or_nil(Compiler *c, Scope *s, const char *fn,
                                 const char *rootline, const char *tname) {
  if (s->class_id < 0 || s->class_id >= c->nclasses) return 0;
  /* the declaration sits just before the root: `sp_RbVal _tN = <init>;` */
  size_t off = (size_t)(rootline - fn);
  char decl[320];
  snprintf(decl, sizeof decl, "sp_RbVal %s = ", tname);
  const char *d = NULL;
  for (const char *q = strstr(fn, decl); q && (size_t)(q - fn) < off; q = strstr(q + 1, decl)) d = q;
  if (!d) return 0;
  const char *init = d + strlen(decl);
  static const char *const READ[] = { "sp_PolyArray_get(", "sp_poly_arr_get_hash(",
                                      "sp_poly_arr_get(", NULL };
  const char *arg = NULL;
  for (int i = 0; READ[i] && !arg; i++)
    if (!strncmp(init, READ[i], strlen(READ[i]))) arg = init + strlen(READ[i]);
  if (!arg) return 0;
  /* the container must be an ivar of this class, proven element-wise */
  static const char *const SELF = "self->iv_";
  if (strncmp(arg, SELF, strlen(SELF))) return 0;
  const char *fname = arg + strlen(SELF);
  size_t fn_len = 0;
  while (fname[fn_len] && (isalnum((unsigned char)fname[fn_len]) || fname[fn_len] == '_')) fn_len++;
  ClassInfo *ci = &c->classes[s->class_id];
  for (int i = 0; i < ci->nivars; i++) {
    const char *m = iv_c(ci->ivars[i] + 1);
    if (strlen(m) == fn_len && !strncmp(m, fname, fn_len))
      return ci->ivar_arr_elem_arr_or_nil[i];
  }
  return 0;
}

static void gc_roots_take_back(Compiler *c, Scope *s, Buf *b, size_t fn_off) {
  if (fn_off >= b->len) return;
  /* straight-line functions only: with a backward edge the text order is not
     the execution order, so "the last mention" says nothing about liveness. */
  {
    const char *fn = b->p + fn_off;
    if (strstr(fn, "setjmp") || strstr(fn, "while (") || strstr(fn, "for (") ||
        strstr(fn, "do {") || strstr(fn, "goto ")) return;
  }
  for (int i = 0; i < s->nlocals; i++) {
    LocalVar *lv = &s->locals[i];
    if (!lv->arr_or_nil || lv->type != TY_POLY) continue;
    char lvname[300], rootline[340];
    snprintf(lvname, sizeof lvname, "lv_%s", lv->name);
    snprintf(rootline, sizeof rootline, "    SP_GC_ROOT_RBVAL(%s);\n", lvname);
    char *at = strstr(b->p + fn_off, rootline);
    if (!at) continue;
    size_t rl = strlen(rootline);
    const char *body = at + rl;
    const char *last = NULL;
    for (const char *q = strstr(body, lvname); q; q = strstr(q + 1, lvname)) last = q;
    if (last && !gc_region_inert(body, last, lvname)) continue;
    buf_erase(b, (size_t)(at - b->p), rl);
  }
  /* temps, by the same rule */
  for (;;) {
    const char *fn = b->p + fn_off;
    const char *at = NULL;
    char tname[64] = {0}, rootline[128];
    for (const char *q = strstr(fn, "SP_GC_ROOT_RBVAL(_t"); q; q = strstr(q + 1, "SP_GC_ROOT_RBVAL(_t")) {
      const char *nm = q + strlen("SP_GC_ROOT_RBVAL(");
      size_t n = 0;
      while (nm[n] && nm[n] != ')' && n < sizeof tname - 1) { tname[n] = nm[n]; n++; }
      tname[n] = '\0';
      if (nm[n] != ')') continue;
      snprintf(rootline, sizeof rootline, "SP_GC_ROOT_RBVAL(%s);", tname);
      if (!gc_temp_is_arr_or_nil(c, s, fn, q, tname)) continue;
      const char *body = q + strlen(rootline);
      const char *lastq = NULL;
      for (const char *r = strstr(body, tname); r; r = strstr(r + 1, tname)) lastq = r;
      if (lastq && !gc_region_inert(body, lastq, tname)) continue;
      at = q; break;
    }
    if (!at) break;
    /* take the whole statement, and the run of spaces before it */
    size_t start = (size_t)(at - b->p), len = strlen(rootline);
    while (start > fn_off && (b->p[start - 1] == ' ' || b->p[start - 1] == '\n')) { start--; len++; }
    buf_erase(b, start, len);
  }
}

/* ---- root frames ----

   Every root the emitters write costs the C compiler three things: taking
   the local's address pins it to a stack slot, the push carries a bounds
   check and a branch, and the cleanup attribute duplicates the pop on every
   exit of the declaring scope. On a large generated TU those were a third of
   clang's unoptimised IR and most of its peak memory (a 900k-line TU peaked
   at 5.9 GB), and every rooted local widened the frame the function keeps
   for its whole run.

   This pass reads the finished function -- the same way the elision and the
   write-barrier scans do, since the roots come from several hundred emission
   sites -- and moves the roots it can prove function-scoped into one stack
   struct registered by a single SP_GC_ROOT_FRAME entry (lib/sp_gc.h):

   - a boxed temporary `sp_RbVal _tN = ...; SP_GC_ROOT_RBVAL(_tN);` has the
     frame slot `_gcf.v[k]` as its home: the declaration becomes a store,
     every mention names the slot, the root line goes. Slots are shared
     between temporaries whose declaring blocks are disjoint, which is what
     C scoping already guarantees for the names, so a function's slot count
     is its deepest nesting, not its temporary count. A temporary whose
     address is taken, that carries a qualifier, that is declared more than
     once or in a form this scan does not read keeps its per-root macro --
     the two forms coexist, each self-contained.
   - a root at the function's top scope (`SP_GC_ROOT*(lv_x)` at brace depth
     1: the prologue's locals) becomes an entry in the frame's pointer array,
     in the root array's own encoding, so the mark path is the one it had.
     A root nested any deeper is left alone: its variable dies with its
     block, and the cleanup pop is what keeps a loop from accumulating them.

   The frame is zeroed once at entry (a zero slot is INT 0), so a slot holds
   its last value until the function returns: over-approximate liveness,
   never under. Exceptions land on the depth an exception frame recorded
   before its setjmp, which was already below this function's entry. */
int g_no_root_frame = 0;

typedef struct { char name[24]; int rooted, decls, addr, bad, slot; } FrameTemp;
typedef struct { FrameTemp *v; int n, cap; } FrameTemps;

static int frame_idch(char c) { return isalnum((unsigned char)c) || c == '_'; }
/* `_t<digits>`: the emitters' temporary names. */
static size_t frame_temp_len(const char *p, size_t end) {
  size_t k = 0;
  if (end < 3 || p[0] != '_' || p[1] != 't' || !isdigit((unsigned char)p[2])) return 0;
  k = 2;
  while (k < end && isdigit((unsigned char)p[k])) k++;
  if (k < end && frame_idch(p[k])) return 0;
  return k;
}
static FrameTemp *frame_temp(FrameTemps *ts, const char *nm, size_t n, int create) {
  for (int i = 0; i < ts->n; i++)
    if (strlen(ts->v[i].name) == n && !strncmp(ts->v[i].name, nm, n)) return &ts->v[i];
  if (!create || n >= sizeof ts->v[0].name) return NULL;
  if (ts->n == ts->cap) {
    ts->cap = ts->cap ? ts->cap * 2 : 64;
    ts->v = realloc(ts->v, sizeof *ts->v * (size_t)ts->cap);
  }
  FrameTemp *t = &ts->v[ts->n++];
  memset(t, 0, sizeof *t);
  memcpy(t->name, nm, n); t->name[n] = '\0'; t->slot = -1;
  return t;
}
/* The end of the string/char literal, comment or preprocessor line starting
   at i, or i when nothing does: the scans below must not read braces or
   names out of a `#line` file name or a string constant. */
static size_t frame_skip_noncode(const char *p, size_t i, size_t end) {
  char c = p[i];
  if (c == '"' || c == '\'') {
    size_t j = i + 1;
    while (j < end && p[j] != c) { if (p[j] == '\\' && j + 1 < end) j++; j++; }
    return j < end ? j + 1 : end;
  }
  if (c == '#' && (i == 0 || p[i-1] == '\n')) {
    size_t j = i;
    while (j < end && p[j] != '\n') j++;
    return j;
  }
  if (c == '/' && i + 1 < end && p[i+1] == '*') {
    size_t j = i + 2;
    while (j + 1 < end && !(p[j] == '*' && p[j+1] == '/')) j++;
    return j + 1 < end ? j + 2 : end;
  }
  if (c == '/' && i + 1 < end && p[i+1] == '/') {
    size_t j = i;
    while (j < end && p[j] != '\n') j++;
    return j;
  }
  return i;
}
/* Does the text before i end with `word` as a whole word (spaces between
   allowed)? Returns the offset where that word starts, or 0 for no. */
static size_t frame_preceded_by(const char *p, size_t i, size_t lo, const char *word) {
  size_t n = strlen(word);
  while (i > lo && p[i-1] == ' ') i--;
  if (i < lo + n || strncmp(p + i - n, word, n)) return 0;
  if (i - n > lo && frame_idch(p[i-n-1])) return 0;
  return i - n;
}

/* First scan: every temporary that is rooted, how it is declared and whether
   its address is taken. */
static void frame_collect(const char *p, size_t beg, size_t end, FrameTemps *ts) {
  size_t i = beg;
  while (i < end) {
    size_t j = frame_skip_noncode(p, i, end);
    if (j != i) { i = j; continue; }
    if (!frame_idch(p[i]) || isdigit((unsigned char)p[i])) { i++; continue; }
    size_t k = i;
    while (k < end && frame_idch(p[k])) k++;
    if (k - i == 16 && !strncmp(p + i, "SP_GC_ROOT_RBVAL", 16) && k < end && p[k] == '(') {
      size_t m = k + 1, tl = frame_temp_len(p + m, end - m);
      if (tl) {
        FrameTemp *t = frame_temp(ts, p + m, tl, 1);
        if (t) {
          if (m + tl + 1 < end && p[m + tl] == ')' && p[m + tl + 1] == ';') t->rooted++;
          else t->bad = 1;
        }
        i = m + tl; continue;
      }
    }
    size_t tl = frame_temp_len(p + i, end - i);
    if (tl) {
      FrameTemp *t = frame_temp(ts, p + i, tl, 1);
      if (t) {
        size_t q = i;
        while (q > beg && p[q-1] == ' ') q--;
        if (q > beg && p[q-1] == '&') t->addr = 1;
        size_t d = frame_preceded_by(p, i, beg, "sp_RbVal");
        if (d) {
          /* a qualifier in front (volatile, const, static) is a form this
             scan does not carry over */
          size_t r = d;
          while (r > beg && p[r-1] == ' ') r--;
          if (r > beg && frame_idch(p[r-1])) t->bad = 1;
          size_t a = k;
          while (a < end && p[a] == ' ') a++;
          if (a < end && p[a] == ';') t->decls++;
          else if (a < end && p[a] == '=' && (a + 1 >= end || p[a+1] != '=')) {
            /* a brace initialiser has no assignment form */
            size_t v = a + 1;
            while (v < end && p[v] == ' ') v++;
            if (v < end && p[v] == '{') t->bad = 1; else t->decls++;
          }
          else t->bad = 1;
        }
      }
      i = k; continue;
    }
    i = k;
  }
}
static int frame_convertible(FrameTemp *t) {
  return t && t->rooted > 0 && t->decls == 1 && !t->addr && !t->bad;
}

#define FRAME_DEPTH_MAX 1024

/* The rewrite. Returns 1 when a frame was inserted; the buffer is untouched
   otherwise. `ins` is the offset just past the SP_GC_SAVE statement -- the
   function's top scope, where the frame is declared and from where the body
   is read. */
/* A Ruby statement that lowers to several C lines has one #line at its top,
   and the preprocessor counts on from it: a C error near the end of an
   inline loop was reported at a Ruby line past the end of the file (#4940).
   Every line of the run is that statement's, so the directive is repeated
   before each line of it. Not after a line ending in a backslash (a macro's
   continuation) or before another directive. Measured on optcarrot: 65% more
   lines, no change in the C compiler's time. */
static void line_map_reanchor(Buf *b) {
  if (!b->p || b->len == 0) return;
  Buf o; memset(&o, 0, sizeof o);
  const char *p = b->p, *end = b->p + b->len;
  const char *cur = NULL; size_t curn = 0;   /* the directive in effect */
  int after = 0, prev_cont = 0;
  while (p < end) {
    const char *nl = memchr(p, '\n', (size_t)(end - p));
    size_t n = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
    int is_dir = n >= 6 && strncmp(p, "#line ", 6) == 0;
    if (is_dir) { cur = p; curn = n; after = 0; }
    else if (cur && after > 0 && !prev_cont && p[0] != '#') buf_putn(&o, cur, curn);
    buf_putn(&o, p, n);
    if (!is_dir) after++;
    size_t tl = n;
    while (tl > 0 && (p[tl - 1] == '\n' || p[tl - 1] == '\r')) tl--;
    prev_cont = tl > 0 && p[tl - 1] == '\\';
    p += n;
  }
  free(b->p);
  *b = o;
}

static int gc_frame_build(Buf *b, size_t ins) {
  if (ins >= b->len) return 0;
  const char *p = b->p;
  size_t end = b->len;
  FrameTemps ts; memset(&ts, 0, sizeof ts);
  frame_collect(p, ins, end, &ts);
  int any = 0;
  for (int i = 0; i < ts.n; i++) if (frame_convertible(&ts.v[i])) { any = 1; break; }
  if (!any && !strstr(p + ins, "SP_GC_ROOT")) { free(ts.v); return 0; }

  Buf nb; memset(&nb, 0, sizeof nb);
  int depth = 1, wm = 0, peak = 0, np = 0;
  int saved[FRAME_DEPTH_MAX];
  size_t i = ins;
  while (i < end) {
    size_t j = frame_skip_noncode(p, i, end);
    if (j != i) { buf_putn(&nb, p + i, j - i); i = j; continue; }
    char c = p[i];
    if (c == '{') {
      if (depth < FRAME_DEPTH_MAX) saved[depth] = wm;
      depth++; buf_putn(&nb, p + i, 1); i++; continue;
    }
    if (c == '}') {
      depth--;
      /* past the bound the slots are simply not shared, which is still right */
      if (depth >= 1 && depth < FRAME_DEPTH_MAX) wm = saved[depth];
      buf_putn(&nb, p + i, 1); i++; continue;
    }
    if (!frame_idch(c) || isdigit((unsigned char)c)) { buf_putn(&nb, p + i, 1); i++; continue; }
    size_t k = i;
    while (k < end && frame_idch(p[k])) k++;
    size_t n = k - i;
    /* a root statement: a converted temporary's is dropped, a top-scope
       one moves into the frame's entries, any other stays */
    if (n >= 10 && !strncmp(p + i, "SP_GC_ROOT", 10) && k < end && p[k] == '(' &&
        (n == 10 || (n == 16 && !strncmp(p + i, "SP_GC_ROOT_RBVAL", 16)) ||
         (n == 14 && !strncmp(p + i, "SP_GC_ROOT_STR", 14)))) {
      size_t m = k + 1, tl = n == 16 ? frame_temp_len(p + m, end - m) : 0;
      if (tl && m + tl + 1 < end && p[m + tl] == ')' && p[m + tl + 1] == ';' &&
          frame_convertible(frame_temp(&ts, p + m, tl, 0))) {
        if (nb.len && nb.p[nb.len-1] == ' ') nb.len--;
        i = m + tl + 2; continue;
      }
      if (depth == 1) {
        size_t q = m; int pd = 1;
        while (q < end && pd) { if (p[q] == '(') pd++; else if (p[q] == ')') pd--; q++; }
        if (pd == 0 && q < end && p[q] == ';') {
          buf_printf(&nb, "_gcf.p[%d] = SP_GC_ENTRY_%s(", np++,
                     n == 16 ? "RBVAL" : n == 14 ? "STR" : "PTR");
          buf_putn(&nb, p + m, q - 1 - m);
          buf_puts(&nb, ");");
          i = q + 1; continue;
        }
      }
      buf_putn(&nb, p + i, n); i = k; continue;
    }
    size_t tl = frame_temp_len(p + i, end - i);
    FrameTemp *t = tl ? frame_temp(&ts, p + i, tl, 0) : NULL;
    if (frame_convertible(t)) {
      int decl = frame_preceded_by(p, i, ins, "sp_RbVal") != 0;
      if (decl) {
        /* the type went into the output already; the slot is the home */
        size_t r = nb.len;
        while (r > 0 && nb.p[r-1] == ' ') r--;
        if (r >= 8 && !strncmp(nb.p + r - 8, "sp_RbVal", 8)) nb.len = r - 8;
        t->slot = wm++;
        if (wm > peak) peak = wm;
        size_t a = k;
        while (a < end && p[a] == ' ') a++;
        if (a < end && p[a] == ';') buf_puts(&nb, "(void)");
      }
      else if (t->slot < 0) { t->slot = wm++; if (wm > peak) peak = wm; }
      buf_printf(&nb, "_gcf.v[%d]", t->slot);
      i = k; continue;
    }
    buf_putn(&nb, p + i, n); i = k;
  }
  free(ts.v);
  if (peak == 0 && np == 0) { free(nb.p); return 0; }
  Buf out; memset(&out, 0, sizeof out);
  buf_putn(&out, p, ins);
  buf_puts(&out, "    struct { sp_gc_frame_hdr h;");
  if (peak) buf_printf(&out, " sp_RbVal v[%d];", peak);
  if (np) buf_printf(&out, " void **p[%d];", np);
  buf_printf(&out, " } _gcf = {{%d, %d}}; SP_GC_ROOT_FRAME(_gcf);\n", peak, np);
  buf_putn(&out, nb.p ? nb.p : "", nb.len);
  free(nb.p);
  free(b->p);
  *b = out;
  return 1;
}

/* ---- write barrier insertion ----

   A reference stored into an object that has already been promoted can be the
   only thing holding a young object, and a generational mark would not reach
   it. The barrier records those stores. Which ivars need it is a question about
   the field's type, and which stores exist is a question about what the
   emitters actually produced -- there are three dozen of them -- so this reads
   the emitted text rather than trusting a list of emission sites to stay
   complete. A store the scan does not recognise keeps its old shape, which is
   correct today and would be a missed barrier under a generational mark, so
   the scan is checked by counting rather than by inspection (see
   SPINEL_WB_REPORT).

   Only reference fields: a barrier on every ivar store, scalars included,
   costs 14% on optcarrot where the reference-only one costs 0.5%. */
/* The class whose struct is named `sp_<name>`, or -1. */
static int wb_class_by_cname(Compiler *c, const char *nm, size_t n) {
  for (int k = 0; k < c->nclasses; k++) {
    const char *cn = c->classes[k].c_name;
    if (cn && strlen(cn) == n && !strncmp(cn, nm, n)) return k;
  }
  return -1;
}

/* `only` >= 0 restricts the question to that class, which is what the holder's
   own C type gives us; -1 falls back to asking whether ANY class declares the
   name as a reference, which over-approximates in the safe direction. */
static int wb_field_is_ref_in(Compiler *c, int only, const char *fld, size_t n) {
  if (n <= 3 || strncmp(fld, "iv_", 3)) return 0;
  fld += 3; n -= 3;
  for (int k = 0; k < c->nclasses; k++) {
    if (only >= 0 && k != only) continue;
    ClassInfo *ci = &c->classes[k];
    for (int i = 0; i < ci->nivars; i++) {
      const char *m = iv_c(ci->ivars[i] + 1);
      if (strlen(m) != n || strncmp(m, fld, n)) continue;
      /* A value-type class lives inline and has no GC header of its own, so
         the barrier's `(hdr *)obj - 1` would read something else entirely.

         Skipping it loses nothing, which is worth stating because it looks
         like it should: a value type cannot be reached FROM the heap either.
         analyze disqualifies a class from the by-value layout as soon as an
         instance is stored into an ivar, a constant, a global or a container
         element, so one only ever lives on the C stack or inside another
         value type. There is no old holder for a store into it to record. */
      if (ci->is_value_type) continue;
      TyKind t = ci->ivar_types[i];
      if (needs_root(t) && !comp_ty_value_obj(c, t)) return 1;
    }
  }
  return 0;
}
static int wb_field_is_ref(Compiler *c, const char *fld, size_t n) {
  return wb_field_is_ref_in(c, -1, fld, n);
}
/* Does class `k` declare the emitted field `fld` (which carries the "iv_"
   prefix)? Used to check that a holder class resolved from the text is really
   the one being written to before letting it suppress a barrier. */
static int wb_class_has_field(Compiler *c, int k, const char *fld, size_t n) {
  if (k < 0 || k >= c->nclasses || n <= 3 || strncmp(fld, "iv_", 3)) return 0;
  fld += 3; n -= 3;
  ClassInfo *ci = &c->classes[k];
  for (int i = 0; i < ci->nivars; i++) {
    const char *m = iv_c(ci->ivars[i] + 1);
    if (strlen(m) == n && !strncmp(m, fld, n)) return 1;
  }
  return 0;
}

/* Walk back from `end` (exclusive) over one C postfix expression: an
   identifier or a balanced parenthesised group, then any `->`/`.`/`[...]`
   chain in front of it. Returns the offset where it starts. */
static size_t wb_lvalue_start(const char *p, size_t end) {
  size_t i = end;
  for (;;) {
    while (i > 0 && (p[i-1] == ' ' || p[i-1] == '\n' || p[i-1] == '\t')) i--;
    if (i == 0) return i;
    if (p[i-1] == ')' || p[i-1] == ']') {
      char open = p[i-1] == ')' ? '(' : '[', close = p[i-1];
      int depth = 0;
      while (i > 0) {
        i--;
        if (p[i] == close) depth++;
        else if (p[i] == open) { depth--; if (!depth) break; }
      }
      if (i == 0) return i;
    }
    else if (isalnum((unsigned char)p[i-1]) || p[i-1] == '_') {
      while (i > 0 && (isalnum((unsigned char)p[i-1]) || p[i-1] == '_')) i--;
    }
    else return i;
    /* a chain link in front of what we just consumed? */
    size_t j = i;
    while (j > 0 && (p[j-1] == ' ' || p[j-1] == '\n')) j--;
    if (j >= 2 && p[j-1] == '>' && p[j-2] == '-') { i = j - 2; continue; }
    if (j >= 1 && p[j-1] == '.' && !(j >= 2 && isdigit((unsigned char)p[j-2]))) { i = j - 1; continue; }
    return i;
  }
}

/* The class of the object a store writes into, from its own C type: `self` in
   a function whose signature says `sp_X *self`, or an explicit `(sp_X *)` cast
   in front of the lvalue. -1 when the text does not say, which falls back to
   the name-based question. Knowing the class is what keeps the barrier off a
   value-type holder, which has no header for it to reach. */
static int wb_holder_class(Compiler *c, const char *p, size_t st, size_t fn_off,
                           size_t lv_end, int cur_self_cls) {
  size_t n = lv_end - st;
  if (n == 4 && !strncmp(p + st, "self", 4)) return cur_self_cls;
  /* `((sp_X *)expr)` or `(sp_X *)expr` */
  const char *q = p + st;
  size_t k = 0;
  while (k < n && (q[k] == '(' || q[k] == ' ')) k++;
  if (k + 3 < n && !strncmp(q + k, "sp_", 3)) {
    size_t e = k + 3;
    while (e < n && (isalnum((unsigned char)q[e]) || q[e] == '_')) e++;
    size_t sp = e;
    while (sp < n && q[sp] == ' ') sp++;
    if (sp < n && q[sp] == '*') return wb_class_by_cname(c, q + k + 3, e - k - 3);
  }
  (void)fn_off;
  return -1;
}

/* A closure cell is a one-word GC object holding the captured variable, and a
   block body writes it as `(*<cellptr>) = v` -- no `iv_` in sight, so the ivar
   scan above never sees it. Whether it holds a reference is written into its
   own allocation: `sp_cell_scan_ptr` and `sp_cell_scan_rbval` mark a GC object,
   `sp_cell_scan_str` reaches the string heap. All three need the barrier: the
   string heap is swept generationally too, so a young string stored into a
   cell an old holder reaches is exactly as invisible as a young object. */
typedef struct { char **v; int n, cap; } WbCells;
static int wb_cells_has(WbCells *cs, const char *nm, size_t n) {
  for (int k = 0; k < cs->n; k++)
    if (strlen(cs->v[k]) == n && !strncmp(cs->v[k], nm, n)) return 1;
  return 0;
}
static void wb_cells_collect(WbCells *cs, const char *p, size_t len) {
  for (size_t i = 0; i + 6 < len; i++) {
    if (strncmp(p + i, "_cell_", 6)) continue;
    size_t s = i + 6, e = s;
    while (e < len && (isalnum((unsigned char)p[e]) || p[e] == '_')) e++;
    i = e - 1;
    if (e == s) continue;
    /* only the declaration says what the cell holds; find its scan argument */
    size_t q = e, stop = e;
    while (stop < len && p[stop] != ';' && p[stop] != '\n') stop++;
    int ref = 0;
    for (; q + 13 < stop; q++)
      if (!strncmp(p + q, "sp_cell_scan_", 13)) { ref = 1; break; }
    if (!ref || wb_cells_has(cs, p + s, e - s)) continue;
    if (cs->n == cs->cap) { cs->cap = cs->cap ? cs->cap * 2 : 16;
                            cs->v = (char **)realloc(cs->v, sizeof(char *) * cs->cap); }
    cs->v[cs->n] = (char *)malloc(e - s + 1);
    memcpy(cs->v[cs->n], p + s, e - s); cs->v[cs->n][e - s] = '\0'; cs->n++;
  }
}
/* Whether the line at `bol` is a function definition's header: it starts in
   column 0 with an identifier that is not a statement keyword (the emitter
   writes some multi-line statements from column 0 -- `if (..) {`, `else {`,
   `while (..) {` -- inside a body), opens a parameter list, and ends in `{`. */
static int wb_is_fn_header(const Buf *b, size_t bol) {
  char c0 = b->p[bol];
  if (!(isalpha((unsigned char)c0) || c0 == '_')) return 0;
  size_t e = bol;
  while (e < b->len && b->p[e] != '\n') e++;
  if (e == bol || b->p[e-1] != '{') return 0;
  static const char *kw[] = { "if ", "if(", "else", "while", "for ", "for(", "switch", "do ", "do{" };
  for (size_t k = 0; k < sizeof kw / sizeof *kw; k++)
    if (!strncmp(b->p + bol, kw[k], strlen(kw[k]))) return 0;
  return memchr(b->p + bol, '(', e - bol) != NULL;
}
/* The enclosing function's header for position `at`: the nearest header line
   before it. `*last_at` and `*last_h` carry the previous answer, so a scan
   walks back only as far as the previous store did. Returns the header's
   offset, or (size_t)-1. */
static size_t wb_fn_header(const Buf *b, size_t at, size_t *last_at, size_t *last_h) {
  size_t k = at;
  while (k > 0) {
    size_t bol = k;
    while (bol > 0 && b->p[bol-1] != '\n') bol--;
    if (bol < at && wb_is_fn_header(b, bol)) { *last_at = at; *last_h = bol; return bol; }
    if (*last_h != (size_t)-1 && bol <= *last_at && bol > *last_h) { *last_at = at; return *last_h; }
    if (bol == 0) break;
    k = bol - 1;
  }
  *last_h = (size_t)-1;
  return (size_t)-1;
}
/* Whether the header at `h` takes `*_cell_<nm>` as a parameter. */
static int wb_header_has_param(const Buf *b, size_t h, const char *nm, size_t nn) {
  size_t e = h;
  while (e < b->len && b->p[e] != '{' && b->p[e] != '\n') e++;
  for (size_t k = h; k + 7 + nn <= e; k++) {
    if (b->p[k] != '*' || strncmp(b->p + k + 1, "_cell_", 6) || strncmp(b->p + k + 7, nm, nn)) continue;
    char t = b->p[k + 7 + nn];
    if (t == ',' || t == ')') return 1;
  }
  return 0;
}
/* `(*X) = v` where X names a reference cell: wrap X so the barrier lands on the
   cell, which is the object the collector reaches the stored value through.

   The names are the whole program's, since a proc body stores through a
   capture field whose cell another function declared. So `_cell_x` matches a
   by-reference String parameter too whenever any function anywhere has a
   heap cell named `x` -- and a by-reference parameter may point at the
   caller's stack local (`&lv_x`), where the barrier reads a header that is not
   there and, if the bytes in front of the slot happen to read as an old clean
   header, sets a dirty bit in the caller's frame. Such a store needs no
   barrier: the lending site pins a heap cell or an ivar owner it lends
   (sp_gc_pin_remembered), and a stack local is rooted by the caller's frame.
   So a `_cell_x` that is the enclosing function's parameter is skipped. */
static void gc_wb_cells(Compiler *c, Buf *b) {
  WbCells cs; memset(&cs, 0, sizeof cs);
  wb_cells_collect(&cs, b->p, b->len);
  if (!cs.n) { free(cs.v); return; }
  size_t hdr_at = 0, hdr_h = (size_t)-1;
  for (size_t i = 0; i + 3 < b->len; i++) {
    if (b->p[i] != '(' || b->p[i+1] != '*') continue;
    size_t j = i, d = 0;
    while (j < b->len) {
      if (b->p[j] == '(') d++;
      else if (b->p[j] == ')') { d--; if (!d) break; }
      j++;
    }
    if (j >= b->len) continue;
    size_t q = j + 1;
    while (q < b->len && b->p[q] == ' ') q++;
    if (q >= b->len || b->p[q] != '=' || b->p[q+1] == '=') continue;
    size_t is = i + 2, ie = j;                     /* the inner expression */
    while (is < ie && b->p[is] == ' ') is++;
    while (ie > is && b->p[ie-1] == ' ') ie--;
    if (ie <= is) continue;
    /* the cell's name is the trailing identifier, whether it is the local
       `_cell_x` or the capture field `((_proc_cap_1 *)_cap)->x` */
    size_t ne = ie, ns = ie;
    while (ns > is && (isalnum((unsigned char)b->p[ns-1]) || b->p[ns-1] == '_')) ns--;
    if (ns == ne) continue;
    const char *nm = b->p + ns; size_t nn = ne - ns;
    int local_cell = 0;
    if (nn > 6 && !strncmp(nm, "_cell_", 6)) { nm += 6; nn -= 6; local_cell = 1; }
    /* the capture struct's field for cell `x` is `c_x`; without the strip no
       store made through `_cap` matched the declaration and the proc body's
       every `(*cap->c_x) = v` went unrecorded */
    else if (nn > 2 && !strncmp(nm, "c_", 2) && ns >= 2 && b->p[ns-1] == '>' && b->p[ns-2] == '-') { nm += 2; nn -= 2; }
    if (!wb_cells_has(&cs, nm, nn)) continue;
    if (!strncmp(b->p + is, "SP_WBO(", 7)) continue;
    if (local_cell) {
      size_t h = wb_fn_header(b, i, &hdr_at, &hdr_h);
      if (h != (size_t)-1 && wb_header_has_param(b, h, nm, nn)) continue;
    }
    /* At statement position, run the barrier AFTER the store: the value
       usually allocates, and an allocation between the barrier and the store
       collects, which clears the record the barrier just made (see
       gc_wb_insert). Anywhere else the wrapper stays, with that hazard. */
    size_t bol2 = i;
    while (bol2 > 0 && b->p[bol2-1] != '\n' && b->p[bol2-1] != ';' && b->p[bol2-1] != '{') bol2--;
    int at_stmt2 = 1;
    for (size_t k = bol2; k < i; k++)
      if (b->p[k] != ' ' && b->p[k] != '\t') { at_stmt2 = 0; break; }
    size_t send = 0;
    if (at_stmt2) {
      size_t k = q + 1, d2 = 0; int str2 = 0, ch2 = 0;
      for (; k < b->len; k++) {
        char x = b->p[k];
        if (str2) { if (x == '\\') k++; else if (x == '"') str2 = 0; continue; }
        if (ch2) { if (x == '\\') k++; else if (x == '\'') ch2 = 0; continue; }
        if (x == '"') { str2 = 1; continue; }
        if (x == '\'') { ch2 = 1; continue; }
        if (x == '(' || x == '[' || x == '{') d2++;
        else if (x == ')' || x == ']' || x == '}') { if (!d2) break; d2--; }
        else if (x == ';' && !d2) { send = k; break; }
      }
      if (send) {                       /* not when it is a statement expression's value */
        size_t k2 = send + 1;
        while (k2 < b->len && (b->p[k2]==' '||b->p[k2]=='\n'||b->p[k2]=='\t')) k2++;
        if (k2 + 1 < b->len && b->p[k2]=='}' && b->p[k2+1]==')') send = 0;
      }
    }
    Buf ins; memset(&ins, 0, sizeof ins);
    if (send) {
      int wid = ++g_tmp;
      buf_printf(&ins, "{ __typeof__(");
      buf_putn(&ins, b->p + is, ie - is);
      buf_printf(&ins, ") _wc%d = ", wid);
      buf_putn(&ins, b->p + is, ie - is);
      buf_printf(&ins, "; (*_wc%d)", wid);
      buf_putn(&ins, b->p + j + 1, send - j);
      buf_printf(&ins, " sp_gc_wb((void *)_wc%d); }", wid);
      size_t grew2 = ins.len - (send + 1 - i);
      size_t tail2 = b->len - (send + 1);
      for (size_t g = 0; g < grew2; g++) buf_putn(b, "\0", 1);
      memmove(b->p + i + ins.len, b->p + i + (send + 1 - i), tail2);
      memcpy(b->p + i, ins.p, ins.len);
      b->p[b->len] = '\0';
      i = i + ins.len;
      free(ins.p);
      continue;
    }
    buf_puts(&ins, "SP_WBO(");
    buf_putn(&ins, b->p + is, ie - is);
    buf_puts(&ins, ")");
    size_t grew = ins.len - (ie - is);
    size_t tail = b->len - ie;
    for (size_t g = 0; g < grew; g++) buf_putn(b, "\0", 1);
    memmove(b->p + is + ins.len, b->p + is + (ie - is), tail);
    memcpy(b->p + is, ins.p, ins.len);
    b->p[b->len] = '\0';
    i = is + ins.len;
    free(ins.p);
  }
  for (int k = 0; k < cs.n; k++) free(cs.v[k]);
  free(cs.v);
  (void)c;
}

static void gc_wb_insert_seg(Compiler *c, Buf *b, size_t fn_off);
/* Each insertion shifts the rest of the buffer, so over the whole program the
   splices cost (barriers x output size): 9,462 barriers into 43 MB on lobsters
   (#4966). They are made per top-level segment instead, split in front of each
   `\nstatic ` line. Nothing the splice reads crosses such a line: the lvalue
   scan walks back over an lvalue and stops at the `}` or `;` before it, the
   statement scan forward stops at the enclosing block's close, and the
   receiver class is reset at every `static ` line anyway. */
static void gc_wb_insert(Compiler *c, Buf *b, size_t fn_off) {
  if (g_no_write_barrier) return;
  gc_wb_cells(c, b);
  if (fn_off != 0 || !b->p) { gc_wb_insert_seg(c, b, fn_off); return; }
  Buf out; memset(&out, 0, sizeof out);
  size_t start = 0;
  while (start < b->len) {
    size_t end = start + 1;
    while (end < b->len && !(b->p[end] == '\n' && !strncmp(b->p + end + 1, "static ", 7))) end++;
    Buf seg; memset(&seg, 0, sizeof seg);
    buf_putn(&seg, b->p + start, end - start);
    gc_wb_insert_seg(c, &seg, 0);
    buf_putn(&out, seg.p, seg.len);
    free(seg.p);
    start = end;
  }
  free(b->p);
  *b = out;
}
static void gc_wb_insert_seg(Compiler *c, Buf *b, size_t fn_off) {
  int cur_self_cls = -1;
  for (size_t i = fn_off; i + 4 < b->len; i++) {
    /* track the enclosing function's receiver type */
    if (b->p[i] == '\n' && !strncmp(b->p + i + 1, "static ", 7)) {
      const char *ln = b->p + i + 1;
      /* only this line: a strstr ran on past a line without `*self` to the
         next one anywhere in the buffer, for every prototype and proc */
      const char *nl = strchr(ln, '\n');
      const char *sf = NULL;
      for (const char *t = ln; *t && (!nl || t < nl); t++)
        if (t[0] == '*' && !strncmp(t, "*self", 5)) { sf = t; break; }
      cur_self_cls = -1;
      if (sf) {
        const char *t = sf;
        while (t > ln && (t[-1] == ' ' || t[-1] == '*')) t--;
        const char *e = t;
        while (t > ln && (isalnum((unsigned char)t[-1]) || t[-1] == '_')) t--;
        if ((size_t)(e - t) > 3 && !strncmp(t, "sp_", 3))
          cur_self_cls = wb_class_by_cname(c, t + 3, (size_t)(e - t) - 3);
      }
    }
    if (b->p[i] != '-' || b->p[i+1] != '>' || strncmp(b->p + i + 2, "iv_", 3)) continue;
    size_t f = i + 2, e = f;
    while (e < b->len && (isalnum((unsigned char)b->p[e]) || b->p[e] == '_')) e++;
    size_t q = e;
    while (q < b->len && b->p[q] == ' ') q++;
    if (q >= b->len || b->p[q] != '=' || b->p[q+1] == '=') continue;   /* a read, or == */
    size_t st = wb_lvalue_start(b->p, i);
    if (st >= i) continue;
    /* The holder's own C type is used to EXCLUDE, not to decide: a value-type
       class lives inline and has no header for the barrier to reach, and
       writing through `(hdr *)obj - 1` there is memory it does not own. Which
       fields are references stays the permissive question, since a store whose
       holder the text does not name still has to be covered. */
    int hc = wb_holder_class(c, b->p, st, fn_off, i, cur_self_cls);
    if (hc >= 0 && wb_class_has_field(c, hc, b->p + f, e - f)) {
      if (!wb_field_is_ref_in(c, hc, b->p + f, e - f)) continue;
    }
    else if (!wb_field_is_ref(c, b->p + f, e - f)) continue;
    /* already wrapped (a nested store re-scanned) */
    if (st >= 7 && !strncmp(b->p + st - 7, "SP_WBO(", 7)) continue;
    if (st >= 14 && !strncmp(b->p + st - 14, "sp_gc_wb((void ", 15 - 1)) continue;
    /* A bare identifier can be named twice, so the barrier goes in front as its
       own statement -- which the C compiler optimizes far better than the
       statement expression the general form needs (8% vs noise on optcarrot).
       Anything else, and any store inside a larger expression, takes the
       wrapper, which evaluates the object exactly once. */
    int simple = 1;
    for (size_t k = st; k < i; k++)
      if (!isalnum((unsigned char)b->p[k]) && b->p[k] != '_') { simple = 0; break; }
    size_t bol = st;
    while (bol > fn_off && b->p[bol-1] != '\n' && b->p[bol-1] != ';' && b->p[bol-1] != '{') bol--;
    int at_stmt = 1;
    for (size_t k = bol; k < st; k++)
      if (b->p[k] != ' ' && b->p[k] != '\t') { at_stmt = 0; break; }
    /* `if (cond) obj->f = v;` -- the store is the whole substatement, so a
       block around it is still a statement and the barrier can follow. */
    if (!at_stmt) {
      size_t k = bol;
      while (k < st && (b->p[k] == ' ' || b->p[k] == '\t')) k++;
      if (k + 3 < st && !strncmp(b->p + k, "if ", 3) && b->p[k+3] == '(') {
        size_t p2 = k + 3, d3 = 0;
        for (; p2 < st; p2++) {
          if (b->p[p2] == '(') d3++;
          else if (b->p[p2] == ')') { d3--; if (!d3) { p2++; break; } }
        }
        while (p2 < st && (b->p[p2] == ' ' || b->p[p2] == '\t')) p2++;
        if (p2 == st) at_stmt = 1;
      }
    }
    /* The barrier has to run AFTER the value is in the slot, not before it is
       computed. The right-hand side usually allocates -- `@a = []` is the
       whole shape -- and an allocation can collect: the collection walks the
       object the barrier just recorded, then clears the remembered set and the
       dirty bit, and the store that follows lands unrecorded. The next minor
       mark does not walk the holder, the value it holds is young and
       unreachable, and it is freed while still in the slot. Nothing between
       the store and the barrier allocates, so putting it after closes the
       window without opening another. Found by rubys as silent data loss on a
       long-lived object that replaces a container (#3513).

       Statement position takes the object into a temp so a non-trivial lvalue
       is evaluated once; an assignment inside a larger expression still uses
       the wrapper, which has the same hazard and no room for a second
       statement. */
    size_t stmt_end = 0;
    /* Not when this store is the last statement of a statement expression:
       that position IS the expression's value, and wrapping it in a block
       makes the value void. Detected by what follows the statement -- `})`
       closes a statement expression. */
    if (at_stmt) {
      size_t k = q + 1, d = 0;
      int str = 0, ch = 0;
      for (; k < b->len; k++) {
        char x = b->p[k];
        if (str) { if (x == '\\') k++; else if (x == '"') str = 0; continue; }
        if (ch) { if (x == '\\') k++; else if (x == '\'') ch = 0; continue; }
        if (x == '"') { str = 1; continue; }
        if (x == '\'') { ch = 1; continue; }
        if (x == '(' || x == '[' || x == '{') d++;
        else if (x == ')' || x == ']' || x == '}') { if (!d) break; d--; }
        else if (x == ';' && !d) { stmt_end = k; break; }
      }
    }
    if (stmt_end) {
      size_t k2 = stmt_end + 1;
      while (k2 < b->len && (b->p[k2] == ' ' || b->p[k2] == '\n' || b->p[k2] == '\t')) k2++;
      if (k2 + 1 < b->len && b->p[k2] == '}' && b->p[k2+1] == ')') stmt_end = 0;
    }
    Buf ins; memset(&ins, 0, sizeof ins);
    if (at_stmt && stmt_end) {
      /* rewrite the whole statement: { typeof(obj) _wb = obj; _wb->f = rhs; wb(_wb); } */
      int wid = ++g_tmp;
      buf_printf(&ins, "{ __typeof__(");
      buf_putn(&ins, b->p + st, i - st);
      buf_printf(&ins, ") _wb%d = ", wid);
      buf_putn(&ins, b->p + st, i - st);
      buf_printf(&ins, "; _wb%d", wid);
      buf_putn(&ins, b->p + i, stmt_end - i + 1);
      /* A statement expression's last statement is its value, and this store
         can be one (`({ ...; o->f = v; })`). Keep the assigned value as the
         block's value so a consumer still reads it. */
      buf_printf(&ins, " sp_gc_wb((void *)_wb%d); }", wid);
      size_t grew2 = ins.len - (stmt_end + 1 - st);
      size_t tail2 = b->len - (stmt_end + 1);
      for (size_t g = 0; g < grew2; g++) buf_putn(b, "\0", 1);
      memmove(b->p + st + ins.len, b->p + st + (stmt_end + 1 - st), tail2);
      memcpy(b->p + st, ins.p, ins.len);
      b->p[b->len] = '\0';
      i = st + ins.len;
      free(ins.p);
      continue;
    }
    if (0) {
      buf_puts(&ins, "sp_gc_wb((void *)");
      buf_putn(&ins, b->p + st, i - st);
      buf_puts(&ins, "); ");
      buf_putn(&ins, b->p + st, i - st);
    }
    else {
      buf_puts(&ins, "SP_WBO(");
      buf_putn(&ins, b->p + st, i - st);
      buf_puts(&ins, ")");
    }
    /* splice `ins` in place of the lvalue text: grow, shift the tail up, write */
    size_t grew = ins.len - (i - st);
    size_t tail = b->len - i;
    for (size_t g = 0; g < grew; g++) buf_putn(b, "\0", 1);
    memmove(b->p + st + ins.len, b->p + st + (i - st), tail);
    memcpy(b->p + st, ins.p, ins.len);
    b->p[b->len] = '\0';
    i = st + ins.len + 1;                    /* continue past what was inserted */
    free(ins.p);
  }
}

void emit_method(Compiler *c, Scope *s, Buf *b) {
  /* A proc form holds its block in a real parameter, so its `yield`s are calls
     on that proc rather than an inline splice (#3399). */
  const char *sv_ypr9 = g_yield_proc_ref;
  TyKind sv_yst9 = g_yield_slot_ty;
  char ypr9[64];
  if (s->is_proc_form && s->blk_param) {
    snprintf(ypr9, sizeof ypr9, "lv_%s", s->blk_param);
    g_yield_proc_ref = ypr9;
    g_yield_slot_ty = TY_POLY;
  }

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
  /* The singleton override does not exist until the statement that created it
     has run (#4084). The object carries its parent's cls_id until then, so a
     call arriving early takes the parent's method -- emitted before SP_GC_SAVE
     so the early return has no GC state to unwind. */
  if (s->class_id >= 0 && !s->is_cmethod && c->classes[s->class_id].is_singleton_of &&
      !c->classes[s->class_id].is_value_type) {
    int pm = sg_delegate_scope(c, s);
    int sg_par = c->classes[s->class_id].parent;
    if (pm < 0 && s->name && comp_method_in_chain(c, sg_par, s->name, NULL) < 0 &&
        !comp_reader_in_chain(c, sg_par, s->name, NULL) &&
        !comp_writer_in_chain(c, sg_par, s->name, NULL)) {
      /* the singleton ADDS a name the parent does not have: before its
         statement runs there is nothing to delegate to, and CRuby answers a
         NoMethodError there (#4084) */
      buf_printf(b, "  if (!sp_class_le((sp_Class){self->cls_id}, (sp_Class){%d})) { sp_raise_nomethod(sp_nomethod_msg(", s->class_id);
      emit_str_literal(b, s->name);
      buf_printf(b, ", sp_box_obj((void *)self, %d)));", sg_par);
      if (method_is_void(s)) buf_puts(b, " return; }\n");
      else buf_printf(b, " return %s; }\n", default_value(s->ret) ? default_value(s->ret) : "0");
    }
    if (pm >= 0) {
      Scope *ps = &c->scopes[pm];
      Buf cb; memset(&cb, 0, sizeof cb);
      emit_method_cname(c, ps, &cb);
      buf_printf(&cb, "((sp_%s *)self", c->classes[c->classes[s->class_id].parent].c_name);
      for (int i = 0; i < s->nparams; i++) {
        LocalVar *a = scope_local(s, s->pnames[i]);
        LocalVar *bp = scope_local(ps, ps->pnames[i]);
        char an[128]; snprintf(an, sizeof an, "lv_%s", s->pnames[i]);
        buf_puts(&cb, ", ");
        if (!a || !bp || a->type == bp->type) buf_puts(&cb, an);
        else if (bp->type == TY_POLY) emit_boxed_text(c, a->type, an, &cb);
        else emit_unbox_text(c, bp->type, an, &cb);
      }
      if (s->blk_param && s->blk_param[0] && !s->yields) buf_printf(&cb, ", lv_%s", s->blk_param);
      buf_puts(&cb, ")");
      /* Not an equality test: the binding may have grown FURTHER links since
         (a second extend), and this body is still live for those -- the object
         carries the newest link's id and every ancestor's method has to run.
         "is my class an ancestor of the object's" is what sp_class_le answers,
         and is_a? asks it the same way. */
      buf_printf(b, "  if (!sp_class_le((sp_Class){self->cls_id}, (sp_Class){%d})) %s", s->class_id,
                 method_is_void(s) ? "{ " : "return ");
      if (method_is_void(s) || s->ret == ps->ret) buf_puts(b, cb.p ? cb.p : "");
      else if (s->ret == TY_POLY) emit_boxed_text(c, ps->ret, cb.p ? cb.p : "", b);
      else emit_unbox_text(c, s->ret, cb.p ? cb.p : "", b);
      buf_puts(b, ";");
      buf_puts(b, method_is_void(s) ? " return; }\n" : "\n");
      free(cb.p);
    }
  }
  size_t gc_save_off = b->len;
  buf_puts(b, "    SP_GC_SAVE();\n");
  size_t gc_save_len = b->len - gc_save_off;
  emit_scope_decls(c, s, b);
  TyKind saved_rt = g_ret_type;
  int saved_ed = g_ensure_depth; g_ensure_depth = 0;
  /* the body's own ensure regions reuse the stack from index 0: keep the
     enclosing function's entries, which a later break or next there reads */
  EnsureCtx saved_estk[MAX_ENSURE_DEPTH]; memcpy(saved_estk, g_ensure_stack, sizeof saved_estk);
  int saved_emcls = g_emitting_class_id; g_emitting_class_id = s->class_id;
  const char *saved_dmn = g_dm_subst_name; int saved_dmnode = g_dm_subst_node;
  g_dm_subst_name = s->dm_subst_name; g_dm_subst_node = s->dm_subst_node;
  /* A proc form holds its block as a real parameter and lowers `yield` to a
     call on it, exactly as a lowered yield method does -- so a nested lifted
     proc containing a `yield` has to capture that parameter the same way. */
  int scope_blk_is_param = s->is_lowered_yield || s->is_proc_form;
  int saved_rseed = g_ret_seeded; g_ret_seeded = s->ret_rbs_seeded;
  int saved_lowered = g_current_scope_is_lowered; g_current_scope_is_lowered = scope_blk_is_param;
  const char *saved_lbn = g_lowered_blk_name;
  g_lowered_blk_name = scope_blk_is_param ? s->blk_param : NULL;
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
    if (cmethod_takes_self_cls(c, (int)(s - c->scopes)))
      snprintf(cm_self9, sizeof cm_self9, "_sp_cls");
    else
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
    /* ...and the walk path's depth, so a non-local return out of a container
       walk (a user #inspect that returns through a proc) drops the frames the
       longjmp jumps over; the exception and catch stacks record theirs inside
       the runtime calls that open their arms, but this node is built here. */
    buf_puts(b, "    _h.exc_top = sp_exc_top; _h.catch_top = sp_catch_top;\n");
    buf_puts(b, "    _h.recur_mark = sp_poly_recur_save();\n");
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
    /* Falling off the end answers nil. For most kinds the slot's default IS
       what a caller reads back as nil, but a seeded nilable return has a
       sentinel of its own: `() -> String?` pinned the slot to `const char *`
       and the tail returned the empty string, so `.nil?` answered false and
       `compact` kept it (#4250). */
    else if (s->ret_rbs_nilable)
      buf_printf(b, "%s;\n", s->ret == TY_INT   ? "SP_INT_NIL"
                            : s->ret == TY_FLOAT ? "sp_float_nil()"
                            : s->ret == TY_STRING ? "NULL"
                                                  : default_value(s->ret));
    else buf_printf(b, "%s;\n", default_value(s->ret));
  }
  g_result_var = sv_rv2; g_result_poly = sv_rp2;
  g_method_pr_label = NULL; g_method_pr_var = NULL;
  g_fn_pr_label = NULL; g_fn_pr_var = NULL; g_fn_ret_type = TY_UNKNOWN;
  g_self_deref = saved_deref;
  g_self = saved_self9;
  g_ret_type = saved_rt; g_ensure_depth = saved_ed;
  memcpy(g_ensure_stack, saved_estk, sizeof saved_estk);
  g_emitting_class_id = saved_emcls;
  g_dm_subst_name = saved_dmn; g_dm_subst_node = saved_dmnode;
  g_current_scope_is_lowered = saved_lowered;
  g_ret_seeded = saved_rseed;
  g_lowered_blk_name = saved_lbn;
  g_brk_ser_var = saved_bser; g_brk_skip_id = saved_bskip;
  g_yield_proc_ref = sv_ypr9; g_yield_slot_ty = sv_yst9;
  buf_puts(b, "}\n");
  if (!g_no_root_elision) gc_roots_take_back(c, s, b, gc_save_off);
  if (!g_no_root_frame) gc_frame_build(b, gc_save_off + gc_save_len);
  gc_save_take_back(b, gc_save_off, gc_save_len);
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
/* Does `name` appear anywhere in this subtree once the subtree at `skip` is
   cut out? Block locals are flattened into the enclosing scope's table, so
   "declared in the block" and "belongs to the enclosing scope" look identical
   from LocalVar alone; this is what tells them apart. A name the enclosing
   scope never touches outside the block is the block's own. */
static int name_used_outside(Compiler *c, int id, int skip, const char *name) {
  if (id < 0 || id == skip || !name) return 0;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode")) {
    const char *n = nt_str(c->nt, id, "name");
    if (n && sp_streq(n, name)) return 1;
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++)
    if (name_used_outside(c, nt_ref_at(c->nt, id, i), skip, name)) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++) if (name_used_outside(c, ids[k], skip, name)) return 1;
  }
  return 0;
}

/* Does this node READ a container that outlives the expression -- storage
   something else still holds?

   It decides whether a widening CONVERSION may be emitted over it. Converting
   a typed array to a poly one COPIES, and a copy is right on a value the
   expression just created and silently wrong on a value someone else holds:
   `e = m.a` over `@a = ["seed"]` handed back a copy, so `e << 7` landed
   nowhere and `equal?` was false where CRuby says true (#4412).

   The test is READ, not "not a literal", and the difference is measured
   rather than argued: across three real applications there are nine
   conversion sites, and four of them take a call that RETURNS a new container
   (`Hash.new("")`, a range) which is safe in fact. Refusing everything that is
   not a literal would refuse six to catch two, four of them correct code.

   A reader is a call whose body is a bare ivar read, which is what both
   `attr_reader :a` and `def a; @a; end` come to. A call that builds is not
   one, and is left alone. */
/* Is scope `s` a method whose whole body is one ivar read -- what both
   `attr_reader :a` and `def a; @a; end` come to? */
static int scope_body_is_ivar_read(Compiler *c, int s) {
  if (s < 0) return 0;
  Scope *sc = &c->scopes[s];
  if (sc->body < 0) return 0;
  int n = 0; const int *st = nt_arr(c->nt, sc->body, "body", &n);
  if (n != 1 || !st) return 0;
  const char *bt = nt_type(c->nt, st[0]);
  return bt && sp_streq(bt, "InstanceVariableReadNode");
}

int conv_reads_shared_storage(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "InstanceVariableReadNode")) return 1;
  if (!sp_streq(ty, "CallNode")) return 0;
  { int ca = nt_ref(nt, node, "arguments");
    int an = 0; if (ca >= 0) nt_arr(nt, ca, "arguments", &an);
    if (an > 0 || nt_ref(nt, node, "block") >= 0) return 0; }
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  /* The reader is a method of the RECEIVER's class, so ask that class. A
     receiver the compiler already knows to be a builtin -- a Range, a typed
     array, a Hash -- reaches no user method however many classes spell one by
     the same name: `(1..max).to_a` makes a fresh array whether or not a
     `Result#to_a` answering `@rows` exists somewhere in the program, and the
     name-only scan below refused it. Only a receiver whose class is not known
     here falls back to that scan. */
  int recv = nt_ref(nt, node, "receiver");
  if (recv >= 0) {
    TyKind rt = comp_ntype(c, recv);
    if (ty_is_object(rt))
      return scope_body_is_ivar_read(c, comp_method_in_chain(c, ty_object_class(rt), nm, NULL));
    if (rt != TY_UNKNOWN && rt != TY_POLY) return 0;
  }
  for (int i = 1; i < c->nscopes; i++) {
    Scope *sc = &c->scopes[i];
    if (!sc->name || !sp_streq(sc->name, nm)) continue;
    if (scope_body_is_ivar_read(c, i)) return 1;
  }
  return 0;
}

void proc_collect_used(Compiler *c, int id, NameSet *out) {
  if (id < 0) return;
  const char *ty = nt_type(c->nt, id);
  if (!ty) return;
  if (sp_streq(ty, "LocalVariableReadNode") || sp_streq(ty, "LocalVariableWriteNode") ||
      sp_streq(ty, "LocalVariableTargetNode") || sp_streq(ty, "LocalVariableOperatorWriteNode") ||
      sp_streq(ty, "LocalVariableOrWriteNode") || sp_streq(ty, "LocalVariableAndWriteNode"))
    nameset_add(out, nt_str(c->nt, id, "name"));
  /* a yield in a lowered method calls the forwarded block, so a lifted body
     containing one captures that block even though no variable read says so */
  if (sp_streq(ty, "YieldNode")) {
    Scope *ys = comp_scope_of(c, id);
    if (ys && ys->is_lowered_yield && ys->blk_param && ys->blk_param[0])
      nameset_add(out, ys->blk_param);
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) { int ch = nt_ref_at(c->nt, id, i); if (ch >= 0) proc_collect_used(c, ch, out); }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) { int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n); for (int k = 0; k < n; k++) if (ids[k] >= 0) proc_collect_used(c, ids[k], out); }
}

/* The ParametersNode of a proc-creating node. A `->{}` LambdaNode carries it
   directly (`parameters`); a `proc {}` / `lambda {}` / block-escape pass-through
   nests it one level deeper (block/BlockNode -> BlockParametersNode -> ParametersNode). */
/* The NumberedParametersNode of a proc-creating node, or -1. proc_params_node
   answers the inner ParametersNode, which a numbered-param block does not
   have. */
int proc_numbered_params_node(Compiler *c, int create) {
  const char *ty = nt_type(c->nt, create);
  int bp;
  if (ty && sp_streq(ty, "LambdaNode")) bp = nt_ref(c->nt, create, "parameters");
  else if (ty && sp_streq(ty, "BlockNode")) bp = nt_ref(c->nt, create, "parameters");
  else {
    int block = nt_ref(c->nt, create, "block");
    bp = block >= 0 ? nt_ref(c->nt, block, "parameters") : -1;
  }
  if (bp < 0) return -1;
  const char *bt = nt_type(c->nt, bp);
  return (bt && sp_streq(bt, "NumberedParametersNode")) ? bp : -1;
}

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
  /* A numbered parameter IS a positional parameter of the proc; it just hangs
     off a NumberedParametersNode with no `requireds` list. Answering NULL here
     left the arity at 0, so `_1` was not bound from args[] and every use of it
     in the body was classified as an enclosing local -- laundered through the
     capture machinery rather than bound as the argument it is. */
  { int bpn = proc_numbered_params_node(c, create);
    if (bpn >= 0) {
      int maxn = (int)nt_int(c->nt, bpn, "maximum", 0);
      if (idx >= maxn || idx >= 9) return NULL;
      return numbered_param_name(c, bpn, idx);
    } }
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

/* Proc args + return ride the sp_int slot of sp_proc_call. A value that fits
   an sp_int directly (int/bool/symbol/nil) needs no conversion; a heap pointer
   (string/array/hash/object) is laundered through (sp_int)(uintptr_t). Other
   shapes (float, poly, range, time) don't fit the slot and defer. */
int proc_slot_is_direct(TyKind t) { return t == TY_INT || t == TY_BOOL || t == TY_SYMBOL || t == TY_NIL || t == TY_UNKNOWN; }
/* Every kind whose C representation is a bare `T *`. The runtime handles
   were missing, so a Mutex (or an IO, a Thread, a Queue, ...) yielded to a
   block through the proc ABI went into the sp_int slot uncast and came
   back out as a pointer without a cast either -- the generated C did not
   compile (#3383). Kept in step with c_type_name; sp_RbVal (poly) and
   sp_Class are structs, not pointers, and stay out. */
int proc_slot_is_ptr(TyKind t) {
  switch (t) {
    case TY_STRING: case TY_STRBUF: case TY_BIGINT: case TY_MATCHDATA:
    case TY_EXCEPTION: case TY_CURRY: case TY_FIBER: case TY_THREAD:
    case TY_QUEUE: case TY_MUTEX: case TY_CONDVAR: case TY_RANDOM:
    case TY_DIR: case TY_ADDRINFO: case TY_SOCKOPT: case TY_OPENSTRUCT:
    case TY_METHOD: case TY_IO: case TY_ARGF: case TY_ENUMERATOR:
    case TY_REGEX:
      return 1;
    default: break;
  }
  /* A narrowed object array (sp_PtrArray of unboxed sp_X*) is as much a heap
     pointer as any other array; leaving it out refused a stored block that
     captured an array holding instances of exactly one user class, while the
     same array holding two classes -- which never narrows -- compiled (#3908). */
  return ty_is_array(t) || ty_is_obj_array(t) || ty_is_hash(t) || ty_is_object(t);
}

/* A parameter shape that fits NEITHER the sp_int slot nor a pointer laundered
   through it: the by-value structs (Range, Time, Rational, Complex, Class, a
   value-type object). Like a float, such a value rides the boxed side channel
   and is unboxed in the callee -- passing it through the sp_int slot did not
   even compile (#3962). */
int proc_slot_via_poly(Compiler *c, TyKind t) {
  if (t == TY_POLY || t == TY_FLOAT) return 0;   /* their own arms handle these */
  if (proc_slot_is_direct(t) || t == TY_PROC) return 0;
  if (ty_is_object(t)) return comp_ty_value_obj(c, t);
  if (proc_slot_is_ptr(t)) return 0;
  return c_type_name(t) != NULL;   /* a shape with no C type at all still defers */
}

/* True if a closure cell for `lv` carries the variable's real typed pointer
   (string / array / hash / object), as opposed to a laundered or scalar slot.
   A typed-pointer cell is a plain `T *_cell_x` whose deref is an ordinary
   lvalue, so reads and (re)assignments need no (sp_int)(uintptr_t) cast. */
/* The scan a closure cell of this type needs. A Regexp is NOT a GC object: it
   is a compiled program the regexp engine mallocs, which sp_mark_rbval already
   excludes from sp_gc_mark for exactly this reason. The cell scan missed the
   same exclusion, so marking a captured Regexp read a GC header one byte in
   front of the engine's allocation and the collector faulted on the garbage it
   found there (#4063). NULL scan: the cell itself is GC-allocated and stays
   alive, and the pattern it points at is never collected. */
const char *cell_scan_fn(TyKind t) {
  if (t == TY_REGEX) return "NULL";
  return (t == TY_STRING) ? "sp_cell_scan_str" : "sp_cell_scan_ptr";
}

/* Types that ride a cell of their own C struct instead of laundering through
   the sp_int slot: a small by-value struct with no GC pointer in it, so the
   cell needs no scan. Float and poly keep hand-written arms (their reset values
   and their scans differ); these share one shape, so they share one arm rather
   than a fourth copy in each of the three cell prologues. #3995 gave a captured
   class its cell but stopped at TY_CLASS, leaving Range / Rational / Complex on
   the "non-integer capture" reject. */
const char *cell_value_struct(TyKind t) {
  switch (t) {
    case TY_CLASS:       return "sp_Class";
    case TY_RANGE:       return "sp_Range";
    case TY_FLOAT_RANGE: return "sp_FloatRange";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_COMPLEX:     return "sp_Complex";
    default: break;
  }
  return NULL;
}

/* The empty value a value-struct cell resets to. A class has no zero cls_id, so
   it uses the -1 sentinel the class arm has always used. */
const char *cell_value_struct_empty(TyKind t) {
  switch (t) {
    case TY_CLASS:       return "((sp_Class){-1, NULL})";
    case TY_RANGE:       return "((sp_Range){0})";
    case TY_FLOAT_RANGE: return "((sp_FloatRange){0})";
    case TY_RATIONAL:    return "((sp_Rational){0})";
    case TY_COMPLEX:     return "((sp_Complex){0})";
    default: break;
  }
  return NULL;
}

int cell_is_typed_ptr(Compiler *c, LocalVar *lv) {
  return lv && proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
}

/* Emit the C element type of `lv`'s closure cell (the cell itself is a pointer
   to this). Proc cells launder sp_Proc* through sp_int; int/bool ride sp_int;
   float/poly have native cells; a heap object uses its real pointer type. */
void emit_cell_elem_type(Compiler *c, LocalVar *lv, Buf *b) {
  if (lv && lv->type == TY_FLOAT) { buf_puts(b, "sp_float"); return; }
  if (lv && lv->type == TY_POLY) { buf_puts(b, "sp_RbVal"); return; }
  { const char *vs = lv ? cell_value_struct(lv->type) : NULL;
    if (vs) { buf_puts(b, vs); return; } }
  if (cell_is_typed_ptr(c, lv)) { emit_ctype(c, lv->type, b); return; }
  buf_puts(b, "sp_int");
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
  /* The dispatch lift hands us the BlockNode itself; the lifted-ness is a
     property of the call that owns it. */
  if (cty && sp_streq(cty, "BlockNode")) {
    int o = cg_block_owner(c, create);
    return o >= 0 ? proc_does_nonlocal_return(c, o) : 0;
  }
  if (!cty || !sp_streq(cty, "CallNode")) return 0;          /* proc/Proc.new are calls */
  const char *cn = nt_str(c->nt, create, "name");
  if (!cn) return 0;
  /* A plain block that is LIFTED to a proc (its call keeps a real &block, or a
     poly receiver's dispatch materializes it) leaves the home frame just like
     an explicit proc literal, so a `return` in it is non-local too. An inlined
     block's `return` is the method's own and never reaches here. */
  { int lblk = nt_ref(c->nt, create, "block");
    const char *lbt = lblk >= 0 ? nt_type(c->nt, lblk) : NULL;
    if (lbt && sp_streq(lbt, "BlockNode") && a_block_is_lifted(c, create))
      return proc_body_has_return(c, nt_ref(c->nt, lblk, "body")); }
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
  int nids = 0; const int *ids = cg_scope_nodes(c, si, &nids);
  for (int k = 0; k < nids; k++)
    if (proc_does_nonlocal_return(c, ids[k])) return 1;
  return 0;
}

/* Lower `Fiber.new { |param| body }` into a static void fn and sp_Fiber_new.
   No-capture case: all locals in the body are fiber-function locals; any
   reference to an outer-scope variable that is NOT heap-celled will compile
   fine only when it's a parameter of the enclosing method (passed by value).
   Captured outer locals (is_cell) are not yet supported -- those fibers will
   produce a C compile error rather than silently miscompiling. */
/* Returns 1 if a type needs a GC root when stored in a fiber capture struct.
   Specifically: the capture is a single GC POINTER, which is what both users
   assume -- the scan writes `if (_c->c_x) sp_gc_mark(...)` and the body writes
   SP_GC_ROOT, which registers `&lv_x` as a void**. A by-value struct is
   neither: the scan's truth test on it is not valid C at all, and the root
   would hand the collector the struct's first word (#4353). None of the
   value-struct types carries a pointer the collector must follow, with the
   single exception of sp_StrRange, whose two endpoints nothing marks
   anywhere -- an ivar's scan does not either, so that gap is wider than this
   function and is not closed here. */
int fiber_cap_needs_root(TyKind t) {
  if (ty_is_struct_valued(t)) return 0;
  return t == TY_STRING || t == TY_BIGINT || ty_is_array(t) || ty_is_hash(t) ||
         ty_is_object(t) || t == TY_POLY || t == TY_PROC || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR ||
         t == TY_EXCEPTION ||
         /* every other heap-backed handle: an unmarked capture is a GC UAF
            (a TCPServer captured into a Thread block was collected, #2922) */
         t == TY_IO || t == TY_DIR || t == TY_ADDRINFO || t == TY_SOCKOPT || t == TY_ENUMERATOR || t == TY_METHOD || t == TY_OPENSTRUCT ||
         t == TY_RANDOM || t == TY_CURRY || t == TY_STRBUF ||
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
  /* A nested block is not skipped: its locals are its own, but its self is
     this body's. Spliced in place it reads self here; lifted to a fiber of
     its own (a Thread inside a Thread) its capture is filled from self
     here. Stopping at the block left the outer fiber without self and the
     inner's `_t->self_ptr = self` naming a variable the outer never had
     (#4620). proc_body_uses_self has always walked through. */
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(c->nt, id, i);
    if (ch >= 0 && fiber_body_uses_self(c, ch)) return 1;
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++)
      if (ids[k] >= 0 && fiber_body_uses_self(c, ids[k])) return 1;
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
  /* `y << a << b` chains through the yielder each push answers, and a
     parenthesised inner push is transparent (#3581) */
  while (rcv >= 0 && nt_type(nt, rcv)) {
    if (sp_streq(nt_type(nt, rcv), "ParenthesesNode")) {
      int pb = nt_ref(nt, rcv, "body"); int pn = 0;
      const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
      rcv = (pn == 1 && pd) ? pd[0] : -1;
      continue;
    }
    if (sp_streq(nt_type(nt, rcv), "CallNode") && nt_str(nt, rcv, "name") &&
        sp_streq(nt_str(nt, rcv, "name"), "<<")) {
      rcv = nt_ref(nt, rcv, "receiver");
      continue;
    }
    break;
  }
  if (rcv < 0 || !nt_type(nt, rcv) || !sp_streq(nt_type(nt, rcv), "LocalVariableReadNode")) return 0;
  const char *rn = nt_str(nt, rcv, "name");
  return rn && sp_streq(rn, yname);
}


void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen, int size_node) {
  nd_stamp(nt_ref(c->nt, id, "block"), ND_BLOCK_PROC);   /* the body is a function of its own */
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

  /* Block parameter names (all requireds). A Thread with multiple args passes
     them as one poly array in resumed_value, and each param binds to an element
     (#2976); a single param binds resumed_value directly. */
  const char *bp0 = NULL;
  const char *bp_names[8]; int nbp = 0;
  const char *bp_rest = NULL;
  int bp_node = nt_ref(nt, blk, "parameters");
  if (bp_node >= 0) {
    int inner = nt_ref(nt, bp_node, "parameters");
    int pn = inner >= 0 ? inner : bp_node;
    int rn = 0; const int *reqs = nt_arr(nt, pn, "requireds", &rn);
    for (int i = 0; i < rn && nbp < 8; i++) {
      const char *pnm = nt_str(nt, reqs[i], "name");
      if (pnm) bp_names[nbp++] = pnm;
    }
    if (nbp > 0) bp0 = bp_names[0];
    /* `|*a|`: no requireds, so nothing above collected a name. The rest param
       takes every resume argument as an array. */
    if (nbp == 0) {
      int rst = nt_ref(nt, pn, "rest");
      const char *rnm = rst >= 0 ? nt_str(nt, rst, "name") : NULL;
      if (rnm) { bp_rest = rnm; bp0 = rnm; }
    }
  }
  /* multi-param binding only for a plain fiber/thread block (not a generator's
     yielder, which uses bp0 as `y`) */
  int multi_bind = !as_gen && nbp > 1;
  int rest_bind = !as_gen && bp_rest != NULL;
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
      int is_bp = 0;
      for (int bi = 0; bi < nbp; bi++) if (sp_streq(nm, bp_names[bi])) { is_bp = 1; break; }
      if (bp_rest && sp_streq(nm, bp_rest)) is_bp = 1;
      if (is_bp) continue;   /* every block param is bound below, never captured */
      LocalVar *lv = scope_local(encl, nm);
      if (!lv || lv->type == TY_UNKNOWN) continue;
      /* A name defined in the body (including a nested block's param/local) is
         a body-local, not a capture -- unless it's a celled enclosing local
         (then the write must reach the outer scope through the cell).

         "Celled" alone is not enough to say the local is the enclosing
         scope's, because block locals are flattened into that scope's table:
         `Thread.new { steps = 0; ... }` puts `steps` there too. Captured, every
         thread got the SAME cell -- eight threads counting to 3000 reported
         about 24,000 between them, silently (#4410). The enclosing scope
         TOUCHING it outside the block is what makes it shared; if it does not,
         the cell is the body's own and is allocated in its prologue. */
      if (nameset_has(&fib_decls, nm)) {
        if (!lv->is_cell) continue;
        if (!encl->body || encl->body < 0 || !name_used_outside(c, encl->body, blk, nm)) continue;
      }
      nameset_add(&caps, nm);
    }
  }
  free(fib_used.v);

  int ncap = caps.n;

  /* Resolve each capture's actual (possibly renamed) source-side identifier
     NOW, while this call site's own rename table is still the active one:
     emitting the fiber body below resets g_nren to 0 for the body's OWN
     locals (a nested inline inside it, e.g. an `each { }` the body itself
     calls, pushes its renames starting at that same index 0), overwriting
     g_ren_from/g_ren_to at the very slots this call site's names occupy.
     g_nren itself is saved and restored around the body, so the COUNT is
     right afterward, but the array CONTENTS at those low indices are not --
     a rename_local() lookup for a capture done only after the body is
     emitted can answer the body's own unrelated renamed name, or the bare
     source name once the body pushed fewer entries (`_cell_recv` instead of
     `_cell__y6_recv`, an undeclared identifier at the C level). */
  char (*cap_rn)[112] = ncap > 0 ? (char (*)[112])malloc(sizeof(char[112]) * (size_t)ncap) : NULL;
  for (int i = 0; i < ncap; i++)
    snprintf(cap_rn[i], sizeof cap_rn[0], "%s", rename_local(caps.v[i]));

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
        /* a shared cell pointer (see emit_scope_decls): float -> sp_float*,
           poly -> sp_RbVal*, heap object -> its typed pointer, else sp_int*. */
        buf_puts(&g_proc_protos, " "); emit_cell_elem_type(c, lv, &g_proc_protos);
        buf_printf(&g_proc_protos, " *c_%s;", caps.v[i]);
      }
      else {
        TyKind ct = lv ? lv->type : TY_POLY;
        buf_printf(&g_proc_protos, " "); emit_ctype(c, ct, &g_proc_protos);
        buf_printf(&g_proc_protos, " c_%s;", caps.v[i]);
      }
    }
    buf_printf(&g_proc_protos, " } _fib_cap_%d;\n", fid);
    buf_printf(&g_proc_protos, "static void _fib_cap_scan_%d(void *p) {\n", fid);
    buf_printf(&g_proc_protos, "  sp_gc_mark(p);\n");
    buf_printf(&g_proc_protos, "  _fib_cap_%d *_c = (_fib_cap_%d *)p;\n", fid, fid);
    if (cap_self && !self_is_value)
      buf_printf(&g_proc_protos, "  if (_c->self_ptr) sp_gc_mark((void *)_c->self_ptr);\n");
    else if (cap_self && class_needs_scan(&c->classes[encl->class_id]))
      buf_printf(&g_proc_protos, "  sp_%s__gc_scan(&_c->self_val);\n", cap_self_class);
    for (int i = 0; i < ncap; i++) {
      LocalVar *lv = encl ? scope_local(encl, caps.v[i]) : NULL;
      TyKind ct = lv ? lv->type : TY_POLY;
      if (lv && lv->is_cell) {
        buf_printf(&g_proc_protos, "  if (_c->c_%s) sp_gc_mark((void *)_c->c_%s);\n", caps.v[i], caps.v[i]);
      }
      else if (fiber_cap_needs_root(ct)) {
        if (ct == TY_POLY) buf_printf(&g_proc_protos, "  sp_mark_rbval(_c->c_%s);\n", caps.v[i]);
        else buf_printf(&g_proc_protos, "  if (_c->c_%s) sp_gc_mark((void *)_c->c_%s);\n", caps.v[i], caps.v[i]);
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
  size_t fib_frame_ins = pb->len;

  /* Save global emission state */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  int sv_bnren = g_block_nren;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  const char *sv_yld = g_yielder_name;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  int sv_cv = g_c_ret_void; g_c_ret_void = 1;   /* the C function is `static void` */
  /* A `return` written in a fiber/thread body cannot reach its home method:
     the body runs on its own stack, and CRuby answers the same shape with
     LocalJumpError whether the home is still on the stack or not, at the top
     level as well. Route it through sp_proc_return with an id no home can
     carry (ids come from sp_proc_home_seq, which starts at 0), so it takes
     that function's not-found tail: LocalJumpError, with the returned value
     staged as #exit_value, exactly as a proc outliving its home does. */
  const char *sv_prh_fb = g_proc_return_home; int sv_ptr_fb = g_proc_toplevel_return;
  g_proc_return_home = "-1"; g_proc_toplevel_return = 0;
  g_pre = NULL; g_indent = 1; g_nren = 0; g_block_id = blk; g_block_nren = 0;
  /* g_block_param_name is the name of the &block a body calls through
     (`blk.call(x)`, `blk[x]`), which is_block_call splices the active block
     for. A Thread.new / Fiber.new block's own first parameter is not that: it
     is a plain value, and naming it here made `ab[1]` on a yielded Array
     splice the body into itself with ab rebound to 1 (`Thread.new([lo, hi])
     { |ab| ab[1] - ab[0] }` answered a nonsense difference). Only the
     Enumerator generator's yielder rides this name (`y << v` / `y.yield v`
     lower to Fiber.yield through g_yielder_name). */
  g_block_param_name = as_gen ? bp0 : NULL; g_self = sv_self;
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
  /* A `break` written directly in a fiber/thread body -- not inside a block
     or a C loop within it -- has nothing to deliver to and cannot reach one
     across the body's own stack. It fell through to a bare C `break;` with no
     loop around it, which did not compile. g_c_ret_void marks the body and
     g_c_loop_depth tells the BreakNode emitter whether a real loop is in
     scope; where neither holds it throws a serial no live scope carries
     (sp_brk_seq starts at 1), taking sp_brk_throw's not-found tail: CRuby's
     LocalJumpError "break from proc-closure", value staged as #exit_value. */
  int sv_fbcld = g_c_loop_depth; g_c_loop_depth = 0;
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
        buf_printf(pb, " *_cell_%s = _fc->c_%s;\n", caps.v[i], caps.v[i]);
        buf_printf(pb, "    SP_GC_ROOT(_cell_%s);\n", caps.v[i]);
        continue;
      }
      TyKind ct = lv ? lv->type : TY_POLY;
      const char *rn = rename_local(caps.v[i]);
      buf_printf(pb, "    "); emit_ctype(c, ct, pb);
      buf_printf(pb, " lv_%s = _fc->c_%s;\n", rn, caps.v[i]);
      if (fiber_cap_needs_root(ct)) {
        if (ct == TY_POLY) buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", rn);
        else buf_printf(pb, "    SP_GC_ROOT(lv_%s);\n", rn);
      }
    }
  }

  /* Block param: first resume value (or nil on initial resume) */
  if (multi_bind) {
    /* the args were packed into a poly array; bind each param to an element
       (nil past the end), matching a positional block binding (#2976) */
    for (int bi = 0; bi < nbp; bi++) {
      const char *bpn = rename_local(bp_names[bi]);
      buf_printf(pb, "    sp_RbVal lv_%s = sp_poly_index_poly(_fb->resumed_value, sp_box_int(%d));\n", bpn, bi);
      buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", bpn);
    }
  }
  else if (rest_bind) {
    /* A rest param collects the resume arguments as an array: nil (no
       arguments) is empty, a packed multi-argument value is already the list,
       and a single argument becomes a one-element list. A single ARRAY
       argument is indistinguishable from a packed pair here -- the fiber
       carries one value and no arity -- so `resume([1, 2])` binds [1, 2]
       rather than [[1, 2]]; carrying the count would be the fix. */
    const char *bpn = rename_local(bp_rest);
    buf_printf(pb, "    sp_PolyArray *lv_%s = ({ sp_RbVal _rv = _fb->resumed_value;"
                   " _rv.tag == SP_TAG_NIL ? sp_PolyArray_new()"
                   " : (_rv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_rv.cls_id))"
                   " ? sp_poly_to_poly_array(_rv)"
                   " : ({ sp_PolyArray *_ra = sp_PolyArray_new(); sp_PolyArray_push(_ra, _rv); _ra; }); });\n", bpn);
    buf_printf(pb, "    SP_GC_ROOT(lv_%s);\n", bpn);
  }
  else if (bp0) {
    const char *bpn = rename_local(bp0);
    /* a generator's parameter is the yielder: as a value (a proc inside the
       body captured it) it is the fiber itself under the Yielder id, whose
       `<<` the runtime answers with Fiber.yield; the body's own `y << v` is
       lowered to Fiber.yield directly (g_yielder_name) */
    if (as_gen) buf_printf(pb, "    sp_RbVal lv_%s = sp_box_obj((void *)_fb, SP_BUILTIN_YIELDER);\n", bpn);
    else buf_printf(pb, "    sp_RbVal lv_%s = _fb->resumed_value;\n", bpn);
    buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", bpn);
    /* captured by a lifted proc: it lives in a cell, seeded from the slot */
    { LocalVar *blv = encl ? scope_local(encl, bp0) : NULL;
      if (blv && blv->is_cell && !nameset_has(&caps, bp0)) {
        buf_printf(pb, "    sp_RbVal *_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval);\n", bp0);
        buf_printf(pb, "    SP_GC_ROOT(_cell_%s);\n", bp0);
        buf_printf(pb, "    *_cell_%s = lv_%s;\n", bp0, bpn);
      } }
  }

  /* Declare fiber-body locals (those written in the body, not captured) */
  if (encl) {
    for (int i = 0; i < encl->nlocals; i++) {
      LocalVar *lv = &encl->locals[i];
      if (lv->is_param) continue;
      if (!lv->name) continue;
      { int is_bp = 0;
        for (int bi = 0; bi < nbp; bi++) if (sp_streq(lv->name, bp_names[bi])) { is_bp = 1; break; }
        if (is_bp) continue; }   /* block params declared above */
      if (nameset_has(&caps, lv->name)) continue;
      if (!nameset_has(&fib_locals, lv->name) && !nameset_has(&fib_decls, lv->name)) continue;
      if (lv->type == TY_UNKNOWN) continue;
      /* A celled local that is NOT in caps is the body's own (see the capture
         rule above): allocate its cell here, once per fiber, rather than
         unpacking a shared one from the capture struct. */
      if (lv->is_cell) { emit_cell_decl(c, encl, lv, pb); continue; }
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
  /* A yield in this body reaches the lowered method's block through the cell
     the frame carries, not a local (#3355). */
  /* This body becomes its own C function, so an enclosing C loop is not in
     scope for it. Left counted, an `ensure` inside the body emitted the
     deferred-`next` propagation as a `continue` -- outside any loop, which no
     C compiler accepts (#3949). The proc-literal emitter resets it for the
     same reason. */
  int sv_fib_loopd = g_c_loop_depth;
  g_c_loop_depth = 0;
  /* The enclosing body's ensure regions are not in scope either: an
     `ensure` in THIS body chained its deferred return / next / exception to
     the enclosing region's `_ensureN` label and `_retvN` / `_nxtfN` flags,
     which live in the enclosing function -- `Thread.new do ... ensure end`
     inside another such body did not compile (#4547). The proc-literal
     emitter starts its body at depth 0 for the same reason. */
  int sv_fib_ensd = g_ensure_depth, sv_fib_lensb = g_loop_ensure_base;
  EnsureCtx sv_fib_estk[MAX_ENSURE_DEPTH]; memcpy(sv_fib_estk, g_ensure_stack, sizeof sv_fib_estk);
  g_ensure_depth = 0; g_loop_ensure_base = 0;
  int sv_yblkc = g_yblk_celled;
  {
    const char *ybn = (g_lowered_blk_name && g_lowered_blk_name[0]) ? g_lowered_blk_name : "__yblk__";
    LocalVar *yblv = encl ? scope_local(encl, ybn) : NULL;
    if (yblv && yblv->is_cell && nameset_has(&caps, ybn)) g_yblk_celled = 1;
  }
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
          /* Everything else goes through the generic boxer. The open/close pair
             that used to stand here knows the scalars, the arrays and the
             objects, and NOT the hashes -- so a `Thread.new { h.each { } }`,
             whose tail answers the hash, emitted a bare `_fb->yielded_value =
             <sp_StrIntHash *>` with an unbalanced close paren after it (#4081).
             A by-value struct tail (`(1..3).each { }` answers its Range) had
             already needed the generic form for the same reason (#3587). */
          Buf rb2 = {0};
          emit_boxed_text(c, lty, vb.p ? vb.p : "", &rb2);
          buf_puts(pb, rb2.p ? rb2.p : "sp_box_nil()");
          free(rb2.p);
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
  if (!g_no_root_frame) gc_frame_build(pb, fib_frame_ins);
  g_c_loop_depth = sv_fib_loopd;
  g_ensure_depth = sv_fib_ensd; g_loop_ensure_base = sv_fib_lensb;
  memcpy(g_ensure_stack, sv_fib_estk, sizeof sv_fib_estk);

  /* Append the completed body to g_procs. Any nested fiber bodies emitted
     while building body_buf already appended themselves to g_procs, so they
     precede this definition there; both sit at file scope. */
  buf_puts(&g_procs, body_buf.p ? body_buf.p : "");
  free(body_buf.p);

  /* Restore emission state */
  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block; g_block_nren = sv_bnren;
  g_block_param_name = sv_bpn; g_self = sv_self; g_ret_type = sv_rt; g_c_ret_void = sv_cv;
  g_proc_return_home = sv_prh_fb; g_proc_toplevel_return = sv_ptr_fb;
  g_self_deref = sv_fbderef;
  g_result_poly = sv_rp; g_result_var = sv_rv; g_yielder_name = sv_yld;
  g_fn_pr_label = sv_fn_prl2; g_fn_pr_var = sv_fn_prv2; g_fn_ret_type = sv_fn_rt2;
  g_brk_ser_var = sv_fbser; g_brk_skip_id = sv_fbskip;
  g_c_loop_depth = sv_fbcld;
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
      /* No shadow publish here: the cell was published at the binding, at
         the top of the body, and the body may have REASSIGNED it since --
         `keep { i }; i += 100; keep { i }` had the second fill copy the
         loop's stale slot over the 100 the first proc was to see. */
      emit_indent(g_pre, g_indent);
      if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, caps.v[i]))
        buf_printf(g_pre, "_t%d->c_%s = ((%s *)_cap)->c_%s;\n", tc, caps.v[i], g_cap_struct, caps.v[i]);
      else if (lv && lv->is_cell)
        /* the rename resolved above, before the fiber body's own emission
           could clobber the table: an INLINED callee's locals are renamed
           (`only` -> `_y1234_only`) and the cell is DECLARED under the
           renamed name by emit_scope_decls, so capturing under the source
           name emits a reference to an identifier that does not exist. */
        buf_printf(g_pre, "_t%d->c_%s = _cell_%s;\n", tc, caps.v[i], cap_rn[i]);   /* the shared cell pointer */
      else
        buf_printf(g_pre, "_t%d->c_%s = lv_%s;\n", tc, caps.v[i], cap_rn[i]);
    }
    free(cap_rn);
    if (as_gen) {
      buf_printf(b, "sp_Enumerator_new_gen(%s, _t%d, ", fname, tc);
      emit_enum_size_arg(c, size_node, b);
      buf_puts(b, ")");
    }
    else {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_gc_wb((void *)_t%d); _t%d->user_data = _t%d;\n", tf, tf, tc);
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
  g_yblk_celled = sv_yblkc;
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
    /* an attr_reader is not a method in the chain -- it reads the ivar
       directly off self, which the body needs captured all the same (#3750) */
    if (nm && comp_reader_in_chain(c, class_id, nm, NULL)) return 1;
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

/* The unified value type of every `return <expr>` that returns from a lambda
   whose body is `id`: the lambda's own body plus any lexically nested
   (non-lambda, non-def) block, since a block's `return` is a non-local return
   from the enclosing lambda. TY_UNKNOWN when the lambda has no such return.
   Used to widen the lambda's C return type so an early boxed return does not
   disagree with a scalar-typed fall-through tail (#3241). */
static TyKind lambda_nonlocal_return_ty(Compiler *c, int id) {
  const char *ty = nt_type(c->nt, id);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "ReturnNode")) {
    int a = nt_ref(c->nt, id, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(c->nt, a, "arguments", &an) : NULL;
    return (an == 1) ? comp_ntype(c, av[0]) : TY_NIL;
  }
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "LambdaNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++) {
    int ch = nt_ref_at(c->nt, id, i);
    if (ch < 0 || !proc_return_descend(c, id, ch)) continue;
    TyKind s = lambda_nonlocal_return_ty(c, ch);
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++) {
      if (ids[k] < 0 || !proc_return_descend(c, id, ids[k])) continue;
      TyKind s = lambda_nonlocal_return_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
}
/* The name a parameter shows in #parameters: a block parameter renamed to
   avoid a scope collision carries a `__bp<N>` suffix that is ours, not the
   program's (#3679). */
/* Strip the shadow rename's `__bp<N>` suffix: `Proc#parameters` reports the
   name the program wrote, not the slot the rename invented. EVERY parameter
   kind has to go through this -- rest, keyword, keyword-rest and block leaked
   the suffix while the positional ones were stripped (#4045). */
static const char *param_public_name(const char *n) {
  if (!n) return n;
  const char *p = strstr(n, "__bp");
  if (!p) return n;
  { const char *q = p + 4;
    if (!*q) return n;
    while (*q >= '0' && *q <= '9') q++;
    if (*q) return n; }
  { size_t len = (size_t)(p - n);
    static char buf[128];
    if (len >= sizeof buf) len = sizeof buf - 1;
    memcpy(buf, n, len); buf[len] = 0;
    return buf; }
}

/* Lower a `proc {}` / `lambda {}` / `Proc.new {}` / `->(){}` literal: emit a
   standalone `static sp_int _proc_N(void *cap, sp_int argc, sp_int *args)`
   (sp_proc_call's ABI) into g_procs, and emit the boxing `sp_proc_new_meta(...)`
   value into `b`. */
/* 1 when `nm` is a local the BLOCK ITSELF declares -- Prism lists exactly those
   in the node's `locals`, params included, so params are asked separately. A
   name the block only READS from the enclosing scope is not there. */
static int proc_owns_local(Compiler *c, int create, const char *nm) {
  const char *locs = nt_str(c->nt, create, "locals");
  if (!locs || !*locs || !nm) return 0;
  size_t nl = strlen(nm);
  for (const char *p = locs; *p; ) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l == nl && memcmp(p, nm, nl) == 0)
      return !subtree_has_param_named_pub(c->nt, nt_ref(c->nt, create, "parameters"), nm);
    if (!e) break;
    p = e + 1;
  }
  return 0;
}

/* How an inlined method's PARAMETER is spelled as an assignment target, under
   the same rule emit_inlined_local_decl declares it by: a cell-promoted local
   is reached through `(*_cell_x)`, a plain one through `lv_x`. The three
   inline emitters bound the plain form unconditionally, so a parameter an
   inner block captures was assigned under a name nothing had declared -- the
   read path was fixed for exactly this in #4088 and the binder was not
   (#4147). `rn` is the renamed (per-inline) name. */
void emit_inlined_param_target(Compiler *c, Scope *m, const char *pname,
                               const char *rn, Buf *b) {
  LocalVar *lv = pname ? scope_local(m, pname) : NULL;
  if (lv && lv->is_cell) buf_printf(b, "(*_cell_%s) = ", rn);
  else buf_printf(b, "lv_%s = ", rn);
  (void)c;
}

/* One inlined local's declaration. A local an inner block captures is reached
   through a heap CELL, and the body keeps that form when the method is inlined
   here -- so the cell is what this frame declares, under the same renamed name.
   The three inline emitters each wrote the plain form, and `(*_cell_x)` was
   left undeclared (#4088). */
void emit_inlined_local_decl(Compiler *c, LocalVar *lv, const char *rn, Buf *b, int din) {
  if (!lv->is_cell) {
    emit_indent(b, din);
    emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, local_init_value(c, lv));
    if (lv->type == TY_POLY) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT_RBVAL(lv_%s);\n", rn); }
    else if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) {
      emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn);
    }
    return;
  }
  const char *vs = cell_value_struct(lv->type);
  emit_indent(b, din);
  emit_cell_elem_type(c, lv, b);
  buf_printf(b, " *_cell_%s = (", rn);
  emit_cell_elem_type(c, lv, b);
  buf_puts(b, " *)sp_gc_alloc(sizeof(");
  emit_cell_elem_type(c, lv, b);
  buf_puts(b, "), NULL, ");
  if (lv->type == TY_PROC) buf_puts(b, "sp_cell_scan_procint");
  else if (lv->type == TY_POLY) buf_puts(b, "sp_cell_scan_rbval");
  else if (lv->type != TY_FLOAT && !vs && cell_is_typed_ptr(c, lv)) buf_puts(b, cell_scan_fn(lv->type));
  else buf_puts(b, "NULL");
  buf_printf(b, "); SP_GC_ROOT(_cell_%s); *_cell_%s = ", rn, rn);
  if (lv->type == TY_FLOAT) buf_puts(b, "0.0");
  else if (lv->type == TY_POLY) buf_puts(b, "sp_box_nil()");
  else if (vs) buf_puts(b, cell_value_struct_empty(lv->type));
  else if (lv->type != TY_PROC && cell_is_typed_ptr(c, lv)) buf_puts(b, "NULL");
  else buf_puts(b, "0");
  buf_puts(b, ";\n");
}

static void emit_proc_literal_here(Compiler *c, int create, Buf *b);

/* The block an inline spliced in (g_block_id) is caller code: it runs under the
   self, emitting class and rename depth its definition site had, which the
   inliner parked in the yield fallbacks. emit_block_invoke restores them when
   it splices the block for a yield; a forwarded `&` / `&blk` / `Proc.new(&b)`
   that turns the same block into a proc has to as well. Otherwise the proc
   captured the inlined callee's receiver as its self, and the block's ivar
   writes landed in that object instead of the one that wrote the block. */
void emit_proc_literal(Compiler *c, int create, Buf *b) {
  if (create < 0 || create != g_block_id || !g_yield_self_fallback) {
    emit_proc_literal_here(c, create, b);
    return;
  }
  const char *sv_self = g_self, *sv_deref = g_self_deref;
  int sv_emcls = g_emitting_class_id, sv_nren = g_nren;
  g_self = g_yield_self_fallback;
  g_self_deref = g_yield_self_deref_fallback;
  g_emitting_class_id = g_yield_emitting_class_fallback;
  if (g_block_nren < g_nren) g_nren = g_block_nren;
  emit_proc_literal_here(c, create, b);
  g_self = sv_self; g_self_deref = sv_deref;
  g_emitting_class_id = sv_emcls; g_nren = sv_nren;
}

static void emit_proc_literal_here(Compiler *c, int create, Buf *b) {
  const NodeTable *nt = c->nt;
  const char *cty = nt_type(nt, create);
  int is_lambda_node = cty && sp_streq(cty, "LambdaNode");
  int is_block_node = cty && sp_streq(cty, "BlockNode");
  if (!is_lambda_node && !is_block_node && nt_ref(nt, create, "block") < 0) { unsupported(c, create, "proc literal without a block"); return; }
  nd_stamp((is_lambda_node || is_block_node) ? create : nt_ref(nt, create, "block"), ND_BLOCK_PROC);

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
  /* How many numbered parameters the proc declares. Read off the node rather
     than off the names used in the body: proc_param_name answers them now, so
     `arity` already counts them and the old "arity == 0" gate never fired --
     which left Proc#parameters reporting none. The metadata below still names
     them `_1` .. `_N`, which is what Ruby reports whatever the slot is
     called internally. */
  int nnumbered = 0;
  { int bpn = proc_numbered_params_node(c, create);
    if (bpn >= 0) {
      /* Read off the node: proc_param_name answers numbered parameters now, so
         `arity` counts them and the body-scan gate below never fires. */
      nnumbered = (int)nt_int(nt, bpn, "maximum", 0);
      if (nnumbered > 9) nnumbered = 9;
    }
    else if (arity == 0 && nposts == 0 && !(restn && restn[0]) && nopts == 0) {
      /* `-> { _1 * 10 }` carries no parameters node at all -- the numbered
         names surface as plain local reads -- so the count comes from the
         body, and the names are the literal ones (nothing renames a block
         with no node to record the new name on). */
      nnumbered = proc_numbered_max(&used);
      for (int k = 1; k <= nnumbered; k++) {
        /* NameSet stores the POINTER: use the scope-interned stable name, not
           a stack buffer. The analyze pass interned _k on this scope already. */
        char nb[4] = { '_', (char)('0' + k), 0, 0 };
        LocalVar *nlv = scope_local_intern(bs, nb);
        nameset_add(&params, nlv->name);
      }
    } }
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
    /* A local the BLOCK declares is this frame's own, not the enclosing
       frame's: capturing it made the outer frame's cell the one every
       invocation shared (so per-iteration closures all saw the last value),
       and the block-local reset then assigned a `_cell_x` no function here
       declared -- the C build stopped (#4087). The prologue declares it. */
    if (lv && lv->is_cell && proc_owns_local(c, create, nm)) { nameset_add(&locals, nm); continue; }
    if (lv && lv->is_cell) {
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      /* Float captures ride the capture struct (a real sp_float field), not the
         proc's argument slot, so they are safe even alongside a float parameter
         -- a first-class proc's `.call` passes float args through the boxed
         side-channel (sp_box_float / sp_poly_to_f), not the truncating slot. */
      int float_cell = lv->type == TY_FLOAT;
      int poly_cell = lv->type == TY_POLY;
      /* a by-value struct rides its own cell, like a float: scalar fields with
         no GC pointer of its own (#3995 for the class; the Range / Rational /
         Complex siblings were left on the reject) */
      int class_cell = cell_value_struct(lv->type) != NULL;
      if (lv->type != TY_INT && lv->type != TY_BOOL && lv->type != TY_SYMBOL &&
          lv->type != TY_UNKNOWN &&
          lv->type != TY_PROC && !float_cell && !ptr_cell && !poly_cell && !class_cell) {
        free(params.v); free(used.v); free(locals.v); free(caps.v);
        unsupported(c, create, "proc capturing a non-integer variable (later slice)");
        return;
      }
      nameset_add(&caps, nm);
    }
    else if (!nameset_has(&locals, nm)) {
      /* read of an enclosing var that wasn't celled and isn't proc-local:
         no storage exists for it inside the fn -- defer rather than miscompile */
      { static char msg[256];
        snprintf(msg, sizeof msg,
                 "proc referencing an uncaptured outer variable `%s` (later slice)", nm);
        free(params.v); free(used.v); free(locals.v); free(caps.v);
        unsupported(c, create, msg); }
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
      /* `|a,|`'s rest is an ImplicitRestNode: it names nothing and collects
         nothing, and CRuby reports the signature as exactly its required
         count (`lambda { |a,| }.arity == 1`). */
      const char *rest_ty = rest >= 0 ? nt_type(nt, rest) : NULL;
      if (rest_ty && sp_streq(rest_ty, "ImplicitRestNode")) rest = -1;
      /* Only a rest parameter makes a PROC's arity negative: optional
         keywords and a keyword rest leave it at the required count, where a
         LAMBDA reports them as negative -- unless a required keyword is there
         too, which pins the count for both (#3652). */
      int neg = rest >= 0 || (nopt > 0 && is_lambda) ||
                (is_lambda && (has_opt_kw || has_kwrest) && !has_req_kw);
      meta_arity = neg ? -(mandatory + 1) : mandatory;
      /* the trailing parameter synthesized for a `|x,|` rest is not one the
         signature has -- CRuby reports `proc { |x,| }.arity` as 1 */
      for (int k = 0; k < arity; k++) {
        const char *pnm = proc_param_name(c, create, k);
        if (pnm && strncmp(pnm, "__implicit_rest_", 16) == 0) {
          meta_arity = meta_arity < 0 ? meta_arity + 1 : meta_arity - 1;
          break;
        }
      }
    }
  }
  /* the Symbol#to_proc lambda answers -2 whatever it was synthesized with */
  if (create >= 0 && nt_int(nt, create, "stp_arity", 0)) meta_arity = -2;

  /* An explicit `return <expr>` tail is a ReturnNode; its value node is the
     argument. Treating it as the effective tail keeps the body's return ABI
     (pointer launder / poly slot) consistent with an implicit tail expression,
     so the value is not lost. (tail_ret_arg stays -1 when the tail is not a
     single-value return.) The body return type is that effective tail's type. */
  int tail_ret_arg = -1;
  int tail_is_return = 0;
  TyKind ret = TY_NIL;
  { int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    /* the destructuring assignments spliced in for a `|(a, b)|` parameter are
       ours; a body that is only those had no statements, so it answers nil
       rather than the array they read (#3679) */
    while (bn > 0 && nt_int(nt, bb[bn - 1], "destr_splice", 0)) bn--;
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
  /* A BLOCK lifted into a proc (the block of a boxed receiver's `each`
     whose class list carries a Ruby each, say) keeps the break its writer
     meant: the call site's break scope (g_brk_ser_var) is captured, and a
     top-level `break` in the body throws to it, exactly as an inlined
     block's does. It was emitted as a bare C `break` in a function with no
     loop and the C did not build (#4665, the take_while of a builtin). */
  int brk_blk = !is_lambda && is_block_node && g_brk_ser_var != NULL && block_has_top_break(c, body);
  /* a lambda whose body can top-level-break returns that value: box the ret */
  if (is_lambda && block_has_top_break(c, body)) ret = TY_POLY;
  /* A lambda with an early `return <e>` (possibly non-local, inside a nested
     block) whose type diverges from the fall-through tail must publish the
     wider (boxed) type: otherwise the early boxed return disagrees with the
     scalar C signature the tail implies (#3241). */
  if (is_lambda && ret != TY_POLY) {
    TyKind lr = lambda_nonlocal_return_ty(c, body);
    if (lr != TY_UNKNOWN && lr != ret) ret = TY_POLY;
  }
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
     unboxes, so no caller observes a poly value; the raw sp_int carrier is
     unused (`return 0`). optcarrot emits no first-class proc calls, so this is
     perf-neutral there; a hot .call boxes/unboxes a tagged scalar (no alloc),
     exactly as CRuby does. */
  if (ret != TY_VOID && ret != TY_NIL) ret = TY_POLY;
  /* The proc fn returns sp_int (the ABI); heap-pointer values (strings,
     arrays, hashes, objects) are laundered through (sp_int)(uintptr_t).
     TY_POLY and float values are stored in _sp_proc_poly_ret (file-static
     sp_RbVal) before return -- float boxed via sp_box_float -- and the call
     site reads it back (unboxing float with sp_poly_to_f).
     Range/time don't fit the slot and defer. */
  int ret_ptr = proc_slot_is_ptr(ret);
  /* No usable value: run the body for effect and return nil (0). TY_NIL as
     well as TY_VOID -- a method whose tail has no value (e.g. `puts`, or a
     bare `yield if block`) is inferred TY_NIL but emitted as a C `void`
     function (method_is_void() keys on !is_scalar_ret, which excludes
     TY_NIL). Returning its result from the sp_int proc trampoline would
     emit `return <void-call>;` and fail to compile; a TY_NIL proc returns
     nil regardless of the tail expression's value, so emit it as a
     statement and fall through to `return 0`. */
  int ret_no_value = (ret == TY_VOID || ret == TY_NIL);
  int ret_poly = (ret == TY_POLY);
  /* boxed through the poly return slot: a float unboxes via sp_poly_to_f at the
     call site; a range/time (by-value structs that don't fit the sp_int
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
  /* A class method that takes the receiving class as a leading parameter has
     it in `_sp_cls`; a lifted block's function signature is (_cap, argc, args)
     and knows nothing of it, so a sibling class-method call inside the block
     referenced an identifier that is not in scope. Carry it in the capture
     struct, the way instance self is carried (#3797). */
  int cap_cls = bs && bs->is_cmethod &&
                cmethod_takes_self_cls(c, (int)(bs - c->scopes));
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
        /* the implicit rest a trailing comma synthesizes (`|a,|`) is not a
           parameter Ruby reports at all (#4045) */
        { const char *inm = nt_str(nt, ids[i], "name");
          if (inm && strncmp(inm, "__implicit_rest", 15) == 0) continue; }
        pkind[meta_count] = pos_kind;
        /* a destructuring group has no name of its own; the synthesized one
           must not leak, and neither must a renamed local's suffix (#3679) */
        const char *rnm = nt_str(nt, ids[i], "name");
        pname[meta_count++] = (rnm && strncmp(rnm, "__destr_", 8) == 0)
                                ? -1 : comp_sym_intern(c, param_public_name(rnm));
      }
      ids = nt_arr(nt, pn, "optionals", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        { const char *inm = nt_str(nt, ids[i], "name");
          if (inm && strncmp(inm, "__implicit_rest", 15) == 0) continue; }
        pkind[meta_count] = "opt";
        { const char *onm = nt_str(nt, ids[i], "name");
          pname[meta_count++] = (onm && strncmp(onm, "__destr_", 8) == 0)
                                  ? -1 : comp_sym_intern(c, param_public_name(onm)); }
      }
      int rest = nt_ref(nt, pn, "rest");
      const char *rty = rest >= 0 ? nt_type(nt, rest) : NULL;
      if (rty && sp_streq(rty, "RestParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = param_public_name(nt_str(nt, rest, "name"));
        pkind[meta_count] = "rest";
        pname[meta_count++] = comp_sym_intern(c, nm ? nm : "*");
      }
      ids = nt_arr(nt, pn, "posts", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        pkind[meta_count] = pos_kind;
        pname[meta_count++] = comp_sym_intern(c, param_public_name(nt_str(nt, ids[i], "name")));
      }
      ids = nt_arr(nt, pn, "keywords", &n);
      for (int i = 0; i < n && meta_count < PMETA_MAX; i++) {
        const char *kt = nt_type(nt, ids[i]);
        pkind[meta_count] = (kt && sp_streq(kt, "OptionalKeywordParameterNode")) ? "key" : "keyreq";
        pname[meta_count++] = comp_sym_intern(c, param_public_name(nt_str(nt, ids[i], "name")));
      }
      int kwrest = nt_ref(nt, pn, "keyword_rest");
      const char *kwty = kwrest >= 0 ? nt_type(nt, kwrest) : NULL;
      if (kwty && sp_streq(kwty, "KeywordRestParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = param_public_name(nt_str(nt, kwrest, "name"));
        pkind[meta_count] = "keyrest";
        pname[meta_count++] = comp_sym_intern(c, nm ? nm : "**");
      }
      int bpar = nt_ref(nt, pn, "block");
      const char *bty = bpar >= 0 ? nt_type(nt, bpar) : NULL;
      if (bty && sp_streq(bty, "BlockParameterNode") && meta_count < PMETA_MAX) {
        const char *nm = param_public_name(nt_str(nt, bpar, "name"));
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
  if (ncap > 0 || cap_self || cap_cls || ret_proc || brk_blk) {
    buf_printf(&g_procs, "typedef struct {");
    for (int i = 0; i < ncap; i++) {
      LocalVar *clv = scope_local(bs, caps.v[i]);
      /* a float capture rides a native sp_float cell, a poly capture an
         sp_RbVal cell, a heap object its typed pointer (see emit_scope_decls). */
      buf_puts(&g_procs, " "); emit_cell_elem_type(c, clv, &g_procs);
      buf_printf(&g_procs, " *c_%s;", caps.v[i]);
    }
    if (cap_self && self_is_value) buf_printf(&g_procs, " sp_%s __self_val;", self_cls);
    else if (cap_self) buf_puts(&g_procs, " void *__self;");
    if (cap_cls) buf_puts(&g_procs, " sp_Class __self_cls;");
    if (ret_proc) buf_puts(&g_procs, " sp_int _home;");  /* home method's proc-return id (sp_proc_home.id) */
    if (brk_blk) buf_puts(&g_procs, " sp_int _brkhome;");  /* the call site's break serial (sp_brk_push) */
    buf_printf(&g_procs, " } _proc_cap_%d;\n", pid);
    buf_printf(&g_procs, "static void _proc_cap_scan_%d(void *p) {\n", pid);
    buf_printf(&g_procs, "  sp_gc_mark(p);\n");
    buf_printf(&g_procs, "  _proc_cap_%d *_c = (_proc_cap_%d *)p;\n", pid, pid);
    for (int i = 0; i < ncap; i++) {
      /* A BYREF parameter's cell is the CALLER's slot -- the address of a
         stack local, or a cell the caller already roots -- and not a GC
         object at all. Marking it walked a header that is not there: on the
         stack shape sp_gc_mark reads the byte before a stack address, and on
         anything that byte does not spell a known marker it dereferences the
         word as a header and calls through h->scan. That is the SIGSEGV in
         the fiber root walk (#4391), and it needs a real server to arrive
         because the capture has to outlive a park.
         Not marking loses nothing: whatever the slot is, the caller is
         holding it -- that is the whole premise of lending it. */
      LocalVar *ccl = scope_local(bs, caps.v[i]);
      if (ccl && ccl->byref_out) {
        buf_printf(&g_procs, "  /* c_%s is a byref slot: the caller roots it */\n", caps.v[i]);
        continue;
      }
      buf_printf(&g_procs, "  if (_c->c_%s) sp_gc_mark((void *)_c->c_%s);\n", caps.v[i], caps.v[i]);
    }
    if (cap_self && self_is_value) {
      if (class_needs_scan(&c->classes[bs->class_id]))
        buf_printf(&g_procs, "  sp_%s__gc_scan(&_c->__self_val);\n", self_cls);
    }
    else if (cap_self) buf_puts(&g_procs, "  if (_c->__self) sp_gc_mark(_c->__self);\n");
    buf_puts(&g_procs, "}\n");
  }

  buf_printf(&g_proc_protos, "static sp_int _proc_%d(void *_cap, sp_int argc, sp_int *args);\n", pid);

  /* Save every emission global: the proc body is a fresh function context. */
  Buf *sv_pre = g_pre; int sv_indent = g_indent, sv_nren = g_nren, sv_block = g_block_id;
  int sv_bnren = g_block_nren;
  const char *sv_bpn = g_block_param_name, *sv_self = g_self, *sv_rv = g_result_var;
  TyKind sv_rt = g_ret_type; int sv_rp = g_result_poly;
  const char *sv_cap_struct = g_cap_struct; NameSet *sv_cap_names = g_cap_names;
  int sv_ensure_depth = g_ensure_depth;
  EnsureCtx sv_estk[MAX_ENSURE_DEPTH]; memcpy(sv_estk, g_ensure_stack, sizeof sv_estk);
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
  /* a non-lambda proc written at TOP LEVEL: its `return` is a top-level
     return, which ends the script rather than just leaving the proc (#3663) */
  int sv_ptr = g_proc_toplevel_return;
  g_proc_toplevel_return = (!is_lambda && !is_block_node && !ret_proc &&
                            comp_scope_of(c, create) == &c->scopes[0]);
  g_pre = NULL; g_indent = 0; g_nren = 0; g_block_id = -1; g_block_nren = 0; g_block_param_name = NULL;
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
  g_proc_body_kind = is_lambda ? 1 : ((brk_proc || brk_blk) ? 2 : 0);
  /* serial -1 never matches a live scope: sp_brk_throw raises the CRuby
     LocalJumpError after evaluating the break value */
  char brk_acc[48] = "";
  if (brk_blk) snprintf(brk_acc, sizeof brk_acc, "((_proc_cap_%d *)_cap)->_brkhome", pid);
  g_proc_brk_home = brk_blk ? brk_acc : brk_proc ? "-1" : NULL;
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
  buf_printf(pb, "static sp_int _proc_%d(void *_cap, sp_int argc, sp_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  size_t proc_frame_ins = pb->len;
  if (ncap == 0 && !cap_self && !cap_cls && !ret_proc) buf_puts(pb, "    (void)_cap;\n");
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
  if (cap_cls) {
    buf_printf(pb, "    sp_Class _sp_cls = ((_proc_cap_%d *)_cap)->__self_cls;\n", pid);
    buf_puts(pb, "    (void)_sp_cls;\n");
  }
  /* Lambda: strict arity -- requireds + trailing posts mandatory, optionals
     widen the max, a splat rest lifts it entirely. */
  int has_kwrest = 0;
  { int pnk = proc_params_node(c, create);
    if (pnk >= 0 && nt_ref(nt, pnk, "keyword_rest") >= 0) has_kwrest = 1; }
  /* `arity` counts numbered parameters when the block carries a
     NumberedParametersNode, so adding nnumbered there counts them twice and a
     `lambda { _1 }.call("a")` was rejected as taking two. Only the
     no-parameters-node form (`-> { _1 }`) has them outside `arity`. */
  int num_extra = proc_numbered_params_node(c, create) < 0 ? nnumbered : 0;
  if (is_lambda) buf_printf(pb, "    sp_proc_lambda_arity_check(argc, %d, %d, %s, %s);\n",
                            arity + nposts + num_extra, nopts,
                            proc_has_rest(c, create) ? "TRUE" : "FALSE",
                            (nkw > 0 || has_kwrest) ? "TRUE" : "FALSE");
  /* CRuby proc auto-splat: a single Array passed to a non-lambda proc taking
     more than one positional is destructured across the parameters. Rewrite
     the argument view (both the sp_int[] slots and the boxed side-channel)
     from the array's elements before binding. */
  if (!is_lambda && arity >= 2) {
    g_needs_proc_poly_argslot = 1;
    buf_puts(pb, "    sp_int _sp_as_buf[16];\n");
    buf_puts(pb, "    if (argc == 1 && _sp_proc_poly_args[0].tag == SP_TAG_OBJ && sp_poly_is_array_kind(_sp_proc_poly_args[0].cls_id)) {\n");
    buf_puts(pb, "      sp_RbVal _sp_as_a = _sp_proc_poly_args[0];\n");
    buf_puts(pb, "      sp_int _sp_as_n = sp_poly_length(_sp_as_a); if (_sp_as_n > 16) _sp_as_n = 16;\n");
    buf_puts(pb, "      for (sp_int _i = 0; _i < _sp_as_n; _i++) {\n");
    buf_puts(pb, "        sp_RbVal _e = sp_poly_arr_get(_sp_as_a, _i);\n");
    buf_puts(pb, "        _sp_proc_poly_args[_i] = _e;\n");
    buf_puts(pb, "        _sp_as_buf[_i] = (_e.tag == SP_TAG_OBJ || _e.tag == SP_TAG_STR) ? (sp_int)(uintptr_t)_e.v.p : _e.v.i;\n");
    buf_puts(pb, "      }\n");
    buf_puts(pb, "      args = _sp_as_buf; argc = _sp_as_n;\n");
    buf_puts(pb, "    }\n");
  }
  for (int k = 0; k < arity; k++) {
    const char *p = proc_param_name(c, create, k);
    LocalVar *lv = scope_local(bs, p);
    TyKind pt = lv ? lv->type : TY_INT;
    buf_puts(pb, "    "); emit_ctype(c, pt, pb); buf_printf(pb, " lv_%s = ", p);
    /* a heap-pointer param is laundered back from the sp_int slot; a TY_POLY
       (sp_RbVal) param doesn't fit the slot, so it rides the _sp_proc_poly_args
       side-channel the call site published before the call. */
    /* A param past the supplied argument count binds nil, not the typed zero
       (CRuby fills missing block/proc params with nil). The body is already
       nil-aware for these slots; supply the matching nil sentinel. */
    if (pt != TY_POLY && pt != TY_FLOAT && proc_slot_via_poly(c, pt)) {
      /* a by-value struct rides the boxed side channel and unboxes here */
      if (k < 16) {
        g_needs_proc_poly_argslot = 1;
        buf_printf(pb, "(argc > %d) ? ", k);
        char slotx[48]; snprintf(slotx, sizeof slotx, "_sp_proc_poly_args[%d]", k);
        emit_unbox_text(c, pt, slotx, pb);
        buf_printf(pb, " : %s;\n", default_value(pt));
      }
      else buf_printf(pb, "%s;\n", default_value(pt));
    }
    else if (pt == TY_POLY || pt == TY_FLOAT) {
      /* A poly param doesn't fit the sp_int slot, so it rides the
         _sp_proc_poly_args side-channel the call site published. A float rides
         it too: a raw sp_float in the slot is value-truncated (0.7 -> 0), so
         read the boxed value back, unboxing a float with sp_poly_to_f. The
         side-channel array holds 16 slots (the proc-call ABI cap). */
      if (k < 16) {
        g_needs_proc_poly_argslot = 1;  /* channel array now lives in spinel_rt.h */
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
      if (nilv) {
        buf_printf(pb, "(argc > %d) ? args[%d] : %s;\n", k, k, nilv);
        /* The binding we just wrote is what makes this slot nullable: called
           with fewer arguments the param holds the sentinel, so boxing it in
           the body (`[a, b, c]`, `a == nil`) has to answer nil rather than
           carry INTPTR_MIN through as a truthy integer. Marked here rather
           than in analyze because this is where the fallback is decided. */
        if (lv && pt == TY_INT) lv->nullable_int = 1;
      }
      else if (pt == TY_PROC) {
        /* a Proc block param (e.g. the accumulator of a proc-composing reduce)
           rides the sp_int slot as a stuffed pointer; cast it back rather than
           assigning sp_int straight to sp_Proc * (-Wint-conversion). (#2874) */
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
      /* the int slot holds a collectable Proc: it needs a scan, like the
         method prologue's (#4077 -- this third copy of the shape was missed) */
      buf_printf(pb, "    sp_int *_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, sp_cell_scan_procint);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = (sp_int)(uintptr_t)lv_%s;%c", p, p, p, p, 10);
    }
    else if (lv->type == TY_FLOAT) {
      buf_printf(pb, "    sp_float *_cell_%s = (sp_float *)sp_gc_alloc(sizeof(sp_float), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
    else if (lv->type == TY_POLY) {
      buf_printf(pb, "    sp_RbVal *_cell_%s = (sp_RbVal *)sp_gc_alloc(sizeof(sp_RbVal), NULL, sp_cell_scan_rbval);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
    else if (cell_value_struct(lv->type)) {
      const char *vs = cell_value_struct(lv->type);
      buf_printf(pb, "    %s *_cell_%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", vs, p, vs, vs, p, p, p, 10);
    }
    else if (proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type)) {
      buf_puts(pb, "    ");
      emit_ctype(c, lv->type, pb);
      buf_printf(pb, " *_cell_%s = (", p);
      emit_ctype(c, lv->type, pb);
      buf_printf(pb, " *)sp_gc_alloc(sizeof(void *), NULL, %s);",
                 cell_scan_fn(lv->type));
      buf_printf(pb, ""
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, 10);
    }
    else {
      buf_printf(pb, "    sp_int *_cell_%s = (sp_int *)sp_gc_alloc(sizeof(sp_int), NULL, NULL);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", p, p, p, p, 10);
    }
  }
  /* A celled BODY-local: the block declares the name and an inner proc
     captures it, so this frame owns the cell. Declare it here and let the
     block-local reset at the top of the body allocate a fresh one per
     invocation -- that reset assigns `_cell_x`, and with the declaration in
     the CALLER (the capture path this local no longer takes) there was
     nothing here to assign to (#4087). */
  { const char *blocs = nt_str(nt, create, "locals");
    char nbuf[128];
    for (const char *q = blocs ? blocs : ""; *q; ) {
      const char *e = strchr(q, ',');
      size_t l = e ? (size_t)(e - q) : strlen(q);
      if (l && l < sizeof nbuf) {
        memcpy(nbuf, q, l); nbuf[l] = 0;
        LocalVar *lv = scope_local(bs, nbuf);
        if (lv && lv->is_cell && !subtree_has_param_named_pub(nt, nt_ref(nt, create, "parameters"), nbuf)) {
          /* Allocate here, not just declare: the block-local reset that would
             otherwise fill it runs only where emit_stmts sees this body as a
             BlockNode, and a proc reached through the poly enumerator never
             gets there -- the body then dereferenced a NULL cell. Each proc
             invocation is a fresh frame, so allocating in the prologue IS the
             per-invocation freshness; a reset that does run re-allocates on
             top, which stays correct. */
          const char *vs = cell_value_struct(lv->type);
          buf_puts(pb, "    ");
          emit_cell_elem_type(c, lv, pb);
          buf_printf(pb, " *_cell_%s = (", nbuf);
          emit_cell_elem_type(c, lv, pb);
          buf_puts(pb, " *)sp_gc_alloc(sizeof(");
          emit_cell_elem_type(c, lv, pb);
          buf_puts(pb, "), NULL, ");
          if (lv->type == TY_PROC) buf_puts(pb, "sp_cell_scan_procint");
          else if (lv->type == TY_POLY) buf_puts(pb, "sp_cell_scan_rbval");
          else if (lv->type != TY_FLOAT && !vs && cell_is_typed_ptr(c, lv)) buf_puts(pb, cell_scan_fn(lv->type));
          else buf_puts(pb, "NULL");
          buf_printf(pb, "); SP_GC_ROOT(_cell_%s); *_cell_%s = ", nbuf, nbuf);
          if (lv->type == TY_FLOAT) buf_puts(pb, "0.0");
          else if (lv->type == TY_POLY) buf_puts(pb, "sp_box_nil()");
          else if (vs) buf_puts(pb, cell_value_struct_empty(lv->type));
          else if (lv->type != TY_PROC && cell_is_typed_ptr(c, lv)) buf_puts(pb, "NULL");
          else buf_puts(pb, "0");
          buf_puts(pb, ";\n");
        }
      }
      if (!e) break;
      q = e + 1;
    } }
  /* Splat rest and trailing post params. Both read the boxed side-channel:
     every call path now publishes all args boxed (yield's lean ABI was
     retired for this), so any position is recoverable regardless of the
     callee's static types. CRuby non-lambda distribution: leading requireds
     from the front, posts from the back, the remainder (possibly empty) is
     the rest; missing posts bind nil. */
  if ((restn && restn[0]) || nposts > 0 || nopts > 0 || nnumbered > 0) {
    g_needs_proc_poly_argslot = 1;  /* channel array now lives in spinel_rt.h */
    /* Only the no-parameters-node form (`-> { _1 }`) binds here. Where the
       block carries a NumberedParametersNode, proc_param_name answers those
       names and the requireds loop above has already declared and bound them;
       doing it twice is a redefinition the C compiler stops on. */
    if (proc_numbered_params_node(c, create) < 0)
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
      buf_printf(pb, "    { sp_int __k = %d + %d, __hi = argc - %d; if (__hi > 16) __hi = 16;\n",
                 arity, nopts, nposts);
      buf_printf(pb, "      for (; __k < __hi; __k++) sp_PolyArray_push(lv_%s, _sp_proc_poly_args[__k]); }\n",
                 restn);
    }
    for (int j = 0; j < nposts; j++) {
      const char *pp = proc_post_name(c, create, j);
      if (!pp) continue;
      buf_printf(pb, "    sp_RbVal lv_%s = ({ sp_int __i = argc - %d + %d;\n", pp, nposts, j);
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
                       " SP_GC_ROOT_RBVAL(lv_%s); (void)lv_%s;%c", krn, krn, krn, 10);   /* the empty hash is held by nothing else */
      }
    }
  }
  /* The boxed-argument and result channels are GC roots, and nothing cleared
     them: the last call's arguments and the last call's result stayed
     reachable for the rest of the program -- half a million objects in a
     benchmark that had long since dropped them, re-marked at every
     collection. The parameters have been read out above, and a call starting
     here makes the previous result stale, so both can be dropped. Cleared
     whatever this proc's own shape is: the CALLER published into them.
     Same discipline as _sp_proc_blk. */
  buf_puts(pb, "    for (int _sp_ac = 0; _sp_ac < argc && _sp_ac < 16; _sp_ac++)"
               " _sp_proc_poly_args[_sp_ac] = sp_box_nil();\n");
  buf_puts(pb, "    _sp_proc_poly_ret = sp_box_nil();\n");
  for (int i = 0; i < locals.n; i++) {
    LocalVar *lv = scope_local(bs, locals.v[i]);
    /* a celled local is a captured var (accessed via _cap), not a fn-local */
    /* skip virtual &block slots (TY_UNKNOWN) but allow rescue-bind vars (TY_EXCEPTION) */
    /* a reassigned PARAMETER is already bound by the arg prologue above --
       re-declaring it is a C redefinition (#3309) */
    if (nameset_has(&params, locals.v[i])) continue;
    if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) declare_local(c, pb, lv, 0);
  }
  if (ret_ptr) {
    /* launder a heap-pointer return through the sp_int slot: emit the body's
       leading statements, then a prelude-wrapped `return (sp_int)(uintptr_t)(<value>)`.
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
      buf_puts(pb, "  return (sp_int)(uintptr_t)(");
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
    if (tail_ret_arg >= 0 && g_proc_toplevel_return) {
      /* a top-level proc's `return` ends the script, so the tail is a real
         return statement rather than a value handed back (#3663) */
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      for (int k = 0; k < bn; k++) emit_stmt(c, bb[k], pb, 1);
    }
    else if (tail_ret_arg >= 0) {
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
    /* Direct-slot return (sp_int carrier). A TY_UNKNOWN tail can still emit
       an sp_RbVal-valued expression -- the unresolved-call gate's
       sp_raise_nomethod(...), or a NameError-raising constant read -- which
       must not flow into the sp_int return raw. Present the carrier type to
       emit_stmts_tail so its existing gate-token coercion fires (that
       coercion deliberately skips when g_ret_type is UNKNOWN). */
    if (ret == TY_UNKNOWN) g_ret_type = TY_INT;
    emit_stmts_tail(c, body, pb, 1);
    buf_puts(pb, "  return 0;\n");
  }
  buf_puts(pb, "}\n");
  if (!g_no_root_frame) gc_frame_build(pb, proc_frame_ins);
  buf_puts(&g_procs, proc_body_buf.p ? proc_body_buf.p : "");
  free(proc_body_buf.p);
  g_c_loop_depth = sv_loopd; g_in_proc_body = sv_inproc;

  g_pre = sv_pre; g_indent = sv_indent; g_nren = sv_nren; g_block_id = sv_block; g_block_nren = sv_bnren;
  g_block_param_name = sv_bpn; g_self = sv_self; g_result_var = sv_rv; g_ret_type = sv_rt;
  g_self_deref = sv_deref;
  g_cap_struct = sv_cap_struct; g_cap_names = sv_cap_names; g_ensure_depth = sv_ensure_depth;
  memcpy(g_ensure_stack, sv_estk, sizeof sv_estk);
  g_brk_ser_var = sv_bser; g_brk_skip_id = sv_bskip;
  g_proc_body_kind = sv_pbk; g_proc_brk_home = sv_pbh;
  g_result_poly = sv_rp;
  g_method_pr_label = sv_pr_label; g_method_pr_var = sv_pr_var; g_proc_return_home = sv_prh;
  g_proc_toplevel_return = sv_ptr;
  g_exc_frame_depth = sv_excd; g_method_pr_exc_depth = sv_prexcd;
  g_rescue_save_depth = sv_rsd;
  g_fn_pr_label = sv_fn_prl; g_fn_pr_var = sv_fn_prv; g_fn_ret_type = sv_fn_rt;

  if (ncap == 0 && !cap_self && !cap_cls && !ret_proc && !brk_blk) {
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
      /* The cell pointer, as named where this proc is BUILT. Inside another
         proc that captures the same name, no `_cell_<n>` is in scope -- the
         cell arrived through that proc's own capture struct, and the nested
         proc has to forward it from there (#3416). */
      for (int i = 0; i < ncap; i++) {
        /* no shadow publish here either: see the fiber/thread fill above */
        emit_indent(g_pre, g_indent);
        if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, caps.v[i]))
          buf_printf(g_pre, "_capv_%d->c_%s = ((%s *)_cap)->c_%s;\n", pid, caps.v[i], g_cap_struct, caps.v[i]);
        else
          /* rename_local for the same reason as the sibling site above. */
          buf_printf(g_pre, "_capv_%d->c_%s = _cell_%s;\n", pid, caps.v[i], rename_local(caps.v[i]));
      }
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
      /* the receiving class, as the enclosing class method knows it: forwarded
         from another proc's capture struct when this one is nested */
      if (cap_cls) {
        emit_indent(g_pre, g_indent);
        if (g_cap_struct)
          buf_printf(g_pre, "_capv_%d->__self_cls = ((%s *)_cap)->__self_cls;\n", pid, g_cap_struct);
        else
          buf_printf(g_pre, "_capv_%d->__self_cls = _sp_cls;\n", pid);
      }
      if (ret_proc) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->_home = _h.id;\n", pid); }
      if (brk_blk) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->_brkhome = %s;\n", pid, sv_bser); }
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
         sp_streq(name, "Object")    || sp_streq(name, "Numeric") ||
         sp_streq(name, "Dir");
}

/* Returns 1 if n is a known built-in exception class name. */
/* The builtin exception class names, as one authority: is_builtin_exception_name
   in analyze_util.c. There used to be a copy here and a third in
   lib/sp_exc.c's matcher, and they drifted -- SystemCallError and the Errno::
   family reached only some of them, so `rescue SystemCallError` compiled to a
   plain name compare and never caught Errno::ENOENT. */
int is_exc_name(const char *n) {
  return is_builtin_exception_name(n);
}

/* Returns 1 if user class ci (or any ancestor) directly inherits a builtin exception. */
int class_is_exc_subclass(Compiler *c, int ci) {
  for (int k = ci, guard = 0; k >= 0 && guard < 256; guard++) {
    int sc = nt_ref(c->nt, c->classes[k].def_node, "superclass");
    const char *sn = sc >= 0 ? nt_str(c->nt, sc, "name") : NULL;
    if (sc >= 0) {
      const char *sty = nt_type(c->nt, sc);
      if (sty && (sp_streq(sty, "ConstantReadNode") || sp_streq(sty, "ConstantPathNode")) &&
          is_exc_name(sn))
        return 1;
    }
    int next = c->classes[k].parent;
    /* The parent links are not resolved yet when the rescue-arm specialization
       asks, so a two-level chain (`class B < A; class A < StandardError`) ended
       the walk at B. Follow the superclass by name instead (#3707). */
    if (next < 0 && sn) next = comp_class_index(c, sn);
    if (next == k) break;
    k = next;
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
  /* a native_struct declared under a qualified name ("IO::Buffer") carries
     it here directly -- the class is leaf-keyed and has no enclosing-class
     chain to rebuild the path from (see register_ffi_decls) */
  if (c->classes[ci].ruby_name_cache) return c->classes[ci].ruby_name_cache;
  /* collect ancestry: max 16 levels deep */
  int chain[16]; int depth = 0;
  for (int k = ci; k >= 0 && depth < 16; ) {
    chain[depth++] = k;
    k = c->classes[k].enclosing_class;
  }
  if (depth == 1) return c->classes[ci].name; /* top-level: no qualification needed */
  /* Built once and kept on the class. This used to answer out of a shared
     static buffer, so the name was only good until the NEXT call -- and a
     caller that took it, emitted an expression, and then wrote it into a
     message got whichever class that expression asked about. It named the
     wrong class in a TypeError, and made every `is_a?` compare its receiver
     against itself (#4133). The name is derived from immutable class data, so
     caching it is also less work. */
  if (c->classes[ci].ruby_name_cache) return c->classes[ci].ruby_name_cache;
  char buf[256];
  buf[0] = '\0';
  for (int i = depth - 1; i >= 0; i--) {
    const char *seg = c->classes[chain[i]].name;
    if (!seg) continue;
    /* A name qualify_colliding_classes rewrote carries its enclosing path
       already (`Brainfuck__Array`): the Ruby-visible name is the leaf, since
       the enclosers are being prepended here. */
    const char *tail = strstr(seg, "__");
    if (tail && i < depth - 1) {
      const char *last = tail;
      while (last) { const char *nx = strstr(last + 2, "__"); if (!nx) break; last = nx; }
      seg = last + 2;
    }
    if (buf[0]) strncat(buf, "::", sizeof(buf) - strlen(buf) - 1);
    strncat(buf, seg, sizeof(buf) - strlen(buf) - 1);
  }
  c->classes[ci].ruby_name_cache = strdup(buf);
  return c->classes[ci].ruby_name_cache;
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

/* True when the resolved user to_s/inspect returns a boxed sp_RbVal (its
   value flows through more than one branch type, e.g. a String on one arm
   and nil on another). Callers that consume the result as a `const char *`
   must route it through sp_poly_to_s instead of a pointer cast (#3266). */
int obj_str_ret_poly(Compiler *c, int cid, int want_inspect) {
  if (cid < 0 || cid >= c->nclasses) return 0;
  int mi = comp_method_in_chain(c, cid, want_inspect ? "inspect" : "to_s", NULL);
  return mi >= 0 && (TyKind)c->scopes[mi].ret == TY_POLY;
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

/* The C type of an ivar field of type t in a class struct. Both
   emit_class_struct and the layout check below spell it through here, so the
   check compares exactly what the struct declares. */
static void emit_ivar_field_ctype(Compiler *c, TyKind t, Buf *b) {
  /* belt and suspenders: analyze widens void/nil ivar slots to poly
     (they have no C storage type); never declare a `void` field. */
  if (t == TY_VOID || t == TY_NIL) t = TY_POLY;
  emit_ctype(c, t == TY_UNKNOWN ? TY_INT : t, b);
}

static char *ivar_field_ctype(Compiler *c, TyKind t) {
  Buf tb; memset(&tb, 0, sizeof tb);
  emit_ivar_field_ctype(c, t, &tb);
  return tb.p ? tb.p : strdup("");
}

/* An inherited method is emitted once and called through a cast to the class
   that defines it (`sp_Base_m((sp_Base *)self)`), so each class struct must
   be a common initial sequence of its subclasses' structs: the same ivars,
   in the same order, with the same C types. inherit_members keeps the names
   and the order, and the ivar fixpoints are meant to keep the types; when an
   inference path widens a subclass slot and not the base's, the two fields
   differ in width, every later member moves, and the base's methods write
   onto the wrong members. Nothing fails at that point -- the subclass reads
   a wrong value later, far from the cause -- so refuse to emit such C.
   Exception subclasses are laid out after the sp_Exception header, so only
   an ancestor on the same side of that line is compared. */
static void check_class_layout_prefix(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_native_class || is_builtin_reopen(ci->name)) continue;
    int is_exc = class_is_exc_subclass(c, i);
    for (int a = ci->parent; a >= 0; a = c->classes[a].parent) {
      ClassInfo *pc = &c->classes[a];
      if (pc->is_native_class || is_builtin_reopen(pc->name)) continue;
      if (class_is_exc_subclass(c, a) != is_exc) continue;
      if (pc->nivars > ci->nivars) {
        fprintf(stderr, "spinel: class layout: %s has %d ivars but its ancestor %s has %d\n",
                ci->name, ci->nivars, pc->name, pc->nivars);
        exit(1);
      }
      for (int k = 0; k < pc->nivars; k++) {
        if (!sp_streq(pc->ivars[k], ci->ivars[k])) {
          fprintf(stderr, "spinel: class layout: ivar slot %d is %s in %s but %s in its ancestor %s\n",
                  k, ci->ivars[k], ci->name, pc->ivars[k], pc->name);
          exit(1);
        }
        char *pt = ivar_field_ctype(c, pc->ivar_types[k]);
        char *ct = ivar_field_ctype(c, ci->ivar_types[k]);
        if (!sp_streq(pt, ct)) {
          fprintf(stderr, "spinel: class layout: %s is `%s` in %s but `%s` in its ancestor %s; "
                          "methods inherited from %s would reach %s's fields through a different layout\n",
                  ci->ivars[k], ct, ci->name, pt, pc->name, pc->name, ci->name);
          exit(1);
        }
        free(pt); free(ct);
      }
    }
  }
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
       fields after (#1415). Every base member up through `backtrace` must
       be mirrored: the rescue machinery, the GC scan, and #set_backtrace
       write/read through the base cast, so omitting one would alias
       (and overrun into) the first ivar. */
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
    /* Trailing base fields: the GC scan reads has_recv/has_key/priv_call
       through the base cast, and #set_backtrace writes `backtrace` through
       the same cast. Mirror them so the offsets match sp_Exception exactly
       and the cast stays valid for ivar-bearing subclasses too. */
    buf_puts(b, "  sp_bool has_recv;\n");
    buf_puts(b, "  sp_bool has_key;\n");
    buf_puts(b, "  sp_bool priv_call;\n");
    buf_puts(b, "  sp_StrArray *backtrace;\n");
    for (int i = 0; i < ci->nivars; i++) {
      buf_puts(b, "  ");
      emit_ivar_field_ctype(c, ci->ivar_types[i], b);
      buf_printf(b, " iv_%s;\n", iv_c(ci->ivars[i] + 1));
    }
    buf_puts(b, "};\n");
    return;
  }
  /* the typedef is forward-declared for every class first (see codegen_program)
     so a class can embed a pointer to a class defined later in the file */
  buf_printf(b, "struct sp_%s_s {\n", ci->c_name);
  buf_puts(b, "  sp_int cls_id;\n");  /* runtime class tag for virtual dispatch */
  for (int i = 0; i < ci->nivars; i++) {
    buf_puts(b, "  ");
    emit_ivar_field_ctype(c, ci->ivar_types[i], b);
    /* ivar name includes '@'; strip it for the field (mangled for a member
       like `verbose?` whose raw name is not a valid C identifier) */
    buf_printf(b, " iv_%s;\n", iv_c(ci->ivars[i] + 1));
  }
  buf_puts(b, "};\n");
}

/* A class needs a GC scan iff any ivar holds a heap reference. A String range
   is one without being a pointer: the struct sits in the object by value and
   carries two GC strings, which needs_root cannot report because the slot
   itself is not a reference (#4353). */
int class_needs_scan(ClassInfo *ci) {
  for (int i = 0; i < ci->nivars; i++) {
    if (needs_root(ci->ivar_types[i]) || ci->ivar_types[i] == TY_STR_RANGE) return 1;
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
  buf_printf(b, "static void sp_%s__gc_scan(void *p) {\n", ci->c_name);
  buf_printf(b, "  sp_%s *o = (sp_%s *)p;\n", ci->c_name, ci->c_name);
  if (is_exc_iv) {
    buf_puts(b, "  sp_mark_string(o->msg);\n");
    buf_puts(b, "  if (o->cause) sp_gc_mark(o->cause);\n");
    buf_puts(b, "  sp_mark_rbval(o->result);\n");
    buf_puts(b, "  sp_mark_rbval(o->xname);\n");
    buf_puts(b, "  sp_mark_rbval(o->xkey);\n");
    buf_puts(b, "  sp_mark_rbval(o->xrecv);\n");
    buf_puts(b, "  if (o->backtrace) sp_gc_mark(o->backtrace);\n");
  }
  for (int i = 0; i < ci->nivars; i++) {
    TyKind t = ci->ivar_types[i];
    const char *iv = iv_c(ci->ivars[i] + 1);
    if (t == TY_STRING) buf_printf(b, "  sp_mark_string(o->iv_%s);\n", iv);
    else if (t == TY_POLY) buf_printf(b, "  sp_mark_rbval(o->iv_%s);\n", iv);
    /* a by-value struct the walker cannot follow: mark what it carries */
    else if (t == TY_STR_RANGE) {
      buf_printf(b, "  sp_mark_string(o->iv_%s.first);\n", iv);
      buf_printf(b, "  sp_mark_string(o->iv_%s.last);\n", iv);
    }
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
  /* Mirror the read-side nil sentinels: an unwritten int ivar's nil is
     SP_INT_NIL and a symbol ivar's is (sp_sym)-1, neither of which is the memset
     zero pattern (symbol 0 is a real symbol), so `@x ||= v` on an unset ivar
     would otherwise keep the zero value instead of running the assignment
     (#3210). */
  if (t == TY_INT) return "SP_INT_NIL";
  if (t == TY_SYMBOL) return "((sp_sym)-1)";
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
    const char *name = iv_c(ci->ivars[i] + 1);  /* skip leading '@', mangle to a C field */
    if (ci->ivar_types[i] == TY_POLY)
      buf_printf(b, "%s%siv_%s = sp_box_nil()%s", lead, lv, name, term);
    else {
      const char *nv = ivar_scalar_nil_init(ci->ivar_types[i]);
      if (nv) buf_printf(b, "%s%siv_%s = %s%s", lead, lv, name, nv, term);
    }
  }
}

/* Was this "class" written as `module`? Module-ness lives in the AST node
   kind, not in ClassInfo, and two emitters need the same answer. */
int comp_class_is_module(Compiler *c, ClassInfo *ci) {
  const char *dt = ci ? nt_type(c->nt, ci->def_node) : NULL;
  return dt && sp_streq(dt, "ModuleNode");
}

/* The proc-form clone of class `cid`'s yielding initialize, when it has one
   and the class is built on the heap: sp_X_new then runs it, and a `new` site
   that splices the body inline allocates through sp_X_new_noinit instead.
   Without the clone sp_X_new only allocates, and a `new` whose class is known
   only at run time left every ivar unset. -1 otherwise. */
int ctor_init_proc_form(Compiler *c, int cid) {
  ClassInfo *ci = &c->classes[cid];
  if (ci->is_struct || ci->is_value_type || ci->is_native_class) return -1;
  if (comp_class_is_module(c, ci)) return -1;
  int init = comp_method_in_chain(c, cid, "initialize", NULL);
  return init >= 0 ? scope_proc_form_of(c, init) : -1;
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
      buf_puts(b, ") {\n");
      /* Root the reference arguments before the object allocation, for the
         reason the Struct constructor below does: SP_POOL_NEW can collect, and
         an argument the call site built as a fresh temporary is reachable from
         nothing else until initialize stores it. A caller's own root does not
         cover this when it lives in a statement expression, whose cleanup pops
         when that expression ends rather than when the call it feeds runs. */
      for (int i = 0; i < si->nparams; i++) {
        LocalVar *p = scope_local(si, si->pnames[i]);
        TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
        if (comp_ty_value_obj(c, pt)) continue;
        if (pt == TY_STRING)     buf_printf(b, "  SP_GC_ROOT_STR(lv_%s);\n", si->pnames[i]);
        else if (pt == TY_POLY)  buf_printf(b, "  SP_GC_ROOT_RBVAL(lv_%s);\n", si->pnames[i]);
        else if (needs_root(pt)) buf_printf(b, "  SP_GC_ROOT(lv_%s);\n", si->pnames[i]);
      }
      buf_printf(b, "  sp_%s *self = SP_POOL_NEW(%s, %s%s%s);\n",
                ci->c_name, ci->c_name,
                class_needs_scan(ci) ? "sp_" : "", class_needs_scan(ci) ? ci->c_name : "NULL",
                class_needs_scan(ci) ? "__gc_scan" : "");
      buf_puts(b, "  SP_GC_ROOT(self);\n");
      buf_printf(b, "  self->cls_id = %d;\n", ctor_cls_id(c, cid));
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
      }
      else {
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
              class_needs_scan(ci) ? "__gc_scan" : "");
    buf_puts(b, "  SP_GC_ROOT(self);\n");
    buf_printf(b, "  self->cls_id = %d;\n", ctor_cls_id(c, cid));
    for (int i = 0; i < ci->nivars; i++)
      buf_printf(b, "  self->iv_%s = a%d;\n", iv_c(ci->ivars[i] + 1), i);  /* skip leading '@' */
    /* Data instances are frozen from construction (CRuby); Struct is mutable. */
    if (ci->is_data) buf_puts(b, "  sp_gc_freeze(self);\n");
    buf_puts(b, "  return self;\n}\n");
    struct_meta:;
    /* Struct/Data #inspect (== #to_s): `#<struct Name m=v, ...>` (Data uses
       `data`). Generated unless the user redefined the method, so a user
       override wins. to_s defers to inspect, which always exists here. */
    /* the same ownership for a member named inspect (#4190) */
    { char insiv[64]; snprintf(insiv, sizeof insiv, "@%s", "inspect");
      int insidx = comp_ivar_index(ci, insiv);
      if (comp_method_in_chain(c, cid, "inspect", NULL) < 0 &&
          insidx >= 0 && ci->ivar_types[insidx] == TY_STRING &&
          comp_resolve_member(c, cid, "inspect", 0, NULL, NULL) == SP_MEMBER_ATTR) {
        buf_printf(b, "static const char *sp_%s_inspect(sp_%s *self) { return self->iv_inspect; }\n",
                   ci->name, ci->name);
      }
      else if (comp_method_in_chain(c, cid, "inspect", NULL) < 0) {
      const char *rn = class_ruby_name(c, cid); if (!rn) rn = ci->name;
      buf_printf(b, "static const char *sp_%s_inspect(sp_%s *self) {\n", ci->c_name, ci->c_name);
      buf_puts(b, "  if (!self) return \"nil\";\n");
      /* A member can hold the struct itself (`s.a = s`), and this function
         renders a member of its own class by calling straight back into
         itself. Stop at the object the render is already inside, as CRuby's
         #<struct S a=#<struct S:...>> does. Only a struct with members can be
         reached from inside itself, so a memberless one keeps its old body. */
      if (ci->nivars > 0)
        buf_printf(b, "  if (sp_poly_recur_seen(SP_POLY_RECUR_INSPECT, self, NULL)) return \"#<%s %s:...>\";\n"
                      "  int _rcm = sp_poly_recur_push(SP_POLY_RECUR_INSPECT, self, NULL);\n",
                   ci->is_data ? "data" : "struct", ci->is_anon_struct ? "" : rn);
      /* an anonymous struct class has no name to show: #<struct a=1, b=2> */
      if (ci->is_anon_struct)
        /* the space that would precede the first member is CRuby's even when
           there is none to precede: `Struct.new.new.inspect` is "#<struct >" */
        buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s%s\"); SP_GC_ROOT(s);\n",
                   ci->is_data ? "data" : "struct", ci->nivars == 0 ? " " : "");
      else
        buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s %s\"); SP_GC_ROOT(s);\n",
                   ci->is_data ? "data" : "struct", rn);
      for (int i = 0; i < ci->nivars; i++) {
        /* CRuby shows a non-identifier member as a symbol: `:verbose?=` (a
           plain identifier stays bare, `name=`) (#3110) */
        const char *mnm = ci->ivars[i] + 1;
        int simple = ((mnm[0] >= 'a' && mnm[0] <= 'z') || (mnm[0] >= 'A' && mnm[0] <= 'Z') || mnm[0] == '_');
        for (const char *q = mnm; simple && *q; q++)
          if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                (*q >= '0' && *q <= '9') || *q == '_')) simple = 0;
        buf_printf(b, "  sp_String_append(s, \"%s%s%s=\");\n", i ? ", " : " ", simple ? "" : ":", mnm);
        TyKind mt = ci->ivar_types[i];
        const char *mcn = ty_is_object(mt) ? obj_str_cname(c, ty_object_class(mt), 1) : NULL;
        if (mcn) {
          /* a struct/data (or user-#inspect) member recurses into its own inspect */
          buf_printf(b, "  sp_String_append(s, self->iv_%s ? sp_%s_inspect((sp_%s *)self->iv_%s) : \"nil\");\n",
                     iv_c(ci->ivars[i] + 1), mcn, mcn, iv_c(ci->ivars[i] + 1));
        }
        else {
          Buf ivb; memset(&ivb, 0, sizeof ivb); buf_printf(&ivb, "self->iv_%s", iv_c(ci->ivars[i] + 1));
          Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, ci->ivar_types[i], ivb.p, &bx);
          buf_printf(b, "  sp_String_append(s, sp_poly_inspect(%s));\n", bx.p);
          free(bx.p); free(ivb.p);
        }
      }
      buf_puts(b, "  sp_String_append(s, \">\");\n");
      if (ci->nivars > 0) buf_puts(b, "  sp_poly_recur_pop(_rcm);\n");
      buf_puts(b, "  return s->data;\n}\n");
      }
    }
    if (comp_method_in_chain(c, cid, "to_s", NULL) < 0) {
      /* A MEMBER named to_s owns the name, as any generated reader does in
         CRuby: `Data.define(:to_s)` answers the member, not the default
         representation. Only a String member takes this -- a non-String
         to_s falls back to the default at interpolation in CRuby too, and
         the direct call goes through the reader arm (#4190). */
      char tosiv[64]; snprintf(tosiv, sizeof tosiv, "@%s", "to_s");
      int tosidx = comp_ivar_index(ci, tosiv);
      if (tosidx >= 0 && ci->ivar_types[tosidx] == TY_STRING &&
          comp_resolve_member(c, cid, "to_s", 0, NULL, NULL) == SP_MEMBER_ATTR)
        buf_printf(b, "static const char *sp_%s_to_s(sp_%s *self) { return self->iv_to_s; }\n",
                   ci->name, ci->name);
      else
        buf_printf(b, "static const char *sp_%s_to_s(sp_%s *self) { return sp_%s_inspect(self); }\n",
                   ci->name, ci->name, ci->name);
    }
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
        buf_printf(b, "sp_Proc *const lv_%s", s->blk_param);
      }
    }
    else buf_puts(b, "void");
    buf_printf(b, ") {\n  sp_%s self = {0};\n  self.cls_id = %d;\n", ci->c_name, cid);
    emit_ivar_nil_inits(b, ci, "self.", "  ", ";\n");
    if (comp_class_is_module(c, ci)) {
      /* A MODULE has no `new` -- `Buffering.new` is a NoMethodError in Ruby --
         and its method bodies are emitted into each INCLUDER, not under the
         module's own name. So there is no sp_<Module>_initialize to call, and
         emitting the call made the constructor ill-formed: a hard error on a
         compiler that rejects implicit declarations, and a quietly built
         module instance where CRuby raises (#4167). The constructor itself
         stays, since call sites in every position name it. */
      buf_printf(b, "  sp_raise_cls(\"NoMethodError\", \"undefined method 'new' for module %s\");\n",
                 class_ruby_name(c, cid));
    }
    else if (init >= 0 && c->scopes[init].reachable && !c->scopes[init].yields) {
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
      buf_printf(b, "  sp_%s *p = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s__gc_scan);\n",
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
  int init_pf = ctor_init_proc_form(c, cid);
  buf_printf(b, "static sp_%s *sp_%s_new%s(", ci->c_name, ci->c_name, init_pf >= 0 ? "_noinit" : "");
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
      buf_printf(b, "sp_Proc *const lv_%s", s->blk_param);
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
      buf_printf(b, ") {\n  sp_%s *self = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s__gc_scan);\n",
                 ci->c_name, ci->c_name, ci->c_name, ci->c_name);
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
            class_needs_scan(ci) ? "__gc_scan" : "");
  buf_printf(b, "  SP_GC_ROOT(self);\n");
  buf_printf(b, "  self->cls_id = %d;\n", ctor_cls_id(c, cid));
  /* memset zero-inits fields, but a poly ivar's zero pattern is not nil and an
     int ivar's nil is SP_INT_NIL, so seed them before initialize runs
     (read-only ivars stay nil; written ones are overwritten). */
  emit_ivar_nil_inits(b, ci, "self->", "  ", ";\n");
  } /* close else (non-exception subclass allocation) */
  if (comp_class_is_module(c, ci)) {
    /* see the value-type branch above: a module has no `new`, and no
       sp_<Module>_initialize exists to call (#4167) */
    buf_printf(b, "  sp_raise_cls(\"NoMethodError\", \"undefined method 'new' for module %s\");\n",
               class_ruby_name(c, cid));
  }
  else if (init >= 0 && c->scopes[init].reachable && !c->scopes[init].yields) {
    buf_printf(b, "  sp_%s_initialize(", c->classes[initcls].c_name);
    if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].c_name);
    buf_puts(b, "self");
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) buf_printf(b, ", lv_%s", s->pnames[i]);
    if (init_has_blk) buf_printf(b, ", lv_%s", s->blk_param);
    buf_puts(b, ");\n");
  }
  buf_puts(b, "  return self;\n}\n");
  if (init_pf < 0) return;
  /* the full constructor: allocate, then run the body through the clone. No
     block reaches a constructor called this way. */
  Scope *s = &c->scopes[init], *pf = &c->scopes[init_pf];
  buf_printf(b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
  for (int i = 0; i < s->nparams; i++) {
    if (i) buf_puts(b, ", ");
    LocalVar *p = scope_local(s, s->pnames[i]);
    emit_ctype(c, (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY, b);
    buf_printf(b, " lv_%s", s->pnames[i]);
  }
  if (s->nparams == 0) buf_puts(b, "void");
  buf_printf(b, ") {\n  sp_%s *self = sp_%s_new_noinit(", ci->c_name, ci->c_name);
  for (int i = 0; i < s->nparams; i++) buf_printf(b, "%slv_%s", i ? ", " : "", s->pnames[i]);
  buf_puts(b, ");\n  SP_GC_ROOT(self);\n");
  buf_printf(b, "  (void)sp_%s_%s(", c->classes[initcls].c_name, mc(pf->name));
  if (initcls != cid) buf_printf(b, "(sp_%s *)", c->classes[initcls].c_name);
  buf_puts(b, "self");
  for (int i = 0; i < s->nparams; i++) {
    LocalVar *p = scope_local(s, s->pnames[i]);
    LocalVar *q = pf->pnames[i] ? scope_local(pf, pf->pnames[i]) : NULL;
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    TyKind qt = (q && q->type != TY_UNKNOWN) ? q->type : TY_POLY;
    char ln[128]; snprintf(ln, sizeof ln, "lv_%s", s->pnames[i]);
    buf_puts(b, ", ");
    if (qt == TY_POLY && pt != TY_POLY) emit_boxed_text(c, pt, ln, b);
    else if (pt == TY_POLY && qt != TY_POLY) emit_unbox_text(c, qt, ln, b);
    else buf_puts(b, ln);
  }
  buf_puts(b, ", NULL);\n  return self;\n}\n");
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
               class_needs_scan(ci) ? "__gc_scan" : "", t, t, t, cid);
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
    case TY_INT:    buf_puts(b, "(val.tag == SP_TAG_NIL ? SP_INT_NIL : (sp_int)sp_poly_to_i(val))"); break;
    case TY_FLOAT:  buf_puts(b, "(sp_float)sp_poly_to_f(val)"); break;
    case TY_STRING: buf_puts(b, "(val.tag == SP_TAG_STR ? val.v.s : NULL)"); break;
    case TY_BOOL:   buf_puts(b, "(val.tag == SP_TAG_BOOL ? val.v.b : 0)"); break;
    case TY_SYMBOL: buf_puts(b, "(val.tag == SP_TAG_SYM ? (sp_sym)val.v.i : 0)"); break;
    /* a Bignum whose value fits the inline representation arrives INT-tagged
       (sp_box_bigint normalizes, #4594): convert rather than answer NULL */
    case TY_BIGINT: buf_puts(b, "(val.tag == SP_TAG_NIL ? NULL : sp_poly_as_bigint(val))"); break;
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
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ (hash key) */
      const char *ivf = iv_c(iv);          /* C field id (mangled member) */
      buf_printf(b, "      sp_StrPolyHash_set(h, SPL(\"%s\"), ", iv);
      if (mt == TY_INT) buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", ivf, ivf);
      else if (mt == TY_STRING) buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", ivf, ivf);
      else if (mt == TY_FLOAT) buf_printf(b, "sp_box_float(o->iv_%s)", ivf);
      else if (mt == TY_BOOL) buf_printf(b, "sp_box_bool(o->iv_%s)", ivf);
      else if (mt == TY_SYMBOL) buf_printf(b, "sp_box_sym(o->iv_%s)", ivf);
      else if (mt == TY_POLY) buf_printf(b, "o->iv_%s", ivf);
      else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_obj(h, SP_BUILTIN_STR_POLY_HASH);\n    }\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* The method answers no value: its emitted C return type is `void`. A body
   whose only statement is a raise infers that, and so does one that ends in an
   assignment. */
static int scope_ret_is_void(Compiler *c, int mi) {
  TyKind r = (TyKind)c->scopes[mi].ret;
  return !ty_is_object(r) && !c_type_name(r);
}

/* A user class's own #to_json, keyed by cls_id and installed as
   sp_obj_to_json_fn: the json package asks for it before the generic field
   reflection, so an object nested in a container serializes the way CRuby's
   json does (which calls #to_json on every value). Only the two shapes that
   occur in practice are dispatched: `def to_json` and `def to_json(*args)`.

   A method that answers no value is dispatched too, for its effect. Refusing
   it here is what kept a `to_json` that only raises from ever running: the
   object fell through to the reflection arm below and serialized as if the
   method were not there, and CRuby's ArgumentError never came. The call
   answers NULL, so the document is what it always was for a method that
   returns -- and for one that does not, the raise is the answer. */
static int obj_to_json_method(Compiler *c, int cid, int *defc) {
  ClassInfo *ci = &c->classes[cid];
  if (ci->is_native_class || !ci->instantiated) return -1;
  int dc = cid;
  int mi = comp_method_in_chain(c, cid, "to_json", &dc);
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (m->is_cmethod) return -1;
  if (m->ret != TY_STRING) {
    /* The no-value arm takes only a shape the dispatch below can call and
       CRuby's json would call: an `&block` parameter is one the emitted call
       does not pass, and a private or protected #to_json is one CRuby never
       reaches, so it serializes the object instead. Both fell through to the
       reflection arm before this arm existed, and still do. */
    if (!scope_ret_is_void(c, mi) || m->blk_param) return -1;
    if (comp_method_vis_in_chain(c, cid, "to_json") != SP_VIS_PUBLIC) return -1;
  }
  if (!scope_has_callable_symbol(c, mi)) return -1;
  if (m->nparams > 1 || (m->nparams == 1 && m->rest_idx != 0)) return -1;
  if (m->nparams == 1) {
    LocalVar *lv = scope_local(m, m->pnames[0]);
    if (!lv || lv->type != TY_POLY_ARRAY) return -1;
  }
  if (defc) *defc = dc;
  return mi;
}

static int obj_to_json_any(Compiler *c) {
  if (!c->native_obj_reflect) return 0;
  for (int i = 0; i < c->nclasses; i++)
    if (obj_to_json_method(c, i, NULL) >= 0) return 1;
  return 0;
}

static void emit_obj_to_json_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_obj_to_json) return;
  buf_puts(b, "static const char *sp_obj_to_json(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (comp_class_is_module(c, &c->classes[i])) continue;   /* no instances (#4654) */
    int defc = -1;
    int mi = obj_to_json_method(c, i, &defc);
    if (mi < 0) continue;
    int vobj = comp_ty_value_obj(c, ty_object(defc));
    int novalue = scope_ret_is_void(c, mi);
    buf_printf(b, "    case %d: %s", i, novalue ? "" : "return ");
    buf_printf(b, "sp_%s_%s(%s(sp_%s *)v.v.p%s);",
               c->classes[defc].c_name, mc(c->scopes[mi].name), vobj ? "*" : "",
               c->classes[defc].c_name,
               c->scopes[mi].nparams == 1 ? ", sp_PolyArray_new()" : "");
    buf_puts(b, novalue ? " return NULL;\n" : "\n");
  }
  buf_puts(b, "    default: return NULL;\n  }\n}\n");
}

/* A user-defined #deconstruct_keys on a plain class. The hash-pattern path
   reads its subject through sp_obj_to_h_fn, which knew only Struct and Data --
   so a subject whose class is a UNION of two user classes matched nothing,
   while a single class matched because its type is static there and the
   method is called directly (#4019). */
static int obj_deconstruct_keys_method(Compiler *c, int ci, int *defc) {
  int dc = -1;
  int mi = comp_method_in_chain(c, ci, "deconstruct_keys", &dc);
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (m->is_cmethod || !ty_is_hash(m->ret) || !m->reachable) return -1;
  if (m->nparams > 1 || (m->nparams == 1 && m->rest_idx == 0)) return -1;
  if (m->nparams == 1) {
    LocalVar *plv = scope_local(m, m->pnames[0]);
    if (!plv || (plv->type != TY_POLY_ARRAY && plv->type != TY_POLY &&
                 plv->type != TY_UNKNOWN)) return -1;
  }
  if (defc) *defc = dc;
  return mi;
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
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ (sym key) */
      const char *ivf = iv_c(iv);          /* C field id (mangled member) */
      buf_printf(b, "      sp_SymPolyHash_set(h, sp_sym_intern(\"%s\"), ", iv);
      if (mt == TY_INT) buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", ivf, ivf);
      else if (mt == TY_STRING) buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", ivf, ivf);
      else if (mt == TY_FLOAT) buf_printf(b, "sp_box_float(o->iv_%s)", ivf);
      else if (mt == TY_BOOL) buf_printf(b, "sp_box_bool(o->iv_%s)", ivf);
      else if (mt == TY_SYMBOL) buf_printf(b, "sp_box_sym(o->iv_%s)", ivf);
      else if (mt == TY_POLY) buf_printf(b, "o->iv_%s", ivf);
      else {
        char fb[128]; snprintf(fb, sizeof fb, "o->iv_%s", ivf);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mt, fb, &bx);
        buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
      }
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_obj(h, SP_BUILTIN_SYM_POLY_HASH);\n    }\n");
  }
  /* a plain class with its own #deconstruct_keys answers through it. A
     module has no instances and its methods are emitted only as the
     includer's, so an arm for it named a function no TU defines and the
     program did not link (#4654, the shape of #4533 at this switch). */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_struct || ci->is_data || !ci->instantiated || ci->is_native_class) continue;
    if (comp_class_is_module(c, ci)) continue;
    int defc = -1;
    int mi = obj_deconstruct_keys_method(c, i, &defc);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    int vobj = comp_ty_value_obj(c, ty_object(defc));
    char argb[64]; argb[0] = 0;
    if (m->nparams == 1) {
      LocalVar *plv = scope_local(m, m->pnames[0]);
      snprintf(argb, sizeof argb, ", %s",
               (plv && plv->type == TY_POLY_ARRAY) ? "sp_PolyArray_new()" : "sp_box_nil()");
    }
    char callb[256];
    snprintf(callb, sizeof callb, "sp_%s_%s(%s(sp_%s *)v.v.p%s)",
             c->classes[defc].c_name, mc(m->name), vobj ? "*" : "",
             c->classes[defc].c_name, argb);
    buf_printf(b, "    case %d: return ", i);
    Buf bx; memset(&bx, 0, sizeof bx);
    emit_boxed_text(c, m->ret, callb, &bx);
    buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
    buf_puts(b, ";\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* The member array of a Struct, installed as sp_obj_struct_values_fn: the
   members in declaration order, boxed as the to_h dispatch above boxes them,
   keyed by cls_id so a Struct read out of a poly container answers `values`,
   `values_at` and a blockless `each` at run time. Not the to_a hook: that one
   calls a #to_a the program wrote on the Struct. A Data has none of the three
   names in CRuby and gets no arm. */
static void emit_obj_struct_values_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_obj_struct_values) return;
  buf_puts(b, "static sp_RbVal sp_obj_struct_values(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_struct || ci->is_data || !ci->instantiated) continue;
    buf_printf(b, "    case %d: {\n", comp_class_index(c, ci->name));
    buf_printf(b, "      sp_%s *o = (sp_%s *)v.v.p; (void)o;\n", ci->c_name, ci->c_name);
    buf_puts(b, "      sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);\n");
    for (int j = 0; j < ci->nivars; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *ivf = iv_c(ci->ivars[j] + 1);
      buf_puts(b, "      sp_PolyArray_push(a, ");
      if (mt == TY_INT) buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", ivf, ivf);
      else if (mt == TY_STRING) buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", ivf, ivf);
      else if (mt == TY_FLOAT) buf_printf(b, "sp_box_float(o->iv_%s)", ivf);
      else if (mt == TY_BOOL) buf_printf(b, "sp_box_bool(o->iv_%s)", ivf);
      else if (mt == TY_SYMBOL) buf_printf(b, "sp_box_sym(o->iv_%s)", ivf);
      else if (mt == TY_POLY) buf_printf(b, "o->iv_%s", ivf);
      else {
        char fb[128]; snprintf(fb, sizeof fb, "o->iv_%s", ivf);
        Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mt, fb, &bx);
        buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
      }
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_poly_array(a);\n    }\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* User-object #to_a, installed as sp_obj_to_a_fn: cls_id switch over every
   instantiated class defining a no-arg to_a with a callable symbol, so a
   container-read Set (or any to_a-bearing object) can be iterated by the
   generic poly machinery (#3234). */
/* The method that materializes this class's elements: its own #to_a, or the
   __enum_to_a synthesized for a class that includes Enumerable and defines
   #each. Without the second, an instance of such a class read out of a
   container was opaque to the poly machinery, which answered for an empty
   collection (0 / nil / false) (#3761). */
static int obj_to_a_method(Compiler *c, int cid, int *defc) {
  int mi = comp_method_in_chain(c, cid, "to_a", defc);
  if (mi >= 0 && c->scopes[mi].nparams == 0 && scope_has_callable_symbol(c, mi)) return mi;
  /* Not for a Data class: CRuby's Data has no #to_a at all (Struct does), and
     answering its members here turned that NameError into an array. */
  if (cid >= 0 && cid < c->nclasses && c->classes[cid].is_data) return -1;
  mi = comp_method_in_chain(c, cid, "__enum_to_a", defc);
  if (mi >= 0 && c->scopes[mi].nparams == 0 && scope_has_callable_symbol(c, mi)) return mi;
  return -1;
}
static int obj_to_a_any(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_native_class || !ci->instantiated || comp_class_is_module(c, ci)) continue;   /* a module has no instances (#4654) */
    if (obj_to_a_method(c, i, NULL) >= 0) return 1;
  }
  return 0;
}
static void emit_obj_to_a_dispatch(Compiler *c, Buf *b) {
  if (!obj_to_a_any(c)) return;
  buf_puts(b, "static sp_RbVal sp_obj_to_a(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_native_class || !ci->instantiated || comp_class_is_module(c, ci)) continue;   /* a module has no instances (#4654) */
    int defc = -1;
    int mi = obj_to_a_method(c, i, &defc);
    if (mi < 0) continue;
    TyKind mret = (TyKind)c->scopes[mi].ret;
    buf_printf(b, "    case %d: {\n", i);
    char callx[256];
    /* a value-type class is passed by value, not behind a pointer */
    int vobj = comp_ty_value_obj(c, ty_object(defc));
    snprintf(callx, sizeof callx, vobj ? "sp_%s_%s(*(sp_%s *)v.v.p)" : "sp_%s_%s((sp_%s *)v.v.p)",
             c->classes[defc].c_name, mc(c->scopes[mi].name), c->classes[defc].c_name);
    buf_puts(b, "      return ");
    if (mret == TY_POLY) buf_puts(b, callx);
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mret, callx, &bx);
           buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(b, ";\n    }\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}

/* User-object #to_ary, installed as sp_obj_to_ary_fn: the CONVERSION protocol
   Kernel#Array asks first, distinct from to_a (enumeration). Only classes that
   define a no-arg to_ary get a case; sp_kernel_array falls to the to_a hook
   and then to wrapping, so an absent dispatch costs nothing (#4187). */
static int obj_to_ary_method(Compiler *c, int cid, int *defc) {
  int mi = comp_method_in_chain(c, cid, "to_ary", defc);
  if (mi >= 0 && c->scopes[mi].nparams == 0 && scope_has_callable_symbol(c, mi)) return mi;
  return -1;
}
static int obj_to_ary_any(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_native_class || !ci->instantiated || comp_class_is_module(c, ci)) continue;   /* a module has no instances (#4654) */
    if (obj_to_ary_method(c, i, NULL) >= 0) return 1;
  }
  return 0;
}
static void emit_obj_to_ary_dispatch(Compiler *c, Buf *b) {
  if (!obj_to_ary_any(c)) return;
  buf_puts(b, "static sp_RbVal sp_obj_to_ary(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (ci->is_native_class || !ci->instantiated || comp_class_is_module(c, ci)) continue;   /* a module has no instances (#4654) */
    int defc = -1;
    int mi = obj_to_ary_method(c, i, &defc);
    if (mi < 0) continue;
    TyKind mret = (TyKind)c->scopes[mi].ret;
    buf_printf(b, "    case %d: return ", i);
    char callx[256];
    int vobj = comp_ty_value_obj(c, ty_object(defc));
    snprintf(callx, sizeof callx, vobj ? "sp_%s_%s(*(sp_%s *)v.v.p)" : "sp_%s_%s((sp_%s *)v.v.p)",
             c->classes[defc].c_name, mc(c->scopes[mi].name), c->classes[defc].c_name);
    if (mret == TY_POLY) buf_puts(b, callx);
    else { Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mret, callx, &bx);
           buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p); }
    buf_puts(b, ";\n");
  }
  buf_puts(b, "    default: return sp_box_nil();\n  }\n}\n");
}
/* Data#with copy-update, installed as sp_obj_with_fn. cls_id switch over every
   instantiated Data: construct a fresh instance whose members come from the
   symbol-keyed override hash `ov` where present, else copied from the receiver.
   Mirrors the typed Data#with emitter for a poly receiver (#2890). */
/* User-object #deconstruct, installed as sp_obj_deconstruct_fn: what a
   `case/in` array pattern matches a boxed element against. A Data answers
   #deconstruct with its members but has no #to_a at all, so the to_a dispatch
   deliberately skips it and a nested array sub-pattern never matched a Data
   element (#3882). Everything else defers to that dispatch. */
/* The scope of class i's own #deconstruct when it has one this TU emits
   and a boxed value can call (no arguments, no block), else -1. A Struct's
   comes from the Struct machinery, not from here. */
static int user_deconstruct_scope(Compiler *c, int i, int *defcls) {
  ClassInfo *ci = &c->classes[i];
  if (!ci->instantiated || ci->is_native_class || ci->is_data) return -1;
  int mi = comp_method_in_chain(c, i, "deconstruct", defcls);
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (!m->reachable || m->yields || m->nparams != 0 || scope_is_shadowed(c, mi) ||
      m->is_transplanted_source) return -1;
  return mi;
}
static int obj_deconstruct_any(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (c->classes[i].is_data && c->classes[i].instantiated && !c->classes[i].is_native_class)
      return 1;
    if (user_deconstruct_scope(c, i, NULL) >= 0) return 1;
  }
  return 0;
}
static void emit_obj_deconstruct_dispatch(Compiler *c, Buf *b) {
  if (!obj_deconstruct_any(c)) return;
  buf_puts(b, "static sp_RbVal sp_obj_deconstruct(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    /* a class with its own #deconstruct answers with it (`in Pt[a, b]` on a
       Pt read out of a mixed array) */
    int dcls = -1, dmi = user_deconstruct_scope(c, i, &dcls);
    if (dmi >= 0) {
      const char *dcn = c->classes[dcls].c_name;
      char call[256];
      snprintf(call, sizeof call, "sp_%s_deconstruct(%s(sp_%s *)v.v.p)",
               dcn, c->classes[dcls].is_value_type ? "*" : "", dcn);
      buf_printf(b, "    case %d: return ", i);
      emit_boxed_text(c, c->scopes[dmi].ret, call, b);
      buf_puts(b, ";\n");
      continue;
    }
    if (!ci->is_data || !ci->instantiated || ci->is_native_class) continue;
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)v.v.p; (void)o;\n", ci->c_name, ci->c_name);
    buf_puts(b, "      sp_PolyArray *_a = sp_PolyArray_new(); SP_GC_ROOT(_a);\n");
    for (int j = 0; j < ci->nivars; j++) {
      char fld[300];
      snprintf(fld, sizeof fld, "o->iv_%s", iv_c(ci->ivars[j] + 1));
      buf_puts(b, "      sp_PolyArray_push(_a, ");
      Buf bx; memset(&bx, 0, sizeof bx);
      emit_boxed_text(c, ci->ivar_types[j], fld, &bx);
      buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
      buf_puts(b, ");\n");
    }
    buf_puts(b, "      return sp_box_poly_array(_a);\n    }\n");
  }
  buf_puts(b, "    default: return sp_obj_to_a_fn ? sp_obj_to_a_fn(v) : sp_box_nil();\n  }\n}\n");
}

/* Which cls_ids are Data classes. Data defines no #dig, and the runtime's dig
   walk has to tell one from a Struct, which does (#3919). */
static void emit_obj_is_data(Compiler *c, Buf *b) {
  if (!obj_deconstruct_any(c)) return;
  buf_puts(b, "static int sp_obj_is_data(int cls_id) {\n  switch (cls_id) {\n");
  int any = 0;
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_data || !ci->instantiated || ci->is_native_class) continue;
    buf_printf(b, "    case %d:\n", i);
    any = 1;
  }
  /* the table may be here for a class's own #deconstruct alone, with no
     Data to list */
  if (any) buf_puts(b, "      return 1;\n");
  buf_puts(b, "    default: return 0;\n  }\n}\n");
}

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
      const char *iv = ci->ivars[j] + 1;   /* sym key */
      const char *ivf = iv_c(iv);           /* C field id */
      if (j) buf_puts(b, ", ");
      /* a per-member statement-expr with its OWN found flag: sp_X_new's args
         evaluate in unspecified order, so a shared flag would race. */
      buf_printf(b, "({ sp_bool _f; sp_RbVal _pv = sp_poly_hash_probe(ov, sp_box_sym(sp_sym_intern(\"%s\")), &_f); _f ? ", iv);
      Buf ub; memset(&ub, 0, sizeof ub); emit_unbox_text(c, mt, "_pv", &ub);
      buf_puts(b, ub.p ? ub.p : "_pv"); free(ub.p);
      buf_printf(b, " : o->iv_%s; })", ivf);
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
/* One conversion-bridge switch (see the caller's comment): forward decls for
   every callee it names, then the cls_id switch. `with_ok` adds the *ok
   out-flag the sp_int form needs (NULL can carry "no method" for a string). */
static int conv_bridge_callee(Compiler *c, int i, const char *mname, TyKind want,
                              int any_shape, int *out_mi) {
  ClassInfo *ci2 = &c->classes[i];
  if (is_builtin_reopen(ci2->name) || ci2->is_native_class) return -1;
  if (!any_shape && comp_ty_value_obj(c, ty_object(i))) return -1;
  int dn = ci2->def_node;
  const char *dt = dn >= 0 ? nt_type(c->nt, dn) : NULL;
  if (dt && sp_streq(dt, "ModuleNode")) return -1;   /* no instances */
  int tdef = -1;
  int tmi = comp_method_in_chain(c, i, mname, &tdef);
  /* a no-parameter method only (a `&block` parameter is not in nparams; the
     bridges pass it as no block, which is what a bare call passes). A method
     that yields is inlined at its call sites and has no function of its own
     to call, so the bridge cannot reach it: the class counts as not having
     the method, which is also what the Kernel#Integer site answered before. */
  if (tmi < 0 || !c->scopes[tmi].reachable || c->scopes[tmi].nparams != 0 ||
      c->scopes[tmi].yields) return -1;
  /* the Kernel#Integer / #Float bridge takes the method whatever it answers
     (`any_shape`), and a value-type class's too, called on the boxed copy */
  /* The answer's static type may be the wanted one or boxed: a boxed answer
     is unwrapped (and judged) by the bridge row itself, so it is eligible
     too. Anything else -- a #to_int answering a String -- is not a
     conversion the protocol can use. */
  if (!any_shape && c->scopes[tmi].ret != want && c->scopes[tmi].ret != TY_POLY) return -1;
  int ddn = c->classes[tdef].def_node;
  const char *ddt = ddn >= 0 ? nt_type(c->nt, ddn) : NULL;
  *out_mi = tmi;
  /* a module def is copied into the includer; an ancestor CLASS def is one
     real function the child casts into */
  return (ddt && sp_streq(ddt, "ModuleNode")) ? i : tdef;
}
/* A conversion method declared with a named `&block` parameter takes it as a
   second C parameter (emit_method_signature's rule: a named block on a method
   that does not yield); the bridges declare it and pass no block, as a bare
   call does. An anonymous `&` adds no parameter. */
static int bridge_has_blk(Compiler *c, int mi) {
  Scope *s = &c->scopes[mi];
  return s->blk_param && s->blk_param[0] && !s->yields;
}
static const char *bridge_blk_param(Compiler *c, int mi) { return bridge_has_blk(c, mi) ? ", sp_Proc *blk" : ""; }
static const char *bridge_blk_arg(Compiler *c, int mi)   { return bridge_has_blk(c, mi) ? ", NULL" : ""; }

static void emit_conv_bridge(Compiler *c, Buf *b, const char *mname, TyKind want,
                             const char *rett, const char *sig, int with_ok,
                             const char *dflt) {
  for (int i = 0; i < c->nclasses; i++) {
    int tmi = -1;
    int callee = conv_bridge_callee(c, i, mname, want, 0, &tmi);
    if (callee != i) continue;   /* an ancestor's own row declares it */
    int poly_ret = c->scopes[tmi].ret == TY_POLY;
    buf_printf(b, "%s%s sp_%s_%s(sp_%s *self%s);\n", g_debug ? "" : "static ",
               poly_ret ? "sp_RbVal" : rett,
               c->classes[callee].c_name, mc(c->scopes[tmi].name),
               c->classes[callee].c_name, bridge_blk_param(c, tmi));
  }
  buf_printf(b, "%s {\n  switch (cls_id) {\n", sig);
  for (int i = 0; i < c->nclasses; i++) {
    int tmi = -1;
    int callee = conv_bridge_callee(c, i, mname, want, 0, &tmi);
    if (callee < 0) continue;
    /* A conversion whose answer is BOXED is still a conversion. Its static
       type is the analysis's business and changes with the mode -- under
       --int-overflow=promote a method returning a plain `1` can be poly --
       but the protocol is CRuby's: call it, and judge the answer. Judged
       here rather than refused at compile time, a class whose #to_int the
       analysis happened to widen kept its conversion, where before the row
       was dropped and every boxed use of the object raised "no implicit
       conversion" (#4747). An answer of the wrong kind is not-ok, which is
       the TypeError CRuby raises for exactly that. */
    if (c->scopes[tmi].ret == TY_POLY) {
      buf_printf(b, "    case %d: { sp_RbVal _cv = sp_%s_%s((sp_%s *)p%s);\n",
                 i, c->classes[callee].c_name, mc(c->scopes[tmi].name),
                 c->classes[callee].c_name, bridge_blk_arg(c, tmi));
      if (want == TY_INT)
        buf_printf(b, "      if (_cv.tag == SP_TAG_INT && _cv.v.i != SP_INT_NIL) { %sreturn _cv.v.i; }\n",
                   with_ok ? "*ok = 1; " : "");
      else
        buf_printf(b, "      if (_cv.tag == SP_TAG_STR && _cv.v.s) { %sreturn _cv.v.s; }\n",
                   with_ok ? "*ok = 1; " : "");
      buf_printf(b, "      %s }\n", dflt);
      continue;
    }
    buf_printf(b, "    case %d: %sreturn sp_%s_%s((sp_%s *)p%s);\n",
               i, with_ok ? "*ok = 1; " : "",
               c->classes[callee].c_name, mc(c->scopes[tmi].name),
               c->classes[callee].c_name, bridge_blk_arg(c, tmi));
  }
  buf_printf(b, "    default: %s\n  }\n}\n", dflt);
}

/* The Kernel#Integer / Kernel#Float bridge (sp_obj_conv_fn): the runtime's
   conversion search reaches a boxed user object's #to_int, #to_i, #to_f and
   #to_str through it WHATEVER each method's static type. CRuby calls the
   method and judges the answer, so a #to_int answering a String or a
   Bignum, or nothing at all (a body that raises), is called and its answer
   boxed for the runtime to judge. Answers 1 with the boxed value, 0 for a
   class without the method -- and asked with a NULL `out`, whether the
   method exists, calling nothing. Emitted only for a program that calls
   Kernel#Integer or Kernel#Float somewhere (Compiler.uses_kconv). */
static void emit_kconv_bridge(Compiler *c, Buf *b) {
  static const char *const names[] = { "to_int", "to_i", "to_f", "to_str" };
  for (int i = 0; i < c->nclasses; i++) {
    for (int w = 0; w < 4; w++) {
      int tmi = -1;
      int callee = conv_bridge_callee(c, i, names[w], TY_UNKNOWN, 1, &tmi);
      if (callee != i) continue;   /* an ancestor's own row declares it */
      buf_puts(b, g_debug ? "" : "static ");
      emit_ctype(c, (TyKind)c->scopes[tmi].ret, b);
      buf_printf(b, " sp_%s_%s(sp_%s %sself%s);\n", c->classes[callee].c_name,
                 mc(c->scopes[tmi].name), c->classes[callee].c_name,
                 comp_ty_value_obj(c, ty_object(callee)) ? "" : "*", bridge_blk_param(c, tmi));
    }
  }
  buf_puts(b, "static int sp_obj_conv_sw(int cls_id, void *p, int which, sp_RbVal *out) {\n"
              "  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    int rows = 0;
    for (int w = 0; w < 4; w++) {
      int tmi = -1;
      int callee = conv_bridge_callee(c, i, names[w], TY_UNKNOWN, 1, &tmi);
      if (callee < 0) continue;
      if (rows++ == 0) buf_printf(b, "    case %d: switch (which) {\n", i);
      char call[256];
      snprintf(call, sizeof call, "sp_%s_%s(%s(sp_%s *)p%s)", c->classes[callee].c_name,
               mc(c->scopes[tmi].name),
               comp_ty_value_obj(c, ty_object(callee)) ? "*" : "", c->classes[callee].c_name,
               bridge_blk_arg(c, tmi));
      TyKind rt = (TyKind)c->scopes[tmi].ret;
      buf_printf(b, "      case %d: if (out) *out = ", w);
      /* a Float slot's nil is a NaN payload, which the plain box would carry
         as a Float answer */
      if (rt == TY_FLOAT)
        buf_printf(b, "({ sp_float _f = %s; sp_float_is_nil(_f) ? sp_box_nil() : sp_box_float(_f); })", call);
      else emit_boxed_text(c, rt, call, b);
      buf_puts(b, "; return 1;\n");
    }
    /* row 4: #to_f for a Math argument, which rb_to_float converts only
       for a Numeric (the runtime's sp_num_to_f) */
    if (class_is_user_numeric(c, i)) {
      int tmi = -1;
      int callee = conv_bridge_callee(c, i, "to_f", TY_UNKNOWN, 1, &tmi);
      if (callee >= 0) {
        if (rows++ == 0) buf_printf(b, "    case %d: switch (which) {\n", i);
        char call[256];
        snprintf(call, sizeof call, "sp_%s_%s(%s(sp_%s *)p%s)", c->classes[callee].c_name,
                 mc(c->scopes[tmi].name),
                 comp_ty_value_obj(c, ty_object(callee)) ? "*" : "", c->classes[callee].c_name,
                 bridge_blk_arg(c, tmi));
        TyKind rt = (TyKind)c->scopes[tmi].ret;
        buf_puts(b, "      case 4: if (out) *out = ");
        if (rt == TY_FLOAT)
          buf_printf(b, "({ sp_float _f = %s; sp_float_is_nil(_f) ? sp_box_nil() : sp_box_float(_f); })", call);
        else emit_boxed_text(c, rt, call, b);
        buf_puts(b, "; return 1;\n");
      }
    }
    if (rows) buf_puts(b, "      default: return 0;\n    }\n");
  }
  buf_puts(b, "    default: return 0;\n  }\n}\n");
}

static int class_inspectable(Compiler *c, int i) {
  ClassInfo *ci = &c->classes[i];
  if (is_builtin_reopen(ci->name)) return 0;
  if (ci->is_native_class) return 0;   /* the package owns the struct */
  /* an exception subclass renders as #<Cls: msg>, which the dispatch below
     routes to sp_exc_inspect: without a case here `p e` fell to the default
     and printed #<Object> (#3813) */
  if (comp_ty_value_obj(c, ty_object(i))) return 0;  /* no stable address */
  return 1;
}
static void emit_obj_inspect_dispatch(Compiler *c, Buf *b) {
  /* Forward-declare the user to_s/inspect methods the switches dispatch to --
     the switch bodies are emitted ahead of the method definitions. */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *fci = &c->classes[i];
    if (is_builtin_reopen(fci->name) || fci->is_native_class) continue;
    if (comp_class_is_module(c, fci)) continue;
    /* a value-type class's methods take self by value */
    int is_val = comp_ty_value_obj(c, ty_object(i));
    const char *mnames[2] = { "to_s", "inspect" };
    for (int m = 0; m < 2; m++) {
      int fdef = -1;
      int fmi = comp_method_in_chain(c, i, mnames[m], &fdef);
      if (fmi < 0 || !c->scopes[fmi].reachable || c->scopes[fmi].ret != TY_STRING ||
          c->scopes[fmi].nparams != 0 || fdef != i) continue;
      /* a debug (-g) build gives user methods external linkage (see
         emit_method_signature); this forward decl must match it */
      buf_printf(b, "%sconst char *sp_%s_%s(sp_%s %sself);\n",
                 g_debug ? "" : "static ",
                 c->classes[fdef].c_name, mc(c->scopes[fmi].name), c->classes[fdef].c_name,
                 is_val ? "" : "*");
    }
  }
  /* user #to_s dispatcher: only classes defining one get an arm; NULL means
     "no user to_s" and the caller renders the #<Name:0xADDR> default. */
  buf_puts(b, "static const char *sp_obj_to_s_sw(int cls_id, void *p) {\n  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *tci = &c->classes[i];
    /* a native class binding its own to_s (IO::Buffer): dispatch to the
       declared C symbol so a boxed instance renders like a typed receiver */
    if (tci->is_native_class && tci->c_struct) {
      int nts = comp_native_method_find(c, i, "to_s", 0, 0);
      if (nts >= 0 && sp_streq(c->native_methods[nts].ret, "string"))
        buf_printf(b, "    case %d: return %s((%s *)p);\n",
                   i, c->native_methods[nts].csym, tci->c_struct);
      continue;
    }
    if (is_builtin_reopen(tci->name) || tci->is_native_class) continue;
    /* a module has no instances and its methods are emitted only as the
       includer's (sp_V_to_s, never sp_M_to_s): an arm for it referenced a
       function no TU defines and the program did not link (#4533) */
    if (comp_class_is_module(c, tci)) continue;
    int tdef = -1;
    int tmi = comp_method_in_chain(c, i, "to_s", &tdef);
    int tsok = tmi >= 0 && c->scopes[tmi].reachable && c->scopes[tmi].ret == TY_STRING &&
               c->scopes[tmi].nparams == 0;
    /* A boxed value-type object carries a pointer to its struct and the
       method takes self by value, as the inline poly dispatch calls it.
       Skipping value types sent `puts obj` / "#{obj}" to the #<A:0x...>
       default past the user's #to_s. */
    if (comp_ty_value_obj(c, ty_object(i))) {
      if (tsok && tdef == i)
        buf_printf(b, "    case %d: return sp_%s_%s(*(sp_%s *)p);\n",
                   i, tci->c_name, mc(c->scopes[tmi].name), tci->c_name);
      continue;
    }
    if (tsok) {
      buf_printf(b, "    case %d: return sp_%s_%s((sp_%s *)p);\n",
                 i, c->classes[tdef].c_name, mc(c->scopes[tmi].name), c->classes[tdef].c_name);
      continue;
    }
    /* Struct/Data #to_s IS #inspect in CRuby, and they have a generated one
       (`#<struct S x=1>` / `#<data ...>`). Without this arm a BOXED struct fell
       to the object default and rendered `#<S:0x...>` -- so `b.to_s` and
       "#{b}" disagreed with a directly-typed receiver, which compiles straight
       to the same inspect. The inspect dispatcher beside this one already
       carries the arm for the same reason (#4387). A user #to_s still wins:
       it is taken above. */
    if (tci->is_struct || tci->is_data) {
      buf_printf(b, "    case %d: return sp_%s_inspect((sp_%s *)p);\n", i, tci->c_name, tci->c_name);
      continue;
    }
    continue;
  }
  buf_puts(b, "    default: return NULL;\n  }\n}\n");
  /* user #to_int / #to_str bridges: the runtime's implicit-conversion sites
     (pack and friends) reach a compiled conversion method on a BOXED object
     through these; a class without one falls to the default (not-ok / NULL)
     and the caller raises CRuby's TypeError. Same shape as the #to_s
     dispatcher above, with two extra rules: a module row has no instances
     (and no standalone body -- module methods are copied per includer), so
     module rows are skipped, and a def that RESOLVES to a module calls the
     includer's own copy. */
  emit_conv_bridge(c, b, "to_int", TY_INT,
                   "sp_int", "static sp_int sp_obj_to_int_sw(int cls_id, void *p, int *ok)",
                   1, "*ok = 0; return 0;");
  emit_conv_bridge(c, b, "to_str", TY_STRING,
                   "const char *", "static const char *sp_obj_to_str_sw(int cls_id, void *p)",
                   0, "return NULL;");
  /* #to_path, asked first by the path slots (sp_poly_arg_path) */
  emit_conv_bridge(c, b, "to_path", TY_STRING,
                   "const char *", "static const char *sp_obj_to_path_sw(int cls_id, void *p)",
                   0, "return NULL;");
  if (c->uses_kconv) emit_kconv_bridge(c, b);
  buf_puts(b, "static const char *sp_obj_cls_name_rt(int cls_id) {\n"
              "  sp_Class _c = {cls_id}; return sp_class_to_s(_c);\n}\n");
  buf_puts(b, "static const char *sp_obj_inspect_sw(int cls_id, void *p) {\n");
  buf_puts(b, "  switch (cls_id) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    /* native classes with a bound #inspect, same as the to_s dispatcher */
    if (c->classes[i].is_native_class && c->classes[i].c_struct) {
      int nin = comp_native_method_find(c, i, "inspect", 0, 0);
      if (nin >= 0 && sp_streq(c->native_methods[nin].ret, "string"))
        buf_printf(b, "    case %d: return %s((%s *)p);\n",
                   i, c->native_methods[nin].csym, c->classes[i].c_struct);
      continue;
    }
    /* a value-type class has no stable address for the default render, but
       a user #inspect of its own still answers (self by value, as in the
       #to_s dispatcher) */
    if (comp_ty_value_obj(c, ty_object(i)) && !comp_class_is_module(c, &c->classes[i])) {
      int vdef = -1;
      int vmi = comp_method_in_chain(c, i, "inspect", &vdef);
      if (vmi >= 0 && vdef == i && c->scopes[vmi].reachable &&
          c->scopes[vmi].ret == TY_STRING && c->scopes[vmi].nparams == 0)
        buf_printf(b, "    case %d: return sp_%s_%s(*(sp_%s *)p);\n",
                   i, c->classes[i].c_name, mc(c->scopes[vmi].name), c->classes[i].c_name);
      continue;
    }
    if (!class_inspectable(c, i)) continue;
    ClassInfo *ci = &c->classes[i];
    /* a user #inspect wins over the default ivar walk, so a contained
       element renders the same as a directly-inspected one. A module's own
       arm would name a function only its includers define (#4533). */
    int uidef = -1;
    int uimi = comp_class_is_module(c, ci) ? -1 : comp_method_in_chain(c, i, "inspect", &uidef);
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
    if (class_is_exc_subclass(c, i)) {
      buf_printf(b, "    case %d: return sp_exc_inspect(p);\n", i);
      continue;
    }
    buf_printf(b, "    case %d: {\n", i);
    buf_printf(b, "      sp_%s *o = (sp_%s *)p; (void)o;\n", ci->c_name, ci->c_name);
    /* An ivar can point back at the object (a tree node's @parent), and the
       walk below renders each ivar through the inspects that come back here.
       CRuby shows the repeated object as #<N:0x... ...>; an object with no
       ivars cannot be reached from inside itself and keeps its old body. */
    if (ci->nivars > 0)
      buf_printf(b, "      if (sp_poly_recur_seen(SP_POLY_RECUR_INSPECT, p, NULL))\n"
                    "        return sp_sprintf(\"#<%s:0x%%016llx ...>\", (unsigned long long)(uintptr_t)p);\n"
                    "      int _rcm = sp_poly_recur_push(SP_POLY_RECUR_INSPECT, p, NULL);\n",
                 class_ruby_name(c, i) ? class_ruby_name(c, i) : ci->name);
    buf_printf(b, "      sp_String *_s = sp_String_new(sp_sprintf(\"#<%s:0x%%016llx\", (unsigned long long)(uintptr_t)p));\n",
               class_ruby_name(c, i) ? class_ruby_name(c, i) : ci->name);
    /* The builder is live across every allocation the ivar walk below makes --
       each element's own inspect, and the sp_sprintf that renders an ivar
       pointing back at this object. Unrooted, a collection mid-walk swept it
       and the appends wrote into a freed buffer (the Struct/Data inspect above
       has always rooted its builder for the same reason). A class with no
       ivars has no such walk: it appends one byte, which reallocs off the GC
       heap and cannot collect, so its arm is left as it was. */
    if (ci->nivars > 0) buf_puts(b, "      SP_GC_ROOT(_s);\n");
    for (int j = 0; j < ci->nivars; j++) {
      char expr[160]; snprintf(expr, sizeof expr, "o->iv_%s", ci->ivars[j] + 1);
      buf_printf(b, "      sp_String_append(_s, \"%s%s=\"); sp_String_append(_s, ",
                 j ? ", " : " ", ci->ivars[j]);
      TyKind ivt = ci->ivar_types[j];
      /* containers have their own typed inspect; scalars box through the
         marshal helper into sp_poly_inspect; an UNKNOWN (never usefully
         typed) ivar occupies an int slot in the layout and renders as its
         nil default. */
      if (ty_is_obj_array(ivt)) {
        int eci = ty_obj_array_class(ivt);
        int edyn = class_has_subclass(c, eci) && !class_is_exc_subclass(c, eci);
        buf_printf(b, "(%s ? sp_ObjPtrArray_inspect(%s, %d) : \"nil\")", expr, expr, edyn ? -1 : eci);
      }
      else if (ivt == TY_INT_ARRAY_ARRAY)
        buf_printf(b, "(%s ? sp_IntArrayPtrArray_inspect(%s) : \"nil\")", expr, expr);
      else if (ivt == TY_FLOAT_ARRAY_ARRAY)
        buf_printf(b, "(%s ? sp_FloatArrayPtrArray_inspect(%s) : \"nil\")", expr, expr);
      else if (ty_is_array(ivt) && ivt != TY_POLY_ARRAY && array_kind(ivt))
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
    if (ci->nivars > 0) buf_puts(b, "      sp_poly_recur_pop(_rcm);\n");
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
      buf_puts(b, "    for (sp_int k = 0; k + 1 < iv->len; k += 2) {\n");
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

/* A parent parameter a bare `super` has no argument for: `*rest` takes an
   empty Array and `**kw` an empty Hash, as CRuby binds them; any other slot
   takes its default. emit_arg_or_default knows no rest kinds, so both came out
   as a NULL the callee read back as nil (#4852). Each empty is rooted in the
   prelude: the next one allocates before the callee roots its parameters. */
static void emit_zsuper_param_fill(Compiler *c, Scope *pm, int i, Buf *b) {
  int is_rest = i == pm->rest_idx, is_kwrest = i == pm->kwrest_idx;
  if (!is_rest && !is_kwrest) { emit_arg_or_default(c, pm, i, -1, b); return; }
  LocalVar *p = pm->pnames[i] ? scope_local(pm, pm->pnames[i]) : NULL;
  int poly = !p || p->type == TY_POLY || p->type == TY_UNKNOWN;
  const char *cty = is_rest ? "sp_PolyArray" : "sp_SymPolyHash";
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "%s *_t%d = %s_new();\n", cty, t, cty);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  char tn[24]; snprintf(tn, sizeof tn, "_t%d", t);
  if (poly) emit_boxed_text(c, is_rest ? TY_POLY_ARRAY : TY_SYM_POLY_HASH, tn, b);
  else buf_puts(b, tn);
}

/* How many leading positional parameters a bare `super`'s method forwards
   into the parent's `*rest` when it has more of them than the parent takes
   before it, or -1 when the plain slot-by-slot forward applies. CRuby hands
   zsuper the method's positionals as an argument list, so `def m(x, y) =
   super` into `def m(x, *rest)` binds rest to [y]; forwarding by slot passed
   the bare y where the parent's C function takes an Array (#4852). A parent
   with required parameters after its rest keeps the slot-by-slot forward. */
static int zsuper_rest_surplus(Compiler *c, Scope *s, Scope *pm) {
  if (pm->rest_idx < 0 || pm->npost_rest > 0) return -1;
  int npos = 0;
  while (npos < s->nparams && npos != s->rest_idx && npos != s->kwrest_idx &&
         !callee_param_is_declared_kwarg(c, s, s->pnames[npos])) npos++;
  return npos > pm->rest_idx ? npos : -1;
}

/* The parent's `*rest` for zsuper_rest_surplus: the method's positionals
   from the rest's index on, boxed into one Array rooted in the prelude. */
static void emit_zsuper_rest_pack(Compiler *c, Scope *s, Scope *pm, int npos, Buf *b) {
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", t);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  for (int i = pm->rest_idx; i < npos; i++) {
    LocalVar *ep = scope_local(s, s->pnames[i]);
    TyKind et = ep && ep->type != TY_UNKNOWN ? ep->type : TY_POLY;
    char txt[128]; snprintf(txt, sizeof txt, "lv_%s", rename_local(s->pnames[i]));
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t);
    if (et == TY_POLY) buf_puts(g_pre, txt);
    else emit_boxed_text(c, et, txt, g_pre);
    buf_puts(g_pre, ");\n");
  }
  LocalVar *rp = scope_local(pm, pm->pnames[pm->rest_idx]);
  char tn[24]; snprintf(tn, sizeof tn, "_t%d", t);
  if (!rp || rp->type == TY_POLY || rp->type == TY_UNKNOWN) emit_boxed_text(c, TY_POLY_ARRAY, tn, b);
  else buf_puts(b, tn);
}

/* The trailing `&blk` slot of a `super` call: the parent's C function takes
   one whenever it keeps a named block parameter, and the call left it out
   (#4852). The call's own block goes there -- a literal as a proc, a `&proc`
   as itself -- and without one the caller's block is forwarded, as CRuby's
   super does with or without arguments; a caller with no block passes NULL. */
static void emit_super_block_arg(Compiler *c, int id, Scope *s, Scope *pm, int lead_comma, Buf *b) {
  if (!pm->blk_param || !pm->blk_param[0] || pm->yields) return;
  if (lead_comma) buf_puts(b, ", ");
  /* In an inlined body a forwarded `&` / `&blk` names the block spliced in
     from the caller: materialize that literal (the callee's own `lv_blk` is
     never declared there). */
  int blk0 = nt_ref(c->nt, id, "block");
  int blk = resolve_forwarded_block(c, blk0);
  if (blk < 0) blk = blk0;
  const char *bty = blk >= 0 ? nt_type(c->nt, blk) : NULL;
  if (bty && sp_streq(bty, "BlockNode")) { emit_proc_literal(c, blk, b); return; }
  if (bty && sp_streq(bty, "BlockArgumentNode")) {
    int fe = nt_ref(c->nt, blk, "expression");
    if (fe >= 0) {
      if (comp_ntype(c, fe) == TY_PROC) emit_expr(c, fe, b);
      else if (sp_streq(nt_type(c->nt, fe), "NilNode")) buf_puts(b, "NULL");
      else unsupported(c, blk, "super with a block argument that is not a proc");
      return;
    }
  }
  if (s->blk_param && s->blk_param[0] && !s->yields)
    buf_printf(b, "lv_%s", rename_local(s->blk_param));
  /* the caller's block, implicitly forwarded from an inlined body */
  else if (s->yields && g_block_id >= 0) emit_proc_literal(c, g_block_id, b);
  else buf_puts(b, "NULL");
}

/* Inline super { block } when the parent method uses yield.
   Returns 1 if the expansion was emitted, 0 if it should fall through to a
   regular function call (parent doesn't yield, has early return, etc.). */
int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) return 0;
  int p = c->classes[s->class_id].parent;
  int defcls = -1;
  /* `super` inside a class method resolves through the parent's CLASS-method
     chain; the instance chain would miss `def self.x` entirely. */
  int mi = p < 0 ? -1
         : s->is_cmethod ? comp_cmethod_in_chain(c, p, s->name, &defcls)
                         : comp_method_in_chain(c, p, s->name, &defcls);
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  int block = nt_ref(c->nt, id, "block");
  /* A bare `super` forwards the caller's block, which is the one currently
     being spliced into this (inlined) method. */
  if (block < 0) block = g_block_id;
  /* No block to splice: the parent is still only ever inlined (a yielding
     method has no C function of its own), and the call to an undefined
     sp_<Cls>_<m> did not link (#4852). A caller holding its block as a proc
     -- a declared `&blk`, or the one a super into a block-taking parent
     synthesizes -- drives the parent's yields through that proc, as an
     inlined `inner(&blk)` does; with neither, `block_given?` folds false. */
  char yprocbuf[128];
  const char *fwd_yield_proc = NULL;
  if (block < 0 && s->blk_param && s->blk_param[0] && !s->yields) {
    snprintf(yprocbuf, sizeof yprocbuf, "lv_%s", rename_local(s->blk_param));
    fwd_yield_proc = yprocbuf;
  }
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }

  int tag = ++g_tmp;
  int saved_nren = g_nren, saved_block = g_block_id;
  int saved_bnren = g_block_nren, saved_yfbn = g_yield_block_fallback_nren;
  const char *saved_bpn = g_block_param_name;
  int saved_yfb = g_yield_block_fallback;
  const char *saved_bbv = g_block_brk_var, *saved_yfbv = g_yield_blk_brk_fallback;
  const char *saved_ser = g_brk_ser_var;
  int saved_bbe = g_block_brk_ebase, saved_yfbe = g_yield_blk_brk_efallback;
  int saved_bbexc = g_block_brk_exc_base, saved_bexc = g_brk_exc_base;
  int saved_ebase = g_brk_ensure_base;

  g_yield_block_fallback = saved_block;
  g_yield_block_fallback_nren = saved_bnren;
  g_yield_blk_brk_fallback = saved_bbv;
  g_yield_blk_brk_efallback = saved_bbe;
  g_block_id = block;
  g_block_nren = (block == saved_block) ? saved_bnren : saved_nren;
  /* same break-context rules as emit_inline_call_x */
  g_block_brk_var = (block == saved_block) ? saved_bbv : saved_ser;
  g_block_brk_ebase = (block == saved_block) ? saved_bbe : saved_ebase;
  g_block_brk_exc_base = (block == saved_block) ? saved_bbexc : saved_bexc;
  g_brk_ser_var = NULL;
  g_block_param_name = m->blk_param;
  const char *saved_ypr = g_yield_proc_ref;
  TyKind saved_yslot = g_yield_slot_ty;
  if (fwd_yield_proc) {
    g_yield_proc_ref = fwd_yield_proc;
    g_yield_slot_ty = as_expr ? comp_ntype(c, id) : TY_UNKNOWN;
  }

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
    emit_inlined_local_decl(c, lv, rn, b, din);
  }

  const char *ty = nt_type(c->nt, id);
  int is_forwarding = ty && sp_streq(ty, "ForwardingSuperNode");
  int args = nt_ref(c->nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  int surplus = is_forwarding ? zsuper_rest_surplus(c, s, m) : -1;
  for (int i = 0; i < m->nparams; i++) {
    emit_indent(b, din);
    { char rn[128]; snprintf(rn, sizeof rn, "_y%d_%s", tag, m->pnames[i]);
      emit_inlined_param_target(c, m, m->pnames[i], rn, b); }
    int sv = g_nren; g_nren = saved_nren;
    if (is_forwarding) {
      if (surplus >= 0 && i == m->rest_idx) emit_zsuper_rest_pack(c, s, m, surplus, b);
      else if (i < s->nparams && (surplus < 0 || i < m->rest_idx)) {
        /* the forwarded local carries the CHILD's type; box it when the
           parent's slot is boxed, as the ordinary inline binder does */
        LocalVar *ep = scope_local(s, s->pnames[i]);
        LocalVar *mp = scope_local(m, m->pnames[i]);
        TyKind et = ep ? ep->type : TY_POLY;
        TyKind mt = mp ? mp->type : TY_POLY;
        char txt[128]; snprintf(txt, sizeof txt, "lv_%s", rename_local(s->pnames[i]));
        if (mt == TY_POLY && et != TY_POLY) emit_boxed_text(c, et, txt, b);
        else buf_puts(b, txt);
      }
      else { g_nren = sv; emit_zsuper_param_fill(c, m, i, b); sv = g_nren; }
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
  g_block_nren = saved_bnren;
  g_yield_block_fallback_nren = saved_yfbn;
  g_block_param_name = saved_bpn;
  g_yield_block_fallback = saved_yfb;
  g_block_brk_var = saved_bbv; g_yield_blk_brk_fallback = saved_yfbv;
  g_block_brk_ebase = saved_bbe; g_yield_blk_brk_efallback = saved_yfbe;
  g_block_brk_exc_base = saved_bbexc; g_brk_exc_base = saved_bexc;
  g_brk_ser_var = saved_ser; g_brk_ensure_base = saved_ebase;
  g_yield_proc_ref = saved_ypr; g_yield_slot_ty = saved_yslot;
  return 1;
}

/* super(args) / super -> call the parent's same-named method. */
/* One parameter a bare `super` forwards, converted to the parent's slot:
   boxed when the parent's is poly, and unboxed when this method's is poly and
   the parent's is typed -- a proc-form clone's parameters are boxed, and its
   super reaching the parent's plain method passed an sp_RbVal into an sp_int
   parameter, which the C compiler refused. */
static void emit_zsuper_arg(Compiler *c, TyKind st, TyKind dt, const char *pname, Buf *b) {
  Buf _bx; memset(&_bx, 0, sizeof _bx);
  buf_printf(&_bx, "lv_%s", rename_local(pname));
  if (dt == TY_POLY && st != TY_POLY && st != TY_UNKNOWN) emit_boxed_text(c, st, _bx.p, b);
  else if (st == TY_POLY && dt == TY_INT) buf_printf(b, "sp_poly_to_i_or_nil(%s)", _bx.p);
  else if (st == TY_POLY && dt == TY_FLOAT) buf_printf(b, "sp_poly_to_f_or_nil(%s)", _bx.p);
  else if (st == TY_POLY && dt != TY_POLY && dt != TY_UNKNOWN) emit_unbox_text(c, dt, _bx.p, b);
  else buf_puts(b, _bx.p);
  free(_bx.p);
}

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
  int p = c->classes[s->class_id].parent;
  const char *uname = comp_super_name(c, p, s->name, s->is_cmethod);
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
                 uname, scn2, raise_tail_value_c(c, comp_ntype(c, id)));
      return;
    }
    buf_printf(b, "sp_%s_s_%s(", c->classes[cdef].c_name, mc(uname));
    /* `super` in a class method keeps the receiving class: forward ours. */
    if (cmethod_takes_self_cls(c, cmi))
      buf_printf(b, "%s%s", cmethod_takes_self_cls(c, (int)(s - c->scopes)) ? "_sp_cls" : "((sp_Class){-1, NULL})",
                 c->scopes[cmi].nparams > 0 ? ", " : "");
    if (ty && sp_streq(ty, "ForwardingSuperNode")) {
      Scope *pm = &c->scopes[cmi];
      int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
      int surplus = zsuper_rest_surplus(c, s, pm);
      if (surplus >= 0) n = pm->rest_idx;
      for (int i = 0; i < n; i++) {
        LocalVar *src = scope_local(s, s->pnames[i]);
        LocalVar *dst = scope_local(pm, pm->pnames[i]);
        TyKind st = src ? src->type : TY_UNKNOWN;
        TyKind dt = dst ? dst->type : TY_UNKNOWN;
        buf_puts(b, i == 0 ? "" : ", ");
        emit_zsuper_arg(c, st, dt, s->pnames[i], b);
      }
      /* the parent's extra parameters take their defaults (#4852) */
      for (int i = n; i < pm->nparams; i++) {
        buf_puts(b, i == 0 ? "" : ", ");
        if (surplus >= 0 && i == pm->rest_idx) emit_zsuper_rest_pack(c, s, pm, surplus, b);
        else emit_zsuper_param_fill(c, pm, i, b);
      }
    }
    else emit_args_filled(c, cmi, nt_ref(c->nt, id, "arguments"), "", b);
    emit_super_block_arg(c, id, s, &c->scopes[cmi],
                         c->scopes[cmi].nparams > 0 || cmethod_takes_self_cls(c, cmi), b);
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
        /* nilable: Exception#initialize STRINGIFIES its message (super(nil)
           keeps the class-name default in CRuby), it never type-checks it */
        emit_str_expr_nilable(c, argv2[0], b);
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
    /* `super()` from an initialize no ancestor defines reaches Object's, which
       takes no arguments and does nothing -- not a NoMethodError. The bare
       form forwards this method's params, so it is only that no-op when there
       are none. */
    if (sp_streq(uname, "initialize")) {
      int fwd_super = ty && sp_streq(ty, "ForwardingSuperNode");
      int sup_args = ty && sp_streq(ty, "SuperNode") ? nt_ref(c->nt, id, "arguments") : -1;
      int sup_argc = 0;
      if (sup_args >= 0) nt_arr(c->nt, sup_args, "arguments", &sup_argc);
      if ((fwd_super && s->nparams == 0) || (!fwd_super && sup_argc == 0)) {
        buf_puts(b, default_value(comp_ntype(c, id)));
        return;
      }
    }
    /* No superclass method anywhere (parent chain, included-module shadow, and
       the exception-initialize special case all missed). CRuby raises
       NoMethodError at runtime, so emit that rather than rejecting at compile
       time -- the call may sit in a branch that never runs. */
    const char *scn = class_ruby_name(c, s->class_id);
    if (!scn) scn = c->classes[s->class_id].name;
    /* The raise never returns, so the trailing value only has to type-check
       in the slot. An UNRESOLVED super -- a module never included, so nothing
       says what it answers -- has no concrete type, and default_value's "0"
       did not fit the sp_RbVal an interpolation reads (#4034). */
    buf_printf(b, "(sp_raise_cls(\"NoMethodError\", \"super: no superclass method '%s' for an instance of %s\"), %s)",
               uname, scn, raise_tail_value_c(c, comp_ntype(c, id)));
    return;
  }
  buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].c_name, mc(uname), c->classes[defcls].c_name, g_self);
  if (ty && sp_streq(ty, "ForwardingSuperNode")) {
    Scope *pm = &c->scopes[mi];
    int n = s->nparams < pm->nparams ? s->nparams : pm->nparams;
    int surplus = zsuper_rest_surplus(c, s, pm);
    if (surplus >= 0) n = pm->rest_idx;
    for (int i = 0; i < n; i++) {
      LocalVar *src = scope_local(s, s->pnames[i]);
      LocalVar *dst = scope_local(pm, pm->pnames[i]);
      TyKind st = src ? src->type : TY_UNKNOWN;
      TyKind dt = dst ? dst->type : TY_UNKNOWN;
      /* Use the local's emitted C name: when this method body is inlined at a
         block call site the params are renamed (e.g. `x` -> `_y5_x`), so bare
         `super`'s implicit forwarding must reference the renamed identifier. */
      buf_puts(b, ", ");
      emit_zsuper_arg(c, st, dt, s->pnames[i], b);
    }
    /* The parent may declare more than this method does -- an optional,
       `*rest`, a keyword, `**` -- which a bare super leaves to their defaults
       and empties, as CRuby does. The call stopped at this method's own
       count and the C had too few arguments (#4852). */
    for (int i = n; i < pm->nparams; i++) {
      buf_puts(b, ", ");
      if (surplus >= 0 && i == pm->rest_idx) emit_zsuper_rest_pack(c, s, pm, surplus, b);
      else emit_zsuper_param_fill(c, pm, i, b);
    }
  }
  else {
    emit_args_filled(c, mi, nt_ref(c->nt, id, "arguments"), ", ", b);
  }
  emit_super_block_arg(c, id, s, &c->scopes[mi], 1, b);
  buf_puts(b, ")");
}

/* Generate sp_obj_cmp_dispatch: a cls_id switch calling each instantiated
   Comparable class's user `<=>`, reporting a nil result as not-comparable.
   Installed as sp_obj_cmp_hook so the runtime comparator (no-block
   sort/min/max/clamp) can order user objects. Only object- and poly-typed
   operands are handled; an exotic operand type omits its arm and falls through
   to not-comparable, which the callers raise as an ArgumentError. */
static void emit_obj_cmp_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static sp_int sp_obj_cmp_dispatch(sp_RbVal a, sp_RbVal b, sp_bool *comparable) {\n");
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
    const char *builtin_guard = NULL; /* operand cls_id a boxed-builtin param requires */
    if (ty_is_object(pt)) {
      int pcls = ty_object_class(pt);
      const char *pcn = c->classes[pcls].name;
      pcid = comp_class_index(c, pcn);   /* guard b against the OPERAND's class, not the receiver's */
      snprintf(argbuf, sizeof argbuf, "%s(sp_%s *)b.v.p",
               c->classes[pcls].is_value_type ? "*" : "", pcn);
      obj_operand = 1;
    }
    else if (pt == TY_POLY) {
      snprintf(argbuf, sizeof argbuf, "b");
    }
    else if (pt == TY_INT) {
      /* a scalar-typed `<=>` param (bound from scalar call sites) still gets
         an arm: guard the operand's tag, unbox, and let the body's own nil
         return (the is_a? mismatch branch) drive the not-comparable path */
      scalar_guard = "SP_TAG_INT";
      snprintf(argbuf, sizeof argbuf, "b.v.i");
    }
    else if (pt == TY_FLOAT) {
      scalar_guard = "SP_TAG_FLT";
      snprintf(argbuf, sizeof argbuf, "b.v.f");
    }
    /* A `<=>` whose operand param settled on a boxed BUILTIN -- a Rational
       operand types it that way -- had no arm at all, so the dispatch table
       came out empty and every comparison through Comparable reported the
       pair as incomparable, even though the class compares them (#4038). */
    else if (pt == TY_RATIONAL) {
      builtin_guard = "SP_BUILTIN_RATIONAL";
      snprintf(argbuf, sizeof argbuf, "*(sp_Rational *)b.v.p");
    }
    else if (pt == TY_COMPLEX) {
      builtin_guard = "SP_BUILTIN_COMPLEX";
      snprintf(argbuf, sizeof argbuf, "*(sp_Complex *)b.v.p");
    }
    else if (pt == TY_TIME) {
      builtin_guard = "SP_BUILTIN_TIME";
      snprintf(argbuf, sizeof argbuf, "*(sp_Time *)b.v.p");
    }
    else {
      continue;
    }
    buf_printf(b, "    case %d: {\n", cid);
    if (obj_operand) {
      /* The operand param was inferred to a single class (`pcid`), but the
         `<=>` body (defined up the chain, e.g. a Comparable mixin) works for
         any object in that hierarchy. Accept `b` when it is `pcid` OR any
         instantiated subclass of it: a subclass shares pcid's layout, so the
         `(sp_pcn *)b` cast below stays valid, and comparing two different
         subclasses (Rectangle vs Square) no longer fails closed (#3188). */
      buf_printf(b, "      if (b.tag != SP_TAG_OBJ || !(b.cls_id == %d", pcid);
      for (int d = 0; d < c->nclasses; d++) {
        if (d == pcid || !c->classes[d].instantiated) continue;
        if (!is_descendant(c, d, pcid)) continue;
        int dcid = comp_class_index(c, c->classes[d].name);
        buf_printf(b, " || b.cls_id == %d", dcid);
      }
      buf_puts(b, ")) { *comparable = FALSE; return 0; }\n");
    }
    if (scalar_guard)
      buf_printf(b, "      if (b.tag != %s) { *comparable = FALSE; return 0; }\n", scalar_guard);
    if (builtin_guard)
      buf_printf(b, "      if (!(b.tag == SP_TAG_OBJ && b.cls_id == %s)) { *comparable = FALSE; return 0; }\n", builtin_guard);
    if (m->ret == TY_INT) {
      /* a `<=>` that also answers nil is a nullable Integer (the nil join):
         its sentinel is the not-comparable answer */
      buf_printf(b, "      sp_int _ri = (sp_int)sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
      if (m->ret_nullable_int) buf_puts(b, "      if (_ri == SP_INT_NIL) { *comparable = FALSE; return 0; }\n");
      buf_puts(b, "      *comparable = TRUE; return _ri;\n");
    }
    else if (m->ret == TY_FLOAT) {
      /* a Float `<=>` result is a valid comparison (CRuby): use its sign */
      buf_printf(b, "      sp_float _rf = sp_%s_%s(%s(sp_%s *)a.v.p, %s);\n",
                 dcn, mc("<=>"), self_vt ? "*" : "", dcn, argbuf);
      if (m->ret_nullable_int) buf_puts(b, "      if (sp_float_is_nil(_rf)) { *comparable = FALSE; return 0; }\n");
      buf_puts(b, "      *comparable = TRUE; return (_rf > 0) - (_rf < 0);\n");
    }
    else {
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

/* A boxed argument `v` handed to parameter `pi` of method scope `m`, as the
   user dispatch tables below pass it: `guard` (empty when any value will do)
   tests that the value fits the parameter, and `arg` is the value in the
   parameter's C type. 0 when the parameter's type has no such form. */
static int user_dispatch_arg(Compiler *c, Scope *m, int pi, const char *v,
                             char *guard, size_t gsz, char *arg, size_t asz) {
  LocalVar *p = scope_local(m, m->pnames[pi]);
  TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
  guard[0] = 0;
  if (ty_is_object(pt)) {
    int pcls = ty_object_class(pt);
    snprintf(guard, gsz, "%s.tag == SP_TAG_OBJ && %s.cls_id == %d",
             v, v, comp_class_index(c, c->classes[pcls].name));
    snprintf(arg, asz, "%s(sp_%s *)%s.v.p",
             c->classes[pcls].is_value_type ? "*" : "", c->classes[pcls].name, v);
  }
  else if (pt == TY_POLY) snprintf(arg, asz, "%s", v);
  else if (pt == TY_INT) { snprintf(guard, gsz, "%s.tag == SP_TAG_INT", v); snprintf(arg, asz, "%s.v.i", v); }
  else if (pt == TY_FLOAT) { snprintf(guard, gsz, "%s.tag == SP_TAG_FLT", v); snprintf(arg, asz, "%s.v.f", v); }
  else if (pt == TY_STRING) { snprintf(guard, gsz, "%s.tag == SP_TAG_STR", v); snprintf(arg, asz, "%s.v.s", v); }
  else if (pt == TY_SYMBOL) { snprintf(guard, gsz, "%s.tag == SP_TAG_SYM", v); snprintf(arg, asz, "(sp_sym)%s.v.i", v); }
  else return 0;
  return 1;
}

/* Whether class k answers a user `[]=` the aset table below can call. */
static int user_aset_scope(Compiler *c, int k, int *defcls) {
  int mi = comp_method_in_chain(c, k, "[]=", defcls);
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  if (!m->reachable || m->yields || scope_is_shadowed(c, mi) ||
      m->is_transplanted_source || m->nparams != 2 || m->rest_idx >= 0) return -1;
  return mi;
}

/* Generate sp_user_aset_dispatch: the `[]=` counterpart of the binop table
   below, installed as sp_user_aset_hook. `r[k] ||= v` and `r[k] += v` on a
   boxed r store through sp_poly_set_poly, which knew only the builtin
   containers, so an object with its own []= lost the store. */
static void emit_user_aset_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static void sp_user_aset_dispatch(sp_RbVal a, sp_RbVal k, sp_RbVal v, sp_bool *handled) {\n");
  buf_puts(b, "  *handled = FALSE;\n  switch (a.cls_id) {\n");
  for (int ci = 0; ci < c->nclasses; ci++) {
    if (!c->classes[ci].instantiated) continue;
    int defcls = -1;
    int mi = user_aset_scope(c, ci, &defcls);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    char g0[96], a0[160], g1[96], a1[160];
    if (!user_dispatch_arg(c, m, 0, "k", g0, sizeof g0, a0, sizeof a0) ||
        !user_dispatch_arg(c, m, 1, "v", g1, sizeof g1, a1, sizeof a1)) continue;
    const char *dcn = c->classes[defcls].c_name;
    buf_printf(b, "    case %d:\n", comp_class_index(c, c->classes[ci].name));
    buf_printf(b, "      if ((%s) && (%s)) {\n", g0[0] ? g0 : "1", g1[0] ? g1 : "1");
    buf_printf(b, "        *handled = TRUE; (void)sp_%s_%s(%s(sp_%s *)a.v.p, %s, %s);\n      }\n",
               dcn, mc(m->name ? m->name : "[]="), c->classes[defcls].is_value_type ? "*" : "", dcn, a0, a1);
    buf_puts(b, "      break;\n");
  }
  buf_puts(b, "    default: break;\n  }\n}\n");
}

/* Generate sp_user_binop_dispatch: a cls_id switch resolving user-defined
   binary operators on a BOXED receiver. Installed as sp_user_binop_hook, the
   last stop before sp_poly_binop_bad raises -- so `acc + x` inside a fold
   whose accumulator widened to poly still reaches Money#+ (#2886). Each arm
   unboxes the operand per the method's bound parameter type; a mismatched
   operand leaves handled FALSE and the caller's TypeError stands. */
static void emit_user_binop_dispatch(Compiler *c, Buf *b) {
  static const char *const uops[] = {
    "+", "-", "*", "/", "%", "**", "<<", ">>", "&", "|", "^",
    /* the comparisons too: a boxed receiver reached sp_poly_cmp, which knows
       nothing of a user `<`, and answered ArgumentError (#3501) */
    "<", ">", "<=", ">=", "<=>", "==",
    /* and the element read, which a boxed `r[k] ||= v` / `r[k] += v` reads
       through sp_poly_index_poly */
    "[]", NULL };
  buf_puts(b, "static sp_RbVal sp_user_binop_dispatch(const char *op, sp_RbVal a, sp_RbVal b, sp_bool *handled) {\n");
  buf_puts(b, "  *handled = FALSE;\n  switch (a.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    int any = 0;
    for (int u = 0; uops[u] && !any; u++)
      if (comp_method_in_chain(c, k, uops[u], NULL) >= 0) any = 1;
    if (!any) continue;
    int cid = comp_class_index(c, c->classes[k].name);
    buf_printf(b, "    case %d: {\n", cid);
    for (int u = 0; uops[u]; u++) {
      int defcls = -1;
      int mi = comp_method_in_chain(c, k, uops[u], &defcls);
      if (mi < 0) continue;
      Scope *m = &c->scopes[mi];
      /* only methods this TU actually emits: an unreachable / yielding /
         shadowed scope has no C function to call */
      if (!m->reachable || m->yields || scope_is_shadowed(c, mi) ||
          m->is_transplanted_source) continue;
      if (m->nparams < 1 || m->rest_idx >= 0) continue;
      if (sp_streq(uops[u], "[]") && m->nparams != 1) continue;
      const char *dcn = c->classes[defcls].c_name;
      int self_vt = c->classes[defcls].is_value_type;
      char argbuf[160], gb[96];
      if (!user_dispatch_arg(c, m, 0, "b", gb, sizeof gb, argbuf, sizeof argbuf)) continue;
      const char *guard = gb[0] ? gb : NULL;
      buf_printf(b, "      if (strcmp(op, \"%s\") == 0%s%s%s) {\n",
                 uops[u], guard ? " && (" : "", guard ? guard : "", guard ? ")" : "");
      char callbuf[256];
      /* an alias (`alias + |`) resolves to its target's scope: name the C
         function after the RESOLVED method, not the queried operator */
      snprintf(callbuf, sizeof callbuf, "sp_%s_%s(%s(sp_%s *)a.v.p, %s)",
               dcn, mc(m->name ? m->name : uops[u]), self_vt ? "*" : "", dcn, argbuf);
      buf_puts(b, "        *handled = TRUE; return ");
      emit_boxed_text(c, m->ret, callbuf, b);
      buf_puts(b, ";\n      }\n");
    }
    /* Comparable's `==` is derived from `<=>`: a class that defines the
       compare but not the equality still answers `a == b` as `(a <=> b) == 0`.
       Without an arm the boxed path fell through to identity and said false
       for two equal values (#3501). */
    if (comp_method_in_chain(c, k, "==", NULL) < 0) {
      int cmp_defcls = -1;
      int cmp_mi = comp_method_in_chain(c, k, "<=>", &cmp_defcls);
      if (cmp_mi >= 0) {
        Scope *cm2 = &c->scopes[cmp_mi];
        if (cm2->reachable && !cm2->yields && !scope_is_shadowed(c, cmp_mi) &&
            !cm2->is_transplanted_source && cm2->nparams == 1 && cm2->rest_idx < 0) {
          LocalVar *cp2 = scope_local(cm2, cm2->pnames[0]);
          TyKind cpt = (cp2 && cp2->type != TY_UNKNOWN) ? cp2->type : TY_POLY;
          const char *ccn = c->classes[cmp_defcls].c_name;
          int cvt = c->classes[cmp_defcls].is_value_type;
          /* the arm compares the spaceship's answer with 0, so that answer has
             to BE a number -- an int, or a float as emit_obj_cmp_dispatch
             already allows. A `<=>` that can return nil is typed poly (or has
             no value at all), and `sp_X_cmp(...) == 0` on it is ill-typed C
             -- reachable as soon as the class also defines #coerce. */
          int cmp_ret_ok = (cm2->ret == TY_INT || cm2->ret == TY_FLOAT);
          if (cmp_ret_ok && (ty_is_object(cpt) || cpt == TY_POLY)) {
            char cargs[160];
            const char *cguard = NULL;
            static char cgb[64];
            if (ty_is_object(cpt)) {
              int pcls2 = ty_object_class(cpt);
              snprintf(cgb, sizeof cgb, "b.tag == SP_TAG_OBJ && b.cls_id == %d",
                       comp_class_index(c, c->classes[pcls2].name));
              cguard = cgb;
              snprintf(cargs, sizeof cargs, "%s(sp_%s *)b.v.p",
                       c->classes[pcls2].is_value_type ? "*" : "", c->classes[pcls2].name);
            }
            else snprintf(cargs, sizeof cargs, "b");
            buf_printf(b, "      if (strcmp(op, \"==\") == 0%s%s%s) {\n",
                       cguard ? " && (" : "", cguard ? cguard : "", cguard ? ")" : "");
            buf_printf(b, "        *handled = TRUE; return sp_box_bool(sp_%s_%s(%s(sp_%s *)a.v.p, %s) == 0);\n      }\n",
                       ccn, mc(cm2->name ? cm2->name : "<=>"), cvt ? "*" : "", ccn, cargs);
          }
        }
      }
    }
    buf_puts(b, "      break;\n    }\n");
  }
  buf_puts(b, "    default: break;\n  }\n  return sp_box_nil();\n}\n");
}

/* Generate sp_user_to_io_dispatch: the cls_id switch behind IO.select's
   #to_io protocol. CRuby waits on anything that answers #to_io, which is how a
   wrapper holding a socket -- a TLS socket, a protocol object -- gets waited
   on; the runtime cannot dispatch a user method itself, so it calls this
   through sp_user_to_io_hook. A class whose #to_io does not answer an IO is
   left out rather than emitted wrong: it falls through to the TypeError the
   element would have raised anyway.

   The return-type gate accepts TY_IO, TY_UNKNOWN, and TY_POLY: a
   wrapper written as `def to_io; @sock; end` lets the compiler infer
   the return type from `@sock`, which is poly (TY_POLY) because the
   constructor takes a raw socket the user hands in. The runtime hook
   checks the result is actually an IO before using it, so a wrong
   return is still a TypeError -- just at the moment of select, not
   at the moment of class discovery.

   A poly return means the emitted body boxes the value into sp_RbVal
   first, then asks the runtime to unwrap an sp_File from it; the
   IO path's existing box machinery is what handles "answer is an IO"
   already (sp_poly_to_file / tag SP_TAG_FILE). The TY_IO arm stays
   the direct cast: that function still returns sp_File * directly. */
static void emit_user_to_io_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static sp_File *sp_user_to_io_dispatch(sp_RbVal v) {\n");
  buf_puts(b, "  switch (v.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "to_io", &defcls);
    if (mi < 0 || defcls < 0) continue;
    Scope *m = &c->scopes[mi];
    if (!m->reachable || m->yields || m->nparams != 0) continue;
    if (m->ret != TY_IO && m->ret != TY_UNKNOWN && m->ret != TY_POLY) continue;
    if (scope_is_shadowed(c, mi) || m->is_transplanted_source) continue;
    const char *dcn = c->classes[defcls].c_name;
    /* Non-yielding &block: the prototype includes a sp_Proc * parameter
       (see emit_method_signature), so the dispatch must pass NULL for it. */
    int has_blk = m->blk_param && m->blk_param[0];
    if (m->ret == TY_IO) {
      buf_printf(b, "    case %d: return sp_%s_to_io(%s(sp_%s *)v.v.p%s);\n",
                 comp_class_index(c, c->classes[k].name),
                 dcn, c->classes[defcls].is_value_type ? "*" : "", dcn,
                 has_blk ? ", NULL" : "");
    } else {
      /* poly/unknown: the #to_io body boxes; unwrap via the poly->file path
         so a non-IO answer falls through to the caller's TypeError instead
         of a segfault. */
      buf_printf(b, "    case %d: { sp_RbVal _r = sp_%s_to_io(%s(sp_%s *)v.v.p%s); return (sp_File *)sp_poly_to_file(_r); }\n",
                 comp_class_index(c, c->classes[k].name),
                 dcn, c->classes[defcls].is_value_type ? "*" : "", dcn,
                 has_blk ? ", NULL" : "");
    }
  }
  buf_puts(b, "    default: break;\n  }\n  return NULL;\n}\n");
}

/* Generate sp_user_coerce_dispatch: a cls_id switch that runs the numeric
   coerce protocol from the ARGUMENT side. `5 + obj` is CRuby's
   `a, b = obj.coerce(5); a <op> b`; the static path already emits that
   directly whenever the object's class is known at the call site, but an
   operand that only reads poly reached sp_poly_binop_bad and raised
   "can't be coerced" instead (#3960). */
static void emit_user_coerce_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static sp_RbVal sp_user_coerce_dispatch(const char *op, sp_RbVal recv, sp_RbVal obj, sp_bool *handled) {\n");
  buf_puts(b, "  *handled = FALSE;\n  sp_PolyArray *_pr = NULL;\n  switch (obj.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated || !class_coerce_emittable(c, k)) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "coerce", &defcls);
    if (mi < 0) continue;
    Scope *m = &c->scopes[mi];
    LocalVar *p = scope_local(m, m->pnames[0]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    /* the parameter takes the boxed numeric receiver; a narrower slot would
       have to be unboxed, and only the poly and numeric shapes can be */
    const char *arg;
    if (pt == TY_POLY) arg = "recv";
    else if (pt == TY_INT) arg = "sp_poly_to_i(recv)";
    else if (pt == TY_FLOAT) arg = "sp_poly_to_f(recv)";
    else continue;
    const char *dcn = c->classes[defcls].c_name;
    /* a #coerce that answers a homogeneously-typed pair -- `[9.0, v.to_f]` is
       a Float array -- is just as valid as a poly one, and rejecting it left
       the operation lowered against the object's address. Convert it. */
    Scope *cm = &c->scopes[mi];
    int cidx = comp_class_index(c, c->classes[k].name);
    if (cm->ret == TY_POLY_ARRAY) {
      buf_printf(b, "    case %d: _pr = sp_%s_coerce(%s(sp_%s *)obj.v.p, %s); break;\n",
                 cidx, dcn, c->classes[defcls].is_value_type ? "*" : "", dcn, arg);
    }
    else {
      Buf callb = {0}, boxb = {0};
      buf_printf(&callb, "sp_%s_coerce(%s(sp_%s *)obj.v.p, %s)",
                 dcn, c->classes[defcls].is_value_type ? "*" : "", dcn, arg);
      emit_boxed_text(c, cm->ret, callb.p, &boxb);
      buf_printf(b, "    case %d: _pr = sp_poly_to_poly_array(%s); break;\n",
                 cidx, boxb.p ? boxb.p : "sp_box_nil()");
      free(callb.p); free(boxb.p);
    }
  }
  buf_puts(b, "    default: break;\n  }\n");
  /* a #coerce that answered something, but not a pair, is CRuby's TypeError --
     distinct from a class with no usable #coerce at all, which leaves the hook
     unhandled so the caller's own error stands. */
  buf_puts(b, "  if (_pr && _pr->len != 2) sp_raise_cls(\"TypeError\", \"coerce must return [x, y]\");\n");
  buf_puts(b, "  if (!_pr) return sp_box_nil();\n");
  buf_puts(b, "  SP_GC_ROOT(_pr);\n");
  /* The pair's own operator finishes the job: for the usual [Klass(other),
     self] that is the class's own method, reached through the binop hook. */
  buf_puts(b, "  sp_RbVal _rv = sp_poly_binop_apply(op, _pr->data[0], _pr->data[1]);\n");
  buf_puts(b, "  if (_rv.tag == SP_TAG_NIL) return sp_box_nil();\n");
  buf_puts(b, "  *handled = TRUE;\n  return _rv;\n}\n");
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
  buf_puts(b, "static sp_int sp_gen_obj_hash(int cls_id, void *p) {\n  switch (cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_hashkey(c, k)) continue;
    int defcls = -1;
    int mi = comp_method_in_chain(c, k, "hash", &defcls);
    Scope *m = &c->scopes[mi];   /* signature validated in class_is_hashkey */
    const char *dcn = c->classes[defcls].c_name;
    const char *slf = c->classes[defcls].is_value_type ? "*" : "";
    buf_printf(b, "    case %d: ", comp_class_index(c, c->classes[k].name));
    if (m->ret == TY_INT)
      buf_printf(b, "return (sp_int)sp_%s_%s(%s(sp_%s *)p);\n", dcn, mc(m->name), slf, dcn);
    else
      buf_printf(b, "return sp_rbval_hash_key(sp_%s_%s(%s(sp_%s *)p));\n", dcn, mc(m->name), slf, dcn);
  }
  /* Struct/Data value keys: hash is the running combination of member hashes. */
  for (int k = 0; k < c->nclasses; k++) {
    if (!class_is_valuekey(c, k)) continue;
    ClassInfo *ci = &c->classes[k];
    /* Unsigned accumulator, like the runtime's array and hash ones and the
       inline Struct#hash at a call site: the rolling h*31+x is meant to wrap,
       and on a signed type that is undefined behavior rather than wraparound.
       A member that holds the struct itself now contributes a large fixed
       constant, so a two-member struct whose self-reference is not last
       overflows on the very next multiply (UBSan caught it). */
    buf_printf(b, "    case %d: { sp_%s *o = (sp_%s *)p; uint64_t _h = %d;\n",
               comp_class_index(c, ci->name), ci->c_name, ci->c_name, ci->nivars + 1);
    for (int i = 0; i < ci->nivars; i++) {
      char fe[128]; snprintf(fe, sizeof fe, "o->iv_%s", iv_c(ci->ivars[i] + 1));
      Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, ci->ivar_types[i], fe, &bx);
      buf_printf(b, "      _h = _h * 31 + (uint64_t)sp_rbval_hash_key(%s);\n", bx.p ? bx.p : fe);
      free(bx.p);
    }
    buf_puts(b, "      return (sp_int)_h; }\n");
  }
  buf_puts(b, "    default: break;\n  }\n  return (sp_int)(uintptr_t)p;\n}\n");

  buf_puts(b, "static sp_bool sp_gen_obj_eql(int cls_id, void *a, void *b_) {\n  switch (cls_id) {\n");
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
    }
    else {
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
  buf_puts(b, "static sp_bool sp_obj_eq_dispatch(sp_RbVal a, sp_RbVal b) {\n  switch (a.cls_id) {\n");
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
      const char *iv = iv_c(ci->ivars[i] + 1);  /* skip leading '@', mangle to a C field */
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
   sp_tu_init() that installs the symbol/regex/class/global-mark hooks and
   compiles the literals at startup. */
void emit_regex_section(Compiler *c, Buf *b) {
  for (int i = 0; i < g_re_count; i++) {
    buf_printf(b, "static mrb_regexp_pattern *sp_re_pat_%d;\n", i);
  }
  /* sp_tu_init wires the hooks below. When none apply (a trivial program uses
     no symbols, regex, class machinery, or heap globals) neither the function
     nor its main() call is emitted, so the symbol/regex runtime it would pin
     stays unreferenced and links away. */
  if (!g_re_init_needed) return;
  /* Forward-declare the symbol interner and the Marshal object dispatchers so
     sp_tu_init can take their addresses before their definitions (emitted into
     the later `body` buffer). */
  if (g_uses_marshal) {
    buf_puts(b,
      "static sp_sym sp_sym_intern(const char *s);\n"
      "static int sp_marshal_obj_dump(sp_mar_buf *b, int cls_id, void *p);\n"
      "static sp_RbVal sp_marshal_obj_load(const char *name, sp_RbVal iv, int *ok);\n");
  }
  if (g_has_user_cmp)
    buf_puts(b, "static sp_int sp_obj_cmp_dispatch(sp_RbVal a, sp_RbVal b, sp_bool *comparable);\n");
  if (g_has_user_binop)
    buf_puts(b, "static sp_RbVal sp_user_binop_dispatch(const char *op, sp_RbVal a, sp_RbVal b, sp_bool *handled);\n");
  if (g_has_user_aset)
    buf_puts(b, "static void sp_user_aset_dispatch(sp_RbVal a, sp_RbVal k, sp_RbVal v, sp_bool *handled);\n");
  if (g_has_user_coerce)
    buf_puts(b, "static sp_RbVal sp_user_coerce_dispatch(const char *op, sp_RbVal recv, sp_RbVal obj, sp_bool *handled);\n");
  if (g_has_user_to_io)
    buf_puts(b, "static sp_File *sp_user_to_io_dispatch(sp_RbVal v);\n");
  if (g_needs_class_machinery)
    buf_puts(b, "static int sp_poly_is_a(sp_RbVal obj, sp_Class klass);\n");
  buf_puts(b, "static void *sp_poly_unbox_cls(sp_RbVal v, int cls, const char *want);\n");
  if (g_gen_obj_hash)
    buf_puts(b, "static sp_RbVal sp_obj_to_hash(sp_RbVal v);\n");
  if (g_gen_obj_to_json)
    buf_puts(b, "static const char *sp_obj_to_json(sp_RbVal v);\n");
  if (g_gen_obj_to_h)
    buf_puts(b, "static sp_RbVal sp_obj_to_h(sp_RbVal v);\n");
  if (g_gen_obj_struct_values)
    buf_puts(b, "static sp_RbVal sp_obj_struct_values(sp_RbVal v);\n");
  if (obj_to_a_any(c))
    buf_puts(b, "static sp_RbVal sp_obj_to_a(sp_RbVal v);\n");
  if (obj_to_ary_any(c))
    buf_puts(b, "static sp_RbVal sp_obj_to_ary(sp_RbVal v);\n");
  if (obj_deconstruct_any(c)) {
    buf_puts(b, "static sp_RbVal sp_obj_deconstruct(sp_RbVal v);\n");
    buf_puts(b, "static int sp_obj_is_data(int cls_id);\n");
  }
  if (g_gen_obj_with)
    buf_puts(b, "static sp_RbVal sp_obj_with(sp_RbVal v, sp_RbVal ov);\n");
  if (g_gen_obj_hashkey)
    buf_puts(b, "static sp_int sp_gen_obj_hash(int cls_id, void *p);\n"
                "static sp_bool sp_gen_obj_eql(int cls_id, void *a, void *b_);\n");
  if (g_gen_obj_valeq)
    buf_puts(b, "static sp_bool sp_obj_eq_dispatch(sp_RbVal a, sp_RbVal b);\n");
  /* sp_tu_init (once sp_re_init, from the days it only compiled the regex
     literals): the generated TU's startup, installing into the runtime the
     hooks this program needs and compiling its regex literals. Emitted, and
     called from main, only when at least one hook applies. */
  buf_puts(b, "static void sp_tu_init(void) {\n");
  /* SPINEL_ALLOC_REPORT type names: attach human names to the scan-fn keys
     the allocation counters use. Runtime-gated on the same flag, so a normal
     run does no work here (#1336). */
  buf_puts(b, "  if (sp_alloc_report_on) {\n"
              "    sp_alloc_report_tag((void *)sp_PolyArray_scan, \"Array\");\n"
              "    sp_alloc_report_tag((void *)sp_StrArray_scan, \"Array(String)\");\n"
              "    sp_alloc_report_tag((void *)sp_SymPolyHash_scan, \"Hash(Symbol)\");\n"
              "    sp_alloc_report_tag((void *)sp_StrPolyHash_scan, \"Hash(String)\");\n"
              "    sp_alloc_report_tag((void *)sp_PolyPolyHash_scan, \"Hash\");\n"
              "    sp_alloc_report_tag((void *)sp_Enumerator_scan, \"Enumerator\");\n"
              "    sp_alloc_report_tag((void *)sp_OpenStruct_scan, \"OpenStruct\");\n"
              "    sp_alloc_report_tag((void *)sp_Dir_scan, \"Dir\");\n"
              "    sp_alloc_report_tag((void *)sp_Proc_scan, \"Proc\");\n"
              "    sp_alloc_report_tag((void *)sp_exc_gc_scan, \"Exception\");\n"
              "    sp_alloc_report_tag((void *)sp_PtrArray_gc_scan, \"Array(Object)\");\n"
              "    sp_alloc_report_tag((void *)sp_StrIntHash_scan, \"Hash(String,Integer)\");\n"
              "    sp_alloc_report_tag((void *)sp_StrStrHash_scan, \"Hash(String,String)\");\n"
              "    sp_alloc_report_tag((void *)sp_IntStrHash_scan, \"Hash(Integer,String)\");\n"
              "    sp_alloc_report_tag((void *)sp_curry_scan, \"Proc(curried)\");\n"
              "    sp_alloc_report_tag((void *)sp_BoundMethod_scan, \"Method\");\n");
  for (int aci = 0; aci < c->nclasses; aci++) {
    ClassInfo *ci = &c->classes[aci];
    if (ci->is_native_class || !ci->instantiated) continue;
    int is_exc_iv = ci->nivars > 0 && class_is_exc_subclass(c, aci);
    if (!class_needs_scan(ci) && !is_exc_iv) continue;   /* no scan emitted */
    const char *rn = class_ruby_name(c, aci) ? class_ruby_name(c, aci) : ci->name;
    buf_printf(b, "    sp_alloc_report_tag((void *)sp_%s__gc_scan, \"%s\");\n", ci->c_name, rn);
  }
  buf_puts(b, "  }\n");
  if (g_uses_symbols)
    buf_puts(b, "  sp_sym_name_fn = sp_sym_to_s;\n");
  /* A C stack that ran out becomes a catchable SystemStackError: the fault
     handler in the runtime archive cannot reach this TU's exception stack,
     so hand it the raise (see sp_raise_stack_overflow). */
  buf_puts(b, "  sp_stack_overflow_raise_fn = sp_raise_stack_overflow;\n");
  buf_puts(b, "  sp_stack_guard_init();\n");
  if (g_has_user_cmp)
    buf_puts(b, "  sp_obj_cmp_hook = sp_obj_cmp_dispatch;\n");
  if (g_has_user_binop)
    buf_puts(b, "  sp_user_binop_hook = sp_user_binop_dispatch;\n");
  if (g_has_user_aset)
    buf_puts(b, "  sp_user_aset_hook = sp_user_aset_dispatch;\n");
  if (g_has_user_coerce)
    buf_puts(b, "  sp_user_coerce_hook = sp_user_coerce_dispatch;\n");
  if (g_has_user_to_io)
    buf_puts(b, "  sp_user_to_io_hook = sp_user_to_io_dispatch;\n");
  if (g_gen_obj_hashkey)
    buf_puts(b, "  sp_obj_hash_hook = sp_gen_obj_hash;\n  sp_obj_eql_hook = sp_gen_obj_eql;\n");
  if (g_gen_obj_valeq)
    buf_puts(b, "  sp_obj_eq_hook = sp_obj_eq_dispatch;\n");
  if (g_needs_class_machinery)
    buf_puts(b, "  sp_user_exc_parent_fn = sp_user_exc_parent;\n"
                "  sp_user_exc_modules_fn = sp_user_exc_modules;\n"
                "  sp_poly_is_a_hook = sp_poly_is_a;\n"
                "  sp_class_le_id_fn = sp_class_le_ids;\n"
                "  sp_class_kind_of_name_fn = sp_class_kind_of_name;\n");
  /* an unoptimised build runs its fibers on 1 MB stacks (see g_opt_level);
     SPINEL_FIBER_STACK in the environment still wins */
  if (g_opt_level < 2)
    buf_puts(b, "  sp_fiber_stack_hint((size_t)1 << 20);\n");
  /* Replace the runtime's hook with the superset that also marks this
     program's heap-typed globals/constants/class-ivars (it chains to
     sp_re_mark_globals itself). Skipped when there are none -- the marker would
     equal the constructor-installed default, so it isn't emitted either. */
  if (g_has_user_global_marks)
    buf_puts(b, "  sp_gc_mark_globals_hook = sp_mark_user_globals;\n");
  /* The out-of-int64 literal table is filled here, at startup, once the
     marker above can see it: filled lazily on first use it was a read-test-
     write on a shared slot that two threads reaching the same literal raced
     on, and the Bignum one of them built was dropped mid-return (#4641).
     A compile-time constant has no reason to wait. */
  if (g_bigl_n)
    buf_printf(b, "  for (int _i = 0; _i < %d; _i++) sp_bigl[_i] = sp_bigint_new_str(sp_bigl_s[_i], 10);\n",
               g_bigl_n);
  /* Install the Marshal vtable: the construction wrappers (spinel_rt.h) plus
     the generated symbol interner and per-class object dump/load. */
  if (g_emit_sym_rt)
    buf_puts(b, "  sp_json_sym_intern_fn = sp_sym_intern;\n");
  if (g_emit_obj_dispatch) {
    buf_puts(b, "  sp_obj_inspect_fn = sp_obj_inspect_sw;\n");
    buf_puts(b, "  sp_obj_to_s_fn = sp_obj_to_s_sw;\n");
    buf_puts(b, "  sp_obj_to_int_fn = sp_obj_to_int_sw;\n");
    buf_puts(b, "  sp_obj_to_str_fn = sp_obj_to_str_sw;\n");
    buf_puts(b, "  sp_obj_to_path_fn = sp_obj_to_path_sw;\n");
    if (c->uses_kconv) buf_puts(b, "  sp_obj_conv_fn = sp_obj_conv_sw;\n");
    buf_puts(b, "  sp_obj_cls_name_fn = sp_obj_cls_name_rt;\n");
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
  if (g_gen_obj_to_json)
    buf_puts(b, "  sp_obj_to_json_fn = sp_obj_to_json;\n");
  if (g_gen_obj_to_h)
    buf_puts(b, "  sp_obj_to_h_fn = sp_obj_to_h;\n");
  if (g_gen_obj_struct_values)
    buf_puts(b, "  sp_obj_struct_values_fn = sp_obj_struct_values;\n");
  if (obj_to_a_any(c))
    buf_puts(b, "  sp_obj_to_a_fn = sp_obj_to_a;\n");
  if (obj_to_ary_any(c))
    buf_puts(b, "  sp_obj_to_ary_fn = sp_obj_to_ary;\n");
  if (obj_deconstruct_any(c)) {
    buf_puts(b, "  sp_obj_deconstruct_fn = sp_obj_deconstruct;\n");
    buf_puts(b, "  sp_obj_is_data_fn = sp_obj_is_data;\n");
  }
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
      buf_printf(b, "  }\nelse {\n    (void)sp_re_pat_%d;\n    sp_re_startup_fail();\n  }\n", i);
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
int scope_def_line(Compiler *c, Scope *s) {
  if (s->def_node < 0) return 0;
  return (int)nt_int(c->nt, s->def_node, "node_line", 0);
}
const char *scope_def_file(Compiler *c, Scope *s) {
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
    case TY_STR_RANGE:             buf_puts(b, "Range[String]"); break;
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
    case TY_ADDRINFO:              buf_puts(b, "Addrinfo"); break;
    case TY_SOCKOPT:               buf_puts(b, "Socket::Option"); break;
    case TY_TMS:                   buf_puts(b, "Process::Tms"); break;
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
/* A method's type as RBS, `(Integer, Integer) -> Array[Integer]`. Answers
   whether any slot widened to untyped. */
static int rbs_method_type_into(Compiler *c, Buf *b, Scope *s) {
  int degraded = 0;
  buf_puts(b, "(");
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
  return degraded;
}
static void rbs_method_line(Compiler *c, Buf *b, const char *defprefix, Scope *s) {
  buf_printf(b, "  %s: ", defprefix);
  int degraded = rbs_method_type_into(c, b, s);
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

/* One widened slot of a method whose signature degraded: the parameter (or
   the return, `param` NULL) and where its marker sits -- the parameter's own
   node, found by name under the def, or the def for a return. */
typedef struct {
  Scope *s;
  const char *param;   /* the parameter's name, NULL for the return */
  int fid, line, col, end_line, end_col;   /* end_line 0 when the parser stamped no end */
} WidenedSlot;

/* Walk every widened slot, in scope order then parameter order, calling
   `fn` on each. The two readers -- the --emit-types JSON and --warn-widen's
   stderr lines -- see the same slots at the same positions (#4522). */
static void each_widened_slot(Compiler *c, void (*fn)(Compiler *, const WidenedSlot *, void *), void *ud) {
  const NodeTable *nt = c->nt;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!s->name || !*s->name || s->def_node < 0) continue;
    if (!scope_sig_degraded(c, s)) continue;
    int dln = (int)nt_int(nt, s->def_node, "node_line", 0);
    if (dln <= 0) continue;
    int dcol = (int)nt_int(nt, s->def_node, "node_col", 0);
    int fid = (int)nt_int(nt, s->def_node, "node_file", 0);
    for (int i = 0; i <= s->nparams; i++) {
      WidenedSlot w; memset(&w, 0, sizeof w);
      w.s = s; w.fid = fid;
      int at = s->def_node;
      if (i < s->nparams) {
        LocalVar *p = scope_local(s, s->pnames[i]);
        TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
        if (!ty_is_degraded(pt) || !s->pnames[i]) continue;
        w.param = s->pnames[i];
        /* the parameter's own node: the one *ParameterNode of this scope
           that carries the name */
        for (int pid = 0; pid < nt->count && pid < c->node_cap; pid++) {
          if (c->nscope[pid] != si) continue;
          const char *pty = nt_type(nt, pid);
          if (!pty || !strstr(pty, "ParameterNode")) continue;
          const char *pn = nt_str(nt, pid, "name");
          if (pn && sp_streq(pn, w.param) && nt_int(nt, pid, "node_line", 0) > 0) { at = pid; break; }
        }
      }
      else if (!ty_is_degraded(s->ret)) continue;
      w.line = (int)nt_int(nt, at, "node_line", 0); w.col = (int)nt_int(nt, at, "node_col", 0);
      if (w.line <= 0) { w.line = dln; w.col = dcol; }
      w.end_line = (int)nt_int(nt, at, "node_end_line", 0);
      w.end_col = (int)nt_int(nt, at, "node_end_col", 0);
      fn(c, &w, ud);
    }
  }
}

/* ---- why a slot widened: the chain from the slot to the birth ---- */

/* The text of node `id` from its source file, one line, at most 48 chars. */
static const char *why_slice(Compiler *c, int id, char *out, size_t cap) {
  static char *cache_path; static char *cache_text;
  const NodeTable *nt = c->nt;
  out[0] = '\0';
  int ln = (int)nt_int(nt, id, "node_line", 0), col = (int)nt_int(nt, id, "node_col", 0);
  int eln = (int)nt_int(nt, id, "node_end_line", 0), ecol = (int)nt_int(nt, id, "node_end_col", 0);
  if (ln <= 0) return out;
  const char *path = emit_file_path(c, (int)nt_int(nt, id, "node_file", 0));
  if (!cache_path || !sp_streq(cache_path, path)) {
    FILE *f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *t = malloc((size_t)n + 1);
    if (!t) { fclose(f); return out; }
    size_t got = fread(t, 1, (size_t)n, f); t[got] = '\0'; fclose(f);
    free(cache_path); free(cache_text);
    cache_path = strdup(path); cache_text = t;
  }
  const char *p = cache_text; int at = 1;
  while (at < ln && *p) { if (*p == '\n') at++; p++; }
  for (int i = 0; i < col && *p && *p != '\n'; i++) p++;
  const char *e = p;
  if (eln == ln && ecol > col) { for (int i = col; i < ecol && *e && *e != '\n'; i++) e++; }
  else if (eln > ln) { while (*e && *e != '\n') e++; }
  else { while (*e && *e != '\n') e++; }
  size_t len = (size_t)(e - p);
  int cut = 0;
  if (len > cap - 4) { len = cap - 4; cut = 1; }
  memcpy(out, p, len); out[len] = '\0';
  if (cut || eln > ln) strcat(out, "…");
  return out;
}

static void why_rbs(Compiler *c, TyKind t, char *out, size_t cap) {
  Buf b; memset(&b, 0, sizeof b);
  ty_to_rbs_into(c, t, &b);
  snprintf(out, cap, "%s", b.p ? b.p : "?");
  free(b.p);
}

/* One hop of a why chain, rendered for stderr and for the JSON. */
typedef struct { Compiler *c; Buf *json; int n; } WhyOut;

static void why_hop(WhyOut *o, int id, const char *role, const char *extra) {
  Compiler *c = o->c; const NodeTable *nt = c->nt;
  int ln = (int)nt_int(nt, id, "node_line", 0), col = (int)nt_int(nt, id, "node_col", 0);
  int fid = (int)nt_int(nt, id, "node_file", 0);
  char text[64], rbs[128];
  why_slice(c, id, text, sizeof text);
  if (id < c->node_cap && c->ntype[id] == TY_UNKNOWN) snprintf(rbs, sizeof rbs, "untyped (no type of its own)");
  else why_rbs(c, id < c->node_cap ? c->ntype[id] : TY_UNKNOWN, rbs, sizeof rbs);
  if (!o->json) {
    /* a synthesized node (an `__enum_to_a` body, a splice) has no source
       line to point at and nothing to quote */
    if (ln <= 0) fprintf(stderr, "spinel: note: %s a synthesized node (no source) is %s%s\n", role, rbs, extra ? extra : "");
    else fprintf(stderr, "spinel: %s:%d:%d: note: %s `%s` is %s%s\n", emit_file_path(c, fid), ln, col + 1, role, text, rbs, extra ? extra : "");
  }
  else {
    Buf *b = o->json;
    if (o->n > 0) buf_puts(b, ",");
    buf_puts(b, "{\"file\":\""); json_escape_into(b, emit_file_path(c, fid));
    buf_printf(b, "\",\"line\":%d,\"col\":%d", ln, col);
    { int eln = (int)nt_int(nt, id, "node_end_line", 0);
      if (eln > 0) buf_printf(b, ",\"end_line\":%d,\"end_col\":%d", eln, (int)nt_int(nt, id, "node_end_col", 0)); }
    buf_puts(b, ",\"role\":\""); json_escape_into(b, role);
    buf_puts(b, "\",\"rbs\":\""); json_escape_into(b, rbs);
    buf_puts(b, "\"");
    if (extra) { buf_puts(b, ",\"note\":\""); json_escape_into(b, extra); buf_puts(b, "\""); }
    buf_puts(b, "}");
  }
  o->n++;
}

/* The return whose recorded value is node `id`, if a def's is: the chain
   reached a callee's return that met two kinds. */
static const SlotWhy *why_return_of(Compiler *c, int id) {
  for (int si = 1; si < c->nscopes; si++)
    if (c->scopes[si].ret_why.node == id && ty_degraded(c->scopes[si].ret)) return &c->scopes[si].ret_why;
  return NULL;
}

/* For an empty literal written to a local: the class of an object later
   pushed into that local in the same scope (`r = []; ... r << Foo.new`),
   -1 when none is. */
static int why_pushed_object(Compiler *c, int lit) {
  const NodeTable *nt = c->nt;
  if (nt_kind(nt, lit) != NK_ArrayNode) return -1;
  const char *nm = NULL; Scope *ws = NULL; int wid = -1;
  NT_FOREACH_KIND(nt, NK_LocalVariableWriteNode, w) {
    if (nt_ref(nt, w, "value") != lit) continue;
    nm = nt_str(nt, w, "name"); ws = comp_scope_of(c, w); wid = w; break;
  }
  if (!nm || !ws) return -1;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    if (id < wid) continue;   /* a push before the write fills an earlier value of the name */
    const char *cn = nt_str(nt, id, "name");
    if (!cn || !(sp_streq(cn, "<<") || sp_streq(cn, "push"))) continue;
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *rn = nt_str(nt, recv, "name");
    if (!rn || !sp_streq(rn, nm) || comp_scope_of(c, recv) != ws) continue;
    int args = nt_ref(nt, id, "arguments");
    int n = 0; const int *a = args >= 0 ? nt_arr(nt, args, "arguments", &n) : NULL;
    if (a && n >= 1 && a[0] < c->node_cap && ty_is_object(c->ntype[a[0]])) return ty_object_class(c->ntype[a[0]]);
  }
  return -1;
}

/* The end of a chain at a slot whose widening value is not untyped in the
   end: two concrete kinds met, or a transient the fixpoint kept. */
static void why_slot_end(WhyOut *o, const SlotWhy *w, const char *role) {
  Compiler *c = o->c;
  TyKind now = c->ntype[w->node];
  char extra[224], b[64];
  int other_ok = w->other >= 0 && w->other < c->node_cap && w->other != w->node;
  /* two concrete kinds met: the slot held one (prev, from `other`) and this
     value brought another; or, for a return, the tail and a `return`. The
     kind that met is `other`'s when there is one -- `prev` is what the slot
     held, which a return's tail and `return` can both already have given it
     (a String tail, a `return nil`: "String, where the slot was String"). */
  int is_ret = sp_streq(role, "returned") || why_return_of(c, w->node) == w;
  int meet = !ty_degraded(w->then) && ((w->prev != TY_UNKNOWN && w->prev != now) || other_ok);
  if (meet) {
    if (other_ok) why_rbs(c, c->ntype[w->other], b, sizeof b); else why_rbs(c, w->prev, b, sizeof b);
    snprintf(extra, sizeof extra, ", where %s %s (two kinds meet: untyped)", is_ret ? "a `return` gives" : "the slot was", b);
  }
  else if (now == TY_UNKNOWN) {
    /* an empty literal's slot is untyped either because the round guessed
       before a write typed it (pessimistic: a fixpoint imprecision) or
       because what fills it later has no typed form anyway -- an Array of
       objects is Array[untyped] by representation, and the round is not
       to blame. Tell them apart by what is pushed. */
    int pcls = why_pushed_object(c, w->node);
    if (pcls >= 0) snprintf(extra, sizeof extra, " (no type of its own: an empty literal); the %s pushed into it makes an Array of objects, which has no typed form: Array[untyped] by representation", class_ruby_name(c, pcls));
    else snprintf(extra, sizeof extra, " (no type of its own: an empty literal, or nothing typed it); on round %d nothing else had typed the slot and it took untyped (pessimistic)", w->round);
  }
  else snprintf(extra, sizeof extra, "; on round %d of inference it was still untyped and the slot kept that (a transient)", w->round);
  why_hop(o, w->node, role, extra);
  if (meet && other_ok) why_hop(o, w->other, "and", NULL);
}

/* The slot a read names, if the read's type came from one with a why. */
static const LocalVar *why_read_slot(Compiler *c, int id) {
  if (nt_kind(c->nt, id) != NK_LocalVariableReadNode) return NULL;
  const char *nm = nt_str(c->nt, id, "name");
  Scope *s = comp_scope_of(c, id);
  LocalVar *lv = (nm && s) ? scope_local(s, nm) : NULL;
  return (lv && (lv->why.node >= 0 || lv->why.reason)) ? lv : NULL;
}

/* For a read of a block parameter: the call whose block declares it (the
   tightest block span containing the read), and that call's receiver. -1
   when none. Structural, at render time: the block-param inference has too
   many sites to record at each, and the relation is the block's. */
static int why_block_recv(Compiler *c, int read_id, const char *pname) {
  const NodeTable *nt = c->nt;
  int rl = (int)nt_int(nt, read_id, "node_line", 0), rc = (int)nt_int(nt, read_id, "node_col", 0);
  if (rl <= 0) return -1;
  int best = -1; long best_len = -1;
  NT_FOREACH_KIND(nt, NK_CallNode, id) {
    int blk = nt_ref(nt, id, "block");
    if (blk < 0 || nt_kind(nt, blk) != NK_BlockNode) continue;
    int bl = (int)nt_int(nt, blk, "node_line", 0), bc = (int)nt_int(nt, blk, "node_col", 0);
    int el = (int)nt_int(nt, blk, "node_end_line", 0), ec = (int)nt_int(nt, blk, "node_end_col", 0);
    if (bl <= 0 || el <= 0) continue;
    if (rl < bl || (rl == bl && rc < bc) || rl > el || (rl == el && rc >= ec)) continue;
    /* declares the name? */
    int params = nt_ref(nt, blk, "parameters");
    int pn = params >= 0 ? nt_ref(nt, params, "parameters") : -1;
    int found = 0;
    if (pn >= 0) {
      int n = 0; const int *req = nt_arr(nt, pn, "requireds", &n);
      for (int i = 0; i < n && !found; i++) { const char *nm = nt_str(nt, req[i], "name"); if (nm && sp_streq(nm, pname)) found = 1; }
    }
    if (!found) continue;
    long len = (long)(el - bl) * 100000 + (ec - bc);
    if (best < 0 || len < best_len) { best = id; best_len = len; }
  }
  return best >= 0 ? nt_ref(nt, best, "receiver") : -1;
}

/* For a send on a poly receiver whose builtin answer is concrete: the scope
   of a user instance method of the name whose return degraded and has a why,
   -1 when none (the receiver hop stands: the builtin answer is untyped too,
   or no candidate's return is). */
static int why_poly_candidate(Compiler *c, int id) {
  const char *name = nt_str(c->nt, id, "name");
  if (!name) return -1;
  int any = -1;
  for (int si = 1; si < c->nscopes; si++) {
    Scope *m = &c->scopes[si];
    if (!m->name || m->def_node < 0 || m->is_cmethod || m->class_id < 0 || !sp_streq(m->name, name)) continue;
    if (!ty_degraded(m->ret) || m->ret_why.node < 0 || m->ret_why.node >= c->node_cap || m->ret_why.node == id) continue;
    any = si; break;
  }
  if (any < 0) return -1;
  return ty_degraded(an_builtin_answer(c, id)) ? -1 : any;
}

/* Follow a slot's why to the expression the poly was born at. `first` is
   what the slot's node is to the slot: passed, written, returned. */
/* A note with no position of its own: a rule's words about the slot. */
static void why_plain(WhyOut *o, const char *text) {
  if (!o->json) { fprintf(stderr, "spinel: note: %s\n", text); return; }
  Buf *b = o->json;
  if (o->n > 0) buf_puts(b, ",");
  buf_puts(b, "{\"role\":\"rule\",\"note\":\""); json_escape_into(b, text); buf_puts(b, "\"}");
  o->n++;
}

static void why_chain(WhyOut *o, const SlotWhy *w, const char *first) {
  Compiler *c = o->c;
  const NodeTable *nt = c->nt;
  if (w->reason) {
    /* a rule degraded the slot: its words, at its subject when it has one.
       A subject that is itself untyped (a default that is an untyped
       expression) leads on as any value would. */
    int subj = (w->node >= 0 && w->node < c->node_cap) ? w->node : -1;
    if (subj < 0) { why_plain(o, w->reason); return; }
    char extra[256]; snprintf(extra, sizeof extra, " -- %s", w->reason);
    if (!ty_degraded(c->ntype[subj]) || c->norigin[subj] == subj || c->norigin[subj] < 0) { why_hop(o, subj, "by", extra); return; }
    why_hop(o, subj, "by", extra);
    SlotWhy on = *w; on.reason = NULL;
    why_chain(o, &on, "from");
    return;
  }
  if (w->node < 0 || w->node >= c->node_cap) {
    why_plain(o, "untraced: no record of what widened it");
    return;
  }
  if (!ty_degraded(c->ntype[w->node])) { why_slot_end(o, w, first); return; }
  int id = w->node, depth = 0;
  const char *role = first;
  int seen[16]; int ns = 0;
  int last_ln = -1, last_col = -1;
  for (;;) {
    int next = c->norigin[id];
    int born = (next == id), lost = (next < 0 || next >= c->node_cap);
    int cyc = 0; for (int i = 0; i < ns; i++) if (seen[i] == next) cyc = 1;
    if (ns < 16) seen[ns++] = id;
    int ln = (int)nt_int(nt, id, "node_line", 0), col = (int)nt_int(nt, id, "node_col", 0);
    /* not a hop of its own: a synthesized node, a wrapper (a body, an arm,
       parentheses), or a node whose origin starts at the same place */
    NodeKind k = nt_kind(nt, id);
    int wrapper = k == NK_StatementsNode || k == NK_ElseNode || k == NK_ParenthesesNode || k == NK_BeginNode;
    /* a head wrapper whose tail starts where it does is not a hop either:
       the tail is the hop, and carries what there is to say about it */
    int same_next = !born && !lost && ln == (int)nt_int(nt, next, "node_line", 0) && col == (int)nt_int(nt, next, "node_col", 0);
    int silent = ln <= 0 || (ln == last_ln && col == last_col) || (wrapper && !born && !lost && (depth > 0 || same_next));
    /* a read whose slot's widening value is concrete in the end: the
       slot's own story ends the chain */
    const LocalVar *rl = why_read_slot(c, id);
    const SlotWhy *rs = rl ? &rl->why : NULL;
    if (rs && rs->reason) {
      /* a slot a rule degraded: the rule's words end the chain */
      if (!silent) why_hop(o, id, role, NULL);
      why_chain(o, rs, "by");
      return;
    }
    if (rs && !ty_degraded(c->ntype[rs->node])) {
      if (!silent) why_hop(o, id, role, NULL);
      why_slot_end(o, rs, rl->is_param ? "passed" : "written");
      return;
    }
    if (born && rs == NULL && nt_kind(nt, id) == NK_LocalVariableReadNode) {
      /* a block parameter: it takes the receiver's elements */
      const char *nm = nt_str(nt, id, "name");
      Scope *sc = comp_scope_of(c, id);
      LocalVar *lv = (nm && sc) ? scope_local(sc, nm) : NULL;
      int recv = (lv && lv->is_block_param) ? why_block_recv(c, id, nm) : -1;
      if (recv >= 0 && recv < c->node_cap && ty_degraded(c->ntype[recv]) && recv != id) {
        if (!silent) why_hop(o, id, role, " (a parameter of the block on:)");
        role = "from"; id = recv; last_ln = -1;
        continue;
      }
      why_hop(o, id, role, " -- untraced from here (how the local was typed is not recorded)");
      return;
    }
    if (born && k == NK_InstanceVariableReadNode) { why_hop(o, id, role, " -- untraced from here: no write of it in this class is untyped (its kind is the writes' meeting, or a rule's)"); return; }
    if (born) { why_hop(o, id, role, " -- born here: no untyped input"); return; }
    if (k == NK_CallNode && !lost && next == nt_ref(nt, id, "receiver")) {
      /* a send on a poly receiver: its result is the meeting of every user
         def of the name with the builtin answer. When the builtin answer is
         concrete (`to_s` is a String on anything), the receiver being
         untyped is not why the result is -- a user candidate whose return
         degraded is, and one such makes every `.to_s` on a poly receiver
         untyped, program-wide. Follow that candidate's return. */
      int cand = why_poly_candidate(c, id);
      if (cand >= 0) {
        char cx[160];
        snprintf(cx, sizeof cx, " (a candidate of the send, `%s#%s`, returns untyped)", class_ruby_name(c, c->scopes[cand].class_id), c->scopes[cand].name);
        /* printed even where a wrapper already named this position: the
           candidate is the news */
        why_hop(o, id, role, cx);
        const SlotWhy *rw = &c->scopes[cand].ret_why;
        if (!ty_degraded(c->ntype[rw->node])) { why_slot_end(o, rw, "returned"); return; }
        role = "returned"; id = rw->node; last_ln = -1; depth++;
        continue;
      }
    }
    if (lost && !ty_degraded(c->ntype[id])) {
      /* a concrete value the chain arrived at: a callee's return that met
         two kinds, else a value the fixpoint saw untyped for a while */
      const SlotWhy *rw = why_return_of(c, id);
      if (rw) { why_slot_end(o, rw, role); return; }
    }
    if (lost || cyc || depth > 40) { why_hop(o, id, role, " -- untraced from here"); return; }
    if (!silent) { why_hop(o, id, role, NULL); last_ln = ln; last_col = col; role = "from"; }
    depth++;
    id = next;
  }
}

/* --warn-widen: the widened slots on stderr as `spinel: file:line:col:
   warning: ...` (the other warnings' form, with the column added, 1-based as
   a compiler's warning is read by an editor), one per slot, at the slot. A
   plain compile said nothing about a signature that degraded to untyped; the
   fact lived only in --emit-types and as a comment in --emit-rbs. */
static void warn_widened_slot(Compiler *c, const WidenedSlot *w, void *ud) {
  (void)ud;
  WhyOut o = { c, NULL, 0 };
  if (w->param) {
    fprintf(stderr, "spinel: %s:%d:%d: warning: parameter `%s` of `%s` widened to untyped (boxed poly slow path)\n",
            emit_file_path(c, w->fid), w->line, w->col + 1, w->param, w->s->name);
    LocalVar *p = scope_local(w->s, w->param);
    int kwrest = w->s->kwrest_idx >= 0 && w->s->pnames[w->s->kwrest_idx] && sp_streq(w->s->pnames[w->s->kwrest_idx], w->param);
    if (kwrest) why_plain(&o, "by construction: a splat parameter holds the extra keywords of every call, untyped");
    else if (p && p->type == TY_UNKNOWN) why_plain(&o, "never bound: no call site gives it a type");
    else if (p) why_chain(&o, &p->why, "passed");
  }
  else {
    fprintf(stderr, "spinel: %s:%d:%d: warning: the return of `%s` widened to untyped (boxed poly slow path)\n",
            emit_file_path(c, w->fid), w->line, w->col + 1, w->s->name);
    why_chain(&o, &w->s->ret_why, "returned");
  }
}

/* The --emit-types record of one widened slot. */
typedef struct { Buf *b; int n; } WidenJson;
static void json_widened_slot(Compiler *c, const WidenedSlot *w, void *ud) {
  WidenJson *j = (WidenJson *)ud;
  Buf *b = j->b;
  if (j->n > 0) buf_puts(b, ",\n");
  buf_puts(b, "    {\"file\":\"");
  json_escape_into(b, emit_file_path(c, w->fid));
  buf_printf(b, "\",\"line\":%d,\"col\":%d", w->line, w->col);
  if (w->end_line > 0) buf_printf(b, ",\"end_line\":%d,\"end_col\":%d", w->end_line, w->end_col);
  buf_puts(b, ",\"severity\":\"warning\",\"method\":\"");
  json_escape_into(b, w->s->name);
  if (w->param) { buf_puts(b, "\",\"slot\":\"param\",\"param\":\""); json_escape_into(b, w->param); buf_puts(b, "\""); }
  else buf_puts(b, "\",\"slot\":\"return\"");
  buf_puts(b, ",\"message\":\"");
  Buf msg; memset(&msg, 0, sizeof msg);
  if (w->param) buf_printf(&msg, "Spinel: parameter `%s` of `%s` widened to untyped (boxed poly slow path)", w->param, w->s->name);
  else buf_printf(&msg, "Spinel: the return of `%s` widened to untyped (boxed poly slow path)", w->s->name);
  json_escape_into(b, msg.p ? msg.p : "");
  free(msg.p);
  buf_puts(b, "\",\"why\":[");
  { WhyOut o = { c, b, 0 };
    if (w->param) {
      LocalVar *p = scope_local(w->s, w->param);
      int kwrest = w->s->kwrest_idx >= 0 && w->s->pnames[w->s->kwrest_idx] && sp_streq(w->s->pnames[w->s->kwrest_idx], w->param);
      if (kwrest) why_plain(&o, "by construction: a splat parameter holds the extra keywords of every call, untyped");
      else if (p && p->type == TY_UNKNOWN) why_plain(&o, "never bound: no call site gives it a type");
      else if (p) why_chain(&o, &p->why, "passed");
    }
    else why_chain(&o, &w->s->ret_why, "returned"); }
  buf_puts(b, "]}");
  j->n++;
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
    /* A parameter is not an expression the typer stamps, so it had no record
       and a consumer found no declaration to go to, and hover on it answered
       the def's signature. Its type is the slot's, the one the signature
       prints for it: untyped for a widened slot, so the parameter's own
       record says what the warning on it says (#4557). */
    { const char *pk = nt_type(nt, id);
      const char *pn = pk && strstr(pk, "ParameterNode") ? nt_str(nt, id, "name") : NULL;
      if (pn && *pn && c->nscope && c->nscope[id] >= 0 && c->nscope[id] < c->nscopes) {
        LocalVar *pl = scope_local(&c->scopes[c->nscope[id]], pn);
        t = (pl && pl->type != TY_UNKNOWN) ? pl->type : TY_POLY;
      }
      else if (t == TY_UNKNOWN || t == TY_VOID) continue; }
    int ln = (int)nt_int(nt, id, "node_line", 0);
    if (ln <= 0) continue;
    int col = (int)nt_int(nt, id, "node_col", 0);
    int fid = (int)nt_int(nt, id, "node_file", 0);
    if (tn > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"file\":\"");
    json_escape_into(&b, emit_file_path(c, fid));
    buf_printf(&b, "\",\"line\":%d,\"col\":%d", ln, col);
    /* the span's end (the parser stamps it for this mode), the node's Prism
       kind and, where the node names something (a call, a variable, a
       constant, a def, a parameter), that name: what a consumer needs to
       pick the expression under a cursor and to find its other mentions
       (#4522) */
    { int eln = (int)nt_int(nt, id, "node_end_line", 0);
      if (eln > 0) buf_printf(&b, ",\"end_line\":%d,\"end_col\":%d", eln, (int)nt_int(nt, id, "node_end_col", 0)); }
    { const char *kind = nt_type(nt, id);
      if (kind) { buf_puts(&b, ",\"kind\":\""); json_escape_into(&b, kind); buf_puts(&b, "\""); }
      const char *nm = nt_str(nt, id, "name");
      if (nm && *nm) { buf_puts(&b, ",\"name\":\""); json_escape_into(&b, nm); buf_puts(&b, "\""); } }
    buf_puts(&b, ",\"type\":\"");
    Buf tag; memset(&tag, 0, sizeof tag);
    ty_tag_into(c, t, &tag);
    json_escape_into(&b, tag.p ? tag.p : "");
    free(tag.p);
    buf_puts(&b, "\",\"rbs\":\"");
    Buf rbs; memset(&rbs, 0, sizeof rbs);
    ty_to_rbs_into(c, t, &rbs);
    json_escape_into(&b, rbs.p ? rbs.p : "");
    free(rbs.p);
    buf_puts(&b, "\"");
    /* a def's `type` is the def expression's value (a Symbol); the method
       type it declares is the `signature`, the same text --emit-rbs writes
       for it, so a consumer needs no second pass for the inferred
       signatures (rubys/spinel-ide) */
    { const char *kind = nt_type(nt, id);
      if (kind && sp_streq(kind, "DefNode")) {
        for (int si = 1; si < c->nscopes; si++) {
          Scope *s = &c->scopes[si];
          if (s->def_node != id) continue;
          Buf sig; memset(&sig, 0, sizeof sig);
          int degraded = rbs_method_type_into(c, &sig, s);
          buf_puts(&b, ",\"owner\":\"");
          json_escape_into(&b, s->class_id >= 0 ? c->classes[s->class_id].name : "Object");
          buf_puts(&b, "\"");
          if (s->is_cmethod) buf_puts(&b, ",\"singleton\":true");
          buf_puts(&b, ",\"signature\":\"");
          json_escape_into(&b, sig.p ? sig.p : "");
          buf_puts(&b, "\"");
          if (degraded) buf_puts(&b, ",\"widened\":true");
          free(sig.p);
          break;
        }
      } }
    buf_puts(&b, "}");
    tn++;
  }
  buf_puts(&b, "\n  ],\n  \"diagnostics\": [\n");
  int dn = 0;
  /* the refusals, in the order the compile met them */
  for (int di = 0; di < g_ndiags; di++) {
    if (dn > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"file\":\"");
    json_escape_into(&b, g_diags[di].file ? g_diags[di].file : "");
    buf_printf(&b, "\",\"line\":%d,\"col\":0,\"severity\":\"error\",\"message\":\"", g_diags[di].line);
    json_escape_into(&b, g_diags[di].msg);
    buf_puts(&b, "\"}");
    dn++;
  }
  /* One warning per widened SLOT, at the slot: a parameter's marker sits on
     the parameter (its node, found by name under the def), a return's on the
     def. The marker used to sit on the def and say "a parameter or return",
     and the reader worked out which from the RBS (#4522). */
  { WidenJson j = { &b, dn };
    each_widened_slot(c, json_widened_slot, &j);
    dn = j.n; }
  /* What codegen decided, per node it decided for (#4522): every emitted
     CallNode's dispatch, every BlockNode's fate. A block nothing lowered to a
     function of its own was spliced in place. */
  buf_puts(&b, "\n  ],\n  \"codegen\": [\n");
  int cn = 0;
  for (int id = 0; id < nt->count && id < c->node_cap; id++) {
    const char *kind = nt_type(nt, id);
    if (!kind) continue;
    int is_call = sp_streq(kind, "CallNode"), is_blk = sp_streq(kind, "BlockNode");
    if (!is_call && !is_blk) continue;
    int d = (g_ndecide && id < g_ndecide_cap) ? g_ndecide[id] : ND_NONE;
    if (is_call && d == ND_NONE) continue;   /* never emitted: dead, or folded away */
    int ln = (int)nt_int(nt, id, "node_line", 0);
    if (ln <= 0) continue;
    int col = (int)nt_int(nt, id, "node_col", 0);
    int fid = (int)nt_int(nt, id, "node_file", 0);
    if (cn > 0) buf_puts(&b, ",\n");
    buf_puts(&b, "    {\"file\":\"");
    json_escape_into(&b, emit_file_path(c, fid));
    buf_printf(&b, "\",\"line\":%d,\"col\":%d", ln, col);
    { int eln = (int)nt_int(nt, id, "node_end_line", 0);
      if (eln > 0) buf_printf(&b, ",\"end_line\":%d,\"end_col\":%d", eln, (int)nt_int(nt, id, "node_end_col", 0)); }
    buf_printf(&b, ",\"kind\":\"%s\"", kind);
    if (is_call) {
      const char *nm = nt_str(nt, id, "name");
      if (nm && *nm) { buf_puts(&b, ",\"name\":\""); json_escape_into(&b, nm); buf_puts(&b, "\""); }
      buf_printf(&b, ",\"dispatch\":\"%s\"", d == ND_SWITCH ? "switch" : d == ND_BOXED ? "boxed" : "direct");
      /* the def a direct call bound to, the arms of a switch (#4557): what
         go-to-definition needs, since the first DefNode of the name is wrong
         whenever two classes define it. A builtin emitted in place and a
         boxed send carry neither. */
      const char *tg = (g_ndtarget && id < g_ndtarget_cap) ? g_ndtarget[id] : NULL;
      if (tg && *tg && d == ND_SWITCH) {
        buf_puts(&b, ",\"candidates\":[");
        for (const char *p = tg; p; ) {
          const char *q = strchr(p, ',');
          if (p != tg) buf_puts(&b, ",");
          buf_puts(&b, "\"");
          { Buf one; memset(&one, 0, sizeof one);
            buf_printf(&one, "%.*s", (int)(q ? (size_t)(q - p) : strlen(p)), p);
            json_escape_into(&b, one.p ? one.p : ""); free(one.p); }
          buf_puts(&b, "\"");
          p = q ? q + 1 : NULL;
        }
        buf_puts(&b, "]");
      }
      else if (tg && *tg && d == ND_DIRECT && !strchr(tg, ',')) {
        buf_puts(&b, ",\"callee\":\""); json_escape_into(&b, tg); buf_puts(&b, "\"");
      }
    }
    else buf_printf(&b, ",\"inlined\":%s", d == ND_BLOCK_PROC ? "false" : "true");
    buf_puts(&b, "}");
    cn++;
  }
  buf_puts(&b, "\n  ]\n}\n");
  return b.p ? b.p : strdup("{\n  \"types\": [\n\n  ],\n  \"diagnostics\": [\n\n  ],\n  \"codegen\": [\n\n  ]\n}\n");
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

/* The builtin class directly above a user class that has no user superclass:
   Struct (-145) or Data (-146) for a Struct.new / Data.define class, the named
   builtin for `class E < StandardError`, Object (-116) otherwise. This is the
   row the generated sp_class_superclass table carries for the class, so the
   runtime's is_a? walk and the compile-time case arm read one answer. */
int class_builtin_superclass(Compiler *c, int i) {
  int builtin_par = c->classes[i].is_struct ? (c->classes[i].is_data ? -146 : -145)
                                            : -116;  /* Object */
  int sc_node = nt_ref(c->nt, c->classes[i].def_node, "superclass");
  if (sc_node >= 0) {
    const char *sc_ty = nt_type(c->nt, sc_node);
    const char *sc_nm = (sc_ty && (sp_streq(sc_ty, "ConstantReadNode") || sp_streq(sc_ty, "ConstantPathNode"))) ? nt_str(c->nt, sc_node, "name") : NULL;
    if (sc_nm) { int bid = builtin_class_id(sc_nm); if (bid != 0) builtin_par = bid; }
  }
  return builtin_par;
}

/* The builtin class above the whole user chain of `cid`: walk the user
   superclasses to the root and answer that root's builtin superclass. */
int class_builtin_parent(Compiler *c, int cid) {
  int k = cid;
  while (k >= 0 && c->classes[k].parent >= 0) k = c->classes[k].parent;
  return k >= 0 ? class_builtin_superclass(c, k) : -116;
}

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
      /* A class a bundled library defines, named without requiring it. CRuby
         raises NameError at run time; here the unknown constant flows into
         the inference as an untyped value and the generated C can end up
         ill-typed far from the cause, so say what is missing instead. */
      {
        static const struct { const char *cls, *feat; } PKG[] = {
          {"StringIO","stringio"}, {"CSV","csv"}, {"JSON","json"}, {"Set","set"},
          {"StringScanner","strscan"}, {"Base64","base64"}, {"Digest","digest"},
          {"ERB","erb"}, {"OptionParser","optparse"}, {"Pathname","pathname"},
          {"SecureRandom","securerandom"}, {"Tempfile","tempfile"},
          {"CGI","cgi"},
          {NULL,NULL} };
        for (int pk = 0; PKG[pk].cls; pk++) {
          if (!sp_streq(nm, PKG[pk].cls)) continue;
          if (comp_class_index(c, nm) >= 0) break;      /* the program defines it */
          if (sp_feature_required(PKG[pk].feat)) break;
          { static char rq[256];
            snprintf(rq, sizeof rq,
                     "%s is provided by the bundled %s library, which this program "
                     "does not require: add `require \"%s\"`. (CRuby's own stdlib "
                     "sometimes loads it as an implementation detail of another "
                     "library; that is not part of its interface -- see "
                     "docs/limitations.md.)", nm, PKG[pk].feat, PKG[pk].feat);
            unsupported_feature(c, i, rq); }
          break;
        }
      }
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
      /* a bare `gets` reads ARGF, which walks the ARGV files. The argument
         count is what the arm in codegen_call.c sees: an expanded `*[]`
         leaves an arguments node with none. */
      if (sp_streq(nm, "gets") && nt_ref(nt, i, "receiver") < 0 &&
          comp_bare_gets_is_argf(c)) {
        int gac = 0, gargs = nt_ref(nt, i, "arguments");
        if (gargs >= 0) nt_arr(nt, gargs, "arguments", &gac);
        if (gac == 0) g_uses_argv = 1;
      }
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
  g_gen_obj_to_json = obj_to_json_any(c);
  /* Any instantiated Struct/Data gets the symbol-keyed to_h dispatch, so a
     Struct/Data read out of a poly container answers #to_h (#2906). */
  g_gen_obj_to_h = 0;
  /* and any instantiated Struct (not a Data) the member-array dispatch, so a
     Struct read out of a poly container answers values, values_at and a
     blockless each from its members, not from a #to_a the program wrote */
  g_gen_obj_struct_values = 0;
  for (int i = 0; i < c->nclasses; i++)
    if (c->classes[i].instantiated && c->classes[i].is_struct && !c->classes[i].is_data) { g_gen_obj_struct_values = 1; break; }
  /* only where a call of one of those names has a boxed receiver: the
     dispatch is unreachable otherwise, and every Struct program would carry it */
  if (g_gen_obj_struct_values) {
    int reached = 0;
    for (int id = 0; id < c->nt->count && !reached; id++) {
      const char *nty = nt_type(c->nt, id);
      if (!nty || !sp_streq(nty, "CallNode")) continue;
      const char *nm = nt_str(c->nt, id, "name");
      int rv = nt_ref(c->nt, id, "receiver");
      if (nm && rv >= 0 && comp_ntype(c, rv) == TY_POLY &&
          (sp_streq(nm, "each") || sp_streq(nm, "each_pair") ||
           sp_streq(nm, "values") || sp_streq(nm, "values_at"))) reached = 1;
    }
    g_gen_obj_struct_values = reached;
  }
  for (int i = 0; i < c->nclasses; i++) {
    if (!c->classes[i].instantiated) continue;
    if (c->classes[i].is_struct || c->classes[i].is_data) { g_gen_obj_to_h = 1; break; }
    /* a plain class answering #deconstruct_keys needs the dispatch too */
    if (!c->classes[i].is_native_class &&
        obj_deconstruct_keys_method(c, i, NULL) >= 0) { g_gen_obj_to_h = 1; break; }
  }
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

/* ---- collect-mode unit state ----
   A collect-mode longjmp leaves the emission globals wherever the abandoned
   unit put them, and several of them point INTO that unit's frame: g_pre is
   aimed at an automatic Buf in some thirty places, g_cap_names at
   emit_proc_literal's `caps`, g_cap_struct and g_proc_return_home at its stack
   arrays. The next unit's emit_local_ref then walks a NameSet in a frame that
   no longer exists -- a crash whose site moves with the optimization level,
   which is what made it look like a bad pointer rather than corruption
   (#4141).

   Saving before the setjmp and restoring on recovery is what EMIT_COLLECT_UNIT
   already does for the buffer length and the conversion hold; these are the
   same case, and restoring the pre-unit value needs no judgement about what
   each global's "between units" value ought to be. Scalars are left alone:
   a stale int is wrong, not undefined, and the next unit assigns its own. */
typedef struct {
  Buf *pre;
  const char *yield_self_fallback;
  const char *yield_self_fallback2, *yield_self_deref_fallback2; int yield_emitting_class_fallback2;
  const char *yield_self_deref_fallback;
  const char *block_param_name;
  const char *yielder_name;
  const char *ie_next_var;
  const char *self;
  const char *self_deref;
  const char *inline_recv_expr;
  const char *dm_subst_name;
  const char *rescue_cls;
  const char *rescue_msg;
  const char *retry_label;
  const char *loop_break_var;
  const char *brk_ser_var;
  const char *block_brk_var;
  const char *yield_blk_brk_fallback;
  const char *proc_brk_home;
  const char *hoist_len_var;
  const char *hoist_len_recv;
  const char *result_var;
  const char *method_pr_label;
  const char *method_pr_var;
  const char *proc_return_home;
  const char *ctor_self;
  const char *ctor_self_deref;
  const char *fn_pr_label;
  const char *fn_pr_var;
  const char *lowered_blk_name;
  const char *yield_lowered_blk_fallback;
  const char *yield_proc_ref;
  const char *cap_struct;
  NameSet *cap_names;
  const char *iow_recv_ref;
  const char *iow_key_ref;
  /* The scalars that shape a unit's return and nesting conventions. A
     nested emitter (a fiber body, a proc body, an inlined call) saves them
     in its own frame and restores them when it returns; a longjmp out of it
     skips the restore, and the unit after the abandoned one -- main, most
     often, which sets none of them -- inherits the nested body's: main's
     `return` came out bare under a fiber body's g_c_ret_void, which -Werror
     refuses. Restored here to their value before the unit, which is the
     between-units value. */
  TyKind ret_type, fn_ret_type, result_ty;
  int c_ret_void, in_proc_body, result_poly, proc_body_kind, proc_toplevel_return;
  int indent, nren, block_nren, block_id, c_loop_depth, ensure_depth;
  int emitting_class_id, inline_recv_class, ie_class_id, dm_subst_node, exc_frame_depth;
  int open_defaults;
} EmitUnitState;

static void emit_unit_state_save(EmitUnitState *s) {
  s->ret_type = g_ret_type; s->fn_ret_type = g_fn_ret_type; s->result_ty = g_result_ty;
  s->c_ret_void = g_c_ret_void; s->in_proc_body = g_in_proc_body; s->result_poly = g_result_poly;
  s->proc_body_kind = g_proc_body_kind; s->proc_toplevel_return = g_proc_toplevel_return;
  s->indent = g_indent; s->nren = g_nren; s->block_nren = g_block_nren; s->block_id = g_block_id;
  s->c_loop_depth = g_c_loop_depth; s->ensure_depth = g_ensure_depth;
  s->emitting_class_id = g_emitting_class_id; s->inline_recv_class = g_inline_recv_class;
  s->ie_class_id = g_ie_class_id; s->dm_subst_node = g_dm_subst_node; s->exc_frame_depth = g_exc_frame_depth;
  s->open_defaults = g_open_defaults;
  s->pre = g_pre;
  s->yield_self_fallback = g_yield_self_fallback;
  s->yield_self_fallback2 = g_yield_self_fallback2; s->yield_self_deref_fallback2 = g_yield_self_deref_fallback2; s->yield_emitting_class_fallback2 = g_yield_emitting_class_fallback2;
  s->yield_self_deref_fallback = g_yield_self_deref_fallback;
  s->block_param_name = g_block_param_name;
  s->yielder_name = g_yielder_name;
  s->ie_next_var = g_ie_next_var;
  s->self = g_self;
  s->self_deref = g_self_deref;
  s->inline_recv_expr = g_inline_recv_expr;
  s->dm_subst_name = g_dm_subst_name;
  s->rescue_cls = g_rescue_cls;
  s->rescue_msg = g_rescue_msg;
  s->retry_label = g_retry_label;
  s->loop_break_var = g_loop_break_var;
  s->brk_ser_var = g_brk_ser_var;
  s->block_brk_var = g_block_brk_var;
  s->yield_blk_brk_fallback = g_yield_blk_brk_fallback;
  s->proc_brk_home = g_proc_brk_home;
  s->hoist_len_var = g_hoist_len_var;
  s->hoist_len_recv = g_hoist_len_recv;
  s->result_var = g_result_var;
  s->method_pr_label = g_method_pr_label;
  s->method_pr_var = g_method_pr_var;
  s->proc_return_home = g_proc_return_home;
  s->ctor_self = g_ctor_self;
  s->ctor_self_deref = g_ctor_self_deref;
  s->fn_pr_label = g_fn_pr_label;
  s->fn_pr_var = g_fn_pr_var;
  s->lowered_blk_name = g_lowered_blk_name;
  s->yield_lowered_blk_fallback = g_yield_lowered_blk_fallback;
  s->yield_proc_ref = g_yield_proc_ref;
  s->cap_struct = g_cap_struct;
  s->cap_names = g_cap_names;
  s->iow_recv_ref = g_iow_recv_ref;
  s->iow_key_ref = g_iow_key_ref;
}

static void emit_unit_state_restore(const EmitUnitState *s) {
  g_ret_type = s->ret_type; g_fn_ret_type = s->fn_ret_type; g_result_ty = s->result_ty;
  g_c_ret_void = s->c_ret_void; g_in_proc_body = s->in_proc_body; g_result_poly = s->result_poly;
  g_proc_body_kind = s->proc_body_kind; g_proc_toplevel_return = s->proc_toplevel_return;
  g_indent = s->indent; g_nren = s->nren; g_block_nren = s->block_nren; g_block_id = s->block_id;
  g_c_loop_depth = s->c_loop_depth; g_ensure_depth = s->ensure_depth;
  g_emitting_class_id = s->emitting_class_id; g_inline_recv_class = s->inline_recv_class;
  g_ie_class_id = s->ie_class_id; g_dm_subst_node = s->dm_subst_node; g_exc_frame_depth = s->exc_frame_depth;
  g_open_defaults = s->open_defaults;
  g_pre = s->pre;
  g_yield_self_fallback = s->yield_self_fallback;
  g_yield_self_fallback2 = s->yield_self_fallback2; g_yield_self_deref_fallback2 = s->yield_self_deref_fallback2; g_yield_emitting_class_fallback2 = s->yield_emitting_class_fallback2;
  g_yield_self_deref_fallback = s->yield_self_deref_fallback;
  g_block_param_name = s->block_param_name;
  g_yielder_name = s->yielder_name;
  g_ie_next_var = s->ie_next_var;
  g_self = s->self;
  g_self_deref = s->self_deref;
  g_inline_recv_expr = s->inline_recv_expr;
  g_dm_subst_name = s->dm_subst_name;
  g_rescue_cls = s->rescue_cls;
  g_rescue_msg = s->rescue_msg;
  g_retry_label = s->retry_label;
  g_loop_break_var = s->loop_break_var;
  g_brk_ser_var = s->brk_ser_var;
  g_block_brk_var = s->block_brk_var;
  g_yield_blk_brk_fallback = s->yield_blk_brk_fallback;
  g_proc_brk_home = s->proc_brk_home;
  g_hoist_len_var = s->hoist_len_var;
  g_hoist_len_recv = s->hoist_len_recv;
  g_result_var = s->result_var;
  g_method_pr_label = s->method_pr_label;
  g_method_pr_var = s->method_pr_var;
  g_proc_return_home = s->proc_return_home;
  g_ctor_self = s->ctor_self;
  g_ctor_self_deref = s->ctor_self_deref;
  g_fn_pr_label = s->fn_pr_label;
  g_fn_pr_var = s->fn_pr_var;
  g_lowered_blk_name = s->lowered_blk_name;
  g_yield_lowered_blk_fallback = s->yield_lowered_blk_fallback;
  g_yield_proc_ref = s->yield_proc_ref;
  g_cap_struct = s->cap_struct;
  g_cap_names = s->cap_names;
  g_iow_recv_ref = s->iow_recv_ref;
  g_iow_key_ref = s->iow_key_ref;
}

/* Emit one top-level output unit (a method, constructor, BEGIN/END block, or the
   top-level body). Each unit runs under a setjmp: an `unsupported` gap longjmps
   back here (instead of exiting), so one run surfaces every unsupported
   construct -- the gap is already printed, this unit's malformed output is
   discarded, and the driver proceeds to the next unit; the run fails once at
   the end (codegen_program), unless SP_COLLECT_ERRORS asks for the rest. `unsupported` re-sets all per-method globals
   on the next emit_method, so an abandoned unit cannot corrupt the next.
   On recovery the buffer is rolled back to its length before this unit, so the
   abandoned unit's partial output is dropped. `body` must be the heap pointer
   (not an automatic) so a longjmp doesn't leave it indeterminate (C99 7.13.2.1);
   _saved_len is set before setjmp and so stays determinate across the jump --
   as is _saved_state, which is written once before the setjmp and never after,
   and which carries the pointer-valued globals the abandoned unit may have
   aimed at its own frame (see EmitUnitState). */
#define EMIT_COLLECT_UNIT(emit_call)                          \
  do {                                                        \
    if (!collect_mode()) { emit_call; }                       \
    else {                                                    \
      size_t _saved_len = body->len;                          \
      ConvHold *_saved_hold = g_conv_hold;                    \
      EmitUnitState _saved_state;                             \
      emit_unit_state_save(&_saved_state);                    \
      if (setjmp(g_unsup_recover) == 0) {                     \
        g_unsup_armed = 1; emit_call; g_unsup_armed = 0;      \
      } \
      else {                                                \
        g_unsup_armed = 0; g_conv_hold = _saved_hold;         \
        emit_unit_state_restore(&_saved_state);               \
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

/* An element read out of a named container (`PLAN[i]`, `PLAN.first`,
   `TABLE.fetch(k)`, `@plan[i]`) is the same kind of name a block parameter
   over that container is: one of the values the container was filled with,
   which the arm set already covers when those are literals. It was refused
   as computed, with the refusal misreading `send` itself as undefined
   (#4850). */
static int node_is_container_read(const NodeTable *nt, int n) {
  if (n < 0 || nt_kind(nt, n) != NK_CallNode) return 0;
  const char *en = nt_str(nt, n, "name");
  int er = nt_ref(nt, n, "receiver");
  NodeKind rk = er >= 0 ? nt_kind(nt, er) : NK_NONE;
  return en && (sp_streq(en, "[]") || sp_streq(en, "first") || sp_streq(en, "last") ||
                sp_streq(en, "fetch") || sp_streq(en, "at") || sp_streq(en, "dig") ||
                sp_streq(en, "sample")) &&
         (rk == NK_ConstantReadNode || rk == NK_ConstantPathNode ||
          rk == NK_LocalVariableReadNode || rk == NK_InstanceVariableReadNode);
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
  if (node_is_container_read(nt, arg)) return 0;
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
    /* a local bound to such an element (`step = PLAN[i]; send(step)`) is
       that element */
    int wv = nt_ref(nt, id, "value");
    if (node_is_computed_name(nt, wv) && !node_is_container_read(nt, wv)) return 1;
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
       walk_scope assigns every node -- including those inside blocks -- the
       enclosing method's scope, and the emit loop emits a scope's body only
       when it is reachable; mirror that gate here. */
    int sc = c->nscope[id];
    if (sc < 0 || sc >= c->nscopes || !c->scopes[sc].reachable) continue;
    unsupported(c, id, "send with a runtime method name (AOT needs a compile-time-known name)");
  }
}


/* Layer 2 (ext-design.md): generate the CRuby extension shim over the
   Layer-1 emission -- the mechanization of the M0 hand shim. Conversions are
   selected from each entry's REAL C signature; a type this table does not
   cover refuses the entry at compile time, naming it. The kernel runs
   off-GVL on the calling thread, one at a time (R6); a raise crosses as
   (class name, message) through the exported try helper (R7). */
static const char *ext_rb_in(TyKind t) {
  switch (t) {
    case TY_INT:    return "spx_in_int";
    case TY_FLOAT:  return "spx_in_float";
    case TY_BOOL:   return "RTEST";
    case TY_STRING: return "spx_in_str";
    case TY_INT_ARRAY:   return "spx_in_int_array";
    case TY_FLOAT_ARRAY: return "spx_in_float_array";
    case TY_STR_ARRAY:   return "spx_in_str_array";
    default: return NULL;
  }
}
static int ext_rb_out(TyKind t, const char *expr, Buf *b) {
  switch (t) {
    case TY_INT:
      buf_printf(b, "(%s) == SP_INT_NIL ? Qnil : LL2NUM((long long)(%s))", expr, expr);
      return 1;
    case TY_FLOAT:
      buf_printf(b, "sp_float_is_nil(%s) ? Qnil : DBL2NUM(%s)", expr, expr);
      return 1;
    case TY_BOOL:
      buf_printf(b, "(%s) ? Qtrue : Qfalse", expr);
      return 1;
    case TY_STRING:
      buf_printf(b, "(%s) ? rb_utf8_str_new(%s, (long)sp_str_byte_len(%s)) : Qnil",
                 expr, expr, expr);
      return 1;
    case TY_INT_ARRAY:
      buf_printf(b, "spx_out_int_array(%s)", expr);
      return 1;
    case TY_FLOAT_ARRAY:
      buf_printf(b, "spx_out_float_array(%s)", expr);
      return 1;
    case TY_STR_ARRAY:
      buf_printf(b, "spx_out_str_array(%s)", expr);
      return 1;
    case TY_VOID:
    case TY_NIL:
      buf_printf(b, "((void)(%s), Qnil)", expr);
      return 1;
    default: return 0;
  }
}

/* R4 (ext-design.md): a mutation of an exported entry's parameter cannot
   reach the caller -- values cross the boundary by copy -- so the divergence
   from CRuby would be silent, which is the one thing an extension must never
   do. Refuse it at compile time, naming the method and the parameter. The
   check is receiver-syntactic (a direct mutator call on the parameter, or an
   index write through it); a mutation through an alias or a callee is not
   caught -- the boundary types keep the surface small, and the error text
   says copy semantics out loud either way. */
static int ext_name_mutates(TyKind t, const char *nm) {
  if (!nm || !*nm) return 0;
  size_t l = strlen(nm);
  if (nm[l - 1] == '!') return 1;
  if (sp_streq(nm, "[]=")) return 1;
  if (ty_is_array(t)) {
    static const char *const A[] = { "push", "<<", "append", "pop", "shift",
      "unshift", "prepend", "insert", "concat", "clear", "delete", "delete_at",
      "delete_if", "keep_if", "fill", "replace", NULL };
    for (int i = 0; A[i]; i++) if (sp_streq(nm, A[i])) return 1;
  }
  else if (t == TY_STRING || t == TY_STRBUF) {
    static const char *const S[] = { "<<", "concat", "insert", "replace",
      "clear", "prepend", "setbyte", NULL };
    for (int i = 0; S[i]; i++) if (sp_streq(nm, S[i])) return 1;
  }
  else if (ty_is_hash(t)) {
    static const char *const H[] = { "store", "delete", "delete_if", "keep_if",
      "clear", "replace", "update", NULL };
    for (int i = 0; H[i]; i++) if (sp_streq(nm, H[i])) return 1;
  }
  return 0;
}
static void ext_refuse_param_mutation(Compiler *c) {
  const NodeTable *nt = c->nt;
  for (int id = 0; id < nt->count; id++) {
    int si = c->nscope[id];
    if (si < 0 || si >= c->nscopes || !c->scopes[si].is_ext_entry) continue;
    NodeKind k = nt_kind(nt, id);
    const char *mnm = NULL;
    int recv = -1;
    if (k == NK_CallNode) { mnm = nt_str(nt, id, "name"); recv = nt_ref(nt, id, "receiver"); }
    else if (k == NK_IndexOperatorWriteNode || k == NK_IndexAndWriteNode ||
             k == NK_IndexOrWriteNode) { mnm = "[]="; recv = nt_ref(nt, id, "receiver"); }
    else continue;
    if (recv < 0 || nt_kind(nt, recv) != NK_LocalVariableReadNode) continue;
    const char *pn = nt_str(nt, recv, "name");
    Scope *sc = &c->scopes[si];
    LocalVar *lv = pn ? scope_local(sc, pn) : NULL;
    if (!lv || !lv->is_param) continue;
    if (!ext_name_mutates(lv->type, mnm)) continue;
    fprintf(stderr,
            "spinel: --ext: %s.%s mutates its parameter `%s` (%s): values cross "
            "the extension boundary by COPY, so the caller's object would not "
            "see it and the divergence from CRuby would be silent. Return the "
            "result instead, or work on a local copy (see "
            "docs/internals/ext-design.md, R4)\n",
            sc->class_id >= 0 ? c->classes[sc->class_id].name : "?",
            sc->name ? sc->name : "?", pn, mnm);
    exit(1);
  }
}

static void ext_generate_cruby_shim(Compiler *c) {
  Buf sb; memset(&sb, 0, sizeof sb);
  const char *feat = g_ext_feature ? g_ext_feature : "spinel_ext";
  buf_printf(&sb,
    "/* Generated by Spinel (--ext cruby). CRuby extension shim over the\n"
    "   Layer-1 library: conversions under the GVL, the kernel off it, one\n"
    "   call at a time; a kernel raise re-raises here by class name. */\n"
    "#include <ruby.h>\n#include <ruby/encoding.h>\n#include <ruby/thread.h>\n"
    "#include <pthread.h>\n#include \"%s.h\"\n\n"
    "static pthread_mutex_t spx_lock = PTHREAD_MUTEX_INITIALIZER;\n"
    "static const char *spx_exc_cls, *spx_exc_msg;   /* written under spx_lock */\n"
    "static void spx_reraise(const char *cls, const char *msg) {\n"
    "  VALUE k = rb_eRuntimeError;\n"
    "  if (cls && *cls) {\n"
    "    VALUE found = rb_funcall(rb_cObject, rb_intern(\"const_get\"), 1, rb_str_new_cstr(cls));\n"
    "    if (RTEST(rb_obj_is_kind_of(found, rb_cClass))) k = found;\n"
    "  }\n"
    "  rb_raise(k, \"%%s\", msg ? msg : \"\");\n}\n\n"
    "static sp_int spx_in_int(VALUE v) {\n"
    "  if (!FIXNUM_P(v)) {\n"
    "    if (RB_TYPE_P(v, T_BIGNUM))\n"
    "      rb_raise(rb_eRangeError, \"integer too big for the compiled kernel (64-bit)\");\n"
    "    rb_raise(rb_eTypeError, \"no implicit conversion of %%s into Integer\", rb_obj_classname(v));\n"
    "  }\n  return (sp_int)NUM2LL(v);\n}\n"
    "static double spx_in_float(VALUE v) { return NUM2DBL(v); }\n"
    "static const char *spx_in_str(VALUE v) {\n"
    "  if (!RB_TYPE_P(v, T_STRING))\n"
    "    rb_raise(rb_eTypeError, \"no implicit conversion of %%s into String\", rb_obj_classname(v));\n"
    "  { int ei = rb_enc_get_index(v);\n"
    "    if (ei != rb_utf8_encindex() && ei != rb_usascii_encindex() && ei != rb_ascii8bit_encindex())\n"
    "      rb_raise(rb_eArgError, \"string encoding %%s cannot cross into the compiled kernel (UTF-8 or binary only)\", rb_enc_name(rb_enc_get(v))); }\n"
    "  return sp_str_from_bytes(RSTRING_PTR(v), (size_t)RSTRING_LEN(v));\n}\n"
    "static sp_IntArray *spx_in_int_array(VALUE v) {\n"
    "  if (!RB_TYPE_P(v, T_ARRAY)) rb_raise(rb_eTypeError, \"no implicit conversion of %%s into Array\", rb_obj_classname(v));\n"
    "  { long n = RARRAY_LEN(v); sp_IntArray *a = sp_IntArray_new(); SP_GC_ROOT(a);\n"
    "    for (long i = 0; i < n; i++) { VALUE e = RARRAY_AREF(v, i);\n"
    "      if (!FIXNUM_P(e)) rb_raise(rb_eTypeError, \"array element %%ld is not an Integer\", i);\n"
    "      sp_IntArray_push(a, (sp_int)NUM2LL(e)); }\n"
    "    return a; }\n}\n"
    "static sp_FloatArray *spx_in_float_array(VALUE v) {\n"
    "  if (!RB_TYPE_P(v, T_ARRAY)) rb_raise(rb_eTypeError, \"no implicit conversion of %%s into Array\", rb_obj_classname(v));\n"
    "  { long n = RARRAY_LEN(v); sp_FloatArray *a = sp_FloatArray_new(); SP_GC_ROOT(a);\n"
    "    for (long i = 0; i < n; i++) sp_FloatArray_push(a, NUM2DBL(RARRAY_AREF(v, i)));\n"
    "    return a; }\n}\n"
    "static sp_StrArray *spx_in_str_array(VALUE v) {\n"
    "  if (!RB_TYPE_P(v, T_ARRAY)) rb_raise(rb_eTypeError, \"no implicit conversion of %%s into Array\", rb_obj_classname(v));\n"
    "  { long n = RARRAY_LEN(v); sp_StrArray *a = sp_StrArray_new(); SP_GC_ROOT(a);\n"
    "    for (long i = 0; i < n; i++) { VALUE e = RARRAY_AREF(v, i);\n"
    "      if (!RB_TYPE_P(e, T_STRING)) rb_raise(rb_eTypeError, \"array element %%ld is not a String\", i);\n"
    "      sp_StrArray_push(a, sp_str_from_bytes(RSTRING_PTR(e), (size_t)RSTRING_LEN(e))); }\n"
    "    return a; }\n}\n"
    "static VALUE spx_out_int_array(sp_IntArray *a) {\n"
    "  if (!a) return Qnil;\n"
    "  { long n = (long)sp_IntArray_length(a); VALUE r = rb_ary_new_capa(n);\n"
    "    for (long i = 0; i < n; i++) rb_ary_push(r, LL2NUM((long long)sp_IntArray_get(a, i)));\n"
    "    return r; }\n}\n"
    "static VALUE spx_out_float_array(sp_FloatArray *a) {\n"
    "  if (!a) return Qnil;\n"
    "  { long n = (long)a->len; VALUE r = rb_ary_new_capa(n);\n"
    "    for (long i = 0; i < n; i++) rb_ary_push(r, DBL2NUM(sp_FloatArray_get(a, i)));\n"
    "    return r; }\n}\n"
    "static VALUE spx_out_str_array(sp_StrArray *a) {\n"
    "  if (!a) return Qnil;\n"
    "  { long n = (long)sp_StrArray_length(a); VALUE r = rb_ary_new_capa(n);\n"
    "    for (long i = 0; i < n; i++) { const char *e = sp_StrArray_get(a, i);\n"
    "      rb_ary_push(r, e ? rb_utf8_str_new(e, (long)sp_str_byte_len(e)) : Qnil); }\n"
    "    return r; }\n}\n\n",
    feat);
  int emitted = 0;
  for (int s9 = 1; s9 < c->nscopes; s9++) {
    Scope *sc = &c->scopes[s9];
    if (!sc->is_ext_entry) continue;
    const char *cn = sc->class_id >= 0 ? c->classes[sc->class_id].c_name : NULL;
    if (!cn) continue;
    /* v1 boundary: fixed positional arity, no block */
    if (sc->rest_idx >= 0 || sc->kwrest_idx >= 0 || sc->nrequired != sc->nparams ||
        (sc->blk_param && sc->blk_param[0]) || sc->yields) {
      fprintf(stderr, "spinel: --ext cruby: %s.%s: only fixed positional "
                      "parameters cross the extension boundary in v1\n",
              c->classes[sc->class_id].name, sc->name);
      exit(1);
    }
    if (cmethod_takes_self_cls(c, s9)) {
      fprintf(stderr, "spinel: --ext cruby: %s.%s: a subclass-dispatched class "
                      "method cannot be an entry\n",
              c->classes[sc->class_id].name, sc->name);
      exit(1);
    }
    for (int p9 = 0; p9 < sc->nparams; p9++) {
      LocalVar *lv = scope_local(sc, sc->pnames[p9]);
      if (!lv || !ext_rb_in(lv->type)) {
        fprintf(stderr, "spinel: --ext cruby: %s.%s: parameter `%s` has type "
                        "%s, which cannot cross the extension boundary "
                        "(Integer/Float/bool/String and their typed arrays "
                        "cross in v1)\n",
                c->classes[sc->class_id].name, sc->name, sc->pnames[p9],
                lv ? ty_name(lv->type) : "?");
        exit(1);
      }
    }
    { Buf probe; memset(&probe, 0, sizeof probe);
      if (!ext_rb_out(sc->ret, "x", &probe)) {
        fprintf(stderr, "spinel: --ext cruby: %s.%s: return type %s cannot "
                        "cross the extension boundary in v1\n",
                c->classes[sc->class_id].name, sc->name, ty_name(sc->ret));
        exit(1);
      }
      free(probe.p); }
    /* the call struct + off-GVL body */
    buf_printf(&sb, "typedef struct {");
    for (int p9 = 0; p9 < sc->nparams; p9++) {
      LocalVar *lv = scope_local(sc, sc->pnames[p9]);
      buf_printf(&sb, " %s a%d;", c_type_name(lv->type), p9);
    }
    if (sc->ret != TY_VOID && sc->ret != TY_NIL)
      buf_printf(&sb, " %s ret;", c_type_name(sc->ret));
    buf_printf(&sb, " } spx_c_%d;\n", s9);
    buf_printf(&sb, "static void spx_body_%d(void *p) { spx_c_%d *c = (spx_c_%d *)p; ",
               s9, s9, s9);
    if (sc->ret != TY_VOID && sc->ret != TY_NIL) buf_puts(&sb, "c->ret = ");
    buf_printf(&sb, "sp_%s_s_%s(", cn, mc(sc->name));
    for (int p9 = 0; p9 < sc->nparams; p9++)
      buf_printf(&sb, "%sc->a%d", p9 ? ", " : "", p9);
    buf_puts(&sb, "); }\n");
    /* the GVL-holding wrapper */
    buf_printf(&sb, "static void *spx_run_%d(void *p) { return (void *)(intptr_t)"
               "%s_try(spx_body_%d, p, &spx_exc_cls, &spx_exc_msg); }\n",
               s9, g_ext_init_name, s9);
    buf_printf(&sb, "static VALUE spx_m_%d(VALUE self", s9);
    for (int p9 = 0; p9 < sc->nparams; p9++) buf_printf(&sb, ", VALUE v%d", p9);
    buf_printf(&sb, ") {\n  spx_c_%d c__; memset(&c__, 0, sizeof c__);\n", s9);
    buf_puts(&sb, "  SP_GC_SAVE();\n");
    for (int p9 = 0; p9 < sc->nparams; p9++) {
      LocalVar *lv = scope_local(sc, sc->pnames[p9]);
      buf_printf(&sb, "  c__.a%d = %s(v%d);", p9, ext_rb_in(lv->type), p9);
      if (lv->type == TY_STRING) buf_printf(&sb, " SP_GC_ROOT_STR(c__.a%d);", p9);
      buf_puts(&sb, "\n");
    }
    buf_puts(&sb, "  { int raised; const char *ec = 0, *em = 0;\n"
                  "    pthread_mutex_lock(&spx_lock);\n");
    buf_printf(&sb, "    raised = (int)(intptr_t)rb_thread_call_without_gvl(spx_run_%d, &c__, RUBY_UBF_IO, NULL);\n", s9);
    buf_puts(&sb, "    if (raised) { ec = spx_exc_cls; em = spx_exc_msg; }\n"
                  "    pthread_mutex_unlock(&spx_lock);\n"
                  "    if (raised) spx_reraise(ec, em);\n  }\n");
    buf_puts(&sb, "  return ");
    { char rexpr[32]; snprintf(rexpr, sizeof rexpr, "c__.ret"); 
      if (sc->ret == TY_VOID || sc->ret == TY_NIL) buf_puts(&sb, "Qnil");
      else ext_rb_out(sc->ret, rexpr, &sb); }
    buf_puts(&sb, ";\n}\n\n");
    emitted++;
  }
  (void)emitted;
  /* Init: modules + module functions */
  { char featfn[256]; size_t fi = 0;
    for (const char *p = feat; *p && fi < sizeof featfn - 1; p++)
      featfn[fi++] = (*p == '-' || *p == '.') ? '_' : *p;
    featfn[fi] = 0;
    buf_printf(&sb, "void Init_%s(void) {\n  %s();\n", featfn, g_ext_init_name);
    for (int s9 = 1; s9 < c->nscopes; s9++) {
      Scope *sc = &c->scopes[s9];
      if (!sc->is_ext_entry || sc->class_id < 0) continue;
      buf_printf(&sb, "  { VALUE m = rb_define_module(\"%s\");\n",
                 c->classes[sc->class_id].name);
      buf_printf(&sb, "    rb_define_module_function(m, \"%s\", spx_m_%d, %d); }\n",
                 sc->name, s9, sc->nparams);
    }
    buf_puts(&sb, "}\n"); }
  free(g_ext_shim_text);
  g_ext_shim_text = sb.p;
}


char *codegen_program(const NodeTable *nt) {
  Compiler *c = comp_new(nt);
  analyze_program(c);
  /* From here on a yield reads the type of the block spliced at THIS site,
     not the union the node cache holds across sites (#3784). Installed after
     analysis so the fixpoint keeps seeing the cache unchanged. */
  sp_yield_site_type_hook = sp_yield_site_type;

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
  /* --profile: write the symbol map next to the binary, then continue to
     the real build (offline symbolization input for perf tooling, #1336). */
  const char *psym_out = getenv("SPINEL_PROFILE_SYMBOL_MAP");
  if (psym_out && *psym_out) {
    char *json = build_symbol_map_json(c);
    emit_write_file(psym_out, json);
    free(json);
  }
  /* --warn-widen, once, before the analysis-only modes return: the slots
     are settled here (the RBS below reads them), and emission below changes
     none of them (checked over the corpus). */
  { const char *ww = getenv("SPINEL_WARN_WIDEN");
    if (ww && *ww) each_widened_slot(c, warn_widened_slot, NULL); }
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
  /* --emit-types is written after the emission below has run: a refusal is
     a codegen verdict, and the JSON that answered "no diagnostics" for a
     program the compile refuses was worth less than no JSON (#4509). The C
     is discarded. */
  const char *types_out = getenv("SPINEL_EMIT_TYPES");
  if (types_out && !*types_out) types_out = NULL;

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
  /* ext mode: the program-hook family (sp_sym_to_s, sp_class_to_s, ...) gets
     external linkage so a host TU including the emitted header resolves to
     THIS TU's definitions; the macro flips the header's prototypes off
     `static` before the include (ext-design.md, Layer 1). */
  if (g_ext_init_name) buf_puts(&b, "#define SPINEL_EXT_KERNEL 1\n");
  /* No poly-renderable value anywhere: skip the sp_poly_inspect hook install
     in the header (it would force sp_sym_to_s / sp_class_to_s definitions
     this TU deliberately omits). */
  if (!g_emit_sym_rt)
    buf_puts(&b, "#define SP_TU_NO_POLY_RENDER 1\n");
  buf_puts(&b, "#include \"spinel_rt.h\"\n");
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
    int any_binstr = 0, any_extern = 0;
    for (int fi = 0; fi < cf->n_ffi_funcs; fi++) {
      const char *ret = cf->ffi_funcs[fi].ret;
      if (sp_streq(ret, "binstr")) any_binstr = 1;
      /* A function taking a callback (qsort, bsearch, lfind) is declared like
         any other: the private name cannot conflict with a header's own
         declaration, and one no included header declares (lfind lives in
         <search.h>) was otherwise called with no prototype at all -- an
         implicit int, truncating a :ptr result. The callback parameter takes
         the trampoline's own pointer type (ffi_cb_arg_ctype). */
      int na = cf->ffi_funcs[fi].nargs;
      /* Declared under a private name bound to the symbol by an asm label
         (ffi_extern_name). __USER_LABEL_PREFIX__ is the target's symbol prefix
         (`_` on Mach-O, empty on ELF and wasm). The label names the raw
         symbol, not whatever a header redirects the name to (fopen64,
         __isoc99_sscanf, a fortify __*_chk): the spec describes the raw
         symbol's ABI, the one dlsym finds for the ffi gem.
         A variadic function (trailing :varargs) is declared the same way, with
         its fixed args and `...`: the private name cannot conflict with a
         header's fortified declaration (printf under gcc + glibc
         _FORTIFY_SOURCE), and calling it is no call through an incompatible
         function type (gcc warns casting fprintf's FILE * to void *). With no
         fixed arg there is no prototype to write (`(...)` needs C23), so the
         call site casts the header-declared symbol instead. */
      int is_va = na > 0 && sp_streq(cf->ffi_funcs[fi].args[na - 1], "varargs");
      int fixed = is_va ? na - 1 : na;
      if (is_va && fixed == 0) continue;
      if (!any_extern) {
        buf_puts(&b, "#define SP_FFI_STR_(x) #x\n#define SP_FFI_STR(x) SP_FFI_STR_(x)\n"
                     "#ifdef __USER_LABEL_PREFIX__\n"
                     "#define SP_FFI_SYM(s) SP_FFI_STR(__USER_LABEL_PREFIX__) s\n"
                     "#else\n#define SP_FFI_SYM(s) s\n#endif\n");
        any_extern = 1;
      }
      buf_puts(&b, "extern ");
      buf_puts(&b, ffi_c_type(ret));
      buf_puts(&b, " ");
      ffi_extern_name(cf, fi, &b);
      buf_puts(&b, "(");
      for (int ai = 0; ai < fixed; ai++) {
        if (ai) buf_puts(&b, ", ");
        int cbi = ffi_find_callback(cf, cf->ffi_funcs[fi].mod, cf->ffi_funcs[fi].args[ai]);
        if (cbi < 0) { buf_puts(&b, ffi_c_type(cf->ffi_funcs[fi].args[ai])); continue; }
        FfiCallback *k = &cf->ffi_callbacks[cbi];
        buf_printf(&b, "%s (*)(", ffi_c_type(k->ret_spec));
        for (int ki = 0; ki < k->nargs; ki++)
          buf_printf(&b, "%s%s", ki ? ", " : "", ffi_cb_arg_ctype(k->arg_specs[ki]));
        buf_puts(&b, k->nargs ? ")" : "void)");
      }
      if (is_va) buf_puts(&b, ", ...");
      if (na == 0) buf_puts(&b, "void");
      buf_printf(&b, ") __asm__(SP_FFI_SYM(\"%s\"));\n",
                 cf->ffi_funcs[fi].csym ? cf->ffi_funcs[fi].csym : cf->ffi_funcs[fi].name);
    }
    /* Byte count for the :binstr return mode (defined in sp_alloc.c). */
    /* sp_alloc.h already declares it, and declares it SP_TLS in the threaded
       build -- re-declaring it here without the storage class is a conflict,
       so name it the same way. */
    if (any_binstr) buf_puts(&b, "extern SP_TLS int sp_ffi_bin_len;\n");

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
       holds only pointers) so the method externs below can name it. */
    for (int nci = 0; nci < cf->nclasses; nci++)
      if (cf->classes[nci].is_native_class && cf->classes[nci].c_struct) {
        buf_printf(&b, "typedef struct %s_s %s;\n", cf->classes[nci].c_struct, cf->classes[nci].c_struct);
        /* A native_struct's C name need not be sp_<class> (IO::Buffer is
           class "Buffer" over sp_IOBuffer), but the self-parameter and cast
           emitters spell instances sp_<c_name>; alias that spelling to the
           declared struct so both name the same type. */
        char sp_name[160];
        snprintf(sp_name, sizeof sp_name, "sp_%s", cf->classes[nci].c_name ? cf->classes[nci].c_name : "");
        if (!sp_streq(sp_name, cf->classes[nci].c_struct))
          buf_printf(&b, "typedef %s %s;\n", cf->classes[nci].c_struct, sp_name);
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
      if (m->kind == 1) buf_printf(&b, "%s *%s(sp_int", cstruct, m->csym);   /* ctor: cls_id first */
      else if (sp_streq(m->ret, "self")) buf_printf(&b, "%s *%s(%s *", cstruct, m->csym, cstruct);
      else { buf_printf(&b, "%s %s(%s *", native_c_type(m->ret), m->csym, cstruct); }
      for (int ai = 0; ai < m->nargs; ai++) { buf_puts(&b, ", "); buf_puts(&b, native_c_type(m->args[ai])); }
      buf_puts(&b, ");\n");
    }
    /* IO::Buffer as an ffi_func pointer argument (codegen_call.c) */
    if (cf->n_ffi_funcs > 0 && ffi_iobuffer_class(cf) >= 0) {
      buf_puts(&b, "extern void *sp_IOBuffer_ffi_base(sp_IOBuffer *, sp_int);\n"
                   "extern void *sp_IOBuffer_ffi_ptr(sp_RbVal, sp_int, sp_int);\n"
                   "extern sp_int sp_IOBuffer_ffi_hold(sp_IOBuffer *);\n"
                   "extern void sp_IOBuffer_ffi_release(sp_IOBuffer *, sp_int);\n"
                   "extern sp_int sp_IOBuffer_ffi_hold_v(sp_RbVal, sp_int);\n"
                   "extern void sp_IOBuffer_ffi_release_v(sp_RbVal, sp_int, sp_int);\n");
      /* which class ids are user classes, whose instances have no C address:
         a boxed pointer argument holding one is refused at run time */
      buf_printf(&b, "static const unsigned char sp_ffi_user_cls[%d] __attribute__((unused)) = {", cf->nclasses > 0 ? cf->nclasses : 1);
      for (int k = 0; k < cf->nclasses; k++)
        buf_printf(&b, "%s%d", k ? "," : "", (!cf->classes[k].is_native_class && !is_builtin_reopen(cf->classes[k].name)) ? 1 : 0);
      if (cf->nclasses == 0) buf_puts(&b, "0");
      buf_puts(&b, "};\n");
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
    /* Inline C fragments are deliberately emitted after Spinel's generated FFI
       declarations and storage, but before any generated function bodies. This
       lets a single Ruby source carry a small adapter while normal ffi_func
       declarations retain their usual type checking and call lowering. */
    for (int si = 0; si < cf->n_ffi_sources; si++) {
      buf_printf(&b, "\n/* ffi_source: %s */\n", cf->ffi_sources[si].mod);
      buf_puts(&b, cf->ffi_sources[si].val);
      if (cf->ffi_sources[si].val[0] &&
          cf->ffi_sources[si].val[strlen(cf->ffi_sources[si].val) - 1] != '\n')
        buf_puts(&b, "\n");
      buf_puts(&b, "/* end ffi_source */\n");
    }
  }
  if (g_emit_sym_rt) {
    int ns = c->nsymbols;
    if (ns > 0) {
      /* A name holding a NUL cannot use either inline literal form: both are
         GNU statement expressions, and this is a STATIC initializer, which
         needs constant expressions. A file-scope object with a real sp_str_hdr
         is one -- `_sym_N.d` is an address constant -- so such a name gets its
         own struct beside the table and the table points at it. Ordinary
         names keep the compact marked-literal form and cost nothing extra.
         sp_str_byte_len then reads the header for the 0xf1 entries and falls
         back to strlen for the 0xff ones, which is right for both. */
      for (int i = 0; i < ns; i++) {
        size_t sl = c->symbol_lens ? c->symbol_lens[i] : strlen(c->symbols[i]);
        if (sl <= strlen(c->symbols[i])) continue;
        buf_printf(&b, "static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _sym_%d = "
                       "{ { NULL, %zu, %zu, 0 }, 0xf1, \"", sl + 1, i, sl + 1, sl);
        emit_c_escaped_n(&b, c->symbols[i], sl);
        buf_puts(&b, "\" };\n");
      }
      buf_printf(&b, "static const char *const sp_sym_names[%d] = {", ns);
      for (int i = 0; i < ns; i++) {
        if (i) buf_puts(&b, ", ");
        size_t sl = c->symbol_lens ? c->symbol_lens[i] : strlen(c->symbols[i]);
        if (sl > strlen(c->symbols[i])) buf_printf(&b, "_sym_%d.d", i);
        else emit_str_literal(&b, c->symbols[i]);
      }
      buf_puts(&b, "};\n");
    }
    /* dynamic intern pool: symbols minted at runtime (Symbol#upcase,
       :"interp", String#to_sym) get ids >= the static count. */
    buf_puts(&b, "static const char *sp_dyn_syms[SP_DYN_SYMS_MAX]; static int sp_ndyn = 0;\n");
    /* Those entries are string-heap strings (sp_str_dup_external) held only by
       this static array, which the collector does not walk: the string sweep
       freed them and the next intern compared against a corpse. Emitted here,
       right after the array, so the declaration is always in scope. */
    buf_puts(&b, "static void sp_mark_dyn_syms(void){for(int _i=0;_i<sp_ndyn;_i++)sp_mark_string(sp_dyn_syms[_i]);}\n");
    g_has_dyn_syms = 1;
    /* Every arm must hand back a MARKED string. sp_sym_names[] entries
       carry the 0xff rodata marker, but a bare "" literal does not, and
       callers root the result (`const char *t = sp_sym_to_s(x);
       SP_GC_ROOT(t);`). sp_gc_mark then reads the arbitrary rodata byte
       before the literal, fails to recognise a marker, treats it as a
       heap object and writes its mark word. A nil Symbol lands on the
       out-of-range arm (id -1), so this was reachable from ordinary
       Ruby. sp_str_empty is the marked empty string. */
    buf_printf(&b, "%s", g_ext_init_name ? "" : "static ");
    buf_printf(&b, "const char *sp_sym_to_s(sp_sym id){"
                   "if(id>=0&&id<%d)return %s;"
                   "if(id>=%d&&id<%d+sp_ndyn)return sp_dyn_syms[id-%d];"
                   "return sp_str_empty;}\n",
                   ns, ns > 0 ? "sp_sym_names[id]" : "sp_str_empty", ns, ns, ns);
    /* Byte-exact interning: a name may hold a NUL, which strcmp cannot see
       past. The stored entries carry their length (a 0xf1 struct entry in its
       header, a 0xff literal through strlen, a dyn entry through its heap
       header), so sp_str_byte_len answers for all three.

       sp_sym_intern keeps strlen semantics for its argument: generated code
       calls it with BARE C literals, which have no marker byte in front, and
       asking sp_str_byte_len for one reads past the object. A caller that HAS
       a spinel string -- String#to_sym -- calls the _n form with the real
       length. The first-byte test keeps strcmp's early exit: without it every
       candidate paid a full length walk before the compare could fail. */
    buf_printf(&b, "%s", g_ext_init_name ? "" : "static ");
    buf_printf(&b, "sp_sym sp_sym_intern_n(const char *s, size_t n){"
                   "for(int i=0;i<%d;i++){const char*_c=%s;if(_c[0]==s[0]&&sp_str_byte_len(_c)==n&&memcmp(_c,s,n)==0)return (sp_sym)i;}"
                   "for(int i=0;i<sp_ndyn;i++){const char*_c=sp_dyn_syms[i];if(_c[0]==s[0]&&sp_str_byte_len(_c)==n&&memcmp(_c,s,n)==0)return (sp_sym)(%d+i);}"
                   "if(sp_ndyn<SP_DYN_SYMS_MAX){sp_dyn_syms[sp_ndyn]=sp_str_from_bytes(s,n);return (sp_sym)(%d+sp_ndyn++);}"
                   "return (sp_sym)0;}\n", ns, ns > 0 ? "sp_sym_names[i]" : "sp_str_empty", ns, ns);
    buf_printf(&b, "%ssp_sym sp_sym_intern(const char *s){return sp_sym_intern_n(s,s?strlen(s):0);}\n\n",
               g_ext_init_name ? "" : "static ");
  }
  /* sp_class_to_s serves the runtime's SP_TAG_CLASS render arms (sp_poly_puts
     / sp_poly_to_s / sp_poly_inspect). Emitted whenever anything in the
     program could reach those (user classes, class values, any poly-capable
     slot -- see the render-reach scan); a purely-scalar program skips it. */
  if (g_emit_class_names) {
    buf_printf(&b, "%s", g_ext_init_name ? "" : "static ");
    buf_puts(&b, "const char *sp_class_to_s(sp_Class c){if(sp_class_nil_p(c))return SPL(\"nil\");if(c.name)return c.name;switch(c.cls_id){");
    for (int i = 0; i < c->nclasses; i++) {
      if (!is_builtin_reopen(c->classes[i].name)) {
        /* An anonymous Struct/Data class has no Ruby-visible name -- the
           StructAnon_<n> the compiler keys it by is not one -- and CRuby
           renders it as the address form. #name already answers nil; this is
           the same class seen through #to_s / #inspect / `p` (#4031). */
        if (c->classes[i].is_anon_struct) {
          buf_printf(&b, "case %d:return sp_sprintf(SPL(\"#<Class:0x%%016llx>\"),"
                         "(unsigned long long)(uintptr_t)&sp_class_to_s+%d);", i, i);
          continue;
        }
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
    buf_puts(&b, "case -163:return SPL(\"Process::Status\");case -164:return SPL(\"Process::Tms\");");
    buf_puts(&b, "case -165:return SPL(\"Dir\");");
    buf_puts(&b, "case -166:return SPL(\"BasicSocket\");case -167:return SPL(\"IPSocket\");");
    buf_puts(&b, "case -168:return SPL(\"TCPSocket\");case -169:return SPL(\"TCPServer\");");
    buf_puts(&b, "case -170:return SPL(\"UDPSocket\");case -171:return SPL(\"UNIXSocket\");");
    buf_puts(&b, "case -172:return SPL(\"UNIXServer\");case -173:return SPL(\"Socket\");");
    /* The concurrency classes name themselves the way CRuby does: the
       top-level constant is an alias, #name answers the qualified form. */
    buf_puts(&b, "case -174:return SPL(\"Thread\");case -175:return SPL(\"Thread::Mutex\");");
    buf_puts(&b, "case -176:return SPL(\"Thread::Queue\");case -177:return SPL(\"Thread::SizedQueue\");");
    buf_puts(&b, "case -178:return SPL(\"Thread::ConditionVariable\");case -179:return SPL(\"Fiber\");");
    buf_puts(&b, "case -180:return SPL(\"MatchData\");");
    buf_puts(&b, "default:return sp_str_empty;} }\n\n");
    /* CRuby INSPECTS a keyword-init Struct class as `K(keyword_init: true)`
       while its name and to_s stay the bare name, so the render arms need a
       second table rather than a suffixed sp_class_to_s (#3947). Emitted
       alongside it, and identical to it when no such class exists. */
    buf_puts(&b, "static const char *sp_class_inspect_name(sp_Class c){switch(c.cls_id){");
    for (int i = 0; i < c->nclasses; i++) {
      if (is_builtin_reopen(c->classes[i].name)) continue;
      if (!c->classes[i].is_struct || c->classes[i].kw_init != 1) continue;
      const char *qname = class_ruby_name(c, i);
      if (!qname) qname = c->classes[i].name;
      buf_printf(&b, "case %d:return SPL(\"%s(keyword_init: true)\");", i, qname);
    }
    buf_puts(&b, "default:break;} return sp_class_to_s(c); }\n\n");
    /* #name of an ANONYMOUS class is nil, where #to_s and #inspect are the
       address form sp_class_to_s renders. The static spelling
       (`Struct.new(:a).name`) has always answered nil; this is the same class
       reached through a value (`obj.class.name`) (#4031). */
    buf_puts(&b, "static const char *sp_class_name_or_nil(sp_Class c){switch(c.cls_id){");
    for (int i = 0; i < c->nclasses; i++)
      if (!is_builtin_reopen(c->classes[i].name) && c->classes[i].is_anon_struct)
        buf_printf(&b, "case %d:return NULL;", i);
    buf_puts(&b, "default:break;} return sp_class_to_s(c); }\n\n");
    /* Inverse of the table above, for resolving a class carried by NAME back to
       its builtin id so the id-keyed hierarchy walks work on it (#3022). Cold
       path only (superclass/ancestors), so a linear scan is fine. */
    buf_puts(&b, "static sp_int sp_builtin_id_of_name(const char *n){\n");
    buf_puts(&b, "  if(!n||!n[0])return SP_CLASS_NIL_ID;\n");
    buf_puts(&b, "  for(sp_int i=-100;i>=-179;i--){const char*s=sp_class_to_s((sp_Class){i,NULL});"
                 "if(s&&s[0]&&!strcmp(s,n))return i;}\n");
    buf_puts(&b, "  return SP_CLASS_NIL_ID;\n}\n\n");
  }
  /* Threaded-runtime marker: the driver greps for this and links the
     -DSP_THREADS runtime variant (libspinel_rt_mt.a) plus -lpthread instead of
     the byte-identical single-threaded archive. Emitted only when the program
     references Thread/Mutex/Queue/... -- so it must follow the feature scan. */
  if (g_uses_threads) buf_puts(&b, "/* SPINEL_USES_THREADS */\n");
  /* Ask the C compiler for a brake the frame estimate cannot provide: on a
     program that runs user code on a fiber stack, any frame past that stack is
     a guard-page crash waiting for the right input, so it is worth a warning
     rather than a silent SIGSEGV (#3913). */
  if (fi_fiber_stack_risk(c)) buf_puts(&b, "/* SPINEL_FIBER_FRAME_GUARD */\n");
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
    /* A rescued exception's #class carries its name with cls_id 0, which would
       otherwise read as user class 0; resolve those by name first (#3031). */
    buf_puts(&b, "  if(c.name){const char*_p=sp_exc_parent_of_name(c.name);"
                 "if(_p){sp_int _id=sp_builtin_id_of_name(_p);"
                 "return _id!=SP_CLASS_NIL_ID?((sp_Class){_id,NULL}):((sp_Class){-1,_p});}}\n");
    buf_puts(&b, "  switch(c.cls_id){\n");
    for (int i = 0; i < c->nclasses; i++) {
      if (is_builtin_reopen(c->classes[i].name)) continue;
      int par = c->classes[i].parent;
      if (par >= 0) {
        buf_printf(&b, "  case %d: return ((sp_Class){%d});\n", i, par);
      }
      else {
        buf_printf(&b, "  case %d: return ((sp_Class){%d});\n", i, class_builtin_superclass(c, i));
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
    /* The same walk up from a user class, matching an ANCESTOR by name: the
       runtime's poly is_a? and class-arm helper hold the arm's class as the
       name it was written with. The class itself was compared before the
       call, and Object and BasicObject were answered there too, so the walk
       starts one step up and stops at Object: a plain user class costs no
       compare, a Struct.new class one. (A name-to-id table scan would cost
       eighty per call, on every poly is_a? a user object fails.) */
    buf_puts(&b, "static int sp_class_kind_of_name(int cls, const char *cn){\n");
    buf_puts(&b, "  sp_Class cur = {cls, NULL};\n");
    buf_printf(&b, "  for(int _i=0;_i<%d;_i++){\n", depth);
    buf_puts(&b, "    sp_Class next=cur.cls_id>=0?sp_class_superclass(cur):sp_builtin_superclass(cur);\n");
    buf_puts(&b, "    if(next.cls_id==cur.cls_id||next.cls_id==-116||next.cls_id==-117)return 0;\n");
    buf_puts(&b, "    const char *s=sp_class_to_s(next);\n");
    buf_puts(&b, "    if(s&&s[0]&&!strcmp(s,cn))return 1;\n");
    buf_puts(&b, "    cur=next;\n");
    buf_puts(&b, "  }\n");
    buf_puts(&b, "  return 0;\n}\n");
  }
  /* Builtin superclass chain (simplified Ruby class hierarchy) */
  buf_puts(&b, "static sp_Class sp_builtin_superclass(sp_Class c){\n");
  /* nil has no superclass: stay nil rather than falling to the Object default,
     which would resurrect a terminated chain into a cycle (#2654) */
  buf_puts(&b, "  if(sp_class_nil_p(c))return SP_CLASS_NIL;\n");
  /* A class carried by NAME (a rescued exception's #class) has no builtin
     cls_id to switch on; resolve its superclass through the exception
     hierarchy rather than defaulting to Object (#3031). */
  buf_puts(&b, "  if(c.name){const char*_p=sp_exc_parent_of_name(c.name);"
               "if(_p){sp_int _id=sp_builtin_id_of_name(_p);"
               "return _id!=SP_CLASS_NIL_ID?((sp_Class){_id,NULL}):((sp_Class){-1,_p});}}\n");
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
  /* File, IO -> Object (a socket's chain terminates through IO) */
  buf_puts(&b, "  case -120: return ((sp_Class){-116});\n");
  buf_puts(&b, "  case -121: return ((sp_Class){-120});\n");
  /* Dir -> Object */
  buf_puts(&b, "  case -165: return ((sp_Class){-116});\n");
  /* the socket chain, as CRuby's:
     TCPServer -> TCPSocket -> IPSocket -> BasicSocket -> IO,
     UDPSocket -> IPSocket, UNIXServer -> UNIXSocket -> BasicSocket,
     Socket -> BasicSocket */
  buf_puts(&b, "  case -166: return ((sp_Class){-120});\n");
  buf_puts(&b, "  case -167:case -171:case -173: return ((sp_Class){-166});\n");
  buf_puts(&b, "  case -168:case -170: return ((sp_Class){-167});\n");
  buf_puts(&b, "  case -169: return ((sp_Class){-168});\n");
  buf_puts(&b, "  case -172: return ((sp_Class){-171});\n");
  /* Thread / Mutex / Queue / ConditionVariable / Fiber -> Object, and
     SizedQueue -> Queue as CRuby has it */
  buf_puts(&b, "  case -174:case -175:case -176:case -178:case -179: return ((sp_Class){-116});\n");
  buf_puts(&b, "  case -177: return ((sp_Class){-176});\n");
  /* Object -> BasicObject */
  buf_puts(&b, "  case -116: return ((sp_Class){-117});\n");
  /* BasicObject: the hierarchy root -- its superclass is nil (#2654) */
  buf_puts(&b, "  case -117: return SP_CLASS_NIL;\n");
  buf_puts(&b, "  default: return ((sp_Class){-116});\n  }\n}\n");

  buf_puts(&b, "static int sp_class_lt(sp_Class a,sp_Class b){return a.cls_id!=b.cls_id&&sp_class_is_ancestor(b,a);}\n");
  buf_puts(&b, "static int sp_class_le(sp_Class a,sp_Class b){return sp_class_is_ancestor(b,a);}\n");
  /* the runtime archive's view of the same question, by id (sp_class_le_id_fn) */
  buf_puts(&b, "static int sp_class_le_ids(int a,int b){return sp_class_is_ancestor((sp_Class){b},(sp_Class){a});}\n");
  buf_puts(&b, "static int sp_class_gt(sp_Class a,sp_Class b){return sp_class_lt(b,a);}\n");
  buf_puts(&b, "static int sp_class_ge(sp_Class a,sp_Class b){return sp_class_le(b,a);}\n");
  /* The checked unbox emit_unbox_text uses for class-typed slots (#4437). */
  buf_puts(&b, "static void *sp_poly_unbox_cls(sp_RbVal v, int cls, const char *want){\n"
               "  if(v.tag==SP_TAG_NIL)return NULL;\n"
               "  if(v.tag==SP_TAG_OBJ&&sp_class_le((sp_Class){v.cls_id},(sp_Class){cls}))return v.v.p;\n"
               "  sp_raise_cls(\"TypeError\", sp_sprintf(\"wrong argument type %s (expected %s)\", sp_poly_class_name(v), want));\n"
               "  return NULL;\n}\n");
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
    /* One pass over the node table, resolving each class/module body to its
       own index. Scanning the whole table once per class was O(classes * N)
       and dominated codegen on class-heavy programs; each class still sees its
       own definitions in id order, so include/prepend order is unchanged. */
    {
      /* scan every def_node body and all reopenings */
      for (int id = 0; id < c->nt->count; id++) {
        const char *ty2 = nt_type(c->nt, id);
        if (!ty2 || (!sp_streq(ty2, "ClassNode") && !sp_streq(ty2, "ModuleNode"))) continue;
        int cp2 = nt_ref(c->nt, id, "constant_path");
        const char *cn2 = cp2 >= 0 ? nt_str(c->nt, cp2, "name") : NULL;
        int ci = cn2 ? comp_class_index(c, cn2) : -1;
        if (ci < 0 || ci >= c->nclasses) continue;
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
    /* An `obj.extend(Mod)` records its membership on the synthesized singleton
       subclass rather than as an `include` statement in a class body, so the
       scan above cannot see it -- the extended object answered is_a?(Mod) with
       false (#4080). Merge what analyze recorded, deduped against the scan. */
    for (int ci = 0; ci < c->nclasses; ci++) {
      ClassInfo *mci = &c->classes[ci];
      for (int m = 0; m < mci->nincluded_mods; m++) {
        int mid3 = mci->included_mods[m];
        if (mid3 < 0 || mid3 >= c->nclasses) continue;
        int seen3 = 0;
        for (int q = 0; q < cls_nincs[ci]; q++) if (cls_incs[ci][q] == mid3) { seen3 = 1; break; }
        if (seen3) continue;
        cls_incs[ci] = realloc(cls_incs[ci], sizeof(int) * (size_t)(cls_nincs[ci] + 1));
        if (!cls_incs[ci]) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
        cls_incs[ci][cls_nincs[ci]++] = mid3;
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
    /* A module has no superclass chain: `M.ancestors` is [M] and
       `N.ancestors` (N includes M) is [N, M]. The walk used to follow the
       Object parent a module shares with a class and append Object, Kernel,
       BasicObject to both. */
    buf_puts(&b, "    if(sp_class_is_module_val(cur))break;\n");
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
    /* the receiver itself is not one of the modules it includes: a module's
       ancestors now start with the module, and it showed up in its own list */
    buf_puts(&b, "  for(sp_int i=0;a&&i<a->len;i++){ sp_Class m={a->data[i].v.i,NULL};\n");
    buf_puts(&b, "    if(m.cls_id==c.cls_id) continue;\n");
    buf_puts(&b, "    if(sp_class_is_module_val(m)) sp_PolyArray_push(r,a->data[i]); }\n");
    buf_puts(&b, "  return r;\n}\n\n");
    /* Module-aware <= by walking sp_class_ancestors (replaces simpler versions). */
    buf_puts(&b, "static int sp_class_le_mod(sp_Class a,sp_Class b){\n");
    buf_puts(&b, "  /* a<=b: b is an ancestor of a, so b must appear in a's ancestors */\n");
    buf_puts(&b, "  sp_PolyArray *ancs=sp_class_ancestors(a);\n");
    buf_puts(&b, "  for(sp_int _i=0;_i<sp_PolyArray_length(ancs);_i++){\n");
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
      /* a boxed exception walks its name chain (Errno::ENOENT -> SystemCallError
         -> StandardError), and a name-backed class (SystemCallError, Errno::*,
         OpenStruct) matches by name -- both are invisible to the cls_id walk */
      "static int sp_poly_is_a(sp_RbVal obj,sp_Class klass){\n"
      "  if (obj.tag == SP_TAG_OBJ && obj.cls_id == SP_BUILTIN_EXCEPTION)\n"
      "    return sp_poly_kind_of_builtin(obj, sp_class_to_s(klass));\n"
      "  if (klass.name) return sp_poly_is_a_dyn(obj, sp_box_class(klass), 0);\n"
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
    /* The modules each exception class includes, for the module-aware match.
       Reuses the same include walk sp_class_ancestors is built from, so
       `rescue SomeModule` and `e.is_a?(SomeModule)` agree. */
    buf_puts(&b, "static const char *const *sp_user_exc_modules(const char *cls){\n");
    if (!any) buf_puts(&b, "  (void)cls;\n");
    if (any) {
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i)) continue;
        if (c->classes[i].nincluded_mods == 0 &&
            c->classes[i].nincluded_mod_names == 0) continue;
        const char *cn0 = class_ruby_name(c, i);
        if (!cn0) cn0 = c->classes[i].name;
        if (!cn0) continue;
        char *cn = strdup(cn0);
        buf_printf(&b, "  { static const char *const _m%d[] = {", i);
        for (int m = 0; m < c->classes[i].nincluded_mods; m++) {
          int mi = c->classes[i].included_mods[m];
          if (mi < 0 || mi >= c->nclasses) continue;
          const char *mn = class_ruby_name(c, mi);
          if (!mn) mn = c->classes[mi].name;
          if (mn) buf_printf(&b, "\"%s\", ", mn);
        }
        /* Builtin modules named by path carry no class index; their qualified
           string is what the match compares. */
        for (int m = 0; m < c->classes[i].nincluded_mod_names; m++)
          buf_printf(&b, "\"%s\", ", c->classes[i].included_mod_names[m]);
        buf_puts(&b, "0 };\n");
        buf_printf(&b, "    if(!strcmp(cls,\"%s\"))return _m%d;\n", cn, i);
        if (c->classes[i].name && !sp_streq(cn, c->classes[i].name))
          buf_printf(&b, "    if(!strcmp(cls,\"%s\"))return _m%d;\n", c->classes[i].name, i);
        buf_puts(&b, "  }\n");
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
  check_class_layout_prefix(c);
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
                       : t == TY_FLOAT ? "SP_FLOAT_NIL_CONST"   /* sp_float_nil() is a call, not a constant */
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
                       : (t == TY_FLOAT) ? "SP_FLOAT_NIL_CONST"
                       : (t == TY_STRING) ? "NULL"
                       : (is_scalar_ret(t)) ? default_value(t) : "0";
      buf_puts(&b, "static ");
      emit_ctype(c, t, &b);
      buf_printf(&b, " civ_%s_%s = %s;\n", ci->name, iv_c(ci->ivars[j] + 1), init);
    }
  }

  /* default-inspect dispatch prototype: bodies call it, the switch itself is
     emitted with the marshal dispatch near the end of the TU. Only user-class
     instances route through it, so a classless program emits neither. */
  if (g_emit_obj_dispatch) {
    buf_puts(&b, "static const char *sp_obj_inspect_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static const char *sp_obj_to_s_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static sp_int sp_obj_to_int_sw(int cls_id, void *p, int *ok) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static const char *sp_obj_to_str_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static const char *sp_obj_to_path_sw(int cls_id, void *p) __attribute__((cold, noinline));\n");
    if (c->uses_kconv)
      buf_puts(&b, "static int sp_obj_conv_sw(int cls_id, void *p, int which, sp_RbVal *out) __attribute__((cold, noinline));\n");
    buf_puts(&b, "static const char *sp_obj_cls_name_rt(int cls_id) __attribute__((cold, noinline));\n");
  }
  /* The #message / #to_s dispatchers below call these bodies unconditionally,
     so a program that defines an override without ever querying it left the
     dispatcher calling an undeclared function (#3834). Mark them before the
     prototypes are written. */
  if (exc_has_user_msg_override(c) || exc_has_nonstring_msg_override(c)) {
    for (int i = 0; i < c->nclasses; i++) {
      if (!class_is_exc_subclass(c, i)) continue;
      static const char *const fns[2] = { "message", "to_s" };
      for (int k = 0; k < 2; k++) {
        int mi = comp_method_in_chain(c, i, fns[k], NULL);
        if (mi >= 0 && (TyKind)c->scopes[mi].ret != TY_UNKNOWN) c->scopes[mi].reachable = 1;
      }
    }
  }

  /* method prototypes (scope 0 is top-level) */
  /* A proc form is named only by a poly dispatch, which is emitted later, so
     the reachability pass cannot see it. Emit it and let the C compiler drop
     it if no arm ends up calling it (#3399). */
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || (!c->scopes[s].reachable && !c->scopes[s].is_proc_form) || scope_is_shadowed(c, s) || (c->scopes[s].is_transplanted_source && !scope_toplevel_included(c, s))) continue; emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }

  /* A boxed exception carries SP_BUILTIN_EXCEPTION, not its user class's
     index, so a poly dispatch over a user exception class's own method keys
     it through this map instead (emit_poly_dispatch_key's exc_cand): its
     class name to that index, the arm's struct being the exception header
     followed by the class's ivars (#5093). */
  { int any_exc = 0;
    for (int i = 0; i < c->nclasses && !any_exc; i++) any_exc = class_is_exc_subclass(c, i);
    if (any_exc) {
      buf_puts(&b, "__attribute__((unused)) static int sp_exc_user_cls_id(sp_RbVal v){\n");
      buf_puts(&b, "  const char *n = v.v.p ? ((sp_Exception *)v.v.p)->cls_name : NULL;\n  if (!n) return 0x7fffffff;\n");
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i) || !c->classes[i].name) continue;
        buf_printf(&b, "  if (strcmp(n, \"%s\") == 0) return %d;\n", c->classes[i].name, i);
      }
      buf_puts(&b, "  return 0x7fffffff;\n}\n");
    } }

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
        }
        else if (mi_tos >= 0) { mi = mi_tos; dcls = dtos; fn = "to_s"; }
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
  /* The boxed pair: an override answering something other than a String cannot
     be represented by the const char * dispatchers above, so #message on an
     exception whose class is only known at run time reported the stored message
     (the class name) instead of what #to_s answered (#3868). */
  if (exc_has_nonstring_msg_override(c)) {
    for (int pass = 0; pass < 2; pass++) {
      int want_message = pass;
      buf_printf(&b, "__attribute__((unused)) static sp_RbVal %s(sp_Exception *e){\n",
                 want_message ? "sp_user_exc_message_v" : "sp_user_exc_to_s_v");
      buf_puts(&b, "  if(!e)return sp_box_str((&(\"\\xff\")[1]));\n  const char *cls=e->cls_name;\n");
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
        }
        else if (mi_tos >= 0) { mi = mi_tos; dcls = dtos; fn = "to_s"; }
        if (mi < 0) continue;
        TyKind mret = (TyKind)c->scopes[mi].ret;
        if (mret == TY_UNKNOWN || mret == TY_VOID) continue;
        const char *dcn = c->classes[dcls].c_name;
        const char *cn0 = class_ruby_name(c, i);
        if (!cn0) cn0 = c->classes[i].name;
        if (!cn0) continue;
        char callx[256];
        snprintf(callx, sizeof callx, "sp_%s_%s((sp_%s*)e)", dcn, fn, dcn);
        Buf bx; memset(&bx, 0, sizeof bx);
        if (mret == TY_POLY) buf_puts(&bx, callx);
        else emit_boxed_text(c, mret, callx, &bx);
        buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return %s;\n", cn0, bx.p ? bx.p : "sp_box_nil()");
        if (c->classes[i].name && !sp_streq(cn0, c->classes[i].name))
          buf_printf(&b, "  if(!strcmp(cls,\"%s\"))return %s;\n",
                     c->classes[i].name, bx.p ? bx.p : "sp_box_nil()");
        free(bx.p);
      }
      buf_printf(&b, "  return sp_box_str(%s(e));\n}\n",
                 exc_has_user_msg_override(c)
                   ? (want_message ? "sp_user_exc_message" : "sp_user_exc_to_s")
                   : "sp_exc_message");
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
      /* the allocation-only half, for the sites that splice the body */
      if (ctor_init_proc_form(c, i) >= 0) {
        buf_printf(&b, "static sp_%s *sp_%s_new_noinit(", ci->c_name, ci->c_name);
        Scope *s = &c->scopes[init];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          LocalVar *p = scope_local(s, s->pnames[m]);
          emit_ctype(c, (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY, &b);
        }
        if (s->nparams == 0) buf_puts(&b, "void");
        buf_puts(&b, ");\n");
      }
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
               lv->type == TY_POLY  ? "{SP_TAG_NIL, 0, {0}}" :
               /* A global read before its first write is nil, so the slot
                  starts at the kind's nil SENTINEL. Only the string case knew
                  that; an int-typed one started at 0, which reads back as the
                  integer zero -- `if $pgid` was truthy on a global nothing had
                  assigned, and `-$pgid` was -0 (#4248). */
               lv->type == TY_INT   ? "SP_INT_NIL" :
               /* the float sentinel's constant spelling (a NaN with the
                  payload), since the union read of sp_float_nil() is not
                  a constant expression */
               lv->type == TY_FLOAT ? "SP_FLOAT_NIL_CONST" :
               lv->type == TY_STRING ? "NULL" : default_value(lv->type));
  }
  /* One slot per DISTINCT out-of-int64 literal, filled on first use. The
     scan is over the AST rather than the emission, so the slots exist before
     the mark function below is written. */
  for (int id = 0; id < c->nt->count; id++) {
    if (nt_kind(c->nt, id) != NK_IntegerNode) continue;
    const char *bv = nt_str(c->nt, id, "bigval");
    if (bv) bigl_intern(bv);
  }
  if (g_bigl_n) {
    buf_printf(&b, "static sp_Bigint *sp_bigl[%d];\n", g_bigl_n);
    buf_printf(&b, "static const char *const sp_bigl_s[%d] = {", g_bigl_n);
    for (int i = 0; i < g_bigl_n; i++)
      buf_printf(&b, "%s\"%s\"", i ? ", " : "", g_bigl_val[i]);
    buf_puts(&b, "};\n");
    /* Filled by sp_tu_init at startup (see emit_regex_section), so a read
       is a plain load: no lazy fill, and so no race between threads that
       reach the same literal first (#4641). */
    buf_puts(&b, "static inline sp_Bigint *sp_bigl_get(int i) { return sp_bigl[i]; }\n\n");
  }
  for (int i = 0; i < c->nconsts; i++) {
    LocalVar *lv = &c->consts[i];
    /* `NAME = nil` still needs a slot: the assignment and every read reference
       cst_NAME, so skipping the declaration left an undeclared identifier
       (#3361). An int carrier is enough -- the reads fold to nil on their own,
       they just have to have something to evaluate. */
    if (lv->type == TY_NIL) {
      buf_printf(&b, "static sp_int cst_%s = SP_INT_NIL;\n", lv->name);
      if (lv->init_guarded) buf_printf(&b, "static int sp_init_in_progress_%s;\n", lv->name);
      continue;
    }
    if (!is_scalar_ret(lv->type)) continue;
    buf_puts(&b, "static ");
    emit_ctype(c, lv->type, &b);
    buf_printf(&b, " cst_%s = %s;\n", lv->name,
               lv->type == TY_RANGE ? "{0}" :
               lv->type == TY_POLY  ? "{SP_TAG_NIL, 0, {0}}" :
               lv->type == TY_FLOAT ? "SP_FLOAT_NIL_CONST" :   /* the constant spelling of the float sentinel */
               default_value(lv->type));
    if (lv->init_guarded) buf_printf(&b, "static int sp_init_in_progress_%s;\n", lv->name);
  }
  if (c->ngvars || c->nconsts) buf_puts(&b, "\n");

  /* Runtime class table: cls_ids of every user exception subclass.
   * sp_raise_poly checks this before re-raising a boxed object,
   * because reading the sp_Exception prefix on a non-exception user
   * object is a wrong-offset read (segfault under clang). The
   * `sp_exc_subclass_count == 0` case still emits the symbols so
   * the runtime links without a separate stub. */
  {
    int n = 0;
    for (int i = 0; i < c->nclasses; i++)
      if (class_is_exc_subclass(c, i)) n++;
    buf_printf(&b, "const sp_int sp_exc_subclass_count = %d;\n", n);
    if (n > 0) {
      buf_puts(&b, "const sp_int sp_exc_subclass_ids[] = {");
      for (int i = 0; i < c->nclasses; i++)
        if (class_is_exc_subclass(c, i))
          buf_printf(&b, " %d,", i);
      buf_puts(&b, " };\n");
    } else {
      buf_puts(&b, "const sp_int sp_exc_subclass_ids[] = { 0 };\n");
    }
  }

  /* GC marking for the file-scope statics above: heap objects reachable
     only through a global/constant/class-ivar slot would otherwise be
     swept (RAND = Rand.new lost its PRNG mid-render). Chained ahead of
     the runtime's own sp_re_mark_globals via the hook in sp_tu_init. */
  {
    /* Collect the user-global mark lines into a temp buffer first. If none are
       emitted, the marker would be identical to the runtime default
       (sp_re_mark_globals, installed by a constructor before main), so skip it
       and the sp_tu_init hook override entirely -- a trivial program carries
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
        const char *iv = iv_c(ci->ivars[j] + 1);
        if (t == TY_STRING) buf_printf(&mk, "  sp_mark_string(civ_%s_%s);\n", ci->name, iv);
        else if (t == TY_POLY) buf_printf(&mk, "  sp_mark_rbval(civ_%s_%s);\n", ci->name, iv);
        else if (needs_root(t)) buf_printf(&mk, "  if (civ_%s_%s) sp_gc_mark((void *)civ_%s_%s);\n", ci->name, iv, ci->name, iv);
      }
      for (int j = 0; j < ci->nsg_readers; j++)
        buf_printf(&mk, "  sp_mark_rbval(sg_%s_%s);\n", ci->name, ci->sg_readers[j]);
      /* class variables are file-scope statics too; one that alone holds an
         object (`@@a |= [x]` rebinding it) was freed under it (#4864) */
      for (int j = 0; j < ci->ncvars; j++) {
        TyKind t = ci->cvar_types[j] == TY_UNKNOWN ? TY_INT : ci->cvar_types[j];
        const char *cv = ci->cvars[j] + 2;
        if (t == TY_STRING) buf_printf(&mk, "  sp_mark_string(cvar_%s_%s);\n", ci->name, cv);
        else if (t == TY_POLY) buf_printf(&mk, "  sp_mark_rbval(cvar_%s_%s);\n", ci->name, cv);
        else if (needs_root(t)) buf_printf(&mk, "  if (cvar_%s_%s) sp_gc_mark((void *)cvar_%s_%s);\n", ci->name, cv, ci->name, cv);
      }
    }
    /* $0 and the proc calling convention's side channel are marked by the
       runtime's own sp_re_mark_globals (lib/spinel_rt.h), so a program with
       none of the globals above carries no marker and no startup hook. */
    if (g_has_dyn_syms) buf_puts(&mk, "  sp_mark_dyn_syms();\n");
    if (g_bigl_n)
      buf_printf(&mk, "  for (int _i = 0; _i < %d; _i++) if (sp_bigl[_i]) sp_gc_mark((void *)sp_bigl[_i]);\n",
                 g_bigl_n);
    g_has_user_global_marks = (mk.p && mk.len > 0);
    if (g_has_user_global_marks) {
      buf_puts(&b, "static void sp_mark_user_globals(void) {\n");
      buf_puts(&b, "  sp_re_mark_globals();\n");
      buf_puts(&b, "  sp_marshal_mark_active();\n");
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
  g_has_user_binop = 0;
  {
    static const char *const uops[] = {
      "+", "-", "*", "/", "%", "**", "<<", ">>", "&", "|", "^", "==", "[]", NULL };
    /* A class that defines a #coerce needs the table for its COMPARISONS too:
       the protocol routes `5 < obj` to the boxed entry, which reaches the
       class through this hook. Only for such a class, though -- an ordinary
       Comparable defines <=> and no coerce, is never reached this way, and
       would only gain a dispatch table it has no use for. */
    static const char *const cops[] = { "<", ">", "<=", ">=", "<=>", NULL };
    for (int k = 0; k < c->nclasses && !g_has_user_binop; k++) {
      if (!c->classes[k].instantiated) continue;
      for (int u = 0; uops[u]; u++)
        if (comp_method_in_chain(c, k, uops[u], NULL) >= 0) { g_has_user_binop = 1; break; }
      /* a `<=>` with no `==` is Comparable's equality, which the table
         derives: a boxed `m == n` (a block parameter, a hash value, a
         `when FIVE`) reaches it only through the table */
      if (!g_has_user_binop && comp_method_in_chain(c, k, "<=>", NULL) >= 0 &&
          comp_method_in_chain(c, k, "==", NULL) < 0) { g_has_user_binop = 1; break; }
      if (g_has_user_binop || !class_has_coerce_shape(c, k)) continue;
      for (int u = 0; cops[u]; u++)
        if (comp_method_in_chain(c, k, cops[u], NULL) >= 0) { g_has_user_binop = 1; break; }
    }
  }
  g_has_user_aset = 0;
  for (int k = 0; k < c->nclasses && !g_has_user_aset; k++)
    if (c->classes[k].instantiated && user_aset_scope(c, k, NULL) >= 0) g_has_user_aset = 1;
  g_has_user_coerce = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    if (class_coerce_emittable(c, k)) { g_has_user_coerce = 1; break; }
  }
  g_has_user_to_io = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].instantiated) continue;
    int dc = -1, mi2 = comp_method_in_chain(c, k, "to_io", &dc);
    if (mi2 < 0 || dc < 0) continue;
    Scope *m2 = &c->scopes[mi2];
    if (m2->reachable && !m2->yields && m2->nparams == 0 &&
        (m2->ret == TY_IO || m2->ret == TY_UNKNOWN || m2->ret == TY_POLY) &&
        !scope_is_shadowed(c, mi2) && !m2->is_transplanted_source) { g_has_user_to_io = 1; break; }
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

  /* sp_tu_init is worth emitting only if it would set at least one hook. */
  g_re_init_needed = g_uses_symbols || g_uses_marshal || g_uses_regex || g_needs_class_machinery ||
                     g_has_user_global_marks || g_has_user_cmp || g_gen_obj_hash || g_gen_obj_to_h || g_gen_obj_with || g_gen_obj_hashkey ||
                     g_gen_obj_valeq;

  /* Constructor defs, method defs, and main go into a separate buffer. Any
     proc literals they contain accumulate static functions into g_procs /
     g_proc_protos; we splice those in ahead of these bodies, since a proc
     function must be declared before the body that references it. */
  Buf *body = (Buf *)calloc(1, sizeof *body);  /* heap: must survive a collect-mode longjmp (see EMIT_COLLECT_UNIT) */
  emit_synth_line_marker(body);
  for (int i = 0; i < c->nclasses; i++)
    if (!is_builtin_reopen(c->classes[i].name))
      EMIT_COLLECT_UNIT(emit_class_new(c, &c->classes[i], body));
  /* user-object Marshal dispatchers (after every class struct + pool define) */
  if (g_uses_marshal) emit_marshal_dispatch(c, body);
  if (g_emit_obj_dispatch) emit_obj_inspect_dispatch(c, body);
  emit_obj_to_hash_dispatch(c, body);
  emit_obj_to_json_dispatch(c, body);
  emit_obj_to_h_dispatch(c, body);
  emit_obj_struct_values_dispatch(c, body);
  emit_obj_deconstruct_dispatch(c, body);
  emit_obj_is_data(c, body);
  emit_obj_to_a_dispatch(c, body);
  emit_obj_to_ary_dispatch(c, body);
  emit_obj_with_dispatch(c, body);
  for (int s = 1; s < c->nscopes; s++) {
    if (c->scopes[s].yields || (!c->scopes[s].reachable && !c->scopes[s].is_proc_form) || scope_is_shadowed(c, s) || (c->scopes[s].is_transplanted_source && !scope_toplevel_included(c, s))) continue; EMIT_COLLECT_UNIT(emit_method(c, &c->scopes[s], body));
  }
  /* Comparable cmp-hook dispatcher (after the user `<=>` definitions it calls). */
  emit_synth_line_marker(body);
  if (g_has_user_cmp) emit_obj_cmp_dispatch(c, body);
  if (g_has_user_binop) emit_user_binop_dispatch(c, body);
  if (g_has_user_aset) emit_user_aset_dispatch(c, body);
  if (g_has_user_coerce) emit_user_coerce_dispatch(c, body);
  if (g_has_user_to_io) emit_user_to_io_dispatch(c, body);
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
        size_t end_frame_ins = body->len;
        EMIT_COLLECT_UNIT(emit_stmts(c, stmts, body, 1));
        buf_puts(body, "}\n");
        if (!g_no_root_frame) gc_frame_build(body, end_frame_ins);
      }
    }
  }

  size_t main_frame_ins = 0;
  if (g_ext_init_name) {
    /* Layer-1 extension emission (ext-design.md): the toplevel body brackets
       into the host-callable init function instead of main, and a tiny
       try-frame helper wraps the exception protocol so a hand-written shim
       never reaches into runtime statics (M0 finding 1). */
    buf_printf(body,
      "int %s_try(void (*fn)(void *), void *ctx, const char **cls, const char **msg) {\n"
      "  sp_exc_check_depth();\n"
      "  sp_exc_rootmark[sp_exc_top] = sp_gc_nroots; sp_rescue_mark[sp_exc_top] = sp_rescue_sp;\n"
      "  sp_exc_msg[sp_exc_top] = 0; sp_exc_obj[sp_exc_top] = 0; sp_exc_top++;\n"
      "  if (setjmp(sp_exc_stack[sp_exc_top - 1]) == 0) { fn(ctx); sp_exc_top--; return 0; }\n"
      "  sp_exc_top--;\n"
      "  sp_gc_nroots = sp_exc_rootmark[sp_exc_top]; sp_rescue_sp = sp_rescue_mark[sp_exc_top];\n"
      "  if (cls) *cls = (const char *)sp_last_exc_cls;\n"
      "  if (msg) *msg = sp_exc_msg[sp_exc_top] ? sp_exc_msg[sp_exc_top] : \"\";\n"
      "  return 1;\n}\n", g_ext_init_name);
    buf_printf(body, "void %s(void){\n", g_ext_init_name);
    buf_puts(body, "    SP_GC_SAVE();\n");
    main_frame_ins = body->len;
    if (g_re_init_needed) buf_puts(body, "    sp_tu_init();\n");
    if (g_uses_threads) buf_puts(body, "    sp_sched_init();\n");
    if (g_uses_program_name) buf_puts(body, "    sp_program_name = sp_str_empty;\n");
  }
  else {
  /* The body runs on a stack this compiler chose, not the one the loader
     gave the process (sp_main_stack_run). It is emitted as its own function
     so the context switch has something to enter, and `main` shrinks to the
     trampoline that hands it over and returns what it left behind. argc/argv
     move to file scope with it: they belong to the frame main keeps.

     The names carry a leading underscore because a Ruby method compiles to
     `sp_<name>`: `def main_body` would otherwise collide with the body
     itself. The backtrace demangler knows _sp_main_body as `<main>` -- it is
     where the top level runs now, and the frame the walk used to find as
     `main`. */
  buf_puts(body, "static int _sp_main_argc; static char **_sp_main_argv;"
                 " static int _sp_main_rc;\n");
  /* NOT static: on ELF, backtrace_symbols names a frame through the dynamic
     symbol table (--debug links with -rdynamic), where a static function does
     not appear -- so the demangler never saw the name it knows as `<main>`
     and the rescued backtrace lost its outermost frame. macOS symbolises from
     the full table, which is why only the Linux lanes showed it. The `_sp_`
     spelling is already reserved, so nothing can collide with it as an
     external. */
  buf_puts(body, "void _sp_main_body(void){\n");
  buf_puts(body, "    SP_GC_SAVE();\n");
  main_frame_ins = body->len;
  if (g_re_init_needed) buf_puts(body, "    sp_tu_init();\n");
  /* Adopt the main thread and chain the scheduler's GC root hook. Placed after
     sp_tu_init so it chains whatever globals hook that installed. */
  if (g_uses_threads) buf_puts(body, "    sp_sched_init();\n");
  /* The ARGV copy loop only matters if the program reads ARGV / ARGF / $*. */
  if (g_uses_argv)
    buf_puts(body, "    { int argc = _sp_main_argc; char **argv = _sp_main_argv; sp_argv.len = argc - 1; sp_argv.data = (const char**)malloc(sizeof(const char*) * (size_t)(argc > 1 ? argc - 1 : 1)); for (int _ai = 0; _ai < argc - 1; _ai++) sp_argv.data[_ai] = sp_str_dup_external(argv[_ai + 1]); }\n");
  if (g_uses_program_name)
    /* argv[0] lives in the process's argument block, not the string heap, so it
     carries no marker byte -- and `$0` is an ordinary Ruby String the caller
     roots. Copy it in. */
    buf_puts(body, "    sp_program_name = _sp_main_argc > 0 ? sp_str_dup_external(_sp_main_argv[0]) : sp_str_empty;\n");
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
  if (g_ext_init_name) {
    /* `if __FILE__ == $0` is the kernel's manual test driver AND the entry
       methods' call-site type source (ext-design.md): inference consumed it,
       the emitted init drops it. Everything else in the toplevel runs. */
    int tb9 = c->scopes[0].body;
    const char *tt9 = tb9 >= 0 ? nt_type(c->nt, tb9) : NULL;
    int tn9 = 0;
    const int *tv9 = (tt9 && sp_streq(tt9, "StatementsNode"))
                     ? nt_arr(c->nt, tb9, "body", &tn9) : NULL;
    for (int k9 = 0; k9 < tn9; k9++) {
      int st9 = tv9[k9];
      if (nt_kind(c->nt, st9) == NK_IfNode) {
        int pr9 = nt_ref(c->nt, st9, "predicate");
        if (pr9 >= 0 && nt_kind(c->nt, pr9) == NK_CallNode &&
            nt_str(c->nt, pr9, "name") && sp_streq(nt_str(c->nt, pr9, "name"), "==")) {
          int rc9 = nt_ref(c->nt, pr9, "receiver");
          int an9 = 0; const int *av9 = call_args(c->nt, pr9, &an9);
          int o9 = an9 == 1 ? av9[0] : -1;
          int sf9 = 0, pg9 = 0;
          for (int w9 = 0; w9 < 2; w9++) {
            int nd9 = w9 ? o9 : rc9;
            if (nd9 < 0) continue;
            NodeKind nk9 = nt_kind(c->nt, nd9);
            if (nk9 == NK_SourceFileNode) sf9 = 1;
            else if (nk9 == NK_GlobalVariableReadNode) {
              const char *gn9 = nt_str(c->nt, nd9, "name");
              if (gn9 && (sp_streq(gn9, "$0") || sp_streq(gn9, "$PROGRAM_NAME"))) pg9 = 1;
            }
          }
          if (sf9 && pg9) continue;
        }
      }
      EMIT_COLLECT_UNIT(emit_stmt(c, st9, body, 1));
    }
  }
  else EMIT_COLLECT_UNIT(emit_stmts(c, c->scopes[0].body, body, 1));
  /* Run any fire-and-forget threads that never got a turn before the program
     exits, so their side effects happen. */
  if (g_uses_threads) buf_puts(body, "    sp_sched_drain();\n");
  /* main's exit status is the hooks' to change: a hook that calls `exit`, or
     that raises, decides what the program exits with (sp_at_exit_run). The
     ext-init form is a void function, so there it just runs them. */
  if (g_needs_at_exit && g_ext_init_name) buf_puts(body, "  sp_at_exit_run(0);\n");
  if (g_ext_init_name) buf_puts(body, "}\n");
  else {
    if (g_needs_at_exit) buf_puts(body, "  _sp_main_rc = sp_at_exit_run(0);\n}\n");
    else buf_puts(body, "  _sp_main_rc = 0;\n}\n");
    buf_puts(body, "int main(int argc,char**argv){\n"
                   "  _sp_main_argc = argc; _sp_main_argv = argv;\n");
    /* an unoptimised build's frames are several times an -O2 one's, so the
       same Ruby depth costs several times the bytes; SPINEL_MAIN_STACK in the
       environment still wins over this */
    if (g_opt_level < 2)
      buf_puts(body, "  sp_main_stack_hint((size_t)1024 << 20);\n");
    buf_puts(body, "  sp_main_stack_run(_sp_main_body);\n"
                   "  return _sp_main_rc;\n}\n");
  }
  if (!g_no_root_frame) gc_frame_build(body, main_frame_ins);

  emit_regex_section(c, &b);
  { const char *pdt[3] = { g_procs.p, body->p, b.p };
    pd_emit_used(pdt, 3, &g_pd_protos, &g_pd_defs); }
  if (g_proc_protos.len) { buf_puts(&b, g_proc_protos.p); buf_puts(&b, "\n"); }
  if (g_pd_protos.len) { buf_puts(&b, g_pd_protos.p); buf_puts(&b, "\n"); }
  if (g_procs.len) { buf_puts(&b, g_procs.p); buf_puts(&b, "\n"); }
  buf_puts(&b, body->p ? body->p : "");
  /* last: an arm may call a helper the body defines (a value type's boxer) */
  if (g_pd_defs.len) { buf_puts(&b, "\n"); buf_puts(&b, g_pd_defs.p); }
  free(body->p);
  free(body);
  /* Over the whole program, not per function: methods, procs, block bodies,
     constructors and main are emitted by different paths, and hooking them one
     at a time left a quarter of the stores bare. */
  gc_wb_insert(c, &b, 0);
  if (g_line_map) line_map_reanchor(&b);
  free(g_procs.p); free(g_proc_protos.p);
  free(g_pd_protos.p); free(g_pd_defs.p);
  memset(&g_pd_protos, 0, sizeof g_pd_protos); memset(&g_pd_defs, 0, sizeof g_pd_defs);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  g_needs_proc_poly_argslot = 0;

  if (g_ext_init_name) {
    ext_refuse_param_mutation(c);
    /* the emitted header IS Layer 1's contract: init, the try-frame pair,
       and every entry in its real C signature. A hand shim compiles against
       it, so a type drift breaks the host's build instead of reinterpreting. */
    Buf hb; memset(&hb, 0, sizeof hb);
    buf_printf(&hb, "/* Generated by Spinel (--ext-init %s). The contract for a host\n"
                    "   shim: call %s() once before any entry; wrap entry calls in\n"
                    "   %s_try to receive a Ruby raise as (class name, message). */\n",
               g_ext_init_name, g_ext_init_name, g_ext_init_name);
    buf_puts(&hb, "#ifndef SPINEL_EXT_H\n#define SPINEL_EXT_H\n");
    buf_puts(&hb, "#define SPINEL_EXT_HOST 1  /* runtime globals resolve to the kernel TU */\n");
    buf_puts(&hb, "#include \"spinel_rt.h\"\n\n");
    buf_printf(&hb, "void %s(void);\n", g_ext_init_name);
    buf_printf(&hb, "int %s_try(void (*fn)(void *), void *ctx, const char **cls, const char **msg);\n\n",
               g_ext_init_name);
    for (int s9 = 1; s9 < c->nscopes; s9++) {
      if (!c->scopes[s9].is_ext_entry) continue;
      emit_method_signature(c, &c->scopes[s9], &hb);
      buf_puts(&hb, ";\n");
    }
    buf_puts(&hb, "\n#endif\n");
    free(g_ext_header_text);
    g_ext_header_text = hb.p;
    if (g_ext_target && sp_streq(g_ext_target, "cruby"))
      ext_generate_cruby_shim(c);
  }
  /* Every refusal was reported as its unit was abandoned; the run fails here,
     once, with nothing written (SP_COLLECT_ERRORS emits the rest regardless,
     for a reduction). */
  if (types_out) {
    char *json = build_types_json(c);
    emit_write_file(types_out, json);
    free(json);
    { const char *keep = getenv("SPINEL_EMIT_TYPES_KEEP_C");
      if (!(keep && *keep)) { free(b.p); b.p = NULL; } }
  }
  if (g_ndiags > 0 && !collect_emit_anyway()) {
    /* not "unsupported": spinel-doctor and spinel-reduce count the lines
       that say so, and this one is the count */
    fprintf(stderr, "spinel: %d refusal%s%s\n", g_ndiags, g_ndiags == 1 ? "" : "s",
            types_out ? "" : ", nothing written");
    if (types_out) fprintf(stderr, "Wrote %s\n", types_out);
    exit(1);
  }
  comp_free(c);
  { const char *keep = getenv("SPINEL_EMIT_TYPES_KEEP_C");
    if (types_out && !(keep && *keep)) return strdup(""); }
  return b.p;
}

