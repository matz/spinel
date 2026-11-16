/* A host over entries whose signatures name `Array[Point]`: it takes the
   sp_PtrArray one entry answers and hands it to the other, through the
   header alone. */
#include <stdio.h>
#include "g.h"

int main(void) {
  Init_ext_geo();
  sp_PtrArray *p = sp_ExtGeo_s_pts(4);
  SP_GC_ROOT(p);
  printf("%.1f\n", sp_ExtGeo_s_length(p));
  return 0;
}
