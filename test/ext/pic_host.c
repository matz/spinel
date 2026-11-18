/* A host that loads the library as a shared object (dlopen) and calls its
   entries through dlsym: the init, an Integer entry, a String entry that
   allocates in the shared object's runtime (often enough for its collector
   to run there), and a raise caught through the try helper. The String header
   supplies the native result type and its byte projection. */
#include <dlfcn.h>
#include <stdio.h>
#include "sp_string.h"

typedef struct { sp_int (*fn)(sp_int); sp_int n; sp_int ret; } call_t;
static void call(void *p) {
  call_t *c = (call_t *)p;
  c->ret = c->fn(c->n);
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  void (*init)(void) = (void (*)(void))dlsym(h, "Init_ext_pic");
  int (*try_)(void (*)(void *), void *, const char **, const char **) =
    (int (*)(void (*)(void *), void *, const char **, const char **))dlsym(h, "Init_ext_pic_try");
  sp_int (*triple)(sp_int) = (sp_int (*)(sp_int))dlsym(h, "sp_ExtPic_s_triple");
  sp_String *(*label)(sp_int) = (sp_String *(*)(sp_int))dlsym(h, "sp_ExtPic_s_label");
  sp_int (*must_pos)(sp_int) = (sp_int (*)(sp_int))dlsym(h, "sp_ExtPic_s_must_pos");
  if (!init || !try_ || !triple || !label || !must_pos) { printf("dlsym: an entry is missing\n"); return 1; }
  init();
  printf("%lld\n", (long long)triple(14));
  printf("%s\n", sp_String_cstr(label(3)));
  for (int i = 0; i < 2000; i++) label(50);
  printf("%s\n", sp_String_cstr(label(5)));
  call_t c = { must_pos, -3, 0 };
  const char *cls = 0, *msg = 0;
  if (try_(call, &c, &cls, &msg)) printf("raised %s: %s\n", cls, msg);
  c.n = 7;
  if (!try_(call, &c, &cls, &msg)) printf("ok %lld\n", (long long)c.ret);
  return 0;
}
