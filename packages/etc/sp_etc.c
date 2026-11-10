/* sp_etc.c -- the user and group databases for the `etc` spin package
   (Path B carried C), linked on demand when `require "etc"` appears.

   Each lookup answers one record as a String on the GC heap, or NULL for
   nil when there is no such entry: the fields of the passwd(5) entry
   (name, passwd, uid, gid, gecos, dir, shell) or of the group(5) entry
   (name, passwd, gid, the members joined by commas), separated by the
   unit separator (0x1f). A walk answers every record, separated by the
   record separator (0x1e). The Ruby of the package splits them and
   builds the Structs; nothing here is thread-safe beyond what
   getpwent(3) and getgrent(3) are.

   wasm32-wasi has no user or group database (docs/wasm.md): there a
   lookup finds no entry, a walk finds none, and the login name is
   $USER's. */
#include "spinel/runtime.h"
#ifndef __wasi__
#include <grp.h>
#endif
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SP_ETC_FS "\x1f"
#define SP_ETC_RS "\x1e"

/* A growing byte buffer that ends as one GC string. Only the final
   allocation touches the GC heap, so nothing needs rooting. */
typedef struct {
  char *p;
  size_t n, cap;
} etc_buf;

static void buf_add(etc_buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->n + n + 1) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) sp_oom_die();
    b->p = p;
    b->cap = cap;
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
}

static void buf_str(etc_buf *b, const char *s) {
  if (s) buf_add(b, s, strlen(s));
}

static void buf_int(etc_buf *b, long long v) {
  char t[32];
  int n = snprintf(t, sizeof t, "%lld", v);
  buf_add(b, t, (size_t)n);
}

static void buf_fs(etc_buf *b) { buf_add(b, SP_ETC_FS, 1); }

static const char *buf_done(etc_buf *b) {
  char *r = sp_str_alloc_raw(b->n + 1);
  if (b->n) memcpy(r, b->p, b->n);
  r[b->n] = '\0';
  sp_str_set_len(r, b->n);
  free(b->p);
  return r;
}

static void passwd_record(etc_buf *b, const struct passwd *pw) {
  buf_str(b, pw->pw_name);
  buf_fs(b);
  buf_str(b, pw->pw_passwd);
  buf_fs(b);
  buf_int(b, (long long)pw->pw_uid);
  buf_fs(b);
  buf_int(b, (long long)pw->pw_gid);
  buf_fs(b);
  buf_str(b, pw->pw_gecos);
  buf_fs(b);
  buf_str(b, pw->pw_dir);
  buf_fs(b);
  buf_str(b, pw->pw_shell);
}

#ifndef __wasi__
static void group_record(etc_buf *b, const struct group *gr) {
  buf_str(b, gr->gr_name);
  buf_fs(b);
  buf_str(b, gr->gr_passwd);
  buf_fs(b);
  buf_int(b, (long long)gr->gr_gid);
  buf_fs(b);
  for (char **m = gr->gr_mem; m && *m; m++) {
    if (m != gr->gr_mem) buf_add(b, ",", 1);
    buf_str(b, *m);
  }
}

static const char *one_passwd(const struct passwd *pw) {
  if (!pw) return NULL;
  etc_buf b = { 0 };
  passwd_record(&b, pw);
  return buf_done(&b);
}

static const char *one_group(const struct group *gr) {
  if (!gr) return NULL;
  etc_buf b = { 0 };
  group_record(&b, gr);
  return buf_done(&b);
}
#endif

/* Etc.getlogin: getlogin(3), else $USER, else nil, as CRuby's etc.c. */
const char *sp_etc_getlogin(void) {
#ifdef __wasi__
  const char *l = NULL;
#else
  const char *l = getlogin();
#endif
  if (!l || !*l) l = getenv("USER");
  if (!l || !*l) return NULL;
  size_t n = strlen(l);
  char *r = sp_str_alloc_raw(n + 1);
  memcpy(r, l, n + 1);
  sp_str_set_len(r, n);
  return r;
}

const char *sp_etc_getpwnam(const char *name) { return one_passwd(getpwnam(name)); }
const char *sp_etc_getpwuid(sp_int uid) { return one_passwd(getpwuid((uid_t)uid)); }
#ifdef __wasi__
const char *sp_etc_getgrnam(const char *name) { (void)name; return NULL; }
const char *sp_etc_getgrgid(sp_int gid) { (void)gid; return NULL; }
#else
const char *sp_etc_getgrnam(const char *name) { return one_group(getgrnam(name)); }
const char *sp_etc_getgrgid(sp_int gid) { return one_group(getgrgid((gid_t)gid)); }
#endif

/* Every entry of the user database, from the first. */
const char *sp_etc_passwd_all(void) {
  etc_buf b = { 0 };
#ifndef __wasi__
  const struct passwd *pw;
  setpwent();
  while ((pw = getpwent()) != NULL) {
    if (b.n) buf_add(&b, SP_ETC_RS, 1);
    passwd_record(&b, pw);
  }
  endpwent();
#endif
  return buf_done(&b);
}

/* Every entry of the group database, from the first. */
const char *sp_etc_group_all(void) {
  etc_buf b = { 0 };
#ifndef __wasi__
  const struct group *gr;
  setgrent();
  while ((gr = getgrent()) != NULL) {
    if (b.n) buf_add(&b, SP_ETC_RS, 1);
    group_record(&b, gr);
  }
  endgrent();
#endif
  return buf_done(&b);
}
