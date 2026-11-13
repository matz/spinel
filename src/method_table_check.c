/* method_table_check.c -- --method-table-check (#10866, step 1). See
   method_table_check.h. */
#include "method_table_check.h"
#include "call_plan.h"
#include "builtin_names.h"
#include "implied_calls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_method_table_check = 0;

/* One site: a read is (frame node or pass, side, name), the first read's
   class and primitive kept; an answer of the question is (call node, side,
   class, name, why). */
typedef struct {
  int node, side, cls, prim, why;   /* why: 0 for a read, else MLW_* */
  int build;                        /* a read made inside the list builders' window */
  char *name, *phase;               /* phase: the pass when no node was open */
} MtSite;

struct MethodTableCheck {
  int depth, frame;     /* brackets open; the node being inferred or emitted */
  int build;            /* list builders' windows open */
  MtSite *site;
  int n, cap;
  int *ix, ixcap;       /* open hash over site: index + 1, 0 empty */
};

struct MethodTableCheck *mtab_new(void) {
  struct MethodTableCheck *m = calloc(1, sizeof *m);
  if (!m) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->frame = -1;
  return m;
}

void mtab_free(struct MethodTableCheck *m) {
  if (!m) return;
  for (int i = 0; i < m->n; i++) { free(m->site[i].name); free(m->site[i].phase); }
  free(m->site); free(m->ix); free(m);
}

void mtab_depth(Compiler *c, int by) {
  if (c->mtab) c->mtab->depth += by;
}

void mtab_build_depth(Compiler *c, int by) {
  if (c->mtab) c->mtab->build += by;
}

MtabMark mtab_mark(Compiler *c) {
  struct MethodTableCheck *m = c->mtab;
  return m ? (MtabMark){ m->frame, m->depth, m->build } : (MtabMark){ -1, 0, 0 };
}

void mtab_unwind(Compiler *c, MtabMark was) {
  struct MethodTableCheck *m = c->mtab;
  if (m) { m->frame = was.frame; m->depth = was.depth; m->build = was.build; }
}

MtabMark mtab_frame_set(Compiler *c, int id) {
  struct MethodTableCheck *m = c->mtab;
  MtabMark was = mtab_mark(c);
  if (m) { m->frame = id; m->depth = 0; m->build = 0; }
  return was;
}

static unsigned site_hash(const MtSite *s) {
  unsigned h = (unsigned)s->node * 2654435761u ^ (unsigned)s->side * 40503u ^ (unsigned)s->why * 97u ^ (unsigned)s->build * 31337u;
  if (s->why) h ^= (unsigned)s->cls * 2246822519u;
  for (const char *q = s->name; *q; q++) h = h * 31u + (unsigned char)*q;
  for (const char *q = s->node < 0 ? s->phase : ""; *q; q++) h = h * 31u + (unsigned char)*q;
  return h;
}

static int site_same(const MtSite *a, const MtSite *b) {
  return a->node == b->node && a->side == b->side && a->why == b->why && a->build == b->build && (!a->why || a->cls == b->cls) &&
         sp_streq(a->name, b->name) && (a->node >= 0 || sp_streq(a->phase, b->phase));
}

static void mtab_grow(struct MethodTableCheck *m) {
  free(m->ix);
  m->ixcap = m->ixcap ? 2 * m->ixcap : 1024;
  m->ix = calloc((size_t)m->ixcap, sizeof *m->ix);
  if (!m->ix) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  for (int i = 0; i < m->n; i++) {
    unsigned j = site_hash(&m->site[i]) & (unsigned)(m->ixcap - 1);
    while (m->ix[j]) j = (j + 1) & (unsigned)(m->ixcap - 1);
    m->ix[j] = i + 1;
  }
}

/* s noted once: kept (its strings copied) where it is new */
static void mtab_note(Compiler *c, MtSite s) {
  struct MethodTableCheck *m = c->mtab;
  if (!s.phase) s.phase = "pass";
  if (2 * (m->n + 1) > m->ixcap) mtab_grow(m);
  unsigned j = site_hash(&s) & (unsigned)(m->ixcap - 1);
  for (; m->ix[j]; j = (j + 1) & (unsigned)(m->ixcap - 1))
    if (site_same(&m->site[m->ix[j] - 1], &s)) return;
  if (m->n == m->cap) {
    m->cap = m->cap ? 2 * m->cap : 1024;
    m->site = realloc(m->site, sizeof *m->site * (size_t)m->cap);
    if (!m->site) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  }
  s.name = strdup(s.name);
  s.phase = strdup(s.phase);
  if (!s.name || !s.phase) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->site[m->n] = s;
  m->ix[j] = ++m->n;
}

void mtab_read(Compiler *c, MtPrim prim, int cls, int side, const char *name) {
  struct MethodTableCheck *m = c->mtab;
  if (!m || m->depth > 0 || !name) return;
  mtab_note(c, (MtSite){ .node = m->frame, .side = side, .cls = cls, .prim = (int)prim, .build = m->build > 0, .name = (char *)name,
                         .phase = (char *)c->nt->pass });
}

void mtab_table(Compiler *c, int id, int cls, unsigned flags, const char *name, int why) {
  struct MethodTableCheck *m = c->mtab;
  if (!m || !name) return;
  mtab_note(c, (MtSite){ .node = id, .side = (flags & CT_CMETHOD) ? MTS_CMETH : MTS_INST, .cls = (flags & CT_TOP) ? -1 : cls,
                         .why = why, .name = (char *)name });
}

static const char *prim_name(int prim) {
  switch ((MtPrim)prim) {
  case MT_METHOD_IN_CLASS: return "comp_method_in_class";
  case MT_CMETHOD_IN_CLASS: return "comp_cmethod_in_class";
  case MT_METHOD_IN_CHAIN: return "comp_method_in_chain";
  case MT_CMETHOD_IN_CHAIN: return "comp_cmethod_in_chain";
  case MT_SELF_METHOD_IN_CHAIN: return "comp_self_method_in_chain";
  case MT_METHOD_INDEX: return "comp_method_index";
  case MT_MAIN_METHOD: return "comp_main_method";
  case MT_INCLUDED_METHOD_INDEX: return "comp_included_method_index";
  case MT_READER_IN_CHAIN: return "comp_reader_in_chain";
  case MT_WRITER_IN_CHAIN: return "comp_writer_in_chain";
  case MT_IS_UNDEFFED_IN_CHAIN: return "comp_is_undeffed_in_chain";
  case MT_METHOD_VIS_IN_CHAIN: return "comp_method_vis_in_chain";
  }
  return "?";
}

static const char *why_name(int why) {
  switch ((MlWhy)why) {
  case MLW_UNPLACED: return "unplaced";
  case MLW_MOVES: return "moves";
  case MLW_MANY: return "many";
  case MLW_UNMAPPED: return "unmapped";
  case MLW_DISAGREE: return "disagree";
  case MLW_NEVER: return "never";
  case MLW_UNASKED: return "unasked";
  }
  return "?";
}

/* Is `name` the method the super at fn calls: the super's own name, the
   shadow its scope reaches through the prepend chain, or the name its
   proc-form clone runs under? */
static int super_reads_own_name(Compiler *c, int fn, const char *name) {
  const char *own[2];
  int n = call_node_names(c, fn, own);
  for (int i = 0; i < n; i++)
    if (own[i] && sp_streq(own[i], name)) return 1;
  const Scope *s = comp_scope_of(c, fn);
  s = s ? comp_super_site(c, s) : NULL;
  if (!s) return 0;
  const char *shadow = comp_super_shadow(c, s);
  const char *u = comp_scope_user_name(s);
  return (shadow && sp_streq(shadow, name)) || (u && sp_streq(u, name)) || (s->name && sp_streq(s->name, name));
}

/* Is `name` one the send or Method maker at fn sends or captures: a literal
   send's name, a maker's Symbol (call_node_names past the node's own)? Not
   where it is the node's own name too (`method(:method)`, an arm of a
   run-time name): the call's question of its own name asks it as well. */
static int send_reads_target(Compiler *c, int fn, const char *name) {
  const char *own[2];
  int n = call_node_names(c, fn, own);
  if (n > 0 && own[0] && sp_streq(own[0], name)) return 0;
  for (int i = 1; i < n; i++)
    if (own[i] && sp_streq(own[i], name)) return 1;
  return 0;
}

/* The family of the reader: what the node being inferred or emitted is, by
   the names it calls (call_node_names) and the predicates of
   builtin_names.c. A node with no frame is a pass: family "pass", with the
   name of the pass it ran in in a field of its own. */
static const char *reader_family(Compiler *c, int fn, const char *name, int build) {
  const NodeTable *nt = c->nt;
  NodeKind k = nt_kind(nt, fn);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    if (super_reads_own_name(c, fn, name)) return "super";
    /* another name's question, asked under the super (D3 a): a respond_to? that supers asks presence, any other the implied call */
    const Scope *s = comp_scope_of(c, fn);
    s = s ? comp_super_site(c, s) : NULL;
    const char *sn = s ? comp_scope_user_name(s) : NULL;
    return sn && (is_respond_to_name(sn) || is_method_defined_query(sn)) ? "presence" : "implied";
  }
  if (k == NK_BlockArgumentNode) {   /* &:sym calls the name; any other &obj calls its to_proc */
    int ex = nt_ref(nt, fn, "expression");
    const char *sym = ex >= 0 && nt_kind(nt, ex) == NK_SymbolNode ? nt_str(nt, ex, "value") : NULL;
    return sym && sp_streq(sym, name) ? "send" : "implied";
  }
  if (k == NK_DefinedNode) return "presence";
  const char *nm = k == NK_CallNode ? nt_str(nt, fn, "name") : NULL;
  if (nm) {
    /* a send or a Method maker is the family's reader where it asks of the
       name it sends or captures; its own name (`send`, `method`) is the
       call's, as any call's is, and another name counts as under any call
       (method_missing, a conversion: implied). A run-time name's arms are
       calls of their own, each the frame of what is asked of it
       (emit_dynamic_send_arm). */
    if ((is_send_family(nm) || is_method_object_maker(nm)) && send_reads_target(c, fn, name)) return "send";
    if (is_respond_to_name(nm) || is_method_defined_query(nm)) return "presence";
    /* a read under a list's node is the list's own where a builder made it (the window of mtab_build_enter), the
       call's where the plan's resolution of the list's call did */
    if (is_instance_method_list(nm) || is_method_presence_query(nm) >= 2) return build ? "lists" : "call";
  }
  const char *own[2];
  int n = call_node_names(c, fn, own);
  for (int i = 0; i < n; i++)
    if (own[i] && sp_streq(own[i], name)) return "call";
  return n > 0 || nt_implied_possible(nt, fn) ? "implied" : "other";
}

/* the program's site: file:line, <synthesized> for a node no source line made, - for none */
static void print_where(Compiler *c, int node, FILE *f) {
  const NodeTable *nt = c->nt;
  long long line = node >= 0 ? nt_int(nt, node, "node_line", 0) : 0;
  const char *file = node >= 0 ? nt_file_path(nt, (int)nt_int(nt, node, "node_file", 0)) : NULL;
  if (node < 0) fputc('-', f);
  else if (line <= 0 || !file) fputs("<synthesized>", f);
  else fprintf(f, "%s:%lld", file, line);
}

static void print_target(Compiler *c, const MtSite *s, FILE *f) {
  const char *side = s->side == MTS_CMETH ? "cmeth" : s->side == MTS_SELF ? "self" : "inst";
  const char *cn = s->cls >= 0 && s->cls < c->nclasses ? c->classes[s->cls].name : "top";
  fprintf(f, "%s:%s#%s", side, cn ? cn : "?", s->name);
}

/* One line per site, in the order the sites were first met:
   read <family> <pass or -> <file>:<line> <side>:<Class>#<name> <prim> [versioned] */
void mtab_report(Compiler *c) {
  struct MethodTableCheck *m = c->mtab;
  if (!m) return;
  c->mtab = NULL;   /* (the report's own questions are no reads, and note none) */
  for (int i = 0; i < m->n; i++) {
    const MtSite *s = &m->site[i];
    if (s->why) fprintf(stderr, "method-table-check: table %s ", why_name(s->why));
    else if (s->node >= 0) fprintf(stderr, "method-table-check: read %s - ", reader_family(c, s->node, s->name, s->build));
    else fprintf(stderr, "method-table-check: read pass %s ", s->phase);
    print_where(c, s->node, stderr);
    fputc(' ', stderr);
    print_target(c, s, stderr);
    if (!s->why) fprintf(stderr, " %s%s", prim_name(s->prim), ct_version_named(c, s->name) ? " versioned" : "");
    fputc('\n', stderr);
  }
  c->mtab = m;
}
