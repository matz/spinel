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
int emit_call_class_method_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_reopen_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, TyKind rt);
int emit_call_regexp_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt, TyKind a0);
int emit_call_regexp_class_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_symbol_bool_string_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_file_dir_time_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_module_fn_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_builtin_cmethod_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_enum_random_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_exception_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_raise_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv);
int emit_call_kernel_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_kernel_flow_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_append_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_store_value_arms(Compiler *c, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_array_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_poly_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_io_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_handle_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_method_obj_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_poly_callable_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_poly_builtin_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);
int emit_call_openstruct_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv);
int emit_call_hash_value_arms(Compiler *c, int id, Buf *b, const NodeTable *nt, const char *name, int recv, int argc, const int *argv, TyKind rt);

/* ---- codegen_call.c's helpers the arms call ---- */
/* An ordered, de-duplicated list of method names for the reflection folds.
   A name is recorded once, at its first (most derived) sighting, and listed
   only if that sighting was visible, so a subclass that makes an inherited
   method private hides the ancestor's public one. */
typedef struct { char **v; unsigned char *show; int n, cap; } ReflNames;
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
extern int g_ie_poly_node;
int ie_body_has_break_next(Compiler *c, int node);
int ie_body_ivar_write(const NodeTable *nt, int node);
TyKind ie_splice_value_ty(Compiler *c, int node);
int basicobject_own_method(const char *n);
void emit_poly_cmp_ordered(Compiler *c, const char *fn, int recv, int arg, Buf *b);
void emit_handle_inspect(Compiler *c, int recv, TyKind rt, Buf *b);
int bigint_cmp_operand_ok(TyKind t);
Buf emit_cmp_self(Compiler *c, int recv, TyKind rt);
int emit_float_bigint_cmp(Compiler *c, int recv, int arg, const char *op, Buf *b);
int emit_int_operand_fail(Compiler *c, int id, int recv, int arg, int is_shift, Buf *b);
int exc_subclass_defines_cmp(Compiler *c);
int obj_cmp_by_identity(TyKind t);
int object_defines_cmp(Compiler *c);
void scalar_nil_test(TyKind t, const char *v, char *out, size_t n);
int any_class_defines(Compiler *c, const char *qm);
int any_class_responds(Compiler *c, const char *qm);
int class_implicit_responds(Compiler *c, int cid, const char *qm);
int class_struct_kind(Compiler *c, int cid);
int class_value_responds(Compiler *c, int tv, const char *qm, Buf *b);
extern const char *const comparable_names[];
int const_name_is_wrong(const char *s);
int emit_io_pick(Compiler *c, int tv, const char *name, int public_only, Buf *b);
void emit_io_pick_public(Compiler *c, const char *qm, const char *pick, Buf *b);
void emit_io_reopen_responds(Compiler *c, int tv, const char *qm, int include_all, Buf *b);
void emit_poly_dispatch_key(Compiler *c, int tv, int cls0_cand, int prim_cand, int exc_cand, Buf *b);
void emit_responds_name(Compiler *c, int k, const char *nm, int tv, Buf *b);
extern const char *const enumerable_names[];
extern int g_rto_nil_obj;
int method_hidden_from_reflection(Compiler *c, int cid, const char *m);
int name_is_synth_method(Compiler *c, const char *m);
int refl_member_of_struct_superclass(Compiler *c, int ci, const char *m);
int respond_to_static_answer(Compiler *c, int id, int recv, TyKind rt, const char *qm, int argc, const int *argv);
int respond_to_user_defined(Compiler *c, int id, int recv);
extern const char *const respond_universal[];
int rt_probe_answer(Compiler *c, int id, int *yes);
extern const char *const struct_names[];
void emit_call_body(Compiler *c, int id, Buf *b);
void emit_ffi_callback_arg(Compiler *c, int cbidx, int argnode, Buf *out);
void emit_ffi_num_arg(Compiler *c, int arg, TyKind at, const char *spec, int dbl, Buf *b);
int emit_or_take_back(Compiler *c, int id, Buf *b, int (*fn)(Compiler *, int, Buf *));
int emit_sg_accessor(Compiler *c, int ci, const char *cn, const char *name, int argc, const int *argv, Buf *b);
void ffi_check_buffer_bounds(Compiler *c, int id, int arg, const char *kind, int off, const char *dir);
extern int g_setter_value_inner;
int call_has_keyword_args(const NodeTable *nt, const int *argv, int argc);
int call_has_splat_arg(const NodeTable *nt, const int *argv, int argc);
int ctor_alloc_decl(Compiler *c, int cid, Buf *b);
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
void emit_ctor_arm_case(Compiler *c, int ci, int rt2, int self_t, const char *pre, const char *args, Buf *b);
void emit_ctor_arm_param(Compiler *c, Scope *is, int j, const ArgLayout *L, const int *atmp, Buf *pdpre, Buf *out);
void emit_ctor_block_slot(Compiler *c, int id, int initm, const char *lead, Buf *b);
int emit_ctor_splice_arm(Compiler *c, int id, int ci, int initm, int rt2, Buf *b);
void emit_exc_msg_arg(Compiler *c, int arg, Buf *b);
int emit_exc_sub_new_arm(Compiler *c, int ci, int argc, const int *atmp, int rt2, Buf *b);
int emit_syserr_family_new(Compiler *c, int id, const char *cn, int argc, const int *argv, Buf *b);
int emit_user_new_arm(Compiler *c, int id, int ci, int argc, const int *atmp, int hoisted, int rt2, Buf *b);
extern int g_ctor_blk_tmp;
int hoist_ctor_block(Compiler *c, int id, Buf *b);
int struct_nil_fills(const ClassInfo *k);
int cls_arm_takes_argc(Scope *s, int argc, int splat);
int collect_class_constants(Compiler *c, int ci, int inherit, const char **out, int max, int n, int depth);
void emit_class_id_is(Compiler *c, const char *cname, const char *expr, Buf *b);
int emit_reopen_arm_test(Compiler *c, int kmi, const char *name, const char *cls, const char *raise, Buf *b);
extern int g_cls_value_recv;
int hoist_dispatch_args(Compiler *c, int argsN, int **sv, int **vw);
int obj_member_shadows(Compiler *c, TyKind rt, const char *nm);
void refl_class_singleton_methods(Compiler *c, int ci, int all, ReflNames *r);
void refl_emit_sym_array(Compiler *c, int cid, ReflNames *r, Buf *b);
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
void emit_reopen_pf_call(Compiler *c, int id, int pf, int cblk, const char *recv_text, Buf *b);
int emit_str_format_untyped_array(Compiler *c, int recv, int a0n, int fck, Buf *b);
void emit_voided_operands(Compiler *c, int recv, int arg, int v, Buf *b);
int parse_named_format(const char *fmt, Buf *rew, const char **names, int *name_len, int maxn);
int re_lit_enc_opts(Compiler *c, int recv, int rre);
void emit_re_opts_flags(Compiler *c, int argc, const int *argv, Buf *out);
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
const char *exc_gated_acc_fn(const char *name);
int hoist_exc_recv(Compiler *c, int recv);
int ctor_init_takes_block(Compiler *c, int initm);
void emit_exc_reopen_construct(Compiler *c, int ci, int initm, int args, int msg_node, Buf *b);
void emit_raise_class_value(Compiler *c, int kn, int mn, Buf *b);
int exc_reopen_initialize(Compiler *c, int ci);
int raise_plain_arg(const NodeTable *nt, int node);
void emit_kconv_call(Compiler *c, int id, const int *av, int ac, int raise, Buf *b);
void emit_pre_format_args(Compiler *c, const int *av, int ac, int ta);
void emit_fiber_storage_key(Compiler *c, int key, Buf *b);
int poly_binop_recv_temp(Compiler *c, int recv, int arg, Buf *b, int *stmt_expr);
int attr_writer_named(Compiler *c, int cls, const char *name);
int boxed_desc_control_arity(const char *name, int argc);
int boxed_printf_list_ok(Compiler *c, const int *argv, int argc);
int boxed_stat_name(const char *name);
int boxed_stat_pred(const char *name);
extern const char *const boxed_stat_sfield[];
int boxed_write_takes_list(Compiler *c, int id, const int *argv, int argc);
int class_method_named(Compiler *c, const char *name);
void emit_gets_sep_args(Compiler *c, const int *argv, int argc, Buf *b);
void emit_io_wait(Compiler *c, const char *name, int argc, const int *argv, const char *r, Buf *b);
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
const char *bm_self_ctype(Scope *tm, int shift);
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
void emit_bind_call(Compiler *c, int id, int target, const int *argv, int argc, Buf *b);
void emit_bind_call_boxed(Compiler *c, int id, int target, int kn, const char *sym, const int *argv, int argc, Buf *b);
void emit_bm_abi_args(Buf *b, const char *rb, int abi, const char *sig, int fixed, int rest, int ret, int poly, int pfixed, const char *thunk, int tmin, int tmax);
int emit_method_obj_on_constant(Compiler *c, int id, int recv, const char *sym, Buf *b);
const char *emit_method_thunk(Compiler *c, int mi, int recv_bound, int *out_min, int *out_max);
int emit_method_tramp_fn(Compiler *c, Scope *tm, int shift, const char *fname, int boxed_src, int self_cls, int *out_min, int *out_max);
int method_legacy_int_abi(Compiler *c, int mi, int recv_bound, char *out_sig, size_t sigcap, int *out_fixed, int *out_rest, int *out_ret);
int method_node_recv_class(Compiler *c, int recv, int dflt);
int method_poly_abi(Compiler *c, int mi, int recv_bound, int *out_fixed, int *out_ret);
int method_scope_arity(Compiler *c, int target, int *out);
void emit_bm_legacy_ok(Buf *b, int tmp, int argc, const char *arg_sig);

#endif
