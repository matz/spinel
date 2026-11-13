/* A pure-C host over an --ext-class emission of methods with optional and
   rest parameters: the rows of the table, a call with each count of
   arguments, and the ArgumentError of a count no row takes. */
#include <stdio.h>
#include <string.h>
#include "m.h"

static void call(sp_RbVal recv, const char *name, int argc, const sp_RbVal *argv) {
  sp_RbVal out;
  if (sp_ext_send(recv, name, argc, argv, &out)) printf("%s\n", sp_poly_inspect(out));
  else printf("no method %s\n", name);
}

typedef struct { sp_RbVal recv; const char *name; int argc; const sp_RbVal *argv; } call_t;
static void call_body(void *p) {
  call_t *c = (call_t *)p;
  call(c->recv, c->name, c->argc, c->argv);
}
static void try_call(sp_RbVal recv, const char *name, int argc, const sp_RbVal *argv) {
  call_t ct = { recv, name, argc, argv };
  const char *cls = 0, *msg = 0;
  if (Init_ext_methods_opt_try(call_body, &ct, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
}

/* the rows of the table of recv's class: the name, the arity (negative for
   a row that takes that many arguments or more, as Method#arity counts),
   and a `.` on a singleton method */
static void list(sp_RbVal recv) {
  const sp_ext_method *m = sp_ext_methods((int)recv.cls_id);
  if (!m) { puts("no table"); return; }
  for (int i = 0; m->name; m++, i++) printf("%s%s%s/%d", i ? " " : "", m->singleton ? "." : "", m->name, m->argc);
  puts("");
}

int main(void) {
  Init_ext_methods_opt();
  sp_RbVal r = sp_ExtOpt_s_make();
  SP_GC_ROOT_RBVAL(r);
  sp_RbVal a[4] = { sp_box_int(1), sp_box_int(2), sp_box_int(3), sp_box_int(4) };
  sp_RbVal out;
  list(r);
  /* the default, the optional argument, and the rest after it */
  sp_ext_send(r, "where", 0, NULL, &out);
  sp_ext_send(r, "where", 1, a, &out);
  sp_ext_send(r, "where", 4, a, &out);
  /* a rest parameter alone */
  sp_ext_send(r, "order", 0, NULL, &out);
  sp_ext_send(r, "order", 3, a, &out);
  /* the method of the named class, not the one of its ancestor */
  sp_ext_send(r, "lim", 1, a, &out);
  call(r, "show", 0, NULL);
  try_call(r, "lim", 0, NULL);
  /* a singleton method, on the class value */
  sp_RbVal k = sp_box_class((sp_Class){ r.cls_id, 0 });
  call(k, "pick", 1, a);
  call(k, "pick", 2, a);
  try_call(k, "pick", 0, NULL);
  try_call(k, "pick", 3, a);
  /* not in the table: private, a keyword parameter, a block parameter */
  call(r, "hid", 0, NULL);
  call(r, "kw", 0, NULL);
  call(r, "blk", 0, NULL);
  return 0;
}
