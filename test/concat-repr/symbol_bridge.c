/* Exercise the exported Symbol Array representation through the ordinary
   native_func :any ABI. Symbol literals currently build PolyArrays. */
#include "sp_array.h"

sp_RbVal concat_symbol_source(sp_RbVal input) {
  SP_GC_ROOT_RBVAL(input);
  sp_PolyArray *source = (sp_PolyArray *)input.v.p;
  sp_IntArray *symbols = sp_IntArray_new();
  SP_GC_ROOT(symbols);
  for (sp_int i = 0; i < source->len; i++)
    sp_IntArray_push(symbols, source->data[i].v.i);
  return sp_box_sym_array(symbols);
}
