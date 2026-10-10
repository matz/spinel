/* repr.c -- a value's C representation, read off the analysis's flags
   (see repr.h). Pure: it reads the types and the flags and changes
   nothing. */

#include <string.h>
#include <limits.h>
#include "repr.h"
#include "analyze_internal.h"
#include "codegen_internal.h"
#include "share.h"
#include "holder.h"
#include "call_plan.h"
#include "builtin_ops.h"

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

/* The layout of a value stored as t that its type names and its kind does
   not: every Array and Hash is a pointer, and which container it points to
   -- what an Array holds its elements as (ty_array_elem), what a Hash
   holds its keys and its values as (its variant's row: ty_hash_key,
   ty_hash_val) -- decides the helpers a call on it takes and the C type
   they name; an Integer is an sp_int scalar, and one held big an
   sp_Bigint * pointer, which takes the Bignum helpers. Every Range is a
   by-value struct, and which one -- an sp_Range of Integer bounds, an
   sp_FloatRange of Float ones, an sp_StrRange of Strings -- decides the
   same. A value with no type at all is RK_NONE, as a void one is; untyped
   tells them apart. */
static void repr_layout(Repr *r, TyKind t) {
  r->elem = ty_is_array(t) || ty_is_obj_array(t) ? ty_array_elem(t) : TY_UNKNOWN;
  r->key = ty_hash_key(t);
  r->val = ty_hash_val(t);
  r->range = t == TY_RANGE ? TY_INT : t == TY_FLOAT_RANGE ? TY_FLOAT
           : t == TY_STR_RANGE ? TY_STRING : TY_UNKNOWN;
  r->big = t == TY_BIGINT;
  r->untyped = t == TY_UNKNOWN;
}

/* What a local's slot says about where its value lives and what it holds,
   beside its type: its cell, a String kept volatile across a setjmp, a box
   proven to hold only a PolyArray or nil. A read of the local says the
   same. */
static void repr_slot_storage(Repr *r, const LocalVar *lv) {
  r->cell = (unsigned char)(lv->byref_out ? RC_BYREF : lv->inline_alias ? RC_ALIAS
                            : lv->is_cell ? RC_HEAP : RC_NONE);
  r->volatile_str = lv->borrowed_volatile != 0;
  r->arr_or_nil = lv->arr_or_nil == 1 && lv->type == TY_POLY;
}

/* An Array the analysis saw a nil stored into: an Integer or Float one's
   mark (`marked`, nullable_int_elem), a pointer one's the nil fact's
   (`pmarked`, obj_elem_may_nil: a nil stored, or a gap left). */
static int repr_elem_nil(TyKind elem, int marked, int pmarked) {
  if (elem == TY_INT || elem == TY_FLOAT) return marked != 0;
  return nil_fact_tracked(elem) && pmarked;
}

int repr_hash_is(Repr r, TyKind key, TyKind val) {
  return r.key != TY_UNKNOWN && r.key == key && r.val == val;
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
  /* an Array subclass instance is boxed as its Array (arysub_box_id) */
  if (c->classes[cid].ary_root > 0) return 0;
  for (int k = 0; k < c->nclasses; k++)
    if (k != cid && c->classes[k].parent == cid) return 1;
  return 0;
}

/* An Integer or Float node whose box has to test for nil, as emit_boxed
   decides it: the node has an oint form (node_has_oint_form), the value
   carrying its nil beside it.
   The analysis re-derives a receiver's type (infer_type) to answer, so the
   question is asked as a pure read (an_pure_read_begin): nothing derived is
   recorded, and asking changes nothing codegen reads next. */

int repr_nil_scalar(const Compiler *c, int node, TyKind t) {
  Compiler *mc = (Compiler *)c;
  int r = 0;
  an_pure_read_begin();
  if (t == TY_INT || t == TY_FLOAT) r = node_has_oint_form(mc, node);
  an_pure_read_end();
  return r;
}

/* repr.h: the write's slot holds the rule's handle */
int repr_write_share(const Compiler *c, int node) {
  if (node < 0 || !c->share_strings) return 0;
  Compiler *mc = (Compiler *)c;
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (k == NK_LocalVariableWriteNode || k == NK_LocalVariableOrWriteNode || k == NK_LocalVariableAndWriteNode ||
      k == NK_LocalVariableOperatorWriteNode) {
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    return s && repr_of_slot(c, scope_local(s, ln)).share;
  }
  if (k == NK_InstanceVariableWriteNode || k == NK_InstanceVariableOrWriteNode ||
      k == NK_InstanceVariableAndWriteNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    return iv >= 0 && repr_of_ivar(c, cid, iv).share;
  }
  if (k == NK_GlobalVariableWriteNode || k == NK_GlobalVariableOrWriteNode || k == NK_GlobalVariableAndWriteNode ||
      k == NK_ClassVariableWriteNode || k == NK_ClassVariableOrWriteNode || k == NK_ClassVariableAndWriteNode)
    return repr_static_share(c, node);
  return 0;
}

/* --share-strings: a call whose every target method answers a shared
   String's handle (Scope.ret_handle): the value is that handle, which the
   callee's tail read publishes. (A call marked to be stored as the handle
   is read with its mark lifted, as a String, and picked up here.) */
int repr_call_returns_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  v = unwrap_parens(c, v);
  if (repr_share_rule(c) && v >= 0 && (nt_kind(nt, v) == NK_SuperNode || nt_kind(nt, v) == NK_ForwardingSuperNode)) {
    const CallPlan *p = cplan_user(c, v);
    return p->dispatch == CP_DIRECT && repr_self_handle(c, p->mi) && c->scopes[p->mi].ret_handle;
  }
  if (!repr_share_rule(c) || v < 0 || nt_kind(nt, v) != NK_CallNode) return 0;
  /* a String's value, or one the pickup marks to be stored as the handle */
  TyKind t = c->ntype[v];
  if (t != TY_STRING && !(t == TY_STRBUF && c->strbuf_box[v])) return 0;
  if (strbuf_io_outbuf(c, v) >= 0) return 1;
  /* a Method's call: the method `method(:m)` names */
  int recv = nt_ref(nt, v, "receiver");
  const char *nm = nt_str(nt, v, "name");
  /* A missing ENV key answers the default handle; a present one clears
     the return channel before making its own String. */
  if (recv >= 0 && nt_kind(nt, recv) == NK_ConstantReadNode &&
      is_env_const(nt_str(nt, recv, "name")) && !comp_const(c, "ENV") &&
      bop_share_named(BOP_ENV, nm) == BSH_FETCH && nt_ref(nt, v, "block") < 0) {
    int argc = 0; const int *argv = call_args(nt, v, &argc);
    if (argc == 2 && repr_of(c, argv[1]).kind == RK_STRBUF) return 1;
  }
  if (recv >= 0 && nm && comp_ntype(c, recv) == TY_METHOD && is_call_alias(nm)) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    return mi > 0 && c->scopes[mi].ret_handle;
  }
  int mis[CPT_MAX];
  int n;
  /* Kernel#String calls to_str, or to_s where the class has no to_str.
     A fresh-answering target must keep the ordinary conversion's copy.
     A nil to_str falls through to to_s; a nil to_s falls to fresh object
     text in the bridge, so neither may pick up an earlier publication. */
  if (recv < 0 && is_string_class_name(nm) && comp_method_index(c, nm) < 0 && !bare_call_class_owned(c, v)) {
    int ac = 0;
    const int *av = call_args(nt, v, &ac);
    if (ac != 1 || nt_ref(nt, v, "block") >= 0 || repr_of(c, av[0]).kind != RK_BOXED) return 0;
    const int *ks = poly_recv_classes(c, v, &n);
    if (!ks || n <= 0 || n > CPT_MAX) return 0;
    for (int i = 0; i < n; i++) {
      int mi = comp_method_in_chain(c, ks[i], "to_str", NULL);
      if (mi >= 0 && c->scopes[mi].ret_nil_pickup) {
        int fallback = comp_method_in_chain(c, ks[i], "to_s", NULL);
        if (fallback < 0 || !c->scopes[fallback].ret_handle || c->scopes[fallback].ret_nil_pickup) return 0;
      }
      if (mi < 0) {
        mi = comp_method_in_chain(c, ks[i], "to_s", NULL);
        if (mi >= 0 && c->scopes[mi].ret_nil_pickup) return 0;
      }
      if (mi < 0) return 0;
      mis[i] = mi;
    }
  }
  else n = cplan_targets(c, v, mis, CPT_MAX);
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) if (!c->scopes[mis[i]].ret_handle) return 0;
  return 1;
}
/* A boxed receiver's reader arms already box their shared fields as
   handles. A String demand can take that boxed result without a copy.
   Names with a builtin face keep their own route. */
int repr_boxed_reader_handle(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (!repr_share_rule(c) || v < 0 || nt_kind(nt, v) != NK_CallNode ||
      (c->ntype[v] != TY_STRING && !(c->ntype[v] == TY_STRBUF && c->strbuf_box[v])) ||
      nt_ref(nt, v, "block") >= 0 ||
      nt_ref(nt, v, "arguments") >= 0) return 0;
  int recv = nt_ref(nt, v, "receiver");
  if (recv < 0 || repr_of(c, recv).kind != RK_BOXED ||
      ty_poly_face_owners(nt_str(nt, v, "name"), 0, 0, 1, 1)) return 0;
  const PolyPlan *p = cplan_poly_arms(c, v);
  if (p->n == 0) return 0;
  for (int i = 0; i < p->n; i++)
    if (p->arm[i].kind != PA_READER || p->arm[i].vty != TY_STRBUF) return 0;
  return 1;
}

/* A boxed to_s can answer the String in the box itself. The route takes a
   block-less, argument-less to_s on a boxed receiver, typed as a String or
   shared buffer, under --share-strings. Every user method the call can
   dispatch to must return the handle or a new String; a target answering
   anything else, or one the call cannot resolve, declines. Builtin arms
   are not among those targets. */
int repr_boxed_to_s_operand(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (!repr_share_rule(c) || v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  const char *nm = nt_str(nt, v, "name");
  int recv = nt_ref(nt, v, "receiver");
  if (!is_to_s_name(nm) || recv < 0 || call_plain_argc(c, v) != 0 ||
      nt_ref(nt, v, "block") >= 0 || repr_of(c, recv).kind != RK_BOXED ||
      (c->ntype[v] != TY_STRING && c->ntype[v] != TY_STRBUF)) return -1;
  int targets[CPT_MAX], n = cplan_targets(c, v, targets, CPT_MAX);
  if (n < 0) return -1;
  for (int i = 0; i < n; i++) {
    Scope *m = &c->scopes[targets[i]];
    if (m->ret != TY_STRING || (!m->ret_handle && !m->ret_fresh)) return -1;
  }
  return recv;
}

int repr_self_handle(const Compiler *c, int scope) {
  if (!c->share_strings) return 0;
  int h = share_self_holder(c, scope);
  return share_self_used(c, h) && repr_str_shares(c, h);
}

int repr_self_shared(const Compiler *c, int node) {
  if (!c->share_strings || node < 0 || nt_kind(c->nt, node) != NK_SelfNode) return 0;
  Scope *s = comp_scope_of((Compiler *)c, node);
  return s && repr_self_handle(c, (int)(s - c->scopes));
}

/* A builtin receiver conversion on a String keeps its handle. The seal,
   the route emitter and boxed-form prediction use the same fact. */
int repr_string_conversion_operand(Compiler *c, int v) {
  v = unwrap_parens(c, v);
  if (!repr_share_rule(c) || v < 0 || nt_kind(c->nt, v) != NK_CallNode) return -1;
  int r = nt_ref(c->nt, v, "receiver");
  return r >= 0 && is_receiver_conversion(nt_str(c->nt, v, "name")) &&
         call_plain_argc(c, v) == 0 && nt_ref(c->nt, v, "block") < 0 &&
         comp_recv_type(c, r) == TY_STRING && cplan_user(c, v)->dispatch == CP_NONE ? r : -1;
}
/* ENV copies its stored bytes, but a store answers its value operand.
   A boxed operand can still hold plain bytes: an observable alias stays
   refused. A mutable key held across value evaluation that can change it
   must carry its handle too. */
int repr_env_store_operand(Compiler *c, int v) {
  const NodeTable *nt = c->nt;
  if (!repr_share_rule(c) || v < 0 || nt_kind(nt, v) != NK_CallNode) return -1;
  int r = nt_ref(nt, v, "receiver"), argc = 0;
  const int *argv = call_args(nt, v, &argc);
  return r >= 0 && nt_kind(nt, r) == NK_ConstantReadNode && is_env_const(nt_str(nt, r, "name")) &&
         !comp_const(c, "ENV") && argc == 2 && bop_share_named(BOP_ENV, nt_str(nt, v, "name")) == BSH_LAST &&
         (repr_of(c, argv[1]).kind != RK_BOXED || share_value_unobserved(c, argv[1])) &&
         (!(share_node_flags(c, argv[0]) & SHF_MUT) || repr_of(c, argv[0]).handle ||
          share_value_fresh(c, argv[0], 0) || !subtree_has_side_effect(c, argv[1])) ? argv[1] : -1;
}
/* Where the boxed form of a shared-mutable String comes from, as emit_boxed
   decides it for a node stored as (or holding) the handle. */
static int repr_strbuf_src(const Compiler *c, int node, TyKind t) {
  Compiler *mc = (Compiler *)c;
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, node);
  if (repr_self_shared(c, node)) return RS_HANDLE;
  if (t == TY_STRING) {
    /* a global holding the handle (--share-strings): its read boxes it */
    if (repr_static_read_kind(k)) return repr_static_share(c, node) ? RS_HANDLE : RS_NONE;
    /* An ivar promoted after the node types settled still reads as
       String here. Its box carries the shared slot's handle too. */
    if (c->share_strings && k == NK_InstanceVariableReadNode) {
      const char *nm = nt_str(nt, node, "name");
      int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
      int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
      return iv >= 0 && repr_of_ivar(c, cid, iv).share ? RS_HANDLE : RS_NONE;
    }
    /* a write in value position whose slot holds the handle the rule
       assigned: its value is that slot, as the slot's read is */
    if (repr_write_share(c, node)) return RS_HANDLE;
    /* a local promoted to the handle after the node types were final */
    if (k != NK_LocalVariableReadNode) return RS_NONE;
    const char *ln = nt_str(nt, node, "name");
    Scope *s = ln ? comp_scope_of(mc, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (c->share_strings && lv && lv->type == TY_POLY && c->nilnarrow[node] == TY_STRBUF) return RS_SLOT_POLY;
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
  /* a global holding the handle (--share-strings) */
  if ((repr_static_read_kind(k) || k == NK_GlobalVariableWriteNode) && repr_static_share(c, node))
    return RS_HANDLE;
  /* an ivar write's value is the slot when the slot is the handle; a
     plain String slot's value is a String, wrapped fresh below (a raise
     arm beside it boxed that `const char *` as the handle, and the C did
     not build) */
  if (k == NK_InstanceVariableWriteNode) {
    const char *nm = nt_str(nt, node, "name");
    int cid = nm ? strbuf_ivar_owner(mc, node) : -1;
    int iv = cid >= 0 ? comp_ivar_index(&c->classes[cid], nm) : -1;
    if (iv < 0 || c->classes[cid].ivar_types[iv] == TY_STRBUF) return RS_HANDLE;
  }
  /* so is any write's whose slot holds the rule's handle */
  if (repr_write_share(c, node)) return RS_HANDLE;
  /* an element a boxed container hands out is a boxed handle already */
  if (strbuf_boxed_elem_read(mc, node)) return RS_ELEM;
  /* a reader call (or a call answering its receiver) that renders the
     handle itself */
  if (k == NK_CallNode) {
    /* A boxed receiver route keeps its handle beside fresh user answers. */
    if (repr_boxed_to_s_operand(mc, node) >= 0) return RS_HANDLE;
    if (repr_string_conversion_operand(mc, node) >= 0) return RS_HANDLE;
    int ops[3];
    if (strbuf_route_clamp(mc, node, ops)) return RS_HANDLE;
    if (repr_env_store_operand(mc, node) >= 0) return RS_HANDLE;
    /* A demanded call whose return route carries a handle is already that
       handle, including when operand ordering holds it in a temp. */
    if (c->strbuf_handle_demand[node] && repr_call_returns_handle(mc, node)) return RS_DEMANDED;
    /* A container store can mark the reader itself as the handle too. */
    if (repr_boxed_reader_handle(mc, node)) return RS_DEMANDED;
    /* The implicit-self reader boxes its slot too, not a fresh String. */
    if (c->share_strings && (c->strbuf_box[node] || c->strbuf_handle_demand[node]) &&
        strbuf_self_reader_slot(mc, node)) return RS_DEMANDED;
    int r = nt_ref(nt, node, "receiver");
    if (r >= 0 && ty_is_object(comp_ntype(c, r)) &&
        (strbuf_marked_yields_handle(mc, node) || c->strbuf_handle_demand[node]))
      return RS_DEMANDED;
    /* A boxed return pickup keeps its published handle too. A demanded
       reader above already emits the handle, including from a held temp. */
    if (c->strbuf_box[node] &&
        (repr_call_returns_handle(mc, node) || strbuf_route_inline_call(mc, node))) return RS_HANDLE;
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

/* A local that was assigned a nilable Integer result holds its nil as the
   call's answer did: `i = s.index("z")` then `i == nil` has to answer
   true. The analysis marks the local (call_returns_nullable_int's local
   arm). */
int repr_local_nullable_int(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  const char *ln = nt_str(nt, node, "name");
  Scope *sc = ln ? comp_scope_of(c, node) : NULL;
  LocalVar *lv = sc ? scope_local(sc, ln) : NULL;
  return lv && lv->nullable_int;
}

/* Can a value held as t be nil? A user object and a builtin pointer the
   analysis follows (nil_fact_tracked: a String, an Array, a Hash, an IO)
   answer by their nil fact, `fact` (analyze_nil.c, #7444). Any other kind
   whose NULL is its nil (a Bignum, a String buffer, a Proc, a Method, a
   MatchData, ...) has no fact, so it can hold one. */
static int repr_may_nil(TyKind t, int fact) {
  return nil_fact_tracked(t) ? fact : ty_null_is_nil(t);
}

Repr repr_of(const Compiler *c, int node) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = r.elem = r.key = r.val = r.range = TY_UNKNOWN;
  r.kind = RK_NONE;
  r.untyped = 1;
  if (node < 0 || node >= c->nt->count) return r;
  r.ty = c->ntype[node];
  r.as_ty = comp_ntype(c, node);
  /* the layout first, from the type it is stored as, then the flags:
     GCC 13's store merging at -O2 drops the nil_cold store when
     repr_layout's bitfield stores come after it; setting the layout first
     avoids it */
  repr_layout(&r, r.as_ty);
  r.narrowed = c->nilnarrow ? c->nilnarrow[node] : TY_UNKNOWN;
  r.handle = c->strbuf_box[node] != 0;
  r.demand = c->strbuf_handle_demand[node] != 0;
  r.read_raw = c->strbuf_read_raw ? c->strbuf_read_raw[node] != 0 : 0;
  r.poly_lift = c->poly_strbuf_lift ? c->poly_strbuf_lift[node] != 0 : 0;
  r.nil_tested = c->nil_tested ? c->nil_tested[node] != 0 : 0;
  r.nil_cold = c->nil_tested ? c->nil_tested[node] == 2 : 0;
  r.head_held = c->head_held ? c->head_held[node] != 0 : 0;
  /* a node is boxed as the type it is stored as; a nil-guard narrowing is
     read where the value is used, not where it is boxed */
  TyKind kt = r.as_ty;
  r.kind = (unsigned char)repr_kind_of_type(c, kt);
  r.dyn_cls = repr_dyn_cls(c, kt);
  if (repr_nil_scalar(c, node, kt)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  r.strbuf_src = (unsigned char)repr_strbuf_src(c, node, kt);
  r.share = (unsigned)repr_static_share(c, node);
  if (r.strbuf_src == RS_SLOT_POLY && !(c->share_strings && r.narrowed == TY_STRBUF)) r.kind = RK_BOXED;
  else if (r.strbuf_src != RS_NONE) r.kind = RK_STRBUF;
  /* a pointer that can be nil (repr_may_nil); a by-value user object the
     nil fact says may be nil too, whose layout has no nil to hold it in */
  if ((r.kind == RK_PTR || r.kind == RK_VOBJ || r.kind == RK_STRBUF) &&
      repr_may_nil(kt, nil_fact_tracked(kt) && nil_fact_node(c, node)))
    r.may_nil = 1;
  /* an Integer or Float Array the analysis marked (nullable_int_elem_array
     asks the node's source, as a pure read) */
  if (r.elem == TY_INT || r.elem == TY_FLOAT) {
    an_pure_read_begin();
    r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, nullable_int_elem_array((Compiler *)c, node), 0);
    an_pure_read_end();
  }
  else r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, 0, nil_elem_fact_node(c, node));
  /* a local read says what its slot does about where the value lives */
  if (nt_kind(c->nt, node) == NK_LocalVariableReadNode) {
    const char *ln = nt_str(c->nt, node, "name");
    Scope *s = ln ? comp_scope_of((Compiler *)c, node) : NULL;
    LocalVar *lv = s ? scope_local(s, ln) : NULL;
    if (lv) repr_slot_storage(&r, lv);
  }
  return r;
}

ReprForm repr_box_form(const Compiler *c, Repr r) {
  TyKind t = r.as_ty;
  switch ((ReprKind)r.kind) {
  case RK_NONE:     return RF_NIL_EFFECT;
  case RK_BOXED:    return RF_PASS;
  case RK_OPT: return t == TY_FLOAT ? RF_FLT_NIL : RF_INT_NIL;
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
  if (r.big) return RF_BIGINT;
  /* an Array of pointers (objects, nested Arrays) is stamped with what its
     elements are */
  if (ty_is_object(r.elem) || ty_is_array(r.elem)) return RF_PTR_ARRAY;
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
   effect -- and a nil into a variable whose nil is not a 0 (an Integer or
   a Float, its nil beside the value; a Symbol) takes the slot's nil. */
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
  /* a String into the shared handle slot an ivar holds under the share
     rule: the handle the value is, or a fresh one around it, as a plain
     `@x = v` stores it. A frozen literal is the handle of its own site
     (SP_SHARE_STRING_LITERALS), so it keeps one identity and stays frozen */
  if (slot == TY_STRBUF && how == CO_HOLD && from == TY_STRING && repr_share_rule(c)) return CF_STRBUF_HANDLE;
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
    "FIT", "BOX", "EMPTY_LIT", "NIL_SENT", "INT2BIG", "POLY_RHS", "CHECKED_UNBOX", "STRBUF_HANDLE", "CONVERT", "REFUSE",
  };
  return form >= 0 && form < CF__COUNT ? names[form] : "?";
}

int g_repr_check = 0;

/* repr_of is a pure read: asked anywhere, it changes nothing. Under
   --repr-check codegen asks it of every node it is about to emit, which an
   ordinary compile does not, and repr_check.sh fails when the C then
   differs from the C without the flag: a type recorded while a view was
   open re-materialized a Range from its own temp. */
void repr_check_ask(const Compiler *c, int node) {
  if (node >= 0 && node < c->nt->count) (void)repr_of(c, node);
}

/* Like the boxing shadow, this observes emission without changing it. The
   existing settled return walk records leaves, not a second return walk.
   Frames distinguish repeated emissions of a node; an enclosing frame
   drops abandoned trial frames just as pa_end does. */
typedef struct { int mi, active; unsigned char want, seen; } ChannelLeaf;
typedef struct { int node, form, ensure, clearing; } ChannelFrame;
struct ReprChannelCheck {
  ChannelLeaf *leaf;
  ChannelFrame *frame;
  int count, n, cap, clearing;
  unsigned long checked, conflicts, pickups;
};

static void channel_grow(struct ReprChannelCheck *q) {
  if (q->n < q->cap) return;
  if (q->cap > INT_MAX / 2) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  q->cap = q->cap ? q->cap * 2 : 16;
  q->frame = realloc(q->frame, (size_t)q->cap * sizeof *q->frame);
  if (!q->frame) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
}

static void channel_drop(struct ReprChannelCheck *q, int frame) {
  q->clearing = q->frame[frame].clearing;
  while (q->n > frame) {
    ChannelFrame *d = &q->frame[--q->n];
    if (!d->ensure) q->leaf[d->node].active = 0;
  }
}

void repr_channel_predict(Compiler *c, int node, int mi, int form) {
  if (!c->share_strings || node < 0) return;
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) {
    q = calloc(1, sizeof *q);
    if (!q) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    q->count = c->nt->count;
    q->leaf = calloc((size_t)q->count, sizeof *q->leaf);
    if (!q->leaf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    c->repr_channel_check = q;
  }
  if (node >= q->count) {
    int count = c->nt->count;
    q->leaf = realloc(q->leaf, (size_t)count * sizeof *q->leaf);
    if (!q->leaf) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    memset(q->leaf + q->count, 0, (size_t)(count - q->count) * sizeof *q->leaf);
    q->count = count;
  }
  q->leaf[node].mi = mi;
  q->leaf[node].want = (unsigned char)form;
}

int repr_channel_begin(Compiler *c, int node) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || node < 0 || node >= q->count) return -1;
  ChannelLeaf *l = &q->leaf[node];
  if (!l->want || l->active || l->mi <= 0 || l->mi >= c->nscopes) return -1;
  Scope *m = &c->scopes[l->mi];
  if (!m->ret_handle && !m->ret_channel_check) return -1;
  channel_grow(q);
  int f = q->n++;
  q->frame[f] = (ChannelFrame){ node, RCH_NONE, 0, q->clearing };
  l->active = f + 1;
  return f;
}

void repr_channel_clearing(Compiler *c, int delta) {
  if (c->repr_channel_check) c->repr_channel_check->clearing += delta;
}

int repr_channel_ensure(Compiler *c, int node) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  Scope *m = q ? comp_scope_of(c, node) : NULL;
  if (!m || (!m->ret_handle && !m->ret_channel_check)) return -1;
  channel_grow(q);
  int f = q->n++;
  q->frame[f] = (ChannelFrame){ node, RCH_NONE, 1, q->clearing };
  return f;
}

void repr_channel_note(Compiler *c, int node, int form) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || node < 0 || node >= q->count) return;
  /* An ensure can overwrite a saved return's channel even when its own
     value is dropped. Report the unproved preservation separately: an
     ensure that returns instead can deliberately supersede that value. */
  if (form == RCH_PUBLISH || form == RCH_CLEAR)
    for (int i = 0; i < q->n; i++) if (q->frame[i].ensure) q->frame[i].form = form;
  int f = q->leaf[node].active - 1;
  if (f >= 0 && f < q->n) q->frame[f].form = form;
}

static const char *channel_form(int form) {
  const char *const names[] = { "unobserved", "publish", "clear", "nil", "handle", "bytes", "boxed" };
  return form >= RCH_NONE && form <= RCH_BOXED ? names[form] : "?";
}

void repr_channel_end(Compiler *c, int frame) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q || frame < 0 || frame >= q->n) return;
  ChannelFrame f = q->frame[frame];
  if (f.ensure) {
    if (f.form != RCH_NONE)
      fprintf(stderr, "repr-check: channel-gap: node %d ensure: emitted %s after saved value; preservation unproved\n",
              f.node, channel_form(f.form));
    channel_drop(q, frame);
    return;
  }
  ChannelLeaf *l = &q->leaf[f.node];
  if (q->clearing) f.form = RCH_CLEAR;
  /* The early pickup walk can precede the owned-return proof. Its local
     read still publishes, but the settled ownership fact permits clearing.
     A call returning a fresh bound argument has the same permission once
     its parameter-return fact has settled. */
  int fresh = share_return_owned(c, f.node, l->mi) ||
              share_node_fresh(c, f.node) || share_call_fresh(c, f.node);
  int want = fresh ? RCH_CLEAR : l->want;
  l->seen = 1;
  q->checked++;
  /* nil is decided by the returned bytes, before any pickup reads the
     channel. A handle-valued view/boxed return carries the value itself. */
  if (want != RCH_NIL && f.form != RCH_HANDLE && f.form != RCH_BOXED && f.form != want &&
      !(fresh && f.form == RCH_PUBLISH)) {
    int conflict = want == RCH_CLEAR || f.form != RCH_NONE;
    const char *cls = conflict ? "channel-conflict" : "channel-unobserved";
    if (conflict) q->conflicts++;
    fprintf(stderr, "repr-check: %s: node %d %s method %s: emitted %s, predicted %s\n",
            cls, f.node, nt_type(c->nt, f.node), c->scopes[l->mi].name,
            channel_form(f.form), channel_form(want));
  }
  channel_drop(q, frame);
}

void repr_channel_call(Compiler *c, int node, int mi) {
  if (mi > 0 && mi < c->nscopes && c->scopes[mi].ret_handle)
    repr_channel_note(c, node, RCH_PUBLISH);
}

void repr_channel_boxed(Compiler *c, int node, int frame) {
  repr_channel_note(c, node, RCH_BOXED);
  repr_channel_end(c, frame);
}

void repr_channel_pickup(Compiler *c, int node, int nil_guard) {
  if (!c->share_strings) return;
  int mis[CPT_MAX], n = cplan_targets(c, node, mis, CPT_MAX);
  if (c->repr_channel_check) c->repr_channel_check->pickups++;
  for (int i = 0; i < n; i++) {
    Scope *m = &c->scopes[mis[i]];
    /* The earlier pickup proof also admits owned handles and mixed fresh
       tails. ret_handle is the later forwarding proof, not its negation. */
    if ((m->ret_handle || m->ret_channel_check) && (!m->ret_nil_pickup || nil_guard)) continue;
    fprintf(stderr, "repr-check: channel-conflict: node %d pickup method %s: "
            "ret_handle %d ret_fresh %d ret_nil_pickup %d nil_guard %d\n",
            node, m->name, m->ret_handle, m->ret_fresh, m->ret_nil_pickup, nil_guard);
  }
  if (n <= 0)
    fprintf(stderr, "repr-check: channel-unobserved: node %d pickup: targets %d\n", node, n);
}

void repr_channel_report(Compiler *c) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) return;
  int unseen = 0;
  for (int i = 0; i < q->count; i++) {
    ChannelLeaf *l = &q->leaf[i];
    if (l->want && !l->seen && l->mi > 0 && c->scopes[l->mi].ret_handle) unseen++;
  }
  fprintf(stderr, "repr-check: channel: %lu tails, %lu conflicts, %lu pickups, %d unemitted leaves\n",
          q->checked, q->conflicts, q->pickups, unseen);
}

void repr_channel_free(Compiler *c) {
  struct ReprChannelCheck *q = c->repr_channel_check;
  if (!q) return;
  free(q->leaf); free(q->frame); free(q);
  c->repr_channel_check = NULL;
}

ReprKind repr_slot_kind(const Compiler *c, const LocalVar *lv) {
  if (!lv) return RK_NONE;
  /* an Integer or Float slot that holds its nil beside the value */
  if (slot_is_oint(lv)) return RK_OPT;
  return repr_kind_of_type(c, lv->type);
}
Repr repr_of_slot(const Compiler *c, const LocalVar *lv) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = r.narrowed = r.elem = r.key = r.val = r.range = TY_UNKNOWN;
  r.kind = RK_NONE;
  r.untyped = 1;
  if (!lv) return r;
  r.ty = r.as_ty = lv->type;
  repr_layout(&r, lv->type);
  ReprKind k = repr_slot_kind(c, lv);
  if (k == RK_OPT) r.may_nil = r.nil_scalar = 1;
  /* a `||=` can read the slot before any write: nil until then */
  if (lv->or_written) r.may_nil = 1;
  /* a pointer slot that can hold nil (repr_may_nil) */
  if (repr_may_nil(lv->type, lv->obj_may_nil)) r.may_nil = 1;
  /* str_shared refines TY_STRBUF; it can outlive that storage type */
  if (lv->type == TY_STRBUF && lv->str_shared) r.handle = 1;
  /* under the rule, that handle is the one it assigned */
  r.share = r.handle && c->share_strings;
  r.elems_handle = lv->elems_shared && lv->type == TY_POLY_ARRAY;
  r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, lv->nullable_int_elem, lv->obj_elem_may_nil);
  repr_slot_storage(&r, lv);
  r.kind = (unsigned char)k;
  r.dyn_cls = repr_dyn_cls(c, lv->type);
  return r;
}

static void repr_share_seal(Compiler *c);

/* Method and constructor signatures pass a kept &block as sp_Proc *.
   Check the slot as well as expression boxing: a read can infer Proc even
   when a copied parameter's local was left untyped and widened to POLY. */
static void repr_check_block_params(Compiler *c) {
  for (int si = 1; si < c->nscopes; si++) {
    Scope *sc = &c->scopes[si];
    if (!sc->blk_param || !sc->blk_param[0] || sc->yields) continue;
    LocalVar *lv = scope_local(sc, sc->blk_param);
    Repr r = repr_of_slot(c, lv);
    if (!lv || !lv->is_param || r.ty != TY_PROC || r.kind != RK_PTR)
      fprintf(stderr, "repr-check: conflict: method %s block parameter %s: "
              "slot %s, signature sp_Proc *\n", sc->name ? sc->name : "?",
              sc->blk_param, ty_name(r.ty));
  }
}

void repr_seal(Compiler *c) {
  if (g_repr_check) repr_check_block_params(c);
  if (c->share_strings) repr_share_seal(c);
  repr_sealed_flag = 1;
}

int repr_sealed(void) { return repr_sealed_flag; }

/* ---- --share-strings (#6765) ---- */

int repr_static_read_kind(NodeKind k) {
  return k == NK_GlobalVariableReadNode || k == NK_ConstantReadNode || k == NK_ConstantPathNode ||
         k == NK_ClassVariableReadNode;
}
/* repr_of's `share` for a read or write node of a global, a constant or a
   class variable: its slot's (only the rule makes one the handle) */
int repr_static_share(const Compiler *c, int node) {
  HolderRef h;
  if (node < 0 || !c->share_strings || !holder_static_node(nt_kind(c->nt, node))) return 0;
  return holder_of_node(c, node, &h) && h.r.share;
}

int repr_str_class_shares(unsigned flags, int holders) {
  if (flags & SHF_IDENTITY) return 1;
  if (!(flags & SHF_MUT)) return 0;
  return holders >= 2 || (flags & (SHF_UNKNOWN | SHF_INDIRECT | SHF_MULTI)) != 0;
}

int repr_str_shares(const Compiler *c, int holder) {
  if (!c->share_strings || !c->share || holder < 0) return 0;
  return repr_str_class_shares(share_class_flags(c, holder), share_class_holders(c, holder));
}

int repr_str_elems_share(const Compiler *c, int holder) {
  if (!c->share_strings || !c->share || holder < 0) return 0;
  int e = share_elem_holder(c, holder);
  return e >= 0 && repr_str_class_shares(share_elem_flags(c, e), share_elem_holders(c, e));
}

/* The representation of share holder h's slot, now that the analysis is
   final: a box (RK_BOXED) holds whatever is stored, the handle included;
   `share` says the slot is the handle. A byref-out local is not (its slot
   is the caller's), and a class variable is every class's of the name (the
   facts key it by name alone): boxed if one is, and the handle only if
   every String one is. ty is TY_UNKNOWN for a holder with no slot. */
static Repr repr_of_share_holder(Compiler *c, const ShareHolder *h) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = TY_UNKNOWN;
  switch (h->kind) {
  case SHK_SELF:
    if (!share_self_used(c, share_self_holder(c, h->scope))) break;
    r.ty = r.as_ty = TY_STRING;
    r.share = repr_self_handle(c, h->scope);
    break;
  case SHK_LOCAL: {
    LocalVar *lv = &c->scopes[h->scope].locals[h->local];
    r = repr_of_slot(c, lv);
    if (lv->byref_out) r.share = 0;
    break;
  }
  case SHK_IVAR: {
    int iv = comp_ivar_index(&c->classes[h->cid], h->name);
    if (iv >= 0) r = repr_of_ivar(c, h->cid, iv);
    break;
  }
  case SHK_GVAR: {
    LocalVar *gv = comp_gvar(c, h->name[0] == '$' ? h->name + 1 : h->name);
    if (gv) r = repr_of_slot(c, gv);
    break;
  }
  case SHK_CONST: {
    LocalVar *cv = h->name ? comp_const(c, h->name) : NULL;
    if (cv) r = repr_of_slot(c, cv);
    break;
  }
  case SHK_CVAR:
    for (int k = 0; k < c->nclasses; k++) {
      int i = h->name ? comp_cvar_index(&c->classes[k], h->name) : -1;
      if (i < 0) continue;
      Repr ck = repr_of_cvar(c, k, i);
      if (ck.ty == TY_POLY) { ck.kind = RK_BOXED; return ck; }
      if (ck.share) r = ck;
      else if (ck.ty == TY_STRING || ck.ty == TY_STRBUF) return ck;
    }
    break;
  default:
    break;
  }
  if (r.ty == TY_POLY) r.kind = RK_BOXED;
  return r;
}

/* A container holder whose elements share: are they boxed (a box holds the
   handle), or typed Strings (`const char *` elements)? 1, 0, or -1 for a
   holder that is no container. A global, a constant and a class variable
   count too: a String Array constant whose elements a block parameter
   changes as the handle holds copies of them */
static int repr_share_elems_carried(Compiler *c, const ShareHolder *h) {
  TyKind t = TY_UNKNOWN;
  if (h->kind == SHK_LOCAL) t = c->scopes[h->scope].locals[h->local].type;
  else if (h->kind == SHK_IVAR) {
    int iv = comp_ivar_index(&c->classes[h->cid], h->name);
    if (iv < 0) return -1;
    t = c->classes[h->cid].ivar_types[iv];
  }
  else if (h->kind == SHK_GVAR || h->kind == SHK_CONST) t = repr_of_share_holder(c, h).ty;
  else if (h->kind == SHK_CVAR) {
    /* every class's of the name, which the facts key as one: a typed
       container of any of them holds copies */
    int any = -1;
    for (int k = 0; k < c->nclasses; k++) {
      int i = h->name ? comp_cvar_index(&c->classes[k], h->name) : -1;
      TyKind ct = i >= 0 ? c->classes[k].cvar_types[i] : TY_UNKNOWN;
      if (!ty_is_array(ct) && !ty_is_hash(ct)) continue;
      if (ct == TY_STR_ARRAY || ct == TY_STR_STR_HASH || ct == TY_INT_STR_HASH) return 0;
      any = 1;
    }
    return any;
  }
  if (!ty_is_array(t) && !ty_is_hash(t)) return -1;
  return repr_typed_str_container(t) ? 0 : 1;
}

/* repr.h: a container type whose C form holds its Strings as `const char *` */
int repr_typed_str_container(TyKind t) {
  return t == TY_STR_ARRAY || t == TY_STR_STR_HASH || t == TY_INT_STR_HASH;
}

int repr_str_literal_shares(Compiler *c, int node) {
  if (!c->share_strings || node < 0) return 0;
  NodeKind k = nt_kind(c->nt, node);
  return (k == NK_ArrayNode || k == NK_HashNode) &&
         repr_typed_str_container(c->ntype[node]) && comp_scope_of(c, node)->reachable &&
         share_node_elems_share(c, node) && share_node_anchored(c, node);
}

static const char *repr_share_kind_name(int kind) {
  switch (kind) {
  case SHK_LOCAL: return "variable";
  case SHK_IVAR:  return "instance variable";
  case SHK_GVAR:  return "global variable";
  case SHK_CVAR:  return "class variable";
  case SHK_CONST: return "constant";
  default:        return "value";
  }
}

/* What a flow kind (ShareFlowKind) hands its value to, for a message. */
static const char *repr_flow_kind_name(int kind) {
  switch (kind) {
  case SHFL_WRITE:  return "a variable's write";
  case SHFL_MEMBER: return "an attribute or member store";
  case SHFL_ARG:    return "a method's argument";
  case SHFL_ELEM:   return "a container's element";
  case SHFL_BLOCK:  return "a block's value a container keeps";
  case SHFL_YIELD:  return "a yield's argument";
  case SHFL_PARAM:  return "a block's parameter";
  case SHFL_LEND:   return "a lent argument";
  case SHFL_MULTI:  return "a multiple write's target";
  case SHFL_MUTATE: return "an in-place change";
  default:          return "a value";
  }
}

/* The route value node v is, for a message: a call's value by its name, a
   variable by its name, else the kind of construct. */
static void repr_flow_route_name(const Compiler *c, int v, char *out, size_t cap) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, v);
  const char *nm = nt_str(nt, v, "name");
  if (k == NK_CallNode && nm) { snprintf(out, cap, "the value of `%s`", nm); return; }
  if (holder_kind_of(k) != HK_NONE && nm) {
    snprintf(out, cap, "%s `%s`", repr_static_read_kind(k) || k == NK_LocalVariableReadNode ||
             k == NK_InstanceVariableReadNode ? "the variable" : "the write of", nm);
    return;
  }
  switch (k) {
  case NK_BeginNode:  snprintf(out, cap, "a begin's value"); return;
  case NK_IfNode: case NK_UnlessNode: case NK_CaseNode: case NK_AndNode: case NK_OrNode:
                      snprintf(out, cap, "a conditional's value"); return;
  case NK_YieldNode:  snprintf(out, cap, "a yield's value"); return;
  case NK_SuperNode: case NK_ForwardingSuperNode:
                      snprintf(out, cap, "super's value"); return;
  case NK_WhileNode: case NK_UntilNode:
                      snprintf(out, cap, "a loop's value"); return;
  default:            snprintf(out, cap, "this value"); return;
  }
}

/* Does flow value node v need the route check: a String (or a box that may
   hold one) whose class the rule shares? */
static int repr_flow_checked(Compiler *c, int v) {
  v = unwrap_parens(c, v);
  if (v < 0) return 0;
  NodeKind k = nt_kind(c->nt, v);
  if (k == NK_NilNode || k == NK_StringNode || k == NK_InterpolatedStringNode) return 0;
  TyKind t = c->ntype[v];
  if (t != TY_STRING && t != TY_STRBUF && t != TY_POLY) return 0;
  /* a container literal is no String */
  if (k == NK_ArrayNode || k == NK_HashNode || k == NK_RangeNode) return 0;
  return share_node_shares(c, v);
}

/* The route half of the seal: every flow the walk recorded into a class
   the rule shares (share_flow_at) has to go along a route codegen hands
   the shared handle on (strbuf_flow_carries), or the program is refused,
   naming the route, rather than compiled handing on a copy. A flow kind
   or a route codegen does not know is refused too. */
static void repr_share_flows_check(Compiler *c, const char *stats) {
  int nf = share_flow_count(c);
  int bad = -1, bad_kind = 0;
  StrbufFlowMemo fm;
  fm.yield_ok = malloc((size_t)(c->nscopes > 0 ? c->nscopes : 1));
  if (!fm.yield_ok) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  memset(fm.yield_ok, -1, (size_t)(c->nscopes > 0 ? c->nscopes : 1));
  for (int i = 0; i < nf; i++) {
    int site, v;
    int kind = share_flow_at(c, i, &site, &v);
    /* the String a flow stores that must be the handle where it is emitted
       (share_value_needs_handle), and whether its destination shares */
    if (stats && stats[0] == '2' && v >= 0 && share_value_needs_handle(c, v)) {
      const char *fp = nt_file_path(c->nt, (int)nt_int(c->nt, v, "node_file", -1));
      fprintf(stderr, "share-flow-need: %s:%d node %d into %s dest-shares=%d needs-handle=%d\n", fp ? fp : "?",
              (int)nt_int(c->nt, v, "node_line", 0), v, repr_flow_kind_name(kind), share_flow_dest_shares(c, i),
              share_value_needs_handle(c, v));
    }
    /* a flow in an unreachable scope is never emitted (bad_lit's rule) */
    if (v < 0 || !comp_scope_of(c, v)->reachable || !repr_flow_checked(c, v)) continue;
    int u = unwrap_parens(c, v);
    int ok = strbuf_flow_carries(c, &fm, kind, site, v);
    if (stats && stats[0] == '2') {
      char rn[160];
      repr_flow_route_name(c, u, rn, sizeof rn);
      const char *fp = nt_file_path(c->nt, (int)nt_int(c->nt, u, "node_file", -1));
      fprintf(stderr, "share-flow: %s:%d node %d %s (%s) into %s carried=%d flags=%u\n", fp ? fp : "?",
              (int)nt_int(c->nt, u, "node_line", 0), u, rn, ty_name(c->ntype[u]), repr_flow_kind_name(kind), ok,
              share_node_flags(c, u));
    }
    if (!ok && bad < 0) { bad = u; bad_kind = kind; }
  }
  free(fm.yield_ok);
  if (bad >= 0) {
    int u = bad, kind = bad_kind;
    char rn[160];
    repr_flow_route_name(c, u, rn, sizeof rn);
    char msg[512];
    snprintf(msg, sizeof msg, "under --share-strings, the String %s hands on as %s is shared with another "
             "name and changed in place, and that route cannot carry the shared handle yet: it would hand "
             "on a copy (#6765)", rn, repr_flow_kind_name(kind));
    unsupported_feature(c, u, msg);
  }
}

/* The analysis is final: every holder the rule shares has to hold the
   handle now. One whose kind cannot carry it yet is refused, naming it,
   rather than compiled holding a copy. SPINEL_SHARE_STATS=1 reports what
   the rule decided. */
static void repr_share_seal(Compiler *c) {
  share_facts_build(c);
  int nh = share_holder_count(c);
  int bad = -1, bad_elems = 0;
  int n_str = 0, n_shared = 0, n_kind[SHK_UNKNOWN + 1] = {0}, n_param = 0, n_elems = 0;
  int n_route_only = 0, n_unknown = 0;
  const char *stats = getenv("SPINEL_SHARE_STATS");
  struct ShareFacts *closed = stats ? share_facts_build_closed(c) : NULL;
  for (int h = 0; h < nh; h++) {
    const ShareHolder *sh = share_holder(c, h);
    int ec = repr_share_elems_carried(c, sh);
    if (ec >= 0 && repr_str_elems_share(c, h)) {
      n_elems++;
      if (!ec && bad < 0) { bad = h; bad_elems = 1; }
    }
    /* does the holder hold the shared handle (a box holds what is stored,
       the handle included)? -1 for one that holds no String */
    Repr hr = repr_of_share_holder(c, sh);
    int carried = hr.kind == RK_BOXED || hr.share ? 1 : hr.ty == TY_STRING || hr.ty == TY_STRBUF ? 0 : -1;
    if (carried < 0) continue;
    n_str++;
    int shares = repr_str_shares(c, h);
    if (!shares) {
      /* a handle a route rule made that the rule does not ask for */
      if (carried && sh->kind != SHK_CVAR && sh->kind != SHK_CONST &&
          !(sh->kind == SHK_LOCAL && c->scopes[sh->scope].locals[sh->local].type == TY_POLY) &&
          !(sh->kind == SHK_IVAR && c->classes[sh->cid].ivar_types[comp_ivar_index(&c->classes[sh->cid], sh->name)] == TY_POLY) &&
          !(sh->kind == SHK_GVAR && carried == 1 && comp_gvar(c, sh->name[0] == '$' ? sh->name + 1 : sh->name)->type == TY_POLY))
        n_route_only++;
      continue;
    }
    n_shared++;
    n_kind[sh->kind]++;
    if (sh->kind == SHK_LOCAL && c->scopes[sh->scope].locals[sh->local].is_param) n_param++;
    if (closed && !share_closed_shares(closed, sh)) n_unknown++;
    if (!carried && bad < 0) bad = h;
  }
  /* a container literal no holder names, whose elements the rule shares
     only once the facts settle after the fixpoint, kept a typed String
     form: its elements would be copies. A literal in an unreachable
     scope is never emitted, so no call can observe its copies. */
  int bad_lit = -1;
  for (int n = 0; n < c->nt->count && bad_lit < 0; n++) {
    NodeKind k = nt_kind(c->nt, n);
    if (k != NK_ArrayNode && k != NK_HashNode) continue;
    if (!comp_scope_of(c, n)->reachable) continue;
    int ne = 0;
    nt_arr(c->nt, n, "elements", &ne);
    /* an empty one holds no String yet: what is stored later goes through
       the holder that keeps it; one nothing can reach again once its
       expression is done (`p [a, b]`) keeps no name for its copies */
    if (ne > 0 && repr_str_literal_shares(c, n))
      bad_lit = n;
  }
  if (stats && stats[0] == '3') share_dump_unknown_mutations(c);
  if (stats && stats[0] == '2')
    for (int h = 0; h < nh; h++) {
      const ShareHolder *sh = share_holder(c, h);
      const char *nm = sh->kind == SHK_LOCAL ? c->scopes[sh->scope].locals[sh->local].name : sh->name;
      fprintf(stderr, "share-holder: %s %s%s%s flags=%u holders=%d shares=%d elems-share=%d\n",
              repr_share_kind_name(sh->kind), nm ? nm : "?",
              sh->kind == SHK_LOCAL ? " in " : "",
              sh->kind == SHK_LOCAL ? (c->scopes[sh->scope].name ? c->scopes[sh->scope].name : "<top>") : "",
              share_class_flags(c, h), share_class_holders(c, h), repr_str_shares(c, h),
              repr_str_elems_share(c, h));
    }
  if (stats) {
    fprintf(stderr, "share-stats: string-holders=%d shared=%d local=%d (param=%d) ivar=%d gvar=%d "
            "cvar=%d const=%d containers=%d via-unknown=%d route-only=%d refused=%d borrows=%d\n",
            n_str, n_shared, n_kind[SHK_LOCAL], n_param, n_kind[SHK_IVAR], n_kind[SHK_GVAR],
            n_kind[SHK_CVAR], n_kind[SHK_CONST], n_elems, n_unknown, n_route_only, bad >= 0,
            c->share_borrows);
    share_facts_drop(closed);
  }
  /* a route master refuses, left to the rule: refused as master does
     unless the final facts share its String */
  share_routes_check(c);
  if (bad < 0 && bad_lit >= 0)
    unsupported_feature(c, bad_lit, "under --share-strings, the Strings this literal holds are shared with "
                        "another name and changed in place, and a typed String container cannot hold the "
                        "shared handle yet (#6765)");
  if (bad >= 0) {
    const ShareHolder *sh = share_holder(c, bad);
    char nm[160];
    if (sh->kind == SHK_LOCAL) snprintf(nm, sizeof nm, "`%s`", c->scopes[sh->scope].locals[sh->local].name);
    else snprintf(nm, sizeof nm, "`%s`", sh->name ? sh->name : "?");
    const char *kind = sh->kind == SHK_LOCAL && c->scopes[sh->scope].locals[sh->local].is_block_param
                       ? "block parameter" : repr_share_kind_name(sh->kind);
    char msg[512];
    if (bad_elems)
      snprintf(msg, sizeof msg, "under --share-strings, the Strings %s %s holds are shared with another "
               "name and changed in place, and a typed String container cannot hold the shared handle "
               "yet (#6765)", kind, nm);
    else
      snprintf(msg, sizeof msg, "under --share-strings, the String %s %s holds is shared with another "
               "name and changed in place, and a %s cannot hold the shared handle yet (#6765)",
               kind, nm, kind);
    unsupported_feature(c, sh->node, msg);
  }
  repr_share_flows_check(c, stats);
}

/* ---- --dump-repr (#7501) ----
   One line per slot with the representation chosen for it, sorted, so the
   dumps two compilers give for one program can be diffed. A slot that
   became a shared handle or a box, or left a by-value layout, costs at run
   time without changing any output (#7482), and this is where it shows.
   Locals, parameters, globals and constants are LocalVars and read through
   repr_of_slot. An ivar, a class variable and a method's value are not:
   repr_of_ivar, repr_of_cvar and repr_of_ret read their own flags by the
   same rules. The dump only reads. It is taken once the
   analysis is final and printed once the compile has passed, so a program
   codegen refuses fails as a compile does; no C is written. */
int g_dump_repr = 0;

static const char *repr_kind_name(int k) {
  static const char *const names[] = {
    "none", "scalar", "opt", "struct", "vobj", "ptr", "strbuf", "boxed",
  };
  return k >= 0 && k <= RK_BOXED ? names[k] : "?";
}

/* an ivar's slot: an Integer or Float one keeps its nil in its nil byte
   (ivar_has_nilbit) when some write can leave nil in it, or when it can be
   read before any write (initialize does not write it, Class#allocate
   makes the object), which its reads box nil-aware (box_nullable_arg) */
Repr repr_of_ivar(const Compiler *c, int cid, int iv) {
  const ClassInfo *ci = &c->classes[cid];
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = ci->ivar_types[iv];
  r.narrowed = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_kind_of_type(c, r.ty);
  if (ivar_has_nilbit((Compiler *)c, cid, iv)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  if (repr_may_nil(r.ty, nil_fact_ivar(c, cid, ci->ivars[iv]))) r.may_nil = 1;
  if (r.ty == TY_STRBUF && ci->ivar_str_shared[iv]) r.handle = 1;
  r.share = r.handle && c->share_strings;
  r.elems_handle = ci->ivar_elems_shared[iv] && r.ty == TY_POLY_ARRAY;
  /* the element marking sits on the family's topmost class that has the
     ivar, where every subclass reads it (nullable_elem_ivar_in) */
  int ec = cid, ek = iv;
  for (int p = ci->parent; p >= 0 && p < c->nclasses; p = c->classes[p].parent) {
    int k = comp_ivar_index((ClassInfo *)&c->classes[p], ci->ivars[iv]);
    if (k < 0) break;
    ec = p; ek = k;
  }
  r.elem_nil_marked = (unsigned)repr_elem_nil(r.elem, c->classes[ec].ivar_nullable_int_elem[ek],
                                               iv < ci->n_ivar_obj_may_nil && ci->ivar_elem_may_nil &&
                                               ci->ivar_elem_may_nil[iv]);
  r.dyn_cls = repr_dyn_cls(c, r.ty);
  return r;
}

/* a class variable's slot, as an ivar's: an Integer or Float one some
   write leaves nil in is an oint (cvar_is_oint) */
ReprKind repr_cvar_kind(const Compiler *c, int cid, int idx) {
  const ClassInfo *ci = &c->classes[cid];
  TyKind t = ci->cvar_types[idx];
  if (cvar_is_oint((Compiler *)c, cid, idx)) return RK_OPT;
  return repr_kind_of_type(c, t);
}
/* Only the rule makes a class variable the shared handle
   (cvar_str_shared). A class variable has no nil fact (analyze_nil.c keeps
   one per ivar): a pointer one may be nil (repr_may_nil), as nil_fact_ivar
   answers for an ivar it has none for. */
Repr repr_of_cvar(const Compiler *c, int cid, int idx) {
  const ClassInfo *ci = &c->classes[cid];
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = ci->cvar_types[idx];
  r.narrowed = r.elem = r.key = r.val = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_cvar_kind(c, cid, idx);
  if (r.kind == RK_OPT) r.may_nil = r.nil_scalar = 1;
  if (repr_may_nil(r.ty, 1)) r.may_nil = 1;
  if (r.ty == TY_STRBUF && ci->cvar_str_shared[idx]) r.handle = 1;
  r.share = r.handle && c->share_strings;
  r.elems_handle = ci->cvar_elems_shared[idx] && r.ty == TY_POLY_ARRAY;
  r.dyn_cls = repr_dyn_cls(c, r.ty);
  return r;
}

/* a method's value: its nilable scalar (ret_nullable_int, or an RBS
   signature's `?`) is an oint */
Repr repr_of_ret(const Compiler *c, const Scope *sc) {
  Repr r;
  memset(&r, 0, sizeof r);
  r.ty = r.as_ty = sc->ret;
  r.narrowed = TY_UNKNOWN;
  repr_layout(&r, r.ty);
  r.kind = (unsigned char)repr_kind_of_type(c, r.ty);
  if ((r.ty == TY_INT || r.ty == TY_FLOAT) && (sc->ret_nullable_int || sc->ret_rbs_nilable)) {
    r.kind = RK_OPT;
    r.may_nil = r.nil_scalar = 1;
  }
  if (repr_may_nil(r.ty, sc->ret_obj_may_nil)) r.may_nil = 1;
  r.dyn_cls = repr_dyn_cls(c, r.ty);
  return r;
}

/* a type's name, with the class of a user object or of an object array */
static void repr_dump_ty(const Compiler *c, TyKind t, char *out, size_t n) {
  TyKind e = ty_is_obj_array(t) ? ty_array_elem(t) : t;
  if (ty_is_object(e) && ty_object_class(e) < c->nclasses)
    snprintf(out, n, "%s%s", ty_is_obj_array(t) ? "obj_array:" : "obj:",
             c->classes[ty_object_class(e)].name);
  else snprintf(out, n, "%s", ty_name(t));
}

typedef struct { char **v; int n, cap; } ReprLines;

/* where a local lives, as the dump prints it */
static const char *repr_cell_name(int cell) {
  switch ((ReprCell)cell) {
  case RC_NONE:  return "";
  case RC_HEAP:  return " cell=heap";
  case RC_BYREF: return " cell=byref";
  case RC_ALIAS: return " cell=alias";
  }
  return " cell=?";
}

static void repr_dump_line(const Compiler *c, ReprLines *ls, const char *where, Repr r) {
  char ty[256], line[1024];
  repr_dump_ty(c, r.ty, ty, sizeof ty);
  snprintf(line, sizeof line, "%s: %s ty=%s%s%s%s%s%s%s%s", where, repr_kind_name(r.kind), ty,
           r.handle ? " handle" : "", r.may_nil ? " may_nil" : "", r.dyn_cls ? " dyn_cls" : "",
           r.elem_nil_marked ? " elem_nil" : "", r.arr_or_nil ? " arr_or_nil" : "",
           repr_cell_name(r.cell), r.volatile_str ? " volatile" : "");
  if (ls->n == ls->cap) {
    ls->cap = ls->cap ? ls->cap * 2 : 64;
    ls->v = realloc(ls->v, (size_t)ls->cap * sizeof *ls->v);
  }
  ls->v[ls->n++] = strdup(line);
}

static int repr_line_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* A method's name as the dump prints it: Class#m, Class.m for a class
   method, <main> for the top level, and a mark for each copy of a method
   another scope also emits. */
static void repr_scope_label(const Compiler *c, const Scope *sc, char *out, size_t n) {
  const char *cls = sc->class_id >= 0 && sc->class_id < c->nclasses ? c->classes[sc->class_id].name : NULL;
  snprintf(out, n, "%s%s%s%s%s%s", cls ? cls : "", cls ? (sc->is_cmethod ? "." : "#") : "",
           sc->name ? sc->name : "<main>", sc->is_proc_form ? "(proc)" : "",
           sc->is_include_copy ? "(include)" : "", sc->is_extend_copy ? "(extend)" : "");
}

char *repr_dump(const Compiler *c) {
  ReprLines ls = {0};
  char where[1024], label[512];
  for (int si = 0; si < c->nscopes; si++) {
    const Scope *sc = &c->scopes[si];
    /* a method nothing calls is not run */
    if (sc->def_node >= 0 && !sc->reachable) continue;
    repr_scope_label(c, sc, label, sizeof label);
    for (int k = 0; k < sc->nlocals; k++) {
      const LocalVar *lv = &sc->locals[k];
      if (!lv->name) continue;
      snprintf(where, sizeof where, "%s %s %s", lv->is_param ? "param" : "local", label, lv->name);
      repr_dump_line(c, &ls, where, repr_of_slot(c, lv));
    }
    if (sc->def_node >= 0) {
      snprintf(where, sizeof where, "ret %s", label);
      repr_dump_line(c, &ls, where, repr_of_ret(c, sc));
    }
  }
  for (int cid = 0; cid < c->nclasses; cid++)
    for (int iv = 0; iv < c->classes[cid].nivars; iv++) {
      snprintf(where, sizeof where, "ivar %s %s", c->classes[cid].name, c->classes[cid].ivars[iv]);
      repr_dump_line(c, &ls, where, repr_of_ivar(c, cid, iv));
    }
  for (int cid = 0; cid < c->nclasses; cid++)
    for (int cv = 0; cv < c->classes[cid].ncvars; cv++) {
      snprintf(where, sizeof where, "cvar %s %s", c->classes[cid].name, c->classes[cid].cvars[cv]);
      repr_dump_line(c, &ls, where, repr_of_cvar(c, cid, cv));
    }
  for (int k = 0; k < c->ngvars; k++) {
    snprintf(where, sizeof where, "gvar $%s", c->gvars[k].name);
    repr_dump_line(c, &ls, where, repr_of_slot(c, &c->gvars[k]));
  }
  for (int k = 0; k < c->nconsts; k++) {
    snprintf(where, sizeof where, "const %s", c->consts[k].name);
    repr_dump_line(c, &ls, where, repr_of_slot(c, &c->consts[k]));
  }
  qsort(ls.v, (size_t)ls.n, sizeof *ls.v, repr_line_cmp);
  size_t len = 1;
  for (int k = 0; k < ls.n; k++) len += strlen(ls.v[k]) + 1;
  char *out = malloc(len), *o = out;
  for (int k = 0; k < ls.n; k++) {
    size_t n = strlen(ls.v[k]);
    memcpy(o, ls.v[k], n);
    o[n] = '\n';
    o += n + 1;
    free(ls.v[k]);
  }
  *o = 0;
  free(ls.v);
  return out;
}

int repr_share_rule(const Compiler *c) { return c->share_strings; }
