/* test/ext/pic_pkg.rb built into a shared object with the json package's
   position-independent object, loaded with dlopen: each entry runs the
   package's C (JSON.generate, JSON.parse) inside the shared object. No
   Spinel header: sp_int is intptr_t. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef intptr_t sp_int;

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  void (*init)(void) = (void (*)(void))dlsym(h, "Init_ext_pic_pkg");
  const char *(*dump)(sp_int) = (const char *(*)(sp_int))dlsym(h, "sp_ExtPicPkg_s_dump");
  sp_int (*sum)(sp_int) = (sp_int (*)(sp_int))dlsym(h, "sp_ExtPicPkg_s_sum");
  if (!init || !dump || !sum) { printf("dlsym: an entry is missing\n"); return 1; }
  init();
  printf("%s\n", dump(7));
  printf("%lld\n", (long long)sum(10));
  return 0;
}
