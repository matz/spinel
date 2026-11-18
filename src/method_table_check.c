/* method_table_check.c -- --method-table-check (#10866, step 1). See
   method_table_check.h. */
#include "method_table_check.h"
#include "call_plan.h"
#include "builtin_names.h"
#include "implied_calls.h"
#include "implicit_calls.h"
#include "analyze_internal.h"

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
  char *fname, *ftgt;               /* a call frame's own name and the one it sends or captures, as they were
                                       when the read was made (a pass may retarget the call later); NULL else */
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
  for (int i = 0; i < m->n; i++) { free(m->site[i].name); free(m->site[i].phase); free(m->site[i].fname); free(m->site[i].ftgt); }
  free(m->site); free(m->ix); free(m);
}

void mtab_depth(Compiler *c, int by) {
  if (c->mtab) c->mtab->depth += by;
}

void mtab_build_depth(Compiler *c, int by) {
  if (c->mtab) c->mtab->build += by;
}

int mtab_framed(Compiler *c) {
  return c->mtab && c->mtab->frame >= 0;
}

MtabMark mtab_mark(Compiler *c) {
  struct MethodTableCheck *m = c->mtab;
  return m ? (MtabMark){ m->frame, m->depth, m->build } : (MtabMark){ -1, 0, 0 };
}

void mtab_step_report(Compiler *c, MtabMark was, const char *pass) {
  struct MethodTableCheck *m = c->mtab;
  if (m && (m->depth != was.depth || m->build != was.build))
    fprintf(stderr, "method-table-check: unbalanced bracket in %s\n", pass);
  mtab_unwind(c, was);
}

void mtab_unwind(Compiler *c, MtabMark was) {
  struct MethodTableCheck *m = c->mtab;
  if (m) { m->frame = was.frame; m->depth = was.depth; m->build = was.build; }
}

MtabMark mtab_frame_keep(Compiler *c, int id) {
  struct MethodTableCheck *m = c->mtab;
  MtabMark was = mtab_mark(c);
  if (m && id >= 0) m->frame = id;   /* (no node: the caller's frame stays) */
  return was;
}

/* The nearest node above `node` that has an implied call named `name` (a `===` row of a pattern, an `in` or a
   `when`: the node that holds the row), as the frame of what is asked about `node`. */
MtabMark mtab_frame_row_owner(Compiler *c, int node, const char *name) {
  const int *par = comp_parent_map(c);
  int up = node >= 0 ? par[node] : -1;
  while (up >= 0 && !ic_node_calls_name(c, up, name, 1)) up = par[up];
  return mtab_frame_keep(c, up);
}

MtabMark mtab_frame_set(Compiler *c, int id) {
  struct MethodTableCheck *m = c->mtab;
  MtabMark was = mtab_frame_keep(c, id);
  if (m) { m->frame = id; m->depth = 0; m->build = 0; }
  return was;
}

static unsigned site_hash(const MtSite *s) {
  unsigned h = (unsigned)s->node * 2654435761u ^ (unsigned)s->side * 40503u ^ (unsigned)s->why * 97u ^ (unsigned)s->build * 31337u;
  if (s->why) h ^= (unsigned)s->cls * 2246822519u;
  for (const char *q = s->name; *q; q++) h = h * 31u + (unsigned char)*q;
  for (const char *q = s->node < 0 ? s->phase : ""; *q; q++) h = h * 31u + (unsigned char)*q;
  for (const char *q = s->fname ? s->fname : ""; *q; q++) h = h * 37u + (unsigned char)*q;
  return h;
}

/* (equal strings, or both NULL) */
static int str_same(const char *a, const char *b) {
  return a == b || (a && b && sp_streq(a, b));
}

static int site_same(const MtSite *a, const MtSite *b) {
  return a->node == b->node && a->side == b->side && a->why == b->why && a->build == b->build && (!a->why || a->cls == b->cls) &&
         sp_streq(a->name, b->name) && (a->node >= 0 || sp_streq(a->phase, b->phase)) && str_same(a->fname, b->fname) &&
         str_same(a->ftgt, b->ftgt);
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
  const char *fname = s.fname, *ftgt = s.ftgt;
  s.name = strdup(s.name);
  s.phase = strdup(s.phase);
  s.fname = fname ? strdup(fname) : NULL;
  s.ftgt = ftgt ? strdup(ftgt) : NULL;
  if (!s.name || !s.phase || (fname && !s.fname) || (ftgt && !s.ftgt)) { fprintf(stderr, "spinel: out of memory\n"); exit(1); }
  m->site[m->n] = s;
  m->ix[j] = ++m->n;
}

/* A call frame's own name and the one it sends or captures (call_node_names:
   a literal send's name, a maker's Symbol, or for a maker retargeted at a
   builtin's `__bam_` wrapper the name its Symbol had, bam_sym), as they
   are now: a later pass may retarget the call (a send at its literal name,
   public_method made a method), and the read is the call's as it was. */
static void frame_call_names(Compiler *c, int fn, const char **own, const char **tgt) {
  const NodeTable *nt = c->nt;
  const char *nm[2];
  int n = call_node_names(c, fn, nm);
  *own = n > 0 ? nm[0] : NULL;
  *tgt = n > 1 ? nm[1] : NULL;
  int args = nt_ref(nt, fn, "arguments"), an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  const char *was = an > 0 && nt_kind(nt, av[0]) == NK_SymbolNode ? nt_str(nt, av[0], "bam_sym") : NULL;
  if (was && *own && is_method_object_maker(*own)) *tgt = was;
}

void mtab_read(Compiler *c, MtPrim prim, int cls, int side, const char *name) {
  struct MethodTableCheck *m = c->mtab;
  if (!m || m->depth > 0 || !name) return;
  const char *own = NULL, *tgt = NULL;
  if (m->frame >= 0 && nt_kind(c->nt, m->frame) == NK_CallNode) frame_call_names(c, m->frame, &own, &tgt);
  mtab_note(c, (MtSite){ .node = m->frame, .side = side, .cls = cls, .prim = (int)prim, .build = m->build > 0,
                         .name = (char *)name, .phase = (char *)c->nt->pass, .fname = (char *)own, .ftgt = (char *)tgt });
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
  case MLW_ACCESSOR: return "accessor";
  case MLW_VIS: return "visibility";
  case MLW_UNDEF: return "undefined";
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

/* The family of the reader: what the node being inferred or emitted is, by
   the names it calls (call_node_names) and the predicates of
   builtin_names.c; a call by the names it had when the read was made
   (frame_call_names). A node with no frame is a pass: family "pass", with
   the name of the pass it ran in in a field of its own.

   A read of another name under a call or a super is `implied` only for one
   of the node's own implied calls (ic_node_calls_name: its syntax rows and
   its builtin's callbacks), and `call` otherwise: a question about another
   call (its receiver's, an argument's) or the call's method_missing, which
   every call may reach as its lookup's tail. A frame with no names of its
   own is `implied` for a row of a node it holds directly (ic_held_rows_name),
   and for an implied call of the call it is an operand of (ic_operand_host);
   `call` where the name is that host call's own (the Range a `clamp` is
   given), and `implied` for a row of its direct parent where that is no call
   (the receiver of `a[i] += 1`: its `[]` and `[]=`); `other` otherwise. */
static const char *reader_family(Compiler *c, const MtSite *s) {
  const NodeTable *nt = c->nt;
  int fn = s->node;
  const char *name = s->name;
  if (s->fname) {
    const char *nm = s->fname;
    int own = sp_streq(nm, name), tgt = s->ftgt && sp_streq(s->ftgt, name);
    /* a send or a Method maker is the family's reader where it asks of the
       name it sends or captures; its own name (`send`, `method`) is the
       call's, as any call's is, also where the captured name is the same
       (`method(:method)`, an arm of a run-time name: the call's question of
       its own name asks it as well), and another name counts as under any
       call (method_missing, a conversion: implied). A run-time name's arms
       are calls of their own, each the frame of what is asked of it
       (emit_dynamic_send_arm). */
    if ((is_send_family(nm) || is_method_object_maker(nm)) && tgt && !own) return "send";
    /* the presence query's question is the queried name's; a read of its own name (`respond_to?` asked of a class
       that may define one) is the call's, as any call's is */
    if (is_method_presence_name_query(nm)) return own ? "call" : "presence";
    /* a read under a list's node is the list's own where a builder made it (the window of mtab_build_enter), the
       call's where the plan's resolution of the list's call did */
    if (is_instance_method_list(nm) || is_method_presence_query(nm) >= 2) return s->build ? "lists" : "call";
    /* (method_missing alone, not is_method_missing_name: respond_to_missing? is a real implied row of respond_to?) */
    if (own || tgt || sp_streq(name, METHOD_MISSING_NAME)) return "call";
    return ic_node_calls_name(c, fn, name, s->side == MTS_CMETH) ? "implied" : "call";
  }
  NodeKind k = nt_kind(nt, fn);
  if (k == NK_SuperNode || k == NK_ForwardingSuperNode) {
    if (super_reads_own_name(c, fn, name)) return "super";
    /* another name's question, asked under the super (D3 a): a respond_to? that supers asks presence, any other the implied call */
    const Scope *sc = comp_scope_of(c, fn);
    sc = sc ? comp_super_site(c, sc) : NULL;
    const char *sn = sc ? comp_scope_user_name(sc) : NULL;
    if (is_method_presence_name_query(sn)) return "presence";
    return !sp_streq(name, METHOD_MISSING_NAME) && ic_node_calls_name(c, fn, name, s->side == MTS_CMETH) ? "implied" : "call";
  }
  if (k == NK_BlockArgumentNode) {   /* &:sym calls the name; any other &obj calls its to_proc */
    int ex = nt_ref(nt, fn, "expression");
    const char *sym = ex >= 0 && nt_kind(nt, ex) == NK_SymbolNode ? nt_str(nt, ex, "value") : NULL;
    return sym && sp_streq(sym, name) ? "send" : "implied";
  }
  if (k == NK_DefinedNode) return "presence";
  const char *own[2];
  int n = call_node_names(c, fn, own);
  for (int i = 0; i < n; i++)
    if (own[i] && sp_streq(own[i], name)) return "call";
  if (ic_node_calls_name(c, fn, name, s->side == MTS_CMETH) || ic_held_rows_name(c, fn, name)) return "implied";
  /* an operand's frame (a variable read some emitter converts) is the implied call of the call it is an operand of */
  int host = n == 0 ? ic_operand_host(c, fn) : fn;
  if (host != fn && ic_node_calls_name(c, host, name, s->side == MTS_CMETH)) return "implied";
  /* ...or the call's own, where the name is the host call's (the Range a `clamp` is given: its question of `clamp`) */
  if (host != fn) {
    const char *hn[2];
    int nh = call_node_names(c, host, hn);
    for (int i = 0; i < nh; i++)
      if (hn[i] && sp_streq(hn[i], name)) return "call";
  }
  /* ...or the implied call of the node it is the operand of, where that node is no call (the receiver of `a[i] += 1`
     and its `[]` and `[]=`) */
  if (n == 0) {
    int up = comp_parent_map(c)[fn];
    if (up >= 0 && ic_node_calls_name(c, up, name, s->side == MTS_CMETH)) return "implied";
  }
  return n > 0 ? "call" : "other";
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
    else if (s->node >= 0) fprintf(stderr, "method-table-check: read %s - ", reader_family(c, s));
    else fprintf(stderr, "method-table-check: read pass %s ", s->phase);
    print_where(c, s->node, stderr);
    fputc(' ', stderr);
    print_target(c, s, stderr);
    if (!s->why) fprintf(stderr, " %s%s", prim_name(s->prim), ct_version_named(c, s->name) ? " versioned" : "");
    fputc('\n', stderr);
  }
  c->mtab = m;
}
