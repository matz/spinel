/* A pure-C host that hands each entry back the object another entry
   answered, and the entry calls a method on the elements of its Array
   field. (The class types are declared here: the header uses them without
   declaring them.) */
#include <stdio.h>
typedef struct sp_Grid_s sp_Grid;
typedef struct sp_Box_s sp_Box;
#include "f.h"

int main(void) {
  Init_ext_field_elems();
  printf("%lld\n", (long long)sp_ExtFieldElems_s_sum_x(sp_ExtFieldElems_s_grid()));
  printf("%lld\n", (long long)sp_ExtFieldElems_s_count_done(sp_ExtFieldElems_s_box()));
  return 0;
}
