/* codegen_call_arms.h -- emit_call_body's arms, by family, in the
   codegen_call_<family>.c files. emit_call_body (codegen_call.c) calls each
   at the position its arms held, in the same order; each answers 1 when it
   emitted the call, 0 to leave it to the arms after it. The helpers of
   codegen_call.c those arms call are declared here too. */
#ifndef SPINEL_CODEGEN_CALL_ARMS_H
#define SPINEL_CODEGEN_CALL_ARMS_H

#include "codegen_internal.h"
#include "codegen_poly.h"
#include "builtin_ops.h"
#include "call_plan.h"

int array_fill_boxes_handle(Compiler *c, int id, int fill);
/* main's own `to_s` or `inspect` (cplan_main_text_call): main's text, the
   String sp_poly_to_s answers for main, with no dispatch and no box for its
   receiver, which is main. emit_call_inner asks it ahead of the arms. */
int emit_main_text_call(Compiler *c, int id, Buf *b);

/* ---- the arms ---- */
int emit_call_synchronize_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);
int emit_call_bigint_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_identity_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_instance_eval_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc);
int emit_call_freeze_dup_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc);
int emit_call_safe_nav_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);
int emit_call_object_override_arms(Compiler *c, int id, Buf *b, const NodeTable *nt);
int emit_call_print_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_compare_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_operator_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0);
int emit_call_reflection_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_new_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_class_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
void emit_class_value_constants(Compiler *c, const char *cv, int inherit, Buf *b);
int emit_call_class_method_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_root_reopen_call(Compiler *c, int id, Buf *b, const NodeTable *nt, int recv, int ci, int mi);
int emit_call_reopen_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, TyKind rt);
int emit_call_regexp_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0);
int emit_call_regexp_class_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_symbol_bool_string_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_file_dir_time_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_marshal_value_call(Compiler *c, int id, Buf *b);   /* codegen_call_module.c */
int emit_call_module_fn_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_builtin_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_enum_random_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_exception_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_raise_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);
int emit_call_kernel_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_io_select(Compiler *c, int argc, const int *argv, Buf *b);
int emit_call_kernel_flow_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_append_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_store_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_array_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_array_random_kw(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_poly_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_handle_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_method_obj_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_poly_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_poly_builtin_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_openstruct_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_hash_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_range_literal_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0);
int emit_call_iter_expr_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_display_ivar_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_symbol_misc_arms(Compiler *c, Buf *b, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_untyped_array_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, TyKind rt);
int emit_call_handle_op_arms(Compiler *c, int id, Buf *b, const char *name, int recv);
int emit_call_implicit_self_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);
int emit_call_proc_literal_arms(Compiler *c, int id, Buf *b, const NodeTable *nt);

/* ---- codegen_call.c's helpers the arms call ---- */
/* An ordered, de-duplicated list of method names for the reflection folds.
   A name is recorded once, at its first (most derived) sighting, and listed
   only if that sighting was visible, so a subclass that makes an inherited
   method private hides the ancestor's public one. show[i] holds REFL_SHOWN
   for a visible sighting and REFL_ROW where a builtin row made it (a
   method present from the start, version 0, which the row answers). at:
   the list call the names answer (its node + 1, 0 for a list no one site
   has), at_cls and at_side the receiver class and side it asks there
   (refl_at, cplan_list_omits). */
enum { REFL_SHOWN = 1, REFL_ROW = 2 };
typedef struct { char **v; unsigned char *show; int n, cap, at, at_cls; unsigned at_side; } ReflNames;
/* The classes a `k.new(...)` switch gives an ArgumentError arm, grouped by
   message so each message is emitted once. */
typedef struct { char msg[256]; int *cis; int ncis; } CtorArityArm;
typedef struct { CtorArityArm *arms; int narms; } CtorArityArms;
void emit_bigint_operand(Compiler *c, int node, Buf *b);
int cmp_operand_may_be_nil(Compiler *c, int id);
int emit_poly_isa_test(Compiler *c, const char *cn, const char *v, int exact, Buf *b);
void emit_pre_root(Compiler *c, TyKind t, int tmp);
int emit_scalar_class_test(Compiler *c, int node, TyKind t, const char *cn, int exact, Buf *b);
int hoist_boxed_rooted(Compiler *c, int node);
TyKind user_cmp_invalid_ret(Compiler *c, int cid);
int user_cmp_needs_check(Compiler *c, int cid);
void emit_ie_param_default(Compiler *c, TyKind t, Buf *b);
int emit_ie_poly(Compiler *c, int id, Buf *b);
int emit_ie_proc(Compiler *c, int id, int recv, int self_cls, int blk, int tramp, Buf *b);
int emit_ie_untraced(Compiler *c, int id, int recv, int blk, Buf *b);
int emit_ie_non_proc_block(Compiler *c, int id, int recv, int blk, Buf *b);
extern int g_ie_poly_node;
int ie_body_has_break_next(Compiler *c, int node);
int ie_body_ivar_write(const NodeTable *nt, int node);
TyKind ie_splice_value_ty(Compiler *c, int node);
int basicobject_own_method(const char *n);
void emit_poly_call2_ordered(Compiler *c, const char *fn, const char *lead, int recv, int arg, Buf *b);
void emit_poly_cmp_ordered(Compiler *c, const char *fn, int recv, int arg, Buf *b);
void emit_handle_inspect(Compiler *c, int recv, TyKind rt, Buf *b);
int bigint_cmp_operand_ok(TyKind t);
int emit_bigint_cmp_inexact(Compiler *c, int recv, int arg, const char *op, Buf *b);
Buf emit_cmp_self(Compiler *c, int recv, TyKind rt, int *tmp);
int emit_float_bigint_cmp(Compiler *c, int recv, int arg, const char *op, Buf *b);
void emit_int_flt_rel(Buf *b, const char *iv, const char *fv, int int_left, const char *op);
int emit_int_float_cmp(Compiler *c, int recv, int arg, const char *op, Buf *b);
int int_flt_lit_exact(Compiler *c, int id);
int emit_int_operand_fail(Compiler *c, int id, int recv, int arg, int is_shift, Buf *b);
int exc_subclass_defines_cmp(Compiler *c);
int obj_cmp_by_identity(TyKind t);
int object_defines_cmp(Compiler *c);
int any_class_defines(Compiler *c, const char *qm);
int any_class_responds(Compiler *c, const char *qm);
int class_implicit_responds(Compiler *c, int cid, const char *qm);
int class_struct_kind(Compiler *c, int cid);
int class_value_responds(Compiler *c, int tv, const char *qm, Buf *b);
void class_value_responds_runtime(int tv, Buf *b);
void emit_class_value_responds_helper(Compiler *c, const char *const *texts, int ntexts, Buf *protos, Buf *defs);
extern const char *const comparable_names[];
int const_name_is_wrong(const char *s);
int emit_io_pick(Compiler *c, int tv, const char *name, int public_only, Buf *b);
void emit_io_pick_public(Compiler *c, const char *qm, const char *pick, Buf *b);
void emit_io_reopen_responds(Compiler *c, int tv, const char *qm, int include_all, Buf *b);
void emit_poly_dispatch_key(Compiler *c, int tv, int cls0_cand, int prim_cand, int exc_cand, Buf *b);
void emit_responds_name(Compiler *c, int k, const char *nm, Buf *pub, Buf *priv);
extern const char *const enumerable_names[];
extern int g_rto_nil_obj;
extern int g_rto_by_cls;
int emit_respond_to_by_cls(Compiler *c, int recv, const char *qm, int argc, const int *argv, Buf *b);
extern int g_rto_exc;
extern int g_rto_force_ia;
int emit_respond_to_exc_cls(Compiler *c, int recv, const char *qm, int argc, const int *argv, Buf *b);
int method_hidden_from_reflection(Compiler *c, int cid, const char *m);
int refl_member_of_struct_superclass(Compiler *c, int ci, const char *m);
int respond_to_static_answer(Compiler *c, int id, int recv, TyKind rt, const char *qm, int argc, const int *argv);
/* nil's static answer to respond_to?(qm, ...); *prog: by a method of the program's */
int nil_responds_static(Compiler *c, const char *qm, int argc, const int *argv, int *prog);
/* respond_to? call id (its argc arguments argv, asking qm) of a class's own
   class method whose answer the version cells hold (cplan_cmethod_presence):
   one of the versions' cells, or what the class answers with none of them,
   written to b. 0, writing nothing, for a call the plan does not answer
   so; with b NULL, only asked. */
int emit_respond_to_cells(Compiler *c, int id, const char *qm, int argc, const int *argv, Buf *b);
int respond_to_user_defined(Compiler *c, int id, int recv);
extern const char *const respond_universal[];
extern const char *const respond_universal_all[]; /* public and private */
int object_private_method_name(const char *qm);
const char *const *object_private_method_names(void); /* NULL-terminated */
int respond_to_runtime_reaches(Compiler *c, TyKind rt);
int root_reopen_defines(Compiler *c, const char *qm);
/* 1 when the program's reopen of the builtin class or module `cls` (its
   chain) defines `name` */
int builtin_reopen_owns(Compiler *c, const char *cls, const char *name);
int bool_side_responds(Compiler *c, const char *qm, int include_all, int foldable, int *ta, int *fa);
int root_reopen_defines_own(Compiler *c, int k, const char *qm);   /* a root reopening k defines qm itself (def, attr_reader, attr_writer) */
int root_reopen_method_vis(Compiler *c, int cid, const char *qm);   /* an Object, Kernel or BasicObject reopening's method qm: its visibility, or -1 */
int private_answer_overridden(Compiler *c, const char *qm, TyKind rt);
int refl_md_dynamic_used(Compiler *c);
void emit_scope_parameters_pushes(Compiler *c, int mi, int tr4, Buf *b);
int refl_im_params_used(Compiler *c);
int emit_refl_im_params_call(Compiler *c, int id, Buf *b);
void emit_refl_im_params(Compiler *c, Buf *b);
void emit_refl_method_defined(Compiler *c, Buf *b);
int rt_probe_answer(Compiler *c, int id, int *yes);
extern const char *const struct_names[];
void emit_call_body(Compiler *c, int id, Buf *b);
void emit_ffi_callback_arg(Compiler *c, int cbidx, int argnode, Buf *out);
void emit_ffi_num_arg(Compiler *c, int arg, TyKind at, const char *spec, int dbl, Buf *b);
int emit_or_take_back(Compiler *c, int id, Buf *b, int (*fn)(Compiler *, int, Buf *));
int emit_sg_accessor(Compiler *c, int id, int ci, const char *cn, const char *name, int argc, const int *argv, Buf *b);
/* Does a value of class k answer call `call`'s `name` by a `class << self`
   accessor ahead of any class method of the name? And the arms of those
   classes in the class-tag ternary over the boxed value in _t<tv>. */
int cls_sg_accessor_first(Compiler *c, int call, int k, const char *name, int argc);
void emit_boxed_cls_sg_first_arms(Compiler *c, int id, const char *name, int argc, const int *argv, int tv, Buf *b);
int cls_value_sg_accessor_named(Compiler *c, const char *name, int argc);
int emit_cls_value_sg_accessor_of(Compiler *c, int id, int recv, const char *ktext, const char *name, int argc,
                                  const int *argv, Buf *b);
void ffi_check_buffer_bounds(Compiler *c, int id, int arg, const char *kind, int off, const char *dir);
extern int g_setter_value_inner;
int emit_cmethod_setter_assign(Compiler *c, int id, Buf *b);
int ctor_alloc_decl(Compiler *c, int cid, Buf *b);
char *ctor_self_text(Compiler *c, int cid, int initm, int t);
int ctor_new_runs(Compiler *c, int cid, int initm);
void ctor_arity_add(CtorArityArms *g, int ci, const char *msg);
void ctor_arity_emit(CtorArityArms *g, Buf *b);
int ctor_arity_error(Compiler *c, int ci, int initm, int argc, char *msg, size_t n);
void ctor_arm_arg(Compiler *c, Scope *is, int j, const char *val, int pd_uid, Buf *pdpre, Buf *args);
int ctor_arm_incompat(Compiler *c, Scope *is, const ArgLayout *L, const int *argv);
int ctor_arm_takes(Compiler *c, Scope *is, int argc);
const char *ctor_blk_lead(const Buf *b);
int ctor_block_dispatchable(Compiler *c, int id);
int ctor_block_splices(Compiler *c, int id);
void emit_builtin_new_arms(Compiler *c, int argc, const int *atmp, int rt2, int kt, int boxed, Buf *b);
int emit_class_new_call(Compiler *c, int id, Buf *b);
void emit_class_value_new_kw(Compiler *c, int id, int recv, int boxed, Buf *b);
/* The head of a Class-value `new` switch on `_t<kt>` (an sp_Class, or a
   boxed value when `boxed`), its result `_t<rt2>` seeded nil. A box's
   cls_id selects a constructor only when it holds a Class: an Integer's, a
   Symbol's or nil's can equal a user class's index, and a user
   object's is its class's, so anything else
   switches on -1, no arm's label, and takes the default's NoMethodError. */
void emit_class_value_new_switch(Compiler *c, int rt2, int kt, int boxed, Buf *b);
void emit_ctor_arm_case(Compiler *c, int ci, int initm, int rt2, int self_t, const char *pre, const char *args, int given, Buf *b);
void struct_nil_fill_stmts(Compiler *c, int ci, int given, const char *obj, Buf *out);
void emit_ctor_arm_param(Compiler *c, Scope *is, int j, const ArgLayout *L, const int *atmp, Buf *pdpre, Buf *out);
void emit_ctor_block_slot(Compiler *c, int id, int initm, const char *lead, Buf *b);
int emit_ctor_splice_arm(Compiler *c, int id, int ci, int initm, int rt2, Buf *b);
void emit_exc_msg_arg(Compiler *c, int arg, Buf *b);
void emit_exc_new_msg(Compiler *c, const char *cn, int arg, Buf *b);
int emit_exc_sub_new_arm(Compiler *c, int ci, int argc, const int *atmp, int rt2, Buf *b);
int emit_exc_sub_new_arm_splat(Compiler *c, int ci, int tsa, const char *pre, int rt2, Buf *b);
int emit_syserr_family_new(Compiler *c, int id, const char *cn, int argc, const int *argv, Buf *b);
int emit_user_new_arm(Compiler *c, int id, int ci, int argc, const int *atmp, int hoisted, int rt2, Buf *b);
extern int g_ctor_blk_tmp;
int hoist_ctor_block(Compiler *c, int id, Buf *b);
int struct_nil_fills(const ClassInfo *k);
int collect_class_constants(Compiler *c, int ci, int inherit, const char **out, const char **shown,
                            int max, int n, int depth);
void emit_class_id_is(Compiler *c, int cname, const char *expr, Buf *b);
int emit_reopen_arm_test(Compiler *c, int kmi, const char *name, const char *cls, const char *raise, Buf *b);
/* The ArgumentError text of Module#name / to_s / inspect given these
   arguments; 0 when there are none or only the run time knows the count */
int class_name_reader_arity_msg(Compiler *c, const char *nm, int argc, const int *argv, char *am, size_t n);
extern int g_cls_value_recv;
int hoist_dispatch_args(Compiler *c, int argsN, int **sv, int **vw);
/* A boxed Integer or Float at a call no target answers: the arms that
   answer it through the scalar kinds' rows ahead of the NoMethodError. */
typedef struct { int open, arms, hn, bslot, *hsv, *hvw; } PolyScalarArms;
int poly_scalar_arms_open(Compiler *c, int id, PolyScalarArms *pa, Buf *b);
void poly_scalar_arms_close(Compiler *c, PolyScalarArms *pa, Buf *b);
int obj_member_shadows(Compiler *c, TyKind rt, const char *nm);
void refl_class_singleton_methods(Compiler *c, int ci, int all, ReflNames *r);
void refl_builtin_singleton_methods(const char *cls, int all, ReflNames *r);
void refl_class_instance_methods(Compiler *c, int ci, int pub, int prot, ReflNames *r);
void refl_module_instance_methods(Compiler *c, int m, int prot, ReflNames *r);
void refl_module_own_methods(Compiler *c, int m, int pub, int prot, int priv, ReflNames *r);
void refl_emit_sym_array(Compiler *c, int cid, ReflNames *r, Buf *b);
void refl_at(ReflNames *r, int id, int cls, unsigned side);
void refl_free(ReflNames *r);
void refl_own_instance_methods(Compiler *c, int ci, int pub, int prot, int priv, int with_modules, ReflNames *r);
void unhoist_dispatch_args(Compiler *c, int n, int *sv, int *vw);
int emit_ctor_new_with_proc(Compiler *c, int id, int ci, Buf *b);
void emit_struct_kw_check(Compiler *c, ClassInfo *cls, int ht, int kwh);
int emit_struct_kw_hash(Compiler *c, int kwh);
void emit_struct_kw_member(Compiler *c, ClassInfo *cls, int a, int ht, Buf *b);
int emit_struct_new_early(Compiler *c, int ci, int argc, const int *argv, int kwh, int *nunk_out, int *late_out, Buf *b);
void emit_io_reopen_call(Compiler *c, int id, int recv, const char *name, Buf *b);
int emit_reopen_block_call(Compiler *c, int id, int recv, int mi, const char *box_fn, Buf *b);
void emit_reopen_pf_call(Compiler *c, int id, int pf, int cblk, const char *recv_text, const char *box_fn, Buf *b);
/* `fmt % args` where the format is a String made where it is written (an
   interpolation, a call's result) and the arguments allocate: nothing holds
   the format while they are built, and C does not say which of the two is
   built first. Opens the call with the format in a rooted temp ahead of the
   arguments, `({ const char *_tN = fmt; SP_GC_ROOT_STR(_tN);
   sp_str_format_polyarr(_tN`, with the nil check where `fck` names the
   temp, and answers the text that closes it after the arguments. NULL, with
   nothing written, for a format something else holds while the arguments
   are built (a literal, a read that cannot allocate, a read out of a slot),
   and for a call's answer beside an argument `arg` that runs code: that
   code may append to the String the call answered, which a temp read
   ahead of it would miss. The caller then emits the call as before. */
const char *emit_str_format_held(Compiler *c, int recv, int arg, int fck, Buf *b);
/* That last case: the format is a call's answer and `arg` runs code, so the
   whole `recv % arg` is emitted as it was before the format was held. */
int str_format_left_alone(Compiler *c, int recv, int arg);
int reopen_yields_container(Compiler *c, Scope *m);
int emit_str_format_untyped_array(Compiler *c, int recv, int a0n, int fck, Buf *b);
void emit_voided_operands(Compiler *c, int recv, int arg, int v, Buf *b);
int parse_named_format(const char *fmt, Buf *rew, const char **names, int *name_len, int maxn);
int re_lit_enc_opts(Compiler *c, int recv, int rre);
void emit_re_opts_flags(Compiler *c, int argc, const int *argv, Buf *out);
void emit_regexp_new(Compiler *c, int argc, const int *argv, Buf *b);
int emit_try_convert_boxed(Compiler *c, const char *cname, int arg, Buf *b);
int re_src_has_backref(const char *s);
int emit_time_civil_ctor(Compiler *c, int id, int is_utc, int is_new, Buf *b);
void emit_time_in_zone(Compiler *c, int ts, int zone, Buf *b);
void emit_utime_arg_ns(Compiler *c, int node, const char *sv, const char *nv, Buf *b);
int emit_clock_id(Compiler *c, int node, Buf *b);
void emit_math_arg(Compiler *c, int node, Buf *out);
int json_to_json_is_builtin(Compiler *c, int recv);
int node_is_stringio(Compiler *c, int node);
void emit_exc_exception(Compiler *c, int recv, int arg, Buf *b);
int emit_name_error_new_kw(Compiler *c, const char *cn, int argc, const int *argv, int recv_v, Buf *b);
int emit_raise_nomethod_new(Compiler *c, int call, Buf *b);
int disp_nomethod_out_of_line(Compiler *c, int call);   /* (codegen.c's value scan asks it too) */
const char *exc_gated_acc_fn(const char *name);
int hoist_exc_recv(Compiler *c, int recv);
int ctor_init_takes_block(Compiler *c, int initm);
void emit_exc_reopen_construct(Compiler *c, int ci, int initm, int args, int msg_node, Buf *b);
void emit_raise_class_value(Compiler *c, int kn, int mn, Buf *b);
int exc_reopen_initialize(Compiler *c, int ci);
void emit_exc_new_no_init(Compiler *c, int id, int ci, int argc, const int *argv, Buf *b);
int raise_plain_arg(const NodeTable *nt, int node);
void emit_kconv_call(Compiler *c, int id, const int *av, int ac, int raise, Buf *b);
void emit_pre_format_args(Compiler *c, const int *av, int ac, int ta);
void emit_fiber_storage_key(Compiler *c, int key, Buf *b);
int poly_binop_recv_temp(Compiler *c, int recv, int arg, Buf *b, int *stmt_expr);
int attr_writer_named(Compiler *c, int cls, const char *name);
int boxed_desc_control_arity(const char *name, int argc);
int boxed_printf_list_ok(Compiler *c, const int *argv, int argc);
int boxed_write_takes_list(Compiler *c, int id, const int *argv, int argc);
int class_method_named(Compiler *c, const char *name);
const char *io_line_reader(const char *name, int has_limit);
void emit_io_line_read(Compiler *c, const char *name, const char *r, const int *argv, int argc, Buf *b);
void emit_io_wait(Compiler *c, const char *name, int argc, const int *argv, const char *r, Buf *b);
void emit_io_socket_send_call(Compiler *c, const char *r, int argc, const int *argv, Buf *b);
void emit_push_arg_list(Compiler *c, const int *argv, int argc, int tpa, Buf *b);
void emit_stat_handle_nomethod(int th, int tv, const char *name, Buf *b);
void emit_stat_handle_only(int th, const char *name, Buf *b);
void emit_unbox_or_keep(Compiler *c, TyKind want, int t, Buf *b);
void emit_poly_kw_all(Compiler *c, int kwh, int th, int any, int ran, Buf *b);
int file_block_param_poly(Compiler *c, int id, const char *pname);
void gets_sep_arg_texts(Compiler *c, const int *argv, int argc, int strict, Buf *sep, Buf *lim, Buf *chomp);
int io_line_args_spread(const NodeTable *nt, const int *argv, int argc);
char adapter_arg_kind(TyKind arr, int is_push, int is_set, int pos);
int bam_binop_wrapper(const Scope *tm);
int bm_call_needs_layout(const NodeTable *nt, int id, const int *argv, int argc);
const char *bm_self_ctype(const Compiler *c, Scope *tm, int shift);
void emit_bm_self_arg(const char *sct, const char *bm, Buf *b);
int call_arg_sig(Compiler *c, const int *argv, int argc, char *out, size_t cap);
void emit_adapter_arg_boxed(const char *v, char kind, Buf *out);
void emit_adapter_arg_static(Compiler *c, int node, char kind, Buf *out);
int emit_bm_flat_args(Compiler *c, const int *argv, int argc, Buf *b);
void emit_bm_spread_call(Compiler *c, int id, int recv, const int *argv, int argc, Buf *b);
void emit_bound_method_call(Compiler *c, int id, int recv, int target, Buf *b);
void emit_method_call_block(Compiler *c, int id, Scope *tm, int lead_comma, Buf *b);
void emit_named_root(Compiler *c, TyKind t, const char *name, int idx, Buf *b);
void emit_wrong_count(Compiler *c, int id, const char *exp, int eval_recv, int given, Buf *b);
const char *forwarded_real_proc(int blk0, int blk);
extern int g_sn_proc_node;
int is_proc_create(Compiler *c, int id);
void abi_sig_token(TyKind t, char *out);
const char *bam_builtin_sym(Compiler *c, int mi, const char *dflt);
int bind_owner_mismatch(Compiler *c, int target, TyKind ot);
int builtin_method_arity(const char *cls, const char *m, int *out);
const char *builtin_method_params(const char *owner, const char *m, int *in_c);
int builtin_method_known(const char *cls, const char *m);
int core_method_vis(const char *cls, const char *m, int inherit, const char **owner);   /* codegen_call_class.c */
int refl_builtin_vis(Compiler *c, int ci, const char *m, int inherit, const char **owner);   /* codegen_call_class.c */
const char *core_class_ancestors(const char *cls);   /* codegen_call_class.c */
const char *core_ancestor_step(const char *p, size_t *len);   /* codegen_call_class.c */
void core_own_methods_each(const char *cls, size_t cl, void (*f)(void *, const char *, int), void *ud);
int emit_value_methods_reflection(Compiler *c, int id, Buf *b);   /* codegen_call.c */
void emit_value_methods_helpers(Compiler *c, const char *const *texts, int ntexts, Buf *protos,
                                Buf *defs);   /* codegen_call.c */
int builtin_comparable_owns(const char *cls, const char *m);
int builtin_comparable_arity(const char *m);
int builtin_numeric_owns(const char *cls, const char *m);
int builtin_numeric_arity(const char *m, int *out);
void emit_bind_call(Compiler *c, int id, int target, const int *argv, int argc, Buf *b);
void emit_bind_call_boxed(Compiler *c, int id, int target, int kn, const char *sym, const int *argv, int argc, Buf *b);
void emit_bm_abi_args(Buf *b, const char *rb, int abi, const char *sig, int fixed, int rest, int ret, int poly, int pfixed, const char *thunk, int tmin, int tmax);
int emit_method_obj_on_constant(Compiler *c, int id, int recv, const char *sym, Buf *b);
int method_obj_of_native_func(Compiler *c, int recv, const char *sym);
const char *emit_method_thunk(Compiler *c, int mi, int recv_bound, int *out_min, int *out_max);
int emit_method_tramp_fn(Compiler *c, Scope *tm, int shift, const char *fname, int boxed_src, int self_cls, int *out_min, int *out_max);
int method_legacy_int_abi(Compiler *c, int mi, int recv_bound, char *out_sig, size_t sigcap, int *out_fixed, int *out_rest, int *out_ret);
int method_node_recv_class(Compiler *c, int recv, int dflt);
int method_poly_abi(Compiler *c, int mi, int recv_bound, int *out_fixed, int *out_ret);
int method_scope_arity(Compiler *c, int target, int *out);
void emit_bm_legacy_ok(Buf *b, int tmp, int argc, const char *arg_sig);
void emit_int_recv_named(Compiler *c, int recv, const char *name, Buf *b);
void emit_upto_recv(Compiler *c, int recv, int lim, Buf *b);
int emit_implicit_self_member(Compiler *c, int id, Buf *b);
int emit_reopen_own_call(Compiler *c, int id, int dispatch_cid, Buf *b);
int emit_reopen_recv_args(Compiler *c, int id, int mi, int recv, int boxed, const char *box_fn, size_t at, Buf *b);
int emit_reopen_recv_in_order(Compiler *c, int id, int mi, int recv, int boxed, const char *box_fn, size_t at, Buf *b);
void emit_reopen_self_arg(Compiler *c, int id, int mi, Buf *b);
int str_fresh_value(Compiler *c, int n);
CallPlan implicit_self_plan(Compiler *c, int id, int dispatch_cid);

int emit_send_blind(Compiler *c, int id, Buf *b);

int emit_call_const_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);

#endif
