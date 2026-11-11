#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef __APPLE__
#include <dlfcn.h>
#endif

/* Replace only this child process's realtime clock; never the host clock. */
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
  if (clock == CLOCK_REALTIME) {
    const char *seconds = getenv("SPINEL_TEST_CLOCK_SEC");
    if (seconds) {
      ts->tv_sec = (time_t)strtoll(seconds, NULL, 10);
      ts->tv_nsec = strtol(getenv("SPINEL_TEST_CLOCK_NSEC"), NULL, 10);
      return 0;
    }
    int64_t ns = strtoll(getenv("SPINEL_TEST_CLOCK_NS"), NULL, 10);
    ts->tv_sec = ns / 1000000000LL;
    ts->tv_nsec = ns % 1000000000LL;
    if (ts->tv_nsec < 0) { ts->tv_sec--; ts->tv_nsec += 1000000000L; }
    return 0;
  }
#ifdef __APPLE__
  return clock_gettime(clock, ts);
#else
  int (*real_clock)(clockid_t, struct timespec *) = dlsym(RTLD_NEXT, "clock_gettime");
  return real_clock(clock, ts);
#endif
}
#ifdef __APPLE__
__attribute__((used)) static struct { const void *replacement, *original; }
interpose __attribute__((section("__DATA,__interpose"))) = {
  (const void *)(uintptr_t)&test_clock_gettime, (const void *)(uintptr_t)&clock_gettime
};
#else
int clock_gettime(clockid_t clock, struct timespec *ts) { return test_clock_gettime(clock, ts); }
#endif
