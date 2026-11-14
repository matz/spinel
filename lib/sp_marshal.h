#ifndef SP_MARSHAL_H
#define SP_MARSHAL_H
/* Marshal.dump / Marshal.load -- public interface to lib/sp_marshal.c.

   The serializer is a standalone translation unit. The read side (length / array
   element / hash pair / kind / symbol name) reuses the generic sp_json_* hooks
   in sp_gc.h. The construction side (intern a symbol, build a result array/hash,
   box Complex/Rational, dispatch a user object, raise) needs types and state
   that live only in the generated TU, so the generated TU fills the sp_marshal_v
   vtable below at startup (see sp_tu_init in codegen).

   Covers nil/true/false/Integer/Float/String/Symbol + Array + Hash + Bignum +
   Complex + Rational + plain user objects in the CRuby 4.8 wire format, with the
   object-link table so cyclic / shared references round-trip. */
#include "sp_gc.h"   /* sp_RbVal, sp_sym, sp_int, sp_float */

/* Dump buffer + wire-emit primitives. Public so the codegen-generated
   per-class object dumper (registered as sp_marshal_v.obj_dump) can write the
   `o` form through them. */
typedef struct sp_mar_buf_s {
  char *p; size_t len, cap;
  /* object-link table: pointer identity -> link id, plus the next id. */
  void **lptr; int *lid; int nl, cl; int link_next;
  /* symbol-link table: every symbol is written in full once; a repeat emits
     `;<index>` (indices count symbols only, separate from object links). */
  char **wsyms; int nws, cws;
} sp_mar_buf;
void sp_mar_b(sp_mar_buf *b, unsigned char c);
void sp_mar_sym(sp_mar_buf *b, const char *name);
void sp_mar_long(sp_mar_buf *b, long n);
void sp_mar_w(sp_mar_buf *b, sp_RbVal v);
/* An Array's or Hash's own record (`[` or `{`), its link already taken: the
   body of a builtin subclass instance's `C` record (#7449). */
void sp_mar_w_body(sp_mar_buf *b, sp_RbVal v);
/* An exception's `o` record up to its own ivars: the class, `nivar` + 2, its
   message and its backtrace (an sp_Exception or a subclass laid out like one). */
void sp_mar_w_exc_head(sp_mar_buf *b, const char *cls, void *p, long nivar);

/* Runtime vtable filled by the generated TU (sp_tu_init). The read side uses
   the sp_json_* hooks in sp_gc.h instead. */
typedef struct {
  sp_sym  (*sym_intern)(const char *);
  sp_RbVal (*arr_new)(void);
  void     (*arr_push)(sp_RbVal arr, sp_RbVal v);
  sp_RbVal (*hash_new)(void);
  void     (*hash_set)(sp_RbVal h, sp_RbVal k, sp_RbVal v);
  sp_RbVal (*box_complex)(sp_float re, sp_float im, int fl);   /* fl: SP_CPLX_RE_F / SP_CPLX_IM_F */
  sp_RbVal (*box_rational)(sp_int num, sp_int den);
  int      (*obj_dump)(sp_mar_buf *b, int cls_id, void *p);   /* generated; writes `o` */
  /* into nil: a new object of the class; into an object: its ivars set
     from iv (a boxed PolyArray of name, value pairs) */
  sp_RbVal (*obj_load)(const char *clsname, sp_RbVal into, sp_RbVal iv, int *ok);
  void     (*raise)(const char *cls, const char *msg);
  sp_RbVal (*hash_default)(sp_RbVal h, int *has_proc);    /* nil for none */
  void     (*hash_set_default)(sp_RbVal h, sp_RbVal d);   /* on a hash_new one */
  void     (*any_push)(sp_RbVal arr, sp_RbVal v);   /* push into a boxed Array of any kind */
} sp_marshal_vt;
extern sp_marshal_vt sp_marshal_v;

const char *sp_marshal_dump(sp_RbVal v);
sp_RbVal sp_marshal_load(const char *s, sp_int len);
void sp_marshal_mark_active(void);   /* mark the in-flight load tables (lib/sp_marshal.c) */
#endif /* SP_MARSHAL_H */
