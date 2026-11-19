#ifndef SP_CEXT_H
#define SP_CEXT_H
#include <ruby.h>
#include "sp_gc.h"
#include "sp_alloc.h"
#include "sp_proc.h"
sp_RbVal sp_cext_object_class(sp_RbVal);
sp_RbVal sp_cext_hash_new(void);
sp_RbVal sp_cext_plain_object_new(void);
void sp_cext_hash_set(sp_RbVal, sp_RbVal, sp_RbVal);
sp_int sp_cext_object_to_int(sp_RbVal);
int sp_cext_type(sp_RbVal, int (*)(int));
sp_RbVal sp_cext_hash_get(sp_RbVal, sp_RbVal);
int sp_cext_hidden_defined(sp_RbVal, sp_sym);
sp_RbVal sp_cext_hidden_get(sp_RbVal, sp_sym);
void sp_cext_hidden_set(sp_RbVal, sp_sym, sp_RbVal);
sp_RbVal sp_cext_freeze(sp_RbVal);
sp_RbVal sp_cext_ary_delete(sp_RbVal, sp_RbVal);
double sp_cext_num2dbl(sp_RbVal);
int sp_cext_integer_pack(sp_RbVal, void *, size_t, size_t);
sp_RbVal sp_cext_sym2str(sp_sym);
struct sp_String_s *sp_cext_sym_name_handle(sp_sym);
sp_RbVal sp_cext_str_plus(sp_RbVal, sp_RbVal);
sp_RbVal sp_cext_widen_array(sp_RbVal);

/* Internal bridge. The compiler/recorder will use this only for opted-in
 * extensions; an ordinary generated translation unit never includes it. */
typedef struct {
    void (*init)(void);
    VALUE (**functions)(ANYARGS);
    size_t count;
    int initialized;
    VALUE *constants;
    size_t constant_count;
    const char *name;   /* the extension's name: its Class.new classes are named by it */
} sp_cext_extension;
void sp_cext_init(sp_cext_extension *);
void sp_cext_lock(void);
void sp_cext_unlock(void);
unsigned sp_cext_lock_depth(void);
unsigned sp_cext_lock_suspend(void);
void sp_cext_lock_resume(unsigned);
int sp_cext_register_address(VALUE *);
int sp_cext_unwinding(void);
void sp_cext_resume_unwind(void);
extern SP_TLS void *sp_cext_current_block;
extern SP_TLS VALUE sp_cext_errinfo;
void sp_cext_set_errinfo(VALUE);
/* Operation kinds: send, ivar read/write, constant read, literal eval. */
extern sp_RbVal (*sp_cext_dispatch_fn)(int, const char *, sp_RbVal, int, const sp_RbVal *, sp_Proc *);
sp_RbVal sp_cext_invoke(sp_cext_extension *, int, int, sp_RbVal, sp_PolyArray *, sp_Proc *);
void *sp_cext_allocate(sp_cext_extension *, int, int);
sp_RbVal sp_cext_duplicate(sp_RbVal, int);
extern void *(*sp_cext_class_alloc_fn)(int);
extern sp_RbVal (*sp_cext_class_lookup_fn)(const char *);
extern void *(*sp_cext_object_alloc_fn)(int);
extern int (*sp_cext_data_class_fn)(int);
/* catch and throw over the generated unit's catch stack (rb_catch, rb_throw,
 * rb_iter_break) */
extern sp_RbVal (*sp_cext_catch_fn)(sp_RbVal, sp_RbVal (*)(void *), void *);
extern void (*sp_cext_throw_fn)(sp_RbVal, sp_RbVal);

VALUE sp_cext_value(sp_RbVal value);
sp_RbVal sp_cext_rbval(VALUE value);
typedef struct { size_t handles, depth; } sp_cext_arena_mark;
typedef struct { void *entries; size_t length, capacity, depth; void *block; VALUE errinfo; } sp_cext_arena_context;
void sp_cext_arena_context_save(sp_cext_arena_context *);
void sp_cext_arena_context_load(const sp_cext_arena_context *);
void sp_cext_arena_context_dispose(sp_cext_arena_context *);
sp_cext_arena_mark sp_cext_arena_enter(void);
sp_cext_arena_mark sp_cext_arena_snapshot(void);
void sp_cext_arena_restore(sp_cext_arena_mark mark);
size_t sp_cext_handle_count(void);
void sp_cext_mark_roots(void);
void sp_cext_sweep_handles(int full);

/* Layout prefix for the compiler's future data classes. The compiler adds
 * Ruby ivars after this prefix and calls these helpers from its scan/finalize
 * functions. The standalone wrapper tests the same lifecycle today. */
typedef struct {
    sp_int cls_id;
    void *cext_data;
    const rb_data_type_t *cext_type;
} sp_cext_data;
void sp_cext_data_scan(void *object);
void sp_cext_data_finalize(void *object);
sp_cext_data *sp_cext_data_new(int cls_id, void *data, const rb_data_type_t *type);
int sp_cext_data_is_kind_of(const sp_cext_data *object, const rb_data_type_t *type);

void sp_cext_exceptions_init(void);
/* Installed by the future compiler bridge for exception classes carrying
 * Ruby ivars. It must allocate the complete generated struct and scan it. */
extern sp_RbVal (*sp_cext_exception_new_fn)(const char *, const char *);


#endif
