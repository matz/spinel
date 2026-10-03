#include "node_table.h"

#include <stdlib.h>
#include <string.h>

/* ---- string unescape (mirrors node_table_loader.rb#unescape_str) ---- */

static char *unescape_dup(const char *s, size_t len, size_t *out_len) {
  char *out = malloc(len + 1);
  if (!out) { if (out_len) *out_len = 0; return NULL; }
  size_t o = 0;
  size_t i = 0;
  while (i < len) {
    char ch = s[i];
    if (ch == '%' && i + 2 < len) {
      char a = s[i + 1], b = s[i + 2];
      char dec = 0;
      int known = 1;
      if (a == '0' && b == 'A') dec = '\n';
      else if (a == '0' && b == 'D') dec = '\r';
      else if (a == '0' && b == '9') dec = '\t';
      else if (a == '2' && b == '0') dec = ' ';
      else if (a == '2' && b == '5') dec = '%';
      else if (a == '0' && b == '0') dec = '\0';
      else known = 0;
      if (known) {
        out[o++] = dec;
        i += 3;
        continue;
      }
      /* unknown escape: keep '%' literally and advance one */
      out[o++] = ch;
      i++;
    }
    else {
      out[o++] = ch;
      i++;
    }
  }
  out[o] = '\0';
  if (out_len) *out_len = o;
  return out;
}

/* ---- field append helpers ---- */

static const SpNode *node_at(const NodeTable *nt, int id);

static void ensure_cap(void **buf, int *cap, int need, size_t elem) {
  if (need <= *cap) return;
  int nc = *cap ? *cap * 2 : 4;
  if (nc < need) nc = need;
  *buf = realloc(*buf, (size_t)nc * elem);
  *cap = nc;
}

static char *dup_n(const char *s, size_t n) {
  char *p = malloc(n + 1);
  if (!p) return NULL;
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

/* ---- line splitting ----
 * Splits a line (sans newline) into tag/id/field/value where value is
 * everything after the third space (possibly empty). Returns the number
 * of leading single-token parts found (1..3) and sets *val_off to the
 * byte offset of the value remainder (or len if none). Token boundaries
 * are reported via the parts[] offset/len pairs. */
typedef struct { const char *p; size_t len; } Tok;

static int split_line(const char *line, size_t len, Tok parts[3], const char **val, size_t *val_len) {
  int np = 0;
  size_t i = 0;
  *val = NULL;
  *val_len = 0;
  while (np < 3 && i < len) {
    size_t start = i;
    while (i < len && line[i] != ' ') i++;
    parts[np].p = line + start;
    parts[np].len = i - start;
    np++;
    if (i < len) i++; /* skip the space */
    if (np == 3) {
      /* remainder (after the 3rd space) is the value */
      *val = line + i;
      *val_len = len - i;
      break;
    }
  }
  return np;
}

/* The digits gather unsigned: the smallest Integer's are one past what a
   long long holds until its sign is applied. */
static long long parse_ll(const char *s, size_t len) {
  int neg = 0;
  unsigned long long v = 0;
  size_t i = 0;
  if (i < len && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
  for (; i < len; i++) {
    if (s[i] < '0' || s[i] > '9') break;
    v = (v * 10) + (unsigned long long)(s[i] - '0');
  }
  return (long long)(neg ? 0ULL - v : v);
}

/* Parse a comma-separated id list into a malloc'd int array. */
static int *parse_id_list(const char *s, size_t len, int *out_n) {
  if (len == 0) { *out_n = 0; return NULL; }
  int cap = 8, n = 0;
  int *ids = malloc(sizeof(int) * cap);
  size_t i = 0;
  while (i < len) {
    size_t start = i;
    while (i < len && s[i] != ',') i++;
    if (n >= cap) { cap *= 2; ids = realloc(ids, sizeof(int) * cap); }
    ids[n++] = (int)parse_ll(s + start, i - start);
    if (i < len) i++; /* skip comma */
  }
  *out_n = n;
  return ids;
}

static int tok_eq(Tok t, const char *lit) {
  size_t l = strlen(lit);
  return t.len == l && memcmp(t.p, lit, l) == 0;
}

NodeTable *nt_load_text(const char *text) {
  NodeTable *nt = calloc(1, sizeof(NodeTable));
  if (!nt) return NULL;
  nt->root_id = 0;

  size_t tlen = strlen(text);

  /* Pass 1: find max node id, ROOT, SOURCE_FILE. */
  int max_id = 0;
  {
    size_t i = 0;
    while (i < tlen) {
      size_t ls = i;
      while (i < tlen && text[i] != '\n') i++;
      size_t llen = i - ls;
      if (i < tlen) i++;
      if (llen == 0) continue;
      const char *line = text + ls;
      Tok parts[3];
      const char *val; size_t vlen;
      int np = split_line(line, llen, parts, &val, &vlen);
      if (np >= 2) {
        if (tok_eq(parts[0], "ROOT")) {
          nt->root_id = (int)parse_ll(parts[1].p, parts[1].len);
        }
        else if (tok_eq(parts[0], "SOURCE_FILE")) {
          /* path is parts[1] plus possibly value; emitted as one token */
          nt->source_file = unescape_dup(parts[1].p, parts[1].len, NULL);
        }
        else if (tok_eq(parts[0], "FILE") && np >= 3) {
          /* FILE <id> <escaped-path>: the multi-file source map for #line.
             Take the path as everything after "FILE <id> " so embedded
             spaces (if any survive escaping) are preserved. */
          int fid = (int)parse_ll(parts[1].p, parts[1].len);
          if (fid >= 0) {
            if (fid >= nt->nfiles) {
              int newn = fid + 1;
              nt->files = realloc(nt->files, sizeof(char *) * (size_t)newn);
              for (int k = nt->nfiles; k < newn; k++) nt->files[k] = NULL;
              nt->nfiles = newn;
            }
            const char *pstart = parts[2].p;
            size_t plen = (size_t)((line + llen) - pstart);
            free(nt->files[fid]);
            nt->files[fid] = unescape_dup(pstart, plen, NULL);
          }
        }
        else {
          /* N/S/I/F/R/A all carry a node id in parts[1] */
          char c = parts[0].p[0];
          if (parts[0].len == 1 && (c == 'N' || c == 'S' || c == 'I' ||
                                    c == 'F' || c == 'R' || c == 'A')) {
            int nid = (int)parse_ll(parts[1].p, parts[1].len);
            if (nid > max_id) max_id = nid;
          }
        }
      }
    }
  }

  nt->count = max_id + 1;
  nt->node_cap = nt->count;
  nt->nodes = calloc((size_t)nt->count, sizeof(SpNode));
  if (!nt->nodes) { free(nt); return NULL; }

  /* Pass 2: populate fields. */
  {
    size_t i = 0;
    while (i < tlen) {
      size_t ls = i;
      while (i < tlen && text[i] != '\n') i++;
      size_t llen = i - ls;
      if (i < tlen) i++;
      if (llen == 0) continue;
      const char *line = text + ls;
      Tok parts[3];
      const char *val; size_t vlen;
      int np = split_line(line, llen, parts, &val, &vlen);
      if (np < 2 || parts[0].len != 1) continue;
      char tag = parts[0].p[0];
      int nid = (int)parse_ll(parts[1].p, parts[1].len);
      if (nid < 0 || nid >= nt->count) continue;
      SpNode *nd = &nt->nodes[nid];

      if (tag == 'N') {
        /* type is parts[2] */
        if (np >= 3) node_set_type(nd, parts[2].p, parts[2].len);
      }
      else if (np >= 3) {
        Tok field = parts[2];
        if (tag == 'S') {
          size_t uvlen = 0;
          char *uv = unescape_dup(val ? val : "", val ? vlen : 0, &uvlen);
          node_add_str(nd, field.p, field.len, uv, uvlen);
        }
        else if (tag == 'I') {
          node_add_int(nd, field.p, field.len, parse_ll(val ? val : "", val ? vlen : 0));
        }
        else if (tag == 'R') {
          int ref = val ? (int)parse_ll(val, vlen) : -1;
          node_add_ref(nd, field.p, field.len, ref);
        }
        else if (tag == 'A') {
          int n = 0;
          int *ids = parse_id_list(val ? val : "", val ? vlen : 0, &n);
          node_add_arr(nd, field.p, field.len, ids, n);
        }
        else if (tag == 'F') {
          /* content = the value token (float string); field name ignored */
          free(nd->content);
          nd->content = dup_n(val ? val : "", val ? vlen : 0);
        }
      }
    }
  }

  return nt;
}

/* Collect the subtree node ids reachable from `id` via ref + array fields. */
static void nt_clone_collect(NodeTable *nt, int id, char *seen, int *list, int *n) {
  if (id < 0 || id >= nt->count || seen[id]) return;
  seen[id] = 1; list[(*n)++] = id;
  SpNode *nd = &nt->nodes[id];
  for (int j = 0; j < nd->nr; j++) nt_clone_collect(nt, nd->r[j].ref, seen, list, n);
  for (int j = 0; j < nd->na; j++)
    for (int k = 0; k < nd->a[j].n; k++) nt_clone_collect(nt, nd->a[j].ids[k], seen, list, n);
}

/* Deep-clone the subtree rooted at `root`, appending fresh nodes to the
   table and remapping internal child references. Refs/array elements that
   point outside the subtree are kept verbatim. Returns the new root id, or
   -1 on failure. Grows nt->count; callers must resize parallel per-node
   arrays (Compiler.ntype / .nscope) to match afterward. */
int nt_clone_subtree(NodeTable *nt, int root) {
  if (root < 0 || root >= nt->count) return -1;
  char *seen = calloc((size_t)nt->count, 1);
  int *list = malloc(sizeof(int) * (size_t)nt->count);
  if (!seen || !list) { free(seen); free(list); return -1; }
  int sn = 0;
  nt_clone_collect(nt, root, seen, list, &sn);
  free(seen);
  int base = nt->count;
  int *map = malloc(sizeof(int) * (size_t)base);
  if (!map) { free(list); return -1; }
  for (int i = 0; i < base; i++) map[i] = -1;
  for (int i = 0; i < sn; i++) map[list[i]] = base + i;
  SpNode *grown = realloc(nt->nodes, sizeof(SpNode) * (size_t)(base + sn));
  if (!grown) { free(list); free(map); return -1; }
  nt->nodes = grown;
  memset(&nt->nodes[base], 0, sizeof(SpNode) * (size_t)sn);
  for (int i = 0; i < sn; i++) {
    SpNode *src = &nt->nodes[list[i]];
    SpNode *dst = &nt->nodes[base + i];
    if (src->type)    dst->type    = dup_n(src->type, strlen(src->type));
    if (src->content) dst->content = dup_n(src->content, strlen(src->content));
    dst->ns = dst->cs = src->ns;
    if (src->ns) { dst->s = malloc(sizeof(SpStrField) * (size_t)src->ns);
      for (int j = 0; j < src->ns; j++) {
        dst->s[j].key = dup_n(src->s[j].key, strlen(src->s[j].key)); dst->s[j].disc = src->s[j].disc;
        dst->s[j].val_len = src->s[j].val_len;
        dst->s[j].val = malloc(src->s[j].val_len + 1);
        memcpy(dst->s[j].val, src->s[j].val, src->s[j].val_len);
        dst->s[j].val[src->s[j].val_len] = 0;
      } }
    dst->ni = dst->ci = src->ni;
    if (src->ni) { dst->i = malloc(sizeof(SpIntField) * (size_t)src->ni);
      for (int j = 0; j < src->ni; j++) { dst->i[j].key = dup_n(src->i[j].key, strlen(src->i[j].key)); dst->i[j].disc = src->i[j].disc; dst->i[j].val = src->i[j].val; } }
    dst->nr = dst->cr = src->nr;
    if (src->nr) { dst->r = malloc(sizeof(SpRefField) * (size_t)src->nr);
      for (int j = 0; j < src->nr; j++) {
        dst->r[j].key = dup_n(src->r[j].key, strlen(src->r[j].key));
        dst->r[j].disc = src->r[j].disc;
        int rf = src->r[j].ref;
        dst->r[j].ref = (rf >= 0 && rf < base && map[rf] >= 0) ? map[rf] : rf;
      } }
    dst->na = dst->ca = src->na;
    if (src->na) { dst->a = malloc(sizeof(SpArrField) * (size_t)src->na);
      for (int j = 0; j < src->na; j++) {
        dst->a[j].key = dup_n(src->a[j].key, strlen(src->a[j].key));
        dst->a[j].disc = src->a[j].disc;
        dst->a[j].n = src->a[j].n;
        dst->a[j].ids = malloc(sizeof(int) * (size_t)src->a[j].n);
        for (int k = 0; k < src->a[j].n; k++) {
          int e = src->a[j].ids[k];
          dst->a[j].ids[k] = (e >= 0 && e < base && map[e] >= 0) ? map[e] : e;
        }
      } }
  }
  int newroot = map[root];
  nt->count = base + sn;
  nt->version++;
  nt->node_cap = nt->count;  /* realloc above sized the array exactly */
  free(list); free(map);
  return newroot;
}

/* ---- synthetic node construction ---- */

/* Retype an existing node in place. A desugar that rewrites one construct
   into another (a CallOperatorWriteNode into the writer CallNode Ruby means)
   has to keep the id: the parent's ref names it. */
void nt_node_set_type(NodeTable *nt, int id, const char *type) {
  if (id < 0 || id >= nt->count || !type) return;
  SpNode *nd = &nt->nodes[id];
  node_set_type(nd, type, strlen(type));
  nd->kind = 0;          /* the cached NodeKind is stale now */
  nt->version++;
}
int nt_new_node(NodeTable *nt, const char *type) {
  if (nt->count >= nt->node_cap) {
    int nc = nt->node_cap ? nt->node_cap * 2 : 8;
    if (nc <= nt->count) nc = nt->count + 1;
    SpNode *grown = realloc(nt->nodes, sizeof(SpNode) * (size_t)nc);
    if (!grown) return -1;
    nt->nodes = grown;
    nt->node_cap = nc;
  }
  int id = nt->count++;
  nt->version++;
  SpNode *nd = &nt->nodes[id];
  memset(nd, 0, sizeof(SpNode));
  if (type) node_set_type(nd, type, strlen(type));
  return id;
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
  nt->version++;
  free(nd->type); nd->type = NULL;
  free(nd->content); nd->content = NULL;
  for (int j = 0; j < nd->ns; j++) { free(nd->s[j].key); free(nd->s[j].val); }
  for (int j = 0; j < nd->ni; j++) free(nd->i[j].key);
  for (int j = 0; j < nd->nr; j++) free(nd->r[j].key);
  for (int j = 0; j < nd->na; j++) { free(nd->a[j].key); free(nd->a[j].ids); }
  nd->ns = nd->ni = nd->nr = nd->na = 0;
  nd->kind = 0;
  if (type) node_set_type(nd, type, strlen(type));
}

void nt_node_set_content(NodeTable *nt, int id, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  nt->version++;
  free(nd->content);
  nd->content = dup_n(val, strlen(val));
}

void nt_node_set_str(NodeTable *nt, int id, const char *key, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  nt->version++;
  size_t klen = strlen(key), vlen = strlen(val);
  for (int j = 0; j < nd->ns; j++)
    if (sp_streq(nd->s[j].key, key)) {
      free(nd->s[j].val); nd->s[j].val = dup_n(val, vlen); nd->s[j].val_len = vlen; return;
    }
  node_add_str(nd, key, klen, dup_n(val, vlen), vlen);
}

void nt_node_set_int(NodeTable *nt, int id, const char *key, long long val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  for (int j = 0; j < nd->ni; j++)
    if (sp_streq(nd->i[j].key, key)) { nd->i[j].val = val; return; }
  node_add_int(nd, key, strlen(key), val);
}

void nt_node_set_ref(NodeTable *nt, int id, const char *key, int child) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  for (int j = 0; j < nd->nr; j++)
    if (sp_streq(nd->r[j].key, key)) { nd->r[j].ref = child; return; }
  node_add_ref(nd, key, strlen(key), child);
}

void nt_node_set_arr(NodeTable *nt, int id, const char *key, const int *ids, int n) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return;
  int *copy = NULL;
  if (n > 0) {
    copy = malloc(sizeof(int) * (size_t)n);
    if (copy) memcpy(copy, ids, sizeof(int) * (size_t)n);
    else n = 0;  /* OOM: store an empty array rather than a NULL/count mismatch */
  }
  for (int j = 0; j < nd->na; j++)
    if (sp_streq(nd->a[j].key, key)) { free(nd->a[j].ids); nd->a[j].ids = copy; nd->a[j].n = n; return; }
  node_add_arr(nd, key, strlen(key), copy, n);
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
  free(nt->source_file);
  for (int k = 0; k < nt->nfiles; k++) free(nt->files[k]);
  free(nt->files);
  free(nt);
}

/* ---- accessors ---- */

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

/* Node ids grouped by kind (CSR), cached per node table. The analyze passes
   each scan the whole table filtering by node kind, dozens of times per fixpoint
   iteration; this lets a pass walk only the nodes of the kind it cares about.
   Ids within a kind are in ascending order (matching a forward `for id` scan).
   Rebuilt when the table pointer or count changes (desugar passes append nodes).
   nt_kind is O(1) (cached per node), so a rebuild is a single linear pass. */
static const NodeTable *nki_nt = NULL;
static int nki_ntc = -1;
static int *nki_ids = NULL;   /* all ids, grouped by kind */
static int *nki_off = NULL;   /* size NK__COUNT+1; kind k spans [off[k], off[k+1]) */
/* Id arrays superseded while an NT_FOREACH_KIND loop was open (its body
   appended nodes and something rebuilt the cache): kept until the last open
   iterator closes, since the loop is still reading its own. */
static int nki_live = 0;
static int **nki_retired = NULL;
static int nki_nretired = 0, nki_cretired = 0;
static void nki_retire(int *p) {
  if (!p) return;
  if (nki_nretired == nki_cretired) {
    int nc = nki_cretired ? nki_cretired * 2 : 8;
    int **np = (int **)realloc(nki_retired, (size_t)nc * sizeof(int *));
    if (!np) return;   /* kept alive by the leak rather than freed under a reader */
    nki_retired = np; nki_cretired = nc;
  }
  nki_retired[nki_nretired++] = p;
}
void nt_kind_iter_open(void) { nki_live++; }
void nt_kind_iter_close(NtKindIter *it) {
  (void)it;
  if (--nki_live > 0) return;
  for (int i = 0; i < nki_nretired; i++) free(nki_retired[i]);
  nki_nretired = 0;
}
static void nki_build(const NodeTable *nt) {
  int n = nt->count;
  if (nki_live > 0) nki_retire(nki_ids); else free(nki_ids);
  free(nki_off);
  nki_off = (int *)calloc((size_t)NK__COUNT + 1, sizeof(int));
  nki_ids = (int *)malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
  nki_nt = nt; nki_ntc = n;
  if (!nki_off || !nki_ids) { nki_nt = NULL; nki_ntc = -1; return; }
  for (int id = 0; id < n; id++) nki_off[(int)nt_kind(nt, id) + 1]++;
  for (int k = 1; k <= NK__COUNT; k++) nki_off[k] += nki_off[k - 1];
  /* nki_off[k] is now the start of kind k; fill, advancing a local cursor */
  int *cur = (int *)malloc((size_t)NK__COUNT * sizeof(int));
  if (!cur) { free(nki_ids); free(nki_off); nki_ids = NULL; nki_off = NULL; nki_nt = NULL; nki_ntc = -1; return; }
  for (int k = 0; k < NK__COUNT; k++) cur[k] = nki_off[k];
  for (int id = 0; id < n; id++) { int k = (int)nt_kind(nt, id); nki_ids[cur[k]++] = id; }
  free(cur);
}
/* Return the ids of nodes of kind `k` (ascending), writing the count to *count.
   The returned pointer is valid until the next call with a changed node table.
   Falls back to count 0 on allocation failure (callers then see no nodes -- they
   must only use this for kinds they would otherwise have filtered for anyway). */
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
  nt->version++;
  nki_nt = NULL;   /* the kind index lists ids by kind: rebuild it */
}

const int *nt_nodes_of_kind(const NodeTable *nt, NodeKind k, int *count) {
  if (nki_nt != nt || nki_ntc != nt->count) nki_build(nt);
  if (!nki_off || !nki_ids || k < 0 || k >= NK__COUNT) { *count = 0; return NULL; }
  *count = nki_off[k + 1] - nki_off[k];
  return nki_ids + nki_off[k];
}


int nt_set_str(NodeTable *nt, int id, const char *key, const char *val) {
  SpNode *nd = (SpNode *)node_at(nt, id);
  if (!nd) return 0;
  nt->version++;
  size_t vlen = strlen(val);
  for (int j = 0; j < nd->ns; j++) {
    if (sp_streq(nd->s[j].key, key)) {
      free(nd->s[j].val);
      nd->s[j].val = malloc(vlen + 1);
      memcpy(nd->s[j].val, val, vlen + 1);
      nd->s[j].val_len = vlen;
      return 1;
    }
  }
  return 0;
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
  for (int i = 0; i < argc; i++) {
    const char *ty = nt_type(nt, argv[i]);
    if (ty && (sp_streq(ty, "SplatNode") || sp_streq(ty, "BlockArgumentNode"))) return 0;
  }
  return 1;
}

/* Copy the subtree at `root` of `src` into `dst` (every field, children
   recursively); the new root's id in dst, or -1. A desugar parses a snippet
   into its own table and grafts the result into the program's. */
int nt_import_subtree(NodeTable *dst, const NodeTable *src, int root) {
  if (!dst || !src || root < 0 || root >= src->count) return -1;
  const SpNode *sn = &src->nodes[root];
  int id = nt_new_node(dst, sn->type ? sn->type : "");
  if (id < 0) return -1;
  if (sn->content) { SpNode *dn = &dst->nodes[id]; dn->content = strdup(sn->content); }
  for (int j = 0; j < sn->ns; j++) {
    char *v = (char *)malloc(sn->s[j].val_len + 1);
    if (!v) return -1;
    memcpy(v, sn->s[j].val, sn->s[j].val_len); v[sn->s[j].val_len] = 0;
    node_add_str(&dst->nodes[id], sn->s[j].key, strlen(sn->s[j].key), v, sn->s[j].val_len);
  }
  for (int j = 0; j < sn->ni; j++) nt_node_set_int(dst, id, sn->i[j].key, sn->i[j].val);
  for (int j = 0; j < sn->nr; j++) {
    int ch = sn->r[j].ref >= 0 ? nt_import_subtree(dst, src, sn->r[j].ref) : -1;
    nt_node_set_ref(dst, id, sn->r[j].key, ch);
  }
  for (int j = 0; j < sn->na; j++) {
    int n = sn->a[j].n;
    int *ids = (int *)malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    if (!ids) return -1;
    for (int q = 0; q < n; q++) ids[q] = sn->a[j].ids[q] >= 0 ? nt_import_subtree(dst, src, sn->a[j].ids[q]) : -1;
    nt_node_set_arr(dst, id, sn->a[j].key, ids, n);
    free(ids);
  }
  return id;
}
