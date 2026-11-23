/* pre_const.h -- compile-time target platform, -D defines, and pure
 * constant-branch evaluation shared by the parser (before require
 * resolution) and analysis (desugar_engine_branches). */
#ifndef SPINEL_PRE_CONST_H
#define SPINEL_PRE_CONST_H

#include <stddef.h>

typedef enum {
  SP_PRE_UNDEF = 0,
  SP_PRE_BOOL,
  SP_PRE_NIL,
  SP_PRE_INT,
  SP_PRE_STR
} SpPreKind;

typedef struct {
  SpPreKind kind;
  int b;
  long long i;
  const char *s;
  size_t slen;
} SpPreVal;

/* Target platform string (matching lib/spinel_rt.h's SP_RUBY_ARCH "-" SP_RUBY_OS). */
void sp_pre_set_target_platform(const char *platform);
const char *sp_pre_target_platform(void);
int sp_pre_target_platform_known(void);

/* CLI -D NAME[=VALUE] compile-time constants. */
int sp_pre_define_cli(const char *spec, char *err, size_t err_cap);
int sp_pre_define_count(void);
const char *sp_pre_define_name(int idx);
const SpPreVal *sp_pre_define_val(int idx);
int sp_pre_define_index(const char *name, size_t len);
int sp_pre_is_builtin_ruby_const(const char *name, size_t len);

/* Look up a provably fixed compile-time constant (RUBY_ENGINE, RUBY_PLATFORM,
 * RUBY_VERSION, or a non-shadowed -D constant). Returns 1 if resolved. */
int sp_pre_lookup_const(const char *name, size_t len,
                        int allow_engine, int allow_platform, int allow_version,
                        const unsigned char *def_shadowed, SpPreVal *out);

/* Is `defined?(name)` / `Object.const_defined?(:name)` provably true on every
 * run of this target before any require executes? Conservative: returns 1 only
 * when provably defined, -1 otherwise (never returns 0 before require loading). */
int sp_pre_defined_const_fixed(const char *name, size_t len, int has_remove_const);

/* Is `op` one of the core String/Integer/Object predicate methods folded here? */
int sp_pre_is_folded_op(const char *op);

/* Is `name` a core class/module whose reopening can change a folded op? */
int sp_pre_is_core_op_owner(const char *name, size_t len);

/* Pure evaluation of compile-time values and predicates. */
int sp_pre_val_truthy(const SpPreVal *v);
int sp_pre_eval_unary(const SpPreVal *v, const char *op, SpPreVal *out);
int sp_pre_eval_binary(const SpPreVal *l, const char *op, const SpPreVal *r, SpPreVal *out);
int sp_pre_eval_str_affix(const SpPreVal *recv, int is_start,
                          const SpPreVal *args, int nargs, SpPreVal *out);

#endif
