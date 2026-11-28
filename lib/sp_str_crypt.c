/* sp_str_crypt.c -- String#crypt, through libc crypt(3).
 *
 * An object of its own in libspinel_rt.a, so only a program that calls
 * String#crypt pulls it in: in sp_str.o every program referenced crypt(),
 * and the link line's -lcrypt made libcrypt a dependency of hello world
 * (#6674). src/main.c adds -lcrypt only when the generated C calls
 * sp_str_crypt. */
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include "sp_str.h"
#include "sp_exc.h"
/* crypt(3) answers a buffer of its own, one for the process: threads
   calling it at once read each other's hash (#11846). The call and the copy
   of its answer are made under this lock (glibc's crypt_r would do, but
   macOS has none). */
#ifdef SP_THREADS
#include <pthread.h>
static pthread_mutex_t sp_crypt_lock = PTHREAD_MUTEX_INITIALIZER;
#define SP_CRYPT_LOCK()   pthread_mutex_lock(&sp_crypt_lock)
#define SP_CRYPT_UNLOCK() pthread_mutex_unlock(&sp_crypt_lock)
#else
#define SP_CRYPT_LOCK()   ((void)0)
#define SP_CRYPT_UNLOCK() ((void)0)
#endif
const char *sp_str_crypt(const char *s, const char *salt) {SP_GC_ROOT_STR(s);SP_GC_ROOT_STR(salt);
  /* the real libc crypt(3) -- DES with a 2-char salt (or the platform's
     extended schemes), byte-identical to CRuby's String#crypt (#2398).
     Declared by hand: glibc hides it behind crypt.h/_XOPEN_SOURCE while
     macOS ships it in unistd.h. */
  extern char *crypt(const char *key, const char *slt);
  if (!s) sp_nil_recv("crypt");
  /* (crypt(3) reads to a NUL: a receiver with one inside is CRuby's
     ArgumentError, not a hash of the bytes before it; a salt is read as
     crypt(3) reads it, past its first two bytes) */
  if (memchr(s, '\0', sp_str_byte_len(s))) sp_raise_cls("ArgumentError", "string contains null byte");
  if (!salt || !salt[0] || !salt[1])
    sp_raise_cls("ArgumentError", "salt too short (need >=2 bytes)");
  /* (no allocation runs while the lock is held: the answer is copied to
     the stack, then to a String) */
  char hash[256];
  int e = 0, fail = 0;
  SP_CRYPT_LOCK();
  errno = 0;
  char *d = crypt(s, salt);
  /* a salt the platform's crypt(3) refuses: CRuby raises the errno it
     leaves ("Invalid argument - crypt"). OpenBSD's crypt returns NULL (it
     takes only bcrypt); libxcrypt's returns a failure token, "*0" or "*1",
     which no hash starts with */
  if (!d || d[0] == '*') { e = errno ? errno : EINVAL; fail = 1; }
  else { strncpy(hash, d, sizeof hash - 1); hash[sizeof hash - 1] = 0; }
  SP_CRYPT_UNLOCK();
  if (fail) sp_raise_cls(sp_errno_class_name(e), sp_sprintf("%s - crypt", strerror(e)));
  const char *r = sp_str_dup_external(hash);
  sp_str_mark_binary((char *)r);   /* (the hash is BINARY, as CRuby's) */
  return r;
}
