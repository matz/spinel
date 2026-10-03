/* The decision registry.

   Some optimizations are a miscompile when their legality check is wrong: a
   nil test dropped, a GC root not kept, a buffer read without a copy. Each
   had, at best, a switch for the whole program (--no-root-elision and its
   kin), so a wrong answer was narrowed by guessing a switch and then reading
   the program's C for the site.

   Here every such decision asks one question before it is taken, and the
   question carries a key, `kind@site`:

     nn-read@app.rb:12:5:x        a decision at a node: file, line, column
     root-frame@Parser#advance    a decision about a method
     root-frame@main              ... about the top-level body
     no-alloc@-                   a node the parser gave no position

   SPINEL_DECISIONS_LOG=FILE writes the keys of the decisions the compile
   took, one per line, each once, in the order they were first asked.
   SPINEL_DECISIONS=FILE is an allow-list in the same format (a line that
   starts with `#` is a comment): a decision whose key is not listed is not
   taken. An empty file therefore denies every keyed decision, and a compile
   given its own log makes the decisions it made before. `spinel bisect`
   (tools/bisect.rb) searches the list for the keys behind a wrong answer.

   A key is a name, not a count. The type fixpoint asks the same question
   every round, and codegen emits one node into several inline sites and into
   buffers it later throws away, so the n-th question is a different one from
   compile to compile; the question at app.rb:12:5 is not. For the same
   reason no key holds a node id, a `_tN` temp, a proc number or a frame
   slot: those come from counters that a denied decision moves.

   An unlisted key is denied because the set of keys is not closed under
   denial: refusing one nil-narrowing fact can make another read start asking
   for one. Under an allow-list a compile can only take decisions the
   unrestricted compile also took.

   Adding a kind is one decide_node/decide_fn call where the optimization is
   about to be applied, after its own check has said yes, one line in the
   table in tools/README.md, and its name in DECISION_KINDS in the Makefile,
   which `make decisions-test` holds the table to. With neither variable set
   the call is a test of one global and the emitted C is what it was. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "decide.h"

int g_decide_on = 0;

/* a set of strings, remembering insertion order */
typedef struct { char **key; int n, cap; int *slot; int nslot; } KeySet;

static int set_find(const KeySet *ks, const char *k) {
  if (!ks->nslot) return -1;
  for (unsigned i = sp_strhash(k) % (unsigned)ks->nslot; ; i = (i + 1) % (unsigned)ks->nslot) {
    int at = ks->slot[i];
    if (at < 0) return -1;
    if (!strcmp(ks->key[at], k)) return at;
  }
}

static void set_place(KeySet *ks, int at) {
  unsigned i = sp_strhash(ks->key[at]) % (unsigned)ks->nslot;
  while (ks->slot[i] >= 0) i = (i + 1) % (unsigned)ks->nslot;
  ks->slot[i] = at;
}

/* Returns 1 when `k` was not in the set before. */
static int set_add(KeySet *ks, const char *k) {
  if (set_find(ks, k) >= 0) return 0;
  if (ks->n == ks->cap) {
    ks->cap = ks->cap ? ks->cap * 2 : 64;
    ks->key = realloc(ks->key, sizeof(char *) * (size_t)ks->cap);
  }
  ks->key[ks->n++] = strdup(k);
  if (ks->n * 2 > ks->nslot) {
    ks->nslot = ks->nslot ? ks->nslot * 2 : 128;
    ks->slot = realloc(ks->slot, sizeof(int) * (size_t)ks->nslot);
    for (int i = 0; i < ks->nslot; i++) ks->slot[i] = -1;
    for (int i = 0; i < ks->n; i++) set_place(ks, i);
  }
  else set_place(ks, ks->n - 1);
  return 1;
}

static KeySet g_allowed, g_taken;
static int g_have_allow = 0;
static const char *g_log_path = NULL;

int decide_setup(void) {
  const char *allow = getenv("SPINEL_DECISIONS");
  const char *log = getenv("SPINEL_DECISIONS_LOG");
  if (allow && *allow) {
    /* a list that cannot be read is not an empty list: that would deny
       everything and call the result the answer */
    FILE *f = fopen(allow, "r");
    if (!f) { fprintf(stderr, "spinel: cannot read decisions file '%s'\n", allow); exit(1); }
    char *line = NULL; size_t cap = 0; ssize_t len;
    while ((len = getline(&line, &cap, f)) >= 0) {
      while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
      if (len == 0 || line[0] == '#') continue;
      set_add(&g_allowed, line);
    }
    /* nor is one that stops being readable part way (a directory opens) */
    int failed = ferror(f);
    free(line);
    fclose(f);
    if (failed) { fprintf(stderr, "spinel: cannot read decisions file '%s'\n", allow); exit(1); }
    g_have_allow = 1;
    g_decide_on = 1;
  }
  if (log && *log) {
    /* emptied now, so a compile that stops early leaves no earlier log */
    FILE *f = fopen(log, "w");
    if (!f) { fprintf(stderr, "spinel: cannot write decisions log '%s'\n", log); exit(1); }
    fclose(f);
    g_log_path = log;
    g_decide_on = 1;
  }
  return g_decide_on;
}

void decide_write_log(void) {
  if (!g_log_path) return;
  FILE *f = fopen(g_log_path, "w");
  if (!f) { fprintf(stderr, "spinel: cannot write decisions log '%s'\n", g_log_path); exit(1); }
  for (int i = 0; i < g_taken.n; i++) { fputs(g_taken.key[i], f); fputc('\n', f); }
  /* a log cut short would read as a compile that took fewer decisions */
  int failed = ferror(f);
  if (fclose(f) != 0 || failed) { fprintf(stderr, "spinel: cannot write decisions log '%s'\n", g_log_path); exit(1); }
}

static int decide_key(const char *key) {
  if (g_have_allow && set_find(&g_allowed, key) < 0) return 0;
  if (g_log_path) set_add(&g_taken, key);
  return 1;
}

/* A key is as long as its parts. One cut to fit a buffer could spell another
   key, and the two decisions would be allowed, denied and named as one. */
int decide_fn_keyed(const char *kind, const char *site, const char *name) {
  static char *key = NULL; static size_t cap = 0;
  int named = name && *name;
  size_t need = strlen(kind) + strlen(site) + (named ? strlen(name) : 0) + 3;
  if (need > cap) { cap = need * 2; key = realloc(key, cap); }
  if (named) snprintf(key, cap, "%s@%s:%s", kind, site, name);
  else snprintf(key, cap, "%s@%s", kind, site);
  return decide_key(key);
}

const char *decide_node_site(const NodeTable *nt, int id) {
  static char *site = NULL; static size_t cap = 0;
  if (!g_decide_on) return "";
  int line = (int)nt_int(nt, id, "node_line", 0);
  /* a node some rewrite made without copying a position: all of them of one
     kind share a key, so the log still lists every key that was asked */
  if (line <= 0) return "-";
  const char *file = nt_file_path(nt, (int)nt_int(nt, id, "node_file", 0));
  if (!file || !*file) file = nt->source_file;
  if (!file || !*file) file = "source.rb";
  /* the parser's column counts from 0; a position a person reads counts from 1 */
  size_t need = strlen(file) + 32;
  if (need > cap) { cap = need * 2; site = realloc(site, cap); }
  snprintf(site, cap, "%s:%d:%d", file, line, (int)nt_int(nt, id, "node_col", 0) + 1);
  return site;
}

int decide_node_keyed(const NodeTable *nt, int id, const char *kind, const char *name) {
  return decide_fn_keyed(kind, decide_node_site(nt, id), name);
}
