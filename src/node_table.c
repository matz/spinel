#include "node_table.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- field append helpers ---- */

static const SpNode *node_at(const NodeTable *nt, int id);

/* An allocation that fails stops the compile, here as in the parser's walk
   below: a node left without its type, a field without its key or a table
   without the slot it was growing by is read later as if it were there. */
static void nt_oom(void) {
  fprintf(stderr, "spinel: out of memory\n");
  exit(1);
}

/* n bytes, never NULL (an empty array still gets a block). */
static void *nt_alloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) nt_oom();
  return p;
}

static void ensure_cap(void **buf, int *cap, int need, size_t elem) {
  if (need <= *cap) return;
  if (*cap > INT_MAX / 2) nt_oom();
  int nc = *cap ? *cap * 2 : 4;
  if (nc < need) nc = need;
  void *grown = realloc(*buf, (size_t)nc * elem);
  if (!grown) nt_oom();
  *buf = grown;
  *cap = nc;
}

/* A copy of the n bytes at s, NUL-terminated. */
static char *dup_n(const char *s, size_t n) {
  char *p = nt_alloc(n + 1);
  memcpy(p, s, n);
  p[n] = '\0';
  return p;
}

static void node_set_type(SpNode *nd, const char *type, size_t len) {
  free(nd->type);
  nd->type = dup_n(type, len);
}

static void node_add_str(SpNode *nd, const char *key, size_t klen, char *val /*owned*/, size_t vlen) {
  ensure_cap((void **)&nd->s, &nd->cs, nd->ns + 1, sizeof(SpStrField));
  nd->s[nd->ns].key = dup_n(key, klen);
  nd->s[nd->ns].disc = SP_FIELD_DISC(key, klen);
  nd->s[nd->ns].val = val;
  nd->s[nd->ns].val_len = vlen;
  nd->ns++;
}

static void node_add_int(SpNode *nd, const char *key, size_t klen, long long v) {
  ensure_cap((void **)&nd->i, &nd->ci, nd->ni + 1, sizeof(SpIntField));
  nd->i[nd->ni].key = dup_n(key, klen);
  nd->i[nd->ni].disc = SP_FIELD_DISC(key, klen);
  nd->i[nd->ni].val = v;
  nd->ni++;
}

static void node_add_ref(SpNode *nd, const char *key, size_t klen, int ref) {
  ensure_cap((void **)&nd->r, &nd->cr, nd->nr + 1, sizeof(SpRefField));
  nd->r[nd->nr].key = dup_n(key, klen);
  nd->r[nd->nr].disc = SP_FIELD_DISC(key, klen);
  nd->r[nd->nr].ref = ref;
  nd->nr++;
}

static void node_add_arr(SpNode *nd, const char *key, size_t klen, int *ids, int n /*owned*/) {
  ensure_cap((void **)&nd->a, &nd->ca, nd->na + 1, sizeof(SpArrField));
  nd->a[nd->na].key = dup_n(key, klen);
  nd->a[nd->na].disc = SP_FIELD_DISC(key, klen);
  nd->a[nd->na].ids = ids;
  nd->a[nd->na].n = n;
  nd->na++;
}

/* A node's entry in nt->aux (node_table.h), the array first extended to
   it: a new entry is unmarked, uncopied and of no origin. */
struct NtAux { unsigned mark, pass; int to, from, a, k; char how; };
static struct NtAux *nt_aux(NodeTable *nt, int id) {
  if (id >= nt->aux_cap) {
    int cap = nt->aux_cap ? nt->aux_cap : 1024;
    while (cap <= id) { if (cap > INT_MAX / 2) nt_oom(); cap *= 2; }
    struct NtAux *g = realloc(nt->aux, sizeof *g * (size_t)cap);
    if (!g) nt_oom();
    for (int i = nt->aux_cap; i < cap; i++) g[i] = (struct NtAux){ 0, 0, -1, -1, 0, 0, 0 };
    nt->aux = g; nt->aux_cap = cap;
  }
  return &nt->aux[id];
}
/* ids [lo, hi) taken anew (a rewrite can drop the nodes it appended and
   append others in their place) */
static void nt_aux_reset(NodeTable *nt, int lo, int hi) {
  for (int i = lo; i < hi && i < nt->aux_cap; i++) nt->aux[i] = (struct NtAux){ 0, 0, -1, -1, 0, 0, 0 };
}

/* The pass appending now, as its origins carry it: a hash of its name,
   0 for none */
static unsigned nt_pass_hash(const NodeTable *nt) {
  if (!nt->pass) return 0;
  unsigned h = 2166136261u;
  for (const char *q = nt->pass; *q; q++) h = (h ^ (unsigned char)*q) * 16777619u;
  return h ? h : 1;
}
/* One origin's count, in nt->counts: how many nodes the pass has given
   (from, how, a) so far. */
struct NtCount { unsigned pass; int from, a, n; char how; };
static unsigned nt_count_slot(const struct NtCount *e, int cap, unsigned pass, int from, char how, int a) {
  unsigned i = (pass ^ (unsigned)from * 2654435761u ^ (unsigned)a * 40503u ^ (unsigned char)how) & (unsigned)(cap - 1);
  while (e[i].n && !(e[i].pass == pass && e[i].from == from && e[i].how == how && e[i].a == a))
    i = (i + 1) & (unsigned)(cap - 1);
  return i;
}
/* the k of the next node the pass (its hash) gives (from, how, a):
   counted in that pass's own walk, so another pass that runs before or
   after it does not move it */
static int nt_count_next(NodeTable *nt, unsigned pass, int from, char how, int a) {
  if (2 * (nt->ncounts + 1) > nt->counts_cap) {
    int cap = nt->counts_cap ? nt->counts_cap * 2 : 256;
    struct NtCount *g = calloc((size_t)cap, sizeof *g);
    if (!g) nt_oom();
    for (int i = 0; i < nt->counts_cap; i++)
      if (nt->counts[i].n) {
        const struct NtCount *o = &nt->counts[i];
        g[nt_count_slot(g, cap, o->pass, o->from, o->how, o->a)] = *o;
      }
    free(nt->counts);
    nt->counts = g; nt->counts_cap = cap;
  }
  struct NtCount *e = &nt->counts[nt_count_slot(nt->counts, nt->counts_cap, pass, from, how, a)];
  if (!e->n) { *e = (struct NtCount){ pass, from, a, 0, how }; nt->ncounts++; }
  return ++e->n;
}
/* `node` given its origin: its source (how 'c'), or the node it is first
   linked under (how 'r': ref field a; 'a': array field a), by the pass
   appending now, as that pass's k-th such node */
static void nt_set_origin(NodeTable *nt, int node, int from, char how, int a) {
  struct NtAux *x = nt_aux(nt, node);
  x->from = from; x->how = how; x->a = a;
  x->pass = nt_pass_hash(nt);
  x->k = nt_count_next(nt, x->pass, from, how, a);
}
/* `child` linked under `parent`: the first link of a node appended since
   nt_track_origins is where it came from */
static void nt_place(NodeTable *nt, int parent, int child, char how, int a) {
  if (!nt->orig_lo || child < nt->orig_lo || child >= nt->count) return;
  if (!nt_aux(nt, child)->how) nt_set_origin(nt, child, parent, how, a);
}

void nt_track_origins(NodeTable *nt) { nt->orig_lo = nt->count > 0 ? nt->count : 1; }

const char *nt_set_pass(NodeTable *nt, const char *pass) {
  const char *was = nt->pass;
  nt->pass = pass;
  return was;
}

int nt_origin(const NodeTable *nt, int id, char *how, int *a, unsigned *pass, int *k) {
  if (!nt->orig_lo || id < nt->orig_lo || id >= nt->aux_cap || !nt->aux[id].how) return -1;
  const struct NtAux *x = &nt->aux[id];
  *how = x->how; *a = x->a; *pass = x->pass; *k = x->k;
  return x->from;
}

/* ---- filling the table (the parser's walk) ---- */

NodeTable *nt_new_table(void) {
  NodeTable *nt = calloc(1, sizeof(NodeTable));
  if (!nt) nt_oom();
  return nt;
}

/* The node at `id`, the table first extended to it; the slots it grows by
   start empty. */
static SpNode *node_put(NodeTable *nt, int id) {
  if (id < 0) return NULL;
  if (id >= nt->node_cap) {
    int nc = nt->node_cap ? nt->node_cap : 256;
    while (nc <= id) {
      if (nc > INT_MAX / 2) nt_oom();
      nc *= 2;
    }
    SpNode *grown = realloc(nt->nodes, sizeof(SpNode) * (size_t)nc);
    if (!grown) nt_oom();
    memset(grown + nt->node_cap, 0, sizeof(SpNode) * (size_t)(nc - nt->node_cap));
    nt->nodes = grown;
    nt->node_cap = nc;
  }
  if (id >= nt->count) { nt_aux_reset(nt, nt->count, id + 1); nt->count = id + 1; }
  return &nt->nodes[id];
}

void nt_put_type(NodeTable *nt, int id, const char *type) {
  SpNode *nd = node_put(nt, id);
  if (nd) node_set_type(nd, type, strlen(type));
}
void nt_put_str(NodeTable *nt, int id, const char *key, const char *val, size_t len) {
  if (len == 16 && !memcmp(val, "__cext_extension", 16) && !strcmp(key, "name")) nt->has_cext_decls = 1;
  SpNode *nd = node_put(nt, id);
  if (nd) node_add_str(nd, key, strlen(key), dup_n(val, len), len);
}
void nt_put_int(NodeTable *nt, int id, const char *key, long long val) {
  SpNode *nd = node_put(nt, id);
  if (nd) node_add_int(nd, key, strlen(key), val);
}
void nt_put_ref(NodeTable *nt, int id, const char *key, int child) {
  SpNode *nd = node_put(nt, id);
  if (!nd) return;
  node_add_ref(nd, key, strlen(key), child);
  nt_place(nt, id, child, 'r', nd->nr - 1);
}
void nt_put_arr(NodeTable *nt, int id, const char *key, int *ids, int n) {
  SpNode *nd = node_put(nt, id);
  if (!nd) { free(ids); return; }
  node_add_arr(nd, key, strlen(key), ids, n);
  for (int e = 0; e < n; e++) nt_place(nt, id, ids[e], 'a', nd->na - 1);
}
void nt_put_content(NodeTable *nt, int id, const char *content) {
  SpNode *nd = node_put(nt, id);
  if (!nd) return;
  free(nd->content);
  nd->content = dup_n(content, strlen(content));
}
void nt_put_source_file(NodeTable *nt, const char *path) {
  free(nt->source_file);
  nt->source_file = dup_n(path, strlen(path));
}
void nt_put_file(NodeTable *nt, int fid, const char *path) {
  if (fid < 0) return;
  if (fid >= nt->nfiles) {
    char **grown = realloc(nt->files, sizeof(char *) * (size_t)(fid + 1));
    if (!grown) nt_oom();
    for (int k = nt->nfiles; k <= fid; k++) grown[k] = NULL;
    nt->files = grown;
    nt->nfiles = fid + 1;
  }
  free(nt->files[fid]);
  nt->files[fid] = dup_n(path, strlen(path));
}

/* ---- the text form (--dump-ast) ---- */

/* A string value on one line: the bytes that would end the record or the
   field (newline, space, NUL) and the escape character itself as %XX. */
static void dump_escaped(FILE *out, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '%' || c == '\n' || c == '\r' || c == '\t' || c == ' ' || c == 0) fprintf(out, "%%%02X", c);
    else fputc(c, out);
  }
}

/* A hash of what the nodes hold: each one's type, content and fields, in
   id order. Two tables with the same hash are, for a check that compares a
   table before and after a pass, the same. */
static unsigned long long nt_hash_bytes(unsigned long long h, const void *p, size_t n) {
  const unsigned char *b = p;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
static unsigned long long nt_hash_str(unsigned long long h, const char *s) {
  return s ? nt_hash_bytes(h, s, strlen(s) + 1) : nt_hash_bytes(h, "", 1);
}
unsigned long long nt_content_hash(const NodeTable *nt) {
  unsigned long long h = 14695981039346656037ull;
  h = nt_hash_bytes(h, &nt->count, sizeof nt->count);
  for (int id = 0; id < nt->count; id++) {
    const SpNode *nd = &nt->nodes[id];
    h = nt_hash_str(h, nd->type);
    h = nt_hash_str(h, nd->content);
    for (int j = 0; j < nd->ns; j++) {
      h = nt_hash_str(h, nd->s[j].key);
      h = nt_hash_bytes(h, nd->s[j].val, nd->s[j].val_len);
    }
    for (int j = 0; j < nd->ni; j++) {
      h = nt_hash_str(h, nd->i[j].key);
      h = nt_hash_bytes(h, &nd->i[j].val, sizeof nd->i[j].val);
    }
    for (int j = 0; j < nd->nr; j++) {
      h = nt_hash_str(h, nd->r[j].key);
      h = nt_hash_bytes(h, &nd->r[j].ref, sizeof nd->r[j].ref);
    }
    for (int j = 0; j < nd->na; j++) {
      h = nt_hash_str(h, nd->a[j].key);
      h = nt_hash_bytes(h, nd->a[j].ids, sizeof(int) * (size_t)nd->a[j].n);
    }
  }
  return h;
}

void nt_dump(const NodeTable *nt, FILE *out) {
  fprintf(out, "ROOT %d\n", nt->root_id);
  if (nt->source_file) {
    fputs("SOURCE_FILE ", out);
    dump_escaped(out, nt->source_file, strlen(nt->source_file));
    fputc('\n', out);
  }
  for (int f = 0; f < nt->nfiles; f++) {
    if (!nt->files[f]) continue;
    fprintf(out, "FILE %d ", f);
    dump_escaped(out, nt->files[f], strlen(nt->files[f]));
    fputc('\n', out);
  }
  for (int id = 0; id < nt->count; id++) {
    const SpNode *nd = &nt->nodes[id];
    if (nd->type) fprintf(out, "N %d %s\n", id, nd->type);
    for (int j = 0; j < nd->ns; j++) {
      fprintf(out, "S %d %s ", id, nd->s[j].key);
      dump_escaped(out, nd->s[j].val, nd->s[j].val_len);
      fputc('\n', out);
    }
    for (int j = 0; j < nd->ni; j++) fprintf(out, "I %d %s %lld\n", id, nd->i[j].key, nd->i[j].val);
    if (nd->content) fprintf(out, "F %d value %s\n", id, nd->content);
    for (int j = 0; j < nd->nr; j++) fprintf(out, "R %d %s %d\n", id, nd->r[j].key, nd->r[j].ref);
    for (int j = 0; j < nd->na; j++) {
      fprintf(out, "A %d %s ", id, nd->a[j].key);
      for (int k = 0; k < nd->a[j].n; k++) fprintf(out, k ? ",%d" : "%d", nd->a[j].ids[k]);
      fputc('\n', out);
    }
  }
}

/* A walk that marks the nodes it collects begins: no mark an earlier one
   left looks current. */
static void nt_mark_begin(NodeTable *nt) {
  nt_aux(nt, nt->count - 1);
  if (++nt->clone_gen == 0) {   /* wrapped: no mark may look current */
    for (int i = 0; i < nt->aux_cap; i++) nt->aux[i].mark = 0;
    nt->clone_gen = 1;
  }
}

/* Collect the subtree node ids reachable from `id` via ref + array fields,
   each marked as in the walk being made (a clone's, nt_subtree_ids'). */
static void nt_subtree_collect(NodeTable *nt, int id, int **list, int *n, int *cap) {
  if (id < 0 || id >= nt->count || nt->aux[id].mark == nt->clone_gen) return;
  nt->aux[id].mark = nt->clone_gen;
  if (*n == *cap) {
    int *g = realloc(*list, sizeof(int) * (size_t)(*cap *= 2));
    if (!g) nt_oom();
    *list = g;
  }
  (*list)[(*n)++] = id;
  SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) nt_subtree_collect(nt, nd->r[j].ref, list, n, cap);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) nt_subtree_collect(nt, nd->a[j].ids[k], list, n, cap);
}

/* A field's node id in the clone: its copy for a node of the subtree */
static int nt_clone_map(const NodeTable *nt, int base, int id) {
  return id >= 0 && id < base && nt->aux[id].mark == nt->clone_gen ? nt->aux[id].to : id;
}

int *nt_subtree_ids(NodeTable *nt, const int *roots, int n, int *out) {
  int cap = 64, sn = 0;
  int *list = nt_alloc(sizeof(int) * (size_t)cap);
  if (nt->count > 0) {
    nt_mark_begin(nt);
    for (int i = 0; i < n; i++) nt_subtree_collect(nt, roots[i], &list, &sn, &cap);
  }
  *out = sn;
  return list;
}

/* Retype every node of the subtree at `id` to a NilNode: a desugar that
   replaced the subtree leaves it out of the program, and the passes that
   walk the node table by id rather than by tree would still see its
   definitions, block parameters and locals, and declare each of them
   (rooted) in main. A NilNode is what those passes skip. A replaced subtree
   stays visible to such by-id walks; making its detachment real belongs to
   the load-phase elaborator's work (#9898). */
void nt_subtree_blank(NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return;
  SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) nt_subtree_blank(nt, nd->r[j].ref);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) nt_subtree_blank(nt, nd->a[j].ids[k]);
  nt_node_set_type(nt, id, "NilNode");
}

/* Deep-clone the subtree rooted at `root`, appending fresh nodes to the
   table and remapping internal child references. Refs/array elements that
   point outside the subtree are kept verbatim. Returns the new root id, or
   -1 for a root outside the table. Grows nt->count; callers must resize parallel per-node
   arrays (Compiler.ntype / .nscope) to match afterward. */
int nt_clone_subtree(NodeTable *nt, int root) {
  if (root < 0 || root >= nt->count) return -1;
  int base = nt->count;
  nt_mark_begin(nt);
  int cap = 64, sn = 0;
  int *list = nt_alloc(sizeof(int) * (size_t)cap);
  nt_subtree_collect(nt, root, &list, &sn, &cap);
  for (int i = 0; i < sn; i++) nt->aux[list[i]].to = base + i;
  /* by doubling, as nt_new_node grows it: a resize to the count each time
     copied the whole table per clone */
  if (base + sn > nt->node_cap) {
    int nc = nt->node_cap > (base + sn) / 2 ? nt->node_cap * 2 : base + sn;
    SpNode *grown = realloc(nt->nodes, sizeof(SpNode) * (size_t)nc);
    if (!grown) nt_oom();
    nt->nodes = grown;
    nt->node_cap = nc;
  }
  memset(&nt->nodes[base], 0, sizeof(SpNode) * (size_t)sn);
  for (int i = 0; i < sn; i++) {
    SpNode *src = &nt->nodes[list[i]];
    SpNode *dst = &nt->nodes[base + i];
    if (src->type)    dst->type    = dup_n(src->type, strlen(src->type));
    if (src->content) dst->content = dup_n(src->content, strlen(src->content));
    dst->ns = dst->cs = src->ns;
    if (src->ns) { dst->s = nt_alloc(sizeof(SpStrField) * (size_t)src->ns);
      for (int j = 0; j < src->ns; j++) {
        dst->s[j].key = dup_n(src->s[j].key, strlen(src->s[j].key)); dst->s[j].disc = src->s[j].disc;
        dst->s[j].val_len = src->s[j].val_len;
        dst->s[j].val = dup_n(src->s[j].val, src->s[j].val_len);
      } }
    dst->ni = dst->ci = src->ni;
    if (src->ni) { dst->i = nt_alloc(sizeof(SpIntField) * (size_t)src->ni);
      for (int j = 0; j < src->ni; j++) { dst->i[j].key = dup_n(src->i[j].key, strlen(src->i[j].key)); dst->i[j].disc = src->i[j].disc; dst->i[j].val = src->i[j].val; } }
    dst->nr = dst->cr = src->nr;
    if (src->nr) { dst->r = nt_alloc(sizeof(SpRefField) * (size_t)src->nr);
      for (int j = 0; j < src->nr; j++) {
        dst->r[j].key = dup_n(src->r[j].key, strlen(src->r[j].key));
        dst->r[j].disc = src->r[j].disc;
        dst->r[j].ref = nt_clone_map(nt, base, src->r[j].ref);
      } }
    dst->na = dst->ca = src->na;
    if (src->na) { dst->a = nt_alloc(sizeof(SpArrField) * (size_t)src->na);
      for (int j = 0; j < src->na; j++) {
        dst->a[j].key = dup_n(src->a[j].key, strlen(src->a[j].key));
        dst->a[j].disc = src->a[j].disc;
        dst->a[j].n = src->a[j].n;
        dst->a[j].ids = nt_alloc(sizeof(int) * (size_t)src->a[j].n);
        for (int k = 0; k < src->a[j].n; k++) dst->a[j].ids[k] = nt_clone_map(nt, base, src->a[j].ids[k]);
      } }
  }
  nt->count = base + sn;
  /* each copy is the appending pass's next one of its source */
  nt_aux(nt, nt->count - 1);
  nt_aux_reset(nt, base, nt->count);
  if (nt->orig_lo) for (int i = 0; i < sn; i++) nt_set_origin(nt, base + i, list[i], 'c', 0);
  nt->version++; nt->edits++; nt->clone_version++;
  free(list);
  return base;   /* the root is the first node collected */
}

/* ---- synthetic node construction ---- */

static void nki_retyped(NodeTable *nt, int id, NodeKind was);
static void nki_drop(NodeTable *nt);

/* Retype an existing node in place. A desugar that rewrites one construct
   into another (a CallOperatorWriteNode into the writer CallNode Ruby means)
   has to keep the id: the parent's ref names it. */
void nt_node_set_type(NodeTable *nt, int id, const char *type) {
  if (id < 0 || id >= nt->count || !type) return;
  NodeKind was = nt_kind(nt, id);
  SpNode *nd = &nt->nodes[id];
  node_set_type(nd, type, strlen(type));
  nd->kind = 0;          /* the cached NodeKind is stale now */
  nt->version++; nt->edits++;
  nki_retyped(nt, id, was);
}
int nt_new_node(NodeTable *nt, const char *type) {
  if (nt->count >= nt->node_cap) {
    int nc = nt->node_cap ? nt->node_cap * 2 : 8;
    if (nc <= nt->count) nc = nt->count + 1;
    SpNode *grown = realloc(nt->nodes, sizeof(SpNode) * (size_t)nc);
    if (!grown) nt_oom();
    nt->nodes = grown;
    nt->node_cap = nc;
  }
  int id = nt->count++;
  nt->version++; nt->edits++;
  nt_aux_reset(nt, id, id + 1);
  SpNode *nd = &nt->nodes[id];
  memset(nd, 0, sizeof(SpNode));
  if (type) node_set_type(nd, type, strlen(type));
  return id;
}
int nt_appended_only_local_reads(const NodeTable *nt, int from) {
  if (from < 0) return 0;
  for (int n = from; n < nt->count; n++)
    if (nt_kind(nt, n) != NK_LocalVariableReadNode) return 0;
  return 1;
}
int nt_new_local_read(NodeTable *nt, const char *name) {
  unsigned v = nt->version;
  int id = nt_new_node(nt, "LocalVariableReadNode");
  nt_node_set_str(nt, id, "name", name);
  nt->read_version += nt->version - v;
  return id;
}
NtStamp nt_stamp(const NodeTable *nt) {
  NtStamp s = { nt->version, nt->read_version, nt->rename_version };
  return s;
}
int nt_stamp_holds(const NodeTable *nt, NtStamp s, int renames) {
  unsigned counted = nt->read_version - s.reads;
  if (renames) counted += nt->rename_version - s.renames;
  return nt->version - s.version == counted;
}
int nt_renames_spare(const NodeTable *nt, NtStamp s, int (*reads)(const char *name)) {
  if (!nt_stamp_holds(nt, s, 1) || nt->rename_version - s.renames > NT_RENAME_LOG) return 0;
  for (unsigned r = s.renames; r != nt->rename_version; r++) {
    const NtRename *e = &nt->rename_log[r % NT_RENAME_LOG];
    if (e->cut || reads(e->from) || reads(e->to)) return 0;
  }
  return 1;
}
int nt_new_int(NodeTable *nt, long long v) {
  int n = nt_new_node(nt, "IntegerNode");
  nt_node_set_int(nt, n, "value", v);
  return n;
}

/* Turn an existing node INTO a node of another type, dropping every field it
   carried. A desugar that rewrites one construct as another needs the result
   to BE the new node rather than a wrapper around it, because consumers
   pattern-match on the node type -- `p []` reads its argument's own kind to
   build the empty literal, and a wrapper hides it. The id is stable, so
   parents and the scope map keep pointing at the right place. */
void nt_node_reset(NodeTable *nt, int id, const char *type) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  NodeKind was = nt_kind(nt, id);
  nt->version++; nt->edits++;
  free(nd->type); nd->type = NULL;
  free(nd->content); nd->content = NULL;
  for (int j = 0; j < nd->ns; j++) { free(nd->s[j].key); free(nd->s[j].val); }
  for (int j = 0; j < nd->ni; j++) free(nd->i[j].key);
  for (int j = 0; j < nd->nr; j++) free(nd->r[j].key);
  for (int j = 0; j < nd->na; j++) { free(nd->a[j].key); free(nd->a[j].ids); }
  nd->ns = nd->ni = nd->nr = nd->na = 0;
  nd->kind = 0;
  if (type) node_set_type(nd, type, strlen(type));
  nki_retyped(nt, id, was);
}

void nt_node_set_str(NodeTable *nt, int id, const char *key, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  nt->version++; nt->edits++;
  size_t klen = strlen(key), vlen = strlen(val);
  if (vlen == 16 && !strcmp(val, "__cext_extension") && !strcmp(key, "name")) nt->has_cext_decls = 1;
  for (int j = 0; j < nd->ns; j++)
    if (sp_streq(nd->s[j].key, key)) {
      free(nd->s[j].val); nd->s[j].val = dup_n(val, vlen); nd->s[j].val_len = vlen; return;
    }
  node_add_str(nd, key, klen, dup_n(val, vlen), vlen);
}

/* rename_log's entry for the rename about to be counted */
static void nt_rename_note(NodeTable *nt, const char *from, const char *to) {
  NtRename *e = &nt->rename_log[nt->rename_version % NT_RENAME_LOG];
  size_t fl = from ? strlen(from) : 0, tl = to ? strlen(to) : 0;
  e->cut = fl >= sizeof e->from || tl >= sizeof e->to;
  if (e->cut) return;
  memcpy(e->from, from ? from : "", fl + 1);
  memcpy(e->to, to ? to : "", tl + 1);
}

/* the replaced string is answered, not freed: a caller up the stack may hold
   the pointer nt_str answered for it (node_table.h) */
char *nt_node_swap_str(NodeTable *nt, int id, const char *key, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return NULL;
  nt->version++;
  if (nt_kind(nt, id) == NK_CallNode && sp_streq(key, "name")) {
    nt_rename_note(nt, nt_str(nt, id, "name"), val);
    nt->rename_version++;
  }
  size_t vlen = strlen(val);
  for (int j = 0; j < nd->ns; j++)
    if (sp_streq(nd->s[j].key, key)) {
      char *held = nd->s[j].val;
      nd->s[j].val = dup_n(val, vlen); nd->s[j].val_len = vlen;
      return held;
    }
  node_add_str(nd, key, strlen(key), dup_n(val, vlen), vlen);
  return NULL;
}

void nt_node_restore_str(NodeTable *nt, int id, const char *key, char *held) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) { free(held); return; }
  nt->version++;
  if (nt_kind(nt, id) == NK_CallNode && sp_streq(key, "name")) {
    nt_rename_note(nt, nt_str(nt, id, "name"), held);
    nt->rename_version++;
  }
  for (int j = 0; j < nd->ns; j++)
    if (sp_streq(nd->s[j].key, key)) {
      free(nd->s[j].val);
      if (held) { nd->s[j].val = held; nd->s[j].val_len = strlen(held); return; }
      /* the swap added the key: the node had none */
      free(nd->s[j].key);
      memmove(&nd->s[j], &nd->s[j + 1], (size_t)(nd->ns - j - 1) * sizeof nd->s[0]);
      nd->ns--;
      return;
    }
  if (held) node_add_str(nd, key, strlen(key), held, strlen(held));
}

void nt_node_set_int(NodeTable *nt, int id, const char *key, long long val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  for (int j = 0; j < nd->ni; j++)
    if (sp_streq(nd->i[j].key, key)) {
      if (nd->i[j].val != val) { nd->i[j].val = val; nt->edits++; }
      return;
    }
  nt->edits++;
  node_add_int(nd, key, strlen(key), val);
}

void nt_node_set_ref(NodeTable *nt, int id, const char *key, int child) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  for (int j = 0; j < nd->nr; j++)
    if (sp_streq(nd->r[j].key, key)) {
      if (nd->r[j].ref != child) { nd->r[j].ref = child; nt->edits++; }
      nt_place(nt, id, child, 'r', j);
      return;
    }
  nt->edits++;
  node_add_ref(nd, key, strlen(key), child);
  nt_place(nt, id, child, 'r', nd->nr - 1);
}

void nt_node_set_arr(NodeTable *nt, int id, const char *key, const int *ids, int n) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  nt->edits++;
  int *copy = NULL;
  if (n > 0) {
    copy = nt_alloc(sizeof(int) * (size_t)n);
    memcpy(copy, ids, sizeof(int) * (size_t)n);
  }
  int j = 0;
  while (j < nd->na && !sp_streq(nd->a[j].key, key)) j++;
  if (j < nd->na) { free(nd->a[j].ids); nd->a[j].ids = copy; nd->a[j].n = n; }
  else node_add_arr(nd, key, strlen(key), copy, n);
  for (int e = 0; e < n; e++) nt_place(nt, id, copy[e], 'a', j);
}

void nt_add_autoload(NodeTable *nt, int node, int file, const char *name, const char *path) {
  if (nt->nautoloads == nt->cautoloads) {
    nt->cautoloads = nt->cautoloads ? 2 * nt->cautoloads : 8;
    nt->autoloads = realloc(nt->autoloads, sizeof *nt->autoloads * (size_t)nt->cautoloads);
    if (!nt->autoloads) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  nt->autoloads[nt->nautoloads++] = (AutoloadRegistration){ node, file, 0, 0, dup_n(name, strlen(name)), path ? dup_n(path, strlen(path)) : NULL };
}

void nt_free(NodeTable *nt) {
  if (!nt) return;
  for (int k = 0; k < nt->count; k++) {
    SpNode *nd = &nt->nodes[k];
    free(nd->type);
    free(nd->content);
    for (int j = 0; j < nd->ns; j++) { free(nd->s[j].key); free(nd->s[j].val); }
    for (int j = 0; j < nd->ni; j++) free(nd->i[j].key);
    for (int j = 0; j < nd->nr; j++) free(nd->r[j].key);
    for (int j = 0; j < nd->na; j++) { free(nd->a[j].key); free(nd->a[j].ids); }
    free(nd->s); free(nd->i); free(nd->r); free(nd->a);
  }
  free(nt->nodes);
  nki_drop(nt);
  for (int i = 0; i < nt->nkind_retired; i++) free(nt->kind_retired[i]);
  free(nt->kind_retired);
  free(nt->kind_moved);
  free(nt->source_file);
  for (int k = 0; k < nt->nfiles; k++) free(nt->files[k]);
  free(nt->files);
  free(nt->file_defers);
  free(nt->file_tier);
  free(nt->file_roots);
  for (int i = 0; i < nt->nautoloads; i++) { free(nt->autoloads[i].name); free(nt->autoloads[i].path); }
  free(nt->autoloads);
  free(nt->aux);
  free(nt->counts);
  if (nt->holders) { free(nt->holders->par); free(nt->holders->stamp); free(nt->holders); }
  free(nt);
}

/* ---- accessors ---- */

void nt_put_file_defers(NodeTable *nt, int fid) {
  if (fid < 0) return;
  if (fid >= nt->nfile_defers) {
    unsigned char *grown = realloc(nt->file_defers, (size_t)fid + 1);
    if (!grown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = nt->nfile_defers; k <= fid; k++) grown[k] = 0;
    nt->file_defers = grown;
    nt->nfile_defers = fid + 1;
  }
  nt->file_defers[fid] = 1;
}
int nt_file_defers(const NodeTable *nt, int fid) {
  return nt && nt->file_defers && fid >= 0 && fid < nt->nfile_defers && nt->file_defers[fid];
}
void nt_put_file_tier(NodeTable *nt, int fid, int tier) {
  if (fid < 0) return;
  if (fid >= nt->nfile_tier) {
    unsigned char *grown = realloc(nt->file_tier, (size_t)fid + 1);
    if (!grown) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
    for (int k = nt->nfile_tier; k <= fid; k++) grown[k] = SP_LIB_NONE;
    nt->file_tier = grown;
    nt->nfile_tier = fid + 1;
  }
  nt->file_tier[fid] = (unsigned char)tier;
}
int nt_file_tier(const NodeTable *nt, int fid) {
  return nt && nt->file_tier && fid >= 0 && fid < nt->nfile_tier ? nt->file_tier[fid] : SP_LIB_NONE;
}
const char *nt_file_path(const NodeTable *nt, int fid) {
  if (!nt || fid < 0 || fid >= nt->nfiles) return NULL;
  return nt->files[fid];
}

static const SpNode *node_at(const NodeTable *nt, int id) {
  if (id < 0 || id >= nt->count) return NULL;
  return &nt->nodes[id];
}

#ifdef SP_WORK_COUNT
unsigned long long g_nt_work = 0;
#endif
const char *nt_type(const NodeTable *nt, int id) {
  NT_WORK();
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->type : NULL;
}

/* Sorted node-type names; index i corresponds to enum value (i + 1) because
   NK_NONE = 0 precedes the X-macro entries. The X-macro list is alphabetically
   sorted, so this array is sorted and bsearch-able. */
static const char *const sp_kind_names[] = {
#define X(n) #n,
  SP_NODE_KINDS(X)
#undef X
};

static int sp_kind_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, *(const char *const *)b);
}

NodeKind nt_kind_resolve(const NodeTable *nt, int id) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return NK_NONE;
  if (nd->kind) return (NodeKind)(nd->kind - 1);  /* cached (stored as kind+1) */
  NodeKind k = NK_NONE;
  if (nd->type) {
    const char *const *hit = (const char *const *)bsearch(
        nd->type, sp_kind_names, sizeof sp_kind_names / sizeof sp_kind_names[0],
        sizeof sp_kind_names[0], sp_kind_cmp);
    if (hit) k = (NodeKind)((hit - sp_kind_names) + 1);
  }
  nd->kind = (int)k + 1;  /* cache; 0 stays "uncomputed" */
  return k;
}

/* The node ids of each kind, ascending (matching a forward `for id` scan),
   kept in the table (kind_ids): the analyze passes each scan the whole
   table filtering by node kind, dozens of times per fixpoint iteration, and
   this lets a pass walk only the nodes of the kind it cares about. Changes
   reach the index at the next ask, as one pass each:
   - a node appended since the last ask joins its kind's ids after the rest
     (desugar passes, the scope walk's clones), so they stay ascending;
   - a node retyped in place (nki_retyped) is noted, and the ask moves every
     noted node at once: each kind they leave or join is merged anew, so a
     pass that retypes nodes while it loops over a kind costs one merge per
     ask, not one per node;
   - a swap or a renumbering, which moves ids between kinds wholesale, and a
     table cut back below the nodes indexed rebuild the index.
   An id array an open NT_FOREACH_KIND loop may be reading is retired, not
   freed, until the table's last open loop closes: the loop reads the ids it
   began with. nt_kind is O(1) (cached per node). */
/* n ids copied or merged, as the scale test counts work (NT_WORK): a
   change that moved a kind's ids per retyped node shows as a ratio far
   above the program's growth */
static void nki_work(int n) {
#ifdef SP_WORK_COUNT
  g_nt_work += (unsigned long long)n;
#else
  (void)n;
#endif
}
static void nki_release(NodeTable *nt, int *p) {
  if (!p) return;
  if (nt->kind_live == 0) { free(p); return; }
  if (nt->nkind_retired == nt->ckind_retired) {
    int nc = nt->ckind_retired ? nt->ckind_retired * 2 : 8;
    int **np = (int **)realloc(nt->kind_retired, (size_t)nc * sizeof(int *));
    if (!np) return;   /* kept alive by the leak rather than freed under a reader */
    nt->kind_retired = np; nt->ckind_retired = nc;
  }
  nt->kind_retired[nt->nkind_retired++] = p;
}
void nt_kind_iter_open(const NodeTable *nt) { ((NodeTable *)nt)->kind_live++; }
void nt_kind_iter_close(NtKindIter *it) {
  NodeTable *nt = (NodeTable *)it->nt;
  if (--nt->kind_live > 0) return;
  for (int i = 0; i < nt->nkind_retired; i++) free(nt->kind_retired[i]);
  nt->nkind_retired = 0;
}
static void nki_drop(NodeTable *nt) {
  nt->nkind_moved = 0;
  if (!nt->kind_ids) return;
  for (int k = 0; k < NK__COUNT; k++) nki_release(nt, nt->kind_ids[k]);
  free(nt->kind_ids); free(nt->kind_n); free(nt->kind_cap);
  nt->kind_ids = NULL; nt->kind_n = nt->kind_cap = NULL;
  nt->kind_count = 0;
}
/* Room for `need` of kind k's ids. A grown array is a copy: an open loop
   reads the old one, and an append in place is no harm to it, as it reads
   only the ids it began with. */
static void nki_grow(NodeTable *nt, int k, int need) {
  if (need <= nt->kind_cap[k]) return;
  int nc = nt->kind_cap[k] ? nt->kind_cap[k] : 8;
  while (nc < need) nc *= 2;
  int *p = nt_alloc(sizeof(int) * (size_t)nc);
  if (nt->kind_n[k]) memcpy(p, nt->kind_ids[k], sizeof(int) * (size_t)nt->kind_n[k]);
  nki_work(nt->kind_n[k]);
  nki_release(nt, nt->kind_ids[k]);
  nt->kind_ids[k] = p;
  nt->kind_cap[k] = nc;
}
/* Node id was retyped in place (nt_node_set_type, nt_node_reset) and may
   be of another kind now. The node count, which is what tells an ask that
   nodes were added, did not change: a desugar that folds
   `RUBY_ENGINE == "spinel"` into a TrueNode appends nothing, and left
   listed as a CallNode, a later walk of the CallNodes was handed the
   TrueNode and read its absent name. Noted with the kind it had; the next
   ask moves it. */
static void nki_retyped(NodeTable *nt, int id, NodeKind was) {
  if (!nt->kind_ids || id >= nt->kind_count || nt_kind(nt, id) == was) return;
  if (nt->nkind_moved + 2 > nt->ckind_moved) {
    int nc = nt->ckind_moved ? nt->ckind_moved * 2 : 64;
    int *np = (int *)realloc(nt->kind_moved, (size_t)nc * sizeof(int));
    if (!np) nt_oom();
    nt->kind_moved = np; nt->ckind_moved = nc;
  }
  nt->kind_moved[nt->nkind_moved++] = id;
  nt->kind_moved[nt->nkind_moved++] = (int)was;
}
/* One noted retype: the node, the kind it is listed under, the kind it has
   now, and its place in the notes (a node retyped twice is listed under the
   kind its first note names) */
typedef struct { int id, was, now, seq; } NkiMove;
static int nki_move_cmp(const void *a, const void *b) {
  const NkiMove *x = a, *y = b;
  if (x->id != y->id) return x->id < y->id ? -1 : 1;
  return x->seq < y->seq ? -1 : x->seq > y->seq;
}
/* Every noted node from the kind it is listed under to the one it has:
   each kind a node leaves or joins is merged into a new array, its ids and
   the joiners in order, the leavers left out */
static void nki_apply_moves(NodeTable *nt) {
  int m = nt->nkind_moved / 2;
  nt->nkind_moved = 0;
  NkiMove *mv = nt_alloc(sizeof(NkiMove) * (size_t)m);
  for (int i = 0; i < m; i++)
    mv[i] = (NkiMove){ nt->kind_moved[2 * i], nt->kind_moved[2 * i + 1], NK_NONE, i };
  qsort(mv, (size_t)m, sizeof(NkiMove), nki_move_cmp);
  unsigned char *hit = calloc((size_t)NK__COUNT, 1);
  if (!hit) nt_oom();
  int u = 0, last = -1;
  for (int i = 0; i < m; i++) {
    if (mv[i].id == last) continue;   /* a later note of the same node */
    last = mv[i].id;
    mv[i].now = (int)nt_kind(nt, mv[i].id);
    if (mv[i].now == mv[i].was) continue;              /* retyped back */
    hit[mv[i].was] = hit[mv[i].now] = 1;
    mv[u++] = mv[i];
  }
  for (int k = 0; k < NK__COUNT; k++) {
    if (!hit[k]) continue;
    int *old = nt->kind_ids[k], on = nt->kind_n[k], i = 0, n = 0, cap = on + u > 8 ? on + u : 8;
    int *p = nt_alloc(sizeof(int) * (size_t)cap);
    for (int j = 0; j < u; j++) {
      if (mv[j].was != k && mv[j].now != k) continue;
      while (i < on && old[i] < mv[j].id) p[n++] = old[i++];
      if (mv[j].was == k) { if (i < on && old[i] == mv[j].id) i++; }
      else p[n++] = mv[j].id;
    }
    while (i < on) p[n++] = old[i++];
    nki_work(n);
    nki_release(nt, old);
    nt->kind_ids[k] = p; nt->kind_n[k] = n; nt->kind_cap[k] = cap;
  }
  free(hit);
  free(mv);
}
/* The index brought up to the table: built afresh when there is none or
   the table was cut back, the noted retypes moved, the nodes not indexed
   yet added to their kinds' ids */
static void nki_refresh(NodeTable *nt) {
  if (nt->kind_ids && nt->kind_count > nt->count) nki_drop(nt);
  if (!nt->kind_ids) {
    nt->kind_ids = (int **)calloc((size_t)NK__COUNT, sizeof(int *));
    nt->kind_n = (int *)calloc((size_t)NK__COUNT, sizeof(int));
    nt->kind_cap = (int *)calloc((size_t)NK__COUNT, sizeof(int));
    if (!nt->kind_ids || !nt->kind_n || !nt->kind_cap) nt_oom();
    nt->kind_count = 0;
  }
  if (nt->nkind_moved) nki_apply_moves(nt);
  for (int id = nt->kind_count; id < nt->count; id++) {
    int k = (int)nt_kind(nt, id);
    nki_grow(nt, k, nt->kind_n[k] + 1);
    nt->kind_ids[k][nt->kind_n[k]++] = id;
  }
  nt->kind_count = nt->count;
}
/* Exchange the contents of nodes a and b: what was node a is now numbered b,
   and the reverse. A desugar that must put a new node AHEAD of an existing
   one in node order (passes that fold by ascending id read it first) builds
   it at the end and swaps it into the earlier slot; the caller re-points the
   references to both. */
void nt_swap_nodes(NodeTable *nt, int a, int b) {
  if (a < 0 || b < 0 || a >= nt->count || b >= nt->count || a == b) return;
  SpNode t = nt->nodes[a];
  nt->nodes[a] = nt->nodes[b];
  nt->nodes[b] = t;
  nt->version++; nt->edits++;
  nki_drop(nt);   /* the kind index lists ids by kind: rebuild it */
}

/* Return the ids of nodes of kind `k` (ascending), writing the count to *count.
   The returned pointer is valid until the table next changes, and while an
   NT_FOREACH_KIND loop is open. */
const int *nt_nodes_of_kind(const NodeTable *nt, NodeKind k, int *count) {
  if (k < 0 || k >= NK__COUNT) { *count = 0; return NULL; }
  NodeTable *w = (NodeTable *)nt;   /* the index is the table's cache */
  if (!w->kind_ids || w->kind_count != w->count || w->nkind_moved) nki_refresh(w);
  *count = w->kind_n[k];
  return w->kind_ids[k];
}


/* Renumber the table's nodes: order[k] is the id node k had. Every field
   that holds a node id moves with it: the refs, the arrays, the root and
   the two int fields the parse writes that hold a node id: node_bi (the
   builtin node a builtins/ node counts from, plus one) and a root's load_by
   (the LoadNode that first runs its file). */
void nt_renumber(NodeTable *nt, const int *order) {
  int n = nt->count;
  int *perm = malloc(sizeof(int) * (size_t)(n ? n : 1));
  SpNode *nodes = malloc(sizeof(SpNode) * (size_t)(nt->node_cap ? nt->node_cap : 1));
  if (!perm || !nodes) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int k = 0; k < n; k++) perm[order[k]] = k;
  for (int k = 0; k < n; k++) nodes[k] = nt->nodes[order[k]];
  for (int k = n; k < nt->node_cap; k++) nodes[k] = nt->nodes[k];
  for (int k = 0; k < n; k++) {
    SpNode *nd = &nodes[k];
    for (int j = 0; j < nd->nr; j++) if (nd->r[j].ref >= 0 && nd->r[j].ref < n) nd->r[j].ref = perm[nd->r[j].ref];
    for (int j = 0; j < nd->na; j++)
      for (int q = 0; q < nd->a[j].n; q++) if (nd->a[j].ids[q] >= 0 && nd->a[j].ids[q] < n) nd->a[j].ids[q] = perm[nd->a[j].ids[q]];
    for (int j = 0; j < nd->ni; j++) {
      if (sp_streq(nd->i[j].key, "node_bi") && nd->i[j].val > 0 && nd->i[j].val <= n) nd->i[j].val = perm[nd->i[j].val - 1] + 1;
      if (sp_streq(nd->i[j].key, "load_by") && nd->i[j].val >= 0 && nd->i[j].val < n) nd->i[j].val = perm[nd->i[j].val];
    }
  }
  free(nt->nodes);
  nt->nodes = nodes;
  if (nt->root_id >= 0 && nt->root_id < n) nt->root_id = perm[nt->root_id];
  for (int f = 0; f < nt->nfile_roots; f++)
    if (nt->file_roots[f] >= 0 && nt->file_roots[f] < n) nt->file_roots[f] = perm[nt->file_roots[f]];
  for (int i = 0; i < nt->nautoloads; i++) nt->autoloads[i].node = perm[nt->autoloads[i].node];
  free(perm);
  nt->version++; nt->edits++;
  nki_drop(nt);   /* the ids of each kind moved */
  free(nt->aux); nt->aux = NULL; nt->aux_cap = 0;   /* and so did each one's entry */
}

int nt_set_str(NodeTable *nt, int id, const char *key, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return 0;
  nt->version++; nt->edits++;
  size_t vlen = strlen(val);
  for (int j = 0; j < nd->ns; j++) {
    if (sp_streq(nd->s[j].key, key)) {
      free(nd->s[j].val);
      nd->s[j].val = dup_n(val, vlen);
      nd->s[j].val_len = vlen;
      return 1;
    }
  }
  return 0;
}





/* A condition's predicate as a constant: 1 always truthy, 0 always falsy,
   -1 a value known only when it runs. Only a literal folds: true and the
   other non-nil literals, false and nil; parentheses around one statement
   are the statement's, `()` is nil, and `(a; b)` is not folded. */
int nt_pred_const(const NodeTable *nt, int pred) {
  int p = pred >= 0 ? nt_paren_single(nt, pred) : -1;
  if (p < 0) return -1;
  switch (nt_kind(nt, p)) {
    case NK_ParenthesesNode: {   /* not one statement: none, or several */
      int body = nt_ref(nt, p, "body"), n = 0;
      if (body >= 0) nt_arr(nt, body, "body", &n);
      return n == 0 ? 0 : -1;
    }
    case NK_FalseNode: case NK_NilNode: return 0;
    case NK_TrueNode: case NK_IntegerNode: case NK_FloatNode: case NK_StringNode: case NK_SymbolNode:
    case NK_ArrayNode: case NK_HashNode: case NK_RegularExpressionNode:
      return 1;
    default: return -1;
  }
}

const char *nt_content(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->content : NULL;
}

int nt_num_refs(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->nr : 0;
}
int nt_ref_at(const NodeTable *nt, int id, int i) {
  const SpNode *nd = node_at(nt, id);
  if (!nd || i < 0 || i >= nd->nr) return -1;
  return nd->r[i].ref;
}
int nt_num_arrs(const NodeTable *nt, int id) {
  const SpNode *nd = node_at(nt, id);
  return nd ? nd->na : 0;
}
const int *nt_arr_at(const NodeTable *nt, int id, int i, int *out_n) {
  *out_n = 0;
  const SpNode *nd = node_at(nt, id);
  if (!nd || i < 0 || i >= nd->na) return NULL;
  *out_n = nd->a[i].n;
  return nd->a[i].ids;
}

int nt_call_args_plain(const NodeTable *nt, int id) {
  int args = nt_ref(nt, id, "arguments");
  if (args < 0) return 1;
  int argc = 0;
  const int *argv = nt_arr(nt, args, "arguments", &argc);
  return !nt_argv_has(nt, argv, argc, NT_ARG_SPLAT | NT_ARG_BLOCK);
}

/* Copy the subtree at `root` of `src` into `dst` (every field, children
   recursively); the new root's id in dst, or -1. A desugar parses a snippet
   into its own table and grafts the result into the program's. */
int nt_import_subtree(NodeTable *dst, const NodeTable *src, int root) {
  if (!dst || !src || root < 0 || root >= src->count) return -1;
  dst->has_cext_decls |= src->has_cext_decls;
  const SpNode *sn = &src->nodes[root];
  int id = nt_new_node(dst, sn->type ? sn->type : "");
  if (id < 0) return -1;
  if (sn->content) { SpNode *dn = &dst->nodes[id]; dn->content = dup_n(sn->content, strlen(sn->content)); }
  for (int j = 0; j < sn->ns; j++) {
    char *v = dup_n(sn->s[j].val, sn->s[j].val_len);
    node_add_str(&dst->nodes[id], sn->s[j].key, strlen(sn->s[j].key), v, sn->s[j].val_len);
  }
  for (int j = 0; j < sn->ni; j++) nt_node_set_int(dst, id, sn->i[j].key, sn->i[j].val);
  for (int j = 0; j < sn->nr; j++) {
    int ch = sn->r[j].ref >= 0 ? nt_import_subtree(dst, src, sn->r[j].ref) : -1;
    nt_node_set_ref(dst, id, sn->r[j].key, ch);
  }
  for (int j = 0; j < sn->na; j++) {
    int n = sn->a[j].n;
    int *ids = nt_alloc(sizeof(int) * (size_t)n);
    for (int q = 0; q < n; q++) ids[q] = sn->a[j].ids[q] >= 0 ? nt_import_subtree(dst, src, sn->a[j].ids[q]) : -1;
    nt_node_set_arr(dst, id, sn->a[j].key, ids, n);
    free(ids);
  }
  return id;
}
