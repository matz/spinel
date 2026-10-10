/* codegen_poly.h -- the poly dispatch's helpers, shared by the files that
   build and emit it: codegen_call.c (the dispatch's arms), codegen_poly_plan.c
   (its plan) and call_plan.c (the resolver). Include after
   codegen_internal.h, whose types they use. */
#ifndef SPINEL_CODEGEN_POLY_H
#define SPINEL_CODEGEN_POLY_H

#include "codegen_internal.h"

void emit_poly_user_box(Compiler *c, int id, Scope *m, const char *call, Buf *b);
int  class_is_prim_reopen(Compiler *c, int k);
int  exc_arm_definer(Compiler *c, int k, const char *name);
int  poly_arm_refuses_none(Compiler *c, int mi, char *exp, size_t n);
void emit_poly_arity_raise(Buf *b, const char *msg);
void emit_unbox_poly_ret(Compiler *c, TyKind slot, const char *expr, Buf *b);
void emit_cmethod_block_arg(Compiler *c, int id, Scope *cm, int blk_tmp, Buf *b);
struct PolyPlan_s;
void emit_poly_user_arms0(Compiler *c, int id, const char *name, int argc, TyKind ret, int tv, int tr,
                          int blk_tmp0, const struct PolyPlan_s *p, Buf *b);
/* The keywords of a call on a poly receiver, as its arms read them. */
typedef struct {
  int kwh, kwn, kwall;
  const int *kwels, *kwtmp;
  const TyKind *kwty;
  int kwall_any;   /* kwall is a PolyPolyHash: a `**` may carry a key that is no Symbol */
} PolyKw;
/* The call's arguments as a poly-dispatch arm reads them: each positional
   evaluated once into a temp ahead of the switch (a splat's temp the array
   it spreads, argv tells which), and the keywords split off (PolyKw). */
typedef struct {
  const int *argv;
  int pos_argc;
  const int *atmp;
  const TyKind *atmp_ty;
  const PolyKw *kw;
  /* per positional, the temp holding the handle its String variable had
     when the argument ran, or 0 (emit_poly_shared_arg) */
  const int *htmp;
} PolyArgs;
void poly_arm_layout(Compiler *c, Scope *ms, const PolyArgs *A, ArgLayout *L);
/* can user arm ks take the argument temps' types (call_plan.c)? */
int  cplan_arm_args_fit(Compiler *c, Scope *ks, const ArgLayout *L, int pos_argc, const TyKind *atmp_ty,
                        int kwall_any);
/* a poly dispatch with arguments, as its user-class arms read it */
typedef struct {
  int argc, pos_argc, kwh, kwall, kwall_any, kw_pos, has_splat_arg, splat_a, stk;
  int is_setter_val, blk_tmp2, tv, tr;
  TyKind ret;
  const int *argv, *atmp, *htmp;
  const TyKind *atmp_ty;
  const PolyKw *kw;
  const struct PolyPlan_s *plan;   /* the dispatch's plan (cplan_poly_arms) */
} PolyUserArgs;
void emit_poly_user_arms_n(Compiler *c, int id, const char *name, const PolyUserArgs *U, Buf *b);
unsigned poly_form_check(int id, const char *name, const char *site, int served, unsigned form, unsigned oform);
void emit_poly_arm_args(Compiler *c, Scope *m, Scope *ms, const ArgLayout *L, const PolyArgs *A, const char *selfd, const char *lead, Buf *pre, Buf *cb);
int poly_native_arm_call(Compiler *c, int k, const char *name, int n, const int *argv, const int *atmp, const TyKind *atmp_ty, int tv, Buf *cb, TyKind *mret);
int emit_poly_native_arm_stmt(Compiler *c, const char *call, TyKind mret, TyKind ret, int tr, int is_setter_val, Buf *b);
int  poly_arm_count(Compiler *c, Scope *m, int kwh, int pos_argc, int splat, char *exp, size_t n);
int  poly_kw_splat_ok(Compiler *c, int el);
int  poly_kw_any_key(Compiler *c, int kwh);
int  obj_class_unrelated(Compiler *c, int a, int b);
int  poly_native_arm_fits(Compiler *c, int k, const char *name, int n, const int *argv,
                          const TyKind *atmp_ty, TyKind *mret);
int  emit_poly_user_arm_n(Compiler *c, int id, int k, const char *call, TyKind mret, Scope *ms, TyKind ret,
                          int tr, int so, int is_setter_val, Buf *b);
void emit_poly_index_cases(TyKind ret, int tr, int tv, const char *idxref, int so, Buf *b);
int  poly_pred_kind(const char *name, int argc);
int  poly_exc_cand(Compiler *c, const char *name);
int  hoist_block_proc(Compiler *c, int cblk);
int  hoist_dispatch_blk_proc(Compiler *c, int id, int cblk);
void emit_trailing_blk_arg(Compiler *c, const Scope *m, int id, int blk_tmp, Buf *b);
/* what a zero-argument poly dispatch answers beside its user arms, and its
   user candidates (poly_specials0) */
typedef struct {
  int lengthlike, empty, class_named, class_reflect, cls_members, pred, ostruct, io_rewind,
      poly_to_a, poly_to_h;
  int ncand, ncall_arm;
} PolySpecials0;
void poly_specials0(Compiler *c, int id, const char *name, PolySpecials0 *s);
/* what a poly dispatch with arguments answers beside its user arms, and its
   keyword split and user candidates (poly_specials_n) */
typedef struct {
  int index, fetch, pdelete, pdig, pvalues_at, pfirstn, include, intersect, arr_index, push, unshift, strdel, strpart, strsetop_n, pstore, straset, strsplit, pred, strencode, strftime, pmerge, pjoin, ppack, cover, gcdlcm, ctryconv;
  int has_splat_arg, kwh, pos_argc, kw_ds, kw_strkey, kw_pos, ncand;
} PolySpecialsN;
void poly_specials_n(Compiler *c, int id, const char *name, int argc, const int *argv, PolySpecialsN *s);
void poly_specials_n_splat(Compiler *c, const int *argv, PolySpecialsN *s, int *splat_a, int *splat_last);
/* a poly dispatch's evaluated arguments, as its arms read them: the
   positional temps and their types, the keyword temps (or the whole hash
   kwall), the result temp and its type, the receiver temp */
typedef struct {
  int argc, pos_argc;
  const int *argv, *atmp;
  const TyKind *atmp_ty;
  int kwall, kwn;
  const int *kwels, *kwtmp;
  const TyKind *kwty;
  TyKind ret;
  int tv, tr;
  const char *idxref;   /* the index as a raw sp_int, for `[]` */
} PolyTemps;
void emit_poly_prearms_n(Compiler *c, const char *name, const PolySpecialsN *ps, const PolyTemps *T, Buf *b);
void emit_poly_str_aset_prearm(Compiler *c, int recv, const PolySpecialsN *ps, const PolyTemps *T, Buf *b);
void emit_poly_str_aset_writeback(Compiler *c, int recv, const PolySpecialsN *ps, const PolyTemps *T, Buf *b);
void emit_poly_str_aset_recv(Compiler *c, int recv, const PolySpecialsN *ps, const int *argv, Buf *b);
int  emit_poly_prearms_n_blk(Compiler *c, int id, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                             int *atmp, TyKind *atmp_ty, const PolyKw *kw, const int *htmp, int is_setter_val,
                             int splat_a, int splat_last, int stk, Buf *b);
void emit_kwh_sym_hash(Compiler *c, const PolyKw *kw, Scope *skip_kw, Buf *out);
void emit_poly_fetch_absent(Compiler *c, int argc, const int *atmp, TyKind dty,
                            int key_node, TyKind ret, TyKind trt, Buf *b);
void emit_poly_cases_n(Compiler *c, int id, const char *name, const PolySpecialsN *ps, const PolyTemps *T,
                       int splat_a, int is_aref, int is_fetch, Buf *b);
void emit_poly_defaults_n(Compiler *c, int id, int recv, const char *name, const PolySpecialsN *ps,
                          const PolyTemps *T, const PolyKw *kw, int splat_a, int is_aref, int is_aref2,
                          int is_fetch, int blk_tmp2, int is_setter_val, Buf *b);
int  poly_num_arm(const char *name, int argc);
void emit_poly_last_default(Compiler *c, int id, int recv, const char *name, int argc, const int *argv,
                            const int *atmp, const TyKind *atmp_ty, TyKind ret, int tv, int tr, int done,
                            Buf *b);
int emit_poly_default_blk_arm(Compiler *c, int id, const char *name, int argc, const int *argv, const int *atmp, const TyKind *atmp_ty, int tv, int tr, int blk_tmp2, Buf *b);
int emit_poly_aset_default(Compiler *c, const char *name, int argc, const int *atmp, const TyKind *atmp_ty, TyKind ret, int tv, int tr, Buf *b);
void emit_kwh_pos_hash(Compiler *c, const PolyKw *kw, int boxed, Buf *out);
int poly_call_blk_proc(Compiler *c, int id, int have);
int  emit_poly_callable_spread_prearm(Compiler *c, const char *name, int sa, const int *atmp,
                                      const TyKind *atmp_ty, int st, int tv, int tr, TyKind ret, Buf *b);
void emit_poly_prearms0(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                        int tv, int tr, Buf *b);
int  emit_poly_prearms0_blk(Compiler *c, int id, const char *name, const PolySpecials0 *ps, TyKind ret,
                            int tv, int tr, Buf *b);
int  emit_poly_callable_prearm(Compiler *c, const char *name, int argc,
                               const int *atmp, const TyKind *atmp_ty, const char *guard,
                               int tv, int tr, TyKind ret, int kwpos, Buf *b);
int  emit_poly_cls_value_prearm(Compiler *c, int id, const char *name, int argc,
                                const int *atmp, const TyKind *atmp_ty, const int *htmp,
                                const PolyKw *kw, int tv, int tr, TyKind ret, int blk_tmp, Buf *b);
void emit_builtin_len_cases(Buf *b, int tr, int tv, const char *open, const char *close);
int emit_poly_builtin_default(Compiler *c, int id, int recv, const char *name, int argc, const int *argv, const int *atmp, const TyKind *atmp_ty, TyKind ret, int tv, int tr, int label, Buf *b);
int emit_poly_pred_value(Compiler *c, int id, const char *tvref, const char *argref, Buf *b);
void emit_poly_enum_for(Compiler *c, const char *val, Buf *b);
void emit_poly_cases0(Compiler *c, int id, int recv, const char *name, const PolySpecials0 *ps,
                      TyKind ret, int tv, int tr, Buf *b);
int  emit_poly_defaults0(Compiler *c, int id, int recv, const char *name, const PolySpecials0 *ps,
                         TyKind ret, int tv, int tr, int obj_default_done, Buf *b);
int  poly_cls_value_cands(Compiler *c, int id, const char *name, int argc, const PolyArgs *A, int diag,
                          int *ccls8, int *cmi8, char (*cexp8)[600], int *wants_blk);
int  poly_key_cls0(Compiler *c, const char *name, int argc, int kwh, int pos_argc, int splat_a);
int  poly_key_prim(Compiler *c, const char *name, int argc, int kwh, int pos_argc, int splat_a);
int  emit_poly_obj_default0(Compiler *c, int id, const char *name, int argc, TyKind ret, int tv, int tr,
                            int *blk_tmp0, Buf *b);

#endif
