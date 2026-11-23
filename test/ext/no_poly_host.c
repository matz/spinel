/* A pure-C host over an ext-init library that renders no Symbol and no
   class name (SP_TU_NO_POLY_RENDER): its init runs the top level. */
#include <stdio.h>
#include "np.h"

int main(void) {
  Init_ext_no_poly();
  fflush(stdout);
  printf("init returned\n");
  return 0;
}
