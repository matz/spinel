/*
 * Stands in for libmruby.a in link-names-test: it defines mruby's names
 * that spinel's regexp engine also uses. A program links this object next
 * to the runtime. If the link picks one of these names for the engine, the
 * program aborts and names the function.
 */
#include <stdio.h>
#include <stdlib.h>

#define FOREIGN(name) \
  void *name(void) { fputs("foreign " #name " called\n", stderr); abort(); }

FOREIGN(mrb_malloc)
FOREIGN(mrb_calloc)
FOREIGN(mrb_realloc)
FOREIGN(mrb_free)
FOREIGN(mrb_str_new)
FOREIGN(mrb_str_cat)
FOREIGN(mrb_format)
FOREIGN(mrb_exc_raise)
FOREIGN(mrb_utf8len)
FOREIGN(mrb_utf8_decode)
FOREIGN(mrb_re_compile)
FOREIGN(mrb_re_exec)
FOREIGN(mrb_uni_case_map)
FOREIGN(mrb_uni_case_fold)
