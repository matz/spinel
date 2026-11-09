/* The emission trace build's hooks (`make build/spinel-trace`; tools/emit_trace.rb
   runs it and reads the file). This file is compiled only into that variant,
   whose objects are built with -finstrument-functions: the C compiler then
   calls __cyg_profile_func_enter/exit around every function of src/, and the
   two hooks below write what ran to the file SPINEL_EMIT_TRACE names.

   The trace is a sequence of 16-byte records, a native uint32 tag and three
   more (or 12 bytes of text):
     TR_HEAD   v[0],v[1] = offset of emit_trace_node, for the reader to line
               the offsets up with the binary's symbol table
     TR_NODE   v = node id, node_line, node_col; the tag's second byte is the
               marker's kind ('c' a call, 's' a statement, 'l' the last
               statement of a method that answers it) and its third is 1 when
               the node is in a required file
     TR_TEXT   the node's label, the call's method name or the statement's
               node type: up to three records after TR_NODE, NUL-padded
     TR_ENTER  v[0],v[1] = the function's offset from the image base
     TR_EXIT   the same
     TR_STACK  (the analysis trace) the reader drops its open frames; the
               TR_ENTER records that follow are the functions open now,
               outermost first
     TR_PASS   (the analysis trace) v[0] = the round; TR_TEXT records carry
               the running pass's name, and the class walk's after a space
     TR_FACT   (the analysis trace) a node's fact changed: v = node id,
               node_line, node_col, the tag's bytes as TR_NODE's; TR_TEXT
               records carry the change, up to eight
   Offsets, not addresses, so two runs write the same bytes under address
   randomization. Names are not resolved here: a static function is not in the
   dynamic symbol table on Linux, so the reader maps the offsets with `nm`.

   Recording starts at the first node marker, which the code generator writes:
   the parse and the analysis run untraced, or one mid-size program's trace
   would be gigabytes. With the variable unset nothing is opened and the hooks
   return at once. SPINEL_EMIT_TRACE_LINE=N narrows it further, to the calls
   and statements on line N of the program's own file and what they call:
   megabytes become kilobytes, and the reader has asked for no more.

   SPINEL_EMIT_TRACE_ANALYSIS, set, traces the analysis of that line instead
   (it needs the line): which pass and which functions gave a node its type
   ('t'), its nil fact ('n') and its class set ('k'). The inference marks
   where a node's fact is about to be computed (emit_trace_ask) and where one
   changed (emit_trace_fact). The hooks keep the open functions in an array,
   writing nothing; an ask for a node of the line writes that array, the pass
   and the node, and records until the asking function returns. A recording
   is held in memory and written only when it matters: the fixpoint asks a
   node's type again in every round, and only the asks that changed a type of
   the line are kept. A fact a pass writes outside any ask is written with
   the open functions and nothing below.

   The state below is file-scope on purpose. It exists only in the variant, it
   is no compiler state (no pass or emitter reads it), and the hooks have no
   argument to carry a context in. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* dladdr on glibc */
#endif
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen_internal.h"
#include "analyze.h"

#define TR_HOOK __attribute__((no_instrument_function))

enum { TR_HEAD = 1, TR_NODE = 2, TR_ENTER = 3, TR_EXIT = 4, TR_TEXT = 5, TR_STACK = 6, TR_PASS = 7, TR_FACT = 8,
       TR_TEXT_LEN = 12, TR_FRAMES = 1 << 16 };
typedef struct { uint32_t tag; union { uint32_t v[3]; char text[TR_TEXT_LEN]; } u; } TraceRec;

static FILE *tr_out;        /* the open trace, NULL while nothing is recorded */
static int tr_asked;        /* SPINEL_EMIT_TRACE has been looked at */
static uintptr_t tr_base;   /* where the compiler's image is loaded */
static uint32_t tr_line;    /* SPINEL_EMIT_TRACE_LINE, or 0 for every line */
static int tr_depth;        /* with a line: the frames open inside a node of it, 0 outside one */
static int tr_on;           /* the emission trace: the first marker was seen */
static int tr_analysis;     /* SPINEL_EMIT_TRACE_ANALYSIS: the analysis trace, and what follows is its state */
static uintptr_t tr_stack[TR_FRAMES];   /* the functions open now */
static int tr_sp;           /* ... and how many (the ones past TR_FRAMES are counted, not held) */
static int tr_rec;          /* recording: the count of open functions at the ask, 0 outside a recording */
static int tr_keep;         /* the recording is worth writing */
static TraceRec *tr_buf;    /* the recording so far */
static size_t tr_nbuf, tr_cbuf;

void emit_trace_node(Compiler *c, int id, int kind) TR_HOOK;
void emit_trace_ask(Compiler *c, int id, int kind, int round, const char *walk) TR_HOOK;
void emit_trace_fact(Compiler *c, int id, int kind, int was, int now) TR_HOOK;
void emit_trace_sets(Compiler *c, const char *walk) TR_HOOK;
void __cyg_profile_func_enter(void *fn, void *site) TR_HOOK;
void __cyg_profile_func_exit(void *fn, void *site) TR_HOOK;

/* One record: to the file, or (the analysis trace) to the recording */
static TR_HOOK void tr_rec_put(const TraceRec *r) {
  if (!tr_analysis) { fwrite(r, sizeof *r, 1, tr_out); return; }
  if (tr_nbuf == tr_cbuf) {
    tr_cbuf = tr_cbuf ? tr_cbuf * 2 : 4096;
    tr_buf = realloc(tr_buf, tr_cbuf * sizeof *tr_buf);
    if (!tr_buf) { fprintf(stderr, "spinel-trace: out of memory\n"); exit(1); }
  }
  tr_buf[tr_nbuf++] = *r;
}

static TR_HOOK void tr_put(uint32_t tag, uint32_t v0, uint32_t v1, uint32_t v2) {
  TraceRec r = { tag, { { v0, v1, v2 } } };
  tr_rec_put(&r);
}

/* The text of the record before: up to max records, NUL-padded */
static TR_HOOK void tr_text(const char *text, int max) {
  size_t len = text ? strlen(text) : 0;
  for (size_t i = 0; i < len && i < (size_t)max * TR_TEXT_LEN; i += TR_TEXT_LEN) {
    TraceRec r = { TR_TEXT, { { 0, 0, 0 } } };
    memcpy(r.u.text, text + i, len - i < TR_TEXT_LEN ? len - i : TR_TEXT_LEN);
    tr_rec_put(&r);
  }
}

/* The recording ends: written, or forgotten */
static TR_HOOK void tr_end(int keep) {
  if (keep) fwrite(tr_buf, sizeof *tr_buf, tr_nbuf, tr_out);
  tr_nbuf = 0;
  tr_rec = 0;
}

static TR_HOOK void tr_put_fn(uint32_t tag, void *fn) {
  uint64_t off = (uintptr_t)fn - tr_base;
  tr_put(tag, (uint32_t)off, (uint32_t)(off >> 32), 0);
}

/* Open the trace, at the first function entered. stdio buffers the records
   and writes what is left at exit, so a refusal's exit(1) keeps the trace up
   to it. */
static TR_HOOK void tr_open(void) {
  const char *path = getenv("SPINEL_EMIT_TRACE"), *line = getenv("SPINEL_EMIT_TRACE_LINE");
  Dl_info info;
  tr_asked = 1;
  if (!path || !*path) return;
  if (!dladdr((void *)emit_trace_node, &info) || !info.dli_fbase) {
    fprintf(stderr, "spinel-trace: cannot find the image base\n");
    exit(1);
  }
  tr_base = (uintptr_t)info.dli_fbase;
  tr_line = line ? (uint32_t)strtoul(line, NULL, 10) : 0;
  tr_analysis = getenv("SPINEL_EMIT_TRACE_ANALYSIS") != NULL;
  if (tr_analysis && !tr_line) {
    fprintf(stderr, "spinel-trace: the analysis trace needs SPINEL_EMIT_TRACE_LINE\n");
    exit(1);
  }
  tr_out = fopen(path, "wb");
  if (!tr_out) { perror(path); exit(1); }
  setvbuf(tr_out, NULL, _IOFBF, 1 << 20);
  tr_put_fn(TR_HEAD, (void *)emit_trace_node);
  if (tr_analysis) tr_end(1);
}

/* The marker at the top of emit_call_body, emit_stmt and emit_stmt_tail: the
   functions entered from here until the caller returns emitted this node.
   The node is read before anything is written, so the accessors' own
   records do not land inside the marker. With a line asked for, a marker on
   it turns the recording on: the count starts at 1, the caller's own frame,
   whose exit brings it back to 0. */
void emit_trace_node(Compiler *c, int id, int kind) {
  if (!tr_asked) tr_open();
  if (!tr_out || tr_analysis) return;
  tr_on = 1;
  const char *file = nt_file_path(c->nt, (int)nt_int(c->nt, id, "node_file", 0));
  uint32_t required = file && *file && c->nt->source_file && !sp_streq(file, c->nt->source_file);
  uint32_t line = (uint32_t)nt_int(c->nt, id, "node_line", 0), col = (uint32_t)nt_int(c->nt, id, "node_col", 0);
  const char *label = kind == 'c' ? nt_str(c->nt, id, "name") : nt_type(c->nt, id);
  if (tr_line && !tr_depth) {
    if (line != tr_line || required) return;
    tr_depth = 1;
  }
  tr_put(TR_NODE | (uint32_t)(kind & 0xff) << 8 | required << 16, (uint32_t)id, line, col);
  tr_text(label, 3);
}

/* ---- the analysis trace ---- */

/* Is the node in a required file (1), and is it on the asked line of the
   program's own (2)? */
static TR_HOOK int tr_where(Compiler *c, int id) {
  if ((uint32_t)nt_int(c->nt, id, "node_line", 0) != tr_line) return 0;   /* (most nodes: one scan) */
  const char *file = nt_file_path(c->nt, (int)nt_int(c->nt, id, "node_file", 0));
  return file && *file && c->nt->source_file && !sp_streq(file, c->nt->source_file) ? 1 : 2;
}

/* A node or fact record and its text. The label of a node is its call's
   name, or its node type. */
static TR_HOOK void tr_put_node(Compiler *c, uint32_t tag, int id, int kind, const char *text, int max) {
  const char *name = text ? text : nt_kind(c->nt, id) == NK_CallNode ? nt_str(c->nt, id, "name") : NULL;
  uint32_t line = (uint32_t)nt_int(c->nt, id, "node_line", 0), col = (uint32_t)nt_int(c->nt, id, "node_col", 0);
  uint32_t required = tr_line == line && tr_where(c, id) == 1;
  if (!name) name = nt_type(c->nt, id);
  tr_put(tag | (uint32_t)(kind & 0xff) << 8 | required << 16, (uint32_t)id, line, col);
  tr_text(name, max);
}

/* What a record outside a recording stands in: the open functions, and the
   pass (c->nt->pass, which the analysis driver keeps in every build) */
static TR_HOOK void tr_context(Compiler *c, int round, const char *walk) {
  char pass[5 * TR_TEXT_LEN];
  snprintf(pass, sizeof pass, "%s%s%s", c->nt->pass ? c->nt->pass : "", walk ? " " : "", walk ? walk : "");
  tr_put(TR_STACK, 0, 0, 0);
  for (int i = 0; i < tr_sp && i < TR_FRAMES; i++) tr_put_fn(TR_ENTER, (void *)tr_stack[i]);
  tr_put(TR_PASS, (uint32_t)round, 0, 0);
  tr_text(pass, 4);
}

/* The marker where a node's fact is about to be computed (infer_type before
   infer_uncached, nf_expr, cf_node): the functions entered from here until
   the caller returns compute it. An ask for a node of the line starts a
   recording; an ask inside one says whose computation the subtree is. A type
   is asked again in every round, so its recording is kept only once a type
   of the line changed in it (emit_trace_fact). */
void emit_trace_ask(Compiler *c, int id, int kind, int round, const char *walk) {
  if (!tr_out || !tr_analysis) return;
  if (!tr_rec) {
    if (tr_where(c, id) != 2) return;
    tr_context(c, round, walk);
    tr_rec = tr_sp;
    tr_keep = kind != 't';
  }
  tr_put_node(c, TR_NODE, id, kind, NULL, 3);
}

static TR_HOOK const char *tr_ty(Compiler *c, int t, char *buf, size_t n) {
  if (ty_is_object((TyKind)t) && ty_object_class((TyKind)t) < c->nclasses) return c->classes[ty_object_class((TyKind)t)].name;
  if (!ty_is_obj_array((TyKind)t)) return ty_name((TyKind)t);
  snprintf(buf, n, "obj_array of %s", tr_ty(c, (int)ty_array_elem((TyKind)t), buf + n / 2, n / 2));
  return buf;
}

/* The source of a nil, in the enum's order (analyze.h) */
static const char *const tr_nil_why[] = { "", "a nil written", "no else branch", "a `&.` call", "a read before a write",
  "an element of an Array holding nil", "a pop, shift or delete_at", "an element read that can miss", "a global",
  "an ivar not set first", "a caller the analysis does not see", "a value the analysis does not model", "" };
_Static_assert(sizeof tr_nil_why / sizeof *tr_nil_why == NFW_GUARDED + 1, "a nil source without its text");

/* The marker where a node's fact changed (comp_set_ntype, nf_expr's store).
   Outside a recording a fact of the line is a pass's own write: it goes out
   with the open functions. */
void emit_trace_fact(Compiler *c, int id, int kind, int was, int now) {
  if (!tr_out || !tr_analysis) return;
  int mine = tr_where(c, id) == 2, lone = !tr_rec;
  if (lone && !mine) return;
  char text[8 * TR_TEXT_LEN + 1], a[64], b[64];
  if (kind == 't') snprintf(text, sizeof text, "%s -> %s", tr_ty(c, was, a, sizeof a), tr_ty(c, now, b, sizeof b));
  else if ((now & 3) != NF_MAY_NIL) snprintf(text, sizeof text, (now & 3) == NF_GUARDED ? "not nil past a guard" : "not nil");
  else snprintf(text, sizeof text, "may be nil: %s", now >> 2 <= NFW_GUARDED ? tr_nil_why[now >> 2] : "?");
  if (lone) tr_context(c, kind == 't' ? g_infer_round : 0, NULL);
  tr_put_node(c, TR_FACT, id, kind, text, 8);
  if (lone) tr_end(1);
  else if (mine) tr_keep = 1;
}

/* The marker where a class walk has left its sets (cf_solve), and where the
   walks' sets have been met (an_cls_facts): the set of each node of the line */
void emit_trace_sets(Compiler *c, const char *walk) {
  if (!tr_out || !tr_analysis) return;
  for (int id = 0; id < c->nt->count; id++) {
    if (tr_where(c, id) != 2) continue;
    const ClsSet *s = cls_fact_node(c, id);
    char text[8 * TR_TEXT_LEN + 1];
    size_t n = 0;
    if (!s) n = (size_t)snprintf(text, sizeof text, "every class");
    else {
      for (int k = cls_set_next(s, -1); k >= 0 && n < sizeof text; k = cls_set_next(s, k))
        n += (size_t)snprintf(text + n, sizeof text - n, "%s%s", n ? ", " : "", k < c->nclasses ? c->classes[k].name : "?");
      if (n < sizeof text) n += (size_t)snprintf(text + n, sizeof text - n, "%s%s%s%s", n ? "" : "no class",
                                                 s->bv ? " +builtin" : "", s->os ? " +ostruct" : "", s->pr ? " +proc" : "");
    }
    if (n >= sizeof text) memcpy(text + sizeof text - 4, "...", 4);
    tr_context(c, 0, walk);
    tr_put_node(c, TR_FACT, id, 'k', text, 8);
    tr_end(1);
  }
}

/* Every call is recorded, or with a line asked for the calls inside a node
   of it. A longjmp out of such a node (a refusal the driver recovers from)
   leaves the count above 0, and the trace then says more than was asked.
   The analysis trace notes the function as open, and records it inside a
   recording. */
void __cyg_profile_func_enter(void *fn, void *site) {
  (void)site;
  if (!tr_asked) tr_open();
  if (!tr_out) return;
  if (tr_analysis) {
    if (tr_sp < TR_FRAMES) tr_stack[tr_sp] = (uintptr_t)fn;
    tr_sp++;
    if (tr_rec) tr_put_fn(TR_ENTER, fn);
    return;
  }
  if (!tr_on || (tr_line && !tr_depth)) return;
  tr_depth++;
  tr_put_fn(TR_ENTER, fn);
}

/* (the analysis trace) The open functions are popped down to this one's
   last entry: a longjmp leaves functions without their exit. The asking
   function's exit ends the recording. */
void __cyg_profile_func_exit(void *fn, void *site) {
  (void)site;
  if (!tr_out) return;
  if (tr_analysis) {
    int i = tr_sp;
    while (i > 0 && i <= TR_FRAMES && tr_stack[i - 1] != (uintptr_t)fn) i--;
    if (!i) return;
    tr_sp = i - 1;
    if (!tr_rec) return;
    tr_put_fn(TR_EXIT, fn);
    if (tr_sp < tr_rec) tr_end(tr_keep);
    return;
  }
  if (!tr_on || (tr_line && !tr_depth)) return;
  tr_depth--;
  tr_put_fn(TR_EXIT, fn);
}
