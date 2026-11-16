/* test/ext/threads.rb built into a shared object over the threaded
   position-independent runtime (lib/libspinel_rt_mt_pic.a), loaded with
   dlopen: each entry starts and joins its own threads, after an init that
   already started some, and every one of them reaches its thread-local
   runtime state through the dynamic TLS model. No Spinel header: sp_int is
   intptr_t. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef intptr_t sp_int;

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  void (*init)(void) = (void (*)(void))dlsym(h, "Init_ext_threads");
  sp_int (*par)(sp_int) = (sp_int (*)(sp_int))dlsym(h, "sp_ExtThreads_s_par");
  sp_int (*bg)(void) = (sp_int (*)(void))dlsym(h, "sp_ExtThreads_s_bg");
  if (!init || !par || !bg) { printf("dlsym: an entry is missing\n"); return 1; }
  init();
  printf("%lld\n", (long long)par(4));
  printf("%lld\n", (long long)par(6));
  printf("%lld\n", (long long)bg());
  return 0;
}
