/* codegen_call_tms.c -- calls on a Process::Tms receiver, split out of
   emit_call_body behind the receiver-type table (#7033). Pure code
   movement, no logic change. */

#include "codegen_internal.h"

/* The receiver table has already selected Process::Tms; leave other names
   to the existing fallbacks at the call site. */
int emit_tms_call(Compiler *c, int id, int recv, const char *name, Buf *b) {
  int argc;
  call_args(c->nt, id, &argc);
  if (argc == 0 &&
      (sp_streq(name, "utime") || sp_streq(name, "stime") ||
       sp_streq(name, "cutime") || sp_streq(name, "cstime"))) {
    buf_puts(b, "("); emit_expr(c, recv, b); buf_printf(b, ").%s", name);
    return 1;
  }
  return 0;
}
