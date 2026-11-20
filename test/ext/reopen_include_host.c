/* A pure-C host over an --ext-class emission whose class includes, in a
   reopening, a module written inside it: the module's method is a row of
   the class's table under its own name (#12224). */
#include <stdio.h>
#include "r.h"

static void call(sp_RbVal recv, const char *name) {
  sp_RbVal out;
  if (sp_ext_send(recv, name, 0, NULL, &out)) printf("%s -> %s\n", name, sp_poly_inspect(out));
  else printf("no method %s\n", name);
}

int main(void) {
  Init_ext_reopen_include();
  sp_RbVal p = sp_ExtReopenMain_s_make();
  SP_GC_ROOT_RBVAL(p);
  call(p, "b?");
  call(p, "k");
  const sp_ext_method *m = sp_ext_methods((int)p.cls_id);
  for (int i = 0; m && m->name; m++, i++) printf("%s%s/%d", i ? " " : "", m->name, m->argc);
  puts("");
  return 0;
}
