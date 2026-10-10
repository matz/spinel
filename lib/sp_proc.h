#ifndef SP_PROC_H
#define SP_PROC_H
/* sp_proc.h -- sp_Proc / sp_Curry struct layouts + cold ops.
 *
 * sp_proc_call itself (the hot per-call dispatch) stays a plain
 * (non-static) function defined directly in spinel_rt.h -- like
 * sp_sprintf/sp_argv, its body is resolved at the final link against the
 * generated TU, so it doesn't need to move for lib/sp_proc.c to reach it.
 * _sp_proc_poly_args/_sp_proc_poly_ret (the boxed calling-convention side
 * channel) are likewise already non-static SP_TLS globals in
 * spinel_rt.h; only extern declarations are needed here.
 *
 * Proc#>>/<</compose and the Proc-return (non-local return via longjmp)
 * machinery stay in spinel_rt.h: sp_proc_compose_fn calls sp_poly_to_i
 * (a value-dispatch function that's hot in optcarrot and excluded from
 * eviction), and proc-return threads through the TU-local exception-stack
 * globals (sp_exc_top / sp_unwind_kind / sp_proc_ret_head).
 *
 * 0 optcarrot uses for every function below.
 */
#include "sp_types.h"   /* sp_int, sp_sym, sp_bool */
#include "sp_gc.h"      /* sp_RbVal, sp_gc_alloc, sp_gc_mark */
#include "sp_alloc.h"   /* sp_PolyArray, sp_box_sym, sp_box_poly_array, sp_raise_cls */

typedef struct sp_Proc { void *fn; void *cap; void (*cap_scan)(void *); sp_int arity; sp_bool lambda_p; sp_int param_count; const sp_sym *param_kinds; const sp_sym *param_names; sp_bool frozen; /* Object#freeze observed (sp_gc_alloc zero-fills) */ void *origin; /* dup/clone lineage root for Proc#== (NULL: self is the root) */ sp_int ie_cls; } sp_Proc;
/* arity: the count this accumulator realizes at -- Proc#curry(n)'s n, else
   the target's required-parameter count (CRuby's min arity, so a variadic
   base realizes on its first call). Carried here so a curry that travels
   through a container or an untyped slot still knows when it is done. */
typedef struct { sp_Proc *target; sp_int arity; sp_int nargs; sp_RbVal args[16]; } sp_Curry;

sp_int sp_proc_call(sp_Proc *p, sp_int argc, sp_int *args);   /* defined in the generated TU */
/* The proc calling convention's boxed side channel: how many arguments it
   carries. Every publisher, the GC scan that keeps them alive, and the gates
   that decline a longer call read this one name -- a second copy of the
   number in any of them is a call whose arguments are published and never
   marked, or marked and never passed. Machine-generated code reaches 17
   parameters and the old 16 refused it outright. */
#define SP_PROC_ARG_SLOTS 64
extern SP_TLS sp_RbVal _sp_proc_poly_args[SP_PROC_ARG_SLOTS];    /* defined in the generated TU */
extern SP_TLS sp_RbVal _sp_proc_poly_ret;                        /* defined in the generated TU */

/* The lineage root of a proc: dups/clones of one proc share it, so Proc#== /
   #eql? compare roots (a dup == its original) while distinct literals differ. */
static inline sp_Proc *sp_proc_root(sp_Proc *p) { return (p && p->origin) ? (sp_Proc *)p->origin : p; }

void sp_Proc_scan(void *p);
sp_Proc *sp_proc_new_meta(void *fn, void *cap, void (*cap_scan)(void *), sp_int arity, sp_bool lambda_p, sp_int param_count, const sp_sym *param_kinds, const sp_sym *param_names);
sp_Proc *sp_proc_dup(sp_Proc *p, int keep_frozen);
sp_Proc *sp_proc_new(void *fn, void *cap, void (*cap_scan)(void *));
sp_int sp_proc_arity(sp_Proc *p);
sp_bool sp_proc_lambda_p(sp_Proc *p);
const char *sp_proc_inspect(sp_Proc *p);
sp_PolyArray *sp_proc_parameters_ids(sp_Proc *p, int mode, sp_sym req_id, sp_sym opt_id);
sp_PolyArray *sp_proc_parameters(sp_Proc *p);
void sp_curry_scan(void *p);
sp_Curry *sp_curry_new(sp_Proc *p);
sp_Curry *sp_curry_new_n(sp_Proc *p, sp_int n, sp_int max);
sp_Curry *sp_curry_apply(sp_Curry *c, sp_RbVal arg);
void sp_curry_publish_args(sp_Curry *c);

/* ---- BoundMethod (Method object): sp_bound_method_new is hot in
   optcarrot (69 uses), so it stays static inline here -- pure textual
   move, same per-TU inlining as before. sp_BoundMethod_scan is a GC
   callback (only ever invoked indirectly through the function pointer
   sp_bound_method_new hands to sp_gc_alloc), so moving its body to
   lib/sp_proc.c costs nothing -- taking its address from an inline
   context is just an embedded constant, not a call site. ---- */
/* What `self` holds, so the scan knows whether to follow it: a Method can be
   bound to an Integer or a Float as easily as to an object, and marking the
   raw value as a pointer reads whatever address it names. */
#define SP_BM_SELF_NONE 0   /* not a reference: a number, a class value, unbound */
#define SP_BM_SELF_OBJ  1
#define SP_BM_SELF_STR  2
/* How the legacy sp_int C return is boxed back into a Ruby value. Any method
   whose C return fits the register records its kind here: a plain Integer, a
   `const char *` String, a nullable `sp_Bigint *`, a void/nil, a bool, a
   Symbol id, a typed-array or user-object pointer. A synthesized typed-array
   adapter (`<int_array>.method(:push)`) launders the array/string it returns
   through the same register. */
#define SP_BM_RET_INT       0
#define SP_BM_RET_STR       1
#define SP_BM_RET_INT_ARRAY 2
#define SP_BM_RET_STR_ARRAY 3
/* A void C return (a Ruby method whose value is nil, or unused): the cast
   reads an undefined register and the answer is nil regardless. */
#define SP_BM_RET_NIL       4
/* The C function returns sp_RbVal (a poly Ruby return): a 16-byte struct
   comes back in two registers, which no sp_int cast can read, so the call
   arms take the sp_RbVal cast for this kind and box nothing. */
#define SP_BM_RET_POLY      5
/* A bool C return: the callee writes the low byte of the return register and
   leaves the rest undefined (SysV x86-64 and AArch64 both), so the low byte
   is the whole answer. A Symbol is its id in an sp_int. */
#define SP_BM_RET_BOOL      6
#define SP_BM_RET_SYM       7
/* A user-class pointer return: the low byte is the kind and the bits above it
   carry the class id the bind site knew statically (a class without
   subclasses), or nothing when the object carries its own id in its first
   field (a subclassed class: the _dyn box reads it, as the generated boxing
   sites do). Unlike the kinds above, this one is NOT a plain enumerator, so
   sp_bm_box_ret matches on the low byte. */
#define SP_BM_RET_OBJ       8
#define SP_BM_RET_OBJ_DYN   9
/* A Bigint return: a nullable `sp_Bigint *` riding the register, boxed into
   the Ruby value it points at (NULL boxes as nil). */
#define SP_BM_RET_BIGINT   10
#define SP_BM_RET_KIND(r)   ((r) & 0xff)
#define SP_BM_RET_OBJ_OF(cls) (SP_BM_RET_OBJ | ((sp_int)(cls) << 8))
typedef struct sp_BoundMethod { void *self; sp_int fn; const char *name; sp_int arity;
  const char *desc;   /* compile-time #inspect rendering ("#<Method: Owner#name(params)>"), or NULL */
  sp_int self_kind;  /* SP_BM_SELF_* */
  sp_int unbound;    /* built by #unbind on a boxed Method: reports UnboundMethod */
  sp_int recv_bound; /* the target's C ABI takes the bound receiver as its leading
                        argument, so the self-ful cast must be used even when
                        `self` is NULL because the receiver VALUE is zero
                        (Integer 0, false, nil) (#4395). `self != NULL` cannot
                        tell a top-level method (self-less) from a
                        receiver-bound wrapper whose value is 0 (#4395); this
                        flag is the bind site's receiver-bound fact. It is
                        independent of legacy_int_abi: an object-bound Method
                        with a non-int return still needs its self slot. */
  sp_int legacy_int_abi; /* whether the target can ride the legacy sp_int-cast poly-call
                            path: 0 declines, 1 callable. A regular method rides when
                            its C return is a box-able kind recorded by legacy_ret
                            (Integer/String/Bigint/nil/bool/Symbol/typed array/user
                            object); a float, by-value struct, or unclassifiable
                            return declines. A synthesized __bam_ wrapper and the
                            typed-array adapters record their kind the same way. See
                            method_legacy_int_abi (#4395). */
  const char *legacy_sig; /* per-position C ABI type tokens, eight chars each, or NULL.
                             Each scalar kind (int/bool/symbol/nil) has its own token,
                             TY_UNKNOWN is the wildcard 0, and every other kind uses
                             100000 + TyKind (an object class encodes its class id in
                             TyKind), so a bool parameter never accepts an int and a
                             String parameter never accepts an IntArray. */
  sp_int legacy_fixed;  /* number of fixed positional C slots the signature fills */
  sp_int legacy_rest;   /* always 0 now: method_legacy_int_abi declines every rest
                           parameter (its trailing sp_PolyArray* has no slot in the
                           static cast), but the runtime gate keeps the rest-form
                           check for any future caller that stamps it */
  sp_int legacy_ret;    /* SP_BM_RET_*: how to box the sp_int C return */
  sp_int poly_abi;      /* whether the target's C signature is the promote poly
                           ABI -- `sp_RbVal fn([void *self,] sp_RbVal...)`:
                           every fixed parameter slot and the return are
                           TY_POLY, with the same structural declines as the
                           legacy classifier (rest/keyword/post-rest, an
                           explicit &blk, a leading _sp_cls). Stamped at bind
                           time by method_poly_abi (codegen_call.c); the
                           promote poly-call arms gate on it instead of
                           assuming every target is poly-signatured -- a
                           Float-parameter or String-returning target read the
                           sp_RbVal registers as garbage (and wasm, whose
                           indirect calls check the callee signature, traps). */
  sp_int poly_fixed;    /* number of fixed sp_RbVal slots the poly signature
                           reads; a call must pass exactly this count */
  sp_int thunk;         /* the per-target THUNK the bind site synthesized (#4542):
                           `sp_int fn(void *cap, sp_int argc, sp_int *args)`, the
                           proc-ABI shape of sp_method_proc_tramp itself, reading
                           every argument from the boxed side-channel, converting
                           each to the parameter's C type (TypeError otherwise),
                           filling omitted optionals from their defaults, packing
                           a rest, and publishing the boxed result. It covers the
                           signatures neither ABI stamp accepts (a Float, a
                           default below full arity, a rest, a mixed promote
                           signature); 0 when the bind site had no static target
                           or the target's shape declines (keywords). */
  sp_int thunk_min, thunk_max;   /* the argument counts it binds (max 16 for a rest) */
} sp_BoundMethod;
void sp_bm_cap_scan(void *p);
sp_int sp_method_proc_tramp(void *cap, sp_int argc, sp_int *args);
sp_Proc *sp_method_to_proc(sp_BoundMethod *m);
void sp_BoundMethod_scan(void *p);

/* The target's address as the Method stores it. On wasm the C compiler
   turns a direct call through a function pointer of another type into a
   thunk, and where it cannot convert an argument or the return (a bool
   against a pointer or an sp_int) the thunk is a trap; it sees such a call
   when it can prove which function the slot holds, which after inlining
   the constructor it can. The slot goes through memory the optimiser may
   not look through, and every call is the indirect one the ABI means, whose
   check is the wasm signature (all i32) and passes. Elsewhere the store is
   the store. */
static inline sp_int sp_bm_fn_opaque(sp_int fn) {
#if defined(__wasm__)
  volatile sp_int v = fn;
  return v;
#else
  return fn;
#endif
}
static inline sp_BoundMethod *sp_bound_method_new(void *self, sp_int self_kind, sp_int fn, const char *name, sp_int arity) { sp_BoundMethod *m = (sp_BoundMethod *)sp_gc_alloc(sizeof(sp_BoundMethod), NULL, sp_BoundMethod_scan); m->self = self; m->self_kind = self_kind; m->fn = sp_bm_fn_opaque(fn); m->name = name; m->arity = arity; m->desc = NULL; m->unbound = 0; m->recv_bound = 0; m->legacy_int_abi = 0; m->legacy_sig = NULL; m->legacy_fixed = 0; m->legacy_rest = 0; m->legacy_ret = SP_BM_RET_INT; m->poly_abi = 0; m->poly_fixed = 0; m->thunk = 0; m->thunk_min = 0; m->thunk_max = 0; return m; }
/* Tag a freshly-built Method with whether its target has the legacy sp_int C
   ABI, the per-position type signature, the fixed/rest slot counts, and how
   its sp_int C return boxes. The constructors default to 0 (unsafe), so every
   statically-known binding sets this before the Method can reach a poly slot
   (#4395). */
static inline sp_BoundMethod *sp_bm_set_abi(sp_BoundMethod *m, sp_int recv_bound, sp_int legacy_int_abi, const char *legacy_sig, sp_int legacy_fixed, sp_int legacy_rest, sp_int legacy_ret, sp_int poly_abi, sp_int poly_fixed) { m->recv_bound = recv_bound; m->legacy_int_abi = legacy_int_abi; m->legacy_sig = legacy_sig; m->legacy_fixed = legacy_fixed; m->legacy_rest = legacy_rest; m->legacy_ret = legacy_ret; m->poly_abi = poly_abi; m->poly_fixed = poly_fixed; return m; }
/* Stamp the bind site's thunk (0 for none) and the counts it binds. */
static inline sp_BoundMethod *sp_bm_set_thunk(sp_BoundMethod *m, sp_int thunk, sp_int tmin, sp_int tmax) { m->thunk = sp_bm_fn_opaque(thunk); m->thunk_min = tmin; m->thunk_max = tmax; return m; }
/* Whether a call passing `argc` arguments takes the thunk: a bound Method
   with one, the count within the side-channel's slots. A count the
   signature cannot bind is the thunk's own ArgumentError, in CRuby's words,
   so the range is not tested here. */
static inline sp_bool sp_bm_thunk_ok(sp_BoundMethod *m, sp_int argc) {
  return m && !m->unbound && m->thunk && argc <= SP_PROC_ARG_SLOTS;
}
/* Box the raw sp_int a legacy-ABI Method returned according to the Ruby return
   the bind site recorded. A plain Integer return is SP_BM_RET_INT; a String or
   Bigint return, an array-returning method, and a void/bool/Symbol return each
   box their own kind; a typed array adapter that returns self (push) or a
   laundered element (StrArray get/set) boxes the real value instead of
   mis-tagging the pointer as an Integer (#4395). A Bigint pointer boxes into
   its Ruby value. SP_BM_RET_INT is a plain Integer: a method whose return
   is a nullable Integer answers an sp_oint, two registers, which this
   one-register ABI cannot carry -- such a method is bound through the boxed
   (sp_RbVal) ABI instead. */
static inline sp_RbVal sp_bm_box_ret(sp_BoundMethod *m, sp_int raw) {
  sp_int r = m ? m->legacy_ret : SP_BM_RET_INT;
  switch (SP_BM_RET_KIND(r)) {
    case SP_BM_RET_STR:       return sp_box_str((const char *)(uintptr_t)raw);
    case SP_BM_RET_INT_ARRAY: return sp_box_nullable_obj((void *)(uintptr_t)raw, SP_BUILTIN_INT_ARRAY);
    case SP_BM_RET_STR_ARRAY: return sp_box_nullable_obj((void *)(uintptr_t)raw, SP_BUILTIN_STR_ARRAY);
    case SP_BM_RET_NIL:       return sp_box_nil();
    case SP_BM_RET_BOOL:      return sp_box_bool((raw & 0xff) != 0);
    case SP_BM_RET_SYM:       return (sp_sym)raw != (sp_sym)-1 ? sp_box_sym((sp_sym)raw) : sp_box_nil();
    case SP_BM_RET_OBJ:       return sp_box_nullable_obj((void *)(uintptr_t)raw, (int)(r >> 8));
    case SP_BM_RET_OBJ_DYN:   return sp_box_nullable_obj_dyn((void *)(uintptr_t)raw, 0);
    case SP_BM_RET_BIGINT:    return raw ? sp_box_bigint((sp_Bigint *)(uintptr_t)raw) : sp_box_nil();
    default:                  return sp_box_int(raw);
  }
}
/* The raw register as the trampoline hands it back untouched to a typed
   caller: a bool callee wrote only the low byte, so normalise it to 0/1 there;
   every other kind is already the value (an int, a Symbol id, a pointer). */
static inline sp_int sp_bm_norm_ret(sp_BoundMethod *m, sp_int raw) {
  return (m && SP_BM_RET_KIND(m->legacy_ret) == SP_BM_RET_BOOL) ? ((raw & 0xff) != 0) : raw;
}
/* Mark a statically-built instance_method/#unbind result as an UnboundMethod,
   so a later dynamic .call/[] through a container sees m->unbound and raises
   instead of invoking the instance C function with no self (#4395). */
static inline sp_BoundMethod *sp_bm_set_unbound(sp_BoundMethod *m) { if (m) m->unbound = 1; return m; }
/* The scalar-kind ABI token of a boxed argument (see abi_sig_token in
   codegen_call.c: TY_INT/TY_BOOL/TY_SYMBOL/TY_NIL are 1/2/3/4), or 0 when the
   value has no scalar sp_int slot at all (a pointer, Float, or Bigint). The
   generic Method trampoline and the poly spread path read the same encoding as
   the statically-typed sp_bm_legacy_abi_ok. */
static inline sp_int sp_bm_boxed_scalar_token(sp_RbVal e) {
  switch (e.tag) {
    case SP_TAG_INT:  return 1;   /* TY_INT */
    case SP_TAG_BOOL: return 2;   /* TY_BOOL */
    case SP_TAG_SYM:  return 3;   /* TY_SYMBOL */
    case SP_TAG_NIL:  return 4;   /* TY_NIL */
    default:          return 0;   /* pointer/Float/Bigint: no scalar slot */
  }
}
/* Whether a boxed argument with scalar token `code` fits the signature's slot
   at position `i`, under the same rule as sp_bm_sig_pos_match: an exact
   scalar-kind match, or the TY_UNKNOWN 0 wildcard on the parameter side. A
   code of 0 (a value with no scalar slot) never fits. */
static inline sp_bool sp_bm_sig_pos_scalar_ok(sp_int code, const char *sig, sp_int i) {
  if (!sig || code == 0) return FALSE;
  sp_int want = 0;
  for (int k = 0; k < 8; k++) want = want * 10 + (sig[8 * i + k] - '0');
  return want == code || want == 0;
}
/* Whether a call passing `argc` arguments whose per-position ABI type tokens
   (eight chars each; see abi_sig_token in codegen_call.c) are `arg_sig` can
   ride the target's legacy sp_int ABI.
   The count must fit the target's fixed signature and every fixed position's
   C type must match exactly: a bool parameter is never handed an int, a
   String* is never handed an IntArray*, and an sp_Foo* is never handed an
   sp_Bar*. An optional parameter is only safe at full arity (the C signature
   reads every fixed slot), so a shorter call declines. Declines a non-callable
   target, an unbound Method, and a non-int return (legacy_int_abi == 0). */
static inline sp_bool sp_bm_sig_pos_match(const char *a, const char *b) {
  if (memcmp(a, b, 8) == 0) return TRUE;
  /* 0 is the TY_UNKNOWN wildcard: it accepts any scalar-kind token, and a
     scalar-kind arg accepts an unknown parameter. Pointer tokens carry the
     100000 offset, so they never match the wildcard. */
  sp_int va = 0, vb = 0;
  for (int i = 0; i < 8; i++) { va = va * 10 + (a[i] - '0'); vb = vb * 10 + (b[i] - '0'); }
  if (va == 0) return vb < 100000;
  if (vb == 0) return va < 100000;
  return FALSE;
}
/* Whether a call passing `argc` arguments can ride the target's promote poly
   ABI (every slot an sp_RbVal): the stamp must be set, the Method bound, and
   the count exactly the fixed slot count the C signature reads. */
static inline sp_bool sp_bm_poly_abi_ok(sp_BoundMethod *m, sp_int argc) {
  return m && !m->unbound && m->poly_abi && argc == m->poly_fixed;
}
static inline sp_bool sp_bm_legacy_abi_ok(sp_BoundMethod *m, sp_int argc, const char *arg_sig) {
  if (!m || m->unbound || !m->legacy_int_abi || !m->legacy_sig || !arg_sig) return FALSE;
  if (m->legacy_rest) { if (argc < m->legacy_fixed) return FALSE; }
  else if (argc != m->legacy_fixed) return FALSE;
  sp_int n = argc < m->legacy_fixed ? argc : m->legacy_fixed;
  for (sp_int k = 0; k < n; k++)
    if (!sp_bm_sig_pos_match(arg_sig + 8 * k, m->legacy_sig + 8 * k)) return FALSE;
  return TRUE;
}
static inline sp_BoundMethod *sp_bound_method_new_d(void *self, sp_int self_kind, sp_int fn, const char *name, sp_int arity, const char *desc) { sp_BoundMethod *m = sp_bound_method_new(self, self_kind, fn, name, arity); m->desc = desc; return m; }
/* Method#unbind on a BOXED method: the same target with the receiver dropped.
   A boxed value carries no syntax for the compile-time unbound rendering, so
   the copy records it (the #class arm reads `unbound`) (#3692). */
static inline sp_BoundMethod *sp_bm_unbind(sp_BoundMethod *m) {
  if (!m) return NULL;
  sp_BoundMethod *u = sp_bound_method_new(NULL, SP_BM_SELF_NONE, m->fn, m->name, m->arity);
  u->desc = m->desc;
  u->unbound = 1;
  u->legacy_int_abi = m->legacy_int_abi;
  u->legacy_sig = m->legacy_sig;
  u->legacy_fixed = m->legacy_fixed;
  u->legacy_rest = m->legacy_rest;
  u->legacy_ret = m->legacy_ret;
  u->poly_abi = m->poly_abi;
  u->poly_fixed = m->poly_fixed;
  u->thunk = m->thunk;
  u->thunk_min = m->thunk_min;
  u->thunk_max = m->thunk_max;
  return u;
}

#endif
