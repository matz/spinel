/* methods_sub_host.c -- the host of test/ext/methods_sub.rb, compiled with
   --ext-class ExtSub::Sub: the table of the subclass lists what its
   ancestor defines, a singleton method of the ancestor runs through the
   subclass, and the define_method in it, which only the host names, is
   registered. */
#include <stdio.h>
#include <string.h>
#include "m.h"

static void show(sp_RbVal v) { printf("%s\n", sp_poly_inspect(v)); }

static void call(sp_RbVal recv, const char *name, int argc, const sp_RbVal *argv) {
  sp_RbVal out;
  if (sp_ext_send(recv, name, argc, argv, &out)) show(out);
  else printf("no method %s\n", name);
}

static void list(sp_RbVal recv) {
  const sp_ext_method *m = sp_ext_methods((int)recv.cls_id);
  if (!m) { puts("no table"); return; }
  for (int i = 0; m->name; m++, i++)
    printf("%s%s%s/%d%s", i ? " " : "", m->singleton ? "." : "", m->name, m->argc,
           m->owner == (int)recv.cls_id ? "" : "^");
  puts("");
}

int main(void) {
  Init_ext_methods_sub();
  sp_RbVal s = sp_ExtSub_s_make();
  SP_GC_ROOT_RBVAL(s);
  list(s);
  call(s, "hi", 0, NULL);
  call(s, "base_name", 0, NULL);
  sp_RbVal ks = sp_box_class((sp_Class){ s.cls_id, 0 }), r;
  if (sp_ext_send(ks, "install", 0, NULL, &r)) { SP_GC_ROOT_RBVAL(r); call(ks, "call_extra", 1, &s); }
  return 0;
}
