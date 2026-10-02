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

#define SP_RUBY_VERSION "4.0.7"

/* Release the compiler-owned, scope/name-borrowing forwarding proofs. */
void fwd_analysis_free(Compiler *c);

/* Set by main.c from --int-overflow=promote. In promote mode the analyzer is
   free to widen accumulating int locals to bigint more aggressively (e.g. block
   iteration loops, not just `while`), since the overflow-raising int macros are
   exactly what promote mode is asking us to avoid. Off (0) for raise/wrap, so
   the default gates and optcarrot (which pins wrap) see no behavior change. */
extern int g_promote_mode;

/* Set by main.c from --plan-check (#7100): inference records, per call node,
   the builtin-op row it answered the call with (c->bop_inf), and codegen
   reports on stderr every call it emitted through a row inference did not
   choose. Off in every normal build. */
extern int g_plan_check;

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
/* True if a regex source contains a capturing group: an unescaped '(' that
   isn't the start of a non-capturing/extension group '(?...'. scan returns
   nested arrays for capturing patterns, which the str_array path can't model. */
int an_re_has_captures(const char *src);
int an_send_name_is_computed(Compiler *c, int arg);
int an_str_mutator_name(const char *nm);
/* A String handed to a proc, a lambda or a Method (#6179): what the targets
   a `.call` / `.()` / `[]` / `.yield` / `===` on a Proc or Method value can
   reach do with its argument at one position. Filled by dyn_call_reach, for
   a node dyn_call_site accepts. */
typedef struct {
  int app;               /* a target appends to the parameter (or hands it on to one that does) */
  int unknown;           /* a target is not known */
  int keeps;             /* a known target reads the plain value and may keep it */
  const char *unlifted;  /* a target reached through a path not shared yet ("bind", "curry") */
  const char *pname;     /* a parameter it binds, for a diagnostic */
  const char *mname;     /* the method it binds, when the target is one */
  int argc1;             /* the call's count of plain positional arguments, plus one; 0 if not known */
} DynReach;
/* The keyword arm (`f.call(k1: s)`): what the targets do with keyword `key`. */
void dyn_call_kw_reach(Compiler *c, int n, const char *key, DynReach *r);
void dyn_value_kw_reach(Compiler *c, int v, const char *key, DynReach *r);
int dyn_method_kw_appends(Compiler *c, int mi, const char *key, int *j_out);
/* A keyword-hash element's Symbol key, and its value node in *val. */
const char *dyn_kw_elem_key(Compiler *c, int el, int *val);
int dyn_call_site(Compiler *c, int n);
void dyn_call_reach(Compiler *c, int n, int k, DynReach *r);
void dyn_value_reach(Compiler *c, int v, int k, DynReach *r);
int dyn_yield_site(Compiler *c, int y);
void dyn_yield_reach(Compiler *c, int y, int k, DynReach *r);
void dyn_blk_reach(Compiler *c, int mi, int k, DynReach *r);
int dyn_yield_param_appends(Compiler *c, int mi, int j);
int dyn_yield_live(Compiler *c, int mi, int k);
int dyn_open_site(Compiler *c, int n, int *shift);
void dyn_open_reach(Compiler *c, int n, int k, DynReach *r);
int dyn_method_appends(Compiler *c, int mi, int j);
int an_local_array_changed_x(Compiler *c, const char *xn, Scope *xs);
int an_local_array_stores_unshared(Compiler *c, const char *xn, Scope *xs);
/* Unknown paths require a shared input; escapes refuse even a shared one. */
typedef enum {
  FWD_RETAINS_BOX = -3, FWD_ESCAPE = -2, FWD_UNKNOWN = -1, FWD_READONLY = 0, FWD_APPENDS = 1
} FwdResult;
FwdResult fwd_rest_elem_appends(Compiler *c, int mi, int i);
int fwd_rest_print_safe(Compiler *c, int mi, int call, int actual);
FwdResult fwd_poly_param_appends(Compiler *c, int mi, int j);
/* Copying escapes require a closed readonly program; frozen provenance
   is sufficient only for a preserving-box retention effect. */
int fwd_box_retention_safe(Compiler *c, int node, FwdResult effect);
/* A boxed parameter's argument a literal block appends to through a yield
   (yield_splice_handles): a String variable there must be the handle. */
int yield_poly_arg_wants_handle(Compiler *c, int a);
/* A literal block a splat into a yield or instance_exec reaches, whose every
   parameter it appends to takes the handle the gathered Array holds
   (block_splat_pull_args). */
int block_splat_shares(Compiler *c, int blk);
FwdResult fwd_param_appends_at(Compiler *c, int mi, int j);
int dyn_block_appends(Compiler *c, int blk, int k);
/* `new` and `raise C, s` into an initialize that appends to a String
   parameter (#6179): the initialize methods a call reaches, the argument
   one binds to a parameter, and whether the initialize appends to it. */
int ctor_call_targets(Compiler *c, int u, int *first, int *boxed, int *out, int cap);
int ctor_param_arg(Compiler *c, Scope *m, int u, int first, int j);
int ctor_param_appends(Compiler *c, int mi, int j);
int ctor_param_reads_only(Compiler *c, int mi, int j);
int ctor_arg_in_splat(Compiler *c, int u, int a);
int an_indexed_each_source(const NodeTable *nt, int recv);
void an_node_dir(const NodeTable *nt, int id, char *dir, size_t cap);
const char *an_memo_reader_ivar(Compiler *c, int mi);
/* The ivar read/write node a local write's value aliases, or -1 */
int strbuf_ivar_alias_value(const NodeTable *nt, int v);

/* Infer (and cache) the type of node `id`. Used during analysis; codegen
   reads the cached results via comp_ntype. */
TyKind infer_type(Compiler *c, int id);

/* `recv` is a blockless call making an Enumerator that yields two values per
   element: each_with_index, with_index, each_with_object, with_object. */
int enum_pair_source_call(const NodeTable *nt, int recv);

/* map and the selecting Enumerables, which a boxed Array answers a blockless
   call of with an Enumerator, as it does each (analyze_infer_recv.c) */
int poly_blockless_enum_name(const char *name);

/* True when node `id`'s value, held in an unboxed scalar slot, can be the
   reserved nil sentinel (SP_INT_NIL / the float twin). The slot type alone
   cannot say -- an `Integer?` and an `Integer` are both TY_INT -- so codegen
   asks this before choosing between sp_box_int and sp_box_int_or_nil at a poly
   boundary. Valid only after analyze_program has settled the marking. */
int nullable_int_value(Compiler *c, int id);
/* The same, asked of the variable rather than of the read: what it can hold
   anywhere, the nil narrowing's facts left out. */
int nullable_int_value_raw(Compiler *c, int id);
int scalar_nil_only_call(Compiler *c, int id, TyKind rt);
int nullable_scalar_nil_only_call(Compiler *c, int id);
int nullable_int_elem_read(Compiler *c, int call);
int nullable_int_elem_array(Compiler *c, int node);
TyKind tuple_elem_read_type(Compiler *c, int node);
TyKind tuple_elem_read_unboxed(Compiler *c, int node);

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
TyKind then_block_value_ty(Compiler *c, int body, TyKind tail);
TyKind hash_merge_block_value_ty(Compiler *c, int id);
/* The value type of every `next` that leaves the block whose body is `node`.
   Only `next`: a `break` leaves the ITERATOR, so its value is the iterator
   call's, not the block's. The analysis joins this with the block's tail to
   type a yield, and the emitter joins it with the same tail to type the
   splice the yield reads -- one answer for one question. */
TyKind block_next_value_ty(Compiler *c, int node);

/* True if CallNode `id` is an Enumerable method on a Range that spinel does not
   handle natively but supports on arrays -- served by materializing the range
   to an int array (both inference and codegen then treat the receiver as an int
   array). Excludes range-native methods (each/map/select/sum/min/count-no-arg). */
int range_enum_redispatch(Compiler *c, int id);
int hash_enum_redispatch(Compiler *c, int id);
int range_lit_float_end(Compiler *c, int recv);   /* (1..5.5): the Float end node, else -1 */
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
/* Pin/read the receiver node the inference should answer as `kind` while
   codegen re-enters a typed emitter for a boxed receiver (the face table in
   types.h). Node -1 clears the pin. */
void an_set_face_node(int node, TyKind kind);
int  an_face_node(void);
TyKind an_face_kind(void);
/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx);
/* The name of a numbered block parameter (`_1`..`_9`) on this parameters node.
   Per BLOCK where a scope holds more than one such block; see
   scope_numbered_block_params. Every site that needs the name goes here. */
const char *numbered_param_name(Compiler *c, int params_node, int idx);
/* Name of a block's trailing rest parameter (`|*a|`), or NULL. */
const char *block_rest_name(Compiler *c, int block);
const char *block_opt_name(Compiler *c, int block, int idx);
const char *block_param_at(Compiler *c, int block, int idx, int n);
int call_plain_argc(Compiler *c, int call);
const char *block_post_name(Compiler *c, int block, int idx);
int block_lone_rest(Compiler *c, int block);
int block_rest_marker(Compiler *c, int block);
int block_lead_only(Compiler *c, int block);
int block_auto_splats(int P, int O, int Q, int R);
int block_no_keywords(Compiler *c, int block);
void block_fill(int P, int O, int Q, int R, int n, int *ot, int *ps);
/* The array literal a splat of this local is sure to spread, or -1. */
int splat_local_sure_lit(Compiler *c, int x);
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
/* A `Hash.new { }` default block lowered to a real proc (analyze_util.c);
   codegen emits it that way. */
int hash_new_block_is_proc(Compiler *c, int id);

/* Element type an `each_with_object([])` accumulator is filled with, inferred
   from how the memo param is pushed to (following a forwarded callable's body).
   TY_UNKNOWN when undetermined; callers default an empty `[]` to int_array. */
TyKind ewo_memo_elem_type(Compiler *c, int callid);

/* For a curry-application node, whether it completes the curry (reaches the base
   proc's arity) and the proc's return type. Returns 1 for a recognized chain. */
int curry_apply_info(Compiler *c, int node, int *out_complete, TyKind *out_ret);
int curry_count_max(Compiler *c, int recv);
int an_program_builds_methods(Compiler *c);   /* the program builds Method objects at all */
int an_zero_arg_builtin_shadowed(Compiler *c, const char *name, int argc);
int an_user_recv_defines_method(Compiler *c, const char *name);
/* obj.methods / public_methods / singleton_methods on an instance of `cid`
   fold to a static symbol list */
int an_object_methods_listable(Compiler *c, int cid, const char *name);
int an_class_singleton_methods_listable(Compiler *c, int cid);
int ewo_memo_passed_to_callable_at(Compiler *c, int callid, int pidx);

/* Class index when a receiverless instance_eval/exec resolves to self, else -1. */
int ie_implicit_self_class(Compiler *c, int id);
int ie_poly_self_classes(Compiler *c, const char *name, int body, int *out, int max,
                         const char **need);
int *ie_body_retype(Compiler *c, int body, int cls);
void ie_body_restore(Compiler *c, int *snap);

/* instance_exec keyword-arg helpers: the call's trailing KeywordHashNode (or
   -1), and the value node bound to a keyword name within it (or -1). */
int ie_call_kwhash(Compiler *c, int id);
size_t block_param_written_len(const char *name);
int block_param_is_renamed(const char *name);
void block_param_invent_name(Compiler *c, char *buf, size_t n,
                             const char *written, int blk);
int ie_kwhash_value(Compiler *c, int kwhash, const char *name);
TyKind ie_kwhash_computed_type(Compiler *c, int kwhash);

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
const char *class_value_instance_method_sym(Compiler *c, int node); /* instance_method on a run-time class */
int class_value_bind_call_target(Compiler *c, int ci, const char *sym, int *armless);
int class_value_bind_call_gap(Compiler *c, int node);
TyKind method_obj_adapter_ret(TyKind arr, const char *op); /* typed-array adapter Ruby return */
int method_recv_node(Compiler *c, int recv);          /* the method(:sym) node behind a Method expr */
int method_recv_nodes(Compiler *c, int recv, int **out); /* every one a re-written local may hold */
int method_expr_is_unbound(Compiler *c, int recv);    /* instance_method with no #bind crossed */
int proc_to_proc_method_nodes(Compiler *c, int recv, int **out); /* the method(:sym) nodes behind <method>.to_proc */
int method_call_param_shift(Compiler *c, int mn, int mi); /* 1 when self carries param[0] (__bam wrapper) */

/* Can a call ever arrive at an instance method of class/module `ci`? Only
   through a value that is one, so a class nobody instantiates -- and that no
   instantiated class inherits from or includes -- cannot be reached. Unsure
   (out-of-range ci) answers 1. Shared by the by-reference name group
   (analyze.c) and codegen's user_defines_or_reads. */
int an_class_can_be_reached(Compiler *c, int ci);

int a_block_is_lifted(Compiler *c, int id);

/* The parameter of Struct/Data initialize scope `s` that a bare `super`
   forwards into member `a`: the keyword of the member's name, else the a-th
   positional parameter. An index into s->pnames, or -1. When member `a` falls
   in the rest parameter, that is the index and *rest_off is the element's
   offset in it; otherwise *rest_off is -1. */
int struct_zsuper_param(Compiler *c, Scope *s, int a, const char *member, int *rest_off);
int struct_super_spreads(Compiler *c, int args);

/* The positional binding plan of a call, shared by codegen, which renders
   it, and inference, which types the parameters from it (codegen_fold.c).
   Where one positional parameter's value comes from, in a call's layout: */
typedef enum {
  ARG_BY_NAME,    /* a keyword parameter or the **kwrest: bound by name */
  ARG_DEFAULT,    /* no argument reaches it: its default */
  ARG_NODE,       /* the argument argv[arg] */
  ARG_KWH,        /* the keyword hash, one more positional argument */
  ARG_ELEM,       /* element `arg` of the splat spread in place, or of the gather */
  ARG_REST,       /* the rest: what the layout leaves between the others */
  ARG_GATHERED,   /* the gather's element by its run-time count (emit_gathered_param) */
} ArgFrom;
/* What a call's keyword hash is to one callee (kw_plan, codegen_fold.c). */
typedef enum {
  KWH_NONE,        /* the call passes no keyword hash */
  KWH_KEYWORDS,    /* keywords: the callee declares a keyword or a **kwrest */
  KWH_POSITIONAL,  /* one more positional Hash: the callee takes no keywords */
  KWH_REFUSED,     /* a `**nil` callee: holding a key, `no keywords accepted` */
} KwhRole;
/* The positionals a keyword hash adds to the count: none, one, or one when
   the run time finds a key in it (only `**` spreads, KWH_POSITIONAL). */
enum { KWC_NONE = 0, KWC_ONE = 1, KWC_NONEMPTY = -1 };
/* The keyword decisions of one call into one callee, taken once: what the
   hash is, what it adds to the positional count, and which keywords the
   call statically gets wrong, in CRuby's order (count, missing, unknown). */
typedef struct {
  int kwh;              /* the trailing KeywordHashNode, or -1 */
  KwhRole role;
  int count;            /* KWC_*: the positionals the hash adds */
  int literal;          /* a literal key (`k: v`, `"s" => v`) */
  int spread;           /* a `**` operand: its keys are the run time's (emit_ds_kwarg_check) */
  int computed;         /* a key only the run time knows (`k => v`) */
  int nmissing;         /* required keywords no key names, when no `**` may name them */
  char missing[512];
  int nunknown;         /* literal keys no keyword takes, each as #inspect writes it */
  char unknown[512];
  int unknown_el[32];   /* ... the kwh elements holding the first 32 of them */
  char first_sym[300];  /* the first of them a Symbol, or "" */
  int args_first;       /* the keywords are judged at run time: every argument runs first */
} KwPlan;
/* The positional layout of one call into one callee (arg_layout, codegen_fold.c):
   decided once, read by each binder that walks the parameters. */
typedef struct {
  int kwh, pos_argc;    /* the trailing keyword hash (-1) and the positionals ahead of it */
  int kwh_slot;         /* the parameter the hash binds as a positional (kwh_positional_slot) */
  int bind_argc;        /* the positionals, and the hash when it binds as one */
  int rest_argc;        /* the arguments a rest and its posts are laid out over (rest_bind_argc) */
  int rest_kwh;         /* the hash a rest takes at its tail (rest_kwh_tail) */
  int gather;           /* the count is the run time's: every positional from one array */
  int gather_kwh;       /* the gather's last element is the hash: 1 when it holds a key, 2 always */
  int splat;            /* static: the argv index of the splat spread in place, or -1 */
  KwPlan kw;            /* the keyword hash's own decisions */
  int n;
  ArgFrom *from;        /* per parameter */
  int *arg;             /* ARG_NODE: the argv index; ARG_ELEM: the element index */
} ArgLayout;
void arg_layout(Compiler *c, Scope *m, const int *argv, int pos_argc, int kwh, int inlined,
                ArgLayout *L);
/* The layout inference types the parameters from, asked before they have
   the types it gives them. */
void arg_layout_untyped(Compiler *c, Scope *m, const int *argv, int pos_argc, int kwh,
                        ArgLayout *L);
void arg_layout_free(ArgLayout *L);
/* May source `s` of a gathered call (argument s, or at pos_argc the hash
   the gather carries by gather_kwh) land in parameter i (analyze_pass.c)? */
int gather_reaches(Compiler *c, Scope *m, const int *argv, int pos_argc, int gather_kwh, int s, int i);
/* Does a bare `super` from `s` into `pm` pass `s`'s keywords as one more
   positional Hash? It does when `pm` takes no keyword at all (no keyword
   parameter, no `**kwrest`, no `**nil`) and `s` declares one: CRuby
   passes them as keywords, which such a method takes positionally. */
int zsuper_kw_positional(Compiler *c, Scope *s, Scope *pm);
int an_thread_arg_block(Compiler *c, int n);
int cap_wrap_mutates_param(Compiler *c, int blk, const char *bp);
#endif
