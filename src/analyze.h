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

/* Whether a boxed ivar setter's receiver can hold class k; an unproved
   receiver conservatively reaches every class. Shared by layout/emission. */
int poly_ivar_set_reaches(Compiler *c, int call, int k);
/* The classes the boxed receiver of call `call` can be an instance of, *n
   of them, or NULL when the analysis cannot bound them (analyze_scope.c) */
/* Kernel#String uses its argument as the conversion's implicit receiver. */
const int *poly_recv_classes(Compiler *c, int call, int *n);
/* Does an instantiated class of the program that the boxed receiver of `call`
   can be (any, when the walk does not bound it) define a method `name`? */
int poly_recv_defines(Compiler *c, int call, const char *name);
/* Does a method of the program answer the call `call` (the builtin reflection
   it names, a program's method of that name in its receiver's class chain)? A
   boxed receiver is asked by the classes it can be, a typed one by the call
   plan's targets. */
int call_reaches_program_method(Compiler *c, int call);
/* The classes and modules the same receiver can be ITSELF, as a value (a
   constant naming one, `{b: Integer}`), marked in a set over the class
   table, or NULL when the walk cannot bound the receiver's values */
const char *poly_recv_class_values(Compiler *c, int call);
/* Can a class method of class k be reached through that receiver: k is a
   class every class value answers for (Class, Module, ...), or one of
   the receiver's Class values is k or under it; 1 when the walk cannot
   bound them. Shared by the dispatch's arms and the call's type. */
int poly_cls_value_reaches(Compiler *c, int call, int k);
/* Is the class-value arm for class method kmi of class k left out of boxed
   call `call`: a module's own `def self.x` when no box can hold the module
   or none reaches the method. The dispatch (poly_cls_value_cands) and the
   call's type (an_class_concrete) both ask it, so they take the same arms. */
int poly_cls_arm_left_out(Compiler *c, int call, int k, int kmi);
/* A class method of `name` on one of the Class values the boxed receiver of
   `call` holds, as far as the walk bounds them (poly_cls_value_reaches): a
   String face then answers only part of the call, and the call's type meets
   the class method's answer. No when the walk leaves the values unbounded. */
int poly_recv_cls_method_owns(Compiler *c, int call, const char *name);
/* Can that receiver be a builtin value as well (nil, a literal, a builtin
   class's instance), which no class it lists stands for? 1 also when the
   walk cannot bound it. */
int poly_recv_builtin(Compiler *c, int call);
/* The same walk for a value no call takes as its receiver (an argument of
   `p`): the classes node v, or with elems one of its elements, can be an
   instance of, marked in set (c->nclasses entries); 0 when it cannot bound
   them. */
int pivs_value_classes(Compiler *c, int v, int elems, char *set);
/* The same for the boxed ivar ivn of class cid: the classes its stores can
   put there, marked in set; 0 when it cannot bound them. */
int pivs_ivar_classes(Compiler *c, int cid, const char *ivn, char *set);
/* --share-strings: the callable literals (a lambda, a proc's literal
   block, a top-level method's `method(:name)`) the receiver of call `call`
   can be, by the same walk: into out (at most cap), their count, or -1
   when it cannot bound them. */
int pivs_callables(Compiler *c, int call, int *out, int cap);
/* Frees the facts poly_ivar_set_reaches keeps on the compiler (c->pivs). */
void pivs_facts_free(Compiler *c);
void strbuf_arg_index_free(Compiler *c);

/* Set by main.c from --int-overflow=promote. In promote mode the analyzer is
   free to widen accumulating int locals to bigint more aggressively (e.g. block
   iteration loops, not just `while`), since the overflow-raising int macros are
   exactly what promote mode is asking us to avoid. Off (0) for raise/wrap, so
   the default gates and optcarrot (which pins wrap) see no behavior change. */
extern int g_promote_mode;
/* --int-overflow=wrap: an Integer stays a machine integer, wrapping; no
   local is widened to Bignum by the loop-growth detectors */
extern int g_wrap_mode;
void an_rat_boxed_mark(int id);
int an_rat_boxed(int id);
int an_rat_value(Compiler *c, int n);
extern int g_exc_abort;   /* --exceptions=abort (src/main.c) */
extern int g_gc_none;     /* --gc=none (src/main.c) */
/* is a def's value (the Symbol it names) used? par: an_parent_map */
int an_def_value_used(const NodeTable *nt, const int *par, int v);
int *an_parent_map(const NodeTable *nt);

/* Set by main.c from --plan-check (#7100): inference records, per call node,
   the builtin-op row it answered the call with (c->bop_inf), and codegen
   reports on stderr every call it emitted through a row inference did not
   choose. Off in every normal build. */
extern int g_plan_check;
/* --arm-check (#7483): each per-arm re-typing codegen runs (arm_body_infer)
   is held against the fact the analysis recorded for it (an_arm_facts);
   a missing fact or a type that differs is reported on stderr. Off in
   every normal build; the C is the same either way. */
extern int g_arm_check;
/* the per-arm facts (analyze_arm.c, #7483) */
typedef struct ArmFact { int block; TyKind kind; int n; int *nodes; TyKind *types; } ArmFact;
void an_arm_facts(Compiler *c);
const ArmFact *arm_fact_find(const Compiler *c, int block, TyKind kind);

/* The nil fact (analyze_nil.c, #7444): whether an object-typed value, or a
   builtin one held as a pointer (nil_fact_tracked), may be nil, decided once
   by the analysis for every node and every slot (a local,
   a parameter, a block parameter, a global, a constant, an ivar, a method's
   value). an_phase_value_types computes it, ahead of the value-type
   selection. nil_fact_node answers for a node, nil_fact_ivar for ivar `ivn`
   (with its '@') of class cid; the slots carry LocalVar.obj_may_nil and
   Scope.ret_obj_may_nil. repr_of and repr_of_slot read them into may_nil. */
enum { NF_UNKNOWN, NF_NOT_NIL, NF_MAY_NIL, NF_GUARDED /* not nil past a guard */ };
void an_nil_facts(Compiler *c);
/* Which constants a read can reach before their assignment: sets
   LocalVar.const_early on each one whose assignment is not settled
   (analyze_const.c). Runs once the constants are registered, ahead of the
   folds that decide `defined?(K)`. */
void an_const_presence(Compiler *c);
/* The program's files under --load-model=graph (analyze_program.c, #9898):
   their top-level scopes and roots, and what the model refuses for now. */
void prog_file_scopes(Compiler *c);
void prog_walk_files(Compiler *c);
void prog_refuse_loads(Compiler *c);
void prog_refuse_order(Compiler *c);
int prog_is_top_scope(const Compiler *c, const Scope *s);
int prog_is_root(const Compiler *c, int id);
int prog_is_root_stmts(const Compiler *c, int st);
int prog_root_stmts(const Compiler *c, int i);
int prog_defines_only(const Compiler *c, int i);
int prog_ordered_load(const Compiler *c, int id);
void prog_top_stmts(const Compiler *c, void (*fn)(void *ud, int stmt), void *ud);
int *prog_top_seq(const Compiler *c, int *n, int **file);
void prog_visit_files(const Compiler *c, void (*fn)(void *ud, int node, int file), void *ud);
/* Does node a run before node b, as the program runs its files
   (analyze_program.c)? PROG_UNKNOWN where no order is known: a file loaded
   only from a method, a condition or a block, against another file. */
enum { PROG_BEFORE = -1, PROG_SAME = 0, PROG_AFTER = 1, PROG_UNKNOWN = 2 };
int prog_order(Compiler *c, int a, int b);
void prog_sort(Compiler *c, int *v, int n, const int *node_of);
int prog_has_files(const Compiler *c);
int prog_cmp(Compiler *c, int a, int b, int at, const char *reader);
/* The load-phase record (analyze_elab.c, #9898): where each statement of
   the load phase runs, as a point (region, seq). Region 0 is the load phase
   the load graph places, each other region one file it does not place;
   seq is the order statements run in. region -1: no point (a method body,
   a lambda or a block run later). elab_cmp answers PROG_BEFORE / AFTER /
   SAME, or PROG_UNKNOWN across regions or without a point. */
typedef struct { int region, seq; } LoadPoint;
typedef struct LoadRecord LoadRecord;
LoadPoint elab_point_of(Compiler *c, int node);
int elab_point_may(Compiler *c, int node);
int elab_cmp(LoadPoint a, LoadPoint b);
int elab_order(Compiler *c, int a, int b);
void elab_top_place(Compiler *c, int node, int *file, int *rank);
long long elab_sort_key(Compiler *c, int node);
int elab_check_on(Compiler *c);
void elab_check_order(Compiler *c, const char *reader, int a, int b, int now);
void elab_check_sort(Compiler *c, const int *now, const int *rec, int n, const int *node_of);
void elab_dump(Compiler *c, FILE *out);
void elab_free(Compiler *c);
extern int g_dump_load_record;   /* --dump-load-record */
/* The load record's histories (analyze_elab.c), recorded by elab_program,
   which takes the scope walk's place: a method's versions by (class,
   singleton, name), with class -1 for main's (Object's) methods; a class's
   bodies (LrOpen) and mixins; the writes of globals (named without `$`),
   main's instance variables and constants. A version keeps its node and
   reads its point from the map (elab_point_of), so a rebuilt map moves
   none. The queries answer ELAB_YES / ELAB_NO / ELAB_UNKNOWN as of point
   p: what a statement in another region, or one that may not run, did
   before p is UNKNOWN. A p with no point (a method body) asks as of the
   end of the load phase. */
enum { ELAB_NO, ELAB_YES, ELAB_UNKNOWN };
typedef enum { LR_DEF, LR_DEFINE_METHOD, LR_READER, LR_WRITER, LR_ALIAS, LR_COPY, LR_UNDEF, LR_REMOVE, LR_VIS, LR_WRITE, LR_OPAQUE } LrKind;
typedef struct {
  int node;
  unsigned char kind;   /* LrKind */
  unsigned char vis;    /* SP_VIS_*, for a method version; BVIS_*, for a bare visibility word */
  unsigned char may;    /* recorded under a condition: may not have run */
  unsigned char nil;    /* a write of nil */
  const char *target;   /* an alias's or a copy's original name */
} LrDef;
typedef struct { int node, body; } LrOpen;
typedef enum { LR_INCLUDE, LR_EXTEND, LR_PREPEND } LrMixKind;
typedef struct { int node, kind, mod; } LrMixin;   /* kind: LrMixKind; mod: the module's class, -2 one the record does not know */
typedef enum { ELAB_GVAR, ELAB_MAIN_IVAR, ELAB_CONST } ElabVar;
void elab_program(Compiler *c);
/* the version of cls's method `name` (singleton when sg) live at p, through
   its ancestors as they stood at p; *out (when asked) the version found. A
   hook (`inherited`, `included`, `method_added`, ...) is asked as the
   singleton method it is: sg = 1. */
int elab_method_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p, const LrDef **out);
/* is module mod in the ancestors of cls (of its singleton when sg) at p? */
int elab_mixed_at(Compiler *c, int cls, int sg, int mod, LoadPoint p);
/* was a non-nil value written to the variable before p? (a constant: one
   the top level writes, Object's) */
int elab_written_before(Compiler *c, ElabVar kind, const char *name, LoadPoint p);
/* the bodies cls runs, in the order they run; *n their number */
const LrOpen *elab_class_bodies(Compiler *c, int cls, int *n);
/* the mixins run on cls (-1: main's, at a file's top level), one per
   module, in the order they run: `include A, B` as `include B` then
   `include A`, A nearest; *n their number */
const LrMixin *elab_class_mixins(Compiler *c, int cls, int *n);
/* what constant `name`, read at node, resolves to there: its lexical scopes'
   own constants, innermost first, then the innermost one's ancestors and
   Object's; *out the write or class definition found */
int elab_const_at(Compiler *c, int node, const char *name, const LrDef **out);
/* the visibility a `def` at node would take from its body: the last bare
   `public` / `private` / `protected` / `module_function` before it in the
   same body (BVIS_*), a class body's public and a file's top level's
   private by default; 0 when not known */
int elab_body_visibility_at(Compiler *c, int node);
/* method `name`'s visibility on cls (singleton when sg) at p: SP_VIS_*,
   -1 when not known or not defined */
int elab_method_vis_at(Compiler *c, int cls, const char *name, int sg, LoadPoint p);
/* did code the record cannot follow (a send, an eval of a String, a mixin
   of a module it does not know, a computed method name, a method of the
   program its body calls) run on cls before p? */
int elab_opaque_before(Compiler *c, int cls, LoadPoint p);
int nil_fact_node(const Compiler *c, int node);
int nil_fact_ivar(const Compiler *c, int cid, const char *ivn);
/* where a nil comes from: a node's (nil_fact_why), a slot's flag itself.
   A value with several sources is reported with the first in this order:
   the nils the program writes, then the ones the analysis cannot bound. */
enum {
  NFW_NONE,      /* not nil */
  NFW_NIL,       /* a nil written: a literal, an empty body, a bare return */
  NFW_NO_ELSE,   /* an if, unless or case with no branch for the other case */
  NFW_SAFE_NAV,  /* a `&.` call */
  NFW_UNSET,     /* a local's read that can run before any write (the
                    definite-assignment walk, a `||=` slot) */
  NFW_ELEM_NIL,  /* an element of an Array the program stores nil into or
                    leaves a gap in (a block parameter an iteration over it
                    binds, an element read or pick out of it) */
  NFW_TAKEN,     /* a pop, shift or delete_at of an object Array, which
                    answers nil once the Array is empty or the index is
                    outside it */
  NFW_ELEM,      /* an element read or a pick that can miss (Array, Hash,
                    String) */
  NFW_GLOBAL,    /* a global, or the main object's ivar, read where no write
                    can be shown to run first (a method's read of one) */
  NFW_IVAR,      /* an ivar some class's initialize does not set first, or one
                    of a class or a module */
  NFW_CALLER,    /* a parameter a caller the analysis does not see binds (a
                    send, a method(:m), a proc or a lambda, a callback) */
  NFW_OPAQUE,    /* a value the analysis does not model: a builtin's answer, a
                    splat's element, a pattern's binding, a yield's value */
  NFW_GUARDED    /* (nil_fact_why only) not nil: a guard narrowed the read */
};
int nil_fact_why(const Compiler *c, int node);
/* Can an element of Array node `node`'s value be nil: one whose elements
   are pointers that the program stores nil into or leaves a gap in
   (NFW_ELEM_NIL's Arrays)? */
int nil_elem_fact_node(const Compiler *c, int node);
/* Does the fact track a value of type t: an object, or a builtin held as a
   pointer that is NULL for nil (a String, an Array, a Hash, an IO)? */
int nil_fact_tracked(TyKind t);

/* One post-convergence bind pass fills UNKNOWN params from empty
   array-literal args (fst([]) with def fst(a) = a.first). */
extern int g_final_bind_pass;
/* The pass that decides container seeds a box reached mid-fixpoint (param_seed_holds, ret_seed_holds, class_ivar_pin_holds). */
extern int g_seed_box_settled;

/* Unified value type of a slot's `recv[k] = v` writes; nwrites (optional)
   reports how many contributed, so a caller can tell "no evidence" from
   "evidence not derived yet". Defined in analyze_pass.c. */
TyKind aset_value_type_ex(Compiler *c, int recv, int *nwrites);
TyKind local_aset_key_type(Compiler *c, Scope *sc, const char *name, int *nwrites);

/* Run inference over the whole program: register locals, reach a fixpoint
   on their types, and fill the node type cache. */
void analyze_program(Compiler *c);
int lower_rows_check(const NodeTable *nt);   /* SPINEL_LOWER_ROWS_CHECK: the parse's lowering rows against the node's */
/* The program as written gives no class a method named `nm`, beyond `defs`
   (0 or 1) defs of that name: read ahead of the desugars, by absence. */
int an_prog_never_gives(const char *nm, int defs);
int an_prog_names_method(const char *nm);
/* Kernel itself as call id's constant receiver (analyze_util.c) */
int an_kernel_const_recv(Compiler *c, int id, int recv, int fresh);
int an_kernel_backtick_recv(Compiler *c, int id, int recv, int fresh, int in_default);
int *an_blank_slates_lacking(Compiler *c, const char *name, int *n);
/* The argument shape of a `super` in a ` override (analyze_util.c) */
int an_super_backtick_shape(Compiler *c, int id, const Scope *s, int *arg);
int an_prog_meta_by_value(void);
/* 1 or -1 when `id` is that Integer literal, parenthesized or not; 0 otherwise */
int an_unit_int_literal(const NodeTable *nt, int id);
/* a push onto a typed Array a builtin call just built, given an element of
   another kind: it answers the general Array (analyze_infer.c) */
int an_push_widens_fresh(Compiler *c, int id);
/* ...and onto one a boxed receiver's builtin built (`x.to_s.scan(re) << 99`
   with x.to_s boxed): the kinds the push stores (AN_LIFT_*), for which the
   receiver's builtin arms box their typed Array as the general one
   (analyze_infer.c) */
enum { AN_LIFT_INT = 1, AN_LIFT_FLOAT = 2, AN_LIFT_STRING = 4, AN_LIFT_SYMBOL = 8, AN_LIFT_OTHER = 16 };
int an_push_lifts_boxed(Compiler *c, int id);
/* a builtin of this name answers a new Array where it answers one (analyze_pass.c) */
int fresh_array_builtin_name(const char *nm);
/* the value of node n is dropped where it is: a statement's, a loop
   body's, an ensure clause's (an_value_dropped, analyze_infer.c) */
int an_node_dropped(Compiler *c, int n);
/* True if a regex source contains a capturing group: an unescaped '(' that
   isn't the start of a non-capturing/extension group '(?...'. scan returns
   nested arrays for capturing patterns, which the str_array path can't model. */
int an_re_has_captures(const char *src);
/* --share-strings: can method mi's last statement answer nil (a nil, or
   a conditional with an arm that is nil or missing)? The deep-return
   pickup of a call to it then answers the call's nil as nil
   (an_tail_is_shared_handle). */
int an_tail_answers_nil(Compiler *c, int mi);
/* --share-strings: does method mi answer a shared String's handle it
   publishes (Scope.ret_handle, with its ret_nil_pickup, ret_pub_fresh and
   ret_pub_bang), or a new String or a parameter on every path
   (Scope.ret_fresh, ret_param)? While an_mark_handle_returns decides them,
   a method not decided yet is decided where it is asked, whatever order
   the methods are defined in; after it, the facts as decided. */
int scope_ret_handle(Compiler *c, int mi);
int scope_ret_fresh(Compiler *c, int mi);
/* Does call's iterator drop what its block answers: one of the each family,
   builtin, or every program method of the name yields only where the value
   is dropped and hands its block on only to such an iterator? */
int an_iter_discards_block(Compiler *c, int call);
/* Does method mi answer a String literal on every path (no `return`)? A
   frozen literal keeps one identity, the handle its site has
   (SP_SHARE_STRING_LITERALS), which a handle made over its bytes finds. */
int scope_ret_str_literal(Compiler *c, int mi);
int scope_ret_param(Compiler *c, int mi);
/* A String `||` or `&&` over a String left operand: a tail whose operands
   are its arms (an_tail_is_shared_handle, emit_tail_value). */
int an_tail_logic_op(Compiler *c, int node);
int an_send_name_is_computed(Compiler *c, int arg);
/* Is scope si an iterator synth_struct_each generated, not a def? */
int scope_is_struct_synth(Compiler *c, int si);
int an_str_mutator_name(const char *nm);
/* Does inlined yielding method mi lend its parameter j at a call whose
   literal block is blk: append to it, hand it to a lent parameter, or yield
   it to a block parameter the block lends? The splice then binds it as an
   alias of the caller's variable. */
int an_inline_param_lent(Compiler *c, int mi, int j, int blk);
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
typedef struct { int unknown, app, keeps, n; TyKind ty[4]; } DynParams;
void dyn_blk_params(Compiler *c, int mi, int k, DynParams *out);
int dyn_open_site(Compiler *c, int n, int *shift);
void dyn_open_reach(Compiler *c, int n, int k, DynReach *r);
int dyn_method_appends(Compiler *c, int mi, int j);
/* The code under node may append to local vn in place, or hand it to a
   parameter that is lent (a byref slot or the handle): 1 also for a
   subtree too deep to be walked to the bottom */
int an_subtree_may_lend_local(Compiler *c, int node, const char *vn);
int an_local_array_changed_x(Compiler *c, const char *xn, Scope *xs);
int an_local_array_stores_unshared(Compiler *c, const char *xn, Scope *xs);
/* 1 appends, 0 does not, -1 cannot tell (refused as appending) */
int fwd_rest_elem_appends(Compiler *c, int mi, int i);
int fwd_poly_param_appends(Compiler *c, int mi, int j);
int refl_im_params_call(Compiler *c, int id);
/* A boxed parameter's argument a literal block appends to through a yield
   (yield_splice_handles): a String variable there must be the handle. */
int yield_poly_arg_wants_handle(Compiler *c, int a);
/* A literal block a splat into a yield or instance_exec reaches, whose every
   parameter it appends to takes the handle the gathered Array holds
   (block_splat_pull_args). */
int block_splat_shares(Compiler *c, int blk);
int fwd_param_appends_at(Compiler *c, int mi, int j);
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
/* Does a receiver of type `rt` have a real method named `name`, among the
   per-call-site-cloned generic Enumerable names (analyze_desugar.c's
   builtin_enum_name_index list) or transform_keys!/transform_values!?
   Used by rt_probe_answer (codegen_call.c) for a respond_to? probe on
   one of these names, which the one-shot rewrites that actually type
   them never reach (K-026 cause A). */
int enum_builtin_responds(Compiler *c, const char *name, TyKind rt);
int enum_unrename(NodeTable *nt, int id);
/* empty infer_type's memo of earlier queries' answers: the start or end of
   a pass whose writes are not all counted (#8852) */
void infer_memo_boundary(void);
void infer_memo_forget_since(unsigned long at);
/* A pure read of the settled analysis (repr_of) asks its questions between
   an_pure_read_begin and an_pure_read_end. infer_type answers as usual but
   records nothing it derives: not the node-type cache, a poly call's
   builtin answer, --plan-check's call records, a block parameter's pinned
   type, the narrowing memo or a call's alias resolution (its name and
   builtin_only, kept for the inference asking). So asking cannot change
   what codegen reads next. They nest. */
void an_pure_read_begin(void);
/* pin one node against infer_type recording its answer; returns the previous pin */
int an_pin_node(int id);
void an_pure_read_end(void);

/* String#lines' argument shapes besides none: (sep), (chomp: ...) and
   (sep, chomp: ...), sep a String -- what a boxed receiver takes the
   typed String path for. */
int poly_lines_args(Compiler *c, int argc, const int *argv);
int lines_sep_ty(Compiler *c, int a);

/* `recv` is a blockless call making an Enumerator that yields two values per
   element: each_with_index, with_index, each_with_object, with_object. */
int enum_pair_source_call(const NodeTable *nt, int recv);

/* map and the selecting Enumerables, which a boxed Array answers a blockless
   call of with an Enumerator, as it does each (analyze_infer_recv.c) */
int poly_blockless_enum_name(const char *name);

/* True when node `id`'s value, held in an unboxed scalar slot, can be the
   nil it carries beside the value (an sp_oint / sp_ofloat). The slot type alone
   cannot say -- an `Integer?` and an `Integer` are both TY_INT -- so codegen
   asks this before choosing between sp_box_int and sp_box_oint at a poly
   boundary. Valid only after analyze_program has settled the marking. */
int nullable_int_value(Compiler *c, int id);
/* The call node nullable_int_value is asked of for its builtin answer
   alone (a poly dispatch's builtin arm re-entering it): the program's
   methods and readers of the name are no targets there. -1 when none. */
extern int g_nn_builtin_call;
/* 1: the typed Integer-valued Hash `node` evaluates to (TY_STR_INT_HASH /
   TY_INT_INT_HASH) may hold a nil VALUE; 0 for every other kind and for
   node < 0 (DESIGN.md D3b-ii) */
int hash_vals_nullable(Compiler *c, int node);
/* The same, asked of the variable rather than of the read: what it can hold
   anywhere, the nil narrowing's facts left out. */
int nullable_int_value_raw(Compiler *c, int id);
/* `@x = v` at write node id leaves @x's nil bit clear, as it already was */
int nn_ivar_write_keeps_clear(Compiler *c, int id);
/* `a[i]` on a pointer array proven to read within it (no nil past the end) */
int an_elem_read_in_bounds(Compiler *c, int id);
/* is position pos of a multiple assignment's value, read from a constant Array of a proven length, within it and no nil (a position from the end is not asked)? */
int masgn_const_elem_in_range(Compiler *c, int value, long long pos);
int scalar_nil_only_call(Compiler *c, int id, TyKind rt);
int nullable_scalar_nil_only_call(Compiler *c, int id);
int nullable_int_elem_array(Compiler *c, int node);
int elem_nil_reach_expr(Compiler *c, int v);
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
int an_reopen_owner(Compiler *c, int id, const char *cn);   /* a builtin class reopen's method of the call's name, or -1 */
int range_lit_float_end(Compiler *c, int recv);   /* (1..5.5): the Float end node, else -1 */
int range_lit_endless(Compiler *c, int recv);     /* (1..): an endless literal with a begin */
int reduce_tail_from_acc(Compiler *c, int tail, const char *accp);

/* True if `node` (a block body / statements subtree) contains a top-level
   `break` that binds to the enclosing block -- i.e. not captured by a nested
   loop or block-bearing call. */
int block_has_top_break(Compiler *c, int node);
/* A `while true` / `until false` with no break of its own: it never
   completes normally, so it answers no value. */
int loop_never_completes(Compiler *c, int node);
/* True if CallNode `id` takes a literal block whose body has a top-level
   break and is an inlined iterator the break wrapper should catch (a real
   receiver, not instance_exec/eval). When true, the call returns the break
   value on break, so its result type widens to poly. */
int call_breaks(Compiler *c, int id);
int an_poly_str_upto(Compiler *c, int id);   /* a boxed upto a String receiver may take */
int an_int_step_range_chain(Compiler *c, int id); /* a blockless times/upto/downto kept as its Range */
int an_blockless_tuples_kept(Compiler *c, int id); /* a blockless combination & co. kept as its Array */
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
/* infer_type answered node id as an Array subclass instance's Array (#7449) */
void an_ary_viewed_mark(Compiler *c, int id);
int an_ary_viewed(Compiler *c, int id);
/* Pin/read the receiver node the inference should answer as `kind` while
   codegen re-enters a typed emitter for a boxed receiver (the face table in
   types.h). Node -1 clears the pin. */
/* The face kind node is pinned to (the face table, types.h): the innermost
   pin, codegen's on the view stack (view_push_face) or inference's own
   (an_face_push / an_face_pop), answers for its node; TY_UNKNOWN for any
   other node, or when none is pinned. face_active() says whether one is. */
TyKind face_of(int node);
int face_active(void);
/* how many codegen views are open (codegen_view.c): a view pins nodes'
   types while an arm is emitted, 0 outside the emission */
int view_depth(void);
void an_face_push(int node, TyKind kind);
void an_face_pop(void);
int an_face_pinned(int node);
int view_face_top(int *node, TyKind *kind);   /* codegen_view.c */
/* Name of a block's idx-th required parameter, or NULL. */
const char *block_param_name(Compiler *c, int block, int idx);
int native_call_ret_plain_num(Compiler *c, int v);   /* a native method declared :int / :float */
int param_strict(Compiler *c, int mi, int k, const char **what, int *ord);   /* a parameter whose nil raises at its first use */
int block_param_used_up(Compiler *c, int blk, const char *nm, int value);
/* The name of a numbered block parameter (`_1`..`_9`) on this parameters node.
   Per BLOCK where a scope holds more than one such block; see
   scope_numbered_block_params. Every site that needs the name goes here. */
const char *numbered_param_name(Compiler *c, int params_node, int idx);
/* Name of a block's trailing rest parameter (`|*a|`), or NULL. */
const char *block_rest_name(Compiler *c, int block);
const char *block_opt_name(Compiler *c, int block, int idx);
const char *block_param_at(Compiler *c, int block, int idx, int n);
int call_plain_argc(Compiler *c, int call);
/* The local a String's own conversion (`s.to_s`) reads, or -1 (analyze.c). */
int an_str_conversion_local(Compiler *c, int v);
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
int an_program_makes_dir(Compiler *c);   /* the program names Dir anywhere */
int an_program_makes_bsearch_enum(Compiler *c);   /* Array#bsearch without a block, anywhere */
int an_program_makes_io(Compiler *c);   /* a boxed value may be an IO (a syntactic test) */
int an_zero_arg_builtin_shadowed(Compiler *c, const char *name, int argc);
int an_user_recv_defines_method(Compiler *c, const char *name);
/* obj.methods / public_methods / singleton_methods on an instance of `cid`
   fold to a static symbol list */
int an_object_methods_listable(Compiler *c, int cid, const char *name);
int an_module_methods_listable(Compiler *c, int m, const char *name);
int an_module_defines(Compiler *c, int m, const char *nm, int *vis);
int an_object_methods_all_arg(Compiler *c, int cid, int argc, const int *argv);
/* obj.methods / public_methods on a builtin value: the core class it lists as */
const char *an_value_methods_class(Compiler *c, int recv, TyKind rt, const char *name, int argc,
                                   const int *argv);
int an_class_singleton_methods_listable(Compiler *c, int cid);
int an_singleton_methods_foldable(Compiler *c, const char *cn);
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
size_t reassigned_param_written_len(const char *name);
int block_param_is_renamed(const char *name);
char *block_param_invent_name(Compiler *c, const char *written, int blk);
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

/* Can a Class value reach a receiver the analysis cannot pin? Memoized
   alongside dynamic constructor binding; shared with boxed ivar emission. */
int class_value_escapes(Compiler *c, int cid);

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
int std_stream_boxed(Compiler *c, const char *nm);   /* $stdout/$stderr may hold a non-IO */
int gvar_has_write(Compiler *c, const char *name);   /* is global name (no `$`) assigned anywhere */
int std_stream_assigned(Compiler *c, const char *nm);   /* ... or is assigned at all */
int std_stream_value_classes(Compiler *c, char *set);   /* classes the streams' stores hold; 0: unbounded */
int std_stream_foreign_writer(Compiler *c, const char *nm);   /* an object output cannot write to */
int std_stream_bare_call_own(Compiler *c, int id, const char *nm);   /* bare nm reaches the program's own */
/* The operation (StrhOp, builtin_names.h) call node `call` is on a String
   Array, whose elements are handles, given the receiver's type rt:
   STRH_NONE unless rt is TY_STR_ARRAY, the call's index
   arguments are Integers, no argument is a splat, a keyword Hash or a block
   argument, and no method of the program's own stands in for Array's. */
int strh_call_op(Compiler *c, int call, TyKind rt);
/* ...and the same for the gate's rounds, where an argument whose type is
   not settled yet counts as the type asked (the gate runs again, and
   refuses for good, once it is) */
int strh_call_op_in(Compiler *c, int call, TyKind rt, int unknown_ok);
int strh_literal_held(Compiler *c, int n);   /* an Array literal a variable of any kind is written */
int strh_var_read(const NodeTable *nt, int n);   /* a variable's read, whatever its kind of variable */

#endif
