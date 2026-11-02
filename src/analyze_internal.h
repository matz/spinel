/* Internal shared declarations for the split analyzer (was one 8.4k-line
 * file): util (helpers) / infer (type inference) / scope (registration &
 * specialization) / pass (fixpoint passes) / analyze.c (driver). */
#ifndef SPINEL_ANALYZE_INTERNAL_H
#define SPINEL_ANALYZE_INTERNAL_H
#include "analyze.h"
#include "value_pos.h"
#include "ffi_spec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The receiver class for a node inside an instance_eval/exec block, or -1. */
/* codegen_util.c's refusal, called from the analyzer too: declared here so
   no analyzer TU calls it undeclared (an implicit `int` prototype, which a
   wasm link reports as a signature mismatch and an LP64 link hides) */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg);
void unsupported_feature_collect(Compiler *c, int id, const char *msg);
int defer_refusals(void); /* codegen_util.c: explicit opt-in to runtime refusals */
int defer_refusals_at(const Compiler *c, int node); /* ...or the node's file's `# spinel: defer-refusals` */
/* (codegen_util.c) a refusal kept as a check where its value arrives,
   reported and counted as a deferred one; and whether the identity Set's
   proof marked any node for that check or a refusal where it is emitted
   (refuse_set_identity_members), with the words they raise */
void refusal_note_deferred(Compiler *c, int id, const char *msg);
extern int g_idset_marks;
extern const char *const g_sid_refusal_msg;
int ie_class_of(Compiler *c, int node);
int attr_reader_ty(Compiler *c, int cid, const char *name, TyKind *out);
int ie_poly_classes_at(Compiler *c, int node, int *out, int max);
/* an empty row a nested seed built at its kind (narrow_object_arrays) */
int oa_seed_row_literal(int lit);
/* Does the program make a bare Object (`Object.new`)? (analyze_infer.c) */
int an_program_news_object(Compiler *c);
int ie_kernel_global(const char *n);

/* Forward declarations for FFI helpers defined later in this file. */
const char *ffi_arg_str(const NodeTable *nt, int nid);
int ffi_arg_int(const NodeTable *nt, int nid);
TyKind ffi_spec_to_ty(const char *spec);
int ffi_find_func(Compiler *c, const char *mod, const char *name);
/* `Fiber.new { }`'s block, or -1 */
int an_fiber_new_block(Compiler *c, int v);
/* --share-strings: can boxed or untyped receiver r be a String
   (analyze.c)? `lits` is the asker's: bound() says how a literal block or
   a lambda of scope `scope` binds variable `name`: 0 not at all, 1 only as
   Enumerator.new's yielder, 2 another way (analyze_share.c keeps it).
   ivar(), when set, says whether instance variable `name` (with its `@`)
   may hold a String, by everything that writes one of that name; the
   receiver an ivar's read or an attribute reader's call is answers by
   it. */
typedef struct PolyLits {
  int (*bound)(void *ctx, Compiler *c, int scope, const char *name);
  void *ctx;
  int (*ivar)(void *ctx, Compiler *c, const char *name);
} PolyLits;
int an_recv_may_be_string(Compiler *c, int r, const PolyLits *lits);
/* Can value v be a String, as an_recv_may_be_string reads a variable's
   written value (by its type, or its form with `lits`)? */
int an_value_may_be_string(Compiler *c, int v, const PolyLits *lits);
/* A noncaptured, nonparameter local with fresh writes, read nowhere else. */
int an_local_read_once(Compiler *c, int n);
int ffi_find_buf(Compiler *c, const char *mod, const char *name);
int ffi_find_reader(Compiler *c, const char *mod, const char *name);
int ffi_find_writer(Compiler *c, const char *mod, const char *name);


/* Re-entrancy guard for yield_value_type: prevents infinite recursion when a
   recursive method forwards its block to itself (e.g. countdown { blk.call }). */
#define MAX_YVT_DEPTH 32
extern int g_yvt_mi[MAX_YVT_DEPTH];
extern int g_yvt_depth;

/* Temporary class override for instance_eval block body inference.
   -1 = no override; set to the receiver's class_id while inferring
   the block body so that InstanceVariableReadNode uses the right class. */
extern int an_ie_class_id;


/* method(:sym) helpers (defined in analyze_util.c, shared with codegen) */
const char *method_sym_arg(Compiler *c, int node);
int is_method_obj_call(Compiler *c, int node);
int method_obj_target_mi(Compiler *c, int node);
int method_recv_node(Compiler *c, int recv);
int method_recv_nodes(Compiler *c, int recv, int **out);
int method_expr_is_unbound(Compiler *c, int recv);
int proc_to_proc_method_nodes(Compiler *c, int recv, int **out);
int local_sole_range_node(Compiler *c, int recv);
int const_array_elems_all_int_array(Compiler *c, const char *cname);
int *an_parent_map(const NodeTable *nt);
const int *comp_parent_map(Compiler *c);
/* The definite-assignment walk (analyze.c), shared with the nil facts
   (analyze_nil.c): the program's parent map, each statement's place in its
   list (filled one list at a time, -1 for a node not in its parent's list),
   whether a read of local `nm` can run before any write of it, and the
   per-list memo the walk keeps, freed once a pass is done with it. */
typedef struct { int *pos; char *done; } DUPos;
int *du_parent_map(const NodeTable *nt);
int du_read_maybe_unset(const NodeTable *nt, const int *par, DUPos *dp, int rd, const char *nm);
void du_memo_free(void);
/* The edges an_value_dropped (and cow_user_block_value, which asks of the
   block it ends at) climbs: where a value is passed on to the statement or
   the block that holds it. */
enum { AN_DROP_CLIMB = VPS_ALL };
int an_value_dropped(const NodeTable *nt, const int *parent, int node);
int cow_user_block_value(Compiler *c, const int *parent, int id);
/* The methods the default build may lend a String parameter's slot by
   address (elig, c->nscopes entries), and whether it passes parameter pi
   of method mi by value: no slot and no shared handle reach it, because a
   member of its name group may not take either. A spliced yielder's
   parameters alias the caller's String, and are not passed by value.
   analyze.c */
void an_byref_eligible_scopes(Compiler *c, char *elig);
/* A runtime protocol name: the emitted program can call the method with
   no CallNode of its own (`puts obj` calls to_s). analyze.c */
int method_name_implicitly_invoked(const char *nm);
int local_all_writes_empty_hash(Compiler *c, Scope *sc, const char *name);
int local_all_writes_empty_hash_or_new(Compiler *c, Scope *sc, const char *name);
int method_call_param_shift(Compiler *c, int mn, int mi);
void mark_path_builders(Compiler *c);   /* analyze_desugar.c */

extern int g_inc_did_clone;
extern int *g_ie_node_class;
/* The local whose write's value infer_write_types is typing, or NULL: a read
   of it there is the write reading itself (`t = t.alias(n)`). analyze.c */
extern LocalVar *g_infer_self_write;

typedef struct { const char **v; int n, cap; } ANameSet;
#define QC_MAXDEPTH 8
/* salt: leading underscores of the qualified name, for a class whose plain
   qualified name is one the program's own class already has. head: the
   name of the parent a compact header names and the passes cannot place,
   or "". */
typedef struct { int node; char path[QC_MAXDEPTH][64]; int depth; char name[128]; int salt; char head[64]; } QCWrite;

/* ---- cross-part function declarations (generated by the split) ---- */
int is_builtin_class_name(const char *n);
void refuse_unreachable_bare_constants(Compiler *c);
int is_builtin_module_name(const char *n);
int const_path_in_closed_builtin(Compiler *c, int id);
int const_path_misses_toplevel(Compiler *c, int id);
int builtin_module_at(int i, const char **name, int *id);
int is_builtin_exception_name(const char *n);
const char *superclass_builtin_exc_name(const NodeTable *nt, int sc);   /* analyze_util.c */
int is_syserr_family_name(const char *n);           /* analyze_util.c */
int builtin_method_known(const char *cls, const char *m);
int builtin_method_known_any(const char *m);   /* codegen_call.c */
int builtin_object_method_known(const char *m);   /* builtin_names.c */
int core_method_vis(const char *cls, const char *m, int inherit);   /* codegen_call_class.c */
const char *core_class_ancestors(const char *cls);   /* codegen_call_class.c */
int builtin_method_arity(const char *cls, const char *m, int *out);
int builtin_cmethod_known(const char *cls, const char *name);
int builtin_instance_method_known(const char *cls, const char *m);   /* codegen_call.c */
int builtin_super_cmethod_known(const char *cls, const char *name);
int builtin_method_names(const char *cls, const char **out, int cap);
int builtin_name_arity_span(const char *name, int with_block, int *lo, int *hi);
int builtin_recv_arity_span(const char *name, int *lo, int *hi);
int builtin_recv_owner_classes(const char *name, const char **out, int cap);
int poly_splat_keyed(const char *name);
int an_bi_generic_splat(Compiler *c, int id);
int builtin_kernel_fn_span(const char *name, int with_block, int *lo, int *hi);
int builtin_arity_violation(Compiler *c, int id);
int is_handler_proc_block(Compiler *c, int id);
int builtin_class_id(const char *name);
int class_is_prim_reopen(Compiler *c, int k);
int stdlib_name_from_source(const char *n);
int builtin_class_parent_id(int id);
int desugar_builtin_reopen_named_superclass(Compiler *c);
int desugar_builtin_reopen_self_class(Compiler *c);
int desugar_time_singleton_bare_ctor(Compiler *c);
int desugar_builtin_reopen_self_cmethods(Compiler *c);
const char *class_ruby_name(Compiler *c, int ci); /* codegen.c */
int builtin_object_method_known(const char *m);
int core_method_name(const char *n);   /* analyze_desugar.c: a core class's public method */
int object_public_method_name(const char *n); /* analyze_desugar.c: one of Object's public instance methods */
int object_instance_method_name(const char *n);        /* analyze_desugar.c: one of Object's instance methods, public or private */
int class_inherits_builtin_exception(Compiler *c, int ci);
/* 1 if super node n, in instance method mi `inspect` / `to_s` of a plain
   class, reaches Kernel's, which builds a new String each call. */
int an_super_object_text(Compiler *c, int n, int mi);
int an_user_defines_or_reads(Compiler *c, const char *name);
/* Defined in codegen_util.c: does a value of builtin type t answer `is_a?(cn)` (1), not (0), or
   only at run time (-1); exact is the instance_of? form. Reached by name, as codegen_internal.h is
   not included here. */
int ty_matches_class(Compiler *c, TyKind t, const char *cn, int exact);
/* The universal "what a receiver answers" table (analyze_infer.c) and the
   lookup its consumers use. See the note on AN_POLY_RAW. */
struct an_poly_raw_row { const char *n; int ac; TyKind t; };
extern const struct an_poly_raw_row AN_POLY_RAW[];
int an_poly_raw_argc(const char *name);
/* 1 when a user class defines `name` as an instance method whose return is not
   `want`: the poly dispatch accumulates its arm beside the builtin ones in one
   C temp, so the call must be typed for what both can hold. */
int an_user_ret_disagrees(Compiler *c, const char *name, TyKind want);
TyKind an_class_concrete(Compiler *c, int id, const char *name, TyKind t);
int an_ty_holds_nil(TyKind t);
int gvar_seeded_before_read(Compiler *c, const char *gname);
int an_empty_container_kind(Compiler *c, int b);
int an_literal_int_rows(Compiler *c, int arr);
int an_empty_container_disagrees(int kind, TyKind other);
int an_or_empty_hash_fallback(Compiler *c, int node);
int an_chunk_family_to_a(Compiler *c, int id);
const char *an_regex_lit_src(Compiler *c, int nid);
int str_in(const char *s, const char *const *set);
/* array element facts analyze.c's Integer/Float Array marks and the nil
   fact's pointer Array marks (analyze_nil.c) share */
int elem_preserving_call(const char *nm);
int slice_read_call(Compiler *c, int call);
int mutated_array(Compiler *c, int recv);
int array_mutation_stores(Compiler *c, int call, int strict, int *from, int *to, int *elems);
int elem_block_params(Compiler *c, int call, int *recv_out);
int nullable_elem_ivar_in(Compiler *c, int cid, const char *nm, ClassInfo **out);
int blk_locals_have(const char *locals, const char *nm);
/* The calls named `name`, ascending: for (id = an_calls_named_first(c, nm);
   id >= 0; id = an_calls_named_next(id)). Check each node as before. */
int an_calls_named_first(Compiler *c, const char *name);
int an_calls_named_next(int id);
/* The calls below n0 named any of names (NULL-terminated), ascending, into
   *out, which the caller frees: their count. A pass that acts only on calls
   of those names, and renames no call it has not reached yet, walks these
   instead of every node of the table, and reaches the calls it would. */
int an_calls_named_any(Compiler *c, const char *const *names, int n0, int **out);
/* The same for the calls whose name pred accepts. */
int an_calls_named_if(Compiler *c, int (*pred)(const char *), int n0, int **out);
/* The same for the calls passed a block argument (`&x`). */
int an_calls_with_block_arg(Compiler *c, int n0, int **out);
/* The local variable writes named `name`, ascending, the same way. */
int an_local_writes_named_first(Compiler *c, const char *name);
int an_local_writes_named_next(int id);
/* The calls that can name method m, ascending (the caller frees the list):
   its own name, `new` for an initialize, and the alias names whose chain
   leads to it. Each still has to be resolved; the list only narrows. */
int *an_scope_call_candidates(Compiler *c, Scope *m, int *n);
int is_arith_op(const char *op);
int node_is_empty_container(Compiler *c, int node);
int bind_coerce_operator_params(Compiler *c);
int is_cmp_op(const char *op);
int is_numeric_coerce_op(const char *op);
int class_coerce_emittable(Compiler *c, int k);
int class_has_coerce_shape(Compiler *c, int k);
int class_has_to_str_shape(Compiler *c, int k);
int is_eq_op(const char *op);
int is_void_call(const char *name);
/* The enclosing scope's own chain owns this receiverless call's name, so no
   Kernel arm may claim it (the codegen twin is bare_call_class_owned). */
int an_bare_call_class_owned(Compiler *c, int id);
/* Resolve a struct member from a literal key node: a SymbolNode names a
   member; an IntegerNode is a positional index. Returns the member index
   (0-based, matching ivar order) or -1. */
int struct_member_idx(Compiler *c, ClassInfo *sc, int keynode);
int struct_member_idx_float(Compiler *c, ClassInfo *sc, int keynode);
/* A Struct `[]=` (analyze_scope.c): 1 for call id on a receiver typed as
   a class (*cls; only a key no literal names is left a `[]=` there), 2 on
   a boxed one, which reaches the classes poly_recv_classes lists, 0 for no
   such call. struct_aset_members: 1 when class k's `[]=` is the Struct's
   own (no `[]=` of the program's; cplan_struct_aset's), with the members
   [*lo, *hi) the key can name (none for a literal naming no member: it
   raises). */
int struct_aset_receiver(Compiler *c, int id, int *cls);
int struct_aset_members(Compiler *c, int id, int k, int *lo, int *hi);
/* Can the `[]=` that node id makes on its receiver (a call, an index
   op-write, an index target) store into a Struct's member: a receiver typed
   as such a Struct, or a box that can hold one (every box whose classes the
   analysis cannot bound, when the program has one)? */
int struct_aset_may_reach(Compiler *c, int id);
/* A read of a String slot the share rule holds as the handle (analyze.c) */
int an_arg_is_shared_handle(Compiler *c, int node);
/* Last statement of a scope's body, or -1. */
int scope_body_last(Compiler *c, int mi);
int scope_block_given_tail(Compiler *c, Scope *sc);
int scope_block_given_else_last(Compiler *c, Scope *sc);
/* ...or the begin a def's own rescue makes its body (analyze_util.c). */
int scope_body_value(Compiler *c, int mi);
/* The expressions whose value method scope mi answers (its body's and each
   `return`'s; see analyze_pass.c). Answers the count, or -1. */
int method_value_leaves(Compiler *c, int mi, int *out, int cap);
int method_value_leaves_or_nil(Compiler *c, int mi, int *out, int cap);
int method_value_leaves_or_nil_narrow(Compiler *c, int mi, int *out, int cap);
int block_given_tail_then_last(Compiler *c, int last);
int super_forwards_caller_block(Compiler *c, int id);
/* 1 if `node` is `<&block-param>.call(...)` / .() / [] for method mi -- the
   explicit-call equivalent of `yield`, inlined the same way. */
int is_blk_param_call(Compiler *c, int node, int mi);
/* 1 if `node` is an instance_eval / instance_exec handing the method's own block on
   as the value the method ends with: inlined, it answers each site's block value. */
int ie_forwards_block_tail(Compiler *c, int node);
/* 1 if the `return` is written in a lambda, which it leaves instead of the method. */
int return_in_lambda(Compiler *c, int rid);
/* 1 if scope `si` returns, other than at `guard_ret`, before it. */
int return_before(Compiler *c, int si, int guard_ret);
TyKind pinned_guard_unpin_ret(Compiler *c, int mi);
/* A proc-typed local's proc_ret: the join of the answers of the procs it is written. 1 on a change. */
int join_local_proc_rets(Compiler *c);
/* Scan a subtree for BreakNode values and return their unified type.
   Stops at DefNode/BlockNode boundaries (inner blocks have their own break scope). */
TyKind scan_break_type(Compiler *c, int id, int depth);
/* Unify the value types of every `throw <tag>, <val>` inside a catch block.
   A nested inner BlockNode is still scanned because `throw` is dynamic. */
TyKind scan_throw_type(Compiler *c, const char *tag);
/* The value type of `yield` / a `<&block-param>.call` inside method mi: the
   block-body value type at a (any) call site of mi. Polymorphic, resolved from
   the first matching caller -- matches how the rewrite inlines per call site. */
TyKind yield_value_type(Compiler *c, int mi);
/* Tail expressions of the blocks reaching `mi`'s yield (see analyze_util.c). */
int yield_block_tails(Compiler *c, int mi, int *out, int max);
/* The calls whose literal block reaches `mi`'s yield, or -1 when not all
   are known (see analyze_util.c). */
int yield_block_calls(Compiler *c, int mi, int *out, int max);
/* Set: yield_block_tails answers a literal block's `next v` too. */
extern int yield_block_tails_next;
/* The block value reaching `mi` from a child's `super` (see analyze_util.c). */
TyKind yield_value_type_via_super(Compiler *c, int mi);
int method_hands_block_tail(Compiler *c, int mi);   /* mi ends in a super handing its block to a yield-tailed chain */
/* Whether a block-passing call hands on its enclosing method's own block
   (`...`, `&` of the block parameter) rather than a proc value of its own. */
int call_forwards_own_block(Compiler *c, int cid);
int yield_value_diverges(Compiler *c, int mi);
/* Is node `id` the value scope `mi` returns by falling off its end? */
int node_is_scope_tail(Compiler *c, int mi, int id);
TyKind yield_aware_elem_ty(Compiler *c, int node);
int an_user_defines_method(Compiler *c, const char *name);
int an_enum_own_find_index(Compiler *c);
/* Can no method of the program take `name`: no def of it or of method_missing, and no alias, define_method, attr_* or Struct / Data member that names it (or names a method only the run time knows)? */
int an_name_binder_free(Compiler *c, const char *name);
int an_native_defines_method(Compiler *c, const char *name);
int an_user_recv_defines_method(Compiler *c, const char *name);
int an_user_define_method(Compiler *c);
extern int g_yvt_unify_all;
/* The return type of a call to method `mi`. A method whose body is just a
   bare `yield` returns the block's value -- and since it inlines per call
   site, use THIS site's block value type rather than the unified return. */
TyKind method_call_ret(Compiler *c, int mi, int call_id);
/* A program's own Object method serving a call on a builtin receiver of
   class `cls` that does not have the name itself (see analyze_infer.c). */
int object_reopen_answers(Compiler *c, const char *cls, int call_id, TyKind *out);
int sclass_value_call(Compiler *c, int id);   /* analyze_infer.c */
/* is_proc_constant / is_proc_literal are declared in analyze.h (codegen needs them). */
/* 1 if `id` is any proc-creating literal: a proc/lambda/Proc.new call (above)
   or a `->(){}` LambdaNode. */
int is_proc_create(Compiler *c, int id);
int local_proc_literal_param_of(Compiler *c, Scope *sc, const char *nm);
int proc_literal_calls_in_sight(Compiler *c, int lit);
int proc_literal_uncalled(Compiler *c, int lit);
int proc_lit_carrier(Compiler *c, int v, int lit);
int widen_hash_arg_for_store(Compiler *c, int arg, TyKind hk, TyKind hv);
int pivs_settle_hash_stores(Compiler *c);
int pivs_hash_stores_widened(Compiler *c);
/* A builtin iterator moved onto its copy, `__enum_each_with_index__3(recv) { }`
   (desugar_builtin_enum_calls), iterates its first argument as `recv.each_with_index { }`
   does: the iterator's name in buf, 1 for such a call name. */
int pivs_enum_iter(const char *name, char *buf, size_t n);
int subtree_has_side_effect(Compiler *c, int id);   /* codegen_util.c */
int call_unused_block(Compiler *c, int id);   /* codegen_util.c */

/* Shared cached local-write index (analyze_pass.c): bucket walk over
   (scope, name) for "every write of local X in scope S" queries, instead of
   a whole-table rescan per query. Callers must still confirm scope/name
   (hash collisions) and node kind on each record. */
int lw_shared_first(Compiler *c, const char *name, int scope);
int lw_shared_node(int rec);
int lw_shared_next(int rec);
/* Nil narrowing (analyze.c): the facts for a round of the nullable marking,
   and the start of a type-inference round, whose facts are computed when
   first asked. */
void nn_compute(Compiler *c, int round);
void nn_inference_round(Compiler *c);
/* Shared cached ivar-write index (analyze_pass.c): the ivar analogue of the
   local-write index, keyed by ivar name only. Callers confirm name (hash
   collisions), node kind, and defining class on each record. */
int ivw_shared_first(Compiler *c, const char *name);
int ivw_shared_node(int rec);
int ivw_shared_next(int rec);
/* The body return type of a proc-creating node (proc/lambda CallNode literal,
   or a LambdaNode). The last statement of the block/lambda body is the value. */
TyKind proc_node_ret(Compiler *c, int create);
/* The body return type (`#call`'s result) of a proc-valued expression, or
   TY_UNKNOWN if not statically known. Resolves a literal directly, a local's
   recorded proc_ret, and a method call's recorded ret_proc_ret. */
TyKind proc_ret_of(Compiler *c, int node);
/* The return type of a proc-typed expression's `.call`; poly when unknown. */
TyKind proc_call_ret(Compiler *c, int recv);
TyKind infer_call(Compiler *c, int id);
TyKind infer_uncached(Compiler *c, int id);
void scope_add_param(Scope *s, const char *name, int defnode);
void collect_def_params(Compiler *c, int def_id, Scope *s);
char *dm_lit_str(Compiler *c, int lit);
char *dm_eval_name(Compiler *c, int node, const char *bv, int lit);
TyKind dm_lit_type(Compiler *c, int lit);
const char *builtin_class_of_type(TyKind t);
int object_fallback_class_of_type(TyKind t);
/* Whether method `m` can take call `call_id`'s arguments (analyze_pass.c) */
int method_takes_call_args(Compiler *c, Scope *m, int call_id);
const char *resolve_class_alias(Compiler *c, const char *cname);
const char *resolve_class_alias_chain(Compiler *c, const char *cname);
const char *const_alias_source(Compiler *c, const char *cname);
void walk_scope(Compiler *c, int id, int scope_idx, int class_id);
/* Where the load phase stands at a node (analyze_elab.c): the statement it
   is in (e, its seq; -1 where no record is made) and how that statement
   runs -- its file, the top-level statement holding it, the statement whose
   body opened its class frame (-1: main's), its nesting for the dump,
   whether it may not run or may run partly (may, partial), the statement
   around it whose body may run again (rep, LrStmt.rep) -- and what the
   node is to it (kind, EP_*). A statement's node and the nodes on its path
   to the bodies it runs (EP_SPINE: a branch, a rescue clause, a written
   value's call, a block run at once) lead to those bodies: a statement
   list (EP_LIST), whose elements are statements, or a lone expression
   (EP_NEW), which is one. Any other node is one of the statement's
   (EP_NODE). bits: LR_LATE / LR_BODY of the node; defer: a block whose call
   keeps it to run later, so that its nodes are late; kept: on a block of a
   call the record does not know (elab_block_runs 0), which may run it at
   once or keep it to run later, so its statements may run after any later
   point too. fmay: may, counted
   from the frame: a condition stands between the node and the body (or
   main's file) holding it; fpartial: partial, counted from the frame. sure:
   an else clause that runs whenever its
   condition is reached (the condition's predicate is a constant false,
   nt_pred_const). */
typedef struct {
  int e, file, top, frame, depth, rep;
  int list;   /* the statement list the node is an element of (a StatementsNode), -1 for a lone expression */
  unsigned char may, partial, kind, bits, defer, fmay, fpartial, sure, kept;
} ElabPos;
enum { EP_NODE, EP_SPINE, EP_LIST, EP_NEW };
ElabPos elab_child(Compiler *c, int p, int ch, const ElabPos *pp);
int elab_ref_last(const NodeTable *nt, int id, int ref);
int elab_walk_enter(Compiler *c, int ch, ElabPos *pos);
void elab_walk_leave(Compiler *c, int e);
void elab_walk_rest(Compiler *c, int id, ElabPos pos);
void elab_walk_site(Compiler *c, int id, const ElabPos *pos);
int elab_header_entity(Compiler *c, int node, const ElabPos *pos);
void elab_header_opened(Compiler *c, int node, int cls, int made);
int elab_rec_open(Compiler *c, int node, int cp, const char *cname, int encl);
void header_parent_encloses(Compiler *c, int par, int child, const char *leaf, int named);
void elab_load_sites(Compiler *c, int stmt, void (*fn)(void *ud, int load, int in_place), void *ud);
void walk_scope_stmt(Compiler *c, int stmt, int k, int scope_idx, ElabPos pos);
int scope_own_defaults(Compiler *c, int di);
void register_module_functions(Compiler *c);
void register_method_visibility(Compiler *c);
void register_locals(Compiler *c);
int new_recv_classes(Compiler *c, int recv, int *out, int cap);
int new_recv_every_class(Compiler *c, int recv);
int dm_each_inner(const NodeTable *nt, int call);   /* the define_method call of `[...].each { |v| define_method(...) { } }`, else -1 */
const char *vis_arg_name(const NodeTable *nt, int arg);   /* a Symbol or String argument's method name (analyze_scope.c) */
void register_struct_members(Compiler *c, ClassInfo *cls, int val);
void register_structs(Compiler *c);
void register_singleton_defs(Compiler *c);
void fix_struct_block_scopes(Compiler *c);
void register_attrs(Compiler *c);
void register_aliases(Compiler *c);
void register_undefs(Compiler *c);
int is_c_ident(const char *s);
void register_globals_consts(Compiler *c);
const char *ffi_arg_str(const NodeTable *nt, int nid);
int ffi_arg_int(const NodeTable *nt, int nid);
TyKind ffi_spec_to_ty(const char *spec);
void register_ffi_decls(Compiler *c);
int ffi_find_func(Compiler *c, const char *mod, const char *name);
int ffi_find_buf(Compiler *c, const char *mod, const char *name);
int ffi_find_reader(Compiler *c, const char *mod, const char *name);
int ffi_find_writer(Compiler *c, const char *mod, const char *name);
int infer_global_const_types(Compiler *c);
int infer_multiwrite_const_types(Compiler *c);
int masgn_tuple_rhs(const NodeTable *nt, int value);
int multi_return_elem_types(Compiler *c, int value, TyKind *out, int max);
void resolve_parents(Compiler *c);
void resolve_inherited_aliases(Compiler *c);
void settle_object_builtin_aliases(Compiler *c);
void settle_mixin_aliases(Compiler *c);       /* an alias names the method in effect where it appears */
void process_include_body(Compiler *c, int ci, int body_node);
void desugar_module_prepends(Compiler *c);
void register_includes(Compiler *c);
int desugar_included_self_class(Compiler *c, int body, const char *cn);
int desugar_included_bare_builtin(Compiler *c, int body, const char *cn);
void register_include_attrs(Compiler *c);
void rewrite_attr_supers(Compiler *c);
void rewrite_self_attr_empty_writes(Compiler *c);
void unmark_referenced_module_sources(Compiler *c);
void register_extends(Compiler *c);
int cmethod_needs_specialization(Compiler *c, int mi, int ci, int def_cls, int *has_new);
void specialize_inherited_cls_new(Compiler *c);
void register_prepends(Compiler *c);
void inherit_members(Compiler *c);
int infer_inherited_ivars(Compiler *c);
int infer_cvar_types(Compiler *c, int nil_only);
int infer_ivar_types(Compiler *c);
/* 1 if `name` (incl '@') is an ivar pinned by an --rbs seed in class `ci`;
   the ivar inference write sites consult this to avoid widening a pinned slot. */
int class_ivar_pinned(ClassInfo *ci, const char *name);
void class_pin_ivar(ClassInfo *ci, const char *name);
void class_unpin_ivar(ClassInfo *ci, const char *name);
/* 1 if a seed still pins the slot against a value of type `vt`; a Hash
   keyed by boxed values reaching a Hash the seed keys by one class drops
   the seed (see seed_form_gives_way in analyze.c) */
int class_ivar_pin_holds(ClassInfo *ci, const char *name, TyKind vt);
int param_seed_holds(LocalVar *lv, TyKind at);
int ret_seed_holds(Scope *s, TyKind r);
void sp_narrow_memo_bump(void);
int infer_write_types(Compiler *c);
void seed_unsupplied_nil_defaults(Compiler *c);
int infer_container_flow(Compiler *c);
void an_cls_facts(Compiler *c);   /* analyze_cls.c (#8530) */
int an_builtin_only_p(void);
unsigned long infer_memo_edits(const Compiler *c); /* what infer_type's memo is keyed by */
int infer_memo_usable(void);                       /* ... and whether it is in use */
TyKind an_builtin_answer(Compiler *c, int id);
/* the kind of a value of class self_cls: a reopened builtin's own (Time -> TY_TIME),
   else the object */
TyKind infer_builtin_self(Compiler *c, int self_cls);
TyKind builtin_value_kind(const char *n);   /* the builtin class n's own value kind, or TY_UNKNOWN */
TyKind an_self_node_type(Compiler *c, int id);
int an_yield_site_builtin_answer(Compiler *c, int id, TyKind kind, TyKind *out);
extern int g_scopes_settled;   /* analysis done (codegen_util.c) */
int poly_expr_flows_container(Compiler *c, int node);
int rejoin_local_writes(Compiler *c);
int widen_arrays_from_map_bang(Compiler *c);
void intern_block_params(Compiler *c);
int local_all_writes_empty_array(Compiler *c, Scope *sc, const char *name);
int infer_param_hash_value(Compiler *c);
int bind_call_params(Compiler *c, int call_id, int mi);
int param_src_misfits(Compiler *c, Scope *m, LocalVar *p, TyKind ct, const int *argv,
                      const ArgLayout *L);
void call_layout(Compiler *c, Scope *m, const int *argv, int argc, ArgLayout *L);
int layout_plain_arg(Compiler *c, Scope *m, const int *argv, const ArgLayout *L, int i);
int call_param_arg(Compiler *c, Scope *m, const int *argv, int argc, int i);
int unassigned_param_read(Compiler *c, Scope *sc, int n);
int param_rest_misfits(Compiler *c, Scope *m, LocalVar *p, TyKind ct, const int *argv, int an);
int opt_before_required(Compiler *c, Scope *m);                 /* codegen_fold.c */
int arg_slot_for_param(Compiler *c, Scope *m, int idx, int argc); /* codegen_fold.c */
int arg_layout_plain_arg(Compiler *c, Scope *m, int pos_argc, int i); /* codegen_fold.c */
int arg_layout_param_node(Compiler *c, Scope *m, int call, int i, int *spread); /* codegen_fold.c */
void arg_layout_param_map(Compiler *c, Scope *m, const int *argv, int argc, int *out); /* codegen_fold.c */
void arg_layout_call_param_map(Compiler *c, Scope *m, int call, int n, int *out, int *spread); /* codegen_fold.c */
int arg_layout_param_source(Compiler *c, Scope *m, int call, int i, int *spread);
int zsuper_param_source(Compiler *c, Scope *s, Scope *pm, int j);         /* codegen_fold.c */
int ty_runs_no_code(TyKind t);  /* codegen_call.c: a builtin reading such a value runs no program code */
int refl_member_of_struct_superclass(Compiler *c, int ci, const char *m);  /* codegen_call.c */
int subtree_reads_match_globals(Compiler *c, int root);  /* codegen_call_recv.c: $~, $1.. read there */
int rest_packable_arm(Compiler *c, Scope *s);                    /* codegen_fold.c */
int class_is_comparable(Compiler *c, int cid);                   /* codegen_call.c */
/* More codegen-side helpers the analyzer calls (declared here so no analyzer
   TU calls one undeclared, as with unsupported_feature above; codegen_internal.h
   declares the same). */
int bare_call_class_owned(Compiler *c, int id);                  /* codegen_call.c */
const int *call_args(const NodeTable *nt, int id, int *argc);    /* codegen_call.c */
int exc_has_nonstring_msg_override(Compiler *c);                 /* codegen_util.c */
int interp_is_literal_fold(const NodeTable *nt, int id);         /* codegen_expr.c */
int is_fresh_array(Compiler *c, int v);                          /* analyze_pass.c */
int ivar_assigned_in_initialize(Compiler *c, int k, const char *ivn);  /* analyze.c */
int json_parse_ostruct_opts(Compiler *c, int kwh);               /* codegen_call_class.c */
int kwh_lookup(const NodeTable *nt, int kwh, const char *kname); /* codegen_fold.c */
int lazy_endpoint_is_infinite(Compiler *c, int right);           /* codegen_call.c */
int proc_form_source(Compiler *c, int s);                        /* codegen_util.c */
int scope_is_shadowed(Compiler *c, int s);                       /* codegen_util.c */
int str_append_chain(Compiler *c, int recv, int *chain, int *base);  /* codegen_util.c */
int strbuf_bang_tail(Compiler *c, int v);                        /* codegen_stmt.c */
int strbuf_builtin_tail(Compiler *c, int v);                     /* codegen_stmt.c */
int strbuf_iter_box_tail(Compiler *c, int v);                    /* codegen_stmt.c */
int strbuf_route_clamp(Compiler *c, int v, int *ops);           /* codegen_stmt.c */
int subtree_may_reassign_state(Compiler *c, int id);             /* codegen_call.c */
int subtree_may_run_proc(Compiler *c, int id);                   /* codegen_call.c */
int subtree_writes_local(Compiler *c, int id, const char *nm);   /* codegen_stmt.c */
/* The type a READ of ivar slot `iv` yields (a shared-mutable string slot
   reads as a plain String). Shared with the receiver-face helpers. */
TyKind ivar_value_ty(ClassInfo *ci, int iv);
int infer_range_call(Compiler *c, int id, TyKind rt, TyKind *out);
int range_object_face(const char *name);  /* Object's face of a Float / String range */
int infer_poly_call(Compiler *c, int id, TyKind rt, TyKind *out);
int infer_object_call(Compiler *c, int id, TyKind rt, TyKind *out);
int infer_arysub_call(Compiler *c, int id, TyKind *out);
TyKind infer_op_assign_type(Compiler *c, TyKind lhs, int value);
int infer_array_call(Compiler *c, int id, TyKind rt, TyKind *out);
int infer_hash_call(Compiler *c, int id, TyKind rt, TyKind *out);
/* bop_find for the call `id`, recording the row under --plan-check */
struct BuiltinOp;
const struct BuiltinOp *an_bop_find(Compiler *c, int id, TyKind rt, const char *name,
                                    int argc, int has_block);
/* an_bop_find for the BOPF_BOXED row of kind rt (bop_find_boxed) */
const struct BuiltinOp *an_bop_find_boxed(Compiler *c, int id, TyKind rt, const char *name,
                                          int argc, int has_block);
/* an_bop_find with the call's arguments' kinds (inferred), so the rows that
   guard an argument are read too */
const struct BuiltinOp *an_bop_find_arg(Compiler *c, int id, TyKind rt, const char *name,
                                        int argc, const int *argv, int has_block);
/* the row inference finds for call `id` on a receiver of kind `rt`, recording nothing */
const struct BuiltinOp *an_bop_row_for_kind(Compiler *c, int id, TyKind rt);
/* bop_result_args with the call's arguments' kinds (inferred) */
TyKind an_bop_result(Compiler *c, const struct BuiltinOp *op, TyKind rt, int id, int argc, const int *argv);
/* the facts bop_result_args reads beside the operands' kinds (BOPX_*) */
int an_bop_facts(Compiler *c, const struct BuiltinOp *op, int id);
/* method_call_ret(c, mi, id) for a call an arm of infer_call bound to the
   user method mi, recording the binding (via UC_*, the class owner_ci whose
   chain was searched) under --plan-check */
TyKind an_user_call(Compiler *c, int id, int mi, int via, int owner_ci);
/* the record alone, for an arm that answers the bound call another way */
void an_user_call_record(Compiler *c, int id, int mi, int via, int owner_ci);
int infer_numeric_call(Compiler *c, int id, TyKind rt, TyKind *out);
/* arg1's kind (TY_UNKNOWN when there is no second argument) and whether a
   one-argument Range literal's own left/right bound is Float-typed (0
   otherwise) -- the two extra pieces ty_recv_builtin_result's clamp arms
   need beyond arg0, computed the same comp_ntype-based way both of its
   callers already compute arg0. const-safe (comp_ntype only reads an
   already-settled cache), so codegen's const Compiler* can call it too. */
void ty_recv_call_extra_args(const Compiler *c, const int *av, int argc,
                              TyKind *arg1, int *range_has_float_bound);
/* The array a map-shaped call answers from its block's tail (analyze_infer_recv.c). */
TyKind infer_sum_block_ty(Compiler *c, int block);
TyKind infer_map_block_ty(Compiler *c, int id, int block);
TyKind infer_map_block_elem_ty(Compiler *c, int block);
void step_kw_positional(Compiler *c, NodeTable *nt, int id, int args, int to_v, int by_v, int by_first);
int step_kw_unknown(Compiler *c, NodeTable *nt, int id, int args);
void kw_error_message(char *out, size_t n, const char *kind, int count, const char *names);
void kw_names_add(char *list, size_t n, int *count, const char *inspected);
void kw_key_inspect(const char *kn, int is_sym, char *out, size_t n);
/* A range endpoint that is the infinite Float constant (analyze_infer.c). */
int infer_end_is_float_inf(Compiler *c, int right);
int propagate_prep_params(Compiler *c);
int an_program_spawns_threads(Compiler *c);
int infer_default_param_types(Compiler *c);
int is_string_only_method(const char *m);
int infer_params_from_ivar_hash_ops(Compiler *c);
int infer_hash_params(Compiler *c);
int infer_array_params(Compiler *c);
int infer_string_params(Compiler *c);
int infer_param_types(Compiler *c);
int infer_param_types_settle(Compiler *c);
/* Every node's type at one point of the late passes (node_types_take): a
   pass handed one joins again only the calls and stores whose inputs have
   widened since. A node made after it had no type then. */
typedef struct { TyKind *ty; int n; } NodeTypes;
void node_types_take(Compiler *c, NodeTypes *s);
int an_store_widened(Compiler *c, int id, const NodeTypes *before);
int infer_changed_param_types(Compiler *c, const NodeTypes *before);
int rejoin_changed_masgn(Compiler *c, const NodeTypes *before);
int rejoin_changed_pattern_locals(Compiler *c, const NodeTypes *before);
int infer_changed_store_types(Compiler *c, const NodeTypes *before);
int infer_for_index(Compiler *c);
int infer_catch_block_params(Compiler *c);
void infer_bigint_loop_locals(Compiler *c);
void block_sites_index(Compiler *c);
int block_sites(Compiler *c, int si, const int **sites);
int block_site_args(Compiler *c, int si, int site, int call);
int a_super_target(Compiler *c, Scope *m);
int a_is_fiber_or_gen_create(Compiler *c, int id);   /* analyze.c */
/* A block's (or a proc literal's) parameters as its binders count them:
   P leading requireds, O optionals, Q posts, R a rest marker (`*r`, `*`,
   a trailing comma), nk keywords; kw when a trailing keyword hash goes to
   the keywords (named ones, or any `**`), never to a positional. */
typedef struct { int pn, num, P, O, Q, R, nk, kw, lambda; } BlockSig;
void block_sig(Compiler *c, int params, int lambda, BlockSig *s);
const char *block_sig_name(Compiler *c, const BlockSig *s, int i);
const char *block_sig_kw_name(Compiler *c, const BlockSig *s, int k);
/* What block_site_types records per positional beside its type: a site may
   leave it without a value (BS_ABSENT), or pass it a literal nil (BS_NIL). */
enum { BS_ABSENT = 1, BS_NIL = 2 };
void block_site_types(Compiler *c, const BlockSig *s, const int *av, int ac,
                      TyKind *pos, char *absent, TyKind *kws);
int block_settle_types(Compiler *c, int blk, const BlockSig *s,
                       const TyKind *pos, const char *absent, const TyKind *kws);
int a_proc_params_node(Compiler *c, int create);
int infer_block_params(Compiler *c);
int narrow_site_boxed_block_params(Compiler *c);
int desugar_to_proc_block_arg(Compiler *c);
int desugar_proc_expr_block_arg(Compiler *c);
int desugar_to_hash_splat(Compiler *c);
int desugar_kernel_array_conv(Compiler *c);
int desugar_splat_to_a(Compiler *c);
int desugar_builtin_splat_forward(Compiler *c);
int builtin_class_arity_spec(const char *cls, const char *m, int *min, int *max,
                             const char **lo_exp, const char **hi_exp);
/* The row an Integer or a Float (k) answers `name` with argc arguments
   from (and a literal block, has_block, where the row's block form takes
   one), for a boxed value that may be either, or NULL: only where CRuby's
   argument counts prove argc right for the class, as a row's own range can
   be the legacy any-count one (analyze_infer.c) */
const struct BuiltinOp *poly_scalar_row(TyKind k, const char *name, int argc, int has_block);
int desugar_lazy_method_call(Compiler *c);
int desugar_value_callable_forwards(Compiler *c);
int desugar_root_scoped_constants(Compiler *c);
const char *errno_canonical_name(const char *n);   /* analyze_util.c */
void desugar_class_reopen(Compiler *c);
int is_class_const(const NodeTable *nt, int id);   /* a `Class` constant read */
int desugar_def_unless_method_defined(Compiler *c);
int desugar_class_body_self_calls(Compiler *c);
int desugar_builtins(Compiler *c);
int desugar_enum_walk_calls(Compiler *c);
int desugar_index_op_write_user(Compiler *c);
int desugar_main_self_call(Compiler *c);
int desugar_cmethod_cvar_reflection(Compiler *c);
int desugar_builtin_enum_calls(Compiler *c);
int desugar_builtin_scalar_defs(Compiler *c);
int desugar_builtin_scalar_calls(Compiler *c);
int revert_widened_builtin_scalar_calls(Compiler *c);
/* Block iterator the nested-table narrow pass keeps. each / map / zip stay
   on the typed emitter. each_with_index / reduce / inject are rewritten
   onto builtins/enumerable.rb; the copy's own each is what walks the table. */
/* what a walk's block does with each row of a table (nested_row_iter_call) */
enum { ROW_WALK_NONE, ROW_WALK_BINDS, ROW_WALK_SPLITS };
int nested_row_iter_call(Compiler *c, int id);
int fold_static_is_a(Compiler *c);
int merge_block_recorded(Compiler *c);
int narrow_empty_array_args_by_yield(Compiler *c);
int builtin_enum_name_index(const char *name);
void enum_send_copy_take(NodeTable *nt, int call);
int an_class_includes_enumerable(Compiler *c, int ci);
int desugar_module_function_aliases(Compiler *c);
int method_body_next_to_return(NodeTable *nt, int id);
void desugar_extended_module_attrs(Compiler *c);
int desugar_enumerable_via_to_a(Compiler *c);
int desugar_public_method(Compiler *c);
int desugar_class_eval_value(Compiler *c);
int desugar_instance_eval_builtin(Compiler *c);
int desugar_builtin_class_var_recv(Compiler *c);
int desugar_class_body_bare_new(Compiler *c);
int desugar_bare_spawn(Compiler *c);
int desugar_bare_class_self_calls(Compiler *c);
int desugar_ie_bare_object_calls(Compiler *c);
int desugar_bare_object_reopen_calls(Compiler *c);
int desugar_descendant_reader_calls(Compiler *c);
int desugar_masgn_store_evidence(Compiler *c);
int desugar_index_assign_user_recv(Compiler *c);
int desugar_index_assign_value(Compiler *c);
int desugar_method_added_hook(Compiler *c);
int desugar_io_console_modes(Compiler *c);
int desugar_reader_index_write(Compiler *c);
int desugar_body_self_call(Compiler *c);
int desugar_body_module_eval(Compiler *c);
int desugar_extend_self(Compiler *c);
int desugar_instance_eval_defs(Compiler *c);
int enumerator_own_with_object(Compiler *c, int id);
int block_body_breaks(const NodeTable *nt, int node);   /* a break that leaves this block */
const LowerRow *lower_row_nt(const NodeTable *nt, int id, int family);   /* the family's row call id's shape matches */
int desugar_kernel_reopen(Compiler *c);
int desugar_singleton_class_mixin(Compiler *c);
int desugar_explicit_receiver_mixin(Compiler *c);
int desugar_singleton_attr(Compiler *c);
int desugar_attr_const_splat(Compiler *c);
int desugar_sclass_readers_through_class(Compiler *c);
int desugar_constant_path_self_alias(Compiler *c);
int desugar_dynamic_const_get_arms(Compiler *c);
int desugar_reopen_implicit_self(Compiler *c);
int desugar_handle_ivars(Compiler *c);
int desugar_fiber_prepended_init(Compiler *c);
int desugar_handle_reopen_self_recv(Compiler *c);
int desugar_call_or_write_reopen(Compiler *c);
int desugar_table_loops(Compiler *c);         /* analyze_ct.c */
int ct_method_defined(const NodeTable *nt, const char *meth);   /* the program defines `meth` anywhere (analyze_ct.c) */

/* ---- the lowering table (analyze_lower.c, #9898) ----
   The rewrites the load stage makes are the rows of one table, run by one
   driver. A row's visit rewrites the shape it eliminates at one node; its
   left says whether that shape is at a node, for SPINEL_LOWER_CHECK. The
   driver owns the loop: lower_run visits every node below the count at its
   entry, in ascending id, those no root reaches included. */

/* the rows, in the order the stage runs them */
enum {
  LW_NIL_BLOCK_ARG, LW_CONST_SET_WRITE, LW_THREAD_START, LW_MARSHAL_RESTORE, LW_SOCKET_OPEN, LW_SYSTEM_OPTIONS, LW_IO_POPEN_BLOCK,
  LW_USER_BACKTICK, LW_LITERAL_UNDEF_METHOD, LW_EXPLICIT_INITIALIZE_CALLS, LW_NOARG_DISPLAY_WRAPPERS,
  LW_PAREN_BLOCK_ARG, LW_PAREN_OPERANDS, LW_SEND_CLASS_MAKERS, LW_SELF_SUPERCLASS, LW_STRUCT_SUBCLASS_NEW_WRITE,
  LW_REFUSE_SUPERCLASS_EXPRS, LW_RESCUE_BLOCK_BODIES,
  LW_MARK_SYM_PROC_BLOCKS, LW_BUILDER_BLOCK_PARAM, LW_BODY_LOCAL_SCOPES, LW_ERRNO_ALIASES, LW_PAREN_DEF_BODY, LW_MARK_MATCH_RANGES,
  LW_MARK_RANGE_LOCALS, LW_DUPLICATE_UNDERSCORE_PARAMS, LW_ENCODING_QUERIES, LW_ALIAS_METHOD_STRING_NAMES,
  LW_ALIAS_METHOD_VALUES, LW_REASSIGNED_BLOCK_PARAMS,
  LW_ITSELF_RECEIVER, LW_TAP_VALUE_WRITE, LW_BLK_PARAM_WRITES, LW_REST_PARAM_WRITES, LW_YIELD_IN_CLOSURE,
  LW_SAFE_NAV_ATTR_WRITE, LW_CONST_ATTR_OP_ASSIGN, LW_STORED_ENUM_EACH, LW_SCOPE_NUMBERED_BLOCK_PARAMS,
  LW_SINGLETON_CLASS_DEFINE_METHOD, LW_EMPTY_HASH_COMPARE_BY_IDENTITY, LW_DEFINE_METHOD_PROC_ARG,
  LW_DEFINE_METHOD_CAPTURES, LW_DEFINE_METHOD_KEYWORDS, LW_NAME_ANON_BLOCK_KWREST, LW_RENAME_SHADOWING_BLOCK_PARAMS,
  LW_SYM_TO_PROC_CALL, LW_ENUM_CHAIN_SHAPES, LW_BUILTIN_KWSPLAT, LW_RIGHTWARD_PATTERN, LW_MATCH_PREDICATE,
  LW_SORT_BY_WITH_INDEX, LW_FORWARDING_TO_REST_CALLEE, LW_ANON_BLOCK_PARAM, LW_BLOCK_IMPLICIT_REST,
  LW_MULTI_VALUE_JUMP, LW_BLOCK_DESTRUCTURE_PARAMS, LW_FOR_NONLOCAL_INDEX, LW_ENUM_FOR_WITH_ARGS,
  LW_ENUMERATOR_PRODUCE, LW_PARAM_DEFAULT_ASSIGNS_LOCAL, LW_RECURSIVE_PARAM_DEFAULTS,
  LW__COUNT
};

typedef struct LowerCtx {
  int check;                /* SPINEL_LOWER_CHECK=1: the rows are checked and the compile stops */
  int findings;             /* what the check reported */
  int rewrote[LW__COUNT];   /* per row, the nodes it rewrote (the check's report) */
  int n0;                   /* the node count at the running row's entry */
  int owner, body;          /* LF_STMT: the body owner being visited and its StatementsNode */
  int scope;                /* LF_SCOPE: the node whose tag names the scope being visited (the owner, or a splice's first statement) */
  const int *scope_stmts;   /* ... and the scope's statements: the owner's body, or the splice's statements */
  int scope_n;
  /* scratch of the running row, zero at each run */
  int serial;               /* the fresh-name counter of a run */
  char **used;              /* names the program wrote that a row's invented ones must step past */
  int nused;
  int used_built;
  int *ids;                 /* node ids a row's gate collected for its visits */
  int nids;
  const char **aliases;     /* recursive_param_defaults: the program's alias pairs, new name then old */
  int naliases;
  /* the parent map (lower_parents), valid for the table it was made of */
  int *parent;
  int parent_n, parent_live;
  unsigned parent_version;
  unsigned long parent_edits;
  int prog_memo[4];         /* answers of the whole program a row asks for as needed: -1 until asked */
  /* spans (lower_span): code a step splices in after rows have run */
  unsigned char ran[LW__COUNT];     /* the row has run over the program */
  unsigned char settle[LW__COUNT];  /* ... and a span left it to lower_settle: LF_PROGRAM, or its gate said yes anew */
  unsigned char gate_no[LW__COUNT]; /* its gate said 0 at its last run over the program */
  unsigned char span_gate[LW__COUNT];   /* its gate in this step's spans: 0 not asked yet, 1 no, 2 yes */
  int *span_ids[LW__COUNT];         /* the ids that ask collected (ids), for the step */
  int span_nids[LW__COUNT];
  struct LowerSpan *span;   /* the span being lowered, or NULL: the program */
  int span_step;            /* spans were lowered since the last lower_settle */
  int span_now;             /* ... and they are the walk's, lowered one splice at a time as it makes them */
  int *later, nlater, clater;   /* span_now: (row, owner) pairs, a row that visits the body's owner, which
                                   lower_settle visits once for all the spans of that body */
  struct BlkpStep *blkp;    /* rename_shadowing_block_params' span form: the program's names, kept for the step */
} LowerCtx;

typedef int LowerFn(Compiler *c, LowerCtx *lc, int id);
/* a row's question of the whole program, asked once at the start of a run: 0 skips the run */
typedef int LowerGate(Compiler *c, LowerCtx *lc);

int lower_stmt_at(const NodeTable *nt, const LowerCtx *lc, int stmt);   /* the place of stmt in lc->body (LF_STMT rows) */
const int *lower_parents(Compiler *c, LowerCtx *lc);   /* every node's parent, made at a row's first ask in a run */
void lower_begin(Compiler *c, LowerCtx *lc);
void lower_check_unreachable(Compiler *c, LowerCtx *lc, const char *row, int root);   /* SPINEL_LOWER_CHECK: no root reaches the subtree at root */
void lower_say(LowerCtx *lc, const char *row, const char *what);   /* SPINEL_LOWER_CHECK: one finding of row's */
int lower_run(Compiler *c, LowerCtx *lc, int row);   /* the nodes it rewrote */
int lower_row_once(Compiler *c, int row);   /* a row on a context of its own, for a pass the fixpoint repeats */
int lower_span(Compiler *c, LowerCtx *lc, int owner, int body, const int *stmts, const int *scopes, int n);   /* the rows that ran, on spliced statements */
int *lower_span_ids(const LowerCtx *lc, int *n);   /* the span's nodes and the ones rows appended for it, ascending; the caller frees */
void blkp_step_free(struct BlkpStep *st);   /* analyze.c */
void lower_settle(Compiler *c, LowerCtx *lc);   /* the rows spans left to the program, once per step */

/* ---- the host (analyze_elab.c, #9898) ----
   A step that splices code into a body after the load stage began, or makes
   a block a class body (Class.new do..end), hands it to the host, which
   makes it what the program's own code is at that point:
   the static class_eval text in it is grafted (analyze_desugar.c's sce),
   and its statements are lowered by the rows that have run (lower_span), at
   host_end, once the step has spliced everything. What the text graft asks
   of the whole program is asked once per step. */
typedef struct HostStep {
  LowerCtx *lc;
  struct SceFacts *sce;    /* the graft's answers about the whole program, made at its first ask in the step */
  int *spans;              /* (owner, body, statement, its splice's first statement) of each statement the step spliced */
  int nspans, cspans;
  int now;                 /* the registration's walk reaches each splice next: it is lowered as it is made */
  struct CtMacroFacts *ct; /* the class-body macros' answers about the whole program, made at their first ask in the walk */
} HostStep;
void host_begin(HostStep *h, LowerCtx *lc);
int host_splice(Compiler *c, HostStep *h, int owner, int body, int at, int n, const char *locals, const char *gone);   /* the n statements at body[at], in a frame of locals but gone (NULL: not known): the count they become */
void host_end(Compiler *c, HostStep *h);
int ct_macro_form(Compiler *c, int call, int list, int j);   /* analyze_ct.c: class-body macro call `call`, statement j of `list`, run under the walk: the statement now first in its place, or -1 */
const char *handle_attr_decl(const NodeTable *nt, int call);   /* analyze_desugar.c: Thread or Fiber for a `Thread.attr_accessor :x` declaration, else NULL */
int handle_attr_form(Compiler *c, int call, int list, int j, int now, int file);   /* analyze_desugar.c: `Thread.attr_accessor :x` reached by the walk (statement j of list, run there when now, in file): the reopening in its place (its first statement), -2 at the file's end, -1 none */
void ct_macro_facts_free(struct CtMacroFacts *f);
struct HostStep *elab_walk_host(Compiler *c);   /* analyze_elab.c: the host a walk-time form splices through, while the walk runs */
int elab_walk_replace(Compiler *c, int old, int with);   /* ...and node with, spliced in place of entered statement old, takes its point */
int elab_stmt_list(Compiler *c, int node, int *frame);   /* the statement list statement node is an element of, *frame the class or module header around it (-1: main's); -1 for no statement */
int desugar_static_class_eval(Compiler *c, HostStep *h);   /* analyze_desugar.c: the graft over every class and module body */
int sce_span(Compiler *c, HostStep *h, int owner, int body, int at, int n, const char *locals, const char *gone);   /* ... over spliced statements */
void sce_facts_free(struct SceFacts *f);
int sce_graft_text(Compiler *c, HostStep *h, int owner, const char *text, int site, LoadPoint at, int **out, int *n);   /* a macro's computed class_eval text, grafted, not placed */
int bo_may_act(const NodeTable *nt, int e);   /* analyze.c: can evaluating e do anything observable (a call may; a read of a local, an instance variable or a constant, a literal cannot) */
int host_prewalk_shape(NodeTable *nt, const int *ids, int n);   /* a node of the span a pre-walk step reads, or -1 */
void lower_finish(Compiler *c, LowerCtx *lc);   /* frees; under the check, reports and exits */

/* the rows' functions, in the files that own the rewrites */
LowerFn desugar_nil_block_arg_at, desugar_nil_block_arg_left;               /* analyze_desugar.c */
LowerGate desugar_thread_start_gate, desugar_marshal_restore_gate, desugar_socket_open_gate;   /* analyze.c */
LowerGate desugar_system_options_gate, desugar_io_popen_block_gate;
LowerFn desugar_thread_start_at, desugar_thread_start_left;
LowerFn desugar_marshal_restore_at, desugar_marshal_restore_left;
LowerFn desugar_socket_open_at, desugar_socket_open_left;
LowerFn desugar_system_options_at, desugar_system_options_left;
LowerFn desugar_io_popen_block_at, desugar_io_popen_block_left;
LowerGate desugar_itself_receiver_gate, desugar_builtin_kwsplat_gate;
LowerFn desugar_itself_receiver_at, desugar_itself_receiver_left;
LowerGate desugar_const_set_write_gate;
LowerFn desugar_const_set_write_at, desugar_const_set_write_left;
LowerFn desugar_struct_subclass_new_write_at, desugar_struct_subclass_new_write_left;
LowerFn refuse_superclass_exprs_at, desugar_sym_to_proc_call_at, desugar_builtin_kwsplat_at;
LowerGate desugar_user_backtick_gate;
LowerFn desugar_user_backtick_at;                                            /* analyze.c */
LowerFn desugar_explicit_initialize_calls_at, scope_numbered_block_params_at, rename_shadowing_block_params_at,
  rename_shadowing_block_params_span;
LowerFn desugar_body_local_scopes_at, desugar_define_method_captures_at;     /* analyze_desugar.c */
LowerFn desugar_builder_block_param_at, desugar_builder_block_param_left;   /* analyze_desugar.c */
LowerFn desugar_forwarding_to_rest_callee_at, desugar_recursive_param_defaults_at;
LowerFn desugar_stored_enum_each_at;                                         /* analyze_desugar.c */
LowerFn desugar_literal_undef_method_at, desugar_literal_undef_method_left;
LowerFn desugar_alias_method_values_at, desugar_alias_method_values_left;
LowerFn desugar_singleton_class_define_method_at, desugar_singleton_class_define_method_left;
LowerFn desugar_define_method_proc_arg_at, desugar_define_method_keywords_at;
LowerFn desugar_block_implicit_rest_at, desugar_block_implicit_rest_left;
LowerFn desugar_noarg_display_wrappers_at, desugar_noarg_display_wrappers_left;   /* analyze.c */
LowerFn desugar_self_superclass_at, desugar_self_superclass_left;
LowerFn desugar_enum_chain_shapes_at;
LowerFn desugar_paren_block_arg_at, desugar_paren_block_arg_left;           /* analyze.c */
LowerFn desugar_paren_operands_at, desugar_paren_operands_left;
LowerFn desugar_send_class_makers_at, desugar_send_class_makers_left;
LowerFn desugar_rescue_block_bodies_at, desugar_rescue_block_bodies_left;
LowerFn mark_sym_proc_blocks_at;                                             /* analyze_desugar.c */
LowerFn desugar_errno_aliases_at, desugar_errno_aliases_left;
LowerFn desugar_paren_def_body_at, desugar_paren_def_body_left;
LowerFn mark_match_ranges_at, mark_range_locals_at;
LowerFn desugar_duplicate_underscore_params_at, desugar_duplicate_underscore_params_left;
LowerFn desugar_encoding_queries_at, desugar_encoding_queries_left;
LowerFn desugar_alias_method_string_names_at, desugar_alias_method_string_names_left;
LowerFn desugar_reassigned_block_params_at, desugar_reassigned_block_params_left;
LowerFn desugar_tap_value_write_at;                                          /* analyze.c */
LowerFn desugar_blk_param_writes_at, desugar_blk_param_writes_left;         /* analyze_desugar.c */
LowerFn desugar_rest_param_writes_at, desugar_rest_param_writes_left;
LowerFn desugar_yield_in_closure_at, desugar_yield_in_closure_left;
LowerFn desugar_safe_nav_attr_write_at, desugar_safe_nav_attr_write_left;
LowerFn desugar_const_attr_op_assign_at, desugar_const_attr_op_assign_left;
LowerFn desugar_empty_hash_compare_by_identity_at, desugar_empty_hash_compare_by_identity_left;
LowerFn name_anon_block_kwrest_at, name_anon_block_kwrest_left;             /* analyze.c */
LowerFn desugar_rightward_pattern_at, desugar_rightward_pattern_left;       /* analyze_desugar.c */
LowerFn desugar_match_predicate_at, desugar_match_predicate_left;
LowerFn desugar_sort_by_with_index_at, desugar_sort_by_with_index_left;
LowerFn desugar_anon_block_param_at, desugar_anon_block_param_left;
LowerFn desugar_multi_value_jump_at, desugar_multi_value_jump_left;
LowerFn desugar_block_destructure_params_at, desugar_block_destructure_params_left;  /* analyze_pass.c */
LowerFn desugar_for_nonlocal_index_at, desugar_for_nonlocal_index_left;
LowerFn desugar_enum_for_with_args_at;                                       /* analyze.c */
LowerFn desugar_enumerator_produce_at, desugar_enumerator_produce_left;
LowerFn desugar_param_default_assigns_local_at;                              /* analyze_desugar.c */
int desugar_compose_method_operand(Compiler *c);
int desugar_mutator_receiver_value(Compiler *c);
int desugar_method_curry(Compiler *c);
int desugar_curry_arity_to_int(Compiler *c);
int desugar_int_enum_with_index(Compiler *c);
int desugar_hash_iter_with_index(Compiler *c);
int desugar_array_op_to_ary(Compiler *c);
int widen_shared_cmp_params(Compiler *c);
int desugar_reduce_proc_arg(Compiler *c);
int desugar_reduce_splat_args(Compiler *c);
int desugar_block_capture_wrap(Compiler *c);
int desugar_user_not_match(Compiler *c);
int desugar_env_enum(Compiler *c);
int desugar_dir_surface(Compiler *c);
int local_write_binds_value(NodeKind k);
int desugar_enumerable_chain(Compiler *c);
int desugar_implicit_send(Compiler *c);
int desugar_public_send_recv(Compiler *c);
int type_block_rest_params(Compiler *c);
void check_block_rest_support(Compiler *c);
int desugar_dynamic_send(Compiler *c);
void expand_static_splat_args(Compiler *c, int from, int count);
int desugar_dynamic_method(Compiler *c);
int desugar_method_call_runtime_name(Compiler *c);
int desugar_builtin_method_defined_conds(Compiler *c);
int desugar_engine_branches(Compiler *c);
int desugar_conditional_defs(Compiler *c);
int desugar_concat_builtin_version(Compiler *c);
/* A dispatcher def over arms `<sel> == <val>` calling `callee` with the
   def's parameters passed on, and an else arm (disp_def_build,
   analyze_desugar.c); `raise NoMethodError, msg` for that arm */
typedef struct { const char *sel; int val; const char *callee; int hist; } DispArm;
int disp_def_build(NodeTable *nt, int like, const char *name, int sg, int ar, int blk,
                   const DispArm *arm, int narm, int els);
int disp_raise_no_method(NodeTable *nt, const char *msg);
int disp_raise_no_method_self(NodeTable *nt, const char *head, int inst);
int disp_forward_call(NodeTable *nt, const char *callee, int ar, int blk);
int disp_super_call(NodeTable *nt, int ar, int blk);
int disp_method_missing_call(NodeTable *nt, const char *name, int ar, int blk);
int desugar_dynamic_respond_to(Compiler *c);
int desugar_toplevel_instance_exec(Compiler *c);
int desugar_binding_lvget(Compiler *c);
int desugar_step_kwargs(Compiler *c);
int desugar_defined_method_call(Compiler *c);
int desugar_respond_to_probe(Compiler *c);
int desugar_symbol_to_proc_call(Compiler *c);
int desugar_call_op_write(Compiler *c);
int desugar_reopened_op_write(Compiler *c);
int desugar_array_at(Compiler *c);
int desugar_unused_block_arg(Compiler *c);
int desugar_unpack_block(Compiler *c);
int desugar_interp_reopened_to_s(Compiler *c);
int desugar_interp_elided_to_s(Compiler *c);
int desugar_array_first_last(Compiler *c);
int desugar_enum_iter_splat_args(Compiler *c);
int desugar_index_value_block_pass(Compiler *c);
int desugar_value_form_drop_block(Compiler *c);
int desugar_iter_splat_args(Compiler *c);
int desugar_builtin_iter_block_shapes(Compiler *c);
int desugar_enum_pair_lone_param(Compiler *c);
int sym_block_values(Compiler *c, int id);
void enum_hop_yield_view(Compiler *c, int id, int hop);
int sym_proc_poly_pair_view(Compiler *c, int id);
int desugar_to_h_block(Compiler *c);
int desugar_ffi_library_functions(Compiler *c);
int desugar_native_func_defs(Compiler *c);
int comp_ffi_library_module(const NodeTable *nt, const char *mn);
int rewrite_ffi_dynamic_calls(Compiler *c);
int desugar_builtin_reopen_self_calls(Compiler *c);
int desugar_builtin_reopen_methods(Compiler *c);
int desugar_object_method_builtin_overrides(Compiler *c);
int desugar_body_ivars(Compiler *c);
int desugar_const_ivar_access(Compiler *c);
int desugar_class_eval_ivars(Compiler *c);
int desugar_builtin_ivars(Compiler *c);
int desugar_inherited_aliases(Compiler *c);
int desugar_builtin_alias_calls(Compiler *c);
int desugar_reader_aliases_before_redef(Compiler *c);
int desugar_readers_over_inherited_defs(Compiler *c);
int desugar_inline_struct_blocks(Compiler *c);
int desugar_singleton_class_meta(Compiler *c);
int desugar_class_new_blocks(Compiler *c, HostStep *h);
int desugar_extend_enumerable(Compiler *c, HostStep *h);
int desugar_included_hooks(Compiler *c, HostStep *h);
int desugar_class_body_object_acts(Compiler *c);
int desugar_struct_block_anonymous(Compiler *c);
int desugar_eval_method_block(Compiler *c);
int desugar_singleton_body_self(Compiler *c);
int desugar_inherited_hooks(Compiler *c);
int desugar_self_const_get(Compiler *c);
int desugar_holder_block_self_const(Compiler *c);
int holder_subclass_defines(const NodeTable *nt, const char *holder, const char *name);
int desugar_dynamic_const_get(Compiler *c);
TyKind return_node_type(Compiler *c, int id);
int infer_return_types(Compiler *c);
int backprop_hash_return_types(Compiler *c);
int backprop_call_target(Compiler *c, int call_id);
int ivar_src_slot(Compiler *c, int v, const char **ivn);
void cr_collect_calls(Compiler *c, const NodeTable *nt, int id, char ***out, int *n, int *cap, int instance_self);
void cr_collect_program_end(Compiler *c, char ***out, int *n, int *cap);   /* the printer of an uncaught exception */
int cr_ty_builtin_value(Compiler *c, TyKind t);
void compute_reachable(Compiler *c, int typed);
void refuse_native_singleton_reopen(Compiler *c, Scope *s);
void compute_instantiated(Compiler *c, int early);
int aname_has(ANameSet *s, const char *nm);
void aname_add(ANameSet *s, const char *nm);
void a_collect_alias_used(Compiler *c, int id, ANameSet *out, int depth);   /* names a closure reads through a lazy chain local */
/* A hashed name set (keys borrowed, not copied); zero-initialize to start. */
typedef struct { const char **key; int *next, *head, n, cap, nb; } ANameHash;
int anh_has(const ANameHash *st, const char *nm);
int anh_find(const ANameHash *st, const char *nm);   /* insertion index, or -1 */
void anh_add(ANameHash *st, const char *nm);
void anh_free(ANameHash *st);
void cr_class_only_names(Compiler *c, ANameHash *out);   /* the names only plain classes' methods hold */
/* The scopes of a name, ascending: the first, and the next after s (-1 at
   the end); from a table while the scope index is frozen, else a scan
   (analyze.c). A pass walking every scope for a name's methods per call
   asks these instead. */
int an_scopes_named_first(Compiler *c, const char *nm);
int an_scopes_named_next(Compiler *c, const char *nm, int s);
/* The scopes sharing scope s's def_node (its copies), the same way. */
int an_scopes_same_def_first(Compiler *c, int s);
int an_scopes_same_def_next(Compiler *c, int s);
int a_is_local_node(const char *ty);
int a_is_write_node(const char *ty);
void a_mark_subtree(Compiler *c, int id, char *inproc);
void a_collect_used(Compiler *c, int id, ANameSet *out);
int a_proc_params_node(Compiler *c, int create);
int a_proc_body(Compiler *c, int create);
int a_block_is_lifted(Compiler *c, int id);
/* 1 if scope `si` creates a proc whose `return` leaves it (codegen.c): the method needs a proc-return frame. */
int scope_creates_returning_proc(Compiler *c, int si);
int a_proc_create_or_lifted(Compiler *c, int id);
void mark_proc_captures(Compiler *c);
void bigint_scan_body(const NodeTable *nt, int id, char ***names, int *n, int *cap);
void detect_bigint_loop_vars(Compiler *c);
void propagate_bigint_cascade(Compiler *c);
void mark_ie_subtree(Compiler *c, int node, int cls);
void build_ie_map(Compiler *c);
int ie_block_body(Compiler *c, int blk);
int ie_class_of(Compiler *c, int node);
int blkp_params_node(Compiler *c, int create);
int blkp_binds_param(Compiler *c, int create, const char *name);
void blkp_rewrite_refs(Compiler *c, int node, const char *oldn, const char *newn);
void numbered_rename_locals_str(NodeTable *nt, int L, const char *from, const char *to);
void blkp_mark_subtree(const NodeTable *nt, int node, char *marks);
void qc_collect_writes(Compiler *c, int node, char (*path)[64], int depth, QCWrite **ws, int *n, int *cap);
int qc_read_chain(const NodeTable *nt, int node, char (*chain)[64], int *abs_anchor);
void qc_qualified_name(char *out, size_t cap, const QCWrite *w);
void qc_rewrite_reads(Compiler *c, int node, char (*mods)[64], int mdepth, QCWrite *ws, int wn);
void qualify_colliding_consts(Compiler *c);
void qc_collect_class_writes(Compiler *c, int node, char (*path)[64], int depth, QCWrite **ws, int *n, int *cap);
void qualify_colliding_classes(Compiler *c);
void topup_forwarding_arity(Compiler *c);
void expand_struct_forwarding_super(Compiler *c);
void refuse_super_init_value(Compiler *c);
void refuse_readonly_global_write(Compiler *c);
/* Defined in codegen.c; the analyzer uses it to specialize/type a
   `rescue <UserExc> => e` binding (#1415). */
int class_is_exc_subclass(Compiler *c, int ci);
/* the native-binding registries' growth and `;`-merge (analyze_scope.c),
   shared with a bundled extension's __cext_link (analyze_cext.c) */
void ffi_semi_append(char **slot, const char *add);
void *ffi_grow(void *p, int n, int *cap, int init, size_t sz);
int exc_class_defines(Compiler *c, const char *name);
int super_reaches_exc_text(Compiler *c, int id, int reopen);
int exc_message_super_to_s(Compiler *c, int id, int *out, int cap);
int exc_subclass_defines(Compiler *c, const char *name);
int exc_user_method_name(Compiler *c, const char *name, int argc);
/* Defined in codegen_fold.c; 1 if class `k` is `anc` or a descendant of it. */
int is_descendant(Compiler *c, int k, int anc);
/* Defined in codegen_fold.c; distinct implementations of call `call`'s
   `name` across cid's subtree. */
int dispatch_impl_count(Compiler *c, int call, int cid, const char *name);
/* Defined in codegen_util.c; the proc-form clone of scope `s`, or -1. */
int scope_proc_form_of(Compiler *c, int s);
int reopen_yields_known_container(Compiler *c, Scope *m);
int ctor_site_on_cycle(Compiler *c, int id, int initm);
/* Defined in codegen_fold.c; the parameter a keyword hash binds as one more
   positional argument after `pos_argc` others, or -1. */
int kwh_arg_param(Compiler *c, Scope *m, int pos_argc);

/* --ext-entry designations (see codegen_internal.h / ext-design.md) */
extern const char *g_ext_entries;

/* the class every value of a boxed-value hash slot holds, or -1 (#4846) */
int hv_value_class(Compiler *c, int recv);
int comp_class_is_module(Compiler *c, ClassInfo *ci);
int poly_ivar_set_class(Compiler *c, int k);
int is_builtin_reopen(const char *name);
char **dsend_candidates(Compiler *c, int *out_n);
int exc_reopen_definers(Compiler *c, const char *mname, int *out, int max);

int infer_cext_builtin_constant(Compiler *c, int node);
#endif
