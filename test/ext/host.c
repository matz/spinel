/* A pure-C host over the Layer-1 emission: init, entries through the header
   contract, a raise caught through the try helper. No Ruby involved. */
#include <stdio.h>
#include "k.h"

typedef struct { sp_int n; sp_int ret; } call_t;
static void call_must_pos(void *p) {
  call_t *c = (call_t *)p;
  c->ret = sp_ExtKernel_s_must_pos(c->n);
}

int main(void) {
  Init_ext_kernel();
  printf("%lld\n", (long long)sp_ExtKernel_s_triple(14LL));
  printf("%s\n", sp_ExtKernel_s_shout(sp_str_from_bytes("hey", 3)));
  sp_IntArray *a = sp_IntArray_new();
  SP_GC_ROOT(a);
  sp_IntArray_push(a, 10); sp_IntArray_push(a, 20); sp_IntArray_push(a, 12);
  printf("%lld\n", (long long)sp_ExtKernel_s_total(a));
  call_t c = { -3, 0 };
  const char *cls = 0, *msg = 0;
  if (Init_ext_kernel_try(call_must_pos, &c, &cls, &msg))
    printf("raised %s: %s\n", cls, msg);
  c.n = 7;
  if (!Init_ext_kernel_try(call_must_pos, &c, &cls, &msg))
    printf("ok %lld\n", (long long)c.ret);
  /* A nullable Integer parameter is an sp_oint: nil is its flag, so INT64_MIN
     is a value. A nullable Integer return is one too. */
  printf("%lld\n", (long long)sp_ExtKernel_s_opt_inc(sp_oint_nil()));
  printf("%lld\n", (long long)sp_ExtKernel_s_opt_inc(sp_oint_of(41LL)));
  printf("%lld\n", (long long)sp_ExtKernel_s_opt_inc(sp_oint_of(INT64_MIN)));
  sp_oint pos = sp_ExtKernel_s_find_pos(a, 20LL);
  if (pos.nil) puts("nil"); else printf("%lld\n", (long long)pos.v);
  pos = sp_ExtKernel_s_find_pos(a, 99LL);
  if (pos.nil) puts("nil"); else printf("%lld\n", (long long)pos.v);
  return 0;
}
