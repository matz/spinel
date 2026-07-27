/* Whole-program type inference (the analyzer pass).
 *
 * Populates the per-node type cache and the local-variable type table in
 * the Compiler. Mirrors the legacy analyzer's role but shares state with
 * codegen in memory instead of via an IR file. infer_type is also the
 * type query codegen uses (through the cache it fills here).
 */
#ifndef SPINEL_ANALYZE_H
#define SPINEL_ANALYZE_H

#include "compiler.h"

/* Set by main.c from --int-overflow=promote. In promote mode the analyzer is
   free to widen accumulating int locals to bigint more aggressively (e.g. block
   iteration loops, not just `while`), since the overflow-raising int macros are
   exactly what promote mode is asking us to avoid. Off (0) for raise/wrap, so
   the default gates and optcarrot (which pins wrap) see no behavior change. */
extern int g_promote_mode;

/* One post-convergence bind pass fills UNKNOWN params from empty
   array-literal args (fst([]) with def fst(a) = a.first). */
extern int g_final_bind_pass;

/* Unified value type of a slot's `recv[k] = v` writes; nwrites (optional)
   reports how many contributed, so a caller can tell "no evidence" from
   "evidence not derived yet". Defined in analyze_pass.c. */
TyKind aset_value_type_ex(Compiler *c, int recv, int *nwrites);
TyKind local_aset_key_type(Compiler *c, Scope *sc, const char *name, int *nwrites);

/* Run inference over the whole program: register locals, reach a fixpoint
   on their types, and fill the node type cache. */
void analyze_program(Compiler *c);

/* Infer (and cache) the type of node `id`. Used during analysis; codegen
   reads the cached results via comp_ntype. */
TyKind infer_type(Compiler *c, int id);

/* Re-infer every node of the subtree at `id` (children first), refreshing the
   whole type cache under the CURRENT scope-local types. The shadow-typing
   emitters (a block param pinned to the receiver's element type for the body's
   emission) need this: infer_type alone does not descend into call ARGUMENTS
   -- a call's type is its callee's return -- so an argument that reads the
   re-typed param kept its stale widened type and was passed unboxed. Stops at
   nested defs/classes (their locals are outside the shadow). */
void infer_subtree(Compiler *c, int id);

/* Unified type of every value-carrying `break`/`next` in a block body (not
   descending into nested blocks/loops), or TY_UNKNOWN if none. Lets a
   collecting emitter widen its element type past the tail expression so a
   `next <other-type>` is boxed rather than assigned to a mismatched temp. */
TyKind ie_block_break_next_ty(Compiler *c, int node);

/* True if CallNode `id` is an Enumerable method on a Range that spinel does not
   handle natively but supports on arrays -- served by materializing the range
   to an int array (both inference and codegen then treat the receiver as an int
   array). Excludes range-native methods (each/map/select/sum/min/count-no-arg). */
int range_enum_redispatch(Compiler *c, int id);
int hash_enum_redispatch(Compiler *c, int id);
int reduce_tail_from_acc(Compiler *c, int tail, const char *accp);

/* True if `node` (a block body / statements subtree) contains a top-level
   `break` that binds to the enclosing block -- i.e. not captured by a nested
   loop or block-bearing call. */
int block_has_top_break(Compiler *c, int node);
/* True if CallNode `id` takes a literal block whose body has a top-level
   break and is an inlined iterator the break wrapper should catch (a real
   receiver, not instance_exec/eval). When true, the call returns the break
   value on break, so its result type widens to poly. */
int call_breaks(Compiler *c, int id);
/* Scope of an inline-able yielding user method a block-bearing CallNode
   resolves to (its literal block is spliced at yield sites), else -1. */
int call_user_yield_mi(Compiler *c, int id);
/* True if scope `scope_idx` contains an explicit `return`. */
int scope_has_return(Compiler *c, int scope_idx);
/* When set, the call_breaks override in infer_call is suppressed so the
   wrapper can compute the call's normal (no-break) result type. */
extern int g_infer_ignore_brk;
extern int g_ret_no_new_poly;
/* Recompute a node's type without consulting the cache (used by the break
   wrapper with g_infer_ignore_brk set to recover the normal result type). */
TyKind infer_uncached(Compiler *c, int id);
/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx);
/* Name of a block's trailing rest parameter (`|*a|`), or NULL. */
const char *block_rest_name(Compiler *c, int block);
const char *block_opt_name(Compiler *c, int block, int idx);
const char *block_post_name(Compiler *c, int block, int idx);
int block_rest_marker(Compiler *c, int block);
const char *block_kwrest_name(Compiler *c, int block);
int block_opt_default(Compiler *c, int block, int idx);
const char *block_keyword_name(Compiler *c, int block, int idx);
int block_keyword_default(Compiler *c, int block, int idx);

/* True if `id` is a `proc {}` / `lambda {}` / `Proc.new {}` literal (a CallNode
   whose block becomes its own lowered proc fn), or the `Proc` constant. Declared
   here (not analyze_internal.h) so codegen can distinguish an inlined iteration
   block from a nested proc/lambda literal. */
int is_proc_constant(const NodeTable *nt, int n);
int is_proc_literal(Compiler *c, int id);

/* Element type an `each_with_object([])` accumulator is filled with, inferred
   from how the memo param is pushed to (following a forwarded callable's body).
   TY_UNKNOWN when undetermined; callers default an empty `[]` to int_array. */
TyKind ewo_memo_elem_type(Compiler *c, int callid);

/* For a curry-application node, whether it completes the curry (reaches the base
   proc's arity) and the proc's return type. Returns 1 for a recognized chain. */
int curry_apply_info(Compiler *c, int node, int *out_complete, TyKind *out_ret);

/* Class index when a receiverless instance_eval/exec resolves to self, else -1. */
int ie_implicit_self_class(Compiler *c, int id);

/* instance_exec keyword-arg helpers: the call's trailing KeywordHashNode (or
   -1), and the value node bound to a keyword name within it (or -1). */
int ie_call_kwhash(Compiler *c, int id);
int ie_kwhash_value(Compiler *c, int kwhash, const char *name);

/* instance_exec trampoline body-arg resolution (mixed local/ivar/literal args):
   effective arg count, and the node to bind/emit for the p-th block param
   (caller arg substituted for a trampoline param read). -1 to bail. */
int ie_tramp_effective_argc(Compiler *c, int caller_id);
int ie_tramp_effective_arg(Compiler *c, int caller_id, int p);

/* Returns 1 if the idx-th required param is a MultiTargetNode (tuple destructure). */
int block_param_is_multi(Compiler *c, int block, int idx);

/* Returns the number of leaves in the MultiTargetNode at requireds[idx]. */
int block_param_multi_count(Compiler *c, int block, int idx);

/* Returns the name of the leaf_idx-th leaf inside the MultiTargetNode at requireds[idx]. */
const char *block_param_multi_leaf(Compiler *c, int block, int idx, int leaf_idx);

/* Bound-Method (`method(:sym)`) resolution, shared with codegen. */
const char *method_sym_arg(Compiler *c, int node);   /* :sym arg name, or NULL */
int is_method_obj_call(Compiler *c, int node);        /* is node a method(:sym) call? */
int method_obj_target_mi(Compiler *c, int node);      /* target method scope idx, or -1 */
int method_recv_node(Compiler *c, int recv);          /* the method(:sym) node behind a Method expr */
int method_expr_is_unbound(Compiler *c, int recv);    /* instance_method with no #bind crossed */
int class_is_blank_slate(Compiler *c, int ci);        /* explicit `< BasicObject` descent */
int proc_to_proc_method_node(Compiler *c, int recv); /* the method(:sym) node behind <method>.to_proc */
int method_call_param_shift(Compiler *c, int mn, int mi); /* 1 when self carries param[0] (__bam wrapper) */

#endif
