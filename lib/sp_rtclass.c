/* sp_rtclass.c -- the records of classes made at run time (sp_rtclass.h).
 *
 * Compiled into libspinel_rt.a: only a program that makes a class at run
 * time calls sp_rtclass_new, so only such a program links this file.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef SP_THREADS
#include <pthread.h>
#endif

#include "sp_alloc.h"   /* sp_raise_cls, SP_ATOMIC_* */

sp_RtClass *sp_rtclass_chunks[SP_RTCLASS_CHUNKS];
static sp_int sp_rtclass_count;
SP_TLS sp_int sp_rtclass_pending;
#ifdef SP_THREADS
static pthread_mutex_t sp_rtclass_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static unsigned char *sp_rtclass_frozen_flag(sp_int id) { return &sp_rtclass_at(id)->frozen; }

/* A text kept for good: the 0xff marker in front of it is a literal's, which
   the collector leaves alone, so a message staged before anything roots it
   can hold it. NULL when it cannot be allocated. */
static const char *sp_rtclass_text(const char *fmt, int is_module, const void *p) {
  char *m = (char *)malloc(48);
  if (!m) return NULL;
  m[0] = (char)0xff;
  snprintf(m + 1, 47, fmt, is_module ? "Module" : "Class", (unsigned long long)(uintptr_t)p);
  return m + 1;
}

sp_Class sp_rtclass_new(sp_int site, int is_module) {
#ifdef SP_THREADS
  pthread_mutex_lock(&sp_rtclass_lock);
#endif
  sp_int n = sp_rtclass_count;
  sp_RtClass *chunk = n / SP_RTCLASS_CHUNK < SP_RTCLASS_CHUNKS ? sp_rtclass_chunks[n / SP_RTCLASS_CHUNK] : NULL;
  if (!chunk && n / SP_RTCLASS_CHUNK < SP_RTCLASS_CHUNKS)
    chunk = sp_rtclass_chunks[n / SP_RTCLASS_CHUNK] = (sp_RtClass *)calloc(SP_RTCLASS_CHUNK, sizeof(sp_RtClass));
  sp_RtClass *r = chunk ? &chunk[n % SP_RTCLASS_CHUNK] : NULL;
  const char *key = r ? sp_rtclass_text("#<%s:0x%016llx>", is_module, r) : NULL;
  /* the lock is released before the raise, which can longjmp past this
     frame; the count stays, so a later call takes the same slot */
  if (!key) {
#ifdef SP_THREADS
    pthread_mutex_unlock(&sp_rtclass_lock);
#endif
    sp_raise_cls("NoMemoryError", "failed to allocate a class");
  }
  r->site = site;
  r->key = key;
  sp_rtclass_count = n + 1;
  sp_rtclass_frozen_fn = sp_rtclass_frozen_flag;
#ifdef SP_THREADS
  pthread_mutex_unlock(&sp_rtclass_lock);
#endif
  return (sp_Class){ SP_RTCLASS_BASE + n, NULL };
}

/* to_s and inspect: the first constant that names it, else its key */
const char *sp_rtclass_display(sp_int id) {
  sp_RtClass *r = sp_rtclass_at(id);
  const char *nm = SP_ATOMIC_LOAD(&r->name, __ATOMIC_ACQUIRE);
  return nm ? nm : r->key;
}

/* `A = k`: the first name stays, as CRuby's does */
void sp_rtclass_assign_name(sp_int id, const char *name) {
  const char *e = NULL;
  SP_ATOMIC_CAS(&sp_rtclass_at(id)->name, &e, name, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}
