#include <limits.h>
#include "codegen_internal.h"
#include "call_plan.h"
#include "repr.h"

/* classes whose pool a proc or fiber body has declared, per program */
static unsigned char *g_pool_fwd = NULL;
static int g_pool_fwd_n = 0;

/* A reference-backed builtin (IO/Fiber/Thread/Queue/Mutex/ConditionVariable/
   Enumerator/Exception/Proc/Method) is a genuinely nilable C pointer: an unset
   ivar, a `return nil` method, or a cache miss yields NULL. It must box via
   sp_box_nullable_obj so a NULL becomes SP_TAG_NIL rather than a "truthy"
   SP_TAG_OBJ wrapping NULL v.p -- which passes `unless x`/`if x`, misreads as
   non-nil (a wrong `nil?`), then segfaults on the first field/method read.
   Value-type builtins (Range/Time/Complex/Rational, boxed as a by-value copy)
   and non-pointer scalars are deliberately excluded -- they are never NULL. */
const char *ty_nullable_builtin_id(TyKind t) {
  /* a reference-backed builtin's box id is its ty_traits row's box_id
     (types.c): IO, Fiber, Thread, Queue, Mutex, ConditionVariable,
     Enumerator, Exception, Proc, Method, Dir, Addrinfo, Socket::Option,
     OpenStruct, MatchData, Regexp (#3950), Proc#curry, Random and ARGF.
     Process::Tms is a VALUE type boxed by heap copy (sp_box_tms), never as
     a nullable pointer (#3132). */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->box_id : NULL;
}

/* The types whose C value is a pointer with NULL for nil: an ivar no
   constructor assigns, a `return nil` method, a miss. A method nil answers
   differently (class, to_s, == nil) has to test for it. */
int ty_null_is_nil(TyKind t) {
  /* a builtin kind's ty_traits row says (types.c); an object array is a
     pointer array, NULL for nil */
  const TyTraits *tr = ty_traits_of(t);
  return tr ? tr->null_is_nil : ty_is_obj_array(t);
}

int node_may_be_null_nil(Compiler *c, int node) {
  switch (nt_kind(c->nt, node)) {
    case NK_StringNode: case NK_InterpolatedStringNode: case NK_XStringNode:
    case NK_ArrayNode: case NK_HashNode: case NK_RegularExpressionNode:
    case NK_InterpolatedRegularExpressionNode: case NK_IntegerNode:
    case NK_LambdaNode: case NK_SelfNode:
      return 0;
    default:
      return 1;
  }
}

void emit_null_guarded_call(Compiler *c, int recv, TyKind rt, const char *fn, const char *nil_c, Buf *b) {
  if (!node_may_be_null_nil(c, recv)) {
    buf_printf(b, "%s(", fn); emit_expr(c, recv, b); buf_puts(b, ")");
    return;
  }
  int t = ++g_tmp;
  Buf rb = expr_buf(c, recv);
  buf_puts(b, "({ "); emit_ctype(c, rt, b);
  buf_printf(b, " _t%d = %s; _t%d ? %s(_t%d) : %s; })", t, rb.p ? rb.p : "0", t, fn, t, nil_c);
  free(rb.p);
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

/* --repr-check (#7100 Phase E, R1): each return of emit_boxed and
   emit_boxed_text records the form it boxed the value in; emit_boxed's are
   compared with the form repr_of predicts for the node, and a mismatch is
   printed to stderr with what explains it. Only statics are touched, so the
   C is the same with and without the flag. */
enum {
  RW_NONE,           /* nothing at the site explains a difference */
  RW_YIELD,          /* a yield boxed by the block of this call site */
  RW_TRANSPLANT,     /* a module ivar read boxed by the including class's field */
  RW_RAN_FIRST,      /* an argument that already ran: its temp */
  RW_LITERAL         /* an empty literal or Hash.new boxed by its shape */
};
static int rc_depth;        /* emit_boxed nesting */
static int rc_text_form = -1;
static TyKind rc_text_ty = TY_UNKNOWN;
/* emit_boxed_text's form, kept for the emit_boxed that called it */
static void rc_text_note(TyKind t, int form) {
  if (!g_repr_check || rc_depth <= 0) return;
  rc_text_form = form;
  rc_text_ty = t;
}
static void rc_note(Compiler *c, int node, int form, int why, int from_text) {
  if (!g_repr_check || node < 0 || form < 0) return;
  Repr r = repr_of(c, node);
  int want = repr_box_form(c, r);
  if (want == form) return;
  const char *cls = "conflict";
  const char *nty = nt_type(c->nt, node);
  if (view_depth() > 0) cls = "view";
  else if (why == RW_YIELD) cls = "yield-site";
  else if (why == RW_TRANSPLANT) cls = "transplant";
  else if (why == RW_RAN_FIRST) cls = "ran-first";
  else if (why == RW_LITERAL) cls = "literal";
  else if ((form == RF_INT_NIL || form == RF_FLT_NIL) &&
           nt_kind(c->nt, node) == NK_LocalVariableReadNode) {
    const char *ln = nt_str(c->nt, node, "name");
    Scope *s = ln ? comp_scope_of(c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (lv && lv->is_block_param && lv->nullable_int) cls = "late-slot";
  }
  if (sp_streq(cls, "conflict") && from_text && rc_text_ty != r.as_ty) cls = "text-vs-node";
  fprintf(stderr, "repr-check: %s: node %d %s %s: emitted %s, predicted %s\n",
          cls, node, nty ? nty : "?", ty_name(r.as_ty),
          repr_form_name(form), repr_form_name(want));
}
#define RC(form, why) rc_note(c, node, (form), (why), 0)
#define RC_TEXT(why) rc_note(c, node, rc_text_form, (why), 1)
#define RCT(form) rc_text_note(t, (form))

/* Box `expr`, a value of kind t, into an sp_RbVal.

   A builtin kind writes its ty_traits row's box_text (types.c) and records
   the row's box_form for --repr-check. The rows box nil as nil wherever the
   kind's C value can hold it, for these reasons:
   - An untyped or void value is already a boxed nil or a poly call result,
     or carries side effects: it is evaluated for effect and yields nil, as
     emit_boxed does. The int box it fell to fed an sp_RbVal to an sp_int.
     A nil-typed value is the same, `(void)0` standing in for no text.
   - A pointer-backed builtin (the handles, the arrays, the hashes, the
     String handle, OpenStruct) boxes NULL as nil through
     sp_box_nullable_obj. sp_box_obj wrapped it in a truthy SP_TAG_OBJ that
     passed `unless x` and then crashed on the first read (#2992, #4134).
   - An Integer slot can hold the nil sentinel a nilable read left behind
     (`"a".rindex("/")`), so it boxes through a temp that tests for it; the
     temp keeps a side-effecting expression evaluated once. A Float slot's
     sentinel and a Bignum slot's NULL box as nil the same way (#4800).
   - A nested table boxes by reference, stamped with what its rows hold
     (#4486).
   A kind whose row has no box_text cannot be boxed, and stops the compile
   rather than fall back to an int box (the poly-box bug family). */
void emit_boxed_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (g_repr_check) { rc_text_form = -1; rc_text_ty = t; }
  const TyTraits *tr = ty_traits_of(t);
  if (tr) {
    if (!tr->box_text) {
      fprintf(stderr, "spinel: emit_boxed_text: cannot box type %d into a poly value\n", (int)t);
      exit(1);
    }
    if (t == TY_NIL && !(expr && *expr)) expr = "(void)0";
    RCT(tr->box_form);
    ty_traits_render(tr->box_text, expr, b);
    return;
  }
  if (ty_is_object(t)) {
    /* A reference-type object is a genuinely nilable C pointer (a hash/cache
       lookup or a method that can `return nil` -- e.g. doom's
       TextureManager#[] boxing build_composite's nullable result). Box via
       sp_box_nullable_obj so a NULL pointer becomes a proper SP_TAG_NIL rather
       than a "truthy" SP_TAG_OBJ wrapping NULL v.p, which passes `unless x` and
       then segfaults on the first field/method read (renderer draw_wall_column
       `texture.width`). A value-type object (a small Struct passed by value) is
       never NULL and `expr` is not a pointer, so the plain box is correct. */
    /* the class id the object carries when it may be a subclass's, by the
       rule emit_boxed follows (repr_dyn_cls): an exception keeps the static
       id, since its object starts with its class name, not an id */
    int dyn = repr_dyn_cls(c, t);
    if (comp_ty_value_obj(c, t))
      buf_printf(b, "sp_box_vobj_%s(%s)", c->classes[ty_object_class(t)].c_name, expr);
    else
      buf_printf(b, "sp_box_nullable_obj%s((void *)(%s), %d)",
                 dyn ? "_dyn" : "", expr, ty_object_class(t));
    RCT(comp_ty_value_obj(c, t) ? RF_VOBJ : dyn ? RF_NULLABLE_DYN : RF_NULLABLE);
    return;
  }
  /* an object array boxes by reference, stamped with its element class */
  if (ty_is_obj_array(t)) { RCT(RF_PTR_ARRAY); buf_printf(b, "sp_box_ptr_array_k((void *)(%s), %s)", expr, ptr_array_stamp(c, t)); return; }
  fprintf(stderr, "spinel: emit_boxed_text: cannot box type %d into a poly value\n", (int)t);
  exit(1);
}

/* The form emit_boxed_text records for kind t under --repr-check, as if an
   emit_boxed had called it; the C it writes is dropped and every static it
   touches is put back. ty_traits_check.c reads the box_form column off it. */
int emit_boxed_text_form(Compiler *c, TyKind t) {
  int sv_check = g_repr_check, sv_depth = rc_depth, sv_form = rc_text_form, sv_tmp = g_tmp;
  TyKind sv_ty = rc_text_ty;
  g_repr_check = 1; rc_depth = 1;
  Buf b; memset(&b, 0, sizeof b);
  emit_boxed_text(c, t, "$e", &b);
  free(b.p);
  int form = rc_text_form;
  g_repr_check = sv_check; rc_depth = sv_depth; rc_text_form = sv_form; rc_text_ty = sv_ty; g_tmp = sv_tmp;
  return form < 0 ? 0 : form;
}

/* Emit `expr` (a poly value) unboxed to its concrete C representation.

   A builtin kind reads its ty_traits row's unbox (types.c). The rows are
   not all a pointer cast, for these reasons:
   - String goes through sp_poly_unbox_s, not a bare `.v.s`: a mutable
     String's box holds its handle there.
   - Bignum goes through sp_poly_as_bigint: a poly slot holds a small Integer
     inline, and the cast read it as an sp_Bigint pointer (#4590).
   - Time, Process::Tms, Rational, Complex and the Range kinds are by-value
     structs boxed behind a heap copy, so they unbox by dereferencing; the
     cast turned a pointer straight into a struct (#3186, #3619).
   - Class is a by-value struct, read by sp_unbox_class (#2797).
   - Integer goes through sp_poly_as_int_or_nil: a boxed nil becomes the
     slot's SP_INT_NIL, never the 0 lying under the nil tag. Integer nil
     always uses the sentinel, whichever path unboxes it.
   - Float likewise goes through sp_poly_as_float_or_nil: a boxed nil becomes
     the sp_float_nil() NaN, never the 0.0 under the tag. A real NaN keeps
     its own bits, which sp_float_is_nil tells apart.
   - The Int, Float and String arrays, the nested tables and the poly-valued
     hashes are distinct C structs, so a boxed value of another kind is
     converted by its sp_poly_as_* entry, which hands back the pointer when
     the kind already matches; the cast read another struct's header
     (#3998, #4424, #4486). */
void emit_unbox_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  const TyTraits *tr = ty_traits_of(t);
  if (tr) { ty_traits_render(tr->unbox, expr, b); return; }
  /* an object array of this class is handed back itself; any other array
     is copied element by element, each checked against the class (#4486) */
  if (ty_is_ptr_array(t))  { buf_printf(b, "sp_poly_as_ptr_array(%s, %s)", expr, ptr_array_stamp(c, t)); return; }
  /* A boxed value read as a class-typed pointer: the tag and class are
     checked, because the alternative is reading another type's memory through
     the cast. A poly ivar that held an Array on one path and a Relation on
     another was passed to a parameter inference had left at Relation, and the
     Array's header was read as a table name (#4437). nil stays NULL, which is
     how an object pointer encodes it. */
  if (ty_is_object(t)) {
    int oc = ty_object_class(t);
    const char *rn = class_ruby_name(c, oc);
    /* a value-type instance is a by-value struct boxed behind a heap copy
       (sp_box_vobj_<C>), so it unboxes by dereferencing */
    int vobj = comp_ty_value_obj(c, t);
    buf_printf(b, "%s(%s *)sp_poly_unbox_cls(%s, %d, ", vobj ? "(*" : "", class_ctype(c, oc), expr, oc);
    emit_str_literal(b, rn ? rn : c->classes[oc].name);
    buf_puts(b, vobj ? "))" : ")");
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
   unguarded form for the many sites that have, except for Integer and
   Float, whose unbox is the guarded one on every path. */
void emit_unbox_nilable_text(Compiler *c, TyKind t, const char *expr, Buf *b) {
  /* a builtin kind's ty_traits row's unbox_nil (types.c): the guarded
     sp_poly_as_int_or_nil / sp_poly_as_float_or_nil, the unbox otherwise */
  const TyTraits *tr = ty_traits_of(t);
  if (tr) { ty_traits_render(tr->unbox_nil, expr, b); return; }
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
  /* a fresh object (`xs.first(C.new)`) is held by nothing but the argument
     while its conversion allocates: root it first */
  if (!comp_ty_value_obj(c, t) && !expr_is_held_ref(c, node)) {
    int tr = ++g_tmp;
    buf_printf(b, "({ sp_%s *_t%d = (sp_%s *)(", c->classes[def].c_name, tr, c->classes[def].c_name);
    emit_expr(c, node, b);
    buf_printf(b, "); SP_GC_ROOT(_t%d); sp_%s_%s(_t%d); })", tr, c->classes[def].c_name, mc(conv), tr);
    if (unbox) buf_puts(b, ")");
    return;
  }
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

/* An empty `[]` or `{}`, or an `Array.new` / `Hash.new`, whose element type
   nothing settles: it keeps TY_UNKNOWN to the end, yet is emitted as an Array
   or a Hash all the same, so in a scalar slot it is the same mismatch as a
   typed one ("abc"[[]], :sym[Array.new]). NULL for anything else. */
static const char *unsettled_container_cls(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_ArrayNode || k == NK_HashNode) {
    int n = 0; nt_arr(nt, node, "elements", &n);
    return n ? NULL : k == NK_ArrayNode ? "Array" : "Hash";
  }
  const char *nm = k == NK_CallNode ? nt_str(nt, node, "name") : NULL;
  int r = nm && sp_streq(nm, "new") ? nt_ref(nt, node, "receiver") : -1;
  const char *rn = r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode ? nt_str(nt, r, "name") : NULL;
  if (rn && (sp_streq(rn, "Array") || sp_streq(rn, "Hash"))) return rn;
  return NULL;
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
    const char *cn = t == TY_UNKNOWN ? unsettled_container_cls(c, node) : conv_wrong_cls_name(t);
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
  /* `*a` forwarded into a scalar int slot (a builtin arg): the one slot takes
     the splat's one element. A splat of any other length is a different call
     -- `a.slice(*[0, 2])` is slice(0, 2) -- which splat_dispatch_on_length
     routes to its own arm where the builtin has one; what reaches here with
     another length raises CRuby's arity error rather than reading element 0
     and dropping the rest. The arity is the one the dispatch recorded on the
     splat (`splat_lo`/`splat_hi`), else this slot's own 1. */
  if (nty && sp_streq(nty, "SplatNode")) {
    int inner = nt_ref(c->nt, node, "expression");
    long long lo = nt_int(c->nt, node, "splat_lo", 1), hi = nt_int(c->nt, node, "splat_hi", 1);
    int ta = ++g_tmp;
    buf_printf(b, "({ sp_PolyArray *_t%d = sp_poly_to_poly_array(sp_splat_to_array(", ta);
    if (inner >= 0) emit_boxed(c, inner, b); else buf_puts(b, "sp_box_nil()");
    buf_printf(b, ")); if (_t%d->len != 1) sp_raise_arity(_t%d->len, %lld, %lld, 0);"
                  " sp_poly_to_i_or_nil(sp_PolyArray_get(_t%d, 0)); })", ta, ta, lo, hi, ta);
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
  /* A Rational or a Complex converts through #to_int too, which the struct
     went into the slot without: a Rational truncates toward zero, and a
     Complex answers its real part only when its imaginary part is an exact
     zero (sp_complex_to_int). */
  if (comp_ntype(c, node) == TY_RATIONAL) {
    int tq = ++g_tmp;
    buf_printf(b, "({ sp_Rational _t%d = ", tq); emit_expr(c, node, b);
    buf_printf(b, "; _t%d.num / _t%d.den; })", tq, tq);
    return;
  }
  if (comp_ntype(c, node) == TY_COMPLEX) {
    buf_puts(b, "sp_complex_to_int("); emit_expr(c, node, b); buf_puts(b, ")");
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
  Buf tmp; memset(&tmp, 0, sizeof tmp);
  emit_expr(c, node, &tmp);
  if (!coerce_const_raise(tmp.p ? tmp.p : "", "0", b))
    emit_coerce_text(c, node, store_value_kind(c, node), TY_INT, CO_CONVERT, tmp.p ? tmp.p : "",
                     "an Integer operand", b);
  free(tmp.p);
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

/* An Integer Range literal's endpoint: a nil that arrives at run time is the
   absent bound `none` (INTPTR_MIN for a beginning, INTPTR_MAX for an end), as
   the written `nil..5` / `1..nil` are. Anything else is read as before. */
void emit_range_endpoint(Compiler *c, int node, const char *none, Buf *b) {
  TyKind t = comp_ntype(c, node);
  if (t == TY_POLY) {
    buf_puts(b, "sp_poly_range_bound("); emit_expr(c, node, b); buf_printf(b, ", (sp_int)(%s))", none);
    return;
  }
  if (t == TY_INT && nullable_int_value(c, node)) {
    int tn = ++g_tmp;
    buf_printf(b, "({ sp_int _t%d = ", tn);
    emit_int_expr_ex(c, node, 0, b);
    buf_printf(b, "; _t%d == SP_INT_NIL ? (sp_int)(%s) : _t%d; })", tn, none, tn);
    return;
  }
  emit_int_expr_nilable(c, node, b);
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
    emit_coerce_text(c, node, store_value_kind(c, node), TY_FLOAT, CO_CONVERT, tmp.p ? tmp.p : "",
                     "a Float operand", b);
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

/* See codegen_internal.h. */
void emit_split_pre(Compiler *c, int node, void (*emit)(Compiler *, int, Buf *), Buf *pre, Buf *val) {
  Buf *sv = g_pre; g_pre = pre;
  emit(c, node, val);
  g_pre = sv;
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
  else emit_coerce_text(c, node, store_value_kind(c, node), TY_STRING, CO_HOLD, txt, "a String operand", b);
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

/* A line reader's separator: nil (NULL) reads to the end and "" is paragraph
   mode, so a boxed nil must not come out as "" as it does in a String slot. */
void emit_str_expr_sep(Compiler *c, int node, Buf *b) {
  if (yield_site_type(c, node) != TY_POLY) { emit_str_expr_nilable(c, node, b); return; }
  int tmp; Buf *hb = conv_hold_begin(b, &tmp);
  Buf *ob = hb ? hb : b;
  buf_puts(ob, "sp_poly_sep_str(");
  emit_expr(c, node, ob); buf_puts(ob, ")");
  if (hb) conv_hold_end(tmp);
}

/* Is `node` a call bound to a user method whose C function is `void`
   (method_is_void)? Bound the way the direct-call emitters bind it: a
   receiverless call through the enclosing self, `Const.m` through the class
   methods, an object receiver through its class -- where every override below
   that class has to answer no value too, or the dispatch carries one. A
   yielding method is spliced at the call and gives the splice's value.
   Parentheses around the call (`push((table(x)))`) are peeled. */
int call_answers_no_value(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  node = unwrap_parens(c, node);
  if (node < 0 || nt_kind(nt, node) != NK_CallNode) return 0;
  const char *nm = nt_str(nt, node, "name");
  if (!nm) return 0;
  int recv = nt_ref(nt, node, "receiver");
  int mi = -1, cid = -1;
  if (recv < 0) {
    mi = comp_self_call_mi(c, node, nm);
    Scope *self = comp_scope_of(c, node);
    if (mi >= 0 && c->scopes[mi].class_id >= 0 && !c->scopes[mi].is_cmethod && self)
      cid = self->class_id;
  }
  else if (nt_kind(nt, recv) == NK_ConstantReadNode || nt_kind(nt, recv) == NK_ConstantPathNode) {
    int ci = comp_class_index(c, nt_str(nt, recv, "name"));
    if (ci >= 0) mi = comp_cmethod_in_chain(c, ci, nm, NULL);
  }
  else if (ty_is_object(comp_ntype(c, recv))) {
    cid = ty_object_class(comp_ntype(c, recv));
    mi = comp_method_in_chain(c, cid, nm, NULL);
  }
  if (mi < 0 || c->scopes[mi].yields || !method_is_void(&c->scopes[mi])) return 0;
  for (int k = 0; cid >= 0 && k < c->nclasses; k++) {
    if (!is_descendant(c, k, cid)) continue;
    int kmi = comp_method_in_chain(c, k, nm, NULL);
    if (kmi >= 0 && (c->scopes[kmi].yields || !method_is_void(&c->scopes[kmi]))) return 0;
  }
  return 1;
}

/* Coerce an unresolved-call value into a concretely-typed slot. An unresolved
   call is typed TY_UNKNOWN and lowers to the gate's sp_raise_nomethod(...) poly
   token (an sp_RbVal that always raises); when it lands in a non-poly slot the
   emitted C would assign sp_RbVal to a scalar/pointer. The token never returns,
   so any type-correct wrapper keeps the C compiling: coerce it to `target`.
   A value that is NOT the token is emitted raw -- callers reach this only for a
   TY_UNKNOWN RHS, where a raw emit is exactly the prior behavior. Returns 1 if
   it coerced the token, 0 if it emitted raw. */
/* The unresolved-call token's conversion into a typed slot (a printf
   format of the token's text), or NULL for emit_unbox_text's */
const char *token_unbox_fmt(TyKind target) {
  const TyTraits *tr = ty_traits_of(target);   /* the unbox_token column (types.c) */
  return tr ? tr->unbox_token : NULL;
}

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
    const char *tf = token_unbox_fmt(target);
    if (tf) buf_printf(b, tf, txt);
    else emit_unbox_text(c, target, txt, b);  /* pointer/object/hash slot */
  }
  else if (is_cls_tok && target != TY_POLY && target != TY_UNKNOWN) {
    buf_printf(b, "({ (void)%s; %s; })", txt, default_value_from_compiler(c, target));
    is_tok = 1;
  }
  /* A call to a method that answers no value is a `void` C call: raw, it put
     a void expression in the typed slot. Its node is TY_UNKNOWN when the
     method ends in the raise of something defined nowhere (`Styler.render`),
     which never returns. Evaluate it and hand the slot its nil, as the boxed
     slots already do with `(call, sp_box_nil())`. */
  else if (target != TY_UNKNOWN && call_answers_no_value(c, node)) {
    buf_printf(b, "((void)(%s), %s)", txt, raise_tail_value_c(c, target));
    is_tok = 1;
  }
  else {
    store_check(c, node, target, "a store of an untyped value", b);
    buf_puts(b, txt);
  }
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
  if (nty && sp_streq(nty, "LocalVariableReadNode")) return repr_local_nullable_int(c, node);
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
  if (is_call_or_yield(nm)) {
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
     (#3288). box_int_or_nil is a no-op on a real int. A float ivar holds
     sp_float_nil() the same way, and boxed plainly it printed NaN. */
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
          TyKind ivt2 = iv2 >= 0 ? c->classes[defc2 >= 0 ? defc2 : cid2].ivar_types[iv2] : TY_UNKNOWN;
          if (ivt2 == TY_INT || ivt2 == TY_FLOAT) return 1;
        }
      }
    }
  }
  return 0;
}

/* emit_boxed's box functions: a value boxed as it is (ty_box_fn), and one
   whose slot can hold its nil (ty_box_nil_fn). NULL: no function box. */
const char *ty_box_fn(TyKind t) {
  const TyTraits *tr = ty_traits_of(t);   /* the box column (types.c) */
  return tr ? tr->box : NULL;
}
const char *ty_box_nil_fn(TyKind t) {
  const TyTraits *tr = ty_traits_of(t);   /* the box_nil column (types.c) */
  return tr ? tr->box_nil : NULL;
}

/* A shared-mutable String's box, by where its handle comes from
   (repr_of's strbuf_src). */
static void emit_boxed_strbuf(Compiler *c, int node, TyKind t, const Repr *rp, Buf *b) {
  if (t == TY_STRING) {
    /* The node-type cache is finalized before the late handle passes run,
       so a local promoted to a shared handle still reads as String here
       while its C slot is an sp_String *. Box the HANDLE: sp_box_str would
       hand the callee a copy, and the append would land in the copy (the
       mixed case -- one callee reached with a reader argument and a plain
       local -- printed the unchanged string). An argument that already ran
       is the handle it read then: the slot read here is late, after a later
       argument may have rebound it (`m(s, *xs, (s = +"q"; 1))` packed "q"
       where CRuby passes the String s held first). */
    char srefS[1024];
    int th = ran_first_handle(node);
    if (th >= 0) {
      buf_printf(b, "sp_box_nullable_obj(_t%d, SP_BUILTIN_STRBUF)", th);
      RC(RF_STRBUF_HANDLE, RW_RAN_FIRST);
      return;
    }
    strbuf_slot_ref(c, node, srefS, sizeof srefS);
    buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", srefS);
    RC(RF_STRBUF_HANDLE, RW_NONE);
    return;
  }
  NodeKind k = nt_kind(c->nt, node);
  if (rp->strbuf_src == RS_HANDLE && k == NK_LocalVariableReadNode) {
    /* a marked container-store read of a shared-mutable string: box the
       sp_String* HANDLE so later in-place mutation is visible through the
       container (#3227 phase 3) */
    const char *bn0 = nt_str(c->nt, node, "name");
    Scope *bs0 = comp_scope_of(c, node);
    LocalVar *blv0 = bs0 ? scope_local(bs0, bn0) : NULL;
    int th0 = blv0 && blv0->type == TY_STRBUF && blv0->str_shared ? ran_first_handle(node) : -1;
    if (th0 >= 0) {   /* ran first: the handle it read then, as above */
      buf_printf(b, "sp_box_nullable_obj(_t%d, SP_BUILTIN_STRBUF)", th0);
      RC(RF_STRBUF_HANDLE, RW_RAN_FIRST);
      return;
    }
    /* through emit_local_ref: a local a proc captures is its cell's handle
       (`f.call(k: s)` beside `-> { s }` named an lv_s nothing declared); a
       handle local holds NULL for nil (a nil argument bound to a parameter
       that is the handle): box that as nil */
    buf_puts(b, "sp_box_nullable_obj(");
    emit_local_ref(c, node, bn0, b);
    buf_puts(b, ", SP_BUILTIN_STRBUF)");
    RC(RF_STRBUF_HANDLE, RW_NONE);
    return;
  }
  if (rp->strbuf_src == RS_HANDLE && k == NK_InstanceVariableReadNode) {
    char srefX[192];
    if (strbuf_slot_ref(c, node, srefX, sizeof srefX)) {
      buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", srefX);
      RC(RF_STRBUF_HANDLE, RW_NONE);
      return;
    }
  }
  /* An ivar WRITE in value position lowers to `({ iv_x = ...; iv_x; })`,
     so its value IS the slot -- the same handle the read above boxes, not
     a string to wrap a fresh handle around (#3993). */
  if (rp->strbuf_src == RS_HANDLE && k == NK_InstanceVariableWriteNode) {
    buf_puts(b, "sp_box_obj(");
    int sv_mw = view_push_repr(c, node, VR_STRBUF_BOX, 0);
    emit_expr(c, node, b);
    view_pop(c, sv_mw);
    buf_puts(b, ", SP_BUILTIN_STRBUF)");
    RC(RF_STRBUF_HANDLE, RW_NONE);
    return;
  }
  /* an element read is ALREADY a boxed handle when the container holds
     one: pass it through (as a handle box either way) so the alias keeps
     the container's own string rather than a fresh copy of it (#3941) */
  if (rp->strbuf_src == RS_ELEM) {
    buf_puts(b, "sp_box_obj(sp_poly_as_strbuf(");
    int sv_m2 = view_push_repr(c, node, VR_STRBUF_BOX, 0);
    emit_expr(c, node, b);
    view_pop(c, sv_m2);
    buf_puts(b, "), SP_BUILTIN_STRBUF)");
    RC(RF_STRBUF_ELEM, RW_NONE);
    return;
  }
  /* A reader call whose ivar is now the shared handle already emits the
     sp_String * itself, so box THAT handle. Wrapping a fresh one around
     it fed sp_String_new_shared (a const char * value) a handle, and the
     C build stopped -- the mixed case where one callee is reached with
     both a reader argument and a container element. */
  if (rp->strbuf_src == RS_DEMANDED) {
    char sref_h[1024];
    int sv_mh = view_push_repr(c, node, VR_STRBUF_BOX, 1);
    int got_h = strbuf_slot_ref(c, node, sref_h, sizeof sref_h);
    view_pop(c, sv_mh);
    if (got_h) {
      buf_printf(b, "sp_box_nullable_obj(%s, SP_BUILTIN_STRBUF)", sref_h);
      RC(RF_STRBUF_HANDLE, RW_NONE);
      return;
    }
  }
  /* a demanded literal / expression store: wrap a FRESH handle so the
     container element is mutable in place (#3227 P3) */
  buf_puts(b, "sp_box_obj(sp_String_new_shared(");
  { Buf eb0; memset(&eb0, 0, sizeof eb0);
    int sv_mark = view_push_repr(c, node, VR_STRBUF_BOX, 0);   /* emit the plain string value */
    emit_str_expr(c, node, &eb0);
    view_pop(c, sv_mark);
    buf_puts(b, eb0.p ? eb0.p : "(&(\"\\xff\")[1])");
    free(eb0.p); }
  buf_puts(b, "), SP_BUILTIN_STRBUF)"); RC(RF_STRBUF_FRESH, RW_NONE);
}

static void emit_boxed_impl(Compiler *c, int node, Buf *b) {
  /* Parentheses are transparent: box the inner expression directly so a
     wrapped yield (`out << (yield x)`) reaches the per-call-site yield boxing
     below rather than being boxed by the shared node type (#2454). Not a
     call argument that already ran into a temp (the g_argov overrides name
     the parenthesized node): unwrapped, the inner expression ran again,
     `m(**(lg(h)))` beside keywords that run ahead running lg twice, and
     `m(x, (x = lg(2)))` into a boxed parameter printing twice. */
  {
    const char *pty = nt_type(c->nt, node);
    if (pty && sp_streq(pty, "ParenthesesNode") && !arg_ran_first(node, 0)) {
      int pbody = nt_ref(c->nt, node, "body"); int pbn = 0;
      const int *pbd = pbody >= 0 ? nt_arr(c->nt, pbody, "body", &pbn) : NULL;
      if (pbn == 1) { emit_boxed(c, pbd[0], b); return; }
    }
  }
  if (nt_kind(c->nt, node) == NK_LocalVariableReadNode && comp_ntype(c, node) == TY_STRING &&
      c->poly_strbuf_lift[node])
    unsupported_feature(c, node, "a String is not yet shared by reference through a narrowed boxed iterator element into an appending parameter");
  {
    const char *bty0 = nt_type(c->nt, node);
    /* `*x` in a boxed value position (break *x / next *x): Ruby's
       splat-to-array (nil -> [], array -> itself, scalar -> [v]). */
    if (bty0 && sp_streq(bty0, "SplatNode")) {
      int inner0 = nt_ref(c->nt, node, "expression");
      buf_puts(b, "sp_splat_to_array(");
      if (inner0 >= 0) emit_boxed(c, inner0, b); else buf_puts(b, "sp_box_nil()");
      buf_puts(b, ")");
      RC(RF_SPECIAL, RW_LITERAL);
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
    buf_puts(b, "((void)");
    { Buf rb; memset(&rb, 0, sizeof rb); emit_yblk_ref(&rb);
      emit_proc_yield(c, rb.p ? rb.p : "NULL", yargc, yargv, b); free(rb.p); }
    buf_puts(b, ", _sp_proc_poly_ret)");
    RC(RF_YIELD, RW_YIELD);
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
    RC(RF_YIELD, RW_YIELD);
    return;
  }
  if (nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode") && g_block_id < 0) {
    /* An unguarded yield with no block raises LocalJumpError. A guarded yield
       folds its guard to a compile-time false and sits inside an `if (0)`, so
       the raise never executes there; the boxed nil keeps the comma expression
       well-typed for the value position. */
    buf_puts(b, "(sp_exc_stage_key(sp_box_str((&(\"\\xff\" \"noreason\")[1]))), "
                "sp_raise_cls(\"LocalJumpError\", \"no block given (yield)\"), sp_box_nil())");
    RC(RF_SPECIAL, RW_YIELD);
    return;
  }
  if (g_block_id >= 0 && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "YieldNode")) {
    /* A forwarding block (`wrap { yield }` handing its own block on) has a
       yield for its tail, and that node's cached type is one site's too:
       the value at THIS site is what the block one level out answers, the
       one the splice will run (#4495), and a tail yield there answers what
       the block a level further out does (yield_block_out). */
    int tblk = g_block_id;
    TyKind bt = TY_NIL;
    for (int depth = 0; tblk >= 0; depth++) {
      int bbody = nt_ref(c->nt, tblk, "body");
      int bn = 0; const int *bb = bbody >= 0 ? nt_arr(c->nt, bbody, "body", &bn) : NULL;
      if (bn <= 0) { bt = TY_NIL; break; }
      int tail = bb[bn - 1];
      if (nt_type(c->nt, tail) && sp_streq(nt_type(c->nt, tail), "YieldNode") &&
          yield_block_out(depth + 1) >= 0) { tblk = yield_block_out(depth + 1); continue; }
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
      if (bt == TY_POLY) { emit_expr(c, node, b); RC(RF_PASS, RW_YIELD); return; }
      Buf yb; memset(&yb, 0, sizeof yb);
      emit_expr(c, node, &yb);
      const char *yt = yb.p ? yb.p : "0";
      /* The invoke emitter boxes an object tail into a poly slot, so for that
         shape the splice above already yielded an sp_RbVal -- re-boxing would
         cast a struct through (void *) (#3329). */
      int pre_boxed = (t == TY_POLY && ty_is_object(bt));
      if (bt == TY_NIL || bt == TY_VOID) {
        buf_printf(b, "({ %s; sp_box_nil(); })", yt);
        RC(RF_NIL_EFFECT, RW_YIELD);
      }
      else if (pre_boxed) {
        buf_puts(b, yt);
        RC(RF_PASS, RW_YIELD);
      }
      else {
        emit_boxed_text(c, bt, yt, b);
        RC_TEXT(RW_YIELD);
      }
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
        int sv_m0 = view_push_repr(c, node, VR_STRBUF_BOX, 0);
        emit_expr(c, node, &ib);
        view_pop(c, sv_m0);
        emit_boxed_text(c, ft0, ib.p ? ib.p : "0", b);
        RC_TEXT(RW_TRANSPLANT);
        free(ib.p);
        return;
      }
    }
  }
  /* an empty array literal [] has TY_UNKNOWN; box it as an empty PolyArray so
     it can hold any element type when stored into a poly slot */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "ArrayNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_poly_array(sp_PolyArray_new())"); RC(RF_SPECIAL, RW_LITERAL); return; }
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
      buf_puts(b, "sp_box_poly_array("); emit_expr(c, node, b); buf_puts(b, ")"); RC(RF_SPECIAL, RW_LITERAL); return;
    }
  }
  /* an empty hash literal {} has TY_UNKNOWN; box it as an empty PolyPolyHash */
  if (t == TY_UNKNOWN && nt_type(c->nt, node) && sp_streq(nt_type(c->nt, node), "HashNode")) {
    int _ne = 0; nt_arr(c->nt, node, "elements", &_ne);
    if (_ne == 0) { buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)"); RC(RF_SPECIAL, RW_LITERAL); return; }
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
      /* an unknown keyword is CRuby's ArgumentError, not the default */
      if (emit_hash_new_capacity_wrap(c, node, b, 1)) { RC(RF_SPECIAL, RW_LITERAL); return; }
      if (emit_hash_new_arg_guard(c, node, b)) { RC(RF_SPECIAL, RW_LITERAL); return; }
      if (han == 1 && hav) {
        buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new_with_default(");
        emit_boxed(c, hav[0], b);
        buf_puts(b, "), SP_BUILTIN_POLY_POLY_HASH)");
      }
      else buf_puts(b, "sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH)");
      RC(RF_SPECIAL, RW_LITERAL);
      return;
    }
  }
  /* The rest dispatches on the value's representation (repr.h), the one
     place the flags beside its type are read: whether a scalar can hold
     its nil sentinel, where a shared String's handle comes from, whether
     an object reads its class id from itself. */
  Repr rp = repr_of(c, node);
  switch ((ReprKind)rp.kind) {
  case RK_BOXED:
    /* a handle-marked read of a local that settled POLY already holds a
       boxed value -- wrapping it as a raw handle would reinterpret an
       sp_RbVal as sp_String* (#3325) */
    if (rp.strbuf_src == RS_SLOT_POLY) {
      buf_printf(b, "lv_%s", rename_local(nt_str(c->nt, node, "name")));
      RC(RF_PASS, RW_NONE);
      return;
    }
    emit_expr(c, node, b);
    RC(RF_PASS, RW_NONE);
    return;
  case RK_SENTINEL:
    /* The Integer or Float slot can hold its nil sentinel (repr_nil_scalar:
       a nilable builtin's answer, an ivar read, a parameter bound from one,
       a local assigned one, a Ruby-defined builtin, every Integer under
       --int-overflow=promote), so boxing it has to yield nil (#3493,
       #5085). Elsewhere a real number is never the sentinel and the plain
       box is the hot path (every Integer into a poly slot). */
    buf_printf(b, "%s(", ty_box_nil_fn(t == TY_FLOAT ? TY_FLOAT : TY_INT));
    emit_expr(c, node, b);
    buf_puts(b, ")");
    RC(t == TY_FLOAT ? RF_FLT_NIL : RF_INT_NIL, RW_NONE);
    return;
  case RK_SCALAR: {
    if (t == TY_NIL) {
      const char *nty = nt_type(c->nt, node);
      if (nty && sp_streq(nty, "NilNode")) { buf_puts(b, "sp_box_nil()"); RC(RF_NIL_EFFECT, RW_NONE); return; }
      /* a nil-typed expression can still have side effects (e.g. a void-valued
         block call): evaluate it for effect, then yield nil */
      buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())");
      RC(RF_NIL_EFFECT, RW_NONE);
      return;
    }
    const char *fn = ty_box_fn(t == TY_INT || t == TY_FLOAT || t == TY_BOOL ? t : TY_SYMBOL);
    buf_printf(b, "%s(", fn);
    emit_expr(c, node, b);
    buf_puts(b, ")");
    RC(t == TY_INT ? RF_INT : t == TY_FLOAT ? RF_FLT : t == TY_BOOL ? RF_BOOL : RF_SYM, RW_NONE);
    return;
  }
  case RK_STRUCT: {
    const char *fn = ty_box_fn(t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_STR_RANGE ||
                               t == TY_TMS || t == TY_TIME || t == TY_COMPLEX || t == TY_RATIONAL
                               ? t : TY_CLASS);
    buf_printf(b, "%s(", fn);
    emit_expr(c, node, b);
    buf_puts(b, ")");
    RC(RF_STRUCT, RW_NONE);
    return;
  }
  case RK_VOBJ:
    /* a value-type object is never NULL and is not a pointer */
    buf_printf(b, "sp_box_vobj_%s(", c->classes[ty_object_class(t)].c_name);
    emit_expr(c, node, b);
    buf_puts(b, ")");
    RC(RF_VOBJ, RW_NONE);
    return;
  case RK_STRBUF:
    emit_boxed_strbuf(c, node, t, &rp, b);
    return;
  case RK_PTR:
    break;
  case RK_NONE:
  default:
    /* TY_UNKNOWN (e.g. unrecognized stdlib class .new): evaluate for side-effects, yield nil */
    buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())"); RC(RF_NIL_EFFECT, RW_NONE); return;
  }
  /* A pointer: NULL is nil. Reference-backed builtins (IO/Fiber/Thread/
     Queue/Mutex/ConditionVariable/Enumerator/Exception/Proc/Method/Regexp/
     Random/ARGF) box NULL as nil via sp_box_nullable_obj, not a truthy
     SP_TAG_OBJ over NULL. */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) {
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_printf(b, "), %s)", nbid);
      RC(RF_NULLABLE, RW_NONE);
      return;
    } }
  if (ty_is_object(t)) {
    /* A reference-type object is a nilable C pointer (a hash/cache lookup or a
       method that can `return nil`, e.g. doom's TextureManager#[] boxing
       build_composite's nullable result). Box via sp_box_nullable_obj so a
       NULL pointer becomes SP_TAG_NIL rather than a truthy SP_TAG_OBJ over a
       NULL v.p (which passes `unless x` then segfaults on the first field
       read). A class with subclasses is only the STATIC type here: an
       inherited method boxing `self` would stamp the defining class, and the
       boxed value then dispatched as the parent (#3773), so the box reads the
       id the object carries (dyn_cls; an exception, whose object starts with
       its class name, keeps the static id: repr_dyn_cls). */
    buf_puts(b, rp.dyn_cls ? "sp_box_nullable_obj_dyn((void *)(" : "sp_box_nullable_obj((void *)(");
    emit_expr(c, node, b);
    buf_printf(b, "), %d)", ty_object_class(t));
    RC(rp.dyn_cls ? RF_NULLABLE_DYN : RF_NULLABLE, RW_NONE);
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
      RC(RF_NULLABLE, RW_NONE);
      return;
    }
    unsupported(c, node, "boxing value into poly"); RC(RF_SPECIAL, RW_NONE); return;
  }
  /* NULL is a bigint slot's nil (nil_value), and boxing it as a Bignum made
     a truthy Integer that printed 0 (#4800). Unconditional: a live Bignum is
     never the NULL pointer, so the test costs one compare on a path that
     already allocates, and no analysis has to prove nilability. */
  if (t == TY_BIGINT) {
    buf_printf(b, "%s(", ty_box_nil_fn(TY_BIGINT)); emit_expr(c, node, b); buf_puts(b, ")");
    RC(RF_BIGINT, RW_NONE);
    return;
  }
  if (t == TY_STRING) {
    buf_printf(b, "%s(", ty_box_fn(TY_STRING)); emit_expr(c, node, b); buf_puts(b, ")");
    RC(RF_STR, RW_NONE);
    return;
  }
  /* Array slots are nilable C pointers: a nil-defaulting param, a nullable
     ivar, or `[x] if cond` in value position is NULL. Box NULL as a proper
     nil, not a truthy OBJ wrapping NULL that passes `if x`/`unless x` and
     then segfaults on the first access (#3275). A non-nil array is never
     NULL, so the guard's untaken branch is free on the hot path. Matches
     emit_boxed_text's array cases. */
  { const char *aid = t == TY_INT_ARRAY ? "SP_BUILTIN_INT_ARRAY" : t == TY_FLOAT_ARRAY ? "SP_BUILTIN_FLT_ARRAY"
                    : t == TY_STR_ARRAY ? "SP_BUILTIN_STR_ARRAY" : t == TY_POLY_ARRAY ? "SP_BUILTIN_POLY_ARRAY"
                    : t == TY_OPENSTRUCT ? "SP_BUILTIN_OPENSTRUCT" : NULL;
    if (aid) {
      buf_puts(b, "sp_box_nullable_obj((void *)("); emit_expr(c, node, b);
      buf_printf(b, "), %s)", aid);
      RC(RF_NULLABLE, RW_NONE);
      return;
    } }
  /* a nested table or an object array boxes by reference, stamped with what
     its elements are, where it used to be refused (#4486) */
  if (ty_is_ptr_array(t)) {
    buf_puts(b, "sp_box_ptr_array_k((void *)("); emit_expr(c, node, b);
    buf_printf(b, "), %s)", ptr_array_stamp(c, t));
    RC(RF_PTR_ARRAY, RW_NONE);
    return;
  }
  /* a pointer kind with no box: evaluate for side-effects, yield nil */
  buf_puts(b, "("); emit_expr(c, node, b); buf_puts(b, ", sp_box_nil())"); RC(RF_NIL_EFFECT, RW_NONE);
}
/* emit_boxed: the boxing of node `node`'s value (emit_boxed_impl); under
   --repr-check it keeps the nesting the recorder reads */
void emit_boxed(Compiler *c, int node, Buf *b) {
  rc_depth++;
  emit_boxed_impl(c, node, b);
  rc_depth--;
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
    case TY_PROCESS_STATUS: buf_puts(&cty, c_type_name(t)); init = "NULL"; ptr = 1; break;
    case TY_OPENSTRUCT: buf_puts(&cty, "sp_OpenStruct *"); init = "NULL"; ptr = 1; break;
    case TY_STRING: buf_puts(&cty, "const char *"); init = "NULL"; ptr = 1; break;  /* nil until assigned (#3295) */
    case TY_POLY:   buf_puts(&cty, "sp_RbVal"); init = "sp_box_nil()"; break;
    case TY_CLASS:  buf_puts(&cty, "sp_Class"); init = "SP_CLASS_NIL"; break;
    default:
      if (comp_ty_value_obj(c, t)) { emit_ctype(c, t, &cty); init = "{0}"; ptr = 0; }
      else if (is_scalar_ret(t) && t != TY_UNKNOWN) { emit_ctype(c, t, &cty); init = "NULL"; ptr = 1; }
      else {
        fprintf(stderr, "spinel: local '%s' has unsupported type %s\n", lv->name, ty_name(t));
        exit(1);
      }
  }
  /* A local a `||=` writes, or one a read can reach before any write
     (maybe_unset), starts as Ruby nil, not as its type's zero: `x ||= v` has
     to be able to tell "never assigned" from "assigned 0", and the early read
     answers nil. The pointer kinds already start NULL; this is what gives the
     sentinel-carrying scalars the same footing (#3388). Block-locals are reset
     to nil_value on every iteration already (emit_block_locals_reset). */
  if ((lv->or_written || lv->maybe_unset) && !lv->is_param && !lv->is_block_param) {
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
    "=~", "!~", "match", "match?", "scan", "gsub", "gsub!", "sub", "sub!",
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
    /* gsub, sub and scan on a String set them for a String pattern too, and
       for a pattern that is a Regexp or a String only at run time, in a
       program that reads them */
    if (g_reads_match_regs && r >= 0 && an > 0 && av &&
        (sp_streq(nm, "gsub") || sp_streq(nm, "gsub!") || sp_streq(nm, "sub") ||
         sp_streq(nm, "sub!") || sp_streq(nm, "scan"))) {
      TyKind rt = comp_ntype(c, r), pt = comp_ntype(c, av[0]);
      if ((rt == TY_STRING || rt == TY_POLY) && (pt == TY_STRING || pt == TY_POLY)) return 1;
    }
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
          buf_printf(b, "    %s *_cell_%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, %s);\n", vs, lv->name, vs, vs,
                     cell_value_struct_scan(lv->type));
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
          lv->type != TY_UNKNOWN && !ptr_cell) {
        /* The top-level scope has no def node, and the refusal named no
           FILE:LINE at all; point it at the local's first write there. */
        int at = s->def_node;
        int nids = 0; const int *ids = at < 0 ? cg_scope_nodes(c, (int)(s - c->scopes), &nids) : NULL;
        for (int k = 0; k < nids && at < 0; k++) {
          const char *wn = nt_kind(c->nt, ids[k]) == NK_LocalVariableWriteNode
                         ? nt_str(c->nt, ids[k], "name") : NULL;
          if (wn && sp_streq(wn, lv->name)) at = ids[k];
        }
        unsupported(c, at, "closure capturing a non-integer variable (later slice)");
      }
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
  emit_scope_decls_ends(c, s, b, NULL);
}

/* emit_scope_decls, noting in ends[i] where local i's declaration text ends
   (a local that declares nothing ends where the one before it did). The top
   level's split (main_body_split) moves each declaration by these. */
void emit_scope_decls_ends(Compiler *c, Scope *s, Buf *b, size_t *ends) {
  int si = (int)(s - c->scopes);
  int has_begin = scope_has_begin(c, si);
  /* $~ and the $1.. globals derived from it are frame-local in Ruby: a match
     inside this method must not outlive it. The cleanup attribute puts the
     caller's registers back on every ordinary exit, early returns included. */
  if (s->name && s->def_node >= 0 && scope_performs_match(c, si))
    buf_puts(b, "    sp_re_frame _sp_rf SP_CLEANUP(sp_re_frame_pop);"
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
    if (ends && i > 0) ends[i - 1] = b->len;
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
    if (lv->is_param && has_begin && (all_vol || name_list_has(volnames, nvol, lv->name))) {
      /* a parameter the body reassigns inside a begin and reads after the
         rescue or a retry's longjmp: a volatile local copy of the incoming
         value (the signature names it lv_<name>__in) (#6552) */
      LocalVar cp = *lv; cp.is_param = 0;
      Buf d; memset(&d, 0, sizeof d);
      declare_local(c, &d, &cp, 1);
      /* declare_local initialises to the type's nil: take the argument */
      char *eq = d.p ? strstr(d.p, " = ") : NULL;
      if (eq) {
        char *semi = strchr(eq, ';');
        buf_printf(b, "%.*s = lv_%s__in%s", (int)(eq - d.p), d.p, lv->name, semi ? semi : ";\n");
      }
      free(d.p);
      continue;
    }
    if (lv->is_param) {
      /* A poly param is an sp_RbVal by value: root through the tagged
         RBVAL form so the collector reads the boxed pointer, not the
         struct's first word (the tag). */
      switch (lv->type) {
      case TY_POLY: buf_printf(b, "    SP_GC_ROOT_RBVAL(lv_%s);\n", lv->name); break;
      case TY_STR_RANGE:   /* two GC strings by value; see emit_local_decl */
        buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.first);\n", lv->name);
        buf_printf(b, "    SP_GC_ROOT_STR(lv_%s.last);\n", lv->name);
        break;
      case TY_STRING: buf_printf(b, "    SP_GC_ROOT_STR(lv_%s);\n", lv->name); break;   /* see emit_local_decl */
      default:
        if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) buf_printf(b, "    SP_GC_ROOT(lv_%s);\n", lv->name);
        break;
      }
    }
    else {
      /* A BLOCK parameter the analyzer never typed still needs storage: the
         loop emitter binds it, and an empty literal receiver leaves no element
         type to infer, so `[].each_with_index { |x, i| }` referenced an
         undeclared lv_x (#3853). A boxed slot is what the binding writes. */
      if (lv->type == TY_UNKNOWN && lv->is_block_param) lv->type = TY_POLY;
      int vol = has_begin && (all_vol || name_list_has(volnames, nvol, lv->name));
      declare_local(c, b, lv, vol);
    }
  }
  if (ends && s->nlocals > 0) ends[s->nlocals - 1] = b->len;
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
  int ans = class_reopen_cmethod(c, -1, s->name) == si, has_desc = 0;
  for (int k = 0; k < c->nclasses && !has_desc && !ans; k++)
    if (k != s->class_id && is_descendant(c, k, s->class_id)) has_desc = 1;
  if (has_desc) {
    for (int nid = 0; nid < c->nt->count && !ans; nid++) {
      if (c->nscope[nid] != si) continue;
      if (nt_kind(c->nt, nid) == NK_SelfNode) { ans = 1; break; }
      /* super in `self.new` constructs whichever class received the call */
      if (comp_super_is_class_new(c, nid)) { ans = 1; break; }
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
  int rc = recv_cls >= 0 || recv_cls <= -100 ? recv_cls : c->scopes[mi].class_id;
  if (rc == c->scopes[mi].class_id && class_reopen_cmethod(c, -1, c->scopes[mi].name) == mi) rc = builtin_class_id("Class");
  buf_printf(b, "((sp_Class){%d, NULL})", rc);
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
/* The C arguments a zero-argument call of this #to_a takes after self: an
   empty rest array for a `*rest` parameter and no block for a `&blk` one --
   the forwarding shape `delegate :to_a, to: :@m` produces. Answers 0 when the
   method takes anything that cannot be defaulted here (a required, optional or
   keyword parameter, a class method), so the caller skips it. */
static int obj_to_a_call_tail(Scope *s, char *out, size_t sz) {
  if (s->nrequired != 0 || s->is_cmethod || s->kwrest_idx >= 0) return 0;
  int rest_only = s->rest_idx >= 0 && s->nparams == 1;
  if (s->nparams != 0 && !rest_only) return 0;
  snprintf(out, sz, "%s%s", rest_only ? ", sp_PolyArray_new()" : "", s->blk_param ? ", NULL" : "");
  return 1;
}

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
  /* an #each reopened on Object reaches every instance, but a user class's
     chain does not name Object: the poly dispatch serves it as its default
     arm, and the normalization walked the object as an empty container
     (#5101) */
  if (mi < 0) {
    int oc = comp_class_index(c, "Object");
    int od = -1;
    if (oc >= 0 && oc != k) {
      mi = comp_method_in_chain(c, oc, "each", &od);
      if (od != oc) mi = -1;
    }
  }
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
  /* a method of Object reached from outside the class's own chain */
  const char *dn = c->classes[c->scopes[mi].class_id].c_name;
  if (sp_streq(dn, "Object")) cn = dn;
  if (sp_streq(cn, "Object") || sp_streq(cn, "Array") || sp_streq(cn, "Numeric"))
    buf_printf(b, "_t%d", tv);
  else
    buf_printf(b, "(sp_%s *)_t%d.v.p", cn, tv);
}

/* A boxed user object reaching the builtin iteration of `name` (each, map,
   any?, ...) whose class answers neither #each nor #to_a nor the name
   itself: CRuby raises NoMethodError, where the walk read it as an empty
   container and the block never ran. */
void emit_poly_iter_obj_reject(Compiler *c, int tv, const char *name, Buf *b) {
  Buf arms; memset(&arms, 0, sizeof arms);
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->instantiated || ci->is_native_class || is_builtin_reopen(ci->name)) continue;
    int bp = class_builtin_superclass(c, k);
    if (bp != -116 && bp != -117 && bp != -146) continue;
    if (comp_method_in_chain(c, k, "each", NULL) >= 0 ||
        comp_method_in_chain(c, k, "to_a", NULL) >= 0 ||
        comp_method_in_chain(c, k, name, NULL) >= 0 ||
        comp_method_in_chain(c, k, "method_missing", NULL) >= 0) continue;
    buf_printf(&arms, " case %d:", k);
  }
  if (arms.p && arms.p[0])
    buf_printf(b, "if (_t%d.tag == SP_TAG_OBJ) switch (_t%d.cls_id) {%s sp_raise_poly_nomethod(\"%s\", _t%d); default: break; }\n",
               tv, tv, arms.p, name, tv);
  free(arms.p);
}

void emit_poly_iter_obj_normalize(Compiler *c, int tv, Buf *b) {
  Buf arms; memset(&arms, 0, sizeof arms);
  for (int k = 0; k < c->nclasses; k++) {
    /* a never-instantiated class can't be the runtime class (#1608) */
    if (!c->classes[k].instantiated) continue;
    int mi = comp_method_in_chain(c, k, "to_a", NULL);
    /* a to_a forwarding its block has a proc-form clone; that is the emitted
       function for a call from poly dispatch (#3399) */
    { int pfi = mi >= 0 ? scope_proc_form_of(c, mi) : -1; if (pfi >= 0) mi = pfi; }
    char tail[64] = "";
    if (mi < 0 || !obj_to_a_call_tail(&c->scopes[mi], tail, sizeof tail)) {
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
    buf_printf(&arms, "%s)); break;", tail);
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
/* name -> first scope of that name; scopes sharing a name chain through
   next. Kept with the counts, so a call node appended later is counted
   without rebuilding it. */
static int *g_mih_head = NULL, *g_mih_next = NULL, g_mih_cap = 0;
static void mih_count_call(Compiler *c, int id) {
  const char *nm = nt_str(c->nt, id, "name");
  if (!nm) return;
  unsigned h = sp_strhash(nm) & (unsigned)(g_mih_cap - 1);
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
    unsigned h = sp_strhash(nm) & (unsigned)(cap - 1);
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
      unsigned b = sp_strhash(sn) & (unsigned)(nb - 1);
      if (g_fi_nm_tail[b] < 0) g_fi_nm_head[b] = si;
      else g_fi_nm_next[g_fi_nm_tail[b]] = si;
      g_fi_nm_tail[b] = si;
    }
    g_fi_nm_nb = nb;
    g_fi_nm_nscopes = c->nscopes;
    g_fi_nm_scopes = c->scopes;
  }
  return g_fi_nm_head[sp_strhash(nm) & (unsigned)(g_fi_nm_nb - 1)];
}

/* Every user method a call could reach: by the receiver's class when it names
   one, by the bare name when the receiver is boxed or absent. */
static void fi_callees_walk(Compiler *c, int callnode, int *out, int *n, int max) {
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
static void fi_collect_calls_walk(Compiler *c, int id, int *out, int *n, int max, int depth) {
  const NodeTable *nt = c->nt;
  if (id < 0) return;
  if (depth > 4096 || *n >= max) { g_fi_trunc = 1; return; }
  if (nt_kind(nt, id) == NK_CallNode) out[(*n)++] = id;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) fi_collect_calls_walk(c, nt_ref_at(nt, id, i), out, n, max, depth + 1);
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int cn = 0; const int *ids = nt_arr_at(nt, id, i, &cn);
    for (int j = 0; j < cn; j++) fi_collect_calls_walk(c, ids[j], out, n, max, depth + 1);
  }
}

/* Not re-walked per ask: fi_build's fixpoints re-list every body's calls and callees on each of up to 256 rounds. */
typedef struct { int a, b, n, trunc; int *ids; } FiMemo;
static FiMemo *g_fi_memo;
static int g_fi_memo_cap, g_fi_memo_n, g_fi_memo_on;

static void fi_memo_reset(int on) {
  for (int i = 0; i < g_fi_memo_cap; i++) if (g_fi_memo[i].ids) free(g_fi_memo[i].ids);
  free(g_fi_memo);
  g_fi_memo = NULL;
  g_fi_memo_cap = g_fi_memo_n = 0;
  g_fi_memo_on = on;
}

static unsigned fi_memo_hash(int a, int b, int cap) {
  return ((unsigned)a * 2654435761u ^ (unsigned)b * 40503u) & (unsigned)(cap - 1);
}

static FiMemo *fi_memo_slot(int a, int b) {
  if (g_fi_memo_n * 2 >= g_fi_memo_cap) {
    int cap = g_fi_memo_cap ? g_fi_memo_cap * 2 : 1024;
    FiMemo *grown = (FiMemo *)calloc((size_t)cap, sizeof(FiMemo));
    if (!grown) return NULL;
    for (int i = 0; i < cap; i++) grown[i].n = -1;
    for (int i = 0; i < g_fi_memo_cap; i++) {
      if (g_fi_memo[i].n < 0) continue;
      unsigned h = fi_memo_hash(g_fi_memo[i].a, g_fi_memo[i].b, cap);
      while (grown[h].n >= 0) h = (h + 1) & (unsigned)(cap - 1);
      grown[h] = g_fi_memo[i];
    }
    free(g_fi_memo);
    g_fi_memo = grown;
    g_fi_memo_cap = cap;
  }
  unsigned h = fi_memo_hash(a, b, g_fi_memo_cap);
  while (g_fi_memo[h].n >= 0) {
    if (g_fi_memo[h].a == a && g_fi_memo[h].b == b) return &g_fi_memo[h];
    h = (h + 1) & (unsigned)(g_fi_memo_cap - 1);
  }
  g_fi_memo[h].a = a;
  g_fi_memo[h].b = b;
  return &g_fi_memo[h];
}

static int fi_memo_store(FiMemo *m, const int *ids, int n, int trunc) {
  m->ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
  if (!m->ids) return 0;
  memcpy(m->ids, ids, sizeof(int) * (size_t)n);
  m->n = n;
  m->trunc = trunc;
  g_fi_memo_n++;
  return 1;
}

static void fi_collect_calls(Compiler *c, int id, int *out, int *n, int max, int depth) {
  if (!g_fi_memo_on || depth != 0 || *n != 0 || id < 0) { fi_collect_calls_walk(c, id, out, n, max, depth); return; }
  FiMemo *m = fi_memo_slot(id, max);
  if (m && m->n >= 0) {
    memcpy(out, m->ids, sizeof(int) * (size_t)m->n);
    *n = m->n;
    if (m->trunc) g_fi_trunc = 1;
    return;
  }
  int sv = g_fi_trunc;
  g_fi_trunc = 0;
  fi_collect_calls_walk(c, id, out, n, max, depth);
  int t = g_fi_trunc;
  g_fi_trunc = sv | t;
  if (m) fi_memo_store(m, out, *n, t);
}

/* Not listed per `max`: a smaller one is a prefix of the full list, since the walk appends in scope order. */
static void fi_callees(Compiler *c, int callnode, int *out, int *n, int max) {
  enum { FI_CALLEES_FULL = 65536 };
  if (!g_fi_memo_on || *n != 0 || max > FI_CALLEES_FULL) { fi_callees_walk(c, callnode, out, n, max); return; }
  FiMemo *m = fi_memo_slot(callnode, -1);
  if (m && m->n < 0) {
    int *full = (int *)malloc(sizeof(int) * FI_CALLEES_FULL);
    if (!full) { fi_callees_walk(c, callnode, out, n, max); return; }
    int fn = 0;
    fi_callees_walk(c, callnode, full, &fn, FI_CALLEES_FULL);
    int ok = fi_memo_store(m, full, fn, 0);
    free(full);
    if (!ok) { fi_callees_walk(c, callnode, out, n, max); return; }
  }
  if (!m) { fi_callees_walk(c, callnode, out, n, max); return; }
  int k = m->n < max ? m->n : max;
  memcpy(out, m->ids, sizeof(int) * (size_t)k);
  *n = k;
}

static int fi_reaches(Compiler *c, int from, int target, unsigned char *seen,
                      unsigned char *cand, int depth);

/* A callee's parameter defaults are emitted at the call site, so a rescue or
   a block in one of them lands in this body too (a `**h` into `new` fills
   initialize's keyword defaults in place). `new` reaches every initialize:
   its receiver may be a Class value. A default's own calls fill in their
   defaults there as well (`def outer(y = inner)` with inner's default
   rescuing), so the walk follows a default's calls a few levels down. */
static int fi_callee_defaults_unforceable_at(Compiler *c, int body, int depth) {
  if (depth > 8) return 1;
  int calls[512]; int nc = 0;
  int sv_trunc = g_fi_trunc;
  g_fi_trunc = 0;
  fi_collect_calls(c, body, calls, &nc, 512, 0);
  int cut = g_fi_trunc;
  g_fi_trunc = sv_trunc;
  if (cut) return 1;
  for (int i = 0; i < nc; i++) {
    const char *nm = nt_str(c->nt, calls[i], "name");
    int cal[32]; int n2 = 0;
    if (nm && sp_streq(nm, "new")) {
      for (int si = 0; si < c->nscopes; si++) {
        Scope *m = &c->scopes[si];
        if (!m->name || m->is_cmethod || !sp_streq(m->name, "initialize")) continue;
        if (n2 >= 32) return 1;
        cal[n2++] = si;
      }
    }
    else fi_callees(c, calls[i], cal, &n2, 32);
    for (int k = 0; k < n2; k++) {
      Scope *cs = &c->scopes[cal[k]];
      for (int q = 0; cs->pdefault && q < cs->nparams; q++) {
        if (cs->pdefault[q] < 0) continue;
        if (fi_body_unforceable(c, cs->pdefault[q], 0)) return 1;
        if (fi_callee_defaults_unforceable_at(c, cs->pdefault[q], depth + 1)) return 1;
      }
    }
  }
  return 0;
}

static int fi_callee_defaults_unforceable(Compiler *c, int body) {
  return fi_callee_defaults_unforceable_at(c, body, 0);
}

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

/* The nodes of a body: what absorbing it copies into its caller. The walk is
   bounded in depth, like the other forcing scans, and stops counting past
   `cap`: a body deeper or larger than that answers cap + 1, which the size
   pass reads as over any budget. */
static long fi_node_count(const NodeTable *nt, int n, int depth, long cap) {
  if (n < 0) return 0;
  if (depth > 64) return cap + 1;
  long k = 1;
  int nr = nt_num_refs(nt, n);
  for (int i = 0; i < nr && k <= cap; i++) k += fi_node_count(nt, nt_ref_at(nt, n, i), depth + 1, cap);
  int na = nt_num_arrs(nt, n);
  for (int i = 0; i < na && k <= cap; i++) {
    int cnt = 0; const int *ids = nt_arr_at(nt, n, i, &cnt);
    for (int j = 0; j < cnt && k <= cap; j++) k += fi_node_count(nt, ids[j], depth + 1, cap);
  }
  return k > cap ? cap + 1 : k;
}

/* Every call in a body, however many: the buffer grows until the collection
   is not truncated (or cannot grow further, and then answers 0). */
static int fi_all_calls(Compiler *c, int body, int **buf, int *cap, int *nc) {
  for (;;) {
    *nc = 0;
    g_fi_trunc = 0;
    fi_collect_calls(c, body, *buf, nc, *cap, 0);
    if (!g_fi_trunc) return 1;
    if (*cap >= (1 << 22)) return 0;
    int *nb = realloc(*buf, sizeof(int) * (size_t)(*cap) * 2);
    if (!nb) return 0;
    *buf = nb; *cap *= 2;
  }
}

static void fi_build(Compiler *c) {
  const NodeTable *nt = c->nt;
  fi_memo_reset(1);
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
    if (fi_callee_defaults_unforceable(c, m->body)) continue;
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
  /* Bound the CODE the forcing copies. Each forced call site gets its own
     copy of the callee, with everything that callee absorbs in turn, so a
     chain of small methods that each call the next a few times multiplies:
     six levels of twenty calls asked the C compiler for 20^6 copies, and it
     spent minutes on one file. Charge each candidate its own nodes plus what
     it absorbs, and stop forcing into a caller once its absorbed total passes
     the budget -- the frame pass above, in nodes, and on every program. A
     body whose calls or callees cannot all be listed counts as over. */
  {
    const char *se = getenv("SPINEL_INLINE_FORCE_SIZE");
    long sbudget = (se && *se) ? atol(se) : 100000;
    if (sbudget > 0 && sbudget < LONG_MAX / 4 && ncand > 0 && ncand <= 2048) {
      long *size = (long *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(long));
      long *own = (long *)calloc((size_t)(c->nscopes > 0 ? c->nscopes : 1), sizeof(long));
      int ccap = 8192;
      int *calls = (int *)malloc(sizeof(int) * (size_t)ccap);
      enum { FI_SCALLEES_MAX = 65536 };
      int *cal = (int *)malloc(sizeof(int) * FI_SCALLEES_MAX);
      if (!calls || !own || !cal) { free(size); size = NULL; }
      if (size) {
        for (int si = 0; si < c->nscopes; si++)
          own[si] = fi_node_count(nt, c->scopes[si].body, 0, sbudget);
        for (int round = 0; round < 8; round++) {
          /* what each forced candidate expands to (acyclic, as above); the
             sums saturate just past the budget, which is all the trim reads */
          for (int it = 0; it < 32; it++) {
            int ch = 0;
            for (int si = 0; si < c->nscopes; si++) {
              long v = 0;
              if (cand[si]) {
                v = own[si];
                int nc = 0;
                if (!fi_all_calls(c, c->scopes[si].body, &calls, &ccap, &nc)) v = sbudget + 1;
                for (int i = 0; i < nc && v <= sbudget; i++) {
                  int n2 = 0;
                  fi_callees(c, calls[i], cal, &n2, FI_SCALLEES_MAX);
                  if (n2 >= FI_SCALLEES_MAX) { v = sbudget + 1; break; }
                  for (int j = 0; j < n2 && v <= sbudget; j++) if (cand[cal[j]]) v += size[cal[j]];
                }
                if (v > sbudget) v = sbudget + 1;
              }
              if (size[si] != v) { size[si] = v; ch = 1; }
            }
            if (!ch) break;
          }
          int trimmed = 0;
          for (int si = 0; si < c->nscopes; si++) {
            long tot = own[si];
            int nc = 0;
            /* a body whose calls cannot all be listed forces nothing */
            int over = !fi_all_calls(c, c->scopes[si].body, &calls, &ccap, &nc) || tot > sbudget;
            for (int i = 0; i < nc; i++) {
              int n2 = 0;
              fi_callees(c, calls[i], cal, &n2, FI_SCALLEES_MAX);
              int wide = n2 >= FI_SCALLEES_MAX;
              for (int j = 0; j < n2; j++) {
                if (!cand[cal[j]]) continue;
                if (!over && !wide && tot + size[cal[j]] <= sbudget) { tot += size[cal[j]]; continue; }
                over = 1; cand[cal[j]] = 0; trimmed = 1;
              }
            }
          }
          if (!trimmed) break;
        }
      }
      free(cal);
      free(calls);
      free(own);
      free(size);
    }
  }
  for (int si = 0; si < c->nscopes; si++) g_fi_state[si] = cand[si] ? 1 : 2;
  free(cand);
  fi_memo_reset(0);
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
                       ? "SP_UNUSED " : "";
  const char *ihint = s->is_ext_entry ? ""   /* exported: no inline linkage */
                    : method_inline_force(c, s) ? "inline SP_ALWAYS_INLINE "
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
    else if (sp_streq(cn, "Array") || sp_streq(cn, "Hash")) { buf_puts(b, "sp_RbVal self"); }
    else if (sp_streq(cn, "Object") || sp_streq(cn, "Numeric")) { buf_puts(b, "sp_RbVal self"); }
    else if (sp_streq(cn, "Range"))   { buf_puts(b, "sp_Range self"); }
    else if (sp_streq(cn, "Time"))    { buf_puts(b, "sp_Time self"); }
    else if (sp_streq(cn, "Thread"))  { buf_puts(b, "sp_thread *self"); }
    else if (sp_streq(cn, "Fiber"))   { buf_puts(b, "sp_Fiber *self"); }
    else if (is_exc_name(c->classes[s->class_id].name)) { buf_puts(b, "sp_Exception *self"); }
    else if (io_family_class(c, s->class_id)) { buf_puts(b, "sp_File *self"); }
    else if (sp_streq(cn, "Class"))   { buf_puts(b, "sp_Class self"); }
    else {
      /* value-type reader methods take self by value; initialize keeps a
         pointer so it can populate the fields during construction. */
      int vt = c->classes[s->class_id].is_value_type;
      int is_init = s->name && sp_streq(s->name, "initialize");
      buf_printf(b, "sp_%s %sself", cn, (vt && !is_init) ? "" : "*");
    }
    wrote = 1;
  }
  /* a parameter the body reassigns inside a begin (and reads after the
     rescue or the retry's longjmp) is as indeterminate there as a local
     would be: volatile, as declare_local makes the local (#6552) */
  char **pvol = NULL; int npvol = 0, pall = 0;
  int psi = (int)(s - c->scopes);
  int pbegin = psi >= 0 && psi < c->nscopes && s->nparams > 0 && scope_has_begin(c, psi);
  if (pbegin) begin_volatile_names(c, psi, &pvol, &npvol, &pall);
  for (int i = 0; i < s->nparams; i++) {
    if (wrote++) buf_puts(b, ", ");
    LocalVar *p = scope_local(s, s->pnames[i]);
    int pv = pbegin && (pall || name_list_has(pvol, npvol, s->pnames[i])) && p && !p->is_cell;
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
    /* gcc does not keep a volatile parameter across the longjmp at -O2:
       the body works on a volatile local copy (see emit_method_body) */
    buf_printf(b, pv ? " lv_%s__in" : " lv_%s", s->pnames[i]);
  }
  free(pvol);
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
      buf_printf(b, "  self->iv_%s = ", iv_c(nm));
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
      buf_printf(b, ", self->iv_%s);\n", iv_c(nm));
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
      buf_printf(b, ")) { self->iv_%s = lv_val; }\n", iv_c(nm));
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
/* "spinel <RUBY_VERSION> (<release> revision <rev>)", set by the driver
   (main.c) from the build stamp; NULL when codegen runs without it, and
   RUBY_DESCRIPTION says "spinel <RUBY_VERSION>". */
const char *g_ruby_description = NULL;
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
    /* a destructured element into a typed ivar slot: a tag test, and on a
       mismatch a raise that leaves the local's live range for good */
    "sp_slot_int_ck", "sp_slot_float_ck", "sp_slot_str_ck", "sp_slot_bool_ck",
    "sp_slot_sym_ck",
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

/* ---- splitting the top level ----

   A program's top level compiles to one C function, and a large program's is
   a large function: the call-binding probe's 25-case programs gave one of
   ~3,900 lines holding 59 setjmps. In a function that calls setjmp, GCC
   gives every call an abnormal edge into one dispatcher block, which holds
   a PHI with an argument per call for each value live at some setjmp (each
   setjmp's own jmp_buf address among them), and its constant propagation
   revisits those PHIs as the calls' edges turn executable. The cost grows
   with calls x calls x setjmps: gcc 16 -O2 spent 58 of that program's 66 s
   in "tree CCP", and gcc 13 took 414 s over 50 cases, where clang took 8.

   So a top level past MAIN_SPLIT_BYTES with a setjmp in it runs as parts of
   about MAIN_PART_BYTES each, cut between its statements, each a function of
   its own that _sp_main_body calls in turn. A local used by one part is
   declared in that part, as it was in the body; one used by several moves to
   file scope, where every part reaches the same storage, and the body keeps
   its initialisation and its root. Its root stays in the body's frame, which
   outlives every part. A program under the threshold, or with no setjmp at
   its top level, is emitted as before. */
#define MAIN_SPLIT_BYTES (64 * 1024)
#define MAIN_PART_BYTES (32 * 1024)

/* Does [beg, end) contain the word `w` outside literals and comments? */
static int main_text_has_word(const char *p, size_t beg, size_t end, const char *w) {
  size_t wl = strlen(w);
  for (size_t i = beg; i < end; ) {
    size_t j = frame_skip_noncode(p, i, end);
    if (j != i) { i = j; continue; }
    if (!frame_idch(p[i])) { i++; continue; }
    size_t k = i;
    while (k < end && frame_idch(p[k])) k++;
    if (k - i == wl && !strncmp(p + i, w, wl)) return 1;
    i = k;
  }
  return 0;
}

typedef struct { const char *name; int local; } MainName;
static int main_name_cmp(const void *a, const void *b) {
  return strcmp(((const MainName *)a)->name, ((const MainName *)b)->name);
}

/* A file-scope local's declaration text, rewritten: each `T lv_x = init;`
   line becomes `static T lv_x;` in `statics` and `lv_x = init;` in `init`
   (nothing for a `{0}`, which static storage already holds); a root or a
   cell's initial store stays in `init` as it was. 0 when a line has any
   other shape, and the caller leaves the top level whole. */
static int main_local_to_static(const char *p, size_t beg, size_t end, const char *lvn,
                                const char *celln, Buf *statics, Buf *init) {
  size_t i = beg;
  while (i < end) {
    const char *nl = memchr(p + i, '\n', end - i);
    size_t le = nl ? (size_t)(nl - p) : end;
    size_t s = i;
    while (s < le && p[s] == ' ') s++;
    if (le - s >= 6 && (!strncmp(p + s, "SP_GC_", 6) || p[s] == '*')) {
      buf_putn(init, p + i, le - i); buf_puts(init, "\n");
    }
    else {
      const char *eq = s < le ? strstr(p + s, " = ") : NULL;
      if (!eq || (size_t)(eq - p) >= le || le == s || p[le - 1] != ';') return 0;
      size_t de = (size_t)(eq - p), vb = de + 3;
      const char *id = NULL;
      if (de - s > strlen(lvn) && !strncmp(p + de - strlen(lvn), lvn, strlen(lvn))) id = lvn;
      else if (de - s > strlen(celln) && !strncmp(p + de - strlen(celln), celln, strlen(celln))) id = celln;
      if (!id || (p[de - strlen(id) - 1] != ' ' && p[de - strlen(id) - 1] != '*')) return 0;
      buf_puts(statics, "static ");
      buf_putn(statics, p + s, de - s);
      buf_puts(statics, ";\n");
      if (!(le - 1 - vb == 3 && !strncmp(p + vb, "{0}", 3)))
        buf_printf(init, "    %s = %.*s\n", id, (int)(le - vb), p + vb);
    }
    i = nl ? le + 1 : end;
  }
  return 1;
}

/* Split the top level (see above). `open` is where `void _sp_main_body` starts,
   `*frame_ins` the body's frame point, the declarations of the scope's first
   nl locals run from `dbeg` to decl_ends[nl-1], and the statements from
   `sbeg` to the end, statement k ending at cuts[k]. On a split the body is
   rebuilt and *frame_ins moved with it; 1 then. */
static int main_body_split(Compiler *c, Buf *body, size_t open, size_t *frame_ins,
                           size_t dbeg, const size_t *decl_ends, int nl, size_t sbeg,
                           const size_t *cuts, int ncuts) {
  const char *p = body->p;
  size_t end = body->len;
  /* a local the statements add to the scope as they are emitted (a
     setter's value, `__sv<n>`) is declared in its statement's own text, and
     moves with it */
  Scope *s0 = &c->scopes[0];
  size_t dend = nl > 0 ? decl_ends[nl - 1] : dbeg;
  if (ncuts < 2 || end - sbeg < MAIN_SPLIT_BYTES || cuts[ncuts - 1] != end) return 0;
  if (!main_text_has_word(p, sbeg, end, "setjmp")) return 0;
  /* a `return` at the top level returns from the body; out of a part it
     would only end the part. The body's other C returns are an ensure's
     epilogue (emit_retf_return) passing such a return on, dead without one. */
  { int nids = 0; const int *ids = cg_scope_nodes(c, 0, &nids);
    for (int k = 0; k < nids; k++)
      if (nt_kind(c->nt, ids[k]) == NK_ReturnNode) return 0; }

  /* the parts: statements [first, last] each, closed once past the size */
  int *pend = (int *)malloc(sizeof(int) * (size_t)ncuts), np = 0;
  size_t from = sbeg;
  for (int k = 0; k < ncuts; k++)
    if (cuts[k] - from >= MAIN_PART_BYTES || k == ncuts - 1) { pend[np++] = k; from = cuts[k]; }
  if (np < 2) { free(pend); return 0; }

  /* which parts name each local */
  int *first = (int *)malloc(sizeof(int) * (size_t)(nl ? nl : 1));
  char *many = (char *)calloc((size_t)(nl ? nl : 1), 1);
  /* each local's two C names, lv_x and its cell _cell_x, at 2i and 2i+1 */
  char **cn = (char **)calloc((size_t)(2 * nl + 1), sizeof(char *));
  MainName *names = (MainName *)malloc(sizeof(MainName) * (size_t)(2 * nl + 1));
  int nn = 0;
  size_t maxn = 0;
  for (int i = 0; i < nl; i++) {
    first[i] = -1;
    const char *nm = s0->locals[i].name;
    if (!nm) continue;
    size_t ln = strlen(nm) + 8;
    cn[2 * i] = (char *)malloc(ln); cn[2 * i + 1] = (char *)malloc(ln);
    snprintf(cn[2 * i], ln, "lv_%s", nm); snprintf(cn[2 * i + 1], ln, "_cell_%s", nm);
    names[nn].name = cn[2 * i]; names[nn++].local = i;
    names[nn].name = cn[2 * i + 1]; names[nn++].local = i;
    if (ln > maxn) maxn = ln;
  }
  qsort(names, (size_t)nn, sizeof *names, main_name_cmp);
  char *w = (char *)malloc(maxn + 1);
  /* region np is what stays in the body: the BEGIN blocks */
  for (int q = 0; q <= np; q++) {
    size_t rb = q == np ? dend : q ? cuts[pend[q - 1]] : sbeg;
    size_t re = q == np ? sbeg : cuts[pend[q]];
    for (size_t i = rb; i < re; ) {
      size_t j = frame_skip_noncode(p, i, re);
      if (j != i) { i = j; continue; }
      if (!frame_idch(p[i])) { i++; continue; }
      size_t k = i;
      while (k < re && frame_idch(p[k])) k++;
      if ((p[i] == 'l' || p[i] == '_') && k - i < maxn) {
        memcpy(w, p + i, k - i); w[k - i] = '\0';
        MainName key = { w, 0 };
        MainName *hit = (MainName *)bsearch(&key, names, (size_t)nn, sizeof *names, main_name_cmp);
        if (hit) {
          int li = hit->local;
          if (first[li] < 0) first[li] = q;
          else if (first[li] != q) many[li] = 1;
        }
      }
      i = k;
    }
  }

  Buf statics, init, out; Buf *pdecl = (Buf *)calloc((size_t)np, sizeof(Buf));
  memset(&statics, 0, sizeof statics); memset(&init, 0, sizeof init); memset(&out, 0, sizeof out);
  int ok = 1;
  for (int i = 0; i < nl && ok; i++) {
    size_t lb = i > 0 ? decl_ends[i - 1] : dbeg, le = decl_ends[i];
    if (le == lb) continue;
    if (first[i] < 0 || (first[i] == np && !many[i])) buf_putn(&init, p + lb, le - lb);
    else if (!many[i]) buf_putn(&pdecl[first[i]], p + lb, le - lb);
    else ok = main_local_to_static(p, lb, le, cn[2 * i], cn[2 * i + 1], &statics, &init);
  }
  if (ok) {
    buf_putn(&out, p, open);
    emit_synth_line_marker(&out);
    if (statics.len) buf_putn(&out, statics.p, statics.len);
    from = sbeg;
    for (int q = 0; q < np; q++) {
      Buf part; memset(&part, 0, sizeof part);
      buf_printf(&part, "static SP_NOINLINE void _sp_main_part_%d(void){\n    SP_GC_SAVE();\n", q + 1);
      size_t ins = part.len;
      if (pdecl[q].len) buf_putn(&part, pdecl[q].p, pdecl[q].len);
      size_t to = cuts[pend[q]];
      buf_putn(&part, p + from, to - from);
      if (part.len && part.p[part.len - 1] != '\n') buf_puts(&part, "\n");
      buf_puts(&part, "}\n");
      if (!g_no_root_frame) gc_frame_build(&part, ins);
      buf_putn(&out, part.p, part.len);
      free(part.p);
      from = to;
    }
    emit_synth_line_marker(&out);
    size_t nopen = out.len;
    buf_putn(&out, p + open, dbeg - open);
    if (init.len) buf_putn(&out, init.p, init.len);
    buf_putn(&out, p + dend, sbeg - dend);
    for (int q = 0; q < np; q++) buf_printf(&out, "  _sp_main_part_%d();\n", q + 1);
    *frame_ins = nopen + (*frame_ins - open);
    free(body->p);
    *body = out;
  }
  for (int q = 0; q < np; q++) free(pdecl[q].p);
  for (int i = 0; i < 2 * nl; i++) free(cn[i]);
  free(pdecl); free(statics.p); free(init.p); free(w);
  free(names); free(cn); free(first); free(many); free(pend);
  return ok;
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
static size_t wb_stmt_end(const Buf *b, size_t q) {
  size_t k = q + 1, d = 0, send = 0;
  int str = 0, ch = 0;
  for (; k < b->len; k++) {
    char x = b->p[k];
    if (str) { if (x == '\\') k++; else if (x == '"') str = 0; continue; }
    if (ch) { if (x == '\\') k++; else if (x == '\'') ch = 0; continue; }
    if (x == '"') { str = 1; continue; }
    if (x == '\'') { ch = 1; continue; }
    if (x == '(' || x == '[' || x == '{') d++;
    else if (x == ')' || x == ']' || x == '}') { if (!d) break; d--; }
    else if (x == ';' && !d) { send = k; break; }
  }
  if (send) {                       /* not when it is a statement expression's value */
    size_t k2 = send + 1;
    while (k2 < b->len && (b->p[k2] == ' ' || b->p[k2] == '\n' || b->p[k2] == '\t')) k2++;
    if (k2 + 1 < b->len && b->p[k2] == '}' && b->p[k2+1] == ')') send = 0;
  }
  return send;
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
    size_t send = at_stmt2 ? wb_stmt_end(b, q) : 0;
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
    /* Not when this store is the last statement of a statement expression:
       that position IS the expression's value, and wrapping it in a block
       makes the value void. Detected by what follows the statement -- `})`
       closes a statement expression. */
    size_t stmt_end = at_stmt ? wb_stmt_end(b, q) : 0;
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
  /* an IO handle has no slots for a program's instance variables */
  if (s->class_id >= 0 && !s->is_cmethod && io_family_class(c, s->class_id) &&
      scope_uses_ivars(c, (int)(s - c->scopes)))
    unsupported(c, s->def_node, "instance variable in an IO reopening");
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
  if (s->class_id >= 0 && !s->is_cmethod && s->name && c->classes[s->class_id].is_value_type &&
      comp_trampoline_kind(c, s->class_id, s->name, NULL)) {
    emit_method_signature(c, s, b);
    buf_puts(b, " {\n  sp_raise_cls(\"NotImplementedError\", \"instance_eval of a proc on a by-value object\");\n");
    if (method_is_void(s)) {
      /* A `return <value>;` in a void function is a constraint violation that
         MinGW gcc flags under -Werror (-Wno-all doesn't cover -Wreturn-type
         there); emit an empty body instead. */
      buf_puts(b, "}\n");
    }
    else {
      buf_puts(b, "  return ");
      if (ty_is_object(s->ret)) buf_puts(b, "NULL");
      else buf_puts(b, default_value_from_compiler(c, s->ret));
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
      else buf_printf(b, " return %s; }\n", default_value_from_compiler(c, s->ret) ? default_value_from_compiler(c, s->ret) : "0");
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
  /* a method's entry is a safe point for pending finalizers (sp_gc.h) */
  if (g_uses_finalizers) buf_puts(b, "    SP_FIN_POLL();\n");
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
  /* A lowered yielding method (its block taken as a real sp_Proc) is a
     function of its own like any other: a proc it creates that `return`s
     needs the frame all the same -- the proc of a block given to a poly
     receiver's #each, whose `return` otherwise came out as a C return from
     the proc function (#flat_find in ruby-vips). */
  int pr_frame = scope_creates_returning_proc(c, si);
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
      else buf_puts(b, default_value_from_compiler(c, s->ret));
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
    g_method_pr_ensure_depth = 0;
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
                                                  : default_value_from_compiler(c, s->ret));
    else buf_printf(b, "%s;\n", default_value_from_compiler(c, s->ret));
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
  const char *spa = nt_str(nt, blk, "sym_proc_arg");
  if (spa) nameset_add(out, spa);
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

/* Does `root` hold `target`? */
static int node_holds(Compiler *c, int root, int target) {
  if (root < 0) return 0;
  if (root == target) return 1;
  int nr = nt_num_refs(c->nt, root);
  for (int i = 0; i < nr; i++)
    if (node_holds(c, nt_ref_at(c->nt, root, i), target)) return 1;
  int na = nt_num_arrs(c->nt, root);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, root, i, &n);
    for (int k = 0; k < n; k++) if (node_holds(c, ids[k], target)) return 1;
  }
  return 0;
}

/* Is `name` a required parameter of a block under `id` that holds `blk`?
   A block around a Fiber or Thread body binds it there. */
static int name_param_of_block_around(Compiler *c, int id, int blk, const char *name) {
  if (id < 0 || id == blk || !name) return 0;
  if (nt_kind(c->nt, id) == NK_BlockNode && node_holds(c, id, blk)) {
    int bp = nt_ref(c->nt, id, "parameters");
    int pl = bp >= 0 ? nt_ref(c->nt, bp, "parameters") : -1;
    int rn = 0; const int *rq = pl >= 0 ? nt_arr(c->nt, pl, "requireds", &rn) : NULL;
    for (int k = 0; k < rn; k++) {
      const char *pn = nt_str(c->nt, rq[k], "name");
      if (pn && sp_streq(pn, name)) return 1;
    }
  }
  int nr = nt_num_refs(c->nt, id);
  for (int i = 0; i < nr; i++)
    if (name_param_of_block_around(c, nt_ref_at(c->nt, id, i), blk, name)) return 1;
  int na = nt_num_arrs(c->nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(c->nt, id, i, &n);
    for (int k = 0; k < n; k++) if (name_param_of_block_around(c, ids[k], blk, name)) return 1;
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

/* Emits typed-array value `v` rebuilt as the general Array a slot typed
   `slot` holds, and returns 1; returns 0 (nothing emitted) where no
   conversion applies. */
int emit_array_into_poly_slot(Compiler *c, TyKind slot, int v, Buf *b) {
  TyKind vt = comp_ntype(c, v);
  const char *k = vt == TY_INT_ARRAY ? "int" : vt == TY_STR_ARRAY ? "str"
                : vt == TY_FLOAT_ARRAY ? "float" : NULL;
  if (slot != TY_POLY_ARRAY || !k) return 0;
  if (conv_reads_shared_storage(c, v))
    unsupported(c, v, "widening a typed array READ into a poly slot "
                      "(the conversion copies, so writes would not be shared)");
  buf_printf(b, "sp_PolyArray_from_%s_array(", k);
  emit_expr(c, v, b);
  buf_puts(b, ")");
  return 1;
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
    if (ys && (ys->is_lowered_yield || ys->is_proc_form) && ys->blk_param && ys->blk_param[0])
      nameset_add(out, ys->blk_param);
  }
  Scope *fs = sp_streq(ty, "ForwardingSuperNode") ? comp_scope_of(c, id) : NULL;
  for (int i = 0; fs && i < fs->nparams; i++) nameset_add(out, fs->pnames[i]);
  if (fs && fs->blk_param && fs->blk_param[0]) nameset_add(out, fs->blk_param);
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
   the sp_int slot: a small by-value struct. Float and poly keep hand-written
   arms (their reset values and their scans differ); these share one shape, so
   they share one arm rather than a fourth copy in each of the three cell
   prologues. #3995 gave a captured class its cell but stopped at TY_CLASS,
   leaving Range / Rational / Complex on the "non-integer capture" reject, and
   that fix stopped short of Time, Process::Tms and a String range, so
   `t = Time.at(0); proc { t.to_i }` was still refused. Every by-value builtin
   (ty_is_struct_valued) is here. */
const char *cell_value_struct(TyKind t) {
  switch (t) {
    case TY_CLASS:       return "sp_Class";
    case TY_RANGE:       return "sp_Range";
    case TY_FLOAT_RANGE: return "sp_FloatRange";
    case TY_STR_RANGE:   return "sp_StrRange";
    case TY_TIME:        return "sp_Time";
    case TY_TMS:         return "sp_Tms";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_COMPLEX:     return "sp_Complex";
    default: break;
  }
  return NULL;
}

/* The scan a value-struct cell needs. Only a String range holds GC pointers,
   its two endpoint strings; the others are scalars and need none. A cell with
   a scan also gets the write barrier on its stores (wb_cells_collect). */
const char *cell_value_struct_scan(TyKind t) {
  return t == TY_STR_RANGE ? "sp_cell_scan_srange" : "NULL";
}

/* The empty value a value-struct cell resets to. A class has no zero cls_id, so
   it uses the -1 sentinel the class arm has always used. */
const char *cell_value_struct_empty(TyKind t) {
  switch (t) {
    case TY_CLASS:       return "((sp_Class){-1, NULL})";
    case TY_RANGE:       return "((sp_Range){0})";
    case TY_FLOAT_RANGE: return "((sp_FloatRange){0})";
    case TY_STR_RANGE:   return "((sp_StrRange){0})";
    case TY_TIME:        return "((sp_Time){0})";
    case TY_TMS:         return "((sp_Tms){0})";
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


/* Does a generator body yield several values in one step -- `y.yield(a, b)`,
   or a splatted `y.yield(*xs)` on its yielder `yname`? The
   fiber packs such a step as an Array, which the enumerator must then mark
   yields_pair so a `|*r|` block takes it spread and a `|x|` block its first. */
static int gen_yields_multi(const NodeTable *nt, int id, const char *yname) {
  if (id < 0 || !yname) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode") ||
      sp_streq(ty, "SingletonClassNode"))
    return 0;
  /* a nested block or lambda whose own parameter is named like the yielder
     yields to that parameter, not to this generator */
  if ((sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode")) &&
      subtree_has_param_named_pub(nt, nt_ref(nt, id, "parameters"), yname))
    return 0;
  if (sp_streq(ty, "CallNode")) {
    const char *nm = nt_str(nt, id, "name");
    int rcv = nt_ref(nt, id, "receiver");
    if (nm && sp_streq(nm, "yield") && rcv >= 0 &&
        nt_type(nt, rcv) && sp_streq(nt_type(nt, rcv), "LocalVariableReadNode") &&
        nt_str(nt, rcv, "name") && sp_streq(nt_str(nt, rcv, "name"), yname)) {
      int ar = nt_ref(nt, id, "arguments");
      int ac = 0; const int *av = ar >= 0 ? nt_arr(nt, ar, "arguments", &ac) : NULL;
      if (ac != 1) return 1;
      if (ac == 1 && nt_type(nt, av[0]) && sp_streq(nt_type(nt, av[0]), "SplatNode")) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) if (gen_yields_multi(nt, nt_ref_at(nt, id, i), yname)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (gen_yields_multi(nt, ids[k], yname)) return 1;
  }
  return 0;
}

/* emitting a fiber body, which lands in g_procs ahead of the constructors */
static int g_in_fiber_body = 0;

void emit_fiber_new(Compiler *c, int id, Buf *b, int as_gen, int size_node) {
  nd_stamp(nt_ref(c->nt, id, "block"), ND_BLOCK_PROC);   /* the body is a function of its own */
  const NodeTable *nt = c->nt;
  int blk = nt_ref(nt, id, "block");
  /* Fiber.new(&x) / Thread.new(&x): x is read once, now, and kept in the
     fiber; the runtime body calls it with what the first resume passes */
  if (blk >= 0 && nt_kind(nt, blk) == NK_BlockArgumentNode) {
    int ex = nt_ref(nt, blk, "expression");
    if (ex < 0 || as_gen) unsupported(c, id, "&-argument form here");
    buf_puts(b, "sp_Fiber_new_callable("); emit_boxed(c, ex, b); buf_puts(b, ")");
    return;
  }
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
        /* A parameter of an iteration block around this one (cell_shadow:
           the loop publishes its cell each iteration) is bound outside the
           body even when nothing else there names it. */
        if (!encl->body || encl->body < 0) continue;
        if (!name_used_outside(c, encl->body, blk, nm) &&
            !(lv->cell_shadow && name_param_of_block_around(c, encl->body, blk, nm))) continue;
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
  g_in_fiber_body++;
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
  RenPark ren_sv = ren_park(0);   /* as a proc body: park the caller's renames */
  g_pre = NULL; g_indent = 1; g_block_id = blk; g_block_nren = 0;
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
  /* The funnel itself is parked with its mirror, as the proc emitter parks
     it: `_pr_done` and `_prret` belong to the method's C function, and an
     `ensure` in the body ended in a `goto` to them from this one. */
  const char *sv_fbprl = g_method_pr_label, *sv_fbprv = g_method_pr_var;
  g_method_pr_label = NULL; g_method_pr_var = NULL;
  const char *sv_fbser = g_brk_ser_var; g_brk_ser_var = NULL;   /* fresh function context */
  /* the body reads its captures from its own _fc, never from an enclosing
     proc's _cap: a fiber made inside a lifted block read `pr` through a
     `_cap` its C function doesn't have */
  const char *sv_fbcap = g_cap_struct; NameSet *sv_fbcapn = g_cap_names;
  g_cap_struct = NULL; g_cap_names = NULL;
  /* A `break` written directly in a fiber/thread body -- not inside a block
     or a C loop within it -- has nothing to deliver to and cannot reach one
     across the body's own stack. It fell through to a bare C `break;` with no
     loop around it, which did not compile. g_c_ret_void marks the body and
     g_c_loop_depth tells the BreakNode emitter whether a real loop is in
     scope; where neither holds it throws a serial no live scope carries
     (sp_brk_seq starts at 1), taking sp_brk_throw's not-found tail: CRuby's
     LocalJumpError "break from proc-closure", value staged as #exit_value. */
  int sv_fbcld = g_c_loop_depth; g_c_loop_depth = 0;
  int sv_fbbody = g_fiber_body; g_fiber_body = body;
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
      /* one non-array value binds the first param only, as a proc does */
      buf_printf(pb, "    sp_RbVal lv_%s = (_fb->resumed_value.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_fb->resumed_value.cls_id))"
                     " ? sp_poly_index_poly(_fb->resumed_value, sp_box_int(%d)) : %s;\n",
                 bpn, bi, bi == 0 ? "_fb->resumed_value" : "sp_box_nil()");
      buf_printf(pb, "    SP_GC_ROOT_RBVAL(lv_%s);\n", bpn);
    }
  }
  else if (rest_bind) {
    /* A rest param collects the resume arguments as an array. pass_argc says
       how many there were, so resume([1, 2]) binds [[1, 2]] and resume(nil)
       [nil]. When it isn't known (-1: a splat, or a Thread's argument) nil is
       empty, an array is the list, and anything else a one-element list. */
    const char *bpn = rename_local(bp_rest);
    buf_printf(pb, "    sp_PolyArray *lv_%s = ({ sp_RbVal _rv = _fb->resumed_value; int _pn = _fb->pass_argc;"
                   " _pn > 1 ? sp_poly_to_poly_array(_rv)"
                   " : _pn == 0 || (_pn < 0 && _rv.tag == SP_TAG_NIL) ? sp_PolyArray_new()"
                   " : (_pn < 0 && _rv.tag == SP_TAG_OBJ && sp_poly_is_array_kind(_rv.cls_id))"
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
    /* |a| takes the first of several values, as a proc does */
    else buf_printf(pb, "    sp_RbVal lv_%s = _fb->pass_argc > 1 ? sp_poly_index_poly(_fb->resumed_value, sp_box_int(0)) : _fb->resumed_value;\n", bpn);
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
        /* A generator ending in a bare `y << v` yields v, then terminates with
           the yielder as its result, which `<<` answers. */
        emit_stmt(c, last, pb, 1);
        buf_printf(pb, "    _fb->yielded_value = lv_%s;\n", rename_local(bp0));
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
  g_in_fiber_body--;

  /* Restore emission state */
  ren_unpark(&ren_sv);
  g_pre = sv_pre; g_indent = sv_indent; g_block_id = sv_block; g_block_nren = sv_bnren;
  g_block_param_name = sv_bpn; g_self = sv_self; g_ret_type = sv_rt; g_c_ret_void = sv_cv;
  g_proc_return_home = sv_prh_fb; g_proc_toplevel_return = sv_ptr_fb;
  g_self_deref = sv_fbderef;
  g_result_poly = sv_rp; g_result_var = sv_rv; g_yielder_name = sv_yld;
  g_fn_pr_label = sv_fn_prl2; g_fn_pr_var = sv_fn_prv2; g_fn_ret_type = sv_fn_rt2;
  g_method_pr_label = sv_fbprl; g_method_pr_var = sv_fbprv;
  g_brk_ser_var = sv_fbser; g_brk_skip_id = sv_fbskip;
  g_cap_struct = sv_fbcap; g_cap_names = sv_fbcapn;
  g_c_loop_depth = sv_fbcld; g_fiber_body = sv_fbbody;
  g_exc_frame_depth = sv_fbexcd; g_method_pr_exc_depth = sv_fbprexcd;
  g_rescue_save_depth = sv_fbrsd;

  /* Emit creation expression:
     If there are captures, allocate a GC-managed capture struct, fill it,
     assign to fiber->user_data, then return the fiber.
     Without captures: just sp_Fiber_new(fname). */
  int gen_multi = as_gen && gen_yields_multi(nt, body, bp0);
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
      buf_printf(b, "%ssp_Enumerator_new_gen(%s, _t%d, ", gen_multi ? "sp_enum_mark_pair(" : "", fname, tc);
      emit_enum_size_arg(c, size_node, b);
      buf_puts(b, gen_multi ? "))" : ")");
    }
    else {
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_gc_wb((void *)_t%d); _t%d->user_data = _t%d;\n", tf, tf, tc);
      buf_printf(b, "_t%d", tf);
    }
  }
  else if (as_gen) {
    buf_printf(b, "%ssp_Enumerator_new_gen(%s, NULL, ", gen_multi ? "sp_enum_mark_pair(" : "", fname);
    emit_enum_size_arg(c, size_node, b);
    buf_puts(b, gen_multi ? "))" : ")");
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
/* Drop the suffix the shadow rename recorded: `Proc#parameters` reports the
   name the program wrote, not the slot the rename invented. EVERY parameter
   kind has to go through this -- rest, keyword, keyword-rest and block leaked
   the suffix while the positional ones were stripped (#4045). */
static const char *param_public_name(const char *n) {
  if (!n) return n;
  if (!strncmp(n, "__blk_kwrest", 12)) return "**";   /* name_anon_block_kwrest */
  if (!block_param_is_renamed(n)) return n;
  { size_t len = block_param_written_len(n);
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

/* 1 when a parameter in this subtree is spelled exactly `nm`. The shadow
   rename leaves a renamed parameter's node spelled NAME__bpNN, so this tells
   it from an unrenamed name the enclosing scope also uses, which
   subtree_has_param_named_pub matches under either spelling. */
static int param_spelled(const NodeTable *nt, int id, const char *nm) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    if (pn && sp_streq(pn, nm)) return 1;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (param_spelled(nt, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (param_spelled(nt, ids[k], nm)) return 1;
  }
  return 0;
}

/* 1 when `nm` is declared by a block nested in the proc body -- one of its
   parameters, or a local Prism lists in its `locals` -- so the proc's frame
   owns it as it owns its own locals (#4087). Such a block is inlined into the
   proc function (`-> { (0...3).map { |i| keep { i } } }`), runs anew on every
   call of the proc, and binds anew on every run. Taken as a capture instead,
   it was the enclosing frame's one cell: the loop bound `lv_i`, which only
   that frame declares, and the C build stopped; an Array.new block read a
   cell nothing wrote; and a recursive call of the proc rebound a local the
   caller still read. A nested lambda owns its own locals, so the walk stops
   there. */
static int nested_block_declares(Compiler *c, int id, const char *nm) {
  const NodeTable *nt = c->nt;
  if (id < 0 || !nm) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty || sp_streq(ty, "LambdaNode")) return 0;
  if (sp_streq(ty, "BlockNode")) {
    int pn = nt_ref(nt, id, "parameters");
    if (param_spelled(nt, pn, nm)) return 1;
    const char *locs = nt_str(nt, id, "locals");
    size_t nl = strlen(nm);
    for (const char *p = locs ? locs : ""; *p; ) {
      const char *e = strchr(p, ',');
      size_t l = e ? (size_t)(e - p) : strlen(p);
      if (l == nl && memcmp(p, nm, nl) == 0) {
        /* The list keeps a renamed parameter's raw NAME: that entry is the
           parameter, not the enclosing scope's `nm`. Numbered and `it`
           parameters have no node of their own, and the list carries their
           renamed spelling. */
        const char *pt = pn >= 0 ? nt_type(nt, pn) : NULL;
        if (pt && (sp_streq(pt, "NumberedParametersNode") || sp_streq(pt, "ItParametersNode"))) return 1;
        if (!subtree_has_param_named_pub(nt, pn, nm)) return 1;
        break;
      }
      if (!e) break;
      p = e + 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (nested_block_declares(c, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) if (nested_block_declares(c, ids[k], nm)) return 1;
  }
  return 0;
}

static void emit_cell_alloc(Compiler *c, LocalVar *lv, const char *nm, Buf *b) {
  const char *vs = cell_value_struct(lv->type);
  emit_cell_elem_type(c, lv, b);
  buf_printf(b, " *_cell_%s = (", nm);
  emit_cell_elem_type(c, lv, b);
  buf_puts(b, " *)sp_gc_alloc(sizeof(");
  emit_cell_elem_type(c, lv, b);
  buf_puts(b, "), NULL, ");
  if (lv->type == TY_PROC) buf_puts(b, "sp_cell_scan_procint");
  else if (lv->type == TY_POLY) buf_puts(b, "sp_cell_scan_rbval");
  else if (vs) buf_puts(b, cell_value_struct_scan(lv->type));
  else if (lv->type != TY_FLOAT && cell_is_typed_ptr(c, lv)) buf_puts(b, cell_scan_fn(lv->type));
  else buf_puts(b, "NULL");
  buf_printf(b, "); SP_GC_ROOT(_cell_%s); *_cell_%s = ", nm, nm);
  if (lv->type == TY_FLOAT) buf_puts(b, "0.0");
  else if (lv->type == TY_POLY) buf_puts(b, "sp_box_nil()");
  else if (vs) buf_puts(b, cell_value_struct_empty(lv->type));
  else if (lv->type != TY_PROC && cell_is_typed_ptr(c, lv)) buf_puts(b, "NULL");
  else buf_puts(b, "0");
  buf_puts(b, ";\n");
}

/* A cell the proc's own frame owns, allocated in its prologue (#4087). A
   cell over an inlined block's parameter also gets the plain C slot the
   loop binds (LocalVar.cell_shadow). */
static void emit_proc_owned_cell(Compiler *c, Buf *pb, LocalVar *lv, const char *nm) {
  if (lv->cell_shadow) declare_local_named(c, pb, lv, nm, 0);
  buf_puts(pb, "    ");
  emit_cell_alloc(c, lv, nm, pb);
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
/* Is lv, a local of some method scope, one that scope's begin/rescue (or
   loop / heavy break) setjmp needs volatile? The inlined copy of that
   method sits under the same setjmp, so it needs it as much: a yield-inlined
   `begin ... ensure` read back `saved = true` as false at -O2 (#6552). */
static int inlined_local_needs_volatile(Compiler *c, LocalVar *lv) {
  for (int si = 0; si < c->nscopes; si++) {
    Scope *s = &c->scopes[si];
    if (!s->locals || lv < s->locals || lv >= s->locals + s->nlocals) continue;
    if (!scope_has_begin(c, si)) return 0;
    char **names = NULL; int nn = 0, all = 0;
    begin_volatile_names(c, si, &names, &nn, &all);
    int hit = all;
    for (int k = 0; k < nn && !hit; k++) if (names[k] && lv->name && sp_streq(names[k], lv->name)) hit = 1;
    free(names);
    return hit;
  }
  return 0;
}

void emit_inlined_local_decl(Compiler *c, LocalVar *lv, const char *rn, Buf *b, int din) {
  if (!lv->is_cell) {
    emit_indent(b, din);
    if (inlined_local_needs_volatile(c, lv)) {
      Buf ct; memset(&ct, 0, sizeof ct);
      emit_ctype(c, lv->type, &ct);
      const char *t = ct.p ? ct.p : "";
      size_t tl = strlen(t);
      while (tl > 0 && t[tl - 1] == ' ') tl--;
      /* a pointer takes the qualifier on itself, as declare_local's does */
      if (tl > 0 && t[tl - 1] == '*') buf_printf(b, "%.*s volatile", (int)tl, t);
      else buf_printf(b, "volatile %s", t);
      free(ct.p);
    }
    else emit_ctype(c, lv->type, b);
    buf_printf(b, " lv_%s = %s;\n", rn, local_init_value(c, lv));
    if (lv->type == TY_POLY) { emit_indent(b, din); buf_printf(b, "SP_GC_ROOT_RBVAL(lv_%s);\n", rn); }
    else if (needs_root(lv->type) && !comp_ty_value_obj(c, lv->type)) {
      emit_indent(b, din); buf_printf(b, "SP_GC_ROOT(lv_%s);\n", rn);
    }
    return;
  }
  emit_indent(b, din);
  emit_cell_alloc(c, lv, rn, b);
}

void emit_inlined_locals(Compiler *c, Scope *m, int tag, Buf *b, int din) {
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    snprintf(g_ren_from[g_nren], sizeof g_ren_from[0], "%s", lv->name);
    snprintf(g_ren_to[g_nren], sizeof g_ren_to[0], "_y%d_%s", tag, lv->name);
    const char *rn = g_ren_to[g_nren];
    g_nren++;
    emit_inlined_local_decl(c, lv, rn, b, din);
  }
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

/* Bind a proc parameter slot: `cond` true reads the argument `arg`, else the
   default node `dv` (or `fallback` when dv < 0) is evaluated. A default can
   need helper statements (an array or hash literal allocates into a temp), so
   they run in the else branch, and the slot is rooted: an allocated default
   has no other reference. A `nilable` Integer slot takes a nil, passed or
   defaulted, as its own nil (emit_unbox_nilable_text). */
static void emit_proc_param_slot(Compiler *c, Buf *pb, const char *name, const char *cond,
                                 const char *arg, int dv, const char *fallback, TyKind lt,
                                 int nilable) {
  Buf dpre = {0}, dval = {0};
  Buf *sv_pre = g_pre; int sv_ind = g_indent;
  g_pre = &dpre; g_indent = 3;
  if (dv >= 0) emit_boxed(c, dv, &dval); else buf_puts(&dval, fallback);
  g_pre = sv_pre; g_indent = sv_ind;
  /* A block lifted into a proc keeps the static type the enclosing scope gave
     its parameter, so the boxed slot is unboxed into that type. */
  int typed = lt == TY_INT || lt == TY_FLOAT || lt == TY_BOOL || lt == TY_SYMBOL ||
              lt == TY_STRING || lt == TY_POLY_POLY_HASH || lt == TY_SYM_POLY_HASH ||
              lt == TY_STR_POLY_HASH || lt == TY_STRBUF;
  const char *slot = typed ? "_pv_" : "lv_";
  buf_printf(pb, "    sp_RbVal %s%s;\n", slot, name);
  buf_printf(pb, "    if (%s) %s%s = %s;\n", cond, slot, name, arg);
  buf_puts(pb, "    else {\n");
  if (dpre.p) buf_puts(pb, dpre.p);
  buf_printf(pb, "      %s%s = %s;\n    }\n", slot, name, dval.p ? dval.p : "sp_box_nil()");
  buf_printf(pb, "    SP_GC_ROOT_RBVAL(%s%s); (void)%s%s;\n", slot, name, slot, name);
  if (typed) {
    char src[160];
    snprintf(src, sizeof src, "_pv_%s", name);
    Buf ub = {0};
    if (nilable) emit_unbox_nilable_text(c, lt, src, &ub);
    /* an optional the body appends to is the shared handle (#6179): the
       boxed channel carries the caller's, and nil, passed or defaulted, is
       the NULL handle */
    else if (lt == TY_STRBUF) buf_printf(&ub, "sp_poly_nil_p(%s) ? NULL : sp_poly_as_strbuf(%s)", src, src);
    else emit_unbox_text(c, lt, src, &ub);
    buf_printf(pb, "    %s lv_%s = %s;", c_type_name(lt), name, ub.p);
    if (proc_slot_is_ptr(lt)) buf_printf(pb, " SP_GC_ROOT(lv_%s);", name);
    buf_printf(pb, " (void)lv_%s;\n", name);
    free(ub.p);
  }
  free(dpre.p); free(dval.p);
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
    /* `**kw` binds the trailing kwargs hash in the prologue below, less the
       named keywords when there are any */
    int kwrest0 = pn0 >= 0 ? nt_ref(nt, pn0, "keyword_rest") : -1;
    const char *kwrty0 = kwrest0 >= 0 ? nt_type(nt, kwrest0) : NULL;
    if (kwrty0 && sp_streq(kwrty0, "KeywordRestParameterNode")) {
      const char *krn = nt_str(nt, kwrest0, "name");
      if (krn) nameset_add(&params, krn);
    }
  }
  proc_collect_used(c, body, &used);
  /* an optional or keyword default runs in this fn too, so what it reads is
     captured like a body read */
  proc_collect_used(c, proc_params_node(c, create), &used);
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
  collect_locals_deep(c, proc_params_node(c, create), &locals);
  for (int u = 0; u < used.n; u++) {
    const char *nm = used.v[u];
    if (nameset_has(&params, nm)) continue;
    LocalVar *lv = scope_local(bs, nm);
    /* A local the BLOCK declares is this frame's own, not the enclosing
       frame's: capturing it made the outer frame's cell the one every
       invocation shared (so per-iteration closures all saw the last value),
       and the block-local reset then assigned a `_cell_x` no function here
       declared -- the C build stopped (#4087). The prologue declares it. */
    if (lv && lv->is_cell && (proc_owns_local(c, create, nm) || nested_block_declares(c, body, nm))) {
      nameset_add(&locals, nm);
      continue;
    }
    if (lv && lv->is_cell) {
      int ptr_cell = proc_slot_is_ptr(lv->type) && !comp_ty_value_obj(c, lv->type);
      /* Float captures ride the capture struct (a real sp_float field), not the
         proc's argument slot, so they are safe even alongside a float parameter
         -- a first-class proc's `.call` passes float args through the boxed
         side-channel (sp_box_float / sp_poly_to_f), not the truncating slot. */
      int float_cell = lv->type == TY_FLOAT;
      int poly_cell = lv->type == TY_POLY;
      /* a by-value struct rides its own cell, like a float (#3995 for the
         class; the Range / Rational / Complex siblings were left on the
         reject, and Time / Tms / a String range after them) */
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
  /* A `next <v>` answers the proc's value as the tail does: a body whose tail
     has no value (`next k.to_s if k.is_a?(Symbol); nil`) still publishes the
     value a `next` hands back, rather than nil for every call. */
  if (ret == TY_NIL || ret == TY_VOID) {
    TyKind nx = ie_block_break_next_ty(c, body);
    if (nx != TY_UNKNOWN && nx != TY_NIL && nx != TY_VOID) ret = TY_POLY;
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
  int ie_mark = ie_class_of(c, body) == ie_class_of(c, create) ? -1 : ie_class_of(c, body);
  if (ie_mark < -1) unsupported(c, create, "proc run by instance_eval on more than one class");
  int ie_cls = ie_mark >= 0 && proc_body_uses_self(c, body, ie_mark) ? ie_mark : -1;
  int scls = g_ie_class_id >= 0 ? g_ie_class_id : bs && !bs->is_cmethod ? bs->class_id : -1;
  int cap_self = ie_cls < 0 && scls >= 0 &&
                 (proc_body_uses_self(c, body, scls) ||
                  proc_body_uses_self(c, proc_params_node(c, create), scls));
  /* A class method that takes the receiving class as a leading parameter has
     it in `_sp_cls`; a lifted block's function signature is (_cap, argc, args)
     and knows nothing of it, so a sibling class-method call inside the block
     referenced an identifier that is not in scope. Carry it in the capture
     struct, the way instance self is carried (#3797). */
  int cap_cls = ie_cls < 0 && bs && bs->is_cmethod &&
                cmethod_takes_self_cls(c, (int)(bs - c->scopes));
  int self_is_value = cap_self && c->classes[scls].is_value_type;
  /* an Object / Array / Hash / Numeric reopening holds self boxed (its
     methods take `sp_RbVal self`): captured as the boxed value */
  int self_boxed = cap_self && !self_is_value && c->classes[scls].name &&
                   (sp_streq(c->classes[scls].name, "Object") || sp_streq(c->classes[scls].name, "Array") ||
                    sp_streq(c->classes[scls].name, "Hash") || sp_streq(c->classes[scls].name, "Numeric"));
  const char *self_cls = cap_self ? c->classes[scls].c_name : NULL;

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
    else if (self_boxed) buf_puts(&g_procs, " sp_RbVal __self_rb;");
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
      if (class_needs_scan(&c->classes[scls]))
        buf_printf(&g_procs, "  sp_%s__gc_scan(&_c->__self_val);\n", self_cls);
    }
    else if (self_boxed) buf_puts(&g_procs, "  sp_mark_rbval(_c->__self_rb);\n");
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
  /* the body's inlines push their renames from slot 0, over the enclosing
     method's live entries: park them, not just the count (#3943) */
  RenPark ren_sv = ren_park(0);
  g_pre = NULL; g_indent = 0; g_block_id = -1; g_block_nren = 0; g_block_param_name = NULL;
  g_self = "self"; g_result_var = NULL; g_ret_type = ret; g_ensure_depth = 0; g_result_poly = 0;
  int sv_iec = g_ie_class_id, sv_bcls = bs ? bs->class_id : -1, sv_bcm = bs ? bs->is_cmethod : 0;
  if (ie_cls >= 0) g_ie_class_id = ie_cls;
  int bs_moved = ie_cls >= 0 && sv_bcls >= 0;
  if (bs_moved) { comp_scope_move_begin(c, (int)(bs - c->scopes)); bs->class_id = ie_cls; bs->is_cmethod = 0; }
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
  int sv_loopd = g_c_loop_depth, sv_inproc = g_in_proc_body, sv_cv = g_c_ret_void;
  g_c_loop_depth = 0; g_in_proc_body = 1;   /* fresh fn: outer loops don't count */
  g_c_ret_void = 0;   /* returns sp_int, whatever the enclosing body returns */
  Buf proc_body_buf; memset(&proc_body_buf, 0, sizeof proc_body_buf);
  Buf *pb = &proc_body_buf;
  buf_printf(pb, "static sp_int _proc_%d(void *_cap, sp_int argc, sp_int *args) {\n", pid);
  buf_puts(pb, "    SP_GC_SAVE();\n");
  size_t proc_frame_ins = pb->len;
  if (ncap == 0 && !cap_self && !cap_cls && !ret_proc) buf_puts(pb, "    (void)_cap;\n");
  buf_puts(pb, "    (void)args;\n");
  buf_puts(pb, "    (void)argc;\n");
  /* `&b`: the block the call that entered this body was given, on the
     _sp_proc_blk side-channel; nil (NULL) when none was (#2648). Read
     before anything below can enter another proc -- a default, a keyword's
     default, the lambda's arity raise -- which sets the channel for its own
     call. */
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
  /* Captured instance self, read back from _cap (#1436). (void) guards the
     over-approximating use-of-self detection. */
  if (cap_self && self_is_value) {
    buf_printf(pb, "    sp_%s self = ((_proc_cap_%d *)_cap)->__self_val;\n", self_cls, pid);
    buf_puts(pb, "    (void)self;\n");
  }
  else if (self_boxed) {
    buf_printf(pb, "    sp_RbVal self = ((_proc_cap_%d *)_cap)->__self_rb;\n", pid);
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
  if (ie_cls >= 0)
    buf_printf(pb, "    sp_%s *self = (sp_%s *)_sp_ie_self; _sp_ie_self = NULL;\n"
               "    if (!self) sp_raise_cls(\"NotImplementedError\", \"proc bound to instance_eval called without it\");\n",
               c->classes[ie_cls].c_name, c->classes[ie_cls].c_name);
  /* Lambda: strict arity -- requireds + trailing posts mandatory, optionals
     widen the max, a splat rest lifts it entirely. */
  int has_kwrest = 0, no_kw = 0;
  { int pnk = proc_params_node(c, create);
    int kwr = pnk >= 0 ? nt_ref(nt, pnk, "keyword_rest") : -1;
    if (kwr >= 0) has_kwrest = 1;
    if (kwr >= 0 && nt_type(nt, kwr) && sp_streq(nt_type(nt, kwr), "NoKeywordsParameterNode")) no_kw = 1; }
  /* The trailing argument is keywords when it is a Hash the call site passed
     as keywords (_sp_proc_kwpos, read before anything below can call another
     proc, and cleared whatever this proc's shape is). A caller that does not
     say -- a runtime path -- has it taken as keywords only past the required
     positionals. The keywords are taken out of the positional count, so an
     optional, a rest or a post never binds the keyword hash. A `**nil` proc
     takes keywords only from a call site that passed them, and refuses them. */
  int has_kwp = nkw > 0 || has_kwrest;
  if (has_kwp) {
    g_needs_proc_poly_argslot = 1;
    buf_printf(pb, "    int _sp_kwpos = _sp_proc_kwpos; _sp_proc_kwpos = 0;\n"
                   "    sp_int _sp_haskw = argc > 0 && argc <= 16"
                   " && (_sp_kwpos == 2 || (_sp_kwpos == 0 && argc > %d))"
                   " && _sp_proc_poly_args[argc-1].tag == SP_TAG_OBJ"
                   " && sp_poly_is_hash_kind(_sp_proc_poly_args[argc-1].cls_id);\n"
                   "    sp_RbVal _sp_kwh = _sp_haskw ? _sp_proc_poly_args[argc-1] : sp_box_nil();"
                   " SP_GC_ROOT_RBVAL(_sp_kwh);\n"
                   "    argc -= _sp_haskw;\n", no_kw ? 16 : arity + nposts);
    if (no_kw)
      buf_puts(pb, "    if (_sp_haskw && sp_poly_length(_sp_kwh) > 0)"
                   " sp_raise_cls(\"ArgumentError\", \"no keywords accepted\");\n");
  }
  /* A block that takes only leading requireds (a trailing comma's rest
     among them) auto-splats a lone Array whatever keywords came; any other
     shape only for a call that passed none, an empty `**h` too (kwpos 3):
     `proc { |a, *r| }.call([1, 2], **{})` binds a = [1, 2]. */
  int as_lead_only = 0, has_rest_marker = 0;
  { int pnr = proc_params_node(c, create);
    int rn = pnr >= 0 ? nt_ref(nt, pnr, "rest") : -1;
    has_rest_marker = rn >= 0;
    as_lead_only = !nopts && !nposts && !has_kwp &&
                   (rn < 0 ? arity > 1 : arity >= 1 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ImplicitRestNode")); }
  int as_gate = !is_lambda && !as_lead_only && block_auto_splats(arity, nopts, nposts, has_rest_marker);
  if (!has_kwp && as_gate)
    buf_puts(pb, "    int _sp_kwpos = _sp_proc_kwpos; _sp_proc_kwpos = 0;\n");
  else if (!has_kwp) buf_puts(pb, "    _sp_proc_kwpos = 0;\n");
  /* `arity` counts numbered parameters when the block carries a
     NumberedParametersNode, so adding nnumbered there counts them twice and a
     `lambda { _1 }.call("a")` was rejected as taking two. Only the
     no-parameters-node form (`-> { _1 }`) has them outside `arity`. */
  int num_extra = proc_numbered_params_node(c, create) < 0 ? nnumbered : 0;
  if (is_lambda) {
    int lreq = arity + nposts + num_extra;
    char kw[256];
    arity_kw_suffix(nt, proc_params_node(c, create), kw, sizeof kw);
    buf_puts(pb, "    ");
    emit_arity_check(pb, "argc", lreq, proc_has_rest(c, create) ? -1 : lreq + nopts, kw);
    buf_puts(pb, ";\n");
  }
  /* CRuby proc auto-splat: a single Array passed to a non-lambda proc is
     destructured across the parameters by the rule a yield's block follows
     (block_auto_splats): `proc { |k, v = 5| }.call([:a, 1])` binds k = :a.
     Rewrite the argument view (both the sp_int[] slots and the boxed
     side-channel) from the array's elements before binding. */
  if (!is_lambda && block_auto_splats(arity, nopts, nposts, has_rest_marker)) {
    g_needs_proc_poly_argslot = 1;
    buf_puts(pb, "    sp_int _sp_as_buf[16];\n");
    buf_printf(pb, "    if (argc == 1%s%s && _sp_proc_poly_args[0].tag == SP_TAG_OBJ && sp_poly_is_array_kind(_sp_proc_poly_args[0].cls_id)) {\n",
               has_kwp ? " && !_sp_haskw" : "", as_gate ? " && _sp_kwpos != 3" : "");
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
        buf_printf(pb, " : %s;\n", default_value_from_compiler(c, pt));
      }
      else buf_printf(pb, "%s;\n", default_value_from_compiler(c, pt));
    }
    else if (pt == TY_POLY || pt == TY_FLOAT) {
      /* A poly param doesn't fit the sp_int slot, so it rides the
         _sp_proc_poly_args side-channel the call site published. A float rides
         it too: a raw sp_float in the slot is value-truncated (0.7 -> 0), so
         read the boxed value back, unboxing a float with sp_poly_to_f_or_nil:
         a nil passed is the slot's own nil, as a missing one is, where
         sp_poly_to_f read it as 0.0. The side-channel array holds 16 slots
         (the proc-call ABI cap). */
      if (k < 16) {
        g_needs_proc_poly_argslot = 1;  /* channel array now lives in spinel_rt.h */
        if (pt == TY_FLOAT)
          buf_printf(pb, "(argc > %d) ? sp_poly_to_f_or_nil(_sp_proc_poly_args[%d]) : sp_float_nil();\n", k, k);
        else
          buf_printf(pb, "(argc > %d) ? _sp_proc_poly_args[%d] : sp_box_nil();\n", k, k);
      }
      else buf_puts(pb, pt == TY_FLOAT ? "sp_float_nil();\n" : "0;\n");
      /* either nil makes it nullable, as an Integer's below */
      if (lv && pt == TY_FLOAT) lv->nullable_int = 1;
    }
    else if (pt == TY_STRBUF && k < 16) {
      /* A String parameter the body appends to is the shared handle (#6179):
         it reads the boxed channel every call publishes, which carries the
         caller's handle when the caller's String is one, and a plain String
         otherwise, wrapped in a handle of its own (sp_poly_as_strbuf). The
         sp_int slot holds bytes, never a handle. */
      g_needs_proc_poly_argslot = 1;
      buf_printf(pb, "(argc > %d) ? sp_poly_as_strbuf(_sp_proc_poly_args[%d]) : NULL;\n", k, k);
      /* the handle made for a plain String is held by nothing else */
      buf_printf(pb, "    SP_GC_ROOT(lv_%s);\n", p);
    }
    else if (proc_slot_is_ptr(pt)) {
      buf_printf(pb, "(argc > %d) ? (", k); emit_ctype(c, pt, pb);
      buf_printf(pb, ")(uintptr_t)args[%d] : NULL;\n", k);
    }
    else {
      const char *nilv = (pt == TY_INT || pt == TY_BOOL) ? "SP_INT_NIL"
                       : (pt == TY_FLOAT) ? "sp_float_nil()"
                       : (pt == TY_SYMBOL) ? "((sp_sym)-1)" : NULL;
      if (nilv && pt == TY_INT && k < 16) {
        /* A nil the call passes arrives in the sp_int slot as the 0 its call
           site put there (emit_proc_call_args), where it read as 0. Only the
           boxed side channel, which every call path publishes and a poly
           parameter reads, tells it from a 0. */
        g_needs_proc_poly_argslot = 1;
        buf_printf(pb, "(argc > %d) ? (_sp_proc_poly_args[%d].tag == SP_TAG_NIL ? SP_INT_NIL : args[%d]) : %s;\n",
                   k, k, k, nilv);
      }
      else if (nilv) buf_printf(pb, "(argc > %d) ? args[%d] : %s;\n", k, k, nilv);
      if (nilv) {
        /* The binding we just wrote is what makes this slot nullable: called
           with fewer arguments, or passed a nil, the param holds the
           sentinel, so boxing it in the body (`[a, b, c]`, `a == nil`) has
           to answer nil rather than carry INTPTR_MIN through as a truthy
           integer. Analysis marks it too where it sees the call that does
           (cs_type_params), so a local copied from it is marked as well;
           this one covers the calls it cannot see (`pr === x`, an alias). */
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
      buf_printf(pb, "    %s *_cell_%s = (%s *)sp_gc_alloc(sizeof(%s), NULL, %s);"
                     " SP_GC_ROOT(_cell_%s); *_cell_%s = lv_%s;%c", vs, p, vs, vs,
                     cell_value_struct_scan(lv->type), p, p, p, 10);
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
          emit_proc_owned_cell(c, pb, lv, nbuf);
        }
      }
      if (!e) break;
      q = e + 1;
    } }
  /* A celled local a block nested in the body declares: this frame owns it
     too (nested_block_declares), and the block-local reset at the top of
     each run of that block gives it a fresh cell. */
  for (int i = 0; i < locals.n; i++) {
    LocalVar *lv = scope_local(bs, locals.v[i]);
    if (!lv || !lv->is_cell || nameset_has(&params, locals.v[i])) continue;
    if (proc_owns_local(c, create, locals.v[i]) || !nested_block_declares(c, body, locals.v[i])) continue;
    emit_proc_owned_cell(c, pb, lv, locals.v[i]);
  }
  /* Splat rest and trailing post params. Both read the boxed side-channel:
     every call path now publishes all args boxed (yield's lean ABI was
     retired for this), so any position is recoverable regardless of the
     callee's static types. They bind by the plan a yield's block does
     (sp_proc_fill at run time, block_fill in emit_block_binds): requireds
     leading and post first, optionals from what remains, a rest the
     middle, extras dropped, missing posts nil. The posts were taken from
     the end, so `proc { |a, b = 5, c| }.call(1, 2, 3, 4)` bound c = 4 and
     `.call(1, "s", :t)` into `|a = 5, b|` bound b = :t. */
  /* A local a default binds (a block param in `x = a.map { |i| i }`) is
     declared ahead of the optional and keyword slots that evaluate it. */
  NameSet dlocals = {0};
  collect_locals_deep(c, proc_params_node(c, create), &dlocals);
  for (int i = 0; i < dlocals.n; i++) {
    LocalVar *lv = scope_local(bs, dlocals.v[i]);
    if (nameset_has(&params, dlocals.v[i])) continue;
    if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) declare_local(c, pb, lv, 0);
  }
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
       requireds and the posts (_sp_ot of them); a slot with no argument evaluates its
       default (which may reference earlier params -- they are bound above /
       to the left). Boxed like rest/post: a first-class proc's optionals are
       type-erased at the call site. */
    if (nopts > 0 || nposts > 0 || (restn && restn[0]))
      buf_printf(pb, "    sp_int _sp_ot, _sp_ps; sp_proc_fill(%d, %d, %d, %d, argc, &_sp_ot, &_sp_ps);\n",
                 arity, nopts, nposts, has_rest_marker);
    for (int j = 0; j < nopts; j++) {
      const char *on = proc_opt_name(c, create, j);
      if (!on) continue;
      char cond[96], arg[64];
      snprintf(cond, sizeof cond, "%d < _sp_ot && %d + %d < 16", j, arity, j);
      snprintf(arg, sizeof arg, "_sp_proc_poly_args[%d + %d]", arity, j);
      { LocalVar *olv = scope_local(bs, on);
        emit_proc_param_slot(c, pb, on, cond, arg, proc_opt_value(c, create, j), "sp_box_nil()",
                             olv ? olv->type : TY_POLY, olv && olv->nullable_int); }
    }
    if (restn && restn[0]) {
      buf_printf(pb, "    sp_PolyArray *lv_%s = sp_PolyArray_new(); SP_GC_ROOT(lv_%s);\n", restn, restn);
      buf_printf(pb, "    { sp_int __k = %d + _sp_ot, __hi = _sp_ps; if (__hi > 16) __hi = 16;\n", arity);
      buf_printf(pb, "      for (; __k < __hi; __k++) sp_PolyArray_push(lv_%s, _sp_proc_poly_args[__k]); }\n",
                 restn);
    }
    /* A post binds as an optional does: the boxed value, unboxed into the
       type the analysis gave the parameter. Declared boxed whatever that
       type, a post a lowered block's yields typed a String or an Integer
       (`rec(x, n - 1, &b)` forwarding to `{ |n = 0, t| t.size }`) was
       read through the boxed value as that type, and the C did not
       build. */
    for (int j = 0; j < nposts; j++) {
      const char *pp = proc_post_name(c, create, j);
      if (!pp) continue;
      LocalVar *plv = scope_local(bs, pp);
      TyKind lt = plv ? plv->type : TY_POLY;
      if (lt == TY_POLY || lt == TY_UNKNOWN) {
        buf_printf(pb, "    sp_RbVal lv_%s = ({ sp_int __i = _sp_ps + %d;\n", pp, j);
        buf_puts(pb, "      (__i < argc && __i < 16) ? _sp_proc_poly_args[__i] : sp_box_nil(); });\n");
        buf_printf(pb, "    (void)lv_%s;\n", pp);
        continue;
      }
      buf_printf(pb, "    sp_RbVal _pv_%s = (_sp_ps + %d < argc && _sp_ps + %d < 16) ? _sp_proc_poly_args[_sp_ps + %d]"
                     " : sp_box_nil(); SP_GC_ROOT_RBVAL(_pv_%s);\n", pp, j, j, j, pp);
      char src[160];
      snprintf(src, sizeof src, "_pv_%s", pp);
      Buf ub = {0};
      if (plv->nullable_int) emit_unbox_nilable_text(c, lt, src, &ub);
      /* nil is the NULL handle, not an empty String (#6179) */
      else if (lt == TY_STRBUF) buf_printf(&ub, "sp_poly_nil_p(%s) ? NULL : sp_poly_as_strbuf(%s)", src, src);
      else emit_unbox_text(c, lt, src, &ub);
      buf_puts(pb, "    ");
      emit_ctype(c, lt, pb);
      buf_printf(pb, " lv_%s = %s;", pp, ub.p ? ub.p : "0");
      if (proc_slot_is_ptr(lt)) buf_printf(pb, " SP_GC_ROOT(lv_%s);", pp);
      buf_printf(pb, " (void)lv_%s;\n", pp);
      free(ub.p);
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
    /* CRuby judges the keywords before any binds or any default runs: every
       required one the hash lacks is `missing` in one message, and then,
       with no **kwrest to take them, every key naming no keyword is
       `unknown`, for a proc as for a lambda. Bound one by one, only the
       first missing keyword was named. */
    int kwr0 = pnk >= 0 ? nt_ref(nt, pnk, "keyword_rest") : -1;
    { int nreqkw = 0;
      for (int j = 0; j < nkw2; j++)
        if (nt_type(nt, kwn[j]) && sp_streq(nt_type(nt, kwn[j]), "RequiredKeywordParameterNode")) nreqkw++;
      if (kwr0 < 0 || nreqkw) {
        buf_puts(pb, "    { static const char *const _pkw[] = {");
        for (int j = 0; j < nkw2; j++) {
          const char *kn = nt_str(nt, kwn[j], "name");
          if (kn) buf_printf(pb, "\"%s\", ", param_public_name(kn));
        }
        buf_puts(pb, "0}, *const _pkr[] = {");
        for (int j = 0; j < nkw2; j++) {
          const char *kn = nt_str(nt, kwn[j], "name");
          if (kn && nt_type(nt, kwn[j]) && sp_streq(nt_type(nt, kwn[j]), "RequiredKeywordParameterNode"))
            buf_printf(pb, "\"%s\", ", param_public_name(kn));
        }
        buf_printf(pb, "0}, *const _pkn[] = {0};%c", 10);
        /* judged without building anything on the path that binds: the
           unknown-key check walks the hash as it did, and a keyword rest
           only looks when a required key is absent */
        buf_puts(pb, "      if (_sp_haskw) { if (1");
        if (kwr0 >= 0) {
          buf_puts(pb, " && (0");
          for (int j = 0; j < nkw2; j++) {
            const char *kn = nt_str(nt, kwn[j], "name");
            if (kn && nt_type(nt, kwn[j]) && sp_streq(nt_type(nt, kwn[j]), "RequiredKeywordParameterNode"))
              buf_printf(pb, " || !sp_poly_has_key(_sp_kwh, sp_box_sym((sp_sym)%d))",
                         comp_sym_intern(c, param_public_name(kn)));
          }
          buf_puts(pb, ")");
        }
        buf_printf(pb, ") sp_kwargs_verify(_sp_kwh, _pkw, _pkr, _pkn, %d); }%c", kwr0 < 0, 10);
        if (nreqkw) {
          char miss[512] = "", km[600]; int nm = 0;
          for (int j = 0; j < nkw2; j++) {
            const char *kn = nt_str(nt, kwn[j], "name");
            if (!kn || !nt_type(nt, kwn[j]) || !sp_streq(nt_type(nt, kwn[j]), "RequiredKeywordParameterNode")) continue;
            char iv[300]; snprintf(iv, sizeof iv, ":%s", param_public_name(kn));
            kw_names_add(miss, sizeof miss, &nm, iv);
          }
          kw_error_message(km, sizeof km, "missing", nm, miss);
          buf_printf(pb, "      else sp_raise_cls(\"ArgumentError\", \"%s\");%c", km, 10);
        }
        buf_printf(pb, "    }%c", 10);
      } }
    for (int j = 0; j < nkw2; j++) {
      const char *kn = nt_str(nt, kwn[j], "name");
      if (!kn) continue;
      const char *kpty = nt_type(nt, kwn[j]);
      int dv = (kpty && sp_streq(kpty, "OptionalKeywordParameterNode"))
                 ? nt_ref(nt, kwn[j], "value") : -1;
      char key[128];
      snprintf(key, sizeof key, "%s", param_public_name(kn));
      int sym_id = comp_sym_intern(c, key);
      char cond[160], arg[128], missing[160];
      snprintf(cond, sizeof cond, "_sp_haskw && sp_poly_has_key(_sp_kwh, sp_box_sym((sp_sym)%d))", sym_id);
      snprintf(arg, sizeof arg, "sp_poly_index_poly(_sp_kwh, sp_box_sym((sp_sym)%d))", sym_id);
      /* A required keyword absent from the call raises ArgumentError (mirrors the
         method-keyword arm); an optional one falls back to its default. */
      snprintf(missing, sizeof missing, "(sp_raise_cls(\"ArgumentError\", \"missing keyword: :%s\"), sp_box_nil())", key);
      LocalVar *klv = scope_local(bs, kn);
      emit_proc_param_slot(c, pb, kn, cond, arg, dv, missing, klv ? klv->type : TY_POLY, 0);
    }
  }
  /* `**kw`: the whole trailing kwargs hash, or an empty hash when the caller
     passed none (#2648). Alongside named keywords it is a copy without them.
     A block lifted into a proc can have its kwrest typed as a concrete hash
     by the enclosing scope, so the boxed hash is unboxed into that type. */
  {
    int pnr = proc_params_node(c, create);
    int kwr = pnr >= 0 ? nt_ref(nt, pnr, "keyword_rest") : -1;
    const char *kwrty = kwr >= 0 ? nt_type(nt, kwr) : NULL;
    const char *krn = kwrty && sp_streq(kwrty, "KeywordRestParameterNode") ? nt_str(nt, kwr, "name") : NULL;
    if (krn) {
      g_needs_proc_poly_argslot = 1;
      if (nkw > 0) {
        buf_printf(pb, "    sp_RbVal _kwr_%s = sp_box_obj(sp_poly_hash_merge(sp_box_nil(), _sp_kwh),"
                       " SP_BUILTIN_POLY_POLY_HASH); SP_GC_ROOT_RBVAL(_kwr_%s);%c", krn, krn, 10);
        int nkw3 = 0;
        const int *kwn3 = nt_arr(nt, pnr, "keywords", &nkw3);
        for (int j = 0; j < nkw3; j++) {
          const char *kn = nt_str(nt, kwn3[j], "name");
          if (kn) buf_printf(pb, "    sp_PolyPolyHash_delete((sp_PolyPolyHash *)_kwr_%s.v.p, sp_box_sym((sp_sym)%d));%c",
                             krn, comp_sym_intern(c, param_public_name(kn)), 10);
        }
      }
      else
        buf_printf(pb, "    sp_RbVal _kwr_%s = _sp_haskw ? _sp_kwh"
                       " : sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH);"
                       " SP_GC_ROOT_RBVAL(_kwr_%s);%c", krn, krn, 10);   /* the empty hash is held by nothing else */
      LocalVar *klv = scope_local(bs, krn);
      TyKind kty = klv ? klv->type : TY_POLY;
      if (kty == TY_POLY_POLY_HASH || kty == TY_SYM_POLY_HASH || kty == TY_STR_POLY_HASH) {
        char src[160];
        snprintf(src, sizeof src, "_kwr_%s", krn);
        Buf ub = {0};
        emit_unbox_text(c, kty, src, &ub);
        buf_printf(pb, "    %s lv_%s = %s; SP_GC_ROOT(lv_%s); (void)lv_%s;%c",
                   c_type_name(kty), krn, ub.p, krn, krn, 10);
        free(ub.p);
      }
      else buf_printf(pb, "    sp_RbVal lv_%s = _kwr_%s; (void)lv_%s;%c", krn, krn, krn, 10);
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
  buf_printf(pb, "    for (int _sp_ac = 0; _sp_ac < argc%s && _sp_ac < 16; _sp_ac++)"
                 " _sp_proc_poly_args[_sp_ac] = sp_box_nil();\n", has_kwp ? " + _sp_haskw" : "");
  buf_puts(pb, "    _sp_proc_poly_ret = sp_box_nil();\n");
  for (int i = 0; i < locals.n; i++) {
    LocalVar *lv = scope_local(bs, locals.v[i]);
    /* a celled local is a captured var (accessed via _cap), not a fn-local */
    /* skip virtual &block slots (TY_UNKNOWN) but allow rescue-bind vars (TY_EXCEPTION) */
    /* a reassigned PARAMETER is already bound by the arg prologue above --
       re-declaring it is a C redefinition (#3309) */
    if (nameset_has(&params, locals.v[i]) || nameset_has(&dlocals, locals.v[i])) continue;
    if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) declare_local(c, pb, lv, 0);
  }
  free(dlocals.v);
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
  g_c_loop_depth = sv_loopd; g_in_proc_body = sv_inproc; g_c_ret_void = sv_cv;

  ren_unpark(&ren_sv);
  g_pre = sv_pre; g_indent = sv_indent; g_block_id = sv_block; g_block_nren = sv_bnren;
  g_block_param_name = sv_bpn; g_self = sv_self; g_result_var = sv_rv; g_ret_type = sv_rt;
  g_self_deref = sv_deref;
  g_ie_class_id = sv_iec;
  if (bs) { bs->class_id = sv_bcls; bs->is_cmethod = sv_bcm; }
  if (bs_moved) comp_scope_move_end();
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

  if (ie_mark >= 0) buf_puts(b, "({ sp_Proc *_pie = ");
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
      else if (self_boxed) { emit_indent(g_pre, g_indent); buf_printf(g_pre, "_capv_%d->__self_rb = %s;\n", pid, sv_self ? sv_self : "self"); }
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
  if (ie_mark >= 0) buf_printf(b, "; _pie->ie_cls = %d; _pie; })", ie_mark + 1);

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
         sp_streq(name, "Dir")       ||
         /* runtime value types with a typedef of their own (sp_Range, sp_Time,
            sp_File, sp_Class): a user struct under that name was a C-level
            typedef collision before any call was reached (activesupport's
            blank.rb reopens Range and Time) */
         sp_streq(name, "Range")     || sp_streq(name, "Time") ||
         sp_streq(name, "File")      || sp_streq(name, "Class") ||
         sp_streq(name, "Hash")      || io_family_name(name) ||
         /* a thread and a fiber are runtime handles too (activesupport's
            IsolatedExecutionState gives both an accessor) */
         sp_streq(name, "Thread")    || sp_streq(name, "Fiber") ||
         /* a builtin exception's reopening (`class LoadError; def is_missing?`)
            adds methods to the runtime's class: the value stays the runtime's
            sp_Exception, raised, rescued and constructed by name as before */
         is_builtin_exception_name(name);
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
    if (superclass_builtin_exc_name(c->nt, sc)) return 1;
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

/* Per-class exception facts, computed once per class table: bit 1 the class
   carries a builtin exception's name, bit 2 it is that builtin's reopening
   (no superclass of its own). The name test is a scan of the builtin table,
   and these are asked for every class at every call site the dispatch
   emits, so asking afresh each time made the emission grow with call sites
   times classes. */
static unsigned char *g_excf;
static int g_excf_n = -1, g_excf_any;
static const Compiler *g_excf_c;
static void excf_fill(Compiler *c) {
  if (g_excf_c == c && g_excf_n == c->nclasses) return;
  free(g_excf);
  g_excf = calloc((size_t)c->nclasses + 1, 1);
  g_excf_n = c->nclasses; g_excf_c = c; g_excf_any = 0;
  for (int k = 0; k < c->nclasses; k++) {
    if (!c->classes[k].name || !is_exc_name(c->classes[k].name)) continue;
    g_excf[k] = 1;
    if (nt_ref(c->nt, c->classes[k].def_node, "superclass") < 0) { g_excf[k] |= 2; g_excf_any = 1; }
  }
}
int any_exc_reopen(Compiler *c) { excf_fill(c); return g_excf_any; }
int class_has_exc_name(Compiler *c, int ci) {
  excf_fill(c);
  return ci >= 0 && ci < g_excf_n && (g_excf[ci] & 1);
}

/* A reopening of a builtin exception class: an entry under the builtin's
   name with no superclass of its own. */
int class_is_exc_reopen(Compiler *c, int ci) {
  excf_fill(c);
  return ci >= 0 && ci < g_excf_n && (g_excf[ci] & 2);
}

/* The reopenings of builtin exception classes that define method mname, at
   most max of them, in declaration order: a call on a value whose static type
   is the base exception picks among them by the runtime class name. Returns
   the count. */
int exc_reopen_definers(Compiler *c, const char *mname, int *out, int max) {
  int n = 0;
  excf_fill(c);
  if (!g_excf_any) return 0;
  for (int k = 0; k < c->nclasses && n < max; k++) {
    if (!(g_excf[k] & 2)) continue;
    int mi = comp_method_in_chain(c, k, mname, NULL);
    if (mi < 0 || c->scopes[mi].class_id != k) continue;
    out[n++] = k;
  }
  return n;
}

/* Emits the head of a pick among the reopenings xr[0..xn) that define one
   method: the one the runtime class cls_expr (a const char * expression)
   reaches first up its ancestry, as Ruby's lookup takes the most-derived
   definition whatever order the reopenings were written in. Leaves _xi<T>
   holding the index into xr (-1: none of them) and returns T. */
int emit_exc_reopen_pick_head(Compiler *c, const int *xr, int xn, const char *cls_expr, Buf *b) {
  int t = ++g_tmp;
  buf_printf(b, "static const char *const _xn%d[] = {", t);
  for (int q = 0; q < xn; q++) buf_printf(b, "%s\"%s\"", q ? ", " : "", c->classes[xr[q]].name);
  buf_printf(b, "}; int _xi%d = sp_exc_nearest_cls(%s, _xn%d, %d); ", t, cls_expr, t, xn);
  return t;
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
    const char *sn = superclass_builtin_exc_name(c->nt, sc);
    if (sn) return sn;
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
    switch (t) {
    case TY_STRING: buf_printf(b, "  sp_mark_string(o->iv_%s);\n", iv); break;
    case TY_POLY: buf_printf(b, "  sp_mark_rbval(o->iv_%s);\n", iv); break;
    /* a by-value struct the walker cannot follow: mark what it carries */
    case TY_STR_RANGE:
      buf_printf(b, "  sp_mark_string(o->iv_%s.first);\n", iv);
      buf_printf(b, "  sp_mark_string(o->iv_%s.last);\n", iv);
      break;
    default:
      if (needs_root(t))
        buf_printf(b, "  if (o->iv_%s) sp_gc_mark((void *)o->iv_%s);\n", iv, iv);
      break;
    }
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
  /* a Class slot's zero pattern is class index 0, a real class: `@k ||= String`
     kept it and answered the first class of the program (#5357) */
  if (t == TY_CLASS) return "SP_CLASS_NIL";
  return NULL;
}

/* Seed every ivar whose zero bit-pattern is not nil. A poly ivar's zero
   pattern has tag 0, not SP_TAG_NIL, so it must be set to sp_box_nil(); an int
   ivar's nil is SP_INT_NIL, not 0. `lv` is the receiver-and-accessor prefix
   ("self.", "self->", "_t3.", "_t3->"); each assignment is bracketed by `lead`
   (indentation) and `term` (`;\n` for a statement, `;` inside a compound expr).
   A string ivar's NULL zero-pattern already reads as nil, so it is skipped. */
static void emit_ivar_nil_inits_from(Buf *b, ClassInfo *ci, int from, const char *lv,
                                     const char *lead, const char *term) {
  for (int i = from; i < ci->nivars; i++) {
    const char *name = iv_c(ci->ivars[i] + 1);  /* skip leading '@', mangle to a C field */
    if (ci->ivar_types[i] == TY_POLY)
      buf_printf(b, "%s%siv_%s = sp_box_nil()%s", lead, lv, name, term);
    /* a Float slot initialize need not assign (ivar_nullable_int): its nil
       is the NaN sentinel, and 0.0 read as a real number */
    else if (ci->ivar_types[i] == TY_FLOAT && ci->ivar_nullable_int[i])
      buf_printf(b, "%s%siv_%s = SP_FLOAT_NIL_CONST%s", lead, lv, name, term);
    else {
      const char *nv = ivar_scalar_nil_init(ci->ivar_types[i]);
      if (nv) buf_printf(b, "%s%siv_%s = %s%s", lead, lv, name, nv, term);
    }
  }
}

static void emit_ivar_nil_inits(Buf *b, ClassInfo *ci, const char *lv,
                                const char *lead, const char *term) {
  emit_ivar_nil_inits_from(b, ci, 0, lv, lead, term);
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

static TyKind scope_param_type(Scope *s, int i) {
  LocalVar *p = scope_local(s, s->pnames[i]);
  return (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
}

static void emit_ctor_params(Compiler *c, int init, int init_has_blk, Buf *b) {
  if (init >= 0 && (c->scopes[init].nparams > 0 || init_has_blk)) {
    Scope *s = &c->scopes[init];
    for (int i = 0; i < s->nparams; i++) {
      if (i) buf_puts(b, ", ");
      TyKind pt = scope_param_type(s, i);
      emit_ctype(c, pt, b);
      buf_printf(b, " lv_%s", s->pnames[i]);
    }
    if (init_has_blk) {
      if (s->nparams > 0) buf_puts(b, ", ");
      buf_printf(b, "sp_Proc *const lv_%s", s->blk_param);
    }
  }
  else buf_puts(b, "void");
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
          TyKind pt = scope_param_type(si, i);
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
        TyKind pt = scope_param_type(si, i);
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
    for (int i = 0; i < ci->nmembers; i++) {
      if (i) buf_puts(b, ", ");
      emit_ctype(c, ci->ivar_types[i], b);
      buf_printf(b, " a%d", i);
    }
    if (ci->nmembers == 0) buf_puts(b, "void");
    buf_puts(b, ") {\n");
    /* Root every heap-backed member argument before allocating the struct:
       SP_POOL_NEW can trigger a GC, and a member value that is a fresh,
       otherwise-unrooted temporary (e.g. a `data[8, 8].delete("\0").upcase`
       WAD lump name) would be swept before it is stored into the ivar,
       leaving a dangling pointer the collector later reads (use-after-free). */
    for (int i = 0; i < ci->nmembers; i++) {
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
    for (int i = 0; i < ci->nmembers; i++)
      buf_printf(b, "  self->iv_%s = a%d;\n", iv_c(ci->ivars[i] + 1), i);  /* skip leading '@' */
    /* an attr's or a method's own ivar is no member: it starts nil */
    emit_ivar_nil_inits_from(b, ci, ci->nmembers, "self->", "  ", ";\n");
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
      /* the String built below allocates while the members are still to be
         read, and the caller may hold the object only in an unrooted temp
         (`p D.make(1)`): swept at the first allocation, the members read
         freed memory under SPINEL_GC_STRESS=1 */
      if (!ci->is_value_type) buf_puts(b, "  SP_GC_ROOT(self);\n");
      /* A member can hold the struct itself (`s.a = s`), and this function
         renders a member of its own class by calling straight back into
         itself. Stop at the object the render is already inside, as CRuby's
         #<struct S a=#<struct S:...>> does. Only a struct with members can be
         reached from inside itself, so a memberless one keeps its old body. */
      if (ci->nmembers > 0)
        buf_printf(b, "  if (sp_poly_recur_seen(SP_POLY_RECUR_INSPECT, self, NULL)) return \"#<%s %s:...>\";\n"
                      "  int _rcm = sp_poly_recur_push(SP_POLY_RECUR_INSPECT, self, NULL);\n",
                   ci->is_data ? "data" : "struct", ci->is_anon_struct ? "" : rn);
      /* an anonymous struct class has no name to show: #<struct a=1, b=2> */
      if (ci->is_anon_struct)
        /* the space that would precede the first member is CRuby's even when
           there is none to precede: `Struct.new.new.inspect` is "#<struct >" */
        buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s%s\"); SP_GC_ROOT(s);\n",
                   ci->is_data ? "data" : "struct", ci->nmembers == 0 ? " " : "");
      else
        buf_printf(b, "  sp_String *s = sp_String_new(\"#<%s %s\"); SP_GC_ROOT(s);\n",
                   ci->is_data ? "data" : "struct", rn);
      for (int i = 0; i < ci->nmembers; i++) {
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
      if (ci->nmembers > 0) buf_puts(b, "  sp_poly_recur_pop(_rcm);\n");
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
    emit_ctor_params(c, init, init_has_blk, b);
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
    buf_printf(b, "SP_UNUSED static sp_RbVal sp_box_vobj_%s(sp_%s v) {\n",
               ci->c_name, ci->c_name);
    for (int i = 0; i < ci->nivars; i++) {
      TyKind it = ci->ivar_types[i];
      const char *iv = iv_c(ci->ivars[i] + 1);
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
  emit_ctor_params(c, init, init_has_blk, b);
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
    /* below SystemCallError, #errno is what its initialize stores: an
       initialize of the program's own that never calls super leaves it nil,
       and with none at all SystemCallError#initialize runs on no arguments */
    if (class_is_syserr(c, cid)) {
      if (init >= 0) buf_puts(b, "  self->xkey = sp_box_nil();\n");
      else buf_puts(b, "  sp_syserr_super((sp_Exception *)self, 0, NULL);\n");
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
  /* the full constructor: allocate, then run the body through the clone,
     which takes the block as a proc. sp_X_new_blk is for a `new(..., &pr)`
     whose proc is known only at run time; sp_X_new passes none. */
  Scope *s = &c->scopes[init], *pf = &c->scopes[init_pf];
  buf_printf(b, "static sp_%s *sp_%s_new_blk(", ci->c_name, ci->c_name);
  for (int i = 0; i < s->nparams; i++) {
    emit_ctype(c, scope_param_type(s, i), b);
    buf_printf(b, " lv_%s, ", s->pnames[i]);
  }
  buf_printf(b, "sp_Proc *_sp_blk) {\n  sp_%s *self = sp_%s_new_noinit(", ci->c_name, ci->c_name);
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
  buf_puts(b, ", _sp_blk);\n  return self;\n}\n");
  buf_printf(b, "static sp_%s *sp_%s_new(", ci->c_name, ci->c_name);
  for (int i = 0; i < s->nparams; i++) {
    if (i) buf_puts(b, ", ");
    emit_ctype(c, scope_param_type(s, i), b);
    buf_printf(b, " lv_%s", s->pnames[i]);
  }
  if (s->nparams == 0) buf_puts(b, "void");
  buf_printf(b, ") {\n  return sp_%s_new_blk(", ci->c_name);
  for (int i = 0; i < s->nparams; i++) buf_printf(b, "lv_%s, ", s->pnames[i]);
  buf_puts(b, "NULL);\n}\n");
}

/* `allocate` on the class the method runs for (allocate_on_own_class). In a
   class method, the class its body is emitted for. In an instance method,
   the object's own class, picked by its id over `base` and the subclasses
   the program allocates, each cast to the base as a bare `new` in a class
   method is; a class with no such subclass allocates `base` alone. */
void emit_own_class_alloc(Compiler *c, int id, int base, Buf *b) {
  const NodeTable *nt = c->nt;
  Scope *s = comp_scope_of(c, id);
  char sel[256] = "";
  if (s && s->is_cmethod) {
    /* a class method is copied for each subclass it runs for, as one with
       a bare `new` is (cmethod_has_bare_new): the copy allocates its class */
    if (g_emitting_class_id >= 0) base = g_emitting_class_id;
  }
  else if (!comp_ty_value_obj(c, ty_object(base))) {
    int recv = nt_ref(nt, id, "receiver");
    Buf sb; memset(&sb, 0, sizeof sb);
    emit_expr(c, nt_ref(nt, recv, "receiver"), &sb);
    if (sb.p) snprintf(sel, sizeof sel, "(%s)->cls_id", sb.p);
    free(sb.p);
  }
  int nsub = 0;
  for (int k = 0; sel[0] && k < c->nclasses; k++)
    if (k != base && is_descendant(c, k, base) && c->classes[k].instantiated &&
        !class_is_exc_subclass(c, k) && !c->classes[k].is_native_class &&
        !comp_ty_value_obj(c, ty_object(k))) nsub++;
  if (!nsub) { emit_obj_alloc_expr(c, base, b); return; }
  int t = ++g_tmp;
  buf_printf(b, "({ int _ac%d = %s; ", t, sel);
  for (int k = 0; k < c->nclasses; k++) {
    if (k == base || !is_descendant(c, k, base) || !c->classes[k].instantiated ||
        class_is_exc_subclass(c, k) || c->classes[k].is_native_class ||
        comp_ty_value_obj(c, ty_object(k))) continue;
    buf_printf(b, "_ac%d == %d ? (sp_%s *)", t, k, c->classes[base].c_name);
    emit_obj_alloc_expr(c, k, b);
    buf_puts(b, " : ");
  }
  emit_obj_alloc_expr(c, base, b);
  buf_puts(b, "; })");
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
  if (class_is_exc_subclass(c, cid)) {
    /* an exception is built on sp_Exception's storage, as its constructor
       builds it: the name, parent and message the raise machinery reads */
    const char *cn2 = class_ruby_name(c, cid); if (!cn2) cn2 = ci->name;
    const char *par = exc_builtin_parent(c, cid);
    if (ci->nivars == 0) {
      /* allocate runs no initialize: below SystemCallError, #errno is nil */
      if (class_is_syserr(c, cid))
        buf_printf(b, "({ sp_Exception *_t%d = sp_exc_new_sub(\"%s\", \"%s\", (&(\"\\xff\")[1]));"
                      " _t%d->xkey = sp_box_nil(); _t%d; })", t, cn2, par, t, t);
      else buf_printf(b, "sp_exc_new_sub(\"%s\", \"%s\", (&(\"\\xff\")[1]))", cn2, par);
      return;
    }
    buf_printf(b, "({ sp_%s *_t%d = (sp_%s *)sp_gc_alloc(sizeof(sp_%s), NULL, sp_%s__gc_scan);"
                  " _t%d->cls_name = \"%s\"; _t%d->parent_cls_name = \"%s\";"
                  " _t%d->msg = (&(\"\\xff\")[1]); _t%d->result = sp_box_nil();"
                  " _t%d->xname = sp_box_nil(); _t%d->xkey = sp_box_nil(); _t%d->xrecv = sp_box_nil();",
               ci->c_name, t, ci->c_name, ci->c_name, ci->c_name, t, cn2, t, par, t, t, t, t, t);
    char lv[32]; snprintf(lv, sizeof lv, "_t%d->", t);
    emit_ivar_nil_inits(b, ci, lv, " ", ";");
    buf_printf(b, " _t%d; })", t);
    return;
  }
  if (is_val) {
    buf_printf(b, "({ sp_%s _t%d = {0}; _t%d.cls_id = %d;", ci->c_name, t, t, cid);
    char lv[32]; snprintf(lv, sizeof lv, "_t%d.", t);
    emit_ivar_nil_inits(b, ci, lv, " ", ";");
    buf_printf(b, " _t%d; })", t);
  }
  else {
    /* A proc or fiber body is written out ahead of the classes'
       constructors, and so ahead of the SP_POOL_DEFINE this names: declare
       the pool to it (a Class value's `new` built inline in a block did not
       compile). */
    if (g_in_proc_body || g_in_fiber_body) {
      if (g_pool_fwd_n < c->nclasses) {
        g_pool_fwd = realloc(g_pool_fwd, (size_t)c->nclasses);
        memset(g_pool_fwd + g_pool_fwd_n, 0, (size_t)(c->nclasses - g_pool_fwd_n));
        g_pool_fwd_n = c->nclasses;
      }
      if (!g_pool_fwd[cid]) {
        g_pool_fwd[cid] = 1;
        buf_printf(&g_proc_protos, "SP_POOL_DECLARE(%s)\n", ci->c_name);
      }
    }
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
   instance and fills its ivars from the loaded name/value pairs. Scalar, poly
   and nested-user-object ivars round-trip directly; an Array or Hash ivar
   loads as the loader's always-poly container and reaches a typed slot
   through the converting unbox every boxed container takes into one. A class
   carrying any other ivar type is left out and raises at runtime. */
static int marshal_container_type(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY: case TY_FLOAT_ARRAY: case TY_STR_ARRAY: case TY_POLY_ARRAY:
    case TY_STR_POLY_HASH: case TY_SYM_POLY_HASH: case TY_POLY_POLY_HASH:
      return 1;
    default:
      return ty_is_ptr_array(t);
  }
}
static int marshal_ivar_type_ok(TyKind t) {
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_STRING: case TY_BOOL:
    case TY_SYMBOL: case TY_BIGINT: case TY_POLY: case TY_NIL:
    /* the shared handle (`attr_reader :buf` and `obj.buf << x`) */
    case TY_STRBUF:
      return 1;
    default:
      /* a nested user object reloads with its real cls_id */
      return ty_is_object(t) || marshal_container_type(t);
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
static void emit_marshal_box_ivar(Compiler *c, TyKind t, const char *expr, Buf *b) {
  if (t == TY_POLY) { buf_puts(b, expr); return; }
  if (marshal_container_type(t)) {
    buf_printf(b, "(%s ? ", expr); emit_boxed_text(c, t, expr, b); buf_puts(b, " : sp_box_nil())");
    return;
  }
  if (t == TY_NIL)  { buf_puts(b, "sp_box_nil()"); return; }
  if (ty_is_object(t)) {
    buf_printf(b, "(%s ? sp_box_obj(%s, %d) : sp_box_nil())", expr, expr, ty_object_class(t));
    return;
  }
  switch (t) {
    case TY_INT:    buf_printf(b, "(%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(%s))", expr, expr); break;
    case TY_FLOAT:  buf_printf(b, "sp_box_float_or_nil(%s)", expr); break;
    case TY_STRING: buf_printf(b, "(%s ? sp_box_str(%s) : sp_box_nil())", expr, expr); break;
    /* the handle's live bytes, as a boxed handle's unbox reads them */
    case TY_STRBUF: buf_printf(b, "(%s ? sp_box_str(sp_String_cstr(%s)) : sp_box_nil())", expr, expr); break;
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
  if (marshal_container_type(t)) {
    buf_puts(b, "(val.tag == SP_TAG_NIL ? NULL : "); emit_unbox_text(c, t, "val", b); buf_puts(b, ")");
    return;
  }
  if (ty_is_object(t)) {
    buf_printf(b, "(val.tag == SP_TAG_OBJ ? (sp_%s *)val.v.p : NULL)", c->classes[ty_object_class(t)].c_name);
    return;
  }
  switch (t) {
    case TY_INT:    buf_puts(b, "(val.tag == SP_TAG_NIL ? SP_INT_NIL : (sp_int)sp_poly_to_i(val))"); break;
    case TY_FLOAT:  buf_puts(b, "(val.tag == SP_TAG_NIL ? sp_float_nil() : (sp_float)sp_poly_to_f(val))"); break;
    case TY_STRING: buf_puts(b, "(val.tag == SP_TAG_STR ? val.v.s : NULL)"); break;
    case TY_STRBUF: buf_puts(b, "(val.tag == SP_TAG_STR ? sp_String_new_shared(val.v.s) : NULL)"); break;
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
    for (int j = 0; j < ci->nmembers; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ (hash key) */
      const char *ivf = iv_c(iv);          /* C field id (mangled member) */
      buf_printf(b, "      sp_StrPolyHash_set(h, SPL(\"%s\"), ", iv);
      switch (mt) {
      case TY_INT: buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", ivf, ivf); break;
      case TY_STRING: buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", ivf, ivf); break;
      case TY_FLOAT: buf_printf(b, "sp_box_float_or_nil(o->iv_%s)", ivf); break;
      case TY_BOOL: buf_printf(b, "sp_box_bool(o->iv_%s)", ivf); break;
      case TY_SYMBOL: buf_printf(b, "sp_box_sym(o->iv_%s)", ivf); break;
      case TY_POLY: buf_printf(b, "o->iv_%s", ivf); break;
      default: buf_puts(b, "sp_box_nil()"); break;
      }
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

/* The class-side names a Class read out of a boxed slot answers, each a
   switch over the program's classes, since the typed constant's answer is
   compiled from the class table: `subclasses` lists the classes whose parent
   the class is, `allocate` builds a bare instance of a class that is not an
   exception (and the empty value of String, Array, Hash and Object, as the
   typed constant does), and a Struct class answers `members` and
   `keyword_init?`. A user class value is boxed by id, or by name once it
   has passed through a name-carrying box; sp_cls_answers_id reads both,
   and a builtin class is read by its name. The default arm raises the
   NoMethodError the call raised before, for a value that is not a class
   as for a class with no answer. Emitted only for a program that calls
   one of the names on a receiver typed poly or left unknown
   (g_gen_cls_answers). */
static void emit_cls_answers_dispatch(Compiler *c, Buf *b) {
  if (!g_gen_cls_answers) return;
  /* the temporaries the allocate arms name live in this function alone:
     hand the counter back so the rest of the program numbers as before */
  int tmp_saved = g_tmp;
  buf_puts(b, "static sp_int sp_cls_answers_id(sp_RbVal v) {\n"
              "  if (v.tag != SP_TAG_CLASS) return -1;\n"
              "  if (v.cls_id != SP_CLASS_BY_NAME) return v.cls_id;\n"
              "  if (!v.v.s) return -1;\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (is_builtin_reopen(c->classes[i].name)) continue;
    const char *cn = class_ruby_name(c, i);
    if (!cn) cn = c->classes[i].name;
    buf_printf(b, "  if (strcmp(v.v.s, \"%s\") == 0) return %d;\n", cn, i);
  }
  buf_puts(b, "  return -1;\n}\n");
  buf_puts(b, "static sp_PolyArray *sp_cls_subclasses(sp_RbVal v) {\n"
              "  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);\n"
              "  switch (sp_cls_answers_id(v)) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    if (is_builtin_reopen(c->classes[i].name)) continue;
    buf_printf(b, "    case %d:", i);
    for (int k = 0; k < c->nclasses; k++) {
      if (c->classes[k].parent != i || is_builtin_reopen(c->classes[k].name)) continue;
      if (c->classes[k].is_singleton_of) continue;   /* a singleton class is not listed */
      const char *kn = class_ruby_name(c, k);
      if (!kn) kn = c->classes[k].name;
      buf_printf(b, " sp_PolyArray_push(a, sp_box_class(((sp_Class){%d, SPL(\"%s\")})));", k, kn);
    }
    buf_puts(b, " return a;\n");
  }
  buf_puts(b, "    default: break;\n  }\n"
              "  sp_raise_nomethod(sp_nomethod_msg(\"subclasses\", v));\n  return a;\n}\n");
  buf_puts(b, "static sp_RbVal sp_cls_allocate(sp_RbVal v) {\n"
              "  switch (sp_cls_answers_id(v)) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (is_builtin_reopen(ci->name) || ci->is_native_class || !ci->instantiated ||
        comp_class_is_module(c, ci) || class_is_exc_subclass(c, i)) continue;
    Buf ax; memset(&ax, 0, sizeof ax);
    emit_obj_alloc_expr(c, i, &ax);
    buf_printf(b, "    case %d: return ", i);
    emit_boxed_text(c, ty_object(i), ax.p ? ax.p : "0", b);
    buf_puts(b, ";\n");
    free(ax.p);
  }
  buf_puts(b, "    default: break;\n  }\n"
              "  if (v.tag == SP_TAG_CLASS) {\n"
              "    const char *n = sp_class_val_name(v);\n"
              "    if (strcmp(n, \"String\") == 0) return sp_box_str(sp_str_dup_external((&(\"\\xff\")[1])));\n"
              "    if (strcmp(n, \"Array\") == 0) return sp_box_poly_array(sp_PolyArray_new());\n"
              "    if (strcmp(n, \"Hash\") == 0) return sp_box_obj(sp_PolyPolyHash_new(), SP_BUILTIN_POLY_POLY_HASH);\n"
              "    if (strcmp(n, \"Object\") == 0) return sp_box_obj(sp_Object_new(), SP_BUILTIN_OBJECT);\n"
              "  }\n"
              "  sp_raise_nomethod(sp_nomethod_msg(\"allocate\", v));\n  return sp_box_nil();\n}\n");
  buf_puts(b, "static sp_PolyArray *sp_cls_members(sp_RbVal v) {\n"
              "  sp_PolyArray *a = sp_PolyArray_new(); SP_GC_ROOT(a);\n"
              "  switch (sp_cls_answers_id(v)) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_struct || comp_cmethod_in_chain(c, i, "members", NULL) >= 0) continue;
    buf_printf(b, "    case %d:", i);
    for (int j = 0; j < ci->nmembers; j++)
      buf_printf(b, " sp_PolyArray_push(a, sp_box_sym(sp_sym_intern(\"%s\")));", ci->ivars[j] + 1);
    buf_puts(b, " return a;\n");
  }
  buf_puts(b, "    default: break;\n  }\n"
              "  sp_raise_nomethod(sp_nomethod_msg(\"members\", v));\n  return a;\n}\n");
  buf_puts(b, "static sp_RbVal sp_cls_keyword_init_p(sp_RbVal v) {\n"
              "  switch (sp_cls_answers_id(v)) {\n");
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    if (!ci->is_struct || comp_cmethod_in_chain(c, i, "keyword_init?", NULL) >= 0) continue;
    buf_printf(b, "    case %d: return %s;\n", i,
               ci->kw_init == 1 ? "sp_box_bool(TRUE)" : ci->kw_init == -1 ? "sp_box_bool(FALSE)" : "sp_box_nil()");
  }
  buf_puts(b, "    default: break;\n  }\n"
              "  sp_raise_nomethod(sp_nomethod_msg(\"keyword_init?\", v));\n  return sp_box_nil();\n}\n");
  g_tmp = tmp_saved;
}
static void emit_member_boxed(Compiler *c, TyKind mt, const char *ivf, Buf *b) {
  switch (mt) {
  case TY_INT: buf_printf(b, "(o->iv_%s == SP_INT_NIL ? sp_box_nil() : sp_box_int(o->iv_%s))", ivf, ivf); break;
  case TY_STRING: buf_printf(b, "(o->iv_%s ? sp_box_str(o->iv_%s) : sp_box_nil())", ivf, ivf); break;
  case TY_FLOAT: buf_printf(b, "sp_box_float_or_nil(o->iv_%s)", ivf); break;
  case TY_BOOL: buf_printf(b, "sp_box_bool(o->iv_%s)", ivf); break;
  case TY_SYMBOL: buf_printf(b, "sp_box_sym(o->iv_%s)", ivf); break;
  case TY_POLY: buf_printf(b, "o->iv_%s", ivf); break;
  default: {
    char fb[128]; snprintf(fb, sizeof fb, "o->iv_%s", ivf);
    Buf bx; memset(&bx, 0, sizeof bx); emit_boxed_text(c, mt, fb, &bx);
    buf_puts(b, bx.p ? bx.p : "sp_box_nil()"); free(bx.p);
    break;
  }
  }
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
    for (int j = 0; j < ci->nmembers; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *iv = ci->ivars[j] + 1;  /* member name, sans @ (sym key) */
      const char *ivf = iv_c(iv);          /* C field id (mangled member) */
      buf_printf(b, "      sp_SymPolyHash_set(h, sp_sym_intern(\"%s\"), ", iv);
      emit_member_boxed(c, mt, ivf, b);
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
    for (int j = 0; j < ci->nmembers; j++) {
      TyKind mt = ci->ivar_types[j];
      const char *ivf = iv_c(ci->ivars[j] + 1);
      buf_puts(b, "      sp_PolyArray_push(a, ");
      emit_member_boxed(c, mt, ivf, b);
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
  { int pfi = mi >= 0 ? scope_proc_form_of(c, mi) : -1; if (pfi >= 0) mi = pfi; }   /* the emitted clone (#3399) */
  char tail[64];
  if (mi >= 0 && obj_to_a_call_tail(&c->scopes[mi], tail, sizeof tail) && scope_has_callable_symbol(c, mi)) return mi;
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
    char tail[64] = "";
    obj_to_a_call_tail(&c->scopes[mi], tail, sizeof tail);   /* qualified in obj_to_a_method: rest/block only */
    snprintf(callx, sizeof callx, vobj ? "sp_%s_%s(*(sp_%s *)v.v.p%s)" : "sp_%s_%s((sp_%s *)v.v.p%s)",
             c->classes[defc].c_name, mc(c->scopes[mi].name), c->classes[defc].c_name, tail);
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
    buf_printf(b, "    case %d: if (!v.v.p) return sp_box_bool(1); return ", i);
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
    for (int j = 0; j < ci->nmembers; j++) {
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
    for (int j = 0; j < ci->nmembers; j++) {
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
   Kernel#Integer or Kernel#Float somewhere (Compiler.uses_kconv), or that
   converts a boxed `**` operand through its #to_hash (row 5,
   Compiler.uses_kw_to_hash; sp_kw_splat_conv judges the answer). */
static const char *const kconv_names[] = { "to_int", "to_i", "to_f", "to_str", NULL, "to_hash" };
static int kconv_row_wanted(Compiler *c, int w) {
  return w == 5 ? c->uses_kw_to_hash : w < 4 && c->uses_kconv;
}
static void emit_kconv_row(Compiler *c, int i, int w, int *rows, Buf *b) {
  int tmi = -1;
  int callee = conv_bridge_callee(c, i, kconv_names[w], TY_UNKNOWN, 1, &tmi);
  if (callee < 0) return;
  if ((*rows)++ == 0) buf_printf(b, "    case %d: switch (which) {\n", i);
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
static void emit_kconv_bridge(Compiler *c, Buf *b) {
  for (int i = 0; i < c->nclasses; i++) {
    for (int w = 0; w < 6; w++) {
      if (!kconv_row_wanted(c, w)) continue;
      int tmi = -1;
      int callee = conv_bridge_callee(c, i, kconv_names[w], TY_UNKNOWN, 1, &tmi);
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
    for (int w = 0; w < 4; w++)
      if (kconv_row_wanted(c, w)) emit_kconv_row(c, i, w, &rows, b);
    /* row 4: #to_f for a Math argument, which rb_to_float converts only
       for a Numeric (the runtime's sp_num_to_f) */
    if (c->uses_kconv && class_is_user_numeric(c, i)) {
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
    if (kconv_row_wanted(c, 5)) emit_kconv_row(c, i, 5, &rows, b);
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
  if (c->uses_kconv || c->uses_kw_to_hash) emit_kconv_bridge(c, b);
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
    /* The object itself is read after the builder's first allocation, and a
       caller may hand over a fresh temporary held nowhere else (`p A.new`,
       `A.new.inspect` boxed): unrooted, a collection there swept it and the
       walk read whatever reused its slot (#<A:0x @a=26, @x=60>). */
    if (ci->nivars > 0) buf_puts(b, "      SP_GC_ROOT(o);\n");
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
    /* an ivar nothing has set yet is not shown (ivar_set_kind): with one
       such, the separator ahead of each shown ivar is decided at run time */
    int any1 = 0;
    for (int j = 0; j < ci->nivars && !any1; j++) any1 = ivar_set_kind(c, i, ci->ivars[j]) == 1;
    if (any1) buf_puts(b, "      int _ivsep = 0; (void)_ivsep;\n");
    for (int j = 0; j < ci->nivars; j++) {
      char expr[160]; snprintf(expr, sizeof expr, "o->iv_%s", iv_c(ci->ivars[j] + 1));
      char tb[256];
      const char *set = any1 ? ivar_set_test(c, i, ci->ivars[j], expr, tb, sizeof tb) : NULL;
      if (set) buf_printf(b, "      if %s { ", set);
      else if (any1) buf_puts(b, "      ");
      if (any1)
        buf_printf(b, "sp_String_append(_s, _ivsep++ ? \", %s=\" : \" %s=\"); sp_String_append(_s, ",
                   ci->ivars[j], ci->ivars[j]);
      else
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
      else if (marshal_ivar_type_ok(ivt) && !marshal_container_type(ivt) && ivt != TY_UNKNOWN) {
        buf_puts(b, "sp_poly_inspect(");
        emit_marshal_box_ivar(c, ivt, expr, b);
        buf_puts(b, ")");
      }
      else if (ivt == TY_UNKNOWN) {
        buf_puts(b, "sp_poly_inspect(");
        emit_marshal_box_ivar(c, TY_INT, expr, b);
        buf_puts(b, ")");
      }
      else
        buf_puts(b, "\"#<?>\"");
      buf_puts(b, set ? "); }\n" : ");\n");
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
      char expr[160]; snprintf(expr, sizeof expr, "o->iv_%s", iv_c(ci->ivars[j] + 1));
      buf_printf(b, "      sp_mar_sym(b, \"%s\"); sp_mar_w(b, ", ci->ivars[j]);
      emit_marshal_box_ivar(c, ci->ivar_types[j], expr, b);
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
                   j ? "else " : "", ci->ivars[j], iv_c(ci->ivars[j] + 1));
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
  const char *cty = is_rest ? "sp_PolyArray" : p && p->type == TY_POLY_POLY_HASH ? "sp_PolyPolyHash" : "sp_SymPolyHash";
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "%s *_t%d = %s_new();\n", cty, t, cty);
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
  char tn[24]; snprintf(tn, sizeof tn, "_t%d", t);
  if (poly) emit_boxed_text(c, is_rest ? TY_POLY_ARRAY : TY_SYM_POLY_HASH, tn, b);
  else buf_puts(b, tn);
}

static void emit_zsuper_arg(Compiler *c, Scope *s, LocalVar *dst, TyKind dt, const char *pname, Buf *b);

/* A bare `super` passes the method's own positionals, in order, and its
   keywords by name, and the parent binds them as it binds any call's
   arguments. With a rest among them their count is the run time's, so they
   gather into one Array (emit_zsuper_gather); without, it is the method's
   positional count, laid out over the parent's parameters as a call of that
   many arguments is (arg_layout): a leading optional funded after the
   requireds, a rest taking the surplus, its posts the last ones. Bound slot
   by slot, `def m(x) = super` into `def m(a = 1, b)` gave a the x, a
   parent's rest and posts did not compile, and a count the parent cannot
   take went through unchecked. */
typedef struct {
  int gather;     /* the emit_zsuper_gather temp, or -1 */
  ArgLayout L;    /* the static layout, when not gathered */
  int kwrest;     /* the parent's **kwrest built from this method's keywords, or -1 */
  int kwsrc;      /* this method's `**`, boxed, the parent's keywords read, or -1 */
} ZSuper;

/* The method's positional parameters ahead of its rest or keywords. */
static int zsuper_npos(Compiler *c, Scope *s) {
  int npos = 0;
  while (npos < s->nparams && npos != s->rest_idx && npos != s->kwrest_idx &&
         !callee_param_is_declared_kwarg(c, s, s->pnames[npos])) npos++;
  return npos;
}

static int emit_zsuper_gather(Compiler *c, Scope *s, Scope *pm);

/* The parent's **kwrest from a bare super: this method's `**`, less the
   keywords the parent names, and the keywords the parent declares no
   parameter for. With neither, an empty Hash; with the `**` alone into a
   parent naming no keyword, the method's own. */
static int emit_zsuper_kwrest(Compiler *c, Scope *s, Scope *pm) {
  int nextra = 0, nnamed = 0;
  for (int i = 0; i < s->nparams; i++)
    if (i != s->kwrest_idx && callee_param_is_declared_kwarg(c, s, s->pnames[i]) &&
        !callee_param_is_declared_kwarg(c, pm, s->pnames[i])) nextra++;
  for (int i = 0; i < pm->nparams; i++)
    if (i != pm->kwrest_idx && callee_param_is_declared_kwarg(c, pm, pm->pnames[i])) nnamed++;
  /* the method's `**` passes whole only when the parent names no keyword */
  int own = s->kwrest_idx >= 0 && s->pnames[s->kwrest_idx];
  if (nextra == 0 && !(own && nnamed > 0)) return -1;
  /* the hash the parent's rest is: of any key when a call may bring one */
  int any = kwrest_any_key(c, pm);
  const char *hk = any ? "sp_PolyPolyHash" : "sp_SymPolyHash";
  int t = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "%s *_t%d = %s_new(); SP_GC_ROOT(_t%d);\n", hk, t, hk, t);
  if (s->kwrest_idx >= 0) {
    LocalVar *kv = scope_local(s, s->pnames[s->kwrest_idx]);
    Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[s->kwrest_idx], &txt);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s(_t%d, ", any ? "sp_kw_merge_any" : "sp_kwrest_merge_poly", t);
    if (kv && kv->type == TY_POLY) buf_puts(g_pre, txt.p);
    else emit_boxed_text(c, kv ? kv->type : TY_SYM_POLY_HASH, txt.p, g_pre);
    buf_puts(g_pre, ");\n");
    free(txt.p);
    /* less the keywords the parent names: those bind from it by name */
    for (int i = 0; i < pm->nparams; i++) {
      if (i == pm->kwrest_idx || !callee_param_is_declared_kwarg(c, pm, pm->pnames[i])) continue;
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, any ? "sp_PolyPolyHash_delete(_t%d, sp_box_sym((sp_sym)%d));\n"
                            : "sp_SymPolyHash_delete(_t%d, (sp_sym)%d);\n", t, comp_sym_intern(c, pm->pnames[i]));
    }
  }
  for (int i = 0; i < s->nparams; i++) {
    if (i == s->kwrest_idx || !callee_param_is_declared_kwarg(c, s, s->pnames[i]) ||
        callee_param_is_declared_kwarg(c, pm, s->pnames[i])) continue;
    LocalVar *ep = scope_local(s, s->pnames[i]);
    Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[i], &txt);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, any ? "sp_PolyPolyHash_set(_t%d, sp_box_sym((sp_sym)%d), "
                          : "sp_SymPolyHash_set(_t%d, (sp_sym)%d, ", t, comp_sym_intern(c, s->pnames[i]));
    if (ep && ep->type == TY_POLY) buf_puts(g_pre, txt.p);
    else emit_boxed_text(c, ep ? ep->type : TY_POLY, txt.p, g_pre);
    buf_puts(g_pre, ");\n");
    free(txt.p);
  }
  return t;
}

/* The keywords a bare super passes are this method's own, by name, and its
   `**`: a keyword the parent names and this method does not reads from the
   `**`, and they are checked as a call's are -- a required one neither
   supplies raises `missing keyword`, and with no **kwrest a key the parent
   does not name raises `unknown keyword`. Bound by name alone, the `**` was
   dropped: `def k(a, **o) = super` into `def k(a, k: 0)` bound k = 0 for
   `k(1, k: 5)`, and a keyword the parent does not take went through. */
static void zsuper_kw_begin(Compiler *c, Scope *s, Scope *pm, ZSuper *z) {
  const NodeTable *nt = c->nt;
  int pn = pm->def_node >= 0 ? nt_ref(nt, pm->def_node, "parameters") : -1;
  int kn = 0; const int *kws = pn >= 0 ? nt_arr(nt, pn, "keywords", &kn) : NULL;
  if (kn == 0) return;
  int own = s->kwrest_idx >= 0 && s->pnames[s->kwrest_idx];
  int extra = 0, missing = 0;
  for (int i = 0; i < s->nparams; i++)
    if (i != s->kwrest_idx && callee_param_is_declared_kwarg(c, s, s->pnames[i]) &&
        !callee_param_is_declared_kwarg(c, pm, s->pnames[i])) extra++;
  for (int k = 0; k < kn; k++) {
    const char *kty = nt_type(nt, kws[k]), *kpn = nt_str(nt, kws[k], "name");
    if (kty && sp_streq(kty, "RequiredKeywordParameterNode") && kpn &&
        !callee_param_is_declared_kwarg(c, s, kpn)) missing++;
  }
  if (!own && !missing && !(extra && pm->kwrest_idx < 0)) return;
  if (own) {
    LocalVar *kv = scope_local(s, s->pnames[s->kwrest_idx]);
    Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[s->kwrest_idx], &txt);
    z->kwsrc = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_RbVal _t%d = ", z->kwsrc);
    if (kv && kv->type == TY_POLY) buf_puts(g_pre, txt.p);
    else emit_boxed_text(c, kv ? kv->type : TY_SYM_POLY_HASH, txt.p, g_pre);
    buf_printf(g_pre, "; SP_GC_ROOT_RBVAL(_t%d);\n", z->kwsrc);
    free(txt.p);
  }
  int chk = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "{ static const char *const _kw%d[] = {", chk);
  for (int k = 0; k < kn; k++) {
    const char *kpn = nt_str(nt, kws[k], "name");
    if (kpn) buf_printf(g_pre, "\"%s\", ", kpn);
  }
  buf_printf(g_pre, "0}, *const _kr%d[] = {", chk);
  for (int k = 0; k < kn; k++) {
    const char *kty = nt_type(nt, kws[k]), *kpn = nt_str(nt, kws[k], "name");
    if (kpn && kty && sp_streq(kty, "RequiredKeywordParameterNode")) buf_printf(g_pre, "\"%s\", ", kpn);
  }
  buf_printf(g_pre, "0}, *const _kl%d[] = {", chk);
  for (int i = 0; i < s->nparams; i++)
    if (i != s->kwrest_idx && callee_param_is_declared_kwarg(c, s, s->pnames[i]))
      buf_printf(g_pre, "\"%s\", ", s->pnames[i]);
  buf_printf(g_pre, "0};\n");
  emit_indent(g_pre, g_indent);
  if (own) buf_printf(g_pre, "  sp_kwargs_verify_lit(_t%d, ", z->kwsrc);
  else buf_puts(g_pre, "  sp_kwargs_verify_lit(sp_box_nil(), ");
  buf_printf(g_pre, "_kw%d, _kr%d, _kl%d, NULL, %d); }\n", chk, chk, chk, pm->kwrest_idx < 0);
}

/* A parent saying `**nil` refuses the keywords a bare super passes it, ahead
   of its count, as it refuses a call's (emit_call_arity_check): this
   method's named keywords always, and its `**` when the run time finds a key
   in it; an empty `**` passes nothing to refuse. The parent takes neither as
   a positional Hash (zsuper_kw_positional) nor binds them anywhere, so
   `def m(a, **o) = super` into `def m(a, **nil)` returned for `m(1, z: 5)`
   where CRuby raises "no keywords accepted". */
static void zsuper_refuse_keywords(Compiler *c, Scope *s, Scope *pm) {
  if (!scope_refuses_keywords(c, pm)) return;
  for (int i = 0; i < s->nparams; i++) {
    if (i == s->kwrest_idx || !s->pnames[i] || !callee_param_is_declared_kwarg(c, s, s->pnames[i])) continue;
    emit_indent(g_pre, g_indent);
    buf_puts(g_pre, "sp_raise_cls(\"ArgumentError\", \"no keywords accepted\");\n");
    return;
  }
  if (s->kwrest_idx < 0 || !s->pnames[s->kwrest_idx]) return;
  LocalVar *kv = scope_local(s, s->pnames[s->kwrest_idx]);
  Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[s->kwrest_idx], &txt);
  emit_indent(g_pre, g_indent);
  buf_puts(g_pre, "if (sp_poly_length(");
  if (kv && kv->type == TY_POLY) buf_puts(g_pre, txt.p);
  else emit_boxed_text(c, kv ? kv->type : TY_SYM_POLY_HASH, txt.p, g_pre);
  buf_puts(g_pre, ") > 0) sp_raise_cls(\"ArgumentError\", \"no keywords accepted\");\n");
  free(txt.p);
}

/* Lay out a bare super from s into pm: the keywords a `**nil` parent
   refuses, then gather, or the static layout and its count check, the
   keywords' check, and the parent's **kwrest. */
static void zsuper_begin(Compiler *c, Scope *s, Scope *pm, ZSuper *z) {
  memset(z, 0, sizeof *z);
  z->kwrest = -1;
  z->kwsrc = -1;
  zsuper_refuse_keywords(c, s, pm);
  z->gather = emit_zsuper_gather(c, s, pm);
  if (z->gather < 0) {
    int npos = zsuper_npos(c, s);
    arg_layout(c, pm, NULL, npos, -1, 0, &z->L);
    int req = 0, total = 0;
    positional_arity(c, pm, &req, &total);
    int max = pm->rest_idx >= 0 ? -1 : total;
    if (npos < req || (max >= 0 && npos > max)) {
      char kw[256], given[16];
      scope_arity_kw_suffix(c, pm, kw, sizeof kw);
      snprintf(given, sizeof given, "%d", npos);
      emit_indent(g_pre, g_indent);
      emit_arity_raise(g_pre, given, req, max, kw);
      buf_puts(g_pre, ";\n");
    }
  }
  zsuper_kw_begin(c, s, pm, z);
  if (pm->kwrest_idx >= 0) z->kwrest = emit_zsuper_kwrest(c, s, pm);
}

static void zsuper_end(ZSuper *z) {
  if (z->gather < 0) arg_layout_free(&z->L);
}

/* The parent's parameter i for a bare super: a positional by the layout, a
   keyword from this method's like-named keyword, anything else its default
   or empty. This method's names read under the renames up to own_nren, the
   parent's defaults (a parent inlined in place) under those up to
   parent_nren. */
static void emit_zsuper_param(Compiler *c, Scope *s, Scope *pm, const ZSuper *z, int i,
                              int own_nren, int parent_nren, Buf *b) {
  int sv = g_nren;
  LocalVar *dst = scope_local(pm, pm->pnames[i]);
  TyKind dt = dst ? dst->type : TY_UNKNOWN;
  int kw = i != pm->kwrest_idx && callee_param_is_declared_kwarg(c, pm, pm->pnames[i]);
  LocalVar *src = kw && callee_param_is_declared_kwarg(c, s, pm->pnames[i]) ? scope_local(s, pm->pnames[i]) : NULL;
  if (src) {
    g_nren = own_nren;
    emit_zsuper_arg(c, s, dst, dt, pm->pnames[i], b);
  }
  else if (kw && z->kwsrc >= 0) {
    g_nren = parent_nren;
    emit_ds_param_extract(c, pm, i, z->kwsrc, TY_POLY, b);
  }
  else if (i == pm->kwrest_idx && (z->kwrest >= 0 || s->kwrest_idx >= 0)) {
    char tn[24];
    if (z->kwrest >= 0) snprintf(tn, sizeof tn, "_t%d", z->kwrest);
    g_nren = own_nren;
    if (z->kwrest < 0) {
      emit_zsuper_arg(c, s, dst, dt, s->pnames[s->kwrest_idx], b);
    }
    else if (dt == TY_POLY) emit_boxed_text(c, kwrest_any_key(c, pm) ? TY_POLY_POLY_HASH : TY_SYM_POLY_HASH, tn, b);
    else buf_puts(b, tn);
  }
  else if (z->gather >= 0 && !kw && i != pm->kwrest_idx) {
    /* a lent parameter ahead of the rest, or among the posts behind it, is
       this method's own, in place: the gather's copy of it would take the
       parent's appends */
    int own = dst && dst->byref_out ? zsuper_param_source(c, s, pm, i) : -1;
    if (own >= 0) {
      g_nren = own_nren;
      emit_zsuper_arg(c, s, dst, dt, s->pnames[own], b);
    }
    else {
      g_nren = parent_nren;
      emit_gathered_param(c, pm, i, z->gather, b);
    }
  }
  else if (z->gather < 0 && z->L.from[i] == ARG_NODE) {
    int a = z->L.arg[i];
    g_nren = own_nren;
    emit_zsuper_arg(c, s, dst, dt, s->pnames[a], b);
  }
  else if (z->gather < 0 && z->L.from[i] == ARG_REST) {
    /* the positionals between the requireds ahead and the posts behind */
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new();\n", t);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "SP_GC_ROOT(_t%d);\n", t);
    g_nren = own_nren;
    for (int k = pm->rest_idx; k < z->L.rest_argc - pm->npost_rest; k++) {
      LocalVar *ep = scope_local(s, s->pnames[k]);
      TyKind et = ep && ep->type != TY_UNKNOWN ? ep->type : TY_POLY;
      Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[k], &txt);
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "sp_PolyArray_push(_t%d, ", t);
      if (et == TY_POLY) buf_puts(g_pre, txt.p);
      else emit_boxed_text(c, et, txt.p, g_pre);
      buf_puts(g_pre, ");\n");
      free(txt.p);
    }
    char tn[24]; snprintf(tn, sizeof tn, "_t%d", t);
    if (dt == TY_POLY || dt == TY_UNKNOWN) emit_boxed_text(c, TY_POLY_ARRAY, tn, b);
    else buf_puts(b, tn);
  }
  else {
    g_nren = parent_nren;
    emit_zsuper_param_fill(c, pm, i, b);
  }
  g_nren = sv;
}

static void emit_zsuper_args(Compiler *c, Scope *s, Scope *pm, const char *sep0, Buf *b) {
  ZSuper z;
  zsuper_begin(c, s, pm, &z);
  for (int i = 0; i < pm->nparams; i++) {
    buf_puts(b, i == 0 ? sep0 : ", ");
    emit_zsuper_param(c, s, pm, &z, i, g_nren, g_nren, b);
  }
  zsuper_end(&z);
}

/* A bare `super` in a method taking `*rest`: CRuby passes its positionals
   with the rest spread among them, so how many reach the parent is known only
   at run time. Gather them into one Array rooted in the prelude and refuse a
   count the parent cannot take; -1 when the method has no named rest. */
static int emit_zsuper_gather(Compiler *c, Scope *s, Scope *pm) {
  int kwpos = zsuper_kw_positional(c, s, pm);
  LocalVar *rv = s->rest_idx >= 0 && s->pnames[s->rest_idx] ? scope_local(s, s->pnames[s->rest_idx]) : NULL;
  if (!rv && !kwpos) return -1;
  int ct = ++g_tmp;
  emit_indent(g_pre, g_indent);
  buf_printf(g_pre, "sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);\n", ct, ct);
  for (int i = 0; i < s->nparams && i != s->kwrest_idx &&
                  !callee_param_is_declared_kwarg(c, s, s->pnames[i]); i++) {
    LocalVar *ep = scope_local(s, s->pnames[i]);
    TyKind et = ep && ep->type != TY_UNKNOWN ? ep->type : TY_POLY;
    Buf txt; memset(&txt, 0, sizeof txt); emit_scope_local_ref(c, s, s->pnames[i], &txt);
    Buf ab; memset(&ab, 0, sizeof ab);
    if (et == TY_POLY) buf_puts(&ab, txt.p);
    else emit_boxed_text(c, et, txt.p, &ab);
    free(txt.p);
    emit_indent(g_pre, g_indent);
    if (i == s->rest_idx)
      buf_printf(g_pre, "sp_PolyArray_append_all(_t%d, sp_poly_to_poly_array(sp_splat_to_array(%s)));\n",
                 ct, ab.p);
    else buf_printf(g_pre, "sp_PolyArray_push(_t%d, %s);\n", ct, ab.p);
    free(ab.p);
  }
  if (kwpos) {
    /* the keywords, one more positional Hash for a parent taking none, as
       CRuby passes them: this method's `**`, then its named keywords, pushed
       when not empty. Dropped, `def n(kx: 90) = super` into `def n()` ran
       where CRuby raises `wrong number of arguments (given 1, expected 0)`. */
    int own = s->kwrest_idx >= 0 && s->pnames[s->kwrest_idx];
    int any = own && kwrest_any_key(c, s);
    const char *hk = any ? "sp_PolyPolyHash" : "sp_SymPolyHash";
    int t = ++g_tmp;
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "%s *_t%d = %s_new(); SP_GC_ROOT(_t%d);\n", hk, t, hk, t);
    if (own) {
      LocalVar *kv = scope_local(s, s->pnames[s->kwrest_idx]);
      char txt[128]; snprintf(txt, sizeof txt, "lv_%s", rename_local(s->pnames[s->kwrest_idx]));
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, "%s(_t%d, ", any ? "sp_kw_merge_any" : "sp_kwrest_merge_poly", t);
      if (kv && kv->type == TY_POLY) buf_puts(g_pre, txt);
      else emit_boxed_text(c, kv ? kv->type : TY_SYM_POLY_HASH, txt, g_pre);
      buf_puts(g_pre, ");\n");
    }
    for (int i = 0; i < s->nparams; i++) {
      if (i == s->kwrest_idx || !s->pnames[i] || !callee_param_is_declared_kwarg(c, s, s->pnames[i])) continue;
      LocalVar *kp = scope_local(s, s->pnames[i]);
      char txt[128]; snprintf(txt, sizeof txt, "lv_%s", rename_local(s->pnames[i]));
      emit_indent(g_pre, g_indent);
      buf_printf(g_pre, any ? "sp_PolyPolyHash_set(_t%d, sp_box_sym(sp_sym_intern(\"%s\")), "
                            : "sp_SymPolyHash_set(_t%d, sp_sym_intern(\"%s\"), ", t, s->pnames[i]);
      if (kp && kp->type == TY_POLY) buf_puts(g_pre, txt);
      else emit_boxed_text(c, kp ? kp->type : TY_POLY, txt, g_pre);
      buf_puts(g_pre, ");\n");
    }
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "if (_t%d->len > 0) sp_PolyArray_push(_t%d, sp_box_obj(_t%d, %s));\n",
               t, ct, t, any ? "SP_BUILTIN_POLY_POLY_HASH" : "SP_BUILTIN_SYM_POLY_HASH");
  }
  emit_gather_arity_check(c, pm, ct);
  return ct;
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
      if (emit_block_arg_proc(c, fe, b)) return;
      if (sp_streq(nt_type(c->nt, fe), "NilNode")) buf_puts(b, "NULL");
      else unsupported(c, blk, "super with a block argument that is not a proc");
      return;
    }
  }
  if (s->blk_param && s->blk_param[0] && !s->yields)
    emit_scope_local_ref(c, s, s->blk_param, b);
  /* the caller's block, implicitly forwarded from an inlined body */
  else if (s->yields && g_block_id >= 0) emit_proc_literal(c, g_block_id, b);
  /* or the proc driving the inlined body (`on(:x, &pr)`) */
  else if (s->yields && g_yield_proc_ref) buf_puts(b, g_yield_proc_ref);
  else buf_puts(b, "NULL");
}

/* Inline super { block } when the parent method uses yield.
   Returns 1 if the expansion was emitted, 0 if it should fall through to a
   regular function call (parent doesn't yield, has early return, etc.). */
int emit_super_inline(Compiler *c, int id, Buf *b, int indent, int as_expr) {
  if (g_plan_check) ucall_emitted(id);
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) return 0;
  refuse_super_splat(c, id, a_super_target(c, s));
  int p = comp_super_parent(c, s->class_id, s->is_cmethod);
  int defcls = -1;
  /* `super` inside a class method resolves through the parent's CLASS-method
     chain; the instance chain would miss `def self.x` entirely. */
  /* the next shadow in this class -- an earlier include's, prepend's or
     extend's copy -- is the parent here, as in emit_super's call form */
  const char *shadow = comp_super_shadow(c, s);
  /* the target is the super's plan (call_plan.c) when it was looked up where
     this splice looks: the shadow's class, or the parent's chain under this
     method's own name. The plan asks the chain for the user name
     (comp_super_name), which a proc-form clone's, a prepend's or an
     include's copy does not carry: the splice looks those up by the copy's
     name and leaves them to emit_super. Those, and under --plan-check as the
     assertion, take the lookup. */
  const CallPlan *spl = cplan_user(c, id);
  int mi = -1;
  if (spl->via == UC_SUPER && spl->mi >= 0 && spl->owner_ci == (shadow ? s->class_id : p) &&
      (shadow || (c->scopes[spl->mi].name && sp_streq(c->scopes[spl->mi].name, s->name)))) {
    mi = spl->mi;
    if (!shadow) defcls = c->scopes[mi].class_id;
  }
  if (g_plan_check && mi >= 0) cplan_served("super-inline");
  if (g_plan_check || mi < 0) {
    int odef = -1;
    int omi = shadow ? (s->is_cmethod ? comp_cmethod_in_class(c, s->class_id, shadow)
                                      : comp_method_in_class(c, s->class_id, shadow))
            : p < 0 ? -1
            : s->is_cmethod ? comp_cmethod_in_chain(c, p, s->name, &odef)
                            : comp_method_in_chain(c, p, s->name, &odef);
    if (mi < 0) {
      if (g_plan_check && omi >= 0)
        fprintf(stderr, "plan-check: cplan-fallback: super-inline node %d %s\n", id, s->name);
      mi = omi; defcls = odef;
    }
    else if (omi != mi || (!shadow && odef != defcls))
      fprintf(stderr, "plan-check: cplan-conflict: super-inline node %d %s: plan %d/%d, lookup %d/%d\n",
              id, s->name, mi, defcls, omi, odef);
  }
  if (mi < 0) return 0;
  Scope *m = &c->scopes[mi];
  if (!m->yields || scope_has_return(c, mi)) return 0;
  if (g_nren + m->nlocals >= MAX_RENAME) return 0;
  for (int i = 0; i < m->nlocals; i++) {
    LocalVar *lv = &m->locals[i];
    if (m->blk_param && lv->name && sp_streq(lv->name, m->blk_param)) continue;
    if (!is_scalar_ret(lv->type)) return 0;
  }
  int block = nt_ref(c->nt, id, "block");
  /* A bare `super` forwards the caller's block, which is the one currently
     being spliced into this (inlined) method. */
  Buf fwd_pb; memset(&fwd_pb, 0, sizeof fwd_pb);
  const char *fwd_yield_proc = NULL;
  int explicit_block_arg = 0;
  if (block < 0) block = g_block_id;
  /* So does `super(&)` or `super(&blk)` naming this method's block; a
     forwarded real proc drives the yields instead, as in an inlined
     `inner(&pr)`, and `super(&nil)` passes no block. */
  else if (nt_kind(c->nt, block) == NK_BlockArgumentNode) {
    block = resolve_forwarded_block(c, block);
    if (block >= 0 && nt_kind(c->nt, block) == NK_BlockArgumentNode) {
      emit_forwarded_proc_arg(c, block, &fwd_pb);
      if (fwd_pb.p && !sp_streq(fwd_pb.p, "NULL")) fwd_yield_proc = fwd_pb.p;
      explicit_block_arg = 1;
      block = -1;
    }
  }
  /* No block to splice: the parent is still only ever inlined (a yielding
     method has no C function of its own), and the call to an undefined
     sp_<Cls>_<m> did not link (#4852). A caller holding its block as a proc
     -- a declared `&blk`, or the one a super into a block-taking parent
     synthesizes -- drives the parent's yields through that proc, as an
     inlined `inner(&blk)` does; with neither, `block_given?` folds false. */
  if (!explicit_block_arg && block < 0 && s->blk_param && s->blk_param[0] && !s->yields) {
    emit_scope_local_ref(c, s, s->blk_param, &fwd_pb);
    fwd_yield_proc = fwd_pb.p;
  }

  /* --plan-check: the super is spliced from mi */
  if (g_plan_check) ucall_observe(c, id, mi, shadow ? s->class_id : defcls, 0);
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
  /* an explicit `&proc` or `&nil` replaces a lowered caller's own block */
  int saved_low = g_current_scope_is_lowered;
  if (explicit_block_arg) g_current_scope_is_lowered = 0;
  if (fwd_yield_proc || explicit_block_arg) {
    g_yield_proc_ref = fwd_yield_proc;
    g_yield_slot_ty = as_expr ? comp_ntype(c, id) : TY_UNKNOWN;
  }

  if (as_expr) buf_puts(b, "({\n");
  else { emit_indent(b, indent); buf_puts(b, "{\n"); }
  int din = indent + 1;

  emit_inlined_locals(c, m, tag, b, din);

  const char *ty = nt_type(c->nt, id);
  int is_forwarding = ty && sp_streq(ty, "ForwardingSuperNode");
  int args = nt_ref(c->nt, id, "arguments");
  int argc = 0;
  const int *argv = args >= 0 ? nt_arr(c->nt, args, "arguments", &argc) : NULL;
  /* Explicit arguments bind as an inlined call's do: binding them slot by
     slot put `super(*r)`'s whole Array into the first parameter, and a rest,
     post or keyword parameter took a positional. */
  /* `super(...)` forwards fixed __fwd_N slots topped up to the parent's
     arity, which cannot say that a caller left an argument out: an optional
     or rest parameter would bind a zero where its default or an empty Array
     belongs. */
  if (argc == 1 && nt_type(c->nt, argv[0]) && sp_streq(nt_type(c->nt, argv[0]), "ForwardingArgumentsNode")) {
    for (int i = 0; i < m->nparams; i++) {
      if (i == m->kwrest_idx || callee_param_is_declared_kwarg(c, m, m->pnames[i])) continue;
      if (i == m->rest_idx || i >= m->nrequired || (m->pdefault && m->pdefault[i] >= 0)) {
        unsupported_feature(c, id, "`super(...)` into a method that yields and takes an optional or *rest "
                                   "parameter: the forwarded arguments cannot leave one out");
        break;
      }
    }
  }
  InlDflt sv_dflt = inl_dflt_enter(m, g_nren, NULL, NULL, -1);
  if (!is_forwarding) {
    int pargc = argc;
    if (argc > 0 && nt_kind(c->nt, argv[argc - 1]) == NK_KeywordHashNode) pargc = argc - 1;
    ArgLayout L;
    arg_layout(c, m, argv, pargc, pargc < argc ? argv[pargc] : -1, 1, &L);
    emit_inline_bind_params(c, m, args, argv, argc, &L, 0, tag, saved_nren, din, b);
    arg_layout_free(&L);
  }
  ZSuper z;
  if (is_forwarding) {
    int sv = g_nren; g_nren = saved_nren;
    zsuper_begin(c, s, m, &z);
    g_nren = sv;
  }
  for (int i = 0; is_forwarding && i < m->nparams; i++) {
    emit_indent(b, din);
    { char rn[128]; snprintf(rn, sizeof rn, "_y%d_%s", tag, m->pnames[i]);
      emit_inlined_param_target(c, m, m->pnames[i], rn, b); }
    emit_zsuper_param(c, s, m, &z, i, saved_nren, g_nren, b);
    buf_puts(b, ";\n");
  }
  if (is_forwarding) zsuper_end(&z);
  inl_dflt_leave(sv_dflt);

  if (as_expr) {
    TyKind rt = comp_ntype(c, id);
    int rtag = ++g_tmp;
    char rvbuf[32]; snprintf(rvbuf, sizeof rvbuf, "_t%d", rtag);
    emit_indent(b, din); emit_ctype(c, rt, b);
    buf_printf(b, " _t%d = %s;\n", rtag, default_value_from_compiler(c, rt));
    const char *sv_rv = g_result_var; g_result_var = rvbuf;
    int sp = g_result_poly; g_result_poly = (rt == TY_POLY);
    TyKind srt = g_result_ty; g_result_ty = rt;
    emit_stmts_tail(c, m->body, b, din);
    g_result_var = sv_rv; g_result_poly = sp; g_result_ty = srt;
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
  g_current_scope_is_lowered = saved_low;
  free(fwd_pb.p);
  return 1;
}

/* super(args) / super -> call the parent's same-named method. */
/* One parameter a bare `super` forwards, converted to the parent's slot:
   boxed when the parent's is poly, and unboxed when this method's is poly and
   the parent's is typed -- a proc-form clone's parameters are boxed, and its
   super reaching the parent's plain method passed an sp_RbVal into an sp_int
   parameter, which the C compiler refused. */
static void emit_zsuper_arg(Compiler *c, Scope *s, LocalVar *dst, TyKind dt, const char *pname, Buf *b) {
  LocalVar *src = scope_local(s, pname);
  TyKind st = src ? src->type : TY_UNKNOWN;
  Buf _bx; memset(&_bx, 0, sizeof _bx);
  emit_scope_local_ref(c, s, pname, &_bx);
  /* A byref parent parameter takes a slot, lent as a call site lends a
     local argument (emit_lent_local): a lent parameter forwards the caller's
     slot, this method's own cell passes pinned, a proc body's capture its
     cell, a plain String its address, and any other value a temp. */
  if (dst && dst->byref_out) {
    if (!emit_lent_local(src, pname, b)) {
      Buf vb; memset(&vb, 0, sizeof vb);
      if (st == TY_POLY) emit_unbox_text(c, TY_STRING, _bx.p, &vb);
      else buf_puts(&vb, _bx.p);
      emit_lent_temp(vb.p, b);
      free(vb.p);
    }
    free(_bx.p);
    return;
  }
  if (dt == TY_POLY && st != TY_POLY && st != TY_UNKNOWN) emit_boxed_text(c, st, _bx.p, b);
  /* a POLY parameter handed to a parent parameter appended to in place: a
     plain String it holds becomes the shared handle first, as a marked read
     does at a call (poly_strbuf_lift, #6179) */
  else if (st == TY_POLY && dt == TY_POLY && (src->poly_lift & POLY_LIFT_ZSUPER) && dst &&
           (dst->poly_lift & POLY_LIFT_APPENDED))
    emit_poly_lift_ref(_bx.p, b);
  else if (st == TY_POLY && dt == TY_INT) buf_printf(b, "sp_poly_to_i_or_nil(%s)", _bx.p);
  else if (st == TY_POLY && dt == TY_FLOAT) buf_printf(b, "sp_poly_to_f_or_nil(%s)", _bx.p);
  else if (st == TY_POLY && dt != TY_POLY && dt != TY_UNKNOWN) emit_unbox_text(c, dt, _bx.p, b);
  else buf_puts(b, _bx.p);
  free(_bx.p);
}

/* `super(*a)` / `super(**h)` in a Struct or Data initialize: how many values
   arrive, and under which names, is known only when it runs. Gather the
   positionals into one array and the keywords into one hash, and set each
   member from them as Struct#initialize / Data#initialize would: by name for
   Data, a keyword_init Struct, or a plain Struct given keywords alone, else
   by position. Taking the splat as the first member's value failed in C, and
   a `**h` supplied no member at all. */
static void emit_struct_super_spread(Compiler *c, ClassInfo *cls, const int *argv, int argc, Buf *b) {
  const NodeTable *nt = c->nt;
  int kwh = argc > 0 && nt_kind(nt, argv[argc - 1]) == NK_KeywordHashNode ? argv[argc - 1] : -1;
  int npos = kwh >= 0 ? argc - 1 : argc;
  int ta = ++g_tmp, tl = ++g_tmp, th = ++g_tmp, tk = ++g_tmp;
  buf_printf(b, "({ sp_PolyArray *_t%d = sp_PolyArray_new(); SP_GC_ROOT(_t%d);", ta, ta);
  for (int a = 0; a < npos; a++) {
    if (nt_kind(nt, argv[a]) == NK_SplatNode) {
      int op = nt_ref(nt, argv[a], "expression");
      buf_printf(b, " sp_PolyArray_append_all(_t%d, ", ta);
      emit_splat_operand_array(c, op >= 0 ? op : argv[a], b);
      buf_puts(b, ");");
    }
    else {
      buf_printf(b, " sp_PolyArray_push(_t%d, ", ta);
      emit_boxed(c, argv[a], b);
      buf_puts(b, ");");
    }
  }
  buf_printf(b, " sp_RbVal _t%d = ", th);
  int kn = 0;
  const int *ke = kwh >= 0 ? nt_arr(nt, kwh, "elements", &kn) : NULL;
  Scope *sc = comp_scope_of(c, kwh >= 0 ? kwh : argv[0]);
  if (kn == 1 && nt_kind(nt, ke[0]) == NK_AssocSplatNode && nt_ref(nt, ke[0], "value") < 0 &&
      sc->kwrest_idx >= 0 && sc->kwrest_idx < sc->nparams && sc->pnames[sc->kwrest_idx]) {
    const char *kr = sc->pnames[sc->kwrest_idx];
    LocalVar *kv = scope_local(sc, kr);
    char src[64]; snprintf(src, sizeof src, "lv_%s", rename_local(kr));
    emit_boxed_text(c, kv && kv->type != TY_UNKNOWN ? kv->type : TY_SYM_POLY_HASH, src, b);
  }
  else if (kwh >= 0) emit_boxed(c, kwh, b);
  else buf_puts(b, "sp_box_nil()");
  buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d);", th);
  int keyed = cls->is_data || cls->kw_init > 0;
  const char *hash_given = kwh >= 0 ? "_t%d.tag != SP_TAG_NIL && sp_poly_length(_t%d) > 0" : "0";
  if (keyed) {
    buf_printf(b, " sp_int _t%d = sp_PolyArray_length(_t%d);", tl, ta);
    { char gv[32]; snprintf(gv, sizeof gv, "_t%d", tl);
      buf_puts(b, " ");
      emit_arity_check(b, gv, 0, 0, NULL);
      buf_puts(b, ";"); }
    buf_printf(b, " sp_bool _t%d = 1;", tk);
    /* every member the keywords leave out is named in one message, as
       Data#initialize names them */
    if (cls->is_data && cls->nmembers > 0) {
      Buf ml; memset(&ml, 0, sizeof ml);
      for (int a = 0; a < cls->nmembers; a++) buf_printf(&ml, "\"%s\", ", cls->ivars[a] + 1);
      buf_printf(b, " sp_kwargs_verify(_t%d, (const char *const[]){%s0}, (const char *const[]){%s0},"
                    " (const char *const[]){0}, 0);", th, ml.p, ml.p);
      free(ml.p);
    }
  }
  else {
    buf_printf(b, " sp_bool _t%d = ", tk);
    if (cls->kw_init == 0 && kwh >= 0) {
      buf_printf(b, "sp_PolyArray_length(_t%d) == 0 && (", ta);
      buf_printf(b, hash_given, th, th);
      buf_puts(b, ");");
    }
    else buf_puts(b, "0;");
    if (kwh >= 0) {
      buf_printf(b, " if (!_t%d && (", tk);
      buf_printf(b, hash_given, th, th);
      buf_printf(b, ")) sp_PolyArray_push(_t%d, _t%d);", ta, th);
    }
    buf_printf(b, " sp_int _t%d = sp_PolyArray_length(_t%d);", tl, ta);
    buf_printf(b, " if (_t%d > %d) sp_raise_cls(\"ArgumentError\", (&(\"\\xff\" \"struct size differs\")[1]));", tl, cls->nmembers);
  }
  for (int a = 0; a < cls->nmembers; a++) {
    const char *mname = cls->ivars[a] + 1;
    Buf ev; memset(&ev, 0, sizeof ev);
    buf_printf(&ev, "(_t%d ? ", tk);
    if (kwh >= 0) {
      buf_printf(&ev, "({ sp_bool _f; sp_RbVal _v = _t%d.tag != SP_TAG_NIL ? sp_poly_hash_probe(_t%d, sp_box_sym(sp_sym_intern(\"%s\")), &_f) : (_f = 0, sp_box_nil()); _f ? _v : ",
                 th, th, mname);
      if (cls->is_data) buf_printf(&ev, "(sp_raise_cls(\"ArgumentError\", \"missing keyword: :%s\"), sp_box_nil())", mname);
      else buf_puts(&ev, "sp_box_nil()");
      buf_puts(&ev, "; })");
    }
    else if (cls->is_data) buf_printf(&ev, "(sp_raise_cls(\"ArgumentError\", \"missing keyword: :%s\"), sp_box_nil())", mname);
    else buf_puts(&ev, "sp_box_nil()");
    buf_printf(&ev, " : (%d < _t%d ? sp_PolyArray_get(_t%d, %d) : sp_box_nil()))", a, tl, ta, a);
    buf_printf(b, " %s->iv_%s = ", g_self, iv_c(mname));
    if (cls->ivar_types[a] == TY_POLY) buf_puts(b, ev.p);
    else emit_unbox_nilable_text(c, cls->ivar_types[a], ev.p, b);
    buf_puts(b, ";");
    free(ev.p);
  }
  buf_puts(b, " sp_box_nil(); })");
}

/* A Struct's or Data's `super(a)` / `super(a: a)` handing on an initialize
   parameter that is the shared handle (#6179): the member, typed from it
   (struct_super_types_members), takes the handle itself, as the bare
   `super` does, so an append after the `super` reaches the member. 1 when
   it emitted. */
static int struct_super_handle_arg(Compiler *c, int v, Buf *b) {
  if (v < 0 || nt_kind(c->nt, v) != NK_LocalVariableReadNode) return 0;
  const char *vn = nt_str(c->nt, v, "name");
  Scope *vs = vn ? comp_scope_of(c, v) : NULL;
  LocalVar *lv = vs ? scope_local(vs, vn) : NULL;
  if (!lv || !lv->dyn_handle || lv->type != TY_STRBUF || !lv->str_shared) return 0;
  emit_local_ref(c, v, vn, b);
  return 1;
}

/* emit_super's target from the super's plan (call_plan.c) when the plan was
   looked up in `owner` (the shadow's class, or the parent's chain), or -1.
   The form's own lookup runs when this gives -1, and under --plan-check,
   where super_plan_check holds the two against each other (`what` names the
   form). */
static int super_plan_mi(Compiler *c, int id, int owner) {
  const CallPlan *spl = cplan_user(c, id);
  return spl->via == UC_SUPER && spl->mi >= 0 && owner >= 0 && spl->owner_ci == owner ? spl->mi : -1;
}

static void super_plan_check(int id, const char *what, const char *name, int served, int mi, int dcls,
                             int omi, int odef) {
  if (!served) {
    if (omi >= 0) fprintf(stderr, "plan-check: cplan-fallback: super node %d %s (%s)\n", id, name, what);
  }
  else if (omi != mi || odef != dcls)
    fprintf(stderr, "plan-check: cplan-conflict: super node %d %s (%s): plan %d/%d, lookup %d/%d\n",
            id, name, what, mi, dcls, omi, odef);
}

void emit_super(Compiler *c, int id, Buf *b) {
  if (g_plan_check) ucall_emitted(id);
  { Scope *ss = comp_scope_of(c, id);
    if (ss && ss->class_id >= 0 && ss->name) refuse_super_splat(c, id, a_super_target(c, ss)); }
  Scope *s = comp_scope_of(c, id);
  if (s->class_id < 0 || !s->name) { unsupported(c, id, "super (not in a method)"); return; }
  const char *ty = nt_type(c->nt, id);
  /* Prepend chain: super goes to the next shadow in the same class (a class
     method's chain is taken below, in the class-method form). */
  const char *shadow = s->is_cmethod ? NULL : comp_super_shadow(c, s);
  if (shadow) {
    buf_printf(b, "sp_%s_%s((sp_%s *)%s",
               c->classes[s->class_id].c_name, mc(shadow),
               c->classes[s->class_id].c_name, g_self);
    /* the shadow's method in this class: the plan's, or the latest scope of
       the shadow's name here */
    int smi = super_plan_mi(c, id, s->class_id);
    if (smi >= 0 && !(c->scopes[smi].class_id == s->class_id && c->scopes[smi].name &&
                      sp_streq(c->scopes[smi].name, shadow)))
      smi = -1;
    int served = smi >= 0;
    if (g_plan_check && served) cplan_served("super");
    if (g_plan_check || !served) {
      int omi = -1;
      for (int k = c->nscopes - 1; k >= 1; k--) {
        Scope *sc = &c->scopes[k];
        if (sc->class_id == s->class_id && sc->name && sp_streq(sc->name, shadow))
          { omi = k; break; }
      }
      if (g_plan_check) super_plan_check(id, "shadow", shadow, served, smi, 0, omi, 0);
      if (!served) smi = omi;
    }
    if (g_plan_check && smi >= 0) ucall_observe(c, id, smi, s->class_id, 0);
    if (ty && sp_streq(ty, "ForwardingSuperNode") && smi >= 0) {
      /* laid out over the shadow's parameters as any bare super is: passed
         slot by slot, a `**` went into the shadow's first keyword */
      emit_zsuper_args(c, s, &c->scopes[smi], ", ", b);
    }
    else if (ty && sp_streq(ty, "ForwardingSuperNode")) {
      for (int i = 0; i < s->nparams; i++) { buf_puts(b, ", "); emit_scope_local_ref(c, s, s->pnames[i], b); }
    }
    else emit_args_filled(c, smi, nt_ref(c->nt, id, "arguments"), ", ", b);
    /* the shadow's `&b` is a C parameter like any parent's: left off, a module
       method taking a block did not link */
    if (smi >= 0) emit_super_block_arg(c, id, s, &c->scopes[smi], 1, b);
    buf_puts(b, ")");
    return;
  }
  /* Strip __prep_N_ prefix to get the user method name for parent chain lookup. */
  int p = comp_super_parent(c, s->class_id, s->is_cmethod);
  const char *uname = comp_super_name(c, p, s->name, s->is_cmethod);
  /* super inside a class method: resolve through the parent's CLASS-method
     chain and call the sp_<Cls>_s_ form (class methods take no instance
     self). The instance path below would miss `def self.x` entirely. */
  if (s->is_cmethod) {
    if (comp_super_is_class_new(c, id)) { emit_super_class_new(c, id, b); return; }
    int cdef = -1;
    int cmi = -1;
    /* a later extend's copy: super reaches the earlier module's copy, kept in
       this class under its shadow name */
    const char *cshadow = comp_super_shadow(c, s);
    /* the plan's class method when it was looked up where this form looks */
    cmi = super_plan_mi(c, id, cshadow ? s->class_id : p);
    if (cmi >= 0 && !c->scopes[cmi].is_cmethod) cmi = -1;
    int served = cmi >= 0;
    if (served) cdef = cshadow ? s->class_id : c->scopes[cmi].class_id;
    if (g_plan_check && served) cplan_served("super");
    if (g_plan_check || !served) {
      int odef = -1, omi;
      if (cshadow) {
        omi = comp_cmethod_in_class(c, s->class_id, cshadow);
        odef = s->class_id;
      }
      else omi = p >= 0 ? comp_cmethod_in_chain(c, p, uname, &odef) : -1;
      if (g_plan_check) super_plan_check(id, "class method", uname, served, cmi, cdef, omi, odef);
      if (!served) { cmi = omi; cdef = odef; }
    }
    if (cshadow) uname = cshadow;
    if (cmi < 0) {
      const char *scn2 = class_ruby_name(c, s->class_id);
      if (!scn2) scn2 = c->classes[s->class_id].name;
      buf_printf(b, "(sp_raise_cls(\"NoMethodError\", \"super: no superclass method '%s' for %s\"), %s)",
                 uname, scn2, raise_tail_value_c(c, comp_ntype(c, id)));
      return;
    }
    if (g_plan_check) ucall_observe(c, id, cmi, cdef, 0);
    buf_printf(b, "sp_%s_s_%s(", c->classes[cdef].c_name, mc(uname));
    /* `super` in a class method keeps the receiving class: forward ours. */
    if (cmethod_takes_self_cls(c, cmi))
    {
      /* a body that does not take its receiving class is only ever run on
         its own class: pass that, not "none", or the parent's body builds
         and names the parent (#5995) */
      char own[48];
      snprintf(own, sizeof own, "((sp_Class){%d, NULL})", s->class_id);
      buf_printf(b, "%s%s", cmethod_takes_self_cls(c, (int)(s - c->scopes)) ? "_sp_cls" : own,
                 c->scopes[cmi].nparams > 0 ? ", " : "");
    }
    if (ty && sp_streq(ty, "ForwardingSuperNode")) emit_zsuper_args(c, s, &c->scopes[cmi], "", b);
    else emit_args_filled(c, cmi, nt_ref(c->nt, id, "arguments"), "", b);
    emit_super_block_arg(c, id, s, &c->scopes[cmi],
                         c->scopes[cmi].nparams > 0 || cmethod_takes_self_cls(c, cmi), b);
    buf_puts(b, ")");
    return;
  }
  int defcls = -1;
  /* the plan's method when it was looked up in the parent's chain */
  int mi = super_plan_mi(c, id, p);
  if (mi >= 0 && c->scopes[mi].is_cmethod) mi = -1;
  int served = mi >= 0;
  if (served) defcls = c->scopes[mi].class_id;
  if (g_plan_check && served) cplan_served("super");
  if (g_plan_check || !served) {
    int odef = -1;
    int omi = p >= 0 ? comp_method_in_chain(c, p, uname, &odef) : -1;
    if (g_plan_check) super_plan_check(id, "instance", uname ? uname : "?", served, mi, defcls, omi, odef);
    if (!served) { mi = omi; defcls = odef; }
  }
  if (g_plan_check && mi >= 0) ucall_observe(c, id, mi, defcls, 0);
  if (mi < 0) {
    /* super(msg) in exception subclass initialize: capture msg into self->msg */
    if ((class_is_exc_subclass(c, s->class_id) || class_is_exc_reopen(c, s->class_id)) &&
        uname && sp_streq(uname, "initialize")) {
      int args_id = nt_ref(c->nt, id, "arguments");
      int argc2 = 0;
      const int *argv2 = NULL;
      if (args_id >= 0) argv2 = nt_arr(c->nt, args_id, "arguments", &argc2);
      /* SystemCallError#initialize(msg = nil, func = nil) builds the message
         from the class's errno ("No such file or directory - msg") and
         stores #errno; a bare `super` forwards this initialize's parameters */
      if (class_is_syserr(c, s->class_id)) {
        char lead[64]; snprintf(lead, sizeof lead, "(sp_Exception *)%s, ", g_self);
        if (ty && sp_streq(ty, "ForwardingSuperNode")) {
          /* a declared keyword (`def initialize(msg:)`) goes over as a Hash
             of keywords, which the loop below would pass by position */
          int pnd = s->def_node >= 0 ? nt_ref(c->nt, s->def_node, "parameters") : -1;
          int nkwd = 0;
          if (pnd >= 0) (void)nt_arr(c->nt, pnd, "keywords", &nkwd);
          if (s->rest_idx >= 0 || s->kwrest_idx >= 0 || nkwd > 0 || nt_ref(c->nt, id, "block") >= 0)
            unsupported(c, id, "a bare super forwarding a rest, keyword or block parameter to SystemCallError#initialize");
          if (s->nparams == 0) { buf_printf(b, "sp_syserr_super(%s0, NULL)", lead); return; }
          int t0 = g_tmp + 1;
          g_tmp += s->nparams;
          buf_puts(b, "({ ");
          for (int k = 0; k < s->nparams; k++) {
            LocalVar *pk = scope_local(s, s->pnames[k]);
            Buf rn; memset(&rn, 0, sizeof rn); emit_scope_local_ref(c, s, s->pnames[k], &rn);
            TyKind pt = (pk && pk->type != TY_UNKNOWN) ? pk->type : TY_POLY;
            buf_printf(b, "sp_RbVal _t%d = ", t0 + k);
            emit_boxed_text(c, pt, rn.p ? rn.p : "0", b);
            buf_printf(b, "; SP_GC_ROOT_RBVAL(_t%d); ", t0 + k);
            free(rn.p);
          }
          buf_printf(b, "sp_syserr_super(%s%d, (sp_RbVal[]){", lead, s->nparams);
          for (int k = 0; k < s->nparams; k++) buf_printf(b, "%s_t%d", k ? ", " : "", t0 + k);
          buf_puts(b, "}); })");
          return;
        }
        emit_syserr_call(c, id, "sp_syserr_super", lead, argc2, argv2, b);
        return;
      }
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
        Buf rn; memset(&rn, 0, sizeof rn); emit_scope_local_ref(c, s, s->pnames[0], &rn);
        /* Effective type mirrors emit_method_signature: a NULL/TY_UNKNOWN
           param is declared TY_POLY (sp_RbVal), so it too must be coerced. */
        TyKind pt = (p0 && p0->type != TY_UNKNOWN) ? p0->type : TY_POLY;
        if (pt == TY_POLY)
          buf_printf(b, "(%s->msg = sp_poly_to_s(%s))", g_self, rn.p);
        else
          buf_printf(b, "(%s->msg = %s)", g_self, rn.p);
        free(rn.p);
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
      if (!is_fwd && struct_super_spreads(c, args_id)) {
        Buf sb; memset(&sb, 0, sizeof sb);
        emit_struct_super_spread(c, cls, sargv, an, &sb);
        buf_printf(b, "((void)%s, %s)", sb.p, default_value_from_compiler(c, comp_ntype(c, id)));
        free(sb.p);
        return;
      }
      int cnt = (kwh >= 0 || is_fwd) ? cls->nmembers : an;
      /* the members a keyword super leaves out, all named in one message */
      char kmiss[600] = "";
      if (kwh >= 0) {
        char ml[512] = ""; int nm = 0;
        for (int a = 0; a < cls->nmembers; a++)
          if (struct_kwarg_value(c, kwh, cls->ivars[a] + 1) < 0) {
            char iv[300]; snprintf(iv, sizeof iv, ":%s", cls->ivars[a] + 1);
            kw_names_add(ml, sizeof ml, &nm, iv);
          }
        if (nm) kw_error_message(kmiss, sizeof kmiss, "missing", nm, ml);
      }
      buf_puts(b, "(");
      for (int a = 0; a < cls->nmembers && a < cnt; a++) {
        TyKind ivt = cls->ivar_types[a];
        int roff = -1;
        int pk = is_fwd ? struct_zsuper_param(c, s, a, cls->ivars[a] + 1, &roff) : -1;
        if (is_fwd && pk < 0) continue;
        buf_printf(b, "%s->iv_%s = ", g_self, iv_c(cls->ivars[a] + 1));
        if (is_fwd && roff >= 0) {
          LocalVar *rv = scope_local(s, s->pnames[pk]);
          Buf src; memset(&src, 0, sizeof src); emit_scope_local_ref(c, s, s->pnames[pk], &src);
          Buf ra; memset(&ra, 0, sizeof ra);
          emit_boxed_text(c, rv ? rv->type : TY_POLY_ARRAY, src.p, &ra);
          Buf el; memset(&el, 0, sizeof el);
          buf_printf(&el, "sp_poly_index_poly(%s, sp_box_int(%d))", ra.p ? ra.p : "sp_box_nil()", roff);
          emit_unbox_nilable_text(c, ivt, el.p, b);
          free(ra.p); free(el.p); free(src.p);
        }
        else if (is_fwd) {
          LocalVar *pv = scope_local(s, s->pnames[pk]);
          TyKind at = pv && pv->type != TY_UNKNOWN ? pv->type : TY_POLY;
          Buf src; memset(&src, 0, sizeof src); emit_scope_local_ref(c, s, s->pnames[pk], &src);
          if (ivt == TY_POLY && at == TY_FLOAT) buf_printf(b, "sp_box_float_or_nil(%s)", src.p);
          /* a parameter the initialize appends to is the handle (#6179); a
             String member holds its bytes, copied, since the handle's next
             growing append moves them */
          else if (ivt == TY_STRING && at == TY_STRBUF)
            buf_printf(b, "(%s ? sp_str_concat(sp_String_cstr(%s), (&(\"\\xff\")[1])) : NULL)", src.p, src.p);
          else if (ivt == TY_POLY && at != TY_POLY) { Buf ex; memset(&ex, 0, sizeof ex); emit_boxed_text(c, at, src.p, &ex); buf_puts(b, ex.p ? ex.p : ""); free(ex.p); }
          else if (ivt != TY_POLY && at == TY_POLY) emit_unbox_nilable_text(c, ivt, src.p, b);
          else buf_puts(b, src.p);
          free(src.p);
        }
        else if (kwh >= 0) {
          int vnode = struct_kwarg_value(c, kwh, cls->ivars[a] + 1);
          if (vnode < 0) {
            /* CRuby raises ArgumentError when a keyword super omits a member.
               The trailing zero-value is dead (sp_raise_cls is noreturn) but
               still type-checks against the member slot -- a value-type-object
               member is an inline struct, so `NULL` won't assign; give it the
               compound-literal zero instead. */
            buf_printf(b, "(sp_raise_cls(\"ArgumentError\", \"%s\"), ", kmiss);
            if (comp_ty_value_obj(c, ivt))
              buf_printf(b, "(sp_%s){0})", c->classes[ty_object_class(ivt)].c_name);
            else
              buf_printf(b, "%s)", default_value_from_compiler(c, ivt));
          }
          else {
            TyKind at = comp_ntype(c, vnode);
            if (ivt == TY_STRBUF && struct_super_handle_arg(c, vnode, b)) {}
            else if (ivt == TY_POLY && at != TY_POLY) emit_boxed(c, vnode, b);
            else if (ivt != TY_POLY && at == TY_POLY) {
              Buf ex; memset(&ex, 0, sizeof ex); emit_expr(c, vnode, &ex);
              emit_unbox_nilable_text(c, ivt, ex.p ? ex.p : "", b); free(ex.p);
            }
            else emit_expr(c, vnode, b);
          }
        }
        else {
          TyKind at = comp_ntype(c, sargv[a]);
          if (ivt == TY_STRBUF && struct_super_handle_arg(c, sargv[a], b)) {}
          else if (ivt == TY_POLY && at != TY_POLY) emit_boxed(c, sargv[a], b);
          else if (ivt != TY_POLY && at == TY_POLY) {
            /* poly arg (e.g. an initialize param that stayed poly) into a scalar
               member slot: unbox to the member's C type. */
            Buf ex; memset(&ex, 0, sizeof ex); emit_expr(c, sargv[a], &ex);
            emit_unbox_nilable_text(c, ivt, ex.p ? ex.p : "", b); free(ex.p);
          }
          else emit_expr(c, sargv[a], b);
        }
        buf_puts(b, ", ");
      }
      buf_printf(b, "%s)", default_value_from_compiler(c, comp_ntype(c, id)));
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
      if ((fwd_super && (s->nparams == 0 || emit_zsuper_gather(c, s, NULL) >= 0)) || (!fwd_super && sup_argc == 0)) {
        buf_puts(b, default_value_from_compiler(c, comp_ntype(c, id)));
        return;
      }
    }
    /* `super` from an exception subclass's own #message / #to_s reaches
       Exception's: the message the exception was raised with. The subclass's
       struct starts with the builtin exception's, so the stored message reads
       straight off self (`def message; [super, detail].join; end`). */
    /* `super` from a reopening of a builtin exception class (`class
       RuntimeError; def tag = "RT:" + super`), or from a user exception
       subclass whose parents leave the name to one: the reopening above it
       that defines the name, picked at run time from the builtin ancestry
       -- the class's own parent for a reopening, the builtin it descends
       from (inclusive) for a subclass. */
    if (!s->is_cmethod && uname && any_exc_reopen(c) &&
        (class_is_exc_reopen(c, s->class_id) || class_is_exc_subclass(c, s->class_id))) {
      int xr0[8], xr[8], xn = 0;
      int xn0 = exc_reopen_definers(c, uname, xr0, 8);
      for (int q = 0; q < xn0; q++) if (xr0[q] != s->class_id) xr[xn++] = xr0[q];
      if (xn > 0) {
        char from[160];
        if (class_is_exc_reopen(c, s->class_id))
          snprintf(from, sizeof from, "sp_exc_parent_of_name(\"%s\")", c->classes[s->class_id].name);
        else
          snprintf(from, sizeof from, "\"%s\"", exc_builtin_parent(c, s->class_id));
        const char *scn = class_ruby_name(c, s->class_id);
        if (!scn) scn = c->classes[s->class_id].name;
        buf_puts(b, "({ ");
        int pk = emit_exc_reopen_pick_head(c, xr, xn, from, b);
        /* none above it: #message / #to_s are Exception's own, the stored
           message; any other name has no superclass method */
        if ((sp_streq(uname, "message") || sp_streq(uname, "to_s")) && comp_ntype(c, id) == TY_STRING)
          buf_printf(b, "_xi%d < 0 ? sp_exc_message((struct sp_Exception_s *)%s) : ", pk, g_self);
        else
          buf_printf(b, "if (_xi%d < 0) sp_raise_cls(\"NoMethodError\", \"super: no superclass method '%s' for an instance of %s\"); ",
                     pk, uname, scn);
        for (int q = 0; q < xn; q++) {
          int xm = comp_method_in_chain(c, xr[q], uname, NULL);
          if (q != xn - 1) buf_printf(b, "_xi%d == %d ? ", pk, q);
          buf_printf(b, "sp_%s_%s((sp_Exception *)%s", mc_reopen_cls(c, xr[q], uname), mc(uname), g_self);
          if (ty && sp_streq(ty, "ForwardingSuperNode")) emit_zsuper_args(c, s, &c->scopes[xm], ", ", b);
          else emit_args_filled(c, xm, nt_ref(c->nt, id, "arguments"), ", ", b);
          buf_puts(b, ")");
          if (q != xn - 1) buf_puts(b, " : ");
        }
        buf_puts(b, "; })");
        return;
      }
    }
    if ((class_is_exc_subclass(c, s->class_id) || class_is_exc_reopen(c, s->class_id)) && !s->is_cmethod &&
        (sp_streq(uname, "message") || sp_streq(uname, "to_s"))) {
      TyKind rt = comp_ntype(c, id);
      Buf mb; memset(&mb, 0, sizeof mb);
      buf_printf(&mb, "sp_exc_message((struct sp_Exception_s *)%s)", g_self);
      if (rt == TY_POLY) { buf_puts(b, "sp_box_str("); buf_puts(b, mb.p); buf_puts(b, ")"); }
      else buf_puts(b, mb.p);
      free(mb.p);
      return;
    }
    /* `super` in a respond_to? override no ancestor defines is Object's:
       the object's method-table answer for a runtime name */
    if (uname && sp_streq(uname, "respond_to?") && emit_super_respond_to(c, id, s, b)) return;
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
  if (sp_streq(c->classes[defcls].name, "Object") || sp_streq(c->classes[defcls].name, "Array") ||
      sp_streq(c->classes[defcls].name, "Hash") || sp_streq(c->classes[defcls].name, "Numeric")) {
    /* Object's methods take self boxed: any value can be the receiver. So
       do an Array, Hash or Numeric reopening's (emit_method_signature),
       which a program class deriving from it reaches by super --
       activesupport's HashWithIndifferentAccess#reverse_merge -- and which
       have no struct to cast self to. */
    buf_printf(b, "sp_%s_%s(", c->classes[defcls].c_name, mc(uname));
    emit_boxed_text(c, ty_object(s->class_id), g_self, b);
  }
  /* a user exception subclass's super reaching its builtin parent's
     reopening: that method takes the runtime's sp_Exception */
  else if (class_is_exc_reopen(c, defcls))
    buf_printf(b, "sp_%s_%s((sp_Exception *)%s", mc_reopen_cls(c, defcls, uname), mc(uname), g_self);
  else
    buf_printf(b, "sp_%s_%s((sp_%s *)%s", c->classes[defcls].c_name, mc(uname), c->classes[defcls].c_name, g_self);
  if (ty && sp_streq(ty, "ForwardingSuperNode")) {
    /* The parent may declare more than this method does -- an optional,
       `*rest`, a keyword, `**` -- which a bare super leaves to their defaults
       and empties, as CRuby does (#4852). */
    emit_zsuper_args(c, s, &c->scopes[mi], ", ", b);
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
    TyKind pt = scope_param_type(m, 0);
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
  TyKind pt = scope_param_type(m, pi);
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

static int g_has_user_init_copy = 0;
static int user_init_copy_scope(Compiler *c, int k, int *defcls) {
  int mi = c->classes[k].instantiated ? comp_method_in_chain(c, k, "initialize_copy", defcls) : -1;
  if (mi < 0) return -1;
  Scope *m = &c->scopes[mi];
  LocalVar *p = m->nparams == 1 && m->rest_idx < 0 ? scope_local(m, m->pnames[0]) : NULL;
  if (!p || !(ty_is_object(p->type) || p->type == TY_POLY) || !m->reachable || m->yields ||
      scope_is_shadowed(c, mi) || m->is_transplanted_source || c->classes[*defcls].is_value_type) return -1;
  return mi;
}

static void emit_user_init_copy_dispatch(Compiler *c, Buf *b) {
  buf_puts(b, "static void sp_user_init_copy_dispatch(sp_RbVal copy, sp_RbVal orig) {\n  switch (copy.cls_id) {\n");
  for (int k = 0; k < c->nclasses; k++) {
    int defcls = -1, mi = user_init_copy_scope(c, k, &defcls);
    if (mi < 0) continue;
    TyKind pt = scope_local(&c->scopes[mi], c->scopes[mi].pnames[0])->type;
    const char *dcn = c->classes[defcls].c_name;
    buf_printf(b, "    case %d: ", k);
    emit_method_cname(c, &c->scopes[mi], b);
    buf_printf(b, "((sp_%s *)copy.v.p, ", dcn);
    if (pt == TY_POLY) buf_puts(b, "orig");
    else buf_printf(b, "(sp_%s *)orig.v.p", c->classes[ty_object_class(pt)].c_name);
    buf_puts(b, c->scopes[mi].blk_param && c->scopes[mi].blk_param[0] ? ", NULL); break;\n" : "); break;\n");
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
    }
    else {
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
    TyKind pt = scope_param_type(m, 0);
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
               comp_class_index(c, ci->name), ci->c_name, ci->c_name, ci->nmembers + 1);
    for (int i = 0; i < ci->nmembers; i++) {
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
    if (ci->nmembers == 0) buf_puts(b, "1");
    for (int i = 0; i < ci->nmembers; i++) {
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
  /* A user exception subclass keeps Exception#== (same class and message)
     unless it defines its own. */
  for (int k = 0; k < c->nclasses; k++) {
    ClassInfo *ci = &c->classes[k];
    if (!ci->instantiated || !class_is_exc_subclass(c, k)) continue;
    if (comp_method_in_chain(c, k, "==", NULL) >= 0) continue;
    buf_printf(b, "    case %d: return sp_exc_eq((sp_Exception *)a.v.p, (sp_Exception *)b.v.p);\n",
               comp_class_index(c, ci->name));
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
  if (g_has_user_init_copy)
    buf_puts(b, "static void sp_user_init_copy_dispatch(sp_RbVal copy, sp_RbVal orig);\n");
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
    /* the scan loop above skips these (the Toplevel pseudo-class, a builtin
       reopen): no sp_<C>__gc_scan exists to tag */
    if (is_builtin_reopen(ci->name)) continue;
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
  if (g_has_user_init_copy)
    buf_puts(b, "  sp_user_init_copy_hook = sp_user_init_copy_dispatch;\n");
  if (g_gen_obj_hashkey)
    buf_puts(b, "  sp_obj_hash_hook = sp_gen_obj_hash;\n  sp_obj_eql_hook = sp_gen_obj_eql;\n");
  if (g_gen_obj_valeq)
    buf_puts(b, "  sp_obj_eq_hook = sp_obj_eq_dispatch;\n");
  if (exc_has_user_msg_override(c))
    buf_puts(b, "  sp_user_exc_to_s_fn = sp_user_exc_to_s;\n");
  if (g_needs_class_machinery)
    buf_puts(b, "  sp_user_exc_parent_fn = sp_user_exc_parent;\n"
                "  sp_user_exc_modules_fn = sp_user_exc_modules;\n"
                "  sp_poly_is_a_hook = sp_poly_is_a;\n"
                "  sp_class_le_id_fn = sp_class_le_ids;\n"
                "  sp_class_cmp_fn = sp_class_cmp_rv;\n"
                "  sp_class_kind_of_name_fn = sp_class_kind_of_name;\n"
                "  sp_class_is_module_fn = sp_class_is_module_val;\n");
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
    if (c->uses_kconv || c->uses_kw_to_hash) buf_puts(b, "  sp_obj_conv_fn = sp_obj_conv_sw;\n");
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
    if (ty_degraded(pt)) degraded = 1;
    j++;
  }
  buf_puts(b, ") -> ");
  if (s->ret == TY_UNKNOWN || s->ret == TY_VOID) {
    buf_puts(b, "void");
  }
  else {
    ty_to_rbs_into(c, s->ret, b);
    if (ty_degraded(s->ret)) degraded = 1;
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
  if (ty_degraded(s->ret)) return 1;
  for (int i = 0; i < s->nparams; i++) {
    LocalVar *p = scope_local(s, s->pnames[i]);
    TyKind pt = (p && p->type != TY_UNKNOWN) ? p->type : TY_POLY;
    if (ty_degraded(pt)) return 1;
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
        if (!ty_degraded(pt) || !s->pnames[i]) continue;
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
      else if (!ty_degraded(s->ret)) continue;
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
int write_text_file(const char *path, const char *text) {
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
    /* an exception path is read whole: the leaf of Math::DomainError,
       "DomainError", has no id, and the class sat under Object */
    const char *exc_nm = superclass_builtin_exc_name(c->nt, sc_node);
    if (exc_nm) sc_nm = exc_nm;
    if (sc_nm) { int bid = builtin_class_id(sc_nm); if (bid != 0) builtin_par = bid; }
  }
  return builtin_par;
}

/* The name of the builtin exception directly above user class `i` when that
   exception has no builtin cls_id (LoadError, SystemCallError, Errno::*, ...),
   else NULL. class_builtin_superclass has no id to answer for those and says
   Object; the runtime knows them by name (sp_exc_parent_of_name carries their
   chain), so the generated superclass table names them instead, and every
   walk that goes through it -- #superclass, #ancestors, is_a?, Module#<,
   Module#=== -- continues up the real exception chain. */
const char *class_builtin_superclass_name(Compiler *c, int i) {
  if (c->classes[i].is_struct) return NULL;
  int sc_node = nt_ref(c->nt, c->classes[i].def_node, "superclass");
  if (sc_node < 0) return NULL;
  const char *sc_nm = superclass_builtin_exc_name(c->nt, sc_node);
  if (!sc_nm || builtin_class_id(sc_nm) != 0) return NULL;
  return errno_canonical_name(sc_nm);
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
   the set is deliberately broad. The one constant it lets by is the class a
   receiverless `raise TypeError, msg` names: that raise goes by the class's
   name (sp_raise_cls), and the builtins raise so. Its message is scanned as
   any other expression. */
static int program_needs_class_machinery(Compiler *c) {
  if (c->nclasses > 0) return 1;
  const NodeTable *nt = c->nt;
  unsigned char *raised = calloc((size_t)nt->count + 1, 1);
  for (int i = 0; raised && i < nt->count; i++) {
    if (nt_kind(nt, i) != NK_CallNode || nt_ref(nt, i, "receiver") >= 0) continue;
    const char *nm = nt_str(nt, i, "name");
    int ac = 0;
    const int *av = nm && sp_streq(nm, "raise") ? call_args(nt, i, &ac) : NULL;
    if (av && (ac == 1 || ac == 2) && av[0] >= 0 && av[0] < nt->count &&
        nt_kind(nt, av[0]) == NK_ConstantReadNode)
      raised[av[0]] = 1;
  }
  int need = 0;
  for (int i = 0; i < nt->count && !need; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && (sp_streq(nm, "class") || sp_streq(nm, "is_a?") ||
                 sp_streq(nm, "kind_of?") || sp_streq(nm, "instance_of?") ||
                 sp_streq(nm, "ancestors") || sp_streq(nm, "superclass") ||
                 sp_streq(nm, "===")))
        need = 1;
    }
    else if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
      const char *nm = nt_str(nt, i, "name");
      if (nm && is_builtin_class_name(nm) && !(raised && raised[i])) need = 1;
    }
  }
  free(raised);
  return need;
}

/* Whole-program scan for the prologue features (see codegen_internal.h). Each
   flag over-approximates (a user method named `rand` keeps srand; that is
   harmless), so a feature that is genuinely used is never missed: a symbol /
   regex / random value can only originate from one of the nodes below. */
static void scan_prologue_features(Compiler *c) {
  const NodeTable *nt = c->nt;
  g_uses_symbols = (c->nsymbols > 0);
  g_uses_marshal = 0;
  g_uses_regex = 0; g_uses_argv = 0; g_uses_threads = 0; g_uses_finalizers = 0;
  g_uses_program_name = 0;
  g_reads_match_regs = 0;
  for (int i = 0; i < nt->count; i++) {
    const char *ty = nt_type(nt, i);
    if (!ty) continue;
    if (sp_streq(ty, "BackReferenceReadNode") || sp_streq(ty, "NumberedReferenceReadNode"))
      g_reads_match_regs = 1;
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
         ill-typed far from the cause, so say what is missing instead.
         Only a TOP-LEVEL name is the library's: a path segment under a
         parent (`ActiveSupport::JSON::Encoding`) names that module's own
         constant, and telling it to `require "json"` refused every
         activesupport file that reaches its JSON encoder. */
      if (sp_streq(ty, "ConstantPathNode") && nt_ref(nt, i, "parent") >= 0) { /* nested */ }
      else {
        static const struct { const char *cls, *feat; } PKG[] = {
          {"StringIO","stringio"}, {"CSV","csv"}, {"JSON","json"}, {"Set","set"},
          {"StringScanner","strscan"}, {"Base64","base64"}, {"Digest","digest"},
          {"ERB","erb"}, {"OptionParser","optparse"}, {"Pathname","pathname"},
          {"SecureRandom","securerandom"}, {"Tempfile","tempfile"},
          {"CGI","cgi"}, {"Benchmark","benchmark"},
          {NULL,NULL} };
        for (int pk = 0; PKG[pk].cls; pk++) {
          if (!sp_streq(nm, PKG[pk].cls)) continue;
          if (comp_class_index(c, nm) >= 0) break;      /* the program defines it */
          /* ... or defines it under a path: a nested `module Digest` the
             collision qualifier registered as `OpenSSL::Digest` (beside
             activesupport's ActiveSupport::Digest) is what a bare `Digest`
             inside that namespace names by Ruby's lexical lookup -- not the
             bundled library */
          { int nested = 0; size_t nl = strlen(nm);
            for (int q = 0; q < c->nclasses && !nested; q++) {
              const char *cn = c->classes[q].name; size_t cl = cn ? strlen(cn) : 0;
              /* the qualifier joins the path with `__` (Wrap__Digest) */
              if (cl > nl + 2 && sp_streq(cn + cl - nl, nm) && cn[cl - nl - 1] == '_' && cn[cl - nl - 2] == '_') nested = 1;
            }
            if (nested) break; }
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
      /* builtins/object_space.rb, spliced for a program that defines finalizers */
      else if (sp_streq(nm, "Finalizers__")) g_uses_finalizers = 1;
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
      else if (nm && (sp_streq(nm, "$~") || sp_streq(nm, "$&") || sp_streq(nm, "$`") ||
                      sp_streq(nm, "$'") || sp_streq(nm, "$+"))) g_reads_match_regs = 1;
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
      if (sp_streq(nm, "last_match")) g_reads_match_regs = 1;
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
           sp_streq(nm, "values") || sp_streq(nm, "values_at") ||
           sp_streq(nm, "entries"))) reached = 1;
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
  /* A class-side name called on a boxed receiver: the sp_cls_* answers are
     generated for that program only, so every other program's C is as it was. */
  g_gen_cls_answers = 0;
  for (int nid = 0; nid < c->nt->count && !g_gen_cls_answers; nid++) {
    const char *nty = nt_type(c->nt, nid);
    if (!nty || !sp_streq(nty, "CallNode")) continue;
    int nrv = nt_ref(c->nt, nid, "receiver");
    if (nrv < 0 || (comp_ntype(c, nrv) != TY_POLY && comp_ntype(c, nrv) != TY_UNKNOWN)) continue;
    const char *nnm = nt_str(c->nt, nid, "name");
    if (nnm && (sp_streq(nnm, "subclasses") || sp_streq(nnm, "allocate") ||
                sp_streq(nnm, "members") || sp_streq(nnm, "keyword_init?")))
      g_gen_cls_answers = 1;
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
typedef struct EmitUnitState {
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
  int loop_exc_base, loop_ensure_base, redo_depth;
  TyKind ie_next_ty;
  int move_depth;   /* instance_exec scope moves (comp_scope_move_unwind) */
  int view_depth;   /* codegen views (view_unwind) */
} EmitUnitState;

void emit_unit_state_save(EmitUnitState *s) {
  s->move_depth = comp_scope_move_depth();
  s->view_depth = view_mark();
  s->ret_type = g_ret_type; s->fn_ret_type = g_fn_ret_type; s->result_ty = g_result_ty;
  s->c_ret_void = g_c_ret_void; s->in_proc_body = g_in_proc_body; s->result_poly = g_result_poly;
  s->proc_body_kind = g_proc_body_kind; s->proc_toplevel_return = g_proc_toplevel_return;
  s->indent = g_indent; s->nren = g_nren; s->block_nren = g_block_nren; s->block_id = g_block_id;
  s->c_loop_depth = g_c_loop_depth; s->ensure_depth = g_ensure_depth;
  s->emitting_class_id = g_emitting_class_id; s->inline_recv_class = g_inline_recv_class;
  s->ie_class_id = g_ie_class_id; s->dm_subst_node = g_dm_subst_node; s->exc_frame_depth = g_exc_frame_depth;
  s->loop_exc_base = g_loop_exc_base; s->loop_ensure_base = g_loop_ensure_base; s->redo_depth = g_redo_depth;
  s->ie_next_ty = g_ie_next_ty;
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

void emit_unit_state_restore(const EmitUnitState *s) {
  comp_scope_move_unwind(s->move_depth);
  view_unwind(s->view_depth);
  g_ret_type = s->ret_type; g_fn_ret_type = s->fn_ret_type; g_result_ty = s->result_ty;
  g_c_ret_void = s->c_ret_void; g_in_proc_body = s->in_proc_body; g_result_poly = s->result_poly;
  g_proc_body_kind = s->proc_body_kind; g_proc_toplevel_return = s->proc_toplevel_return;
  g_indent = s->indent; g_nren = s->nren; g_block_nren = s->block_nren; g_block_id = s->block_id;
  g_c_loop_depth = s->c_loop_depth; g_ensure_depth = s->ensure_depth;
  g_emitting_class_id = s->emitting_class_id; g_inline_recv_class = s->inline_recv_class;
  g_ie_class_id = s->ie_class_id; g_dm_subst_node = s->dm_subst_node; g_exc_frame_depth = s->exc_frame_depth;
  g_loop_exc_base = s->loop_exc_base; g_loop_ensure_base = s->loop_ensure_base; g_redo_depth = s->redo_depth;
  g_ie_next_ty = s->ie_next_ty;
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
/* A probe's snapshot of the emitter state, for a probe that may be
   abandoned by a longjmp partway through a loop or a nested body. */
EmitUnitState *emit_state_snapshot(void) {
  EmitUnitState *s = malloc(sizeof *s);
  emit_unit_state_save(s);
  return s;
}
void emit_state_release(EmitUnitState *s, int rollback) {
  if (rollback) emit_unit_state_restore(s);
  free(s);
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

/* --defer-refusals: the statements `raise NotImplementedError, msg`, in
   scope `scope`, positioned as `like` -- the body a refused method is
   emitted with instead, so its callers still have it to call. */
static int deferred_raise_body(Compiler *c, int like, int scope, const char *msg) {
  NodeTable *nt = (NodeTable *)c->nt;
  int st = nt_new_node(nt, "StatementsNode"), call = nt_new_node(nt, "CallNode");
  int args = nt_new_node(nt, "ArgumentsNode"), cls = nt_new_node(nt, "ConstantReadNode");
  int str = nt_new_node(nt, "StringNode");
  if (st < 0 || call < 0 || args < 0 || cls < 0 || str < 0) return -1;
  comp_grow_node_arrays(c);
  int ids[5] = { st, call, args, cls, str };
  for (int i = 0; i < 5; i++) {
    c->nscope[ids[i]] = scope;
    nt_node_set_int(nt, ids[i], "node_line", nt_int(nt, like, "node_line", 0));
    nt_node_set_int(nt, ids[i], "node_file", nt_int(nt, like, "node_file", 0));
  }
  nt_node_set_str(nt, call, "name", "raise");
  nt_node_set_ref(nt, call, "receiver", -1);
  nt_node_set_ref(nt, call, "block", -1);
  nt_node_set_str(nt, cls, "name", "NotImplementedError");
  nt_node_set_str(nt, str, "content", msg);
  nt_node_set_str(nt, str, "unescaped", msg);
  int av[2] = { cls, str };
  nt_node_set_arr(nt, args, "arguments", av, 2);
  nt_node_set_ref(nt, call, "arguments", args);
  nt_node_set_arr(nt, st, "body", &call, 1);
  c->ntype[cls] = TY_CLASS; c->ntype[str] = TY_STRING; c->ntype[call] = TY_VOID; c->ntype[st] = TY_VOID;
  return st;
}

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
  NT_FOREACH_KIND(c->nt, NK_CallNode, id) refuse_from_plan(c, id, CRF_BINDING, "refuse-binding");
}

/* `const_get(name)` with a runtime name that the lowering could not reach:
   one with no receiver the desugar could supply (an instance method, where
   self has no const_get) or one the fixpoint never lowered. Left alone it
   typed nothing and the first call on its value raised NoMethodError for
   "unknown" (#4843); refuse it where it is written instead. The explicit
   and class-method forms lower to a static dispatch (desugar_dynamic_const_get). */
static void reject_runtime_const_get(Compiler *c) {
  NT_FOREACH_KIND(c->nt, NK_CallNode, id) refuse_from_plan(c, id, CRF_CONST_GET, "refuse-const-get");
}

static void reject_runtime_send(Compiler *c) {
  NT_FOREACH_KIND(c->nt, NK_CallNode, id) refuse_from_plan(c, id, CRF_SEND, "refuse-send");
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
    /* The argument conversions, run under rb_protect. Each converted argument
       is rooted for the whole call: the root a spx_in_*_array helper pushes
       pops as the helper returns, and the next argument's conversion
       allocates. A conversion that raises longjmps past C cleanup
       attributes, so these roots are pushed by hand and the wrapper puts the
       root count back itself before any raise leaves it -- a root left behind
       would point into a dead frame for the next collection to read. */
    buf_printf(&sb, "typedef struct { spx_c_%d *c;", s9);
    for (int p9 = 0; p9 < sc->nparams; p9++) buf_printf(&sb, " VALUE v%d;", p9);
    buf_printf(&sb, " } spx_a_%d;\n", s9);
    buf_printf(&sb, "static VALUE spx_conv_%d(VALUE p) { spx_a_%d *a = (spx_a_%d *)p;\n",
               s9, s9, s9);
    for (int p9 = 0; p9 < sc->nparams; p9++) {
      LocalVar *lv = scope_local(sc, sc->pnames[p9]);
      buf_printf(&sb, "  a->c->a%d = %s(a->v%d);", p9, ext_rb_in(lv->type), p9);
      if (lv->type == TY_STRING || lv->type == TY_INT_ARRAY ||
          lv->type == TY_FLOAT_ARRAY || lv->type == TY_STR_ARRAY)
        buf_printf(&sb, " _sp_gc_root_push((void**)((uintptr_t)&a->c->a%d | "
                        "_SP_GC_SLOT_TAG(a->c->a%d)));", p9, p9);
      buf_puts(&sb, "\n");
    }
    buf_puts(&sb, "  return Qnil;\n}\n");
    /* The kernel call, also under rb_protect: CRuby checks interrupts as
       rb_thread_call_without_gvl takes the GVL back, so a Thread#raise, a
       kill or a signal can raise out of it -- past the unlock and the root
       restore below, if nothing catches it here. */
    buf_printf(&sb, "static VALUE spx_call_%d(VALUE p) { return (VALUE)(intptr_t)"
                    "rb_thread_call_without_gvl(spx_run_%d, (void *)p, RUBY_UBF_IO, NULL); }\n",
               s9, s9);
    buf_printf(&sb, "static VALUE spx_m_%d(VALUE self", s9);
    for (int p9 = 0; p9 < sc->nparams; p9++) buf_printf(&sb, ", VALUE v%d", p9);
    buf_printf(&sb, ") {\n  spx_c_%d c__; memset(&c__, 0, sizeof c__);\n", s9);
    buf_puts(&sb, "  SP_GC_SAVE();\n");
    buf_printf(&sb, "  { spx_a_%d a__ = { &c__", s9);
    for (int p9 = 0; p9 < sc->nparams; p9++) buf_printf(&sb, ", v%d", p9);
    buf_printf(&sb, " }; int state = 0;\n"
                    "    rb_protect(spx_conv_%d, (VALUE)&a__, &state);\n"
                    "    if (state) { sp_gc_nroots = _gc_saved; rb_jump_tag(state); } }\n", s9);
    buf_puts(&sb, "  { int raised, state = 0; const char *ec = 0, *em = 0;\n"
                  "    pthread_mutex_lock(&spx_lock);\n");
    buf_printf(&sb, "    raised = (int)(intptr_t)rb_protect(spx_call_%d, (VALUE)&c__, &state);\n", s9);
    buf_puts(&sb, "    if (state) raised = 0;\n"
                  "    if (raised) { ec = spx_exc_cls; em = spx_exc_msg; }\n"
                  "    pthread_mutex_unlock(&spx_lock);\n"
                  "    if (state) { sp_gc_nroots = _gc_saved; rb_jump_tag(state); }\n"
                  "    if (raised) { sp_gc_nroots = _gc_saved; spx_reraise(ec, em); }\n  }\n");
    /* The arguments are done with; the return conversion allocates only on
       the Ruby heap, and can raise (NoMemoryError) past the cleanup. */
    buf_puts(&sb, "  sp_gc_nroots = _gc_saved;\n");
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


static int cmp_int_pair(const void *a, const void *b) {
  const int *x = a, *y = b;
  if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
  return x[1] < y[1] ? -1 : x[1] > y[1];
}

static int exc_text_method(Compiler *c, int i, int want_message, int *dcls, const char **fn) {
  int dmsg = -1, dtos = -1;
  int mi_msg = comp_method_in_chain(c, i, "message", &dmsg);
  int mi_tos = comp_method_in_chain(c, i, "to_s", &dtos);
  if (want_message && mi_msg >= 0) { *dcls = dmsg; *fn = "message"; return mi_msg; }
  if (mi_tos >= 0) { *dcls = dtos; *fn = "to_s"; return mi_tos; }
  return -1;
}

/* Does the text call sp_poly_is_a? Its definition and its prototype are
   not calls. */
static int calls_poly_is_a(const char *t) {
  for (const char *q = t ? strstr(t, "sp_poly_is_a(") : NULL; q; q = strstr(q + 1, "sp_poly_is_a(")) {
    if (q > t && (q[-1] == '_' || isalnum((unsigned char)q[-1]))) continue;
    if (q - t >= 4 && !strncmp(q - 4, "int ", 4)) continue;
    return 1;
  }
  return 0;
}

/* Insert `s` into `b` at offset `at`. */
static void buf_splice(Buf *b, size_t at, const char *s) {
  char *tail = strdup(b->p + at);
  b->len = at;
  b->p[at] = 0;
  buf_puts(b, s);
  buf_puts(b, tail);
  free(tail);
}

/* SystemCallError#initialize reads the errno through the class's own `Errno`
   constant, so a class of the program below SystemCallError that defines
   one picks the number its instances carry. The runtime knows the Errno
   classes' numbers by name and not a constant of the program's, so such a
   class would get the number of the Errno class above it -- or, directly
   under SystemCallError, CRuby's TypeError where CRuby answers the
   constant's number. Refused rather than answered differently. */
static void refuse_syserr_errno_const(Compiler *c) {
  /* any write in the class's body, however nested (`if ..; Errno = 13;
     end`) and in any reopening of it: node_cbody names the class body */
  static const NodeKind wk[] = { NK_ConstantWriteNode, NK_ConstantOrWriteNode,
                                 NK_ConstantOperatorWriteNode, NK_ConstantAndWriteNode };
  for (size_t w = 0; w < sizeof wk / sizeof wk[0]; w++) {
    int n = 0;
    const int *st = nt_nodes_of_kind(c->nt, wk[w], &n);
    for (int i = 0; i < n; i++) {
      int ci = c->node_cbody[st[i]];
      /* the name may come qualified (`MyErr__Errno`) when two classes
         define one */
      const char *wn = nt_str(c->nt, st[i], "name");
      size_t wl = wn ? strlen(wn) : 0;
      if (ci >= 0 && class_is_syserr(c, ci) && wn &&
          (sp_streq(wn, "Errno") || (wl > 7 && sp_streq(wn + wl - 7, "__Errno"))))
        unsupported_feature(c, st[i], "an Errno constant defined in a subclass of SystemCallError "
                                      "(its errno is read from the Errno class above it)");
    }
  }
  int kn = 0;
  const int *ks = nt_nodes_of_kind(c->nt, NK_ConstantPathWriteNode, &kn);
  for (int i = 0; i < kn; i++) {
    int tg = nt_ref(c->nt, ks[i], "target");
    int par = tg >= 0 ? nt_ref(c->nt, tg, "parent") : -1;
    const char *pn = par >= 0 ? nt_str(c->nt, par, "name") : NULL;
    if (tg >= 0 && sp_streq(nt_str(c->nt, tg, "name"), "Errno") && pn &&
        class_is_syserr(c, comp_class_index(c, pn)))
      unsupported_feature(c, ks[i], "an Errno constant defined in a subclass of SystemCallError "
                                    "(its errno is read from the Errno class above it)");
  }
}

char *codegen_program(const NodeTable *nt) {
  char *isa_ext = NULL;  /* sp_poly_is_a's class-value arms, and where they go */
  size_t isa_ext_at = 0;
  Compiler *c = comp_new(nt);
  analyze_program(c);
  if (g_dump_traits) { ty_traits_dump(c); exit(0); }
  if (g_check_traits) exit(ty_traits_check(c) ? 1 : 0);
  g_scopes_settled = 1;   /* scope_is_shadowed may answer from its table now */
  /* From here on a yield reads the type of the block spliced at THIS site,
     not the union the node cache holds across sites (#3784). Installed after
     analysis so the fixpoint keeps seeing the cache unchanged. */
  sp_yield_site_type_hook = sp_yield_site_type;

  /* `#line` directives are emitted only when the parser stamped per-node
     source lines (SPINEL_LINE_MAP / SPINEL_DEBUG); the same env gates both
     sides so codegen and the AST agree. */
  g_line_map = (getenv("SPINEL_LINE_MAP") || getenv("SPINEL_DEBUG")) ? 1 : 0;
  g_debug = getenv("SPINEL_DEBUG") ? 1 : 0;
  g_check_stores = getenv("SPINEL_CHECK_STORES") ? 1 : 0;
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
  refuse_syserr_errno_const(c);

  /* Analyze-only emit modes (legacy --emit-*): write the requested artifact
     from the analysis result and skip codegen. Returns an empty translation
     unit so the driver writes no binary. */
  /* --profile: write the symbol map next to the binary, then continue to
     the real build (offline symbolization input for perf tooling, #1336). */
  const char *psym_out = getenv("SPINEL_PROFILE_SYMBOL_MAP");
  if (psym_out && *psym_out) {
    char *json = build_symbol_map_json(c);
    write_text_file(psym_out, json);
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
    write_text_file(sym_out, json);
    free(json);
    comp_free(c);
    return strdup("");
  }
  const char *rbs_out = getenv("SPINEL_EMIT_RBS");
  if (rbs_out && *rbs_out) {
    char *rbs = build_rbs_text(c);
    write_text_file(rbs_out, rbs);
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
  reject_runtime_const_get(c);
  reject_binding(c);

  Buf b; memset(&b, 0, sizeof b);
  memset(&g_procs, 0, sizeof g_procs);
  memset(&g_proc_protos, 0, sizeof g_proc_protos);
  if (g_pool_fwd) memset(g_pool_fwd, 0, (size_t)g_pool_fwd_n);
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
      if (m->rest) buf_puts(&b, ", sp_int, sp_RbVal *");
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
      buf_printf(&b, "static const unsigned char sp_ffi_user_cls[%d] SP_UNUSED = {", cf->nclasses > 0 ? cf->nclasses : 1);
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
      /* a reopened builtin's entry (`class Object; def m` gives Object one)
         is what its constant boxes to, and it prints the builtin's name;
         the Toplevel pseudo-class alone has no Ruby name */
      if (is_builtin_reopen(c->classes[i].name) && !sp_streq(c->classes[i].name, "Toplevel")) {
        buf_printf(&b, "case %d:return SPL(\"%s\");", i, c->classes[i].name);
        continue;
      }
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
      else if (class_builtin_superclass_name(c, i)) {
        buf_printf(&b, "  case %d: return ((sp_Class){-1, SPL(\"%s\")});\n", i,
                   class_builtin_superclass_name(c, i));
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
    /* sp_class_eq, not the ids: a name-backed class (an id-less builtin
       exception such as LoadError or Errno::ENOENT) carries cls_id -1 with
       its name, so every two of them would compare equal by id */
    buf_puts(&b, "    if(sp_class_eq(cur,anc))return 1;\n");
    buf_puts(&b, "    if(cur.cls_id==-117)break;\n"); /* BasicObject: root */
    buf_puts(&b, "    sp_Class next=cur.cls_id>=0?sp_class_superclass(cur):sp_builtin_superclass(cur);\n");
    buf_puts(&b, "    if(sp_class_eq(next,cur))break;\n");
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
    buf_puts(&b, "    if(sp_class_eq(next,cur)||next.cls_id==-116||next.cls_id==-117)return 0;\n");
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

  buf_puts(&b, "static int sp_class_lt(sp_Class a,sp_Class b){return !sp_class_eq(a,b)&&sp_class_is_ancestor(b,a);}\n");
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
  buf_puts(&b, "#define sp_class_lt3(A,B) ({ sp_Class _cx=(A),_cy=(B); sp_class_eq(_cx,_cy)?sp_box_bool(0):(sp_class_le(_cx,_cy)?sp_box_bool(1):(sp_class_le(_cy,_cx)?sp_box_bool(0):sp_box_nil())); })\n");
  buf_puts(&b, "#define sp_class_le3(A,B) ({ sp_Class _cx=(A),_cy=(B); sp_class_le(_cx,_cy)?sp_box_bool(1):(sp_class_le(_cy,_cx)?sp_box_bool(0):sp_box_nil()); })\n");
  buf_puts(&b, "#define sp_class_gt3(A,B) sp_class_lt3(B,A)\n");
  buf_puts(&b, "#define sp_class_ge3(A,B) sp_class_le3(B,A)\n");
  buf_puts(&b, "#define sp_class_cmp3(A,B) ({ sp_Class _cx=(A),_cy=(B); sp_class_eq(_cx,_cy)?sp_box_int(0):(sp_class_le(_cx,_cy)?sp_box_int(-1):(sp_class_le(_cy,_cx)?sp_box_int(1):sp_box_nil())); })\n");
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
    /* One pass over every class/module body (class_body_list, a Struct.new
       block included), in source order so each class sees its includes in
       the order they run. Scanning the whole table once per class was
       O(classes * N) and dominated codegen on class-heavy programs. */
    {
      int *bcls, *bbody;
      int nbodies = class_body_list(c, &bcls, &bbody);
      int (*bord)[2] = malloc((size_t)(nbodies > 0 ? nbodies : 1) * sizeof *bord);
      for (int bi = 0; bi < nbodies; bi++) { bord[bi][0] = bbody[bi]; bord[bi][1] = bcls[bi]; }
      qsort(bord, (size_t)nbodies, sizeof *bord, cmp_int_pair);
      for (int bx = 0; bx < nbodies; bx++) {
        int ci = bord[bx][1];
        int body2 = bord[bx][0];
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
      free(bord);
      free(bcls);
      free(bbody);
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
    /* A module's own includes are part of every includer's ancestry:
       `module M2; include M1; end; class A; include M2; end` gives A the
       ancestors [A, M2, M1, ...], and A.include?(M1) / A.new.is_a?(M1) are
       true. The tables held direct includes only, so those answered false
       (activesupport's concerns include one another this way). Close each
       list transitively, keeping the include-order convention the emission
       below reverses: a module's closure goes right BEFORE the module. */
    {
      int **closed = calloc((size_t)c->nclasses, sizeof(int *));
      int  *nclosed = calloc((size_t)c->nclasses, sizeof(int));
      for (int ci = 0; ci < c->nclasses; ci++) {
        int *out = NULL, nout = 0;
        /* iterative DFS in include order: push m's closure, then m */
        int *seen = calloc((size_t)c->nclasses, sizeof(int));
        seen[ci] = 1;
        /* frame: (class index, next include position) */
        int fr_ci[128], fr_q[128]; int nfr = 0;
        fr_ci[nfr] = ci; fr_q[nfr] = 0; nfr++;
        while (nfr > 0) {
          int cur = fr_ci[nfr - 1]; int q = fr_q[nfr - 1];
          if (q >= cls_nincs[cur]) {
            nfr--;
            if (nfr > 0) { int m = cur; int dup = 0;
              for (int j = 0; j < nout; j++) if (out[j] == m) dup = 1;
              if (!dup) { out = realloc(out, sizeof(int) * (size_t)(nout + 1)); out[nout++] = m; } }
            continue;
          }
          fr_q[nfr - 1] = q + 1;
          int m = cls_incs[cur][q];
          if (m < 0) {   /* a builtin module: no includes of its own */
            int dup = 0; for (int j = 0; j < nout; j++) if (out[j] == m) dup = 1;
            if (!dup) { out = realloc(out, sizeof(int) * (size_t)(nout + 1)); out[nout++] = m; }
            continue;
          }
          if (m >= c->nclasses || seen[m] || nfr >= 128) continue;
          seen[m] = 1;
          fr_ci[nfr] = m; fr_q[nfr] = 0; nfr++;
        }
        free(seen);
        closed[ci] = out; nclosed[ci] = nout;
      }
      for (int ci = 0; ci < c->nclasses; ci++) { free(cls_incs[ci]); cls_incs[ci] = closed[ci]; cls_nincs[ci] = nclosed[ci]; }
      free(closed); free(nclosed);
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
    /* a name-backed exception class's modules (IO::EAGAINWaitReadable
       includes IO::WaitReadable), as the rescue match reads them */
    buf_puts(&b, "        if(cur.name){const char*const*_m=sp_exc_modules_of_name(cur.name);"
                 "for(int _k=0;_m&&_m[_k];_k++)sp_PolyArray_push(a,sp_box_class_name(_m[_k]));}\n");
    buf_puts(&b, "        sp_Class bn=sp_builtin_superclass(cur);\n");
    /* the root (BasicObject) yields the nil class: that terminates the walk.
       Chain end used to be marked by a self-reference, so keep that check too. */
    buf_puts(&b, "        if(sp_class_nil_p(bn)||sp_class_eq(bn,cur))break;\n");
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
    buf_puts(&b, "    if(sp_class_eq(next,cur))break;\n");
    buf_puts(&b, "    cur=next;\n");
    buf_puts(&b, "  }\n");
    buf_puts(&b, "  return a;\n}\n\n");
    /* Module#included_modules: the ancestors that are modules (#2674). The
       ancestors are id-backed boxes (sp_box_class of a name-less sp_Class), so
       the cls_id rides the int slot. */
    buf_puts(&b, "static sp_PolyArray *sp_class_included_modules(sp_Class c) SP_UNUSED;\n");
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
    buf_puts(&b, "    if(v.tag==7&&sp_class_eq(sp_unbox_class(v),b))return 1;\n");
    buf_puts(&b, "  }\n");
    /* User-class sp_class_ancestors stops before builtin parents.
       If the target is a builtin, fall back to the chain-walking check. */
    buf_puts(&b, "  if(b.cls_id<0)return sp_class_is_ancestor(b,a);\n");
    buf_puts(&b, "  return 0;\n}\n");
    buf_puts(&b, "#undef sp_class_le\n#define sp_class_le sp_class_le_mod\n");
    buf_puts(&b, "#undef sp_class_lt\n#define sp_class_lt(a,b) (!sp_class_eq(a,b)&&sp_class_le_mod(a,b))\n");
    buf_puts(&b, "#undef sp_class_gt\n#define sp_class_gt(a,b) (!sp_class_eq(a,b)&&sp_class_le_mod(b,a))\n");
    buf_puts(&b, "#undef sp_class_ge\n#define sp_class_ge(a,b) sp_class_le_mod(b,a)\n");
    /* sp_poly_get_class: maps a poly value to its sp_Class for dynamic is_a? */
    buf_puts(&b,
      "static sp_Class sp_poly_get_class(sp_RbVal v){\n"
      "  switch(v.tag){\n"
      "  case SP_TAG_INT: return ((sp_Class){-100});\n"
      /* a Bignum is an Integer too, to grep, all? and === */
      "  case SP_TAG_BIGINT: return ((sp_Class){-100});\n"
      "  case SP_TAG_STR: return ((sp_Class){-102});\n"
      "  case SP_TAG_FLT: return ((sp_Class){-101});\n"
      "  case SP_TAG_BOOL: return v.v.b?((sp_Class){-111}):((sp_Class){-112});\n"
      "  case SP_TAG_NIL: return ((sp_Class){-110});\n"
      "  case SP_TAG_SYM: return ((sp_Class){-103});\n"
      "  case SP_TAG_CLASS: return sp_class_is_module_val(sp_unbox_class(v))?((sp_Class){-108}):((sp_Class){-109});\n"
      "  case SP_TAG_OBJ: if(v.cls_id>=0)return ((sp_Class){v.cls_id});\n"
      /* a String builder (a shared-mutable String handle) is a String; a
         box with no handle is not one */
      "    if(v.cls_id==SP_BUILTIN_STRBUF&&v.v.p)return ((sp_Class){-102});\n"
      "    if(sp_poly_is_array_kind(v.cls_id))return ((sp_Class){-104});\n"
      "    if(v.cls_id==SP_BUILTIN_RANGE||v.cls_id==SP_BUILTIN_STR_RANGE)return ((sp_Class){-106});\n"
      "    if(v.cls_id==SP_BUILTIN_TIME)return ((sp_Class){-107});\n"
      "    if(v.cls_id==SP_BUILTIN_PROC)return ((sp_Class){-118});\n"
      "    if(v.cls_id==SP_BUILTIN_ENUMERATOR)return ((sp_Class){-144});\n"
      "    if(v.cls_id>=-12)return ((sp_Class){-116});\n"
      "    if(v.cls_id>=-20||v.cls_id==-34)return ((sp_Class){-105});\n"  /* hashes */
      /* a BasicObject.new is a BasicObject, not an Object or a Kernel */
      "    if(v.cls_id==SP_BUILTIN_BASIC_OBJECT)return ((sp_Class){-117});\n"
      "    return ((sp_Class){-116});\n"
      "  default: return ((sp_Class){-116});\n"
      "  }\n}\n"
      /* a boxed exception walks its name chain (Errno::ENOENT -> SystemCallError
         -> StandardError), and a name-backed class (SystemCallError, Errno::*,
         OpenStruct) matches by name -- both are invisible to the cls_id walk */
      "static int sp_poly_is_a(sp_RbVal obj,sp_Class klass){\n"
      "  if (obj.tag == SP_TAG_OBJ && obj.cls_id == SP_BUILTIN_EXCEPTION)\n"
      "    return sp_poly_kind_of_builtin(obj, sp_class_to_s(klass));\n"
      "  if (klass.name) return sp_poly_is_a_dyn(obj, sp_box_class(klass), 0);\n");
    /* a class value is also an instance of the modules it (or a superclass)
       extends, its singleton's ancestors: one arm per such class. The arms
       are spliced in here at the end, and only when the program calls
       sp_poly_is_a: most programs that extend a module never do. */
    { int any_ext = 0;
      for (int k = 0; k < c->nclasses && !any_ext; k++) if (comp_class_extends_any(c, k)) any_ext = 1;
      if (any_ext) {
        Buf eb; memset(&eb, 0, sizeof eb);
        buf_puts(&eb, "  if (obj.tag == SP_TAG_CLASS) switch (sp_unbox_class(obj).cls_id) {\n");
        for (int k = 0; k < c->nclasses; k++) {
          if (!comp_class_extends_any(c, k)) continue;
          buf_printf(&eb, "  case %d: if (", k);
          int any = 0;
          for (int m = 0; m < c->nclasses; m++)
            if (comp_class_is_module(c, &c->classes[m]) && comp_class_singleton_has_module(c, k, m)) {
              buf_printf(&eb, "%sklass.cls_id == %d", any ? " || " : "", m);
              any = 1;
            }
          buf_puts(&eb, any ? ") return 1; break;\n" : "0) return 1; break;\n");
        }
        buf_puts(&eb, "  default: break;\n  }\n");
        isa_ext = eb.p;
        isa_ext_at = b.len;
      } }
    buf_puts(&b,
      "  return sp_class_le(sp_poly_get_class(obj),klass);\n}\n");
    /* Module#< / <= / > / >= / <=> where an operand is boxed: the tri-state
       answer of sp_class_lt3 and friends, TypeError for a non-class operand
       (nil for <=>), and the ordinary poly comparison when the receiver turns
       out not to be a class. The runtime reaches the same answer through
       sp_class_cmp_fn. */
    buf_puts(&b,
      "static sp_RbVal sp_class_cmp_rv(sp_RbVal a, sp_RbVal b){return sp_class_cmp3(sp_unbox_class(a),sp_unbox_class(b));}\n"
      "static sp_RbVal sp_class_op_rv(sp_RbVal a, sp_RbVal b, int op) SP_UNUSED;\n"
      "static sp_RbVal sp_class_op_rv(sp_RbVal a, sp_RbVal b, int op){\n"
      "  if(a.tag!=SP_TAG_CLASS){\n"
      "    switch(op){\n"
      "    case 0: return sp_box_bool(sp_poly_lt(a,b));\n"
      "    case 1: return sp_box_bool(sp_poly_le(a,b));\n"
      "    case 2: return sp_box_bool(sp_poly_gt(a,b));\n"
      "    case 3: return sp_box_bool(sp_poly_ge(a,b));\n"
      "    default: { sp_int r=sp_poly_spaceship(a,b); return r==SP_INT_NIL?sp_box_nil():sp_box_int(r); }\n"
      "    }\n"
      "  }\n"
      "  if(b.tag!=SP_TAG_CLASS){\n"
      "    if(op==4)return sp_box_nil();\n"
      "    sp_raise_cls(\"TypeError\",\"compared with non class/module\");\n"
      "  }\n"
      "  sp_Class x=sp_unbox_class(a),y=sp_unbox_class(b);\n"
      "  switch(op){\n"
      "  case 0: return sp_class_lt3(x,y);\n"
      "  case 1: return sp_class_le3(x,y);\n"
      "  case 2: return sp_class_gt3(x,y);\n"
      "  case 3: return sp_class_ge3(x,y);\n"
      "  default: return sp_class_cmp3(x,y);\n"
      "  }\n}\n");
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
          /* a builtin exception by its whole path (Errno::ENOENT) */
          const char *bpar = superclass_builtin_exc_name(c->nt, sc);
          if (bpar) par = bpar;
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
      if (cvar_defined_probed(c, ci->cvars[j]))
        buf_printf(&b, "static int cvar_%s_%s__set = 0;\n", ci->name, ci->cvars[j] + 2);
    }
  }

  /* singleton accessor slots: `class << self; attr_accessor :x; end`
     backed by a file-scope sp_RbVal per (class, name), init = nil. */
  for (int i = 0; i < c->nclasses; i++) {
    ClassInfo *ci = &c->classes[i];
    for (int j = 0; j < ci->nsg_readers; j++)
      buf_printf(&b, "static sp_RbVal sg_%s_%s = {SP_TAG_NIL, 0, {0}};\n",
                 ci->name, ci->sg_readers[j]);
    /* a writer-only accessor (`attr_writer :level`) has a slot of its own
       too: the setter wrote to an undeclared name */
    for (int j = 0; j < ci->nsg_writers; j++)
      if (!comp_is_sg_reader(ci, ci->sg_writers[j]))
        buf_printf(&b, "static sp_RbVal sg_%s_%s = {SP_TAG_NIL, 0, {0}};\n",
                   ci->name, ci->sg_writers[j]);
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
    buf_puts(&b, "static const char *sp_obj_inspect_sw(int cls_id, void *p) SP_COLD SP_NOINLINE;\n");
    buf_puts(&b, "static const char *sp_obj_to_s_sw(int cls_id, void *p) SP_COLD SP_NOINLINE;\n");
    buf_puts(&b, "static sp_int sp_obj_to_int_sw(int cls_id, void *p, int *ok) SP_COLD SP_NOINLINE;\n");
    buf_puts(&b, "static const char *sp_obj_to_str_sw(int cls_id, void *p) SP_COLD SP_NOINLINE;\n");
    buf_puts(&b, "static const char *sp_obj_to_path_sw(int cls_id, void *p) SP_COLD SP_NOINLINE;\n");
    if (c->uses_kconv || c->uses_kw_to_hash)
      buf_puts(&b, "static int sp_obj_conv_sw(int cls_id, void *p, int which, sp_RbVal *out) SP_COLD SP_NOINLINE;\n");
    buf_puts(&b, "static const char *sp_obj_cls_name_rt(int cls_id) SP_COLD SP_NOINLINE;\n");
  }
  /* The #message / #to_s dispatchers below call these bodies unconditionally,
     so a program that defines an override without ever querying it left the
     dispatcher calling an undeclared function (#3834). Mark them before the
     prototypes are written. */
  if (exc_has_user_msg_override(c) || exc_has_nonstring_msg_override(c)) {
    for (int i = 0; i < c->nclasses; i++) {
      if (!class_is_exc_subclass(c, i) && !class_is_exc_reopen(c, i)) continue;
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
  for (int s = 1; s < c->nscopes; s++) { if (c->scopes[s].yields || (!c->scopes[s].reachable && (!c->scopes[s].is_proc_form || !proc_form_live(c, s))) || scope_is_shadowed(c, s) || (c->scopes[s].is_transplanted_source && !scope_toplevel_included(c, s))) continue; emit_method_signature(c, &c->scopes[s], &b); buf_puts(&b, ";\n"); }

  /* A boxed exception carries SP_BUILTIN_EXCEPTION, not its user class's
     index, so a poly dispatch over a user exception class's own method keys
     it through this map instead (emit_poly_dispatch_key's exc_cand): its
     class name to that index, the arm's struct being the exception header
     followed by the class's ivars (#5093). The name an exception carries is
     its qualified Ruby name ("Storage::WriteError"), as class_ruby_name
     gives it, not the class table's short name. */
  /* A builtin exception's reopening (`class KeyError; def hint`) is an entry
     too: a boxed exception of exactly that class keys to it by name, and one
     of a class under it (a builtin the runtime raised, with only its
     ancestor reopened) by the runtime's ancestry, most-derived reopening
     first in declaration order, a base Exception reopening last. */
  { int any_exc = 0;
    for (int i = 0; i < c->nclasses && !any_exc; i++)
      any_exc = class_is_exc_subclass(c, i) || class_is_exc_reopen(c, i);
    if (any_exc) {
      buf_puts(&b, "SP_UNUSED static int sp_exc_user_cls_id(sp_RbVal v){\n");
      buf_puts(&b, "  const char *n = v.v.p ? ((sp_Exception *)v.v.p)->cls_name : NULL;\n  if (!n) return 0x7fffffff;\n");
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i) && !class_is_exc_reopen(c, i)) continue;
        const char *qn = class_ruby_name(c, i);
        if (!qn) qn = c->classes[i].name;
        if (!qn) continue;
        buf_printf(&b, "  if (strcmp(n, \"%s\") == 0) return %d;\n", qn, i);
      }
      /* the reopening nearest the runtime class up its ancestry, so a
         RuntimeError keys to a RuntimeError reopening ahead of a
         StandardError one declared before it */
      int xr[256], xn = 0;
      for (int i = 0; i < c->nclasses && xn < 256; i++)
        if (class_is_exc_reopen(c, i)) xr[xn++] = i;
      if (xn > 0) {
        buf_puts(&b, "  { ");
        int t = emit_exc_reopen_pick_head(c, xr, xn, "n", &b);
        buf_printf(&b, "static const int _xc%d[] = {", t);
        for (int q = 0; q < xn; q++) buf_printf(&b, "%s%d", q ? ", " : "", xr[q]);
        buf_printf(&b, "}; if (_xi%d >= 0) return _xc%d[_xi%d]; }\n", t, t, t);
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
  /* a reopening's #message / #to_s answering something other than a String
     cannot stand in for the stored message the runtime reads: refused
     rather than left unseen by #message */
  if (any_exc_reopen(c))
    for (int i = 0; i < c->nclasses; i++) {
      if (!class_is_exc_reopen(c, i)) continue;
      for (int k = 0; k < 2; k++) {
        int mi = comp_method_in_chain(c, i, k ? "to_s" : "message", NULL);
        if (mi < 0 || c->scopes[mi].class_id != i) continue;
        TyKind mr = (TyKind)c->scopes[mi].ret;
        if (mr != TY_STRING && mr != TY_UNKNOWN)
          unsupported_feature(c, c->scopes[mi].def_node,
                              "a builtin exception reopening's #message / #to_s that answers a non-String");
      }
    }
  if (exc_has_user_msg_override(c)) {
    for (int pass = 0; pass < 2; pass++) {
      int want_message = pass;  /* 0 = to_s dispatcher, 1 = message dispatcher */
      buf_printf(&b, "SP_UNUSED static const char *%s(sp_Exception *e){\n",
                 want_message ? "sp_user_exc_message" : "sp_user_exc_to_s");
      buf_puts(&b, "  if(!e)return (&(\"\\xff\")[1]);\n  const char *cls=e->cls_name;\n");
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i)) continue;
        int dcls = -1; const char *fn = NULL;
        int mi = exc_text_method(c, i, want_message, &dcls, &fn);
        if (mi < 0) continue;
        if ((TyKind)c->scopes[mi].ret != TY_STRING) continue;  /* string-returning only */
        /* a reopening's method is picked by the runtime class below */
        if (class_is_exc_reopen(c, dcls)) continue;
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
      /* a builtin exception's reopening: #message is the nearest reopening's
         #message, else (Exception#message calls #to_s) the nearest #to_s,
         else the stored message */
      for (int f = want_message ? 0 : 1; f < 2; f++) {
        const char *fn = f ? "to_s" : "message";
        int xr0[8], xr[8], xn = 0;
        int xn0 = exc_reopen_definers(c, fn, xr0, 8);
        for (int q = 0; q < xn0; q++) {
          int mi = comp_method_in_chain(c, xr0[q], fn, NULL);
          if ((TyKind)c->scopes[mi].ret == TY_STRING) xr[xn++] = xr0[q];
        }
        if (xn == 0) continue;
        buf_puts(&b, "  { ");
        int pk = emit_exc_reopen_pick_head(c, xr, xn, "cls", &b);
        buf_puts(&b, "\n");
        for (int q = 0; q < xn; q++)
          buf_printf(&b, "    if (_xi%d == %d) return sp_%s_%s(e);\n", pk, q, mc_reopen_cls(c, xr[q], fn), mc(fn));
        buf_puts(&b, "  }\n");
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
      buf_printf(&b, "SP_UNUSED static sp_RbVal %s(sp_Exception *e){\n",
                 want_message ? "sp_user_exc_message_v" : "sp_user_exc_to_s_v");
      buf_puts(&b, "  if(!e)return sp_box_str((&(\"\\xff\")[1]));\n  const char *cls=e->cls_name;\n");
      for (int i = 0; i < c->nclasses; i++) {
        if (!class_is_exc_subclass(c, i)) continue;
        int dcls = -1; const char *fn = NULL;
        int mi = exc_text_method(c, i, want_message, &dcls, &fn);
        if (mi < 0) continue;
        TyKind mret = (TyKind)c->scopes[mi].ret;
        if (mret == TY_UNKNOWN || mret == TY_VOID) continue;
        if (class_is_exc_reopen(c, dcls)) continue;   /* picked by sp_user_exc_message */
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
          TyKind pm = scope_param_type(s, m);
          emit_ctype(c, pm, &b);
        }
        if (s->nparams == 0) buf_puts(&b, "void");
      }
      else {
        for (int m = 0; m < ci->nmembers; m++) { if (m) buf_puts(&b, ", "); emit_ctype(c, ci->ivar_types[m], &b); }
        if (ci->nmembers == 0) buf_puts(&b, "void");
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
          TyKind pm = scope_param_type(s, m);
          emit_ctype(c, pm, &b);
        }
        if (p_has_blk) { if (s->nparams > 0) buf_puts(&b, ", "); buf_puts(&b, "sp_Proc *"); }
        buf_puts(&b, ");\n");
      }
      else buf_printf(&b, "static sp_%s %ssp_%s_new(void);\n", ci->c_name, star, ci->c_name);
      /* a proc body emitted ahead of the constructor definitions may box a
         value-type instance into its poly return slot */
      if (ci->is_value_type)
        buf_printf(&b, "SP_UNUSED static sp_RbVal sp_box_vobj_%s(sp_%s v);\n", ci->c_name, ci->c_name);
      /* the allocation-only half, for the sites that splice the body */
      if (ctor_init_proc_form(c, i) >= 0) {
        buf_printf(&b, "static sp_%s *sp_%s_new_noinit(", ci->c_name, ci->c_name);
        Scope *s = &c->scopes[init];
        for (int m = 0; m < s->nparams; m++) {
          if (m) buf_puts(&b, ", ");
          emit_ctype(c, scope_param_type(s, m), &b);
        }
        if (s->nparams == 0) buf_puts(&b, "void");
        buf_puts(&b, ");\n");
        buf_printf(&b, "static sp_%s *sp_%s_new_blk(", ci->c_name, ci->c_name);
        for (int m = 0; m < s->nparams; m++) {
          emit_ctype(c, scope_param_type(s, m), &b);
          buf_puts(&b, ", ");
        }
        buf_puts(&b, "sp_Proc *);\n");
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
               /* The interpreter's own flags start false, not nil: the
                  ruby run without -w or -d reads `$VERBOSE` and `$DEBUG` as
                  false, and a nil `$VERBOSE` is the one that silences warn */
               lv->type == TY_POLY && comp_gvar_is_interp_flag(lv->name) ? "{SP_TAG_BOOL, 0, {0}}" :
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
    }
    else {
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
      for (int j = 0; j < ci->nsg_writers; j++)
        if (!comp_is_sg_reader(ci, ci->sg_writers[j]))
          buf_printf(&mk, "  sp_mark_rbval(sg_%s_%s);\n", ci->name, ci->sg_writers[j]);
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
  g_has_user_init_copy = 0;
  for (int id = 0, r, dc; id < c->nt->count && !g_has_user_init_copy; id++) {
    const char *dn = nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
    if (dn && (sp_streq(dn, "dup") || sp_streq(dn, "clone")) &&
        (r = nt_ref(c->nt, id, "receiver")) >= 0 && comp_ntype(c, r) == TY_POLY)
      for (int k = 0; k < c->nclasses && !g_has_user_init_copy; k++)
        if (user_init_copy_scope(c, k, &dc) >= 0) g_has_user_init_copy = 1;
  }
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

  /* the sp_cls_* answers are defined among the dispatches below; a proc body
     that asks one is spliced ahead of them and reads these */
  if (g_gen_cls_answers)
    buf_puts(&b, "static sp_PolyArray *sp_cls_subclasses(sp_RbVal v);\n"
                 "static sp_RbVal sp_cls_allocate(sp_RbVal v);\n"
                 "static sp_PolyArray *sp_cls_members(sp_RbVal v);\n"
                 "static sp_RbVal sp_cls_keyword_init_p(sp_RbVal v);\n");
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
  emit_cls_answers_dispatch(c, body);
  emit_obj_deconstruct_dispatch(c, body);
  emit_obj_is_data(c, body);
  emit_obj_to_a_dispatch(c, body);
  emit_obj_to_ary_dispatch(c, body);
  emit_obj_with_dispatch(c, body);
  for (int s = 1; s < c->nscopes; s++) {
    if (c->scopes[s].yields || (!c->scopes[s].reachable && (!c->scopes[s].is_proc_form || !proc_form_live(c, s))) || scope_is_shadowed(c, s) || (c->scopes[s].is_transplanted_source && !scope_toplevel_included(c, s))) continue;
    int ndiag0 = g_ndiags;
    EMIT_COLLECT_UNIT(emit_method(c, &c->scopes[s], body));
    /* --defer-refusals: a refused method is emitted again with a body that
       raises NotImplementedError naming the refusal, when it is called */
    if (defer_refusals() && g_ndiags > ndiag0 && c->scopes[s].body >= 0) {
      char dmsg[2600];
      const SpDiag *d = &g_diags[g_ndiags - 1];
      if (d->line > 0) snprintf(dmsg, sizeof dmsg, "%s:%d: %s", d->file ? d->file : "?", d->line, d->msg);
      else snprintf(dmsg, sizeof dmsg, "%s", d->msg);
      int ob = c->scopes[s].body;
      int nb = deferred_raise_body(c, ob, s, dmsg);
      if (nb >= 0) {
        c->scopes[s].body = nb;
        EMIT_COLLECT_UNIT(emit_method(c, &c->scopes[s], body));
        c->scopes[s].body = ob;
      }
    }
  }
  /* Comparable cmp-hook dispatcher (after the user `<=>` definitions it calls). */
  emit_synth_line_marker(body);
  if (g_has_user_cmp) emit_obj_cmp_dispatch(c, body);
  if (g_has_user_binop) emit_user_binop_dispatch(c, body);
  if (g_has_user_aset) emit_user_aset_dispatch(c, body);
  if (g_has_user_coerce) emit_user_coerce_dispatch(c, body);
  if (g_has_user_to_io) emit_user_to_io_dispatch(c, body);
  if (g_has_user_init_copy) emit_user_init_copy_dispatch(c, body);
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

  size_t main_frame_ins = 0, main_open = 0;
  int sv_main_cv = g_c_ret_void;
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
    if (g_reads_match_regs) buf_puts(body, "    sp_re_track_last = 1;\n");
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
  main_open = body->len;
  buf_puts(body, "void _sp_main_body(void){\n");
  /* The main body is a `void` C function: an `ensure` epilogue at top level
     (emit_retf_return) must return bare, as it does in a fiber body. */
  g_c_ret_void = 1;
  buf_puts(body, "    SP_GC_SAVE();\n");
  main_frame_ins = body->len;
  if (g_re_init_needed) buf_puts(body, "    sp_tu_init();\n");
  /* Adopt the main thread and chain the scheduler's GC root hook. Placed after
     sp_tu_init so it chains whatever globals hook that installed. */
  if (g_uses_threads) buf_puts(body, "    sp_sched_init();\n");
  /* gsub / sub / scan record their last match only for a program that reads
     it (g_reads_match_regs) */
  if (g_reads_match_regs) buf_puts(body, "    sp_re_track_last = 1;\n");
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
  size_t main_dbeg = body->len;
  int main_ndecl = c->scopes[0].nlocals;
  size_t *main_dends = (size_t *)calloc((size_t)(main_ndecl + 1), sizeof(size_t));
  emit_scope_decls_ends(c, &c->scopes[0], body, main_dends);
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
       the emitted init drops it. Everything else in the toplevel runs. The
       parser folded the predicate to a boolean marked program_guard. */
    int tb9 = c->scopes[0].body;
    const char *tt9 = tb9 >= 0 ? nt_type(c->nt, tb9) : NULL;
    int tn9 = 0;
    const int *tv9 = (tt9 && sp_streq(tt9, "StatementsNode"))
                     ? nt_arr(c->nt, tb9, "body", &tn9) : NULL;
    for (int k9 = 0; k9 < tn9; k9++) {
      int st9 = tv9[k9];
      if (nt_kind(c->nt, st9) == NK_IfNode) {
        int pr9 = nt_ref(c->nt, st9, "predicate");
        if (pr9 >= 0 && nt_int(c->nt, pr9, "program_guard", 0)) continue;
      }
      EMIT_COLLECT_UNIT(emit_stmt(c, st9, body, 1));
    }
  }
  else {
    /* each statement's end is noted, where main_body_split may cut */
    int tb = c->scopes[0].body, tn = 0;
    if (tb >= 0 && nt_kind(c->nt, tb) == NK_StatementsNode) nt_arr(c->nt, tb, "body", &tn);
    size_t *cuts = (size_t *)calloc((size_t)(tn + 1), sizeof(size_t));
    size_t sbeg = body->len;
    volatile int ncuts = 0;   /* set under the collect unit's setjmp */
    EMIT_COLLECT_UNIT(ncuts = emit_top_stmts(c, tb, body, 1, cuts));
    main_body_split(c, body, main_open, &main_frame_ins, main_dbeg, main_dends, main_ndecl, sbeg, cuts, ncuts);
    free(cuts);
  }
  free(main_dends);
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
    /* without hooks, the finalizers still registered run here, where the
       program ends in Ruby terms -- before the C exit handlers, a library's
       own teardown among them (sp_at_exit_run does the same with hooks) */
    else buf_puts(body, "  _sp_main_rc = 0;\n  sp_fin_run_exit();\n}\n");
    g_c_ret_void = sv_main_cv;
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
  if (isa_ext) {
    if (calls_poly_is_a(b.p + isa_ext_at) || calls_poly_is_a(g_procs.p) ||
        calls_poly_is_a(body->p) || calls_poly_is_a(g_pd_defs.p))
      buf_splice(&b, isa_ext_at, isa_ext);
    free(isa_ext);
  }
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
  if (g_pool_fwd) memset(g_pool_fwd, 0, (size_t)g_pool_fwd_n);
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
    write_text_file(types_out, json);
    free(json);
    { const char *keep = getenv("SPINEL_EMIT_TYPES_KEEP_C");
      if (!(keep && *keep)) { free(b.p); b.p = NULL; } }
  }
  if (g_ndiags > 0 && defer_refusals())
    fprintf(stderr, "spinel: %d refusal%s deferred to run time\n", g_ndiags, g_ndiags == 1 ? "" : "s");
  if (g_ndiags > 0 && !collect_emit_anyway() && !defer_refusals()) {
    /* not "unsupported": spinel-doctor and spinel-reduce count the lines
       that say so, and this one is the count */
    fprintf(stderr, "spinel: %d refusal%s%s\n", g_ndiags, g_ndiags == 1 ? "" : "s",
            types_out ? "" : ", nothing written");
    if (types_out) fprintf(stderr, "Wrote %s\n", types_out);
    exit(1);
  }
  if (g_plan_check) ucall_report(c);
  comp_free(c);
  { const char *keep = getenv("SPINEL_EMIT_TYPES_KEEP_C");
    if (types_out && !(keep && *keep)) return strdup(""); }
  return b.p;
}

