/* repr.c -- a value's C representation, read off the analysis's flags
   (see repr.h). Pure: it reads the types and the flags and changes
   nothing. */

#include <string.h>
#include "repr.h"

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
static int repr_dyn_cls(const Compiler *c, TyKind t) {
  if (!ty_is_object(t)) return 0;
  int cid = ty_object_class(t);
  if (cid < 0 || cid >= c->nclasses) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (k != cid && c->classes[k].parent == cid) return 1;
  return 0;
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
  TyKind kt = r.narrowed != TY_UNKNOWN ? r.narrowed : r.as_ty;
  r.kind = (unsigned char)repr_kind_of_type(c, kt);
  r.dyn_cls = repr_dyn_cls(c, kt);
  /* a local's read is held as its slot is: the nil sentinel of a nilable
     Integer or Float, the handle of a shared-mutable String */
  if (nt_kind(c->nt, node) == NK_LocalVariableReadNode && r.narrowed == TY_UNKNOWN) {
    const char *ln = nt_str(c->nt, node, "name");
    Scope *s = ln ? comp_scope_of((Compiler *)c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (lv) {
      Repr sr = repr_of_slot(c, lv);
      if (sr.kind == RK_SENTINEL && (kt == TY_INT || kt == TY_FLOAT)) {
        r.kind = RK_SENTINEL;
        r.may_nil = 1;
      }
      if (sr.kind == RK_STRBUF && (kt == TY_STRING || kt == TY_STRBUF)) {
        r.kind = RK_STRBUF;
        r.handle = 1;
      }
    }
  }
  return r;
}

ReprForm repr_box_form(const Compiler *c, Repr r) {
  TyKind t = r.narrowed != TY_UNKNOWN ? r.narrowed : r.as_ty;
  switch ((ReprKind)r.kind) {
  case RK_NONE:     return RF_NIL_EFFECT;
  case RK_BOXED:    return RF_PASS;
  case RK_SENTINEL: return t == TY_FLOAT ? RF_FLT_NIL : RF_INT_NIL;
  case RK_STRUCT:   return RF_STRUCT;
  case RK_VOBJ:     return RF_VOBJ;
  case RK_STRBUF:   return r.handle ? RF_STRBUF_HANDLE : RF_STRBUF_FRESH;
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
