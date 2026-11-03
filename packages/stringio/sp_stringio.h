#ifndef SP_STRINGIO_H
#define SP_STRINGIO_H
/* StringIO -- a carried-C spin package (Path B typed object).

   The struct lives HERE, owned by the package; the generated TU sees only a
   forward declaration and holds pointers. cls_id is the first field (the
   object-header convention shared with user classes) and is stamped by the
   compiler-passed constructor argument, which lets a StringIO flow through
   poly values, arrays, and cls_id dispatch like any object. Instances are
   GC-allocated; the finalizer frees the malloc'd buffer. */
#include "spinel/runtime.h"   /* sp_int, sp_bool, sp_gc_alloc, sp_str_* */
#include "sp_string.h"        /* sp_String, the shared handle a StringIO may hold */

typedef struct sp_StringIO_s {
  sp_int cls_id;      /* object header: runtime class id, compiler-stamped */
  char *buf;           /* malloc'd, grown on demand; freed by the finalizer --
                          or, while `borrowed`, the constructor's GC string
                          shared read-only so #string keeps identity (the
                          first mutation copies it private) */
  int64_t len, cap, pos, lineno;
  int closed;          /* the sides it does not take: SIO_NO_READ (1) for an
                          output-only mode ("w", "a"), SIO_NO_WRITE (2) for
                          "r"; #close sets both, and #closed? asks for both */
  int borrowed;
  /* --share-strings: the String the StringIO is opened on, as its shared
     handle (sp_StringIO_new_h): the handle holds the bytes, and buf, len
     and cap mirror it, read again on each entry since a change to the
     String through another name moves them. NULL otherwise. */
  sp_String *str;
  int ro;              /* no write: 1 opened on a frozen String (even after
                          #string=), 2 handed one by #string= */
} sp_StringIO;

sp_StringIO *sp_StringIO_new(sp_int cls_id);
sp_StringIO *sp_StringIO_new_s(sp_int cls_id, const char *init);
sp_StringIO *sp_StringIO_new_sm(sp_int cls_id, const char *init, const char *mode);
const char *sp_StringIO_string(sp_StringIO *s);
sp_int sp_StringIO_pos(sp_StringIO *s);
sp_int sp_StringIO_set_pos(sp_StringIO *s, sp_int p);
sp_int sp_StringIO_size(sp_StringIO *s);
sp_int sp_StringIO_lineno(sp_StringIO *s);
sp_int sp_StringIO_write(sp_StringIO *s, const char *str);
void sp_StringIO_puts(sp_StringIO *s, const char *str);
void sp_StringIO_puts_empty(sp_StringIO *s);
void sp_StringIO_print(sp_StringIO *s, const char *str);
sp_int sp_StringIO_putc(sp_StringIO *s, sp_int ch);
const char *sp_StringIO_putc_s(sp_StringIO *s, const char *str);   /* answers its argument */
const char *sp_StringIO_read(sp_StringIO *s);
const char *sp_StringIO_read_n(sp_StringIO *s, sp_int n);
const char *sp_StringIO_gets(sp_StringIO *s);
const char *sp_StringIO_getc(sp_StringIO *s);
sp_RbVal sp_StringIO_getbyte(sp_StringIO *s);   /* nil at the end */
sp_int sp_StringIO_readbyte(sp_StringIO *s);
const char *sp_StringIO_readchar(sp_StringIO *s);
sp_int sp_StringIO_rewind(sp_StringIO *s);
sp_int sp_StringIO_seek(sp_StringIO *s, sp_int off);
sp_int sp_StringIO_tell(sp_StringIO *s);
sp_bool sp_StringIO_eof_p(sp_StringIO *s);
sp_int sp_StringIO_truncate(sp_StringIO *s, sp_int l);
void sp_StringIO_close(sp_StringIO *s);
sp_bool sp_StringIO_closed_p(sp_StringIO *s);
sp_StringIO *sp_StringIO_flush(sp_StringIO *s);
sp_bool sp_StringIO_sync(sp_StringIO *s);
sp_bool sp_StringIO_isatty(sp_StringIO *s);
sp_int sp_StringIO_zero(sp_StringIO *s);   /* fsync/fileno/pid: always 0 */
sp_StringIO *sp_StringIO_shl(sp_StringIO *s, const char *str);   /* << returns self */
const char *sp_StringIO_gets_sep(sp_StringIO *s, const char *sep);
sp_int sp_StringIO_seek2(sp_StringIO *s, sp_int off, sp_int whence);
const char *sp_StringIO_readline(sp_StringIO *s);
sp_RbVal sp_StringIO_readlines(sp_StringIO *s);
const char *sp_StringIO_gets_a1(sp_StringIO *s, sp_RbVal a);
const char *sp_StringIO_gets_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b);
const char *sp_StringIO_gets_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c);
const char *sp_StringIO_readline_a1(sp_StringIO *s, sp_RbVal a);
const char *sp_StringIO_readline_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b);
const char *sp_StringIO_readline_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c);
sp_RbVal sp_StringIO_readlines_a1(sp_StringIO *s, sp_RbVal a);
sp_RbVal sp_StringIO_readlines_a2(sp_StringIO *s, sp_RbVal a, sp_RbVal b);
sp_RbVal sp_StringIO_readlines_a3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c);
void sp_StringIO_print_v1(sp_StringIO *s, sp_RbVal a);
void sp_StringIO_print_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b);
void sp_StringIO_print_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2);
void sp_StringIO_puts_v1(sp_StringIO *s, sp_RbVal a);
void sp_StringIO_puts_v2(sp_StringIO *s, sp_RbVal a, sp_RbVal b);
void sp_StringIO_puts_v3(sp_StringIO *s, sp_RbVal a, sp_RbVal b, sp_RbVal c2);
void sp_StringIO_print_va(sp_StringIO *s, sp_int n, sp_RbVal *v);
void sp_StringIO_puts_va(sp_StringIO *s, sp_int n, sp_RbVal *v);
sp_int sp_StringIO_write_va(sp_StringIO *s, sp_int n, sp_RbVal *v);
const char *sp_StringIO_read_va(sp_StringIO *s, sp_int n, sp_RbVal *v);
void sp_StringIO_free(void *p);             /* GC finalizer: frees buf */
void sp_StringIO_scan_gc(void *p);          /* GC scan: what a subclass's scan marks first */
/* a subclass's `super(...)` in initialize: the constructors' in-place twins */
void sp_StringIO_init(sp_StringIO *s, sp_int cls_id);
void sp_StringIO_init_s(sp_StringIO *s, sp_int cls_id, const char *init);
void sp_StringIO_init_sm(sp_StringIO *s, sp_int cls_id, const char *init, const char *mode);
/* --share-strings: the forms over the String as its shared handle (the
   binding's `native_share`): opened on it, with a mode, or on a String of
   its own; and #string answering that handle itself */
sp_StringIO *sp_StringIO_new_h(sp_int cls_id, sp_String *str);
sp_StringIO *sp_StringIO_new_hm(sp_int cls_id, sp_String *str, const char *mode);
sp_StringIO *sp_StringIO_new_hn(sp_int cls_id);
void sp_StringIO_init_h(sp_StringIO *s, sp_int cls_id, sp_String *str);
void sp_StringIO_init_hm(sp_StringIO *s, sp_int cls_id, sp_String *str, const char *mode);
void sp_StringIO_init_hn(sp_StringIO *s, sp_int cls_id);
sp_String *sp_StringIO_string_h(sp_StringIO *s);
const char *sp_StringIO_set_string(sp_StringIO *s, const char *str);
sp_String *sp_StringIO_set_string_h(sp_StringIO *s, sp_String *str);
#endif /* SP_STRINGIO_H */
