#include "analyze_internal.h"

/* Per-iteration memo for the (cid, ivname) full-table-scan narrow helpers
   below. Each is O(nodes); they are queried once per `@ivar[i]` expression,
   so on a large input (the self-compile) the naive form is O(nodes^2). The
   result is stable within one fixpoint iteration, so a generation-stamped
   direct-mapped cache collapses the repeats to O(nodes) per (cid, ivar).
   The generation is bumped once per fixpoint iteration (sp_narrow_memo_bump),
   so a stale entry can only survive within an iteration the fixpoint will
   re-run anyway. */
#define SP_NMEMO_SZ 16384
static unsigned g_narrow_gen = 1;
static struct { unsigned gen; long key; signed char val; } g_nmemo[SP_NMEMO_SZ];
void sp_narrow_memo_bump(void) { g_narrow_gen++; }
static long narrow_key(int which, int cid, const char *ivname) {
  unsigned long h = 1469598103934665603UL ^ (unsigned)which;
  h = (h * 1099511628211UL) ^ (unsigned)cid;
  for (const char *p = ivname; p && *p; p++) h = (h * 1099511628211UL) ^ (unsigned char)*p;
  return (long)(h & 0x7fffffffffffffffUL);
}
static int narrow_memo_get(long key, int *hit) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  if (g_nmemo[slot].gen == g_narrow_gen && g_nmemo[slot].key == key) { *hit = 1; return g_nmemo[slot].val; }
  *hit = 0; return 0;
}
static void narrow_memo_put(long key, int val) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  g_nmemo[slot].gen = g_narrow_gen; g_nmemo[slot].key = key; g_nmemo[slot].val = (signed char)val;
}

/* Whether every element written into the poly-array ivar `@<ivname>` of class
   `cid` is an int-returning kind: a bound method (a dispatch-table entry, called
   with an int arg returns int), an int array, an int, or nil filler. Mirrors
   legacy's cls_ivar_observed_types check in poly_index_narrow_int. */
/* Classify a value type stored as an element of a dispatch/data table:
   1 = int-returning when indexed/called (int bit, int array get, bound
   method/proc call, or a poly assumed to be such a callable); 2 = neutral
   filler (nil/unknown); 0 = a clearly non-int kind. */
static int table_elem_int_returning(TyKind vt) {
  if (vt == TY_INT || vt == TY_INT_ARRAY || vt == TY_METHOD || vt == TY_PROC ||
      vt == TY_POLY)
    return 1;
  if (vt == TY_NIL || vt == TY_UNKNOWN) return 2;
  return 0;
}

static int ivar_array_elems_int_returning_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* element write `@ivar[i] = v` */
    if (!strcmp(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || strcmp(nm, "[]=")) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0) continue;
      const char *rty = nt_type(nt, recv);
      if (!rty || strcmp(rty, "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || strcmp(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      int k = table_elem_int_returning(comp_ntype(c, av[1]));
      if (k == 0) return 0;
      if (k == 1) saw = 1;
      continue;
    }
    /* whole-ivar write `@ivar = [..]` / `@ivar = [x] * n` */
    if (!strcmp(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || strcmp(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;   /* the ArrayNode whose elements to inspect */
      if (vty && !strcmp(vty, "ArrayNode")) arr = v;
      else if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
               !strcmp(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");   /* `[..] * n` */
        if (ar >= 0 && nt_type(nt, ar) && !strcmp(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      if (arr < 0) return 0;   /* non-literal whole write: can't verify */
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        int k = table_elem_int_returning(comp_ntype(c, els[e]));
        if (k == 0) return 0;
        if (k == 1) saw = 1;
      }
      continue;
    }
  }
  return saw;
}

/* `@table[i][j]` where @table is a poly array of int-returning callables/arrays
   (a method dispatch table) yields an int. Returns 1 if `id` matches that shape
   for the enclosing class. The index types don't matter (they may themselves be
   poly mid-fixpoint); the result type follows from the table's element kinds. */
static int ivar_array_elems_int_returning(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(0, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_int_returning_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

static int poly_double_index_int(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || strcmp(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && !strcmp(aty, "RangeNode")) return 0;   /* slice, not an element */
  int inner = nt_ref(nt, id, "receiver");
  if (inner < 0) return 0;
  const char *ity = nt_type(nt, inner);
  if (!ity || strcmp(ity, "CallNode")) return 0;
  const char *inm = nt_str(nt, inner, "name");
  if (!inm || strcmp(inm, "[]")) return 0;
  int iargs = nt_ref(nt, inner, "arguments");
  int ian = 0;
  const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &ian) : NULL;
  if (ian != 1) return 0;
  (void)iav;
  int ivnode = nt_ref(nt, inner, "receiver");
  if (ivnode < 0) return 0;
  const char *vty = nt_type(nt, ivnode);
  if (!vty || strcmp(vty, "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, ivnode, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_int_returning(c, cid, ivname);
}

/* Whether every element stored into poly-array ivar `@<ivname>` is an int
   array (a nested array of int arrays, e.g. @chr_banks / @nmt_mem). Element
   reads then yield an int array rather than a boxed poly. */
static int ivar_array_elems_all_int_array_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (!strcmp(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || strcmp(nm, "[]=")) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || strcmp(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || strcmp(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (!strcmp(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || strcmp(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;
      if (vty && !strcmp(vty, "ArrayNode")) arr = v;
      else if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
               !strcmp(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");
        if (ar >= 0 && nt_type(nt, ar) && !strcmp(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      else if (vty && !strcmp(vty, "CallNode") && nt_str(nt, v, "name") &&
               (!strcmp(nt_str(nt, v, "name"), "map") || !strcmp(nt_str(nt, v, "name"), "collect"))) {
        /* `@x = src.map { ... }`: elements are the block's result type. */
        int blk = nt_ref(nt, v, "block");
        int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn <= 0) return 0;
        TyKind et = comp_ntype(c, bb[bn - 1]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      if (arr < 0) return 0;
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        TyKind et = comp_ntype(c, els[e]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      continue;
    }
  }
  return saw;
}

/* `@nested[i]` (single index) where @nested is a poly array whose every
   element is an int array yields an int array (not a boxed poly). Returns the
   ivar name via *out_iv for codegen, or NULL. */
static int ivar_array_elems_all_int_array(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(1, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_all_int_array_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

static int poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || strcmp(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && !strcmp(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || strcmp(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, recv, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_all_int_array(c, cid, ivname);
}

TyKind infer_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) return TY_UNKNOWN;

  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;

  /* `@table[i][j]` on a poly-array dispatch table of int-returning entries
     yields an int (a method/peek table). Resolves the optcarrot CPU's
     fetch/peek/store, which would otherwise run boxed-poly. */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1 && poly_double_index_int(c, id))
    return TY_INT;
  /* `@nested[i]` on a poly array of int arrays yields an int array (nested
     array of int arrays -- @chr_banks / @nmt_mem). Without this the element
     read is a boxed poly and cascades poly through the PPU. */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1 && poly_index_int_array(c, id))
    return TY_INT_ARRAY;

  /* Complex / Rational value types. */
  if (recv < 0 && !strcmp(name, "Complex")) return TY_COMPLEX;
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    if (rrty && !strcmp(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        !strcmp(nt_str(nt, recv, "name"), "Complex") && !strcmp(name, "polar"))
      return TY_COMPLEX;
  }
  if (rt == TY_COMPLEX) {
    if (!strcmp(name, "real") || !strcmp(name, "imaginary") || !strcmp(name, "imag") ||
        !strcmp(name, "abs") || !strcmp(name, "magnitude")) return TY_FLOAT;
    if (!strcmp(name, "conjugate") || !strcmp(name, "conj") ||
        !strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*")) return TY_COMPLEX;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
  }
  /* Proc#curry and curry application via []. */
  if (rt == TY_PROC && !strcmp(name, "curry")) return TY_CURRY;
  if (rt == TY_CURRY && (!strcmp(name, "[]") || !strcmp(name, "call") || !strcmp(name, "()")))
    return TY_CURRY;

  if (rt == TY_INT && !strcmp(name, "quo")) return TY_RATIONAL;
  if (rt == TY_RATIONAL) {
    if (!strcmp(name, "numerator") || !strcmp(name, "denominator")) return TY_INT;
    if (!strcmp(name, "to_f")) return TY_FLOAT;
    if (!strcmp(name, "to_i")) return TY_INT;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
  }

  /* Safe navigation &. : nil receiver always short-circuits to nil */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && !strcmp(call_op, "&.") && rt == TY_NIL) return TY_NIL;
  }

  /* nil receiver: type inference for NilClass methods */
  if (recv >= 0 && rt == TY_NIL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
    if (!strcmp(name, "nil?") || !strcmp(name, "is_a?") || !strcmp(name, "kind_of?") ||
        !strcmp(name, "instance_of?")) return TY_BOOL;
    if (!strcmp(name, "to_i") || !strcmp(name, "to_int")) return TY_INT;
    if (!strcmp(name, "to_f") || !strcmp(name, "to_r")) return TY_FLOAT;
    if (!strcmp(name, "to_a")) return TY_POLY_ARRAY;
    if (!strcmp(name, "to_h")) return TY_SYM_POLY_HASH;
    if (!strcmp(name, "respond_to?")) return TY_BOOL;
  }

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last/min/max/pop/shift/sample of an empty array returns 0
           (the typed slot's zero value); carry it as an int */
        if ((!strcmp(name, "first") || !strcmp(name, "last") ||
             !strcmp(name, "min") || !strcmp(name, "max") ||
             !strcmp(name, "sample") ||
             !strcmp(name, "pop") || !strcmp(name, "shift")) && argc == 0) return TY_INT;
        rt = TY_POLY_ARRAY;
      }
    }
  }

  /* `<&block-param>.call(...)` inside a yielding method: the explicit-call form
     of yield. Its value is the call-site block's value (resolved like yield). */
  {
    int emi = (int)(comp_scope_of(c, id) - c->scopes);
    if (emi > 0 && is_blk_param_call(c, id, emi)) return yield_value_type(c, emi);
  }

  /* __dir__ -> the source directory (a string) */
  if (recv < 0 && !strcmp(name, "__dir__") && argc == 0) return TY_STRING;

  /* Kernel#caller -> the call stack as strings (empty in release builds) */
  if (recv < 0 && !strcmp(name, "caller") && argc <= 2) return TY_STR_ARRAY;

  /* bare `name` inside a class method body -> the class name string */
  if (recv < 0 && !strcmp(name, "name") && argc == 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }
  /* `self.name` / `self.to_s` inside a class method -> the class name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }

  /* loop { break val } -> the type of the break value */
  if (recv < 0 && !strcmp(name, "loop")) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        TyKind bt = scan_break_type(c, body, 0);
        if (bt != TY_UNKNOWN) return bt;
      }
    }
    return TY_NIL;
  }

  /* catch(:tag) { ... [throw :tag, val] ... } -> unify the block's last
     value with every throw value targeting the tag */
  if (recv < 0 && !strcmp(name, "catch")) {
    int blk = nt_ref(nt, id, "block");
    TyKind result = TY_UNKNOWN;
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
        if (bn > 0) result = infer_type(c, bb[bn - 1]);
        TyKind tt = scan_throw_type(c, body, 0);
        if (tt != TY_UNKNOWN) result = ty_unify(result, tt);
      }
    }
    return result == TY_UNKNOWN ? TY_NIL : result;
  }

  /* recv.instance_eval/exec { ... } -> the block's last-expression type
     (bare calls inside resolve via the ie node->class map). A trampoline
     method `recv.M { ... }` resolves the same way. */
  int ie_kind = (recv >= 0 && (!strcmp(name, "instance_eval") || !strcmp(name, "instance_exec")) &&
                 ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (!ie_kind && recv >= 0 && ty_is_object(rt) && nt_ref(nt, id, "block") >= 0)
    ie_kind = comp_trampoline_kind(c, ty_object_class(rt), name, NULL) != 0;
  /* receiverless instance_eval/exec inside an instance method resolves to self */
  if (!ie_kind && recv < 0 && ie_implicit_self_class(c, id) >= 0) ie_kind = 1;
  if (ie_kind) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      const char *bty = nt_type(nt, blk);
      if (bty && !strcmp(bty, "BlockArgumentNode")) {
        /* `instance_exec(args, &b)` forwards the enclosing method's block; the
           value it produces is that method's own forwarded-block value across
           call sites (the method inlines per site, splicing the literal). */
        Scope *encl = comp_scope_of(c, id);
        int emi = encl ? (int)(encl - c->scopes) : -1;
        if (emi >= 0) {
          TyKind ft = yield_value_type(c, emi);
          if (ft != TY_UNKNOWN && ft != TY_VOID) return ft;
        }
        /* The forwarded block's value isn't statically pinned here (its call
           sites may not be typed yet during the fixpoint). It is a real boxed
           value, not nil -- poly keeps the result a scalar carrier so the
           enclosing method stays inlinable and the splice yields it per site. */
        return TY_POLY;
      }
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); return bt == TY_VOID ? TY_NIL : bt; }
      return TY_NIL;
    }
  }

  /* method(:sym) / <recv>.method(:sym) -> a bound Method object */
  if (name && !strcmp(name, "method") && method_sym_arg(c, id) != NULL) return TY_METHOD;

  /* <method>.call(args) / [] -> the target method's return type. */
  if (recv >= 0 && rt == TY_METHOD &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]"))) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (mi >= 0) return c->scopes[mi].ret == TY_UNKNOWN ? TY_INT : c->scopes[mi].ret;
    return TY_INT;  /* bound-method ABI returns mrb_int */
  }
  /* <method>.name -> the method name string; .arity -> int */
  if (recv >= 0 && rt == TY_METHOD && argc == 0) {
    if (!strcmp(name, "name")) return TY_STRING;
    if (!strcmp(name, "arity")) return TY_INT;
  }
  /* <poly>.call(args): a boxed Proc/Method called through the runtime ABI,
     which returns mrb_int. (Skip when a user class defines `call`: that goes
     through normal dispatch and returns the method's own type.) */
  if (recv >= 0 && rt == TY_POLY &&
      (!strcmp(name, "call") || !strcmp(name, "()"))) {
    int has_user_call = 0;
    for (int k = 0; k < c->nclasses && !has_user_call; k++)
      if (comp_method_in_class(c, k, "call") >= 0) has_user_call = 1;
    if (!has_user_call) return TY_INT;
  }

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) return TY_PROC;

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type */
  if (recv >= 0 && rt == TY_PROC &&
      (!strcmp(name, "call") || !strcmp(name, "()") || !strcmp(name, "[]")))
    return proc_call_ret(c, recv);

  /* Proc composition: proc << proc / proc >> proc -> a new Proc. */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (!strcmp(name, "<<") || !strcmp(name, ">>")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_PROC;

  /* Proc introspection */
  if (recv >= 0 && rt == TY_PROC && argc == 0) {
    if (!strcmp(name, "arity")) return TY_INT;
    if (!strcmp(name, "lambda?")) return TY_BOOL;
    if (!strcmp(name, "parameters")) return TY_POLY_ARRAY;
  }

  /* TY_CLASS method dispatch -- .new on a dynamic class variable returns TY_POLY.
     Exception: self.class.new(...) resolves to the enclosing class statically. */
  if (recv >= 0 && rt == TY_CLASS && !strcmp(name, "new") &&
      nt_type(nt, recv) && strcmp(nt_type(nt, recv), "ConstantReadNode") != 0 &&
      strcmp(nt_type(nt, recv), "ConstantPathNode") != 0) {
    int _is_self_class = (nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class") &&
      nt_ref(nt, recv, "receiver") >= 0 &&
      nt_type(nt, nt_ref(nt, recv, "receiver")) &&
      !strcmp(nt_type(nt, nt_ref(nt, recv, "receiver")), "SelfNode"));
    if (!_is_self_class) return TY_POLY;
  }

  if (recv >= 0 && rt == TY_CLASS && strcmp(name, "new") != 0) {
    if (argc == 0 && (!strcmp(name, "to_s") || !strcmp(name, "name") || !strcmp(name, "inspect")))
      return TY_STRING;
    if (argc == 0 && !strcmp(name, "nil?")) return TY_BOOL;
    if (argc == 0 && !strcmp(name, "class")) return TY_STRING;
    if (argc == 0 && !strcmp(name, "superclass")) return TY_CLASS;
    if (argc == 1 && (!strcmp(name, "==") || !strcmp(name, "eql?") || !strcmp(name, "!="))) return TY_BOOL;
    if (argc == 1 && (!strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") || !strcmp(name, ">="))) return TY_BOOL;
    if (argc == 0 && !strcmp(name, "ancestors")) return TY_POLY_ARRAY;
    if (argc == 1 && (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?"))) return TY_BOOL;
    if (argc <= 1 && !strcmp(name, "instance_methods")) return TY_POLY;
  }

  /* __method__ / __callee__ -> the enclosing method's name (a symbol) */
  if (recv < 0 && argc == 0 &&
      (!strcmp(name, "__method__") || !strcmp(name, "__callee__")))
    return TY_SYMBOL;

  /* identity methods: return the receiver unchanged */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "freeze") || !strcmp(name, "itself") ||
       !strcmp(name, "dup") || !strcmp(name, "clone")))
    return rt;

  /* x.class -> a class-name string (for known builtin receivers) or TY_CLASS (user objects) */
  if (recv >= 0 && argc == 0 && !strcmp(name, "class")) {
    if (ty_is_object(rt)) return TY_CLASS;  /* user object: return sp_Class value */
    if (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
        rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL || rt == TY_POLY ||
        rt == TY_METHOD || rt == TY_PROC || rt == TY_IO || rt == TY_ARGF ||
        ty_is_array(rt) || ty_is_hash(rt))
      return TY_STRING;
  }

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (!strcmp(name, "name") || !strcmp(name, "to_s")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class"))
    return TY_STRING;

  /* __ENCODING__.name / .to_s / .inspect -> the encoding name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SourceEncodingNode"))
    return TY_STRING;
  /* <enc>.encoding.name -> the encoding name string */
  if (recv >= 0 && argc == 0 && !strcmp(name, "name") && rt == TY_POLY &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* Module.singleton_writer= / Module.singleton_reader */
  if (recv >= 0 && nt_type(nt, recv) &&
      (!strcmp(nt_type(nt, recv), "ConstantReadNode") ||
       !strcmp(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) {
      ClassInfo *cls = &c->classes[ci];
      int nlen = (int)strlen(name);
      /* setter: name ends with '=' */
      if (nlen > 1 && name[nlen - 1] == '=') {
        char base[256]; int blen = nlen - 1;
        if (blen > 0 && blen < (int)sizeof(base)) {
          memcpy(base, name, (size_t)blen); base[blen] = '\0';
          if (comp_is_sg_writer(cls, base)) return TY_VOID;
        }
      }
else {
        if (comp_is_sg_reader(cls, name)) return TY_POLY;
      }
    }
  }
  /* self.singleton_writer= / self.singleton_reader: inside a class method
     or directly in a class/module body (g_cbody_class_id). */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "SelfNode")) {
    Scope *_self = comp_scope_of(c, id);
    int _sg_cid = (_self && _self->is_cmethod && _self->class_id >= 0)
                  ? _self->class_id : g_cbody_class_id;
    if (_sg_cid >= 0) {
      ClassInfo *_cls = &c->classes[_sg_cid];
      int _nlen = (int)strlen(name);
      if (_nlen > 1 && name[_nlen - 1] == '=') {
        char _base[256]; int _blen = _nlen - 1;
        if (_blen > 0 && _blen < (int)sizeof(_base)) {
          memcpy(_base, name, (size_t)_blen); _base[_blen] = '\0';
          if (comp_is_sg_writer(_cls, _base)) return TY_VOID;
        }
      }
      else if (comp_is_sg_reader(_cls, name)) return TY_POLY;
    }
  }

  /* FFI: call on a module that registered ffi_func/ffi_buffer/ffi_read_* */
  if (recv >= 0 && nt_type(nt, recv)) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (!strcmp(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (!strcmp(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      int fi = ffi_find_func(c, rcmod, name);
      if (fi >= 0) return ffi_spec_to_ty(c->ffi_func_ret[fi]);
      /* ffi_buffer: Module.buf_name returns the static char* (ptr type -> TY_POLY) */
      if (ffi_find_buf(c, rcmod, name) >= 0) return TY_POLY;
      /* ffi_read_*: Module.reader_name(buf) returns int or ptr */
      int ri = ffi_find_reader(c, rcmod, name);
      if (ri >= 0) {
        const char *kind = c->ffi_reader_kinds[ri];
        if (kind && !strcmp(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
    }
  }

  /* SomeClass.name / .to_s / .inspect -> class name string */
  if (recv >= 0 && argc == 0 &&
      (!strcmp(name, "name") || !strcmp(name, "to_s") || !strcmp(name, "inspect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_STRING;
  /* SomeClass.superclass -> sp_Class value for the parent class */
  if (recv >= 0 && argc == 0 && !strcmp(name, "superclass") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_CLASS;

  /* SomeClass.ancestors -> PolyArray of class objects */
  if (recv >= 0 && argc == 0 && !strcmp(name, "ancestors") &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_POLY_ARRAY;

  /* SomeClass.instance_methods / .public_instance_methods -> PolyArray of symbols */
  if (recv >= 0 && argc <= 1 &&
      (!strcmp(name, "instance_methods") || !strcmp(name, "public_instance_methods") ||
       !strcmp(name, "private_instance_methods") || !strcmp(name, "protected_instance_methods")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode"))
    return TY_POLY_ARRAY;

  /* self.class.new(...) -> an instance of the enclosing class */
  if (recv >= 0 && !strcmp(name, "new") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0) return ty_object(self->class_id);
  }

  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && !strcmp(name, "new")) {
    const char *rty = nt_type(nt, recv);
    /* a namespaced class (M::Sub) or root-qualified builtin (::Array etc) */
    if (rty && !strcmp(rty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && !strcmp(cn, "Port")) {   /* Ractor::Port.new -> shared channel */
        int par = nt_ref(nt, recv, "parent");
        const char *pn = par >= 0 ? nt_str(nt, par, "name") : NULL;
        if (pn && !strcmp(pn, "Ractor")) return TY_RACTOR_PORT;
      }
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        if (class_inherits_builtin_exception(c, ci)) return TY_EXCEPTION;
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      /* ::Array.new / ::String.new / ::StringIO.new etc. */
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "Array")) return TY_POLY_ARRAY;
      if (cn && !strcmp(cn, "Object")) return TY_POLY;
      if (cn && !strcmp(cn, "String")) return TY_STRING;
      if (cn && !strcmp(cn, "StringIO")) return TY_STRINGIO;
      if (cn && !strcmp(cn, "StringScanner")) return TY_STRINGSCANNER;
      if (cn && !strcmp(cn, "Hash")) return TY_UNKNOWN;
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
      if (cn && !strcmp(cn, "Fiber")) return TY_FIBER;
      if (cn && !strcmp(cn, "Ractor")) return TY_RACTOR;
      if (cn && (!strcmp(cn, "Thread") || !strcmp(cn, "Mutex") || !strcmp(cn, "Monitor") ||
                 !strcmp(cn, "Random") || !strcmp(cn, "IO") || !strcmp(cn, "File") ||
                 !strcmp(cn, "GzipReader") || !strcmp(cn, "GzipWriter"))) return TY_POLY;
    }
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = comp_class_index(c, cn);
      if (ci >= 0) {
        if (class_inherits_builtin_exception(c, ci)) return TY_EXCEPTION;
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      if (cn && !strcmp(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && !strcmp(cn, "Array")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* Array.new(n) { body }: element type from last expression of block body */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          if (bn > 0 && bb) { TyKind et = infer_type(c, bb[bn - 1]); if (et != TY_UNKNOWN) return ty_array_of(et); }
        }
        /* a bare `Array.new` carries no element type; leave it UNKNOWN (like an
           empty `[]`) so the push-promotion pass can narrow it from `<<`/push. */
        if (argc == 0 && blk < 0) return TY_UNKNOWN;
        return TY_POLY_ARRAY;
      }
      if (cn && !strcmp(cn, "Array")) return TY_POLY_ARRAY; /* Array.new / Array.new(n) */
      if (cn && !strcmp(cn, "Object")) return TY_POLY;  /* identity sentinel */
      if (cn && !strcmp(cn, "String")) return TY_STRING;
      if (cn && !strcmp(cn, "StringIO")) return TY_STRINGIO;
      if (cn && !strcmp(cn, "StringScanner")) return TY_STRINGSCANNER;
      /* Hash.new { |hash, key| default } : a string-keyed poly hash with a
         default-proc (the block computes the missing-key value). */
      if (cn && !strcmp(cn, "Hash") && nt_ref(nt, id, "block") >= 0) return TY_STR_POLY_HASH;
      if (cn && !strcmp(cn, "Hash")) return TY_UNKNOWN; /* hash type determined by key usage */
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
      /* Builtin object types */
      if (cn && !strcmp(cn, "Fiber")) return TY_FIBER;
      if (cn && !strcmp(cn, "Ractor")) return TY_RACTOR;
      /* Thread.new { block }: modeled as a Fiber run to completion on #value. */
      if (cn && !strcmp(cn, "Thread") && nt_ref(nt, id, "block") >= 0) return TY_FIBER;
      if (cn && !strcmp(cn, "Random")) return TY_RANDOM;
      if (cn && (!strcmp(cn, "Thread") || !strcmp(cn, "Mutex") || !strcmp(cn, "Monitor") ||
                 !strcmp(cn, "IO") || !strcmp(cn, "File") ||
                 !strcmp(cn, "GzipReader") || !strcmp(cn, "GzipWriter"))) return TY_POLY;
    }
  }

  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && !strcmp(name, "compile")) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && !strcmp(cn, "Regexp")) return TY_REGEX;
    }
  }

  /* StringScanner instance methods */
  if (recv >= 0 && rt == TY_STRINGSCANNER) {
    if (!strcmp(name, "scan") || !strcmp(name, "check") || !strcmp(name, "scan_until") ||
        !strcmp(name, "matched") || !strcmp(name, "pre_match") || !strcmp(name, "post_match") ||
        !strcmp(name, "rest") || !strcmp(name, "string") || !strcmp(name, "getch") ||
        !strcmp(name, "peek") || !strcmp(name, "[]")) return TY_STRING;  /* nullable via NULL */
    if (!strcmp(name, "matched?") || !strcmp(name, "eos?") || !strcmp(name, "rest?")) return TY_BOOL;
    if (!strcmp(name, "pos") || !strcmp(name, "charpos") || !strcmp(name, "rest_size")) return TY_INT;
    if (!strcmp(name, "reset") || !strcmp(name, "terminate") || !strcmp(name, "unscan")) return TY_STRINGSCANNER;
  }

  /* Regexp class methods */
  if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Regexp")) {
    if ((!strcmp(name, "escape") || !strcmp(name, "quote")) && argc >= 1) return TY_STRING;
    if (!strcmp(name, "union") && argc >= 1) return TY_REGEX;
    if (!strcmp(name, "last_match") && argc == 0) return TY_POLY;
    if (!strcmp(name, "last_match") && argc == 1) return TY_STRING;
  }

  /* Regexp instance methods */
  if (recv >= 0 && rt == TY_REGEX) {
    if (!strcmp(name, "match?") || !strcmp(name, "===")) return TY_BOOL;
    if (!strcmp(name, "match")) return TY_MATCHDATA;
    if (!strcmp(name, "=~")) return TY_POLY;
    if (!strcmp(name, "source") || !strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "freeze") || !strcmp(name, "dup") || !strcmp(name, "clone")) return TY_REGEX;
  }

  /* MatchData instance methods */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    if (!strcmp(name, "[]") && argc == 1) return TY_STRING;
    if (!strcmp(name, "pre_match") || !strcmp(name, "post_match") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "begin") || !strcmp(name, "end") || !strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
    if (!strcmp(name, "offset")) return TY_INT_ARRAY;
    if (!strcmp(name, "captures") || !strcmp(name, "to_a") || !strcmp(name, "named_captures")) return TY_POLY_ARRAY;
    if (!strcmp(name, "nil?")) return TY_BOOL;
  }

  /* StringIO.open(args) { |io| body } -> the block body's value */
  if (recv >= 0 && !strcmp(name, "open") && nt_type(nt, recv) &&
      !strcmp(nt_type(nt, recv), "ConstantReadNode") && nt_str(nt, recv, "name") &&
      !strcmp(nt_str(nt, recv, "name"), "StringIO")) {
    int block = nt_ref(nt, id, "block");
    int bbody = block >= 0 ? nt_ref(nt, block, "body") : -1;
    if (bbody >= 0) {
      int bn = 0; const int *bb = nt_arr(nt, bbody, "body", &bn);
      return bn > 0 ? infer_type(c, bb[bn - 1]) : TY_NIL;
    }
    return TY_STRINGIO;
  }

  /* StringIO instance methods */
  if (recv >= 0 && rt == TY_STRINGIO) {
    if (!strcmp(name, "string") || !strcmp(name, "read")) return TY_STRING;
    if (!strcmp(name, "gets") || !strcmp(name, "getc")) return TY_POLY;  /* nil at eof */
    if (!strcmp(name, "eof?") || !strcmp(name, "eof") || !strcmp(name, "closed?") ||
        !strcmp(name, "sync") || !strcmp(name, "isatty") || !strcmp(name, "tty?")) return TY_BOOL;
    if (!strcmp(name, "flush")) return TY_STRINGIO;
    /* pos/tell/size/length/lineno/write/puts/print/putc/getbyte/seek/... */
    return TY_INT;
  }

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Time") &&
        (!strcmp(name, "now") || !strcmp(name, "at") || !strcmp(name, "local") ||
         !strcmp(name, "mktime") || !strcmp(name, "utc") || !strcmp(name, "gm") ||
         !strcmp(name, "new")))
      return TY_TIME;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC") &&
        (!strcmp(name, "start") || !strcmp(name, "compact")))
      return TY_NIL;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "GC") &&
        !strcmp(name, "stat"))
      return TY_STR_INT_HASH;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Process")) {
      if (!strcmp(name, "pid") || !strcmp(name, "ppid")) return TY_INT;
      if (!strcmp(name, "clock_gettime")) return TY_FLOAT;
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Integer") &&
        !strcmp(name, "sqrt"))
      return TY_INT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Math") &&
        (!strcmp(name, "sin") || !strcmp(name, "cos") || !strcmp(name, "tan") ||
         !strcmp(name, "asin") || !strcmp(name, "acos") || !strcmp(name, "atan") ||
         !strcmp(name, "atan2") || !strcmp(name, "sinh") || !strcmp(name, "cosh") ||
         !strcmp(name, "tanh") || !strcmp(name, "asinh") || !strcmp(name, "acosh") ||
         !strcmp(name, "atanh") || !strcmp(name, "exp") || !strcmp(name, "log") ||
         !strcmp(name, "log2") || !strcmp(name, "log10") || !strcmp(name, "sqrt") ||
         !strcmp(name, "cbrt") || !strcmp(name, "hypot") || !strcmp(name, "frexp") ||
         !strcmp(name, "ldexp") || !strcmp(name, "erf") || !strcmp(name, "erfc")))
      return TY_FLOAT;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "JSON") &&
        (!strcmp(name, "generate") || !strcmp(name, "dump")))
      return TY_STRING;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir") &&
        (!strcmp(name, "exist?") || !strcmp(name, "exists?")))
      return TY_BOOL;
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "Dir")) {
      if (!strcmp(name, "pwd") || !strcmp(name, "home")) return TY_STRING;
      if (!strcmp(name, "glob")) return TY_STR_ARRAY;
      if (!strcmp(name, "mkdir") || !strcmp(name, "rmdir") || !strcmp(name, "chdir"))
        return TY_INT;
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "File")) {
      if (!strcmp(name, "basename") || !strcmp(name, "dirname") || !strcmp(name, "extname") ||
          !strcmp(name, "read") || !strcmp(name, "binread") || !strcmp(name, "expand_path") ||
          !strcmp(name, "join"))
        return TY_STRING;
      if (!strcmp(name, "exist?") || !strcmp(name, "exists?"))
        return TY_BOOL;
      if (!strcmp(name, "write") || !strcmp(name, "binwrite") || !strcmp(name, "delete"))
        return TY_INT;
      if (!strcmp(name, "readable?") || !strcmp(name, "directory?") || !strcmp(name, "file?") ||
          !strcmp(name, "zero?") || !strcmp(name, "empty?"))
        return TY_BOOL;
      if (!strcmp(name, "mtime"))
        return TY_TIME;
      if (!strcmp(name, "readlines")) return TY_STR_ARRAY;
      /* File.open / File.new without a block -> a typed IO handle */
      if (!strcmp(name, "open") || !strcmp(name, "new")) {
        int blk = nt_ref(nt, id, "block");
        if (blk < 0) return TY_IO;
        /* Pin block param to TY_IO so body dispatch works (f.write, f.puts, etc.) */
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_IO;
        return TY_POLY;
      }
    }
    if (rty && !strcmp(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "IO")) {
      /* IO.pipe -> [r, w] pair; each is TY_POLY; the pair is a str_array */
      if (!strcmp(name, "pipe")) return TY_STR_ARRAY;
    }
    /* Fiber.new {} / Thread.new {} / Fiber.current etc.
       Handles both bare Const and ::Const path forms. */
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      const char *cn2 = nt_str(nt, recv, "name");
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Fiber")) return TY_FIBER;
      /* Thread.new { block } modeled as a Fiber (single-threaded); #value
         resumes it to completion. */
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Thread") &&
          nt_ref(nt, id, "block") >= 0)
        return TY_FIBER;
      if (cn2 && !strcmp(name, "new") && !strcmp(cn2, "Random")) return TY_RANDOM;
      if (cn2 && !strcmp(name, "new") &&
          (!strcmp(cn2, "Thread") || !strcmp(cn2, "Mutex")))
        return TY_POLY;
      if (cn2 && !strcmp(cn2, "Fiber") && !strcmp(name, "current")) return TY_FIBER;
      if (cn2 && !strcmp(cn2, "Fiber") && !strcmp(name, "yield")) return TY_POLY;
      /* Ractor class methods: Ractor.new -> ractor, Ractor.receive -> poly
         (a message of statically-unknown type), Ractor.yield -> nil. */
      if (cn2 && !strcmp(cn2, "Ractor") && !strcmp(name, "new")) return TY_RACTOR;
      if (cn2 && !strcmp(cn2, "Ractor") && !strcmp(name, "receive")) return TY_POLY;
      if (cn2 && !strcmp(cn2, "Ractor") && !strcmp(name, "yield")) return TY_NIL;
      /* Ractor.shareable?(x) -> bool; Ractor.make_shareable(x) deep-freezes x
         and returns it (pass-through type: a frozen value keeps its type). */
      if (cn2 && !strcmp(cn2, "Ractor") && !strcmp(name, "shareable?")) return TY_BOOL;
      if (cn2 && !strcmp(cn2, "Ractor") && !strcmp(name, "make_shareable"))
        return argc >= 1 ? infer_type(c, argv[0]) : TY_POLY;
      /* Random class methods: Random.rand(n)->int / Random.rand->float */
      if (cn2 && !strcmp(cn2, "Random") && !strcmp(name, "rand"))
        return argc >= 1 ? TY_INT : TY_FLOAT;
      if (cn2 && !strcmp(cn2, "Random") && !strcmp(name, "bytes")) return TY_STRING;
    }
  }

  /* TY_FIBER instance methods */
  if (recv >= 0 && rt == TY_FIBER) {
    if (!strcmp(name, "resume") || !strcmp(name, "transfer")) return TY_POLY;
    if (!strcmp(name, "alive?")) return TY_BOOL;
    if (!strcmp(name, "value")) return TY_POLY;
  }

  /* TY_RACTOR instance methods: r.send(v) / r << v return the Ractor (like
     Ruby's #send), r.take returns a message of statically-unknown type. */
  if (recv >= 0 && rt == TY_RACTOR) {
    if (!strcmp(name, "send") || !strcmp(name, "<<")) return TY_RACTOR;
    if (!strcmp(name, "take")) return TY_POLY;
  }

  /* TY_RACTOR_PORT instance methods: send/<< return the port, receive a
     message of unknown type, close nil. */
  if (recv >= 0 && rt == TY_RACTOR_PORT) {
    if (!strcmp(name, "send") || !strcmp(name, "<<")) return TY_RACTOR_PORT;
    if (!strcmp(name, "receive")) return TY_POLY;
    if (!strcmp(name, "close")) return TY_POLY;
  }

  /* TY_RANDOM instance methods */
  if (recv >= 0 && rt == TY_RANDOM) {
    if (!strcmp(name, "rand")) return argc >= 1 ? TY_INT : TY_FLOAT;
    if (!strcmp(name, "bytes")) return TY_STRING;
    if (!strcmp(name, "seed")) return TY_INT;
  }

  /* ARGF pseudo-IO methods */
  if (recv >= 0 && rt == TY_ARGF) {
    if (!strcmp(name, "read") || !strcmp(name, "gets") || !strcmp(name, "readline") ||
        !strcmp(name, "filename") || !strcmp(name, "path") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "readlines") || !strcmp(name, "to_a")) return TY_STR_ARRAY;
    if (!strcmp(name, "eof?") || !strcmp(name, "eof")) return TY_BOOL;
    if (!strcmp(name, "each_line") || !strcmp(name, "each_string") || !strcmp(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      return TY_ARGF;
    }
  }

  /* TY_IO (File/IO handle) instance methods */
  if (recv >= 0 && rt == TY_IO) {
    if (!strcmp(name, "read") || !strcmp(name, "gets") || !strcmp(name, "readline") ||
        !strcmp(name, "path") || !strcmp(name, "to_path")) return TY_STRING;
    if (!strcmp(name, "read") && nt_ref(nt, id, "arguments") >= 0) return TY_STRING;
    if (!strcmp(name, "readlines")) return TY_STR_ARRAY;
    if (!strcmp(name, "write") || !strcmp(name, "syswrite") || !strcmp(name, "pos") ||
        !strcmp(name, "tell") || !strcmp(name, "seek") || !strcmp(name, "rewind") ||
        !strcmp(name, "close")) return TY_INT;
    if (!strcmp(name, "print") || !strcmp(name, "puts") || !strcmp(name, "flush")) return TY_NIL;
    if (!strcmp(name, "closed?") || !strcmp(name, "eof?") || !strcmp(name, "eof")) return TY_BOOL;
    if (!strcmp(name, "each_line") || !strcmp(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      return TY_IO;
    }
    return TY_POLY;
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (!strcmp(name, "-") && argc > 0) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_TIME) return TY_FLOAT;
    }
    if (!strcmp(name, "utc") || !strcmp(name, "gmtime") || !strcmp(name, "getutc") ||
        !strcmp(name, "localtime") || !strcmp(name, "getlocal") || !strcmp(name, "+") ||
        !strcmp(name, "-")) return TY_TIME;
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect") || !strcmp(name, "strftime") ||
        !strcmp(name, "iso8601") || !strcmp(name, "zone") || !strcmp(name, "asctime") ||
        !strcmp(name, "ctime")) return TY_STRING;
    if (!strcmp(name, "to_f") || !strcmp(name, "subsec")) return TY_FLOAT;
    if (!strcmp(name, "utc?") || !strcmp(name, "gmt?") || !strcmp(name, "dst?") ||
        !strcmp(name, "isdst") ||
        !strcmp(name, "sunday?") || !strcmp(name, "monday?") ||
        !strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") ||
        !strcmp(name, ">=") || !strcmp(name, "==") || !strcmp(name, "!=")) return TY_BOOL;
    if (!strcmp(name, "<=>")) return TY_INT;
    if (!strcmp(name, "class")) return TY_STRING;
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    return TY_INT;
  }

  /* `Module.accessor.cmethod(...)` where the singleton accessor statically
     folds to a constant (Stage-1): dispatch as that constant's class method. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int mi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
      if (mi >= 0) return c->scopes[mi].ret;
    }
    /* Stage-2: accessor holds one of several constants; unify their cmethod returns. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      TyKind r = TY_UNKNOWN;
      for (int k = 0; k < ncand; k++) {
        int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
        if (mi >= 0) r = ty_unify(r, c->scopes[mi].ret);
      }
      if (r != TY_UNKNOWN) return r;
    }
  }

  /* Class.cmethod(...) / M::Sub.cmethod(...) -> the class method's return type */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) return c->scopes[mi].ret;
      }
    }
    /* obj.class.cmeth(...) -> unify class method return types across hierarchy */
    if (rty && !strcmp(rty, "CallNode") &&
        nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "class")) {
      int robj = nt_ref(nt, recv, "receiver");
      TyKind rrt = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
      if (ty_is_object(rrt)) {
        int cid = ty_object_class(rrt);
        int mi = comp_cmethod_in_chain(c, cid, name, NULL);
        if (mi >= 0) {
          TyKind r = (TyKind)c->scopes[mi].ret;
          for (int k = 0; k < c->nclasses; k++) {
            int _desc = 0;
            for (int _p = c->classes[k].parent; _p >= 0; _p = c->classes[_p].parent)
              if (_p == cid) { _desc = 1; break; }
            if (!_desc) continue;
            int kmi = comp_cmethod_in_class(c, k, name);
            if (kmi >= 0) r = ty_unify(r, (TyKind)c->scopes[kmi].ret);
          }
          return r;
        }
      }
    }
  }

  /* Struct instance methods */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    if (!strcmp(name, "to_a") || !strcmp(name, "values") ||
        !strcmp(name, "deconstruct") || !strcmp(name, "members")) return TY_POLY_ARRAY;
    if (!strcmp(name, "to_h")) {
      int block = nt_ref(nt, id, "block");
      if (block >= 0) {
        /* to_h { |k,v| [nk, nv] }: hash type from the block's pair */
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        if (last >= 0 && nt_type(nt, last) && !strcmp(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, els[0]), vt = infer_type(c, els[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING && vt == TY_STRING) return TY_STR_STR_HASH;
            if (kt == TY_STRING) return TY_STR_POLY_HASH;
            TyKind h = ty_hash_of(kt, vt);
            return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
    if (!strcmp(name, "dig") && argc >= 1) {
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) {
        TyKind mt = sc->ivar_types[mi];
        if (argc == 1) return mt;
        /* dig(member, key, ...): index into the member's container */
        if (ty_is_hash(mt) && argc == 2) return ty_hash_val(mt);
        if (ty_is_array(mt) && argc == 2) return ty_array_elem(mt);
        return TY_POLY;
      }
    }
    if (!strcmp(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int]: return specific member type if known */
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) return sc->ivar_types[mi];
      /* integer index: try to resolve literal */
      const char *kty = nt_type(nt, argv[0]);
      if (kty && !strcmp(kty, "IntegerNode")) {
        long long idx = (long long)nt_int(nt, argv[0], "value", 0);
        if (idx < 0) idx += (long long)sc->nivars;
        if (idx >= 0 && idx < sc->nivars) return sc->ivar_types[(int)idx];
      }
      return TY_POLY;
    }
    if (!strcmp(name, "[]=") && argc == 2) return sc->nivars > 0 ? sc->ivar_types[0] : TY_POLY;
  }

  /* built-in class reopening: look up user-defined methods on scalar built-in types */
  if (recv >= 0) {
    const char *oc_cn = NULL;
    if (rt == TY_STRING)       oc_cn = "String";
    else if (rt == TY_INT)     oc_cn = "Integer";
    else if (rt == TY_FLOAT)   oc_cn = "Float";
    else if (rt == TY_SYMBOL)  oc_cn = "Symbol";
    else if (rt == TY_BOOL)    oc_cn = "TrueClass";
    if (oc_cn) {
      int oc_ci = comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return method_call_ret(c, oc_mi, id);
      }
    }
  }

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    if (!strcmp(name, "is_a?") || !strcmp(name, "kind_of?") || !strcmp(name, "instance_of?") ||
        !strcmp(name, "respond_to?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "nil?") || !strcmp(name, "equal?") || !strcmp(name, "frozen?")) return TY_BOOL;
    /* attr reader (resolve alias so `alias v access_token` returns @access_token type) */
    { int rdcls = -1;
      if (comp_reader_in_chain(c, cid, name, &rdcls)) {
        const char *rname = comp_resolve_alias(c, cid, name);
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", rname);
        ClassInfo *rci = (rdcls >= 0 && rdcls < c->nclasses) ? &c->classes[rdcls] : cls;
        int iv = comp_ivar_index(rci, ivn);
        if (iv >= 0) return rci->ivar_types[iv];
      }
    }
    /* attr writer: obj.x= returns the assigned value */
    size_t ln = strlen(name);
    if (ln >= 2 && name[ln - 1] == '=') {
      char base[256];
      if (ln - 1 < sizeof base) {
        memcpy(base, name, ln - 1); base[ln - 1] = '\0';
        if (comp_writer_in_chain(c, cid, base, NULL) && argc >= 1) return infer_type(c, argv[0]);
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      TyKind r = method_call_ret(c, mi, id);
      /* Unify with descendant direct overrides: codegen dispatch emits a
         cls_id switch over all overrides, so the result type must cover all. */
      for (int k = 0; k < c->nclasses; k++) {
        int is_desc = 0;
        for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
          if (p == cid) { is_desc = 1; break; }
        if (!is_desc) continue;
        int dmi = comp_method_in_class(c, k, name);
        if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
      }
      return r;
    }
    if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      { int rdcls2 = -1;
        if (comp_reader_in_chain(c, self->class_id, name, &rdcls2)) {
          const char *rname2 = comp_resolve_alias(c, self->class_id, name);
          char ivn[256];
          snprintf(ivn, sizeof ivn, "@%s", rname2);
          ClassInfo *rci2 = (rdcls2 >= 0 && rdcls2 < c->nclasses) ? &c->classes[rdcls2] : &c->classes[self->class_id];
          int iv = comp_ivar_index(rci2, ivn);
          if (iv >= 0) return rci2->ivar_types[iv];
        }
      }
      /* bare `new` inside a class method returns an instance of self's class */
      if (self->is_cmethod && !strcmp(name, "new"))
        return ty_object(self->class_id);
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi < 0 && self->is_cmethod)
        mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        TyKind r = method_call_ret(c, mi, id);
        /* Unify with descendant direct overrides: codegen dispatch will
           emit a cls_id switch over all overrides, so the return type
           must accommodate every override's return type. */
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name) :
                                       comp_method_in_class(c, k, name);
          if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
        }
        return r;
      }
      /* Built-in class reopening: implicit self → delegate to built-in type lookup */
      if (mi < 0 && !self->is_cmethod) {
        const char *bcn = c->classes[self->class_id].name;
        TyKind brt = TY_UNKNOWN;
        if (!strcmp(bcn, "String"))        brt = TY_STRING;
        else if (!strcmp(bcn, "Integer"))  brt = TY_INT;
        else if (!strcmp(bcn, "Float"))    brt = TY_FLOAT;
        else if (!strcmp(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          /* Temporarily set rt to the built-in type and recursively call infer_call
             is not safe. Instead inline key return types for common method names. */
          if (brt == TY_STRING) {
            if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
                !strcmp(name, "capitalize") || !strcmp(name, "reverse") || !strcmp(name, "strip") ||
                !strcmp(name, "lstrip") || !strcmp(name, "rstrip") || !strcmp(name, "chomp") ||
                !strcmp(name, "chop") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
                !strcmp(name, "to_s") || !strcmp(name, "inspect") || !strcmp(name, "succ") ||
                !strcmp(name, "next") || !strcmp(name, "chr") || !strcmp(name, "encode") ||
                !strcmp(name, "b") || !strcmp(name, "force_encoding") || !strcmp(name, "scrub") ||
                !strcmp(name, "squeeze") || !strcmp(name, "tr") || !strcmp(name, "delete"))
              return TY_STRING;
            if ((!strcmp(name, "+") || !strcmp(name, "*")) && argc >= 1) return TY_STRING;
            if (!strcmp(name, "gsub") || !strcmp(name, "sub")) return TY_STRING;
            if (!strcmp(name, "[]") || !strcmp(name, "slice")) return TY_STRING;
            if (!strcmp(name, "length") || !strcmp(name, "size") || !strcmp(name, "bytesize") ||
                !strcmp(name, "to_i") || !strcmp(name, "count") || !strcmp(name, "ord") ||
                !strcmp(name, "hex") || !strcmp(name, "oct") || !strcmp(name, "rindex") ||
                !strcmp(name, "index"))
              return TY_INT;
            if (!strcmp(name, "to_f")) return TY_FLOAT;
            if (!strcmp(name, "to_sym")) return TY_SYMBOL;
            if (!strcmp(name, "empty?") || !strcmp(name, "include?") ||
                !strcmp(name, "start_with?") || !strcmp(name, "end_with?") ||
                !strcmp(name, "==") || !strcmp(name, "!="))
              return TY_BOOL;
            if (!strcmp(name, "split") || !strcmp(name, "chars") || !strcmp(name, "lines") ||
                !strcmp(name, "bytes"))
              return TY_STR_ARRAY;
          }
          else if (brt == TY_INT) {
            if (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
                !strcmp(name, "/") || !strcmp(name, "%") || !strcmp(name, "**") ||
                !strcmp(name, "abs") || !strcmp(name, "succ") || !strcmp(name, "next") ||
                !strcmp(name, "pred") || !strcmp(name, "gcd") || !strcmp(name, "lcm") ||
                !strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
                !strcmp(name, "<<") || !strcmp(name, ">>"))
              return TY_INT;
            if (!strcmp(name, "to_f")) return TY_FLOAT;
            if (!strcmp(name, "to_s")) return TY_STRING;
            if (!strcmp(name, "to_r")) return TY_POLY;
            if (!strcmp(name, "odd?") || !strcmp(name, "even?") || !strcmp(name, "zero?") ||
                !strcmp(name, "==") || !strcmp(name, "!=") || !strcmp(name, "<") ||
                !strcmp(name, "<=") || !strcmp(name, ">") || !strcmp(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_FLOAT) {
            if (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
                !strcmp(name, "/") || !strcmp(name, "**") || !strcmp(name, "abs") ||
                !strcmp(name, "floor") || !strcmp(name, "ceil") || !strcmp(name, "round") ||
                !strcmp(name, "truncate"))
              return TY_FLOAT;
            if (!strcmp(name, "to_i")) return TY_INT;
            if (!strcmp(name, "to_s")) return TY_STRING;
            if (!strcmp(name, "zero?") || !strcmp(name, "nan?") || !strcmp(name, "infinite?") ||
                !strcmp(name, "finite?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
                !strcmp(name, "<") || !strcmp(name, "<=") || !strcmp(name, ">") ||
                !strcmp(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_SYMBOL) {
            if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "inspect"))
              return TY_STRING;
            if (!strcmp(name, "to_sym") || !strcmp(name, "itself")) return TY_SYMBOL;
            if (!strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
            if (!strcmp(name, "empty?") || !strcmp(name, "==") || !strcmp(name, "!="))
              return TY_BOOL;
          }
        }
      }
      /* Method defined only in descendants (not in base chain):
         unify return types of all descendant implementations. */
      if (self->is_cmethod) {
        TyKind r = TY_UNKNOWN; int found = 0;
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = comp_cmethod_in_class(c, k, name);
          if (dmi < 0) continue;
          r = found ? ty_unify(r, (TyKind)c->scopes[dmi].ret) : (TyKind)c->scopes[dmi].ret;
          found = 1;
        }
        if (found) return r;
      }
    }
  }

  /* bare call inside a module/class body -> class method of that module/class.
     Use the per-node enclosing-cbody: g_cbody_class_id is only set during the
     scope pass, not the inference fixpoint, so relying on it leaves a bare
     module-body cmethod call (e.g. `take(mk)` where mk is `def self.mk`) typed
     void. The scope pass records the enclosing cbody per node in node_cbody[id]
     (cf. analyze_scope.c, analyze.c which already read it during inference). */
  if (recv < 0) {
    int cbody = c->node_cbody[id];
    if (cbody < 0) cbody = g_cbody_class_id;
    if (cbody >= 0) {
      int smi = comp_cmethod_in_chain(c, cbody, name, NULL);
      if (smi >= 0) return method_call_ret(c, smi, id);
    }
  }
  /* bare call inside an instance_eval/exec block: dispatch on receiver class */
  if (recv < 0) {
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) return method_call_ret(c, imi, id);
    }
  }
  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi < 0) mi = comp_included_method_index(c, name);
    if (mi >= 0) return method_call_ret(c, mi, id);
    /* Kernel conversions */
    if (!strcmp(name, "Integer") && (argc == 1 || argc == 2)) return TY_INT;
    if (!strcmp(name, "Float") && argc == 1) return TY_FLOAT;
    if (!strcmp(name, "String") && argc == 1) return TY_STRING;
    if ((!strcmp(name, "format") || !strcmp(name, "sprintf")) && argc >= 1) return TY_STRING;
    if (!strcmp(name, "system") && argc >= 1) return TY_BOOL;
    if (!strcmp(name, "trap") && argc >= 1) return TY_STRING;
    if (!strcmp(name, "rand")) {
      if (argc == 0) return TY_FLOAT;
      /* rand(float_range) → Float */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && !strcmp(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;
      }
      return TY_INT;
    }
    if (!strcmp(name, "srand")) return TY_INT;
  }
  /* Signal.trap / ::Signal.trap */
  if (recv >= 0 && !strcmp(name, "trap") && argc >= 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && (!strcmp(rty, "ConstantReadNode") || !strcmp(rty, "ConstantPathNode"))) {
      const char *rname = nt_str(nt, recv, "name");
      if (rname && !strcmp(rname, "Signal")) return TY_STRING;
    }
  }

  /* Fiber storage: Fiber[:k] and Fiber.current[:k] -> poly */
  if (recv >= 0 && !strcmp(name, "[]") && argc == 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "Fiber")) return TY_POLY;
    }
    if (rty && !strcmp(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && !strcmp(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber"))
          return TY_POLY;
      }
    }
  }
  /* Fiber[:k] = v -> returns v's type */
  if (recv >= 0 && !strcmp(name, "[]=") && argc == 2) {
    const char *rty = nt_type(nt, recv);
    int is_fiber = 0;
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "Fiber")) is_fiber = 1;
    }
    else if (rty && !strcmp(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && !strcmp(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && !strcmp(rrty, "ConstantReadNode") && rrn && !strcmp(rrn, "Fiber"))
          is_fiber = 1;
      }
    }
    if (is_fiber) return infer_type(c, argv[1]);
  }
  /* ENV[key] -> string or nil (use TY_STRING; null means nil) */
  if (recv >= 0 && argc >= 1 && (!strcmp(name, "[]") || !strcmp(name, "fetch"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && !strcmp(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && !strcmp(rn, "ENV")) return TY_STRING;
    }
  }

  /* each_slice(n).map/collect { |...| } chain: return array of block result type */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_es = nt_ref(nt, id, "block");
    if (blk_es >= 0) {
      int body_es = nt_ref(nt, blk_es, "body");
      int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
      return ty_array_of(bn_es > 0 ? infer_type(c, bb_es[bn_es - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).map/collect { |...| } chain: return array of block result type */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_ec = nt_ref(nt, id, "block");
    if (blk_ec >= 0) {
      int body_ec = nt_ref(nt, blk_ec, "body");
      int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
      return ty_array_of(bn_ec > 0 ? infer_type(c, bb_ec[bn_ec - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).with_index(off).map/collect { |...| } chain */
  if (recv >= 0 && rt == TY_UNKNOWN && (!strcmp(name, "map") || !strcmp(name, "collect")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && !strcmp(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && !strcmp(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int blk_wi = nt_ref(nt, id, "block");
      if (blk_wi >= 0) {
        int body_wi = nt_ref(nt, blk_wi, "body");
        int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
        return ty_array_of(bn_wi > 0 ? infer_type(c, bb_wi[bn_wi - 1]) : TY_UNKNOWN);
      }
    }
  }

  /* array receiver methods */
  if (recv >= 0 && ty_is_array(rt)) {
    int block = nt_ref(nt, id, "block");
    if (block >= 0) {
      if (!strcmp(name, "map") || !strcmp(name, "collect")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
      }
      if (!strcmp(name, "flat_map") || !strcmp(name, "collect_concat")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind bret = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        /* block returns an array -> flatten one level keeps its element type;
           a scalar block return behaves like map (each wrapped element). */
        return ty_is_array(bret) ? bret : ty_array_of(bret);
      }
      if (!strcmp(name, "select") || !strcmp(name, "reject") ||
          !strcmp(name, "filter") || !strcmp(name, "sort_by") ||
          !strcmp(name, "take_while") || !strcmp(name, "drop_while"))
        return rt;
      if (!strcmp(name, "max_by") || !strcmp(name, "min_by") ||
          !strcmp(name, "find") || !strcmp(name, "detect"))
        return ty_array_elem(rt);  /* returns an element */
      if (!strcmp(name, "partition")) return TY_POLY_ARRAY;  /* [[truthy...],[falsy...]] */
    }
    /* grep/grep_v without a block filter by `pattern === e`, preserving the
       receiver's array type. */
    if ((!strcmp(name, "grep") || !strcmp(name, "grep_v")) &&
        nt_ref(nt, id, "block") < 0 && argc == 1)
      return rt;
    if (!strcmp(name, "[]")) {
      /* arr[range] / arr[start, len] -> a subarray; arr[i] -> an element */
      if (argc == 2) return rt;
      if (argc == 1 && nt_type(nt, argv[0]) && !strcmp(nt_type(nt, argv[0]), "RangeNode")) return rt;
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "at") && argc == 1) return ty_array_elem(rt);  /* like [i] */
    if (!strcmp(name, "fetch") && (argc == 1 || argc == 2)) return ty_array_elem(rt);
    if (!strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_array_elem(rt);
      return TY_POLY;
    }
    /* index returns nil on a miss -> poly (int-or-nil) */
    if ((!strcmp(name, "index") || !strcmp(name, "find_index") || !strcmp(name, "rindex")) &&
        (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count") || !strcmp(name, "index") || !strcmp(name, "find_index")) return TY_INT;
    if (!strcmp(name, "sum")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return bn > 0 ? infer_type(c, bb[bn - 1]) : ty_array_elem(rt);
      }
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "inject") || !strcmp(name, "reduce")) {
      /* inject(&:&|:||:-) over a literal array of int arrays: set operation
         folding the inner arrays -> an int array. */
      if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
        int blk = nt_ref(nt, id, "block");
        const char *sop = NULL;
        if (blk >= 0 && nt_type(nt, blk) && !strcmp(nt_type(nt, blk), "BlockArgumentNode")) {
          int ex = nt_ref(nt, blk, "expression");
          if (ex >= 0 && nt_type(nt, ex) && !strcmp(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
        }
        if (sop && (!strcmp(sop, "&") || !strcmp(sop, "|") || !strcmp(sop, "-"))) return TY_INT_ARRAY;
      }
      /* When an init argument is provided, the return type matches the init type.
         inject(:op) is the no-init operator form — the sole symbol arg is the
         operator, NOT an init value, so skip the "return argv[0] type" path. */
      if (argc > 0 && argv) {
        const char *a0ty = nt_type(nt, argv[0]);
        int is_sym_op = a0ty && !strcmp(a0ty, "SymbolNode") && argc == 1;
        if (!is_sym_op) {
          TyKind it = infer_type(c, argv[0]);
          if (it != TY_UNKNOWN) {
            /* The accumulator is reassigned to the block body each iteration,
               so an int seed folded over floats accumulates float. */
            int rblk = nt_ref(nt, id, "block");
            int rbody = rblk >= 0 ? nt_ref(nt, rblk, "body") : -1;
            int rbn = 0; const int *rbb = rbody >= 0 ? nt_arr(nt, rbody, "body", &rbn) : NULL;
            if (rbn > 0) { TyKind bt = infer_type(c, rbb[rbn - 1]); if (ty_is_numeric(bt)) it = ty_promote_numeric(it, bt); }
            return it;
          }
        }
      }
      /* empty array literal `[]` with sym op: codegen treats as int_array → returns int */
      if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; nt_arr(nt, recv, "elements", &en);
        if (en == 0) return TY_INT;
      }
      /* Block body last expression determines the return type when available. */
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); if (bt != TY_UNKNOWN) return bt; }
      }
      return ty_array_elem(rt);
    }
    if (!strcmp(name, "each_with_object") && argc > 0 && argv) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) return TY_INT_ARRAY;
      }
      return at;
    }
    if (!strcmp(name, "tally") && argc == 0) {
      if (rt == TY_INT_ARRAY) return TY_INT_INT_HASH;
      if (rt == TY_STR_ARRAY) return TY_STR_INT_HASH;
      if (rt == TY_POLY_ARRAY) return TY_SYM_POLY_HASH;
    }
    if (!strcmp(name, "group_by") && block >= 0 && ty_is_array(rt))
      return TY_POLY_POLY_HASH;
    if ((!strcmp(name, "first") || !strcmp(name, "last")) && argc == 1) return rt;  /* first(n)/last(n) -> subarray */
    if ((!strcmp(name, "drop") || !strcmp(name, "take")) && argc == 1) return rt;  /* subarray */
    if (!strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "sample") ||
        !strcmp(name, "pop") || !strcmp(name, "shift")) return ty_array_elem(rt);
    if (!strcmp(name, "minmax")) return rt;  /* [min, max], same element kind */
    if (!strcmp(name, "join"))                        return TY_STRING;
    if (!strcmp(name, "pack") && argc == 1)           return TY_STRING;
    if (!strcmp(name, "inspect") || !strcmp(name, "to_s")) return TY_STRING;
    if (!strcmp(name, "empty?") || !strcmp(name, "include?")) return TY_BOOL;
    if ((!strcmp(name, "all?") || !strcmp(name, "any?") ||
         !strcmp(name, "none?") || !strcmp(name, "one?")) && argc == 0) return TY_BOOL;
    if ((!strcmp(name, "bsearch") || !strcmp(name, "find") || !strcmp(name, "detect")) && block >= 0)
      return ty_array_elem(rt);  /* element or nil */
    if ((!strcmp(name, "map!") || !strcmp(name, "collect!")) && block >= 0) {
      /* Typed arrays (int/str/float): in-place mutation preserves element type.
         The block param may be widened to TY_POLY when shared with other blocks,
         but the array type is determined by the receiver, not the block body. */
      if (ty_array_elem(rt) != TY_POLY)
        return rt;
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind bt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
      return bt != TY_UNKNOWN ? ty_array_of(bt) : rt;
    }
    if ((!strcmp(name, "select!") || !strcmp(name, "filter!") || !strcmp(name, "reject!") ||
         !strcmp(name, "keep_if") || !strcmp(name, "delete_if")) && block >= 0) return rt;
    if (!strcmp(name, "find_index") || !strcmp(name, "index")) return TY_INT;  /* int or nil */
    if (!strcmp(name, "each_index")) return rt;
    if ((!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append")) &&
        argc >= 1 && argv && rt != TY_POLY_ARRAY && ty_array_elem(rt) != TY_UNKNOWN) {
      /* Heterogeneous push on a typed-array literal: lift to poly. */
      TyKind elem_t = ty_array_elem(rt);
      const char *rty = nt_type(nt, recv);
      if (rty && !strcmp(rty, "ArrayNode")) {
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = infer_type(c, argv[ai]);
          if (at != TY_UNKNOWN && at != elem_t) return TY_POLY_ARRAY;
        }
      }
      return rt;
    }
    if (!strcmp(name, "push") || !strcmp(name, "<<") || !strcmp(name, "append") ||
        !strcmp(name, "reverse") || !strcmp(name, "sort") || !strcmp(name, "uniq") ||
        !strcmp(name, "to_a") || !strcmp(name, "dup") || !strcmp(name, "clone") ||
        !strcmp(name, "compact") || !strcmp(name, "compact!") || !strcmp(name, "flatten") || !strcmp(name, "clear") ||
        !strcmp(name, "transpose") ||
        !strcmp(name, "shuffle") ||
        (!strcmp(name, "union") && argc == 0) ||
        !strcmp(name, "reverse!") || !strcmp(name, "sort!") || !strcmp(name, "shuffle!") ||
        !strcmp(name, "uniq!") ||
        !strcmp(name, "rotate!") || !strcmp(name, "insert") || !strcmp(name, "freeze") ||
        (!strcmp(name, "fill") && argc >= 1 && argc <= 3) ||
        !strcmp(name, "replace") ||
        !strcmp(name, "values_at")) return rt;
    if (!strcmp(name, "zip") && block < 0) return TY_POLY_ARRAY;
    if (!strcmp(name, "product") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "repeated_combination") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "frozen?")) return TY_BOOL;
    if ((!strcmp(name, "delete_at") || !strcmp(name, "delete")) && argc == 1)
      return ty_array_elem(rt);
    if (!strcmp(name, "shift") && argc == 0) return ty_array_elem(rt);
    if (!strcmp(name, "slice!") && argc == 2) return rt;  /* removed subarray */
    if (!strcmp(name, "[]=") && argc == 2)            return ty_array_elem(rt);
    if (!strcmp(name, "[]=") && argc == 3)            return infer_type(c, argv[2]);
    if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && rt == TY_POLY_ARRAY)
      return TY_POLY_ARRAY;  /* the matching sub-array, or nil (NULL ptr) */
    if (!strcmp(name, "to_h") && argc == 0 && block < 0) {
      /* Infer hash type from the first pair element of an array literal */
      if (recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, recv, "elements", &en);
        if (en > 0 && nt_type(nt, els[0]) && !strcmp(nt_type(nt, els[0]), "ArrayNode")) {
          int en2 = 0; const int *els2 = nt_arr(nt, els[0], "elements", &en2);
          if (en2 >= 2) {
            TyKind kt = infer_type(c, els2[0]);
            TyKind vt = infer_type(c, els2[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING) {
              TyKind h = ty_hash_of(TY_STRING, vt);
              return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
            }
            TyKind h = ty_hash_of(kt, vt);
            if (h != TY_UNKNOWN) return h;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
  }

  /* exception receiver methods */
  /* A specialized rescue var is typed as the exception subclass object, but
     its exception-shaped queries still answer as on a base exception, unless
     the subclass defines its own override (#1415). */
  int exc_shaped = rt == TY_EXCEPTION ||
                   (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)) &&
                    comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (recv >= 0 && exc_shaped) {
    if (!strcmp(name, "message") || !strcmp(name, "to_s") ||
        !strcmp(name, "to_str") || !strcmp(name, "inspect") ||
        !strcmp(name, "full_message") || !strcmp(name, "class")) return TY_STRING;
    if (!strcmp(name, "backtrace")) return TY_STR_ARRAY;  /* empty: no frames captured */
  }

  /* poly receiver / poly operand: result type of operations on sp_RbVal */
  if (recv >= 0 && (rt == TY_POLY || a0 == TY_POLY)) {
    /* String operators with a poly operand are NOT poly arithmetic: `str % x`
       is printf formatting, `str + x` is concatenation, `str * n` is repeat --
       all yield a string. Defer them to the rt==TY_STRING path below. */
    if (!(rt == TY_STRING && (!strcmp(name, "%") || !strcmp(name, "+") || !strcmp(name, "*"))) &&
        (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") ||
         !strcmp(name, "/") || !strcmp(name, "%") || !strcmp(name, "**")))
      return TY_POLY;
    if (!strcmp(name, "<") || !strcmp(name, ">") || !strcmp(name, "<=") ||
        !strcmp(name, ">=") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "nil?") || !strcmp(name, "is_a?") || !strcmp(name, "kind_of?") ||
        !strcmp(name, "include?"))
      return TY_BOOL;
    if (rt == TY_POLY) {
      /* &. on a poly receiver may short-circuit to nil at runtime → always poly */
      {
        const char *call_op = nt_str(nt, id, "call_operator");
        if (recv >= 0 && call_op && !strcmp(call_op, "&.")) return TY_POLY;
      }
      if (!strcmp(name, "to_s") || !strcmp(name, "inspect")) return TY_STRING;
      if ((!strcmp(name, "gsub") || !strcmp(name, "sub")) && argc == 2) return TY_STRING;
      if (!strcmp(name, "join")) return TY_STRING;
      if (!strcmp(name, "to_i") || !strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
      if (!strcmp(name, "to_f")) return TY_FLOAT;
      if (!strcmp(name, "[]") && argc == 1) return TY_POLY;  /* boxed array element access */
      if (!strcmp(name, "[]") && argc == 2) return TY_POLY;  /* 2-arg poly slice */
      if (!strcmp(name, "dig") && argc >= 1) return TY_POLY;
      {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          TyKind et = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
          return et != TY_UNKNOWN ? ty_array_of(et) : TY_POLY_ARRAY;
        }
      }
      /* poly method dispatch: unify the return type over every class that
         defines `name` (the runtime cls_id picks the impl). */
      TyKind r = TY_UNKNOWN; int found = 0;
      for (int k = 0; k < c->nclasses; k++) {
        int mi = comp_method_in_chain(c, k, name, NULL);
        if (mi >= 0) { r = found ? ty_unify(r, c->scopes[mi].ret) : c->scopes[mi].ret; found = 1; continue; }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", name);
          int iv = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind rt2 = iv >= 0 ? c->classes[rdcls].ivar_types[iv] : TY_UNKNOWN;
          r = found ? ty_unify(r, rt2) : rt2; found = 1;
        }
      }
      if (found) return r;
      /* Fiber/Thread/IO/File instance methods: fallback when no user class defines `name`. */
      if (!strcmp(name, "resume") || !strcmp(name, "value") || !strcmp(name, "join"))
        return TY_POLY;
      if (!strcmp(name, "alive?") || !strcmp(name, "dead?") || !strcmp(name, "closed?") ||
          !strcmp(name, "eof?")) return TY_BOOL;
      if (!strcmp(name, "write") || !strcmp(name, "read") || !strcmp(name, "gets") ||
          !strcmp(name, "readline")) return TY_STRING;
      if (!strcmp(name, "close") || !strcmp(name, "flush")) return TY_NIL;
      if (!strcmp(name, "fileno")) return TY_INT;
      if (!strcmp(name, "synchronize")) {
        int blk_id = nt_ref(nt, id, "block");
        if (blk_id >= 0) {
          int bdy = nt_ref(nt, blk_id, "body");
          int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
          if (bbn > 0) return infer_type(c, bbb[bbn - 1]);
        }
        return TY_NIL;
      }
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (!strcmp(name, "to_s") || !strcmp(name, "id2name") || !strcmp(name, "name")) return TY_STRING;
    if (!strcmp(name, "inspect")) return TY_STRING;
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "swapcase") ||
        !strcmp(name, "to_sym") || !strcmp(name, "itself")) return TY_SYMBOL;
    if (!strcmp(name, "length") || !strcmp(name, "size")) return TY_INT;
    if (!strcmp(name, "empty?") || !strcmp(name, "==") || !strcmp(name, "!=")) return TY_BOOL;
  }

  /* range receiver methods */
  if (recv >= 0 && rt == TY_RANGE) {
    /* a literal string range ("a".."z") yields strings, not ints */
    if (!strcmp(name, "to_a")) {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && !strcmp(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && !strcmp(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 && infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING)
          return TY_STR_ARRAY;
      }
    }
    if (!strcmp(name, "to_a") || !strcmp(name, "minmax")) return TY_INT_ARRAY;
    if (!strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "cover?") || !strcmp(name, "exclude_end?") ||
        !strcmp(name, "eql?") || !strcmp(name, "==") || !strcmp(name, "!=") ||
        !strcmp(name, "overlap?")) return TY_BOOL;
    if (!strcmp(name, "step")) return TY_INT_ARRAY;
    if (!strcmp(name, "all?") || !strcmp(name, "any?") ||
        !strcmp(name, "none?") || !strcmp(name, "one?")) return TY_BOOL;
    if (!strcmp(name, "each") && nt_ref(nt, id, "block") < 0) return TY_INT_ARRAY;
    if ((!strcmp(name, "first") || !strcmp(name, "last")) && argc == 1) return TY_INT_ARRAY;
    if (!strcmp(name, "sum") || !strcmp(name, "min") || !strcmp(name, "max") ||
        !strcmp(name, "first") || !strcmp(name, "last") ||
        !strcmp(name, "size") || !strcmp(name, "count") ||
        !strcmp(name, "begin") || !strcmp(name, "end"))  return TY_INT;
    if (!strcmp(name, "bsearch")) return TY_INT;  /* a member, or nil (nullable int) */
    int block = nt_ref(nt, id, "block");
    if (block >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
    }
  }

  /* (range).lazy[.select/reject{blk}].first(n) / .first */
  if ((!strcmp(name, "first") || !strcmp(name, "last")) &&
      recv >= 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode")) {
    const char *rname = nt_str(nt, recv, "name");
    int lazy_src = -1;
    if (rname && !strcmp(rname, "lazy")) {
      lazy_src = nt_ref(nt, recv, "receiver");
    }
    else if (rname && (!strcmp(rname, "select") || !strcmp(rname, "reject") || !strcmp(rname, "filter"))) {
      int inner = nt_ref(nt, recv, "receiver");
      if (inner >= 0 && nt_type(nt, inner) && !strcmp(nt_type(nt, inner), "CallNode")) {
        const char *iname = nt_str(nt, inner, "name");
        if (iname && !strcmp(iname, "lazy")) lazy_src = nt_ref(nt, inner, "receiver");
      }
    }
    if (lazy_src >= 0 && infer_type(c, lazy_src) == TY_RANGE)
      return (argc == 1) ? TY_INT_ARRAY : TY_INT;
  }

  /* hash receiver methods */
  if (recv >= 0 && !strcmp(name, "default") && argc == 0 &&
      nt_type(nt, recv) && (!strcmp(nt_type(nt, recv), "HashNode") ||
                             !strcmp(nt_type(nt, recv), "KeywordHashNode"))) {
    return TY_POLY; /* {}.default -> nil (poly nil) */
  }
  /* fetch(key, default) on an unknown/empty hash: return the default type */
  if (recv >= 0 && !strcmp(name, "fetch") && argc >= 2 && !ty_is_hash(rt)) {
    TyKind dt = infer_type(c, argv[1]);
    if (dt != TY_UNKNOWN) return dt;
  }
  if (recv >= 0 && ty_is_hash(rt)) {
    if (!strcmp(name, "to_proc")) return TY_PROC;
    if (!strcmp(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) return TY_SYMBOL;
    if (!strcmp(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) return rt;  /* identity */
    if (!strcmp(name, "[]"))     return ty_hash_val(rt);
    if (!strcmp(name, "[]="))    return argc >= 2 ? ty_unify(infer_type(c, argv[1]), ty_hash_val(rt)) : ty_hash_val(rt);
    if (!strcmp(name, "fetch")) {
      TyKind vt = ty_hash_val(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        /* A hash literal default `{}` infers TY_UNKNOWN but is still a hash value
           — incompatible with a non-hash hash-val type like TY_INT. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(nt, argv[1]);
          if (atn && (!strcmp(atn, "HashNode") || !strcmp(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        if (ty_unify(vt, dt) == TY_POLY) return TY_POLY;
      }
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        TyKind bvt = bn > 0 ? infer_type(c, bb[bn - 1]) : vt;
        if (bvt != vt) return TY_POLY;
      }
      return vt;
    }
    if (!strcmp(name, "delete")) return ty_hash_val(rt);
    if (!strcmp(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_hash_val(rt);
      return TY_POLY;
    }
    if (!strcmp(name, "default") && argc == 0) return TY_POLY;
    if (!strcmp(name, "length") || !strcmp(name, "size") ||
        !strcmp(name, "count")) return TY_INT;
    if (!strcmp(name, "keys"))   return ty_array_of(ty_hash_key(rt));
    if (!strcmp(name, "values")) return ty_array_of(ty_hash_val(rt));
    if (!strcmp(name, "values_at") || !strcmp(name, "fetch_values")) return TY_POLY_ARRAY;
    {
      int block = nt_ref(nt, id, "block");
      if (block >= 0 && (!strcmp(name, "map") || !strcmp(name, "collect"))) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return ty_array_of(bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN);
      }
      if (block >= 0 && (!strcmp(name, "select") || !strcmp(name, "filter") || !strcmp(name, "reject"))) return rt;
      if (block >= 0 && !strcmp(name, "transform_keys")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nkt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind r = ty_hash_of(nkt, ty_hash_val(rt));
        return r != TY_UNKNOWN ? r : rt;
      }
      if (block >= 0 && !strcmp(name, "transform_values")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nvt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind r = ty_hash_of(ty_hash_key(rt), nvt);
        return r != TY_UNKNOWN ? r : rt;
      }
    }
    if (!strcmp(name, "merge") && argc == 1) {
      TyKind at = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
      if (at == rt) return rt;  /* same type: trivial */
      /* cross-variant str-keyed merge: promote to str_poly_hash */
      if (ty_hash_key(rt) == TY_STRING && ty_is_hash(at) && ty_hash_key(at) == TY_STRING)
        return TY_STR_POLY_HASH;
      /* cross-variant sym-keyed merge: both sym → sym_poly (only sym_poly exists) */
      if (ty_hash_key(rt) == TY_SYMBOL && ty_is_hash(at) && ty_hash_key(at) == TY_SYMBOL)
        return TY_SYM_POLY_HASH;
      return rt;
    }
    if (!strcmp(name, "dup") || !strcmp(name, "clone") || !strcmp(name, "replace") ||
        !strcmp(name, "merge")) return rt;
    if (!strcmp(name, "has_key?") || !strcmp(name, "key?") ||
        !strcmp(name, "include?") || !strcmp(name, "member?") ||
        !strcmp(name, "has_value?") || !strcmp(name, "value?") ||
        !strcmp(name, "empty?")) return TY_BOOL;
    if (!strcmp(name, "each_with_object") && argc > 0 && argv) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && !strcmp(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && !strcmp(a0ty, "ArrayNode") && an0 == 0) {
          /* When hash values are poly the block pushes poly values, so the
             accumulator widens to poly_array */
          return ty_hash_val(rt) == TY_POLY ? TY_POLY_ARRAY : TY_INT_ARRAY;
        }
      }
      return at;
    }
    if (!strcmp(name, "flatten") && argc <= 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "invert") && argc == 0) {
      /* swap key/value types where we have a typed variant */
      if (rt == TY_STR_STR_HASH) return TY_STR_STR_HASH;
      return TY_POLY_POLY_HASH;
    }
    if ((!strcmp(name, "assoc") || !strcmp(name, "rassoc")) && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "compact") && argc == 0) return rt;
  }

  /* <str>.encoding.name -> the encoding name string */
  if (!strcmp(name, "name") && argc == 0 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && !strcmp(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    if (!strcmp(name, "encoding") && argc == 0) return TY_POLY;  /* an Encoding value */
    if (!strcmp(name, "upcase") || !strcmp(name, "downcase") ||
        !strcmp(name, "capitalize") || !strcmp(name, "reverse") ||
        !strcmp(name, "strip") || !strcmp(name, "lstrip") ||
        !strcmp(name, "rstrip") || !strcmp(name, "chomp") ||
        !strcmp(name, "chop") || !strcmp(name, "chr") || !strcmp(name, "clamp") ||
        !strcmp(name, "squeeze") || !strcmp(name, "tr") || !strcmp(name, "tr_s") ||
        !strcmp(name, "succ") || !strcmp(name, "next") ||
        !strcmp(name, "delete")) return TY_STRING;
    if (!strcmp(name, "[]") || !strcmp(name, "slice") || !strcmp(name, "byteslice") ||
        !strcmp(name, "force_encoding") || !strcmp(name, "b") || !strcmp(name, "encode")) return TY_STRING;
    if ((!strcmp(name, "dump") || !strcmp(name, "undump")) && argc == 0) return TY_STRING;
    if (!strcmp(name, "index") && argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "RegularExpressionNode")) return TY_POLY;  /* nil on no match */
    }
    if (!strcmp(name, "index") || !strcmp(name, "to_i") || !strcmp(name, "count") ||
        !strcmp(name, "oct") || !strcmp(name, "hex") || !strcmp(name, "ord") ||
        !strcmp(name, "casecmp") ||
        !strcmp(name, "bytesize") || !strcmp(name, "setbyte") || !strcmp(name, "getbyte")) return TY_INT;
    if (!strcmp(name, "scrub") || !strcmp(name, "crypt")) return TY_STRING;
    if (!strcmp(name, "rindex")) return TY_INT;
    if (!strcmp(name, "partition") || !strcmp(name, "rpartition")) return TY_STR_ARRAY;
    if (!strcmp(name, "casecmp?") || !strcmp(name, "ascii_only?") || !strcmp(name, "valid_encoding?")) return TY_BOOL;
    if (!strcmp(name, "to_f"))  return TY_FLOAT;
    if (!strcmp(name, "each_char") || !strcmp(name, "each_line") || !strcmp(name, "each_byte")) return TY_STRING;
    { int blk = nt_ref(nt, id, "block");
      if (blk >= 0 && (!strcmp(name, "chars") || !strcmp(name, "lines"))) return TY_STRING;
      if (blk >= 0 && (!strcmp(name, "bytes") || !strcmp(name, "codepoints"))) return TY_STRING; }
    if (!strcmp(name, "split") || !strcmp(name, "lines")) return TY_STR_ARRAY;
    if (!strcmp(name, "scan") && argc == 1) {
      /* scan with capture groups returns poly_array (array of arrays or strings) */
      const char *aty = nt_type(nt, argv[0]);
      if (aty && !strcmp(aty, "RegularExpressionNode")) {
        const char *src = nt_str(nt, argv[0], "unescaped");
        if (src && an_re_has_captures(src)) return TY_POLY_ARRAY;
      }
      return TY_STR_ARRAY;
    }
    if (!strcmp(name, "upto") && argc == 1) return TY_STR_ARRAY;  /* blockless: materialized sequence */
    if (!strcmp(name, "bytes") || !strcmp(name, "codepoints")) return TY_INT_ARRAY;
    if (!strcmp(name, "unpack") && argc == 1) return TY_POLY_ARRAY;
    if (!strcmp(name, "chars")) return TY_STR_ARRAY;
    if (!strcmp(name, "gsub") || !strcmp(name, "sub") || !strcmp(name, "tr") ||
        !strcmp(name, "center") || !strcmp(name, "ljust") || !strcmp(name, "rjust"))
      return TY_STRING;
    if (!strcmp(name, "*")) return TY_STRING;
    /* in-place append / concat reassign the receiver and evaluate to it */
    if ((!strcmp(name, "<<") || !strcmp(name, "concat") || !strcmp(name, "prepend")) && argc == 1)
      return TY_STRING;
  }
  /* <int_array>.product(<int_array>)[.to_a].inspect -> a string */
  if (!strcmp(name, "inspect") && argc == 0 && recv >= 0) {
    int pr = recv;
    if (nt_type(nt, pr) && !strcmp(nt_type(nt, pr), "CallNode") &&
        nt_str(nt, pr, "name") && !strcmp(nt_str(nt, pr, "name"), "to_a"))
      pr = nt_ref(nt, pr, "receiver");
    if (pr >= 0 && nt_type(nt, pr) && !strcmp(nt_type(nt, pr), "CallNode") && nt_str(nt, pr, "name") &&
        (!strcmp(nt_str(nt, pr, "name"), "product") || !strcmp(nt_str(nt, pr, "name"), "slice_before") ||
         !strcmp(nt_str(nt, pr, "name"), "slice_after") || !strcmp(nt_str(nt, pr, "name"), "slice_when") ||
         !strcmp(nt_str(nt, pr, "name"), "chunk")))
      return TY_STRING;
  }

  /* numeric.step(...) without a block materializes the sequence as an array */
  if (recv >= 0 && ty_is_numeric(rt) && !strcmp(name, "step") && nt_ref(nt, id, "block") < 0) {
    int args = nt_ref(nt, id, "arguments");
    int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
    int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
              (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
    return isf ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    if (!strcmp(name, "ceil") || !strcmp(name, "floor") ||
        !strcmp(name, "round") || !strcmp(name, "truncate")) return TY_INT;  /* no precision arg -> self */
    if (!strcmp(name, "divmod") && argc == 1) return TY_INT_ARRAY;  /* [quotient, remainder] */
    if ((!strcmp(name, "allbits?") || !strcmp(name, "anybits?") || !strcmp(name, "nobits?")) && argc == 1) return TY_BOOL;
    if (!strcmp(name, "even?") || !strcmp(name, "odd?") || !strcmp(name, "zero?") ||
        !strcmp(name, "positive?") || !strcmp(name, "negative?")) return TY_BOOL;
    if ((!strcmp(name, "ceildiv") || !strcmp(name, "pow")) && argc >= 1) return TY_INT;
    if ((!strcmp(name, "pred") || !strcmp(name, "succ") || !strcmp(name, "next")) && argc == 0) return TY_INT;
    if (!strcmp(name, "nonzero?") && argc == 0) return TY_INT;  /* self or nil (nullable int) */
    /* times/upto/downto/step with a block return the receiver (self) */
    if ((!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto") ||
         !strcmp(name, "step")) && nt_ref(nt, id, "block") >= 0) return TY_INT;
    /* times/upto/downto without a block return a range-like enumerator */
    if ((!strcmp(name, "times") || !strcmp(name, "upto") || !strcmp(name, "downto")) &&
        nt_ref(nt, id, "block") < 0) return TY_RANGE;
    if (!strcmp(name, "chr")) return TY_STRING;
    if (!strcmp(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (!strcmp(name, "div") && argc == 1) return TY_INT;  /* floor division */
    if (!strcmp(name, "gcd") || !strcmp(name, "lcm") || !strcmp(name, "clamp")) return TY_INT;
    if (!strcmp(name, "magnitude") && argc == 0) return TY_INT;  /* alias for abs */
    if ((!strcmp(name, "modulo") || !strcmp(name, "remainder")) && argc == 1) return TY_INT;
    if (!strcmp(name, "gcdlcm") && argc == 1) return TY_INT_ARRAY;  /* [gcd, lcm] */
    if (!strcmp(name, "digits")) return TY_INT_ARRAY;
    if (!strcmp(name, "to_s") && argc == 1) return TY_STRING;
    if (!strcmp(name, "coerce") && argc == 1) {
      TyKind a0 = infer_type(c, argv[0]);
      return (a0 == TY_FLOAT) ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
    }
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if (!strcmp(name, "coerce") && argc == 1) return TY_FLOAT_ARRAY;  /* [Float(other), self] */
    if (!strcmp(name, "divmod") && argc == 1) return TY_POLY_ARRAY;  /* [Integer, Float] */
    if (!strcmp(name, "infinite?")) return TY_INT;   /* nil / 1 / -1 (nullable int) */
    if (!strcmp(name, "nan?") || !strcmp(name, "finite?") ||
        !strcmp(name, "positive?") || !strcmp(name, "negative?") ||
        !strcmp(name, "zero?")) return TY_BOOL;
    if (!strcmp(name, "next_float") || !strcmp(name, "prev_float") ||
        !strcmp(name, "abs") || !strcmp(name, "magnitude") ||
        !strcmp(name, "modulo") || !strcmp(name, "to_f")) return TY_FLOAT;
    if (!strcmp(name, "floor") || !strcmp(name, "ceil") ||
        !strcmp(name, "round") || !strcmp(name, "truncate")) {
      /* value-based return type: ndigits > 0 (literal) -> Float, else Integer */
      if (argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        if (aty && !strcmp(aty, "IntegerNode") && nt_int(nt, argv[0], "value", 0) > 0)
          return TY_FLOAT;
      }
      return TY_INT;
    }
  }

  /* /re/ === str -> match boolean */
  if (!strcmp(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "RegularExpressionNode"))
    return TY_BOOL;
  /* Class.===(obj) is always bool */
  if (!strcmp(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "ConstantReadNode"))
    return TY_BOOL;

  if ((!strcmp(name, "-@") || !strcmp(name, "+@")) && recv >= 0 && argc == 0)
    return ty_is_numeric(rt) ? rt : rt == TY_POLY ? TY_POLY : TY_UNKNOWN;
  /* unary bitwise complement: ~int / ~poly -> int (poly value coerced via to_i) */
  if (!strcmp(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY))
    return TY_INT;
  if (!strcmp(name, "!")) return TY_BOOL;
  if (!strcmp(name, "respond_to?") && recv >= 0) return TY_BOOL;
  if ((!strcmp(name, "method_defined?") || !strcmp(name, "const_defined?")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "nil?") && recv >= 0 && argc == 0) return TY_BOOL;
  if (!strcmp(name, "object_id") && recv >= 0 && argc == 0) return TY_INT;
  if (!strcmp(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) return TY_BOOL;
  if ((!strcmp(name, "match?") || !strcmp(name, "!~")) && recv >= 0) return TY_BOOL;
  if (!strcmp(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    const char *rrt = nt_type(nt, recv), *art = argc > 0 ? nt_type(nt, argv[0]) : NULL;
    if ((rrt && !strcmp(rrt, "RegularExpressionNode")) ||
        (art && !strcmp(art, "RegularExpressionNode"))) return TY_MATCHDATA;
  }
  if (!strcmp(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if ((rrt && !strcmp(rrt, "RegularExpressionNode")) ||
        (art && !strcmp(art, "RegularExpressionNode")) ||
        rt == TY_REGEX || a0t == TY_REGEX) return TY_POLY;
  }
  if (!strcmp(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if (rt == TY_REGEX || a0t == TY_REGEX) return TY_MATCHDATA;
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "RegularExpressionNode")) {
    if (!strcmp(name, "source")) return TY_STRING;
    if (!strcmp(name, "options")) return TY_INT;
  }

  /* array set operations: &, intersection, |, union(1-arg), -, difference */
  if (recv >= 0 && argc == 1 &&
      (!strcmp(name, "&") || !strcmp(name, "intersection") ||
       !strcmp(name, "|") || !strcmp(name, "union") ||
       !strcmp(name, "-") || !strcmp(name, "difference"))) {
    if (ty_is_array(rt) && a0 == rt) return rt;
    if (ty_is_array(rt) && a0 == TY_POLY_ARRAY) return rt;
    if (ty_is_array(a0) && rt == TY_POLY_ARRAY) return a0;
    /* empty array [] arg (TY_UNKNOWN): result is same kind as receiver */
    if (ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
  }
  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (!strcmp(name, "%")) return TY_STRING;  /* sprintf (array or single value) */
      if (!strcmp(name, "+") || !strcmp(name, "*")) {
        /* `str + x` / `str * n` always yield a String; a poly operand (which
           holds a string at runtime) is coerced via sp_poly_to_s in codegen. */
        return TY_STRING;
      }
      return TY_UNKNOWN;
    }
    /* array + same-kind -> same kind; different-kind -> poly_array */
    if (!strcmp(name, "+") && ty_is_array(rt) && a0 == rt) return rt;
    if (!strcmp(name, "+") && ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
    /* array * int -> same array type (repeat); array * string -> join string */
    if (!strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_INT) return rt;
    if (!strcmp(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(rt) && ty_is_numeric(a0)) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) return TY_FLOAT;
      if (rt == TY_BIGINT || a0 == TY_BIGINT) return TY_BIGINT;
      return TY_INT;
    }
    /* a poly operand makes the +,-,*,/ result poly: codegen lowers these to
       sp_poly_<op>, which returns a (boxed) poly, so the static type must agree. */
    if ((rt == TY_POLY || a0 == TY_POLY) &&
        (!strcmp(name, "+") || !strcmp(name, "-") || !strcmp(name, "*") || !strcmp(name, "/")))
      return TY_POLY;
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && !strcmp(name, "<=>")) return TY_INT;
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) return TY_BOOL;
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^") ||
       !strcmp(name, "<<") || !strcmp(name, ">>")))
    return TY_INT;
  /* poly recv bitwise op: result is int (sp_poly_to_i applied). */
  if (recv >= 0 && argc == 1 && rt == TY_POLY &&
      (!strcmp(name, ">>") || !strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")))
    return TY_INT;
  /* poly recv `<<` is ambiguous (Integer#<< shift vs Array#push append); the
     runtime sp_poly_shl dispatches on the tag and returns a boxed result either
     way, so the static type is poly. A downstream bitwise op coerces it back to
     int, and an append keeps its (boxed) array -- both stay consistent. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && !strcmp(name, "<<"))
    return TY_POLY;
  /* boolean &/|/^ */
  if (recv >= 0 && argc == 1 && rt == TY_BOOL &&
      (!strcmp(name, "&") || !strcmp(name, "|") || !strcmp(name, "^")))
    return TY_BOOL;

  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') return TY_BOOL;

  if (!strcmp(name, "to_s") || !strcmp(name, "inspect") ||
      !strcmp(name, "chr") || !strcmp(name, "to_str")) return TY_STRING;
  if (!strcmp(name, "to_i") || !strcmp(name, "to_int") ||
      !strcmp(name, "length") || !strcmp(name, "size") ||
      !strcmp(name, "ord") || !strcmp(name, "abs")) return TY_INT;
  if (!strcmp(name, "to_f")) return TY_FLOAT;
  if (!strcmp(name, "to_sym")) return TY_SYMBOL;

  if (is_void_call(name) && recv < 0) return TY_VOID;

  /* $stdout/$stderr.puts/print/write return nil (so a value-position use --
     an if/else arm or assignment -- unifies and boxes as nil). */
  if (recv >= 0 && (!strcmp(name, "puts") || !strcmp(name, "print") || !strcmp(name, "write")) &&
      nt_type(nt, recv) && !strcmp(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gv = nt_str(nt, recv, "name");
    if (gv && (!strcmp(gv, "$stdout") || !strcmp(gv, "$stderr"))) return TY_NIL;
  }

  /* tap: run block, return self */
  if (!strcmp(name, "tap") && recv >= 0) return rt;
  /* then / yield_self: run block, return block result */
  if (!strcmp(name, "then") || !strcmp(name, "yield_self")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      /* Pin block param to receiver type so body inference uses the right type */
      const char *bp0 = block_param_name(c, blk_id, 0);
      Scope *bs = bp0 ? comp_scope_of(c, blk_id) : NULL;
      LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind saved_blv = blv ? blv->type : TY_UNKNOWN;
      if (blv && rt != TY_UNKNOWN) blv->type = rt;
      TyKind result = infer_type(c, bbb[bbn - 1]);
      if (blv) blv->type = saved_blv;
      return result;
    }
  }
  if (!strcmp(name, "instance_eval")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0 && ty_is_object(rt) &&
        comp_method_in_chain(c, ty_object_class(rt), "instance_eval", NULL) < 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      int saved_ie = an_ie_class_id;
      an_ie_class_id = ty_object_class(rt);
      TyKind result = infer_type(c, bbb[bbn - 1]);
      an_ie_class_id = saved_ie;
      return result;
    }
    return TY_POLY;
  }

  /* safe navigation &. with unresolved type: return poly (receiver may be nil at runtime) */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && !strcmp(call_op, "&.")) return TY_POLY;
  }

  /* Builtin class reopening: look up user-defined methods on Array/Numeric/Object
     receivers where no builtin method matched. */
  if (recv >= 0) {
    /* Array reopening: any array-typed receiver */
    if (ty_is_array(rt)) {
      int oc_ci = comp_class_index(c, "Array");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Numeric reopening: integers and floats */
    if (rt == TY_INT || rt == TY_FLOAT) {
      int oc_ci = comp_class_index(c, "Numeric");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* FalseClass methods (TrueClass already checked earlier for TY_BOOL) */
    if (rt == TY_BOOL) {
      int oc_ci = comp_class_index(c, "FalseClass");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Object reopening: universal fallback for any receiver type */
    {
      int oc_ci = comp_class_index(c, "Object");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
  }

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  NodeKind nk = nt_kind(nt, id);

  if (nk == NK_IntegerNode)             return TY_INT;
  if (nk == NK_FloatNode)               return TY_FLOAT;
  if (nk == NK_ImaginaryNode)           return TY_COMPLEX;
  if (nk == NK_RationalNode)            return TY_RATIONAL;
  if (nk == NK_StringNode)              return TY_STRING;
  if (nk == NK_SourceFileNode)          return TY_STRING;
  if (nk == NK_SourceLineNode)          return TY_INT;
  if (nk == NK_SourceEncodingNode)      return TY_POLY;
  if (nk == NK_RegularExpressionNode ||
      nk == NK_InterpolatedRegularExpressionNode) return TY_REGEX;
  if (nk == NK_InterpolatedStringNode)  return TY_STRING;
  if (nk == NK_XStringNode || nk == NK_InterpolatedXStringNode) return TY_STRING;
  if (nk == NK_InterpolatedSymbolNode)  return TY_SYMBOL;
  if (nk == NK_SymbolNode)              return TY_SYMBOL;
  if (nk == NK_TrueNode)                return TY_BOOL;
  if (nk == NK_FalseNode)               return TY_BOOL;
  if (nk == NK_NilNode)                 return TY_NIL;
  if (nk == NK_RangeNode) {
    /* infer the bounds so codegen can tell an int range from a string range */
    int lo = nt_ref(nt, id, "left"), hi = nt_ref(nt, id, "right");
    if (lo >= 0) infer_type(c, lo);
    if (hi >= 0) infer_type(c, hi);
    return TY_RANGE;
  }
  /* A splat inside an array literal (`[*0..10]`, `[*arr]`) contributes the
     element type of the splatted collection, so the literal stays a typed
     array instead of widening to poly_array. The value returned here is the
     would-be element type, which the ArrayNode arm unifies in. */
  if (nk == NK_SplatNode) {
    int inner = nt_ref(nt, id, "expression");
    if (inner < 0) return TY_UNKNOWN;
    const char *ity = nt_type(nt, inner);
    if (ity && !strcmp(ity, "RangeNode")) {
      int lo = nt_ref(nt, inner, "left");
      return (lo >= 0 && infer_type(c, lo) == TY_STRING) ? TY_STRING : TY_INT;
    }
    TyKind it = infer_type(c, inner);
    if (ty_is_array(it)) return ty_array_elem(it);
    return TY_POLY;
  }
  if (nk == NK_LambdaNode)              return TY_PROC;
  /* an assignment expression evaluates to the assigned value -- but codegen
     lowers `x = expr` to `({ lv_x = ...; lv_x; })`, so the chain value IS the
     slot. Return the local's slot type (when known) so a chained `a = b = expr`
     boxes consistently with the slot, mirroring the ivar-write rule below. */
  if (nk == NK_LocalVariableWriteNode) {
    const char *lwn = nt_str(nt, id, "name");
    Scope *lws = comp_scope_of(c, id);
    LocalVar *lwv = lwn ? scope_local(lws, lwn) : NULL;
    if (lwv && lwv->type != TY_UNKNOWN) return lwv->type;
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_InstanceVariableWriteNode ||
      nk == NK_InstanceVariableOrWriteNode ||
      nk == NK_InstanceVariableAndWriteNode ||
      nk == NK_InstanceVariableOperatorWriteNode) {
    /* expression evaluates to the ivar slot's type (same as a read): codegen
       lowers `@a = expr` to `({ iv_a = ...; iv_a; })`, so the chain value IS the
       slot, and inference must match to keep `@x = @a = expr` boxing consistent. */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0) return infer_type(c, nt_ref(nt, id, "value"));
    ClassInfo *ci = &c->classes[s->class_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (nk == NK_LocalVariableOperatorWriteNode) {
    const char *nm2 = nt_str(nt, id, "name");
    Scope *s2 = comp_scope_of(c, id);
    LocalVar *lv2 = nm2 ? scope_local(s2, nm2) : NULL;
    TyKind ct2 = lv2 ? lv2->type : TY_UNKNOWN;
    TyKind vt2 = infer_type(c, nt_ref(nt, id, "value"));
    if (ct2 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct2) && ty_is_numeric(vt2))
      return (ct2 == TY_FLOAT || vt2 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct2 != TY_UNKNOWN ? ct2 : vt2;
  }
  if (nk == NK_LocalVariableOrWriteNode || nk == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
  }

  if (nk == NK_LocalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* &block param that escapes (not yield-inlined): the LocalVar slot type is
       TY_UNKNOWN, but the value is a Proc object when the method does not inline
       the block (yields==0). Return TY_PROC so callers can type the return value. */
    if (nm && s && s->blk_param && s->blk_param[0] && !strcmp(nm, s->blk_param)
        && !s->yields)
      return TY_PROC;
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    if (lv) return lv->type;
    return TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && !strcmp(nm, "$/")) return TY_STRING;
    if (nm && !strcmp(nm, "$?")) return TY_INT;  /* last child exit status */
    if (nm && (!strcmp(nm, "$PROGRAM_NAME") || !strcmp(nm, "$0"))) return TY_STRING;
    if (nm && (!strcmp(nm, "$!") || !strcmp(nm, "$;") || !strcmp(nm, "$,"))) return TY_NIL;
    /* regex match globals: nullable strings ($~ == $&, $`, $', $+) */
    if (nm && (!strcmp(nm, "$~") || !strcmp(nm, "$&") || !strcmp(nm, "$`") ||
               !strcmp(nm, "$'") || !strcmp(nm, "$+"))) return TY_STRING;
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (nk == NK_ConstantReadNode) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    if (lv) return lv->type;
    if (nm && (!strcmp(nm, "RUBY_DESCRIPTION") || !strcmp(nm, "RUBY_VERSION") ||
               !strcmp(nm, "RUBY_PLATFORM") || !strcmp(nm, "RUBY_ENGINE") ||
               !strcmp(nm, "RUBY_ENGINE_VERSION") || !strcmp(nm, "RUBY_RELEASE_DATE") ||
               !strcmp(nm, "RUBY_REVISION") || !strcmp(nm, "RUBY_COPYRIGHT"))) return TY_STRING;
    if (nm && !strcmp(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && !strcmp(nm, "ARGF")) return TY_ARGF;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    return TY_UNKNOWN;
  }
  if (nk == NK_DefinedNode) return TY_STRING;  /* a label string, or nil (NULL) */
  if (nk == NK_NumberedReferenceReadNode) return TY_STRING;  /* $1..$9: capture, or nil (NULL) */
  if (nk == NK_BackReferenceReadNode) return TY_STRING;  /* $&/$`/$'/$~/$+: nullable string */
  if (nk == NK_ConstantPathNode) {
    /* M::CONST -> resolve by the final path component (constants register
       under their unqualified name) */
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    if (lv) return lv->type;
    if (nm && !strcmp(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && !strcmp(nm, "ARGF")) return TY_ARGF;
    /* well-known module constants */
    int par_id = nt_ref(nt, id, "parent");
    const char *par_ty = par_id >= 0 ? nt_type(nt, par_id) : NULL;
    const char *par_nm = (par_ty && !strcmp(par_ty, "ConstantReadNode")) ? nt_str(nt, par_id, "name") : NULL;
    if (par_nm && !strcmp(par_nm, "Float")) {
      if (nm && (!strcmp(nm, "MAX") || !strcmp(nm, "MIN") || !strcmp(nm, "EPSILON") ||
                 !strcmp(nm, "INFINITY") || !strcmp(nm, "NAN") || !strcmp(nm, "DIG") ||
                 !strcmp(nm, "MANT_DIG") || !strcmp(nm, "RADIX"))) return TY_FLOAT;
    }
    if (par_nm && !strcmp(par_nm, "Math")) {
      if (nm && (!strcmp(nm, "PI") || !strcmp(nm, "E"))) return TY_FLOAT;
    }
    if (par_nm && !strcmp(par_nm, "File")) {
      if (nm && (!strcmp(nm, "SEPARATOR") || !strcmp(nm, "PATH_SEPARATOR") ||
                 !strcmp(nm, "ALT_SEPARATOR"))) return TY_STRING;
    }
    if (par_nm && !strcmp(par_nm, "Integer")) {
      if (nm && (!strcmp(nm, "MAX") || !strcmp(nm, "MIN"))) return TY_UNKNOWN; /* raises NameError */
    }
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* FFI const: Module::NAME -> int */
    if (par_nm && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (!strcmp(c->ffi_const_mods[fci], par_nm) &&
            !strcmp(c->ffi_const_names[fci], nm))
          return TY_INT;
      }
    }
    return TY_UNKNOWN;
  }
  if (nk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    int self_cls = s->class_id;
    /* `self` inside an instance_eval/exec block is the rebound receiver. */
    if (self_cls < 0) self_cls = (an_ie_class_id >= 0) ? an_ie_class_id : ie_class_of(c, id);
    if (self_cls < 0) return TY_UNKNOWN;
    const char *cn = c->classes[self_cls].name;
    if (!strcmp(cn, "String"))  return TY_STRING;
    if (!strcmp(cn, "Integer")) return TY_INT;
    if (!strcmp(cn, "Float"))   return TY_FLOAT;
    if (!strcmp(cn, "Symbol"))  return TY_SYMBOL;
    if (!strcmp(cn, "TrueClass") || !strcmp(cn, "FalseClass") || !strcmp(cn, "NilClass")) return TY_BOOL;
    if (!strcmp(cn, "Array"))   return TY_POLY_ARRAY;
    if (!strcmp(cn, "Object"))  return TY_POLY;  /* dynamic: called on any receiver type */
    return ty_object(self_cls);
  }
  if (nk == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cls_id = (s->class_id >= 0) ? s->class_id : an_ie_class_id;
    if (cls_id < 0) cls_id = ie_class_of(c, id);
    if (cls_id < 0) cls_id = comp_class_index(c, "Toplevel");
    if (cls_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) return TY_UNKNOWN;
    int idx = nm ? comp_cvar_index(&c->classes[cid], nm) : -1;
    return idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableWriteNode ||
      nk == NK_ClassVariableOperatorWriteNode ||
      nk == NK_ClassVariableOrWriteNode ||
      nk == NK_ClassVariableAndWriteNode)
    return infer_type(c, nt_ref(nt, id, "value"));
  if (nk == NK_IndexOrWriteNode || nk == NK_IndexAndWriteNode) {
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) return TY_UNKNOWN;
    TyKind rt = infer_type(c, recv);
    if (ty_is_array(rt)) return ty_array_elem(rt);
    if (ty_is_hash(rt)) return ty_hash_val(rt);
    return TY_POLY;
  }
  if (nk == NK_ParenthesesNode) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_NIL;
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_StatementsNode) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_CaseNode) {
    /* value = unify of each when's body; a missing else means a no-match
       falls through to nil */
    int nw = 0; const int *whens = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, whens[w], "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    if (else_c >= 0) { int st = nt_ref(nt, else_c, "statements"); r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL); }
    else r = ty_unify(r, TY_NIL);
    return r;
  }
  if (nk == NK_CaseMatchNode) {
    /* case X; in PATTERN; ... — value = unify of each arm's body (+ else). */
    int nw = 0; const int *conds = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, conds[w], "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    if (else_c >= 0) { int st = nt_ref(nt, else_c, "statements"); r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL); }
    return r;
  }
  if (nk == NK_IfNode || nk == NK_UnlessNode) {
    int is_unless = nk == NK_UnlessNode;
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    TyKind tt = then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
    TyKind et = else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    return ty_unify(tt, et);
  }
  if (nk == NK_ElseNode) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (nk == NK_ArrayNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) return TY_UNKNOWN;  /* empty: element type comes from usage */
    TyKind e = TY_UNKNOWN;
    for (int k = 0; k < n; k++) e = ty_unify(e, infer_type(c, els[k]));
    return ty_array_of(e);
  }
  if (nk == NK_HashNode || nk == NK_KeywordHashNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) return TY_UNKNOWN;
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (!aty || strcmp(aty, "AssocNode")) return TY_UNKNOWN;
      kt = ty_unify(kt, infer_type(c, nt_ref(nt, els[k], "key")));
      int vnode = nt_ref(nt, els[k], "value");
      TyKind vt_elem = infer_type(c, vnode);
      /* A nested hash literal (even empty `{}`) is a non-scalar value; treat
         it as poly so the outer hash promotes to a poly-valued variant. */
      if (vt_elem == TY_UNKNOWN) {
        const char *vnode_ty = nt_type(nt, vnode);
        if (vnode_ty && (!strcmp(vnode_ty, "HashNode") || !strcmp(vnode_ty, "KeywordHashNode")))
          vt_elem = TY_POLY;
      }
      vt = ty_unify(vt, vt_elem);
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    /* unsupported combination -> fall back to poly storage */
    if (hv == TY_UNKNOWN && vt != TY_UNKNOWN) return TY_POLY_POLY_HASH;
    return hv;
  }
  if (nk == NK_YieldNode)
    return yield_value_type(c, (int)(comp_scope_of(c, id) - c->scopes));
  if (nk == NK_SuperNode || nk == NK_ForwardingSuperNode) {
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
    if (shadow) {
      int mi = comp_method_in_class(c, s->class_id, shadow);
      return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
    }
    const char *uname = comp_prep_user_name(s->name);
    int p = c->classes[s->class_id].parent;
    if (p < 0) return TY_UNKNOWN;
    int mi = comp_method_in_chain(c, p, uname, NULL);
    return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
  }
  if (nk == NK_AndNode || nk == NK_OrNode) {
    TyKind lt = infer_type(c, nt_ref(nt, id, "left"));
    TyKind rt = infer_type(c, nt_ref(nt, id, "right"));
    if (lt == TY_BOOL && rt == TY_BOOL) return TY_BOOL;
    return ty_unify(lt, rt);  /* value form: a || b -> common type */
  }
  if (nk == NK_BeginNode) {
    /* value = body value unified with each rescue handler's value */
    int body = nt_ref(nt, id, "statements");
    TyKind r = body >= 0 ? infer_type(c, body) : TY_NIL;
    for (int rs = nt_ref(nt, id, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent")) {
      int st = nt_ref(nt, rs, "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    return r;
  }
  if (nk == NK_CallNode) return infer_call(c, id);

  if (nk == NK_RescueModifierNode) {
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    TyKind et = e >= 0 ? infer_type(c, e) : TY_NIL;
    TyKind rt = r >= 0 ? infer_type(c, r) : TY_NIL;
    /* a diverging expression like raise has no real type; use the rescue arm's type */
    if (et == TY_UNKNOWN || et == TY_VOID || et == TY_NIL) return rt;
    return ty_unify(et, rt);
  }

  /* MultiWriteNode as expression: value is the RHS array. */
  if (nk == NK_MultiWriteNode)
    return infer_type(c, nt_ref(nt, id, "value"));

  return TY_UNKNOWN;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  TyKind t = infer_uncached(c, id);
  c->ntype[id] = t;
  return t;
}

/* ---- scope assignment ---- */

void scope_add_param(Scope *s, const char *name, int defnode) {
  if (s->nparams % 8 == 0) {
    s->pnames = realloc(s->pnames, sizeof(char *) * (size_t)(s->nparams + 8));
    s->pdefault = realloc(s->pdefault, sizeof(int) * (size_t)(s->nparams + 8));
  }
  s->pdefault[s->nparams] = defnode;
  s->pnames[s->nparams++] = strdup(name);
  if (defnode < 0) s->nrequired = s->nparams;
  LocalVar *lv = scope_local_intern(s, name);
  lv->is_param = 1;
}

/* Collect parameters from a DefNode into scope s. */
