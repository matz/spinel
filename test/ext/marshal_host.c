/* A pure-C host over an ext-init library whose entry uses Marshal. */
#include <stdio.h>
#include "s.h"

int main(void) {
  Init_ext_marshal();
  fprintf(stderr, "%lld\n", (long long)sp_ExtMarshal_s_round_trip(41LL));
  return 0;
}
