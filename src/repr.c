/* repr.c -- a value's C representation, read off the analysis's flags
   (see repr.h). Pure: it reads the types and the flags and changes
   nothing. */

#include <string.h>
#include "repr.h"
#include "codegen_internal.h"

static int repr_sealed_flag;

/* the kind a value of type t is held in, before any flag refines it */
static ReprKind repr_kind_of_type(const Compiler *c, TyKind t) {
  switch (t) {
  case TY_UNKNOWN: case TY_VOID:
    return RK_NONE;
  case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_SYMBOL: case TY_NIL:
    return RK_SCALAR;
  case TY_RANGE: case TY_FLOAT_RANGE: case TY_STR_RANGE: case TY_TIME:
  case TY_TMS: case TY_COMPLEX: case TY_RATIONAL: case TY_CLASS:
    return RK_STRUCT;
  case TY_STRBUF:
    return RK_STRBUF;
  case TY_POLY:
    return RK_BOXED;
  default:
    break;
  }
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    if (cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type) return RK_VOBJ;
  }
  return RK_PTR;
}

/* an object whose class some other class inherits from: its static type is
   only the base, so its box reads the class from the object */
int repr_dyn_cls(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return 0;
  /* an exception's object starts with its class name, not a class id, so
     it is boxed with the static id (emit_boxed) */
  if (c->classes[cid].is_value_type || class_is_exc_subclass((Compiler *)c, cid)) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (k != cid && c->classes[k].parent == cid) return 1;
  return 0;
}

/* An Integer or Float node whose box has to test for the nil sentinel, as
   emit_boxed decides it: the analysis's answer for the node
   (nullable_int_value, through call_returns_nullable_int, which also reads
   a local's slot and a builtin's name), an Integer ivar read (every one is
   nil-initialized), a parameter bound from such an ivar (box_nullable_arg),
   a node in a Ruby-defined builtin (enum_builtin_node), and every Integer
   under --int-overflow=promote. */
int repr_nil_scalar(const Compiler *c, int node, TyKind t) {
  Compiler *mc = (Compiler *)c;
  if (t == TY_INT)
    return g_promote_mode || call_returns_nullable_int(mc, node) ||
           nt_kind(c->nt, node) == NK_InstanceVariableReadNode ||
           box_nullable_arg(mc, node) || enum_builtin_node(mc, node);
  if (t == TY_FLOAT)
    return call_returns_nullable_int(mc, node) || box_nullable_arg(mc, node) ||
           enum_builtin_node(mc, node);
  return 0;
}

/* Where the boxed form of a shared-mutable String comes from, as emit_boxed
   decides it for a node stored as (or holding) the handle. */
static int repr_strbuf_src(const Compiler *c, int node, TyKind t) {
  Compiler *mc = (Compiler *)c;
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (t == TY_STRING) {
    /* a local promoted to the handle after the node types were final */
    if (k != NK_LocalVariableReadNode) return RS_NONE;
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && lv->type == TY_STRBUF && lv->str_shared ? RS_HANDLE : RS_NONE;
  }
  if (t != TY_STRBUF) return RS_NONE;
  if (k == NK_LocalVariableReadNode) {
    /* the mark can outlive the slot's type: a slot that settled poly holds
       the box already */
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    return lv && lv->type == TY_POLY ? RS_SLOT_POLY : RS_HANDLE;
  }
  if (k == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    if (iv >= 0 && c->classes[cid].ivar_types[iv] == TY_STRBUF) return RS_HANDLE;
  }
  /* an ivar write's value is the slot */
  if (k == NK_InstanceVariableWriteNode) return RS_HANDLE;
  /* an element a boxed container hands out is a boxed handle already */
  if (strbuf_boxed_elem_read(mc, node)) return RS_ELEM;
  /* a reader call (or a call answering its receiver) that renders the
     handle itself */
  if (k == NK_CallNode) {
    int r = nt_ref(nt, node, "receiver");
    if (r >= 0 && ty_is_object(comp_ntype(c, r)) &&
        (strbuf_marked_yields_handle(mc, node) || c->strbuf_handle_demand[node]))
      return RS_DEMANDED;
  }
  /* a String value stored where a handle is demanded: a fresh one */
  return RS_FRESH;
}

/* The type a node is STORED as, given its cached type t (comp_ntype's
   String-handle refinement). TY_STRBUF is a codegen-only storage refinement
   (a mutable sp_String for a `<<`-appended local): all type-directed logic
   treats it as a String, and only a read marked strbuf_box yields the live
   HANDLE, so the mutation is observable through the container it is stored
   in (#3227). A node under a handle demand STORES as the handle -- a temp
   spilled from it has to be an sp_String *, not a const char * -- while
   still dispatching as a String, which comp_recv_type answers for. That
   split is the whole point of the second array (#4363). */
TyKind repr_stored_type(const Compiler *c, int id, TyKind t) {
  if (t == TY_STRBUF) return c->strbuf_box[id] ? TY_STRBUF : TY_STRING;
  if (c->strbuf_handle_demand[id]) return TY_STRBUF;
  return t;
}

/* 1 iff t is a user-object type whose class is represented by value (sp_X,
   not a heap pointer). See detect_value_types. */
int repr_value_obj(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  return cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type;
}

/* An argument whose boxing must allow nil though its typed reads need no
   nil check: an int ivar read nothing has to assign first, or a parameter
   already bound from one (box_nullable_arg). */
int repr_box_nullable_arg(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (v < 0) return 0;
  if (nt_kind(nt, v) == NK_InstanceVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0 || cid >= c->nclasses) return 0;
    ClassInfo *ci = &c->classes[cid];
    const char *ivn = nt_str(nt, v, "name");
    int iv = comp_ivar_index(ci, ivn);
    if (iv < 0 || (ci->ivar_types[iv] != TY_INT && ci->ivar_types[iv] != TY_FLOAT)) return 0;
    return !ivar_assigned_in_initialize(c, cid, ivn);
  }
  if (nt_kind(nt, v) == NK_LocalVariableReadNode) {
    Scope *s = comp_scope_of(c, v);
    const char *ln = nt_str(nt, v, "name");
    LocalVar *lv = s && ln ? scope_local(s, ln) : NULL;
    return lv && lv->is_param && lv->box_nullable;
  }
  return 0;
}

/* A local that was assigned a nilable Integer result carries the sentinel
   just as the call did: `i = s.index("z")` then `i == nil` has to answer
   true. The analysis marks the local (call_returns_nullable_int's local
   arm). */
int repr_local_nullable_int(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ln = nt_str(nt, node, "name");
  Scope *sc = ln ? comp_scope_of(c, node) : NULL;
  LocalVar *lv = sc ? scope_local(sc, ln) : NULL;
  return lv && lv->nullable_int;
}

Repr repr_of(const Compiler *c, int node) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = TY_UNKNOWN;
  r.kind = RK_NONE;
  if (node < 0 || node >= c->nt->count) return r;
  r.ty = c->ntype[node];
  r.as_ty = comp_ntype(c, node);
  r.narrowed = c->nilnarrow ? c->nilnarrow[node] : TY_UNKNOWN;
  r.handle = c->strbuf_box[node] != 0;
  r.demand = c->strbuf_handle_demand[node] != 0;
  r.read_raw = c->strbuf_read_raw ? c->strbuf_read_raw[node] != 0 : 0;
  r.poly_lift = c->poly_strbuf_lift ? c->poly_strbuf_lift[node] != 0 : 0;
  /* a node is boxed as the type it is stored as; a nil-guard narrowing is
     read where the value is used, not where it is boxed */
  TyKind kt = r.as_ty;
  r.kind = (unsigned char)repr_kind_of_type(c, kt);
  r.dyn_cls = repr_dyn_cls(c, kt);
  if (repr_nil_scalar(c, node, kt)) {
    r.kind = RK_SENTINEL;
    r.may_nil = r.nil_scalar = 1;
  }
  r.strbuf_src = (unsigned char)repr_strbuf_src(c, node, kt);
  if (r.strbuf_src == RS_SLOT_POLY) r.kind = RK_BOXED;
  else if (r.strbuf_src != RS_NONE) r.kind = RK_STRBUF;
  return r;
}

ReprForm repr_box_form(const Compiler *c, Repr r) {
  TyKind t = r.as_ty;
  switch ((ReprKind)r.kind) {
  case RK_NONE:     return RF_NIL_EFFECT;
  case RK_BOXED:    return RF_PASS;
  case RK_SENTINEL: return t == TY_FLOAT ? RF_FLT_NIL : RF_INT_NIL;
  case RK_STRUCT:   return RF_STRUCT;
  case RK_VOBJ:     return RF_VOBJ;
  case RK_STRBUF:
    return r.strbuf_src == RS_ELEM ? RF_STRBUF_ELEM
         : r.strbuf_src == RS_FRESH ? RF_STRBUF_FRESH : RF_STRBUF_HANDLE;
  case RK_SCALAR:
    switch (t) {
    case TY_INT:    return RF_INT;
    case TY_FLOAT:  return RF_FLT;
    case TY_BOOL:   return RF_BOOL;
    case TY_SYMBOL: return RF_SYM;
    default:        return RF_NIL_EFFECT;   /* nil */
    }
  case RK_PTR:
  default:
    break;
  }
  (void)c;
  if (t == TY_STRING) return RF_STR;
  if (t == TY_BIGINT) return RF_BIGINT;
  if (ty_is_ptr_array(t)) return RF_PTR_ARRAY;
  if (ty_is_object(t)) return r.dyn_cls ? RF_NULLABLE_DYN : RF_NULLABLE;
  return RF_NULLABLE;
}

const char *repr_form_name(int form) {
  static const char *const names[RF__COUNT] = {
    "PASS", "NIL_EFFECT", "INT", "INT_NIL", "FLT", "FLT_NIL", "BIGINT", "STR",
    "BOOL", "SYM", "STRUCT", "NULLABLE", "NULLABLE_DYN", "VOBJ",
    "STRBUF_HANDLE", "STRBUF_FRESH", "STRBUF_ELEM", "PTR_ARRAY", "YIELD", "SPECIAL",
  };
  return form >= 0 && form < RF__COUNT ? names[form] : "?";
}

/* The C value class of a kind: what C allows between two of them. */
int repr_store_class(const Compiler *c, TyKind t) {
  switch (t) {
    case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_SYMBOL: return SC_ARITH;
    case TY_POLY: return SC_BOXED;
    case TY_UNKNOWN: case TY_VOID: case TY_NIL: return SC_NONE;
    default: break;
  }
  if (ty_is_object(t)) return comp_ty_value_obj(c, t) ? SC_STRUCT : SC_PTR;
  if (ty_is_struct_valued(t)) return SC_STRUCT;
  return c_type_name(t) ? SC_PTR : SC_NONE;
}

/* Does a value of kind `from`, written as it is, keep its value in a slot of
   kind `to`? The same C type does; so does an exact arithmetic widening
   (an Integer into a Float slot, a boolean into an Integer one), a nil
   literal's 0 in a pointer slot, which is NULL, and a subclass instance in
   its ancestor's pointer slot. A nil fits as it is only where it is a
   literal (repr_store_nil_fits). An untyped value's C type is whatever its
   emitter chose (a boxed result, the gate's token, a super call's String),
   which the kind does not say, so it is not checked. A void one fits
   nothing. */
/* A nil literal renders as 0, which is a pointer slot's NULL and a boolean's
   false: it is written as it is there, and into an operand a builtin
   converts itself (CO_CONVERT), whose nilable forms read the 0 as they
   always have. Any other nil value -- a call that answers nil, kept for its
   effect -- and a nil into a variable whose nil is a sentinel (an Integer,
   a Float, a Symbol) takes the slot's nil. */
int repr_store_nil_fits(Compiler *c, int node, TyKind slot, int how) {
  return node >= 0 && nt_kind(c->nt, node) == NK_NilNode &&
         (repr_store_class(c, slot) == SC_PTR || slot == TY_BOOL ||
          (how == CO_CONVERT && repr_store_class(c, slot) == SC_ARITH));
}

int repr_store_fits(Compiler *c, TyKind from, TyKind to) {
  if (from == to || to == TY_UNKNOWN || to == TY_VOID || from == TY_UNKNOWN) return 1;
  int fc = repr_store_class(c, from), tc = repr_store_class(c, to);
  if (from == TY_NIL) return 0;   /* see store_nil_fits */
  if (fc == SC_NONE) return 0;
  if (fc == SC_ARITH && tc == SC_ARITH) return from != TY_FLOAT || to == TY_FLOAT;
  if (ty_is_object(from) && ty_is_object(to) && fc == SC_PTR && tc == SC_PTR)
    return is_descendant(c, ty_object_class(from), ty_object_class(to));
  Buf fb, tb;
  memset(&fb, 0, sizeof fb); memset(&tb, 0, sizeof tb);
  emit_ctype(c, from, &fb); emit_ctype(c, to, &tb);
  int same = fb.p && tb.p && sp_streq(fb.p, tb.p);
  free(fb.p); free(tb.p);
  return same;
}

/* Would emit_empty_literal_as build `v` at the slot's kind? An empty
   literal, a bare Array.new or Hash.new, for an Array or Hash slot it has
   a constructor for. */
static int repr_empty_lit_as(Compiler *c, int v, TyKind slot) {
  const NodeTable *nt = c->nt;
  const char *vty = v >= 0 ? nt_type(nt, v) : NULL;
  if (!vty) return 0;
  int n = 0;
  if (sp_streq(vty, "ArrayNode")) {
    nt_arr(nt, v, "elements", &n);
    return !n && (ty_is_ptr_array(slot) || slot == TY_POLY_ARRAY || array_kind(slot));
  }
  if (sp_streq(vty, "HashNode") || sp_streq(vty, "KeywordHashNode")) {
    nt_arr(nt, v, "elements", &n);
    return !n && ty_is_hash(slot) && ty_hash_cname(slot);
  }
  if (sp_streq(vty, "CallNode") && node_is_empty_container(nt, v)) {
    const char *rn = nt_str(nt, nt_ref(nt, v, "receiver"), "name");
    if (rn && sp_streq(rn, "Hash") && ty_is_hash(slot) && ty_hash_cname(slot)) return 1;
    if (rn && sp_streq(rn, "Array"))
      return ty_is_ptr_array(slot) || slot == TY_POLY_ARRAY || array_kind(slot);
  }
  return 0;
}

/* Does emit_poly_rhs_coerced take a boxed value into a `slot` slot? An
   object through the checked unbox, a scalar or a String through its
   conversion. */
static int repr_poly_rhs_ok(TyKind slot) {
  return ty_is_object(slot) || slot == TY_INT || slot == TY_BOOL || slot == TY_FLOAT ||
         slot == TY_SYMBOL || slot == TY_STRING;
}

int repr_coerce_text_form(Compiler *c, int node, TyKind from, TyKind slot, int how) {
  if (repr_store_fits(c, from, slot) || (from == TY_NIL && repr_store_nil_fits(c, node, slot, how)))
    return CF_FIT;
  if (slot == TY_POLY) return CF_BOX;
  if (from == TY_VOID || from == TY_NIL) return CF_NIL_SENT;
  if (slot == TY_BIGINT && from == TY_INT) return CF_INT2BIG;
  if (how == CO_CONVERT && slot == TY_FLOAT && (from == TY_BIGINT || from == TY_RATIONAL))
    return CF_CONVERT;
  return CF_REFUSE;
}

int repr_coerce_plan(Compiler *c, int node, TyKind slot, int how, TyKind *from_out) {
  if (from_out) *from_out = TY_UNKNOWN;
  if (how == CO_CONVERT && slot == TY_BOOL) return CF_CONVERT;
  TyKind from = store_value_kind(c, node);
  if (from_out) *from_out = from;
  int container = ty_is_array(slot) || ty_is_hash(slot);
  if (from == TY_UNKNOWN && container && repr_empty_lit_as(c, node, slot)) return CF_EMPTY_LIT;
  if (repr_store_fits(c, from, slot) || (from == TY_NIL && repr_store_nil_fits(c, node, slot, how)))
    return CF_FIT;
  if (slot == TY_POLY) return CF_BOX;
  if (container && repr_empty_lit_as(c, node, slot)) return CF_EMPTY_LIT;
  if (from == TY_NIL && nt_kind(c->nt, node) == NK_NilNode) return CF_NIL_SENT;
  if (slot == TY_BIGINT && from == TY_INT) return CF_INT2BIG;
  if (from == TY_POLY && how == CO_HOLD) {
    if (repr_poly_rhs_ok(slot)) return CF_POLY_RHS;
    if (ty_is_array(slot) || ty_is_ptr_array(slot) || ty_is_hash(slot) || slot == TY_BIGINT ||
        slot == TY_STRBUF || slot == TY_CLASS || (ty_is_object(slot) && !repr_value_obj(c, slot)))
      return CF_CHECKED_UNBOX;
  }
  return repr_coerce_text_form(c, node, from, slot, how);
}

int repr_coerce_form(Compiler *c, int node, TyKind slot, int how) {
  return repr_coerce_plan(c, node, slot, how, NULL);
}

const char *repr_coerce_form_name(int form) {
  static const char *const names[CF__COUNT] = {
    "FIT", "BOX", "EMPTY_LIT", "NIL_SENT", "INT2BIG", "POLY_RHS", "CHECKED_UNBOX", "CONVERT", "REFUSE",
  };
  return form >= 0 && form < CF__COUNT ? names[form] : "?";
}

int g_repr_check = 0;

Repr repr_of_slot(const Compiler *c, const LocalVar *lv) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = TY_UNKNOWN;
  r.kind = RK_NONE;
  if (!lv) return r;
  r.ty = r.as_ty = lv->type;
  ReprKind k = repr_kind_of_type(c, lv->type);
  /* an Integer or Float slot some write leaves nil in: its sentinel */
  if ((lv->type == TY_INT || lv->type == TY_FLOAT) &&
      (lv->nullable_int || lv->box_nullable || lv->maybe_unset)) {
    k = RK_SENTINEL;
    r.may_nil = 1;
  }
  /* a `||=` can read the slot before any write: nil until then */
  if (lv->or_written) r.may_nil = 1;
  /* a shared-mutable String is held as its handle */
  if (lv->str_shared) { k = RK_STRBUF; r.handle = 1; }
  r.kind = (unsigned char)k;
  r.dyn_cls = repr_dyn_cls(c, lv->type);
  return r;
}

void repr_seal(Compiler *c) {
  (void)c;
  repr_sealed_flag = 1;
}

int repr_sealed(void) { return repr_sealed_flag; }
