/* A pure-C host over an ext-init library whose entry calls a Struct's `new`
   the load redefined. */
#include <stdio.h>
#include "l.h"

int main(void) {
  Init_ext_late_new();
  printf("%lld\n", (long long)sp_ExtLateNew_s_e(1LL));
  printf("%lld\n", (long long)sp_ExtLateNew_s_e(2LL));
  return 0;
}
