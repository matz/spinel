/* A pure-C host over an ext-init library whose entries pass instances of
   the program's classes. It includes the emitted header and nothing else
   of the library: no typedef of its own for the classes' C types. */
#include <stdio.h>
#include "c.h"

int main(void) {
  Init_ext_classes();
  sp_TodoList *l = sp_ExtClasses_s_new_list();
  sp_ExtClasses_s_add(l);
  printf("%lld\n", (long long)sp_ExtClasses_s_add(l));
  sp_Pt p = sp_ExtClasses_s_pt(3LL);
  printf("%lld %lld\n", (long long)p.iv_x, (long long)p.iv_y);
  sp_Oops *e = sp_ExtClasses_s_oops();
  printf("%s %s\n", e->cls_name, e->msg);
  return 0;
}
