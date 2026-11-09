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
   Offsets, not addresses, so two runs write the same bytes under address
   randomization. Names are not resolved here: a static function is not in the
   dynamic symbol table on Linux, so the reader maps the offsets with `nm`.

   Recording starts at the first node marker, which the code generator writes:
   the parse and the analysis run untraced, or one mid-size program's trace
   would be gigabytes. With the variable unset nothing is opened and the hooks
   return at once. SPINEL_EMIT_TRACE_LINE=N narrows it further, to the calls
   and statements on line N of the program's own file and what they call:
   megabytes become kilobytes, and the reader has asked for no more.

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

#define TR_HOOK __attribute__((no_instrument_function))

enum { TR_HEAD = 1, TR_NODE = 2, TR_ENTER = 3, TR_EXIT = 4, TR_TEXT = 5, TR_TEXT_LEN = 12 };
typedef struct { uint32_t tag; union { uint32_t v[3]; char text[TR_TEXT_LEN]; } u; } TraceRec;

static FILE *tr_out;        /* the open trace, NULL while nothing is recorded */
static int tr_asked;        /* SPINEL_EMIT_TRACE has been looked at */
static uintptr_t tr_base;   /* where the compiler's image is loaded */
static uint32_t tr_line;    /* SPINEL_EMIT_TRACE_LINE, or 0 for every line */
static int tr_depth;        /* with a line: the frames open inside a node of it, 0 outside one */

void emit_trace_node(Compiler *c, int id, int kind) TR_HOOK;
void __cyg_profile_func_enter(void *fn, void *site) TR_HOOK;
void __cyg_profile_func_exit(void *fn, void *site) TR_HOOK;

static TR_HOOK void tr_put(uint32_t tag, uint32_t v0, uint32_t v1, uint32_t v2) {
  TraceRec r = { tag, { { v0, v1, v2 } } };
  fwrite(&r, sizeof r, 1, tr_out);
}

static TR_HOOK void tr_put_fn(uint32_t tag, void *fn) {
  uint64_t off = (uintptr_t)fn - tr_base;
  tr_put(tag, (uint32_t)off, (uint32_t)(off >> 32), 0);
}

/* Open the trace on the first marker. stdio buffers the records and writes
   what is left at exit, so a refusal's exit(1) keeps the trace up to it. */
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
  tr_out = fopen(path, "wb");
  if (!tr_out) { perror(path); exit(1); }
  setvbuf(tr_out, NULL, _IOFBF, 1 << 20);
  tr_put_fn(TR_HEAD, (void *)emit_trace_node);
}

/* The marker at the top of emit_call_body, emit_stmt and emit_stmt_tail: the
   functions entered from here until the caller returns emitted this node. The node is
   read before anything is written, so the accessors' own records do not land
   inside the marker. With a line asked for, a marker on it turns the
   recording on: the count starts at 1, the caller's own frame, whose exit
   brings it back to 0. */
void emit_trace_node(Compiler *c, int id, int kind) {
  if (!tr_asked) tr_open();
  if (!tr_out) return;
  const char *file = nt_file_path(c->nt, (int)nt_int(c->nt, id, "node_file", 0));
  uint32_t required = file && *file && c->nt->source_file && !sp_streq(file, c->nt->source_file);
  uint32_t line = (uint32_t)nt_int(c->nt, id, "node_line", 0), col = (uint32_t)nt_int(c->nt, id, "node_col", 0);
  const char *label = kind == 'c' ? nt_str(c->nt, id, "name") : nt_type(c->nt, id);
  size_t len = label ? strlen(label) : 0;
  if (tr_line && !tr_depth) {
    if (line != tr_line || required) return;
    tr_depth = 1;
  }
  tr_put(TR_NODE | (uint32_t)(kind & 0xff) << 8 | required << 16, (uint32_t)id, line, col);
  for (size_t i = 0; i < len && i < 3 * TR_TEXT_LEN; i += TR_TEXT_LEN) {
    TraceRec r = { TR_TEXT, { { 0, 0, 0 } } };
    memcpy(r.u.text, label + i, len - i < TR_TEXT_LEN ? len - i : TR_TEXT_LEN);
    fwrite(&r, sizeof r, 1, tr_out);
  }
}

/* Every call is recorded, or with a line asked for the calls inside a node
   of it. A longjmp out of such a node (a refusal the driver recovers from)
   leaves the count above 0, and the trace then says more than was asked. */
void __cyg_profile_func_enter(void *fn, void *site) {
  (void)site;
  if (!tr_out || (tr_line && !tr_depth)) return;
  tr_depth++;
  tr_put_fn(TR_ENTER, fn);
}

void __cyg_profile_func_exit(void *fn, void *site) {
  (void)site;
  if (!tr_out || (tr_line && !tr_depth)) return;
  tr_depth--;
  tr_put_fn(TR_EXIT, fn);
}
